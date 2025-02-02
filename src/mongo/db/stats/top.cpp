// top.cpp
/*
 *    Copyright (C) 2010 10gen Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */


#include <cstddef>
#include <cstdint>
#include <mutex>
#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kDefault

#include "mongo/platform/basic.h"

#include "mongo/db/jsobj.h"
#include "mongo/db/service_context.h"
#include "mongo/db/stats/top.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/log.h"

namespace mongo {

using std::endl;
using std::string;
using std::stringstream;
using std::vector;

extern thread_local int16_t localThreadId;

namespace {

const auto getTop = ServiceContext::declareDecoration<Top>();

}  // namespace

Top::UsageData::UsageData(const UsageData& older, const UsageData& newer) {
    // this won't be 100% accurate on rollovers and drop(), but at least it won't be negative
    time = (newer.time >= older.time) ? (newer.time - older.time) : newer.time;
    count = (newer.count >= older.count) ? (newer.count - older.count) : newer.count;
}

Top::CollectionData::CollectionData(const CollectionData& older, const CollectionData& newer)
    : total(older.total, newer.total),
      readLock(older.readLock, newer.readLock),
      writeLock(older.writeLock, newer.writeLock),
      queries(older.queries, newer.queries),
      getmore(older.getmore, newer.getmore),
      insert(older.insert, newer.insert),
      update(older.update, newer.update),
      remove(older.remove, newer.remove),
      commands(older.commands, newer.commands) {}

// static
Top& Top::get(ServiceContext* service) {
    return getTop(service);
}

void Top::record(OperationContext* opCtx,
                 StringData ns,
                 LogicalOp logicalOp,
                 LockType lockType,
                 long long micros,
                 bool command,
                 Command::ReadWriteType readWriteType) {
    if (ns[0] == '?') {
        return;
    }

    auto hashedNs = UsageMap::HashedKey(ns);


    if ((command || logicalOp == LogicalOp::opQuery) && ns == _lastDropped) {
        std::scoped_lock<std::mutex> lock(_lastDroppedMutex);
        _lastDropped = "";
        return;
    }

    int16_t id = localThreadId + 1;

    std::scoped_lock<std::mutex> lock(_usageMutexVector[id]);
    CollectionData& coll = _usageVector[id][hashedNs];
    _record(opCtx, coll, logicalOp, lockType, micros, readWriteType);
}

void Top::_record(OperationContext* opCtx,
                  CollectionData& c,
                  LogicalOp logicalOp,
                  LockType lockType,
                  uint64_t micros,
                  Command::ReadWriteType readWriteType) {

    _incrementHistogram(opCtx, micros, &c.opLatencyHistogram, readWriteType);

    c.total.inc(micros);

    if (lockType == LockType::WriteLocked)
        c.writeLock.inc(micros);
    else if (lockType == LockType::ReadLocked)
        c.readLock.inc(micros);

    switch (logicalOp) {
        case LogicalOp::opInvalid:
            // use 0 for unknown, non-specific
            break;
        case LogicalOp::opUpdate:
            c.update.inc(micros);
            break;
        case LogicalOp::opInsert:
            c.insert.inc(micros);
            break;
        case LogicalOp::opQuery:
            c.queries.inc(micros);
            break;
        case LogicalOp::opGetMore:
            c.getmore.inc(micros);
            break;
        case LogicalOp::opDelete:
            c.remove.inc(micros);
            break;
        case LogicalOp::opKillCursors:
            break;
        case LogicalOp::opCommand:
            c.commands.inc(micros);
            break;
        default:
            MONGO_UNREACHABLE;
    }
}

void Top::collectionDropped(StringData ns, bool databaseDropped) {
    for (size_t i = 0; i < _usageVector.size(); ++i) {
        std::scoped_lock<std::mutex> lock(_usageMutexVector[i]);
        _usageVector[i].erase(ns);
    }

    if (!databaseDropped) {
        // If a collection drop occurred, there will be a subsequent call to record for this
        // collection namespace which must be ignored. This does not apply to a database drop.
        std::scoped_lock<std::mutex> lock(_lastDroppedMutex);
        _lastDropped = ns.toString();
    }
}

void Top::cloneMap(Top::UsageMap& out) const {
    // stdx::lock_guard<SimpleMutex> lk(_lock);
    // out = _usage;
    MONGO_UNREACHABLE;
}

void Top::append(BSONObjBuilder& b) {
    // stdx::lock_guard<SimpleMutex> lk(_lock);
    UsageMap all = _mergeUsageVector();
    _appendToUsageMap(b, all);
}

void Top::_appendToUsageMap(BSONObjBuilder& b, const UsageMap& map) const {
    // pull all the names into a vector so we can sort them for the user

    vector<string> names;
    for (UsageMap::const_iterator i = map.begin(); i != map.end(); ++i) {
        names.push_back(i->first);
    }

    std::sort(names.begin(), names.end());

    for (size_t i = 0; i < names.size(); i++) {
        BSONObjBuilder bb(b.subobjStart(names[i]));

        const CollectionData& coll = map.find(names[i])->second;

        _appendStatsEntry(b, "total", coll.total);

        _appendStatsEntry(b, "readLock", coll.readLock);
        _appendStatsEntry(b, "writeLock", coll.writeLock);

        _appendStatsEntry(b, "queries", coll.queries);
        _appendStatsEntry(b, "getmore", coll.getmore);
        _appendStatsEntry(b, "insert", coll.insert);
        _appendStatsEntry(b, "update", coll.update);
        _appendStatsEntry(b, "remove", coll.remove);
        _appendStatsEntry(b, "commands", coll.commands);

        bb.done();
    }
}

void Top::_appendStatsEntry(BSONObjBuilder& b, const char* statsName, const UsageData& map) const {
    BSONObjBuilder bb(b.subobjStart(statsName));
    bb.appendNumber("time", map.time);
    bb.appendNumber("count", map.count);
    bb.done();
}

void Top::appendLatencyStats(StringData ns, bool includeHistograms, BSONObjBuilder* builder) {
    auto hashedNs = UsageMap::HashedKey(ns);
    // stdx::lock_guard<SimpleMutex> lk(_lock);
    BSONObjBuilder latencyStatsBuilder;
    UsageMap all = _mergeUsageVector();
    all[hashedNs].opLatencyHistogram.append(includeHistograms, &latencyStatsBuilder);
    builder->append("ns", ns);
    builder->append("latencyStats", latencyStatsBuilder.obj());
}

void Top::incrementGlobalLatencyStats(OperationContext* opCtx,
                                      uint64_t latency,
                                      Command::ReadWriteType readWriteType) {
    // stdx::lock_guard<SimpleMutex> guard(_lock);
    // MONGO_UNREACHABLE;
    int16_t id = localThreadId + 1;
    std::scoped_lock<std::mutex> lk(_histogramMutexVector[id]);
    _incrementHistogram(opCtx, latency, &_histogramVector[id], readWriteType);
}

void Top::appendGlobalLatencyStats(bool includeHistograms, BSONObjBuilder* builder) {
    OperationLatencyHistogram globalHistogramStats;

    for (size_t i = 0; i < _histogramVector.size(); ++i) {
        std::scoped_lock<std::mutex> lock(_histogramMutexVector[i]);
        globalHistogramStats += _histogramVector[i];
    }

    globalHistogramStats.append(includeHistograms, builder);
}

void Top::incrementGlobalTransactionLatencyStats(uint64_t latency) {
    // stdx::lock_guard<SimpleMutex> guard(_lock);
    // MONGO_UNREACHABLE;
    int16_t id = localThreadId + 1;
    std::scoped_lock<std::mutex> lk(_histogramMutexVector[id]);
    _histogramVector[id].increment(latency, Command::ReadWriteType::kTransaction);
}

void Top::_incrementHistogram(OperationContext* opCtx,
                              uint64_t latency,
                              OperationLatencyHistogram* histogram,
                              Command::ReadWriteType readWriteType) {
    // Only update histogram if operation came from a user.
    Client* client = opCtx->getClient();
    if (client->isFromUserConnection() && !client->isInDirectClient()) {
        histogram->increment(latency, readWriteType);
    }
}

Top::UsageMap Top::_mergeUsageVector() {
    UsageMap all;
    for (size_t i = 0; i < _usageVector.size(); ++i) {
        std::scoped_lock<std::mutex> lock(_usageMutexVector[i]);
        for (const auto& [k, v] : _usageVector[i]) {
            if (all.find(k) == all.end()) {
                all[k] = v;
            } else {
                all[k] += v;
            }
        }
    }

    return all;
}
}  // namespace mongo

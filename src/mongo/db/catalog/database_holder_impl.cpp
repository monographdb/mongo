/**
 *    Copyright (C) 2017 MongoDB Inc.
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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kStorage

#include "mongo/platform/basic.h"

#include "mongo/base/string_data.h"
#include "mongo/db/catalog/database_holder_impl.h"


#include "mongo/base/init.h"
#include "mongo/db/audit.h"
#include "mongo/db/background.h"
#include "mongo/db/catalog/database.h"
#include "mongo/db/catalog/database_catalog_entry.h"
#include "mongo/db/catalog/namespace_uuid_cache.h"
#include "mongo/db/catalog/uuid_catalog.h"
#include "mongo/db/client.h"
#include "mongo/db/clientcursor.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/service_context.h"
#include "mongo/db/storage/storage_engine.h"
#include "mongo/util/log.h"
#include "mongo/util/scopeguard.h"

namespace mongo {
extern thread_local uint16_t localThreadId;

namespace {

const auto dbHolderStorage = ServiceContext::declareDecoration<DatabaseHolder>();

}  // namespace

MONGO_REGISTER_SHIM(DatabaseHolder::getDatabaseHolder)
()->DatabaseHolder& {
    return dbHolderStorage(getGlobalServiceContext());
}

MONGO_REGISTER_SHIM(DatabaseHolder::makeImpl)
(PrivateTo<DatabaseHolder>)->std::unique_ptr<DatabaseHolder::Impl> {
    return std::make_unique<DatabaseHolderImpl>();
}

using std::set;
using std::size_t;
using std::string;
using std::stringstream;

namespace {

StringData _todb(StringData ns) {
    size_t i = ns.find('.');
    if (i == std::string::npos) {
        uassert(13074, "db name can't be empty", ns.size());
        return ns;
    }

    uassert(13075, "db name can't be empty", i > 0);

    const StringData d = ns.substr(0, i);
    uassert(13280,
            "invalid db name: " + ns.toString(),
            NamespaceString::validDBName(d, NamespaceString::DollarInDbNameBehavior::Allow));

    return d;
}

}  // namespace


// thread_local bool DatabaseHolderImpl::registered{false};
// thread_local std::atomic<bool> DatabaseHolderImpl::localReadLock{false};

// class ReadLock {
// public:
//     MONGO_DISALLOW_COPYING(ReadLock);
//     ReadLock() {
//         bool excepted = false;
//         while (!DatabaseHolderImpl::localReadLock.compare_exchange_strong(excepted, true)) {
//             excepted = false;
//             mongo::localThreadId=
//         }
//     }
//     ~ReadLock() {
//         DatabaseHolderImpl::localReadLock.store(false);
//     }
// };

// class WriteLock {
// public:
//     MONGO_DISALLOW_COPYING(WriteLock);
//     explicit WriteLock(DatabaseHolderImpl* impl) : _impl{impl} {
//         for (auto& rlk : _impl->_localReadLockVector) {
//             bool excepted = false;
//             while (!rlk->compare_exchange_strong(excepted, true)) {
//                 excepted = false;
//             }
//         }
//     }
//     ~WriteLock() {
//         for (auto& rlk : _impl->_localReadLockVector) {
//             rlk->store(false);
//         }
//     }

// private:
//     DatabaseHolderImpl* _impl;
// };

/*
 * RAII style warpper for SimpleSpinlock
 */
class ThreadLocalLock {
public:
    explicit ThreadLocalLock(txservice::SimpleSpinlock& lock) : _lock(lock) {
        _lock.Lock();
    }
    ~ThreadLocalLock() {
        _lock.Unlock();
    }
    txservice::SimpleSpinlock& _lock;
};
class WriteLock {
public:
    explicit WriteLock(DatabaseHolderImpl* impl) : _impl(impl) {
        for (auto& lk : _impl->_lockVector) {
            lk.Lock();
        }
    }

    ~WriteLock() {
        for (auto& lk : _impl->_lockVector) {
            lk.Unlock();
        }
    }
    DatabaseHolderImpl* _impl;
};


Database* DatabaseHolderImpl::get(OperationContext* opCtx, StringData ns) const {
    // _registerReadLock();
    const StringData db = _todb(ns);
    invariant(opCtx->lockState()->isDbLockedForMode(db, MODE_IS));

    // stdx::lock_guard<SimpleMutex> lk(_m);

    // ReadLock rlk_;
    const auto& localDBCache = _dbCaches[localThreadId];
    ThreadLocalLock rlk(_lockVector[localThreadId]);
    DBCache::const_iterator it = localDBCache.find(db);
    if (it != localDBCache.end()) {
        return it->second;
    }

    return NULL;
}

std::set<std::string> DatabaseHolderImpl::_getNamesWithConflictingCasing_inlock(StringData name) {
    std::set<std::string> duplicates;
    const auto& dbCache = _dbCaches[localThreadId];
    for (const auto& nameAndPointer : dbCache) {
        // A name that's equal with case-insensitive match must be identical, or it's a duplicate.
        if (name.equalCaseInsensitive(nameAndPointer.first) && name != nameAndPointer.first) {
            duplicates.insert(nameAndPointer.first);
        }
    }
    return duplicates;
}

std::set<std::string> DatabaseHolderImpl::getNamesWithConflictingCasing(StringData name) {
    // stdx::lock_guard<SimpleMutex> lk(_m);
    // _registerReadLock();
    ThreadLocalLock rlk(_lockVector[localThreadId]);
    return _getNamesWithConflictingCasing_inlock(name);
}

Database* DatabaseHolderImpl::openDb(OperationContext* opCtx, StringData ns, bool* justCreated) {
    // _registerReadLock();
    const StringData dbname = _todb(ns);
    invariant(opCtx->lockState()->isDbLockedForMode(dbname, MODE_X));

    if (justCreated)
        *justCreated = false;  // Until proven otherwise.

    // stdx::unique_lock<SimpleMutex> lk(_m);
    // std::unique_lock<std::mutex> lk(_localReadLockVectorMutex);
    MONGO_LOG(0) << "localThreadId: " << localThreadId;
    MONGO_LOG(0) << "_dbCache size: " << _dbCaches.size();
    auto& localDbCache = _dbCaches[localThreadId];
    {
        ThreadLocalLock rlk(_lockVector[localThreadId]);

        // The following will insert a nullptr for dbname, which will treated the same as a non-
        // existant database by the get method, yet still counts in getNamesWithConflictingCasing.
        if (auto db = localDbCache[dbname]) {
            return db;
        }

        // Check casing in lock to avoid transient duplicates.
        auto duplicates = _getNamesWithConflictingCasing_inlock(dbname);
        uassert(ErrorCodes::DatabaseDifferCase,
                str::stream() << "db already exists with different case already have: ["
                              << *duplicates.cbegin() << "] trying to create [" << dbname.toString()
                              << "]",
                duplicates.empty());


        // Do the catalog lookup and database creation outside of the scoped lock, because these may
        // block. Only one thread can be inside this method for the same DB name, because of the
        // requirement for X-lock on the database when we enter. So there is no way we can insert
        // two different databases for the same name.
    }
    // We've inserted a nullptr entry for dbname: make sure to remove it on unsuccessful exit.
    auto removeDbGuard = MakeGuard([&localDbCache, this, dbname] {
        // if (!lk.owns_lock())
        //     lk.lock();
        ThreadLocalLock rlk(_lockVector[localThreadId]);
        localDbCache.erase(dbname);
    });
    StorageEngine* storageEngine = getGlobalServiceContext()->getStorageEngine();
    DatabaseCatalogEntry* entry = storageEngine->getDatabaseCatalogEntry(opCtx, dbname);

    if (!entry->exists()) {
        audit::logCreateDatabase(&cc(), dbname);
        if (justCreated)
            *justCreated = true;
    }

    // Finally replace our nullptr entry with the new Database pointer.
    // auto newDb = stdx::make_unique<Database>(opCtx, dbname, entry);
    auto newDbPtr = new Database(opCtx, dbname, entry);

    removeDbGuard.Dismiss();

    {
        WriteLock wlk(this);
        for (auto &dbCache : _dbCaches) {
            dbCache[dbname] = newDbPtr;
            // auto it = dbCache.find(dbname);
            // dassert(it != _dbs.end() && it->second == nullptr);
            // if (it->second == nullptr) {
            //     it->second = newDbPtr;
            // }
        }
    }

    DEV {
        ThreadLocalLock rlk(_lockVector[localThreadId]);
        invariant(_getNamesWithConflictingCasing_inlock(dbname).empty());
    }

    return newDbPtr;
}

namespace {
void evictDatabaseFromUUIDCatalog(OperationContext* opCtx, Database* db) {
    UUIDCatalog::get(opCtx).onCloseDatabase(db);
    for (auto&& coll : *db) {
        NamespaceUUIDCache::get(opCtx).evictNamespace(coll->ns());
    }
}
}  // namespace

void DatabaseHolderImpl::close(OperationContext* opCtx, StringData ns, const std::string& reason) {
    // _registerReadLock();
    invariant(opCtx->lockState()->isW());

    const StringData dbName = _todb(ns);

    // stdx::lock_guard<SimpleMutex> lk(_m);
    {
        // std::unique_lock<std::mutex> lk(_localReadLockVectorMutex);
        WriteLock wlk(this);
        const auto& dbCache = _dbCaches[localThreadId];
        DBCache::const_iterator it = dbCache.find(dbName);
        if (it == dbCache.end()) {
            return;
        }

        auto db = it->second;
        repl::oplogCheckCloseDatabase(opCtx, db);
        evictDatabaseFromUUIDCatalog(opCtx, db);

        // only close and delete db pointer once
        db->close(opCtx, reason);
        delete db;
        db = nullptr;

        // but erase on all maps
        for (auto& dbCache : _dbCaches) {
            dbCache.erase(dbName);
        }
    }

    getGlobalServiceContext()
        ->getStorageEngine()
        ->closeDatabase(opCtx, dbName)
        .transitional_ignore();
}

void DatabaseHolderImpl::closeAll(OperationContext* opCtx, const std::string& reason) {
    // _registerReadLock();
    invariant(opCtx->lockState()->isW());

    // stdx::lock_guard<SimpleMutex> lk(_m);
    // std::unique_lock<std::mutex> lk(_localReadLockVectorMutex);
    WriteLock wlk(this);

    auto& dbCache = _dbCaches[localThreadId];
    for (auto& [name, db] : dbCache) {
        BackgroundOperation::assertNoBgOpInProgForDb(name);
        LOG(2) << "DatabaseHolder::closeAll name:" << name;
        repl::oplogCheckCloseDatabase(opCtx, db);
        evictDatabaseFromUUIDCatalog(opCtx, db);
        db->close(opCtx, reason);
        delete db;

        getGlobalServiceContext()
            ->getStorageEngine()
            ->closeDatabase(opCtx, name)
            .transitional_ignore();
    }
    _dbCaches.clear();
}

// void DatabaseHolderImpl::_registerReadLock() const {
//     if (!registered) {
//         registered = true;
//         std::lock_guard<std::mutex> lk(_localReadLockVectorMutex);
//         _localReadLockVector.emplace_back(&localReadLock);
//     }
// }

// void DatabaseHolderImpl::_lockLocalReadLock() const {
//     bool excepted = false;
//     while (!localReadLock.compare_exchange_strong(excepted, true)) {
//         excepted = false;
//     }
// }

// void DatabaseHolderImpl::_unlockLocalReadLock() const {
//     localReadLock.store(false);
// }

// void DatabaseHolderImpl::_lockWriteLock() {
//     for (auto& rlk : _localReadLockVector) {
//         bool excepted = false;
//         while (!rlk->compare_exchange_strong(excepted, true)) {
//             excepted = false;
//         }
//     }
// }

// void DatabaseHolderImpl::_unlockWriteLock() {
//     for (auto& rlk : _localReadLockVector) {
//         rlk->store(false);
//     }
// }

}  // namespace mongo

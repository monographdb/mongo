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

#pragma once

#include <memory>
#include <unordered_map>
#include <vector>

#include "mongo/base/string_data.h"
#include "mongo/db/catalog/database_holder.h"
#include "mongo/db/server_options.h"
#include "mongo/util/string_map.h"

namespace mongo {

class Database;
class OperationContext;

/**
 * Registry of opened databases.
 */
class DatabaseHolderImpl : public DatabaseHolder::Impl {
public:
    DatabaseHolderImpl() = default;

    /**
     * Retrieves an already opened database or returns NULL. Must be called with the database
     * locked in at least IS-mode.
     */
    Database* get(OperationContext* opCtx, StringData ns)  override;

    /**
     * Retrieves a database reference if it is already opened, or opens it if it hasn't been
     * opened/created yet. Must be called with the database locked in X-mode.
     *
     * @param justCreated Returns whether the database was newly created (true) or it already
     *          existed (false). Can be NULL if this information is not necessary.
     */
    Database* openDb(OperationContext* opCtx, StringData ns, bool* justCreated = nullptr) override;

    /**
     * Closes the specified database. Must be called with the database locked in X-mode.
     * No background jobs must be in progress on the database when this function is called.
     */
    void close(OperationContext* opCtx, StringData ns, const std::string& reason) override;

    /**
     * Closes all opened databases. Must be called with the global lock acquired in X-mode.
     * Will uassert if any background jobs are running when this function is called.
     *
     * @param reason The reason for close.
     */
    void closeAll(OperationContext* opCtx, const std::string& reason) override;

    /**
     * Returns the set of existing database names that differ only in casing.
     */
    std::set<std::string> getNamesWithConflictingCasing(StringData name) override;

private:
    std::set<std::string> _getNamesWithConflictingCasing_inlock(StringData name) const;
    using DBMap = std::map<std::string, std::unique_ptr<Database>, std::less<void>>;

    // mutable std::vector<std::mutex> _dbMapMutexVector{serverGlobalParams.reservedThreadNum + 1};
    std::vector<DBMap> _dbMapVector{
        serverGlobalParams.reservedThreadNum +
        1};  // the first is used for other threads while the rest is used for thread group
};
}  // namespace mongo

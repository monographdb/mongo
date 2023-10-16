#pragma once

#include <vector>

#include "mongo/base/disallow_copying.h"
#include "mongo/db/modules/monograph/tx_service/include/spinlock.h"

namespace mongo {
class ReadLock {
public:
    MONGO_DISALLOW_COPYING(ReadLock);
    explicit ReadLock(txservice::SimpleSpinlock& lock) : _lock(lock) {
        _lock.Lock();
    }
    ~ReadLock() {
        _lock.Unlock();
    }
    txservice::SimpleSpinlock& _lock;
};

class WriteLock {
public:
    MONGO_DISALLOW_COPYING(WriteLock);
    explicit WriteLock(std::vector<txservice::SimpleSpinlock>& lockVector)
        : _lockVector(lockVector) {
        for (auto& lk : _lockVector) {
            lk.Lock();
        }
    }

    ~WriteLock() {
        for (auto& lk : _lockVector) {
            lk.Unlock();
        }
    }
    std::vector<txservice::SimpleSpinlock>& _lockVector;
};
}  // namespace mongo
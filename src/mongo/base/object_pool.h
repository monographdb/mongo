#pragma once

#include <array>
#include <functional>
#include <memory>
#include <queue>
#include <stack>
#include <utility>
#include <vector>

#include "mongo/db/server_options.h"

/*
 * https://stackoverflow.com/questions/64758775/result-type-must-be-constructible-from-value-type-of-input-range-when-trying-t
 */
namespace mongo {
extern thread_local uint16_t localThreadId;

template <typename T>
class ObjectPool {
    using Deleter = std::function<void(T*)>;

public:
    explicit ObjectPool() {
        size_t threadNum = serverGlobalParams.reservedThreadNum;
        // for (size_t i = 0; i < threadNum; ++i) {
        //     std::queue<std::unique_ptr<T>> q;
        //     // for (size_t i = 0; i < kDefaultCapacity; ++i) {
        //     //     q.emplace(std::make_unique<T>());
        //     // }
        //     pool.emplace_back(q);
        // }
    }

    template <typename... Args>
    std::unique_ptr<T,Deleter> newObject(Args&&... args) {
        auto& localPool = pool[localThreadId];
        std::unique_ptr<T> uptr{nullptr};

        if (localPool.empty()) {
            uptr = std::make_unique<T>(std::forward<Args>(args)...);
        } else {
            uptr = std::move(localPool.front());
            localPool.pop();
        }
        return std::unique_ptr<T>(uptr.release()/*,
                                  [&](T* ptr) { localPool.emplace(std::unique_ptr<T>(ptr)); }*/);
    }

private:
    static constexpr size_t kDefaultCapacity{32};
    static constexpr size_t kMaxThreadNum{100};
    std::array<std::queue<std::unique_ptr<T>>, kMaxThreadNum> pool;
};
}  // namespace mongo
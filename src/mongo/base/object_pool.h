#pragma once

// #include <glog/logging.h>

#include <array>
#include <functional>
#include <list>
#include <memory>
#include <queue>
#include <utility>
#include <vector>


namespace mongo {

extern thread_local uint16_t localThreadId;


template <typename T>
class ObjectPool {
public:
    // consider using funtion pointer
    using Deleter = std::function<void(T*)>;

    ObjectPool(const ObjectPool&) = delete;
    ObjectPool(ObjectPool&&) = delete;
    ~ObjectPool() = default;

    // inline static ObjectPool<T>& getInstance() {
    //     static ObjectPool<T> _instance;
    //     return _instance;
    // }

    template <typename... Args>
    static std::unique_ptr<T, Deleter> newObject(Args&&... args) {
        static ObjectPool<T> _instance;

        auto& localPool = _instance._pool[localThreadId];
        std::unique_ptr<T> uptr{nullptr};

        if (localPool.empty()) {
            // LOG(INFO) << "allocate";
            uptr = std::make_unique<T>(std::forward<Args>(args)...);
        } else {
            // LOG(INFO) << "reuse";
            uptr = std::move(localPool.front());
            localPool.pop();
            uptr->reset(std::forward<Args>(args)...);
        }
        return std::unique_ptr<T, Deleter>(uptr.release(), [&](T* ptr) {
            // recycle
            localPool.push(std::unique_ptr<T>(ptr));
        });
    }

private:
    explicit ObjectPool() {
        //     size_t threadNum = serverGlobalParams.reservedThreadNum;
        //     for (size_t i = 0; i < threadNum; ++i) {
        //         std::queue<std::unique_ptr<T>> q;
        //         // for (size_t i = 0; i < kDefaultCapacity; ++i) {
        //         //     q.emplace(std::make_unique<T>());
        //         // }
        //         _pool.emplace_back(q);
        //     }
    }


private:
    static constexpr size_t kDefaultCapacity{32};
    static constexpr size_t kMaxThreadNum{64};

    /*
     * Why use list as container for queue
     * https://stackoverflow.com/questions/65140603/how-to-resize-a-stdvectorstdqueuestdunique-ptrint
     */
    std::array<std::queue<std::unique_ptr<T>, std::list<std::unique_ptr<T>>>, kMaxThreadNum> _pool;
};
}  // namespace mongo
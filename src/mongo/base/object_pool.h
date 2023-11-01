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
    ObjectPool(const ObjectPool&) = delete;
    ObjectPool(ObjectPool&&) = delete;
    ~ObjectPool() = default;

    class Deleter {
    public:
        void operator()(T* ptr) {
            auto& localPool = getInstance()._pool[localThreadId];
            localPool.push(std::unique_ptr<T>(ptr));
        }
    };

    /*
      Implicitly convert type for classes which need a polymorphism Deleter.
      For example, RecoveryUnit and MonographRecoveryUnit
    */
    template <typename Base>
    static void PolyDeleter(Base* ptr) {
        auto& localPool = getInstance()._pool[localThreadId];
        localPool.push(std::unique_ptr<T>(static_cast<T*>(ptr)));
    }

    inline static ObjectPool<T>& getInstance() {
        static ObjectPool<T> _instance;
        return _instance;
    }

    template <typename... Args>
    static std::unique_ptr<T, Deleter> newObject(Args&&... args) {
        auto& localPool = getInstance()._pool[localThreadId];
        std::unique_ptr<T> uptr{nullptr};

        if (localPool.empty()) {
            uptr = std::make_unique<T>(std::forward<Args>(args)...);
        } else {
            uptr = std::move(localPool.front());
            localPool.pop();
            uptr->reset(std::forward<Args>(args)...);
        }
        return std::unique_ptr<T, Deleter>(uptr.release());
    }

    template <typename Base, typename... Args>
    static std::unique_ptr<Base, void (*)(Base*)> newObject(Args&&... args) {
        auto& localPool = getInstance()._pool[localThreadId];
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
        return std::unique_ptr<Base, void (*)(Base*)>(uptr.release(), &PolyDeleter<Base>);
    }

    /*
      Some Mongo classes have owned custom deleter.
      We could modify their custom to reuse Object
      instead of returning std::unique_ptr<T, Deleter> when creation.
      For example, PlanExecutor
    */
    template <typename... Args>
    static std::unique_ptr<T> newObjectDefaultDeleter(Args&&... args) {
        auto& localPool = getInstance()._pool[localThreadId];
        std::unique_ptr<T> uptr{nullptr};

        if (localPool.empty()) {
            uptr = std::make_unique<T>(std::forward<Args>(args)...);
        } else {
            uptr = std::move(localPool.front());
            localPool.pop();
            uptr->reset(std::forward<Args>(args)...);
        }
        return std::unique_ptr<T>(uptr.release());
    }

    template <typename... Args>
    static T* newObjectRawPointer(Args&&... args) {
        auto& localPool = getInstance()._pool[localThreadId];
        std::unique_ptr<T> uptr{nullptr};

        if (localPool.empty()) {
            uptr = std::make_unique<T>(std::forward<Args>(args)...);
        } else {
            uptr = std::move(localPool.front());
            localPool.pop();
            uptr->reset(std::forward<Args>(args)...);
        }
        return uptr.release();
    }

    /*
      Only class that allocate without std::unqiue_ptr<T, Deleter> provided by ObjectPool
      need call this function to recycle object manually.
    */
    static void recycleObject(T* ptr) {
        auto& localPool = getInstance()._pool[localThreadId];
        localPool.push(std::unique_ptr<T>(ptr));
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
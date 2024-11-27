#pragma once

#include "pool_allocator.h"

#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <list>

namespace threads {
    class RWSpinLock {
    public:
        inline static void readLock(std::atomic<int32_t> & lock) noexcept {
            auto v = lock.load(std::memory_order_acquire);
            while (v < 0 || !lock.compare_exchange_weak(v, v + 1, std::memory_order_acq_rel, std::memory_order_relaxed)) {
                v = lock.load(std::memory_order_acquire);
            }
        }
        inline static void readUnlock(std::atomic<int32_t> & lock) noexcept {
            lock.fetch_sub(1, std::memory_order_release);
        }

        inline static void writeLock(std::atomic<int32_t> & lock) noexcept {
            int32_t v = 0;
            while (!lock.compare_exchange_weak(v, -1, std::memory_order_acq_rel, std::memory_order_relaxed)) { 
                v = 0; // it will set it to what it currently held
            }
        }
        inline static void writeUnlock(std::atomic<int32_t> & lock) noexcept {
            lock.store(0, std::memory_order_release);
        }

        RWSpinLock(std::atomic<int32_t> & rwLock) : _rwLock(rwLock) {}
        ~RWSpinLock() { unlock(); }

        inline void readLock() noexcept {
            unlock();
            readLock(_rwLock);
            _state = State::read;
        }

        inline void writeLock() noexcept {
            unlock();
            writeLock(_rwLock);
            _state = State::write;
        }

        inline void unlock() noexcept {
            switch (_state) {
            case State::read:
                readUnlock(_rwLock);
                break;
            case State::write:
                writeUnlock(_rwLock);
                break;
            default:
                break;
            }

            _state = State::free;
        }

    private:
        enum class State : uint8_t {
            free = 0u, read = 1u, write = 2u
        };
        std::atomic<int32_t> & _rwLock;
        State _state = State::free;
    };
}

namespace allocators {

    template<size_t bytes, size_t align, bool thread_safe>
    class PoolAllocatorChain;

    struct PoolChainAllocationMetaInfo {
        void * pool = nullptr;
    };

    template<size_t size, size_t align>
    struct AlignmentInfo {
        friend class PoolAllocatorChain<size, align, false>;
        friend class PoolAllocatorChain<size, align, true>;

        static constexpr uintptr_t kMetaAlignMask = alignof(PoolChainAllocationMetaInfo) - 1u;
        static constexpr uintptr_t kAllocationSize = ((size + sizeof(PoolChainAllocationMetaInfo)) + kMetaAlignMask) & ~kMetaAlignMask;
        static constexpr uintptr_t kAllocationAlign = std::max(align, alignof(PoolChainAllocationMetaInfo));

        private:
            inline static PoolChainAllocationMetaInfo * metaInfo(void * m) noexcept {
                return reinterpret_cast<PoolChainAllocationMetaInfo*>(
                    reinterpret_cast<uintptr_t>(
                        (reinterpret_cast<std::byte*>(m) + size) + kMetaAlignMask) & ~kMetaAlignMask
                    );
        }
    };

    template <typename Allocator, typename Counter>
    struct PoolChunk {
        Allocator allocator;
        Counter allocations = {0u};

        explicit PoolChunk(size_t capacity) : allocator(capacity) {}

        inline size_t used_memory() const noexcept {
            return sizeof(PoolChunk) + allocator.used_memory();
        }

        PoolChunk(PoolChunk &&) noexcept = default;
        PoolChunk(const PoolChunk &) = delete;

        PoolChunk & operator=(PoolChunk && pool) = delete;
        PoolChunk & operator=(const PoolChunk & pool) = delete;
    };

    template<size_t bytes, size_t align, bool thread_safe>
    class PoolAllocatorChain;

    // common implementation
    template<size_t bytes, size_t align>
    class PoolAllocatorChain<bytes, align, false> final {
    public:
        using AllocationMetaInfo = PoolChainAllocationMetaInfo;
        using AlignInfo = AlignmentInfo<bytes, align>;
        using Allocator = PoolAllocator<AlignInfo::kAllocationSize, AlignInfo::kAllocationAlign, false>;
        using ChainedPool = PoolChunk<Allocator, uint32_t>;

        inline const AllocationMetaInfo & getMetaInfo(void * m) const noexcept {
            return *AlignInfo::metaInfo(m);
        }

        [[nodiscard]] inline void * alloc() noexcept {
            for (auto & pool : _poolsChain) {
                if (void * mem = pool->allocator.alloc()) {
                    AlignInfo::metaInfo(mem)->pool = pool;
                    ++pool->allocations;
                    return mem;
                };
            }

            if (_reservedPool || !_pools.nospace()) {
                auto * pool = _reservedPool ? _poolsChain.emplace_back(_reservedPool) :
                                                _poolsChain.emplace_back(_pools.template create<ChainedPool>(_poolCapacity));
                _reservedPool = nullptr;

                void * mem = pool->allocator.alloc();
                ++pool->allocations;
                AlignInfo::metaInfo(mem)->pool = pool;
                return mem;
            }
            
            assert(false); // not enough memory

            return nullptr;
        }

        inline bool free(void * mem) noexcept {
            const auto & info = *AlignInfo::metaInfo(mem);
            auto * pool = static_cast<ChainedPool*>(info.pool);

            if (pool->allocator.free(mem)) {
                if (--pool->allocations == 0u) {
                    setReservedPool(pool);
                }
                return true; 
            }
            return false;
        }

        // object type functions
        template<typename T, typename... Args>
        [[nodiscard]] inline T * create(Args &&... args) { // = alloc with type
            if (auto * m = alloc()) {
                return new(m) T(std::forward<Args>(args)...);
            }
            return nullptr;
        }

        template<typename T>
        inline bool destroy(T * value) noexcept {  // = free with type
            const auto & info = *AlignInfo::metaInfo(value);
            auto * pool = static_cast<ChainedPool*>(info.pool);

            if (pool->allocator.destroy(value)) {
                if (--pool->allocations == 0u) {
                    setReservedPool(pool);
                }
                return true; 
            }
            return false;
        }

        inline size_t used_memory() const noexcept { // in bytes
            const size_t staticMemory = sizeof(std::list<ChainedPool*>) + sizeof(ChainedPool*) +
                                        sizeof(size_t) * 2u + _pools.used_memory();
            const size_t poolsCount = _poolsChain.size();
            const size_t usefulMemory = (poolsCount + (_reservedPool ? 1u : 0u)) * _chunkMemorySize;
            const size_t additionaMemory = poolsCount * sizeof(ChainedPool*);
            return staticMemory + usefulMemory + additionaMemory;
        }

        PoolAllocatorChain(size_t poolCapacity, size_t poolsCount) :
            _poolCapacity(poolCapacity),
            _pools{poolsCount},
            _poolsChain{_pools.template create<ChainedPool>(_poolCapacity)} {
                _chunkMemorySize = _poolsChain.back()->used_memory();
        }

    private:
        inline void setReservedPool(ChainedPool* pool) noexcept {
            if (_reservedPool && _reservedPool != pool) {
                _pools.destroy(_reservedPool);
            }
            _poolsChain.remove(pool);
            _reservedPool = pool;
        }

        size_t _chunkMemorySize = 0u;
        size_t _poolCapacity = 0u;
        PoolAllocator<sizeof(ChainedPool), alignof(ChainedPool), false> _pools;
        std::list<ChainedPool*> _poolsChain;
        ChainedPool* _reservedPool = nullptr;
    };


    // thread safe implementation
    template<size_t bytes, size_t align>
    class PoolAllocatorChain<bytes, align, true> final {
    public:
        using AllocationMetaInfo = PoolChainAllocationMetaInfo;
        using AlignInfo = AlignmentInfo<bytes, align>;
        using Allocator = PoolAllocator<AlignInfo::kAllocationSize, AlignInfo::kAllocationAlign, true>;
        using ChainedPool = PoolChunk<Allocator, std::atomic<uint32_t>>;

        inline const AllocationMetaInfo & getMetaInfo(void * m) const noexcept {
            return *AlignInfo::metaInfo(m);
        }

        [[nodiscard]] inline void * alloc() noexcept {
            __alloc_start:
       
            threads::RWSpinLock::readLock(_rwLock);
            
            for (auto & pool : _poolsChain) {
                if (void * mem = pool->allocator.alloc()) {
                    AlignInfo::metaInfo(mem)->pool = pool;
                    pool->allocations.fetch_add(1u, std::memory_order_release);

                    //read unlock
                    _rwLock.fetch_sub(1, std::memory_order_release);

                    return mem;
                };
            }

            threads::RWSpinLock::readUnlock(_rwLock);

            {
                const auto chainSize = _poolsChain.size();

                threads::RWSpinLock::writeLock(_rwLock);

                if (chainSize != _poolsChain.size()) {
                    threads::RWSpinLock::writeUnlock(_rwLock);
                    goto __alloc_start; // state changed by other thread
                }

                if (_reservedPool || !_pools.nospace()) {
                    auto * pool = _reservedPool ? _reservedPool : _pools.template create<ChainedPool>(_poolCapacity);
                    _reservedPool = nullptr;
                    _poolsChain.emplace_back(pool);
                    threads::RWSpinLock::writeUnlock(_rwLock);
                    goto __alloc_start;
                }
            }
            
            assert(false); // not enough memory

            return nullptr;
        }

        inline bool free(void * mem) noexcept {
            const auto & info = *AlignInfo::metaInfo(mem);
            auto * pool = static_cast<ChainedPool*>(info.pool);

            if (pool->allocator.free(mem)) {
                if (pool->allocations.fetch_sub(1u, std::memory_order_acq_rel) == 1u) {
                    setReservedPool(pool);
                }
                return true; 
            }
            return false;
        }

        // object type functions
        template<typename T, typename... Args>
        [[nodiscard]] inline T * create(Args &&... args) { // = alloc with type
            if (auto * m = alloc()) {
                return new(m) T(std::forward<Args>(args)...);
            }
            return nullptr;
        }

        template<typename T>
        inline bool destroy(T * value) noexcept {  // = free with type
            const auto & info = *AlignInfo::metaInfo(value);
            auto * pool = static_cast<ChainedPool*>(info.pool);

            if (pool->allocator.destroy(value)) {
                if (pool->allocations.fetch_sub(1u, std::memory_order_acq_rel) == 1u) {
                    setReservedPool(pool);
                }
                return true; 
            }
            return false;
        }

        PoolAllocatorChain(size_t poolCapacity, size_t poolsCount) :
            _poolCapacity(poolCapacity),
            _pools{poolsCount},
            _poolsChain{_pools.template create<ChainedPool>(_poolCapacity)} {
        }

    private:
        inline void setReservedPool(ChainedPool* pool) noexcept {
            threads::RWSpinLock::writeLock(_rwLock);

            if (pool->allocations.load(std::memory_order_acquire) == 0u) {    
                if (_reservedPool && _reservedPool != pool) {
                    _pools.destroy(_reservedPool);
                }
                _reservedPool = pool;
                _poolsChain.remove(pool);
            }

            threads::RWSpinLock::writeUnlock(_rwLock);
        }

        size_t _poolCapacity = 0u;
        PoolAllocator<sizeof(ChainedPool), alignof(ChainedPool), false> _pools;
        std::list<ChainedPool*> _poolsChain;
        ChainedPool* _reservedPool = nullptr;
        std::atomic<int32_t> _rwLock = {0};
    };

}
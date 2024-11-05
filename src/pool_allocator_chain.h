#pragma once

#include "pool_allocator.h"
#include "threads.h"

#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <list>

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

        PoolAllocatorChain(size_t poolCapacity, size_t poolsCount) :
            _poolCapacity(poolCapacity),
            _pools{poolsCount},
            _poolsChain{_pools.template create<ChainedPool>(_poolCapacity)} {
        }

    private:
        inline void setReservedPool(ChainedPool* pool) noexcept {
            if (_reservedPool && _reservedPool != pool) {
                _pools.destroy(_reservedPool);
            }
            _poolsChain.remove(pool);
            _reservedPool = pool;
        }

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
            // read lock
            auto v = _rwLock.load(std::memory_order_acquire);
            while (v < 0 || !_rwLock.compare_exchange_weak(v, v + 1, std::memory_order_acq_rel, std::memory_order_relaxed)) {
                v = _rwLock.load(std::memory_order_acquire);
            }
       
            for (auto & pool : _poolsChain) {
                if (void * mem = pool->allocator.alloc()) {
                    AlignInfo::metaInfo(mem)->pool = pool;
                    pool->allocations.fetch_add(1u, std::memory_order_release);

                    //read unlock
                    _rwLock.fetch_sub(1, std::memory_order_release);

                    return mem;
                };
            }

            //read unlock
            _rwLock.fetch_sub(1, std::memory_order_release);

            {
                const auto chainSize = _poolsChain.size();

                threads::SpinLock lock(_reservedLock);
                if (chainSize != _poolsChain.size()) {
                    lock.unlock();
                    goto __alloc_start; // state changed by other thread
                }

                if (_reservedPool || !_pools.nospace()) {
                    auto * pool = _reservedPool ? _reservedPool : _pools.template create<ChainedPool>(_poolCapacity);
                    _reservedPool = nullptr;
                    _poolsChain.emplace_back(pool);
                    lock.unlock();
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
            // write lock
            int32_t v = 0;
            while (!_rwLock.compare_exchange_weak(v, -1, std::memory_order_acq_rel, std::memory_order_relaxed)) { 
                v = 0; // it will set it to what it currently held
            }

            threads::SpinLock lock(_reservedLock);
            if (pool->allocations.load(std::memory_order_acquire) == 0u) {    
                if (_reservedPool && _reservedPool != pool) {
                    _pools.destroy(_reservedPool);
                }
                _reservedPool = pool;
                _poolsChain.remove(pool);
            }

            // write unlock
            _rwLock.store(0, std::memory_order_release);
        }

        size_t _poolCapacity = 0u;
        PoolAllocator<sizeof(ChainedPool), alignof(ChainedPool), false> _pools;
        std::list<ChainedPool*> _poolsChain;
        ChainedPool* _reservedPool = nullptr;
        std::atomic_flag _reservedLock = ATOMIC_FLAG_INIT;
        std::atomic<int32_t> _rwLock = {0};
    };

}
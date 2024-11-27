#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>
//#include <tuple>

namespace allocators {
    template<size_t bytes, typename...Types>
    class Value final {
        template<size_t sizeInBytes, typename... Ts>
        struct max_size : std::integral_constant<std::size_t, std::max({sizeInBytes, sizeof(Ts)...})> {};
        
        //template<typename... Ts>
        //struct types_size : std::integral_constant<std::size_t, sizeof...(Ts)> {};
        //using types = std::tuple<Types...>;
        //using first_type = typename std::tuple_element<0u, types>::type;
        //using types_count = types_size<Types...>;

    public:
        Value() = default;
        Value(Value &&) = delete;
        Value(const Value &obj) = delete;
        Value &operator=(Value &&obj) = delete;
        Value &operator=(const Value &obj) = delete;

        template<typename U>
        [[maybe_unused]] explicit Value(U &&obj) noexcept {
            new(_data.data()) U(std::forward<U>(obj));
        }

        template<typename U, typename... Ts>
        [[maybe_unused]] explicit Value(Ts &&... ts) noexcept {
            new(_data.data()) U(std::forward<Ts>(ts)...);
        }

        ~Value() = default;

        template<typename U>
        U & emplace(U &&obj) noexcept {
            return *new(_data.data()) U(std::forward<U>(obj));
        }

        template<typename U, typename... Ts>
        U & emplace(Ts &&... ts) noexcept {
            return *new(_data.data()) U(std::forward<Ts>(ts)...);
        }

        template<typename U>
        U & get() noexcept {
            return *reinterpret_cast<U *>(_data.data());
        }

        template<typename U>
        const U & get() const noexcept {
            return *reinterpret_cast<U *>(_data.data());
        }

    private:
        std::array<std::byte, max_size<bytes, Types...>::value> _data;
    };
}

namespace std {
    template <typename T, size_t bytes, typename...Args>
    T & get(const allocators::Value<bytes, Args...> & v) noexcept {
        return v.template get<T>();
    }

    template <typename T, size_t bytes, typename...Args>
    T & get(allocators::Value<bytes, Args...> & v) noexcept {
        return v.template get<T>();
    }
}

namespace allocators {
    template<size_t bytes, size_t align, bool thread_safe = false>
    class PoolAllocator final {
        template<bool, typename Dummy = int>
        struct common_pointer_type {
            using type = void*;
        };
        template<typename Dummy>
        struct common_pointer_type<true, Dummy> {
            using type = std::atomic<void*>;
        };

        struct AllocationHolder final {
            explicit AllocationHolder(void* v) : next(v) {}
            void* next = nullptr;
        };

        struct AllocationOnly final {};

        using ponter_type = typename common_pointer_type<thread_safe>::type;
        using memory_element_type = Value<bytes, AllocationOnly, AllocationHolder>;

    public:
        inline constexpr size_t memory_align() const noexcept { return align; };
        inline static consteval size_t element_size() noexcept {
            return (bytes > sizeof(AllocationHolder)) ? bytes : sizeof(AllocationHolder);
        }

        inline size_t memory_size() const noexcept { // in bytes
            constexpr uintptr_t mask = align - 1u;
            constexpr auto size = element_size();
            return (_capacity * size) + mask;
        } 

        inline size_t capacity() const noexcept { return _capacity; }

        inline size_t used_memory() const noexcept {
            return sizeof(PoolAllocator) +  memory_size();
        }

        explicit PoolAllocator(size_t capacity) : _capacity(capacity) {
            assert(capacity > 0u);
            constexpr uintptr_t mask = align - 1u; // size_t ?
            constexpr auto size = element_size();
            static_assert((mask & align) == 0u); // check align is pow2
            
            _allocatedMemory = malloc((_capacity * size) + mask);
            _memory = reinterpret_cast<memory_element_type*>((reinterpret_cast<uintptr_t>(_allocatedMemory) + mask) & ~mask);

            for (size_t i = 0u; i < _capacity; ++i) {
                _memory[i].template emplace<AllocationHolder>(&_memory[i + 1u]);
            }
            _current = &_memory[0u];
        }

        ~PoolAllocator() {
            if (_allocatedMemory) {
                free(_allocatedMemory);
            }
        }

        PoolAllocator(const PoolAllocator & allocator) = delete;
        PoolAllocator & operator=(const PoolAllocator &allocator) = delete;

        PoolAllocator(PoolAllocator && allocator) noexcept : 
            _capacity(allocator._capacity), 
            _allocatedMemory(allocator._allocatedMemory),
            _memory(allocator._memory) {
            if constexpr (thread_safe) {
                allocator._current = _current.exchange(allocator._current, std::memory_order_acq_rel);
            } else {
                std::swap(_current, allocator._current);
            }

            allocator._allocatedMemory = nullptr;
        };

        PoolAllocator & operator= (PoolAllocator && allocator) = delete;
        
        [[nodiscard]] inline void * alloc() noexcept {
            if (auto * m = fetch_add()) {
                return &m->template emplace<AllocationOnly>();
            }
            return nullptr;
        }

        inline bool free(void * value) noexcept {
            const auto id = (reinterpret_cast<memory_element_type *>(value) - &_memory[0u]);
            if (id >= 0u && id < _capacity) {
                fetch_sub(id, value);
                return true;
            }
            return false;
        }

        inline const memory_element_type* memory() const noexcept { return _memory; }

        [[nodiscard]] inline bool nospace() const noexcept {
            if constexpr (thread_safe) {
                return _current.load(std::memory_order_acquire) == &_memory[_capacity];
            } else {
                return _current == &_memory[_capacity];
            }
        }

        // object type functions
        template<typename T, typename... Args>
        [[nodiscard]] inline T * create(Args &&... args) { // = alloc with type
            if (auto * m = fetch_add()) {
                return &m->template emplace<T>(std::forward<Args>(args)...);
            }
            return nullptr;
        }

        template<typename T>
        inline bool destroy(T * value) noexcept {  // = free with type
            const auto id = (reinterpret_cast<memory_element_type *>(value) - &_memory[0u]);
            if (id >= 0u && id < _capacity) {
                value->~T();
                fetch_sub(id, value);
                return true;
            }
            return false;
        }

    private:
        [[nodiscard]] inline memory_element_type * fetch_add() noexcept {
            if constexpr (thread_safe) {
                if (void * value = _current.load(std::memory_order_acquire); value != &_memory[_capacity]) {
                    while (!_current.compare_exchange_weak(value, static_cast<AllocationHolder*>(value)->next, std::memory_order_release, std::memory_order_relaxed)) {
                        if (value == &_memory[_capacity]) {
                            return nullptr;
                        }
                    }
                    return reinterpret_cast<memory_element_type *>(value);
                } 
            } else {
                if (void * value = _current; value != &_memory[_capacity]) {
                    _current = static_cast<AllocationHolder*>(value)->next;
                    return reinterpret_cast<memory_element_type *>(value);
                }
            }

            return nullptr;
        }

        inline void fetch_sub(ptrdiff_t id, void * value) noexcept {
            if constexpr (thread_safe) {
                auto & aHolder = _memory[id].template emplace<AllocationHolder>(_current.load(std::memory_order_acquire));
                while (!_current.compare_exchange_weak(aHolder.next, &aHolder, std::memory_order_release, std::memory_order_relaxed));
            //    _memory[id].template emplace<AllocationHolder>(_current.exchange(value, std::memory_order_acq_rel));
            } else {
                _memory[id].template emplace<AllocationHolder>(_current);
                _current = value;
            }
        }

        size_t _capacity = 0u;
        void* _allocatedMemory = nullptr;
        memory_element_type* _memory = nullptr;
        ponter_type _current = nullptr;
    };
}
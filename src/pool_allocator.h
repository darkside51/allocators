#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <tuple>
#include <vector>

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
    template<size_t bytes, size_t count, size_t align, bool thread_safe = false>
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
        PoolAllocator() {
            constexpr size_t mask = align - 1u;
            constexpr auto size = (bytes > sizeof(AllocationHolder)) ? bytes : sizeof(AllocationHolder);
            static_assert((mask & align) == 0u); // check align is pow2
            
            _allocatedMemory = malloc((count * size) + mask);
            _memory = reinterpret_cast<memory_element_type*>((reinterpret_cast<uintptr_t>(_allocatedMemory) + mask) & ~mask);

            for (size_t i = 0u; i < count; ++i) {
                _memory[i].template emplace<AllocationHolder>(&_memory[i + 1u]);
            }
            _current = &_memory[0u];
        }

        ~PoolAllocator() {
            free(_allocatedMemory);
        }

        PoolAllocator(PoolAllocator &&allocator) = delete;
        PoolAllocator(const PoolAllocator &allocator) = delete;
        PoolAllocator & operator=(PoolAllocator &&allocator) = delete;
        PoolAllocator & operator=(const PoolAllocator &allocator) = delete;

        [[nodiscard]] void * alloc() noexcept {
            return &fetch()->template emplace<AllocationOnly>();
        }

        void free(void *value) noexcept {
            const auto id = (reinterpret_cast<memory_element_type *>(value) - &_memory[0u]);
            assert(id >= 0u && id < count);
            if constexpr (thread_safe) {
                _memory[id].template emplace<AllocationHolder>(_current.exchange(value, std::memory_order_acq_rel));
            } else {
                _memory[id].template emplace<AllocationHolder>(_current);
                _current = value;
            }
        }

        const std::vector<memory_element_type> &memory() const noexcept { return _memory; }

        [[nodiscard]] bool empty() const noexcept {
            if constexpr (thread_safe) {
                return _current.load(std::memory_order_acquire) == &*_memory[count];
            } else {
                return _current == &*_memory[count];
            }
        }

        // object type functions
        template<typename T, typename... Args>
        [[nodiscard]] T * create(Args &&... args) { // = alloc with type
            return &fetch()->template emplace<T>(std::forward<Args>(args)...);
        }

        template<typename T>
        void destroy(T * value) noexcept {  // = free with type
            const auto id = (reinterpret_cast<memory_element_type *>(value) - &_memory[0u]);
            assert(id >= 0u && id < count);
            value->~T();
            if constexpr (thread_safe) {
                _memory[id].template emplace<AllocationHolder>(_current.exchange(value, std::memory_order_acq_rel));
            } else {
                _memory[id].template emplace<AllocationHolder>(_current);
                _current = value;
            }
        }

    private:
        [[nodiscard]] memory_element_type * fetch() noexcept {
            if constexpr (thread_safe) {
                void* value = _current.load(std::memory_order_acquire);
                while (!_current.compare_exchange_weak(value, static_cast<AllocationHolder*>(value)->next, std::memory_order_acq_rel));
                return reinterpret_cast<memory_element_type *>(value);
            } else {
                void* value = _current;
                _current = static_cast<AllocationHolder*>(value)->next;
                return reinterpret_cast<memory_element_type *>(value);
            }
        }

        void* _allocatedMemory = nullptr;
        memory_element_type* _memory = nullptr;
        ponter_type _current = nullptr;
    };
}
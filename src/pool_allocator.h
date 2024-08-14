#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <tuple>
#include <vector>

#ifdef STD_VARIANT_ALLOCATOR_VALUE
#include <variant>
#else
namespace allocators {
    template<typename...Types>
    class Value final {
        template<typename... Ts>
        struct types_size : std::integral_constant<std::size_t, sizeof...(Ts)> {};

        template<typename... Ts>
        struct max_sizeof : std::integral_constant<std::size_t, std::max({sizeof(Ts)...})> {};

        using types = std::tuple<Types...>;
        using first_type = typename std::tuple_element<0u, types>::type;
        using types_count = types_size<Types...>;

    public:
        Value() = default;
        [[maybe_unused]] Value(Value &&) noexcept {
            assert(false);
        }
        Value(const Value &obj) = delete;
        Value &operator=(Value &&obj) = delete;
        Value &operator=(const Value &obj) = delete;

        template<typename U, std::enable_if_t<!std::is_same_v<U, Value<Types...>>, bool> = true>
        [[maybe_unused]] explicit Value(U &&obj) noexcept {
            if constexpr (std::is_same_v<U, first_type>) { _assigned = 1u; }
            new(_data.data()) U(std::forward<U>(obj));
        }

        template<typename U, typename... Ts>
        [[maybe_unused]] explicit Value(Ts &&... ts) noexcept {
            if constexpr (std::is_same_v<U, first_type>) { _assigned = 1u; }
            new(_data.data()) U(std::forward<Ts>(ts)...);
        }

        ~Value() {
            destroy();
        }

        inline void destroy() noexcept {
            if (_assigned == 1u) {
                _assigned = 0u;
                reinterpret_cast<first_type *>(_data.data())->~first_type();
            }
        }

        template<typename U>
        U & emplace(U &&obj) noexcept {
            destroy();
            if constexpr (std::is_same_v<U, first_type>) {
                _assigned = 1u;
            }
            return *new(_data.data()) U(std::forward<U>(obj));
        }

        template<typename U, typename... Ts>
        U & emplace(Ts &&... ts) noexcept {
            destroy();
            if constexpr (std::is_same_v<U, first_type>) {
                _assigned = 1u;
            }
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
        std::array<std::byte, max_sizeof<Types...>::value> _data;
        uint8_t _assigned = 0u;
    };
}

namespace std {
    template <typename T, typename...Args>
    T & get(const allocators::Value<Args...> & v) noexcept {
        return v.template get<T>();
    }

    template <typename T, typename...Args>
    T & get(allocators::Value<Args...> & v) noexcept {
        return v.template get<T>();
    }
}
#endif

namespace allocators {
    template<typename T, size_t size, bool thread_safe = true>
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
#if defined STD_VARIANT_ALLOCATOR_VALUE
        using memory_element_type = std::variant<T, AllocationOnly, AllocationHolder>;
#else
        using memory_element_type = Value<T, AllocationOnly, AllocationHolder>;
#endif

    public:
        PoolAllocator() {
            _memory.reserve(size);
            for (size_t i = 0u; i < size; ++i) {
                _memory.emplace_back(AllocationHolder(&_memory[i + 1u]));
            }
            _current = &_memory[0u];
        }

        ~PoolAllocator() = default;

        PoolAllocator(PoolAllocator &&allocator) = delete;
        PoolAllocator(const PoolAllocator &allocator) = delete;
        PoolAllocator & operator=(PoolAllocator &&allocator) = delete;
        PoolAllocator & operator=(const PoolAllocator &allocator) = delete;

        template<typename... Args>
        [[nodiscard]] T * create(Args &&... args) {
            return &fetch()->template emplace<T>(std::forward<Args>(args)...);
        }

        template<typename... Args>
        [[nodiscard]] T & createValue(Args &&... args) {
            return fetch()->template emplace<T>(std::forward<Args>(args)...);
        }

        [[nodiscard]] void * alloc() noexcept {
            return &fetch()->template emplace<AllocationOnly>();
        }

        void free(void *value) noexcept {
            const auto id = (reinterpret_cast<memory_element_type *>(value) - &_memory[0U]);
            assert(id >= 0u && id < size);
            if constexpr (thread_safe) {
                _memory[id].template emplace<AllocationHolder>(_current.exchange(value, std::memory_order_acq_rel));
            } else {
                _memory[id].template emplace<AllocationHolder>(_current);
                _current = value;
            }
        }

        void freeValue(T & value) noexcept { free(&value); }

        const std::vector<memory_element_type> &memory() const noexcept { return _memory; }

        [[nodiscard]] bool empty() const noexcept {
            if constexpr (thread_safe) {
                return _current.load(std::memory_order_acquire) == &*_memory.end();
            } else {
                return _current == &*_memory.end();
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

        std::vector<memory_element_type> _memory;
        ponter_type _current = nullptr;
    };
}
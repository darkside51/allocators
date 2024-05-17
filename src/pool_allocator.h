#pragma once

#include <array>
#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <tuple>
#include <vector>

#ifdef STD_VARIANT_ALLOCATOR_VALUE
#include <variant>
#endif

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

namespace allocators {
    template<typename T, size_t size, bool thread_safe = true>
    class PoolAllocator final {
        template<bool, typename Dummy = int>
        struct common_counter_type {
            using type = size_t;
        };
        template<typename Dummy>
        struct common_counter_type<true, Dummy> {
            using type = std::atomic<size_t>;
        };

        class SpinLock final {
        public:
            explicit SpinLock(std::atomic_flag &l) : _locker(l) {
                while (_locker.test_and_set(std::memory_order_acquire)) {}
            }
            ~SpinLock() { _locker.clear(std::memory_order_release); }

            SpinLock(SpinLock &&allocator) = delete;
            SpinLock(const SpinLock &allocator) = delete;
            SpinLock & operator=(SpinLock &&allocator) = delete;
            SpinLock & operator=(const SpinLock &allocator) = delete;

        private:
            std::atomic_flag &_locker;
        };

        template<typename>
        struct locker_type {
            using type = std::byte;
        };
        template<typename U>
        struct locker_type<std::atomic<U>> {
            using type = std::atomic_flag;
        };

        struct AllocationHolder {
            explicit AllocationHolder(size_t v) : next(v) {}
            size_t next = 0u;
        };

        struct AllocationOnly {};

        using counter_type = typename common_counter_type<thread_safe>::type;
#if defined STD_VARIANT_ALLOCATOR_VALUE
        using memory_element_type = std::variant<T, AllocationOnly, AllocationHolder>;
#else
        using memory_element_type = Value<T, AllocationOnly, AllocationHolder>;
#endif

    public:
        PoolAllocator() {
            _memory.reserve(size);
            for (size_t i = 0U; i < size; ++i) {
                _memory.emplace_back(AllocationHolder{i + 1u});
            }
        }

        ~PoolAllocator() = default;

        PoolAllocator(PoolAllocator &&allocator) = delete;
        PoolAllocator(const PoolAllocator &allocator) = delete;
        PoolAllocator & operator=(PoolAllocator &&allocator) = delete;
        PoolAllocator & operator=(const PoolAllocator &allocator) = delete;

        template<typename... Args>
        [[nodiscard]] T * create(Args &&... args) {
            return &_memory[fetch()].template emplace<T>(std::forward<Args>(args)...);
        }

        template<typename... Args>
        [[nodiscard]] T & createValue(Args &&... args) {
            return _memory[fetch()].template emplace<T>(std::forward<Args>(args)...);
        }

        [[nodiscard]] void * alloc() noexcept {
            return &_memory[fetch()].template emplace<AllocationOnly>();
        }

        void free(void *value) noexcept {
            const auto id = (reinterpret_cast<memory_element_type *>(value) - &_memory[0U]);
            assert(id >= 0u && id < size);
            if constexpr (thread_safe) {
                _memory[id].template emplace<AllocationHolder>(_current.exchange(id, std::memory_order_acq_rel));
            } else {
                _memory[id].template emplace<AllocationHolder>(_current);
                _current = id;
            }
        }

        void freeValue(T & value) noexcept { free(&value); }

        const std::vector<memory_element_type> &memory() const noexcept { return _memory; }

        [[nodiscard]] bool empty() const noexcept {
            if constexpr (thread_safe) {
                return _current.load(std::memory_order_acquire) == size;
            } else {
                return _current == size;
            }
        }

    private:
        [[nodiscard]] size_t fetch() noexcept {
            if constexpr (thread_safe) {
                SpinLock l(_flag); // how disable this step?
                const size_t id = _current.load(std::memory_order_acquire);
                assert(id < size);
                _current.store(std::get<AllocationHolder>(_memory[id]).next, std::memory_order_release);
                return id;
            } else {
                assert(_current < size);
                const size_t id = _current;
                _current = std::get<AllocationHolder>(_memory[id]).next;
                return id;
            }
        }

        std::vector<memory_element_type> _memory;
        counter_type _current = {0u};
        [[maybe_unused]] typename locker_type<counter_type>::type _flag = {};
    };
}
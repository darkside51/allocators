#pragma once

#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <span>

namespace allocators {
    template<size_t sizeInBytes, bool top_down = true>
    class StackAllocatorImpl final {
    public:
        explicit StackAllocatorImpl(std::span<std::byte> memory) : _memory(memory) { clear(); }

        StackAllocatorImpl(StackAllocatorImpl &&allocator) = delete;
        StackAllocatorImpl(const StackAllocatorImpl &allocator) = delete;
        StackAllocatorImpl & operator=(StackAllocatorImpl &&allocator) = delete;
        StackAllocatorImpl & operator=(const StackAllocatorImpl &allocator) = delete;

        void *alloc(size_t bytes) noexcept {
            if constexpr (top_down) {
                assert(_head >= bytes);
                _head -= bytes;
                return &_memory[_head];
            } else {
                void *result = &_memory[_head];
                _head += bytes;
                assert(_head <= sizeInBytes);
                return result;
            }
        }

        void *allocAligned(size_t bytes, size_t align) noexcept {
            const size_t bytes_aligned = (bytes + align - 1u);
            auto * mem = alloc(bytes_aligned);
            return reinterpret_cast<void*>(alignedAddress(reinterpret_cast<uintptr_t>(mem), align));
        }

        template <typename T, typename...Args>
        [[nodiscard]] T * create(Args&&... args) {
            return new(alloc(sizeof(T))) T(std::forward<Args>(args)...);
        }

        template <typename T, typename...Args>
        [[nodiscard]] T * createAligned(size_t align, Args&&... args) {
            return new(allocAligned(sizeof(T), align)) T(std::forward<Args>(args)...);
        }

        template <typename T>
        inline void destroy(T* ptr) const noexcept {
            assert(ptr >= reinterpret_cast<T*>(&_memory[0]) && ptr <= reinterpret_cast<T*>(&_memory[sizeInBytes - 1u]));
            ptr->~T();
        }

        inline void free(size_t marker) noexcept {
            if constexpr (top_down) {
                assert(marker <= sizeInBytes);
            }
            _head = marker;
        }

        inline void clear() noexcept {
            if constexpr (top_down) {
                _head = sizeInBytes;
            } else {
                _head = 0u;
            }
        }

        [[nodiscard]] inline size_t head() const noexcept { return _head; }

        [[nodiscard]] inline size_t freeBytesCount() const noexcept {
            if constexpr (top_down) {
                return _head;
            } else {
                return sizeInBytes - _head;
            }
        }

    private:
        [[nodiscard]] inline uintptr_t alignedAddress(uintptr_t ptr, size_t align) const noexcept {
            const size_t mask = align - 1u;
            assert((mask & align) == 0u); // check is pow2
            return (ptr + mask) & ~mask;
        }

        std::span<std::byte> _memory;
        size_t _head = 0u;
    };

    template<size_t sizeInBytes>
    class StackAllocator final {
        using type = StackAllocatorImpl<sizeInBytes, true>;
    public:
        StackAllocator() : _stack(_memory) {}
        inline type &stack() noexcept { return _stack; }
        inline type* operator->() noexcept { return &_stack; }

    private:
        std::array<std::byte, sizeInBytes> _memory;
        type _stack;
    };

    template<size_t sizeInBytes>
    class DualStackAllocator final {
        using top_stack_type = StackAllocatorImpl<sizeInBytes / 2u, true>;
        using bottom_stack_type = StackAllocatorImpl<sizeInBytes - sizeInBytes / 2u, false>;
    public:
        DualStackAllocator() : _topStack(std::span<std::byte>{_memory.begin() + sizeInBytes / 2u, _memory.end()}),
                               _bottomStack(_memory) {}

        inline top_stack_type &top() noexcept { return _topStack; }
        inline bottom_stack_type &bottom() noexcept { return _bottomStack; }

    private:
        std::array<std::byte, sizeInBytes> _memory;
        top_stack_type _topStack;
        bottom_stack_type _bottomStack;
    };

    template<size_t sizeInBytes, bool top_down = true>
    class StackScope {
        using type = StackAllocatorImpl<sizeInBytes, top_down>;
    public:
        explicit StackScope(type & stack) : _stack(stack), _marker(stack.head()) {}
        ~StackScope() { _stack.free(_marker); }

        StackScope(StackScope &&allocator) = delete;
        StackScope(const StackScope &allocator) = delete;
        StackScope & operator=(StackScope &&allocator) = delete;
        StackScope & operator=(const StackScope &allocator) = delete;

    private:
        size_t _marker;
        type & _stack;
    };
}
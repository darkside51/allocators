#include "src/pool_allocator.h"
#include "src/stack_allocator.h"

#include <cstdio>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

class TestObject {
public:
    TestObject(uint32_t x, uint32_t y, std::string s) : x(x), y(y), s(std::move(s)) {}
    ~TestObject() {
        printf("~TestObject()\n");
    }
    uint32_t x = 0u;
    uint32_t y = 0u;
    std::string s;
};

int main() {
    {
        printf("PoolAllocator example begin\n");

        allocators::PoolAllocator<sizeof(TestObject), alignof(TestObject), false> pool{16u};
        auto *objPtr0 = pool.create<TestObject>(10u, 20u, "abc");
        [[maybe_unused]] auto *objPtr1 = pool.create<TestObject>(11u, 21u, "abc1");
        auto * obj0 = pool.create<TestObject>(110u, 210u, "def");

        auto * mem = pool.alloc();
        auto *ptr = new(mem) TestObject(330, 444, "aaa");

        pool.free(objPtr0);
        pool.destroy(obj0);

        ptr->~TestObject();
        pool.free(mem);
        objPtr0 = nullptr;

        [[maybe_unused]] auto *objPtr2 = pool.create<TestObject>(12u, 22u, "abc2");
        [[maybe_unused]] auto * obj1 = pool.create<TestObject>(111u, 211u, "def1");
        [[maybe_unused]] auto * obj2 = pool.create<TestObject>(1112u, 2112u, "def2");

        printf("PoolAllocator example end\n");
    }

    {
        printf("async PoolAllocator example begin\n");
        allocators::PoolAllocator<sizeof(TestObject), alignof(TestObject), true> aPool{16u};

        std::vector<std::thread> threads;
        for (size_t i = 0; i < 4; ++i) {
            threads.emplace_back([&aPool, i]() {
                for (size_t j = 0; j < 4; ++j) {
                    [[maybe_unused]] auto * value = aPool.create<TestObject>(j + i * 10, i + j * 10 + 1, "");
                    std::this_thread::sleep_for(std::chrono::milliseconds(rand() % 4));
                }
            });
        }

        for (auto &t: threads) {
            t.join();
        }

        printf("async PoolAllocator example end\n");
    }

    {
        printf("StackAllocator example begin\n");

        allocators::StackAllocator<1024u> stack;
        auto * m = stack->alloc(4u);
        [[maybe_unused]] auto v0 = new(m) uint32_t(111);

        auto marker = stack->head();
        auto * m1 = stack->alloc(1u);
        [[maybe_unused]] auto v1 = new(m1) uint8_t(222);
        stack->free(marker);

        {
            allocators::StackScope scope(stack.stack());
            auto m3 = stack->alloc(4u);
            [[maybe_unused]] auto v3 = new(m3) uint32_t(444);
        }

        [[maybe_unused]] auto * vm0 = stack->create<uint32_t>(12345u);
        auto * vm1 = stack->createAligned<uint32_t>(4u, 123456u);
        stack->destroy(vm1);

        printf("StackAllocator example end\n");
    }

    {
        printf("DualStackAllocator example begin\n");

        allocators::DualStackAllocator<16u> stack;
        [[maybe_unused]] auto dm0 = stack.top().create<uint32_t>(111u);
        [[maybe_unused]] auto dm1 = stack.top().create<uint32_t>(222u);

        auto dm2 = stack.bottom().alloc(4u);
        auto dm3 = stack.bottom().alloc(4u);
        [[maybe_unused]] auto dv2 = new(dm2) uint32_t(333);
        [[maybe_unused]] auto dv3 = new(dm3) uint32_t(444);

        printf("DualStackAllocator example end\n");
    }

    return 0;
}

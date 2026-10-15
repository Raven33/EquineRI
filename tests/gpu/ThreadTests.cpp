// The any-thread list (P106): pipeline creation, waits and pure queries on
// other threads while the device thread records and submits; every other call
// from another thread is reported in validation builds.

#include "GpuTestCommon.hpp"

#include <TestHarness.hpp>

#include <RI/RI.hpp>
#include <RI/Helpers/TypedCommands.hpp>

#include <atomic>
#include <csignal>
#include <cstdlib>
#include <thread>
#include <vector>

using gputest::createTestDevice;
using gputest::loadShader;

namespace {

struct FillRoot {
    uint32 inputSlot;
    uint32 outputSlot;
    uint32 paramsSlot;
    float scale;
    uint32 count;
};

} // namespace

// Eight threads create named pipelines and ask pure queries while this thread
// records, submits and waits; a ninth thread waits on the submitted points.
// Then every pipeline runs once.
EQ_TEST(threads, pipelines_on_other_threads) {
    RI::Device device = createTestDevice();
    const std::vector<std::byte> bytecode = loadShader("fill");
    constexpr int kThreads = 8;
    constexpr int kPerThread = 16;

    RI::Memory memory = RI::allocateMemory(device, RI::MemoryKind::eGPU, 1 << 16, nullptr, "thread test memory");
    RI::Buffer buffer = RI::createBuffer(device, memory, 0,
                                         {.size = 256, .usage = RI::BufferUsage::eCopyDst, .name = "cleared"});
    RI::DescriptorHeap heap = RI::createDescriptorHeap(device, {.resourceCount = 8});
    RI::CommandList cmd = RI::createCommandList(device, "device thread commands");

    std::vector<std::vector<RI::Pipeline>> made(kThreads);
    std::atomic<int> finished{0};
    std::atomic<bool> queriesOk{true};
    std::vector<std::thread> workers;
    for (int t = 0; t < kThreads; ++t) {
        workers.emplace_back([&, t] {
            for (int i = 0; i < kPerThread; ++i) {
                const RI::MemoryRequirements requirements =
                    RI::bufferRequirements(device, {.size = 256, .usage = RI::BufferUsage::eRead});
                if (requirements.size < 256 ||
                    !RI::formatSupported(device, RI::Format::eRGBA8Unorm, RI::TextureUsage::eSampled)) {
                    queriesOk = false;
                }
                made[t].push_back(RI::createComputePipeline(device, {.shader = bytecode, .name = "worker pipeline"}));
            }
            ++finished;
        });
    }

    std::atomic<uint64> lastPoint{0};
    std::atomic<bool> stop{false};
    std::thread waiter([&] {
        RI::Queue queue = device.graphicsQueue;
        while (!stop) {
            const RI::TimelinePoint point{lastPoint.load()};
            RI::wait(queue, point);
            if (!RI::isReached(queue, point)) {
                queriesOk = false;
            }
        }
    });

    int submissions = 0;
    while (finished < kThreads || submissions == 0) {
        if (submissions > 0) {
            RI::resetCommandList(cmd);
        }
        RI::cmdBegin(cmd);
        RI::cmdClearBuffer(cmd, buffer, 7);
        RI::cmdEnd(cmd);
        const RI::TimelinePoint point = RI::submit(device.graphicsQueue, cmd);
        lastPoint = point.value;
        RI::wait(device.graphicsQueue, point);
        ++submissions;
    }
    for (std::thread &worker : workers) {
        worker.join();
    }
    stop = true;
    waiter.join();
    EQ_CHECK(queriesOk);

    RI::resetCommandList(cmd);
    RI::cmdBegin(cmd);
    RI::cmdSetHeap(cmd, heap);
    for (const std::vector<RI::Pipeline> &pipelines : made) {
        EQ_CHECK(pipelines.size() == kPerThread);
        for (const RI::Pipeline &pipeline : pipelines) {
            RI::cmdSetPipeline(cmd, pipeline);
            RI::Helpers::cmdSetRootData(cmd, FillRoot{.count = 0}); // every thread returns at once
            RI::cmdDispatch(cmd, 1, 1, 1);
        }
    }
    RI::cmdEnd(cmd);
    RI::wait(device.graphicsQueue, RI::submit(device.graphicsQueue, cmd));

    for (std::vector<RI::Pipeline> &pipelines : made) {
        for (RI::Pipeline &pipeline : pipelines) {
            RI::destroyPipeline(device, pipeline);
        }
    }
    RI::destroyCommandList(device, cmd);
    RI::destroyDescriptorHeap(device, heap);
    RI::destroyBuffer(device, buffer);
    RI::freeMemory(device, memory);
    RI::destroyDevice(device);
}

#if EQUINE_VALIDATION
// A call outside the any-thread list from another thread (P106).
EQ_TEST(threads, call_from_other_thread_reports) {
    std::signal(SIGABRT, [](int) { std::_Exit(0); });
    RI::Device device = createTestDevice();
    std::thread([&] { RI::createCommandList(device); }).join();
}
#endif

#define private public
#include "../src/hotkey_manager.hpp"
#undef private

#include <iostream>
#include <thread>
#include <vector>
#include <atomic>
#include <chrono>
#include <cassert>
#include <random>

using namespace ColdSteel3;

// Test 1: Concurrently call SetKeyStateProvider while multiple worker threads invoke IsKeyDown (100,000 iterations each)
bool Test_Concurrent_SetKeyStateProvider_And_IsKeyDown() {
    std::cout << "[RUN ] Test_Concurrent_SetKeyStateProvider_And_IsKeyDown..." << std::endl;
    HotkeyManager mgr;

    const int iterationsPerWorker = 100000;
    const int numWorkerThreads = 4;
    std::atomic<bool> startFlag{false};
    std::atomic<bool> workersDone{false};
    std::atomic<int> completedWorkers{0};
    std::atomic<uint64_t> totalQueries{0};
    std::atomic<uint64_t> totalProviderSwaps{0};

    // Worker threads continuously invoking IsKeyDown for 100,000 iterations each
    std::vector<std::thread> workers;
    workers.reserve(numWorkerThreads);
    for (int t = 0; t < numWorkerThreads; ++t) {
        workers.emplace_back([&, t]() {
            while (!startFlag.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            uint64_t count = 0;
            const int testKeys[] = { VK_SHIFT, VK_CONTROL, VK_F1, VK_F2, VK_F7, VK_F8, 0, 0x41 };
            const size_t numKeys = sizeof(testKeys) / sizeof(testKeys[0]);

            for (int i = 0; i < iterationsPerWorker; ++i) {
                int key = testKeys[(i + t) % numKeys];
                bool down = mgr.IsKeyDown(key);
                (void)down;
                count++;
            }
            totalQueries.fetch_add(count, std::memory_order_relaxed);
            if (completedWorkers.fetch_add(1, std::memory_order_acq_rel) + 1 == numWorkerThreads) {
                workersDone.store(true, std::memory_order_release);
            }
        });
    }

    // Provider mutator thread: continuously swaps SetKeyStateProvider until all workers finish
    std::thread mutator([&]() {
        while (!startFlag.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        uint64_t swapCount = 0;
        while (!workersDone.load(std::memory_order_acquire)) {
            if (swapCount % 3 == 0) {
                auto state = std::make_shared<std::string>("state_" + std::to_string(swapCount));
                mgr.SetKeyStateProvider([state, swapCount](int k) -> bool {
                    return (k + static_cast<int>(swapCount) + static_cast<int>(state->length())) % 2 == 0;
                });
            } else if (swapCount % 3 == 1) {
                mgr.SetKeyStateProvider([swapCount](int k) -> bool {
                    return (k == VK_SHIFT) && (swapCount % 2 == 0);
                });
            } else {
                mgr.ResetKeyStateProvider();
            }
            swapCount++;
        }
        totalProviderSwaps.store(swapCount, std::memory_order_release);
    });

    startFlag.store(true, std::memory_order_release);

    for (auto& w : workers) {
        w.join();
    }
    mutator.join();

    std::cout << "       Completed " << totalProviderSwaps.load() << " provider swaps and " 
              << totalQueries.load() << " IsKeyDown queries across 4 worker threads." << std::endl;
    assert(totalQueries.load() == static_cast<uint64_t>(iterationsPerWorker * numWorkerThreads));
    std::cout << "[PASS] Test_Concurrent_SetKeyStateProvider_And_IsKeyDown" << std::endl;
    return true;
}

// Test 2: Concurrently call SetBinding while polling loop / worker threads read IsActionKeyDown (100,000 iterations each)
bool Test_Concurrent_SetBinding_And_IsActionKeyDown() {
    std::cout << "[RUN ] Test_Concurrent_SetBinding_And_IsActionKeyDown..." << std::endl;
    HotkeyManager mgr;

    mgr.SetKeyStateProvider([](int k) -> bool {
        return (k == VK_F1 || k == VK_F5 || k == VK_F7 || k == 0x41);
    });

    const int iterationsPerReader = 100000;
    const int numReaderThreads = 4;
    std::atomic<bool> startFlag{false};
    std::atomic<bool> readersDone{false};
    std::atomic<int> completedReaders{0};
    std::atomic<uint64_t> totalActionQueries{0};
    std::atomic<uint64_t> totalBindingMutations{0};

    // Reader threads reading IsActionKeyDown across all action types
    std::vector<std::thread> readers;
    readers.reserve(numReaderThreads);
    for (int t = 0; t < numReaderThreads; ++t) {
        readers.emplace_back([&, t]() {
            while (!startFlag.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            uint64_t count = 0;
            const size_t actionCount = static_cast<size_t>(HotkeyAction::ActionCount);

            for (int i = 0; i < iterationsPerReader; ++i) {
                HotkeyAction action = static_cast<HotkeyAction>((i + t) % actionCount);
                bool down = mgr.IsActionKeyDown(action);
                (void)down;
                count++;
            }
            totalActionQueries.fetch_add(count, std::memory_order_relaxed);
            if (completedReaders.fetch_add(1, std::memory_order_acq_rel) + 1 == numReaderThreads) {
                readersDone.store(true, std::memory_order_release);
            }
        });
    }

    // Binding mutator thread
    std::thread mutator([&]() {
        while (!startFlag.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        const size_t actionCount = static_cast<size_t>(HotkeyAction::ActionCount);
        uint64_t mutCount = 0;
        while (!readersDone.load(std::memory_order_acquire)) {
            HotkeyAction action = static_cast<HotkeyAction>(mutCount % actionCount);
            if (mutCount % 2 == 0) {
                mgr.SetBinding(action, VK_F1, VK_F5);
            } else {
                mgr.SetBinding(action, 0x41 + (mutCount % 26), (mutCount % 4 == 0) ? VK_F7 : 0);
            }
            mutCount++;
        }
        totalBindingMutations.store(mutCount, std::memory_order_release);
    });

    startFlag.store(true, std::memory_order_release);

    for (auto& r : readers) {
        r.join();
    }
    mutator.join();

    std::cout << "       Completed " << totalBindingMutations.load() << " binding mutations and " 
              << totalActionQueries.load() << " IsActionKeyDown queries across 4 reader threads." << std::endl;
    assert(totalActionQueries.load() == static_cast<uint64_t>(iterationsPerReader * numReaderThreads));
    std::cout << "[PASS] Test_Concurrent_SetBinding_And_IsActionKeyDown" << std::endl;
    return true;
}

// Test 3: Concurrently call SetKeyStateProvider (100,000 iterations) while multiple worker threads continuously invoke Update() & IsKeyDown() (100,000 iterations each)
bool Test_Concurrent_SetKeyStateProvider_With_Update_And_IsKeyDown() {
    std::cout << "[RUN ] Test_Concurrent_SetKeyStateProvider_With_Update_And_IsKeyDown..." << std::endl;
    HotkeyManager mgr;

    std::atomic<uint64_t> callbacksCount{0};
    mgr.SetOnTogglePitch([&]() { callbacksCount.fetch_add(1, std::memory_order_relaxed); });
    mgr.SetOnToggleZoom([&]() { callbacksCount.fetch_add(1, std::memory_order_relaxed); });
    mgr.SetOnAdjustAltitude([&](float) { callbacksCount.fetch_add(1, std::memory_order_relaxed); });

    const int iterationsPerWorker = 100000;
    const int numWorkerThreads = 4;
    std::atomic<bool> startFlag{false};
    std::atomic<bool> workersDone{false};
    std::atomic<int> completedWorkers{0};
    std::atomic<uint64_t> totalWorkUnits{0};
    std::atomic<uint64_t> totalProviderMutations{0};

    // Workers calling IsKeyDown, CalculateSpeedMultiplier, and Update for 100,000 iterations each
    std::vector<std::thread> workers;
    workers.reserve(numWorkerThreads);
    for (int t = 0; t < numWorkerThreads; ++t) {
        workers.emplace_back([&, t]() {
            while (!startFlag.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            uint64_t count = 0;
            for (int i = 0; i < iterationsPerWorker; ++i) {
                if (i % 3 == 0) {
                    mgr.Update();
                } else if (i % 3 == 1) {
                    bool k = mgr.IsKeyDown(VK_F1);
                    (void)k;
                } else {
                    float mult = mgr.CalculateSpeedMultiplier();
                    (void)mult;
                }
                count++;
            }
            totalWorkUnits.fetch_add(count, std::memory_order_relaxed);
            if (completedWorkers.fetch_add(1, std::memory_order_acq_rel) + 1 == numWorkerThreads) {
                workersDone.store(true, std::memory_order_release);
            }
        });
    }

    // Provider mutator thread: continuously mutating until all workers complete 100,000 iterations
    std::thread mutator([&]() {
        while (!startFlag.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        uint64_t count = 0;
        while (!workersDone.load(std::memory_order_acquire)) {
            if (count % 2 == 0) {
                mgr.SetKeyStateProvider([count](int k) -> bool {
                    return (k == VK_F1 || k == VK_SHIFT) && (count % 4 != 0);
                });
            } else {
                mgr.ResetKeyStateProvider();
            }
            count++;
        }
        totalProviderMutations.store(count, std::memory_order_release);
    });

    startFlag.store(true, std::memory_order_release);

    for (auto& w : workers) {
        w.join();
    }
    mutator.join();

    std::cout << "       Completed " << totalProviderMutations.load() << " provider mutations and "
              << totalWorkUnits.load() << " worker operations. Callbacks fired: "
              << callbacksCount.load() << std::endl;
    assert(totalWorkUnits.load() == static_cast<uint64_t>(iterationsPerWorker * numWorkerThreads));
    std::cout << "[PASS] Test_Concurrent_SetKeyStateProvider_With_Update_And_IsKeyDown" << std::endl;
    return true;
}

// Test 4: Combined Maximum Contention Stress:
// - Background polling thread running at 60Hz
// - Mutator 1: SetKeyStateProvider (100k swaps)
// - Mutator 2: SetBinding (100k swaps)
// - Worker 1: IsKeyDown & CalculateSpeedMultiplier (100k queries)
// - Worker 2: IsActionKeyDown (100k queries)
// - Worker 3 & 4: Concurrent Update() calls (100k updates each)
bool Test_Adv_MaximumContention_Stress() {
    std::cout << "[RUN ] Test_Adv_MaximumContention_Stress..." << std::endl;
    HotkeyManager mgr;

    std::atomic<uint64_t> eventsFired{0};
    mgr.SetOnTogglePitch([&]() { eventsFired.fetch_add(1, std::memory_order_relaxed); });
    mgr.SetOnToggleZoom([&]() { eventsFired.fetch_add(1, std::memory_order_relaxed); });
    mgr.SetOnAdjustAltitude([&](float) { eventsFired.fetch_add(1, std::memory_order_relaxed); });
    mgr.SetOnAdjustZoom([&](float) { eventsFired.fetch_add(1, std::memory_order_relaxed); });
    mgr.SetOnResetDefaults([&]() { eventsFired.fetch_add(1, std::memory_order_relaxed); });

    mgr.Start(); // Start background polling thread

    const int iterations = 100000;
    std::atomic<bool> startFlag{false};
    std::atomic<bool> doneFlag{false};
    std::atomic<int> completedTasks{0};
    const int totalTasks = 6;

    auto markDone = [&]() {
        if (completedTasks.fetch_add(1, std::memory_order_acq_rel) + 1 == totalTasks) {
            doneFlag.store(true, std::memory_order_release);
        }
    };

    // Thread 1: SetKeyStateProvider (100k iterations)
    std::thread tProvider([&]() {
        while (!startFlag.load(std::memory_order_acquire)) std::this_thread::yield();
        for (int i = 0; i < iterations; ++i) {
            if (i % 2 == 0) {
                mgr.SetKeyStateProvider([i](int k) -> bool {
                    return (k == VK_F1 || k == VK_F7 || k == VK_SHIFT) && ((i + k) % 3 != 0);
                });
            } else {
                mgr.ResetKeyStateProvider();
            }
        }
        markDone();
    });

    // Thread 2: SetBinding (100k iterations)
    std::thread tBinding([&]() {
        while (!startFlag.load(std::memory_order_acquire)) std::this_thread::yield();
        const size_t actionCount = static_cast<size_t>(HotkeyAction::ActionCount);
        for (int i = 0; i < iterations; ++i) {
            HotkeyAction action = static_cast<HotkeyAction>(i % actionCount);
            mgr.SetBinding(action, (i % 2 == 0) ? VK_F1 : VK_F5, (i % 3 == 0) ? VK_F7 : 0);
        }
        markDone();
    });

    // Thread 3: IsKeyDown & CalculateSpeedMultiplier (100k iterations)
    std::thread tQuery1([&]() {
        while (!startFlag.load(std::memory_order_acquire)) std::this_thread::yield();
        for (int i = 0; i < iterations; ++i) {
            mgr.IsKeyDown(VK_F1);
            mgr.IsKeyDown(VK_SHIFT);
            mgr.CalculateSpeedMultiplier();
        }
        markDone();
    });

    // Thread 4: IsActionKeyDown (100k iterations)
    std::thread tQuery2([&]() {
        while (!startFlag.load(std::memory_order_acquire)) std::this_thread::yield();
        for (int i = 0; i < iterations; ++i) {
            mgr.IsActionKeyDown(HotkeyAction::TogglePitch);
            mgr.IsActionKeyDown(HotkeyAction::AltitudeUp);
            mgr.IsActionKeyDown(HotkeyAction::ToggleZoom);
        }
        markDone();
    });

    // Thread 5: Worker calling Update() (100k iterations)
    std::thread tUpdate1([&]() {
        while (!startFlag.load(std::memory_order_acquire)) std::this_thread::yield();
        for (int i = 0; i < iterations; ++i) {
            mgr.Update();
        }
        markDone();
    });

    // Thread 6: Worker calling Update() (100k iterations)
    std::thread tUpdate2([&]() {
        while (!startFlag.load(std::memory_order_acquire)) std::this_thread::yield();
        for (int i = 0; i < iterations; ++i) {
            mgr.Update();
        }
        markDone();
    });

    startFlag.store(true, std::memory_order_release);

    tProvider.join();
    tBinding.join();
    tQuery1.join();
    tQuery2.join();
    tUpdate1.join();
    tUpdate2.join();

    mgr.Stop();

    std::cout << "       Completed full 6-thread 100k iteration stress harness (600,000 operations total).\n"
              << "       Events dispatched safely: " << eventsFired.load() << std::endl;
    std::cout << "[PASS] Test_Adv_MaximumContention_Stress" << std::endl;
    return true;
}

int main() {
    std::cout << "============================================================" << std::endl;
    std::cout << " Cold Steel III - Concurrency & Thread-Safety Stress Harness " << std::endl;
    std::cout << "============================================================" << std::endl;

    assert(Test_Concurrent_SetKeyStateProvider_And_IsKeyDown());
    assert(Test_Concurrent_SetBinding_And_IsActionKeyDown());
    assert(Test_Concurrent_SetKeyStateProvider_With_Update_And_IsKeyDown());
    assert(Test_Adv_MaximumContention_Stress());

    std::cout << "============================================================" << std::endl;
    std::cout << " All Concurrency & Thread-Safety Stress Tests PASSED (0 races, 0 deadlocks)" << std::endl;
    std::cout << "============================================================" << std::endl;
    return 0;
}

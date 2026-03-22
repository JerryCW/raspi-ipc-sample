#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

#include "buffer/frame_buffer_pool.h"

using namespace sc;

// ============================================================
// Helper: create a FrameInfo with a given sequence number
// ============================================================
static FrameInfo make_info(uint64_t seq, uint32_t w = 640, uint32_t h = 480) {
    FrameInfo fi{};
    fi.width = w;
    fi.height = h;
    fi.stride = w * 3;
    fi.timestamp = std::chrono::steady_clock::now();
    fi.sequence_number = seq;
    return fi;
}

// Helper: create dummy pixel data
static std::vector<uint8_t> make_data(size_t size = 128) {
    return std::vector<uint8_t>(size, 0xAB);
}

// ============================================================
// FrameBuffer — basic construction and accessors
// ============================================================

TEST(FrameBufferTest, ConstructAndAccess) {
    auto data = make_data(256);
    auto info = make_info(1);

    FrameBuffer fb(data, info);

    EXPECT_EQ(fb.size(), 256u);
    EXPECT_EQ(fb.info().sequence_number, 1u);
    EXPECT_EQ(fb.info().width, 640u);
    EXPECT_FALSE(fb.empty());
    EXPECT_EQ(fb.data()[0], 0xAB);
}

TEST(FrameBufferTest, DefaultIsEmpty) {
    FrameBuffer fb;
    EXPECT_TRUE(fb.empty());
    EXPECT_EQ(fb.size(), 0u);
}

// ============================================================
// FrameBuffer — move semantics
// ============================================================

TEST(FrameBufferTest, MoveConstruct) {
    FrameBuffer fb1(make_data(100), make_info(42));
    EXPECT_EQ(fb1.size(), 100u);

    FrameBuffer fb2(std::move(fb1));

    // fb2 should have the data
    EXPECT_EQ(fb2.size(), 100u);
    EXPECT_EQ(fb2.info().sequence_number, 42u);
    EXPECT_FALSE(fb2.empty());

    // fb1 should be moved-from (empty)
    EXPECT_TRUE(fb1.empty());
}

TEST(FrameBufferTest, MoveAssign) {
    FrameBuffer fb1(make_data(200), make_info(7));
    FrameBuffer fb2;

    fb2 = std::move(fb1);

    EXPECT_EQ(fb2.size(), 200u);
    EXPECT_EQ(fb2.info().sequence_number, 7u);
    EXPECT_TRUE(fb1.empty());
}

TEST(FrameBufferTest, Reset) {
    FrameBuffer fb;
    EXPECT_TRUE(fb.empty());

    fb.reset(make_data(64), make_info(99));
    EXPECT_FALSE(fb.empty());
    EXPECT_EQ(fb.size(), 64u);
    EXPECT_EQ(fb.info().sequence_number, 99u);
}

// ============================================================
// FrameBufferPool — initialization
// ============================================================

TEST(FrameBufferPoolTest, InitializeSuccess) {
    FrameBufferPool pool;
    auto result = pool.initialize(10, 640, 480);
    EXPECT_TRUE(result.is_ok());
    EXPECT_EQ(pool.total_count(), 10u);
    EXPECT_EQ(pool.available_count(), 10u);
    EXPECT_EQ(pool.dropped_count(), 0u);
}

TEST(FrameBufferPoolTest, InitializeZeroPoolSize) {
    FrameBufferPool pool;
    auto result = pool.initialize(0, 640, 480);
    EXPECT_TRUE(result.is_err());
    EXPECT_EQ(result.error().code, ErrorCode::InvalidArgument);
}

TEST(FrameBufferPoolTest, InitializeZeroDimensions) {
    FrameBufferPool pool;
    auto r1 = pool.initialize(10, 0, 480);
    EXPECT_TRUE(r1.is_err());

    FrameBufferPool pool2;
    auto r2 = pool2.initialize(10, 640, 0);
    EXPECT_TRUE(r2.is_err());
}

TEST(FrameBufferPoolTest, DoubleInitializeFails) {
    FrameBufferPool pool;
    EXPECT_TRUE(pool.initialize(5, 640, 480).is_ok());
    auto result = pool.initialize(5, 640, 480);
    EXPECT_TRUE(result.is_err());
}

// ============================================================
// FrameBufferPool — acquire and return
// ============================================================

TEST(FrameBufferPoolTest, AcquireAndReturn) {
    FrameBufferPool pool;
    pool.initialize(3, 640, 480);

    EXPECT_EQ(pool.available_count(), 3u);

    {
        auto r1 = pool.acquire();
        ASSERT_TRUE(r1.is_ok());
        EXPECT_EQ(pool.available_count(), 2u);

        auto r2 = pool.acquire();
        ASSERT_TRUE(r2.is_ok());
        EXPECT_EQ(pool.available_count(), 1u);
    }
    // shared_ptrs destroyed → buffers returned
    EXPECT_EQ(pool.available_count(), 3u);
}

TEST(FrameBufferPoolTest, AcquireNotInitialized) {
    FrameBufferPool pool;
    auto result = pool.acquire();
    EXPECT_TRUE(result.is_err());
    EXPECT_EQ(result.error().code, ErrorCode::InvalidArgument);
}

// ============================================================
// FrameBufferPool — submit and consume
// ============================================================

TEST(FrameBufferPoolTest, SubmitAndConsume) {
    FrameBufferPool pool;
    pool.initialize(5, 640, 480);

    auto acq = pool.acquire();
    ASSERT_TRUE(acq.is_ok());
    auto buf = std::move(acq).value();
    buf->reset(make_data(100), make_info(1));

    auto submit_result = pool.submit(buf);
    EXPECT_TRUE(submit_result.is_ok());

    auto consume = pool.consume_latest();
    ASSERT_TRUE(consume.is_ok());
    EXPECT_EQ(consume.value()->info().sequence_number, 1u);
}

TEST(FrameBufferPoolTest, ConsumeLatestReturnsNewest) {
    FrameBufferPool pool;
    pool.initialize(5, 640, 480);

    // Submit 3 frames
    for (uint64_t i = 1; i <= 3; ++i) {
        auto acq = pool.acquire();
        ASSERT_TRUE(acq.is_ok());
        auto buf = std::move(acq).value();
        buf->reset(make_data(64), make_info(i));
        pool.submit(buf);
    }

    // consume_latest should return frame #3 and drop #1, #2
    auto consume = pool.consume_latest();
    ASSERT_TRUE(consume.is_ok());
    EXPECT_EQ(consume.value()->info().sequence_number, 3u);

    // Older frames were dropped
    EXPECT_GE(pool.dropped_count(), 2u);
}

TEST(FrameBufferPoolTest, ConsumeEmptyFails) {
    FrameBufferPool pool;
    pool.initialize(5, 640, 480);

    auto result = pool.consume_latest();
    EXPECT_TRUE(result.is_err());
    EXPECT_EQ(result.error().code, ErrorCode::NotFound);
}

TEST(FrameBufferPoolTest, SubmitNullFails) {
    FrameBufferPool pool;
    pool.initialize(5, 640, 480);

    auto result = pool.submit(nullptr);
    EXPECT_TRUE(result.is_err());
    EXPECT_EQ(result.error().code, ErrorCode::InvalidArgument);
}

// ============================================================
// FrameBufferPool — pool exhaustion drops oldest
// ============================================================

TEST(FrameBufferPoolTest, PoolExhaustedDropsOldest) {
    FrameBufferPool pool;
    pool.initialize(3, 640, 480);

    // Acquire all 3 buffers and submit them as pending frames
    std::vector<std::shared_ptr<FrameBuffer>> frames;
    for (uint64_t i = 1; i <= 3; ++i) {
        auto acq = pool.acquire();
        ASSERT_TRUE(acq.is_ok());
        auto buf = std::move(acq).value();
        buf->reset(make_data(32), make_info(i));
        pool.submit(buf);
        // Don't hold onto the shared_ptr beyond submit
    }

    EXPECT_EQ(pool.available_count(), 0u);

    // Now try to acquire — should drop oldest pending frame
    auto acq = pool.acquire();
    ASSERT_TRUE(acq.is_ok()) << "Should reclaim buffer by dropping oldest frame";
    EXPECT_GE(pool.dropped_count(), 1u);
}

// ============================================================
// FrameBufferPool — multi-threaded safety
// ============================================================

TEST(FrameBufferPoolTest, ConcurrentAcquireReturn) {
    FrameBufferPool pool;
    pool.initialize(30, 640, 480);

    constexpr int kThreads = 8;
    constexpr int kIterations = 100;
    std::atomic<int> success_count{0};

    auto worker = [&]() {
        for (int i = 0; i < kIterations; ++i) {
            auto acq = pool.acquire();
            if (acq.is_ok()) {
                auto buf = std::move(acq).value();
                buf->reset(make_data(16), make_info(static_cast<uint64_t>(i)));
                success_count.fetch_add(1, std::memory_order_relaxed);
                // buf goes out of scope → returned to pool
            }
            // Small yield to increase interleaving
            std::this_thread::yield();
        }
    };

    std::vector<std::thread> threads;
    threads.reserve(kThreads);
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back(worker);
    }
    for (auto& t : threads) {
        t.join();
    }

    // All buffers should be returned
    EXPECT_EQ(pool.available_count(), 30u);
    EXPECT_GT(success_count.load(), 0);
}

TEST(FrameBufferPoolTest, ConcurrentProducerConsumer) {
    FrameBufferPool pool;
    pool.initialize(10, 640, 480);

    constexpr int kFrames = 50;
    std::atomic<int> produced{0};
    std::atomic<int> consumed{0};
    std::atomic<bool> done{false};

    // Producer thread
    auto producer = [&]() {
        for (int i = 0; i < kFrames; ++i) {
            auto acq = pool.acquire();
            if (acq.is_ok()) {
                auto buf = std::move(acq).value();
                buf->reset(make_data(32), make_info(static_cast<uint64_t>(i)));
                pool.submit(buf);
                produced.fetch_add(1, std::memory_order_relaxed);
            }
            std::this_thread::yield();
        }
        done.store(true, std::memory_order_release);
    };

    // Consumer thread — runs until producer is done, then drains remaining
    auto consumer = [&]() {
        while (!done.load(std::memory_order_acquire)) {
            auto result = pool.consume_latest();
            if (result.is_ok()) {
                consumed.fetch_add(1, std::memory_order_relaxed);
            }
            std::this_thread::yield();
        }
        // Drain any remaining pending frames
        for (int i = 0; i < 10; ++i) {
            auto result = pool.consume_latest();
            if (result.is_ok()) {
                consumed.fetch_add(1, std::memory_order_relaxed);
            }
        }
    };

    std::thread prod_thread(producer);
    std::thread cons_thread(consumer);

    prod_thread.join();
    cons_thread.join();

    // At least some frames should have been consumed
    EXPECT_GT(consumed.load(), 0);
    // Consumed cannot exceed produced
    EXPECT_LE(consumed.load(), produced.load());
}

TEST(FrameBufferPoolTest, MultiProducerMultiConsumer) {
    FrameBufferPool pool;
    pool.initialize(20, 640, 480);

    constexpr int kProducers = 4;
    constexpr int kConsumers = 4;
    constexpr int kFramesPerProducer = 30;
    std::atomic<int> total_produced{0};
    std::atomic<int> total_consumed{0};
    std::atomic<bool> producers_done{false};

    auto producer = [&](int id) {
        for (int i = 0; i < kFramesPerProducer; ++i) {
            auto acq = pool.acquire();
            if (acq.is_ok()) {
                auto buf = std::move(acq).value();
                uint64_t seq = static_cast<uint64_t>(id * kFramesPerProducer + i);
                buf->reset(make_data(16), make_info(seq));
                pool.submit(buf);
                total_produced.fetch_add(1, std::memory_order_relaxed);
            }
            std::this_thread::yield();
        }
    };

    auto consumer = [&]() {
        while (!producers_done.load(std::memory_order_acquire)) {
            auto result = pool.consume_latest();
            if (result.is_ok()) {
                // Verify frame data is accessible (no data race / corruption)
                auto& frame = result.value();
                EXPECT_NE(frame->data(), nullptr);
                EXPECT_GT(frame->size(), 0u);
                total_consumed.fetch_add(1, std::memory_order_relaxed);
            }
            std::this_thread::yield();
        }
        // Drain remaining
        for (int i = 0; i < 20; ++i) {
            auto result = pool.consume_latest();
            if (result.is_ok()) {
                auto& frame = result.value();
                EXPECT_NE(frame->data(), nullptr);
                EXPECT_GT(frame->size(), 0u);
                total_consumed.fetch_add(1, std::memory_order_relaxed);
            }
        }
    };

    std::vector<std::thread> threads;
    for (int i = 0; i < kProducers; ++i) {
        threads.emplace_back(producer, i);
    }
    for (int i = 0; i < kConsumers; ++i) {
        threads.emplace_back(consumer);
    }

    // Wait for producers
    for (int i = 0; i < kProducers; ++i) {
        threads[i].join();
    }
    producers_done.store(true, std::memory_order_release);

    // Wait for consumers
    for (int i = kProducers; i < static_cast<int>(threads.size()); ++i) {
        threads[i].join();
    }

    EXPECT_GT(total_produced.load(), 0);
    EXPECT_GT(total_consumed.load(), 0);
}

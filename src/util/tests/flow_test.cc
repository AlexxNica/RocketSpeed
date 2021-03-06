//  Copyright (c) 2014, Facebook, Inc.  All rights reserved.
//  This source code is licensed under the BSD-style license found in the
//  LICENSE file in the root directory of this source tree. An additional grant
//  of patent rights can be found in the PATENTS file in the same directory.
//
#include <memory>
#include <random>
#include <string>
#include "src/util/testharness.h"
#include "src/messages/flow_control.h"
#include "src/util/common/observable_set.h"
#include "src/util/common/processor.h"
#include "src/messages/msg_loop.h"
#include "src/messages/observable_map.h"
#include "src/messages/queues.h"
#include "src/util/common/rate_limiter_sink.h"
#include "src/util/common/retry_later_sink.h"

namespace rocketspeed {

class FlowTest : public ::testing::Test {
 public:
  FlowTest() {
    env_ = Env::Default();
    EXPECT_OK(test::CreateLogger(env_, "FlowTest", &info_log_));
  }

  template <typename T>
  std::shared_ptr<SPSCQueue<T>> MakeQueue(size_t size) {
    return std::make_shared<SPSCQueue<T>>(
      info_log_,
      std::make_shared<QueueStats>("queue"),
      size,
      "test_queue");
  }

  std::shared_ptr<SPSCQueue<int>> MakeIntQueue(size_t size) {
    return MakeQueue<int>(size);
  }

  Env* env_;
  EnvOptions env_options_;
  std::shared_ptr<Logger> info_log_;
};

TEST_F(FlowTest, PartitionedFlow) {
  // Setup:
  //                    overflow
  //                  P0   |     P1
  //                 +--+  v   +-----+
  //   +------+=10k=>|  |=100=>|sleep|=> counter0++
  //   | 10k  |      +--+      +-----+
  //   | msgs |      +--+      +-----+
  //   +------+=10k=>|  |=10k=>|     |=> counter1++
  //                 +--+      +-----+
  //                  P2         P3
  //
  // This thread fills queues into P0 and P2 with messages.
  // P0 and P2 forward messages to P1 and P3 respectively, with backoff.
  // P1 sleeps on each message (so incoming queue will overflow).
  // Check that all messages are processed.

  enum : int { kNumMessages = 10000 };
  enum : int { kSmallQueue = 100 };
  int sleep_micros = 100;
  MsgLoop loop(env_, env_options_, 0, 4, info_log_, "flow");
  ASSERT_OK(loop.Initialize());
  EventLoop* event_loop[4];
  for (int i = 0; i < 4; ++i) {
    event_loop[i] = loop.GetEventLoop(i);
  }

  // Create all our queues.
  auto queue0 = MakeIntQueue(kNumMessages);
  auto queue2 = MakeIntQueue(kNumMessages);
  auto queue01 = MakeIntQueue(kSmallQueue);
  auto queue23 = MakeIntQueue(kNumMessages);

  // Register queue read event handlers.
  InstallSource<int>(
    event_loop[0],
    queue0.get(),
    [&] (Flow* flow, int x) {
      flow->Write(queue01.get(), x);
    });

  port::Semaphore sem1;
  InstallSource<int>(
    event_loop[1],
    queue01.get(),
    [&] (Flow*, int) {
      env_->SleepForMicroseconds(sleep_micros);
      sem1.Post();
    });

  InstallSource<int>(
    event_loop[2],
    queue2.get(),
    [&] (Flow* flow, int x) {
      ASSERT_TRUE(flow->Write(queue23.get(), x));
    });

  port::Semaphore sem3;
  InstallSource<int>(
    event_loop[3],
    queue23.get(),
    [&] (Flow*, int) {
      sem3.Post();
    });

  MsgLoopThread flow_threads(env_, &loop, "flow");

  for (int i = 0; i < kNumMessages; ++i) {
    // Queue is big enough for all these writes, all writes should succeed.
    int x = i, y = i;
    ASSERT_TRUE(queue0->Write(x));
    ASSERT_TRUE(queue2->Write(y));
  }

  // Should complete nearly immediately.
  uint64_t start = env_->NowMicros();
  for (int i = 0; i < kNumMessages; ++i) {
    ASSERT_TRUE(sem3.TimedWait(std::chrono::milliseconds(100)));
  }

  // Sleeping pipeline should take longer.
  // At least the sum of sleep_micros for all messages.
  int expected = kNumMessages * sleep_micros;
  for (int i = 0; i < kNumMessages; ++i) {
    ASSERT_TRUE(sem1.TimedWait(std::chrono::milliseconds(100)));
  }

  // Check that everything took roughly the expected amount of time.
  uint64_t taken = env_->NowMicros() - start;
  ASSERT_GT(taken, expected);
  ASSERT_LT(taken, expected * 2.0);
}

TEST_F(FlowTest, Fanout) {
  // Setup:
  //                    overflow
  //                  P0   |     P1
  //                 +--+  v   +-----+
  //   +------+=10k=>|  |=100=>|sleep|=> counter0++
  //   | 10k  |      |  |  |   +-----+
  //   | msgs |      |  |  V   +-----+
  //   +------+=10k=>|  |=100=>|sleep|=> counter1++
  //                 +--+      +-----+
  //                             P2
  //
  // This thread fills queues into P0 with messages.
  // P0 forward messages to P1 and P3, with backoff.
  // P1 and P2 sleeps on each message to cause overflow.
  // Check that all messages are processed.

  enum : int { kNumMessages = 10000 };
  enum : int { kSmallQueue = 100 };
  int sleep_micros = 100;
  MsgLoop loop(env_, env_options_, 0, 3, info_log_, "flow");
  ASSERT_OK(loop.Initialize());
  EventLoop* event_loop[3];
  for (int i = 0; i < 3; ++i) {
    event_loop[i] = loop.GetEventLoop(i);
  }

  // Create all our queues.
  auto queue0 = MakeIntQueue(kNumMessages);
  auto queue01 = MakeIntQueue(kSmallQueue);
  auto queue02 = MakeIntQueue(kSmallQueue);

  // Register queue read event handlers.
  InstallSource<int>(
    event_loop[0],
    queue0.get(),
    [&] (Flow* flow, int x) {
      // Fanout to P1 and P2
      flow->Write(queue01.get(), x);
      flow->Write(queue02.get(), x);
    });

  port::Semaphore sem1;
  InstallSource<int>(
    event_loop[1],
    queue01.get(),
    [&] (Flow*, int) {
      env_->SleepForMicroseconds(sleep_micros);
      sem1.Post();
    });

  port::Semaphore sem2;
  InstallSource<int>(
    event_loop[2],
    queue02.get(),
    [&] (Flow* flow, int x) {
      env_->SleepForMicroseconds(sleep_micros);
      sem2.Post();
    });

  MsgLoopThread flow_threads(env_, &loop, "flow");

  for (int i = 0; i < kNumMessages; ++i) {
    // Queue is big enough for all these writes, all writes should succeed.
    int x = i;
    ASSERT_TRUE(queue0->Write(x));
  }

  // Sleeping pipeline should take some time.
  // At least the sum of sleep_micros for all messages.
  uint64_t start = env_->NowMicros();
  int expected = kNumMessages * sleep_micros;
  for (int i = 0; i < kNumMessages; ++i) {
    ASSERT_TRUE(sem1.TimedWait(std::chrono::milliseconds(100)));
    ASSERT_TRUE(sem2.TimedWait(std::chrono::milliseconds(100)));
  }

  // Check that everything took roughly the expected amount of time.
  uint64_t taken = env_->NowMicros() - start;
  ASSERT_GT(taken, expected);
  ASSERT_LT(taken, expected * 2.0);
}

TEST_F(FlowTest, MultiLayerRandomized) {
  // Setup:
  // Many layers of processors, with each processor in each layer forwarding to
  // all processors in next layer. Inter-layer queues will be very small to
  // ensure overflow.

  enum : int { kNumMessages = 100000 };
  enum : int { kSmallQueue = 10 };
  enum : int { kLayers = 10 };
  enum : int { kPerLayer = 5 };
  enum : int { kNumProcessors = kLayers * kPerLayer };
  MsgLoop loop(env_, env_options_, 0, kNumProcessors, info_log_, "flow");
  ASSERT_OK(loop.Initialize());

  // Create all our queues.
  // queue[i][j][k] is to processor j in the ith layer, from processor k in
  // layer (i - 1).
  std::shared_ptr<SPSCQueue<int>> queue[kLayers][kPerLayer][kPerLayer];
  for (int i = 1; i < kLayers; ++i) {
    for (int j = 0; j < kPerLayer; ++j) {
      for (int k = 0; k < kPerLayer; ++k) {
        queue[i][j][k] = MakeIntQueue(kSmallQueue);
      }
    }
  }
  // Queues into top layer processors.
  std::shared_ptr<SPSCQueue<int>> input[kPerLayer];
  for (int i = 0; i < kPerLayer; ++i) {
    input[i] = MakeIntQueue(kNumMessages);
  }

  // Register queue read event handlers.
  port::Semaphore sem;
  for (int i = 1; i < kLayers; ++i) {
    for (int j = 0; j < kPerLayer; ++j) {
      for (int k = 0; k < kPerLayer; ++k) {
        InstallSource<int>(
          loop.GetEventLoop(i * kPerLayer + j),
          queue[i][j][k].get(),
          [&, i, j] (Flow* flow, int x) {
            if (i == kLayers - 1) {
              sem.Post();
            } else {
              // Route to a processor in next layer based on value.
              int p = x % kPerLayer;
              x /= kPerLayer;
              flow->Write(queue[i + 1][p][j].get(), x);
            }
          });
      }
    }
  }
  for (int i = 0; i < kPerLayer; ++i) {
    InstallSource<int>(
      loop.GetEventLoop(i),
      input[i].get(),
      [&, i] (Flow* flow, int x) {
        // Route to a processor in next layer based on value.
        int p = x % kPerLayer;
        x /= kPerLayer;
        flow->Write(queue[1][p][i].get(), x);
      });
  }

  MsgLoopThread flow_threads(env_, &loop, "flow");

  std::mt19937 rng;
  int routing_max = 1;
  for (int i = 0; i < kLayers; ++i) {
    routing_max *= kPerLayer;
  }
  std::uniform_int_distribution<int> dist(0, routing_max);
  for (int i = 0; i < kNumMessages; ++i) {
    // Queue is big enough for all these writes, all writes should succeed.
    int x = dist(rng);
    ASSERT_TRUE(input[i % kPerLayer]->Write(x));
  }

  // Sleeping pipeline should take some time.
  // At least the sum of sleep_micros for all messages.
  for (int i = 0; i < kNumMessages; ++i) {
    ASSERT_TRUE(sem.TimedWait(std::chrono::milliseconds(1000)));
  }
  // No more.
  ASSERT_TRUE(!sem.TimedWait(std::chrono::milliseconds(1000)));
}

TEST_F(FlowTest, ObservableMap) {
  // Setup:
  //
  //   +----------+    +---------+    +--------+
  //   | 10k msgs |===>| obs map |=1=>| reader |
  //   +----------+    +---------+    +--------+


  enum : int { kNumMessages = 10000 };
  int sleep_micros = 100;
  MsgLoop loop(env_, env_options_, 0, 1, info_log_, "flow");
  ASSERT_OK(loop.Initialize());

  auto obs_map = std::make_shared<ObservableMap<std::string, int>>();
  auto queue = MakeQueue<std::pair<std::string, int>>(1);

  port::Semaphore done;
  int reads = 0;
  int last_a = -1;
  int last_b = -1;
  InstallSource<std::pair<std::string, int>>(
    loop.GetEventLoop(0),
    obs_map.get(),
    [&] (Flow* flow, std::pair<std::string, int> kv) {
      flow->Write(queue.get(), kv);
    });

  InstallSource<std::pair<std::string, int>>(
    loop.GetEventLoop(0),
    queue.get(),
    [&] (Flow* flow, std::pair<std::string, int> kv) {
      auto key = kv.first;
      auto value = kv.second;
      int* last = key == "a" ? &last_a : &last_b;
      ASSERT_GT(value, *last);  // always increasing
      *last = value;
      ++reads;
      if (last_a == kNumMessages - 1 && last_b == kNumMessages - 1) {
        done.Post();
      }
      env_->SleepForMicroseconds(sleep_micros);
    });

  MsgLoopThread flow_threads(env_, &loop, "flow");
  for (int i = 0; i < kNumMessages; ++i) {
    std::unique_ptr<Command> cmd(
      MakeExecuteCommand([&, i] () {
        obs_map->Write("a", i);
        obs_map->Write("b", i);
      }));
    loop.SendCommand(std::move(cmd), 0);
  }

  ASSERT_TRUE(done.TimedWait(std::chrono::seconds(5)));
  ASSERT_LT(reads, kNumMessages * 2);  // ensure some were merged
  ASSERT_EQ(last_a, kNumMessages - 1);  // ensure all written
  ASSERT_EQ(last_b, kNumMessages - 1);  // ensure all written
}

TEST_F(FlowTest, ObservableSet) {
  // This test checks that ObservableSet correctly executes subscriptions,
  // and tolerant to modifications from within the callback

  MsgLoop loop(env_, env_options_, 0, 1, info_log_, "flow");
  ASSERT_OK(loop.Initialize());

  auto obs_set =
      std::make_shared<ObservableSet<std::string>>(loop.GetEventLoop(0));

  std::map<std::string, int> processed;
  port::Semaphore done;

  int done_after = 0;
  InstallSource<std::string>(
    loop.GetEventLoop(0),
    obs_set.get(),
    [&] (Flow* flow, std::string key) {
      ++processed[key];
      ASSERT_GT(processed[key], 0);
      ASSERT_GT(done_after, 0);
      --done_after;
      if (!done_after) {
        done.Post();
        obs_set->Clear();
        return;
      }

      if (key[0] == 'e') {
        // Those two will be merged, with zero effect
        obs_set->Add("bad explosion");
        obs_set->Remove("bad explosion");

        obs_set->Add("explode again");
      }
    });

  MsgLoopThread flow_threads(env_, &loop, "flow");

  auto send_exec_command = [&loop](std::function<void()> func) {
    std::unique_ptr<Command> cmd(MakeExecuteCommand(func));
    loop.SendCommand(std::move(cmd), 0);
  };

  done_after = 3;
  send_exec_command([&](){ obs_set->Add("a"); });
  send_exec_command([&](){ obs_set->Add("b"); });
  send_exec_command([&](){ obs_set->Add("c"); });
  ASSERT_TRUE(done.TimedWait(std::chrono::seconds(5)));
  ASSERT_EQ(done_after, 0);
  ASSERT_EQ(processed.size(), 3);
  ASSERT_EQ(processed["a"], 1);
  ASSERT_EQ(processed["b"], 1);
  ASSERT_EQ(processed["c"], 1);
  processed.clear();

  enum : int { kNumKeys = 1079 };
  done_after = kNumKeys;
  send_exec_command([&](){ obs_set->Add("explode"); });
  ASSERT_TRUE(done.TimedWait(std::chrono::seconds(5)));
  ASSERT_EQ(done_after, 0);
  ASSERT_EQ(processed.size(), 2);
  ASSERT_EQ(processed["explode"], 1);
  ASSERT_EQ(processed["explode again"], kNumKeys - 1);

  send_exec_command([&]() { obs_set.reset(); });
}

TEST_F(FlowTest, SourcelessFlow) {
  // This tests that when one uses SourcelessFlow to write to a Sink and the
  // Sink overflows, the messages still get flushed once the Sink becomes
  // writable again.

  enum : int { kNumMessages = 10000 };
  MsgLoop loop(env_, env_options_, 0, 1, info_log_, "flow");
  ASSERT_OK(loop.Initialize());
  MsgLoopThread flow_threads(env_, &loop, "flow");

  FlowControl* flow_control = loop.GetEventLoop(0)->GetFlowControl();
  auto queue = MakeQueue<int>(kNumMessages / 2);
  port::Semaphore done;
  int read = 0;
  InstallSource<int>(
    loop.GetEventLoop(0),
    queue.get(),
    [&](Flow* flow, int v) {
      if (++read == kNumMessages) {
        done.Post();
      }
    });

  std::unique_ptr<Command> cmd(MakeExecuteCommand([&]() {
    SourcelessFlow no_flow(flow_control);
    for (int i = 0; i < kNumMessages; ++i) {
      no_flow.Write(queue.get(), i);
    }
  }));
  loop.SendCommand(std::move(cmd), 0);

  ASSERT_TRUE(done.TimedWait(std::chrono::seconds(5)));
}

class RateLimiterSinkFlowTest : public FlowTest {
 public:
  RateLimiterSinkFlowTest() {}
  void TestImpl(
    int num_messages, int rate_limit, int reader_size,
    int rate_duration, int reader_sleep_time);
};

void RateLimiterSinkFlowTest::TestImpl(
  int num_messages, int rate_limit, int reader_size,
  int rate_duration, int reader_sleep_time) {
  // Setup:
  //               ____________________________________
  //              |                                    |
  //   +--------+ |  +--------------+    +-----------+ |
  //   | N msgs |=|=>| RateLim(M/S) |=P=>| reader(ST)| |
  //   +--------+ |  +--------------+    +-----------+ |
  //              |____________________________________|
  //
  //  N: num_messages
  //  M / S: rate_limit / rate_duration
  //  P: reader_size
  //  ST: reader_sleep_time
  //
  // The RateLimiter tries to write to the queue at M writes per S microseconds
  // If the limit exceeds or the queue gets full, it backsoff for both of
  // rate limiter and underlying queue to be ready for write.

  int kNumMessages = num_messages;
  int kRateLimit = rate_limit;
  int kReaderSize = reader_size;
  std::chrono::microseconds kRateDuration(rate_duration);
  int sleep_micros = reader_sleep_time;

  std::vector<bool> values(kNumMessages, false);

  MsgLoop loop(env_, env_options_, 0, 2, info_log_, "flow");
  ASSERT_OK(loop.Initialize());

  auto queue0 = MakeIntQueue(kNumMessages);
  auto queue1 = MakeIntQueue(kReaderSize);
  auto rate_limiter_sink = std::make_shared<RateLimiterSink<int>>(
    kRateLimit, kRateDuration, queue1.get());

  InstallSource<int>(
    loop.GetEventLoop(0),
    queue0.get(),
    [&] (Flow* flow, int x) {
      flow->Write(rate_limiter_sink.get(), x);
    });

  port::Semaphore sem1;
  InstallSource<int>(
    loop.GetEventLoop(1),
    queue1.get(),
    [&] (Flow*, int x) {
      if (sleep_micros) {
        env_->SleepForMicroseconds(sleep_micros);
      }
      values[x] = true;
      sem1.Post();
    });

  MsgLoopThread flow_threads(env_, &loop, "flow");

  uint64_t start = env_->NowMicros();
  for (int i = 0; i < kNumMessages; ++i) {
    int x = i;
    ASSERT_TRUE(queue0->Write(x));
  }

  for (int i = 0; i < kNumMessages; ++i) {
    sem1.Wait();
  }

  for (int i = 0; i < kNumMessages; ++i) {
    ASSERT_TRUE(values[i]);
  }

  uint64_t taken = env_->NowMicros() - start;

  int expected = sleep_micros * kNumMessages;
  expected = std::max(expected, (kNumMessages / kReaderSize) * sleep_micros);
  expected = std::max(expected, (kNumMessages / kRateLimit) * sleep_micros);
  expected = std::max(expected,
    (kNumMessages / kRateLimit) * (int)kRateDuration.count());
  expected = std::max(expected,
    (kNumMessages / kReaderSize) * (int)kRateDuration.count()
  );

  ASSERT_GT(taken, expected * 0.8);
  ASSERT_LT(taken, expected * 1.4);
}

TEST_F(RateLimiterSinkFlowTest, Test_1) {
  TestImpl(500, 2, 1, 1000, 2000);
}

TEST_F(RateLimiterSinkFlowTest, Test_2) {
  TestImpl(500, 1, 2, 1000, 2000);
}

TEST_F(RateLimiterSinkFlowTest, Test_3) {
  TestImpl(500, 1, 1, 1000, 2000);
}

TEST_F(RateLimiterSinkFlowTest, Test_4) {
  TestImpl(5000, 100, 100, 1000, 1000);
}

TEST_F(RateLimiterSinkFlowTest, Test_5) {
  TestImpl(5000, 1000, 5000, 10000, 0);
}

TEST_F(RateLimiterSinkFlowTest, Test_6) {
  TestImpl(5000, 1000, 1000, 10000, 0);
}

TEST_F(FlowTest, RetryLaterSink) {
  // Tests that backoff times specified by a RetryLaterSink are fulfilled.
  // We first write messages to a queue, which is read by an EventLoop and fed
  // into the RetryLaterSink.

  // The time to back off for on each consecutive read.
  std::vector<int> backoffs = {
    0, 100, 200, 0, 0, 200, 200, 200, 0, 0
  };
  auto num_messages = std::count(backoffs.begin(), backoffs.end(), 0);
  int total_ms = std::accumulate(backoffs.begin(), backoffs.end(), 0);

  MsgLoop loop(env_, env_options_, 0, 2, info_log_, "flow");
  ASSERT_OK(loop.Initialize());
  EventLoop* event_loop[2];
  for (int i = 0; i < 2; ++i) {
    event_loop[i] = loop.GetEventLoop(i);
  }

  // Create our queue.
  auto queue = MakeIntQueue(num_messages);

  // Create retry later sink.
  int expected = 0;
  size_t hits = 0;
  auto last_time = std::chrono::steady_clock::now();
  std::chrono::milliseconds expected_delay(0);
  port::Semaphore done;
  RetryLaterSink<int> sink([&] (int& x) {
    EXPECT_EQ(x, expected);
    EXPECT_LT(hits, backoffs.size());
    EXPECT_GE(std::chrono::steady_clock::now() - last_time, expected_delay);
    auto backoff = backoffs[hits++];
    if (backoff == 0) {
      expected++;
      if (expected == num_messages) {
        done.Post();
      }
    }
    expected_delay = std::chrono::milliseconds(backoff);
    last_time = std::chrono::steady_clock::now();
    if (backoff) {
      return BackPressure::RetryAfter(expected_delay);
    } else {
      return BackPressure::None();
    }
  });

  // Register queue read event handlers.
  InstallSource<int>(
    event_loop[0],
    queue.get(),
    [&] (Flow* flow, int x) {
      flow->Write(&sink, x);
    });

  MsgLoopThread flow_threads(env_, &loop, "flow");
  for (int i = 0; i < num_messages; ++i) {
    // Queue is big enough for all these writes, all writes should succeed.
    int x = i;
    ASSERT_TRUE(queue->Write(x));
  }

  ASSERT_TRUE(done.TimedWait(std::chrono::milliseconds(2 * total_ms)));
}

TEST_F(FlowTest, BackpressureStatsAndLogging) {
  // Check that the backpressure_lifted statistics is correctly updated when
  // a source is unregistered.
  // To test, we setup a source queue, which writes into a sink queue until it
  // becomes blocked. Nothing is reading the sink queue. We then unregisterd
  // the source queue and check that the statistic updates.

  // Also check that flow control logs a warning when a source is blocked for
  // too long.
  port::Semaphore block_msg;
  auto checker = [&] (std::string msg) {
    if (msg.find("source 'test_queue' blocked") != std::string::npos) {
      block_msg.Post();
    }
  };
  std::shared_ptr<Logger> test_log =
    std::make_shared<test::TestLogger>(checker);

  MsgLoop::Options opts;
  opts.event_loop.flow_control_blocked_warn_period = std::chrono::seconds(1);

  MsgLoop loop(env_, env_options_, 0, 1, test_log, "flow", opts);
  ASSERT_OK(loop.Initialize());
  EventLoop* event_loop = loop.GetEventLoop(0);

  // Create our queues.
  auto source_queue = MakeIntQueue(2);
  auto sink_queue = MakeIntQueue(1);

  // Register source_queue read event handlers.
  port::Semaphore sem;
  InstallSource<int>(
    event_loop,
    source_queue.get(),
    [&] (Flow* flow, int x) {
      flow->Write(sink_queue.get(), x);
      sem.Post();
    });

  MsgLoopThread flow_threads(env_, &loop, "flow");
  int x = 1;
  source_queue->Write(x);
  source_queue->Write(x);

  // sink_queue should be blocked soon.
  ASSERT_TRUE(sem.TimedWait(std::chrono::seconds(1)));
  ASSERT_TRUE(sem.TimedWait(std::chrono::seconds(1)));

  // Check that backpressure was applied, but not lifted.
  auto stats1 = loop.GetStatisticsSync();
  auto applied_stat = "flow.flow_control.backpressure_applied";
  auto lifted_stat = "flow.flow_control.backpressure_lifted";
  ASSERT_EQ(stats1.GetCounterValue(applied_stat), 1);
  ASSERT_EQ(stats1.GetCounterValue(lifted_stat), 0);

  // Check that the message was logged a few times (should take 3 seconds total)
  ASSERT_TRUE(block_msg.TimedWait(std::chrono::seconds(5)));
  ASSERT_TRUE(block_msg.TimedWait(std::chrono::seconds(5)));
  ASSERT_TRUE(block_msg.TimedWait(std::chrono::seconds(5)));

  // Unregister the source.
  port::Semaphore done;
  std::unique_ptr<Command> cmd(
    MakeExecuteCommand([&] () {
      event_loop->GetFlowControl()->UnregisterSource(source_queue.get());
      done.Post();
    }));
  loop.SendCommand(std::move(cmd), 0);
  ASSERT_TRUE(done.TimedWait(std::chrono::seconds(5)));

  // Check that backpressure was lifted.
  auto stats2 = loop.GetStatisticsSync();
  ASSERT_EQ(stats2.GetCounterValue(applied_stat), 1);
  ASSERT_EQ(stats2.GetCounterValue(lifted_stat), 1);

  // Should no longer receive any blocked messages.
  ASSERT_TRUE(!block_msg.TimedWait(std::chrono::seconds(2)));
}

}  // namespace rocketspeed

int main(int argc, char** argv) {
  return rocketspeed::test::RunAllTests(argc, argv);
}

// Copyright (c) 2014, Facebook, Inc.  All rights reserved.
// This source code is licensed under the BSD-style license found in the
// LICENSE file in the root directory of this source tree. An additional grant
// of patent rights can be found in the PATENTS file in the same directory.
//
#pragma once

#include <map>
#include <memory>
#include <mutex>
#include "src/messages/commands.h"
#include "src/messages/messages.h"
#include "src/pilot/options.h"
#include "src/util/statistics.h"
#include "src/util/worker_loop.h"
#include "src/util/object_pool.h"

namespace rocketspeed {

class LogStorage;
class Pilot;

// This command instructs a pilot worker to append to the log storage, and
// then send an ack on completion.
class PilotWorkerCommand {
 public:
  PilotWorkerCommand() = default;

  PilotWorkerCommand(LogID logid,
                     std::unique_ptr<MessageData> msg,
                     uint64_t issued_time)
  : logid_(logid)
  , msg_(std::move(msg))
  , issued_time_(issued_time) {
  }

  // Get the log ID to append to.
  LogID GetLogID() const {
    return logid_;
  }

  // Releases ownership of the message and returns it.
  MessageData* ReleaseMessage() {
    return msg_.release();
  }

  // Get the time when this command was issued by the Pilot.
  uint64_t GetIssuedTime() const {
    return issued_time_;
  }

 private:
  LogID logid_;
  std::unique_ptr<MessageData> msg_;
  uint64_t issued_time_;
};

// These Commands sent from the Worker to the Pilot
class PilotCommand : public Command {
 public:
  PilotCommand(std::string message, const HostId& host, uint64_t issued_time):
    Command(issued_time),
    message_(std::move(message)) {
    recipient_.push_back(host);
  }
  void GetMessage(std::string* out) {
    out->assign(std::move(message_));
  }
  // return the Destination HostId, otherwise returns null.
  const Recipients& GetDestination() const {
    return recipient_;
  }
  bool IsSendCommand() const  {
    return true;
  }

 private:
  Recipients recipient_;
  std::string message_;
};

class PilotWorker;

// Storage for captured objects in the append callback.
struct AppendClosure : public PooledObject<AppendClosure> {
 public:
  AppendClosure(PilotWorker* worker,
                std::unique_ptr<MessageData> msg,
                LogID logid,
                uint64_t now)
  : worker_(worker)
  , msg_(std::move(msg))
  , logid_(logid)
  , append_time_(now) {
  }

  void operator()(Status append_status, SequenceNumber seqno);

 private:
  PilotWorker* worker_;
  std::unique_ptr<MessageData> msg_;
  LogID logid_;
  uint64_t append_time_;
};

/**
 * Pilot worker object. The pilot will allocate several of these, ideally one
 * per hardware thread. The workers take load off of the main thread by handling
 * the log appends and ack sending, and allows us to scale to multiple cores.
 */
class PilotWorker {
 public:
  // Constructs a new PilotWorker (does not start a thread).
  PilotWorker(const PilotOptions& options,
              LogStorage* storage,
              Pilot* pilot);

  // Forward a message to this worker for processing.
  // This will asynchronously append the message into the log storage,
  // and then send an ack back to the to the message origin.
  bool Forward(LogID logid, std::unique_ptr<MessageData> msg);

  // Start the worker loop on this thread.
  // Blocks until the worker loop ends.
  void Run();

  // Stop the worker loop.
  void Stop() {
    worker_loop_.Stop();
  }

  // Check if the worker loop is running.
  bool IsRunning() const {
    return worker_loop_.IsRunning();
  }

  // Get the statistics for this worker.
  const Statistics& GetStatistics() const {
    return stats_.all;
  }

  void AppendCallback(Status append_status,
                      SequenceNumber seqno,
                      std::unique_ptr<MessageData> msg,
                      LogID logid,
                      uint64_t append_time);

 private:
  friend struct AppendClosure;

  // Callback for worker loop commands.
  void CommandCallback(PilotWorkerCommand command);

  // Send an ack message to the host for the msgid.
  void SendAck(MessageData* msg,
               SequenceNumber seqno,
               MessageDataAck::AckStatus status);

  WorkerLoop<PilotWorkerCommand> worker_loop_;
  LogStorage* storage_;
  const PilotOptions& options_;
  Pilot* pilot_;
  PooledObjectList<AppendClosure> append_closure_pool_;
  std::mutex append_closure_pool_mutex_;

  struct Stats {
    Stats() {
      append_latency = all.AddLatency("rocketspeed.pilot.append_latency_us");
      worker_latency = all.AddLatency("rocketspeed.pilot.worker_latency_us");
      append_requests = all.AddCounter("rocketspeed.pilot.append_requests");
      failed_appends = all.AddCounter("rocketspeed.pilot.failed_appends");
    }

    Statistics all;

    // Latency of append request -> response.
    Histogram* append_latency;

    // Latency of send -> command process
    Histogram* worker_latency;

    // Number of append requests received.
    Counter* append_requests;

    // Number of append failures.
    Counter* failed_appends;
  } stats_;
};

}  // namespace rocketspeed

// Copyright (c) 2014, Facebook, Inc.  All rights reserved.
// This source code is licensed under the BSD-style license found in the
// LICENSE file in the root directory of this source tree. An additional grant
// of patent rights can be found in the PATENTS file in the same directory.
//
#pragma once

#include <memory>
#include <random>
#include <vector>
#include "include/Status.h"
#include "include/Types.h"
#include "include/Env.h"
#include "src/messages/messages.h"
#include "src/messages/msg_loop.h"
#include "src/util/storage.h"
#include "src/util/subscription_map.h"
#include "src/util/topic_uuid.h"
#include "src/util/common/linked_map.h"
#include "src/util/common/statistics.h"
#include "src/util/common/thread_check.h"
#include "src/controltower/options.h"
#include "src/controltower/data_cache.h"
#include "src/controltower/topic.h"
#include "src/controltower/tower.h"

namespace rocketspeed {

class Flow;
class LogTailer;
class LogRouter;
class LogReader;
class MsgLoop;

template <typename> class ThreadLocalQueues;
template <typename, typename> class ObservableMap;

class TopicTailer {
 friend class ControlTowerTest;
 public:
  /**
   * Create a TopicTailer.
   *
   * @param env Environment.
   * @param msg_loop MsgLoop this tailer belongs to.
   * @param worker_id Worker index tailer runs on.
   * @param log_tailer Tailer for logs. Will be manipulated by TopicTailer.
   * @param log_router For routing topics to logs.
   * @param info_log For logging.
   * @param cache_size_per_room cache size in bytes
   * @param cache_data_from_system_namespaces
   * @param cache_block_size  Number of messages in a cache block
   * @param bloom_bits_per_msg (used in cache)
   * @param on_message Callback for Deliver and Gap messages.
   * @param copilot_worker Function that maps copilot sub to worker.
   * @param tailer Output parameter for created TopicTailer.
   * @return ok() if TopicTailer created, otherwise error.
   */
  static Status CreateNewInstance(
    BaseEnv* env,
    MsgLoop* msg_loop,
    int worker_id,
    LogTailer* log_tailer,
    std::shared_ptr<LogRouter> log_router,
    std::shared_ptr<Logger> info_log,
    size_t cache_size_per_room,
    bool cache_data_from_system_namespaces,
    size_t cache_block_size,
    int bloom_bits_per_msg,
    std::function<void(Flow*,
                       const Message&,
                       std::vector<CopilotSub>)> on_message,
    std::function<int(const CopilotSub&)> copilot_worker,
    ControlTowerOptions::TopicTailer options,
    TopicTailer** tailer);

  /**
   * Initialize the TopicTailer first before using it.
   *
   * @param reader_ids IDs of readers on LogTailer.
   * @param max_subscription_lag Maximum number of sequence numbers that a
   *                             subscription can lag behind before being sent
   *                             a gap.
   * @return ok if successful, otherwise error code.
   */
  Status Initialize(const std::vector<size_t>& reader_ids,
                    int64_t max_subscription_lag);

  /**
   * Adds a subscriber to a topic. This call is not thread-safe.
   */
  Status AddSubscriber(const TopicUUID& topic,
                       SequenceNumber start,
                       CopilotSub id);

  /**
   * Removes a single subscription. This call is not thread-safe.
   */
  Status RemoveSubscriber(CopilotSub id);

  /**
   * Removes all subscriptions for a stream.
   */
  Status RemoveSubscriber(StreamID stream_id);

  /**
   * Process a data record from a log tailer, and forward to on_message.
   *
   * @param flow Flow from source.
   * @param msg Log record message.
   * @param log_id ID of log record resides in.
   * @param reader_id ID of reader delivering the record.
   */
  void SendLogRecord(
    Flow* flow,
    std::unique_ptr<MessageData>& msg,
    LogID log_id,
    size_t reader_id);

  /**
   * Process a gap record from a log tailer, and forward to on_message.
   *
   * @param flow Flow from source.
   * @param log_id ID of log containing the gap.
   * @param type The type of gap encountered.
   * @param from The first gap sequence number (inclusive).
   * @param to The last gap sequence number (inclusive).
   * @param reader_id ID of reader that encountered the gap.
   */
  void SendGapRecord(
    Flow* flow,
    LogID log_id,
    GapType type,
    SequenceNumber from,
    SequenceNumber to,
    size_t reader_id);

  /**
   * Should be called frequently to allow time-based processing.
   */
  void Tick();

  Statistics GetStatistics() const;

  /**
   * Clear the cache
   */
  std::string ClearCache();

  /**
   * Sets the size of the cache
   */
  std::string SetCacheCapacity(size_t newcapacity);

  /**
   * Get the current usage of the cache (in bytes)
   */
  std::string GetCacheUsage();

  /**
   * Get the current configured capacity of the cache (in bytes)
   */
  std::string GetCacheCapacity();

  /**
   * Get an estimate of tail seqno for a log, or 0 if unknown.
   */
  SequenceNumber GetTailSeqnoEstimate(LogID log_id) const;

  /**
   * Get human-readable information about a particular log.
   */
  std::string GetLogInfo(LogID log_id) const;

  /**
   * Get human-readable information about all logs.
   */
  std::string GetAllLogsInfo() const;

  ~TopicTailer();

  struct LogReaderId {
    explicit LogReaderId(LogID _log_id, LogReader* _reader)
    : log_id(_log_id), reader(_reader) {}

    bool operator==(const LogReaderId& rhs) const {
      return reader == rhs.reader && log_id == rhs.log_id;
    }

    LogID log_id;
    LogReader* reader;
  };

 private:
  struct FindLatestSeqnoResponse {
    Status status;
    LogID log_id;
    SequenceNumber seqno;
  };

  struct PendingSubscription {
    PendingSubscription(LogID _logid, SequenceNumber _seqno)
    : logid(_logid), seqno(_seqno) {}

    LogID logid;
    SequenceNumber seqno;
  };

  // private constructor
  TopicTailer(BaseEnv* env,
              MsgLoop* msg_loop,
              int worker_id,
              LogTailer* log_tailer,
              std::shared_ptr<LogRouter> log_router,
              std::shared_ptr<Logger> info_log,
              size_t cache_size_per_room,
              bool cache_data_from_system_namespaces,
              size_t cache_block_size,
              int bloom_bits_per_msg,
              std::function<void(Flow*,
                                 const Message&,
                                 std::vector<CopilotSub>)> on_message,
              std::function<int(const CopilotSub&)> copilot_worker,
              ControlTowerOptions::TopicTailer options);

  void AddTailSubscriber(Flow* flow,
                         const TopicUUID& topic,
                         CopilotSub id,
                         LogID logid,
                         SequenceNumber seqno);

  void AddSubscriberInternal(const TopicUUID& topic,
                             CopilotSub id,
                             LogID logid,
                             SequenceNumber start);

  void RemoveSubscriberInternal(const TopicUUID& topic,
                                CopilotSub id,
                                LogID logid);

  void RemoveSubscriberInternal(StreamID stream_id);

  /**
   * Finds the LogReader* with given reader_id, or nullptr if none found.
   */
  LogReader* FindLogReader(size_t reader_id);

  /**
   * Assign a new subscription (id + topic) to a LogReader.
   */
  LogReader* ReaderForNewSubscription(CopilotSub id,
                                      const TopicUUID& topic,
                                      LogID logid,
                                      SequenceNumber seqno);

  /**
   * Attempt to merge src into all other readers.
   * Merging into another reader means that the other reader will subsume all
   * subscriptions that src was serving, and src will stop reading on log_id.
   *
   * @return true if the reader was merged.
   */
  bool AttemptReaderMerges(LogReader* src, LogID log_id);

  /**
   * Deliver as much data from cache as possible.
   * seqno is set to the new fast-forwarded sequence number to subscribe to.
   * @return true if all was delivered, or false if caller must retry later.
   */
  bool DeliverFromCache(Flow* flow, const TopicUUID& topic,
          CopilotSub copilot_sub, LogID logid, SequenceNumber* seqno);

  /**
   * Sends a FindLatestSeqno request for a log.
   *
   * @param log_id Log to send for.
   */
  void SendFindLatestSeqnoRequest(LogID log_id);

  /**
   * Processes asynchronous response to a FindLatestSeqno request in the
   * storage.
   *
   * @param flow Source flow of incoming response.
   * @param resp Reponse information from the request.
   */
  void ProcessFindLatestSeqnoResponse(Flow* flow,
                                      FindLatestSeqnoResponse resp);

  /** Returns the number of FindLatestSeqno requests currently in flight. */
  size_t InFlightFindLatestSeqnoRequests() const;

  /**
   * Process a record from a log. Not thread safe.
   *
   * @param data Message that has been received from the log storage.
   * @param log_id Log that it was received on.
   * @param reader Reader that it was received for.
   * @param flow Source flow of incoming message.
   */
  void ReceiveLogRecord(std::unique_ptr<MessageData> data,
                        LogID log_id,
                        LogReader* reader,
                        Flow* flow);

  enum class CacheRead {
    kNoneRead,
    kReadContinue,
    kReadBackoff
  };

  /**
   * Checks the cache for records relevant to the reader and delivers them.
   *
   * @param flow Source flow.
   * @param log_id Log to read from.
   * @param reader_id ID of reader to advance.
   * @return true if records were read from the cache.
   */
  CacheRead AdvanceReaderFromCache(Flow* flow, LogID log_id, LogReader* reader);

  /**
   * Invokes AdvanceReaderFromCache and starts/stops of the LogReader depending
   * on whether backpressure was applied, and whether the reader is currently
   * paused or not.
   */
  void SendCacheRecord(Flow* flow, LogID log_id, LogReader* reader);

  /**
   * Attempts to read from the cache for a new subscription, and opens reader
   * if backpressure wasn't applied. If backpressure was applied, the
   * subscription will be added to a pending queue to be re-processed later.
   */
  void ProcessPendingSubscription(Flow* flow,
                                  const TopicUUID& topic,
                                  CopilotSub id,
                                  LogID logid,
                                  SequenceNumber seqno);

  /** Pending reader queue for a subscription */
  ObservableMap<CopilotSub, PendingSubscription>*
  GetPendingReaderQueue(const CopilotSub& sub);

  ThreadCheck thread_check_;

  BaseEnv* env_;

  // Message loop that we belong to, and our worker index.
  MsgLoop* msg_loop_;
  const int worker_id_;

  // The log tailer.
  LogTailer* log_tailer_;

  // Topic to log router.
  std::shared_ptr<LogRouter> log_router_;

  // Information log
  std::shared_ptr<Logger> info_log_;

  // Each reader is capable of reading each log once.
  // We have multiple readers in case a log is subscribed to at multiple
  // positions.
  std::vector<std::unique_ptr<LogReader>> log_readers_;

  // This is a 'virtual' reader for all pending subscriptions.
  // It has the same interface as LogReader, but doesn't actually read
  // from a log. Once a read reader (from log_readers_) becomes available,
  // it will take the state of pending_reader_ and open the log at the
  // correct position.
  std::unique_ptr<LogReader> pending_reader_;

  // Callback for outgoing messages.
  std::function<void(Flow*,
                     const Message&,
                     std::vector<CopilotSub>)> on_message_;

  // Subscription information per topic
  std::unordered_map<LogID, TopicManager> topic_map_;

  // Cached tail sequence number per log.
  std::unordered_map<LogID, SequenceNumber> tail_seqno_cached_;

  // Cache of data read from storage
  DataCache data_cache_;

  std::mt19937_64& prng_;

  ControlTowerOptions::TopicTailer options_;

  // Queues for storage threads responding with latest seqnos to rooms.
  std::unique_ptr<ThreadLocalQueues<FindLatestSeqnoResponse>>
    latest_seqno_queues_;

  // Map of subscriptions per stream.
  SubscriptionMap<TopicUUID> stream_subscriptions_;

  // The set of copilots awaiting find time response for each log.
  std::unordered_map<LogID, std::vector<CopilotSub>>
    pending_find_time_response_;

  // Set of FindLatestSeqno requests that haven't been sent yet.
  LinkedSet<LogID> pending_find_time_requests_;

  EventLoop* event_loop_;

  // Queue of subscriptions that are still reading from the cache and haven't
  // been added to a log reader yet.
  std::vector<std::shared_ptr<ObservableMap<CopilotSub, PendingSubscription>>>
    cache_readers_;

  // Really, this should be an ObservableSet, but nullptr_t will do for now.
  // We should also have a single source per LogReaderId, but that would
  // likely create a prohibitive number of eventfds, so it is avoided for now.
  std::shared_ptr<ObservableMap<LogReaderId, std::nullptr_t>>
    reentry_cache_readers_;

  // Maps copilots to worker thread index.
  std::function<int(const CopilotSub&)> copilot_worker_;

  struct Stats {
    Stats() {
      const std::string prefix = "tower.topic_tailer.";

      log_records_received =
        all.AddCounter(prefix + "log_records_received");
      log_records_received_payload_size =
        all.AddCounter(prefix + "log_records_received_payload_size");
      backlog_records_received =
        all.AddCounter(prefix + "backlog_records_received");
      tail_records_received =
        all.AddCounter(prefix + "tail_records_received");
      new_tail_records_sent =
        all.AddCounter(prefix + "new_tail_records_sent");
      log_records_with_subscriptions =
        all.AddCounter(prefix + "log_records_with_subscriptions");
      log_records_without_subscriptions =
        all.AddCounter(prefix + "log_records_without_subscriptions");
      bumped_subscriptions =
        all.AddCounter(prefix + "bumped_subscriptions");
      gap_records_received =
        all.AddCounter(prefix + "gap_records_received");
      gap_records_with_subscriptions =
        all.AddCounter(prefix + "gap_records_with_subscriptions");
      gap_records_without_subscriptions =
        all.AddCounter(prefix + "gap_records_without_subscriptions");
      benign_gaps_received =
        all.AddCounter(prefix + "benign_gaps_received");
      malignant_gaps_received =
        all.AddCounter(prefix + "malignant_gaps_received");
      add_subscriber_requests =
        all.AddCounter(prefix + "add_subscriber_requests");
      add_subscriber_requests_at_0 =
        all.AddCounter(prefix + "add_subscriber_requests_at_0");
      add_subscriber_requests_at_0_fast =
        all.AddCounter(prefix + "add_subscriber_requests_at_0_fast");
      add_subscriber_requests_at_0_slow =
        all.AddCounter(prefix + "add_subscriber_requests_at_0_slow");
      updated_subscriptions =
        all.AddCounter(prefix + "updated_subscriptions");
      remove_subscriber_requests =
        all.AddCounter(prefix + "remove_subscriber_requests");
      records_served_from_cache =
        all.AddCounter(prefix + "records_served_from_cache");
      reader_merges =
        all.AddCounter(prefix + "reader_merges");
      cache_reentries =
        all.AddCounter(prefix + "cache_reentries");
      cache_usage =
        all.AddCounter(prefix + "cache_usage");
      cache_reader_backoff =
        all.AddCounter(prefix + "cache_reader_backoff");
    }

    Statistics all;
    Counter* log_records_received;
    Counter* log_records_received_payload_size;
    Counter* backlog_records_received;
    Counter* tail_records_received;
    Counter* new_tail_records_sent;
    Counter* log_records_with_subscriptions;
    Counter* log_records_without_subscriptions;
    Counter* log_records_out_of_order;
    Counter* bumped_subscriptions;
    Counter* gap_records_received;
    Counter* gap_records_out_of_order;
    Counter* gap_records_with_subscriptions;
    Counter* gap_records_without_subscriptions;
    Counter* benign_gaps_received;
    Counter* malignant_gaps_received;
    Counter* add_subscriber_requests;
    Counter* add_subscriber_requests_at_0;
    Counter* add_subscriber_requests_at_0_fast;
    Counter* add_subscriber_requests_at_0_slow;
    Counter* updated_subscriptions;
    Counter* remove_subscriber_requests;
    Counter* records_served_from_cache;
    Counter* reader_merges;
    Counter* cache_reentries;
    Counter* cache_usage;
    Counter* cache_reader_backoff;
  } stats_;
};

}  // namespace rocketspeed

namespace std {
template <>
struct hash<rocketspeed::TopicTailer::LogReaderId> {
  size_t operator()(const rocketspeed::TopicTailer::LogReaderId& value) const {
    return std::hash<decltype(value.reader)>()(value.reader) ^
           std::hash<decltype(value.log_id)>()(value.log_id);
  }
};
}

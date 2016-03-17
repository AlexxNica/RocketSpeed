/* -*- Mode: C++; tab-width: 2; c-basic-offset: 2; indent-tabs-mode: nil -*- */
#pragma once

#include <chrono>
#include <memory>
#include <vector>
#include <tuple>

#include "logdevice/include/Client.h"
#include "logdevice/include/types.h"
#include "logdevice/common/types_internal.h"

namespace facebook { namespace logdevice {

/**
 * @file Utility class for buffering and batching appends on the client.
 *
 * The regular Client::append() immediately sends the record to LogDevice.
 * Because of the per-append cost of processing inside LogDevice, sending many
 * small records can limit throughput.
 *
 * This class allows latency of writes to be traded off for throughput.  It
 * presents a similar API to Client::append() but buffers appends for the same
 * log on the client and sends them to LogDevice in fewer, larger, records.
 * The records are automatically decoded on the read path by Reader.
 *
 * BufferedWriter appends are by necessity async so there is a callback
 * interface to notify the application when an append has completed.  Because
 * BufferedWriter is meant for high-throughput writing, the callback interface
 * does not use std::function but a slightly more complicated setup: the
 * application provides a single subclass of AppendCallback and an optional
 * piece of context (void*) for each append.
 *
 * Applications are expected to configure the latency tradeoff via
 * Options::time_trigger.  For example, a value of 1 second means that
 * buffered writes for a log will be flushed when the oldest of them has been
 * buffered for 1 second.  With a steady stream of appends to the log, we will
 * essentially flush once every second.
 *
 * See Options for additional features:
 * - automatic retrying of failed writes
 * - compression
 * - overall memory limit
 *
 * All methods in this class are thread-safe.
 *
 * See doc/buffered-writer.md for an overview of the implementation.
 */

class BufferedWriterImpl;

class BufferedWriter {
 public:
  /**
   * Callback interface.  All methods get called on an unspecified thread.
   * Applications should subclass and override desired notification methods.
   */
  class AppendCallback {
   public:
    using Context = void*;
    using ContextSet = std::vector<std::pair<Context, std::string> >;

    /**
     * Called when a batch of records for the same log was successfully
     * appended.
     *
     * Payload strings (available in `contexts') are no longer needed within
     * BufferedWriter so the application is free to steal them.  All of the
     * records share the same LSN and timestamp, available in `attrs'.
     */
    virtual void onSuccess(logid_t log_id,
                           ContextSet contexts,
                           const DataRecordAttributes& attrs) { }
    /**
     * Called when a batch of records for the same log failed to be appended,
     * and BufferedWriter exhausted all retries it was configured to do (if
     * any).
     *
     * Payload strings (available in `contexts') are no longer needed within
     * BufferedWriter so the application is free to steal them.
     */
    virtual void onFailure(logid_t log_id,
                           ContextSet contexts,
                           Status status) { }

    enum class RetryDecision { ALLOW, DENY };

    /**
     * Called when a batch of records for the same log failed to be appended,
     * but BufferedWriter is planning to retry.
     *
     * If ALLOW is returned, BufferedWriter will proceed to schedule the retry
     * for this batch.  If DENY is returned, BufferedWriter will not retry and
     * will instead invoke onFailure() shortly after.
     */
    virtual RetryDecision onRetry(logid_t log_id,
                                  const ContextSet& contexts,
                                  Status status) {
      return RetryDecision::ALLOW;
    }

    virtual ~AppendCallback() { }
  };

  struct Options {
    Options() { }

    // Flush buffered writes for a log when the oldest has been buffered this
    // long (negative for no trigger)
    std::chrono::milliseconds time_trigger{-1};

    // Flush buffered writes for a log as soon there are this many payload
    // bytes buffered (negative for no trigger)
    ssize_t size_trigger = -1;

    enum class RetryMode {
      // No retries
      NONE,
      // Retry each batch independently.
      //
      // This can cause writes to get reordered.  For example, suppose two
      // batches 1 and 2 get sent out, 1 fails and 2 succeeds.  After 1 is
      // retried, the contents of the log would be 21 (or 121 if the very
      // first write actually succeeded but we could not get confirmation).
      INDEPENDENT,
    };
    RetryMode retry_mode = RetryMode::NONE;
    // Max number of times to retry (negative for no limit).  You may also
    // manually track retries and have onRetry() return DENY to stop retrying
    // a particular batch.
    int retry_count = -1;
    // Initial delay before retrying (negative for a default 2x the append
    // timeout).  Subsequent retries are made after successively larger delays
    // (exponential backoff with a factor of 2) up to retry_max_delay
    std::chrono::milliseconds retry_initial_delay{-1};
    // Max delay when retrying (negative for no limit)
    std::chrono::milliseconds retry_max_delay{60000};

    enum class Compression {
      NONE = 0x00,
      ZSTD = 0x01,
      LZ4 = 0x04,
      LZ4_HC = 0x05,
    };
    Compression compression = Compression::LZ4;

    // Approximate memory budget for buffered and inflight writes.  If an
    // append() call would exceed this limit, it fails fast with E::NOBUFS.
    //
    // Accounting is not completely accurate for performance reasons.  There
    // is internal overhead per batch and there may be pathological cases
    // where actual memory usage exceeds the limit.  However, in most cases it
    // should stay well under.
    //
    // Negative for no limit.
    int32_t memory_limit_mb = -1;

    // Should the number of records in the batch be included in the payload?
    // TODO (#7720785): this should be always enabled, but may break readers
    // built using an older version of the library that doesn't understand
    // the new format.
    bool include_batch_size = false;
  };

  /**
   * Constructing and destructing a BufferedWriter involves interthread
   * communication (with LogDevice library threads) and may block if those
   * threads are busy.  BufferedWriter instances are meant to be long-lived
   * (and clients will typically use just one).
   */
  static std::unique_ptr<BufferedWriter> create(std::shared_ptr<Client> client,
                                                AppendCallback* callback,
                                                Options options = Options());

  /**
   * Same as Client::append() except the append may get buffered.
   *
   * If the call succeeds (returns 0), the class assumes ownership of the
   * payload.  If the call fails, the payload remains in the given
   * std::string.
   */
  int append(logid_t logid,
             std::string&& payload,
             AppendCallback::Context callback_context);

  using Append = std::tuple<logid_t, std::string, AppendCallback::Context>;
  /**
   * Multi-write version of append().  Requires less interthread communication
   * than calling append() for each record.
   *
   * @return A vector of Status objects, one for each input append.  The
   *         status is E::OK if the append was successfully queued for
   *         writing, or otherwise one of the `err' codes documented for the
   *         single-write append().  If some of the appends fail, their
   *         payloads remain in the input vector.
   */
  std::vector<Status> append(std::vector<Append>&& appends);

  /**
   * Instructs the class to immediately flush all buffered appends.  Does not
   * block, just passes messages to LogDevice threads.

   * It is not intended for this to be called often in production as it
   * can limit the amount of batching; space- and time-based flushing should
   * be preferred.
   *
   * @return 0 on success, -1 if messages could not be posted to some
   *         LogDevice threads
   */
  int flushAll();

  /**
   * NOTE: involves communication with LogDevice threads, blocks until they
   * acknowledge the destruction.
   */
  virtual ~BufferedWriter() { }

 private:
  BufferedWriter() {}           // can be constructed by the factory only
  BufferedWriter(const BufferedWriter&) = delete;
  BufferedWriter& operator= (const BufferedWriter&) = delete;

  friend class BufferedWriterImpl;
  BufferedWriterImpl* impl(); // downcasts (this)
};

}}

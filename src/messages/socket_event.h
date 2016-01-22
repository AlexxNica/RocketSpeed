//  Copyright (c) 2014, Facebook, Inc.  All rights reserved.
//  This source code is licensed under the BSD-style license found in the
//  LICENSE file in the root directory of this source tree. An additional grant
//  of patent rights can be found in the PATENTS file in the same directory.
//
#pragma once

#include <cstdint>
#include <deque>
#include <memory>
#include <string>

#include "src/messages/event_callback.h"
#include "src/messages/types.h"
#include "src/port/port.h"
#include "src/util/common/flow_control.h"
#include "include/HostId.h"
#include "src/util/common/statistics.h"
#include "src/util/common/thread_check.h"

namespace rocketspeed {

class EventCallback;
class EventLoop;
class Stream;

struct MessageOnStream {
  Stream* stream;
  std::unique_ptr<Message> message;
};

/**
 * Maximum number of iovecs to write at once. Note that an array of iovec will
 * be allocated on the stack with this length, so it should not be too high.
 */
static constexpr size_t kMaxIovecs = 256;

/** Size (in octets) of an encoded message header. */
static constexpr size_t kMessageHeaderEncodedSize =
    sizeof(uint8_t) + sizeof(uint32_t);

class SocketEventStats {
 public:
  explicit SocketEventStats(const std::string& prefix);

  Statistics all;
  Histogram* write_latency;     // time between message was serialised and sent
  Histogram* write_size_bytes;  // total bytes in write calls
  Histogram* write_size_iovec;  // total iovecs in write calls.
  Histogram* write_succeed_bytes;  // successful bytes written in write calls
  Histogram* write_succeed_iovec;  // successful iovecs written in write calls
  Counter* socket_writes;          // number of calls to write(v)
  Counter* partial_socket_writes;  // number of writes that partially succeeded
  Counter* messages_received[size_t(MessageType::max) + 1];
};

class SocketEvent : public Source<MessageOnStream>,
                    public Sink<SerializedOnStream> {
 public:
  /**
   * Creates a new SocketEvent for provided physical socket.
   *
   * @param event_loop An event loop to register the socket with.
   * @param fd The physical socket.
   * @param destination An optional destination, if present indicates that this
   *                    is an outbound socket.
   */
  static std::unique_ptr<SocketEvent> Create(EventLoop* event_loop,
                                             int fd,
                                             HostId destination = HostId());

  /**
   * Closes all streams on the connection and connection itself.
   * Since the socket will be closed as a result of this call, no goodby message
   * will be sent to the remote host, but every local stream will receive a
   * goodbye message.
   *
   * @param reason A reason why this connection is closing.
   */
  enum class ClosureReason : uint8_t {
    Error = 0x00,
    Graceful = 0x01,
  };
  void Close(ClosureReason reason);

  ~SocketEvent();

  /**
   * Creates a new outbound stream.
   * Provided stream ID must be not be used for any other stream on the
   * connection.
   *
   * @param stream_id A stream ID of the stream to be created.
   */
  std::unique_ptr<Stream> OpenStream(StreamID stream_id);

  /** Inherited from Source<MessageOnStream>. */
  void RegisterReadEvent(EventLoop* event_loop) final override;

  /** Inherited from Source<MessageOnStream>. */
  void SetReadEnabled(EventLoop* event_loop, bool enabled) final override;

  /** Inherited from Sink<SerializedOnStream>. */
  bool Write(SerializedOnStream& value, bool check_thread) final override;

  /** Inherited from Sink<SerializedOnStream>. */
  bool FlushPending(bool thread_check) final override;

  /** Inherited from Sink<SerializedOnStream>. */
  std::unique_ptr<EventCallback> CreateWriteCallback(
      EventLoop* event_loop, std::function<void()> callback) final override;

  bool IsInbound() const { return !destination_; }

  const HostId& GetDestination() const { return destination_; }

  EventLoop* GetEventLoop() const { return event_loop_; }

  const std::shared_ptr<Logger>& GetLogger() const;

 private:
  ThreadCheck thread_check_;

  const std::shared_ptr<SocketEventStats> stats_;

  /** Whether the socket is closing or has been closed. */
  bool closing_ = false;

  /** Reader and deserializer state. */
  size_t hdr_idx_;
  char hdr_buf_[kMessageHeaderEncodedSize];
  size_t msg_idx_;
  size_t msg_size_;
  std::unique_ptr<char[]> msg_buf_;  // receive buffer

  /** Writer and serializer state. */
  /** A list of chunks of data to be written. */
  std::deque<std::shared_ptr<TimestampedString>> send_queue_;
  /** The next valid offset in the earliest chunk of data to be written. */
  Slice partial_;

  /** The physical socket and read/write event associated with it. */
  int fd_;
  std::unique_ptr<EventCallback> read_ev_;
  std::unique_ptr<EventCallback> write_ev_;

  /** An EventTrigger to notify that the sink has some spare capacity. */
  EventTrigger write_ready_;
  /** A flow control object for this socket. */
  FlowControl flow_control_;

  EventLoop* event_loop_;

  bool timeout_cancelled_;  // have we removed from EventLoop connect_timeout_?

  /** A remote destination, non-empty for outbound connections only. */
  HostId destination_;
  /**
   * A map from remote (the one on the wire) StreamID to corresponding Stream
   * object for all (both inbound and outbound) streams.
   */
  std::unordered_map<StreamID, Stream*> remote_id_to_stream_;
  /** A map of all streams owned by this socket. */
  std::unordered_map<Stream*, std::unique_ptr<Stream>> owned_streams_;

  SocketEvent(EventLoop* event_loop, int fd, HostId destination);

  /**
   * Unregisters a stream with provided remote StreamID from the SocketEvent and
   * triggers closure of the socket if that was the last stream.
   * If the corresponding stream object is owned by the socket, it's destruction
   * will be deferred.
   *
   * @param remote_id A remote StreamID of the stream to unregister.
   */
  void UnregisterStream(StreamID remote_id);

  /** Handles write availability events from EventLoop. */
  Status WriteCallback();

  /** Handles read availability events from EventLoop. */
  Status ReadCallback();

  /**
   * Handles received messagea
   *
   * @param remote_id An ID of the stream that the message arrived on.
   * @param message The message.
   * @return True if another message can be received in the same read callback.
   */
  bool Receive(StreamID remote_id, std::unique_ptr<Message> message);
};

}  // namespace rocketspeed

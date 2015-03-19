// Copyright (c) 2014, Facebook, Inc.  All rights reserved.
// This source code is licensed under the BSD-style license found in the
// LICENSE file in the root directory of this source tree. An additional grant
// of patent rights can be found in the PATENTS file in the same directory.
//
#include "src/client/client.h"

#include <cassert>
#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <unordered_set>

#include "include/Logger.h"
#include "include/RocketSpeed.h"
#include "include/Slice.h"
#include "include/Status.h"
#include "include/Types.h"
#include "include/WakeLock.h"
#include "src/client/message_received.h"
#include "src/client/smart_wake_lock.h"
#include "src/messages/msg_loop_base.h"
#include "src/port/port.h"
#include "src/util/common/hash.h"

#ifndef USE_MQTTMSGLOOP
#include "src/messages/msg_loop.h"
#else
#include "rocketspeed/mqttclient/mqtt_msg_loop.h"
#include "rocketspeed/mqttclient/proxygen_mqtt.h"
#endif

namespace rocketspeed {

/** An implementation of the Client API that represents a creation error. */
class ClientCreationError : public Client {
 public:
  explicit ClientCreationError(const Status& creationStatus)
    : creationStatus_(creationStatus)
  {}

  virtual Status Start(SubscribeCallback,
                       MessageReceivedCallback,
                       RestoreStrategy) {
    return creationStatus_;
  }

  virtual PublishStatus Publish(const Topic&,
                                const NamespaceID,
                                const TopicOptions&,
                                const Slice&,
                                PublishCallback,
                                const MsgId message_id) {
    return PublishStatus(creationStatus_, message_id);
  }

  virtual void ListenTopics(const std::vector<SubscriptionRequest>&) {}
  virtual void Acknowledge(const MessageReceived&) {}
  virtual void SaveSubscriptions(SnapshotCallback) {}

 private:
  Status creationStatus_;
};

Status Client::Create(ClientOptions options, std::unique_ptr<Client>* client) {
  assert (client);
  std::unique_ptr<ClientImpl> clientImpl;
  auto st = ClientImpl::Create(std::move(options), &clientImpl);
  if (st.ok()) {
    client->reset(clientImpl.release());
  } else {
    client->reset(new ClientCreationError(st));
  }
  return st;
}

Status ClientImpl::Create(ClientOptions options,
                          std::unique_ptr<ClientImpl>* client,
                          bool is_internal) {
  assert (client);
  if (!is_internal) {
    if (options.config.GetTenantID() <= 100) {
      return Status::InvalidArgument("TenantId must be greater than 100.");
    }
  }
  // Validate arguments.
  if (options.config.GetPilotHostIds().empty()) {
    return Status::InvalidArgument("Must have at least one pilot.");
  }
  if (options.config.GetCopilotHostIds().empty()) {
    return Status::InvalidArgument("Must have at least one copilot.");
  }
  if (!options.info_log) {
    options.info_log = std::make_shared<NullLogger>();
  }

#ifndef USE_MQTTMSGLOOP
  MsgLoop* msg_loop_ = new MsgLoop(options.env,
                                   EnvOptions(),
                                   0,
                                   options.config.GetNumWorkers(),
                                   options.info_log,
                                   "client",
                                   options.client_id);
#else
  MQTTMsgLoop* msg_loop_ = new MQTTMsgLoop(
      options.env,
      options.client_id,
      options.config.GetPilotHostIds().front(),
      options.username,
      options.access_token,
      true, // We enable SSL when talking over MQTT.
      options.info_log,
      &ProxygenMQTTClient::Create);
#endif

  // TODO(pja) 1 : Just using first pilot for now, should use some sort of map.
  client->reset(new ClientImpl(options.env,
                               options.wake_lock,
                               options.config.GetPilotHostIds().front(),
                               options.config.GetCopilotHostIds().front(),
                               options.config.GetTenantID(),
                               msg_loop_,
                               std::move(options.storage),
                               options.info_log,
                               is_internal));
  return Status::OK();
}

/**
 * State of a client. We have one such structure per worker thread, a single
 * topic can have its state in only one such structure. Aligned to avoid false
 * sharing.
 */
struct alignas(CACHE_LINE_SIZE) ClientWorkerData {
  /** Map a subscribed topic to the last sequence number received. */
  std::unordered_map<TopicID, SequenceNumber> topic_map;
};

ClientImpl::ClientImpl(BaseEnv* env,
                       std::shared_ptr<WakeLock> wake_lock,
                       const HostId& pilot_host_id,
                       const HostId& copilot_host_id,
                       TenantID tenant_id,
                       MsgLoopBase* msg_loop,
                       std::unique_ptr<SubscriptionStorage> storage,
                       std::shared_ptr<Logger> info_log,
                       bool is_internal)
: env_(env)
, wake_lock_(std::move(wake_lock))
, copilot_host_id_(copilot_host_id)
, tenant_id_(tenant_id)
, msg_loop_(msg_loop)
, msg_loop_thread_spawned_(false)
, storage_(std::move(storage))
, info_log_(info_log)
, is_internal_(is_internal)
, publisher_(env, info_log, msg_loop, &wake_lock_, std::move(pilot_host_id)) {
  using std::placeholders::_1;

  // Setup callbacks.
  std::map<MessageType, MsgCallbackType> callbacks;
  callbacks[MessageType::mDeliver] = [this] (std::unique_ptr<Message> msg) {
    ProcessData(std::move(msg));
  };
  callbacks[MessageType::mMetadata] = [this] (std::unique_ptr<Message> msg) {
    ProcessMetadata(std::move(msg));
  };

  worker_data_.reset(new ClientWorkerData[msg_loop_->GetNumWorkers()]);

  msg_loop_->RegisterCallbacks(callbacks);

  if (storage_) {
    // Initialize subscription storage.
    storage_->Initialize(
        std::bind(&ClientImpl::ProcessRestoredSubscription, this, _1),
        msg_loop_);
  }
}

Status ClientImpl::Start(SubscribeCallback subscribe_callback,
                         MessageReceivedCallback receive_callback,
                         RestoreStrategy restore_strategy) {
  subscription_callback_ = std::move(subscribe_callback);
  receive_callback_ = std::move(receive_callback);
  if (!storage_ && restore_strategy == RestoreStrategy::kRestoreOnly) {
    return Status::InvalidArgument(
        "Cannot restore subscriptions without subscription storage strategy.");
  }
  if (!storage_ && restore_strategy == RestoreStrategy::kResubscribe) {
    return Status::InvalidArgument(
        "Cannot resubscribe without subscription storage startegy.");
  }
  if (restore_strategy == RestoreStrategy::kRestoreOnly ||
      restore_strategy == RestoreStrategy::kResubscribe) {
    // Read initial state from snapshot.
    Status status = storage_->ReadSnapshot();
    if (!status.ok()) {
      return status;
    }
  }

  msg_loop_thread_ = env_->StartThread([this]() {
    msg_loop_->Run();
  }, "client");
  msg_loop_thread_spawned_ = true;

  Status st = msg_loop_->WaitUntilRunning();
  if (!st.ok()) {
    return st;
  }

  if (restore_strategy == RestoreStrategy::kResubscribe) {
    // Resubscribe to previously subscribed topics.
    storage_->LoadAll();
  }
  return Status::OK();
}

ClientImpl::~ClientImpl() {
  // Delete the message loop.
  // This stops the event loop, which may block.
  delete msg_loop_;

  if (msg_loop_thread_spawned_) {
    // Wait for thread to join.
    env_->WaitForJoin(msg_loop_thread_);
  }
}

PublishStatus ClientImpl::Publish(const Topic& name,
                                  const NamespaceID namespace_id,
                                  const TopicOptions& options,
                                  const Slice& data,
                                  PublishCallback callback,
                                  const MsgId message_id) {
  if (!is_internal_) {
    if (namespace_id <= 100) {
      return PublishStatus(
          Status::InvalidArgument(
              "NamespaceIDs <= 100 are reserver for internal usage."),
          message_id);
    }
  }
  return publisher_.Publish(tenant_id_,
                            namespace_id,
                            name,
                            options,
                            data,
                            std::move(callback),
                            message_id);
}

// Subscribe to a specific topics.
void ClientImpl::ListenTopics(const std::vector<SubscriptionRequest>& topics) {
  // Vector of subscriptions for each worker loop.
  // (subscriptions are sharded on topic to worker loops).
  std::vector<std::vector<TopicPair>> subscribe(msg_loop_->GetNumWorkers());
  std::vector<SubscriptionRequest> restore;

  // Determine which requests can be executed right away and which
  // subscriptions need to be restored.
  for (const auto& elem : topics) {
    if (storage_) {
      // Update subscription state accordingly.
      storage_->Update(elem);
    }

    // No need to restore subscription state if we want to unsubscribe.
    if (!elem.subscribe || elem.start) {
      auto type = elem.subscribe ? MetadataType::mSubscribe
                                 : MetadataType::mUnSubscribe;
      auto start = elem.subscribe ? elem.start.get() : 0;
      int worker_id = GetWorkerForTopic(elem.topic_name);
      subscribe[worker_id].emplace_back(start,
                                        elem.topic_name,
                                        type,
                                        elem.namespace_id);
    } else {
      restore.push_back(elem);
    }
  }

  if (storage_ && !restore.empty()) {
    wake_lock_.AcquireForLoadingSubscriptions();
    storage_->Load(std::move(restore));
  }
  for (int worker_id = 0; worker_id < msg_loop_->GetNumWorkers(); ++worker_id) {
    if (!subscribe[worker_id].empty()) {
      IssueSubscriptions(std::move(subscribe[worker_id]), worker_id);
    }
  }
}

void ClientImpl::IssueSubscriptions(std::vector<TopicPair> topics,
                                    int worker_id) {
  // Construct message.
  MessageMetadata message(tenant_id_,
                          MessageMetadata::MetaType::Request,
                          msg_loop_->GetClientId(worker_id),
                          topics);

  // Send to event loop for processing (the loop will free it).
  wake_lock_.AcquireForSending();
  Status status = msg_loop_->SendRequest(message,
                                         copilot_host_id_.ToClientId(),
                                         worker_id);

  // If there was any error, invoke callback with appropriate status
  if (!status.ok() && subscription_callback_) {
    for (auto& elem : topics) {
      SubscriptionStatus error_msg;
      error_msg.status = status;
      error_msg.namespace_id = elem.namespace_id;
      error_msg.topic_name = std::move(elem.topic_name);
      subscription_callback_(std::move(error_msg));
    }
  }
}

void ClientImpl::Acknowledge(const MessageReceived& message) {
  if (storage_) {
    // Note the +1. We store the next sequence number we want, but the
    // acknowledged message carries the number that we already know about.
    SubscriptionRequest request(message.GetNamespaceId(),
                                message.GetTopicName().ToString(),
                                true,
                                message.GetSequenceNumber() + 1);
    wake_lock_.AcquireForUpdatingSubscriptions();
    storage_->Update(std::move(request));
  }
}

void ClientImpl::SaveSubscriptions(SnapshotCallback snapshot_callback) {
  if (storage_) {
    storage_->WriteSnapshot(snapshot_callback);
  } else {
    snapshot_callback(Status::InternalError(
        "Cannot save subscriptions without subscription storage."));
  }
}

/*
** Process a received data message and deliver it to application.
*/
void ClientImpl::ProcessData(std::unique_ptr<Message> msg) {
  wake_lock_.AcquireForReceiving();
  msg_loop_->ThreadCheck();

  // Check that message has correct origin.
  if (!msg_loop_->CheckMessageOrigin(msg.get())) {
    return;
  }

  const MessageData* data = static_cast<const MessageData*>(msg.get());

  LOG_INFO(info_log_, "Received data (%.16s)",
    data->GetPayload().ToString().c_str());

  // Extract topic id from message.
  TopicID topic_id(data->GetNamespaceId(), data->GetTopicName().ToString());

  // Get the topic map for this thread.
  int worker_id = msg_loop_->GetThreadWorkerIndex();
  auto& topic_map = worker_data_[worker_id].topic_map;

  // verify that we are subscribed to this topic
  auto iter = topic_map.find(topic_id);
  // No active subscription to this topic, ignore message
  if (iter == topic_map.end()) {
    LOG_INFO(info_log_,
      "Discarded message (%.16s) due to missing subcription for Topic(%d, %s)",
      data->GetPayload().ToString().c_str(),
      data->GetNamespaceId(),
      topic_id.topic_name.c_str());
    return;
  }
  SequenceNumber last_msg_received = iter->second;

  // Old message, ignore iter
  if (data->GetSequenceNumber() <= last_msg_received) {
    LOG_INFO(info_log_,
      "Message (%.16s)@%llu received out of order on Topic(%d, %s)@%llu",
      data->GetPayload().ToString().c_str(),
      static_cast<long long unsigned int>(data->GetSequenceNumber()),
      data->GetNamespaceId(),
      topic_id.topic_name.c_str(),
      static_cast<long long unsigned int>(last_msg_received));
    return;
  }
  // update last seqno received for this topic
  iter->second = data->GetSequenceNumber();

  // Create message wrapper for client (do not copy payload)
  std::unique_ptr<MessageReceivedClient> newmsg(
                                     new MessageReceivedClient(std::move(msg)));

  // deliver message to application
  receive_callback_(std::move(newmsg));
}

// Process Metadata response messages arriving from the Cloud.
void ClientImpl::ProcessMetadata(std::unique_ptr<Message> msg) {
  wake_lock_.AcquireForReceiving();
  msg_loop_->ThreadCheck();

  // Check that message has correct origin.
  if (!msg_loop_->CheckMessageOrigin(msg.get())) {
    return;
  }

  const MessageMetadata* meta = static_cast<const MessageMetadata*>(msg.get());

  // The client should receive only responses to subscribe/unsubscribe.
  assert(meta->GetMetaType() == MessageMetadata::MetaType::Response);

  // Get the topic map for this thread.
  int worker_id = msg_loop_->GetThreadWorkerIndex();
  auto& topic_map = worker_data_[worker_id].topic_map;

  std::vector<TopicPair> pairs = meta->GetTopicInfo();
  // This is the response ack of a subscription request sent earlier.
  for (auto& elem : pairs) {

    // invoke application-registered callback
    if (subscription_callback_) {
      SubscriptionStatus ret;
      ret.status = Status::OK();
      ret.seqno = elem.seqno;  // start seqno of this subscription
      ret.subscribed = (elem.topic_type == MetadataType::mSubscribe);
      ret.topic_name = elem.topic_name;
      ret.namespace_id = elem.namespace_id;
      subscription_callback_(std::move(ret));
    }
    // Record confirmed subscriptions
    TopicID topic_id(elem.namespace_id, std::move(elem.topic_name));
    // topic_map stores last received, so use -1 to say we want higher seqnos.
    topic_map[topic_id] = elem.seqno - 1;

  }
}

void ClientImpl::ProcessRestoredSubscription(
    const std::vector<SubscriptionRequest> &restored) {
  // Vector of subscriptions for each worker loop.
  // (subscriptions are sharded on topic to worker loops).
  std::vector<std::vector<TopicPair>> subscribe(msg_loop_->GetNumWorkers());

  for (const auto& elem : restored) {
    if (!elem.subscribe) {
      // We shouldn't ever restore unsubscribe request.
      LOG_WARN(info_log_,
               "Restored unsubscribe request for namespace %d topic %s",
               elem.namespace_id,
               elem.topic_name.c_str());
      assert(0);
    } else if (elem.start) {
      int worker_id = GetWorkerForTopic(elem.topic_name);
      subscribe[worker_id].emplace_back(elem.start.get(),
                                        elem.topic_name,
                                        MetadataType::mSubscribe,
                                        elem.namespace_id);
    } else {
      // Inform the user that subscription restoring failed.
      if (subscription_callback_) {
        SubscriptionStatus failed_restore;
        // This is the status returned when we failed to restore subscription.
        failed_restore.status = Status::NotFound();
        failed_restore.namespace_id = elem.namespace_id;
        failed_restore.topic_name = elem.topic_name;
        subscription_callback_(std::move(failed_restore));
      }
    }
  }

  for (int worker_id = 0; worker_id < msg_loop_->GetNumWorkers(); ++worker_id) {
    if (!subscribe[worker_id].empty()) {
      IssueSubscriptions(subscribe[worker_id], worker_id);
    }
  }
}

Statistics ClientImpl::GetStatistics() const {
  return msg_loop_->GetStatistics();
}

int ClientImpl::GetWorkerForTopic(const Topic& name) const {
  return static_cast<int>(MurmurHash2<std::string>()(name) %
                          msg_loop_->GetNumWorkers());
}

MessageReceivedClient::~MessageReceivedClient() {
}

}  // namespace rocketspeed

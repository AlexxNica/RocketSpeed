// Copyright (c) 2014, Facebook, Inc.  All rights reserved.
// This source code is licensed under the BSD-style license found in the
// LICENSE file in the root directory of this source tree. An additional grant
// of patent rights can be found in the PATENTS file in the same directory.
//
#define __STDC_FORMAT_MACROS
#include "src/client/client.h"

#include <cmath>
#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <random>
#include <thread>
#include <tuple>
#include <unordered_map>
#include <unordered_set>

#include "external/folly/move_wrapper.h"

#include "include/Logger.h"
#include "include/RocketSpeed.h"
#include "include/Slice.h"
#include "include/Status.h"
#include "include/SubscriptionStorage.h"
#include "include/Types.h"
#include "include/WakeLock.h"
#include "src/client/smart_wake_lock.h"
#include "src/client/subscriber.h"
#include "src/messages/msg_loop.h"
#include "src/port/port.h"
#include "src/util/common/flow_control.h"
#include "src/util/common/random.h"
#include "src/util/timeout_list.h"

namespace rocketspeed {

////////////////////////////////////////////////////////////////////////////////
Status Client::Create(ClientOptions options,
                      std::unique_ptr<Client>* out_client) {
  RS_ASSERT(out_client);
  std::unique_ptr<ClientImpl> client_impl;
  auto st = ClientImpl::Create(std::move(options), &client_impl);
  if (st.ok()) {
    *out_client = std::move(client_impl);
  }
  return st;
}

Status ClientImpl::Create(ClientOptions options,
                          std::unique_ptr<ClientImpl>* out_client,
                          bool is_internal) {
  RS_ASSERT(out_client);

  // Validate arguments.
  if (!options.config) {
    return Status::InvalidArgument("Missing configuration.");
  }
  if (options.backoff_base < 1.0) {
    return Status::InvalidArgument("Backoff base must be >= 1.0");
  }
  if (!options.backoff_distribution) {
    return Status::InvalidArgument("Missing backoff distribution.");
  }
  if (!options.info_log) {
    options.info_log = std::make_shared<NullLogger>();
  }

  std::unique_ptr<MsgLoop> msg_loop_(new MsgLoop(options.env,
                                                 EnvOptions(),
                                                 -1,  // port
                                                 options.num_workers,
                                                 options.info_log,
                                                 "client"));

  Status st = msg_loop_->Initialize();
  if (!st.ok()) {
    return st;
  }

  std::unique_ptr<ClientImpl> client(
      new ClientImpl(std::move(options), std::move(msg_loop_), is_internal));

  st = client->Start();
  if (!st.ok()) {
    return st;
  }

  *out_client = std::move(client);
  return Status::OK();
}

namespace {

class RouterFromConfiguration : public SubscriptionRouter {
 public:
  explicit RouterFromConfiguration(const std::shared_ptr<Configuration>& config)
  : config_(config) {}

  size_t GetVersion() override { return config_->GetCopilotVersion(); }

  HostId GetHost() override {
    HostId host_id;
    auto st = config_->GetCopilot(&host_id);
    return st.ok() ? host_id : HostId();
  }

  void MarkHostDown(const HostId& host_id) override {}

 private:
  std::shared_ptr<Configuration> config_;
};

class ShardingFromConfiguration : public ShardingStrategy {
 public:
  explicit ShardingFromConfiguration(
      const std::shared_ptr<Configuration>& config)
  : config_(config) {}

  size_t GetShard(const NamespaceID& namespace_id,
                  const Topic& topic_name) const override {
    return 0;
  }

  std::unique_ptr<SubscriptionRouter> GetRouter(size_t shard) override {
    RS_ASSERT(shard == 0);
    return std::unique_ptr<SubscriptionRouter>(
        new RouterFromConfiguration(config_));
  }

 private:
  std::shared_ptr<Configuration> config_;
};

}  // namespace

ClientImpl::ClientImpl(ClientOptions options,
                       std::unique_ptr<MsgLoop> msg_loop,
                       bool is_internal)
: options_(std::move(options))
, wake_lock_(std::move(options_.wake_lock))
, msg_loop_(std::move(msg_loop))
, msg_loop_thread_spawned_(false)
, is_internal_(is_internal)
, publisher_(options_, msg_loop_.get(), &wake_lock_)
, subscriber_(options_,
              msg_loop_,
              [](MsgLoop* loop, const NamespaceID&, const Topic&) {
                return loop->LoadBalancedWorkerId();
              },
              std::unique_ptr<ShardingStrategy>(
                  new ShardingFromConfiguration(options_.config))) {
  LOG_VITAL(options_.info_log, "Creating Client");
}

void ClientImpl::SetDefaultCallbacks(
    SubscribeCallback subscription_callback,
    std::function<void(std::unique_ptr<MessageReceived>&)> deliver_callback,
    std::function<void(std::unique_ptr<DataLossInfo>&)> data_loss_callback) {
  subscription_cb_fallback_ = std::move(subscription_callback);
  deliver_cb_fallback_ = std::move(deliver_callback);
  data_loss_callback_ = std::move(data_loss_callback);
}

ClientImpl::~ClientImpl() {
  Stop();
}

void ClientImpl::Stop() {
  // Stop the event loop. May block.
  msg_loop_->Stop();

  if (msg_loop_thread_spawned_) {
    // Wait for thread to join.
    options_.env->WaitForJoin(msg_loop_thread_);
    msg_loop_thread_spawned_ = false;
  }
}

PublishStatus ClientImpl::Publish(const TenantID tenant_id,
                                  const Topic& name,
                                  const NamespaceID& namespace_id,
                                  const TopicOptions& options,
                                  const Slice& data,
                                  PublishCallback callback,
                                  const MsgId message_id) {
  if (!is_internal_) {
    if (tenant_id <= 100 && tenant_id != GuestTenant) {
      return PublishStatus(
          Status::InvalidArgument("TenantID must be greater than 100."),
          message_id);
    }

    if (IsReserved(namespace_id)) {
      return PublishStatus(Status::InvalidArgument(
                               "NamespaceID is reserved for internal usage."),
                           message_id);
    }
  }
  return publisher_.Publish(tenant_id,
                            namespace_id,
                            name,
                            options,
                            data,
                            std::move(callback),
                            message_id);
}

namespace {
template <typename T>
std::function<void(Flow*, T)> IgnoreFlow(std::function<void(T)> f) {
  if (!f) {
    return nullptr;
  }
  auto moved_f = folly::makeMoveWrapper(std::move(f));
  return [moved_f](Flow*, T t) { return (*moved_f)(std::forward<T>(t)); };
}
}

SubscriptionHandle ClientImpl::Subscribe(
    SubscriptionParameters parameters,
    std::function<void(std::unique_ptr<MessageReceived>&)> deliver_callback,
    SubscribeCallback subscription_callback,
    std::function<void(std::unique_ptr<DataLossInfo>&)> data_loss_callback) {
  // Select callbacks taking fallbacks into an account.
  if (!subscription_callback) {
    subscription_callback = subscription_cb_fallback_;
  }
  if (!deliver_callback) {
    deliver_callback = deliver_cb_fallback_;
  }
  if (!data_loss_callback) {
    data_loss_callback = data_loss_callback_;
  }

  return subscriber_.Subscribe(nullptr,
                               std::move(parameters),
                               IgnoreFlow(std::move(deliver_callback)),
                               std::move(subscription_callback),
                               IgnoreFlow(std::move(data_loss_callback)));
}

Status ClientImpl::Unsubscribe(SubscriptionHandle sub_handle) {
  return subscriber_.Unsubscribe(nullptr, sub_handle) ? Status::OK()
                                                      : Status::NoBuffer();
}

Status ClientImpl::Acknowledge(const MessageReceived& message) {
  return subscriber_.Acknowledge(nullptr, message) ? Status::OK()
                                                   : Status::NoBuffer();
}

void ClientImpl::SaveSubscriptions(SaveSubscriptionsCallback save_callback) {
  subscriber_.SaveSubscriptions(std::move(save_callback));
}

Status ClientImpl::RestoreSubscriptions(
    std::vector<SubscriptionParameters>* subscriptions) {
  if (!options_.storage) {
    return Status::NotInitialized();
  }

  return options_.storage->RestoreSubscriptions(subscriptions);
}

Statistics ClientImpl::GetStatisticsSync() {
  Statistics aggregated = msg_loop_->GetStatisticsSync();
  aggregated.Aggregate(subscriber_.GetStatisticsSync());
  return aggregated;
}

Status ClientImpl::Start() {
  auto st = subscriber_.Start();
  if (!st.ok()) {
    return st;
  }

  msg_loop_thread_ =
      options_.env->StartThread([this]() { msg_loop_->Run(); }, "client");
  msg_loop_thread_spawned_ = true;
  return Status::OK();
}

}  // namespace rocketspeed

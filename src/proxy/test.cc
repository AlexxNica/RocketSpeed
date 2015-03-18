//  Copyright (c) 2014, Facebook, Inc.  All rights reserved.
//  This source code is licensed under the BSD-style license found in the
//  LICENSE file in the root directory of this source tree. An additional grant
//  of patent rights can be found in the PATENTS file in the same directory.
//
#include <unistd.h>
#include <chrono>
#include <string>
#include <vector>

#include "src/test/test_cluster.h"
#include "src/proxy/proxy.h"
#include "src/util/testharness.h"
#include "src/port/port.h"

namespace rocketspeed {

class ProxyTest {
 public:
  ProxyTest() {
    env = Env::Default();
    ASSERT_OK(test::CreateLogger(env, "ProxyTest", &info_log));
    cluster.reset(new LocalTestCluster(info_log, true, true, true));
    ASSERT_OK(cluster->GetStatus());

    // Create proxy.
    ProxyOptions opts;
    opts.info_log = info_log;
    opts.conf.reset(Configuration::Create(cluster->GetPilotHostIds(),
                                          cluster->GetCopilotHostIds(),
                                          Tenant::GuestTenant,
                                          4));
    ASSERT_OK(Proxy::CreateNewInstance(std::move(opts), &proxy));
  }

  Env* env;
  std::shared_ptr<Logger> info_log;
  std::unique_ptr<LocalTestCluster> cluster;
  std::unique_ptr<Proxy> proxy;
};

TEST(ProxyTest, Publish) {
  // Start the proxy.
  // We're going to publish a message and expect an ack in return.
  const ClientID our_client = "proxy_client";
  port::Semaphore checkpoint;

  std::atomic<int64_t> expected_session;
  auto on_message = [&] (int64_t session, std::string data) {
    ASSERT_EQ(session, expected_session.load());
    std::unique_ptr<Message> msg =
      Message::CreateNewInstance(Slice(data).ToUniqueChars(), data.size());
    ASSERT_TRUE(msg != nullptr);
    ASSERT_EQ(MessageType::mDataAck, msg->GetMessageType());
    // Proxy is free to rewrite client ids, but any change should be invisible
    // to the clients.
    ASSERT_EQ(our_client, msg->GetOrigin());
    checkpoint.Post();
  };
  std::atomic<size_t> forcibly_disconnected(0);
  auto on_disconnect = [&](const std::vector<int64_t>& sessions) {
    forcibly_disconnected += sessions.size();
  };
  proxy->Start(on_message, on_disconnect);

  // Send a publish message.
  std::string serial;
  MessageData publish(MessageType::mPublish,
                      Tenant::GuestTenant,
                      our_client,
                      Slice("topic"),
                      101,
                      Slice("payload"));
  publish.SerializeToString(&serial);

  const int64_t session = 123;
  expected_session = session;

  // Send through proxy to pilot. Pilot should respond and proxy will send
  // serialized response to on_message defined above.
  ASSERT_OK(proxy->Forward(serial, session, -1));
  ASSERT_TRUE(checkpoint.TimedWait(std::chrono::seconds(1)));

  // Now try some out of order messages.

  ASSERT_OK(proxy->Forward(serial, session, 1));
  // should not arrive
  ASSERT_TRUE(!checkpoint.TimedWait(std::chrono::milliseconds(100)));

  ASSERT_OK(proxy->Forward(serial, session, 2));
  // should not arrive
  ASSERT_TRUE(!checkpoint.TimedWait(std::chrono::milliseconds(100)));

  ASSERT_OK(proxy->Forward(serial, session, 0));
  // all three should arrive
  ASSERT_TRUE(checkpoint.TimedWait(std::chrono::milliseconds(100)));
  ASSERT_TRUE(checkpoint.TimedWait(std::chrono::milliseconds(100)));
  ASSERT_TRUE(checkpoint.TimedWait(std::chrono::milliseconds(100)));

  expected_session = session + 1;
  ASSERT_OK(proxy->Forward(serial, session + 1, 0));
  // different session, should arrive
  ASSERT_TRUE(checkpoint.TimedWait(std::chrono::milliseconds(100)));

  // It doesn't mean that on_disconnect would not eventually be called, but if
  // called, it is always a thread that handles one of the messages that we were
  // waiting for.
  ASSERT_EQ(0, forcibly_disconnected.load());
}

TEST(ProxyTest, SeqnoError) {
  // Start the proxy.
  // We're going to ping an expect an error.
  port::Semaphore checkpoint;
  auto on_disconnect = [&] (std::vector<int64_t> session) {
    checkpoint.Post();
  };
  proxy->Start(nullptr, on_disconnect);

  std::string serial;
  MessagePing ping(Tenant::GuestTenant,
                   MessagePing::PingType::Request,
                   "client");
  ping.SerializeToString(&serial);

  const int64_t session = 123;

  // Send to proxy on seqno 999999999. Will be out of buffer space and fail.
  // Should get the on_disconnect_ error.
  ASSERT_OK(proxy->Forward(serial, session, 999999999));
  ASSERT_TRUE(checkpoint.TimedWait(std::chrono::seconds(1)));
}

TEST(ProxyTest, DestroySession) {
  // Start the proxy.
  // We're going to ping an expect an error.
  port::Semaphore checkpoint;
  auto on_message = [&] (int64_t session, std::string data) {
    checkpoint.Post();
  };
  proxy->Start(on_message, nullptr);

  std::string serial;
  MessagePing ping(Tenant::GuestTenant,
                   MessagePing::PingType::Request,
                   "client");
  ping.SerializeToString(&serial);

  const int64_t session = 123;

  // Send to proxy then await response.
  ASSERT_OK(proxy->Forward(serial, session, 0));
  ASSERT_TRUE(checkpoint.TimedWait(std::chrono::seconds(1)));

  // Check that pilot and copilot have at least one client.
  ASSERT_NE(cluster->GetPilot()->GetMsgLoop()->GetNumClientsSync(), 0);
  ASSERT_NE(cluster->GetCopilot()->GetMsgLoop()->GetNumClientsSync(), 0);

  // Now destroy, and send at seqno 1. Should not get response.
  proxy->DestroySession(session);
  ASSERT_OK(proxy->Forward(serial, session, 1));
  ASSERT_TRUE(!checkpoint.TimedWait(std::chrono::milliseconds(100)));

  // Check that pilot and copilot have no clients.
  ASSERT_EQ(cluster->GetPilot()->GetMsgLoop()->GetNumClientsSync(), 0);
  ASSERT_EQ(cluster->GetCopilot()->GetMsgLoop()->GetNumClientsSync(), 0);
}

TEST(ProxyTest, ServerDown) {
  // Start the proxy.
  // We're going to ping and expect an error.
  port::Semaphore checkpoint;
  auto on_message = [&] (int64_t session, std::string data) {
    checkpoint.Post();
  };
  port::Semaphore disconnect_checkpoint;
  auto on_disconnect = [&] (std::vector<int64_t> sessions) {
    disconnect_checkpoint.Post();
    std::sort(sessions.begin(), sessions.end());
    ASSERT_EQ(sessions.size(), 2);
    ASSERT_EQ(sessions[0], 123);
    ASSERT_EQ(sessions[1], 456);
  };
  proxy->Start(on_message, on_disconnect);

  std::string serial;
  MessagePing ping(Tenant::GuestTenant,
                   MessagePing::PingType::Request,
                   "client");
  ping.SerializeToString(&serial);

  // Send to proxy then await response.
  ASSERT_OK(proxy->Forward(serial, 123, 0));
  ASSERT_OK(proxy->Forward(serial, 456, 0));
  ASSERT_TRUE(checkpoint.TimedWait(std::chrono::seconds(1)));
  ASSERT_TRUE(checkpoint.TimedWait(std::chrono::seconds(1)));

  // Now destroy cluster.
  cluster.reset(nullptr);

  // Should get disconnect message.
  ASSERT_TRUE(disconnect_checkpoint.TimedWait(std::chrono::seconds(1)));
}

TEST(ProxyTest, ForwardGoodbye) {
  // Start the proxy.
  // We're going to talk to pilot and copilot, then say goodbye.
  port::Semaphore checkpoint;
  auto on_message = [&] (int64_t session, std::string data) {
    checkpoint.Post();
  };
  proxy->Start(on_message, nullptr);

  // Send a publish message.
  std::string publish_serial;
  MessageData publish(MessageType::mPublish,
                      Tenant::GuestTenant,
                      "client",
                      Slice("topic"),
                      101,
                      Slice("payload"));
  publish.SerializeToString(&publish_serial);

  const int64_t session = 123;
  ASSERT_OK(proxy->Forward(publish_serial, session, 0));
  ASSERT_TRUE(checkpoint.TimedWait(std::chrono::seconds(1)));

  // Send subscribe message.
  NamespaceID ns = 101;
  std::string sub_serial;
  MessageMetadata sub(Tenant::GuestTenant,
                      MessageMetadata::MetaType::Request,
                      "client",
                      { TopicPair(1, "topic", MetadataType::mSubscribe, ns) });
  sub.SerializeToString(&sub_serial);
  ASSERT_OK(proxy->Forward(sub_serial, session, 1));
  ASSERT_TRUE(checkpoint.TimedWait(std::chrono::seconds(1)));

  // Check that pilot and copilot have at least one client.
  // Copilot may have more due to control tower connections.
  int npilot = cluster->GetPilot()->GetMsgLoop()->GetNumClientsSync();
  int ncopilot = cluster->GetCopilot()->GetMsgLoop()->GetNumClientsSync();
  ASSERT_NE(npilot, 0);
  ASSERT_NE(ncopilot, 0);

  // Send goodbye message.
  std::string goodbye_serial;
  MessageGoodbye goodbye(Tenant::GuestTenant,
                         "client",
                         MessageGoodbye::Code::Graceful,
                         MessageGoodbye::OriginType::Client);
  goodbye.SerializeToString(&goodbye_serial);
  ASSERT_OK(proxy->Forward(goodbye_serial, session, 2));
  env->SleepForMicroseconds(10000);  // time to propagate

  // Check that pilot and copilot have one fewer clients each.
  // Account for one extra client used by the copilot to write
  // to the rollcall topic.
  int num_rollcall_clients = 1;
  ASSERT_EQ(cluster->GetPilot()->GetMsgLoop()->GetNumClientsSync(),
            npilot - 1 + num_rollcall_clients);
  ASSERT_EQ(cluster->GetCopilot()->GetMsgLoop()->GetNumClientsSync(),
            ncopilot - 1 + num_rollcall_clients);
}

}  // namespace rocketspeed

int main(int argc, char** argv) {
  return rocketspeed::test::RunAllTests();
}
#include <stdio.h>
#include <unistd.h>
#include <gflags/gflags.h>
#include "include/Env.h"
#include "include/Rocketeer.h"
#include "include/RocketeerServer.h"

using namespace rocketspeed;

DEFINE_uint64(port, 5834, "port to listen on");
DEFINE_uint64(threads, 16, "number of threads");

/**
 * Rocketeer that just immediately rejects subscriptions.
 */
class RejectAllRocketeer : public Rocketeer {
 public:
  RejectAllRocketeer() {}

  void HandleNewSubscription(
      Flow* flow, InboundID inbound_id, SubscriptionParameters params)
      override {
    // Immediately terminate.
    Unsubscribe(flow, inbound_id, std::move(params.namespace_id),
        std::move(params.topic_name), UnsubscribeReason::Invalid);
  }

  void HandleUnsubscribe(
      Flow*, InboundID, NamespaceID, Topic, TerminationSource) override {}
};

int main(int argc, char** argv) {
  // Start AcceptAll Rocketeer listening on port supplied in flags.
  GFLAGS::ParseCommandLineFlags(&argc, &argv, true);
  rocketspeed::Env::InstallSignalHandlers();

  RocketeerOptions options;
  options.port = static_cast<uint16_t>(FLAGS_port);
  options.stats_prefix = "rejectall";

  auto server = RocketeerServer::Create(std::move(options));
  std::vector<RejectAllRocketeer> rocketeers(FLAGS_threads);
  for (auto& rocketeer : rocketeers) {
    server->Register(&rocketeer);
  }

  auto st = server->Start();
  if (!st.ok()) {
    fprintf(stderr, "Failed to start server: %s\n", st.ToString().c_str());
    return 1;
  }
  pause();
  server->Stop();
  return 0;
}

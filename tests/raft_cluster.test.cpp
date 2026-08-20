#include <catch2/catch_test_macros.hpp>

#include "raft/InProcessTransport.h"
#include "raft/MemoryRaftStorage.h"
#include "raft/RaftNode.h"
#include "storage/CachedKvStore.h"
#include "storage/InMemoryKvStore.h"
#include "storage/KvCodec.h"

#include <chrono>
#include <atomic>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

using namespace std::chrono_literals;
using cppcache::raft::ClientStatus;
using cppcache::raft::InProcessTransport;
using cppcache::raft::MemoryRaftStorage;
using cppcache::raft::NodeId;
using cppcache::raft::RaftNode;
using cppcache::raft::RaftNodeOptions;
using cppcache::raft::Role;
using cppcache::storage::CachedKvStore;
using cppcache::storage::InMemoryKvStore;
using cppcache::storage::KvCommand;
using cppcache::storage::KvOperation;
using cppcache::storage::KvStateMachine;

namespace {

bool waitUntil(const std::function<bool()> &condition,
               std::chrono::milliseconds timeout = 3s) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    if (condition())
      return true;
    std::this_thread::sleep_for(10ms);
  }
  return condition();
}

class ThreeNodeCluster {
public:
  explicit ThreeNodeCluster(std::size_t snapshotThreshold = 1000)
      : transport(std::make_shared<InProcessTransport>()) {
    RaftNodeOptions options;
    options.electionTimeout = 150ms;
    options.heartbeatInterval = 30ms;
    options.tickInterval = 5ms;
    options.snapshotThreshold = snapshotThreshold;

    for (NodeId id = 1; id <= 3; ++id) {
      std::vector<NodeId> peers;
      for (NodeId peer = 1; peer <= 3; ++peer) {
        if (peer != id)
          peers.push_back(peer);
      }
      auto storage = std::make_shared<MemoryRaftStorage>();
      storages[id] = storage;
      auto stateMachine =
          std::make_unique<KvStateMachine>(std::make_unique<CachedKvStore>(
              std::make_unique<InMemoryKvStore>(), 16));
      nodes[id] = std::make_unique<RaftNode>(id, peers, storage,
                                             std::move(stateMachine), options);
      nodes[id]->setTransport(transport);
      transport->registerNode(id, nodes[id].get());
    }
    for (auto &[id, node] : nodes) {
      (void)id;
      node->start();
    }
  }

  ~ThreeNodeCluster() {
    for (auto &[id, node] : nodes) {
      node->stop();
      transport->unregisterNode(id);
    }
  }

  std::optional<NodeId> leader(std::optional<NodeId> excluded = std::nullopt) {
    for (const auto &[id, node] : nodes) {
      if ((!excluded || id != *excluded) && node->role() == Role::Leader)
        return id;
    }
    return std::nullopt;
  }

  std::shared_ptr<InProcessTransport> transport;
  std::unordered_map<NodeId, std::shared_ptr<MemoryRaftStorage>> storages;
  std::unordered_map<NodeId, std::unique_ptr<RaftNode>> nodes;
};

class FailingStorage final : public cppcache::raft::IRaftStorage {
public:
  cppcache::raft::PersistedRaftState load() const override { return {}; }
  void save(const cppcache::raft::PersistedRaftState &) override {
    throw std::runtime_error("injected persistence failure");
  }
};

KvCommand put(std::string value, std::uint64_t requestId) {
  return {KvOperation::Put, "key", std::move(value), "client", requestId};
}

} // namespace

TEST_CASE("three-node Raft cluster replicates committed KV commands",
          "[raft][cluster]") {
  ThreeNodeCluster cluster;
  REQUIRE(waitUntil([&] { return cluster.leader().has_value(); }));
  const NodeId leader = *cluster.leader();

  const auto result = cluster.nodes.at(leader)->submit(put("value", 1), 2s);
  REQUIRE(result.status == ClientStatus::Committed);
  REQUIRE(waitUntil([&] {
    for (const auto &[id, node] : cluster.nodes) {
      (void)id;
      if (node->localRead("key") != "value")
        return false;
    }
    return true;
  }));

  NodeId follower = leader == 1 ? 2 : 1;
  REQUIRE(cluster.nodes.at(follower)->submit(put("ignored", 2), 100ms).status ==
          ClientStatus::NotLeader);
}

TEST_CASE("Raft elects a replacement leader after a partition",
          "[raft][cluster][partition]") {
  ThreeNodeCluster cluster;
  REQUIRE(waitUntil([&] { return cluster.leader().has_value(); }));
  const NodeId oldLeader = *cluster.leader();
  cluster.transport->isolate(oldLeader);

  REQUIRE(waitUntil([&] { return cluster.leader(oldLeader).has_value(); }, 4s));
  const NodeId newLeader = *cluster.leader(oldLeader);
  REQUIRE(cluster.nodes.at(newLeader)
              ->submit(put("after-failover", 1), 2s)
              .status == ClientStatus::Committed);
  REQUIRE(cluster.nodes.at(oldLeader)->read("key", 150ms).status ==
          ClientStatus::Timeout);

  cluster.transport->heal(oldLeader);
  REQUIRE(waitUntil([&] {
    return cluster.nodes.at(oldLeader)->localRead("key") == "after-failover";
  }));
  REQUIRE(cluster.nodes.at(oldLeader)->role() == Role::Follower);
}

TEST_CASE("Raft installs a snapshot on a lagging follower",
          "[raft][cluster][snapshot]") {
  ThreeNodeCluster cluster(3);
  REQUIRE(waitUntil([&] { return cluster.leader().has_value(); }));
  const NodeId leader = *cluster.leader();
  NodeId lagging = 3;
  if (lagging == leader)
    lagging = 2;
  cluster.transport->isolate(lagging);

  for (std::uint64_t request = 1; request <= 6; ++request) {
    REQUIRE(cluster.nodes.at(leader)
                ->submit(put("value-" + std::to_string(request), request), 2s)
                .status == ClientStatus::Committed);
  }
  REQUIRE(cluster.nodes.at(leader)->commitIndex() >= 7);

  cluster.transport->heal(lagging);
  REQUIRE(waitUntil([&] {
    return cluster.nodes.at(lagging)->localRead("key") == "value-6";
  }));
}

TEST_CASE("Raft worker survives repeated concurrent leader step-down",
          "[raft][cluster][regression]") {
  ThreeNodeCluster cluster(2);
  REQUIRE(waitUntil([&] { return cluster.leader().has_value(); }));

  std::atomic<bool> running{true};
  std::thread higherTermVotes([&] {
    cppcache::raft::Term term = 100;
    while (running.load(std::memory_order_relaxed)) {
      for (auto &[id, node] : cluster.nodes) {
        const NodeId candidate = id == 1 ? 2 : 1;
        (void)node->handleRequestVote(
            {term, candidate, node->commitIndex(), term});
      }
      ++term;
      std::this_thread::sleep_for(1ms);
    }
  });

  std::this_thread::sleep_for(500ms);
  running.store(false, std::memory_order_relaxed);
  higherTermVotes.join();

  for (const auto &[id, node] : cluster.nodes) {
    (void)id;
    REQUIRE(node->currentTerm() >= 100);
  }
}

TEST_CASE("Raft worker reports failures and can be restarted safely",
          "[raft][cluster][regression]") {
  RaftNodeOptions options;
  options.electionTimeout = 10ms;
  options.tickInterval = 1ms;
  auto node = std::make_unique<RaftNode>(
      1, std::vector<NodeId>{}, std::make_shared<FailingStorage>(),
      std::make_unique<KvStateMachine>(std::make_unique<InMemoryKvStore>()),
      options);
  node->setTransport(std::make_shared<InProcessTransport>());

  node->start();
  REQUIRE(waitUntil([&] { return node->workerFailure().has_value(); }, 500ms));
  REQUIRE(*node->workerFailure() == "injected persistence failure");

  node->start();
  REQUIRE(waitUntil([&] { return node->workerFailure().has_value(); }, 500ms));

  std::thread firstStop([&] { node->stop(); });
  std::thread secondStop([&] { node->stop(); });
  firstStop.join();
  secondStop.join();
}

TEST_CASE("Raft validates snapshots before changing durable progress",
          "[raft][snapshot][regression]") {
  auto node = std::make_unique<RaftNode>(
      1, std::vector<NodeId>{}, std::make_shared<MemoryRaftStorage>(),
      std::make_unique<KvStateMachine>(std::make_unique<InMemoryKvStore>()));

  cppcache::storage::KvSnapshot oversized;
  for (std::size_t client = 1;
       client <= KvStateMachine::kMaxTrackedClients + 1; ++client) {
    oversized.clients.emplace(
        "client-" + std::to_string(client),
        cppcache::storage::ClientRequestRecord{1, client});
  }
  oversized.lastApplySequence = oversized.clients.size();
  const auto encoded = cppcache::storage::encodeSnapshot(oversized);

  REQUIRE_THROWS(node->handleInstallSnapshot({1, 2, 5, 1, encoded}));
  REQUIRE(node->currentTerm() == 0);
  REQUIRE(node->commitIndex() == 0);
  REQUIRE_FALSE(node->localRead("key"));
}

TEST_CASE("KV command and snapshot codecs reject ambiguity", "[raft][codec]") {
  const KvCommand original{KvOperation::Append, std::string("a\0key", 5),
                           std::string("value\0data", 10), "client", 42};
  const auto encodedCommand = cppcache::storage::encodeCommand(original);
  const auto decodedCommand = cppcache::storage::decodeCommand(encodedCommand);
  REQUIRE(decodedCommand.operation == original.operation);
  REQUIRE(decodedCommand.key == original.key);
  REQUIRE(decodedCommand.value == original.value);
  REQUIRE(decodedCommand.clientId == original.clientId);
  REQUIRE(decodedCommand.requestId == original.requestId);

  cppcache::storage::KvSnapshot snapshot;
  snapshot.entries.emplace("key", "value");
  snapshot.clients.emplace(
      "client", cppcache::storage::ClientRequestRecord{42, 1});
  snapshot.lastApplySequence = 1;
  const auto decodedSnapshot = cppcache::storage::decodeSnapshot(
      cppcache::storage::encodeSnapshot(snapshot));
  REQUIRE(decodedSnapshot.entries == snapshot.entries);
  REQUIRE(decodedSnapshot.clients == snapshot.clients);
  REQUIRE(decodedSnapshot.lastApplySequence == snapshot.lastApplySequence);
  REQUIRE_THROWS(cppcache::storage::decodeCommand("truncated"));
}

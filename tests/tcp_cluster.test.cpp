#include <catch2/catch_test_macros.hpp>

#include "raft/MemoryRaftStorage.h"
#include "raft/RaftNode.h"
#include "raft/TcpKvClient.h"
#include "raft/TcpTransport.h"
#include "storage/CachedKvStore.h"
#include "storage/InMemoryKvStore.h"

#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

using namespace std::chrono_literals;
using cppcache::raft::ClientStatus;
using cppcache::raft::Endpoint;
using cppcache::raft::MemoryRaftStorage;
using cppcache::raft::NodeId;
using cppcache::raft::RaftNode;
using cppcache::raft::RaftNodeOptions;
using cppcache::raft::Role;
using cppcache::raft::TcpKvClient;
using cppcache::raft::TcpTransport;
using cppcache::storage::CachedKvStore;
using cppcache::storage::InMemoryKvStore;
using cppcache::storage::KvStateMachine;

namespace {

std::uint16_t availablePort() {
  const int socket = ::socket(AF_INET, SOCK_STREAM, 0);
  if (socket < 0)
    throw std::runtime_error("failed to create port probe socket");
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  address.sin_port = 0;
  if (::bind(socket, reinterpret_cast<sockaddr *>(&address), sizeof(address)) !=
      0) {
    ::close(socket);
    throw std::runtime_error("failed to bind port probe socket");
  }
  socklen_t size = sizeof(address);
  if (::getsockname(socket, reinterpret_cast<sockaddr *>(&address), &size) !=
      0) {
    ::close(socket);
    throw std::runtime_error("failed to inspect port probe socket");
  }
  const std::uint16_t port = ntohs(address.sin_port);
  ::close(socket);
  return port;
}

bool waitUntil(const std::function<bool()> &condition,
               std::chrono::milliseconds timeout = 4s) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    if (condition())
      return true;
    std::this_thread::sleep_for(10ms);
  }
  return condition();
}

class TcpCluster {
public:
  TcpCluster() {
    for (NodeId id = 1; id <= 3; ++id)
      endpoints[id] = {"127.0.0.1", availablePort()};

    RaftNodeOptions options;
    options.electionTimeout = 200ms;
    options.heartbeatInterval = 40ms;
    options.tickInterval = 5ms;
    options.snapshotThreshold = 4;

    for (NodeId id = 1; id <= 3; ++id) {
      std::vector<NodeId> peerIds;
      std::unordered_map<NodeId, Endpoint> peerEndpoints;
      for (NodeId peer = 1; peer <= 3; ++peer) {
        if (peer == id)
          continue;
        peerIds.push_back(peer);
        peerEndpoints.emplace(peer, endpoints.at(peer));
      }
      auto stateMachine =
          std::make_unique<KvStateMachine>(std::make_unique<CachedKvStore>(
              std::make_unique<InMemoryKvStore>(), 16));
      nodes[id] = std::make_unique<RaftNode>(
          id, peerIds, std::make_shared<MemoryRaftStorage>(),
          std::move(stateMachine), options);
      transports[id] = std::make_shared<TcpTransport>(
          id, endpoints.at(id), std::move(peerEndpoints), 150ms);
      transports[id]->attach(nodes[id].get());
      nodes[id]->setTransport(transports[id]);
    }
    for (auto &[id, transport] : transports) {
      (void)id;
      transport->start();
    }
    for (auto &[id, node] : nodes) {
      (void)id;
      node->start();
    }
  }

  ~TcpCluster() {
    for (auto &[id, node] : nodes) {
      (void)id;
      node->stop();
    }
    for (auto &[id, transport] : transports) {
      (void)id;
      transport->stop();
    }
  }

  std::optional<NodeId> leader(std::optional<NodeId> excluded = std::nullopt) {
    for (const auto &[id, node] : nodes) {
      if ((!excluded || id != *excluded) && node->role() == Role::Leader)
        return id;
    }
    return std::nullopt;
  }

  std::vector<Endpoint> serverEndpoints() const {
    return {endpoints.at(1), endpoints.at(2), endpoints.at(3)};
  }

  void stopNode(NodeId id) {
    nodes.at(id)->stop();
    transports.at(id)->stop();
  }

  std::unordered_map<NodeId, Endpoint> endpoints;
  std::unordered_map<NodeId, std::unique_ptr<RaftNode>> nodes;
  std::unordered_map<NodeId, std::shared_ptr<TcpTransport>> transports;
};

} // namespace

TEST_CASE("TCP Raft cluster serves replicated KV operations",
          "[raft][tcp][integration]") {
  TcpCluster cluster;
  REQUIRE(waitUntil([&] { return cluster.leader().has_value(); }));
  TcpKvClient client(cluster.serverEndpoints(), 1s);

  REQUIRE(client.put("key", "a", "tcp-client", 1).status ==
          ClientStatus::Committed);
  REQUIRE(client.append("key", "b", "tcp-client", 2).status ==
          ClientStatus::Committed);
  const auto value = client.get("key");
  REQUIRE(value.status == ClientStatus::Committed);
  REQUIRE(value.value == "ab");

  REQUIRE(client.erase("key", "tcp-client", 3).status ==
          ClientStatus::Committed);
  const auto missing = client.get("key");
  REQUIRE(missing.status == ClientStatus::Committed);
  REQUIRE_FALSE(missing.value);
}

TEST_CASE("TCP client finds the replacement leader after node failure",
          "[raft][tcp][integration]") {
  TcpCluster cluster;
  REQUIRE(waitUntil([&] { return cluster.leader().has_value(); }));
  TcpKvClient client(cluster.serverEndpoints(), 1s);
  REQUIRE(client.put("key", "before", "tcp-client", 1).status ==
          ClientStatus::Committed);

  const NodeId oldLeader = *cluster.leader();
  cluster.stopNode(oldLeader);
  REQUIRE(waitUntil([&] { return cluster.leader(oldLeader).has_value(); }));

  REQUIRE(client.put("key", "after", "tcp-client", 2).status ==
          ClientStatus::Committed);
  REQUIRE(client.get("key").value == "after");
}

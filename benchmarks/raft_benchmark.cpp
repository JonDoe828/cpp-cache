#include "raft/InProcessTransport.h"
#include "raft/MemoryRaftStorage.h"
#include "raft/RaftNode.h"
#include "storage/CachedKvStore.h"
#include "storage/InMemoryKvStore.h"
#include "storage/KvStateMachine.h"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <functional>
#include <iomanip>
#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {

using namespace std::chrono_literals;
using cppcache::raft::ClientStatus;
using cppcache::raft::InProcessTransport;
using cppcache::raft::MemoryRaftStorage;
using cppcache::raft::NodeId;
using cppcache::raft::RaftNode;
using cppcache::raft::Role;
using cppcache::storage::CachedKvStore;
using cppcache::storage::InMemoryKvStore;
using cppcache::storage::KvCommand;
using cppcache::storage::KvOperation;
using cppcache::storage::KvStateMachine;

struct Options {
  std::size_t operations{2000};
  std::size_t rounds{5};
  std::size_t failovers{7};
};

bool waitUntil(const std::function<bool()> &condition,
               std::chrono::milliseconds timeout = 3s) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    if (condition())
      return true;
    std::this_thread::sleep_for(1ms);
  }
  return condition();
}

class ThreeNodeCluster {
public:
  ThreeNodeCluster() : transport_(std::make_shared<InProcessTransport>()) {
    for (NodeId id = 1; id <= 3; ++id) {
      std::vector<NodeId> peers;
      for (NodeId peer = 1; peer <= 3; ++peer) {
        if (peer != id)
          peers.push_back(peer);
      }

      auto stateMachine =
          std::make_unique<KvStateMachine>(std::make_unique<CachedKvStore>(
              std::make_unique<InMemoryKvStore>(), 256));
      nodes_[id] = std::make_unique<RaftNode>(
          id, std::move(peers), std::make_shared<MemoryRaftStorage>(),
          std::move(stateMachine));
      nodes_[id]->setTransport(transport_);
      transport_->registerNode(id, nodes_[id].get());
    }

    for (auto &[id, node] : nodes_) {
      (void)id;
      node->start();
    }
  }

  ~ThreeNodeCluster() {
    for (auto &[id, node] : nodes_) {
      (void)id;
      node->stop();
    }
    for (const auto &[id, node] : nodes_) {
      (void)node;
      transport_->unregisterNode(id);
    }
  }

  ThreeNodeCluster(const ThreeNodeCluster &) = delete;
  ThreeNodeCluster &operator=(const ThreeNodeCluster &) = delete;

  std::optional<NodeId>
  leader(std::optional<NodeId> excluded = std::nullopt) const {
    for (const auto &[id, node] : nodes_) {
      if ((!excluded || id != *excluded) && node->role() == Role::Leader)
        return id;
    }
    return std::nullopt;
  }

  RaftNode &node(NodeId id) { return *nodes_.at(id); }
  const auto &nodes() const { return nodes_; }
  InProcessTransport &transport() { return *transport_; }

private:
  std::shared_ptr<InProcessTransport> transport_;
  std::unordered_map<NodeId, std::unique_ptr<RaftNode>> nodes_;
};

std::size_t parsePositiveCount(std::string_view text,
                               std::string_view optionName) {
  std::size_t value = 0;
  const char *begin = text.data();
  const char *end = begin + text.size();
  const auto [position, error] = std::from_chars(begin, end, value);
  if (error != std::errc{} || position != end || value == 0)
    throw std::invalid_argument(std::string(optionName) +
                                " must be a positive integer");
  return value;
}

Options parseOptions(int argc, char **argv) {
  Options options;
  for (int index = 1; index < argc; ++index) {
    const std::string_view option = argv[index];
    if (option == "--help") {
      std::cout << "Usage: raft_benchmark [--operations N] [--rounds N] "
                   "[--failovers N]\n";
      std::exit(0);
    }
    if (index + 1 >= argc)
      throw std::invalid_argument(std::string(option) + " requires a value");

    const std::string_view value = argv[++index];
    if (option == "--operations")
      options.operations = parsePositiveCount(value, option);
    else if (option == "--rounds")
      options.rounds = parsePositiveCount(value, option);
    else if (option == "--failovers")
      options.failovers = parsePositiveCount(value, option);
    else
      throw std::invalid_argument("unknown option: " + std::string(option));
  }
  return options;
}

NodeId waitForLeader(const ThreeNodeCluster &cluster,
                     std::optional<NodeId> excluded = std::nullopt) {
  std::optional<NodeId> leader;
  if (!waitUntil([&] {
        leader = cluster.leader(excluded);
        return leader.has_value();
      }))
    throw std::runtime_error("cluster did not elect a leader before timeout");
  return *leader;
}

KvCommand put(std::string key, std::string value, std::string clientId,
              std::uint64_t requestId) {
  return {KvOperation::Put, std::move(key), std::move(value),
          std::move(clientId), requestId};
}

void requireCommitted(const cppcache::raft::SubmitResult &result) {
  if (result.status != ClientStatus::Committed || !result.result ||
      !result.result->applied)
    throw std::runtime_error("benchmark write was not committed and applied");
}

double runThroughputRound(std::size_t operations) {
  ThreeNodeCluster cluster;
  const NodeId leader = waitForLeader(cluster);
  constexpr std::size_t warmupOperations = 100;
  std::uint64_t requestId = 1;

  for (std::size_t index = 0; index < warmupOperations; ++index) {
    requireCommitted(cluster.node(leader).submit(
        put("warmup-" + std::to_string(index % 64), "value",
            "throughput-client", requestId++),
        2s));
  }

  std::string finalKey;
  std::string finalValue;
  const auto started = std::chrono::steady_clock::now();
  for (std::size_t index = 0; index < operations; ++index) {
    finalKey = "key-" + std::to_string(index % 1024);
    finalValue = "value-" + std::to_string(index);
    requireCommitted(cluster.node(leader).submit(
        put(finalKey, finalValue, "throughput-client", requestId++), 2s));
  }
  const auto elapsed = std::chrono::steady_clock::now() - started;

  if (!waitUntil([&] {
        return std::all_of(cluster.nodes().begin(), cluster.nodes().end(),
                           [&](const auto &entry) {
                             return entry.second->localRead(finalKey) ==
                                    finalValue;
                           });
      }))
    throw std::runtime_error("committed value did not reach every node");

  const double seconds = std::chrono::duration<double>(elapsed).count();
  return static_cast<double>(operations) / seconds;
}

double runFailoverRound(std::size_t round) {
  ThreeNodeCluster cluster;
  const NodeId oldLeader = waitForLeader(cluster);
  requireCommitted(cluster.node(oldLeader).submit(
      put("failover-key", "before", "failover-warmup", 1), 2s));

  const auto started = std::chrono::steady_clock::now();
  cluster.transport().isolate(oldLeader);
  const NodeId newLeader = waitForLeader(cluster, oldLeader);
  requireCommitted(cluster.node(newLeader).submit(
      put("failover-key", "after", "failover-client-" + std::to_string(round),
          1),
      2s));
  const auto elapsed = std::chrono::steady_clock::now() - started;

  cluster.transport().heal(oldLeader);
  return std::chrono::duration<double, std::milli>(elapsed).count();
}

double percentile(std::vector<double> samples, double fraction) {
  std::sort(samples.begin(), samples.end());
  const auto rank = static_cast<std::size_t>(
      std::ceil(fraction * static_cast<double>(samples.size())));
  return samples[std::max<std::size_t>(1, rank) - 1];
}

void printSamples(std::string_view label, const std::vector<double> &samples,
                  std::string_view unit) {
  std::cout << label << '\n';
  for (std::size_t index = 0; index < samples.size(); ++index)
    std::cout << "  round " << index + 1 << ": " << samples[index] << ' '
              << unit << '\n';
  std::cout << "  p50: " << percentile(samples, 0.50) << ' ' << unit << '\n'
            << "  p95: " << percentile(samples, 0.95) << ' ' << unit << '\n';
}

} // namespace

int main(int argc, char **argv) {
  try {
    const Options options = parseOptions(argc, argv);
    std::cout << std::fixed << std::setprecision(2) << "configuration\n"
              << "  nodes: 3\n"
              << "  transport: in-process\n"
              << "  storage: memory\n"
              << "  election timeout: 300-600 ms\n"
              << "  snapshot threshold: 1000 entries\n"
              << "  operations per throughput round: " << options.operations
              << "\n\n";

    std::vector<double> throughputSamples;
    throughputSamples.reserve(options.rounds);
    for (std::size_t round = 0; round < options.rounds; ++round)
      throughputSamples.push_back(runThroughputRound(options.operations));
    printSamples("sequential committed write throughput", throughputSamples,
                 "ops/s");

    std::cout << '\n';
    std::vector<double> failoverSamples;
    failoverSamples.reserve(options.failovers);
    for (std::size_t round = 0; round < options.failovers; ++round)
      failoverSamples.push_back(runFailoverRound(round));
    printSamples("leader isolation to replacement write commit",
                 failoverSamples, "ms");
  } catch (const std::exception &error) {
    std::cerr << "benchmark failed: " << error.what() << '\n';
    return 1;
  }
  return 0;
}

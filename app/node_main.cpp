#include "raft/FileRaftStorage.h"
#include "raft/RaftNode.h"
#include "raft/TcpTransport.h"
#include "storage/CachedKvStore.h"
#include "storage/InMemoryKvStore.h"

#include <csignal>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace {

volatile std::sig_atomic_t stopRequested = 0;

void requestStop(int) { stopRequested = 1; }

cppcache::raft::Endpoint parseEndpoint(const std::string &text) {
  const auto separator = text.rfind(':');
  if (separator == std::string::npos || separator == 0 ||
      separator + 1 == text.size())
    throw std::invalid_argument("endpoint must have host:port form");
  const auto port = std::stoul(text.substr(separator + 1));
  if (port == 0 || port > 65535)
    throw std::invalid_argument("endpoint port is out of range");
  return {text.substr(0, separator), static_cast<std::uint16_t>(port)};
}

struct Configuration {
  cppcache::raft::NodeId id{0};
  cppcache::raft::Endpoint listen;
  std::unordered_map<cppcache::raft::NodeId, cppcache::raft::Endpoint> peers;
  std::string dataPath;
  std::size_t cacheCapacity{1024};
  std::size_t snapshotThreshold{1000};
};

Configuration parseArguments(int argc, char **argv) {
  Configuration configuration;
  for (int index = 1; index < argc; ++index) {
    const std::string option = argv[index];
    if (index + 1 >= argc)
      throw std::invalid_argument("missing value for " + option);
    const std::string value = argv[++index];
    if (option == "--id") {
      configuration.id = static_cast<cppcache::raft::NodeId>(std::stoul(value));
    } else if (option == "--listen") {
      configuration.listen = parseEndpoint(value);
    } else if (option == "--peer") {
      const auto separator = value.find('=');
      if (separator == std::string::npos)
        throw std::invalid_argument("peer must have id=host:port form");
      const auto id = static_cast<cppcache::raft::NodeId>(
          std::stoul(value.substr(0, separator)));
      configuration.peers[id] = parseEndpoint(value.substr(separator + 1));
    } else if (option == "--data") {
      configuration.dataPath = value;
    } else if (option == "--cache-capacity") {
      configuration.cacheCapacity = std::stoull(value);
    } else if (option == "--snapshot-threshold") {
      configuration.snapshotThreshold = std::stoull(value);
    } else {
      throw std::invalid_argument("unknown option: " + option);
    }
  }
  if (configuration.id == 0 || configuration.listen.port == 0 ||
      configuration.dataPath.empty())
    throw std::invalid_argument("--id, --listen and --data are required");
  configuration.peers.erase(configuration.id);
  return configuration;
}

} // namespace

int main(int argc, char **argv) {
  try {
    const Configuration configuration = parseArguments(argc, argv);
    std::vector<cppcache::raft::NodeId> peerIds;
    peerIds.reserve(configuration.peers.size());
    for (const auto &[id, endpoint] : configuration.peers) {
      (void)endpoint;
      peerIds.push_back(id);
    }

    auto raftStorage = std::make_shared<cppcache::raft::FileRaftStorage>(
        configuration.dataPath);
    auto stateMachine = std::make_unique<cppcache::storage::KvStateMachine>(
        std::make_unique<cppcache::storage::CachedKvStore>(
            std::make_unique<cppcache::storage::InMemoryKvStore>(),
            configuration.cacheCapacity));
    cppcache::raft::RaftNodeOptions options;
    options.snapshotThreshold = configuration.snapshotThreshold;
    cppcache::raft::RaftNode node(configuration.id, peerIds, raftStorage,
                                  std::move(stateMachine), options);
    auto transport = std::make_shared<cppcache::raft::TcpTransport>(
        configuration.id, configuration.listen, configuration.peers);
    transport->attach(&node);
    node.setTransport(transport);

    std::signal(SIGINT, requestStop);
    std::signal(SIGTERM, requestStop);
    transport->start();
    node.start();
    std::cout << "node " << configuration.id << " listening on "
              << configuration.listen.host << ':' << configuration.listen.port
              << '\n';
    while (!stopRequested && !node.workerFailure())
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    node.stop();
    transport->stop();
    if (const auto failure = node.workerFailure()) {
      std::cerr << "node worker failed: " << *failure << '\n';
      return 1;
    }
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "node error: " << error.what() << '\n';
    return 1;
  }
}

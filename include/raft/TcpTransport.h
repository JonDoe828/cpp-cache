#pragma once

#include "raft/IRaftTransport.h"
#include "raft/TcpEndpoint.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace cppcache::raft {

class RaftNode;

class TcpTransport final : public IRaftTransport {
public:
  TcpTransport(
      NodeId localId, Endpoint listenEndpoint,
      std::unordered_map<NodeId, Endpoint> peers,
      std::chrono::milliseconds rpcTimeout = std::chrono::milliseconds(300));
  ~TcpTransport() override;

  void attach(RaftNode *node);
  void start();
  void stop();

  std::optional<RequestVoteResponse>
  requestVote(NodeId target, const RequestVoteRequest &request) override;
  std::optional<AppendEntriesResponse>
  appendEntries(NodeId target, const AppendEntriesRequest &request) override;
  std::optional<InstallSnapshotResponse>
  installSnapshot(NodeId target,
                  const InstallSnapshotRequest &request) override;

private:
  std::optional<std::string> call(NodeId target, std::uint8_t type,
                                  const std::string &payload,
                                  std::uint8_t expectedResponse);
  void acceptLoop();
  void workerLoop();
  void clientWorkerLoop();
  bool handleConnection(int socket);
  void handleClientRequest(int socket, const std::string &payload);

  NodeId localId_;
  Endpoint listenEndpoint_;
  std::unordered_map<NodeId, Endpoint> peers_;
  std::chrono::milliseconds rpcTimeout_;
  RaftNode *node_{nullptr};

  std::atomic<bool> running_{false};
  int listenSocket_{-1};
  std::thread acceptThread_;
  std::vector<std::thread> workers_;
  std::mutex queueMutex_;
  std::condition_variable queueCondition_;
  std::deque<int> connections_;

  struct ClientConnection {
    int socket;
    std::string payload;
  };
  std::vector<std::thread> clientWorkers_;
  std::mutex clientQueueMutex_;
  std::condition_variable clientQueueCondition_;
  std::deque<ClientConnection> clientConnections_;
};

} // namespace cppcache::raft

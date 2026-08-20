#pragma once

#include "raft/IRaftTransport.h"
#include "raft/RaftCore.h"
#include "storage/KvStateMachine.h"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace cppcache::raft {

struct RaftNodeOptions {
  std::chrono::milliseconds electionTimeout{300};
  std::chrono::milliseconds heartbeatInterval{50};
  std::chrono::milliseconds tickInterval{10};
  std::size_t snapshotThreshold{1000};
};

enum class ClientStatus { Committed, NotLeader, Timeout };

struct SubmitResult {
  ClientStatus status{ClientStatus::Timeout};
  std::optional<NodeId> leaderId;
  std::optional<storage::ApplyResult> result;
};

struct ReadResult {
  ClientStatus status{ClientStatus::Timeout};
  std::optional<NodeId> leaderId;
  std::optional<std::string> value;
};

class RaftNode {
public:
  RaftNode(NodeId id, std::vector<NodeId> peers,
           std::shared_ptr<IRaftStorage> raftStorage,
           std::unique_ptr<storage::KvStateMachine> stateMachine,
           RaftNodeOptions options = {});
  ~RaftNode();

  RaftNode(const RaftNode &) = delete;
  RaftNode &operator=(const RaftNode &) = delete;

  void setTransport(std::shared_ptr<IRaftTransport> transport);
  void start();
  void stop();
  std::optional<std::string> workerFailure() const;

  RequestVoteResponse handleRequestVote(const RequestVoteRequest &request);
  AppendEntriesResponse
  handleAppendEntries(const AppendEntriesRequest &request);
  InstallSnapshotResponse
  handleInstallSnapshot(const InstallSnapshotRequest &request);

  SubmitResult submit(const storage::KvCommand &command,
                      std::chrono::milliseconds timeout);
  ReadResult read(const std::string &key, std::chrono::milliseconds timeout);

  NodeId id() const { return id_; }
  Role role() const { return core_.role(); }
  Term currentTerm() const { return core_.currentTerm(); }
  std::optional<NodeId> leaderId() const { return core_.leaderId(); }
  LogIndex commitIndex() const { return core_.commitIndex(); }
  std::optional<std::string> localRead(const std::string &key);

private:
  void run();
  void runElection();
  std::size_t replicateAll();
  bool replicatePeer(NodeId peer);
  void applyCommitted();
  void resetElectionDeadline();
  void recordWorkerFailure(std::string message);
  std::shared_ptr<IRaftTransport> transport() const;

  NodeId id_;
  std::vector<NodeId> peers_;
  RaftCore core_;
  std::unique_ptr<storage::KvStateMachine> stateMachine_;
  RaftNodeOptions options_;

  mutable std::mutex transportMutex_;
  std::shared_ptr<IRaftTransport> transport_;
  std::mutex lifecycleMutex_;
  std::atomic<bool> running_{false};
  std::thread worker_;
  mutable std::mutex workerFailureMutex_;
  std::optional<std::string> workerFailure_;
  std::atomic<std::int64_t> electionDeadlineMillis_{0};
  std::chrono::steady_clock::time_point lastHeartbeat_{};

  std::mutex replicationMutex_;
  std::mutex applyMutex_;
  std::unordered_set<LogIndex> pendingResults_;
  std::unordered_map<LogIndex, storage::ApplyResult> applyResults_;
  LogIndex appliedIndex_{0};

  std::mutex electionRandomMutex_;
  std::mt19937_64 electionRandom_;
};

} // namespace cppcache::raft

#pragma once

#include "raft/IRaftStorage.h"
#include "raft/RaftTypes.h"

#include <memory>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace cppcache::raft {

class RaftCore {
public:
  RaftCore(NodeId id, std::vector<NodeId> peers,
           std::shared_ptr<IRaftStorage> storage);

  RequestVoteRequest startElection();
  RequestVoteResponse handleRequestVote(const RequestVoteRequest &request);
  bool recordVote(NodeId peer, const RequestVoteResponse &response);

  std::optional<AppendEntriesRequest> makeAppendEntries(NodeId peer) const;
  AppendEntriesResponse
  handleAppendEntries(const AppendEntriesRequest &request);
  void recordAppendEntriesResponse(NodeId peer,
                                   const AppendEntriesResponse &response);

  std::optional<InstallSnapshotRequest> makeInstallSnapshot(NodeId peer) const;
  InstallSnapshotResponse
  handleInstallSnapshot(const InstallSnapshotRequest &request);
  void recordInstallSnapshotResponse(NodeId peer,
                                     const InstallSnapshotResponse &response,
                                     LogIndex installedIndex);

  std::optional<LogIndex> append(const storage::KvCommand &command);
  std::vector<CommittedEntry> takeCommitted();
  void compact(LogIndex lastIncludedIndex, const std::string &snapshot);

  void becomeFollower(Term term, std::optional<NodeId> leader = std::nullopt);
  Role role() const;
  Term currentTerm() const;
  std::optional<NodeId> leaderId() const;
  LogIndex commitIndex() const;
  LogIndex lastLogIndex() const;
  LogIndex snapshotIndex() const;
  std::string snapshot() const;

private:
  Term termAt(LogIndex index) const;
  std::size_t offsetOf(LogIndex index) const;
  LogIndex lastLogIndexLocked() const;
  Term lastLogTermLocked() const;
  void persistLocked();
  void stepDownLocked(Term term, std::optional<NodeId> leader);
  void becomeLeaderLocked();
  void advanceCommitIndexLocked();
  bool hasMajority(std::size_t votes) const;

  NodeId id_;
  std::vector<NodeId> peers_;
  std::shared_ptr<IRaftStorage> storage_;

  mutable std::mutex mutex_;
  Term currentTerm_{0};
  std::optional<NodeId> votedFor_;
  Role role_{Role::Follower};
  std::optional<NodeId> leaderId_;
  std::vector<LogEntry> log_;
  LogIndex snapshotIndex_{0};
  Term snapshotTerm_{0};
  std::string snapshot_;
  LogIndex commitIndex_{0};
  LogIndex lastApplied_{0};

  std::unordered_set<NodeId> votes_;
  std::unordered_map<NodeId, LogIndex> nextIndex_;
  std::unordered_map<NodeId, LogIndex> matchIndex_;
};

} // namespace cppcache::raft

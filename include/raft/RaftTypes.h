#pragma once

#include "storage/KvCommand.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace cppcache::raft {

using NodeId = std::uint32_t;
using Term = std::uint64_t;
using LogIndex = std::uint64_t;

enum class Role { Follower, Candidate, Leader };

struct LogEntry {
  Term term{0};
  storage::KvCommand command{};
};

struct RequestVoteRequest {
  Term term{0};
  NodeId candidateId{0};
  LogIndex lastLogIndex{0};
  Term lastLogTerm{0};
};

struct RequestVoteResponse {
  Term term{0};
  bool voteGranted{false};
};

struct AppendEntriesRequest {
  Term term{0};
  NodeId leaderId{0};
  LogIndex previousLogIndex{0};
  Term previousLogTerm{0};
  std::vector<LogEntry> entries;
  LogIndex leaderCommit{0};
};

struct AppendEntriesResponse {
  Term term{0};
  bool success{false};
  LogIndex matchIndex{0};
  LogIndex nextIndexHint{1};
};

struct InstallSnapshotRequest {
  Term term{0};
  NodeId leaderId{0};
  LogIndex lastIncludedIndex{0};
  Term lastIncludedTerm{0};
  std::string snapshot;
};

struct InstallSnapshotResponse {
  Term term{0};
  bool success{false};
};

struct PersistedRaftState {
  Term currentTerm{0};
  std::optional<NodeId> votedFor;
  LogIndex snapshotIndex{0};
  Term snapshotTerm{0};
  std::string snapshot;
  LogIndex commitIndex{0};
  std::vector<LogEntry> log;
};

struct CommittedEntry {
  LogIndex index{0};
  storage::KvCommand command;
};

} // namespace cppcache::raft

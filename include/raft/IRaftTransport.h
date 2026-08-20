#pragma once

#include "raft/RaftTypes.h"

#include <optional>

namespace cppcache::raft {

class IRaftTransport {
public:
  virtual ~IRaftTransport() = default;

  virtual std::optional<RequestVoteResponse>
  requestVote(NodeId target, const RequestVoteRequest &request) = 0;
  virtual std::optional<AppendEntriesResponse>
  appendEntries(NodeId target, const AppendEntriesRequest &request) = 0;
  virtual std::optional<InstallSnapshotResponse>
  installSnapshot(NodeId target, const InstallSnapshotRequest &request) = 0;
};

} // namespace cppcache::raft

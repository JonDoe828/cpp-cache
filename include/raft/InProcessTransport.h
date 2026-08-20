#pragma once

#include "raft/IRaftTransport.h"

#include <mutex>
#include <set>
#include <unordered_map>
#include <utility>

namespace cppcache::raft {

class RaftNode;

class InProcessTransport final : public IRaftTransport {
public:
  void registerNode(NodeId id, RaftNode *node);
  void unregisterNode(NodeId id);
  void disconnect(NodeId first, NodeId second);
  void reconnect(NodeId first, NodeId second);
  void isolate(NodeId id);
  void heal(NodeId id);

  std::optional<RequestVoteResponse>
  requestVote(NodeId target, const RequestVoteRequest &request) override;
  std::optional<AppendEntriesResponse>
  appendEntries(NodeId target, const AppendEntriesRequest &request) override;
  std::optional<InstallSnapshotResponse>
  installSnapshot(NodeId target,
                  const InstallSnapshotRequest &request) override;

private:
  using Link = std::pair<NodeId, NodeId>;

  static Link link(NodeId first, NodeId second);
  RaftNode *resolve(NodeId source, NodeId target);

  std::mutex mutex_;
  std::unordered_map<NodeId, RaftNode *> nodes_;
  std::set<Link> disconnected_;
};

} // namespace cppcache::raft

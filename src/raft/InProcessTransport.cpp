#include "raft/InProcessTransport.h"

#include "raft/RaftNode.h"

#include <algorithm>

namespace cppcache::raft {

void InProcessTransport::registerNode(NodeId id, RaftNode *node) {
  std::lock_guard<std::mutex> lock(mutex_);
  nodes_[id] = node;
}

void InProcessTransport::unregisterNode(NodeId id) {
  std::lock_guard<std::mutex> lock(mutex_);
  nodes_.erase(id);
}

void InProcessTransport::disconnect(NodeId first, NodeId second) {
  std::lock_guard<std::mutex> lock(mutex_);
  disconnected_.insert(link(first, second));
}

void InProcessTransport::reconnect(NodeId first, NodeId second) {
  std::lock_guard<std::mutex> lock(mutex_);
  disconnected_.erase(link(first, second));
}

void InProcessTransport::isolate(NodeId id) {
  std::lock_guard<std::mutex> lock(mutex_);
  for (const auto &[peer, node] : nodes_) {
    (void)node;
    if (peer != id)
      disconnected_.insert(link(id, peer));
  }
}

void InProcessTransport::heal(NodeId id) {
  std::lock_guard<std::mutex> lock(mutex_);
  for (auto it = disconnected_.begin(); it != disconnected_.end();) {
    if (it->first == id || it->second == id)
      it = disconnected_.erase(it);
    else
      ++it;
  }
}

std::optional<RequestVoteResponse>
InProcessTransport::requestVote(NodeId target,
                                const RequestVoteRequest &request) {
  RaftNode *node = resolve(request.candidateId, target);
  if (!node)
    return std::nullopt;
  return node->handleRequestVote(request);
}

std::optional<AppendEntriesResponse>
InProcessTransport::appendEntries(NodeId target,
                                  const AppendEntriesRequest &request) {
  RaftNode *node = resolve(request.leaderId, target);
  if (!node)
    return std::nullopt;
  return node->handleAppendEntries(request);
}

std::optional<InstallSnapshotResponse>
InProcessTransport::installSnapshot(NodeId target,
                                    const InstallSnapshotRequest &request) {
  RaftNode *node = resolve(request.leaderId, target);
  if (!node)
    return std::nullopt;
  return node->handleInstallSnapshot(request);
}

InProcessTransport::Link InProcessTransport::link(NodeId first, NodeId second) {
  return std::minmax(first, second);
}

RaftNode *InProcessTransport::resolve(NodeId source, NodeId target) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (disconnected_.find(link(source, target)) != disconnected_.end())
    return nullptr;
  auto it = nodes_.find(target);
  return it == nodes_.end() ? nullptr : it->second;
}

} // namespace cppcache::raft

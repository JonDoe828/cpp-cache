#include "raft/RaftCore.h"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace cppcache::raft {

RaftCore::RaftCore(NodeId id, std::vector<NodeId> peers,
                   std::shared_ptr<IRaftStorage> storage)
    : id_(id), storage_(std::move(storage)) {
  if (!storage_)
    throw std::invalid_argument("raft storage must not be null");

  std::unordered_set<NodeId> uniquePeers;
  for (const NodeId peer : peers) {
    if (peer != id_ && uniquePeers.insert(peer).second)
      peers_.push_back(peer);
  }

  PersistedRaftState state = storage_->load();
  currentTerm_ = state.currentTerm;
  votedFor_ = state.votedFor;
  log_ = std::move(state.log);
  snapshotIndex_ = state.snapshotIndex;
  snapshotTerm_ = state.snapshotTerm;
  snapshot_ = std::move(state.snapshot);
  commitIndex_ =
      std::max(snapshotIndex_,
               std::min(state.commitIndex, snapshotIndex_ + log_.size()));
  lastApplied_ = snapshotIndex_;
}

RequestVoteRequest RaftCore::startElection() {
  std::lock_guard<std::mutex> lock(mutex_);
  ++currentTerm_;
  role_ = Role::Candidate;
  leaderId_.reset();
  votedFor_ = id_;
  votes_.clear();
  votes_.insert(id_);
  persistLocked();

  if (hasMajority(votes_.size()))
    becomeLeaderLocked();

  return {currentTerm_, id_, lastLogIndexLocked(), lastLogTermLocked()};
}

RequestVoteResponse
RaftCore::handleRequestVote(const RequestVoteRequest &request) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (request.term < currentTerm_)
    return {currentTerm_, false};

  bool persistentStateChanged = request.term > currentTerm_;
  if (persistentStateChanged)
    stepDownLocked(request.term, std::nullopt);

  const bool candidateIsCurrent =
      request.lastLogTerm > lastLogTermLocked() ||
      (request.lastLogTerm == lastLogTermLocked() &&
       request.lastLogIndex >= lastLogIndexLocked());
  const bool canVote = !votedFor_ || *votedFor_ == request.candidateId;
  const bool grant = canVote && candidateIsCurrent;
  if (grant) {
    persistentStateChanged = persistentStateChanged ||
                             !votedFor_ || *votedFor_ != request.candidateId;
    votedFor_ = request.candidateId;
    role_ = Role::Follower;
    leaderId_.reset();
  }
  if (persistentStateChanged)
    persistLocked();
  return {currentTerm_, grant};
}

bool RaftCore::recordVote(NodeId peer, const RequestVoteResponse &response) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (response.term > currentTerm_) {
    stepDownLocked(response.term, std::nullopt);
    persistLocked();
    return false;
  }
  if (role_ != Role::Candidate || response.term != currentTerm_ ||
      !response.voteGranted)
    return role_ == Role::Leader;

  votes_.insert(peer);
  if (hasMajority(votes_.size()))
    becomeLeaderLocked();
  return role_ == Role::Leader;
}

std::optional<AppendEntriesRequest>
RaftCore::makeAppendEntries(NodeId peer) const {
  std::lock_guard<std::mutex> lock(mutex_);
  if (role_ != Role::Leader)
    return std::nullopt;

  auto nextIt = nextIndex_.find(peer);
  if (nextIt == nextIndex_.end())
    throw std::invalid_argument("unknown raft peer");
  const LogIndex next = std::max(nextIt->second, snapshotIndex_ + 1);
  const LogIndex previous = next - 1;

  std::vector<LogEntry> entries;
  if (next <= lastLogIndexLocked()) {
    const auto begin =
        log_.begin() + static_cast<std::ptrdiff_t>(offsetOf(next));
    entries.assign(begin, log_.end());
  }
  return AppendEntriesRequest{currentTerm_, id_, previous, termAt(previous),
                              std::move(entries), commitIndex_};
}

AppendEntriesResponse
RaftCore::handleAppendEntries(const AppendEntriesRequest &request) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (request.term < currentTerm_)
    return {currentTerm_, false, 0, lastLogIndexLocked() + 1};

  bool persistentStateChanged = request.term > currentTerm_;
  if (persistentStateChanged || role_ != Role::Follower)
    stepDownLocked(request.term, request.leaderId);
  else
    leaderId_ = request.leaderId;

  if (request.previousLogIndex < snapshotIndex_) {
    if (persistentStateChanged)
      persistLocked();
    return {currentTerm_, false, 0, snapshotIndex_ + 1};
  }
  if (request.previousLogIndex > lastLogIndexLocked()) {
    if (persistentStateChanged)
      persistLocked();
    return {currentTerm_, false, 0, lastLogIndexLocked() + 1};
  }
  if (termAt(request.previousLogIndex) != request.previousLogTerm) {
    const Term conflictTerm = termAt(request.previousLogIndex);
    LogIndex hint = request.previousLogIndex;
    while (hint > snapshotIndex_ && termAt(hint - 1) == conflictTerm)
      --hint;
    if (persistentStateChanged)
      persistLocked();
    return {currentTerm_, false, 0, hint};
  }

  LogIndex index = request.previousLogIndex + 1;
  std::size_t incoming = 0;
  while (incoming < request.entries.size() && index <= lastLogIndexLocked()) {
    if (termAt(index) != request.entries[incoming].term) {
      log_.resize(offsetOf(index));
      persistentStateChanged = true;
      break;
    }
    ++index;
    ++incoming;
  }
  if (incoming < request.entries.size()) {
    log_.insert(log_.end(),
                request.entries.begin() + static_cast<std::ptrdiff_t>(incoming),
                request.entries.end());
    persistentStateChanged = true;
  }

  if (request.leaderCommit > commitIndex_) {
    commitIndex_ = std::min(request.leaderCommit, lastLogIndexLocked());
    persistentStateChanged = true;
  }
  if (persistentStateChanged)
    persistLocked();
  return {currentTerm_, true, request.previousLogIndex + request.entries.size(),
          request.previousLogIndex + request.entries.size() + 1};
}

void RaftCore::recordAppendEntriesResponse(
    NodeId peer, const AppendEntriesResponse &response) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (response.term > currentTerm_) {
    stepDownLocked(response.term, std::nullopt);
    persistLocked();
    return;
  }
  if (role_ != Role::Leader || response.term != currentTerm_)
    return;

  if (response.success) {
    const LogIndex previousCommit = commitIndex_;
    matchIndex_[peer] = std::max(matchIndex_[peer], response.matchIndex);
    nextIndex_[peer] = matchIndex_[peer] + 1;
    advanceCommitIndexLocked();
    if (commitIndex_ != previousCommit)
      persistLocked();
  } else {
    nextIndex_[peer] = std::max<LogIndex>(1, response.nextIndexHint);
  }
}

std::optional<InstallSnapshotRequest>
RaftCore::makeInstallSnapshot(NodeId peer) const {
  std::lock_guard<std::mutex> lock(mutex_);
  if (role_ != Role::Leader)
    return std::nullopt;
  auto nextIt = nextIndex_.find(peer);
  if (nextIt == nextIndex_.end())
    throw std::invalid_argument("unknown raft peer");
  if (nextIt->second > snapshotIndex_)
    return std::nullopt;
  return InstallSnapshotRequest{currentTerm_, id_, snapshotIndex_,
                                snapshotTerm_, snapshot_};
}

InstallSnapshotResponse
RaftCore::handleInstallSnapshot(const InstallSnapshotRequest &request) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (request.term < currentTerm_)
    return {currentTerm_, false};

  bool persistentStateChanged = request.term > currentTerm_;
  stepDownLocked(request.term, request.leaderId);
  if (request.lastIncludedIndex > snapshotIndex_) {
    if (request.lastIncludedIndex <= lastLogIndexLocked() &&
        termAt(request.lastIncludedIndex) == request.lastIncludedTerm) {
      const std::size_t retainedOffset =
          offsetOf(request.lastIncludedIndex) + 1;
      log_.erase(log_.begin(),
                 log_.begin() + static_cast<std::ptrdiff_t>(retainedOffset));
    } else {
      log_.clear();
    }
    snapshotIndex_ = request.lastIncludedIndex;
    snapshotTerm_ = request.lastIncludedTerm;
    snapshot_ = request.snapshot;
    commitIndex_ = std::max(commitIndex_, snapshotIndex_);
    lastApplied_ = std::max(lastApplied_, snapshotIndex_);
    persistentStateChanged = true;
  }
  if (persistentStateChanged)
    persistLocked();
  return {currentTerm_, true};
}

void RaftCore::recordInstallSnapshotResponse(
    NodeId peer, const InstallSnapshotResponse &response,
    LogIndex installedIndex) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (response.term > currentTerm_) {
    stepDownLocked(response.term, std::nullopt);
    persistLocked();
    return;
  }
  if (role_ == Role::Leader && response.term == currentTerm_ &&
      response.success) {
    matchIndex_[peer] = std::max(matchIndex_[peer], installedIndex);
    nextIndex_[peer] = matchIndex_[peer] + 1;
  }
}

std::optional<LogIndex> RaftCore::append(const storage::KvCommand &command) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (role_ != Role::Leader)
    return std::nullopt;
  log_.push_back({currentTerm_, command});
  const LogIndex index = lastLogIndexLocked();
  matchIndex_[id_] = index;
  advanceCommitIndexLocked();
  persistLocked();
  return index;
}

std::vector<CommittedEntry> RaftCore::takeCommitted() {
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<CommittedEntry> committed;
  while (lastApplied_ < commitIndex_) {
    ++lastApplied_;
    if (lastApplied_ > snapshotIndex_)
      committed.push_back({lastApplied_, log_[offsetOf(lastApplied_)].command});
  }
  return committed;
}

void RaftCore::compact(LogIndex lastIncludedIndex,
                       const std::string &snapshot) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (lastIncludedIndex <= snapshotIndex_)
    return;
  if (lastIncludedIndex > lastApplied_ ||
      lastIncludedIndex > lastLogIndexLocked())
    throw std::invalid_argument("snapshot index has not been applied");

  const Term includedTerm = termAt(lastIncludedIndex);
  const std::size_t eraseCount = offsetOf(lastIncludedIndex) + 1;
  log_.erase(log_.begin(),
             log_.begin() + static_cast<std::ptrdiff_t>(eraseCount));
  snapshotIndex_ = lastIncludedIndex;
  snapshotTerm_ = includedTerm;
  snapshot_ = snapshot;
  persistLocked();
}

void RaftCore::becomeFollower(Term term, std::optional<NodeId> leader) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (term < currentTerm_)
    return;
  stepDownLocked(term, leader);
  persistLocked();
}

Role RaftCore::role() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return role_;
}

Term RaftCore::currentTerm() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return currentTerm_;
}

std::optional<NodeId> RaftCore::leaderId() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return leaderId_;
}

LogIndex RaftCore::commitIndex() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return commitIndex_;
}

LogIndex RaftCore::lastLogIndex() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return lastLogIndexLocked();
}

LogIndex RaftCore::snapshotIndex() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return snapshotIndex_;
}

std::string RaftCore::snapshot() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return snapshot_;
}

Term RaftCore::termAt(LogIndex index) const {
  if (index == snapshotIndex_)
    return snapshotTerm_;
  if (index < snapshotIndex_ || index > lastLogIndexLocked())
    throw std::out_of_range("raft log index is unavailable");
  return log_[offsetOf(index)].term;
}

std::size_t RaftCore::offsetOf(LogIndex index) const {
  return static_cast<std::size_t>(index - snapshotIndex_ - 1);
}

LogIndex RaftCore::lastLogIndexLocked() const {
  return snapshotIndex_ + log_.size();
}

Term RaftCore::lastLogTermLocked() const {
  return log_.empty() ? snapshotTerm_ : log_.back().term;
}

void RaftCore::persistLocked() {
  storage_->save({currentTerm_, votedFor_, snapshotIndex_, snapshotTerm_,
                  snapshot_, commitIndex_, log_});
}

void RaftCore::stepDownLocked(Term term, std::optional<NodeId> leader) {
  if (term > currentTerm_) {
    currentTerm_ = term;
    votedFor_.reset();
  }
  role_ = Role::Follower;
  leaderId_ = leader;
  votes_.clear();
}

void RaftCore::becomeLeaderLocked() {
  role_ = Role::Leader;
  leaderId_ = id_;
  log_.push_back({currentTerm_, {storage::KvOperation::NoOp, "", "", "", 0}});
  const LogIndex next = lastLogIndexLocked() + 1;
  nextIndex_.clear();
  matchIndex_.clear();
  matchIndex_[id_] = lastLogIndexLocked();
  for (const NodeId peer : peers_) {
    nextIndex_[peer] = next;
    matchIndex_[peer] = 0;
  }
  advanceCommitIndexLocked();
  persistLocked();
}

void RaftCore::advanceCommitIndexLocked() {
  for (LogIndex index = lastLogIndexLocked(); index > commitIndex_; --index) {
    if (termAt(index) != currentTerm_)
      continue;
    std::size_t replicated = 1;
    for (const NodeId peer : peers_) {
      auto it = matchIndex_.find(peer);
      if (it != matchIndex_.end() && it->second >= index)
        ++replicated;
    }
    if (hasMajority(replicated)) {
      commitIndex_ = index;
      break;
    }
  }
}

bool RaftCore::hasMajority(std::size_t votes) const {
  const std::size_t clusterSize = peers_.size() + 1;
  return votes >= clusterSize / 2 + 1;
}

} // namespace cppcache::raft

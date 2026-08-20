#include "raft/RaftNode.h"

#include "storage/KvCodec.h"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace cppcache::raft {
namespace {

std::int64_t nowMillis() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

} // namespace

RaftNode::RaftNode(NodeId id, std::vector<NodeId> peers,
                   std::shared_ptr<IRaftStorage> raftStorage,
                   std::unique_ptr<storage::KvStateMachine> stateMachine,
                   RaftNodeOptions options)
    : id_(id), peers_(std::move(peers)),
      core_(id, peers_, std::move(raftStorage)),
      stateMachine_(std::move(stateMachine)), options_(options),
      appliedIndex_(core_.snapshotIndex()),
      electionRandom_(static_cast<std::mt19937_64::result_type>(
          std::chrono::steady_clock::now().time_since_epoch().count()) ^
                      static_cast<std::mt19937_64::result_type>(id)) {
  if (!stateMachine_)
    throw std::invalid_argument("raft state machine must not be null");
  if (options_.electionTimeout <= std::chrono::milliseconds::zero() ||
      options_.heartbeatInterval <= std::chrono::milliseconds::zero() ||
      options_.tickInterval <= std::chrono::milliseconds::zero())
    throw std::invalid_argument("raft timeouts must be positive");

  const std::string persistedSnapshot = core_.snapshot();
  if (!persistedSnapshot.empty())
    stateMachine_->restore(storage::decodeSnapshot(persistedSnapshot));
  applyCommitted();
  resetElectionDeadline();
}

RaftNode::~RaftNode() { stop(); }

void RaftNode::setTransport(std::shared_ptr<IRaftTransport> transport) {
  if (!transport)
    throw std::invalid_argument("raft transport must not be null");
  std::lock_guard<std::mutex> lock(transportMutex_);
  transport_ = std::move(transport);
}

void RaftNode::start() {
  std::lock_guard<std::mutex> lifecycleLock(lifecycleMutex_);
  if (!transport())
    throw std::logic_error("raft transport must be configured before start");
  if (running_.load())
    return;
  if (worker_.joinable())
    worker_.join();
  {
    std::lock_guard<std::mutex> failureLock(workerFailureMutex_);
    workerFailure_.reset();
  }
  running_.store(true);
  resetElectionDeadline();
  try {
    worker_ = std::thread(&RaftNode::run, this);
  } catch (...) {
    running_.store(false);
    throw;
  }
}

void RaftNode::stop() {
  std::lock_guard<std::mutex> lifecycleLock(lifecycleMutex_);
  running_.store(false);
  if (worker_.joinable())
    worker_.join();
}

std::optional<std::string> RaftNode::workerFailure() const {
  std::lock_guard<std::mutex> lock(workerFailureMutex_);
  return workerFailure_;
}

RequestVoteResponse
RaftNode::handleRequestVote(const RequestVoteRequest &request) {
  const auto response = core_.handleRequestVote(request);
  if (response.voteGranted)
    resetElectionDeadline();
  return response;
}

AppendEntriesResponse
RaftNode::handleAppendEntries(const AppendEntriesRequest &request) {
  const Term before = core_.currentTerm();
  auto response = core_.handleAppendEntries(request);
  if (request.term >= before)
    resetElectionDeadline();
  if (response.success)
    applyCommitted();
  return response;
}

InstallSnapshotResponse
RaftNode::handleInstallSnapshot(const InstallSnapshotRequest &request) {
  std::optional<storage::KvSnapshot> decodedSnapshot;
  if (request.term >= core_.currentTerm() &&
      request.lastIncludedIndex > core_.snapshotIndex())
    decodedSnapshot = storage::decodeSnapshot(request.snapshot);

  InstallSnapshotResponse response;
  {
    std::lock_guard<std::mutex> lock(applyMutex_);
    const LogIndex previousSnapshot = core_.snapshotIndex();
    response = core_.handleInstallSnapshot(request);
    if (response.success && core_.snapshotIndex() > previousSnapshot) {
      if (!decodedSnapshot)
        throw std::logic_error("snapshot was not validated before installation");
      stateMachine_->restore(*decodedSnapshot);
      appliedIndex_ = request.lastIncludedIndex;
    }
  }
  if (request.term >= core_.currentTerm())
    resetElectionDeadline();
  return response;
}

SubmitResult RaftNode::submit(const storage::KvCommand &command,
                              std::chrono::milliseconds timeout) {
  if (command.operation == storage::KvOperation::NoOp)
    throw std::invalid_argument("clients cannot submit Raft no-op commands");
  if (command.clientId.empty() || command.requestId == 0)
    throw std::invalid_argument("client id and request id are required");

  std::optional<LogIndex> index;
  {
    std::lock_guard<std::mutex> applyLock(applyMutex_);
    index = core_.append(command);
    if (index)
      pendingResults_.insert(*index);
  }
  if (!index)
    return {ClientStatus::NotLeader, core_.leaderId(), std::nullopt};

  const auto discardPendingResult = [this, index] {
    std::lock_guard<std::mutex> applyLock(applyMutex_);
    pendingResults_.erase(*index);
    applyResults_.erase(*index);
  };

  const auto deadline = std::chrono::steady_clock::now() + timeout;
  do {
    (void)replicateAll();
    applyCommitted();
    if (core_.commitIndex() >= *index) {
      std::lock_guard<std::mutex> applyLock(applyMutex_);
      auto resultIt = applyResults_.find(*index);
      std::optional<storage::ApplyResult> result;
      if (resultIt != applyResults_.end()) {
        result = resultIt->second;
        applyResults_.erase(resultIt);
      }
      pendingResults_.erase(*index);
      return {ClientStatus::Committed, id_, std::move(result)};
    }
    if (core_.role() != Role::Leader) {
      discardPendingResult();
      return {ClientStatus::NotLeader, core_.leaderId(), std::nullopt};
    }
    std::this_thread::sleep_for(options_.tickInterval);
  } while (std::chrono::steady_clock::now() < deadline);

  discardPendingResult();
  return {ClientStatus::Timeout, core_.leaderId(), std::nullopt};
}

ReadResult RaftNode::read(const std::string &key,
                          std::chrono::milliseconds timeout) {
  if (core_.role() != Role::Leader)
    return {ClientStatus::NotLeader, core_.leaderId(), std::nullopt};

  const auto deadline = std::chrono::steady_clock::now() + timeout;
  const std::size_t majority = (peers_.size() + 1) / 2 + 1;
  do {
    if (replicateAll() >= majority) {
      applyCommitted();
      return {ClientStatus::Committed, id_, stateMachine_->get(key)};
    }
    if (core_.role() != Role::Leader)
      return {ClientStatus::NotLeader, core_.leaderId(), std::nullopt};
    std::this_thread::sleep_for(options_.tickInterval);
  } while (std::chrono::steady_clock::now() < deadline);
  return {ClientStatus::Timeout, core_.leaderId(), std::nullopt};
}

std::optional<std::string> RaftNode::localRead(const std::string &key) {
  return stateMachine_->get(key);
}

void RaftNode::run() {
  try {
    lastHeartbeat_ = std::chrono::steady_clock::now();
    while (running_.load()) {
      const auto now = std::chrono::steady_clock::now();
      if (core_.role() == Role::Leader) {
        if (now - lastHeartbeat_ >= options_.heartbeatInterval) {
          (void)replicateAll();
          applyCommitted();
          lastHeartbeat_ = now;
        }
      } else if (nowMillis() >= electionDeadlineMillis_.load()) {
        runElection();
      }
      std::this_thread::sleep_for(options_.tickInterval);
    }
  } catch (const std::exception &error) {
    recordWorkerFailure(error.what());
  } catch (...) {
    recordWorkerFailure("unknown error");
  }
  running_.store(false);
}

void RaftNode::recordWorkerFailure(std::string message) {
  std::lock_guard<std::mutex> lock(workerFailureMutex_);
  workerFailure_ = std::move(message);
}

void RaftNode::runElection() {
  resetElectionDeadline();
  const RequestVoteRequest request = core_.startElection();
  auto activeTransport = transport();
  for (const NodeId peer : peers_) {
    auto response = activeTransport->requestVote(peer, request);
    if (response && core_.recordVote(peer, *response))
      break;
  }
  if (core_.role() == Role::Leader) {
    lastHeartbeat_ = std::chrono::steady_clock::now();
    (void)replicateAll();
  }
}

std::size_t RaftNode::replicateAll() {
  std::lock_guard<std::mutex> replicationLock(replicationMutex_);
  if (core_.role() != Role::Leader)
    return 0;
  std::size_t successes = 1;
  for (const NodeId peer : peers_) {
    if (replicatePeer(peer))
      ++successes;
  }
  return successes;
}

bool RaftNode::replicatePeer(NodeId peer) {
  auto activeTransport = transport();
  for (int attempt = 0; attempt < 32 && core_.role() == Role::Leader;
       ++attempt) {
    auto snapshotRequest = core_.makeInstallSnapshot(peer);
    if (snapshotRequest) {
      auto response = activeTransport->installSnapshot(peer, *snapshotRequest);
      if (!response)
        return false;
      core_.recordInstallSnapshotResponse(peer, *response,
                                          snapshotRequest->lastIncludedIndex);
      if (!response->success)
        return false;
    }

    const auto request = core_.makeAppendEntries(peer);
    if (!request)
      return false;
    auto response = activeTransport->appendEntries(peer, *request);
    if (!response)
      return false;
    core_.recordAppendEntriesResponse(peer, *response);
    if (response->success)
      return true;
  }
  return false;
}

void RaftNode::applyCommitted() {
  std::lock_guard<std::mutex> lock(applyMutex_);
  const auto entries = core_.takeCommitted();
  for (const auto &entry : entries) {
    auto result = stateMachine_->apply(entry.command);
    if (entry.command.operation != storage::KvOperation::NoOp &&
        pendingResults_.contains(entry.index))
      applyResults_[entry.index] = std::move(result);
    appliedIndex_ = entry.index;
  }

  if (options_.snapshotThreshold > 0 &&
      appliedIndex_ >= core_.snapshotIndex() + options_.snapshotThreshold) {
    core_.compact(appliedIndex_,
                  storage::encodeSnapshot(stateMachine_->snapshot()));
  }
}

void RaftNode::resetElectionDeadline() {
  std::chrono::milliseconds jitter;
  {
    std::lock_guard<std::mutex> lock(electionRandomMutex_);
    std::uniform_int_distribution<std::int64_t> distribution(
        0, options_.electionTimeout.count());
    jitter = std::chrono::milliseconds(distribution(electionRandom_));
  }
  electionDeadlineMillis_.store(nowMillis() +
                                (options_.electionTimeout + jitter).count());
}

std::shared_ptr<IRaftTransport> RaftNode::transport() const {
  std::lock_guard<std::mutex> lock(transportMutex_);
  return transport_;
}

} // namespace cppcache::raft

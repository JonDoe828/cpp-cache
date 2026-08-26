#include "raft/TcpTransport.h"

#include "TcpProtocol.h"
#include "TcpSocket.h"
#include "raft/RaftNode.h"

#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <stdexcept>
#include <utility>

namespace cppcache::raft {

TcpTransport::TcpTransport(NodeId localId, Endpoint listenEndpoint,
                           std::unordered_map<NodeId, Endpoint> peers,
                           std::chrono::milliseconds rpcTimeout)
    : localId_(localId), listenEndpoint_(std::move(listenEndpoint)),
      peers_(std::move(peers)), rpcTimeout_(rpcTimeout) {
  if (listenEndpoint_.port == 0 || rpcTimeout_ <= std::chrono::milliseconds(0))
    throw std::invalid_argument("invalid TCP transport configuration");
}

TcpTransport::~TcpTransport() { stop(); }

void TcpTransport::attach(RaftNode *node) {
  if (!node || node->id() != localId_)
    throw std::invalid_argument("TCP transport node id mismatch");
  node_ = node;
}

void TcpTransport::start() {
  if (!node_)
    throw std::logic_error("attach a Raft node before starting TCP transport");
  bool expected = false;
  if (!running_.compare_exchange_strong(expected, true))
    return;
  listenSocket_ = tcp::createListener(listenEndpoint_);
  if (listenSocket_ < 0) {
    running_.store(false);
    throw std::runtime_error("failed to listen on TCP endpoint");
  }
  for (int index = 0; index < 4; ++index)
    workers_.emplace_back(&TcpTransport::workerLoop, this);
  for (int index = 0; index < 2; ++index)
    clientWorkers_.emplace_back(&TcpTransport::clientWorkerLoop, this);
  acceptThread_ = std::thread(&TcpTransport::acceptLoop, this);
}

void TcpTransport::stop() {
  if (!running_.exchange(false))
    return;
  queueCondition_.notify_all();
  if (acceptThread_.joinable())
    acceptThread_.join();
  if (listenSocket_ >= 0) {
    ::close(listenSocket_);
    listenSocket_ = -1;
  }
  for (auto &worker : workers_) {
    if (worker.joinable())
      worker.join();
  }
  workers_.clear();
  clientQueueCondition_.notify_all();
  for (auto &worker : clientWorkers_) {
    if (worker.joinable())
      worker.join();
  }
  clientWorkers_.clear();
  {
    std::lock_guard<std::mutex> lock(queueMutex_);
    for (const int socket : connections_)
      ::close(socket);
    connections_.clear();
  }
  {
    std::lock_guard<std::mutex> lock(clientQueueMutex_);
    for (const auto &connection : clientConnections_)
      ::close(connection.socket);
    clientConnections_.clear();
  }
}

std::optional<RequestVoteResponse>
TcpTransport::requestVote(NodeId target, const RequestVoteRequest &request) {
  auto payload = call(target, tcp::kRequestVote, tcp::encode(request),
                      tcp::kRequestVoteResponse);
  if (!payload)
    return std::nullopt;
  return tcp::decodeRequestVoteResponse(*payload);
}

std::optional<AppendEntriesResponse>
TcpTransport::appendEntries(NodeId target,
                            const AppendEntriesRequest &request) {
  auto payload = call(target, tcp::kAppendEntries, tcp::encode(request),
                      tcp::kAppendEntriesResponse);
  if (!payload)
    return std::nullopt;
  return tcp::decodeAppendEntriesResponse(*payload);
}

std::optional<InstallSnapshotResponse>
TcpTransport::installSnapshot(NodeId target,
                              const InstallSnapshotRequest &request) {
  auto payload = call(target, tcp::kInstallSnapshot, tcp::encode(request),
                      tcp::kInstallSnapshotResponse);
  if (!payload)
    return std::nullopt;
  return tcp::decodeInstallSnapshotResponse(*payload);
}

std::optional<std::string> TcpTransport::call(NodeId target, std::uint8_t type,
                                              const std::string &payload,
                                              std::uint8_t expectedResponse) {
  auto endpoint = peers_.find(target);
  if (endpoint == peers_.end())
    return std::nullopt;
  return tcp::callEndpoint(endpoint->second, rpcTimeout_, type, payload,
                           expectedResponse);
}

void TcpTransport::acceptLoop() {
  while (running_.load()) {
    pollfd descriptor{listenSocket_, POLLIN, 0};
    const int ready = ::poll(&descriptor, 1, 100);
    if (ready < 0) {
      if (errno == EINTR)
        continue;
      break;
    }
    if (ready == 0 || !running_.load())
      continue;
    const int socket = ::accept(listenSocket_, nullptr, nullptr);
    if (socket < 0) {
      if (errno == EINTR)
        continue;
      break;
    }
    tcp::configureTimeout(socket,
                          rpcTimeout_ + std::chrono::milliseconds(2000));
    {
      std::lock_guard<std::mutex> lock(queueMutex_);
      connections_.push_back(socket);
    }
    queueCondition_.notify_one();
  }
}

void TcpTransport::workerLoop() {
  while (true) {
    int socket = -1;
    {
      std::unique_lock<std::mutex> lock(queueMutex_);
      queueCondition_.wait(
          lock, [&] { return !running_.load() || !connections_.empty(); });
      if (connections_.empty()) {
        if (!running_.load())
          return;
        continue;
      }
      socket = connections_.front();
      connections_.pop_front();
    }
    if (!handleConnection(socket))
      ::close(socket);
  }
}

void TcpTransport::clientWorkerLoop() {
  while (true) {
    ClientConnection connection{-1, {}};
    {
      std::unique_lock<std::mutex> lock(clientQueueMutex_);
      clientQueueCondition_.wait(lock, [&] {
        return !running_.load() || !clientConnections_.empty();
      });
      if (clientConnections_.empty()) {
        if (!running_.load())
          return;
        continue;
      }
      connection = std::move(clientConnections_.front());
      clientConnections_.pop_front();
    }
    handleClientRequest(connection.socket, connection.payload);
    ::close(connection.socket);
  }
}

bool TcpTransport::handleConnection(int socket) {
  try {
    auto frame = tcp::receiveFrame(socket);
    if (!frame)
      return false;
    if (frame->first == tcp::kClientRequest) {
      {
        std::lock_guard<std::mutex> lock(clientQueueMutex_);
        clientConnections_.push_back({socket, std::move(frame->second)});
      }
      clientQueueCondition_.notify_one();
      return true;
    }
    switch (frame->first) {
    case tcp::kRequestVote: {
      const auto response =
          node_->handleRequestVote(tcp::decodeRequestVote(frame->second));
      (void)tcp::sendFrame(socket, tcp::kRequestVoteResponse,
                           tcp::encode(response));
      break;
    }
    case tcp::kAppendEntries: {
      const auto response =
          node_->handleAppendEntries(tcp::decodeAppendEntries(frame->second));
      (void)tcp::sendFrame(socket, tcp::kAppendEntriesResponse,
                           tcp::encode(response));
      break;
    }
    case tcp::kInstallSnapshot: {
      const auto response = node_->handleInstallSnapshot(
          tcp::decodeInstallSnapshot(frame->second));
      (void)tcp::sendFrame(socket, tcp::kInstallSnapshotResponse,
                           tcp::encode(response));
      break;
    }
    default:
      break;
    }
  } catch (const std::exception &) {
  }
  return false;
}

void TcpTransport::handleClientRequest(int socket, const std::string &payload) {
  try {
    const auto request = tcp::decodeClientRequest(payload);
    const auto timeout = std::chrono::milliseconds(request.timeoutMillis);
    tcp::ClientWireResponse response;
    if (request.read) {
      const auto result = node_->read(request.command.key, timeout);
      response.status = result.status;
      response.leaderId = result.leaderId;
      response.value = result.value;
    } else {
      const auto result = node_->submit(request.command, timeout);
      response.status = result.status;
      response.leaderId = result.leaderId;
      response.applyResult = result.result;
    }
    (void)tcp::sendFrame(socket, tcp::kClientResponse, tcp::encode(response));
  } catch (const std::exception &) {
  }
}

} // namespace cppcache::raft

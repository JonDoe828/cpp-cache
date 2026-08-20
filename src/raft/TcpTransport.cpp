#include "raft/TcpTransport.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace cppcache::raft {
namespace {

constexpr std::uint32_t kMaxFrameSize = 64 * 1024 * 1024;
constexpr std::uint32_t kMaxEntries = 100'000;
constexpr std::uint32_t kMaxClientTimeoutMillis = 10'000;
constexpr std::uint8_t kRequestVote = 1;
constexpr std::uint8_t kRequestVoteResponse = 2;
constexpr std::uint8_t kAppendEntries = 3;
constexpr std::uint8_t kAppendEntriesResponse = 4;
constexpr std::uint8_t kInstallSnapshot = 5;
constexpr std::uint8_t kInstallSnapshotResponse = 6;
constexpr std::uint8_t kClientRequest = 10;
constexpr std::uint8_t kClientResponse = 11;

class Socket {
public:
  explicit Socket(int descriptor = -1) : descriptor_(descriptor) {}
  ~Socket() {
    if (descriptor_ >= 0)
      ::close(descriptor_);
  }
  Socket(const Socket &) = delete;
  Socket &operator=(const Socket &) = delete;
  Socket(Socket &&other) noexcept
      : descriptor_(std::exchange(other.descriptor_, -1)) {}
  Socket &operator=(Socket &&other) noexcept {
    if (this != &other) {
      if (descriptor_ >= 0)
        ::close(descriptor_);
      descriptor_ = std::exchange(other.descriptor_, -1);
    }
    return *this;
  }
  int get() const { return descriptor_; }
  int release() { return std::exchange(descriptor_, -1); }

private:
  int descriptor_;
};

class Writer {
public:
  void u8(std::uint8_t value) { bytes_.push_back(static_cast<char>(value)); }
  void boolean(bool value) { u8(value ? 1 : 0); }
  void u32(std::uint32_t value) {
    const std::uint32_t network = htonl(value);
    bytes_.append(reinterpret_cast<const char *>(&network), sizeof(network));
  }
  void u64(std::uint64_t value) {
    for (int shift = 56; shift >= 0; shift -= 8)
      u8(static_cast<std::uint8_t>((value >> shift) & 0xff));
  }
  void string(std::string_view value) {
    if (value.size() > kMaxFrameSize)
      throw std::invalid_argument("wire string exceeds frame limit");
    u32(static_cast<std::uint32_t>(value.size()));
    bytes_.append(value);
  }
  std::string finish() { return std::move(bytes_); }

private:
  std::string bytes_;
};

class Reader {
public:
  explicit Reader(std::string_view bytes) : bytes_(bytes) {}
  std::uint8_t u8() {
    require(1);
    return static_cast<std::uint8_t>(bytes_[position_++]);
  }
  bool boolean() {
    const auto value = u8();
    if (value > 1)
      throw std::runtime_error("invalid wire boolean");
    return value != 0;
  }
  std::uint32_t u32() {
    require(sizeof(std::uint32_t));
    std::uint32_t network{};
    std::memcpy(&network, bytes_.data() + position_, sizeof(network));
    position_ += sizeof(network);
    return ntohl(network);
  }
  std::uint64_t u64() {
    std::uint64_t value = 0;
    for (int index = 0; index < 8; ++index)
      value = (value << 8) | u8();
    return value;
  }
  std::string string() {
    const std::uint32_t size = u32();
    require(size);
    std::string value(bytes_.substr(position_, size));
    position_ += size;
    return value;
  }
  void finish() const {
    if (position_ != bytes_.size())
      throw std::runtime_error("trailing bytes in wire payload");
  }

private:
  void require(std::size_t size) const {
    if (position_ > bytes_.size() || size > bytes_.size() - position_)
      throw std::runtime_error("truncated wire payload");
  }
  std::string_view bytes_;
  std::size_t position_{0};
};

void writeCommand(Writer &writer, const storage::KvCommand &command) {
  writer.u8(static_cast<std::uint8_t>(command.operation));
  writer.string(command.key);
  writer.string(command.value);
  writer.string(command.clientId);
  writer.u64(command.requestId);
}

storage::KvCommand readCommand(Reader &reader) {
  const auto operation = reader.u8();
  if (operation > static_cast<std::uint8_t>(storage::KvOperation::NoOp))
    throw std::runtime_error("invalid wire KV operation");
  return {static_cast<storage::KvOperation>(operation), reader.string(),
          reader.string(), reader.string(), reader.u64()};
}

std::string encode(const RequestVoteRequest &request) {
  Writer writer;
  writer.u64(request.term);
  writer.u32(request.candidateId);
  writer.u64(request.lastLogIndex);
  writer.u64(request.lastLogTerm);
  return writer.finish();
}

RequestVoteRequest decodeRequestVote(std::string_view bytes) {
  Reader reader(bytes);
  RequestVoteRequest request{reader.u64(), reader.u32(), reader.u64(),
                             reader.u64()};
  reader.finish();
  return request;
}

std::string encode(const RequestVoteResponse &response) {
  Writer writer;
  writer.u64(response.term);
  writer.boolean(response.voteGranted);
  return writer.finish();
}

RequestVoteResponse decodeRequestVoteResponse(std::string_view bytes) {
  Reader reader(bytes);
  RequestVoteResponse response{reader.u64(), reader.boolean()};
  reader.finish();
  return response;
}

std::string encode(const AppendEntriesRequest &request) {
  Writer writer;
  writer.u64(request.term);
  writer.u32(request.leaderId);
  writer.u64(request.previousLogIndex);
  writer.u64(request.previousLogTerm);
  writer.u32(static_cast<std::uint32_t>(request.entries.size()));
  for (const auto &entry : request.entries) {
    writer.u64(entry.term);
    writeCommand(writer, entry.command);
  }
  writer.u64(request.leaderCommit);
  return writer.finish();
}

AppendEntriesRequest decodeAppendEntries(std::string_view bytes) {
  Reader reader(bytes);
  AppendEntriesRequest request;
  request.term = reader.u64();
  request.leaderId = reader.u32();
  request.previousLogIndex = reader.u64();
  request.previousLogTerm = reader.u64();
  const auto count = reader.u32();
  if (count > kMaxEntries)
    throw std::runtime_error("too many entries in append request");
  request.entries.reserve(count);
  for (std::uint32_t index = 0; index < count; ++index)
    request.entries.push_back({reader.u64(), readCommand(reader)});
  request.leaderCommit = reader.u64();
  reader.finish();
  return request;
}

std::string encode(const AppendEntriesResponse &response) {
  Writer writer;
  writer.u64(response.term);
  writer.boolean(response.success);
  writer.u64(response.matchIndex);
  writer.u64(response.nextIndexHint);
  return writer.finish();
}

AppendEntriesResponse decodeAppendEntriesResponse(std::string_view bytes) {
  Reader reader(bytes);
  AppendEntriesResponse response{reader.u64(), reader.boolean(), reader.u64(),
                                 reader.u64()};
  reader.finish();
  return response;
}

std::string encode(const InstallSnapshotRequest &request) {
  Writer writer;
  writer.u64(request.term);
  writer.u32(request.leaderId);
  writer.u64(request.lastIncludedIndex);
  writer.u64(request.lastIncludedTerm);
  writer.string(request.snapshot);
  return writer.finish();
}

InstallSnapshotRequest decodeInstallSnapshot(std::string_view bytes) {
  Reader reader(bytes);
  InstallSnapshotRequest request{reader.u64(), reader.u32(), reader.u64(),
                                 reader.u64(), reader.string()};
  reader.finish();
  return request;
}

std::string encode(const InstallSnapshotResponse &response) {
  Writer writer;
  writer.u64(response.term);
  writer.boolean(response.success);
  return writer.finish();
}

InstallSnapshotResponse decodeInstallSnapshotResponse(std::string_view bytes) {
  Reader reader(bytes);
  InstallSnapshotResponse response{reader.u64(), reader.boolean()};
  reader.finish();
  return response;
}

struct ClientWireRequest {
  bool read{false};
  storage::KvCommand command;
  std::uint32_t timeoutMillis{0};
};

std::string encode(const ClientWireRequest &request) {
  Writer writer;
  writer.boolean(request.read);
  writeCommand(writer, request.command);
  writer.u32(request.timeoutMillis);
  return writer.finish();
}

ClientWireRequest decodeClientRequest(std::string_view bytes) {
  Reader reader(bytes);
  ClientWireRequest request{reader.boolean(), readCommand(reader),
                            reader.u32()};
  reader.finish();
  if (request.timeoutMillis == 0 ||
      request.timeoutMillis > kMaxClientTimeoutMillis)
    throw std::runtime_error("invalid client timeout");
  return request;
}

struct ClientWireResponse {
  ClientStatus status{ClientStatus::Timeout};
  std::optional<NodeId> leaderId;
  std::optional<storage::ApplyResult> applyResult;
  std::optional<std::string> value;
};

std::string encode(const ClientWireResponse &response) {
  Writer writer;
  writer.u8(static_cast<std::uint8_t>(response.status));
  writer.boolean(response.leaderId.has_value());
  if (response.leaderId)
    writer.u32(*response.leaderId);
  writer.boolean(response.applyResult.has_value());
  if (response.applyResult) {
    writer.boolean(response.applyResult->applied);
    writer.boolean(response.applyResult->value.has_value());
    if (response.applyResult->value)
      writer.string(*response.applyResult->value);
  }
  writer.boolean(response.value.has_value());
  if (response.value)
    writer.string(*response.value);
  return writer.finish();
}

ClientWireResponse decodeClientResponse(std::string_view bytes) {
  Reader reader(bytes);
  const auto status = reader.u8();
  if (status > static_cast<std::uint8_t>(ClientStatus::Timeout))
    throw std::runtime_error("invalid client response status");
  ClientWireResponse response;
  response.status = static_cast<ClientStatus>(status);
  if (reader.boolean())
    response.leaderId = reader.u32();
  if (reader.boolean()) {
    storage::ApplyResult result;
    result.applied = reader.boolean();
    if (reader.boolean())
      result.value = reader.string();
    response.applyResult = std::move(result);
  }
  if (reader.boolean())
    response.value = reader.string();
  reader.finish();
  return response;
}

bool sendAll(int socket, const void *data, std::size_t size) {
  const auto *bytes = static_cast<const char *>(data);
  while (size > 0) {
    const ssize_t sent = ::send(socket, bytes, size, MSG_NOSIGNAL);
    if (sent <= 0) {
      if (sent < 0 && errno == EINTR)
        continue;
      return false;
    }
    bytes += sent;
    size -= static_cast<std::size_t>(sent);
  }
  return true;
}

bool receiveAll(int socket, void *data, std::size_t size) {
  auto *bytes = static_cast<char *>(data);
  while (size > 0) {
    const ssize_t received = ::recv(socket, bytes, size, 0);
    if (received <= 0) {
      if (received < 0 && errno == EINTR)
        continue;
      return false;
    }
    bytes += received;
    size -= static_cast<std::size_t>(received);
  }
  return true;
}

bool sendFrame(int socket, std::uint8_t type, const std::string &payload) {
  if (payload.size() + 1 > kMaxFrameSize)
    return false;
  const std::uint32_t size =
      htonl(static_cast<std::uint32_t>(payload.size() + 1));
  return sendAll(socket, &size, sizeof(size)) &&
         sendAll(socket, &type, sizeof(type)) &&
         sendAll(socket, payload.data(), payload.size());
}

std::optional<std::pair<std::uint8_t, std::string>> receiveFrame(int socket) {
  std::uint32_t networkSize{};
  if (!receiveAll(socket, &networkSize, sizeof(networkSize)))
    return std::nullopt;
  const std::uint32_t size = ntohl(networkSize);
  if (size == 0 || size > kMaxFrameSize)
    return std::nullopt;
  std::uint8_t type{};
  if (!receiveAll(socket, &type, sizeof(type)))
    return std::nullopt;
  std::string payload(size - 1, '\0');
  if (!payload.empty() &&
      !receiveAll(socket, payload.data(), payload.size()))
    return std::nullopt;
  return std::make_pair(type, std::move(payload));
}

void configureTimeout(int socket, std::chrono::milliseconds timeout) {
  timeval value{};
  value.tv_sec = static_cast<long>(timeout.count() / 1000);
  value.tv_usec = static_cast<long>((timeout.count() % 1000) * 1000);
  (void)::setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO, &value, sizeof(value));
  (void)::setsockopt(socket, SOL_SOCKET, SO_SNDTIMEO, &value, sizeof(value));
}

Socket connectTo(const Endpoint &endpoint, std::chrono::milliseconds timeout) {
  addrinfo hints{};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  addrinfo *addresses = nullptr;
  const std::string port = std::to_string(endpoint.port);
  if (::getaddrinfo(endpoint.host.c_str(), port.c_str(), &hints, &addresses) !=
      0)
    return Socket{};

  Socket connected;
  for (addrinfo *address = addresses; address; address = address->ai_next) {
    Socket candidate(::socket(address->ai_family, address->ai_socktype,
                              address->ai_protocol));
    if (candidate.get() < 0)
      continue;
    const int flags = ::fcntl(candidate.get(), F_GETFL, 0);
    (void)::fcntl(candidate.get(), F_SETFL, flags | O_NONBLOCK);
    int result = ::connect(candidate.get(), address->ai_addr,
                           static_cast<socklen_t>(address->ai_addrlen));
    if (result < 0 && errno == EINPROGRESS) {
      pollfd descriptor{candidate.get(), POLLOUT, 0};
      result = ::poll(&descriptor, 1, static_cast<int>(timeout.count()));
      if (result > 0) {
        int error = 0;
        socklen_t errorSize = sizeof(error);
        (void)::getsockopt(candidate.get(), SOL_SOCKET, SO_ERROR, &error,
                           &errorSize);
        result = error == 0 ? 0 : -1;
      } else {
        result = -1;
      }
    }
    (void)::fcntl(candidate.get(), F_SETFL, flags);
    if (result == 0) {
      configureTimeout(candidate.get(), timeout);
      connected = std::move(candidate);
      break;
    }
  }
  ::freeaddrinfo(addresses);
  return connected;
}

int createListener(const Endpoint &endpoint) {
  addrinfo hints{};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_flags = AI_PASSIVE;
  addrinfo *addresses = nullptr;
  const std::string port = std::to_string(endpoint.port);
  const char *host = endpoint.host.empty() ? nullptr : endpoint.host.c_str();
  if (::getaddrinfo(host, port.c_str(), &hints, &addresses) != 0)
    return -1;

  int listener = -1;
  for (addrinfo *address = addresses; address; address = address->ai_next) {
    Socket candidate(::socket(address->ai_family, address->ai_socktype,
                              address->ai_protocol));
    if (candidate.get() < 0)
      continue;
    int reuse = 1;
    (void)::setsockopt(candidate.get(), SOL_SOCKET, SO_REUSEADDR, &reuse,
                       sizeof(reuse));
    if (::bind(candidate.get(), address->ai_addr,
               static_cast<socklen_t>(address->ai_addrlen)) == 0 &&
        ::listen(candidate.get(), 128) == 0) {
      listener = candidate.release();
      break;
    }
  }
  ::freeaddrinfo(addresses);
  return listener;
}

std::optional<std::string> callEndpoint(const Endpoint &endpoint,
                                        std::chrono::milliseconds timeout,
                                        std::uint8_t type,
                                        const std::string &payload,
                                        std::uint8_t expectedResponse) {
  Socket socket = connectTo(endpoint, timeout);
  if (socket.get() < 0 || !sendFrame(socket.get(), type, payload))
    return std::nullopt;
  auto response = receiveFrame(socket.get());
  if (!response || response->first != expectedResponse)
    return std::nullopt;
  return std::move(response->second);
}

} // namespace

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
  listenSocket_ = createListener(listenEndpoint_);
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
  auto payload =
      call(target, kRequestVote, encode(request), kRequestVoteResponse);
  if (!payload)
    return std::nullopt;
  return decodeRequestVoteResponse(*payload);
}

std::optional<AppendEntriesResponse>
TcpTransport::appendEntries(NodeId target,
                            const AppendEntriesRequest &request) {
  auto payload =
      call(target, kAppendEntries, encode(request), kAppendEntriesResponse);
  if (!payload)
    return std::nullopt;
  return decodeAppendEntriesResponse(*payload);
}

std::optional<InstallSnapshotResponse>
TcpTransport::installSnapshot(NodeId target,
                              const InstallSnapshotRequest &request) {
  auto payload =
      call(target, kInstallSnapshot, encode(request), kInstallSnapshotResponse);
  if (!payload)
    return std::nullopt;
  return decodeInstallSnapshotResponse(*payload);
}

std::optional<std::string> TcpTransport::call(NodeId target, std::uint8_t type,
                                              const std::string &payload,
                                              std::uint8_t expectedResponse) {
  auto endpoint = peers_.find(target);
  if (endpoint == peers_.end())
    return std::nullopt;
  return callEndpoint(endpoint->second, rpcTimeout_, type, payload,
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
    configureTimeout(socket, rpcTimeout_ + std::chrono::milliseconds(2000));
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
    auto frame = receiveFrame(socket);
    if (!frame)
      return false;
    if (frame->first == kClientRequest) {
      {
        std::lock_guard<std::mutex> lock(clientQueueMutex_);
        clientConnections_.push_back({socket, std::move(frame->second)});
      }
      clientQueueCondition_.notify_one();
      return true;
    }
    switch (frame->first) {
    case kRequestVote: {
      const auto response =
          node_->handleRequestVote(decodeRequestVote(frame->second));
      (void)sendFrame(socket, kRequestVoteResponse, encode(response));
      break;
    }
    case kAppendEntries: {
      const auto response =
          node_->handleAppendEntries(decodeAppendEntries(frame->second));
      (void)sendFrame(socket, kAppendEntriesResponse, encode(response));
      break;
    }
    case kInstallSnapshot: {
      const auto response =
          node_->handleInstallSnapshot(decodeInstallSnapshot(frame->second));
      (void)sendFrame(socket, kInstallSnapshotResponse, encode(response));
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
    const auto request = decodeClientRequest(payload);
    const auto timeout = std::chrono::milliseconds(request.timeoutMillis);
    ClientWireResponse response;
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
    (void)sendFrame(socket, kClientResponse, encode(response));
  } catch (const std::exception &) {
  }
}

TcpKvClient::TcpKvClient(std::vector<Endpoint> servers,
                         std::chrono::milliseconds timeout)
    : servers_(std::move(servers)), timeout_(timeout) {
  if (servers_.empty() || timeout_ <= std::chrono::milliseconds(0) ||
      timeout_ > std::chrono::milliseconds(kMaxClientTimeoutMillis))
    throw std::invalid_argument("KV client requires servers and a timeout");
}

ReadResult TcpKvClient::get(const std::string &key) {
  ClientWireRequest request;
  request.read = true;
  request.command.operation = storage::KvOperation::Put;
  request.command.key = key;
  request.command.clientId = "read";
  request.command.requestId = 1;
  request.timeoutMillis = static_cast<std::uint32_t>(timeout_.count());

  for (const std::size_t index : attemptOrder()) {
    auto payload = call(servers_[index], encode(request));
    if (!payload)
      continue;
    const auto response = decodeClientResponse(*payload);
    if (response.status == ClientStatus::Committed) {
      leaderIndex_ = index;
      return {response.status, response.leaderId, response.value};
    }
  }
  return {ClientStatus::Timeout, std::nullopt, std::nullopt};
}

SubmitResult TcpKvClient::put(const std::string &key, const std::string &value,
                              const std::string &clientId,
                              std::uint64_t requestId) {
  return write(storage::KvOperation::Put, key, value, clientId, requestId);
}

SubmitResult TcpKvClient::append(const std::string &key,
                                 const std::string &value,
                                 const std::string &clientId,
                                 std::uint64_t requestId) {
  return write(storage::KvOperation::Append, key, value, clientId, requestId);
}

SubmitResult TcpKvClient::erase(const std::string &key,
                                const std::string &clientId,
                                std::uint64_t requestId) {
  return write(storage::KvOperation::Erase, key, "", clientId, requestId);
}

std::optional<std::string> TcpKvClient::call(const Endpoint &endpoint,
                                             const std::string &payload) {
  return callEndpoint(endpoint, timeout_ + std::chrono::milliseconds(100),
                      kClientRequest, payload, kClientResponse);
}

SubmitResult TcpKvClient::write(storage::KvOperation operation,
                                const std::string &key,
                                const std::string &value,
                                const std::string &clientId,
                                std::uint64_t requestId) {
  ClientWireRequest request{false,
                            {operation, key, value, clientId, requestId},
                            static_cast<std::uint32_t>(timeout_.count())};
  for (const std::size_t index : attemptOrder()) {
    auto payload = call(servers_[index], encode(request));
    if (!payload)
      continue;
    const auto response = decodeClientResponse(*payload);
    if (response.status == ClientStatus::Committed) {
      leaderIndex_ = index;
      return {response.status, response.leaderId, response.applyResult};
    }
  }
  return {ClientStatus::Timeout, std::nullopt, std::nullopt};
}

std::vector<std::size_t> TcpKvClient::attemptOrder() const {
  std::vector<std::size_t> order;
  order.reserve(servers_.size());
  if (leaderIndex_ && *leaderIndex_ < servers_.size())
    order.push_back(*leaderIndex_);
  for (std::size_t index = 0; index < servers_.size(); ++index) {
    if (!leaderIndex_ || index != *leaderIndex_)
      order.push_back(index);
  }
  return order;
}

} // namespace cppcache::raft

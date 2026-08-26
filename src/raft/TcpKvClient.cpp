#include "raft/TcpKvClient.h"

#include "TcpProtocol.h"
#include "TcpSocket.h"

#include <stdexcept>
#include <utility>

namespace cppcache::raft {

TcpKvClient::TcpKvClient(std::vector<Endpoint> servers,
                         std::chrono::milliseconds timeout)
    : servers_(std::move(servers)), timeout_(timeout) {
  if (servers_.empty() || timeout_ <= std::chrono::milliseconds(0) ||
      timeout_ > std::chrono::milliseconds(tcp::kMaxClientTimeoutMillis))
    throw std::invalid_argument("KV client requires servers and a timeout");
}

ReadResult TcpKvClient::get(const std::string &key) {
  tcp::ClientWireRequest request;
  request.read = true;
  request.command.operation = storage::KvOperation::Put;
  request.command.key = key;
  request.command.clientId = "read";
  request.command.requestId = 1;
  request.timeoutMillis = static_cast<std::uint32_t>(timeout_.count());

  for (const std::size_t index : attemptOrder()) {
    auto payload = call(servers_[index], tcp::encode(request));
    if (!payload)
      continue;
    const auto response = tcp::decodeClientResponse(*payload);
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
  return tcp::callEndpoint(endpoint, timeout_ + std::chrono::milliseconds(100),
                           tcp::kClientRequest, payload, tcp::kClientResponse);
}

SubmitResult TcpKvClient::write(storage::KvOperation operation,
                                const std::string &key,
                                const std::string &value,
                                const std::string &clientId,
                                std::uint64_t requestId) {
  tcp::ClientWireRequest request{false,
                                 {operation, key, value, clientId, requestId},
                                 static_cast<std::uint32_t>(timeout_.count())};
  for (const std::size_t index : attemptOrder()) {
    auto payload = call(servers_[index], tcp::encode(request));
    if (!payload)
      continue;
    const auto response = tcp::decodeClientResponse(*payload);
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

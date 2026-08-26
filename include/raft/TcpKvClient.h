#pragma once

#include "raft/RaftNode.h"
#include "raft/TcpEndpoint.h"

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace cppcache::raft {

class TcpKvClient {
public:
  explicit TcpKvClient(
      std::vector<Endpoint> servers,
      std::chrono::milliseconds timeout = std::chrono::milliseconds(1000));

  ReadResult get(const std::string &key);
  SubmitResult put(const std::string &key, const std::string &value,
                   const std::string &clientId, std::uint64_t requestId);
  SubmitResult append(const std::string &key, const std::string &value,
                      const std::string &clientId, std::uint64_t requestId);
  SubmitResult erase(const std::string &key, const std::string &clientId,
                     std::uint64_t requestId);

private:
  std::optional<std::string> call(const Endpoint &endpoint,
                                  const std::string &payload);
  SubmitResult write(storage::KvOperation operation, const std::string &key,
                     const std::string &value, const std::string &clientId,
                     std::uint64_t requestId);
  std::vector<std::size_t> attemptOrder() const;

  std::vector<Endpoint> servers_;
  std::chrono::milliseconds timeout_;
  std::optional<std::size_t> leaderIndex_;
};

} // namespace cppcache::raft

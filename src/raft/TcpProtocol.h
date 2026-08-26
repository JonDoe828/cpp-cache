#pragma once

#include "raft/RaftNode.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace cppcache::raft::tcp {

inline constexpr std::uint32_t kMaxFrameSize = 64 * 1024 * 1024;
inline constexpr std::uint32_t kMaxClientTimeoutMillis = 10'000;

inline constexpr std::uint8_t kRequestVote = 1;
inline constexpr std::uint8_t kRequestVoteResponse = 2;
inline constexpr std::uint8_t kAppendEntries = 3;
inline constexpr std::uint8_t kAppendEntriesResponse = 4;
inline constexpr std::uint8_t kInstallSnapshot = 5;
inline constexpr std::uint8_t kInstallSnapshotResponse = 6;
inline constexpr std::uint8_t kClientRequest = 10;
inline constexpr std::uint8_t kClientResponse = 11;

struct ClientWireRequest {
  bool read{false};
  storage::KvCommand command;
  std::uint32_t timeoutMillis{0};
};

struct ClientWireResponse {
  ClientStatus status{ClientStatus::Timeout};
  std::optional<NodeId> leaderId;
  std::optional<storage::ApplyResult> applyResult;
  std::optional<std::string> value;
};

std::string encode(const RequestVoteRequest &request);
RequestVoteRequest decodeRequestVote(std::string_view bytes);

std::string encode(const RequestVoteResponse &response);
RequestVoteResponse decodeRequestVoteResponse(std::string_view bytes);

std::string encode(const AppendEntriesRequest &request);
AppendEntriesRequest decodeAppendEntries(std::string_view bytes);

std::string encode(const AppendEntriesResponse &response);
AppendEntriesResponse decodeAppendEntriesResponse(std::string_view bytes);

std::string encode(const InstallSnapshotRequest &request);
InstallSnapshotRequest decodeInstallSnapshot(std::string_view bytes);

std::string encode(const InstallSnapshotResponse &response);
InstallSnapshotResponse decodeInstallSnapshotResponse(std::string_view bytes);

std::string encode(const ClientWireRequest &request);
ClientWireRequest decodeClientRequest(std::string_view bytes);

std::string encode(const ClientWireResponse &response);
ClientWireResponse decodeClientResponse(std::string_view bytes);

} // namespace cppcache::raft::tcp

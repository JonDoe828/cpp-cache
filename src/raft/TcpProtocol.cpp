#include "TcpProtocol.h"

#include <arpa/inet.h>

#include <cstddef>
#include <cstring>
#include <stdexcept>
#include <utility>

namespace cppcache::raft::tcp {
namespace {

constexpr std::uint32_t kMaxEntries = 100'000;

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

} // namespace

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

} // namespace cppcache::raft::tcp

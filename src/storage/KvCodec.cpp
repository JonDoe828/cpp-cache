#include "storage/KvCodec.h"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <type_traits>
#include <vector>

namespace cppcache::storage {
namespace {

constexpr std::uint64_t kMaxFieldSize = 64 * 1024 * 1024;
constexpr std::uint64_t kMaxEntries = 1'000'000;

class Writer {
public:
  template <typename T> void number(T value) {
    static_assert(std::is_integral_v<T>);
    using Unsigned = std::make_unsigned_t<T>;
    const auto unsignedValue = static_cast<Unsigned>(value);
    for (std::size_t byte = sizeof(T); byte > 0; --byte) {
      const auto shift = static_cast<unsigned>((byte - 1) * 8);
      bytes_.push_back(
          static_cast<char>((unsignedValue >> shift) & Unsigned{0xff}));
    }
  }

  void string(std::string_view value) {
    number(static_cast<std::uint64_t>(value.size()));
    bytes_.append(value);
  }

  std::string finish() { return std::move(bytes_); }

private:
  std::string bytes_;
};

class Reader {
public:
  explicit Reader(std::string_view bytes) : bytes_(bytes) {}

  template <typename T> T number() {
    static_assert(std::is_integral_v<T>);
    require(sizeof(T));
    using Unsigned = std::make_unsigned_t<T>;
    Unsigned value{};
    for (std::size_t byte = 0; byte < sizeof(T); ++byte) {
      value = static_cast<Unsigned>(
          (value << 8) |
          static_cast<unsigned char>(bytes_[position_++]));
    }
    return static_cast<T>(value);
  }

  std::string string() {
    const auto size = number<std::uint64_t>();
    if (size > kMaxFieldSize)
      throw std::runtime_error("KV field exceeds size limit");
    require(static_cast<std::size_t>(size));
    std::string value(bytes_.substr(position_, static_cast<std::size_t>(size)));
    position_ += static_cast<std::size_t>(size);
    return value;
  }

  void finish() const {
    if (position_ != bytes_.size())
      throw std::runtime_error("trailing bytes in KV payload");
  }

private:
  void require(std::size_t size) const {
    if (size > bytes_.size() - position_)
      throw std::runtime_error("truncated KV payload");
  }

  std::string_view bytes_;
  std::size_t position_{0};
};

void writeCommand(Writer &writer, const KvCommand &command) {
  writer.number(static_cast<std::uint8_t>(command.operation));
  writer.string(command.key);
  writer.string(command.value);
  writer.string(command.clientId);
  writer.number(command.requestId);
}

KvCommand readCommand(Reader &reader) {
  const auto operation = reader.number<std::uint8_t>();
  if (operation > static_cast<std::uint8_t>(KvOperation::NoOp))
    throw std::runtime_error("invalid KV operation");
  return {static_cast<KvOperation>(operation), reader.string(), reader.string(),
          reader.string(), reader.number<std::uint64_t>()};
}

} // namespace

std::string encodeCommand(const KvCommand &command) {
  Writer writer;
  writeCommand(writer, command);
  return writer.finish();
}

KvCommand decodeCommand(std::string_view bytes) {
  Reader reader(bytes);
  KvCommand command = readCommand(reader);
  reader.finish();
  return command;
}

std::string encodeSnapshot(const KvSnapshot &snapshot) {
  Writer writer;

  std::vector<std::string> keys;
  keys.reserve(snapshot.entries.size());
  for (const auto &[key, value] : snapshot.entries) {
    (void)value;
    keys.push_back(key);
  }
  std::sort(keys.begin(), keys.end());
  writer.number(static_cast<std::uint64_t>(keys.size()));
  for (const auto &key : keys) {
    writer.string(key);
    writer.string(snapshot.entries.at(key));
  }

  keys.clear();
  keys.reserve(snapshot.clients.size());
  for (const auto &[client, record] : snapshot.clients) {
    (void)record;
    keys.push_back(client);
  }
  std::sort(keys.begin(), keys.end());
  writer.number(static_cast<std::uint64_t>(keys.size()));
  for (const auto &client : keys) {
    writer.string(client);
    const auto &record = snapshot.clients.at(client);
    writer.number(record.requestId);
    writer.number(record.applySequence);
  }
  writer.number(snapshot.lastApplySequence);
  return writer.finish();
}

KvSnapshot decodeSnapshot(std::string_view bytes) {
  Reader reader(bytes);
  KvSnapshot snapshot;
  const auto entryCount = reader.number<std::uint64_t>();
  if (entryCount > kMaxEntries)
    throw std::runtime_error("KV snapshot has too many entries");
  for (std::uint64_t index = 0; index < entryCount; ++index) {
    std::string key = reader.string();
    std::string value = reader.string();
    if (!snapshot.entries.emplace(std::move(key), std::move(value)).second)
      throw std::runtime_error("duplicate key in KV snapshot");
  }

  const auto clientCount = reader.number<std::uint64_t>();
  if (clientCount > KvStateMachine::kMaxTrackedClients)
    throw std::runtime_error("KV snapshot has too many clients");
  for (std::uint64_t index = 0; index < clientCount; ++index) {
    std::string client = reader.string();
    ClientRequestRecord record;
    record.requestId = reader.number<std::uint64_t>();
    record.applySequence = reader.number<std::uint64_t>();
    if (!snapshot.clients.emplace(std::move(client), record).second)
      throw std::runtime_error("duplicate client in KV snapshot");
  }
  snapshot.lastApplySequence = reader.number<std::uint64_t>();
  reader.finish();
  return snapshot;
}

} // namespace cppcache::storage

#include "raft/FileRaftStorage.h"

#include <fcntl.h>
#include <unistd.h>

#include <array>
#include <cstdint>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <system_error>
#include <type_traits>

namespace cppcache::raft {
namespace {

constexpr std::array<char, 8> kMagic{'C', 'P', 'P', 'R', 'A', 'F', 'T', '4'};
constexpr std::uint64_t kMaxStringSize = 64 * 1024 * 1024;
constexpr std::uint64_t kMaxLogEntries = 1'000'000;

template <typename T> void writeNumber(std::ostream &output, T value) {
  static_assert(std::is_integral_v<T>);
  using Unsigned = std::make_unsigned_t<T>;
  const auto unsignedValue = static_cast<Unsigned>(value);
  for (std::size_t byte = sizeof(T); byte > 0; --byte) {
    const auto shift = static_cast<unsigned>((byte - 1) * 8);
    output.put(static_cast<char>((unsignedValue >> shift) & Unsigned{0xff}));
  }
  if (!output)
    throw std::runtime_error("failed to write raft state");
}

template <typename T> T readNumber(std::istream &input) {
  static_assert(std::is_integral_v<T>);
  using Unsigned = std::make_unsigned_t<T>;
  Unsigned value{};
  for (std::size_t byte = 0; byte < sizeof(T); ++byte) {
    const int next = input.get();
    if (next == std::char_traits<char>::eof())
      throw std::runtime_error("truncated raft state");
    value = static_cast<Unsigned>(
        (value << 8) | static_cast<unsigned char>(next));
  }
  return static_cast<T>(value);
}

void writeString(std::ostream &output, const std::string &value) {
  writeNumber(output, static_cast<std::uint64_t>(value.size()));
  output.write(value.data(), static_cast<std::streamsize>(value.size()));
  if (!output)
    throw std::runtime_error("failed to write raft state string");
}

std::string readString(std::istream &input) {
  const auto size = readNumber<std::uint64_t>(input);
  if (size > kMaxStringSize)
    throw std::runtime_error("raft state string exceeds size limit");
  std::string value(static_cast<std::size_t>(size), '\0');
  input.read(value.data(), static_cast<std::streamsize>(value.size()));
  if (!input)
    throw std::runtime_error("truncated raft state string");
  return value;
}

void writeCommand(std::ostream &output, const storage::KvCommand &command) {
  writeNumber(output, static_cast<std::uint8_t>(command.operation));
  writeString(output, command.key);
  writeString(output, command.value);
  writeString(output, command.clientId);
  writeNumber(output, command.requestId);
}

storage::KvCommand readCommand(std::istream &input) {
  const auto operation = readNumber<std::uint8_t>(input);
  if (operation > static_cast<std::uint8_t>(storage::KvOperation::NoOp))
    throw std::runtime_error("invalid operation in raft state");
  return {static_cast<storage::KvOperation>(operation), readString(input),
          readString(input), readString(input),
          readNumber<std::uint64_t>(input)};
}

} // namespace

FileRaftStorage::FileRaftStorage(std::filesystem::path path)
    : path_(std::move(path)) {
  if (path_.empty())
    throw std::invalid_argument("raft storage path must not be empty");
}

PersistedRaftState FileRaftStorage::load() const {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!std::filesystem::exists(path_))
    return {};

  std::ifstream input(path_, std::ios::binary);
  if (!input)
    throw std::runtime_error("failed to open raft state for reading");

  std::array<char, kMagic.size()> magic{};
  input.read(magic.data(), static_cast<std::streamsize>(magic.size()));
  if (magic != kMagic)
    throw std::runtime_error("invalid raft state header");

  PersistedRaftState state;
  state.currentTerm = readNumber<Term>(input);
  const bool hasVote = readNumber<std::uint8_t>(input) != 0;
  if (hasVote)
    state.votedFor = readNumber<NodeId>(input);
  state.snapshotIndex = readNumber<LogIndex>(input);
  state.snapshotTerm = readNumber<Term>(input);
  state.snapshot = readString(input);
  state.commitIndex = readNumber<LogIndex>(input);

  const auto entryCount = readNumber<std::uint64_t>(input);
  if (entryCount > kMaxLogEntries)
    throw std::runtime_error("raft log exceeds entry limit");
  state.log.reserve(static_cast<std::size_t>(entryCount));
  for (std::uint64_t index = 0; index < entryCount; ++index)
    state.log.push_back({readNumber<Term>(input), readCommand(input)});
  return state;
}

void FileRaftStorage::save(const PersistedRaftState &state) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!path_.parent_path().empty())
    std::filesystem::create_directories(path_.parent_path());

  auto temporaryPath = path_;
  temporaryPath += ".tmp";
  {
    std::ofstream output(temporaryPath, std::ios::binary | std::ios::trunc);
    if (!output)
      throw std::runtime_error("failed to open temporary raft state");

    output.write(kMagic.data(), static_cast<std::streamsize>(kMagic.size()));
    writeNumber(output, state.currentTerm);
    writeNumber(output, static_cast<std::uint8_t>(state.votedFor.has_value()));
    if (state.votedFor)
      writeNumber(output, *state.votedFor);
    writeNumber(output, state.snapshotIndex);
    writeNumber(output, state.snapshotTerm);
    writeString(output, state.snapshot);
    writeNumber(output, state.commitIndex);
    writeNumber(output, static_cast<std::uint64_t>(state.log.size()));
    for (const auto &entry : state.log) {
      writeNumber(output, entry.term);
      writeCommand(output, entry.command);
    }
    output.flush();
    if (!output)
      throw std::runtime_error("failed to flush raft state");
  }

  const int temporaryFile = ::open(temporaryPath.c_str(), O_RDONLY);
  if (temporaryFile < 0 || ::fsync(temporaryFile) != 0) {
    if (temporaryFile >= 0)
      ::close(temporaryFile);
    throw std::runtime_error("failed to sync temporary raft state");
  }
  ::close(temporaryFile);

  std::error_code error;
  std::filesystem::rename(temporaryPath, path_, error);
  if (error)
    throw std::runtime_error("failed to replace raft state: " +
                             error.message());

  const std::filesystem::path parent = path_.parent_path().empty()
                                           ? std::filesystem::path(".")
                                           : path_.parent_path();
  const int directory = ::open(parent.c_str(), O_RDONLY | O_DIRECTORY);
  if (directory < 0 || ::fsync(directory) != 0) {
    if (directory >= 0)
      ::close(directory);
    throw std::runtime_error("failed to sync raft state directory");
  }
  ::close(directory);
}

} // namespace cppcache::raft

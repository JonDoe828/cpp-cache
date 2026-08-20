#pragma once

#include "storage/IKvStore.h"
#include "storage/KvCommand.h"

#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>

namespace cppcache::storage {

struct ApplyResult {
  bool applied;
  std::optional<std::string> value;
};

struct ClientRequestRecord {
  std::uint64_t requestId{0};
  std::uint64_t applySequence{0};

  bool operator==(const ClientRequestRecord &) const = default;
};

struct KvSnapshot {
  IKvStore::Entries entries;
  std::unordered_map<std::string, ClientRequestRecord> clients;
  std::uint64_t lastApplySequence{0};
};

class KvStateMachine {
public:
  static constexpr std::size_t kMaxTrackedClients = 4096;

  explicit KvStateMachine(std::unique_ptr<IKvStore> store);

  ApplyResult apply(const KvCommand &command);
  std::optional<std::string> get(const std::string &key);
  std::size_t size() const;

  KvSnapshot snapshot() const;
  void restore(const KvSnapshot &snapshot);

private:
  mutable std::mutex mutex_;
  std::unique_ptr<IKvStore> store_;
  std::unordered_map<std::string, ClientRequestRecord> clients_;
  std::map<std::uint64_t, std::string> clientsBySequence_;
  std::uint64_t lastApplySequence_{0};
};

} // namespace cppcache::storage

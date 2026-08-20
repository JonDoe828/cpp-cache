#pragma once

#include "raft/IRaftStorage.h"

#include <filesystem>
#include <mutex>

namespace cppcache::raft {

class FileRaftStorage final : public IRaftStorage {
public:
  explicit FileRaftStorage(std::filesystem::path path);

  PersistedRaftState load() const override;
  void save(const PersistedRaftState &state) override;

private:
  std::filesystem::path path_;
  mutable std::mutex mutex_;
};

} // namespace cppcache::raft

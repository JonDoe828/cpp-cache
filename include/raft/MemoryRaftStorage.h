#pragma once

#include "raft/IRaftStorage.h"

#include <mutex>

namespace cppcache::raft {

class MemoryRaftStorage final : public IRaftStorage {
public:
  PersistedRaftState load() const override;
  void save(const PersistedRaftState &state) override;

private:
  mutable std::mutex mutex_;
  PersistedRaftState state_;
};

} // namespace cppcache::raft

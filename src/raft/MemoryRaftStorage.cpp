#include "raft/MemoryRaftStorage.h"

namespace cppcache::raft {

PersistedRaftState MemoryRaftStorage::load() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return state_;
}

void MemoryRaftStorage::save(const PersistedRaftState &state) {
  std::lock_guard<std::mutex> lock(mutex_);
  state_ = state;
}

} // namespace cppcache::raft

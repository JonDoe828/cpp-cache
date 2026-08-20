#pragma once

#include "raft/RaftTypes.h"

namespace cppcache::raft {

class IRaftStorage {
public:
  virtual ~IRaftStorage() = default;
  virtual PersistedRaftState load() const = 0;
  virtual void save(const PersistedRaftState &state) = 0;
};

} // namespace cppcache::raft

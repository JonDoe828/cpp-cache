#pragma once

#include "storage/IKvStore.h"

#include <shared_mutex>

namespace cppcache::storage {

class InMemoryKvStore final : public IKvStore {
public:
  std::optional<std::string> get(const std::string &key) override;
  void put(const std::string &key, const std::string &value) override;
  bool erase(const std::string &key) override;
  std::size_t size() const override;

  Entries snapshot() const override;
  void restore(const Entries &entries) override;

private:
  mutable std::shared_mutex mutex_;
  Entries entries_;
};

} // namespace cppcache::storage

#pragma once

#include "cache/LruCache.h"
#include "storage/IKvStore.h"

#include <memory>
#include <mutex>

namespace cppcache::storage {

class CachedKvStore final : public IKvStore {
public:
  CachedKvStore(std::unique_ptr<IKvStore> backingStore,
                std::size_t cacheCapacity);

  std::optional<std::string> get(const std::string &key) override;
  void put(const std::string &key, const std::string &value) override;
  bool erase(const std::string &key) override;
  std::size_t size() const override;

  Entries snapshot() const override;
  void restore(const Entries &entries) override;

private:
  mutable std::mutex mutex_;
  std::unique_ptr<IKvStore> backingStore_;
  cppcache::cache::LruCache<std::string, std::string> cache_;
};

} // namespace cppcache::storage

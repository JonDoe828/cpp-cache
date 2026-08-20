#include "storage/CachedKvStore.h"

#include <stdexcept>
#include <utility>

namespace cppcache::storage {

CachedKvStore::CachedKvStore(std::unique_ptr<IKvStore> backingStore,
                             std::size_t cacheCapacity)
    : backingStore_(std::move(backingStore)), cache_(cacheCapacity) {
  if (!backingStore_)
    throw std::invalid_argument("backing store must not be null");
}

std::optional<std::string> CachedKvStore::get(const std::string &key) {
  std::lock_guard<std::mutex> lock(mutex_);

  std::string value;
  if (cache_.get(key, value))
    return value;

  auto storedValue = backingStore_->get(key);
  if (storedValue)
    cache_.put(key, *storedValue);
  return storedValue;
}

void CachedKvStore::put(const std::string &key, const std::string &value) {
  std::lock_guard<std::mutex> lock(mutex_);
  cache_.remove(key);
  backingStore_->put(key, value);
  cache_.put(key, value);
}

bool CachedKvStore::erase(const std::string &key) {
  std::lock_guard<std::mutex> lock(mutex_);
  cache_.remove(key);
  return backingStore_->erase(key);
}

std::size_t CachedKvStore::size() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return backingStore_->size();
}

IKvStore::Entries CachedKvStore::snapshot() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return backingStore_->snapshot();
}

void CachedKvStore::restore(const Entries &entries) {
  std::lock_guard<std::mutex> lock(mutex_);
  cache_.purge();
  backingStore_->restore(entries);
}

} // namespace cppcache::storage

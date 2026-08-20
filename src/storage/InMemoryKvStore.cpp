#include "storage/InMemoryKvStore.h"

#include <mutex>

namespace cppcache::storage {

std::optional<std::string> InMemoryKvStore::get(const std::string &key) {
  std::shared_lock<std::shared_mutex> lock(mutex_);
  auto it = entries_.find(key);
  if (it == entries_.end())
    return std::nullopt;
  return it->second;
}

void InMemoryKvStore::put(const std::string &key, const std::string &value) {
  std::unique_lock<std::shared_mutex> lock(mutex_);
  entries_[key] = value;
}

bool InMemoryKvStore::erase(const std::string &key) {
  std::unique_lock<std::shared_mutex> lock(mutex_);
  return entries_.erase(key) != 0;
}

std::size_t InMemoryKvStore::size() const {
  std::shared_lock<std::shared_mutex> lock(mutex_);
  return entries_.size();
}

IKvStore::Entries InMemoryKvStore::snapshot() const {
  std::shared_lock<std::shared_mutex> lock(mutex_);
  return entries_;
}

void InMemoryKvStore::restore(const Entries &entries) {
  std::unique_lock<std::shared_mutex> lock(mutex_);
  entries_ = entries;
}

} // namespace cppcache::storage

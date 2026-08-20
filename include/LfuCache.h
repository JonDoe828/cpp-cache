#pragma once

#include "ICachePolicy.h"

#include <algorithm>
#include <cstddef>
#include <functional>
#include <list>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

template <typename Key, typename Value>
class LfuCache : public ICachePolicy<Key, Value> {
public:
  explicit LfuCache(std::size_t capacity, int maxAverageNum = 1000000)
      : capacity_(capacity),
        maxAverageNum_(maxAverageNum > 0
                           ? static_cast<std::size_t>(maxAverageNum)
                           : std::size_t{1}) {}

  explicit LfuCache(int capacity, int maxAverageNum = 1000000)
      : LfuCache(capacity > 0 ? static_cast<std::size_t>(capacity) : 0,
                 maxAverageNum) {}

  ~LfuCache() override = default;

  void put(const Key &key, const Value &value) override {
    if (capacity_ == 0)
      return;

    std::lock_guard<std::mutex> lock(mutex_);
    auto nodeIt = nodes_.find(key);
    if (nodeIt != nodes_.end()) {
      nodeIt->second.value = value;
      promote(nodeIt);
      return;
    }

    if (nodes_.size() >= capacity_)
      evictLeastFrequent();

    auto &bucket = frequencyBuckets_[1];
    bucket.push_back(key);
    auto position = std::prev(bucket.end());
    nodes_.emplace(key, Entry{value, 1, position});
    minFrequency_ = 1;
    ++totalFrequency_;
    ageIfNeeded();
  }

  bool get(const Key &key, Value &value) override {
    std::lock_guard<std::mutex> lock(mutex_);
    auto nodeIt = nodes_.find(key);
    if (nodeIt == nodes_.end())
      return false;

    value = nodeIt->second.value;
    promote(nodeIt);
    return true;
  }

  Value get(const Key &key) override {
    Value value{};
    (void)get(key, value);
    return value;
  }

  void purge() {
    std::lock_guard<std::mutex> lock(mutex_);
    nodes_.clear();
    frequencyBuckets_.clear();
    minFrequency_ = 0;
    totalFrequency_ = 0;
  }

private:
  using FrequencyList = std::list<Key>;

  struct Entry {
    Value value;
    std::size_t frequency;
    typename FrequencyList::iterator position;
  };

  using NodeMap = std::unordered_map<Key, Entry>;
  using NodeIterator = typename NodeMap::iterator;

  void promote(NodeIterator nodeIt) {
    const std::size_t oldFrequency = nodeIt->second.frequency;
    auto bucketIt = frequencyBuckets_.find(oldFrequency);
    bucketIt->second.erase(nodeIt->second.position);

    if (bucketIt->second.empty()) {
      frequencyBuckets_.erase(bucketIt);
      if (minFrequency_ == oldFrequency)
        minFrequency_ = oldFrequency + 1;
    }

    const std::size_t newFrequency = oldFrequency + 1;
    auto &newBucket = frequencyBuckets_[newFrequency];
    newBucket.push_back(nodeIt->first);
    nodeIt->second.frequency = newFrequency;
    nodeIt->second.position = std::prev(newBucket.end());
    ++totalFrequency_;
    ageIfNeeded();
  }

  void evictLeastFrequent() {
    auto bucketIt = frequencyBuckets_.find(minFrequency_);
    if (bucketIt == frequencyBuckets_.end() || bucketIt->second.empty())
      return;

    Key key = bucketIt->second.front();
    bucketIt->second.pop_front();

    auto nodeIt = nodes_.find(key);
    if (nodeIt != nodes_.end()) {
      totalFrequency_ -= nodeIt->second.frequency;
      nodes_.erase(nodeIt);
    }

    if (bucketIt->second.empty())
      frequencyBuckets_.erase(bucketIt);
  }

  void ageIfNeeded() {
    if (nodes_.empty() || totalFrequency_ / nodes_.size() <= maxAverageNum_)
      return;

    std::vector<std::size_t> frequencies;
    frequencies.reserve(frequencyBuckets_.size());
    for (const auto &[frequency, keys] : frequencyBuckets_) {
      if (!keys.empty())
        frequencies.push_back(frequency);
    }
    std::sort(frequencies.begin(), frequencies.end());

    std::unordered_map<std::size_t, FrequencyList> rebuiltBuckets;
    std::size_t rebuiltTotal = 0;
    std::size_t rebuiltMinimum = 0;

    for (const std::size_t frequency : frequencies) {
      auto bucketIt = frequencyBuckets_.find(frequency);
      for (const Key &key : bucketIt->second) {
        auto nodeIt = nodes_.find(key);
        if (nodeIt == nodes_.end())
          continue;

        const std::size_t agedFrequency =
            std::max<std::size_t>(1, frequency / 2);
        auto &rebuiltBucket = rebuiltBuckets[agedFrequency];
        rebuiltBucket.push_back(key);
        nodeIt->second.frequency = agedFrequency;
        nodeIt->second.position = std::prev(rebuiltBucket.end());
        rebuiltTotal += agedFrequency;
        if (rebuiltMinimum == 0 || agedFrequency < rebuiltMinimum)
          rebuiltMinimum = agedFrequency;
      }
    }

    frequencyBuckets_.swap(rebuiltBuckets);
    totalFrequency_ = rebuiltTotal;
    minFrequency_ = rebuiltMinimum;
  }

  std::size_t capacity_;
  std::size_t maxAverageNum_;
  std::size_t minFrequency_{0};
  std::size_t totalFrequency_{0};
  std::mutex mutex_;
  NodeMap nodes_;
  std::unordered_map<std::size_t, FrequencyList> frequencyBuckets_;
};

template <typename Key, typename Value>
class KHashLfuCache : public ICachePolicy<Key, Value> {
public:
  KHashLfuCache(std::size_t capacity, int sliceNum, int maxAverageNum = 10)
      : capacity_(capacity), sliceNum_(resolveSliceCount(capacity, sliceNum)) {
    const std::size_t baseSize = capacity_ / sliceNum_;
    const std::size_t remainder = capacity_ % sliceNum_;

    for (std::size_t i = 0; i < sliceNum_; ++i) {
      const std::size_t sliceSize = baseSize + (i < remainder ? 1 : 0);
      lfuSliceCaches_.emplace_back(
          std::make_unique<LfuCache<Key, Value>>(sliceSize, maxAverageNum));
    }
  }

  void put(const Key &key, const Value &value) override {
    lfuSliceCaches_[sliceIndex(key)]->put(key, value);
  }

  bool get(const Key &key, Value &value) override {
    return lfuSliceCaches_[sliceIndex(key)]->get(key, value);
  }

  Value get(const Key &key) override {
    Value value{};
    (void)get(key, value);
    return value;
  }

  void purge() {
    for (auto &cache : lfuSliceCaches_)
      cache->purge();
  }

private:
  static std::size_t resolveSliceCount(std::size_t capacity, int requested) {
    std::size_t count = requested > 0
                            ? static_cast<std::size_t>(requested)
                            : std::max(1u, std::thread::hardware_concurrency());
    if (capacity > 0)
      count = std::min(count, capacity);
    return std::max<std::size_t>(1, count);
  }

  std::size_t sliceIndex(const Key &key) const {
    return std::hash<Key>{}(key) % sliceNum_;
  }

  std::size_t capacity_;
  std::size_t sliceNum_;
  std::vector<std::unique_ptr<LfuCache<Key, Value>>> lfuSliceCaches_;
};

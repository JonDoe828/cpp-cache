#pragma once

#include "cache/ICachePolicy.h"

#include <algorithm>
#include <cstddef>
#include <list>
#include <mutex>
#include <unordered_map>

namespace cppcache::cache {

template <typename Key, typename Value>
class ArcCache : public ICachePolicy<Key, Value> {
public:
  explicit ArcCache(std::size_t capacity = 10,
                    std::size_t /* transformThreshold */ = 2)
      : capacity_(capacity) {}

  explicit ArcCache(int capacity, int transformThreshold = 2)
      : ArcCache(capacity > 0 ? static_cast<std::size_t>(capacity) : 0,
                 transformThreshold > 0
                     ? static_cast<std::size_t>(transformThreshold)
                     : std::size_t{1}) {}

  ~ArcCache() override = default;

  void put(const Key &key, const Value &value) override {
    if (capacity_ == 0)
      return;

    std::lock_guard<std::mutex> lock(mutex_);

    auto t1It = t1Positions_.find(key);
    if (t1It != t1Positions_.end()) {
      values_[key] = value;
      promoteToT2(key, t1It);
      return;
    }

    auto t2It = t2Positions_.find(key);
    if (t2It != t2Positions_.end()) {
      values_[key] = value;
      moveToFront(t2_, key, t2It);
      return;
    }

    auto b1It = b1Positions_.find(key);
    if (b1It != b1Positions_.end()) {
      const std::size_t delta =
          std::max<std::size_t>(1, b2_.size() / b1_.size());
      targetT1Size_ = std::min(capacity_, targetT1Size_ + delta);
      replace(key);
      eraseFromList(b1_, b1Positions_, b1It);
      addResidentToFront(t2_, t2Positions_, key, value);
      return;
    }

    auto b2It = b2Positions_.find(key);
    if (b2It != b2Positions_.end()) {
      const std::size_t delta =
          std::max<std::size_t>(1, b1_.size() / b2_.size());
      targetT1Size_ = delta >= targetT1Size_ ? 0 : targetT1Size_ - delta;
      replace(key);
      eraseFromList(b2_, b2Positions_, b2It);
      addResidentToFront(t2_, t2Positions_, key, value);
      return;
    }

    makeRoomForNewKey(key);
    addResidentToFront(t1_, t1Positions_, key, value);
  }

  bool get(const Key &key, Value &value) override {
    std::lock_guard<std::mutex> lock(mutex_);

    auto t1It = t1Positions_.find(key);
    if (t1It != t1Positions_.end()) {
      value = values_.at(key);
      promoteToT2(key, t1It);
      return true;
    }

    auto t2It = t2Positions_.find(key);
    if (t2It == t2Positions_.end())
      return false;

    value = values_.at(key);
    moveToFront(t2_, key, t2It);
    return true;
  }

  Value get(const Key &key) override {
    Value value{};
    (void)get(key, value);
    return value;
  }

  std::size_t size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return values_.size();
  }

private:
  using KeyList = std::list<Key>;
  using PositionMap = std::unordered_map<Key, typename KeyList::iterator>;
  using PositionIterator = typename PositionMap::iterator;

  static void eraseFromList(KeyList &list, PositionMap &positions,
                            PositionIterator positionIt) {
    list.erase(positionIt->second);
    positions.erase(positionIt);
  }

  static void moveToFront(KeyList &list, const Key &key,
                          PositionIterator positionIt) {
    list.erase(positionIt->second);
    list.push_front(key);
    positionIt->second = list.begin();
  }

  void promoteToT2(const Key &key, PositionIterator t1It) {
    eraseFromList(t1_, t1Positions_, t1It);
    t2_.push_front(key);
    t2Positions_[key] = t2_.begin();
  }

  void addResidentToFront(KeyList &list, PositionMap &positions, const Key &key,
                          const Value &value) {
    list.push_front(key);
    positions[key] = list.begin();
    values_[key] = value;
  }

  void addGhostToFront(KeyList &list, PositionMap &positions, const Key &key) {
    list.push_front(key);
    positions[key] = list.begin();
  }

  static void removeLeastRecent(KeyList &list, PositionMap &positions) {
    if (list.empty())
      return;
    positions.erase(list.back());
    list.pop_back();
  }

  void evictT1ToB1() {
    const Key key = t1_.back();
    t1Positions_.erase(key);
    t1_.pop_back();
    values_.erase(key);
    addGhostToFront(b1_, b1Positions_, key);
  }

  void evictT2ToB2() {
    const Key key = t2_.back();
    t2Positions_.erase(key);
    t2_.pop_back();
    values_.erase(key);
    addGhostToFront(b2_, b2Positions_, key);
  }

  void replace(const Key &incomingKey) {
    const bool incomingFromB2 =
        b2Positions_.find(incomingKey) != b2Positions_.end();
    if (!t1_.empty() && (t1_.size() > targetT1Size_ ||
                         (incomingFromB2 && t1_.size() == targetT1Size_))) {
      evictT1ToB1();
    } else if (!t2_.empty()) {
      evictT2ToB2();
    } else if (!t1_.empty()) {
      evictT1ToB1();
    }
  }

  void makeRoomForNewKey(const Key &key) {
    if (t1_.size() + b1_.size() == capacity_) {
      if (t1_.size() < capacity_) {
        removeLeastRecent(b1_, b1Positions_);
        replace(key);
      } else {
        const Key evictedKey = t1_.back();
        t1Positions_.erase(evictedKey);
        t1_.pop_back();
        values_.erase(evictedKey);
      }
      return;
    }

    if (t1_.size() + b1_.size() < capacity_) {
      const std::size_t totalSize =
          t1_.size() + t2_.size() + b1_.size() + b2_.size();
      if (totalSize >= capacity_) {
        if (totalSize >= 2 * capacity_)
          removeLeastRecent(b2_, b2Positions_);
        replace(key);
      }
    }
  }

  std::size_t capacity_;
  std::size_t targetT1Size_{0};
  mutable std::mutex mutex_;

  std::unordered_map<Key, Value> values_;
  KeyList t1_;
  KeyList t2_;
  KeyList b1_;
  KeyList b2_;
  PositionMap t1Positions_;
  PositionMap t2Positions_;
  PositionMap b1Positions_;
  PositionMap b2Positions_;
};

} // namespace cppcache::cache

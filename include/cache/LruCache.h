#pragma once

#include "cache/ICachePolicy.h"
#include <algorithm>
#include <cassert>
#include <cstddef>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <unordered_map>
#include <vector>

namespace cppcache::cache {

template <typename Key, typename Value>
class LruCache : public ICachePolicy<Key, Value> {
public:
  explicit LruCache(std::size_t capacity) : capacity_(capacity) {
    initializeList();
  }

  explicit LruCache(int capacity)
      : LruCache(capacity > 0 ? static_cast<std::size_t>(capacity) : 0) {}

  ~LruCache() override = default;

  // 添加缓存
  void put(const Key &key, const Value &value) override {
    if (capacity_ == 0)
      return;

    std::lock_guard<std::mutex> lock(mutex_);
    auto it = nodeMap_.find(key);
    if (it != nodeMap_.end()) {
      // 如果在当前容器中,则更新value,并调用get方法，代表该数据刚被访问
      updateExistingNode(it->second, value);
      return;
    }

    addNewNode(key, value);
  }

  bool get(const Key &key, Value &value) override {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = nodeMap_.find(key);
    if (it != nodeMap_.end()) {
      moveToMostRecent(it->second);
      value = it->second->getValue();
      return true;
    }
    return false;
  }

  Value get(const Key &key) override {
    Value value{};
    // memset(&value, 0, sizeof(value));   // memset
    // 是按字节设置内存的，对于复杂类型（如 string）使用 memset
    // 可能会破坏对象的内部结构
    get(key, value);
    return value;
  }

  // 删除指定元素
  virtual void remove(const Key &key) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = nodeMap_.find(key);
    if (it != nodeMap_.end()) {
      removeNode(it->second);
      nodeMap_.erase(it);
    }
  }

  virtual void purge() {
    std::lock_guard<std::mutex> lock(mutex_);
    nodeMap_.clear();
    initializeList();
  }

private:
  struct Node {
    Key key_;
    Value value_;
    // std::size_t accessCount_{1};
    std::weak_ptr<Node> prev_;
    std::shared_ptr<Node> next_;

    Node() = default;
    Node(const Key &key, const Value &value) : key_(key), value_(value) {}

    const Key &getKey() const { return key_; }
    const Value &getValue() const { return value_; }
    void setValue(const Value &value) { value_ = value; }

    // std::size_t getAccessCount() const { return accessCount_; }
    // void incrementAccessCount() { ++accessCount_; }
  };

  using NodePtr = std::shared_ptr<Node>;
  using NodeMap = std::unordered_map<Key, NodePtr>;

  void initializeList() {
    // 创建首尾虚拟节点
    dummyHead_ = std::make_shared<Node>();
    dummyTail_ = std::make_shared<Node>();
    dummyHead_->next_ = dummyTail_;
    dummyTail_->prev_ = dummyHead_;
  }

  void updateExistingNode(NodePtr node, const Value &value) {
    node->setValue(value);
    moveToMostRecent(node);
  }

  void addNewNode(const Key &key, const Value &value) {
    if (nodeMap_.size() >= capacity_) {
      evictLeastRecent();
    }

    NodePtr newNode = std::make_shared<Node>(key, value);
    insertNode(newNode);
    nodeMap_.try_emplace(key, newNode);
  }

  // 将该节点移动到最新的位置
  void moveToMostRecent(NodePtr node) {
    removeNode(node);
    insertNode(node);
  }

  void removeNode(NodePtr node) {
    auto prev = node->prev_.lock();
    auto next = node->next_;
    if (!prev || !next)
      return; // 不在链上或 dummy

    prev->next_ = next;
    next->prev_ = prev;

    node->next_.reset();
    node->prev_.reset();
  }

  // 从尾部插入结点
  void insertNode(NodePtr node) {
    auto prev = dummyTail_->prev_.lock();
    assert(prev && "LRU list invariant broken: tail must have prev");

    node->next_ = dummyTail_;
    node->prev_ = prev;
    prev->next_ = node;
    dummyTail_->prev_ = node;
  }

  // 驱逐最近最少访问
  void evictLeastRecent() {
    NodePtr leastRecent = dummyHead_->next_;
    if (!leastRecent || leastRecent == dummyTail_)
      return; // 空链表不驱逐
    removeNode(leastRecent);
    nodeMap_.erase(leastRecent->getKey());
  }

private:
  std::size_t capacity_; // 缓存容量
  NodeMap nodeMap_;      // key -> Node
  std::mutex mutex_;
  NodePtr dummyHead_; // 虚拟头结点
  NodePtr dummyTail_;
};

// LRU优化：Lru-k版本。 通过继承的方式进行再优化
template <typename Key, typename Value>
class LruKCache : public LruCache<Key, Value> {
public:
  LruKCache(int capacity, int historyCapacity, int k)
      : LruCache<Key, Value>(capacity),
        k_(k > 0 ? static_cast<std::size_t>(k) : std::size_t{1}),
        historyList_(
            std::make_unique<LruCache<Key, HistoryEntry>>(historyCapacity)) {}

  bool get(const Key &key, Value &value) override {
    std::lock_guard<std::mutex> lock(k_mutex_);
    if (LruCache<Key, Value>::get(key, value))
      return true;

    HistoryEntry history;
    (void)historyList_->get(key, history);
    ++history.accessCount;

    if (history.accessCount >= k_ && history.value) {
      value = *history.value;
      historyList_->remove(key);
      LruCache<Key, Value>::put(key, value);
      return true;
    }

    historyList_->put(key, history);
    return false;
  }

  Value get(const Key &key) override {
    Value value{};
    (void)get(key, value);
    return value;
  }

  void put(const Key &key, const Value &value) override {
    std::lock_guard<std::mutex> lock(k_mutex_);
    Value existingValue{};
    if (LruCache<Key, Value>::get(key, existingValue)) {
      LruCache<Key, Value>::put(key, value);
      return;
    }

    HistoryEntry history;
    (void)historyList_->get(key, history);
    ++history.accessCount;
    history.value = value;

    if (history.accessCount >= k_) {
      historyList_->remove(key);
      LruCache<Key, Value>::put(key, value);
      return;
    }

    historyList_->put(key, history);
  }

  void remove(const Key &key) override {
    std::lock_guard<std::mutex> lock(k_mutex_);
    LruCache<Key, Value>::remove(key);
    historyList_->remove(key);
  }

  void purge() override {
    std::lock_guard<std::mutex> lock(k_mutex_);
    LruCache<Key, Value>::purge();
    historyList_->purge();
  }

private:
  struct HistoryEntry {
    std::size_t accessCount{0};
    std::optional<Value> value;
  };

  std::size_t k_;
  std::mutex k_mutex_;
  std::unique_ptr<LruCache<Key, HistoryEntry>> historyList_;
};

// lru优化：对lru进行分片，提高高并发使用的性能
template <typename Key, typename Value>
class ShardedLruCache : public ICachePolicy<Key, Value> {
public:
  ShardedLruCache(std::size_t capacity, int sliceNum)
      : capacity_(capacity), sliceNum_(resolveSliceCount(capacity, sliceNum)) {
    const std::size_t baseSize = capacity_ / sliceNum_;
    const std::size_t remainder = capacity_ % sliceNum_;
    for (std::size_t i = 0; i < sliceNum_; ++i) {
      const std::size_t sliceSize = baseSize + (i < remainder ? 1 : 0);
      slices_.emplace_back(std::make_unique<LruCache<Key, Value>>(sliceSize));
    }
  }

  void put(const Key &key, const Value &value) override {
    // 获取key的hash值，并计算出对应的分片索引
    slices_[sliceIndex(key)]->put(key, value);
  }

  bool get(const Key &key, Value &value) override {
    // 获取key的hash值，并计算出对应的分片索引
    return slices_[sliceIndex(key)]->get(key, value);
  }

  Value get(const Key &key) override {
    Value value{};
    get(key, value);
    return value;
  }

private:
  // 将key转换为对应hash值
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

private:
  std::size_t capacity_;                                      // 总容量
  std::size_t sliceNum_;                                      // 切片数量
  std::vector<std::unique_ptr<LruCache<Key, Value>>> slices_; // 切片LRU缓存
};

} // namespace cppcache::cache

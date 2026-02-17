#include "ICachePolicy.h"
#include <cassert>
#include <cmath>
#include <cstddef>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

template <typename Key, typename Value>
class LruCache : public ICachePolicy<Key, Value> {
public:
  LruCache(int capacity)
      : capacity_(capacity > 0 ? static_cast<std::size_t>(capacity) : 0) {
    initializeList();
  }

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
  void remove(const Key &key) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = nodeMap_.find(key);
    if (it != nodeMap_.end()) {
      removeNode(it->second);
      nodeMap_.erase(it);
    }
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
      : LruCache<Key, Value>(capacity), k_(k),
        historyList_(std::make_unique<LruCache<Key, size_t>>(historyCapacity)) {
  }

  Value get(const Key &key) {
    std::lock_guard<std::mutex> lock(k_mutex_);
    // 首先尝试从主缓存获取数据
    Value value{};
    bool inMainCache = LruCache<Key, Value>::get(key, value);

    // 获取并更新访问历史计数
    size_t historyCount = historyList_->get(key);
    historyCount++;
    historyList_->put(key, historyCount);

    // 如果数据在主缓存中，直接返回
    if (inMainCache) {
      return value;
    }

    // 如果数据不在主缓存，但访问次数达到了k次
    if (historyCount >= k_) {
      // 检查是否有历史值记录
      auto it = historyValueMap_.find(key);
      if (it != historyValueMap_.end()) {
        // 有历史值，将其添加到主缓存
        Value storedValue = it->second;

        // 从历史记录移除
        historyList_->remove(key);
        historyValueMap_.erase(it);

        // 添加到主缓存
        LruCache<Key, Value>::put(key, storedValue);

        return storedValue;
      }
      // 没有历史值记录，无法添加到缓存，返回默认值
    }

    // 数据不在主缓存且不满足添加条件，返回默认值
    return value;
  }

  void put(const Key &key, const Value &value) {
    std::lock_guard<std::mutex> lock(k_mutex_);
    // 检查是否已在主缓存
    Value existingValue{};
    bool inMainCache = LruCache<Key, Value>::get(key, existingValue);

    if (inMainCache) {
      // 已在主缓存，直接更新
      LruCache<Key, Value>::put(key, value);
      return;
    }

    // 获取并更新访问历史
    std::size_t historyCount = historyList_->get(key);
    historyCount++;
    historyList_->put(key, historyCount);

    // 保存值到历史记录映射，供后续get操作使用
    historyValueMap_[key] = value;

    // 检查是否达到k次访问阈值
    if (historyCount >= k_) {
      // 达到阈值，添加到主缓存
      historyList_->remove(key);
      historyValueMap_.erase(key);
      LruCache<Key, Value>::put(key, value);
    }
  }

private:
  int k_; // 进入缓存队列的评判标准
  mutable std::mutex k_mutex_;
  std::unique_ptr<LruCache<Key, size_t>>
      historyList_; // 访问数据历史记录(value为访问次数)
  std::unordered_map<Key, Value> historyValueMap_; // 存储未达到k次访问的数据值
};

// lru优化：对lru进行分片，提高高并发使用的性能
template <typename Key, typename Value> class KHashLruCaches {
public:
  KHashLruCaches(size_t capacity, int sliceNum)
      : capacity_(capacity),
        sliceNum_(sliceNum > 0 ? sliceNum
                               : std::thread::hardware_concurrency()) {
    size_t sliceSize = std::ceil(
        capacity / static_cast<double>(sliceNum_)); // 获取每个分片的大小
    for (int i = 0; i < sliceNum_; ++i) {
      lruSliceCaches_.emplace_back(new LruCache<Key, Value>(sliceSize));
    }
  }

  void put(const Key &key, const Value &value) {
    // 获取key的hash值，并计算出对应的分片索引
    size_t sliceIndex = Hash(key) % sliceNum_;
    lruSliceCaches_[sliceIndex]->put(key, value);
  }

  bool get(Key key, Value &value) {
    // 获取key的hash值，并计算出对应的分片索引
    size_t sliceIndex = Hash(key) % sliceNum_;
    return lruSliceCaches_[sliceIndex]->get(key, value);
  }

  Value get(Key key) {
    Value value{};
    get(key, value);
    return value;
  }

private:
  // 将key转换为对应hash值
  size_t Hash(Key key) {
    std::hash<Key> hashFunc;
    return hashFunc(key);
  }

private:
  std::size_t capacity_; // 总容量
  int sliceNum_;         // 切片数量
  std::vector<std::unique_ptr<LruCache<Key, Value>>>
      lruSliceCaches_; // 切片LRU缓存
};
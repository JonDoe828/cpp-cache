#pragma once

template <typename Key, typename Value> class ICachePolicy {
public:
  virtual ~ICachePolicy() = default;

  // 添加缓存接口
  virtual void put(const Key &key, const Value &value) = 0;

  // key是传入参数  访问到的值以传出参数的形式返回 | 访问成功返回true
  virtual bool get(const Key &key, Value &value) = 0;
  // 如果缓存中能找到key，则直接返回value
  virtual Value get(const Key &key) = 0;
};

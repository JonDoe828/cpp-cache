#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <unordered_map>

namespace cppcache::storage {

class IKvStore {
public:
  using Entries = std::unordered_map<std::string, std::string>;

  virtual ~IKvStore() = default;

  virtual std::optional<std::string> get(const std::string &key) = 0;
  virtual void put(const std::string &key, const std::string &value) = 0;
  virtual bool erase(const std::string &key) = 0;
  virtual std::size_t size() const = 0;

  virtual Entries snapshot() const = 0;
  virtual void restore(const Entries &entries) = 0;
};

} // namespace cppcache::storage

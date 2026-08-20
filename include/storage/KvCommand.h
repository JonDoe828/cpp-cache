#pragma once

#include <cstdint>
#include <string>

namespace cppcache::storage {

enum class KvOperation { Put, Append, Erase, NoOp };

struct KvCommand {
  KvOperation operation{KvOperation::Put};
  std::string key;
  std::string value;
  std::string clientId;
  std::uint64_t requestId{0};
};

} // namespace cppcache::storage

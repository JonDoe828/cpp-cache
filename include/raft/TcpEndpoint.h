#pragma once

#include <cstdint>
#include <string>

namespace cppcache::raft {

struct Endpoint {
  std::string host;
  std::uint16_t port{0};
};

} // namespace cppcache::raft

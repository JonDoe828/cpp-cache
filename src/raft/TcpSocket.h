#pragma once

#include "raft/TcpEndpoint.h"

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>

namespace cppcache::raft::tcp {

bool sendFrame(int socket, std::uint8_t type, const std::string &payload);
std::optional<std::pair<std::uint8_t, std::string>> receiveFrame(int socket);

void configureTimeout(int socket, std::chrono::milliseconds timeout);
int createListener(const Endpoint &endpoint);

std::optional<std::string> callEndpoint(const Endpoint &endpoint,
                                        std::chrono::milliseconds timeout,
                                        std::uint8_t type,
                                        const std::string &payload,
                                        std::uint8_t expectedResponse);

} // namespace cppcache::raft::tcp

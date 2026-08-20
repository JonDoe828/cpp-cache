#include "raft/TcpTransport.h"

#include <unistd.h>

#include <array>
#include <cstdint>
#include <iostream>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

cppcache::raft::Endpoint parseEndpoint(const std::string &text) {
  const auto separator = text.rfind(':');
  if (separator == std::string::npos || separator == 0 ||
      separator + 1 == text.size())
    throw std::invalid_argument("endpoint must have host:port form");
  const auto port = std::stoul(text.substr(separator + 1));
  if (port == 0 || port > 65535)
    throw std::invalid_argument("endpoint port is out of range");
  return {text.substr(0, separator), static_cast<std::uint16_t>(port)};
}

std::vector<cppcache::raft::Endpoint> parseServers(const std::string &text) {
  std::vector<cppcache::raft::Endpoint> servers;
  std::size_t begin = 0;
  while (begin < text.size()) {
    const auto end = text.find(',', begin);
    servers.push_back(parseEndpoint(text.substr(begin, end - begin)));
    if (end == std::string::npos)
      break;
    begin = end + 1;
  }
  return servers;
}

std::string clientId() {
  std::array<char, 256> hostname{};
  if (::gethostname(hostname.data(), hostname.size()) != 0)
    throw std::runtime_error("failed to determine client hostname");
  hostname.back() = '\0';
  std::random_device random;
  const std::uint64_t nonce =
      (static_cast<std::uint64_t>(random()) << 32) ^ random();
  return "cpp-cache-cli-" + std::to_string(::getuid()) + "@" +
         hostname.data() + ":" + std::to_string(::getpid()) + ":" +
         std::to_string(nonce);
}

void printUsage() {
  std::cerr
      << "usage: cpp_cache_cli host:port[,host:port...] get KEY\n"
         "       cpp_cache_cli host:port[,host:port...] put KEY VALUE\n"
         "       cpp_cache_cli host:port[,host:port...] append KEY VALUE\n"
         "       cpp_cache_cli host:port[,host:port...] erase KEY\n";
}

} // namespace

int main(int argc, char **argv) {
  if (argc < 4) {
    printUsage();
    return 2;
  }
  try {
    cppcache::raft::TcpKvClient client(parseServers(argv[1]));
    const std::string operation = argv[2];
    const std::string key = argv[3];
    if (operation == "get") {
      const auto result = client.get(key);
      if (result.status != cppcache::raft::ClientStatus::Committed)
        return 1;
      if (result.value)
        std::cout << *result.value << '\n';
      else
        std::cout << "(nil)\n";
      return 0;
    }

    constexpr std::uint64_t id = 1;
    const std::string stableClientId = clientId();
    cppcache::raft::SubmitResult result;
    if (operation == "put" && argc == 5) {
      result = client.put(key, argv[4], stableClientId, id);
    } else if (operation == "append" && argc == 5) {
      result = client.append(key, argv[4], stableClientId, id);
    } else if (operation == "erase" && argc == 4) {
      result = client.erase(key, stableClientId, id);
    } else {
      printUsage();
      return 2;
    }
    if (result.status != cppcache::raft::ClientStatus::Committed)
      return 1;
    if (!result.result || !result.result->applied) {
      std::cerr << "write was not applied\n";
      return 1;
    }
    std::cout << "OK\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "client error: " << error.what() << '\n';
    return 1;
  }
}

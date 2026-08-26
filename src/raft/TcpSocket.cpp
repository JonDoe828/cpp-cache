#include "TcpSocket.h"

#include "TcpProtocol.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstddef>
#include <utility>

namespace cppcache::raft::tcp {
namespace {

class Socket {
public:
  explicit Socket(int descriptor = -1) : descriptor_(descriptor) {}
  ~Socket() {
    if (descriptor_ >= 0)
      ::close(descriptor_);
  }

  Socket(const Socket &) = delete;
  Socket &operator=(const Socket &) = delete;

  Socket(Socket &&other) noexcept
      : descriptor_(std::exchange(other.descriptor_, -1)) {}

  Socket &operator=(Socket &&other) noexcept {
    if (this != &other) {
      if (descriptor_ >= 0)
        ::close(descriptor_);
      descriptor_ = std::exchange(other.descriptor_, -1);
    }
    return *this;
  }

  int get() const { return descriptor_; }
  int release() { return std::exchange(descriptor_, -1); }

private:
  int descriptor_;
};

bool sendAll(int socket, const void *data, std::size_t size) {
  const auto *bytes = static_cast<const char *>(data);
  while (size > 0) {
    const ssize_t sent = ::send(socket, bytes, size, MSG_NOSIGNAL);
    if (sent <= 0) {
      if (sent < 0 && errno == EINTR)
        continue;
      return false;
    }
    bytes += sent;
    size -= static_cast<std::size_t>(sent);
  }
  return true;
}

bool receiveAll(int socket, void *data, std::size_t size) {
  auto *bytes = static_cast<char *>(data);
  while (size > 0) {
    const ssize_t received = ::recv(socket, bytes, size, 0);
    if (received <= 0) {
      if (received < 0 && errno == EINTR)
        continue;
      return false;
    }
    bytes += received;
    size -= static_cast<std::size_t>(received);
  }
  return true;
}

Socket connectTo(const Endpoint &endpoint, std::chrono::milliseconds timeout) {
  addrinfo hints{};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  addrinfo *addresses = nullptr;
  const std::string port = std::to_string(endpoint.port);
  if (::getaddrinfo(endpoint.host.c_str(), port.c_str(), &hints, &addresses) !=
      0)
    return Socket{};

  Socket connected;
  for (addrinfo *address = addresses; address; address = address->ai_next) {
    Socket candidate(::socket(address->ai_family, address->ai_socktype,
                              address->ai_protocol));
    if (candidate.get() < 0)
      continue;
    const int flags = ::fcntl(candidate.get(), F_GETFL, 0);
    (void)::fcntl(candidate.get(), F_SETFL, flags | O_NONBLOCK);
    int result = ::connect(candidate.get(), address->ai_addr,
                           static_cast<socklen_t>(address->ai_addrlen));
    if (result < 0 && errno == EINPROGRESS) {
      pollfd descriptor{candidate.get(), POLLOUT, 0};
      result = ::poll(&descriptor, 1, static_cast<int>(timeout.count()));
      if (result > 0) {
        int error = 0;
        socklen_t errorSize = sizeof(error);
        (void)::getsockopt(candidate.get(), SOL_SOCKET, SO_ERROR, &error,
                           &errorSize);
        result = error == 0 ? 0 : -1;
      } else {
        result = -1;
      }
    }
    (void)::fcntl(candidate.get(), F_SETFL, flags);
    if (result == 0) {
      configureTimeout(candidate.get(), timeout);
      connected = std::move(candidate);
      break;
    }
  }
  ::freeaddrinfo(addresses);
  return connected;
}

} // namespace

bool sendFrame(int socket, std::uint8_t type, const std::string &payload) {
  if (payload.size() + 1 > kMaxFrameSize)
    return false;
  const std::uint32_t size =
      htonl(static_cast<std::uint32_t>(payload.size() + 1));
  return sendAll(socket, &size, sizeof(size)) &&
         sendAll(socket, &type, sizeof(type)) &&
         sendAll(socket, payload.data(), payload.size());
}

std::optional<std::pair<std::uint8_t, std::string>> receiveFrame(int socket) {
  std::uint32_t networkSize{};
  if (!receiveAll(socket, &networkSize, sizeof(networkSize)))
    return std::nullopt;
  const std::uint32_t size = ntohl(networkSize);
  if (size == 0 || size > kMaxFrameSize)
    return std::nullopt;
  std::uint8_t type{};
  if (!receiveAll(socket, &type, sizeof(type)))
    return std::nullopt;
  std::string payload(size - 1, '\0');
  if (!payload.empty() && !receiveAll(socket, payload.data(), payload.size()))
    return std::nullopt;
  return std::make_pair(type, std::move(payload));
}

void configureTimeout(int socket, std::chrono::milliseconds timeout) {
  timeval value{};
  value.tv_sec = static_cast<long>(timeout.count() / 1000);
  value.tv_usec = static_cast<long>((timeout.count() % 1000) * 1000);
  (void)::setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO, &value, sizeof(value));
  (void)::setsockopt(socket, SOL_SOCKET, SO_SNDTIMEO, &value, sizeof(value));
}

int createListener(const Endpoint &endpoint) {
  addrinfo hints{};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_flags = AI_PASSIVE;
  addrinfo *addresses = nullptr;
  const std::string port = std::to_string(endpoint.port);
  const char *host = endpoint.host.empty() ? nullptr : endpoint.host.c_str();
  if (::getaddrinfo(host, port.c_str(), &hints, &addresses) != 0)
    return -1;

  int listener = -1;
  for (addrinfo *address = addresses; address; address = address->ai_next) {
    Socket candidate(::socket(address->ai_family, address->ai_socktype,
                              address->ai_protocol));
    if (candidate.get() < 0)
      continue;
    int reuse = 1;
    (void)::setsockopt(candidate.get(), SOL_SOCKET, SO_REUSEADDR, &reuse,
                       sizeof(reuse));
    if (::bind(candidate.get(), address->ai_addr,
               static_cast<socklen_t>(address->ai_addrlen)) == 0 &&
        ::listen(candidate.get(), 128) == 0) {
      listener = candidate.release();
      break;
    }
  }
  ::freeaddrinfo(addresses);
  return listener;
}

std::optional<std::string> callEndpoint(const Endpoint &endpoint,
                                        std::chrono::milliseconds timeout,
                                        std::uint8_t type,
                                        const std::string &payload,
                                        std::uint8_t expectedResponse) {
  Socket socket = connectTo(endpoint, timeout);
  if (socket.get() < 0 || !sendFrame(socket.get(), type, payload))
    return std::nullopt;
  auto response = receiveFrame(socket.get());
  if (!response || response->first != expectedResponse)
    return std::nullopt;
  return std::move(response->second);
}

} // namespace cppcache::raft::tcp

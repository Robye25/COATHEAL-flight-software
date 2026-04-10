#include "coatheal/sd_notify.hpp"

#if defined(__linux__)
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <cstdlib>
#include <cstring>
#endif

namespace coatheal {

#if defined(__linux__)

bool SdNotify(const std::string& message) {
  const char* sock_path = std::getenv("NOTIFY_SOCKET");
  if (sock_path == nullptr || sock_path[0] == '\0') {
    return false;
  }

  const std::size_t path_len = std::strlen(sock_path);
  if (path_len >= sizeof(sockaddr_un{}.sun_path)) {
    return false;
  }

  const int fd = ::socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
  if (fd < 0) {
    return false;
  }

  sockaddr_un addr{};
  addr.sun_family = AF_UNIX;
  std::memcpy(addr.sun_path, sock_path, path_len + 1);

  // systemd uses '@' as a stand-in for the abstract-namespace null byte.
  socklen_t addrlen = static_cast<socklen_t>(sizeof(sockaddr_un));
  if (addr.sun_path[0] == '@') {
    addr.sun_path[0] = '\0';
    addrlen = static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) + path_len);
  }

#ifdef MSG_NOSIGNAL
  const int flags = MSG_NOSIGNAL;
#else
  const int flags = 0;
#endif

  const ssize_t sent = ::sendto(fd, message.data(), message.size(), flags,
                                reinterpret_cast<sockaddr*>(&addr), addrlen);
  ::close(fd);
  return sent == static_cast<ssize_t>(message.size());
}

#else

bool SdNotify(const std::string& /*message*/) {
  // No-op outside Linux. Windows/macOS dev hosts don't have a notify socket.
  return false;
}

#endif

}  // namespace coatheal

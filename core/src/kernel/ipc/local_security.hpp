#pragma once
#include <asterion/foundation/error.hpp>
#include <sys/socket.h>
#include <unistd.h>
namespace asterion::ipc::detail {
inline void verify_local_peer(int fd) {
#ifdef __APPLE__
  uid_t uid = 0;
  gid_t gid = 0;
  if (::getpeereid(fd, &uid, &gid) != 0 || uid != ::geteuid())
    throw Error(ErrorCode::unavailable, "IPC peer identity rejected");
#else
  struct ucred credential{};
  socklen_t size = sizeof(credential);
  if (::getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &credential, &size) != 0 ||
      credential.uid != ::geteuid())
    throw Error(ErrorCode::unavailable, "IPC peer identity rejected");
#endif
}
} // namespace asterion::ipc::detail

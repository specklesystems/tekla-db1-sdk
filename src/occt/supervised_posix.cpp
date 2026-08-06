#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "occt.hpp"
#include "protocol.hpp"

namespace tekla::db1::detail {

struct SupervisedOcctHost::Impl {
  explicit Impl(std::filesystem::path worker_value, std::uint32_t timeout_value)
      : worker(std::move(worker_value)), timeout_milliseconds(timeout_value) {}

  ~Impl() { stop(false); }

  [[nodiscard]] Result<bool> start() {
    if (pid > 0) return Result<bool>::success(true);
    std::array<int, 2> sockets{-1, -1};
    if (::socketpair(AF_UNIX, SOCK_STREAM, 0, sockets.data()) != 0) {
      return Result<bool>::failure(
          {ErrorCode::io_error, "Creating the OCCT worker channel failed."});
    }
#if defined(SO_NOSIGPIPE)
    int enabled = 1;
    (void)::setsockopt(sockets[0], SOL_SOCKET, SO_NOSIGPIPE, &enabled, sizeof(enabled));
#endif
    int send_buffer_bytes = 64 * 1024;
    (void)::setsockopt(sockets[0], SOL_SOCKET, SO_SNDBUF, &send_buffer_bytes,
                       sizeof(send_buffer_bytes));
    const int socket_flags = ::fcntl(sockets[0], F_GETFL, 0);
    if (socket_flags < 0 || ::fcntl(sockets[0], F_SETFL, socket_flags | O_NONBLOCK) < 0) {
      ::close(sockets[0]);
      ::close(sockets[1]);
      return Result<bool>::failure(
          {ErrorCode::io_error, "Configuring the OCCT worker channel failed."});
    }
    const pid_t child = ::fork();
    if (child < 0) {
      ::close(sockets[0]);
      ::close(sockets[1]);
      return Result<bool>::failure(
          {ErrorCode::io_error, "Starting the OCCT worker process failed."});
    }
    if (child == 0) {
      ::close(sockets[0]);
      if (::dup2(sockets[1], STDIN_FILENO) < 0 || ::dup2(sockets[1], STDOUT_FILENO) < 0) _exit(126);
      ::close(sockets[1]);
      const std::string executable = worker.string();
      ::execl(executable.c_str(), executable.c_str(), static_cast<char*>(nullptr));
      _exit(127);
    }
    ::close(sockets[1]);
    pid = child;
    socket = sockets[0];
    return Result<bool>::success(true);
  }

  void stop(bool force) noexcept {
    if (socket >= 0) {
      ::close(socket);
      socket = -1;
    }
    if (pid > 0) {
      if (force) (void)::kill(pid, SIGKILL);
      int status = 0;
      while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {
      }
      pid = -1;
    }
  }

  [[nodiscard]] Result<bool> wait_until_ready(
      short events, std::chrono::steady_clock::time_point deadline) const {
    for (;;) {
      const auto now = std::chrono::steady_clock::now();
      if (now >= deadline)
        return Result<bool>::failure(
            {ErrorCode::geometry_timeout, "The OCCT worker exceeded its wall-clock deadline."});
      const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
      pollfd descriptor{socket, events, 0};
      const int ready =
          ::poll(&descriptor, 1,
                 static_cast<int>(std::min<std::int64_t>(remaining.count() + 1, 2147483647)));
      if (ready < 0 && errno == EINTR) continue;
      if (ready == 0)
        return Result<bool>::failure(
            {ErrorCode::geometry_timeout, "The OCCT worker exceeded its wall-clock deadline."});
      if (ready < 0 || (descriptor.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0)
        return Result<bool>::failure(
            {ErrorCode::geometry_backend_crash, "The OCCT worker process terminated."});
      if ((descriptor.revents & events) != 0) return Result<bool>::success(true);
    }
  }

  [[nodiscard]] Result<bool> send_all(std::span<const std::byte> bytes,
                                      std::chrono::steady_clock::time_point deadline) {
    std::size_t offset = 0;
    while (offset < bytes.size()) {
      auto ready = wait_until_ready(POLLOUT, deadline);
      if (!ready) return ready;
#if defined(MSG_NOSIGNAL)
      constexpr int flags = MSG_NOSIGNAL | MSG_DONTWAIT;
#else
      constexpr int flags = MSG_DONTWAIT;
#endif
      const auto count = ::send(socket, bytes.data() + offset, bytes.size() - offset, flags);
      if (count < 0 && errno == EINTR) continue;
      if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) continue;
      if (count <= 0)
        return Result<bool>::failure(
            {ErrorCode::geometry_backend_crash, "The OCCT worker stopped accepting work."});
      offset += static_cast<std::size_t>(count);
    }
    return Result<bool>::success(true);
  }

  [[nodiscard]] Result<bool> receive_all(std::span<std::byte> bytes,
                                         std::chrono::steady_clock::time_point deadline) {
    std::size_t offset = 0;
    while (offset < bytes.size()) {
      auto ready = wait_until_ready(POLLIN, deadline);
      if (!ready) return ready;
      const auto count = ::recv(socket, bytes.data() + offset, bytes.size() - offset, MSG_DONTWAIT);
      if (count < 0 && errno == EINTR) continue;
      if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) continue;
      if (count <= 0)
        return Result<bool>::failure(
            {ErrorCode::geometry_backend_crash, "The OCCT worker process terminated."});
      offset += static_cast<std::size_t>(count);
    }
    return Result<bool>::success(true);
  }

  [[nodiscard]] Result<OcctMesh> evaluate(const OcctRequest& request) {
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_milliseconds);
    auto started = start();
    if (!started) return Result<OcctMesh>::failure(started.error());
    auto encoded = encode_occt_request(request);
    if (!encoded) return Result<OcctMesh>::failure(encoded.error());
    auto sent = send_all(encoded.value(), deadline);
    if (!sent) {
      stop(true);
      return Result<OcctMesh>::failure(sent.error());
    }
    std::array<std::byte, kOcctResponseHeaderSize> header{};
    auto received = receive_all(header, deadline);
    if (!received) {
      stop(true);
      return Result<OcctMesh>::failure(received.error());
    }
    auto response_size = occt_response_size(header);
    if (!response_size) {
      stop(true);
      return Result<OcctMesh>::failure(response_size.error());
    }
    std::vector<std::byte> response(response_size.value());
    std::copy(header.begin(), header.end(), response.begin());
    received = receive_all(std::span(response).subspan(header.size()), deadline);
    if (!received) {
      stop(true);
      return Result<OcctMesh>::failure(received.error());
    }
    return decode_occt_response(response);
  }

  std::filesystem::path worker;
  std::uint32_t timeout_milliseconds = 0;
  pid_t pid = -1;
  int socket = -1;
};

SupervisedOcctHost::SupervisedOcctHost(std::filesystem::path worker,
                                       std::uint32_t timeout_milliseconds)
    : impl_(std::make_unique<Impl>(std::move(worker), timeout_milliseconds)) {}
SupervisedOcctHost::~SupervisedOcctHost() = default;
SupervisedOcctHost::SupervisedOcctHost(SupervisedOcctHost&&) noexcept = default;
SupervisedOcctHost& SupervisedOcctHost::operator=(SupervisedOcctHost&&) noexcept = default;

Result<OcctMesh> SupervisedOcctHost::evaluate(const OcctRequest& request) {
  return impl_->evaluate(request);
}

}  // namespace tekla::db1::detail

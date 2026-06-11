#include <arpa/inet.h>
#include <bupp/base/completion_queue_entry.h>
#include <bupp/base/ring.h>
#include <bupp/base/submission_queue_entry.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include <array>
#include <csignal>
#include <cstdint>
#include <iostream>
#include <unordered_map>
#include <utility>

#include "example_support.h"

namespace {

constexpr std::uint16_t k_port = 7000;
constexpr int k_backlog = 128;
constexpr std::uint64_t k_operation_shift = 56;

volatile std::sig_atomic_t stop_requested = 0;

enum class operation : std::uint8_t {
  accept = 1,
  recv = 2,
  send = 3,
};

struct connection {
  bupp::examples::base::unique_fd fd;
  std::array<char, 4096> buffer{};
  std::size_t bytes = 0;
  std::size_t sent = 0;
};

using connection_map = std::unordered_map<int, connection>;

void handle_signal(int) { stop_requested = 1; }

std::uint64_t pack_user_data(operation op, int fd) noexcept {
  return (static_cast<std::uint64_t>(op) << k_operation_shift) |
         static_cast<std::uint32_t>(fd);
}

operation unpack_operation(std::uint64_t user_data) noexcept {
  return static_cast<operation>(user_data >> k_operation_shift);
}

int unpack_fd(std::uint64_t user_data) noexcept {
  return static_cast<int>(static_cast<std::uint32_t>(user_data));
}

bupp::examples::base::unique_fd make_listener() {
  bupp::examples::base::unique_fd listener(
      ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0));
  if (!listener.is_open()) {
    std::cerr << "echo_server: socket failed\n";
    return {};
  }

  int reuse = 1;
  if (::setsockopt(listener.get(), SOL_SOCKET, SO_REUSEADDR, &reuse,
                   static_cast<socklen_t>(sizeof(reuse))) != 0) {
    std::cerr << "echo_server: setsockopt failed\n";
    return {};
  }

  sockaddr_in address{};
  address.sin_family = static_cast<sa_family_t>(AF_INET);
  address.sin_port = htons(k_port);
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

  if (::bind(listener.get(), reinterpret_cast<const sockaddr*>(&address),
             static_cast<socklen_t>(sizeof(address))) != 0) {
    std::cerr << "echo_server: bind 127.0.0.1:" << k_port << " failed\n";
    return {};
  }

  if (::listen(listener.get(), k_backlog) != 0) {
    std::cerr << "echo_server: listen failed\n";
    return {};
  }

  return listener;
}

bool queue_accept(bupp::base::ring& ring, int listener_fd) {
  bupp::base::submission_queue_entry sqe =
      bupp::examples::base::get_sqe_or_log(ring, "echo_server accept");
  if (sqe.raw() == nullptr) {
    return false;
  }

  sqe.prep_accept(listener_fd, nullptr, nullptr, SOCK_CLOEXEC | SOCK_NONBLOCK);
  sqe.set_data64(pack_user_data(operation::accept, 0));
  return true;
}

bool queue_recv(bupp::base::ring& ring, connection& client) {
  bupp::base::submission_queue_entry sqe =
      bupp::examples::base::get_sqe_or_log(ring, "echo_server recv");
  if (sqe.raw() == nullptr) {
    return false;
  }

  sqe.prep_recv(client.fd.get(), client.buffer.data(), client.buffer.size(), 0);
  sqe.set_data64(pack_user_data(operation::recv, client.fd.get()));
  return true;
}

bool queue_send(bupp::base::ring& ring, connection& client) {
  bupp::base::submission_queue_entry sqe =
      bupp::examples::base::get_sqe_or_log(ring, "echo_server send");
  if (sqe.raw() == nullptr) {
    return false;
  }

  const char* data = client.buffer.data() + client.sent;
  const std::size_t remaining = client.bytes - client.sent;
  sqe.prep_send(client.fd.get(), data, remaining, MSG_NOSIGNAL);
  sqe.set_data64(pack_user_data(operation::send, client.fd.get()));
  return true;
}

void close_connection(connection_map& connections, int fd) {
  const auto erased = connections.erase(fd);
  if (erased != 0U) {
    std::cout << "echo_server: closed fd=" << fd << '\n';
  }
}

bool submit_pending(bupp::base::ring& ring) {
  const int result = ring.submit();
  if (result < 0) {
    std::cerr << "echo_server: submit failed: " << result << '\n';
    return false;
  }
  return true;
}

}  // namespace

int main() {
  std::signal(SIGINT, handle_signal);
  std::signal(SIGTERM, handle_signal);

  bupp::base::ring ring;
  switch (bupp::examples::base::init_ring(ring, 256, "echo_server")) {
    case bupp::examples::base::ring_init_result::ready:
      break;
    case bupp::examples::base::ring_init_result::unavailable:
      return 0;
    case bupp::examples::base::ring_init_result::failed:
      return 1;
  }

  bupp::examples::base::unique_fd listener = make_listener();
  if (!listener.is_open()) {
    return 1;
  }

  connection_map connections;
  if (!queue_accept(ring, listener.get()) || !submit_pending(ring)) {
    return 1;
  }

  std::cout << "echo_server: listening on 127.0.0.1:" << k_port << '\n';

  while (stop_requested == 0) {
    bupp::base::completion_queue_entry cqe;
    const int wait_result = ring.wait_cqe(cqe);
    if (wait_result < 0) {
      if (wait_result == -EINTR && stop_requested != 0) {
        break;
      }
      std::cerr << "echo_server: wait failed: " << wait_result << '\n';
      return 1;
    }

    const int result = cqe.res();
    const std::uint64_t user_data = cqe.get_data64();
    ring.cqe_seen(cqe);

    switch (unpack_operation(user_data)) {
      case operation::accept: {
        if (!queue_accept(ring, listener.get())) {
          return 1;
        }

        if (result < 0) {
          std::cerr << "echo_server: accept result=" << result << '\n';
          break;
        }

        const int client_fd = result;
        auto [client, inserted] = connections.try_emplace(client_fd);
        if (!inserted) {
          static_cast<void>(::close(client_fd));
          break;
        }

        client->second.fd.reset(client_fd);
        if (!queue_recv(ring, client->second)) {
          close_connection(connections, client_fd);
        } else {
          std::cout << "echo_server: accepted fd=" << client_fd << '\n';
        }
        break;
      }

      case operation::recv: {
        const int fd = unpack_fd(user_data);
        auto client = connections.find(fd);
        if (client == connections.end()) {
          break;
        }

        if (result <= 0) {
          close_connection(connections, fd);
          break;
        }

        client->second.bytes = static_cast<std::size_t>(result);
        client->second.sent = 0;
        if (!queue_send(ring, client->second)) {
          close_connection(connections, fd);
        }
        break;
      }

      case operation::send: {
        const int fd = unpack_fd(user_data);
        auto client = connections.find(fd);
        if (client == connections.end()) {
          break;
        }

        if (result <= 0) {
          close_connection(connections, fd);
          break;
        }

        client->second.sent += static_cast<std::size_t>(result);
        if (client->second.sent < client->second.bytes) {
          if (!queue_send(ring, client->second)) {
            close_connection(connections, fd);
          }
        } else if (!queue_recv(ring, client->second)) {
          close_connection(connections, fd);
        }
        break;
      }
    }

    if (!submit_pending(ring)) {
      return 1;
    }
  }

  std::cout << "echo_server: stopping\n";
  return 0;
}

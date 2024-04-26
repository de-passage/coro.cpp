#include "fd_coroutine.hpp"
#include "fd_timers.hpp"

#include <arpa/inet.h>
#include <bits/chrono.h>
#include <iostream>
#include <netinet/ip.h>
#include <sys/poll.h>
#include <sys/select.h>
#include <sys/signalfd.h>
#include <unistd.h>
#include <utility>

#define DPSG_COMPILE_DEBUG_HPP

using namespace dpsg;
using namespace dpsg::vt100;

#include "vt100.hpp"

using namespace dpsg::vt100;

fd_reader read_from_stdin(auto color, int count = 10) {
  while (count--) {
    co_await descriptors::stdin;
    std::string whatever;
    getline(std::cin, whatever);
    std::cout << color << whatever << reset << std::endl;
  }
}

fd_reader read_once(auto color) { return read_from_stdin(color, 1); }

fd_reader wait_on_tcp_socket(auto color, int port, int count = 1) {
  int sock = posix_enforce(socket(AF_INET, SOCK_STREAM, 0));
  sockaddr_in addr;
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  posix_enforce(inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr));
  auto ret = connect(sock, (sockaddr *)&addr, sizeof(addr));
  if (ret < 0) {
    std::cout << color << "<FAILED TO CONNECT>" << reset << std::endl;
    co_return;
  }

  while (count--) {
    co_await fd_t{sock};
    std::array<char, 512> buffer;

    int readed = posix_enforce(read(sock, buffer.data(), buffer.size()));
    if (readed == 0) {
      std::cout << color << "<Stream closed>" << std::endl;
      posix_enforce(close(sock));
      co_return;
    } else {
      std::cout << color << std::string_view(buffer.data(), readed) << reset
                << std::endl;
    }
  }

  posix_enforce(close(sock));
}

fd_reader handle_connection_on(auto color, fd_t connection_port) {
  while (true) {
    co_await connection_port;
    std::array<char, 512> buffer;

    int readed =
        posix_enforce(read(connection_port.fd, buffer.data(), buffer.size()));
    if (readed == 0) {
      std::cout << color << "<Stream closed>" << std::endl;
      co_return;
    } else {
      std::cout << color << std::string_view(buffer.data(), readed) << reset
                << std::endl;
    }
  }
}

fd_reader::scheduler_t set_scheduler(fd_scheduler_round_robin &awaiter) {
  return {[&awaiter](fd_reader reader) { awaiter.add(std::move(reader)); }};
}

fd_reader spawn_tcp_server(fd_scheduler_round_robin &awaiter, auto color,
                           int listening_port, in_addr_t inaddr = INADDR_ANY) {
  co_await set_scheduler(awaiter);

  int sock = posix_enforce(socket(AF_INET, SOCK_STREAM, 0));

  sockaddr_in addr;
  addr.sin_family = AF_INET;
  addr.sin_port = htons(listening_port);
  addr.sin_addr.s_addr = inaddr;

  posix_enforce(bind(sock, (sockaddr *)&addr, sizeof(addr)));
  posix_enforce(listen(sock, 1));
  while (true) {
    co_await fd_t{sock};
    socklen_t size;
    fd_t connection{posix_enforce(accept(sock, (sockaddr *)&addr, &size))};
    std::cout << color << "Connection accepted on " << addr.sin_port << " from "
              << inet_ntoa(addr.sin_addr) << reset << std::endl;
    co_yield handle_connection_on(color, connection);
  }
}

timer hello_timing_world() {
  using namespace std::chrono_literals;
  std::cout << (white|bold) << "Hello " << reset << std::flush;
  co_await 1s;
  std::cout << (white|bold) << "World!" << reset << std::endl;
}

timer spit_every(auto duration, auto color) {
  while (true) {
    co_await duration;
    std::cout << color << "!" << reset << std::flush;
  }
}

int main() {
  using namespace std::literals;
  fd_scheduler_round_robin awaiter;
  timer_scheduler timers;
  awaiter.add(timers.manage_timers().name("Timer Manager"));
  timers.add(hello_timing_world(), spit_every(.9s, yellow));
  awaiter.add(read_from_stdin(red, 10).name("red 10"),
              read_from_stdin(green, 5).name("green 5"),
              read_once(blue).name("blue"),
              wait_on_tcp_socket(cyan, 42069, 3).name("client"),
              spawn_tcp_server(awaiter, magenta, 41069).name("server"));
  awaiter.run();
}

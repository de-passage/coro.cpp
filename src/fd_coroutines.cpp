#include <algorithm>
#include <arpa/inet.h>
#include <bits/chrono.h>
#include <bits/types/sigevent_t.h>
#include <cassert>
#include <coroutine>
#include <csignal>
#include <ctime>
#include <deque>
#include <exception>
#include <functional>
#include <iostream>
#include <netinet/ip.h>
#include <queue>
#include <ranges>
#include <sys/poll.h>
#include <sys/select.h>
#include <sys/signalfd.h>
#include <unistd.h>
#include <unordered_map>
#include <utility>
#include <vector>

#include "debug.hpp"

using namespace dpsg;
using namespace dpsg::vt100;
std::mutex debug_::mutex_{};

#define posix_enforce_impl(expr, var, func, line)                              \
  [&] {                                                                        \
    int var;                                                                   \
    do {                                                                       \
      var = (expr);                                                            \
      if (var < 0) {                                                           \
        if (errno == EINTR || errno == EAGAIN) {                               \
          continue;                                                            \
        }                                                                      \
        perror(#expr);                                                         \
        std::exit(errno);                                                      \
      }                                                                        \
    } while (false);                                                           \
    return var;                                                                \
  }()

#define posix_enforce_pp_sucks(...) posix_enforce_impl(__VA_ARGS__)
#define posix_enforce(...)                                                     \
  posix_enforce_pp_sucks((__VA_ARGS__), ret_##__COUNTER__, __FUNCTION__,       \
                         __LINE__)

#include "vt100.hpp"

using namespace dpsg::vt100;

struct fd_t {
  int fd;
  explicit operator int() const noexcept { return fd; }
};

std::ostream &operator<<(std::ostream &out, fd_t fd) {
  return out << (int)fd.fd;
}
std::ostream &operator<<(std::ostream &out, const pollfd &fd) {
  return out << "{fd:" << fd.fd << ",events:" << (int)fd.events << ",revents"
             << (int)fd.revents << "}";
}

template <class T> std::string transform(const std::deque<T> &q) {
  std::stringstream out;
  out << "Q(" << q.size() << ")[";
  if (!q.empty()) {
    out << transform(q.front());
  }
  out << ']';
  return out.str();
}

namespace detail {
template <class T>
concept dereferenceable = requires(T obj) { *obj; };
}

decltype(auto) transform(::detail::dereferenceable auto const &it) {
  return transform(*it);
}

namespace descriptors {
constexpr static inline fd_t stdin{STDIN_FILENO};

fd_t make_signal_fd() {
  sigset_t mask;
  sigemptyset(&mask);
  sigaddset(&mask, SIGUSR1);
  int fd = posix_enforce(signalfd(-1, &mask, 0));
  return {fd};
}
} // namespace descriptors

template <class T> struct yield_on_awaken {
  T value;

  yield_on_awaken(std::same_as<T> auto &&value)
      : value{std::forward<decltype(value)>(value)} {}

  T &await_resume() & { return value; }

  const T &await_resume() const & { return value; }

  T &&await_resume() && { return static_cast<T &&>(value); }
  const T &&await_resume() const && { return static_cast<const T &&>(value); }

  bool await_ready() { return false; }
  void await_suspend([[maybe_unused]] std::coroutine_handle<> h) {}
};
template <class T> yield_on_awaken(T &&) -> yield_on_awaken<T>;

struct fd_reader {
  struct scheduler_t {
    std::function<void(fd_reader)> func;
  };
  struct promise_type;
  using handle_t = std::coroutine_handle<promise_type>;
  struct promise_type {
    std::function<void(fd_reader)> schedule_ = nullptr;

    std::vector<fd_t> waiting_on_{};
    fd_t ready_signal_{-1};

    auto await_transform(fd_t fd) noexcept {
      waiting_on_.clear();
      waiting_on_.push_back(fd);
      return yield_on_awaken{fd};
    }

    std::suspend_never await_transform(scheduler_t scheduler) {
      schedule_ = std::move(scheduler.func);
      return {};
    }

    std::suspend_never initial_suspend() noexcept { return {}; }
    std::suspend_always final_suspend() noexcept { return {}; }

    fd_reader get_return_object() {
      return fd_reader{handle_t::from_promise(*this)};
    }
    void return_void() { waiting_on_.clear(); }

    void unhandled_exception() { exception_ = std::current_exception(); }

    std::exception_ptr exception_{nullptr};

    std::suspend_never yield_value(fd_reader reader) {
      assert(schedule_ != nullptr);
      schedule_(std::move(reader));
      return {};
    }
  };

  fd_reader(handle_t h) : handle_{h} {}
  fd_reader(const fd_reader &other) = delete;
  fd_reader(fd_reader &&other) noexcept
      : name_{std::move(other.name())},
        handle_(std::exchange(other.handle_, nullptr)) {}
  fd_reader &operator=(const fd_reader &other) = delete;
  fd_reader &operator=(fd_reader &&other) noexcept {
    handle_ = std::exchange(other.handle_, nullptr);
    name_ = std::move(other.name_);
    return *this;
  }

  void signal_ready(int fd) {
    assert(handle_ != nullptr);
    assert(!handle_.done());

    handle_.promise().ready_signal_ = {fd};
    handle_.resume();
    if (handle_.promise().exception_ != nullptr) {
      std::rethrow_exception(handle_.promise().exception_);
    }
  }

  const std::vector<fd_t> &waiting_on() const {
    assert(handle_);
    assert(!handle_.done());

    return handle_.promise().waiting_on_;
  }

  bool running() const { return handle_ && !handle_.done(); }
  operator bool() const { return running(); }

  ~fd_reader() {
    if (handle_) {
      handle_.destroy();
    }
  }

  std::string name_;
  fd_reader &name(std::string name) & {
    std::cout << "naming " << name;
    name_ = std::move(name);
    return *this;
  }
  fd_reader &&name(std::string name) && {
    name_ = std::move(name);
    return static_cast<fd_reader &&>(*this);
  }
  const std::string &name() const { return name_; }

private:
  handle_t handle_;
};

std::ostream &operator<<(std::ostream &out, const fd_reader &r) {
  out << "{reader: " << r.name() << ", expecting: ";
  if (r.running()) {
    out << transform(r.waiting_on());
  } else {
    out << "<done>";
  }
  return out << "}";
}
const fd_reader &transform(fd_reader *r) { return *r; }

struct fd_awaiter {

  using reader_container = std::vector<fd_reader>;
  reader_container readers;

  void run() {
    DEBUG_ONLY(auto rev = dpsg::vt100::reverse);
    using namespace std::ranges;
    std::vector<pollfd> fds;
    std::unordered_map<int, std::deque<fd_reader *>> in_waiting;

    while (any_of(readers, [](auto &r) { return r.running(); })) {
      // id the values to delete
      std::vector<reader_container::iterator> to_delete;

      // Prepare the polling data
      for (auto it = begin(readers); it != end(readers); ++it) {
        auto &r = *it;
        if (!r) {
          continue;
        }
        auto &wants = r.waiting_on();
        DEBUG(rev, "Preping: ", &r);
        for (auto w : wants) {
          pollfd event;
          event.fd = static_cast<int>(w);
          // Insert the fd in the waiting list if it isn't already there
          auto fds_it = std::lower_bound(
              fds.begin(), fds.end(), event,
              [](const pollfd &left, const pollfd &right) -> bool {
                return left.fd < right.fd;
              });
          if (fds_it == fds.end() ||
              fds_it->fd !=
                  event.fd) { // otherwise we're already waiting for this
            DEBUG(rev,
                  "Inserting before: ", fds_it == fds.end() ? -1 : fds_it->fd);
            event.events = POLLIN | POLLPRI;
            fds.emplace(fds_it, event);
          } else {
            DEBUG(rev, "Ignoring already existing fd (", fds_it->fd, ")");
          }

          // Remember that this fd is being waited on by this object
          // if it wasn't already the case
          auto it = in_waiting.emplace((int)w,
                                       std::initializer_list<fd_reader *>{&r});
          DEBUG(green | rev, "Insertion returned: ", it);
          if (!it.second) {
            for (auto &v : it.first->second) {
              if (v == &r) {
                DEBUG(rev | cyan, "Already in the list");
                goto already_waiting;
              }
            }
            DEBUG(rev | cyan, "Inserting in the list");
            it.first->second.push_back(&r);
          already_waiting:;
          } else {
            DEBUG(rev | cyan, "New list created");
          }
        }
      }

      DEBUG(rev, "Polling: ", fds);
      DEBUG(rev, "Waiting: ", in_waiting);
      // Do the poll
      [[maybe_unused]] int p = posix_enforce(poll(fds.data(), fds.size(), -1));
      assert(p != 0);

      // Execute the needed coroutines
      for (auto &fd : fds) {
        if ((fd.revents & (POLLIN | POLLPRI)) != 0) {
          DEBUG(rev, "Event on ", fd.fd, " (", fd.revents, ")");
          auto it = in_waiting.find(fd.fd);
          assert(it != in_waiting.end());
          auto &p = *it;
          auto fd = p.first;
          auto first = p.second.front();
          p.second.pop_front();
          if (p.second.empty()) {
            in_waiting.erase(it);
          }
          DEBUG(rev, "Signaling readiness: ", first);
          first->signal_ready(fd);
          DEBUG(rev, "Done");
        }
      }

      DEBUG(rev | orange, "Cleaning ", fds);
      // Clean the containers
      auto last = fds.end();
      for (auto i = fds.begin(); i < last;) {
        DEBUG(rev | yellow, "Inspecting ", *i);
        if (!in_waiting.contains(i->fd)) {
          DEBUG(rev | yellow, "Contained ", *i);
          last--;
          if (last == i) {
            break;
          }
          std::iter_swap(i, last);
        } else {
          i++;
        }
      }
      if (last != fds.end()) {
        fds.erase(last, fds.end());
      }
      // we need to sort the fds to be able to use lower_bound
      std::sort(fds.begin(), fds.end(),
                [](const pollfd &left, const pollfd &right) {
                  return left.fd < right.fd;
                });

      if (!to_delete.empty()) {
        for (auto it : to_delete) {
          DEBUG(rev | red, "Deleting ", *it, " from ", readers);
          readers.erase(it);
          DEBUG(rev | orange, "Now: ", readers);
        }
        // We have invalidated all the pointers,
        // we'll need to rebuild the index
        in_waiting.clear();
      }

      DEBUG(rev | yellow, "End of iteration, fds: ", fds);
    }

    DEBUG(vt100::reverse, "Exiting loop");
  }

  template <class... Args> void add(Args &&...args) {
    (readers.emplace_back(std::forward<Args>(args)), ...);
  }
};

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
    std::cout << color << "<FAILED TO CONNECT>" << std::endl;
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

fd_reader::scheduler_t set_scheduler(fd_awaiter &awaiter) {
  return {[&awaiter](fd_reader reader) { awaiter.add(std::move(reader)); }};
}

fd_reader spawn_tcp_server(fd_awaiter &awaiter, auto color, int listening_port,
                           in_addr_t inaddr = INADDR_ANY) {
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

struct timers {
  using clock = std::chrono::steady_clock;
  using time_point = clock::time_point;
  using duration = clock::duration;
  using timer_id = uint64_t;

  struct timer {
    int id;
    time_point when;
    std::coroutine_handle<> then;
  };

  timer_t posix_timer_id;

  timers() {
    sigevent evp;
    evp.sigev_notify = SIGEV_SIGNAL;
    evp.sigev_signo = SIGUSR1;

    timer_create(CLOCK_MONOTONIC, &evp, &posix_timer_id);
  }

  using container = std::vector<timer>;

  timer_id insert_timer(time_point d) {
    using namespace std::chrono;
    time_point next{std::numeric_limits<time_point>::max()};

    if (!timers.empty()) {
      next = timers.front().when;
    }
    std::push_heap(timers.begin(), timers.end(),
                   [](const timer &left, const timer &right) {
                     return left.when > right.when;
                   });
    auto now = clock::now();
    auto wake = d - now;
    auto secs = duration_cast<seconds>(wake);

    if (next < d) {
      itimerspec spec{
          .it_interval = {0, 0},
          .it_value = {.tv_sec = secs.count(),
                       .tv_nsec =
                           duration_cast<nanoseconds>(wake - secs).count()},
      };
      timer_settime(posix_timer_id, 0, &spec, nullptr);
    }
    return id_counter++;
  }

  fd_reader manage_timers(fd_awaiter &awaiter) {
    auto signal_fd = descriptors::make_signal_fd();

    do {
      co_await signal_fd;

      timer top = timers.front();
      auto now = clock::now();
      signalfd_siginfo info;
      posix_enforce(read(signal_fd.fd, &info, sizeof(info)));

      if (top.when <= now) {
        std::pop_heap(timers.begin(), timers.end(),
                      [](const timer &left, const timer &right) {
                        return left.when > right.when;
                      });
        timers.pop_back();
      }
    } while (true);
  }

private:
  container timers;
  timer_id id_counter = 0;
};

int main() {
  fd_awaiter awaiter;
  awaiter.add(read_from_stdin(red, 10).name("red 10"),
              read_from_stdin(green, 5).name("green 5"),
              read_once(blue).name("blue"),
              wait_on_tcp_socket(cyan, 42069, 3).name("client"),
              spawn_tcp_server(awaiter, magenta, 41069).name("server"));
  awaiter.run();
}

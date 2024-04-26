#pragma once

#include "common.hpp"
#include "common_awaitables.hpp"

#include "debug.hpp"
#include "vt100.hpp"

#include <algorithm>
#include <arpa/inet.h>
#include <cassert>
#include <coroutine>
#include <csignal>
#include <deque>
#include <functional>
#include <iostream>
#include <ranges>
#include <sstream>
#include <sys/poll.h>
#include <thread>
#include <unistd.h>
#include <unordered_map>

struct fd_t {
  int fd;
  explicit operator int() const noexcept { return fd; }
};

inline std::ostream &operator<<(std::ostream &out, fd_t fd) {
  return out << (int)fd.fd;
}
inline std::ostream &operator<<(std::ostream &out, const pollfd &fd) {
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
} // namespace descriptors

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

inline std::ostream &operator<<(std::ostream &out, const fd_reader &r) {
  out << "{reader: " << r.name() << ", expecting: ";
  if (r.running()) {
    out << transform(r.waiting_on());
  } else {
    out << "<done>";
  }
  return out << "}";
}
inline const fd_reader &transform(fd_reader *r) { return *r; }

struct fd_scheduler_round_robin {

  using reader_container = std::vector<fd_reader>;
  reader_container readers;

  void run() {
    using namespace dpsg;
    using namespace dpsg::vt100;
    using namespace std::ranges;
    DEBUG_ONLY(auto rev = vt100::reverse);
    std::vector<pollfd> fds;
    std::unordered_map<int, std::deque<fd_reader *>> in_waiting;

    while (any_of(readers, [](auto &r) { return r.running(); })) {
      // id the values to delete
      std::vector<reader_container::iterator> to_delete;

      // Prepare the polling data
      for (auto it = begin(readers); it != end(readers); ++it) {
        auto &r = *it;
        if (!r) {
          to_delete.push_back(it);
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

struct fd_scheduler_single_consumer {
  using consumer_t = fd_reader;
  using consumer_pointer = consumer_t *;

  using reader_container = std::vector<consumer_t>;
  reader_container readers;

  void run() {
    using namespace dpsg;
    using namespace dpsg::vt100;
    using namespace std::ranges;
    std::vector<pollfd> fds;
    std::unordered_map<int, consumer_pointer> in_waiting;
    auto start = std::chrono::steady_clock::now();

    while (any_of(readers, [](auto &r) { return r.running(); })) {
      std::vector<reader_container::iterator> to_delete;
      // For each file descriptor
      // Find a live reader
      for (auto it = begin(readers); it != end(readers); ++it) {
        auto &r = *it;
        if (!r) {
          // it's dead
          // delete it at the end
          to_delete.push_back(it);
          continue;
        }

        auto &wants = r.waiting_on();
        for (auto w : wants) {
          pollfd event;
          event.fd = static_cast<int>(w);

          // Insert the fd in the waiting list if it isn't already there
          auto fds_it = std::lower_bound(
              fds.begin(), fds.end(), event,
              [](const pollfd &left, const pollfd &right) -> bool {
                return left.fd < right.fd;
              });

          if (fds_it == fds.end() || fds_it->fd != event.fd) {
            event.events = POLLIN | POLLPRI;
            fds.emplace(fds_it, event);
          } // otherwise we're already waiting for this
          else {
            throw std::runtime_error("Multiple consumers for " +
                                     std::to_string(event.fd));
          }
          in_waiting.emplace(w, &r);
        }
      }

      assert(!fds.empty());
      // Do the poll
      [[maybe_unused]] int p = posix_enforce(poll(fds.data(), fds.size(), -1));
      for (auto &fd : fds) {
        // resume the coroutine matching the event
        if ((fd.revents & (POLLIN | POLLPRI)) != 0) {
          auto it = in_waiting.find(fd.fd);
          assert(it != in_waiting.end());
          auto fd = it->first;
          auto first = it->second;

          first->signal_ready(fd);
          start = std::chrono::steady_clock::now();
          in_waiting.erase(it);
        }
      }

      // Delete unused file descriptors from the list
      DEBUG_ONLY(size_t last_size = fds.size());
      auto last = fds.end();
      for (auto i = fds.begin(); i < last;) {
        if (!in_waiting.contains(i->fd)) {
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
      assert(fds.size() != last_size);

      if (!to_delete.empty()) {
        for (auto it : to_delete) {
          readers.erase(it);
        }
        // We have invalidated all the pointers,
        // we'll need to rebuild the index
        in_waiting.clear();
      }
    }
  }

  template <class... Args> void add(Args &&...args) {
    (readers.emplace_back(std::forward<Args>(args)), ...);
  }
};

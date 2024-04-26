#pragma once

#include "basic_type.hpp"
#include "common.hpp"
#include "fd_coroutine.hpp"

#include <algorithm>
#include <chrono>
#include <functional>
#include <limits>
#include <sys/signalfd.h>

namespace descriptors {

inline fd_t make_signal_fd() {
  sigset_t mask;
  sigemptyset(&mask);
  sigaddset(&mask, SIGUSR1);
  posix_enforce(sigprocmask(SIG_BLOCK, &mask, nullptr));
  int fd = posix_enforce(signalfd(-1, &mask, 0));
  return {fd};
}

} // namespace descriptors

template <class T>
concept Duration = template_instance_of<T, std::chrono::duration>;
using timer_id = uint64_t;
constexpr static inline timer_id invalid_timer =
    std::numeric_limits<timer_id>::max();

struct start_immediately : std::suspend_never {};
struct start_defered : std::suspend_always {};

template <one_of<start_immediately, start_defered> T = start_defered>
struct basic_timer {
  struct promise_type;
  using policy_t = T;
  using handle_t = std::coroutine_handle<promise_type>;
  using duration_type = std::chrono::system_clock::duration;
  using scheduler_signature = void(duration_type, handle_t);
  using scheduler_type = std::function<scheduler_signature>;

  struct resume_caller_when_done {
    std::coroutine_handle<promise_type> self;

    bool await_ready() noexcept { return false; }
    void
    await_suspend(std::coroutine_handle<promise_type> resume_me_pls) noexcept {
      self.promise().next = resume_me_pls;
      self.promise().scheduler_ = std::move(resume_me_pls.promise().scheduler_);
      self.resume();
    }

    void await_resume() noexcept {}
  };

  struct promise_type : basic_promise<void, basic_timer, promise_type> {
    scheduler_type scheduler_{nullptr};
    timer_id id{0};
    std::coroutine_handle<promise_type> next{nullptr};
    policy_t initial_suspend() noexcept { return {}; }
    std::suspend_never final_suspend() noexcept { return {}; }

    std::suspend_always await_transform(Duration auto duration) noexcept {
      assert(scheduler_ != nullptr);
      scheduler_(
          std::chrono::duration_cast<std::chrono::system_clock::duration>(
              duration),
          handle_t::from_promise(*this));
      return {};
    }

    template <class U>
      requires(!Duration<std::decay_t<U>>)
    decltype(auto) await_transform(U &&u) noexcept {
      return std::forward<U>(u);
    }

    void return_void() {
      if (next != nullptr) {
        next.promise().scheduler_ = std::move(scheduler_);
        next.resume();
      }
    }
  };

  basic_timer(handle_t h) : handle_{h} {}
  basic_timer(const basic_timer &other) = delete;
  basic_timer(basic_timer &&other) noexcept
      : handle_(std::exchange(other.handle_, nullptr)) {}
  basic_timer &operator=(const basic_timer &other) = delete;
  basic_timer &operator=(basic_timer &&other) noexcept {
    handle_ = std::exchange(other.handle_, nullptr);
    return *this;
  }

  void set_scheduler(scheduler_type scheduler) {
    assert(handle_ != nullptr);
    handle_.promise().scheduler_ = std::move(scheduler);
  }

  template <std::same_as<start_defered> U = policy_t> void start() {
    assert(handle_ != nullptr);
    assert(handle_.promise().scheduler_ != nullptr);
    handle_.resume();
  }

  resume_caller_when_done operator co_await() const noexcept {
    return resume_caller_when_done{.self = this->handle_};
  }

private:
  handle_t handle_;
};

using timer = basic_timer<start_defered>;

struct timer_scheduler {
  using clock = std::chrono::steady_clock;
  using time_point = clock::time_point;
  using duration = clock::duration;

  struct timer_handle {
    timer_id id;
    time_point when;
    std::coroutine_handle<> then;
    friend std::ostream &operator<<(std::ostream &out, timer_handle th) {
      return out << "!th{id:" << th.id << ",when:"
                 << (std::chrono::duration_cast<std::chrono::milliseconds>(
                         th.when.time_since_epoch()) %
                     10000)
                 << ",then:" << th.then.address() << "}";
    }
  };

  timer_t posix_timer_id;

  timer_scheduler() {
    sigevent evp;
    evp.sigev_notify = SIGEV_SIGNAL;
    evp.sigev_signo = SIGUSR1;

    timer_create(CLOCK_MONOTONIC, &evp, &posix_timer_id);
  }

  using container = std::vector<timer_handle>;

  template <class A, class B>
  timer_id insert_timer(std::chrono::duration<A, B> wait,
                        std::coroutine_handle<> to_resume) {
    auto now = clock::now();
    auto this_counter = id_counter++;
    time_point when = now + std::chrono::duration_cast<duration>(wait);

    bool schedule_me = true;
    if (!timers_.empty()) {
      auto next = timers_.front().when;

      schedule_me = when < next;

    } else {
    }

    timers_.push_back(timer_handle{
        .id = this_counter,
        .when = when,
        .then = to_resume,
    });
    std::ranges::push_heap(
        timers_.begin(), timers_.end(),
        [](const timer_handle &left, const timer_handle &right) {
          return left.when > right.when;
        });

    // New timer is before last soonest, reschedule
    if (schedule_me) {
      schedule_(wait);
    }
    return this_counter;
  }

  template <template_instance_of<basic_timer>... Timer>
  void add(Timer &&...new_timers) {
    (
        [&] {
          new_timers.set_scheduler([this](Timer::duration_type duration,
                                          std::coroutine_handle<> resumable) {
            insert_timer(duration, resumable);
          });
          new_timers.start();
        }(),
        ...); // yeah, don't mention it
  }

  fd_reader manage_timers() {
    running = true;
    auto signal_fd = descriptors::make_signal_fd();

    do {
      co_await signal_fd;
      signalfd_siginfo info;
      posix_enforce(read(signal_fd.fd, &info, sizeof(info)));

      for (;;) {
        if (!running) {
          co_return;
        }
        assert(!timers_.empty());
        timer_handle top = timers_.front();

        auto now = clock::now();
        if (top.when <= now) {
          std::ranges::pop_heap(
              timers_, [](const timer_handle &left, const timer_handle &right) {
                return left.when > right.when;
              });
          timers_.pop_back();
          top.then.resume();
        } else {
          schedule_(top.when - now);
          break;
        }
        if (timers_.empty()) {
          break;
        }
      }
    } while (true);
  }

  void stop() {
    running = false;
    raise(SIGUSR1);
  }

private:
  bool running = false;
  container timers_;
  timer_id id_counter = 0;

  template <class A, class B>
  void schedule_(std::chrono::duration<A, B> wake_time) {
    using namespace std::chrono;
    auto secs = duration_cast<seconds>(wake_time);
    itimerspec spec{
        .it_interval = {0, 0},
        .it_value = {.tv_sec = secs.count(),
                     .tv_nsec =
                         duration_cast<nanoseconds>(wake_time - secs).count()},
    };
    timer_settime(posix_timer_id, 0, &spec, nullptr);
  }
};

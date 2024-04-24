#ifndef HEADER_GUART_THREADED_TASK_HPP
#define HEADER_GUART_THREADED_TASK_HPP

#include "basic_type.hpp"

#include <condition_variable>
#include <coroutine>
#include <optional>
#include <semaphore>
#include <thread>
#include <vector>

struct empty {};

template <size_t N, class Result> struct parallel {
  using handle_t = std::coroutine_handle<>;
  using rtype = std::vector<Result>;
  struct impl_ {
    std::mutex mutex_{};
    std::condition_variable signal_{};
    handle_t continuation_{};
    rtype results_{};
  };
  std::unique_ptr<impl_> impl_;
  struct impl_* operator->() {
    return impl_.get();
  }

  template <class... Fs>
    requires(sizeof...(Fs) == N &&
             std::is_convertible_v<
                 std::common_type_t<std::invoke_result_t<Fs>...>, Result>)
  parallel(Fs &&...functions) : impl_{std::make_unique<struct impl_>()} {
    (spawn_(std::forward<Fs>(functions)), ...);
  }

  parallel(const parallel &other) = delete;
  parallel(parallel &&other) noexcept : impl_{std::move(other.impl_)} {}
  parallel &operator=(const parallel &other) = delete;
  parallel &operator=(parallel &&other) noexcept = default;

  template <class F> void spawn_(F &&func) {
    std::thread([self = impl_.get(), f = std::forward<F>(func)] {
      auto r = f();
      std::unique_lock lock{self->mutex_};
      self->results_.emplace_back(std::move(r));
      if (self->continuation_ != nullptr && self->results_.size() == N) {
        DEBUG(orange, "Continuing, size is ", self->results_.size());
        self->continuation_.resume();
        return;
      }
      lock.unlock();
      self->signal_.notify_one();
    }).detach();
  }

  void join() {
    std::unique_lock lock{impl_->mutex_};
    if (impl_->results_.size() != N) {
      impl_->signal_.wait(lock, [&] { return impl_->results_.size() == N; });
    }
  }

  bool done() {
    std::lock_guard lock{impl_->mutex_};
    return impl_->results_.size() == N;
  }

  bool attach(handle_t h) {
    std::lock_guard lock{impl_->mutex_};
    if (impl_->results_.size() == N) {
      return true;
    }
    impl_->continuation_ = h;
    return false;
  }

  rtype &results() & { return impl_->results_; }
  const rtype &results() const & { return impl_->results_; }
  rtype &&results() && { return static_cast<rtype &&>(impl_->results_); }
  const rtype &&results() const && {
    return static_cast<const rtype &&>(impl_->results_);
  }

  ~parallel() { DEBUG(purple, "parallel is being destroyed: "); }

  struct ref_awaitable {
    parallel &ref_;

    bool await_ready() noexcept { return ref_.done(); };
    bool await_suspend(std::coroutine_handle<> handle) noexcept {
      return !ref_.attach(handle);
    }

    auto await_resume() noexcept { return ref_.results(); }
  };
  auto operator co_await() & { return ref_awaitable{*this}; }

  struct move_awaitable {
    parallel ref_;

    bool await_ready() noexcept { return ref_.done(); };
    bool await_suspend(std::coroutine_handle<> handle) noexcept {
      return !ref_.attach(handle);
    }

    auto await_resume() noexcept { return std::move(ref_.results()); }
  };
  auto operator co_await() && { return move_awaitable{std::move(*this)}; }
};

template <class... Fs>
parallel(Fs &&...)
    -> parallel<sizeof...(Fs), std::common_type_t<std::invoke_result_t<Fs>...>>;

#endif // HEADER_GUART_THREADED_TASK_HPP

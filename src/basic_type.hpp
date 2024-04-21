#pragma once
#include <cassert>
#include <coroutine>
#include <utility>

#include "debug.hpp"

template <class T, class I, class Self> struct basic_promise {
  using handle_type = std::coroutine_handle<Self>;
  handle_type get_handle() { return handle_type::from_promise(*static_cast<Self *>(this)); }
  T result_;
  I get_return_object() {
    return I{get_handle()};
  }
  std::suspend_never initial_suspend() { return {}; }
  std::suspend_always final_suspend() noexcept { return {}; }
  template <std::convertible_to<T> U> void return_value(U &&value) {
    DEBUG(purple, "return_value called with ", orange, value, purple, " in ", get_handle().address());
    result_ = std::forward<U>(value);
  }
  void unhandled_exception() {}
  T &&result() && {DEBUG(purple, "returning ", orange, result_, purple, " in ", get_handle().address()); return std::move(result_); }
  T &result() & { DEBUG(purple, "returning ", orange, result_, purple, " in ", get_handle().address());return result_; }
  const T &&result() const && { DEBUG(purple, "returning ", orange, result_, purple, " in ", get_handle().address());return std::move(result_); }
  const T &result() const & { DEBUG(purple, "returning ", orange, result_, purple, " in ", get_handle().address());return result_; }
};

template <class I, class Self> struct basic_promise<void, I, Self> {
  using handle_type = std::coroutine_handle<Self>;
  I get_return_object() {
    return I{handle_type::from_promise(*static_cast<Self *>(this))};
  }
  std::suspend_never initial_suspend() { return {}; }
  std::suspend_never final_suspend() noexcept { return {}; }
  void return_void() {}
  void unhandled_exception() {}
};

template<class P>
struct resumable_handle {
  using handle_type = std::coroutine_handle<P>;

  explicit(true) resumable_handle(handle_type coro) : handle_(coro) {}
  resumable_handle(const resumable_handle &) = delete;
  resumable_handle(resumable_handle && other) : handle_(std::exchange(other.handle_, nullptr)) {

  }
  resumable_handle& operator=(const resumable_handle &) = delete;
  resumable_handle& operator=(resumable_handle && other) {
    this->handle_ = std::exchange(other.handle_, nullptr);
  }
  void resume() { handle_.resume(); }
  handle_type get_handle() const { return handle_; }
  handle_type release_handle() { return std::exchange(handle_, nullptr); }

private:
  handle_type handle_;
};

template <class T, class P> struct trivial_task : resumable_handle<P> {
  using handle_type = resumable_handle<P>::handle_type;

  explicit(false) trivial_task(handle_type coro) : resumable_handle<P>(coro) {}
  ~trivial_task() {
    if (this->get_handle() != nullptr) {
      this->get_handle().destroy();
    }
  }

  T &&result() && {  return std::move(this->get_handle().promise().result); }
  T &result() & { return this->get_handle().promise().result; }
  const T &&result() const && { return std::move(this->get_handle().promise().result); }
  const T &result() const & { return this->get_handle().promise().result; }
};

template <class P> struct trivial_task<void, P> : resumable_handle<P> {
  using handle_type = resumable_handle<P>::handle_type;

  explicit(false) trivial_task(handle_type coro) : resumable_handle<P>(coro) {}
};

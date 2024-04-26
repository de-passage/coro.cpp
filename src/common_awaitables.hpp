#pragma once

#include "concepts.hpp"

#include <coroutine>
#include <utility>

struct suspend_if {
  bool should_suspend;
  constexpr explicit suspend_if(bool should_suspend) : should_suspend(should_suspend) {}
  constexpr bool await_ready() const noexcept { return !should_suspend; }
  constexpr void await_suspend([[maybe_unused]] std::coroutine_handle<> coro) const noexcept {}
  constexpr void await_resume() const noexcept {}
};
static_assert(Awaitable<suspend_if>);

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

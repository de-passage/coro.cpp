#pragma once

#include <concepts>
#include <coroutine>

template <class T>
concept Executor = requires(T t) {
  { t.execute() } -> std::same_as<void>;
};

template<class T> concept Resumable = requires(T t) {
  { t.resume() } -> std::same_as<void>;
};

template <class T, class Executor>
concept Executable = requires(T t, Executor e) {
  requires Resumable<T>;
  e.schedule(t);
};

template <class T>
concept Awaitable = requires(T t) {
  { t.await_ready() } -> std::same_as<bool>;
  requires requires {
    { t.await_suspend(std::coroutine_handle<>{}) } -> std::same_as<void>;
  } || requires {
    { t.await_suspend(std::coroutine_handle<>{}) } -> std::same_as<bool>;
  } || requires {
    {
      t.await_suspend(std::coroutine_handle<>{})
    } -> std::convertible_to<std::coroutine_handle<>>;
  };
  t.await_resume();
};

template<class T>
concept CoroutineWithExecutor = requires(T t) {
  { t.promise().executor } -> Executor;
};

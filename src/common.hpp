#pragma once

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

#include <utility>

template <class T, template <class...> class C>
struct is_template_instance : std::false_type {};
template <template <class...> class C, class... Args>
struct is_template_instance<C<Args...>, C> : std::true_type {};

template <class T, template <class...> class C>
constexpr static inline bool is_template_instance_v =
    is_template_instance<T, C>::value;

template <class T, template <class...> class C>
concept template_instance_of = is_template_instance_v<T, C>;

template <class T, class... Args> struct is_one_of {
  constexpr static inline bool value = (std::is_same_v<T, Args> || ...);
};
template <class T, class... Args>
constexpr static inline bool value = is_one_of<T, Args...>::value;

template <class T, class... Args>
concept one_of = is_one_of<T, Args...>::value;

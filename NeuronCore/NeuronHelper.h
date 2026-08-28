#pragma once

#include <type_traits>

namespace Neuron
{
// bugprone-macro-parentheses wants every macro argument wrapped, and T here is a TYPE: it appears
// as static_cast<T>, T&, and in ## pastes, none of which can take parentheses -- static_cast<(T)>
// does not compile. The check cannot tell a type parameter from a value one, so it is silenced for
// this block only, with the reason, rather than disabled for the tree.
// NOLINTBEGIN(bugprone-macro-parentheses)

// Sequential enum helper: provides ++, --, dereference, SizeOf, Iterator, and Range.
// Use for enums whose values are sequential indices (e.g., SpaceObjectCategory).
// Does NOT provide bitwise operators — use ENUM_FLAGS_HELPER for flag enums.
#define ENUM_HELPER(T, S, E)                                                            \
  T inline operator++(T& _value) noexcept                                               \
  {                                                                                     \
    return _value = static_cast<T>(static_cast<std::underlying_type_t<T>>(_value) + 1); \
  }                                                                                     \
  T inline operator++(T& _value, int) noexcept                                          \
  {                                                                                     \
    T old = _value;                                                                     \
    _value = static_cast<T>(static_cast<std::underlying_type_t<T>>(_value) + 1);        \
    return old;                                                                         \
  }                                                                                     \
  T inline operator--(T& _value) noexcept                                               \
  {                                                                                     \
    return _value = static_cast<T>(static_cast<std::underlying_type_t<T>>(_value) - 1); \
  }                                                                                     \
  T inline operator--(T& _value, int) noexcept                                          \
  {                                                                                     \
    T old = _value;                                                                     \
    _value = static_cast<T>(static_cast<std::underlying_type_t<T>>(_value) - 1);        \
    return old;                                                                         \
  }                                                                                     \
  inline T operator*(T _type) noexcept                                                  \
  {                                                                                     \
    return _type;                                                                       \
  }                                                                                     \
  constexpr size_t SizeOf##T() noexcept                                                 \
  {                                                                                     \
    return static_cast<size_t>(T::E) + 1;                                               \
  }                                                                                     \
  class It##T                                                                           \
  {                                                                                     \
    int value;                                                                          \
                                                                                        \
  public:                                                                               \
    explicit It##T(int v)                                                               \
      : value(v)                                                                        \
    {                                                                                   \
    }                                                                                   \
    ##T operator*() const                                                               \
    {                                                                                   \
      return static_cast<##T>(value);                                                   \
    }                                                                                   \
    bool operator!=(const It##T& other) const                                           \
    {                                                                                   \
      return value != other.value;                                                      \
    }                                                                                   \
    It##T& operator++()                                                                 \
    {                                                                                   \
      ++value;                                                                          \
      return *this;                                                                     \
    }                                                                                   \
  };                                                                                    \
  class Range##T                                                                        \
  {                                                                                     \
  public:                                                                               \
    It##T begin() const                                                                 \
    {                                                                                   \
      return It##T(static_cast<std::underlying_type_t<T>>(T::S));                       \
    }                                                                                   \
    It##T end() const                                                                   \
    {                                                                                   \
      return It##T(static_cast<std::underlying_type_t<T>>(T::E) + 1);                   \
    }                                                                                   \
  };
//template <> struct std::formatter<T> : std::formatter<int> {                                                                                     \
  //  auto format(const T& id, std::format_context& ctx) const {return std::formatter<int>::format(static_cast<int>(id), ctx); }                     \
  //}

// Flags enum helper: provides bitwise operators (|, &, ^, ~, !) for enums used as bit flags.
// Also includes sequential helpers from ENUM_HELPER.
#define ENUM_FLAGS_HELPER(T, S, E)                                                                  \
  ENUM_HELPER(T, S, E)                                                                              \
  constexpr T operator|(T a, T b) noexcept                                                          \
  {                                                                                                 \
    return T(((_ENUM_FLAG_SIZED_INTEGER<T>::type)a) | ((_ENUM_FLAG_SIZED_INTEGER<T>::type)b));      \
  }                                                                                                 \
  inline T& operator|=(T& a, T b) noexcept                                                          \
  {                                                                                                 \
    return (T&)(((_ENUM_FLAG_SIZED_INTEGER<T>::type&)a) |= ((_ENUM_FLAG_SIZED_INTEGER<T>::type)b)); \
  }                                                                                                 \
  constexpr T operator&(T a, T b) noexcept                                                          \
  {                                                                                                 \
    return T(((_ENUM_FLAG_SIZED_INTEGER<T>::type)a) & ((_ENUM_FLAG_SIZED_INTEGER<T>::type)b));      \
  }                                                                                                 \
  inline T& operator&=(T& a, T b) noexcept                                                          \
  {                                                                                                 \
    return (T&)(((_ENUM_FLAG_SIZED_INTEGER<T>::type&)a) &= ((_ENUM_FLAG_SIZED_INTEGER<T>::type)b)); \
  }                                                                                                 \
  constexpr T operator~(T a) noexcept                                                               \
  {                                                                                                 \
    return T(~((_ENUM_FLAG_SIZED_INTEGER<T>::type)a));                                              \
  }                                                                                                 \
  constexpr T operator^(T a, T b) noexcept                                                          \
  {                                                                                                 \
    return T(((_ENUM_FLAG_SIZED_INTEGER<T>::type)a) ^ ((_ENUM_FLAG_SIZED_INTEGER<T>::type)b));      \
  }                                                                                                 \
  inline T& operator^=(T& a, T b) noexcept                                                          \
  {                                                                                                 \
    return (T&)(((_ENUM_FLAG_SIZED_INTEGER<T>::type&)a) ^= ((_ENUM_FLAG_SIZED_INTEGER<T>::type)b)); \
  }                                                                                                 \
  constexpr bool operator!(T a) noexcept                                                            \
  {                                                                                                 \
    return !((_ENUM_FLAG_SIZED_INTEGER<T>::type)a);                                                 \
  }

// NOLINTEND(bugprone-macro-parentheses)

template <typename T> constexpr bool IsValidEnum(T _value) noexcept
{
  return (_value >= begin(_value) && _value < end(_value));
}

template <typename T> constexpr size_t I(T _value) noexcept
{
  return static_cast<size_t>(_value);
}

class BaseException : public std::exception
{
public:
  BaseException(std::string _s) noexcept
    : m_s(std::move(_s))
  {
  }
  ~BaseException() noexcept override = default;

  [[nodiscard]] const char* what() const noexcept override
  {
    return m_s.c_str();
  }

protected:
  std::string m_s;
};

struct NonCopyable
{
  NonCopyable() = default;
  NonCopyable(const NonCopyable&) = delete;
  NonCopyable& operator=(const NonCopyable&) = delete;
};

struct HandleCloser
{
  void operator()(HANDLE _handle) const noexcept
  {
    if (_handle)
      CloseHandle(_handle);
  }
};

using ScopedHandle = std::unique_ptr<void, HandleCloser>;
inline HANDLE SafeHandle(HANDLE _handle) noexcept
{
  return (_handle == INVALID_HANDLE_VALUE) ? nullptr : _handle;
}
} // namespace Neuron

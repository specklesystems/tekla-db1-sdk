#pragma once

#include <tekla/db1/error.hpp>
#include <utility>
#include <variant>

namespace tekla::db1 {

template <typename T>
class Result {
 public:
  static Result success(T value) { return Result(std::move(value)); }
  static Result failure(Error error) { return Result(std::move(error)); }

  [[nodiscard]] bool has_value() const noexcept { return std::holds_alternative<T>(state_); }
  [[nodiscard]] explicit operator bool() const noexcept { return has_value(); }

  [[nodiscard]] T& value() & { return std::get<T>(state_); }
  [[nodiscard]] const T& value() const& { return std::get<T>(state_); }
  [[nodiscard]] T&& value() && { return std::get<T>(std::move(state_)); }

  [[nodiscard]] Error& error() & { return std::get<Error>(state_); }
  [[nodiscard]] const Error& error() const& { return std::get<Error>(state_); }

 private:
  explicit Result(T value) : state_(std::move(value)) {}
  explicit Result(Error error) : state_(std::move(error)) {}

  std::variant<T, Error> state_;
};

}  // namespace tekla::db1

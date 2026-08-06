#pragma once

#include <cstddef>

namespace tekla::db1::detail {

struct ArchiveLimits {
  std::size_t archives = 64U;
  std::size_t entries = 65'536U;
  std::size_t inflated_bytes = 256U * 1024U * 1024U;
};

class ArchiveBudget {
 public:
  explicit ArchiveBudget(ArchiveLimits limits = {}) : limits_(limits) {}

  [[nodiscard]] bool reserve_archives(std::size_t count) noexcept {
    return reserve(count, limits_.archives, archives_);
  }

  [[nodiscard]] bool reserve_entries(std::size_t count) noexcept {
    return reserve(count, limits_.entries, entries_);
  }

  [[nodiscard]] bool reserve_inflated_bytes(std::size_t count) noexcept {
    return reserve(count, limits_.inflated_bytes, inflated_bytes_);
  }

 private:
  [[nodiscard]] static bool reserve(std::size_t count, std::size_t limit,
                                    std::size_t& used) noexcept {
    if (used > limit || count > limit - used) return false;
    used += count;
    return true;
  }

  ArchiveLimits limits_;
  std::size_t archives_ = 0U;
  std::size_t entries_ = 0U;
  std::size_t inflated_bytes_ = 0U;
};

}  // namespace tekla::db1::detail

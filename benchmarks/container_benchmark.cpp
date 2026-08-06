#include <array>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <memory>
#include <span>
#include <string_view>
#include <tekla/db1/model.hpp>
#include <vector>

#if defined(_WIN32)
#define NOMINMAX
#include <Psapi.h>
#include <Windows.h>
#else
#include <sys/resource.h>
#endif

namespace {

class SharedBytes final : public tekla::db1::ByteSource {
 public:
  explicit SharedBytes(std::shared_ptr<const std::vector<std::byte>> bytes)
      : bytes_(std::move(bytes)) {}

  [[nodiscard]] std::span<const std::byte> bytes() const noexcept override { return *bytes_; }

 private:
  std::shared_ptr<const std::vector<std::byte>> bytes_;
};

void append_u32(std::vector<std::byte>& bytes, std::uint32_t value) {
  for (unsigned shift = 0; shift != 32; shift += 8) {
    bytes.push_back(static_cast<std::byte>((value >> shift) & 0xffU));
  }
}

void append_ascii(std::vector<std::byte>& bytes, std::string_view text) {
  bytes.insert(bytes.end(), reinterpret_cast<const std::byte*>(text.data()),
               reinterpret_cast<const std::byte*>(text.data() + text.size()));
}

void append_record(std::vector<std::byte>& bytes, std::uint32_t value) {
  bytes.push_back(std::byte{0});
  append_u32(bytes, value);
  append_u32(bytes, 7);
  append_u32(bytes, 9);
}

std::vector<std::byte> synthetic_database(std::uint32_t rows) {
  constexpr std::array<std::byte, 4> table_end{std::byte{0x66}, std::byte{0xc0}, std::byte{0xce},
                                               std::byte{0xdb}};
  constexpr std::array<std::byte, 4> final_footer{std::byte{0x4f}, std::byte{0x61}, std::byte{0xbc},
                                                  std::byte{0x00}};
  std::vector<std::byte> bytes;
  bytes.reserve(128U + static_cast<std::size_t>(rows) * 13U);
  append_ascii(bytes, "Xsteel");
  bytes.push_back(std::byte{0x85});
  append_ascii(bytes, " 9.66 7d72d8c9-0250-4f3a-8760-bcef517f016e");
  append_u32(bytes, 1);
  bytes.insert(bytes.end(), table_end.begin(), table_end.end());
  append_u32(bytes, 4);
  append_u32(bytes, 1);
  append_u32(bytes, 0);
  for (std::uint32_t row = 0; row < rows; ++row) append_record(bytes, row);
  bytes.push_back(std::byte{0});
  bytes.insert(bytes.end(), table_end.begin(), table_end.end());
  append_u32(bytes, 4);
  append_u32(bytes, 1);
  append_u32(bytes, 0);
  append_record(bytes, 0);
  bytes.push_back(std::byte{0});
  bytes.insert(bytes.end(), final_footer.begin(), final_footer.end());
  return bytes;
}

bool parse_u32(std::string_view text, std::uint32_t& value) {
  const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
  return result.ec == std::errc{} && result.ptr == text.data() + text.size();
}

std::uint64_t peak_rss_bytes() {
#if defined(_WIN32)
  PROCESS_MEMORY_COUNTERS counters{};
  counters.cb = sizeof(counters);
  if (GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof(counters)) == 0) return 0;
  return static_cast<std::uint64_t>(counters.PeakWorkingSetSize);
#else
  rusage usage{};
  if (getrusage(RUSAGE_SELF, &usage) != 0) return 0;
#if defined(__APPLE__)
  return static_cast<std::uint64_t>(usage.ru_maxrss);
#else
  return static_cast<std::uint64_t>(usage.ru_maxrss) * 1024U;
#endif
#endif
}

}  // namespace

int main(int argc, char** argv) {
  std::uint32_t rows = 250'000;
  std::uint32_t iterations = 10;
  for (int index = 1; index < argc; ++index) {
    const std::string_view argument(argv[index]);
    if ((argument == "--rows" || argument == "--iterations") && index + 1 < argc) {
      std::uint32_t value = 0;
      if (!parse_u32(argv[++index], value) || value == 0) {
        std::fprintf(stderr, "benchmark counts must be positive integers\n");
        return 2;
      }
      if (argument == "--rows")
        rows = value;
      else
        iterations = value;
    } else {
      std::fprintf(stderr, "usage: tekla-db1-container-benchmark [--rows N] [--iterations N]\n");
      return 2;
    }
  }
  if (rows > 10'000'000U || iterations > 10'000U) {
    std::fprintf(stderr, "benchmark request exceeds its safety ceiling\n");
    return 2;
  }

  auto storage = std::make_shared<const std::vector<std::byte>>(synthetic_database(rows));
  auto source = std::make_shared<const SharedBytes>(storage);
  std::uint64_t observed_rows = 0;
  const auto started = std::chrono::steady_clock::now();
  for (std::uint32_t iteration = 0; iteration < iterations; ++iteration) {
    tekla::db1::ModelPackage package;
    package.add(tekla::db1::Asset(tekla::db1::AssetRole::model_database, "benchmark.db1", source));
    auto model = tekla::db1::open(std::move(package));
    if (!model) {
      std::fprintf(stderr, "synthetic benchmark fixture failed: %s\n",
                   model.error().message.c_str());
      return 1;
    }
    observed_rows += model.value().info().visible_rows;
  }
  const auto elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - started);
  if (observed_rows != static_cast<std::uint64_t>(iterations) * (rows + 1ULL)) return 1;
  const auto total_bytes = static_cast<double>(storage->size()) * iterations;
  const double mib_per_second = total_bytes / (1024.0 * 1024.0) / elapsed.count();
  std::printf(
      "{\"benchmark\":\"owned_buffer_container_open\",\"payload_bytes\":%zu,"
      "\"rows\":%u,\"iterations\":%u,\"seconds\":%.9f,\"mib_per_second\":%.3f,"
      "\"peak_rss_bytes\":%llu}\n",
      storage->size(), rows, iterations, elapsed.count(), mib_per_second,
      static_cast<unsigned long long>(peak_rss_bytes()));
  return 0;
}

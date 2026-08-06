#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdio>
#include <span>
#include <vector>

#include "occt/protocol.hpp"

namespace {

bool read_exact(std::span<std::byte> bytes) {
  std::size_t offset = 0;
  while (offset < bytes.size()) {
    const auto count = std::fread(bytes.data() + offset, 1, bytes.size() - offset, stdin);
    if (count == 0) return false;
    offset += count;
  }
  return true;
}

bool write_exact(std::span<const std::byte> bytes) {
  std::size_t offset = 0;
  while (offset < bytes.size()) {
    const auto count = std::fwrite(bytes.data() + offset, 1, bytes.size() - offset, stdout);
    if (count == 0) return false;
    offset += count;
  }
  return std::fflush(stdout) == 0;
}

}  // namespace

int main() {
  using namespace tekla::db1::detail;
  for (;;) {
    std::array<std::byte, kOcctRequestHeaderSize> header{};
    if (!read_exact(header)) return std::feof(stdin) != 0 ? 0 : 1;
    auto request_size = occt_request_size(header);
    if (!request_size) return 2;
    std::vector<std::byte> request(request_size.value());
    std::copy(header.begin(), header.end(), request.begin());
    if (!read_exact(std::span(request).subspan(header.size()))) return 1;
    auto decoded = decode_occt_request(request);
    if (!decoded) return 2;
    auto evaluation = evaluate_occt(decoded.value());
    auto response = encode_occt_response(evaluation);
    if (!response) {
      auto fallback = encode_occt_response(tekla::db1::Result<OcctMesh>::failure(response.error()));
      if (!fallback || !write_exact(fallback.value())) return 1;
      continue;
    }
    if (!write_exact(response.value())) return 1;
  }
}

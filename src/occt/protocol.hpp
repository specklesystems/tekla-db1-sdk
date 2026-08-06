#pragma once

#include <array>
#include <cstddef>
#include <span>
#include <vector>

#include "occt.hpp"

namespace tekla::db1::detail {

[[nodiscard]] Result<std::vector<std::byte>> encode_occt_request(const OcctRequest& request);
[[nodiscard]] Result<OcctRequest> decode_occt_request(std::span<const std::byte> bytes);

struct TranslationNormalizedOcctRequest {
  std::vector<std::byte> bytes;
  std::array<double, 3> origin{};
};

// Produces a byte-exact cache key for requests that differ only by a model-space
// translation. Object ids are deliberately excluded: they identify owners, not
// topology. The original request is never mutated.
[[nodiscard]] Result<TranslationNormalizedOcctRequest> encode_translation_normalized_occt_request(
    const OcctRequest& request);
[[nodiscard]] Result<std::vector<std::byte>> encode_occt_response(const Result<OcctMesh>& response);
[[nodiscard]] Result<OcctMesh> decode_occt_response(std::span<const std::byte> bytes);
[[nodiscard]] Result<std::size_t> occt_request_size(std::span<const std::byte> header);
[[nodiscard]] Result<std::size_t> occt_response_size(std::span<const std::byte> header);

inline constexpr std::size_t kOcctRequestHeaderSize = 112;
inline constexpr std::size_t kOcctResponseHeaderSize = 56;
inline constexpr std::size_t kOcctMaxMessageBytes = 512U * 1024U * 1024U;

}  // namespace tekla::db1::detail

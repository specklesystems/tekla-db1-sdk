#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdio>
#include <span>
#include <unordered_map>
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
  using namespace tekla::db1;
  using namespace tekla::db1::detail;
  std::unordered_map<std::uint64_t, std::size_t> evaluation_counts;
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
    const auto object_id = decoded.value().object_id;
    const auto evaluation_count = evaluation_counts[object_id]++;
    const bool missing_semantic_node =
        decoded.value().nodes.empty() ||
        std::any_of(decoded.value().nodes.begin(), decoded.value().nodes.end(),
                    [](const OcctShapeNode& node) {
                      return node.base_extrusion.loops.empty() &&
                             node.base_ruled_sweep.loops.empty();
                    });
    bool has_duplicate_cutter = false;
    for (std::size_t first = 0; first < decoded.value().subtract_meshes.size(); ++first) {
      for (std::size_t second = first + 1U; second < decoded.value().subtract_meshes.size();
           ++second) {
        const auto& left = decoded.value().subtract_meshes[first];
        const auto& right = decoded.value().subtract_meshes[second];
        has_duplicate_cutter = has_duplicate_cutter ||
                               (left.positions == right.positions && left.indices == right.indices);
      }
    }
    Result<OcctMesh> evaluation =
        missing_semantic_node
            ? Result<OcctMesh>::failure({ErrorCode::internal_error,
                                         "A supported CSG node lost its semantic geometry recipe."})
        : has_duplicate_cutter
            ? Result<OcctMesh>::failure(
                  {ErrorCode::internal_error, "A topology request retained a duplicate cutter."})
        : evaluation_count != 0U && object_id != 1207U && object_id != 1208U && object_id != 1209U
            ? Result<OcctMesh>::failure({ErrorCode::internal_error,
                                         "The same topology object was evaluated more than once."})
        : object_id == 1205U
            ? Result<OcctMesh>::failure(
                  {ErrorCode::invalid_topology, "Synthetic deterministic topology failure."})
        : object_id == 1209U && evaluation_count == 0U
            ? Result<OcctMesh>::failure(
                  {ErrorCode::geometry_timeout, "Synthetic transient topology failure."})
            : evaluate_occt(decoded.value());
    auto response = encode_occt_response(evaluation);
    if (!response || !write_exact(response.value())) return 1;
  }
}

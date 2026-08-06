#include <array>
#include <bit>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <limits>
#include <optional>
#include <string_view>
#include <tekla/db1/tekla_db1.hpp>

namespace {

struct RunResult {
  double process_seconds = 0.0;
  double drain_seconds = 0.0;
  std::uint64_t mesh_count = 0;
  std::uint64_t vertex_count = 0;
  std::uint64_t triangle_count = 0;
  std::uint64_t diagnostic_count = 0;
  std::array<double, 6> bounds{
      std::numeric_limits<double>::infinity(),  std::numeric_limits<double>::infinity(),
      std::numeric_limits<double>::infinity(),  -std::numeric_limits<double>::infinity(),
      -std::numeric_limits<double>::infinity(), -std::numeric_limits<double>::infinity()};
  double volume = 0.0;
  std::uint64_t geometry_hash = 0xcbf29ce484222325ULL;
};

bool parse_u64(std::string_view text, std::uint64_t& value) {
  const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
  return parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size();
}

tekla::db1::Result<RunResult> run(const tekla::db1::Model& model, tekla::db1::TopologyMode mode,
                                  std::string_view worker, std::uint64_t object_id) {
  tekla::db1::ProcessRequest request;
  request.stages = tekla::db1::Stage::display_geometry;
  request.topology_mode = mode;
  request.topology_worker_path = worker;
  request.topology_timeout_milliseconds = 600'000;
  request.geometry_object_id_min = object_id;
  request.geometry_object_id_max = object_id;

  const auto process_started = std::chrono::steady_clock::now();
  auto stream = model.process(request);
  const auto process_elapsed = std::chrono::steady_clock::now() - process_started;
  if (!stream) return tekla::db1::Result<RunResult>::failure(stream.error());

  RunResult result;
  result.process_seconds = std::chrono::duration<double>(process_elapsed).count();
  const auto drain_started = std::chrono::steady_clock::now();
  for (;;) {
    auto batch = stream.value()->next();
    if (!batch) return tekla::db1::Result<RunResult>::failure(batch.error());
    if (batch.value().kind == tekla::db1::BatchKind::end) break;
    if (batch.value().kind == tekla::db1::BatchKind::meshes) {
      result.mesh_count += batch.value().meshes.size();
      for (const auto& mesh : batch.value().meshes) {
        result.vertex_count += mesh.positions.size() / 3U;
        result.triangle_count += mesh.indices.size() / 3U;
        const auto mix = [&](std::uint64_t value) {
          result.geometry_hash ^= value;
          result.geometry_hash *= 0x100000001b3ULL;
        };
        for (std::size_t index = 0; index + 2U < mesh.positions.size(); index += 3U) {
          for (std::size_t axis = 0; axis < 3U; ++axis) {
            const auto value = static_cast<double>(mesh.positions[index + axis]);
            result.bounds[axis] = std::min(result.bounds[axis], value);
            result.bounds[axis + 3U] = std::max(result.bounds[axis + 3U], value);
            mix(std::bit_cast<std::uint32_t>(mesh.positions[index + axis]));
          }
        }
        double signed_volume = 0.0;
        const auto point = [&](std::uint32_t vertex, std::size_t axis) {
          return static_cast<double>(mesh.positions[static_cast<std::size_t>(vertex) * 3U + axis]);
        };
        for (std::size_t index = 0; index + 2U < mesh.indices.size(); index += 3U) {
          const auto a = mesh.indices[index];
          const auto b = mesh.indices[index + 1U];
          const auto c = mesh.indices[index + 2U];
          signed_volume +=
              point(a, 0U) * (point(b, 1U) * point(c, 2U) - point(b, 2U) * point(c, 1U)) -
              point(a, 1U) * (point(b, 0U) * point(c, 2U) - point(b, 2U) * point(c, 0U)) +
              point(a, 2U) * (point(b, 0U) * point(c, 1U) - point(b, 1U) * point(c, 0U));
          mix(a);
          mix(b);
          mix(c);
        }
        result.volume += std::abs(signed_volume / 6.0);
      }
    } else if (batch.value().kind == tekla::db1::BatchKind::diagnostics) {
      result.diagnostic_count += batch.value().diagnostics.size();
    }
  }
  result.drain_seconds =
      std::chrono::duration<double>(std::chrono::steady_clock::now() - drain_started).count();
  return tekla::db1::Result<RunResult>::success(result);
}

void print_result(std::string_view mode, const RunResult& result) {
  std::printf(
      "{\"mode\":\"%.*s\",\"process_seconds\":%.9f,\"drain_seconds\":%.9f,"
      "\"mesh_count\":%llu,\"vertex_count\":%llu,\"triangle_count\":%llu,"
      "\"diagnostic_count\":%llu,\"bounds\":[%.9f,%.9f,%.9f,%.9f,%.9f,%.9f],"
      "\"volume\":%.9f,\"geometry_hash\":\"%016llx\"}\n",
      static_cast<int>(mode.size()), mode.data(), result.process_seconds, result.drain_seconds,
      static_cast<unsigned long long>(result.mesh_count),
      static_cast<unsigned long long>(result.vertex_count),
      static_cast<unsigned long long>(result.triangle_count),
      static_cast<unsigned long long>(result.diagnostic_count), result.bounds[0], result.bounds[1],
      result.bounds[2], result.bounds[3], result.bounds[4], result.bounds[5], result.volume,
      static_cast<unsigned long long>(result.geometry_hash));
}

bool equivalent_geometry(const RunResult& left, const RunResult& right) {
  return left.mesh_count == right.mesh_count && left.vertex_count == right.vertex_count &&
         left.triangle_count == right.triangle_count &&
         left.diagnostic_count == right.diagnostic_count && left.bounds == right.bounds &&
         left.geometry_hash == right.geometry_hash;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 3 && argc != 4) {
    std::fprintf(stderr,
                 "usage: tekla-db1-topology-object-benchmark <model.db1> <object-id> "
                 "[worker]\n");
    return 2;
  }
  std::uint64_t object_id = 0;
  if (!parse_u64(argv[2], object_id)) {
    std::fprintf(stderr, "object id must be an unsigned integer\n");
    return 2;
  }
  auto package = tekla::db1::map_model_package(std::filesystem::path(argv[1]));
  if (!package) {
    std::fprintf(stderr, "%s\n", package.error().message.c_str());
    return 1;
  }
  auto model = tekla::db1::open(std::move(package.value()));
  if (!model) {
    std::fprintf(stderr, "%s\n", model.error().message.c_str());
    return 1;
  }

  auto direct = run(model.value(), tekla::db1::TopologyMode::direct, {}, object_id);
  if (!direct) {
    std::fprintf(stderr, "direct: %s\n", direct.error().message.c_str());
    return 1;
  }
  print_result("direct", direct.value());

  if (argc == 4) {
    auto supervised = run(model.value(), tekla::db1::TopologyMode::supervised, argv[3], object_id);
    if (!supervised) {
      std::fprintf(stderr, "supervised: %s\n", supervised.error().message.c_str());
      return 1;
    }
    print_result("supervised", supervised.value());
    if (!equivalent_geometry(direct.value(), supervised.value())) {
      std::fprintf(stderr, "direct and supervised geometry differ\n");
      return 1;
    }
  }
  return 0;
}

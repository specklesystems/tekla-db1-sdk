#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <tekla/db1/tekla_db1.hpp>
#include <unordered_map>

namespace {

struct ObjectInfo {
  std::string application_id;
  std::string name;
  std::string profile;
};

[[nodiscard]] const char* geometry_kind_name(tekla::db1::DefinitionGeometryKind kind) noexcept {
  using tekla::db1::DefinitionGeometryKind;
  switch (kind) {
    case DefinitionGeometryKind::straight_extrusion:
      return "straight_extrusion";
    case DefinitionGeometryKind::circular_arc_extrusion:
      return "circular_arc_extrusion";
    case DefinitionGeometryKind::polyline_extrusion:
      return "polyline_extrusion";
    case DefinitionGeometryKind::lofted_plate:
      return "lofted_plate";
    case DefinitionGeometryKind::imported_shape:
      return "imported_shape";
  }
  return "unknown";
}

[[nodiscard]] const char* curve_kind_name(tekla::db1::CurveGeometryKind kind) noexcept {
  using tekla::db1::CurveGeometryKind;
  switch (kind) {
    case CurveGeometryKind::polyline:
      return "polyline";
    case CurveGeometryKind::line_segment:
      return "line_segment";
  }
  return "unknown";
}

void print_error(const tekla::db1::Error& error) {
  std::fprintf(stderr, "tekla-db1-inventory: %s\n", error.message.c_str());
}

void print_json_string(std::string_view value) {
  std::putchar('"');
  for (const char raw_character : value) {
    const auto character = static_cast<unsigned char>(raw_character);
    switch (character) {
      case '"':
        std::fputs("\\\"", stdout);
        break;
      case '\\':
        std::fputs("\\\\", stdout);
        break;
      case '\b':
        std::fputs("\\b", stdout);
        break;
      case '\f':
        std::fputs("\\f", stdout);
        break;
      case '\n':
        std::fputs("\\n", stdout);
        break;
      case '\r':
        std::fputs("\\r", stdout);
        break;
      case '\t':
        std::fputs("\\t", stdout);
        break;
      default:
        if (character < 0x20U || character >= 0x80U) {
          std::printf("\\u%04x", static_cast<unsigned>(character));
        } else {
          std::putchar(static_cast<int>(character));
        }
    }
  }
  std::putchar('"');
}

struct MeshMetrics {
  double volume = 0.0;
  double surface_area = 0.0;
};

[[nodiscard]] MeshMetrics mesh_metrics(const tekla::db1::MeshView& mesh) {
  MeshMetrics result;
  if (mesh.positions.size() < 3U) return result;
  const std::array<double, 3> reference{mesh.positions[0], mesh.positions[1], mesh.positions[2]};
  for (std::size_t index = 0; index + 2U < mesh.indices.size(); index += 3U) {
    std::array<std::array<double, 3>, 3> vertices{};
    bool valid = true;
    for (std::size_t corner = 0; corner < 3U; ++corner) {
      const std::size_t offset = static_cast<std::size_t>(mesh.indices[index + corner]) * 3U;
      if (offset + 2U >= mesh.positions.size()) {
        valid = false;
        break;
      }
      for (std::size_t axis = 0; axis < 3U; ++axis) {
        vertices[corner][axis] = mesh.positions[offset + axis] - reference[axis];
      }
    }
    if (!valid) continue;
    const std::array<double, 3> first{vertices[1][0] - vertices[0][0],
                                      vertices[1][1] - vertices[0][1],
                                      vertices[1][2] - vertices[0][2]};
    const std::array<double, 3> second{vertices[2][0] - vertices[0][0],
                                       vertices[2][1] - vertices[0][1],
                                       vertices[2][2] - vertices[0][2]};
    const std::array<double, 3> cross{first[1] * second[2] - first[2] * second[1],
                                      first[2] * second[0] - first[0] * second[2],
                                      first[0] * second[1] - first[1] * second[0]};
    result.surface_area += 0.5 * std::hypot(cross[0], cross[1], cross[2]);
    result.volume +=
        (vertices[0][0] * (vertices[1][1] * vertices[2][2] - vertices[1][2] * vertices[2][1]) -
         vertices[0][1] * (vertices[1][0] * vertices[2][2] - vertices[1][2] * vertices[2][0]) +
         vertices[0][2] * (vertices[1][0] * vertices[2][1] - vertices[1][1] * vertices[2][0])) /
        6.0;
  }
  result.volume = std::abs(result.volume);
  return result;
}

}  // namespace

int main(int argc, char** argv) {
  // Inventory output is commonly redirected during long corpus audits. Keep
  // every completed object visible so an interrupted run remains actionable.
  std::setvbuf(stdout, nullptr, _IOLBF, 0);
  if (argc < 2) {
    std::fprintf(stderr,
                 "usage: tekla-db1-inventory <model.db1> "
                 "[--topology-mode disabled|direct|supervised] "
                 "[--topology-worker <path>] [--topology-timeout-ms <ms>] "
                 "[--object-id-min <id>] "
                 "[--object-id-max <id>]\n");
    return 2;
  }
  auto topology_mode = tekla::db1::TopologyMode::disabled;
  std::optional<std::string> topology_worker;
  std::uint32_t topology_timeout_milliseconds = 10'000;
  std::uint64_t object_id_min = 0;
  std::uint64_t object_id_max = std::numeric_limits<std::uint64_t>::max();
  for (int index = 2; index < argc; index += 2) {
    if (index + 1 >= argc) {
      std::fprintf(stderr, "tekla-db1-inventory: option value missing\n");
      return 2;
    }
    const std::string_view option(argv[index]);
    if (option == "--topology-mode") {
      const std::string_view value(argv[index + 1]);
      if (value == "disabled") {
        topology_mode = tekla::db1::TopologyMode::disabled;
      } else if (value == "direct") {
        topology_mode = tekla::db1::TopologyMode::direct;
      } else if (value == "supervised") {
        topology_mode = tekla::db1::TopologyMode::supervised;
      } else {
        std::fprintf(stderr, "tekla-db1-inventory: invalid topology mode\n");
        return 2;
      }
      continue;
    }
    if (option == "--topology-worker") {
      topology_worker = argv[index + 1];
      continue;
    }
    char* end = nullptr;
    const auto value = std::strtoull(argv[index + 1], &end, 10);
    if (end == argv[index + 1] || *end != '\0') {
      std::fprintf(stderr, "tekla-db1-inventory: invalid object id\n");
      return 2;
    }
    if (option == "--topology-timeout-ms") {
      if (value > std::numeric_limits<std::uint32_t>::max()) {
        std::fprintf(stderr, "tekla-db1-inventory: topology timeout is too large\n");
        return 2;
      }
      topology_timeout_milliseconds = static_cast<std::uint32_t>(value);
    } else if (option == "--object-id-min") {
      object_id_min = value;
    } else if (option == "--object-id-max") {
      object_id_max = value;
    } else {
      std::fprintf(stderr, "tekla-db1-inventory: unknown option\n");
      return 2;
    }
  }
  auto package = tekla::db1::map_model_package(std::filesystem::path(argv[1]));
  if (!package) {
    print_error(package.error());
    return 1;
  }
  auto model = tekla::db1::open(std::move(package.value()));
  if (!model) {
    print_error(model.error());
    return 1;
  }
  std::unordered_map<std::uint64_t, ObjectInfo> objects;
  tekla::db1::ProcessRequest semantic_request;
  semantic_request.stages = tekla::db1::Stage::identities | tekla::db1::Stage::properties;
  auto semantics = model.value().process(semantic_request);
  if (!semantics) {
    print_error(semantics.error());
    return 1;
  }
  while (true) {
    auto batch = semantics.value()->next();
    if (!batch) {
      print_error(batch.error());
      return 1;
    }
    if (batch.value().kind == tekla::db1::BatchKind::end) break;
    if (batch.value().kind == tekla::db1::BatchKind::objects) {
      for (const auto& object : batch.value().objects) {
        objects[object.internal_id].application_id = object.application_id;
      }
    } else if (batch.value().kind == tekla::db1::BatchKind::properties) {
      for (const auto& property : batch.value().properties) {
        if (property.kind != tekla::db1::PropertyValueKind::text) continue;
        if (property.name == "name") objects[property.object_id].name = property.text_value;
        if (property.name == "profile") {
          objects[property.object_id].profile = property.text_value;
        }
      }
    }
  }

  std::unordered_map<std::uint64_t, std::string> definition_profiles;
  std::unordered_map<std::uint64_t, tekla::db1::DefinitionGeometryKind> definition_kinds;
  tekla::db1::ProcessRequest geometry_request;
  geometry_request.stages =
      tekla::db1::Stage::definition_geometry | tekla::db1::Stage::display_geometry;
  if (topology_worker && topology_mode == tekla::db1::TopologyMode::disabled) {
    topology_mode = tekla::db1::TopologyMode::supervised;
  }
  geometry_request.topology_mode = topology_mode;
  geometry_request.topology_timeout_milliseconds = topology_timeout_milliseconds;
  if (topology_worker) {
    geometry_request.topology_worker_path = *topology_worker;
  }
  geometry_request.geometry_object_id_min = object_id_min;
  geometry_request.geometry_object_id_max = object_id_max;
  auto geometry = model.value().process(geometry_request);
  if (!geometry) {
    print_error(geometry.error());
    return 1;
  }
  while (true) {
    auto batch = geometry.value()->next();
    if (!batch) {
      print_error(batch.error());
      return 1;
    }
    if (batch.value().kind == tekla::db1::BatchKind::end) break;
    if (batch.value().kind == tekla::db1::BatchKind::definition_geometry) {
      for (const auto& definition : batch.value().definitions) {
        definition_profiles.insert_or_assign(definition.object_id, std::string(definition.profile));
        definition_kinds.insert_or_assign(definition.object_id, definition.kind);
      }
    } else if (batch.value().kind == tekla::db1::BatchKind::meshes) {
      for (const auto& mesh : batch.value().meshes) {
        std::array<double, 3> minimum{std::numeric_limits<double>::infinity(),
                                      std::numeric_limits<double>::infinity(),
                                      std::numeric_limits<double>::infinity()};
        std::array<double, 3> maximum{-std::numeric_limits<double>::infinity(),
                                      -std::numeric_limits<double>::infinity(),
                                      -std::numeric_limits<double>::infinity()};
        for (std::size_t index = 0; index + 2U < mesh.positions.size(); index += 3U) {
          for (std::size_t axis = 0; axis < 3U; ++axis) {
            minimum[axis] =
                std::min(minimum[axis], static_cast<double>(mesh.positions[index + axis]));
            maximum[axis] =
                std::max(maximum[axis], static_cast<double>(mesh.positions[index + axis]));
          }
        }
        const auto found = objects.find(mesh.object_id);
        const ObjectInfo empty;
        const auto& object = found == objects.end() ? empty : found->second;
        std::fputs("{\"kind\":\"mesh\",\"object_id\":", stdout);
        std::printf("%llu,\"application_id\":", static_cast<unsigned long long>(mesh.object_id));
        print_json_string(object.application_id);
        std::fputs(",\"name\":", stdout);
        print_json_string(object.name);
        std::fputs(",\"profile\":", stdout);
        const auto profile = definition_profiles.find(mesh.object_id);
        print_json_string(profile == definition_profiles.end() ? object.profile : profile->second);
        std::fputs(",\"definition_kind\":", stdout);
        const auto geometry_kind = definition_kinds.find(mesh.object_id);
        const auto metrics = mesh_metrics(mesh);
        print_json_string(geometry_kind == definition_kinds.end()
                              ? "unknown"
                              : geometry_kind_name(geometry_kind->second));
        std::printf(
            ",\"vertex_count\":%zu,\"triangle_count\":%zu,"
            "\"volume\":%.17g,\"surface_area\":%.17g,"
            "\"bounds\":[[%.17g,%.17g,%.17g],[%.17g,%.17g,%.17g]]}\n",
            mesh.positions.size() / 3U, mesh.indices.size() / 3U, metrics.volume,
            metrics.surface_area, minimum[0], minimum[1], minimum[2], maximum[0], maximum[1],
            maximum[2]);
      }
    } else if (batch.value().kind == tekla::db1::BatchKind::curves) {
      for (const auto& curve : batch.value().curves) {
        std::array<double, 3> minimum{std::numeric_limits<double>::infinity(),
                                      std::numeric_limits<double>::infinity(),
                                      std::numeric_limits<double>::infinity()};
        std::array<double, 3> maximum{-std::numeric_limits<double>::infinity(),
                                      -std::numeric_limits<double>::infinity(),
                                      -std::numeric_limits<double>::infinity()};
        double length = 0.0;
        for (std::size_t index = 0; index < curve.points.size(); ++index) {
          const auto& point = curve.points[index];
          minimum[0] = std::min(minimum[0], point.x);
          minimum[1] = std::min(minimum[1], point.y);
          minimum[2] = std::min(minimum[2], point.z);
          maximum[0] = std::max(maximum[0], point.x);
          maximum[1] = std::max(maximum[1], point.y);
          maximum[2] = std::max(maximum[2], point.z);
          if (index != 0U) {
            const auto& previous = curve.points[index - 1U];
            length += std::hypot(point.x - previous.x, point.y - previous.y, point.z - previous.z);
          }
        }
        const auto found = objects.find(curve.object_id);
        const ObjectInfo empty;
        const auto& object = found == objects.end() ? empty : found->second;
        std::fputs("{\"kind\":\"curve\",\"object_id\":", stdout);
        std::printf("%llu,\"application_id\":", static_cast<unsigned long long>(curve.object_id));
        print_json_string(object.application_id);
        std::fputs(",\"name\":", stdout);
        print_json_string(object.name);
        std::fputs(",\"profile\":", stdout);
        const auto profile = definition_profiles.find(curve.object_id);
        print_json_string(profile == definition_profiles.end() ? object.profile : profile->second);
        std::fputs(",\"curve_kind\":", stdout);
        print_json_string(curve_kind_name(curve.kind));
        std::printf(",\"point_count\":%zu,\"radius\":%.17g,\"length\":%.17g", curve.points.size(),
                    curve.radius, length);
        if (curve.points.empty()) {
          std::fputs(",\"bounds\":null}\n", stdout);
        } else {
          std::printf(",\"bounds\":[[%.17g,%.17g,%.17g],[%.17g,%.17g,%.17g]]}\n", minimum[0],
                      minimum[1], minimum[2], maximum[0], maximum[1], maximum[2]);
        }
      }
    } else if (batch.value().kind == tekla::db1::BatchKind::diagnostics) {
      for (const auto& diagnostic : batch.value().diagnostics) {
        const auto found = objects.find(diagnostic.object_id);
        const ObjectInfo empty;
        const auto& object = found == objects.end() ? empty : found->second;
        std::fputs("{\"kind\":\"diagnostic\",\"object_id\":", stdout);
        std::printf("%llu,\"application_id\":",
                    static_cast<unsigned long long>(diagnostic.object_id));
        print_json_string(object.application_id);
        std::fputs(",\"name\":", stdout);
        print_json_string(object.name);
        std::fputs(",\"profile\":", stdout);
        const auto profile = definition_profiles.find(diagnostic.object_id);
        print_json_string(profile == definition_profiles.end() ? object.profile : profile->second);
        std::printf(",\"error_code\":%u,\"message\":", static_cast<unsigned>(diagnostic.code));
        print_json_string(diagnostic.message);
        std::fputs("}\n", stdout);
      }
    }
  }
  return 0;
}

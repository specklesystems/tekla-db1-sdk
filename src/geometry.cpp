#include "geometry.hpp"

#include "geometry_recipe.hpp"
#include "profile.hpp"
#include "record.hpp"
#include "shape.hpp"
#include "standard_profile.hpp"
#if defined(TEKLA_DB1_HAS_OCCT)
#include "occt/occt.hpp"
#include "occt/protocol.hpp"
#endif

#include <algorithm>
#include <array>
#include <bit>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <limits>
#include <numbers>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace tekla::db1::detail {
namespace {

struct Attribute {
  std::string profile;
  std::string object_class;
  std::uint32_t object_type = 0;
  std::uint32_t form_type = 0;
};

struct LegacyArc {
  double radius = 0.0;
  std::uint32_t segment_count = 0;
};

struct Axes {
  Vector3d x;
  Vector3d y;
};

struct CoordinateSystem {
  std::uint32_t axes_id = 0;
  Vector3d origin;
  double length = 0.0;
};

struct Chamfer {
  std::uint32_t type = 0;
  double x = 0.0;
  double y = 0.0;
  std::uint32_t end_types = 0;
  double first_end_dimension = 0.0;
  double second_end_dimension = 0.0;
};

struct GeometryOperation {
  std::uint32_t type = 0;
  std::uint32_t target_id = 0;
  std::uint32_t relation_id = 0;
  std::uint32_t event_id = 0;
};

struct MeshData {
  std::uint64_t object_id = 0;
  // Keep analytic and topology inputs in model precision. Public MeshView
  // conversion happens only after every persisted operation has been replayed.
  std::vector<double> positions;
  std::vector<std::uint32_t> indices;
  std::optional<RuledSweepRecipe> ruled_sweep_recipe;
  Vector3d longitudinal_axis{1.0, 0.0, 0.0};
  Vector3d section_origin;
  Vector3d section_y_axis;
  Vector3d section_z_axis;
  bool has_section_frame = false;
  double circular_inner_radius = 0.0;
  double circular_inner_apothem = 0.0;
  double exact_surface_area = 0.0;
  double exact_volume = 0.0;
  bool has_exact_topology_metrics = false;
};

struct ReportMetrics {
  double surface_area = 0.0;
  double cover_surface_area = 0.0;
  double volume = 0.0;
  double longitudinal_min = 0.0;
  double longitudinal_max = 0.0;
  double section_y_min = 0.0;
  double section_y_max = 0.0;
  double section_z_min = 0.0;
  double section_z_max = 0.0;
  bool valid = false;
  bool has_cover_surface_area = false;
  bool has_section_extents = false;
};

struct Section {
  enum class Kind { solid, hollow } kind = Kind::solid;
  std::vector<std::array<double, 2>> outer;
  std::vector<std::array<double, 2>> inner;
  std::vector<std::array<std::uint32_t, 3>> cap;
  // Positive when the outer boundary is an analytic circle. The display
  // contour remains faceted, but fitting/report extents use the exact support
  // function of the circle rather than the tessellation vertices.
  double circular_outer_radius = 0.0;
  double circular_inner_radius = 0.0;
};

struct TaperedSection {
  std::vector<std::array<double, 2>> start;
  std::vector<std::array<double, 2>> end;
};

struct TaperedIParameters {
  double start_height = 0.0;
  double end_height = 0.0;
  double web = 0.0;
  double bottom_flange = 0.0;
  double bottom_width = 0.0;
  double top_flange = 0.0;
  double top_width = 0.0;
};

struct LoftRails {
  std::array<std::vector<Vector3d>, 2> rails;
};

struct GeometryNode {
  std::uint32_t id = 0;
  std::uint32_t parent_id = 0;
  std::uint32_t type = 0;
  std::uint32_t subtype = 0;
  std::uint32_t geometry_id = 0;
  std::uint32_t coordinate_system_id = 0;
};

struct DoubleArrayRecord {
  std::uint32_t next_id = 0;
  std::uint32_t value_count = 0;
  std::array<double, 12> values{};
};

struct PolygonChunk {
  std::uint32_t number = 0;
  std::vector<Vector3d> points;
  std::vector<double> dx;
  std::vector<double> dy;
  std::vector<std::uint32_t> types;
};

struct Contour {
  std::vector<Vector3d> points;
  std::vector<double> dx;
  std::vector<double> dy;
  std::vector<std::uint32_t> types;
};

[[nodiscard]] constexpr bool contains(Stage stages, Stage stage) noexcept {
  return (static_cast<std::uint32_t>(stages) & static_cast<std::uint32_t>(stage)) != 0;
}

[[nodiscard]] double length(Vector3d value) noexcept {
  return std::sqrt(value.x * value.x + value.y * value.y + value.z * value.z);
}

[[nodiscard]] std::optional<Vector3d> normalized(Vector3d value) noexcept {
  const double magnitude = length(value);
  if (!std::isfinite(magnitude) || magnitude <= 1e-12) return std::nullopt;
  return Vector3d{value.x / magnitude, value.y / magnitude, value.z / magnitude};
}

[[nodiscard]] Vector3d cross(Vector3d lhs, Vector3d rhs) noexcept {
  return {lhs.y * rhs.z - lhs.z * rhs.y, lhs.z * rhs.x - lhs.x * rhs.z,
          lhs.x * rhs.y - lhs.y * rhs.x};
}

[[nodiscard]] double dot(Vector3d lhs, Vector3d rhs) noexcept {
  return lhs.x * rhs.x + lhs.y * rhs.y + lhs.z * rhs.z;
}

[[nodiscard]] Vector3d add(Vector3d lhs, Vector3d rhs) noexcept {
  return {lhs.x + rhs.x, lhs.y + rhs.y, lhs.z + rhs.z};
}

[[nodiscard]] Vector3d subtract(Vector3d lhs, Vector3d rhs) noexcept {
  return {lhs.x - rhs.x, lhs.y - rhs.y, lhs.z - rhs.z};
}

[[nodiscard]] Vector3d scale(Vector3d value, double factor) noexcept {
  return {value.x * factor, value.y * factor, value.z * factor};
}

[[nodiscard]] Vector3d point(Vector3d origin, Vector3d x_axis, Vector3d y_axis, Vector3d z_axis,
                             double x, double y, double z) noexcept {
  return {origin.x + x_axis.x * x + y_axis.x * y + z_axis.x * z,
          origin.y + x_axis.y * x + y_axis.y * y + z_axis.y * z,
          origin.z + x_axis.z * x + y_axis.z * y + z_axis.z * z};
}

[[nodiscard]] ReportMetrics report_metrics(const MeshData& mesh, Vector3d longitudinal_axis) {
  ReportMetrics result;
  if (mesh.positions.size() < 9U || mesh.positions.size() % 3U != 0U || mesh.indices.size() < 3U ||
      mesh.indices.size() % 3U != 0U) {
    return result;
  }
  const auto axis = normalized(longitudinal_axis);
  if (!axis) return result;
  const std::size_t vertex_count = mesh.positions.size() / 3U;
  const auto vertex = [&](std::uint32_t index) -> std::optional<Vector3d> {
    if (index >= vertex_count) return std::nullopt;
    const std::size_t offset = static_cast<std::size_t>(index) * 3U;
    return Vector3d{mesh.positions[offset], mesh.positions[offset + 1U],
                    mesh.positions[offset + 2U]};
  };
  const Vector3d reference{mesh.positions[0], mesh.positions[1], mesh.positions[2]};
  result.longitudinal_min = std::numeric_limits<double>::infinity();
  result.longitudinal_max = -std::numeric_limits<double>::infinity();
  result.section_y_min = std::numeric_limits<double>::infinity();
  result.section_y_max = -std::numeric_limits<double>::infinity();
  result.section_z_min = std::numeric_limits<double>::infinity();
  result.section_z_max = -std::numeric_limits<double>::infinity();
  const auto transverse_y =
      mesh.has_section_frame ? normalized(cross(mesh.section_z_axis, *axis)) : std::nullopt;
  for (std::size_t offset = 0; offset < mesh.positions.size(); offset += 3U) {
    const Vector3d current{mesh.positions[offset], mesh.positions[offset + 1U],
                           mesh.positions[offset + 2U]};
    const double projection = dot(subtract(current, reference), *axis);
    result.longitudinal_min = std::min(result.longitudinal_min, projection);
    result.longitudinal_max = std::max(result.longitudinal_max, projection);
    if (transverse_y) {
      const auto local = subtract(current, mesh.section_origin);
      const double section_y = dot(local, *transverse_y);
      const double section_z = dot(local, mesh.section_z_axis);
      result.section_y_min = std::min(result.section_y_min, section_y);
      result.section_y_max = std::max(result.section_y_max, section_y);
      result.section_z_min = std::min(result.section_z_min, section_z);
      result.section_z_max = std::max(result.section_z_max, section_z);
      result.has_section_extents = true;
    }
  }
  double signed_volume = 0.0;
  for (std::size_t index = 0; index < mesh.indices.size(); index += 3U) {
    const auto a = vertex(mesh.indices[index]);
    const auto b = vertex(mesh.indices[index + 1U]);
    const auto c = vertex(mesh.indices[index + 2U]);
    if (!a || !b || !c) return ReportMetrics{};
    const auto ab = subtract(*b, *a);
    const auto ac = subtract(*c, *a);
    const auto area_vector = cross(ab, ac);
    const double triangle_area = 0.5 * length(area_vector);
    result.surface_area += triangle_area;
    bool internal_void_wall = false;
    if (mesh.circular_inner_radius > 0.0 && mesh.circular_inner_apothem > 0.0) {
      const auto normal = normalized(area_vector);
      const auto void_transverse_y = normalized(cross(mesh.section_z_axis, *axis));
      const auto radial = [&](Vector3d value) {
        const auto relative = subtract(value, mesh.section_origin);
        return std::hypot(dot(relative, void_transverse_y.value_or(mesh.section_y_axis)),
                          dot(relative, mesh.section_z_axis));
      };
      // OCCT re-triangulates planar cylinder facets after Boolean clipping.
      // Their normals remain transverse, but large model coordinates can
      // leave roughly 1e-7 of longitudinal noise after subtraction.
      constexpr double angular_tolerance = 1.0e-5;
      // The stored local frame is double precision, but legacy component
      // placements can carry large global translations and rounded basis
      // vectors. A millimetre-scale model only needs micrometre radial
      // discrimination here; the outer wall remains several millimetres away.
      const double radial_tolerance = std::max(1.0e-3, mesh.circular_inner_radius * 1.0e-6);
      const auto on_inner_band = [&](Vector3d value) {
        const double radius = radial(value);
        return radius >= mesh.circular_inner_apothem - radial_tolerance &&
               radius <= mesh.circular_inner_radius + radial_tolerance;
      };
      const bool all_on_inner_band = on_inner_band(*a) && on_inner_band(*b) && on_inner_band(*c);
      internal_void_wall =
          normal && std::abs(dot(*normal, *axis)) <= angular_tolerance && all_on_inner_band;
      result.has_cover_surface_area = true;
    }
    if (!internal_void_wall) result.cover_surface_area += triangle_area;
    const auto translated_a = subtract(*a, reference);
    const auto translated_b = subtract(*b, reference);
    const auto translated_c = subtract(*c, reference);
    signed_volume += dot(translated_a, cross(translated_b, translated_c)) / 6.0;
  }
  result.volume = std::abs(signed_volume);
  if (mesh.has_exact_topology_metrics) {
    result.surface_area = mesh.exact_surface_area;
    result.volume = mesh.exact_volume;
  }
  result.valid = std::isfinite(result.surface_area) && std::isfinite(result.volume) &&
                 std::isfinite(result.longitudinal_min) && std::isfinite(result.longitudinal_max);
  return result;
}

[[nodiscard]] MeshData instantiate_shape(std::uint64_t object_id, const ShapeMesh& shape,
                                         const DefinitionGeometryView& definition) {
  MeshData mesh{.object_id = object_id};
  mesh.positions.reserve(shape.vertices.size() * 3U);
  for (const auto local : shape.vertices) {
    const auto world = point(definition.origin, definition.x_axis, definition.y_axis,
                             definition.z_axis, local.x, local.y, local.z);
    mesh.positions.insert(mesh.positions.end(), {world.x, world.y, world.z});
  }
  mesh.indices = shape.indices;
  return mesh;
}

[[nodiscard]] std::optional<double> number(std::string_view value) noexcept {
  double result = 0.0;
  const auto parsed = std::from_chars(value.data(), value.data() + value.size(), result);
  if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() ||
      !std::isfinite(result) || result <= 0.0) {
    return std::nullopt;
  }
  return result;
}

[[nodiscard]] std::optional<double> plate_thickness(std::string_view profile) noexcept {
  if (profile.starts_with("PLT"))
    profile.remove_prefix(3U);
  else if (profile.starts_with("PL") || profile.starts_with("BL") || profile.starts_with("FL"))
    profile.remove_prefix(2U);
  return number(profile);
}

[[nodiscard]] std::optional<std::uint32_t> unsigned_number(std::string_view value) noexcept {
  std::uint32_t result = 0;
  const auto parsed = std::from_chars(value.data(), value.data() + value.size(), result);
  if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size()) {
    return std::nullopt;
  }
  return result;
}

[[nodiscard]] std::optional<LegacyArc> legacy_arc(std::string_view value) {
  std::array<std::string_view, 3> tokens{};
  std::size_t token_count = 0;
  for (std::size_t begin = 0; begin < value.size();) {
    while (begin < value.size() && std::isspace(static_cast<unsigned char>(value[begin])) != 0)
      ++begin;
    if (begin == value.size()) break;
    const auto end = value.find_first_of(" \t\r\n", begin);
    if (token_count == tokens.size()) return std::nullopt;
    tokens[token_count++] =
        value.substr(begin, end == std::string_view::npos ? value.size() - begin : end - begin);
    begin = end == std::string_view::npos ? value.size() : end;
  }
  if (token_count != 3 || !unsigned_number(tokens[0]) || tokens[1].size() < 2 ||
      tokens[1].size() > 4 || tokens[1].back() != '0')
    return std::nullopt;
  const auto segments = unsigned_number(tokens[1].substr(0, tokens[1].size() - 1));
  const auto radius = number(tokens[2]);
  if (!segments || *segments < 2 || !radius) return std::nullopt;
  return LegacyArc{*radius, *segments};
}

[[nodiscard]] std::vector<double> dimensions(std::string_view value,
                                             std::string_view separators = "*Xx") {
  std::vector<double> result;
  std::size_t begin = 0;
  while (begin < value.size()) {
    const auto end = value.find_first_of(separators, begin);
    const auto token =
        value.substr(begin, end == std::string_view::npos ? value.size() - begin : end - begin);
    auto parsed = number(token);
    if (!parsed) return {};
    result.push_back(*parsed);
    if (end == std::string_view::npos) break;
    begin = end + 1;
  }
  return result;
}

[[nodiscard]] bool centered_square_profile(std::string_view profile) {
  const auto values = dimensions(profile);
  return values.size() == 2U &&
         std::abs(values[0] - values[1]) <= std::max(values[0], values[1]) * 1.0e-12;
}

[[nodiscard]] std::optional<Vector3d> fallback_square_y_axis(Vector3d x_axis) noexcept {
  const auto x = normalized(x_axis);
  if (!x) return std::nullopt;
  constexpr std::array<Vector3d, 3> references{Vector3d{0.0, 0.0, 1.0}, Vector3d{0.0, 1.0, 0.0},
                                               Vector3d{1.0, 0.0, 0.0}};
  const auto reference =
      std::min_element(references.begin(), references.end(), [&](Vector3d left, Vector3d right) {
        return std::abs(dot(*x, left)) < std::abs(dot(*x, right));
      });
  return normalized(subtract(*reference, scale(*x, dot(*x, *reference))));
}

[[nodiscard]] std::optional<TaperedSection> tapered_ellipse(std::string_view profile) {
  if (!profile.starts_with("ELD")) return std::nullopt;
  const auto values = dimensions(profile.substr(3));
  if (values.size() != 4) return std::nullopt;
  constexpr std::size_t segments = 40;
  TaperedSection result;
  result.start.reserve(segments);
  result.end.reserve(segments);
  for (std::size_t index = 0; index < segments; ++index) {
    const double angle =
        2.0 * std::numbers::pi * static_cast<double>(index) / static_cast<double>(segments);
    const double cosine = std::cos(angle);
    const double sine = std::sin(angle);
    result.start.push_back({values[0] * cosine / 2.0, values[1] * sine / 2.0});
    result.end.push_back({values[2] * cosine / 2.0, values[3] * sine / 2.0});
  }
  return result;
}

[[nodiscard]] std::optional<TaperedSection> variable_rectangle(std::string_view profile) {
  if (!profile.starts_with("PL_V")) return std::nullopt;
  std::array<double, 6> values{};
  std::size_t count = 0U;
  auto encoded = profile.substr(4U);
  for (std::size_t begin = 0U; begin < encoded.size();) {
    if (count == values.size()) return std::nullopt;
    const auto end = encoded.find('*', begin);
    const auto token =
        encoded.substr(begin, end == std::string_view::npos ? encoded.size() - begin : end - begin);
    double value = 0.0;
    const auto parsed = std::from_chars(token.data(), token.data() + token.size(), value);
    if (parsed.ec != std::errc{} || parsed.ptr != token.data() + token.size() ||
        !std::isfinite(value) || value < 0.0) {
      return std::nullopt;
    }
    values[count++] = value;
    if (end == std::string_view::npos) break;
    begin = end + 1U;
  }
  if (count != values.size() || std::min({values[0], values[1], values[2], values[3]}) <= 0.0) {
    return std::nullopt;
  }
  const auto rect = [](double height, double width) {
    return std::vector<std::array<double, 2>>{{-height / 2.0, -width / 2.0},
                                              {height / 2.0, -width / 2.0},
                                              {height / 2.0, width / 2.0},
                                              {-height / 2.0, width / 2.0}};
  };
  return TaperedSection{.start = rect(values[0], values[1]), .end = rect(values[2], values[3])};
}

[[nodiscard]] std::optional<TaperedIParameters> tapered_i_parameters(std::string_view profile) {
  TaperedIParameters result;
  if (profile.starts_with("PHI")) {
    const auto values = dimensions(profile.substr(3U), "*-");
    if (values.size() != 5U && values.size() != 7U) return std::nullopt;
    result = {.start_height = values[0],
              .end_height = values[1],
              .web = values[2],
              .bottom_flange = values[3],
              .bottom_width = values[4],
              .top_flange = values.size() == 7U ? values[5] : values[3],
              .top_width = values.size() == 7U ? values[6] : values[4]};
  } else if (profile.starts_with("TPG")) {
    const auto values = dimensions(profile.substr(3U));
    if (values.size() != 12U || values[1] != values[7] || values[2] != values[8] ||
        values[3] != values[9] || values[4] != values[10] || values[5] != values[11]) {
      return std::nullopt;
    }
    result = {.start_height = values[0],
              .end_height = values[6],
              .web = values[3],
              .bottom_flange = values[5],
              .bottom_width = values[4],
              .top_flange = values[2],
              .top_width = values[1]};
  } else if (profile.starts_with("PGT")) {
    const auto values = dimensions(profile.substr(3U));
    if (values.size() != 8U || values[1] != values[5] || values[2] != values[6] ||
        values[3] != values[7]) {
      return std::nullopt;
    }
    result = {.start_height = values[0],
              .end_height = values[4],
              .web = values[3],
              .bottom_flange = values[2],
              .bottom_width = values[1],
              .top_flange = values[2],
              .top_width = values[1]};
  } else {
    return std::nullopt;
  }
  if (std::min({result.start_height, result.end_height, result.web, result.bottom_flange,
                result.bottom_width, result.top_flange, result.top_width}) <= 0.0 ||
      result.web >= std::min(result.bottom_width, result.top_width) ||
      result.bottom_flange + result.top_flange >= result.start_height ||
      result.bottom_flange + result.top_flange >= result.end_height) {
    return std::nullopt;
  }
  return result;
}

[[nodiscard]] std::vector<std::array<double, 2>> tapered_i_contour(
    const TaperedIParameters& parameters, double height, double anchor, double width_center) {
  const double half_height = height / 2.0;
  const double half_web = parameters.web / 2.0;
  const double bottom_half_width = parameters.bottom_width / 2.0;
  const double top_half_width = parameters.top_width / 2.0;
  const double shift = anchor + half_height;
  const std::array<std::array<double, 2>, 12> conventional{
      {{-bottom_half_width, -half_height},
       {bottom_half_width, -half_height},
       {bottom_half_width, -half_height + parameters.bottom_flange},
       {half_web, -half_height + parameters.bottom_flange},
       {half_web, half_height - parameters.top_flange},
       {top_half_width, half_height - parameters.top_flange},
       {top_half_width, half_height},
       {-top_half_width, half_height},
       {-top_half_width, half_height - parameters.top_flange},
       {-half_web, half_height - parameters.top_flange},
       {-half_web, -half_height + parameters.bottom_flange},
       {-bottom_half_width, -half_height + parameters.bottom_flange}}};
  std::vector<std::array<double, 2>> result;
  result.reserve(conventional.size());
  for (const auto coordinate : conventional) {
    result.push_back({coordinate[1] + shift, -coordinate[0] + width_center});
  }
  return result;
}

[[nodiscard]] std::string uppercase(std::string_view value) {
  std::string result(value);
  std::transform(result.begin(), result.end(), result.begin(), [](unsigned char character) {
    return static_cast<char>(std::toupper(character));
  });
  return result;
}

[[nodiscard]] std::vector<std::array<double, 2>> rectangle(double height, double width) {
  return {{-height / 2.0, -width / 2.0},
          {height / 2.0, -width / 2.0},
          {height / 2.0, width / 2.0},
          {-height / 2.0, width / 2.0}};
}

[[nodiscard]] std::vector<std::array<double, 2>> circle(double diameter, std::size_t segments,
                                                        double angle_offset = 0.0) {
  std::vector<std::array<double, 2>> result;
  result.reserve(segments);
  const double radius = diameter / 2.0;
  for (std::size_t index = 0; index < segments; ++index) {
    const double angle =
        std::numbers::pi + angle_offset +
        static_cast<double>(index) * 2.0 * std::numbers::pi / static_cast<double>(segments);
    result.push_back({radius * std::cos(angle), radius * std::sin(angle)});
  }
  return result;
}

[[nodiscard]] std::vector<std::array<double, 2>> rounded_rectangle(
    double height, double width, double radius, std::size_t segments_per_corner = 5U) {
  std::vector<std::array<double, 2>> result;
  if (height <= 0.0 || width <= 0.0 || radius < 0.0 || 2.0 * radius > std::min(height, width) ||
      segments_per_corner < 2U) {
    return result;
  }
  result.reserve(4U * segments_per_corner);
  const double half_height = height / 2.0;
  const double half_width = width / 2.0;
  const std::array<std::array<double, 2>, 4> centers{{
      {-half_height + radius, -half_width + radius},
      {half_height - radius, -half_width + radius},
      {half_height - radius, half_width - radius},
      {-half_height + radius, half_width - radius},
  }};
  for (std::size_t corner = 0; corner < centers.size(); ++corner) {
    const double start = std::numbers::pi + static_cast<double>(corner) * std::numbers::pi / 2.0;
    for (std::size_t step = 0; step < segments_per_corner; ++step) {
      const double angle = start + static_cast<double>(step) * std::numbers::pi / 2.0 /
                                       static_cast<double>(segments_per_corner - 1U);
      result.push_back({centers[corner][0] + radius * std::cos(angle),
                        centers[corner][1] + radius * std::sin(angle)});
    }
  }
  return result;
}

[[nodiscard]] std::size_t round_segment_count(double diameter, double chord_tolerance = 1.0) {
  const double radius = diameter / 2.0;
  if (radius <= chord_tolerance) return 6;
  const double angular_count = std::numbers::pi / std::acos(1.0 - chord_tolerance / radius);
  const double boundary_count = std::ceil(angular_count) + 1.0;
  return std::max<std::size_t>(6, 4U * static_cast<std::size_t>(std::ceil(boundary_count / 4.0)));
}

[[nodiscard]] std::optional<Section> parse_section(std::string_view raw) {
  std::string profile = uppercase(raw);
  profile.erase(
      std::remove_if(profile.begin(), profile.end(),
                     [](unsigned char character) { return std::isspace(character) != 0; }),
      profile.end());
  if (profile.empty()) return std::nullopt;

  // Older DB1 strings are byte-oriented. Normalize the legacy single-byte
  // diameter sign before parsing the profile spelling.
  const bool legacy_diameter = static_cast<unsigned char>(profile.front()) == 0xD8U;

  if (const auto marker = profile.find("-HAUPTT"); marker != std::string::npos) {
    if (auto alias = parse_section(std::string_view(profile).substr(0U, marker))) {
      return alias;
    }
  }
  if (const auto separator = profile.rfind('-');
      separator != std::string::npos && separator + 1U < profile.size() &&
      !(profile.front() == 'U' && profile.size() > 1U &&
        std::isdigit(static_cast<unsigned char>(profile[1])) != 0) &&
      std::all_of(profile.begin() + static_cast<std::ptrdiff_t>(separator + 1U), profile.end(),
                  [](unsigned char character) { return std::isdigit(character) != 0; })) {
    if (auto alias = parse_section(std::string_view(profile).substr(0U, separator))) {
      return alias;
    }
  }

  if (const auto standard = standard_profile_contour(profile)) {
    return Section{.kind = Section::Kind::solid, .outer = {standard->begin(), standard->end()}};
  }

  // A channel remains as a small parametric fallback until it joins the
  // standardized contour catalog above.
  if (profile == "U220") {
    return Section{.kind = Section::Kind::solid,
                   .outer = {{-110.0, -40.0},
                             {110.0, -40.0},
                             {110.0, 40.0},
                             {97.5, 40.0},
                             {97.5, -31.0},
                             {-97.5, -31.0},
                             {-97.5, 40.0},
                             {-110.0, 40.0}}};
  }

  auto solid_rectangle = [&](std::string_view encoded) -> std::optional<Section> {
    const auto values = dimensions(encoded);
    if (values.size() != 2) return std::nullopt;
    return Section{.kind = Section::Kind::solid, .outer = rectangle(values[0], values[1])};
  };
  if (profile.starts_with("BL")) {
    const auto values = dimensions(std::string_view(profile).substr(2));
    if (values.size() == 2) {
      const auto normalized = normalized_plate_dimensions(values[0], values[1]);
      return Section{.kind = Section::Kind::solid,
                     .outer = rectangle(normalized[0], normalized[1])};
    }
  }
  if (profile.starts_with("PLT")) {
    const auto values = dimensions(std::string_view(profile).substr(3));
    if (values.size() == 2) {
      const auto normalized = normalized_plate_dimensions(values[0], values[1]);
      return Section{.kind = Section::Kind::solid,
                     .outer = rectangle(normalized[0], normalized[1])};
    }
  }
  if (profile.starts_with("PLATE")) {
    const auto values = dimensions(std::string_view(profile).substr(5));
    if (values.size() == 2U) {
      const auto normalized = normalized_plate_dimensions(values[0], values[1]);
      return Section{.kind = Section::Kind::solid,
                     .outer = rectangle(normalized[0], normalized[1])};
    }
  }
  if (profile.starts_with("PL") || profile.starts_with("FL")) {
    const auto values = dimensions(std::string_view(profile).substr(2));
    if (values.size() == 2U) {
      const auto normalized = normalized_plate_dimensions(values[0], values[1]);
      return Section{.kind = Section::Kind::solid,
                     .outer = rectangle(normalized[0], normalized[1])};
    }
  }
  if (profile.starts_with("HWR")) {
    return solid_rectangle(std::string_view(profile).substr(3));
  }
  if (profile.starts_with("AB")) {
    if (const auto diameter = number(std::string_view(profile).substr(2));
        diameter && *diameter > 0.0) {
      return Section{.kind = Section::Kind::solid,
                     .outer = circle(*diameter, 24),
                     .circular_outer_radius = *diameter / 2.0};
    }
  }
  if (profile.starts_with("HE") && !profile.starts_with("HEA") && !profile.starts_with("HEB") &&
      !profile.starts_with("HEM")) {
    if (const auto corner_diameter = number(std::string_view(profile).substr(2));
        corner_diameter && *corner_diameter > 0.0) {
      return Section{.kind = Section::Kind::solid, .outer = circle(*corner_diameter, 6)};
    }
  }
  const std::size_t round_prefix = profile.starts_with("ROD")  ? 3U
                                   : profile.starts_with("RD") ? 2U
                                                               : 1U;
  if ((profile.starts_with("D") || profile.starts_with("RD") || profile.starts_with("ROD")) &&
      number(std::string_view(profile).substr(round_prefix))) {
    const double diameter = *number(std::string_view(profile).substr(round_prefix));
    const std::size_t segments = round_segment_count(diameter);
    return Section{
        .kind = Section::Kind::solid,
        .outer = circle(diameter, segments, segments == 6 ? std::numbers::pi / 6.0 : 0.0),
        .circular_outer_radius = diameter / 2.0};
  }
  if (profile.starts_with("CHS") || profile.starts_with("TUBE") || profile.starts_with("RO") ||
      profile.starts_with("Ø") || legacy_diameter) {
    const std::size_t prefix = profile.starts_with("CHS")    ? 3U
                               : profile.starts_with("TUBE") ? 4U
                               : profile.starts_with("Ø")    ? std::string_view("Ø").size()
                               : legacy_diameter             ? 1U
                                                             : 2U;
    const auto values = dimensions(std::string_view(profile).substr(prefix));
    if (values.size() != 2 || values[0] <= 2.0 * values[1]) return std::nullopt;
    const auto segments = round_segment_count(values[0]);
    return Section{.kind = Section::Kind::hollow,
                   .outer = circle(values[0], segments),
                   .inner = circle(values[0] - 2.0 * values[1], segments),
                   .circular_outer_radius = values[0] / 2.0,
                   .circular_inner_radius = values[0] / 2.0 - values[1]};
  }
  if (profile.starts_with("O")) {
    const auto values = dimensions(std::string_view(profile).substr(1), "-");
    if (values.size() == 2U && values[0] > 2.0 * values[1]) {
      const auto segments = round_segment_count(values[0]);
      return Section{.kind = Section::Kind::hollow,
                     .outer = circle(values[0], segments),
                     .inner = circle(values[0] - 2.0 * values[1], segments),
                     .circular_outer_radius = values[0] / 2.0,
                     .circular_inner_radius = values[0] / 2.0 - values[1]};
    }
  }
  if (profile.starts_with("PD")) {
    const auto values = dimensions(std::string_view(profile).substr(2));
    if (values.size() != 2 || values[0] <= 2.0 * values[1]) return std::nullopt;
    return Section{.kind = Section::Kind::hollow,
                   .outer = circle(values[0], 12),
                   .inner = circle(values[0] - 2.0 * values[1], 12),
                   .circular_outer_radius = values[0] / 2.0,
                   .circular_inner_radius = values[0] / 2.0 - values[1]};
  }
  if (profile.starts_with("SHS") || profile.starts_with("QR")) {
    const std::size_t prefix = profile.starts_with("QR") ? 2U : 3U;
    const auto values = dimensions(std::string_view(profile).substr(prefix));
    if (values.size() != 2 && values.size() != 3) return std::nullopt;
    const double height = values[0];
    const double width = values.size() == 2 ? values[0] : values[1];
    const double thickness = values.back();
    if (height <= 2.0 * thickness || width <= 2.0 * thickness) {
      return std::nullopt;
    }
    return Section{.kind = Section::Kind::hollow,
                   .outer = rectangle(height, width),
                   .inner = rectangle(height - 2.0 * thickness, width - 2.0 * thickness)};
  }
  if (profile.starts_with("RHS")) {
    const auto values = dimensions(std::string_view(profile).substr(3));
    if (values.size() != 2U && values.size() != 3U) return std::nullopt;
    const double height = values[0];
    const double width = values.size() == 2U ? values[0] : values[1];
    const double thickness = values.back();
    if (height <= 2.0 * thickness || width <= 2.0 * thickness) {
      return std::nullopt;
    }
    return Section{.kind = Section::Kind::hollow,
                   .outer = rectangle(height, width),
                   .inner = rectangle(height - 2.0 * thickness, width - 2.0 * thickness)};
  }
  if (profile.size() >= 4U && profile.front() == 'U') {
    const auto separator = profile.find('-');
    if (separator != std::string::npos) {
      const auto size = number(std::string_view(profile).substr(1U, separator - 1U));
      const auto thickness = number(std::string_view(profile).substr(separator + 1U));
      if (size && thickness && 5.0 * *thickness < *size) {
        const double half = *size / 2.0;
        const double outer = 2.5 * *thickness;
        const double radius = 1.5 * *thickness;
        std::vector<std::array<double, 2>> contour{{-half, -half},
                                                   {half, -half},
                                                   {half, -half + *thickness},
                                                   {-half + outer, -half + *thickness}};
        constexpr std::size_t arc_segments = 6U;
        const std::array<double, 2> lower_center{-half + outer, -half + outer};
        for (std::size_t step = 1U; step < arc_segments; ++step) {
          const double angle = -std::numbers::pi / 2.0 - static_cast<double>(step) *
                                                             std::numbers::pi / 2.0 /
                                                             static_cast<double>(arc_segments - 1U);
          contour.push_back({lower_center[0] + radius * std::cos(angle),
                             lower_center[1] + radius * std::sin(angle)});
        }
        contour.push_back({-half + *thickness, half - outer});
        const std::array<double, 2> upper_center{-half + outer, half - outer};
        for (std::size_t step = 1U; step < arc_segments; ++step) {
          const double angle = std::numbers::pi - static_cast<double>(step) * std::numbers::pi /
                                                      2.0 / static_cast<double>(arc_segments - 1U);
          contour.push_back({upper_center[0] + radius * std::cos(angle),
                             upper_center[1] + radius * std::sin(angle)});
        }
        contour.insert(contour.end(), {{half, half - *thickness}, {half, half}, {-half, half}});
        return Section{.kind = Section::Kind::solid, .outer = std::move(contour)};
      }
    }
  }
  if (profile.starts_with("BLL")) {
    const auto values = dimensions(std::string_view(profile).substr(3U));
    if (values.size() == 3U && values[2] > 0.0 &&
        5.0 * values[2] < std::min(values[0], values[1])) {
      const auto [height, width, thickness] = std::tuple{values[0], values[1], values[2]};
      const double half_height = height / 2.0;
      const double half_width = width / 2.0;
      const double tangent = 2.5 * thickness;
      const double radius = 1.5 * thickness;
      std::vector<std::array<double, 2>> contour{{-half_width, -half_height},
                                                 {half_width, -half_height},
                                                 {half_width, -half_height + thickness},
                                                 {-half_width + tangent, -half_height + thickness}};
      const std::array<double, 2> center{-half_width + tangent, -half_height + tangent};
      constexpr std::size_t arc_segments = 5U;
      for (std::size_t step = 1U; step < arc_segments; ++step) {
        const double angle = -std::numbers::pi / 2.0 - static_cast<double>(step) *
                                                           std::numbers::pi / 2.0 /
                                                           static_cast<double>(arc_segments - 1U);
        contour.push_back(
            {center[0] + radius * std::cos(angle), center[1] + radius * std::sin(angle)});
      }
      contour.insert(contour.end(),
                     {{-half_width + thickness, half_height}, {-half_width, half_height}});
      return Section{.kind = Section::Kind::solid, .outer = std::move(contour)};
    }
  }
  if (profile.starts_with("P")) {
    const auto values = dimensions(std::string_view(profile).substr(1U));
    if (values.size() == 3U && values[2] > 0.0 &&
        4.0 * values[2] < std::min(values[0], values[1])) {
      const auto outer = rounded_rectangle(values[0], values[1], 2.0 * values[2], 6U);
      const auto inner = rounded_rectangle(values[0] - 2.0 * values[2], values[1] - 2.0 * values[2],
                                           values[2], 6U);
      if (!outer.empty() && !inner.empty()) {
        return Section{.kind = Section::Kind::hollow, .outer = outer, .inner = inner};
      }
    }
  }
  if (profile.starts_with("PRMDAS")) {
    auto encoded = std::string_view(profile).substr(6U);
    if (const auto separator = encoded.find('-'); separator != std::string_view::npos) {
      encoded = encoded.substr(0U, separator);
    }
    const auto values = dimensions(encoded);
    if (values.size() >= 2U) {
      return Section{.kind = Section::Kind::solid, .outer = rectangle(values[0], values[1])};
    }
  }
  if (profile.starts_with("HI") || profile.starts_with("WI")) {
    const auto values = dimensions(std::string_view(profile).substr(2), "*Xx-");
    if (values.size() == 4U) {
      const auto [height, web, flange, width] =
          std::tuple{values[0], values[1], values[2], values[3]};
      if (web < width && 2.0 * flange < height) {
        const double half_height = height / 2.0;
        const double half_width = width / 2.0;
        const double half_web = web / 2.0;
        return Section{.kind = Section::Kind::solid,
                       .outer = {{-half_height, half_width},
                                 {-half_height, -half_width},
                                 {-half_height + flange, -half_width},
                                 {-half_height + flange, -half_web},
                                 {half_height - flange, -half_web},
                                 {half_height - flange, -half_width},
                                 {half_height, -half_width},
                                 {half_height, half_width},
                                 {half_height - flange, half_width},
                                 {half_height - flange, half_web},
                                 {-half_height + flange, half_web},
                                 {-half_height + flange, half_width}}};
      }
    }
  }
  if (profile.starts_with("BHK") || profile.starts_with("HK")) {
    const std::size_t prefix = profile.starts_with("BHK") ? 3U : 2U;
    const auto values = dimensions(std::string_view(profile).substr(prefix));
    if (values.size() == 5U) {
      const auto [height, web, flange, width, edge] =
          std::tuple{values[0], values[1], values[2], values[3], values[4]};
      const double half_height = height / 2.0;
      const double half_width = width / 2.0;
      const double outside_web = half_width - edge;
      const double inside_web = outside_web - web;
      if (2.0 * flange < height && edge >= 0.0 && inside_web > 0.0) {
        return Section{.kind = Section::Kind::solid,
                       .outer = {{-half_height, half_width},
                                 {-half_height, -half_width},
                                 {-half_height + flange, -half_width},
                                 {-half_height + flange, -outside_web},
                                 {half_height - flange, -outside_web},
                                 {half_height - flange, -half_width},
                                 {half_height, -half_width},
                                 {half_height, half_width},
                                 {half_height - flange, half_width},
                                 {half_height - flange, outside_web},
                                 {-half_height + flange, outside_web},
                                 {-half_height + flange, half_width}}};
      }
    }
  }
  if (profile.starts_with("PG")) {
    const auto values = dimensions(std::string_view(profile).substr(2));
    if (values.size() == 6) {
      const auto [height, top_width, top_flange, web, bottom_width, bottom_flange] =
          std::tuple{values[0], values[1], values[2], values[3], values[4], values[5]};
      if (std::min({height, top_width, top_flange, web, bottom_width, bottom_flange}) > 0.0 &&
          web < std::min(top_width, bottom_width) && top_flange + bottom_flange < height) {
        const double half_height = height / 2.0;
        const double half_web = web / 2.0;
        const double top_half_width = top_width / 2.0;
        const double bottom_half_width = bottom_width / 2.0;
        return Section{.kind = Section::Kind::solid,
                       .outer = {{-half_height, bottom_half_width},
                                 {-half_height, -bottom_half_width},
                                 {-half_height + bottom_flange, -bottom_half_width},
                                 {-half_height + bottom_flange, -half_web},
                                 {half_height - top_flange, -half_web},
                                 {half_height - top_flange, -top_half_width},
                                 {half_height, -top_half_width},
                                 {half_height, top_half_width},
                                 {half_height - top_flange, top_half_width},
                                 {half_height - top_flange, half_web},
                                 {-half_height + bottom_flange, half_web},
                                 {-half_height + bottom_flange, bottom_half_width}}};
      }
    }
  }
  if (profile.starts_with("TG")) {
    const auto values = dimensions(std::string_view(profile).substr(2));
    if (values.size() == 4) {
      const auto [height, width, flange, web] =
          std::tuple{values[0], values[1], values[2], values[3]};
      if (std::min({height, width, flange, web}) > 0.0 && web < width && 2.0 * flange < height) {
        const double half_height = height / 2.0;
        const double half_width = width / 2.0;
        const double half_web = web / 2.0;
        return Section{.kind = Section::Kind::solid,
                       .outer = {{-half_height, half_width},
                                 {-half_height, -half_width},
                                 {-half_height + flange, -half_width},
                                 {-half_height + flange, -half_web},
                                 {half_height - flange, -half_web},
                                 {half_height - flange, -half_width},
                                 {half_height, -half_width},
                                 {half_height, half_width},
                                 {half_height - flange, half_width},
                                 {half_height - flange, half_web},
                                 {-half_height + flange, half_web},
                                 {-half_height + flange, half_width}}};
      }
    }
  }
  if (profile.starts_with("B_WLD_H")) {
    const auto values = dimensions(std::string_view(profile).substr(7));
    if (values.size() != 6) return std::nullopt;
    const auto [height, positive_width, negative_width, web, positive_flange, negative_flange] =
        std::tuple{values[0], values[1], values[2], values[3], values[4], values[5]};
    if (web >= std::min(positive_width, negative_width) ||
        positive_flange + negative_flange >= height) {
      return std::nullopt;
    }
    const double half_height = height / 2.0;
    const double half_web = web / 2.0;
    const double positive_half_width = positive_width / 2.0;
    const double negative_half_width = negative_width / 2.0;
    return Section{.kind = Section::Kind::solid,
                   .outer = {{-half_height, negative_half_width},
                             {-half_height, -negative_half_width},
                             {-half_height + negative_flange, -negative_half_width},
                             {-half_height + negative_flange, -half_web},
                             {half_height - positive_flange, -half_web},
                             {half_height - positive_flange, -positive_half_width},
                             {half_height, -positive_half_width},
                             {half_height, positive_half_width},
                             {half_height - positive_flange, positive_half_width},
                             {half_height - positive_flange, half_web},
                             {-half_height + negative_flange, half_web},
                             {-half_height + negative_flange, negative_half_width}}};
  }
  if (profile.starts_with("IRR_I")) {
    const auto values = dimensions(std::string_view(profile).substr(5));
    if (values.size() != 3 || values[2] > values[1]) return std::nullopt;
    const double half_height = values[0] / 2.0;
    const double half_width = values[1] / 2.0;
    return Section{.kind = Section::Kind::solid,
                   .outer = {{half_height, -half_width},
                             {-half_height, -half_width},
                             {-half_height, values[2] - half_width},
                             {half_height, half_width}}};
  }
  if (profile.starts_with("L") || profile.starts_with("UKA")) {
    const std::size_t prefix = profile.starts_with("UKA") ? 3U : 1U;
    const auto values = dimensions(std::string_view(profile).substr(prefix));
    if ((values.size() == 2 || values.size() == 3)) {
      const double height = values[0];
      const double width = values.size() == 2 ? values[0] : values[1];
      const double thickness = values.back();
      if (thickness < std::min(height, width)) {
        return Section{.kind = Section::Kind::solid,
                       .outer = {{-height / 2.0, -width / 2.0},
                                 {height / 2.0, -width / 2.0},
                                 {height / 2.0, -width / 2.0 + thickness},
                                 {-height / 2.0 + thickness, -width / 2.0 + thickness},
                                 {-height / 2.0 + thickness, width / 2.0},
                                 {-height / 2.0, width / 2.0}},
                       .cap = {{0, 1, 2}, {0, 2, 3}, {0, 3, 5}, {3, 4, 5}}};
      }
    }
  }
  return solid_rectangle(profile);
}

[[nodiscard]] double scalar(std::span<const std::byte> tuple, const FieldSchema& field) noexcept {
  if (field.type == FieldType::f32) return read_f32(tuple, field.offset);
  if (field.type == FieldType::f64) return read_f64(tuple, field.offset);
  return static_cast<double>(read_u32(tuple, field.offset));
}

[[nodiscard]] double signed_area(std::span<const std::array<double, 2>> points) noexcept {
  double area = 0.0;
  for (std::size_t index = 0; index < points.size(); ++index) {
    const auto& current = points[index];
    const auto& next = points[(index + 1U) % points.size()];
    area += current[0] * next[1] - next[0] * current[1];
  }
  return area / 2.0;
}

void set_section_metrics(DefinitionGeometryView& definition, const Section& section) {
  if (section.outer.size() < 3U) return;
  double minimum_first = std::numeric_limits<double>::infinity();
  double maximum_first = -std::numeric_limits<double>::infinity();
  double minimum_second = std::numeric_limits<double>::infinity();
  double maximum_second = -std::numeric_limits<double>::infinity();
  for (const auto& coordinate : section.outer) {
    minimum_first = std::min(minimum_first, coordinate[0]);
    maximum_first = std::max(maximum_first, coordinate[0]);
    minimum_second = std::min(minimum_second, coordinate[1]);
    maximum_second = std::max(maximum_second, coordinate[1]);
  }
  const double outer_area = std::abs(signed_area(section.outer));
  const double inner_area = std::abs(signed_area(section.inner));
  const double area = outer_area - inner_area;
  const double height = maximum_first - minimum_first;
  const double width = maximum_second - minimum_second;
  if (std::isfinite(area) && std::isfinite(height) && std::isfinite(width) && area > 0.0 &&
      height > 0.0 && width > 0.0) {
    definition.section_area = area;
    definition.section_height = height;
    definition.section_width = width;
    definition.has_section_metrics = true;
  }
}

void set_report_section_metrics(DefinitionGeometryView& definition,
                                const StandardProfileMetrics& metrics) {
  if (!std::isfinite(metrics.area) || !std::isfinite(metrics.cover_perimeter) ||
      !std::isfinite(metrics.height) || !std::isfinite(metrics.width) || metrics.area <= 0.0 ||
      metrics.cover_perimeter <= 0.0 || metrics.height <= 0.0 || metrics.width <= 0.0) {
    return;
  }
  definition.report_section_area = metrics.area;
  definition.report_cover_perimeter = metrics.cover_perimeter;
  definition.report_section_height = metrics.height;
  definition.report_section_width = metrics.width;
  definition.has_report_section_metrics = true;
}

void set_report_section_metrics(DefinitionGeometryView& definition, const CatalogSection& section) {
  if (!section.has_report_metrics) return;
  set_report_section_metrics(
      definition, StandardProfileMetrics{.area = section.report_area,
                                         .cover_perimeter = section.report_cover_perimeter,
                                         .height = section.report_height,
                                         .width = section.report_width});
}

void set_tapered_section_metrics(DefinitionGeometryView& definition,
                                 const TaperedSection& section) {
  if (section.start.size() < 3U) return;
  Section start;
  start.kind = Section::Kind::solid;
  start.outer = section.start;
  set_section_metrics(definition, start);
}

[[nodiscard]] std::optional<std::vector<std::array<double, 2>>> offset_polygon_outward(
    std::span<const std::array<double, 2>> points, double distance) {
  if (points.size() < 3U || !std::isfinite(distance) || distance < 0.0) {
    return std::nullopt;
  }
  if (distance == 0.0) {
    return std::vector<std::array<double, 2>>(points.begin(), points.end());
  }
  const double area = signed_area(points);
  if (std::abs(area) <= 1.0e-12) return std::nullopt;
  const double orientation = area > 0.0 ? 1.0 : -1.0;
  std::vector<std::array<double, 2>> directions(points.size());
  std::vector<std::array<double, 2>> normals(points.size());
  for (std::size_t index = 0; index < points.size(); ++index) {
    const auto& start = points[index];
    const auto& end = points[(index + 1U) % points.size()];
    const double dx = end[0] - start[0];
    const double dy = end[1] - start[1];
    const double edge_length = std::hypot(dx, dy);
    if (edge_length <= 1.0e-12) return std::nullopt;
    directions[index] = {dx, dy};
    normals[index] = {orientation * dy / edge_length, -orientation * dx / edge_length};
  }
  std::vector<std::array<double, 2>> result;
  result.reserve(points.size());
  for (std::size_t index = 0; index < points.size(); ++index) {
    const std::size_t previous = (index + points.size() - 1U) % points.size();
    const auto& vertex = points[index];
    const auto& previous_direction = directions[previous];
    const auto& current_direction = directions[index];
    const auto& previous_normal = normals[previous];
    const auto& current_normal = normals[index];
    const std::array<double, 2> previous_point{vertex[0] + previous_normal[0] * distance,
                                               vertex[1] + previous_normal[1] * distance};
    const std::array<double, 2> current_point{vertex[0] + current_normal[0] * distance,
                                              vertex[1] + current_normal[1] * distance};
    const double denominator =
        previous_direction[0] * current_direction[1] - previous_direction[1] * current_direction[0];
    const double scale_value = std::hypot(previous_direction[0], previous_direction[1]) *
                               std::hypot(current_direction[0], current_direction[1]);
    if (std::abs(denominator) <= scale_value * 1.0e-10) {
      std::array<double, 2> average{previous_normal[0] + current_normal[0],
                                    previous_normal[1] + current_normal[1]};
      double average_length = std::hypot(average[0], average[1]);
      if (average_length <= 1.0e-12) {
        average = current_normal;
        average_length = 1.0;
      }
      result.push_back({vertex[0] + average[0] / average_length * distance,
                        vertex[1] + average[1] / average_length * distance});
      continue;
    }
    const std::array<double, 2> delta{current_point[0] - previous_point[0],
                                      current_point[1] - previous_point[1]};
    const double parameter =
        (delta[0] * current_direction[1] - delta[1] * current_direction[0]) / denominator;
    const std::array<double, 2> candidate{previous_point[0] + previous_direction[0] * parameter,
                                          previous_point[1] + previous_direction[1] * parameter};
    if (std::hypot(candidate[0] - vertex[0], candidate[1] - vertex[1]) > distance * 1.0e4) {
      result.push_back(current_point);
    } else {
      result.push_back(candidate);
    }
  }
  return result;
}

[[nodiscard]] double cross2(const std::array<double, 2>& a, const std::array<double, 2>& b,
                            const std::array<double, 2>& c) noexcept {
  return (b[0] - a[0]) * (c[1] - a[1]) - (b[1] - a[1]) * (c[0] - a[0]);
}

[[nodiscard]] bool point_in_triangle(const std::array<double, 2>& point,
                                     const std::array<double, 2>& a, const std::array<double, 2>& b,
                                     const std::array<double, 2>& c) noexcept {
  constexpr double epsilon = -1e-12;
  return cross2(a, b, point) >= epsilon && cross2(b, c, point) >= epsilon &&
         cross2(c, a, point) >= epsilon;
}

[[nodiscard]] std::optional<std::vector<std::array<std::uint32_t, 3>>> triangulate(
    std::span<const std::array<double, 2>> points) {
  if (points.size() < 3) return std::nullopt;
  std::vector<std::uint32_t> remaining(points.size());
  for (std::size_t index = 0; index < points.size(); ++index) {
    remaining[index] = static_cast<std::uint32_t>(index);
  }
  if (signed_area(points) < 0.0) std::reverse(remaining.begin(), remaining.end());
  std::vector<std::array<std::uint32_t, 3>> result;
  result.reserve(points.size() - 2U);
  while (remaining.size() > 3) {
    bool clipped = false;
    for (std::size_t cursor = 0; cursor < remaining.size(); ++cursor) {
      const auto previous = remaining[(cursor + remaining.size() - 1U) % remaining.size()];
      const auto current = remaining[cursor];
      const auto following = remaining[(cursor + 1U) % remaining.size()];
      if (cross2(points[previous], points[current], points[following]) <= 1e-12) {
        continue;
      }
      bool contains_point = false;
      for (const auto candidate : remaining) {
        if (candidate == previous || candidate == current || candidate == following) continue;
        if (point_in_triangle(points[candidate], points[previous], points[current],
                              points[following])) {
          contains_point = true;
          break;
        }
      }
      if (contains_point) continue;
      result.push_back({previous, current, following});
      remaining.erase(remaining.begin() + static_cast<std::ptrdiff_t>(cursor));
      clipped = true;
      break;
    }
    if (!clipped) return std::nullopt;
  }
  result.push_back({remaining[0], remaining[1], remaining[2]});
  return result;
}

[[nodiscard]] std::optional<std::vector<std::array<double, 2>>> chamfered(const Contour& contour) {
  if (contour.points.size() < 3 || contour.dx.size() != contour.points.size() ||
      contour.dy.size() != contour.points.size() || contour.types.size() != contour.points.size())
    return std::nullopt;
  std::vector<std::array<double, 2>> result;
  if (std::find(contour.types.begin(), contour.types.end(), 40U) != contour.types.end()) {
    if (std::any_of(contour.types.begin(), contour.types.end(),
                    [](std::uint32_t type) { return type != 0U && type != 40U; })) {
      return std::nullopt;
    }
    for (std::size_t index = 0; index < contour.types.size(); ++index) {
      if (contour.types[index] == 40U &&
          contour.types[(index + contour.types.size() - 1U) % contour.types.size()] == 40U) {
        return std::nullopt;
      }
    }
    const auto append_distinct = [&](std::array<double, 2> value) {
      if (result.empty() ||
          std::hypot(result.back()[0] - value[0], result.back()[1] - value[1]) > 1e-7) {
        result.push_back(value);
      }
    };
    for (std::size_t index = 0; index < contour.points.size(); ++index) {
      if (contour.types[index] == 40U) continue;
      const auto start = contour.points[index];
      append_distinct({start.x, start.y});
      const auto control_index = (index + 1U) % contour.points.size();
      if (contour.types[control_index] != 40U) continue;
      const auto end = contour.points[(index + 2U) % contour.points.size()];
      const auto control = contour.points[control_index];
      const double determinant =
          2.0 * (start.x * (control.y - end.y) + control.x * (end.y - start.y) +
                 end.x * (start.y - control.y));
      if (std::abs(determinant) <= 1e-9) return std::nullopt;
      const double start_squared = start.x * start.x + start.y * start.y;
      const double control_squared = control.x * control.x + control.y * control.y;
      const double end_squared = end.x * end.x + end.y * end.y;
      const std::array<double, 2> center{
          (start_squared * (control.y - end.y) + control_squared * (end.y - start.y) +
           end_squared * (start.y - control.y)) /
              determinant,
          (start_squared * (end.x - control.x) + control_squared * (start.x - end.x) +
           end_squared * (control.x - start.x)) /
              determinant};
      const auto angle = [&](Vector3d point) {
        return std::atan2(point.y - center[1], point.x - center[0]);
      };
      const double start_angle = angle(start);
      const auto positive_delta = [&](double candidate) {
        double value = std::fmod(candidate - start_angle, 2.0 * std::numbers::pi);
        if (value < 0.0) value += 2.0 * std::numbers::pi;
        return value;
      };
      const double control_delta = positive_delta(angle(control));
      const double end_delta = positive_delta(angle(end));
      const double sweep =
          control_delta <= end_delta ? end_delta : end_delta - 2.0 * std::numbers::pi;
      constexpr double maximum_step = 5.0 * std::numbers::pi / 180.0;
      const auto steps = std::max<std::size_t>(
          2U, static_cast<std::size_t>(std::ceil(std::abs(sweep) / maximum_step)));
      const double radius = std::hypot(start.x - center[0], start.y - center[1]);
      for (std::size_t step = 1; step < steps; ++step) {
        const double sampled_angle =
            start_angle + sweep * static_cast<double>(step) / static_cast<double>(steps);
        append_distinct({center[0] + radius * std::cos(sampled_angle),
                         center[1] + radius * std::sin(sampled_angle)});
      }
    }
    if (result.size() > 1U && std::hypot(result.front()[0] - result.back()[0],
                                         result.front()[1] - result.back()[1]) <= 1e-7) {
      result.pop_back();
    }
    return result.size() >= 3U ? std::optional{std::move(result)} : std::nullopt;
  }
  for (std::size_t index = 0; index < contour.points.size(); ++index) {
    const auto point = contour.points[index];
    const auto type = contour.types[index];
    if (type == 0U) {
      result.push_back({point.x, point.y});
      continue;
    }
    if (type != 10U && type != 20U && type != 30U) return std::nullopt;
    const auto previous =
        contour.points[(index + contour.points.size() - 1U) % contour.points.size()];
    const auto following = contour.points[(index + 1U) % contour.points.size()];
    const auto unit_toward = [&](const Vector3d& target) -> std::optional<std::array<double, 2>> {
      const double x = target.x - point.x;
      const double y = target.y - point.y;
      const double magnitude = std::hypot(x, y);
      if (magnitude <= 1e-9) return std::nullopt;
      return std::array<double, 2>{x / magnitude, y / magnitude};
    };
    const auto previous_unit = unit_toward(previous);
    const auto following_unit = unit_toward(following);
    if (!previous_unit || !following_unit) return std::nullopt;
    if (type == 10U) {
      result.push_back({point.x + (*previous_unit)[0] * std::abs(contour.dx[index]),
                        point.y + (*previous_unit)[1] * std::abs(contour.dx[index])});
      result.push_back({point.x + (*following_unit)[0] * std::abs(contour.dy[index]),
                        point.y + (*following_unit)[1] * std::abs(contour.dy[index])});
      continue;
    }
    double radius = std::abs(contour.dx[index]);
    if (radius == 0.0) radius = std::abs(contour.dy[index]);
    if (radius == 0.0) {
      result.push_back({point.x, point.y});
      continue;
    }
    const double cosine = std::clamp(
        (*previous_unit)[0] * (*following_unit)[0] + (*previous_unit)[1] * (*following_unit)[1],
        -1.0, 1.0);
    const double angle = std::acos(cosine);
    if (angle <= 1e-6 || angle >= std::numbers::pi - 1e-6) return std::nullopt;
    const double previous_length = std::hypot(previous.x - point.x, previous.y - point.y);
    const double following_length = std::hypot(following.x - point.x, following.y - point.y);
    double tangent_distance = radius / std::tan(angle / 2.0);
    const double maximum_distance = std::min(previous_length, following_length) * (1.0 - 1e-9);
    if (tangent_distance > maximum_distance) {
      tangent_distance = maximum_distance;
      radius = tangent_distance * std::tan(angle / 2.0);
    }
    const std::array<double, 2> start{point.x + (*previous_unit)[0] * tangent_distance,
                                      point.y + (*previous_unit)[1] * tangent_distance};
    const std::array<double, 2> end{point.x + (*following_unit)[0] * tangent_distance,
                                    point.y + (*following_unit)[1] * tangent_distance};
    const std::array<double, 2> bisector{(*previous_unit)[0] + (*following_unit)[0],
                                         (*previous_unit)[1] + (*following_unit)[1]};
    const double bisector_length = std::hypot(bisector[0], bisector[1]);
    if (bisector_length <= 1e-9) return std::nullopt;
    const double center_distance = radius / std::sin(angle / 2.0);
    const std::array<double, 2> center{point.x + bisector[0] / bisector_length * center_distance,
                                       point.y + bisector[1] / bisector_length * center_distance};
    const double start_angle = std::atan2(start[1] - center[1], start[0] - center[0]);
    double end_angle = std::atan2(end[1] - center[1], end[0] - center[0]);
    const std::array<double, 2> incoming{point.x - previous.x, point.y - previous.y};
    const std::array<double, 2> outgoing{following.x - point.x, following.y - point.y};
    const double turn = incoming[0] * outgoing[1] - incoming[1] * outgoing[0];
    if (turn > 0.0) {
      while (end_angle <= start_angle) end_angle += 2.0 * std::numbers::pi;
    } else {
      while (end_angle >= start_angle) end_angle -= 2.0 * std::numbers::pi;
    }
    const double sweep = end_angle - start_angle;
    const std::size_t segments = std::max<std::size_t>(
        2, static_cast<std::size_t>(std::ceil(std::abs(sweep) / (5.0 * std::numbers::pi / 180.0))));
    for (std::size_t step = 0; step <= segments; ++step) {
      const double sample =
          start_angle + sweep * static_cast<double>(step) / static_cast<double>(segments);
      const std::array<double, 2> value{center[0] + radius * std::cos(sample),
                                        center[1] + radius * std::sin(sample)};
      if (result.empty() ||
          std::hypot(result.back()[0] - value[0], result.back()[1] - value[1]) > 1e-7) {
        result.push_back(value);
      }
    }
  }
  if (result.size() > 1 && std::hypot(result.front()[0] - result.back()[0],
                                      result.front()[1] - result.back()[1]) <= 1e-7) {
    result.pop_back();
  }
  return result;
}

void append_vertex(MeshData& mesh, Vector3d value) {
  mesh.positions.push_back(value.x);
  mesh.positions.push_back(value.y);
  mesh.positions.push_back(value.z);
}

[[nodiscard]] RecipeLoop section_recipe_loop(std::span<const std::array<double, 2>> ring,
                                             const DefinitionGeometryView& definition, double x) {
  RecipeLoop loop;
  loop.points.reserve(ring.size());
  for (const auto& coordinate : ring) {
    loop.points.push_back(point(definition.origin, definition.x_axis, definition.y_axis,
                                definition.z_axis, x, coordinate[0], coordinate[1]));
  }
  return loop;
}

[[nodiscard]] ExtrusionRecipe extrusion_recipe(std::uint64_t object_id, const Section& section,
                                               const DefinitionGeometryView& definition) {
  auto outer = section.outer;
  auto inner = section.inner;
  if (signed_area(outer) < 0.0) std::reverse(outer.begin(), outer.end());
  if (signed_area(inner) < 0.0) std::reverse(inner.begin(), inner.end());
  ExtrusionRecipe recipe{.object_id = object_id,
                         .vector = scale(definition.x_axis, definition.length)};
  recipe.loops.push_back(section_recipe_loop(outer, definition, 0.0));
  if (section.kind == Section::Kind::hollow && inner.size() >= 3U) {
    recipe.loops.push_back(section_recipe_loop(inner, definition, 0.0));
  }
  return recipe;
}

[[nodiscard]] ExtrusionRecipe contour_extrusion_recipe(
    std::uint64_t object_id, std::span<const std::array<double, 2>> contour,
    const DefinitionGeometryView& definition, double thickness) {
  auto normalized_contour = std::vector<std::array<double, 2>>(contour.begin(), contour.end());
  if (signed_area(normalized_contour) < 0.0) {
    std::reverse(normalized_contour.begin(), normalized_contour.end());
  }
  RecipeLoop loop;
  loop.points.reserve(normalized_contour.size());
  const double base = -thickness / 2.0;
  for (const auto& coordinate : normalized_contour) {
    loop.points.push_back(point(definition.origin, definition.x_axis, definition.y_axis,
                                definition.z_axis, coordinate[0], coordinate[1], base));
  }
  ExtrusionRecipe recipe{.object_id = object_id, .vector = scale(definition.z_axis, thickness)};
  recipe.loops.push_back(std::move(loop));
  return recipe;
}

[[nodiscard]] MeshData extrude(std::uint64_t object_id, const Section& section,
                               const DefinitionGeometryView& definition) {
  MeshData mesh;
  mesh.object_id = object_id;
  mesh.section_origin = definition.origin;
  mesh.section_y_axis = definition.y_axis;
  mesh.section_z_axis = definition.z_axis;
  mesh.has_section_frame = true;
  mesh.circular_inner_radius = section.circular_inner_radius;
  auto outer = section.outer;
  auto inner = section.inner;
  if (signed_area(outer) < 0.0) std::reverse(outer.begin(), outer.end());
  if (signed_area(inner) < 0.0) std::reverse(inner.begin(), inner.end());
  const std::size_t outer_count = outer.size();
  const std::size_t inner_count = inner.size();
  if (mesh.circular_inner_radius > 0.0 && inner_count >= 3U) {
    mesh.circular_inner_apothem = std::numeric_limits<double>::infinity();
    for (std::size_t index = 0; index < inner_count; ++index) {
      const auto& first = inner[index];
      const auto& second = inner[(index + 1U) % inner_count];
      const double edge_x = second[0] - first[0];
      const double edge_y = second[1] - first[1];
      const double edge_length = std::hypot(edge_x, edge_y);
      if (edge_length <= 1.0e-12) continue;
      mesh.circular_inner_apothem =
          std::min(mesh.circular_inner_apothem,
                   std::abs(first[0] * second[1] - first[1] * second[0]) / edge_length);
    }
    if (!std::isfinite(mesh.circular_inner_apothem)) mesh.circular_inner_apothem = 0.0;
  }
  const auto append_ring = [&](std::span<const std::array<double, 2>> ring, double x) {
    for (const auto& coordinate : ring) {
      append_vertex(mesh, point(definition.origin, definition.x_axis, definition.y_axis,
                                definition.z_axis, x, coordinate[0], coordinate[1]));
    }
  };
  append_ring(outer, 0.0);
  append_ring(outer, definition.length);
  if (section.kind == Section::Kind::hollow) {
    append_ring(inner, 0.0);
    append_ring(inner, definition.length);
  }
  const auto quad = [&](std::uint32_t a, std::uint32_t b, std::uint32_t c, std::uint32_t d) {
    mesh.indices.insert(mesh.indices.end(), {a, b, c, a, c, d});
  };
  for (std::size_t index = 0; index < outer_count; ++index) {
    const auto next = (index + 1U) % outer_count;
    quad(static_cast<std::uint32_t>(index), static_cast<std::uint32_t>(next),
         static_cast<std::uint32_t>(next + outer_count),
         static_cast<std::uint32_t>(index + outer_count));
  }
  if (section.kind == Section::Kind::solid) {
    std::vector<std::array<std::uint32_t, 3>> cap;
    if (const auto evaluated = triangulate(outer)) cap = *evaluated;
    for (const auto& triangle : cap) {
      mesh.indices.insert(mesh.indices.end(), {triangle[0], triangle[2], triangle[1]});
      mesh.indices.insert(mesh.indices.end(),
                          {triangle[0] + static_cast<std::uint32_t>(outer_count),
                           triangle[1] + static_cast<std::uint32_t>(outer_count),
                           triangle[2] + static_cast<std::uint32_t>(outer_count)});
    }
  } else {
    const auto inner_start = static_cast<std::uint32_t>(outer_count * 2U);
    const auto inner_end = inner_start + static_cast<std::uint32_t>(inner_count);
    for (std::size_t index = 0; index < inner_count; ++index) {
      const auto next = (index + 1U) % inner_count;
      quad(inner_start + static_cast<std::uint32_t>(next),
           inner_start + static_cast<std::uint32_t>(index),
           inner_end + static_cast<std::uint32_t>(index),
           inner_end + static_cast<std::uint32_t>(next));
    }
    if (outer_count == inner_count) {
      for (std::size_t index = 0; index < outer_count; ++index) {
        const auto next = (index + 1U) % outer_count;
        quad(static_cast<std::uint32_t>(next), static_cast<std::uint32_t>(index),
             inner_start + static_cast<std::uint32_t>(index),
             inner_start + static_cast<std::uint32_t>(next));
        quad(static_cast<std::uint32_t>(outer_count + index),
             static_cast<std::uint32_t>(outer_count + next),
             inner_end + static_cast<std::uint32_t>(next),
             inner_end + static_cast<std::uint32_t>(index));
      }
    }
  }
  return mesh;
}

[[nodiscard]] MeshData loft(std::uint64_t object_id, const TaperedSection& section,
                            const DefinitionGeometryView& definition) {
  MeshData mesh{.object_id = object_id};
  const auto count = static_cast<std::uint32_t>(section.start.size());
  for (const auto& coordinate : section.start) {
    append_vertex(mesh, point(definition.origin, definition.x_axis, definition.y_axis,
                              definition.z_axis, 0.0, coordinate[0], coordinate[1]));
  }
  for (const auto& coordinate : section.end) {
    append_vertex(mesh, point(definition.origin, definition.x_axis, definition.y_axis,
                              definition.z_axis, definition.length, coordinate[0], coordinate[1]));
  }
  for (std::uint32_t index = 0; index < count; ++index) {
    const auto next = (index + 1U) % count;
    mesh.indices.insert(mesh.indices.end(),
                        {index, next, next + count, index, next + count, index + count});
  }
  const auto start_cap = triangulate(section.start);
  const auto end_cap = triangulate(section.end);
  if (start_cap) {
    for (const auto triangle : *start_cap) {
      mesh.indices.insert(mesh.indices.end(), {triangle[0], triangle[2], triangle[1]});
    }
  }
  if (end_cap) {
    for (const auto triangle : *end_cap) {
      mesh.indices.insert(mesh.indices.end(),
                          {triangle[0] + count, triangle[1] + count, triangle[2] + count});
    }
  }
  return mesh;
}

[[nodiscard]] std::optional<MeshData> loft_plate(std::uint64_t object_id,
                                                 const LoftRails& persisted,
                                                 const DefinitionGeometryView& definition,
                                                 double thickness) {
  if (!std::isfinite(thickness) || thickness <= 0.0 || persisted.rails[0].size() < 2U ||
      persisted.rails[1].size() < 2U) {
    return std::nullopt;
  }
  std::array<std::vector<Vector3d>, 2> rails;
  for (std::size_t rail = 0; rail < 2U; ++rail) {
    rails[rail].reserve(persisted.rails[rail].size());
    for (const auto local : persisted.rails[rail]) {
      const auto intermediate = point(definition.origin, definition.x_axis, definition.y_axis,
                                      definition.z_axis, local.x, local.y, local.z);
      rails[rail].push_back(point(definition.origin, definition.x_axis, definition.y_axis,
                                  definition.z_axis, intermediate.x, intermediate.y,
                                  intermediate.z));
    }
  }
  std::array<std::vector<double>, 2> stations;
  for (std::size_t rail = 0; rail < 2U; ++rail) {
    stations[rail].push_back(0.0);
    for (std::size_t index = 1; index < rails[rail].size(); ++index) {
      stations[rail].push_back(stations[rail].back() +
                               length(subtract(rails[rail][index], rails[rail][index - 1U])));
    }
    if (stations[rail].back() <= 1.0e-9) return std::nullopt;
    for (auto& station : stations[rail]) station /= stations[rail].back();
  }
  std::vector<double> parameters = stations[0];
  parameters.insert(parameters.end(), stations[1].begin(), stations[1].end());
  std::sort(parameters.begin(), parameters.end());
  parameters.erase(
      std::unique(parameters.begin(), parameters.end(),
                  [](double lhs, double rhs) { return std::abs(lhs - rhs) <= 1.0e-10; }),
      parameters.end());
  const auto sample = [&](std::size_t rail, double parameter) {
    if (parameter <= 0.0) return rails[rail].front();
    for (std::size_t index = 1; index < stations[rail].size(); ++index) {
      if (parameter > stations[rail][index] + 1.0e-12) continue;
      const double span = stations[rail][index] - stations[rail][index - 1U];
      const double fraction =
          span <= 1.0e-12 ? 0.0 : (parameter - stations[rail][index - 1U]) / span;
      return add(rails[rail][index - 1U],
                 scale(subtract(rails[rail][index], rails[rail][index - 1U]), fraction));
    }
    return rails[rail].back();
  };
  std::array<std::vector<Vector3d>, 2> paired;
  for (const auto parameter : parameters) {
    paired[0].push_back(sample(0, parameter));
    paired[1].push_back(sample(1, parameter));
  }
  MeshData mesh{.object_id = object_id};
  const double half = thickness / 2.0;
  for (std::size_t index = 0; index < parameters.size(); ++index) {
    const auto midpoint = scale(add(paired[0][index], paired[1][index]), 0.5);
    const auto previous =
        index == 0U ? midpoint : scale(add(paired[0][index - 1U], paired[1][index - 1U]), 0.5);
    const auto following = index + 1U == parameters.size()
                               ? midpoint
                               : scale(add(paired[0][index + 1U], paired[1][index + 1U]), 0.5);
    const auto tangent = index == 0U                       ? subtract(following, midpoint)
                         : index + 1U == parameters.size() ? subtract(midpoint, previous)
                                                           : subtract(following, previous);
    const auto normal = normalized(cross(tangent, subtract(paired[1][index], paired[0][index])));
    if (!normal) return std::nullopt;
    const auto offset = scale(*normal, half);
    append_vertex(mesh, add(paired[0][index], offset));
    append_vertex(mesh, add(paired[1][index], offset));
    append_vertex(mesh, subtract(paired[0][index], offset));
    append_vertex(mesh, subtract(paired[1][index], offset));
  }
  const auto quad = [&](std::uint32_t a, std::uint32_t b, std::uint32_t c, std::uint32_t d) {
    mesh.indices.insert(mesh.indices.end(), {a, b, c, a, c, d});
  };
  for (std::uint32_t index = 0; index + 1U < parameters.size(); ++index) {
    const auto current = index * 4U;
    const auto following = current + 4U;
    quad(current, following, following + 1U, current + 1U);
    quad(current + 2U, current + 3U, following + 3U, following + 2U);
    quad(current + 2U, following + 2U, following, current);
    quad(current + 1U, following + 1U, following + 3U, current + 3U);
  }
  const auto last = static_cast<std::uint32_t>((parameters.size() - 1U) * 4U);
  quad(2U, 0U, 1U, 3U);
  quad(last, last + 2U, last + 3U, last + 1U);
  return mesh;
}

[[nodiscard]] std::optional<MeshData> extrude_contour(
    std::uint64_t object_id, std::span<const std::array<double, 2>> contour,
    const DefinitionGeometryView& definition, double thickness) {
  std::vector<std::array<double, 2>> normalized_contour(contour.begin(), contour.end());
  if (signed_area(normalized_contour) < 0.0) {
    std::reverse(normalized_contour.begin(), normalized_contour.end());
  }
  const auto cap = triangulate(normalized_contour);
  if (!cap || thickness <= 0.0) return std::nullopt;
  MeshData mesh;
  mesh.object_id = object_id;
  const double half = thickness / 2.0;
  for (const double z : {-half, half}) {
    for (const auto& coordinate : normalized_contour) {
      append_vertex(mesh, point(definition.origin, definition.x_axis, definition.y_axis,
                                definition.z_axis, coordinate[0], coordinate[1], z));
    }
  }
  const auto count = static_cast<std::uint32_t>(normalized_contour.size());
  for (const auto& triangle : *cap) {
    mesh.indices.insert(mesh.indices.end(), {triangle[2], triangle[1], triangle[0]});
    mesh.indices.insert(mesh.indices.end(),
                        {triangle[0] + count, triangle[1] + count, triangle[2] + count});
  }
  for (std::uint32_t index = 0; index < count; ++index) {
    const auto next = (index + 1U) % count;
    mesh.indices.insert(mesh.indices.end(),
                        {index, next, next + count, index, next + count, index + count});
  }
  return mesh;
}

[[nodiscard]] std::optional<std::vector<Vector3d>> sample_arc(Vector3d start, Vector3d end,
                                                              Vector3d bulge_axis, double radius,
                                                              std::uint32_t segment_count) {
  const Vector3d chord = subtract(end, start);
  const double chord_length = length(chord);
  if (!std::isfinite(radius) || radius <= 0.0 || segment_count == 0 || chord_length <= 1e-9 ||
      chord_length > 2.0 * radius + std::max(1e-6, 2.0 * radius * 1e-6)) {
    return std::nullopt;
  }
  const Vector3d tangent = scale(chord, 1.0 / chord_length);
  const auto bulge = normalized(subtract(bulge_axis, scale(tangent, dot(bulge_axis, tangent))));
  if (!bulge) return std::nullopt;
  const double half_chord = std::min(radius, chord_length / 2.0);
  const double half_angle = std::asin(half_chord / radius);
  const double center_offset = std::sqrt(std::max(0.0, radius * radius - half_chord * half_chord));
  const Vector3d midpoint = scale(add(start, end), 0.5);
  std::vector<Vector3d> path;
  path.reserve(static_cast<std::size_t>(segment_count) + 1U);
  for (std::uint32_t step = 0; step <= segment_count; ++step) {
    const double fraction = static_cast<double>(step) / static_cast<double>(segment_count);
    const double angle = std::numbers::pi / 2.0 + half_angle - 2.0 * half_angle * fraction;
    const double along = radius * std::cos(angle);
    const double across = radius * std::sin(angle) - center_offset;
    path.push_back(add(midpoint, add(scale(tangent, along), scale(*bulge, across))));
  }
  path.front() = start;
  path.back() = end;
  return path;
}

[[nodiscard]] std::optional<MeshData> sweep_arc(std::uint64_t object_id, const Section& section,
                                                std::span<const Vector3d> path) {
  if (path.size() < 2 || section.outer.size() < 3) return std::nullopt;
  const auto plane_normal =
      normalized(cross(subtract(path.back(), path.front()), subtract(path[1], path.front())));
  if (!plane_normal) return std::nullopt;
  const auto circle_center = [&]() -> std::optional<Vector3d> {
    if (path.size() < 3U) return std::nullopt;
    const auto first_to_middle = subtract(path[path.size() / 2U], path.front());
    const auto first_to_last = subtract(path.back(), path.front());
    const auto normal = cross(first_to_middle, first_to_last);
    const double denominator = 2.0 * dot(normal, normal);
    if (!std::isfinite(denominator) || denominator <= 1.0e-18) return std::nullopt;
    const auto offset =
        scale(add(scale(cross(first_to_last, normal), dot(first_to_middle, first_to_middle)),
                  scale(cross(normal, first_to_middle), dot(first_to_last, first_to_last))),
              1.0 / denominator);
    return add(path.front(), offset);
  }();
  if (!circle_center) return std::nullopt;
  MeshData mesh;
  mesh.object_id = object_id;
  const std::size_t outer_count = section.outer.size();
  const std::size_t inner_count = section.inner.size();
  const std::size_t ring_count = outer_count + inner_count;
  RuledSweepRecipe recipe{.object_id = object_id, .circular_spine = true};
  recipe.loops.push_back({.points = section.outer});
  if (inner_count != 0U) recipe.loops.push_back({.points = section.inner});
  recipe.stations.reserve(path.size());
  mesh.positions.reserve(path.size() * ring_count * 3U);
  for (std::size_t station = 0; station < path.size(); ++station) {
    const Vector3d direction = station == 0 ? subtract(path[1], path[0])
                               : station + 1 == path.size()
                                   ? subtract(path.back(), path[path.size() - 2U])
                                   : subtract(path[station + 1U], path[station - 1U]);
    const auto tangent = normalized(direction);
    const auto chord_transverse =
        tangent ? normalized(cross(*plane_normal, *tangent)) : std::nullopt;
    auto transverse = normalized(subtract(path[station], *circle_center));
    if (!tangent || !chord_transverse || !transverse) return std::nullopt;
    if (dot(*transverse, *chord_transverse) < 0.0) transverse = scale(*transverse, -1.0);
    recipe.stations.push_back(
        {.origin = path[station], .y_axis = *transverse, .z_axis = *plane_normal});
    for (const auto& coordinate : section.outer) {
      append_vertex(mesh, add(path[station], add(scale(*transverse, coordinate[0]),
                                                 scale(*plane_normal, coordinate[1]))));
    }
    for (const auto& coordinate : section.inner) {
      append_vertex(mesh, add(path[station], add(scale(*transverse, coordinate[0]),
                                                 scale(*plane_normal, coordinate[1]))));
    }
  }
  const auto quad = [&](std::uint32_t a, std::uint32_t b, std::uint32_t c, std::uint32_t d) {
    mesh.indices.insert(mesh.indices.end(), {a, b, c, a, c, d});
  };
  for (std::size_t station = 0; station + 1 < path.size(); ++station) {
    const auto current = static_cast<std::uint32_t>(station * ring_count);
    const auto next_ring = current + static_cast<std::uint32_t>(ring_count);
    for (std::size_t index = 0; index < outer_count; ++index) {
      const auto next = (index + 1U) % outer_count;
      quad(current + static_cast<std::uint32_t>(index), current + static_cast<std::uint32_t>(next),
           next_ring + static_cast<std::uint32_t>(next),
           next_ring + static_cast<std::uint32_t>(index));
    }
    for (std::size_t index = 0; index < inner_count; ++index) {
      const auto next = (index + 1U) % inner_count;
      const auto inner = static_cast<std::uint32_t>(outer_count);
      quad(current + inner + static_cast<std::uint32_t>(next),
           current + inner + static_cast<std::uint32_t>(index),
           next_ring + inner + static_cast<std::uint32_t>(index),
           next_ring + inner + static_cast<std::uint32_t>(next));
    }
  }
  if (section.kind == Section::Kind::solid) {
    std::vector<std::array<std::uint32_t, 3>> cap = section.cap;
    if (cap.empty()) {
      if (const auto evaluated = triangulate(section.outer)) cap = *evaluated;
    }
    const auto last = static_cast<std::uint32_t>((path.size() - 1U) * ring_count);
    for (const auto& triangle : cap) {
      mesh.indices.insert(mesh.indices.end(), {triangle[0], triangle[2], triangle[1]});
      mesh.indices.insert(mesh.indices.end(),
                          {last + triangle[0], last + triangle[1], last + triangle[2]});
    }
  } else if (outer_count == inner_count) {
    const auto last = static_cast<std::uint32_t>((path.size() - 1U) * ring_count);
    const auto inner = static_cast<std::uint32_t>(outer_count);
    for (std::size_t index = 0; index < outer_count; ++index) {
      const auto next = (index + 1U) % outer_count;
      quad(static_cast<std::uint32_t>(next), static_cast<std::uint32_t>(index),
           inner + static_cast<std::uint32_t>(index), inner + static_cast<std::uint32_t>(next));
      quad(last + static_cast<std::uint32_t>(index), last + static_cast<std::uint32_t>(next),
           last + inner + static_cast<std::uint32_t>(next),
           last + inner + static_cast<std::uint32_t>(index));
    }
  }
  mesh.ruled_sweep_recipe = std::move(recipe);
  return mesh;
}

[[nodiscard]] std::optional<MeshData> sweep_path(std::uint64_t object_id, const Section& section,
                                                 std::span<const Vector3d> path, Vector3d initial_y,
                                                 Vector3d initial_z) {
  if (path.size() < 2 || section.outer.size() < 3) return std::nullopt;
  std::vector<Vector3d> tangents;
  tangents.reserve(path.size() - 1U);
  for (std::size_t index = 0; index + 1U < path.size(); ++index) {
    const auto tangent = normalized(subtract(path[index + 1U], path[index]));
    if (!tangent) return std::nullopt;
    tangents.push_back(*tangent);
  }
  const auto first_y =
      normalized(subtract(initial_y, scale(tangents.front(), dot(initial_y, tangents.front()))));
  if (!first_y) return std::nullopt;
  auto first_z = normalized(cross(tangents.front(), *first_y));
  if (!first_z) return std::nullopt;
  if (dot(*first_z, initial_z) < 0.0) first_z = scale(*first_z, -1.0);

  MeshData mesh;
  mesh.object_id = object_id;
  const std::size_t outer_count = section.outer.size();
  const std::size_t inner_count = section.inner.size();
  const std::size_t ring_count = outer_count + inner_count;
  RuledSweepRecipe recipe{.object_id = object_id};
  recipe.loops.push_back({.points = section.outer});
  if (inner_count != 0U) recipe.loops.push_back({.points = section.inner});
  recipe.stations.reserve(path.size());
  mesh.positions.reserve(path.size() * ring_count * 3U);
  Vector3d previous_y = *first_y;
  Vector3d previous_z = *first_z;
  for (std::size_t station = 0; station < path.size(); ++station) {
    Vector3d tangent = station == 0U ? tangents.front()
                       : station + 1U == path.size()
                           ? tangents.back()
                           : add(tangents[station - 1U], tangents[station]);
    const auto unit_tangent = normalized(tangent);
    if (!unit_tangent) return std::nullopt;
    const auto transported_y =
        normalized(subtract(previous_y, scale(*unit_tangent, dot(previous_y, *unit_tangent))));
    if (!transported_y) return std::nullopt;
    auto transported_z = normalized(cross(*unit_tangent, *transported_y));
    if (!transported_z) return std::nullopt;
    Vector3d y_axis = *transported_y;
    Vector3d z_axis = *transported_z;
    if (dot(z_axis, previous_z) < 0.0) {
      y_axis = scale(y_axis, -1.0);
      z_axis = scale(z_axis, -1.0);
    }
    recipe.stations.push_back({.origin = path[station], .y_axis = y_axis, .z_axis = z_axis});
    for (const auto& coordinate : section.outer) {
      append_vertex(mesh, add(path[station],
                              add(scale(y_axis, coordinate[0]), scale(z_axis, coordinate[1]))));
    }
    for (const auto& coordinate : section.inner) {
      append_vertex(mesh, add(path[station],
                              add(scale(y_axis, coordinate[0]), scale(z_axis, coordinate[1]))));
    }
    previous_y = y_axis;
    previous_z = z_axis;
  }
  const auto quad = [&](std::uint32_t a, std::uint32_t b, std::uint32_t c, std::uint32_t d) {
    mesh.indices.insert(mesh.indices.end(), {a, b, c, a, c, d});
  };
  for (std::size_t station = 0; station + 1U < path.size(); ++station) {
    const auto current = static_cast<std::uint32_t>(station * ring_count);
    const auto next_ring = current + static_cast<std::uint32_t>(ring_count);
    for (std::size_t index = 0; index < outer_count; ++index) {
      const auto next = (index + 1U) % outer_count;
      quad(current + static_cast<std::uint32_t>(index), current + static_cast<std::uint32_t>(next),
           next_ring + static_cast<std::uint32_t>(next),
           next_ring + static_cast<std::uint32_t>(index));
    }
    for (std::size_t index = 0; index < inner_count; ++index) {
      const auto next = (index + 1U) % inner_count;
      const auto inner = static_cast<std::uint32_t>(outer_count);
      quad(current + inner + static_cast<std::uint32_t>(next),
           current + inner + static_cast<std::uint32_t>(index),
           next_ring + inner + static_cast<std::uint32_t>(index),
           next_ring + inner + static_cast<std::uint32_t>(next));
    }
  }
  if (section.kind == Section::Kind::solid) {
    std::vector<std::array<std::uint32_t, 3>> cap = section.cap;
    if (cap.empty()) {
      if (const auto evaluated = triangulate(section.outer)) cap = *evaluated;
    }
    const auto last = static_cast<std::uint32_t>((path.size() - 1U) * ring_count);
    for (const auto& triangle : cap) {
      mesh.indices.insert(mesh.indices.end(), {triangle[0], triangle[2], triangle[1]});
      mesh.indices.insert(mesh.indices.end(),
                          {last + triangle[0], last + triangle[1], last + triangle[2]});
    }
  } else if (outer_count == inner_count) {
    const auto last = static_cast<std::uint32_t>((path.size() - 1U) * ring_count);
    const auto inner = static_cast<std::uint32_t>(outer_count);
    for (std::size_t index = 0; index < outer_count; ++index) {
      const auto next = (index + 1U) % outer_count;
      quad(static_cast<std::uint32_t>(next), static_cast<std::uint32_t>(index),
           inner + static_cast<std::uint32_t>(index), inner + static_cast<std::uint32_t>(next));
      quad(last + static_cast<std::uint32_t>(index), last + static_cast<std::uint32_t>(next),
           last + inner + static_cast<std::uint32_t>(next),
           last + inner + static_cast<std::uint32_t>(index));
    }
  }
  mesh.ruled_sweep_recipe = std::move(recipe);
  return mesh;
}

[[nodiscard]] std::vector<Vector3d> section_path(std::span<const Vector3d> reference_path,
                                                 Vector3d section_origin) {
  std::vector<Vector3d> result(reference_path.begin(), reference_path.end());
  if (result.empty()) return result;
  const auto translation = subtract(section_origin, result.front());
  for (auto& station : result) station = add(station, translation);
  return result;
}

[[nodiscard]] const TableSchema* populated_table(const ModelStorage& storage, const Schema& schema,
                                                 std::span<const std::string_view> names) noexcept {
  for (const auto name : names) {
    const auto* candidate = schema.find_table(name);
    if (candidate != nullptr && candidate->ordinal < storage.layout.tables.size() &&
        storage.layout.tables[candidate->ordinal].info.row_count != 0)
      return candidate;
  }
  return nullptr;
}

class GeometryReader final : public BatchReader {
 public:
  GeometryReader(std::shared_ptr<const ModelStorage> storage, const TableLayout& parts,
                 const TableSchema& part_schema,
                 std::unordered_map<std::uint32_t, Attribute> attributes,
                 std::unordered_map<std::uint32_t, Axes> axes,
                 std::unordered_map<std::uint32_t, CoordinateSystem> coordinate_systems,
                 std::unordered_map<std::uint32_t, Vector3d> points,
                 std::unordered_map<std::uint32_t, Chamfer> chamfers,
                 std::unordered_set<std::uint32_t> boolean_operatives,
                 std::unordered_map<std::uint32_t, std::vector<GeometryOperation>> operations,
                 std::unordered_map<std::uint32_t, std::uint64_t> part_rows,
                 std::unordered_map<std::uint32_t, Contour> contours,
                 std::unordered_map<std::uint32_t, Contour> paths,
                 std::unordered_map<std::uint32_t, LoftRails> loft_rails,
                 LocalProfileCatalog profiles, ShapeCatalog shapes,
                 std::array<std::uint32_t, 9> offsets, bool emit_definitions, bool emit_meshes,
                 std::size_t batch_size, TopologyMode topology_mode,
                 std::string topology_worker_path, std::uint32_t topology_timeout_milliseconds,
                 std::uint64_t object_id_min, std::uint64_t object_id_max)
      : storage_(std::move(storage)),
        parts_(&parts),
        part_schema_(&part_schema),
        attributes_(std::move(attributes)),
        axes_(std::move(axes)),
        coordinate_systems_(std::move(coordinate_systems)),
        points_(std::move(points)),
        chamfers_(std::move(chamfers)),
        boolean_operatives_(std::move(boolean_operatives)),
        operations_(std::move(operations)),
        part_rows_(std::move(part_rows)),
        contours_(std::move(contours)),
        paths_(std::move(paths)),
        loft_rails_(std::move(loft_rails)),
        profiles_(std::move(profiles)),
        shapes_(std::move(shapes)),
        offsets_(offsets),
        emit_definitions_(emit_definitions),
        emit_meshes_(emit_meshes),
        batch_size_(batch_size),
        topology_mode_(topology_mode),
        object_id_min_(object_id_min),
        object_id_max_(object_id_max) {
#if defined(TEKLA_DB1_HAS_OCCT)
    if (topology_mode_ == TopologyMode::supervised && !topology_worker_path.empty()) {
      supervised_host_ = std::make_unique<SupervisedOcctHost>(std::move(topology_worker_path),
                                                              topology_timeout_milliseconds);
    }
#else
    (void)topology_worker_path;
    (void)topology_timeout_milliseconds;
#endif
    definitions_.reserve(batch_size_);
    definition_paths_.reserve(batch_size_);
    mesh_data_.reserve(batch_size_);
    mesh_positions_.reserve(batch_size_);
    meshes_.reserve(batch_size_);
    diagnostics_.reserve(batch_size_);
  }

  Result<BatchView> next() override {
    if (pending_ == 1) {
      pending_ = 2;
      if (!meshes_.empty())
        return Result<BatchView>::success(BatchView{.kind = BatchKind::meshes, .meshes = meshes_});
    }
    if (pending_ <= 2) {
      pending_ = 3;
      if (!diagnostics_.empty())
        return Result<BatchView>::success(
            BatchView{.kind = BatchKind::diagnostics, .diagnostics = diagnostics_});
    }
    if (row_ >= parts_->info.row_count) {
      return Result<BatchView>::success(BatchView{.kind = BatchKind::end});
    }
    pending_ = 1;
    definitions_.clear();
    definition_paths_.clear();
    mesh_data_.clear();
    extrusion_recipes_.clear();
    mesh_positions_.clear();
    meshes_.clear();
    diagnostics_.clear();
    diagnostic_messages_.clear();
    const auto payload = storage_->payload.bytes();
    std::size_t count = 0;
    while (row_ < parts_->info.row_count && count < batch_size_) {
      const auto record = parts_->record(payload, row_++);
      if (record.empty())
        return Result<BatchView>::failure(
            {ErrorCode::invalid_container, "A geometry part lies outside the payload."});
      const auto tuple = record.subspan(1, part_schema_->tuple_size);
      const auto object_id = read_u32(tuple, offsets_[0]);
      if (object_id < object_id_min_ || object_id > object_id_max_) {
        ++count;
        continue;
      }
      const bool boolean_operative = boolean_operatives_.contains(object_id);
      const bool emit_boolean_operative =
          boolean_operative && std::getenv("TEKLA_DB1_EMIT_BOOLEAN_OPERATIVES") != nullptr;
      if (boolean_operative && !emit_boolean_operative) {
        ++count;
        continue;
      }
      const auto attribute = attributes_.find(read_u32(tuple, offsets_[1]));
      const auto basis = axes_.find(read_u32(tuple, offsets_[2]));
      const double extrusion_length = read_f64(tuple, offsets_[6]);
      if (attribute != attributes_.end() && attribute->second.object_type != 2U &&
          !emit_boolean_operative) {
        ++count;
        continue;
      }
      if (attribute == attributes_.end() || basis == axes_.end() ||
          !std::isfinite(extrusion_length) || extrusion_length <= 0.0) {
        diagnostics_.push_back({ErrorCode::invalid_geometry, object_id,
                                "A straight part has incomplete frame or profile data."});
        ++count;
        continue;
      }
      auto x_axis = normalized(basis->second.x);
      auto y_axis = normalized(basis->second.y);
      if (x_axis && !y_axis && centered_square_profile(attribute->second.profile))
        y_axis = fallback_square_y_axis(*x_axis);
      auto z_axis = x_axis && y_axis ? normalized(cross(*x_axis, *y_axis)) : std::nullopt;
      if (!x_axis || !y_axis || !z_axis) {
        diagnostics_.push_back({ErrorCode::invalid_geometry, object_id,
                                "A straight part coordinate system is degenerate."});
        ++count;
        continue;
      }
      DefinitionGeometryView definition{
          .object_id = object_id,
          .kind = DefinitionGeometryKind::straight_extrusion,
          .profile = attribute->second.profile,
          .origin = {read_f64(tuple, offsets_[3]), read_f64(tuple, offsets_[4]),
                     read_f64(tuple, offsets_[5])},
          .x_axis = *x_axis,
          .y_axis = *y_axis,
          .z_axis = *z_axis,
          .length = extrusion_length,
          .form_type = attribute->second.form_type,
          .has_operations = operations_.contains(object_id)};
      definition.report_length = extrusion_length;
      definition.has_report_length = true;
      definition.catalog_report_scalars_eligible = true;
      if (const auto attached = operations_.find(object_id);
          attached != operations_.end() && !attached->second.empty()) {
        for (const auto& operation : attached->second) {
          if (operation.type != 9U) {
            definition.catalog_report_scalars_eligible = false;
          }
        }
      }
      const auto arc = legacy_arc(attribute->second.object_class);
      const bool imported_shape =
          attribute->second.form_type == 105U || attribute->second.form_type == 115U;
      if (imported_shape) {
        definition.kind = DefinitionGeometryKind::imported_shape;
      } else if (attribute->second.form_type == 8U) {
        definition.kind = DefinitionGeometryKind::lofted_plate;
      } else if (arc) {
        definition.kind = DefinitionGeometryKind::circular_arc_extrusion;
        definition.radius = arc->radius;
        definition.segment_count = arc->segment_count;
      } else if (attribute->second.form_type == 4U || attribute->second.form_type == 44U ||
                 attribute->second.form_type == 64U || attribute->second.form_type == 74U) {
        definition.kind = DefinitionGeometryKind::polyline_extrusion;
        const auto persisted = paths_.find(read_u32(tuple, offsets_[8]));
        const auto reference = points_.find(read_u32(tuple, offsets_[7]));
        if (persisted != paths_.end() && reference != points_.end()) {
          definition_paths_.emplace_back();
          auto& path = definition_paths_.back();
          path.reserve(persisted->second.points.size());
          for (const auto delta : persisted->second.points) {
            path.push_back(add(reference->second, delta));
          }
          definition.path = path;
        }
      }
      definitions_.push_back(definition);
      if (emit_meshes_) {
        const auto mesh_count_before = mesh_data_.size();
        if (imported_shape) {
          Error shape_error;
          if (const auto* shape = shapes_.resolve(definition.profile, shape_error)) {
            mesh_data_.push_back(instantiate_shape(object_id, *shape, definition));
          } else {
            add_diagnostic(shape_error.code, object_id, std::move(shape_error.message));
          }
        } else if (attribute->second.form_type == 8U) {
          const auto persisted = loft_rails_.find(read_u32(tuple, offsets_[8]));
          const auto thickness = number(definition.profile);
          const auto mesh = persisted != loft_rails_.end() && thickness
                                ? loft_plate(object_id, persisted->second, definition, *thickness)
                                : std::nullopt;
          if (mesh) {
            mesh_data_.push_back(std::move(*mesh));
          } else {
            diagnostics_.push_back({ErrorCode::invalid_geometry, object_id,
                                    "The persisted lofted-plate definition is invalid."});
          }
        } else if (attribute->second.form_type == 2U || attribute->second.form_type == 62U ||
                   attribute->second.form_type == 82U) {
          const auto persisted = contours_.find(read_u32(tuple, offsets_[8]));
          const auto thickness = plate_thickness(definition.profile);
          const auto contour = persisted == contours_.end()
                                   ? std::optional<std::vector<std::array<double, 2>>>{}
                                   : chamfered(persisted->second);
          const auto mesh = contour && thickness
                                ? extrude_contour(object_id, *contour, definition, *thickness)
                                : std::nullopt;
          if (mesh) {
            extrusion_recipes_.insert_or_assign(
                object_id, contour_extrusion_recipe(object_id, *contour, definition, *thickness));
            mesh_data_.push_back(std::move(*mesh));
          } else {
            diagnostics_.push_back({ErrorCode::invalid_geometry, object_id,
                                    "The persisted contour-plate definition is invalid."});
          }
        } else if (auto variable = variable_rectangle(definition.profile);
                   (attribute->second.form_type == 51U || attribute->second.form_type == 61U) &&
                   variable) {
          set_tapered_section_metrics(definitions_.back(), *variable);
          mesh_data_.push_back(loft(object_id, *variable, definition));
        } else if (const auto tapered_i = tapered_i_parameters(uppercase(definition.profile))) {
          const auto reference = points_.find(read_u32(tuple, offsets_[7]));
          const Vector3d delta = reference == points_.end()
                                     ? Vector3d{}
                                     : subtract(reference->second, definition.origin);
          const double anchor = reference == points_.end() ? -tapered_i->start_height / 2.0
                                                           : dot(delta, definition.y_axis);
          const double width_center =
              reference == points_.end() ? 0.0 : dot(delta, definition.z_axis);
          TaperedSection tapered{
              .start = tapered_i_contour(*tapered_i, tapered_i->start_height, anchor, width_center),
              .end = tapered_i_contour(*tapered_i, tapered_i->end_height, anchor, width_center)};
          set_tapered_section_metrics(definitions_.back(), tapered);
          mesh_data_.push_back(loft(object_id, tapered, definition));
        } else if (auto tapered = tapered_ellipse(definition.profile)) {
          set_tapered_section_metrics(definitions_.back(), *tapered);
          mesh_data_.push_back(loft(object_id, *tapered, definition));
        } else if (auto analytic = parse_section(definition.profile);
                   analytic || profiles_.find(definition.profile) != nullptr) {
          Section section;
          const auto* catalog = profiles_.find(definition.profile);
          if (analytic) {
            section = std::move(*analytic);
          } else {
            section.kind = catalog->hollow ? Section::Kind::hollow : Section::Kind::solid;
            section.outer = catalog->outer;
            section.inner = catalog->inner;
          }
          if (emit_boolean_operative && !section.inner.empty()) {
            section.kind = Section::Kind::solid;
            section.inner.clear();
            section.cap.clear();
          }
          set_section_metrics(definitions_.back(), section);
          set_fitted_report_length(object_id, definitions_.back(), section);
          if (catalog) {
            set_report_section_metrics(definitions_.back(), *catalog);
          } else if (const auto metrics = standard_profile_metrics(definition.profile)) {
            set_report_section_metrics(definitions_.back(), *metrics);
            // The built-in fallback is restricted to standardized rolled
            // sections whose persisted Report values remain gross catalog
            // scalars even when component relations are attached. Model-local
            // arbitrary profiles keep the stricter fitting-only policy.
            definitions_.back().catalog_report_scalars_eligible = true;
          }
          if (definition.kind == DefinitionGeometryKind::polyline_extrusion) {
            const auto display_path = section_path(definition.path, definition.origin);
            const auto mesh =
                sweep_path(object_id, section, display_path, definition.y_axis, definition.z_axis);
            if (mesh) {
              mesh_data_.push_back(std::move(*mesh));
            } else {
              diagnostics_.push_back({ErrorCode::invalid_geometry, object_id,
                                      "The persisted polybeam path or sweep is invalid."});
            }
          } else if (arc) {
            const auto path =
                sample_arc(definition.origin,
                           add(definition.origin, scale(definition.x_axis, definition.length)),
                           definition.y_axis, arc->radius, arc->segment_count);
            const auto mesh = path ? sweep_arc(object_id, section, *path) : std::nullopt;
            if (mesh) {
              mesh_data_.push_back(std::move(*mesh));
            } else {
              diagnostics_.push_back({ErrorCode::invalid_geometry, object_id,
                                      "The legacy curved-beam arc or sweep is invalid."});
            }
          } else {
            extrusion_recipes_.insert_or_assign(object_id,
                                                extrusion_recipe(object_id, section, definition));
            mesh_data_.push_back(extrude(object_id, section, definition));
          }
        } else {
          diagnostics_.push_back(
              {ErrorCode::decoder_unavailable, object_id,
               "The profile section is not available to the analytic evaluator."});
        }
        if (mesh_data_.size() != mesh_count_before) {
          apply_operations(object_id, mesh_data_.back());
          mesh_data_.back().longitudinal_axis = definition.x_axis;
        }
      }
      ++count;
    }
    for (const auto& mesh : mesh_data_) {
      auto& positions = mesh_positions_.emplace_back();
      positions.reserve(mesh.positions.size());
      for (const auto value : mesh.positions) {
        positions.push_back(static_cast<float>(value));
      }
      const auto metrics = report_metrics(mesh, mesh.longitudinal_axis);
      meshes_.push_back(
          MeshView{.object_id = mesh.object_id,
                   .positions = positions,
                   .indices = mesh.indices,
                   .surface_area = metrics.surface_area,
                   .cover_surface_area = metrics.cover_surface_area,
                   .volume = metrics.volume,
                   .longitudinal_min = metrics.longitudinal_min,
                   .longitudinal_max = metrics.longitudinal_max,
                   .section_y_min = metrics.section_y_min,
                   .section_y_max = metrics.section_y_max,
                   .section_z_min = metrics.section_z_min,
                   .section_z_max = metrics.section_z_max,
                   .has_report_metrics = metrics.valid,
                   .has_cover_surface_area = metrics.valid && metrics.has_cover_surface_area,
                   .has_section_extents = metrics.valid && metrics.has_section_extents});
    }
    if (emit_definitions_ && !definitions_.empty())
      return Result<BatchView>::success(
          BatchView{.kind = BatchKind::definition_geometry, .definitions = definitions_});
    if (!meshes_.empty()) {
      pending_ = 2;
      return Result<BatchView>::success(BatchView{.kind = BatchKind::meshes, .meshes = meshes_});
    }
    pending_ = 3;
    if (!diagnostics_.empty())
      return Result<BatchView>::success(
          BatchView{.kind = BatchKind::diagnostics, .diagnostics = diagnostics_});
    return next();
  }

 private:
  void set_fitted_report_length(std::uint32_t object_id, DefinitionGeometryView& definition,
                                const Section& section) const {
    const auto attached = operations_.find(object_id);
    if (attached == operations_.end() || attached->second.empty() || section.outer.size() < 3U) {
      return;
    }

    struct FittingPlane {
      Vector3d origin;
      Vector3d normal;
      GeometryOperation operation;
    };
    std::vector<FittingPlane> planes;
    const auto midpoint = add(definition.origin, scale(definition.x_axis, definition.length / 2.0));
    for (const auto& operation : attached->second) {
      if (operation.type != 9U) continue;
      const auto coordinate_system = coordinate_systems_.find(operation.target_id);
      if (coordinate_system == coordinate_systems_.end()) continue;
      const auto operation_axes = axes_.find(coordinate_system->second.axes_id);
      if (operation_axes == axes_.end()) continue;
      auto normal = normalized(cross(operation_axes->second.x, operation_axes->second.y));
      if (!normal) continue;
      if (dot(subtract(midpoint, coordinate_system->second.origin), *normal) < 0.0) {
        normal = scale(*normal, -1.0);
      }
      FittingPlane candidate{coordinate_system->second.origin, *normal, operation};
      bool replaced = false;
      for (auto& existing : planes) {
        if (dot(existing.normal, *normal) < 1.0 - 1.0e-8) continue;
        if (std::pair{operation.event_id, operation.relation_id} >
            std::pair{existing.operation.event_id, existing.operation.relation_id}) {
          existing = candidate;
        }
        replaced = true;
        break;
      }
      if (!replaced) planes.push_back(candidate);
    }
    if (planes.empty()) return;

    if (section.circular_outer_radius > 0.0) {
      const bool has_start_fitting =
          std::any_of(planes.begin(), planes.end(), [&](const FittingPlane& plane) {
            return dot(definition.x_axis, plane.normal) > 1.0e-12;
          });
      const bool has_end_fitting =
          std::any_of(planes.begin(), planes.end(), [&](const FittingPlane& plane) {
            return dot(definition.x_axis, plane.normal) < -1.0e-12;
          });
      double interval_start = has_start_fitting ? -std::numeric_limits<double>::infinity() : 0.0;
      double interval_end =
          has_end_fitting ? std::numeric_limits<double>::infinity() : definition.length;
      for (const auto& plane : planes) {
        const double denominator = dot(definition.x_axis, plane.normal);
        if (std::abs(denominator) <= 1.0e-12) continue;
        const double center_station =
            dot(subtract(plane.origin, definition.origin), plane.normal) / denominator;
        const double transverse_support =
            section.circular_outer_radius *
            std::hypot(dot(definition.y_axis, plane.normal), dot(definition.z_axis, plane.normal)) /
            std::abs(denominator);
        if (denominator > 0.0) {
          interval_start = std::max(interval_start, center_station - transverse_support);
        } else {
          interval_end = std::min(interval_end, center_station + transverse_support);
        }
      }
      if (std::isfinite(interval_start) && std::isfinite(interval_end) &&
          interval_end > interval_start) {
        definition.report_length = interval_end - interval_start;
        definition.has_report_length = true;
      }
      return;
    }

    // A fitting keeps the half-space containing the nominal member midpoint.
    // For every section boundary point, intersect its longitudinal generator
    // with all active fitting half-spaces. The extrema of the surviving
    // generators give the fitted-solid length without quantising through
    // the float display mesh. With the persisted fitting model there is at
    // most one active plane for each oriented normal (duplicates above are
    // reduced by event/relation order), so extrema occur on the section
    // boundary vertices represented by the analytic/catalog contour.
    double longitudinal_min = std::numeric_limits<double>::infinity();
    double longitudinal_max = -std::numeric_limits<double>::infinity();
    const bool has_start_fitting = std::any_of(
        planes.begin(), planes.end(),
        [&](const FittingPlane& plane) { return dot(definition.x_axis, plane.normal) > 1.0e-12; });
    const bool has_end_fitting = std::any_of(
        planes.begin(), planes.end(),
        [&](const FittingPlane& plane) { return dot(definition.x_axis, plane.normal) < -1.0e-12; });
    for (const auto& coordinate : section.outer) {
      double interval_start = has_start_fitting ? -std::numeric_limits<double>::infinity() : 0.0;
      double interval_end =
          has_end_fitting ? std::numeric_limits<double>::infinity() : definition.length;
      for (const auto& plane : planes) {
        const double denominator = dot(definition.x_axis, plane.normal);
        if (std::abs(denominator) <= 1.0e-12) continue;
        const double numerator = dot(subtract(plane.origin, definition.origin), plane.normal) -
                                 coordinate[0] * dot(definition.y_axis, plane.normal) -
                                 coordinate[1] * dot(definition.z_axis, plane.normal);
        const double station = numerator / denominator;
        if (denominator > 0.0) {
          interval_start = std::max(interval_start, station);
        } else {
          interval_end = std::min(interval_end, station);
        }
      }
      if (interval_end + 1.0e-9 < interval_start) continue;
      longitudinal_min = std::min(longitudinal_min, interval_start);
      longitudinal_max = std::max(longitudinal_max, interval_end);
    }
    if (std::isfinite(longitudinal_min) && std::isfinite(longitudinal_max) &&
        longitudinal_max > longitudinal_min) {
      definition.report_length = longitudinal_max - longitudinal_min;
      definition.has_report_length = true;
    }
  }

  [[nodiscard]] std::optional<MeshData> operative_mesh(std::uint32_t object_id,
                                                       double cutter_extension = 0.0,
                                                       bool fill_envelope = true) {
    const auto row = part_rows_.find(object_id);
    if (row == part_rows_.end()) return std::nullopt;
    const auto record = parts_->record(storage_->payload.bytes(), row->second);
    if (record.empty()) return std::nullopt;
    const auto tuple = record.subspan(1, part_schema_->tuple_size);
    const auto attribute = attributes_.find(read_u32(tuple, offsets_[1]));
    const auto basis = axes_.find(read_u32(tuple, offsets_[2]));
    const double extrusion_length = read_f64(tuple, offsets_[6]);
    if (attribute == attributes_.end() || basis == axes_.end() ||
        !std::isfinite(extrusion_length) || extrusion_length <= 0.0) {
      return std::nullopt;
    }
    const auto x_axis = normalized(basis->second.x);
    auto y_axis = normalized(basis->second.y);
    if (x_axis && !y_axis && centered_square_profile(attribute->second.profile))
      y_axis = fallback_square_y_axis(*x_axis);
    const auto z_axis = x_axis && y_axis ? normalized(cross(*x_axis, *y_axis)) : std::nullopt;
    if (!x_axis || !y_axis || !z_axis) return std::nullopt;
    DefinitionGeometryView definition{
        .object_id = object_id,
        .profile = attribute->second.profile,
        .origin = {read_f64(tuple, offsets_[3]), read_f64(tuple, offsets_[4]),
                   read_f64(tuple, offsets_[5])},
        .x_axis = *x_axis,
        .y_axis = *y_axis,
        .z_axis = *z_axis,
        .length = extrusion_length,
        .form_type = attribute->second.form_type};
    // Stored operative parts are frequently tangent to the host surface. A
    // microscopic through-extension prevents an exact shared face from being
    // classified as a no-op without changing model-scale bounds.
    const bool imported_shape =
        attribute->second.form_type == 105U || attribute->second.form_type == 115U;
    if (imported_shape) {
      Error error;
      const auto* shape = shapes_.resolve(definition.profile, error);
      if (shape == nullptr) return std::nullopt;
      return instantiate_shape(object_id, *shape, definition);
    }
    if (attribute->second.form_type == 2U || attribute->second.form_type == 62U ||
        attribute->second.form_type == 82U) {
      const auto persisted = contours_.find(read_u32(tuple, offsets_[8]));
      const auto thickness = plate_thickness(definition.profile);
      const auto contour = persisted == contours_.end()
                               ? std::optional<std::vector<std::array<double, 2>>>{}
                               : chamfered(persisted->second);
      return contour && thickness ? extrude_contour(object_id, *contour, definition,
                                                    *thickness + 2.0 * cutter_extension)
                                  : std::nullopt;
    }
    definition.origin = subtract(definition.origin, scale(definition.x_axis, cutter_extension));
    definition.length += 2.0 * cutter_extension;
    if (auto variable = variable_rectangle(definition.profile);
        (attribute->second.form_type == 51U || attribute->second.form_type == 61U) && variable) {
      return loft(object_id, *variable, definition);
    }
    if (auto tapered = tapered_ellipse(definition.profile)) {
      return loft(object_id, *tapered, definition);
    }
    auto analytic = parse_section(definition.profile);
    Section section;
    if (analytic) {
      section = std::move(*analytic);
    } else if (const auto* catalog = profiles_.find(definition.profile)) {
      section.kind = catalog->hollow ? Section::Kind::hollow : Section::Kind::solid;
      section.outer = catalog->outer;
      section.inner = catalog->inner;
    } else {
      return std::nullopt;
    }
    // Tekla evaluates a BooleanPart as the filled outer envelope of its
    // operative profile. Visible hollow members retain their inner loop, but
    // subtracting only that material shell leaves false end fragments where
    // Tekla produces a clean fitted cut.
    if (fill_envelope && !section.inner.empty()) {
      section.kind = Section::Kind::solid;
      section.inner.clear();
      section.cap.clear();
    }
    if (attribute->second.form_type == 4U || attribute->second.form_type == 44U ||
        attribute->second.form_type == 64U || attribute->second.form_type == 74U) {
      const auto persisted = paths_.find(read_u32(tuple, offsets_[8]));
      const auto reference = points_.find(read_u32(tuple, offsets_[7]));
      if (persisted == paths_.end() || reference == points_.end()) return std::nullopt;
      std::vector<Vector3d> path;
      path.reserve(persisted->second.points.size());
      for (const auto delta : persisted->second.points) {
        path.push_back(add(reference->second, delta));
      }
      path = section_path(path, definition.origin);
      return sweep_path(object_id, section, path, definition.y_axis, definition.z_axis);
    }
    if (const auto arc = legacy_arc(attribute->second.object_class)) {
      const auto path = sample_arc(
          definition.origin, add(definition.origin, scale(definition.x_axis, definition.length)),
          definition.y_axis, arc->radius, arc->segment_count);
      return path ? sweep_arc(object_id, section, *path) : std::nullopt;
    }
    return extrude(object_id, section, definition);
  }

  [[nodiscard]] static double mesh_volume(const MeshData& mesh) {
    if (mesh.positions.size() < 3U || mesh.indices.size() < 3U) return 0.0;
    const Vector3d reference{mesh.positions[0], mesh.positions[1], mesh.positions[2]};
    double signed_volume = 0.0;
    for (std::size_t index = 0; index + 2U < mesh.indices.size(); index += 3U) {
      const auto vertex = [&](std::uint32_t vertex_index) {
        const std::size_t offset = static_cast<std::size_t>(vertex_index) * 3U;
        return subtract(Vector3d{mesh.positions[offset], mesh.positions[offset + 1U],
                                 mesh.positions[offset + 2U]},
                        reference);
      };
      const auto first = vertex(mesh.indices[index]);
      const auto second = vertex(mesh.indices[index + 1U]);
      const auto third = vertex(mesh.indices[index + 2U]);
      signed_volume += dot(first, cross(second, third)) / 6.0;
    }
    return std::abs(signed_volume);
  }

  void expand_tangent_cutter(std::uint32_t host_id, std::uint32_t cutter_id,
                             const MeshData& host_mesh, MeshData& cutter_mesh,
                             double through_interval_extension) const {
    const auto cutter_row = part_rows_.find(cutter_id);
    const auto host_row = part_rows_.find(host_id);
    if (cutter_row == part_rows_.end() || host_row == part_rows_.end() ||
        host_mesh.positions.empty() || cutter_mesh.positions.empty()) {
      return;
    }
    const auto record = parts_->record(storage_->payload.bytes(), cutter_row->second);
    if (record.empty()) return;
    const auto tuple = record.subspan(1, part_schema_->tuple_size);
    const auto attribute = attributes_.find(read_u32(tuple, offsets_[1]));
    const auto basis = axes_.find(read_u32(tuple, offsets_[2]));
    if (attribute == attributes_.end() || basis == axes_.end()) return;
    const auto form_type = attribute->second.form_type;
    bool single_loop_prism = false;
    if (form_type == 2U || form_type == 62U || form_type == 82U) {
      single_loop_prism = contours_.contains(read_u32(tuple, offsets_[8]));
    } else if (form_type != 4U && form_type != 44U && form_type != 64U && form_type != 74U &&
               form_type != 105U && form_type != 115U &&
               !tapered_ellipse(attribute->second.profile)) {
      if (const auto section = parse_section(attribute->second.profile)) {
        // BooleanPart geometry is constructed from the filled outer envelope,
        // even when the persisted member profile is hollow.  Its cutter mesh
        // is therefore a single-loop prism and must receive the same tangent
        // tolerance treatment as an intrinsically solid section.
        single_loop_prism = section->outer.size() >= 3U;
      } else if (const auto* catalog = profiles_.find(attribute->second.profile)) {
        single_loop_prism = catalog->outer.size() >= 3U;
      }
    }
    if (!single_loop_prism) return;
    const auto cutter_x = normalized(basis->second.x);
    const auto cutter_y = normalized(basis->second.y);
    const auto cutter_z =
        cutter_x && cutter_y ? normalized(cross(*cutter_x, *cutter_y)) : std::nullopt;
    if (!cutter_x || !cutter_y || !cutter_z) return;

    const Vector3d cutter_origin{read_f64(tuple, offsets_[3]), read_f64(tuple, offsets_[4]),
                                 read_f64(tuple, offsets_[5])};
    const auto host_record = parts_->record(storage_->payload.bytes(), host_row->second);
    if (host_record.empty()) return;
    const auto host_tuple = host_record.subspan(1, part_schema_->tuple_size);
    const auto host_basis = axes_.find(read_u32(host_tuple, offsets_[2]));
    if (host_basis == axes_.end()) return;
    const auto host_x = normalized(host_basis->second.x);
    const auto host_y = normalized(host_basis->second.y);
    const auto host_z = host_x && host_y ? normalized(cross(*host_x, *host_y)) : std::nullopt;
    if (!host_x || !host_y || !host_z) return;
    const std::array<Vector3d, 3> host_axes{*host_x, *host_y, *host_z};
    const Vector3d host_origin{read_f64(host_tuple, offsets_[3]), read_f64(host_tuple, offsets_[4]),
                               read_f64(host_tuple, offsets_[5])};

    std::array<std::array<double, 2>, 3> host_bounds;
    std::array<std::array<double, 2>, 3> cutter_host_bounds;
    for (auto& bounds : host_bounds) {
      bounds = {std::numeric_limits<double>::infinity(), -std::numeric_limits<double>::infinity()};
    }
    cutter_host_bounds = host_bounds;
    const auto update_bounds = [&](const MeshData& source,
                                   std::array<std::array<double, 2>, 3>& bounds) {
      for (std::size_t index = 0; index + 2U < source.positions.size(); index += 3U) {
        const Vector3d point{source.positions[index], source.positions[index + 1U],
                             source.positions[index + 2U]};
        for (std::size_t axis = 0; axis < host_axes.size(); ++axis) {
          const double coordinate = dot(subtract(point, host_origin), host_axes[axis]);
          bounds[axis][0] = std::min(bounds[axis][0], coordinate);
          bounds[axis][1] = std::max(bounds[axis][1], coordinate);
        }
      }
    };
    update_bounds(host_mesh, host_bounds);
    update_bounds(cutter_mesh, cutter_host_bounds);
    constexpr double tolerance = 1.0e-2;
    const bool crosses_host_end = (cutter_host_bounds[0][0] < host_bounds[0][0] - tolerance &&
                                   cutter_host_bounds[0][1] > host_bounds[0][0] + tolerance) ||
                                  (cutter_host_bounds[0][0] < host_bounds[0][1] - tolerance &&
                                   cutter_host_bounds[0][1] > host_bounds[0][1] + tolerance);

    std::size_t aligned_index = 0U;
    double aligned_magnitude = 0.0;
    for (std::size_t axis = 0; axis < host_axes.size(); ++axis) {
      const double candidate = std::abs(dot(*cutter_x, host_axes[axis]));
      if (candidate > aligned_magnitude) {
        aligned_magnitude = candidate;
        aligned_index = axis;
      }
    }

    const double persisted_length = read_f64(tuple, offsets_[6]);
    if (!std::isfinite(persisted_length) || persisted_length <= 0.0) return;
    const auto station = [&](Vector3d point) {
      return dot(subtract(point, cutter_origin), *cutter_x);
    };

    // Rebuild ordinary single-loop prisms from their analytic section after
    // applying tolerance envelopes. Moving the two tessellated end rings
    // independently creates a microscopically warped cutter whenever the
    // persisted and host axes are only approximately parallel; OCCT can then
    // classify a whole opening differently at large world coordinates.
    const bool contour_plate = form_type == 2U || form_type == 62U || form_type == 82U;
    const std::size_t vertex_count = cutter_mesh.positions.size() / 3U;
    if (!contour_plate && vertex_count >= 6U && vertex_count % 2U == 0U) {
      const std::size_t ring_count = vertex_count / 2U;
      std::vector<std::array<double, 2>> contour;
      contour.reserve(ring_count);
      bool analytic_prism = true;
      for (std::size_t index = 0; index < ring_count; ++index) {
        const std::size_t start_offset = index * 3U;
        const std::size_t end_offset = (index + ring_count) * 3U;
        const Vector3d start{cutter_mesh.positions[start_offset],
                             cutter_mesh.positions[start_offset + 1U],
                             cutter_mesh.positions[start_offset + 2U]};
        const Vector3d end{cutter_mesh.positions[end_offset],
                           cutter_mesh.positions[end_offset + 1U],
                           cutter_mesh.positions[end_offset + 2U]};
        const auto start_local = subtract(start, cutter_origin);
        const auto end_local = subtract(end, cutter_origin);
        const double start_x = dot(start_local, *cutter_x);
        const double end_x = dot(end_local, *cutter_x);
        const double start_y = dot(start_local, *cutter_y);
        const double start_z = dot(start_local, *cutter_z);
        const double end_y = dot(end_local, *cutter_y);
        const double end_z = dot(end_local, *cutter_z);
        if (std::abs(start_x) > tolerance || std::abs(end_x - persisted_length) > tolerance ||
            std::abs(start_y - end_y) > tolerance || std::abs(start_z - end_z) > tolerance) {
          analytic_prism = false;
          break;
        }
        contour.push_back({start_y, start_z});
      }
      if (analytic_prism) {
        Vector3d expanded_origin = cutter_origin;
        double expanded_length = persisted_length;
        if (crosses_host_end) {
          constexpr double crossing_end_extension = 1.0e-3;
          const auto expanded = offset_polygon_outward(contour, crossing_end_extension);
          if (!expanded) return;
          contour = *expanded;
          expanded_origin = subtract(expanded_origin, scale(*cutter_x, crossing_end_extension));
          expanded_length += 2.0 * crossing_end_extension;
        }

        const Vector3d aligned_axis = host_axes[aligned_index];
        const double alignment = dot(*cutter_x, aligned_axis);
        if (std::abs(alignment) > 0.99) {
          const Vector3d endpoint = add(expanded_origin, scale(*cutter_x, expanded_length));
          const std::array<double, 2> endpoint_coordinates{
              dot(subtract(expanded_origin, host_origin), aligned_axis),
              dot(subtract(endpoint, host_origin), aligned_axis)};
          const auto& aligned_bounds = host_bounds[aligned_index];
          const bool spans_aligned_host_interval =
              (std::abs(endpoint_coordinates[0] - aligned_bounds[0]) <= tolerance ||
               std::abs(endpoint_coordinates[1] - aligned_bounds[0]) <= tolerance) &&
              (std::abs(endpoint_coordinates[0] - aligned_bounds[1]) <= tolerance ||
               std::abs(endpoint_coordinates[1] - aligned_bounds[1]) <= tolerance);
          for (std::size_t endpoint_index = 0; endpoint_index < 2U; ++endpoint_index) {
            const double coordinate = endpoint_coordinates[endpoint_index];
            double outward = 0.0;
            if (std::abs(coordinate - aligned_bounds[0]) <= tolerance) {
              outward = -1.0;
            } else if (std::abs(coordinate - aligned_bounds[1]) <= tolerance) {
              outward = 1.0;
            } else {
              continue;
            }
            const double epsilon = aligned_index != 0U && spans_aligned_host_interval
                                       ? through_interval_extension
                                       : 1.0e-5;
            const Vector3d delta = scale(aligned_axis, outward * epsilon);
            const double axial_delta = dot(delta, *cutter_x);
            if (endpoint_index == 0U) {
              expanded_origin = add(expanded_origin, delta);
              expanded_length -= axial_delta;
            } else {
              expanded_length += axial_delta;
            }
          }
        }

        for (auto& coordinate : contour) {
          const Vector3d section_point = add(expanded_origin, add(scale(*cutter_y, coordinate[0]),
                                                                  scale(*cutter_z, coordinate[1])));
          Vector3d delta{};
          for (std::size_t axis = 0; axis < host_axes.size(); ++axis) {
            if (axis == aligned_index && std::abs(alignment) > 0.99) continue;
            const double value = dot(subtract(section_point, host_origin), host_axes[axis]);
            if (std::abs(value - host_bounds[axis][0]) <= tolerance) {
              delta = add(delta, scale(host_axes[axis], -1.0e-5));
            } else if (std::abs(value - host_bounds[axis][1]) <= tolerance) {
              delta = add(delta, scale(host_axes[axis], 1.0e-5));
            }
          }
          coordinate[0] += dot(delta, *cutter_y);
          coordinate[1] += dot(delta, *cutter_z);
        }
        for (std::size_t index = 0; index < ring_count; ++index) {
          const auto start = point(expanded_origin, *cutter_x, *cutter_y, *cutter_z, 0.0,
                                   contour[index][0], contour[index][1]);
          const auto end = point(expanded_origin, *cutter_x, *cutter_y, *cutter_z, expanded_length,
                                 contour[index][0], contour[index][1]);
          const std::size_t start_offset = index * 3U;
          const std::size_t end_offset = (index + ring_count) * 3U;
          cutter_mesh.positions[start_offset] = start.x;
          cutter_mesh.positions[start_offset + 1U] = start.y;
          cutter_mesh.positions[start_offset + 2U] = start.z;
          cutter_mesh.positions[end_offset] = end.x;
          cutter_mesh.positions[end_offset + 1U] = end.y;
          cutter_mesh.positions[end_offset + 2U] = end.z;
        }
        return;
      }
    }

    // A section can straddle a host end without placing any persisted contour
    // vertex on that end plane. Expand that cutter's section as a whole so a
    // sub-float remnant cannot survive the Boolean difference.
    const auto cutter_local = [&](Vector3d point, Vector3d axis) {
      return dot(subtract(point, cutter_origin), axis);
    };
    if (crosses_host_end) {
      std::array<double, 2> cutter_y_bounds{std::numeric_limits<double>::infinity(),
                                            -std::numeric_limits<double>::infinity()};
      std::array<double, 2> cutter_z_bounds = cutter_y_bounds;
      for (std::size_t index = 0; index + 2U < cutter_mesh.positions.size(); index += 3U) {
        const Vector3d point{cutter_mesh.positions[index], cutter_mesh.positions[index + 1U],
                             cutter_mesh.positions[index + 2U]};
        const double y = cutter_local(point, *cutter_y);
        const double z = cutter_local(point, *cutter_z);
        cutter_y_bounds[0] = std::min(cutter_y_bounds[0], y);
        cutter_y_bounds[1] = std::max(cutter_y_bounds[1], y);
        cutter_z_bounds[0] = std::min(cutter_z_bounds[0], z);
        cutter_z_bounds[1] = std::max(cutter_z_bounds[1], z);
      }
      constexpr double crossing_end_extension = 1.0e-3;
      const double y_midpoint = (cutter_y_bounds[0] + cutter_y_bounds[1]) * 0.5;
      const double z_midpoint = (cutter_z_bounds[0] + cutter_z_bounds[1]) * 0.5;
      for (std::size_t index = 0; index + 2U < cutter_mesh.positions.size(); index += 3U) {
        Vector3d point{cutter_mesh.positions[index], cutter_mesh.positions[index + 1U],
                       cutter_mesh.positions[index + 2U]};
        const double y = cutter_local(point, *cutter_y);
        const double z = cutter_local(point, *cutter_z);
        point = add(point, scale(*cutter_y, y < y_midpoint ? -crossing_end_extension
                                                           : crossing_end_extension));
        point = add(point, scale(*cutter_z, z < z_midpoint ? -crossing_end_extension
                                                           : crossing_end_extension));
        point = add(point, scale(*cutter_x, station(point) < persisted_length * 0.5
                                                ? -crossing_end_extension
                                                : crossing_end_extension));
        cutter_mesh.positions[index] = point.x;
        cutter_mesh.positions[index + 1U] = point.y;
        cutter_mesh.positions[index + 2U] = point.z;
      }
    }

    Vector3d expanded_origin = cutter_origin;
    double expanded_length = persisted_length;
    if (crosses_host_end) {
      constexpr double crossing_end_extension = 1.0e-3;
      expanded_origin = subtract(expanded_origin, scale(*cutter_x, crossing_end_extension));
      expanded_length += 2.0 * crossing_end_extension;
    }

    if (aligned_magnitude > 0.99) {
      const Vector3d aligned_axis = host_axes[aligned_index];
      const Vector3d endpoint = add(expanded_origin, scale(*cutter_x, expanded_length));
      const std::array<double, 2> endpoint_coordinates{
          dot(subtract(expanded_origin, host_origin), aligned_axis),
          dot(subtract(endpoint, host_origin), aligned_axis)};
      const auto& aligned_bounds = host_bounds[aligned_index];
      const bool spans_aligned_host_interval =
          (std::abs(endpoint_coordinates[0] - aligned_bounds[0]) <= tolerance ||
           std::abs(endpoint_coordinates[1] - aligned_bounds[0]) <= tolerance) &&
          (std::abs(endpoint_coordinates[0] - aligned_bounds[1]) <= tolerance ||
           std::abs(endpoint_coordinates[1] - aligned_bounds[1]) <= tolerance);
      for (std::size_t endpoint_index = 0; endpoint_index < 2U; ++endpoint_index) {
        const double coordinate = endpoint_coordinates[endpoint_index];
        double outward = 0.0;
        if (std::abs(coordinate - aligned_bounds[0]) <= tolerance) {
          outward = -1.0;
        } else if (std::abs(coordinate - aligned_bounds[1]) <= tolerance) {
          outward = 1.0;
        } else {
          continue;
        }
        const double epsilon = aligned_index != 0U && spans_aligned_host_interval
                                   ? through_interval_extension
                                   : 1.0e-5;
        const Vector3d delta = scale(aligned_axis, outward * epsilon);
        const double axial_delta = dot(delta, *cutter_x);
        for (std::size_t index = 0; index + 2U < cutter_mesh.positions.size(); index += 3U) {
          Vector3d point{cutter_mesh.positions[index], cutter_mesh.positions[index + 1U],
                         cutter_mesh.positions[index + 2U]};
          const double original_station = station(point);
          const bool is_endpoint = endpoint_index == 0U
                                       ? original_station < persisted_length * 0.5
                                       : original_station >= persisted_length * 0.5;
          if (is_endpoint) {
            point = add(point, delta);
            cutter_mesh.positions[index] = point.x;
            cutter_mesh.positions[index + 1U] = point.y;
            cutter_mesh.positions[index + 2U] = point.z;
          }
        }
        if (endpoint_index == 0U) {
          expanded_origin = add(expanded_origin, delta);
          expanded_length -= axial_delta;
        } else {
          expanded_length += axial_delta;
        }
      }
    }

    // Expand persisted cutter vertices that lie on a host face. This is the
    // ordinary tangent-face case, distinct from a section crossing an end
    // plane without a vertex on it.
    constexpr double tangent_extension = 1.0e-5;
    for (std::size_t index = 0; index + 2U < cutter_mesh.positions.size(); index += 3U) {
      Vector3d point{cutter_mesh.positions[index], cutter_mesh.positions[index + 1U],
                     cutter_mesh.positions[index + 2U]};
      for (std::size_t axis = 0; axis < host_axes.size(); ++axis) {
        if (axis == aligned_index && aligned_magnitude > 0.99) continue;
        const double y = cutter_local(point, *cutter_y);
        const double z = cutter_local(point, *cutter_z);
        const Vector3d section_point =
            add(expanded_origin, add(scale(*cutter_y, y), scale(*cutter_z, z)));
        const double coordinate = dot(subtract(section_point, host_origin), host_axes[axis]);
        if (std::abs(coordinate - host_bounds[axis][0]) <= tolerance) {
          point = add(point, scale(host_axes[axis], -tangent_extension));
        } else if (std::abs(coordinate - host_bounds[axis][1]) <= tolerance) {
          point = add(point, scale(host_axes[axis], tangent_extension));
        }
      }
      cutter_mesh.positions[index] = point.x;
      cutter_mesh.positions[index + 1U] = point.y;
      cutter_mesh.positions[index + 2U] = point.z;
    }
  }

  void add_diagnostic(ErrorCode code, std::uint64_t object_id, std::string message) {
    diagnostic_messages_.push_back(std::move(message));
    diagnostics_.push_back({code, object_id, diagnostic_messages_.back()});
  }

  [[nodiscard]] std::optional<MeshData> edge_chamfer_cutter(std::uint32_t host_id,
                                                            const MeshData& host_mesh,
                                                            std::uint32_t chamfer_id) {
    const auto chamfer = chamfers_.find(chamfer_id);
    const auto edge = coordinate_systems_.find(chamfer_id);
    const auto host_row = part_rows_.find(host_id);
    if (chamfer == chamfers_.end() || edge == coordinate_systems_.end() ||
        host_row == part_rows_.end() || chamfer->second.type != 1U ||
        chamfer->second.end_types != 12U ||
        std::abs(chamfer->second.first_end_dimension) > 1.0e-12 ||
        std::abs(chamfer->second.second_end_dimension) > 1.0e-12 ||
        !std::isfinite(edge->second.length) || edge->second.length <= 0.0) {
      return std::nullopt;
    }
    const auto edge_basis = axes_.find(edge->second.axes_id);
    if (edge_basis == axes_.end()) return std::nullopt;
    const auto edge_axis = normalized(edge_basis->second.x);
    const auto edge_y_axis = normalized(edge_basis->second.y);
    if (!edge_axis || !edge_y_axis) return std::nullopt;

    const auto host_record = parts_->record(storage_->payload.bytes(), host_row->second);
    if (host_record.empty()) return std::nullopt;
    const auto host_tuple = host_record.subspan(1, part_schema_->tuple_size);
    const auto host_attribute = attributes_.find(read_u32(host_tuple, offsets_[1]));
    const auto host_basis = axes_.find(read_u32(host_tuple, offsets_[2]));
    if (host_attribute == attributes_.end() || host_basis == axes_.end()) {
      return std::nullopt;
    }
    const auto host_x = normalized(host_basis->second.x);
    const auto host_y = normalized(host_basis->second.y);
    const auto host_z = host_x && host_y ? normalized(cross(*host_x, *host_y)) : std::nullopt;
    if (!host_x || !host_y || !host_z) return std::nullopt;
    const Vector3d host_origin{read_f64(host_tuple, offsets_[3]), read_f64(host_tuple, offsets_[4]),
                               read_f64(host_tuple, offsets_[5])};

    Vector3d previous_direction;
    Vector3d following_direction;
    double previous_distance = 0.0;
    double following_distance = 0.0;
    double epsilon = 1.0e-3;
    const bool contour_plate = host_attribute->second.form_type == 2U ||
                               host_attribute->second.form_type == 62U ||
                               host_attribute->second.form_type == 82U;
    if (contour_plate) {
      const auto edge_z_axis = normalized(cross(*edge_axis, *edge_y_axis));
      if (!edge_z_axis) return std::nullopt;
      const auto previous = normalized(add(scale(*edge_y_axis, -1.0), *edge_z_axis));
      const auto following = normalized(subtract(scale(*edge_y_axis, -1.0), *edge_z_axis));
      if (!previous || !following || std::abs(dot(*previous, *following)) > 1.0e-4) {
        return std::nullopt;
      }
      previous_direction = *previous;
      following_direction = *following;
      previous_distance = std::abs(chamfer->second.x);
      following_distance = std::abs(chamfer->second.y);
      epsilon = 2.0;
    } else {
      const std::array<Vector3d, 3> host_axes{*host_x, *host_y, *host_z};
      std::size_t aligned_index = 0;
      double aligned_dot = 0.0;
      for (std::size_t index = 0; index < host_axes.size(); ++index) {
        const double candidate = std::abs(dot(*edge_axis, host_axes[index]));
        if (candidate > aligned_dot) {
          aligned_dot = candidate;
          aligned_index = index;
        }
      }
      if (aligned_dot < 0.999) return std::nullopt;
      std::array<std::size_t, 2> cross_indices{};
      for (std::size_t index = 0, output = 0; index < host_axes.size(); ++index) {
        if (index != aligned_index) cross_indices[output++] = index;
      }
      const auto local = [&](Vector3d point, Vector3d axis) {
        return dot(subtract(point, host_origin), axis);
      };
      std::array<std::array<double, 2>, 3> bounds{};
      for (auto& bound : bounds) {
        bound = {std::numeric_limits<double>::infinity(), -std::numeric_limits<double>::infinity()};
      }
      for (std::size_t index = 0; index + 2U < host_mesh.positions.size(); index += 3U) {
        const Vector3d world{host_mesh.positions[index], host_mesh.positions[index + 1U],
                             host_mesh.positions[index + 2U]};
        for (std::size_t axis = 0; axis < host_axes.size(); ++axis) {
          const double value = local(world, host_axes[axis]);
          bounds[axis][0] = std::min(bounds[axis][0], value);
          bounds[axis][1] = std::max(bounds[axis][1], value);
        }
      }
      const Vector3d first_axis = host_axes[cross_indices[0]];
      const Vector3d second_axis = host_axes[cross_indices[1]];
      const double first_coordinate = local(edge->second.origin, first_axis);
      const double second_coordinate = local(edge->second.origin, second_axis);
      const auto nearest_boundary = [](double value, std::array<double, 2> candidates) {
        return std::abs(value - candidates[0]) < std::abs(value - candidates[1]) ? candidates[0]
                                                                                 : candidates[1];
      };
      if (std::abs(first_coordinate -
                   nearest_boundary(first_coordinate, bounds[cross_indices[0]])) > 1.0e-2 ||
          std::abs(second_coordinate -
                   nearest_boundary(second_coordinate, bounds[cross_indices[1]])) > 1.0e-2) {
        return std::nullopt;
      }
      const double edge_y_first = dot(*edge_y_axis, first_axis);
      const double edge_y_second = dot(*edge_y_axis, second_axis);
      if (std::abs(edge_y_first) < 0.1 || std::abs(edge_y_second) < 0.1 ||
          std::abs(dot(*edge_y_axis, host_axes[aligned_index])) > 1.0e-2) {
        return std::nullopt;
      }
      const auto inward_first = scale(first_axis, edge_y_first > 0.0 ? -1.0 : 1.0);
      const auto inward_second = scale(second_axis, edge_y_second > 0.0 ? -1.0 : 1.0);
      if ((edge_y_first > 0.0) == (edge_y_second > 0.0)) {
        previous_direction = inward_second;
        following_direction = inward_first;
      } else {
        previous_direction = inward_first;
        following_direction = inward_second;
      }
      previous_distance = std::abs(chamfer->second.y);
      following_distance = std::abs(chamfer->second.x);
      const double maximum_dimension =
          std::max(bounds[cross_indices[0]][1] - bounds[cross_indices[0]][0],
                   bounds[cross_indices[1]][1] - bounds[cross_indices[1]][0]);
      if (previous_distance >= maximum_dimension || following_distance >= maximum_dimension)
        return std::nullopt;
    }
    if (!std::isfinite(previous_distance) || !std::isfinite(following_distance) ||
        previous_distance <= 0.0 || following_distance <= 0.0) {
      return std::nullopt;
    }
    Section triangle{
        .kind = Section::Kind::solid,
        .outer = {{-epsilon, -epsilon},
                  {previous_distance * (1.0 + epsilon / following_distance), -epsilon},
                  {-epsilon, following_distance * (1.0 + epsilon / previous_distance)}}};
    DefinitionGeometryView definition{
        .object_id = chamfer_id,
        .origin = subtract(edge->second.origin, scale(*edge_axis, epsilon)),
        .x_axis = *edge_axis,
        .y_axis = previous_direction,
        .z_axis = following_direction,
        .length = edge->second.length + 2.0 * epsilon};
    return extrude(chamfer_id, triangle, definition);
  }

#if defined(TEKLA_DB1_HAS_OCCT)
  [[nodiscard]] static OcctTriangleMesh occt_mesh(const MeshData& mesh) {
    OcctTriangleMesh result;
    result.positions.reserve(mesh.positions.size());
    for (const auto value : mesh.positions) result.positions.push_back(value);
    result.indices = mesh.indices;
    return result;
  }

  [[nodiscard]] static OcctTriangleMesh take_occt_mesh(MeshData&& mesh) noexcept {
    OcctTriangleMesh result;
    result.positions = std::move(mesh.positions);
    result.indices = std::move(mesh.indices);
    return result;
  }

  [[nodiscard]] static std::size_t mesh_storage_bytes(const MeshData& mesh) noexcept {
    constexpr auto maximum = std::numeric_limits<std::size_t>::max();
    if (mesh.positions.size() > maximum / sizeof(double) ||
        mesh.indices.size() > maximum / sizeof(std::uint32_t)) {
      return maximum;
    }
    const auto positions = mesh.positions.size() * sizeof(double);
    const auto indices = mesh.indices.size() * sizeof(std::uint32_t);
    return indices > maximum - positions ? maximum : positions + indices;
  }

  [[nodiscard]] static OcctExtrusion occt_extrusion(const ExtrusionRecipe& recipe) {
    OcctExtrusion result{
        .vector_x = recipe.vector.x, .vector_y = recipe.vector.y, .vector_z = recipe.vector.z};
    result.loops.reserve(recipe.loops.size());
    for (const auto& source : recipe.loops) {
      auto& target = result.loops.emplace_back().positions;
      target.reserve(source.points.size() * 3U);
      for (const auto point : source.points) {
        target.insert(target.end(), {point.x, point.y, point.z});
      }
    }
    return result;
  }

  [[nodiscard]] static const RuledSweepRecipe* ruled_sweep_recipe_from_mesh(
      std::uint64_t object_id, const MeshData& mesh) noexcept {
    if (!mesh.ruled_sweep_recipe || mesh.ruled_sweep_recipe->object_id != object_id ||
        mesh.ruled_sweep_recipe->stations.size() < 2U || mesh.ruled_sweep_recipe->loops.empty()) {
      return nullptr;
    }
    return &*mesh.ruled_sweep_recipe;
  }

  [[nodiscard]] static OcctRuledSweep occt_ruled_sweep(const RuledSweepRecipe& recipe) {
    OcctRuledSweep result{.circular_spine = recipe.circular_spine};
    result.loops.reserve(recipe.loops.size());
    for (const auto& source_loop : recipe.loops) {
      auto& target_sections = result.loops.emplace_back().sections;
      target_sections.reserve(recipe.stations.size());
      for (const auto& station : recipe.stations) {
        auto& target = target_sections.emplace_back().positions;
        target.reserve(source_loop.points.size() * 3U);
        for (const auto coordinate : source_loop.points) {
          const auto point = add(station.origin, add(scale(station.y_axis, coordinate[0]),
                                                     scale(station.z_axis, coordinate[1])));
          target.insert(target.end(), {point.x, point.y, point.z});
        }
      }
    }
    return result;
  }

  [[nodiscard]] static std::optional<ExtrusionRecipe> prism_recipe_from_mesh(
      std::uint64_t object_id, const MeshData& mesh) {
    if (mesh.positions.size() < 18U || mesh.positions.size() % 6U != 0U) return std::nullopt;
    const std::size_t vertex_count = mesh.positions.size() / 3U;
    const std::size_t ring_count = vertex_count / 2U;
    if (mesh.indices.size() != ring_count * 12U - 12U) return std::nullopt;
    const auto vertex = [&](std::size_t index) {
      const auto offset = index * 3U;
      return Vector3d{mesh.positions[offset], mesh.positions[offset + 1U],
                      mesh.positions[offset + 2U]};
    };
    const Vector3d vector = subtract(vertex(ring_count), vertex(0U));
    const double scale_tolerance = std::max(1.0, length(vector)) * 1.0e-10;
    const auto finite_point = [](Vector3d point) {
      return std::isfinite(point.x) && std::isfinite(point.y) && std::isfinite(point.z);
    };
    if (!finite_point(vector) || length(vector) <= 1.0e-12) return std::nullopt;
    RecipeLoop loop;
    loop.points.reserve(ring_count);
    for (std::size_t index = 0; index < ring_count; ++index) {
      const Vector3d start = vertex(index);
      const Vector3d candidate = subtract(vertex(index + ring_count), start);
      if (!finite_point(start) || length(subtract(candidate, vector)) > scale_tolerance) {
        return std::nullopt;
      }
      loop.points.push_back(start);
    }
    ExtrusionRecipe recipe{.object_id = object_id, .vector = vector};
    recipe.loops.push_back(std::move(loop));
    return recipe;
  }

  template <typename TopologyNode>
  void make_bounded_topology_base(std::uint32_t object_id, const MeshData& mesh,
                                  TopologyNode& request, bool force_mesh_fallback = false) const {
    // A two-station PL_V member is a straight planar frustum.  Its generated
    // faceted boundary is already exact, while the generic ruled-sweep recipe
    // is intended for transported/path sections and can change the solid that
    // reaches Boolean evaluation.  Keep this exact planar case faceted until
    // the topology protocol has a dedicated straight-loft primitive.
    bool exact_faceted_straight_loft = false;
    if (const auto row = part_rows_.find(object_id); row != part_rows_.end()) {
      const auto record = parts_->record(storage_->payload.bytes(), row->second);
      if (!record.empty()) {
        const auto tuple = record.subspan(1, part_schema_->tuple_size);
        if (const auto attribute = attributes_.find(read_u32(tuple, offsets_[1]));
            attribute != attributes_.end()) {
          exact_faceted_straight_loft = attribute->second.form_type == 51U;
        }
      }
    }
    bool semantic_source = false;
    if constexpr (requires(TopologyNode node) { node.base_extrusion; }) {
      const auto extrusion = extrusion_recipes_.find(object_id);
      const auto sweep = ruled_sweep_recipe_from_mesh(object_id, mesh);
      const FeaturePlanSummary plan{
          .object_id = object_id,
          .has_semantic_recipe = extrusion != extrusion_recipes_.end() || sweep != nullptr,
          .has_features = operations_.contains(object_id)};
      if (!force_mesh_fallback && !exact_faceted_straight_loft &&
          select_evaluation_tier(plan) == EvaluationTier::analytic_brep) {
        if (sweep) {
          request.base_ruled_sweep = occt_ruled_sweep(*sweep);
        } else {
          request.base_extrusion = occt_extrusion(extrusion->second);
        }
        semantic_source = true;
      }
    }
    if (!semantic_source) request.base_mesh = occt_mesh(mesh);
    const auto row = part_rows_.find(object_id);
    if (row == part_rows_.end()) return;
    const auto record = parts_->record(storage_->payload.bytes(), row->second);
    if (record.empty()) return;
    const auto tuple = record.subspan(1, part_schema_->tuple_size);
    const auto attribute = attributes_.find(read_u32(tuple, offsets_[1]));
    const auto basis = axes_.find(read_u32(tuple, offsets_[2]));
    if (attribute == attributes_.end() || basis == axes_.end()) return;
    const auto form_type = attribute->second.form_type;
    if (form_type == 2U || form_type == 4U || form_type == 8U || form_type == 44U ||
        form_type == 62U || form_type == 64U || form_type == 74U || form_type == 82U ||
        form_type == 105U || form_type == 115U || legacy_arc(attribute->second.object_class)) {
      return;
    }
    const auto x_axis = normalized(basis->second.x);
    const auto y_axis = normalized(basis->second.y);
    const auto z_axis = x_axis && y_axis ? normalized(cross(*x_axis, *y_axis)) : std::nullopt;
    const double persisted_length = read_f64(tuple, offsets_[6]);
    if (!x_axis || !y_axis || !z_axis || !std::isfinite(persisted_length) ||
        persisted_length <= 0.0) {
      return;
    }
    const Vector3d origin{read_f64(tuple, offsets_[3]), read_f64(tuple, offsets_[4]),
                          read_f64(tuple, offsets_[5])};
    double padding = 1.0;
    for (std::size_t index = 0; index + 2U < mesh.positions.size(); index += 3U) {
      const Vector3d point{mesh.positions[index], mesh.positions[index + 1U],
                           mesh.positions[index + 2U]};
      const auto local = subtract(point, origin);
      padding = std::max(padding, std::hypot(dot(local, *y_axis), dot(local, *z_axis)) + 1.0);
    }
    constexpr double endpoint_tolerance = 1.0e-2;
    if constexpr (requires(TopologyNode node) { node.base_extrusion; }) {
      if (!request.base_extrusion.loops.empty()) {
        for (auto& loop : request.base_extrusion.loops) {
          for (std::size_t index = 0; index + 2U < loop.positions.size(); index += 3U) {
            loop.positions[index] -= x_axis->x * padding;
            loop.positions[index + 1U] -= x_axis->y * padding;
            loop.positions[index + 2U] -= x_axis->z * padding;
          }
        }
        request.base_extrusion.vector_x += x_axis->x * 2.0 * padding;
        request.base_extrusion.vector_y += x_axis->y * 2.0 * padding;
        request.base_extrusion.vector_z += x_axis->z * 2.0 * padding;
      }
    }
    if (!semantic_source) {
      const std::size_t vertex_count = request.base_mesh.positions.size() / 3U;
      if (exact_faceted_straight_loft && vertex_count >= 6U && vertex_count % 2U == 0U) {
        const std::size_t ring_count = vertex_count / 2U;
        const double factor = padding / persisted_length;
        for (std::size_t index = 0; index < ring_count; ++index) {
          const std::size_t start_offset = index * 3U;
          const std::size_t end_offset = (index + ring_count) * 3U;
          const Vector3d start{request.base_mesh.positions[start_offset],
                               request.base_mesh.positions[start_offset + 1U],
                               request.base_mesh.positions[start_offset + 2U]};
          const Vector3d end{request.base_mesh.positions[end_offset],
                             request.base_mesh.positions[end_offset + 1U],
                             request.base_mesh.positions[end_offset + 2U]};
          const auto delta = subtract(end, start);
          const auto expanded_start = subtract(start, scale(delta, factor));
          const auto expanded_end = add(end, scale(delta, factor));
          request.base_mesh.positions[start_offset] = expanded_start.x;
          request.base_mesh.positions[start_offset + 1U] = expanded_start.y;
          request.base_mesh.positions[start_offset + 2U] = expanded_start.z;
          request.base_mesh.positions[end_offset] = expanded_end.x;
          request.base_mesh.positions[end_offset + 1U] = expanded_end.y;
          request.base_mesh.positions[end_offset + 2U] = expanded_end.z;
        }
      } else {
        for (std::size_t index = 0; index + 2U < request.base_mesh.positions.size(); index += 3U) {
          Vector3d point{request.base_mesh.positions[index],
                         request.base_mesh.positions[index + 1U],
                         request.base_mesh.positions[index + 2U]};
          const double station = dot(subtract(point, origin), *x_axis);
          if (std::abs(station) <= endpoint_tolerance) {
            point = subtract(point, scale(*x_axis, padding));
          } else if (std::abs(station - persisted_length) <= endpoint_tolerance) {
            point = add(point, scale(*x_axis, padding));
          }
          request.base_mesh.positions[index] = point.x;
          request.base_mesh.positions[index + 1U] = point.y;
          request.base_mesh.positions[index + 2U] = point.z;
        }
      }
    }
    // A persisted fitting plane replaces the corresponding nominal end of a
    // straight member.  It is not an additional clip inside the nominal
    // [0, length] prism: an oblique fitting can retain a section corner beyond
    // the reference-line endpoint.  Keep the synthetic endpoint only on a
    // side for which no fitting is present.  The real fitting half-space is
    // appended later with the rest of the persisted operation graph.
    bool has_start_fitting = false;
    bool has_end_fitting = false;
    if (const auto attached = operations_.find(object_id); attached != operations_.end()) {
      const auto midpoint = add(origin, scale(*x_axis, persisted_length / 2.0));
      for (const auto& operation : attached->second) {
        if (operation.type != 9U) continue;
        const auto coordinate_system = coordinate_systems_.find(operation.target_id);
        if (coordinate_system == coordinate_systems_.end()) continue;
        const auto operation_axes = axes_.find(coordinate_system->second.axes_id);
        if (operation_axes == axes_.end()) continue;
        auto normal = normalized(cross(operation_axes->second.x, operation_axes->second.y));
        if (!normal) continue;
        if (dot(subtract(midpoint, coordinate_system->second.origin), *normal) < 0.0) {
          normal = scale(*normal, -1.0);
        }
        const double longitudinal_direction = dot(*x_axis, *normal);
        has_start_fitting = has_start_fitting || longitudinal_direction > 1.0e-12;
        has_end_fitting = has_end_fitting || longitudinal_direction < -1.0e-12;
      }
    }
    if (!has_start_fitting) {
      request.keep_half_spaces.push_back(
          {origin.x, origin.y, origin.z, x_axis->x, x_axis->y, x_axis->z});
    }
    const auto end = add(origin, scale(*x_axis, persisted_length));
    if (!has_end_fitting) {
      request.keep_half_spaces.push_back({end.x, end.y, end.z, -x_axis->x, -x_axis->y, -x_axis->z});
    }
  }
#endif

#if defined(TEKLA_DB1_HAS_OCCT)
  static constexpr std::size_t maximum_operation_graph_depth = 32U;
  static constexpr std::size_t maximum_operation_graph_nodes = 4096U;
  static constexpr std::size_t maximum_operation_graph_edges = 65'536U;
  static constexpr std::size_t maximum_operative_cache_entries = 4096U;
  static constexpr std::size_t maximum_operative_cache_bytes = 256U * 1024U * 1024U;
  static constexpr std::size_t maximum_topology_result_cache_entries = 4096U;
  static constexpr std::size_t maximum_topology_result_cache_bytes = 256U * 1024U * 1024U;
  static constexpr std::size_t maximum_topology_request_mesh_bytes = 256U * 1024U * 1024U;
  static constexpr double operative_linear_deflection = 0.5;
  static constexpr double operative_angular_deflection = 0.5;

  struct OperationGraphState {
    std::unordered_set<std::uint32_t> active;
    std::unordered_map<std::size_t, std::vector<std::uint32_t>> shared_nodes;
    std::size_t nodes = 0U;
    std::size_t edges = 0U;
    std::size_t request_mesh_bytes = 0U;
    std::size_t context_dependent_failures = 0U;
    std::size_t partial_results = 0U;
  };

  [[nodiscard]] static std::size_t csg_node_hash(std::uint32_t object_id,
                                                 const MeshData& mesh) noexcept {
    std::uint64_t value = static_cast<std::uint64_t>(object_id) ^ 0xcbf29ce484222325ULL;
    const auto mix = [&](std::uint64_t item) {
      value ^= item;
      value *= 0x100000001b3ULL;
    };
    mix(mesh.positions.size());
    mix(mesh.indices.size());
    for (const auto coordinate : mesh.positions) mix(std::bit_cast<std::uint64_t>(coordinate));
    for (const auto index : mesh.indices) mix(index);
    return static_cast<std::size_t>(value ^ (value >> 32U));
  }

  struct ActiveOperationGuard {
    std::unordered_set<std::uint32_t>& active;
    std::uint32_t object_id;
    ~ActiveOperationGuard() { active.erase(object_id); }
  };

  struct OperativeCacheKey {
    std::uint32_t object_id = 0U;
    TopologyMode topology_mode = TopologyMode::disabled;
    std::uint64_t linear_deflection = 0U;
    std::uint64_t angular_deflection = 0U;
    bool operator==(const OperativeCacheKey&) const = default;
  };

  struct OperativeCacheKeyHash {
    std::size_t operator()(const OperativeCacheKey& key) const noexcept {
      std::size_t value = key.object_id;
      const auto mix = [&](std::uint64_t item) {
        const auto folded = item ^ (item >> 32U);
        value ^= static_cast<std::size_t>(folded) + 0x9e3779b9U + (value << 6U) + (value >> 2U);
      };
      mix(static_cast<std::uint64_t>(key.topology_mode));
      mix(key.linear_deflection);
      mix(key.angular_deflection);
      return value;
    }
  };

  struct OperativeCacheEntry {
    std::optional<MeshData> mesh;
    std::optional<Error> failure;
  };

  struct TopologyResultCacheKeyHash {
    std::size_t operator()(const std::vector<std::byte>& bytes) const noexcept {
      std::uint64_t value = 0xcbf29ce484222325ULL;
      for (const auto byte : bytes) {
        value ^= std::to_integer<std::uint8_t>(byte);
        value *= 0x100000001b3ULL;
      }
      return static_cast<std::size_t>(value ^ (value >> 32U));
    }
  };

  struct TopologyResultCacheEntry {
    std::array<double, 3> origin{};
    OcctMesh mesh;
  };

  struct MeshBounds {
    Vector3d minimum;
    Vector3d maximum;
  };

  [[nodiscard]] static std::optional<MeshBounds> mesh_bounds(const MeshData& mesh) noexcept {
    if (mesh.positions.size() < 3U) return std::nullopt;
    MeshBounds bounds{{mesh.positions[0], mesh.positions[1], mesh.positions[2]},
                      { mesh.positions[0],
                        mesh.positions[1],
                        mesh.positions[2] }};
    for (std::size_t index = 3U; index + 2U < mesh.positions.size(); index += 3U) {
      bounds.minimum.x = std::min(bounds.minimum.x, mesh.positions[index]);
      bounds.minimum.y = std::min(bounds.minimum.y, mesh.positions[index + 1U]);
      bounds.minimum.z = std::min(bounds.minimum.z, mesh.positions[index + 2U]);
      bounds.maximum.x = std::max(bounds.maximum.x, mesh.positions[index]);
      bounds.maximum.y = std::max(bounds.maximum.y, mesh.positions[index + 1U]);
      bounds.maximum.z = std::max(bounds.maximum.z, mesh.positions[index + 2U]);
    }
    return bounds;
  }

  [[nodiscard]] static bool intersects(const MeshBounds& left, const MeshBounds& right,
                                       double tolerance = 1.0e-3) noexcept {
    return left.minimum.x <= right.maximum.x + tolerance &&
           left.maximum.x + tolerance >= right.minimum.x &&
           left.minimum.y <= right.maximum.y + tolerance &&
           left.maximum.y + tolerance >= right.minimum.y &&
           left.minimum.z <= right.maximum.z + tolerance &&
           left.maximum.z + tolerance >= right.minimum.z;
  }

  static void trim_regular_sweep_to_relevance(MeshData& mesh, const MeshBounds& relevance) {
    // A generated single-loop sweep has two triangles per side quad and two
    // triangulated end caps. Boolean operands can span an entire building bay
    // even when only a few adjacent sweep stations can touch the host. Keep
    // the relevant segment interval plus one segment of clearance at each end
    // and rebuild closed caps there. The discarded closed sweep portions are
    // disjoint from the host, so they cannot affect A - B.
    if (!mesh.ruled_sweep_recipe || mesh.ruled_sweep_recipe->loops.size() != 1U) {
      return;
    }
    const std::size_t vertex_count = mesh.positions.size() / 3U;
    const std::size_t triangle_count = mesh.indices.size() / 3U;
    if (vertex_count < 12U || mesh.positions.size() % 3U != 0U || mesh.indices.size() % 3U != 0U ||
        mesh.indices.size() < 6U)
      return;
    const std::size_t ring_count = mesh.ruled_sweep_recipe->loops.front().points.size();
    if (ring_count < 3U || ring_count >= vertex_count || vertex_count % ring_count != 0U ||
        mesh.indices[0] != 0U || mesh.indices[1] != 1U || mesh.indices[2] != ring_count + 1U ||
        mesh.indices[3] != 0U || mesh.indices[4] != ring_count + 1U ||
        mesh.indices[5] != ring_count)
      return;
    const std::size_t station_count = mesh.ruled_sweep_recipe->stations.size();
    if (station_count != vertex_count / ring_count) return;
    if (station_count < 3U) return;
    const std::size_t side_triangle_count = 2U * ring_count * (station_count - 1U);
    const std::size_t cap_triangle_count = 2U * (ring_count - 2U);
    if (triangle_count != side_triangle_count + cap_triangle_count) return;

    const auto segment_bounds = [&](std::size_t segment) {
      const std::size_t first_vertex = segment * ring_count;
      MeshBounds bounds{{mesh.positions[first_vertex * 3U], mesh.positions[first_vertex * 3U + 1U],
                         mesh.positions[first_vertex * 3U + 2U]},
                        { mesh.positions[first_vertex * 3U],
                          mesh.positions[first_vertex * 3U + 1U],
                          mesh.positions[first_vertex * 3U + 2U] }};
      for (std::size_t vertex = first_vertex + 1U; vertex < first_vertex + 2U * ring_count;
           ++vertex) {
        const std::size_t offset = vertex * 3U;
        bounds.minimum.x = std::min(bounds.minimum.x, mesh.positions[offset]);
        bounds.minimum.y = std::min(bounds.minimum.y, mesh.positions[offset + 1U]);
        bounds.minimum.z = std::min(bounds.minimum.z, mesh.positions[offset + 2U]);
        bounds.maximum.x = std::max(bounds.maximum.x, mesh.positions[offset]);
        bounds.maximum.y = std::max(bounds.maximum.y, mesh.positions[offset + 1U]);
        bounds.maximum.z = std::max(bounds.maximum.z, mesh.positions[offset + 2U]);
      }
      return bounds;
    };

    std::optional<std::size_t> first_segment;
    std::size_t last_segment = 0U;
    for (std::size_t segment = 0U; segment + 1U < station_count; ++segment) {
      if (!intersects(segment_bounds(segment), relevance)) continue;
      if (!first_segment) first_segment = segment;
      last_segment = segment;
    }
    if (!first_segment) {
      mesh.positions.clear();
      mesh.indices.clear();
      return;
    }
    const std::size_t first_station = *first_segment > 0U ? *first_segment - 1U : 0U;
    const std::size_t last_station = std::min(station_count - 1U, last_segment + 2U);
    if (first_station == 0U && last_station + 1U == station_count) return;

    MeshData trimmed;
    trimmed.object_id = mesh.object_id;
    const std::size_t trimmed_station_count = last_station - first_station + 1U;
    trimmed.ruled_sweep_recipe = mesh.ruled_sweep_recipe;
    trimmed.ruled_sweep_recipe->stations.assign(
        mesh.ruled_sweep_recipe->stations.begin() + static_cast<std::ptrdiff_t>(first_station),
        mesh.ruled_sweep_recipe->stations.begin() + static_cast<std::ptrdiff_t>(last_station + 1U));
    const std::size_t first_coordinate = first_station * ring_count * 3U;
    const std::size_t last_coordinate = (last_station + 1U) * ring_count * 3U;
    trimmed.positions.assign(mesh.positions.begin() + static_cast<std::ptrdiff_t>(first_coordinate),
                             mesh.positions.begin() + static_cast<std::ptrdiff_t>(last_coordinate));
    trimmed.indices.reserve((2U * ring_count * (trimmed_station_count - 1U) + cap_triangle_count) *
                            3U);
    const auto quad = [&](std::uint32_t a, std::uint32_t b, std::uint32_t c, std::uint32_t d) {
      trimmed.indices.insert(trimmed.indices.end(), {a, b, c, a, c, d});
    };
    for (std::size_t station = 0U; station + 1U < trimmed_station_count; ++station) {
      const auto current = static_cast<std::uint32_t>(station * ring_count);
      const auto next_ring = current + static_cast<std::uint32_t>(ring_count);
      for (std::size_t index = 0U; index < ring_count; ++index) {
        const auto next = (index + 1U) % ring_count;
        quad(current + static_cast<std::uint32_t>(index),
             current + static_cast<std::uint32_t>(next),
             next_ring + static_cast<std::uint32_t>(next),
             next_ring + static_cast<std::uint32_t>(index));
      }
    }
    const std::size_t cap_offset = side_triangle_count * 3U;
    const auto original_last = static_cast<std::uint32_t>((station_count - 1U) * ring_count);
    const auto trimmed_last = static_cast<std::uint32_t>((trimmed_station_count - 1U) * ring_count);
    for (std::size_t triangle = 0U; triangle < ring_count - 2U; ++triangle) {
      const std::size_t first = cap_offset + triangle * 6U;
      trimmed.indices.insert(trimmed.indices.end(), {mesh.indices[first], mesh.indices[first + 1U],
                                                     mesh.indices[first + 2U]});
      trimmed.indices.insert(trimmed.indices.end(),
                             {mesh.indices[first + 3U] - original_last + trimmed_last,
                              mesh.indices[first + 4U] - original_last + trimmed_last,
                              mesh.indices[first + 5U] - original_last + trimmed_last});
    }
    mesh = std::move(trimmed);
  }

  [[nodiscard]] OperativeCacheKey operative_cache_key(std::uint32_t object_id) const noexcept {
    return {object_id, topology_mode_, std::bit_cast<std::uint64_t>(operative_linear_deflection),
            std::bit_cast<std::uint64_t>(operative_angular_deflection)};
  }

  [[nodiscard]] static std::size_t operative_cache_bytes(const MeshData& mesh) noexcept {
    return sizeof(OperativeCacheEntry) + mesh.positions.size() * sizeof(double) +
           mesh.indices.size() * sizeof(std::uint32_t);
  }

  [[nodiscard]] static std::size_t operative_cache_bytes(const Error& failure) noexcept {
    return sizeof(OperativeCacheEntry) + failure.message.size();
  }

  void cache_operative(const OperativeCacheKey& key, const MeshData& mesh) {
    const auto bytes = operative_cache_bytes(mesh);
    if (operative_cache_.size() >= maximum_operative_cache_entries ||
        bytes > maximum_operative_cache_bytes - operative_cache_bytes_) {
      return;
    }
    OperativeCacheEntry value;
    value.mesh = mesh;
    const bool inserted = operative_cache_.emplace(key, std::move(value)).second;
    if (!inserted) return;
    operative_cache_bytes_ += bytes;
  }

  void cache_operative_failure(const OperativeCacheKey& key, const Error& failure) {
    const auto bytes = operative_cache_bytes(failure);
    if (operative_cache_.size() >= maximum_operative_cache_entries ||
        bytes > maximum_operative_cache_bytes - operative_cache_bytes_) {
      return;
    }
    OperativeCacheEntry value;
    value.failure = failure;
    const bool inserted = operative_cache_.emplace(key, std::move(value)).second;
    if (!inserted) return;
    operative_cache_bytes_ += bytes;
  }

  [[nodiscard]] std::optional<OcctMesh> cached_topology_result(
      const TranslationNormalizedOcctRequest& key, std::uint64_t object_id) const {
    if (std::getenv("TEKLA_DB1_DISABLE_OCCT_CACHE") != nullptr) return std::nullopt;
    const auto found = topology_result_cache_.find(key.bytes);
    if (found == topology_result_cache_.end()) return std::nullopt;
    OcctMesh mesh = found->second.mesh;
    mesh.object_id = object_id;
    for (std::size_t index = 0U; index + 2U < mesh.positions.size(); index += 3U) {
      for (std::size_t axis = 0U; axis < 3U; ++axis) {
        const double translated = static_cast<double>(mesh.positions[index + axis]) +
                                  key.origin[axis] - found->second.origin[axis];
        mesh.positions[index + axis] = static_cast<float>(translated);
      }
    }
    if (std::getenv("TEKLA_DB1_OCCT_CACHE_PROFILE") != nullptr) {
      std::fprintf(stderr,
                   "{\"occt_cache\":true,\"object_id\":%llu,\"outcome\":\"hit\","
                   "\"key_bytes\":%zu}\n",
                   static_cast<unsigned long long>(object_id), key.bytes.size());
    }
    return mesh;
  }

  void cache_topology_result(TranslationNormalizedOcctRequest key, const OcctMesh& mesh) {
    if (std::getenv("TEKLA_DB1_DISABLE_OCCT_CACHE") != nullptr) return;
    const std::size_t bytes = key.bytes.size() + mesh.positions.size() * sizeof(float) +
                              mesh.indices.size() * sizeof(std::uint32_t) +
                              sizeof(TopologyResultCacheEntry);
    if (topology_result_cache_.size() >= maximum_topology_result_cache_entries ||
        bytes > maximum_topology_result_cache_bytes - topology_result_cache_bytes_) {
      return;
    }
    TopologyResultCacheEntry entry{key.origin, mesh};
    const bool inserted =
        topology_result_cache_.emplace(std::move(key.bytes), std::move(entry)).second;
    if (!inserted) return;
    topology_result_cache_bytes_ += bytes;
    if (std::getenv("TEKLA_DB1_OCCT_CACHE_PROFILE") != nullptr) {
      std::fprintf(stderr,
                   "{\"occt_cache\":true,\"object_id\":%llu,\"outcome\":\"insert\","
                   "\"entry_bytes\":%zu,\"total_bytes\":%zu}\n",
                   static_cast<unsigned long long>(mesh.object_id), bytes,
                   topology_result_cache_bytes_);
    }
  }

  [[nodiscard]] static bool context_independent_failure(ErrorCode code) noexcept {
    // A resource limit can describe either a deterministic graph bound or a
    // transient evaluator allocation failure. The public error code does not
    // distinguish those failure causes, so retry rather than poisoning the
    // operative cache after temporary memory pressure.
    return code == ErrorCode::invalid_argument || code == ErrorCode::decoder_unavailable ||
           code == ErrorCode::invalid_geometry || code == ErrorCode::invalid_topology;
  }

  [[nodiscard]] Result<std::uint32_t> append_csg_node(
      std::uint32_t object_id, MeshData mesh, OperationGraphState& graph, std::size_t depth,
      OcctRequest& request, std::optional<ExtrusionRecipe> semantic_recipe = std::nullopt,
      bool force_mesh_fallback = false) {
    if (depth >= maximum_operation_graph_depth) {
      ++graph.context_dependent_failures;
      return Result<std::uint32_t>::failure(
          {ErrorCode::resource_limit, "A Boolean operation graph exceeds the depth limit."});
    }
    if (!graph.active.insert(object_id).second) {
      ++graph.context_dependent_failures;
      return Result<std::uint32_t>::failure(
          {ErrorCode::invalid_topology, "A Boolean operation graph contains a cycle."});
    }
    ActiveOperationGuard guard{graph.active, object_id};
    const auto semantic_extrusion = !force_mesh_fallback && semantic_recipe
                                        ? std::optional{occt_extrusion(*semantic_recipe)}
                                        : std::nullopt;
    const auto* ruled_recipe =
        force_mesh_fallback ? nullptr : ruled_sweep_recipe_from_mesh(object_id, mesh);
    // The persisted circular path is analytic, but a nested cutter's tiny
    // host-relative tolerance envelope is currently applied to retained
    // section stations. Revolving that adjusted profile produces a different
    // in-between envelope and can turn a stable faceted cut into a tangent
    // OCCT classification problem. Keep visible circular members analytic;
    // use the guarded faceted tier for circular Boolean operands until the
    // envelope itself is represented as an analytic feature.
    if (depth > 0U && ruled_recipe != nullptr && ruled_recipe->circular_spine) {
      ruled_recipe = nullptr;
    }
    const auto semantic_sweep =
        ruled_recipe ? std::optional{occt_ruled_sweep(*ruled_recipe)} : std::nullopt;
    std::optional<std::size_t> shared_hash;
    if (depth > 0U) {
      shared_hash = csg_node_hash(object_id, mesh);
      if (const auto existing = graph.shared_nodes.find(*shared_hash);
          existing != graph.shared_nodes.end()) {
        for (const auto candidate : existing->second) {
          if (candidate >= request.nodes.size()) continue;
          const auto& node = request.nodes[candidate];
          const bool same_semantic_source =
              (semantic_sweep && node.base_ruled_sweep == *semantic_sweep) ||
              (semantic_extrusion && !semantic_sweep && node.base_extrusion == *semantic_extrusion);
          const bool same_mesh_source = !semantic_sweep && !semantic_extrusion &&
                                        node.base_mesh.positions == mesh.positions &&
                                        node.base_mesh.indices == mesh.indices;
          if (node.object_id == object_id && (same_semantic_source || same_mesh_source)) {
            return Result<std::uint32_t>::success(candidate);
          }
        }
      }
    }
    if (graph.nodes >= maximum_operation_graph_nodes) {
      ++graph.context_dependent_failures;
      return Result<std::uint32_t>::failure(
          {ErrorCode::resource_limit, "A Boolean operation graph exceeds the node limit."});
    }
    const auto bytes = mesh_storage_bytes(mesh);
    if (bytes > maximum_topology_request_mesh_bytes - graph.request_mesh_bytes) {
      ++graph.context_dependent_failures;
      return Result<std::uint32_t>::failure(
          {ErrorCode::resource_limit, "A topology request exceeds its mesh-memory limit."});
    }
    graph.request_mesh_bytes += bytes;
    ++graph.nodes;
    const auto node_index = static_cast<std::uint32_t>(request.nodes.size());
    request.nodes.emplace_back();
    request.nodes[node_index].object_id = object_id;
    if (depth == 0U) {
      make_bounded_topology_base(object_id, mesh, request.nodes[node_index], force_mesh_fallback);
    } else if (semantic_sweep) {
      request.nodes[node_index].base_ruled_sweep = *semantic_sweep;
    } else if (semantic_extrusion) {
      // Cutter tolerance envelopes have already been applied to the recipe.
      // Keep it as a prism here; synthetic endpoint clipping is root-only.
      request.nodes[node_index].base_extrusion = *semantic_extrusion;
    } else {
      // Nested operatives are already bounded by their persisted definition
      // and any real fitting planes below.  Re-clipping each child to a
      // synthetic endpoint envelope both defeats tangent extensions and adds
      // two expensive Common operations per node.
      request.nodes[node_index].base_mesh = occt_mesh(mesh);
    }

    const auto found = operations_.find(object_id);
    if (found == operations_.end() || found->second.empty()) {
      if (shared_hash) graph.shared_nodes[*shared_hash].push_back(node_index);
      return Result<std::uint32_t>::success(node_index);
    }
    bool replay_boolean_children = true;
    if (const auto row = part_rows_.find(object_id); row != part_rows_.end()) {
      const auto record = parts_->record(storage_->payload.bytes(), row->second);
      const auto tuple = record.empty() ? std::span<const std::byte>{}
                                        : record.subspan(1, part_schema_->tuple_size);
      const auto attribute =
          tuple.empty() ? attributes_.end() : attributes_.find(read_u32(tuple, offsets_[1]));
      if (attribute != attributes_.end()) {
        // A type-11 part is already the persisted Boolean operand. Its own
        // outgoing type-11 relations describe the component/construction
        // graph that produced it, not a second level of cuts to replay on the
        // operand. Following those links recursively can walk across sibling
        // members of a repeated component and changes A - B into
        // A - (B - C ...). Fittings and edge chamfers attached directly to
        // the operand remain real geometry operations and are still applied.
        replay_boolean_children = attribute->second.object_type != 11U;
      }
    }
    if (found->second.size() > maximum_operation_graph_edges - graph.edges) {
      ++graph.context_dependent_failures;
      return Result<std::uint32_t>::failure(
          {ErrorCode::resource_limit, "A Boolean operation graph exceeds the edge limit."});
    }
    graph.edges += found->second.size();

    Vector3d minimum{mesh.positions[0], mesh.positions[1], mesh.positions[2]};
    Vector3d maximum = minimum;
    for (std::size_t index = 3U; index + 2U < mesh.positions.size(); index += 3U) {
      minimum.x = std::min(minimum.x, mesh.positions[index]);
      minimum.y = std::min(minimum.y, mesh.positions[index + 1U]);
      minimum.z = std::min(minimum.z, mesh.positions[index + 2U]);
      maximum.x = std::max(maximum.x, mesh.positions[index]);
      maximum.y = std::max(maximum.y, mesh.positions[index + 1U]);
      maximum.z = std::max(maximum.z, mesh.positions[index + 2U]);
    }
    const Vector3d midpoint = scale(add(minimum, maximum), 0.5);
    const MeshBounds host_bounds{minimum, maximum};

    struct BooleanCutter {
      std::uint32_t object_id = 0U;
      MeshData mesh;
      std::optional<ExtrusionRecipe> semantic_recipe;
      double volume = 0.0;
    };
    std::vector<BooleanCutter> cutters;
    std::unordered_set<std::uint32_t> boolean_targets;
    for (const auto& operation : found->second) {
      if (operation.type != 11U || !replay_boolean_children ||
          !boolean_targets.insert(operation.target_id).second)
        continue;
      auto cutter = operative_mesh(operation.target_id, 0.0, true);
      if (!cutter) {
        ++graph.partial_results;
        add_diagnostic(ErrorCode::invalid_geometry, object_id,
                       "Boolean operative " + std::to_string(operation.target_id) +
                           " was omitted: its persisted geometry is incomplete.");
        continue;
      }
      // All persisted child operations are subtractive or clipping, so they
      // can only shrink this raw operative. A disjoint raw envelope therefore
      // cannot affect the host and need not enter OCCT at all. This matters for
      // repeated legacy components whose relation graph retains cutters for
      // sibling placements outside the current part.
      const auto cutter_bounds = mesh_bounds(*cutter);
      if (!cutter_bounds || !intersects(host_bounds, *cutter_bounds)) continue;
      trim_regular_sweep_to_relevance(*cutter, host_bounds);
      if (cutter->positions.empty() || cutter->indices.empty()) continue;
      const double volume = mesh_volume(*cutter);
      cutters.push_back({operation.target_id, std::move(*cutter), std::nullopt, volume});
    }
    if (cutters.size() > 1U) {
      const auto [minimum_volume, maximum_volume] =
          std::minmax_element(cutters.begin(), cutters.end(),
                              [](const BooleanCutter& left, const BooleanCutter& right) {
                                return left.volume < right.volume;
                              });
      const double tolerance = std::max(1.0, maximum_volume->volume * 1.0e-8);
      const bool uniform = maximum_volume->volume > 0.0 &&
                           maximum_volume->volume - minimum_volume->volume <= tolerance;
      const double extension = uniform ? 1.0e-3 : 1.0e-5;
      for (auto& cutter : cutters) {
        expand_tangent_cutter(object_id, cutter.object_id, mesh, cutter.mesh, extension);
      }
    } else {
      for (auto& cutter : cutters) {
        expand_tangent_cutter(object_id, cutter.object_id, mesh, cutter.mesh, 1.0e-5);
      }
    }
    for (auto& cutter : cutters) {
      cutter.semantic_recipe = prism_recipe_from_mesh(cutter.object_id, cutter.mesh);
    }
    std::stable_sort(cutters.begin(), cutters.end(),
                     [](const BooleanCutter& left, const BooleanCutter& right) {
                       return left.volume > right.volume;
                     });
    for (auto& cutter : cutters) {
      auto child = append_csg_node(cutter.object_id, std::move(cutter.mesh), graph, depth + 1U,
                                   request, std::move(cutter.semantic_recipe), force_mesh_fallback);
      if (!child) {
        ++graph.partial_results;
        add_diagnostic(child.error().code, object_id,
                       "Boolean operative " + std::to_string(cutter.object_id) +
                           " was omitted: " + child.error().message);
        continue;
      }
      request.nodes[node_index].subtract_nodes.push_back(child.value());
    }

    struct PlaneCandidate {
      OcctHalfSpace plane;
      GeometryOperation operation;
    };
    std::vector<PlaneCandidate> planes;
    for (const auto& operation : found->second) {
      if (operation.type == 79U) {
        auto cutter = edge_chamfer_cutter(object_id, mesh, operation.target_id);
        if (!cutter) {
          add_diagnostic(
              ErrorCode::decoder_unavailable, object_id,
              "A persisted edge chamfer is outside the supported straight-corner slice.");
          continue;
        }
        const auto cutter_bytes = mesh_storage_bytes(*cutter);
        if (graph.nodes >= maximum_operation_graph_nodes ||
            cutter_bytes > maximum_topology_request_mesh_bytes - graph.request_mesh_bytes) {
          ++graph.context_dependent_failures;
          return Result<std::uint32_t>::failure(
              {ErrorCode::resource_limit, "A topology request exceeds its graph limits."});
        }
        graph.request_mesh_bytes += cutter_bytes;
        ++graph.nodes;
        const auto child_index = static_cast<std::uint32_t>(request.nodes.size());
        request.nodes.emplace_back();
        request.nodes[child_index].object_id = operation.target_id;
        request.nodes[child_index].base_mesh = take_occt_mesh(std::move(*cutter));
        request.nodes[node_index].subtract_nodes.push_back(child_index);
        continue;
      }
      if (operation.type == 11U || (operation.type != 9U && operation.type != 12U)) continue;
      const auto coordinate_system = coordinate_systems_.find(operation.target_id);
      if (coordinate_system == coordinate_systems_.end()) continue;
      const auto axes = axes_.find(coordinate_system->second.axes_id);
      if (axes == axes_.end()) continue;
      auto normal = normalized(cross(axes->second.x, axes->second.y));
      if (!normal) continue;
      if (operation.type == 12U) {
        normal = scale(*normal, -1.0);
      } else if (dot(subtract(midpoint, coordinate_system->second.origin), *normal) < 0.0) {
        normal = scale(*normal, -1.0);
      }
      PlaneCandidate candidate{
          {coordinate_system->second.origin.x, coordinate_system->second.origin.y,
           coordinate_system->second.origin.z, normal->x, normal->y, normal->z},
          operation};
      bool replaced = false;
      if (operation.type == 9U) {
        for (auto& existing : planes) {
          const Vector3d existing_normal{existing.plane.normal_x, existing.plane.normal_y,
                                         existing.plane.normal_z};
          if (existing.operation.type != 9U || dot(existing_normal, *normal) < 1.0 - 1.0e-8)
            continue;
          if (std::pair{operation.event_id, operation.relation_id} >
              std::pair{existing.operation.event_id, existing.operation.relation_id}) {
            existing = candidate;
          }
          replaced = true;
          break;
        }
      }
      if (!replaced) planes.push_back(candidate);
    }
    for (const auto& plane : planes) {
      request.nodes[node_index].keep_half_spaces.push_back(plane.plane);
    }
    if (shared_hash) graph.shared_nodes[*shared_hash].push_back(node_index);
    return Result<std::uint32_t>::success(node_index);
  }

  [[nodiscard]] Result<bool> evaluate_operations_csg(std::uint32_t object_id, MeshData& mesh,
                                                     OperationGraphState& graph) {
    const auto found = operations_.find(object_id);
    if (found == operations_.end() || found->second.empty()) return Result<bool>::success(false);
    const auto build_request = [&](OperationGraphState& state,
                                   bool force_mesh_fallback) -> Result<OcctRequest> {
      OcctRequest request;
      request.object_id = object_id;
      request.linear_deflection = operative_linear_deflection;
      request.angular_deflection = operative_angular_deflection;
      auto root =
          append_csg_node(object_id, mesh, state, 0U, request, std::nullopt, force_mesh_fallback);
      if (!root) return Result<OcctRequest>::failure(std::move(root.error()));
      if (root.value() != 0U) {
        return Result<OcctRequest>::failure(
            {ErrorCode::internal_error, "The topology request did not produce root node zero."});
      }
      return Result<OcctRequest>::success(std::move(request));
    };
    const auto evaluate_request = [&](const OcctRequest& request) {
      auto key = encode_translation_normalized_occt_request(request);
      if (key) {
        if (auto cached = cached_topology_result(key.value(), request.object_id)) {
          return Result<OcctMesh>::success(std::move(*cached));
        }
      }
      auto evaluated = topology_mode_ == TopologyMode::supervised
                           ? supervised_host_ != nullptr
                                 ? supervised_host_->evaluate(request)
                                 : Result<OcctMesh>::failure(
                                       {ErrorCode::invalid_argument,
                                        "A supervised topology worker path was not supplied."})
                           : direct_host_.evaluate(request);
      if (evaluated && key) cache_topology_result(std::move(key.value()), evaluated.value());
      return evaluated;
    };

    auto request = build_request(graph, false);
    if (!request) return Result<bool>::failure(std::move(request.error()));
    const auto& root_node = request.value().nodes.front();
    if (root_node.subtract.empty() && root_node.subtract_nodes.empty() &&
        root_node.keep_half_spaces.empty()) {
      return Result<bool>::success(false);
    }
    Result<OcctMesh> evaluated = evaluate_request(request.value());
    if (!evaluated && (evaluated.error().code == ErrorCode::invalid_topology ||
                       evaluated.error().code == ErrorCode::geometry_timeout)) {
      const auto semantic_failure = evaluated.error();
      OperationGraphState fallback_graph;
      auto fallback_request = build_request(fallback_graph, true);
      if (fallback_request) evaluated = evaluate_request(fallback_request.value());
      if (evaluated) {
        add_diagnostic(
            semantic_failure.code, object_id,
            "Analytic topology used the guarded faceted fallback: " + semantic_failure.message);
      }
    }
    if (!evaluated) return Result<bool>::failure(std::move(evaluated.error()));
    mesh.positions.assign(evaluated.value().positions.begin(), evaluated.value().positions.end());
    mesh.indices = std::move(evaluated.value().indices);
    mesh.exact_surface_area = evaluated.value().exact_surface_area;
    mesh.exact_volume = evaluated.value().exact_volume;
    mesh.has_exact_topology_metrics = evaluated.value().has_exact_metrics;
    return Result<bool>::success(true);
  }

  [[nodiscard]] Result<MeshData> resolved_operative(std::uint32_t object_id,
                                                    OperationGraphState& graph, std::size_t depth,
                                                    const MeshBounds& relevance) {
    if (graph.active.contains(object_id)) {
      ++graph.context_dependent_failures;
      return Result<MeshData>::failure(
          {ErrorCode::invalid_topology, "A Boolean operation graph contains a cycle."});
    }
    const auto key = operative_cache_key(object_id);
    const auto attached = operations_.find(object_id);
    const bool has_attached_operations = attached != operations_.end() && !attached->second.empty();
    if (!has_attached_operations) {
      if (const auto cached = operative_cache_.find(key); cached != operative_cache_.end()) {
        if (cached->second.mesh) return Result<MeshData>::success(*cached->second.mesh);
        return Result<MeshData>::failure(*cached->second.failure);
      }
    }
    const bool fill_envelope =
        std::getenv("TEKLA_DB1_HOLLOW_NESTED_OPERATIVES") == nullptr || depth == 1U;
    auto mesh = operative_mesh(object_id, 0.0, fill_envelope);
    if (!mesh) {
      Error failure{ErrorCode::invalid_geometry, "Boolean operative " + std::to_string(object_id) +
                                                     " has incomplete persisted geometry."};
      cache_operative_failure(key, failure);
      return Result<MeshData>::failure(std::move(failure));
    }
    bool attached_operations_reach_host = false;
    if (has_attached_operations) {
      for (const auto& operation : attached->second) {
        if (operation.type != 11U) {
          attached_operations_reach_host = true;
          break;
        }
        const auto nested = operative_mesh(operation.target_id, 0.0, depth + 1U == 1U);
        const auto nested_bounds = nested ? mesh_bounds(*nested) : std::nullopt;
        if (!nested_bounds || intersects(*nested_bounds, relevance)) {
          attached_operations_reach_host = true;
          break;
        }
      }
    }
    if (!attached_operations_reach_host || std::getenv("TEKLA_DB1_RAW_OPERATIVES") != nullptr) {
      if (!has_attached_operations) cache_operative(key, *mesh);
      return Result<MeshData>::success(std::move(*mesh));
    }
    const auto failure_generation = graph.context_dependent_failures;
    auto evaluated = evaluate_operations(object_id, *mesh, graph, depth, relevance);
    if (!evaluated) {
      if (failure_generation == graph.context_dependent_failures &&
          context_independent_failure(evaluated.error().code)) {
        if (!has_attached_operations) cache_operative_failure(key, evaluated.error());
      }
      return Result<MeshData>::failure(std::move(evaluated.error()));
    }
    return Result<MeshData>::success(std::move(*mesh));
  }

  [[nodiscard]] Result<bool> evaluate_operations(
      std::uint32_t object_id, MeshData& mesh, OperationGraphState& graph, std::size_t depth,
      std::optional<MeshBounds> relevance = std::nullopt) {
    const auto found = operations_.find(object_id);
    if (found == operations_.end() || found->second.empty()) {
      return Result<bool>::success(false);
    }
    if (depth >= maximum_operation_graph_depth) {
      ++graph.context_dependent_failures;
      return Result<bool>::failure(
          {ErrorCode::resource_limit, "A Boolean operation graph exceeds the depth limit."});
    }
    if (!graph.active.insert(object_id).second) {
      ++graph.context_dependent_failures;
      return Result<bool>::failure(
          {ErrorCode::invalid_topology, "A Boolean operation graph contains a cycle."});
    }
    ActiveOperationGuard guard{graph.active, object_id};
    if (graph.nodes >= maximum_operation_graph_nodes) {
      ++graph.context_dependent_failures;
      return Result<bool>::failure(
          {ErrorCode::resource_limit, "A Boolean operation graph exceeds the node limit."});
    }
    ++graph.nodes;
    if (found->second.size() > maximum_operation_graph_edges - graph.edges) {
      ++graph.context_dependent_failures;
      return Result<bool>::failure(
          {ErrorCode::resource_limit, "A Boolean operation graph exceeds the edge limit."});
    }
    graph.edges += found->second.size();

    std::size_t topology_request_mesh_bytes = mesh_storage_bytes(mesh);
    if (topology_request_mesh_bytes > maximum_topology_request_mesh_bytes) {
      ++graph.context_dependent_failures;
      return Result<bool>::failure(
          {ErrorCode::resource_limit, "A topology request exceeds its mesh-memory limit."});
    }
    OcctRequest request;
    request.object_id = object_id;
    request.linear_deflection = operative_linear_deflection;
    request.angular_deflection = operative_angular_deflection;
    make_bounded_topology_base(object_id, mesh, request);
    Vector3d minimum{mesh.positions[0], mesh.positions[1], mesh.positions[2]};
    Vector3d maximum = minimum;
    for (std::size_t index = 3; index < mesh.positions.size(); index += 3U) {
      minimum.x = std::min(minimum.x, static_cast<double>(mesh.positions[index]));
      minimum.y = std::min(minimum.y, static_cast<double>(mesh.positions[index + 1U]));
      minimum.z = std::min(minimum.z, static_cast<double>(mesh.positions[index + 2U]));
      maximum.x = std::max(maximum.x, static_cast<double>(mesh.positions[index]));
      maximum.y = std::max(maximum.y, static_cast<double>(mesh.positions[index + 1U]));
      maximum.z = std::max(maximum.z, static_cast<double>(mesh.positions[index + 2U]));
    }
    const MeshBounds operation_relevance = relevance.value_or(MeshBounds{minimum, maximum});
    const Vector3d midpoint = scale(add(minimum, maximum), 0.5);
    struct PlaneCandidate {
      OcctHalfSpace plane;
      GeometryOperation operation;
    };
    std::vector<PlaneCandidate> planes;

    struct BooleanCutter {
      std::uint32_t object_id = 0;
      MeshData mesh;
      double volume = 0.0;
    };
    std::vector<BooleanCutter> boolean_cutters;
    std::unordered_set<std::uint32_t> boolean_targets;
    for (const auto& operation : found->second) {
      if (operation.type != 11U) continue;
      // Subtracting one persisted operative more than once is idempotent. In
      // addition to avoiding redundant kernel work, this prevents repeated
      // relation rows from multiplying an otherwise shared cached mesh.
      if (!boolean_targets.insert(operation.target_id).second) continue;
      auto cutter = resolved_operative(operation.target_id, graph, depth + 1U, operation_relevance);
      if (!cutter) {
        ++graph.partial_results;
        add_diagnostic(cutter.error().code, object_id,
                       "Boolean operative " + std::to_string(operation.target_id) +
                           " was omitted: " + cutter.error().message);
        continue;
      }
      const auto bytes = mesh_storage_bytes(cutter.value());
      if (bytes > maximum_topology_request_mesh_bytes - topology_request_mesh_bytes) {
        ++graph.context_dependent_failures;
        return Result<bool>::failure(
            {ErrorCode::resource_limit, "A topology request exceeds its mesh-memory limit."});
      }
      topology_request_mesh_bytes += bytes;
      const double volume = mesh_volume(cutter.value());
      boolean_cutters.push_back({operation.target_id, std::move(cutter.value()), volume});
    }
    if (boolean_cutters.size() > 1U) {
      const auto [minimum_volume, maximum_volume] =
          std::minmax_element(boolean_cutters.begin(), boolean_cutters.end(),
                              [](const BooleanCutter& left, const BooleanCutter& right) {
                                return left.volume < right.volume;
                              });
      const double tolerance = std::max(1.0, maximum_volume->volume * 1.0e-8);
      const bool uniform_cutter_cohort =
          maximum_volume->volume > 0.0 &&
          maximum_volume->volume - minimum_volume->volume <= tolerance;
      const double through_interval_extension = uniform_cutter_cohort ? 1.0e-3 : 1.0e-5;
      for (auto& cutter : boolean_cutters) {
        expand_tangent_cutter(object_id, cutter.object_id, mesh, cutter.mesh,
                              through_interval_extension);
      }
    } else {
      for (auto& cutter : boolean_cutters) {
        expand_tangent_cutter(object_id, cutter.object_id, mesh, cutter.mesh, 1.0e-5);
      }
    }
    std::stable_sort(boolean_cutters.begin(), boolean_cutters.end(),
                     [](const BooleanCutter& left, const BooleanCutter& right) {
                       return left.volume > right.volume;
                     });
    for (auto& cutter : boolean_cutters) {
      request.subtract_meshes.push_back(take_occt_mesh(std::move(cutter.mesh)));
    }

    for (const auto& operation : found->second) {
      if (operation.type == 79U) {
        if (auto cutter = edge_chamfer_cutter(object_id, mesh, operation.target_id)) {
          const auto bytes = mesh_storage_bytes(*cutter);
          if (bytes > maximum_topology_request_mesh_bytes - topology_request_mesh_bytes) {
            ++graph.context_dependent_failures;
            return Result<bool>::failure(
                {ErrorCode::resource_limit, "A topology request exceeds its mesh-memory limit."});
          }
          topology_request_mesh_bytes += bytes;
          request.subtract_meshes.push_back(take_occt_mesh(std::move(*cutter)));
        } else {
          add_diagnostic(ErrorCode::decoder_unavailable, object_id,
                         "A persisted edge chamfer is outside the supported "
                         "straight-corner slice.");
        }
        continue;
      }
      if (operation.type == 11U) continue;
      if (operation.type != 9U && operation.type != 12U) continue;
      const auto coordinate_system = coordinate_systems_.find(operation.target_id);
      if (coordinate_system == coordinate_systems_.end()) continue;
      const auto axes = axes_.find(coordinate_system->second.axes_id);
      if (axes == axes_.end()) continue;
      auto normal = normalized(cross(axes->second.x, axes->second.y));
      if (!normal) continue;
      if (operation.type == 12U) {
        // Legacy cutting planes retain the half-space opposite the persisted
        // coordinate-system normal.  Unlike a fitting, a cut can sit well
        // away from the uncut member midpoint, so midpoint orientation keeps
        // the wrong (usually much longer) side of the member.
        normal = scale(*normal, -1.0);
      } else if (dot(subtract(midpoint, coordinate_system->second.origin), *normal) < 0.0) {
        normal = scale(*normal, -1.0);
      }
      PlaneCandidate candidate{
          {coordinate_system->second.origin.x, coordinate_system->second.origin.y,
           coordinate_system->second.origin.z, normal->x, normal->y, normal->z},
          operation};
      bool replaced = false;
      if (operation.type == 9U) {
        for (auto& existing : planes) {
          const Vector3d existing_normal{existing.plane.normal_x, existing.plane.normal_y,
                                         existing.plane.normal_z};
          if (existing.operation.type != 9U || dot(existing_normal, *normal) < 1.0 - 1.0e-8) {
            continue;
          }
          if (std::pair{operation.event_id, operation.relation_id} >
              std::pair{existing.operation.event_id, existing.operation.relation_id}) {
            existing = candidate;
          }
          replaced = true;
          break;
        }
      }
      if (!replaced) planes.push_back(candidate);
    }
    for (const auto& plane : planes) request.keep_half_spaces.push_back(plane.plane);
    if (request.subtract_meshes.empty() && request.keep_half_spaces.empty()) {
      return Result<bool>::success(false);
    }
    Result<OcctMesh> evaluated =
        topology_mode_ == TopologyMode::supervised
            ? supervised_host_ != nullptr
                  ? supervised_host_->evaluate(request)
                  : Result<OcctMesh>::failure(
                        {ErrorCode::invalid_argument,
                         "A supervised topology worker path was not supplied."})
            : direct_host_.evaluate(request);
    if (!evaluated) {
      return Result<bool>::failure(std::move(evaluated.error()));
    }
    mesh.positions.assign(evaluated.value().positions.begin(), evaluated.value().positions.end());
    mesh.indices = std::move(evaluated.value().indices);
    return Result<bool>::success(true);
  }
#endif

  void apply_operations(std::uint32_t object_id, MeshData& mesh) {
    const auto found = operations_.find(object_id);
    if (found == operations_.end() || found->second.empty()) return;
    if (topology_mode_ == TopologyMode::disabled) {
      diagnostics_.push_back(
          {ErrorCode::decoder_unavailable, object_id,
           "Persisted fitting and Boolean operations require topology evaluation."});
      return;
    }
#if !defined(TEKLA_DB1_HAS_OCCT)
    (void)mesh;
    diagnostics_.push_back({ErrorCode::decoder_unavailable, object_id,
                            "This build does not contain the optional topology evaluator."});
#else
    OperationGraphState graph;
    auto evaluated = evaluate_operations_csg(object_id, mesh, graph);
    if (!evaluated) {
      add_diagnostic(evaluated.error().code, object_id, std::move(evaluated.error().message));
    }
#endif
  }

  std::shared_ptr<const ModelStorage> storage_;
  const TableLayout* parts_ = nullptr;
  const TableSchema* part_schema_ = nullptr;
  std::unordered_map<std::uint32_t, Attribute> attributes_;
  std::unordered_map<std::uint32_t, Axes> axes_;
  std::unordered_map<std::uint32_t, CoordinateSystem> coordinate_systems_;
  std::unordered_map<std::uint32_t, Vector3d> points_;
  std::unordered_map<std::uint32_t, Chamfer> chamfers_;
  std::unordered_set<std::uint32_t> boolean_operatives_;
  std::unordered_map<std::uint32_t, std::vector<GeometryOperation>> operations_;
  std::unordered_map<std::uint32_t, std::uint64_t> part_rows_;
  std::unordered_map<std::uint32_t, Contour> contours_;
  std::unordered_map<std::uint32_t, Contour> paths_;
  std::unordered_map<std::uint32_t, LoftRails> loft_rails_;
  LocalProfileCatalog profiles_;
  ShapeCatalog shapes_;
  std::array<std::uint32_t, 9> offsets_{};
  bool emit_definitions_ = false;
  bool emit_meshes_ = false;
  std::size_t batch_size_ = 0;
  TopologyMode topology_mode_ = TopologyMode::disabled;
  std::uint64_t object_id_min_ = 0;
  std::uint64_t object_id_max_ = std::numeric_limits<std::uint64_t>::max();
#if defined(TEKLA_DB1_HAS_OCCT)
  std::unordered_map<OperativeCacheKey, OperativeCacheEntry, OperativeCacheKeyHash>
      operative_cache_;
  std::size_t operative_cache_bytes_ = 0U;
  std::unordered_map<std::vector<std::byte>, TopologyResultCacheEntry, TopologyResultCacheKeyHash>
      topology_result_cache_;
  std::size_t topology_result_cache_bytes_ = 0U;
  DirectOcctHost direct_host_;
  std::unique_ptr<SupervisedOcctHost> supervised_host_;
#endif
  std::uint64_t row_ = 0;
  int pending_ = 3;
  std::vector<DefinitionGeometryView> definitions_;
  std::vector<std::vector<Vector3d>> definition_paths_;
  std::unordered_map<std::uint32_t, ExtrusionRecipe> extrusion_recipes_;
  std::vector<MeshData> mesh_data_;
  std::vector<std::vector<float>> mesh_positions_;
  std::vector<MeshView> meshes_;
  std::vector<Diagnostic> diagnostics_;
  std::deque<std::string> diagnostic_messages_;
};

}  // namespace

Result<ProcessStream> make_geometry_stream(std::shared_ptr<const ModelStorage> storage,
                                           const Schema& schema, const ProcessRequest& request) {
  const auto* parts = schema.find_table("part");
  constexpr std::array<std::string_view, 10> attribute_names{
      "part_attr",         "old_part_attr_911", "old_part_attr_866", "old_part_attr_801",
      "old_part_attr_787", "old_part_attr_784", "old_part_attr_742", "old_part_attr_650",
      "old_part_attr_644", "old_part_attr_622"};
  const auto* attributes = populated_table(*storage, schema, attribute_names);
  const auto* coordinate_systems = schema.find_table("coordsys_attr");
  if (parts == nullptr || attributes == nullptr || coordinate_systems == nullptr) {
    return Result<ProcessStream>::failure(
        {ErrorCode::schema_mismatch, "Straight-part geometry tables are unavailable."});
  }
  constexpr std::array<std::string_view, 9> part_fields{
      "id",          "part_attr_id", "csys_attr_id",      "csys_x", "csys_y", "csys_z",
      "csys_length", "p1",           "GeometryTreeRootId"};
  std::array<std::uint32_t, 9> offsets{};
  for (std::size_t index = 0; index < part_fields.size(); ++index) {
    const auto* field = find_field(schema, *parts, part_fields[index]);
    if (field == nullptr)
      return Result<ProcessStream>::failure(
          {ErrorCode::schema_mismatch, "The straight-part frame layout is incomplete."});
    offsets[index] = field->offset;
  }

  std::unordered_map<std::uint32_t, std::string> string_segments;
  std::unordered_map<std::uint32_t, std::uint32_t> string_next;
  if (const auto* strings = schema.find_table("string")) {
    const auto* id = find_field(schema, *strings, "id");
    const auto* next = find_field(schema, *strings, "next_id");
    const auto* value = find_field(schema, *strings, "string");
    const auto& layout = storage->layout.tables[strings->ordinal];
    if (id != nullptr && next != nullptr && value != nullptr) {
      string_segments.reserve(static_cast<std::size_t>(layout.info.row_count));
      string_next.reserve(static_cast<std::size_t>(layout.info.row_count));
      for (std::uint64_t row = 0; row < layout.info.row_count; ++row) {
        const auto record = layout.record(storage->payload.bytes(), row);
        if (record.empty()) continue;
        const auto tuple = record.subspan(1, strings->tuple_size);
        const auto record_id = read_u32(tuple, id->offset);
        string_segments.insert_or_assign(record_id,
                                         std::string(read_text(tuple, value->offset, value->size)));
        string_next.insert_or_assign(record_id, read_u32(tuple, next->offset));
      }
    }
  }
  const auto resolve_string = [&](std::uint32_t id) {
    std::string result;
    for (std::size_t count = 0; id != 0 && count <= string_segments.size(); ++count) {
      const auto segment = string_segments.find(id);
      if (segment == string_segments.end()) break;
      result += segment->second;
      const auto next = string_next.find(id);
      id = next == string_next.end() ? 0 : next->second;
    }
    return result;
  };

  const auto* attr_id = find_field(schema, *attributes, "id");
  const auto* profile = find_field(schema, *attributes, "prof");
  if (profile == nullptr) profile = find_field(schema, *attributes, "Geometry");
  const auto* parameter = find_field(schema, *attributes, "ParameterStringId");
  const auto* form_type = find_field(schema, *attributes, "form_type");
  const auto* object_type = find_field(schema, *attributes, "obj_type");
  const auto* object_class = find_field(schema, *attributes, "ryhma");
  if (attr_id == nullptr || profile == nullptr)
    return Result<ProcessStream>::failure(
        {ErrorCode::schema_mismatch, "The part profile layout is incomplete."});
  std::unordered_map<std::uint32_t, Attribute> attribute_map;
  const auto& attribute_layout = storage->layout.tables[attributes->ordinal];
  attribute_map.reserve(static_cast<std::size_t>(attribute_layout.info.row_count));
  for (std::uint64_t row = 0; row < attribute_layout.info.row_count; ++row) {
    const auto record = attribute_layout.record(storage->payload.bytes(), row);
    if (record.empty()) continue;
    const auto tuple = record.subspan(1, attributes->tuple_size);
    std::string full_profile(read_text(tuple, profile->offset, profile->size));
    if (parameter != nullptr) full_profile += resolve_string(read_u32(tuple, parameter->offset));
    attribute_map.insert_or_assign(
        read_u32(tuple, attr_id->offset),
        Attribute{std::move(full_profile),
                  object_class == nullptr
                      ? std::string{}
                      : std::string(read_text(tuple, object_class->offset, object_class->size)),
                  object_type == nullptr ? 0U : read_u32(tuple, object_type->offset),
                  form_type == nullptr ? 0U : read_u32(tuple, form_type->offset)});
  }

  constexpr std::array<std::string_view, 7> axis_fields{"id",     "xdir_x", "xdir_y", "xdir_z",
                                                        "ydir_x", "ydir_y", "ydir_z"};
  std::array<const FieldSchema*, 7> axis_offsets{};
  for (std::size_t index = 0; index < axis_fields.size(); ++index) {
    axis_offsets[index] = find_field(schema, *coordinate_systems, axis_fields[index]);
    if (axis_offsets[index] == nullptr)
      return Result<ProcessStream>::failure(
          {ErrorCode::schema_mismatch, "The coordinate-system layout is incomplete."});
  }
  std::unordered_map<std::uint32_t, Axes> axes_map;
  const auto& axes_layout = storage->layout.tables[coordinate_systems->ordinal];
  axes_map.reserve(static_cast<std::size_t>(axes_layout.info.row_count));
  for (std::uint64_t row = 0; row < axes_layout.info.row_count; ++row) {
    const auto record = axes_layout.record(storage->payload.bytes(), row);
    if (record.empty()) continue;
    const auto tuple = record.subspan(1, coordinate_systems->tuple_size);
    axes_map.insert_or_assign(
        read_u32(tuple, axis_offsets[0]->offset),
        Axes{{read_f64(tuple, axis_offsets[1]->offset), read_f64(tuple, axis_offsets[2]->offset),
              read_f64(tuple, axis_offsets[3]->offset)},
             {read_f64(tuple, axis_offsets[4]->offset), read_f64(tuple, axis_offsets[5]->offset),
              read_f64(tuple, axis_offsets[6]->offset)}});
  }
  std::unordered_map<std::uint32_t, CoordinateSystem> coordinate_system_map;
  if (const auto* coordinate_system_table = schema.find_table("coordsys")) {
    const auto* id = find_field(schema, *coordinate_system_table, "id");
    const auto* axes_id = find_field(schema, *coordinate_system_table, "csys_attr_id");
    const auto* x = find_field(schema, *coordinate_system_table, "x1");
    const auto* y = find_field(schema, *coordinate_system_table, "y1");
    const auto* z = find_field(schema, *coordinate_system_table, "z1");
    const auto* length = find_field(schema, *coordinate_system_table, "length");
    const auto& layout = storage->layout.tables[coordinate_system_table->ordinal];
    if (id != nullptr && axes_id != nullptr && x != nullptr && y != nullptr && z != nullptr) {
      coordinate_system_map.reserve(static_cast<std::size_t>(layout.info.row_count));
      for (std::uint64_t row = 0; row < layout.info.row_count; ++row) {
        const auto record = layout.record(storage->payload.bytes(), row);
        if (record.empty()) continue;
        const auto tuple = record.subspan(1, coordinate_system_table->tuple_size);
        coordinate_system_map.insert_or_assign(
            read_u32(tuple, id->offset),
            CoordinateSystem{read_u32(tuple, axes_id->offset),
                             {scalar(tuple, *x), scalar(tuple, *y), scalar(tuple, *z)},
                             length == nullptr ? 0.0 : scalar(tuple, *length)});
      }
    }
  }
  std::unordered_map<std::uint32_t, Vector3d> point_map;
  if (const auto* points = schema.find_table("point")) {
    const auto* id = find_field(schema, *points, "id");
    const auto* x = find_field(schema, *points, "x");
    const auto* y = find_field(schema, *points, "y");
    const auto* z = find_field(schema, *points, "z");
    const auto& layout = storage->layout.tables[points->ordinal];
    if (id != nullptr && x != nullptr && y != nullptr && z != nullptr) {
      point_map.reserve(static_cast<std::size_t>(layout.info.row_count));
      for (std::uint64_t row = 0; row < layout.info.row_count; ++row) {
        const auto record = layout.record(storage->payload.bytes(), row);
        if (record.empty()) continue;
        const auto tuple = record.subspan(1, points->tuple_size);
        point_map.insert_or_assign(
            read_u32(tuple, id->offset),
            Vector3d{scalar(tuple, *x), scalar(tuple, *y), scalar(tuple, *z)});
      }
    }
  }
  std::unordered_map<std::uint32_t, Chamfer> chamfer_map;
  if (const auto* chamfers = schema.find_table("chamfer")) {
    const auto* id = find_field(schema, *chamfers, "id");
    const auto* type = find_field(schema, *chamfers, "type");
    const auto* x = find_field(schema, *chamfers, "x");
    const auto* y = find_field(schema, *chamfers, "y");
    const auto* end_types = find_field(schema, *chamfers, "endtypes");
    const auto* first = find_field(schema, *chamfers, "first_end_dimension");
    const auto* second = find_field(schema, *chamfers, "second_end_dimension");
    const auto& layout = storage->layout.tables[chamfers->ordinal];
    if (id != nullptr && type != nullptr && x != nullptr && y != nullptr && end_types != nullptr &&
        first != nullptr && second != nullptr) {
      chamfer_map.reserve(static_cast<std::size_t>(layout.info.row_count));
      for (std::uint64_t row = 0; row < layout.info.row_count; ++row) {
        const auto record = layout.record(storage->payload.bytes(), row);
        if (record.empty() || (std::to_integer<std::uint8_t>(record[0]) & 0x08U) != 0U) continue;
        const auto tuple = record.subspan(1, chamfers->tuple_size);
        chamfer_map.insert_or_assign(read_u32(tuple, id->offset),
                                     Chamfer{read_u32(tuple, type->offset), scalar(tuple, *x),
                                             scalar(tuple, *y), read_u32(tuple, end_types->offset),
                                             scalar(tuple, *first), scalar(tuple, *second)});
      }
    }
  }
  std::unordered_set<std::uint32_t> boolean_operatives;
  std::unordered_map<std::uint32_t, std::vector<GeometryOperation>> operations;
  if (const auto* relations = schema.find_table("relation")) {
    const auto* relation_id = find_field(schema, *relations, "id");
    const auto* type = find_field(schema, *relations, "type");
    const auto* source = find_field(schema, *relations, "id1");
    const auto* target = find_field(schema, *relations, "id2");
    const auto& relation_layout = storage->layout.tables[relations->ordinal];
    if (relation_id != nullptr && type != nullptr && source != nullptr && target != nullptr) {
      for (std::uint64_t row = 0; row < relation_layout.info.row_count; ++row) {
        const auto record = relation_layout.record(storage->payload.bytes(), row);
        if (record.empty() || (std::to_integer<std::uint8_t>(record[0]) & 0x08U) != 0U) continue;
        const auto tuple = record.subspan(1, relations->tuple_size);
        const auto operation_type = read_u32(tuple, type->offset);
        if (operation_type != 9U && operation_type != 11U && operation_type != 12U &&
            operation_type != 79U)
          continue;
        const auto target_id = read_u32(tuple, target->offset);
        if (operation_type == 79U && !chamfer_map.contains(target_id)) continue;
        if (operation_type == 11U) boolean_operatives.insert(target_id);
        operations[read_u32(tuple, source->offset)].push_back(
            {operation_type, target_id, read_u32(tuple, relation_id->offset),
             read_u32(record, 1 + relations->tuple_size + 4U)});
      }
    }
  }
  std::unordered_map<std::uint32_t, std::uint32_t> contour_geometry_by_root;
  std::unordered_map<std::uint32_t, std::uint32_t> path_geometry_by_root;
  std::unordered_map<std::uint32_t, GeometryNode> geometry_nodes;
  std::unordered_map<std::uint32_t, std::vector<std::uint32_t>> geometry_children;
  if (const auto* nodes = schema.find_table("geometry_tree_node")) {
    const auto* id = find_field(schema, *nodes, "Id");
    const auto* parent = find_field(schema, *nodes, "ParentId");
    const auto* type = find_field(schema, *nodes, "Type");
    const auto* subtype = find_field(schema, *nodes, "SubType");
    const auto* geometry_id = find_field(schema, *nodes, "GeometryId");
    const auto* coordinate_system_id = find_field(schema, *nodes, "CoordSysId");
    const auto& layout = storage->layout.tables[nodes->ordinal];
    if (id != nullptr && parent != nullptr && type != nullptr && subtype != nullptr &&
        geometry_id != nullptr && coordinate_system_id != nullptr) {
      for (std::uint64_t row = 0; row < layout.info.row_count; ++row) {
        const auto record = layout.record(storage->payload.bytes(), row);
        if (record.empty()) continue;
        const auto tuple = record.subspan(1, nodes->tuple_size);
        const GeometryNode node{
            read_u32(tuple, id->offset),          read_u32(tuple, parent->offset),
            read_u32(tuple, type->offset),        read_u32(tuple, subtype->offset),
            read_u32(tuple, geometry_id->offset), read_u32(tuple, coordinate_system_id->offset)};
        geometry_nodes.insert_or_assign(node.id, node);
        geometry_children[node.parent_id].push_back(node.id);
        if (node.parent_id == 0U) {
          const auto node_type = node.type;
          auto* roots = node_type == 1U   ? &contour_geometry_by_root
                        : node_type == 3U ? &path_geometry_by_root
                                          : nullptr;
          if (roots != nullptr) {
            roots->insert_or_assign(node.id, node.geometry_id);
          }
        }
      }
    }
  }

  constexpr std::array<std::string_view, 2> double_array_names{"double_array",
                                                               "old_double_array_738"};
  const auto* double_arrays = populated_table(*storage, schema, double_array_names);
  std::unordered_map<std::uint32_t, DoubleArrayRecord> double_array_records;
  if (double_arrays != nullptr) {
    const auto* id = find_field(schema, *double_arrays, "id");
    const auto* next = find_field(schema, *double_arrays, "next_id");
    const auto* value_count = find_field(schema, *double_arrays, "n_values");
    const auto& layout = storage->layout.tables[double_arrays->ordinal];
    if (id != nullptr && next != nullptr && value_count != nullptr) {
      for (std::uint64_t row = 0; row < layout.info.row_count; ++row) {
        const auto record = layout.record(storage->payload.bytes(), row);
        if (record.empty()) continue;
        const auto tuple = record.subspan(1, double_arrays->tuple_size);
        DoubleArrayRecord values;
        values.next_id = read_u32(tuple, next->offset);
        values.value_count = read_u32(tuple, value_count->offset);
        for (std::size_t index = 0; index < values.values.size(); ++index) {
          const auto* value = find_field(schema, *double_arrays, "value_" + std::to_string(index));
          if (value == nullptr) break;
          values.values[index] = scalar(tuple, *value);
        }
        double_array_records.insert_or_assign(read_u32(tuple, id->offset), values);
      }
    }
  }

  constexpr std::array<std::string_view, 2> polygon_names{"partpolygon", "old_partpolygon_898"};
  const auto* polygons = populated_table(*storage, schema, polygon_names);
  std::unordered_map<std::uint32_t, std::vector<PolygonChunk>> polygon_chunks;
  if (polygons != nullptr) {
    const auto* id = find_field(schema, *polygons, "id");
    const auto* number_field = find_field(schema, *polygons, "no");
    const auto& layout = storage->layout.tables[polygons->ordinal];
    if (id != nullptr) {
      for (std::uint64_t row = 0; row < layout.info.row_count; ++row) {
        const auto record = layout.record(storage->payload.bytes(), row);
        if (record.empty()) continue;
        const auto tuple = record.subspan(1, polygons->tuple_size);
        PolygonChunk chunk;
        chunk.number = number_field == nullptr ? 0U : read_u32(tuple, number_field->offset);
        for (std::size_t index = 1; index <= 10; ++index) {
          const auto suffix = std::to_string(index);
          const auto* x = find_field(schema, *polygons, "x" + suffix);
          const auto* y = find_field(schema, *polygons, "y" + suffix);
          const auto* z = find_field(schema, *polygons, "z" + suffix);
          const auto* dx = find_field(schema, *polygons, "dx" + suffix);
          const auto* dy = find_field(schema, *polygons, "dy" + suffix);
          const auto* type = find_field(schema, *polygons, "types" + suffix);
          if (x == nullptr || y == nullptr || dx == nullptr || dy == nullptr || type == nullptr)
            break;
          const auto corner_type = read_u32(tuple, type->offset);
          if (corner_type == 2'147'483'647U) break;
          chunk.points.push_back(
              {scalar(tuple, *x), scalar(tuple, *y), z == nullptr ? 0.0 : scalar(tuple, *z)});
          chunk.dx.push_back(scalar(tuple, *dx));
          chunk.dy.push_back(scalar(tuple, *dy));
          chunk.types.push_back(corner_type);
        }
        if (!chunk.points.empty()) {
          polygon_chunks[read_u32(tuple, id->offset)].push_back(std::move(chunk));
        }
      }
    }
  }
  std::unordered_map<std::uint32_t, Contour> contours;
  const auto assemble_polygons = [&](const auto& roots) {
    std::unordered_map<std::uint32_t, Contour> result;
    result.reserve(roots.size());
    for (const auto& [root, geometry_id] : roots) {
      const auto chunks = polygon_chunks.find(geometry_id);
      if (chunks == polygon_chunks.end()) continue;
      std::sort(
          chunks->second.begin(), chunks->second.end(),
          [](const PolygonChunk& lhs, const PolygonChunk& rhs) { return lhs.number < rhs.number; });
      Contour contour;
      for (const auto& chunk : chunks->second) {
        contour.points.insert(contour.points.end(), chunk.points.begin(), chunk.points.end());
        contour.dx.insert(contour.dx.end(), chunk.dx.begin(), chunk.dx.end());
        contour.dy.insert(contour.dy.end(), chunk.dy.begin(), chunk.dy.end());
        contour.types.insert(contour.types.end(), chunk.types.begin(), chunk.types.end());
      }
      result.insert_or_assign(root, std::move(contour));
    }
    return result;
  };
  contours = assemble_polygons(contour_geometry_by_root);
  auto paths = assemble_polygons(path_geometry_by_root);
  std::unordered_map<std::uint32_t, LoftRails> loft_rails;
  for (const auto& [root_id, root] : geometry_nodes) {
    if (root.parent_id != 0U || root.type != 1000U || root.subtype != 1000U ||
        root.geometry_id != 0U || root.coordinate_system_id != 0U) {
      continue;
    }
    const auto root_children = geometry_children.find(root_id);
    if (root_children == geometry_children.end()) continue;
    const GeometryNode* rail_container = nullptr;
    for (const auto child_id : root_children->second) {
      const auto child = geometry_nodes.find(child_id);
      if (child != geometry_nodes.end() && child->second.type == 1000U &&
          child->second.subtype == 1001U) {
        rail_container = &child->second;
      }
    }
    if (rail_container == nullptr) continue;
    const auto heads = geometry_children.find(rail_container->id);
    if (heads == geometry_children.end() || heads->second.size() != 2U) continue;
    LoftRails decoded;
    bool valid = true;
    for (std::size_t rail = 0; rail < 2U && valid; ++rail) {
      auto node_id = heads->second[rail];
      std::unordered_set<std::uint32_t> seen;
      while (node_id != 0U && seen.insert(node_id).second) {
        const auto node = geometry_nodes.find(node_id);
        if (node == geometry_nodes.end() || node->second.type != 6U || node->second.subtype != 0U ||
            node->second.coordinate_system_id != 0U) {
          valid = false;
          break;
        }
        const auto values = double_array_records.find(node->second.geometry_id);
        if (values == double_array_records.end() || values->second.value_count != 6U) {
          valid = false;
          break;
        }
        const Vector3d start{values->second.values[0], values->second.values[1],
                             values->second.values[2]};
        const Vector3d end{values->second.values[3], values->second.values[4],
                           values->second.values[5]};
        if (!decoded.rails[rail].empty() &&
            length(subtract(decoded.rails[rail].back(), start)) > 1.0e-4) {
          valid = false;
          break;
        }
        if (decoded.rails[rail].empty()) decoded.rails[rail].push_back(start);
        decoded.rails[rail].push_back(end);
        const auto children = geometry_children.find(node_id);
        if (children == geometry_children.end() || children->second.empty()) {
          node_id = 0U;
        } else if (children->second.size() == 1U) {
          node_id = children->second.front();
        } else {
          valid = false;
        }
      }
    }
    if (valid) loft_rails.insert_or_assign(root_id, std::move(decoded));
  }
  const auto* parts_layout = &storage->layout.tables[parts->ordinal];
  std::unordered_map<std::uint32_t, std::uint64_t> part_rows;
  part_rows.reserve(static_cast<std::size_t>(parts_layout->info.row_count));
  for (std::uint64_t row = 0; row < parts_layout->info.row_count; ++row) {
    const auto record = parts_layout->record(storage->payload.bytes(), row);
    if (record.empty()) continue;
    part_rows.insert_or_assign(read_u32(record.subspan(1, parts->tuple_size), offsets[0]), row);
  }
  LocalProfileCatalog profiles(storage->package);
  ShapeCatalog shapes(storage->package);
  return Result<ProcessStream>::success(std::make_unique<GeometryReader>(
      std::move(storage), *parts_layout, *parts, std::move(attribute_map), std::move(axes_map),
      std::move(coordinate_system_map), std::move(point_map), std::move(chamfer_map),
      std::move(boolean_operatives), std::move(operations), std::move(part_rows),
      std::move(contours), std::move(paths), std::move(loft_rails), std::move(profiles),
      std::move(shapes), offsets, contains(request.stages, Stage::definition_geometry),
      contains(request.stages, Stage::display_geometry),
      request.batch_memory_budget_bytes == 0
          ? 256U
          : static_cast<std::size_t>(
                std::clamp<std::uint64_t>(request.batch_memory_budget_bytes / 32768U, 1, 4096)),
      request.topology_mode, std::string(request.topology_worker_path),
      request.topology_timeout_milliseconds, request.geometry_object_id_min,
      request.geometry_object_id_max));
}

}  // namespace tekla::db1::detail

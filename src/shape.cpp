#include "shape.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <zlib.h>

namespace tekla::db1::detail {
namespace {

constexpr std::size_t maximum_shape_bytes = 512U * 1024U * 1024U;
constexpr double geometry_epsilon = 1e-10;
using Point2 = std::array<double, 2>;

[[nodiscard]] std::string normalized_id(std::string_view value) {
  while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front())) != 0) {
    value.remove_prefix(1);
  }
  while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back())) != 0) {
    value.remove_suffix(1);
  }
  if (value.size() >= 2 && value.front() == '{' && value.back() == '}') {
    value.remove_prefix(1);
    value.remove_suffix(1);
  }
  std::string result(value);
  std::transform(result.begin(), result.end(), result.begin(), [](unsigned char byte) {
    return static_cast<char>(std::tolower(byte));
  });
  return result;
}

[[nodiscard]] std::string normalized_path(std::string_view value) {
  std::string result(value);
  std::replace(result.begin(), result.end(), '\\', '/');
  std::transform(result.begin(), result.end(), result.begin(), [](unsigned char byte) {
    return static_cast<char>(std::tolower(byte));
  });
  return result;
}

[[nodiscard]] std::string stem(std::string_view path) {
  const auto slash = path.find_last_of('/');
  if (slash != std::string_view::npos) path.remove_prefix(slash + 1U);
  const auto dot = path.find_last_of('.');
  if (dot != std::string_view::npos) path = path.substr(0, dot);
  return normalized_id(path);
}

[[nodiscard]] std::optional<std::string_view> element(
    std::string_view xml, std::string_view tag, std::size_t from = 0) {
  const std::string opening = "<" + std::string(tag);
  const auto begin = xml.find(opening, from);
  if (begin == std::string_view::npos) return std::nullopt;
  const auto content = xml.find('>', begin + opening.size());
  if (content == std::string_view::npos) return std::nullopt;
  const std::string closing = "</" + std::string(tag) + ">";
  const auto end = xml.find(closing, content + 1U);
  if (end == std::string_view::npos) return std::nullopt;
  return xml.substr(content + 1U, end - content - 1U);
}

[[nodiscard]] std::vector<std::string_view> elements(
    std::string_view xml, std::string_view tag) {
  std::vector<std::string_view> result;
  const std::string opening = "<" + std::string(tag);
  const std::string closing = "</" + std::string(tag) + ">";
  std::size_t cursor = 0;
  while (true) {
    const auto begin = xml.find(opening, cursor);
    if (begin == std::string_view::npos) break;
    const auto content = xml.find('>', begin + opening.size());
    if (content == std::string_view::npos) break;
    const auto end = xml.find(closing, content + 1U);
    if (end == std::string_view::npos) break;
    result.push_back(xml.substr(content + 1U, end - content - 1U));
    cursor = end + closing.size();
  }
  return result;
}

[[nodiscard]] std::optional<double> floating(std::string_view value) {
  double result = 0.0;
  const auto parsed = std::from_chars(value.data(), value.data() + value.size(), result);
  if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() ||
      !std::isfinite(result)) return std::nullopt;
  return result;
}

[[nodiscard]] std::optional<std::uint32_t> index_value(std::string_view value) {
  std::uint32_t result = 0;
  const auto parsed = std::from_chars(value.data(), value.data() + value.size(), result);
  if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size()) {
    return std::nullopt;
  }
  return result;
}

[[nodiscard]] Result<std::vector<std::byte>> inflate_shape(
    std::span<const std::byte> bytes) {
  z_stream stream{};
  if (inflateInit2(&stream, 16 + MAX_WBITS) != Z_OK) {
    return Result<std::vector<std::byte>>::failure(
        {ErrorCode::decompression_failed, "Could not initialize imported-shape decompression."});
  }
  stream.next_in = reinterpret_cast<Bytef*>(const_cast<std::byte*>(bytes.data()));
  stream.avail_in = static_cast<uInt>(std::min<std::size_t>(
      bytes.size(), std::numeric_limits<uInt>::max()));
  std::vector<std::byte> output;
  std::array<std::byte, 64U * 1024U> chunk{};
  int status = Z_OK;
  while (status == Z_OK) {
    stream.next_out = reinterpret_cast<Bytef*>(chunk.data());
    stream.avail_out = static_cast<uInt>(chunk.size());
    status = inflate(&stream, Z_NO_FLUSH);
    const auto produced = chunk.size() - stream.avail_out;
    if (output.size() > maximum_shape_bytes - produced) {
      inflateEnd(&stream);
      return Result<std::vector<std::byte>>::failure(
          {ErrorCode::resource_limit, "An imported shape exceeds the decode limit."});
    }
    output.insert(output.end(), chunk.begin(), chunk.begin() +
                                             static_cast<std::ptrdiff_t>(produced));
    if (stream.avail_in == 0 && bytes.size() > stream.total_in) {
      const auto remaining = bytes.subspan(stream.total_in);
      stream.next_in = reinterpret_cast<Bytef*>(const_cast<std::byte*>(remaining.data()));
      stream.avail_in = static_cast<uInt>(std::min<std::size_t>(
          remaining.size(), std::numeric_limits<uInt>::max()));
    }
  }
  inflateEnd(&stream);
  if (status != Z_STREAM_END) {
    return Result<std::vector<std::byte>>::failure(
        {ErrorCode::decompression_failed, "An imported-shape gzip stream is invalid."});
  }
  return Result<std::vector<std::byte>>::success(std::move(output));
}

[[nodiscard]] double orientation(Point2 a, Point2 b, Point2 c) noexcept {
  return (b[0] - a[0]) * (c[1] - a[1]) -
         (b[1] - a[1]) * (c[0] - a[0]);
}

[[nodiscard]] double signed_area(std::span<const std::uint32_t> ring,
                                 const std::vector<Point2>& points) noexcept {
  double area = 0.0;
  for (std::size_t index = 0; index < ring.size(); ++index) {
    const auto a = points[ring[index]];
    const auto b = points[ring[(index + 1U) % ring.size()]];
    area += a[0] * b[1] - b[0] * a[1];
  }
  return area / 2.0;
}

[[nodiscard]] bool same(Point2 a, Point2 b) noexcept {
  return std::abs(a[0] - b[0]) <= geometry_epsilon &&
         std::abs(a[1] - b[1]) <= geometry_epsilon;
}

[[nodiscard]] bool on_segment(Point2 a, Point2 b, Point2 value) noexcept {
  return std::abs(orientation(a, b, value)) <= geometry_epsilon &&
         value[0] >= std::min(a[0], b[0]) - geometry_epsilon &&
         value[0] <= std::max(a[0], b[0]) + geometry_epsilon &&
         value[1] >= std::min(a[1], b[1]) - geometry_epsilon &&
         value[1] <= std::max(a[1], b[1]) + geometry_epsilon;
}

[[nodiscard]] bool intersects(Point2 a, Point2 b, Point2 c, Point2 d) noexcept {
  const double ab_c = orientation(a, b, c);
  const double ab_d = orientation(a, b, d);
  const double cd_a = orientation(c, d, a);
  const double cd_b = orientation(c, d, b);
  if (((ab_c > geometry_epsilon && ab_d < -geometry_epsilon) ||
       (ab_c < -geometry_epsilon && ab_d > geometry_epsilon)) &&
      ((cd_a > geometry_epsilon && cd_b < -geometry_epsilon) ||
       (cd_a < -geometry_epsilon && cd_b > geometry_epsilon))) return true;
  return on_segment(a, b, c) || on_segment(a, b, d) ||
         on_segment(c, d, a) || on_segment(c, d, b);
}

[[nodiscard]] bool inside_polygon(Point2 value, std::span<const std::uint32_t> ring,
                                  const std::vector<Point2>& points) noexcept {
  bool inside = false;
  Point2 previous = points[ring.back()];
  for (const auto index : ring) {
    const Point2 current = points[index];
    if ((current[1] > value[1]) != (previous[1] > value[1])) {
      const double crossing = (previous[0] - current[0]) *
                                  (value[1] - current[1]) /
                                  (previous[1] - current[1]) +
                              current[0];
      if (value[0] < crossing) inside = !inside;
    }
    previous = current;
  }
  return inside;
}

[[nodiscard]] bool inside_triangle(Point2 value, Point2 a, Point2 b,
                                   Point2 c) noexcept {
  return orientation(a, b, value) >= -geometry_epsilon &&
         orientation(b, c, value) >= -geometry_epsilon &&
         orientation(c, a, value) >= -geometry_epsilon;
}

[[nodiscard]] std::optional<std::vector<std::uint32_t>> bridge_holes(
    std::vector<std::uint32_t> outer,
    std::vector<std::vector<std::uint32_t>> holes,
    const std::vector<Point2>& points) {
  std::vector<std::array<std::uint32_t, 2>> boundaries;
  const auto append_edges = [&](const std::vector<std::uint32_t>& ring) {
    for (std::size_t index = 0; index < ring.size(); ++index) {
      boundaries.push_back({ring[index], ring[(index + 1U) % ring.size()]});
    }
  };
  append_edges(outer);
  for (const auto& hole : holes) append_edges(hole);
  std::vector<std::array<std::uint32_t, 2>> bridges;
  std::sort(holes.begin(), holes.end(), [&](const auto& lhs, const auto& rhs) {
    const auto rightmost = [&](const auto& ring) {
      return std::max_element(ring.begin(), ring.end(), [&](auto a, auto b) {
        return points[a][0] < points[b][0];
      });
    };
    return points[*rightmost(lhs)][0] > points[*rightmost(rhs)][0];
  });
  for (const auto& hole : holes) {
    const auto hole_at = static_cast<std::size_t>(std::distance(
        hole.begin(), std::max_element(hole.begin(), hole.end(), [&](auto a, auto b) {
          if (points[a][0] != points[b][0]) return points[a][0] < points[b][0];
          return points[a][1] > points[b][1];
        })));
    const auto hole_vertex = hole[hole_at];
    const auto hole_point = points[hole_vertex];
    std::optional<std::pair<double, std::size_t>> nearest;
    for (std::size_t position = 0; position < outer.size(); ++position) {
      const auto outer_vertex = outer[position];
      const auto outer_point = points[outer_vertex];
      if (same(hole_point, outer_point)) continue;
      const Point2 midpoint{(hole_point[0] + outer_point[0]) / 2.0,
                            (hole_point[1] + outer_point[1]) / 2.0};
      if (!inside_polygon(midpoint, outer, points)) continue;
      bool in_hole = false;
      for (const auto& candidate : holes) {
        if (inside_polygon(midpoint, candidate, points)) {
          in_hole = true;
          break;
        }
      }
      if (in_hole) continue;
      bool crossing = false;
      for (const auto edge : boundaries) {
        if (edge[0] == hole_vertex || edge[1] == hole_vertex ||
            edge[0] == outer_vertex || edge[1] == outer_vertex) continue;
        if (intersects(hole_point, outer_point, points[edge[0]], points[edge[1]])) {
          crossing = true;
          break;
        }
      }
      if (crossing) continue;
      for (const auto bridge : bridges) {
        if (intersects(hole_point, outer_point, points[bridge[0]], points[bridge[1]])) {
          crossing = true;
          break;
        }
      }
      if (crossing) continue;
      const double dx = hole_point[0] - outer_point[0];
      const double dy = hole_point[1] - outer_point[1];
      const double distance = dx * dx + dy * dy;
      if (!nearest || distance < nearest->first) nearest = {distance, position};
    }
    if (!nearest) return std::nullopt;
    const auto outer_at = nearest->second;
    const auto outer_vertex = outer[outer_at];
    std::vector<std::uint32_t> combined;
    combined.reserve(outer.size() + hole.size() + 2U);
    combined.insert(combined.end(), outer.begin(), outer.begin() +
                                                    static_cast<std::ptrdiff_t>(outer_at + 1U));
    for (std::size_t index = 0; index < hole.size(); ++index) {
      combined.push_back(hole[(hole_at + index) % hole.size()]);
    }
    combined.push_back(hole_vertex);
    combined.push_back(outer_vertex);
    combined.insert(combined.end(), outer.begin() +
                                      static_cast<std::ptrdiff_t>(outer_at + 1U), outer.end());
    outer = std::move(combined);
    bridges.push_back({hole_vertex, outer_vertex});
  }
  return outer;
}

[[nodiscard]] std::optional<std::vector<std::uint32_t>> ear_clip(
    std::vector<std::uint32_t> polygon, const std::vector<Point2>& points) {
  std::vector<std::uint32_t> triangles;
  triangles.reserve(polygon.size() * 3U);
  while (polygon.size() > 3U) {
    bool clipped = false;
    for (std::size_t cursor = 0; cursor < polygon.size(); ++cursor) {
      const auto previous = polygon[(cursor + polygon.size() - 1U) % polygon.size()];
      const auto current = polygon[cursor];
      const auto following = polygon[(cursor + 1U) % polygon.size()];
      const auto a = points[previous];
      const auto b = points[current];
      const auto c = points[following];
      if (orientation(a, b, c) <= geometry_epsilon) continue;
      bool diagonal_crosses = false;
      for (std::size_t edge = 0; edge < polygon.size(); ++edge) {
        const auto first = polygon[edge];
        const auto second = polygon[(edge + 1U) % polygon.size()];
        if (first == previous || first == following || second == previous ||
            second == following) continue;
        if (intersects(a, c, points[first], points[second])) {
          diagonal_crosses = true;
          break;
        }
      }
      if (diagonal_crosses) continue;
      bool contains = false;
      for (const auto candidate : polygon) {
        const auto value = points[candidate];
        if (same(value, a) || same(value, b) || same(value, c)) continue;
        if (inside_triangle(value, a, b, c)) {
          contains = true;
          break;
        }
      }
      if (contains) continue;
      triangles.insert(triangles.end(), {previous, current, following});
      polygon.erase(polygon.begin() + static_cast<std::ptrdiff_t>(cursor));
      clipped = true;
      break;
    }
    if (clipped) continue;
    std::optional<std::size_t> collinear;
    for (std::size_t cursor = 0; cursor < polygon.size(); ++cursor) {
      const auto previous = polygon[(cursor + polygon.size() - 1U) % polygon.size()];
      const auto current = polygon[cursor];
      const auto following = polygon[(cursor + 1U) % polygon.size()];
      if (std::abs(orientation(points[previous], points[current], points[following])) <=
          geometry_epsilon) {
        collinear = cursor;
        break;
      }
    }
    if (!collinear) return std::nullopt;
    polygon.erase(polygon.begin() + static_cast<std::ptrdiff_t>(*collinear));
  }
  if (polygon.size() == 3U &&
      std::abs(orientation(points[polygon[0]], points[polygon[1]], points[polygon[2]])) >
          geometry_epsilon) {
    triangles.insert(triangles.end(), polygon.begin(), polygon.end());
  }
  return triangles;
}

[[nodiscard]] std::optional<std::vector<std::uint32_t>> triangulate_face(
    const std::vector<Vector3d>& vertices, std::vector<std::uint32_t> outer,
    std::vector<std::vector<std::uint32_t>> holes) {
  if (outer.size() < 3U) return std::nullopt;
  Vector3d normal{};
  for (std::size_t index = 0; index < outer.size(); ++index) {
    const auto current = vertices[outer[index]];
    const auto next = vertices[outer[(index + 1U) % outer.size()]];
    normal.x += (current.y - next.y) * (current.z + next.z);
    normal.y += (current.z - next.z) * (current.x + next.x);
    normal.z += (current.x - next.x) * (current.y + next.y);
  }
  const std::array<double, 3> magnitude{std::abs(normal.x), std::abs(normal.y),
                                        std::abs(normal.z)};
  const auto drop = static_cast<std::size_t>(std::distance(
      magnitude.begin(), std::max_element(magnitude.begin(), magnitude.end())));
  if (magnitude[drop] <= geometry_epsilon) return std::nullopt;
  std::vector<Point2> projected;
  projected.reserve(vertices.size());
  for (const auto vertex : vertices) {
    if (drop == 0U) projected.push_back({vertex.y, vertex.z});
    if (drop == 1U) projected.push_back({vertex.x, vertex.z});
    if (drop == 2U) projected.push_back({vertex.x, vertex.y});
  }
  const bool reversed = signed_area(outer, projected) < 0.0;
  if (reversed) std::reverse(outer.begin(), outer.end());
  for (auto& hole : holes) {
    if (hole.size() < 3U) return std::nullopt;
    if (signed_area(hole, projected) > 0.0) std::reverse(hole.begin(), hole.end());
  }
  auto polygon = bridge_holes(std::move(outer), std::move(holes), projected);
  if (!polygon) return std::nullopt;
  auto triangles = ear_clip(std::move(*polygon), projected);
  if (!triangles) return std::nullopt;
  if (reversed) {
    for (std::size_t index = 0; index < triangles->size(); index += 3U) {
      std::swap((*triangles)[index + 1U], (*triangles)[index + 2U]);
    }
  }
  return triangles;
}

[[nodiscard]] Result<ShapeMesh> decode_shape(std::span<const std::byte> source) {
  std::vector<std::byte> inflated;
  if (source.size() >= 2U && source[0] == std::byte{0x1f} &&
      source[1] == std::byte{0x8b}) {
    auto decoded = inflate_shape(source);
    if (!decoded) return Result<ShapeMesh>::failure(decoded.error());
    inflated = std::move(decoded.value());
    source = inflated;
  }
  std::string_view xml(reinterpret_cast<const char*>(source.data()), source.size());
  const auto points_xml = element(xml, "Points");
  const auto faces_xml = element(xml, "Faces");
  if (!points_xml || !faces_xml) {
    return Result<ShapeMesh>::failure(
        {ErrorCode::invalid_geometry, "An imported Polymesh has no Points or Faces."});
  }
  ShapeMesh result;
  for (const auto point_xml : elements(*points_xml, "Point")) {
    const auto x_xml = element(point_xml, "X");
    const auto y_xml = element(point_xml, "Y");
    const auto z_xml = element(point_xml, "Z");
    const auto x = x_xml ? floating(*x_xml) : std::nullopt;
    const auto y = y_xml ? floating(*y_xml) : std::nullopt;
    const auto z = z_xml ? floating(*z_xml) : std::nullopt;
    if (!x || !y || !z) {
      return Result<ShapeMesh>::failure(
          {ErrorCode::invalid_geometry, "An imported Polymesh point is invalid."});
    }
    result.vertices.push_back({*x, *y, *z});
  }
  if (result.vertices.empty()) {
    return Result<ShapeMesh>::failure(
        {ErrorCode::invalid_geometry, "An imported Polymesh has no vertices."});
  }
  for (const auto face_xml : elements(*faces_xml, "Face")) {
    const auto outer_xml = element(face_xml, "OuterLoop");
    if (!outer_xml) {
      return Result<ShapeMesh>::failure(
          {ErrorCode::invalid_geometry, "An imported Polymesh face has no outer loop."});
    }
    const auto read_ring = [&](std::string_view ring_xml)
        -> std::optional<std::vector<std::uint32_t>> {
      std::vector<std::uint32_t> ring;
      for (const auto value : elements(ring_xml, "Index")) {
        const auto parsed = index_value(value);
        if (!parsed || *parsed >= result.vertices.size()) return std::nullopt;
        ring.push_back(*parsed);
      }
      return ring.size() >= 3U ? std::optional{std::move(ring)} : std::nullopt;
    };
    auto outer = read_ring(*outer_xml);
    if (!outer) {
      return Result<ShapeMesh>::failure(
          {ErrorCode::invalid_geometry, "An imported Polymesh face index is invalid."});
    }
    std::vector<std::vector<std::uint32_t>> holes;
    if (const auto inner_xml = element(face_xml, "InnerLoops")) {
      for (const auto loop_xml : elements(*inner_xml, "Loop")) {
        auto ring = read_ring(loop_xml);
        if (!ring) {
          return Result<ShapeMesh>::failure(
              {ErrorCode::invalid_geometry, "An imported Polymesh hole index is invalid."});
        }
        holes.push_back(std::move(*ring));
      }
    }
    auto triangles = triangulate_face(result.vertices, std::move(*outer), std::move(holes));
    if (!triangles) {
      return Result<ShapeMesh>::failure(
          {ErrorCode::invalid_geometry, "An imported Polymesh face cannot be triangulated."});
    }
    result.indices.insert(result.indices.end(), triangles->begin(), triangles->end());
  }
  if (result.indices.empty()) {
    return Result<ShapeMesh>::failure(
        {ErrorCode::invalid_geometry, "An imported Polymesh has no triangles."});
  }
  return Result<ShapeMesh>::success(std::move(result));
}

}  // namespace

ShapeCatalog::ShapeCatalog(const ModelPackage& package) {
  std::unordered_map<std::string, std::shared_ptr<const ByteSource>> geometry;
  for (const auto& asset : package.assets()) {
    if (asset.source() == nullptr) continue;
    const auto path = normalized_path(asset.logical_name());
    if (path.find("shapegeometries/") != std::string::npos) {
      geometry.insert_or_assign(stem(path), asset.source());
    }
  }
  for (const auto& asset : package.assets()) {
    if (asset.source() == nullptr) continue;
    const auto path = normalized_path(asset.logical_name());
    if (path.find("shapes/") == std::string::npos ||
        path.find("shapegeometries/") != std::string::npos ||
        !path.ends_with(".xml")) continue;
    const auto bytes = asset.source()->bytes();
    const std::string_view xml(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    const auto guid_xml = element(xml, "Guid");
    const auto storage_xml = element(xml, "BrepStorageId");
    if (!guid_xml || !storage_xml) continue;
    const auto guid = normalized_id(*guid_xml);
    const auto storage_id = normalized_id(*storage_xml);
    const auto source = geometry.find(storage_id);
    definitions_.insert_or_assign(
        guid, Definition{storage_id, source == geometry.end() ? nullptr : source->second});
  }
}

const ShapeMesh* ShapeCatalog::resolve(std::string_view raw_guid, Error& error) {
  const auto guid = normalized_id(raw_guid);
  if (const auto found = meshes_.find(guid); found != meshes_.end()) return &found->second;
  if (const auto found = errors_.find(guid); found != errors_.end()) {
    error = found->second;
    return nullptr;
  }
  const auto definition = definitions_.find(guid);
  if (definition == definitions_.end()) {
    error = {ErrorCode::decoder_unavailable,
             "The imported-shape profile has no model-local metadata."};
    errors_.insert_or_assign(guid, error);
    return nullptr;
  }
  if (definition->second.geometry == nullptr) {
    error = {ErrorCode::decoder_unavailable,
             "The imported-shape metadata has no model-local Polymesh asset."};
    errors_.insert_or_assign(guid, error);
    return nullptr;
  }
  auto decoded = decode_shape(definition->second.geometry->bytes());
  if (!decoded) {
    error = decoded.error();
    errors_.insert_or_assign(guid, error);
    return nullptr;
  }
  auto [inserted, unused] = meshes_.insert_or_assign(guid, std::move(decoded.value()));
  static_cast<void>(unused);
  return &inserted->second;
}

}  // namespace tekla::db1::detail

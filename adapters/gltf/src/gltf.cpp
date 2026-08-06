#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <iomanip>
#include <limits>
#include <numbers>
#include <optional>
#include <ostream>
#include <span>
#include <sstream>
#include <string>
#include <tekla/db1/adapters/gltf.hpp>
#include <utility>
#include <vector>

namespace tekla::db1::gltf {
namespace {

constexpr std::uint32_t kGlbMagic = 0x46546C67U;
constexpr std::uint32_t kGlbVersion = 2U;
constexpr std::uint32_t kJsonChunk = 0x4E4F534AU;
constexpr std::uint32_t kBinaryChunk = 0x004E4942U;

struct MeshData {
  std::uint64_t object_id = 0;
  std::vector<float> positions;
  std::vector<std::uint32_t> indices;
  std::array<float, 3> minimum{};
  std::array<float, 3> maximum{};
  std::uint32_t mode = 4U;
};

struct PrimitiveData {
  std::uint64_t object_id = 0;
  std::array<float, 3> minimum{};
  std::array<float, 3> maximum{};
  std::uint32_t position_offset = 0;
  std::uint32_t index_offset = 0;
  std::uint32_t position_byte_length = 0;
  std::uint32_t index_byte_length = 0;
  std::uint32_t position_count = 0;
  std::uint32_t index_count = 0;
  std::uint32_t mode = 4U;
};

void append_u32(std::vector<std::byte>& destination, std::uint32_t value) {
  for (unsigned shift = 0; shift < 32; shift += 8) {
    destination.push_back(static_cast<std::byte>((value >> shift) & 0xFFU));
  }
}

void append_f32(std::vector<std::byte>& destination, float value) {
  append_u32(destination, std::bit_cast<std::uint32_t>(value));
}

void append_floats(std::vector<std::byte>& destination, std::span<const float> values) {
  if (std::endian::native == std::endian::little) {
    const auto offset = destination.size();
    destination.resize(offset + values.size_bytes());
    std::memcpy(destination.data() + offset, values.data(), values.size_bytes());
  } else {
    for (const auto value : values) append_f32(destination, value);
  }
}

void append_indices(std::vector<std::byte>& destination, std::span<const std::uint32_t> values) {
  if (std::endian::native == std::endian::little) {
    const auto offset = destination.size();
    destination.resize(offset + values.size_bytes());
    std::memcpy(destination.data() + offset, values.data(), values.size_bytes());
  } else {
    for (const auto value : values) append_u32(destination, value);
  }
}

void align_four(std::vector<std::byte>& bytes) {
  while (bytes.size() % 4U != 0U) {
    bytes.push_back(std::byte{0});
  }
}

void write_u32(std::ostream& output, std::uint32_t value) {
  std::array<char, 4> bytes{};
  for (unsigned shift = 0; shift < 32; shift += 8) {
    bytes[shift / 8U] = static_cast<char>((value >> shift) & 0xFFU);
  }
  output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

Result<MeshData> copy_mesh(const MeshView& view) {
  if (view.positions.empty() || view.positions.size() % 3U != 0U || view.indices.empty() ||
      view.indices.size() % 3U != 0U) {
    return Result<MeshData>::failure(
        {ErrorCode::invalid_geometry, "A mesh must contain XYZ positions and triangle indices."});
  }

  const std::size_t vertex_count = view.positions.size() / 3U;
  if (vertex_count > std::numeric_limits<std::uint32_t>::max() ||
      view.indices.size() > std::numeric_limits<std::uint32_t>::max()) {
    return Result<MeshData>::failure(
        {ErrorCode::invalid_geometry, "A mesh exceeds GLB's 32-bit element count limit."});
  }
  for (const float coordinate : view.positions) {
    if (!std::isfinite(coordinate)) {
      return Result<MeshData>::failure(
          {ErrorCode::invalid_geometry, "A mesh position is not finite."});
    }
  }
  for (std::uint32_t index : view.indices) {
    if (index >= vertex_count) {
      return Result<MeshData>::failure(
          {ErrorCode::invalid_geometry, "A mesh index exceeds its vertex count."});
    }
  }

  MeshData mesh;
  mesh.object_id = view.object_id;
  mesh.positions.assign(view.positions.begin(), view.positions.end());
  mesh.indices.assign(view.indices.begin(), view.indices.end());
  mesh.minimum = {mesh.positions[0], mesh.positions[1], mesh.positions[2]};
  mesh.maximum = mesh.minimum;
  for (std::size_t i = 3; i < mesh.positions.size(); i += 3) {
    for (std::size_t axis = 0; axis < 3; ++axis) {
      mesh.minimum[axis] = std::min(mesh.minimum[axis], mesh.positions[i + axis]);
      mesh.maximum[axis] = std::max(mesh.maximum[axis], mesh.positions[i + axis]);
    }
  }
  return Result<MeshData>::success(std::move(mesh));
}

std::array<double, 3> cross(std::array<double, 3> lhs, std::array<double, 3> rhs) {
  return {lhs[1] * rhs[2] - lhs[2] * rhs[1], lhs[2] * rhs[0] - lhs[0] * rhs[2],
          lhs[0] * rhs[1] - lhs[1] * rhs[0]};
}

std::optional<std::array<double, 3>> normalized(std::array<double, 3> value) {
  const double length = std::sqrt(value[0] * value[0] + value[1] * value[1] + value[2] * value[2]);
  if (!std::isfinite(length) || length <= 1.0e-12) return std::nullopt;
  return std::array<double, 3>{value[0] / length, value[1] / length, value[2] / length};
}

std::array<double, 3> subtract(Vector3d lhs, Vector3d rhs) {
  return {lhs.x - rhs.x, lhs.y - rhs.y, lhs.z - rhs.z};
}

double dot(std::array<double, 3> lhs, std::array<double, 3> rhs) {
  return lhs[0] * rhs[0] + lhs[1] * rhs[1] + lhs[2] * rhs[2];
}

std::array<double, 3> add(std::array<double, 3> lhs, std::array<double, 3> rhs) {
  return {lhs[0] + rhs[0], lhs[1] + rhs[1], lhs[2] + rhs[2]};
}

std::array<double, 3> scale(std::array<double, 3> value, double factor) {
  return {value[0] * factor, value[1] * factor, value[2] * factor};
}

std::optional<std::array<double, 3>> perpendicular(std::array<double, 3> tangent,
                                                   std::array<double, 3> previous = {}) {
  if (const auto transported = normalized(add(previous, scale(tangent, -dot(previous, tangent)))))
    return transported;
  std::array<double, 3> trial{0.0, 0.0, 1.0};
  if (std::abs(tangent[2]) > 0.9) trial = {0.0, 1.0, 0.0};
  return normalized(cross(tangent, trial));
}

bool append_curve_sweep(MeshData& mesh, std::span<const Vector3d> input, double radius) {
  constexpr std::uint32_t facets = 8U;
  std::vector<Vector3d> points;
  points.reserve(input.size());
  for (const auto point : input) {
    if (points.empty() || normalized(subtract(point, points.back()))) points.push_back(point);
  }
  if (points.size() < 2U) return false;

  mesh.positions.reserve(points.size() * facets * 3U + 6U);
  mesh.indices.reserve(points.size() * facets * 6U);

  std::vector<std::array<double, 3>> segment_tangents;
  segment_tangents.reserve(points.size() - 1U);
  for (std::size_t index = 0; index + 1U < points.size(); ++index)
    segment_tangents.push_back(*normalized(subtract(points[index + 1U], points[index])));

  std::array<double, 3> previous_normal{};
  for (std::size_t point_index = 0; point_index < points.size(); ++point_index) {
    auto tangent = point_index == 0U ? segment_tangents.front()
                   : point_index + 1U == points.size()
                       ? segment_tangents.back()
                       : add(segment_tangents[point_index - 1U], segment_tangents[point_index]);
    const auto normalized_tangent = normalized(tangent);
    if (!normalized_tangent)
      tangent = segment_tangents[point_index];
    else
      tangent = *normalized_tangent;
    const auto first = perpendicular(tangent, previous_normal);
    if (!first) return false;
    previous_normal = *first;
    const auto second = cross(tangent, *first);
    const auto center = points[point_index];
    for (std::uint32_t index = 0; index < facets; ++index) {
      const double angle =
          static_cast<double>(index) * 2.0 * std::numbers::pi / static_cast<double>(facets);
      const double first_scale = radius * std::cos(angle);
      const double second_scale = radius * std::sin(angle);
      mesh.positions.insert(
          mesh.positions.end(),
          {static_cast<float>(center.x + (*first)[0] * first_scale + second[0] * second_scale),
           static_cast<float>(center.y + (*first)[1] * first_scale + second[1] * second_scale),
           static_cast<float>(center.z + (*first)[2] * first_scale + second[2] * second_scale)});
    }
  }
  const auto start_center = static_cast<std::uint32_t>(mesh.positions.size() / 3U);
  mesh.positions.insert(mesh.positions.end(),
                        {static_cast<float>(points.front().x), static_cast<float>(points.front().y),
                         static_cast<float>(points.front().z)});
  const auto end_center = static_cast<std::uint32_t>(mesh.positions.size() / 3U);
  mesh.positions.insert(mesh.positions.end(),
                        {static_cast<float>(points.back().x), static_cast<float>(points.back().y),
                         static_cast<float>(points.back().z)});
  for (std::uint32_t segment = 0; segment + 1U < points.size(); ++segment) {
    const auto first_ring = segment * facets;
    const auto second_ring = first_ring + facets;
    for (std::uint32_t index = 0; index < facets; ++index) {
      const auto next = (index + 1U) % facets;
      mesh.indices.insert(mesh.indices.end(),
                          {first_ring + index, first_ring + next, second_ring + next,
                           first_ring + index, second_ring + next, second_ring + index});
    }
  }
  for (std::uint32_t index = 0; index < facets; ++index) {
    const auto next = (index + 1U) % facets;
    const auto last_ring = static_cast<std::uint32_t>(points.size() - 1U) * facets;
    mesh.indices.insert(mesh.indices.end(), {start_center, next, index, end_center,
                                             last_ring + index, last_ring + next});
  }
  return true;
}

Result<MeshData> copy_curve(const CurveView& view) {
  if (view.points.size() < 2U || !std::isfinite(view.radius) || view.radius < 0.0) {
    return Result<MeshData>::failure(
        {ErrorCode::invalid_geometry, "A curve needs at least two points and a valid radius."});
  }
  for (const auto point : view.points) {
    if (!std::isfinite(point.x) || !std::isfinite(point.y) || !std::isfinite(point.z) ||
        !std::isfinite(static_cast<float>(point.x)) ||
        !std::isfinite(static_cast<float>(point.y)) ||
        !std::isfinite(static_cast<float>(point.z))) {
      return Result<MeshData>::failure(
          {ErrorCode::invalid_geometry, "A curve point is not a finite GLB coordinate."});
    }
  }
  const auto limit = static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max());
  if ((view.radius == 0.0 &&
       (view.points.size() > limit || view.points.size() - 1U > limit / 2U)) ||
      (view.radius > 0.0 &&
       (view.points.size() > (limit - 2U) / 8U || view.points.size() > limit / 48U))) {
    return Result<MeshData>::failure(
        {ErrorCode::invalid_geometry, "A curve exceeds GLB's 32-bit element count limit."});
  }
  MeshData result;
  result.object_id = view.object_id;
  if (view.radius == 0.0) {
    result.mode = 1U;
    result.positions.reserve(view.points.size() * 3U);
    for (const auto point : view.points) {
      result.positions.insert(
          result.positions.end(),
          {static_cast<float>(point.x), static_cast<float>(point.y), static_cast<float>(point.z)});
    }
    for (std::uint32_t index = 0; index + 1U < view.points.size(); ++index)
      result.indices.insert(result.indices.end(), {index, index + 1U});
  } else {
    if (!append_curve_sweep(result, view.points, view.radius)) {
      return Result<MeshData>::failure(
          {ErrorCode::invalid_geometry, "A physical curve has no non-degenerate segment."});
    }
  }
  if (std::any_of(result.positions.begin(), result.positions.end(),
                  [](float coordinate) { return !std::isfinite(coordinate); })) {
    return Result<MeshData>::failure(
        {ErrorCode::invalid_geometry, "Curve tessellation produced a non-finite GLB coordinate."});
  }
  result.minimum = {result.positions[0], result.positions[1], result.positions[2]};
  result.maximum = result.minimum;
  for (std::size_t index = 3U; index < result.positions.size(); index += 3U) {
    for (std::size_t axis = 0; axis < 3U; ++axis) {
      result.minimum[axis] = std::min(result.minimum[axis], result.positions[index + axis]);
      result.maximum[axis] = std::max(result.maximum[axis], result.positions[index + axis]);
    }
  }
  return Result<MeshData>::success(std::move(result));
}

std::string build_json(const std::vector<PrimitiveData>& meshes, std::size_t binary_size) {
  std::ostringstream json;
  json << std::setprecision(std::numeric_limits<float>::max_digits10);
  json << R"({"asset":{"version":"2.0","generator":"tekla-db1-sdk"},)";
  json << R"("scene":0,"scenes":[{"nodes":[)";
  for (std::size_t i = 0; i < meshes.size(); ++i) {
    if (i != 0) json << ',';
    json << i;
  }
  json << R"(]}],"nodes":[)";
  for (std::size_t i = 0; i < meshes.size(); ++i) {
    if (i != 0) json << ',';
    json << R"({"mesh":)" << i << R"(,"name":"object-)" << meshes[i].object_id << R"("})";
  }
  json << R"(],"meshes":[)";
  for (std::size_t i = 0; i < meshes.size(); ++i) {
    if (i != 0) json << ',';
    json << R"({"primitives":[{"attributes":{"POSITION":)" << i * 2U << R"(},"indices":)"
         << i * 2U + 1U << R"(,"mode":)" << meshes[i].mode << "}]})";
  }
  json << R"(],"buffers":[{"byteLength":)" << binary_size << R"(}],"bufferViews":[)";
  for (std::size_t i = 0; i < meshes.size(); ++i) {
    if (i != 0) json << ',';
    const PrimitiveData& mesh = meshes[i];
    json << R"({"buffer":0,"byteOffset":)" << mesh.position_offset << R"(,"byteLength":)"
         << mesh.position_byte_length << R"(,"target":34962},)";
    json << R"({"buffer":0,"byteOffset":)" << mesh.index_offset << R"(,"byteLength":)"
         << mesh.index_byte_length << R"(,"target":34963})";
  }
  json << R"(],"accessors":[)";
  for (std::size_t i = 0; i < meshes.size(); ++i) {
    if (i != 0) json << ',';
    const PrimitiveData& mesh = meshes[i];
    json << R"({"bufferView":)" << i * 2U << R"(,"componentType":5126,"count":)"
         << mesh.position_count << R"(,"type":"VEC3","min":[)" << mesh.minimum[0] << ','
         << mesh.minimum[1] << ',' << mesh.minimum[2] << R"(],"max":[)" << mesh.maximum[0] << ','
         << mesh.maximum[1] << ',' << mesh.maximum[2] << R"(]},)";
    json << R"({"bufferView":)" << i * 2U + 1U << R"(,"componentType":5125,"count":)"
         << mesh.index_count << R"(,"type":"SCALAR"})";
  }
  json << "]}";
  return json.str();
}

Result<PrimitiveData> pack_mesh(MeshData mesh, std::vector<std::byte>& binary) {
  constexpr auto limit = static_cast<std::uint64_t>(std::numeric_limits<std::uint32_t>::max());
  const auto position_bytes = static_cast<std::uint64_t>(mesh.positions.size()) * sizeof(float);
  const auto index_bytes = static_cast<std::uint64_t>(mesh.indices.size()) * sizeof(std::uint32_t);
  const auto binary_bytes = static_cast<std::uint64_t>(binary.size());
  if (position_bytes > limit || index_bytes > limit || binary_bytes > limit ||
      position_bytes > limit - binary_bytes ||
      index_bytes + 3U > limit - binary_bytes - position_bytes) {
    return Result<PrimitiveData>::failure(
        {ErrorCode::invalid_geometry, "The GLB exceeds the format's 32-bit size limit."});
  }
  align_four(binary);
  PrimitiveData primitive{.object_id = mesh.object_id,
                          .minimum = mesh.minimum,
                          .maximum = mesh.maximum,
                          .position_offset = static_cast<std::uint32_t>(binary.size()),
                          .position_byte_length = static_cast<std::uint32_t>(position_bytes),
                          .index_byte_length = static_cast<std::uint32_t>(index_bytes),
                          .position_count = static_cast<std::uint32_t>(mesh.positions.size() / 3U),
                          .index_count = static_cast<std::uint32_t>(mesh.indices.size()),
                          .mode = mesh.mode};
  append_floats(binary, mesh.positions);
  align_four(binary);
  primitive.index_offset = static_cast<std::uint32_t>(binary.size());
  append_indices(binary, mesh.indices);
  return Result<PrimitiveData>::success(primitive);
}

}  // namespace

Result<WriteReport> write_glb(BatchReader& reader, std::ostream& output) {
  std::vector<PrimitiveData> meshes;
  std::vector<std::byte> binary;
  std::uint64_t mesh_count = 0U;
  std::uint64_t curve_count = 0U;
  for (;;) {
    auto next = reader.next();
    if (!next) return Result<WriteReport>::failure(next.error());

    const BatchView& batch = next.value();
    if (batch.kind == BatchKind::end) break;
    if (batch.kind == BatchKind::meshes) {
      for (const MeshView& mesh_view : batch.meshes) {
        auto copied = copy_mesh(mesh_view);
        if (!copied) return Result<WriteReport>::failure(copied.error());
        auto packed = pack_mesh(std::move(copied).value(), binary);
        if (!packed) return Result<WriteReport>::failure(packed.error());
        meshes.push_back(std::move(packed).value());
        ++mesh_count;
      }
    } else if (batch.kind == BatchKind::curves) {
      for (const CurveView& curve_view : batch.curves) {
        auto copied = copy_curve(curve_view);
        if (!copied) return Result<WriteReport>::failure(copied.error());
        auto packed = pack_mesh(std::move(copied).value(), binary);
        if (!packed) return Result<WriteReport>::failure(packed.error());
        meshes.push_back(std::move(packed).value());
        ++curve_count;
      }
    }
  }

  align_four(binary);

  std::string json = build_json(meshes, binary.size());
  while (json.size() % 4U != 0U) json.push_back(' ');

  const std::uint64_t total_size = 12U + 8U + json.size() + 8U + binary.size();
  if (total_size > std::numeric_limits<std::uint32_t>::max()) {
    return Result<WriteReport>::failure(
        {ErrorCode::invalid_geometry, "The GLB exceeds the format's 32-bit size limit."});
  }

  try {
    write_u32(output, kGlbMagic);
    write_u32(output, kGlbVersion);
    write_u32(output, static_cast<std::uint32_t>(total_size));
    write_u32(output, static_cast<std::uint32_t>(json.size()));
    write_u32(output, kJsonChunk);
    output.write(json.data(), static_cast<std::streamsize>(json.size()));
    write_u32(output, static_cast<std::uint32_t>(binary.size()));
    write_u32(output, kBinaryChunk);
    output.write(reinterpret_cast<const char*>(binary.data()),
                 static_cast<std::streamsize>(binary.size()));
  } catch (const std::ios_base::failure&) {
    return Result<WriteReport>::failure({ErrorCode::io_error, "Writing the GLB output failed."});
  } catch (const std::exception&) {
    return Result<WriteReport>::failure({ErrorCode::io_error, "Writing the GLB output failed."});
  } catch (...) {
    return Result<WriteReport>::failure({ErrorCode::io_error, "Writing the GLB output failed."});
  }

  if (!output) {
    return Result<WriteReport>::failure({ErrorCode::io_error, "Writing the GLB output failed."});
  }

  return Result<WriteReport>::success(
      {.mesh_count = mesh_count, .curve_count = curve_count, .bytes_written = total_size});
}

}  // namespace tekla::db1::gltf

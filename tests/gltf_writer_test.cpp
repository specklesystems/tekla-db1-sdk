#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <span>
#include <sstream>
#include <stdexcept>
#include <streambuf>
#include <string>
#include <tekla/db1/adapters/gltf.hpp>
#include <vector>

namespace {

int failures = 0;

#define CHECK(condition, message)         \
  do {                                    \
    if (!(condition)) {                   \
      std::printf("FAIL: %s\n", message); \
      ++failures;                         \
    }                                     \
  } while (false)

class TriangleReader final : public tekla::db1::BatchReader {
 public:
  tekla::db1::Result<tekla::db1::BatchView> next() override {
    if (emitted_) {
      return tekla::db1::Result<tekla::db1::BatchView>::success(
          {.kind = tekla::db1::BatchKind::end});
    }
    emitted_ = true;
    return tekla::db1::Result<tekla::db1::BatchView>::success(
        {.kind = tekla::db1::BatchKind::meshes, .meshes = meshes_});
  }

 private:
  bool emitted_ = false;
  const std::array<float, 9> positions_ = {0.0F, 0.0F, 0.0F, 1.0F, 0.0F, 0.0F, 0.0F, 1.0F, 0.0F};
  const std::array<std::uint32_t, 3> indices_ = {0, 1, 2};
  const std::array<tekla::db1::MeshView, 1> meshes_ = {
      tekla::db1::MeshView{42, positions_, indices_}};
};

class InvalidMeshReader final : public tekla::db1::BatchReader {
 public:
  tekla::db1::Result<tekla::db1::BatchView> next() override {
    if (emitted_) {
      return tekla::db1::Result<tekla::db1::BatchView>::success(
          {.kind = tekla::db1::BatchKind::end});
    }
    emitted_ = true;
    return tekla::db1::Result<tekla::db1::BatchView>::success(
        {.kind = tekla::db1::BatchKind::meshes, .meshes = meshes_});
  }

 private:
  bool emitted_ = false;
  const std::array<float, 2> positions_ = {0.0F, 0.0F};
  const std::array<std::uint32_t, 3> indices_ = {0, 1, 2};
  const std::array<tekla::db1::MeshView, 1> meshes_ = {
      tekla::db1::MeshView{7, positions_, indices_}};
};

class CurveReader final : public tekla::db1::BatchReader {
 public:
  tekla::db1::Result<tekla::db1::BatchView> next() override {
    if (emitted_) {
      return tekla::db1::Result<tekla::db1::BatchView>::success(
          {.kind = tekla::db1::BatchKind::end});
    }
    emitted_ = true;
    return tekla::db1::Result<tekla::db1::BatchView>::success(
        {.kind = tekla::db1::BatchKind::curves, .curves = curves_});
  }

 private:
  bool emitted_ = false;
  const std::array<tekla::db1::Vector3d, 2> grid_points_ = {tekla::db1::Vector3d{0.0, 0.0, 0.0},
                                                            tekla::db1::Vector3d{100.0, 0.0, 0.0}};
  const std::array<tekla::db1::Vector3d, 3> rebar_points_ = {
      tekla::db1::Vector3d{0.0, 0.0, 0.0}, tekla::db1::Vector3d{0.0, 100.0, 0.0},
      tekla::db1::Vector3d{50.0, 100.0, 0.0}};
  const std::array<tekla::db1::CurveView, 2> curves_ = {
      tekla::db1::CurveView{70, tekla::db1::CurveGeometryKind::line_segment, grid_points_, 0.0},
      tekla::db1::CurveView{71, tekla::db1::CurveGeometryKind::polyline, rebar_points_, 8.0}};
};

class BatchedPhysicalCurveReader final : public tekla::db1::BatchReader {
 public:
  BatchedPhysicalCurveReader(std::size_t curve_count, std::size_t point_count,
                             std::size_t batch_size)
      : batch_size_(batch_size) {
    points_.reserve(point_count);
    for (std::size_t index = 0; index < point_count; ++index) {
      points_.push_back({static_cast<double>(index) * 25.0, index % 2U == 0U ? 0.0 : 10.0,
                         static_cast<double>(index % 3U)});
    }
    curves_.reserve(curve_count);
    for (std::size_t index = 0; index < curve_count; ++index) {
      curves_.push_back({10'000U + index, tekla::db1::CurveGeometryKind::polyline, points_, 6.0});
    }
  }

  tekla::db1::Result<tekla::db1::BatchView> next() override {
    if (next_ == curves_.size()) {
      return tekla::db1::Result<tekla::db1::BatchView>::success(
          {.kind = tekla::db1::BatchKind::end});
    }
    const auto count = std::min(batch_size_, curves_.size() - next_);
    const auto batch = std::span<const tekla::db1::CurveView>(curves_).subspan(next_, count);
    next_ += count;
    return tekla::db1::Result<tekla::db1::BatchView>::success(
        {.kind = tekla::db1::BatchKind::curves, .curves = batch});
  }

 private:
  std::vector<tekla::db1::Vector3d> points_;
  std::vector<tekla::db1::CurveView> curves_;
  std::size_t batch_size_ = 1U;
  std::size_t next_ = 0U;
};

class InvalidPhysicalCurveReader final : public tekla::db1::BatchReader {
 public:
  tekla::db1::Result<tekla::db1::BatchView> next() override {
    if (done_) {
      return tekla::db1::Result<tekla::db1::BatchView>::success(
          {.kind = tekla::db1::BatchKind::end});
    }
    done_ = true;
    return tekla::db1::Result<tekla::db1::BatchView>::success(
        {.kind = tekla::db1::BatchKind::curves, .curves = curves_});
  }

 private:
  bool done_ = false;
  const std::array<tekla::db1::Vector3d, 2> points_ = {
      tekla::db1::Vector3d{0.0, 0.0, 0.0},
      tekla::db1::Vector3d{std::numeric_limits<double>::quiet_NaN(), 1.0, 0.0}};
  const std::array<tekla::db1::CurveView, 1> curves_ = {
      tekla::db1::CurveView{99U, tekla::db1::CurveGeometryKind::polyline, points_, 4.0}};
};

class OverflowingPhysicalCurveReader final : public tekla::db1::BatchReader {
 public:
  tekla::db1::Result<tekla::db1::BatchView> next() override {
    if (done_) {
      return tekla::db1::Result<tekla::db1::BatchView>::success(
          {.kind = tekla::db1::BatchKind::end});
    }
    done_ = true;
    return tekla::db1::Result<tekla::db1::BatchView>::success(
        {.kind = tekla::db1::BatchKind::curves, .curves = curves_});
  }

 private:
  bool done_ = false;
  const std::array<tekla::db1::Vector3d, 2> points_ = {tekla::db1::Vector3d{0.0, 0.0, 0.0},
                                                       tekla::db1::Vector3d{100.0, 0.0, 0.0}};
  const std::array<tekla::db1::CurveView, 1> curves_ = {tekla::db1::CurveView{
      100U, tekla::db1::CurveGeometryKind::polyline, points_, std::numeric_limits<double>::max()}};
};

class RejectingStreamBuffer final : public std::streambuf {
 protected:
  std::streamsize xsputn(const char*, std::streamsize) override { return 0; }

  int_type overflow(int_type) override { return traits_type::eof(); }
};

class ThrowingStreamBuffer final : public std::streambuf {
 protected:
  std::streamsize xsputn(const char*, std::streamsize) override {
    throw std::runtime_error("synthetic stream-buffer failure");
  }

  int_type overflow(int_type) override {
    throw std::runtime_error("synthetic stream-buffer failure");
  }
};

class NonStandardThrowingStreamBuffer final : public std::streambuf {
 protected:
  std::streamsize xsputn(const char*, std::streamsize) override { throw 17; }

  int_type overflow(int_type) override { throw 17; }
};

bool write_returns_io_error_without_throw(tekla::db1::BatchReader& reader, std::ostream& output) {
  try {
    const auto result = tekla::db1::gltf::write_glb(reader, output);
    return !result.has_value() && result.error().code == tekla::db1::ErrorCode::io_error;
  } catch (...) {
    return false;
  }
}

std::uint32_t read_u32(const std::string& bytes, std::size_t offset) {
  std::uint32_t value = 0;
  std::memcpy(&value, bytes.data() + offset, sizeof(value));
  return value;
}

}  // namespace

int main() {
  TriangleReader reader;
  std::ostringstream output(std::ios::binary);

  auto result = tekla::db1::gltf::write_glb(reader, output);
  CHECK(result.has_value(), "a public mesh batch writes as GLB");
  CHECK(result.value().mesh_count == 1, "the adapter reports the consumed mesh count");

  const std::string bytes = output.str();
  CHECK(bytes.size() >= 20, "the GLB has a header and JSON chunk");
  CHECK(read_u32(bytes, 0) == 0x46546C67U, "the GLB magic is correct");
  CHECK(read_u32(bytes, 4) == 2U, "the GLB version is 2");
  CHECK(read_u32(bytes, 8) == bytes.size(), "the declared GLB length is exact");
  CHECK(bytes.find("\"name\":\"object-42\"") != std::string::npos,
        "the model-local object key reaches the output adapter");

  TriangleReader repeated_reader;
  std::ostringstream repeated_output(std::ios::binary);
  auto repeated = tekla::db1::gltf::write_glb(repeated_reader, repeated_output);
  CHECK(repeated.has_value() && repeated_output.str() == bytes,
        "the same public mesh stream produces byte-identical GLB output");

  CurveReader curve_reader;
  std::ostringstream curve_output(std::ios::binary);
  auto curves = tekla::db1::gltf::write_glb(curve_reader, curve_output);
  CHECK(curves.has_value(), "public renderer and physical curves write as GLB");
  CHECK(curves.value().mesh_count == 0 && curves.value().curve_count == 2,
        "the adapter reports curve and mesh inputs separately");
  CHECK(curve_output.str().find("\"mode\":1") != std::string::npos,
        "a zero-radius grid curve remains a glTF line primitive");
  CHECK(curve_output.str().find("\"mode\":4") != std::string::npos,
        "a positive-radius rebar curve becomes bounded triangle geometry");
  CHECK(curve_output.str().find("\"count\":2,\"type\":\"VEC3\"") != std::string::npos,
        "the grid primitive preserves both line endpoints");
  CHECK(curve_output.str().find("\"count\":26,\"type\":\"VEC3\"") != std::string::npos &&
            curve_output.str().find("\"count\":144,\"type\":\"SCALAR\"") != std::string::npos,
        "a multi-leg rebar reuses its joint ring and only caps the sweep ends");

  constexpr std::size_t physical_curve_count = 512U;
  BatchedPhysicalCurveReader many_curves(physical_curve_count, 33U, 37U);
  std::ostringstream many_curve_output(std::ios::binary);
  const auto many_started = std::chrono::steady_clock::now();
  auto many = tekla::db1::gltf::write_glb(many_curves, many_curve_output);
  const auto many_elapsed =
      std::chrono::duration<double>(std::chrono::steady_clock::now() - many_started).count();
  CHECK(many.has_value() && many.value().curve_count == physical_curve_count,
        "many batched physical curves are consumed exactly once");
  CHECK(many.has_value() && many.value().bytes_written < 6U * 1024U * 1024U,
        "shared sweep rings keep a large physical-curve GLB below its regression bound");
  CHECK(many_elapsed < 5.0,
        "large physical-curve conversion stays within the generous CI performance bound");
  if (many) {
    std::printf("physical-curve regression: curves=%llu bytes=%llu seconds=%.3f\n",
                static_cast<unsigned long long>(many.value().curve_count),
                static_cast<unsigned long long>(many.value().bytes_written), many_elapsed);
  }

  InvalidPhysicalCurveReader invalid_curve_reader;
  std::ostringstream invalid_curve_output(std::ios::binary);
  auto invalid_curve = tekla::db1::gltf::write_glb(invalid_curve_reader, invalid_curve_output);
  CHECK(!invalid_curve.has_value() &&
            invalid_curve.error().code == tekla::db1::ErrorCode::invalid_geometry,
        "a non-finite physical-curve point fails before tessellation emits geometry");

  OverflowingPhysicalCurveReader overflowing_curve_reader;
  std::ostringstream overflowing_curve_output(std::ios::binary);
  auto overflowing_curve =
      tekla::db1::gltf::write_glb(overflowing_curve_reader, overflowing_curve_output);
  CHECK(!overflowing_curve.has_value() &&
            overflowing_curve.error().code == tekla::db1::ErrorCode::invalid_geometry,
        "finite curve inputs that overflow GLB coordinates fail before packing");

  InvalidMeshReader invalid_reader;
  std::ostringstream invalid_output(std::ios::binary);
  auto invalid = tekla::db1::gltf::write_glb(invalid_reader, invalid_output);
  CHECK(!invalid.has_value(), "invalid geometry fails rather than producing a corrupt GLB");
  CHECK(invalid.error().code == tekla::db1::ErrorCode::invalid_geometry,
        "invalid geometry has a stable error code");

  RejectingStreamBuffer rejecting_buffer;
  std::ostream rejecting_output(&rejecting_buffer);
  rejecting_output.exceptions(std::ios::badbit | std::ios::failbit);
  TriangleReader rejecting_reader;
  CHECK(write_returns_io_error_without_throw(rejecting_reader, rejecting_output),
        "an exception-enabled short stream write returns io_error without escaping");

  ThrowingStreamBuffer throwing_buffer;
  std::ostream throwing_output(&throwing_buffer);
  throwing_output.exceptions(std::ios::badbit | std::ios::failbit);
  TriangleReader throwing_reader;
  CHECK(write_returns_io_error_without_throw(throwing_reader, throwing_output),
        "an exception thrown by a stream buffer returns io_error without escaping");

  NonStandardThrowingStreamBuffer nonstandard_throwing_buffer;
  std::ostream nonstandard_throwing_output(&nonstandard_throwing_buffer);
  nonstandard_throwing_output.exceptions(std::ios::badbit | std::ios::failbit);
  TriangleReader nonstandard_throwing_reader;
  CHECK(write_returns_io_error_without_throw(nonstandard_throwing_reader,
                                             nonstandard_throwing_output),
        "a non-standard stream-buffer exception returns io_error without escaping");

  if (failures != 0) {
    std::printf("FAILED (%d)\n", failures);
    return 1;
  }
  std::printf("ALL PASS\n");
  return 0;
}

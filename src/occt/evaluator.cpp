#include <BRepAlgoAPI_Common.hxx>
#include <BRepAlgoAPI_Cut.hxx>
#include <BRepAlgoAPI_Fuse.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepBuilderAPI_MakePolygon.hxx>
#include <BRepBuilderAPI_MakeSolid.hxx>
#include <BRepBuilderAPI_Sewing.hxx>
#include <BRepCheck_Analyzer.hxx>
#include <BRepGProp.hxx>
#include <BRepLib.hxx>
#include <BRepMesh_IncrementalMesh.hxx>
#include <BRepOffsetAPI_ThruSections.hxx>
#include <BRepPrimAPI_MakeBox.hxx>
#include <BRepPrimAPI_MakeHalfSpace.hxx>
#include <BRepPrimAPI_MakePrism.hxx>
#include <BRepPrimAPI_MakeRevol.hxx>
#include <BRep_Tool.hxx>
#include <GC_MakeArcOfCircle.hxx>
#include <Geom_Circle.hxx>
#include <Geom_TrimmedCurve.hxx>
#include <GProp_GProps.hxx>
#include <Poly_Triangulation.hxx>
#include <Standard_Failure.hxx>
#include <TopAbs_Orientation.hxx>
#include <TopAbs_ShapeEnum.hxx>
#include <TopExp_Explorer.hxx>
#include <TopLoc_Location.hxx>
#include <TopTools_ListOfShape.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Face.hxx>
#include <TopoDS_Shape.hxx>
#include <TopoDS_Shell.hxx>
#include <TopoDS_Wire.hxx>
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <gp_Circ.hxx>
#include <gp_Dir.hxx>
#include <gp_Pln.hxx>
#include <gp_Pnt.hxx>
#include <gp_Trsf.hxx>
#include <gp_Vec.hxx>
#include <limits>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "occt.hpp"

namespace tekla::db1::detail {
namespace {

[[nodiscard]] bool valid_box(const OcctBox& box) noexcept {
  return std::isfinite(box.x) && std::isfinite(box.y) && std::isfinite(box.z) &&
         std::isfinite(box.size_x) && std::isfinite(box.size_y) && std::isfinite(box.size_z) &&
         box.size_x > 0.0 && box.size_y > 0.0 && box.size_z > 0.0;
}

struct TraceBounds {
  std::array<double, 3> minimum{std::numeric_limits<double>::infinity(),
                                std::numeric_limits<double>::infinity(),
                                std::numeric_limits<double>::infinity()};
  std::array<double, 3> maximum{-std::numeric_limits<double>::infinity(),
                                -std::numeric_limits<double>::infinity(),
                                -std::numeric_limits<double>::infinity()};
  bool populated = false;
};

void include_trace_point(TraceBounds& bounds, double x, double y, double z) noexcept {
  if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z)) return;
  const std::array<double, 3> point{x, y, z};
  for (std::size_t axis = 0; axis < point.size(); ++axis) {
    bounds.minimum[axis] = std::min(bounds.minimum[axis], point[axis]);
    bounds.maximum[axis] = std::max(bounds.maximum[axis], point[axis]);
  }
  bounds.populated = true;
}

void include_trace_box(TraceBounds& bounds, const OcctBox& box) noexcept {
  if (!valid_box(box)) return;
  include_trace_point(bounds, box.x, box.y, box.z);
  include_trace_point(bounds, box.x + box.size_x, box.y + box.size_y, box.z + box.size_z);
}

void include_trace_mesh(TraceBounds& bounds, const OcctTriangleMesh& mesh) noexcept {
  for (std::size_t index = 0; index + 2U < mesh.positions.size(); index += 3U) {
    include_trace_point(bounds, mesh.positions[index], mesh.positions[index + 1U],
                        mesh.positions[index + 2U]);
  }
}

void include_trace_loop(TraceBounds& bounds, const OcctLoop& loop, double offset_x = 0.0,
                        double offset_y = 0.0, double offset_z = 0.0) noexcept {
  for (std::size_t index = 0; index + 2U < loop.positions.size(); index += 3U) {
    include_trace_point(bounds, loop.positions[index] + offset_x,
                        loop.positions[index + 1U] + offset_y,
                        loop.positions[index + 2U] + offset_z);
  }
}

[[nodiscard]] std::optional<TraceBounds> request_trace_bounds(const OcctRequest& request) noexcept {
  TraceBounds bounds;
  const auto include_node = [&](const OcctShapeNode& node) {
    include_trace_box(bounds, node.base);
    include_trace_mesh(bounds, node.base_mesh);
    for (const auto& loop : node.base_extrusion.loops) {
      include_trace_loop(bounds, loop);
      include_trace_loop(bounds, loop, node.base_extrusion.vector_x, node.base_extrusion.vector_y,
                         node.base_extrusion.vector_z);
    }
    for (const auto& loop : node.base_ruled_sweep.loops) {
      for (const auto& section : loop.sections) include_trace_loop(bounds, section);
    }
    for (const auto& cutter : node.subtract) include_trace_box(bounds, cutter);
  };
  if (!request.nodes.empty()) {
    for (const auto& node : request.nodes) include_node(node);
  } else {
    include_trace_box(bounds, request.base);
    include_trace_mesh(bounds, request.base_mesh);
    for (const auto& cutter : request.subtract) include_trace_box(bounds, cutter);
    for (const auto& cutter : request.subtract_meshes) include_trace_mesh(bounds, cutter);
  }
  return bounds.populated ? std::optional{bounds} : std::nullopt;
}

[[nodiscard]] const char* request_operation_kind(const OcctRequest& request) noexcept {
  if (!request.nodes.empty()) return "csg_graph";
  if (!request.subtract_meshes.empty()) return "legacy_mesh_boolean";
  if (!request.subtract.empty()) return "legacy_box_boolean";
  if (!request.keep_half_spaces.empty()) return "legacy_half_space";
  return "tessellation";
}

[[nodiscard]] const char* request_definition_kind(const OcctRequest& request) noexcept {
  if (request.nodes.empty()) return request.base_mesh.positions.empty() ? "box" : "mesh_fallback";
  const auto& root = request.nodes.front();
  if (!root.base_extrusion.loops.empty()) return "extrusion";
  if (!root.base_ruled_sweep.loops.empty()) return "ruled_sweep";
  if (!root.base_mesh.positions.empty()) return "mesh_fallback";
  return "box";
}

[[nodiscard]] TopoDS_Shape make_box(const OcctBox& box) {
  return BRepPrimAPI_MakeBox(gp_Pnt(box.x, box.y, box.z), box.size_x, box.size_y, box.size_z)
      .Shape();
}

[[nodiscard]] bool valid_mesh(const OcctTriangleMesh& mesh) noexcept {
  if (mesh.positions.size() < 9U || mesh.positions.size() % 3U != 0U || mesh.indices.size() < 3U ||
      mesh.indices.size() % 3U != 0U)
    return false;
  const auto vertex_count = mesh.positions.size() / 3U;
  return std::all_of(mesh.positions.begin(), mesh.positions.end(),
                     [](double value) { return std::isfinite(value); }) &&
         std::all_of(mesh.indices.begin(), mesh.indices.end(),
                     [&](std::uint32_t index) { return index < vertex_count; });
}

[[nodiscard]] bool valid_extrusion(const OcctExtrusion& extrusion) noexcept {
  using Vector = std::array<double, 3>;
  const auto point = [](const OcctLoop& loop, std::size_t index) {
    const auto offset = index * 3U;
    return Vector{loop.positions[offset], loop.positions[offset + 1U], loop.positions[offset + 2U]};
  };
  const auto subtract = [](Vector left, Vector right) {
    return Vector{left[0] - right[0], left[1] - right[1], left[2] - right[2]};
  };
  const auto cross = [](Vector left, Vector right) {
    return Vector{left[1] * right[2] - left[2] * right[1], left[2] * right[0] - left[0] * right[2],
                  left[0] * right[1] - left[1] * right[0]};
  };
  const auto dot = [](Vector left, Vector right) {
    return left[0] * right[0] + left[1] * right[1] + left[2] * right[2];
  };
  const auto length = [&](Vector value) { return std::sqrt(dot(value, value)); };

  const Vector extrusion_vector{extrusion.vector_x, extrusion.vector_y, extrusion.vector_z};
  const double extrusion_length = length(extrusion_vector);
  if (extrusion.loops.empty() || !std::isfinite(extrusion_length) || extrusion_length <= 1.0e-12) {
    return false;
  }
  if (!std::all_of(extrusion.loops.begin(), extrusion.loops.end(), [](const OcctLoop& loop) {
        return loop.positions.size() >= 9U && loop.positions.size() % 3U == 0U &&
               std::all_of(loop.positions.begin(), loop.positions.end(),
                           [](double value) { return std::isfinite(value); });
      })) {
    return false;
  }

  const auto& outer = extrusion.loops.front();
  const Vector origin = point(outer, 0U);
  Vector normal{};
  double extent = 0.0;
  for (std::size_t index = 1U; index < outer.positions.size() / 3U; ++index) {
    extent = std::max(extent, length(subtract(point(outer, index), origin)));
  }
  const double tolerance = std::max(1.0, extent) * 1.0e-10;
  for (std::size_t index = 1U; index + 1U < outer.positions.size() / 3U; ++index) {
    normal =
        cross(subtract(point(outer, index), origin), subtract(point(outer, index + 1U), origin));
    if (length(normal) > tolerance * tolerance) break;
  }
  const double normal_length = length(normal);
  if (!std::isfinite(normal_length) || normal_length <= tolerance * tolerance ||
      std::abs(dot(normal, extrusion_vector)) <= normal_length * extrusion_length * 1.0e-10) {
    return false;
  }

  for (const auto& loop : extrusion.loops) {
    Vector area{};
    const auto count = loop.positions.size() / 3U;
    for (std::size_t index = 0U; index < count; ++index) {
      const Vector current = point(loop, index);
      const Vector next = point(loop, (index + 1U) % count);
      if (length(subtract(next, current)) <= tolerance ||
          std::abs(dot(normal, subtract(current, origin))) > normal_length * tolerance) {
        return false;
      }
      const Vector contribution = cross(subtract(current, origin), subtract(next, origin));
      area[0] += contribution[0];
      area[1] += contribution[1];
      area[2] += contribution[2];
    }
    if (std::abs(dot(area, normal)) <= normal_length * tolerance * tolerance) return false;
  }
  return true;
}

[[nodiscard]] TopoDS_Wire make_loop_wire(const OcctLoop& loop) {
  BRepBuilderAPI_MakePolygon polygon;
  for (std::size_t index = 0; index + 2U < loop.positions.size(); index += 3U) {
    polygon.Add(
        gp_Pnt(loop.positions[index], loop.positions[index + 1U], loop.positions[index + 2U]));
  }
  polygon.Close();
  return polygon.IsDone() ? polygon.Wire() : TopoDS_Wire{};
}

[[nodiscard]] TopoDS_Shape make_extrusion(const OcctExtrusion& extrusion) {
  const auto outer = make_loop_wire(extrusion.loops.front());
  if (outer.IsNull()) return {};
  BRepBuilderAPI_MakeFace face(outer);
  if (!face.IsDone()) return {};
  for (std::size_t index = 1U; index < extrusion.loops.size(); ++index) {
    const auto inner = make_loop_wire(extrusion.loops[index]);
    if (inner.IsNull()) return {};
    face.Add(TopoDS::Wire(inner.Reversed()));
    if (!face.IsDone()) return {};
  }
  const gp_Vec vector(extrusion.vector_x, extrusion.vector_y, extrusion.vector_z);
  BRepPrimAPI_MakePrism prism(face.Face(), vector);
  return prism.IsDone() ? prism.Shape() : TopoDS_Shape{};
}

[[nodiscard]] gp_Pnt section_centroid(const OcctLoop& section);

[[nodiscard]] bool valid_sweep_section(const OcctRuledSweep& sweep,
                                       std::size_t section_index) noexcept {
  const auto& outer = sweep.loops.front().sections[section_index];
  const gp_Pnt origin(outer.positions[0], outer.positions[1], outer.positions[2]);
  gp_Vec outer_area;
  double scale = 1.0;
  const auto accumulate_area = [&](const OcctLoop& loop, gp_Vec& area) {
    const auto point_count = loop.positions.size() / 3U;
    for (std::size_t point_index = 0U; point_index < point_count; ++point_index) {
      const auto next_index = (point_index + 1U) % point_count;
      const gp_Pnt current(loop.positions[point_index * 3U], loop.positions[point_index * 3U + 1U],
                           loop.positions[point_index * 3U + 2U]);
      const gp_Pnt next(loop.positions[next_index * 3U], loop.positions[next_index * 3U + 1U],
                        loop.positions[next_index * 3U + 2U]);
      const gp_Vec current_offset(origin, current);
      const gp_Vec next_offset(origin, next);
      scale = std::max({scale, current_offset.Magnitude(), next_offset.Magnitude()});
      area += current_offset.Crossed(next_offset);
    }
  };
  accumulate_area(outer, outer_area);
  const double tolerance = std::max(1.0e-7, scale * 1.0e-9);
  if (!std::isfinite(outer_area.Magnitude()) || outer_area.Magnitude() <= tolerance * tolerance) {
    return false;
  }
  const gp_Dir normal(outer_area);
  for (const auto& sweep_loop : sweep.loops) {
    const auto& loop = sweep_loop.sections[section_index];
    gp_Vec area;
    accumulate_area(loop, area);
    if (!std::isfinite(area.Magnitude()) || area.Magnitude() <= tolerance * tolerance) return false;
    const auto point_count = loop.positions.size() / 3U;
    for (std::size_t point_index = 0U; point_index < point_count; ++point_index) {
      const auto next_index = (point_index + 1U) % point_count;
      const gp_Pnt current(loop.positions[point_index * 3U], loop.positions[point_index * 3U + 1U],
                           loop.positions[point_index * 3U + 2U]);
      const gp_Pnt next(loop.positions[next_index * 3U], loop.positions[next_index * 3U + 1U],
                        loop.positions[next_index * 3U + 2U]);
      if (current.Distance(next) <= tolerance ||
          std::abs(gp_Vec(origin, current).Dot(gp_Vec(normal))) > tolerance) {
        return false;
      }
    }
  }
  return true;
}

[[nodiscard]] bool valid_ruled_sweep(const OcctRuledSweep& sweep) noexcept {
  if (sweep.loops.empty() || sweep.loops.size() > 4'096U ||
      sweep.loops.front().sections.size() < 2U || sweep.loops.front().sections.size() > 262'144U) {
    return false;
  }
  const auto section_count = sweep.loops.front().sections.size();
  if (sweep.circular_spine && section_count < 3U) return false;
  const bool counts_are_valid =
      std::all_of(sweep.loops.begin(), sweep.loops.end(), [&](const OcctSweepLoop& loop) {
        if (loop.sections.size() != section_count) return false;
        const auto coordinate_count = loop.sections.front().positions.size();
        return coordinate_count >= 9U && coordinate_count <= 16U * 1024U * 1024U &&
               coordinate_count % 3U == 0U &&
               std::all_of(loop.sections.begin(), loop.sections.end(),
                           [&](const OcctLoop& section) {
                             return section.positions.size() == coordinate_count &&
                                    std::all_of(section.positions.begin(), section.positions.end(),
                                                [](double value) { return std::isfinite(value); });
                           });
      });
  if (!counts_are_valid) return false;
  for (std::size_t section = 0U; section < section_count; ++section) {
    if (!valid_sweep_section(sweep, section)) return false;
    if (section != 0U &&
        section_centroid(sweep.loops.front().sections[section - 1U])
                .Distance(section_centroid(sweep.loops.front().sections[section])) <= 1.0e-9) {
      return false;
    }
  }
  return true;
}

[[nodiscard]] TopoDS_Shape make_ruled_sweep_loop(const OcctSweepLoop& loop) {
  BRepOffsetAPI_ThruSections builder(true, true, 1.0e-7);
  builder.CheckCompatibility(true);
  for (const auto& section : loop.sections) {
    const auto wire = make_loop_wire(section);
    if (wire.IsNull()) return {};
    builder.AddWire(wire);
  }
  builder.Build();
  return builder.IsDone() ? builder.Shape() : TopoDS_Shape{};
}

[[nodiscard]] gp_Pnt section_centroid(const OcctLoop& section) {
  double x = 0.0;
  double y = 0.0;
  double z = 0.0;
  const auto count = section.positions.size() / 3U;
  for (std::size_t index = 0U; index < count; ++index) {
    x += section.positions[index * 3U];
    y += section.positions[index * 3U + 1U];
    z += section.positions[index * 3U + 2U];
  }
  const auto divisor = static_cast<double>(count);
  return {x / divisor, y / divisor, z / divisor};
}

[[nodiscard]] TopoDS_Shape make_circular_sweep(const OcctRuledSweep& sweep) {
  const auto& outer_loop = sweep.loops.front();
  if (outer_loop.sections.size() < 3U) return {};
  const auto first = section_centroid(outer_loop.sections.front());
  const auto middle = section_centroid(outer_loop.sections[outer_loop.sections.size() / 2U]);
  const auto last = section_centroid(outer_loop.sections.back());
  GC_MakeArcOfCircle arc(first, middle, last);
  if (!arc.IsDone()) return {};
  const Handle(Geom_TrimmedCurve) curve = arc.Value();
  const Handle(Geom_Circle) circle = Handle(Geom_Circle)::DownCast(curve->BasisCurve());
  if (circle.IsNull()) return {};
  const auto axis = circle->Circ().Axis();
  const auto center = axis.Location();
  const auto axis_direction = axis.Direction();
  const gp_Vec first_radial(center, first);
  const double radius = first_radial.Magnitude();
  const double tolerance = std::max(1.0e-6, radius * 1.0e-7);
  if (!std::isfinite(radius) || radius <= tolerance) return {};
  for (std::size_t section_index = 0U; section_index < outer_loop.sections.size();
       ++section_index) {
    const auto current_center = section_centroid(outer_loop.sections[section_index]);
    const gp_Vec current(center, current_center);
    if (std::abs(current.Dot(gp_Vec(axis_direction))) > tolerance ||
        std::abs(current.Magnitude() - radius) > tolerance) {
      return {};
    }
    const double angle = std::atan2(gp_Vec(axis_direction).Dot(first_radial.Crossed(current)),
                                    first_radial.Dot(current));
    gp_Trsf rotation;
    rotation.SetRotation(axis, angle);
    for (std::size_t loop_index = 0U; loop_index < sweep.loops.size(); ++loop_index) {
      const auto& source = sweep.loops[loop_index].sections.front();
      const auto& target = sweep.loops[loop_index].sections[section_index];
      for (std::size_t coordinate = 0U; coordinate < source.positions.size(); coordinate += 3U) {
        const gp_Pnt source_point(source.positions[coordinate], source.positions[coordinate + 1U],
                                  source.positions[coordinate + 2U]);
        const gp_Pnt target_point(target.positions[coordinate], target.positions[coordinate + 1U],
                                  target.positions[coordinate + 2U]);
        if (source_point.Transformed(rotation).Distance(target_point) > tolerance) return {};
      }
    }
  }
  const auto outer_wire = make_loop_wire(outer_loop.sections.front());
  if (outer_wire.IsNull()) return {};
  BRepBuilderAPI_MakeFace profile(outer_wire);
  if (!profile.IsDone()) return {};
  for (std::size_t index = 1U; index < sweep.loops.size(); ++index) {
    const auto inner_wire = make_loop_wire(sweep.loops[index].sections.front());
    if (inner_wire.IsNull()) return {};
    profile.Add(TopoDS::Wire(inner_wire.Reversed()));
    if (!profile.IsDone()) return {};
  }
  const double angle = curve->LastParameter() - curve->FirstParameter();
  BRepPrimAPI_MakeRevol revolution(profile.Face(), axis, angle, false);
  return revolution.IsDone() ? revolution.Shape() : TopoDS_Shape{};
}

[[nodiscard]] TopoDS_Shape make_ruled_sweep(const OcctRuledSweep& sweep) {
  if (sweep.circular_spine) {
    const auto revolved = make_circular_sweep(sweep);
    if (!revolved.IsNull()) return revolved;
  }
  TopoDS_Shape result = make_ruled_sweep_loop(sweep.loops.front());
  if (result.IsNull()) return {};
  for (std::size_t index = 1U; index < sweep.loops.size(); ++index) {
    const TopoDS_Shape void_shape = make_ruled_sweep_loop(sweep.loops[index]);
    if (void_shape.IsNull()) return {};
    BRepAlgoAPI_Cut cut(result, void_shape);
    cut.SetRunParallel(false);
    cut.SetFuzzyValue(1.0e-7);
    cut.Build();
    if (!cut.IsDone()) return {};
    result = cut.Shape();
  }
  return result;
}

[[nodiscard]] TopoDS_Shape make_extruded_mesh_solid(const OcctTriangleMesh& mesh) {
  const std::size_t vertex_count = mesh.positions.size() / 3U;
  if (vertex_count < 6U || vertex_count % 2U != 0U) return {};
  const std::size_t ring_count = vertex_count / 2U;
  if (mesh.indices.size() != ring_count * 12U - 12U) return {};
  const auto point = [&](std::size_t vertex) {
    const std::size_t offset = vertex * 3U;
    return gp_Pnt(mesh.positions[offset], mesh.positions[offset + 1U], mesh.positions[offset + 2U]);
  };
  const gp_Pnt first = point(0U);
  const gp_Pnt second = point(ring_count);
  const gp_Vec extrusion(first, second);
  if (extrusion.SquareMagnitude() <= 1.0e-18) return {};
  for (std::size_t index = 1U; index < ring_count; ++index) {
    const gp_Vec candidate(point(index), point(index + ring_count));
    if (!candidate.IsEqual(extrusion, 1.0e-8, 1.0e-8)) return {};
  }
  BRepBuilderAPI_MakePolygon polygon;
  for (std::size_t index = 0; index < ring_count; ++index) {
    polygon.Add(point(index));
  }
  polygon.Close();
  if (!polygon.IsDone()) return {};
  BRepBuilderAPI_MakeFace face(polygon.Wire());
  if (!face.IsDone()) return {};
  BRepPrimAPI_MakePrism prism(face.Face(), extrusion);
  return prism.IsDone() ? prism.Shape() : TopoDS_Shape{};
}

[[nodiscard]] TopoDS_Shape make_mesh_solid(const OcctTriangleMesh& mesh) {
  const bool profile = std::getenv("TEKLA_DB1_OCCT_PROFILE") != nullptr;
  const auto started = std::chrono::steady_clock::now();
  if (auto prism = make_extruded_mesh_solid(mesh); !prism.IsNull()) {
    if (profile) {
      const double seconds =
          std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
      std::fprintf(stderr,
                   "{\"occt_mesh_solid\":true,\"path\":\"prism\","
                   "\"vertices\":%zu,\"triangles\":%zu,\"seconds\":%.9f}\n",
                   mesh.positions.size() / 3U, mesh.indices.size() / 3U, seconds);
    }
    return prism;
  }
  BRepBuilderAPI_Sewing sewing(1.0e-5, true, true, true, false);
  for (std::size_t index = 0; index < mesh.indices.size(); index += 3U) {
    BRepBuilderAPI_MakePolygon polygon;
    for (std::size_t corner = 0; corner < 3U; ++corner) {
      const auto vertex = static_cast<std::size_t>(mesh.indices[index + corner]) * 3U;
      polygon.Add(
          gp_Pnt(mesh.positions[vertex], mesh.positions[vertex + 1U], mesh.positions[vertex + 2U]));
    }
    polygon.Close();
    if (!polygon.IsDone()) return {};
    BRepBuilderAPI_MakeFace face(polygon.Wire());
    if (!face.IsDone()) return {};
    sewing.Add(face.Face());
  }
  sewing.Perform();
  const TopoDS_Shape sewn = sewing.SewedShape();
  if (sewn.IsNull()) return {};
  BRepBuilderAPI_MakeSolid solid;
  if (sewn.ShapeType() == TopAbs_SHELL) {
    solid.Add(TopoDS::Shell(sewn));
  } else {
    for (TopExp_Explorer explorer(sewn, TopAbs_SHELL); explorer.More(); explorer.Next()) {
      solid.Add(TopoDS::Shell(explorer.Current()));
    }
  }
  if (!solid.IsDone()) return {};
  TopoDS_Solid result = solid.Solid();
  if (!BRepLib::OrientClosedSolid(result)) return {};
  if (profile) {
    const double seconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    std::fprintf(stderr,
                 "{\"occt_mesh_solid\":true,\"path\":\"sewing\","
                 "\"vertices\":%zu,\"triangles\":%zu,\"seconds\":%.9f}\n",
                 mesh.positions.size() / 3U, mesh.indices.size() / 3U, seconds);
  }
  return result;
}

[[nodiscard]] bool valid_half_space(const OcctHalfSpace& plane) noexcept {
  const double magnitude = std::hypot(plane.normal_x, plane.normal_y, plane.normal_z);
  return std::isfinite(plane.origin_x) && std::isfinite(plane.origin_y) &&
         std::isfinite(plane.origin_z) && std::isfinite(magnitude) && magnitude > 1.0e-12;
}

struct CsgBuildProfile {
  double base_seconds = 0.0;
  double half_space_seconds = 0.0;
  double box_boolean_seconds = 0.0;
  double mesh_boolean_seconds = 0.0;
  std::uint64_t cutter_triangles = 0U;
  std::uint64_t analytic_extrusion_nodes = 0U;
  std::uint64_t analytic_sweep_nodes = 0U;
  std::uint64_t mesh_fallback_nodes = 0U;
  std::uint64_t box_nodes = 0U;
};

[[nodiscard]] Result<TopoDS_Shape> build_csg_node(const OcctRequest& request,
                                                  std::uint32_t node_index, std::size_t depth,
                                                  std::vector<std::uint8_t>& states,
                                                  std::vector<TopoDS_Shape>& cache,
                                                  CsgBuildProfile& profile) {
  using Clock = std::chrono::steady_clock;
  constexpr std::size_t maximum_depth = 32U;
  if (node_index >= request.nodes.size()) {
    return Result<TopoDS_Shape>::failure(
        {ErrorCode::invalid_argument, "An OCCT CSG edge target is invalid."});
  }
  if (depth >= maximum_depth) {
    return Result<TopoDS_Shape>::failure(
        {ErrorCode::resource_limit, "An OCCT CSG graph exceeds the depth limit."});
  }
  if (states[node_index] == 1U) {
    return Result<TopoDS_Shape>::failure(
        {ErrorCode::invalid_topology, "An OCCT CSG graph contains a cycle."});
  }
  if (states[node_index] == 2U) return Result<TopoDS_Shape>::success(cache[node_index]);
  states[node_index] = 1U;
  const auto& node = request.nodes[node_index];
  const bool use_extrusion = !node.base_extrusion.loops.empty();
  const bool use_sweep = !node.base_ruled_sweep.loops.empty();
  const bool use_mesh = !node.base_mesh.positions.empty() || !node.base_mesh.indices.empty();
  const std::size_t source_count =
      static_cast<std::size_t>(use_extrusion) + static_cast<std::size_t>(use_sweep) +
      static_cast<std::size_t>(use_mesh) + static_cast<std::size_t>(valid_box(node.base));
  if (source_count != 1U || (use_extrusion ? !valid_extrusion(node.base_extrusion)
                             : use_sweep   ? !valid_ruled_sweep(node.base_ruled_sweep)
                             : use_mesh    ? !valid_mesh(node.base_mesh)
                                           : !valid_box(node.base))) {
    states[node_index] = 0U;
    return Result<TopoDS_Shape>::failure(
        {ErrorCode::invalid_argument, "An OCCT CSG node has invalid base geometry."});
  }
  const auto base_started = Clock::now();
  if (use_extrusion) {
    ++profile.analytic_extrusion_nodes;
  } else if (use_sweep) {
    ++profile.analytic_sweep_nodes;
  } else if (use_mesh) {
    ++profile.mesh_fallback_nodes;
  } else {
    ++profile.box_nodes;
  }
  if (std::getenv("TEKLA_DB1_OCCT_PROFILE") != nullptr) {
    std::fprintf(stderr,
                 "{\"occt_csg_node\":true,\"node_index\":%u,\"object_id\":%llu,"
                 "\"base_kind\":\"%s\",\"vertices\":%zu,\"triangles\":%zu}\n",
                 node_index, static_cast<unsigned long long>(node.object_id),
                 use_extrusion ? "extrusion"
                 : use_sweep   ? "ruled_sweep"
                 : use_mesh    ? "mesh_fallback"
                               : "box",
                 node.base_mesh.positions.size() / 3U, node.base_mesh.indices.size() / 3U);
  }
  TopoDS_Shape shape = use_extrusion ? make_extrusion(node.base_extrusion)
                       : use_sweep   ? make_ruled_sweep(node.base_ruled_sweep)
                       : use_mesh    ? make_mesh_solid(node.base_mesh)
                                     : make_box(node.base);
  profile.base_seconds += std::chrono::duration<double>(Clock::now() - base_started).count();
  if (shape.IsNull()) {
    states[node_index] = 0U;
    return Result<TopoDS_Shape>::failure(
        {ErrorCode::invalid_topology, "OCCT could not construct a CSG-node solid."});
  }
  if (std::getenv("TEKLA_DB1_OCCT_PROFILE") != nullptr) {
    GProp_GProps properties;
    BRepGProp::VolumeProperties(shape, properties);
    std::fprintf(stderr,
                 "{\"occt_csg_base\":true,\"node_index\":%u,\"object_id\":%llu,"
                 "\"volume\":%.17g}\n",
                 node_index, static_cast<unsigned long long>(node.object_id),
                 std::abs(properties.Mass()));
  }
  for (const auto& plane : node.keep_half_spaces) {
    const auto started = Clock::now();
    if (!valid_half_space(plane)) {
      states[node_index] = 0U;
      return Result<TopoDS_Shape>::failure(
          {ErrorCode::invalid_argument, "An OCCT CSG half-space is invalid."});
    }
    const gp_Dir direction(plane.normal_x, plane.normal_y, plane.normal_z);
    const gp_Pnt origin(plane.origin_x, plane.origin_y, plane.origin_z);
    const TopoDS_Face face =
        BRepBuilderAPI_MakeFace(gp_Pln(origin, direction), -1.0e7, 1.0e7, -1.0e7, 1.0e7);
    const gp_Pnt keep(plane.origin_x + direction.X(), plane.origin_y + direction.Y(),
                      plane.origin_z + direction.Z());
    BRepAlgoAPI_Common operation(shape, BRepPrimAPI_MakeHalfSpace(face, keep).Shape());
    operation.SetRunParallel(false);
    operation.Build();
    if (!operation.IsDone()) {
      states[node_index] = 0U;
      return Result<TopoDS_Shape>::failure(
          {ErrorCode::invalid_topology, "OCCT could not clip a CSG node."});
    }
    shape = operation.Shape();
    profile.half_space_seconds += std::chrono::duration<double>(Clock::now() - started).count();
  }
  for (const auto& cutter : node.subtract) {
    const auto started = Clock::now();
    if (!valid_box(cutter)) {
      states[node_index] = 0U;
      return Result<TopoDS_Shape>::failure(
          {ErrorCode::invalid_argument, "An OCCT CSG box cutter is invalid."});
    }
    BRepAlgoAPI_Cut operation(shape, make_box(cutter));
    operation.SetRunParallel(false);
    operation.SetFuzzyValue(1.0e-4);
    operation.Build();
    if (!operation.IsDone()) {
      states[node_index] = 0U;
      return Result<TopoDS_Shape>::failure(
          {ErrorCode::invalid_topology, "OCCT could not subtract a CSG box."});
    }
    shape = operation.Shape();
    profile.box_boolean_seconds += std::chrono::duration<double>(Clock::now() - started).count();
  }
  std::vector<TopoDS_Shape> union_shapes;
  union_shapes.reserve(node.union_nodes.size());
  for (const auto child_index : node.union_nodes) {
    auto child = build_csg_node(request, child_index, depth + 1U, states, cache, profile);
    if (!child) {
      states[node_index] = 0U;
      return child;
    }
    union_shapes.push_back(std::move(child.value()));
  }
  if (!union_shapes.empty()) {
    const auto started = Clock::now();
    BRepAlgoAPI_Fuse operation;
    TopTools_ListOfShape arguments;
    TopTools_ListOfShape tools;
    arguments.Append(shape);
    for (const auto& child : union_shapes) tools.Append(child);
    operation.SetArguments(arguments);
    operation.SetTools(tools);
    operation.SetRunParallel(false);
    operation.SetFuzzyValue(1.0e-4);
    operation.Build();
    if (!operation.IsDone()) {
      states[node_index] = 0U;
      return Result<TopoDS_Shape>::failure(
          {ErrorCode::invalid_topology, "OCCT could not fuse a nested CSG node."});
    }
    shape = operation.Shape();
    profile.mesh_boolean_seconds += std::chrono::duration<double>(Clock::now() - started).count();
  }
  std::vector<TopoDS_Shape> child_shapes;
  child_shapes.reserve(node.subtract_nodes.size());
  for (const auto child_index : node.subtract_nodes) {
    auto child = build_csg_node(request, child_index, depth + 1U, states, cache, profile);
    if (!child) {
      states[node_index] = 0U;
      return child;
    }
    profile.cutter_triangles += request.nodes[child_index].base_mesh.indices.size() / 3U;
    child_shapes.push_back(std::move(child.value()));
  }
  if (!child_shapes.empty()) {
    const auto started = Clock::now();
    BRepAlgoAPI_Cut operation;
    TopTools_ListOfShape arguments;
    TopTools_ListOfShape tools;
    arguments.Append(shape);
    for (const auto& child : child_shapes) tools.Append(child);
    operation.SetArguments(arguments);
    operation.SetTools(tools);
    operation.SetRunParallel(false);
    operation.SetFuzzyValue(1.0e-4);
    operation.Build();
    if (!operation.IsDone()) {
      states[node_index] = 0U;
      return Result<TopoDS_Shape>::failure(
          {ErrorCode::invalid_topology, "OCCT could not subtract a nested CSG node."});
    }
    shape = operation.Shape();
    profile.mesh_boolean_seconds += std::chrono::duration<double>(Clock::now() - started).count();
  }
  if (shape.IsNull()) {
    states[node_index] = 0U;
    return Result<TopoDS_Shape>::failure(
        {ErrorCode::invalid_topology, "OCCT produced a null CSG-node shape."});
  }
  cache[node_index] = shape;
  states[node_index] = 2U;
  return Result<TopoDS_Shape>::success(std::move(shape));
}

}  // namespace

namespace {

Result<OcctMesh> evaluate_occt_unchecked(const OcctRequest& request) {
  using Clock = std::chrono::steady_clock;
  const bool profile = std::getenv("TEKLA_DB1_OCCT_PROFILE") != nullptr;
  const auto total_started = Clock::now();
  double base_seconds = 0.0;
  double half_space_seconds = 0.0;
  double box_boolean_seconds = 0.0;
  double cutter_solid_seconds = 0.0;
  double mesh_boolean_seconds = 0.0;
  double tessellation_seconds = 0.0;
  double extraction_seconds = 0.0;
  std::uint64_t cutter_triangles = 0;
  std::uint64_t analytic_extrusion_nodes = 0;
  std::uint64_t analytic_sweep_nodes = 0;
  std::uint64_t mesh_fallback_nodes = 0;
  std::uint64_t box_nodes = 0;
  const bool use_graph = !request.nodes.empty();
  const bool use_mesh = !request.base_mesh.positions.empty() || !request.base_mesh.indices.empty();
  if ((!use_graph && (use_mesh ? !valid_mesh(request.base_mesh) : !valid_box(request.base))) ||
      !std::isfinite(request.linear_deflection) || !std::isfinite(request.angular_deflection) ||
      request.linear_deflection <= 0.0 || request.angular_deflection <= 0.0) {
    return Result<OcctMesh>::failure(
        {ErrorCode::invalid_argument, "The OCCT work item has invalid dimensions or tolerances."});
  }
  TopoDS_Shape shape;
  if (use_graph) {
    if (request.nodes.size() > 4096U) {
      return Result<OcctMesh>::failure(
          {ErrorCode::resource_limit, "The OCCT CSG graph exceeds the node limit."});
    }
    std::vector<std::uint8_t> states(request.nodes.size(), 0U);
    std::vector<TopoDS_Shape> cache(request.nodes.size());
    CsgBuildProfile graph_profile;
    auto built = build_csg_node(request, 0U, 0U, states, cache, graph_profile);
    if (!built) return Result<OcctMesh>::failure(std::move(built.error()));
    shape = built.value();
    base_seconds = graph_profile.base_seconds;
    half_space_seconds = graph_profile.half_space_seconds;
    box_boolean_seconds = graph_profile.box_boolean_seconds;
    mesh_boolean_seconds = graph_profile.mesh_boolean_seconds;
    cutter_triangles = graph_profile.cutter_triangles;
    analytic_extrusion_nodes = graph_profile.analytic_extrusion_nodes;
    analytic_sweep_nodes = graph_profile.analytic_sweep_nodes;
    mesh_fallback_nodes = graph_profile.mesh_fallback_nodes;
    box_nodes = graph_profile.box_nodes;
  } else {
    const auto base_started = Clock::now();
    shape = use_mesh ? make_mesh_solid(request.base_mesh) : make_box(request.base);
    base_seconds = std::chrono::duration<double>(Clock::now() - base_started).count();
    if (shape.IsNull())
      return Result<OcctMesh>::failure(
          {ErrorCode::invalid_topology, "OCCT could not sew the base mesh into a solid."});
  }
  for (const auto& plane : use_graph ? std::vector<OcctHalfSpace>{} : request.keep_half_spaces) {
    const auto operation_started = Clock::now();
    if (!valid_half_space(plane))
      return Result<OcctMesh>::failure(
          {ErrorCode::invalid_argument, "An OCCT half-space has invalid coordinates."});
    const gp_Dir direction(plane.normal_x, plane.normal_y, plane.normal_z);
    const gp_Pnt origin(plane.origin_x, plane.origin_y, plane.origin_z);
    const TopoDS_Face face =
        BRepBuilderAPI_MakeFace(gp_Pln(origin, direction), -1.0e7, 1.0e7, -1.0e7, 1.0e7);
    const gp_Pnt keep(plane.origin_x + direction.X(), plane.origin_y + direction.Y(),
                      plane.origin_z + direction.Z());
    BRepAlgoAPI_Common operation(shape, BRepPrimAPI_MakeHalfSpace(face, keep).Shape());
    operation.SetRunParallel(false);
    operation.Build();
    if (!operation.IsDone())
      return Result<OcctMesh>::failure(
          {ErrorCode::invalid_topology, "OCCT could not complete a half-space clip."});
    // A cutter may legitimately intersect only a fraction of its persisted
    // volume. Accept every completed OCCT result; supervised execution is the
    // containment boundary for kernel faults and timeouts.
    shape = operation.Shape();
    half_space_seconds += std::chrono::duration<double>(Clock::now() - operation_started).count();
  }
  for (const auto& cutter : use_graph ? std::vector<OcctBox>{} : request.subtract) {
    const auto operation_started = Clock::now();
    if (!valid_box(cutter))
      return Result<OcctMesh>::failure(
          {ErrorCode::invalid_argument, "An OCCT cutter has invalid dimensions."});
    const auto cutter_shape = make_box(cutter);
    BRepAlgoAPI_Cut operation(shape, cutter_shape);
    operation.SetRunParallel(false);
    operation.SetFuzzyValue(1.0e-4);
    operation.Build();
    if (!operation.IsDone())
      return Result<OcctMesh>::failure(
          {ErrorCode::invalid_topology, "OCCT could not complete a Boolean subtraction."});
    shape = operation.Shape();
    box_boolean_seconds += std::chrono::duration<double>(Clock::now() - operation_started).count();
  }
  const bool batch_mesh_cuts = !use_graph && std::getenv("TEKLA_DB1_OCCT_BATCH_CUT") != nullptr &&
                               request.subtract_meshes.size() > 1U;
  if (batch_mesh_cuts) {
    TopTools_ListOfShape arguments;
    TopTools_ListOfShape tools;
    arguments.Append(shape);
    for (const auto& cutter_mesh : request.subtract_meshes) {
      cutter_triangles += cutter_mesh.indices.size() / 3U;
      if (!valid_mesh(cutter_mesh))
        return Result<OcctMesh>::failure(
            {ErrorCode::invalid_argument, "An OCCT mesh cutter is invalid."});
      const auto solid_started = Clock::now();
      const auto cutter = make_mesh_solid(cutter_mesh);
      cutter_solid_seconds += std::chrono::duration<double>(Clock::now() - solid_started).count();
      if (cutter.IsNull())
        return Result<OcctMesh>::failure(
            {ErrorCode::invalid_topology, "OCCT could not sew a cutter mesh into a solid."});
      tools.Append(cutter);
    }
    const auto boolean_started = Clock::now();
    BRepAlgoAPI_Cut operation;
    operation.SetArguments(arguments);
    operation.SetTools(tools);
    operation.SetRunParallel(false);
    operation.SetFuzzyValue(1.0e-4);
    operation.Build();
    if (!operation.IsDone())
      return Result<OcctMesh>::failure(
          {ErrorCode::invalid_topology, "OCCT could not complete a mesh subtraction."});
    shape = operation.Shape();
    mesh_boolean_seconds = std::chrono::duration<double>(Clock::now() - boolean_started).count();
  } else if (!use_graph) {
    for (const auto& cutter_mesh : request.subtract_meshes) {
      cutter_triangles += cutter_mesh.indices.size() / 3U;
      if (!valid_mesh(cutter_mesh))
        return Result<OcctMesh>::failure(
            {ErrorCode::invalid_argument, "An OCCT mesh cutter is invalid."});
      const auto solid_started = Clock::now();
      const auto cutter = make_mesh_solid(cutter_mesh);
      cutter_solid_seconds += std::chrono::duration<double>(Clock::now() - solid_started).count();
      if (cutter.IsNull())
        return Result<OcctMesh>::failure(
            {ErrorCode::invalid_topology, "OCCT could not sew a cutter mesh into a solid."});
      const auto boolean_started = Clock::now();
      BRepAlgoAPI_Cut operation(shape, cutter);
      operation.SetRunParallel(false);
      operation.SetFuzzyValue(1.0e-4);
      operation.Build();
      if (!operation.IsDone())
        return Result<OcctMesh>::failure(
            {ErrorCode::invalid_topology, "OCCT could not complete a mesh subtraction."});
      shape = operation.Shape();
      mesh_boolean_seconds += std::chrono::duration<double>(Clock::now() - boolean_started).count();
    }
  }
  if (shape.IsNull())
    return Result<OcctMesh>::failure(
        {ErrorCode::invalid_topology, "OCCT produced a null result shape."});
  BRepCheck_Analyzer topology_check(shape, true);
  if (!topology_check.IsValid()) {
    return Result<OcctMesh>::failure(
        {ErrorCode::invalid_topology, "OCCT produced an invalid result topology."});
  }
  bool contains_solid = false;
  for (TopExp_Explorer explorer(shape, TopAbs_SOLID); explorer.More(); explorer.Next()) {
    contains_solid = true;
    break;
  }
  if (!contains_solid) {
    return Result<OcctMesh>::failure(
        {ErrorCode::invalid_topology, "OCCT produced no material solid."});
  }
  const auto tessellation_started = Clock::now();
  BRepMesh_IncrementalMesh mesher(shape, request.linear_deflection, false,
                                  request.angular_deflection, true);
  mesher.Perform();
  tessellation_seconds = std::chrono::duration<double>(Clock::now() - tessellation_started).count();
  if (!mesher.IsDone())
    return Result<OcctMesh>::failure(
        {ErrorCode::invalid_topology, "OCCT tessellation did not complete."});

  const auto extraction_started = Clock::now();
  OcctMesh mesh;
  mesh.object_id = request.object_id;
  GProp_GProps surface_properties;
  GProp_GProps volume_properties;
  BRepGProp::SurfaceProperties(shape, surface_properties);
  BRepGProp::VolumeProperties(shape, volume_properties);
  mesh.exact_surface_area = surface_properties.Mass();
  mesh.exact_volume = std::abs(volume_properties.Mass());
  mesh.has_exact_metrics = std::isfinite(mesh.exact_surface_area) &&
                           mesh.exact_surface_area > 0.0 &&
                           std::isfinite(mesh.exact_volume) && mesh.exact_volume > 0.0;
  for (TopExp_Explorer explorer(shape, TopAbs_FACE); explorer.More(); explorer.Next()) {
    const TopoDS_Face face = TopoDS::Face(explorer.Current());
    TopLoc_Location location;
    const Handle(Poly_Triangulation) triangulation = BRep_Tool::Triangulation(face, location);
    if (triangulation.IsNull()) continue;
    const auto base_index = static_cast<std::uint32_t>(mesh.positions.size() / 3U);
    for (Standard_Integer node = 1; node <= triangulation->NbNodes(); ++node) {
      const gp_Pnt transformed = triangulation->Node(node).Transformed(location.Transformation());
      mesh.positions.insert(mesh.positions.end(), {static_cast<float>(transformed.X()),
                                                   static_cast<float>(transformed.Y()),
                                                   static_cast<float>(transformed.Z())});
    }
    for (Standard_Integer triangle = 1; triangle <= triangulation->NbTriangles(); ++triangle) {
      Standard_Integer first = 0;
      Standard_Integer second = 0;
      Standard_Integer third = 0;
      triangulation->Triangle(triangle).Get(first, second, third);
      if (face.Orientation() == TopAbs_REVERSED) std::swap(second, third);
      mesh.indices.insert(mesh.indices.end(), {base_index + static_cast<std::uint32_t>(first - 1),
                                               base_index + static_cast<std::uint32_t>(second - 1),
                                               base_index + static_cast<std::uint32_t>(third - 1)});
    }
  }
  if (mesh.positions.empty() || mesh.indices.empty())
    return Result<OcctMesh>::failure(
        {ErrorCode::invalid_topology, "OCCT produced no display triangles."});
  extraction_seconds = std::chrono::duration<double>(Clock::now() - extraction_started).count();
  if (profile) {
    const double total_seconds =
        std::chrono::duration<double>(Clock::now() - total_started).count();
    std::array<double, 6> result_bounds{mesh.positions[0], mesh.positions[1], mesh.positions[2],
                                        mesh.positions[0], mesh.positions[1], mesh.positions[2]};
    for (std::size_t index = 3U; index + 2U < mesh.positions.size(); index += 3U) {
      for (std::size_t axis = 0; axis < 3U; ++axis) {
        result_bounds[axis] =
            std::min(result_bounds[axis], static_cast<double>(mesh.positions[index + axis]));
        result_bounds[axis + 3U] =
            std::max(result_bounds[axis + 3U], static_cast<double>(mesh.positions[index + axis]));
      }
    }
    std::fprintf(
        stderr,
        "{\"occt_profile\":true,\"object_id\":%llu,\"operation_kind\":\"%s\","
        "\"definition_kind\":\"%s\",\"outcome\":\"success\","
        "\"linear_deflection\":%.9g,\"angular_deflection\":%.9g,"
        "\"result_bounds\":[%.9g,%.9g,%.9g,%.9g,%.9g,%.9g],"
        "\"output_vertices\":%zu,\"output_triangles\":%zu,\"base_vertices\":%zu,"
        "\"base_triangles\":%zu,\"half_space_count\":%zu,\"box_cutter_count\":%zu,"
        "\"mesh_cutter_count\":%zu,\"mesh_cutter_triangles\":%llu,"
        "\"analytic_extrusion_nodes\":%llu,\"analytic_sweep_nodes\":%llu,"
        "\"mesh_fallback_nodes\":%llu,"
        "\"box_nodes\":%llu,"
        "\"base_seconds\":%.9f,\"half_space_seconds\":%.9f,"
        "\"box_boolean_seconds\":%.9f,\"cutter_solid_seconds\":%.9f,"
        "\"mesh_boolean_seconds\":%.9f,\"tessellation_seconds\":%.9f,"
        "\"extraction_seconds\":%.9f,\"total_seconds\":%.9f}\n",
        static_cast<unsigned long long>(request.object_id), request_operation_kind(request),
        request_definition_kind(request), request.linear_deflection, request.angular_deflection,
        result_bounds[0], result_bounds[1], result_bounds[2], result_bounds[3], result_bounds[4],
        result_bounds[5], mesh.positions.size() / 3U, mesh.indices.size() / 3U,
        use_graph ? request.nodes.front().base_mesh.positions.size() / 3U
                  : request.base_mesh.positions.size() / 3U,
        use_graph ? request.nodes.front().base_mesh.indices.size() / 3U
                  : request.base_mesh.indices.size() / 3U,
        use_graph ? request.nodes.front().keep_half_spaces.size() : request.keep_half_spaces.size(),
        use_graph ? request.nodes.front().subtract.size() : request.subtract.size(),
        use_graph ? request.nodes.front().subtract_nodes.size() : request.subtract_meshes.size(),
        static_cast<unsigned long long>(cutter_triangles),
        static_cast<unsigned long long>(analytic_extrusion_nodes),
        static_cast<unsigned long long>(analytic_sweep_nodes),
        static_cast<unsigned long long>(mesh_fallback_nodes),
        static_cast<unsigned long long>(box_nodes), base_seconds, half_space_seconds,
        box_boolean_seconds, cutter_solid_seconds, mesh_boolean_seconds, tessellation_seconds,
        extraction_seconds, total_seconds);
  }
  return Result<OcctMesh>::success(std::move(mesh));
}

}  // namespace

Result<OcctMesh> invoke_occt_guarded(const OcctRequest& request, OcctEvaluatorFunction evaluator) {
  if (evaluator == nullptr) {
    return Result<OcctMesh>::failure(
        {ErrorCode::invalid_argument, "The topology evaluator is missing."});
  }
  const auto started = std::chrono::steady_clock::now();
  const auto run = [&]() -> Result<OcctMesh> {
    try {
      return evaluator(request);
    } catch (const Standard_Failure& failure) {
      const char* message = failure.GetMessageString();
      return Result<OcctMesh>::failure(
          {ErrorCode::invalid_topology, message != nullptr && *message != '\0'
                                            ? std::string("OCCT rejected the topology: ") + message
                                            : "OCCT rejected the topology."});
    } catch (const std::bad_alloc&) {
      return Result<OcctMesh>::failure(
          {ErrorCode::resource_limit, "The topology evaluator exhausted its memory budget."});
    } catch (const std::exception& failure) {
      return Result<OcctMesh>::failure(
          {ErrorCode::invalid_topology,
           std::string("The topology evaluator failed: ") + failure.what()});
    } catch (...) {
      return Result<OcctMesh>::failure(
          {ErrorCode::invalid_topology,
           "The topology evaluator failed with an unknown exception."});
    }
  };
  auto result = run();
  if (std::getenv("TEKLA_DB1_OCCT_PROFILE") != nullptr) {
    const double seconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    const auto bounds = request_trace_bounds(request);
    if (bounds) {
      std::fprintf(
          stderr,
          "{\"occt_outcome\":true,\"object_id\":%llu,"
          "\"operation_kind\":\"%s\",\"definition_kind\":\"%s\","
          "\"outcome\":\"%s\",\"error_code\":%u,"
          "\"linear_deflection\":%.9g,\"angular_deflection\":%.9g,"
          "\"input_bounds\":[%.9g,%.9g,%.9g,%.9g,%.9g,%.9g],"
          "\"seconds\":%.9f}\n",
          static_cast<unsigned long long>(request.object_id), request_operation_kind(request),
          request_definition_kind(request), result ? "success" : "failed",
          result ? 0U : static_cast<unsigned>(result.error().code), request.linear_deflection,
          request.angular_deflection, bounds->minimum[0], bounds->minimum[1], bounds->minimum[2],
          bounds->maximum[0], bounds->maximum[1], bounds->maximum[2], seconds);
    } else {
      std::fprintf(stderr,
                   "{\"occt_outcome\":true,\"object_id\":%llu,"
                   "\"operation_kind\":\"%s\",\"definition_kind\":\"%s\","
                   "\"outcome\":\"%s\",\"error_code\":%u,"
                   "\"linear_deflection\":%.9g,\"angular_deflection\":%.9g,"
                   "\"input_bounds\":null,\"seconds\":%.9f}\n",
                   static_cast<unsigned long long>(request.object_id),
                   request_operation_kind(request), request_definition_kind(request),
                   result ? "success" : "failed",
                   result ? 0U : static_cast<unsigned>(result.error().code),
                   request.linear_deflection, request.angular_deflection, seconds);
    }
  }
  return result;
}

Result<OcctMesh> evaluate_occt(const OcctRequest& request) {
  return invoke_occt_guarded(request, &evaluate_occt_unchecked);
}

Result<OcctMesh> DirectOcctHost::evaluate(const OcctRequest& request) const {
  return evaluate_occt(request);
}

}  // namespace tekla::db1::detail

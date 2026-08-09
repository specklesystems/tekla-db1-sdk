#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <span>
#include <tekla/db1/model.hpp>
#include <tuple>
#include <unordered_set>
#include <vector>

#include "schema.hpp"

namespace {

int failures = 0;

#define CHECK(condition, message)         \
  do {                                    \
    if (!(condition)) {                   \
      std::printf("FAIL: %s\n", message); \
      ++failures;                         \
    }                                     \
  } while (false)

void append_u32(std::vector<std::byte>& bytes, std::uint32_t value) {
  for (unsigned shift = 0; shift != 32; shift += 8) {
    bytes.push_back(static_cast<std::byte>((value >> shift) & 0xffU));
  }
}

void append_f64(std::vector<std::byte>& bytes, double value) {
  const auto bits = std::bit_cast<std::uint64_t>(value);
  append_u32(bytes, static_cast<std::uint32_t>(bits));
  append_u32(bytes, static_cast<std::uint32_t>(bits >> 32U));
}

void append_fixed(std::vector<std::byte>& bytes, std::string_view value, std::size_t size) {
  const auto count = std::min(value.size(), size);
  const auto* first = reinterpret_cast<const std::byte*>(value.data());
  bytes.insert(bytes.end(), first, first + count);
  bytes.insert(bytes.end(), size - count, std::byte{0});
}

void write_u32(std::span<std::byte> bytes, std::size_t offset, std::uint32_t value) {
  for (unsigned shift = 0; shift != 32; shift += 8) {
    bytes[offset + shift / 8U] = static_cast<std::byte>((value >> shift) & 0xffU);
  }
}

void write_f64(std::span<std::byte> bytes, std::size_t offset, double value) {
  const auto bits = std::bit_cast<std::uint64_t>(value);
  write_u32(bytes, offset, static_cast<std::uint32_t>(bits));
  write_u32(bytes, offset + 4, static_cast<std::uint32_t>(bits >> 32U));
}

void write_fixed(std::span<std::byte> bytes, std::size_t offset, std::size_t size,
                 std::string_view value) {
  const auto count = std::min(value.size(), size);
  std::memcpy(bytes.data() + offset, value.data(), count);
}

void append_ascii(std::vector<std::byte>& bytes, const char* text) {
  const auto size = std::strlen(text);
  const auto* first = reinterpret_cast<const std::byte*>(text);
  bytes.insert(bytes.end(), first, first + size);
}

void replace_ascii_once(std::vector<std::byte>& bytes, std::string_view from, std::string_view to) {
  CHECK(to.size() <= from.size(), "a fixed-width test replacement does not grow");
  const auto* first = reinterpret_cast<const std::byte*>(from.data());
  const auto found = std::search(bytes.begin(), bytes.end(), first, first + from.size());
  CHECK(found != bytes.end(), "the fixed-width test replacement target exists");
  if (found == bytes.end() || to.size() > from.size()) return;
  std::fill(found, found + static_cast<std::ptrdiff_t>(from.size()), std::byte{0});
  const auto* replacement = reinterpret_cast<const std::byte*>(to.data());
  std::copy(replacement, replacement + to.size(), found);
}

enum class PartFrameFixture {
  orthogonal,
  skewed,
  rotated,
};

enum class PolygonWeldFrameFixture {
  orthogonal,
  leg_parallel_to_tangent,
  leg_almost_parallel_to_tangent,
  reversed_handedness,
  noisy_tangent_component,
  collinear_model_basis,
  large_model_origin,
};

void append_table(std::vector<std::byte>& bytes, const tekla::db1::detail::Schema& schema,
                  const tekla::db1::detail::TableSchema& table, bool final,
                  std::string_view profile = "200*10", std::string_view object_class = "14",
                  bool boolean_operative = false, std::uint32_t form_type = 7, bool contour = false,
                  bool polybeam = false, bool lofted = false, bool edge_chamfer = false,
                  bool arc_contour = false, bool component_fixture = false,
                  bool keyed_footer = false, bool weld_fixture = false,
                  bool zero_transverse_axis = false, bool duplicate_property_fixture = false,
                  bool report_fixture = false, bool zero_position_report_fixture = false,
                  bool bolt_fixture = false, bool legacy_bolt_fixture = false,
                  bool assembly_fixture = false,
                  PartFrameFixture part_frame = PartFrameFixture::orthogonal,
                  bool mixed_contour = false) {
  constexpr std::array<std::byte, 4> table_end{std::byte{0x66}, std::byte{0xc0}, std::byte{0xce},
                                               std::byte{0xdb}};
  constexpr std::array<std::byte, 4> final_footer{std::byte{0x4f}, std::byte{0x61}, std::byte{0xbc},
                                                  std::byte{0x00}};
  append_u32(bytes, table.tuple_size);
  append_u32(bytes, table.descriptor_count);
  for (const auto descriptor : schema.table_descriptors(table)) {
    append_u32(bytes, descriptor);
  }

  if (table.name == "object" && !legacy_bolt_fixture) {
    bytes.push_back(std::byte{0});
    append_u32(bytes, 1201);
    append_u32(bytes, weld_fixture ? 1201U : 0U);
    append_u32(bytes, 700);
    constexpr std::array<std::uint8_t, 16> guid{0x12, 0x34, 0x56, 0x78, 0x12, 0x34, 0x4a, 0xbc,
                                                0x8d, 0xef, 0x12, 0x34, 0x56, 0x78, 0x9a, 0xbc};
    for (const auto byte : guid) {
      bytes.push_back(static_cast<std::byte>(byte));
    }
    append_u32(bytes, weld_fixture                          ? 13U
                      : bolt_fixture || legacy_bolt_fixture ? 10U
                      : assembly_fixture                    ? 2U
                                                            : 1U);
    append_u32(bytes, weld_fixture                          ? 0U
                      : bolt_fixture || legacy_bolt_fixture ? 1U
                      : assembly_fixture                    ? 0U
                                                            : 22U);
    for (int index = 0; index < 4; ++index) {
      append_u32(bytes, 0);
    }
    append_u32(bytes, report_fixture ? 256U : 3U);
    append_u32(bytes, 4);
    append_u32(bytes, 5);
    append_u32(bytes, 6);
    append_u32(bytes, 7);
    append_u32(bytes, 91);
    append_u32(bytes, 92);
    if (weld_fixture) {
      bytes.push_back(std::byte{0});
      const auto tuple_offset = bytes.size();
      bytes.resize(bytes.size() + table.tuple_size, std::byte{0});
      auto tuple = std::span<std::byte>(bytes).subspan(tuple_offset, table.tuple_size);
      for (const auto& field : schema.table_fields(table)) {
        if (field.name == "id" || field.name == "kuuluu") write_u32(tuple, field.offset, 1202U);
        if (field.name == "assembly") write_u32(tuple, field.offset, 700U);
        if (field.name == "type") write_u32(tuple, field.offset, 13U);
        if (field.name == "subtype") write_u32(tuple, field.offset, 0U);
        if (field.name == "guid") {
          for (std::size_t index = 0; index < field.size; ++index) {
            tuple[field.offset + index] = static_cast<std::byte>(0xd0U + index);
          }
        }
      }
      append_u32(bytes, 93U);
      append_u32(bytes, 94U);
    }
    if (component_fixture) {
      const auto append_object = [&](std::uint32_t id, std::uint32_t parent, std::uint32_t type,
                                     std::uint8_t guid_seed) {
        bytes.push_back(std::byte{0});
        const auto tuple_offset = bytes.size();
        bytes.resize(bytes.size() + table.tuple_size, std::byte{0});
        auto tuple = std::span<std::byte>(bytes).subspan(tuple_offset, table.tuple_size);
        for (const auto& field : schema.table_fields(table)) {
          if (field.name == "id") write_u32(tuple, field.offset, id);
          if (field.name == "kuuluu") write_u32(tuple, field.offset, parent);
          if (field.name == "assembly") write_u32(tuple, field.offset, 700U);
          if (field.name == "type") write_u32(tuple, field.offset, type);
          if (field.name == "subtype") write_u32(tuple, field.offset, 0U);
          if (field.name == "guid") {
            for (std::size_t index = 0; index < field.size; ++index) {
              tuple[field.offset + index] =
                  static_cast<std::byte>(guid_seed + static_cast<std::uint8_t>(index));
            }
          }
        }
        append_u32(bytes, id + 100U);
        append_u32(bytes, id + 200U);
      };
      append_object(2000U, 0U, 4U, 0x20U);
      append_object(2001U, 2000U, 1U, 0x30U);
      append_object(2002U, 2000U, 10U, 0x40U);
      append_object(2100U, 0U, 4U, 0x50U);
      append_object(2101U, 2100U, 2U, 0x60U);
    }
    if (assembly_fixture) {
      const auto append_object = [&](std::uint32_t id, std::uint32_t parent, std::uint32_t assembly,
                                     std::uint32_t type, std::uint32_t subtype,
                                     std::uint8_t guid_seed) {
        bytes.push_back(std::byte{0});
        const auto tuple_offset = bytes.size();
        bytes.resize(bytes.size() + table.tuple_size, std::byte{0});
        auto tuple = std::span<std::byte>(bytes).subspan(tuple_offset, table.tuple_size);
        for (const auto& field : schema.table_fields(table)) {
          if (field.name == "id") write_u32(tuple, field.offset, id);
          if (field.name == "kuuluu") write_u32(tuple, field.offset, parent);
          if (field.name == "assembly") write_u32(tuple, field.offset, assembly);
          if (field.name == "type") write_u32(tuple, field.offset, type);
          if (field.name == "subtype") write_u32(tuple, field.offset, subtype);
          if (field.name == "guid") {
            for (std::size_t index = 0; index < field.size; ++index) {
              tuple[field.offset + index] =
                  static_cast<std::byte>(guid_seed + static_cast<std::uint8_t>(index));
            }
          }
        }
        append_u32(bytes, id + 100U);
        append_u32(bytes, id + 200U);
      };
      append_object(1202U, 1201U, 700U, 2U, 0U, 0x70U);
      append_object(1203U, 0U, 700U, 2U, 0U, 0x80U);
      append_object(1204U, 0U, 0U, 47U, 0U, 0xa0U);
      if (schema.internal_format == "9.52" || schema.internal_format == "9.66") {
        append_object(1205U, 1201U, 0U, 11U, 0U, 0xb0U);
        append_object(1206U, 1201U, 0U, 12U, 0U, 0xc0U);
      }
      append_object(700U, 0U, 700U, 15U, 0U, 0x90U);
    }
  } else if (table.name == "numattr_attr") {
    bytes.push_back(std::byte{0});
    append_u32(bytes, 501);
    append_u32(bytes, 0);
    append_f64(bytes, 12.5);
    append_fixed(bytes, "HEIGHT", 24);
    append_u32(bytes, 0);
    append_u32(bytes, 71);
    append_u32(bytes, 72);
  } else if (table.name == "strattr_attr") {
    const auto append_string_definition = [&](std::uint32_t id, std::string_view name,
                                              std::string_view value) {
      bytes.push_back(std::byte{0});
      append_u32(bytes, id);
      append_u32(bytes, 0);
      append_fixed(bytes, name, 21);
      append_fixed(bytes, value, 83);
      append_u32(bytes, 0);
      append_u32(bytes, 0);
      append_u32(bytes, 73);
      append_u32(bytes, 74);
    };
    append_string_definition(502, duplicate_property_fixture ? "HEIGHT" : "LABEL",
                             duplicate_property_fixture ? "12.5" : "Beam A");
    if (duplicate_property_fixture) append_string_definition(504, "LABEL", "Beam A");
  } else if (table.name == "idattr_attr") {
    bytes.push_back(std::byte{0});
    append_u32(bytes, 503);
    append_u32(bytes, 0);
    append_u32(bytes, 700);
    append_fixed(bytes, "OWNER", 24);
    append_u32(bytes, 0);
    append_u32(bytes, 75);
    append_u32(bytes, 76);
  } else if (table.name == "numattr" || table.name == "strattr" || table.name == "idattr") {
    const auto append_attribute_link = [&](std::uint32_t id, std::uint32_t definition_id) {
      bytes.push_back(std::byte{0});
      append_u32(bytes, id);
      append_u32(bytes, definition_id);
      append_u32(bytes, 1201);
      append_u32(bytes, 0);
      append_u32(bytes, 0);
      append_u32(bytes, 0);
      append_u32(bytes, 77);
      append_u32(bytes, 78);
    };
    append_attribute_link(table.name == "numattr"   ? 601
                          : table.name == "strattr" ? 602
                                                    : 603,
                          table.name == "numattr"   ? 501
                          : table.name == "strattr" ? 502
                                                    : 503);
    if (duplicate_property_fixture && table.name == "strattr") {
      append_attribute_link(604, 504);
    }
  } else if (table.name == "relation") {
    const auto append_relation = [&](std::uint32_t id, std::uint32_t type, std::uint32_t source,
                                     std::uint32_t target) {
      bytes.push_back(std::byte{0});
      append_u32(bytes, id);
      append_u32(bytes, type);
      append_u32(bytes, source);
      append_u32(bytes, target);
      append_fixed(bytes, "", 39);
      bytes.push_back(std::byte{0});
      append_u32(bytes, 2);
      append_u32(bytes, id + 100U);
      append_u32(bytes, id + 200U);
    };
    if (assembly_fixture) {
      append_relation(800U, 7U, 1201U, 1203U);
      append_relation(801U, 11U, 1201U, 700U);
      append_relation(802U, 12U, 1203U, 1202U);
      append_relation(803U, 11U, 1201U, 1202U);
      append_relation(804U, 47U, 1201U, 1204U);
      if (schema.internal_format == "9.52" || schema.internal_format == "9.66") {
        append_relation(805U, 11U, 1201U, 1205U);
        append_relation(806U, 12U, 1201U, 1206U);
      }
    } else {
      append_relation(800U,
                      edge_chamfer        ? 79U
                      : boolean_operative ? 11U
                                          : 9U,
                      boolean_operative ? 700U : 1201U,
                      edge_chamfer        ? 990U
                      : boolean_operative ? 1201U
                                          : 700U);
    }
  } else if ((component_fixture || report_fixture) && table.name == "string") {
    const auto append_string = [&](std::uint32_t id, std::uint32_t next, std::string_view value) {
      bytes.push_back(std::byte{0});
      const auto tuple_offset = bytes.size();
      bytes.resize(bytes.size() + table.tuple_size, std::byte{0});
      auto tuple = std::span<std::byte>(bytes).subspan(tuple_offset, table.tuple_size);
      for (const auto& field : schema.table_fields(table)) {
        if (field.name == "id") write_u32(tuple, field.offset, id);
        if (field.name == "next_id") write_u32(tuple, field.offset, next);
        if (field.name == "string") write_fixed(tuple, field.offset, field.size, value);
      }
      append_u32(bytes, id + 100U);
      append_u32(bytes, id + 200U);
    };
    if (report_fixture) {
      append_string(3000U, 3001U, "2");
      append_string(3001U, 0U, "00");
    } else {
      append_string(3000U, 0U, "Fixture joint");
      append_string(3001U, 0U, "Fixture macro");
      append_string(3002U, 0U, "Joint description");
      append_string(3003U, 0U, "Macro description");
    }
  } else if ((component_fixture || assembly_fixture) && table.name == "joint") {
    bytes.push_back(std::byte{0});
    const auto tuple_offset = bytes.size();
    bytes.resize(bytes.size() + table.tuple_size, std::byte{0});
    auto tuple = std::span<std::byte>(bytes).subspan(tuple_offset, table.tuple_size);
    for (const auto& field : schema.table_fields(table)) {
      if (field.name == "id") write_u32(tuple, field.offset, assembly_fixture ? 900U : 2000U);
      if (field.name == "obj_type") write_u32(tuple, field.offset, 4U);
      if (field.name == "joint_no") write_u32(tuple, field.offset, 123U);
      if (field.name == "name") write_u32(tuple, field.offset, component_fixture ? 3000U : 0U);
      if (field.name == "prim") write_u32(tuple, field.offset, assembly_fixture ? 1202U : 1201U);
      if (field.name == "sek") write_u32(tuple, field.offset, assembly_fixture ? 1203U : 700U);
      if (field.name == "seknum") write_u32(tuple, field.offset, 1U);
      if (field.name == "desc") write_u32(tuple, field.offset, component_fixture ? 3002U : 0U);
    }
    append_u32(bytes, 2200U);
    append_u32(bytes, 2300U);
  } else if (component_fixture && table.name == "macro") {
    bytes.push_back(std::byte{0});
    const auto tuple_offset = bytes.size();
    bytes.resize(bytes.size() + table.tuple_size, std::byte{0});
    auto tuple = std::span<std::byte>(bytes).subspan(tuple_offset, table.tuple_size);
    for (const auto& field : schema.table_fields(table)) {
      if (field.name == "id") write_u32(tuple, field.offset, 2100U);
      if (field.name == "obj_type") write_u32(tuple, field.offset, 4U);
      if (field.name == "type") write_u32(tuple, field.offset, 42U);
      if (field.name == "number") write_u32(tuple, field.offset, 77U);
      if (field.name == "name") write_u32(tuple, field.offset, 3001U);
      if (field.name == "desc") write_u32(tuple, field.offset, 3003U);
    }
    append_u32(bytes, 2300U);
    append_u32(bytes, 2400U);
  } else if (weld_fixture && table.name == "welding") {
    const auto append_weld = [&](std::uint32_t id, std::uint32_t common_id) {
      bytes.push_back(std::byte{0});
      const auto tuple_offset = bytes.size();
      bytes.resize(bytes.size() + table.tuple_size, std::byte{0});
      auto tuple = std::span<std::byte>(bytes).subspan(tuple_offset, table.tuple_size);
      for (const auto& field : schema.table_fields(table)) {
        if (field.name == "id") write_u32(tuple, field.offset, id);
        if (field.name == "weld_common_attr_id") write_u32(tuple, field.offset, common_id);
        if (field.name == "weld_seam1_id") write_u32(tuple, field.offset, 902U);
        if (field.name == "weld_seam2_id") write_u32(tuple, field.offset, 903U);
      }
      append_u32(bytes, id + 1300U);
      append_u32(bytes, id + 1400U);
    };
    append_weld(1201U, 901U);
    append_weld(1202U, 904U);
  } else if (weld_fixture && table.name == "welding_common_attr") {
    const auto append_common = [&](std::uint32_t id, std::uint32_t workshop) {
      bytes.push_back(std::byte{0});
      const auto tuple_offset = bytes.size();
      bytes.resize(bytes.size() + table.tuple_size, std::byte{0});
      auto tuple = std::span<std::byte>(bytes).subspan(tuple_offset, table.tuple_size);
      for (const auto& field : schema.table_fields(table)) {
        if (field.name == "id") write_u32(tuple, field.offset, id);
        if (field.name == "around_weld") write_u32(tuple, field.offset, 1U);
        if (field.name == "workshop_weld") write_u32(tuple, field.offset, workshop);
        if (field.name == "compound_weld") write_u32(tuple, field.offset, 1U);
        if (field.name == "logical_weld") write_u32(tuple, field.offset, 0U);
        if (field.name == "intermittent_type") write_u32(tuple, field.offset, 2U);
      }
      append_u32(bytes, id + 1800U);
      append_u32(bytes, id + 1900U);
    };
    append_common(901U, 1U);
    append_common(904U, 0U);
  } else if (weld_fixture && table.name == "welding_attr") {
    const auto append_weld_attribute = [&](std::uint32_t id, float size, std::uint32_t type,
                                           std::uint32_t intermittent) {
      bytes.push_back(std::byte{0});
      const auto tuple_offset = bytes.size();
      bytes.resize(bytes.size() + table.tuple_size, std::byte{0});
      auto tuple = std::span<std::byte>(bytes).subspan(tuple_offset, table.tuple_size);
      for (const auto& field : schema.table_fields(table)) {
        if (field.name == "id") write_u32(tuple, field.offset, id);
        if (field.name == "size")
          write_u32(tuple, field.offset, std::bit_cast<std::uint32_t>(size));
        if (field.name == "type") write_u32(tuple, field.offset, type);
        if (field.name == "intermittent") write_u32(tuple, field.offset, intermittent);
      }
      append_u32(bytes, id + 100U);
      append_u32(bytes, id + 200U);
    };
    append_weld_attribute(902U, 5.0F, 10U, 1U);
    append_weld_attribute(903U, 3.0F, 6U, 0U);
  } else if ((bolt_fixture || legacy_bolt_fixture) && table.name == "bolt") {
    bytes.push_back(std::byte{0});
    const auto tuple_offset = bytes.size();
    bytes.resize(bytes.size() + table.tuple_size, std::byte{0});
    auto tuple = std::span<std::byte>(bytes).subspan(tuple_offset, table.tuple_size);
    for (const auto& field : schema.table_fields(table)) {
      if (field.name == "id") write_u32(tuple, field.offset, 1201U);
      if (field.name == "bolt_attr_id" || field.name == "attr_id")
        write_u32(tuple, field.offset, 900U);
      if (field.name == "polygon_id") write_u32(tuple, field.offset, 960U);
    }
    append_u32(bytes, 1202U);
    append_u32(bytes, 1203U);
  } else if (bolt_fixture && table.name == "bolt_attr") {
    bytes.push_back(std::byte{0});
    const auto tuple_offset = bytes.size();
    bytes.resize(bytes.size() + table.tuple_size, std::byte{0});
    auto tuple = std::span<std::byte>(bytes).subspan(tuple_offset, table.tuple_size);
    for (const auto& field : schema.table_fields(table)) {
      if (field.name == "id") write_u32(tuple, field.offset, 900U);
      if (field.name == "mat") write_fixed(tuple, field.offset, field.size, "FISCHER FAZ");
      if (field.name == "npoints") write_u32(tuple, field.offset, 4U);
      if (field.name == "BoltDiameter")
        write_u32(tuple, field.offset, std::bit_cast<std::uint32_t>(16.0F));
      if (field.name == "BoltLength")
        write_u32(tuple, field.offset, std::bit_cast<std::uint32_t>(50.0F));
    }
    append_u32(bytes, 901U);
    append_u32(bytes, 902U);
  } else if (legacy_bolt_fixture && table.name == "old_bolt_attr_897") {
    bytes.push_back(std::byte{0});
    const auto tuple_offset = bytes.size();
    bytes.resize(bytes.size() + table.tuple_size, std::byte{0});
    auto tuple = std::span<std::byte>(bytes).subspan(tuple_offset, table.tuple_size);
    for (const auto& field : schema.table_fields(table)) {
      if (field.name == "id") write_u32(tuple, field.offset, 900U);
      if (field.name == "mat") write_fixed(tuple, field.offset, field.size, "UNDEFINED_STUD");
      if (field.name == "BoltStructure") write_u32(tuple, field.offset, 110U);
      if (field.name == "BoltDiameter" || field.name == "BoltLength") {
        const double value = field.name == "BoltDiameter" ? 20.0 : 175.0;
        if (field.type == tekla::db1::detail::FieldType::f32) {
          write_u32(tuple, field.offset, std::bit_cast<std::uint32_t>(static_cast<float>(value)));
        } else {
          write_f64(tuple, field.offset, value);
        }
      }
    }
    append_u32(bytes, 901U);
    append_u32(bytes, 902U);
  } else if ((bolt_fixture && table.name == "partpolygon") ||
             (legacy_bolt_fixture && table.name == "old_partpolygon_898")) {
    bytes.push_back(std::byte{0});
    const auto tuple_offset = bytes.size();
    bytes.resize(bytes.size() + table.tuple_size, std::byte{0});
    auto tuple = std::span<std::byte>(bytes).subspan(tuple_offset, table.tuple_size);
    for (const auto& field : schema.table_fields(table)) {
      if (field.name == "id") write_u32(tuple, field.offset, 960U);
      for (std::size_t index = 1U; index <= 10U; ++index) {
        if (field.name == "types" + std::to_string(index)) {
          write_u32(tuple, field.offset, index <= 4U ? 0U : 2'147'483'647U);
        }
      }
    }
    append_u32(bytes, 961U);
    append_u32(bytes, 962U);
  } else if (table.name == "part" && !bolt_fixture && !legacy_bolt_fixture) {
    bytes.push_back(std::byte{0});
    const auto tuple_offset = bytes.size();
    bytes.resize(bytes.size() + table.tuple_size, std::byte{0});
    auto tuple = std::span<std::byte>(bytes).subspan(tuple_offset, table.tuple_size);
    for (const auto& field : schema.table_fields(table)) {
      if (field.name == "id") write_u32(tuple, field.offset, 1201);
      if (field.name == "part_attr_id") write_u32(tuple, field.offset, 900);
      if (field.name == "csys_attr_id") write_u32(tuple, field.offset, 901);
      if (field.name == "p1") write_u32(tuple, field.offset, polybeam ? 970U : 0U);
      if (field.name == "GeometryTreeRootId") {
        write_u32(tuple, field.offset, contour || polybeam || lofted ? 950U : 0U);
      }
      if (field.name == "csys_x") {
        write_f64(tuple, field.offset, edge_chamfer ? 500000.0 : 10.0);
      }
      if (field.name == "csys_y") {
        write_f64(tuple, field.offset, edge_chamfer ? 500000.0 : 20.0);
      }
      if (field.name == "csys_z") {
        write_f64(tuple, field.offset, edge_chamfer ? 500000.0 : 30.0);
      }
      if (field.name == "csys_length") write_f64(tuple, field.offset, 1000.0);
    }
    append_u32(bytes, 83);
    append_u32(bytes, 84);
  } else if (table.name == "part_attr" || table.name == "old_part_attr_911") {
    bytes.push_back(std::byte{0});
    const auto tuple_offset = bytes.size();
    bytes.resize(bytes.size() + table.tuple_size, std::byte{0});
    auto tuple = std::span<std::byte>(bytes).subspan(tuple_offset, table.tuple_size);
    for (const auto& field : schema.table_fields(table)) {
      if (field.name == "id") write_u32(tuple, field.offset, 900);
      if (field.name == "form_type") write_u32(tuple, field.offset, form_type);
      if (field.name == "obj_type") write_u32(tuple, field.offset, 2);
      if (field.name == "ben") write_fixed(tuple, field.offset, field.size, "Main beam");
      if (field.name == "prof" || field.name == "Geometry") {
        write_fixed(tuple, field.offset, field.size, profile);
      }
      if (field.name == "ParameterStringId" && report_fixture) {
        write_u32(tuple, field.offset, 3000U);
      }
      if (field.name == "mat") {
        write_fixed(tuple, field.offset, field.size, report_fixture ? "C35/45" : "S355");
      }
      if (field.name == "finish") write_fixed(tuple, field.offset, field.size, "PAINT");
      if (field.name == "ryhma") {
        write_fixed(tuple, field.offset, field.size, object_class);
      }
    }
    append_u32(bytes, 85);
    append_u32(bytes, 86);
  } else if ((report_fixture || assembly_fixture) && table.name == "assembly") {
    bytes.push_back(std::byte{0});
    const auto tuple_offset = bytes.size();
    bytes.resize(bytes.size() + table.tuple_size, std::byte{0});
    auto tuple = std::span<std::byte>(bytes).subspan(tuple_offset, table.tuple_size);
    for (const auto& field : schema.table_fields(table)) {
      if (field.name == "id") write_u32(tuple, field.offset, 700U);
      if (field.name == "dum") write_u32(tuple, field.offset, 1201U);
      if (field.name == "flags") write_u32(tuple, field.offset, 1U);
    }
    append_u32(bytes, 87U);
    append_u32(bytes, 88U);
  } else if (report_fixture && table.name == "object_phase") {
    bytes.push_back(std::byte{0});
    const auto tuple_offset = bytes.size();
    bytes.resize(bytes.size() + table.tuple_size, std::byte{0});
    auto tuple = std::span<std::byte>(bytes).subspan(tuple_offset, table.tuple_size);
    for (const auto& field : schema.table_fields(table)) {
      if (field.name == "object_id") write_u32(tuple, field.offset, 1201U);
      if (field.name == "phase_number") write_u32(tuple, field.offset, 24U);
    }
    append_u32(bytes, 89U);
    append_u32(bytes, 90U);
  } else if (report_fixture && table.name == "object_numbering") {
    const auto append_numbering_link = [&](std::uint32_t object_id, std::uint32_t group_id) {
      bytes.push_back(std::byte{0});
      const auto tuple_offset = bytes.size();
      bytes.resize(bytes.size() + table.tuple_size, std::byte{0});
      auto tuple = std::span<std::byte>(bytes).subspan(tuple_offset, table.tuple_size);
      for (const auto& field : schema.table_fields(table)) {
        if (field.name == "id") write_u32(tuple, field.offset, object_id);
        if (field.name == "numbering_group") write_u32(tuple, field.offset, group_id);
      }
      append_u32(bytes, object_id + 10U);
      append_u32(bytes, object_id + 20U);
    };
    append_numbering_link(1201U, 5000U);
    append_numbering_link(700U, 6000U);
  } else if (report_fixture && table.name == "part_numbering") {
    bytes.push_back(std::byte{0});
    const auto tuple_offset = bytes.size();
    bytes.resize(bytes.size() + table.tuple_size, std::byte{0});
    auto tuple = std::span<std::byte>(bytes).subspan(tuple_offset, table.tuple_size);
    for (const auto& field : schema.table_fields(table)) {
      if (field.name == "id") write_u32(tuple, field.offset, 5000U);
      if (field.name == "start_no") write_u32(tuple, field.offset, 1U);
      if (field.name == "fpos") write_u32(tuple, field.offset, 35U);
      if (field.name == "pos") write_fixed(tuple, field.offset, field.size, "Beton");
    }
    append_u32(bytes, 91U);
    append_u32(bytes, 92U);
  } else if (report_fixture && table.name == "assembly_numbering") {
    bytes.push_back(std::byte{0});
    const auto tuple_offset = bytes.size();
    bytes.resize(bytes.size() + table.tuple_size, std::byte{0});
    auto tuple = std::span<std::byte>(bytes).subspan(tuple_offset, table.tuple_size);
    for (const auto& field : schema.table_fields(table)) {
      if (field.name == "id") write_u32(tuple, field.offset, 6000U);
      if (field.name == "start_no") {
        write_u32(tuple, field.offset, zero_position_report_fixture ? 1U : 90781U);
      }
      if (field.name == "fpos") {
        write_u32(tuple, field.offset, zero_position_report_fixture ? 0U : 6U);
      }
      if (field.name == "pos" && zero_position_report_fixture) {
        write_fixed(tuple, field.offset, field.size, "A9");
      }
    }
    append_u32(bytes, 93U);
    append_u32(bytes, 94U);
  } else if (table.name == "coordsys_attr") {
    const auto append_axes = [&](std::uint32_t id, tekla::db1::Vector3d x, tekla::db1::Vector3d y) {
      bytes.push_back(std::byte{0});
      const auto tuple_offset = bytes.size();
      bytes.resize(bytes.size() + table.tuple_size, std::byte{0});
      auto tuple = std::span<std::byte>(bytes).subspan(tuple_offset, table.tuple_size);
      for (const auto& field : schema.table_fields(table)) {
        if (field.name == "id") write_u32(tuple, field.offset, id);
        if (field.name == "xdir_x") write_f64(tuple, field.offset, x.x);
        if (field.name == "xdir_y") write_f64(tuple, field.offset, x.y);
        if (field.name == "xdir_z") write_f64(tuple, field.offset, x.z);
        if (field.name == "ydir_x") write_f64(tuple, field.offset, y.x);
        if (field.name == "ydir_y") write_f64(tuple, field.offset, y.y);
        if (field.name == "ydir_z") write_f64(tuple, field.offset, y.z);
      }
      append_u32(bytes, id + 100U);
      append_u32(bytes, id + 101U);
    };
    const auto x_axis = part_frame == PartFrameFixture::rotated
                            ? tekla::db1::Vector3d{0.0, 1.0, 0.0}
                            : tekla::db1::Vector3d{1.0, 0.0, 0.0};
    const auto y_axis =
        zero_transverse_axis                      ? tekla::db1::Vector3d{}
        : part_frame == PartFrameFixture::skewed  ? tekla::db1::Vector3d{0.5, 1.0, 0.0}
        : part_frame == PartFrameFixture::rotated ? tekla::db1::Vector3d{-1.0, 0.0, 0.0}
                                                  : tekla::db1::Vector3d{0.0, 1.0, 0.0};
    append_axes(901U, x_axis, y_axis);
    if (edge_chamfer) {
      constexpr double diagonal = 0.7071067811865476;
      append_axes(991U, {0.0, 0.0, -1.0}, {diagonal, diagonal, 0.0});
    }
  } else if ((edge_chamfer || bolt_fixture || legacy_bolt_fixture) && table.name == "coordsys") {
    bytes.push_back(std::byte{0});
    const auto tuple_offset = bytes.size();
    bytes.resize(bytes.size() + table.tuple_size, std::byte{0});
    auto tuple = std::span<std::byte>(bytes).subspan(tuple_offset, table.tuple_size);
    for (const auto& field : schema.table_fields(table)) {
      if (field.name == "id") write_u32(tuple, field.offset, edge_chamfer ? 990U : 1201U);
      if (field.name == "csys_attr_id") write_u32(tuple, field.offset, edge_chamfer ? 991U : 901U);
      if (field.name == "x1") write_f64(tuple, field.offset, edge_chamfer ? 501000.0 : 0.0);
      if (field.name == "y1") write_f64(tuple, field.offset, edge_chamfer ? 500050.0 : 0.0);
      if (field.name == "z1") write_f64(tuple, field.offset, edge_chamfer ? 500100.0 : 0.0);
      if (field.name == "length") write_f64(tuple, field.offset, edge_chamfer ? 200.0 : 50.0);
    }
    append_u32(bytes, 1090U);
    append_u32(bytes, 1091U);
  } else if ((bolt_fixture || legacy_bolt_fixture) && table.name == "bolt_hole_points") {
    bytes.push_back(std::byte{0});
    const auto tuple_offset = bytes.size();
    bytes.resize(bytes.size() + table.tuple_size, std::byte{0});
    auto tuple = std::span<std::byte>(bytes).subspan(tuple_offset, table.tuple_size);
    for (const auto& field : schema.table_fields(table)) {
      if (field.name == "bolt_id" || field.name == "id") write_u32(tuple, field.offset, 1201U);
      if (field.name == "int_point_1_z") write_f64(tuple, field.offset, -30.0);
      if (field.name == "int_point_2_z") write_f64(tuple, field.offset, 0.0);
    }
    append_u32(bytes, 1190U);
    append_u32(bytes, 1191U);
  } else if (edge_chamfer && table.name == "chamfer") {
    bytes.push_back(std::byte{0});
    const auto tuple_offset = bytes.size();
    bytes.resize(bytes.size() + table.tuple_size, std::byte{0});
    auto tuple = std::span<std::byte>(bytes).subspan(tuple_offset, table.tuple_size);
    for (const auto& field : schema.table_fields(table)) {
      if (field.name == "id") write_u32(tuple, field.offset, 990U);
      if (field.name == "type") write_u32(tuple, field.offset, 1U);
      if (field.name == "x" || field.name == "y") {
        write_f64(tuple, field.offset, 25.0);
      }
      if (field.name == "endtypes") write_u32(tuple, field.offset, 12U);
    }
    append_u32(bytes, 1092U);
    append_u32(bytes, 1093U);
  } else if (polybeam && table.name == "point") {
    bytes.push_back(std::byte{0});
    const auto tuple_offset = bytes.size();
    bytes.resize(bytes.size() + table.tuple_size, std::byte{0});
    auto tuple = std::span<std::byte>(bytes).subspan(tuple_offset, table.tuple_size);
    for (const auto& field : schema.table_fields(table)) {
      if (field.name == "id") write_u32(tuple, field.offset, 970);
      if (field.name == "x") write_f64(tuple, field.offset, 100.0);
      if (field.name == "y" || field.name == "z") write_f64(tuple, field.offset, 0.0);
    }
    append_u32(bytes, 89);
    append_u32(bytes, 90);
  } else if (lofted && table.name == "geometry_tree_node") {
    const auto append_node = [&](std::uint32_t id, std::uint32_t parent, std::uint32_t type,
                                 std::uint32_t subtype, std::uint32_t geometry) {
      bytes.push_back(std::byte{0});
      const auto tuple_offset = bytes.size();
      bytes.resize(bytes.size() + table.tuple_size, std::byte{0});
      auto tuple = std::span<std::byte>(bytes).subspan(tuple_offset, table.tuple_size);
      for (const auto& field : schema.table_fields(table)) {
        if (field.name == "Id") write_u32(tuple, field.offset, id);
        if (field.name == "ParentId") write_u32(tuple, field.offset, parent);
        if (field.name == "Type") write_u32(tuple, field.offset, type);
        if (field.name == "SubType") write_u32(tuple, field.offset, subtype);
        if (field.name == "GeometryId") write_u32(tuple, field.offset, geometry);
      }
      append_u32(bytes, id + 100U);
      append_u32(bytes, id + 101U);
    };
    append_node(950U, 0U, 1000U, 1000U, 0U);
    append_node(951U, 950U, 1000U, 1001U, 0U);
    append_node(952U, 950U, 1000U, 1002U, 0U);
    append_node(953U, 951U, 6U, 0U, 980U);
    append_node(954U, 951U, 6U, 0U, 981U);
  } else if (lofted && table.name == "double_array") {
    const auto append_array = [&](std::uint32_t id, std::array<double, 6> values) {
      bytes.push_back(std::byte{0});
      const auto tuple_offset = bytes.size();
      bytes.resize(bytes.size() + table.tuple_size, std::byte{0});
      auto tuple = std::span<std::byte>(bytes).subspan(tuple_offset, table.tuple_size);
      for (const auto& field : schema.table_fields(table)) {
        if (field.name == "id") write_u32(tuple, field.offset, id);
        if (field.name == "n_values") write_u32(tuple, field.offset, 6U);
        for (std::size_t index = 0; index < values.size(); ++index) {
          if (field.name == "value_" + std::to_string(index)) {
            write_f64(tuple, field.offset, values[index]);
          }
        }
      }
      append_u32(bytes, id + 100U);
      append_u32(bytes, id + 101U);
    };
    append_array(980U, {-10.0, -20.0, -30.0, 990.0, -20.0, -30.0});
    append_array(981U, {-10.0, 80.0, -30.0, 990.0, 80.0, -30.0});
  } else if ((contour || polybeam) && table.name == "geometry_tree_node") {
    bytes.push_back(std::byte{0});
    const auto tuple_offset = bytes.size();
    bytes.resize(bytes.size() + table.tuple_size, std::byte{0});
    auto tuple = std::span<std::byte>(bytes).subspan(tuple_offset, table.tuple_size);
    for (const auto& field : schema.table_fields(table)) {
      if (field.name == "Id") write_u32(tuple, field.offset, 950);
      if (field.name == "Type") write_u32(tuple, field.offset, polybeam ? 3U : 1U);
      if (field.name == "GeometryId") write_u32(tuple, field.offset, 960);
    }
    append_u32(bytes, 95);
    append_u32(bytes, 96);
  } else if ((contour || polybeam) &&
             (table.name == "partpolygon" || table.name == "old_partpolygon_898")) {
    bytes.push_back(std::byte{0});
    const auto tuple_offset = bytes.size();
    bytes.resize(bytes.size() + table.tuple_size, std::byte{0});
    auto tuple = std::span<std::byte>(bytes).subspan(tuple_offset, table.tuple_size);
    constexpr std::array<double, 4> contour_x{0.0, 300.0, 300.0, 0.0};
    constexpr std::array<double, 4> contour_y{0.0, 0.0, 300.0, 300.0};
    constexpr std::array<double, 5> arc_x{0.0, 150.0, 300.0, 300.0, 0.0};
    constexpr std::array<double, 5> arc_y{0.0, -100.0, 0.0, 300.0, 300.0};
    constexpr std::array<double, 5> mixed_x{0.0, 300.0, 400.0, 300.0, 0.0};
    constexpr std::array<double, 5> mixed_y{0.0, 0.0, 150.0, 300.0, 300.0};
    constexpr std::array<double, 3> path_x{0.0, 500.0, 1000.0};
    constexpr std::array<double, 3> path_y{0.0, 500.0, 0.0};
    const auto point_count = polybeam        ? path_x.size()
                             : mixed_contour ? mixed_x.size()
                             : arc_contour   ? arc_x.size()
                                             : contour_x.size();
    for (const auto& field : schema.table_fields(table)) {
      if (field.name == "id") write_u32(tuple, field.offset, 960);
      for (std::size_t index = 0; index < 10; ++index) {
        const auto suffix = std::to_string(index + 1U);
        const auto write_scalar = [&](double value) {
          if (field.type == tekla::db1::detail::FieldType::f32) {
            write_u32(tuple, field.offset, std::bit_cast<std::uint32_t>(static_cast<float>(value)));
          } else {
            write_f64(tuple, field.offset, value);
          }
        };
        if (field.name == "x" + suffix) {
          write_scalar(index < point_count ? polybeam        ? path_x[index]
                                             : mixed_contour ? mixed_x[index]
                                             : arc_contour   ? arc_x[index]
                                                             : contour_x[index]
                                           : 0.0);
        }
        if (field.name == "y" + suffix) {
          write_scalar(index < point_count ? polybeam        ? path_y[index]
                                             : mixed_contour ? mixed_y[index]
                                             : arc_contour   ? arc_y[index]
                                                             : contour_y[index]
                                           : 0.0);
        }
        if (field.name == "z" + suffix) write_scalar(0.0);
        if (field.name == "dx" + suffix || field.name == "dy" + suffix) {
          write_scalar(mixed_contour && index == 0U                                       ? 50.0
                       : contour && !arc_contour && !mixed_contour && index < point_count ? 150.0
                                                                                          : 0.0);
        }
        if (field.name == "types" + suffix) {
          const auto corner_type = mixed_contour && index == 0U   ? 20U
                                   : mixed_contour && index == 2U ? 40U
                                   : arc_contour && index == 1U   ? 40U
                                   : contour && !arc_contour      ? 20U
                                                                  : 0U;
          write_u32(tuple, field.offset, index < point_count ? corner_type : 2'147'483'647U);
        }
      }
    }
    append_u32(bytes, 97);
    append_u32(bytes, 98);
  } else if (table.name == "old_object_attr_900" ||
             (assembly_fixture &&
              (table.name == "old_object_attr_951" || table.name == "old_object_attr_915" ||
               table.name == "old_object_attr_879"))) {
    if (assembly_fixture) {
      const auto append_attribute = [&](std::uint32_t id, std::uint32_t type,
                                        std::uint32_t subtype = 0U) {
        bytes.push_back(std::byte{0});
        const auto tuple_offset = bytes.size();
        bytes.resize(bytes.size() + table.tuple_size, std::byte{0});
        auto tuple = std::span<std::byte>(bytes).subspan(tuple_offset, table.tuple_size);
        for (const auto& field : schema.table_fields(table)) {
          if (field.name == "id") write_u32(tuple, field.offset, id);
          if (field.name == "type") write_u32(tuple, field.offset, type);
          if (field.name == "subtype") write_u32(tuple, field.offset, subtype);
          if (field.name == "obj_flag") write_u32(tuple, field.offset, 5U);
        }
        append_u32(bytes, id + 100U);
        append_u32(bytes, id + 200U);
      };
      append_attribute(77U, 2U);
      append_attribute(78U, 15U);
      append_attribute(79U, 47U);
    } else {
      bytes.push_back(std::byte{0});
      append_u32(bytes, 77);
      append_u32(bytes, legacy_bolt_fixture ? 10U : 2U);
      append_u32(bytes, legacy_bolt_fixture ? 1U : 23U);
      append_u32(bytes, 0);
      append_u32(bytes, 5);
      append_u32(bytes, 6);
      append_u32(bytes, 7);
      append_u32(bytes, 81);
      append_u32(bytes, 82);
    }
  } else if (table.name == "old_object_948") {
    if (assembly_fixture) {
      const auto append_object = [&](std::uint32_t id, std::uint32_t attribute,
                                     std::uint32_t parent, std::uint32_t assembly,
                                     std::string_view guid) {
        bytes.push_back(std::byte{0});
        const auto tuple_offset = bytes.size();
        bytes.resize(bytes.size() + table.tuple_size, std::byte{0});
        auto tuple = std::span<std::byte>(bytes).subspan(tuple_offset, table.tuple_size);
        for (const auto& field : schema.table_fields(table)) {
          if (field.name == "id") write_u32(tuple, field.offset, id);
          if (field.name == "object_attr_id") write_u32(tuple, field.offset, attribute);
          if (field.name == "kuuluu") write_u32(tuple, field.offset, parent);
          if (field.name == "assembly") write_u32(tuple, field.offset, assembly);
          if (field.name == "guid") write_fixed(tuple, field.offset, field.size, guid);
        }
        append_u32(bytes, id + 100U);
        append_u32(bytes, id + 200U);
      };
      append_object(1201U, 77U, 0U, 700U, "{00000001-0000-4000-8000-000000000001}");
      append_object(1202U, 77U, 1201U, 700U, "{00000002-0000-4000-8000-000000000002}");
      append_object(1203U, 77U, 0U, 700U, "{00000003-0000-4000-8000-000000000003}");
      append_object(1204U, 79U, 0U, 0U, "{00000005-0000-4000-8000-000000000005}");
      append_object(700U, 78U, 0U, 700U, "{00000004-0000-4000-8000-000000000004}");
    } else {
      bytes.push_back(std::byte{0});
      append_u32(bytes, legacy_bolt_fixture ? 1201U : 1202U);
      append_u32(bytes, 77);
      append_u32(bytes, 0);
      append_u32(bytes, 701);
      append_ascii(bytes, "{87654321-4321-4cba-9fed-cba987654321}");
      bytes.push_back(std::byte{0});
      append_u32(bytes, 93);
      append_u32(bytes, 94);
    }
  }

  bytes.push_back(std::byte{0});
  if (final) {
    bytes.insert(bytes.end(), final_footer.begin(), final_footer.end());
  } else if (keyed_footer && table.has_table_key) {
    append_u32(bytes, table.table_key);
    bytes.insert(bytes.end(), table_end.begin(), table_end.end());
  } else {
    bytes.insert(bytes.end(), table_end.begin(), table_end.end());
  }
}

std::vector<std::byte> database_with_one_object(
    std::string_view profile = "200*10", std::string_view object_class = "14",
    std::string_view format = "9.66", bool boolean_operative = false, std::uint32_t form_type = 7,
    bool contour = false, bool polybeam = false, bool lofted = false, bool edge_chamfer = false,
    bool arc_contour = false, bool component_fixture = false, bool keyed_tables = false,
    bool weld_fixture = false, bool zero_transverse_axis = false,
    bool duplicate_property_fixture = false, bool report_fixture = false,
    bool zero_position_report_fixture = false, bool bolt_fixture = false,
    bool legacy_bolt_fixture = false, bool assembly_fixture = false,
    PartFrameFixture part_frame = PartFrameFixture::orthogonal, bool mixed_contour = false) {
  constexpr std::array<std::byte, 4> table_end{std::byte{0x66}, std::byte{0xc0}, std::byte{0xce},
                                               std::byte{0xdb}};
  const auto* schema = tekla::db1::detail::schema_for(format, 0x85);
  CHECK(schema != nullptr, "the requested generated schema is registered");
  std::vector<std::byte> bytes;
  append_ascii(bytes, "Xsteel");
  bytes.push_back(std::byte{0x85});
  bytes.push_back(std::byte{' '});
  const auto* format_first = reinterpret_cast<const std::byte*>(format.data());
  bytes.insert(bytes.end(), format_first, format_first + format.size());
  append_ascii(bytes, " 7d72d8c9-0250-4f3a-8760-bcef517f016e");
  append_u32(bytes, 1);
  bytes.insert(bytes.end(), table_end.begin(), table_end.end());
  if (schema != nullptr) {
    for (std::size_t index = 0; index < schema->tables.size(); ++index) {
      append_table(bytes, *schema, schema->tables[index], index + 1 == schema->tables.size(),
                   profile, object_class, boolean_operative, form_type, contour, polybeam, lofted,
                   edge_chamfer, arc_contour, component_fixture, keyed_tables, weld_fixture,
                   zero_transverse_axis, duplicate_property_fixture, report_fixture,
                   zero_position_report_fixture, bolt_fixture, legacy_bolt_fixture,
                   assembly_fixture, part_frame, mixed_contour);
    }
  }
  return bytes;
}

void append_polygon_weld_table(std::vector<std::byte>& bytes,
                               const tekla::db1::detail::Schema& schema,
                               const tekla::db1::detail::TableSchema& table, bool final,
                               bool malformed_polygon, bool semantic_only,
                               PolygonWeldFrameFixture frame_fixture, std::size_t polygon_count,
                               std::size_t polygon_row_count) {
  constexpr std::array<std::byte, 4> table_end{std::byte{0x66}, std::byte{0xc0}, std::byte{0xce},
                                               std::byte{0xdb}};
  constexpr std::array<std::byte, 4> final_footer{std::byte{0x4f}, std::byte{0x61}, std::byte{0xbc},
                                                  std::byte{0x00}};
  append_u32(bytes, table.tuple_size);
  append_u32(bytes, table.descriptor_count);
  for (const auto descriptor : schema.table_descriptors(table)) append_u32(bytes, descriptor);

  const auto append_tuple = [&](const auto& write) {
    bytes.push_back(std::byte{0});
    const auto tuple_offset = bytes.size();
    bytes.resize(bytes.size() + table.tuple_size, std::byte{0});
    auto tuple = std::span<std::byte>(bytes).subspan(tuple_offset, table.tuple_size);
    write(tuple);
    append_u32(bytes, 10'001U);
    append_u32(bytes, 20'001U);
  };
  const auto write_scalar = [](std::span<std::byte> tuple,
                               const tekla::db1::detail::FieldSchema& field, double value) {
    if (field.type == tekla::db1::detail::FieldType::f32) {
      write_u32(tuple, field.offset, std::bit_cast<std::uint32_t>(static_cast<float>(value)));
    } else {
      write_f64(tuple, field.offset, value);
    }
  };

  if (table.name == "object") {
    append_tuple([&](std::span<std::byte> tuple) {
      for (const auto& field : schema.table_fields(table)) {
        if (field.name == "id") write_u32(tuple, field.offset, 1201U);
        if (field.name == "type") write_u32(tuple, field.offset, 13U);
        if (field.name == "guid") {
          for (std::size_t index = 0; index < field.size; ++index) {
            tuple[field.offset + index] = static_cast<std::byte>(0x40U + index);
          }
        }
      }
    });
  } else if (table.name == "coordsys_attr") {
    append_tuple([&](std::span<std::byte> tuple) {
      for (const auto& field : schema.table_fields(table)) {
        if (field.name == "id") write_u32(tuple, field.offset, 901U);
        if (field.name == "xdir_x") {
          write_scalar(tuple, field,
                       frame_fixture == PolygonWeldFrameFixture::collinear_model_basis ? 1.0 : 0.0);
        }
        if (field.name == "xdir_y") {
          write_scalar(tuple, field,
                       frame_fixture == PolygonWeldFrameFixture::collinear_model_basis ? 0.0 : 1.0);
        }
        if (field.name == "xdir_z") write_scalar(tuple, field, 0.0);
        if (field.name == "ydir_x") {
          write_scalar(
              tuple, field,
              frame_fixture == PolygonWeldFrameFixture::collinear_model_basis ? 1.0 : -1.0);
        }
        if (field.name == "ydir_y") write_scalar(tuple, field, 0.0);
        if (field.name == "ydir_z") write_scalar(tuple, field, 0.0);
      }
    });
  } else if (table.name == "coordsys") {
    append_tuple([&](std::span<std::byte> tuple) {
      for (const auto& field : schema.table_fields(table)) {
        if (field.name == "id") write_u32(tuple, field.offset, 1201U);
        if (field.name == "csys_attr_id") write_u32(tuple, field.offset, 901U);
        const double origin =
            frame_fixture == PolygonWeldFrameFixture::large_model_origin ? 1.0e9 : 0.0;
        if (field.name == "x1") write_scalar(tuple, field, origin + 100.0);
        if (field.name == "y1") write_scalar(tuple, field, origin + 200.0);
        if (field.name == "z1") write_scalar(tuple, field, origin + 300.0);
      }
    });
  } else if (table.name == "part_attr") {
    append_tuple([&](std::span<std::byte> tuple) {
      for (const auto& field : schema.table_fields(table)) {
        if (field.name == "id") write_u32(tuple, field.offset, 999U);
        if (field.name == "prof") write_fixed(tuple, field.offset, field.size, "200*10");
      }
    });
  } else if (table.name == "welding") {
    append_tuple([&](std::span<std::byte> tuple) {
      for (const auto& field : schema.table_fields(table)) {
        if (field.name == "id") write_u32(tuple, field.offset, 1201U);
        if (field.name == "weld_common_attr_id") write_u32(tuple, field.offset, 902U);
        if (field.name == "weld_seam1_id") write_u32(tuple, field.offset, 903U);
      }
    });
  } else if (table.name == "welding_common_attr") {
    append_tuple([&](std::span<std::byte> tuple) {
      for (const auto& field : schema.table_fields(table)) {
        if (field.name == "id") write_u32(tuple, field.offset, 902U);
        if (field.name == "workshop_weld") write_u32(tuple, field.offset, 1U);
        if (field.name == "compound_weld" || field.name == "logical_weld") {
          write_u32(tuple, field.offset, semantic_only ? 1U : 0U);
        }
      }
    });
  } else if (table.name == "welding_attr") {
    append_tuple([&](std::span<std::byte> tuple) {
      for (const auto& field : schema.table_fields(table)) {
        if (field.name == "id") write_u32(tuple, field.offset, 903U);
        if (field.name == "size") write_scalar(tuple, field, 5.0);
        if (field.name == "type") write_u32(tuple, field.offset, 10U);
      }
    });
  } else if (table.name == "relation") {
    for (std::size_t polygon = 0U; polygon < polygon_count; ++polygon) {
      const auto polygon_id = static_cast<std::uint32_t>(950U + polygon);
      append_tuple([&](std::span<std::byte> tuple) {
        for (const auto& field : schema.table_fields(table)) {
          if (field.name == "id") write_u32(tuple, field.offset, polygon_id + 1U);
          if (field.name == "type") write_u32(tuple, field.offset, 40'001U);
          if (field.name == "id1") write_u32(tuple, field.offset, 1201U);
          if (field.name == "id2") write_u32(tuple, field.offset, polygon_id);
        }
      });
    }
  } else if (table.name == "weldingpolygon") {
    for (std::size_t polygon = 0U; polygon < polygon_count; ++polygon) {
      const auto polygon_id = static_cast<std::uint32_t>(950U + polygon);
      const auto value_count = polygon_row_count * 4U;
      const auto segment_count = (value_count - 1U) / 3U;
      std::vector<tekla::db1::Vector3d> path_values;
      path_values.reserve(segment_count * 3U + 1U);
      for (std::size_t segment = 0U; segment < segment_count; ++segment) {
        path_values.push_back(
            {static_cast<double>(polygon) * 20.0 + static_cast<double>(segment) * 10.0, 0.0, 0.0});
        path_values.push_back(frame_fixture == PolygonWeldFrameFixture::leg_parallel_to_tangent
                                  ? tekla::db1::Vector3d{1.0, 0.0, 0.0}
                              : frame_fixture ==
                                      PolygonWeldFrameFixture::leg_almost_parallel_to_tangent
                                  ? tekla::db1::Vector3d{1.0, 1.0e-8, 0.0}
                              : frame_fixture == PolygonWeldFrameFixture::noisy_tangent_component
                                  ? tekla::db1::Vector3d{4.6e-5, 1.0, 0.0}
                              : frame_fixture == PolygonWeldFrameFixture::reversed_handedness
                                  ? tekla::db1::Vector3d{0.0, 0.0, 1.0}
                                  : tekla::db1::Vector3d{0.0, 1.0, 0.0});
        path_values.push_back(frame_fixture == PolygonWeldFrameFixture::reversed_handedness
                                  ? tekla::db1::Vector3d{0.0, 1.0, 0.0}
                                  : tekla::db1::Vector3d{0.0, 0.0, 1.0});
      }
      path_values.push_back(
          {static_cast<double>(polygon) * 20.0 + static_cast<double>(segment_count) * 10.0, 0.0,
           0.0});
      CHECK(path_values.size() == value_count,
            "the polygon-weld row fixture forms complete segment triples");
      for (std::size_t row = 0U; row < polygon_row_count; ++row) {
        append_tuple([&](std::span<std::byte> tuple) {
          const auto points =
              std::span<const tekla::db1::Vector3d>(path_values).subspan(row * 4U, 4U);
          for (const auto& field : schema.table_fields(table)) {
            if (field.name == "id") write_u32(tuple, field.offset, polygon_id);
            if (field.name == "no") write_u32(tuple, field.offset, static_cast<std::uint32_t>(row));
            if (field.name == "number_of_points_in_row") {
              write_u32(tuple, field.offset, malformed_polygon ? 5U : 4U);
            }
            if (field.name == "type") write_u32(tuple, field.offset, 1U);
            for (std::size_t index = 0; index < points.size(); ++index) {
              const auto suffix = std::to_string(index + 1U);
              if (field.name == "x" + suffix) write_scalar(tuple, field, points[index].x);
              if (field.name == "y" + suffix) write_scalar(tuple, field, points[index].y);
              if (field.name == "z" + suffix) write_scalar(tuple, field, points[index].z);
            }
          }
        });
      }
    }
  }

  bytes.push_back(std::byte{0});
  if (final) {
    bytes.insert(bytes.end(), final_footer.begin(), final_footer.end());
  } else {
    bytes.insert(bytes.end(), table_end.begin(), table_end.end());
  }
}

std::vector<std::byte> database_with_polygon_weld_geometry(
    bool malformed_polygon = false, bool semantic_only = false,
    PolygonWeldFrameFixture frame_fixture = PolygonWeldFrameFixture::orthogonal,
    std::size_t polygon_count = 1U, std::size_t polygon_row_count = 1U) {
  constexpr std::array<std::byte, 4> table_end{std::byte{0x66}, std::byte{0xc0}, std::byte{0xce},
                                               std::byte{0xdb}};
  constexpr std::string_view format = "9.66";
  const auto* schema = tekla::db1::detail::schema_for(format, 0x85);
  CHECK(schema != nullptr, "the polygon-weld fixture schema is registered");
  std::vector<std::byte> bytes;
  append_ascii(bytes, "Xsteel");
  bytes.push_back(std::byte{0x85});
  bytes.push_back(std::byte{' '});
  const auto* format_first = reinterpret_cast<const std::byte*>(format.data());
  bytes.insert(bytes.end(), format_first, format_first + format.size());
  append_ascii(bytes, " 7d72d8c9-0250-4f3a-8760-bcef517f016e");
  append_u32(bytes, 1U);
  bytes.insert(bytes.end(), table_end.begin(), table_end.end());
  if (schema != nullptr) {
    for (std::size_t index = 0; index < schema->tables.size(); ++index) {
      append_polygon_weld_table(bytes, *schema, schema->tables[index],
                                index + 1U == schema->tables.size(), malformed_polygon,
                                semantic_only, frame_fixture, polygon_count, polygon_row_count);
    }
  }
  return bytes;
}

void append_surface_treatment_table(std::vector<std::byte>& bytes,
                                    const tekla::db1::detail::Schema& schema,
                                    const tekla::db1::detail::TableSchema& table, bool final) {
  constexpr std::array<std::byte, 4> table_end{std::byte{0x66}, std::byte{0xc0}, std::byte{0xce},
                                               std::byte{0xdb}};
  constexpr std::array<std::byte, 4> final_footer{std::byte{0x4f}, std::byte{0x61}, std::byte{0xbc},
                                                  std::byte{0x00}};
  append_u32(bytes, table.tuple_size);
  append_u32(bytes, table.descriptor_count);
  for (const auto descriptor : schema.table_descriptors(table)) append_u32(bytes, descriptor);

  std::uint32_t row_number = 0U;
  const auto append_tuple = [&](const auto& write) {
    bytes.push_back(std::byte{0});
    const auto tuple_offset = bytes.size();
    bytes.resize(bytes.size() + table.tuple_size, std::byte{0});
    auto tuple = std::span<std::byte>(bytes).subspan(tuple_offset, table.tuple_size);
    write(tuple);
    append_u32(bytes, 30'000U + row_number);
    append_u32(bytes, 40'000U + row_number);
    ++row_number;
  };
  const auto write_scalar = [](std::span<std::byte> tuple,
                               const tekla::db1::detail::FieldSchema& field, double value) {
    if (field.type == tekla::db1::detail::FieldType::f32) {
      write_u32(tuple, field.offset, std::bit_cast<std::uint32_t>(static_cast<float>(value)));
    } else {
      write_f64(tuple, field.offset, value);
    }
  };

  if (table.name == "object") {
    const auto append_object = [&](std::uint32_t id, std::uint32_t type, std::uint32_t subtype,
                                   std::uint8_t guid_seed) {
      append_tuple([&](std::span<std::byte> tuple) {
        for (const auto& field : schema.table_fields(table)) {
          if (field.name == "id") write_u32(tuple, field.offset, id);
          if (field.name == "type") write_u32(tuple, field.offset, type);
          if (field.name == "subtype") write_u32(tuple, field.offset, subtype);
          if (field.name == "guid") {
            for (std::size_t index = 0; index < field.size; ++index) {
              tuple[field.offset + index] =
                  static_cast<std::byte>(guid_seed + static_cast<std::uint8_t>(index));
            }
          }
        }
      });
    };
    append_object(1201U, 3U, 0U, 0x20U);
    append_object(1301U, 73U, 3U, 0x60U);
  } else if (table.name == "coordsys_attr") {
    append_tuple([&](std::span<std::byte> tuple) {
      for (const auto& field : schema.table_fields(table)) {
        if (field.name == "id") write_u32(tuple, field.offset, 901U);
        if (field.name == "xdir_y" || field.name == "ydir_x") write_scalar(tuple, field, 1.0);
      }
    });
  } else if (table.name == "coordsys") {
    append_tuple([&](std::span<std::byte> tuple) {
      for (const auto& field : schema.table_fields(table)) {
        if (field.name == "id") write_u32(tuple, field.offset, 1201U);
        if (field.name == "csys_attr_id") write_u32(tuple, field.offset, 901U);
        if (field.name == "z1") write_scalar(tuple, field, -15.0);
        if (field.name == "length") write_scalar(tuple, field, 1010.0);
      }
    });
  } else if (table.name == "part_attr") {
    append_tuple([&](std::span<std::byte> tuple) {
      for (const auto& field : schema.table_fields(table)) {
        if (field.name == "id") write_u32(tuple, field.offset, 999U);
        if (field.name == "prof") write_fixed(tuple, field.offset, field.size, "200*10");
      }
    });
  } else if (table.name == "point") {
    const auto append_point = [&](std::uint32_t id, double x) {
      append_tuple([&](std::span<std::byte> tuple) {
        for (const auto& field : schema.table_fields(table)) {
          if (field.name == "id") write_u32(tuple, field.offset, id);
          if (field.name == "x") write_scalar(tuple, field, x);
        }
      });
    };
    append_point(920U, 0.0);
    append_point(921U, 250.0);
  } else if (table.name == "positioning") {
    append_tuple([&](std::span<std::byte> tuple) {
      for (const auto& field : schema.table_fields(table)) {
        if (field.name == "id") write_u32(tuple, field.offset, 905U);
        if (field.name == "PositionAtDepth") write_u32(tuple, field.offset, 2U);
      }
    });
  } else if (table.name == "surfacing") {
    append_tuple([&](std::span<std::byte> tuple) {
      for (const auto& field : schema.table_fields(table)) {
        if (field.name == "id") write_u32(tuple, field.offset, 1301U);
        if (field.name == "attr_id") write_u32(tuple, field.offset, 903U);
        if (field.name == "PositioningAttrId") write_u32(tuple, field.offset, 905U);
        if (field.name == "p1") write_u32(tuple, field.offset, 920U);
        if (field.name == "p2") write_u32(tuple, field.offset, 921U);
        if (field.name == "polygon_id") write_u32(tuple, field.offset, 950U);
      }
    });
  } else if (table.name == "surfacing_attr") {
    append_tuple([&](std::span<std::byte> tuple) {
      for (const auto& field : schema.table_fields(table)) {
        if (field.name == "id") write_u32(tuple, field.offset, 903U);
        if (field.name == "npoints") write_u32(tuple, field.offset, 4U);
        if (field.name == "ryhma") write_fixed(tuple, field.offset, field.size, "0");
        if (field.name == "ben") write_fixed(tuple, field.offset, field.size, "SURFACE-GRATING");
        if (field.name == "Geometry") write_fixed(tuple, field.offset, field.size, "1.000000");
        if (field.name == "mat") write_fixed(tuple, field.offset, field.size, "Zero_Density");
        if (field.name == "finish") write_fixed(tuple, field.offset, field.size, "TS8 - Open-Mesh");
        if (field.name == "surfacing_type") write_u32(tuple, field.offset, 3U);
        if (field.name == "father_cuts") write_u32(tuple, field.offset, 1U);
      }
    });
  } else if (table.name == "partpolygon") {
    append_tuple([&](std::span<std::byte> tuple) {
      constexpr std::array<double, 4> x{0.0, 250.0, 250.0, 0.0};
      constexpr std::array<double, 4> y{0.0, 0.0, 1010.0, 1010.0};
      for (const auto& field : schema.table_fields(table)) {
        if (field.name == "id") write_u32(tuple, field.offset, 950U);
        if (field.name == "hash_id") write_u32(tuple, field.offset, 951U);
        for (std::size_t index = 0U; index < 10U; ++index) {
          const auto suffix = std::to_string(index + 1U);
          if (index < x.size() && field.name == "x" + suffix) write_scalar(tuple, field, x[index]);
          if (index < y.size() && field.name == "y" + suffix) write_scalar(tuple, field, y[index]);
          if (field.name == "types" + suffix) {
            write_u32(tuple, field.offset, index < x.size() ? 0U : 2'147'483'647U);
          }
        }
      }
    });
  } else if (table.name == "relation") {
    append_tuple([&](std::span<std::byte> tuple) {
      for (const auto& field : schema.table_fields(table)) {
        if (field.name == "id") write_u32(tuple, field.offset, 960U);
        if (field.name == "type") write_u32(tuple, field.offset, 73U);
        if (field.name == "id1") write_u32(tuple, field.offset, 1201U);
        if (field.name == "id2") write_u32(tuple, field.offset, 1301U);
      }
    });
  }

  bytes.push_back(std::byte{0});
  if (final) {
    bytes.insert(bytes.end(), final_footer.begin(), final_footer.end());
  } else {
    bytes.insert(bytes.end(), table_end.begin(), table_end.end());
  }
}

std::vector<std::byte> database_with_surface_treatment() {
  constexpr std::array<std::byte, 4> table_end{std::byte{0x66}, std::byte{0xc0}, std::byte{0xce},
                                               std::byte{0xdb}};
  constexpr std::string_view format = "9.66";
  const auto* schema = tekla::db1::detail::schema_for(format, 0x85);
  CHECK(schema != nullptr, "the surface-treatment fixture schema is registered");
  std::vector<std::byte> bytes;
  append_ascii(bytes, "Xsteel");
  bytes.push_back(std::byte{0x85});
  bytes.push_back(std::byte{' '});
  bytes.insert(bytes.end(), reinterpret_cast<const std::byte*>(format.data()),
               reinterpret_cast<const std::byte*>(format.data() + format.size()));
  append_ascii(bytes, " 7d72d8c9-0250-4f3a-8760-bcef517f016e");
  append_u32(bytes, 1U);
  bytes.insert(bytes.end(), table_end.begin(), table_end.end());
  if (schema != nullptr) {
    for (std::size_t index = 0U; index < schema->tables.size(); ++index) {
      append_surface_treatment_table(bytes, *schema, schema->tables[index],
                                     index + 1U == schema->tables.size());
    }
  }
  return bytes;
}

void append_pour_table(std::vector<std::byte>& bytes, const tekla::db1::detail::Schema& schema,
                       const tekla::db1::detail::TableSchema& table, bool final) {
  constexpr std::array<std::byte, 4> table_end{std::byte{0x66}, std::byte{0xc0}, std::byte{0xce},
                                               std::byte{0xdb}};
  constexpr std::array<std::byte, 4> final_footer{std::byte{0x4f}, std::byte{0x61}, std::byte{0xbc},
                                                  std::byte{0x00}};
  append_u32(bytes, table.tuple_size);
  append_u32(bytes, table.descriptor_count);
  for (const auto descriptor : schema.table_descriptors(table)) append_u32(bytes, descriptor);

  std::uint32_t row_number = 0U;
  const auto append_tuple = [&](const auto& write) {
    bytes.push_back(std::byte{0});
    const auto tuple_offset = bytes.size();
    bytes.resize(bytes.size() + table.tuple_size, std::byte{0});
    auto tuple = std::span<std::byte>(bytes).subspan(tuple_offset, table.tuple_size);
    write(tuple);
    append_u32(bytes, 50'000U + row_number);
    append_u32(bytes, 60'000U + row_number);
    ++row_number;
  };

  if (table.name == "object") {
    const auto append_object = [&](std::uint32_t id, std::uint32_t type, std::uint32_t subtype,
                                   std::uint8_t guid_seed) {
      append_tuple([&](std::span<std::byte> tuple) {
        for (const auto& field : schema.table_fields(table)) {
          if (field.name == "id") write_u32(tuple, field.offset, id);
          if (field.name == "type") write_u32(tuple, field.offset, type);
          if (field.name == "subtype") write_u32(tuple, field.offset, subtype);
          if (field.name == "guid") {
            for (std::size_t index = 0; index < field.size; ++index) {
              tuple[field.offset + index] =
                  static_cast<std::byte>(guid_seed + static_cast<std::uint8_t>(index));
            }
          }
        }
      });
    };
    append_object(1401U, 90U, 0U, 0x30U);
    append_object(1402U, 101U, 0U, 0x50U);
    append_object(1403U, 90U, 1U, 0x70U);
  } else if (table.name == "pour_object") {
    append_tuple([&](std::span<std::byte> tuple) {
      for (const auto& field : schema.table_fields(table)) {
        if (field.name == "id") write_u32(tuple, field.offset, 1401U);
        if (field.name == "obj_class") write_u32(tuple, field.offset, 7U);
        if (field.name == "pour_phase") write_u32(tuple, field.offset, 12U);
        if (field.name == "pour_unit_id") write_u32(tuple, field.offset, 1402U);
        if (field.name == "pour_number") write_fixed(tuple, field.offset, field.size, "POUR-42");
        if (field.name == "pour_type") write_fixed(tuple, field.offset, field.size, "Bridge deck");
        if (field.name == "concrete_mixture")
          write_fixed(tuple, field.offset, field.size, "C35/45");
      }
    });
  } else if (table.name == "pour_unit") {
    append_tuple([&](std::span<std::byte> tuple) {
      for (const auto& field : schema.table_fields(table)) {
        if (field.name == "id") write_u32(tuple, field.offset, 1402U);
        if (field.name == "pour_id") write_u32(tuple, field.offset, 1401U);
        if (field.name == "name") write_fixed(tuple, field.offset, field.size, "Deck pour unit");
      }
    });
  }

  bytes.push_back(std::byte{0});
  bytes.insert(bytes.end(), final ? final_footer.begin() : table_end.begin(),
               final ? final_footer.end() : table_end.end());
}

std::vector<std::byte> database_with_pour_objects() {
  constexpr std::array<std::byte, 4> table_end{std::byte{0x66}, std::byte{0xc0}, std::byte{0xce},
                                               std::byte{0xdb}};
  constexpr std::string_view format = "9.66";
  const auto* schema = tekla::db1::detail::schema_for(format, 0x85);
  CHECK(schema != nullptr, "the pour fixture schema is registered");
  std::vector<std::byte> bytes;
  append_ascii(bytes, "Xsteel");
  bytes.push_back(std::byte{0x85});
  bytes.push_back(std::byte{' '});
  bytes.insert(bytes.end(), reinterpret_cast<const std::byte*>(format.data()),
               reinterpret_cast<const std::byte*>(format.data() + format.size()));
  append_ascii(bytes, " 7d72d8c9-0250-4f3a-8760-bcef517f016e");
  append_u32(bytes, 1U);
  bytes.insert(bytes.end(), table_end.begin(), table_end.end());
  if (schema != nullptr) {
    for (std::size_t index = 0U; index < schema->tables.size(); ++index) {
      append_pour_table(bytes, *schema, schema->tables[index], index + 1U == schema->tables.size());
    }
  }
  return bytes;
}

void append_rebar_splice_table(std::vector<std::byte>& bytes,
                               const tekla::db1::detail::Schema& schema,
                               const tekla::db1::detail::TableSchema& table, bool final) {
  constexpr std::array<std::byte, 4> table_end{std::byte{0x66}, std::byte{0xc0}, std::byte{0xce},
                                               std::byte{0xdb}};
  constexpr std::array<std::byte, 4> final_footer{std::byte{0x4f}, std::byte{0x61}, std::byte{0xbc},
                                                  std::byte{0x00}};
  append_u32(bytes, table.tuple_size);
  append_u32(bytes, table.descriptor_count);
  for (const auto descriptor : schema.table_descriptors(table)) append_u32(bytes, descriptor);

  std::uint32_t row_number = 0U;
  const auto append_tuple = [&](const auto& write) {
    bytes.push_back(std::byte{0});
    const auto tuple_offset = bytes.size();
    bytes.resize(bytes.size() + table.tuple_size, std::byte{0});
    auto tuple = std::span<std::byte>(bytes).subspan(tuple_offset, table.tuple_size);
    write(tuple);
    append_u32(bytes, 70'000U + row_number);
    append_u32(bytes, 80'000U + row_number);
    ++row_number;
  };

  if (table.name == "object") {
    const auto append_object = [&](std::uint32_t id, std::uint32_t type, std::uint32_t subtype,
                                   std::uint8_t guid_seed) {
      append_tuple([&](std::span<std::byte> tuple) {
        for (const auto& field : schema.table_fields(table)) {
          if (field.name == "id") write_u32(tuple, field.offset, id);
          if (field.name == "type") write_u32(tuple, field.offset, type);
          if (field.name == "subtype") write_u32(tuple, field.offset, subtype);
          if (field.name == "guid") {
            for (std::size_t index = 0; index < field.size; ++index) {
              tuple[field.offset + index] =
                  static_cast<std::byte>(guid_seed + static_cast<std::uint8_t>(index));
            }
          }
        }
      });
    };
    append_object(1501U, 74U, 0U, 0x20U);
    append_object(1502U, 16U, 1U, 0x40U);
    append_object(1503U, 16U, 1U, 0x60U);
  } else if (table.name == "rebar_splice") {
    append_tuple([&](std::span<std::byte> tuple) {
      for (const auto& field : schema.table_fields(table)) {
        if (field.name == "id") write_u32(tuple, field.offset, 1501U);
        if (field.name == "rebarid1") write_u32(tuple, field.offset, 1502U);
        if (field.name == "rebarid2") write_u32(tuple, field.offset, 1503U);
        if (field.name == "end1") write_u32(tuple, field.offset, 1U);
        if (field.name == "end2") write_u32(tuple, field.offset, 0U);
        if (field.name == "type") write_u32(tuple, field.offset, 2U);
        if (field.name == "laplength") write_f64(tuple, field.offset, 600.0);
        if (field.name == "offset") write_f64(tuple, field.offset, 12.5);
        if (field.name == "clearance") write_f64(tuple, field.offset, 8.0);
        if (field.name == "position") write_u32(tuple, field.offset, 1U);
      }
    });
  }

  bytes.push_back(std::byte{0});
  bytes.insert(bytes.end(), final ? final_footer.begin() : table_end.begin(),
               final ? final_footer.end() : table_end.end());
}

std::vector<std::byte> database_with_rebar_splice() {
  constexpr std::array<std::byte, 4> table_end{std::byte{0x66}, std::byte{0xc0}, std::byte{0xce},
                                               std::byte{0xdb}};
  constexpr std::string_view format = "9.66";
  const auto* schema = tekla::db1::detail::schema_for(format, 0x85);
  CHECK(schema != nullptr, "the rebar-splice fixture schema is registered");
  std::vector<std::byte> bytes;
  append_ascii(bytes, "Xsteel");
  bytes.push_back(std::byte{0x85});
  bytes.push_back(std::byte{' '});
  bytes.insert(bytes.end(), reinterpret_cast<const std::byte*>(format.data()),
               reinterpret_cast<const std::byte*>(format.data() + format.size()));
  append_ascii(bytes, " 7d72d8c9-0250-4f3a-8760-bcef517f016e");
  append_u32(bytes, 1U);
  bytes.insert(bytes.end(), table_end.begin(), table_end.end());
  if (schema != nullptr) {
    for (std::size_t index = 0U; index < schema->tables.size(); ++index) {
      append_rebar_splice_table(bytes, *schema, schema->tables[index],
                                index + 1U == schema->tables.size());
    }
  }
  return bytes;
}

void append_surface_object_table(std::vector<std::byte>& bytes,
                                 const tekla::db1::detail::Schema& schema,
                                 const tekla::db1::detail::TableSchema& table, bool final) {
  constexpr std::array<std::byte, 4> table_end{std::byte{0x66}, std::byte{0xc0}, std::byte{0xce},
                                               std::byte{0xdb}};
  constexpr std::array<std::byte, 4> final_footer{std::byte{0x4f}, std::byte{0x61}, std::byte{0xbc},
                                                  std::byte{0x00}};
  append_u32(bytes, table.tuple_size);
  append_u32(bytes, table.descriptor_count);
  for (const auto descriptor : schema.table_descriptors(table)) append_u32(bytes, descriptor);

  std::uint32_t row_number = 0U;
  const auto append_tuple = [&](const auto& write) {
    bytes.push_back(std::byte{0});
    const auto tuple_offset = bytes.size();
    bytes.resize(bytes.size() + table.tuple_size, std::byte{0});
    auto tuple = std::span<std::byte>(bytes).subspan(tuple_offset, table.tuple_size);
    write(tuple);
    append_u32(bytes, 90'000U + row_number);
    append_u32(bytes, 100'000U + row_number);
    ++row_number;
  };

  if (table.name == "object") {
    const auto append_object = [&](std::uint32_t id, std::uint32_t type, std::uint32_t subtype,
                                   std::uint8_t guid_seed) {
      append_tuple([&](std::span<std::byte> tuple) {
        for (const auto& field : schema.table_fields(table)) {
          if (field.name == "id") write_u32(tuple, field.offset, id);
          if (field.name == "type") write_u32(tuple, field.offset, type);
          if (field.name == "subtype") write_u32(tuple, field.offset, subtype);
          if (field.name == "guid") {
            for (std::size_t index = 0; index < field.size; ++index) {
              tuple[field.offset + index] =
                  static_cast<std::byte>(guid_seed + static_cast<std::uint8_t>(index));
            }
          }
        }
      });
    };
    append_object(1601U, 97U, 1U, 0x10U);
    append_object(1602U, 2U, 0U, 0x30U);
  } else if (table.name == "surface_object") {
    append_tuple([&](std::span<std::byte> tuple) {
      for (const auto& field : schema.table_fields(table)) {
        if (field.name == "id") write_u32(tuple, field.offset, 1601U);
        if (field.name == "obj_class") write_u32(tuple, field.offset, 60U);
        if (field.name == "name")
          write_fixed(tuple, field.offset, field.size, "Rebar set leg surface");
        if (field.name == "group") write_fixed(tuple, field.offset, field.size, "4");
        if (field.name == "related_part_id") write_u32(tuple, field.offset, 1602U);
        if (field.name == "geometry_type") write_u32(tuple, field.offset, 2U);
        if (field.name == "created_from_pour") write_u32(tuple, field.offset, 0U);
        if (field.name == "length_direction_x") write_f64(tuple, field.offset, 1.0);
        if (field.name == "length_direction_y") write_f64(tuple, field.offset, 0.0);
        if (field.name == "length_direction_z") write_f64(tuple, field.offset, 0.0);
        if (field.name == "layer_number") write_u32(tuple, field.offset, 2U);
        if (field.name == "offset") write_f64(tuple, field.offset, 12.5);
        if (field.name == "offset_type") write_u32(tuple, field.offset, 1U);
        if (field.name == "surface_type")
          write_fixed(tuple, field.offset, field.size, "Rebar set leg surface");
      }
    });
  }

  bytes.push_back(std::byte{0});
  bytes.insert(bytes.end(), final ? final_footer.begin() : table_end.begin(),
               final ? final_footer.end() : table_end.end());
}

std::vector<std::byte> database_with_surface_object() {
  constexpr std::array<std::byte, 4> table_end{std::byte{0x66}, std::byte{0xc0}, std::byte{0xce},
                                               std::byte{0xdb}};
  constexpr std::string_view format = "9.66";
  const auto* schema = tekla::db1::detail::schema_for(format, 0x85);
  CHECK(schema != nullptr, "the surface-object fixture schema is registered");
  std::vector<std::byte> bytes;
  append_ascii(bytes, "Xsteel");
  bytes.push_back(std::byte{0x85});
  bytes.push_back(std::byte{' '});
  bytes.insert(bytes.end(), reinterpret_cast<const std::byte*>(format.data()),
               reinterpret_cast<const std::byte*>(format.data() + format.size()));
  append_ascii(bytes, " 7d72d8c9-0250-4f3a-8760-bcef517f016e");
  append_u32(bytes, 1U);
  bytes.insert(bytes.end(), table_end.begin(), table_end.end());
  if (schema != nullptr) {
    for (std::size_t index = 0U; index < schema->tables.size(); ++index) {
      append_surface_object_table(bytes, *schema, schema->tables[index],
                                  index + 1U == schema->tables.size());
    }
  }
  return bytes;
}

void append_rebar_set_table(std::vector<std::byte>& bytes, const tekla::db1::detail::Schema& schema,
                            const tekla::db1::detail::TableSchema& table, bool final) {
  constexpr std::array<std::byte, 4> table_end{std::byte{0x66}, std::byte{0xc0}, std::byte{0xce},
                                               std::byte{0xdb}};
  constexpr std::array<std::byte, 4> final_footer{std::byte{0x4f}, std::byte{0x61}, std::byte{0xbc},
                                                  std::byte{0x00}};
  append_u32(bytes, table.tuple_size);
  append_u32(bytes, table.descriptor_count);
  for (const auto descriptor : schema.table_descriptors(table)) append_u32(bytes, descriptor);

  std::uint32_t row_number = 0U;
  const auto append_tuple = [&](const auto& write) {
    bytes.push_back(std::byte{0});
    const auto tuple_offset = bytes.size();
    bytes.resize(bytes.size() + table.tuple_size, std::byte{0});
    auto tuple = std::span<std::byte>(bytes).subspan(tuple_offset, table.tuple_size);
    write(tuple);
    append_u32(bytes, 110'000U + row_number);
    append_u32(bytes, 120'000U + row_number);
    ++row_number;
  };

  if (table.name == "object") {
    const auto append_object = [&](std::uint32_t id, std::uint32_t type, std::uint32_t subtype,
                                   std::uint8_t guid_seed) {
      append_tuple([&](std::span<std::byte> tuple) {
        for (const auto& field : schema.table_fields(table)) {
          if (field.name == "id") write_u32(tuple, field.offset, id);
          if (field.name == "type") write_u32(tuple, field.offset, type);
          if (field.name == "subtype") write_u32(tuple, field.offset, subtype);
          if (field.name == "guid") {
            for (std::size_t index = 0; index < field.size; ++index) {
              tuple[field.offset + index] =
                  static_cast<std::byte>(guid_seed + static_cast<std::uint8_t>(index));
            }
          }
        }
      });
    };
    append_object(1701U, 47U, 9U, 0x10U);
    append_object(1702U, 10247U, 0U, 0x30U);
    append_object(1703U, 96U, 1U, 0x50U);
    append_object(1704U, 96U, 3U, 0x70U);
  } else if (table.name == "string") {
    const auto append_string = [&](std::uint32_t id, std::string_view value) {
      append_tuple([&](std::span<std::byte> tuple) {
        for (const auto& field : schema.table_fields(table)) {
          if (field.name == "id") write_u32(tuple, field.offset, id);
          if (field.name == "string") write_fixed(tuple, field.offset, field.size, value);
        }
      });
    };
    append_string(1801U, "REBAR");
    append_string(1802U, "B500B");
    append_string(1803U, "12");
    append_string(1804U, "Uncoated");
  } else if (table.name == "rebarset") {
    append_tuple([&](std::span<std::byte> tuple) {
      for (const auto& field : schema.table_fields(table)) {
        if (field.name == "id") write_u32(tuple, field.offset, 1701U);
        if (field.name == "prop_id") write_u32(tuple, field.offset, 1901U);
        if (field.name == "layer_order_number") write_u32(tuple, field.offset, 2U);
        if (field.name == "orientation_id") write_u32(tuple, field.offset, 1902U);
        if (field.name == "flags") write_u32(tuple, field.offset, 3U);
      }
    });
  } else if (table.name == "rebarset_prop") {
    append_tuple([&](std::span<std::byte> tuple) {
      for (const auto& field : schema.table_fields(table)) {
        if (field.name == "id") write_u32(tuple, field.offset, 1901U);
        if (field.name == "bar_class") write_u32(tuple, field.offset, 4U);
        if (field.name == "name_id") write_u32(tuple, field.offset, 1801U);
        if (field.name == "grade_id") write_u32(tuple, field.offset, 1802U);
        if (field.name == "size_id") write_u32(tuple, field.offset, 1803U);
        if (field.name == "finish_id") write_u32(tuple, field.offset, 1804U);
        if (field.name == "bending_radius") write_f64(tuple, field.offset, 30.0);
        if (field.name == "start_number") write_u32(tuple, field.offset, 1U);
        if (field.name == "prefix") write_fixed(tuple, field.offset, field.size, "RS");
      }
    });
  } else if (table.name == "rebarset_group") {
    append_tuple([&](std::span<std::byte> tuple) {
      for (const auto& field : schema.table_fields(table)) {
        if (field.name == "id") write_u32(tuple, field.offset, 1702U);
      }
    });
  } else if (table.name == "rebarset_end_detail_strip") {
    append_tuple([&](std::span<std::byte> tuple) {
      for (const auto& field : schema.table_fields(table)) {
        if (field.name == "id") write_u32(tuple, field.offset, 1703U);
        if (field.name == "end_type") write_u32(tuple, field.offset, 1U);
        if (field.name == "hook_type") write_u32(tuple, field.offset, 2U);
        if (field.name == "thread_type") write_u32(tuple, field.offset, 3U);
        if (field.name == "crank_type") write_u32(tuple, field.offset, 4U);
        if (field.name == "crank_length_type") write_u32(tuple, field.offset, 5U);
        if (field.name == "end_offset") write_f64(tuple, field.offset, 25.0);
        if (field.name == "end_offset_type") write_u32(tuple, field.offset, 6U);
        if (field.name == "bars_affected") write_u32(tuple, field.offset, 7U);
        if (field.name == "first_affected") write_u32(tuple, field.offset, 8U);
        if (field.name == "follows_edges") write_u32(tuple, field.offset, 1U);
        if (field.name == "apply_order_number") write_u32(tuple, field.offset, 9U);
        if (field.name == "flags") write_u32(tuple, field.offset, 10U);
      }
    });
  } else if (table.name == "rebarset_splitter") {
    append_tuple([&](std::span<std::byte> tuple) {
      for (const auto& field : schema.table_fields(table)) {
        if (field.name == "id") write_u32(tuple, field.offset, 1704U);
        if (field.name == "attr_id") write_u32(tuple, field.offset, 1903U);
        if (field.name == "apply_order_number") write_u32(tuple, field.offset, 11U);
      }
    });
  } else if (table.name == "rebarset_splitter_attr") {
    append_tuple([&](std::span<std::byte> tuple) {
      for (const auto& field : schema.table_fields(table)) {
        if (field.name == "id") write_u32(tuple, field.offset, 1903U);
        if (field.name == "lap_length") write_f64(tuple, field.offset, 450.0);
        if (field.name == "bars_affected") write_u32(tuple, field.offset, 1U);
        if (field.name == "first_affected") write_u32(tuple, field.offset, 2U);
        if (field.name == "follows_edges") write_u32(tuple, field.offset, 1U);
        if (field.name == "stagger_type") write_u32(tuple, field.offset, 3U);
        if (field.name == "stagger_offset") write_f64(tuple, field.offset, 120.0);
        if (field.name == "lap_side") write_u32(tuple, field.offset, 2U);
        if (field.name == "split_offset") write_f64(tuple, field.offset, 15.0);
        if (field.name == "lap_offset_dir") write_u32(tuple, field.offset, 4U);
        if (field.name == "splicing_type") write_u32(tuple, field.offset, 5U);
        if (field.name == "crank_side") write_u32(tuple, field.offset, 6U);
        if (field.name == "crank_length_type") write_u32(tuple, field.offset, 7U);
        if (field.name == "crank_id") write_u32(tuple, field.offset, 1904U);
      }
    });
  } else if (table.name == "relation") {
    const auto append_relation = [&](std::uint32_t id, std::uint32_t type, std::uint32_t source,
                                     std::uint32_t target) {
      append_tuple([&](std::span<std::byte> tuple) {
        for (const auto& field : schema.table_fields(table)) {
          if (field.name == "id") write_u32(tuple, field.offset, id);
          if (field.name == "type") write_u32(tuple, field.offset, type);
          if (field.name == "id1") write_u32(tuple, field.offset, source);
          if (field.name == "id2") write_u32(tuple, field.offset, target);
        }
      });
    };
    append_relation(2001U, 60005U, 1701U, 1702U);
    append_relation(2002U, 96U, 1701U, 1703U);
    append_relation(2003U, 96U, 1701U, 1704U);
  }

  bytes.push_back(std::byte{0});
  bytes.insert(bytes.end(), final ? final_footer.begin() : table_end.begin(),
               final ? final_footer.end() : table_end.end());
}

std::vector<std::byte> database_with_rebar_set() {
  constexpr std::array<std::byte, 4> table_end{std::byte{0x66}, std::byte{0xc0}, std::byte{0xce},
                                               std::byte{0xdb}};
  constexpr std::string_view format = "9.66";
  const auto* schema = tekla::db1::detail::schema_for(format, 0x85);
  CHECK(schema != nullptr, "the rebar-set fixture schema is registered");
  std::vector<std::byte> bytes;
  append_ascii(bytes, "Xsteel");
  bytes.push_back(std::byte{0x85});
  bytes.push_back(std::byte{' '});
  bytes.insert(bytes.end(), reinterpret_cast<const std::byte*>(format.data()),
               reinterpret_cast<const std::byte*>(format.data() + format.size()));
  append_ascii(bytes, " 7d72d8c9-0250-4f3a-8760-bcef517f016e");
  append_u32(bytes, 1U);
  bytes.insert(bytes.end(), table_end.begin(), table_end.end());
  if (schema != nullptr) {
    for (std::size_t index = 0U; index < schema->tables.size(); ++index) {
      append_rebar_set_table(bytes, *schema, schema->tables[index],
                             index + 1U == schema->tables.size());
    }
  }
  return bytes;
}

std::vector<std::byte> database_with_relationship_semantics(std::string_view format) {
  return database_with_one_object("200*10", "14", format, false, 7U, false, false, false, false,
                                  false, false, false, false, false, false, false, false, false,
                                  false, true);
}

std::vector<std::byte> database_with_skewed_part_frame() {
  return database_with_one_object("200*10", "14", "9.66", false, 7U, false, false, false, false,
                                  false, false, false, false, false, false, false, false, false,
                                  false, false, PartFrameFixture::skewed);
}

std::vector<std::byte> database_with_rotated_part_frame() {
  return database_with_one_object("200*10", "14", "9.66", false, 7U, false, false, false, false,
                                  false, false, false, false, false, false, false, false, false,
                                  false, false, PartFrameFixture::rotated);
}

std::vector<std::byte> database_with_report_semantics(std::string_view format) {
  return database_with_one_object("IPE", "14", format, false, 7U, false, false, false, false, false,
                                  false, false, false, false, false, true);
}

std::vector<std::byte> database_with_zero_position_report_semantics(std::string_view format) {
  return database_with_one_object("IPE200", "14", format, false, 7U, false, false, false, false,
                                  false, false, false, false, false, false, true, true);
}

std::vector<std::byte> database_with_bolt_semantics() {
  return database_with_one_object("200*10", "14", "9.66", false, 7U, false, false, false, false,
                                  false, false, false, false, false, false, false, false, true);
}

std::vector<std::byte> database_with_legacy_bolt_geometry() {
  return database_with_one_object("200*10", "14", "8.95", false, 7U, false, false, false, false,
                                  false, false, false, false, false, false, false, false, false,
                                  true);
}

void append_shared_rebar_table(std::vector<std::byte>& bytes,
                               const tekla::db1::detail::Schema& schema,
                               const tekla::db1::detail::TableSchema& table, bool final,
                               std::size_t rebar_count, std::size_t array_count,
                               bool malformed_group, bool oversized_incomplete_group,
                               bool mesh_fixture = false) {
  constexpr std::array<std::byte, 4> table_end{std::byte{0x66}, std::byte{0xc0}, std::byte{0xce},
                                               std::byte{0xdb}};
  constexpr std::array<std::byte, 4> final_footer{std::byte{0x4f}, std::byte{0x61}, std::byte{0xbc},
                                                  std::byte{0x00}};
  append_u32(bytes, table.tuple_size);
  append_u32(bytes, table.descriptor_count);
  for (const auto descriptor : schema.table_descriptors(table)) append_u32(bytes, descriptor);

  std::uint32_t row_number = 0U;
  const auto append_tuple = [&](const auto& write) {
    bytes.push_back(std::byte{0});
    const auto tuple_offset = bytes.size();
    bytes.resize(bytes.size() + table.tuple_size, std::byte{0});
    auto tuple = std::span<std::byte>(bytes).subspan(tuple_offset, table.tuple_size);
    write(tuple);
    append_u32(bytes, 100'000U + row_number);
    append_u32(bytes, 200'000U + row_number);
    ++row_number;
  };
  const auto write_scalar = [](std::span<std::byte> tuple,
                               const tekla::db1::detail::FieldSchema& field, double value) {
    if (field.type == tekla::db1::detail::FieldType::f32) {
      write_u32(tuple, field.offset, std::bit_cast<std::uint32_t>(static_cast<float>(value)));
    } else {
      write_f64(tuple, field.offset, value);
    }
  };

  if (table.name == "object") {
    append_tuple([&](std::span<std::byte> tuple) {
      for (const auto& field : schema.table_fields(table)) {
        if (field.name == "id") write_u32(tuple, field.offset, 1200U);
        if (field.name == "type") write_u32(tuple, field.offset, 1U);
        if (field.name == "subtype") write_u32(tuple, field.offset, 22U);
      }
    });
    for (std::size_t index = 0; index < rebar_count; ++index) {
      const auto rebar_id = 1201U + static_cast<std::uint32_t>(index);
      append_tuple([&](std::span<std::byte> tuple) {
        for (const auto& field : schema.table_fields(table)) {
          if (field.name == "id") write_u32(tuple, field.offset, rebar_id);
          if (field.name == "type") write_u32(tuple, field.offset, 47U);
          if (field.name == "subtype") {
            write_u32(tuple, field.offset,
                      mesh_fixture ? (index == 0U ? 6U : 8U)
                                   : (malformed_group || oversized_incomplete_group ? 1U : 0U));
          }
        }
      });
    }
  } else if (table.name == "part") {
    append_tuple([&](std::span<std::byte> tuple) {
      for (const auto& field : schema.table_fields(table)) {
        if (field.name == "id") write_u32(tuple, field.offset, 1200U);
        if (field.name == "part_attr_id") write_u32(tuple, field.offset, 904U);
        if (field.name == "csys_attr_id") write_u32(tuple, field.offset, 901U);
        if (field.name == "csys_length") write_scalar(tuple, field, 100.0);
      }
    });
  } else if (table.name == "part_attr") {
    append_tuple([&](std::span<std::byte> tuple) {
      for (const auto& field : schema.table_fields(table)) {
        if (field.name == "id") write_u32(tuple, field.offset, 904U);
        if (field.name == "form_type") write_u32(tuple, field.offset, 7U);
        if (field.name == "obj_type") write_u32(tuple, field.offset, 2U);
        if (field.name == "prof") write_fixed(tuple, field.offset, field.size, "200*10");
      }
    });
  } else if (table.name == "coordsys_attr") {
    append_tuple([&](std::span<std::byte> tuple) {
      for (const auto& field : schema.table_fields(table)) {
        if (field.name == "id") write_u32(tuple, field.offset, 901U);
        if (field.name == "xdir_x" || field.name == "ydir_y") write_scalar(tuple, field, 1.0);
      }
    });
  } else if (table.name == "coordsys") {
    for (std::size_t index = 0; index < rebar_count; ++index) {
      const auto rebar_id = 1201U + static_cast<std::uint32_t>(index);
      append_tuple([&](std::span<std::byte> tuple) {
        for (const auto& field : schema.table_fields(table)) {
          if (field.name == "id") write_u32(tuple, field.offset, rebar_id);
          if (field.name == "csys_attr_id") write_u32(tuple, field.offset, 901U);
          if (field.name == "x1")
            write_scalar(tuple, field,
                         mesh_fixture ? (index == 0U ? 10.0 : 100.0) : static_cast<double>(index));
          if (field.name == "y1" && mesh_fixture)
            write_scalar(tuple, field, index == 0U ? 20.0 : 200.0);
          if (field.name == "z1" && mesh_fixture)
            write_scalar(tuple, field, index == 0U ? 30.0 : 300.0);
        }
      });
    }
  } else if (table.name == "rebar_attr") {
    append_tuple([&](std::span<std::byte> tuple) {
      for (const auto& field : schema.table_fields(table)) {
        if (field.name == "id") write_u32(tuple, field.offset, 902U);
        if (field.name == "profile_id") write_u32(tuple, field.offset, 903U);
        if (field.name == "bar_class") write_u32(tuple, field.offset, 14U);
        if (field.name == "bar_type_info_id" && (malformed_group || oversized_incomplete_group)) {
          write_u32(tuple, field.offset, 904U);
        }
      }
    });
  } else if (table.name == "string") {
    append_tuple([&](std::span<std::byte> tuple) {
      for (const auto& field : schema.table_fields(table)) {
        if (field.name == "id") write_u32(tuple, field.offset, 903U);
        if (field.name == "string") write_fixed(tuple, field.offset, field.size, "8");
      }
    });
    if (mesh_fixture) {
      append_tuple([&](std::span<std::byte> tuple) {
        for (const auto& field : schema.table_fields(table)) {
          if (field.name == "id") write_u32(tuple, field.offset, 905U);
          if (field.name == "string") write_fixed(tuple, field.offset, field.size, "8\t12");
        }
      });
      append_tuple([&](std::span<std::byte> tuple) {
        for (const auto& field : schema.table_fields(table)) {
          if (field.name == "id") write_u32(tuple, field.offset, 906U);
          if (field.name == "string") write_fixed(tuple, field.offset, field.size, "400\t250");
        }
      });
    }
  } else if (mesh_fixture && table.name == "mesh_attr") {
    for (std::size_t index = 0; index < 2U; ++index) {
      append_tuple([&](std::span<std::byte> tuple) {
        for (const auto& field : schema.table_fields(table)) {
          if (field.name == "id")
            write_u32(tuple, field.offset, 907U + static_cast<std::uint32_t>(index));
          if (field.name == "diameters_id") write_u32(tuple, field.offset, 905U);
          if (field.name == "spacings_id") write_u32(tuple, field.offset, 906U);
          if (field.name == "width") write_scalar(tuple, field, index == 0U ? 1000.0 : 1800.0);
          if (field.name == "height") write_scalar(tuple, field, index == 0U ? 600.0 : 500.0);
          if (field.name == "longit_overhang_left") write_scalar(tuple, field, 50.0);
          if (field.name == "cross_overhang_left") write_scalar(tuple, field, 100.0);
          if (field.name == "mesh_flags") write_u32(tuple, field.offset, 0U);
        }
      });
    }
  } else if (table.name == "double_array") {
    std::vector<const tekla::db1::detail::FieldSchema*> values;
    for (const auto& field : schema.table_fields(table)) {
      if (field.name.starts_with("value_")) values.push_back(&field);
    }
    const auto append_array = [&](std::uint32_t id, std::uint32_t next_id, std::size_t value_count,
                                  double value) {
      append_tuple([&](std::span<std::byte> tuple) {
        for (const auto& field : schema.table_fields(table)) {
          if (field.name == "id") write_u32(tuple, field.offset, id);
          if (field.name == "next_id") write_u32(tuple, field.offset, next_id);
          if (field.name == "n_values") {
            write_u32(tuple, field.offset, static_cast<std::uint32_t>(value_count));
          }
        }
        for (std::size_t index = 0; index < value_count; ++index) {
          write_scalar(tuple, *values[index], value);
        }
      });
    };
    if (mesh_fixture) {
      const auto append_values = [&](std::uint32_t id, std::span<const double> source) {
        std::size_t offset = 0U;
        auto current_id = id;
        while (offset < source.size()) {
          const auto count = std::min(values.size(), source.size() - offset);
          const auto next_id = offset + count == source.size() ? 0U : current_id + 1U;
          append_tuple([&](std::span<std::byte> tuple) {
            for (const auto& field : schema.table_fields(table)) {
              if (field.name == "id") write_u32(tuple, field.offset, current_id);
              if (field.name == "next_id") write_u32(tuple, field.offset, next_id);
              if (field.name == "n_values")
                write_u32(tuple, field.offset, static_cast<std::uint32_t>(count));
            }
            for (std::size_t index = 0; index < count; ++index)
              write_scalar(tuple, *values[index], source[offset + index]);
          });
          offset += count;
          ++current_id;
        }
      };
      constexpr std::array<double, 15> polygon_mesh{
          0.0, 0.0, 0.0, 1000.0, 0.0, 0.0, 1000.0, 600.0, 0.0, 0.0, 600.0, 0.0, 0.0, 0.0, 0.0};
      constexpr std::array<double, 12> polygon_geometry{};
      constexpr std::array<double, 9> bent_polygon{0.0, 0.0,    0.0,    1000.0, 0.0,
                                                   0.0, 1000.0, 1000.0, 0.0};
      constexpr std::array<double, 1> bend_radius{100.0};
      constexpr std::array<double, 12> bent_geometry{0.0, 0.0, 0.0, 0.0, 0.0, 500.0,
                                                     0.0, 0.0, 0.0, 0.0, 0.0, 0.0};
      append_values(10'000U, polygon_mesh);
      append_values(11'000U, polygon_geometry);
      append_values(12'000U, bent_polygon);
      append_values(13'000U, bend_radius);
      append_values(14'000U, bent_geometry);
    } else if (oversized_incomplete_group) {
      append_array(10'000U, 0U, 6U, 0.0);
      append_array(20'001U, 0U, 11U, 0.0);
      for (std::size_t index = 0; index < array_count; ++index) {
        const auto array_id = 30'000U + static_cast<std::uint32_t>(index);
        const auto next_id = index + 1U == array_count ? 99'999U : array_id + 1U;
        append_array(array_id, next_id, 1U, 100.0);
      }
    } else {
      for (std::size_t index = 0; index < array_count; ++index) {
        const auto array_id = 10'000U + static_cast<std::uint32_t>(index);
        append_tuple([&](std::span<std::byte> tuple) {
          for (const auto& field : schema.table_fields(table)) {
            if (field.name == "id") write_u32(tuple, field.offset, array_id);
            if (field.name == "next_id") {
              write_u32(tuple, field.offset, index + 1U == array_count ? 0U : array_id + 1U);
            }
            if (field.name == "n_values") {
              write_u32(tuple, field.offset,
                        malformed_group ? 6U : static_cast<std::uint32_t>(values.size()));
            }
          }
          const auto value_count = malformed_group ? 6U : values.size();
          for (std::size_t value_index = 0; value_index < value_count; ++value_index) {
            write_scalar(tuple, *values[value_index], static_cast<double>(value_index));
          }
        });
      }
    }
    if (malformed_group) {
      append_array(20'000U, 0U, 1U, 100.0);
      append_array(20'001U, 0U, 11U, 0.0);
    }
  } else if ((malformed_group || oversized_incomplete_group) && table.name == "int_array") {
    std::vector<const tekla::db1::detail::FieldSchema*> values;
    for (const auto& field : schema.table_fields(table)) {
      if (field.name.starts_with("value_")) values.push_back(&field);
    }
    append_tuple([&](std::span<std::byte> tuple) {
      for (const auto& field : schema.table_fields(table)) {
        if (field.name == "id") write_u32(tuple, field.offset, 904U);
        if (field.name == "n_values") write_u32(tuple, field.offset, 3U);
      }
      write_u32(tuple, values[0]->offset, 1U);
      write_u32(tuple, values[1]->offset, 0U);
      write_u32(tuple, values[2]->offset, 1U);
    });
  } else if (table.name == "rebar") {
    for (std::size_t index = 0; index < rebar_count; ++index) {
      const auto rebar_id = 1201U + static_cast<std::uint32_t>(index);
      append_tuple([&](std::span<std::byte> tuple) {
        for (const auto& field : schema.table_fields(table)) {
          if (field.name == "id") write_u32(tuple, field.offset, rebar_id);
          if (field.name == "rebar_attr_id") write_u32(tuple, field.offset, 902U);
          if (field.name == "mesh_attr_id" && mesh_fixture)
            write_u32(tuple, field.offset, 907U + static_cast<std::uint32_t>(index));
          if (field.name == "polygon_id")
            write_u32(tuple, field.offset, mesh_fixture && index == 1U ? 12'000U : 10'000U);
          if (field.name == "bending_id" && mesh_fixture && index == 1U)
            write_u32(tuple, field.offset, 13'000U);
          if (field.name == "spacing_id" && (malformed_group || oversized_incomplete_group)) {
            write_u32(tuple, field.offset, oversized_incomplete_group ? 30'000U : 20'000U);
          }
          if (field.name == "offset_id" && (malformed_group || oversized_incomplete_group)) {
            write_u32(tuple, field.offset, 20'001U);
          }
          if (field.name == "offset_id" && mesh_fixture)
            write_u32(tuple, field.offset, index == 0U ? 11'000U : 14'000U);
        }
      });
    }
  }

  bytes.push_back(std::byte{0});
  bytes.insert(bytes.end(), final ? final_footer.begin() : table_end.begin(),
               final ? final_footer.end() : table_end.end());
}

std::vector<std::byte> database_with_shared_rebar_polygon(std::size_t rebar_count,
                                                          std::size_t array_count,
                                                          bool malformed_group = false,
                                                          bool oversized_incomplete_group = false,
                                                          bool mesh_fixture = false) {
  constexpr std::array<std::byte, 4> table_end{std::byte{0x66}, std::byte{0xc0}, std::byte{0xce},
                                               std::byte{0xdb}};
  const auto* schema = tekla::db1::detail::schema_for("9.66", 0x85);
  CHECK(schema != nullptr, "the generated 9.66 schema is registered for rebar budget tests");
  std::vector<std::byte> bytes;
  append_ascii(bytes, "Xsteel");
  bytes.push_back(std::byte{0x85});
  append_ascii(bytes, " 9.66 7d72d8c9-0250-4f3a-8760-bcef517f016e");
  append_u32(bytes, 1U);
  bytes.insert(bytes.end(), table_end.begin(), table_end.end());
  if (schema != nullptr) {
    for (std::size_t index = 0; index < schema->tables.size(); ++index) {
      append_shared_rebar_table(bytes, *schema, schema->tables[index],
                                index + 1U == schema->tables.size(), rebar_count, array_count,
                                malformed_group, oversized_incomplete_group, mesh_fixture);
    }
  }
  return bytes;
}

std::vector<std::byte> database_with_classic_rebar_meshes() {
  return database_with_shared_rebar_polygon(2U, 0U, false, false, true);
}

#if defined(TEKLA_DB1_TEST_HAS_OCCT)
void append_boolean_chain_table(std::vector<std::byte>& bytes,
                                const tekla::db1::detail::Schema& schema,
                                const tekla::db1::detail::TableSchema& table, bool final,
                                std::size_t part_count, bool cycle, bool shared,
                                bool boolean_part_operands, bool swept_root,
                                bool additive_operand) {
  constexpr std::array<std::byte, 4> table_end{std::byte{0x66}, std::byte{0xc0}, std::byte{0xce},
                                               std::byte{0xdb}};
  constexpr std::array<std::byte, 4> final_footer{std::byte{0x4f}, std::byte{0x61}, std::byte{0xbc},
                                                  std::byte{0x00}};
  const bool custom = table.name == "relation" || table.name == "part" ||
                      table.name == "part_attr" || table.name == "old_part_attr_911" ||
                      table.name == "coordsys_attr";
  if (!custom) {
    append_table(bytes, schema, table, final, swept_root ? "D32" : "200*10", "14", false,
                 swept_root ? 4U : 7U, false, swept_root);
    return;
  }

  append_u32(bytes, table.tuple_size);
  append_u32(bytes, table.descriptor_count);
  for (const auto descriptor : schema.table_descriptors(table)) append_u32(bytes, descriptor);
  const auto append_tuple = [&](const auto& write) {
    bytes.push_back(std::byte{0});
    const auto tuple_offset = bytes.size();
    bytes.resize(bytes.size() + table.tuple_size, std::byte{0});
    auto tuple = std::span<std::byte>(bytes).subspan(tuple_offset, table.tuple_size);
    write(tuple);
    append_u32(bytes, 2000U + static_cast<std::uint32_t>(tuple_offset));
    append_u32(bytes, 3000U + static_cast<std::uint32_t>(tuple_offset));
  };

  if (table.name == "relation") {
    constexpr std::array<std::array<std::uint32_t, 2>, 15> shared_relations{{
        {1201U, 1203U},
        {1201U, 1203U},
        {1202U, 1203U},
        {1202U, 1203U},
        {1203U, 1204U},
        {1201U, 1205U},
        {1202U, 1205U},
        {1205U, 1206U},
        {1201U, 1207U},
        {1202U, 1207U},
        {1207U, 1208U},
        {1208U, 1207U},
        {1201U, 1209U},
        {1202U, 1209U},
        {1209U, 1210U},
    }};
    const std::size_t relation_count = shared ? shared_relations.size() : part_count - 1U;
    for (std::size_t index = 0; index < relation_count; ++index) {
      const auto id_offset = static_cast<std::uint32_t>(index);
      append_tuple([&](std::span<std::byte> tuple) {
        for (const auto& field : schema.table_fields(table)) {
          if (field.name == "id") write_u32(tuple, field.offset, 800U + id_offset);
          if (field.name == "type") write_u32(tuple, field.offset, additive_operand ? 38U : 11U);
          if (field.name == "id1") {
            write_u32(tuple, field.offset, shared ? shared_relations[index][0] : 1201U + id_offset);
          }
          if (field.name == "id2") {
            write_u32(tuple, field.offset, shared ? shared_relations[index][1] : 1202U + id_offset);
          }
        }
      });
    }
    if (cycle && part_count >= 3U) {
      append_tuple([&](std::span<std::byte> tuple) {
        for (const auto& field : schema.table_fields(table)) {
          if (field.name == "id") write_u32(tuple, field.offset, 899U);
          if (field.name == "type") write_u32(tuple, field.offset, 11U);
          if (field.name == "id1") {
            write_u32(tuple, field.offset, 1200U + static_cast<std::uint32_t>(part_count));
          }
          if (field.name == "id2") write_u32(tuple, field.offset, 1202U);
        }
      });
    }
  } else if (table.name == "part") {
    for (std::size_t index = 0; index < part_count; ++index) {
      const auto id_offset = static_cast<std::uint32_t>(index);
      append_tuple([&](std::span<std::byte> tuple) {
        for (const auto& field : schema.table_fields(table)) {
          if (field.name == "id") write_u32(tuple, field.offset, 1201U + id_offset);
          if (field.name == "part_attr_id") {
            write_u32(tuple, field.offset,
                      (boolean_part_operands || swept_root || additive_operand) && index > 0U
                          ? 902U
                          : 900U);
          }
          if (field.name == "csys_attr_id") write_u32(tuple, field.offset, 901U);
          if (field.name == "p1")
            write_u32(tuple, field.offset, swept_root && index == 0U ? 970U : 0U);
          if (field.name == "GeometryTreeRootId") {
            write_u32(tuple, field.offset, swept_root && index == 0U ? 950U : 0U);
          }
          if (field.name == "csys_x") {
            write_f64(tuple, field.offset,
                      additive_operand && index > 0U ? 910.0
                      : shared && index < 2U         ? 10.0
                                                     : 10.0 + 100.0 * static_cast<double>(index));
          }
          if (field.name == "csys_y") write_f64(tuple, field.offset, 20.0);
          if (field.name == "csys_z") write_f64(tuple, field.offset, 30.0);
          if (field.name == "csys_length") {
            write_f64(tuple, field.offset, index == 0U || (shared && index == 1U) ? 1000.0 : 250.0);
          }
        }
      });
    }
  } else if (table.name == "part_attr" || table.name == "old_part_attr_911") {
    const auto append_attribute = [&](std::uint32_t id, std::uint32_t object_type, bool polybeam) {
      append_tuple([&](std::span<std::byte> tuple) {
        for (const auto& field : schema.table_fields(table)) {
          if (field.name == "id") write_u32(tuple, field.offset, id);
          if (field.name == "form_type") write_u32(tuple, field.offset, polybeam ? 4U : 7U);
          if (field.name == "obj_type") write_u32(tuple, field.offset, object_type);
          if (field.name == "ben") write_fixed(tuple, field.offset, field.size, "Boolean fixture");
          if (field.name == "prof" || field.name == "Geometry") {
            write_fixed(tuple, field.offset, field.size,
                        polybeam                         ? "D32"
                        : additive_operand && id == 902U ? "O200*20"
                                                         : "200*200");
          }
          if (field.name == "mat") write_fixed(tuple, field.offset, field.size, "S355");
        }
      });
    };
    append_attribute(900U, 2U, swept_root);
    if (boolean_part_operands || swept_root || additive_operand) {
      append_attribute(902U, additive_operand ? 38U : boolean_part_operands ? 11U : 2U, false);
    }
  } else if (table.name == "coordsys_attr") {
    append_tuple([&](std::span<std::byte> tuple) {
      for (const auto& field : schema.table_fields(table)) {
        if (field.name == "id") write_u32(tuple, field.offset, 901U);
        if (field.name == "xdir_x") write_f64(tuple, field.offset, 1.0);
        if (field.name == "ydir_y") write_f64(tuple, field.offset, 1.0);
      }
    });
  }

  bytes.push_back(std::byte{0});
  bytes.insert(bytes.end(), final ? final_footer.begin() : table_end.begin(),
               final ? final_footer.end() : table_end.end());
}

std::vector<std::byte> database_with_boolean_chain(std::size_t part_count, bool cycle = false,
                                                   bool shared = false,
                                                   bool boolean_part_operands = false,
                                                   bool swept_root = false,
                                                   bool additive_operand = false) {
  constexpr std::array<std::byte, 4> table_end{std::byte{0x66}, std::byte{0xc0}, std::byte{0xce},
                                               std::byte{0xdb}};
  const auto* schema = tekla::db1::detail::schema_for("9.66", 0x85);
  CHECK(schema != nullptr, "the generated 9.66 schema is registered for Boolean graph tests");
  std::vector<std::byte> bytes;
  append_ascii(bytes, "Xsteel");
  bytes.push_back(std::byte{0x85});
  append_ascii(bytes, " 9.66 7d72d8c9-0250-4f3a-8760-bcef517f016e");
  append_u32(bytes, 1U);
  bytes.insert(bytes.end(), table_end.begin(), table_end.end());
  if (schema != nullptr) {
    for (std::size_t index = 0; index < schema->tables.size(); ++index) {
      append_boolean_chain_table(bytes, *schema, schema->tables[index],
                                 index + 1U == schema->tables.size(), part_count, cycle, shared,
                                 boolean_part_operands, swept_root, additive_operand);
    }
  }
  return bytes;
}
#endif

std::vector<std::byte> database_with_one_legacy_object() {
  constexpr std::array<std::byte, 4> table_end{std::byte{0x66}, std::byte{0xc0}, std::byte{0xce},
                                               std::byte{0xdb}};
  const auto* schema = tekla::db1::detail::schema_for("8.95", 0x85);
  CHECK(schema != nullptr, "the generated 8.95 schema is registered");
  std::vector<std::byte> bytes;
  append_ascii(bytes, "Xsteel");
  bytes.push_back(std::byte{0x85});
  append_ascii(bytes, " 8.95 7d72d8c9-0250-4f3a-8760-bcef517f016e");
  append_u32(bytes, 1);
  bytes.insert(bytes.end(), table_end.begin(), table_end.end());
  if (schema != nullptr) {
    for (std::size_t index = 0; index < schema->tables.size(); ++index) {
      append_table(bytes, *schema, schema->tables[index], index + 1 == schema->tables.size());
    }
  }
  return bytes;
}

std::vector<std::uint64_t> display_signature(const tekla::db1::Model& model) {
  tekla::db1::ProcessRequest request;
  request.stages = tekla::db1::Stage::display_geometry;
  auto processed = model.process(request);
  if (!processed) return {};
  std::vector<std::uint64_t> signature;
  while (true) {
    auto batch = processed.value()->next();
    if (!batch) return {};
    signature.push_back(static_cast<std::uint64_t>(batch.value().kind));
    if (batch.value().kind == tekla::db1::BatchKind::end) break;
    for (const auto& mesh : batch.value().meshes) {
      signature.push_back(mesh.object_id);
      signature.push_back(mesh.positions.size());
      signature.push_back(mesh.indices.size());
      for (const float coordinate : mesh.positions) {
        signature.push_back(std::bit_cast<std::uint32_t>(coordinate));
      }
      signature.insert(signature.end(), mesh.indices.begin(), mesh.indices.end());
    }
  }
  return signature;
}

bool reconstructs_model_mesh(const tekla::db1::MeshView& local_mesh,
                             const tekla::db1::MeshView& model_mesh) {
  if (local_mesh.positions.size() != model_mesh.positions.size() ||
      local_mesh.indices.size() != model_mesh.indices.size() ||
      !std::equal(local_mesh.indices.begin(), local_mesh.indices.end(), model_mesh.indices.begin(),
                  model_mesh.indices.end())) {
    return false;
  }
  for (std::size_t index = 0U; index + 2U < local_mesh.positions.size(); index += 3U) {
    const auto& placement = local_mesh.placement;
    const tekla::db1::Vector3d reconstructed{
        placement.origin.x + placement.x_axis.x * local_mesh.positions[index] +
            placement.y_axis.x * local_mesh.positions[index + 1U] +
            placement.z_axis.x * local_mesh.positions[index + 2U],
        placement.origin.y + placement.x_axis.y * local_mesh.positions[index] +
            placement.y_axis.y * local_mesh.positions[index + 1U] +
            placement.z_axis.y * local_mesh.positions[index + 2U],
        placement.origin.z + placement.x_axis.z * local_mesh.positions[index] +
            placement.y_axis.z * local_mesh.positions[index + 1U] +
            placement.z_axis.z * local_mesh.positions[index + 2U]};
    if (std::abs(reconstructed.x - model_mesh.positions[index]) >= 1.0e-6 ||
        std::abs(reconstructed.y - model_mesh.positions[index + 1U]) >= 1.0e-6 ||
        std::abs(reconstructed.z - model_mesh.positions[index + 2U]) >= 1.0e-6) {
      return false;
    }
  }
  return true;
}

}  // namespace

int main([[maybe_unused]] int argc, [[maybe_unused]] char** argv) {
  using namespace tekla::db1;
  CHECK(ProcessRequest{}.mesh_coordinate_mode == MeshCoordinateMode::model_space,
        "display meshes remain in model space unless local placement is requested");
  CHECK(object_role(ObjectKind::beam) == ObjectRole::model_element,
        "ordinary parts are independently publishable model elements");
  CHECK(object_role(ObjectKind::component) == ObjectRole::model_element,
        "component occurrences are independently publishable model elements");
  CHECK(object_role(ObjectKind::boolean_part) == ObjectRole::evaluation_feature &&
            object_role(ObjectKind::cut_plane) == ObjectRole::evaluation_feature,
        "Boolean parts and cut planes are geometry-evaluation features");
  const auto bytes = database_with_one_object();
  ModelPackage package;
  package.add(Asset::copy(AssetRole::model_database, "identity.db1", bytes));
  auto model = open(std::move(package));
  CHECK(model.has_value(), "a schema-conforming database opens");
  if (model) {
    ProcessRequest request;
    request.stages = Stage::identities;
    auto processed = model.value().process(request);
    CHECK(processed.has_value(), "identity-only processing is available");
    if (processed) {
      auto first = processed.value()->next();
      CHECK(first.has_value() && first.value().kind == BatchKind::objects,
            "the first semantic batch contains objects");
      if (first && first.value().kind == BatchKind::objects) {
        CHECK(first.value().objects.size() == 1, "one object identity is emitted");
        if (!first.value().objects.empty()) {
          const auto& object = first.value().objects.front();
          CHECK(object.internal_id == 1201, "the internal ID is decoded");
          CHECK(object.application_id == "12345678-1234-4abc-8def-123456789abc",
                "the RFC-4122 application ID is decoded");
          CHECK(object.type == 1 && object.subtype == 22, "object type identity is decoded");
          CHECK(object.kind == ObjectKind::unknown,
                "unknown raw identities remain explicitly unclassified");
          CHECK(object.assembly_id == 700, "assembly identity is decoded");
          CHECK(object.row_id == 91 && object.event_id == 92, "physical row identity is retained");
        }
      }
      auto end = processed.value()->next();
      CHECK(end.has_value() && end.value().kind == BatchKind::end,
            "the identity stream terminates explicitly");
    }
  }

  const auto weld_bytes = database_with_one_object("200*10", "14", "9.66", false, 7U, false, false,
                                                   false, false, false, false, false, true);
  ModelPackage weld_package;
  weld_package.add(Asset::copy(AssetRole::model_database, "weld.db1", weld_bytes));
  auto weld_model = open(std::move(weld_package));
  CHECK(weld_model.has_value(), "the workshop-weld fixture opens");
  if (weld_model) {
    ProcessRequest request;
    request.stages = Stage::identities;
    auto processed = weld_model.value().process(request);
    CHECK(processed.has_value(), "workshop-weld identity processing is available");
    if (processed) {
      auto batch = processed.value()->next();
      bool saw_workshop = false;
      bool saw_site = false;
      if (batch && batch.value().kind == BatchKind::objects) {
        for (const auto& object : batch.value().objects) {
          saw_workshop |= object.internal_id == 1201U && object.kind == ObjectKind::weld &&
                          object.weld_location == WeldLocation::workshop;
          saw_site |= object.internal_id == 1202U && object.kind == ObjectKind::weld &&
                      object.weld_location == WeldLocation::site;
        }
      }
      CHECK(saw_workshop && saw_site,
            "welding common-attribute references distinguish workshop and site welds");
    }

    ProcessRequest property_request;
    property_request.stages = Stage::properties;
    property_request.batch_memory_budget_bytes = sizeof(PropertyView) * 11U;
    auto properties = weld_model.value().process(property_request);
    CHECK(properties.has_value(), "weld property processing is available without part rows");
    bool saw_shop = false;
    bool saw_around = false;
    bool saw_compound = false;
    bool saw_logical = false;
    bool saw_intermittent_type = false;
    bool saw_size_above = false;
    bool saw_type_above = false;
    bool saw_intermittent_above = false;
    bool saw_size_below = false;
    bool saw_type_below = false;
    bool saw_intermittent_below = false;
    if (properties) {
      while (true) {
        auto batch = properties.value()->next();
        CHECK(batch.has_value(), "weld property batches decode without an error");
        if (!batch || batch.value().kind == BatchKind::end) break;
        if (batch.value().kind != BatchKind::properties) continue;
        std::unordered_set<std::uint64_t> batch_welds;
        for (const auto& property : batch.value().properties) {
          if (property.group == "Tekla" && property.name.starts_with("weld")) {
            batch_welds.insert(property.object_id);
          }
          if (property.object_id != 1201U || property.group != "Tekla") {
            continue;
          }
          saw_shop |= property.name == "weldShop" && property.kind == PropertyValueKind::integer &&
                      property.integer_value == 1;
          saw_around |= property.name == "weldAround" &&
                        property.kind == PropertyValueKind::integer && property.integer_value == 1;
          saw_compound |= property.name == "weldCompound" &&
                          property.kind == PropertyValueKind::integer &&
                          property.integer_value == 1;
          saw_logical |= property.name == "weldLogical" &&
                         property.kind == PropertyValueKind::integer && property.integer_value == 0;
          saw_intermittent_type |= property.name == "weldIntermittentType" &&
                                   property.kind == PropertyValueKind::integer &&
                                   property.integer_value == 2;
          saw_size_above |= property.name == "weldSizeAbove" &&
                            property.kind == PropertyValueKind::floating &&
                            property.floating_value == 5.0;
          saw_type_above |= property.name == "weldTypeAbove" &&
                            property.kind == PropertyValueKind::integer &&
                            property.integer_value == 10;
          saw_intermittent_above |= property.name == "weldIntermittentAbove" &&
                                    property.kind == PropertyValueKind::integer &&
                                    property.integer_value == 1;
          saw_size_below |= property.name == "weldSizeBelow" &&
                            property.kind == PropertyValueKind::floating &&
                            property.floating_value == 3.0;
          saw_type_below |= property.name == "weldTypeBelow" &&
                            property.kind == PropertyValueKind::integer &&
                            property.integer_value == 6;
          saw_intermittent_below |= property.name == "weldIntermittentBelow" &&
                                    property.kind == PropertyValueKind::integer &&
                                    property.integer_value == 0;
        }
        CHECK(batch_welds.size() <= 1U,
              "a one-occurrence weld property budget bounds each reusable output batch");
      }
    }
    CHECK(saw_shop && saw_around && saw_compound && saw_logical && saw_intermittent_type,
          "persisted common weld semantics are ordinary Tekla properties");
    CHECK(saw_size_above && saw_type_above && saw_intermittent_above && saw_size_below &&
              saw_type_below && saw_intermittent_below,
          "linked weld seams expose native above/below size, type, and intermittent values");
  }

  const auto polygon_weld_bytes = database_with_polygon_weld_geometry();
  ModelPackage polygon_weld_package;
  polygon_weld_package.add(
      Asset::copy(AssetRole::model_database, "polygon-weld.db1", polygon_weld_bytes));
  auto polygon_weld_model = open(std::move(polygon_weld_package));
  CHECK(polygon_weld_model.has_value(), "a persisted polygon-weld database opens");
  if (polygon_weld_model) {
    ProcessRequest request;
    request.stages = Stage::display_geometry;
    auto processed = polygon_weld_model.value().process(request);
    CHECK(processed.has_value(), "polygon-weld display processing is available");
    bool saw_weld_mesh = false;
    bool saw_polygon_record_mesh = false;
    if (processed) {
      while (true) {
        auto batch = processed.value()->next();
        CHECK(batch.has_value(), "polygon-weld display batches decode");
        if (!batch || batch.value().kind == BatchKind::end) break;
        for (const auto& mesh : batch.value().meshes) {
          saw_polygon_record_mesh = saw_polygon_record_mesh || mesh.object_id == 950U;
          if (mesh.object_id != 1201U || mesh.positions.empty()) continue;
          std::array<float, 6> bounds{mesh.positions[0], mesh.positions[1], mesh.positions[2],
                                      mesh.positions[0], mesh.positions[1], mesh.positions[2]};
          for (std::size_t index = 3U; index < mesh.positions.size(); index += 3U) {
            for (std::size_t axis = 0; axis < 3U; ++axis) {
              bounds[axis] = std::min(bounds[axis], mesh.positions[index + axis]);
              bounds[axis + 3U] = std::max(bounds[axis + 3U], mesh.positions[index + axis]);
            }
          }
          const bool indices_valid =
              mesh.indices.size() % 3U == 0U &&
              std::all_of(mesh.indices.begin(), mesh.indices.end(),
                          [&](std::uint32_t index) { return index < mesh.positions.size() / 3U; });
          constexpr std::array<float, 6> expected_bounds{95.0F,  200.0F, 300.0F,
                                                         100.0F, 210.0F, 305.0F};
          saw_weld_mesh = mesh.positions.size() == 18U && mesh.indices.size() == 24U &&
                          indices_valid && bounds == expected_bounds;
        }
      }
    }
    CHECK(saw_weld_mesh,
          "a persisted fillet path becomes a bounded triangular mesh on the weld object");
    CHECK(!saw_polygon_record_mesh,
          "polygon storage IDs remain internal and do not become display owners");
  }

  const auto surface_treatment_bytes = database_with_surface_treatment();
  ModelPackage surface_treatment_package;
  surface_treatment_package.add(
      Asset::copy(AssetRole::model_database, "surface-treatment.db1", surface_treatment_bytes));
  auto surface_treatment_model = open(std::move(surface_treatment_package));
  CHECK(surface_treatment_model.has_value(), "a persisted surface-treatment database opens");
  if (surface_treatment_model) {
    ProcessRequest request;
    request.stages =
        Stage::identities | Stage::properties | Stage::semantic_relations | Stage::display_geometry;
    auto processed = surface_treatment_model.value().process(request);
    CHECK(processed.has_value(), "surface-treatment processing is available");
    if (!processed)
      std::printf("surface-treatment process error: %s\n", processed.error().message.c_str());
    bool saw_identity = false;
    bool saw_name = false;
    bool saw_thickness = false;
    bool saw_material = false;
    bool saw_father = false;
    bool saw_mesh = false;
    if (processed) {
      while (true) {
        auto batch = processed.value()->next();
        CHECK(batch.has_value(), "surface-treatment batches decode");
        if (!batch || batch.value().kind == BatchKind::end) break;
        for (const auto& object : batch.value().objects) {
          saw_identity |=
              object.internal_id == 1301U && object.kind == ObjectKind::surface_treatment;
        }
        for (const auto& property : batch.value().properties) {
          saw_name |= property.object_id == 1301U && property.name == "name" &&
                      property.text_value == "SURFACE-GRATING";
          saw_thickness |= property.object_id == 1301U && property.name == "thickness" &&
                           property.kind == PropertyValueKind::floating &&
                           property.floating_value == 1.0;
        }
        for (const auto& material : batch.value().materials) {
          saw_material |= material.object_id == 1301U && material.name == "Zero_Density" &&
                          material.finish == "TS8 - Open-Mesh";
        }
        for (const auto& relation : batch.value().semantic_relations) {
          saw_father |= relation.kind == SemanticRelationKind::subelement &&
                        relation.source_id == 1201U && relation.target_id == 1301U &&
                        relation.source_relation_id == 960U;
        }
        for (const auto& mesh : batch.value().meshes) {
          if (mesh.object_id != 1301U || mesh.positions.empty()) continue;
          std::array<float, 6> bounds{mesh.positions[0], mesh.positions[1], mesh.positions[2],
                                      mesh.positions[0], mesh.positions[1], mesh.positions[2]};
          for (std::size_t index = 3U; index < mesh.positions.size(); index += 3U) {
            for (std::size_t axis = 0U; axis < 3U; ++axis) {
              bounds[axis] = std::min(bounds[axis], mesh.positions[index + axis]);
              bounds[axis + 3U] = std::max(bounds[axis + 3U], mesh.positions[index + axis]);
            }
          }
          const bool indices_valid =
              mesh.indices.size() == 36U &&
              std::all_of(mesh.indices.begin(), mesh.indices.end(),
                          [&](std::uint32_t index) { return index < mesh.positions.size() / 3U; });
          double signed_volume = 0.0;
          if (indices_valid) {
            for (std::size_t index = 0U; index < mesh.indices.size(); index += 3U) {
              const auto point = [&](std::uint32_t vertex, std::size_t axis) {
                return static_cast<double>(
                    mesh.positions[static_cast<std::size_t>(vertex) * 3U + axis]);
              };
              const auto a = mesh.indices[index];
              const auto b = mesh.indices[index + 1U];
              const auto c = mesh.indices[index + 2U];
              signed_volume +=
                  (point(a, 0U) * (point(b, 1U) * point(c, 2U) - point(b, 2U) * point(c, 1U)) -
                   point(a, 1U) * (point(b, 0U) * point(c, 2U) - point(b, 2U) * point(c, 0U)) +
                   point(a, 2U) * (point(b, 0U) * point(c, 1U) - point(b, 1U) * point(c, 0U))) /
                  6.0;
            }
          }
          saw_mesh = mesh.positions.size() == 24U && indices_valid &&
                     std::abs(signed_volume - 252500.0) <= 1.0e-6 &&
                     bounds == std::array<float, 6>{0.0F, 0.0F, -1.0F, 250.0F, 1010.0F, 0.0F};
        }
      }
    }
    CHECK(saw_identity, "a persisted type-73 row has a stable surface-treatment identity");
    CHECK(saw_name && saw_thickness && saw_material,
          "surface-treatment attributes expose name, thickness, and material semantics");
    CHECK(saw_father, "the persisted father relation owns the surface treatment");
    CHECK(saw_mesh, "a tile surface becomes its persisted one-millimetre treatment solid");

    ProcessRequest local_request;
    local_request.stages = Stage::display_geometry;
    local_request.mesh_coordinate_mode = MeshCoordinateMode::local_with_rigid_placement;
    auto local_processed = surface_treatment_model.value().process(local_request);
    CHECK(local_processed.has_value(), "surface-treatment local-mesh processing is available");
    bool saw_local_mesh = false;
    if (local_processed) {
      while (true) {
        auto batch = local_processed.value()->next();
        CHECK(batch.has_value(), "surface-treatment local-mesh batches decode");
        if (!batch || batch.value().kind == BatchKind::end) break;
        for (const auto& mesh : batch.value().meshes) {
          if (mesh.object_id != 1301U || mesh.positions.empty()) continue;
          std::array<float, 6> bounds{mesh.positions[0], mesh.positions[1], mesh.positions[2],
                                      mesh.positions[0], mesh.positions[1], mesh.positions[2]};
          for (std::size_t index = 3U; index < mesh.positions.size(); index += 3U) {
            for (std::size_t axis = 0U; axis < 3U; ++axis) {
              bounds[axis] = std::min(bounds[axis], mesh.positions[index + axis]);
              bounds[axis + 3U] = std::max(bounds[axis + 3U], mesh.positions[index + axis]);
            }
          }
          saw_local_mesh =
              mesh.coordinate_space == MeshCoordinateMode::local_with_rigid_placement &&
              bounds == std::array<float, 6>{0.0F, 0.0F, -1.0F, 250.0F, 1010.0F, 0.0F} &&
              mesh.placement.origin.x == 0.0 && mesh.placement.origin.y == 0.0 &&
              mesh.placement.origin.z == 0.0 && mesh.placement.x_axis.x == 1.0 &&
              mesh.placement.y_axis.y == 1.0 && mesh.placement.z_axis.z == 1.0;
        }
      }
    }
    CHECK(saw_local_mesh,
          "surface treatments expose reusable local geometry with a rigid model placement");
  }

  const auto pour_bytes = database_with_pour_objects();
  ModelPackage pour_package;
  pour_package.add(Asset::copy(AssetRole::model_database, "pours.db1", pour_bytes));
  auto pour_model = open(std::move(pour_package));
  CHECK(pour_model.has_value(), "a persisted pour-object database opens");
  if (pour_model) {
    ProcessRequest request;
    request.stages = Stage::identities | Stage::properties | Stage::semantic_relations;
    auto processed = pour_model.value().process(request);
    CHECK(processed.has_value(), "pour object and unit processing is available");
    bool saw_pour_object = false;
    bool saw_pour_unit = false;
    bool saw_unrelated_subtype = false;
    bool saw_number = false;
    bool saw_type = false;
    bool saw_mixture = false;
    bool saw_unit_name = false;
    bool saw_membership = false;
    if (processed) {
      while (true) {
        auto batch = processed.value()->next();
        CHECK(batch.has_value(), "pour batches decode");
        if (!batch || batch.value().kind == BatchKind::end) break;
        for (const auto& object : batch.value().objects) {
          saw_pour_object |= object.internal_id == 1401U && object.kind == ObjectKind::pour_object;
          saw_pour_unit |= object.internal_id == 1402U && object.kind == ObjectKind::pour_unit;
          saw_unrelated_subtype |=
              object.internal_id == 1403U && object.kind == ObjectKind::unknown;
        }
        for (const auto& property : batch.value().properties) {
          saw_number |= property.object_id == 1401U && property.name == "pourNumber" &&
                        property.text_value == "POUR-42";
          saw_type |= property.object_id == 1401U && property.name == "pourType" &&
                      property.text_value == "Bridge deck";
          saw_mixture |= property.object_id == 1401U && property.name == "concreteMixture" &&
                         property.text_value == "C35/45";
          saw_unit_name |= property.object_id == 1402U && property.name == "name" &&
                           property.text_value == "Deck pour unit";
        }
        for (const auto& relation : batch.value().semantic_relations) {
          saw_membership |= relation.kind == SemanticRelationKind::in_assembly &&
                            relation.source_id == 1401U && relation.target_id == 1402U &&
                            relation.origin == SemanticRelationOrigin::pour_membership;
        }
      }
    }
    CHECK(saw_pour_object && saw_pour_unit,
          "persisted type-90 and type-101 rows have stable pour identities");
    CHECK(saw_unrelated_subtype, "unproven type-90 subtypes remain unclassified");
    CHECK(saw_number && saw_type && saw_mixture && saw_unit_name,
          "pour objects and units expose their persisted exchange semantics");
    CHECK(saw_membership, "a pour object is linked to its persisted pour unit");
  }

  const auto splice_bytes = database_with_rebar_splice();
  ModelPackage splice_package;
  splice_package.add(Asset::copy(AssetRole::model_database, "splice.db1", splice_bytes));
  auto splice_model = open(std::move(splice_package));
  CHECK(splice_model.has_value(), "a persisted rebar-splice database opens");
  if (splice_model) {
    ProcessRequest request;
    request.stages = Stage::identities | Stage::properties | Stage::semantic_relations;
    request.batch_memory_budget_bytes = sizeof(SemanticRelationView);
    auto processed = splice_model.value().process(request);
    CHECK(processed.has_value(), "rebar-splice semantic processing is available");
    bool saw_identity = false;
    bool saw_type = false;
    bool saw_lap = false;
    bool saw_offset = false;
    bool saw_clearance = false;
    bool saw_position = false;
    bool saw_first_end = false;
    bool saw_second_end = false;
    bool saw_first_group = false;
    bool saw_second_group = false;
    if (processed) {
      while (true) {
        auto batch = processed.value()->next();
        CHECK(batch.has_value(), "rebar-splice batches decode");
        if (!batch || batch.value().kind == BatchKind::end) break;
        for (const auto& object : batch.value().objects) {
          saw_identity |= object.internal_id == 1501U && object.kind == ObjectKind::rebar_splice;
        }
        for (const auto& property : batch.value().properties) {
          saw_type |= property.object_id == 1501U && property.name == "spliceType" &&
                      property.integer_value == 2;
          saw_lap |= property.object_id == 1501U && property.name == "lapLength" &&
                     property.floating_value == 600.0;
          saw_offset |= property.object_id == 1501U && property.name == "offset" &&
                        property.floating_value == 12.5;
          saw_clearance |= property.object_id == 1501U && property.name == "clearance" &&
                           property.floating_value == 8.0;
          saw_position |= property.object_id == 1501U && property.name == "barPositions" &&
                          property.integer_value == 1;
          saw_first_end |= property.object_id == 1501U && property.name == "firstEnd" &&
                           property.integer_value == 1;
          saw_second_end |= property.object_id == 1501U && property.name == "secondEnd" &&
                            property.integer_value == 0;
        }
        for (const auto& relation : batch.value().semantic_relations) {
          saw_first_group |= relation.kind == SemanticRelationKind::connects_to &&
                             relation.source_id == 1501U && relation.target_id == 1502U &&
                             relation.ordinal == 0U &&
                             relation.origin == SemanticRelationOrigin::rebar_splice;
          saw_second_group |= relation.kind == SemanticRelationKind::connects_to &&
                              relation.source_id == 1501U && relation.target_id == 1503U &&
                              relation.ordinal == 1U &&
                              relation.origin == SemanticRelationOrigin::rebar_splice;
        }
      }
    }
    CHECK(saw_identity, "persisted type-74 rows have stable rebar-splice identities");
    CHECK(saw_type && saw_lap && saw_offset && saw_clearance && saw_position && saw_first_end &&
              saw_second_end,
          "rebar splices expose their persisted connection semantics");
    CHECK(saw_first_group && saw_second_group,
          "a rebar splice connects to both persisted reinforcement endpoints");
  }

  const auto surface_object_bytes = database_with_surface_object();
  ModelPackage surface_object_package;
  surface_object_package.add(
      Asset::copy(AssetRole::model_database, "surface-object.db1", surface_object_bytes));
  auto surface_object_model = open(std::move(surface_object_package));
  CHECK(surface_object_model.has_value(), "a persisted surface-object database opens");
  if (surface_object_model) {
    ProcessRequest request;
    request.stages = Stage::identities | Stage::properties | Stage::semantic_relations;
    request.batch_memory_budget_bytes = sizeof(PropertyView) * 2U;
    auto processed = surface_object_model.value().process(request);
    CHECK(processed.has_value(), "surface-object semantic processing is available");
    bool saw_identity = false;
    bool saw_name = false;
    bool saw_class = false;
    bool saw_group = false;
    bool saw_geometry_type = false;
    bool saw_layer = false;
    bool saw_offset = false;
    bool saw_surface_type = false;
    bool saw_host = false;
    if (processed) {
      while (true) {
        auto batch = processed.value()->next();
        CHECK(batch.has_value(), "surface-object batches decode");
        if (!batch || batch.value().kind == BatchKind::end) break;
        for (const auto& object : batch.value().objects) {
          saw_identity |= object.internal_id == 1601U && object.kind == ObjectKind::surface_object;
        }
        for (const auto& property : batch.value().properties) {
          saw_name |= property.object_id == 1601U && property.name == "name" &&
                      property.text_value == "Rebar set leg surface";
          saw_class |= property.object_id == 1601U && property.name == "class" &&
                       property.integer_value == 60;
          saw_group |=
              property.object_id == 1601U && property.name == "group" && property.text_value == "4";
          saw_geometry_type |= property.object_id == 1601U && property.name == "geometryType" &&
                               property.integer_value == 2;
          saw_layer |= property.object_id == 1601U && property.name == "layerNumber" &&
                       property.integer_value == 2;
          saw_offset |= property.object_id == 1601U && property.name == "additionalOffset" &&
                        property.floating_value == 12.5;
          saw_surface_type |= property.object_id == 1601U && property.name == "surfaceType" &&
                              property.text_value == "Rebar set leg surface";
        }
        for (const auto& relation : batch.value().semantic_relations) {
          saw_host |= relation.kind == SemanticRelationKind::hosted_on &&
                      relation.source_id == 1601U && relation.target_id == 1602U &&
                      relation.origin == SemanticRelationOrigin::surface_object;
        }
      }
    }
    CHECK(saw_identity, "persisted type-97 rows have stable surface-object identities");
    CHECK(saw_name && saw_class && saw_group && saw_geometry_type && saw_layer && saw_offset &&
              saw_surface_type,
          "surface objects expose their persisted surface semantics");
    CHECK(saw_host, "a surface object is linked to its persisted host part");
  }

  const auto rebar_set_bytes = database_with_rebar_set();
  ModelPackage rebar_set_package;
  rebar_set_package.add(Asset::copy(AssetRole::model_database, "rebar-set.db1", rebar_set_bytes));
  auto rebar_set_model = open(std::move(rebar_set_package));
  CHECK(rebar_set_model.has_value(), "a persisted rebar-set database opens");
  if (rebar_set_model) {
    ProcessRequest request;
    request.stages = Stage::identities | Stage::properties | Stage::semantic_relations;
    request.batch_memory_budget_bytes = sizeof(PropertyView) * 2U;
    auto processed = rebar_set_model.value().process(request);
    CHECK(processed.has_value(), "rebar-set semantic processing is available");
    bool saw_set = false;
    bool saw_group = false;
    bool saw_end_detail = false;
    bool saw_splitter = false;
    bool saw_name = false;
    bool saw_grade = false;
    bool saw_radius = false;
    bool saw_end_offset = false;
    bool saw_lap_length = false;
    bool saw_stagger = false;
    bool saw_group_child = false;
    bool saw_end_child = false;
    bool saw_splitter_child = false;
    if (processed) {
      while (true) {
        auto batch = processed.value()->next();
        CHECK(batch.has_value(), "rebar-set batches decode");
        if (!batch || batch.value().kind == BatchKind::end) break;
        for (const auto& object : batch.value().objects) {
          saw_set |= object.internal_id == 1701U && object.kind == ObjectKind::rebar_set;
          saw_group |= object.internal_id == 1702U && object.kind == ObjectKind::rebar_set_group;
          saw_end_detail |=
              object.internal_id == 1703U && object.kind == ObjectKind::rebar_end_detail_modifier;
          saw_splitter |= object.internal_id == 1704U && object.kind == ObjectKind::rebar_splitter;
        }
        for (const auto& property : batch.value().properties) {
          saw_name |= property.object_id == 1701U && property.name == "name" &&
                      property.text_value == "REBAR";
          saw_grade |= property.object_id == 1701U && property.name == "grade" &&
                       property.text_value == "B500B";
          saw_radius |= property.object_id == 1701U && property.name == "bendingRadius" &&
                        property.floating_value == 30.0;
          saw_end_offset |= property.object_id == 1703U && property.name == "endOffset" &&
                            property.floating_value == 25.0;
          saw_lap_length |= property.object_id == 1704U && property.name == "lapLength" &&
                            property.floating_value == 450.0;
          saw_stagger |= property.object_id == 1704U && property.name == "staggerOffset" &&
                         property.floating_value == 120.0;
        }
        for (const auto& relation : batch.value().semantic_relations) {
          saw_group_child |= relation.kind == SemanticRelationKind::subelement &&
                             relation.source_id == 1701U && relation.target_id == 1702U;
          saw_end_child |= relation.kind == SemanticRelationKind::subelement &&
                           relation.source_id == 1701U && relation.target_id == 1703U;
          saw_splitter_child |= relation.kind == SemanticRelationKind::subelement &&
                                relation.source_id == 1701U && relation.target_id == 1704U;
        }
      }
    }
    CHECK(saw_set && saw_group && saw_end_detail && saw_splitter,
          "persisted rebar-set objects have stable semantic identities");
    CHECK(saw_name && saw_grade && saw_radius,
          "rebar sets expose their persisted reinforcement properties");
    CHECK(saw_end_offset && saw_lap_length && saw_stagger,
          "rebar-set modifiers expose their persisted behavior");
    CHECK(saw_group_child && saw_end_child && saw_splitter_child,
          "rebar-set groups and modifiers are linked to their owning set");
  }

  const auto parallel_leg_weld_bytes = database_with_polygon_weld_geometry(
      false, false, PolygonWeldFrameFixture::leg_parallel_to_tangent);
  ModelPackage parallel_leg_weld_package;
  parallel_leg_weld_package.add(Asset::copy(
      AssetRole::model_database, "parallel-leg-polygon-weld.db1", parallel_leg_weld_bytes));
  auto parallel_leg_weld_model = open(std::move(parallel_leg_weld_package));
  CHECK(parallel_leg_weld_model.has_value(), "a parallel-leg polygon-weld database opens");
  if (parallel_leg_weld_model) {
    ProcessRequest request;
    request.stages = Stage::display_geometry;
    auto processed = parallel_leg_weld_model.value().process(request);
    CHECK(processed.has_value(), "parallel-leg polygon-weld processing remains object-scoped");
    bool emitted_mesh = false;
    bool saw_invalid_geometry = false;
    if (processed) {
      while (true) {
        auto batch = processed.value()->next();
        CHECK(batch.has_value(), "parallel-leg polygon-weld batches fail open");
        if (!batch || batch.value().kind == BatchKind::end) break;
        for (const auto& mesh : batch.value().meshes) {
          emitted_mesh = emitted_mesh || mesh.object_id == 1201U;
        }
        for (const auto& diagnostic : batch.value().diagnostics) {
          saw_invalid_geometry =
              saw_invalid_geometry ||
              (diagnostic.object_id == 1201U && diagnostic.code == ErrorCode::invalid_geometry &&
               diagnostic.message.find("polygon-weld") != std::string_view::npos);
        }
      }
    }
    CHECK(
        !emitted_mesh && saw_invalid_geometry,
        "a polygon-weld leg parallel to its segment tangent is rejected with an object diagnostic");
  }

  const auto almost_parallel_leg_weld_bytes = database_with_polygon_weld_geometry(
      false, false, PolygonWeldFrameFixture::leg_almost_parallel_to_tangent);
  ModelPackage almost_parallel_leg_weld_package;
  almost_parallel_leg_weld_package.add(Asset::copy(AssetRole::model_database,
                                                   "almost-parallel-leg-polygon-weld.db1",
                                                   almost_parallel_leg_weld_bytes));
  auto almost_parallel_leg_weld_model = open(std::move(almost_parallel_leg_weld_package));
  CHECK(almost_parallel_leg_weld_model.has_value(),
        "an almost-parallel-leg polygon-weld database opens");
  if (almost_parallel_leg_weld_model) {
    ProcessRequest request;
    request.stages = Stage::display_geometry;
    auto processed = almost_parallel_leg_weld_model.value().process(request);
    CHECK(processed.has_value(),
          "almost-parallel-leg polygon-weld processing remains object-scoped");
    bool emitted_mesh = false;
    bool saw_invalid_geometry = false;
    if (processed) {
      while (true) {
        auto batch = processed.value()->next();
        CHECK(batch.has_value(), "almost-parallel-leg polygon-weld batches fail open");
        if (!batch || batch.value().kind == BatchKind::end) break;
        for (const auto& mesh : batch.value().meshes) {
          emitted_mesh = emitted_mesh || mesh.object_id == 1201U;
        }
        for (const auto& diagnostic : batch.value().diagnostics) {
          saw_invalid_geometry =
              saw_invalid_geometry ||
              (diagnostic.object_id == 1201U && diagnostic.code == ErrorCode::invalid_geometry &&
               diagnostic.message.find("polygon-weld") != std::string_view::npos);
        }
      }
    }
    CHECK(!emitted_mesh && saw_invalid_geometry,
          "a polygon-weld leg within 1e-8 radians of its segment tangent is rejected");
  }

  const auto reversed_frame_weld_bytes = database_with_polygon_weld_geometry(
      false, false, PolygonWeldFrameFixture::reversed_handedness);
  ModelPackage reversed_frame_weld_package;
  reversed_frame_weld_package.add(Asset::copy(
      AssetRole::model_database, "reversed-frame-polygon-weld.db1", reversed_frame_weld_bytes));
  auto reversed_frame_weld_model = open(std::move(reversed_frame_weld_package));
  CHECK(reversed_frame_weld_model.has_value(), "a reversed-frame polygon-weld database opens");
  if (reversed_frame_weld_model) {
    ProcessRequest request;
    request.stages = Stage::display_geometry;
    auto processed = reversed_frame_weld_model.value().process(request);
    CHECK(processed.has_value(), "reversed-frame polygon-weld processing is available");
    bool saw_outward_mesh = false;
    if (processed) {
      while (true) {
        auto batch = processed.value()->next();
        CHECK(batch.has_value(), "reversed-frame polygon-weld batches decode");
        if (!batch || batch.value().kind == BatchKind::end) break;
        for (const auto& mesh : batch.value().meshes) {
          if (mesh.object_id != 1201U) continue;
          double signed_volume = 0.0;
          bool nondegenerate = true;
          for (std::size_t index = 0U; index + 2U < mesh.indices.size(); index += 3U) {
            const auto vertex = [&](std::uint32_t vertex_index) {
              const auto offset = static_cast<std::size_t>(vertex_index) * 3U;
              return std::array<double, 3>{mesh.positions[offset], mesh.positions[offset + 1U],
                                           mesh.positions[offset + 2U]};
            };
            const auto first = vertex(mesh.indices[index]);
            const auto second = vertex(mesh.indices[index + 1U]);
            const auto third = vertex(mesh.indices[index + 2U]);
            const std::array<double, 3> edge_a{second[0] - first[0], second[1] - first[1],
                                               second[2] - first[2]};
            const std::array<double, 3> edge_b{third[0] - first[0], third[1] - first[1],
                                               third[2] - first[2]};
            const std::array<double, 3> normal{edge_a[1] * edge_b[2] - edge_a[2] * edge_b[1],
                                               edge_a[2] * edge_b[0] - edge_a[0] * edge_b[2],
                                               edge_a[0] * edge_b[1] - edge_a[1] * edge_b[0]};
            nondegenerate =
                nondegenerate &&
                (normal[0] * normal[0] + normal[1] * normal[1] + normal[2] * normal[2] > 1.0e-12);
            signed_volume += (first[0] * (second[1] * third[2] - second[2] * third[1]) +
                              first[1] * (second[2] * third[0] - second[0] * third[2]) +
                              first[2] * (second[0] * third[1] - second[1] * third[0])) /
                             6.0;
          }
          saw_outward_mesh = nondegenerate && signed_volume > 1.0e-9;
        }
      }
    }
    CHECK(saw_outward_mesh,
          "a reversed polygon-weld frame is canonicalized to nondegenerate outward faces");
  }

  const auto noisy_frame_weld_bytes = database_with_polygon_weld_geometry(
      false, false, PolygonWeldFrameFixture::noisy_tangent_component);
  ModelPackage noisy_frame_weld_package;
  noisy_frame_weld_package.add(Asset::copy(AssetRole::model_database,
                                           "noisy-frame-polygon-weld.db1", noisy_frame_weld_bytes));
  auto noisy_frame_weld_model = open(std::move(noisy_frame_weld_package));
  CHECK(noisy_frame_weld_model.has_value(), "a noisy-frame polygon-weld database opens");
  if (noisy_frame_weld_model) {
    ProcessRequest request;
    request.stages = Stage::display_geometry;
    auto processed = noisy_frame_weld_model.value().process(request);
    CHECK(processed.has_value(), "noisy-frame polygon-weld processing is available");
    bool saw_projected_mesh = false;
    bool saw_object_diagnostic = false;
    if (processed) {
      while (true) {
        auto batch = processed.value()->next();
        CHECK(batch.has_value(), "noisy-frame polygon-weld batches decode");
        if (!batch || batch.value().kind == BatchKind::end) break;
        for (const auto& mesh : batch.value().meshes) {
          saw_projected_mesh =
              saw_projected_mesh || (mesh.object_id == 1201U && mesh.positions.size() == 18U &&
                                     mesh.indices.size() == 24U);
        }
        for (const auto& diagnostic : batch.value().diagnostics) {
          saw_object_diagnostic = saw_object_diagnostic || diagnostic.object_id == 1201U;
        }
      }
    }
    CHECK(saw_projected_mesh && !saw_object_diagnostic,
          "a persisted weld leg with a 4.6e-5 tangent cosine is projected into the segment plane");
  }

  const auto collinear_basis_weld_bytes = database_with_polygon_weld_geometry(
      false, false, PolygonWeldFrameFixture::collinear_model_basis);
  ModelPackage collinear_basis_weld_package;
  collinear_basis_weld_package.add(Asset::copy(
      AssetRole::model_database, "collinear-basis-polygon-weld.db1", collinear_basis_weld_bytes));
  auto collinear_basis_weld_model = open(std::move(collinear_basis_weld_package));
  CHECK(collinear_basis_weld_model.has_value(), "a collinear-basis polygon-weld database opens");
  if (collinear_basis_weld_model) {
    ProcessRequest request;
    request.stages = Stage::display_geometry;
    auto processed = collinear_basis_weld_model.value().process(request);
    CHECK(processed.has_value(), "collinear-basis polygon-weld processing remains object-scoped");
    bool emitted_mesh = false;
    bool saw_invalid_geometry = false;
    if (processed) {
      while (true) {
        auto batch = processed.value()->next();
        CHECK(batch.has_value(), "collinear-basis polygon-weld batches fail open");
        if (!batch || batch.value().kind == BatchKind::end) break;
        for (const auto& mesh : batch.value().meshes) {
          emitted_mesh = emitted_mesh || mesh.object_id == 1201U;
        }
        for (const auto& diagnostic : batch.value().diagnostics) {
          saw_invalid_geometry =
              saw_invalid_geometry ||
              (diagnostic.object_id == 1201U && diagnostic.code == ErrorCode::invalid_geometry &&
               diagnostic.message.find("polygon-weld") != std::string_view::npos);
        }
      }
    }
    CHECK(!emitted_mesh && saw_invalid_geometry,
          "a polygon-weld with a collinear model coordinate basis is rejected before meshing");
  }

  const auto large_origin_weld_bytes = database_with_polygon_weld_geometry(
      false, false, PolygonWeldFrameFixture::large_model_origin);
  ModelPackage large_origin_weld_package;
  large_origin_weld_package.add(Asset::copy(
      AssetRole::model_database, "large-origin-polygon-weld.db1", large_origin_weld_bytes));
  auto large_origin_weld_model = open(std::move(large_origin_weld_package));
  CHECK(large_origin_weld_model.has_value(), "a large-origin polygon-weld database opens");
  if (large_origin_weld_model) {
    ProcessRequest request;
    request.stages = Stage::display_geometry;
    auto processed = large_origin_weld_model.value().process(request);
    CHECK(processed.has_value(), "large-origin polygon-weld processing remains object-scoped");
    bool emitted_mesh = false;
    bool saw_invalid_geometry = false;
    if (processed) {
      while (true) {
        auto batch = processed.value()->next();
        CHECK(batch.has_value(), "large-origin polygon-weld batches fail open");
        if (!batch || batch.value().kind == BatchKind::end) break;
        for (const auto& mesh : batch.value().meshes) {
          emitted_mesh = emitted_mesh || mesh.object_id == 1201U;
        }
        for (const auto& diagnostic : batch.value().diagnostics) {
          saw_invalid_geometry =
              saw_invalid_geometry ||
              (diagnostic.object_id == 1201U && diagnostic.code == ErrorCode::invalid_geometry &&
               diagnostic.message.find("polygon-weld") != std::string_view::npos);
        }
      }
    }
    CHECK(!emitted_mesh && saw_invalid_geometry,
          "a polygon-weld whose 5mm and 10mm features collapse after float conversion is rejected");
  }

  const auto malformed_polygon_weld_bytes = database_with_polygon_weld_geometry(true);
  ModelPackage malformed_polygon_weld_package;
  malformed_polygon_weld_package.add(Asset::copy(
      AssetRole::model_database, "malformed-polygon-weld.db1", malformed_polygon_weld_bytes));
  auto malformed_polygon_weld_model = open(std::move(malformed_polygon_weld_package));
  CHECK(malformed_polygon_weld_model.has_value(), "a malformed polygon-weld database opens");
  if (malformed_polygon_weld_model) {
    ProcessRequest request;
    request.stages = Stage::display_geometry;
    auto processed = malformed_polygon_weld_model.value().process(request);
    CHECK(processed.has_value(), "malformed polygon-weld processing remains object-scoped");
    bool emitted_mesh = false;
    bool saw_invalid_geometry = false;
    if (processed) {
      while (true) {
        auto batch = processed.value()->next();
        CHECK(batch.has_value(), "malformed polygon-weld batches fail open");
        if (!batch || batch.value().kind == BatchKind::end) break;
        for (const auto& mesh : batch.value().meshes) {
          emitted_mesh = emitted_mesh || mesh.object_id == 1201U;
        }
        for (const auto& diagnostic : batch.value().diagnostics) {
          saw_invalid_geometry =
              saw_invalid_geometry ||
              (diagnostic.object_id == 1201U && diagnostic.code == ErrorCode::invalid_geometry &&
               diagnostic.message.find("polygon-weld") != std::string_view::npos);
        }
      }
    }
    CHECK(!emitted_mesh && saw_invalid_geometry,
          "an incomplete persisted weld path is omitted with an object-scoped diagnostic");
  }

  ModelPackage bounded_polygon_weld_package;
  bounded_polygon_weld_package.add(
      Asset::copy(AssetRole::model_database, "bounded-polygon-weld.db1", polygon_weld_bytes));
  auto bounded_polygon_weld_model = open(std::move(bounded_polygon_weld_package));
  CHECK(bounded_polygon_weld_model.has_value(), "a bounded polygon-weld database opens");
  if (bounded_polygon_weld_model) {
    ProcessRequest request;
    request.stages = Stage::display_geometry;
    request.geometry_memory_budget_bytes = 192U;
    auto processed = bounded_polygon_weld_model.value().process(request);
    CHECK(processed.has_value(), "bounded polygon-weld processing remains object-scoped");
    bool emitted_mesh = false;
    bool saw_resource_limit = false;
    if (processed) {
      while (true) {
        auto batch = processed.value()->next();
        CHECK(batch.has_value(), "bounded polygon-weld batches fail open");
        if (!batch || batch.value().kind == BatchKind::end) break;
        for (const auto& mesh : batch.value().meshes) {
          emitted_mesh = emitted_mesh || mesh.object_id == 1201U;
        }
        for (const auto& diagnostic : batch.value().diagnostics) {
          saw_resource_limit =
              saw_resource_limit ||
              (diagnostic.object_id == 1201U && diagnostic.code == ErrorCode::resource_limit &&
               diagnostic.message.find("Polygon-weld") != std::string_view::npos);
        }
      }
    }
    CHECK(!emitted_mesh && saw_resource_limit,
          "polygon-weld retained rows and meshes honor the aggregate geometry budget");
  }

  const auto merged_polygon_weld_bytes =
      database_with_polygon_weld_geometry(false, false, PolygonWeldFrameFixture::orthogonal, 2U);
  ModelPackage merged_polygon_weld_package;
  merged_polygon_weld_package.add(
      Asset::copy(AssetRole::model_database, "merged-polygon-weld.db1", merged_polygon_weld_bytes));
  auto merged_polygon_weld_model = open(std::move(merged_polygon_weld_package));
  CHECK(merged_polygon_weld_model.has_value(), "a two-polygon single-weld database opens");
  if (merged_polygon_weld_model) {
    ProcessRequest request;
    request.stages = Stage::display_geometry;
    request.geometry_memory_budget_bytes = 648U;
    auto processed = merged_polygon_weld_model.value().process(request);
    CHECK(processed.has_value(), "two-polygon weld processing honors its tight budget");
    std::size_t weld_meshes = 0U;
    bool saw_complete_mesh = false;
    bool saw_resource_limit = false;
    if (processed) {
      while (true) {
        auto batch = processed.value()->next();
        CHECK(batch.has_value(), "two-polygon weld batches decode");
        if (!batch || batch.value().kind == BatchKind::end) break;
        for (const auto& mesh : batch.value().meshes) {
          if (mesh.object_id != 1201U) continue;
          ++weld_meshes;
          saw_complete_mesh = mesh.positions.size() == 36U && mesh.indices.size() == 48U;
        }
        for (const auto& diagnostic : batch.value().diagnostics) {
          saw_resource_limit = saw_resource_limit || (diagnostic.object_id == 1201U &&
                                                      diagnostic.code == ErrorCode::resource_limit);
        }
      }
    }
    CHECK(weld_meshes == 1U && saw_complete_mesh && !saw_resource_limit,
          "merged polygon geometry charges one MeshData overhead for its weld owner");
  }

  const auto many_row_weld_bytes = database_with_polygon_weld_geometry(
      false, false, PolygonWeldFrameFixture::orthogonal, 1U, 4U);
  ModelPackage many_row_weld_package;
  many_row_weld_package.add(
      Asset::copy(AssetRole::model_database, "many-row-polygon-weld.db1", many_row_weld_bytes));
  auto many_row_weld_model = open(std::move(many_row_weld_package));
  CHECK(many_row_weld_model.has_value(), "a many-row polygon-weld database opens");
  if (many_row_weld_model) {
    ProcessRequest request;
    request.stages = Stage::display_geometry;
    request.geometry_memory_budget_bytes = 1168U;
    auto processed = many_row_weld_model.value().process(request);
    CHECK(processed.has_value(), "many-row polygon-weld processing honors its tight budget");
    bool saw_complete_mesh = false;
    bool saw_resource_limit = false;
    if (processed) {
      while (true) {
        auto batch = processed.value()->next();
        CHECK(batch.has_value(), "many-row polygon-weld batches decode");
        if (!batch || batch.value().kind == BatchKind::end) break;
        for (const auto& mesh : batch.value().meshes) {
          saw_complete_mesh =
              saw_complete_mesh || (mesh.object_id == 1201U && mesh.positions.size() == 54U &&
                                    mesh.indices.size() == 96U);
        }
        for (const auto& diagnostic : batch.value().diagnostics) {
          saw_resource_limit = saw_resource_limit || (diagnostic.object_id == 1201U &&
                                                      diagnostic.code == ErrorCode::resource_limit);
        }
      }
    }
    CHECK(saw_complete_mesh && !saw_resource_limit,
          "many-row weld paths need no unbudgeted flattened-value allocation");
  }

  const auto semantic_only_weld_bytes = database_with_polygon_weld_geometry(false, true);
  ModelPackage semantic_only_weld_package;
  semantic_only_weld_package.add(Asset::copy(
      AssetRole::model_database, "semantic-only-polygon-weld.db1", semantic_only_weld_bytes));
  auto semantic_only_weld_model = open(std::move(semantic_only_weld_package));
  CHECK(semantic_only_weld_model.has_value(), "a logical compound-weld database opens");
  if (semantic_only_weld_model) {
    ProcessRequest request;
    request.stages = Stage::display_geometry;
    auto processed = semantic_only_weld_model.value().process(request);
    CHECK(processed.has_value(), "logical compound-weld processing remains available");
    bool emitted_mesh = false;
    bool emitted_diagnostic = false;
    if (processed) {
      while (true) {
        auto batch = processed.value()->next();
        CHECK(batch.has_value(), "logical compound-weld batches decode");
        if (!batch || batch.value().kind == BatchKind::end) break;
        for (const auto& mesh : batch.value().meshes) {
          emitted_mesh = emitted_mesh || mesh.object_id == 1201U;
        }
        for (const auto& diagnostic : batch.value().diagnostics) {
          emitted_diagnostic = emitted_diagnostic || diagnostic.object_id == 1201U;
        }
      }
    }
    CHECK(!emitted_mesh && !emitted_diagnostic,
          "logical and compound welds remain semantic-only without guessed geometry");
  }

  ModelPackage semantic_package;
  semantic_package.add(Asset::copy(AssetRole::model_database, "semantic.db1", bytes));
  auto semantic_model = open(std::move(semantic_package));
  CHECK(semantic_model.has_value(), "the semantic fixture opens independently");
  if (semantic_model) {
    ProcessRequest request;
    request.stages = Stage::identities | Stage::properties | Stage::relations;
    auto processed = semantic_model.value().process(request);
    CHECK(processed.has_value(), "semantic processing composes requested stages");
    std::size_t properties = 0;
    std::size_t relations = 0;
    bool saw_height = false;
    bool saw_label = false;
    bool saw_reference = false;
    bool saw_part_name = false;
    bool saw_part_profile = false;
    bool saw_part_material = false;
    std::size_t materials = 0;
    if (processed) {
      while (true) {
        auto batch = processed.value()->next();
        CHECK(batch.has_value(), "semantic batches decode without an error");
        if (!batch || batch.value().kind == BatchKind::end) {
          break;
        }
        if (batch.value().kind == BatchKind::properties) {
          properties += batch.value().properties.size();
          for (const auto& property : batch.value().properties) {
            saw_height |= property.object_id == 1201 && property.name == "HEIGHT" &&
                          property.kind == PropertyValueKind::floating &&
                          property.floating_value == 12.5;
            saw_label |= property.object_id == 1201 && property.name == "LABEL" &&
                         property.kind == PropertyValueKind::text &&
                         property.text_value == "Beam A";
            saw_reference |= property.object_id == 1201 && property.name == "OWNER" &&
                             property.kind == PropertyValueKind::reference &&
                             property.reference_id == 700;
            saw_part_name |= property.object_id == 1201 && property.name == "name" &&
                             property.text_value == "Main beam";
            saw_part_profile |= property.object_id == 1201 && property.name == "profile" &&
                                property.text_value == "200*10";
            saw_part_material |= property.object_id == 1201 && property.name == "material" &&
                                 property.text_value == "S355";
          }
        } else if (batch.value().kind == BatchKind::materials) {
          materials += batch.value().materials.size();
          CHECK(!batch.value().materials.empty() &&
                    batch.value().materials.front().name == "S355" &&
                    batch.value().materials.front().finish == "PAINT",
                "part material semantics retain material and finish");
        } else if (batch.value().kind == BatchKind::relations) {
          relations += batch.value().relations.size();
          if (!batch.value().relations.empty()) {
            const auto& relation = batch.value().relations.front();
            CHECK(relation.relation_id == 800 && relation.type == 9 && relation.source_id == 1201 &&
                      relation.target_id == 700,
                  "a typed relation is decoded");
          }
        }
      }
    }
    CHECK(properties == 9 && saw_height && saw_label && saw_reference && saw_part_name &&
              saw_part_profile && saw_part_material,
          "attributes and native part properties preserve their value types");
    CHECK(materials == 1, "one part material assignment is emitted");
    CHECK(relations == 1, "one relation is emitted");
  }

  for (const auto format :
       {std::string_view{"8.74"}, std::string_view{"8.95"}, std::string_view{"9.08"},
        std::string_view{"9.21"}, std::string_view{"9.52"}, std::string_view{"9.66"}}) {
    const auto relationship_bytes = database_with_relationship_semantics(format);
    ModelPackage relationship_package;
    relationship_package.add(
        Asset::copy(AssetRole::model_database, "relationships.db1", relationship_bytes));
    auto relationship_model = open(std::move(relationship_package));
    CHECK(relationship_model.has_value(), "the semantic relationship fixture opens");
    if (!relationship_model) continue;

    ProcessRequest request;
    request.stages = Stage::identities | Stage::relations | Stage::semantic_relations;
    auto processed = relationship_model.value().process(request);
    CHECK(processed.has_value(), "semantic relationships are independently requestable");
    std::unordered_set<std::uint64_t> object_ids;
    std::vector<SemanticRelationView> semantic_relations;
    bool saw_assembly = false;
    bool saw_boolean_part = false;
    bool saw_cut_plane = false;
    bool saw_raw_boolean_relation = false;
    bool saw_raw_cut_relation = false;
    if (processed) {
      while (true) {
        auto batch = processed.value()->next();
        CHECK(batch.has_value(), "semantic relationship batches decode without an error");
        if (!batch || batch.value().kind == BatchKind::end) break;
        if (batch.value().kind == BatchKind::objects) {
          for (const auto& object : batch.value().objects) {
            object_ids.insert(object.internal_id);
            saw_assembly |= object.internal_id == 700U && object.kind == ObjectKind::assembly;
            saw_boolean_part |=
                object.internal_id == 1205U && object.kind == ObjectKind::boolean_part;
            saw_cut_plane |= object.internal_id == 1206U && object.kind == ObjectKind::cut_plane;
          }
        } else if (batch.value().kind == BatchKind::relations) {
          for (const auto& relation : batch.value().relations) {
            saw_raw_boolean_relation |= relation.relation_id == 805U && relation.type == 11U &&
                                        relation.source_id == 1201U && relation.target_id == 1205U;
            saw_raw_cut_relation |= relation.relation_id == 806U && relation.type == 12U &&
                                    relation.source_id == 1201U && relation.target_id == 1206U;
          }
        } else if (batch.value().kind == BatchKind::semantic_relations) {
          semantic_relations.insert(semantic_relations.end(),
                                    batch.value().semantic_relations.begin(),
                                    batch.value().semantic_relations.end());
        }
      }
    }
    CHECK(saw_assembly, "a persisted assembly has a stable semantic object kind");
    if (format == "9.52" || format == "9.66") {
      CHECK(saw_boolean_part && saw_cut_plane,
            "evaluation features remain available through raw object identity batches");
      CHECK(saw_raw_boolean_relation && saw_raw_cut_relation,
            "evaluation feature relations remain available through raw relation batches");
    }
    CHECK(semantic_relations.size() == 9U,
          "persisted hierarchy, assembly, hosting, and connection semantics produce nine "
          "deduplicated edges");
    for (const auto& relation : semantic_relations) {
      CHECK(relation.source_id != relation.target_id && object_ids.contains(relation.source_id) &&
                object_ids.contains(relation.target_id),
            "semantic relationships never expose self-edges or dangling endpoints");
    }
    const auto contains_relation = [&](SemanticRelationKind kind, std::uint64_t source,
                                       std::uint64_t target, std::uint32_t ordinal,
                                       SemanticRelationOrigin origin) {
      return std::ranges::any_of(semantic_relations, [&](const auto& relation) {
        return relation.kind == kind && relation.source_id == source &&
               relation.target_id == target && relation.ordinal == ordinal &&
               relation.origin == origin;
      });
    };
    CHECK(contains_relation(SemanticRelationKind::subelement, 1201U, 1202U, 0U,
                            SemanticRelationOrigin::object_parent),
          "SUBELEMENT is directed from a persisted parent to its child");
    CHECK(contains_relation(SemanticRelationKind::subelement, 1201U, 1203U, 1U,
                            SemanticRelationOrigin::stored_relation),
          "stored relation type 7 supplements SUBELEMENT deterministically");
    CHECK(contains_relation(SemanticRelationKind::subelement, 1201U, 700U, 2U,
                            SemanticRelationOrigin::stored_relation),
          "stored relation type 11 supplements SUBELEMENT deterministically");
    CHECK(contains_relation(SemanticRelationKind::subelement, 1203U, 1202U, 0U,
                            SemanticRelationOrigin::stored_relation),
          "stored relation type 12 supplements SUBELEMENT deterministically");
    CHECK(std::ranges::count_if(semantic_relations,
                                [](const auto& relation) {
                                  return relation.kind == SemanticRelationKind::subelement &&
                                         relation.source_id == 1201U && relation.target_id == 1202U;
                                }) == 1,
          "a stored route overlapping an object parent does not duplicate SUBELEMENT");
    CHECK(std::ranges::none_of(semantic_relations,
                               [](const auto& relation) {
                                 return relation.source_id == 1205U ||
                                        relation.target_id == 1205U ||
                                        relation.source_id == 1206U || relation.target_id == 1206U;
                               }),
          "evaluation features are not exposed as semantic relationship endpoints");
    CHECK(contains_relation(SemanticRelationKind::in_assembly, 1201U, 700U, 0U,
                            SemanticRelationOrigin::assembly_membership) &&
              contains_relation(SemanticRelationKind::in_assembly, 1202U, 700U, 1U,
                                SemanticRelationOrigin::assembly_membership) &&
              contains_relation(SemanticRelationKind::in_assembly, 1203U, 700U, 2U,
                                SemanticRelationOrigin::assembly_membership),
          "IN_ASSEMBLY is member-to-assembly with the main member at ordinal zero");
    CHECK(contains_relation(SemanticRelationKind::hosted_on, 1204U, 1201U, 0U,
                            SemanticRelationOrigin::rebar_host),
          "HOSTED_ON reverses the persisted host-to-reinforcement relation");
    CHECK(contains_relation(SemanticRelationKind::connects_to, 1202U, 1203U, 0U,
                            SemanticRelationOrigin::component_connection),
          "CONNECTS_TO follows the persisted joint primary-to-secondary direction");
    const auto hosted = std::ranges::find_if(semantic_relations, [](const auto& relation) {
      return relation.kind == SemanticRelationKind::hosted_on;
    });
    const auto connection = std::ranges::find_if(semantic_relations, [](const auto& relation) {
      return relation.kind == SemanticRelationKind::connects_to;
    });
    CHECK(hosted != semantic_relations.end() && hosted->source_relation_id == 804U,
          "HOSTED_ON retains its persisted relation identity");
    CHECK(connection != semantic_relations.end() && connection->source_relation_id == 900U,
          "CONNECTS_TO retains its persisted joint identity");
  }

  for (const auto format : {std::string_view{"9.52"}, std::string_view{"9.66"}}) {
    const auto report_bytes = database_with_report_semantics(format);
    ModelPackage report_package;
    report_package.add(Asset::copy(AssetRole::model_database, "report.db1", report_bytes));
    auto report_model = open(std::move(report_package));
    CHECK(report_model.has_value(), "the report-property fixture opens");
    if (!report_model) continue;

    ProcessRequest request;
    request.stages = Stage::properties;
    auto processed = report_model.value().process(request);
    CHECK(processed.has_value(), "report-property processing is available");
    bool saw_profile = false;
    bool saw_phase = false;
    bool saw_prefix = false;
    bool saw_assembly_position = false;
    bool saw_profile_type = false;
    if (processed) {
      while (true) {
        auto batch = processed.value()->next();
        CHECK(batch.has_value(), "report-property batches decode without an error");
        if (!batch || batch.value().kind == BatchKind::end) break;
        if (batch.value().kind != BatchKind::properties) continue;
        for (const auto& property : batch.value().properties) {
          if (property.object_id != 1201U) continue;
          saw_profile |= property.group == "Tekla" && property.name == "profile" &&
                         property.kind == PropertyValueKind::text &&
                         property.text_value == "IPE200";
          saw_phase |= property.group == "Report" && property.name == "PHASE" &&
                       property.kind == PropertyValueKind::integer && property.integer_value == 24;
          saw_prefix |= property.group == "Report" && property.name == "PREFIX" &&
                        property.kind == PropertyValueKind::text && property.text_value == "Beton";
          saw_assembly_position |= property.group == "Report" && property.name == "ASSEMBLY_POS" &&
                                   property.kind == PropertyValueKind::text &&
                                   property.text_value == "90786(?)";
          saw_profile_type |= property.group == "Report" && property.name == "PROFILE_TYPE" &&
                              property.kind == PropertyValueKind::text &&
                              property.text_value == "I";
        }
      }
    }
    CHECK(saw_profile && saw_phase && saw_prefix && saw_assembly_position && saw_profile_type,
          "linked profile, phase, numbering, and profile type are emitted for supported schemas");
  }

  for (const auto format : {std::string_view{"9.52"}, std::string_view{"9.66"}}) {
    const auto report_bytes = database_with_zero_position_report_semantics(format);
    ModelPackage report_package;
    report_package.add(
        Asset::copy(AssetRole::model_database, "zero-position-report.db1", report_bytes));
    auto report_model = open(std::move(report_package));
    CHECK(report_model.has_value(), "the prefixed zero-position report fixture opens");
    if (!report_model) continue;

    ProcessRequest request;
    request.stages = Stage::properties;
    auto processed = report_model.value().process(request);
    CHECK(processed.has_value(), "prefixed zero-position report processing is available");
    bool saw_profile = false;
    bool saw_assembly_position = false;
    bool saw_profile_type = false;
    if (processed) {
      while (true) {
        auto batch = processed.value()->next();
        CHECK(batch.has_value(), "prefixed zero-position report batches decode");
        if (!batch || batch.value().kind == BatchKind::end) break;
        if (batch.value().kind != BatchKind::properties) continue;
        for (const auto& property : batch.value().properties) {
          if (property.object_id != 1201U) continue;
          saw_profile |= property.group == "Tekla" && property.name == "profile" &&
                         property.kind == PropertyValueKind::text &&
                         property.text_value == "IPE200";
          saw_assembly_position |= property.group == "Report" && property.name == "ASSEMBLY_POS" &&
                                   property.kind == PropertyValueKind::text &&
                                   property.text_value == "A9/0";
          saw_profile_type |= property.group == "Report" && property.name == "PROFILE_TYPE" &&
                              property.kind == PropertyValueKind::text &&
                              property.text_value == "I";
        }
      }
    }
    CHECK(saw_profile && saw_assembly_position && saw_profile_type,
          "prefixed zero positions and already-combined profiles retain Tekla report semantics");
  }

  const auto bolt_bytes = database_with_bolt_semantics();
  ModelPackage bolt_package;
  bolt_package.add(Asset::copy(AssetRole::model_database, "bolt.db1", bolt_bytes));
  auto bolt_model = open(std::move(bolt_package));
  CHECK(bolt_model.has_value(), "the bolt semantic fixture opens");
  if (bolt_model) {
    ProcessRequest request;
    request.stages = Stage::properties;
    auto processed = bolt_model.value().process(request);
    CHECK(processed.has_value(), "bolt semantic processing is available without part rows");
    bool saw_size = false;
    bool saw_count = false;
    bool saw_standard = false;
    bool saw_report_type = false;
    bool saw_report_standard = false;
    bool saw_length = false;
    if (processed) {
      while (true) {
        auto batch = processed.value()->next();
        CHECK(batch.has_value(), "bolt semantic batches decode without an error");
        if (!batch || batch.value().kind == BatchKind::end) break;
        if (batch.value().kind != BatchKind::properties) continue;
        for (const auto& property : batch.value().properties) {
          if (property.object_id != 1201U) continue;
          saw_size |= property.group == "Tekla" && property.name == "boltSize" &&
                      property.kind == PropertyValueKind::floating &&
                      property.floating_value == 16.0;
          saw_count |= property.group == "Tekla" && property.name == "boltCount" &&
                       property.kind == PropertyValueKind::integer && property.integer_value == 4;
          saw_standard |= property.group == "Tekla" && property.name == "boltStandard" &&
                          property.text_value == "FISCHER FAZ";
          saw_report_type |= property.group == "Report" && property.name == "BOLT_TYPE" &&
                             property.text_value == "FISCHER FAZ";
          saw_report_standard |= property.group == "Report" && property.name == "BOLT_STANDARD" &&
                                 property.text_value == "FAZ";
          saw_length |= property.group == "Report" && property.name == "LENGTH" &&
                        property.kind == PropertyValueKind::floating &&
                        property.floating_value == 50.0;
        }
      }
    }
    CHECK(saw_size && saw_count && saw_standard && saw_report_type && saw_report_standard &&
              saw_length,
          "bolt groups expose the six connector-compatible semantic properties");
  }

  const auto legacy_bolt_bytes = database_with_legacy_bolt_geometry();
  ModelPackage legacy_bolt_package;
  legacy_bolt_package.add(
      Asset::copy(AssetRole::model_database, "legacy-bolt.db1", legacy_bolt_bytes));
  auto legacy_bolt_model = open(std::move(legacy_bolt_package));
  CHECK(legacy_bolt_model.has_value(), "the legacy bolt geometry fixture opens");
  if (legacy_bolt_model) {
    ProcessRequest request;
    request.stages = Stage::display_geometry;
    auto processed = legacy_bolt_model.value().process(request);
    CHECK(processed.has_value(), "legacy bolt display geometry processing is available");
    std::size_t mesh_count = 0U;
    std::size_t vertex_count = 0U;
    if (processed) {
      while (true) {
        auto batch = processed.value()->next();
        CHECK(batch.has_value(), "legacy bolt geometry batches decode without an error");
        if (!batch || batch.value().kind == BatchKind::end) break;
        if (batch.value().kind != BatchKind::meshes) continue;
        mesh_count += batch.value().meshes.size();
        for (const auto& mesh : batch.value().meshes) {
          if (mesh.object_id == 1201U) vertex_count += mesh.positions.size() / 3U;
        }
      }
    }
    CHECK(mesh_count == 1U && vertex_count == 144U,
          "a legacy UNDEFINED_STUD group emits four headed octagonal fasteners");
  }

  const auto duplicate_property_bytes =
      database_with_one_object("200*10", "14", "9.66", false, 7U, false, false, false, false, false,
                               false, false, false, false, true);
  ModelPackage duplicate_property_package;
  duplicate_property_package.add(
      Asset::copy(AssetRole::model_database, "duplicate-property.db1", duplicate_property_bytes));
  auto duplicate_property_model = open(std::move(duplicate_property_package));
  CHECK(duplicate_property_model.has_value(), "the duplicate-property fixture opens");
  if (duplicate_property_model) {
    ProcessRequest request;
    request.stages = Stage::properties;
    auto processed = duplicate_property_model.value().process(request);
    CHECK(processed.has_value(), "duplicate properties are normalized during processing");
    std::size_t height_count = 0;
    bool saw_numeric_height = false;
    bool saw_text_label = false;
    bool saw_unrelated_part_property = false;
    if (processed) {
      while (true) {
        auto batch = processed.value()->next();
        CHECK(batch.has_value(), "normalized property batches decode without an error");
        if (!batch || batch.value().kind == BatchKind::end) break;
        if (batch.value().kind != BatchKind::properties) continue;
        for (const auto& property : batch.value().properties) {
          if (property.object_id == 1201 && property.group == "User Defined Attributes" &&
              property.name == "HEIGHT") {
            ++height_count;
            saw_numeric_height =
                property.kind == PropertyValueKind::floating && property.floating_value == 12.5;
          }
          saw_text_label |= property.object_id == 1201 &&
                            property.group == "User Defined Attributes" &&
                            property.name == "LABEL" && property.kind == PropertyValueKind::text &&
                            property.text_value == "Beam A";
          saw_unrelated_part_property |= property.object_id == 1201 && property.group == "Tekla" &&
                                         property.name == "profile" &&
                                         property.text_value == "200*10";
        }
      }
    }
    CHECK(height_count == 1U && saw_numeric_height,
          "a numeric UDA deterministically outranks an equivalent text UDA");
    CHECK(saw_text_label && saw_unrelated_part_property,
          "normalization preserves unrelated UDA and native semantic properties");
  }

  const auto component_bytes = database_with_one_object("200*10", "14", "9.66", false, 7U, false,
                                                        false, false, false, false, true);
  ModelPackage component_package;
  component_package.add(Asset::copy(AssetRole::model_database, "components.db1", component_bytes));
  auto component_model = open(std::move(component_package));
  CHECK(component_model.has_value(), "the persisted component fixture opens");
  if (component_model) {
    ProcessRequest request;
    request.stages = Stage::identities | Stage::relations | Stage::instances;
    auto processed = component_model.value().process(request);
    CHECK(processed.has_value(), "persisted component instances are available with relations");
    std::size_t instance_count = 0;
    bool saw_joint = false;
    bool saw_macro = false;
    std::size_t component_child_objects = 0;
    if (processed) {
      while (true) {
        auto batch = processed.value()->next();
        CHECK(batch.has_value(), "component semantic batches decode without an error");
        if (!batch || batch.value().kind == BatchKind::end) break;
        if (batch.value().kind == BatchKind::objects) {
          for (const auto& object : batch.value().objects) {
            component_child_objects += object.parent_id == 2000U || object.parent_id == 2100U;
          }
        } else if (batch.value().kind == BatchKind::instances) {
          instance_count += batch.value().instances.size();
          for (const auto& instance : batch.value().instances) {
            saw_joint |=
                instance.object_id == 2000U && instance.kind == InstanceKind::joint &&
                instance.name == "Fixture joint" && instance.description == "Joint description" &&
                instance.number == 123U && instance.primary_object_id == 1201U &&
                instance.secondary_object_id == 700U && instance.secondary_object_count == 1U &&
                instance.persisted_child_count == 2U &&
                instance.definition_status == ComponentDefinitionStatus::not_checked;
            saw_macro |= instance.object_id == 2100U && instance.kind == InstanceKind::macro &&
                         instance.name == "Fixture macro" &&
                         instance.description == "Macro description" && instance.number == 77U &&
                         instance.type == 42U && instance.persisted_child_count == 1U &&
                         instance.definition_status == ComponentDefinitionStatus::not_checked;
          }
        }
      }
    }
    CHECK(component_child_objects == 3U,
          "component children retain their independent object identities and parent IDs");
    CHECK(instance_count == 2U && saw_joint && saw_macro,
          "joint and macro occurrences retain names, inputs, and persisted child counts");
  }

  const auto component_catalog_bytes = database_with_one_object(
      "200*10", "14", "9.66", false, 7U, false, false, false, false, false, true, true);
  ModelPackage component_definition_package;
  component_definition_package.add(
      Asset::copy(AssetRole::model_database, "components.db1", component_bytes));
  component_definition_package.add(
      Asset::copy(AssetRole::component_catalog, "xslib.db1", component_catalog_bytes));
  auto component_definition_model = open(std::move(component_definition_package));
  CHECK(component_definition_model.has_value(), "the component definition fixture opens");
  if (component_definition_model) {
    ProcessRequest request;
    request.stages = Stage::component_definitions;
    auto processed = component_definition_model.value().process(request);
    CHECK(processed.has_value(), "component definition indexing is explicitly available");
    std::size_t available_definitions = 0;
    if (processed) {
      while (true) {
        auto batch = processed.value()->next();
        CHECK(batch.has_value(), "component definition batches decode without an error");
        if (!batch || batch.value().kind == BatchKind::end) break;
        if (batch.value().kind != BatchKind::instances) continue;
        for (const auto& instance : batch.value().instances) {
          available_definitions +=
              instance.definition_status == ComponentDefinitionStatus::available;
        }
      }
    }
    CHECK(available_definitions == 2U,
          "joint and macro occurrences resolve against the keyed component catalog");
  }

  ModelPackage missing_component_definition_package;
  missing_component_definition_package.add(
      Asset::copy(AssetRole::model_database, "components.db1", component_bytes));
  auto missing_component_definition_model = open(std::move(missing_component_definition_package));
  CHECK(missing_component_definition_model.has_value(),
        "a model without a component catalog still opens");
  if (missing_component_definition_model) {
    ProcessRequest request;
    request.stages = Stage::component_definitions;
    auto processed = missing_component_definition_model.value().process(request);
    CHECK(processed.has_value(), "a missing optional component catalog is not an error");
    std::size_t unavailable_definitions = 0;
    if (processed) {
      while (true) {
        auto batch = processed.value()->next();
        CHECK(batch.has_value(), "missing-catalog batches decode without an error");
        if (!batch || batch.value().kind == BatchKind::end) break;
        if (batch.value().kind != BatchKind::instances) continue;
        for (const auto& instance : batch.value().instances) {
          unavailable_definitions +=
              instance.definition_status == ComponentDefinitionStatus::unavailable;
        }
      }
    }
    CHECK(unavailable_definitions == 2U,
          "occurrences explicitly report unavailable definitions without a catalog");
  }

  ModelPackage limited_component_definition_package;
  limited_component_definition_package.add(
      Asset::copy(AssetRole::model_database, "components.db1", component_bytes));
  limited_component_definition_package.add(
      Asset::copy(AssetRole::component_catalog, "xslib.db1", component_catalog_bytes));
  auto limited_component_definition_model = open(std::move(limited_component_definition_package));
  CHECK(limited_component_definition_model.has_value(),
        "the catalog budget fixture opens before optional processing");
  if (limited_component_definition_model) {
    ProcessRequest request;
    request.stages = Stage::component_definitions;
    request.component_catalog_memory_budget_bytes = 1U;
    auto processed = limited_component_definition_model.value().process(request);
    CHECK(!processed && processed.error().code == ErrorCode::resource_limit,
          "an explicit component catalog budget is enforced before indexing");
  }

  ModelPackage unkeyed_component_definition_package;
  unkeyed_component_definition_package.add(
      Asset::copy(AssetRole::model_database, "components.db1", component_bytes));
  unkeyed_component_definition_package.add(
      Asset::copy(AssetRole::component_catalog, "xslib.db1", component_bytes));
  auto unkeyed_component_definition_model = open(std::move(unkeyed_component_definition_package));
  CHECK(unkeyed_component_definition_model.has_value(),
        "an unkeyed catalog remains deferred during model opening");
  if (unkeyed_component_definition_model) {
    ProcessRequest request;
    request.stages = Stage::component_definitions;
    auto processed = unkeyed_component_definition_model.value().process(request);
    CHECK(!processed && processed.error().code == ErrorCode::schema_mismatch,
          "an explicitly requested catalog must expose compatible keyed tables");
  }

  auto wrong_kind_catalog_bytes = component_catalog_bytes;
  replace_ascii_once(wrong_kind_catalog_bytes, "Fixture macro", "Other macro");
  replace_ascii_once(wrong_kind_catalog_bytes, "Fixture joint", "Fixture macro");
  ModelPackage wrong_kind_definition_package;
  wrong_kind_definition_package.add(
      Asset::copy(AssetRole::model_database, "components.db1", component_bytes));
  wrong_kind_definition_package.add(
      Asset::copy(AssetRole::component_catalog, "xslib.db1", wrong_kind_catalog_bytes));
  auto wrong_kind_definition_model = open(std::move(wrong_kind_definition_package));
  CHECK(wrong_kind_definition_model.has_value(), "the kind-specific catalog fixture opens");
  if (wrong_kind_definition_model) {
    ProcessRequest request;
    request.stages = Stage::component_definitions;
    auto processed = wrong_kind_definition_model.value().process(request);
    CHECK(processed.has_value(), "kind-specific component definitions are indexed");
    std::size_t available_definitions = 0;
    if (processed) {
      while (true) {
        auto batch = processed.value()->next();
        CHECK(batch.has_value(), "kind-specific definition batches decode");
        if (!batch || batch.value().kind == BatchKind::end) break;
        if (batch.value().kind != BatchKind::instances) continue;
        for (const auto& instance : batch.value().instances) {
          available_definitions +=
              instance.definition_status == ComponentDefinitionStatus::available;
        }
      }
    }
    CHECK(available_definitions == 0U,
          "a joint definition cannot satisfy a same-named macro occurrence");
  }

  ProcessStream detached_instance_stream;
  {
    ModelPackage detached_package;
    detached_package.add(Asset::copy(AssetRole::model_database, "components.db1", component_bytes));
    auto detached_model = open(std::move(detached_package));
    CHECK(detached_model.has_value(), "the detached instance-stream fixture opens");
    if (detached_model) {
      ProcessRequest request;
      request.stages = Stage::instances;
      auto processed = detached_model.value().process(request);
      CHECK(processed.has_value(), "an instance stream can be created independently");
      if (processed) detached_instance_stream = std::move(processed.value());
    }
  }
  if (detached_instance_stream) {
    auto batch = detached_instance_stream->next();
    CHECK(batch.has_value() && batch.value().kind == BatchKind::instances &&
              !batch.value().instances.empty() &&
              batch.value().instances.front().name == "Fixture joint",
          "an instance stream retains its model-backed strings after Model destruction");
  }

  ModelPackage geometry_package;
  geometry_package.add(Asset::copy(AssetRole::model_database, "geometry.db1", bytes));
  auto geometry_model = open(std::move(geometry_package));
  CHECK(geometry_model.has_value(), "the analytic geometry fixture opens");
  if (geometry_model) {
    ProcessRequest request;
    request.stages = Stage::definition_geometry | Stage::display_geometry;
    auto processed = geometry_model.value().process(request);
    CHECK(processed.has_value(), "analytic geometry processing is available");
    std::size_t definitions = 0;
    std::size_t meshes = 0;
    if (processed) {
      while (true) {
        auto batch = processed.value()->next();
        CHECK(batch.has_value(), "geometry batches decode without an error");
        if (!batch || batch.value().kind == BatchKind::end) break;
        if (batch.value().kind == BatchKind::definition_geometry) {
          definitions += batch.value().definitions.size();
          CHECK(!batch.value().definitions.empty() &&
                    batch.value().definitions.front().profile == "200*10" &&
                    batch.value().definitions.front().length == 1000.0 &&
                    batch.value().definitions.front().has_section_metrics &&
                    std::abs(batch.value().definitions.front().section_area - 2000.0) < 1.0e-9 &&
                    std::abs(batch.value().definitions.front().section_height - 200.0) < 1.0e-9 &&
                    std::abs(batch.value().definitions.front().section_width - 10.0) < 1.0e-9,
                "definition geometry retains the profile and evaluated frame");
        } else if (batch.value().kind == BatchKind::meshes) {
          meshes += batch.value().meshes.size();
          CHECK(!batch.value().meshes.empty() &&
                    batch.value().meshes.front().coordinate_space ==
                        MeshCoordinateMode::model_space &&
                    batch.value().meshes.front().positions.size() == 24 &&
                    batch.value().meshes.front().indices.size() == 36 &&
                    batch.value().meshes.front().has_report_metrics &&
                    std::abs(batch.value().meshes.front().surface_area - 424000.0) < 1.0e-6 &&
                    std::abs(batch.value().meshes.front().volume - 2000000.0) < 1.0e-6 &&
                    std::abs(batch.value().meshes.front().longitudinal_max -
                             batch.value().meshes.front().longitudinal_min - 1000.0) < 1.0e-9,
                "a rectangular section becomes a closed analytic mesh");
        }
      }
    }
    CHECK(definitions == 1 && meshes == 1,
          "one straight part emits one definition and one display mesh");

    const auto first_signature = display_signature(geometry_model.value());
    const auto second_signature = display_signature(geometry_model.value());
    CHECK(!first_signature.empty() && first_signature == second_signature,
          "repeated processing emits the same ordered display geometry");

    ProcessRequest world_request;
    world_request.stages = Stage::display_geometry;
    auto world_processed = geometry_model.value().process(world_request);
    ProcessRequest local_request;
    local_request.stages = Stage::display_geometry;
    local_request.mesh_coordinate_mode = MeshCoordinateMode::local_with_rigid_placement;
    auto local_processed = geometry_model.value().process(local_request);
    CHECK(world_processed.has_value() && local_processed.has_value(),
          "world and placed-local display processing are both available");
    if (world_processed && local_processed) {
      auto world_batch = world_processed.value()->next();
      auto local_batch = local_processed.value()->next();
      CHECK(world_batch.has_value() && local_batch.has_value() &&
                world_batch.value().kind == BatchKind::meshes &&
                local_batch.value().kind == BatchKind::meshes &&
                world_batch.value().meshes.size() == 1U && local_batch.value().meshes.size() == 1U,
            "the analytic fixture emits matching world and placed-local mesh batches");
      if (world_batch && local_batch && world_batch.value().kind == BatchKind::meshes &&
          local_batch.value().kind == BatchKind::meshes &&
          world_batch.value().meshes.size() == 1U && local_batch.value().meshes.size() == 1U) {
        const auto& world_mesh = world_batch.value().meshes.front();
        const auto& local_mesh = local_batch.value().meshes.front();
        CHECK(local_mesh.coordinate_space == MeshCoordinateMode::local_with_rigid_placement &&
                  local_mesh.placement.origin.x == 10.0 && local_mesh.placement.origin.y == 20.0 &&
                  local_mesh.placement.origin.z == 30.0 && local_mesh.placement.x_axis.x == 1.0 &&
                  local_mesh.placement.y_axis.y == 1.0 && local_mesh.placement.z_axis.z == 1.0,
              "a requested local mesh carries its persisted rigid placement");
        CHECK(reconstructs_model_mesh(local_mesh, world_mesh),
              "the local placement reconstructs the unchanged world mesh and topology");
        CHECK(local_mesh.has_report_metrics == world_mesh.has_report_metrics &&
                  local_mesh.has_cover_surface_area == world_mesh.has_cover_surface_area &&
                  local_mesh.has_section_extents == world_mesh.has_section_extents &&
                  local_mesh.surface_area == world_mesh.surface_area &&
                  local_mesh.cover_surface_area == world_mesh.cover_surface_area &&
                  local_mesh.volume == world_mesh.volume &&
                  local_mesh.longitudinal_min == world_mesh.longitudinal_min &&
                  local_mesh.longitudinal_max == world_mesh.longitudinal_max &&
                  local_mesh.section_y_min == world_mesh.section_y_min &&
                  local_mesh.section_y_max == world_mesh.section_y_max &&
                  local_mesh.section_z_min == world_mesh.section_z_min &&
                  local_mesh.section_z_max == world_mesh.section_z_max,
              "coordinate representation does not change double-precision report metrics");
      }
    }

    ProcessRequest excluded_request;
    excluded_request.stages = Stage::display_geometry;
    excluded_request.geometry_object_id_min = 1202;
    auto excluded = geometry_model.value().process(excluded_request);
    CHECK(excluded.has_value(), "a resumable geometry window can be requested");
    if (excluded) {
      auto batch = excluded.value()->next();
      CHECK(batch.has_value() && batch.value().kind == BatchKind::end,
            "geometry outside the requested internal-id window is skipped");
    }
  }

  const auto rotated_frame_bytes = database_with_rotated_part_frame();
  ModelPackage rotated_frame_package;
  rotated_frame_package.add(
      Asset::copy(AssetRole::model_database, "rotated-frame.db1", rotated_frame_bytes));
  auto rotated_frame_model = open(std::move(rotated_frame_package));
  CHECK(rotated_frame_model.has_value(), "the rotated-frame geometry fixture opens");
  if (rotated_frame_model) {
    ProcessRequest world_request;
    world_request.stages = Stage::display_geometry;
    auto world_processed = rotated_frame_model.value().process(world_request);
    ProcessRequest local_request;
    local_request.stages = Stage::display_geometry;
    local_request.mesh_coordinate_mode = MeshCoordinateMode::local_with_rigid_placement;
    auto local_processed = rotated_frame_model.value().process(local_request);
    CHECK(world_processed.has_value() && local_processed.has_value(),
          "a rotated frame can be processed in both coordinate modes");
    if (world_processed && local_processed) {
      auto world_batch = world_processed.value()->next();
      auto local_batch = local_processed.value()->next();
      CHECK(world_batch.has_value() && local_batch.has_value() &&
                world_batch.value().kind == BatchKind::meshes &&
                local_batch.value().kind == BatchKind::meshes &&
                world_batch.value().meshes.size() == 1U && local_batch.value().meshes.size() == 1U,
            "a rotated frame emits matching world and placed-local batches");
      if (world_batch && local_batch && world_batch.value().kind == BatchKind::meshes &&
          local_batch.value().kind == BatchKind::meshes &&
          world_batch.value().meshes.size() == 1U && local_batch.value().meshes.size() == 1U) {
        const auto& world_mesh = world_batch.value().meshes.front();
        const auto& local_mesh = local_batch.value().meshes.front();
        CHECK(local_mesh.coordinate_space == MeshCoordinateMode::local_with_rigid_placement &&
                  local_mesh.placement.origin.x == 10.0 && local_mesh.placement.origin.y == 20.0 &&
                  local_mesh.placement.origin.z == 30.0 && local_mesh.placement.x_axis.x == 0.0 &&
                  local_mesh.placement.x_axis.y == 1.0 && local_mesh.placement.y_axis.x == -1.0 &&
                  local_mesh.placement.y_axis.y == 0.0 && local_mesh.placement.z_axis.z == 1.0,
              "a placed mesh publishes local-to-model basis vectors as matrix columns");
        CHECK(reconstructs_model_mesh(local_mesh, world_mesh),
              "column-oriented placement reconstructs rotated model geometry");
      }
    }
  }

  const auto skewed_frame_bytes = database_with_skewed_part_frame();
  ModelPackage skewed_frame_package;
  skewed_frame_package.add(
      Asset::copy(AssetRole::model_database, "skewed-frame.db1", skewed_frame_bytes));
  auto skewed_frame_model = open(std::move(skewed_frame_package));
  CHECK(skewed_frame_model.has_value(), "the skewed-frame geometry fixture opens");
  if (skewed_frame_model) {
    ProcessRequest world_request;
    world_request.stages = Stage::display_geometry;
    auto world_processed = skewed_frame_model.value().process(world_request);
    ProcessRequest local_request;
    local_request.stages = Stage::display_geometry;
    local_request.mesh_coordinate_mode = MeshCoordinateMode::local_with_rigid_placement;
    auto local_processed = skewed_frame_model.value().process(local_request);
    CHECK(world_processed.has_value() && local_processed.has_value(),
          "a skewed frame can be processed in both coordinate modes");
    if (world_processed && local_processed) {
      auto world_batch = world_processed.value()->next();
      auto local_batch = local_processed.value()->next();
      CHECK(world_batch.has_value() && local_batch.has_value() &&
                world_batch.value().kind == BatchKind::meshes &&
                local_batch.value().kind == BatchKind::meshes &&
                world_batch.value().meshes.size() == 1U && local_batch.value().meshes.size() == 1U,
            "a skewed frame still emits one mesh in either requested mode");
      if (world_batch && local_batch && world_batch.value().kind == BatchKind::meshes &&
          local_batch.value().kind == BatchKind::meshes &&
          world_batch.value().meshes.size() == 1U && local_batch.value().meshes.size() == 1U) {
        const auto& world_mesh = world_batch.value().meshes.front();
        const auto& local_mesh = local_batch.value().meshes.front();
        CHECK(local_mesh.coordinate_space == MeshCoordinateMode::model_space &&
                  local_mesh.placement.origin.x == 0.0 && local_mesh.placement.origin.y == 0.0 &&
                  local_mesh.placement.origin.z == 0.0 && local_mesh.placement.x_axis.x == 1.0 &&
                  local_mesh.placement.y_axis.y == 1.0 && local_mesh.placement.z_axis.z == 1.0 &&
                  std::equal(local_mesh.positions.begin(), local_mesh.positions.end(),
                             world_mesh.positions.begin(), world_mesh.positions.end()),
              "an untrustworthy frame falls back for that mesh to unchanged model coordinates");
        CHECK(local_mesh.surface_area == world_mesh.surface_area &&
                  local_mesh.volume == world_mesh.volume,
              "a per-mesh placement fallback preserves report metrics");
      }
    }
  }

  const auto zero_axis_square_bytes =
      database_with_one_object("15*15", "14", "9.66", false, 7U, false, false, false, false, false,
                               false, false, false, true);
  ModelPackage zero_axis_square_package;
  zero_axis_square_package.add(
      Asset::copy(AssetRole::model_database, "zero-axis-square.db1", zero_axis_square_bytes));
  auto zero_axis_square_model = open(std::move(zero_axis_square_package));
  CHECK(zero_axis_square_model.has_value(), "the zero-transverse-axis square fixture opens");
  if (zero_axis_square_model) {
    ProcessRequest request;
    request.stages = Stage::display_geometry;
    auto processed = zero_axis_square_model.value().process(request);
    CHECK(processed.has_value(), "a centered square can reconstruct its missing transverse axis");
    std::size_t meshes = 0;
    std::size_t degenerate_diagnostics = 0;
    if (processed) {
      while (true) {
        auto batch = processed.value()->next();
        CHECK(batch.has_value(), "zero-axis square geometry batches decode");
        if (!batch || batch.value().kind == BatchKind::end) break;
        if (batch.value().kind == BatchKind::meshes) meshes += batch.value().meshes.size();
        if (batch.value().kind == BatchKind::diagnostics)
          for (const auto& diagnostic : batch.value().diagnostics)
            degenerate_diagnostics += diagnostic.message.find("coordinate system is degenerate") !=
                                      std::string_view::npos;
      }
    }
    CHECK(meshes == 1U && degenerate_diagnostics == 0U,
          "a centered square uses a deterministic fallback section basis");
  }

  const auto lis_bytes = database_with_one_object("TEST-I");
  constexpr std::string_view lis_profile =
      "PROFILE DATABASE EXPORT VERSION = 3\n"
      "PROFILE_NAME = \"TEST-I\";\n"
      "{ TYPE = 1; {\n"
      "\"HEIGHT\" 200\n"
      "\"WIDTH\" 100\n"
      "\"WEB_THICKNESS\" 10\n"
      "\"FLANGE_THICKNESS\" 20\n"
      "} }\n";
  ModelPackage lis_package;
  lis_package.add(Asset::copy(AssetRole::model_database, "lis.db1", lis_bytes));
  lis_package.add(Asset::copy(AssetRole::catalog_snapshot, "Template/PG.lis",
                              std::as_bytes(std::span(lis_profile))));
  auto lis_model = open(std::move(lis_package));
  CHECK(lis_model.has_value(), "the project-catalog LIS fixture opens");
  if (lis_model) {
    ProcessRequest request;
    request.stages = Stage::display_geometry;
    auto processed = lis_model.value().process(request);
    CHECK(processed.has_value(), "project-catalog LIS geometry processing is available");
    if (processed) {
      auto batch = processed.value()->next();
      CHECK(batch.has_value() && batch.value().kind == BatchKind::meshes &&
                batch.value().meshes.size() == 1 &&
                batch.value().meshes.front().positions.size() == 72 &&
                batch.value().meshes.front().indices.size() == 132,
            "a project-catalog I-section is resolved and triangulated as a concave profile");
    }
  }

  for (const auto& [profile, positions, indices, expected_bounds] :
       std::array<std::tuple<std::string_view, std::size_t, std::size_t, std::array<float, 6>>, 39>{
           std::tuple{"IPE200", 168U, 324U,
                      std::array<float, 6>{10.0F, -80.0F, -20.0F, 1010.0F, 120.0F, 80.0F}},
           std::tuple{"HEA120", 168U, 324U,
                      std::array<float, 6>{10.0F, -37.0F, -30.0F, 1010.0F, 77.0F, 90.0F}},
           std::tuple{"HEB220", 168U, 324U,
                      std::array<float, 6>{10.0F, -90.0F, -80.0F, 1010.0F, 130.0F, 140.0F}},
           std::tuple{"HEM220", 168U, 324U,
                      std::array<float, 6>{10.0F, -100.0F, -83.0F, 1010.0F, 140.0F, 143.0F}},
           std::tuple{"U220", 48U, 84U,
                      std::array<float, 6>{10.0F, -90.0F, -10.0F, 1010.0F, 130.0F, 70.0F}},
           std::tuple{"PFC200*90*30", 96U, 180U,
                      std::array<float, 6>{10.0F, -80.0F, -15.0F, 1010.0F, 120.0F, 75.0F}},
           std::tuple{"SPHERE60.3", 900U, 1788U,
                      std::array<float, 6>{10.0F, -10.15F, -0.15F, 1010.0F, 50.15F, 60.15F}},
           std::tuple{"CAP2135", 960U, 1920U,
                      std::array<float, 6>{10.0F, -1047.5F, -1037.5F, 510.0F, 1087.5F, 1097.5F}},
           std::tuple{"BL15*130", 24U, 36U,
                      std::array<float, 6>{10.0F, -45.0F, 22.5F, 1010.0F, 85.0F, 37.5F}},
           std::tuple{"HWR77*42", 24U, 36U,
                      std::array<float, 6>{10.0F, -18.5F, 9.0F, 1010.0F, 58.5F, 51.0F}},
           std::tuple{"AB32", 144U, 276U,
                      std::array<float, 6>{10.0F, 4.0F, 14.0F, 1010.0F, 36.0F, 46.0F}},
           std::tuple{"HE59", 36U, 60U,
                      std::array<float, 6>{10.0F, -9.5F, 4.4522505F, 1010.0F, 49.5F, 55.547749F}},
           std::tuple{"PG1250*600*50*25*600*50", 72U, 132U,
                      std::array<float, 6>{10.0F, -605.0F, -270.0F, 1010.0F, 645.0F, 330.0F}},
           std::tuple{"TG550*254*16*11", 72U, 132U,
                      std::array<float, 6>{10.0F, -255.0F, -97.0F, 1010.0F, 295.0F, 157.0F}},
           std::tuple{"O50-2", 192U, 384U,
                      std::array<float, 6>{10.0F, -5.0F, 5.0F, 1010.0F, 45.0F, 55.0F}},
           std::tuple{"O112*10", 240U, 480U,
                      std::array<float, 6>{10.0F, -36.0F, -26.0F, 1010.0F, 76.0F, 86.0F}},
           std::tuple{"SPD2135*16", 912U, 1824U,
                      std::array<float, 6>{10.0F, -1047.5F, -1037.5F, 1010.0F, 1087.5F, 1097.5F}},
           std::tuple{"QR100*5", 48U, 96U,
                      std::array<float, 6>{10.0F, -30.0F, -20.0F, 1010.0F, 70.0F, 80.0F}},
           std::tuple{"RHS90*4", 48U, 96U,
                      std::array<float, 6>{10.0F, -25.0F, -15.0F, 1010.0F, 65.0F, 75.0F}},
           std::tuple{"Ø17.5*2.75", 96U, 192U,
                      std::array<float, 6>{10.0F, 11.25F, 21.25F, 1010.0F, 28.75F, 38.75F}},
           std::tuple{"HI310-12-10*80", 72U, 132U,
                      std::array<float, 6>{10.0F, -135.0F, -10.0F, 1010.0F, 175.0F, 70.0F}},
           std::tuple{"WI300-15-20*300", 72U, 132U,
                      std::array<float, 6>{10.0F, -130.0F, -120.0F, 1010.0F, 170.0F, 180.0F}},
           std::tuple{"ROD30", 72U, 132U,
                      std::array<float, 6>{10.0F, 5.0F, 15.0F, 1010.0F, 35.0F, 45.0F}},
           std::tuple{"PLATE830*16", 24U, 36U,
                      std::array<float, 6>{10.0F, -395.0F, 22.0F, 1010.0F, 435.0F, 38.0F}},
           std::tuple{"PGT688*254*21*13*303*254*21*13", 72U, 132U,
                      std::array<float, 6>{10.0F, -324.0F, -97.0F, 1010.0F, 364.0F, 157.0F}},
           std::tuple{"TPG1000*255*15*10*255*15*310*255*15*10*255*15", 72U, 132U,
                      std::array<float, 6>{10.0F, -480.0F, -97.5F, 1010.0F, 520.0F, 157.5F}},
           std::tuple{"BHK950*25*50*500*50", 72U, 132U,
                      std::array<float, 6>{10.0F, -455.0F, -220.0F, 1010.0F, 495.0F, 280.0F}},
           std::tuple{"U200-2", 108U, 204U,
                      std::array<float, 6>{10.0F, -80.0F, -70.0F, 1010.0F, 120.0F, 130.0F}},
           std::tuple{"UNP140", 156U, 300U,
                      std::array<float, 6>{10.0F, -10.0F, -40.0F, 1010.0F, 50.0F, 100.0F}},
           std::tuple{"BLL160*125*10", 60U, 108U,
                      std::array<float, 6>{10.0F, -42.5F, -50.0F, 1010.0F, 82.5F, 110.0F}},
           std::tuple{"P200*200*16", 288U, 576U,
                      std::array<float, 6>{10.0F, -80.0F, -70.0F, 1010.0F, 120.0F, 130.0F}},
           std::tuple{"PRMDAS1000*1000-1*1-0*0", 24U, 36U,
                      std::array<float, 6>{10.0F, -480.0F, -470.0F, 1010.0F, 520.0F, 530.0F}},
           std::tuple{
               "HSW-E150*0.88", 276U, 540U,
               std::array<float, 6>{10.0F, -400.0F, -46.9400024F, 1010.0F, 440.0F, 106.940002F}},
           std::tuple{"THYSSEN-T85-N-0.75", 804U, 1596U,
                      std::array<float, 6>{10.0F, -22.2F, -553.09F, 1010.0F, 62.2F, 613.09F}},
           std::tuple{"B_WLD_H1500*600*900*25*50*70", 72U, 132U,
                      std::array<float, 6>{10.0F, -730.0F, -420.0F, 1010.0F, 770.0F, 480.0F}},
           std::tuple{"IRR_I250*275*270", 24U, 36U,
                      std::array<float, 6>{10.0F, -105.0F, -107.5F, 1010.0F, 145.0F, 167.5F}},
           std::tuple{"600X55", 24U, 36U,
                      std::array<float, 6>{10.0F, -280.0F, 2.5F, 1010.0F, 320.0F, 57.5F}},
           std::tuple{"ELD2000*2000*2500*2500", 240U, 468U,
                      std::array<float, 6>{10.0F, -1230.0F, -1220.0F, 1010.0F, 1270.0F, 1280.0F}},
           std::tuple{"PLT20*1165", 24U, 36U,
                      std::array<float, 6>{10.0F, -562.5F, 20.0F, 1010.0F, 602.5F, 40.0F}}}) {
    const auto standard_bytes = database_with_one_object(profile);
    ModelPackage standard_package;
    standard_package.add(
        Asset::copy(AssetRole::model_database, "standard-profile.db1", standard_bytes));
    auto standard_model = open(std::move(standard_package));
    CHECK(standard_model.has_value(), "the nominal standard-profile fixture opens");
    if (!standard_model) continue;
    ProcessRequest request;
    request.stages = Stage::display_geometry;
    auto processed = standard_model.value().process(request);
    CHECK(processed.has_value(), "nominal standard-profile processing is available");
    if (!processed) continue;
    auto batch = processed.value()->next();
    bool matches_expected_geometry = false;
    if (batch.has_value() && batch.value().kind == BatchKind::meshes &&
        batch.value().meshes.size() == 1) {
      const auto& mesh = batch.value().meshes.front();
      std::array<float, 6> actual{mesh.positions[0], mesh.positions[1], mesh.positions[2],
                                  mesh.positions[0], mesh.positions[1], mesh.positions[2]};
      for (std::size_t index = 3; index < mesh.positions.size(); index += 3) {
        for (std::size_t axis = 0; axis < 3; ++axis) {
          actual[axis] = std::min(actual[axis], mesh.positions[index + axis]);
          actual[axis + 3] = std::max(actual[axis + 3], mesh.positions[index + axis]);
        }
      }
      matches_expected_geometry = mesh.positions.size() == positions &&
                                  mesh.indices.size() == indices && actual == expected_bounds;
      if (!matches_expected_geometry) {
        std::printf(
            "profile %.*s: positions=%zu indices=%zu bounds=[%.9g,%.9g,%.9g,%.9g,%.9g,%.9g]\n",
            static_cast<int>(profile.size()), profile.data(), mesh.positions.size(),
            mesh.indices.size(), actual[0], actual[1], actual[2], actual[3], actual[4], actual[5]);
      }
    } else {
      std::printf("profile %.*s: no mesh batch\n", static_cast<int>(profile.size()),
                  profile.data());
    }
    CHECK(matches_expected_geometry,
          "a nominal standard profile matches the expected section bounds and closes");
  }

  const auto variable_bytes =
      database_with_one_object("PL_V200*100*300*150*0*0", "14", "9.66", false, 51U);
  ModelPackage variable_package;
  variable_package.add(
      Asset::copy(AssetRole::model_database, "variable-profile.db1", variable_bytes));
  auto variable_model = open(std::move(variable_package));
  CHECK(variable_model.has_value(), "the variable-profile fixture opens");
  if (variable_model) {
    ProcessRequest request;
    request.stages = Stage::display_geometry;
    auto processed = variable_model.value().process(request);
    CHECK(processed.has_value(), "variable-profile processing is available");
    if (processed) {
      auto batch = processed.value()->next();
      CHECK(batch.has_value() && batch.value().kind == BatchKind::meshes &&
                batch.value().meshes.size() == 1U &&
                batch.value().meshes.front().positions.size() == 24U &&
                batch.value().meshes.front().indices.size() == 36U,
            "a PL_V form-51 profile emits a closed rectangular frustum");
    }
  }

  const auto round_bytes = database_with_one_object("D1");
  ModelPackage round_package;
  round_package.add(Asset::copy(AssetRole::model_database, "round.db1", round_bytes));
  auto round_model = open(std::move(round_package));
  CHECK(round_model.has_value(), "the round-profile fixture opens");
  if (round_model) {
    ProcessRequest request;
    request.stages = Stage::display_geometry;
    auto processed = round_model.value().process(request);
    CHECK(processed.has_value(), "round-profile geometry processing is available");
    if (processed) {
      auto batch = processed.value()->next();
      CHECK(batch.has_value() && batch.value().kind == BatchKind::meshes &&
                batch.value().meshes.size() == 1,
            "one round-profile mesh is emitted");
      if (batch && batch.value().kind == BatchKind::meshes && !batch.value().meshes.empty()) {
        CHECK(batch.value().meshes.front().positions.size() == 36 &&
                  batch.value().meshes.front().indices.size() == 60,
              "D1 uses the six-facet minimum");
      }
    }
  }

  const auto polygonal_tube_bytes = database_with_one_object("PD40*10");
  ModelPackage polygonal_tube_package;
  polygonal_tube_package.add(
      Asset::copy(AssetRole::model_database, "polygonal-tube.db1", polygonal_tube_bytes));
  auto polygonal_tube_model = open(std::move(polygonal_tube_package));
  CHECK(polygonal_tube_model.has_value(), "the polygonal-tube fixture opens");
  if (polygonal_tube_model) {
    ProcessRequest request;
    request.stages = Stage::display_geometry;
    auto processed = polygonal_tube_model.value().process(request);
    CHECK(processed.has_value(), "polygonal-tube processing is available");
    if (processed) {
      auto batch = processed.value()->next();
      CHECK(batch.has_value() && batch.value().kind == BatchKind::meshes &&
                batch.value().meshes.size() == 1 &&
                batch.value().meshes.front().positions.size() == 144 &&
                batch.value().meshes.front().indices.size() == 288,
            "PD40*10 becomes the expected 12-facet annular section");
    }
  }

  const auto square_hollow_bytes = database_with_one_object("SHS120*120*5.0");
  ModelPackage square_hollow_package;
  square_hollow_package.add(
      Asset::copy(AssetRole::model_database, "square-hollow.db1", square_hollow_bytes));
  auto square_hollow_model = open(std::move(square_hollow_package));
  CHECK(square_hollow_model.has_value(), "the explicit-width square-hollow fixture opens");
  if (square_hollow_model) {
    ProcessRequest request;
    request.stages = Stage::display_geometry;
    auto processed = square_hollow_model.value().process(request);
    CHECK(processed.has_value(), "explicit-width square-hollow processing is available");
    if (processed) {
      auto batch = processed.value()->next();
      CHECK(batch.has_value() && batch.value().kind == BatchKind::meshes &&
                batch.value().meshes.size() == 1,
            "SHS height-width-thickness spelling emits a hollow mesh");
    }
  }

  const auto curved_bytes = database_with_one_object("CHS89*3", "9      590 8800.00", "9.08");
  ModelPackage curved_package;
  curved_package.add(Asset::copy(AssetRole::model_database, "curved.db1", curved_bytes));
  auto curved_model = open(std::move(curved_package));
  CHECK(curved_model.has_value(), "the legacy curved-beam fixture opens");
  if (curved_model) {
    ProcessRequest request;
    request.stages = Stage::definition_geometry | Stage::display_geometry;
    auto processed = curved_model.value().process(request);
    CHECK(processed.has_value(), "legacy curved-beam processing is available");
    bool saw_curved_definition = false;
    bool saw_curved_mesh = false;
    if (processed) {
      while (true) {
        auto batch = processed.value()->next();
        CHECK(batch.has_value(), "legacy curved-beam batches decode");
        if (!batch || batch.value().kind == BatchKind::end) break;
        if (batch.value().kind == BatchKind::definition_geometry &&
            !batch.value().definitions.empty()) {
          const auto& definition = batch.value().definitions.front();
          saw_curved_definition =
              definition.kind == DefinitionGeometryKind::circular_arc_extrusion &&
              definition.radius == 8800.0 && definition.segment_count == 59;
        }
        if (batch.value().kind == BatchKind::meshes && !batch.value().meshes.empty()) {
          const auto& mesh = batch.value().meshes.front();
          float maximum_y = -1.0e30F;
          for (std::size_t index = 1; index < mesh.positions.size(); index += 3) {
            maximum_y = std::max(maximum_y, mesh.positions[index]);
          }
          saw_curved_mesh =
              mesh.positions.size() == 5760 && mesh.indices.size() == 11520 && maximum_y > 70.0F;
        }
      }
    }
    CHECK(saw_curved_definition,
          "the packed legacy class becomes circular-arc definition geometry");
    CHECK(saw_curved_mesh, "the hollow section is swept along all native curved-beam segments");
  }

  const auto operative_bytes = database_with_one_object("200*10", "14", "9.66", true);
  ModelPackage operative_package;
  operative_package.add(Asset::copy(AssetRole::model_database, "operative.db1", operative_bytes));
  auto operative_model = open(std::move(operative_package));
  CHECK(operative_model.has_value(), "the Boolean-operative fixture opens");
  if (operative_model) {
    ProcessRequest request;
    request.stages = Stage::display_geometry;
    auto processed = operative_model.value().process(request);
    CHECK(processed.has_value(), "Boolean-operative processing is available");
    if (processed) {
      auto batch = processed.value()->next();
      CHECK(batch.has_value() && batch.value().kind == BatchKind::end,
            "a Boolean operative is not emitted as top-level DISPLAY geometry");
    }
  }

  const auto contour_bytes = database_with_one_object("PL10", "14", "9.08", false, 2, true);
  ModelPackage contour_package;
  contour_package.add(Asset::copy(AssetRole::model_database, "contour.db1", contour_bytes));
  auto contour_model = open(std::move(contour_package));
  CHECK(contour_model.has_value(), "the contour-plate fixture opens");
  if (contour_model) {
    ProcessRequest request;
    request.stages = Stage::display_geometry;
    auto processed = contour_model.value().process(request);
    CHECK(processed.has_value(), "contour-plate processing is available");
    if (processed) {
      auto batch = processed.value()->next();
      CHECK(batch.has_value() && batch.value().kind == BatchKind::meshes &&
                batch.value().meshes.size() == 1 &&
                batch.value().meshes.front().positions.size() == 432 &&
                batch.value().meshes.front().indices.size() == 852,
            "a rounded persisted contour becomes a closed plate mesh");
    }
  }

  const auto arc_contour_bytes =
      database_with_one_object("PL10", "14", "9.08", false, 2, true, false, false, false, true);
  ModelPackage arc_contour_package;
  arc_contour_package.add(
      Asset::copy(AssetRole::model_database, "arc-contour.db1", arc_contour_bytes));
  auto arc_contour_model = open(std::move(arc_contour_package));
  CHECK(arc_contour_model.has_value(), "the arc-contour fixture opens");
  if (arc_contour_model) {
    ProcessRequest request;
    request.stages = Stage::display_geometry;
    auto processed = arc_contour_model.value().process(request);
    CHECK(processed.has_value(), "arc-contour processing is available");
    if (processed) {
      auto batch = processed.value()->next();
      CHECK(batch.has_value() && batch.value().kind == BatchKind::meshes &&
                batch.value().meshes.size() == 1 &&
                batch.value().meshes.front().positions.size() > 24 &&
                batch.value().meshes.front().indices.size() > 36,
            "a persisted three-point arc becomes a tessellated plate edge");
    }
  }

  const auto mixed_contour_bytes = database_with_one_object(
      "PL10", "14", "9.08", false, 2, true, false, false, false, false, false, false, false, false,
      false, false, false, false, false, false, PartFrameFixture::orthogonal, true);
  ModelPackage mixed_contour_package;
  mixed_contour_package.add(
      Asset::copy(AssetRole::model_database, "mixed-contour.db1", mixed_contour_bytes));
  auto mixed_contour_model = open(std::move(mixed_contour_package));
  CHECK(mixed_contour_model.has_value(), "the mixed-curve contour fixture opens");
  if (mixed_contour_model) {
    ProcessRequest request;
    request.stages = Stage::display_geometry;
    auto processed = mixed_contour_model.value().process(request);
    CHECK(processed.has_value(), "mixed-curve contour processing is available");
    if (processed) {
      auto batch = processed.value()->next();
      CHECK(batch.has_value() && batch.value().kind == BatchKind::meshes &&
                batch.value().meshes.size() == 1 &&
                batch.value().meshes.front().positions.size() > 24 &&
                batch.value().meshes.front().indices.size() > 36,
            "rounding and three-point arcs compose within one persisted contour");
    }
  }

  for (const auto profile : {std::string_view{"PLT10"}, std::string_view{"10"}}) {
    const auto legacy_contour_bytes =
        database_with_one_object(profile, "14", "8.95", false, 2, true);
    ModelPackage legacy_contour_package;
    legacy_contour_package.add(
        Asset::copy(AssetRole::model_database, "legacy-contour.db1", legacy_contour_bytes));
    auto legacy_contour_model = open(std::move(legacy_contour_package));
    CHECK(legacy_contour_model.has_value(), "the legacy contour-plate fixture opens");
    if (legacy_contour_model) {
      ProcessRequest request;
      request.stages = Stage::display_geometry;
      auto processed = legacy_contour_model.value().process(request);
      CHECK(processed.has_value(), "legacy contour-plate processing is available");
      if (processed) {
        auto batch = processed.value()->next();
        CHECK(batch.has_value() && batch.value().kind == BatchKind::meshes &&
                  batch.value().meshes.size() == 1,
              "legacy PLT-prefixed and numeric plate thicknesses emit meshes");
      }
    }
  }

  const auto polybeam_bytes = database_with_one_object("D32", "14", "9.66", false, 4, false, true);
  ModelPackage polybeam_package;
  polybeam_package.add(Asset::copy(AssetRole::model_database, "polybeam.db1", polybeam_bytes));
  auto polybeam_model = open(std::move(polybeam_package));
  CHECK(polybeam_model.has_value(), "the persisted-polybeam fixture opens");
  if (polybeam_model) {
    ProcessRequest request;
    request.stages = Stage::definition_geometry | Stage::display_geometry;
    auto processed = polybeam_model.value().process(request);
    CHECK(processed.has_value(), "persisted-polybeam processing is available");
    bool saw_polybeam_definition = false;
    bool saw_polybeam_mesh = false;
    if (processed) {
      while (true) {
        auto batch = processed.value()->next();
        CHECK(batch.has_value(), "persisted-polybeam batches decode");
        if (!batch || batch.value().kind == BatchKind::end) break;
        if (batch.value().kind == BatchKind::definition_geometry &&
            batch.value().definitions.size() == 1) {
          const auto& definition = batch.value().definitions.front();
          saw_polybeam_definition =
              definition.kind == DefinitionGeometryKind::polyline_extrusion &&
              definition.path.size() == 3 && definition.path.front().x == 100.0 &&
              definition.path[1].x == 600.0 && definition.path[1].y == 500.0 &&
              definition.path.back().x == 1100.0;
        }
        if (batch.value().kind == BatchKind::meshes && batch.value().meshes.size() == 1) {
          const auto& mesh = batch.value().meshes.front();
          std::array<double, 3> first_ring_center{};
          for (std::size_t vertex = 0; vertex < 12; ++vertex) {
            for (std::size_t axis = 0; axis < 3; ++axis) {
              first_ring_center[axis] += mesh.positions[vertex * 3U + axis] / 12.0;
            }
          }
          saw_polybeam_mesh = mesh.positions.size() == 108 && mesh.indices.size() == 204 &&
                              std::abs(first_ring_center[0] - 85.8578644) < 1e-4 &&
                              std::abs(first_ring_center[1] - 14.1421356) < 1e-4 &&
                              std::abs(first_ring_center[2] - 30.0) < 1e-4;
        }
      }
    }
    CHECK(saw_polybeam_definition, "the point plus type-3 polygon becomes a public polyline path");
    CHECK(saw_polybeam_mesh,
          "the eccentric solid profile is swept on its persisted reference path");
  }

  const auto lofted_bytes =
      database_with_one_object("10", "14", "9.66", false, 8, false, false, true);
  ModelPackage lofted_package;
  lofted_package.add(Asset::copy(AssetRole::model_database, "lofted.db1", lofted_bytes));
  auto lofted_model = open(std::move(lofted_package));
  CHECK(lofted_model.has_value(), "the persisted-loft fixture opens");
  if (lofted_model) {
    ProcessRequest request;
    request.stages = Stage::definition_geometry | Stage::display_geometry;
    auto processed = lofted_model.value().process(request);
    CHECK(processed.has_value(), "persisted-loft processing is available");
    bool saw_lofted_definition = false;
    bool saw_lofted_mesh = false;
    if (processed) {
      while (true) {
        auto batch = processed.value()->next();
        CHECK(batch.has_value(), "persisted-loft batches decode");
        if (!batch || batch.value().kind == BatchKind::end) break;
        if (batch.value().kind == BatchKind::definition_geometry &&
            batch.value().definitions.size() == 1U) {
          saw_lofted_definition =
              batch.value().definitions.front().kind == DefinitionGeometryKind::lofted_plate;
        }
        if (batch.value().kind == BatchKind::meshes && batch.value().meshes.size() == 1U) {
          const auto& mesh = batch.value().meshes.front();
          std::array<float, 6> bounds{mesh.positions[0], mesh.positions[1], mesh.positions[2],
                                      mesh.positions[0], mesh.positions[1], mesh.positions[2]};
          for (std::size_t index = 3U; index < mesh.positions.size(); index += 3U) {
            for (std::size_t axis = 0; axis < 3U; ++axis) {
              bounds[axis] = std::min(bounds[axis], mesh.positions[index + axis]);
              bounds[axis + 3U] = std::max(bounds[axis + 3U], mesh.positions[index + axis]);
            }
          }
          constexpr std::array<float, 6> expected{10.0F, 20.0F, 25.0F, 1010.0F, 120.0F, 35.0F};
          saw_lofted_mesh =
              mesh.positions.size() == 24U && mesh.indices.size() == 36U && bounds == expected;
        }
      }
    }
    CHECK(saw_lofted_definition, "the type-1000 rail tree is exposed as a lofted definition");
    CHECK(saw_lofted_mesh, "two persisted rail segments become a closed thickened ruled surface");
  }

  constexpr std::string_view shape_guid = "d982988a-82b5-4894-8465-c188ec30815a";
  const auto shape_bytes = database_with_one_object(shape_guid, "14", "9.66", false, 105);
  constexpr std::string_view shape_metadata =
      "<ImportPart><Info>"
      "<BrepStorageId>shape-storage</BrepStorageId>"
      "<Guid>d982988a-82b5-4894-8465-c188ec30815a</Guid>"
      "</Info></ImportPart>";
  constexpr std::string_view shape_geometry =
      "<Polymesh><Points>"
      "<Point><X>0</X><Y>0</Y><Z>0</Z></Point>"
      "<Point><X>10</X><Y>0</Y><Z>0</Z></Point>"
      "<Point><X>10</X><Y>10</Y><Z>0</Z></Point>"
      "<Point><X>0</X><Y>10</Y><Z>0</Z></Point>"
      "<Point><X>3</X><Y>3</Y><Z>0</Z></Point>"
      "<Point><X>3</X><Y>7</Y><Z>0</Z></Point>"
      "<Point><X>7</X><Y>7</Y><Z>0</Z></Point>"
      "<Point><X>7</X><Y>3</Y><Z>0</Z></Point>"
      "</Points><Faces><Face>"
      "<OuterLoop><Index>0</Index><Index>1</Index><Index>2</Index>"
      "<Index>3</Index></OuterLoop>"
      "<InnerLoops><Loop><Index>4</Index><Index>5</Index>"
      "<Index>6</Index><Index>7</Index></Loop></InnerLoops>"
      "</Face></Faces></Polymesh>";
  ModelPackage shape_package;
  shape_package.add(Asset::copy(AssetRole::model_database, "shape.db1", shape_bytes));
  shape_package.add(Asset::copy(AssetRole::catalog_snapshot,
                                "Shapes/d982988a-82b5-4894-8465-c188ec30815a.xml",
                                std::as_bytes(std::span(shape_metadata))));
  shape_package.add(Asset::copy(AssetRole::catalog_snapshot, "ShapeGeometries/shape-storage.xml",
                                std::as_bytes(std::span(shape_geometry))));
  auto shape_model = open(std::move(shape_package));
  CHECK(shape_model.has_value(), "the imported-shape fixture opens");
  if (shape_model) {
    ProcessRequest request;
    request.stages = Stage::definition_geometry | Stage::display_geometry;
    auto processed = shape_model.value().process(request);
    CHECK(processed.has_value(), "imported-shape processing is available");
    bool saw_shape_definition = false;
    bool saw_shape_mesh = false;
    if (processed) {
      while (true) {
        auto batch = processed.value()->next();
        CHECK(batch.has_value(), "imported-shape batches decode");
        if (!batch || batch.value().kind == BatchKind::end) break;
        if (batch.value().kind == BatchKind::definition_geometry &&
            batch.value().definitions.size() == 1) {
          saw_shape_definition =
              batch.value().definitions.front().kind == DefinitionGeometryKind::imported_shape;
        }
        if (batch.value().kind == BatchKind::meshes && batch.value().meshes.size() == 1) {
          const auto& mesh = batch.value().meshes.front();
          double area = 0.0;
          for (std::size_t index = 0; index < mesh.indices.size(); index += 3) {
            const auto a = mesh.indices[index] * 3U;
            const auto b = mesh.indices[index + 1U] * 3U;
            const auto c = mesh.indices[index + 2U] * 3U;
            area += std::abs((mesh.positions[b] - mesh.positions[a]) *
                                 (mesh.positions[c + 1U] - mesh.positions[a + 1U]) -
                             (mesh.positions[b + 1U] - mesh.positions[a + 1U]) *
                                 (mesh.positions[c] - mesh.positions[a])) /
                    2.0;
          }
          saw_shape_mesh = mesh.positions.size() == 24 && mesh.indices.size() == 24 &&
                           std::abs(area - 84.0) < 1e-5 && mesh.positions[0] == 10.0F &&
                           mesh.positions[1] == 20.0F && mesh.positions[2] == 30.0F;
        }
      }
    }
    CHECK(saw_shape_definition, "form-105 geometry is classified as a shared imported shape");
    CHECK(saw_shape_mesh,
          "a face hole is preserved and the shared shape is transformed per instance");
  }

  const auto classic_mesh_bytes = database_with_classic_rebar_meshes();
  ModelPackage classic_mesh_package;
  classic_mesh_package.add(
      Asset::copy(AssetRole::model_database, "classic-rebar-mesh.db1", classic_mesh_bytes));
  auto classic_mesh_model = open(std::move(classic_mesh_package));
  CHECK(classic_mesh_model.has_value(), "a classic polygon and bent rebar-mesh database opens");
  if (classic_mesh_model) {
    ProcessRequest property_request;
    property_request.stages = Stage::properties;
    auto properties = classic_mesh_model.value().process(property_request);
    CHECK(properties.has_value(), "classic rebar class processing is available");
    std::size_t classified_rebars = 0U;
    if (properties) {
      while (true) {
        auto batch = properties.value()->next();
        CHECK(batch.has_value(), "classic rebar property batches decode");
        if (!batch || batch.value().kind == BatchKind::end) break;
        if (batch.value().kind != BatchKind::properties) continue;
        for (const auto& property : batch.value().properties) {
          if ((property.object_id == 1201U || property.object_id == 1202U) &&
              property.group == "Tekla" && property.name == "class" &&
              property.kind == PropertyValueKind::integer && property.integer_value == 14U) {
            ++classified_rebars;
          }
        }
      }
    }
    CHECK(classified_rebars == 2U, "persisted rebar classes are emitted as semantic properties");

    ProcessRequest request;
    request.stages = Stage::display_geometry;
    auto processed = classic_mesh_model.value().process(request);
    CHECK(processed.has_value(), "classic rebar-mesh processing is available");
    std::size_t polygon_curve_count = 0U;
    std::size_t bent_curve_count = 0U;
    bool saw_polygon_longitudinal = false;
    bool saw_polygon_cross = false;
    bool saw_bent_cross = false;
    bool saw_bent_longitudinal = false;
    bool saw_bent_arc_longitudinal = false;
    bool saw_mesh_decoder_gap = false;
    if (processed) {
      while (true) {
        auto batch = processed.value()->next();
        CHECK(batch.has_value(), "classic rebar-mesh batches decode");
        if (!batch || batch.value().kind == BatchKind::end) break;
        for (const auto& curve : batch.value().curves) {
          if (curve.object_id == 1201U) {
            ++polygon_curve_count;
            if (curve.points.size() == 2U && std::abs(curve.radius - 5.0) < 1e-9) {
              saw_polygon_longitudinal =
                  saw_polygon_longitudinal || (std::abs(curve.points[0].x - 60.0) < 1e-9 &&
                                               std::abs(curve.points[0].y - 20.0) < 1e-9 &&
                                               std::abs(curve.points[0].z - 18.0) < 1e-9 &&
                                               std::abs(curve.points[1].y - 620.0) < 1e-9);
            }
            if (curve.points.size() == 2U && std::abs(curve.radius - 7.0) < 1e-9) {
              saw_polygon_cross =
                  saw_polygon_cross || (std::abs(curve.points[0].x - 10.0) < 1e-9 &&
                                        std::abs(curve.points[0].y - 120.0) < 1e-9 &&
                                        std::abs(curve.points[0].z - 30.0) < 1e-9 &&
                                        std::abs(curve.points[1].x - 1010.0) < 1e-9);
            }
          }
          if (curve.object_id == 1202U) {
            ++bent_curve_count;
            if (curve.points.size() == 3U && std::abs(curve.radius - 7.0) < 1e-9) {
              saw_bent_cross = saw_bent_cross || (std::abs(curve.points[0].x - 100.0) < 1e-9 &&
                                                  std::abs(curve.points[0].y - 277.038) < 1e-3 &&
                                                  std::abs(curve.points[0].z - 350.0) < 1e-9 &&
                                                  std::abs(curve.points[2].x - 1022.962) < 1e-3 &&
                                                  std::abs(curve.points[2].y - 1200.0) < 1e-9);
            }
            if (curve.points.size() == 2U && std::abs(curve.radius - 5.0) < 1e-9) {
              saw_bent_longitudinal =
                  saw_bent_longitudinal || (std::abs(curve.points[0].x - 200.0) < 1e-6 &&
                                            std::abs(curve.points[0].y - 289.038) < 1e-3 &&
                                            std::abs(curve.points[0].z - 300.0) < 1e-6 &&
                                            std::abs(curve.points[1].z - 800.0) < 1e-6);
              saw_bent_arc_longitudinal =
                  saw_bent_arc_longitudinal ||
                  (curve.points[0].x > 950.0 && curve.points[0].x < 1000.0 &&
                   curve.points[0].y > 300.0 && curve.points[0].y < 350.0 &&
                   std::abs(curve.points[1].z - 800.0) < 1e-6);
            }
          }
        }
        for (const auto& diagnostic : batch.value().diagnostics) {
          if ((diagnostic.object_id == 1201U || diagnostic.object_id == 1202U) &&
              diagnostic.code == ErrorCode::decoder_unavailable) {
            saw_mesh_decoder_gap = true;
          }
        }
      }
    }
    CHECK(polygon_curve_count == 6U && saw_polygon_longitudinal && saw_polygon_cross,
          "classic subtype-6 polygon meshes emit longitudinal and cross bars with layer radii");
    CHECK(bent_curve_count == 7U && saw_bent_cross && saw_bent_longitudinal &&
              saw_bent_arc_longitudinal,
          "classic subtype-8 bent meshes solve cover and sample straight and arc positions");
    CHECK(!saw_mesh_decoder_gap, "classic rebar meshes do not report a decoder gap");

    ProcessRequest bounded_request;
    bounded_request.stages = Stage::display_geometry;
    bounded_request.geometry_memory_budget_bytes = 900U;
    bounded_request.geometry_object_id_min = 1202U;
    bounded_request.geometry_object_id_max = 1202U;
    auto bounded = classic_mesh_model.value().process(bounded_request);
    CHECK(bounded.has_value(), "bounded classic bent-mesh processing remains object-scoped");
    bool emitted_bounded_bent_mesh = false;
    bool saw_bent_mesh_expansion_limit = false;
    if (bounded) {
      while (true) {
        auto batch = bounded.value()->next();
        CHECK(batch.has_value(), "bounded classic bent-mesh batches decode");
        if (!batch || batch.value().kind == BatchKind::end) break;
        for (const auto& curve : batch.value().curves)
          emitted_bounded_bent_mesh = emitted_bounded_bent_mesh || curve.object_id == 1202U;
        for (const auto& diagnostic : batch.value().diagnostics) {
          saw_bent_mesh_expansion_limit =
              saw_bent_mesh_expansion_limit ||
              (diagnostic.object_id == 1202U && diagnostic.code == ErrorCode::resource_limit &&
               diagnostic.message.find("rebar-mesh expansion") != std::string_view::npos);
        }
      }
    }
    CHECK(!emitted_bounded_bent_mesh && saw_bent_mesh_expansion_limit,
          "bent mesh crossbar point storage is budgeted before any curves are retained");
  }

  const auto shared_rebar_bytes = database_with_shared_rebar_polygon(300U, 300U);
  ModelPackage shared_rebar_package;
  shared_rebar_package.add(
      Asset::copy(AssetRole::model_database, "shared-rebar.db1", shared_rebar_bytes));
  auto shared_rebar_model = open(std::move(shared_rebar_package));
  CHECK(shared_rebar_model.has_value(), "a shared-array rebar database opens");
  if (shared_rebar_model) {
    ProcessRequest request;
    request.stages = Stage::display_geometry;
    request.geometry_memory_budget_bytes = 128U * 1024U;
    auto processed = shared_rebar_model.value().process(request);
    CHECK(processed.has_value(), "bounded shared-array rebar processing opens");
    std::uint64_t retained_point_bytes = 0U;
    bool saw_resource_limit = false;
    if (processed) {
      while (true) {
        auto batch = processed.value()->next();
        CHECK(batch.has_value(), "bounded shared-array rebar batches decode");
        if (!batch || batch.value().kind == BatchKind::end) break;
        for (const auto& curve : batch.value().curves) {
          retained_point_bytes += curve.points.size_bytes();
        }
        for (const auto& diagnostic : batch.value().diagnostics) {
          saw_resource_limit = saw_resource_limit || diagnostic.code == ErrorCode::resource_limit;
        }
      }
    }
    CHECK(retained_point_bytes <= request.geometry_memory_budget_bytes,
          "retained non-part point data stays within the aggregate geometry budget");
    CHECK(saw_resource_limit, "omitted shared-array rebars produce a resource-limit diagnostic");
  }

  const auto malformed_group_bytes = database_with_shared_rebar_polygon(1U, 1U, true);
  ModelPackage malformed_group_package;
  malformed_group_package.add(
      Asset::copy(AssetRole::model_database, "malformed-rebar-group.db1", malformed_group_bytes));
  auto malformed_group_model = open(std::move(malformed_group_package));
  CHECK(malformed_group_model.has_value(), "a malformed rebar-group database opens");
  if (malformed_group_model) {
    ProcessRequest request;
    request.stages = Stage::display_geometry;
    auto processed = malformed_group_model.value().process(request);
    CHECK(processed.has_value(), "malformed rebar-group processing remains object-scoped");
    bool saw_invalid_geometry = false;
    bool saw_wrong_decoder_code = false;
    if (processed) {
      while (true) {
        auto batch = processed.value()->next();
        CHECK(batch.has_value(), "malformed rebar-group batches decode");
        if (!batch || batch.value().kind == BatchKind::end) break;
        for (const auto& diagnostic : batch.value().diagnostics) {
          if (diagnostic.object_id != 1201U) continue;
          saw_invalid_geometry =
              saw_invalid_geometry || diagnostic.code == ErrorCode::invalid_geometry;
          saw_wrong_decoder_code =
              saw_wrong_decoder_code || diagnostic.code == ErrorCode::decoder_unavailable;
        }
      }
    }
    CHECK(saw_invalid_geometry && !saw_wrong_decoder_code,
          "malformed persisted rebar-group arrays report invalid geometry");
  }

  const auto oversized_link_bytes = database_with_shared_rebar_polygon(32U, 256U, false, true);
  ModelPackage oversized_link_package;
  oversized_link_package.add(Asset::copy(
      AssetRole::model_database, "oversized-incomplete-rebar-group.db1", oversized_link_bytes));
  auto oversized_link_model = open(std::move(oversized_link_package));
  CHECK(oversized_link_model.has_value(), "an oversized linked-array database opens");
  if (oversized_link_model) {
    ProcessRequest request;
    request.stages = Stage::display_geometry;
    request.geometry_memory_budget_bytes = 1024U;
    auto processed = oversized_link_model.value().process(request);
    CHECK(processed.has_value(), "oversized linked-array processing remains object-scoped");
    bool saw_resource_limit = false;
    bool first_object_was_misclassified = false;
    if (processed) {
      while (true) {
        auto batch = processed.value()->next();
        CHECK(batch.has_value(), "oversized linked-array batches decode");
        if (!batch || batch.value().kind == BatchKind::end) break;
        for (const auto& diagnostic : batch.value().diagnostics) {
          if (diagnostic.object_id != 1201U) continue;
          saw_resource_limit = saw_resource_limit || diagnostic.code == ErrorCode::resource_limit;
          first_object_was_misclassified =
              first_object_was_misclassified || diagnostic.code == ErrorCode::invalid_geometry;
        }
      }
    }
    CHECK(saw_resource_limit && !first_object_was_misclassified,
          "an oversized incomplete shared chain exhausts the aggregate decode budget");
  }

  const auto legacy_bytes = database_with_one_legacy_object();
  ModelPackage legacy_package;
  legacy_package.add(Asset::copy(AssetRole::model_database, "legacy-identity.db1", legacy_bytes));
  auto legacy_model = open(std::move(legacy_package));
  CHECK(legacy_model.has_value(), "a schema-conforming legacy database opens");
  if (legacy_model) {
    ProcessRequest request;
    request.stages = Stage::identities;
    auto processed = legacy_model.value().process(request);
    CHECK(processed.has_value(), "legacy identity-only processing is available");
    if (processed) {
      auto first = processed.value()->next();
      CHECK(first.has_value() && first.value().objects.size() == 1,
            "one normalized legacy identity is emitted");
      if (first && !first.value().objects.empty()) {
        const auto& object = first.value().objects.front();
        CHECK(object.internal_id == 1202 && object.assembly_id == 701,
              "legacy object relationships are decoded");
        CHECK(object.application_id == "87654321-4321-4cba-9fed-cba987654321",
              "legacy text GUIDs are normalized");
        CHECK(object.type == 2 && object.subtype == 23 && object.object_flags == 5,
              "legacy attribute rows are joined into the identity");
      }
    }

    ProcessRequest display_request;
    display_request.stages = Stage::display_geometry;
    auto displayed = legacy_model.value().process(display_request);
    CHECK(displayed.has_value(), "legacy display processing opens without eager non-part work");
    bool saw_spurious_nonpart_diagnostic = false;
    if (displayed) {
      while (true) {
        auto batch = displayed.value()->next();
        CHECK(batch.has_value(), "legacy display batches decode");
        if (!batch || batch.value().kind == BatchKind::end) break;
        if (batch.value().kind != BatchKind::diagnostics) continue;
        for (const auto& diagnostic : batch.value().diagnostics) {
          saw_spurious_nonpart_diagnostic =
              saw_spurious_nonpart_diagnostic ||
              (diagnostic.object_id == 0U &&
               diagnostic.message.find("legacy non-part") != std::string_view::npos);
        }
      }
    }
    CHECK(!saw_spurious_nonpart_diagnostic,
          "an empty legacy non-part domain does not emit an object-zero warning");
  }

#if defined(TEKLA_DB1_TEST_HAS_OCCT)
  const auto check_boolean_graph =
      [&](std::size_t part_count, bool cycle, ErrorCode expected_diagnostic,
          std::string_view diagnostic_fragment, bool boolean_part_operands) {
        const auto graph_bytes =
            database_with_boolean_chain(part_count, cycle, false, boolean_part_operands);
        ModelPackage graph_package;
        graph_package.add(Asset::copy(AssetRole::model_database, "boolean-graph.db1", graph_bytes));
        auto graph_model = open(std::move(graph_package));
        CHECK(graph_model.has_value(), "the synthetic Boolean graph opens");
        if (!graph_model) return;
        ProcessRequest request;
        request.stages = Stage::display_geometry;
        request.topology_mode = TopologyMode::direct;
        auto processed = graph_model.value().process(request);
        CHECK(processed.has_value(), "the synthetic Boolean graph processes");
        bool saw_mesh = false;
        bool saw_expected_diagnostic = expected_diagnostic == ErrorCode::none;
        bool saw_legacy_nested_omission = false;
        bool saw_any_diagnostic = false;
        if (processed) {
          while (true) {
            auto batch = processed.value()->next();
            CHECK(batch.has_value(), "Boolean graph batches decode");
            if (!batch || batch.value().kind == BatchKind::end) break;
            saw_mesh = saw_mesh ||
                       (batch.value().kind == BatchKind::meshes && !batch.value().meshes.empty());
            if (batch.value().kind != BatchKind::diagnostics) continue;
            for (const auto& diagnostic : batch.value().diagnostics) {
              saw_any_diagnostic = true;
              saw_legacy_nested_omission =
                  saw_legacy_nested_omission ||
                  (diagnostic.code == ErrorCode::decoder_unavailable &&
                   diagnostic.message.find("nested Boolean operative") != std::string_view::npos);
              if (diagnostic.code == expected_diagnostic &&
                  diagnostic.message.find(diagnostic_fragment) != std::string_view::npos) {
                saw_expected_diagnostic = true;
              }
            }
          }
        }
        CHECK(saw_mesh, "a bad nested operative never suppresses the top-level host mesh");
        CHECK(saw_expected_diagnostic, "Boolean graph safety failures are explicit diagnostics");
        CHECK(!saw_legacy_nested_omission,
              "nested Boolean operatives are recursively evaluated instead of blanket-omitted");
        if (expected_diagnostic == ErrorCode::none) {
          CHECK(!saw_any_diagnostic, "a valid nested Boolean graph evaluates without diagnostics");
        }
      };

  check_boolean_graph(3U, false, ErrorCode::none, {}, false);
  check_boolean_graph(3U, true, ErrorCode::invalid_topology, "cycle", false);
  check_boolean_graph(35U, false, ErrorCode::resource_limit, "depth", false);
  check_boolean_graph(3U, true, ErrorCode::none, {}, true);

  const auto additive_bytes = database_with_boolean_chain(2U, false, false, false, false, true);
  ModelPackage additive_package;
  additive_package.add(
      Asset::copy(AssetRole::model_database, "additive-boolean.db1", additive_bytes));
  auto additive_model = open(std::move(additive_package));
  CHECK(additive_model.has_value(), "the additive Boolean fixture opens");
  if (additive_model) {
    ProcessRequest request;
    request.stages = Stage::display_geometry;
    request.topology_mode = TopologyMode::direct;
    auto processed = additive_model.value().process(request);
    CHECK(processed.has_value(), "the additive Boolean fixture processes");
    float maximum_x = std::numeric_limits<float>::lowest();
    double host_volume = 0.0;
    if (processed) {
      while (true) {
        auto batch = processed.value()->next();
        CHECK(batch.has_value(), "additive Boolean batches decode");
        if (!batch || batch.value().kind == BatchKind::end) break;
        if (batch.value().kind != BatchKind::meshes) continue;
        for (const auto& mesh : batch.value().meshes) {
          if (mesh.object_id != 1201U) continue;
          host_volume = mesh.volume;
          for (std::size_t coordinate = 0U; coordinate < mesh.positions.size(); coordinate += 3U) {
            maximum_x = std::max(maximum_x, mesh.positions[coordinate]);
          }
        }
      }
    }
    CHECK(maximum_x > 1159.0F,
          "a persisted type-38 Boolean operand adds material beyond the host bounds");
    CHECK(host_volume > 41'000'000.0 && host_volume < 43'000'000.0,
          "a hollow type-38 operand adds its material shell rather than its filled envelope");
  }

  const auto swept_boolean_bytes = database_with_boolean_chain(2U, false, false, false, true);
  ModelPackage swept_boolean_package;
  swept_boolean_package.add(
      Asset::copy(AssetRole::model_database, "swept-boolean.db1", swept_boolean_bytes));
  auto swept_boolean_model = open(std::move(swept_boolean_package));
  CHECK(swept_boolean_model.has_value(), "the polyline-host Boolean fixture opens");
  if (swept_boolean_model) {
    CHECK(argc == 2, "the unique-request topology worker is supplied by the OCCT test preset");
    for (const auto mode : {TopologyMode::direct, TopologyMode::supervised}) {
      if (mode == TopologyMode::supervised && argc != 2) continue;
      ProcessRequest request;
      request.stages = Stage::display_geometry;
      request.topology_mode = mode;
      if (mode == TopologyMode::supervised) request.topology_worker_path = argv[1];
      auto processed = swept_boolean_model.value().process(request);
      CHECK(processed.has_value(), "the polyline-host Boolean fixture processes publicly");
      std::size_t mesh_count = 0U;
      std::size_t vertex_count = 0U;
      std::size_t triangle_count = 0U;
      std::size_t diagnostic_count = 0U;
      std::array<float, 6> actual_bounds{
          std::numeric_limits<float>::max(),    std::numeric_limits<float>::max(),
          std::numeric_limits<float>::max(),    std::numeric_limits<float>::lowest(),
          std::numeric_limits<float>::lowest(), std::numeric_limits<float>::lowest()};
      if (processed) {
        while (true) {
          auto batch = processed.value()->next();
          CHECK(batch.has_value(), "polyline-host Boolean batches decode");
          if (!batch || batch.value().kind == BatchKind::end) break;
          if (batch.value().kind == BatchKind::meshes) {
            mesh_count += batch.value().meshes.size();
            for (const auto& mesh : batch.value().meshes) {
              vertex_count += mesh.positions.size() / 3U;
              triangle_count += mesh.indices.size() / 3U;
              for (std::size_t index = 0U; index + 2U < mesh.positions.size(); index += 3U) {
                for (std::size_t axis = 0U; axis < 3U; ++axis) {
                  actual_bounds[axis] = std::min(actual_bounds[axis], mesh.positions[index + axis]);
                  actual_bounds[axis + 3U] =
                      std::max(actual_bounds[axis + 3U], mesh.positions[index + axis]);
                }
              }
            }
          } else if (batch.value().kind == BatchKind::diagnostics) {
            diagnostic_count += batch.value().diagnostics.size();
          }
        }
      }
      constexpr std::array<float, 6> expected_bounds{74.5442F, 2.82843F, 14.0F,
                                                     1125.46F, 550.912F, 46.0F};
      const bool bounds_match = std::equal(
          actual_bounds.begin(), actual_bounds.end(), expected_bounds.begin(),
          [](float actual, float expected) { return std::abs(actual - expected) < 0.01F; });
      CHECK(mesh_count == 1U && vertex_count == 192U && triangle_count == 112U && bounds_match,
            "a polyline host with a persisted cut emits one evaluated display solid");
      CHECK(diagnostic_count == 0U,
            "the retained polyline recipe reaches topology without a fallback diagnostic");
    }
  }

  if (argc == 2) {
    const auto shared_bytes = database_with_boolean_chain(10U, false, true);
    ModelPackage shared_package;
    shared_package.add(
        Asset::copy(AssetRole::model_database, "shared-boolean-graph.db1", shared_bytes));
    auto shared_model = open(std::move(shared_package));
    CHECK(shared_model.has_value(), "the shared Boolean operative graph opens");
    if (shared_model) {
      ProcessRequest request;
      request.stages = Stage::display_geometry;
      request.topology_mode = TopologyMode::supervised;
      request.topology_worker_path = argv[1];
      auto processed = shared_model.value().process(request);
      CHECK(processed.has_value(), "the shared Boolean operative graph processes");
      std::size_t mesh_count = 0U;
      std::size_t deterministic_failure_count = 0U;
      std::size_t cycle_failure_count = 0U;
      std::size_t transient_failure_count = 0U;
      bool saw_repeated_evaluation = false;
      if (processed) {
        while (true) {
          auto batch = processed.value()->next();
          CHECK(batch.has_value(), "shared Boolean operative batches decode");
          if (!batch || batch.value().kind == BatchKind::end) break;
          if (batch.value().kind == BatchKind::meshes) mesh_count += batch.value().meshes.size();
          if (batch.value().kind == BatchKind::diagnostics) {
            for (const auto& diagnostic : batch.value().diagnostics) {
              deterministic_failure_count +=
                  diagnostic.message.find("Synthetic deterministic topology failure") !=
                          std::string::npos
                      ? 1U
                      : 0U;
              cycle_failure_count +=
                  diagnostic.message.find("cycle") != std::string_view::npos ? 1U : 0U;
              transient_failure_count += diagnostic.code == ErrorCode::geometry_timeout ? 1U : 0U;
              saw_repeated_evaluation =
                  saw_repeated_evaluation || diagnostic.code == ErrorCode::internal_error;
            }
          }
        }
      }
      CHECK(mesh_count == 2U, "both hosts referencing one operative emit display meshes");
      CHECK(deterministic_failure_count == 0U,
            "nested CSG nodes are not evaluated as independent worker requests");
      CHECK(cycle_failure_count == 2U,
            "active-path cycle failures prevent reuse of a partial operative result");
      CHECK(transient_failure_count == 0U,
            "a flat CSG request does not expose intermediate worker failures");
      CHECK(!saw_repeated_evaluation,
            "successful and deterministically failing shared operatives reach OCCT only once");
    }
  }

  const auto chamfer_bytes =
      database_with_one_object("100*200", "14", "9.66", false, 7, false, false, false, true);
  ModelPackage chamfer_package;
  chamfer_package.add(
      Asset::copy(AssetRole::model_database, "large-coordinate-chamfer.db1", chamfer_bytes));
  auto chamfer_model = open(std::move(chamfer_package));
  CHECK(chamfer_model.has_value(), "the edge-chamfer fixture opens");
  if (chamfer_model) {
    ProcessRequest request;
    request.stages = Stage::display_geometry;
    request.topology_mode = TopologyMode::direct;
    auto processed = chamfer_model.value().process(request);
    CHECK(processed.has_value(), "edge-chamfer topology processing is available");
    bool saw_trimmed_corner = false;
    bool saw_original_corner = false;
    if (processed) {
      while (true) {
        auto batch = processed.value()->next();
        CHECK(batch.has_value(), "edge-chamfer batches decode");
        if (!batch || batch.value().kind == BatchKind::end) break;
        if (batch.value().kind != BatchKind::meshes || batch.value().meshes.empty()) {
          continue;
        }
        const auto& mesh = batch.value().meshes.front();
        for (std::size_t index = 0; index + 2U < mesh.positions.size(); index += 3U) {
          const float x = mesh.positions[index];
          const float y = mesh.positions[index + 1U];
          saw_trimmed_corner = saw_trimmed_corner || (x > 500999.0F && y < 500026.0F) ||
                               (y > 500049.0F && x < 500976.0F);
          saw_original_corner = saw_original_corner || (x > 500990.0F && y > 500040.0F);
        }
      }
    }
    CHECK(saw_trimmed_corner && !saw_original_corner,
          "large world coordinates retain sub-float chamfer clearance until topology completes");
  }
#endif

  if (failures != 0) {
    std::printf("FAILED (%d)\n", failures);
    return 1;
  }
  std::printf("ALL PASS\n");
  return 0;
}

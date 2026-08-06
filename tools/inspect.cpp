#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <map>
#include <string>
#include <string_view>
#include <tekla/db1/tekla_db1.hpp>

namespace {

struct InstanceTotals {
  std::uint64_t count = 0;
  std::uint64_t persisted_child_count = 0;
  std::uint64_t available_definition_count = 0;
};

[[nodiscard]] const char* wrapper_name(tekla::db1::Wrapper wrapper) noexcept {
  return wrapper == tekla::db1::Wrapper::gzip ? "gzip" : "raw";
}
[[nodiscard]] const char* footer_name(tekla::db1::TableFooter footer) noexcept {
  using tekla::db1::TableFooter;
  switch (footer) {
    case TableFooter::connected:
      return "connected";
    case TableFooter::keyed:
      return "keyed";
    case TableFooter::plain:
      return "plain";
    case TableFooter::final:
      return "final";
  }
  return "unknown";
}

void print_error(const tekla::db1::Error& error) {
  std::fprintf(stderr, "tekla-db1-inspect: %s\n", error.message.c_str());
}

void print_json_string(std::string_view value) {
  std::putchar('"');
  for (const auto raw : value) {
    const auto character = static_cast<unsigned char>(raw);
    if (character == '"' || character == '\\') {
      std::putchar('\\');
      std::putchar(static_cast<int>(character));
    } else if (character >= 0x20U && character < 0x80U) {
      std::putchar(static_cast<int>(character));
    } else {
      std::printf("\\u%04x", static_cast<unsigned>(character));
    }
  }
  std::putchar('"');
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2 || argc > 3) {
    std::fprintf(stderr,
                 "usage: tekla-db1-inspect <model.db1> "
                 "[--tables|--objects|--semantics|--instances|"
                 "--component-definitions|--geometry]\n");
    return 2;
  }
  const bool include_tables = argc == 3 && std::string_view(argv[2]) == "--tables";
  const bool include_objects = argc == 3 && std::string_view(argv[2]) == "--objects";
  const bool include_semantics = argc == 3 && std::string_view(argv[2]) == "--semantics";
  const bool include_instances = argc == 3 && std::string_view(argv[2]) == "--instances";
  const bool include_component_definitions =
      argc == 3 && std::string_view(argv[2]) == "--component-definitions";
  const bool include_geometry = argc == 3 && std::string_view(argv[2]) == "--geometry";
  if (argc == 3 && !include_tables && !include_objects && !include_semantics &&
      !include_instances && !include_component_definitions && !include_geometry) {
    std::fprintf(stderr, "tekla-db1-inspect: unknown option: %s\n", argv[2]);
    return 2;
  }

  const auto started = std::chrono::steady_clock::now();
  const std::filesystem::path path(argv[1]);
  auto package = tekla::db1::map_model_package(path);
  if (!package) {
    print_error(package.error());
    return 1;
  }
  auto model = tekla::db1::open(std::move(package.value()));
  if (!model) {
    print_error(model.error());
    return 1;
  }
  const auto elapsed =
      std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
  const auto& info = model.value().info();
  std::uint64_t object_count = 0;
  std::uint64_t visible_object_count = 0;
  std::uint64_t property_count = 0;
  std::uint64_t relation_count = 0;
  std::uint64_t subelement_count = 0;
  std::uint64_t assembly_membership_count = 0;
  std::uint64_t connection_count = 0;
  std::uint64_t hosted_count = 0;
  std::uint64_t assembly_count = 0;
  std::uint64_t instance_count = 0;
  std::uint64_t persisted_component_child_count = 0;
  std::uint64_t available_component_definition_count = 0;
  std::uint64_t unavailable_component_definition_count = 0;
  std::uint64_t material_count = 0;
  std::uint64_t definition_count = 0;
  std::uint64_t circular_arc_definition_count = 0;
  std::uint64_t polyline_definition_count = 0;
  std::uint64_t mesh_count = 0;
  std::uint64_t curve_count = 0;
  std::uint64_t diagnostic_count = 0;
  std::map<std::pair<std::string, std::string>, InstanceTotals> instance_names;
  double process_elapsed = 0.0;
  if (include_objects || include_semantics || include_instances || include_component_definitions ||
      include_geometry) {
    tekla::db1::ProcessRequest request;
    request.stages =
        include_geometry
            ? tekla::db1::Stage::definition_geometry | tekla::db1::Stage::display_geometry
        : include_component_definitions ? tekla::db1::Stage::component_definitions
        : include_semantics
            ? tekla::db1::Stage::identities | tekla::db1::Stage::properties |
                  tekla::db1::Stage::relations | tekla::db1::Stage::semantic_relations |
                  tekla::db1::Stage::instances
        : include_instances ? tekla::db1::Stage::identities | tekla::db1::Stage::instances
                            : tekla::db1::Stage::identities;
    const auto process_started = std::chrono::steady_clock::now();
    auto stream = model.value().process(request);
    if (!stream) {
      print_error(stream.error());
      return 1;
    }
    while (true) {
      auto batch = stream.value()->next();
      if (!batch) {
        print_error(batch.error());
        return 1;
      }
      if (batch.value().kind == tekla::db1::BatchKind::end) {
        break;
      }
      if (batch.value().kind == tekla::db1::BatchKind::objects) {
        object_count += batch.value().objects.size();
        for (const auto& object : batch.value().objects) {
          visible_object_count += object.visible ? 1U : 0U;
          assembly_count += object.kind == tekla::db1::ObjectKind::assembly ? 1U : 0U;
        }
      } else if (batch.value().kind == tekla::db1::BatchKind::properties) {
        property_count += batch.value().properties.size();
      } else if (batch.value().kind == tekla::db1::BatchKind::relations) {
        relation_count += batch.value().relations.size();
      } else if (batch.value().kind == tekla::db1::BatchKind::semantic_relations) {
        for (const auto& relation : batch.value().semantic_relations) {
          subelement_count +=
              relation.kind == tekla::db1::SemanticRelationKind::subelement ? 1U : 0U;
          assembly_membership_count +=
              relation.kind == tekla::db1::SemanticRelationKind::in_assembly ? 1U : 0U;
          connection_count +=
              relation.kind == tekla::db1::SemanticRelationKind::connects_to ? 1U : 0U;
          hosted_count +=
              relation.kind == tekla::db1::SemanticRelationKind::hosted_on ? 1U : 0U;
        }
      } else if (batch.value().kind == tekla::db1::BatchKind::instances) {
        instance_count += batch.value().instances.size();
        for (const auto& instance : batch.value().instances) {
          persisted_component_child_count += instance.persisted_child_count;
          available_component_definition_count +=
              instance.definition_status == tekla::db1::ComponentDefinitionStatus::available ? 1U
                                                                                             : 0U;
          unavailable_component_definition_count +=
              instance.definition_status == tekla::db1::ComponentDefinitionStatus::unavailable ? 1U
                                                                                               : 0U;
          if (include_instances || include_component_definitions) {
            const auto kind = instance.kind == tekla::db1::InstanceKind::joint
                                  ? std::string("joint")
                                  : std::string("macro");
            auto& totals = instance_names[{kind, std::string(instance.name)}];
            ++totals.count;
            totals.persisted_child_count += instance.persisted_child_count;
            totals.available_definition_count +=
                instance.definition_status == tekla::db1::ComponentDefinitionStatus::available ? 1U
                                                                                               : 0U;
          }
        }
      } else if (batch.value().kind == tekla::db1::BatchKind::materials) {
        material_count += batch.value().materials.size();
      } else if (batch.value().kind == tekla::db1::BatchKind::definition_geometry) {
        definition_count += batch.value().definitions.size();
        for (const auto& definition : batch.value().definitions) {
          circular_arc_definition_count +=
              definition.kind == tekla::db1::DefinitionGeometryKind::circular_arc_extrusion ? 1U
                                                                                            : 0U;
          polyline_definition_count +=
              definition.kind == tekla::db1::DefinitionGeometryKind::polyline_extrusion ? 1U : 0U;
        }
      } else if (batch.value().kind == tekla::db1::BatchKind::meshes) {
        mesh_count += batch.value().meshes.size();
      } else if (batch.value().kind == tekla::db1::BatchKind::curves) {
        curve_count += batch.value().curves.size();
      } else if (batch.value().kind == tekla::db1::BatchKind::diagnostics) {
        diagnostic_count += batch.value().diagnostics.size();
      }
    }
    process_elapsed = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() -
                                                                process_started)
                          .count();
  }

  std::printf("{\n");
  std::printf("  \"wrapper\": \"%s\",\n", wrapper_name(info.wrapper));
  std::printf("  \"product\": \"%s\",\n", info.product.c_str());
  std::printf("  \"kind_marker\": %u,\n", static_cast<unsigned>(info.kind_marker));
  std::printf("  \"internal_format\": \"%s\",\n", info.internal_format.c_str());
  std::printf("  \"database_uuid\": \"%s\",\n", info.database_uuid.c_str());
  std::printf("  \"source_bytes\": %llu,\n", static_cast<unsigned long long>(info.source_bytes));
  std::printf("  \"payload_bytes\": %llu,\n", static_cast<unsigned long long>(info.payload_bytes));
  std::printf("  \"table_count\": %zu,\n", info.tables.size());
  std::printf("  \"visible_rows\": %llu,\n", static_cast<unsigned long long>(info.visible_rows));
  std::printf("  \"invisible_rows\": %llu,\n",
              static_cast<unsigned long long>(info.invisible_rows));
  std::printf("  \"open_milliseconds\": %.3f", elapsed);
  if (include_objects || include_semantics || include_instances || include_component_definitions ||
      include_geometry) {
    std::printf(",\n  \"object_count\": %llu,\n", static_cast<unsigned long long>(object_count));
    std::printf("  \"visible_object_count\": %llu,\n",
                static_cast<unsigned long long>(visible_object_count));
    std::printf("  \"identity_milliseconds\": %.3f", process_elapsed);
    if (include_semantics || include_instances || include_component_definitions) {
      std::printf(",\n  \"property_count\": %llu,\n",
                  static_cast<unsigned long long>(property_count));
      std::printf("  \"relation_count\": %llu,\n", static_cast<unsigned long long>(relation_count));
      std::printf("  \"subelement_count\": %llu,\n",
                  static_cast<unsigned long long>(subelement_count));
      std::printf("  \"assembly_count\": %llu,\n", static_cast<unsigned long long>(assembly_count));
      std::printf("  \"assembly_membership_count\": %llu,\n",
                  static_cast<unsigned long long>(assembly_membership_count));
      std::printf("  \"connection_count\": %llu,\n",
                  static_cast<unsigned long long>(connection_count));
      std::printf("  \"hosted_count\": %llu,\n",
                  static_cast<unsigned long long>(hosted_count));
      std::printf("  \"instance_count\": %llu,\n", static_cast<unsigned long long>(instance_count));
      std::printf("  \"persisted_component_child_count\": %llu,\n",
                  static_cast<unsigned long long>(persisted_component_child_count));
      std::printf("  \"available_component_definition_count\": %llu,\n",
                  static_cast<unsigned long long>(available_component_definition_count));
      std::printf("  \"unavailable_component_definition_count\": %llu,\n",
                  static_cast<unsigned long long>(unavailable_component_definition_count));
      std::printf("  \"material_count\": %llu", static_cast<unsigned long long>(material_count));
      if (include_instances || include_component_definitions) {
        std::printf(",\n  \"instance_names\": [\n");
        std::size_t index = 0;
        for (const auto& [key, totals] : instance_names) {
          std::printf("    {\"kind\": ");
          print_json_string(key.first);
          std::printf(", \"name\": ");
          print_json_string(key.second);
          std::printf(
              ", \"count\": %llu, \"persisted_child_count\": %llu, "
              "\"available_definition_count\": %llu}%s\n",
              static_cast<unsigned long long>(totals.count),
              static_cast<unsigned long long>(totals.persisted_child_count),
              static_cast<unsigned long long>(totals.available_definition_count),
              ++index == instance_names.size() ? "" : ",");
        }
        std::printf("  ]");
      }
    } else if (include_geometry) {
      std::printf(",\n  \"definition_count\": %llu,\n",
                  static_cast<unsigned long long>(definition_count));
      std::printf("  \"circular_arc_definition_count\": %llu,\n",
                  static_cast<unsigned long long>(circular_arc_definition_count));
      std::printf("  \"polyline_definition_count\": %llu,\n",
                  static_cast<unsigned long long>(polyline_definition_count));
      std::printf("  \"mesh_count\": %llu,\n", static_cast<unsigned long long>(mesh_count));
      std::printf("  \"curve_count\": %llu,\n", static_cast<unsigned long long>(curve_count));
      std::printf("  \"diagnostic_count\": %llu",
                  static_cast<unsigned long long>(diagnostic_count));
    }
  }
  if (include_tables) {
    std::printf(",\n  \"tables\": [\n");
    for (std::size_t index = 0; index < info.tables.size(); ++index) {
      const auto& table = info.tables[index];
      std::printf(
          "    {\"ordinal\": %u, \"tuple_size\": %u, \"field_count\": %u, "
          "\"rows\": %llu, \"visible_rows\": %llu, "
          "\"invisible_rows\": %llu, \"footer\": \"%s\"",
          table.ordinal, table.tuple_size, table.field_count,
          static_cast<unsigned long long>(table.row_count),
          static_cast<unsigned long long>(table.visible_rows),
          static_cast<unsigned long long>(table.invisible_rows), footer_name(table.footer));
      if (table.table_key.has_value()) {
        std::printf(", \"table_key\": %u", *table.table_key);
      }
      std::printf("}%s\n", index + 1 == info.tables.size() ? "" : ",");
    }
    std::printf("  ]\n");
  } else {
    std::printf("\n");
  }
  std::printf("}\n");
  return 0;
}

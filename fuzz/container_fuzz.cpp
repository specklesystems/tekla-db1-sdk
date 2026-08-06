#include <cstddef>
#include <cstdint>
#include <span>
#include <tekla/db1/model.hpp>

namespace {

constexpr std::size_t maximum_input_bytes = 64U * 1024U * 1024U;
constexpr std::uint64_t maximum_batches = 4'096;
constexpr std::uint64_t maximum_items = 1'000'000;
constexpr std::uint64_t maximum_elements = 8'000'000;

volatile std::uint64_t fuzz_observation = 0;

bool drain(tekla::db1::BatchReader& reader) {
  std::uint64_t batch_count = 0;
  std::uint64_t item_count = 0;
  std::uint64_t element_count = 0;
  while (batch_count++ < maximum_batches) {
    auto next = reader.next();
    if (!next) return true;
    const auto& batch = next.value();
    if (batch.kind == tekla::db1::BatchKind::end) return true;

    const auto add = [&](std::uint64_t count) {
      if (count > maximum_items - item_count) return false;
      item_count += count;
      return true;
    };
    if (!add(batch.objects.size()) || !add(batch.properties.size()) ||
        !add(batch.relations.size()) || !add(batch.materials.size()) ||
        !add(batch.definitions.size()) || !add(batch.meshes.size()) ||
        !add(batch.curves.size()) || !add(batch.diagnostics.size())) {
      return false;
    }

    const auto add_elements = [&](std::size_t count) {
      if (count > maximum_elements - element_count) return false;
      element_count += count;
      return true;
    };
    for (const auto& mesh : batch.meshes) {
      if (!add_elements(mesh.positions.size()) || !add_elements(mesh.indices.size())) return false;
      fuzz_observation = mesh.object_id ^ mesh.positions.size() ^ mesh.indices.size();
    }
    for (const auto& curve : batch.curves) {
      if (!add_elements(curve.points.size())) return false;
      fuzz_observation = curve.object_id ^ curve.points.size();
    }
    for (const auto& definition : batch.definitions) {
      if (!add_elements(definition.path.size())) return false;
      fuzz_observation = definition.object_id ^ definition.path.size();
    }
    if (!batch.objects.empty()) fuzz_observation = batch.objects.front().internal_id;
    if (!batch.properties.empty()) fuzz_observation = batch.properties.front().object_id;
    if (!batch.relations.empty()) fuzz_observation = batch.relations.front().relation_id;
    if (!batch.materials.empty()) fuzz_observation = batch.materials.front().object_id;
    if (!batch.diagnostics.empty()) fuzz_observation = batch.diagnostics.front().object_id;
  }
  return false;
}

void process_and_drain(tekla::db1::Model& model, tekla::db1::Stage stages) {
  tekla::db1::ProcessRequest request;
  request.stages = stages;
  request.batch_memory_budget_bytes = 64U * 1024U;
  request.topology_mode = tekla::db1::TopologyMode::disabled;
  auto stream = model.process(request);
  if (stream) (void)drain(*stream.value());
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  if (size > maximum_input_bytes) return 0;
  const auto bytes = std::as_bytes(std::span(data, size));
  tekla::db1::ModelPackage package;
  package.add(tekla::db1::Asset::copy(tekla::db1::AssetRole::model_database, "fuzz.db1", bytes));
  tekla::db1::OpenOptions options;
  options.max_payload_bytes = maximum_input_bytes;
  auto model = tekla::db1::open(std::move(package), options);
  if (model) {
    const auto& info = model.value().info();
    fuzz_observation = info.tables.size();
    process_and_drain(model.value(), tekla::db1::Stage::identities |
                                         tekla::db1::Stage::properties |
                                         tekla::db1::Stage::relations);
    process_and_drain(model.value(), tekla::db1::Stage::definition_geometry |
                                         tekla::db1::Stage::display_geometry);
  }
  return 0;
}

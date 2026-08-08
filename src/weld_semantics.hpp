#pragma once

#include <cstdint>
#include <optional>
#include <tekla/db1/process.hpp>
#include <tekla/db1/result.hpp>
#include <vector>

#include "schema.hpp"
#include "storage.hpp"

namespace tekla::db1::detail {

struct WeldCommonSemantics {
  WeldLocation location = WeldLocation::unknown;
  std::uint32_t workshop = 0U;
  std::uint32_t around = 0U;
  std::uint32_t compound = 0U;
  std::uint32_t logical = 0U;
  std::optional<std::uint32_t> intermittent_type;
};

struct WeldSeamSemantics {
  double size = 0.0;
  std::uint32_t type = 0U;
  std::uint32_t intermittent = 0U;
};

struct WeldSemantics {
  std::uint32_t object_id = 0U;
  WeldCommonSemantics common;
  std::optional<WeldSeamSemantics> above;
  std::optional<WeldSeamSemantics> below;
};

// Decodes the persisted weld joins once into output-neutral semantic values.
// An empty result means the dialect/model has no populated native weld tables.
[[nodiscard]] Result<std::vector<WeldSemantics>> load_weld_semantics(const ModelStorage& storage,
                                                                     const Schema& schema);

}  // namespace tekla::db1::detail

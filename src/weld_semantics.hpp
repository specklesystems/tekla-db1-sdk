#pragma once

#include <cstdint>
#include <memory>
#include <tekla/db1/process.hpp>
#include <tekla/db1/result.hpp>
#include <unordered_map>

#include "schema.hpp"
#include "storage.hpp"

namespace tekla::db1::detail {

// Identity retains only the persisted object-to-location join required by the
// object stream; richer weld values remain in the property reader's catalogs.
[[nodiscard]] Result<std::unordered_map<std::uint32_t, WeldLocation>> load_weld_locations(
    const ModelStorage& storage, const Schema& schema);

// Creates a row-at-a-time weld property stream. Common/seam attribute rows are
// indexed once, while decoded weld occurrences and PropertyViews are retained
// only for the current reusable output batch.
[[nodiscard]] Result<ProcessStream> make_weld_semantic_stream(
    std::shared_ptr<const ModelStorage> storage, const Schema& schema,
    const ProcessRequest& request);

}  // namespace tekla::db1::detail

#pragma once

#include "schema.hpp"
#include "storage.hpp"

#include <algorithm>
#include <array>
#include <memory>
#include <tekla/db1/process.hpp>
#include <tekla/db1/result.hpp>

namespace tekla::db1::detail {

// Tekla accepts both thickness*width and width*thickness spellings for the
// rectangular plate families.  Its evaluated section consistently places the
// major dimension on local Y and the minor dimension on local Z.
[[nodiscard]] constexpr std::array<double, 2> normalized_plate_dimensions(
    double first, double second) noexcept {
  return {std::max(first, second), std::min(first, second)};
}

[[nodiscard]] Result<ProcessStream> make_geometry_stream(
    std::shared_ptr<const ModelStorage> storage, const Schema& schema,
    const ProcessRequest& request);

}  // namespace tekla::db1::detail

#pragma once

#include <array>
#include <optional>
#include <span>
#include <string_view>

namespace tekla::db1::detail {

struct StandardProfileMetrics {
  double area = 0.0;
  double cover_perimeter = 0.0;
  double height = 0.0;
  double width = 0.0;
};

// Exact display contours for common, standardized hot-rolled sections,
// captured from evaluated viewer sections (HEIGHT on coordinate 0). These are
// consulted before model-local catalog assets: the parametric contours
// reconstructed from LIS records lack the fillets and flange tapers preserved
// here.
[[nodiscard]] std::optional<std::span<const std::array<double, 2>>> standard_profile_contour(
    std::string_view name) noexcept;

// Nominal engineering values from the corresponding standardized section
// tables. They intentionally differ from measurements of the faceted display
// contours above and are used only for report products.
[[nodiscard]] std::optional<StandardProfileMetrics> standard_profile_metrics(
    std::string_view name) noexcept;

}  // namespace tekla::db1::detail

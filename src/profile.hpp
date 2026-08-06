#pragma once

#include <array>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>
#include <tekla/db1/package.hpp>

namespace tekla::db1::detail {

struct CatalogSection {
  bool hollow = false;
  std::vector<std::array<double, 2>> outer;
  std::vector<std::array<double, 2>> inner;
  // Nominal engineering values from the catalog's general/analysis record.
  // These are intentionally distinct from measurements of the faceted
  // display contour above.
  double report_area = 0.0;
  double report_cover_perimeter = 0.0;
  double report_height = 0.0;
  double report_width = 0.0;
  bool has_report_metrics = false;
};

class LocalProfileCatalog {
 public:
  explicit LocalProfileCatalog(const ModelPackage& package);

  [[nodiscard]] const CatalogSection* find(std::string_view name) const noexcept;

 private:
  std::unordered_map<std::string, CatalogSection> sections_;
};

}  // namespace tekla::db1::detail

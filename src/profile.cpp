#include "profile.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <numbers>
#include <optional>

namespace tekla::db1::detail {
namespace {

[[nodiscard]] std::string normalized(std::string_view value) {
  while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front())) != 0) {
    value.remove_prefix(1);
  }
  while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back())) != 0) {
    value.remove_suffix(1);
  }
  std::string result(value);
  std::transform(result.begin(), result.end(), result.begin(), [](unsigned char character) {
    return static_cast<char>(std::toupper(character));
  });
  return result;
}

[[nodiscard]] std::string normalized_path(std::string_view value) {
  std::string result(value);
  std::replace(result.begin(), result.end(), '\\', '/');
  std::transform(result.begin(), result.end(), result.begin(), [](unsigned char character) {
    return static_cast<char>(std::tolower(character));
  });
  return result;
}

[[nodiscard]] std::optional<double> numeric(std::string_view value) {
  while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front())) != 0) {
    value.remove_prefix(1);
  }
  double result = 0.0;
  const auto parsed = std::from_chars(value.data(), value.data() + value.size(), result);
  if (parsed.ec != std::errc{} || !std::isfinite(result)) return std::nullopt;
  return result;
}

[[nodiscard]] std::optional<double> property(std::string_view record, std::string_view name) {
  const std::string marker = "\"" + std::string(name) + "\"";
  const auto found = record.find(marker);
  if (found == std::string_view::npos) return std::nullopt;
  return numeric(record.substr(found + marker.size()));
}

[[nodiscard]] std::optional<int> profile_type(std::string_view record) {
  std::size_t cursor = 0;
  while ((cursor = record.find("TYPE", cursor)) != std::string_view::npos) {
    const bool left =
        cursor == 0 || (std::isalnum(static_cast<unsigned char>(record[cursor - 1U])) == 0 &&
                        record[cursor - 1U] != '_');
    const auto after = cursor + 4U;
    const bool right =
        after == record.size() || std::isalnum(static_cast<unsigned char>(record[after])) == 0;
    if (left && right) {
      const auto equals = record.find('=', after);
      if (equals == std::string_view::npos) return std::nullopt;
      int result = 0;
      auto value = record.substr(equals + 1U);
      while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front())) != 0) {
        value.remove_prefix(1);
      }
      const auto parsed = std::from_chars(value.data(), value.data() + value.size(), result);
      if (parsed.ec == std::errc{}) return result;
      return std::nullopt;
    }
    cursor = after;
  }
  return std::nullopt;
}

[[nodiscard]] std::size_t round_segment_count(double diameter, double chord_tolerance = 1.0) {
  const double radius = diameter / 2.0;
  if (radius <= chord_tolerance) return 6U;
  const double angular_count = std::numbers::pi / std::acos(1.0 - chord_tolerance / radius);
  const double boundary_count = std::ceil(angular_count) + 1.0;
  return std::max<std::size_t>(6U, 4U * static_cast<std::size_t>(std::ceil(boundary_count / 4.0)));
}

[[nodiscard]] std::vector<std::array<double, 2>> circle(double diameter) {
  const auto segments = round_segment_count(diameter);
  std::vector<std::array<double, 2>> result;
  result.reserve(segments);
  for (std::size_t index = 0; index < segments; ++index) {
    const double angle = std::numbers::pi + 2.0 * std::numbers::pi * static_cast<double>(index) /
                                                static_cast<double>(segments);
    result.push_back({diameter / 2.0 * std::cos(angle), diameter / 2.0 * std::sin(angle)});
  }
  return result;
}

[[nodiscard]] std::optional<CatalogSection> decode_record(std::string_view record, int type) {
  const auto height_value = property(record, "HEIGHT");
  const auto width_value = property(record, "WIDTH");
  const auto diameter = property(record, "DIAMETER");
  const double height =
      type == 6 || type == 7 ? diameter.value_or(0.0) : height_value.value_or(0.0);
  const double width =
      type == 6 || type == 7 ? diameter.value_or(0.0) : width_value.value_or(height);
  if (height <= 0.0 || width <= 0.0) return std::nullopt;
  CatalogSection result;
  const auto report_area = property(record, "CROSS_SECTION_AREA");
  const auto report_cover_area = property(record, "COVER_AREA");
  if (report_area && report_cover_area && *report_area > 0.0 && *report_cover_area > 0.0) {
    result.report_area = *report_area;
    // The LIS field is stored as square millimetres per metre. Convert it to
    // the equivalent millimetres of covered section boundary.
    result.report_cover_perimeter = *report_cover_area / 1000.0;
    result.report_height = height;
    result.report_width = width;
    result.has_report_metrics = true;
  }
  if (type == 1 || type == 11) {
    const auto web = property(record, "WEB_THICKNESS");
    const auto flange = property(record, "FLANGE_THICKNESS");
    if (!web || !flange || *web <= 0.0 || *flange <= 0.0 || *web >= width ||
        2.0 * *flange >= height)
      return std::nullopt;
    const std::array<std::array<double, 2>, 12> conventional{
        {{-width / 2.0, -height / 2.0},
         {width / 2.0, -height / 2.0},
         {width / 2.0, -height / 2.0 + *flange},
         {*web / 2.0, -height / 2.0 + *flange},
         {*web / 2.0, height / 2.0 - *flange},
         {width / 2.0, height / 2.0 - *flange},
         {width / 2.0, height / 2.0},
         {-width / 2.0, height / 2.0},
         {-width / 2.0, height / 2.0 - *flange},
         {-*web / 2.0, height / 2.0 - *flange},
         {-*web / 2.0, -height / 2.0 + *flange},
         {-width / 2.0, -height / 2.0 + *flange}}};
    result.outer.reserve(conventional.size());
    for (const auto coordinate : conventional) {
      result.outer.push_back({coordinate[1], -coordinate[0]});
    }
  } else if (type == 2) {
    const auto first = property(record, "FLANGE_THICKNESS_1");
    const auto second = property(record, "FLANGE_THICKNESS_2");
    if (!first || !second || *first <= 0.0 || *second <= 0.0 || *first >= height ||
        *second >= width)
      return std::nullopt;
    result.outer = {
        {-height / 2.0, -width / 2.0},          {height / 2.0, -width / 2.0},
        {height / 2.0, -width / 2.0 + *second}, {-height / 2.0 + *first, -width / 2.0 + *second},
        {-height / 2.0 + *first, width / 2.0},  {-height / 2.0, width / 2.0}};
  } else if (type == 4 || type == 9) {
    const auto web = property(record, "WEB_THICKNESS");
    const auto flange = property(record, "FLANGE_THICKNESS");
    if (!web || !flange || *web <= 0.0 || *flange <= 0.0 || *web >= width ||
        2.0 * *flange >= height)
      return std::nullopt;
    result.outer = {{-width / 2.0, -height / 2.0},
                    {width / 2.0, -height / 2.0},
                    {width / 2.0, -height / 2.0 + *flange},
                    {-width / 2.0 + *web, -height / 2.0 + *flange},
                    {-width / 2.0 + *web, height / 2.0 - *flange},
                    {width / 2.0, height / 2.0 - *flange},
                    {width / 2.0, height / 2.0},
                    {-width / 2.0, height / 2.0}};
  } else if (type == 5) {
    result.outer = {{-height / 2.0, -width / 2.0},
                    {height / 2.0, -width / 2.0},
                    {height / 2.0, width / 2.0},
                    {-height / 2.0, width / 2.0}};
  } else if (type == 6) {
    result.outer = circle(diameter.value_or(0.0));
  } else if (type == 7) {
    const auto wall = property(record, "PLATE_THICKNESS");
    if (!wall || *wall <= 0.0 || 2.0 * *wall >= width) return std::nullopt;
    result.hollow = true;
    result.outer = circle(width);
    result.inner = circle(width - 2.0 * *wall);
  } else if (type == 8) {
    const auto wall = property(record, "PLATE_THICKNESS");
    if (!wall || *wall <= 0.0 || 2.0 * *wall >= std::min(height, width)) {
      return std::nullopt;
    }
    result.hollow = true;
    result.outer = {{-height / 2.0, -width / 2.0},
                    {height / 2.0, -width / 2.0},
                    {height / 2.0, width / 2.0},
                    {-height / 2.0, width / 2.0}};
    result.inner = {{-(height - 2.0 * *wall) / 2.0, -(width - 2.0 * *wall) / 2.0},
                    {(height - 2.0 * *wall) / 2.0, -(width - 2.0 * *wall) / 2.0},
                    {(height - 2.0 * *wall) / 2.0, (width - 2.0 * *wall) / 2.0},
                    {-(height - 2.0 * *wall) / 2.0, (width - 2.0 * *wall) / 2.0}};
  } else if (type == 10) {
    const auto web = property(record, "WEB_THICKNESS");
    const auto flange = property(record, "FLANGE_THICKNESS");
    if (!web || !flange || *web <= 0.0 || *flange <= 0.0 || *web >= width || *flange >= height)
      return std::nullopt;
    result.outer = {{-width / 2.0, -height / 2.0},
                    {width / 2.0, -height / 2.0},
                    {width / 2.0, -height / 2.0 + *flange},
                    {*web / 2.0, -height / 2.0 + *flange},
                    {*web / 2.0, height / 2.0},
                    {-*web / 2.0, height / 2.0},
                    {-*web / 2.0, -height / 2.0 + *flange},
                    {-width / 2.0, -height / 2.0 + *flange}};
  } else {
    return std::nullopt;
  }
  return result.outer.size() >= 3U ? std::optional{std::move(result)} : std::nullopt;
}

}  // namespace

LocalProfileCatalog::LocalProfileCatalog(const ModelPackage& package) {
  for (const auto& asset : package.assets()) {
    if (asset.source() == nullptr) continue;
    const auto path = normalized_path(asset.logical_name());
    const bool profile_directory = path.find("environment/profiles/") != std::string::npos;
    const bool project_catalog = path == "pg.lis" || path.ends_with("/pg.lis");
    if ((!profile_directory && !project_catalog) || !path.ends_with(".lis")) {
      continue;
    }
    const auto bytes = asset.source()->bytes();
    const std::string_view document(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    std::size_t cursor = 0;
    while ((cursor = document.find("PROFILE_NAME", cursor)) != std::string_view::npos) {
      const auto equals = document.find('=', cursor + 12U);
      const auto quote = equals == std::string_view::npos ? std::string_view::npos
                                                          : document.find('"', equals + 1U);
      const auto quote_end =
          quote == std::string_view::npos ? std::string_view::npos : document.find('"', quote + 1U);
      if (quote_end == std::string_view::npos) break;
      const auto next = document.find("PROFILE_NAME", quote_end + 1U);
      const auto record = document.substr(
          cursor, next == std::string_view::npos ? document.size() - cursor : next - cursor);
      const auto type = profile_type(record);
      if (type) {
        if (auto section = decode_record(record, *type)) {
          sections_.insert_or_assign(
              normalized(document.substr(quote + 1U, quote_end - quote - 1U)), std::move(*section));
        }
      }
      cursor = next == std::string_view::npos ? document.size() : next;
    }
  }
}

const CatalogSection* LocalProfileCatalog::find(std::string_view name) const noexcept {
  const auto found = sections_.find(normalized(name));
  return found == sections_.end() ? nullptr : &found->second;
}

}  // namespace tekla::db1::detail

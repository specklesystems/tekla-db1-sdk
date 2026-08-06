#pragma once

#include <memory>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>
#include <tekla/db1/package.hpp>
#include <tekla/db1/process.hpp>
#include <tekla/db1/result.hpp>

namespace tekla::db1 {

struct OpenOptions {
  bool validate_container = true;
  std::uint64_t max_payload_bytes = 8ULL * 1024ULL * 1024ULL * 1024ULL;
};

enum class Wrapper {
  raw,
  gzip,
};

enum class TableFooter {
  connected,
  keyed,
  plain,
  final,
};

struct TableInfo {
  std::uint32_t ordinal = 0;
  std::uint32_t tuple_size = 0;
  std::uint32_t field_count = 0;
  std::uint64_t row_count = 0;
  std::uint64_t visible_rows = 0;
  std::uint64_t invisible_rows = 0;
  std::optional<std::uint32_t> table_key;
  TableFooter footer = TableFooter::plain;
};

struct ModelInfo {
  Wrapper wrapper = Wrapper::raw;
  std::string product;
  std::uint8_t kind_marker = 0;
  std::string internal_format;
  std::string database_uuid;
  std::uint64_t source_bytes = 0;
  std::uint64_t payload_bytes = 0;
  std::uint64_t visible_rows = 0;
  std::uint64_t invisible_rows = 0;
  std::vector<TableInfo> tables;
};

class Model {
 public:
  Model(Model&&) noexcept;
  Model& operator=(Model&&) noexcept;
  ~Model();

  Model(const Model&) = delete;
  Model& operator=(const Model&) = delete;

  [[nodiscard]] const ModelInfo& info() const noexcept;
  [[nodiscard]] Result<ProcessStream> process(const ProcessRequest& request) const;

 private:
  struct Impl;
  explicit Model(std::unique_ptr<Impl> impl);

  std::unique_ptr<Impl> impl_;

  friend Result<Model> open(ModelPackage package, const OpenOptions& options);
};

[[nodiscard]] Result<Model> open(ModelPackage package, const OpenOptions& options = {});

}  // namespace tekla::db1

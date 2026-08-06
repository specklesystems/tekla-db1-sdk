#pragma once

#include <string>

namespace tekla::db1 {

enum class ErrorCode {
  none = 0,
  invalid_argument,
  missing_model_database,
  decompression_failed,
  resource_limit,
  invalid_container,
  unsupported_format,
  schema_mismatch,
  decoder_unavailable,
  invalid_geometry,
  geometry_timeout,
  geometry_backend_crash,
  invalid_topology,
  io_error,
  internal_error,
};

struct Error {
  ErrorCode code = ErrorCode::none;
  std::string message;
};

}  // namespace tekla::db1

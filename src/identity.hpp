#pragma once

#include "schema.hpp"
#include "storage.hpp"

#include <memory>
#include <tekla/db1/process.hpp>
#include <tekla/db1/result.hpp>

namespace tekla::db1::detail {

[[nodiscard]] ObjectKind object_kind(std::uint32_t type, std::uint32_t subtype) noexcept;

[[nodiscard]] Result<ProcessStream> make_identity_stream(
    std::shared_ptr<const ModelStorage> storage, const Schema& schema,
    const ProcessRequest& request);

}  // namespace tekla::db1::detail

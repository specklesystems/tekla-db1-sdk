#pragma once

#include "schema.hpp"
#include "storage.hpp"

#include <memory>
#include <tekla/db1/process.hpp>
#include <tekla/db1/result.hpp>

namespace tekla::db1::detail {

[[nodiscard]] Result<ProcessStream> make_property_stream(
    std::shared_ptr<const ModelStorage> storage, const Schema& schema,
    const ProcessRequest& request);
[[nodiscard]] Result<ProcessStream> make_relation_stream(
    std::shared_ptr<const ModelStorage> storage, const Schema& schema,
    const ProcessRequest& request);

[[nodiscard]] Result<ProcessStream> make_instance_stream(
    std::shared_ptr<const ModelStorage> storage, const Schema& schema,
    const ProcessRequest& request);

}  // namespace tekla::db1::detail

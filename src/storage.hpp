#pragma once

#include "container.hpp"

#include <tekla/db1/package.hpp>
#include <utility>

namespace tekla::db1::detail {

struct ModelStorage {
  ModelStorage(ModelPackage package_value, Payload payload_value,
               DatabaseLayout layout_value)
      : package(std::move(package_value)),
        payload(std::move(payload_value)),
        layout(std::move(layout_value)) {}

  ModelPackage package;
  Payload payload;
  DatabaseLayout layout;
};

}  // namespace tekla::db1::detail

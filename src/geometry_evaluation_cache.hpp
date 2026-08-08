#pragma once

#include <functional>
#include <memory>

#include "occt/occt.hpp"

namespace tekla::db1::detail {

// Owns completed topology evaluations independently of model traversal. The
// cache canonicalizes model translation, preserves result ownership, and
// applies the bounded-memory and diagnostic policy for this expensive
// evaluator boundary.
class GeometryEvaluationCache {
 public:
  using Evaluator = std::function<Result<OcctMesh>(const OcctRequest&)>;

  GeometryEvaluationCache();
  ~GeometryEvaluationCache();
  GeometryEvaluationCache(GeometryEvaluationCache&&) noexcept;
  GeometryEvaluationCache& operator=(GeometryEvaluationCache&&) noexcept;

  GeometryEvaluationCache(const GeometryEvaluationCache&) = delete;
  GeometryEvaluationCache& operator=(const GeometryEvaluationCache&) = delete;

  [[nodiscard]] Result<OcctMesh> evaluate(const OcctRequest& request, const Evaluator& evaluator);

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace tekla::db1::detail

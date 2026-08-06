#include <cstdint>
#include <tekla/db1/model.hpp>
#include <utility>
#include <vector>

#include "container.hpp"
#include "geometry.hpp"
#include "identity.hpp"
#include "nonpart_geometry.hpp"
#include "schema.hpp"
#include "semantic.hpp"
#include "storage.hpp"

namespace tekla::db1 {

namespace {

class CompositeReader final : public BatchReader {
 public:
  explicit CompositeReader(std::vector<ProcessStream> streams) : streams_(std::move(streams)) {}

  Result<BatchView> next() override {
    while (current_ < streams_.size()) {
      auto batch = streams_[current_]->next();
      if (!batch) {
        return batch;
      }
      if (batch.value().kind != BatchKind::end) {
        return batch;
      }
      streams_[current_].reset();
      ++current_;
    }
    return Result<BatchView>::success(BatchView{.kind = BatchKind::end});
  }

 private:
  std::vector<ProcessStream> streams_;
  std::size_t current_ = 0;
};

class DeferredNonPartReader final : public BatchReader {
 public:
  DeferredNonPartReader(std::shared_ptr<const detail::ModelStorage> storage,
                        const detail::Schema& schema, ProcessRequest request)
      : storage_(std::move(storage)), schema_(&schema), request_(request) {}

  Result<BatchView> next() override {
    if (!stream_) {
      auto opened = detail::make_nonpart_geometry_stream(storage_, *schema_, request_);
      if (!opened) return Result<BatchView>::failure(opened.error());
      stream_ = std::move(opened).value();
    }
    return stream_->next();
  }

 private:
  std::shared_ptr<const detail::ModelStorage> storage_;
  const detail::Schema* schema_ = nullptr;
  ProcessRequest request_;
  ProcessStream stream_;
};

[[nodiscard]] constexpr bool contains(Stage stages, Stage stage) noexcept {
  return (static_cast<std::uint32_t>(stages) & static_cast<std::uint32_t>(stage)) != 0;
}

}  // namespace

struct Model::Impl {
  Impl(ModelPackage package_value, detail::Payload payload_value,
       detail::DatabaseLayout layout_value)
      : storage(std::make_shared<detail::ModelStorage>(
            std::move(package_value), std::move(payload_value), std::move(layout_value))) {}

  std::shared_ptr<const detail::ModelStorage> storage;
};

Model::Model(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
Model::Model(Model&&) noexcept = default;
Model& Model::operator=(Model&&) noexcept = default;
Model::~Model() = default;

const ModelInfo& Model::info() const noexcept { return impl_->storage->layout.info; }

Result<ProcessStream> Model::process(const ProcessRequest& request) const {
  auto schema = detail::validate_schema(impl_->storage->layout);
  if (!schema) {
    return Result<ProcessStream>::failure(schema.error());
  }
  constexpr auto supported = static_cast<std::uint32_t>(Stage::identities) |
                             static_cast<std::uint32_t>(Stage::properties) |
                             static_cast<std::uint32_t>(Stage::relations) |
                             static_cast<std::uint32_t>(Stage::semantic_relations) |
                             static_cast<std::uint32_t>(Stage::instances) |
                             static_cast<std::uint32_t>(Stage::component_definitions) |
                             static_cast<std::uint32_t>(Stage::definition_geometry) |
                             static_cast<std::uint32_t>(Stage::display_geometry);
  if ((static_cast<std::uint32_t>(request.stages) & ~supported) != 0) {
    return Result<ProcessStream>::failure(
        {ErrorCode::decoder_unavailable,
         "One or more requested processing stages are not implemented."});
  }

  std::vector<ProcessStream> streams;
  const auto append = [&](Result<ProcessStream> stream) -> Result<bool> {
    if (!stream) {
      return Result<bool>::failure(stream.error());
    }
    streams.push_back(std::move(stream.value()));
    return Result<bool>::success(true);
  };
  if (contains(request.stages, Stage::identities)) {
    auto result = append(detail::make_identity_stream(impl_->storage, *schema.value(), request));
    if (!result) return Result<ProcessStream>::failure(result.error());
  }
  if (contains(request.stages, Stage::properties)) {
    auto result = append(detail::make_property_stream(impl_->storage, *schema.value(), request));
    if (!result) return Result<ProcessStream>::failure(result.error());
  }
  if (contains(request.stages, Stage::relations)) {
    auto result = append(detail::make_relation_stream(impl_->storage, *schema.value(), request));
    if (!result) return Result<ProcessStream>::failure(result.error());
  }
  if (contains(request.stages, Stage::semantic_relations)) {
    auto result =
        append(detail::make_semantic_relation_stream(impl_->storage, *schema.value(), request));
    if (!result) return Result<ProcessStream>::failure(result.error());
  }
  if (contains(request.stages, Stage::instances) ||
      contains(request.stages, Stage::component_definitions)) {
    auto result = append(detail::make_instance_stream(impl_->storage, *schema.value(), request));
    if (!result) return Result<ProcessStream>::failure(result.error());
  }
  if (contains(request.stages, Stage::definition_geometry) ||
      contains(request.stages, Stage::display_geometry)) {
    auto result = append(detail::make_geometry_stream(impl_->storage, *schema.value(), request));
    if (!result) return Result<ProcessStream>::failure(result.error());
  }
  if (contains(request.stages, Stage::display_geometry)) {
    streams.push_back(
        std::make_unique<DeferredNonPartReader>(impl_->storage, *schema.value(), request));
  }
  return Result<ProcessStream>::success(std::make_unique<CompositeReader>(std::move(streams)));
}

Result<Model> open(ModelPackage package, const OpenOptions& options) {
  const Asset* database = package.find_first(AssetRole::model_database);
  if (database == nullptr || database->source() == nullptr) {
    return Result<Model>::failure(
        {ErrorCode::missing_model_database, "The model package has no model database."});
  }
  auto payload = detail::decode_payload(database->source(), options.max_payload_bytes);
  if (!payload) {
    return Result<Model>::failure(payload.error());
  }
  auto layout = detail::inspect_payload(payload.value(), options.validate_container);
  if (!layout) {
    return Result<Model>::failure(layout.error());
  }

  return Result<Model>::success(Model(std::make_unique<Model::Impl>(
      std::move(package), std::move(payload.value()), std::move(layout.value()))));
}

}  // namespace tekla::db1

#include <algorithm>
#include <tekla/db1/package.hpp>
#include <utility>

namespace tekla::db1 {
namespace {

class OwnedBytes final : public ByteSource {
 public:
  explicit OwnedBytes(std::span<const std::byte> bytes) : bytes_(bytes.begin(), bytes.end()) {}

  [[nodiscard]] std::span<const std::byte> bytes() const noexcept override { return bytes_; }

 private:
  std::vector<std::byte> bytes_;
};

}  // namespace

Asset::Asset(AssetRole role, std::string logical_name, std::shared_ptr<const ByteSource> source)
    : role_(role), logical_name_(std::move(logical_name)), source_(std::move(source)) {}

Asset Asset::copy(AssetRole role, std::string logical_name, std::span<const std::byte> bytes) {
  return Asset(role, std::move(logical_name), std::make_shared<OwnedBytes>(bytes));
}

void ModelPackage::add(Asset asset) { assets_.push_back(std::move(asset)); }

const Asset* ModelPackage::find_first(AssetRole role) const noexcept {
  const auto found = std::find_if(assets_.begin(), assets_.end(),
                                  [role](const Asset& asset) { return asset.role() == role; });
  return found == assets_.end() ? nullptr : &*found;
}

}  // namespace tekla::db1

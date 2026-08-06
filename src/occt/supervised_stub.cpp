#include <utility>

#include "occt.hpp"

namespace tekla::db1::detail {

struct SupervisedOcctHost::Impl {
  Impl(std::filesystem::path, std::uint32_t) {}
};

SupervisedOcctHost::SupervisedOcctHost(std::filesystem::path worker,
                                       std::uint32_t timeout_milliseconds)
    : impl_(std::make_unique<Impl>(std::move(worker), timeout_milliseconds)) {}
SupervisedOcctHost::~SupervisedOcctHost() = default;
SupervisedOcctHost::SupervisedOcctHost(SupervisedOcctHost&&) noexcept = default;
SupervisedOcctHost& SupervisedOcctHost::operator=(SupervisedOcctHost&&) noexcept = default;

Result<OcctMesh> SupervisedOcctHost::evaluate(const OcctRequest&) {
  return Result<OcctMesh>::failure(
      {ErrorCode::decoder_unavailable,
       "The supervised OCCT host is not implemented on this platform."});
}

}  // namespace tekla::db1::detail

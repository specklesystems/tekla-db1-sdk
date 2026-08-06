#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string_view>
#include <tekla/db1/adapters/gltf.hpp>
#include <tekla/db1/tekla_db1.hpp>

namespace {

void print_error(const tekla::db1::Error& error) {
  std::fprintf(stderr, "tekla-db1-glb: %s\n", error.message.c_str());
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 3 && argc != 5) {
    std::fprintf(stderr,
                 "usage: tekla-db1-glb <model.db1> <preview.glb> "
                 "[--topology-worker <path>]\n");
    return 2;
  }
  if (argc == 5 && std::string_view(argv[3]) != "--topology-worker") {
    std::fprintf(stderr, "tekla-db1-glb: unknown option\n");
    return 2;
  }
  const std::filesystem::path input(argv[1]);
  auto package = tekla::db1::map_model_package(input);
  if (!package) {
    print_error(package.error());
    return 1;
  }
  auto model = tekla::db1::open(std::move(package.value()));
  if (!model) {
    print_error(model.error());
    return 1;
  }
  tekla::db1::ProcessRequest request;
  request.stages = tekla::db1::Stage::display_geometry;
  if (argc == 5) {
    request.topology_mode = tekla::db1::TopologyMode::supervised;
    request.topology_worker_path = argv[4];
  }
  auto stream = model.value().process(request);
  if (!stream) {
    print_error(stream.error());
    return 1;
  }
  std::ofstream output(argv[2], std::ios::binary);
  if (!output) {
    std::fprintf(stderr, "tekla-db1-glb: cannot open output file\n");
    return 1;
  }
  auto report = tekla::db1::gltf::write_glb(*stream.value(), output);
  if (!report) {
    print_error(report.error());
    return 1;
  }
  std::printf("wrote %llu meshes and %llu curves (%llu bytes)\n",
              static_cast<unsigned long long>(report.value().mesh_count),
              static_cast<unsigned long long>(report.value().curve_count),
              static_cast<unsigned long long>(report.value().bytes_written));
  return 0;
}

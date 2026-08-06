#include <cstdio>
#include <string_view>
#include <tekla/db1/version.hpp>

static_assert(!tekla::db1::source_revision.empty());
static_assert(tekla::db1::source_revision == TEKLA_DB1_EXPECTED_SOURCE_REVISION);

int main() {
  if (tekla::db1::source_revision == "unknown") {
    std::puts("source revision unavailable; configure with "
              "TEKLA_DB1_SOURCE_REVISION_OVERRIDE for archive builds");
  }
  return 0;
}

if(NOT TEKLA_DB1_ENABLE_SANITIZERS)
  message(FATAL_ERROR
    "TEKLA_DB1_BUILD_FUZZERS requires TEKLA_DB1_ENABLE_SANITIZERS=ON so the core is instrumented")
endif()

include(CheckCXXSourceCompiles)
set(_tekla_db1_saved_required_flags "${CMAKE_REQUIRED_FLAGS}")
set(_tekla_db1_saved_required_link_options "${CMAKE_REQUIRED_LINK_OPTIONS}")
set(CMAKE_REQUIRED_FLAGS "${CMAKE_REQUIRED_FLAGS} -fsanitize=fuzzer")
list(APPEND CMAKE_REQUIRED_LINK_OPTIONS -fsanitize=fuzzer)
check_cxx_source_compiles(
  "#include <cstddef>\n#include <cstdint>\nextern \"C\" int LLVMFuzzerTestOneInput(const std::uint8_t*, std::size_t) { return 0; }"
  TEKLA_DB1_HAS_WORKING_LIBFUZZER)
set(CMAKE_REQUIRED_FLAGS "${_tekla_db1_saved_required_flags}")
set(CMAKE_REQUIRED_LINK_OPTIONS "${_tekla_db1_saved_required_link_options}")
if(NOT TEKLA_DB1_HAS_WORKING_LIBFUZZER)
  message(FATAL_ERROR
    "TEKLA_DB1_BUILD_FUZZERS requires a compiler with a usable libFuzzer compile/link runtime. "
    "Select an LLVM installation that ships the runtime (for example Homebrew LLVM on macOS).")
endif()

add_executable(tekla_db1_container_fuzz container_fuzz.cpp)
target_link_libraries(tekla_db1_container_fuzz PRIVATE tekla_db1::core)
tekla_db1_set_project_warnings(tekla_db1_container_fuzz)
target_compile_options(tekla_db1_container_fuzz PRIVATE
  -fsanitize=fuzzer,address,undefined
  -fno-omit-frame-pointer)
target_link_options(tekla_db1_container_fuzz PRIVATE
  -fsanitize=fuzzer,address,undefined)

add_executable(tekla_db1_fuzz_seed_generator seed_generator.cpp)
target_include_directories(tekla_db1_fuzz_seed_generator PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(tekla_db1_fuzz_seed_generator PRIVATE tekla_db1::core)
tekla_db1_set_project_warnings(tekla_db1_fuzz_seed_generator)

set(_tekla_db1_fuzz_corpus "${CMAKE_CURRENT_BINARY_DIR}/corpus")
add_custom_command(
  OUTPUT "${_tekla_db1_fuzz_corpus}/synthetic-9.66.db1"
  COMMAND ${CMAKE_COMMAND} -E make_directory "${_tekla_db1_fuzz_corpus}"
  COMMAND tekla_db1_fuzz_seed_generator "${_tekla_db1_fuzz_corpus}/synthetic-9.66.db1"
  DEPENDS tekla_db1_fuzz_seed_generator
  VERBATIM)
add_custom_target(tekla_db1_fuzz_corpus ALL
  DEPENDS "${_tekla_db1_fuzz_corpus}/synthetic-9.66.db1")
add_dependencies(tekla_db1_container_fuzz tekla_db1_fuzz_corpus)

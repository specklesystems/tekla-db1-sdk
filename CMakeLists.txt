cmake_minimum_required(VERSION 3.25)

project(
  tekla_db1_sdk
  VERSION 0.1.0
  DESCRIPTION "Native, output-neutral Tekla model database processing SDK"
  LANGUAGES CXX)

include(CTest)
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)
include(cmake/TeklaDb1Warnings.cmake)
find_package(ZLIB REQUIRED)

option(TEKLA_DB1_BUILD_GLTF "Build the built-in glTF/GLB adapter" ON)
option(TEKLA_DB1_BUILD_TOOLS "Build native SDK command-line tools" ON)
option(TEKLA_DB1_BUILD_BENCHMARKS "Build synthetic SDK benchmarks" OFF)
option(TEKLA_DB1_BUILD_FUZZERS "Build libFuzzer input harnesses (Clang only)" OFF)
option(TEKLA_DB1_ENABLE_OCCT "Build the optional OCCT topology evaluator" OFF)
option(TEKLA_DB1_WARNINGS_AS_ERRORS "Treat SDK compiler warnings as errors" OFF)
option(TEKLA_DB1_ENABLE_SANITIZERS "Enable AddressSanitizer and UndefinedBehaviorSanitizer" OFF)
set(
  TEKLA_DB1_SOURCE_REVISION_OVERRIDE
  ""
  CACHE STRING
        "Full 40- or 64-digit hexadecimal source revision to embed when Git metadata is unavailable")
set(
  TEKLA_DB1_EXTRA_ADAPTERS
  ""
  CACHE STRING
        "Semicolon-separated adapter source directories to add to this build")

set(TEKLA_DB1_CONFIGURED_SOURCE_REVISION "")
if(NOT TEKLA_DB1_SOURCE_REVISION_OVERRIDE STREQUAL "")
  set(TEKLA_DB1_CONFIGURED_SOURCE_REVISION
      "${TEKLA_DB1_SOURCE_REVISION_OVERRIDE}")
else()
  find_package(Git QUIET)
  # Do not inherit an unrelated parent repository when configuring a source
  # archive extracted inside another checkout.
  if(Git_FOUND AND EXISTS "${PROJECT_SOURCE_DIR}/.git")
    execute_process(
      COMMAND "${GIT_EXECUTABLE}" rev-parse --verify HEAD
      WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
      RESULT_VARIABLE _tekla_db1_git_result
      OUTPUT_VARIABLE TEKLA_DB1_CONFIGURED_SOURCE_REVISION
      ERROR_QUIET
      OUTPUT_STRIP_TRAILING_WHITESPACE)
    if(NOT _tekla_db1_git_result EQUAL 0)
      set(TEKLA_DB1_CONFIGURED_SOURCE_REVISION "")
    endif()
  endif()
endif()
if(TEKLA_DB1_CONFIGURED_SOURCE_REVISION STREQUAL "")
  set(TEKLA_DB1_CONFIGURED_SOURCE_REVISION "unknown")
else()
  string(TOLOWER "${TEKLA_DB1_CONFIGURED_SOURCE_REVISION}"
                 TEKLA_DB1_CONFIGURED_SOURCE_REVISION)
  string(LENGTH "${TEKLA_DB1_CONFIGURED_SOURCE_REVISION}"
                _tekla_db1_source_revision_length)
  if(NOT TEKLA_DB1_CONFIGURED_SOURCE_REVISION MATCHES "^[0-9a-f]+$" OR
     (NOT _tekla_db1_source_revision_length EQUAL 40 AND
      NOT _tekla_db1_source_revision_length EQUAL 64))
    message(FATAL_ERROR
      "TEKLA_DB1_SOURCE_REVISION_OVERRIDE must be a full 40- or 64-digit hexadecimal revision")
  endif()
endif()
message(STATUS
  "tekla-db1-sdk source revision: ${TEKLA_DB1_CONFIGURED_SOURCE_REVISION}")

add_library(tekla_db1_core STATIC
  src/generated/schemas.cpp
  src/container.cpp
  src/geometry.cpp
  src/identity.cpp
  src/mapped_file.cpp
  src/mesh_placement.cpp
  src/model.cpp
  src/nonpart_geometry.cpp
  src/package.cpp
  src/profile.cpp
  src/role_schema.cpp
  src/semantic.cpp
  src/schema_validation.cpp)
target_sources(tekla_db1_core PRIVATE
  src/shape.cpp
  src/standard_profile.cpp)
add_library(tekla_db1::core ALIAS tekla_db1_core)

configure_file(
  include/tekla/db1/version.hpp.in
  "${PROJECT_BINARY_DIR}/generated/tekla/db1/version.hpp"
  @ONLY)

set_target_properties(tekla_db1_core PROPERTIES
  CXX_STANDARD 20
  CXX_STANDARD_REQUIRED ON
  CXX_EXTENSIONS OFF
  EXPORT_NAME core
  POSITION_INDEPENDENT_CODE ON)

target_include_directories(tekla_db1_core PUBLIC
  $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
  $<BUILD_INTERFACE:${PROJECT_BINARY_DIR}/generated>
  $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_compile_features(tekla_db1_core PUBLIC cxx_std_20)
target_link_libraries(tekla_db1_core PRIVATE ZLIB::ZLIB)
tekla_db1_set_project_warnings(tekla_db1_core)
tekla_db1_enable_sanitizers(tekla_db1_core)

if(TEKLA_DB1_ENABLE_OCCT)
  find_package(OpenCASCADE REQUIRED COMPONENTS
    FoundationClasses ModelingData ModelingAlgorithms)
  target_sources(tekla_db1_core PRIVATE
    src/geometry_evaluation_cache.cpp
    src/occt/evaluator.cpp
    src/occt/protocol.cpp)
  if(UNIX)
    target_sources(tekla_db1_core PRIVATE src/occt/supervised_posix.cpp)
  else()
    target_sources(tekla_db1_core PRIVATE src/occt/supervised_stub.cpp)
  endif()
  target_include_directories(tekla_db1_core SYSTEM PRIVATE ${OpenCASCADE_INCLUDE_DIR})
  target_link_libraries(tekla_db1_core PRIVATE
    ${OpenCASCADE_FoundationClasses_LIBRARIES}
    ${OpenCASCADE_ModelingData_LIBRARIES}
    ${OpenCASCADE_ModelingAlgorithms_LIBRARIES})
  target_compile_definitions(tekla_db1_core PRIVATE TEKLA_DB1_HAS_OCCT=1)
endif()

if(TEKLA_DB1_BUILD_GLTF)
  add_subdirectory(adapters/gltf)
endif()

if(TEKLA_DB1_BUILD_TOOLS)
  add_subdirectory(tools)
endif()

if(TEKLA_DB1_BUILD_BENCHMARKS)
  add_subdirectory(benchmarks)
endif()

if(TEKLA_DB1_BUILD_FUZZERS)
  add_subdirectory(fuzz)
endif()

set(_tekla_db1_adapter_index 0)
foreach(_tekla_db1_adapter IN LISTS TEKLA_DB1_EXTRA_ADAPTERS)
  math(EXPR _tekla_db1_adapter_index "${_tekla_db1_adapter_index} + 1")
  get_filename_component(
    _tekla_db1_adapter_path
    "${_tekla_db1_adapter}"
    ABSOLUTE
    BASE_DIR "${PROJECT_SOURCE_DIR}")
  if(NOT EXISTS "${_tekla_db1_adapter_path}/CMakeLists.txt")
    message(FATAL_ERROR
      "External adapter has no CMakeLists.txt: ${_tekla_db1_adapter_path}")
  endif()
  add_subdirectory(
    "${_tekla_db1_adapter_path}"
    "${PROJECT_BINARY_DIR}/external-adapters/${_tekla_db1_adapter_index}")
endforeach()

if(BUILD_TESTING)
  add_subdirectory(tests)
endif()

install(
  TARGETS tekla_db1_core
  EXPORT tekla_db1Targets
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

if(TEKLA_DB1_BUILD_GLTF)
  install(
    TARGETS tekla_db1_gltf
    EXPORT tekla_db1Targets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()

install(
  FILES
    include/tekla/db1/error.hpp
    include/tekla/db1/model.hpp
    include/tekla/db1/package.hpp
    include/tekla/db1/process.hpp
    include/tekla/db1/result.hpp
    include/tekla/db1/tekla_db1.hpp
  DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/tekla/db1)
install(
  FILES "${PROJECT_BINARY_DIR}/generated/tekla/db1/version.hpp"
  DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/tekla/db1)

if(TEKLA_DB1_BUILD_GLTF)
  install(
    FILES include/tekla/db1/adapters/gltf.hpp
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/tekla/db1/adapters)
endif()

configure_package_config_file(
  cmake/tekla_db1Config.cmake.in
  "${PROJECT_BINARY_DIR}/tekla_db1Config.cmake"
  INSTALL_DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/tekla_db1")
write_basic_package_version_file(
  "${PROJECT_BINARY_DIR}/tekla_db1ConfigVersion.cmake"
  VERSION ${PROJECT_VERSION}
  COMPATIBILITY SameMinorVersion)

install(
  EXPORT tekla_db1Targets
  FILE tekla_db1Targets.cmake
  NAMESPACE tekla_db1::
  DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/tekla_db1")
install(
  FILES
    "${PROJECT_BINARY_DIR}/tekla_db1Config.cmake"
    "${PROJECT_BINARY_DIR}/tekla_db1ConfigVersion.cmake"
  DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/tekla_db1")

install(
  FILES
    README.md
    CHANGELOG.md
    THIRD_PARTY_NOTICES.md
  DESTINATION "${CMAKE_INSTALL_DATADIR}/doc/tekla_db1_sdk")

if(EXISTS "${PROJECT_SOURCE_DIR}/LICENSE")
  install(
    FILES LICENSE
    DESTINATION "${CMAKE_INSTALL_DATADIR}/doc/tekla_db1_sdk")
endif()

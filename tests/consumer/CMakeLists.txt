cmake_minimum_required(VERSION 3.25)
project(tekla_db1_consumer_test LANGUAGES CXX)

include(CTest)

option(TEKLA_DB1_CONSUMER_REQUIRE_GLTF "Consume the optional glTF component" ON)
if(TEKLA_DB1_CONSUMER_REQUIRE_GLTF)
  find_package(tekla_db1 0.1.0 EXACT REQUIRED COMPONENTS core gltf)
else()
  find_package(tekla_db1 0.1.0 EXACT REQUIRED COMPONENTS core)
endif()
if(NOT DEFINED tekla_db1_SOURCE_REVISION OR
   tekla_db1_SOURCE_REVISION STREQUAL "")
  message(FATAL_ERROR "Installed package did not export tekla_db1_SOURCE_REVISION")
endif()

add_executable(tekla_db1_consumer main.cpp)
if(TEKLA_DB1_CONSUMER_REQUIRE_GLTF)
  target_link_libraries(tekla_db1_consumer PRIVATE tekla_db1::gltf)
  target_compile_definitions(tekla_db1_consumer PRIVATE TEKLA_DB1_CONSUMER_WITH_GLTF=1)
else()
  target_link_libraries(tekla_db1_consumer PRIVATE tekla_db1::core)
endif()
target_compile_features(tekla_db1_consumer PRIVATE cxx_std_20)
target_compile_definitions(tekla_db1_consumer PRIVATE
  TEKLA_DB1_EXPECTED_VERSION="${tekla_db1_VERSION}"
  TEKLA_DB1_EXPECTED_VERSION_MAJOR=${tekla_db1_VERSION_MAJOR}
  TEKLA_DB1_EXPECTED_VERSION_MINOR=${tekla_db1_VERSION_MINOR}
  TEKLA_DB1_EXPECTED_VERSION_PATCH=${tekla_db1_VERSION_PATCH}
  TEKLA_DB1_EXPECTED_SOURCE_REVISION="${tekla_db1_SOURCE_REVISION}")
add_test(NAME installed_package_consumer COMMAND tekla_db1_consumer)

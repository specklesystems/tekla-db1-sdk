add_executable(tekla_db1_package_test package_test.cpp)
target_link_libraries(tekla_db1_package_test PRIVATE tekla_db1::core ZLIB::ZLIB)
tekla_db1_set_project_warnings(tekla_db1_package_test)
tekla_db1_enable_sanitizers(tekla_db1_package_test)
add_test(NAME package_contract COMMAND tekla_db1_package_test)

add_executable(tekla_db1_version_test version_test.cpp)
target_link_libraries(tekla_db1_version_test PRIVATE tekla_db1::core)
target_compile_definitions(tekla_db1_version_test PRIVATE
  TEKLA_DB1_EXPECTED_SOURCE_REVISION="${TEKLA_DB1_CONFIGURED_SOURCE_REVISION}")
tekla_db1_set_project_warnings(tekla_db1_version_test)
tekla_db1_enable_sanitizers(tekla_db1_version_test)
add_test(NAME source_revision_contract COMMAND tekla_db1_version_test)

add_executable(tekla_db1_robustness_test robustness_test.cpp)
target_link_libraries(tekla_db1_robustness_test PRIVATE tekla_db1::core ZLIB::ZLIB)
tekla_db1_set_project_warnings(tekla_db1_robustness_test)
tekla_db1_enable_sanitizers(tekla_db1_robustness_test)
add_test(NAME malformed_input_contract COMMAND tekla_db1_robustness_test)

add_executable(tekla_db1_geometry_evaluation_test geometry_evaluation_test.cpp)
target_include_directories(tekla_db1_geometry_evaluation_test PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(tekla_db1_geometry_evaluation_test PRIVATE tekla_db1::core)
tekla_db1_set_project_warnings(tekla_db1_geometry_evaluation_test)
tekla_db1_enable_sanitizers(tekla_db1_geometry_evaluation_test)
add_test(NAME geometry_evaluation_contract COMMAND tekla_db1_geometry_evaluation_test)

add_executable(tekla_db1_identity_test identity_test.cpp)
target_include_directories(tekla_db1_identity_test PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(tekla_db1_identity_test PRIVATE tekla_db1::core)
tekla_db1_set_project_warnings(tekla_db1_identity_test)
tekla_db1_enable_sanitizers(tekla_db1_identity_test)

if(TARGET tekla_db1_gltf)
  add_executable(tekla_db1_gltf_test gltf_writer_test.cpp)
  target_link_libraries(tekla_db1_gltf_test PRIVATE tekla_db1::gltf)
  tekla_db1_set_project_warnings(tekla_db1_gltf_test)
  tekla_db1_enable_sanitizers(tekla_db1_gltf_test)
  add_test(NAME gltf_adapter_contract COMMAND tekla_db1_gltf_test)

  add_executable(tekla_db1_gltf_determinism_fixture gltf_determinism_fixture.cpp)
  target_link_libraries(tekla_db1_gltf_determinism_fixture PRIVATE tekla_db1::gltf)
  tekla_db1_set_project_warnings(tekla_db1_gltf_determinism_fixture)
  tekla_db1_enable_sanitizers(tekla_db1_gltf_determinism_fixture)
  add_test(
    NAME gltf_cross_process_determinism
    COMMAND ${CMAKE_COMMAND}
      -DGLTF_FIXTURE=$<TARGET_FILE:tekla_db1_gltf_determinism_fixture>
      -DOUTPUT_DIR=${CMAKE_CURRENT_BINARY_DIR}/gltf-determinism
      -P ${CMAKE_CURRENT_SOURCE_DIR}/gltf_determinism.cmake)
endif()

if(TEKLA_DB1_ENABLE_OCCT)
  add_executable(tekla_db1_geometry_evaluation_cache_test geometry_evaluation_cache_test.cpp)
  target_include_directories(tekla_db1_geometry_evaluation_cache_test
    PRIVATE ${PROJECT_SOURCE_DIR}/src)
  target_link_libraries(tekla_db1_geometry_evaluation_cache_test PRIVATE tekla_db1::core)
  tekla_db1_set_project_warnings(tekla_db1_geometry_evaluation_cache_test)
  tekla_db1_enable_sanitizers(tekla_db1_geometry_evaluation_cache_test)
  add_test(NAME geometry_evaluation_cache_contract
    COMMAND tekla_db1_geometry_evaluation_cache_test)
endif()

if(TEKLA_DB1_ENABLE_OCCT AND TARGET tekla-db1-occt-worker)
  target_compile_definitions(tekla_db1_identity_test PRIVATE
    TEKLA_DB1_TEST_HAS_OCCT=1)
  add_executable(tekla_db1_occt_unique_worker occt_unique_worker.cpp)
  target_include_directories(tekla_db1_occt_unique_worker PRIVATE ${PROJECT_SOURCE_DIR}/src)
  target_link_libraries(tekla_db1_occt_unique_worker PRIVATE tekla_db1::core)
  tekla_db1_set_project_warnings(tekla_db1_occt_unique_worker)
  tekla_db1_enable_sanitizers(tekla_db1_occt_unique_worker)
  add_test(NAME identity_stream_contract COMMAND tekla_db1_identity_test
    $<TARGET_FILE:tekla_db1_occt_unique_worker>)
  add_executable(tekla_db1_occt_hang_worker occt_fault_worker.cpp)
  target_compile_definitions(tekla_db1_occt_hang_worker PRIVATE
    TEKLA_DB1_TEST_HANG_WORKER=1)
  add_executable(tekla_db1_occt_crash_worker occt_fault_worker.cpp)
  tekla_db1_set_project_warnings(tekla_db1_occt_hang_worker)
  tekla_db1_set_project_warnings(tekla_db1_occt_crash_worker)
  add_executable(tekla_db1_occt_test occt_host_test.cpp)
  target_include_directories(tekla_db1_occt_test PRIVATE ${PROJECT_SOURCE_DIR}/src)
  target_include_directories(tekla_db1_occt_test SYSTEM PRIVATE ${OpenCASCADE_INCLUDE_DIR})
  target_link_libraries(tekla_db1_occt_test PRIVATE tekla_db1::core)
  tekla_db1_set_project_warnings(tekla_db1_occt_test)
  tekla_db1_enable_sanitizers(tekla_db1_occt_test)
  add_test(NAME occt_host_contract COMMAND tekla_db1_occt_test
    $<TARGET_FILE:tekla-db1-occt-worker>
    $<TARGET_FILE:tekla_db1_occt_hang_worker>
    $<TARGET_FILE:tekla_db1_occt_crash_worker>)
else()
  add_test(NAME identity_stream_contract COMMAND tekla_db1_identity_test)
endif()

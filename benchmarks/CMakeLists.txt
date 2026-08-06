add_executable(tekla-db1-container-benchmark container_benchmark.cpp)
target_link_libraries(tekla-db1-container-benchmark PRIVATE tekla_db1::core)
tekla_db1_set_project_warnings(tekla-db1-container-benchmark)
tekla_db1_enable_sanitizers(tekla-db1-container-benchmark)

if(WIN32)
  target_link_libraries(tekla-db1-container-benchmark PRIVATE Psapi)
endif()

if(TEKLA_DB1_ENABLE_OCCT)
  add_executable(tekla-db1-topology-object-benchmark topology_object_benchmark.cpp)
  target_link_libraries(tekla-db1-topology-object-benchmark PRIVATE tekla_db1::core)
  tekla_db1_set_project_warnings(tekla-db1-topology-object-benchmark)
  tekla_db1_enable_sanitizers(tekla-db1-topology-object-benchmark)
endif()

if(BUILD_TESTING)
  add_test(
    NAME container_benchmark_smoke
    COMMAND tekla-db1-container-benchmark --rows 1000 --iterations 2)
  set_tests_properties(container_benchmark_smoke PROPERTIES LABELS benchmark)
endif()

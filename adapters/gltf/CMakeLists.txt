add_library(tekla_db1_gltf STATIC src/gltf.cpp)
add_library(tekla_db1::gltf ALIAS tekla_db1_gltf)

target_link_libraries(tekla_db1_gltf PUBLIC tekla_db1::core)
set_target_properties(tekla_db1_gltf PROPERTIES
  CXX_STANDARD 20
  CXX_STANDARD_REQUIRED ON
  CXX_EXTENSIONS OFF
  EXPORT_NAME gltf
  POSITION_INDEPENDENT_CODE ON)
tekla_db1_set_project_warnings(tekla_db1_gltf)
tekla_db1_enable_sanitizers(tekla_db1_gltf)

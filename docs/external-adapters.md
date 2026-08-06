# External adapters

An adapter consumes only installed public headers and the exported
tekla_db1::core CMake target.

## Independent repository

~~~cmake
find_package(tekla_db1 0.1 REQUIRED COMPONENTS core)

add_library(my_adapter src/my_adapter.cpp)
target_link_libraries(my_adapter PRIVATE tekla_db1::core)
target_compile_features(my_adapter PRIVATE cxx_std_20)
~~~

This is the preferred shape for independently published adapters.

## Optional git submodule

During coordinated development, an adapter repository can be mounted anywhere,
including beneath adapters/, and included without modifying the SDK:

~~~sh
git submodule add <adapter-repository> adapters/speckle
cmake -S . -B build/with-speckle \
  -DTEKLA_DB1_EXTRA_ADAPTERS=adapters/speckle
cmake --build build/with-speckle
~~~

Multiple paths use a CMake semicolon-separated list:

~~~sh
cmake -S . -B build/all \
  -DTEKLA_DB1_EXTRA_ADAPTERS="adapters/speckle;../tekla-usdz-adapter"
~~~

An external adapter must provide a CMakeLists.txt and link against public
targets. It must not include files from src/ or rely on implementation details.

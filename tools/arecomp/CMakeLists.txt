# The build-time half of the project: decode 68000 machine code and emit C++.

add_library(arecomp_core STATIC
  src/toml_lite.cpp
  src/manifest.cpp
  src/image.cpp
  src/analyzer.cpp
  src/bootblock.cpp
  src/codegen.cpp
  src/project.cpp
)

target_include_directories(arecomp_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(arecomp_core PUBLIC amiga_runtime PRIVATE arecomp_warnings)

add_executable(arecomp src/main.cpp)
target_link_libraries(arecomp PRIVATE arecomp_core arecomp_warnings)

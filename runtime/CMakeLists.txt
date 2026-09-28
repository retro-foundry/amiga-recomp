# The runtime half of the project: the Amiga machine the recompiled CPU code
# runs inside. Generated ports link against this and nothing else.

set(ARECOMP_RUNTIME_SOURCES
  memory/guest_memory.cpp
  cpu/decoder.cpp
  cpu/runtime.cpp
  cpu/dispatch.cpp
  cpu/hooks.cpp
)

if(ARECOMP_ENABLE_INTERPRETER)
  list(APPEND ARECOMP_RUNTIME_SOURCES cpu/interpreter.cpp)
endif()

add_library(amiga_runtime STATIC ${ARECOMP_RUNTIME_SOURCES})

target_include_directories(amiga_runtime PUBLIC
  $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/include>)

target_link_libraries(amiga_runtime PRIVATE arecomp_warnings)

if(ARECOMP_ENABLE_INTERPRETER)
  target_compile_definitions(amiga_runtime PUBLIC ARECOMP_ENABLE_INTERPRETER=1)
else()
  target_compile_definitions(amiga_runtime PUBLIC ARECOMP_ENABLE_INTERPRETER=0)
endif()

add_library(amiga_recomp::runtime ALIAS amiga_runtime)

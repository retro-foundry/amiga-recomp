# The runtime half of the project: the Amiga machine the recompiled CPU code
# runs inside. Generated ports link against this and nothing else.
#
# This file is also added directly by generated ports, which live outside the
# repository, so it must not assume it is being built from the top level.

cmake_minimum_required(VERSION 3.20)

get_filename_component(ARECOMP_ROOT_DIR "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)

if(NOT TARGET arecomp_warnings)
  add_library(arecomp_warnings INTERFACE)
  target_compile_definitions(arecomp_warnings INTERFACE _CRT_SECURE_NO_WARNINGS)
  if(MSVC)
    target_compile_options(arecomp_warnings INTERFACE /W4 /permissive- /EHsc)
  else()
    target_compile_options(arecomp_warnings INTERFACE
      -Wall -Wextra -Wshadow -Wno-unused-parameter)
  endif()
endif()

if(NOT DEFINED ARECOMP_ENABLE_INTERPRETER)
  option(ARECOMP_ENABLE_INTERPRETER
         "Compile the reference interpreter in as a development fallback" ON)
endif()

set(ARECOMP_RUNTIME_SOURCES
  ${CMAKE_CURRENT_LIST_DIR}/memory/guest_memory.cpp
  ${CMAKE_CURRENT_LIST_DIR}/cpu/decoder.cpp
  ${CMAKE_CURRENT_LIST_DIR}/cpu/runtime.cpp
  ${CMAKE_CURRENT_LIST_DIR}/cpu/dispatch.cpp
  ${CMAKE_CURRENT_LIST_DIR}/cpu/hooks.cpp
)

if(ARECOMP_ENABLE_INTERPRETER)
  list(APPEND ARECOMP_RUNTIME_SOURCES ${CMAKE_CURRENT_LIST_DIR}/cpu/interpreter.cpp)
endif()

add_library(amiga_runtime STATIC ${ARECOMP_RUNTIME_SOURCES})

target_include_directories(amiga_runtime PUBLIC
  $<BUILD_INTERFACE:${ARECOMP_ROOT_DIR}/include>)

target_link_libraries(amiga_runtime PRIVATE arecomp_warnings)

if(ARECOMP_ENABLE_INTERPRETER)
  target_compile_definitions(amiga_runtime PUBLIC ARECOMP_ENABLE_INTERPRETER=1)
else()
  target_compile_definitions(amiga_runtime PUBLIC ARECOMP_ENABLE_INTERPRETER=0)
endif()

add_library(amiga_recomp::runtime ALIAS amiga_runtime)

cmake_minimum_required(VERSION 3.20)
project(amiga_recomp LANGUAGES CXX VERSION 0.1.0)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "" FORCE)
endif()

option(ARECOMP_BUILD_TESTS "Build the unit and differential tests" ON)
option(ARECOMP_WITH_SDL "Build the SDL2 platform backend" OFF)
option(ARECOMP_WITH_MUSASHI "Use an external Musashi build as a CPU oracle" OFF)
option(ARECOMP_ENABLE_INTERPRETER
       "Compile the reference interpreter in as a development fallback" ON)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

add_library(arecomp_warnings INTERFACE)
if(MSVC)
  target_compile_options(arecomp_warnings INTERFACE /W4 /permissive- /EHsc)
else()
  target_compile_options(arecomp_warnings INTERFACE
    -Wall -Wextra -Wshadow -Wno-unused-parameter)
endif()

add_subdirectory(runtime)
add_subdirectory(tools/arecomp)

if(ARECOMP_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

message(STATUS "amiga-recomp ${PROJECT_VERSION}")
message(STATUS "  tests       : ${ARECOMP_BUILD_TESTS}")
message(STATUS "  interpreter : ${ARECOMP_ENABLE_INTERPRETER}")
message(STATUS "  SDL backend : ${ARECOMP_WITH_SDL}")
message(STATUS "  Musashi     : ${ARECOMP_WITH_MUSASHI}")

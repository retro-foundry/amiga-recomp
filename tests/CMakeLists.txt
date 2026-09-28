function(arecomp_add_test name)
  add_executable(${name} ${ARGN})
  target_link_libraries(${name} PRIVATE amiga_runtime arecomp_warnings)
  target_include_directories(${name} PRIVATE
    ${CMAKE_SOURCE_DIR}/tests
    ${CMAKE_SOURCE_DIR}/tools/arecomp/src)
  add_test(NAME ${name} COMMAND ${name})
endfunction()

arecomp_add_test(test_decoder cpu/decoder_test.cpp)
arecomp_add_test(test_semantics cpu/semantics_test.cpp)
arecomp_add_test(test_memory memory/guest_memory_test.cpp)

function(arecomp_add_test name)
  add_executable(${name} ${ARGN})
  target_link_libraries(${name} PRIVATE amiga_runtime arecomp_warnings)
  target_include_directories(${name} PRIVATE
    ${CMAKE_SOURCE_DIR}/tests
    ${CMAKE_SOURCE_DIR}/tools/arecomp/src)
  target_compile_definitions(${name} PRIVATE
    ARECOMP_TESTROM_DIR="${CMAKE_SOURCE_DIR}/testroms/synthetic"
    ARECOMP_PORTS_DIR="${CMAKE_SOURCE_DIR}/ports")
  add_test(NAME ${name} COMMAND ${name})
endfunction()

arecomp_add_test(test_decoder cpu/decoder_test.cpp)
arecomp_add_test(test_semantics cpu/semantics_test.cpp)
arecomp_add_test(test_memory memory/guest_memory_test.cpp)
arecomp_add_test(test_interpreter integration/interpreter_test.cpp)
arecomp_add_test(test_isa_sweep_values integration/isa_sweep_test.cpp)
arecomp_add_test(test_chipset chipset/chipset_test.cpp)
arecomp_add_test(test_widescreen chipset/widescreen_test.cpp)
arecomp_add_test(test_disk chipset/disk_test.cpp)
arecomp_add_test(test_copper_demo integration/copper_demo_test.cpp)
arecomp_add_test(test_manifest integration/manifest_test.cpp)
target_link_libraries(test_manifest PRIVATE arecomp_core)

# --------------------------------------------------------------------------
# Differential testing against generated code.
#
# The recompiler is run at build time and its output is compiled straight into
# the test binary, so the comparison always covers the translator as it is now,
# not a checked-in snapshot of what it once produced.
# --------------------------------------------------------------------------

function(arecomp_add_differential_test name manifest testrom image work_base work_size)
  set(out_dir ${CMAKE_BINARY_DIR}/recomp/${name})
  set(generated
    ${out_dir}/generated/blocks.hpp
    ${out_dir}/generated/blocks_000.cpp
    ${out_dir}/generated/block_table.cpp)

  add_custom_command(
    OUTPUT ${generated}
    COMMAND $<TARGET_FILE:arecomp> recompile ${manifest} -o ${out_dir}
    DEPENDS arecomp ${manifest} ${testrom}
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    COMMENT "arecomp: translating ${name}"
    VERBATIM)

  add_custom_target(${name}_generate DEPENDS ${generated})

  add_executable(test_${name} differential/differential_test.cpp
    ${out_dir}/generated/blocks_000.cpp
    ${out_dir}/generated/block_table.cpp)
  add_dependencies(test_${name} ${name}_generate)
  target_include_directories(test_${name} PRIVATE
    ${CMAKE_SOURCE_DIR}/tests
    ${out_dir}/generated)
  target_compile_definitions(test_${name} PRIVATE
    ARECOMP_TESTROM_DIR="${CMAKE_SOURCE_DIR}/testroms/synthetic"
    ARECOMP_DIFF_IMAGE="${image}"
    ARECOMP_DIFF_WORK_BASE=${work_base}
    ARECOMP_DIFF_WORK_SIZE=${work_size})
  target_link_libraries(test_${name} PRIVATE amiga_runtime arecomp_warnings)
  add_test(NAME test_${name} COMMAND test_${name})
endfunction()

arecomp_add_differential_test(vertical_slice
  ${CMAKE_SOURCE_DIR}/ports/vertical-slice.toml
  ${CMAKE_SOURCE_DIR}/testroms/synthetic/vertical_slice.bin
  vertical_slice.bin 0x2000 64)

arecomp_add_differential_test(isa_sweep
  ${CMAKE_SOURCE_DIR}/ports/isa-sweep.toml
  ${CMAKE_SOURCE_DIR}/testroms/synthetic/isa_sweep.bin
  isa_sweep.bin 0x3000 256)

# Host test suite, built by host/CMakeLists.txt and run with CTest:
#
#   cmake -S host -B build-host && cmake --build build-host
#   ctest --test-dir build-host --output-on-failure
#
# Each test_*.c is a self-contained program using the harness in test.h;
# a non-zero exit status is a failure.

set(SS_FW_DIR ${CMAKE_CURRENT_LIST_DIR}/../firmware)

# Core tests
foreach(name synth smf)
    add_executable(test_${name} test_${name}.c)
    target_link_libraries(test_${name} ss_core)
    add_test(NAME ${name} COMMAND test_${name})
endforeach()

# The firmware's MIDI parsers (free of Pico SDK calls)
add_executable(test_midi_parse test_midi_parse.c ${SS_FW_DIR}/midi_parse.c)
target_include_directories(test_midi_parse PRIVATE ${SS_FW_DIR})
target_compile_options(test_midi_parse PRIVATE -Wall -Wextra)
add_test(NAME midi_parse COMMAND test_midi_parse)

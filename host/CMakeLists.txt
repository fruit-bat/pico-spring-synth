# Host (PC) build: the reference renderer and the test suite.
#
#   cmake -S host -B build-host && cmake --build build-host
#   ctest --test-dir build-host --output-on-failure
#
# Builds the same core/ sources as the firmware, for the PC:
#   ssrender  renders a MIDI file (or a patch audition) to WAV
# ss_config.h defaults apply; override with -D options as on the device.
# Configure with -DSS_SANITIZE=ON to build everything with AddressSanitizer
# and UndefinedBehaviorSanitizer (use a separate build directory).
#
cmake_minimum_required(VERSION 3.13)
project(pico_spring_synth_host C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

option(SS_SANITIZE "Build with AddressSanitizer and UndefinedBehaviorSanitizer" OFF)
if(SS_SANITIZE)
    add_compile_options(-fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address,undefined)
endif()

set(SS_CORE_DIR ${CMAKE_CURRENT_LIST_DIR}/../core)

# The synthesizer: strings, patches, effects and the MIDI file player
add_library(ss_core STATIC
    ${SS_CORE_DIR}/ss_synth.c
    ${SS_CORE_DIR}/ss_patch.c
    ${SS_CORE_DIR}/ss_effects.c
    ${SS_CORE_DIR}/ss_smf.c
    ${SS_CORE_DIR}/ss_crc32.c
)
target_include_directories(ss_core PUBLIC ${SS_CORE_DIR})
target_compile_options(ss_core PUBLIC -Wall -Wextra)
target_link_libraries(ss_core PUBLIC m)

# ssrender -t uses a second thread to mirror the Pico's dual core render
add_executable(ssrender ssrender.c)
find_package(Threads REQUIRED)
target_link_libraries(ssrender ss_core Threads::Threads)

enable_testing()
add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/../tests tests)

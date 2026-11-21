# pico-spring-synth firmware (RP2350).
#
#   cmake -S . -B build -DSS_AUDIO_OUT=I2S      (or PWM)
#   cmake --build build
#
cmake_minimum_required(VERSION 3.13)

if(NOT PICO_BOARD)
    set(PICO_BOARD waveshare_rp2350_plus_16mb)
endif()

include(pico_sdk_import.cmake)

project(pico_spring_synth C CXX ASM)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

pico_sdk_init()

# ---------------------------------------------------------------------------
# Configuration
# ---------------------------------------------------------------------------

set(SS_AUDIO_OUT "I2S" CACHE STRING "Audio output: I2S or PWM")
set_property(CACHE SS_AUDIO_OUT PROPERTY STRINGS I2S PWM)
set(SS_SAMPLE_RATE 44100 CACHE STRING "Output sample rate in Hz")
set(SS_SYS_CLOCK_KHZ 150000 CACHE STRING "System clock in kHz (the RP2350 is rated 150000; above that the core voltage is raised)")
# The CPU budget, in partials with each string counting 3 (measured with
# SS_BENCH: a partial costs 11 to 15 cycles per sample, a string 35). 280
# per 150 MHz keeps the worst block of a dense song on the Pico below about
# 75% of each core, leaving room for the effects and for bursts of MIDI.
# Empty = scaled with the clock.
set(SS_COST_BUDGET "" CACHE STRING "Render budget: partials + 3 x strings (empty: 280 per 150 MHz)")
set(SS_MAX_STRINGS "" CACHE STRING "Strings (sounding channel + key) at once (empty: 48 per 150 MHz, at most 160)")
if(SS_COST_BUDGET STREQUAL "")
    math(EXPR SS_COST_BUDGET_VALUE "${SS_SYS_CLOCK_KHZ} * 280 / 150000")
else()
    set(SS_COST_BUDGET_VALUE ${SS_COST_BUDGET})
endif()
if(SS_MAX_STRINGS STREQUAL "")
    math(EXPR SS_MAX_STRINGS_VALUE "${SS_SYS_CLOCK_KHZ} * 48 / 150000")
    if(SS_MAX_STRINGS_VALUE GREATER 160)
        set(SS_MAX_STRINGS_VALUE 160)
    endif()
else()
    set(SS_MAX_STRINGS_VALUE ${SS_MAX_STRINGS})
endif()
option(SS_DUAL_CORE "Render strings on both cores" ON)

set(SS_I2S_DATA_PIN 12 CACHE STRING "I2S data out GPIO")
set(SS_I2S_CLOCK_PIN_BASE 13 CACHE STRING "I2S BCLK GPIO (LRCLK is the next GPIO); with MCLK, LRCLK GPIO (BCLK is the next)")
set(SS_I2S_MCLK_PIN -1 CACHE STRING "I2S MCLK GPIO (256 x sample rate), -1 for none")
set(SS_PWM_PIN_L 20 CACHE STRING "PWM left GPIO (must be channel A of a slice)")
set(SS_PWM_PIN_R 21 CACHE STRING "PWM right GPIO (channel B of the same slice)")
set(SS_MIDI_UART_RX_PIN 5 CACHE STRING "MIDI in UART RX GPIO (UART1)")

# Waveshare Pico-Audio (PCM5101A version) with a Pico-pinout board plugged on
# top: DIN 26, BCK 27, LRCK 28. Overrides the I2S pins above.
option(SS_PICO_AUDIO "I2S pins for a stacked Waveshare Pico-Audio (PCM5101A)" OFF)
# Waveshare Pico-Audio Rev2.1 (CS4344): DIN 22, MCLK 26, LRCK 27, SCLK 28.
# Overrides the I2S pins above and turns on MCLK.
option(SS_PICO_AUDIO_V21 "I2S pins and MCLK for a stacked Waveshare Pico-Audio Rev2.1 (CS4344)" OFF)
if(SS_PICO_AUDIO AND SS_PICO_AUDIO_V21)
    message(FATAL_ERROR "SS_PICO_AUDIO and SS_PICO_AUDIO_V21 are exclusive")
endif()
if(SS_PICO_AUDIO)
    set(SS_AUDIO_OUT I2S)
    set(SS_I2S_DATA_PIN 26)
    set(SS_I2S_CLOCK_PIN_BASE 27)
    set(SS_I2S_MCLK_PIN -1)
endif()
if(SS_PICO_AUDIO_V21)
    set(SS_AUDIO_OUT I2S)
    set(SS_I2S_DATA_PIN 22)
    set(SS_I2S_CLOCK_PIN_BASE 27)
    set(SS_I2S_MCLK_PIN 26)
endif()
if(SS_I2S_MCLK_PIN GREATER_EQUAL 0)
    message(STATUS "pico-spring-synth: ${PICO_BOARD}, ${SS_AUDIO_OUT}, I2S data ${SS_I2S_DATA_PIN} LRCLK ${SS_I2S_CLOCK_PIN_BASE} BCLK +1 MCLK ${SS_I2S_MCLK_PIN}")
else()
    message(STATUS "pico-spring-synth: ${PICO_BOARD}, ${SS_AUDIO_OUT}, I2S data ${SS_I2S_DATA_PIN} BCLK ${SS_I2S_CLOCK_PIN_BASE} LRCLK +1")
endif()
message(STATUS "pico-spring-synth: ${SS_SYS_CLOCK_KHZ} kHz, ${SS_MAX_STRINGS_VALUE} strings, cost budget ${SS_COST_BUDGET_VALUE}")

set(SS_DEMO_MIDI "" CACHE FILEPATH "Optional MIDI file embedded and played at boot until MIDI input arrives")

# ---------------------------------------------------------------------------
# Firmware
# ---------------------------------------------------------------------------

set(SS_CORE_DIR ${CMAKE_CURRENT_LIST_DIR}/core)
set(SS_FW_DIR ${CMAKE_CURRENT_LIST_DIR}/firmware)

add_executable(pico_spring_synth
    ${SS_FW_DIR}/main.c
    ${SS_FW_DIR}/midi_in.c
    ${SS_FW_DIR}/midi_parse.c
    ${SS_FW_DIR}/settings.c
    ${SS_FW_DIR}/usb_descriptors.c
    ${SS_CORE_DIR}/ss_synth.c
    ${SS_CORE_DIR}/ss_patch.c
    ${SS_CORE_DIR}/ss_effects.c
    ${SS_CORE_DIR}/ss_smf.c
    ${SS_CORE_DIR}/ss_crc32.c
)

# Render micro benchmarks, printed once at boot when the console connects
option(SS_BENCH "Build the render micro benchmarks into the firmware" OFF)
if(SS_BENCH)
    target_sources(pico_spring_synth PRIVATE ${SS_FW_DIR}/bench.c)
    target_compile_definitions(pico_spring_synth PRIVATE SS_BENCH=1)
endif()

if(SS_AUDIO_OUT STREQUAL "I2S")
    target_sources(pico_spring_synth PRIVATE ${SS_FW_DIR}/audio_i2s.c)
    if(SS_I2S_MCLK_PIN GREATER_EQUAL 0)
        pico_generate_pio_header(pico_spring_synth ${SS_FW_DIR}/audio_i2s_mclk.pio)
    else()
        pico_generate_pio_header(pico_spring_synth ${SS_FW_DIR}/audio_i2s.pio)
    endif()
    target_link_libraries(pico_spring_synth hardware_pio)
elseif(SS_AUDIO_OUT STREQUAL "PWM")
    target_sources(pico_spring_synth PRIVATE ${SS_FW_DIR}/audio_pwm.c)
    target_link_libraries(pico_spring_synth hardware_pwm)
else()
    message(FATAL_ERROR "SS_AUDIO_OUT must be I2S or PWM")
endif()

# Embed an optional demo MIDI file as a C array
set(SS_DEMO_C ${CMAKE_CURRENT_BINARY_DIR}/demo_midi.c)
if(SS_DEMO_MIDI)
    file(READ ${SS_DEMO_MIDI} SS_DEMO_HEX HEX)
    string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," SS_DEMO_BYTES "${SS_DEMO_HEX}")
    file(WRITE ${SS_DEMO_C} "#include <stdint.h>\n#include <stddef.h>\nconst uint8_t ss_demo_midi[] = {${SS_DEMO_BYTES}};\nconst size_t ss_demo_midi_size = sizeof(ss_demo_midi);\n")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${SS_DEMO_MIDI})
else()
    file(WRITE ${SS_DEMO_C} "#include <stdint.h>\n#include <stddef.h>\nconst uint8_t ss_demo_midi[1] = {0};\nconst size_t ss_demo_midi_size = 0;\n")
endif()
target_sources(pico_spring_synth PRIVATE ${SS_DEMO_C})

target_include_directories(pico_spring_synth PRIVATE ${SS_CORE_DIR} ${SS_FW_DIR})

target_compile_definitions(pico_spring_synth PRIVATE
    SS_AUDIO_OUT_${SS_AUDIO_OUT}=1
    SS_SAMPLE_RATE=${SS_SAMPLE_RATE}
    SS_SYS_CLOCK_KHZ=${SS_SYS_CLOCK_KHZ}
    SS_MAX_STRINGS=${SS_MAX_STRINGS_VALUE}
    SS_COST_BUDGET=${SS_COST_BUDGET_VALUE}
    SS_DUAL_CORE=$<BOOL:${SS_DUAL_CORE}>
    SS_I2S_DATA_PIN=${SS_I2S_DATA_PIN}
    SS_I2S_CLOCK_PIN_BASE=${SS_I2S_CLOCK_PIN_BASE}
    SS_I2S_MCLK_PIN=${SS_I2S_MCLK_PIN}
    SS_PWM_PIN_L=${SS_PWM_PIN_L}
    SS_PWM_PIN_R=${SS_PWM_PIN_R}
    SS_MIDI_UART_RX_PIN=${SS_MIDI_UART_RX_PIN}
    PICO_STDIO_USB_CONNECT_WAIT_TIMEOUT_MS=0
    # Opening the console at 1200 baud reboots into BOOTSEL (the SDK leaves
    # this off when the application has its own USB descriptors)
    PICO_ENABLE_USB_RESET_VIA_BAUD_RATE=1
    PICO_CORE1_STACK_SIZE=0x1000
)

# The synth core is float heavy: keep it fast. -ffp-contract=off stops GCC
# fusing multiply + add into VFMA, which costs more than a VMUL and a VADD
# on the RP2350 (measured in pico-fsynth).
set_source_files_properties(
    ${SS_CORE_DIR}/ss_synth.c ${SS_CORE_DIR}/ss_effects.c
    PROPERTIES COMPILE_OPTIONS "-O3;-fno-math-errno;-ffp-contract=off"
)

target_link_libraries(pico_spring_synth
    pico_stdlib
    pico_multicore
    pico_unique_id
    hardware_flash
    hardware_dma
    hardware_uart
    hardware_clocks
    hardware_vreg
    tinyusb_device
)

pico_enable_stdio_usb(pico_spring_synth 1)
pico_enable_stdio_uart(pico_spring_synth 0)
pico_add_extra_outputs(pico_spring_synth)

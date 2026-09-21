# Standalone PSRAM diagnostics for the ClockworkPi PicoCalc.
#
# Not part of the emulator build. Each target is its own firmware: flash one,
# read the result over USB CDC, flash the next. They exist because diagnosing
# the PSRAM from inside picocalc_nes means a wrong guess leaves the board
# unbootable with nothing on screen - every tool here bounds its transfers so a
# bad configuration reports a line instead of freezing.
#
#   cd samples/v3/tools/psram-test
#   mkdir -p build && cd build
#   PICO_SDK_PATH=~/Source/pico-sdk cmake .. -DPICO_BOARD=pico2
#   cmake --build . -j$(nproc)

cmake_minimum_required(VERSION 3.13)
set(CMAKE_C_STANDARD 11)

include(pico_sdk_import.cmake)
project(psram-test C CXX ASM)

set(CMAKE_BUILD_TYPE "Release")
# Run from SRAM so flash/XIP timing is never a variable in these numbers.
set(PICO_COPY_TO_RAM 1)

pico_sdk_init()

# The vendored library, which on this branch carries QPI support.
add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/../../rp2040-psram rp2040-psram)

# ClockworkPi PicoCalc mainboard PSRAM (ESP-PSRAM64H).
# Quad mode uses MOSI+1/+2/+3, i.e. SIO1=GP3, SIO2=GP4, SIO3=GP5.
set(PSRAM_DEFS
    PSRAM_SPINLOCK=1
    PSRAM_ASYNC=1
    PSRAM_PIN_CS=20
    PSRAM_PIN_SCK=21
    PSRAM_PIN_MOSI=2
    PSRAM_PIN_MISO=3
)

function(psram_tool name src)
    add_executable(${name} ${src})
    target_compile_definitions(${name} PRIVATE ${PSRAM_DEFS})
    target_link_libraries(${name}
        rp2040-psram pico_stdlib hardware_pio hardware_dma hardware_pll
        hardware_vreg hardware_clocks hardware_watchdog)
    pico_enable_stdio_usb(${name} 1)
    pico_enable_stdio_uart(${name} 1)
    pico_add_extra_outputs(${name})
endfunction()

psram_tool(psram-bench    bench.c)      # SPI vs QPI throughput and integrity
psram_tool(psram-probe    probe.c)      # Read ID both modes + cross-mode r/w
psram_tool(psram-realign  realign.c)    # proves the one-nibble read offset
psram_tool(psram-variants variants2.c)  # PIO read-phase variant matrix
pico_generate_pio_header(psram-variants ${CMAKE_CURRENT_LIST_DIR}/qv.pio)

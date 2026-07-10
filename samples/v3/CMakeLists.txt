cmake_minimum_required(VERSION 3.12)

# Pull in PICO SDK (must be before project)
set(PICO_SDK_PATH $ENV{PICO_SDK_PATH})
include(${PICO_SDK_PATH}/pico_sdk_init.cmake)

project(picocalc_nes C CXX ASM)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

# Initialize the SDK
pico_sdk_init()

add_executable(picocalc_nes)

pico_generate_pio_header(picocalc_nes ${CMAKE_CURRENT_LIST_DIR}/picocalc.pio)

# rp2040-psram: CS=GP20, SCK=GP21, MOSI=GP2, MISO=GP3 on PIO1 (large-ROM path)
set(PICO_SDK true)
add_subdirectory(rp2040-psram)
pico_generate_pio_header(picocalc_nes ${CMAKE_CURRENT_LIST_DIR}/rp2040-psram/psram_spi.pio)

set(SHAPONES_PATH ../../core)
set(FATFS_PATH ../fatfs/source)

file(GLOB SHAPONES_SOURCES ${SHAPONES_PATH}/src/*.cpp)

target_sources(
    picocalc_nes PRIVATE
    common.cpp
    picocalc.cpp
    i2ckbd.cpp
    picocalc_nes.cpp
    boot_menu.cpp
    host_intf.cpp
    psram_loader.cpp
    ${SHAPONES_SOURCES}
    ${FATFS_PATH}/ff.c
    ${FATFS_PATH}/ffsystem.c
    ${FATFS_PATH}/ffunicode.c
    ${FATFS_PATH}/mmc_pico_spi.c
    ../font/mono8x16/mono8x16.cpp
    ../font/mono8x16/mono8x16_bmp.cpp
)

target_include_directories(
    picocalc_nes PRIVATE
    .
    ${SHAPONES_PATH}/include
    ${FATFS_PATH}
    ../font/mono8x16
)

target_link_libraries(
    picocalc_nes PRIVATE
    pico_stdlib
    hardware_pio
    hardware_pwm
    hardware_dma
    hardware_irq
    hardware_spi
    hardware_i2c
    pico_multicore
    rp2040-psram
)

pico_enable_stdio_usb(picocalc_nes 1)

target_compile_definitions(picocalc_nes PRIVATE
    SHAPONES_ENABLE_LOG=1
    PSRAM_PIN_CS=20
    PSRAM_PIN_SCK=21
    PSRAM_PIN_MOSI=2
    PSRAM_PIN_MISO=3
    PSRAM_ASYNC     # required: the library's async inline helpers are unguarded
                    # and won't compile as C++ without pio_spi_write_async declared
    DISABLE_NUNCHUCK  # GP4/GP5 share traces with PSRAM on Picocalc PCB
)

pico_add_extra_outputs(picocalc_nes)

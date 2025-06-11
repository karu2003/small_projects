# == DO NOT EDIT THE FOLLOWING LINES for the Raspberry Pi Pico VS Code Extension to work ==
if(WIN32)
    set(USERHOME $ENV{USERPROFILE})
else()
    set(USERHOME $ENV{HOME})
endif()
set(sdkVersion 2.1.1)
set(toolchainVersion 14_2_Rel1)
set(picotoolVersion 2.1.1)
set(picoVscode ${USERHOME}/.pico-sdk/cmake/pico-vscode.cmake)
if (EXISTS ${picoVscode})
    include(${picoVscode})
endif()
# ====================================================================================
cmake_minimum_required(VERSION 3.13)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# cmake -DPICO_BOARD=pico -DFAMILY=rp2040 -DCMAKE_BUILD_TYPE=Debug ..

# ====================================================================================
set(PICO_BOARD
    pico
    CACHE STRING "Board type")

set(FAMILY
    rp2040
    CACHE STRING "MCU family")

if(DEFINED ENV{TinyUSB_PATH})
  set(TINYUSB_PATH $ENV{TinyUSB_PATH})
  message(STATUS "Using TinyUSB from: ${TINYUSB_PATH}")
else()
  message(FATAL_ERROR "The environment variable TinyUSB_PATH is not defined!")
endif()

if(NOT DEFINED ENV{PICO_SDK_PATH})
  set(PICO_SDK_PATH "/путь/к/вашему/pico-sdk")
  message(STATUS "Using custom Pico SDK path: ${PICO_SDK_PATH}")
else()
  set(PICO_SDK_PATH $ENV{PICO_SDK_PATH})
  message(STATUS "Using Pico SDK path from environment: ${PICO_SDK_PATH}")
endif()

if(NOT EXISTS ${PICO_SDK_PATH})
  message(FATAL_ERROR "PICO SDK не найден по пути: ${PICO_SDK_PATH}")
endif()

# Pull in Raspberry Pi Pico SDK (must be before project)
include(pico_sdk_import.cmake)

include($ENV{TinyUSB_PATH}/hw/bsp/family_support.cmake)

project(laser_sound C CXX ASM)

pico_sdk_init()

# Add executable. Default name is the project name, version 0.1
add_executable(laser_sound receiver.c transmitter.c usb_descriptors.c)

pico_generate_pio_header(laser_sound ${CMAKE_CURRENT_LIST_DIR}/ppm.pio)

target_compile_definitions(laser_sound PRIVATE PICO_BOARD="pico"
                                               FAMILY="rp2040")

pico_set_program_name(laser_sound "laser_sound")
pico_set_program_version(laser_sound "0.1")

# Modify the below lines to enable/disable output over UART/USB
pico_enable_stdio_uart(laser_sound 0)
pico_enable_stdio_usb(laser_sound 0)
target_link_libraries(
  laser_sound PUBLIC pico_stdlib hardware_pio hardware_clocks pico_multicore
                     tinyusb_device tinyusb_board)

target_include_directories(
  laser_sound PRIVATE ${CMAKE_CURRENT_LIST_DIR} ${TINYUSB_PATH}/src
                      ${TINYUSB_PATH}/hw ${TINYUSB_PATH}/hw/bsp)

pico_add_extra_outputs(laser_sound)

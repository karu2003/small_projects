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
# Generated Cmake Pico project file

cmake_minimum_required(VERSION 3.13)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# == DO NOT EDIT THE FOLLOWING LINES for the Raspberry Pi Pico VS Code Extension
# to work ==
if(WIN32)
  set(USERHOME $ENV{USERPROFILE})
else()
  set(USERHOME $ENV{HOME})
endif()
set(sdkVersion 2.1.1)
set(toolchainVersion 14_2_Rel1)
set(picotoolVersion 2.1.1)
set(picoVscode ${USERHOME}/.pico-sdk/cmake/pico-vscode.cmake)
if(EXISTS ${picoVscode})
  include(${picoVscode})
endif()
# ====================================================================================
set(PICO_BOARD
    pico
    CACHE STRING "Board type")

# Pull in SDK (must be before project)
include(pico_sdk_import.cmake)

project(laser_sound_card2 C CXX ASM)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

if (PICO_SDK_VERSION_STRING VERSION_LESS "1.3.0")
    message(FATAL_ERROR "Raspberry Pi Pico SDK version 1.3.0 (or later) required. Your version is ${PICO_SDK_VERSION_STRING}")
endif()

# Check for PICO_EXTRAS_PATH
if(NOT DEFINED ENV{PICO_EXTRAS_PATH})
    message(FATAL_ERROR "PICO_EXTRAS_PATH environment variable is not set. Please set it to the path of pico-extras repository (e.g., export PICO_EXTRAS_PATH=/home/andrew/pico/pico-extras)")
endif()

# Include pico_extras from PICO_EXTRAS_PATH
include($ENV{PICO_EXTRAS_PATH}/external/pico_extras_import.cmake)

# Initialize the SDK
pico_sdk_init()

add_executable(laser_sound_card2
    main.c
)

target_include_directories(laser_sound_card2 PRIVATE ${CMAKE_CURRENT_LIST_DIR})

target_link_libraries(laser_sound_card2 
    pico_stdlib
    pico_multicore
    hardware_pio
    hardware_timer
    usb_device
)

pico_generate_pio_header(laser_sound_card2 ${CMAKE_CURRENT_LIST_DIR}/ppm.pio)

pico_add_extra_outputs(laser_sound_card2)

target_compile_definitions(laser_sound_card2 PRIVATE
        AUDIO_FREQ_MAX=48000

        # ours are zero based, so say so
        PICO_USBDEV_USE_ZERO_BASED_INTERFACES=1

        # need large descriptor
        PICO_USBDEV_MAX_DESCRIPTOR_SIZE=256


        PICO_USBDEV_ISOCHRONOUS_BUFFER_STRIDE_TYPE=1
        PICO_USBDEV_ENABLE_DEBUG_TRAgCE
)

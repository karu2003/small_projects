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

project(dual_core_usb_audio C CXX ASM)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

if (PICO_SDK_VERSION_STRING VERSION_LESS "1.3.0")
    message(FATAL_ERROR "Raspberry Pi Pico SDK version 1.3.0 (or later) required. Your version is ${PICO_SDK_VERSION_STRING}")
endif()

# Initialize the SDK
pico_sdk_init()

add_executable(dual_core_usb_audio
    main.c
    usb_descriptors.c
)

# Make sure TinyUSB can find tusb_config.h
target_include_directories(dual_core_usb_audio PRIVATE ${CMAKE_CURRENT_LIST_DIR})

# Pull in our pico_stdlib which pulls in commonly used features
target_link_libraries(dual_core_usb_audio 
    pico_stdlib
    pico_multicore
    hardware_pio
    hardware_timer
    hardware_irq
    tinyusb_device
    tinyusb_board
    pico_unique_id
)

pico_generate_pio_header(dual_core_usb_audio ${CMAKE_CURRENT_LIST_DIR}/ppm.pio)

# Enable USB output, disable UART output
pico_enable_stdio_usb(dual_core_usb_audio 0)
pico_enable_stdio_uart(dual_core_usb_audio 1)

# Create map/bin/hex file etc.
pico_add_extra_outputs(dual_core_usb_audio)

# Compile flags
target_compile_definitions(dual_core_usb_audio PRIVATE
    CFG_TUSB_MCU=OPT_MCU_RP2040
    CFG_TUSB_OS=OPT_OS_PICO
    PICO_DEFAULT_UART_TX_PIN=0
    PICO_DEFAULT_UART_RX_PIN=1
)
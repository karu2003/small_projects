#pragma once

#include "hardware/clocks.h"
#include "hardware/pio.h"
#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "pico/time.h"
#include <cstdlib>
#include <cstdint>
#include <stdio.h>

// Include generated header files with PIO programs
#include "ppm.pio.h"

#define PULSE_GEN_PIN 0
#define PULSE_DET_PIN 1
#define LED_PIN 25

#define LED_TIME 500
#define SYS_FREQ 133000
#define MIN_TACKT 5

// Command codes for multicore FIFO
#define CMD_TEST_PULSE 1
#define CMD_STOP 2
#define CMD_READ_MEASUREMENT 3

// Structure for transferring commands between cores
typedef struct {
    uint32_t command;
    uint32_t pause_width;
    bool verbose;
} core_command_t;

// Structure for transferring results
typedef struct {
    uint32_t measured_width;
    bool success;
    uint32_t timestamp;  // Measurement timestamp
} core_result_t;

// Main function signatures
void first_core_main();  // Function for Core0 (receiver)
void second_core_main(); // Function for Core1 (transmitter + interface)
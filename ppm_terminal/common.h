#pragma once

#include "hardware/clocks.h"
#include "hardware/pio.h"
#include "pico/multicore.h"
#include "pico/stdlib.h"
#include "pico/time.h"
#include <cstdint>
#include <cstdlib>
#include <stdio.h>

#include "hardware/structs/timer.h"
#include "hardware/timer.h"

// Include generated header files with PIO programs
#include "ppm.pio.h"

#define PULSE_GEN_PIN 0
#define PULSE_DET_PIN 1
#define LED_PIN       25

#define LED_TIME  500
// #define SYS_FREQ  133000
#define SYS_FREQ 250000

#if SYS_FREQ == 133000
#define MIN_TACKT 5
#elif SYS_FREQ == 250000
#define MIN_TACKT 10
#endif

#define MAX_CODE  1024
#define MIN_PULSE_PERIOD 3.0f

static constexpr float    MIN_PULSE_PERIOD_US = MIN_PULSE_PERIOD/2;
static constexpr float    PIO_FREQ            = SYS_FREQ * 1000.0f;
static constexpr uint16_t MIN_INTERVAL_CYCLES =
    MIN_PULSE_PERIOD_US * (SYS_FREQ / 1000);
static constexpr float    AUDIO_SAMPLE_RATE = 48000.0f;
static constexpr uint32_t AUDIO_FRAME_TICKS =
    SYS_FREQ * 10.0 / AUDIO_SAMPLE_RATE;

// Main function signatures
void first_core_main();     // Function for Core0 (receiver)
void second_core_main();    // Function for Core1 (transmitter + interface)
#pragma once

#include "hardware/clocks.h"
#include "hardware/pio.h"
#include "pico/multicore.h"
#include "pico/stdlib.h"
#include "pico/time.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "hardware/structs/timer.h"
#include "hardware/timer.h"

// Include generated header files with PIO programs
#include "ppm.pio.h"

#define PULSE_GEN_PIN 0
#define PULSE_DET_PIN 1
#define LED_PIN       25

#define LED_TIME 500
#define SYS_FREQ 250000

#if SYS_FREQ == 133000
#define MIN_TACKT 5
#elif SYS_FREQ == 250000
#define MIN_TACKT 10
#endif

#define AUDIO_BUFFER_SIZE 192    // 4ms at 48kHz
static int16_t audio_buffer_rx[AUDIO_BUFFER_SIZE];
static int16_t audio_buffer_tx[AUDIO_BUFFER_SIZE];

#define MAX_CODE         1024
#define MIN_PULSE_PERIOD 3.0f

static const float    MIN_PULSE_PERIOD_US = MIN_PULSE_PERIOD / 2;
static const float    PIO_FREQ            = SYS_FREQ * 1000.0f;
static const uint16_t MIN_INTERVAL_CYCLES =
    MIN_PULSE_PERIOD_US * (SYS_FREQ / 1000);
static const float    AUDIO_SAMPLE_RATE = 48000.0f;
static const uint32_t AUDIO_FRAME_TICKS = SYS_FREQ * 10.0 / AUDIO_SAMPLE_RATE;

extern int16_t audio_in_buffer[];
extern int16_t audio_out_buffer[];
// extern volatile uint32_t last_ppm_received;

// Функции для преобразования аудио в PPM и обратно
uint32_t audio_to_ppm(int16_t audio_sample);
int16_t  ppm_to_audio(uint32_t ppm_value);

#ifdef __cplusplus
extern "C" {
#endif

// Main function signatures
// void first_core_main(void);     // Function for Core0 (receiver)
// void second_core_main(void);    // Function for Core1 (transmitter + interface)

#ifdef __cplusplus
}
#endif
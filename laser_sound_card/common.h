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
#include "tusb.h"

// Include generated header files with PIO programs
#include "ppm.pio.h"

#define PULSE_GEN_PIN 0
#define PULSE_DET_PIN 1
#define LED_PIN       25

// #define SYS_FREQ 133000
#define SYS_FREQ 250000

#if SYS_FREQ == 133000
#define MIN_TACKT 5
#elif SYS_FREQ == 250000
#define MIN_TACKT 8
#endif

#define MAX_CODE          1024
#define MIN_PULSE_PERIOD  3.0f
#define AUDIO_SAMPLE_RATE 48000

/* Blink pattern
 * - 25 ms   : streaming data
 * - 250 ms  : device not mounted
 * - 1000 ms : device mounted
 * - 2500 ms : device is suspended
 */
enum
{
    BLINK_STREAMING   = 25,
    BLINK_NOT_MOUNTED = 250,
    BLINK_MOUNTED     = 1000,
    BLINK_SUSPENDED   = 2500,
};

enum
{
    VOLUME_CTRL_0_DB    = 0,
    VOLUME_CTRL_10_DB   = 2560,
    VOLUME_CTRL_20_DB   = 5120,
    VOLUME_CTRL_30_DB   = 7680,
    VOLUME_CTRL_40_DB   = 10240,
    VOLUME_CTRL_50_DB   = 12800,
    VOLUME_CTRL_60_DB   = 15360,
    VOLUME_CTRL_70_DB   = 17920,
    VOLUME_CTRL_80_DB   = 20480,
    VOLUME_CTRL_90_DB   = 23040,
    VOLUME_CTRL_100_DB  = 25600,
    VOLUME_CTRL_SILENCE = 0x8000,
};

static const float MIN_PULSE_PERIOD_US = MIN_PULSE_PERIOD / 2;
static const float PIO_FREQ            = SYS_FREQ * 1000.0f;

static const uint16_t MIN_INTERVAL_CYCLES =
    MIN_PULSE_PERIOD_US * (SYS_FREQ / 1000);

// Main function signatures
void first_core_main(void);     // Function for Core0 (receiver)
void second_core_main(void);    // Function for Core1 (transmitter + interface)

// Statistics structure
typedef struct {
    uint32_t total_pcm_received;
    uint32_t total_ppm_convert;
    uint32_t total_pcm_convert;
    uint32_t total_ppm_sent;
    uint32_t total_ppm_received;
    uint32_t total_sent;
    uint32_t total_received;
    uint64_t total_summed_ppm_out;    // Sum of all PPM values at output
    uint64_t total_summed_ppm_in;     // Sum of all PPM values at input
    uint64_t total_summed_ppm_in_usb; // Sum of all PPM values after reception, before USB transmission
    uint64_t total_ticks_attempt_send_to_usb;
    uint64_t total_bytes_sent_to_usb;
} statistics_t;

// Structure for speaker double buffering
typedef struct {
    uint16_t          ppm_buffer[(CFG_TUD_AUDIO_FUNC_1_EP_OUT_SW_BUF_SZ / 4) / 2];    // Buffer for ready PPM values
    volatile uint16_t size;                                                           // Number of PPM values in buffer
    volatile uint16_t position;                                                       // Current position for reading
    volatile bool     ready;                                                          // Buffer ready for use
} spk_ppm_buffer_t;

// Structure for microphone double buffering
typedef struct {
    int32_t           pcm_buffer[CFG_TUD_AUDIO_FUNC_1_EP_IN_SW_BUF_SZ / 4];    // Buffer for PCM data
    volatile uint16_t size;                                                    // Data size in buffer
    volatile uint16_t position;                                                // Current position for writing (changed from int)
    volatile bool     ready;                                                   // Buffer ready for transmission
} mic_pcm_buffer_t;

// Structure for data exchange between cores
typedef struct {
    uint32_t          buffer[2][48];
    volatile uint16_t size[2];
    volatile uint16_t packet_size;
    volatile uint8_t  write_index;
    volatile uint8_t  read_index;
    semaphore_t       sem_empty;
    semaphore_t       sem_full;
} core_shared_buffer_t;

// Declaration of shared variables
extern core_shared_buffer_t shared_ppm_data;
extern volatile bool        sem_initialized;

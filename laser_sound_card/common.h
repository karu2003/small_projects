#pragma once

#include "hardware/clocks.h"
#include "hardware/pio.h"
#include "pico/multicore.h"
#include "pico/sem.h"
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

#define SYS_FREQ 250000

#if SYS_FREQ == 133000
#define MIN_TACKT 5
#elif SYS_FREQ == 250000
#define MIN_TACKT 10
#endif

#define MAX_CODE          1024
#define MIN_PULSE_PERIOD  3.0f
#define AUDIO_SAMPLE_RATE 48000

static const float    MIN_PULSE_PERIOD_US = MIN_PULSE_PERIOD / 2;
static const float    PIO_FREQ            = SYS_FREQ * 1000.0f;
static const uint16_t MIN_INTERVAL_CYCLES =
    MIN_PULSE_PERIOD_US * (SYS_FREQ / 1000);

// Main function signatures
void first_core_main(void);     // Function for Core0 (receiver)
void second_core_main(void);    // Function for Core1 (transmitter + interface)

// Структура для двойной буферизации динамика
typedef struct {
    uint32_t          ppm_buffer[CFG_TUD_AUDIO_FUNC_1_EP_OUT_SW_BUF_SZ / 4];    // Буфер готовых PPM значений
    volatile uint16_t size;                                                     // Количество PPM значений в буфере
    volatile uint16_t position;                                                 // Текущая позиция для чтения
    volatile bool     ready;                                                    // Буфер готов к использованию
} spk_ppm_buffer_t;

// Структура для двойной буферизации микрофона
typedef struct {
    int32_t           pcm_buffer[CFG_TUD_AUDIO_FUNC_1_EP_IN_SW_BUF_SZ / 4];    // Буфер для PCM данных
    volatile uint16_t size;                                                    // Размер данных в буфере
    volatile uint16_t position;                                                // Текущая позиция для записи (изменено с int)
    volatile bool     ready;                                                   // Буфер готов к отправке
} mic_pcm_buffer_t;

// Структура для обмена данными между ядрами
typedef struct {
    uint32_t          buffer[2][128];
    volatile uint16_t size[2];
    volatile uint16_t packet_size;
    volatile uint8_t  write_index;
    volatile uint8_t  read_index;
    semaphore_t       sem_empty;
    semaphore_t       sem_full;
} core_shared_buffer_t;

// Объявление общих переменных
extern core_shared_buffer_t shared_ppm_data;
extern volatile bool        sem_initialized;

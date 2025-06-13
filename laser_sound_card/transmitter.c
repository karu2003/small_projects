#include "common.h"
#include "hardware/uart.h"
#include "usb_descriptors.h"
#include <bsp/board_api.h>
#include <string.h>

// List of supported sample rates
const uint32_t sample_rates[] = {44100, AUDIO_SAMPLE_RATE};

uint32_t current_sample_rate = AUDIO_SAMPLE_RATE;
uint8_t  current_resolution;

#define UART_ID   uart0
#define BAUD_RATE 115200

#define UART_TX_PIN 16    // GPIO16 - UART0 TX
#define UART_RX_PIN 17    // GPIO17 - UART0 RX

#define BUFFER_SIZE (CFG_TUD_AUDIO_FUNC_1_EP_OUT_SW_BUF_SZ / 2)

// Структура для двойной буферизации динамика
typedef struct {
    uint32_t      ppm_buffer[BUFFER_SIZE / 4];    // Буфер готовых PPM значений
    volatile int  size;                           // Количество PPM значений в буфере
    volatile int  position;                       // Текущая позиция для чтения
    volatile bool ready;                          // Буфер готов к использованию
} spk_ppm_buffer_t;

// Структура для двойной буферизации микрофона
typedef struct {
    int32_t       pcm_buffer[BUFFER_SIZE / 4];    // Буфер для PCM данных
    volatile int  size;                           // Размер данных в буфере
    volatile int  position;                       // Текущая позиция для записи
    volatile bool ready;                          // Буфер готов к отправке
} mic_pcm_buffer_t;

// Двойные буферы для динамика (USB -> PPM)
static spk_ppm_buffer_t spk_buffers[2];
static volatile uint8_t current_spk_write_buffer = 0;    // Буфер для подготовки PPM
static volatile uint8_t current_spk_read_buffer  = 0;    // Буфер для чтения в timer

// Двойные буферы для микрофона (PPM -> USB)
static mic_pcm_buffer_t mic_buffers[2];
static volatile uint8_t current_mic_write_buffer = 0;    // Буфер для записи из PPM
static volatile uint8_t current_mic_read_buffer  = 0;    // Буфер для чтения в USB

// Рабочий буфер для приема USB данных
static int32_t usb_work_buffer[BUFFER_SIZE / 4];

// Статистика
static volatile uint32_t spk_packets_received  = 0;
static volatile uint32_t spk_samples_processed = 0;
static volatile uint32_t mic_samples_received  = 0;
static volatile uint32_t mic_packets_sent      = 0;
static volatile uint32_t timer_irq_count       = 0;

void setup_uart() {
    // Инициализация UART0
    uart_init(UART_ID, BAUD_RATE);

    // Установка функций на GPIO
    gpio_set_function(UART_TX_PIN, GPIO_FUNC_UART);
    gpio_set_function(UART_RX_PIN, GPIO_FUNC_UART);

    // Перенаправление стандартного вывода на UART
    stdio_uart_init();
}

#define N_SAMPLE_RATES TU_ARRAY_SIZE(sample_rates)

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

static uint32_t blink_interval_ms = BLINK_NOT_MOUNTED;

// Audio controls
// Current states
int8_t  mute[CFG_TUD_AUDIO_FUNC_1_N_CHANNELS_RX + 1];      // +1 for master channel 0
int16_t volume[CFG_TUD_AUDIO_FUNC_1_N_CHANNELS_RX + 1];    // +1 for master channel 0

// Buffer for microphone data
// int32_t mic_buf[CFG_TUD_AUDIO_FUNC_1_EP_IN_SW_BUF_SZ / 2];
// Buffer for speaker data
// int32_t spk_buf[CFG_TUD_AUDIO_FUNC_1_EP_OUT_SW_BUF_SZ / 2];
// Speaker data size received in the last frame
// volatile int spk_data_size;
// Resolution per format
// const uint8_t resolutions_per_format[CFG_TUD_AUDIO_FUNC_1_N_FORMATS] = {CFG_TUD_AUDIO_FUNC_1_FORMAT_1_RESOLUTION_RX,
//                                                                         CFG_TUD_AUDIO_FUNC_1_FORMAT_2_RESOLUTION_RX};

void led_blinking_task(void);
void audio_task(void);
void audio_control_task(void);

static PIO  pio = pio1;    // Use another PIO to avoid conflicts
static uint sm_gen;

volatile uint32_t ppm_code_to_send = 0;

uint32_t audio_frame_ticks;

void generate_pulse(uint32_t pause_width, bool verbose) {
    pio_sm_put_blocking(pio, sm_gen, pause_width);
}

uint32_t calculate_audio_frame_ticks() {
    // return clock_get_hz(clk_sys) / current_sample_rate;
    return 1000000 / current_sample_rate;
}

// Initialize PIO for pulse generator
void init_pulse_generator(float freq) {
    sm_gen               = pio_claim_unused_sm(pio, true);
    uint          offset = pio_add_program(pio, &pulse_generator_program);
    pio_sm_config c      = pulse_generator_program_get_default_config(offset);

    // Setup pins for PIO
    sm_config_set_set_pins(&c, PULSE_GEN_PIN, 1);
    pio_gpio_init(pio, PULSE_GEN_PIN);
    pio_sm_set_consecutive_pindirs(pio, sm_gen, PULSE_GEN_PIN, 1, true);

    sm_config_set_clkdiv(&c, (float)clock_get_hz(clk_sys) / freq);

    pio_sm_init(pio, sm_gen, offset, &c);
    pio_sm_set_enabled(pio, sm_gen, true);
}

// Преобразование 16-битного PCM в 10-битное значение для PPM
uint32_t audio_to_ppm(int16_t audio_sample) {
    if (audio_sample >= 0) {
        // [0, 32767] -> [511, 1023]
        return 511 + (uint32_t)((uint64_t)audio_sample * 512 / 32767);
    }
    else {
        // [-32768, -1] -> [0, 510]
        return 511 - (uint32_t)((uint64_t)(-audio_sample - 1) * 511 / 32767) - 1;
    }
}

// Преобразование 24-битного PCM в 10-битное значение для PPM
uint32_t audio24_to_ppm(int32_t audio_sample) {
    // Убеждаемся, что это валидное 24-битное значение
    audio_sample = (audio_sample << 8) >> 8;    // Расширение знака для 24-бит

    if (audio_sample >= 0) {
        // [0, 8388607] -> [511, 1023]
        return 511 + (uint32_t)((uint64_t)audio_sample * 512 / 8388607);
    }
    else {
        // [-8388608, -1] -> [0, 510]
        return 511 - (uint32_t)((uint64_t)(-audio_sample - 1) * 511 / 8388607) - 1;
    }
}

// Преобразование 10-битного PPM обратно в 16-битное PCM
int16_t ppm_to_audio(uint32_t ppm_value) {
    // Ограничиваем диапазон
    ppm_value &= 0x3FF;    // 0-1023

    // Масштабирование от 10-бит (0-1023) к 16-бит (0-65535)
    uint32_t unsigned_sample = (ppm_value * 65535 + 511) / 1023;

    // Сдвиг обратно к знаковому диапазону
    return (int16_t)((int32_t)unsigned_sample - 32768);
}

// Преобразование 10-битного PPM обратно в 24-битное PCM
int32_t ppm_to_audio24(uint32_t ppm_value) {
    // Ограничиваем диапазон
    ppm_value &= 0x3FF;    // 0-1023

    // Масштабирование от 10-бит (0-1023) к 24-бит (0-16777215)
    uint32_t unsigned_sample = (ppm_value * 16777215 + 511) / 1023;

    // Сдвиг обратно к знаковому диапазону
    int32_t signed_sample = (int32_t)((int64_t)unsigned_sample - 8388608);

    // Убеждаемся, что результат помещается в 24 бита
    return (signed_sample << 8) >> 8;
}

void led_blinking_task(void) {
    static uint32_t start_ms  = 0;
    static bool     led_state = false;

    // Blink every interval ms
    if (board_millis() - start_ms < blink_interval_ms)
        return;
    start_ms += blink_interval_ms;

    board_led_write(led_state);
    led_state = 1 - led_state;
}

// Инициализация двойной буферизации
void init_double_buffering(void) {
    // Инициализация буферов динамика
    for (int i = 0; i < 2; i++) {
        spk_buffers[i].size     = 0;
        spk_buffers[i].position = 0;
        spk_buffers[i].ready    = false;
    }
    current_spk_write_buffer = 0;
    current_spk_read_buffer  = 0;

    // Инициализация буферов микрофона
    for (int i = 0; i < 2; i++) {
        mic_buffers[i].size     = 0;
        mic_buffers[i].position = 0;
        mic_buffers[i].ready    = false;
    }
    current_mic_write_buffer = 0;
    current_mic_read_buffer  = 0;

    printf("Double buffering initialized (no FIFO)\r\n");
}

// Таск обработки динамика (USB -> PPM)
void spk_task(void) {
    // 1. Прием данных из USB и подготовка PPM
    spk_ppm_buffer_t *write_buf = &spk_buffers[current_spk_write_buffer];

    if (!write_buf->ready) {
        // Буфер PPM свободен, можем готовить новые данные
        uint16_t packet_size = (uint16_t)(current_sample_rate / 1000 *
                                          CFG_TUD_AUDIO_FUNC_1_N_BYTES_PER_SAMPLE_RX *
                                          CFG_TUD_AUDIO_FUNC_1_N_CHANNELS_RX);

        int bytes_read = tud_audio_read(usb_work_buffer, packet_size);

        if (bytes_read > 0) {
            // Преобразуем PCM в PPM значения
            int ppm_count = 0;

            if (current_resolution == 16) {
                int16_t *src   = (int16_t *)usb_work_buffer;
                int16_t *limit = (int16_t *)usb_work_buffer + bytes_read / 2;

                while (src + 1 < limit && ppm_count < (BUFFER_SIZE / 4)) {
                    int32_t left  = *src++;
                    int32_t right = *src++;
                    int16_t mono  = (int16_t)((left >> 1) + (right >> 1));

                    write_buf->ppm_buffer[ppm_count++] = audio_to_ppm(mono);
                }
            }
            else if (current_resolution == 24) {
                int32_t *src   = (int32_t *)usb_work_buffer;
                int32_t *limit = (int32_t *)usb_work_buffer + bytes_read / 4;

                while (src + 1 < limit && ppm_count < (BUFFER_SIZE / 4)) {
                    int32_t left  = *src++;
                    int32_t right = *src++;
                    int32_t mono  = (int32_t)((left >> 1) + (right >> 1));

                    write_buf->ppm_buffer[ppm_count++] = audio24_to_ppm(mono);
                }
            }

            if (ppm_count > 0) {
                write_buf->size  = ppm_count;
                write_buf->ready = true;

                // Переключаемся на следующий буфер для записи
                current_spk_write_buffer = (current_spk_write_buffer + 1) % 2;
                spk_packets_received++;
                spk_samples_processed += ppm_count;

                // Отладка каждые 1000 пакетов
                if (spk_packets_received % 1000 == 0) {
                    printf("SPK: Packet #%lu, PPM samples=%d\r\n",
                           spk_packets_received,
                           ppm_count);
                }
            }
        }
    }
}

// for (uint8_t cnt = 0; cnt < 2; cnt++) {
//     tud_audio_write_support_ff(cnt, i2s_dummy_buffer[cnt], AUDIO_SAMPLE_RATE / 1000 * CFG_TUD_AUDIO_FUNC_1_N_BYTES_PER_SAMPLE_TX * CFG_TUD_AUDIO_FUNC_1_CHANNEL_PER_FIFO_TX);
// }
// tud_audio_write(i2s_dummy_buffer, AUDIO_SAMPLE_RATE/1000 * CFG_TUD_AUDIO_FUNC_1_N_BYTES_PER_SAMPLE_TX * CFG_TUD_AUDIO_FUNC_1_N_CHANNELS_TX);

// Таск обработки микрофона (PPM -> USB)
void mic_task(void) {
    // 1. Прием данных из межъядерного FIFO
    if (multicore_fifo_rvalid()) {
        uint32_t          ppm_value = multicore_fifo_pop_blocking();
        mic_pcm_buffer_t *write_buf = &mic_buffers[current_mic_write_buffer];

        if (current_resolution == 16) {
            int16_t  audio_sample = ppm_to_audio(ppm_value);
            int16_t *dst          = (int16_t *)((uint8_t *)write_buf->pcm_buffer + write_buf->position);

            // Стерео: дублируем моно сигнал
            *dst++ = audio_sample;       // Левый канал
            *dst++ = audio_sample;       // Правый канал
            write_buf->position += 4;    // 2 сэмпла по 2 байта
            mic_samples_received++;

            // Если буфер заполнен, помечаем его готовым
            if (write_buf->position >= 96) {    // ~24 стерео сэмпла
                write_buf->size     = write_buf->position;
                write_buf->ready    = true;
                write_buf->position = 0;

                // Переключаемся на следующий буфер
                current_mic_write_buffer = (current_mic_write_buffer + 1) % 2;
            }
        }
        else if (current_resolution == 24) {
            int32_t  audio_sample = ppm_to_audio24(ppm_value);
            int32_t *dst          = (int32_t *)((uint8_t *)write_buf->pcm_buffer + write_buf->position);

            // Стерео: дублируем моно сигнал
            *dst++ = audio_sample;       // Левый канал
            *dst++ = audio_sample;       // Правый канал
            write_buf->position += 8;    // 2 сэмпла по 4 байта
            mic_samples_received++;

            // Если буфер заполнен, помечаем его готовым
            if (write_buf->position >= 192) {    // ~24 стерео сэмпла
                write_buf->size     = write_buf->position;
                write_buf->ready    = true;
                write_buf->position = 0;

                // Переключаемся на следующий буфер
                current_mic_write_buffer = (current_mic_write_buffer + 1) % 2;
            }
        }
    }

    // 2. Отправка готовых буферов в USB
    mic_pcm_buffer_t *read_buf = &mic_buffers[current_mic_read_buffer];

    if (read_buf->ready) {
        // Отправляем данные в USB
        if (tud_audio_write((uint8_t *)read_buf->pcm_buffer, read_buf->size)) {
            read_buf->ready    = false;
            read_buf->size     = 0;
            read_buf->position = 0;
            mic_packets_sent++;

            // Переключаемся на следующий буфер для чтения
            current_mic_read_buffer = (current_mic_read_buffer + 1) % 2;

            // Отладка каждые 500 пакетов
            if (mic_packets_sent % 500 == 0) {
                printf("MIC: Sent packet #%lu\r\n", mic_packets_sent);
            }
        }
    }
}

// Минимальный timer0_irq_handler - только выборка и отправка
void timer0_irq_handler() {
    if (timer_hw->intr & (1u << 0)) {
        timer_hw->intr = 1u << 0;
        timer_irq_count++;

        uint32_t ppm_value = MIN_INTERVAL_CYCLES;    // По умолчанию тишина

        // Быстрое чтение из текущего буфера
        spk_ppm_buffer_t *read_buf = &spk_buffers[current_spk_read_buffer];

        if (read_buf->ready && read_buf->position < read_buf->size) {
            // Читаем готовое PPM значение
            ppm_value = MIN_INTERVAL_CYCLES + read_buf->ppm_buffer[read_buf->position++];

            // Если буфер полностью прочитан, освобождаем его
            if (read_buf->position >= read_buf->size) {
                read_buf->ready    = false;
                read_buf->size     = 0;
                read_buf->position = 0;

                // Переключаемся на следующий буфер для чтения
                current_spk_read_buffer = (current_spk_read_buffer + 1) % 2;
            }
        }

        // Отправляем PPM импульс
        generate_pulse(ppm_value, false);

        // Устанавливаем следующее прерывание
        timer_hw->alarm[0] = timer_hw->timerawl + audio_frame_ticks;
    }
}

// Функция вывода статистики
void print_audio_stats(void) {
    static uint32_t last_stats_time = 0;

    if (board_millis() - last_stats_time > 2000) {    // Каждые 2 секунды
        last_stats_time = board_millis();

        // Статистика буферов динамика
        int spk_buf0_fill = spk_buffers[0].ready ? (spk_buffers[0].size - spk_buffers[0].position) : 0;
        int spk_buf1_fill = spk_buffers[1].ready ? (spk_buffers[1].size - spk_buffers[1].position) : 0;

        // Статистика буферов микрофона
        int mic_buf0_fill = mic_buffers[0].ready ? mic_buffers[0].size : mic_buffers[0].position;
        int mic_buf1_fill = mic_buffers[1].ready ? mic_buffers[1].size : mic_buffers[1].position;

        printf("STATS: SPK_RX=%lu SPK_PROC=%lu MIC_RX=%lu MIC_TX=%lu TIMER=%lu\r\n",
               spk_packets_received,
               spk_samples_processed,
               mic_samples_received,
               mic_packets_sent,
               timer_irq_count);

        printf("BUFFERS: SPK[%d,%d] MIC[%d,%d] WR/RD_IDX=%u/%u %u/%u\r\n",
               spk_buf0_fill,
               spk_buf1_fill,
               mic_buf0_fill,
               mic_buf1_fill,
               current_spk_write_buffer,
               current_spk_read_buffer,
               current_mic_write_buffer,
               current_mic_read_buffer);

        // Сброс счетчиков
        spk_packets_received  = 0;
        spk_samples_processed = 0;
        mic_samples_received  = 0;
        mic_packets_sent      = 0;
        timer_irq_count       = 0;
    }
}

// Замена старой audio_task
void audio_task(void) {
    // 1. Обработка динамика (USB -> PPM)
    spk_task();

    // 2. Обработка микрофона (PPM -> USB)
    mic_task();

    // 3. Вывод статистики
    print_audio_stats();
}

// Обновить second_core_main()
void second_core_main() {
    board_init();
    tusb_init();
    setup_uart();

    tusb_rhport_init_t dev_init = {
        .role  = TUSB_ROLE_DEVICE,
        .speed = TUSB_SPEED_AUTO};
    tusb_init(BOARD_TUD_RHPORT, &dev_init);

    if (board_init_after_tusb) {
        board_init_after_tusb();
    }

    TU_LOG1("Laser Audio running\r\n");
    stdio_init_all();

    // Инициализация двойной буферизации
    init_double_buffering();

    init_pulse_generator(PIO_FREQ);

    audio_frame_ticks = 1000000 / AUDIO_SAMPLE_RATE;

    // Setup timer interrupt for audio sampling
    irq_set_exclusive_handler(TIMER_IRQ_0, timer0_irq_handler);
    hw_set_bits(&timer_hw->inte, (1u << 0));
    irq_set_enabled(TIMER_IRQ_0, true);

    timer_hw->alarm[0] = timer_hw->timerawl + audio_frame_ticks;

    printf("Audio system initialized with double buffering (no FIFO)\r\n");

    // Main operation loop on Core1
    while (1) {
        tud_task();
        audio_task();    // Включает spk_task() и mic_task()
        led_blinking_task();
    }
}

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

#define BUFFER_SIZE (CFG_TUD_AUDIO_FUNC_1_EP_OUT_SW_BUF_SZ)

// Структура для двойной буферизации динамика
typedef struct {
    uint32_t      ppm_buffer[BUFFER_SIZE];    // Буфер готовых PPM значений
    volatile int  size;                       // Количество PPM значений в буфере
    volatile int  position;                   // Текущая позиция для чтения
    volatile bool ready;                      // Буфер готов к использованию
} spk_ppm_buffer_t;

// Структура для двойной буферизации микрофона
typedef struct {
    int32_t       pcm_buffer[BUFFER_SIZE];    // Буфер для PCM данных
    volatile int  size;                       // Размер данных в буфере
    volatile int  position;                   // Текущая позиция для записи
    volatile bool ready;                      // Буфер готов к отправке
} mic_pcm_buffer_t;

// Двойные буферы для динамика (USB -> PPM)
static spk_ppm_buffer_t spk_buffers[2];
static volatile uint8_t current_spk_write_buffer = 0;    // Буфер для подготовки PPM
static volatile uint8_t current_spk_read_buffer  = 0;    // Буфер для чтения в timer

// Двойные буферы для микрофона (PPM -> USB)
static mic_pcm_buffer_t mic_buffers[2];
static volatile uint8_t current_mic_write_buffer = 0;    // Буфер для записи из PPM
static volatile uint8_t current_mic_read_buffer  = 0;    // Буфер для чтения в USB

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
const uint8_t resolutions_per_format[CFG_TUD_AUDIO_FUNC_1_N_FORMATS] = {CFG_TUD_AUDIO_FUNC_1_FORMAT_1_RESOLUTION_RX,
                                                                        CFG_TUD_AUDIO_FUNC_1_FORMAT_2_RESOLUTION_RX};

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

uint32_t audio_to_ppm(int16_t audio_sample) {
    if (audio_sample >= 0) {
        return 511 + (uint32_t)((uint64_t)audio_sample * 512 / 32767);
    }
    else {
        return 511 - (uint32_t)((uint64_t)(-audio_sample - 1) * 511 / 32767) - 1;
    }
}

int16_t ppm_to_audio(uint32_t ppm_value) {
    ppm_value &= 0x3FF;
    if (ppm_value >= 511) {
        return (int16_t)(((ppm_value - 511) * 32767) / 512);
    }
    else {
        return (int16_t)(-32768 + ((ppm_value * 32767) / 510));
    }
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

void init_double_buffering(void) {

    for (int i = 0; i < 2; i++) {
        spk_buffers[i].size     = 0;
        spk_buffers[i].position = 0;
        spk_buffers[i].ready    = false;
    }
    current_spk_write_buffer = 0;
    current_spk_read_buffer  = 0;

    for (int i = 0; i < 2; i++) {
        mic_buffers[i].size     = 0;
        mic_buffers[i].position = 0;
        mic_buffers[i].ready    = false;
    }
    current_mic_write_buffer = 0;
    current_mic_read_buffer  = 0;

    printf("Double buffering initialized (no FIFO)\r\n");
}

// bool tud_audio_rx_done_pre_read_cb(uint8_t rhport, uint16_t n_bytes_received, uint8_t func_id, uint8_t ep_out, uint8_t cur_alt_setting)
// {
//   (void)rhport;
//   (void)func_id;
//   (void)ep_out;
//   (void)cur_alt_setting;

//   spk_data_size = tud_audio_read(spk_buf, n_bytes_received);
//   return true;
// }

bool tud_audio_rx_done_post_read_cb(uint8_t rhport, uint16_t n_bytes_received, uint8_t func_id, uint8_t ep_out, uint8_t cur_alt_setting) {
    (void)rhport;
    (void)func_id;
    (void)ep_out;
    (void)cur_alt_setting;

    // Данные уже прочитаны TinyUSB автоматически!
    // Просто устанавливаем флаг, что пришли новые данные
    spk_packets_received++;

    // Для отладки
    if (spk_packets_received % 1000 == 0) {
        printf("USB: Получены данные, %d байт\r\n", n_bytes_received);
    }

    return true;
}

// bool tud_audio_rx_done_post_read_cb(uint8_t rhport, uint16_t n_bytes_received, uint8_t func_id, uint8_t ep_out, uint8_t cur_alt_setting)
// {
//   (void)rhport;
//   (void)n_bytes_received;
//   (void)func_id;
//   (void)ep_out;
//   (void)cur_alt_setting;

//   fifo_count = tud_audio_available();
//   // Same averaging method used in UAC2 class
//   fifo_count_avg = (uint32_t)(((uint64_t)fifo_count_avg * 63  + ((uint32_t)fifo_count << 16)) >> 6);

//   return true;
// }

// Таск обработки динамика (USB -> PPM)
void spk_task(void) {
    // 1. Прием данных из USB и подготовка PPM
    spk_ppm_buffer_t *write_buf = &spk_buffers[current_spk_write_buffer];

    if (!write_buf->ready) {
        // Буфер PPM свободен, можем готовить новые данные
        uint16_t packet_size = (uint16_t)(current_sample_rate / 1000 *
                                          CFG_TUD_AUDIO_FUNC_1_N_BYTES_PER_SAMPLE_RX *
                                          CFG_TUD_AUDIO_FUNC_1_N_CHANNELS_RX);

        // Читаем данные напрямую в буфер динамика, используя его как временное хранилище PCM
        int16_t *pcm_data   = (int16_t *)write_buf->ppm_buffer;    // Временно используем для PCM
        int      bytes_read = tud_audio_read((uint8_t *)pcm_data, packet_size);

        if (bytes_read > 0) {
            // Преобразуем PCM в PPM значения на месте
            int ppm_count   = 0;
            int pcm_samples = bytes_read / 2;    // Количество 16-битных семплов

            // Преобразуем стерео в моно и затем в PPM
            for (int i = 0; i < pcm_samples - 1; i += 2) {
                int32_t left  = pcm_data[i];
                int32_t right = pcm_data[i + 1];
                int16_t mono  = (int16_t)((left >> 1) + (right >> 1));

                // Сохраняем PPM код в том же буфере, смещая к началу
                write_buf->ppm_buffer[ppm_count++] = audio_to_ppm(mono);

                // Предотвращаем выход за границы буфера
                if (ppm_count >= BUFFER_SIZE / 4)
                    break;
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
                    printf("SPK: Packet #%lu, PCM=%d bytes, PPM=%d samples\r\n",
                           spk_packets_received,
                           bytes_read,
                           ppm_count);
                }
            }
        }
    }
}

// Упрощаем mic_task, убирая ветку для 24-бит
void mic_task(void) {
    // 1. Прием данных из межъядерного FIFO
    if (multicore_fifo_rvalid()) {
        uint32_t          ppm_value = multicore_fifo_pop_blocking();
        mic_pcm_buffer_t *write_buf = &mic_buffers[current_mic_write_buffer];

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

    // На эти (правильные названия и формат):
    printf("Test: PCM max (32767) -> PPM=%u\n", audio_to_ppm(32767));
    printf("Test: PCM half max (16384) -> PPM=%u\n", audio_to_ppm(16384));
    printf("Test: PCM zero (0) -> PPM=%u\n", audio_to_ppm(0));
    printf("Test: PCM half min (-16384) -> PPM=%u\n", audio_to_ppm(-16384));
    printf("Test: PCM min (-32767) -> PPM=%u\n", audio_to_ppm(-32767));

    printf("Test: PPM min (0) -> PCM=%d\n", ppm_to_audio(0));
    printf("Test: PPM mid (511) -> PCM=%d\n", ppm_to_audio(511));
    printf("Test: PPM max (1023) -> PCM=%d\n", ppm_to_audio(1023));

    // Устанавливаем разрешение принудительно на 16 бит
    current_resolution = 16;

    // Main operation loop on Core1
    while (1) {
        tud_task();
        audio_task();    // Включает spk_task() и mic_task()
        led_blinking_task();
    }
}

//--------------------------------------------------------------------+
// Device callbacks
//--------------------------------------------------------------------+

// Invoked when device is mounted
void tud_mount_cb(void) {
    blink_interval_ms = BLINK_MOUNTED;
}

// Invoked when device is unmounted
void tud_umount_cb(void) {
    blink_interval_ms = BLINK_NOT_MOUNTED;
}

// Invoked when usb bus is suspended
// remote_wakeup_en : if host allow us  to perform remote wakeup
// Within 7ms, device must draw an average of current less than 2.5 mA from bus
void tud_suspend_cb(bool remote_wakeup_en) {
    (void)remote_wakeup_en;
    blink_interval_ms = BLINK_SUSPENDED;
}

// Invoked when usb bus is resumed
void tud_resume_cb(void) {
    blink_interval_ms = tud_mounted() ? BLINK_MOUNTED : BLINK_NOT_MOUNTED;
}

// Helper for clock get requests
static bool tud_audio_clock_get_request(uint8_t rhport, audio_control_request_t const *request) {
    TU_ASSERT(request->bEntityID == UAC2_ENTITY_CLOCK);

    if (request->bControlSelector == AUDIO_CS_CTRL_SAM_FREQ) {
        if (request->bRequest == AUDIO_CS_REQ_CUR) {
            TU_LOG1("Clock get current freq %" PRIu32 "\r\n", current_sample_rate);

            audio_control_cur_4_t curf = {(int32_t)tu_htole32(current_sample_rate)};
            return tud_audio_buffer_and_schedule_control_xfer(rhport, (tusb_control_request_t const *)request, &curf, sizeof(curf));
        }
        else if (request->bRequest == AUDIO_CS_REQ_RANGE) {
            audio_control_range_4_n_t(N_SAMPLE_RATES) rangef =
                {
                    .wNumSubRanges = tu_htole16(N_SAMPLE_RATES)};
            TU_LOG1("Clock get %d freq ranges\r\n", N_SAMPLE_RATES);
            for (uint8_t i = 0; i < N_SAMPLE_RATES; i++) {
                rangef.subrange[i].bMin = (int32_t)sample_rates[i];
                rangef.subrange[i].bMax = (int32_t)sample_rates[i];
                rangef.subrange[i].bRes = 0;
                TU_LOG1("Range %d (%d, %d, %d)\r\n", i, (int)rangef.subrange[i].bMin, (int)rangef.subrange[i].bMax, (int)rangef.subrange[i].bRes);
            }

            return tud_audio_buffer_and_schedule_control_xfer(rhport, (tusb_control_request_t const *)request, &rangef, sizeof(rangef));
        }
    }
    else if (request->bControlSelector == AUDIO_CS_CTRL_CLK_VALID &&
             request->bRequest == AUDIO_CS_REQ_CUR) {
        audio_control_cur_1_t cur_valid = {.bCur = 1};
        TU_LOG1("Clock get is valid %u\r\n", cur_valid.bCur);
        return tud_audio_buffer_and_schedule_control_xfer(rhport, (tusb_control_request_t const *)request, &cur_valid, sizeof(cur_valid));
    }
    TU_LOG1("Clock get request not supported, entity = %u, selector = %u, request = %u\r\n",
            request->bEntityID,
            request->bControlSelector,
            request->bRequest);
    return false;
}

// Helper for clock set requests
static bool tud_audio_clock_set_request(uint8_t rhport, audio_control_request_t const *request, uint8_t const *buf) {
    (void)rhport;

    TU_ASSERT(request->bEntityID == UAC2_ENTITY_CLOCK);
    TU_VERIFY(request->bRequest == AUDIO_CS_REQ_CUR);

    if (request->bControlSelector == AUDIO_CS_CTRL_SAM_FREQ) {
        TU_VERIFY(request->wLength == sizeof(audio_control_cur_4_t));

        current_sample_rate = (uint32_t)((audio_control_cur_4_t const *)buf)->bCur;

        TU_LOG1("Clock set current freq: %" PRIu32 "\r\n", current_sample_rate);

        return true;
    }
    else {
        TU_LOG1("Clock set request not supported, entity = %u, selector = %u, request = %u\r\n",
                request->bEntityID,
                request->bControlSelector,
                request->bRequest);
        return false;
    }
}

// Helper for feature unit get requests
static bool tud_audio_feature_unit_get_request(uint8_t rhport, audio_control_request_t const *request) {
    TU_ASSERT(request->bEntityID == UAC2_ENTITY_SPK_FEATURE_UNIT);

    if (request->bControlSelector == AUDIO_FU_CTRL_MUTE && request->bRequest == AUDIO_CS_REQ_CUR) {
        audio_control_cur_1_t mute1 = {.bCur = mute[request->bChannelNumber]};
        TU_LOG1("Get channel %u mute %d\r\n", request->bChannelNumber, mute1.bCur);
        return tud_audio_buffer_and_schedule_control_xfer(rhport, (tusb_control_request_t const *)request, &mute1, sizeof(mute1));
    }
    else if (request->bControlSelector == AUDIO_FU_CTRL_VOLUME) {
        if (request->bRequest == AUDIO_CS_REQ_RANGE) {
            audio_control_range_2_n_t(1) range_vol = {
                .wNumSubRanges = tu_htole16(1),
                .subrange[0]   = {.bMin = tu_htole16(-VOLUME_CTRL_50_DB), tu_htole16(VOLUME_CTRL_0_DB), tu_htole16(256)}};
            TU_LOG1("Get channel %u volume range (%d, %d, %u) dB\r\n", request->bChannelNumber, range_vol.subrange[0].bMin / 256, range_vol.subrange[0].bMax / 256, range_vol.subrange[0].bRes / 256);
            return tud_audio_buffer_and_schedule_control_xfer(rhport, (tusb_control_request_t const *)request, &range_vol, sizeof(range_vol));
        }
        else if (request->bRequest == AUDIO_CS_REQ_CUR) {
            audio_control_cur_2_t cur_vol = {.bCur = tu_htole16(volume[request->bChannelNumber])};
            TU_LOG1("Get channel %u volume %d dB\r\n", request->bChannelNumber, cur_vol.bCur / 256);
            return tud_audio_buffer_and_schedule_control_xfer(rhport, (tusb_control_request_t const *)request, &cur_vol, sizeof(cur_vol));
        }
    }
    TU_LOG1("Feature unit get request not supported, entity = %u, selector = %u, request = %u\r\n",
            request->bEntityID,
            request->bControlSelector,
            request->bRequest);

    return false;
}

// Helper for feature unit set requests
static bool tud_audio_feature_unit_set_request(uint8_t rhport, audio_control_request_t const *request, uint8_t const *buf) {
    (void)rhport;

    TU_ASSERT(request->bEntityID == UAC2_ENTITY_SPK_FEATURE_UNIT);
    TU_VERIFY(request->bRequest == AUDIO_CS_REQ_CUR);

    if (request->bControlSelector == AUDIO_FU_CTRL_MUTE) {
        TU_VERIFY(request->wLength == sizeof(audio_control_cur_1_t));

        mute[request->bChannelNumber] = ((audio_control_cur_1_t const *)buf)->bCur;

        TU_LOG1("Set channel %d Mute: %d\r\n", request->bChannelNumber, mute[request->bChannelNumber]);

        return true;
    }
    else if (request->bControlSelector == AUDIO_FU_CTRL_VOLUME) {
        TU_VERIFY(request->wLength == sizeof(audio_control_cur_2_t));

        volume[request->bChannelNumber] = ((audio_control_cur_2_t const *)buf)->bCur;

        TU_LOG1("Set channel %d volume: %d dB\r\n", request->bChannelNumber, volume[request->bChannelNumber] / 256);

        return true;
    }
    else {
        TU_LOG1("Feature unit set request not supported, entity = %u, selector = %u, request = %u\r\n",
                request->bEntityID,
                request->bControlSelector,
                request->bRequest);
        return false;
    }
}

//--------------------------------------------------------------------+
// Application Callback API Implementations
//--------------------------------------------------------------------+

// Invoked when audio class specific get request received for an entity
bool tud_audio_get_req_entity_cb(uint8_t rhport, tusb_control_request_t const *p_request) {
    audio_control_request_t const *request = (audio_control_request_t const *)p_request;

    if (request->bEntityID == UAC2_ENTITY_CLOCK)
        return tud_audio_clock_get_request(rhport, request);
    if (request->bEntityID == UAC2_ENTITY_SPK_FEATURE_UNIT)
        return tud_audio_feature_unit_get_request(rhport, request);
    else {
        TU_LOG1("Get request not handled, entity = %d, selector = %d, request = %d\r\n",
                request->bEntityID,
                request->bControlSelector,
                request->bRequest);
    }
    return false;
}

// Invoked when audio class specific set request received for an entity
bool tud_audio_set_req_entity_cb(uint8_t rhport, tusb_control_request_t const *p_request, uint8_t *buf) {
    audio_control_request_t const *request = (audio_control_request_t const *)p_request;

    if (request->bEntityID == UAC2_ENTITY_SPK_FEATURE_UNIT)
        return tud_audio_feature_unit_set_request(rhport, request, buf);
    if (request->bEntityID == UAC2_ENTITY_CLOCK)
        return tud_audio_clock_set_request(rhport, request, buf);
    TU_LOG1("Set request not handled, entity = %d, selector = %d, request = %d\r\n",
            request->bEntityID,
            request->bControlSelector,
            request->bRequest);

    return false;
}

bool tud_audio_set_itf_close_EP_cb(uint8_t rhport, tusb_control_request_t const *p_request) {
    (void)rhport;

    uint8_t const itf = tu_u16_low(tu_le16toh(p_request->wIndex));
    uint8_t const alt = tu_u16_low(tu_le16toh(p_request->wValue));

    if (ITF_NUM_AUDIO_STREAMING_SPK == itf && alt == 0)
        blink_interval_ms = BLINK_MOUNTED;

    return true;
}

bool tud_audio_set_itf_cb(uint8_t rhport, tusb_control_request_t const *p_request) {
    (void)rhport;
    uint8_t const itf = tu_u16_low(tu_le16toh(p_request->wIndex));
    uint8_t const alt = tu_u16_low(tu_le16toh(p_request->wValue));

    printf("USB: Установка интерфейса %d, alt %d\r\n", itf, alt);

    // Обновляем индикацию режима работы
    if (ITF_NUM_AUDIO_STREAMING_SPK == itf) {
        if (alt != 0) {
            blink_interval_ms = BLINK_STREAMING;    // Потоковый режим
            printf("USB: Включен потоковый режим аудио!\r\n");
        }
        else {
            blink_interval_ms = BLINK_MOUNTED;    // Нет потока данных
            printf("USB: Отключен потоковый режим аудио\r\n");
        }
    }

    // Сбрасываем состояние всех буферов динамика
    for (int i = 0; i < 2; i++) {
        spk_buffers[i].size     = 0;
        spk_buffers[i].position = 0;
        spk_buffers[i].ready    = false;
    }
    current_spk_write_buffer = 0;
    current_spk_read_buffer  = 0;

    // Устанавливаем разрешение и частоту дискретизации
    if (alt != 0) {
        current_resolution = resolutions_per_format[alt - 1];
        audio_frame_ticks  = calculate_audio_frame_ticks();

        printf("USB: Установлено разрешение %d бит, audio_frame_ticks=%lu\r\n",
               current_resolution,
               audio_frame_ticks);

        // Перенастраиваем таймер на новую частоту
        timer_hw->alarm[0] = timer_hw->timerawl + audio_frame_ticks;
    }

    return true;
}

// bool tud_audio_rx_done_pre_read_cb(uint8_t rhport, uint16_t n_bytes_received, uint8_t func_id, uint8_t ep_out, uint8_t cur_alt_setting) {
//     (void)rhport;
//     (void)func_id;
//     (void)ep_out;
//     (void)cur_alt_setting;

//     if (spk_buffer_busy) {
//         return false;
//     }

//     spk_data_size = tud_audio_read(spk_buf, n_bytes_received);
//     if (spk_data_size > 0) {
//         spk_buffer_busy = true;
//     }

//     return true;
// }

// bool tud_audio_rx_done_pre_read_cb(uint8_t rhport, uint16_t n_bytes_received, uint8_t func_id, uint8_t ep_out, uint8_t cur_alt_setting) {
//     (void)rhport;
//     (void)func_id;
//     (void)cur_alt_setting;

//     // Если буфер занят, отказываемся принимать новые данные
//     if (spk_buffer_busy) {
//         // Сообщаем об этом для отладки
//         static uint32_t last_busy_log = 0;
//         if (board_millis() - last_busy_log > 50) {
//             last_busy_log = board_millis();
//             printf("USB: Device busy, NAK sent\r\n");
//         }

//         // Просим TinyUSB отправить NAK пакет
//         // tud_edpt_stall(ep_out);
//         return false;
//     }

//     spk_data_size = tud_audio_read(spk_buf, n_bytes_received);
//     if (spk_data_size > 0) {
//         spk_buffer_busy = true;
//     }

//     return true;
// }

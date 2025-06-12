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

#define PPM_FIFO_SIZE 256
volatile uint32_t ppm_fifo[PPM_FIFO_SIZE];
volatile uint8_t  ppm_fifo_head = 0;
volatile uint8_t  ppm_fifo_tail = 0;

volatile uint32_t fifo_push_count  = 0;
volatile uint32_t fifo_pop_count   = 0;
volatile uint32_t fifo_full_count  = 0;
volatile uint32_t fifo_empty_count = 0;

volatile bool spk_buffer_busy = false;

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
int32_t mic_buf[CFG_TUD_AUDIO_FUNC_1_EP_IN_SW_BUF_SZ / 4];
// Buffer for speaker data
int32_t spk_buf[CFG_TUD_AUDIO_FUNC_1_EP_OUT_SW_BUF_SZ / 4];
// Speaker data size received in the last frame
volatile int spk_data_size;
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

static inline bool ppm_fifo_push(uint32_t value) {
    uint8_t next_head = (ppm_fifo_head + 1) % PPM_FIFO_SIZE;
    if (next_head == ppm_fifo_tail) {
        // FIFO полон
        fifo_full_count++;
        return false;
    }
    ppm_fifo[ppm_fifo_head] = value;
    ppm_fifo_head           = next_head;
    fifo_push_count++;
    return true;
}

static inline bool ppm_fifo_pop(uint32_t *value) {
    if (ppm_fifo_head == ppm_fifo_tail) {
        // FIFO пуст
        fifo_empty_count++;
        return false;
    }
    *value        = ppm_fifo[ppm_fifo_tail];
    ppm_fifo_tail = (ppm_fifo_tail + 1) % PPM_FIFO_SIZE;
    fifo_pop_count++;
    return true;
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

void timer0_irq_handler() {
    if (timer_hw->intr & (1u << 0)) {
        timer_hw->intr = 1u << 0;

        static uint32_t irq_count = 0;
        irq_count++;

        uint32_t ppm_value;
        if (ppm_fifo_pop(&ppm_value)) {
            generate_pulse(MIN_INTERVAL_CYCLES + ppm_value, false);

            // if (irq_count % 500 == 0) {
            //     printf("IRQ #%lu: PPM=%u\r\n", irq_count, ppm_value);
            // }
        }
        else {
            generate_pulse(MIN_INTERVAL_CYCLES, false);    // тишина
        }

        timer_hw->alarm[0] = timer_hw->timerawl + audio_frame_ticks;
    }
}

// Main function of Core1 (audio transmission)
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

    init_pulse_generator(PIO_FREQ);

    // LED для Core1
    bool led_state = false;

    audio_frame_ticks = 1000000 / AUDIO_SAMPLE_RATE;

    // Setup timer interrupt for audio sampling
    irq_set_exclusive_handler(TIMER_IRQ_0, timer0_irq_handler);
    hw_set_bits(&timer_hw->inte, (1u << 0));
    irq_set_enabled(TIMER_IRQ_0, true);

    timer_hw->alarm[0] = timer_hw->timerawl + audio_frame_ticks;

    // printf("Test: PCM max (32767) -> PPM=%u\n", audio_to_ppm(32767));
    // printf("Test: PCM half (16384) -> PPM=%u\n", audio_to_ppm(16384));
    // printf("Test: PCM zero (0) -> PPM=%u\n", audio_to_ppm(0));
    // printf("Test: PCM zero (-16384) -> PPM=%u\n", audio_to_ppm(-16384));
    // printf("Test: PCM max (-32767) -> PPM=%u\n", audio_to_ppm(-32767));

    // Main operation loop on Core1
    while (1) {
        tud_task();
        audio_task();
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

    TU_LOG2("Set interface %d alt %d\r\n", itf, alt);
    if (ITF_NUM_AUDIO_STREAMING_SPK == itf && alt != 0)
        blink_interval_ms = BLINK_STREAMING;

    // Clear buffer when streaming format is changed
    spk_data_size = 0;
    if (alt != 0) {
        current_resolution = resolutions_per_format[alt - 1];
        audio_frame_ticks  = calculate_audio_frame_ticks();
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

bool tud_audio_rx_done_pre_read_cb(uint8_t rhport, uint16_t n_bytes_received, uint8_t func_id, uint8_t ep_out, uint8_t cur_alt_setting) {
    (void)rhport;
    (void)func_id;
    (void)cur_alt_setting;

    // Если буфер занят, отказываемся принимать новые данные
    if (spk_buffer_busy) {
        // Сообщаем об этом для отладки
        static uint32_t last_busy_log = 0;
        if (board_millis() - last_busy_log > 50) {
            last_busy_log = board_millis();
            printf("USB: Device busy, NAK sent\r\n");
        }

        // Просим TinyUSB отправить NAK пакет
        // tud_edpt_stall(ep_out);
        return false;
    }

    spk_data_size = tud_audio_read(spk_buf, n_bytes_received);
    if (spk_data_size > 0) {
        spk_buffer_busy = true;
    }

    return true;
}

void audio_task(void) {
    // Вывод статистики FIFO раз в секунду
    static uint32_t last_stats_time = 0;
    if (board_millis() - last_stats_time > 1000) {
        last_stats_time = board_millis();

        // Вычисляем текущее заполнение FIFO
        uint8_t fifo_usage;
        if (ppm_fifo_head >= ppm_fifo_tail) {
            fifo_usage = ppm_fifo_head - ppm_fifo_tail;
        }
        else {
            fifo_usage = PPM_FIFO_SIZE - (ppm_fifo_tail - ppm_fifo_head);
        }

        printf("FIFO: push=%lu pop=%lu full=%lu empty=%lu fill=%u/%u\r\n",
               fifo_push_count,
               fifo_pop_count,
               fifo_full_count,
               fifo_empty_count,
               fifo_usage,
               PPM_FIFO_SIZE);
    }

    if (spk_data_size) {
        bool fifo_full = false;
        if (current_resolution == 16) {
            int16_t *src   = (int16_t *)spk_buf;
            int16_t *limit = (int16_t *)spk_buf + spk_data_size / 2;

            static uint32_t debug_counter = 0;

            while (src <= limit) {
                int32_t  left      = *src++;
                int32_t  right     = *src++;
                int16_t  mono      = (int16_t)((left >> 1) + (right >> 1));
                uint32_t ppm_value = audio_to_ppm(mono);

                // Отладка каждые 50 сэмплов
                if (++debug_counter % 50 == 0) {
                    printf("PCM: left=%d, right=%d, mono=%d -> PPM=%u\n",
                           (int)left,
                           (int)right,
                           (int)mono,
                           ppm_value);
                }
                if (!ppm_fifo_push(ppm_value)) {
                    fifo_full = true;
                    break;
                }
            }
            int used = (uint8_t *)src - (uint8_t *)spk_buf;
            if (used < spk_data_size) {
                memmove(spk_buf, src, spk_data_size - used);
            }
            spk_data_size -= used;
        }
        else if (current_resolution == 24) {
            int32_t *src   = (int32_t *)spk_buf;
            int32_t *limit = (int32_t *)spk_buf + spk_data_size / 4;
            while (src <= limit) {
                int32_t  left      = *src++;
                int32_t  right     = *src++;
                int32_t  mono      = (int32_t)((left >> 1) + (right >> 1));
                uint32_t ppm_value = audio24_to_ppm(mono);
                if (!ppm_fifo_push(ppm_value)) {
                    fifo_full = true;
                    break;
                }
            }
            int used = (uint8_t *)src - (uint8_t *)spk_buf;
            if (used < spk_data_size) {
                memmove(spk_buf, src, spk_data_size - used);
            }
            spk_data_size -= used;
        }
        // На этот код:
        if (spk_data_size == 0) {
            // Просто сбрасываем флаг busy
            spk_buffer_busy = false;
            printf("Буфер освобожден, прием возобновлен\r\n");
        }
    }

    // 2. Обработка входящих данных из FIFO (от второго ядра) в USB (микрофон) — без изменений
    static uint32_t last_received_value = 0;
    static uint16_t mic_buffer_index    = 0;

    if (multicore_fifo_rvalid()) {
        uint32_t ppm_value  = multicore_fifo_pop_blocking();
        last_received_value = ppm_value;

        if (current_resolution == 16) {
            int16_t  audio_sample = ppm_to_audio(ppm_value);
            int16_t *dst          = (int16_t *)((uint8_t *)mic_buf + mic_buffer_index);
            *dst++                = audio_sample;
            *dst++                = audio_sample;
            mic_buffer_index += 4;
            if (mic_buffer_index >= 48) {
                tud_audio_write((uint8_t *)mic_buf, mic_buffer_index);
                mic_buffer_index = 0;
            }
        }
        else if (current_resolution == 24) {
            int32_t  audio_sample = ppm_to_audio24(ppm_value);
            int32_t *dst          = (int32_t *)((uint8_t *)mic_buf + mic_buffer_index);
            *dst++                = audio_sample;
            *dst++                = audio_sample;
            mic_buffer_index += 8;
            if (mic_buffer_index >= 96) {
                tud_audio_write((uint8_t *)mic_buf, mic_buffer_index);
                mic_buffer_index = 0;
            }
        }
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

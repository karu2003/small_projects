#include "common.h"
#include "hardware/uart.h"
#include "pico/sem.h"
#include "usb_descriptors.h"
#include <bsp/board_api.h>
#include <limits.h>
#include <string.h>

// List of supported sample rates
const uint32_t sample_rates[] = {44100, AUDIO_SAMPLE_RATE};

uint32_t current_sample_rate = AUDIO_SAMPLE_RATE;

#define UART_ID   uart0
#define BAUD_RATE 115200

#define UART_TX_PIN 16    // GPIO16 - UART0 TX
#define UART_RX_PIN 17    // GPIO17 - UART0 RX

#define N_SAMPLE_RATES TU_ARRAY_SIZE(sample_rates)

static uint32_t blink_interval_ms = BLINK_NOT_MOUNTED;

// Audio controls
// Current states
int8_t  mute[CFG_TUD_AUDIO_FUNC_1_N_CHANNELS_RX + 1];      // +1 for master channel 0
int16_t volume[CFG_TUD_AUDIO_FUNC_1_N_CHANNELS_RX + 1];    // +1 for master channel 0

// Buffer for microphone data
int32_t mic_buf[CFG_TUD_AUDIO_FUNC_1_EP_IN_SW_BUF_SZ / 4];
int16_t *mic_dst;
// Buffer for speaker data
int32_t spk_buf[CFG_TUD_AUDIO_FUNC_1_EP_OUT_SW_BUF_SZ / 4];
// Speaker data size received in the last frame
uint16_t spk_data_size;
// Resolution per format
const uint8_t resolutions_per_format[CFG_TUD_AUDIO_FUNC_1_N_FORMATS] = {CFG_TUD_AUDIO_FUNC_1_FORMAT_1_RESOLUTION_RX,
                                                                        CFG_TUD_AUDIO_FUNC_1_FORMAT_2_RESOLUTION_RX};
// Current resolution, update on format change
uint8_t current_resolution;
uint8_t has_custom_value = false;
uint16_t pcm_ticks_in_buffer = 0;

// Двойные буферы для динамика (USB -> PPM)
static spk_ppm_buffer_t spk_buffers[2];
static volatile uint8_t current_spk_write_buffer = 0;    // Буфер для подготовки PPM
static volatile uint8_t current_spk_read_buffer  = 0;    // Буфер для чтения в timer

// // Двойные буферы для микрофона (PPM -> USB)
// static mic_pcm_buffer_t mic_buffers[2];
// static volatile uint8_t current_mic_write_buffer = 0;    // Буфер для записи из PPM
// static volatile uint8_t current_mic_read_buffer  = 0;    // Буфер для чтения в USB

static volatile uint32_t timer_irq_count      = 0;
static volatile uint32_t spk_buf_pos          = 0;
static volatile bool     buffer_being_updated = false;

volatile statistics_t statistics = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};

void led_blinking_task(void);
void spk_task(void);
void mic_task(void);
void audio_control_task(void);
void statistics_task(void);

static PIO  pio = pio1;    // Use another PIO to avoid conflicts
static uint sm_gen;

uint32_t audio_frame_ticks;

void setup_uart() {
    uart_init(UART_ID, BAUD_RATE);

    gpio_set_function(UART_TX_PIN, GPIO_FUNC_UART);
    gpio_set_function(UART_RX_PIN, GPIO_FUNC_UART);

    stdio_uart_init();
}

void init_double_buffering(void) {
    for (int i = 0; i < 2; i++) {
        spk_buffers[i].size     = 0;
        spk_buffers[i].position = 0;
        spk_buffers[i].ready    = false;
    }
    current_spk_write_buffer = 0;
    current_spk_read_buffer  = 0;

    // for (int i = 0; i < 2; i++) {
    //     mic_buffers[i].size     = 0;
    //     mic_buffers[i].position = 0;
    //     mic_buffers[i].ready    = false;
    // }
    // current_mic_write_buffer = 0;
    // current_mic_read_buffer  = 0;
}

void init_core_shared_buffer(void) {
    // Инициализация счетчиков и индексов
    shared_ppm_data.write_index = 0;
    shared_ppm_data.read_index  = 0;
    shared_ppm_data.size[0]     = 0;
    shared_ppm_data.size[1]     = 0;

    // Инициализация семафоров
    sem_init(&shared_ppm_data.sem_empty, 2, 2);    // Два пустых буфера
    sem_init(&shared_ppm_data.sem_full, 0, 2);     // Нет заполненных буферов

    // Установим флаг инициализации
    // sem_initialized = true;
    sem_initialized = false;
}

void generate_pulse(uint32_t pause_width) {
    pio_sm_put_blocking(pio, sm_gen, pause_width);
}

uint32_t calculate_audio_frame_ticks() {
    return 1000000 / current_sample_rate;
}

// Initialize PIO for pulse generator
void init_pulse_generator(float freq) {
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wsign-conversion"
    sm_gen      = pio_claim_unused_sm(pio, true);
    uint offset = pio_add_program(pio, &pulse_generator_program);
#pragma GCC diagnostic pop
    pio_sm_config c = pulse_generator_program_get_default_config(offset);

    // Setup pins for PIO
    sm_config_set_set_pins(&c, PULSE_GEN_PIN, 1);
    pio_gpio_init(pio, PULSE_GEN_PIN);
    pio_sm_set_consecutive_pindirs(pio, sm_gen, PULSE_GEN_PIN, 1, true);

    sm_config_set_clkdiv(&c, (float)clock_get_hz(clk_sys) / freq);

    pio_sm_init(pio, sm_gen, offset, &c);
    pio_sm_set_enabled(pio, sm_gen, true);
}

uint16_t audio_to_ppm(int16_t audio_sample) {
    return (uint16_t)(((int64_t)audio_sample + 32768) * 1024 / 65536);
}

int16_t ppm_to_audio(uint32_t ppm_value) {
    ppm_value &= 0x3FF;
    return (int16_t)(((int64_t)ppm_value * 65536 / 1024) - 32768);
}
void timer0_irq_handler() {
    if (timer_hw->intr & (1u << 0)) {
        timer_hw->intr = 1u << 0;

        uint32_t ppm_value;

        if (spk_buffers[current_spk_read_buffer].ready &&
            spk_buffers[current_spk_read_buffer].position < spk_buffers[current_spk_read_buffer].size) {
            
            // statistics.total_summed_ppm_out += spk_buffers[current_spk_read_buffer].ppm_buffer[spk_buffers[current_spk_read_buffer].position];

            ppm_value = MIN_INTERVAL_CYCLES +
                        spk_buffers[current_spk_read_buffer].ppm_buffer[spk_buffers[current_spk_read_buffer].position++];
            statistics.total_ppm_sent++;
            statistics.total_summed_ppm_out += (ppm_value - MIN_INTERVAL_CYCLES);

            if (spk_buffers[current_spk_read_buffer].position >= spk_buffers[current_spk_read_buffer].size) {
                spk_buffers[current_spk_read_buffer].ready    = false;
                spk_buffers[current_spk_read_buffer].position = 0;
                spk_buffers[current_spk_read_buffer].size     = 0;

                current_spk_read_buffer = (uint8_t)((current_spk_read_buffer + 1) % 2);
            }
        }
        else {
            ppm_value = MIN_INTERVAL_CYCLES;
        }

        generate_pulse(ppm_value);
        statistics.total_sent++;
        timer_hw->alarm[0] = timer_hw->timerawl + audio_frame_ticks;
    }
}

void first_core_main() {
    board_init();
    setup_uart();

    init_core_shared_buffer();

    tusb_rhport_init_t dev_init = {
        .role  = TUSB_ROLE_DEVICE,
        .speed = TUSB_SPEED_AUTO};
    tusb_init(BOARD_TUD_RHPORT, &dev_init);

    if (board_init_after_tusb) {
        board_init_after_tusb();
    }

    TU_LOG1("Laser Audio running\r\n");
    stdio_init_all();

    init_double_buffering();

    init_pulse_generator(PIO_FREQ);

    audio_frame_ticks = 1000000 / AUDIO_SAMPLE_RATE;

    // Setup timer interrupt for audio sampling
    irq_set_exclusive_handler(TIMER_IRQ_0, timer0_irq_handler);
    hw_set_bits(&timer_hw->inte, (1u << 0));
    irq_set_enabled(TIMER_IRQ_0, true);

    timer_hw->alarm[0] = timer_hw->timerawl + audio_frame_ticks;

    // test audio zo PPM
    printf("Audio to PPM(-32768): %d\r\n", audio_to_ppm(-32768));
    printf("Audio to PPM(0): %d\r\n", audio_to_ppm(0));
    printf("Audio to PPM(32767): %d\r\n", audio_to_ppm(32767));
    printf("PPM to Audio(0): %d\r\n", ppm_to_audio(0));
    printf("PPM to Audio(512): %d\r\n", ppm_to_audio(512));
    printf("PPM to Audio(1023): %d\r\n", ppm_to_audio(1023));

    // Main operation loop on Core1
    while (1) {
        tud_task();
        spk_task();
        mic_task();
        statistics_task();
        // audio_control_task();
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
        audio_frame_ticks   = calculate_audio_frame_ticks();

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
    }

    return true;
}

bool tud_audio_rx_done_pre_read_cb(uint8_t rhport, uint16_t n_bytes_received, uint8_t func_id, uint8_t ep_out, uint8_t cur_alt_setting) {
    (void)rhport;
    (void)func_id;
    (void)ep_out;
    (void)cur_alt_setting;

    // if (sem_initialized) {
    //     shared_ppm_data.packet_size = (uint16_t)n_bytes_received;
    // }

    if (!spk_buffers[current_spk_write_buffer].ready) {
        spk_data_size = tud_audio_read(spk_buf, n_bytes_received);
        statistics.total_pcm_received += spk_data_size;
        TU_LOG1("RX done pre read callback called, received %d bytes\r\n", spk_data_size);
        if (sem_initialized) {
            shared_ppm_data.packet_size = spk_data_size;
        }
        return true;
    }
    TU_LOG1("RX done pre read callback called, but buffer is not ready\r\n");
    return false;
}

bool tud_audio_tx_done_pre_load_cb(uint8_t rhport, uint8_t itf, uint8_t ep_in, uint8_t cur_alt_setting) {
    (void)rhport;
    (void)itf;
    (void)ep_in;
    (void)cur_alt_setting;

    // static uint8_t silence[192] = {0};
    // tud_audio_write(silence, sizeof(silence));

    return true;

    // if (mic_buffers[current_mic_read_buffer].ready) {
    //     uint16_t written = tud_audio_write((uint8_t *)mic_buffers[current_mic_read_buffer].pcm_buffer,
    //                                        (uint16_t)(mic_buffers[current_mic_read_buffer].size * sizeof(int16_t)));

    //     if (written > 0) {
    //         // Буфер успешно отправлен, освобождаем его
    //         mic_buffers[current_mic_read_buffer].ready    = false;
    //         mic_buffers[current_mic_read_buffer].size     = 0;
    //         mic_buffers[current_mic_read_buffer].position = 0;
    //         current_mic_read_buffer                       = (uint8_t)((current_mic_read_buffer + 1) % 2);
    //         return true;
    //     }
    // }
    // return false;
}

void spk_task(void) {
    if (spk_data_size && !spk_buffers[current_spk_write_buffer].ready) {
        if (current_resolution == 16) {
            int16_t  *src        = (int16_t *)spk_buf;
            int16_t  *limit      = (int16_t *)spk_buf + spk_data_size / 2;
            uint16_t *dst        = spk_buffers[current_spk_write_buffer].ppm_buffer;
            uint16_t  buffer_pos = 0;

            while (src < limit) {
                int32_t left  = *src++;
                int32_t right = *src++;
                int16_t mixed = (int16_t)((left >> 1) + (right >> 1));
                
                // statistics.total_summed_ppm_out += audio_to_ppm(mixed);
                dst[buffer_pos++] = audio_to_ppm(mixed);
                statistics.total_ppm_convert++;
                
            }

            spk_buffers[current_spk_write_buffer].size     = buffer_pos;
            spk_buffers[current_spk_write_buffer].position = 0;
            spk_buffers[current_spk_write_buffer].ready    = true;

            current_spk_write_buffer = (uint8_t)((current_spk_write_buffer + 1) % 2);
        }
        spk_data_size = 0;
    }
}

void mic_task(void) {
    // Проверяем, что USB готов
    if (tud_audio_mounted() && current_resolution == 16) {
        // Размер пакета
        uint16_t packet_size = 96;

        // Заполняем буфер тишиной
        if (pcm_ticks_in_buffer == 0) {
            memset(mic_buf, 0, packet_size);
            mic_dst           = (int16_t *)mic_buf;
        }

        uint16_t samples_added = 0;
        uint16_t max_samples   = packet_size / 4;

        // Сначала пробуем получить данные из семафоров, если они инициализированы
        if (sem_initialized) {
            // Не блокирующая проверка наличия данных
            if (sem_try_acquire(&shared_ppm_data.sem_full)) {
                // Есть данные - обрабатываем буфер
                uint8_t  read_buf  = shared_ppm_data.read_index;
                uint16_t available = shared_ppm_data.size[read_buf];

                // Преобразуем PPM в PCM данные для USB
                for (uint16_t i = 0; i < available && samples_added < max_samples; i++) {
                    uint32_t ppm_value = shared_ppm_data.buffer[read_buf][i];
                    statistics.total_summed_ppm_in_usb += ppm_value;
                    int16_t  pcm       = ppm_to_audio(ppm_value);
                    statistics.total_pcm_convert++;

                    *mic_dst++ = pcm;    // Левый канал
                    *mic_dst++ = pcm;    // Правый канал

                    samples_added++;
                }

                // Освобождаем буфер
                shared_ppm_data.size[read_buf] = 0;
                shared_ppm_data.read_index     = (uint8_t)((read_buf + 1) % 2);    // Добавляем явное приведение типа
                sem_release(&shared_ppm_data.sem_empty);
            }
        }

        // Если данных из семафоров недостаточно или их нет, используем FIFO
        while (multicore_fifo_rvalid() /*&& samples_added < max_samples*/) {
            uint32_t ppm_value = multicore_fifo_pop_blocking();
            statistics.total_summed_ppm_in_usb += ppm_value;
            int16_t  pcm       = ppm_to_audio(ppm_value);
            pcm_ticks_in_buffer++;
            statistics.total_pcm_convert++;

            *mic_dst++ = pcm;

            samples_added++;

            if(pcm_ticks_in_buffer == packet_size){
                break;
            }
        }

        // Отправка данных через USB
        // TODO: send incomplete packets at end of transmission.
        if(pcm_ticks_in_buffer == packet_size) {
            uint16_t bytes_written = tud_audio_write((uint8_t *)mic_buf, pcm_ticks_in_buffer*2); // pcm_ticks_in_buffer*2 because pcm ticks are 16bit and we're counting bytes here.
            statistics.total_ticks_attempt_send_to_usb += pcm_ticks_in_buffer;
            statistics.total_bytes_sent_to_usb += bytes_written;
            pcm_ticks_in_buffer = 0;
        }
    }
}

// void mic_task(void) {
//     // Проверяем, что USB готов и есть данные в FIFO
//     if (tud_audio_mounted() && current_resolution == 16) {
//         // Размер пакета должен соответствовать размеру входящего пакета
//         uint16_t packet_size = spk_data_size > 0 ? spk_data_size : 192;

//         // Заполняем буфер тишиной
//         memset(mic_buf, 0, packet_size);

//         // Заполняем буфер доступными данными из FIFO (сколько есть)
//         int16_t *dst           = (int16_t *)mic_buf;
//         uint16_t samples_added = 0;
//         uint16_t max_samples   = packet_size / 4;    // Число моно-сэмплов (пар L/R)

//         while (multicore_fifo_rvalid() && samples_added < max_samples) {
//             // Не блокирующее чтение из FIFO
//             uint32_t ppm_value = multicore_fifo_pop_blocking();    // Здесь есть данные, так что блокировка минимальна

//             // Преобразование PPM -> PCM и запись в буфер (стерео)
//             int16_t pcm = ppm_to_audio(ppm_value);
//             *dst++      = pcm;    // Левый канал
//             *dst++      = pcm;    // Правый канал

//             samples_added++;
//         }

//         // Отправка данных напрямую без проверки результата
//         tud_audio_write((uint8_t *)mic_buf, packet_size);
//     }
// }

// void mic_task(void) {
//     // Если буфер для записи свободен, и пришли данные по FIFO
//     if (!mic_buffers[current_mic_write_buffer].ready && multicore_fifo_rvalid()) {
//         uint32_t ppm_value = multicore_fifo_pop_blocking();

//         if (current_resolution == 16) {
//             int16_t audio_sample = ppm_to_audio(ppm_value);

//             int      pos = mic_buffers[current_mic_write_buffer].position;
//             int16_t *dst = (int16_t *)mic_buffers[current_mic_write_buffer].pcm_buffer + pos;

//             *dst++ = audio_sample;
//             *dst++ = audio_sample;
//             mic_buffers[current_mic_write_buffer].position += 2;

//             if (mic_buffers[current_mic_write_buffer].position >= 96) {
//                 mic_buffers[current_mic_write_buffer].size =
//                     mic_buffers[current_mic_write_buffer].position;
//                 mic_buffers[current_mic_write_buffer].ready = true;

//                 if (!mic_buffers[current_mic_read_buffer].ready) {
//                     current_mic_read_buffer = current_mic_write_buffer;
//                 }

//                 current_mic_write_buffer = (uint8_t)((current_mic_write_buffer + 1) % 2);
//             }
//         }
//     }
// }

//--------------------------------------------------------------------+
// BLINKING TASK
//--------------------------------------------------------------------+
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

void statistics_task(void) {
    // Print statistics every 15 seconds
    static uint32_t last_print_ms = 0;

    if (board_millis() - last_print_ms < 15000)
        return;
    last_print_ms = board_millis();

    printf("Statistics:\r\n");
    printf("  Total PCM received: %lu\r\n", statistics.total_pcm_received);
    printf("  Total PPM converted: %lu\r\n", statistics.total_ppm_convert);
    printf("  Total PCM converted: %lu\r\n", statistics.total_pcm_convert);
    printf("  Total PPM sent: %lu\r\n", statistics.total_ppm_sent);
    printf("  Total PPM received: %lu\r\n", statistics.total_ppm_received);
    printf("  Total sent: %lu\r\n", statistics.total_sent);
    printf("  Total received: %lu\r\n", statistics.total_received);
    printf("  Total summed PPM out: %llu\r\n", statistics.total_summed_ppm_out);
    printf("  Total summed PPM in: %llu\r\n", statistics.total_summed_ppm_in);
    printf("  Total summed PPM before USB communication: %llu\r\n", statistics.total_summed_ppm_in_usb);
    printf("  Total ticks attempted to send to USB: %llu\r\n", statistics.total_ticks_attempt_send_to_usb);
    printf("  Total total bytes sent to USB: %llu\r\n", statistics.total_bytes_sent_to_usb);

    // Reset statistics after printing
    statistics.total_pcm_received      = 0;
    statistics.total_ppm_convert       = 0;
    statistics.total_pcm_convert       = 0;
    statistics.total_ppm_sent          = 0;
    statistics.total_ppm_received      = 0;
    statistics.total_sent              = 0;
    statistics.total_received          = 0;
    statistics.total_summed_ppm_out    = 0;
    statistics.total_summed_ppm_in     = 0;
    statistics.total_summed_ppm_in_usb = 0;
    statistics.total_ticks_attempt_send_to_usb = 0;
    statistics.total_bytes_sent_to_usb = 0;
}

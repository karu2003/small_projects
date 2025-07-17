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
int32_t  mic_buf[CFG_TUD_AUDIO_FUNC_1_EP_IN_SW_BUF_SZ / 4];
int16_t *mic_dst;
// Buffer for speaker data
// int32_t spk_buf[CFG_TUD_AUDIO_FUNC_1_EP_OUT_SW_BUF_SZ / 4];
// Speaker data size received in the last frame
uint16_t spk_data_size;
// Resolution per format
const uint8_t resolutions_per_format[CFG_TUD_AUDIO_FUNC_1_N_FORMATS] = {CFG_TUD_AUDIO_FUNC_1_FORMAT_1_RESOLUTION_RX,
                                                                        CFG_TUD_AUDIO_FUNC_1_FORMAT_2_RESOLUTION_RX};
// Current resolution, update on format change
uint8_t  current_resolution;
uint16_t pcm_ticks_in_buffer = 0;

// Double buffers for speaker (USB -> PPM)
// static spk_ppm_buffer_t spk_buffers[2];
// static volatile uint8_t current_spk_write_buffer = 0;    // Buffer for PPM preparation
// static volatile uint8_t current_spk_read_buffer  = 0;    // Buffer for reading in timer

void led_blinking_task(void);
void spk_task(void);
void mic_task(void);
void audio_processing_task(void);

static PIO  pio = pio1;
static uint sm_gen;

uint32_t audio_frame_ticks;

static uint dma_chan_pdm;
static uint pio_sm;

void setup_pdm_system() {
    // Настройка PIO для PDM вывода
    PIO pio = pio0;
    pio_sm = 0;
    uint offset = pio_add_program(pio, &pdm_out_program);
    
    pio_sm_config c = pdm_out_program_get_default_config(offset);
    sm_config_set_out_pins(&c, LASER_PIN, 1);
    sm_config_set_sideset_pins(&c, LASER_PIN);
    
    // Настройка частоты PDM
    float div = (float)clock_get_hz(clk_sys) / PDM_FREQ;
    sm_config_set_clkdiv(&c, div);
    
    sm_config_set_out_shift(&c, false, true, 32);
    sm_config_set_fifo_join(&c, PIO_FIFO_JOIN_TX);
    
    // Инициализация GPIO
    pio_gpio_init(pio, LASER_PIN);
    pio_sm_set_consecutive_pindirs(pio, pio_sm, LASER_PIN, 1, true);
    
    // Запуск state machine
    pio_sm_init(pio, pio_sm, offset, &c);
    pio_sm_set_enabled(pio, pio_sm, true);
    
    // Настройка DMA для PDM
    dma_chan_pdm = dma_claim_unused_channel(true);
    dma_channel_config dma_c = dma_channel_get_default_config(dma_chan_pdm);
    
    channel_config_set_transfer_data_size(&dma_c, DMA_SIZE_32);
    channel_config_set_read_increment(&dma_c, true);
    channel_config_set_write_increment(&dma_c, false);
    channel_config_set_dreq(&dma_c, pio_get_dreq(pio, pio_sm, true));
    channel_config_set_chain_to(&dma_c, dma_chan_pdm);  // Зацикливание
    
    // Настройка прерывания DMA
    dma_channel_set_irq0_enabled(dma_chan_pdm, true);
    irq_set_exclusive_handler(DMA_IRQ_0, dma_pdm_handler);
    irq_set_enabled(DMA_IRQ_0, true);
    
    // Запуск первого DMA transfer
    dma_channel_configure(
        dma_chan_pdm, &dma_c,
        &pio0_hw->txf[pio_sm],
        audio_buffers.pdm_buffer_a,
        BUFFER_SIZE/32,
        true
    );
}

// Обработчик DMA прерывания
void __isr dma_pdm_handler() {
    if (dma_channel_get_irq0_status(dma_chan_pdm)) {
        dma_channel_acknowledge_irq0(dma_chan_pdm);
        
        // Переключение буферов
        audio_buffers.pdm_buffer_switch = !audio_buffers.pdm_buffer_switch;
        uint32_t *next_buffer = audio_buffers.pdm_buffer_switch ? 
                               audio_buffers.pdm_buffer_b : 
                               audio_buffers.pdm_buffer_a;
        
        // Настройка следующего transfer
        dma_channel_set_read_addr(dma_chan_pdm, next_buffer, true);
        
        // Сигнал для обработки в основном цикле
        audio_buffers.pdm_ready = true;
    }
}

void setup_uart() {
    uart_init(UART_ID, BAUD_RATE);
    gpio_set_function(UART_TX_PIN, GPIO_FUNC_UART);
    gpio_set_function(UART_RX_PIN, GPIO_FUNC_UART);
    stdio_uart_init();
}

// void init_double_buffering(void) {
//     for (int i = 0; i < 2; i++) {
//         spk_buffers[i].size     = 0;
//         spk_buffers[i].position = 0;
//         spk_buffers[i].ready    = false;
//     }
//     current_spk_write_buffer = 0;
//     current_spk_read_buffer  = 0;
// }

void generate_pulse(uint32_t pause_width) {
    pio_sm_put_blocking(pio, sm_gen, pause_width);
}

uint32_t calculate_audio_frame_ticks() {
    return 1000000 / current_sample_rate;
}

uint32_t pcm_to_pdm_advanced(uint16_t *pcm_samples, int count) {
    uint32_t pdm_word = 0;
    
    for (int i = 0; i < count && i < 32; i++) {
        // Преобразование в знаковый формат
        int32_t pcm_signed = (int32_t)pcm_samples[i] - 32768;
        
        // Дельта-сигма модулятор второго порядка
        int32_t error1 = pcm_signed - ds_modulator.prev_output;
        ds_modulator.integrator1 += error1;
        
        int32_t error2 = ds_modulator.integrator1 - ds_modulator.prev_output;
        ds_modulator.integrator2 += error2;
        
        // Квантование
        int32_t output;
        if (ds_modulator.integrator2 >= 0) {
            output = 32767;
            pdm_word |= (1 << i);
        } else {
            output = -32768;
        }
        
        ds_modulator.prev_output = output;
    }
    
    return pdm_word;
}



// Initialize PIO for pulse generator
// void init_pulse_generator(float freq) {
// #pragma GCC diagnostic push
// #pragma GCC diagnostic ignored "-Wsign-conversion"
//     sm_gen      = pio_claim_unused_sm(pio, true);
//     uint offset = pio_add_program(pio, &pulse_generator_program);
// #pragma GCC diagnostic pop
//     pio_sm_config c = pulse_generator_program_get_default_config(offset);

//     // Setup pins for PIO
//     sm_config_set_set_pins(&c, PULSE_GEN_PIN, 1);
//     pio_gpio_init(pio, PULSE_GEN_PIN);
//     pio_sm_set_consecutive_pindirs(pio, sm_gen, PULSE_GEN_PIN, 1, true);

//     sm_config_set_clkdiv(&c, (float)clock_get_hz(clk_sys) / freq);

//     pio_sm_init(pio, sm_gen, offset, &c);
//     pio_sm_set_enabled(pio, sm_gen, true);
// }

// uint16_t audio_to_ppm(int16_t audio_sample) {
//     return (uint16_t)(((int64_t)audio_sample + 32768) * 1024 / 65536);
// }

// int16_t ppm_to_audio(uint32_t ppm_value) {
//     ppm_value &= 0x3FF;
//     return (int16_t)(((int64_t)ppm_value * 65536 / 1024) - 32768);
// }

// void timer0_irq_handler() {
//     if (timer_hw->intr & (1u << 0)) {
//         timer_hw->intr = 1u << 0;

//         uint32_t ppm_value;

//         if (spk_buffers[current_spk_read_buffer].ready &&
//             spk_buffers[current_spk_read_buffer].position < spk_buffers[current_spk_read_buffer].size) {

//             ppm_value = MIN_INTERVAL_CYCLES +
//                         spk_buffers[current_spk_read_buffer].ppm_buffer[spk_buffers[current_spk_read_buffer].position++];

//             if (spk_buffers[current_spk_read_buffer].position >= spk_buffers[current_spk_read_buffer].size) {
//                 spk_buffers[current_spk_read_buffer].ready    = false;
//                 spk_buffers[current_spk_read_buffer].position = 0;
//                 spk_buffers[current_spk_read_buffer].size     = 0;

//                 current_spk_read_buffer = (uint8_t)((current_spk_read_buffer + 1) % 2);
//             }
//         }
//         else {
//             ppm_value = MIN_INTERVAL_CYCLES;
//         }

//         generate_pulse(ppm_value);
//         timer_hw->alarm[0] = timer_hw->timerawl + audio_frame_ticks;
//     }
// }

void first_core_main() {
    board_init();
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

    init_double_buffering();

    init_pulse_generator(PIO_FREQ);

    audio_frame_ticks = 1000000 / AUDIO_SAMPLE_RATE;

    // Setup timer interrupt for audio sampling
    //irq_set_exclusive_handler(TIMER_IRQ_0, timer0_irq_handler);
    hw_set_bits(&timer_hw->inte, (1u << 0));
    irq_set_enabled(TIMER_IRQ_0, true);

    timer_hw->alarm[0] = timer_hw->timerawl + audio_frame_ticks;

    // Main operation loop on Core1
    while (1) {
        tud_task();
        spk_task();
        audio_processing_task();
        mic_task();
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

    if (!spk_buffers[current_spk_write_buffer].ready) {
        spk_data_size = tud_audio_read(spk_buf, n_bytes_received);
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

    return true;
}

void spk_task(void) {
    if (spk_data_size && !spk_buffers[current_spk_write_buffer].ready) {
        if (current_resolution == 16) {
            int16_t  *src        = (int16_t *)spk_buf;
            int16_t  *limit      = (int16_t *)spk_buf + spk_data_size / 2;
            uint16_t *dst        = spk_buffers[current_spk_write_buffer].ppm_buffer;
            uint16_t  buffer_pos = 0;

            while (src < limit) {
                int32_t left      = *src++;
                int32_t right     = *src++;
                int16_t mixed     = (int16_t)((left >> 1) + (right >> 1));
                dst[buffer_pos++] = audio_to_ppm(mixed);
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
    static absolute_time_t last_fill_time;

    if (!tud_audio_mounted() || current_resolution != 16) {
        return;
    }

    const uint16_t packet_size_bytes = 96;

    // Initialize on first call or after sending
    if (pcm_ticks_in_buffer == 0) {
        memset(mic_buf, 0, packet_size_bytes);
        mic_dst        = (int16_t *)mic_buf;
        last_fill_time = get_absolute_time();
    }

    while (multicore_fifo_rvalid() && (pcm_ticks_in_buffer < packet_size_bytes)) {
        uint32_t ppm_value = multicore_fifo_pop_blocking();
        int16_t  pcm       = ppm_to_audio(ppm_value);
        *mic_dst++         = pcm;
        pcm_ticks_in_buffer += 2;
    }

    // Check sending conditions:
    bool buffer_full     = (pcm_ticks_in_buffer >= packet_size_bytes);
    bool timeout_expired = absolute_time_diff_us(last_fill_time, get_absolute_time()) >= 1000;

    if (buffer_full || (pcm_ticks_in_buffer > 0 && timeout_expired)) {
        tud_audio_write((uint8_t *)mic_buf, pcm_ticks_in_buffer);
        pcm_ticks_in_buffer = 0;
    }
}

void audio_processing_task() {
    static uint32_t sample_counter = 0;
    
    // Обработка PCM -> PDM когда готовы новые данные
    if (audio_buffers.pcm_ready) {
        uint16_t *pcm_source = audio_buffers.pcm_buffer_switch ? 
                              audio_buffers.pcm_buffer_a : 
                              audio_buffers.pcm_buffer_b;
        
        uint32_t *pdm_dest = audio_buffers.pdm_buffer_switch ? 
                            audio_buffers.pdm_buffer_a : 
                            audio_buffers.pdm_buffer_b;
        
        // Преобразование PCM в PDM
        for (int i = 0; i < BUFFER_SIZE/32; i++) {
            pdm_dest[i] = pcm_to_pdm_advanced(&pcm_source[i * 32], 32);
        }
        
        // Переключение буферов
        audio_buffers.pcm_buffer_switch = !audio_buffers.pcm_buffer_switch;
        audio_buffers.pcm_ready = false;
        
        sample_counter++;
    }
    
    // Мониторинг производительности
    if (sample_counter % 1000 == 0) {
        printf("Processed %lu buffers\n", sample_counter);
    }
}

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

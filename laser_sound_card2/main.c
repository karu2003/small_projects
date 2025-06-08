// main.c - Dual core audio project with USB support
#include "common.h"
#include "usb_descriptors.h"
#include <bsp/board_api.h>
#include <pico/multicore.h>
#include <pico/stdlib.h>
#include <tusb.h>

// Core 0 - PPM receiver
static PIO           pio0_instance = pio0;
static uint          sm_det;
static volatile bool detector_running = false;

// Core 1 - USB audio and PPM generator
static PIO  pio1_instance = pio1;
static uint sm_gen;

volatile uint32_t ppm_code_to_send = 0;
volatile bool     has_custom_value = false;

// ============================================================================
// CORE 0 FUNCTIONS - PPM Reception
// ============================================================================

void update_measurements() {
    while (detector_running && !pio_sm_is_rx_fifo_empty(pio0_instance, sm_det)) {
        uint32_t measured_width  = pio_sm_get(pio0_instance, sm_det);
        uint32_t corrected_width = (measured_width + MIN_TACKT) - MIN_INTERVAL_CYCLES;

        if (corrected_width > 0 && corrected_width <= MAX_CODE) {

            int16_t audio_sample = (int16_t)((corrected_width * 32767) / MAX_CODE);

            // Send audio sample to Core 1 via FIFO
            if (multicore_fifo_wready()) {
                multicore_fifo_push_blocking(audio_sample);
            }
        }
    }
}

void init_pulse_detector(float freq) {
    sm_det               = pio_claim_unused_sm(pio0_instance, true);
    uint          offset = pio_add_program(pio0_instance, &pulse_detector_program);
    pio_sm_config c      = pulse_detector_program_get_default_config(offset);

    sm_config_set_in_pins(&c, PULSE_DET_PIN);
    sm_config_set_jmp_pin(&c, PULSE_DET_PIN);
    pio_gpio_init(pio0_instance, PULSE_DET_PIN);
    pio_sm_set_consecutive_pindirs(pio0_instance, sm_det, PULSE_DET_PIN, 1, false);

    sm_config_set_clkdiv(&c, (float)clock_get_hz(clk_sys) / freq);
    pio_sm_init(pio0_instance, sm_det, offset, &c);
}

void start_detector() {
    pio_sm_clear_fifos(pio0_instance, sm_det);
    pio_sm_set_enabled(pio0_instance, sm_det, true);
    detector_running = true;
}

// Main function for Core0 (receiver)
void first_core_main() {
    init_pulse_detector(PIO_FREQ);
    start_detector();

    bool            led_state       = false;
    absolute_time_t next_led_toggle = make_timeout_time_ms(LED_TIME);

    while (1) {
        update_measurements();

        if (absolute_time_diff_us(get_absolute_time(), next_led_toggle) <= 0) {
            led_state = !led_state;
            gpio_put(LED_PIN, led_state);
            next_led_toggle = make_timeout_time_ms(LED_TIME);
        }

        tight_loop_contents();
    }
}

// ============================================================================
// CORE 1 FUNCTIONS - USB Audio and PPM Generation
// ============================================================================

void generate_pulse(uint32_t pause_width) {
    pio_sm_put_blocking(pio1_instance, sm_gen, pause_width);
}

void timer0_irq_handler() {
    if (timer_hw->intr & (1u << 0)) {
        timer_hw->intr = 1u << 0;

        uint32_t ppm_value;
        if (has_custom_value) {
            ppm_value        = MIN_INTERVAL_CYCLES + ppm_code_to_send;
            has_custom_value = false;
        }
        else {
            ppm_value = MIN_INTERVAL_CYCLES;
        }

        generate_pulse(ppm_value);

        // Schedule next interrupt
        timer_hw->alarm[0] = timer_hw->timerawl + AUDIO_FRAME_TICKS;
    }
}

void init_pulse_generator(float freq) {
    sm_gen               = pio_claim_unused_sm(pio1_instance, true);
    uint          offset = pio_add_program(pio1_instance, &pulse_generator_program);
    pio_sm_config c      = pulse_generator_program_get_default_config(offset);

    sm_config_set_set_pins(&c, PULSE_GEN_PIN, 1);
    pio_gpio_init(pio1_instance, PULSE_GEN_PIN);
    pio_sm_set_consecutive_pindirs(pio1_instance, sm_gen, PULSE_GEN_PIN, 1, true);

    sm_config_set_clkdiv(&c, (float)clock_get_hz(clk_sys) / freq);

    pio_sm_init(pio1_instance, sm_gen, offset, &c);
    pio_sm_set_enabled(pio1_instance, sm_gen, true);
}

// USB Audio callbacks
bool tud_audio_tx_done_pre_load_cb(uint8_t rhport, uint8_t itf, uint8_t ep_in, uint8_t cur_alt_setting) {
    (void)rhport;
    (void)itf;
    (void)ep_in;
    (void)cur_alt_setting;

    // Fill audio buffer with data from Core 0
    for (int i = 0; i < AUDIO_BUFFER_SIZE; i++) {
        // Generate or process audio data here
        audio_buffer_tx[i] = 0; // Placeholder - replace with actual audio data
    }

    // Write audio data to TinyUSB
    return tud_audio_n_write(itf, audio_buffer_tx, AUDIO_BUFFER_SIZE * sizeof(int16_t));
}

bool tud_audio_rx_done_post_read_cb(uint8_t rhport, uint16_t n_bytes_received, uint8_t func_id, uint8_t ep_out, uint8_t cur_alt_setting) {
    (void)rhport;
    (void)func_id;
    (void)ep_out;
    (void)cur_alt_setting;

    // Process received audio data and convert to PPM
    if (n_bytes_received >= sizeof(int16_t)) {
        // Read audio data from TinyUSB
        uint16_t bytes_read = tud_audio_n_read(func_id, audio_buffer_rx, AUDIO_BUFFER_SIZE * sizeof(int16_t));
        
        if (bytes_read >= sizeof(int16_t)) {
            int16_t audio_sample = audio_buffer_rx[0];

            // Convert audio sample to PPM code
            if (audio_sample != 0) {
                // Convert 16-bit signed audio to PPM code (0-1023)
                uint32_t ppm_value = (uint32_t)((audio_sample + 32768) * MAX_CODE / 65536);
                ppm_code_to_send = ppm_value;
                has_custom_value = true;
            }
        }
    }

    return true;
}

// USB mount/unmount callbacks
void tud_mount_cb(void) {
    // Reset audio state when mounted
    memset(audio_buffer_rx, 0, sizeof(audio_buffer_rx));
    memset(audio_buffer_tx, 0, sizeof(audio_buffer_tx));
}

void tud_umount_cb(void) {
    // Clean up when unmounted
}

// Additional audio callbacks that may be required
bool tud_audio_set_itf_close_EP_cb(uint8_t rhport, tusb_control_request_t const * p_request) {
    (void)rhport;
    (void)p_request;
    return true;
}

bool tud_audio_set_itf_cb(uint8_t rhport, tusb_control_request_t const * p_request) {
    (void)rhport;
    (void)p_request;
    return true;
}

bool tud_audio_clock_get_request(uint8_t rhport, audio_control_request_t const *request)
{
    if (request->bEntityID == 0x04) {
        if (request->bControlSelector == AUDIO_CS_CTRL_SAM_FREQ) {
            // Для запроса о диапазоне частот дискретизации
            if (request->bRequest == AUDIO_CS_REQ_CUR) {
                // Текущая частота - 48000 Гц
                uint32_t sample_rate = 48000;
                return tud_audio_buffer_and_schedule_control_xfer(rhport, (tusb_control_request_t const*)request, 
                                                                &sample_rate, sizeof(sample_rate));
            } 
            else if (request->bRequest == AUDIO_CS_REQ_RANGE) {
                // Поддерживаем только одну частоту - 48000 Гц
                audio_control_range_4_t range_param;
                range_param.wNumSubRanges = 1;
                range_param.subrange[0].bMin = 48000;
                range_param.subrange[0].bMax = 48000;
                range_param.subrange[0].bRes = 0;

                return tud_audio_buffer_and_schedule_control_xfer(rhport, (tusb_control_request_t const*)request, 
                                                      &range_param, sizeof(range_param));
            }
        }
    }
    return false;
}

bool tud_audio_clock_set_request(uint8_t rhport, audio_control_request_t const *request, uint8_t const *data)
{
    (void)data;
    if (request->bEntityID == 0x04) {
        if (request->bControlSelector == AUDIO_CS_CTRL_SAM_FREQ) {
            // Принимаем любую частоту в нашем диапазоне (мы поддерживаем только 48000)
            return true;
        }
    }
    return false;
}

// Main function for Core1 (USB audio and PPM generation)
void second_core_main() {
    // Initialize USB
    board_init();

    // Initialize TinyUSB device stack with port number
    tusb_init(BOARD_TUD_RHPORT);

    if (board_init_after_tusb) {
        board_init_after_tusb();
    }

    TU_LOG1("Dual-Core USB Audio Device\r\n");

    // Initialize PPM generator
    init_pulse_generator(PIO_FREQ);

    // Setup timer interrupt for PPM generation
    irq_set_exclusive_handler(TIMER_IRQ_0, timer0_irq_handler);
    hw_set_bits(&timer_hw->inte, (1u << 0));
    irq_set_enabled(TIMER_IRQ_0, true);
    timer_hw->alarm[0] = timer_hw->timerawl + AUDIO_FRAME_TICKS;

    // LED indicator for Core1
    bool            led_state            = false;
    absolute_time_t next_led_toggle_time = make_timeout_time_ms(LED_TIME * 2);

    // Main operation loop on Core1
    while (1) {
        tud_task();    // TinyUSB device task

        tight_loop_contents();
    }
}

// ============================================================================
// MAIN FUNCTION
// ============================================================================

int main() {
    // Initialize system
    set_sys_clock_khz(SYS_FREQ, true);

    // Initialize LEDs
    gpio_init(LED_PIN);
    gpio_set_dir(LED_PIN, GPIO_OUT);

    // Reset and launch Core1
    multicore_reset_core1();
    sleep_ms(100);
    multicore_launch_core1(second_core_main);

    // Start Core0 main loop
    first_core_main();

    return 0;
}
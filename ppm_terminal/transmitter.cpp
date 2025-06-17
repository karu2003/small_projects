#include "common.h"
#include <bsp/board_api.h>
#include <iostream>
#include <string>
#include <tusb.h>

static PIO  pio = pio1;    // Use another PIO to avoid conflicts
static uint sm_gen;

static volatile uint32_t ppm_code_to_send = 0;
static volatile bool     has_custom_value = false;

uint32_t          current_sample_rate = AUDIO_SAMPLE_RATE;
volatile uint32_t audio_frame_ticks;
// volatile  uint32_t audio_frame_ticks = (SYS_FREQ * 1000) / AUDIO_SAMPLE_RATE;

void generate_pulse(uint32_t pause_width, bool verbose) {
    pio_sm_put_blocking(pio, sm_gen, pause_width);
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

        generate_pulse(ppm_value, false);

        timer_hw->alarm[0] = timer_hw->timerawl + audio_frame_ticks;
    }
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

// Function for processing user commands
void process_command(const char *input) {
    char *endptr;
    int   value = strtol(input, &endptr, 10);

    if (endptr != input && value >= 0 && value <= 1024) {

        ppm_code_to_send = static_cast<uint32_t>(value);
        has_custom_value = true;

        char msg[64];
        snprintf(msg, sizeof(msg), "Queued code for transmission: %d\r\n", value);
        if (tud_cdc_connected()) {
            tud_cdc_write_str(msg);
            tud_cdc_write_flush();
        }
    }
    else {
        if (tud_cdc_connected()) {
            tud_cdc_write_str("Please enter a value from 0 to 1024.\r\n");
            tud_cdc_write_flush();
        }
    }
}

void process_received_measurements() {
    while (multicore_fifo_rvalid()) {
        uint32_t measured_width = multicore_fifo_pop_blocking();

        char debug_msg[128];
        snprintf(debug_msg, sizeof(debug_msg), "Width: %u\r\n", measured_width);

        if (tud_cdc_connected()) {
            tud_cdc_write_str(debug_msg);
            tud_cdc_write_flush();
        }
    }
}

uint32_t calculate_audio_frame_ticks() {
    // return (uint32_t)clock_get_hz(clk_sys) / current_sample_rate / 100;
    return 1000000 / current_sample_rate;
}

// Main function of Core1 (user interface + transmission)
void second_core_main() {
    board_init();
    tusb_init();

    if (board_init_after_tusb) {
        board_init_after_tusb();
    }

    // stdio_init_all();

    init_pulse_generator(PIO_FREQ);

    // LED для Core1
    bool            was_connected = false;
    char            input[64];
    size_t          input_pos            = 0;
    static uint8_t  led_state            = 0;
    absolute_time_t next_led_toggle_time = make_timeout_time_ms(LED_TIME * 2);

    audio_frame_ticks = calculate_audio_frame_ticks();

    irq_set_exclusive_handler(TIMER_IRQ_0, timer0_irq_handler);
    hw_set_bits(&timer_hw->inte, (1u << 0));
    irq_set_enabled(TIMER_IRQ_0, true);
    timer_hw->alarm[0] = timer_hw->timerawl + audio_frame_ticks;

    // Main operation loop on Core1
    while (1) {
        tud_task();

        process_received_measurements();

        if (tud_cdc_connected()) {
            if (!was_connected) {
                tud_cdc_write_str("=== PPM Echo System ===\r\n");
                tud_cdc_write_str("Generator Pin: ");
                tud_cdc_write_str(std::to_string(PULSE_GEN_PIN).c_str());
                tud_cdc_write_str("\r\nDetector Pin: ");
                tud_cdc_write_str(std::to_string(PULSE_DET_PIN).c_str());
                tud_cdc_write_str("\r\nClock: ");
                tud_cdc_write_str(std::to_string(clock_get_hz(clk_sys)).c_str());
                tud_cdc_write_str(" Hz\r\n");
                tud_cdc_write_str("Frame Ticks: ");
                tud_cdc_write_str(std::to_string(audio_frame_ticks).c_str());
                tud_cdc_write_str("\r\n");
                tud_cdc_write_str("Sample Rate: ");
                tud_cdc_write_str(std::to_string(current_sample_rate).c_str());
                tud_cdc_write_str(" Hz\r\n");
                tud_cdc_write_str("Enter a value from 0 to 1024 to send via PPM.\r\n");
                tud_cdc_write_flush();
                was_connected = true;
            }

            if (tud_cdc_available()) {
                uint8_t  buf[64];
                uint32_t count = tud_cdc_read(buf, sizeof(buf));
                if (count > 0) {
                    tud_cdc_write(buf, count);    // echo
                    tud_cdc_write_flush();
                    for (uint32_t i = 0; i < count; i++) {
                        char c = static_cast<char>(buf[i]);
                        if (c == '\r' || c == '\n') {
                            if (input_pos > 0) {
                                input[input_pos] = '\0';
                                tud_cdc_write_str("\r\n");
                                process_command(input);
                                input_pos = 0;
                            }
                        }
                        else if (input_pos < sizeof(input) - 1) {
                            input[input_pos++] = c;
                        }
                    }
                }
            }
        }
        else {
            was_connected = false;
        }
        sleep_ms(1);
    }
}
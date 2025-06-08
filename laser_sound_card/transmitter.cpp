#include "common.h"
#include "usb_descriptors.h"
#include <bsp/board_api.h>
#include <iostream>
#include <string>
#include <tusb.h>

static PIO  pio = pio1;    // Use another PIO to avoid conflicts
static uint sm_gen;

volatile uint32_t ppm_code_to_send = 0;
volatile bool     has_custom_value = false;

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

        // Schedule next interrupt
        timer_hw->alarm[0] = timer_hw->timerawl + AUDIO_FRAME_TICKS;
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

// Main function of Core1 (audio transmission)
void second_core_main() {
    board_init();
    tusb_init();

    // init device stack on configured roothub port
    tusb_rhport_init_t dev_init = {
        .role  = TUSB_ROLE_DEVICE,
        .speed = TUSB_SPEED_AUTO};
    tusb_init(BOARD_TUD_RHPORT, &dev_init);

    if (board_init_after_tusb) {
        board_init_after_tusb();
    }

    TU_LOG1("Laser Audio running\r\n");

    init_pulse_generator(PIO_FREQ);

    // LED для Core1
    bool            led_state            = false;
    absolute_time_t next_led_toggle_time = make_timeout_time_ms(LED_TIME * 2);

    // Setup timer interrupt for audio sampling
    irq_set_exclusive_handler(TIMER_IRQ_0, timer0_irq_handler);
    hw_set_bits(&timer_hw->inte, (1u << 0));
    irq_set_enabled(TIMER_IRQ_0, true);
    timer_hw->alarm[0] = timer_hw->timerawl + AUDIO_FRAME_TICKS;

    // Main operation loop on Core1
    while (1) {
        tud_task();    // TinyUSB device task

        // Обновление LED индикатора
        if (absolute_time_diff_us(get_absolute_time(), next_led_toggle_time) <= 0) {
            led_state = !led_state;
            gpio_put(LED_PIN, led_state);
            next_led_toggle_time = make_timeout_time_ms(LED_TIME * 2);
        }

        sleep_ms(1);
    }
}
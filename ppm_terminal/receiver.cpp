#include "common.h"
#include <pico/stdlib.h>

static PIO           pio = pio0;
static uint          sm_det;
static volatile bool detector_running = false;

void update_measurements() {

    while (detector_running && !pio_sm_is_rx_fifo_empty(pio, sm_det)) {
        uint32_t measured_width = pio_sm_get(pio, sm_det);

        uint32_t corrected_width = (measured_width + MIN_TACKT) - MIN_INTERVAL_CYCLES;

        if (corrected_width > 0) {
            if (multicore_fifo_wready()) {
                multicore_fifo_push_blocking(corrected_width);
            }
        }
    }
}

// Initialize PIO for pulse detector
void init_pulse_detector(float freq) {
    sm_det               = pio_claim_unused_sm(pio, true);
    uint          offset = pio_add_program(pio, &pulse_detector_program);
    pio_sm_config c      = pulse_detector_program_get_default_config(offset);

    sm_config_set_in_pins(&c, PULSE_DET_PIN);
    sm_config_set_jmp_pin(&c, PULSE_DET_PIN);
    pio_gpio_init(pio, PULSE_DET_PIN);
    pio_sm_set_consecutive_pindirs(pio, sm_det, PULSE_DET_PIN, 1, false);

    sm_config_set_clkdiv(&c, (float)clock_get_hz(clk_sys) / freq);
    pio_sm_init(pio, sm_det, offset, &c);
}

void start_detector() {
    pio_sm_clear_fifos(pio, sm_det);
    pio_sm_set_enabled(pio, sm_det, true);
    detector_running = true;
}

// Main function for Core0 (receiver)
void first_core_main() {
    init_pulse_detector(PIO_FREQ);
    start_detector();

    bool led_state = false;

    absolute_time_t next_led_toggle = make_timeout_time_ms(LED_TIME);

    while (1) {

        update_measurements();

        if (absolute_time_diff_us(get_absolute_time(), next_led_toggle) <= 0) {
            led_state = !led_state;
            gpio_put(LED_PIN, led_state);
            next_led_toggle = make_timeout_time_ms(LED_TIME);
        }
    }
}

int main() {
    set_sys_clock_khz(SYS_FREQ, true);
    gpio_init(LED_PIN);
    gpio_set_dir(LED_PIN, GPIO_OUT);

    multicore_reset_core1();
    sleep_ms(100);
    multicore_launch_core1(second_core_main);

    first_core_main();
    return 0;
}
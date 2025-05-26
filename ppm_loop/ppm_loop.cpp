#include "hardware/clocks.h"
#include "hardware/pio.h"
#include "pico/stdlib.h"
#include "pico/time.h"
#include <bsp/board_api.h>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <tusb.h>

// Include generated header files with PIO programs
#include "ppm.pio.h"

#define PULSE_GEN_PIN 0
#define PULSE_DET_PIN 1
#define LED_PIN 25

static PIO pio = pio0;
static uint sm_gen, sm_det;

#define LED_TIME 500
#define SYS_FREQ 133000
// #define SYS_FREQ 250000

// Initialize PIO for pulse generator
void init_pulse_generator() {
  sm_gen = pio_claim_unused_sm(pio, true);

  // Load program
  uint offset = pio_add_program(pio, &pulse_generator_program);

  // Configure state machine
  pio_sm_config c = pulse_generator_program_get_default_config(offset);

  // Setup pins for PIO
  sm_config_set_set_pins(&c, PULSE_GEN_PIN,
                         1);         // 1 pin starting from PULSE_GEN_PIN
  pio_gpio_init(pio, PULSE_GEN_PIN); // Only initialization for PIO
  pio_sm_set_consecutive_pindirs(pio, sm_gen, PULSE_GEN_PIN, 1,
                                 true); // Set pin direction to output

  // Frequency divider (default 1.0)
  sm_config_set_clkdiv(&c, 1.0f);

  // Initialize state machine
  pio_sm_init(pio, sm_gen, offset, &c);
}

// Initialize PIO for pulse detector
void init_pulse_detector() {
  sm_det = pio_claim_unused_sm(pio, true);

  // Load program
  uint offset = pio_add_program(pio, &pulse_detector_program);

  // Configure state machine
  pio_sm_config c = pulse_detector_program_get_default_config(offset);

  // Setup pins
  sm_config_set_in_pins(&c, PULSE_DET_PIN);
  sm_config_set_jmp_pin(&c, PULSE_DET_PIN);
  pio_gpio_init(pio, PULSE_DET_PIN);
  pio_sm_set_consecutive_pindirs(pio, sm_det, PULSE_DET_PIN, 1, false);

  // Initialize state machine
  pio_sm_init(pio, sm_det, offset, &c);
}

// Function for generating and measuring pause between pulses
uint32_t test_pulse(uint32_t pause_width, bool verbose = true) {
  // Clear FIFOs
  pio_sm_clear_fifos(pio, sm_gen);
  pio_sm_clear_fifos(pio, sm_det);

  // Debug output of pin states before test
  if (verbose) {
    tud_cdc_write_str("Starting test with pause: ");
    tud_cdc_write_str(std::to_string(pause_width).c_str());
    tud_cdc_write_str(" cycles\r\n");

    // Check pin state before sending pulse
    bool pin_state_before = gpio_get(PULSE_GEN_PIN);
    tud_cdc_write_str("Generator pin state before test: ");
    tud_cdc_write_str(pin_state_before ? "HIGH\r\n" : "LOW\r\n");
    tud_cdc_write_flush();
  }

  // Force set initial state
  gpio_put(PULSE_GEN_PIN, 0);
  sleep_us(10);

  // Start detector (must start first)
  pio_sm_set_enabled(pio, sm_det, true);
  sleep_us(1); // Small delay

  // Start generator
  pio_sm_set_enabled(pio, sm_gen, true);

  // Send pause duration to generator
  pio_sm_put_blocking(pio, sm_gen, pause_width);

  // Debug output
  if (verbose) {
    tud_cdc_write_str("Pause sent to PIO\r\n");

    // Short wait to check pin state
    sleep_us(10);
    bool pin_state_during = gpio_get(PULSE_GEN_PIN);
    tud_cdc_write_str("Generator pin state during pause: ");
    tud_cdc_write_str(pin_state_during ? "HIGH\r\n" : "LOW\r\n");
    tud_cdc_write_flush();
  } else {
    sleep_us(10);
  }

  // Wait for result from detector
  uint32_t measured_width = 0;

  // Small wait for operation to complete
  sleep_ms(1);

  if (verbose) {
    bool pin_state_after = gpio_get(PULSE_GEN_PIN);
    tud_cdc_write_str("Generator pin state after pause: ");
    tud_cdc_write_str(pin_state_after ? "HIGH\r\n" : "LOW\r\n");
  }

  if (!pio_sm_is_rx_fifo_empty(pio, sm_det)) {
    measured_width = pio_sm_get_blocking(pio, sm_det);
    if (verbose) {
      tud_cdc_write_str("Measured pause: ");
      tud_cdc_write_str(std::to_string(measured_width).c_str());
      tud_cdc_write_str(" cycles\r\n");
    }
  } else if (verbose) {
    tud_cdc_write_str("Detector FIFO is empty, pause not detected\r\n");
  }
  
  if (verbose) {
    tud_cdc_write_flush();
  }

  // Stop state machines
  pio_sm_set_enabled(pio, sm_gen, false);
  pio_sm_set_enabled(pio, sm_det, false);

  // Reset state machines to initial state
  pio_sm_restart(pio, sm_gen);
  pio_sm_restart(pio, sm_det);

  return measured_width;
}

void process_command(const char *input) {
  if (input[0] == 'T' || input[0] == 't') {
    printf("\n===== Starting pause duration tests (0-1500 cycles) =====\n\n");
    printf("| %8s | %8s | %10s |\n", "Expected", "Measured", "Difference");
    printf("|----------|----------|------------|\n");
    
    int discrepancyCount = 0;
    
    // Test all values from 0 to 1500
    for (uint32_t width = 0; width <= 1500; width++) {
      uint32_t measured = test_pulse(width, false) - 2;  // Disable debug output during tests
      int32_t diff = (int32_t)measured - (int32_t)width;
      
      // Only output values that don't match expectations
      if (diff != 0) {
        printf("| %8d | %8d | %+10d |\n", width, measured, diff);
        discrepancyCount++;
      }
      
      // Display progress every 100 values
      if (width % 100 == 0 && width > 0) {
        printf("Progress: %d/1500 (%.1f%%)\n", width, (width / 1500.0) * 100);
      }
    }
    
    if (discrepancyCount == 0) {
      printf("| All values match expectations! No discrepancies found. |\n");
    } else {
      printf("\nFound %d values with discrepancies\n", discrepancyCount);
    }
    
    printf("\n=========== Test completed ===========\n");
  } else {
    char *endptr;
    int width = strtol(input, &endptr, 10);
    if (endptr != input && width >= 0 && width <= 1500) {
      printf("\n--- Single test with pause: %d cycles ---\n", width);
      uint32_t measured = test_pulse(width, true);  // Enable detailed output for single test
      printf("Set pause: %-3d | Measured pause: %-3d cycles\n\n", width, measured);
    } else {
      printf("Please enter a value from 0 to 1500, or 'T' to run all tests.\n");
    }
  }
}

int main() {
  set_sys_clock_khz(SYS_FREQ, true);
  board_init();
  tusb_init();
  static uint8_t led_state = 0;
  absolute_time_t next_led_toggle_time = make_timeout_time_ms(LED_TIME);

  stdio_init_all();
  gpio_init(LED_PIN);
  gpio_set_dir(LED_PIN, GPIO_OUT);

  if (board_init_after_tusb) {
    board_init_after_tusb();
  }

  bool was_connected = false;

  init_pulse_generator();
  init_pulse_detector();

  char input[64];
  size_t input_pos = 0;
  while (1) {
    tud_task();
    if (absolute_time_diff_us(get_absolute_time(), next_led_toggle_time) <= 0) {
      led_state = !led_state;
      gpio_put(LED_PIN, led_state);
      next_led_toggle_time = make_timeout_time_ms(LED_TIME);
    }
    if (tud_cdc_connected()) {
      if (!was_connected) {
        tud_cdc_write_str("=== PIO Wait Command Test ===\r\n");
        tud_cdc_write_str("Generator Pin: ");
        tud_cdc_write_str(std::to_string(PULSE_GEN_PIN).c_str());
        tud_cdc_write_str("\r\nDetector Pin: ");
        tud_cdc_write_str(std::to_string(PULSE_DET_PIN).c_str());
        tud_cdc_write_str("\r\nClock frequency: ");
        tud_cdc_write_str(std::to_string(clock_get_hz(clk_sys)).c_str());
        tud_cdc_write_str(" Hz\r\n");
        tud_cdc_write_flush();
        was_connected = true;
      }
      if (tud_cdc_available()) {
        uint8_t buf[64];
        uint32_t count = tud_cdc_read(buf, sizeof(buf));
        if (count > 0) {
          tud_cdc_write(buf, count); // echo
          tud_cdc_write_flush();
          for (uint32_t i = 0; i < count; i++) {
            char c = static_cast<char>(buf[i]);
            if (c == '\r' || c == '\n') {
              if (input_pos > 0) {
                input[input_pos] = '\0';
                tud_cdc_write_str("\r\n");
                tud_cdc_write_str("You entered: ");
                tud_cdc_write_str(input);
                tud_cdc_write_str("\r\n");
                tud_cdc_write_flush();
                process_command(input);
                input_pos = 0;
              }
            } else if (input_pos < sizeof(input) - 1) {
              input[input_pos++] = c;
            }
          }
        }
      }
    } else {
      was_connected = false;
    }
    sleep_ms(10);
  }
  return 0;
}
#include "common.h"
#include <bsp/board_api.h>
#include <iostream>
#include <string>
#include <tusb.h>

static PIO pio = pio1; // Use another PIO to avoid conflicts
static uint sm_gen;

// Initialize PIO for pulse generator
void init_pulse_generator() {
  sm_gen = pio_claim_unused_sm(pio, true);

  // Load program
  uint offset = pio_add_program(pio, &pulse_generator_program);

  // Configure state machine
  pio_sm_config c = pulse_generator_program_get_default_config(offset);

  // Setup pins for PIO
  sm_config_set_set_pins(&c, PULSE_GEN_PIN, 1);
  pio_gpio_init(pio, PULSE_GEN_PIN);
  pio_sm_set_consecutive_pindirs(pio, sm_gen, PULSE_GEN_PIN, 1, true);

  // Frequency divider (default 1.0)
  sm_config_set_clkdiv(&c, 1.0f);

  // Initialize state machine
  pio_sm_init(pio, sm_gen, offset, &c);
}

// Function for pulse generation
void generate_pulse(uint32_t pause_width, bool verbose) {
  // Clear FIFOs
  pio_sm_clear_fifos(pio, sm_gen);

  // Force set initial state
  gpio_put(PULSE_GEN_PIN, 0);

  // Start generator
  pio_sm_set_enabled(pio, sm_gen, true);

  // Send pause duration to generator
  pio_sm_put_blocking(pio, sm_gen, pause_width);

  if (verbose) {
    tud_cdc_write_str("Pulse generated with pause width: ");
    tud_cdc_write_str(std::to_string(pause_width).c_str());
    tud_cdc_write_str(" cycles\r\n");
    tud_cdc_write_flush();
  }

  //   Give time for pulse generation
  sleep_ms(1);

  // Stop state machine
  pio_sm_set_enabled(pio, sm_gen, false);
  pio_sm_restart(pio, sm_gen);
}

// Function to request the last measurement from Core0
core_result_t get_last_measurement() {
  // Use static variable for command
  static core_command_t cmd = {CMD_READ_MEASUREMENT, 0, false};

  // Send command to Core0
  multicore_fifo_push_blocking((uint32_t)&cmd);

  // Add timeout to prevent hanging
  absolute_time_t timeout = make_timeout_time_ms(100);

  // Wait for response with timeout
  static core_result_t empty_result = {0, false, 0};

  while (!multicore_fifo_rvalid()) {
    if (absolute_time_diff_us(get_absolute_time(), timeout) <= 0) {
      printf("Timeout waiting for measurement\n");
      return empty_result;
    }
    sleep_ms(1);
  }

  // Get response from Core0
  core_result_t *result = (core_result_t *)multicore_fifo_pop_blocking();

  // Copy result to static variable
  static core_result_t safe_result;
  memcpy(&safe_result, result, sizeof(core_result_t));

  return safe_result;
}

// Function for processing user commands
void process_command(const char *input) {
  if (input[0] == 'T' || input[0] == 't') {
    printf("\n===== Starting pause duration tests (%d-1500 cycles) =====\n\n",
           MIN_TACKT);
    printf("Note: Values from 0 to %d are not measured due to hardware "
           "limitations.\n\n",
           MIN_TACKT - 1);
    printf("| %8s | %8s | %10s |\n", "Expected", "Measured", "Difference");
    printf("|----------|----------|------------|\n");

    int discrepancyCount = 0;

    // Test all values from 0 to 1500
    for (uint32_t width = MIN_TACKT; width <= 1500; width++) {
      // Генерируем импульс с заданной шириной
      generate_pulse(width, false);

      core_result_t result = get_last_measurement();

      if (result.success) {
        uint32_t measured = result.measured_width + MIN_TACKT;
        int32_t diff = (int32_t)measured - (int32_t)width;

        // Only output values that don't match expectations
        if (diff != 0) {
          printf("| %8d | %8d | %+10d |\n", width, measured, diff);
          discrepancyCount++;
        }
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

      generate_pulse(static_cast<uint32_t>(width), true);

      core_result_t result = get_last_measurement();

      if (result.success) {
        uint32_t measured = result.measured_width + MIN_TACKT;
        printf("Set pause: %-3d | Measured pause: %-3d cycles\n\n", width,
               measured);
      } else {
        printf("Measurement failed\n\n");
      }
    } else {
      printf("Please enter a value from 0 to 1500, or 'T' to run all tests.\n");
    }
  }
}

// Main function of Core1 (user interface + transmission)
void second_core_main() {
  // Required initializations for user interface
  board_init();
  tusb_init();

  if (board_init_after_tusb) {
    board_init_after_tusb();
  }

  // Initialize generator on Core1
  init_pulse_generator();

  // Variables for interface operation
  bool was_connected = false;
  char input[64];
  size_t input_pos = 0;

  // LED for Core1 operation indication
  static uint8_t led_state = 0;
  absolute_time_t next_led_toggle_time = make_timeout_time_ms(
      LED_TIME * 2); // Different blinking period to distinguish from Core0

  // Main operation loop on Core1
  while (1) {
    tud_task();

    // LED control for operation indication
    if (absolute_time_diff_us(get_absolute_time(), next_led_toggle_time) <= 0) {
      led_state = !led_state;
      gpio_put(LED_PIN + 1, led_state); // Use another LED
      next_led_toggle_time = make_timeout_time_ms(LED_TIME * 2);
    }

    if (tud_cdc_connected()) {
      if (!was_connected) {
        tud_cdc_write_str("=== PIO Wait Command Test (Multicore) ===\r\n");
        tud_cdc_write_str("Generator Pin: ");
        tud_cdc_write_str(std::to_string(PULSE_GEN_PIN).c_str());
        tud_cdc_write_str("\r\nDetector Pin: ");
        tud_cdc_write_str(std::to_string(PULSE_DET_PIN).c_str());
        tud_cdc_write_str("\r\nClock frequency: ");
        tud_cdc_write_str(std::to_string(clock_get_hz(clk_sys)).c_str());
        tud_cdc_write_str(" Hz\r\n");
        tud_cdc_write_str(
            "Core0: Receiver (always running), Core1: Transmitter + UI\r\n");
        tud_cdc_write_str("Enter a value from 0 to 1500 for pulse width, or "
                          "'T' to test all values.\r\n");
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
}
#include "common.h"
#include <pico/stdlib.h>
#include <cstring>  // For memcpy function

static PIO pio = pio0;
static uint sm_det;
static volatile bool detector_running = false;
static volatile core_result_t last_measurement = {0, false, 0};
static volatile bool new_measurement_available = false;

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

// Function to start the detector in continuous reception mode
void start_detector() {
  // Clear FIFOs
  pio_sm_clear_fifos(pio, sm_det);
  
  // Start detector
  pio_sm_set_enabled(pio, sm_det, true);
  detector_running = true;
}

// Function for checking and updating measurement data
void update_measurements() {
  if (detector_running && !pio_sm_is_rx_fifo_empty(pio, sm_det)) {
    // Get new measurement
    uint32_t measured_width = pio_sm_get(pio, sm_det);
    
    // Update measurement data
    last_measurement.measured_width = measured_width;
    last_measurement.success = true;
    last_measurement.timestamp = to_ms_since_boot(get_absolute_time());
    
    // Set flag for new measurement availability
    new_measurement_available = true;
  }
}

// Process commands from Core1
void process_core1_command() {
  if (multicore_fifo_rvalid()) {
    uint32_t cmd_ptr = multicore_fifo_pop_blocking();
    core_command_t* cmd = (core_command_t*)cmd_ptr;
    
    switch (cmd->command) {
      case CMD_READ_MEASUREMENT: {
        // Create a static copy of the last measurement
        static core_result_t result_copy;
        memcpy(&result_copy, (const void*)&last_measurement, sizeof(core_result_t));
        
        // Reset the new measurement flag
        new_measurement_available = false;
        
        // Send the result back to Core1
        multicore_fifo_push_blocking((uint32_t)&result_copy);
        break;
      }
      
      default:
        // Ignore unknown commands
        break;
    }
  }
}

// Main function for Core0 (receiver)
void first_core_main() {
  // Initialize detector
  init_pulse_detector();
  
  // Start detector in continuous reception mode
  start_detector();
  
  // Variables for LED control
  bool led_state = false;
  absolute_time_t next_led_toggle = make_timeout_time_ms(LED_TIME);
  
  // Main operation loop
  while (1) {
    // Check for new measurements
    update_measurements();
    
    // Process commands from Core1
    process_core1_command();
    
    // LED blinking for operation indication
    if (absolute_time_diff_us(get_absolute_time(), next_led_toggle) <= 0) {
      led_state = !led_state;
      gpio_put(LED_PIN, led_state);
      next_led_toggle = make_timeout_time_ms(LED_TIME);
    }
    
    // Small delay to reduce load
    // sleep_us(10);
  }
}

int main() {
  // Set clock frequency
  set_sys_clock_khz(SYS_FREQ, true);
  
  // Initialize standard components
  stdio_init_all();
  
  // LED for Core0 operation indication
  gpio_init(LED_PIN);
  gpio_set_dir(LED_PIN, GPIO_OUT);

  // Reset Core1 before starting for reliability
  multicore_reset_core1();
  sleep_ms(100);  // Give time for reset
  
  // Start Core1 (user interface and transmission)
  multicore_launch_core1(second_core_main);
  
  // Start Core0 (continuous reception)
  first_core_main();
  
  return 0;
}
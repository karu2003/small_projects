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

// Подключаем сгенерированные заголовочные файлы с PIO программами
#include "wait.pio.h"

#define PULSE_GEN_PIN 0
#define PULSE_DET_PIN 1
#define LED_PIN 25

static PIO pio = pio0;
static uint sm_gen, sm_det;

#define LED_TIME 500
#define SYS_FREQ 133000
// #define SYS_FREQ 250000

// Инициализация PIO для генератора импульсов
void init_pulse_generator() {
  sm_gen = pio_claim_unused_sm(pio, true);

  // Загрузка программы
  uint offset = pio_add_program(pio, &pulse_generator_program);

  // Конфигурация state machine
  pio_sm_config c = pulse_generator_program_get_default_config(offset);

  // Настройка пинов для PIO
  sm_config_set_set_pins(&c, PULSE_GEN_PIN, 1); // 1 пин начиная с PULSE_GEN_PIN
  pio_gpio_init(pio, PULSE_GEN_PIN); // Только инициализация для PIO
  pio_sm_set_consecutive_pindirs(pio, sm_gen, PULSE_GEN_PIN, 1,
                                 true); // Направление пина на выход

  // Делитель частоты (по умолчанию 1.0)
  sm_config_set_clkdiv(&c, 1.0f);

  // Инициализация state machine
  pio_sm_init(pio, sm_gen, offset, &c);
}

// Инициализация PIO для детектора импульсов
void init_pulse_detector() {
  sm_det = pio_claim_unused_sm(pio, true);

  // Загрузка программы
  uint offset = pio_add_program(pio, &pulse_detector_program);

  // Конфигурация state machine
  pio_sm_config c = pulse_detector_program_get_default_config(offset);

  // Настройка пинов
  sm_config_set_in_pins(&c, PULSE_DET_PIN);
  sm_config_set_jmp_pin(&c, PULSE_DET_PIN);
  pio_gpio_init(pio, PULSE_DET_PIN);
  pio_sm_set_consecutive_pindirs(pio, sm_det, PULSE_DET_PIN, 1, false);

  // Инициализация state machine
  pio_sm_init(pio, sm_det, offset, &c);
}

// Функция для генерации и измерения импульса
uint32_t test_pulse(uint32_t pulse_width) {
  // Очистка FIFO
  pio_sm_clear_fifos(pio, sm_gen);
  pio_sm_clear_fifos(pio, sm_det);

  // Отладочный вывод состояния пинов перед тестом
  tud_cdc_write_str("Начало теста с шириной: ");
  tud_cdc_write_str(std::to_string(pulse_width).c_str());
  tud_cdc_write_str(" циклов\r\n");

  // Проверка состояния пина перед отправкой импульса
  bool pin_state_before = gpio_get(PULSE_GEN_PIN);
  tud_cdc_write_str("Состояние пина генератора перед тестом: ");
  tud_cdc_write_str(pin_state_before ? "HIGH\r\n" : "LOW\r\n");
  tud_cdc_write_flush();

  // Принудительно устанавливаем начальное состояние
  gpio_put(PULSE_GEN_PIN, 0);
  sleep_us(10);

  // Запуск детектора (должен запуститься первым)
  pio_sm_set_enabled(pio, sm_det, true);
  sleep_us(1); // Небольшая задержка

  // Запуск генератора
  pio_sm_set_enabled(pio, sm_gen, true);

  // Отправка длительности импульса генератору
  pio_sm_put_blocking(pio, sm_gen, pulse_width);

  // Отладочный вывод
  tud_cdc_write_str("Импульс отправлен в PIO\r\n");

  // Короткое ожидание, чтобы успеть проверить состояние пина
  sleep_us(10);
  bool pin_state_during = gpio_get(PULSE_GEN_PIN);
  tud_cdc_write_str("Состояние пина генератора во время импульса: ");
  tud_cdc_write_str(pin_state_during ? "HIGH\r\n" : "LOW\r\n");
  tud_cdc_write_flush();

  // Ожидание результата от детектора
  uint32_t measured_width = 0;

  // Небольшое ожидание для завершения операции
  sleep_ms(1);

  bool pin_state_after = gpio_get(PULSE_GEN_PIN);
  tud_cdc_write_str("Состояние пина генератора после импульса: ");
  tud_cdc_write_str(pin_state_after ? "HIGH\r\n" : "LOW\r\n");

  if (!pio_sm_is_rx_fifo_empty(pio, sm_det)) {
    measured_width = pio_sm_get_blocking(pio, sm_det);
    tud_cdc_write_str("Измерено: ");
    tud_cdc_write_str(std::to_string(measured_width).c_str());
    tud_cdc_write_str(" циклов\r\n");
  } else {
    tud_cdc_write_str("FIFO детектора пуст, сигнал не обнаружен\r\n");
  }
  tud_cdc_write_flush();

  // Остановка state machines
  pio_sm_set_enabled(pio, sm_gen, false);
  pio_sm_set_enabled(pio, sm_det, false);

  // Сброс state machines к начальному состоянию
  pio_sm_restart(pio, sm_gen);
  pio_sm_restart(pio, sm_det);

  return measured_width;
}

void process_command(const char *input) {
  if (input[0] == 'T' || input[0] == 't') {
    printf("\nStarting pulse width tests (0-32 cycles):\n");
    printf("Expected | Measured | Difference\n");
    printf("---------|----------|----------\n");
    for (uint32_t width = 0; width <= 32; width++) {
      uint32_t measured = test_pulse(width);
      int32_t diff = (int32_t)measured - (int32_t)width;
      printf("%8d | %8d | %+9d\n", width, measured, diff);
    }
    printf("\n=== Test completed ===\n");
  } else {
    char *endptr;
    int width = strtol(input, &endptr, 10);
    if (endptr != input && width >= 0 && width <= 32) {
      uint32_t measured = test_pulse(width);
      printf("Width: %d -> Measured: %d cycles\n", width, measured);
    } else {
      printf(
          "Please enter a value between 0 and 32, or 'T' to run all tests.\n");
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
          tud_cdc_write(buf, count); // эхо
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
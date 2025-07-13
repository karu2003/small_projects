#include "common.h"
#include "pico/sem.h"
#include <pico/stdlib.h>

static PIO           pio = pio0;
static uint          sm_det;
static volatile bool detector_running = false;

// extern statistics_t statistics;

// void update_measurements() {
//     static uint16_t buffer_pos       = 0;
//     static uint8_t  current_buffer   = 0;
//     static bool     buffer_acquired  = false;
//     //static uint32_t last_packet_size = 192;    // Начальный размер по умолчанию

//     // Получаем текущий размер пакета от первого ядра
//     // if (sem_initialized) {
//     //     // Храним актуальный размер пакета для межъядерного обмена
//     //     last_packet_size = shared_ppm_data.packet_size > 0 ? shared_ppm_data.packet_size : last_packet_size;
//     // }

//     while (detector_running && !pio_sm_is_rx_fifo_empty(pio, sm_det)) {
//         uint32_t measured_width  = pio_sm_get(pio, sm_det);
//         uint16_t corrected_width = (uint16_t)(measured_width + MIN_TACKT) - MIN_INTERVAL_CYCLES;
//         statistics.total_received++;

//         if (corrected_width > 0 && corrected_width <= MAX_CODE) {
//             statistics.total_ppm_received++;
//             statistics.total_summed_ppm_in += corrected_width;
//             // Если система семафоров инициализирована
//             if (sem_initialized) {
//                 // Если буфер ещё не получен - пробуем получить пустой буфер
//                 if (!buffer_acquired) {
//                     if (sem_try_acquire(&shared_ppm_data.sem_empty)) {
//                         // Получен пустой буфер
//                         current_buffer  = shared_ppm_data.write_index;
//                         buffer_pos      = 0;
//                         buffer_acquired = true;
//                     }
//                 }

//                 // Записываем данные, если буфер доступен
//                 if (buffer_acquired) {
//                     shared_ppm_data.buffer[current_buffer][buffer_pos++] = corrected_width;

//                     // Если буфер заполнен до размера пакета, уведомляем другое ядро
//                     if (/*buffer_pos >= last_packet_size / 4 ||*/ buffer_pos >= 48) {    // Делим на 4 для стерео 16 бит
//                         shared_ppm_data.size[current_buffer] = buffer_pos;
//                         shared_ppm_data.write_index          = (uint8_t)((current_buffer + 1) % 2);
//                         sem_release(&shared_ppm_data.sem_full);
//                         buffer_acquired = false;
//                     }
//                 }
//             }
//             // Иначе используем старый механизм FIFO
//             else if (multicore_fifo_wready()) {
//                 multicore_fifo_push_blocking(corrected_width);
//             }
//         }
//     }

//     // Если буфер частично заполнен и давно не было данных,
//     // отправляем то что есть (по таймауту)
//     if (buffer_acquired && buffer_pos > 0) {
//         static uint32_t last_update_time = 0;
//         uint32_t        current_time     = time_us_32();

//         // Исправление во второй части функции (по таймауту)
//         if (current_time - last_update_time > 5000) {    // 5 мс
//             shared_ppm_data.size[current_buffer] = buffer_pos;
//             shared_ppm_data.write_index          = (uint8_t)((current_buffer + 1) % 2);
//             sem_release(&shared_ppm_data.sem_full);
//             buffer_acquired  = false;
//             last_update_time = current_time;
//         }
//     }
// }

void update_measurements() {
    while (detector_running && !pio_sm_is_rx_fifo_empty(pio, sm_det)) {
        uint32_t measured_width  = pio_sm_get(pio, sm_det);
        uint32_t corrected_width = (measured_width + MIN_TACKT) - MIN_INTERVAL_CYCLES;

        if (corrected_width > 0 && corrected_width <= MAX_CODE) {
            if (multicore_fifo_wready()) {
                multicore_fifo_push_blocking(corrected_width);
            }
        }
    }
}

// Initialize PIO for pulse detector
void init_pulse_detector(float freq) {
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wsign-conversion"
    sm_det      = pio_claim_unused_sm(pio, true);
    uint offset = pio_add_program(pio, &pulse_detector_program);
#pragma GCC diagnostic pop
    pio_sm_config c = pulse_detector_program_get_default_config(offset);

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

void second_core_main() {
    init_pulse_detector(PIO_FREQ);
    start_detector();

    while (1) {
        update_measurements();
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
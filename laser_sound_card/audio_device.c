#include "tusb.h"
#include "common.h"
#include "hardware/timer.h"
#include <string.h>

// Аудиобуферы для входа и выхода
int16_t audio_in_buffer[CFG_TUD_AUDIO_FUNC_1_EP_IN_SW_BUF_SZ / 2];
int16_t audio_out_buffer[CFG_TUD_AUDIO_FUNC_1_EP_OUT_SW_BUF_SZ / 2];

// Буфер для текущих и предыдущих аудиоданных
volatile uint16_t current_audio_out_sample = 0;
volatile uint16_t current_audio_in_sample = 0;

// Преобразование 16-битного аудио в 10-битное значение для PPM
uint32_t audio_to_ppm(int16_t audio_sample) {
    // Нормализация 16-битного значения до диапазона 0-1023 (10 бит)
    return ((uint32_t)(audio_sample + 32768) >> 6) & 0x3FF;
}

// Преобразование 10-битного PPM обратно в 16-битное аудио
int16_t ppm_to_audio(uint32_t ppm_value) {
    // Подразумевается, что ppm_value находится в диапазоне 0-1023
    return ((int16_t)(ppm_value << 6)) - 32768;
}

// Получение данных из PCM аудио буфера из USB хоста
void tud_audio_rx_cb(uint8_t rhport, uint8_t *buffer, uint16_t buf_size) {
    (void)rhport;
    
    // Копируем данные в наш буфер
    memcpy(audio_out_buffer, buffer, buf_size);
    
    // Получаем текущий образец и преобразуем в PPM
    int16_t sample = audio_out_buffer[0]; // Берем первый сэмпл для демонстрации
    uint32_t ppm_value = audio_to_ppm(sample);
    
    // Устанавливаем значение для передачи через PPM
    extern volatile uint32_t ppm_code_to_send;
    extern volatile bool has_custom_value;
    
    ppm_code_to_send = ppm_value;
    has_custom_value = true;
}

// Запрос данных для передачи аудио с микрофона на хост
bool tud_audio_tx_cb(uint8_t rhport, uint8_t itf, uint8_t ep_in, uint8_t ctrl_tag, void *buffer, uint16_t buf_size) {
    (void)rhport;
    (void)itf;
    (void)ep_in;
    (void)ctrl_tag;
    
    // Получаем последнее значение PPM и преобразуем в аудио
    extern volatile uint32_t last_ppm_received;
    
    int16_t audio_sample = ppm_to_audio(last_ppm_received);
    
    // Заполняем буфер одинаковыми значениями для простоты
    for (int i = 0; i < buf_size/sizeof(int16_t); i++) {
        ((int16_t*)buffer)[i] = audio_sample;
    }
    
    return true;
}

// Обработка управляющих запросов для аудиоустройства
bool tud_audio_set_req_cb(uint8_t rhport, uint8_t const *p_request) {
    (void)rhport;
    (void)p_request;
    // Добавить обработку управляющих запросов, если необходимо
    return true;
}

bool tud_audio_get_req_cb(uint8_t rhport, uint8_t const *p_request) {
    (void)rhport;
    (void)p_request;
    // Добавить обработку запросов на получение информации
    return true;
}
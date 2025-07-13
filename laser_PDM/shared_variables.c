#include "common.h"

// Глобальная структура, доступная обоим ядрам
core_shared_buffer_t shared_ppm_data __attribute__((section(".scratch_x")));

// Флаг, указывающий, что обмен через семафоры инициализирован
volatile bool sem_initialized = false;
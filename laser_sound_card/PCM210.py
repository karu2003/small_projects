import numpy as np
import matplotlib.pyplot as plt

def generate_triangle_wave(sample_rate, duration, frequency, bit_depth=16):
    """
    Генерация треугольного сигнала
    
    Parameters:
    sample_rate: частота дискретизации
    duration: длительность в секундах
    frequency: частота треугольного сигнала
    bit_depth: разрядность (16 или 24 бит)
    """
    t = np.linspace(0, duration, int(sample_rate * duration), False)
    
    # Генерируем треугольную волну от 0 до максимума
    triangle = 2 * np.abs(2 * (t * frequency - np.floor(t * frequency + 0.5))) - 1
    
    # Сдвигаем и масштабируем от 0 до максимального значения
    if bit_depth == 16:
        max_val = 32767  # 2^15 - 1
        triangle_scaled = ((triangle + 1) / 2 * max_val).astype(np.int16)
    elif bit_depth == 24:
        max_val = 8388607  # 2^23 - 1
        triangle_scaled = ((triangle + 1) / 2 * max_val).astype(np.int32)
    
    return triangle_scaled, t

def truncate_to_10bit(samples, source_bits):
    """
    Простое усечение в 10-бит
    """
    if source_bits == 16:
        return samples >> 6
    elif source_bits == 24:
        return samples >> 14

def round_to_10bit(samples, source_bits):
    """
    Округление в 10-бит
    """
    if source_bits == 16:
        return (samples + 32) >> 6
    elif source_bits == 24:
        return (samples + 8192) >> 14

def dither_to_10bit(samples, source_bits):
    """
    Конвертация с дизерингом в 10-бит
    """
    shift = source_bits - 10
    dither_amplitude = 1 << (shift - 1)
    
    # Треугольный дизер (более качественный чем прямоугольный)
    dither_noise = np.random.triangular(-dither_amplitude, 0, dither_amplitude, len(samples))
    
    dithered = samples + dither_noise.astype(samples.dtype)
    return dithered >> shift

def convert_and_compare(samples, source_bits, time_array):
    """
    Применяет все методы конвертации и выводит сравнение
    """
    methods = {
        'Исходный': samples,
        'Усечение': truncate_to_10bit(samples, source_bits),
        'Округление': round_to_10bit(samples, source_bits),
        'Дизеринг': dither_to_10bit(samples, source_bits)
    }
    
    # Ограничиваем 10-битные значения диапазоном 0-1023 (для положительных значений)
    for key in ['Усечение', 'Округление', 'Дизеринг']:
        methods[key] = np.clip(methods[key], 0, 1023)
    
    # Добавляем обратно преобразованные сигналы
    roundtrip_methods = {}
    for key in ['Усечение', 'Округление', 'Дизеринг']:
        restored = convert_10bit_back(methods[key], source_bits)
        roundtrip_methods[f'{key} (восст.)'] = restored
    
    methods.update(roundtrip_methods)
    
    return methods

def convert_10bit_to_16bit(samples_10bit):
    """
    Обратное преобразование 10-бит в 16-бит
    """
    return (samples_10bit << 6).astype(np.int16)

def convert_10bit_to_24bit(samples_10bit):
    """
    Обратное преобразование 10-бит в 24-бит
    """
    return (samples_10bit << 14).astype(np.int32)

def convert_10bit_back(samples_10bit, target_bits):
    """
    Универсальная функция обратного преобразования
    """
    if target_bits == 16:
        return convert_10bit_to_16bit(samples_10bit)
    elif target_bits == 24:
        return convert_10bit_to_24bit(samples_10bit)
    else:
        raise ValueError("Поддерживаются только 16 и 24 бита")

def analyze_quantization_error(original, converted, source_bits):
    """
    Анализ ошибки квантования
    """
    # Масштабируем для сравнения
    scale_factor = 2**(source_bits - 10)
    converted_scaled = converted * scale_factor
    
    error = original - converted_scaled
    
    return {
        'max_error': np.max(np.abs(error)),
        'rms_error': np.sqrt(np.mean(error**2)),
        'snr_db': 20 * np.log10(np.std(original) / np.std(error)) if np.std(error) > 0 else float('inf')
    }

def analyze_roundtrip_error(original, converted_10bit, source_bits):
    """
    Анализ ошибки полного цикла (прямое + обратное преобразование)
    """
    # Обратное преобразование
    restored = convert_10bit_back(converted_10bit, source_bits)
    
    # Вычисляем ошибку
    error = original - restored
    
    return {
        'max_error': np.max(np.abs(error)),
        'rms_error': np.sqrt(np.mean(error**2)),
        'snr_db': 20 * np.log10(np.std(original) / np.std(error)) if np.std(error) > 0 else float('inf'),
        'restored_signal': restored
    }

def print_statistics(methods, source_bits):
    """
    Выводит статистику по всем методам
    """
    print(f"\n=== Статистика конвертации из {source_bits}-бит в 10-бит ===")
    print(f"{'Метод':<20} {'Мин':<8} {'Макс':<8} {'Среднее':<10} {'СКО':<10}")
    print("-" * 65)
    
    original = methods['Исходный']
    
    for name, data in methods.items():
        if name == 'Исходный':
            print(f"{name:<20} {data.min():<8} {data.max():<8} {data.mean():<10.1f} {data.std():<10.1f}")
        elif not name.endswith('(восст.)'):
            print(f"{name:<20} {data.min():<8} {data.max():<8} {data.mean():<10.1f} {data.std():<10.1f}")
            
            # Анализ ошибок прямого преобразования
            error_stats = analyze_quantization_error(original, data, source_bits)
            print(f"{'  → Прямая ошибка':<20}: Макс: {error_stats['max_error']:.1f}, "
                  f"RMS: {error_stats['rms_error']:.1f}, SNR: {error_stats['snr_db']:.1f} дБ")
        else:
            # Восстановленный сигнал
            print(f"{name:<20} {data.min():<8} {data.max():<8} {data.mean():<10.1f} {data.std():<10.1f}")
            
            # Анализ ошибок полного цикла
            base_method = name.replace(' (восст.)', '')
            method_10bit = methods[base_method]
            roundtrip_stats = analyze_roundtrip_error(original, method_10bit, source_bits)
            print(f"{'  → Полный цикл':<20}: Макс: {roundtrip_stats['max_error']:.1f}, "
                  f"RMS: {roundtrip_stats['rms_error']:.1f}, SNR: {roundtrip_stats['snr_db']:.1f} дБ")

def plot_comparison(methods, time_array, start_sample=0, end_sample=200):
    """
    Визуализация сравнения методов
    """
    # Фильтруем методы для отображения
    plot_methods = {}
    
    # Сначала добавляем исходный сигнал
    if 'Исходный' in methods:
        plot_methods['Исходный'] = methods['Исходный']
    
    # Добавляем методы прямого преобразования (10-бит)
    conversion_methods = ['Усечение', 'Округление', 'Дизеринг']
    for method in conversion_methods:
        if method in methods:
            plot_methods[f'{method} (10-бит)'] = methods[method]
    
    # Добавляем восстановленные сигналы
    for method in conversion_methods:
        restored_key = f'{method} (восст.)'
        if restored_key in methods:
            plot_methods[f'{method} (восст.)'] = methods[restored_key]
    
    # Создаем сетку графиков
    n_plots = len(plot_methods)
    if n_plots <= 4:
        rows, cols = 2, 2
    elif n_plots <= 6:
        rows, cols = 2, 3
    elif n_plots <= 9:
        rows, cols = 3, 3
    else:
        rows, cols = 4, 3
    
    plt.figure(figsize=(5 * cols, 4 * rows))
    
    # Ограничиваем диапазон для детального просмотра
    t_plot = time_array[start_sample:end_sample]
    
    for i, (name, data) in enumerate(plot_methods.items(), 1):
        plt.subplot(rows, cols, i)
        
        if name == 'Исходный':
            plt.plot(t_plot, data[start_sample:end_sample], 'b-', linewidth=2, label=name)
        elif '(10-бит)' in name:
            # 10-битные данные
            plt.plot(t_plot, data[start_sample:end_sample], 'r-', linewidth=1, label=name)
            # Также показываем масштабированную версию для сравнения
            scaled_data = data * 64  # Масштабирование для 16->10 бит
            plt.plot(t_plot, scaled_data[start_sample:end_sample], 'r--', linewidth=1, alpha=0.5, label=f'{name} (×64)')
        elif '(восст.)' in name:
            # Восстановленные сигналы
            plt.plot(t_plot, data[start_sample:end_sample], 'g-', linewidth=1, label=name)
            # Показываем разность с исходным
            if 'Исходный' in methods:
                error = data - methods['Исходный']
                plt.plot(t_plot, error[start_sample:end_sample], 'm:', linewidth=1, alpha=0.7, label='Ошибка')
        
        plt.title(f'{name}')
        plt.xlabel('Время (с)')
        plt.ylabel('Амплитуда')
        plt.grid(True, alpha=0.3)
        plt.legend(fontsize=8)
    
    plt.tight_layout()
    plt.show()

def plot_error_analysis(methods, time_array, source_bits, start_sample=0, end_sample=200):
    """
    Отдельный график для анализа ошибок
    """
    if 'Исходный' not in methods:
        return
    
    original = methods['Исходный']
    t_plot = time_array[start_sample:end_sample]
    
    plt.figure(figsize=(15, 8))
    
    # График 1: Исходный сигнал
    plt.subplot(2, 2, 1)
    plt.plot(t_plot, original[start_sample:end_sample], 'b-', linewidth=2)
    plt.title('Исходный сигнал')
    plt.xlabel('Время (с)')
    plt.ylabel('Амплитуда')
    plt.grid(True, alpha=0.3)
    
    # График 2: Ошибки прямого преобразования
    plt.subplot(2, 2, 2)
    scale_factor = 2**(source_bits - 10)
    
    for method in ['Усечение', 'Округление', 'Дизеринг']:
        if method in methods:
            converted_scaled = methods[method] * scale_factor
            error = original - converted_scaled
            plt.plot(t_plot, error[start_sample:end_sample], linewidth=1, label=f'{method}')
    
    plt.title('Ошибки прямого преобразования')
    plt.xlabel('Время (с)')
    plt.ylabel('Ошибка')
    plt.legend()
    plt.grid(True, alpha=0.3)
    
    # График 3: Ошибки полного цикла
    plt.subplot(2, 2, 3)
    
    for method in ['Усечение', 'Округление', 'Дизеринг']:
        restored_key = f'{method} (восст.)'
        if restored_key in methods:
            error = original - methods[restored_key]
            plt.plot(t_plot, error[start_sample:end_sample], linewidth=1, label=f'{method}')
    
    plt.title('Ошибки полного цикла')
    plt.xlabel('Время (с)')
    plt.ylabel('Ошибка')
    plt.legend()
    plt.grid(True, alpha=0.3)
    
    # График 4: Сравнение всех восстановленных сигналов
    plt.subplot(2, 2, 4)
    plt.plot(t_plot, original[start_sample:end_sample], 'b-', linewidth=2, label='Исходный')
    
    for method in ['Усечение', 'Округление', 'Дизеринг']:
        restored_key = f'{method} (восст.)'
        if restored_key in methods:
            plt.plot(t_plot, methods[restored_key][start_sample:end_sample], 
                    linewidth=1, alpha=0.8, label=f'{method} (восст.)')
    
    plt.title('Сравнение восстановленных сигналов')
    plt.xlabel('Время (с)')
    plt.ylabel('Амплитуда')
    plt.legend()
    plt.grid(True, alpha=0.3)
    
    plt.tight_layout()
    plt.show()

# Основная демонстрация
if __name__ == "__main__":
    # Параметры сигнала
    sample_rate = 48000  # Hz
    duration = 0.01      # секунд (короткий отрезок для детального анализа)
    frequency = 1000     # Hz
    
    print("=== Демонстрация конвертации PCM треугольного сигнала ===")
    
    # Тестируем для 16-бит
    print("\n16-битный треугольный сигнал:")
    triangle_16, time_16 = generate_triangle_wave(sample_rate, duration, frequency, 16)
    methods_16 = convert_and_compare(triangle_16, 16, time_16)
    print_statistics(methods_16, 16)
    
    # Показываем первые значения
    print(f"\nПервые 10 значений (прямое преобразование):")
    for name, data in methods_16.items():
        if not name.endswith('(восст.)'):
            print(f"{name:<12}: {data[:10]}")
    
    print(f"\nПервые 10 значений (обратное преобразование):")
    for name, data in methods_16.items():
        if name.endswith('(восст.)'):
            print(f"{name:<20}: {data[:10]}")
    
    # Демонстрация обратного преобразования
    print(f"\n=== Демонстрация обратного преобразования ===")
    test_10bit_values = np.array([0, 256, 512, 768, 1023])  # Типичные 10-битные значения
    print(f"10-битные значения: {test_10bit_values}")
    print(f"→ 16-бит: {convert_10bit_to_16bit(test_10bit_values)}")
    print(f"→ 24-бит: {convert_10bit_to_24bit(test_10bit_values)}")
    
    # Проверка цикла туда-обратно
    print(f"\n=== Проверка цикла туда-обратно ===")
    original_16bit = np.array([0, 8192, 16384, 24576, 32767])
    print(f"Исходные 16-бит: {original_16bit}")
    
    # Прямое преобразование разными методами
    truncated = truncate_to_10bit(original_16bit, 16)
    rounded = round_to_10bit(original_16bit, 16)
    
    print(f"→ 10-бит (усечение): {truncated}")
    print(f"→ 10-бит (округление): {rounded}")
    
    # Обратное преобразование
    restored_trunc = convert_10bit_to_16bit(truncated)
    restored_round = convert_10bit_to_16bit(rounded)
    
    print(f"→ 16-бит (из усечения): {restored_trunc}")
    print(f"→ 16-бит (из округления): {restored_round}")
    
    # Ошибки
    error_trunc = original_16bit - restored_trunc
    error_round = original_16bit - restored_round
    
    print(f"Ошибки усечения: {error_trunc}")
    print(f"Ошибки округления: {error_round}")
    
    # Тестируем для 24-бит
    print("\n" + "="*60)
    print("24-битный треугольный сигнал:")
    triangle_24, time_24 = generate_triangle_wave(sample_rate, duration, frequency, 24)
    methods_24 = convert_and_compare(triangle_24, 24, time_24)
    print_statistics(methods_24, 24)
    
    # Показываем первые значения
    print(f"\nПервые 10 значений (прямое преобразование):")
    for name, data in methods_24.items():
        if not name.endswith('(восст.)'):
            print(f"{name:<12}: {data[:10]}")
    
    print(f"\nПервые 10 значений (обратное преобразование):")
    for name, data in methods_24.items():
        if name.endswith('(восст.)'):
            print(f"{name:<20}: {data[:10]}")
    
    # Демонстрация различий между методами на конкретных значениях
    print("\n" + "="*60)
    print("Сравнение методов на пике треугольника:")
    
    # Находим пик треугольника (максимальное значение)
    peak_idx_16 = np.argmax(triangle_16)
    print(f"\n16-бит (индекс {peak_idx_16}):")
    for name, data in methods_16.items():
        if name == 'Исходный':
            print(f"{name:<20}: {data[peak_idx_16]} (макс: {np.max(data)})")
        elif not name.endswith('(восст.)'):
            original_val = triangle_16[peak_idx_16]
            converted_val = data[peak_idx_16]
            error = abs(original_val - converted_val * 64)  # 64 = 2^6 для 16->10 бит
            print(f"{name:<20}: {converted_val} (ошибка: {error})")
        else:
            # Восстановленный сигнал
            original_val = triangle_16[peak_idx_16]
            restored_val = data[peak_idx_16]
            error = abs(original_val - restored_val)
            print(f"{name:<20}: {restored_val} (ошибка полного цикла: {error})")
    
    # Анализ потери разрешения
    print(f"\n=== Анализ потери разрешения ===")
    print(f"16-бит -> 10-бит: потеря {16-10} = 6 битов, коэффициент {2**6} = 64")
    print(f"24-бит -> 10-бит: потеря {24-10} = 14 битов, коэффициент {2**14} = 16384")
    print(f"Исходный диапазон 16-бит: 0 - 32767")
    print(f"Результирующий диапазон 10-бит: 0 - 1023")
    print(f"Потеря динамического диапазона: {20 * np.log10(64):.1f} дБ")
    
    print(f"\n=== Теоретические пределы ошибок ===")
    print(f"16→10→16 бит:")
    print(f"  Усечение: максимальная ошибка = {2**6 - 1} = 63")
    print(f"  Округление: максимальная ошибка = {2**6 // 2} = 32")
    print(f"24→10→24 бит:")
    print(f"  Усечение: максимальная ошибка = {2**14 - 1} = 16383")
    print(f"  Округление: максимальная ошибка = {2**14 // 2} = 8192")
    
    # Раскомментируйте для визуализации (требует matplotlib)
    plot_comparison(methods_16, time_16)
    plot_error_analysis(methods_16, time_16, 16)
#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Симуляция функций преобразования PCM ↔ PPM для тестирования
"""

import numpy as np
import matplotlib.pyplot as plt

# Константы из C кода
INT16_MIN = -32768
INT16_MAX = 32767
INT24_MIN = -8388608
INT24_MAX = 8388607

def audio_to_ppm(audio_sample):
    """Преобразование 16-битного PCM в 10-битное значение для PPM"""
    # Сдвиг от знакового диапазона [-32768, 32767] к беззнаковому [0, 65535]
    unsigned_sample = int(audio_sample) + 32768
    
    # Масштабирование от 16-бит (0-65535) к 10-бит (0-1023)
    # Используем округление для лучшей точности
    return (unsigned_sample * 1023 + 32767) // 65535

def audio24_to_ppm(audio_sample):
    """Преобразование 24-битного PCM в 10-битное значение для PPM"""
    # Убеждаемся, что это валидное 24-битное значение
    audio_sample = int(audio_sample)
    
    # Ограничиваем диапазон 24-битными значениями
    if audio_sample > INT24_MAX:
        audio_sample = INT24_MAX
    elif audio_sample < INT24_MIN:
        audio_sample = INT24_MIN
    
    # Сдвиг от знакового диапазона [-8388608, 8388607] к беззнаковому [0, 16777215]
    unsigned_sample = audio_sample + 8388608
    
    # Масштабирование от 24-бит (0-16777215) к 10-бит (0-1023)
    return (unsigned_sample * 1023 + 8388607) // 16777215

def ppm_to_audio(ppm_value):
    """Преобразование 10-битного PPM обратно в 16-битное PCM"""
    # Ограничиваем диапазон
    ppm_value &= 0x3FF  # 0-1023
    
    # Масштабирование от 10-бит (0-1023) к 16-бит (0-65535)
    unsigned_sample = (ppm_value * 65535 + 511) // 1023
    
    # Сдвиг обратно к знаковому диапазону
    return int(unsigned_sample) - 32768

def ppm_to_audio24(ppm_value):
    """Преобразование 10-битного PPM обратно в 24-битное PCM"""
    # Ограничиваем диапазон
    ppm_value &= 0x3FF  # 0-1023
    
    # Масштабирование от 10-бит (0-1023) к 24-бит (0-16777215)
    unsigned_sample = (ppm_value * 16777215 + 511) // 1023
    
    # Сдвиг обратно к знаковому диапазону
    signed_sample = unsigned_sample - 8388608
    
    # Ограничиваем результат 24-битным диапазоном
    if signed_sample > INT24_MAX:
        signed_sample = INT24_MAX
    elif signed_sample < INT24_MIN:
        signed_sample = INT24_MIN
        
    return signed_sample

def test_conversion_accuracy():
    """Тестирование точности преобразования"""
    print("=== Тест точности преобразования ===")
    
    # Тест для 16-бит
    print("\n16-битные тесты:")
    test_samples_16 = [INT16_MIN, INT16_MIN//2, -1, 0, 1, INT16_MAX//2, INT16_MAX]
    
    for sample in test_samples_16:
        ppm = audio_to_ppm(sample)
        back = ppm_to_audio(ppm)
        error = abs(sample - back)
        print(f"16-bit: {sample:6d} -> {ppm:4d} -> {back:6d} (error: {error})")
    
    # Тест для 24-бит
    print("\n24-битные тесты:")
    test_samples_24 = [INT24_MIN, INT24_MIN//2, -1, 0, 1, INT24_MAX//2, INT24_MAX]
    
    for sample in test_samples_24:
        ppm = audio24_to_ppm(sample)
        back = ppm_to_audio24(ppm)  # Исправлено: используем ppm, а не sample
        error = abs(sample - back)
        print(f"24-bit: {sample:8d} -> {ppm:4d} -> {back:8d} (error: {error})")

def generate_ramp_signal(duration=1.0, sample_rate=48000, bits=16):
    """Генерация ramp-сигнала для тестирования"""
    samples = int(duration * sample_rate)
    t = np.linspace(0, duration, samples, endpoint=False)
    
    if bits == 16:
        # Линейный ramp от -32768 до 32767
        ramp = np.linspace(INT16_MIN, INT16_MAX, samples, dtype=np.int32)  # Используем int32 для избежания переполнения
    else:  # 24-bit
        # Линейный ramp от -8388608 до 8388607
        ramp = np.linspace(INT24_MIN, INT24_MAX, samples, dtype=np.int32)
    
    return t, ramp

def simulate_transmission(ramp_signal, bits=16):
    """Симуляция передачи через PPM"""
    transmitted = []
    
    for sample in ramp_signal:
        sample = int(sample)  # Убеждаемся, что это целое число
        if bits == 16:
            ppm = audio_to_ppm(sample)
            reconstructed = ppm_to_audio(ppm)
        else:  # 24-bit
            ppm = audio24_to_ppm(sample)
            reconstructed = ppm_to_audio24(ppm)
            
        transmitted.append(reconstructed)
    
    return np.array(transmitted, dtype=np.int32)  # Используем int32 для больших значений

def plot_results(t, original, transmitted, bits=16):
    """Визуализация результатов"""
    fig, (ax1, ax2, ax3) = plt.subplots(3, 1, figsize=(12, 10))
    
    # Исходный сигнал
    ax1.plot(t * 1000, original, 'b-', linewidth=1, label='Исходный ramp')
    ax1.set_ylabel(f'Амплитуда ({bits}-bit)')
    ax1.set_title('Исходный ramp-сигнал')
    ax1.grid(True, alpha=0.3)
    ax1.legend()
    
    # Восстановленный сигнал
    ax2.plot(t * 1000, transmitted, 'r-', linewidth=1, label='Восстановленный')
    ax2.set_ylabel(f'Амплитуда ({bits}-bit)')
    ax2.set_title('Восстановленный после PPM')
    ax2.grid(True, alpha=0.3)
    ax2.legend()
    
    # Ошибка
    error = transmitted - original
    ax3.plot(t * 1000, error, 'g-', linewidth=1, label='Ошибка')
    ax3.set_xlabel('Время (мс)')
    ax3.set_ylabel('Ошибка')
    ax3.set_title('Ошибка восстановления')
    ax3.grid(True, alpha=0.3)
    ax3.legend()
    
    plt.tight_layout()
    plt.show()
    
    # Статистика ошибок
    print(f"\n=== Статистика ошибок ({bits}-bit) ===")
    print(f"Максимальная ошибка: {np.max(np.abs(error))}")
    print(f"Средняя ошибка: {np.mean(np.abs(error)):.2f}")
    print(f"RMS ошибка: {np.sqrt(np.mean(error**2)):.2f}")

def test_edge_cases():
    """Тестирование граничных случаев"""
    print("\n=== Тест граничных случаев ===")
    
    # Тест монотонности для 16-бит
    print("\nТест монотонности (16-бит):")
    prev_ppm = -1
    monotonic = True
    for sample in range(INT16_MIN, INT16_MAX, 1000):
        ppm = audio_to_ppm(sample)
        if ppm < prev_ppm:
            monotonic = False
            print(f"Нарушение монотонности: {sample} -> {ppm} (prev: {prev_ppm})")
        prev_ppm = ppm
    
    if monotonic:
        print("Монотонность 16-бит: ОК")
    
    # Проверка диапазона PPM
    print(f"\nДиапазон PPM значений:")
    print(f"16-бит: {audio_to_ppm(INT16_MIN)} - {audio_to_ppm(INT16_MAX)}")
    print(f"24-бит: {audio24_to_ppm(INT24_MIN)} - {audio24_to_ppm(INT24_MAX)}")

def plot_ppm_values(t, original, ppm_values, transmitted, bits=16):
    """Визуализация PPM значений в процессе передачи"""
    fig, ((ax1, ax2), (ax3, ax4)) = plt.subplots(2, 2, figsize=(15, 10))
    
    # Исходный сигнал (увеличенный масштаб для детализации)
    start_idx = 0
    end_idx = min(200, len(original))  # Показываем первые 200 сэмплов
    
    ax1.plot(t[start_idx:end_idx] * 1000, original[start_idx:end_idx], 'b-', linewidth=2, marker='o', markersize=3)
    ax1.set_ylabel(f'Амплитуда ({bits}-bit)')
    ax1.set_title('Исходный PCM сигнал (детализация)')
    ax1.grid(True, alpha=0.3)
    
    # PPM значения
    ax2.plot(t[start_idx:end_idx] * 1000, ppm_values[start_idx:end_idx], 'r-', linewidth=2, marker='s', markersize=3)
    ax2.set_ylabel('PPM значение (10-bit)')
    ax2.set_title('PPM значения (0-1023)')
    ax2.grid(True, alpha=0.3)
    ax2.set_ylim(-10, 1030)
    
    # Восстановленный сигнал
    ax3.plot(t[start_idx:end_idx] * 1000, transmitted[start_idx:end_idx], 'g-', linewidth=2, marker='^', markersize=3)
    ax3.set_xlabel('Время (мс)')
    ax3.set_ylabel(f'Амплитуда ({bits}-bit)')
    ax3.set_title('Восстановленный PCM сигнал')
    ax3.grid(True, alpha=0.3)
    
    # Ошибка восстановления
    error = transmitted[start_idx:end_idx] - original[start_idx:end_idx]
    ax4.plot(t[start_idx:end_idx] * 1000, error, 'm-', linewidth=2, marker='x', markersize=3)
    ax4.set_xlabel('Время (мс)')
    ax4.set_ylabel('Ошибка')
    ax4.set_title('Ошибка восстановления')
    ax4.grid(True, alpha=0.3)
    
    plt.tight_layout()
    plt.show()

def analyze_ppm_distribution(ppm_values):
    """Анализ распределения PPM значений"""
    print(f"\n=== Анализ PPM значений ===")
    print(f"Минимальное PPM: {np.min(ppm_values)}")
    print(f"Максимальное PPM: {np.max(ppm_values)}")
    print(f"Среднее PPM: {np.mean(ppm_values):.2f}")
    print(f"Уникальных PPM значений: {len(np.unique(ppm_values))}")
    
    # Гистограмма распределения PPM
    plt.figure(figsize=(10, 6))
    plt.hist(ppm_values, bins=50, alpha=0.7, color='blue', edgecolor='black')
    plt.xlabel('PPM значение')
    plt.ylabel('Количество')
    plt.title('Распределение PPM значений')
    plt.grid(True, alpha=0.3)
    plt.show()

def simulate_transmission_with_ppm(ramp_signal, bits=16):
    """Симуляция передачи через PPM с сохранением PPM значений"""
    transmitted = []
    ppm_values = []
    
    for sample in ramp_signal:
        sample = int(sample)
        if bits == 16:
            ppm = audio_to_ppm(sample)
            reconstructed = ppm_to_audio(ppm)
        else:  # 24-bit
            ppm = audio24_to_ppm(sample)
            reconstructed = ppm_to_audio24(ppm)
        
        ppm_values.append(ppm)
        transmitted.append(reconstructed)
    
    return np.array(transmitted, dtype=np.int32), np.array(ppm_values)

def print_conversion_table(bits=16, num_samples=20):
    """Печать таблицы преобразования для анализа"""
    print(f"\n=== Таблица преобразования ({bits}-bit) ===")
    print("PCM → PPM → PCM восстановленный | Ошибка")
    print("-" * 50)
    
    if bits == 16:
        test_range = np.linspace(INT16_MIN, INT16_MAX, num_samples, dtype=np.int32)
        for pcm in test_range:
            ppm = audio_to_ppm(pcm)
            restored = ppm_to_audio(ppm)
            error = abs(pcm - restored)
            print(f"{pcm:6d} → {ppm:4d} → {restored:6d} | {error:3d}")
    else:  # 24-bit
        test_range = np.linspace(INT24_MIN, INT24_MAX, num_samples, dtype=np.int32)
        for pcm in test_range:
            ppm = audio24_to_ppm(pcm)
            restored = ppm_to_audio24(ppm)
            error = abs(pcm - restored)
            print(f"{pcm:8d} → {ppm:4d} → {restored:8d} | {error:5d}")

def main():
    """Основная функция"""
    print("Симуляция функций преобразования PCM ↔ PPM")
    print("=" * 50)
    
    # Тестирование точности
    test_conversion_accuracy()
    
    # Печать таблиц преобразования
    print_conversion_table(bits=16, num_samples=10)
    print_conversion_table(bits=24, num_samples=10)
    
    # Тестирование граничных случаев
    test_edge_cases()
    
    # Симуляция ramp-сигнала (16-бит) с PPM
    print("\n=== Симуляция передачи ramp-сигнала (16-бит) ===")
    t, ramp_16 = generate_ramp_signal(duration=0.01, sample_rate=48000, bits=16)
    transmitted_16, ppm_16 = simulate_transmission_with_ppm(ramp_16, bits=16)
    
    # Показываем PPM значения
    plot_ppm_values(t, ramp_16, ppm_16, transmitted_16, bits=16)
    analyze_ppm_distribution(ppm_16)
    plot_results(t, ramp_16, transmitted_16, bits=16)
    
    # Симуляция ramp-сигнала (24-бит) с PPM
    print("\n=== Симуляция передачи ramp-сигнала (24-бит) ===")
    t, ramp_24 = generate_ramp_signal(duration=0.01, sample_rate=48000, bits=24)
    transmitted_24, ppm_24 = simulate_transmission_with_ppm(ramp_24, bits=24)
    
    # Показываем PPM значения
    plot_ppm_values(t, ramp_24, ppm_24, transmitted_24, bits=24)
    analyze_ppm_distribution(ppm_24)
    plot_results(t, ramp_24, transmitted_24, bits=24)

if __name__ == "__main__":
    main()
#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Скрипт для передачи плавно нарастающего сигнала через аудиокарту
Используется для тестирования точности воспроизведения без анализа записи
"""

import numpy as np
import sounddevice as sd
import matplotlib.pyplot as plt
import argparse
import time


class AudioRampGenerator:
    def __init__(
        self,
        device_name=None,
        sample_rate=48000,
        output_channel=0,
        amplitude=0.9,
    ):
        self.sample_rate = sample_rate
        self.device_name = device_name
        self.device_id = self._find_device()
        self.output_channel = output_channel
        self.amplitude = amplitude
        
    def _find_device(self):
        """Найти аудиоустройство в системе"""
        devices = sd.query_devices()

        if self.device_name:
            # Пробуем интерпретировать как индекс устройства
            try:
                index = int(self.device_name)
                if 0 <= index < len(devices):
                    print(f"Найдено устройство по индексу {index}: {devices[index]['name']}")
                    return index
            except ValueError:
                # Если не число, ищем по имени
                for i, device in enumerate(devices):
                    if self.device_name.lower() in device["name"].lower():
                        print(f"Найдено устройство по имени: {device['name']}")
                        return i

        # Если устройство не найдено, показать доступные
        print("Доступные аудиоустройства:")
        for i, device in enumerate(devices):
            print(f"{i}: {device['name']} (вход: {device['max_input_channels']}, выход: {device['max_output_channels']})")

        return None

    def generate_ramp_signal(self, duration=10.0, ramp_type="linear"):
        """Генерация плавно нарастающего сигнала заданной длительности"""
        # Расчет количества отсчетов
        num_samples = int(self.sample_rate * duration)
        
        if ramp_type == "linear":
            # Линейно нарастающий сигнал от 0 до максимума
            signal_data = np.linspace(0, self.amplitude, num_samples)
        elif ramp_type == "triangle":
            # Треугольный сигнал (нарастание и спад)
            half_samples = num_samples // 2
            rising = np.linspace(0, self.amplitude, half_samples)
            falling = np.linspace(self.amplitude, 0, num_samples - half_samples)
            signal_data = np.concatenate((rising, falling))
        elif ramp_type == "exponential":
            # Экспоненциально нарастающий сигнал
            x = np.linspace(0, 5, num_samples)  # Диапазон от 0 до 5 для экспоненты
            signal_data = (np.exp(x) - 1) / (np.exp(5) - 1) * self.amplitude
        elif ramp_type == "logarithmic":
            # Логарифмически нарастающий сигнал
            x = np.linspace(1, 10, num_samples)  # Начинаем с 1 для избежания log(0)
            signal_data = np.log(x) / np.log(10) * self.amplitude
        elif ramp_type == "sine_sweep":
            # Синусоидальная частотная развертка от 20 Гц до 20 кГц
            t = np.linspace(0, duration, num_samples)
            # Фаза зависит от частоты, которая меняется экспоненциально
            phase = 2 * np.pi * 20 * duration / np.log(20000/20) * (np.exp(t/duration * np.log(20000/20)) - 1)
            signal_data = self.amplitude * np.sin(phase)
        else:
            print(f"Неизвестный тип сигнала: {ramp_type}. Использую линейную рампу.")
            signal_data = np.linspace(0, self.amplitude, num_samples)
            
        print(f"Создан сигнал типа {ramp_type}: {num_samples} отсчетов, {duration:.2f} секунд")
        return signal_data.astype(np.float32)

    def play_signal(self, signal_data):
        """Воспроизведение сигнала через аудиокарту без записи"""
        if self.device_id is None:
            print("Устройство не выбрано!")
            return False

        # Получить информацию о каналах устройства
        device_info = sd.query_devices(self.device_id)
        out_channels = device_info["max_output_channels"]

        print(f"Устройство: {device_info['name']}")
        print(f"Доступно выходных каналов: {out_channels}")
        print(f"Частота дискретизации: {self.sample_rate} Гц")
        print(f"Длительность: {len(signal_data)/self.sample_rate:.2f} секунд")

        # Создаем массив для всех каналов
        out_data = np.zeros((len(signal_data), out_channels), dtype=np.float32)
        
        # Заполняем все каналы сигналом
        for ch in range(out_channels):
            out_data[:, ch] = signal_data
        
        try:
            # Воспроизведение сигнала блокирующим методом для точного тайминга
            start_time = time.time()
            sd.play(out_data, samplerate=self.sample_rate, device=self.device_id, blocking=True)
            elapsed = time.time() - start_time
            
            print(f"Воспроизведение завершено! Затраченное время: {elapsed:.3f} секунд")
            return True
            
        except Exception as e:
            print(f"Ошибка при воспроизведении: {e}")
            return False

    def visualize_signal(self, signal_data):
        """Визуализация генерируемого сигнала"""
        plt.figure(figsize=(12, 6))
        
        # Вычисление временной шкалы в секундах
        duration = len(signal_data) / self.sample_rate
        t = np.linspace(0, duration, len(signal_data))
        
        plt.plot(t, signal_data, 'b-')
        plt.title("Сигнал для воспроизведения")
        plt.xlabel("Время (сек)")
        plt.ylabel("Амплитуда")
        plt.grid(True)
        
        # Добавить информацию о сигнале
        plt.text(0.02, 0.95, 
                f"Частота дискретизации: {self.sample_rate} Гц\n"
                f"Длительность: {duration:.2f} секунд\n"
                f"Макс. амплитуда: {self.amplitude:.2f}",
                transform=plt.gca().transAxes,
                bbox=dict(boxstyle="round", fc="white", alpha=0.8))
        
        plt.tight_layout()
        plt.show()

    def run_test(self, duration=10.0, ramp_type="linear", visualize=True):
        """Запуск теста непрерывного воспроизведения"""
        if self.device_id is None:
            print("Устройство не выбрано!")
            return False
            
        print(f"\n=== ЗАПУСК ТЕСТА НЕПРЕРЫВНОГО ВОСПРОИЗВЕДЕНИЯ ===")
        print(f"Тип сигнала: {ramp_type}")
        
        # Генерация сигнала
        signal_data = self.generate_ramp_signal(duration, ramp_type)
        
        # Визуализация сигнала, если требуется
        if visualize:
            self.visualize_signal(signal_data)
        
        # Воспроизведение сигнала
        success = self.play_signal(signal_data)
        
        if success:
            print("\nТест успешно завершен!")
        else:
            print("\nТест не удался!")
        
        return success


def main():
    parser = argparse.ArgumentParser(description="Тест непрерывного воспроизведения аудио")
    parser.add_argument("--device", "-d", type=str, help="Имя или индекс аудиоустройства")
    parser.add_argument("--sample-rate", "-s", type=int, default=48000, help="Частота дискретизации")
    parser.add_argument("--output-channel", "-o", type=int, default=0, help="Индекс выходного канала")
    parser.add_argument("--amplitude", "-a", type=float, default=0.9, help="Максимальная амплитуда сигнала (0-1)")
    parser.add_argument("--duration", "-t", type=float, default=10.0, help="Длительность сигнала в секундах")
    parser.add_argument("--ramp-type", "-r", type=str, default="linear", 
                        choices=["linear", "triangle", "exponential", "logarithmic", "sine_sweep"],
                        help="Тип нарастания сигнала")
    parser.add_argument("--no-plot", action="store_true", help="Не показывать график сигнала")

    args = parser.parse_args()
    
    generator = AudioRampGenerator(
        device_name=args.device,
        sample_rate=args.sample_rate,
        output_channel=args.output_channel,
        amplitude=args.amplitude
    )
    
    generator.run_test(
        duration=args.duration,
        ramp_type=args.ramp_type,
        visualize=not args.no_plot
    )


if __name__ == "__main__":
    main()
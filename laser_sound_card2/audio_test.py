#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Скрипт для тестирования PPM аудиокарты
Передает аудио сигнал через карту и сравнивает с принятым
"""

import numpy as np
import sounddevice as sd
import time
import matplotlib.pyplot as plt
from scipy import signal
import argparse

class PPMAudioTester:
    def __init__(self, device_name=None, sample_rate=48000, duration=2.0):
        self.sample_rate = sample_rate
        self.duration = duration
        self.device_name = device_name
        self.device_id = self._find_device()
        
    def _find_device(self):
        """Найти PPM аудиокарту в системе"""
        devices = sd.query_devices()
        
        if self.device_name:
            for i, device in enumerate(devices):
                if self.device_name.lower() in device['name'].lower():
                    print(f"Найдено устройство: {device['name']}")
                    return i
        
        # Если устройство не найдено, показать доступные
        print("Доступные аудиоустройства:")
        for i, device in enumerate(devices):
            print(f"{i}: {device['name']} (in: {device['max_input_channels']}, out: {device['max_output_channels']})")
        
        return None
    
    def generate_test_signal(self, freq=1000, amplitude=0.5):
        """Генерация тестового синусоидального сигнала"""
        t = np.linspace(0, self.duration, int(self.sample_rate * self.duration), False)
        signal_data = amplitude * np.sin(2 * np.pi * freq * t)
        return signal_data.astype(np.float32)
    
    def generate_sweep_signal(self, start_freq=100, end_freq=8000, amplitude=0.5):
        """Генерация sweep сигнала для частотного анализа"""
        t = np.linspace(0, self.duration, int(self.sample_rate * self.duration), False)
        sweep = signal.chirp(t, start_freq, self.duration, end_freq, method='linear')
        return (amplitude * sweep).astype(np.float32)
    
    def play_and_record(self, test_signal):
        """Воспроизведение и одновременная запись сигнала"""
        recorded_data = np.zeros((len(test_signal), 1), dtype=np.float32)
        
        def callback(indata, outdata, frames, time, status):
            if status:
                print(f"Статус аудио: {status}")
            
            # Воспроизведение тестового сигнала
            if callback.play_idx + frames <= len(test_signal):
                outdata[:, 0] = test_signal[callback.play_idx:callback.play_idx + frames]
                callback.play_idx += frames
            else:
                outdata.fill(0)
            
            # Запись входящего сигнала
            if callback.rec_idx + frames <= len(recorded_data):
                recorded_data[callback.rec_idx:callback.rec_idx + frames] = indata
                callback.rec_idx += frames
        
        callback.play_idx = 0
        callback.rec_idx = 0
        
        try:
            with sd.Stream(device=self.device_id,
                          channels=1,
                          samplerate=self.sample_rate,
                          callback=callback,
                          dtype=np.float32):
                print(f"Воспроизведение и запись в течение {self.duration} секунд...")
                sd.sleep(int(self.duration * 1000))
                
        except Exception as e:
            print(f"Ошибка аудио: {e}")
            return None
        
        return recorded_data.flatten()
    
    def analyze_signals(self, original, recorded):
        """Анализ и сравнение оригинального и записанного сигналов"""
        results = {}
        
        # Выравнивание по времени (компенсация задержки)
        correlation = np.correlate(recorded, original, mode='full')
        delay = np.argmax(correlation) - len(original) + 1
        
        if abs(delay) < len(recorded):
            if delay > 0:
                recorded_aligned = recorded[delay:]
                original_aligned = original[:len(recorded_aligned)]
            else:
                recorded_aligned = recorded[:len(recorded) + delay]
                original_aligned = original[-delay:len(recorded_aligned) - delay]
        else:
            recorded_aligned = recorded
            original_aligned = original
        
        # Обрезка до одинаковой длины
        min_len = min(len(original_aligned), len(recorded_aligned))
        original_aligned = original_aligned[:min_len]
        recorded_aligned = recorded_aligned[:min_len]
        
        # Расчет метрик
        results['delay_samples'] = delay
        results['delay_ms'] = delay * 1000 / self.sample_rate
        
        # SNR (соотношение сигнал/шум)
        signal_power = np.mean(original_aligned**2)
        noise_power = np.mean((recorded_aligned - original_aligned)**2)
        results['snr_db'] = 10 * np.log10(signal_power / (noise_power + 1e-10))
        
        # THD (общие гармонические искажения) - упрощенная оценка
        results['thd_percent'] = np.sqrt(noise_power / signal_power) * 100
        
        # Корреляция
        if len(original_aligned) > 0 and len(recorded_aligned) > 0:
            correlation_coef = np.corrcoef(original_aligned, recorded_aligned)[0, 1]
            results['correlation'] = correlation_coef if not np.isnan(correlation_coef) else 0
        else:
            results['correlation'] = 0
        
        # Частотный анализ
        freqs_orig, psd_orig = signal.welch(original_aligned, self.sample_rate, nperseg=1024)
        freqs_rec, psd_rec = signal.welch(recorded_aligned, self.sample_rate, nperseg=1024)
        
        results['original_aligned'] = original_aligned
        results['recorded_aligned'] = recorded_aligned
        results['freq_original'] = (freqs_orig, psd_orig)
        results['freq_recorded'] = (freqs_rec, psd_rec)
        
        return results
    
    def plot_results(self, results):
        """Визуализация результатов тестирования"""
        fig, axes = plt.subplots(2, 2, figsize=(12, 8))
        
        # Временные сигналы
        t = np.linspace(0, len(results['original_aligned']) / self.sample_rate, 
                       len(results['original_aligned']))
        
        axes[0, 0].plot(t[:1000], results['original_aligned'][:1000], 'b-', label='Оригинал')
        axes[0, 0].plot(t[:1000], results['recorded_aligned'][:1000], 'r-', label='Записанный')
        axes[0, 0].set_title('Временные сигналы (первые 1000 отсчетов)')
        axes[0, 0].set_xlabel('Время (с)')
        axes[0, 0].set_ylabel('Амплитуда')
        axes[0, 0].legend()
        axes[0, 0].grid(True)
        
        # Спектр мощности
        freqs_orig, psd_orig = results['freq_original']
        freqs_rec, psd_rec = results['freq_recorded']
        
        axes[0, 1].semilogy(freqs_orig, psd_orig, 'b-', label='Оригинал')
        axes[0, 1].semilogy(freqs_rec, psd_rec, 'r-', label='Записанный')
        axes[0, 1].set_title('Спектральная плотность мощности')
        axes[0, 1].set_xlabel('Частота (Гц)')
        axes[0, 1].set_ylabel('PSD')
        axes[0, 1].legend()
        axes[0, 1].grid(True)
        
        # Разность сигналов
        diff = results['recorded_aligned'] - results['original_aligned']
        axes[1, 0].plot(t[:1000], diff[:1000], 'g-')
        axes[1, 0].set_title('Разность сигналов')
        axes[1, 0].set_xlabel('Время (с)')
        axes[1, 0].set_ylabel('Амплитуда')
        axes[1, 0].grid(True)
        
        # Метрики
        metrics_text = f"""SNR: {results['snr_db']:.1f} дБ
THD: {results['thd_percent']:.2f}%
Корреляция: {results['correlation']:.3f}
Задержка: {results['delay_ms']:.1f} мс"""
        
        axes[1, 1].text(0.1, 0.5, metrics_text, fontsize=12, 
                        verticalalignment='center', fontfamily='monospace')
        axes[1, 1].set_title('Метрики качества')
        axes[1, 1].axis('off')
        
        plt.tight_layout()
        plt.show()
    
    def run_test(self, test_type='sine', frequency=1000):
        """Запуск полного теста"""
        print(f"=== Тест PPM аудиокарты ===")
        print(f"Частота дискретизации: {self.sample_rate} Гц")
        print(f"Длительность: {self.duration} с")
        
        if self.device_id is None:
            print("Устройство не выбрано!")
            return None
        
        # Генерация тестового сигнала
        if test_type == 'sine':
            test_signal = self.generate_test_signal(frequency)
            print(f"Генерация синуса {frequency} Гц")
        elif test_type == 'sweep':
            test_signal = self.generate_sweep_signal()
            print("Генерация sweep 100-8000 Гц")
        else:
            print("Неизвестный тип теста!")
            return None
        
        # Воспроизведение и запись
        recorded = self.play_and_record(test_signal)
        
        if recorded is None:
            print("Ошибка записи!")
            return None
        
        # Анализ результатов
        print("Анализ сигналов...")
        results = self.analyze_signals(test_signal, recorded)
        
        # Вывод результатов
        print("\n=== РЕЗУЛЬТАТЫ ТЕСТИРОВАНИЯ ===")
        print(f"SNR: {results['snr_db']:.1f} дБ")
        print(f"THD: {results['thd_percent']:.2f}%")
        print(f"Корреляция: {results['correlation']:.3f}")
        print(f"Задержка: {results['delay_ms']:.1f} мс ({results['delay_samples']} отсчетов)")
        
        # Оценка качества
        if results['snr_db'] > 40:
            quality = "ОТЛИЧНО"
        elif results['snr_db'] > 30:
            quality = "ХОРОШО"
        elif results['snr_db'] > 20:
            quality = "УДОВЛЕТВОРИТЕЛЬНО"
        else:
            quality = "ПЛОХО"
        
        print(f"Общая оценка: {quality}")
        
        # Показать графики
        self.plot_results(results)
        
        return results

def main():
    parser = argparse.ArgumentParser(description='Тестирование PPM аудиокарты')
    parser.add_argument('--device', '-d', type=str, help='Имя аудиоустройства')
    parser.add_argument('--frequency', '-f', type=int, default=1000, help='Частота тестового сигнала (Гц)')
    parser.add_argument('--duration', '-t', type=float, default=2.0, help='Длительность теста (с)')
    parser.add_argument('--test-type', '-m', choices=['sine', 'sweep'], default='sine', help='Тип теста')
    parser.add_argument('--sample-rate', '-s', type=int, default=48000, help='Частота дискретизации')
    
    args = parser.parse_args()
    
    # Создание и запуск тестера
    tester = PPMAudioTester(
        device_name=args.device,
        sample_rate=args.sample_rate,
        duration=args.duration
    )
    
    # Запуск теста
    results = tester.run_test(test_type=args.test_type, frequency=args.frequency)
    
    if results:
        print("\nТест завершен успешно!")
    else:
        print("\nТест не удался!")

if __name__ == "__main__":
    main()
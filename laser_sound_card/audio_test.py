#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Скрипт для тестирования аудиокарты
Передает аудио сигнал через карту и сравнивает с принятым
"""

import numpy as np
import sounddevice as sd
import matplotlib.pyplot as plt
from scipy import signal
import argparse


class AudioTester:
    def __init__(
        self,
        device_name=None,
        sample_rate=48000,
        duration=2.0,
        input_channel=0,
        output_channel=0,
        gain=1.0,
    ):
        self.sample_rate = sample_rate
        self.duration = duration
        self.device_name = device_name
        self.device_id = self._find_device()
        self.input_channel = input_channel
        self.output_channel = output_channel
        self.gain = gain
        self.threshold = 0.1

    def _find_device(self):
        """Найти аудиокарту в системе"""
        devices = sd.query_devices()

        if self.device_name:
            # Пробуем интерпретировать как индекс устройства
            try:
                index = int(self.device_name)
                if 0 <= index < len(devices):
                    print(
                        f"Найдено устройство по индексу {index}: {devices[index]['name']}"
                    )
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
            print(
                f"{i}: {device['name']} (in: {device['max_input_channels']}, out: {device['max_output_channels']})"
            )

        return None

    def generate_test_signal(
        self, test_type="sine", frequency=1000, amplitude=0.5, duration=None
    ):
        """Генерация тестового сигнала нужного типа"""
        if duration is None:
            duration = self.duration

        samples = int(self.sample_rate * duration)
        t = np.linspace(0, duration, samples, False)

        if test_type == "sine":
            # Синусоидальный сигнал
            signal_data = amplitude * np.sin(2 * np.pi * frequency * t)
        elif test_type == "sweep":
            # Sweep сигнал для частотного анализа
            signal_data = amplitude * signal.chirp(
                t, 100, duration, 8000, method="linear"
            )
        elif test_type == "multi":
            # Комбинированный сигнал для спектрального анализа
            signal_data = np.zeros(samples, dtype=np.float32)
            signal_data += 0.3 * np.sin(2 * np.pi * 500 * t)
            signal_data += 0.2 * np.sin(2 * np.pi * 5000 * t)
            signal_data += 0.1 * np.sin(2 * np.pi * 10000 * t)
        else:
            raise ValueError(f"Неизвестный тип теста: {test_type}")

        return signal_data.astype(np.float32)

    def run_audio_test(
        self, test_type="sine", frequency=1000, mode="normal", duration=None
    ):
        """Универсальный метод запуска тестирования в нужном режиме"""
        if self.device_id is None:
            print("Устройство не выбрано!")
            return None

        # Определяем параметры теста в зависимости от режима
        if mode == "loop":
            print(f"=== Тест loop-устройства ===")
            if not duration:
                duration = (
                    1.0  # Для loop-теста используем короткую длительность по умолчанию
                )
            test_signal = self.generate_test_signal("multi", duration=duration)
        else:
            print(f"=== Стандартный тест аудиокарты ===")
            print(f"Частота дискретизации: {self.sample_rate} Гц")
            print(f"Длительность: {self.duration if not duration else duration} с")
            if not duration:
                duration = self.duration
            test_signal = self.generate_test_signal(
                test_type, frequency, duration=duration
            )
            if test_type == "sine":
                print(f"Генерация синуса {frequency} Гц")
            elif test_type == "sweep":
                print("Генерация sweep 100-8000 Гц")

        # Запуск аудио-стрима и получение записанных данных
        stream_result = self.process_audio_stream(test_signal, duration, mode)

        if stream_result is None:
            print("Ошибка записи!")
            return None

        # Обработка результатов в зависимости от режима
        if mode == "loop":
            success, loop_results = stream_result
            if success:
                print("Loop-тест успешен!")
                self.plot_results(loop_results, mode="loop")
                return True
            else:
                print("Loop-тест не удался!")
                return False
        else:
            # Анализ результатов для обычного режима
            recorded = stream_result
            results = self.analyze_signals(test_signal, recorded)
            self.display_results(results)
            self.plot_results(results)
            return results

    def process_audio_stream(self, test_signal, duration, mode="normal"):
        """Обработка аудио-потока для воспроизведения и записи"""
        # Получаем информацию о каналах
        device_info = sd.query_devices(self.device_id)
        in_channels = min(device_info["max_input_channels"], 2)
        out_channels = min(device_info["max_output_channels"], 2)

        print(f"Устройство: {device_info['name']}")
        print(f"Доступно каналов: вход - {in_channels}, выход - {out_channels}")

        # Массив для записи
        recorded = np.zeros((len(test_signal), in_channels), dtype=np.float32)
        signal_detected = False

        def callback(indata, outdata, frames, time, status):
            if status:
                print(f"Статус аудио: {status}")

            # Отправляем сигнал на все выходные каналы
            if callback.play_idx + frames <= len(test_signal):
                for ch in range(outdata.shape[1]):
                    outdata[:, ch] = test_signal[
                        callback.play_idx : callback.play_idx + frames
                    ]
                callback.play_idx += frames
            else:
                outdata.fill(0)

            # Записываем сигнал со всех входных каналов
            if callback.rec_idx + frames <= len(test_signal):
                recorded[callback.rec_idx : callback.rec_idx + frames, :] = indata
                callback.rec_idx += frames

            # Проверка уровня входного сигнала
            nonlocal signal_detected
            for ch in range(indata.shape[1]):
                ch_level = np.max(np.abs(indata[:, ch]))
                if ch_level > 0.05:
                    signal_detected = True

        callback.play_idx = 0
        callback.rec_idx = 0

        # Запуск аудиопотока
        try:
            with sd.Stream(
                device=self.device_id,
                channels=(in_channels, out_channels),
                samplerate=self.sample_rate,
                callback=callback,
                dtype=np.float32,
            ):
                print(f"Воспроизведение и запись в течение {duration} секунд...")
                sd.sleep(int(duration * 1000) + 500)
        except Exception as e:
            print(f"Ошибка аудио: {e}")
            return None if mode == "normal" else (False, None)

        # Анализ уровней сигнала по каналам
        max_levels = []
        for ch in range(in_channels):
            ch_max = np.max(np.abs(recorded[:, ch]))
            max_levels.append(ch_max)
            print(f"Максимальная амплитуда канал {ch}: {ch_max:.4f}")

        # Проверка наличия сигнала
        if any(level > 0.01 for level in max_levels):
            best_channel = max_levels.index(max(max_levels))
            print(
                f"Используется канал {best_channel} с амплитудой {max_levels[best_channel]:.4f}"
            )

            if mode == "loop":
                # Дополнительная обработка для режима loop
                loop_results = self.process_loop_results(
                    test_signal, recorded[:, best_channel], best_channel
                )
                return True, loop_results
            else:
                # Для обычного режима возвращаем записанные данные с лучшего канала
                return recorded[:, best_channel]
        else:
            # Сигнал не обнаружен
            print(
                "ВНИМАНИЕ! Не обнаружен входной сигнал. Проверьте подключение и настройки!"
            )
            return None if mode == "normal" else (False, None)

    def process_loop_results(self, test_signal, recorded_data, best_channel):
        """Обработка результатов loop-теста"""
        # Спектральный анализ
        print("\nПроверка спектра сигнала...")
        freqs_orig, psd_orig = signal.welch(test_signal, self.sample_rate, nperseg=1024)
        freqs_rec, psd_rec = signal.welch(recorded_data, self.sample_rate, nperseg=1024)

        # Вычисляем корреляцию спектров и другие метрики
        spec_corr = np.corrcoef(psd_orig, psd_rec)[0, 1]
        nrmse = np.sqrt(np.mean((psd_orig - psd_rec) ** 2)) / (
            np.max(psd_orig) - np.min(psd_orig)
        )

        print(f"Корреляция спектров: {spec_corr:.3f}")
        print(f"Нормализованная ошибка (NRMSE): {nrmse:.5f}")

        # Вычисляем разницу амплитуд на тестовых частотах
        for freq, amp in [(500, 0.3), (5000, 0.2), (10000, 0.1)]:
            idx = np.abs(freqs_orig - freq).argmin()
            orig_amp_at_freq = psd_orig[idx]
            rec_amp_at_freq = psd_rec[idx]
            if orig_amp_at_freq > 0:
                rel_diff = (
                    abs(orig_amp_at_freq - rec_amp_at_freq) / orig_amp_at_freq * 100
                )
                print(f"Частота {freq} Гц: разница амплитуд {rel_diff:.1f}%")

        # Обработка нулевых значений в начале сигнала
        processed_data, start_offset = self.trim_leading_zeros(
            recorded_data, threshold=self.threshold
        )

        # Измерение амплитуд и задержки
        orig_amp = np.max(np.abs(test_signal))
        rec_amp = np.max(np.abs(processed_data))
        amp_ratio = (rec_amp / orig_amp) * 100

        correlation = np.correlate(processed_data, test_signal, mode="full")
        delay = np.argmax(correlation) - len(test_signal) + 1
        delay_ms = delay * 1000 / self.sample_rate

        print(f"Амплитуда исходного сигнала: {orig_amp:.4f}")
        print(f"Амплитуда входного сигнала: {rec_amp:.4f}")
        print(f"Соотношение вход/выход: {amp_ratio:.1f}%")
        print(f"Задержка: {delay_ms:.1f} мс ({delay} отсчетов)")

        # Формируем результат
        return {
            "best_channel": best_channel,
            "spectrum_corr": spec_corr,
            "nrmse": nrmse,
            "recorded_data": processed_data,
            "test_signal": test_signal,
            "orig_amplitude": orig_amp,
            "input_amplitude": rec_amp,
            "amplitude_ratio": amp_ratio,
            "delay_samples": delay,
            "delay_ms": delay_ms,
            "start_offset": start_offset,
            "freq_original": (freqs_orig, psd_orig),
            "freq_recorded": (freqs_rec, psd_rec),
        }

    def trim_leading_zeros(self, signal_data, threshold=0.1):
        """Удаление нулевых значений в начале сигнала"""
        start_idx = 0
        for i, sample in enumerate(signal_data):
            if abs(sample) > threshold:
                start_idx = i
                break

        if start_idx > 0:
            print(f"Обнаружены нули в начале сигнала. Обрезаем {start_idx} отсчетов.")
            return signal_data[start_idx:], start_idx
        else:
            return signal_data, 0

    def analyze_signals(self, original, recorded):
        """Анализ и сравнение оригинального и записанного сигналов"""
        results = {}

        # Сохраняем оригинальные сигналы
        results["original_raw"] = original
        results["recorded_raw"] = recorded

        # Сохраняем максимальную амплитуду сигналов
        results["input_amplitude"] = np.max(np.abs(recorded))
        results["original_amplitude"] = np.max(np.abs(original))

        # Обработка нулей в начале сигнала
        recorded_trimmed, start_offset = self.trim_leading_zeros(
            recorded, threshold=self.threshold
        )
        results["start_offset"] = start_offset

        # Выравнивание по времени (компенсация задержки)
        correlation = np.correlate(recorded_trimmed, original, mode="full")
        delay = np.argmax(correlation) - len(original) + 1

        # Сохраняем информацию о задержке
        results["delay_samples"] = delay
        results["delay_ms"] = delay * 1000 / self.sample_rate
        # results["start_offset_ms"] = start_offset * 1000 / self.sample_rate

        # Создание выровненных сигналов
        if abs(delay) < len(recorded_trimmed):
            if delay > 0:
                recorded_aligned = recorded_trimmed[delay:]
                original_aligned = original[: len(recorded_aligned)]
            else:
                recorded_aligned = recorded_trimmed[: len(recorded_trimmed) + delay]
                original_aligned = original[-delay : len(recorded_aligned) - delay]
        else:
            recorded_aligned = recorded_trimmed
            original_aligned = original

        # Обрезка до одинаковой длины
        min_len = min(len(original_aligned), len(recorded_aligned))
        original_aligned = original_aligned[:min_len]
        recorded_aligned = recorded_aligned[:min_len]

        # Вычисляем амплитудный коэффициент для компенсации
        amp_ratio = (
            np.max(np.abs(recorded_aligned)) / np.max(np.abs(original_aligned))
            if np.max(np.abs(original_aligned)) > 0
            else 1.0
        )
        results["amp_ratio"] = amp_ratio

        # Масштабируем записанный сигнал для визуализации
        recorded_aligned_scaled = (
            recorded_aligned * (1.0 / amp_ratio) if amp_ratio > 0 else recorded_aligned
        )

        # Метрики качества
        # SNR (соотношение сигнал/шум)
        signal_power = np.mean(original_aligned**2)
        noise_power = np.mean((recorded_aligned - original_aligned) ** 2)
        results["snr_db"] = 10 * np.log10(signal_power / (noise_power + 1e-10))

        # THD (общие гармонические искажения)
        results["thd_percent"] = np.sqrt(noise_power / signal_power) * 100

        # Корреляция сигналов
        if len(original_aligned) > 0 and len(recorded_aligned) > 0:
            correlation_coef = np.corrcoef(original_aligned, recorded_aligned)[0, 1]
            results["correlation"] = (
                correlation_coef if not np.isnan(correlation_coef) else 0
            )
        else:
            results["correlation"] = 0

        # Частотный анализ
        freqs_orig, psd_orig = signal.welch(
            original_aligned, self.sample_rate, nperseg=1024
        )
        freqs_rec, psd_rec = signal.welch(
            recorded_aligned, self.sample_rate, nperseg=1024
        )

        # Сохраняем результаты
        results["original_aligned"] = original_aligned
        results["recorded_aligned"] = recorded_aligned
        results["recorded_aligned_scaled"] = recorded_aligned_scaled
        results["freq_original"] = (freqs_orig, psd_orig)
        results["freq_recorded"] = (freqs_rec, psd_rec)

        return results

    def display_results(self, results):
        """Вывод результатов тестирования"""
        print("\n=== РЕЗУЛЬТАТЫ ТЕСТИРОВАНИЯ ===")
        print(f"Амплитуда исходного сигнала: {results['original_amplitude']:.4f}")
        print(f"Амплитуда входного сигнала: {results['input_amplitude']:.4f}")
        print(
            f"Соотношение вход/выход: {(results['input_amplitude']/results['original_amplitude']*100):.1f}%"
        )
        print(f"SNR: {results['snr_db']:.1f} дБ")
        print(f"THD: {results['thd_percent']:.2f}%")
        print(f"Корреляция: {results['correlation']:.3f}")
        print(
            f"Задержка: {results['delay_ms']:.1f} мс ({results['delay_samples']} отсчетов)"
        )

        # Оценка качества
        if results["snr_db"] > 40:
            quality = "ОТЛИЧНО"
        elif results["snr_db"] > 30:
            quality = "ХОРОШО"
        elif results["snr_db"] > 20:
            quality = "УДОВЛЕТВОРИТЕЛЬНО"
        else:
            quality = "ПЛОХО"

        print(f"Общая оценка: {quality}")

    def plot_results(self, results, mode="normal"):
        """Визуализация результатов тестирования"""
        fig, axes = plt.subplots(2, 2, figsize=(12, 8))

        # Определяем данные для графиков в зависимости от режима
        if mode == "normal":
            # Данные для обычного режима
            test_signal = results["original_raw"]
            recorded_data = results["recorded_raw"]
            original_aligned = results["original_aligned"]
            recorded_aligned = results["recorded_aligned_scaled"]
            # Метрики для обычного режима
            metrics_text = f"""Амплитуда исх: {results.get('original_amplitude', 0):.4f}
Амплитуда вх: {results.get('input_amplitude', 0):.4f}
Соотн. вх/вых: {(results.get('input_amplitude', 0)/results.get('original_amplitude', 1)*100):.1f}%
Усиление: {results.get('amp_ratio', 1.0):.3f}x
SNR: {results['snr_db']:.1f} дБ
THD: {results['thd_percent']:.2f}%
Корреляция: {results['correlation']:.3f}
Задержка: {results['delay_ms']:.1f} мс
Смещение нач.: {results.get('start_offset', 0)} отсчетов"""
        else:
            # Данные для loop режима
            test_signal = results["test_signal"]
            recorded_data = results["recorded_data"]
            # Дополнительно создаем выровненные данные для второго графика
            if abs(results["delay_samples"]) < len(recorded_data):
                if results["delay_samples"] > 0:
                    recorded_aligned = recorded_data[results["delay_samples"] :]
                    original_aligned = test_signal[: len(recorded_aligned)]
                else:
                    recorded_aligned = recorded_data[
                        : len(recorded_data) + results["delay_samples"]
                    ]
                    original_aligned = test_signal[
                        -results["delay_samples"] : len(recorded_aligned)
                        - results["delay_samples"]
                    ]

                # Обрезка до одинаковой длины
                min_aligned_len = min(len(original_aligned), len(recorded_aligned))
                original_aligned = original_aligned[:min_aligned_len]
                recorded_aligned = recorded_aligned[:min_aligned_len]
            else:
                original_aligned = test_signal
                recorded_aligned = recorded_data[: len(test_signal)]

            # Метрики для loop-режима
            metrics_text = f"""Корреляция спектров: {results['spectrum_corr']:.3f}
Нормализованная ошибка: {results['nrmse']:.5f}
Амплитуда исх: {results['orig_amplitude']:.4f}
Амплитуда вх: {results['input_amplitude']:.4f}
Соотн. вх/вых: {results['amplitude_ratio']:.1f}%
Смещение нач.: {results.get('start_offset', 0)} отсчетов
Задержка: {results['delay_ms']:.1f} мс
Используемый канал: {results['best_channel']}"""

        # Общий код для обоих режимов:

        # 1. График исходных сигналов с учетом смещения
        start_offset = results.get("start_offset", 0)
        delay_samples = results.get("delay_samples", 0)
        total_offset = start_offset + delay_samples  # Общее смещение между сигналами

        # Конвертируем временные шкалы из секунд в миллисекунды
        t_orig = np.linspace(0, len(test_signal) / self.sample_rate * 1000, len(test_signal))
        t_rec = np.linspace(0, len(recorded_data) / self.sample_rate * 1000, len(recorded_data))

        samples_to_show = min(
            1000 if mode == "loop" else 5000, len(test_signal), len(recorded_data)
        )

        # Отрисовка с правильным смещением, время в миллисекундах
        axes[0, 0].plot(
            t_orig[:samples_to_show],
            test_signal[:samples_to_show],
            "b-",
            alpha=0.7,
            label="Исходный",
        )
        axes[0, 0].plot(
            t_orig[:samples_to_show] + total_offset/self.sample_rate * 1000,  # Сдвигаем график и переводим в мс
            recorded_data[:samples_to_show],
            "r-",
            alpha=0.7,
            label="Записанный",
        )

        # Отображение задержки стрелкой
        total_delay_ms = total_offset * 1000 / self.sample_rate
        axes[0, 0].annotate(
            "",
            xy=(total_offset / self.sample_rate * 1000, 0),  # Переводим в мс
            xytext=(0, 0),
            arrowprops=dict(arrowstyle="<->", color="green", lw=2),
        )
        axes[0, 0].text(
            total_offset / (2 * self.sample_rate) * 1000,  # Переводим в мс
            0.05,
            f"Задержка: {total_delay_ms:.1f} мс",
            color="green",
            ha="center",
        )

        axes[0, 0].set_title("Исходные сигналы с задержкой")
        axes[0, 0].set_xlabel("Время (мс)")  # Изменена единица измерения
        axes[0, 0].set_ylabel("Амплитуда")
        axes[0, 0].legend()
        axes[0, 0].grid(True)

        # 2. График выровненных сигналов
        if len(original_aligned) > 0 and len(recorded_aligned) > 0:
            t_aligned = np.linspace(
                0, len(original_aligned) / self.sample_rate * 1000, len(original_aligned)  # Переводим в мс
            )
            samples_to_show = min(1000, len(original_aligned))

            axes[1, 0].plot(
                t_aligned[:samples_to_show],
                original_aligned[:samples_to_show],
                "b-",
                label="Оригинал",
            )
            axes[1, 0].plot(
                t_aligned[:samples_to_show],
                recorded_aligned[:samples_to_show],
                "r-",
                label="Записанный",
            )
            axes[1, 0].set_title(
                "Выровненные сигналы" + (" с компенсацией" if mode == "normal" else "")
            )
            axes[1, 0].set_xlabel("Время (мс)")  # Изменена единица измерения
            axes[1, 0].set_ylabel("Амплитуда")
            axes[1, 0].legend()
            axes[1, 0].grid(True)
        else:
            # Если нет выровненных данных, показываем спектрограмму
            f, t_spec, Sxx = signal.spectrogram(test_signal, self.sample_rate)
            axes[1, 0].pcolormesh(t_spec * 1000, f, 10 * np.log10(Sxx), shading="gouraud")  # t_spec в мс
            axes[1, 0].set_title("Спектрограмма исходного сигнала")
            axes[1, 0].set_ylabel("Частота [Гц]")
            axes[1, 0].set_xlabel("Время [мс]")  # Изменена единица измерения

        # 3. Спектры мощности
        freqs_orig, psd_orig = (
            results["freq_original"]
            if "freq_original" in results
            else signal.welch(test_signal, self.sample_rate, nperseg=1024)
        )
        freqs_rec, psd_rec = (
            results["freq_recorded"]
            if "freq_recorded" in results
            else signal.welch(recorded_data, self.sample_rate, nperseg=1024)
        )

        axes[0, 1].semilogy(freqs_orig, psd_orig, "b-", label="Оригинал")
        axes[0, 1].semilogy(freqs_rec, psd_rec, "r-", label="Записанный")
        axes[0, 1].set_title("Спектральная плотность мощности")
        axes[0, 1].set_xlabel("Частота (Гц)")
        axes[0, 1].set_ylabel("PSD")
        axes[0, 1].legend()
        axes[0, 1].grid(True)

        # 4. Метрики
        axes[1, 1].text(
            0.1,
            0.5,
            metrics_text,
            fontsize=12,
            verticalalignment="center",
            fontfamily="monospace",
        )
        axes[1, 1].set_title("Метрики качества")
        axes[1, 1].axis("off")

        plt.tight_layout()
        plt.show()


def main():
    parser = argparse.ArgumentParser(description="Тестирование аудиокарты")
    parser.add_argument(
        "--device", "-d", type=str, help="Имя или индекс аудиоустройства"
    )
    parser.add_argument(
        "--frequency",
        "-f",
        type=int,
        default=1000,
        help="Частота тестового сигнала (Гц)",
    )
    parser.add_argument(
        "--duration", "-t", type=float, default=2.0, help="Длительность теста (с)"
    )
    parser.add_argument(
        "--test-type",
        "-m",
        choices=["sine", "sweep", "multi"],
        default="sine",
        help="Тип теста",
    )
    parser.add_argument(
        "--sample-rate", "-s", type=int, default=48000, help="Частота дискретизации"
    )
    parser.add_argument(
        "--input-channel", "-i", type=int, default=0, help="Индекс входного канала"
    )
    parser.add_argument(
        "--output-channel", "-o", type=int, default=0, help="Индекс выходного канала"
    )
    parser.add_argument(
        "--test-loop", "-l", action="store_true", help="Тест режима loopback"
    )
    parser.add_argument(
        "--gain", "-g", type=float, default=1.0, help="Коэффициент усиления"
    )

    args = parser.parse_args()

    tester = AudioTester(
        device_name=args.device,
        sample_rate=args.sample_rate,
        duration=args.duration,
        input_channel=args.input_channel,
        output_channel=args.output_channel,
        gain=args.gain,
    )

    # Запуск теста в соответствующем режиме
    mode = "loop" if args.test_loop else "normal"
    result = tester.run_audio_test(
        test_type=args.test_type,
        frequency=args.frequency,
        mode=mode,
        duration=args.duration,
    )

    if result:
        print("\nТест завершен успешно!")
    else:
        print("\nТест не удался!")


if __name__ == "__main__":
    main()

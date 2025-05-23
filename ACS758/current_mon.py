import machine
from machine import Pin, ADC
from pimoroni import RGBLED
from picographics import PicoGraphics, DISPLAY_PICO_DISPLAY
from sys import exit
import gc
from RP2040ADC import Rp2040AdcDmaAveraging
import array  
import utime
import time
import asyncio

# Dictionary of scaling factors for ACS758
# Key: "<current><direction>", where U - uni-directional (1), B - bi-directional (0)
ACS758 = {
    "50B": {"scale": 40, "offset": 2.5, "scale3": 20, "offset3": 1.65, "dir": 0},
    "50U": {"scale": 60, "offset": 0.6, "scale3": 30, "offset3": 0.36, "dir": 1},
    "100B": {"scale": 20, "offset": 2.5, "scale3": 10, "offset3": 1.65, "dir": 0},
    "100U": {"scale": 40, "offset": 0.6, "scale3": 20, "offset3": 0.36, "dir": 1},
    "150B": {"scale": 13.3, "offset": 2.6, "scale3": 6.65, "offset3": 1.65, "dir": 0},
    "150U": {"scale": 26.7, "offset": 0.6, "scale3": 13.35, "offset3": 0.36, "dir": 1},
    "200B": {"scale": 10, "offset": 2.5, "scale3": 5, "offset3": 1.65, "dir": 0},
    "200U": {"scale": 20, "offset": 0.6, "scale3": 10, "offset3": 0.36, "dir": 1},
}

# Set up the display and drawing constants
display = PicoGraphics(display=DISPLAY_PICO_DISPLAY, rotate=0)
display.set_backlight(0.5)

WHITE = display.create_pen(255, 255, 255)
BLACK = display.create_pen(0, 0, 0)
CYAN = display.create_pen(0, 255, 255)
MAGENTA = display.create_pen(255, 0, 255)
YELLOW = display.create_pen(255, 255, 0)
GREEN = display.create_pen(0, 255, 0)
RED = display.create_pen(255, 0, 0)
BLUE = display.create_pen(0, 0, 255)

WIDTH, HEIGHT = display.get_bounds()

# Button configuration
button_a = Pin(12, Pin.IN, Pin.PULL_UP)
button_b = Pin(13, Pin.IN, Pin.PULL_UP)
button_x = Pin(14, Pin.IN, Pin.PULL_UP)
button_y = Pin(15, Pin.IN, Pin.PULL_UP)

# Enable signal pin
enable = Pin(22, Pin.OUT)
enable.value(0)  # Set to low initially

PIN_IN = 28

# ADC configuration for current sensor
sensor_temp = machine.ADC(2)
power_ACS758 = 5.0
R1 = 1600
R2 = 3200
Divider = R2 / (R1 + R2)  # Voltage divider ratio

# Set up the RGB LED
led = RGBLED(6, 7, 8)

conversion_factor = 3.3 / 4095

# DMA settings optimized for 100µs signal capture
ADC_SAMPLE_TIME_US = 2  # Примерно 2 мкс на отсчёт при DIV_REG=0 (500 кГц)
CAPTURE_DEPTH = 64      # Например, 64 отсчёта (128 мкс захвата)
SAMPLE_BUFFER_SIZE = CAPTURE_DEPTH
TRIGGER_THRESHOLD = 0.1  # Voltage threshold to detect signal start

def calc_timer_period_ms(capture_depth, sample_time_us):
    # Минимальный период таймера в миллисекундах (с запасом +10%)
    min_period_us = capture_depth * sample_time_us * 1.1
    return max(1, int(min_period_us // 1000 + (min_period_us % 1000 > 0)))

TIMER_PERIOD_MS = calc_timer_period_ms(CAPTURE_DEPTH, ADC_SAMPLE_TIME_US)

# State variables
current_value = 0.0
max_current = 0.0
button_y_state = False  # Track the state of button_y (True for STOP, False for START)
state_error = False  # Track if the state is in error
sampling_active = False
actual_sampling_rate = 0.0
signal_detected = False
signal_start_time = 0
signal_duration = 0

dma_active = False

# Function for setting LED color based on current
def current_to_color(current):
    if current < 0.1:
        return (0, 0, 80)  # Blue for low current
    elif current < 5.0:
        return (0, 80, 0)  # Green for normal current
    elif current < 10.0:
        return (80, 80, 0)  # Yellow for elevated current
    else:
        return (80, 0, 0)  # Red for high current

def calculate_current(adc_value, sensor_type, power_ACS758, divider=1.0):

    params = ACS758[sensor_type]
    if power_ACS758 == 3.3:
        offset = params["offset3"] * divider
        scale = params["scale3"] * divider
    else:
        offset = params["offset"] * divider
        scale = params["scale"] * divider
    current = (adc_value - offset) * (scale/1000)
    if current < 0:
        current = -1 * current
    return current

def capture_current_dma():
    """Function for capturing 100µs current pulses using DMA with maximum speed"""
    global current_value, max_current, dma_active, signal_detected, signal_duration

    if not dma_active:
        try:
            adc_dma.capture_start()
            dma_active = True
        except Exception as e:
            print(f"DMA start error: {e}")
            dma_active = False
        return 0

    try:
        if adc_dma.is_done():
            adc_value = adc_dma.wait_and_read_average_u12()
            dma_active = False

            voltage = adc_value * conversion_factor
            current_value = calculate_current(voltage, "100U", power_ACS758, Divider)
            if current_value > max_current:
                max_current = current_value

            # Просто сохраняем длительность захвата (для других целей)
            signal_duration = SAMPLE_BUFFER_SIZE * ADC_SAMPLE_TIME_US

            # Флаг обнаружения сигнала
            signal_detected = True if current_value > TRIGGER_THRESHOLD else False

            return 0  # actual_sampling_rate больше не вычисляется здесь
        else:
            return 0
    except Exception as e:
        print(f"DMA read error: {e}")
        dma_active = False
        return 0

# Для анимации колесика
spinner_phase = 0
spinner_chars = ['|', '/', '-', '\\']
spinner_colors = [CYAN, YELLOW, MAGENTA, GREEN]

def update_display():
    """Update screen with layout for 240x135 display"""
    global current_value, max_current, button_y_state, state_error, actual_sampling_rate
    global signal_detected, signal_duration
    
    # Clear the screen
    display.set_pen(BLACK)
    display.clear()
    
    # --- Top row: status ---
    status_y = 5
    # Left side - operation status
    display.set_pen(GREEN if button_y_state else RED)
    status_text = "ACTIVE" if button_y_state else "STOP"
    display.text(status_text, 5, status_y, WIDTH, 2)
    
    # Right side - signal status
    display.set_pen(GREEN if signal_detected else BLUE)
    signal_status = "SIGNAL" if signal_detected else ""  # убираем READY
    if signal_status:
        display.text(signal_status, WIDTH - 75, status_y, WIDTH, 2)
    
    # --- Заголовки с подписями current и max ---
    display.set_pen(CYAN)
    display.text("CURRENT", 10, 30, WIDTH, 2)
    display.text("MAX", 130, 30, WIDTH, 2)
    
    # --- Значения тока и максимального тока ---
    display.set_pen(WHITE)
    current_str = f"{current_value:.1f}"
    display.text(current_str, 10, 55, WIDTH, 3)
    
    display.set_pen(MAGENTA)
    peak_str = f"{max_current:.1f}"
    display.text(peak_str, 130, 55, WIDTH, 3)
    
    # --- Техническая информация ---
    display.set_pen(YELLOW)
    tech_y = 90
    # Показываем фиксированную частоту 500 kHz
    rate_info = "500.0 kHz"
    display.text(rate_info, 10, tech_y, WIDTH, 2)
    
    # Error information, if any
    if state_error:
        display.set_pen(RED)
        display.text("ERROR", WIDTH // 2 - 40, tech_y, WIDTH, 2)
    
    # --- Button instructions ---
    display.set_pen(CYAN)
    display.text("Reset", 5, HEIGHT - 15, WIDTH, 1)
    display.text("Start/Stop", WIDTH - 80, HEIGHT - 15, WIDTH, 1)

    # --- Анимированный индикатор работы программы ---
    global spinner_phase
    spinner_y = HEIGHT - 32  # чуть выше надписи кнопки
    spinner_x = WIDTH - 18
    spinner_char = spinner_chars[spinner_phase % len(spinner_chars)]
    spinner_color = spinner_colors[spinner_phase % len(spinner_colors)]
    display.set_pen(spinner_color)
    display.text(spinner_char, spinner_x, spinner_y, WIDTH, 2)

    # Update display
    display.update()
    
    # Set LED color based on current
    led.set_rgb(*current_to_color(current_value))

def read_buttons():
    """Process button presses"""
    global button_y_state, state_error, max_current, dma_active, current_value

    # Button Y - start/stop 
    if button_y.value() == 0:  # Button pressed
        utime.sleep_ms(200)  # Debounce
        button_y_state = not button_y_state  # Toggle state
        enable.value(1 if button_y_state else 0)  # Enable/disable signal
        state_error = False  # Reset error on state change

        # Stop DMA if stopping measurement
        if not button_y_state and dma_active:
            # Дождаться завершения DMA, если он активен
            timeout = utime.ticks_ms() + 100  # максимум 100 мс ожидания
            while dma_active and not adc_dma.is_done():
                if utime.ticks_ms() > timeout:
                    print("DMA stop timeout!")
                    break
                utime.sleep_ms(1)
            dma_active = False

    # Button B - reset maximum value
    if button_b.value() == 0:  # Button pressed
        utime.sleep_ms(200)  # Debounce
        max_current = 0.0  # Reset maximum value
        current_value = 0.0
        
adc_dma = Rp2040AdcDmaAveraging(gpio_pin=PIN_IN, dma_chan=1, adc_samples=SAMPLE_BUFFER_SIZE)

async def adc_dma_task():
    while True:
        if button_y_state:
            capture_current_dma()
        await asyncio.sleep_ms(TIMER_PERIOD_MS)

async def main():
    global spinner_phase
    loop = asyncio.get_event_loop()
    loop.create_task(adc_dma_task())
    while True:
        read_buttons()
        update_display()
        spinner_phase = (spinner_phase + 1) % 1000  # увеличиваем фазу для анимации
        await asyncio.sleep(0.1)  # корректно внутри async def

try:
    asyncio.run(main())
except KeyboardInterrupt:
    print("Program terminated.")
    enable.value(0)
    dma_active = False
    raise SystemExit
import machine
from machine import Pin, ADC
from pimoroni_bus import SPIBus
from pimoroni import RGBLED
from picographics import PicoGraphics, DISPLAY_PICO_DISPLAY
from sys import exit
import gc
from RP2040ADC import Rp2040AdcDmaAveraging
import array
import utime
import time
import _thread

# Dictionary of scaling factors for ACS758
# Key: "<current><direction>", where U - uni-directional (1), B - bi-directional (0)
ACS758 = {
    "50B": {"scale": 0.040, "offset": 2.5, "scale3": 0.020, "offset3": 1.65, "dir": 0},
    "50U": {"scale": 0.060, "offset": 0.6, "scale3": 0.030, "offset3": 0.36, "dir": 1},
    "100B": {"scale": 0.020, "offset": 2.5, "scale3": 0.010, "offset3": 1.65, "dir": 0},
    "100U": {"scale": 0.040, "offset": 0.6, "scale3": 0.020, "offset3": 0.36, "dir": 1},
    "150B": {"scale": 0.0133, "offset": 2.6, "scale3": 0.00665, "offset3": 1.65, "dir": 0},
    "150U": {"scale": 0.0267, "offset": 0.6, "scale3": 0.01335, "offset3": 0.36, "dir": 1},
    "200B": {"scale": 0.010, "offset": 2.5, "scale3": 0.005, "offset3": 1.65, "dir": 0},
    "200U": {"scale": 0.020, "offset": 0.6, "scale3": 0.010, "offset3": 0.36, "dir": 1},
}

try:
    import machine

    spi = machine.SPI(
        0,
        baudrate=62_500_000,
        polarity=1,
        phase=1,
        bits=8,
        firstbit=machine.SPI.MSB,
        sck=machine.Pin(18),
        mosi=machine.Pin(19),
    )
except Exception as e:
    print("SPI speed setup skipped:", e)

display = PicoGraphics(display=DISPLAY_PICO_DISPLAY, bus=spi, rotate=0)
display.set_backlight(0.9)

WHITE = display.create_pen(255, 255, 255)
BLACK = display.create_pen(0, 0, 0)
CYAN = display.create_pen(0, 255, 255)
MAGENTA = display.create_pen(255, 0, 255)
YELLOW = display.create_pen(255, 255, 0)
GREEN = display.create_pen(0, 255, 0)
RED = display.create_pen(255, 0, 0)
BLUE = display.create_pen(0, 0, 255)

WIDTH, HEIGHT = display.get_bounds()


button_a = Pin(12, Pin.IN, Pin.PULL_UP)
button_b = Pin(13, Pin.IN, Pin.PULL_UP)
button_x = Pin(14, Pin.IN, Pin.PULL_UP)
button_y = Pin(15, Pin.IN, Pin.PULL_UP)


enable = Pin(22, Pin.OUT)
enable.value(0)

PIN_IN = 28

power_ACS758 = 5.0
R1 = 1600
R2 = 3190
Divider = R2 / (R1 + R2)


led = RGBLED(6, 7, 8)

conversion_factor = 3.3 / 4095

# DMA settings optimized for 100µs signal capture
ADC_SAMPLE_TIME_US = 2  # About 2 μs per sample with DIV_REG=0 (500 kHz)
CAPTURE_DEPTH = 64      # For example, 64 samples (128 μs capture)
SAMPLE_BUFFER_SIZE = CAPTURE_DEPTH
TRIGGER_THRESHOLD = 0.4  # Voltage threshold to detect signal start


def calc_timer_period_ms(capture_depth, sample_time_us):
    # Minimum timer period in milliseconds (with 10% margin)
    min_period_us = capture_depth * sample_time_us * 1.1
    return max(1, int(min_period_us // 1000 + (min_period_us % 1000 > 0)))


TIMER_PERIOD_MS = calc_timer_period_ms(CAPTURE_DEPTH, ADC_SAMPLE_TIME_US)


current_value = 0.0
max_current = 0.0
button_y_state = False
state_error = False
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
        offset = params["offset3"]
        scale = params["scale3"]
    else:
        offset = params["offset"]
        scale = params["scale"]
    adc_value = adc_value/divider
    # print(f"ADC Value: {adc_value}, Offset: {offset}, Scale: {scale},Divider: {divider}")
    current = (adc_value - offset) / scale
    if current < 0:
        current = -1 * current
    return current


# Data structures for exchange between cores
shared_data = {
    "current_value": 0.0,
    "max_current": 0.0,
    "signal_detected": False,
    "signal_duration": 0,
}
data_lock = _thread.allocate_lock()  # Mutex for shared_data access
dma_thread_running = True  # Flag for safely stopping the DMA thread


def capture_current_dma():
    """Function for capturing current pulses using DMA with maximum speed"""
    global dma_active

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
            val = calculate_current(voltage, "100U", power_ACS758, Divider)
            # val = voltage / Divider
            duration = SAMPLE_BUFFER_SIZE * ADC_SAMPLE_TIME_US
            is_signal = val > TRIGGER_THRESHOLD


            with data_lock:
                shared_data["current_value"] = val
                if val > shared_data["max_current"]:
                    shared_data["max_current"] = val
                shared_data["signal_detected"] = is_signal
                shared_data["signal_duration"] = duration

            return 0
        else:
            return 0
    except Exception as e:
        print(f"DMA read error: {e}")
        dma_active = False
        return 0


# For spinner animation
spinner_phase = 0
spinner_chars = ["|", "/", "-", "\\"]
spinner_colors = [CYAN, YELLOW, MAGENTA, GREEN]

# Adding separate lock for spinner and data cache
spinner_lock = _thread.allocate_lock()
cached_data = {
    "current_value": 0.0,
    "max_current": 0.0,
    "signal_detected": False,
}
data_changed = False


def update_display():
    """Update screen with layout for 240x135 display"""
    global spinner_phase, cached_data, data_changed

    with data_lock:
        if (shared_data["current_value"] != cached_data["current_value"] or
            shared_data["max_current"] != cached_data["max_current"] or
            shared_data["signal_detected"] != cached_data["signal_detected"]):
            
            cached_data["current_value"] = shared_data["current_value"]
            cached_data["max_current"] = shared_data["max_current"] 
            cached_data["signal_detected"] = shared_data["signal_detected"]
            data_changed = True

    with spinner_lock:
        current_spinner_phase = spinner_phase

    if data_changed or current_spinner_phase % 2 == 0:
        # Use local variables from cache
        current_value = cached_data["current_value"]
        max_current = cached_data["max_current"]
        signal_detected = cached_data["signal_detected"]
        
        # Clear the screen
        display.set_pen(BLACK)
        display.clear()

        # --- Top row: status ---
        status_y = 5
        # Left side - operation status
        display.set_pen(GREEN)
        status_text = "ACTIVE"  # DMA is always active
        display.text(status_text, 5, status_y, WIDTH, 2)

        # Right side - signal status
        display.set_pen(GREEN if signal_detected else BLUE)
        signal_status = "SIGNAL" if signal_detected else ""  # removing READY
        if signal_status:
            display.text(signal_status, WIDTH - 75, status_y, WIDTH, 2)

        # --- Заголовки с подписями current и max ---
        display.set_pen(CYAN)
        display.text("CURRENT", 10, 30, WIDTH, 2)
        display.text("MAX", 130, 30, WIDTH, 2)

        # --- Значения тока и максимального тока ---
        display.set_pen(WHITE)
        current_str = f"{current_value:.3f}"
        display.text(current_str, 10, 55, WIDTH, 3)

        display.set_pen(MAGENTA)
        peak_str = f"{max_current:.3f}"
        display.text(peak_str, 130, 55, WIDTH, 3)

        # --- Техническая информация ---
        display.set_pen(YELLOW)
        tech_y = 90
        # Show fixed frequency 500 kHz
        rate_info = "500.0 kHz"
        display.text(rate_info, 10, tech_y, WIDTH, 2)

        # Error information, if any
        if state_error:
            display.set_pen(RED)
            display.text("ERROR", WIDTH // 2 - 40, tech_y, WIDTH, 2)

        # --- Button instructions ---
        display.set_pen(CYAN)
        display.text("Reset", 5, HEIGHT - 15, WIDTH, 1)  # Reset button only

        # --- Animated indicator for program operation ---
        spinner_y = HEIGHT - 32  # slightly above the button label
        spinner_x = WIDTH - 18
        spinner_char = spinner_chars[current_spinner_phase % len(spinner_chars)]
        spinner_color = spinner_colors[current_spinner_phase % len(spinner_colors)]
        display.set_pen(spinner_color)
        display.text(spinner_char, spinner_x, spinner_y, WIDTH, 2)
        
        # Update display
        display.update()
        
        # Set LED color based on current
        led.set_rgb(*current_to_color(current_value))
        
        # Reset data change flag after update
        data_changed = False


def read_buttons():
    """Process button presses"""
    # Button B - reset maximum value
    if button_b.value() == 0:  # Button pressed
        utime.sleep_ms(200)  # Debounce
        # Safely reset maximum value through lock
        with data_lock:
            shared_data["max_current"] = 0.0
            shared_data["signal_detected"] = False
            shared_data["current_value"] = 0.0


adc_dma = Rp2040AdcDmaAveraging(
    gpio_pin=PIN_IN, dma_chan=1, adc_samples=SAMPLE_BUFFER_SIZE
)


def dma_core1_loop():
    while dma_thread_running:
        capture_current_dma()
        utime.sleep_ms(TIMER_PERIOD_MS)


USE_TWO_CORES = True
DISPLAY_UPDATE_MIN_MS = 40 if USE_TWO_CORES else 20

def main():
    global spinner_phase
    last_display_update = utime.ticks_ms()

    if USE_TWO_CORES:
        _thread.start_new_thread(dma_core1_loop, ())

    try:
        while True:
            read_buttons()
            if not USE_TWO_CORES:
                capture_current_dma()
            now = utime.ticks_ms()
            if utime.ticks_diff(now, last_display_update) >= DISPLAY_UPDATE_MIN_MS:
                update_display()
                with spinner_lock:
                    spinner_phase = (spinner_phase + 1) % len(spinner_chars)
                last_display_update = now
            else:
                utime.sleep_ms(1)
    except KeyboardInterrupt:
        print("Program terminated.")
        enable.value(0)
        global dma_thread_running
        dma_thread_running = False
        raise SystemExit

if __name__ == "__main__":
    main()

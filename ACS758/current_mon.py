import machine
from machine import Pin, ADC, Timer, mem32
import time
from pimoroni import RGBLED
from picographics import PicoGraphics, DISPLAY_PICO_DISPLAY
import utime
from sys import exit
import gc

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

# ADC configuration for current sensor
sensor_temp = machine.ADC(2)
power_ACS758 = 5.0
R1 = 1600
R2 = 3200
Divider = R2 / (R1 + R2)  # Voltage divider ratio

# Set up the RGB LED
led = RGBLED(6, 7, 8)

conversion_factor = 3.3 / (65535)

# DMA settings for fast scanning
CAPTURE_DEPTH = 1000  # Capture depth for 50µs signal
SAMPLE_BUFFER_SIZE = CAPTURE_DEPTH
SAMPLING_RATE_HZ = 500000

current_value = 0.0
max_current = 0.0
button_y_state = False  # Track the state of button_y (True for STOP, False for START)
state_error = False  # Track if the state is in error
sampling_active = False

capture_buffer = []


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

    current = (adc_value - offset) * (scale / 1000)

    if current < 0:
        current = -1 * current
    return current

filter_buffer = []

def filter_adc_value(new_value, window=5):
    global filter_buffer
    filter_buffer.append(new_value)
    if len(filter_buffer) > window:
        filter_buffer.pop(0)
    return sum(filter_buffer) / len(filter_buffer)

def capture_current():
    """Function for capturing current with maximum scanning speed"""
    global capture_buffer, current_value, max_current, Divider, power_ACS758

    capture_buffer.clear()
    gc.collect()  

    start_time = utime.ticks_us()
    for _ in range(SAMPLE_BUFFER_SIZE):
        raw = sensor_temp.read_u16() * conversion_factor
        capture_buffer.append(raw)
    end_time = utime.ticks_us()

    # for raw in capture_buffer:
    #     filtered_value = filter_adc_value(raw)
    #     filter_buffer.append(filtered_value)

    if capture_buffer:
        max_raw = max(capture_buffer)
        max_current_sample = calculate_current(max_raw, "100U", power_ACS758, Divider)

        if max_current_sample > max_current:
            max_current = max_current_sample

        sum_of_squares = 0
        for raw_value in capture_buffer:
            sum_of_squares += raw_value**2

        # RMS = sqrt(average of squared values)
        if len(capture_buffer) > 0:
            current_value = (sum_of_squares / len(capture_buffer)) ** 0.5
            current_value = calculate_current(current_value, "100U", power_ACS758, Divider)

    # Calculate actual sampling rate
    capture_duration = utime.ticks_diff(end_time, start_time) / 1000000
    actual_rate = SAMPLE_BUFFER_SIZE / capture_duration if capture_duration > 0 else 0

    return actual_rate


def update_display():
    """Update screen with current and maximum values"""
    global current_value, max_current, button_y_state, state_error

    display.set_pen(BLACK)
    display.clear()

    # Display current value in large font
    display.set_pen(WHITE)
    display.text(f"Current:", 10, 10, WIDTH, 2)
    display.text(f"{current_value:.1f}", 10, 35, WIDTH, 4)

    # Display maximum current value - moved to right side
    display.set_pen(MAGENTA)
    display.text(f"Max:", WIDTH // 2 + 10, 10, WIDTH, 2)
    display.text(f"{max_current:.1f}", WIDTH // 2 + 10, 35, WIDTH, 4)

    # Operation status - moved up
    display.set_pen(GREEN if button_y_state else RED)
    status_text = "RUNNING" if button_y_state else "STOPPED"
    display.text(f"Status: {status_text}", 10, 80, WIDTH, 2)

    # Error information, if any
    if state_error:
        display.set_pen(RED)
        display.text("ERROR", WIDTH // 2 - 40, 130, WIDTH, 3)

    # Button instructions positioned at bottom with more space
    display.set_pen(CYAN)
    # Left side - B button
    display.text("Reset Max", 10, HEIGHT - 30, WIDTH, 2)
    # Right side - Y button
    display.text("Start/Stop", WIDTH - 125, HEIGHT - 30, WIDTH, 2)

    # Update display
    display.update()

    # Set LED color based on current
    led.set_rgb(*current_to_color(current_value))


def read_buttons():
    global button_y_state, state_error, max_current, current_value

    # Button Y - start/stop
    if button_y.value() == 0:  # Button pressed
        utime.sleep_ms(200)  # Debounce
        button_y_state = not button_y_state  # Toggle state
        enable.value(1 if button_y_state else 0)  # Enable/disable signal
        state_error = False  # Reset error on state change

    # Button B - reset maximum value (changed from B to A)
    if button_b.value() == 0:  # Button pressed (changed from button_b to button_a)
        utime.sleep_ms(200)  # Debounce
        max_current = 0.0  # Reset maximum value
        current_value = 0.0


# Main program loop
print("Program started. Press 'Ctrl+C' to exit.")

try:
    while True:
        # Read button states
        read_buttons()

        if button_y_state:
            actual_rate = capture_current()
            # print(f"Actual sampling rate: {actual_rate:.2f} Hz")
        update_display()
        utime.sleep_ms(1)

except KeyboardInterrupt:
    print("Program terminated.")
    enable.value(0)  # Disable signal on exit
    raise SystemExit

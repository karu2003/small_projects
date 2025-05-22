import machine
from machine import Pin
import time
from pimoroni import RGBLED
from picographics import PicoGraphics, DISPLAY_PICO_DISPLAY
import utime
from machine import Timer
import sys  # Add for handling KeyboardInterrupt
from sys import exit
import os  # For checking the stop command
import utime

# Dictionary of scaling factors for ACS758
# Key: "<current><direction>", where U - uni-directional (1), B - bi-directional (0)
ACS758 = {
    "50B": {"scale": 40, "offset": 2.5, "scale3": 20, "offset3": 1.65, "dir": 0},  # *
    "50U": {"scale": 60, "offset": 0.6, "scale3": 30, "offset3": 0.36, "dir": 1},
    "100B": {"scale": 20, "offset": 2.5, "scale3": 10, "offset3": 1.65, "dir": 0},  # *
    "100U": {"scale": 40, "offset": 0.6, "scale3": 20, "offset3": 0.36, "dir": 1},  # *
    "150B": {"scale": 13.3, "offset": 2.6, "scale3": 6.65, "offset3": 1.65, "dir": 0},
    "150U": {"scale": 26.7, "offset": 0.6, "scale3": 13.35, "offset3": 0.36, "dir": 1},
    "200B": {"scale": 10, "offset": 2.5, "scale3": 5, "offset3": 1.65, "dir": 0},
    "200U": {"scale": 20, "offset": 0.6, "scale3": 10, "offset3": 0.36, "dir": 1},
}

# Set up the display and drawing constants
display = PicoGraphics(display=DISPLAY_PICO_DISPLAY, rotate=0)

# Set the display backlight to 50%
display.set_backlight(0.5)

WHITE = display.create_pen(255, 255, 255)
BLACK = display.create_pen(0, 0, 0)
CYAN = display.create_pen(0, 255, 255)
MAGENTA = display.create_pen(255, 0, 255)
YELLOW = display.create_pen(255, 255, 0)
GREEN = display.create_pen(0, 255, 0)

WIDTH, HEIGHT = display.get_bounds()

DIS_MAP = 1.0  # Display map for the Pico Display Pack 3.3

button_a = Pin(12, Pin.IN, Pin.PULL_UP)
button_b = Pin(13, Pin.IN, Pin.PULL_UP)
button_x = Pin(14, Pin.IN, Pin.PULL_UP)
button_y = Pin(15, Pin.IN, Pin.PULL_UP)

enable = Pin(22, Pin.OUT)  # Pin for enabling/disabling the signal PAD 29
enable.value(0)  # Set to low initially

sensor_temp = machine.ADC(2)
power_ACS758 = 5.0
R1 = 1600
R2 = 3140
Devider = R2 / (R1 + R2)  # Voltage divider ratio

# Set up the RGB LED for Display Pack and Display Pack 2.0
led = RGBLED(6, 7, 8)

# Used for calculating a temperature from the raw sensor reading
conversion_factor = 3.3 / (65535)

temp_min = 0.05
temp_max = 1

voltages = []

colors = [(0, 0, 255), (0, 255, 0), (255, 255, 0), (255, 0, 0)]

HORIZONTAL_SWEEP = 1  # Horizontal sweep duration in seconds
BUFFER_MULTIPLIER = 1  # Increase buffer size to be larger than the screen width
BUFFER_SIZE = WIDTH * BUFFER_MULTIPLIER  # Buffer size is now larger than screen width
RATE = BUFFER_SIZE / HORIZONTAL_SWEEP  # Sampling rate in Hz
DISPLAY_RATE = 25  # Display update rate in Hz
SAMPLES_PER_FRAME = int(RATE / DISPLAY_RATE)  # Number of new samples per display update

SYNC_LEVEL = 0.4  # Synchronization level in volts
SYNC_LEVEL_H = 0.05  # Synchronization level in volts
sync_offset = 3  # Offset for moving sync point horizontally

adc_buffer = []  # List to store ADC values
time_buffer = []  # List to store timestamps

adc_timer = Timer()  # Ensure adc_timer is defined before use
display_timer = Timer()  # Ensure display_timer is defined before use

# Frequency of sampling (calculated)
sampling_rate = RATE  # Hz
time_per_sample = 1 / sampling_rate  # Time between samples (in seconds)

# Variable to store the time between crossings
last_sync_time = None
sync_time_diff = None
sync_cross_count = 0
sync_below = True  # Track if the signal has dropped below SYNC_LEVEL
time_update = False
time_diff = 0.0

dif_error = 2.0  # Variable to track the difference in time between crossings

button_y_state = False  # Track the state of button_y (True for STOP, False for START)
state_error = False  # Track if the state is in error

max_voltage = 0.0  # Variable to track the maximum voltage
# Timestamp of the last update to max_voltage
max_voltage_last_update = utime.ticks_ms()
max_voltage_timeout = 5000  # Timeout in milliseconds for max_voltage

last_sync_cross_count = 0  # Track the last value of sync_cross_count


def temperature_to_color(temp):
    # Map temperature to a color
    temp = min(temp, temp_max)
    temp = max(temp, temp_min)

    f_index = float(temp - temp_min) / float(temp_max - temp_min)
    f_index *= len(colors) - 1
    index = int(f_index)

    if index == len(colors) - 1:
        return colors[index]

    blend_b = f_index - index
    blend_a = 1.0 - blend_b

    a = colors[index]
    b = colors[index + 1]

    return [int((a[i] * blend_a) + (b[i] * blend_b)) for i in range(3)]


def find_sync_point(adc_buffer, sync_level, sync_offset=0):
    """Find the first index where the signal crosses sync_level with a positive slope, considering sync_offset."""
    # Iterate through all data in the buffer considering the offset
    for i in range(sync_offset, len(adc_buffer) - 1):
        # Check if the previous value is below the sync level and the current value is above or equal
        if adc_buffer[i - 1] < sync_level and adc_buffer[i] >= sync_level:
            return i  # Return the index of the sync point
    return None  # If no crossing is found, return None


def calculate_current(adc_value, sensor_type, power_ACS758, divider=1.0):
    """
    Вычисляет ток на основе adc_value, типа сенсора, напряжения питания и делителя напряжения.
    sensor_type: ключ из ACS758, например '50B', '100U' и т.д.
    power_ACS758: напряжение питания сенсора (например, 3.3 или 5.0)
    adc_value: напряжение на выходе датчика (в вольтах)
    divider: коэффициент делителя напряжения (по умолчанию 1.0)
    """
    params = ACS758[sensor_type]
    if power_ACS758 == 3.3:
        offset = params["offset3"] * divider
        scale = params["scale3"] * divider
    else:
        offset = params["offset"] * divider
        scale = params["scale"] * divider
    current = (adc_value - offset) * 1000 / scale
    if current < 0:
        current = -1 * current
    return current


filter_buffer = []  

def filter_adc_value(new_value, window=15):
    global filter_buffer
    filter_buffer.append(new_value)
    if len(filter_buffer) > window:
        filter_buffer.pop(0)
    return sum(filter_buffer) / len(filter_buffer)


def read_adc(timer):
    global adc_buffer, time_buffer, sync_cross_count, sync_below, max_voltage, max_voltage_last_update, last_sync_cross_count
    try:
        for _ in range(SAMPLES_PER_FRAME):  # Read multiple samples per frame
            raw = sensor_temp.read_u16() * conversion_factor
            filtered = filter_adc_value(raw)
            reading = calculate_current(filtered, "100u", power_ACS758, Devider)  # Calculate current
            adc_buffer.append(reading)  # Add value to the buffer

            # Update max_voltage if the current reading is higher
            if reading > max_voltage:
                max_voltage = reading

            # Check if the reading is above SYNC_LEVEL
            if reading >= SYNC_LEVEL and sync_below:
                if button_y_state:
                    sync_cross_count += 1
                    time_buffer.append(
                        utime.ticks_ms()
                    )  # Add timestamp in milliseconds
                sync_below = False  # Signal is now above SYNC_LEVEL
                max_voltage_last_update = utime.ticks_ms()  # Update the timestamp

            # Reset sync_below when the signal drops below SYNC_LEVEL
            if reading < SYNC_LEVEL - SYNC_LEVEL_H:
                sync_below = True

            if len(adc_buffer) > BUFFER_SIZE:  # Limit buffer size
                adc_buffer.pop(0)

        # Check if max_voltage should be reset
        if reading < SYNC_LEVEL:  # First check if sync_cross_count has not changed
            if (
                utime.ticks_diff(utime.ticks_ms(), max_voltage_last_update)
                > max_voltage_timeout
            ):
                max_voltage = 0.0  # Reset max_voltage to zero

    except (KeyboardInterrupt, SystemExit):
        print("Interrupt in read_adc. Cleaning up resources.")
        adc_timer.deinit()
        display_timer.deinit()
        print("Exiting read_adc gracefully.")
        return


def update_display(timer):
    global last_sync_time, sync_time_diff, sync_cross_count, time_update, time_diff, button_y_state, max_voltage, SYNC_LEVEL, SYNC_LEVEL_H, state_error

    # Fills the screen with black
    display.set_pen(BLACK)
    display.clear()

    # Check if there is enough data in the buffer to display
    if len(adc_buffer) >= WIDTH:  # Changed condition to greater than or equal
        # Find the synchronization point
        sync_index = find_sync_point(adc_buffer, SYNC_LEVEL, sync_offset)

        if sync_index is not None:
            # If a sync point is found, record the time
            if last_sync_time is not None:
                # Calculate the time difference between two consecutive crossings
                current_time = utime.ticks_us() + sync_index * time_per_sample
                sync_time_diff = (
                    utime.ticks_diff(current_time, last_sync_time) / 1_000_000
                )  # Time difference in seconds
            last_sync_time = utime.ticks_us() + sync_index * time_per_sample

            # Start displaying from the point shifted by sync_offset
            start_index = sync_index + sync_offset
            # If start_index exceeds the buffer, limit it
            if start_index + WIDTH > len(adc_buffer):
                start_index = len(adc_buffer) - WIDTH
            # Display data starting from this point
            display_data = adc_buffer[start_index : start_index + WIDTH]
            adc_buffer.clear()
        else:
            # If no synchronization is found, start from the end of the buffer
            start_index = len(adc_buffer) - WIDTH
            display_data = adc_buffer[start_index : start_index + WIDTH]
    else:
        # If there is less data than the screen width, display the entire buffer
        display_data = adc_buffer

    if len(display_data) > 1:
        # Draw the graph by connecting points to simulate an oscilloscope
        display.set_pen(WHITE)
        step = WIDTH / len(display_data)  # Calculate step size for the X-axis
        for i in range(1, len(display_data)):
            x1 = int((i - 1) * step)  # Scale X coordinates
            y1 = HEIGHT - int(
                (display_data[i - 1] / DIS_MAP) * HEIGHT
            )  # Scale Y coordinates
            x2 = int(i * step)  # Scale X coordinates
            y2 = HEIGHT - int(
                (display_data[i] / DIS_MAP) * HEIGHT
            )  # Scale Y coordinates
            display.line(x1, y1, x2, y2)  # Draw a line between points

    # Set the RGB LED color based on the last value
    if len(display_data) > 0:
        led.set_rgb(*temperature_to_color(time_diff))

    # Draw a white rectangular area for the text
    display.set_pen(WHITE)
    display.rectangle(1, 1, 100, 25)

    # Write the current value on the screen
    if len(display_data) > 0:
        display.set_pen(BLACK)
        display.text("{:.2f}".format(display_data[-1]) + "A", 3, 3, 240, 3)

    if len(time_buffer) > 1:
        # Convert time difference to seconds and round to three decimal places
        time_diff = round((time_buffer[1] - time_buffer[0]), 3) / 1000
        time_buffer.clear()
        time_update = True

    if time_update:
        display.set_pen(WHITE)
        display.text("D: {:.3f} s".format(time_diff), 3, 25, 240, 3)

    if time_diff > dif_error:
        enable.value(0)  # Disable the signal
        button_y_state = False
        time_diff = 0.0  # Reset time_diff
        state_error = True  # Set error state

    display.set_pen(WHITE)
    display.text("CNT: {}".format(sync_cross_count), 3, 47, 240, 3)

    # Display the maximum voltage
    display.set_pen(WHITE)
    display.text("Max: {:.2f}A".format(max_voltage), 3, 69, 240, 3)

    # Check button_y state and toggle between STOP and START
    if button_y.value() == 0:  # Button pressed
        utime.sleep_ms(200)  # Debounce delay
        button_y_state = not button_y_state  # Toggle state
        enable.value(1 if button_y_state else 0)  # Enable or disable the signal
        time_diff = 0.0
        state_error = False  # Reset error state

    if state_error:
        error_pen = display.create_pen(255, 0, 0)  # Create a red pen
        display.set_pen(error_pen)
        display.text("ERROR", WIDTH // 2 - 10, HEIGHT // 2 - 23, 240, 4)

    # Display STOP or START in the bottom-right corner
    display.set_pen(WHITE)
    if button_y_state:
        display.text("STOP", WIDTH - 80, HEIGHT - 30, 240, 3)  # Shifted further left
    else:
        display.text("START", WIDTH - 80, HEIGHT - 30, 240, 3)  # Shifted further left

    # Check button_b state and reset sync_cross_count
    if button_b.value() == 0:  # Button pressed
        utime.sleep_ms(200)  # Debounce delay
        sync_cross_count = 0  # Reset sync_cross_count

    # Display RESET in the bottom-left corner, moved a few lines higher
    display.set_pen(WHITE)
    display.text("RESET", 3, HEIGHT - 30, 240, 3)  # Adjusted Y-coordinate to move up

    # Update the display
    display.update()


# Учёт входного сопротивления ADC
Rin_ADC = 500_000  # Входное сопротивление ADC, Ом
# Эффективный делитель с учётом Rin_ADC
Devider_eff = (R2 * Rin_ADC) / (R1 * Rin_ADC + R2 * Rin_ADC + R1 * R2)

# Set up the timer to call `read_adc` at the specified rate
adc_timer.init(freq=int(RATE), mode=Timer.PERIODIC, callback=read_adc)

# Set up the timer to call `update_display` at the specified rate
display_timer.init(freq=DISPLAY_RATE, mode=Timer.PERIODIC, callback=update_display)

print("Program is running. Press 'Ctrl+C' twice to exit.")

try:
    while True:
        pass  # Main loop is now empty as timers handle ADC reading and display updates
except KeyboardInterrupt:
    print("Program terminated.")
    adc_timer.deinit()
    display_timer.deinit()
    raise SystemExit  # Terminate the program

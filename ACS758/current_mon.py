import machine
from machine import Pin, ADC, Timer
import time
from pimoroni import RGBLED
from picographics import PicoGraphics, DISPLAY_PICO_DISPLAY
import utime
from sys import exit
import gc  # Garbage collector for memory cleanup
import uctypes
import array
import rp_devices as devs

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
R2 = 3140
Devider = R2 / (R1 + R2)  # Voltage divider ratio

# Taking into account ADC input resistance
Rin_ADC = 500_000  # ADC input resistance, Ohm
# Effective divider taking Rin_ADC into account
Devider_eff = (R2 * Rin_ADC) / (R1 * Rin_ADC + R2 * Rin_ADC + R1 * R2)

# Set up the RGB LED
led = RGBLED(6, 7, 8)

conversion_factor = 3.3 / (65535)

# DMA settings optimized for 100µs signal capture
CAPTURE_DEPTH = 500  # Optimized for 100µs signal (500 samples at 5MHz = 100µs)
SAMPLE_BUFFER_SIZE = CAPTURE_DEPTH
SAMPLING_RATE_HZ = 5000000  # 5 MHz target sampling rate for 100µs signals
TRIGGER_THRESHOLD = 0.1  # Voltage threshold to detect signal start

# DMA channel and configuration constants
DMA_BASE = 0x50000000
DMA_CH0_READ_ADDR = 0x50000000
DMA_CH0_WRITE_ADDR = 0x50000004
DMA_CH0_TRANS_COUNT = 0x50000008
DMA_CH0_CTRL_TRIG = 0x5000000c
DMA_CH0_AL1_CTRL = 0x50000010

# ADC base address and registers
ADC_BASE = 0x4004c000
ADC_RESULT = 0x4004c00c
ADC_FCS = 0x4004c004
ADC_DIV = 0x4004c008

# Timer for triggering ADC at regular intervals
TIM_BASE = 0x40054000
ALARM0 = 0x40054010

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

# Buffer for capturing ADC values using DMA
capture_buffer = array.array('H', [0] * SAMPLE_BUFFER_SIZE)  # 16-bit unsigned integers
dma_active = False

class DMAController(ADC):
    def __init__(self, adc_channel=2):
        super().__init__(adc_channel)
        self.channel = 0
        self.buffer = capture_buffer
        self.buffer_addr = uctypes.addressof(self.buffer)
        self.trigger_index = 0
        self.dma_chan = devs.DMA_CHANS[self.channel]
        self.adc = devs.ADC_DEVICE

    def _drain_adc_fifo(self):
        # Сбросить FIFO ADC перед запуском DMA
        while not self.adc.CS.READY:
            pass
        while not self.adc.FCS.EMPTY:
            _ = self.adc.FIFO_REG

    def setup_dma_channel(self):
        # Настройка DMA-канала для передачи из ADC FIFO в буфер
        self.dma_chan.READ_ADDR_REG = devs.ADC_FIFO_ADDR
        self.dma_chan.WRITE_ADDR_REG = self.buffer_addr
        self.dma_chan.TRANS_COUNT_REG = SAMPLE_BUFFER_SIZE
        # Control: enable, high prio, DREQ=36 (ADC), incr write, 16bit, enable
        ctrl_val = (1 << 31) | (1 << 24) | (36 << 15) | (1 << 4) | (1 << 2) | (1 << 0)
        self.dma_chan.CTRL_TRIG_REG = ctrl_val

    def setup_adc_for_dma(self):
        self.adc.DIV_REG = (9 << 8)  # 5 МГц
        # Включить DMA запросы, DREQ, FIFO EN
        fcs_val = (1 << 11) | (1 << 10) | (1 << 8) | (1 << 3)
        self.adc.FCS_REG = fcs_val
        # Выбрать канал (AINSEL = 2)
        self.adc.CS_REG = (self.adc.CS_REG & ~(0x7 << 12)) | (2 << 12)

    def start_capture(self):
        global dma_active
        if not dma_active:
            self._drain_adc_fifo()
            for i in range(len(self.buffer)):
                self.buffer[i] = 0
            self.setup_adc_for_dma()
            self.setup_dma_channel()
            self.adc.CS.START_MANY = 1
            dma_active = True

    def is_complete(self):
        return self.dma_chan.TRANS_COUNT_REG == 0

    def stop_capture(self):
        global dma_active
        self.dma_chan.CTRL_TRIG_REG = 0
        self.dma_chan.TRANS_COUNT_REG = 0
        self.adc.CS.START_MANY = 0
        dma_active = False
        
    def find_signal_boundaries(self, voltages):
        """Find start and end of signal in captured data"""
        signal_start = -1
        signal_end = -1
        threshold = TRIGGER_THRESHOLD
        
        # Find signal start (first sample above threshold)
        for i, voltage in enumerate(voltages):
            if voltage > threshold:
                signal_start = i
                break
                
        # Find signal end (last sample above threshold)
        if signal_start != -1:
            for i in range(len(voltages) - 1, signal_start, -1):
                if voltages[i] > threshold:
                    signal_end = i
                    break
                    
        return signal_start, signal_end
        
    def get_data(self):
        """Get captured data and analyze 100µs signal"""
        global signal_detected, signal_start_time, signal_duration
        
        if self.is_complete():
            # Convert raw ADC values to voltages
            voltages = []
            valid_samples = 0
            
            for raw_val in self.buffer:
                if raw_val > 0:  # Valid reading
                    voltage = raw_val * conversion_factor
                    voltages.append(voltage)
                    valid_samples += 1
                else:
                    break  # Stop at first invalid sample
                    
            if voltages:
                # Find signal boundaries
                start_idx, end_idx = self.find_signal_boundaries(voltages)
                
                if start_idx != -1 and end_idx != -1:
                    # Signal detected
                    signal_detected = True
                    signal_duration = (end_idx - start_idx) / actual_sampling_rate * 1000000  # µs
                    
                    # Extract signal portion for analysis
                    signal_voltages = voltages[start_idx:end_idx + 1]
                    return signal_voltages
                else:
                    signal_detected = False
                    return voltages  # Return all data if no clear signal found
                    
            return []
        return []

# Initialize DMA controller
dma_controller = DMAController()

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
    """
    Calculates current based on adc_value, sensor type, power supply voltage and voltage divider.
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

# Buffer for filtering
filter_buffer = []

def filter_adc_value(new_value, window=10):
    global filter_buffer
    filter_buffer.append(new_value)
    if len(filter_buffer) > window:
        filter_buffer.pop(0)
    return sum(filter_buffer) / len(filter_buffer)

def capture_current_dma():
    """Function for capturing 100µs current pulses using DMA with maximum speed"""
    global current_value, max_current, actual_sampling_rate, dma_active, signal_detected
    
    start_time = utime.ticks_us()
    
    # Start DMA capture if not already active
    if not dma_active:
        dma_controller.start_capture()
        return 0  # Return early on first call
    
    # Check if DMA capture is complete
    if dma_controller.is_complete():
        end_time = utime.ticks_us()
        
        # Get captured data (already filtered for signal boundaries)
        voltages = dma_controller.get_data()
        
        if voltages:
            # Calculate actual sampling rate
            capture_duration = utime.ticks_diff(end_time, start_time) / 1000000
            actual_sampling_rate = len(voltages) / capture_duration if capture_duration > 0 else 0
            
            # Analyze signal characteristics for 100µs pulse
            if signal_detected:
                # For detected signals, use peak detection
                max_voltage = max(voltages)
                
                # Calculate RMS for more accurate current measurement
                rms_voltage = (sum(v*v for v in voltages) / len(voltages)) ** 0.5
                
                # Use RMS for current calculation (better for pulsed signals)
                current_rms = calculate_current(rms_voltage, "100U", power_ACS758, Devider)
                max_current_sample = calculate_current(max_voltage, "100U", power_ACS758, Devider)
                
                # Update current value with RMS for stability
                current_value = filter_adc_value(current_rms, window=5)
                
                # Update maximum if peak is higher
                if max_current_sample > max_current:
                    max_current = max_current_sample
            else:
                # No signal detected, use average for noise floor
                avg_voltage = sum(voltages) / len(voltages) if voltages else 0
                current_value = calculate_current(filter_adc_value(avg_voltage), 
                                                 "100U", power_ACS758, Devider)
        
        # Stop current DMA and prepare for next capture cycle
        dma_controller.stop_capture()
        dma_active = False
        
        return actual_sampling_rate
    
    return actual_sampling_rate

def capture_current_fallback():
    """Fallback function for capturing current without DMA"""
    global current_value, max_current, actual_sampling_rate
    
    capture_buffer_local = []
    gc.collect()  # Clear memory before capture
    
    # Begin capturing data at maximum speed
    start_time = utime.ticks_us()
    for _ in range(SAMPLE_BUFFER_SIZE // 4):  # Reduced for fallback
        raw = sensor_temp.read_u16() * conversion_factor
        capture_buffer_local.append(raw)
    end_time = utime.ticks_us()
    
    # Process captured data
    if capture_buffer_local:
        # Find maximum value
        max_raw = max(capture_buffer_local)
        filtered_max = filter_adc_value(max_raw)
        max_current_sample = calculate_current(filtered_max, "100U", power_ACS758, Devider)
        
        # Update maximum value if new value is higher
        if max_current_sample > max_current:
            max_current = max_current_sample
        
        # Current value - the last measured one
        current_value = calculate_current(filter_adc_value(capture_buffer_local[-1]), 
                                         "100U", power_ACS758, Devider)
    
    # Calculate actual sampling rate
    capture_duration = utime.ticks_diff(end_time, start_time) / 1000000
    actual_sampling_rate = len(capture_buffer_local) / capture_duration if capture_duration > 0 else 0
    
    return actual_sampling_rate

def update_display():
    """Update screen with current and maximum values, optimized for 100µs signals"""
    global current_value, max_current, button_y_state, state_error, actual_sampling_rate
    global signal_detected, signal_duration
    
    # Clear the screen
    display.set_pen(BLACK)
    display.clear()
    
    # Display current value in large font
    display.set_pen(WHITE)
    display.text(f"Current:", 10, 10, WIDTH, 2)
    display.text(f"{current_value:.2f}A", 10, 35, WIDTH, 4)
    
    # Display maximum current value - moved to right side
    display.set_pen(MAGENTA)
    display.text(f"Peak:", WIDTH//2 + 10, 10, WIDTH, 2)
    display.text(f"{max_current:.2f}A", WIDTH//2 + 10, 35, WIDTH, 4)
    
    # Operation status
    display.set_pen(GREEN if button_y_state else RED)
    status_text = "RUNNING" if button_y_state else "STOPPED"
    display.text(f"Status: {status_text}", 10, 80, WIDTH, 2)
    
    # Signal detection status for 100µs pulses
    display.set_pen(GREEN if signal_detected else BLUE)
    signal_status = "SIGNAL" if signal_detected else "READY"
    display.text(f"Signal: {signal_status}", WIDTH//2 + 10, 80, WIDTH, 2)
    
    # Display sampling rate and signal duration
    display.set_pen(YELLOW)
    display.text(f"Rate: {actual_sampling_rate/1000:.1f}kHz", 10, 100, WIDTH, 1)
    
    if signal_detected:
        display.text(f"Pulse: {signal_duration:.1f}µs", WIDTH//2 + 10, 100, WIDTH, 1)
    
    # DMA status indicator
    display.set_pen(CYAN if dma_active else YELLOW)
    dma_status = "DMA" if dma_active else "STD"
    display.text(f"Mode: {dma_status}", 10, 115, WIDTH, 1)
    
    # Threshold indicator
    display.set_pen(WHITE)
    display.text(f"Trig: {TRIGGER_THRESHOLD:.1f}V", WIDTH//2 + 10, 115, WIDTH, 1)
    
    # Error information, if any
    if state_error:
        display.set_pen(RED)
        display.text("ERROR", WIDTH // 2 - 40, 130, WIDTH, 3)
    
    # Button instructions positioned at bottom
    display.set_pen(CYAN)
    # Left side - B button 
    display.text("Reset Peak", 10, HEIGHT - 30, WIDTH, 1)
    # Right side - Y button
    display.text("Start/Stop", WIDTH - 125, HEIGHT - 30, WIDTH, 1)
    
    # Update display
    display.update()
    
    # Set LED color based on current (more sensitive for µs pulses)
    led.set_rgb(*current_to_color(current_value))

def read_buttons():
    """Process button presses"""
    global button_y_state, state_error, max_current, dma_active
    
    # Button Y - start/stop 
    if button_y.value() == 0:  # Button pressed
        utime.sleep_ms(200)  # Debounce
        button_y_state = not button_y_state  # Toggle state
        enable.value(1 if button_y_state else 0)  # Enable/disable signal
        state_error = False  # Reset error on state change
        
        # Stop DMA if stopping measurement
        if not button_y_state and dma_active:
            dma_controller.stop_capture()
    
    # Button B - reset maximum value
    if button_b.value() == 0:  # Button pressed
        utime.sleep_ms(200)  # Debounce
        max_current = 0.0  # Reset maximum value

# Main program loop
print("Program started with DMA support optimized for 100us signals.")
print(f"Target sampling rate: {SAMPLING_RATE_HZ/1000:.1f} kHz")
print(f"Capture buffer size: {SAMPLE_BUFFER_SIZE} samples")
print(f"Signal detection threshold: {TRIGGER_THRESHOLD:.2f}V")
print(f"Expected signal resolution: {SAMPLE_BUFFER_SIZE/(SAMPLING_RATE_HZ/1000000):.1f} samples per 100us")

try:
    use_dma = True  # Flag to enable/disable DMA usage
    
    while True:
        # Read button states
        read_buttons()
        
        # If capture mode is active, capture data
        if button_y_state:
            try:
                if use_dma:
                    actual_rate = capture_current_dma()
                else:
                    actual_rate = capture_current_fallback()
            except Exception as e:
                print(f"DMA error, falling back to standard mode: {e}")
                use_dma = False
                dma_controller.stop_capture()
                actual_rate = capture_current_fallback()
        
        # Update display
        update_display()
        
        # Small delay optimized for 100µs signal detection
        utime.sleep_ms(10)  # Faster update rate for short pulses
        
except KeyboardInterrupt:
    print("Program terminated.")
    enable.value(0)  # Disable signal on exit
    if dma_active:
        dma_controller.stop_capture()
    raise SystemExit
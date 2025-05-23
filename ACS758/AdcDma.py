from RP2040ADC import Rp2040AdcDmaAveraging
import time

PIN_IN = 28
conversion_factor = 3.3 / 4095

avg_adc = Rp2040AdcDmaAveraging(gpio_pin=PIN_IN, dma_chan=0, adc_samples=64)
print("ADC + DMA test for Raspberry Pi Pico")
while True:
  avg_adc.capture_start()
  print("ADC:", avg_adc.wait_and_read_average_u12()*conversion_factor)
  time.sleep(0.1)
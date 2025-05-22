#include <Arduino.h>

// Number to count to with PWM (TOP value). Frequency can be calculated by
// freq = GCLK4_freq / (TCC0_prescaler * (1 + TOP_value))
// With TOP of 479, we get a 100 kHz square wave in this example
uint32_t period = 480 - 1;

void setup() {

  // Because we are using TCC0, limit period to 24 bits
  period = (period < 0x00ffffff) ? period : 0x00ffffff;

  // Enable and configure generic clock generator 4
  GCLK->GENCTRL.reg = GCLK_GENCTRL_IDC |         // Improve duty cycle
                      GCLK_GENCTRL_GENEN |       // Enable generic clock gen
                      GCLK_GENCTRL_SRC_DFLL48M | // Select 48MHz as source
                      GCLK_GENCTRL_ID(4);        // Select GCLK4
  while (GCLK->STATUS.bit.SYNCBUSY)
    ; // Wait for synchronization

  // Set clock divider of 1 to generic clock generator 4
  GCLK->GENDIV.reg = GCLK_GENDIV_DIV(1) | // Divide 48 MHz by 1
                     GCLK_GENDIV_ID(4);   // Apply to GCLK4 4
  while (GCLK->STATUS.bit.SYNCBUSY)
    ; // Wait for synchronization

  // Enable GCLK4 and connect it to TCC0 and TCC1
  GCLK->CLKCTRL.reg = GCLK_CLKCTRL_CLKEN |       // Enable generic clock
                      GCLK_CLKCTRL_GEN_GCLK4 |   // Select GCLK4
                      GCLK_CLKCTRL_ID_TCC0_TCC1; // Feed GCLK4 to TCC0/1
  while (GCLK->STATUS.bit.SYNCBUSY)
    ; // Wait for synchronization

  // Divide counter by 1 giving 48 MHz (20.83 ns) on each TCC0 tick
  TCC0->CTRLA.reg |= TCC_CTRLA_PRESCALER(TCC_CTRLA_PRESCALER_DIV1_Val);

  // Use "Normal PWM" (single-slope PWM): count up to PER, match on CC[n]
  TCC0->WAVE.reg = TCC_WAVE_WAVEGEN_NPWM; // Select NPWM as waveform
  while (TCC0->SYNCBUSY.bit.WAVE)
    ; // Wait for synchronization

  // Set the period (the number to count to (TOP) before resetting timer)
  TCC0->PER.reg = period;
  while (TCC0->SYNCBUSY.bit.PER)
    ;

  // Set PWM signal to output 100 ns pulse
  // n for CC[n] is determined by n = x % 4 where x is from WO[x]
  TCC0->CC[2].reg = 5; // 100 ns = 5 ticks (48 MHz -> 20.83 ns per tick)
  while (TCC0->SYNCBUSY.bit.CC2)
    ;

  // Configure PA18 (D10 on Arduino Zero) to be output
  PORT->Group[PORTA].DIRSET.reg = PORT_PA18; // Set pin as output
  PORT->Group[PORTA].OUTCLR.reg = PORT_PA18; // Set pin to low

  // Enable the port multiplexer for PA18
  PORT->Group[PORTA].PINCFG[18].reg |= PORT_PINCFG_PMUXEN;

  // Connect TCC0 timer to PA18. Function F is TCC0/WO[2] for PA18.
  // Odd pin num (2*n + 1): use PMUXO
  // Even pin num (2*n): use PMUXE
  PORT->Group[PORTA].PMUX[9].reg = PORT_PMUX_PMUXE_F;

  // Enable output (start PWM)
  TCC0->CTRLA.reg |= (TCC_CTRLA_ENABLE);
  while (TCC0->SYNCBUSY.bit.ENABLE)
    ; // Wait for synchronization
}

void loop() {
  // Do nothing
}
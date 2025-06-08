// tusb_config.h
#ifndef _TUSB_CONFIG_H_
#define _TUSB_CONFIG_H_

#ifdef __cplusplus
extern "C" {
#endif

//--------------------------------------------------------------------
// COMMON CONFIGURATION
//--------------------------------------------------------------------

// defined by compiler flags for flexibility
#ifndef CFG_TUSB_MCU
#error CFG_TUSB_MCU must be defined
#endif

// RHPort number used for device can be defined by board.mk, default to port 0
#ifndef BOARD_TUD_RHPORT
#define BOARD_TUD_RHPORT      0
#endif

// RHPort max operational speed can defined by board.mk
#ifndef BOARD_TUD_MAX_SPEED
#define BOARD_TUD_MAX_SPEED   OPT_MODE_DEFAULT_SPEED
#endif

//--------------------------------------------------------------------
// Device Configuration
//--------------------------------------------------------------------

// Enable Device stack
#define CFG_TUD_ENABLED       1

// Port0 DEVICE mode with internal phy
#define CFG_TUSB_RHPORT0_MODE     (OPT_MODE_DEVICE | OPT_MODE_FULL_SPEED)

#ifndef CFG_TUSB_OS
#define CFG_TUSB_OS           OPT_OS_NONE
#endif

#ifndef CFG_TUSB_DEBUG
#define CFG_TUSB_DEBUG        0
#endif

/* USB DMA on some MCUs can only access a specific SRAM region with restriction on alignment.
 */
#ifndef CFG_TUSB_MEM_SECTION
#define CFG_TUSB_MEM_SECTION
#endif

#ifndef CFG_TUSB_MEM_ALIGN
#define CFG_TUSB_MEM_ALIGN          __attribute__ ((aligned(4)))
#endif

//--------------------------------------------------------------------
// DEVICE CONFIGURATION
//--------------------------------------------------------------------

#ifndef CFG_TUD_ENDPOINT0_SIZE
#define CFG_TUD_ENDPOINT0_SIZE    64
#endif

//------------- CLASS -------------//
#define CFG_TUD_AUDIO             1
#define CFG_TUD_CDC               0
#define CFG_TUD_MSC               0
#define CFG_TUD_HID               0
#define CFG_TUD_MIDI              0
#define CFG_TUD_VENDOR            0

//--------------------------------------------------------------------
// AUDIO CLASS DRIVER CONFIGURATION
//--------------------------------------------------------------------

// Audio parameters
#define AUDIO_SAMPLE_RATE   48000
#define AUDIO_SAMPLE_DEPTH  16
#define AUDIO_CHANNELS      1  // Mono

// Calculate descriptor length manually for mono audio device
// This includes: IAD + Audio Control Interface + Audio Streaming Interfaces
#define CFG_TUD_AUDIO_FUNC_1_DESC_LEN                 (TUD_AUDIO_DESC_IAD_LEN + TUD_AUDIO_DESC_STD_AC_LEN + TUD_AUDIO_DESC_CS_AC_LEN + TUD_AUDIO_DESC_CLK_SRC_LEN + TUD_AUDIO_DESC_INPUT_TERM_LEN + TUD_AUDIO_DESC_OUTPUT_TERM_LEN + TUD_AUDIO_DESC_FEATURE_UNIT_ONE_CHANNEL_LEN + 2*(TUD_AUDIO_DESC_STD_AS_INT_LEN + TUD_AUDIO_DESC_STD_AS_INT_LEN + TUD_AUDIO_DESC_CS_AS_INT_LEN + TUD_AUDIO_DESC_TYPE_I_FORMAT_LEN + TUD_AUDIO_DESC_STD_AS_ISO_EP_LEN + TUD_AUDIO_DESC_CS_AS_ISO_EP_LEN))

// How many formats are supported in total
#define CFG_TUD_AUDIO_FUNC_1_N_FORMATS                2

// Audio control buffer size of audio function 1
#define CFG_TUD_AUDIO_FUNC_1_CTRL_BUF_SZ              64

// Number of Standard AS Interface Descriptors
#define CFG_TUD_AUDIO_FUNC_1_N_AS_INT                 2

// Number of clock entities defined per audio function
#define CFG_TUD_AUDIO_FUNC_1_N_CLK_SRC                1

// Number of channels in audio function 1
#define CFG_TUD_AUDIO_FUNC_1_N_CHANNELS_TX            1
#define CFG_TUD_AUDIO_FUNC_1_N_CHANNELS_RX            1

// Number of supported sample rates per audio function
#define CFG_TUD_AUDIO_FUNC_1_N_SAMPLE_RATES           1

// Enable/disable feedback EP
#define CFG_TUD_AUDIO_ENABLE_EP_IN                    1
#define CFG_TUD_AUDIO_ENABLE_EP_OUT                   1

// Maximum endpoint sizes - required by TinyUSB
#define CFG_TUD_AUDIO_EP_SZ_IN                        TUD_AUDIO_EP_SIZE(AUDIO_SAMPLE_RATE, AUDIO_SAMPLE_DEPTH/8, AUDIO_CHANNELS)
#define CFG_TUD_AUDIO_EP_SZ_OUT                       TUD_AUDIO_EP_SIZE(AUDIO_SAMPLE_RATE, AUDIO_SAMPLE_DEPTH/8, AUDIO_CHANNELS)

// Function 1 endpoint sizes (required)
#define CFG_TUD_AUDIO_FUNC_1_EP_IN_SZ_MAX             CFG_TUD_AUDIO_EP_SZ_IN
#define CFG_TUD_AUDIO_FUNC_1_EP_OUT_SZ_MAX            CFG_TUD_AUDIO_EP_SZ_OUT

// Buffer sizes for endpoint transfers
#define CFG_TUD_AUDIO_FUNC_1_EP_IN_SW_BUF_SZ          CFG_TUD_AUDIO_EP_SZ_IN
#define CFG_TUD_AUDIO_FUNC_1_EP_OUT_SW_BUF_SZ         CFG_TUD_AUDIO_EP_SZ_OUT

#ifdef __cplusplus
}
#endif

#endif /* _TUSB_CONFIG_H_ */
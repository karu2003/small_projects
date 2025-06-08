// usb_descriptors.h
#ifndef USB_DESCRIPTORS_H_
#define USB_DESCRIPTORS_H_

#include "tusb.h"

#ifdef __cplusplus
extern "C" {
#endif

// EP numbers
enum {
    EPNUM_AUDIO_OUT = 0x01,
    EPNUM_AUDIO_IN  = 0x81
};

// Interface numbers
enum {
    ITF_NUM_AUDIO_CONTROL = 0,
    ITF_NUM_AUDIO_STREAMING_SPK,
    ITF_NUM_AUDIO_STREAMING_MIC,
    ITF_NUM_TOTAL
};

#ifdef __cplusplus
}
#endif

#endif /* USB_DESCRIPTORS_H_ */
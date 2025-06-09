// main.c - Dual core audio project with USB support
#include "common.h"
#include "lufa/AudioClassCommon.h"
#include "pico/usb_device.h"
#include <pico/multicore.h>
#include <pico/stdlib.h>
#include <string.h>

// Core 0 - PPM receiver
static PIO           pio0_instance = pio0;
static uint          sm_det;
static volatile bool detector_running = false;

// Core 1 - USB audio and PPM generator
static PIO  pio1_instance = pio1;
static uint sm_gen;

volatile uint32_t ppm_code_to_send = 0;
volatile bool     has_custom_value = false;

// USB Audio Configuration
#define AUDIO_SAMPLE_RATE  48000
#define AUDIO_CHANNELS     1
#define AUDIO_SAMPLE_DEPTH 16
// #define AUDIO_BUFFER_SIZE  96

// Аудио буферы
static int16_t audio_buffer_in[AUDIO_BUFFER_SIZE];     // Буфер для направления устройство->хост
static int16_t audio_buffer_out[AUDIO_BUFFER_SIZE];    // Буфер для направления хост->устройство

// ============================================================================
// USB AUDIO IMPLEMENTATION BASED ON USB_SOUND_CARD.C EXAMPLE
// ============================================================================

// todo fix these
#define VENDOR_ID  0x2e8au
#define PRODUCT_ID 0xfeddu

// USB endpoints
#define AUDIO_OUT_ENDPOINT 0x01U
#define AUDIO_IN_ENDPOINT  0x82U

// #define AUDIO_SAMPLE_FREQ(frq)      (uint8_t)(frq), (uint8_t)((frq >> 8)), (uint8_t)((frq >> 16))
#define AUDIO_MAX_PACKET_SIZE(freq) (uint8_t)(((freq + 999) / 1000) * 4)
#define FEATURE_MUTE_CONTROL        1u
#define FEATURE_VOLUME_CONTROL      2u
#define ENDPOINT_FREQ_CONTROL       1u

// Состояния для USB аудио
static struct {
    uint32_t freq;
    int16_t  volume;
    int16_t  vol_mul;
    uint8_t  mute;
} audio_state = {
    .freq    = AUDIO_SAMPLE_RATE,
    .volume  = 0,
    .vol_mul = 0x7FFF,    // По умолчанию полная громкость
    .mute    = 0};

// Audio device config
struct audio_device_config {
    struct usb_configuration_descriptor descriptor;
    struct usb_interface_descriptor     ac_interface;
    struct __packed {
        struct __packed {
            uint8_t  bLength;
            uint8_t  bDescriptorType;
            uint8_t  bDescriptorSubtype;
            uint16_t bcdADC;
            uint16_t wTotalLength;
            uint8_t  bInCollection;
            uint8_t  baInterfaceNr[2];    // Массив из 2 интерфейсов (1 и 2)
        } core;
        // Терминалы для вывода звука (OUT - воспроизведение)
        USB_Audio_StdDescriptor_InputTerminal_t  input_terminal;
        USB_Audio_StdDescriptor_FeatureUnit_t    feature_unit;
        USB_Audio_StdDescriptor_OutputTerminal_t output_terminal;

        // Терминалы для записи звука (IN - микрофон)
        USB_Audio_StdDescriptor_InputTerminal_t  mic_input_terminal;
        USB_Audio_StdDescriptor_FeatureUnit_t    mic_feature_unit;
        USB_Audio_StdDescriptor_OutputTerminal_t mic_output_terminal;
    } ac_audio;
    struct usb_interface_descriptor as_zero_interface;
    struct usb_interface_descriptor as_op_interface;
    struct __packed {
        USB_Audio_StdDescriptor_Interface_AS_t streaming;
        struct __packed {
            USB_Audio_StdDescriptor_Format_t core;
            USB_Audio_SampleFreq_t           freqs[1];
        } format;
    } as_audio;
    struct __packed {
        struct usb_endpoint_descriptor_long          core;
        USB_Audio_StdDescriptor_StreamEndpoint_Spc_t audio;
    } ep1;
    struct __packed {
        struct usb_endpoint_descriptor_long          core;
        USB_Audio_StdDescriptor_StreamEndpoint_Spc_t audio;
    } ep2;
};

static const struct audio_device_config audio_device_config = {
    .descriptor = {
        .bLength             = sizeof(struct usb_configuration_descriptor),
        .bDescriptorType     = DTYPE_Configuration,
        .wTotalLength        = sizeof(struct audio_device_config),
        .bNumInterfaces      = 3,    // Увеличиваем число интерфейсов до 3 (1 контрольный + 2 потоковых)
        .bConfigurationValue = 0x01,
        .iConfiguration      = 0x00,
        .bmAttributes        = 0x80,
        .bMaxPower           = 0x32,
    },
    .ac_interface = {
        .bLength            = sizeof(struct usb_interface_descriptor),
        .bDescriptorType    = DTYPE_Interface,
        .bInterfaceNumber   = 0x00,
        .bAlternateSetting  = 0x00,
        .bNumEndpoints      = 0x00,
        .bInterfaceClass    = AUDIO_CSCP_AudioClass,
        .bInterfaceSubClass = AUDIO_CSCP_ControlSubclass,
        .bInterfaceProtocol = AUDIO_CSCP_ControlProtocol,
        .iInterface         = 0x00,
    },
    .ac_audio = {
        .core = {
            .bLength = sizeof(audio_device_config.ac_audio.core), .bDescriptorType = AUDIO_DTYPE_CSInterface, .bDescriptorSubtype = AUDIO_DSUBTYPE_CSInterface_Header, .bcdADC = VERSION_BCD(1, 0, 0), .wTotalLength = sizeof(struct audio_device_config) - (sizeof(struct usb_configuration_descriptor) + sizeof(struct usb_interface_descriptor)),
            .bInCollection = 2,         // Указываем, что у нас два интерфейса
            .baInterfaceNr = {1, 2},    // Номера интерфейсов: 1 для OUT, 2 для IN
        },
        // Устройство воспроизведения (OUT)
        .input_terminal = {
            .bLength            = sizeof(USB_Audio_StdDescriptor_InputTerminal_t),
            .bDescriptorType    = AUDIO_DTYPE_CSInterface,
            .bDescriptorSubtype = AUDIO_DSUBTYPE_CSInterface_InputTerminal,
            .bTerminalID        = 1,
            .wTerminalType      = AUDIO_TERMINAL_STREAMING,
            .bAssocTerminal     = 0,
            .bNrChannels        = AUDIO_CHANNELS,
            .wChannelConfig     = (AUDIO_CHANNELS == 1) ? AUDIO_CHANNEL_CENTER_FRONT : (AUDIO_CHANNEL_LEFT_FRONT | AUDIO_CHANNEL_RIGHT_FRONT),
            .iChannelNames      = 0,
            .iTerminal          = 0,
        },
        .feature_unit = {
            .bLength            = sizeof(USB_Audio_StdDescriptor_FeatureUnit_t),
            .bDescriptorType    = AUDIO_DTYPE_CSInterface,
            .bDescriptorSubtype = AUDIO_DSUBTYPE_CSInterface_Feature,
            .bUnitID            = 2,
            .bSourceID          = 1,
            .bControlSize       = 1,
            .bmaControls        = {AUDIO_FEATURE_MUTE | AUDIO_FEATURE_VOLUME, 0, 0},
            .iFeature           = 0,
        },
        .output_terminal = {
            .bLength            = sizeof(USB_Audio_StdDescriptor_OutputTerminal_t),
            .bDescriptorType    = AUDIO_DTYPE_CSInterface,
            .bDescriptorSubtype = AUDIO_DSUBTYPE_CSInterface_OutputTerminal,
            .bTerminalID        = 3,
            .wTerminalType      = AUDIO_TERMINAL_OUT_SPEAKER,
            .bAssocTerminal     = 0,
            .bSourceID          = 2,
            .iTerminal          = 0,
        },

        // Устройство записи (IN)
        .mic_input_terminal = {
            .bLength            = sizeof(USB_Audio_StdDescriptor_InputTerminal_t),
            .bDescriptorType    = AUDIO_DTYPE_CSInterface,
            .bDescriptorSubtype = AUDIO_DSUBTYPE_CSInterface_InputTerminal,
            .bTerminalID        = 4,
            .wTerminalType      = AUDIO_TERMINAL_IN_MIC,
            .bAssocTerminal     = 0,
            .bNrChannels        = AUDIO_CHANNELS,
            .wChannelConfig     = (AUDIO_CHANNELS == 1) ? AUDIO_CHANNEL_CENTER_FRONT : (AUDIO_CHANNEL_LEFT_FRONT | AUDIO_CHANNEL_RIGHT_FRONT),
            .iChannelNames      = 0,
            .iTerminal          = 0,
        },
        .mic_feature_unit = {
            .bLength            = sizeof(USB_Audio_StdDescriptor_FeatureUnit_t),
            .bDescriptorType    = AUDIO_DTYPE_CSInterface,
            .bDescriptorSubtype = AUDIO_DSUBTYPE_CSInterface_Feature,
            .bUnitID            = 5,
            .bSourceID          = 4,
            .bControlSize       = 1,
            .bmaControls        = {AUDIO_FEATURE_MUTE | AUDIO_FEATURE_VOLUME, 0, 0},
            .iFeature           = 0,
        },
        .mic_output_terminal = {
            .bLength            = sizeof(USB_Audio_StdDescriptor_OutputTerminal_t),
            .bDescriptorType    = AUDIO_DTYPE_CSInterface,
            .bDescriptorSubtype = AUDIO_DSUBTYPE_CSInterface_OutputTerminal,
            .bTerminalID        = 6,
            .wTerminalType      = AUDIO_TERMINAL_STREAMING,
            .bAssocTerminal     = 0,
            .bSourceID          = 5,
            .iTerminal          = 0,
        },
    },
    .as_zero_interface = {
        .bLength            = sizeof(struct usb_interface_descriptor),
        .bDescriptorType    = DTYPE_Interface,
        .bInterfaceNumber   = 0x01,
        .bAlternateSetting  = 0x00,
        .bNumEndpoints      = 0x00,
        .bInterfaceClass    = AUDIO_CSCP_AudioClass,
        .bInterfaceSubClass = AUDIO_CSCP_AudioStreamingSubclass,
        .bInterfaceProtocol = AUDIO_CSCP_ControlProtocol,
        .iInterface         = 0x00,
    },
    .as_op_interface = {
        .bLength            = sizeof(struct usb_interface_descriptor),
        .bDescriptorType    = DTYPE_Interface,
        .bInterfaceNumber   = 0x01,
        .bAlternateSetting  = 0x01,
        .bNumEndpoints      = 0x02,
        .bInterfaceClass    = AUDIO_CSCP_AudioClass,
        .bInterfaceSubClass = AUDIO_CSCP_AudioStreamingSubclass,
        .bInterfaceProtocol = AUDIO_CSCP_ControlProtocol,
        .iInterface         = 0x00,
    },
    .as_zero_mic_interface = {
        .bLength            = sizeof(struct usb_interface_descriptor),
        .bDescriptorType    = DTYPE_Interface,
        .bInterfaceNumber   = 0x02,    // Используем номер интерфейса 2
        .bAlternateSetting  = 0x00,
        .bNumEndpoints      = 0x00,
        .bInterfaceClass    = AUDIO_CSCP_AudioClass,
        .bInterfaceSubClass = AUDIO_CSCP_AudioStreamingSubclass,
        .bInterfaceProtocol = AUDIO_CSCP_ControlProtocol,
        .iInterface         = 0x00,
    },
    .as_op_mic_interface = {
        .bLength            = sizeof(struct usb_interface_descriptor),
        .bDescriptorType    = DTYPE_Interface,
        .bInterfaceNumber   = 0x02,    // Тот же номер интерфейса 2
        .bAlternateSetting  = 0x01,    // Но другая альтернативная настройка
        .bNumEndpoints      = 0x01,    // Один эндпоинт для микрофона (IN)
        .bInterfaceClass    = AUDIO_CSCP_AudioClass,
        .bInterfaceSubClass = AUDIO_CSCP_AudioStreamingSubclass,
        .bInterfaceProtocol = AUDIO_CSCP_ControlProtocol,
        .iInterface         = 0x00,
    },
    .as_audio = {
        .streaming = {
            .bLength = sizeof(audio_device_config.as_audio.streaming), .bDescriptorType = AUDIO_DTYPE_CSInterface, .bDescriptorSubtype = AUDIO_DSUBTYPE_CSInterface_General, .bTerminalLink = 1, .bDelay = 1,
            .wFormatTag = 1,    // PCM
        },
        .format = {
            .core = {
                .bLength              = sizeof(audio_device_config.as_audio.format),
                .bDescriptorType      = AUDIO_DTYPE_CSInterface,
                .bDescriptorSubtype   = AUDIO_DSUBTYPE_CSInterface_FormatType,
                .bFormatType          = 1,
                .bNrChannels          = AUDIO_CHANNELS,
                .bSubFrameSize        = 2,
                .bBitResolution       = 16,
                .bSampleFrequencyType = count_of(audio_device_config.as_audio.format.freqs),
            },
            .freqs = {AUDIO_SAMPLE_FREQ(AUDIO_SAMPLE_RATE)},
        },
    },
    .ep1 = {.core = {
                .bLength          = sizeof(struct usb_endpoint_descriptor_long),
                .bDescriptorType  = DTYPE_Endpoint,
                .bEndpointAddress = AUDIO_OUT_ENDPOINT,
                .bmAttributes     = 5,    // Isochronous
                .wMaxPacketSize   = 96, // 1 канал, 16 бит, 48000 Гц
                .bInterval        = 1,
                .bRefresh         = 0,
                .bSyncAddr        = 0,
            },
            .audio = {
                .bLength            = sizeof(USB_Audio_StdDescriptor_StreamEndpoint_Spc_t),
                .bDescriptorType    = AUDIO_DTYPE_CSEndpoint,
                .bDescriptorSubtype = AUDIO_DSUBTYPE_CSEndpoint_General,
                .bmAttributes       = 1,
                .bLockDelayUnits    = 0,
                .wLockDelay         = 0,
            }},
    .ep2 = {.core = {
                .bLength          = sizeof(struct usb_endpoint_descriptor_long),
                .bDescriptorType  = DTYPE_Endpoint,
                .bEndpointAddress = AUDIO_IN_ENDPOINT,
                .bmAttributes     = 5,    // Isochronous
                .wMaxPacketSize   = 96, // 1 канал, 16 бит, 48000 Гц
                .bInterval        = 1,
                .bRefresh         = 0,
                .bSyncAddr        = 0,
            },
            .audio = {
                .bLength            = sizeof(USB_Audio_StdDescriptor_StreamEndpoint_Spc_t),
                .bDescriptorType    = AUDIO_DTYPE_CSEndpoint,
                .bDescriptorSubtype = AUDIO_DSUBTYPE_CSEndpoint_General,
                .bmAttributes       = 1,
                .bLockDelayUnits    = 0,
                .wLockDelay         = 0,
            }}};

static const struct usb_device_descriptor device_descriptor = {
    .bLength            = 18,
    .bDescriptorType    = 0x01,
    .bcdUSB             = 0x0110,
    .bDeviceClass       = 0x00,
    .bDeviceSubClass    = 0x00,
    .bDeviceProtocol    = 0x00,
    .bMaxPacketSize0    = 0x40,
    .idVendor           = VENDOR_ID,
    .idProduct          = PRODUCT_ID,
    .bcdDevice          = 0x0100,
    .iManufacturer      = 0x01,
    .iProduct           = 0x02,
    .iSerialNumber      = 0x03,
    .bNumConfigurations = 0x01,
};

// String descriptors
static const char *string_descriptors[] = {
    // "\x09\x04",          // Языковой дескриптор (US English)
    "ka_ru",             // Manufacturer
    "PPM Audio Card",    // Product
    "123321"             // Serial
};

// Преобразование дБ в значение громкости
// Таблица значений для преобразования уровня громкости в коэффициент
static uint16_t db_to_vol[91] = {
    0x0001, 0x0001, 0x0001, 0x0001, 0x0001, 0x0001, 0x0002, 0x0002, 0x0002, 0x0002, 0x0003, 0x0003, 0x0004, 0x0004, 0x0005, 0x0005, 0x0006, 0x0007, 0x0008, 0x0009, 0x000a, 0x000b, 0x000d, 0x000e, 0x0010, 0x0012, 0x0014, 0x0017, 0x001a, 0x001d, 0x0020, 0x0024, 0x0029, 0x002e, 0x0033, 0x003a, 0x0041, 0x0049, 0x0052, 0x005c, 0x0067, 0x0074, 0x0082, 0x0092, 0x00a4, 0x00b8, 0x00ce, 0x00e7, 0x0104, 0x0124, 0x0147, 0x016f, 0x019c, 0x01ce, 0x0207, 0x0246, 0x028d, 0x02dd, 0x0337, 0x039b, 0x040c, 0x048a, 0x0518, 0x05b7, 0x066a, 0x0732, 0x0813, 0x090f, 0x0a2a, 0x0b68, 0x0ccc, 0x0e5c, 0x101d, 0x1214, 0x1449, 0x16c3, 0x198a, 0x1ca7, 0x2026, 0x2413, 0x287a, 0x2d6a, 0x32f5, 0x392c, 0x4026, 0x47fa, 0x50c3, 0x5a9d, 0x65ac, 0x7214, 0x7fff};

#define CENTER_VOLUME_INDEX 91

#define ENCODE_DB(x) ((uint16_t)(int16_t)((x) * 256))

#define MIN_VOLUME        ENCODE_DB(-CENTER_VOLUME_INDEX)
#define DEFAULT_VOLUME    ENCODE_DB(0)
#define MAX_VOLUME        ENCODE_DB(count_of(db_to_vol) - CENTER_VOLUME_INDEX)
#define VOLUME_RESOLUTION ENCODE_DB(1)

static struct usb_interface ac_interface;
static struct usb_interface as_op_interface;
static struct usb_interface as_op_mic_interface;
static struct usb_endpoint  ep_op_out;
static struct usb_endpoint  ep_op_in;
static struct usb_endpoint ep_mic_in;

static bool usb_audio_out_active = false;

// Обновление громкости
static void audio_set_volume(int16_t volume) {
    audio_state.volume = volume;
    volume += CENTER_VOLUME_INDEX * 256;
    if (volume < 0)
        volume = 0;
    if (volume >= count_of(db_to_vol) * 256)
        volume = count_of(db_to_vol) * 256 - 1;
    audio_state.vol_mul = db_to_vol[((uint16_t)volume) >> 8u];
}

// Обработка аудио данных от хоста
static void _as_audio_packet(struct usb_endpoint *ep) {
    struct usb_buffer *usb_buffer = usb_current_out_packet_buffer(ep);

    if (usb_buffer->data_len >= 2) {
        // Копируем данные в аудио буфер
        memcpy(audio_buffer_out, usb_buffer->data, usb_buffer->data_len > (AUDIO_BUFFER_SIZE * 2) ? (AUDIO_BUFFER_SIZE * 2) : usb_buffer->data_len);

        // Применяем громкость, если не в режиме mute
        if (!audio_state.mute) {
            uint16_t vol_mul    = audio_state.vol_mul;
            int16_t *audio_data = (int16_t *)audio_buffer_out;
            for (int i = 0; i < AUDIO_BUFFER_SIZE; i++) {
                audio_data[i] = (int16_t)((audio_data[i] * vol_mul) >> 15u);
            }
        }
        else {
            // Если mute, заполняем нулями
            memset(audio_buffer_out, 0, AUDIO_BUFFER_SIZE * 2);
        }

        // Извлекаем среднее значение для управления PPM
        int32_t  avg_value = 0;
        int16_t *samples   = (int16_t *)audio_buffer_out;
        for (int i = 0; i < AUDIO_BUFFER_SIZE; i++) {
            avg_value += abs(samples[i]);
        }
        avg_value /= AUDIO_BUFFER_SIZE;

        // Преобразуем уровень аудио в PPM код
        ppm_code_to_send = (avg_value * MAX_CODE) / 32767;
        has_custom_value = true;
    }

    // Продолжаем прием данных
    usb_grow_transfer(ep->current_transfer, 1);
    usb_packet_done(ep);
}

static void _as_audio_in_packet(struct usb_endpoint *ep) {
    // Передаем данные из audio_buffer_in в хост
    struct usb_buffer *buffer = usb_current_in_packet_buffer(ep);

    // Определяем максимальный размер передачи
    uint32_t bytes_to_send = AUDIO_BUFFER_SIZE * sizeof(int16_t);    // 16 бит = 2 байта

    // Убедимся, что не превышаем размер буфера
    if (bytes_to_send > buffer->data_max) {
        bytes_to_send = buffer->data_max;
    }

    // Копируем данные
    memcpy(buffer->data, audio_buffer_in, bytes_to_send);

    // Устанавливаем фактический размер передачи
    buffer->data_len = bytes_to_send;

    usb_packet_done(ep);
}

static const struct usb_transfer_type as_transfer_type = {
    .on_packet            = _as_audio_packet,
    .initial_packet_count = 1,
};
static struct usb_transfer as_transfer;

static const struct usb_transfer_type as_in_transfer_type = {
    .on_packet            = _as_audio_in_packet,
    .initial_packet_count = 1,
};
static struct usb_transfer as_in_transfer;

// Обработка запросов управления аудио
static bool do_get_current(struct usb_setup_packet *setup) {
    if ((setup->bmRequestType & USB_REQ_TYPE_RECIPIENT_MASK) == USB_REQ_TYPE_RECIPIENT_INTERFACE) {
        switch (setup->wValue >> 8u) {
            case FEATURE_MUTE_CONTROL: {
                usb_start_tiny_control_in_transfer(audio_state.mute, 1);
                return true;
            }
            case FEATURE_VOLUME_CONTROL: {
                usb_start_tiny_control_in_transfer(audio_state.volume, 2);
                return true;
            }
        }
    }
    else if ((setup->bmRequestType & USB_REQ_TYPE_RECIPIENT_MASK) == USB_REQ_TYPE_RECIPIENT_ENDPOINT) {
        if ((setup->wValue >> 8u) == ENDPOINT_FREQ_CONTROL) {
            // Всегда возвращаем 48kHz в формате 3 байт
            uint32_t freq = AUDIO_SAMPLE_RATE;
            usb_start_tiny_control_in_transfer(freq, 3);
            return true;
        }
    }
    return false;
}

static bool do_get_minimum(struct usb_setup_packet *setup) {
    if ((setup->bmRequestType & USB_REQ_TYPE_RECIPIENT_MASK) == USB_REQ_TYPE_RECIPIENT_INTERFACE) {
        switch (setup->wValue >> 8u) {
            case FEATURE_VOLUME_CONTROL: {
                usb_start_tiny_control_in_transfer(MIN_VOLUME, 2);
                return true;
            }
        }
    }
    return false;
}

static bool do_get_maximum(struct usb_setup_packet *setup) {
    if ((setup->bmRequestType & USB_REQ_TYPE_RECIPIENT_MASK) == USB_REQ_TYPE_RECIPIENT_INTERFACE) {
        switch (setup->wValue >> 8u) {
            case FEATURE_VOLUME_CONTROL: {
                usb_start_tiny_control_in_transfer(MAX_VOLUME, 2);
                return true;
            }
        }
    }
    return false;
}

static bool do_get_resolution(struct usb_setup_packet *setup) {
    if ((setup->bmRequestType & USB_REQ_TYPE_RECIPIENT_MASK) == USB_REQ_TYPE_RECIPIENT_INTERFACE) {
        switch (setup->wValue >> 8u) {
            case FEATURE_VOLUME_CONTROL: {
                usb_start_tiny_control_in_transfer(VOLUME_RESOLUTION, 2);
                return true;
            }
        }
    }
    return false;
}

static struct audio_control_cmd {
    uint8_t cmd;
    uint8_t type;
    uint8_t cs;
    uint8_t cn;
    uint8_t unit;
    uint8_t len;
} audio_control_cmd_t;

static void audio_cmd_packet(struct usb_endpoint *ep) {
    assert(audio_control_cmd_t.cmd == AUDIO_REQ_SetCurrent);
    struct usb_buffer *buffer = usb_current_out_packet_buffer(ep);
    audio_control_cmd_t.cmd   = 0;
    if (buffer->data_len >= audio_control_cmd_t.len) {
        if (audio_control_cmd_t.type == USB_REQ_TYPE_RECIPIENT_INTERFACE) {
            switch (audio_control_cmd_t.cs) {
                case FEATURE_MUTE_CONTROL: {
                    audio_state.mute = buffer->data[0];
                    break;
                }
                case FEATURE_VOLUME_CONTROL: {
                    audio_set_volume(*(int16_t *)buffer->data);
                    break;
                }
            }
        }
        else if (audio_control_cmd_t.type == USB_REQ_TYPE_RECIPIENT_ENDPOINT) {
            if (audio_control_cmd_t.cs == ENDPOINT_FREQ_CONTROL) {
                (void)(*(uint32_t *)buffer->data);
                audio_state.freq = AUDIO_SAMPLE_RATE;
            }
        }
    }
    usb_start_empty_control_in_transfer_null_completion();
}

static const struct usb_transfer_type _audio_cmd_transfer_type = {
    .on_packet            = audio_cmd_packet,
    .initial_packet_count = 1,
};

static void usb_start_control_out_transfer_null_completion(void) {
    static const struct usb_transfer_type null_transfer_type = {
        .on_packet            = NULL,
        .initial_packet_count = 1,
    };
    static struct usb_transfer null_transfer = {
        .type = &null_transfer_type,
    };
    usb_start_control_out_transfer(&null_transfer_type);
}

static bool as_set_alternate(struct usb_interface *interface, uint alt) {
    usb_audio_out_active = (alt == 1);
    return alt < 2;
}

static bool do_set_current(struct usb_setup_packet *setup) {
    if (setup->wLength && setup->wLength < 64) {
        audio_control_cmd_t.cmd  = AUDIO_REQ_SetCurrent;
        audio_control_cmd_t.type = setup->bmRequestType & USB_REQ_TYPE_RECIPIENT_MASK;
        audio_control_cmd_t.len  = (uint8_t)setup->wLength;
        audio_control_cmd_t.unit = setup->wIndex >> 8u;
        audio_control_cmd_t.cs   = setup->wValue >> 8u;
        audio_control_cmd_t.cn   = (uint8_t)setup->wValue;

        if ((audio_control_cmd_t.type == USB_REQ_TYPE_RECIPIENT_ENDPOINT) &&
            (audio_control_cmd_t.cs == ENDPOINT_FREQ_CONTROL)) {

            // Извлекаем номер эндпоинта из младших 8 бит wIndex
            uint8_t endpoint = setup->wIndex & 0xFF;

            // Принимаем запрос, но оставляем нашу фиксированную частоту
            audio_state.freq = AUDIO_SAMPLE_RATE;

            if (setup->wLength > 0) {
                // Подготавливаем буфер для запроса с данными и возвращаем успех
                usb_start_control_out_transfer_null_completion();
            }
            else {
                // Для запросов без данных отправляем пустой пакет
                usb_start_empty_control_in_transfer_null_completion();
            }
            return true;
        }

        usb_start_control_out_transfer(&_audio_cmd_transfer_type);
        return true;
    }
    return false;
}

static bool ac_setup_request_handler(__unused struct usb_interface *interface, struct usb_setup_packet *setup) {
    setup = __builtin_assume_aligned(setup, 4);
    if (USB_REQ_TYPE_TYPE_CLASS == (setup->bmRequestType & USB_REQ_TYPE_TYPE_MASK)) {
        switch (setup->bRequest) {
            case AUDIO_REQ_SetCurrent:
                return do_set_current(setup);
            case AUDIO_REQ_GetCurrent:
                return do_get_current(setup);
            case AUDIO_REQ_GetMinimum:
                return do_get_minimum(setup);
            case AUDIO_REQ_GetMaximum:
                return do_get_maximum(setup);
            case AUDIO_REQ_GetResolution:
                return do_get_resolution(setup);
            default:
                break;
        }
    }
    return false;
}

bool _as_setup_request_handler(__unused struct usb_endpoint *ep, struct usb_setup_packet *setup) {
    setup = __builtin_assume_aligned(setup, 4);
    if (USB_REQ_TYPE_TYPE_CLASS == (setup->bmRequestType & USB_REQ_TYPE_TYPE_MASK)) {
        switch (setup->bRequest) {
            case AUDIO_REQ_SetCurrent:
                return do_set_current(setup);
            case AUDIO_REQ_GetCurrent:
                return do_get_current(setup);
            case AUDIO_REQ_GetMinimum:
                return do_get_minimum(setup);
            case AUDIO_REQ_GetMaximum:
                return do_get_maximum(setup);
            case AUDIO_REQ_GetResolution:
                return do_get_resolution(setup);
            default:
                break;
        }
    }
    return false;
}

const char *_get_descriptor_string(uint index) {
    if (index <= count_of(string_descriptors)) {
        return string_descriptors[index - 1];
    }
    else {
        return "";
    }
}

// Инициализация USB аудио устройства
void usb_audio_init() {
    usb_interface_init(&ac_interface, &audio_device_config.ac_interface, NULL, 0, true);
    ac_interface.setup_request_handler = ac_setup_request_handler;

    // Только один потоковый интерфейс с двумя endpoint'ами
    static struct usb_endpoint *const op_endpoints[] = {&ep_op_out, &ep_op_in};
    usb_interface_init(&as_op_interface, &audio_device_config.as_op_interface, op_endpoints, 2, true);

    as_op_interface.set_alternate_handler = as_set_alternate;
    ep_op_out.setup_request_handler       = _as_setup_request_handler;
    ep_op_in.setup_request_handler        = _as_setup_request_handler;

    as_transfer.type = &as_transfer_type;
    usb_set_default_transfer(&ep_op_out, &as_transfer);

    as_in_transfer.type = &as_in_transfer_type;
    usb_set_default_transfer(&ep_op_in, &as_in_transfer);

    // Только два интерфейса!
    static struct usb_interface *const device_interfaces[] = {
        &ac_interface,
        &as_op_interface,
    };

    struct usb_device *device = usb_device_init(
        &device_descriptor,
        &audio_device_config.descriptor,
        device_interfaces,
        count_of(device_interfaces),
        _get_descriptor_string);
    assert(device);

    audio_set_volume(DEFAULT_VOLUME);
    usb_device_start();
}

// ============================================================================
// CORE 0 FUNCTIONS - PPM Reception
// ============================================================================

void update_measurements() {
    while (detector_running && !pio_sm_is_rx_fifo_empty(pio0_instance, sm_det)) {
        uint32_t measured_width  = pio_sm_get(pio0_instance, sm_det);
        uint32_t corrected_width = (measured_width + MIN_TACKT) - MIN_INTERVAL_CYCLES;

        if (corrected_width > 0 && corrected_width <= MAX_CODE) {
            int16_t audio_sample = (int16_t)((corrected_width * 32767) / MAX_CODE);

            // Заполняем буфер для USB аудио IN (микрофон)
            for (int i = 0; i < AUDIO_BUFFER_SIZE; i++) {
                audio_buffer_in[i] = audio_sample;
            }

            // Send audio sample to Core 1 via FIFO
            if (multicore_fifo_wready()) {
                multicore_fifo_push_blocking(audio_sample);
            }
        }
    }
}

void init_pulse_detector(float freq) {
    sm_det               = pio_claim_unused_sm(pio0_instance, true);
    uint          offset = pio_add_program(pio0_instance, &pulse_detector_program);
    pio_sm_config c      = pulse_detector_program_get_default_config(offset);

    sm_config_set_in_pins(&c, PULSE_DET_PIN);
    sm_config_set_jmp_pin(&c, PULSE_DET_PIN);
    pio_gpio_init(pio0_instance, PULSE_DET_PIN);
    pio_sm_set_consecutive_pindirs(pio0_instance, sm_det, PULSE_DET_PIN, 1, false);

    sm_config_set_clkdiv(&c, (float)clock_get_hz(clk_sys) / freq);
    pio_sm_init(pio0_instance, sm_det, offset, &c);
}

void start_detector() {
    pio_sm_clear_fifos(pio0_instance, sm_det);
    pio_sm_set_enabled(pio0_instance, sm_det, true);
    detector_running = true;
}

void first_core_main() {
    init_pulse_detector(PIO_FREQ);
    start_detector();

    bool            led_state       = false;
    absolute_time_t next_led_toggle = make_timeout_time_ms(LED_TIME);

    while (1) {
        update_measurements();

        if (absolute_time_diff_us(get_absolute_time(), next_led_toggle) <= 0) {
            led_state = !led_state;
            gpio_put(LED_PIN, led_state);
            next_led_toggle = make_timeout_time_ms(LED_TIME);
        }

        tight_loop_contents();
    }
}

// ============================================================================
// CORE 1 FUNCTIONS - USB Audio and PPM Generation
// ============================================================================

void generate_pulse(uint32_t pause_width) {
    pio_sm_put_blocking(pio1_instance, sm_gen, pause_width);
}

void timer0_irq_handler() {
    if (timer_hw->intr & (1u << 0)) {
        timer_hw->intr = 1u << 0;

        uint32_t ppm_value;
        if (has_custom_value) {
            ppm_value        = MIN_INTERVAL_CYCLES + ppm_code_to_send;
            has_custom_value = false;
        }
        else {
            ppm_value = MIN_INTERVAL_CYCLES;
        }

        generate_pulse(ppm_value);
        timer_hw->alarm[0] = timer_hw->timerawl + AUDIO_FRAME_TICKS;
    }
}

void init_pulse_generator(float freq) {
    sm_gen               = pio_claim_unused_sm(pio1_instance, true);
    uint          offset = pio_add_program(pio1_instance, &pulse_generator_program);
    pio_sm_config c      = pulse_generator_program_get_default_config(offset);

    sm_config_set_set_pins(&c, PULSE_GEN_PIN, 1);
    pio_gpio_init(pio1_instance, PULSE_GEN_PIN);
    pio_sm_set_consecutive_pindirs(pio1_instance, sm_gen, PULSE_GEN_PIN, 1, true);

    sm_config_set_clkdiv(&c, (float)clock_get_hz(clk_sys) / freq);

    pio_sm_init(pio1_instance, sm_gen, offset, &c);
    pio_sm_set_enabled(pio1_instance, sm_gen, true);
}

void second_core_main() {
    // Initialize USB device and audio
    usb_audio_init();

    printf("USB Audio Device started\n");

    // Initialize PPM generator
    init_pulse_generator(PIO_FREQ);

    // Setup timer interrupt for PPM generation
    irq_set_exclusive_handler(TIMER_IRQ_0, timer0_irq_handler);
    hw_set_bits(&timer_hw->inte, (1u << 0));
    irq_set_enabled(TIMER_IRQ_0, true);
    timer_hw->alarm[0] = timer_hw->timerawl + AUDIO_FRAME_TICKS;

    while (1) {
        // Process audio from Core 0
        if (multicore_fifo_rvalid()) {
            multicore_fifo_pop_blocking();    // просто очищаем FIFO
        }
        __wfi();
    }
}

// ============================================================================
// MAIN FUNCTION
// ============================================================================

int main() {
    // Initialize system
    set_sys_clock_khz(SYS_FREQ, true);
    stdio_init_all();

    // Initialize LEDs
    gpio_init(LED_PIN);
    gpio_set_dir(LED_PIN, GPIO_OUT);

    // Reset and launch Core1
    multicore_reset_core1();
    sleep_ms(100);
    multicore_launch_core1(second_core_main);

    // Start Core0 main loop
    first_core_main();

    return 0;
}
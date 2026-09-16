/* USB descriptors for the POD Go MIDI controller image.
 *
 * This is a standards-compliant USB-MIDI Streaming interface with 512-byte
 * bulk endpoints (docs/10 section 2). Two deliberate differences:
 *
 *   - **No audio.** This image has MIDI and a vendor interface for the log
 *     (docs/10 section 5) - or, in the lab image, the vendor interface alone
 *     (tusb_config.h).
 *
 *   - **The embedded MIDI OUT jack is sourced from us, not from a DIN socket.**
 *     This keeps the interface active on a unit without a physical DIN input
 *     (docs/10). TinyUSB's macro wires the embedded jacks to the endpoints,
 *     which is what we want.
 *
 * Identity: pid.codes VID 0x1209 with PID 0x0001, which is explicitly reserved
 * for internal testing. Using Line 6's 0x0E41 would make our test image
 * indistinguishable from a real unit on the bus, and is not ours to use.
 */

#include "tusb.h"
#include "midi_control/midi_control_version.h"

#define USB_VID   0x1209      /* pid.codes */
#define USB_PID   0x0001      /* reserved for internal testing */

/* ---- device ------------------------------------------------------------ */
static const tusb_desc_device_t desc_device = {
    .bLength            = sizeof(tusb_desc_device_t),
    .bDescriptorType    = TUSB_DESC_DEVICE,
    /* 0x0201, not 0x0200, and it matters: Windows only asks for the BOS
     * descriptor from a device claiming 2.01 or later, and the BOS is what
     * carries the MS OS 2.0 platform capability that gets WinUSB bound to the
     * log interface. At 2.00 the descriptors below are correct and never
     * read. */
    .bcdUSB             = 0x0201,
    .bDeviceClass       = 0x00,          /* per-interface */
    .bDeviceSubClass    = 0x00,
    .bDeviceProtocol    = 0x00,
    .bMaxPacketSize0    = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor           = USB_VID,
    .idProduct          = USB_PID,
    /* One version per interface layout. Windows caches what it learns about a
     * device - including whether it has MS OS descriptors - under
     * VID/PID/bcdDevice, and never revisits that without a new version, so a
     * layout that reuses another's number gets the other's cached answer. That
     * looks exactly like the descriptors being wrong.
     *
     *   0x0100   MIDI alone, no MS OS descriptors - the builds before these
     *   0x0200   the vendor interface alone - the lab image
     *   0x0201   MIDI and the vendor interface - the controller */
#if MIDI_CONTROL_ENABLE_MIDI
    .bcdDevice          = 0x0300,
#else
    .bcdDevice          = 0x0200,
#endif
    .iManufacturer      = 0x01,
    .iProduct           = 0x02,
    .iSerialNumber      = 0x03,
    .bNumConfigurations = 0x01,
};

uint8_t const *tud_descriptor_device_cb(void)
{
    return (uint8_t const *)&desc_device;
}

/* ---- configuration ----------------------------------------------------- */

/* With MIDI off the vendor interface is the only one, and the device stops
 * being composite at all - which is the point of the switch. The numbering
 * follows automatically rather than being written twice. */
enum {
#if MIDI_CONTROL_ENABLE_MIDI
    ITF_NUM_AUDIO_CONTROL = 0,
    ITF_NUM_MIDI_STREAMING,
    ITF_NUM_VENDOR,
#else
    ITF_NUM_VENDOR = 0,
#endif
    ITF_NUM_TOTAL
};

/* The established MIDI endpoint numbers are retained for host compatibility.
 * The vendor pair sits above them and is ours alone. */
#define EPNUM_MIDI_OUT    0x02
#define EPNUM_MIDI_IN     0x82
#define EPNUM_VENDOR_OUT  0x03
#define EPNUM_VENDOR_IN   0x83

#if MIDI_CONTROL_ENABLE_MIDI
  #define CONFIG_MIDI_LEN  TUD_MIDI_DESC_LEN
#else
  #define CONFIG_MIDI_LEN  0
#endif

#define CONFIG_TOTAL_LEN  (TUD_CONFIG_DESC_LEN + CONFIG_MIDI_LEN + \
                           TUD_VENDOR_DESC_LEN)

static const uint8_t desc_configuration[] = {
    /* config number, interface count, string index, total length, attribute,
     * power in mA. Self-powered: the unit has its own supply (docs/10). */
    TUD_CONFIG_DESCRIPTOR(1, ITF_NUM_TOTAL, 0, CONFIG_TOTAL_LEN,
                          TUSB_DESC_CONFIG_ATT_SELF_POWERED, 0),

#if MIDI_CONTROL_ENABLE_MIDI
    /* interface number, string index, EP OUT, EP IN, EP size */
    TUD_MIDI_DESCRIPTOR(ITF_NUM_AUDIO_CONTROL, 4,
                        EPNUM_MIDI_OUT, EPNUM_MIDI_IN, 512),
#endif

    TUD_VENDOR_DESCRIPTOR(ITF_NUM_VENDOR, 5,
                          EPNUM_VENDOR_OUT, EPNUM_VENDOR_IN, 512),
};

/* A configuration descriptor whose wTotalLength disagrees with the bytes that
 * follow it is one of the two classic causes of "this device cannot start"
 * (code 10) on the composite parent - the other being bNumInterfaces. Neither
 * is visible from the host beyond that one error, so both get checked here
 * where the compiler can see them. */
TU_VERIFY_STATIC(sizeof(desc_configuration) == CONFIG_TOTAL_LEN,
                 "configuration descriptor length does not match its header");

/* ---- WinUSB, without an INF -------------------------------------------- */

/* Windows binds a vendor-specific interface to nothing at all by default, and
 * an unbound interface is invisible to WinUsb_Initialize. The way out is the
 * Microsoft OS 2.0 descriptors: a BOS platform capability points at a
 * descriptor set that names WINUSB as the compatible ID for this one function
 * and declares a device interface GUID for it. Windows reads that during
 * enumeration and installs WinUSB itself.
 *
 * The GUID below is ours and arbitrary - it only has to match what
 * tools/midi_control_usb.py passes to SetupDiGetClassDevs. It is deliberately not the
 * one from L6WinUsb.inf: this is our device and it should not answer to Line
 * 6's identifier.
 *
 * The lengths are hand-computed and have to be exact, because Windows silently
 * ignores a malformed set and the symptom is simply no device interface ever
 * appearing. Each subset header carries the total length of itself plus
 * everything nested inside it. */
#define MS_OS_20_VENDOR_CODE  0x41

/* The registry property is the same either way: the name, then the GUID as a
 * REG_MULTI_SZ, which is why the GUID is followed by two terminators rather
 * than one. 2+2+2+2 header and name length, 42 bytes of name, 2 for the data
 * length, 80 of data. */
#define MS_OS_20_PROP_LEN     132u
#define MS_OS_20_COMPAT_LEN   20u

/* Composite and non-composite devices want *different shapes* of descriptor
 * set, and this is not a detail Windows is forgiving about.
 *
 * A composite device nests the compatible ID and the property inside a
 * configuration subset and then a function subset, so they attach to one
 * function out of several. A device with a single interface has no function to
 * single out, and Microsoft's layout puts both directly in the set header. Use
 * the nested form on a non-composite device and the property is liable to be
 * ignored - which looks exactly like the GUID being wrong, and is the sort of
 * thing that costs a power cycle per guess.
 *
 * So the shape follows the switch that decides whether this device is
 * composite at all. */
#if MIDI_CONTROL_ENABLE_MIDI
  #define MS_OS_20_FUNC_LEN   (8u + MS_OS_20_COMPAT_LEN + MS_OS_20_PROP_LEN)
  #define MS_OS_20_CONF_LEN   (8u + MS_OS_20_FUNC_LEN)
  #define MS_OS_20_TOTAL_LEN  (10u + MS_OS_20_CONF_LEN)
#else
  #define MS_OS_20_TOTAL_LEN  (10u + MS_OS_20_COMPAT_LEN + MS_OS_20_PROP_LEN)
#endif

#define U16C(c)  (uint8_t)(c), 0x00

/* The two pieces that are identical in both shapes, so they are written once. */
#define MS_OS_20_COMPATIBLE_ID \
    U16_TO_U8S_LE(MS_OS_20_COMPAT_LEN), U16_TO_U8S_LE(0x0003), \
    'W', 'I', 'N', 'U', 'S', 'B', 0x00, 0x00, \
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00

#define MS_OS_20_REGISTRY_PROPERTY \
    U16_TO_U8S_LE(MS_OS_20_PROP_LEN), U16_TO_U8S_LE(0x0004), \
    U16_TO_U8S_LE(7), U16_TO_U8S_LE(42), \
    U16C('D'), U16C('e'), U16C('v'), U16C('i'), U16C('c'), U16C('e'), \
    U16C('I'), U16C('n'), U16C('t'), U16C('e'), U16C('r'), U16C('f'), \
    U16C('a'), U16C('c'), U16C('e'), U16C('G'), U16C('U'), U16C('I'), \
    U16C('D'), U16C('s'), 0x00, 0x00, \
    U16_TO_U8S_LE(80), \
    U16C('{'), U16C('6'), U16C('E'), U16C('4'), U16C('B'), U16C('1'), \
    U16C('F'), U16C('2'), U16C('A'), U16C('-'), U16C('0'), U16C('C'), \
    U16C('3'), U16C('D'), U16C('-'), U16C('4'), U16C('A'), U16C('5'), \
    U16C('E'), U16C('-'), U16C('9'), U16C('B'), U16C('7'), U16C('C'), \
    U16C('-'), U16C('8'), U16C('D'), U16C('1'), U16C('E'), U16C('2'), \
    U16C('F'), U16C('3'), U16C('A'), U16C('4'), U16C('B'), U16C('5'), \
    U16C('C'), U16C('}'), 0x00, 0x00, 0x00, 0x00

static const uint8_t desc_ms_os_20[] = {
    /* set header: length, type 0x00, Windows 8.1+, total length */
    U16_TO_U8S_LE(10), U16_TO_U8S_LE(0x0000),
    U32_TO_U8S_LE(0x06030000), U16_TO_U8S_LE(MS_OS_20_TOTAL_LEN),

#if MIDI_CONTROL_ENABLE_MIDI
    /* configuration subset: length, type 0x01, config index 0, reserved */
    U16_TO_U8S_LE(8), U16_TO_U8S_LE(0x0001),
    0, 0, U16_TO_U8S_LE(MS_OS_20_CONF_LEN),

    /* function subset: length, type 0x02, first interface, reserved */
    U16_TO_U8S_LE(8), U16_TO_U8S_LE(0x0002),
    ITF_NUM_VENDOR, 0, U16_TO_U8S_LE(MS_OS_20_FUNC_LEN),
#endif

    MS_OS_20_COMPATIBLE_ID,
    MS_OS_20_REGISTRY_PROPERTY,
};

TU_VERIFY_STATIC(sizeof(desc_ms_os_20) == MS_OS_20_TOTAL_LEN,
                 "MS OS 2.0 descriptor set length does not match its header");

#define BOS_TOTAL_LEN  (TUD_BOS_DESC_LEN + TUD_BOS_MICROSOFT_OS_DESC_LEN)

static const uint8_t desc_bos[] = {
    TUD_BOS_DESCRIPTOR(BOS_TOTAL_LEN, 1),
    TUD_BOS_MS_OS_20_DESCRIPTOR(MS_OS_20_TOTAL_LEN, MS_OS_20_VENDOR_CODE),
};

uint8_t const *tud_descriptor_bos_cb(void)
{
    return desc_bos;
}

/* Windows asks for the descriptor set with a vendor request on the device,
 * using the code we advertised in the BOS. Nothing else is served here. */
bool tud_vendor_control_xfer_cb(uint8_t rhport, uint8_t stage,
                                tusb_control_request_t const *request)
{
    if (stage != CONTROL_STAGE_SETUP)
        return true;

    if (request->bmRequestType_bit.type == TUSB_REQ_TYPE_VENDOR &&
        request->bRequest == MS_OS_20_VENDOR_CODE &&
        request->wIndex == 7) {          /* 7 = get the descriptor set */
        return tud_control_xfer(rhport, request, (void *)(uintptr_t)desc_ms_os_20,
                                sizeof(desc_ms_os_20));
    }

    return false;
}

uint8_t const *tud_descriptor_configuration_cb(uint8_t index)
{
    (void)index;
    return desc_configuration;
}

/* ---- strings ----------------------------------------------------------- */
/* Every name here is MIDI_CONTROL_PRODUCT_NAME. The MIDI interface's string is the
 * port name a host shows, and the app finds the unit by it. */
static const char *const string_desc_arr[] = {
    (const char[]){0x09, 0x04},          /* 0: language, en-US */
    MIDI_CONTROL_PRODUCT_NAME,           /* 1: manufacturer   */
    MIDI_CONTROL_PRODUCT_NAME,           /* 2: product        */
    "000001",                            /* 3: serial         */
    MIDI_CONTROL_PRODUCT_NAME " MIDI",   /* 4: MIDI interface */
    MIDI_CONTROL_PRODUCT_NAME " Log",    /* 5: vendor interface */
};

static uint16_t _desc_str[32];

uint16_t const *tud_descriptor_string_cb(uint8_t index, uint16_t langid)
{
    (void)langid;

    size_t chr_count;

    if (index == 0) {
        _desc_str[1] = 0x0409;
        chr_count = 1;
    } else {
        if (index >= TU_ARRAY_SIZE(string_desc_arr))
            return NULL;

        const char *str = string_desc_arr[index];
        chr_count = strlen(str);
        const size_t max_count = TU_ARRAY_SIZE(_desc_str) - 1;
        if (chr_count > max_count)
            chr_count = max_count;

        for (size_t i = 0; i < chr_count; i++)
            _desc_str[1 + i] = str[i];
    }

    /* first word is the length and descriptor type */
    _desc_str[0] = (uint16_t)((TUSB_DESC_STRING << 8) | (2 * chr_count + 2));
    return _desc_str;
}

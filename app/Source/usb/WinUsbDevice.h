#pragma once

/* A thin WinUSB wrapper, Windows only. Mirrors tools/midi_control_usb.py's
 * WinUsbDevice class: enumeration by device-interface GUID, overlapped bulk
 * transfers with a per-pipe timeout, control transfers. No JUCE here so the
 * Windows headers stay out of the rest of the app. */

#include <cstdint>
#include <string>
#include <vector>

class WinUsbDevice
{
public:
    struct Guid
    {
        uint32_t data1;
        uint16_t data2, data3;
        uint8_t data4[8];
    };

    /* Parse "{FD7BDAAB-7D76-B4C8-B9BE-209C747294D0}". */
    static Guid parseGuid (const char* text);

    /* Every present device path exposing the interface GUID. Paths carry the
     * VID and PID in the form "vid_1fc9&pid_000c", lower case. */
    static std::vector<std::string> enumeratePaths (const Guid& guid);

    struct Pipe
    {
        uint8_t id;
        uint8_t type;        /* 0 control, 1 isochronous, 2 bulk, 3 interrupt */
        uint16_t maxPacket;
    };

    WinUsbDevice() = default;
    ~WinUsbDevice();
    WinUsbDevice (const WinUsbDevice&) = delete;
    WinUsbDevice& operator= (const WinUsbDevice&) = delete;

    bool open (const std::string& path);
    void close();
    bool isOpen() const;

    std::vector<Pipe> pipes();
    bool findBulkPipes (uint8_t& in, uint8_t& out);

    void setTimeout (uint8_t pipe, uint32_t ms);
    void resetPipe (uint8_t pipe);

    /* Bytes written, or -1. */
    int write (uint8_t pipe, const void* data, size_t len);

    /* Bytes read; 0 on timeout (not an error); -1 on failure. */
    int read (uint8_t pipe, void* buf, size_t len);

    /* Returns bytes transferred, or -1. `data` may be null for a zero-length
     * transfer. */
    int control (uint8_t requestType, uint8_t request, uint16_t value, uint16_t index,
                 void* data, uint16_t len);

    const std::string& lastError() const { return error; }

private:
    void* fileHandle = nullptr;      /* HANDLE */
    void* winusbHandle = nullptr;    /* WINUSB_INTERFACE_HANDLE */
    std::string error;
};

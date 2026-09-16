#pragma once

/* The unit's USB interfaces, from the host: what is on the bus, the
 * reboot-into-DFU sequence, the boot ROM's DFU download, and our
 * firmware's vendor endpoint (log, eMMC backup, eMMC write). A C++ port of
 * tools/midi_control_usb.py, so the app can install firmware with nothing else
 * installed; the Python tool remains the reference.
 *
 * Windows only. Every function here blocks and is meant for a worker thread;
 * `log` receives progress lines and may be called from that thread.
 *
 * The rules that keep this safe are the same as the Python tool's, and are
 * enforced here rather than left to the caller (docs/06-persistence.md):
 *   - a write to the eMMC is refused by the firmware outside sectors 0..1364;
 *   - the vendor IN pipe is read until quiet before the handle is closed,
 *     because closing mid-stream strands the transfer on the device;
 *   - nothing here writes the eMMC without the caller having a verified
 *     backup, which the FirmwarePanel checks before it calls writeEmmc(). */

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace MidiControlUsb
{
    using Log = std::function<void (const std::string&)>;

    struct Detection
    {
        bool factory = false;       /* 0E41:4247 with the factory WinUSB interface */
        bool dfu = false;           /* 1FC9:000C, the NXP boot ROM */
        bool midiControl = false;   /* our firmware's log interface */
        std::string describe() const;
    };

    Detection detect();

    struct Result
    {
        bool ok = false;
        std::string message;
    };

    /* Send the nine-message prelude that ends in the DFU trigger. The unit
     * drops off the bus and returns as 1FC9:000C. */
    Result rebootToDfu (const Log& log);

    /* Ask our firmware to hand the unit to the boot ROM's DFU loader (vendor
     * command 68, value 127), then wait up to 20 s for 1FC9:000C; ok only if
     * it appears. */
    Result rebootMidiControlToDfu (const Log& log);

    /* DFU_DNLOAD `image` (a .bin with the NXP header) to the boot ROM in
     * 2048-byte blocks, end the transfer with a zero-length DNLOAD and the
     * GETSTATUS that starts the image, then wait up to 30 s for our firmware
     * to enumerate; ok only if it does. RAM only. */
    Result dfuLoad (const std::vector<uint8_t>& image, const Log& log);

    /* Our firmware's vendor endpoint. */
    class VendorSession
    {
    public:
        VendorSession();
        ~VendorSession();

        bool open (std::string& error);
        bool isOpen() const;
        void close();

        /* Send a {command, value} pair. */
        bool command (uint8_t cc, uint8_t value);

        /* Ask for the log (replayed from the start) and read until quiet. */
        std::string readLog (bool replay, double budgetSeconds);

        /* Command 79: sectors 0..1364. Returns true and fills `out` with the
         * declared length when complete. */
        bool dumpEmmc (std::vector<uint8_t>& out, const Log& log);

        /* Command 76: write `data` (whole sectors) at `lba`, and return the
         * device's result text. Success is the text containing
         * "WRITTEN AND VERIFIED". Refuses past sector 1364 before sending. */
        std::string writeEmmc (uint32_t lba, const std::vector<uint8_t>& data, const Log& log);

        /* Send a command and return the single result packet it answers with. */
        std::string readResult (uint8_t cc, uint8_t value);

    private:
        struct Impl;
        Impl* impl;
    };
}

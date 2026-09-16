#include "MidiControlUsb.h"
#include "WinUsbDevice.h"
#include "midi_control/midi_control_version.h"

#include <chrono>
#include <cstring>
#include <thread>

namespace MidiControlUsb
{

/* Windows interface GUID shared by the factory-system message pipe and the
 * boot-ROM DFU device. */
static const char* const kLine6Guid = "{FD7BDAAB-7D76-B4C8-B9BE-209C747294D0}";

/* Ours, declared by the firmware's MS OS 2.0 descriptors (usb_descriptors.c).
 * Must match. */
static const char* const kMidiControlGuid = "{6E4B1F2A-0C3D-4A5E-9B7C-8D1E2F3A4B5C}";

static const uint8_t kLogEpIn = 0x83;
static const uint8_t kLogEpOut = 0x03;
static const uint32_t kLogPacket = 512;
static const uint32_t kLogTimeoutMs = 150;
static const uint32_t kSafeSectors = 1365;

static void sleepMs (int ms)
{
    std::this_thread::sleep_for (std::chrono::milliseconds (ms));
}

static double now()
{
    using namespace std::chrono;
    return duration<double> (steady_clock::now().time_since_epoch()).count();
}

static std::string hex (const uint8_t* d, size_t n)
{
    static const char* digits = "0123456789abcdef";
    std::string s;
    for (size_t i = 0; i < n; ++i)
    {
        if (i)
            s += ' ';
        s += digits[d[i] >> 4];
        s += digits[d[i] & 15];
    }
    return s;
}

static std::vector<uint8_t> fromHex (const char* text)
{
    std::vector<uint8_t> out;
    int hi = -1;
    for (const char* p = text; *p; ++p)
    {
        int v;
        if (*p >= '0' && *p <= '9') v = *p - '0';
        else if (*p >= 'a' && *p <= 'f') v = *p - 'a' + 10;
        else if (*p >= 'A' && *p <= 'F') v = *p - 'A' + 10;
        else continue;
        if (hi < 0)
            hi = v;
        else
        {
            out.push_back ((uint8_t) (hi * 16 + v));
            hi = -1;
        }
    }
    return out;
}

/* ---- detection ---------------------------------------------------------- */

std::string Detection::describe() const
{
    std::string s;
    if (factory) s += "POD Go (factory system)";
    if (dfu) s += std::string (s.empty() ? "" : ", ") + "POD Go in Firmware Update Mode (boot ROM)";
    if (midiControl) s += std::string (s.empty() ? "" : ", ") + MIDI_CONTROL_PRODUCT_NAME " firmware";
    return s.empty() ? "no POD Go on the bus" : s;
}

Detection detect()
{
    Detection d;
    for (auto& p : WinUsbDevice::enumeratePaths (WinUsbDevice::parseGuid (kLine6Guid)))
    {
        if (p.find ("vid_0e41") != std::string::npos)
            d.factory = true;
        if (p.find ("vid_1fc9") != std::string::npos)
            d.dfu = true;
    }
    d.midiControl = ! WinUsbDevice::enumeratePaths (WinUsbDevice::parseGuid (kMidiControlGuid)).empty();
    return d;
}

/* ---- reboot into DFU ---------------------------------------------------- */

/* The complete prelude POD Go Edit sends immediately before the unit drops
 * into DFU, replayed unchanged with its original sequence numbers. The three
 * writes of aa0000xx are arming steps; the last message is the trigger.
 * Sending the trigger on its own gets the reject status. */
static const struct { const char* label; const char* hex; } kEnterDfu[] = {
    { "hello",                  "0c0000280110ef03000000020001002100100000" },
    { "setup write @0x1000",    "110000180110ef030002000400100000000003000100000003000000" },
    { "setup write @0x1000",    "120000180110ef030003000400100000010000000200000000000000" },
    { "read  @0x100a",          "080000180110ef03000400080a100000" },
    { "write @0x100a aa000001", "0c0000180110ef03000500040a100000aa000001" },
    { "write @0x100e aa000003", "0c0000180110ef03000600040e100000aa000003" },
    { "write @0x1032 aa000002", "0c0000180110ef030007000432100000aa000002" },
    { "read  @0x1036",          "080000180110ef030008000836100000" },
    { "TRIGGER @0x1036",        "080000180110ef030009000236100000" },
};

Result rebootToDfu (const Log& log)
{
    Result r;
    std::string path;
    for (auto& p : WinUsbDevice::enumeratePaths (WinUsbDevice::parseGuid (kLine6Guid)))
        if (p.find ("vid_0e41") != std::string::npos)
        {
            path = p;
            break;
        }
    if (path.empty())
    {
        r.message = "no POD Go factory system on the bus";
        return r;
    }

    WinUsbDevice dev;
    if (! dev.open (path))
    {
        r.message = dev.lastError();
        return r;
    }
    uint8_t in, out;
    if (! dev.findBulkPipes (in, out))
    {
        r.message = "the message pipe has no bulk endpoints";
        return r;
    }
    dev.setTimeout (in, 2000);
    dev.setTimeout (out, 2000);

    /* A stale reply left in the pipe shifts every response by one and makes
     * a working sequence look rejected. */
    uint8_t buf[512];
    int drained = 0;
    while (dev.read (in, buf, sizeof (buf)) > 0)
        ++drained;
    if (drained)
        log ("drained " + std::to_string (drained) + " stale reply(s) from the IN pipe");

    for (auto& step : kEnterDfu)
    {
        auto msg = fromHex (step.hex);
        if (dev.write (out, msg.data(), msg.size()) < 0)
        {
            r.message = std::string ("write failed at '") + step.label + "': " + dev.lastError();
            return r;
        }
        const int n = dev.read (in, buf, sizeof (buf));
        std::string verdict;
        if (n <= 0)
            verdict = "no reply";
        else if (n >= 12 && buf[10] == 0x00 && buf[11] == 0x10)
            verdict = "REJECTED (status 0x0010)";
        else
            verdict = "ok";
        log (std::string ("-> ") + step.label + "  " + verdict);
    }

    r.ok = true;
    r.message = "sequence sent; the unit should drop off the bus and return in Firmware Update Mode within a few seconds";
    return r;
}

/* Our firmware answers the command, then leaves the bus about a second later
 * and hands over to the boot ROM (firmware/src/board.c, board_enter_dfu). The
 * answer only says it was taken; the boot ROM appearing is the result. */
Result rebootMidiControlToDfu (const Log& log)
{
    Result r;
    {
        VendorSession session;
        std::string error;
        if (! session.open (error))
        {
            r.message = error;
            return r;
        }
        const auto answer = session.readResult (68, 127);
        log (answer.find ("REBOOTING INTO DFU") != std::string::npos
                 ? std::string ("the firmware took the command")
                 : "no answer to the command (" + (answer.empty() ? std::string ("nothing") : answer) + ")");
        session.close();
    }

    log ("waiting for Firmware Update Mode (the boot ROM, 1FC9:000C)");
    const double deadline = now() + 20.0;
    Detection d;
    do
    {
        sleepMs (500);
        d = detect();
        if (d.dfu)
        {
            r.ok = true;
            r.message = "the unit is in Firmware Update Mode (boot ROM)";
            return r;
        }
    } while (now() < deadline);

    r.message = d.midiControl ? MIDI_CONTROL_PRODUCT_NAME " is still on the bus: the command was not taken"
                              : "the unit was not in Firmware Update Mode within 20 s of leaving the bus. Power it off, "
                                "then hold PAGE > while powering it on: it comes up in Firmware Update Mode";
    return r;
}

/* ---- DFU download ----------------------------------------------------- */

static const uint8_t DFU_DNLOAD = 1, DFU_GETSTATUS = 3;

static const char* dfuStateName (int s)
{
    static const char* names[] = { "appIDLE", "appDETACH", "dfuIDLE", "dfuDNLOAD-SYNC", "dfuDNBUSY",
                                   "dfuDNLOAD-IDLE", "dfuMANIFEST-SYNC", "dfuMANIFEST",
                                   "dfuMANIFEST-WAIT-RESET", "dfuUPLOAD-IDLE", "dfuERROR" };
    return s >= 0 && s <= 10 ? names[s] : "?";
}

Result dfuLoad (const std::vector<uint8_t>& image, const Log& log)
{
    Result r;
    if (image.size() < 16 || image[0] != 0x1A || image[1] != 0x00)
    {
        r.message = "not an NXP boot image (no 1a 00 header)";
        return r;
    }

    std::string path;
    for (auto& p : WinUsbDevice::enumeratePaths (WinUsbDevice::parseGuid (kLine6Guid)))
        if (p.find ("vid_1fc9") != std::string::npos)
        {
            path = p;
            break;
        }
    if (path.empty())
    {
        r.message = "the unit is not in Firmware Update Mode (no 1FC9:000C on the bus) - put it there first (step 1)";
        return r;
    }

    const size_t xfer = 2048;
    const size_t nblk = (image.size() + xfer - 1) / xfer;
    {
        WinUsbDevice dev;
        if (! dev.open (path))
        {
            r.message = dev.lastError();
            return r;
        }

        uint8_t st[6];
        if (dev.control (0xA1, DFU_GETSTATUS, 0, 0, st, 6) == 6)
            log (std::string ("initial state: ") + dfuStateName (st[4]) + " (status " + std::to_string (st[0]) + ")");
        else
            log ("initial GETSTATUS failed");

        log ("DFU_DNLOAD " + std::to_string (image.size()) + " bytes in " + std::to_string (nblk) + " blocks");
        bool jumped = false;
        for (size_t blk = 0; blk < nblk; ++blk)
        {
            const size_t off = blk * xfer;
            const size_t len = std::min (xfer, image.size() - off);
            std::vector<uint8_t> chunk (image.begin() + (long) off, image.begin() + (long) (off + len));
            if (dev.control (0x21, DFU_DNLOAD, (uint16_t) blk, 0, chunk.data(), (uint16_t) len) < 0)
            {
                /* Once the ROM has the whole image it can jump immediately,
                 * tearing down USB before the status stage completes. */
                log ("block " + std::to_string (blk) + ": transfer reported an error - the ROM may have jumped");
                jumped = true;
                break;
            }
            if (dev.control (0xA1, DFU_GETSTATUS, 0, 0, st, 6) < 6)
            {
                log ("block " + std::to_string (blk) + ": sent, no GETSTATUS response");
                jumped = true;
                break;
            }
            if (st[0] != 0 || st[4] == 10)
            {
                r.message = "firmware update error at block " + std::to_string (blk) + ": status " + std::to_string (st[0])
                          + ", state " + dfuStateName (st[4]);
                return r;
            }
            if (blk == 0 || blk == nblk - 1)
                log ("block " + std::to_string (blk) + ": ok, state " + dfuStateName (st[4]));
        }

        if (! jumped)
        {
            /* The zero-length DNLOAD only ends the transfer: the ROM then waits
             * in dfuMANIFEST-SYNC for the host to ask its status, and that
             * request is what starts the image. Without it the download
             * reports success and the unit stays in DFU (docs/09). */
            log ("DFU_DNLOAD zero length (end of transfer)");
            if (dev.control (0x21, DFU_DNLOAD, (uint16_t) nblk, 0, nullptr, 0) < 0)
                log ("no response - expected once the ROM has jumped to the image");
            else if (dev.control (0xA1, DFU_GETSTATUS, 0, 0, st, 6) == 6)
                log (std::string ("state: ") + dfuStateName (st[4]) + " (status " + std::to_string (st[0]) + ")");
            else
                log ("no GETSTATUS response - expected once the ROM has jumped to the image");
        }
    }

    /* A finished download proves only that the ROM took the bytes; the image
     * running is our firmware's interface appearing. The DFU handle is already
     * closed, so nothing holds the device while it goes. */
    log ("waiting for " MIDI_CONTROL_PRODUCT_NAME " to enumerate");
    const double deadline = now() + 30.0;
    Detection d;
    do
    {
        sleepMs (500);
        d = detect();
        if (d.midiControl)
        {
            r.ok = true;
            r.message = "the unit is running " MIDI_CONTROL_PRODUCT_NAME " from RAM";
            return r;
        }
    } while (now() < deadline);

    r.message = d.dfu ? "still in Firmware Update Mode after 30 s: the boot ROM did not start the image. "
                        "Power the unit off, hold PAGE > while powering it on, and load again"
                      : MIDI_CONTROL_PRODUCT_NAME " did not enumerate within 30 s of leaving Firmware Update Mode: "
                        "read the stage marker at the bottom left of the unit's screen (docs/09)";
    return r;
}

/* ---- the vendor session ----------------------------------------------- */

struct VendorSession::Impl
{
    WinUsbDevice dev;

    /* Read until the device has been quiet for `quietReads` empty reads, or
     * the budget runs out. Appends to `out`. Returns bytes received. */
    size_t readUntilQuiet (std::string& out, double budgetSeconds, int quietReads)
    {
        uint8_t buf[512];
        const double deadline = now() + budgetSeconds;
        size_t got = 0;
        int quiet = 0;
        while (now() < deadline)
        {
            const int n = dev.read (kLogEpIn, buf, sizeof (buf));
            if (n > 0)
            {
                out.append ((const char*) buf, (size_t) n);
                got += (size_t) n;
                quiet = 0;
            }
            else
            {
                if (++quiet >= quietReads && got)
                    break;
                if (n < 0)
                    break;
            }
        }
        return got;
    }

    void drain()
    {
        uint8_t buf[512];
        for (int i = 0; i < 4; ++i)
            if (dev.read (kLogEpIn, buf, sizeof (buf)) <= 0)
                break;
    }
};

VendorSession::VendorSession() : impl (new Impl) {}

VendorSession::~VendorSession()
{
    close();
    delete impl;
}

bool VendorSession::open (std::string& error)
{
    auto paths = WinUsbDevice::enumeratePaths (WinUsbDevice::parseGuid (kMidiControlGuid));
    if (paths.empty())
    {
        error = "no " MIDI_CONTROL_PRODUCT_NAME " log interface found. Is the " MIDI_CONTROL_PRODUCT_NAME " firmware running? "
                "If it was loaded before the vendor interface existed, unplug and replug once "
                "so Windows re-reads the descriptors.";
        return false;
    }
    if (! impl->dev.open (paths[0]))
    {
        error = impl->dev.lastError();
        return false;
    }
    impl->dev.resetPipe (kLogEpIn);
    impl->dev.resetPipe (kLogEpOut);
    impl->dev.setTimeout (kLogEpIn, kLogTimeoutMs);
    impl->dev.setTimeout (kLogEpOut, 2000);
    return true;
}

bool VendorSession::isOpen() const
{
    return impl->dev.isOpen();
}

void VendorSession::close()
{
    if (impl->dev.isOpen())
    {
        /* Never close with a transfer queued on the device. */
        impl->drain();
        impl->dev.close();
    }
}

bool VendorSession::command (uint8_t cc, uint8_t value)
{
    const uint8_t b[2] = { cc, value };
    return impl->dev.write (kLogEpOut, b, 2) == 2;
}

std::string VendorSession::readLog (bool replay, double budgetSeconds)
{
    std::string out;
    if (replay)
        command (126, 1);
    impl->readUntilQuiet (out, budgetSeconds, 20);
    impl->drain();
    return out;
}

std::string VendorSession::readResult (uint8_t cc, uint8_t value)
{
    command (cc, value);
    uint8_t buf[512];
    std::string last;
    int quiet = 0;
    for (int i = 0; i < 200; ++i)
    {
        const int n = impl->dev.read (kLogEpIn, buf, sizeof (buf));
        if (n > 0)
        {
            last.assign ((const char*) buf, (size_t) n);
            quiet = 0;
        }
        else if (++quiet >= 8 && ! last.empty())
            break;
    }
    return last;
}

bool VendorSession::dumpEmmc (std::vector<uint8_t>& out, const Log& log)
{
    out.clear();
    if (! command (79, 1))
        return false;

    std::vector<uint8_t> blob;
    uint8_t buf[512];
    size_t need = 0;
    bool started = false;
    int quiet = 0;
    const double deadline = now() + 300.0;
    size_t lastReported = 0;

    while (now() < deadline)
    {
        const int n = impl->dev.read (kLogEpIn, buf, sizeof (buf));
        if (n > 0)
        {
            blob.insert (blob.end(), buf, buf + n);
            started = true;
            quiet = 0;
            if (need == 0 && blob.size() >= 16)
            {
                if (memcmp (blob.data(), "SSEMMCDP", 8) != 0)
                {
                    log ("bad frame header: " + hex (blob.data(), 8));
                    return false;
                }
                const uint32_t len = (uint32_t) blob[8] | ((uint32_t) blob[9] << 8)
                                   | ((uint32_t) blob[10] << 16) | ((uint32_t) blob[11] << 24);
                need = 16 + len;
                log ("device reports " + std::to_string (len) + " bytes (" + std::to_string (len / 512) + " sectors)");
            }
            if (need && blob.size() - lastReported >= 128 * 1024)
            {
                lastReported = blob.size();
                log ("  " + std::to_string (blob.size() - 16) + " of " + std::to_string (need - 16) + " bytes");
            }
            if (need && blob.size() >= need)
                break;
        }
        else
        {
            if (n < 0)
                break;
            if (++quiet >= 40 && started)
                break;
            if (quiet >= 200)
                break;
        }
    }

    if (need == 0)
    {
        log ("no frame received - is the " MIDI_CONTROL_PRODUCT_NAME " firmware running?");
        return false;
    }
    if (blob.size() < need)
    {
        log ("short dump: got " + std::to_string (blob.size()) + " of " + std::to_string (need) + " bytes");
        return false;
    }
    out.assign (blob.begin() + 16, blob.begin() + (long) need);
    impl->drain();
    return true;
}

std::string VendorSession::writeEmmc (uint32_t lba, const std::vector<uint8_t>& data, const Log& log)
{
    std::vector<uint8_t> payload = data;
    if (payload.size() % 512)
        payload.resize (payload.size() + (512 - payload.size() % 512), 0);
    const uint32_t sectors = (uint32_t) (payload.size() / 512);
    if (lba + sectors > kSafeSectors)
        return "REFUSED by the app: the write would run past sector 1364, the end of the backed-up region";

    log ("writing " + std::to_string (payload.size()) + " bytes (" + std::to_string (sectors)
         + " sectors) at LBA " + std::to_string (lba));

    if (! command (76, 127))
        return "could not arm the write: " + impl->dev.lastError();
    sleepMs (200);

    uint8_t header[20] = { 'S', 'S', 'W', 'R', 'I', 'M', 'G', '!' };
    const uint32_t len = (uint32_t) payload.size();
    /* CRC is advisory on the device (it verifies by reading back); send 0. */
    const uint32_t crc = 0;
    memcpy (header + 8, &lba, 4);
    memcpy (header + 12, &len, 4);
    memcpy (header + 16, &crc, 4);
    if (impl->dev.write (kLogEpOut, header, sizeof (header)) < 0)
        return "header write failed: " + impl->dev.lastError();

    size_t sent = 0;
    while (sent < payload.size())
    {
        const size_t chunk = std::min ((size_t) 4096, payload.size() - sent);
        if (impl->dev.write (kLogEpOut, payload.data() + sent, chunk) < 0)
            return "payload write failed at " + std::to_string (sent) + ": " + impl->dev.lastError();
        sent += chunk;
    }
    log ("sent " + std::to_string (sent) + " bytes; waiting for the device to write and verify");

    /* The device writes and reads back before answering, so give it room. */
    uint8_t buf[512];
    std::string got;
    int quiet = 0;
    for (int i = 0; i < 1200; ++i)
    {
        const int n = impl->dev.read (kLogEpIn, buf, sizeof (buf));
        if (n > 0)
        {
            got.assign ((const char*) buf, (size_t) n);
            quiet = 0;
        }
        else if (++quiet >= 10 && ! got.empty())
            break;
    }
    impl->drain();
    if (got.empty())
        return "no answer from the device - read its log to see what happened";
    return got;
}

} // namespace MidiControlUsb

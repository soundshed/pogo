#include "WinUsbDevice.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <setupapi.h>
#include <winusb.h>

static std::string lastErrorText (const char* what)
{
    return std::string (what) + " failed: " + std::to_string (GetLastError());
}

WinUsbDevice::Guid WinUsbDevice::parseGuid (const char* text)
{
    std::string h;
    for (const char* p = text; *p; ++p)
        if (isxdigit ((unsigned char) *p))
            h += *p;
    Guid g {};
    if (h.size() != 32)
        return g;
    g.data1 = (uint32_t) std::stoul (h.substr (0, 8), nullptr, 16);
    g.data2 = (uint16_t) std::stoul (h.substr (8, 4), nullptr, 16);
    g.data3 = (uint16_t) std::stoul (h.substr (12, 4), nullptr, 16);
    for (int i = 0; i < 8; ++i)
        g.data4[i] = (uint8_t) std::stoul (h.substr (16 + (size_t) i * 2, 2), nullptr, 16);
    return g;
}

static GUID toWinGuid (const WinUsbDevice::Guid& g)
{
    GUID w;
    w.Data1 = g.data1;
    w.Data2 = g.data2;
    w.Data3 = g.data3;
    memcpy (w.Data4, g.data4, 8);
    return w;
}

std::vector<std::string> WinUsbDevice::enumeratePaths (const Guid& guid)
{
    std::vector<std::string> out;
    GUID g = toWinGuid (guid);
    HDEVINFO set = SetupDiGetClassDevsW (&g, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (set == INVALID_HANDLE_VALUE)
        return out;

    for (DWORD i = 0;; ++i)
    {
        SP_DEVICE_INTERFACE_DATA ifd {};
        ifd.cbSize = sizeof (ifd);
        if (! SetupDiEnumDeviceInterfaces (set, nullptr, &g, i, &ifd))
            break;
        DWORD needed = 0;
        SetupDiGetDeviceInterfaceDetailW (set, &ifd, nullptr, 0, &needed, nullptr);
        if (needed == 0)
            continue;
        std::vector<uint8_t> buf (needed);
        auto* detail = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W*> (buf.data());
        detail->cbSize = sizeof (SP_DEVICE_INTERFACE_DETAIL_DATA_W);
        if (! SetupDiGetDeviceInterfaceDetailW (set, &ifd, detail, needed, nullptr, nullptr))
            continue;
        std::wstring w (detail->DevicePath);
        std::string s;
        for (wchar_t c : w)
            s += (char) (c < 128 ? tolower ((int) c) : '?');
        out.push_back (s);
    }
    SetupDiDestroyDeviceInfoList (set);
    return out;
}

WinUsbDevice::~WinUsbDevice()
{
    close();
}

bool WinUsbDevice::open (const std::string& path)
{
    close();
    std::wstring w (path.begin(), path.end());
    HANDLE h = CreateFileW (w.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                            nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED, nullptr);
    if (h == INVALID_HANDLE_VALUE)
    {
        error = lastErrorText ("CreateFile") + " - is POD Go Edit or another tool holding the device?";
        return false;
    }
    WINUSB_INTERFACE_HANDLE wh = nullptr;
    if (! WinUsb_Initialize (h, &wh))
    {
        error = lastErrorText ("WinUsb_Initialize");
        CloseHandle (h);
        return false;
    }
    fileHandle = h;
    winusbHandle = wh;
    return true;
}

void WinUsbDevice::close()
{
    if (winusbHandle != nullptr)
    {
        WinUsb_Free ((WINUSB_INTERFACE_HANDLE) winusbHandle);
        winusbHandle = nullptr;
    }
    if (fileHandle != nullptr)
    {
        CloseHandle ((HANDLE) fileHandle);
        fileHandle = nullptr;
    }
}

bool WinUsbDevice::isOpen() const
{
    return winusbHandle != nullptr;
}

std::vector<WinUsbDevice::Pipe> WinUsbDevice::pipes()
{
    std::vector<Pipe> out;
    USB_INTERFACE_DESCRIPTOR d {};
    if (! WinUsb_QueryInterfaceSettings ((WINUSB_INTERFACE_HANDLE) winusbHandle, 0, &d))
        return out;
    for (UCHAR i = 0; i < d.bNumEndpoints; ++i)
    {
        WINUSB_PIPE_INFORMATION p {};
        if (WinUsb_QueryPipe ((WINUSB_INTERFACE_HANDLE) winusbHandle, 0, i, &p))
            out.push_back ({ p.PipeId, (uint8_t) p.PipeType, p.MaximumPacketSize });
    }
    return out;
}

bool WinUsbDevice::findBulkPipes (uint8_t& in, uint8_t& out)
{
    in = out = 0;
    for (auto& p : pipes())
    {
        if (p.type != 2)
            continue;
        if ((p.id & 0x80) && in == 0)
            in = p.id;
        else if (! (p.id & 0x80) && out == 0)
            out = p.id;
    }
    return in != 0 && out != 0;
}

void WinUsbDevice::setTimeout (uint8_t pipe, uint32_t ms)
{
    ULONG v = ms;
    WinUsb_SetPipePolicy ((WINUSB_INTERFACE_HANDLE) winusbHandle, pipe, PIPE_TRANSFER_TIMEOUT, sizeof (v), &v);
}

void WinUsbDevice::resetPipe (uint8_t pipe)
{
    /* ResetPipe only. AbortPipe blocks indefinitely on a pipe that is already
     * wedged, which turns a diagnostic into another hang. */
    WinUsb_ResetPipe ((WINUSB_INTERFACE_HANDLE) winusbHandle, pipe);
}

/* One overlapped transfer, waited for. Returns bytes moved; -1 on timeout or
 * stall; -2 on a hard failure. */
static int transfer (WINUSB_INTERFACE_HANDLE wh, bool isRead, uint8_t pipe, void* buf, size_t len)
{
    OVERLAPPED ov {};
    ov.hEvent = CreateEventW (nullptr, TRUE, FALSE, nullptr);
    ULONG n = 0;
    BOOL ok = isRead ? WinUsb_ReadPipe (wh, pipe, (PUCHAR) buf, (ULONG) len, &n, &ov)
                     : WinUsb_WritePipe (wh, pipe, (PUCHAR) buf, (ULONG) len, &n, &ov);
    int result;
    if (ok)
        result = (int) n;
    else if (GetLastError() != ERROR_IO_PENDING)
        result = -2;
    else if (WinUsb_GetOverlappedResult (wh, &ov, &n, TRUE))
        result = (int) n;
    else
        result = GetLastError() == ERROR_SEM_TIMEOUT ? -1 : -2;
    CloseHandle (ov.hEvent);
    return result;
}

int WinUsbDevice::write (uint8_t pipe, const void* data, size_t len)
{
    std::vector<uint8_t> copy ((const uint8_t*) data, (const uint8_t*) data + len);
    const int n = transfer ((WINUSB_INTERFACE_HANDLE) winusbHandle, false, pipe, copy.data(), len);
    if (n < 0)
    {
        error = lastErrorText ("WritePipe");
        return -1;
    }
    return n;
}

int WinUsbDevice::read (uint8_t pipe, void* buf, size_t len)
{
    const int n = transfer ((WINUSB_INTERFACE_HANDLE) winusbHandle, true, pipe, buf, len);
    if (n == -1)
        return 0;
    if (n < 0)
    {
        error = lastErrorText ("ReadPipe");
        return -1;
    }
    return n;
}

int WinUsbDevice::control (uint8_t requestType, uint8_t request, uint16_t value, uint16_t index,
                           void* data, uint16_t len)
{
    WINUSB_SETUP_PACKET pkt {};
    pkt.RequestType = requestType;
    pkt.Request = request;
    pkt.Value = value;
    pkt.Index = index;
    pkt.Length = len;
    ULONG n = 0;
    uint8_t dummy = 0;
    if (! WinUsb_ControlTransfer ((WINUSB_INTERFACE_HANDLE) winusbHandle, pkt,
                                  (PUCHAR) (data != nullptr ? data : &dummy), len, &n, nullptr))
    {
        error = lastErrorText ("ControlTransfer");
        return -1;
    }
    return (int) n;
}

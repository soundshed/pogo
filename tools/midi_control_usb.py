#!/usr/bin/env python3
"""
Talk to a POD Go over its vendor WinUSB interface, using only ctypes and the
Windows DLLs that are already present. No libusb, no pyusb, no dfu-util.

Line 6 bind both the POD Go's vendor interface and the NXP boot-ROM DFU device
to WinUSB, with the device interface GUID below (taken from L6WinUsb.inf), so
one code path reaches both.

Commands, in increasing order of risk:

  info      Enumerate and print interface/pipe layout. Sends NOTHING to the
            device. Completely passive.

  probe     Send the standard session-open handshake, then read the IN pipe.
            This is routine connection traffic and is read-only with respect
            to device state.

  reboot-dfu  Put the unit into DFU mode: the standard enter-DFU prelude or,
              when our firmware is running, its vendor command 68.
              THIS REBOOTS THE UNIT. Requires --i-mean-it. Reversible - the
              unit lands in DFU mode, comes back normally, or hangs; a power
              cycle fixes all three.

  dfu-load <file>  Download an image to the boot ROM over DFU and let it run.
                   Requires --i-mean-it. The LPC4350 is flashless, so this only
                   ever touches SRAM; a power cycle undoes it entirely.

  log       Read our own firmware's diagnostic log off its vendor bulk
            endpoint and print it. Read-only. Replaces the SysEx-over-MIDI
            path, which works but cannot be driven from a script: winmm
            serialises every capture through one port handle, and a listener
            that is killed - or a second one started while the first still
            holds the port - wedges the whole MIDI subsystem until the device
            re-enumerates. This is the same WinUSB code path as everything
            else here, and it has never once misbehaved.

  cc <n> <v>  Send one command to our firmware over the same endpoint, as the
              two bytes {number, value}. Identical in effect to the MIDI
              control change of the same number.

  result    Ask for the last experiment's answer - a single short packet, and
            the reliable way to get a measurement out. `--run N` runs command
            N first. Prefer this over `log` for anything that is a result
            rather than a narrative: the log is a stream, and a stream is what
            all the trouble in docs/10 section 5 was about.

Nothing here writes to flash or eMMC.
"""

import argparse
import ctypes as C
import struct
import sys
import time
from ctypes import wintypes

# From C:\Program Files (x86)\Line6\Tools\Driver2 Archive\L6WinUsb\L6WinUsb.inf
L6_INTERFACE_GUID = "{FD7BDAAB-7D76-B4C8-B9BE-209C747294D0}"

# Our own firmware's log interface. Declared by the device itself, in the
# MS OS 2.0 descriptor set in firmware/usb-midi/usb_descriptors.c, so Windows
# binds WinUSB to it with no INF and no driver install. Must match that file.
MIDI_CONTROL_LOG_GUID = "{6E4B1F2A-0C3D-4A5E-9B7C-8D1E2F3A4B5C}"

INVALID_HANDLE_VALUE = wintypes.HANDLE(-1).value
GENERIC_READ, GENERIC_WRITE = 0x80000000, 0x40000000
FILE_SHARE_READ, FILE_SHARE_WRITE = 1, 2
OPEN_EXISTING = 3
FILE_ATTRIBUTE_NORMAL = 0x80
FILE_FLAG_OVERLAPPED = 0x40000000
ERROR_IO_PENDING = 997
DIGCF_PRESENT, DIGCF_DEVICEINTERFACE = 0x02, 0x10
PIPE_TRANSFER_TIMEOUT = 0x03

setupapi = C.WinDLL("setupapi", use_last_error=True)
kernel32 = C.WinDLL("kernel32", use_last_error=True)
winusb = C.WinDLL("winusb", use_last_error=True)


class GUID(C.Structure):
    _fields_ = [("Data1", wintypes.DWORD), ("Data2", wintypes.WORD),
                ("Data3", wintypes.WORD), ("Data4", C.c_ubyte * 8)]


class SP_DEVICE_INTERFACE_DATA(C.Structure):
    _fields_ = [("cbSize", wintypes.DWORD), ("InterfaceClassGuid", GUID),
                ("Flags", wintypes.DWORD), ("Reserved", C.POINTER(C.c_ulong))]


class USB_INTERFACE_DESCRIPTOR(C.Structure):
    _fields_ = [(n, C.c_ubyte) for n in (
        "bLength", "bDescriptorType", "bInterfaceNumber", "bAlternateSetting",
        "bNumEndpoints", "bInterfaceClass", "bInterfaceSubClass",
        "bInterfaceProtocol", "iInterface")]


class WINUSB_PIPE_INFORMATION(C.Structure):
    _fields_ = [("PipeType", C.c_int), ("PipeId", C.c_ubyte),
                ("MaximumPacketSize", C.c_ushort), ("Interval", C.c_ubyte)]


class OVERLAPPED(C.Structure):
    _fields_ = [("Internal", C.c_void_p), ("InternalHigh", C.c_void_p),
                ("Offset", wintypes.DWORD), ("OffsetHigh", wintypes.DWORD),
                ("hEvent", wintypes.HANDLE)]


class WINUSB_SETUP_PACKET(C.Structure):
    _pack_ = 1
    _fields_ = [("RequestType", C.c_ubyte), ("Request", C.c_ubyte),
                ("Value", C.c_ushort), ("Index", C.c_ushort),
                ("Length", C.c_ushort)]


PIPE_TYPE = {0: "control", 1: "isochronous", 2: "bulk", 3: "interrupt"}

# Declare prototypes explicitly. Without this, ctypes defaults restype to c_int
# and silently truncates the 64-bit HANDLEs that SetupDiGetClassDevsW and
# CreateFileW return, which makes every subsequent call fail for no visible
# reason.
setupapi.SetupDiGetClassDevsW.restype = wintypes.HANDLE
setupapi.SetupDiGetClassDevsW.argtypes = [C.POINTER(GUID), wintypes.LPCWSTR,
                                          wintypes.HWND, wintypes.DWORD]
setupapi.SetupDiEnumDeviceInterfaces.restype = wintypes.BOOL
setupapi.SetupDiEnumDeviceInterfaces.argtypes = [
    wintypes.HANDLE, C.c_void_p, C.POINTER(GUID), wintypes.DWORD,
    C.POINTER(SP_DEVICE_INTERFACE_DATA)]
setupapi.SetupDiGetDeviceInterfaceDetailW.restype = wintypes.BOOL
setupapi.SetupDiGetDeviceInterfaceDetailW.argtypes = [
    wintypes.HANDLE, C.POINTER(SP_DEVICE_INTERFACE_DATA), C.c_void_p,
    wintypes.DWORD, C.POINTER(wintypes.DWORD), C.c_void_p]
setupapi.SetupDiDestroyDeviceInfoList.restype = wintypes.BOOL
setupapi.SetupDiDestroyDeviceInfoList.argtypes = [wintypes.HANDLE]

kernel32.CreateFileW.restype = wintypes.HANDLE
kernel32.CreateFileW.argtypes = [wintypes.LPCWSTR, wintypes.DWORD, wintypes.DWORD,
                                 C.c_void_p, wintypes.DWORD, wintypes.DWORD,
                                 wintypes.HANDLE]
kernel32.CloseHandle.argtypes = [wintypes.HANDLE]
kernel32.CreateEventW.restype = wintypes.HANDLE
kernel32.CreateEventW.argtypes = [C.c_void_p, wintypes.BOOL, wintypes.BOOL,
                                  wintypes.LPCWSTR]

winusb.WinUsb_Initialize.restype = wintypes.BOOL
winusb.WinUsb_Initialize.argtypes = [wintypes.HANDLE, C.POINTER(C.c_void_p)]
winusb.WinUsb_ResetPipe.restype = wintypes.BOOL
winusb.WinUsb_ResetPipe.argtypes = [C.c_void_p, C.c_ubyte]
winusb.WinUsb_AbortPipe.restype = wintypes.BOOL
winusb.WinUsb_AbortPipe.argtypes = [C.c_void_p, C.c_ubyte]
winusb.WinUsb_Free.argtypes = [C.c_void_p]
winusb.WinUsb_QueryInterfaceSettings.restype = wintypes.BOOL
winusb.WinUsb_QueryInterfaceSettings.argtypes = [
    C.c_void_p, C.c_ubyte, C.POINTER(USB_INTERFACE_DESCRIPTOR)]
winusb.WinUsb_QueryPipe.restype = wintypes.BOOL
winusb.WinUsb_QueryPipe.argtypes = [C.c_void_p, C.c_ubyte, C.c_ubyte,
                                    C.POINTER(WINUSB_PIPE_INFORMATION)]
winusb.WinUsb_SetPipePolicy.restype = wintypes.BOOL
winusb.WinUsb_SetPipePolicy.argtypes = [C.c_void_p, C.c_ubyte, wintypes.ULONG,
                                        wintypes.ULONG, C.c_void_p]
winusb.WinUsb_WritePipe.restype = wintypes.BOOL
winusb.WinUsb_WritePipe.argtypes = [C.c_void_p, C.c_ubyte, C.c_void_p,
                                    wintypes.ULONG, C.POINTER(wintypes.ULONG),
                                    C.c_void_p]
winusb.WinUsb_ReadPipe.restype = wintypes.BOOL
winusb.WinUsb_ReadPipe.argtypes = winusb.WinUsb_WritePipe.argtypes
winusb.WinUsb_ControlTransfer.restype = wintypes.BOOL
winusb.WinUsb_ControlTransfer.argtypes = [C.c_void_p, WINUSB_SETUP_PACKET,
                                          C.c_void_p, wintypes.ULONG,
                                          C.POINTER(wintypes.ULONG), C.c_void_p]
winusb.WinUsb_GetOverlappedResult.restype = wintypes.BOOL
winusb.WinUsb_GetOverlappedResult.argtypes = [C.c_void_p, C.POINTER(OVERLAPPED),
                                              C.POINTER(wintypes.ULONG), wintypes.BOOL]


def parse_guid(s):
    """{FD7BDAAB-7D76-B4C8-B9BE-209C747294D0} -> GUID. Avoids needing ole32."""
    h = s.strip("{}").replace("-", "")
    g = GUID()
    g.Data1 = int(h[0:8], 16)
    g.Data2 = int(h[8:12], 16)
    g.Data3 = int(h[12:16], 16)
    for i in range(8):
        g.Data4[i] = int(h[16 + i * 2:18 + i * 2], 16)
    return g


def device_paths(guid_str):
    """Every device path currently exposing the given interface GUID."""
    g = parse_guid(guid_str)
    hdev = setupapi.SetupDiGetClassDevsW(C.byref(g), None, None,
                                         DIGCF_PRESENT | DIGCF_DEVICEINTERFACE)
    if hdev == INVALID_HANDLE_VALUE:
        raise OSError("SetupDiGetClassDevs failed: %d" % C.get_last_error())
    out, i = [], 0
    try:
        while True:
            ifd = SP_DEVICE_INTERFACE_DATA()
            ifd.cbSize = C.sizeof(SP_DEVICE_INTERFACE_DATA)
            if not setupapi.SetupDiEnumDeviceInterfaces(hdev, None, C.byref(g), i,
                                                        C.byref(ifd)):
                break
            need = wintypes.DWORD()
            setupapi.SetupDiGetDeviceInterfaceDetailW(hdev, C.byref(ifd), None, 0,
                                                      C.byref(need), None)
            buf = C.create_string_buffer(need.value)
            # cbSize is the size of the fixed part only: 8 on 64-bit, 6 on 32-bit.
            struct.pack_into("I", buf, 0, 8 if C.sizeof(C.c_void_p) == 8 else 6)
            if setupapi.SetupDiGetDeviceInterfaceDetailW(hdev, C.byref(ifd), buf,
                                                         need.value, None, None):
                out.append(C.wstring_at(C.addressof(buf) + 4))
            i += 1
    finally:
        setupapi.SetupDiDestroyDeviceInfoList(hdev)
    return out


class WinUsbDevice:
    def __init__(self, path):
        self.path = path
        self.handle = kernel32.CreateFileW(
            wintypes.LPCWSTR(path), GENERIC_READ | GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE, None, OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED, None)
        self.handle = wintypes.HANDLE(self.handle)
        if self.handle.value in (None, INVALID_HANDLE_VALUE):
            raise OSError("CreateFile failed (%d) - is POD Go Edit running?"
                          % C.get_last_error())
        self.wh = C.c_void_p()
        if not winusb.WinUsb_Initialize(self.handle, C.byref(self.wh)):
            kernel32.CloseHandle(self.handle)
            raise OSError("WinUsb_Initialize failed: %d" % C.get_last_error())

    def close(self):
        if self.wh:
            winusb.WinUsb_Free(self.wh)
            self.wh = None
        if self.handle is not None and self.handle.value not in (None, INVALID_HANDLE_VALUE):
            kernel32.CloseHandle(self.handle)
            self.handle = None

    def __enter__(self):
        return self

    def __exit__(self, *a):
        self.close()

    def interface_descriptor(self, alt=0):
        d = USB_INTERFACE_DESCRIPTOR()
        if not winusb.WinUsb_QueryInterfaceSettings(self.wh, C.c_ubyte(alt), C.byref(d)):
            raise OSError("QueryInterfaceSettings failed: %d" % C.get_last_error())
        return d

    def pipes(self, alt=0):
        d = self.interface_descriptor(alt)
        out = []
        for i in range(d.bNumEndpoints):
            p = WINUSB_PIPE_INFORMATION()
            if winusb.WinUsb_QueryPipe(self.wh, C.c_ubyte(alt), C.c_ubyte(i), C.byref(p)):
                out.append(p)
        return out

    def reset_pipe(self, pipe_id):
        """Abort anything in flight and clear the pipe's state.

        Needed because closing a WinUSB handle aborts the host side of a
        transfer but leaves the device thinking one is still outstanding. Our
        firmware then waits forever for a completion that is never coming, and
        the endpoint goes quiet in a way that looks exactly like the firmware
        having died - which cost a diagnosis. Resetting on open makes each run
        independent of how the last one ended."""
        # ResetPipe only. AbortPipe blocks indefinitely on a pipe that is
        # already wedged, which turns a diagnostic into another hang - it was
        # doing exactly that while this was being debugged.
        winusb.WinUsb_ResetPipe(self.wh, C.c_ubyte(pipe_id))

    def set_timeout(self, pipe_id, ms):
        v = wintypes.ULONG(ms)
        winusb.WinUsb_SetPipePolicy(self.wh, C.c_ubyte(pipe_id),
                                    wintypes.ULONG(PIPE_TRANSFER_TIMEOUT),
                                    wintypes.ULONG(C.sizeof(v)), C.byref(v))

    def _transfer(self, fn, pipe_id, buf, length):
        """Issue an overlapped transfer and wait for it. Returns bytes moved,
        or None if the pipe timed out or stalled."""
        ov = OVERLAPPED()
        ov.hEvent = kernel32.CreateEventW(None, True, False, None)
        n = wintypes.ULONG()
        try:
            ok = fn(self.wh, C.c_ubyte(pipe_id), buf, wintypes.ULONG(length),
                    C.byref(n), C.byref(ov))
            if not ok:
                err = C.get_last_error()
                if err != ERROR_IO_PENDING:
                    return None
                if not winusb.WinUsb_GetOverlappedResult(self.wh, C.byref(ov),
                                                         C.byref(n), True):
                    return None
            return n.value
        finally:
            kernel32.CloseHandle(ov.hEvent)

    def write(self, pipe_id, data):
        buf = C.create_string_buffer(bytes(data), len(data))
        n = self._transfer(winusb.WinUsb_WritePipe, pipe_id, buf, len(data))
        if n is None:
            raise OSError("WritePipe failed: %d" % C.get_last_error())
        return n

    def read(self, pipe_id, length):
        buf = C.create_string_buffer(length)
        n = self._transfer(winusb.WinUsb_ReadPipe, pipe_id, buf, length)
        if n is None:
            return None                      # timeout or stall - caller decides
        return buf.raw[:n]

    def control(self, request_type, request, value, index, data=b"", length=0):
        pkt = WINUSB_SETUP_PACKET(request_type, request, value, index,
                                  len(data) if data else length)
        n = wintypes.ULONG()
        buf = C.create_string_buffer(bytes(data), len(data)) if data \
            else C.create_string_buffer(max(length, 1))
        ok = winusb.WinUsb_ControlTransfer(self.wh, pkt, buf,
                                           wintypes.ULONG(len(data) if data else length),
                                           C.byref(n), None)
        if not ok:
            raise OSError("ControlTransfer failed: %d" % C.get_last_error())
        return buf.raw[:n.value]


# ---------------------------------------------------------------- msg pipe

ROUTE_H2D = bytes.fromhex("0110ef03")

# Kept as a fixed sequence because the message framing is only partly
# understood (see docs/10-usb.md).
HELLO = bytes.fromhex("0c0000280110ef030000000200010021" "00100000")
READ_100A = bytes.fromhex("080000180110ef03000400080a100000")
ENTER_DFU = bytes.fromhex("080000180110ef030009000236100000")

# The complete prelude POD Go Edit sent immediately before the unit dropped into
# DFU, replayed unchanged in order with its original sequence numbers. The three
# writes of aa0000xx at 0x100a/0x100e/0x1032 look like arming steps; the last
# message is the trigger. Sending the trigger on its own is NOT enough - the
# device answers it with the reject status.
ENTER_DFU_SEQUENCE = [
    ("hello",                 "0c0000280110ef03000000020001002100100000"),
    ("setup write @0x1000",   "110000180110ef030002000400100000000003000100000003000000"),
    ("setup write @0x1000",   "120000180110ef030003000400100000010000000200000000000000"),
    ("read  @0x100a",         "080000180110ef03000400080a100000"),
    ("write @0x100a aa000001", "0c0000180110ef03000500040a100000aa000001"),
    ("write @0x100e aa000003", "0c0000180110ef03000600040e100000aa000003"),
    ("write @0x1032 aa000002", "0c0000180110ef030007000432100000aa000002"),
    ("read  @0x1036",         "080000180110ef030008000836100000"),
    ("TRIGGER @0x1036",       "080000180110ef030009000236100000"),
]

# The connect sequence POD Go Edit sends every time it attaches, replayed
# unchanged with the original sequence numbers. The two writes at 0x1000 appear
# to open the channel - without them the device ignores the read at 0x100a.
# Nothing here is persistent; it is ordinary connect traffic.
CONNECT = [
    ("hello",              HELLO),
    ("setup write @0x1000", bytes.fromhex("110000180110ef030002000400100000"
                                          "000003000100000003000000")),
    ("setup write @0x1000", bytes.fromhex("120000180110ef030003000400100000"
                                          "010000000200000000000000")),
    ("read @0x100a",       bytes.fromhex("080000180110ef03000400080a100000")),
]


def show(tag, data):
    if data is None:
        print(f"  {tag}: (nothing - timeout)")
        return
    print(f"  {tag}: {len(data)} bytes  {data.hex(' ')}")


def find_pipes(dev):
    bulk_in = bulk_out = None
    for p in dev.pipes():
        if PIPE_TYPE.get(p.PipeType) == "bulk":
            if p.PipeId & 0x80:
                bulk_in = bulk_in or p.PipeId
            else:
                bulk_out = bulk_out or p.PipeId
    return bulk_in, bulk_out


def cmd_info(args):
    paths = device_paths(L6_INTERFACE_GUID)
    print(f"device paths exposing the Line 6 WinUSB interface: {len(paths)}")
    for p in paths:
        print(f"\n  {p}")
        try:
            with WinUsbDevice(p) as dev:
                d = dev.interface_descriptor()
                print(f"    interface {d.bInterfaceNumber} alt {d.bAlternateSetting}  "
                      f"class {d.bInterfaceClass:#04x} sub {d.bInterfaceSubClass:#04x} "
                      f"proto {d.bInterfaceProtocol:#04x}  endpoints={d.bNumEndpoints}")
                for pi in dev.pipes():
                    print(f"      pipe {pi.PipeId:#04x}  {PIPE_TYPE.get(pi.PipeType,'?'):<11} "
                          f"maxpacket={pi.MaximumPacketSize:<5} interval={pi.Interval}")
        except OSError as e:
            print(f"    !! {e}")
    return 0


def cmd_probe(args):
    # The DFU boot ROM (1FC9) exposes the same GUID; only the factory system is 0E41.
    paths = [p for p in device_paths(L6_INTERFACE_GUID) if "vid_0e41" in p.lower()]
    if not paths:
        print("no factory system (0E41) on the Line 6 WinUSB interface")
        return 1
    with WinUsbDevice(paths[0]) as dev:
        bulk_in, bulk_out = find_pipes(dev)
        print(f"bulk out={bulk_out:#04x}  bulk in={bulk_in:#04x}")
        dev.set_timeout(bulk_in, args.timeout)
        dev.set_timeout(bulk_out, args.timeout)

        print("\ndraining any stale data from the IN pipe...")
        for _ in range(4):
            if not dev.read(bulk_in, 512):
                break

        # Without --full: hello then straight to the read, which is enough to
        # show whether the device answers at all. With --full: the two setup
        # writes at 0x1000 as well, which is what POD Go Edit actually sends.
        seq = CONNECT if args.full else [CONNECT[0], CONNECT[3]]
        for label, msg in seq:
            print(f"\n-> {label}")
            show("sent", msg)
            dev.write(bulk_out, msg)
            show("recv", dev.read(bulk_in, 512))
    return 0


CC_REBOOT_DFU = 68


def reboot_dfu_from_midi_control(args):
    """Our firmware: vendor command 68, value 127.

    It answers, leaves the bus about a second later and hands the unit to the
    boot ROM without a reset (firmware/src/board.c, board_enter_dfu; docs/01
    section 3). That route has not been run on a unit, so the
    result is the boot ROM appearing, not the answer.
    """
    dev = _log_device(args)
    if dev is None:
        return 1
    with dev:
        dev.write(LOG_EP_OUT, bytes([CC_REBOOT_DFU, 127]))
        print("sent {%d, 127}" % CC_REBOOT_DFU)
        # The answer is the last packet before the unit leaves. Read until
        # quiet, so nothing is left queued when the handle closes; a device
        # that has gone reads as quiet.
        answer = b""
        quiet = 0
        for _ in range(60):
            chunk = dev.read(LOG_EP_IN, LOG_EP_PACKET)
            if chunk:
                answer, quiet = chunk, 0
            else:
                quiet += 1
                if answer and quiet >= 8:
                    break
    text = answer.decode("ascii", "replace").strip()
    print("the firmware took the command" if "REBOOTING INTO DFU" in text
          else "no answer to the command (%s)" % (text or "nothing"))

    print("waiting for 1FC9:000C...")
    deadline = time.time() + 20.0
    while time.time() < deadline:
        time.sleep(0.5)
        if any("vid_1fc9" in p.lower() for p in device_paths(L6_INTERFACE_GUID)):
            print("1FC9:000C present: the unit is in DFU mode.")
            return 0
    if device_paths(MIDI_CONTROL_LOG_GUID):
        print("our firmware is still on the bus: the command was not taken.")
    else:
        print("no DFU device within 20 s. Power the unit off, then hold PAGE > while "
              "powering it on: it comes up in DFU mode.")
    return 1


def cmd_reboot_dfu(args):
    if not args.i_mean_it:
        print("This REBOOTS the unit. Re-run with --i-mean-it if that is intended.")
        return 2
    if device_paths(MIDI_CONTROL_LOG_GUID):
        return reboot_dfu_from_midi_control(args)
    paths = [p for p in device_paths(L6_INTERFACE_GUID) if "vid_0e41" in p.lower()]
    if not paths:
        print("no factory system (0E41) on the Line 6 WinUSB interface")
        return 1
    with WinUsbDevice(paths[0]) as dev:
        bulk_in, bulk_out = find_pipes(dev)
        dev.set_timeout(bulk_in, args.timeout)
        dev.set_timeout(bulk_out, args.timeout)

        # Essential: a stale reply left in the pipe shifts every response by one
        # and makes a working sequence look rejected.
        drained = 0
        while dev.read(bulk_in, 512):
            drained += 1
        if drained:
            print(f"(drained {drained} stale reply/replies from the IN pipe)\n")

        for label, hexmsg in ENTER_DFU_SEQUENCE:
            msg = bytes.fromhex(hexmsg)
            reply = None
            dev.write(bulk_out, msg)
            reply = dev.read(bulk_in, 512)
            rtype = struct.unpack(">H", reply[10:12])[0] if reply and len(reply) >= 12 else None
            mtype = struct.unpack(">H", msg[10:12])[0]
            if reply is None:
                verdict = "no reply"
            elif rtype == 0x0010:
                verdict = "REJECTED (status 0x0010)"
            else:
                verdict = f"ok (reply type {rtype:#06x})"
            print(f"-> {label:<26} type={mtype:#06x}  {verdict}")
            if reply is not None and label.startswith("TRIGGER"):
                print(f"   reply: {reply.hex(' ')}")

    print("\nSequence sent. Watch the bus for 1FC9:000C.")
    return 0


DFU_DNLOAD, DFU_GETSTATUS, DFU_CLRSTATUS, DFU_ABORT = 1, 3, 4, 6
DFU_STATE = {
    0: "appIDLE", 1: "appDETACH", 2: "dfuIDLE", 3: "dfuDNLOAD-SYNC",
    4: "dfuDNBUSY", 5: "dfuDNLOAD-IDLE", 6: "dfuMANIFEST-SYNC",
    7: "dfuMANIFEST", 8: "dfuMANIFEST-WAIT-RESET", 9: "dfuUPLOAD-IDLE",
    10: "dfuERROR",
}


def dfu_getstatus(dev):
    r = dev.control(0xA1, DFU_GETSTATUS, 0, 0, length=6)
    if len(r) < 6:
        return None
    status, t0, t1, t2, state, istr = r[0], r[1], r[2], r[3], r[4], r[5]
    poll = t0 | (t1 << 8) | (t2 << 16)
    return {"status": status, "poll_ms": poll, "state": state,
            "state_name": DFU_STATE.get(state, "?")}


def cmd_dfu_load(args):
    if not args.i_mean_it:
        print("This loads code into the unit's RAM and runs it. "
              "Re-run with --i-mean-it if that is intended.")
        return 2

    img = open(args.image, "rb").read()
    if img[:2] != b"\x1a\x00":
        print(f"{args.image}: header does not start 1a 00 - not an LPC boot image?")
        return 1
    blocks = struct.unpack("<H", img[2:4])[0]
    print(f"image: {args.image}")
    print(f"  {len(img)} bytes, header {img[:4].hex(' ')} -> {blocks} block(s) "
          f"= {blocks * 512} body bytes")

    paths = [p for p in device_paths(L6_INTERFACE_GUID) if "vid_1fc9" in p.lower()]
    if not paths:
        print("\nNo 1FC9:000C DFU device present. Run 'reboot-dfu --i-mean-it' first.")
        return 1

    with WinUsbDevice(paths[0]) as dev:
        d = dev.interface_descriptor()
        print(f"\nDFU device: class {d.bInterfaceClass:#04x} sub {d.bInterfaceSubClass:#04x} "
              f"proto {d.bInterfaceProtocol:#04x}")

        xfer = 2048
        try:
            cfg = dev.control(0x80, 6, 0x0200, 0, length=128)
            i = 0
            while i < len(cfg) and cfg[i]:
                blen, btype = cfg[i], cfg[i + 1]
                if btype == 0x21 and blen >= 9:
                    attrs = cfg[i + 2]
                    xfer = struct.unpack("<H", cfg[i + 5:i + 7])[0]
                    ver = struct.unpack("<H", cfg[i + 7:i + 9])[0]
                    print(f"  DFU functional: bmAttributes={attrs:#04x} "
                          f"(dnload={'y' if attrs & 1 else 'n'}, "
                          f"upload={'y' if attrs & 2 else 'n'}, "
                          f"manifestTolerant={'y' if attrs & 4 else 'n'}, "
                          f"willDetach={'y' if attrs & 8 else 'n'})  "
                          f"wTransferSize={xfer}  DFU {ver >> 8}.{ver & 0xff:02d}")
                i += blen if blen else 1
        except OSError as e:
            print(f"  (could not read config descriptor: {e})")

        st = dfu_getstatus(dev)
        print(f"  initial state: {st['state_name']} (status {st['status']}, "
              f"poll {st['poll_ms']} ms)" if st else "  GETSTATUS failed")

        # Chunk at the ROM's declared wTransferSize. Our
        # own test images fit in one block, but the vendor applet is 94 KB and
        # sending it whole is not an option.
        nblk = (len(img) + xfer - 1) // xfer
        print(f"\n-> DFU_DNLOAD, {len(img)} bytes in {nblk} block(s) of <= {xfer}")
        jumped = False
        for blk in range(nblk):
            chunk = img[blk * xfer:(blk + 1) * xfer]
            try:
                dev.control(0x21, DFU_DNLOAD, blk, 0, data=chunk)
            except OSError as e:
                # Once the ROM has the whole image it can jump immediately,
                # tearing down USB before the status stage completes. WinUSB
                # reports that as a failed transfer even though the data landed.
                print(f"   block {blk}: transfer reported {e}")
                print(f"   (ROM may have jumped; {blk} of {nblk} blocks acknowledged)")
                jumped = True
                break
            st = dfu_getstatus(dev)
            if st is None:
                print(f"   block {blk}: sent, no GETSTATUS response")
                jumped = True
                break
            if st["status"] != 0 or st["state"] == 10:
                print(f"   block {blk}: DFU error - status {st['status']}, "
                      f"state {st['state_name']}")
                return 1
            if blk == 0 or blk == nblk - 1 or nblk <= 4:
                print(f"   block {blk}: ok, state {st['state_name']}")

        if not jumped and not args.no_terminate:
            print("\n-> DFU_DNLOAD zero length (end of transfer)")
            try:
                dev.control(0x21, DFU_DNLOAD, nblk, 0, data=b"")
                st = dfu_getstatus(dev)
                print(f"   state: {st['state_name']} (status {st['status']})"
                      if st else "   no GETSTATUS response")
            except OSError as e:
                print(f"   no response - {e}")
                print("   (expected if the ROM has already jumped to the image)")

    print("\nDownloaded. Now watch the bus.")
    return 0


def _log_device(args):
    """Open our firmware's vendor interface, or explain why it is not there."""
    paths = device_paths(MIDI_CONTROL_LOG_GUID)
    if not paths:
        print("No log interface from our firmware found.")
        print("  - is our firmware loaded? (it enumerates as VID_1209 PID_0001)")
        print("  - the factory system does not have this interface")
        print("  - Windows caches driver bindings per device: if the firmware")
        print("    was loaded before the vendor interface existed, unplug and")
        print("    replug once so it re-reads the MS OS descriptors")
        return None
    dev = WinUsbDevice(paths[0])
    dev.reset_pipe(LOG_EP_IN)
    dev.reset_pipe(LOG_EP_OUT)
    # A short pipe timeout, not the 2 s the other commands use.
    #
    # The firmware can only have one IN transfer outstanding, so it writes a
    # packet and then waits for the host to take it before writing the next.
    # That makes throughput one packet per read, and with a 2 s timeout a read
    # that returns nothing costs two seconds - so a 30 s budget captured only
    # part of the log and then closed mid-stream, which is what wedges the
    # pipe. At 150 ms the empty reads are cheap and the stream keeps up.
    dev.set_timeout(LOG_EP_IN, LOG_READ_TIMEOUT_MS)
    return dev


LOG_EP_IN = 0x83
# One packet per read, not a big buffer.
#
# The firmware writes in 512-byte chunks and 512 is the endpoint's max packet
# size, so a burst of them contains no short packet to tell the host a transfer
# has ended. Ask for 4096 and ReadPipe waits for all of it, or for the timeout,
# and a stream that is merely paused looks like a stream that has stopped -
# which is how the log kept getting truncated mid-line. Ask for exactly one
# packet and every read completes as soon as one arrives.
LOG_EP_PACKET = 512
LOG_READ_TIMEOUT_MS = 150
LOG_EP_OUT = 0x03


def cmd_log(args):
    dev = _log_device(args)
    if dev is None:
        return 1

    with dev:
        if args.replay:
            dev.write(LOG_EP_OUT, bytes([126, 1]))     # CC#126: rewind

        # Send the command on the *same* handle that will read the answer.
        # Doing it as a separate `cc` invocation means a close in between, and
        # a close aborts the in-flight IN transfer the firmware is waiting to
        # complete - after which nothing more arrives.
        if args.cc is not None:
            dev.write(LOG_EP_OUT, bytes([args.cc & 0xFF, args.ccval & 0xFF]))
            print("--- sent {%d, %d} ---" % (args.cc, args.ccval))

        # Read until the firmware goes quiet, and only then let the handle
        # close.
        #
        # This is the whole discipline for this endpoint. Closing while the
        # device still has an IN transfer queued leaves that transfer in flight
        # on the device for good: the next open reads nothing, forever, and
        # only a reset clears it. So --seconds is a *budget*, not a duration -
        # the loop stops early once nothing has arrived for a while, which is
        # the only moment it is safe to let go.
        deadline = time.time() + args.seconds
        pending = b""
        got = 0
        quiet = 0

        while time.time() < deadline:
            chunk = dev.read(LOG_EP_IN, LOG_EP_PACKET)
            if not chunk:
                quiet += 1
                if got and quiet >= 20:      # ~3 s of silence at 150 ms
                    break                              # drained and settled
                continue
            quiet = 0
            got += len(chunk)
            pending += chunk
            while b"\n" in pending:
                line, pending = pending.split(b"\n", 1)
                print(line.decode("ascii", "replace"))

        if pending:
            print(pending.decode("ascii", "replace"))

        # One last sweep, so nothing is left queued at close even if the budget
        # ran out mid-stream.
        for _ in range(4):
            if not dev.read(LOG_EP_IN, LOG_EP_PACKET):
                break

    if got == 0:
        print("(nothing received - try --replay, or check the firmware is running)")
    return 0


def cmd_result(args):
    """Ask for the last result packet and print it.

    The whole point is that this is one packet. The firmware keeps every answer
    under 400 bytes, so what comes back is always shorter than the endpoint's
    512 and therefore terminates the read by itself - no streaming, nothing
    left queued, and nothing to strand when the handle closes. Contrast `log`,
    which is a stream and has to be treated with care (docs/10 section 5).
    """
    dev = _log_device(args)
    if dev is None:
        return 1

    with dev:
        if args.run is not None:
            dev.write(LOG_EP_OUT, bytes([args.run & 0xFF, args.runval & 0xFF]))
        else:
            dev.write(LOG_EP_OUT, bytes([93, 1]))       # CC#93: re-send it

        # Collect every packet and keep the last.
        #
        # The firmware emits after each measurement and each emit carries the
        # whole answer so far, so the last packet is the most complete one -
        # and if the experiment stalls part way, the last packet is exactly
        # what it had measured before it did. Stopping at the first short
        # packet, which an earlier version did, returns only the first line.
        got = b""
        quiet = 0
        for _ in range(200):
            chunk = dev.read(LOG_EP_IN, LOG_EP_PACKET)
            if chunk:
                got = chunk
                quiet = 0
            else:
                quiet += 1
                if got and quiet >= 8:
                    break

    if not got:
        print("(no result - has an experiment been run yet?)")
        return 1

    print(got.decode("ascii", "replace").rstrip())
    return 0


def cmd_dump_nor(args):
    """Dump the 1 MiB QSPI NOR to a file over the vendor endpoint.

    The firmware (spifi.c, CC#98) sends a 16-byte header - magic "SSNORDMP", a
    u32 LE length and a u32 LE CRC32 - then that many raw bytes. This reads by
    byte count, verifies the CRC, and writes the payload out. Read-only on the
    device: a SPIFI read cannot alter the flash.
    """
    import binascii

    out = args.image or "nor-dump.bin"

    dev = _log_device(args)
    if dev is None:
        return 1

    with dev:
        # The lab image must be loaded - the controller has no SPIFI command.
        dev.write(LOG_EP_OUT, bytes([98, 1]))       # CC#98: dump the NOR
        print("--- requested NOR dump (CC#98) ---")

        # The device computes a CRC over the whole 1 MiB before the first byte
        # arrives, so tolerate a few seconds of initial silence, then read until
        # the declared length is in hand.
        blob = bytearray()
        need = None
        started = False
        quiet = 0
        deadline = time.time() + 120.0

        while time.time() < deadline:
            chunk = dev.read(LOG_EP_IN, LOG_EP_PACKET)
            if chunk:
                blob += chunk
                started = True
                quiet = 0
                if need is None and len(blob) >= 16:
                    if bytes(blob[:8]) != b"SSNORDMP":
                        print("bad header %r - is the lab image loaded?"
                              % bytes(blob[:8]))
                        return 1
                    need = 16 + struct.unpack_from("<I", blob, 8)[0]
                if need is not None and len(blob) >= need:
                    break
            else:
                quiet += 1
                # ~30 empty 150 ms reads (4.5 s) of silence after data began,
                # or before it began, is enough to call it stalled.
                if quiet >= 30 and (started or time.time() > deadline - 90):
                    break

    if need is None:
        print("(no header received - is the firmware the lab image, and running?)")
        return 1
    if len(blob) < need:
        print(f"short dump: got {len(blob)} of {need} bytes")
        return 1

    length = struct.unpack_from("<I", blob, 8)[0]
    want_crc = struct.unpack_from("<I", blob, 12)[0]
    payload = bytes(blob[16:16 + length])
    got_crc = binascii.crc32(payload) & 0xFFFFFFFF

    with open(out, "wb") as f:
        f.write(payload)

    ok = got_crc == want_crc
    print(f"wrote {len(payload)} bytes to {out}")
    print(f"  CRC32 device={want_crc:08x} host={got_crc:08x}  {'OK' if ok else 'MISMATCH'}")
    print(f"  first 16 bytes: {payload[:16].hex(' ')}")
    return 0 if ok else 1


def cmd_dump_emmc(args):
    """Back up the eMMC's application image region to a file.

    The firmware (main.c, CC#79) reads the image header at LBA 1, works out how
    many sectors the image occupies, and streams sector 0 onwards: a 16-byte
    frame - magic "SSEMMCDP", a u32 LE length, a u32 reserved - then that many
    bytes. Read-only on the device; emmc.c has no write path.

    There is deliberately no checksum in the frame. The NOR dump carried one and
    it was wrong, reporting a mismatch on data that was provably correct. What
    established that backup was comparing it against known-good bytes and taking
    a second dump to show the read repeats - so do that here too:

        dump-emmc a.bin ; dump-emmc b.bin ; cmp a.bin b.bin
    """
    out = args.image or "emmc-app-backup.bin"

    dev = _log_device(args)
    if dev is None:
        return 1

    with dev:
        dev.write(LOG_EP_OUT, bytes([79, 1]))          # CC#79
        print("--- requested eMMC application-image backup (CC#79) ---")

        blob = bytearray()
        need = None
        started = False
        quiet = 0
        deadline = time.time() + 300.0

        while time.time() < deadline:
            chunk = dev.read(LOG_EP_IN, LOG_EP_PACKET)
            if chunk:
                blob += chunk
                started = True
                quiet = 0
                if need is None and len(blob) >= 16:
                    if bytes(blob[:8]) != b"SSEMMCDP":
                        print("bad frame %r - is the lab image loaded?" % bytes(blob[:8]))
                        return 1
                    need = 16 + struct.unpack_from("<I", blob, 8)[0]
                    print("  device reports %d bytes (%d sectors)"
                          % (need - 16, (need - 16) // 512))
                if need is not None and len(blob) >= need:
                    break
            else:
                quiet += 1
                # The card is initialised and the header read before the first
                # byte arrives, so allow a slow start; once data has begun,
                # a few seconds of silence means it has stopped.
                if quiet >= 40 and started:
                    break
                if quiet >= 200:
                    break

    if need is None:
        print("(no frame received - is the lab image running?)")
        return 1

    payload = bytes(blob[16:need])
    with open(out, "wb") as f:
        f.write(payload)

    short = len(blob) < need
    print("wrote %d bytes to %s%s"
          % (len(payload), out, "  *** SHORT - a sector read failed ***" if short else ""))
    if payload[:4]:
        print("  LBA 0 first 16 bytes: %s" % payload[:16].hex(" "))
    if len(payload) >= 528:
        print("  LBA 1 first 16 bytes: %s" % payload[512:528].hex(" "))
    return 1 if short else 0


def cmd_write_emmc(args):
    """Send an image to the device and have it written to the eMMC.

    **This writes storage a power cycle does not restore.** It is the same path
    used to put the backup back as to install new firmware, which is deliberate:
    the way out is exercised by the same code as the way in.

    Wire format, matching main.c CC#76: the two-byte command {76, 127} to arm,
    then a 20-byte header - b"SSWRIMG!", u32 LE lba, u32 LE length, u32 LE CRC32
    - then the payload. The device buffers the whole thing in SDRAM and writes
    it, then reads every sector back and compares. The CRC is reported, not checked.

    The device refuses any target outside sectors 0..1364, the region a verified
    backup exists for.
    """
    import binascii

    if not args.image:
        print("write-emmc needs a file")
        return 1
    lba = int(args.number) if args.number is not None else None
    if lba is None:
        print("write-emmc needs a target LBA, e.g. write-emmc img.bin 1")
        return 1

    payload = open(args.image, "rb").read()
    if not payload:
        print("empty file")
        return 1
    # Whole sectors only - the device writes in 512-byte blocks.
    if len(payload) % 512:
        payload += b"\x00" * (512 - len(payload) % 512)

    crc = binascii.crc32(payload) & 0xFFFFFFFF
    sectors = len(payload) // 512
    print(f"{args.image}: {len(payload)} bytes ({sectors} sectors) -> LBA {lba}, crc32 {crc:08x}")
    if lba + sectors > 1365:
        print("  refusing: that runs past sector 1364, which is the end of the")
        print("  region we hold a verified backup of. The device would refuse too.")
        return 1

    if not args.i_mean_it:
        print("\nThis WRITES the eMMC and a power cycle will not undo it.")
        print("Re-run with --i-mean-it if that is intended.")
        return 2

    dev = _log_device(args)
    if dev is None:
        return 1

    with dev:
        dev.write(LOG_EP_OUT, bytes([76, 127]))            # arm
        time.sleep(0.2)

        header = b"SSWRIMG!" + struct.pack("<III", lba, len(payload), crc)
        dev.write(LOG_EP_OUT, header)

        # Chunked so neither side has to hold the whole transfer in one write.
        sent = 0
        while sent < len(payload):
            chunk = payload[sent:sent + 4096]
            dev.write(LOG_EP_OUT, chunk)
            sent += len(chunk)
        print(f"  sent {sent} bytes; waiting for the device to verify")

        # The device writes and reads back before answering, so give it room.
        got = b""
        quiet = 0
        for _ in range(600):
            chunk = dev.read(LOG_EP_IN, LOG_EP_PACKET)
            if chunk:
                got = chunk
                quiet = 0
            else:
                quiet += 1
                if got and quiet >= 10:
                    break

    if not got:
        print("(no answer - check the log with: midi_control_usb.py log --replay)")
        return 1

    text = got.decode("ascii", "replace").rstrip()
    print(text)
    return 0 if "WRITTEN AND VERIFIED" in text else 1


def cmd_cc(args):
    if args.number is None or args.value is None:
        print("cc needs a number and a value")
        return 1

    dev = _log_device(args)
    if dev is None:
        return 1

    with dev:
        dev.write(LOG_EP_OUT, bytes([args.number & 0xFF, args.value & 0xFF]))
        print("sent {%d, %d}" % (args.number, args.value))

        # Drain whatever the command produced before letting the handle close.
        #
        # This is not for the output - it is thrown away - but because closing
        # a handle while the firmware has an IN transfer queued leaves that
        # transfer in flight on the device forever. Nothing arrives on the next
        # open, no matter how long it waits, and the endpoint stays that way
        # until the LPC is reset. It looks exactly like a dead firmware and it
        # is not.
        #
        # `log --cc` avoids the problem entirely by never closing in between,
        # and is the better way to drive this. This just stops the standalone
        # command being a trap.
        # Drain to quiet, not for a fixed period.
        #
        # A fixed two seconds was itself a trap: after CC#126 rewinds the log
        # the firmware has the whole buffer to send, and closing part way
        # through strands the transfer exactly as any other mid-stream close
        # does. Same rule as `log` - the handle may only be let go once the
        # device has stopped talking.
        deadline = time.time() + 30.0
        quiet = 0
        while time.time() < deadline:
            if dev.read(LOG_EP_IN, LOG_EP_PACKET):
                quiet = 0
            else:
                quiet += 1
                if quiet >= 20:
                    break

    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("command", choices=["info", "probe", "reboot-dfu", "dfu-load",
                                        "log", "cc", "result", "dump-nor",
                                        "dump-emmc", "write-emmc"])
    ap.add_argument("image", nargs="?",
                    help="dfu-load: image file to download; "
                         "dump-nor: output file (default nor-dump.bin)")
    ap.add_argument("number", nargs="?", type=int, help="cc: command number")
    ap.add_argument("value", nargs="?", type=int, help="cc: command value")
    ap.add_argument("--seconds", type=float, default=30.0,
                    help="log: budget, not duration - reading stops early once "
                         "the firmware goes quiet, which is the only safe "
                         "moment to close the handle")
    ap.add_argument("--run", type=int, default=None,
                    help="result: run this command first, then read the answer")
    ap.add_argument("--runval", type=int, default=1,
                    help="result: the value for --run (default 1)")
    ap.add_argument("--cc", type=int, default=None,
                    help="log: send this command first, on the same handle")
    ap.add_argument("--ccval", type=int, default=1,
                    help="log: the value for --cc (default 1)")
    ap.add_argument("--replay", action="store_true",
                    help="log: ask the firmware to replay its buffer from the "
                         "start, so lines emitted before this ran are not lost")
    ap.add_argument("--no-terminate", action="store_true",
                    help="dfu-load: skip the zero-length terminating DNLOAD")
    ap.add_argument("--timeout", type=int, default=2000, help="pipe timeout ms")
    ap.add_argument("--full", action="store_true",
                    help="probe: also replay the two setup writes at 0x1000 that "
                         "POD Go Edit sends on every connect")
    ap.add_argument("--i-mean-it", action="store_true",
                    help="required for commands that change device state")
    args = ap.parse_args()
    handlers = {"info": cmd_info, "probe": cmd_probe,
                "reboot-dfu": cmd_reboot_dfu, "dfu-load": cmd_dfu_load,
                "log": cmd_log, "cc": cmd_cc, "result": cmd_result,
                "dump-nor": cmd_dump_nor, "dump-emmc": cmd_dump_emmc,
                "write-emmc": cmd_write_emmc}
    if args.command == "cc":
        if args.image is None or args.number is None:
            ap.error("cc needs a number and a value, e.g. cc 91 1")
        args.number, args.value = int(args.image), int(args.number)
    if args.command == "dfu-load" and not args.image:
        ap.error("dfu-load needs an image file")
    return handlers[args.command](args)


if __name__ == "__main__":
    sys.exit(main())

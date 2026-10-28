# SPDX-FileCopyrightText: 2026 ElasticSea
# SPDX-License-Identifier: Apache-2.0
"""Windows-only XRSP transport that opens just the Quest's XRSP interface.

libusb on Windows opens every interface of a composite device, which fails
while adb holds the ADB interface. WinUSB lets us open interface 0 alone via
its device-interface path, so adb (and live logcat) can keep running.
"""
import ctypes
import ctypes.wintypes as wt
import struct
import sys
import time

XRSP_INTERFACE_GUID = "{f72fe0d4-cbcb-407d-8814-9ed673d0dd6b}"
EP_OUT, EP_IN = 0x01, 0x81
READ_SIZE = 1024  # Quest pads transfers to 1 KiB and sends no short packets; larger reads stall and lose data on timeout

DIGCF_PRESENT, DIGCF_DEVICEINTERFACE = 0x2, 0x10
GENERIC_READ, GENERIC_WRITE = 0x80000000, 0x40000000
FILE_SHARE_READ, FILE_SHARE_WRITE = 0x1, 0x2
OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, FILE_FLAG_OVERLAPPED = 3, 0x80, 0x40000000
INVALID_HANDLE = wt.HANDLE(-1).value
PIPE_TRANSFER_TIMEOUT = 0x03
ERROR_SEM_TIMEOUT, ERROR_OPERATION_ABORTED, ERROR_IO_PENDING = 121, 995, 997
WAIT_OBJECT_0, WAIT_TIMEOUT = 0, 0x102
WAIT_SLICE_MS = 100  # wait in slices so Ctrl+C stays responsive


class GUID(ctypes.Structure):
    _fields_ = [("Data1", wt.DWORD), ("Data2", wt.WORD), ("Data3", wt.WORD), ("Data4", ctypes.c_ubyte * 8)]

    @classmethod
    def parse(cls, s):
        h = s.strip("{}").replace("-", "")
        g = cls(int(h[0:8], 16), int(h[8:12], 16), int(h[12:16], 16))
        g.Data4[:] = bytes.fromhex(h[16:])
        return g


class SP_DEVICE_INTERFACE_DATA(ctypes.Structure):
    _fields_ = [("cbSize", wt.DWORD), ("InterfaceClassGuid", GUID), ("Flags", wt.DWORD),
                ("Reserved", ctypes.c_void_p)]


class OVERLAPPED(ctypes.Structure):
    _fields_ = [("Internal", ctypes.c_void_p), ("InternalHigh", ctypes.c_void_p), ("Offset", wt.DWORD),
                ("OffsetHigh", wt.DWORD), ("hEvent", wt.HANDLE)]


setupapi = ctypes.WinDLL("setupapi", use_last_error=True)
kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
winusb = ctypes.WinDLL("winusb", use_last_error=True)

setupapi.SetupDiGetClassDevsW.restype = wt.HANDLE
setupapi.SetupDiGetClassDevsW.argtypes = [ctypes.POINTER(GUID), wt.LPCWSTR, wt.HWND, wt.DWORD]
setupapi.SetupDiEnumDeviceInterfaces.argtypes = [wt.HANDLE, ctypes.c_void_p, ctypes.POINTER(GUID), wt.DWORD,
                                                 ctypes.POINTER(SP_DEVICE_INTERFACE_DATA)]
setupapi.SetupDiGetDeviceInterfaceDetailW.argtypes = [wt.HANDLE, ctypes.POINTER(SP_DEVICE_INTERFACE_DATA),
                                                      ctypes.c_void_p, wt.DWORD, ctypes.POINTER(wt.DWORD),
                                                      ctypes.c_void_p]
setupapi.SetupDiDestroyDeviceInfoList.argtypes = [wt.HANDLE]
kernel32.CreateFileW.restype = wt.HANDLE
kernel32.CreateFileW.argtypes = [wt.LPCWSTR, wt.DWORD, wt.DWORD, ctypes.c_void_p, wt.DWORD, wt.DWORD, wt.HANDLE]
kernel32.CloseHandle.argtypes = [wt.HANDLE]
kernel32.CreateEventW.restype = wt.HANDLE
kernel32.CreateEventW.argtypes = [ctypes.c_void_p, wt.BOOL, wt.BOOL, wt.LPCWSTR]
kernel32.ResetEvent.argtypes = [wt.HANDLE]
kernel32.WaitForSingleObject.restype = wt.DWORD
kernel32.WaitForSingleObject.argtypes = [wt.HANDLE, wt.DWORD]
winusb.WinUsb_Initialize.argtypes = [wt.HANDLE, ctypes.POINTER(ctypes.c_void_p)]
winusb.WinUsb_Free.argtypes = [ctypes.c_void_p]
winusb.WinUsb_SetPipePolicy.argtypes = [ctypes.c_void_p, ctypes.c_ubyte, wt.ULONG, wt.ULONG, ctypes.c_void_p]
winusb.WinUsb_WritePipe.argtypes = [ctypes.c_void_p, ctypes.c_ubyte, ctypes.c_void_p, wt.ULONG,
                                    ctypes.POINTER(wt.ULONG), ctypes.c_void_p]
winusb.WinUsb_ReadPipe.argtypes = winusb.WinUsb_WritePipe.argtypes
winusb.WinUsb_ResetPipe.argtypes = [ctypes.c_void_p, ctypes.c_ubyte]
winusb.WinUsb_AbortPipe.argtypes = [ctypes.c_void_p, ctypes.c_ubyte]
winusb.WinUsb_GetOverlappedResult.argtypes = [ctypes.c_void_p, ctypes.POINTER(OVERLAPPED), ctypes.POINTER(wt.ULONG),
                                              wt.BOOL]


def _check(ok, what):
    if not ok:
        raise OSError(ctypes.get_last_error(), f"{what} failed (error {ctypes.get_last_error()})")


def find_xrsp_paths():
    guid = GUID.parse(XRSP_INTERFACE_GUID)
    info = setupapi.SetupDiGetClassDevsW(ctypes.byref(guid), None, None, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE)
    if info == INVALID_HANDLE:
        raise OSError(ctypes.get_last_error(), "SetupDiGetClassDevs failed")
    paths = []
    try:
        i = 0
        while True:
            data = SP_DEVICE_INTERFACE_DATA(cbSize=ctypes.sizeof(SP_DEVICE_INTERFACE_DATA))
            if not setupapi.SetupDiEnumDeviceInterfaces(info, None, ctypes.byref(guid), i, ctypes.byref(data)):
                break
            need = wt.DWORD()
            setupapi.SetupDiGetDeviceInterfaceDetailW(info, ctypes.byref(data), None, 0, ctypes.byref(need), None)
            buf = ctypes.create_string_buffer(need.value)
            # SP_DEVICE_INTERFACE_DETAIL_DATA_W.cbSize is 8 on 64-bit
            struct.pack_into("<I", buf, 0, 8 if ctypes.sizeof(ctypes.c_void_p) == 8 else 6)
            _check(setupapi.SetupDiGetDeviceInterfaceDetailW(info, ctypes.byref(data), buf, need, None, None),
                   "SetupDiGetDeviceInterfaceDetail")
            paths.append(ctypes.wstring_at(ctypes.addressof(buf) + 4))
            i += 1
    finally:
        setupapi.SetupDiDestroyDeviceInfoList(info)
    return paths


class WinUsbPipe:
    """Raw bulk pipe pair on the XRSP interface.

    All transfers are overlapped: we wait on an event with our own deadline and
    abort the pipe on timeout, so a call never blocks past timeout_ms and any
    bytes that arrived before the abort are kept.
    """

    def __init__(self):
        paths = find_xrsp_paths()
        if not paths:
            raise RuntimeError("Quest XRSP interface not found (plugged in? OVRService stopped?)")
        self.path = paths[0]
        self.file = kernel32.CreateFileW(self.path, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                         None, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED, None)
        if self.file == INVALID_HANDLE:
            err = ctypes.get_last_error()
            raise OSError(err, f"cannot open XRSP interface (error {err}; is OVRService still running?)")
        self.handle = ctypes.c_void_p()
        if not winusb.WinUsb_Initialize(self.file, ctypes.byref(self.handle)):
            err = ctypes.get_last_error()
            kernel32.CloseHandle(self.file)
            raise OSError(err, f"WinUsb_Initialize failed (error {err})")
        self.events = {ep: kernel32.CreateEventW(None, True, False, None) for ep in (EP_OUT, EP_IN)}
        for ep in (EP_OUT, EP_IN):
            # No driver-side timeout; _transfer enforces the deadline itself
            v = wt.ULONG(0)
            _check(winusb.WinUsb_SetPipePolicy(self.handle, ep, PIPE_TRANSFER_TIMEOUT, 4, ctypes.byref(v)),
                   "WinUsb_SetPipePolicy")

    def close(self):
        for ep in (EP_OUT, EP_IN):
            winusb.WinUsb_AbortPipe(self.handle, ep)
        winusb.WinUsb_Free(self.handle)
        kernel32.CloseHandle(self.file)
        for ev in self.events.values():
            kernel32.CloseHandle(ev)

    def _transfer(self, call, ep, buf, size, timeout_ms):
        """Run one overlapped transfer; return (bytes done, timed_out)."""
        ov = OVERLAPPED(hEvent=self.events[ep])
        kernel32.ResetEvent(ov.hEvent)
        done = wt.ULONG()
        if not call(self.handle, ep, buf, size, None, ctypes.byref(ov)):
            err = ctypes.get_last_error()
            if err != ERROR_IO_PENDING:
                raise OSError(err, f"{call.__name__} failed (error {err})")
        end = time.monotonic() + timeout_ms / 1000
        timed_out = False
        try:
            while True:
                left = max(0, int((end - time.monotonic()) * 1000))
                r = kernel32.WaitForSingleObject(ov.hEvent, min(left, WAIT_SLICE_MS))
                if r == WAIT_OBJECT_0:
                    break
                if r != WAIT_TIMEOUT:
                    raise OSError(ctypes.get_last_error(), "WaitForSingleObject failed")
                if left == 0:
                    timed_out = True
                    break
        finally:
            if timed_out or sys.exc_info()[0] is not None:
                # Cancel, then wait for the cancelled transfer so buf is no longer in use
                winusb.WinUsb_AbortPipe(self.handle, ep)
            ok = winusb.WinUsb_GetOverlappedResult(self.handle, ctypes.byref(ov), ctypes.byref(done), True)
            err = ctypes.get_last_error()
        if not ok and err not in (ERROR_OPERATION_ABORTED, ERROR_SEM_TIMEOUT):
            raise OSError(err, f"{call.__name__} failed (error {err})")
        return done.value, timed_out or not ok

    def write(self, data, timeout_ms=1000):
        buf = ctypes.create_string_buffer(bytes(data), len(data))
        done, timed_out = self._transfer(winusb.WinUsb_WritePipe, EP_OUT, buf, len(data), timeout_ms)
        if timed_out:
            raise OSError(ERROR_SEM_TIMEOUT, f"WinUsb_WritePipe timed out after {done}/{len(data)} bytes")
        return done

    def read(self, size=READ_SIZE, timeout_ms=1000):
        """Return bytes read (possibly partial on timeout), or None if nothing arrived in time."""
        buf = ctypes.create_string_buffer(size)
        done, _ = self._transfer(winusb.WinUsb_ReadPipe, EP_IN, buf, size, timeout_ms)
        return buf.raw[:done] if done else None

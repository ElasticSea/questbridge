"""Windows-only XRSP transport that opens just the Quest's XRSP interface.

libusb on Windows opens every interface of a composite device, which fails
while adb holds the ADB interface. WinUSB lets us open interface 0 alone via
its device-interface path, so adb (and live logcat) can keep running.
"""
import ctypes
import ctypes.wintypes as wt
import struct

XRSP_INTERFACE_GUID = "{f72fe0d4-cbcb-407d-8814-9ed673d0dd6b}"
EP_OUT, EP_IN = 0x01, 0x81
READ_SIZE = 1024  # Quest pads transfers to 1 KiB and sends no short packets; larger reads stall and lose data on timeout

DIGCF_PRESENT, DIGCF_DEVICEINTERFACE = 0x2, 0x10
GENERIC_READ, GENERIC_WRITE = 0x80000000, 0x40000000
FILE_SHARE_READ, FILE_SHARE_WRITE = 0x1, 0x2
OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, FILE_FLAG_OVERLAPPED = 3, 0x80, 0x40000000
INVALID_HANDLE = wt.HANDLE(-1).value
PIPE_TRANSFER_TIMEOUT = 0x03
ERROR_SEM_TIMEOUT = 121


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
winusb.WinUsb_Initialize.argtypes = [wt.HANDLE, ctypes.POINTER(ctypes.c_void_p)]
winusb.WinUsb_Free.argtypes = [ctypes.c_void_p]
winusb.WinUsb_SetPipePolicy.argtypes = [ctypes.c_void_p, ctypes.c_ubyte, wt.ULONG, wt.ULONG, ctypes.c_void_p]
winusb.WinUsb_WritePipe.argtypes = [ctypes.c_void_p, ctypes.c_ubyte, ctypes.c_void_p, wt.ULONG,
                                    ctypes.POINTER(wt.ULONG), ctypes.c_void_p]
winusb.WinUsb_ReadPipe.argtypes = winusb.WinUsb_WritePipe.argtypes
winusb.WinUsb_ResetPipe.argtypes = [ctypes.c_void_p, ctypes.c_ubyte]


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
    """Raw bulk pipe pair on the XRSP interface."""

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
        _check(winusb.WinUsb_Initialize(self.file, ctypes.byref(self.handle)), "WinUsb_Initialize")
        self._timeout_ms = None

    def close(self):
        winusb.WinUsb_Free(self.handle)
        kernel32.CloseHandle(self.file)

    def _set_timeout(self, ep, ms):
        v = wt.ULONG(ms)
        _check(winusb.WinUsb_SetPipePolicy(self.handle, ep, PIPE_TRANSFER_TIMEOUT, 4, ctypes.byref(v)),
               "WinUsb_SetPipePolicy")

    def write(self, data, timeout_ms=1000):
        self._set_timeout(EP_OUT, timeout_ms)
        buf = ctypes.create_string_buffer(bytes(data), len(data))
        done = wt.ULONG()
        _check(winusb.WinUsb_WritePipe(self.handle, EP_OUT, buf, len(data), ctypes.byref(done), None),
               "WinUsb_WritePipe")
        return done.value

    def read(self, size=READ_SIZE, timeout_ms=1000):
        """Return bytes read, or None on timeout."""
        if self._timeout_ms != timeout_ms:
            self._set_timeout(EP_IN, timeout_ms)
            self._timeout_ms = timeout_ms
        buf = ctypes.create_string_buffer(size)
        done = wt.ULONG()
        if not winusb.WinUsb_ReadPipe(self.handle, EP_IN, buf, size, ctypes.byref(done), None):
            if ctypes.get_last_error() == ERROR_SEM_TIMEOUT:
                return None
            _check(False, "WinUsb_ReadPipe")
        return buf.raw[:done.value]

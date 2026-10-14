/**
 * @file focusspoof/testtarget.c
 * @brief Win32 host app used to smoke-test focusspoof.dll injection.
 *
 * Creates a visible top-level window and pumps its message loop. Every 2 s it
 * writes a line to logs\focusspoof_target_<pid>.log (relative to the working
 * directory) recording whether the injected
 * focus spoof makes GetForegroundWindow() report *this* window ("me:1") even
 * when another window holds the real foreground - which is exactly the spoof
 * the game engine relies on - and whether the injected input blocker swallowed
 * a posted WM_KEYDOWN before the window's own WndProc saw it (key_blocked:1).
 *
 * It also fast-probes XInput every ~20 ms and flushes one-shot markers when a
 * controller state read succeeds (SERVED / SERVEDEX: XInputRewrite, forwarded
 * over loopback UDP) or when the presence side reports a device on a machine
 * with no controller attached (FAKECAPS / FAKEBAT / FAKEKEY: XInputFakePad,
 * the injected fake-pad layer; FAKESET: a rumble write acknowledged by the
 * fake pad; FAKEDSOUND / FAKEAUDIOIDS: the audio-endpoint presence answers).
 * ProbeEnable() additionally exercises XInputEnable, whose call is proven by
 * the worker log's first-call line rather than a target marker (the API
 * succeeds without any fake). With no XInput hook armed and no physical
 * controller, XInputGetState always returns ERROR_DEVICE_NOT_CONNECTED, so the
 * markers can only come from the DLL.
 *
 * It also probes DirectInput8 every 2 s (DIFOUND / DICREATE / DISTATE) so the
 * injected DirectInput fake gamepad is exercised end-to-end exactly like a
 * DirectInput title: enumerate DI8DEVCLASS_GAMECTRL, create the pad it found,
 * bind a gamepad data format, acquire and read state. Without the injected
 * layer EnumDevices finds no gamepad, so DIFOUND can only come from the DLL.
 * A device that enumerates but reports ERROR_DEVICE_REMOVED is a ghost node
 * left by an uninstalled driver: the probe drops it and re-enumerates,
 * skipping that instance GUID, until a working pad is found.
 *
 * Every ~20 ms it also walks the raw-input device tree exactly like a title
 * that detects controllers on hot-plug (GetRawInputDeviceList +
 * GetRawInputDeviceInfoW, looking for a RIM_TYPEHID device with the Xbox 360
 * identity). A real tree only holds keyboard/mouse, so the RAWFAKE marker can
 * only come from the injected fake-HID layer.
 *
 * Usage: testtarget.exe [suffix]
 * Build: gcc -municode -mwindows -O2 -s testtarget.c -o testtarget.exe \
 *        -lsetupapi -lhid -lole32 -loleaut32
 * 32-bit (MINGW32 toolchain):
 *        gcc -m32 -municode -mwindows -O2 -s testtarget.c \
 *            -o testtarget32.exe -lsetupapi -lhid -lole32 -loleaut32
 */
#define INITGUID
#include <windows.h>
#include <xinput.h>
#include <dinput.h>
#include <stdio.h>
#include <wchar.h>
#include <wbemidl.h>
#include <oleauto.h>
#include <setupapi.h>
#include <hidsdi.h>
#include <hidclass.h>

static HWND g_hwnd = 0;
static FILE* g_log = 0;
static char g_suffix[16] = "";
static volatile LONG g_sawKeydown = 0; ///< Set when WndProc actually receives WM_KEYDOWN.
static volatile LONG g_wmiFakeLogged = 0; ///< 1 once the WMI probe saw the fake PNP entity.
static volatile LONG g_hidFakeLogged = 0; ///< 1 once the SetupDi probe saw the fake HID device.

typedef DWORD(WINAPI* TGetState)(DWORD, XINPUT_STATE*);
static TGetState g_xgs = 0; ///< XInputGetState resolved lazily from xinput1_4.dll.
/* Undocumented extended XInput layout: the plain fields plus a trailing DWORD
   several engines validate as zero before they accept a snapshot. */
typedef struct _XGAME_EX {
  WORD wButtons;
  BYTE bLeftTrigger;
  BYTE bRightTrigger;
  SHORT sThumbLX;
  SHORT sThumbLY;
  SHORT sThumbRX;
  SHORT sThumbRY;
  DWORD dwUnknown;
} XGAME_EX;
typedef struct _XSTATE_EX {
  DWORD dwPacketNumber;
  XGAME_EX Gamepad;
} XSTATE_EX;
typedef DWORD(WINAPI* TGetStateEx)(DWORD, XSTATE_EX*);
static TGetStateEx g_xgsEx = 0; ///< XInputGetStateEx resolved lazily (name, then ordinal 100).
typedef DWORD(WINAPI* TGetCaps)(DWORD, DWORD, XINPUT_CAPABILITIES*);
static TGetCaps g_xcaps = 0; ///< XInputGetCapabilities resolved lazily.
/* Undocumented extended capabilities descriptor: the standard descriptor plus
   the console identity some engines require before they poll the pad. */
typedef struct _XCAPS_EX {
  XINPUT_CAPABILITIES Capabilities;
  WORD vendorId;
  WORD productId;
  WORD versionNumber;
  WORD unk1;
} XCAPS_EX;
typedef DWORD(WINAPI* TGetCapsEx)(DWORD, DWORD, DWORD, XCAPS_EX*);
static TGetCapsEx g_xcapsEx = 0; ///< XInputGetCapabilitiesEx resolved lazily.
typedef DWORD(WINAPI* TGetBattery)(DWORD, BYTE, XINPUT_BATTERY_INFORMATION*);
static TGetBattery g_xbat = 0; ///< XInputGetBatteryInformation resolved lazily.
typedef DWORD(WINAPI* TGetKeystroke)(DWORD, DWORD, XINPUT_KEYSTROKE*);
static TGetKeystroke g_xkey = 0; ///< XInputGetKeystroke resolved lazily.
typedef DWORD(WINAPI* TSetState)(DWORD, XINPUT_VIBRATION*);
static TSetState g_xset = 0; ///< XInputSetState resolved lazily.
typedef void(WINAPI* TEnable)(BOOL);
static TEnable g_xenable = 0; ///< XInputEnable resolved lazily (xinput1_3.dll).
typedef DWORD(WINAPI* TGetDsound)(DWORD, GUID*, GUID*);
static TGetDsound g_xdsound = 0; ///< XInputGetDSoundAudioDeviceGuids resolved lazily.
typedef DWORD(WINAPI* TGetAudioIds)(DWORD, WCHAR*, UINT*, WCHAR*, UINT*);
static TGetAudioIds g_xaudioIds = 0; ///< XInputGetAudioDeviceIds resolved lazily.

/**
 * @brief Query the controller with the process's own XInputGetState.
 *
 * The import is resolved dynamically so the probe exercises the exact module
 * focusspoof.dll is expected to hook (xinput1_4 is loaded here first, which
 * makes the injected resolver prefer it).
 * @param btnOut Receives the gamepad button bits.
 * @return The HRESULT returned by XInputGetState.
 */
static DWORD ProbeXInput(DWORD* btnOut) {
  *btnOut = 0;
  if (!g_xgs) {
    HMODULE h = LoadLibraryA("xinput1_4.dll");
    if (h)
      g_xgs = (TGetState)GetProcAddress(h, "XInputGetState");
  }
  if (!g_xgs)
    return 0xFFFFFFFF;
  XINPUT_STATE st;
  DWORD hr = g_xgs(0, &st);
  *btnOut = st.Gamepad.wButtons;
  return hr;
}

/**
 * @brief Query the controller through the undocumented extended entry.
 *
 * Many titles read the pad via XInputGetStateEx instead of plain
 * XInputGetState; probing it proves the injected rewrite serves that entry
 * with a valid EX layout. The trailing dwUnknown is read too: engines treat a
 * non-zero value as a corrupt snapshot, so the rewrite must hand back zero.
 * xinput1_4 publishes the entry by name and at ordinal 100; some runtimes
 * only expose a matching ordinal.
 * @param btnOut Receives the gamepad button bits.
 * @param unknownOut Receives the trailing EX validation DWORD.
 * @return The HRESULT returned by the extended entry.
 */
static DWORD ProbeXInputEx(DWORD* btnOut, DWORD* unknownOut) {
  *btnOut = 0;
  *unknownOut = 0xFFFFFFFF;
  if (!g_xgsEx) {
    HMODULE h = LoadLibraryA("xinput1_4.dll");
    if (h) {
      g_xgsEx = (TGetStateEx)GetProcAddress(h, "XInputGetStateEx");
      if (!g_xgsEx)
        g_xgsEx = (TGetStateEx)GetProcAddress(h, (LPCSTR)MAKEWORD(100, 0));
    }
  }
  if (!g_xgsEx)
    return 0xFFFFFFFF;
  XSTATE_EX st;
  DWORD hr = g_xgsEx(0, &st);
  *btnOut = st.Gamepad.wButtons;
  *unknownOut = st.Gamepad.dwUnknown;
  return hr;
}

static volatile LONG g_servedSaw = 0;    ///< Latched when a rewrite-served state was observed.
static volatile LONG g_servedLogged = 0; ///< Whether the SERVED marker was flushed.
static volatile LONG g_servedExSaw = 0;    ///< Latched when the extended entry was served.
static volatile LONG g_servedExLogged = 0; ///< Whether the SERVEDEX marker was flushed.
static volatile LONG g_fakeCapsSaw = 0;    ///< Latched when fake capabilities were observed.
static volatile LONG g_fakeCapsLogged = 0; ///< Whether the FAKECAPS marker was flushed.
static volatile LONG g_fakeCapsExSaw = 0;    ///< Latched when fake extended capabilities were observed.
static volatile LONG g_fakeCapsExLogged = 0; ///< Whether the FAKECAPSEX marker was flushed.
static volatile LONG g_fakeBatSaw = 0;     ///< Latched when fake battery info was observed.
static volatile LONG g_fakeBatLogged = 0;  ///< Whether the FAKEBAT marker was flushed.
static volatile LONG g_fakeKeySaw = 0;     ///< Latched when the fake keystroke state was observed.
static volatile LONG g_fakeKeyLogged = 0;  ///< Whether the FAKEKEY marker was flushed.
static volatile LONG g_fakeSetSaw = 0;     ///< Latched when a rumble write was acknowledged.
static volatile LONG g_fakeSetLogged = 0;  ///< Whether the FAKESET marker was flushed.
static volatile LONG g_fakeDsoundSaw = 0;  ///< Latched when the fake DSound GUID answer was observed.
static volatile LONG g_fakeDsoundLogged = 0; ///< Whether the FAKEDSOUND marker was flushed.
static volatile LONG g_fakeAudioIdsSaw = 0;  ///< Latched when the fake audio-ID answer was observed.
static volatile LONG g_fakeAudioIdsLogged = 0; ///< Whether the FAKEAUDIOIDS marker was flushed.

/**
 * @brief Query the device descriptor with the process's own
 * XInputGetCapabilities (xinput1_4, the module focusspoof hooks).
 * @param subtypeOut Receives the reported subtype.
 * @return The HRESULT returned by XInputGetCapabilities.
 */
static DWORD ProbeCaps(WORD* subtypeOut) {
  *subtypeOut = 0;
  if (!g_xcaps) {
    HMODULE h = LoadLibraryA("xinput1_4.dll");
    if (h)
      g_xcaps = (TGetCaps)GetProcAddress(h, "XInputGetCapabilities");
  }
  if (!g_xcaps)
    return 0xFFFFFFFF;
  XINPUT_CAPABILITIES caps;
  memset(&caps, 0, sizeof(caps));
  DWORD hr = g_xcaps(0, 0, &caps);
  *subtypeOut = caps.SubType;
  return hr;
}

/**
 * @brief Query the extended device descriptor with the process's own
 * XInputGetCapabilitiesEx (name on xinput1_4, ordinal 108 on the legacy
 * runtimes). Some engines only accept a pad whose extended descriptor
 * carries the Xbox 360 identity, so the fake layer must serve it too.
 * @param vendorOut Receives the reported vendor ID.
 * @param productOut Receives the reported product ID.
 * @return The HRESULT returned by XInputGetCapabilitiesEx.
 */
static DWORD ProbeCapsEx(WORD* vendorOut, WORD* productOut) {
  *vendorOut = 0;
  *productOut = 0;
  if (!g_xcapsEx) {
    HMODULE h = LoadLibraryA("xinput1_4.dll");
    if (h) {
      g_xcapsEx = (TGetCapsEx)GetProcAddress(h, "XInputGetCapabilitiesEx");
      if (!g_xcapsEx)
        g_xcapsEx = (TGetCapsEx)GetProcAddress(h, (LPCSTR)MAKEWORD(108, 0));
    }
  }
  if (!g_xcapsEx)
    return 0xFFFFFFFF;
  XCAPS_EX caps;
  memset(&caps, 0, sizeof(caps));
  DWORD hr = g_xcapsEx(0, 0, XINPUT_FLAG_GAMEPAD, &caps);
  *vendorOut = caps.vendorId;
  *productOut = caps.productId;
  return hr;
}

/**
 * @brief Query the battery report with the process's own
 * XInputGetBatteryInformation (xinput1_4).
 * @param typeOut Receives the reported battery type.
 * @return The HRESULT returned by XInputGetBatteryInformation.
 */
static DWORD ProbeBattery(BYTE* typeOut) {
  *typeOut = 0;
  if (!g_xbat) {
    HMODULE h = LoadLibraryA("xinput1_4.dll");
    if (h)
      g_xbat = (TGetBattery)GetProcAddress(h, "XInputGetBatteryInformation");
  }
  if (!g_xbat)
    return 0xFFFFFFFF;
  XINPUT_BATTERY_INFORMATION bi;
  memset(&bi, 0, sizeof(bi));
  DWORD hr = g_xbat(0, 0, &bi);
  *typeOut = bi.BatteryType;
  return hr;
}

/**
 * @brief Query the keystroke buffer with the process's own
 * XInputGetKeystroke (xinput1_4).
 * @return The HRESULT returned by XInputGetKeystroke.
 */
static DWORD ProbeKeystroke(void) {
  if (!g_xkey) {
    HMODULE h = LoadLibraryA("xinput1_4.dll");
    if (h)
      g_xkey = (TGetKeystroke)GetProcAddress(h, "XInputGetKeystroke");
  }
  if (!g_xkey)
    return 0xFFFFFFFF;
  XINPUT_KEYSTROKE key;
  memset(&key, 0, sizeof(key));
  return g_xkey(0, 0, &key);
}

/**
 * @brief Write rumble with the process's own XInputSetState (xinput1_4).
 *
 * The fake-pad layer must consume this write and still return ERROR_SUCCESS,
 * so the game believes rumble support exists without any local motor moving.
 * @return The HRESULT returned by XInputSetState.
 */
static DWORD ProbeSetState(void) {
  if (!g_xset) {
    HMODULE h = LoadLibraryA("xinput1_4.dll");
    if (h)
      g_xset = (TSetState)GetProcAddress(h, "XInputSetState");
  }
  if (!g_xset)
    return 0xFFFFFFFF;
  XINPUT_VIBRATION vib;
  vib.wLeftMotorSpeed = 0xFFFF;
  vib.wRightMotorSpeed = 0xFFFF;
  return g_xset(0, &vib);
}

/**
 * @brief Call XInputEnable (xinput1_3) with TRUE.
 *
 * The fake family hooks count these calls and log the first one in the worker
 * log, so the smoke asserts on that log line rather than on a target marker
 * (the API legitimately succeeds without any fake, so it cannot prove the
 * injected layer by itself).
 */
static void ProbeEnable(void) {
  if (!g_xenable) {
    HMODULE h = LoadLibraryA("xinput1_3.dll");
    if (h)
      g_xenable = (TEnable)GetProcAddress(h, "XInputEnable");
  }
  if (g_xenable)
    g_xenable(TRUE);
}

/**
 * @brief Query the DirectSound audio endpoint GUIDs (xinput1_3).
 *
 * A plain process reports ERROR_DEVICE_NOT_CONNECTED (no pad), so a
 * FAKEDSOUND marker can only come from the injected fake-pad layer, which
 * answers "pad present, no headset" with ERROR_SUCCESS and zero GUIDs.
 * @return The HRESULT returned by XInputGetDSoundAudioDeviceGuids.
 */
static DWORD ProbeDsound(void) {
  if (!g_xdsound) {
    HMODULE h = LoadLibraryA("xinput1_3.dll");
    if (h)
      g_xdsound = (TGetDsound)GetProcAddress(h, "XInputGetDSoundAudioDeviceGuids");
  }
  if (!g_xdsound)
    return 0xFFFFFFFF;
  GUID render = {0}, capture = {0};
  return g_xdsound(0, &render, &capture);
}

/**
 * @brief Query the per-device audio endpoint IDs (xinput1_4).
 *
 * Same policy as ProbeDsound through the Unicode API: only the fake-pad layer
 * turns the not-connected report into "present, zero-length IDs".
 * @return The HRESULT returned by XInputGetAudioDeviceIds.
 */
static DWORD ProbeAudioIds(void) {
  if (!g_xaudioIds) {
    HMODULE h = LoadLibraryA("xinput1_4.dll");
    if (h)
      g_xaudioIds = (TGetAudioIds)GetProcAddress(h, "XInputGetAudioDeviceIds");
  }
  if (!g_xaudioIds)
    return 0xFFFFFFFF;
  WCHAR render[16] = {0}, capture[16] = {0};
  UINT rc = 16, cc = 16;
  return g_xaudioIds(0, render, &rc, capture, &cc);
}

/* ---- Raw-input fake-HID probe ------------------------------------------ */

typedef UINT(WINAPI* TRawList)(PRAWINPUTDEVICELIST, PUINT, UINT); ///< GetRawInputDeviceList.
typedef UINT(WINAPI* TRawInfo)(HANDLE, UINT, LPVOID, PUINT);      ///< GetRawInputDeviceInfoW.
static TRawList g_rawList = 0;   ///< GetRawInputDeviceList resolved lazily from user32.
static TRawInfo g_rawInfoW = 0;  ///< GetRawInputDeviceInfoW resolved lazily from user32.
static volatile LONG g_rawFakeSaw = 0;   ///< Latched once the fake Xbox HID device was seen.
static volatile LONG g_rawFakeLogged = 0; ///< Whether the RAWFAKE marker was flushed.

/**
 * @brief Resolve the raw-input enumeration APIs and scan for the fake pad.
 *
 * Mirrors a title that detects controllers on hot-plug: GetRawInputDeviceList
 * twice (size probe, then fill), then GetRawInputDeviceInfoW(RIDI_DEVICEINFO)
 * on every entry, looking for a RIM_TYPEHID device carrying the Xbox 360
 * identity (vendor 045E / product 028E). A plain process only has real
 * keyboard/mouse devices in the tree, so a hit can only come from the
 * injected fake-HID layer.
 * @return 1 when the Xbox pad was found, 0 otherwise.
 */
static int ProbeRawList(void) {
  if (!g_rawList) {
    HMODULE h = LoadLibraryA("user32.dll");
    if (h) {
      g_rawList = (TRawList)GetProcAddress(h, "GetRawInputDeviceList");
      g_rawInfoW = (TRawInfo)GetProcAddress(h, "GetRawInputDeviceInfoW");
    }
  }
  if (!g_rawList || !g_rawInfoW)
    return 0;
  UINT count = 0;
  if (g_rawList(NULL, &count, sizeof(RAWINPUTDEVICELIST)) == (UINT)-1)
    return 0;
  if (count == 0 || count > 128)
    return 0;
  RAWINPUTDEVICELIST list[128];
  memset(list, 0, sizeof(list));
  UINT got = g_rawList(list, &count, sizeof(RAWINPUTDEVICELIST));
  if (got == (UINT)-1 || got > 128)
    got = count < 128 ? count : 128;
  for (UINT i = 0; i < got; i++) {
    if (list[i].dwType != RIM_TYPEHID)
      continue;
    RID_DEVICE_INFO info;
    memset(&info, 0, sizeof(info));
    info.cbSize = sizeof(info);
    UINT sz = sizeof(info);
    UINT r = g_rawInfoW(list[i].hDevice, RIDI_DEVICEINFO, &info, &sz);
    if (r == (UINT)-1 || sz < sizeof(info))
      continue;
    if (info.dwType == RIM_TYPEHID && info.hid.dwVendorId == 0x045E &&
        info.hid.dwProductId == 0x028E)
      return 1;
  }
  return 0;
}

/* ---- DirectInput probe ------------------------------------------------ */

/**
 * @brief DirectInput state layout the probe reads from the fake pad.
 *
 * Declaring our own format (instead of c_dfDIJoystick2 from dxguid) keeps
 * the probe dependency-free while mirroring the Xbox-360-over-DirectInput
 * identity the injected fake device advertises.
 */
typedef struct _DITESTSTATE {
  LONG lX, lY, lZ, lRx, lRy, lRz; ///< Axes; Z/Rz carry the right stick.
  DWORD pov;                      ///< Point-of-view hat.
  BYTE btn[16];                   ///< Button array.
} DITESTSTATE;

/// Button object initialiser for DITESTSTATE.btn[i].
#define DIBTN(i)                                                               \
  {                                                                            \
    &GUID_Button, FIELD_OFFSET(DITESTSTATE, btn[i]),                           \
        DIDFT_PSHBUTTON | DIDFT_MAKEINSTANCE(i), 0                             \
  }

/// Standard gamepad objects (6 axes, 1 POV, 16 buttons) at DITESTSTATE offsets.
static const DIOBJECTDATAFORMAT g_diTestObjs[] = {
    {&GUID_XAxis, FIELD_OFFSET(DITESTSTATE, lX),
     DIDFT_ABSAXIS | DIDFT_MAKEINSTANCE(0), 0},
    {&GUID_YAxis, FIELD_OFFSET(DITESTSTATE, lY),
     DIDFT_ABSAXIS | DIDFT_MAKEINSTANCE(1), 0},
    {&GUID_ZAxis, FIELD_OFFSET(DITESTSTATE, lZ),
     DIDFT_ABSAXIS | DIDFT_MAKEINSTANCE(2), 0},
    {&GUID_RxAxis, FIELD_OFFSET(DITESTSTATE, lRx),
     DIDFT_ABSAXIS | DIDFT_MAKEINSTANCE(3), 0},
    {&GUID_RyAxis, FIELD_OFFSET(DITESTSTATE, lRy),
     DIDFT_ABSAXIS | DIDFT_MAKEINSTANCE(4), 0},
    {&GUID_RzAxis, FIELD_OFFSET(DITESTSTATE, lRz),
     DIDFT_ABSAXIS | DIDFT_MAKEINSTANCE(5), 0},
    {&GUID_POV, FIELD_OFFSET(DITESTSTATE, pov), DIDFT_POV | DIDFT_MAKEINSTANCE(0),
     0},
    DIBTN(0), DIBTN(1), DIBTN(2), DIBTN(3), DIBTN(4), DIBTN(5), DIBTN(6),
    DIBTN(7), DIBTN(8), DIBTN(9), DIBTN(10), DIBTN(11), DIBTN(12), DIBTN(13),
    DIBTN(14), DIBTN(15),
};

/// Data format binding the objects above to DITESTSTATE.
static const DIDATAFORMAT g_diTestFormat = {
    sizeof(DIDATAFORMAT), sizeof(DIOBJECTDATAFORMAT), 0, sizeof(DITESTSTATE),
    6 + 1 + 16, (LPDIOBJECTDATAFORMAT)g_diTestObjs};

/* ---- DMC5-style wildcard format --------------------------------------- */
/* Devil May Cry 5's engine binds button objects with a wildcard instance
   (DIDFT_OPTIONAL | DIDFT_ANYINSTANCE) and a null pguid; the X360-over-DI
   convention button i -> DIJOYSTATE2 offset 48 + i is the only identity.
   Bind the same shape to prove the fake resolves the button number from
   the object offset when no usable instance is present. */
static const DIOBJECTDATAFORMAT g_diWildObjs[] = {
    {&GUID_XAxis, 0, DIDFT_OPTIONAL | DIDFT_ANYINSTANCE | DIDFT_AXIS, 0},
    {&GUID_YAxis, 4, DIDFT_OPTIONAL | DIDFT_ANYINSTANCE | DIDFT_AXIS, 0},
    {&GUID_ZAxis, 8, DIDFT_OPTIONAL | DIDFT_ANYINSTANCE | DIDFT_AXIS, 0},
    {&GUID_RxAxis, 12, DIDFT_OPTIONAL | DIDFT_ANYINSTANCE | DIDFT_AXIS, 0},
    {&GUID_RyAxis, 16, DIDFT_OPTIONAL | DIDFT_ANYINSTANCE | DIDFT_AXIS, 0},
    {&GUID_RzAxis, 20, DIDFT_OPTIONAL | DIDFT_ANYINSTANCE | DIDFT_AXIS, 0},
    {&GUID_Slider, 24, DIDFT_OPTIONAL | DIDFT_ANYINSTANCE | DIDFT_AXIS, 0},
    {&GUID_Slider, 28, DIDFT_OPTIONAL | DIDFT_ANYINSTANCE | DIDFT_AXIS, 0},
    {&GUID_POV, 32, DIDFT_OPTIONAL | DIDFT_ANYINSTANCE | DIDFT_POV, 0},
    {&GUID_POV, 36, DIDFT_OPTIONAL | DIDFT_ANYINSTANCE | DIDFT_POV, 0},
    {&GUID_POV, 40, DIDFT_OPTIONAL | DIDFT_ANYINSTANCE | DIDFT_POV, 0},
    {&GUID_POV, 44, DIDFT_OPTIONAL | DIDFT_ANYINSTANCE | DIDFT_POV, 0},
    {0, 48, DIDFT_OPTIONAL | DIDFT_ANYINSTANCE | DIDFT_BUTTON, 0},
    {0, 49, DIDFT_OPTIONAL | DIDFT_ANYINSTANCE | DIDFT_BUTTON, 0},
    {0, 50, DIDFT_OPTIONAL | DIDFT_ANYINSTANCE | DIDFT_BUTTON, 0},
    {0, 51, DIDFT_OPTIONAL | DIDFT_ANYINSTANCE | DIDFT_BUTTON, 0},
    {0, 52, DIDFT_OPTIONAL | DIDFT_ANYINSTANCE | DIDFT_BUTTON, 0},
    {0, 53, DIDFT_OPTIONAL | DIDFT_ANYINSTANCE | DIDFT_BUTTON, 0},
    {0, 54, DIDFT_OPTIONAL | DIDFT_ANYINSTANCE | DIDFT_BUTTON, 0},
    {0, 55, DIDFT_OPTIONAL | DIDFT_ANYINSTANCE | DIDFT_BUTTON, 0},
    {0, 56, DIDFT_OPTIONAL | DIDFT_ANYINSTANCE | DIDFT_BUTTON, 0},
    {0, 57, DIDFT_OPTIONAL | DIDFT_ANYINSTANCE | DIDFT_BUTTON, 0},
    {0, 58, DIDFT_OPTIONAL | DIDFT_ANYINSTANCE | DIDFT_BUTTON, 0},
    {0, 59, DIDFT_OPTIONAL | DIDFT_ANYINSTANCE | DIDFT_BUTTON, 0},
    {0, 60, DIDFT_OPTIONAL | DIDFT_ANYINSTANCE | DIDFT_BUTTON, 0},
    {0, 61, DIDFT_OPTIONAL | DIDFT_ANYINSTANCE | DIDFT_BUTTON, 0},
    {0, 62, DIDFT_OPTIONAL | DIDFT_ANYINSTANCE | DIDFT_BUTTON, 0},
    {0, 63, DIDFT_OPTIONAL | DIDFT_ANYINSTANCE | DIDFT_BUTTON, 0},
};

/// DMC5-shaped format: 8 axis slots, 4 POV slots, 16 buttons, 272 bytes.
static const DIDATAFORMAT g_diWildFormat = {
    sizeof(DIDATAFORMAT), sizeof(DIOBJECTDATAFORMAT), 0, 272,
    8 + 4 + 16, (LPDIOBJECTDATAFORMAT)g_diWildObjs};

typedef HRESULT(WINAPI* TDICreate8)(HINSTANCE, DWORD, REFIID, LPVOID*,
                                    LPUNKNOWN); ///< DirectInput8Create signature.

static TDICreate8 g_diCreate = 0;           ///< Resolved DirectInput8Create.
static IDirectInput8W* g_di = 0;            ///< Lazily created IDirectInput8W.
static IDirectInputDevice8W* g_diDev = 0;   ///< Fake pad device, once created.
static GUID g_diInstance;                   ///< Instance GUID returned by enumeration.
static WORD g_diDevType = 0;                ///< Dev type of the enumerated pad.
static volatile LONG g_diFound = 0;         ///< 1 once a gamepad was enumerated.
static volatile LONG g_diLogged = 0;        ///< Whether DIFOUND was flushed.
static volatile LONG g_diProbed = 0;        ///< 1 once identity/object/buffer probed.
static volatile LONG g_diDead = 0;          ///< 1 once the latched device proved removed.
static volatile LONG g_diDeadHandled = 0;   ///< 1 once a removed device was dropped.
static DITESTSTATE g_diState;               ///< Last state read from the pad.

/// Maximum number of DirectInput candidates collected in one enumeration.
#define DI_MAX_CANDS 16
static GUID g_diCands[DI_MAX_CANDS];     ///< Instance GUIDs found by enumeration.
static WORD g_diCandTypes[DI_MAX_CANDS]; ///< Dev types of the candidates found.
static int g_diCandCount = 0;            ///< Candidates collected in the last pass.
static int g_diCandIdx = 0;              ///< Next candidate to try.

/**
 * @brief Collect every enumerated DirectInput gamepad (up to a cap).
 * @param inst Enumerated device instance.
 * @param ctx Unused.
 * @return DIENUM_CONTINUE while room remains, DIENUM_STOP otherwise.
 */
static BOOL CALLBACK DiEnumCb(LPCDIDEVICEINSTANCEW inst, LPVOID ctx) {
  (void)ctx;
  if (g_diDead &&
      memcmp(&inst->guidInstance, &g_diInstance, sizeof(GUID)) == 0)
    return DIENUM_CONTINUE; /* skip the device that proved removed */
  if (g_diCandCount >= DI_MAX_CANDS)
    return DIENUM_STOP; /* enough candidates for one walk */
  g_diCands[g_diCandCount] = inst->guidInstance;
  g_diCandTypes[g_diCandCount] = inst->dwDevType;
  ++g_diCandCount;
  return DIENUM_CONTINUE;
}

/**
 * @brief Enumerate/walk candidates and probe the working fake DirectInput pad.
 *
 * Mirrors what a DirectInput title does: enumerate gamepads, create the one
 * it found, bind a data format, acquire and read state. It proves the
 * injected fake device is discoverable and serves fresh state - the whole
 * point of the no-driver streaming path. Ghost nodes left by an uninstalled
 * driver enumerate but fail GetDeviceState, so every candidate is tried
 * within one walk and only a pad that answers state is kept. On a plain
 * process (nothing injected) EnumDevices finds no gamepad, so DIFOUND never
 * appears.
 */
static void ProbeDirectInput(void) {
  if (!g_di) {
    HMODULE h = LoadLibraryA("dinput8.dll");
    if (h)
      g_diCreate = (TDICreate8)GetProcAddress(h, "DirectInput8Create");
    if (!g_diCreate)
      return;
    if (FAILED(g_diCreate(GetModuleHandleW(NULL), DIRECTINPUT_VERSION,
                          &IID_IDirectInput8W, (LPVOID*)&g_di, NULL)) ||
        !g_di)
      return;
  }
  if (g_diFound == 0) {
    g_diCandCount = 0;
    g_diCandIdx = 0;
    g_di->lpVtbl->EnumDevices(g_di, DI8DEVCLASS_GAMECTRL, DiEnumCb, NULL,
                              DIEDFL_ATTACHEDONLY);
    if (g_diCandCount > 0) {
      InterlockedExchange(&g_diFound, 1);
      if (g_diLogged == 0) {
        InterlockedExchange(&g_diLogged, 1);
        if (g_log) {
          fprintf(g_log, "DIFOUND t=%lu devtype=%04x\n", GetTickCount(),
                  (unsigned)g_diCandTypes[0]);
          fflush(g_log);
        }
      }
    }
  }
  if (g_diFound == 0)
    return;
  /* Walk every candidate until one serves state; drop the dead ghosts. */
  while (g_diDev == NULL && g_diCandIdx < g_diCandCount) {
    g_diInstance = g_diCands[g_diCandIdx];
    g_diDevType = g_diCandTypes[g_diCandIdx];
    ++g_diCandIdx;
    if (FAILED(g_di->lpVtbl->CreateDevice(g_di, &g_diInstance, &g_diDev,
                                          NULL)) ||
        !g_diDev) {
      if (g_log) {
        fprintf(g_log, "DICREATEFAIL t=%lu\n", GetTickCount());
        fflush(g_log);
      }
      g_diDev = NULL;
      continue;
    }
    g_diDev->lpVtbl->SetDataFormat(g_diDev, &g_diTestFormat);
    g_diDev->lpVtbl->SetCooperativeLevel(g_diDev, g_hwnd,
                                         DISCL_BACKGROUND | DISCL_NONEXCLUSIVE);
    g_diDev->lpVtbl->Acquire(g_diDev);
    InterlockedExchange(&g_diDeadHandled, 0);
    InterlockedExchange(&g_diProbed, 0);
    if (g_log) {
      fprintf(g_log, "DICREATE t=%lu\n", GetTickCount());
      fflush(g_log);
    }
    /* Probe the identity/object/buffer path once per candidate. */
    DIDEVCAPS caps;
    memset(&caps, 0, sizeof(caps));
    caps.dwSize = sizeof(caps);
    HRESULT hc = g_diDev->lpVtbl->GetCapabilities(g_diDev, &caps);
    DIDEVICEOBJECTINSTANCEW oi;
    memset(&oi, 0, sizeof(oi));
    oi.dwSize = sizeof(oi);
    HRESULT ho = g_diDev->lpVtbl->GetObjectInfo(
        g_diDev, &oi, DIDFT_ABSAXIS | DIDFT_MAKEINSTANCE(0), DIPH_BYID);
    DIDEVICEINSTANCEW din;
    memset(&din, 0, sizeof(din));
    din.dwSize = sizeof(din);
    HRESULT hd = g_diDev->lpVtbl->GetDeviceInfo(g_diDev, &din);
    DIPROPDWORD bw;
    memset(&bw, 0, sizeof(bw));
    bw.diph.dwSize = sizeof(bw);
    bw.diph.dwHeaderSize = sizeof(DIPROPHEADER);
    bw.diph.dwObj = 0;
    bw.diph.dwHow = DIPH_DEVICE;
    bw.dwData = 16;
    HRESULT hp = g_diDev->lpVtbl->SetProperty(g_diDev, DIPROP_BUFFERSIZE,
                                              &bw.diph);
    DIDEVICEOBJECTDATA ev[8];
    DWORD rows = 8;
    HRESULT hdata = g_diDev->lpVtbl->GetDeviceData(g_diDev,
                                                   sizeof(DIDEVICEOBJECTDATA),
                                                   ev, &rows, 0);
    if (g_log) {
      fprintf(g_log,
              "DICAPS t=%lu hr=%ld axes=%lu buttons=%lu povs=%lu devtype=%04x\n",
              GetTickCount(), (long)hc, (unsigned long)caps.dwAxes,
              (unsigned long)caps.dwButtons, (unsigned long)caps.dwPOVs,
              (unsigned)caps.dwDevType);
      fprintf(g_log,
              "DINFO t=%lu hr=%ld inst=%08lx prod=%08lx devtype=%04x\n",
              GetTickCount(), (long)hd, (unsigned long)din.guidInstance.Data1,
              (unsigned long)din.guidProduct.Data1, (unsigned)din.dwDevType);
      fprintf(g_log, "DOBJINFO t=%lu hr=%ld ofs=%lu type=%08lx\n",
              GetTickCount(), (long)ho, (unsigned long)oi.dwOfs,
              (unsigned long)oi.dwType);
      fprintf(g_log, "DPROP t=%lu hr=%ld\n", GetTickCount(), (long)hp);
      fprintf(g_log, "DDATA t=%lu hr=%ld rows=%lu\n", GetTickCount(),
              (long)hdata, (unsigned long)rows);
      fflush(g_log);
    }
    HRESULT hr =
        g_diDev->lpVtbl->GetDeviceState(g_diDev, sizeof(g_diState), &g_diState);
    if (FAILED(hr)) {
      /* Ghost node left by an uninstalled driver: it enumerates and answers
         the identity probes, but GetDeviceState reports an error (on this
         machine 0x8007000C, ERROR_INVALID_ACCESS, rather than
         ERROR_DEVICE_REMOVED). Drop it and try the next candidate - the
         injected fake always answers DI_OK. */
      InterlockedExchange(&g_diDead, 1); /* skip it on re-enumeration */
      g_diDev->lpVtbl->Release(g_diDev);
      g_diDev = NULL;
      InterlockedExchange(&g_diFound, 0);
      continue;
    }
  }
  if (g_diDev == NULL)
    return;
  /* DMC5-style binding proof: bind a format of wildcard instances with
     null-pguid buttons at offsets 48..63 (as DMC5's engine does), read
     state, and restore the named format. The button number is resolved
     from the object offset, so b0 in this layout mirrors the A button.
     Logged every pass like DISTATE, so a late datagram is still seen. */
  {
    BYTE wild[272];
    memset(wild, 0, sizeof(wild));
    HRESULT hb = g_diDev->lpVtbl->SetDataFormat(g_diDev, &g_diWildFormat);
    HRESULT hw =
        g_diDev->lpVtbl->GetDeviceState(g_diDev, sizeof(wild), wild);
    g_diDev->lpVtbl->SetDataFormat(g_diDev, &g_diTestFormat);
    if (g_log) {
      fprintf(g_log,
              "DIWILD t=%lu hr=%ld bind=%ld lx=%ld ly=%ld b0=%u b1=%u b2=%u "
              "b3=%u b4=%u b5=%u\n",
              GetTickCount(), (long)hw, (long)hb,
              (long)(*(LONG*)(wild + 0)), (long)(*(LONG*)(wild + 4)),
              (unsigned)wild[48], (unsigned)wild[49], (unsigned)wild[50],
              (unsigned)wild[51], (unsigned)wild[52], (unsigned)wild[53]);
      fflush(g_log);
    }
  }
  HRESULT hr =
      g_diDev->lpVtbl->GetDeviceState(g_diDev, sizeof(g_diState), &g_diState);
  if (FAILED(hr) && InterlockedCompareExchange(&g_diDeadHandled, 1, 0) == 0) {
    /* The bound device stopped serving state after having passed the walk.
       Treat it like a ghost: drop it and let a later pass re-enumerate. */
    g_diDev->lpVtbl->Release(g_diDev);
    g_diDev = NULL;
    InterlockedExchange(&g_diDead, 1);
    InterlockedExchange(&g_diFound, 0);
    return;
  }
  if (g_log) {
    fprintf(g_log,
            "DISTATE t=%lu hr=%ld lx=%ld ly=%ld rx=%ld ry=%ld lt=%ld rt=%ld "
            "pov=%lu b0=%u b1=%u\n",
            GetTickCount(), (long)hr, g_diState.lX, g_diState.lY, g_diState.lZ,
            g_diState.lRz, g_diState.lRx, g_diState.lRy,
            (unsigned)g_diState.pov, (unsigned)g_diState.btn[0],
            (unsigned)g_diState.btn[1]);
    fflush(g_log);
  }
}

/* ------------------------------------------------------------------ */
/* WMI + SetupDi/HidD probes (XInput-path gate checks)                */
/* ------------------------------------------------------------------ */

/* MinGW's wbemidl.h does not declare the WMI GUIDs; define them here with
   the canonical SDK values (the same ones focusspoof.dll detours on). */
static const GUID kTestClsidWbemLocator = {0x4590F811, 0x1D3A, 0x11D0,
                                           {0x89, 0x1F, 0x00, 0xAA, 0x00, 0x4B, 0x2E, 0x24}};
static const GUID kTestIidWbemLocator = {0xDC12A687, 0x737F, 0x11CF,
                                         {0x88, 0x4D, 0x00, 0xAA, 0x00, 0x4B, 0x2E, 0x24}};
static const GUID kTestIidWbemServices = {0x9556DC99, 0x828B, 0x11CF,
                                          {0xA3, 0x7E, 0x00, 0xAA, 0x00, 0x24, 0x2E, 0xAE}};

/**
 * @brief WMI Win32_PNPEntity probe: emulate DMC5's "is this an XInput pad?"
 *        filter.
 *
 * The injected DLL arms a fake WMI provider when XInputFakePad is on; the
 * injected Win32_PNPEntity entry carries a DeviceID containing "IG_00". This
 * probe walks the real WMI stack (which the DLL detours once armed) and
 * logs WMIFAKE the moment that entry is visible, exactly like the game's
 * adoption check would see it.
 */
static void ProbeWmiPnp(void) {
  IWbemLocator* locator = NULL;
  IWbemServices* services = NULL;
  HRESULT hr = CoCreateInstance(&kTestClsidWbemLocator, NULL,
                                CLSCTX_INPROC_SERVER, &kTestIidWbemLocator,
                                (void**)&locator);
  if (SUCCEEDED(hr) && locator) {
    BSTR ns = SysAllocString(L"ROOT\\CIMV2");
    hr = locator->lpVtbl->ConnectServer(locator, ns, NULL, NULL, NULL, 0,
                                        NULL, NULL, &services);
    SysFreeString(ns);
  }
  int found = 0;
  if (SUCCEEDED(hr) && services) {
    IEnumWbemClassObject* en = NULL;
    BSTR cls = SysAllocString(L"Win32_PNPEntity");
    hr = services->lpVtbl->CreateInstanceEnum(services, cls, 0, NULL, &en);
    SysFreeString(cls);
    if (SUCCEEDED(hr) && en) {
      IWbemClassObject* rows[16];
      ULONG scanned = 0;
      for (;;) {
        ULONG got = 0;
        HRESULT nhr = en->lpVtbl->Next(en, 0, 16, rows, &got);
        if (got == 0 || FAILED(nhr))
          break;
        for (ULONG i = 0; i < got && scanned < 512; ++i, ++scanned) {
          VARIANT v;
          VariantInit(&v);
          HRESULT ghr =
              rows[i]->lpVtbl->Get(rows[i], L"DeviceID", 0, &v, NULL, NULL);
          if (SUCCEEDED(ghr) && v.vt == VT_BSTR && v.bstrVal &&
              wcsstr(v.bstrVal, L"IG_00") != NULL)
            found = 1;
          VariantClear(&v);
          rows[i]->lpVtbl->Release(rows[i]);
        }
        if (found || scanned >= 512)
          break;
      }
      en->lpVtbl->Release(en);
    }
  }
  if (services)
    services->lpVtbl->Release(services);
  if (locator)
    locator->lpVtbl->Release(locator);
  if (found && InterlockedCompareExchange(&g_wmiFakeLogged, 1, 0) == 0) {
    if (g_log) {
      fprintf(g_log, "WMIFAKE t=%lu\n", GetTickCount());
      fflush(g_log);
    }
  }
}

/**
 * @brief SetupDi/HidD probe: enumerate HID interfaces and read VID/PID.
 *
 * DMC5 gates its XInput path on seeing an HID device whose vendor ID is the
 * Xbox one (0x045E) via HidD_GetAttributes. The injected virtual HID device
 * advertises exactly that; this probe replays the game's enumeration and
 * logs HIDFAKE once the fake device is visible through SetupDi.
 */
static void ProbeHidSetupDi(void) {
  HDEVINFO set = SetupDiGetClassDevsW(&GUID_DEVINTERFACE_HID, NULL, NULL,
                                      DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
  if (set == INVALID_HANDLE_VALUE)
    return;
  int found = 0;
  SP_DEVICE_INTERFACE_DATA data;
  ZeroMemory(&data, sizeof(data));
  data.cbSize = sizeof(data);
  for (DWORD i = 0; i < 64 && !found; ++i) {
    if (!SetupDiEnumDeviceInterfaces(set, NULL, &GUID_DEVINTERFACE_HID, i,
                                     &data))
      break;
    DWORD need = 0;
    if (!SetupDiGetDeviceInterfaceDetailW(set, &data, NULL, 0, &need, NULL) ||
        GetLastError() != ERROR_INSUFFICIENT_BUFFER || need == 0)
      continue;
    PSP_DEVICE_INTERFACE_DETAIL_DATA_W detail =
        (PSP_DEVICE_INTERFACE_DETAIL_DATA_W)malloc(need);
    if (!detail)
      continue;
    detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
    BOOL ok = SetupDiGetDeviceInterfaceDetailW(set, &data, detail, need, &need,
                                               NULL);
    if (ok) {
      HANDLE hDev = CreateFileW(detail->DevicePath,
                                GENERIC_READ | GENERIC_WRITE,
                                FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                                OPEN_EXISTING, FILE_FLAG_OVERLAPPED, NULL);
      if (hDev != INVALID_HANDLE_VALUE) {
        HIDD_ATTRIBUTES att;
        ZeroMemory(&att, sizeof(att));
        att.Size = sizeof(att);
        if (HidD_GetAttributes(hDev, &att) && att.VendorID == 0x045E &&
            att.ProductID == 0x028E)
          found = 1;
        CloseHandle(hDev);
      }
    }
    free(detail);
  }
  SetupDiDestroyDeviceInfoList(set);
  if (found && InterlockedCompareExchange(&g_hidFakeLogged, 1, 0) == 0) {
    if (g_log) {
      fprintf(g_log, "HIDFAKE t=%lu\n", GetTickCount());
      fflush(g_log);
    }
  }
}

/**
 * @brief Fast XInput probe: sample the state every ~20 ms.
 *
 * The rewritten state is only valid for the slot freshness window (~120 ms),
 * so a 2 s log cadence would miss it. A classic real XInputGetState in this
 * environment (no controller attached) always returns
 * ERROR_DEVICE_NOT_CONNECTED, so observing hr==0 can only come from the
 * injected rewrite - the markers are flushed immediately when such an
 * observation happens, one for the plain entry and one for the extended one.
 */
static void ProbeFast(void) {
  static DWORD last = 0;
  DWORD now = GetTickCount();
  if (now - last < 20)
    return;
  last = now;
  DWORD xbtn = 0;
  DWORD hr = ProbeXInput(&xbtn);
  if (hr == 0 && InterlockedCompareExchange(&g_servedSaw, 1, 0) == 0 &&
      g_servedLogged == 0) {
    InterlockedExchange(&g_servedLogged, 1);
    if (g_log) {
      fprintf(g_log, "SERVED t=%lu xbtn=%u\n", now, xbtn);
      fflush(g_log);
    }
  }
  DWORD xbtnEx = 0, unknownEx = 0xFFFFFFFF;
  DWORD hrEx = ProbeXInputEx(&xbtnEx, &unknownEx);
  if (hrEx == 0 && InterlockedCompareExchange(&g_servedExSaw, 1, 0) == 0 &&
      g_servedExLogged == 0) {
    InterlockedExchange(&g_servedExLogged, 1);
    if (g_log) {
      fprintf(g_log, "SERVEDEX t=%lu xbtn=%u unknown=%lu\n", now, xbtnEx,
              unknownEx);
      fflush(g_log);
    }
  }

  /* Fake-pad presence probes: without XInputFakePad these all report
     ERROR_DEVICE_NOT_CONNECTED, so the markers only appear when the injected
     presence layer is armed (a real pad would also latch them, but the smoke
     harness runs with no controller attached). */
  WORD subtype = 0;
  DWORD hrC = ProbeCaps(&subtype);
  if (hrC == 0 && InterlockedCompareExchange(&g_fakeCapsSaw, 1, 0) == 0 &&
      g_fakeCapsLogged == 0) {
    InterlockedExchange(&g_fakeCapsLogged, 1);
    if (g_log) {
      fprintf(g_log, "FAKECAPS t=%lu subtype=%u\n", now, (unsigned)subtype);
      fflush(g_log);
    }
  }
  WORD vid = 0, pid = 0;
  DWORD hrCEx = ProbeCapsEx(&vid, &pid);
  if (hrCEx == 0 && InterlockedCompareExchange(&g_fakeCapsExSaw, 1, 0) == 0 &&
      g_fakeCapsExLogged == 0) {
    InterlockedExchange(&g_fakeCapsExLogged, 1);
    if (g_log) {
      fprintf(g_log, "FAKECAPSEX t=%lu vid=%04x pid=%04x\n", now, (unsigned)vid,
              (unsigned)pid);
      fflush(g_log);
    }
  }
  BYTE batType = 0;
  DWORD hrB = ProbeBattery(&batType);
  if (hrB == 0 && InterlockedCompareExchange(&g_fakeBatSaw, 1, 0) == 0 &&
      g_fakeBatLogged == 0) {
    InterlockedExchange(&g_fakeBatLogged, 1);
    if (g_log) {
      fprintf(g_log, "FAKEBAT t=%lu type=%u\n", now, (unsigned)batType);
      fflush(g_log);
    }
  }
  DWORD hrK = ProbeKeystroke();
  if (hrK == ERROR_EMPTY &&
      InterlockedCompareExchange(&g_fakeKeySaw, 1, 0) == 0 &&
      g_fakeKeyLogged == 0) {
    InterlockedExchange(&g_fakeKeyLogged, 1);
    if (g_log) {
      fprintf(g_log, "FAKEKEY t=%lu\n", now);
      fflush(g_log);
    }
  }
  DWORD hrS = ProbeSetState();
  if (hrS == 0 && InterlockedCompareExchange(&g_fakeSetSaw, 1, 0) == 0 &&
      g_fakeSetLogged == 0) {
    InterlockedExchange(&g_fakeSetLogged, 1);
    if (g_log) {
      fprintf(g_log, "FAKESET t=%lu\n", now);
      fflush(g_log);
    }
  }

  /* XInput input-system family: Enable is asserted via the worker log (see
     ProbeEnable), the audio endpoints prove the fake layer the same way the
     presence set above does. */
  ProbeEnable();
  DWORD hrD = ProbeDsound();
  if (hrD == 0 && InterlockedCompareExchange(&g_fakeDsoundSaw, 1, 0) == 0 &&
      g_fakeDsoundLogged == 0) {
    InterlockedExchange(&g_fakeDsoundLogged, 1);
    if (g_log) {
      fprintf(g_log, "FAKEDSOUND t=%lu\n", now);
      fflush(g_log);
    }
  }
  DWORD hrA = ProbeAudioIds();
  if (hrA == 0 && InterlockedCompareExchange(&g_fakeAudioIdsSaw, 1, 0) == 0 &&
      g_fakeAudioIdsLogged == 0) {
    InterlockedExchange(&g_fakeAudioIdsLogged, 1);
    if (g_log) {
      fprintf(g_log, "FAKEAUDIOIDS t=%lu\n", now);
      fflush(g_log);
    }
  }

  /* Raw-input fake-HID probe: cheap enough for the fast loop, and a 20 ms
     cadence makes the RAWFAKE marker appear within a heartbeat of the
     injection even when a smoke assert window is short. */
  if (ProbeRawList() && InterlockedCompareExchange(&g_rawFakeSaw, 1, 0) == 0 &&
      g_rawFakeLogged == 0) {
    InterlockedExchange(&g_rawFakeLogged, 1);
    if (g_log) {
      fprintf(g_log, "RAWFAKE t=%lu\n", now);
      fflush(g_log);
    }
  }
}

static LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
  if (m == WM_CLOSE || m == WM_DESTROY) {
    PostQuitMessage(0);
    return 0;
  }
  if (m == WM_KEYDOWN) {
    InterlockedExchange(&g_sawKeydown, 1);
    return 0;
  }
  return DefWindowProcW(h, m, w, l);
}

int WINAPI wWinMain(HINSTANCE h, HINSTANCE, LPWSTR cmd, int nCmdShow) {
  WNDCLASSEXW wc;
  ZeroMemory(&wc, sizeof wc);
  wc.cbSize = sizeof wc;
  wc.lpfnWndProc = WndProc;
  wc.hInstance = h;
  wc.lpszClassName = L"FsTestTarget";
  wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
  RegisterClassExW(&wc);

  if (cmd && cmd[0]) {
    /* cmd is the whole wide command line; the suffix is the first token. */
    LPCWSTR p = cmd;
    while (*p && *p != L' ' && *p != L'\t')
      p++;
    char narrow[32];
    int n = WideCharToMultiByte(CP_ACP, 0, cmd, (int)(p - cmd), narrow, 31, 0, 0);
    narrow[n > 0 ? n : 0] = 0;
    strcpy(g_suffix, narrow);
  }

  /* The suffix (if any) only goes into the log file; the title is fixed. */
  const wchar_t* title = L"focusspoof-target";

  g_hwnd = CreateWindowExW(0, wc.lpszClassName, title,
      WS_OVERLAPPEDWINDOW, 100, 100, 640, 480,
      NULL, NULL, h, NULL);
  ShowWindow(g_hwnd, SW_SHOWNORMAL);
  UpdateWindow(g_hwnd);

  CreateDirectoryA("logs", NULL);
  char logPath[64];
  sprintf(logPath, "logs\\focusspoof_target_%lu.log", GetCurrentProcessId());
  g_log = fopen(logPath, "w");

  DWORD last = 0;
  MSG m;
  while (1) {
    ProbeFast(); /* sample every ~20 ms so the rewrite window is never missed */
    if (PeekMessageW(&m, NULL, 0, 0, PM_REMOVE) > 0) {
      if (m.message == WM_QUIT)
        break;
      TranslateMessage(&m);
      DispatchMessageW(&m);
    }

    DWORD now = GetTickCount();
    if (now - last >= 2000) {
      last = now;
      if (g_log) {
        HWND fg = GetForegroundWindow();
        /*
         * key_blocked is a black-box check of the injected input blocker:
         * we post a WM_KEYDOWN to our own queue, let the message pump
         * dispatch it, and see whether WndProc ever received it. When the
         * blocker is live the subclassed SwallowProc eats the message
         * (key_blocked=1); without it WndProc sees the keydown.
         */
        InterlockedExchange(&g_sawKeydown, 0);
        PostMessageW(g_hwnd, WM_KEYDOWN, (WPARAM)'A', 0);
        Sleep(60);  /* give the pump time to dispatch/for the swallow */
        int key_blocked = !g_sawKeydown;
        DWORD xbtn = 0;
        DWORD xhr = ProbeXInput(&xbtn);
        fprintf(g_log,
                "suffix=%s t=%lu me:%d key_blocked:%d fg=%p target=%p "
                "xstate:%lu xbtn:%u\n",
                g_suffix, now, (int)(fg == g_hwnd), key_blocked,
                (void*)fg, (void*)g_hwnd, xhr, xbtn);
        fflush(g_log);
        /* WMI must land before the DI bind in the same tick: the auto-rescan
           driver treats "DI bound without WMI adoption" as a false-match and
           retracts the fakes, so the fake PNP entry has to be observable
           first. */
        ProbeWmiPnp();
        ProbeHidSetupDi();
        ProbeDirectInput();
      }
    }
    Sleep(10);
  }

  if (g_log)
    fclose(g_log);
  return 0;
}
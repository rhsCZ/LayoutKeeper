#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <array>
#include <algorithm>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>
#include <sstream>
#include <iomanip>
#include <cwctype>
#include "resource.h"
#ifdef _DEBUG
#include <cstdio>
#endif

#pragma comment(linker, "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

namespace {
constexpr wchar_t kSubkey[] = L"Software\\LayoutKeeper\\Processes";
constexpr wchar_t kMutex[] = L"Local\\LayoutKeeper.Instance";
constexpr UINT_PTR kTimer = 1;
// Sample frequently enough to catch a layout switch followed by a quick close.
// Once the foreground window's input thread exits, its final HKL is unavailable.
constexpr UINT kPollMs = 20;
constexpr UINT kApplyTimeoutMs = 700;
constexpr ULONGLONG kStartupDelayMs = 300;
constexpr ULONGLONG kRecentlyStartedMs = 5000;
constexpr wchar_t kClassName[] = L"LayoutKeeper.MessageWindow";
constexpr UINT kTrayMessage = WM_APP + 3;
constexpr UINT kTrayIconId = 1;

#ifdef _DEBUG
void DebugLog(const std::wstring& message) {
    SYSTEMTIME time{};
    GetLocalTime(&time);
    std::wostringstream line;
    line << L"[" << std::setfill(L'0') << std::setw(2) << time.wHour << L":" << std::setw(2) << time.wMinute
        << L":" << std::setw(2) << time.wSecond << L"." << std::setw(3) << time.wMilliseconds << L"] " << message << L"\r\n";
    const std::wstring text = line.str();
    DWORD written{};
    HANDLE output = GetStdHandle(STD_OUTPUT_HANDLE);
    if (output != nullptr && output != INVALID_HANDLE_VALUE) WriteConsoleW(output, text.c_str(), static_cast<DWORD>(text.size()), &written, nullptr);
    OutputDebugStringW(text.c_str());
}
std::wstring HexValue(ULONG_PTR value) {
    std::wostringstream output;
    output << L"0x" << std::hex << std::uppercase << value;
    return output.str();
}
#define LK_LOG(expression) do { std::wostringstream lk_log_stream; lk_log_stream << expression; DebugLog(lk_log_stream.str()); } while (false)
#endif

struct Handle {
    HANDLE value{};
    ~Handle() { if (value) CloseHandle(value); }
    Handle() = default;
    explicit Handle(HANDLE h) : value(h) {}
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
};
struct RegKey {
    HKEY value{};
    ~RegKey() { if (value) RegCloseKey(value); }
    RegKey() = default;
    RegKey(const RegKey&) = delete;
    RegKey& operator=(const RegKey&) = delete;
};

struct Identity { std::wstring image, serialized; };
struct Pending { HWND hwnd{}; HWND target{}; DWORD tid{}; ULONG_PTR desired{}; ULONG_PTR before{}; ULONGLONG expires{}; bool foregroundRetried{}; };
struct ScheduledRestore { HWND hwnd{}; HWND target{}; DWORD tid{}; ULONG_PTR desired{}; ULONG_PTR before{}; ULONGLONG due{}; };
struct Observation { HWND hwnd{}; DWORD tid{}; ULONG_PTR hkl{}; ULONGLONG tick{}; };
HWND g_window{};
HWND g_aboutWindow{};
HICON g_trayIcon{};
UINT g_taskbarCreated{};
bool g_trayVersion4{};
HWND g_activeAppWindow{};
HWINEVENTHOOK g_foregroundHook{};
HWINEVENTHOOK g_windowHook{};
std::vector<Pending> g_pending;
std::vector<ScheduledRestore> g_scheduled;
Observation g_last{};

std::wstring GetProcessImage(DWORD pid) {
    Handle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid));
    if (!process.value) return {};
    std::vector<wchar_t> buffer(32768);
    DWORD size = static_cast<DWORD>(buffer.size());
    if (!QueryFullProcessImageNameW(process.value, 0, buffer.data(), &size)) return {};
    return std::wstring(buffer.data(), size);
}
std::wstring Pack(const std::wstring& value) {
    return std::to_wstring(value.size()) + L":" + value;
}
bool GetIdentity(HWND hwnd, Identity& id) {
    if (!IsWindow(hwnd)) return false;
    DWORD pid{};
    GetWindowThreadProcessId(hwnd, &pid);
    if (!pid) return false;
    const std::wstring imagePath = GetProcessImage(pid);
    if (imagePath.empty()) return false;
    const size_t separator = imagePath.find_last_of(L"\\/");
    id.image = imagePath.substr(separator == std::wstring::npos ? 0 : separator + 1);
    if (id.image.empty()) return false;
    std::transform(id.image.begin(), id.image.end(), id.image.begin(), [](wchar_t ch) { return static_cast<wchar_t>(std::towlower(ch)); });
    id.serialized = Pack(id.image);
    return true;
}
uint64_t HashIdentity(const std::wstring& s) {
    uint64_t hash = 14695981039346656037ull;
    for (wchar_t ch : s) {
        uint32_t c = static_cast<uint32_t>(ch);
        for (int i = 0; i < 4; ++i) { hash ^= static_cast<uint8_t>(c & 0xff); hash *= 1099511628211ull; c >>= 8; }
    }
    return hash;
}
std::wstring Hex64(uint64_t n) { std::wostringstream out; out << std::hex << std::setw(16) << std::setfill(L'0') << n; return out.str(); }
std::wstring EntryName(const Identity& id, bool create) {
    RegKey root;
    DWORD disposition{};
    LONG rc = create ? RegCreateKeyExW(HKEY_CURRENT_USER, kSubkey, 0, nullptr, 0, KEY_READ | KEY_WRITE, nullptr, &root.value, &disposition)
                     : RegOpenKeyExW(HKEY_CURRENT_USER, kSubkey, 0, KEY_READ, &root.value);
    if (rc != ERROR_SUCCESS) return {};
    const std::wstring base = Hex64(HashIdentity(id.serialized));
    for (unsigned int suffix = 0; suffix < 16; ++suffix) {
        std::wstring candidate = base;
        if (suffix) candidate += L"-" + std::to_wstring(suffix);
        RegKey entry;
        DWORD d{};
        rc = create ? RegCreateKeyExW(root.value, candidate.c_str(), 0, nullptr, 0, KEY_READ | KEY_WRITE, nullptr, &entry.value, &d)
                    : RegOpenKeyExW(root.value, candidate.c_str(), 0, KEY_READ, &entry.value);
        if (rc == ERROR_FILE_NOT_FOUND) return create ? candidate : L"";
        if (rc != ERROR_SUCCESS) return {};
        DWORD type{}, bytes{};
        rc = RegQueryValueExW(entry.value, L"Identity", nullptr, &type, nullptr, &bytes);
        if (rc == ERROR_FILE_NOT_FOUND && create) return candidate;
        if (rc != ERROR_SUCCESS || type != REG_SZ || bytes < sizeof(wchar_t) || bytes > 1024 * 1024) continue;
        std::vector<wchar_t> stored(bytes / sizeof(wchar_t));
        if (RegQueryValueExW(entry.value, L"Identity", nullptr, &type, reinterpret_cast<BYTE*>(stored.data()), &bytes) == ERROR_SUCCESS && std::wstring(stored.data()) == id.serialized) return candidate;
    }
    return {};
}
bool ReadLayout(const Identity& id, ULONG_PTR& layout) {
    std::wstring name = EntryName(id, false);
    if (name.empty()) return false;
    RegKey root, entry;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kSubkey, 0, KEY_READ, &root.value) != ERROR_SUCCESS || RegOpenKeyExW(root.value, name.c_str(), 0, KEY_READ, &entry.value) != ERROR_SUCCESS) return false;
    DWORD type{}, bytes = sizeof(uint64_t); uint64_t value{};
    if (RegQueryValueExW(entry.value, L"HKL", nullptr, &type, reinterpret_cast<BYTE*>(&value), &bytes) != ERROR_SUCCESS || type != REG_QWORD || bytes != sizeof(value)) return false;
    layout = static_cast<ULONG_PTR>(value);
    return true;
}
bool WriteLayout(const Identity& id, ULONG_PTR layout) {
    std::wstring name = EntryName(id, true);
    if (name.empty()) {
        #ifdef _DEBUG
        LK_LOG(L"Registry: failed to find/create identity entry for " << id.image);
        #endif
        return false;
    }
    RegKey root, entry;
    LONG rc = RegCreateKeyExW(HKEY_CURRENT_USER, kSubkey, 0, nullptr, 0, KEY_READ | KEY_WRITE, nullptr, &root.value, nullptr);
    if (rc == ERROR_SUCCESS) rc = RegCreateKeyExW(root.value, name.c_str(), 0, nullptr, 0, KEY_READ | KEY_WRITE, nullptr, &entry.value, nullptr);
    if (rc != ERROR_SUCCESS) {
        #ifdef _DEBUG
        LK_LOG(L"Registry: cannot open entry for " << id.image << L", error=" << rc);
        #endif
        return false;
    }
    DWORD type{}, bytes{};
    if (RegQueryValueExW(entry.value, L"Identity", nullptr, &type, nullptr, &bytes) != ERROR_SUCCESS) {
        const DWORD identityBytes = static_cast<DWORD>((id.serialized.size() + 1) * sizeof(wchar_t));
        rc = RegSetValueExW(entry.value, L"Identity", 0, REG_SZ, reinterpret_cast<const BYTE*>(id.serialized.c_str()), identityBytes);
        if (rc != ERROR_SUCCESS) {
            #ifdef _DEBUG
            LK_LOG(L"Registry: failed to write process identity for " << id.image << L", error=" << rc);
            #endif
            return false;
        }
    }
    ULONG_PTR existing{};
    if (ReadLayout(id, existing) && existing == layout) {
        #ifdef _DEBUG
        LK_LOG(L"Registry: unchanged HKL " << HexValue(layout) << L" for " << id.image << L"; no write");
        #endif
        return true;
    }
    uint64_t stored = static_cast<uint64_t>(layout);
    rc = RegSetValueExW(entry.value, L"HKL", 0, REG_QWORD, reinterpret_cast<const BYTE*>(&stored), sizeof(stored));
    if (rc != ERROR_SUCCESS) {
        #ifdef _DEBUG
        LK_LOG(L"Registry: failed to write HKL " << HexValue(layout) << L" for " << id.image << L", error=" << rc);
        #endif
        return false;
    }
    #ifdef _DEBUG
    LK_LOG(L"Registry: saved HKL " << HexValue(layout) << L" for " << id.image << L" (entry " << name << L")");
    #endif
    return true;
}
HWND ResolveAppWindow(HWND hwnd) {
    wchar_t windowClass[64]{};
    GetClassNameW(hwnd, windowClass, static_cast<int>(std::size(windowClass)));
    if (std::wstring_view(windowClass) != L"ApplicationFrameWindow") return hwnd;

    // The frame belongs to ApplicationFrameHost, but its CoreWindow owns input.
    const HWND appWindow = FindWindowExW(hwnd, nullptr, L"Windows.UI.Core.CoreWindow", nullptr);
    return IsWindowVisible(appWindow) ? appWindow : nullptr;
}
ULONG_PTR QueryLayout(HWND hwnd, DWORD& tid, HWND* focusHwnd = nullptr) {
    DWORD windowTid = GetWindowThreadProcessId(hwnd, nullptr);
    if (!windowTid) { tid = 0; return 0; }
    HWND target = hwnd;
    GUITHREADINFO guiInfo{ sizeof(guiInfo) };
    if (GetGUIThreadInfo(windowTid, &guiInfo) && guiInfo.hwndFocus) {
        DWORD focusTid = GetWindowThreadProcessId(guiInfo.hwndFocus, nullptr);
        DWORD focusPid{}, windowPid{};
        GetWindowThreadProcessId(guiInfo.hwndFocus, &focusPid);
        GetWindowThreadProcessId(hwnd, &windowPid);
        if (focusTid && focusPid == windowPid &&
            (guiInfo.hwndFocus == hwnd || IsChild(hwnd, guiInfo.hwndFocus))) {
            target = guiInfo.hwndFocus;
            windowTid = focusTid;
        }
    }
    tid = windowTid;
    if (focusHwnd) *focusHwnd = target;
    return reinterpret_cast<ULONG_PTR>(GetKeyboardLayout(tid));
}
bool IsRecentlyStarted(DWORD pid) {
    Handle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid));
    if (!process.value) return false;
    FILETIME creation{}, exit{}, kernel{}, user{}, now{};
    if (!GetProcessTimes(process.value, &creation, &exit, &kernel, &user)) return false;
    GetSystemTimeAsFileTime(&now);
    ULARGE_INTEGER start{}, current{};
    start.LowPart = creation.dwLowDateTime; start.HighPart = creation.dwHighDateTime;
    current.LowPart = now.dwLowDateTime; current.HighPart = now.dwHighDateTime;
    constexpr ULONGLONG kFileTimeTicksPerMs = 10000;
    return current.QuadPart >= start.QuadPart && current.QuadPart - start.QuadPart <= kRecentlyStartedMs * kFileTimeTicksPerMs;
}
bool IsLayoutInstalled(ULONG_PTR layout) {
    int count = GetKeyboardLayoutList(0, nullptr);
    if (count <= 0 || count > 128) return false;
    std::vector<HKL> layouts(static_cast<size_t>(count));
    int actual = GetKeyboardLayoutList(count, layouts.data());
    for (int i = 0; i < actual; ++i) if (reinterpret_cast<ULONG_PTR>(layouts[i]) == layout) return true;
    return false;
}
void OnWindowAvailable(HWND hwnd, DWORD reason) {
    (void)reason;
    if (!hwnd) {
        #ifdef _DEBUG
        LK_LOG(L"Window event ignored: null HWND, event=" << reason);
        #endif
        return;
    }
    if (!IsWindow(hwnd)) {
        #ifdef _DEBUG
        LK_LOG(L"Window event ignored: invalid HWND=" << HexValue(reinterpret_cast<ULONG_PTR>(hwnd)) << L", event=" << reason);
        #endif
        return;
    }
    if (hwnd == g_window || hwnd == g_aboutWindow) {
        #ifdef _DEBUG
        LK_LOG(L"Window event ignored: LayoutKeeper's own window, HWND=" << HexValue(reinterpret_cast<ULONG_PTR>(hwnd)));
        #endif
        return;
    }
    const HWND root = GetAncestor(hwnd, GA_ROOT);
    // A hosted app can own the actual foreground HWND even when its root
    // belongs to ApplicationFrameHost. Only filter child create/show events.
    if (root != hwnd && reason != EVENT_SYSTEM_FOREGROUND) {
        #ifdef _DEBUG
        LK_LOG(L"Window event ignored: child window HWND=" << HexValue(reinterpret_cast<ULONG_PTR>(hwnd))
            << L", root HWND=" << HexValue(reinterpret_cast<ULONG_PTR>(root)) << L", event=" << reason);
        #endif
        return;
    }
    const bool foreground = GetForegroundWindow() == hwnd;
    if (foreground) {
        hwnd = ResolveAppWindow(hwnd);
        if (!hwnd) {
            // The frame can become foreground before the app's input window exists.
            g_activeAppWindow = nullptr;
            return;
        }
        g_activeAppWindow = hwnd;
    }
    Identity id;
    if (!GetIdentity(hwnd, id)) {
        #ifdef _DEBUG
        LK_LOG(L"Window event: unable to identify process for hwnd=" << HexValue(reinterpret_cast<ULONG_PTR>(hwnd)));
        #endif
        return;
    }
    DWORD tid{};
    HWND inputHwnd{};
    ULONG_PTR current = QueryLayout(hwnd, tid, &inputHwnd);
    if (!current || !tid) {
        #ifdef _DEBUG
        LK_LOG(L"Window event: GetKeyboardLayout failed for hwnd=" << HexValue(reinterpret_cast<ULONG_PTR>(hwnd)));
        #endif
        return;
    }
    DWORD pid{};
    GetWindowThreadProcessId(hwnd, &pid);
    #ifdef _DEBUG
    const wchar_t* eventName = reason == EVENT_SYSTEM_FOREGROUND ? L"FOREGROUND" : reason == EVENT_OBJECT_CREATE ? L"CREATE" : reason == EVENT_OBJECT_SHOW ? L"SHOW" : L"UNKNOWN";
    LK_LOG(L"Window event=" << eventName << L"(" << reason << L"); exe=" << id.image << L"; hwnd=" << HexValue(reinterpret_cast<ULONG_PTR>(hwnd))
        << L"; pid=" << pid << L"; input HWND=" << HexValue(reinterpret_cast<ULONG_PTR>(inputHwnd))
        << L"; input tid=" << tid << L"; current HKL=" << HexValue(current) << (foreground ? L"; foreground" : L"; background"));
    #else
    (void)pid;
    #endif
    if (!foreground) {
        #ifdef _DEBUG
        LK_LOG(L"Restore deferred: this window is not the current foreground window; no layout request or registry update performed");
        #endif
        return;
    }
    ULONG_PTR saved{};
    if (!ReadLayout(id, saved)) {
        #ifdef _DEBUG
        LK_LOG(L"Registry: no valid saved HKL for " << id.image << L"; storing current " << HexValue(current));
        #endif
        WriteLayout(id, current);
        if (foreground) g_last = {hwnd, tid, current, GetTickCount64()};
        return;
    }
    #ifdef _DEBUG
    LK_LOG(L"Registry: loaded saved HKL " << HexValue(saved) << L" for " << id.image);
    #endif
    if (saved == current) {
        #ifdef _DEBUG
        LK_LOG(L"Restore not needed: current HKL already matches saved value");
        #endif
        if (foreground) g_last = {hwnd, tid, current, GetTickCount64()};
        return;
    }
    if (!IsLayoutInstalled(saved)) {
        #ifdef _DEBUG
        LK_LOG(L"Restore skipped: saved HKL " << HexValue(saved) << L" is not in GetKeyboardLayoutList; replacing it with " << HexValue(current));
        #endif
        WriteLayout(id, current);
        if (foreground) g_last = {hwnd, tid, current, GetTickCount64()};
        return;
    }
    if (IsRecentlyStarted(pid)) {
        for (const ScheduledRestore& scheduled : g_scheduled) {
            if (scheduled.hwnd == hwnd && scheduled.tid == tid && scheduled.desired == saved) return;
        }
        if (g_scheduled.size() >= 16) g_scheduled.erase(g_scheduled.begin());
        g_scheduled.push_back({hwnd, inputHwnd, tid, saved, current, GetTickCount64() + kStartupDelayMs});
        #ifdef _DEBUG
        LK_LOG(L"Restore delayed for recently started process: exe=" << id.image << L", pid=" << pid
            << L", delay=" << kStartupDelayMs << L" ms, current HKL=" << HexValue(current)
            << L", requested HKL=" << HexValue(saved));
        #endif
        return;
    }
    for (Pending& pending : g_pending) {
        if (pending.hwnd == hwnd && pending.tid == tid && pending.desired == saved) {
            if (foreground && !pending.foregroundRetried) {
                pending.foregroundRetried = true;
                pending.target = inputHwnd;
                [[maybe_unused]] const BOOL posted = PostMessageW(pending.target, WM_INPUTLANGCHANGEREQUEST, 0, static_cast<LPARAM>(saved));
                #ifdef _DEBUG
                LK_LOG(L"Restore retry on foreground: hwnd=" << HexValue(reinterpret_cast<ULONG_PTR>(hwnd))
                    << L", input hwnd=" << HexValue(reinterpret_cast<ULONG_PTR>(pending.target)) << L", HKL=" << HexValue(saved)
                    << L", PostMessage=" << (posted ? L"success" : L"failed") << (posted ? L"" : L", error=" + std::to_wstring(GetLastError())));
                if (!posted) LK_LOG(L"Restore retry PostMessageW failed");
                #endif
            }
            return;
        }
    }
    if (g_pending.size() >= 16) g_pending.erase(g_pending.begin());
    g_pending.push_back({hwnd, inputHwnd, tid, saved, current, GetTickCount64() + 2000});
    const BOOL posted = PostMessageW(inputHwnd, WM_INPUTLANGCHANGEREQUEST, 0, static_cast<LPARAM>(saved));
    #ifdef _DEBUG
    LK_LOG(L"Restore request: hwnd=" << HexValue(reinterpret_cast<ULONG_PTR>(hwnd)) << L", exe=" << id.image
        << L", input hwnd=" << HexValue(reinterpret_cast<ULONG_PTR>(inputHwnd))
        << L", current HKL=" << HexValue(current) << L", requested HKL=" << HexValue(saved)
        << L", PostMessage=" << (posted ? L"success" : L"failed") << (posted ? L"" : L", error=" + std::to_wstring(GetLastError())));
    #endif
    if (!posted) {
        g_pending.pop_back();
        if (foreground) g_last = {hwnd, tid, current, GetTickCount64()};
    }
}
void CALLBACK WinEventProc(HWINEVENTHOOK, DWORD event, HWND hwnd, LONG object, LONG child, DWORD, DWORD) {
    if (event == EVENT_SYSTEM_FOREGROUND && object == OBJID_WINDOW && child == CHILDID_SELF) {
        PostMessageW(g_window, WM_APP + 1, reinterpret_cast<WPARAM>(hwnd), 0);
    } else if ((event == EVENT_OBJECT_CREATE || event == EVENT_OBJECT_SHOW) && object == OBJID_WINDOW && child == CHILDID_SELF) {
        // Object create/show hooks also report child controls. Their top-level root
        // receives its own create/show event, so avoid queueing every control.
        if (!hwnd || GetAncestor(hwnd, GA_ROOT) != hwnd) return;
        PostMessageW(g_window, WM_APP + 2, static_cast<WPARAM>(event), reinterpret_cast<LPARAM>(hwnd));
    }
}
void OnForeground(HWND hwnd) {
    const HWND currentForeground = GetForegroundWindow();
    if (!currentForeground) return;
    if (hwnd != currentForeground) {
        #ifdef _DEBUG
        LK_LOG(L"Ignoring stale foreground event for HWND=" << HexValue(reinterpret_cast<ULONG_PTR>(hwnd))
            << L"; current foreground is HWND=" << HexValue(reinterpret_cast<ULONG_PTR>(currentForeground)));
        #endif
        hwnd = currentForeground;
    }
    OnWindowAvailable(hwnd, EVENT_SYSTEM_FOREGROUND);
}
void CheckActive() {
    const HWND foreground = GetForegroundWindow();
    const HWND hwnd = ResolveAppWindow(foreground);
    if (!hwnd || hwnd == g_window || hwnd == g_aboutWindow) {
        g_activeAppWindow = nullptr;
        return;
    }
    if (hwnd != g_activeAppWindow) {
        // Handle a newly attached app window even if the frame stayed foreground.
        OnForeground(foreground);
        return;
    }
    DWORD tid{};
    HWND inputHwnd{};
    ULONG_PTR current = QueryLayout(hwnd, tid, &inputHwnd);
    if (!tid || !current) return;
    const ULONGLONG now = GetTickCount64();
    for (auto it = g_scheduled.begin(); it != g_scheduled.end();) {
        if (!IsWindow(it->hwnd)) { it = g_scheduled.erase(it); continue; }
        if (it->hwnd != hwnd) { ++it; continue; }
        if (current == it->desired) {
            #ifdef _DEBUG
            LK_LOG(L"Delayed restore canceled: requested HKL is already active");
            #endif
            it = g_scheduled.erase(it);
            continue;
        }
        if (it->tid != tid) {
            #ifdef _DEBUG
            LK_LOG(L"Delayed restore is waiting for the active input thread to stabilize: hwnd="
                << HexValue(reinterpret_cast<ULONG_PTR>(hwnd)) << L", old tid=" << it->tid << L", new tid=" << tid
                << L", input hwnd=" << HexValue(reinterpret_cast<ULONG_PTR>(inputHwnd)));
            #endif
            it->target = inputHwnd;
            it->tid = tid;
            it->before = current;
            it->due = now + kStartupDelayMs;
            ++it;
            continue;
        }
        it->target = inputHwnd;
        if (current != it->before) {
            #ifdef _DEBUG
            LK_LOG(L"Delayed restore canceled because the user changed layout during startup delay: hwnd="
                << HexValue(reinterpret_cast<ULONG_PTR>(hwnd)) << L", before=" << HexValue(it->before)
                << L", observed=" << HexValue(current));
            #endif
            Identity id;
            if (GetIdentity(hwnd, id)) WriteLayout(id, current);
            it = g_scheduled.erase(it);
            g_last = {hwnd, tid, current, now};
            continue;
        }
        if (now < it->due) { ++it; continue; }
        if (g_pending.size() >= 16) g_pending.erase(g_pending.begin());
        g_pending.push_back({it->hwnd, it->target, it->tid, it->desired, it->before, now + 2000});
        const BOOL posted = PostMessageW(it->target, WM_INPUTLANGCHANGEREQUEST, 0, static_cast<LPARAM>(it->desired));
        #ifdef _DEBUG
        LK_LOG(L"Delayed restore request: hwnd=" << HexValue(reinterpret_cast<ULONG_PTR>(it->hwnd))
            << L", input hwnd=" << HexValue(reinterpret_cast<ULONG_PTR>(it->target)) << L", tid=" << it->tid << L", current HKL=" << HexValue(current)
            << L", requested HKL=" << HexValue(it->desired) << L", PostMessage=" << (posted ? L"success" : L"failed"));
        #endif
        if (!posted) g_pending.pop_back();
        it = g_scheduled.erase(it);
        if (posted) return;
    }
    for (auto it = g_pending.begin(); it != g_pending.end();) {
        if (it->hwnd == hwnd && it->tid == tid) {
            if (current == it->desired) {
                #ifdef _DEBUG
                LK_LOG(L"Restore verified: hwnd=" << HexValue(reinterpret_cast<ULONG_PTR>(hwnd)) << L", tid=" << tid << L", HKL=" << HexValue(current));
                #endif
                it = g_pending.erase(it);
                g_last = {hwnd, tid, current, now};
                return;
            }
            if (current != it->before) {
                #ifdef _DEBUG
                LK_LOG(L"Layout changed during pending restore; accepting it as a manual choice. hwnd="
                    << HexValue(reinterpret_cast<ULONG_PTR>(hwnd)) << L", previous=" << HexValue(it->before)
                    << L", requested=" << HexValue(it->desired) << L", observed=" << HexValue(current));
                #endif
                Identity id;
                if (GetIdentity(hwnd, id)) WriteLayout(id, current);
                it = g_pending.erase(it);
                g_last = {hwnd, tid, current, now};
                return;
            }
            if (now < it->expires) return;
            #ifdef _DEBUG
            LK_LOG(L"Restore timed out after 2 seconds: hwnd=" << HexValue(reinterpret_cast<ULONG_PTR>(hwnd))
                << L", requested=" << HexValue(it->desired) << L", still observed=" << HexValue(current)
                << L". Target may have ignored or blocked WM_INPUTLANGCHANGEREQUEST.");
            #endif
            it = g_pending.erase(it);
            break;
        }
        if (now >= it->expires || !IsWindow(it->hwnd)) it = g_pending.erase(it); else ++it;
    }
    if (g_last.hwnd == hwnd && g_last.tid == tid && g_last.hkl != current) {
        #ifdef _DEBUG
        LK_LOG(L"Manual/external layout change detected in foreground window: hwnd=" << HexValue(reinterpret_cast<ULONG_PTR>(hwnd))
            << L", input hwnd=" << HexValue(reinterpret_cast<ULONG_PTR>(inputHwnd)) << L", input tid=" << tid
            << L", old HKL=" << HexValue(g_last.hkl) << L", new HKL=" << HexValue(current));
        #endif
        Identity id;
        if (GetIdentity(hwnd, id)) WriteLayout(id, current);
    }
    g_last = {hwnd, tid, current, now};
}
NOTIFYICONDATAW TrayIconData(HWND hwnd) {
    NOTIFYICONDATAW data{sizeof(data)};
    data.hWnd = hwnd;
    data.uID = kTrayIconId;
    return data;
}

void AddTrayIcon(HWND hwnd) {
    if (!g_trayIcon) return;
    auto data = TrayIconData(hwnd);
    data.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP;
    data.uCallbackMessage = kTrayMessage;
    data.hIcon = g_trayIcon;
    wcscpy_s(data.szTip, L"LayoutKeeper");
    g_trayVersion4 = false;
    if (!Shell_NotifyIconW(NIM_ADD, &data)) {
        #ifdef _DEBUG
        LK_LOG(L"Unable to add the notification area icon");
        #endif
        return;
    }
    data.uVersion = NOTIFYICON_VERSION_4;
    g_trayVersion4 = Shell_NotifyIconW(NIM_SETVERSION, &data) != FALSE;
}

INT_PTR CALLBACK AboutDialogProc(HWND dialog, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_INITDIALOG:
        g_aboutWindow = dialog;
        SendMessageW(dialog, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(g_trayIcon));
        return TRUE;
    case WM_NOTIFY: {
        const auto* header = reinterpret_cast<const NMHDR*>(lParam);
        if (header->idFrom == IDC_PROJECT_LINK && (header->code == NM_CLICK || header->code == NM_RETURN)) {
            const auto* link = reinterpret_cast<const NMLINK*>(lParam);
            ShellExecuteW(dialog, L"open", link->item.szUrl, nullptr, nullptr, SW_SHOWNORMAL);
            return TRUE;
        }
        break;
    }
    case WM_COMMAND:
        if (LOWORD(wParam) == IDOK || LOWORD(wParam) == IDCANCEL) {
            EndDialog(dialog, LOWORD(wParam));
            return TRUE;
        }
        break;
    case WM_CLOSE:
        EndDialog(dialog, IDCANCEL);
        return TRUE;
    case WM_DESTROY:
        g_aboutWindow = nullptr;
        break;
    }
    return FALSE;
}

void ShowTrayMenu(HWND hwnd) {
    const HINSTANCE instance = GetModuleHandleW(nullptr);
    const HMENU menu = LoadMenuW(instance, MAKEINTRESOURCEW(IDR_TRAY_MENU));
    if (!menu) return;

    POINT position{};
    GetCursorPos(&position);
    NOTIFYICONIDENTIFIER icon{sizeof(icon)};
    icon.hWnd = hwnd;
    icon.uID = kTrayIconId;
    RECT bounds{};
    if (SUCCEEDED(Shell_NotifyIconGetRect(&icon, &bounds))) position = {bounds.left, bounds.top};

    SetForegroundWindow(hwnd);
    const UINT command = TrackPopupMenu(GetSubMenu(menu, 0), TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON | TPM_BOTTOMALIGN,
                                        position.x, position.y, 0, hwnd, nullptr);
    PostMessageW(hwnd, WM_NULL, 0, 0);
    DestroyMenu(menu);

    if (command) {
        PostMessageW(hwnd, WM_COMMAND, MAKEWPARAM(command, 0), 0);
    } else {
        auto data = TrayIconData(hwnd);
        Shell_NotifyIconW(NIM_SETFOCUS, &data);
    }
}

LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    if (g_taskbarCreated && message == g_taskbarCreated) {
        AddTrayIcon(hwnd);
        return 0;
    }
    switch (message) {
    case WM_APP + 1: OnForeground(reinterpret_cast<HWND>(wParam)); return 0;
    case WM_APP + 2: OnWindowAvailable(reinterpret_cast<HWND>(lParam), static_cast<DWORD>(wParam)); return 0;
    case kTrayMessage:
        if (LOWORD(lParam) == WM_CONTEXTMENU || (!g_trayVersion4 && lParam == WM_RBUTTONUP)) ShowTrayMenu(hwnd);
        return 0;
    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case IDM_ABOUT:
            if (g_aboutWindow) {
                SetForegroundWindow(g_aboutWindow);
            } else {
                [[maybe_unused]] const INT_PTR result = DialogBoxParamW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDD_ABOUT),
                                                                        nullptr, AboutDialogProc, 0);
                #ifdef _DEBUG
                if (result == -1) LK_LOG(L"Unable to open About dialog: error=" << GetLastError());
                #endif
            }
            return 0;
        case IDM_EXIT:
            if (g_aboutWindow) EndDialog(g_aboutWindow, IDCANCEL);
            PostQuitMessage(0);
            return 0;
        }
        break;
    case WM_TIMER: CheckActive(); return 0;
    case WM_QUERYENDSESSION: return TRUE;
    case WM_ENDSESSION: if (wParam) PostQuitMessage(0); return 0;
    case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(hwnd, message, wParam, lParam);
}
}

int RunApplication(HINSTANCE instance) {
    #ifdef _DEBUG
    SetConsoleTitleW(L"LayoutKeeper - Debug diagnostics");
    LK_LOG(L"Starting LayoutKeeper debug build; polling interval=" << kPollMs << L" ms; identity key is executable filename");
    #endif
    Handle mutex(CreateMutexW(nullptr, TRUE, kMutex));
    if (!mutex.value) {
        #ifdef _DEBUG
        LK_LOG(L"Startup failed: CreateMutexW error=" << GetLastError());
        #endif
        return 1;
    }
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        #ifdef _DEBUG
        LK_LOG(L"Another LayoutKeeper instance is already running; exiting");
        #endif
        return 0;
    }
    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = WindowProc; wc.hInstance = instance; wc.lpszClassName = kClassName;
    if (!RegisterClassExW(&wc)) {
        #ifdef _DEBUG
        LK_LOG(L"Startup failed: RegisterClassExW error=" << GetLastError());
        #endif
        return 1;
    }
    // Explorer broadcasts TaskbarCreated only to top-level windows.
    g_window = CreateWindowExW(WS_EX_TOOLWINDOW, kClassName, L"LayoutKeeper", WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, instance, nullptr);
    if (!g_window) {
        #ifdef _DEBUG
        LK_LOG(L"Startup failed: CreateWindowExW error=" << GetLastError());
        #endif
        return 1;
    }
    INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_LINK_CLASS};
    InitCommonControlsEx(&controls);
    [[maybe_unused]] const HRESULT iconResult = LoadIconMetric(instance, MAKEINTRESOURCEW(IDI_LAYOUTKEEPER), LIM_SMALL, &g_trayIcon);
    #ifdef _DEBUG
    if (FAILED(iconResult)) LK_LOG(L"Unable to load the notification area icon");
    #endif
    g_taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
    AddTrayIcon(g_window);
    g_foregroundHook = SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND, nullptr, WinEventProc, 0, 0, WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
    #ifdef _DEBUG
    LK_LOG(L"Foreground event hook: " << (g_foregroundHook ? L"installed" : L"FAILED") << (g_foregroundHook ? L"" : L", error=" + std::to_wstring(GetLastError())));
    #endif
    g_windowHook = SetWinEventHook(EVENT_OBJECT_CREATE, EVENT_OBJECT_SHOW, nullptr, WinEventProc, 0, 0, WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
    #ifdef _DEBUG
    LK_LOG(L"Window create/show event hook: " << (g_windowHook ? L"installed" : L"FAILED") << (g_windowHook ? L"" : L", error=" + std::to_wstring(GetLastError())));
    #endif
    [[maybe_unused]] const UINT_PTR timer = SetTimer(g_window, kTimer, kPollMs, nullptr);
    #ifdef _DEBUG
    if (!timer) LK_LOG(L"WARNING: SetTimer failed, error=" << GetLastError());
    else LK_LOG(L"Foreground layout polling timer started");
    #endif
    OnForeground(GetForegroundWindow());
    MSG message{};
    BOOL result{};
    while ((result = GetMessageW(&message, nullptr, 0, 0)) > 0) { TranslateMessage(&message); DispatchMessageW(&message); }
    #ifdef _DEBUG
    if (result == -1) LK_LOG(L"Message loop failed: GetMessageW error=" << GetLastError());
    else LK_LOG(L"Message loop is exiting");
    #endif
    KillTimer(g_window, kTimer);
    if (g_foregroundHook) UnhookWinEvent(g_foregroundHook);
    if (g_windowHook) UnhookWinEvent(g_windowHook);
    auto trayData = TrayIconData(g_window);
    Shell_NotifyIconW(NIM_DELETE, &trayData);
    if (g_trayIcon) DestroyIcon(g_trayIcon);
    DestroyWindow(g_window);
    UnregisterClassW(kClassName, instance);
    ReleaseMutex(mutex.value);
    #ifdef _DEBUG
    LK_LOG(L"LayoutKeeper stopped");
    #endif
    return static_cast<int>(message.wParam);
}

#ifdef _DEBUG
int wmain() { return RunApplication(GetModuleHandleW(nullptr)); }
#else
int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) { return RunApplication(instance); }
#endif


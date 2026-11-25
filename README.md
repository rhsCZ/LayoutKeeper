# LayoutKeeper

LayoutKeeper is a small Windows 11 background application that remembers an input locale (HKL) per executable process and requests restoration when one of that process's windows is created or becomes active. It uses only C++20 and the Windows SDK. The native Visual Studio C++ project builds x86 and x64 Windows subsystem executables in Release.

## Build

Open `LayoutKeeper.slnx` in Visual Studio with the Desktop development with C++ workload and a Windows 10/11 SDK installed. Build **x64 / Release** for `output\x64\Release\LayoutKeeper.exe` or **Win32 / Release** for `output\Win32\Release\LayoutKeeper.exe`.

The **Debug** configuration opens a diagnostic console. It logs foreground/create/show events, executable identity, current and saved HKLs, registry reads/writes, restore requests and their verification or timeout, and manual layout changes. **Release** runs without a console.

The project targets MSVC toolset v145. Change `PlatformToolset` in `LayoutKeeper.vcxproj` to the installed MSVC toolset if v145 is unavailable. No CMake, third-party libraries, administrator rights, or .NET runtime are required. Release builds link the Visual C++ runtime statically.

## Installers

Build **Release | x64** for `setup_x64.iss` or **Release | Win32** for `setup_x86.iss`. Compile the matching script with Inno Setup 6.3 or newer. The setup executables are written to `installer-output`. The package version comes from the application resource by default. Pass `/DAppVersion=<version>` to `ISCC.exe` to override the package version.

Setup installs into `%LOCALAPPDATA%\Programs\LayoutKeeper` by default without elevation. It adds the `LayoutKeeper` value under `HKEY_CURRENT_USER\Software\Microsoft\Windows\CurrentVersion\Run`, so the application starts when that user signs in. Uninstall removes the startup value.

## Use

Run `LayoutKeeper.exe`. Right-click its notification area icon and select **About** for the version and project link or **Exit** to stop it. Launching a second copy in the same session exits immediately. Stored preferences remain in the current user's registry.

On first foreground encounter with an executable, its current HKL is saved. For later windows, LayoutKeeper restores only when that window is actually foreground; background create/show notifications are deferred because their thread may share input state with the active window. A newly created window is therefore restored when it receives foreground. LayoutKeeper observes `EVENT_SYSTEM_FOREGROUND`, `EVENT_OBJECT_CREATE`, and `EVENT_OBJECT_SHOW` with out-of-context WinEvent hooks, and checks the foreground layout every 20 ms because Windows delivers `WM_INPUTLANGCHANGE` to the target application. A layout switch and window closure between samples can still escape detection. A successfully observed requested change is treated as automatic; a different layout observed after a pending request is treated as a user choice and saved.

## Registry

Data is stored under `HKEY_CURRENT_USER\Software\LayoutKeeper\Processes`. Each subkey is named by a 64-bit FNV-1a hash of the lowercased executable filename including `.exe`, with a numeric suffix used if a hash collision is detected. `Identity` stores that filename for collision checking; `HKL` stores the pointer-sized HKL as `REG_QWORD`. Writes happen only when the HKL differs from the saved value.

## Window identity and limits

The persistent identity is only the executable filename, including its `.exe` extension; its directory is ignored. Every process instance with the same filename shares one saved layout, even when launched from different paths. This is stable across process restarts and does not depend on titles. Different executable filenames have separate preferences. HWND, PID, and TID are used only transiently and are never persisted.

Windows associates the input locale with a thread, not with a process or HWND. `GetKeyboardLayout` queries the thread that owns the keyboard-focused HWND, falling back to the app window's thread when no focused HWND is available. For hosted applications such as Windows Calculator, LayoutKeeper uses the app's CoreWindow inside the active frame. The stored HKL is the complete pointer-sized value, not just its language ID. HKL values are system handles and can vary by session or layout configuration; the implementation checks the stored value against `GetKeyboardLayoutList` before requesting restoration. Text Services Framework input methods may have state beyond the HKL and may not restore identically through the classic Win32 message.

When a saved layout differs from the current layout, LayoutKeeper delays the restore request by 300 milliseconds if the target process started within the last five seconds. This gives applications such as modern Notepad time to finish initializing. If the user changes the layout during this delay, the pending restore is canceled and the new layout is saved.

`WM_INPUTLANGCHANGEREQUEST` is a posted request to the target's focused window and can be ignored, delayed, or blocked by integrity/UIPI boundaries. LayoutKeeper verifies success by querying the target thread on later foreground checks and does not retry indefinitely. If the requested HKL is no longer listed, the current HKL replaces the stale preference. Some applications manage input through TSF or custom controls and may not honor the request. Per-window input method settings in Windows can also affect what the target thread reports.

The app does not install keyboard hooks, inject code, run elevated, or enumerate all processes. The timer queries only the foreground window's input thread and writes to the registry when a layout changes.

## Manual checks

On Windows 11 with at least two input layouts installed:

1. Run LayoutKeeper, set one application to Czech and another to English, then switch between them.
2. Open another window from an application with a saved layout and confirm LayoutKeeper attempts restoration when the window is created or shown and when it becomes foreground.
3. Close and reopen one application, then switch to it and inspect whether its layout is restored.
4. Open multiple windows from one executable and confirm they share one saved layout regardless of title.
5. Change layout manually while a window stays active, then switch away and back to confirm the updated setting is saved for that executable.
6. Remove a previously stored layout and activate the window; the current available layout should replace the stale preference.
7. Try an elevated application and an application with custom IME/TSF handling; requests can fail across integrity levels or be ignored.
8. Observe the process in Task Manager while idle. It should use the message loop and low-frequency foreground check without scanning unrelated processes.

The project was built successfully with the installed Visual Studio MSVC toolset. The interactive scenarios above require a Windows desktop session and remain manual checks.

## Contributing

Report bugs or suggest improvements in [GitHub Issues](https://github.com/rhsCZ/LayoutKeeper/issues). If you already have a fix or improvement, feel free to open a [pull request](https://github.com/rhsCZ/LayoutKeeper/pulls) directly.

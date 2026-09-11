# Masiam — Imno GUI + DBKKernel driver

`Imno` is a native Windows GUI memory tool (scanner, cheat table, byte patcher,
pointer resolver, and a DBK64 kernel feature tab) backed by the `DBKKernel`
(DBK64) kernel driver — the Cheat Engine-style kernel driver.

- `Imno/` — the user-mode GUI (`Imno.cpp` / `Imno.h`). Talks to the driver over
  the `IOCTL_CE_*` device-control interface (byte-identical to
  `DBKKernel/IOPLDispatcher.h`).
- `DBKKernel/` — the kernel driver, built as `DBK64.sys` (x64) / `DBK32.sys` (x86).
- `third_party/` — vendored, pinned third-party sources compiled directly into
  the GUI (no vcpkg or external package manager required):
  - **Dear ImGui** `v1.90.9`
  - **GLFW** `3.3.9`
  - **Zydis** `v4.1.0` + **Zycore** `v1.5.0`
- `King.sln` — solution that builds `DBKKernel` then `Imno` (Imno depends on
  DBKKernel). `build.bat` builds the Release x64 configuration (default) and
  logs errors to `build_error.log`. The driver's Release config builds
  **without** the `TOBESIGNED` self-check, so the unsigned driver still loads
  under test signing.

## How Imno loads the driver

On startup, `Imno` (run as **Administrator**):

1. Enables `SeDebugPrivilege` (the driver refuses to open without it).
2. Creates/starts the `DBK64` kernel service pointing at `DBK64.sys` next to the
   executable, and writes the four registry values the driver reads at
   `DriverEntry` (`A` = device name, `B` = symlink, `C`/`D` = event names) under
   `HKLM\SYSTEM\CurrentControlSet\Services\DBK64`.
3. Opens `\\.\DBK64` and issues the IOCTLs.

Both projects output to `<repo>\x64\Release\`, so `DBK64.sys` lands next to
`Imno.exe` automatically (no copy step needed).

## Requirements to build

Everything the GUI needs is already in the repo (`third_party/`). You only need:

- **Visual Studio 2019** (v142 toolset) with the **"Desktop development with C++"**
  workload (includes the Windows 10 SDK).
- **Windows Driver Kit (WDK 10)** for the `WindowsKernelModeDriver10.0` platform
  toolset (needed to build the `DBKKernel` driver).

No vcpkg / Conan / NuGet setup is required — `Imno` compiles Dear ImGui, GLFW,
Zydis and Zycore straight from the vendored sources.

## Building

```
build.bat            # Release x64 (default), writes errors to build_error.log
build.bat Debug      # Debug x64 if you need it
```
or open `King.sln` in Visual Studio, set **Release | x64**, and build.

## Running

1. Make sure `DBK64.sys` is next to `Imno.exe` (it is, after a solution build).
2. The driver is **unsigned**, so enable test signing once and reboot:
   ```
   bcdedit /set testsigning on
   ```
3. Run `Imno.exe` as **Administrator**.

## Notes

- The scanner enumerates memory regions **through the driver**
  (`IOCTL_CE_QUERY_VIRTUAL_MEMORY`), so it works on protected processes
  (e.g. `svchost`/PPL) and on guarded 32-bit games. Pointers for 32-bit targets
  are resolved as 4-byte pointers automatically.
- The driver's Release configuration is built **without** `TOBESIGNED` (the
  Dark Byte signature self-check), because `Imno.exe` is not signed with Dark
  Byte's key. If you ever obtain a signing certificate you can re-enable it in
  `DBKKernel/DBKKernel.vcxproj`.

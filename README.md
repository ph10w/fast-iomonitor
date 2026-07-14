# Fast IoMonitor

Fast IoMonitor logs file read and write operations performed by selected processes
on Windows to a UTF-8 CSV file. A privileged service communicates with the
minifilter; the client runs without administrator privileges.

> **Important:** This is a learning and testing MVP for an isolated test VM, not
> a production-ready driver. A bug in kernel code can crash Windows. Distribution
> requires, among other things, an assigned minifilter altitude, production
> signing, load testing, and a security review.

## Architecture

```text
Target processes
  -> IRP_MJ_READ / IRP_MJ_WRITE
  -> IoMonitor.sys (per-handle path cache and post-operation status)
  -> bounded nonpaged queue (1,024 events)
  -> batches of up to 32 events
  -> IoMonitorService.exe (LocalSystem)
  -> secured local named pipe
  -> IoMonitorClient.exe
  -> UTF-8 CSV
```

The driver does not block file operations or modify their data or status. The
target PID and selected operation are checked in the pre-operation callback
before memory is allocated or the file name is resolved. Normalized paths are
cached in the driver per stream handle after the first successful lookup. If the
queue is full or a nonpaged-pool allocation fails, the driver discards the event
and increments `DroppedEvents`. The client reports these losses on `stderr`.

Directories and files:

- `driver/`: minifilter, INF, and WDK project
- `client/`: C++ console logger
- `service/`: privileged Windows service and named-pipe broker
- `shared/`: fixed message format shared by kernel and user mode
- `FastIoMonitor.sln`: Visual Studio solution for x64 Debug/Release
- `build.ps1`: build helper; `-ClientOnly` works without the WDK
- `install-service.ps1`: optionally installs the driver and installs or removes
  `IoMonitorService`

The internal runtime identifiers and binary names continue to begin with
`IoMonitor` for compatibility. This allows the renamed version to update an
existing installation instead of creating a second driver and service alongside
it.

## Prerequisites

- Windows 10 version 2004 (build 19041) or later, or Windows 11 x64
- Visual Studio with **Desktop development with C++**
- for driver builds, the **Windows Driver Kit (WDK)** matching the SDK/Visual
  Studio version, including Visual Studio integration
- administrator privileges to install and load the minifilter and install the
  Windows service; the logging client does not require them
- an appropriately configured test-signing environment for the Release driver

The WDK provides `fltKernel.h`, `FltMgr.lib`, INF validation, and driver signing.
The standard Windows SDK is sufficient only for the user-mode client.

## Build

Run the full build from a regular PowerShell session in the project directory
with this command:

```powershell
pwsh.exe -File .\build.ps1
```

Command and parameter details:

- `pwsh.exe` starts PowerShell 7.
- `-File .\build.ps1` runs the build script from the current directory.
- `-Configuration <Debug|Release>` optionally selects the build configuration.
  The default is `Release`.
- `-ClientOnly` builds only `IoMonitorClient.exe` and therefore does not require
  the WDK.
- `-VisualStudio 2022` uses Visual Studio 2022. Alternatively,
  `-VisualStudio Latest` selects the newest Visual Studio installation found;
  the default is `2022`.

Build outputs are written to `bin\x64\Debug` or `bin\x64\Release`. In addition to
the driver and client, a full build also produces `IoMonitorService.exe` there.
`build.ps1` does not modify services or certificate stores and therefore does not
require administrator privileges.

## Test signing

64-bit Windows does not load unsigned kernel drivers. The exact procedure depends
on the local WDK and certificate configuration. In a disposable test VM, the
Release driver test-signed by Visual Studio can be used with Windows test mode
enabled. Enabling test mode changes the boot security configuration and requires
a restart; it may be rejected while Secure Boot is enabled.

Do not do this on a production system. Test mode is typically enabled from an
administrative console with:

```powershell
bcdedit /set testsigning on
```

After testing, it can be disabled again with:

```powershell
bcdedit /set testsigning off
```

Both changes take effect only after a restart.

The INF uses `SERVICE_DEMAND_START`, so the driver does not start automatically
with Windows. It writes instance values under `Instances` on Windows 10 and
versions of Windows 11 before 24H2, and additionally under
`Parameters\Instances` on Windows 11 version 24H2 and later.

## Install the driver and service

After building, install and start the driver and broker service together from an
administrative PowerShell session:

```powershell
sudo pwsh.exe -File .\install-service.ps1 -LoadDriver
```

Command and parameter details:

- `sudo` elevates only the installation script. The subsequently launched
  `IoMonitorClient.exe` runs without elevation.
- `pwsh.exe -File .\install-service.ps1` performs the driver and service
  installation.
- `-Configuration <Debug|Release>` optionally selects the driver package and
  service binary. The default is `bin\x64\Release`.
- `-LoadDriver` installs the driver package and loads the minifilter. If it is
  already loaded, the script stops the broker, unloads the minifilter, and loads
  it again. `-LoadDriver` includes `-InstallDriver`.
- `-InstallDriver` installs the driver package but does not force an already
  running minifilter to reload. If it is not loaded yet, it is loaded when the
  dependent broker service starts.
- Without `-InstallDriver` or `-LoadDriver`, only the broker is installed or
  updated; the driver must already be registered.
- `-Uninstall` stops and removes the broker, unloads the minifilter, and deletes
  all OEM driver packages unambiguously identified as IoMonitor from the Driver
  Store. The WDK test certificate, which may be shared with other test drivers,
  is retained. This parameter cannot be combined with the driver parameters.

When installing the driver, the script checks the WDK test certificate and, if
necessary, imports it into `LocalMachine\Root` and
`LocalMachine\TrustedPublisher`. This must be done only on a test system. The
thumbprint most recently imported is recorded in `.iomonitor-cert-state.json`.
If the WDK certificate changes, the script removes only the previous certificate
recorded for IoMonitor; other WDK certificates remain untouched.

The script copies the service to `%ProgramFiles%\IoMonitor`, registers it as an
automatically started `LocalSystem` service, and adds the `IoMonitor` minifilter
as a dependency. On restart, the Service Control Manager therefore starts the
minifilter first and the broker afterward.

Standard users are intentionally not granted permission to start or stop the
service. This is not required for logging because the service runs automatically.
Its named pipe accepts only local, authenticated users. Before setting the target
PIDs, the service also verifies that every target process belongs to the same
Windows user as the connected client.

## Record file operations

Monitor all currently running processes with a given executable name without
administrator privileges using this command:

```powershell
.\bin\x64\Release\IoMonitorClient.exe --process-name notepad.exe --operation Read --output .\io_access.csv
```

Command and parameter details:

- `.\bin\x64\Release\IoMonitorClient.exe` starts the previously built Release
  client.
- `--process-name notepad.exe` finds all running processes with this base name.
  If no matching process exists at startup, the client waits for the first match.
  Matching is case-insensitive, and the `.exe` suffix may be omitted.
- `--operation Read` instructs the minifilter to capture only read operations.
  Valid values are `Read`, `Write`, and `All`; matching is case-insensitive. The
  default is `All`.
- `--output .\io_access.csv` selects the output file. The default is
  `io_access.csv` in the current directory.
- `--append` appends to an existing CSV file. Without this parameter, the file is
  created or overwritten at startup.
- `--pid <PID>` alternatively monitors exactly one explicit PID and cannot be
  combined with `--process-name`.
- `--help` or `-h` displays command-line help.

Only one client can be connected to the broker at a time. Each client can have up
to 64 unique PIDs belonging to the same Windows user active at once. `Ctrl+C`
ends both the initial waiting period and active logging. After the first match,
the client also stops when all detected target processes have exited and it was
able to open synchronization handles for them. Exited processes are removed from
the active PID list.

## CSV fields

The CSV contains exactly these columns:

- UTC timestamp with 100 ns resolution from Windows system time
- process path determined by the client
- `Read` or `Write`
- success field (`1` or `0`)
- normalized file path. Known NT device prefixes are converted by the client to
  drive paths such as `C:\Users\...` using a mapping cached at startup; unknown NT
  paths remain unchanged

## Stop and uninstall

Stop the client first, then run:

```powershell
sudo pwsh.exe -File .\install-service.ps1 -Uninstall
```

The script handles the full procedure automatically: it stops and deletes the
broker, unloads the minifilter, identifies the IoMonitor OEM package by multiple
project-specific INF attributes, and removes it with
`pnputil /delete-driver ... /uninstall`. Other drivers in the same
`ActivityMonitor` class are not selected based on the class alone.

The WDK test certificate is intentionally not removed automatically because the
same certificate may also sign other locally built test drivers.

## Known MVP limitations

- If necessary, the client waits for the first process with the specified
  executable name and then includes all matching PIDs found at that time. Matching
  processes started later are not added automatically, nor are child processes
  with a different name.
- If a PID is reused, the wrong process could briefly be captured when no valid
  process handle is available. The broker rechecks the user and PID but cannot
  completely rule out reuse after that check either.
- Paging I/O does not always have a requesting thread. In this case Windows
  reports PID 0; such events cannot be reliably associated with the target PID
  and are not logged.
- Memory-mapped I/O and lazy-writer activity therefore do not necessarily
  correspond to a direct logical access by the target process.
- Name resolution can fail in certain I/O contexts. The minifilter discards such
  events before they reach the CSV.
- Protocol paths are limited to 511 UTF-16 characters and are truncated with a
  marker when necessary.
- The path cache is invalidated when a rename occurs through the same stream
  handle. If a concurrent rename uses a different handle, an already cached path
  may remain stale until the original handle is closed.
- A crash or slow CSV disk can lose events; the target process is intentionally
  never subjected to backpressure.
- The INF altitude `385201` is a test value only. Before distribution, Microsoft
  must assign an altitude appropriate for the load-order group.

## Next steps toward a robust release

- additionally validate process identity using creation time or the kernel
  process object
- add configurable queue and batch sizes and runtime metrics
- add multiple concurrent clients, log rotation, and robust shutdown semantics
- run Driver Verifier, Static Driver Verifier, CodeQL, and load tests in a VM
- plan for a production certificate, assigned altitude, and HLK/compatibility
  testing

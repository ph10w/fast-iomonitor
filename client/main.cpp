#define NOMINMAX
#include <windows.h>
#include <tlhelp32.h>

#include <algorithm>
#include <cwchar>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

#include "../shared/io_monitor_protocol.h"

namespace {

volatile LONG gStopRequested = FALSE;
constexpr DWORD PROCESS_DISCOVERY_INTERVAL_MS = 500;

struct TargetProcess {
    ULONG ProcessId;
    HANDLE Handle;
};

struct DevicePathMapping {
    std::wstring DevicePrefix;
    std::wstring DosPrefix;
};

enum class OperationFilter : ULONG {
    Read = IO_MONITOR_OPERATION_READ,
    Write = IO_MONITOR_OPERATION_WRITE,
    All = IO_MONITOR_OPERATION_ALL
};

BOOL WINAPI ConsoleControlHandler(DWORD controlType)
{
    switch (controlType) {
    case CTRL_C_EVENT:
    case CTRL_BREAK_EVENT:
    case CTRL_CLOSE_EVENT:
    case CTRL_LOGOFF_EVENT:
    case CTRL_SHUTDOWN_EVENT:
        InterlockedExchange(&gStopRequested, TRUE);
        return TRUE;
    default:
        return FALSE;
    }
}

std::string WideToUtf8(const WCHAR* value, std::size_t length)
{
    if (value == nullptr || length == 0) {
        return {};
    }

    const int required = WideCharToMultiByte(
        CP_UTF8,
        0,
        value,
        static_cast<int>(length),
        nullptr,
        0,
        nullptr,
        nullptr);
    if (required <= 0) {
        return "<utf16-conversion-failed>";
    }

    std::string result(static_cast<std::size_t>(required), '\0');
    WideCharToMultiByte(
        CP_UTF8,
        0,
        value,
        static_cast<int>(length),
        result.data(),
        required,
        nullptr,
        nullptr);
    return result;
}

std::string WideToUtf8(const std::wstring& value)
{
    return WideToUtf8(value.data(), value.size());
}

std::string CsvEscape(const std::string& value)
{
    if (value.find_first_of(",\"\r\n") == std::string::npos) {
        return value;
    }

    std::string escaped;
    escaped.reserve(value.size() + 2);
    escaped.push_back('"');
    for (const char character : value) {
        if (character == '"') {
            escaped.push_back('"');
        }
        escaped.push_back(character);
    }
    escaped.push_back('"');
    return escaped;
}

std::string FormatTimestamp(LONGLONG timestamp100ns)
{
    ULARGE_INTEGER value{};
    FILETIME fileTime{};
    SYSTEMTIME systemTime{};
    std::ostringstream output;

    value.QuadPart = static_cast<ULONGLONG>(timestamp100ns);
    fileTime.dwLowDateTime = value.LowPart;
    fileTime.dwHighDateTime = value.HighPart;

    if (!FileTimeToSystemTime(&fileTime, &systemTime)) {
        output << timestamp100ns;
        return output.str();
    }

    const auto fractionalTicks = static_cast<unsigned long>(value.QuadPart % 10000000ULL);
    output << std::setfill('0')
           << std::setw(4) << systemTime.wYear << '-'
           << std::setw(2) << systemTime.wMonth << '-'
           << std::setw(2) << systemTime.wDay << 'T'
           << std::setw(2) << systemTime.wHour << ':'
           << std::setw(2) << systemTime.wMinute << ':'
           << std::setw(2) << systemTime.wSecond << '.'
           << std::setw(7) << fractionalTicks << 'Z';
    return output.str();
}

std::string FormatStatus(ULONG status)
{
    std::ostringstream output;
    output << "0x" << std::hex << std::uppercase << std::setfill('0')
           << std::setw(8) << status;
    return output.str();
}

std::string FormatHresult(HRESULT result)
{
    return FormatStatus(static_cast<ULONG>(result));
}

std::wstring QueryProcessImage(HANDLE process)
{
    std::wstring image(32768, L'\0');
    DWORD length = static_cast<DWORD>(image.size());
    if (!QueryFullProcessImageNameW(process, 0, image.data(), &length)) {
        return {};
    }
    image.resize(length);
    return image;
}

bool EqualsIgnoreCase(const std::wstring& left, const std::wstring& right)
{
    return CompareStringOrdinal(
               left.c_str(),
               static_cast<int>(left.size()),
               right.c_str(),
               static_cast<int>(right.size()),
               TRUE) == CSTR_EQUAL;
}

bool StartsWithIgnoreCase(
    const std::wstring& value,
    const std::wstring& prefix)
{
    if (value.size() < prefix.size()) {
        return false;
    }
    return CompareStringOrdinal(
               value.c_str(),
               static_cast<int>(prefix.size()),
               prefix.c_str(),
               static_cast<int>(prefix.size()),
               TRUE) == CSTR_EQUAL;
}

std::vector<DevicePathMapping> BuildDevicePathMappings()
{
    std::vector<DevicePathMapping> mappings;
    const DWORD logicalDrives = GetLogicalDrives();
    std::vector<WCHAR> deviceTargets(32768, L'\0');

    for (unsigned int driveIndex = 0; driveIndex < 26; ++driveIndex) {
        if ((logicalDrives & (1UL << driveIndex)) == 0) {
            continue;
        }

        const WCHAR driveName[] = {
            static_cast<WCHAR>(L'A' + driveIndex),
            L':',
            L'\0'
        };
        std::fill(deviceTargets.begin(), deviceTargets.end(), L'\0');
        if (QueryDosDeviceW(
                driveName,
                deviceTargets.data(),
                static_cast<DWORD>(deviceTargets.size())) == 0) {
            continue;
        }

        const WCHAR* deviceTarget = deviceTargets.data();
        while (*deviceTarget != L'\0') {
            const std::wstring devicePrefix(deviceTarget);
            if (!devicePrefix.empty()) {
                mappings.push_back({devicePrefix, driveName});
            }
            deviceTarget += std::wcslen(deviceTarget) + 1;
        }
    }

    WCHAR windowsDirectory[MAX_PATH]{};
    const UINT windowsDirectoryLength =
        GetWindowsDirectoryW(windowsDirectory, MAX_PATH);
    if (windowsDirectoryLength != 0 && windowsDirectoryLength < MAX_PATH) {
        mappings.push_back({L"\\SystemRoot", windowsDirectory});
    }

    std::sort(
        mappings.begin(),
        mappings.end(),
        [](const DevicePathMapping& left, const DevicePathMapping& right) {
            return left.DevicePrefix.size() > right.DevicePrefix.size();
        });
    return mappings;
}

std::wstring ResolveDevicePath(
    const std::wstring& path,
    const std::vector<DevicePathMapping>& mappings)
{
    for (const DevicePathMapping& mapping : mappings) {
        if (StartsWithIgnoreCase(path, mapping.DevicePrefix) &&
            (path.size() == mapping.DevicePrefix.size() ||
             path[mapping.DevicePrefix.size()] == L'\\')) {
            return mapping.DosPrefix + path.substr(mapping.DevicePrefix.size());
        }
    }

    const std::wstring dosDevicesPrefix = L"\\??\\";
    if (StartsWithIgnoreCase(path, dosDevicesPrefix)) {
        return path.substr(dosDevicesPrefix.size());
    }

    const std::wstring mupPrefix = L"\\Device\\Mup";
    if (StartsWithIgnoreCase(path, mupPrefix) &&
        path.size() > mupPrefix.size() &&
        path[mupPrefix.size()] == L'\\') {
        return L"\\" + path.substr(mupPrefix.size());
    }

    return path;
}

std::wstring NormalizeProcessName(const std::wstring& value)
{
    std::wstring processName = std::filesystem::path(value).filename().wstring();
    if (processName.empty()) {
        return {};
    }

    if (processName.size() < 4 ||
        !EqualsIgnoreCase(processName.substr(processName.size() - 4), L".exe")) {
        processName += L".exe";
    }
    return processName;
}

bool FindProcessIdsByName(
    const std::wstring& processName,
    std::vector<ULONG>& processIds,
    DWORD& error)
{
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) {
        error = GetLastError();
        return false;
    }

    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    if (Process32FirstW(snapshot, &entry)) {
        do {
            if (entry.th32ProcessID != 0 &&
                entry.th32ProcessID <= MAXLONG &&
                EqualsIgnoreCase(entry.szExeFile, processName)) {
                processIds.push_back(entry.th32ProcessID);
            }
        } while (Process32NextW(snapshot, &entry));

        error = GetLastError();
        if (error == ERROR_NO_MORE_FILES) {
            error = ERROR_SUCCESS;
        }
    } else {
        error = GetLastError();
        if (error == ERROR_NO_MORE_FILES) {
            error = ERROR_SUCCESS;
        }
    }

    CloseHandle(snapshot);
    if (error != ERROR_SUCCESS) {
        return false;
    }

    std::sort(processIds.begin(), processIds.end());
    processIds.erase(std::unique(processIds.begin(), processIds.end()), processIds.end());
    return true;
}

std::vector<ULONG> GetTargetProcessIds(const std::vector<TargetProcess>& targets)
{
    std::vector<ULONG> processIds;
    processIds.reserve(targets.size());
    for (const TargetProcess& target : targets) {
        processIds.push_back(target.ProcessId);
    }
    return processIds;
}

std::wstring FormatProcessIds(const std::vector<ULONG>& processIds)
{
    std::wostringstream output;
    for (std::size_t index = 0; index < processIds.size(); ++index) {
        if (index != 0) {
            output << L", ";
        }
        output << processIds[index];
    }
    return output.str();
}

void CloseTargets(std::vector<TargetProcess>& targets)
{
    for (TargetProcess& target : targets) {
        if (target.Handle != nullptr) {
            CloseHandle(target.Handle);
            target.Handle = nullptr;
        }
    }
}

bool EnsureBrokerServiceRunning(HRESULT& result)
{
    constexpr DWORD serviceStartTimeoutMilliseconds = 15000;

    SC_HANDLE manager = OpenSCManagerW(
        nullptr,
        nullptr,
        SC_MANAGER_CONNECT);
    if (manager == nullptr) {
        result = HRESULT_FROM_WIN32(GetLastError());
        return false;
    }

    SC_HANDLE service = OpenServiceW(
        manager,
        IO_MONITOR_SERVICE_NAME,
        SERVICE_QUERY_STATUS | SERVICE_START);
    if (service == nullptr) {
        result = HRESULT_FROM_WIN32(GetLastError());
        CloseServiceHandle(manager);
        return false;
    }

    const ULONGLONG startTick = GetTickCount64();
    bool startObserved = false;
    bool succeeded = false;

    for (;;) {
        SERVICE_STATUS_PROCESS status{};
        DWORD bytesNeeded = 0;
        if (!QueryServiceStatusEx(
                service,
                SC_STATUS_PROCESS_INFO,
                reinterpret_cast<LPBYTE>(&status),
                sizeof(status),
                &bytesNeeded)) {
            result = HRESULT_FROM_WIN32(GetLastError());
            break;
        }

        if (status.dwCurrentState == SERVICE_RUNNING) {
            result = S_OK;
            succeeded = true;
            break;
        }

        if (status.dwCurrentState == SERVICE_STOPPED) {
            if (startObserved) {
                DWORD error = status.dwWin32ExitCode;
                if (error == ERROR_SERVICE_SPECIFIC_ERROR &&
                    status.dwServiceSpecificExitCode != ERROR_SUCCESS) {
                    error = status.dwServiceSpecificExitCode;
                }
                if (error == ERROR_SUCCESS) {
                    error = ERROR_SERVICE_NOT_ACTIVE;
                }
                result = HRESULT_FROM_WIN32(error);
                break;
            }

            if (!StartServiceW(service, 0, nullptr)) {
                const DWORD error = GetLastError();
                if (error != ERROR_SERVICE_ALREADY_RUNNING) {
                    result = HRESULT_FROM_WIN32(error);
                    break;
                }
            }
            startObserved = true;
        } else if (status.dwCurrentState == SERVICE_START_PENDING) {
            startObserved = true;
        } else if (status.dwCurrentState != SERVICE_STOP_PENDING) {
            result = HRESULT_FROM_WIN32(ERROR_SERVICE_CANNOT_ACCEPT_CTRL);
            break;
        }

        if (GetTickCount64() - startTick >= serviceStartTimeoutMilliseconds) {
            result = HRESULT_FROM_WIN32(ERROR_SERVICE_REQUEST_TIMEOUT);
            break;
        }

        const DWORD waitMilliseconds = std::clamp<DWORD>(
            status.dwWaitHint / 10,
            50,
            500);
        Sleep(waitMilliseconds);
    }

    CloseServiceHandle(service);
    CloseServiceHandle(manager);
    return succeeded;
}

bool StopBrokerService(HRESULT& result)
{
    constexpr DWORD serviceStopTimeoutMilliseconds = 15000;

    SC_HANDLE manager = OpenSCManagerW(
        nullptr,
        nullptr,
        SC_MANAGER_CONNECT);
    if (manager == nullptr) {
        result = HRESULT_FROM_WIN32(GetLastError());
        return false;
    }

    SC_HANDLE service = OpenServiceW(
        manager,
        IO_MONITOR_SERVICE_NAME,
        SERVICE_QUERY_STATUS | SERVICE_STOP);
    if (service == nullptr) {
        result = HRESULT_FROM_WIN32(GetLastError());
        CloseServiceHandle(manager);
        return false;
    }

    SERVICE_STATUS_PROCESS status{};
    DWORD bytesNeeded = 0;
    bool succeeded = false;
    if (!QueryServiceStatusEx(
            service,
            SC_STATUS_PROCESS_INFO,
            reinterpret_cast<LPBYTE>(&status),
            sizeof(status),
            &bytesNeeded)) {
        result = HRESULT_FROM_WIN32(GetLastError());
    } else if (status.dwCurrentState == SERVICE_STOPPED) {
        result = S_OK;
        succeeded = true;
    } else {
        SERVICE_STATUS controlStatus{};
        if (!ControlService(service, SERVICE_CONTROL_STOP, &controlStatus)) {
            const DWORD error = GetLastError();
            if (error != ERROR_SERVICE_NOT_ACTIVE) {
                result = HRESULT_FROM_WIN32(error);
            } else {
                result = S_OK;
                succeeded = true;
            }
        } else {
            const ULONGLONG startTick = GetTickCount64();
            for (;;) {
                if (!QueryServiceStatusEx(
                        service,
                        SC_STATUS_PROCESS_INFO,
                        reinterpret_cast<LPBYTE>(&status),
                        sizeof(status),
                        &bytesNeeded)) {
                    result = HRESULT_FROM_WIN32(GetLastError());
                    break;
                }
                if (status.dwCurrentState == SERVICE_STOPPED) {
                    result = S_OK;
                    succeeded = true;
                    break;
                }
                if (GetTickCount64() - startTick >= serviceStopTimeoutMilliseconds) {
                    result = HRESULT_FROM_WIN32(ERROR_SERVICE_REQUEST_TIMEOUT);
                    break;
                }

                const DWORD waitMilliseconds = std::clamp<DWORD>(
                    status.dwWaitHint / 10,
                    50,
                    500);
                Sleep(waitMilliseconds);
            }
        }
    }

    CloseServiceHandle(service);
    CloseServiceHandle(manager);
    return succeeded;
}

class BrokerServiceStopGuard {
public:
    BrokerServiceStopGuard() = default;
    BrokerServiceStopGuard(const BrokerServiceStopGuard&) = delete;
    BrokerServiceStopGuard& operator=(const BrokerServiceStopGuard&) = delete;

    ~BrokerServiceStopGuard()
    {
        if (active_) {
            HRESULT ignoredResult = S_OK;
            (void)StopBrokerService(ignoredResult);
        }
    }

    void Dismiss()
    {
        active_ = false;
    }

private:
    bool active_ = true;
};

bool RemoveExitedTargets(std::vector<TargetProcess>& targets)
{
    bool changed = false;
    auto target = targets.begin();
    while (target != targets.end()) {
        if (target->Handle != nullptr &&
            WaitForSingleObject(target->Handle, 0) == WAIT_OBJECT_0) {
            CloseHandle(target->Handle);
            target = targets.erase(target);
            changed = true;
        } else {
            ++target;
        }
    }
    return changed;
}

bool SendCommand(
    HANDLE pipe,
    ULONG commandCode,
    const std::vector<ULONG>& processIds,
    ULONG operationMask,
    ULONG waitMilliseconds,
    IO_MONITOR_RESPONSE& response,
    HRESULT& result)
{
    if (processIds.size() > IO_MONITOR_MAX_TARGET_PROCESSES) {
        result = E_INVALIDARG;
        return false;
    }

    IO_MONITOR_COMMAND command{};
    DWORD bytesTransferred = 0;
    IO_MONITOR_PIPE_REPLY reply{};

    command.Version = IO_MONITOR_PROTOCOL_VERSION;
    command.Command = commandCode;
    command.TargetProcessCount = static_cast<ULONG>(processIds.size());
    command.WaitMilliseconds = waitMilliseconds;
    command.OperationMask = operationMask;
    std::copy(processIds.begin(), processIds.end(), command.ProcessIds);
    if (!WriteFile(
            pipe,
            &command,
            sizeof(command),
            &bytesTransferred,
            nullptr)) {
        result = HRESULT_FROM_WIN32(GetLastError());
        return false;
    }
    if (bytesTransferred != sizeof(command)) {
        result = HRESULT_FROM_WIN32(ERROR_WRITE_FAULT);
        return false;
    }

    bytesTransferred = 0;
    if (!ReadFile(
            pipe,
            &reply,
            sizeof(reply),
            &bytesTransferred,
            nullptr)) {
        result = HRESULT_FROM_WIN32(GetLastError());
        return false;
    }
    if (bytesTransferred != sizeof(reply) ||
        reply.Version != IO_MONITOR_PROTOCOL_VERSION) {
        result = HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
        return false;
    }
    if (reply.Win32Error != ERROR_SUCCESS) {
        result = HRESULT_FROM_WIN32(reply.Win32Error);
        return false;
    }

    response = reply.Response;
    if (response.Version != IO_MONITOR_PROTOCOL_VERSION ||
        response.Command != commandCode ||
        response.EventCount > IO_MONITOR_MAX_EVENTS_PER_BATCH) {
        result = HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
        return false;
    }
    return true;
}

void WriteCsvRow(
    std::ofstream& output,
    const IO_MONITOR_EVENT& eventRecord,
    const std::unordered_map<ULONG, std::string>& processImages,
    const std::string& fallbackProcessImage,
    const std::vector<DevicePathMapping>& devicePathMappings)
{
    const std::size_t pathCharacters = std::min<std::size_t>(
        eventRecord.PathLengthBytes / sizeof(WCHAR),
        IO_MONITOR_MAX_PATH_CHARS - 1);
    const std::wstring ntPath(eventRecord.Path, pathCharacters);
    const std::string path = WideToUtf8(
        ResolveDevicePath(ntPath, devicePathMappings));
    const char* operation =
        eventRecord.Operation == IO_MONITOR_OPERATION_READ ? "Read" : "Write";
    const bool succeeded = static_cast<LONG>(eventRecord.Status) >= 0;
    const auto processImage = processImages.find(eventRecord.ProcessId);
    const std::string& image = processImage != processImages.end()
        ? processImage->second
        : fallbackProcessImage;

    output << FormatTimestamp(eventRecord.Timestamp100ns) << ','
           << CsvEscape(image) << ','
           << operation << ','
           << (succeeded ? 1 : 0) << ','
           << CsvEscape(path) << '\n';
}

void PrintUsage()
{
    std::wcerr
        << L"Usage: IoMonitorClient.exe --process-name <name.exe> "
           L"[--operation <Read|Write|All>] [--output <file.csv>] [--append]\n"
        << L"       IoMonitorClient.exe --pid <PID> "
           L"[--operation <Read|Write|All>] [--output <file.csv>] [--append]\n";
}

} // namespace

int wmain(int argc, wchar_t* argv[])
{
    ULONG explicitProcessId = 0;
    std::wstring requestedProcessName;
    std::filesystem::path outputPath = L"io_access.csv";
    bool append = false;
    OperationFilter operationFilter = OperationFilter::All;

    for (int index = 1; index < argc; ++index) {
        const std::wstring argument = argv[index];
        if (argument == L"--process-name" && index + 1 < argc) {
            requestedProcessName = argv[++index];
        } else if (argument == L"--pid" && index + 1 < argc) {
            wchar_t* end = nullptr;
            const unsigned long parsed = wcstoul(argv[++index], &end, 10);
            if (end == argv[index] || *end != L'\0' || parsed == 0 || parsed > MAXLONG) {
                std::wcerr << L"Invalid PID.\n";
                return 2;
            }
            explicitProcessId = static_cast<ULONG>(parsed);
        } else if (argument == L"--output" && index + 1 < argc) {
            outputPath = argv[++index];
        } else if (argument == L"--operation" && index + 1 < argc) {
            const std::wstring value = argv[++index];
            if (EqualsIgnoreCase(value, L"Read")) {
                operationFilter = OperationFilter::Read;
            } else if (EqualsIgnoreCase(value, L"Write")) {
                operationFilter = OperationFilter::Write;
            } else if (EqualsIgnoreCase(value, L"All")) {
                operationFilter = OperationFilter::All;
            } else {
                std::wcerr << L"Invalid operation. Use Read, Write, or All.\n";
                return 2;
            }
        } else if (argument == L"--append") {
            append = true;
        } else if (argument == L"--help" || argument == L"-h") {
            PrintUsage();
            return 0;
        } else {
            std::wcerr << L"Unknown or incomplete argument: " << argument << L'\n';
            PrintUsage();
            return 2;
        }
    }

    if ((explicitProcessId == 0) == requestedProcessName.empty()) {
        std::wcerr << L"Specify exactly one of --process-name or --pid.\n";
        PrintUsage();
        return 2;
    }

    const std::wstring processName = requestedProcessName.empty()
        ? std::wstring{}
        : NormalizeProcessName(requestedProcessName);
    if (!requestedProcessName.empty() && processName.empty()) {
        std::wcerr << L"Invalid process name.\n";
        return 2;
    }

    std::vector<ULONG> processIds;
    if (explicitProcessId != 0) {
        processIds.push_back(explicitProcessId);
    } else {
        DWORD enumerationError = ERROR_SUCCESS;
        if (!FindProcessIdsByName(processName, processIds, enumerationError)) {
            std::wcerr << L"Cannot enumerate processes. Win32 error "
                       << enumerationError << L".\n";
            return 3;
        }
        if (processIds.empty()) {
            SetConsoleCtrlHandler(ConsoleControlHandler, TRUE);
            std::wcout << L"Waiting for " << processName
                       << L" to start. Press Ctrl+C to stop.\n"
                       << std::flush;

            while (InterlockedCompareExchange(
                       &gStopRequested,
                       FALSE,
                       FALSE) == FALSE &&
                   processIds.empty()) {
                Sleep(PROCESS_DISCOVERY_INTERVAL_MS);
                if (InterlockedCompareExchange(
                        &gStopRequested,
                        FALSE,
                        FALSE) != FALSE) {
                    break;
                }

                enumerationError = ERROR_SUCCESS;
                if (!FindProcessIdsByName(
                        processName,
                        processIds,
                        enumerationError)) {
                    SetConsoleCtrlHandler(ConsoleControlHandler, FALSE);
                    std::wcerr << L"Cannot enumerate processes. Win32 error "
                               << enumerationError << L".\n";
                    return 3;
                }
            }

            SetConsoleCtrlHandler(ConsoleControlHandler, FALSE);
            if (InterlockedCompareExchange(
                    &gStopRequested,
                    FALSE,
                    FALSE) != FALSE) {
                std::wcout << L"Stopped while waiting for " << processName << L".\n";
                return 0;
            }
        }
    }

    if (processIds.size() > IO_MONITOR_MAX_TARGET_PROCESSES) {
        std::wcerr << L"Found " << processIds.size() << L" matching processes, but the maximum is "
                   << IO_MONITOR_MAX_TARGET_PROCESSES << L".\n";
        return 3;
    }

    HRESULT serviceResult = S_OK;
    if (!EnsureBrokerServiceRunning(serviceResult)) {
        std::cerr << "Cannot start "
                  << WideToUtf8(
                         IO_MONITOR_SERVICE_NAME,
                         std::wcslen(IO_MONITOR_SERVICE_NAME))
                  << ". HRESULT " << FormatHresult(serviceResult)
                  << ". Verify that the service is installed and that this user has start permission.\n";
        return 4;
    }
    BrokerServiceStopGuard serviceStopGuard;

    std::vector<TargetProcess> targets;
    std::unordered_map<ULONG, std::string> processImages;
    const std::vector<DevicePathMapping> devicePathMappings =
        BuildDevicePathMappings();
    targets.reserve(processIds.size());
    processImages.reserve(processIds.size());
    const std::string fallbackProcessImage = WideToUtf8(processName);

    for (const ULONG processId : processIds) {
        HANDLE process = OpenProcess(
            SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION,
            FALSE,
            processId);
        if (process == nullptr) {
            std::wcerr << L"Warning: PID " << processId
                       << L" could not be opened; its exit and PID reuse cannot be detected. "
                       << L"Win32 error " << GetLastError() << L".\n";
        }

        std::wstring image;
        if (process != nullptr) {
            image = QueryProcessImage(process);
        }
        processImages.emplace(
            processId,
            image.empty() ? fallbackProcessImage : WideToUtf8(image));
        targets.push_back({processId, process});
    }

    bool writeHeader = !append;
    if (append) {
        std::error_code error;
        writeHeader = !std::filesystem::exists(outputPath, error) ||
                      std::filesystem::file_size(outputPath, error) == 0;
    }

    std::ios::openmode mode = std::ios::binary | std::ios::out;
    mode |= append ? std::ios::app : std::ios::trunc;
    std::ofstream csv(outputPath, mode);
    if (!csv) {
        std::wcerr << L"Cannot open CSV output: " << outputPath.c_str() << L'\n';
        CloseTargets(targets);
        return 3;
    }

    if (writeHeader) {
        csv << "timestamp_utc,process_image,operation,success,path\n";
    }

    if (!WaitNamedPipeW(IO_MONITOR_PIPE_NAME, 3000) &&
        GetLastError() != ERROR_SEM_TIMEOUT) {
        const HRESULT result = HRESULT_FROM_WIN32(GetLastError());
        std::cerr << "Cannot find IoMonitorService named pipe. HRESULT "
                  << FormatHresult(result)
                  << ". Verify that IoMonitorService is installed and running.\n";
        CloseTargets(targets);
        return 4;
    }

    HANDLE port = CreateFileW(
        IO_MONITOR_PIPE_NAME,
        GENERIC_READ | GENERIC_WRITE,
        0,
        nullptr,
        OPEN_EXISTING,
        0,
        nullptr);
    if (port == INVALID_HANDLE_VALUE) {
        const HRESULT result = HRESULT_FROM_WIN32(GetLastError());
        std::cerr << "Cannot connect to IoMonitorService. HRESULT "
                  << FormatHresult(result)
                  << ". Verify that the service is running and no other client is connected.\n";
        CloseTargets(targets);
        return 4;
    }

    DWORD pipeMode = PIPE_READMODE_MESSAGE;
    if (!SetNamedPipeHandleState(port, &pipeMode, nullptr, nullptr)) {
        const HRESULT result = HRESULT_FROM_WIN32(GetLastError());
        std::cerr << "Cannot configure IoMonitorService pipe. HRESULT "
                  << FormatHresult(result) << ".\n";
        CloseHandle(port);
        CloseTargets(targets);
        return 4;
    }

    HRESULT result = S_OK;
    IO_MONITOR_RESPONSE response{};
    if (!SendCommand(
            port,
            IO_MONITOR_COMMAND_SET_TARGETS,
            processIds,
            static_cast<ULONG>(operationFilter),
            0,
            response,
            result)) {
        std::cerr << "Could not set target process list. HRESULT "
                  << FormatHresult(result) << ".\n";
        CloseHandle(port);
        CloseTargets(targets);
        return 5;
    }

    SetConsoleCtrlHandler(ConsoleControlHandler, TRUE);
    ULONGLONG lastDroppedEvents = 0;
    unsigned long rowsSinceFlush = 0;
    bool allTargetsExited = false;
    bool shutdownSucceeded = true;

    const auto writeResponseEvents = [&](const IO_MONITOR_RESPONSE& eventResponse) {
        if (eventResponse.DroppedEvents != lastDroppedEvents) {
            std::cerr << "Warning: " << eventResponse.DroppedEvents
                      << " event(s) dropped because the kernel queue was full or allocation failed.\n";
            lastDroppedEvents = eventResponse.DroppedEvents;
        }

        for (ULONG eventIndex = 0;
             eventIndex < eventResponse.EventCount;
             ++eventIndex) {
            WriteCsvRow(
                csv,
                eventResponse.Events[eventIndex],
                processImages,
                fallbackProcessImage,
                devicePathMappings);
            if (++rowsSinceFlush >= 100) {
                csv.flush();
                rowsSinceFlush = 0;
            }
        }
    };

    if (!processName.empty()) {
        std::wcout << L"Monitoring " << processName << L"; PIDs: "
                   << FormatProcessIds(processIds) << L". Press Ctrl+C to stop.\n";
    } else {
        std::wcout << L"Monitoring PID " << explicitProcessId
                   << L". Press Ctrl+C to stop.\n";
    }

    while (InterlockedCompareExchange(&gStopRequested, FALSE, FALSE) == FALSE) {
        if (RemoveExitedTargets(targets)) {
            processIds = GetTargetProcessIds(targets);
            if (processIds.empty()) {
                allTargetsExited = true;
                break;
            }
            if (!SendCommand(
                    port,
                    IO_MONITOR_COMMAND_SET_TARGETS,
                    processIds,
                    static_cast<ULONG>(operationFilter),
                    0,
                    response,
                    result)) {
                std::cerr << "Could not update target process list. HRESULT "
                          << FormatHresult(result) << ".\n";
                break;
            }
            std::wcout << L"Updated target PIDs: " << FormatProcessIds(processIds) << L".\n";
        }

        if (!SendCommand(
                port,
                IO_MONITOR_COMMAND_GET_EVENTS,
                {},
                0,
                500,
                response,
                result)) {
            std::cerr << "Communication with minifilter failed. HRESULT "
                      << FormatHresult(result) << ".\n";
            break;
        }

        writeResponseEvents(response);
    }

    if (!SendCommand(
            port,
            IO_MONITOR_COMMAND_STOP_CAPTURE,
            {},
            0,
            0,
            response,
            result)) {
        std::cerr << "Could not stop event capture before draining the queue. HRESULT "
                  << FormatHresult(result) << ".\n";
        shutdownSucceeded = false;
    } else {
        while (response.QueueDepth != 0) {
            if (!SendCommand(
                    port,
                    IO_MONITOR_COMMAND_GET_EVENTS,
                    {},
                    0,
                    0,
                    response,
                    result)) {
                std::cerr << "Could not drain the remaining event queue. HRESULT "
                          << FormatHresult(result) << ".\n";
                shutdownSucceeded = false;
                break;
            }
            writeResponseEvents(response);
        }
    }

    IO_MONITOR_RESPONSE ignoredResponse{};
    HRESULT ignoredResult = S_OK;
    (void)SendCommand(
        port,
        IO_MONITOR_COMMAND_STOP,
        {},
        0,
        0,
        ignoredResponse,
        ignoredResult);

    csv.flush();
    if (!csv) {
        std::cerr << "Could not finish writing the CSV output.\n";
        shutdownSucceeded = false;
    }
    CloseHandle(port);
    CloseTargets(targets);
    SetConsoleCtrlHandler(ConsoleControlHandler, FALSE);

    HRESULT serviceStopResult = S_OK;
    if (!StopBrokerService(serviceStopResult)) {
        std::cerr << "Could not stop IoMonitorService. HRESULT "
                  << FormatHresult(serviceStopResult) << ".\n";
        shutdownSucceeded = false;
    }
    serviceStopGuard.Dismiss();

    if (allTargetsExited) {
        std::wcout << L"All target processes exited. CSV written to "
                   << outputPath.c_str() << L".\n";
    } else {
        std::wcout << L"Stopped. CSV written to " << outputPath.c_str() << L".\n";
    }
    return shutdownSucceeded ? 0 : 6;
}

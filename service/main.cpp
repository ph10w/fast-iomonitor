#define NOMINMAX
#include <windows.h>
#include <fltuser.h>
#include <sddl.h>

#include <algorithm>
#include <iostream>
#include <vector>

#include "../shared/io_monitor_protocol.h"

namespace {

constexpr WCHAR kPipeSecurity[] =
    L"D:P(A;;GA;;;SY)(A;;GA;;;BA)(A;;GRGW;;;AU)";
static_assert(
    sizeof(IO_MONITOR_PIPE_REPLY) <= 64 * 1024,
    "The batched reply must fit into a single named-pipe message.");

SERVICE_STATUS_HANDLE gStatusHandle = nullptr;
SERVICE_STATUS gStatus{};
HANDLE gStopEvent = nullptr;
HANDLE gServiceThread = nullptr;

void ReportServiceStatus(DWORD state, DWORD win32Error, DWORD waitHint)
{
    static DWORD checkpoint = 1;

    gStatus.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    gStatus.dwCurrentState = state;
    gStatus.dwWin32ExitCode = win32Error;
    gStatus.dwWaitHint = waitHint;
    gStatus.dwControlsAccepted = state == SERVICE_RUNNING
        ? SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN
        : 0;
    gStatus.dwCheckPoint = state == SERVICE_START_PENDING ||
                            state == SERVICE_STOP_PENDING
        ? checkpoint++
        : 0;

    if (gStatusHandle != nullptr) {
        SetServiceStatus(gStatusHandle, &gStatus);
    }
}

DWORD HresultToWin32(HRESULT result)
{
    if (HRESULT_FACILITY(result) == FACILITY_WIN32) {
        return HRESULT_CODE(result);
    }
    return result == S_OK ? ERROR_SUCCESS : ERROR_GEN_FAILURE;
}

bool QueryTokenUser(HANDLE token, std::vector<BYTE>& buffer, DWORD& error)
{
    DWORD required = 0;
    GetTokenInformation(token, TokenUser, nullptr, 0, &required);
    error = GetLastError();
    if (error != ERROR_INSUFFICIENT_BUFFER || required == 0) {
        return false;
    }

    buffer.resize(required);
    if (!GetTokenInformation(
            token,
            TokenUser,
            buffer.data(),
            required,
            &required)) {
        error = GetLastError();
        return false;
    }
    error = ERROR_SUCCESS;
    return true;
}

DWORD GetPipeClientUser(HANDLE pipe, std::vector<BYTE>& userBuffer)
{
    if (!ImpersonateNamedPipeClient(pipe)) {
        return GetLastError();
    }

    HANDLE token = nullptr;
    DWORD error = ERROR_SUCCESS;
    if (!OpenThreadToken(GetCurrentThread(), TOKEN_QUERY, TRUE, &token)) {
        error = GetLastError();
    }

    if (!RevertToSelf() && error == ERROR_SUCCESS) {
        error = GetLastError();
    }
    if (error != ERROR_SUCCESS) {
        if (token != nullptr) {
            CloseHandle(token);
        }
        return error;
    }

    if (!QueryTokenUser(token, userBuffer, error)) {
        CloseHandle(token);
        return error;
    }
    CloseHandle(token);
    return ERROR_SUCCESS;
}

DWORD ValidateTargetOwners(HANDLE pipe, IO_MONITOR_COMMAND& command)
{
    if (command.TargetProcessCount == 0 ||
        command.TargetProcessCount > IO_MONITOR_MAX_TARGET_PROCESSES ||
        command.OperationMask == 0 ||
        (command.OperationMask & ~IO_MONITOR_OPERATION_ALL) != 0) {
        return ERROR_INVALID_PARAMETER;
    }

    std::vector<BYTE> clientUserBuffer;
    DWORD error = GetPipeClientUser(pipe, clientUserBuffer);
    if (error != ERROR_SUCCESS) {
        return error;
    }
    const auto* clientUser = reinterpret_cast<const TOKEN_USER*>(
        clientUserBuffer.data());

    for (ULONG index = 0; index < command.TargetProcessCount; ++index) {
        const ULONG processId = command.ProcessIds[index];
        if (processId == 0 || processId > MAXLONG) {
            return ERROR_INVALID_PARAMETER;
        }

        HANDLE process = OpenProcess(
            PROCESS_QUERY_LIMITED_INFORMATION,
            FALSE,
            processId);
        if (process == nullptr) {
            return GetLastError();
        }

        HANDLE processToken = nullptr;
        if (!OpenProcessToken(process, TOKEN_QUERY, &processToken)) {
            error = GetLastError();
            CloseHandle(process);
            return error;
        }

        FILETIME creationTime{};
        FILETIME exitTime{};
        FILETIME kernelTime{};
        FILETIME userTime{};
        if (!GetProcessTimes(
                process,
                &creationTime,
                &exitTime,
                &kernelTime,
                &userTime)) {
            error = GetLastError();
            CloseHandle(processToken);
            CloseHandle(process);
            return error;
        }
        ULARGE_INTEGER creationTimeValue{};
        creationTimeValue.LowPart = creationTime.dwLowDateTime;
        creationTimeValue.HighPart = creationTime.dwHighDateTime;
        command.ProcessCreationTimes100ns[index] = creationTimeValue.QuadPart;
        CloseHandle(process);

        std::vector<BYTE> processUserBuffer;
        if (!QueryTokenUser(processToken, processUserBuffer, error)) {
            CloseHandle(processToken);
            return error;
        }
        CloseHandle(processToken);

        const auto* processUser = reinterpret_cast<const TOKEN_USER*>(
            processUserBuffer.data());
        if (!EqualSid(clientUser->User.Sid, processUser->User.Sid)) {
            return ERROR_ACCESS_DENIED;
        }
    }

    return ERROR_SUCCESS;
}

DWORD ValidateCommand(HANDLE pipe, IO_MONITOR_COMMAND& command)
{
    if (command.Version != IO_MONITOR_PROTOCOL_VERSION) {
        return ERROR_REVISION_MISMATCH;
    }

    switch (command.Command) {
    case IO_MONITOR_COMMAND_SET_TARGETS:
        return ValidateTargetOwners(pipe, command);
    case IO_MONITOR_COMMAND_GET_EVENTS:
    case IO_MONITOR_COMMAND_GET_STATUS:
    case IO_MONITOR_COMMAND_STOP:
    case IO_MONITOR_COMMAND_STOP_CAPTURE:
        return command.TargetProcessCount == 0 && command.OperationMask == 0
            ? ERROR_SUCCESS
            : ERROR_INVALID_PARAMETER;
    default:
        return ERROR_INVALID_FUNCTION;
    }
}

DWORD SendDriverCommand(
    HANDLE driverPort,
    const IO_MONITOR_COMMAND& command,
    IO_MONITOR_RESPONSE& response)
{
    DWORD bytesReturned = 0;
    ZeroMemory(&response, sizeof(response));

    const HRESULT result = FilterSendMessage(
        driverPort,
        const_cast<PIO_MONITOR_COMMAND>(&command),
        sizeof(command),
        &response,
        sizeof(response),
        &bytesReturned);
    if (FAILED(result)) {
        return HresultToWin32(result);
    }
    if (bytesReturned != sizeof(response) ||
        response.Version != IO_MONITOR_PROTOCOL_VERSION ||
        response.Command != command.Command ||
        response.EventCount > IO_MONITOR_MAX_EVENTS_PER_BATCH) {
        return ERROR_INVALID_DATA;
    }
    return ERROR_SUCCESS;
}

void StopDriverTargets(HANDLE driverPort)
{
    IO_MONITOR_COMMAND command{};
    IO_MONITOR_RESPONSE response{};
    command.Version = IO_MONITOR_PROTOCOL_VERSION;
    command.Command = IO_MONITOR_COMMAND_STOP;
    (void)SendDriverCommand(driverPort, command, response);
}

enum class ClientSessionResult {
    Disconnected,
    StopService,
    Failed
};

ClientSessionResult HandleClient(HANDLE pipe, HANDLE driverPort)
{
    while (WaitForSingleObject(gStopEvent, 0) != WAIT_OBJECT_0) {
        IO_MONITOR_COMMAND command{};
        DWORD bytesRead = 0;
        if (!ReadFile(
                pipe,
                &command,
                sizeof(command),
                &bytesRead,
                nullptr)) {
            const DWORD error = GetLastError();
            return error == ERROR_BROKEN_PIPE ||
                   error == ERROR_NO_DATA ||
                   error == ERROR_OPERATION_ABORTED
                ? ClientSessionResult::Disconnected
                : ClientSessionResult::Failed;
        }

        IO_MONITOR_PIPE_REPLY reply{};
        reply.Version = IO_MONITOR_PROTOCOL_VERSION;
        if (bytesRead != sizeof(command)) {
            reply.Win32Error = ERROR_INVALID_DATA;
        } else {
            reply.Win32Error = ValidateCommand(pipe, command);
            if (reply.Win32Error == ERROR_SUCCESS) {
                reply.Win32Error = SendDriverCommand(
                    driverPort,
                    command,
                    reply.Response);
            }
        }

        DWORD bytesWritten = 0;
        if (!WriteFile(
                pipe,
                &reply,
                sizeof(reply),
            &bytesWritten,
            nullptr) ||
            bytesWritten != sizeof(reply)) {
            return ClientSessionResult::Failed;
        }

        if (command.Command == IO_MONITOR_COMMAND_STOP &&
            bytesRead == sizeof(command) &&
            reply.Win32Error == ERROR_SUCCESS) {
            return ClientSessionResult::StopService;
        }
    }
    return ClientSessionResult::Disconnected;
}

DWORD RunPipeServer(HANDLE driverPort)
{
    PSECURITY_DESCRIPTOR securityDescriptor = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
            kPipeSecurity,
            SDDL_REVISION_1,
            &securityDescriptor,
            nullptr)) {
        return GetLastError();
    }

    SECURITY_ATTRIBUTES securityAttributes{};
    securityAttributes.nLength = sizeof(securityAttributes);
    securityAttributes.lpSecurityDescriptor = securityDescriptor;

    DWORD result = ERROR_SUCCESS;
    while (WaitForSingleObject(gStopEvent, 0) != WAIT_OBJECT_0) {
        HANDLE pipe = CreateNamedPipeW(
            IO_MONITOR_PIPE_NAME,
            PIPE_ACCESS_DUPLEX,
            PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT |
                PIPE_REJECT_REMOTE_CLIENTS,
            1,
            sizeof(IO_MONITOR_PIPE_REPLY),
            sizeof(IO_MONITOR_COMMAND),
            0,
            &securityAttributes);
        if (pipe == INVALID_HANDLE_VALUE) {
            result = GetLastError();
            break;
        }

        const BOOL connected = ConnectNamedPipe(pipe, nullptr);
        const DWORD connectError = connected ? ERROR_SUCCESS : GetLastError();
        bool stopService = false;
        if (connected || connectError == ERROR_PIPE_CONNECTED) {
            const ClientSessionResult sessionResult =
                HandleClient(pipe, driverPort);
            stopService = sessionResult == ClientSessionResult::StopService;
            StopDriverTargets(driverPort);
            FlushFileBuffers(pipe);
            DisconnectNamedPipe(pipe);
        } else if (connectError != ERROR_OPERATION_ABORTED) {
            result = connectError;
        }

        CloseHandle(pipe);
        if (stopService || connectError == ERROR_OPERATION_ABORTED) {
            break;
        }
    }

    LocalFree(securityDescriptor);
    return result;
}

DWORD WINAPI ServiceControlHandler(
    DWORD control,
    DWORD,
    void*,
    void*)
{
    if (control == SERVICE_CONTROL_STOP || control == SERVICE_CONTROL_SHUTDOWN) {
        ReportServiceStatus(SERVICE_STOP_PENDING, ERROR_SUCCESS, 5000);
        if (gStopEvent != nullptr) {
            SetEvent(gStopEvent);
        }
        if (gServiceThread != nullptr) {
            CancelSynchronousIo(gServiceThread);
        }
        return NO_ERROR;
    }
    if (control == SERVICE_CONTROL_INTERROGATE) {
        ReportServiceStatus(gStatus.dwCurrentState, gStatus.dwWin32ExitCode, 0);
    }
    return NO_ERROR;
}

void WINAPI ServiceMain(DWORD, PWSTR*)
{
    gStatusHandle = RegisterServiceCtrlHandlerExW(
        IO_MONITOR_SERVICE_NAME,
        ServiceControlHandler,
        nullptr);
    if (gStatusHandle == nullptr) {
        return;
    }

    ReportServiceStatus(SERVICE_START_PENDING, ERROR_SUCCESS, 5000);
    gStopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (gStopEvent == nullptr ||
        !DuplicateHandle(
            GetCurrentProcess(),
            GetCurrentThread(),
            GetCurrentProcess(),
            &gServiceThread,
            0,
            FALSE,
            DUPLICATE_SAME_ACCESS)) {
        const DWORD error = GetLastError();
        if (gStopEvent != nullptr) {
            CloseHandle(gStopEvent);
            gStopEvent = nullptr;
        }
        ReportServiceStatus(SERVICE_STOPPED, error, 0);
        return;
    }

    HANDLE driverPort = INVALID_HANDLE_VALUE;
    const HRESULT connectResult = FilterConnectCommunicationPort(
        IO_MONITOR_PORT_NAME,
        0,
        nullptr,
        0,
        nullptr,
        &driverPort);
    if (FAILED(connectResult)) {
        const DWORD error = HresultToWin32(connectResult);
        CloseHandle(gServiceThread);
        CloseHandle(gStopEvent);
        gServiceThread = nullptr;
        gStopEvent = nullptr;
        ReportServiceStatus(SERVICE_STOPPED, error, 0);
        return;
    }

    ReportServiceStatus(SERVICE_RUNNING, ERROR_SUCCESS, 0);
    const DWORD serverError = RunPipeServer(driverPort);

    if (gStatus.dwCurrentState == SERVICE_RUNNING) {
        ReportServiceStatus(SERVICE_STOP_PENDING, ERROR_SUCCESS, 5000);
    }
    StopDriverTargets(driverPort);
    CloseHandle(driverPort);
    CloseHandle(gServiceThread);
    CloseHandle(gStopEvent);
    gServiceThread = nullptr;
    gStopEvent = nullptr;
    ReportServiceStatus(SERVICE_STOPPED, serverError, 0);
}

} // namespace

int wmain()
{
    SERVICE_TABLE_ENTRYW serviceTable[] = {
        {const_cast<PWSTR>(IO_MONITOR_SERVICE_NAME), ServiceMain},
        {nullptr, nullptr}
    };

    if (!StartServiceCtrlDispatcherW(serviceTable)) {
        const DWORD error = GetLastError();
        std::wcerr << L"IoMonitorService must be started by the Windows Service "
                      L"Control Manager. Win32 error "
                   << error << L".\n";
        return static_cast<int>(error);
    }
    return 0;
}

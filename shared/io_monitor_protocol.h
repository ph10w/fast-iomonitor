#pragma once

// Shared wire format for the kernel minifilter and the user-mode logger.
// Both projects are intentionally x64-only in this MVP.

#define IO_MONITOR_PROTOCOL_VERSION 6UL
#define IO_MONITOR_PORT_NAME L"\\IoMonitorPort"
#define IO_MONITOR_SERVICE_NAME L"IoMonitorService"
#define IO_MONITOR_PIPE_NAME L"\\\\.\\pipe\\IoMonitorService"
#define IO_MONITOR_MAX_PATH_CHARS 512UL
#define IO_MONITOR_MAX_TARGET_PROCESSES 64UL
#define IO_MONITOR_MAX_EVENTS_PER_BATCH 32UL

#define IO_MONITOR_COMMAND_SET_TARGETS 1UL
#define IO_MONITOR_COMMAND_GET_EVENTS 2UL
#define IO_MONITOR_COMMAND_GET_STATUS 3UL
#define IO_MONITOR_COMMAND_STOP       4UL
#define IO_MONITOR_COMMAND_STOP_CAPTURE 5UL

// The broker treats STOP as the final command of a client session. It sends the
// reply first, then closes the pipe and exits the service process.

#define IO_MONITOR_OPERATION_READ  1UL
#define IO_MONITOR_OPERATION_WRITE 2UL
#define IO_MONITOR_OPERATION_ALL \
    (IO_MONITOR_OPERATION_READ | IO_MONITOR_OPERATION_WRITE)

#define IO_MONITOR_FLAG_PAGING_IO          0x00000001UL
#define IO_MONITOR_FLAG_NON_CACHED         0x00000002UL
#define IO_MONITOR_FLAG_SYNCHRONOUS_PAGING 0x00000004UL
#define IO_MONITOR_FLAG_IRP_OPERATION      0x00000008UL
#define IO_MONITOR_FLAG_FAST_IO_OPERATION  0x00000010UL
#define IO_MONITOR_FLAG_NAME_TRUNCATED     0x00000020UL
#define IO_MONITOR_FLAG_NAME_UNAVAILABLE   0x00000040UL

#pragma pack(push, 8)

typedef struct _IO_MONITOR_COMMAND {
    ULONG Version;
    ULONG Command;
    ULONG TargetProcessCount;
    ULONG WaitMilliseconds;
    ULONG OperationMask;
    ULONG ProcessIds[IO_MONITOR_MAX_TARGET_PROCESSES];
} IO_MONITOR_COMMAND, *PIO_MONITOR_COMMAND;

typedef struct _IO_MONITOR_EVENT {
    ULONGLONG Sequence;
    LONGLONG Timestamp100ns;
    ULONGLONG RequestedBytes;
    ULONGLONG ActualBytes;
    ULONG ProcessId;
    ULONG ThreadId;
    ULONG Operation;
    ULONG Flags;
    ULONG Status;
    ULONG Generation;
    ULONG PathLengthBytes;
    ULONG Reserved;
    WCHAR Path[IO_MONITOR_MAX_PATH_CHARS];
} IO_MONITOR_EVENT, *PIO_MONITOR_EVENT;

typedef struct _IO_MONITOR_RESPONSE {
    ULONG Version;
    ULONG Command;
    ULONG TargetProcessCount;
    ULONG EventCount;
    ULONG QueueDepth;
    ULONG Reserved;
    ULONGLONG DroppedEvents;
    IO_MONITOR_EVENT Events[IO_MONITOR_MAX_EVENTS_PER_BATCH];
} IO_MONITOR_RESPONSE, *PIO_MONITOR_RESPONSE;

// User-mode reply envelope used between IoMonitorClient and IoMonitorService.
// Win32Error is ERROR_SUCCESS when Response contains a valid driver response.
typedef struct _IO_MONITOR_PIPE_REPLY {
    ULONG Version;
    ULONG Win32Error;
    IO_MONITOR_RESPONSE Response;
} IO_MONITOR_PIPE_REPLY, *PIO_MONITOR_PIPE_REPLY;

#pragma pack(pop)

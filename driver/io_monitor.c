#include <fltKernel.h>
#include "../shared/io_monitor_protocol.h"

#define IO_MONITOR_POOL_TAG 'noMI'
#define IO_MONITOR_NAME_CONTEXT_TAG 'cnMI'
#define IO_MONITOR_QUEUE_CAPACITY 1024UL
#define IO_MONITOR_MAX_WAIT_MS 1000UL
#define IO_MONITOR_PID_BLOOM_WORDS 4UL

DRIVER_INITIALIZE DriverEntry;

static PFLT_FILTER gFilter = NULL;
static PFLT_PORT gServerPort = NULL;
static PFLT_PORT gClientPort = NULL;

static KSPIN_LOCK gQueueLock;
static KEVENT gQueueAvailable;
static PIO_MONITOR_EVENT gQueue[IO_MONITOR_QUEUE_CAPACITY];
static ULONG gQueueHead = 0;
static ULONG gQueueTail = 0;
static ULONG gQueueCount = 0;
static NPAGED_LOOKASIDE_LIST gEventLookaside;

typedef struct _IO_MONITOR_NAME_CONTEXT {
    USHORT PathLengthBytes;
    WCHAR Path[IO_MONITOR_MAX_PATH_CHARS];
} IO_MONITOR_NAME_CONTEXT, *PIO_MONITOR_NAME_CONTEXT;

typedef struct _IO_MONITOR_TARGET_SNAPSHOT {
    EX_RUNDOWN_REF Rundown;
    ULONG ProcessCount;
    ULONG OperationMask;
    ULONG Generation;
    ULONG ProcessIds[IO_MONITOR_MAX_TARGET_PROCESSES];
} IO_MONITOR_TARGET_SNAPSHOT, *PIO_MONITOR_TARGET_SNAPSHOT;

static FAST_MUTEX gTargetUpdateMutex;
static IO_MONITOR_TARGET_SNAPSHOT gTargetSnapshot;
static volatile LONG gTargetSnapshotActive = FALSE;
static BOOLEAN gTargetRundownCompleted = FALSE;
static volatile LONG gGeneration = 0;
static volatile LONG gFastProcessCount = 0;
static volatile LONG gFastSingleProcessId = 0;
static volatile LONG gFastOperationMask = 0;
static volatile LONG64 gTargetPidBloom[IO_MONITOR_PID_BLOOM_WORDS];
static DECLSPEC_CACHEALIGN volatile LONG64 gSequence = 0;
static volatile LONG64 gDroppedEvents = 0;

static VOID IoMonClearQueue(VOID);
static VOID IoMonStopTargets(VOID);

static FLT_PREOP_CALLBACK_STATUS
IoMonPreOperation(
    _Inout_ PFLT_CALLBACK_DATA Data,
    _In_ PCFLT_RELATED_OBJECTS FltObjects,
    _Flt_CompletionContext_Outptr_ PVOID *CompletionContext
    );

static FLT_POSTOP_CALLBACK_STATUS
IoMonPostOperation(
    _Inout_ PFLT_CALLBACK_DATA Data,
    _In_ PCFLT_RELATED_OBJECTS FltObjects,
    _In_opt_ PVOID CompletionContext,
    _In_ FLT_POST_OPERATION_FLAGS Flags
    );

static FLT_PREOP_CALLBACK_STATUS
IoMonPreSetInformation(
    _Inout_ PFLT_CALLBACK_DATA Data,
    _In_ PCFLT_RELATED_OBJECTS FltObjects,
    _Flt_CompletionContext_Outptr_ PVOID *CompletionContext
    );

static VOID
IoMonNameContextCleanup(
    _In_ PFLT_CONTEXT Context,
    _In_ FLT_CONTEXT_TYPE ContextType
    );

static NTSTATUS
IoMonUnload(
    _In_ FLT_FILTER_UNLOAD_FLAGS Flags
    );

static NTSTATUS
IoMonConnect(
    _In_ PFLT_PORT ClientPort,
    _In_opt_ PVOID ServerPortCookie,
    _In_reads_bytes_opt_(SizeOfContext) PVOID ConnectionContext,
    _In_ ULONG SizeOfContext,
    _Outptr_result_maybenull_ PVOID *ConnectionPortCookie
    );

static VOID
IoMonDisconnect(
    _In_opt_ PVOID ConnectionCookie
    );

static NTSTATUS
IoMonMessage(
    _In_opt_ PVOID PortCookie,
    _In_reads_bytes_opt_(InputBufferLength) PVOID InputBuffer,
    _In_ ULONG InputBufferLength,
    _Out_writes_bytes_to_opt_(OutputBufferLength, *ReturnOutputBufferLength) PVOID OutputBuffer,
    _In_ ULONG OutputBufferLength,
    _Out_ PULONG ReturnOutputBufferLength
    );

static const FLT_CONTEXT_REGISTRATION gContexts[] = {
    {
        FLT_STREAMHANDLE_CONTEXT,
        0,
        IoMonNameContextCleanup,
        sizeof(IO_MONITOR_NAME_CONTEXT),
        IO_MONITOR_NAME_CONTEXT_TAG,
        NULL,
        NULL,
        NULL
    },
    { FLT_CONTEXT_END }
};

static const FLT_OPERATION_REGISTRATION gOperations[] = {
    { IRP_MJ_READ, 0, IoMonPreOperation, IoMonPostOperation },
    { IRP_MJ_WRITE, 0, IoMonPreOperation, IoMonPostOperation },
    { IRP_MJ_SET_INFORMATION, 0, IoMonPreSetInformation, NULL },
    { IRP_MJ_OPERATION_END }
};

static const FLT_REGISTRATION gRegistration = {
    sizeof(FLT_REGISTRATION),
    FLT_REGISTRATION_VERSION,
    0,
    gContexts,
    gOperations,
    IoMonUnload,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL
};

static LONG64
IoMonReadLong64(
    _In_ volatile LONG64 *Value
    )
{
    return InterlockedCompareExchange64(Value, 0, 0);
}

static ULONG
IoMonHashProcessId(
    _In_ ULONG ProcessId
    )
{
    ULONG hash = ProcessId;

    hash ^= hash >> 16;
    hash *= 0x7feb352dUL;
    hash ^= hash >> 15;
    hash *= 0x846ca68bUL;
    hash ^= hash >> 16;
    return hash;
}

static BOOLEAN
IoMonMightContainTarget(
    _In_ ULONG ProcessId,
    _In_ ULONG Operation
    )
{
    LONG processCount;
    ULONG hash;
    ULONG wordIndex;
    ULONGLONG bit;

    if (ReadAcquire(&gTargetSnapshotActive) == FALSE ||
        !FlagOn((ULONG)ReadNoFence(&gFastOperationMask), Operation)) {
        return FALSE;
    }

    processCount = ReadNoFence(&gFastProcessCount);
    if (processCount == 1) {
        return (ULONG)ReadNoFence(&gFastSingleProcessId) == ProcessId;
    }
    if (processCount <= 0) {
        return FALSE;
    }

    hash = IoMonHashProcessId(ProcessId);
    wordIndex = (hash >> 6) & (IO_MONITOR_PID_BLOOM_WORDS - 1);
    bit = 1ULL << (hash & 63);
    return FlagOn(
        (ULONGLONG)ReadNoFence64(&gTargetPidBloom[wordIndex]),
        bit) != 0;
}

static PIO_MONITOR_TARGET_SNAPSHOT
IoMonAcquireTargetSnapshot(VOID)
{
    for (;;) {
        if (ReadAcquire(&gTargetSnapshotActive) == FALSE) {
            return NULL;
        }

        if (!ExAcquireRundownProtection(&gTargetSnapshot.Rundown)) {
            continue;
        }

        if (ReadAcquire(&gTargetSnapshotActive) != FALSE) {
            return &gTargetSnapshot;
        }

        ExReleaseRundownProtection(&gTargetSnapshot.Rundown);
    }
}

static BOOLEAN
IoMonSnapshotContainsProcess(
    _In_ const IO_MONITOR_TARGET_SNAPSHOT *Snapshot,
    _In_ ULONG ProcessId
    )
{
    ULONG low;
    ULONG high;

    if (Snapshot->ProcessCount == 0) {
        return FALSE;
    }
    if (Snapshot->ProcessCount == 1) {
        return Snapshot->ProcessIds[0] == ProcessId;
    }

    low = 0;
    high = Snapshot->ProcessCount;
    while (low < high) {
        const ULONG middle = low + ((high - low) / 2);
        const ULONG candidate = Snapshot->ProcessIds[middle];

        if (candidate == ProcessId) {
            return TRUE;
        }
        if (candidate < ProcessId) {
            low = middle + 1;
        } else {
            high = middle;
        }
    }
    return FALSE;
}

static BOOLEAN
IoMonSnapshotMatches(
    _In_ const IO_MONITOR_TARGET_SNAPSHOT *Snapshot,
    _In_ ULONG ProcessId,
    _In_ ULONG Operation,
    _In_ ULONG Generation
    )
{
    return Snapshot->Generation == Generation &&
        FlagOn(Snapshot->OperationMask, Operation) &&
        IoMonSnapshotContainsProcess(Snapshot, ProcessId);
}

static BOOLEAN
IoMonGetTargetGeneration(
    _In_ ULONG ProcessId,
    _In_ ULONG Operation,
    _Out_ PULONG Generation
    )
{
    PIO_MONITOR_TARGET_SNAPSHOT snapshot;
    BOOLEAN isTarget = FALSE;

    snapshot = IoMonAcquireTargetSnapshot();
    if (snapshot != NULL) {
        if (FlagOn(snapshot->OperationMask, Operation) &&
            IoMonSnapshotContainsProcess(snapshot, ProcessId)) {
            *Generation = snapshot->Generation;
            isTarget = TRUE;
        }
        ExReleaseRundownProtection(&snapshot->Rundown);
    }
    return isTarget;
}

static ULONG
IoMonTargetCount(VOID)
{
    PIO_MONITOR_TARGET_SNAPSHOT snapshot;
    ULONG count = 0;

    snapshot = IoMonAcquireTargetSnapshot();
    if (snapshot != NULL) {
        count = snapshot->ProcessCount;
        ExReleaseRundownProtection(&snapshot->Rundown);
    }
    return count;
}

static ULONG
IoMonQueueDepth(VOID)
{
    KIRQL oldIrql;
    ULONG depth;

    KeAcquireSpinLock(&gQueueLock, &oldIrql);
    depth = gQueueCount;
    KeReleaseSpinLock(&gQueueLock, oldIrql);
    return depth;
}

static PIO_MONITOR_EVENT
IoMonPopEvent(VOID)
{
    KIRQL oldIrql;
    PIO_MONITOR_EVENT eventRecord = NULL;

    KeAcquireSpinLock(&gQueueLock, &oldIrql);

    if (gQueueCount != 0) {
        eventRecord = gQueue[gQueueTail];
        gQueue[gQueueTail] = NULL;
        gQueueTail = (gQueueTail + 1) % IO_MONITOR_QUEUE_CAPACITY;
        --gQueueCount;
    }

    if (gQueueCount == 0) {
        KeClearEvent(&gQueueAvailable);
    }

    KeReleaseSpinLock(&gQueueLock, oldIrql);
    return eventRecord;
}

static ULONG
IoMonPopEventBatch(
    _Out_writes_(IO_MONITOR_MAX_EVENTS_PER_BATCH) PIO_MONITOR_EVENT Events
    )
{
    PIO_MONITOR_EVENT queuedEvents[IO_MONITOR_MAX_EVENTS_PER_BATCH];
    KIRQL oldIrql;
    ULONG eventCount = 0;
    ULONG index;

    KeAcquireSpinLock(&gQueueLock, &oldIrql);
    while (eventCount < IO_MONITOR_MAX_EVENTS_PER_BATCH &&
           gQueueCount != 0) {
        queuedEvents[eventCount] = gQueue[gQueueTail];
        gQueue[gQueueTail] = NULL;
        gQueueTail = (gQueueTail + 1) % IO_MONITOR_QUEUE_CAPACITY;
        --gQueueCount;
        ++eventCount;
    }
    if (gQueueCount == 0) {
        KeClearEvent(&gQueueAvailable);
    }
    KeReleaseSpinLock(&gQueueLock, oldIrql);

    for (index = 0; index < eventCount; ++index) {
        RtlCopyMemory(
            &Events[index],
            queuedEvents[index],
            sizeof(IO_MONITOR_EVENT));
        ExFreeToNPagedLookasideList(
            &gEventLookaside,
            queuedEvents[index]);
    }
    return eventCount;
}

static VOID
IoMonEnqueueEvent(
    _In_ PIO_MONITOR_EVENT EventRecord
    )
{
    PIO_MONITOR_TARGET_SNAPSHOT snapshot;
    KIRQL oldIrql;
    BOOLEAN queued = FALSE;
    BOOLEAN shouldCountDrop = FALSE;
    BOOLEAN shouldSignal = FALSE;

    snapshot = IoMonAcquireTargetSnapshot();
    if (snapshot != NULL &&
        IoMonSnapshotMatches(
            snapshot,
            EventRecord->ProcessId,
            EventRecord->Operation,
            EventRecord->Generation)) {
        KeAcquireSpinLock(&gQueueLock, &oldIrql);
        if (gQueueCount < IO_MONITOR_QUEUE_CAPACITY) {
            shouldSignal = gQueueCount == 0;
            gQueue[gQueueHead] = EventRecord;
            gQueueHead = (gQueueHead + 1) % IO_MONITOR_QUEUE_CAPACITY;
            ++gQueueCount;
            queued = TRUE;
        } else {
            shouldCountDrop = TRUE;
        }
        KeReleaseSpinLock(&gQueueLock, oldIrql);
    }

    if (snapshot != NULL) {
        ExReleaseRundownProtection(&snapshot->Rundown);
    }

    if (queued) {
        if (shouldSignal) {
            KeSetEvent(&gQueueAvailable, IO_NO_INCREMENT, FALSE);
        }
        return;
    }

    if (shouldCountDrop) {
        InterlockedIncrement64(&gDroppedEvents);
    }
    ExFreeToNPagedLookasideList(&gEventLookaside, EventRecord);
}

static VOID
IoMonClearQueue(VOID)
{
    PIO_MONITOR_EVENT eventRecord;

    for (;;) {
        eventRecord = IoMonPopEvent();
        if (eventRecord == NULL) {
            break;
        }
        ExFreeToNPagedLookasideList(&gEventLookaside, eventRecord);
    }
}

static VOID
IoMonStopTargets(VOID)
{
    LONG wasActive;
    ULONG bloomIndex;

    ExAcquireFastMutex(&gTargetUpdateMutex);
    wasActive = InterlockedExchange(&gTargetSnapshotActive, FALSE);
    InterlockedExchange(&gFastProcessCount, 0);
    InterlockedExchange(&gFastSingleProcessId, 0);
    InterlockedExchange(&gFastOperationMask, 0);
    for (bloomIndex = 0;
         bloomIndex < IO_MONITOR_PID_BLOOM_WORDS;
         ++bloomIndex) {
        InterlockedExchange64(&gTargetPidBloom[bloomIndex], 0);
    }
    (VOID)InterlockedIncrement(&gGeneration);
    if (wasActive != FALSE) {
        ExWaitForRundownProtectionRelease(&gTargetSnapshot.Rundown);
        gTargetRundownCompleted = TRUE;
    }

    gTargetSnapshot.ProcessCount = 0;
    gTargetSnapshot.OperationMask = 0;
    gTargetSnapshot.Generation = 0;
    RtlZeroMemory(
        gTargetSnapshot.ProcessIds,
        sizeof(gTargetSnapshot.ProcessIds));

    IoMonClearQueue();
    KeSetEvent(&gQueueAvailable, IO_NO_INCREMENT, FALSE);
    ExReleaseFastMutex(&gTargetUpdateMutex);
}

static VOID
IoMonSetTargets(
    _In_reads_(ProcessCount) const ULONG *ProcessIds,
    _In_ ULONG ProcessCount,
    _In_ ULONG OperationMask
    )
{
    LONG wasActive;
    ULONG index;
    ULONGLONG bloom[IO_MONITOR_PID_BLOOM_WORDS] = { 0 };

    ExAcquireFastMutex(&gTargetUpdateMutex);
    wasActive = InterlockedExchange(&gTargetSnapshotActive, FALSE);
    if (wasActive != FALSE) {
        ExWaitForRundownProtectionRelease(&gTargetSnapshot.Rundown);
        gTargetRundownCompleted = TRUE;
    }

    IoMonClearQueue();
    InterlockedExchange64(&gDroppedEvents, 0);

    if (gTargetRundownCompleted) {
        ExReInitializeRundownProtection(&gTargetSnapshot.Rundown);
        gTargetRundownCompleted = FALSE;
    }

    RtlCopyMemory(
        gTargetSnapshot.ProcessIds,
        ProcessIds,
        ProcessCount * sizeof(ProcessIds[0]));

    for (index = 1; index < ProcessCount; ++index) {
        const ULONG processId = gTargetSnapshot.ProcessIds[index];
        ULONG insertionIndex = index;

        while (insertionIndex != 0 &&
            gTargetSnapshot.ProcessIds[insertionIndex - 1] > processId) {
            gTargetSnapshot.ProcessIds[insertionIndex] =
                gTargetSnapshot.ProcessIds[insertionIndex - 1];
            --insertionIndex;
        }
        gTargetSnapshot.ProcessIds[insertionIndex] = processId;
    }

    gTargetSnapshot.ProcessCount = ProcessCount;
    gTargetSnapshot.OperationMask = OperationMask;
    gTargetSnapshot.Generation = (ULONG)InterlockedIncrement(&gGeneration);

    for (index = 0; index < ProcessCount; ++index) {
        const ULONG hash = IoMonHashProcessId(
            gTargetSnapshot.ProcessIds[index]);
        const ULONG wordIndex =
            (hash >> 6) & (IO_MONITOR_PID_BLOOM_WORDS - 1);
        bloom[wordIndex] |= 1ULL << (hash & 63);
    }
    for (index = 0; index < IO_MONITOR_PID_BLOOM_WORDS; ++index) {
        InterlockedExchange64(&gTargetPidBloom[index], (LONG64)bloom[index]);
    }
    InterlockedExchange(&gFastOperationMask, (LONG)OperationMask);
    InterlockedExchange(
        &gFastSingleProcessId,
        ProcessCount == 1 ? (LONG)gTargetSnapshot.ProcessIds[0] : 0);
    InterlockedExchange(&gFastProcessCount, (LONG)ProcessCount);
    InterlockedExchange(&gTargetSnapshotActive, TRUE);
    ExReleaseFastMutex(&gTargetUpdateMutex);
}

static BOOLEAN
IoMonCaptureFileName(
    _In_ PFLT_CALLBACK_DATA Data,
    _In_ PCFLT_RELATED_OBJECTS FltObjects,
    _Inout_ PIO_MONITOR_EVENT EventRecord
    )
{
    NTSTATUS status;
    PFLT_FILE_NAME_INFORMATION nameInfo = NULL;
    PIO_MONITOR_NAME_CONTEXT nameContext = NULL;
    PIO_MONITOR_NAME_CONTEXT existingContext = NULL;
    USHORT bytesToCopy;
    const USHORT maximumBytes =
        (USHORT)((IO_MONITOR_MAX_PATH_CHARS - 1) * sizeof(WCHAR));

    if (KeGetCurrentIrql() > APC_LEVEL) {
        EventRecord->Flags |= IO_MONITOR_FLAG_NAME_UNAVAILABLE;
        return FALSE;
    }

    if (FltObjects->FileObject != NULL) {
        status = FltGetStreamHandleContext(
            FltObjects->Instance,
            FltObjects->FileObject,
            (PFLT_CONTEXT *)&nameContext);
        if (NT_SUCCESS(status) && nameContext != NULL) {
            bytesToCopy = nameContext->PathLengthBytes;
            if (bytesToCopy != 0 && bytesToCopy <= maximumBytes) {
                RtlCopyMemory(
                    EventRecord->Path,
                    nameContext->Path,
                    bytesToCopy);
                EventRecord->Path[bytesToCopy / sizeof(WCHAR)] = L'\0';
                EventRecord->PathLengthBytes = bytesToCopy;
                FltReleaseContext(nameContext);
                return TRUE;
            }
            FltReleaseContext(nameContext);
            nameContext = NULL;
        }
    }

    status = FltGetFileNameInformation(
        Data,
        FLT_FILE_NAME_NORMALIZED | FLT_FILE_NAME_QUERY_DEFAULT,
        &nameInfo);

    if (!NT_SUCCESS(status) || nameInfo == NULL) {
        EventRecord->Flags |= IO_MONITOR_FLAG_NAME_UNAVAILABLE;
        return FALSE;
    }

    bytesToCopy = nameInfo->Name.Length;
    if (bytesToCopy > maximumBytes) {
        bytesToCopy = maximumBytes;
        EventRecord->Flags |= IO_MONITOR_FLAG_NAME_TRUNCATED;
    }

    if (bytesToCopy == 0 || nameInfo->Name.Buffer == NULL) {
        EventRecord->Flags |= IO_MONITOR_FLAG_NAME_UNAVAILABLE;
        FltReleaseFileNameInformation(nameInfo);
        return FALSE;
    }

    RtlCopyMemory(EventRecord->Path, nameInfo->Name.Buffer, bytesToCopy);
    EventRecord->Path[bytesToCopy / sizeof(WCHAR)] = L'\0';
    EventRecord->PathLengthBytes = bytesToCopy;

    FltReleaseFileNameInformation(nameInfo);

    if (FltObjects->FileObject != NULL &&
        NT_SUCCESS(FltAllocateContext(
            gFilter,
            FLT_STREAMHANDLE_CONTEXT,
            sizeof(IO_MONITOR_NAME_CONTEXT),
            NonPagedPoolNx,
            (PFLT_CONTEXT *)&nameContext))) {
        RtlZeroMemory(nameContext, sizeof(*nameContext));
        nameContext->PathLengthBytes = bytesToCopy;
        RtlCopyMemory(nameContext->Path, EventRecord->Path, bytesToCopy);
        nameContext->Path[bytesToCopy / sizeof(WCHAR)] = L'\0';

        status = FltSetStreamHandleContext(
            FltObjects->Instance,
            FltObjects->FileObject,
            FLT_SET_CONTEXT_KEEP_IF_EXISTS,
            nameContext,
            (PFLT_CONTEXT *)&existingContext);
        if (existingContext != NULL) {
            FltReleaseContext(existingContext);
        }
        FltReleaseContext(nameContext);
    }

    return TRUE;
}

static FLT_PREOP_CALLBACK_STATUS
IoMonPreSetInformation(
    _Inout_ PFLT_CALLBACK_DATA Data,
    _In_ PCFLT_RELATED_OBJECTS FltObjects,
    _Flt_CompletionContext_Outptr_ PVOID *CompletionContext
    )
{
    FILE_INFORMATION_CLASS informationClass;
    PFLT_CONTEXT deletedContext = NULL;

    *CompletionContext = NULL;
    informationClass =
        Data->Iopb->Parameters.SetFileInformation.FileInformationClass;

    if ((informationClass == FileRenameInformation ||
         informationClass == FileRenameInformationEx) &&
        FltObjects->FileObject != NULL &&
        KeGetCurrentIrql() <= APC_LEVEL &&
        NT_SUCCESS(FltDeleteStreamHandleContext(
            FltObjects->Instance,
            FltObjects->FileObject,
            &deletedContext)) &&
        deletedContext != NULL) {
        FltReleaseContext(deletedContext);
    }

    return FLT_PREOP_SUCCESS_NO_CALLBACK;
}

static VOID
IoMonNameContextCleanup(
    _In_ PFLT_CONTEXT Context,
    _In_ FLT_CONTEXT_TYPE ContextType
    )
{
    UNREFERENCED_PARAMETER(Context);
    UNREFERENCED_PARAMETER(ContextType);
}

static FLT_PREOP_CALLBACK_STATUS
IoMonPreOperation(
    _Inout_ PFLT_CALLBACK_DATA Data,
    _In_ PCFLT_RELATED_OBJECTS FltObjects,
    _Flt_CompletionContext_Outptr_ PVOID *CompletionContext
    )
{
    ULONG requestorProcessId;
    ULONG operation;
    ULONG generation;
    PIO_MONITOR_EVENT eventRecord;
    LARGE_INTEGER timestamp;

    UNREFERENCED_PARAMETER(FltObjects);
    *CompletionContext = NULL;

    operation = Data->Iopb->MajorFunction == IRP_MJ_READ
        ? IO_MONITOR_OPERATION_READ
        : IO_MONITOR_OPERATION_WRITE;
    requestorProcessId = FltGetRequestorProcessId(Data);
    if (requestorProcessId == 0 ||
        !IoMonMightContainTarget(requestorProcessId, operation) ||
        !IoMonGetTargetGeneration(
            requestorProcessId,
            operation,
            &generation)) {
        return FLT_PREOP_SUCCESS_NO_CALLBACK;
    }

    eventRecord = (PIO_MONITOR_EVENT)ExAllocateFromNPagedLookasideList(
        &gEventLookaside);

    if (eventRecord == NULL) {
        InterlockedIncrement64(&gDroppedEvents);
        return FLT_PREOP_SUCCESS_NO_CALLBACK;
    }

    RtlZeroMemory(eventRecord, sizeof(*eventRecord));
    KeQuerySystemTime(&timestamp);

    eventRecord->Sequence = (ULONGLONG)InterlockedIncrement64(&gSequence);
    eventRecord->Timestamp100ns = timestamp.QuadPart;
    eventRecord->ProcessId = requestorProcessId;
    eventRecord->ThreadId = HandleToULong(PsGetCurrentThreadId());
    eventRecord->Generation = (ULONG)generation;
    eventRecord->Operation = operation;

    if (operation == IO_MONITOR_OPERATION_READ) {
        eventRecord->RequestedBytes = Data->Iopb->Parameters.Read.Length;
    } else {
        eventRecord->RequestedBytes = Data->Iopb->Parameters.Write.Length;
    }

    if (FlagOn(Data->Iopb->IrpFlags, IRP_PAGING_IO)) {
        eventRecord->Flags |= IO_MONITOR_FLAG_PAGING_IO;
    }
    if (FlagOn(Data->Iopb->IrpFlags, IRP_NOCACHE)) {
        eventRecord->Flags |= IO_MONITOR_FLAG_NON_CACHED;
    }
    if (FlagOn(Data->Iopb->IrpFlags, IRP_SYNCHRONOUS_PAGING_IO)) {
        eventRecord->Flags |= IO_MONITOR_FLAG_SYNCHRONOUS_PAGING;
    }
    if (FLT_IS_IRP_OPERATION(Data)) {
        eventRecord->Flags |= IO_MONITOR_FLAG_IRP_OPERATION;
    }
    if (FLT_IS_FASTIO_OPERATION(Data)) {
        eventRecord->Flags |= IO_MONITOR_FLAG_FAST_IO_OPERATION;
    }

    if (!IoMonCaptureFileName(Data, FltObjects, eventRecord)) {
        ExFreeToNPagedLookasideList(&gEventLookaside, eventRecord);
        return FLT_PREOP_SUCCESS_NO_CALLBACK;
    }

    *CompletionContext = eventRecord;
    return FLT_PREOP_SUCCESS_WITH_CALLBACK;
}

static FLT_POSTOP_CALLBACK_STATUS
IoMonPostOperation(
    _Inout_ PFLT_CALLBACK_DATA Data,
    _In_ PCFLT_RELATED_OBJECTS FltObjects,
    _In_opt_ PVOID CompletionContext,
    _In_ FLT_POST_OPERATION_FLAGS Flags
    )
{
    PIO_MONITOR_EVENT eventRecord = (PIO_MONITOR_EVENT)CompletionContext;

    UNREFERENCED_PARAMETER(FltObjects);

    if (eventRecord == NULL) {
        return FLT_POSTOP_FINISHED_PROCESSING;
    }

    if (FlagOn(Flags, FLTFL_POST_OPERATION_DRAINING)) {
        ExFreeToNPagedLookasideList(&gEventLookaside, eventRecord);
        return FLT_POSTOP_FINISHED_PROCESSING;
    }

    eventRecord->Status = (ULONG)Data->IoStatus.Status;
    eventRecord->ActualBytes = (ULONGLONG)Data->IoStatus.Information;
    IoMonEnqueueEvent(eventRecord);

    return FLT_POSTOP_FINISHED_PROCESSING;
}

static NTSTATUS
IoMonConnect(
    _In_ PFLT_PORT ClientPort,
    _In_opt_ PVOID ServerPortCookie,
    _In_reads_bytes_opt_(SizeOfContext) PVOID ConnectionContext,
    _In_ ULONG SizeOfContext,
    _Outptr_result_maybenull_ PVOID *ConnectionPortCookie
    )
{
    PVOID previousPort;

    UNREFERENCED_PARAMETER(ServerPortCookie);
    UNREFERENCED_PARAMETER(ConnectionContext);
    UNREFERENCED_PARAMETER(SizeOfContext);

    *ConnectionPortCookie = NULL;
    previousPort = InterlockedCompareExchangePointer(
        (PVOID volatile *)&gClientPort,
        ClientPort,
        NULL);

    return previousPort == NULL ? STATUS_SUCCESS : STATUS_DEVICE_BUSY;
}

static VOID
IoMonDisconnect(
    _In_opt_ PVOID ConnectionCookie
    )
{
    UNREFERENCED_PARAMETER(ConnectionCookie);
    IoMonStopTargets();
    FltCloseClientPort(gFilter, &gClientPort);
}

static NTSTATUS
IoMonMessage(
    _In_opt_ PVOID PortCookie,
    _In_reads_bytes_opt_(InputBufferLength) PVOID InputBuffer,
    _In_ ULONG InputBufferLength,
    _Out_writes_bytes_to_opt_(OutputBufferLength, *ReturnOutputBufferLength) PVOID OutputBuffer,
    _In_ ULONG OutputBufferLength,
    _Out_ PULONG ReturnOutputBufferLength
    )
{
    IO_MONITOR_COMMAND command;
    PIO_MONITOR_RESPONSE response = NULL;
    LARGE_INTEGER timeout;
    ULONG waitMilliseconds;
    ULONG index;
    ULONG previousIndex;
    NTSTATUS status = STATUS_SUCCESS;

    UNREFERENCED_PARAMETER(PortCookie);
    *ReturnOutputBufferLength = 0;

    if (InputBuffer == NULL || InputBufferLength < sizeof(command) ||
        OutputBuffer == NULL || OutputBufferLength < sizeof(IO_MONITOR_RESPONSE)) {
        return STATUS_INVALID_PARAMETER;
    }

    __try {
        RtlCopyMemory(&command, InputBuffer, sizeof(command));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return GetExceptionCode();
    }

    if (command.Version != IO_MONITOR_PROTOCOL_VERSION) {
        return STATUS_REVISION_MISMATCH;
    }

    response = (PIO_MONITOR_RESPONSE)ExAllocatePool2(
        POOL_FLAG_PAGED,
        sizeof(IO_MONITOR_RESPONSE),
        IO_MONITOR_POOL_TAG);
    if (response == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    RtlZeroMemory(response, sizeof(*response));

    switch (command.Command) {
    case IO_MONITOR_COMMAND_SET_TARGETS:
        if (command.TargetProcessCount == 0 ||
            command.TargetProcessCount > IO_MONITOR_MAX_TARGET_PROCESSES ||
            command.OperationMask == 0 ||
            FlagOn(command.OperationMask, ~IO_MONITOR_OPERATION_ALL)) {
            status = STATUS_INVALID_PARAMETER;
            break;
        }
        for (index = 0; index < command.TargetProcessCount; ++index) {
            if (command.ProcessIds[index] == 0 ||
                command.ProcessIds[index] > MAXLONG) {
                status = STATUS_INVALID_PARAMETER;
                break;
            }
            for (previousIndex = 0; previousIndex < index; ++previousIndex) {
                if (command.ProcessIds[previousIndex] == command.ProcessIds[index]) {
                    status = STATUS_INVALID_PARAMETER;
                    break;
                }
            }
            if (!NT_SUCCESS(status)) {
                break;
            }
        }
        if (NT_SUCCESS(status)) {
            IoMonSetTargets(
                command.ProcessIds,
                command.TargetProcessCount,
                command.OperationMask);
        }
        break;

    case IO_MONITOR_COMMAND_GET_EVENTS:
        response->EventCount = IoMonPopEventBatch(response->Events);
        if (response->EventCount == 0 && command.WaitMilliseconds != 0) {
            waitMilliseconds = command.WaitMilliseconds;
            if (waitMilliseconds > IO_MONITOR_MAX_WAIT_MS) {
                waitMilliseconds = IO_MONITOR_MAX_WAIT_MS;
            }
            timeout.QuadPart = -((LONGLONG)waitMilliseconds * 10 * 1000);
            (VOID)KeWaitForSingleObject(
                &gQueueAvailable,
                Executive,
                KernelMode,
                FALSE,
                &timeout);
            response->EventCount = IoMonPopEventBatch(response->Events);
        }
        break;

    case IO_MONITOR_COMMAND_GET_STATUS:
        break;

    case IO_MONITOR_COMMAND_STOP:
        IoMonStopTargets();
        break;

    default:
        status = STATUS_INVALID_PARAMETER;
        break;
    }

    if (NT_SUCCESS(status)) {
        response->Version = IO_MONITOR_PROTOCOL_VERSION;
        response->Command = command.Command;
        response->TargetProcessCount = IoMonTargetCount();
        response->QueueDepth = IoMonQueueDepth();
        response->DroppedEvents = (ULONGLONG)IoMonReadLong64(&gDroppedEvents);

        __try {
            RtlCopyMemory(OutputBuffer, response, sizeof(*response));
            *ReturnOutputBufferLength = sizeof(*response);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            status = GetExceptionCode();
            *ReturnOutputBufferLength = 0;
        }
    }

    ExFreePoolWithTag(response, IO_MONITOR_POOL_TAG);
    return status;
}

static NTSTATUS
IoMonUnload(
    _In_ FLT_FILTER_UNLOAD_FLAGS Flags
    )
{
    UNREFERENCED_PARAMETER(Flags);

    IoMonStopTargets();

    if (gServerPort != NULL) {
        FltCloseCommunicationPort(gServerPort);
        gServerPort = NULL;
    }
    if (gFilter != NULL) {
        FltUnregisterFilter(gFilter);
        gFilter = NULL;
    }
    ExDeleteNPagedLookasideList(&gEventLookaside);

    return STATUS_SUCCESS;
}

NTSTATUS
DriverEntry(
    _In_ PDRIVER_OBJECT DriverObject,
    _In_ PUNICODE_STRING RegistryPath
    )
{
    NTSTATUS status;
    PSECURITY_DESCRIPTOR securityDescriptor = NULL;
    OBJECT_ATTRIBUTES objectAttributes;
    UNICODE_STRING portName;

    UNREFERENCED_PARAMETER(RegistryPath);

    KeInitializeSpinLock(&gQueueLock);
    KeInitializeEvent(&gQueueAvailable, NotificationEvent, FALSE);
    RtlZeroMemory(gQueue, sizeof(gQueue));
    ExInitializeFastMutex(&gTargetUpdateMutex);
    RtlZeroMemory(&gTargetSnapshot, sizeof(gTargetSnapshot));
    ExInitializeRundownProtection(&gTargetSnapshot.Rundown);
    ExInitializeNPagedLookasideList(
        &gEventLookaside,
        NULL,
        NULL,
        POOL_NX_ALLOCATION,
        sizeof(IO_MONITOR_EVENT),
        IO_MONITOR_POOL_TAG,
        0);

    status = FltRegisterFilter(DriverObject, &gRegistration, &gFilter);
    if (!NT_SUCCESS(status)) {
        ExDeleteNPagedLookasideList(&gEventLookaside);
        return status;
    }

    status = FltBuildDefaultSecurityDescriptor(
        &securityDescriptor,
        FLT_PORT_ALL_ACCESS);
    if (!NT_SUCCESS(status)) {
        goto Cleanup;
    }

    RtlInitUnicodeString(&portName, IO_MONITOR_PORT_NAME);
    InitializeObjectAttributes(
        &objectAttributes,
        &portName,
        OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE,
        NULL,
        securityDescriptor);

    status = FltCreateCommunicationPort(
        gFilter,
        &gServerPort,
        &objectAttributes,
        NULL,
        IoMonConnect,
        IoMonDisconnect,
        IoMonMessage,
        1);

    FltFreeSecurityDescriptor(securityDescriptor);
    securityDescriptor = NULL;

    if (!NT_SUCCESS(status)) {
        goto Cleanup;
    }

    status = FltStartFiltering(gFilter);
    if (!NT_SUCCESS(status)) {
        goto Cleanup;
    }

    return STATUS_SUCCESS;

Cleanup:
    if (securityDescriptor != NULL) {
        FltFreeSecurityDescriptor(securityDescriptor);
    }
    if (gServerPort != NULL) {
        FltCloseCommunicationPort(gServerPort);
        gServerPort = NULL;
    }
    if (gFilter != NULL) {
        FltUnregisterFilter(gFilter);
        gFilter = NULL;
    }
    ExDeleteNPagedLookasideList(&gEventLookaside);
    return status;
}

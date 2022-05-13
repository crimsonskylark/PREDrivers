#include <ntifs.h>
#include <ntddk.h>

static UNICODE_STRING DEVICE_NAME  = RTL_CONSTANT_STRING(L"\\Device\\PREApc");
static UNICODE_STRING SYMLINK_NAME = RTL_CONSTANT_STRING(L"\\??\\PREApc");

static constexpr ULONG THREAD_LIST_HEAD      = 0x30;
static constexpr ULONG THREAD_LIST_ENTRY     = 0x2f8;
static constexpr ULONG KAPC_STATE_FIELD      = 0x98;
static constexpr ULONG KAPC_STATE_LIST_ENTRY = 0x10;

static constexpr ULONG POOL_TAG = ' ERP';

static HANDLE MonitorThreadHandle = nullptr;

extern "C" {

    typedef VOID(*PKNORMAL_ROUTINE)(
        PVOID NormalContext,
        PVOID SystemArgument1,
        PVOID SystemArgument2
        );

    typedef VOID(*PKKERNEL_ROUTINE)(
        PKAPC Apc,
        PKNORMAL_ROUTINE* NormalRoutine,
        PVOID* NormalContext,
        PVOID* SystemArgument1,
        PVOID* SystemArgument2
        );

    typedef VOID(*PKRUNDOWN_ROUTINE)(
        PKAPC Apc
        );

    typedef enum _KAPC_ENVIRONMENT {
        OriginalApcEnvironment,
        AttachedApcEnvironment,
        CurrentApcEnvironment,
        InsertApcEnvironment
    } KAPC_ENVIRONMENT, * PKAPC_ENVIRONMENT;

    NTKERNELAPI VOID KeInitializeApc(
        PKAPC,
        PKTHREAD,
        KAPC_ENVIRONMENT,
        PKKERNEL_ROUTINE,
        PKRUNDOWN_ROUTINE,
        PKNORMAL_ROUTINE,
        KPROCESSOR_MODE,
        PVOID);

    NTKERNELAPI BOOLEAN KeInsertQueueApc(
        PRKAPC Apc,
        PVOID SystemArgument1,
        PVOID SystemArgument2,
        KPRIORITY Increment
    );

    VOID ApcRoutine(PKAPC Apc, PKNORMAL_ROUTINE* NormalRoutine, PVOID* NormalContext, PVOID* SystemArgument1, PVOID* SystemArgument2) {
        DbgPrint("Apc: %p\nNormal routine: %p\nNormal context: %p\nSystem argument: %p\nSystem argument (2): %p\n", Apc,
            NormalRoutine,
            NormalContext,
            SystemArgument1,
            SystemArgument2);
    }

    VOID ApcRoutineRundown(PKAPC Apc) {
        ExFreePoolWithTag(Apc, ' ERP');
        DbgPrint("inside rundown routine");
    }

    VOID ApcMonitorThread(PVOID Ctx) {
        UNREFERENCED_PARAMETER(Ctx);
        DbgPrint("starting monitor thread...\n");

        auto proc = PsGetCurrentProcess();
        auto procThreadListHead = reinterpret_cast<LIST_ENTRY*>(reinterpret_cast<CHAR*>(proc) + THREAD_LIST_HEAD);
        
        SIZE_T threadCount = 0;

        for (auto Entry = procThreadListHead->Flink; Entry != procThreadListHead; Entry = Entry->Flink) {
            auto threadObj = (CHAR*)Entry - THREAD_LIST_ENTRY;
            DbgPrint("[%llu] Thread object: %p\n", threadCount, threadObj);
            if (MmIsAddressValid(threadObj)) {
                const auto threadApcState = reinterpret_cast<PKAPC_STATE>(threadObj + KAPC_STATE_FIELD);
                LIST_ENTRY* kernelApcListHead = &(threadApcState->ApcListHead[KernelMode]);
                LIST_ENTRY* userApcListHead   = &(threadApcState->ApcListHead[UserMode]);

                DbgPrint("=========>\tAPC(%p:%p)\n", kernelApcListHead, userApcListHead);

                KIRQL oldIrql = KeRaiseIrqlToSynchLevel();

                // Queue some APCs if the list is empty to ensure the code below actually works.
                if (IsListEmpty(kernelApcListHead)) {
                    for (SIZE_T idx = 0; idx < 5; idx++) {
                        // will never fail because my computer has infinite RAM
                        auto testApcObj = static_cast<PKAPC>(ExAllocatePoolZero(NonPagedPool, sizeof(KAPC), POOL_TAG));
                        KeInitializeApc(testApcObj, reinterpret_cast<PETHREAD>(threadObj), OriginalApcEnvironment, ApcRoutine, ApcRoutineRundown, nullptr, KernelMode, nullptr);
                        if (KeInsertQueueApc(testApcObj, nullptr, nullptr, 0)) 
                            DbgPrint("QueuedApc(%p, %p)\n", testApcObj, ApcRoutine);
                    }
                }

                for (auto KernelApcEntry = kernelApcListHead->Flink; KernelApcEntry != kernelApcListHead; KernelApcEntry = KernelApcEntry->Flink) {
                    auto queuedApcObj = reinterpret_cast<CHAR*>(KernelApcEntry) - KAPC_STATE_LIST_ENTRY;
                    DbgPrint("[Kernel] Entry: %p (%p)\n", KernelApcEntry, queuedApcObj);
                }

                KeLowerIrql(oldIrql);
            }
            threadCount++;
        }
    }

    VOID DrvUnload(PDRIVER_OBJECT DrvObj) {
        IoDeleteSymbolicLink(&SYMLINK_NAME);
        IoDeleteDevice(DrvObj->DeviceObject);
        if (MonitorThreadHandle)
            ZwClose(MonitorThreadHandle);
    }

    NTSTATUS DrvOpenClose(PDEVICE_OBJECT DvcObj, PIRP Irp) {
        UNREFERENCED_PARAMETER(DvcObj);
        Irp->IoStatus.Status = STATUS_SUCCESS;
        Irp->IoStatus.Information = 0;
        return STATUS_SUCCESS;
    }

    NTSTATUS DriverEntry(PDRIVER_OBJECT DrvObj, UNICODE_STRING RegPath) {
        UNREFERENCED_PARAMETER(RegPath);

        PDEVICE_OBJECT dvcObj = nullptr;
        auto status = STATUS_SUCCESS;

        status = IoCreateDevice(DrvObj, 0, &DEVICE_NAME, 0, 0, 0, &dvcObj);

        if (!NT_SUCCESS(status)) {
            DbgPrint("unable to create device: %08x\n", status);
            return status;
        }

        status = IoCreateSymbolicLink(&SYMLINK_NAME, &DEVICE_NAME);

        if (!NT_SUCCESS(status)) {
            DbgPrint("unable to create symbolic link: %08x\n", status);
            IoDeleteDevice(dvcObj);
            return status;
        }

        DrvObj->MajorFunction[IRP_MJ_CREATE] =
            DrvObj->MajorFunction[IRP_MJ_CLOSE] = DrvOpenClose;
        DrvObj->DriverUnload = DrvUnload;

        DbgPrint("created device (%p) and symbolic link.\n", dvcObj);

        status = PsCreateSystemThread(&MonitorThreadHandle, THREAD_ALL_ACCESS, nullptr, NtCurrentProcess(), nullptr, ApcMonitorThread, NULL);

        if (!NT_SUCCESS(status)) {
            DbgPrint("unable to create monitor thread: %08x\n", status);
            IoDeleteSymbolicLink(&SYMLINK_NAME);
            IoDeleteDevice(dvcObj);
            return status;
        }

        return status;
    }
}
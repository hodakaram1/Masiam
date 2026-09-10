#pragma once

#include <windows.h>
#include <winioctl.h>
#include <vector>
#include <string>
#include <thread>
#include <mutex>
#include <atomic>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <d3d11.h>
#include <GLFW/glfw3.h>
#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3native.h>

#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_dx11.h"
#include "Resource.h"

// Zydis & AsmJit Includes (vcpkg headers)
#include <Zydis/Zydis.h>
#include <asmjit/asmjit.h>

// =====================================================================
//  IOCTL Codes
// =====================================================================
#define SIOCTL_TYPE 50000

#define IOCTL_V2_FIRST_SCAN         CTL_CODE(SIOCTL_TYPE, 0x902, METHOD_BUFFERED, FILE_READ_ACCESS | FILE_WRITE_ACCESS)
#define IOCTL_V2_GET_PROCESS_LIST   CTL_CODE(SIOCTL_TYPE, 0x903, METHOD_BUFFERED, FILE_READ_ACCESS | FILE_WRITE_ACCESS)
#define IOCTL_V2_NEXT_SCAN          CTL_CODE(SIOCTL_TYPE, 0x904, METHOD_BUFFERED, FILE_READ_ACCESS | FILE_WRITE_ACCESS)
#define IOCTL_V2_WRITE_MEMORY       CTL_CODE(SIOCTL_TYPE, 0x905, METHOD_BUFFERED, FILE_READ_ACCESS | FILE_WRITE_ACCESS)
#define IOCTL_V2_READ_MEMORY        CTL_CODE(SIOCTL_TYPE, 0x906, METHOD_BUFFERED, FILE_READ_ACCESS | FILE_WRITE_ACCESS)
#define IOCTL_V2_ENUM_MODULES       CTL_CODE(SIOCTL_TYPE, 0x907, METHOD_BUFFERED, FILE_READ_ACCESS | FILE_WRITE_ACCESS)
#define IOCTL_V2_PATCH_MEMORY       CTL_CODE(SIOCTL_TYPE, 0x908, METHOD_BUFFERED, FILE_READ_ACCESS | FILE_WRITE_ACCESS)

// Maximum addresses kept from a first scan (driver reports truncation past this)
#define MAX_SCAN_RESULTS 200000

// First-scan request flags (SCAN_REQUEST_V2.Flags)
#define SCAN_FLAG_UNALIGNED       0x1
// First-scan output layout: out[0] holds result flags, addresses follow from out[1]
#define SCAN_RESULT_TRUNCATED     0x1

// =====================================================================
//  Shared Structures (Pack = 1)
// =====================================================================
#pragma pack(push, 1)

typedef struct _ProcessInfoV2 {
    ULONG ProcessId;
    CHAR  Name[64];
} ProcessInfoV2, * PProcessInfoV2;

typedef struct _SCAN_REQUEST_V2 {
    ULONG     TargetPid;
    ULONG     DataType;
    ULONG64   SearchValue64;
    ULONG_PTR StartAddress;
    ULONG_PTR EndAddress;
    ULONG     Flags;
} SCAN_REQUEST_V2, * PSCAN_REQUEST_V2;

typedef struct _NEXT_SCAN_HEADER_V2 {
    ULONG     TargetPid;
    ULONG     DataType;
    ULONG64   SearchValue64;
    ULONG     AddressCount;
} NEXT_SCAN_HEADER_V2, * PNEXT_SCAN_HEADER_V2;

typedef struct _MEMORY_REQUEST_V2 {
    ULONG     TargetPid;
    ULONG_PTR Address;
    ULONG     DataSize;
    ULONG64   Value64;
} MEMORY_REQUEST_V2, * PMEMORY_REQUEST_V2;

typedef struct _PATCH_REQUEST_V2 {
    ULONG     TargetPid;
    ULONG_PTR Address;
    UCHAR     Pattern[32];
    ULONG     Size;
} PATCH_REQUEST_V2, * PPATCH_REQUEST_V2;

typedef struct _ModuleInfoV2 {
    CHAR      ModuleName[128];
    CHAR      FullPath[260];
    ULONG_PTR BaseAddress;
    ULONG     Size;
} ModuleInfoV2, * PModuleInfoV2;

#pragma pack(pop)

// =====================================================================
//  Cheat Table Item Structure
// =====================================================================
struct CheatItem {
    ULONG_PTR Address;
    ULONG64   Value64;
    bool      Enabled;
    int       DataType;   // size/type captured when the item was added
    ULONG     Pid;        // process captured when the item was added
    char      Description[64];
};

// =====================================================================
//  Function Prototypes
// =====================================================================
bool LoadAndStartDriver();
void StopAndUnloadDriver();
bool ConnectDriver();

ULONG GetDataSize(int dataType);
void WriteMemory(ULONG pid, ULONG_PTR addr, ULONG64 val64, int dataType);
void ReadMemory(ULONG pid, ULONG_PTR addr, ULONG64* outVal, int dataType);
void PatchMemory(ULONG_PTR addr, const UCHAR* pattern, ULONG size);

ULONG64 ParseInputToValue(const char* str, int dataType);
void FormatValueToString(ULONG64 val64, int dataType, char* outBuf, size_t maxLen);
ULONG_PTR ResolvePointerPath(ULONG pid, ULONG_PTR baseAddress, const std::vector<LONG>& offsets);
struct ResolvedPointer;
ResolvedPointer ResolvePointerSmart(ULONG pid, const char* addressInput, const char* offsetsInput);
std::string GetAutoOffsetForAddress(ULONG_PTR targetAddr);
std::string GetZydisDisassembledBytes(ULONG_PTR targetAddr, ULONG instructionCount);

void FreezeLoop();
void RefreshProcessList();
void RefreshModules();

void AsyncFirstScanWorker(ULONG targetPid, int dataType, ULONG64 searchVal64, bool useRange, ULONG_PTR rangeStart, ULONG_PTR rangeEnd, bool allowUnaligned);
void AsyncNextScanWorker(ULONG targetPid, int dataType, ULONG64 searchVal64, std::vector<ULONG_PTR> prevResults);
void StartFirstScan();
void StartNextScan();
void ExportResultsToFile();
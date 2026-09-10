#include "Imno.h"

// Direct Link Libraries for D3D11 & DXGI
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "advapi32.lib")

// Smart Pointer Resolver struct (must be before globals)
struct ResolvedPointer {
    ULONG_PTR FinalAddress = 0;
    ULONG64   FinalValue = 0;
    std::string BaseModule;
    ULONG_PTR BaseOffset = 0;
    std::vector<LONG> Offsets;
    bool Success = false;
};

// Globals
HWND g_hWnd = NULL;
GLFWwindow* g_Window = NULL;
HANDLE g_hDriver = INVALID_HANDLE_VALUE;
bool   g_DriverConnected = false;

std::vector<ProcessInfo> g_ProcessList;
std::vector<ModuleInfo>  g_LoadedModules;
ULONG g_SelectedPid = 0;
char  g_ProcessFilter[64] = "";
char  g_ModuleFilter[64] = "";

// Scan State
int   g_SelectedDataType = 4; // 4 = 4 Bytes
char  g_ScanValueInput[128] = "";
char  g_ScanRangeStart[64] = "0x10000";
char  g_ScanRangeEnd[64] = "0x7FFFFFFFFFFF";
bool  g_UseScanRange = false;
bool  g_AllowUnaligned = false;

std::vector<ULONG_PTR> g_ScanResults;
std::mutex             g_ScanResultsLock;   // guards g_ScanResults between UI and scan workers
std::atomic<bool>      g_IsScanning(false);
std::atomic<int>       g_ScanProgress(0);
std::atomic<bool>      g_ScanTruncated(false);
std::atomic<unsigned>  g_ScanVersion(0);    // bumped whenever results change

// Cheat Table & Freeze
std::vector<CheatItem> g_CheatTable;
std::mutex g_CheatTableLock;
std::atomic<bool> g_FreezeRunning(true);
std::thread g_FreezeThread;

// Pointer Resolver & Assembler Patching
char g_PointerBaseInput[128] = "";
char g_PointerOffsetInput[128] = "0";
ULONG_PTR g_ResolvedAddress = 0;
ULONG64   g_ResolvedValue = 0;
ResolvedPointer g_LastResolved = {};

char g_PatchAddressInput[128] = "";
char g_PatchPatternInput[128] = "0x90, 0x90";

// Kernel tab state
ULONG     g_KernelVersion = 0;
ULONG64   g_KernelCR0 = 0;
ULONG64   g_KernelCR3 = 0;
ULONG64   g_KernelCR4 = 0;
ULONG64   g_KernelMsrValue = 0;
DBK_SEG_TABLE g_KernelIdt = {};
DBK_SEG_TABLE g_KernelGdt = {};
char      g_MsrReadInput[64] = "0xC0000082";   // IA32_LSTAR
char      g_MsrWriteMsr[64] = "0xC0000082";
char      g_MsrWriteValue[64] = "0";
char      g_PhysReadAddr[64] = "0";
char      g_PhysReadSize[64] = "0x100";
char      g_PhysReadDump[8192] = "";
char      g_PhysWriteAddr[64] = "0";
char      g_PhysWriteHex[1024] = "0x90, 0x90";
char      g_AllocNonPagedSize[64] = "0x1000";
ULONG64   g_AllocNonPagedAddr = 0;
char      g_AllocProcessSize[64] = "0x1000";
ULONG64   g_AllocProcessAddr = 0;
ULONG64   g_OpenProcessHandle = 0;
ULONG64   g_PeProcess = 0;
char      g_KernelStatus[256] = "";

// =====================================================================
//  Privilege & Driver Control Helpers
// =====================================================================
bool EnableSeDebugPrivilege()
{
    HANDLE hToken = NULL;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &hToken))
        return false;

    TOKEN_PRIVILEGES tp = {};
    LUID luid;
    if (!LookupPrivilegeValueW(NULL, SE_DEBUG_NAME, &luid)) {
        CloseHandle(hToken);
        return false;
    }

    tp.PrivilegeCount = 1;
    tp.Privileges[0].Luid = luid;
    tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;

    bool ok = AdjustTokenPrivileges(hToken, FALSE, &tp, sizeof(tp), NULL, NULL) != FALSE;
    CloseHandle(hToken);
    return ok;
}

bool LoadAndStartDriver()
{
    WCHAR sysPath[MAX_PATH];
    GetModuleFileNameW(NULL, sysPath, MAX_PATH);
    wchar_t* lastSlash = wcsrchr(sysPath, L'\\');
    if (lastSlash) *(lastSlash + 1) = L'\0';
    else return false;
    wcscat_s(sysPath, MAX_PATH, DBK_DRIVER_FILE);

    if (GetFileAttributesW(sysPath) == INVALID_FILE_ATTRIBUTES)
        return false;

    SC_HANDLE hSCM = OpenSCManagerW(NULL, NULL, SC_MANAGER_ALL_ACCESS);
    if (!hSCM) return false;

    SC_HANDLE hService = OpenServiceW(hSCM, DBK_SERVICE_NAME, SERVICE_ALL_ACCESS);
    if (!hService) {
        hService = CreateServiceW(
            hSCM,
            DBK_SERVICE_NAME,
            DBK_SERVICE_NAME,
            SERVICE_ALL_ACCESS,
            SERVICE_KERNEL_DRIVER,
            SERVICE_DEMAND_START,
            SERVICE_ERROR_NORMAL,
            sysPath,
            NULL, NULL, NULL, NULL, NULL
        );
    }

    if (!hService) {
        CloseServiceHandle(hSCM);
        return false;
    }

    // Always point the service at our DBK64.sys
    ChangeServiceConfigW(hService, SERVICE_KERNEL_DRIVER, SERVICE_DEMAND_START,
        SERVICE_ERROR_NORMAL, sysPath, NULL, NULL, NULL, NULL, NULL, DBK_SERVICE_NAME);

    // Stop a running instance so the registry values below are re-read on next start
    SERVICE_STATUS ss = {};
    ControlService(hService, SERVICE_CONTROL_STOP, &ss);

    // The driver reads A/B/C/D from its service key at DriverEntry.
    HKEY hKey = NULL;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, DBK_SERVICE_REG_KEY, 0, KEY_SET_VALUE, &hKey) == ERROR_SUCCESS) {
        RegSetValueExW(hKey, L"A", 0, REG_SZ, (const BYTE*)DBK_DEVICE_NAME,     (DWORD)((wcslen(DBK_DEVICE_NAME) + 1) * sizeof(WCHAR)));
        RegSetValueExW(hKey, L"B", 0, REG_SZ, (const BYTE*)DBK_SYMLINK_NAME,    (DWORD)((wcslen(DBK_SYMLINK_NAME) + 1) * sizeof(WCHAR)));
        RegSetValueExW(hKey, L"C", 0, REG_SZ, (const BYTE*)DBK_PROCESS_EVENT,   (DWORD)((wcslen(DBK_PROCESS_EVENT) + 1) * sizeof(WCHAR)));
        RegSetValueExW(hKey, L"D", 0, REG_SZ, (const BYTE*)DBK_THREAD_EVENT,    (DWORD)((wcslen(DBK_THREAD_EVENT) + 1) * sizeof(WCHAR)));
        RegCloseKey(hKey);
    }

    StartServiceW(hService, 0, NULL);

    CloseServiceHandle(hService);
    CloseServiceHandle(hSCM);
    return true;
}

void StopAndUnloadDriver()
{
    SC_HANDLE hSCM = OpenSCManagerW(NULL, NULL, SC_MANAGER_ALL_ACCESS);
    if (!hSCM) return;

    SC_HANDLE hService = OpenServiceW(hSCM, DBK_SERVICE_NAME, SERVICE_ALL_ACCESS);
    if (hService) {
        SERVICE_STATUS status;
        ControlService(hService, SERVICE_CONTROL_STOP, &status);
        DeleteService(hService);
        CloseServiceHandle(hService);
    }

    // Clean the A/B/C/D values from the service key
    HKEY hKey = NULL;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, DBK_SERVICE_REG_KEY, 0, KEY_SET_VALUE, &hKey) == ERROR_SUCCESS) {
        RegDeleteValueW(hKey, L"A");
        RegDeleteValueW(hKey, L"B");
        RegDeleteValueW(hKey, L"C");
        RegDeleteValueW(hKey, L"D");
        RegCloseKey(hKey);
    }

    CloseServiceHandle(hSCM);
}

bool ConnectDriver()
{
    DisconnectDriver();

    g_hDriver = CreateFileW(DBK_DEVICE_PATH,
        GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        NULL,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        NULL);

    g_DriverConnected = (g_hDriver != INVALID_HANDLE_VALUE);
    if (g_DriverConnected)
        DbkGetVersion(&g_KernelVersion);

    return g_DriverConnected;
}

void DisconnectDriver()
{
    if (g_hDriver != INVALID_HANDLE_VALUE) {
        CloseHandle(g_hDriver);
        g_hDriver = INVALID_HANDLE_VALUE;
    }
    g_DriverConnected = false;
}

// =====================================================================
//  Low-level DBK64 IOCTL helpers
// =====================================================================
bool DbkIoctl(ULONG code, const void* in, ULONG inSize, void* out, ULONG outSize, ULONG* returned)
{
    if (g_hDriver == INVALID_HANDLE_VALUE) return false;
    DWORD br = 0;
    BOOL ok = DeviceIoControl(g_hDriver, code, (LPVOID)in, inSize, out, outSize, &br, NULL);
    if (returned) *returned = br;
    return ok != FALSE;
}

bool DbkReadBytes(ULONG pid, ULONG_PTR addr, void* out, ULONG size)
{
    if (g_hDriver == INVALID_HANDLE_VALUE || pid == 0 || size == 0 || size > DBK_MAX_IO_SIZE || out == NULL)
        return false;

    DBK_READ_REQUEST req;
    req.ProcessId = pid;
    req.Address = addr;
    req.BytesToRead = (USHORT)size;

    ULONG returned = 0;
    return DbkIoctl(IOCTL_CE_READMEMORY, &req, sizeof(req), out, size, &returned) && returned == size;
}

bool DbkWriteBytes(ULONG pid, ULONG_PTR addr, const void* in, ULONG size)
{
    if (g_hDriver == INVALID_HANDLE_VALUE || pid == 0 || size == 0 || size > DBK_MAX_IO_SIZE || in == NULL)
        return false;

    ULONG total = DBK_WRITE_HEADER_SIZE + size;
    std::vector<BYTE> buf(total, 0);
    *(ULONG64*)(&buf[0])  = pid;
    *(ULONG64*)(&buf[8])  = addr;
    *(USHORT*)(&buf[16])  = (USHORT)size;
    memcpy(&buf[DBK_WRITE_HEADER_SIZE], in, size);

    return DbkIoctl(IOCTL_CE_WRITEMEMORY, buf.data(), total, NULL, 0, NULL);
}

// =====================================================================
//  Memory & Patch Operations
// =====================================================================
ULONG GetDataSize(int dataType) {
    switch (dataType) {
    case 3: return 1;   // 1 Byte
    case 2: return 2;   // 2 Bytes
    case 4: return 4;   // 4 Bytes
    case 1: return 4;   // Float
    case 0: return 8;   // 8 Bytes
    case 5: return 8;   // Double
    default: return 4;
    }
}

void WriteMemory(ULONG pid, ULONG_PTR addr, ULONG64 val64, int dataType)
{
    if (g_hDriver == INVALID_HANDLE_VALUE || pid == 0) return;
    ULONG sz = GetDataSize(dataType);
    DbkWriteBytes(pid, addr, &val64, sz);
}

void ReadMemory(ULONG pid, ULONG_PTR addr, ULONG64* outVal, int dataType)
{
    if (g_hDriver == INVALID_HANDLE_VALUE || pid == 0) { *outVal = 0; return; }
    ULONG sz = GetDataSize(dataType);
    ULONG64 v = 0;
    if (!DbkReadBytes(pid, addr, &v, sz)) v = 0;
    *outVal = v;
}

void PatchMemory(ULONG_PTR addr, const UCHAR* pattern, ULONG size)
{
    if (g_hDriver == INVALID_HANDLE_VALUE || g_SelectedPid == 0 || size == 0 || size > 32) return;
    DbkWriteBytes(g_SelectedPid, addr, pattern, size);
}

ULONG64 ParseInputToValue(const char* str, int dataType) {
    if (dataType == 1) { // Float
        float f = (float)atof(str);
        ULONG tmp;
        memcpy(&tmp, &f, sizeof(float));
        return (ULONG64)tmp;
    }
    else if (dataType == 5) { // Double
        double d = atof(str);
        ULONG64 tmp;
        memcpy(&tmp, &d, sizeof(double));
        return tmp;
    }
    else {
        return _strtoui64(str, NULL, 0);
    }
}

void FormatValueToString(ULONG64 val64, int dataType, char* outBuf, size_t maxLen)
{
    if (dataType == 1) { // Float
        ULONG tmp = (ULONG)val64;
        float f;
        memcpy(&f, &tmp, sizeof(float));
        sprintf_s(outBuf, maxLen, "%.2f", f);
    }
    else if (dataType == 5) { // Double
        double d;
        memcpy(&d, &val64, sizeof(double));
        sprintf_s(outBuf, maxLen, "%.2lf", d);
    }
    else if (dataType == 0) { // 8 Bytes / Int64
        sprintf_s(outBuf, maxLen, "%lld", (long long)val64);
    }
    else if (dataType == 3) { // Byte
        sprintf_s(outBuf, maxLen, "%u", (unsigned char)val64);
    }
    else if (dataType == 2) { // 2 Bytes
        sprintf_s(outBuf, maxLen, "%u", (unsigned short)val64);
    }
    else { // 4 Bytes
        sprintf_s(outBuf, maxLen, "%lu", (unsigned long)val64);
    }
}

// Forward declaration
static std::vector<LONG> ParseOffsetList(const char* str);

const ModuleInfo* FindModuleByAddress(ULONG_PTR addr)
{
    for (const auto& mod : g_LoadedModules) {
        if (addr >= mod.BaseAddress && addr < (mod.BaseAddress + mod.Size))
            return &mod;
    }
    return nullptr;
}

ResolvedPointer ResolvePointerSmart(ULONG pid, const char* addressInput, const char* offsetsInput)
{
    ResolvedPointer result;
    if (pid == 0) return result;

    ULONG_PTR baseAddr = (ULONG_PTR)_strtoui64(addressInput, NULL, 0);
    if (baseAddr == 0) return result;

    const ModuleInfo* mod = FindModuleByAddress(baseAddr);
    if (mod) {
        result.BaseModule = mod->ModuleName;
        result.BaseOffset = baseAddr - mod->BaseAddress;
        baseAddr = mod->BaseAddress + result.BaseOffset;
    }
    else {
        result.BaseModule = "Unknown";
        result.BaseOffset = baseAddr;
    }

    result.Offsets = ParseOffsetList(offsetsInput);
    if (result.Offsets.empty()) return result;

    ULONG_PTR current = baseAddr;
    for (size_t i = 0; i < result.Offsets.size(); ++i) {
        ULONG64 val64 = 0;
        ReadMemory(pid, current, &val64, 0); // pointers are 8 bytes
        if (val64 == 0) return result;

        if (i == result.Offsets.size() - 1) {
            result.FinalAddress = (ULONG_PTR)val64 + result.Offsets[i];
            ReadMemory(pid, result.FinalAddress, &result.FinalValue, 0);
            result.Success = true;
        }
        else {
            current = (ULONG_PTR)val64 + result.Offsets[i];
        }
    }
    return result;
}

// =====================================================================
//  Zydis Disassembler Integration
// =====================================================================
std::string GetAutoOffsetForAddress(ULONG_PTR targetAddr)
{
    for (const auto& mod : g_LoadedModules) {
        if (targetAddr >= mod.BaseAddress && targetAddr < (mod.BaseAddress + mod.Size)) {
            ULONG_PTR offset = targetAddr - mod.BaseAddress;
            std::stringstream ss;
            ss << mod.ModuleName << " + 0x" << std::hex << offset;
            return ss.str();
        }
    }
    std::stringstream ss;
    ss << "0x" << std::hex << targetAddr;
    return ss.str();
}

std::string GetZydisDisassembledBytes(ULONG_PTR targetAddr, ULONG instructionCount)
{
    if (instructionCount == 0) instructionCount = 1;

    UCHAR codeBuffer[64] = { 0 };
    for (ULONG offset = 0; offset < 64; offset += 8) {
        ULONG64 val = 0;
        ReadMemory(g_SelectedPid, targetAddr + offset, &val, 5);
        memcpy(codeBuffer + offset, &val, 8);
    }

    ZydisDecoder decoder;
    ZydisDecoderInit(&decoder, ZYDIS_MACHINE_MODE_LONG_64, ZYDIS_STACK_WIDTH_64);

    ZydisDecodedInstruction instruction;
    ZydisDecodedOperand operands[ZYDIS_MAX_OPERAND_COUNT];
    std::string resultBytes;
    bool firstByte = true;

    SIZE_T offset = 0;
    for (ULONG decoded = 0; decoded < instructionCount && offset < sizeof(codeBuffer); decoded++) {
        if (ZydisDecoderDecodeFull(&decoder, codeBuffer + offset, sizeof(codeBuffer) - offset, &instruction, operands) != ZYAN_STATUS_SUCCESS)
            break;

        for (ZyanUSize b = 0; b < instruction.length; b++) {
            char hexByte[8];
            sprintf_s(hexByte, sizeof(hexByte), "0x%02X", codeBuffer[offset + b]);
            if (!firstByte) resultBytes += ", ";
            resultBytes += hexByte;
            firstByte = false;
        }
        offset += instruction.length;
    }

    if (resultBytes.empty()) resultBytes = "0x90, 0x90";
    return resultBytes;
}

ULONG_PTR ResolvePointerPath(ULONG pid, ULONG_PTR baseAddress, const std::vector<LONG>& offsets)
{
    if (pid == 0 || baseAddress == 0) return 0;

    ULONG_PTR currentAddress = baseAddress;
    for (size_t i = 0; i < offsets.size(); ++i) {
        ULONG64 val64 = 0;
        ReadMemory(pid, currentAddress, &val64, 0);
        if (val64 == 0) return 0;

        if (i == offsets.size() - 1) {
            return (ULONG_PTR)val64 + offsets[i];
        }
        else {
            currentAddress = (ULONG_PTR)val64 + offsets[i];
        }
    }
    return currentAddress;
}

// =====================================================================
//  Input parsing helpers
// =====================================================================
static bool ParseHexBytes(const char* str, UCHAR* outBytes, ULONG maxBytes, ULONG* outCount)
{
    ULONG count = 0;
    const char* p = str;
    while (*p && count < maxBytes) {
        while (*p == ' ' || *p == ',') p++;
        if (!*p) break;

        char* end = NULL;
        unsigned long v = strtoul(p, &end, 16);
        if (end == p || v > 0xFF) return false;
        outBytes[count++] = (UCHAR)v;
        p = end;
    }
    *outCount = count;
    return count > 0;
}

static std::vector<LONG> ParseOffsetList(const char* str)
{
    std::vector<LONG> offsets;
    const char* p = str;
    while (*p) {
        while (*p == ' ' || *p == ',') p++;
        if (!*p) break;

        char* end = NULL;
        long v = strtol(p, &end, 0);
        if (end == p) break;
        offsets.push_back(v);
        p = end;
    }
    return offsets;
}

// =====================================================================
//  DBK64 feature helpers (Kernel tab)
// =====================================================================
bool DbkGetVersion(ULONG* version)
{
    if (version) *version = 0;
    ULONG v = 0;
    if (DbkIoctl(IOCTL_CE_GETVERSION, NULL, 0, &v, sizeof(v))) {
        if (version) *version = v;
        return true;
    }
    return false;
}

bool DbkGetCR0(ULONG64* out)
{
    if (out) *out = 0;
    ULONG64 v = 0;
    if (DbkIoctl(IOCTL_CE_GETCR0, NULL, 0, &v, sizeof(v))) { if (out) *out = v; return true; }
    return false;
}

bool DbkGetCR3(ULONG pid, ULONG64* out)
{
    if (out) *out = 0;
    ULONG inputPid = pid;
    ULONG64 v = 0;
    if (DbkIoctl(IOCTL_CE_GETCR3, &inputPid, sizeof(inputPid), &v, sizeof(v))) { if (out) *out = v; return true; }
    return false;
}

bool DbkGetCR4(ULONG64* out)
{
    if (out) *out = 0;
    ULONG64 v = 0;
    if (DbkIoctl(IOCTL_CE_GETCR4, NULL, 0, &v, sizeof(v))) { if (out) *out = v; return true; }
    return false;
}

bool DbkReadMsr(ULONG msr, ULONG64* out)
{
    if (out) *out = 0;
    ULONG64 v = 0;
    if (DbkIoctl(IOCTL_CE_READMSR, &msr, sizeof(msr), &v, sizeof(v))) { if (out) *out = v; return true; }
    return false;
}

bool DbkWriteMsr(ULONG64 msr, ULONG64 value)
{
    DBK_MSR_WRITE req;
    req.Msr = msr;
    req.Value = value;
    return DbkIoctl(IOCTL_CE_WRITEMSR, &req, sizeof(req), NULL, 0, NULL);
}

bool DbkGetIdt(USHORT* limit, ULONG_PTR* base)
{
    if (limit) *limit = 0;
    if (base) *base = 0;
    DBK_SEG_TABLE t = {};
    if (DbkIoctl(IOCTL_CE_GETIDT, NULL, 0, &t, sizeof(t))) {
        if (limit) *limit = t.Limit;
        if (base) *base = t.Base;
        return true;
    }
    return false;
}

bool DbkGetGdt(USHORT* limit, ULONG_PTR* base)
{
    if (limit) *limit = 0;
    if (base) *base = 0;
    DBK_SEG_TABLE t = {};
    if (DbkIoctl(IOCTL_CE_GETGDT, NULL, 0, &t, sizeof(t))) {
        if (limit) *limit = t.Limit;
        if (base) *base = t.Base;
        return true;
    }
    return false;
}

bool DbkReadPhysical(ULONG64 addr, void* out, ULONG size)
{
    if (size == 0 || size > 0x2000 || out == NULL) return false;   // driver maps a 0x2000 view
    DBK_PHYS_RW req;
    req.Address = addr;
    req.Bytes = size;
    return DbkIoctl(IOCTL_CE_READPHYSICALMEMORY, &req, sizeof(req), out, size, NULL);
}

bool DbkWritePhysical(ULONG64 addr, const void* in, ULONG size)
{
    if (size == 0 || size > 0x2000 || in == NULL) return false;
    ULONG total = sizeof(DBK_PHYS_RW) + size;
    std::vector<BYTE> buf(total, 0);
    *(ULONG64*)(&buf[0]) = addr;
    *(ULONG64*)(&buf[8]) = size;
    memcpy(&buf[sizeof(DBK_PHYS_RW)], in, size);
    return DbkIoctl(IOCTL_CE_WRITEPHYSICALMEMORY, buf.data(), total, NULL, 0, NULL);
}

bool DbkAllocNonPaged(ULONG size, ULONG64* out)
{
    if (out) *out = 0;
    ULONG64 addr = 0;
    if (DbkIoctl(IOCTL_CE_ALLOCATEMEM_NONPAGED, &size, sizeof(size), &addr, sizeof(addr))) {
        if (out) *out = addr;
        return addr != 0;
    }
    return false;
}

bool DbkFreeNonPaged(ULONG64 addr)
{
    return DbkIoctl(IOCTL_CE_FREE_NONPAGED, &addr, sizeof(addr), NULL, 0, NULL);
}

bool DbkAllocProcessMem(ULONG pid, ULONG64 size, ULONG64* out)
{
    if (out) *out = 0;
    DBK_ALLOC_PROCESS req = {};
    req.ProcessId = pid;
    req.BaseAddress = 0;
    req.Size = size;
    req.AllocationType = 0x3000;             // MEM_COMMIT | MEM_RESERVE
    req.Protect = 0x40;                      // PAGE_EXECUTE_READWRITE
    ULONG64 addr = 0;
    if (DbkIoctl(IOCTL_CE_ALLOCATEMEM, &req, sizeof(req), &addr, sizeof(addr))) {
        if (out) *out = addr;
        return addr != 0;
    }
    return false;
}

bool DbkSuspendProcess(ULONG pid)
{
    return DbkIoctl(IOCTL_CE_SUSPENDPROCESS, &pid, sizeof(pid), NULL, 0, NULL);
}

bool DbkResumeProcess(ULONG pid)
{
    return DbkIoctl(IOCTL_CE_RESUMEPROCESS, &pid, sizeof(pid), NULL, 0, NULL);
}

bool DbkOpenProcessHandle(ULONG pid, ULONG64* handle, UCHAR* special)
{
    if (handle) *handle = 0;
    if (special) *special = 0;
    DBK_OPENPROCESS_OUT out = {};
    if (DbkIoctl(IOCTL_CE_OPENPROCESS, &pid, sizeof(pid), &out, sizeof(out))) {
        if (handle) *handle = out.Handle;
        if (special) *special = out.Special;
        return true;
    }
    return false;
}

bool DbkGetPEPROCESS(ULONG pid, ULONG64* out)
{
    if (out) *out = 0;
    ULONG64 v = 0;
    if (DbkIoctl(IOCTL_CE_GETPEPROCESS, &pid, sizeof(pid), &v, sizeof(v))) { if (out) *out = v; return true; }
    return false;
}

bool DbkQueryVirtualMemory(ULONG pid, ULONG_PTR addr, ULONG_PTR* length, ULONG* protection)
{
    if (length) *length = 0;
    if (protection) *protection = 0;
    if (g_hDriver == INVALID_HANDLE_VALUE || pid == 0)
        return false;

    // Input and output share the same buffered buffer: the driver reads the
    // {pid, addr} header, then overwrites it with {length, protection}.
    DBK_QUERY_VMEM_INOUT buf = {};
    buf.In.ProcessId = pid;
    buf.In.StartAddress = addr;

    if (!DbkIoctl(IOCTL_CE_QUERY_VIRTUAL_MEMORY, &buf, sizeof(buf), &buf, sizeof(buf)))
        return false;

    if (length) *length = (ULONG_PTR)buf.Out.Length;
    if (protection) *protection = buf.Out.Protection;
    return true;
}

// =====================================================================
//  Freeze / Cheat Table Loop
// =====================================================================
void FreezeLoop()
{
    while (g_FreezeRunning) {
        if (g_hDriver != INVALID_HANDLE_VALUE) {
            std::lock_guard<std::mutex> lock(g_CheatTableLock);
            for (auto& item : g_CheatTable) {
                if (item.Enabled && item.Pid != 0) {
                    WriteMemory(item.Pid, item.Address, item.Value64, item.DataType);
                }
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
}

// =====================================================================
//  Process & Module Enumeration (user-mode Toolhelp, no driver needed)
// =====================================================================
void RefreshProcessList()
{
    g_ProcessList.clear();

    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return;

    PROCESSENTRY32W pe;
    pe.dwSize = sizeof(pe);
    if (Process32FirstW(snap, &pe)) {
        do {
            ProcessInfo info;
            info.ProcessId = pe.th32ProcessID;
            memset(info.Name, 0, sizeof(info.Name));
            WideCharToMultiByte(CP_UTF8, 0, pe.szExeFile, -1, info.Name, (int)sizeof(info.Name) - 1, NULL, NULL);
            g_ProcessList.push_back(info);
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
}

void RefreshModules()
{
    g_LoadedModules.clear();
    if (g_SelectedPid == 0) return;

    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, g_SelectedPid);
    if (snap == INVALID_HANDLE_VALUE) return;

    MODULEENTRY32W me;
    me.dwSize = sizeof(me);
    if (Module32FirstW(snap, &me)) {
        do {
            ModuleInfo info;
            memset(&info, 0, sizeof(info));
            WideCharToMultiByte(CP_UTF8, 0, me.szModule, -1, info.ModuleName, (int)sizeof(info.ModuleName) - 1, NULL, NULL);
            WideCharToMultiByte(CP_UTF8, 0, me.szExePath, -1, info.FullPath, (int)sizeof(info.FullPath) - 1, NULL, NULL);
            info.BaseAddress = (ULONG_PTR)me.modBaseAddr;
            info.Size = me.modBaseSize;
            g_LoadedModules.push_back(info);
        } while (Module32NextW(snap, &me));
    }
    CloseHandle(snap);
}

// =====================================================================
//  Memory region enumeration & scanning
//  Regions are enumerated through the DBK64 driver (IOCTL_CE_QUERY_VIRTUAL_MEMORY)
//  so protected processes (svchost/PPL) and guarded games (anti-cheat) are
//  scanned too; the same driver performs the actual reads.
// =====================================================================
struct RegionInfo {
    ULONG_PTR Start;
    ULONG_PTR End;
};

static ULONG_PTR MaxAddr(ULONG_PTR a, ULONG_PTR b) { return a > b ? a : b; }
static ULONG_PTR MinAddr(ULONG_PTR a, ULONG_PTR b) { return a < b ? a : b; }

// Kernel-reported protection values (see FindFirstDifferentAddress in memscan.c)
static bool IsReadableProtect(DWORD protect)
{
    // PAGE_EXECUTE_READ (0x20) and PAGE_EXECUTE_READWRITE (0x40) are readable;
    // PAGE_NOACCESS (0x01) / unknown are not.
    return (protect == PAGE_EXECUTE_READ || protect == PAGE_EXECUTE_READWRITE);
}

// Enumerate readable regions by walking the target's page tables in the kernel.
static std::vector<RegionInfo> EnumerateRegions(ULONG pid, ULONG_PTR rangeStart, ULONG_PTR rangeEnd)
{
    std::vector<RegionInfo> regions;

    ULONG_PTR cursor = rangeStart & ~(ULONG_PTR)0xFFF; // page-aligned start
    ULONG guard = 0;

    while (cursor < rangeEnd) {
        if (++guard > 1 << 22) break; // safety: ~4M queries max

        ULONG_PTR length = 0;
        ULONG protection = 0;
        if (!DbkQueryVirtualMemory(pid, cursor, &length, &protection))
            break; // no more regions (or driver call failed)

        if (length == 0)
            break;

        ULONG_PTR base = cursor & ~(ULONG_PTR)0xFFF;
        ULONG_PTR end = base + length;
        if (end <= base)
            break; // overflow / wrapped past end of address space

        if (IsReadableProtect(protection)) {
            ULONG_PTR s = MaxAddr(base, rangeStart);
            ULONG_PTR e = MinAddr(end, rangeEnd);
            if (s < e) regions.push_back({ s, e });
        }

        cursor = end;
    }

    return regions;
}

// User-mode fallback when the kernel walker returns nothing (e.g. querying the
// kernel page tables is unavailable on this OS build).
static std::vector<RegionInfo> EnumerateRegionsUserMode(ULONG pid, ULONG_PTR rangeStart, ULONG_PTR rangeEnd)
{
    std::vector<RegionInfo> regions;
    HANDLE h = OpenProcess(PROCESS_QUERY_INFORMATION, FALSE, pid);
    if (!h) return regions;

    MEMORY_BASIC_INFORMATION mbi;
    ULONG_PTR addr = rangeStart;
    while (addr < rangeEnd) {
        SIZE_T queried = VirtualQueryEx(h, (LPCVOID)addr, &mbi, sizeof(mbi));
        if (!queried) break;

        ULONG_PTR regionEnd = (ULONG_PTR)mbi.BaseAddress + (ULONG_PTR)mbi.RegionSize;
        bool readable = (mbi.State == MEM_COMMIT) &&
            !(mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) &&
            (mbi.Protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY |
                            PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) != 0;

        if (readable) {
            ULONG_PTR s = MaxAddr((ULONG_PTR)mbi.BaseAddress, rangeStart);
            ULONG_PTR e = MinAddr(regionEnd, rangeEnd);
            if (s < e) regions.push_back({ s, e });
        }

        if (regionEnd <= (ULONG_PTR)mbi.BaseAddress) break; // overflow guard
        addr = regionEnd;
    }

    CloseHandle(h);
    return regions;
}

void AsyncFirstScanWorker(ULONG targetPid, int dataType, ULONG64 searchVal64, bool useRange, ULONG_PTR rangeStart, ULONG_PTR rangeEnd, bool allowUnaligned)
{
    g_IsScanning = true;
    g_ScanProgress = 0;
    g_ScanTruncated = false;
    {
        std::lock_guard<std::mutex> lock(g_ScanResultsLock);
        g_ScanResults.clear();
    }

    if (g_hDriver == INVALID_HANDLE_VALUE || targetPid == 0) {
        g_IsScanning = false;
        return;
    }

    ULONG sz = GetDataSize(dataType);
    ULONG_PTR rStart = useRange ? rangeStart : 0x10000;
    ULONG_PTR rEnd = useRange ? rangeEnd : 0x7FFFFFFFFFFFULL;

    std::vector<RegionInfo> regions = EnumerateRegions(targetPid, rStart, rEnd);
    if (regions.empty())
        regions = EnumerateRegionsUserMode(targetPid, rStart, rEnd);

    std::vector<ULONG_PTR> results;
    std::vector<BYTE> chunk(DBK_MAX_IO_SIZE);   // max the driver can read per IOCTL
    ULONG64 totalBytes = 0, doneBytes = 0;
    for (auto& r : regions) totalBytes += (ULONG64)(r.End - r.Start);

    for (auto& r : regions) {
        ULONG_PTR cur = r.Start;
        while (cur < r.End) {
            ULONG_PTR remaining = r.End - cur;
            ULONG chunkSize = (ULONG)MinAddr(remaining, (ULONG_PTR)chunk.size());

            if (DbkReadBytes(targetPid, cur, chunk.data(), chunkSize)) {
                ULONG maxOff = (chunkSize >= sz) ? (chunkSize - sz + 1) : 0;
                ULONG step = allowUnaligned ? 1 : sz;
                for (ULONG off = 0; off < maxOff; off += step) {
                    if (memcmp(&chunk[off], &searchVal64, sz) == 0) {
                        results.push_back(cur + off);
                        if (results.size() >= MAX_SCAN_RESULTS) {
                            g_ScanTruncated = true;
                            goto scan_done;
                        }
                    }
                }
            }

            cur += chunkSize;
            doneBytes += chunkSize;
            g_ScanProgress = totalBytes ? (int)(doneBytes * 100 / totalBytes) : 100;
        }
    }

scan_done:
    {
        std::lock_guard<std::mutex> lock(g_ScanResultsLock);
        g_ScanResults = std::move(results);
    }
    g_ScanVersion++;
    g_ScanProgress = 100;
    g_IsScanning = false;
}

void AsyncNextScanWorker(ULONG targetPid, int dataType, ULONG64 searchVal64, std::vector<ULONG_PTR> prevResults)
{
    g_IsScanning = true;
    g_ScanProgress = 0;
    g_ScanTruncated = false;

    if (g_hDriver == INVALID_HANDLE_VALUE || prevResults.empty()) {
        g_IsScanning = false;
        return;
    }

    ULONG sz = GetDataSize(dataType);
    std::vector<ULONG_PTR> results;
    results.reserve(prevResults.size());

    ULONG64 total = (ULONG64)prevResults.size(), done = 0;
    for (auto a : prevResults) {
        ULONG64 v = 0;
        if (DbkReadBytes(targetPid, a, &v, sz) && memcmp(&v, &searchVal64, sz) == 0)
            results.push_back(a);
        done++;
        g_ScanProgress = total ? (int)(done * 100 / total) : 100;
    }

    {
        std::lock_guard<std::mutex> lock(g_ScanResultsLock);
        g_ScanResults = std::move(results);
    }
    g_ScanVersion++;
    g_ScanProgress = 100;
    g_IsScanning = false;
}

void StartFirstScan()
{
    if (g_SelectedPid == 0 || g_IsScanning) return;
    ULONG64 val64 = ParseInputToValue(g_ScanValueInput, g_SelectedDataType);
    ULONG_PTR rStart = 0x10000;
    ULONG_PTR rEnd = 0x7FFFFFFFFFFFULL;
    if (g_UseScanRange) {
        rStart = (ULONG_PTR)_strtoui64(g_ScanRangeStart, NULL, 0);
        rEnd = (ULONG_PTR)_strtoui64(g_ScanRangeEnd, NULL, 0);
    }
    std::thread(AsyncFirstScanWorker, g_SelectedPid, g_SelectedDataType, val64, g_UseScanRange, rStart, rEnd, g_AllowUnaligned).detach();
}

void StartNextScan()
{
    if (g_SelectedPid == 0 || g_IsScanning) return;

    std::vector<ULONG_PTR> currentCopy;
    {
        std::lock_guard<std::mutex> lock(g_ScanResultsLock);
        currentCopy = g_ScanResults;
    }
    if (currentCopy.empty()) return;

    ULONG64 val64 = ParseInputToValue(g_ScanValueInput, g_SelectedDataType);
    std::thread(AsyncNextScanWorker, g_SelectedPid, g_SelectedDataType, val64, currentCopy).detach();
}

void ExportResultsToFile()
{
    std::vector<ULONG_PTR> copy;
    {
        std::lock_guard<std::mutex> lock(g_ScanResultsLock);
        copy = g_ScanResults;
    }

    std::ofstream outFile("ScanResultsExport.txt");
    if (!outFile.is_open()) return;
    for (auto addr : copy) {
        outFile << "0x" << std::hex << addr << "\n";
    }
    outFile.close();
}

// =====================================================================
//  Kernel tab helpers (UI actions)
// =====================================================================
static void FormatPhysDump(const BYTE* data, ULONG size, ULONG64 baseAddr, char* out, size_t outMax)
{
    std::stringstream ss;
    for (ULONG row = 0; row < size; row += 16) {
        char ascii[17] = "................";
        ss << std::hex << std::uppercase << std::setw(16) << std::setfill('0') << (baseAddr + row) << "  ";

        ULONG lineLen = (size - row) < 16 ? (size - row) : 16;
        for (ULONG i = 0; i < 16; i++) {
            if (i < lineLen) {
                ss << std::setw(2) << (unsigned)data[row + i] << " ";
                BYTE c = data[row + i];
                if (c >= 0x20 && c < 0x7F) ascii[i] = (char)c;
            }
            else {
                ss << "   ";
            }
        }
        ss << " |" << ascii << "|";
        if (row + 16 < size) ss << "\n";
    }
    strncpy_s(out, outMax, ss.str().c_str(), _TRUNCATE);
}

// =====================================================================
//  WinMain / Entry Point
// =====================================================================
int APIENTRY WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow)
{
    UNREFERENCED_PARAMETER(hInstance);
    UNREFERENCED_PARAMETER(hPrevInstance);
    UNREFERENCED_PARAMETER(lpCmdLine);
    UNREFERENCED_PARAMETER(nCmdShow);

    EnableSeDebugPrivilege();

    LoadAndStartDriver();
    bool driverConnected = ConnectDriver();
    if (!driverConnected) {
        MessageBoxA(NULL, "Failed to connect to DBK64 (DBKKernel driver)!\nRun as Administrator and make sure DBK64.sys is next to the exe.",
            "Warning", MB_OK | MB_ICONWARNING);
    }

    if (!glfwInit()) return 1;

    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    GLFWwindow* window = glfwCreateWindow(1280, 720, "Imno - DBK64 Kernel Suite", NULL, NULL);
    if (!window) {
        glfwTerminate();
        return 1;
    }
    g_Window = window;
    g_hWnd = glfwGetWin32Window(window);

    // Setup Direct3D 11
    DXGI_SWAP_CHAIN_DESC sd;
    ZeroMemory(&sd, sizeof(sd));
    sd.BufferCount = 2;
    sd.BufferDesc.Width = 0;
    sd.BufferDesc.Height = 0;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferDesc.RefreshRate.Numerator = 60;
    sd.BufferDesc.RefreshRate.Denominator = 1;
    sd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = g_hWnd;
    sd.SampleDesc.Count = 1;
    sd.SampleDesc.Quality = 0;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    UINT createDeviceFlags = 0;
    D3D_FEATURE_LEVEL featureLevel;
    const D3D_FEATURE_LEVEL featureLevelArray[2] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0, };
    ID3D11Device* pd3dDevice = NULL;
    ID3D11DeviceContext* pd3dDeviceContext = NULL;
    IDXGISwapChain* pSwapChain = NULL;

    if (D3D11CreateDeviceAndSwapChain(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, createDeviceFlags, featureLevelArray, 2,
        D3D11_SDK_VERSION, &sd, &pSwapChain, &pd3dDevice, &featureLevel, &pd3dDeviceContext) != S_OK)
    {
        glfwDestroyWindow(window);
        glfwTerminate();
        return 1;
    }

    ID3D11RenderTargetView* mainRenderTargetView = NULL;
    auto CreateRT = [&]() {
        if (mainRenderTargetView) { mainRenderTargetView->Release(); mainRenderTargetView = NULL; }
        ID3D11Texture2D* pBackBuffer = NULL;
        pSwapChain->GetBuffer(0, IID_PPV_ARGS(&pBackBuffer));
        pd3dDevice->CreateRenderTargetView(pBackBuffer, NULL, &mainRenderTargetView);
        pBackBuffer->Release();
    };
    CreateRT();

    // Setup Dear ImGui context
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO(); (void)io;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

    ImGui::StyleColorsDark();

    ImGui_ImplGlfw_InitForOther(window, true);
    ImGui_ImplDX11_Init(pd3dDevice, pd3dDeviceContext);

    // Initial fetch of processes & kernel info
    if (driverConnected) {
        RefreshProcessList();
        DbkGetVersion(&g_KernelVersion);
    }

    g_FreezeThread = std::thread(FreezeLoop);

    int lastWidth = 0, lastHeight = 0;
    glfwGetFramebufferSize(window, &lastWidth, &lastHeight);

    while (!glfwWindowShouldClose(window))
    {
        glfwPollEvents();

        // Handle resize - unbind RTV before ResizeBuffers
        int fbWidth, fbHeight;
        glfwGetFramebufferSize(window, &fbWidth, &fbHeight);
        if (fbWidth > 0 && fbHeight > 0 && (fbWidth != lastWidth || fbHeight != lastHeight)) {
            lastWidth = fbWidth;
            lastHeight = fbHeight;
            pd3dDeviceContext->OMSetRenderTargets(0, nullptr, nullptr);
            if (mainRenderTargetView) { mainRenderTargetView->Release(); mainRenderTargetView = NULL; }
            ImGui_ImplDX11_InvalidateDeviceObjects();
            pSwapChain->ResizeBuffers(0, (UINT)fbWidth, (UINT)fbHeight, DXGI_FORMAT_UNKNOWN, 0);
            CreateRT();
            ImGui_ImplDX11_CreateDeviceObjects();
        }
        if (fbWidth == 0 || fbHeight == 0) {
            Sleep(10);
            continue;
        }

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        ImGui::SetNextWindowPos(ImVec2(0, 0), ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImVec2((float)fbWidth, (float)fbHeight), ImGuiCond_Always);
        ImGui::Begin("Imno - DBK64 Kernel Suite", NULL, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoTitleBar);

        // Top Bar: Driver Status Active/Inactive & Process Combo
        driverConnected = (g_hDriver != INVALID_HANDLE_VALUE);
        if (driverConnected) {
            ImGui::TextColored({ 0.0f, 1.0f, 0.0f, 1.0f }, "[DBK64 Driver: ACTIVE]");
        }
        else {
            ImGui::TextColored({ 1.0f, 0.0f, 0.0f, 1.0f }, "[DBK64 Driver: INACTIVE]");
            ImGui::SameLine();
            if (ImGui::Button("Reconnect")) {
                LoadAndStartDriver();
                driverConnected = ConnectDriver();
                if (driverConnected) RefreshProcessList();
            }
        }

        ImGui::SameLine();
        if (ImGui::Button("Refresh", ImVec2(80, 25))) {
            RefreshProcessList();
        }

        ImGui::SameLine();
        ImGui::SetNextItemWidth(150);
        ImGui::InputText("Filter", g_ProcessFilter, sizeof(g_ProcessFilter));
        ImGui::SameLine();

        ImGui::SetNextItemWidth(260);
        char comboLabel[128];
        strcpy_s(comboLabel, sizeof(comboLabel), "Select Target Process...");
        if (g_SelectedPid != 0) {
            const char* foundName = NULL;
            for (const auto& p : g_ProcessList) {
                if (p.ProcessId == g_SelectedPid) { foundName = p.Name; break; }
            }
            if (foundName) sprintf_s(comboLabel, sizeof(comboLabel), "%s (%u)", foundName, g_SelectedPid);
            else sprintf_s(comboLabel, sizeof(comboLabel), "PID %u", g_SelectedPid);
        }
        if (ImGui::BeginCombo("##proccombo", comboLabel)) {
            for (const auto& p : g_ProcessList) {
                if (g_ProcessFilter[0] != '\0' && strstr(p.Name, g_ProcessFilter) == NULL) continue;
                char label[128];
                sprintf_s(label, sizeof(label), "%s (%u)", p.Name, p.ProcessId);
                bool isSelected = (g_SelectedPid == p.ProcessId);
                if (ImGui::Selectable(label, isSelected)) {
                    g_SelectedPid = p.ProcessId;
                    RefreshModules();
                }
                if (isSelected) ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }

        ImGui::SameLine();
        if (g_SelectedPid != 0) {
            ImGui::TextColored({ 0.0f, 1.0f, 0.0f, 1.0f }, "PID: %u", g_SelectedPid);
        }

        ImGui::Separator();

        // Tabs
        if (ImGui::BeginTabBar("ImnoTabs"))
        {
            // TAB 1: MEMORY SCANNER
            if (ImGui::BeginTabItem("Memory Scanner"))
            {
                ImGui::Text("Data Type:");
                ImGui::SameLine();
                ImGui::RadioButton("4 Bytes", &g_SelectedDataType, 4); ImGui::SameLine();
                ImGui::RadioButton("2 Bytes", &g_SelectedDataType, 2); ImGui::SameLine();
                ImGui::RadioButton("1 Byte", &g_SelectedDataType, 3); ImGui::SameLine();
                ImGui::RadioButton("Float", &g_SelectedDataType, 1); ImGui::SameLine();
                ImGui::RadioButton("Double", &g_SelectedDataType, 5); ImGui::SameLine();
                ImGui::RadioButton("8 Bytes", &g_SelectedDataType, 0);

                ImGui::Spacing();
                ImGui::Text("Scan Value:");
                ImGui::SameLine();
                ImGui::SetNextItemWidth(200);
                ImGui::InputText("##scanval", g_ScanValueInput, sizeof(g_ScanValueInput));
                ImGui::SameLine();

                if (!g_IsScanning) {
                    if (ImGui::Button("First Scan", ImVec2(110, 30))) {
                        StartFirstScan();
                    }
                    ImGui::SameLine();
                    if (ImGui::Button("Next Scan", ImVec2(110, 30))) {
                        StartNextScan();
                    }
                    ImGui::SameLine();
                    if (ImGui::Button("Export Results", ImVec2(120, 30))) {
                        ExportResultsToFile();
                    }
                }
                else {
                    ImGui::TextDisabled("Scanning in progress...");
                }

                ImGui::Checkbox("Unaligned Scan (slower)", &g_AllowUnaligned);
                ImGui::SameLine();
                ImGui::Checkbox("Scan Range", &g_UseScanRange);
                if (g_UseScanRange) {
                    ImGui::SetNextItemWidth(140);
                    ImGui::InputText("Start", g_ScanRangeStart, sizeof(g_ScanRangeStart));
                    ImGui::SameLine();
                    ImGui::SetNextItemWidth(140);
                    ImGui::InputText("End", g_ScanRangeEnd, sizeof(g_ScanRangeEnd));
                }

                if (g_IsScanning) {
                    ImGui::ProgressBar((float)g_ScanProgress / 100.0f, ImVec2(-1, 0), "Scanning Process Memory (kernel reads)...");
                }

                ImGui::Spacing();
                ImGui::Separator();

                // UI-side snapshot of the scan results
                static std::vector<ULONG_PTR> s_DisplayAddrs;
                static std::vector<ULONG64>   s_DisplayVals;
                static unsigned               s_DisplayVersion = 0xFFFFFFFF;
                static bool                   s_NeedValueRefresh = true;
                static ULONGLONG              s_LastValueRefresh = 0;

                if (!g_IsScanning && s_DisplayVersion != g_ScanVersion) {
                    std::lock_guard<std::mutex> lock(g_ScanResultsLock);
                    s_DisplayAddrs = g_ScanResults;
                    s_DisplayVersion = g_ScanVersion;
                    s_DisplayVals.assign(s_DisplayAddrs.size(), 0);
                    s_NeedValueRefresh = true;
                }

                ImGui::Text("Scan Results Found: %zu", s_DisplayAddrs.size());
                if (!g_IsScanning && g_ScanTruncated) {
                    ImGui::TextColored({ 1.0f, 0.6f, 0.0f, 1.0f },
                        "Results truncated at %d - use Next Scan or a narrower range!", MAX_SCAN_RESULTS);
                }

                ImGui::BeginChild("ResultsChild", ImVec2(0, 350), true);
                {
                    bool doRefresh = !g_IsScanning &&
                        (s_NeedValueRefresh || (GetTickCount64() - s_LastValueRefresh > 400));

                    ImGuiListClipper clipper;
                    clipper.Begin((int)s_DisplayAddrs.size());
                    while (clipper.Step())
                    {
                        for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; i++) {
                            if (doRefresh) {
                                ReadMemory(g_SelectedPid, s_DisplayAddrs[i], &s_DisplayVals[i], g_SelectedDataType);
                            }

                            char valStr[64];
                            FormatValueToString(s_DisplayVals[i], g_SelectedDataType, valStr, sizeof(valStr));

                            char label[160];
                            sprintf_s(label, sizeof(label), "0x%llX : %s##res%d", s_DisplayAddrs[i], valStr, i);
                            if (ImGui::Selectable(label)) {
                                std::lock_guard<std::mutex> lock(g_CheatTableLock);
                                std::string autoDesc = GetAutoOffsetForAddress(s_DisplayAddrs[i]);
                                g_CheatTable.push_back({ s_DisplayAddrs[i], s_DisplayVals[i], false, g_SelectedDataType, g_SelectedPid, "" });
                                strcpy_s(g_CheatTable.back().Description, sizeof(g_CheatTable.back().Description), autoDesc.c_str());
                            }
                        }
                    }
                    if (doRefresh) {
                        s_LastValueRefresh = GetTickCount64();
                        s_NeedValueRefresh = false;
                    }
                }
                ImGui::EndChild();

                ImGui::EndTabItem();
            }

            // TAB 2: CHEAT TABLE
            if (ImGui::BeginTabItem("Cheat Table"))
            {
                ImGui::Text("Cheat Table (Active Freeze & Patches):");

                ImGui::BeginChild("CheatTableChild", ImVec2(0, 300), true);
                {
                    std::lock_guard<std::mutex> lock(g_CheatTableLock);
                    for (int i = 0; i < (int)g_CheatTable.size(); i++) {
                        ImGui::PushID(i);

                        bool isEnabled = g_CheatTable[i].Enabled;
                        if (ImGui::Checkbox("##en", &isEnabled)) {
                            g_CheatTable[i].Enabled = isEnabled;
                        }
                        ImGui::SameLine();

                        ImGui::Text("0x%llX [PID %u]", g_CheatTable[i].Address, g_CheatTable[i].Pid);
                        ImGui::SameLine(220);

                        char valStr[64];
                        FormatValueToString(g_CheatTable[i].Value64, g_CheatTable[i].DataType, valStr, sizeof(valStr));

                        ImGui::SetNextItemWidth(140);
                        if (ImGui::InputText("##val", valStr, sizeof(valStr))) {
                            g_CheatTable[i].Value64 = ParseInputToValue(valStr, g_CheatTable[i].DataType);
                        }

                        ImGui::SameLine(380);
                        ImGui::SetNextItemWidth(180);
                        ImGui::InputText("##desc", g_CheatTable[i].Description, sizeof(g_CheatTable[i].Description));

                        ImGui::SameLine(570);
                        if (ImGui::Button("Remove")) {
                            g_CheatTable.erase(g_CheatTable.begin() + i--);
                        }

                        ImGui::PopID();
                    }
                }
                ImGui::EndChild();

                ImGui::EndTabItem();
            }

            // TAB 3: MODULES
            if (ImGui::BeginTabItem("Modules"))
            {
                if (ImGui::Button("Refresh Modules", ImVec2(150, 30))) {
                    RefreshModules();
                }
                ImGui::SameLine();
                ImGui::SetNextItemWidth(250);
                ImGui::InputText("Filter Modules", g_ModuleFilter, sizeof(g_ModuleFilter));

                ImGui::BeginChild("ModulesChild", ImVec2(0, 400), true);
                for (const auto& mod : g_LoadedModules) {
                    if (g_ModuleFilter[0] != '\0' && strstr(mod.ModuleName, g_ModuleFilter) == NULL) continue;
                    ImGui::Text("%s | Base: 0x%llX | Size: 0x%X", mod.ModuleName, mod.BaseAddress, mod.Size);
                    ImGui::TextWrapped("Path: %s", mod.FullPath);
                    ImGui::Separator();
                }
                ImGui::EndChild();

                ImGui::EndTabItem();
            }

            // TAB 4: PATCHER & POINTERS
            if (ImGui::BeginTabItem("Patcher & Pointers"))
            {
                ImGui::Text("Byte Patcher (reads/disassembles and overwrites bytes at an address):");
                ImGui::SetNextItemWidth(220);
                ImGui::InputText("##patchaddr", g_PatchAddressInput, sizeof(g_PatchAddressInput));
                ImGui::SameLine();
                if (ImGui::Button("Read Bytes", ImVec2(110, 25)) && g_SelectedPid != 0) {
                    ULONG_PTR addr = (ULONG_PTR)_strtoui64(g_PatchAddressInput, NULL, 0);
                    if (addr) {
                        std::string bytes = GetZydisDisassembledBytes(addr, 1);
                        strcpy_s(g_PatchPatternInput, sizeof(g_PatchPatternInput), bytes.c_str());
                    }
                }

                ImGui::SetNextItemWidth(340);
                ImGui::InputText("##patchpattern", g_PatchPatternInput, sizeof(g_PatchPatternInput));
                ImGui::SameLine();
                if (ImGui::Button("Patch Memory", ImVec2(120, 25)) && g_SelectedPid != 0) {
                    ULONG_PTR addr = (ULONG_PTR)_strtoui64(g_PatchAddressInput, NULL, 0);
                    UCHAR bytes[32];
                    ULONG byteCount = 0;
                    if (addr && ParseHexBytes(g_PatchPatternInput, bytes, sizeof(bytes), &byteCount)) {
                        PatchMemory(addr, bytes, byteCount);
                    }
                }

                ImGui::Spacing();
                ImGui::Separator();

                ImGui::Text("Smart Pointer Resolver (address + comma-separated offsets):");
                ImGui::TextWrapped("Enter any address (e.g. module+offset or raw 0x...). Base module auto-detected.");
                ImGui::SetNextItemWidth(220);
                ImGui::InputText("Address", g_PointerBaseInput, sizeof(g_PointerBaseInput));
                ImGui::SameLine();
                ImGui::SetNextItemWidth(340);
                ImGui::InputText("Offsets (e.g. 0x10, 0x24, 0x8)", g_PointerOffsetInput, sizeof(g_PointerOffsetInput));
                ImGui::SameLine();
                if (ImGui::Button("Resolve", ImVec2(100, 25)) && g_SelectedPid != 0) {
                    g_LastResolved = ResolvePointerSmart(g_SelectedPid, g_PointerBaseInput, g_PointerOffsetInput);
                    if (g_LastResolved.Success) {
                        g_ResolvedAddress = g_LastResolved.FinalAddress;
                        g_ResolvedValue = g_LastResolved.FinalValue;
                    }
                    else {
                        g_ResolvedAddress = 0;
                        g_ResolvedValue = 0;
                    }
                }

                if (g_ResolvedAddress && g_LastResolved.Success) {
                    ImGui::Separator();
                    ImGui::Text("Base Module: %s", g_LastResolved.BaseModule.c_str());
                    ImGui::Text("Base Offset: 0x%llX", g_LastResolved.BaseOffset);
                    ImGui::Text("Offsets: ");
                    ImGui::SameLine();
                    for (size_t i = 0; i < g_LastResolved.Offsets.size(); ++i) {
                        if (i > 0) ImGui::SameLine(); ImGui::Text("0x%X", g_LastResolved.Offsets[i]);
                    }
                    ImGui::Text("Final Address: 0x%llX", g_ResolvedAddress);
                    char resolvedStr[64];
                    FormatValueToString(g_ResolvedValue, 0, resolvedStr, sizeof(resolvedStr));
                    ImGui::Text("Value: %s (0x%llX)", resolvedStr, g_ResolvedValue);
                }

                ImGui::EndTabItem();
            }

            // TAB 5: KERNEL (DBK64 full feature set)
            if (ImGui::BeginTabItem("Kernel"))
            {
                ImGui::BeginChild("KernelChild", ImVec2(0, 0), false);

                if (ImGui::CollapsingHeader("Driver Info", ImGuiTreeNodeFlags_DefaultOpen)) {
                    ImGui::Text("Driver version: %u (expected %u)", g_KernelVersion, DBK_VERSION_EXPECTED);
                    if (ImGui::Button("Re-read version", ImVec2(140, 24))) {
                        DbkGetVersion(&g_KernelVersion);
                    }
                }

                if (ImGui::CollapsingHeader("Control Registers", ImGuiTreeNodeFlags_DefaultOpen)) {
                    if (ImGui::Button("Read CR0/CR3/CR4", ImVec2(140, 24))) {
                        DbkGetCR0(&g_KernelCR0);
                        DbkGetCR4(&g_KernelCR4);
                        DbkGetCR3(g_SelectedPid ? g_SelectedPid : GetCurrentProcessId(), &g_KernelCR3);
                    }
                    ImGui::Text("CR0: 0x%llX", g_KernelCR0);
                    ImGui::Text("CR3: 0x%llX (selected process)", g_KernelCR3);
                    ImGui::Text("CR4: 0x%llX", g_KernelCR4);
                }

                if (ImGui::CollapsingHeader("MSR (Model Specific Registers)")) {
                    ImGui::SetNextItemWidth(200);
                    ImGui::InputText("MSR index", g_MsrReadInput, sizeof(g_MsrReadInput));
                    ImGui::SameLine();
                    if (ImGui::Button("Read MSR", ImVec2(90, 24))) {
                        ULONG msr = (ULONG)_strtoui64(g_MsrReadInput, NULL, 0);
                        DbkReadMsr(msr, &g_KernelMsrValue);
                    }
                    ImGui::Text("Read value: 0x%llX", g_KernelMsrValue);

                    ImGui::Separator();
                    ImGui::SetNextItemWidth(200);
                    ImGui::InputText("MSR index", g_MsrWriteMsr, sizeof(g_MsrWriteMsr));
                    ImGui::SetNextItemWidth(200);
                    ImGui::InputText("New value", g_MsrWriteValue, sizeof(g_MsrWriteValue));
                    ImGui::SameLine();
                    if (ImGui::Button("Write MSR", ImVec2(90, 24))) {
                        ULONG64 msr = _strtoui64(g_MsrWriteMsr, NULL, 0);
                        ULONG64 val = _strtoui64(g_MsrWriteValue, NULL, 0);
                        if (DbkWriteMsr(msr, val))
                            strcpy_s(g_KernelStatus, sizeof(g_KernelStatus), "MSR written OK");
                        else
                            strcpy_s(g_KernelStatus, sizeof(g_KernelStatus), "MSR write failed");
                    }
                    if (g_KernelStatus[0]) ImGui::Text("%s", g_KernelStatus);
                }

                if (ImGui::CollapsingHeader("IDT / GDT")) {
                    if (ImGui::Button("Read IDT & GDT", ImVec2(140, 24))) {
                        DbkGetIdt(&g_KernelIdt.Limit, &g_KernelIdt.Base);
                        DbkGetGdt(&g_KernelGdt.Limit, &g_KernelGdt.Base);
                    }
                    ImGui::Text("IDT: limit 0x%X, base 0x%llX", g_KernelIdt.Limit, g_KernelIdt.Base);
                    ImGui::Text("GDT: limit 0x%X, base 0x%llX", g_KernelGdt.Limit, g_KernelGdt.Base);
                }

                if (ImGui::CollapsingHeader("Physical Memory")) {
                    ImGui::SetNextItemWidth(200);
                    ImGui::InputText("Physical address", g_PhysReadAddr, sizeof(g_PhysReadAddr));
                    ImGui::SetNextItemWidth(120);
                    ImGui::InputText("Bytes", g_PhysReadSize, sizeof(g_PhysReadSize));
                    ImGui::SameLine();
                    if (ImGui::Button("Read Physical", ImVec2(110, 24))) {
                        ULONG64 addr = _strtoui64(g_PhysReadAddr, NULL, 0);
                        ULONG size = (ULONG)_strtoui64(g_PhysReadSize, NULL, 0);
                        if (size > 0x2000) size = 0x2000;
                        std::vector<BYTE> buf(size ? size : 0x100);
                        if (DbkReadPhysical(addr, buf.data(), (ULONG)buf.size()))
                            FormatPhysDump(buf.data(), (ULONG)buf.size(), addr, g_PhysReadDump, sizeof(g_PhysReadDump));
                        else
                            strcpy_s(g_PhysReadDump, sizeof(g_PhysReadDump), "Physical read failed");
                    }
                    ImGui::TextWrapped("%s", g_PhysReadDump);

                    ImGui::Separator();
                    ImGui::SetNextItemWidth(200);
                    ImGui::InputText("Physical address", g_PhysWriteAddr, sizeof(g_PhysWriteAddr));
                    ImGui::SetNextItemWidth(340);
                    ImGui::InputText("Hex bytes (0x90, 0x90 ...)", g_PhysWriteHex, sizeof(g_PhysWriteHex));
                    ImGui::SameLine();
                    if (ImGui::Button("Write Physical", ImVec2(110, 24))) {
                        ULONG64 addr = _strtoui64(g_PhysWriteAddr, NULL, 0);
                        UCHAR bytes[512];
                        ULONG count = 0;
                        if (ParseHexBytes(g_PhysWriteHex, bytes, sizeof(bytes), &count) && count <= 0x2000) {
                            if (DbkWritePhysical(addr, bytes, count))
                                strcpy_s(g_KernelStatus, sizeof(g_KernelStatus), "Physical write OK");
                            else
                                strcpy_s(g_KernelStatus, sizeof(g_KernelStatus), "Physical write failed");
                        }
                    }
                }

                if (ImGui::CollapsingHeader("Kernel Memory Allocation")) {
                    ImGui::SetNextItemWidth(200);
                    ImGui::InputText("Nonpaged size", g_AllocNonPagedSize, sizeof(g_AllocNonPagedSize));
                    ImGui::SameLine();
                    if (ImGui::Button("Allocate kernel", ImVec2(120, 24))) {
                        ULONG size = (ULONG)_strtoui64(g_AllocNonPagedSize, NULL, 0);
                        DbkAllocNonPaged(size, &g_AllocNonPagedAddr);
                    }
                    ImGui::Text("Kernel addr: 0x%llX", g_AllocNonPagedAddr);
                    ImGui::SameLine();
                    if (ImGui::Button("Free kernel", ImVec2(90, 24)) && g_AllocNonPagedAddr) {
                        DbkFreeNonPaged(g_AllocNonPagedAddr);
                        g_AllocNonPagedAddr = 0;
                    }

                    ImGui::Separator();
                    ImGui::SetNextItemWidth(200);
                    ImGui::InputText("Process alloc size", g_AllocProcessSize, sizeof(g_AllocProcessSize));
                    ImGui::SameLine();
                    if (ImGui::Button("Allocate in process", ImVec2(140, 24)) && g_SelectedPid != 0) {
                        ULONG64 size = _strtoui64(g_AllocProcessSize, NULL, 0);
                        DbkAllocProcessMem(g_SelectedPid, size, &g_AllocProcessAddr);
                    }
                    ImGui::Text("Process addr: 0x%llX", g_AllocProcessAddr);
                }

                if (ImGui::CollapsingHeader("Process Control")) {
                    if (ImGui::Button("Suspend Process", ImVec2(130, 24)) && g_SelectedPid != 0) {
                        if (DbkSuspendProcess(g_SelectedPid))
                            strcpy_s(g_KernelStatus, sizeof(g_KernelStatus), "Process suspended");
                        else
                            strcpy_s(g_KernelStatus, sizeof(g_KernelStatus), "Suspend failed");
                    }
                    ImGui::SameLine();
                    if (ImGui::Button("Resume Process", ImVec2(130, 24)) && g_SelectedPid != 0) {
                        if (DbkResumeProcess(g_SelectedPid))
                            strcpy_s(g_KernelStatus, sizeof(g_KernelStatus), "Process resumed");
                        else
                            strcpy_s(g_KernelStatus, sizeof(g_KernelStatus), "Resume failed");
                    }

                    ImGui::Separator();
                    if (ImGui::Button("Open kernel handle", ImVec2(140, 24)) && g_SelectedPid != 0) {
                        UCHAR special = 0;
                        DbkOpenProcessHandle(g_SelectedPid, &g_OpenProcessHandle, &special);
                    }
                    ImGui::Text("Kernel handle: 0x%llX", g_OpenProcessHandle);

                    ImGui::Separator();
                    if (ImGui::Button("Get PEPROCESS", ImVec2(140, 24)) && g_SelectedPid != 0) {
                        DbkGetPEPROCESS(g_SelectedPid, &g_PeProcess);
                    }
                    ImGui::Text("PEPROCESS: 0x%llX", g_PeProcess);
                }

                ImGui::EndChild();
                ImGui::EndTabItem();
            }

            ImGui::EndTabBar();
        }

        ImGui::End();

        ImGui::Render();
        const float clear_color[4] = { 0.08f, 0.08f, 0.10f, 1.00f };
        pd3dDeviceContext->OMSetRenderTargets(1, &mainRenderTargetView, NULL);
        pd3dDeviceContext->ClearRenderTargetView(mainRenderTargetView, clear_color);
        D3D11_VIEWPORT vp{};
        vp.Width = (FLOAT)fbWidth;
        vp.Height = (FLOAT)fbHeight;
        vp.MinDepth = 0.0f;
        vp.MaxDepth = 1.0f;
        vp.TopLeftX = 0;
        vp.TopLeftY = 0;
        pd3dDeviceContext->RSSetViewports(1, &vp);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

        pSwapChain->Present(1, 0);
    }

    // Cleanup
    g_FreezeRunning = false;
    if (g_FreezeThread.joinable()) g_FreezeThread.join();

    ImGui_ImplDX11_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();

    if (mainRenderTargetView) mainRenderTargetView->Release();
    if (pSwapChain) pSwapChain->Release();
    if (pd3dDeviceContext) pd3dDeviceContext->Release();
    if (pd3dDevice) pd3dDevice->Release();

    glfwDestroyWindow(window);
    glfwTerminate();

    DisconnectDriver();
    StopAndUnloadDriver();

    return 0;
}

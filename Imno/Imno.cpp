#include "Imno.h"

// Direct Link Libraries for D3D11 & DXGI
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "dwmapi.lib")

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

std::vector<ProcessInfoV2> g_ProcessList;
std::vector<ModuleInfoV2>  g_LoadedModules;
ULONG g_SelectedPid = 0;
char  g_ProcessFilter[64] = "";
char  g_ModuleFilter[64] = "";

// Scan State
int   g_SelectedDataType = 4; // 4 = 4 Bytes
char  g_ScanValueInput[128] = "";
char  g_ScanRangeStart[64] = "0";
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

// =====================================================================
//  Driver Control Helpers
// =====================================================================
bool LoadAndStartDriver()
{
    SC_HANDLE hSCM = OpenSCManagerW(NULL, NULL, SC_MANAGER_ALL_ACCESS);
    if (!hSCM) return false;

    WCHAR driverPath[MAX_PATH];
    GetModuleFileNameW(NULL, driverPath, MAX_PATH);
    wchar_t* lastSlash = wcsrchr(driverPath, L'\\');
    if (lastSlash) *(lastSlash + 1) = L'\0';
    wcscat_s(driverPath, MAX_PATH, L"MasterXDriver.sys");

    SC_HANDLE hService = CreateServiceW(
        hSCM,
        L"MasterXDriver",
        L"MasterXDriver",
        SERVICE_ALL_ACCESS,
        SERVICE_KERNEL_DRIVER,
        SERVICE_DEMAND_START,
        SERVICE_ERROR_NORMAL,
        driverPath,
        NULL, NULL, NULL, NULL, NULL
    );

    if (!hService) {
        hService = OpenServiceW(hSCM, L"MasterXDriver", SERVICE_ALL_ACCESS);
    }

    if (hService) {
        StartServiceW(hService, 0, NULL);
        CloseServiceHandle(hService);
    }

    CloseServiceHandle(hSCM);
    return true;
}

void StopAndUnloadDriver()
{
    SC_HANDLE hSCM = OpenSCManagerW(NULL, NULL, SC_MANAGER_ALL_ACCESS);
    if (!hSCM) return;

    SC_HANDLE hService = OpenServiceW(hSCM, L"MasterXDriver", SERVICE_ALL_ACCESS);
    if (hService) {
        SERVICE_STATUS status;
        ControlService(hService, SERVICE_CONTROL_STOP, &status);
        DeleteService(hService);
        CloseServiceHandle(hService);
    }

    CloseServiceHandle(hSCM);
}

bool ConnectDriver()
{
    if (g_hDriver != INVALID_HANDLE_VALUE) CloseHandle(g_hDriver);
    
    g_hDriver = CreateFileW(L"\\\\.\\Global\\MasterXDriver",
        GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    
    if (g_hDriver == INVALID_HANDLE_VALUE) {
        g_hDriver = CreateFileW(L"\\\\.\\MasterXDriver",
            GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    }
    return g_hDriver != INVALID_HANDLE_VALUE;
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
    MEMORY_REQUEST_V2 req = { pid, addr, sz, val64 };
    DWORD br = 0;
    DeviceIoControl(g_hDriver, IOCTL_V2_WRITE_MEMORY, &req, sizeof(req), NULL, 0, &br, NULL);
}

void ReadMemory(ULONG pid, ULONG_PTR addr, ULONG64* outVal, int dataType)
{
    if (g_hDriver == INVALID_HANDLE_VALUE || pid == 0) { *outVal = 0; return; }
    ULONG sz = GetDataSize(dataType);
    MEMORY_REQUEST_V2 req = { pid, addr, sz, 0 };
    DWORD br = 0;
    DeviceIoControl(g_hDriver, IOCTL_V2_READ_MEMORY, &req, sizeof(req), outVal, sizeof(ULONG64), &br, NULL);
}

void PatchMemory(ULONG_PTR addr, const UCHAR* pattern, ULONG size)
{
    if (g_hDriver == INVALID_HANDLE_VALUE || g_SelectedPid == 0 || size == 0 || size > 32) return;
    PATCH_REQUEST_V2 req = {};
    req.TargetPid = g_SelectedPid;
    req.Address = addr;
    req.Size = size;
    memcpy(req.Pattern, pattern, size);

    DWORD br = 0;
    DeviceIoControl(g_hDriver, IOCTL_V2_PATCH_MEMORY, &req, sizeof(req), NULL, 0, &br, NULL);
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
    else if (dataType == 4) { // 8 Bytes / Int64
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

const ModuleInfoV2* FindModuleByAddress(ULONG_PTR addr)
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

    const ModuleInfoV2* mod = FindModuleByAddress(baseAddr);
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
            ReadMemory(pid, result.FinalAddress, &result.FinalValue, 0); // read final value as 8 bytes
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
        ReadMemory(pid, currentAddress, &val64, 0); // pointers are always 8 bytes (dataType 0)
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
//  Freeze / Cheat Table Loop
// =====================================================================
void FreezeLoop()
{
    while (g_FreezeRunning) {
        if (g_hDriver != INVALID_HANDLE_VALUE) {
            std::lock_guard<std::mutex> lock(g_CheatTableLock);
            for (auto& item : g_CheatTable) {
                // Each item keeps its own pid/type, so switching the selected
                // process or data type never corrupts unrelated memory
                if (item.Enabled && item.Pid != 0) {
                    WriteMemory(item.Pid, item.Address, item.Value64, item.DataType);
                }
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
}

// =====================================================================
//  Process & Module Enumeration
// =====================================================================
void RefreshProcessList()
{
    if (g_hDriver == INVALID_HANDLE_VALUE) return;
    ULONG br = 0;
    std::vector<ProcessInfoV2> temp(4096);
    if (DeviceIoControl(g_hDriver, IOCTL_V2_GET_PROCESS_LIST, NULL, 0, temp.data(), (DWORD)(temp.size() * sizeof(ProcessInfoV2)), &br, NULL)) {
        ULONG count = br / sizeof(ProcessInfoV2);
        if (count > 0) {
            g_ProcessList.assign(temp.begin(), temp.begin() + count);
        }
    }
}

void RefreshModules()
{
    if (g_hDriver == INVALID_HANDLE_VALUE || g_SelectedPid == 0) return;
    ULONG br = 0;
    std::vector<ModuleInfoV2> temp(1024);
    if (DeviceIoControl(g_hDriver, IOCTL_V2_ENUM_MODULES, &g_SelectedPid, sizeof(g_SelectedPid), temp.data(), (DWORD)(temp.size() * sizeof(ModuleInfoV2)), &br, NULL)) {
        ULONG count = br / sizeof(ModuleInfoV2);
        g_LoadedModules.assign(temp.begin(), temp.begin() + count);
    }
    else {
        g_LoadedModules.clear();
    }
}

// =====================================================================
//  Async Scanning Workers
// =====================================================================
void AsyncFirstScanWorker(ULONG targetPid, int dataType, ULONG64 searchVal64, bool useRange, ULONG_PTR rangeStart, ULONG_PTR rangeEnd, bool allowUnaligned)
{
    g_IsScanning = true;
    g_ScanProgress = 0;
    g_ScanTruncated = false;
    {
        std::lock_guard<std::mutex> lock(g_ScanResultsLock);
        g_ScanResults.clear();
    }

    if (g_hDriver == INVALID_HANDLE_VALUE) {
        g_IsScanning = false;
        return;
    }

    SCAN_REQUEST_V2 req = { targetPid, (ULONG)dataType, searchVal64, rangeStart, rangeEnd,
                            (ULONG)(allowUnaligned ? SCAN_FLAG_UNALIGNED : 0) };
    ULONG br = 0;
    std::vector<ULONG_PTR> tempResults((size_t)MAX_SCAN_RESULTS + 1); // slot 0 = result flags

    BOOL ok = DeviceIoControl(g_hDriver, IOCTL_V2_FIRST_SCAN, &req, sizeof(req), tempResults.data(), (DWORD)(tempResults.size() * sizeof(ULONG_PTR)), &br, NULL);
    if (ok && br >= sizeof(ULONG_PTR)) {
        ULONG totalSlots = br / sizeof(ULONG_PTR);
        g_ScanTruncated = (tempResults[0] & SCAN_RESULT_TRUNCATED) != 0;
        std::lock_guard<std::mutex> lock(g_ScanResultsLock);
        g_ScanResults.assign(tempResults.begin() + 1, tempResults.begin() + totalSlots);
    }
    else {
        std::lock_guard<std::mutex> lock(g_ScanResultsLock);
        g_ScanResults.clear();
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

    size_t headerSize = sizeof(NEXT_SCAN_HEADER_V2);
    size_t dataSize = prevResults.size() * sizeof(ULONG_PTR);
    std::vector<UCHAR> buffer(headerSize + dataSize);

    PNEXT_SCAN_HEADER_V2 hdr = (PNEXT_SCAN_HEADER_V2)buffer.data();
    hdr->TargetPid = targetPid;
    hdr->DataType = (ULONG)dataType;
    hdr->SearchValue64 = searchVal64;
    hdr->AddressCount = (ULONG)prevResults.size();

    memcpy(buffer.data() + headerSize, prevResults.data(), dataSize);

    ULONG br = 0;
    std::vector<ULONG_PTR> newResults(prevResults.size());

    BOOL ok = DeviceIoControl(g_hDriver, IOCTL_V2_NEXT_SCAN, buffer.data(), (DWORD)buffer.size(), newResults.data(), (DWORD)(newResults.size() * sizeof(ULONG_PTR)), &br, NULL);
    if (ok && br >= sizeof(ULONG_PTR)) {
        ULONG count = br / sizeof(ULONG_PTR);
        std::lock_guard<std::mutex> lock(g_ScanResultsLock);
        g_ScanResults.assign(newResults.begin(), newResults.begin() + count);
    }
    else {
        std::lock_guard<std::mutex> lock(g_ScanResultsLock);
        g_ScanResults.clear();
    }

    g_ScanVersion++;
    g_ScanProgress = 100;
    g_IsScanning = false;
}

void StartFirstScan()
{
    if (g_SelectedPid == 0 || g_IsScanning) return;
    ULONG64 val64 = ParseInputToValue(g_ScanValueInput, g_SelectedDataType);
    ULONG_PTR rStart = 0;
    ULONG_PTR rEnd = 0x7FFFFFFFFFFF;
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
//  WinMain / Entry Point
// =====================================================================
int APIENTRY WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow)
{
    UNREFERENCED_PARAMETER(hInstance);
    UNREFERENCED_PARAMETER(hPrevInstance);
    UNREFERENCED_PARAMETER(lpCmdLine);
    UNREFERENCED_PARAMETER(nCmdShow);

    LoadAndStartDriver();
    bool driverConnected = ConnectDriver();
    if (!driverConnected) {
        MessageBoxA(NULL, "Failed to connect to MasterXDriver! Run as Administrator.", "Warning", MB_OK | MB_ICONWARNING);
    }

    if (!glfwInit()) return 1;

    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    GLFWwindow* window = glfwCreateWindow(1280, 720, "MasterX Suite v2.0 - Ultimate Kernel Engine", NULL, NULL);
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

    // Initial fetch of processes
    if (driverConnected) {
        RefreshProcessList();
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
        // Use framebuffer size for ImGui window so it matches SwapChain (fix DPI / resize mismatch)
        ImGui::SetNextWindowSize(ImVec2((float)fbWidth, (float)fbHeight), ImGuiCond_Always);
        ImGui::Begin("MasterX Suite v2.0", NULL, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoTitleBar);

        // Top Bar: Driver Status Active/Inactive & Process Combo
        driverConnected = (g_hDriver != INVALID_HANDLE_VALUE);
        if (driverConnected) {
            ImGui::TextColored({ 0.0f, 1.0f, 0.0f, 1.0f }, "[Driver: ACTIVE]");
        }
        else {
            ImGui::TextColored({ 1.0f, 0.0f, 0.0f, 1.0f }, "[Driver: INACTIVE]");
            ImGui::SameLine();
            if (ImGui::Button("Reconnect")) {
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
        if (ImGui::BeginTabBar("MasterXTabs"))
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
                    ImGui::ProgressBar((float)g_ScanProgress / 100.0f, ImVec2(-1, 0), "Scanning Kernel Memory...");
                }

                ImGui::Spacing();
                ImGui::Separator();

                // UI-side snapshot of the scan results: refreshed only when a scan
                // finishes, so the render loop never touches the worker's vector
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
                    // Only the visible rows are read (and at most ~2x per second),
                    // instead of thousands of IOCTLs every frame
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

            ImGui::EndTabBar();
        }

        ImGui::End();

        ImGui::Render();
        const float clear_color[4] = { 0.08f, 0.08f, 0.10f, 1.00f };
        pd3dDeviceContext->OMSetRenderTargets(1, &mainRenderTargetView, NULL);
        pd3dDeviceContext->ClearRenderTargetView(mainRenderTargetView, clear_color);
        // Ensure viewport covers new framebuffer size
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

    StopAndUnloadDriver();

    return 0;
}
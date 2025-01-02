// HomeMonitor V2: Viewing Application using WIN32 + DX12
//
// [2024/12/23]

#define NOMINMAX    // Needed to avoid Windows.h version of min/max being used

#include <d3d12.h>
#include <dxgi1_4.h>
#include <dwmapi.h>
#include <tchar.h>
#include <string>
#include <fstream>
#include <filesystem>
#include <algorithm>
#include <ranges>

#include "Imgui/imgui.h"
#include "Imgui/imgui_impl_win32.h"
#include "Imgui/imgui_impl_dx12.h"
#include "Imgui/implot.h"

#include "Resources/resource.h"

#include "ThingSpeak/ThingSpeak.h"

#define DEBUG_HOMEMONITOR       true
#define HOMEMONITOR_USE_VSYNC   false

#define MAX_HOMEMONITOR_USER_INPUT_SIZE   30

#if (DEBUG_HOMEMONITOR)
#include <iostream>
#else
#pragma comment(linker, "/SUBSYSTEM:windows /ENTRY:mainCRTStartup") // hide command line
#endif

// Config for example app
static const int APP_NUM_FRAMES_IN_FLIGHT = 3;
static const int APP_NUM_BACK_BUFFERS = 3;
static const int APP_SRV_HEAP_SIZE = 64;

struct FrameContext
{
    ID3D12CommandAllocator*     CommandAllocator;
    UINT64                      FenceValue;
};

// Simple free list based allocator
struct DirectX12HeapAllocator
{
    ID3D12DescriptorHeap*       Heap = nullptr;
    D3D12_DESCRIPTOR_HEAP_TYPE  HeapType = D3D12_DESCRIPTOR_HEAP_TYPE_NUM_TYPES;
    D3D12_CPU_DESCRIPTOR_HANDLE HeapStartCpu;
    D3D12_GPU_DESCRIPTOR_HANDLE HeapStartGpu;
    UINT                        HeapHandleIncrement;
    ImVector<int>               FreeIndices;

    void Create(ID3D12Device* device,
                ID3D12DescriptorHeap* heap)
    {
        IM_ASSERT(Heap == nullptr && FreeIndices.empty());
        Heap = heap;
        D3D12_DESCRIPTOR_HEAP_DESC desc = heap->GetDesc();
        HeapType = desc.Type;
        HeapStartCpu = Heap->GetCPUDescriptorHandleForHeapStart();
        HeapStartGpu = Heap->GetGPUDescriptorHandleForHeapStart();
        HeapHandleIncrement = device->GetDescriptorHandleIncrementSize(HeapType);
        FreeIndices.reserve((int)desc.NumDescriptors);
        for (int n = desc.NumDescriptors; n > 0; n--)
        {
            FreeIndices.push_back(n);
        }
    }
    void Destroy()
    {
        Heap = nullptr;
        FreeIndices.clear();
    }
    void Alloc(D3D12_CPU_DESCRIPTOR_HANDLE* out_cpu_desc_handle,
               D3D12_GPU_DESCRIPTOR_HANDLE* out_gpu_desc_handle)
    {
        IM_ASSERT(FreeIndices.Size > 0);
        int idx = FreeIndices.back();
        FreeIndices.pop_back();
        out_cpu_desc_handle->ptr = HeapStartCpu.ptr + (idx * HeapHandleIncrement);
        out_gpu_desc_handle->ptr = HeapStartGpu.ptr + (idx * HeapHandleIncrement);
    }
    void Free(D3D12_CPU_DESCRIPTOR_HANDLE out_cpu_desc_handle,
              D3D12_GPU_DESCRIPTOR_HANDLE out_gpu_desc_handle)
    {
        int cpu_idx = (int)((out_cpu_desc_handle.ptr - HeapStartCpu.ptr) / HeapHandleIncrement);
        int gpu_idx = (int)((out_gpu_desc_handle.ptr - HeapStartGpu.ptr) / HeapHandleIncrement);
        IM_ASSERT(cpu_idx == gpu_idx);
        FreeIndices.push_back(cpu_idx);
    }
};

// Data
static FrameContext                   g_frameContext[APP_NUM_FRAMES_IN_FLIGHT] = {};
static UINT                           g_frameIndex = 0;

static ID3D12Device*                  g_pd3dDevice = nullptr;
static ID3D12DescriptorHeap*          g_pd3dRtvDescHeap = nullptr;
static ID3D12DescriptorHeap*          g_pd3dSrvDescHeap = nullptr;
static DirectX12HeapAllocator         g_pd3dSrvDescHeapAlloc;
static ID3D12CommandQueue*            g_pd3dCommandQueue = nullptr;
static ID3D12GraphicsCommandList*     g_pd3dCommandList = nullptr;
static ID3D12Fence*                   g_fence = nullptr;
static HANDLE                         g_fenceEvent = nullptr;
static UINT64                         g_fenceLastSignaledValue = 0;
static IDXGISwapChain3*               g_pSwapChain = nullptr;
static bool                           g_SwapChainOccluded = false;
static HANDLE                         g_hSwapChainWaitableObject = nullptr;
static ID3D12Resource*                g_mainRenderTargetResource[APP_NUM_BACK_BUFFERS] = {};
static D3D12_CPU_DESCRIPTOR_HANDLE    g_mainRenderTargetDescriptor[APP_NUM_BACK_BUFFERS] = {};

// Forward declarations of helper functions
bool CreateDeviceD3D(HWND hWnd);
void CleanupDeviceD3D();
void CreateRenderTarget();
void CleanupRenderTarget();
void WaitForLastSubmittedFrame();
FrameContext* WaitForNextFrameResources();
LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg,
                                                             WPARAM wParam, LPARAM lParam);

// Win32 API - Declarations
void Win32RegisterAndCreateWindow(HWND& hwnd, WNDCLASSEXW& windowClass);

// HomeMonitor Global Definitions
typedef struct
{
    unsigned int colorRgba;
    ImVec4 colorRgb;
    bool available;
} ColorOption_t;

typedef struct
{
    unsigned int assignedColorRgba;   // RGBA Color. E.g. (255, 255, 255, 0)
    ImVec4 assignedColorRgb;          // RGB Color. E.g. (1.0, 1.0, 1.0, 0.0)
} HomeMonitorAssignedColor_t;

typedef struct
{
    ThingSpeak thingSpeak;
    HomeMonitorAssignedColor_t assignedColor;

    // Display properties
    bool displayData;
} HomeMonitor_t;

// HomeMonitor Global Declarations
bool darkMode = false;

// HomeMonitor Window Creation
void HomeMonitorCreateViewerPropertiesWindow(std::vector<HomeMonitor_t>& homeMonitors);
void HomeMonitorCreateAddThingSpeakObjectWindow(std::vector<HomeMonitor_t>& homeMonitors);
void HomeMonitorCreateThingSpeakViewerWindow(std::string name,
                                             std::string xAxisLabel,
                                             std::string yAxisLabel,
                                             ThingSpeakField field,
                                             std::vector<HomeMonitor_t>& homeMonitors);

// HomeMonitor Graph Functions
void HomeMonitorGraphStyleLight();
void HomeMonitorGraphStyleDark();

bool HomeMonitorSetColor(HomeMonitor_t& homeMonitor);
void HomeMonitorDrawVerticalCursor();
void HomeMonitorDrawHorizontalLine();

std::pair<int, int> HomeMonitorGetClosestPointToMouse(ThingSpeakField field,
                                                      std::vector<HomeMonitor_t>& homeMonitors);
std::pair<float, float> HomeMonitorGetYAxisBoundaries(ThingSpeakField field,
                                                      std::vector<HomeMonitor_t>& homeMonitors);

int main(int argc, char** argv)
{
    HWND hwnd;
    WNDCLASSEXW windowClass;
    Win32RegisterAndCreateWindow(hwnd, windowClass);

    // Initialize Direct3D
    if (!CreateDeviceD3D(hwnd))
    {
        CleanupDeviceD3D();
        ::UnregisterClassW(windowClass.lpszClassName,
                           windowClass.hInstance);
        return 1;
    }

    // Show the window
    ::ShowWindow(hwnd, SW_SHOWDEFAULT);
    ::UpdateWindow(hwnd);

    // Setup context
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;  // Enable Keyboard Controls
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;   // Enable Gamepad Controls
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;      // Enable Docking
    io.ConfigFlags |= ImGuiConfigFlags_ViewportsEnable;    // Enable Multi-Viewport / Platform Windows

    ImPlot::CreateContext();

    // Setup Dear ImGui initial style
    ImVec4 clearColor;
    if (darkMode)
    {
        ImGui::StyleColorsDark();
        HomeMonitorGraphStyleDark();
        clearColor = ImVec4(0.2f, 0.2f, 0.2f, 1.00f);
    }
    else
    {
        ImGui::StyleColorsLight();
        HomeMonitorGraphStyleLight();
        clearColor = ImVec4(0.8f, 0.8f, 0.8f, 1.00f);
    }
    darkMode = !darkMode;

    // When viewports are enabled we tweak WindowRounding/WindowBg
    // so platform windows can look identical to regular ones.
    ImGuiStyle& style = ImGui::GetStyle();
    if (io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable)
    {
        style.WindowRounding = 0.0f;
        style.Colors[ImGuiCol_WindowBg].w = 1.0f;
    }

    // Setup Platform/Renderer backends
    ImGui_ImplWin32_Init(hwnd);

    ImGui_ImplDX12_InitInfo init_info = {};
    init_info.Device = g_pd3dDevice;
    init_info.CommandQueue = g_pd3dCommandQueue;
    init_info.NumFramesInFlight = APP_NUM_FRAMES_IN_FLIGHT;
    init_info.RTVFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
    init_info.DSVFormat = DXGI_FORMAT_UNKNOWN;
    init_info.SrvDescriptorHeap = g_pd3dSrvDescHeap;
    init_info.SrvDescriptorAllocFn = [](ImGui_ImplDX12_InitInfo*,
                                        D3D12_CPU_DESCRIPTOR_HANDLE* out_cpu_handle,
                                        D3D12_GPU_DESCRIPTOR_HANDLE* out_gpu_handle) {
                                            return g_pd3dSrvDescHeapAlloc.Alloc(out_cpu_handle,
                                                                                out_gpu_handle);
                                        };
    init_info.SrvDescriptorFreeFn = [](ImGui_ImplDX12_InitInfo*,
                                       D3D12_CPU_DESCRIPTOR_HANDLE cpu_handle,
                                       D3D12_GPU_DESCRIPTOR_HANDLE gpu_handle) {
                                            return g_pd3dSrvDescHeapAlloc.Free(cpu_handle,
                                                                               gpu_handle);
                                       };
    ImGui_ImplDX12_Init(&init_info);

    // Load font
    char font_file[] = "D:\\06_PersonalProjects\\HomeMonitorV2\\Fonts\\Roboto-Regular.ttf";
    io.Fonts->AddFontFromFileTTF(font_file, 16.0f);

    // Initialize ThingSpeak structures
    std::string thingSpeakFilePath = "D:\\06_PersonalProjects\\HomeMonitorV2\\ThingSpeak\\ThingSpeakObjects.json";

    std::ifstream thingSpeakObjectsFile(thingSpeakFilePath);
    if (!thingSpeakObjectsFile.is_open())
    {
        std::cerr << "Could not open " << thingSpeakFilePath << std::endl;
        return -1;
    }

    json thingSpeakObjectsJson;
    thingSpeakObjectsFile >> thingSpeakObjectsJson;

    thingSpeakObjectsFile.close();

    std::vector<HomeMonitor_t> homeMonitors;
    for (auto& thingSpeakObject : thingSpeakObjectsJson)
    {
        HomeMonitor_t homeMonitor;

        homeMonitor.thingSpeak = { thingSpeakObject["name"],
                                   thingSpeakObject["channel"],
                                   thingSpeakObject["key"]      };

        HomeMonitorSetColor(homeMonitor);

        homeMonitors.push_back(homeMonitor);
    }

    std::string fieldName;
    int totalDataPoints;

    int result;
    auto pollingDelay = std::chrono::steady_clock::now();

    // Start rendering loop
    bool done = false;

    while (!done)
    {
        // Poll and handle messages (inputs, window resize, etc.)
        // 
        // See the WndProc() function below for for procedure to
        // dispatch events to the Win32 backend.
        MSG msg;
        while (::PeekMessage(&msg, nullptr, 0U, 0U, PM_REMOVE))
        {
            ::TranslateMessage(&msg);
            ::DispatchMessage(&msg);
            if (msg.message == WM_QUIT)
            {
                done = true;
            }
        }
        if (done)
        {
            break;
        }

        // Handle window screen locked
        if (g_SwapChainOccluded &&
            g_pSwapChain->Present(0, DXGI_PRESENT_TEST) == DXGI_STATUS_OCCLUDED)
        {
            ::Sleep(10);
            continue;
        }
        g_SwapChainOccluded = false;

        // Start the Dear ImGui frame
        ImGui_ImplDX12_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        // Create docking space
        ImGui::DockSpaceOverViewport(0, ImGui::GetMainViewport());

        // Create HomeMonitor control windows
        HomeMonitorCreateViewerPropertiesWindow(homeMonitors);
        HomeMonitorCreateAddThingSpeakObjectWindow(homeMonitors);

        // Refresh data periodically
        if (std::chrono::steady_clock::now() > pollingDelay)
        {
            auto currentTime = std::chrono::system_clock::now();
            std::time_t refreshTime = std::chrono::system_clock::to_time_t(currentTime);
            std::cout << "\nRefreshing data at " << std::ctime(&refreshTime) << std::endl;

            result = homeMonitors[0].thingSpeak.GetFieldData();

            pollingDelay = std::chrono::steady_clock::now() + std::chrono::minutes(5);
        }

        // Create Homemonitor plotting windows
        HomeMonitorCreateThingSpeakViewerWindow("Temperature",
                                                "Entry ID", "Temperature (Fahrenheit)",
                                                ThingSpeakField::Temperature, homeMonitors);
        HomeMonitorCreateThingSpeakViewerWindow("Humidity",
                                                "Entry ID", "Relative Humidity (%)",
                                                ThingSpeakField::Humidity, homeMonitors);

        // Rendering
        ImGui::Render();

        FrameContext* frameCtx = WaitForNextFrameResources();
        UINT backBufferIdx = g_pSwapChain->GetCurrentBackBufferIndex();
        frameCtx->CommandAllocator->Reset();

        D3D12_RESOURCE_BARRIER barrier = {};
        barrier.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Flags                  = D3D12_RESOURCE_BARRIER_FLAG_NONE;
        barrier.Transition.pResource   = g_mainRenderTargetResource[backBufferIdx];
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
        barrier.Transition.StateAfter  = D3D12_RESOURCE_STATE_RENDER_TARGET;
        g_pd3dCommandList->Reset(frameCtx->CommandAllocator, nullptr);
        g_pd3dCommandList->ResourceBarrier(1, &barrier);

        // Render Dear ImGui graphics
        const float clearColorWithAlpha[4] = 
        {
            clearColor.x * clearColor.w,
            clearColor.y * clearColor.w,
            clearColor.z * clearColor.w,
            clearColor.w
        };
        g_pd3dCommandList->ClearRenderTargetView(g_mainRenderTargetDescriptor[backBufferIdx],
                                                 clearColorWithAlpha, 0, nullptr);
        g_pd3dCommandList->OMSetRenderTargets(1, &g_mainRenderTargetDescriptor[backBufferIdx],
                                              FALSE, nullptr);
        g_pd3dCommandList->SetDescriptorHeaps(1, &g_pd3dSrvDescHeap);

        ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), g_pd3dCommandList);
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        barrier.Transition.StateAfter  = D3D12_RESOURCE_STATE_PRESENT;
        g_pd3dCommandList->ResourceBarrier(1, &barrier);
        g_pd3dCommandList->Close();

        g_pd3dCommandQueue->ExecuteCommandLists(1, (ID3D12CommandList* const*)&g_pd3dCommandList);

        // Update and Render additional Platform Windows
        if (io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable)
        {
            ImGui::UpdatePlatformWindows();
            ImGui::RenderPlatformWindowsDefault();
        }

        // Present
        HRESULT hr;
        #if (HOMEMONITOR_USE_VSYNC)
        hr = g_pSwapChain->Present(1, 0);
        #else
        hr = g_pSwapChain->Present(0, 0);
        #endif
        g_SwapChainOccluded = (hr == DXGI_STATUS_OCCLUDED);

        UINT64 fenceValue = g_fenceLastSignaledValue + 1;
        g_pd3dCommandQueue->Signal(g_fence, fenceValue);
        g_fenceLastSignaledValue = fenceValue;
        frameCtx->FenceValue = fenceValue;
    }

    WaitForLastSubmittedFrame();

    // Cleanup
    ImGui_ImplDX12_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImPlot::DestroyContext();
    ImGui::DestroyContext();

    CleanupDeviceD3D();
    ::DestroyWindow(hwnd);
    ::UnregisterClassW(windowClass.lpszClassName, windowClass.hInstance);

    return 0;
}

/**
 * @brief Create "Viewer Properties" window of HomeMonitor GUI
 * 
 *  @param homeMonitors - Collection of HomeMonitor objects to render
 */
void HomeMonitorCreateViewerPropertiesWindow(std::vector<HomeMonitor_t>& homeMonitors)
{
    int result;

    ImGui::Begin("Viewer Properties");

    ImGui::Text("General Actions");
    ImGui::Dummy(ImVec2(0.0f, 10.0f));

    if (ImGui::Button("Toggle Theme", ImVec2(100, 0)))
    {
        if (darkMode)
        {
            ImGui::StyleColorsDark();
            HomeMonitorGraphStyleDark();
        }
        else
        {
            ImGui::StyleColorsLight();
            HomeMonitorGraphStyleLight();
        }
        darkMode = !darkMode;
    }
    ImGui::Dummy(ImVec2(0.0f, 5.0f));

    if (ImGui::Button("Refresh Data", ImVec2(100, 0)))
    {
        result = homeMonitors[0].thingSpeak.GetFieldData();
    }

    HomeMonitorDrawHorizontalLine();

    ImGui::Text("Toggle Plot Visibility");
    ImGui::Dummy(ImVec2(0.0f, 10.0f));

    ImVec4 color;
    if (homeMonitors[0].displayData)
    {
        color = homeMonitors[0].assignedColor.assignedColorRgb;
    }
    else
    {
        color = ImVec4(0.8f, 0.8f, 0.8f, 1.0f);
    }
    ImVec4 colorOnHover = ImVec4(color.x, color.y, color.z, (color.w * 0.5));

    ImGui::PushStyleColor(ImGuiCol_CheckMark, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_FrameBg, color);
    ImGui::PushStyleColor(ImGuiCol_FrameBgActive, color);
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, colorOnHover);

    ImGui::Checkbox(homeMonitors[0].thingSpeak.GetName().c_str(),
                    &homeMonitors[0].displayData);

    ImGui::PopStyleColor();
    ImGui::PopStyleColor();
    ImGui::PopStyleColor();
    ImGui::PopStyleColor();

    #if (DEBUG_HOMEMONITOR)
    HomeMonitorDrawHorizontalLine();

    ImGuiIO& io = ImGui::GetIO();
    ImGui::Text("System Diagnostics");
    ImGui::BulletText("Averaging %.1f FPS\n(Equal to %.3f ms/frame)",
                      io.Framerate, (1000.0f / io.Framerate));
    #endif

    ImGui::End();   // Viewer Properties
}

/**
 * @brief Create "Add ThingSpeak Object" window of HomeMonitor GUI
 * 
 * @param homeMonitors - Collection of HomeMonitor objects to render
 */
void HomeMonitorCreateAddThingSpeakObjectWindow(std::vector<HomeMonitor_t>& homeMonitors)
{
    ImGui::Begin("Add ThingSpeak Object");

    static char nameInputBuffer[MAX_HOMEMONITOR_USER_INPUT_SIZE] = "";
    ImGui::Text("Name", ImVec2(150, 0));
    ImGui::SameLine();
    ImGui::SetNextItemWidth(150.0f);
    ImGui::InputTextWithHint("##nameInput", "e.g. \"Bedroom\"",
                             nameInputBuffer, IM_ARRAYSIZE(nameInputBuffer));

    static char channelInputBuffer[MAX_HOMEMONITOR_USER_INPUT_SIZE] = "";
    ImGui::SameLine();
    ImGui::Text("Channel ID", ImVec2(150, 0));
    ImGui::SameLine();
    ImGui::SetNextItemWidth(150.0f);
    ImGui::InputTextWithHint("##channelInput", "e.g. \"1277292\"",
                             channelInputBuffer, IM_ARRAYSIZE(channelInputBuffer));

    static char apiKeyInputBuffer[MAX_HOMEMONITOR_USER_INPUT_SIZE] = "";
    ImGui::SameLine();
    ImGui::Text("Key", ImVec2(150, 0));
    ImGui::SameLine();
    ImGui::SetNextItemWidth(200.0f);
    ImGui::InputTextWithHint("##keyInput", "e.g. \"I4BV5Q70NNDWH0SP\"",
                             apiKeyInputBuffer, IM_ARRAYSIZE(apiKeyInputBuffer));

    ImGui::SameLine();
    if (ImGui::Button("Add", ImVec2(100, 0)))
    {
        #if (DEBUG_HOMEMONITOR)
        std::cout << "Name = " << nameInputBuffer << ", "
                  << "Channel = " << channelInputBuffer << ", "
                  << "Key = " << apiKeyInputBuffer << std::endl;
        #endif

        // TODO: Add entries to JSON file

        memset(channelInputBuffer, 0, sizeof(channelInputBuffer));
        memset(apiKeyInputBuffer, 0, sizeof(apiKeyInputBuffer));
    }

    ImGui::End();   // General Controls
}

/**
 * @brief Create temperature graph HomeMonitor GUI
 * 
 * @param homeMonitors - Collection of HomeMonitor objects to render
 */
void HomeMonitorCreateTemperatureViewerWindow(std::vector<HomeMonitor_t>& homeMonitors)
{
    ImVec2 maxWindowSize(-1, -1);

    ImGui::Begin("Temperature Viewer");

    ImPlot::PushStyleVar(ImPlotStyleVar_LineWeight, 2.5f);
    if (ImPlot::BeginPlot("Temperature", maxWindowSize), ImPlotFlags_NoInputs)
    {
        ImPlot::SetupAxes("Entry Number", "Temperature (Fahrenheit)");

        for (auto& homeMonitor : homeMonitors)
        {
            if (homeMonitor.displayData)
            {
                auto xAxisDataArray = homeMonitor.thingSpeak.GetTemperature()->xAxisData;
                auto yAxisDataArray = homeMonitor.thingSpeak.GetTemperature()->yAxisData;

                ImPlot::PushStyleColor(0, homeMonitor.assignedColor.assignedColorRgb);
                ImPlot::PlotLine(homeMonitor.thingSpeak.GetName().c_str(),
                                xAxisDataArray,
                                yAxisDataArray,
                                homeMonitor.thingSpeak.GetTemperature()->numDataPoints,
                                ImPlotLegendFlags_NoButtons);
                ImPlot::PopStyleColor();
            }
        }

        if (ImPlot::IsPlotHovered())
        {
            HomeMonitorDrawVerticalCursor();

            std::pair<int, int> closestIndicies =
                HomeMonitorGetClosestPointToMouse(ThingSpeakField::Temperature, homeMonitors);

            if (closestIndicies.first != -1)
            {
                auto homeMonitor = homeMonitors[closestIndicies.first];
                auto dataset = homeMonitor.thingSpeak.GetTemperature();
                auto entryId = closestIndicies.second;

                ImGui::BeginTooltip();
                ImGui::Text("Trendline: %s", homeMonitor.thingSpeak.GetName().c_str());
                ImGui::Text("Entry ID: %d", entryId);
                ImGui::Text("Temperature: %.2f", dataset->yAxisData[entryId]);
                ImGui::Text("Date/Time Captured (PST): %s", dataset->timestamp[entryId].c_str());
                ImGui::EndTooltip();
            }
        }

        ImPlot::EndPlot();
    }
    ImPlot::PopStyleVar();
    ImGui::End();
}

/**
 * @brief Create ThingSpeak graph viewer for HomeMonitor GUI
 * 
 * @param name - Graph name
 * @param xAxisLabel - Label to use for X-Axis
 * @param yAxisLabel - Label to use for Y-Axis
 * @param field - ThingSpeak field to use
 * @param homeMonitors - Collection of HomeMonitor objects to render
 */
void HomeMonitorCreateThingSpeakViewerWindow(std::string name,
                                             std::string xAxisLabel,
                                             std::string yAxisLabel,
                                             ThingSpeakField field,
                                             std::vector<HomeMonitor_t>& homeMonitors)
{
    ImVec2 maxWindowSize(-1, -1);

    std::string windowName(name + " Viewer");
    ImGui::Begin(windowName.c_str());

    ImPlot::PushStyleVar(ImPlotStyleVar_LineWeight, 2.5f);
    if (ImPlot::BeginPlot(name.c_str(), maxWindowSize), ImPlotFlags_NoInputs)
    {
        std::pair<float, float> yLimits =
            HomeMonitorGetYAxisBoundaries(field, homeMonitors);

        ImPlot::SetupAxes(xAxisLabel.c_str(), yAxisLabel.c_str());
        ImPlot::SetupAxisLimitsConstraints(ImAxis_X1, 0, (MAX_THINGSPEAK_REQUEST_SIZE - 1));
        ImPlot::SetupAxisLimitsConstraints(ImAxis_Y1,
                                           yLimits.first - 0.5,
                                           yLimits.second + 0.5);

        ThingSpeakFeedData_t const * dataset;

        for (auto& homeMonitor : homeMonitors)
        {
            if (homeMonitor.displayData)
            {
                if (field == ThingSpeakField::Temperature)
                {
                    dataset = homeMonitor.thingSpeak.GetTemperature();
                }
                else
                {
                    dataset = homeMonitor.thingSpeak.GetHumidity();
                }

                ImPlot::PushStyleColor(0, homeMonitor.assignedColor.assignedColorRgb);
                ImPlot::PlotLine(homeMonitor.thingSpeak.GetName().c_str(),
                                dataset->xAxisData, dataset->yAxisData,
                                dataset->numDataPoints, ImPlotLegendFlags_NoButtons);
                ImPlot::PopStyleColor();
            }
        }

        if (ImPlot::IsPlotHovered())
        {
            HomeMonitorDrawVerticalCursor();

            std::pair<int, int> closestIndicies =
                HomeMonitorGetClosestPointToMouse(field, homeMonitors);

            if (closestIndicies.first != -1)
            {
                auto homeMonitor = homeMonitors[closestIndicies.first];
                auto entryId = closestIndicies.second;

                if (field == ThingSpeakField::Temperature)
                {
                    dataset = homeMonitor.thingSpeak.GetTemperature();
                }
                else
                {
                    dataset = homeMonitor.thingSpeak.GetHumidity();
                }

                ImGui::BeginTooltip();
                ImGui::Text("Trendline: %s", homeMonitor.thingSpeak.GetName().c_str());
                ImGui::Text("Entry ID: %d", entryId);
                ImGui::Text("%s: %.2f", name.c_str(), dataset->yAxisData[entryId]);
                ImGui::Text("Date/Time Captured (PST): %s", dataset->timestamp[entryId].c_str());
                ImGui::EndTooltip();
            }
        }

        ImPlot::EndPlot();
    }
    ImPlot::PopStyleVar();
    ImGui::End();
}

/**
 * @brief Assign a unique color for a HomeMonitor object
 * 
 * @param homeMonitor - Object to assign the color to
 * @return bool - True if a color is 
 */
bool HomeMonitorSetColor(HomeMonitor_t& homeMonitor)
{
    static std::vector<ColorOption_t> colorOptions =
    {
        // Blue
        {IM_COL32(  0, 114, 189, 255), ImVec4(0.0f,   0.447f, 0.741f, 1.0f), true},
        // Orange
        {IM_COL32(217, 120,   0, 255), ImVec4(0.851f, 0.471f, 0.0f,   1.0f), true},
        // Green
        {IM_COL32(119, 172,  48, 255), ImVec4(0.467f, 0.675f, 0.188f, 1.0f), true},
        // Purple
        {IM_COL32(126,  47, 142, 255), ImVec4(0.494f, 0.184f, 0.557f, 1.0f), true},
        // Yellow
        {IM_COL32(237, 177,  32, 255), ImVec4(0.929f, 0.694f, 0.125f, 1.0f), true},
    };

    uint16_t const maxRgbValue = 255;

    bool colorSelected = false;

    for (auto& option : colorOptions)
    {
        if (option.available)
        {
            #if (DEBUG_HOMEMONITOR)
            std::cout << "Assigning color: " << std::endl;
            std::cout << option.colorRgb.w << ", " << option.colorRgb.x << ", "
                      << option.colorRgb.y << ", " << option.colorRgb.z << ", " << std::endl;
            #endif
            homeMonitor.assignedColor.assignedColorRgba = option.colorRgba;
            homeMonitor.assignedColor.assignedColorRgb = option.colorRgb;

            colorSelected = true;
            option.available = false;

            break;
        }
    }

    return colorSelected;
}

/**
 * @brief Draw vertical bar at cursor on plot this function is called within
 * 
 */
void HomeMonitorDrawVerticalCursor()
{
    ImDrawList* draw_list = ImPlot::GetPlotDrawList();
    ImPlotPoint mouse   = ImPlot::GetPlotMousePos();
    mouse.x             = std::round(mouse.x);
    float  tool_l       = ImPlot::PlotToPixels(mouse.x - 0.25 * 1.5, mouse.y).x;
    float  tool_r       = ImPlot::PlotToPixels(mouse.x + 0.25 * 1.5, mouse.y).x;
    float  tool_t       = ImPlot::GetPlotPos().y;
    float  tool_b       = tool_t + ImPlot::GetPlotSize().y;
    ImPlot::PushPlotClipRect();
    draw_list->AddRectFilled(ImVec2(tool_l, tool_t), ImVec2(tool_r, tool_b), IM_COL32(255,0,0,32));
    ImPlot::PopPlotClipRect();
}

/**
 * @brief Draw horizontal line with spacing above/below.
 *        Used to visually separate sections of GUI
 * 
 */
void HomeMonitorDrawHorizontalLine()
{
    float const spacing = 10.0;
    float const margin  = 20.0;

    ImGui::Dummy(ImVec2(0.0f, spacing));

    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    ImVec2 start = ImGui::GetCursorScreenPos();
    ImVec2 end = ImVec2((start.x + ImGui::GetWindowWidth() - margin), start.y);
    draw_list->AddLine(start, end, IM_COL32(128, 128, 128, 60), 0.5f);

    ImGui::Dummy(ImVec2(0.0f, spacing));
}

/**
 * @brief Determine the closest point to the cursor from the set of points
 *        currently marked visible in the graph
 * 
 * @param homeMonitors - Collection of HomeMonitor objects to traverse
 * @param field - Type of field data to search
 * @return std::pair<int, int> - Pair containing:
 *                               (1) The HomeMonitor object with the nearest point,
 *                               (2) The index of the closest point from the
 *                                   points stored in the HomeMonitor object
 *                               Or a negative pair {-1, -1} if not point exists
 */
std::pair<int, int> HomeMonitorGetClosestPointToMouse(ThingSpeakField field,
                                                      std::vector<HomeMonitor_t>& homeMonitors)
{
    int xDistanceRounded;
    float yDistance;
    float yMinDistance = FLT_MAX;

    std::pair<int, int> closestValue = {-1, -1};
    ThingSpeakFeedData_t const * dataset;

    ImPlotPoint mousePos = ImPlot::GetPlotMousePos();

    for (int i = 0; i < homeMonitors.size(); i++)
    {
        if (!homeMonitors[i].displayData)
        {
            // Data is not visible to user, and should not be considered
            continue;
        }

        if (field == ThingSpeakField::Temperature)
        {
            dataset = homeMonitors[i].thingSpeak.GetTemperature();
        }
        else
        {
            dataset = homeMonitors[i].thingSpeak.GetHumidity();
        }

        for (int j = 0; j < dataset->numDataPoints; j++)
        {
            xDistanceRounded = std::abs(std::round(mousePos.x - dataset->xAxisData[j]));
            if (xDistanceRounded != 0)
            {
                // Data point is not in the same column as cursor
                continue;
            }

            yDistance = std::abs(mousePos.y - dataset->yAxisData[j]);
            if (yDistance < yMinDistance)
            {
                closestValue.first = i;
                closestValue.second = j;

                yMinDistance = yDistance;
            }
        }
    }

    return closestValue;
}

/**
 * @brief Determine upper and lower Y-axis (vertical) boundaries based on
 *        visible data
 * 
 * @return std::pair<float, float> - Min, Max Y-Axis boundaries
 */
std::pair<float, float> HomeMonitorGetYAxisBoundaries(ThingSpeakField field,
                                                  std::vector<HomeMonitor_t>& homeMonitors)
{
    float yMin = FLT_MAX;
    float yMax = FLT_MIN;

    ThingSpeakFeedData_t const * dataset;

    for (auto& homeMonitor : homeMonitors)
    {
        if (field == ThingSpeakField::Temperature)
        {
            dataset = homeMonitor.thingSpeak.GetTemperature();
        }
        else
        {
            dataset = homeMonitor.thingSpeak.GetHumidity();
        }

        if (homeMonitor.displayData)
        {
            for (int i = 0; i < dataset->numDataPoints; i++)
            {
                yMin = std::min(dataset->yAxisData[i], yMin);
                yMax = std::max(dataset->yAxisData[i], yMax);
            }
        }
    }

    return {yMin, yMax};
}

/**
 * @brief Define, register, and instantiate Win32 window instance with icon
 * 
 * @param hwnd - ID of created window
 * @param windowClass - Window Class object (i.e. window attributes)
 */
void Win32RegisterAndCreateWindow(HWND& hwnd, WNDCLASSEXW& windowClass)
{
    // Define application window
    HICON hIcon = static_cast<HICON>(::LoadImage(
        GetModuleHandle(nullptr),
        MAKEINTRESOURCE(IDI_ICON),
        IMAGE_ICON,
        0, 0,
        LR_DEFAULTCOLOR
    ));

    windowClass = {
        sizeof(WNDCLASSEXW),        // Size of struct
        CS_CLASSDC,                 // Allocate one shared device context
        WndProc,                    // Pointer to window procedure
        0L, 0L,                     // Do not allocate any extra bytes
        GetModuleHandle(nullptr),   // No handle neded
        hIcon,                      // Use custom icon
        nullptr,                    // Manually set cursor shape
        nullptr,                    // Manually paint background
        nullptr,                    // No default menu
        L"HomeMonitor",             // Window name
        hIcon                       // Use custom small icon
    };
    ::RegisterClassExW(&windowClass);

    // Create application window using attributes defined above
    hwnd = ::CreateWindowW(windowClass.lpszClassName,
                           L"HomeMonitor",
                           WS_OVERLAPPEDWINDOW,
                           100, 100,
                           CW_USEDEFAULT, CW_USEDEFAULT,
                           nullptr,
                           nullptr,
                           windowClass.hInstance,
                           nullptr
    );
}

/**
 * @brief Win32 Message Handler
 * 
 *        You can read the io.WantCaptureMouse, io.WantCaptureKeyboard flags to
 *        tell if dear imgui wants to use your inputs.
 * 
 *        - When io.WantCaptureMouse is true, do not dispatch mouse input data
 *          to your main application, or clear/overwrite your copy of the mouse data.
 *        - When io.WantCaptureKeyboard is true, do not dispatch keyboard input data
 *          to your main application, or clear/overwrite your copy of the keyboard data.
 * 
 *        Generally you may always pass all inputs to dear imgui, and hide them from
 *        your application based on those two flags.
 * 
 * @param hWnd 
 * @param msg 
 * @param wParam 
 * @param lParam 
 * 
 * @return Result of message handler 
 */
LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam))
    {
        return true;
    }

    bool returnZero = false;

    switch (msg)
    {
        case WM_SIZE:
            // Attempt to resize frame to be rendered in swapchain
            if (g_pd3dDevice != nullptr && wParam != SIZE_MINIMIZED)
            {
                WaitForLastSubmittedFrame();
                CleanupRenderTarget();
                HRESULT result = g_pSwapChain->ResizeBuffers(0, (UINT)LOWORD(lParam),
                                                             (UINT)HIWORD(lParam), DXGI_FORMAT_UNKNOWN,
                                                             DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT);
                assert(SUCCEEDED(result) && "Failed to resize swapchain.");
                CreateRenderTarget();
            }

            returnZero = true;
        case WM_SYSCOMMAND:
            // Disable ALT application menu
            if ((wParam & 0xfff0) == SC_KEYMENU)
            {
                returnZero = true;
            }
            break;
        case WM_DESTROY:
            ::PostQuitMessage(0);
            return 0;
    }

    return ((returnZero) ? 0 : (::DefWindowProcW(hWnd, msg, wParam, lParam)));
}

/* WndProc Handler Helper Functions */
bool CreateDeviceD3D(HWND hWnd)
{
    // Setup swap chain
    DXGI_SWAP_CHAIN_DESC1 sd;
    {
        ZeroMemory(&sd, sizeof(sd));
        sd.BufferCount = APP_NUM_BACK_BUFFERS;
        sd.Width = 0;
        sd.Height = 0;
        sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        sd.Flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
        sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        sd.SampleDesc.Count = 1;
        sd.SampleDesc.Quality = 0;
        sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        sd.AlphaMode = DXGI_ALPHA_MODE_UNSPECIFIED;
        sd.Scaling = DXGI_SCALING_STRETCH;
        sd.Stereo = FALSE;
    }

    // Create device
    D3D_FEATURE_LEVEL featureLevel = D3D_FEATURE_LEVEL_11_0;
    if (D3D12CreateDevice(nullptr, featureLevel,
                          IID_PPV_ARGS(&g_pd3dDevice)) != S_OK)
    {
        return false;
    }

    {
        D3D12_DESCRIPTOR_HEAP_DESC desc = {};
        desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        desc.NumDescriptors = APP_NUM_BACK_BUFFERS;
        desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
        desc.NodeMask = 1;
        if (g_pd3dDevice->CreateDescriptorHeap(&desc,
                                               IID_PPV_ARGS(&g_pd3dRtvDescHeap)) != S_OK)
        {
            return false;
        }

        SIZE_T rtvDescriptorSize =
            g_pd3dDevice->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle =
            g_pd3dRtvDescHeap->GetCPUDescriptorHandleForHeapStart();

        for (UINT i = 0; i < APP_NUM_BACK_BUFFERS; i++)
        {
            g_mainRenderTargetDescriptor[i] = rtvHandle;
            rtvHandle.ptr += rtvDescriptorSize;
        }
    }

    {
        D3D12_DESCRIPTOR_HEAP_DESC desc = {};
        desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        desc.NumDescriptors = APP_SRV_HEAP_SIZE;
        desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        if (g_pd3dDevice->CreateDescriptorHeap(&desc,
                                               IID_PPV_ARGS(&g_pd3dSrvDescHeap)) != S_OK)
        {
            return false;
        }
        g_pd3dSrvDescHeapAlloc.Create(g_pd3dDevice, g_pd3dSrvDescHeap);
    }

    {
        D3D12_COMMAND_QUEUE_DESC desc = {};
        desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        desc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
        desc.NodeMask = 1;
        if (g_pd3dDevice->CreateCommandQueue(&desc,
                                             IID_PPV_ARGS(&g_pd3dCommandQueue)) != S_OK)
        {
            return false;
        }
    }

    for (UINT i = 0; i < APP_NUM_FRAMES_IN_FLIGHT; i++)
    {
        if (g_pd3dDevice->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                 IID_PPV_ARGS(&g_frameContext[i].CommandAllocator)) != S_OK)
        {
            return false;
        }
    }

    bool createCommandList = g_pd3dDevice->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                             g_frameContext[0].CommandAllocator,
                                                             nullptr, IID_PPV_ARGS(&g_pd3dCommandList));
    if ((createCommandList != S_OK) || (g_pd3dCommandList->Close() != S_OK))
    {
        return false;
    }

    if (g_pd3dDevice->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_fence)) != S_OK)
        return false;

    g_fenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    if (g_fenceEvent == nullptr)
        return false;

    {
        IDXGIFactory4* dxgiFactory = nullptr;
        IDXGISwapChain1* swapChain1 = nullptr;
        if (CreateDXGIFactory1(IID_PPV_ARGS(&dxgiFactory)) != S_OK)
        {
            return false;
        }
        if (dxgiFactory->CreateSwapChainForHwnd(g_pd3dCommandQueue,
                                                hWnd, &sd, nullptr,
                                                nullptr, &swapChain1) != S_OK)
        {
            return false;
        }
        if (swapChain1->QueryInterface(IID_PPV_ARGS(&g_pSwapChain)) != S_OK)
        {
            return false;
        }
        swapChain1->Release();
        dxgiFactory->Release();
        g_pSwapChain->SetMaximumFrameLatency(APP_NUM_BACK_BUFFERS);
        g_hSwapChainWaitableObject = g_pSwapChain->GetFrameLatencyWaitableObject();
    }

    CreateRenderTarget();
    return true;
}

void CleanupDeviceD3D()
{
    CleanupRenderTarget();
    if (g_pSwapChain)
    {
        g_pSwapChain->SetFullscreenState(false, nullptr);
        g_pSwapChain->Release();
        g_pSwapChain = nullptr;
    }

    if (g_hSwapChainWaitableObject != nullptr)
    {
        CloseHandle(g_hSwapChainWaitableObject);
    }

    for (UINT i = 0; i < APP_NUM_FRAMES_IN_FLIGHT; i++)
    {
        if (g_frameContext[i].CommandAllocator)
        {
            g_frameContext[i].CommandAllocator->Release();
            g_frameContext[i].CommandAllocator = nullptr;
        }
    }    

    if (g_pd3dCommandQueue)
    {
        g_pd3dCommandQueue->Release();
        g_pd3dCommandQueue = nullptr;
    }
    if (g_pd3dCommandList)
    {
        g_pd3dCommandList->Release();
        g_pd3dCommandList = nullptr;
    }
    if (g_pd3dRtvDescHeap)
    {
        g_pd3dRtvDescHeap->Release();
        g_pd3dRtvDescHeap = nullptr;
    }
    if (g_pd3dSrvDescHeap)
    {
        g_pd3dSrvDescHeap->Release();
        g_pd3dSrvDescHeap = nullptr;
    }
    if (g_fence)
    {
        g_fence->Release();
        g_fence = nullptr;
    }
    if (g_fenceEvent)
    {
        CloseHandle(g_fenceEvent);
        g_fenceEvent = nullptr;
    }
    if (g_pd3dDevice)
    {
        g_pd3dDevice->Release();
        g_pd3dDevice = nullptr;
    }
}

void CreateRenderTarget()
{
    for (UINT i = 0; i < APP_NUM_BACK_BUFFERS; i++)
    {
        ID3D12Resource* pBackBuffer = nullptr;
        g_pSwapChain->GetBuffer(i, IID_PPV_ARGS(&pBackBuffer));
        g_pd3dDevice->CreateRenderTargetView(pBackBuffer, nullptr, g_mainRenderTargetDescriptor[i]);
        g_mainRenderTargetResource[i] = pBackBuffer;
    }
}

void CleanupRenderTarget()
{
    WaitForLastSubmittedFrame();

    for (UINT i = 0; i < APP_NUM_BACK_BUFFERS; i++)
    {
        if (g_mainRenderTargetResource[i]) {
            g_mainRenderTargetResource[i]->Release();
            g_mainRenderTargetResource[i] = nullptr;
        }
    }
}

void WaitForLastSubmittedFrame()
{
    FrameContext* frameCtx = &g_frameContext[g_frameIndex % APP_NUM_FRAMES_IN_FLIGHT];

    UINT64 fenceValue = frameCtx->FenceValue;
    if (fenceValue == 0)
    {
        // No fence was signaled
        return;
    }

    frameCtx->FenceValue = 0;
    if (g_fence->GetCompletedValue() >= fenceValue)
    {
        return;
    }

    g_fence->SetEventOnCompletion(fenceValue, g_fenceEvent);
    WaitForSingleObject(g_fenceEvent, INFINITE);
}

FrameContext* WaitForNextFrameResources()
{
    UINT nextFrameIndex = g_frameIndex + 1;
    g_frameIndex = nextFrameIndex;

    HANDLE waitableObjects[] = {
        g_hSwapChainWaitableObject,
        nullptr
    };
    DWORD numWaitableObjects = 1;

    FrameContext* frameCtx = &g_frameContext[nextFrameIndex % APP_NUM_FRAMES_IN_FLIGHT];
    UINT64 fenceValue = frameCtx->FenceValue;
    if (fenceValue != 0)
    {
        // means no fence was signaled
        frameCtx->FenceValue = 0;
        g_fence->SetEventOnCompletion(fenceValue, g_fenceEvent);
        waitableObjects[1] = g_fenceEvent;
        numWaitableObjects = 2;
    }

    WaitForMultipleObjects(numWaitableObjects, waitableObjects, TRUE, INFINITE);

    return frameCtx;
}

/* ImPlot Graph Styles */
void HomeMonitorGraphStyleLight()
{
    ImPlotStyle& style              = ImPlot::GetStyle();

    ImVec4* colors                  = style.Colors;
    colors[ImPlotCol_Line]          = IMPLOT_AUTO_COL;
    colors[ImPlotCol_Fill]          = IMPLOT_AUTO_COL;
    colors[ImPlotCol_MarkerOutline] = IMPLOT_AUTO_COL;
    colors[ImPlotCol_MarkerFill]    = IMPLOT_AUTO_COL;
    colors[ImPlotCol_ErrorBar]      = ImVec4(0.00f, 0.00f, 0.00f, 1.00f);
    colors[ImPlotCol_FrameBg]       = ImVec4(1.00f, 1.00f, 1.00f, 1.00f);
    colors[ImPlotCol_PlotBg]        = ImVec4(0.95f, 0.95f, 0.95f, 1.00f);
    colors[ImPlotCol_PlotBorder]    = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
    colors[ImPlotCol_LegendBg]      = ImVec4(0.92f, 0.92f, 0.95f, 1.00f);
    colors[ImPlotCol_LegendBorder]  = ImVec4(0.80f, 0.81f, 0.85f, 1.00f);
    colors[ImPlotCol_LegendText]    = ImVec4(0.00f, 0.00f, 0.00f, 1.00f);
    colors[ImPlotCol_TitleText]     = ImVec4(0.00f, 0.00f, 0.00f, 1.00f);
    colors[ImPlotCol_InlayText]     = ImVec4(0.00f, 0.00f, 0.00f, 1.00f);
    colors[ImPlotCol_AxisText]      = ImVec4(0.00f, 0.00f, 0.00f, 1.00f);
    colors[ImPlotCol_AxisGrid]      = ImVec4(1.00f, 1.00f, 1.00f, 1.00f);
    colors[ImPlotCol_AxisBgHovered] = ImVec4(0.92f, 0.92f, 0.95f, 1.00f);
    colors[ImPlotCol_AxisBgActive]  = ImVec4(0.92f, 0.92f, 0.95f, 0.75f);
    colors[ImPlotCol_Selection]     = ImVec4(1.00f, 0.65f, 0.00f, 1.00f);
    colors[ImPlotCol_Crosshairs]    = ImVec4(0.23f, 0.10f, 0.64f, 0.50f);

    style.LineWeight       = 1.5;
    style.Marker           = ImPlotMarker_Circle;
    style.MarkerSize       = 4;
    style.MarkerWeight     = 1;
    style.FillAlpha        = 1.0f;
    style.ErrorBarSize     = 5;
    style.ErrorBarWeight   = 1.5f;
    style.DigitalBitHeight = 8;
    style.DigitalBitGap    = 4;
    style.PlotBorderSize   = 0;
    style.MinorAlpha       = 1.0f;
    style.MajorTickLen     = ImVec2(0,0);
    style.MinorTickLen     = ImVec2(0,0);
    style.MajorTickSize    = ImVec2(0,0);
    style.MinorTickSize    = ImVec2(0,0);
    style.MajorGridSize    = ImVec2(1.2f,1.2f);
    style.MinorGridSize    = ImVec2(1.2f,1.2f);
    style.PlotPadding      = ImVec2(12,12);
    style.LabelPadding     = ImVec2(5,5);
    style.LegendPadding    = ImVec2(5,5);
    style.MousePosPadding  = ImVec2(5,5);
    style.PlotMinSize      = ImVec2(300,225);
}

void HomeMonitorGraphStyleDark()
{
    ImPlotStyle& style = ImPlot::GetStyle();

    ImVec4* colors = style.Colors;
    colors[ImPlotCol_Line]          = IMPLOT_AUTO_COL;
    colors[ImPlotCol_Fill]          = IMPLOT_AUTO_COL;
    colors[ImPlotCol_MarkerOutline] = IMPLOT_AUTO_COL;
    colors[ImPlotCol_MarkerFill]    = IMPLOT_AUTO_COL;
    colors[ImPlotCol_ErrorBar]      = ImVec4(1.00f, 1.00f, 1.00f, 1.00f);
    colors[ImPlotCol_FrameBg]       = ImVec4(0.10f, 0.10f, 0.10f, 1.00f);
    colors[ImPlotCol_PlotBg]        = ImVec4(0.15f, 0.15f, 0.17f, 1.00f);
    colors[ImPlotCol_PlotBorder]    = ImVec4(0.50f, 0.50f, 0.50f, 0.50f);
    colors[ImPlotCol_LegendBg]      = ImVec4(0.15f, 0.15f, 0.15f, 1.00f);
    colors[ImPlotCol_LegendBorder]  = ImVec4(0.50f, 0.50f, 0.50f, 1.00f);
    colors[ImPlotCol_LegendText]    = ImVec4(1.00f, 1.00f, 1.00f, 1.00f);
    colors[ImPlotCol_TitleText]     = ImVec4(1.00f, 1.00f, 1.00f, 1.00f);
    colors[ImPlotCol_InlayText]     = ImVec4(1.00f, 1.00f, 1.00f, 1.00f);
    colors[ImPlotCol_AxisText]      = ImVec4(1.00f, 1.00f, 1.00f, 1.00f);
    colors[ImPlotCol_AxisGrid]      = ImVec4(0.20f, 0.20f, 0.20f, 1.00f);
    colors[ImPlotCol_AxisBgHovered] = ImVec4(0.30f, 0.30f, 0.30f, 1.00f);
    colors[ImPlotCol_AxisBgActive]  = ImVec4(0.30f, 0.30f, 0.30f, 0.75f);
    colors[ImPlotCol_Selection]     = ImVec4(1.00f, 0.65f, 0.00f, 1.00f);
    colors[ImPlotCol_Crosshairs]    = ImVec4(1.00f, 1.00f, 1.00f, 0.50f);

    style.LineWeight       = 1.5;
    style.Marker           = ImPlotMarker_Circle;
    style.MarkerSize       = 4;
    style.MarkerWeight     = 1;
    style.FillAlpha        = 1.0f;
    style.ErrorBarSize     = 5;
    style.ErrorBarWeight   = 1.5f;
    style.DigitalBitHeight = 8;
    style.DigitalBitGap    = 4;
    style.PlotBorderSize   = 0;
    style.MinorAlpha       = 1.0f;
    style.MajorTickLen     = ImVec2(0,0);
    style.MinorTickLen     = ImVec2(0,0);
    style.MajorTickSize    = ImVec2(0,0);
    style.MinorTickSize    = ImVec2(0,0);
    style.MajorGridSize    = ImVec2(1.2f,1.2f);
    style.MinorGridSize    = ImVec2(1.2f,1.2f);
    style.PlotPadding      = ImVec2(12,12);
    style.LabelPadding     = ImVec2(5,5);
    style.LegendPadding    = ImVec2(5,5);
    style.MousePosPadding  = ImVec2(5,5);
    style.PlotMinSize      = ImVec2(300,225);
}

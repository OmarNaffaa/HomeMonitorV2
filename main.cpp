// HomeMonitor V2: Viewing Application using WIN32 + DX12
//
// [2024/12/23]
#include <d3d12.h>
#include <dxgi1_4.h>
#include <tchar.h>
#include <string>

#include "Imgui/imgui.h"
#include "Imgui/imgui_impl_win32.h"
#include "Imgui/imgui_impl_dx12.h"
#include "Imgui/implot.h"

#include "Resources/resource.h"

#include "ThingSpeak/ThingSpeak.h"

#define HOMEMONITOR_DARK_MODE   true
#define HOMEMONITOR_USE_VSYNC   true

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

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);


int main(int argc, char** argv)
{
    ImGui_ImplWin32_EnableDpiAwareness();

    // Define application window
    HICON hIcon = static_cast<HICON>(::LoadImage(
        GetModuleHandle(nullptr),
        MAKEINTRESOURCE(IDI_ICON),
        IMAGE_ICON,
        0, 0,
        LR_DEFAULTCOLOR
    ));

    WNDCLASSEXW windowClass = {
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
    HWND hwnd = ::CreateWindowW(windowClass.lpszClassName,
                                L"HomeMonitor",
                                WS_OVERLAPPEDWINDOW,
                                100, 100,
                                CW_USEDEFAULT, CW_USEDEFAULT,
                                nullptr,
                                nullptr,
                                windowClass.hInstance,
                                nullptr
    );

    // Initialize Direct3D
    if (!CreateDeviceD3D(hwnd))
    {
        CleanupDeviceD3D();
        ::UnregisterClassW(windowClass.lpszClassName, windowClass.hInstance);
        return 1;
    }

    // Show the window
    ::ShowWindow(hwnd, SW_SHOWDEFAULT);
    ::UpdateWindow(hwnd);

    // Setup context
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO(); (void)io;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;     // Enable Keyboard Controls
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;      // Enable Gamepad Controls
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;         // Enable Docking
    io.ConfigFlags |= ImGuiConfigFlags_ViewportsEnable;       // Enable Multi-Viewport / Platform Windows
    //io.ConfigViewportsNoAutoMerge = true;
    //io.ConfigViewportsNoTaskBarIcon = true;

    ImPlot::CreateContext();

    // Setup Dear ImGui style
    #if (HOMEMONITOR_DARK_MODE)
    ImGui::StyleColorsDark();
    ImVec4 clearColor = ImVec4(0.2f, 0.2f, 0.2f, 1.00f);
    auto invisibleColor = IM_COL32(255, 0, 0, 255);
    #else
    ImGui::StyleColorsLight();
    ImVec4 clearColor = ImVec4(0.8f, 0.8f, 0.8f, 1.00f);
    #endif

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
    io.Fonts->AddFontFromFileTTF(font_file, 22.0f);

    // Start rendering loop
    bool done = false;

    ThingSpeak ts("1277292", "I4BV5Q70NNDWH0SP");
    ts.GetChannelData(48);

    while (!done)
    {
        // Poll and handle messages (inputs, window resize, etc.)
        // See the WndProc() function below for our to dispatch events to the Win32 backend.
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

        // Create Homemonitor control window
        ImGui::Begin("Controls");
        ImGui::Text("First test with Imgui library");
        ImGui::Text("Application average %.3f ms/frame (%.1f FPS)",
                     1000.0f / io.Framerate, io.Framerate);
        ImGui::End();

        // Create Homemonitor plotting window
        ImGui::Begin("Lines");
        static float xs1[1001], ys1[1001];
        for (int i = 0; i < 1001; ++i) {
            xs1[i] = i * 0.001f;
            ys1[i] = 0.5f + 0.5f * sinf(50 * (xs1[i] + (float)ImGui::GetTime() / 10));
        }
        static double xs2[20], ys2[20];
        for (int i = 0; i < 20; ++i) {
            xs2[i] = i * 1/19.0f;
            ys2[i] = xs2[i] * xs2[i];
        }
        ImVec2 maxWindowSize(-1, -1);
        if (ImPlot::BeginPlot("Line Plots", maxWindowSize)) {
            ImPlot::SetupAxes("x","y");
            ImPlot::PlotLine("f(x)", xs1, ys1, 1001);
            ImPlot::SetNextMarkerStyle(ImPlotMarker_Circle);
            ImPlot::PlotLine("g(x)", xs2, ys2, 20,ImPlotLineFlags_Segments);
            ImPlot::EndPlot();
        }
        ImGui::End();

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
        const float clearColor_with_alpha[4] = {
            clearColor.x * clearColor.w,
            clearColor.y * clearColor.w,
            clearColor.z * clearColor.w,
            clearColor.w
        };
        g_pd3dCommandList->ClearRenderTargetView(g_mainRenderTargetDescriptor[backBufferIdx],
                                                 clearColor_with_alpha, 0, nullptr);
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

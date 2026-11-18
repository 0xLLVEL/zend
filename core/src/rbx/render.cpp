#include "rbx/render.hpp"

#include <Windows.h>

#include <d3d11.h>
#include <dxgi.h>

#include <imgui.h>
#include <imgui_impl_dx11.h>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <new>

#include "rbx/drawing.hpp"
#include "rbx/memory.hpp"
#include "rbx/offsets.hpp"
#include "rbx/scheduler.hpp"
#include "util/log.hpp"

// Overlay renderer: find RenderJob's swapchain through the current view and
// device, clone its vtable, and draw Drawing objects in Present. ResizeBuffers
// drops the render target before DXGI resizes the buffers. No queries against
// page protections: every pointer read goes through rbx::memory.

namespace zend::rbx::render {
namespace {

// RenderJob -> RenderView -> device -> swapchain. Values come from the pinned
// build manifest (ClientOffsets::RenderView in the reference tree).
constexpr std::uint32_t kRenderJobToView = 0x1E0;
constexpr std::uint32_t kViewToDevice = 0x10;
constexpr std::uint32_t kDeviceToSwapChain = 0x88;
constexpr std::uint32_t kVtableSlots = 32; // base + IDXGISwapChain1..4

using PresentFn = HRESULT(WINAPI*)(IDXGISwapChain*, UINT, UINT);
using ResizeFn = HRESULT(WINAPI*)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);

HRESULT WINAPI detail_present_hook(IDXGISwapChain* swapchain, UINT sync, UINT flags);
HRESULT WINAPI detail_resize_hook(IDXGISwapChain* swapchain, UINT buffer_count,
                                  UINT width, UINT height, DXGI_FORMAT format,
                                  UINT flags);

std::atomic<bool> g_stopping{false};
HANDLE g_thread = nullptr;

std::mutex g_hook_mutex;
std::uintptr_t g_swapchain = 0;
std::uintptr_t g_original_vtable = 0;
std::uintptr_t* g_clone = nullptr;

std::atomic<std::uintptr_t> g_active{0};
std::atomic<std::uintptr_t> g_present_original{0};
std::atomic<std::uintptr_t> g_resize_original{0};

// Owned by the present thread once the first frame initializes ImGui.
ImGuiContext* g_imgui = nullptr;
ID3D11Device* g_device = nullptr;
ID3D11DeviceContext* g_context = nullptr;
ID3D11RenderTargetView* g_rtv = nullptr;
std::uintptr_t g_rtv_chain = 0;
std::uint32_t g_rtv_width = 0;
std::uint32_t g_rtv_height = 0;
DXGI_FORMAT g_rtv_format = DXGI_FORMAT_UNKNOWN;
std::uintptr_t g_failed_device = 0;
std::atomic<bool> g_overlay_disabled{false};

std::uintptr_t resolve_swapchain() {
    const std::uintptr_t job = find_job("RenderJob");
    if (!job) return 0;

    std::uintptr_t view = 0;
    std::uintptr_t device = 0;
    std::uintptr_t swapchain = 0;
    if (!read_ptr(job + kRenderJobToView, view) || !is_user_ptr(view) ||
        !read_ptr(view + kViewToDevice, device) || !is_user_ptr(device) ||
        !read_ptr(device + kDeviceToSwapChain, swapchain) ||
        !is_user_ptr(swapchain)) {
        return 0;
    }
    return swapchain;
}

bool install_hook(std::uintptr_t swapchain) {
    std::lock_guard<std::mutex> lock(g_hook_mutex);

    std::uintptr_t vtable = 0;
    if (!read_ptr(swapchain, vtable) || !is_user_ptr(vtable)) return false;

    if (swapchain == g_swapchain && g_clone) {
        if (vtable == reinterpret_cast<std::uintptr_t>(g_clone)) return true;
    }

    std::uintptr_t slots[kVtableSlots] = {};
    for (std::uint32_t i = 0; i < kVtableSlots; ++i) {
        if (!read_ptr(vtable + i * sizeof(std::uintptr_t), slots[i])) return false;
    }
    if (!is_user_ptr(slots[8]) || !is_user_ptr(slots[13])) return false;

    auto* clone = new (std::nothrow) std::uintptr_t[kVtableSlots];
    if (!clone) return false;
    std::memcpy(clone, slots, sizeof(slots));
    clone[8] = reinterpret_cast<std::uintptr_t>(&detail_present_hook);
    clone[13] = reinterpret_cast<std::uintptr_t>(&detail_resize_hook);

    const std::uintptr_t old_present = g_present_original.load();
    const std::uintptr_t old_resize = g_resize_original.load();
    const std::uintptr_t old_active = g_active.load();

    // Publish the originals before the game can enter the new hook.
    g_present_original.store(slots[8]);
    g_resize_original.store(slots[13]);
    g_active.store(swapchain);

    if (!write_ptr(swapchain, reinterpret_cast<std::uintptr_t>(clone))) {
        g_present_original.store(old_present);
        g_resize_original.store(old_resize);
        g_active.store(old_active);
        delete[] clone;
        return false;
    }

    // Retire the previous clone. It is intentionally leaked: a Present call
    // that already loaded its pointer must keep executing valid code.
    if (g_swapchain && g_clone) {
        std::uintptr_t current = 0;
        if (read_ptr(g_swapchain, current) &&
            current == reinterpret_cast<std::uintptr_t>(g_clone)) {
            write_ptr(g_swapchain, g_original_vtable);
        }
    }

    g_swapchain = swapchain;
    g_original_vtable = vtable;
    g_clone = clone;
    logf("render: hooked swapchain=%p vtable=%p present=%p resize=%p",
         reinterpret_cast<void*>(swapchain),
         reinterpret_cast<void*>(vtable),
         reinterpret_cast<void*>(slots[8]),
         reinterpret_cast<void*>(slots[13]));
    return true;
}

void teardown_renderer() {
    // Stop Lua from touching font data before the context goes away.
    drawing::reset_renderer();

    if (g_rtv) {
        g_rtv->Release();
        g_rtv = nullptr;
    }
    g_rtv_chain = 0;
    g_rtv_width = 0;
    g_rtv_height = 0;
    g_rtv_format = DXGI_FORMAT_UNKNOWN;

    if (g_imgui) {
        ImGui::SetCurrentContext(g_imgui);
        ImGui_ImplDX11_Shutdown();
        ImGui::DestroyContext(g_imgui);
        g_imgui = nullptr;
    }
    if (g_context) {
        g_context->Release();
        g_context = nullptr;
    }
    if (g_device) {
        g_device->Release();
        g_device = nullptr;
    }
    g_failed_device = 0;
}

bool ensure_imgui(IDXGISwapChain* swapchain) {
    ID3D11Device* device = nullptr;
    if (FAILED(swapchain->GetDevice(__uuidof(ID3D11Device),
                                    reinterpret_cast<void**>(&device))) ||
        !device) {
        return false;
    }

    if (g_imgui && device == g_device) {
        device->Release();
        if (ImGui::GetCurrentContext() != g_imgui) {
            ImGui::SetCurrentContext(g_imgui);
        }
        return true;
    }

    const std::uintptr_t candidate = reinterpret_cast<std::uintptr_t>(device);
    if (candidate == g_failed_device) {
        device->Release();
        return false;
    }

    if (g_imgui || g_device) {
        logf("render: device changed (%p -> %p), reinitializing overlay",
             static_cast<void*>(g_device), static_cast<void*>(device));
        teardown_renderer();
    }

    ID3D11DeviceContext* context = nullptr;
    device->GetImmediateContext(&context);
    if (!context) {
        g_failed_device = candidate;
        device->Release();
        log("render: no immediate context");
        return false;
    }

    ImGuiContext* imgui = ImGui::CreateContext();
    if (!imgui) {
        context->Release();
        g_failed_device = candidate;
        device->Release();
        return false;
    }
    ImGui::SetCurrentContext(imgui);
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;
    io.Fonts->AddFontDefault();

    if (!ImGui_ImplDX11_Init(device, context)) {
        ImGui::DestroyContext(imgui);
        context->Release();
        g_failed_device = candidate;
        device->Release();
        return false;
    }

    g_device = device;
    g_context = context;
    g_imgui = imgui;
    logf("render: imgui initialized (device=%p context=%p)",
         static_cast<void*>(device), static_cast<void*>(context));
    return true;
}

void ensure_target(IDXGISwapChain* swapchain) {
    if (!g_device) return;

    ID3D11Texture2D* backbuffer = nullptr;
    if (FAILED(swapchain->GetBuffer(0, __uuidof(ID3D11Texture2D),
                                    reinterpret_cast<void**>(&backbuffer))) ||
        !backbuffer) {
        return;
    }

    D3D11_TEXTURE2D_DESC desc{};
    backbuffer->GetDesc(&desc);

    // A new swapchain can have the same size and format as the old one, but its
    // backbuffer belongs to a different object, so the RTV must be rebuilt.
    if (g_rtv && (reinterpret_cast<std::uintptr_t>(swapchain) != g_rtv_chain ||
                  desc.Width != g_rtv_width || desc.Height != g_rtv_height ||
                  desc.Format != g_rtv_format)) {
        logf("render: swapchain target changed (%p -> %p, %ux%u)",
             reinterpret_cast<void*>(g_rtv_chain),
             reinterpret_cast<void*>(swapchain), desc.Width, desc.Height);
        g_rtv->Release();
        g_rtv = nullptr;
        g_rtv_chain = 0;
    }
    if (!g_rtv) {
        const HRESULT result =
            g_device->CreateRenderTargetView(backbuffer, nullptr, &g_rtv);
        if (SUCCEEDED(result)) {
            g_rtv_chain = reinterpret_cast<std::uintptr_t>(swapchain);
            g_rtv_width = desc.Width;
            g_rtv_height = desc.Height;
            g_rtv_format = desc.Format;
        } else {
            logf("render: CreateRenderTargetView failed hr=%08lX",
                 static_cast<unsigned long>(result));
        }
    }
    backbuffer->Release();
}

float dpi_scale(HWND window, float fallback) {
    if (!window) return fallback;
    const UINT dpi = GetDpiForWindow(window);
    return dpi > 0 ? static_cast<float>(dpi) / 96.0f : fallback;
}

void render_frame(IDXGISwapChain* swapchain) {
    if (g_overlay_disabled.load(std::memory_order_relaxed)) return;
    if (!ensure_imgui(swapchain)) return;

    DXGI_SWAP_CHAIN_DESC desc{};
    if (FAILED(swapchain->GetDesc(&desc)) || desc.BufferDesc.Width == 0 ||
        desc.BufferDesc.Height == 0) {
        return;
    }

    ensure_target(swapchain);
    if (!g_rtv) return;

    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(static_cast<float>(desc.BufferDesc.Width),
                            static_cast<float>(desc.BufferDesc.Height));
    io.DeltaTime = 1.0f / 60.0f;

    const float scale = dpi_scale(desc.OutputWindow, 1.0f);
    ImGui_ImplDX11_NewFrame();
    ImGui::NewFrame();
    drawing::render_all(ImGui::GetBackgroundDrawList(), scale);
    ImGui::Render();

    g_context->OMSetRenderTargets(1, &g_rtv, nullptr);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    // Do not keep the backbuffer referenced by the context: ResizeBuffers
    // fails while any reference is alive.
    g_context->OMSetRenderTargets(0, nullptr, nullptr);
}

HRESULT WINAPI detail_present_hook(IDXGISwapChain* swapchain, UINT sync, UINT flags) {
    const auto original =
        reinterpret_cast<PresentFn>(g_present_original.load(std::memory_order_acquire));
    if (!original) return E_FAIL;

    if (swapchain &&
        reinterpret_cast<std::uintptr_t>(swapchain) ==
            g_active.load(std::memory_order_acquire) &&
        !g_overlay_disabled.load(std::memory_order_relaxed)) {
        __try {
            render_frame(swapchain);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            g_overlay_disabled.store(true);
            log("render: overlay disabled after a fault");
        }
    }
    return original(swapchain, sync, flags);
}

HRESULT WINAPI detail_resize_hook(IDXGISwapChain* swapchain, UINT buffer_count,
                                  UINT width, UINT height, DXGI_FORMAT format,
                                  UINT flags) {
    const auto original =
        reinterpret_cast<ResizeFn>(g_resize_original.load(std::memory_order_acquire));
    if (!original) return E_FAIL;

    if (g_rtv &&
        reinterpret_cast<std::uintptr_t>(swapchain) == g_rtv_chain) {
        ID3D11RenderTargetView* target = g_rtv;
        g_rtv = nullptr;
        g_rtv_chain = 0;
        g_rtv_width = 0;
        g_rtv_height = 0;
        g_rtv_format = DXGI_FORMAT_UNKNOWN;
        __try {
            target->Release();
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
    }
    return original(swapchain, buffer_count, width, height, format, flags);
}

DWORD WINAPI watch_thread(void*) {
    log("render: watcher started");
    while (!g_stopping.load(std::memory_order_relaxed)) {
        const std::uintptr_t swapchain = resolve_swapchain();
        if (swapchain) {
            install_hook(swapchain);
            Sleep(1000);
        } else {
            Sleep(250);
        }
    }
    log("render: watcher stopped");
    return 0;
}

} // namespace
} // namespace zend::rbx::render

namespace zend::rbx::render {

void start() {
    if (g_thread) return;
    g_stopping.store(false);
    g_overlay_disabled.store(false);
    g_thread = CreateThread(nullptr, 0, watch_thread, nullptr, 0, nullptr);
    if (!g_thread) {
        log("render: could not start watcher");
    }
}

void stop() {
    g_stopping.store(true);
    g_active.store(0);

    // Lock-free on purpose: DllMain detach can run after other threads are
    // gone. Only the pointer restore matters; the clone stays allocated.
    if (g_swapchain && g_clone) {
        std::uintptr_t current = 0;
        if (read_ptr(g_swapchain, current) &&
            current == reinterpret_cast<std::uintptr_t>(g_clone)) {
            write_ptr(g_swapchain, g_original_vtable);
        }
    }
    g_swapchain = 0;
    g_clone = nullptr;
}

} // namespace zend::rbx::render

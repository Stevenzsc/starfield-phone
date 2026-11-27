// Screen texture writer (P0): find the phone screen texture by its unique size when the game creates it, and copy
// an animated test pattern into it while the phone is open.
//
// Same idea as ScreenSFSE (its log shows it): hook the ID3D12Device resource-creation methods to catch the texture,
// and hook ID3D12CommandQueue::ExecuteCommandLists to run our own copy before the game's command lists.
//
// This file does not use CommonLibSF so that <windows.h> and <d3d12.h> can be included normally.

#include "screen.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d12.h>

#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>

#pragma comment(lib, "d3d12.lib")

namespace
{
	Screen::LogFn g_log = nullptr;

	void Log(const char* a_fmt, ...)
	{
		if (!g_log) {
			return;
		}
		char    buf[512];
		va_list ap;
		va_start(ap, a_fmt);
		std::vsnprintf(buf, sizeof(buf), a_fmt, ap);
		va_end(ap);
		g_log(buf);
	}

	// vtable slots (d3d12.h declaration order)
	constexpr int kSlotCreateCommittedResource = 27;
	constexpr int kSlotCreatePlacedResource = 29;
	constexpr int kSlotCreatePlacedResource1 = 70;
	constexpr int kSlotCreatePlacedResource2 = 77;
	constexpr int kSlotExecuteCommandLists = 10;

	using CreateCommitted_t = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*, const D3D12_HEAP_PROPERTIES*, D3D12_HEAP_FLAGS,
		const D3D12_RESOURCE_DESC*, D3D12_RESOURCE_STATES, const D3D12_CLEAR_VALUE*, REFIID, void**);
	using CreatePlaced_t = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*, ID3D12Heap*, UINT64, const D3D12_RESOURCE_DESC*,
		D3D12_RESOURCE_STATES, const D3D12_CLEAR_VALUE*, REFIID, void**);
	using CreatePlaced1_t = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*, ID3D12Heap*, UINT64, const D3D12_RESOURCE_DESC1*,
		D3D12_RESOURCE_STATES, const D3D12_CLEAR_VALUE*, REFIID, void**);
	using CreatePlaced2_t = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*, ID3D12Heap*, UINT64, const D3D12_RESOURCE_DESC1*,
		D3D12_BARRIER_LAYOUT, const D3D12_CLEAR_VALUE*, UINT32, const DXGI_FORMAT*, REFIID, void**);
	using Execute_t = void(STDMETHODCALLTYPE*)(ID3D12CommandQueue*, UINT, ID3D12CommandList* const*);

	CreateCommitted_t g_createCommitted = nullptr;
	CreatePlaced_t    g_createPlaced = nullptr;
	CreatePlaced1_t   g_createPlaced1 = nullptr;
	CreatePlaced2_t   g_createPlaced2 = nullptr;
	Execute_t         g_execute = nullptr;

	std::uint32_t g_width = 0;
	std::uint32_t g_height = 0;

	std::mutex           g_lock;
	ID3D12Resource*      g_target = nullptr;  // AddRef'd
	std::atomic_bool     g_active{ false };
	std::atomic_uint32_t g_marker{ 0 };
	std::atomic_uint32_t g_matches{ 0 };
	std::atomic_uint32_t g_writes{ 0 };

	bool IsScreenDesc(UINT64 a_width, UINT a_height, D3D12_RESOURCE_DIMENSION a_dim, DXGI_FORMAT a_fmt)
	{
		return a_dim == D3D12_RESOURCE_DIMENSION_TEXTURE2D && a_width == g_width && a_height == g_height &&
		       (a_fmt == DXGI_FORMAT_R8G8B8A8_TYPELESS || a_fmt == DXGI_FORMAT_R8G8B8A8_UNORM ||
				   a_fmt == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB);
	}

	void Capture(const char* a_via, REFIID a_riid, void** a_out, DXGI_FORMAT a_fmt, UINT16 a_mips)
	{
		if (!a_out || !*a_out) {
			return;
		}
		ID3D12Resource* res = nullptr;
		if (FAILED(static_cast<IUnknown*>(*a_out)->QueryInterface(IID_PPV_ARGS(&res)))) {
			return;
		}
		(void)a_riid;
		std::lock_guard lk(g_lock);
		if (g_target) {
			g_target->Release();
		}
		g_target = res;  // keep the QueryInterface reference
		const auto n = ++g_matches;
		Log("screen texture captured via %s: %ux%u fmt=%d mips=%u (match #%u)", a_via, g_width, g_height, a_fmt, a_mips, n);
	}

	HRESULT STDMETHODCALLTYPE CreateCommitted_Hook(ID3D12Device* a_this, const D3D12_HEAP_PROPERTIES* a_heap,
		D3D12_HEAP_FLAGS a_flags, const D3D12_RESOURCE_DESC* a_desc, D3D12_RESOURCE_STATES a_state,
		const D3D12_CLEAR_VALUE* a_clear, REFIID a_riid, void** a_out)
	{
		if (a_desc && IsScreenDesc(a_desc->Width, a_desc->Height, a_desc->Dimension, a_desc->Format)) {
			D3D12_RESOURCE_DESC desc = *a_desc;
			desc.Flags |= D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS;
			const auto hr = g_createCommitted(a_this, a_heap, a_flags, &desc, a_state, a_clear, a_riid, a_out);
			if (SUCCEEDED(hr)) {
				Capture("CreateCommittedResource", a_riid, a_out, desc.Format, desc.MipLevels);
			}
			return hr;
		}
		return g_createCommitted(a_this, a_heap, a_flags, a_desc, a_state, a_clear, a_riid, a_out);
	}

	HRESULT STDMETHODCALLTYPE CreatePlaced_Hook(ID3D12Device* a_this, ID3D12Heap* a_heap, UINT64 a_offset,
		const D3D12_RESOURCE_DESC* a_desc, D3D12_RESOURCE_STATES a_state, const D3D12_CLEAR_VALUE* a_clear,
		REFIID a_riid, void** a_out)
	{
		if (a_desc && IsScreenDesc(a_desc->Width, a_desc->Height, a_desc->Dimension, a_desc->Format)) {
			D3D12_RESOURCE_DESC desc = *a_desc;
			desc.Flags |= D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS;
			const auto hr = g_createPlaced(a_this, a_heap, a_offset, &desc, a_state, a_clear, a_riid, a_out);
			if (SUCCEEDED(hr)) {
				Capture("CreatePlacedResource", a_riid, a_out, desc.Format, desc.MipLevels);
			} else {
				Log("CreatePlacedResource with simultaneous access failed (0x%08X); retrying without", (unsigned)hr);
				const auto hr2 = g_createPlaced(a_this, a_heap, a_offset, a_desc, a_state, a_clear, a_riid, a_out);
				if (SUCCEEDED(hr2)) {
					Capture("CreatePlacedResource (no simultaneous)", a_riid, a_out, a_desc->Format, a_desc->MipLevels);
				}
				return hr2;
			}
			return hr;
		}
		return g_createPlaced(a_this, a_heap, a_offset, a_desc, a_state, a_clear, a_riid, a_out);
	}

	HRESULT STDMETHODCALLTYPE CreatePlaced1_Hook(ID3D12Device* a_this, ID3D12Heap* a_heap, UINT64 a_offset,
		const D3D12_RESOURCE_DESC1* a_desc, D3D12_RESOURCE_STATES a_state, const D3D12_CLEAR_VALUE* a_clear,
		REFIID a_riid, void** a_out)
	{
		if (a_desc && IsScreenDesc(a_desc->Width, a_desc->Height, a_desc->Dimension, a_desc->Format)) {
			D3D12_RESOURCE_DESC1 desc = *a_desc;
			desc.Flags |= D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS;
			const auto hr = g_createPlaced1(a_this, a_heap, a_offset, &desc, a_state, a_clear, a_riid, a_out);
			if (SUCCEEDED(hr)) {
				Capture("CreatePlacedResource1", a_riid, a_out, desc.Format, desc.MipLevels);
			}
			return hr;
		}
		return g_createPlaced1(a_this, a_heap, a_offset, a_desc, a_state, a_clear, a_riid, a_out);
	}

	HRESULT STDMETHODCALLTYPE CreatePlaced2_Hook(ID3D12Device* a_this, ID3D12Heap* a_heap, UINT64 a_offset,
		const D3D12_RESOURCE_DESC1* a_desc, D3D12_BARRIER_LAYOUT a_layout, const D3D12_CLEAR_VALUE* a_clear,
		UINT32 a_numCastable, const DXGI_FORMAT* a_castable, REFIID a_riid, void** a_out)
	{
		if (a_desc && IsScreenDesc(a_desc->Width, a_desc->Height, a_desc->Dimension, a_desc->Format)) {
			D3D12_RESOURCE_DESC1 desc = *a_desc;
			desc.Flags |= D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS;
			const auto hr = g_createPlaced2(a_this, a_heap, a_offset, &desc, a_layout, a_clear, a_numCastable, a_castable, a_riid, a_out);
			if (SUCCEEDED(hr)) {
				Capture("CreatePlacedResource2", a_riid, a_out, desc.Format, desc.MipLevels);
			}
			return hr;
		}
		return g_createPlaced2(a_this, a_heap, a_offset, a_desc, a_layout, a_clear, a_numCastable, a_castable, a_riid, a_out);
	}

	// ---- writer: our own command list, executed on the game's direct queue before its lists ----

	constexpr int kSlots = 3;

	struct Writer
	{
		ID3D12Device*                      device = nullptr;
		ID3D12CommandAllocator*            alloc[kSlots]{};
		ID3D12GraphicsCommandList*         list = nullptr;
		ID3D12Resource*                    upload[kSlots]{};
		std::uint8_t*                      mapped[kSlots]{};
		UINT64                             fenceValue[kSlots]{};
		ID3D12Fence*                       fence = nullptr;
		UINT64                             nextFence = 1;
		D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
		int                                slot = 0;
		bool                               ok = false;
		bool                               failed = false;
	} g_w;

	bool InitWriter(ID3D12Resource* a_target)
	{
		if (g_w.ok) {
			return true;
		}
		if (g_w.failed) {
			return false;
		}
		g_w.failed = true;  // cleared on success
		if (FAILED(a_target->GetDevice(IID_PPV_ARGS(&g_w.device)))) {
			Log("writer: GetDevice failed");
			return false;
		}
		const auto desc = a_target->GetDesc();
		UINT64     total = 0;
		g_w.device->GetCopyableFootprints(&desc, 0, 1, 0, &g_w.footprint, nullptr, nullptr, &total);

		D3D12_HEAP_PROPERTIES hp{};
		hp.Type = D3D12_HEAP_TYPE_UPLOAD;
		D3D12_RESOURCE_DESC bd{};
		bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
		bd.Width = total;
		bd.Height = 1;
		bd.DepthOrArraySize = 1;
		bd.MipLevels = 1;
		bd.SampleDesc.Count = 1;
		bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

		for (int i = 0; i < kSlots; ++i) {
			if (FAILED(g_w.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&g_w.alloc[i]))) ||
				FAILED(g_w.device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_GENERIC_READ,
					nullptr, IID_PPV_ARGS(&g_w.upload[i]))) ||
				FAILED(g_w.upload[i]->Map(0, nullptr, reinterpret_cast<void**>(&g_w.mapped[i])))) {
				Log("writer: slot %d setup failed", i);
				return false;
			}
		}
		if (FAILED(g_w.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g_w.alloc[0], nullptr, IID_PPV_ARGS(&g_w.list))) ||
			FAILED(g_w.list->Close()) ||
			FAILED(g_w.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_w.fence)))) {
			Log("writer: command list / fence setup failed");
			return false;
		}
		g_w.ok = true;
		g_w.failed = false;
		Log("writer ready: footprint rowPitch=%u size=%llu, format %d", g_w.footprint.Footprint.RowPitch,
			(unsigned long long)total, desc.Format);
		return true;
	}

	// Animated test pattern: colour bands that scroll down, a white frame, and a moving square.
	void FillPattern(std::uint8_t* a_dst, std::uint32_t a_frame)
	{
		const auto pitch = g_w.footprint.Footprint.RowPitch;
		for (std::uint32_t y = 0; y < g_height; ++y) {
			auto* row = a_dst + std::size_t(y) * pitch;
			const std::uint32_t band = ((y + a_frame * 6) / 74) % 6;
			static constexpr std::uint8_t colors[6][3] = {
				{ 255, 40, 40 }, { 255, 200, 0 }, { 40, 220, 60 }, { 0, 200, 255 }, { 60, 80, 255 }, { 220, 40, 255 }
			};
			for (std::uint32_t x = 0; x < g_width; ++x) {
				auto* p = row + std::size_t(x) * 4;
				const bool border = x < 12 || y < 12 || x >= g_width - 12 || y >= g_height - 12;
				const std::uint32_t sx = (a_frame * 7) % (g_width - 80);
				const bool square = x >= sx && x < sx + 80 && y >= g_height / 2 - 40 && y < g_height / 2 + 40;
				// variant marker: N white squares (24 px, 6 px gap, wrapping every 14) near the top, on black
				const std::uint32_t mx = x >= 24 ? (x - 24) : 0xFFFF, my = y >= 30 ? (y - 30) : 0xFFFF;
				const std::uint32_t idx = (my / 34) * 14 + mx / 30;
				const bool inBand = x >= 24 && y >= 30 && my / 34 < 2 && mx / 30 < 14;
				const bool mark = inBand && idx < g_marker.load() && (mx % 30) < 24 && (my % 34) < 24;
				if (inBand) {
					p[0] = p[1] = p[2] = mark ? 255 : 0;
				} else if (border || square) {
					p[0] = p[1] = p[2] = 255;
				} else {
					p[0] = colors[band][0];
					p[1] = colors[band][1];
					p[2] = colors[band][2];
				}
				p[3] = 255;
			}
		}
	}

	void STDMETHODCALLTYPE Execute_Hook(ID3D12CommandQueue* a_this, UINT a_count, ID3D12CommandList* const* a_lists)
	{
		static auto last = std::chrono::steady_clock::now();
		static std::uint32_t frame = 0;

		if (g_active.load() && a_this->GetDesc().Type == D3D12_COMMAND_LIST_TYPE_DIRECT) {
			std::unique_lock lk(g_lock, std::try_to_lock);
			const auto now = std::chrono::steady_clock::now();
			if (lk.owns_lock() && g_target && now - last >= std::chrono::milliseconds(66) && InitWriter(g_target)) {
				const int s = g_w.slot;
				if (g_w.fence->GetCompletedValue() >= g_w.fenceValue[s]) {
					last = now;
					FillPattern(g_w.mapped[s], frame++);

					g_w.alloc[s]->Reset();
					g_w.list->Reset(g_w.alloc[s], nullptr);
					D3D12_TEXTURE_COPY_LOCATION dst{};
					dst.pResource = g_target;
					dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
					dst.SubresourceIndex = 0;
					D3D12_TEXTURE_COPY_LOCATION src{};
					src.pResource = g_w.upload[s];
					src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
					src.PlacedFootprint = g_w.footprint;
					g_w.list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
					g_w.list->Close();

					ID3D12CommandList* ours[] = { g_w.list };
					g_execute(a_this, 1, ours);
					g_w.fenceValue[s] = g_w.nextFence++;
					a_this->Signal(g_w.fence, g_w.fenceValue[s]);
					g_w.slot = (s + 1) % kSlots;

					const auto n = ++g_writes;
					if (n == 1 || n % 300 == 0) {
						Log("screen writes: %u", n);
					}
				}
			}
		}
		g_execute(a_this, a_count, a_lists);
	}

	template <class T>
	void Patch(void** a_vtbl, int a_slot, T a_hook, T& a_orig, const char* a_name)
	{
		DWORD old = 0;
		if (!VirtualProtect(&a_vtbl[a_slot], sizeof(void*), PAGE_EXECUTE_READWRITE, &old)) {
			Log("hook %s: VirtualProtect failed", a_name);
			return;
		}
		a_orig = reinterpret_cast<T>(a_vtbl[a_slot]);
		a_vtbl[a_slot] = reinterpret_cast<void*>(a_hook);
		VirtualProtect(&a_vtbl[a_slot], sizeof(void*), old, &old);
		Log("hooked %s (slot %d)", a_name, a_slot);
	}
}

namespace Screen
{
	bool Install(std::uint32_t a_width, std::uint32_t a_height, LogFn a_log)
	{
		g_log = a_log;
		g_width = a_width;
		g_height = a_height;

		ID3D12Device* dev = nullptr;
		if (FAILED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&dev)))) {
			Log("D3D12CreateDevice (dummy) failed; screen writer disabled");
			return false;
		}
		D3D12_COMMAND_QUEUE_DESC qd{};
		qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
		ID3D12CommandQueue* queue = nullptr;
		if (FAILED(dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue)))) {
			Log("CreateCommandQueue (dummy) failed; screen writer disabled");
			dev->Release();
			return false;
		}

		auto** dv = *reinterpret_cast<void***>(dev);
		auto** qv = *reinterpret_cast<void***>(queue);
		Patch(dv, kSlotCreateCommittedResource, &CreateCommitted_Hook, g_createCommitted, "CreateCommittedResource");
		Patch(dv, kSlotCreatePlacedResource, &CreatePlaced_Hook, g_createPlaced, "CreatePlacedResource");
		Patch(dv, kSlotCreatePlacedResource1, &CreatePlaced1_Hook, g_createPlaced1, "CreatePlacedResource1");
		Patch(dv, kSlotCreatePlacedResource2, &CreatePlaced2_Hook, g_createPlaced2, "CreatePlacedResource2");
		Patch(qv, kSlotExecuteCommandLists, &Execute_Hook, g_execute, "ExecuteCommandLists");

		// the vtables live in the D3D12 runtime and stay valid; the dummy objects are no longer needed
		queue->Release();
		dev->Release();
		Log("screen writer armed for %ux%u RGBA8 textures", a_width, a_height);
		return true;
	}

	void SetMarker(std::uint32_t a_count)
	{
		g_marker = a_count;
	}

	void SetActive(bool a_active)
	{
		g_active = a_active;
		Log("screen writer %s (texture %s, matches %u, writes %u)", a_active ? "ON" : "OFF",
			g_target ? "captured" : "NOT captured", g_matches.load(), g_writes.load());
	}
}

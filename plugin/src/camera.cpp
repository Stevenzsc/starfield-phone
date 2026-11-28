// Camera app: live viewfinder on the phone screen and photos.
//
// At Present time (hooked through the vtable of a dummy DXGI swap chain, shared by every swap chain of the same
// class) the centre of the back buffer is copied into a readback buffer on the swap chain's own command queue.
// A few frames later the CPU converts it to RGBA8, scales it down to the viewfinder size and hands it to the UI.
// A requested photo saves the full-size crop as a PNG from a worker thread.
//
// This file does not use CommonLibSF so that <windows.h>, <d3d12.h> and <dxgi1_4.h> can be included normally.

#include "camera.h"
#include "ui.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <knownfolders.h>
#include <shlobj.h>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#if __has_include(<stb_image_write.h>)
#	include <stb_image_write.h>
#else
#	include <stb/stb_image_write.h>
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")

namespace
{
	Camera::LogFn g_log = nullptr;

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

	// IDXGISwapChain vtable slots (dxgi.h / dxgi1_2.h declaration order)
	constexpr int kSlotPresent = 8;
	constexpr int kSlotPresent1 = 22;

	using Present_t = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
	using Present1_t = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain1*, UINT, UINT, const DXGI_PRESENT_PARAMETERS*);
	Present_t  g_present = nullptr;
	Present1_t g_present1 = nullptr;

	std::atomic_bool g_open{ false };
	std::atomic_bool g_photoRequested{ false };
	std::atomic_bool g_photoSaving{ false };

	// ---- pixel formats ----
	enum class Kind
	{
		kUnsupported,
		kRGBA8,
		kBGRA8,
		kRGB10A2,
		kRGBA16F,
	};

	Kind KindOf(DXGI_FORMAT a_fmt)
	{
		switch (a_fmt) {
		case DXGI_FORMAT_R8G8B8A8_TYPELESS:
		case DXGI_FORMAT_R8G8B8A8_UNORM:
		case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
			return Kind::kRGBA8;
		case DXGI_FORMAT_B8G8R8A8_TYPELESS:
		case DXGI_FORMAT_B8G8R8A8_UNORM:
		case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
		case DXGI_FORMAT_B8G8R8X8_TYPELESS:
		case DXGI_FORMAT_B8G8R8X8_UNORM:
		case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB:
			return Kind::kBGRA8;
		case DXGI_FORMAT_R10G10B10A2_TYPELESS:
		case DXGI_FORMAT_R10G10B10A2_UNORM:
			return Kind::kRGB10A2;
		case DXGI_FORMAT_R16G16B16A16_FLOAT:
			return Kind::kRGBA16F;
		default:
			return Kind::kUnsupported;
		}
	}

	float HalfToFloat(std::uint16_t a_h)
	{
		const std::uint32_t s = (a_h >> 15) & 1, e = (a_h >> 10) & 0x1F, m = a_h & 0x3FF;
		float               v;
		if (e == 0) {
			v = std::ldexp(float(m), -24);
		} else if (e == 31) {
			v = m ? 0.0f : 65504.0f;
		} else {
			v = std::ldexp(float(m | 0x400), int(e) - 25);
		}
		return s ? -v : v;
	}

	// linear [0,1] -> sRGB byte (scRGB back buffers in HDR mode)
	std::uint8_t LinearToSrgb(float a_v)
	{
		static const auto lut = [] {
			std::vector<std::uint8_t> t(4096);
			for (int i = 0; i < 4096; ++i) {
				const float c = float(i) / 4095.0f;
				const float s = c <= 0.0031308f ? c * 12.92f : 1.055f * std::pow(c, 1.0f / 2.4f) - 0.055f;
				t[i] = std::uint8_t(std::clamp(s, 0.0f, 1.0f) * 255.0f + 0.5f);
			}
			return t;
		}();
		return lut[std::size_t(std::clamp(a_v, 0.0f, 1.0f) * 4095.0f + 0.5f)];
	}

	inline void ReadPixel(const std::uint8_t* a_p, Kind a_kind, std::uint32_t a_rgb[3])
	{
		switch (a_kind) {
		case Kind::kRGBA8:
			a_rgb[0] = a_p[0], a_rgb[1] = a_p[1], a_rgb[2] = a_p[2];
			break;
		case Kind::kBGRA8:
			a_rgb[0] = a_p[2], a_rgb[1] = a_p[1], a_rgb[2] = a_p[0];
			break;
		case Kind::kRGB10A2: {
			std::uint32_t v;
			std::memcpy(&v, a_p, 4);
			a_rgb[0] = (v & 0x3FF) >> 2, a_rgb[1] = ((v >> 10) & 0x3FF) >> 2, a_rgb[2] = ((v >> 20) & 0x3FF) >> 2;
			break;
		}
		case Kind::kRGBA16F: {
			std::uint16_t h[3];
			std::memcpy(h, a_p, 6);
			for (int i = 0; i < 3; ++i) {
				a_rgb[i] = LinearToSrgb(HalfToFloat(h[i]));
			}
			break;
		}
		default:
			a_rgb[0] = a_rgb[1] = a_rgb[2] = 0;
			break;
		}
	}

	// ---- readback ring ----
	constexpr int kSlots = 3;

	struct Slot
	{
		ID3D12CommandAllocator*            alloc = nullptr;
		ID3D12Resource*                    buf = nullptr;
		UINT64                             size = 0;
		UINT64                             fence = 0;
		D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
		std::uint32_t                      w = 0, h = 0;
		Kind                               kind = Kind::kUnsupported;
		bool                               pending = false;
		bool                               photo = false;
	};

	struct Capture
	{
		std::mutex                 lock;
		ID3D12Device*              device = nullptr;
		ID3D12GraphicsCommandList* list = nullptr;
		ID3D12Fence*               fence = nullptr;
		UINT64                     nextFence = 1;
		Slot                       slots[kSlots];
		int                        next = 0;
		bool                       ok = false;
		bool                       failed = false;
		std::chrono::steady_clock::time_point last{};
		std::uint32_t              frames = 0;
	} g_c;

	bool InitCapture(ID3D12Device* a_device)
	{
		if (g_c.ok) {
			return g_c.device == a_device;
		}
		if (g_c.failed) {
			return false;
		}
		g_c.failed = true;  // cleared on success
		g_c.device = a_device;
		g_c.device->AddRef();
		for (auto& s : g_c.slots) {
			if (FAILED(g_c.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&s.alloc)))) {
				Log("camera: CreateCommandAllocator failed");
				return false;
			}
		}
		if (FAILED(g_c.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g_c.slots[0].alloc, nullptr, IID_PPV_ARGS(&g_c.list))) ||
			FAILED(g_c.list->Close()) ||
			FAILED(g_c.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_c.fence)))) {
			Log("camera: command list / fence setup failed");
			return false;
		}
		g_c.ok = true;
		g_c.failed = false;
		Log("camera: capture ready");
		return true;
	}

	bool EnsureBuffer(Slot& a_s, UINT64 a_size)
	{
		if (a_s.buf && a_s.size >= a_size) {
			return true;
		}
		if (a_s.buf) {
			a_s.buf->Release();
			a_s.buf = nullptr;
			a_s.size = 0;
		}
		D3D12_HEAP_PROPERTIES hp{};
		hp.Type = D3D12_HEAP_TYPE_READBACK;
		D3D12_RESOURCE_DESC bd{};
		bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
		bd.Width = a_size;
		bd.Height = 1;
		bd.DepthOrArraySize = 1;
		bd.MipLevels = 1;
		bd.SampleDesc.Count = 1;
		bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
		if (FAILED(g_c.device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
				IID_PPV_ARGS(&a_s.buf)))) {
			Log("camera: readback buffer (%llu bytes) failed", (unsigned long long)a_size);
			return false;
		}
		a_s.size = a_size;
		return true;
	}

	// ---- photos ----
	std::wstring PhotoDir()
	{
		PWSTR docs = nullptr;
		if (FAILED(SHGetKnownFolderPath(FOLDERID_Documents, 0, nullptr, &docs))) {
			return {};
		}
		std::wstring dir = docs;
		CoTaskMemFree(docs);
		for (const wchar_t* part : { L"\\My Games", L"\\Starfield", L"\\PhoneCamera" }) {
			dir += part;
			CreateDirectoryW(dir.c_str(), nullptr);
		}
		return dir;
	}

	void WriteToFile(void* a_ctx, void* a_data, int a_size)
	{
		std::fwrite(a_data, 1, std::size_t(a_size), static_cast<FILE*>(a_ctx));
	}

	void SavePhoto(std::vector<std::uint8_t> a_rgba, std::uint32_t a_w, std::uint32_t a_h)
	{
		std::thread([px = std::move(a_rgba), a_w, a_h] {
			const auto dir = PhotoDir();
			bool       ok = false;
			char       name[64] = "";
			if (!dir.empty()) {
				const std::time_t t = std::time(nullptr);
				std::tm           tm{};
				localtime_s(&tm, &t);
				std::strftime(name, sizeof(name), "Photo_%Y%m%d_%H%M%S.png", &tm);
				std::wstring path = dir + L"\\";
				for (const char* p = name; *p; ++p) {
					path += wchar_t(*p);
				}
				if (FILE* f = _wfopen(path.c_str(), L"wb")) {
					ok = stbi_write_png_to_func(&WriteToFile, f, int(a_w), int(a_h), 4, px.data(), int(a_w) * 4) != 0;
					std::fclose(f);
				}
			}
			Log("camera: photo %ux%u %s %s", a_w, a_h, name, ok ? "saved" : "FAILED");
			Ui::SetCameraStatus(ok ? std::string("已保存 ") + name : std::string("保存失败"));
			g_photoSaving = false;
		}).detach();
	}

	// ---- per-present work ----

	// convert a finished readback: preview frame for the UI, and the photo if this capture was one
	void Consume(Slot& a_s)
	{
		int vw = 0, vh = 0;
		const bool want = Ui::CameraTarget(vw, vh);
		if (!want && !a_s.photo) {
			return;
		}
		D3D12_RANGE    rr{ 0, SIZE_T(a_s.size) };
		std::uint8_t*  base = nullptr;
		if (FAILED(a_s.buf->Map(0, &rr, reinterpret_cast<void**>(&base)))) {
			return;
		}
		const auto*         src = base + a_s.fp.Offset;
		const std::uint32_t pitch = a_s.fp.Footprint.RowPitch;
		const std::uint32_t bpp = a_s.kind == Kind::kRGBA16F ? 8 : 4;

		if (want && vw > 0 && vh > 0) {
			std::vector<std::uint8_t> out(std::size_t(vw) * vh * 4);
			const float sx = float(a_s.w) / float(vw), sy = float(a_s.h) / float(vh);
			for (int y = 0; y < vh; ++y) {
				for (int x = 0; x < vw; ++x) {
					std::uint32_t sum[3]{}, rgb[3];
					for (int k = 0; k < 4; ++k) {  // 2x2 samples inside the source footprint of this pixel
						const float fx = (float(x) + 0.25f + 0.5f * float(k & 1)) * sx;
						const float fy = (float(y) + 0.25f + 0.5f * float(k >> 1)) * sy;
						const auto  px = std::min(a_s.w - 1, std::uint32_t(fx));
						const auto  py = std::min(a_s.h - 1, std::uint32_t(fy));
						ReadPixel(src + std::size_t(py) * pitch + std::size_t(px) * bpp, a_s.kind, rgb);
						sum[0] += rgb[0], sum[1] += rgb[1], sum[2] += rgb[2];
					}
					auto* d = &out[(std::size_t(y) * vw + x) * 4];
					d[0] = std::uint8_t(sum[0] / 4), d[1] = std::uint8_t(sum[1] / 4), d[2] = std::uint8_t(sum[2] / 4), d[3] = 255;
				}
			}
			Ui::SetCameraFrame(std::move(out), vw, vh);
		}

		if (a_s.photo) {
			std::vector<std::uint8_t> full(std::size_t(a_s.w) * a_s.h * 4);
			for (std::uint32_t y = 0; y < a_s.h; ++y) {
				const auto* row = src + std::size_t(y) * pitch;
				auto*       d = &full[std::size_t(y) * a_s.w * 4];
				for (std::uint32_t x = 0; x < a_s.w; ++x, d += 4) {
					std::uint32_t rgb[3];
					ReadPixel(row + std::size_t(x) * bpp, a_s.kind, rgb);
					d[0] = std::uint8_t(rgb[0]), d[1] = std::uint8_t(rgb[1]), d[2] = std::uint8_t(rgb[2]), d[3] = 255;
				}
			}
			SavePhoto(std::move(full), a_s.w, a_s.h);
			a_s.photo = false;
		}
		D3D12_RANGE none{ 0, 0 };
		a_s.buf->Unmap(0, &none);
	}

	void OnPresent(IDXGISwapChain* a_sc)
	{
		if (!g_open.load()) {
			return;
		}
		int vw = 0, vh = 0;
		const bool wantPreview = Ui::CameraTarget(vw, vh);
		bool       photo = g_photoRequested.load();
		if (!wantPreview && !photo) {
			return;
		}
		std::unique_lock lk(g_c.lock, std::try_to_lock);
		if (!lk.owns_lock()) {
			return;
		}

		// finished captures: show the newest, save any photo
		if (g_c.ok) {
			const UINT64 done = g_c.fence->GetCompletedValue();
			Slot*        newest = nullptr;
			for (auto& s : g_c.slots) {
				if (s.pending && s.fence <= done && (!newest || s.fence > newest->fence)) {
					newest = &s;
				}
			}
			for (auto& s : g_c.slots) {
				if (s.pending && s.fence <= done && s.photo && &s != newest) {
					Consume(s);  // an older photo capture still has to be saved
				}
			}
			if (newest) {
				Consume(*newest);
			}
			for (auto& s : g_c.slots) {
				if (s.pending && s.fence <= done) {
					s.pending = false;
				}
			}
		}

		const auto now = std::chrono::steady_clock::now();
		if (!photo && now - g_c.last < std::chrono::milliseconds(50)) {
			return;
		}

		IDXGISwapChain3* sc3 = nullptr;
		if (FAILED(a_sc->QueryInterface(IID_PPV_ARGS(&sc3)))) {
			return;
		}
		ID3D12Resource*     bb = nullptr;
		ID3D12CommandQueue* queue = nullptr;
		ID3D12Device*       dev = nullptr;
		const UINT          index = sc3->GetCurrentBackBufferIndex();
		const bool          got = SUCCEEDED(sc3->GetBuffer(index, IID_PPV_ARGS(&bb))) &&
		                 SUCCEEDED(a_sc->GetDevice(IID_PPV_ARGS(&queue))) && SUCCEEDED(bb->GetDevice(IID_PPV_ARGS(&dev)));
		sc3->Release();
		auto release = [&] {
			if (bb) bb->Release();
			if (queue) queue->Release();
			if (dev) dev->Release();
		};
		if (!got || !InitCapture(dev)) {
			static bool once = false;
			if (!once) {
				once = true;
				Log("camera: no D3D12 back buffer / queue on this swap chain (got=%d)", got);
			}
			release();
			return;
		}

		const auto bd = bb->GetDesc();
		const Kind kind = KindOf(bd.Format);
		if (kind == Kind::kUnsupported) {
			static bool once = false;
			if (!once) {
				once = true;
				Log("camera: back buffer format %d not supported", bd.Format);
			}
			release();
			return;
		}

		// free slot (its previous copy finished and was consumed)
		Slot* slot = nullptr;
		for (int i = 0; i < kSlots && !slot; ++i) {
			auto& s = g_c.slots[(g_c.next + i) % kSlots];
			if (!s.pending) {
				slot = &s;
				g_c.next = (g_c.next + i + 1) % kSlots;
			}
		}
		if (!slot) {
			release();
			return;
		}

		// centre crop with the viewfinder's aspect (height / width), as tall as the frame allows
		const float aspect = (vw > 0 && vh > 0) ? float(vh) / float(vw) : 1.5f;
		const auto  W = std::uint32_t(bd.Width), H = bd.Height;
		std::uint32_t ch = H, cw = std::uint32_t(std::lround(float(H) / aspect));
		if (cw > W) {
			cw = W;
			ch = std::min(H, std::uint32_t(std::lround(float(W) * aspect)));
		}
		const std::uint32_t x0 = (W - cw) / 2, y0 = (H - ch) / 2;

		D3D12_RESOURCE_DESC td = bd;
		td.Width = cw;
		td.Height = ch;
		td.MipLevels = 1;
		td.DepthOrArraySize = 1;
		td.Flags = D3D12_RESOURCE_FLAG_NONE;
		UINT64 total = 0;
		g_c.device->GetCopyableFootprints(&td, 0, 1, 0, &slot->fp, nullptr, nullptr, &total);
		if (!EnsureBuffer(*slot, total)) {
			release();
			return;
		}
		slot->w = cw;
		slot->h = ch;
		slot->kind = kind;
		slot->photo = photo && !g_photoSaving.load();
		if (slot->photo) {
			g_photoSaving = true;
			g_photoRequested = false;
		}

		slot->alloc->Reset();
		g_c.list->Reset(slot->alloc, nullptr);
		D3D12_RESOURCE_BARRIER b{};
		b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
		b.Transition.pResource = bb;
		b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
		b.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
		b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
		g_c.list->ResourceBarrier(1, &b);
		D3D12_TEXTURE_COPY_LOCATION dst{};
		dst.pResource = slot->buf;
		dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
		dst.PlacedFootprint = slot->fp;
		D3D12_TEXTURE_COPY_LOCATION src{};
		src.pResource = bb;
		src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
		src.SubresourceIndex = 0;
		const D3D12_BOX box{ x0, y0, 0, x0 + cw, y0 + ch, 1 };
		g_c.list->CopyTextureRegion(&dst, 0, 0, 0, &src, &box);
		std::swap(b.Transition.StateBefore, b.Transition.StateAfter);
		g_c.list->ResourceBarrier(1, &b);
		g_c.list->Close();

		ID3D12CommandList* lists[] = { g_c.list };
		queue->ExecuteCommandLists(1, lists);
		slot->fence = g_c.nextFence++;
		queue->Signal(g_c.fence, slot->fence);
		slot->pending = true;
		g_c.last = now;

		if (++g_c.frames == 1) {
			Log("camera: first capture %ux%u fmt=%d, crop %ux%u at (%u,%u), viewfinder %dx%d", W, H, bd.Format, cw, ch, x0, y0, vw, vh);
		}
		release();
	}

	HRESULT STDMETHODCALLTYPE Present_Hook(IDXGISwapChain* a_this, UINT a_sync, UINT a_flags)
	{
		if (!(a_flags & DXGI_PRESENT_TEST)) {
			OnPresent(a_this);
		}
		return g_present(a_this, a_sync, a_flags);
	}

	HRESULT STDMETHODCALLTYPE Present1_Hook(IDXGISwapChain1* a_this, UINT a_sync, UINT a_flags, const DXGI_PRESENT_PARAMETERS* a_params)
	{
		if (!(a_flags & DXGI_PRESENT_TEST)) {
			OnPresent(a_this);
		}
		return g_present1(a_this, a_sync, a_flags, a_params);
	}

	template <class T>
	void Patch(void** a_vtbl, int a_slot, T a_hook, T& a_orig, const char* a_name)
	{
		DWORD old = 0;
		if (!VirtualProtect(&a_vtbl[a_slot], sizeof(void*), PAGE_EXECUTE_READWRITE, &old)) {
			Log("camera: hook %s: VirtualProtect failed", a_name);
			return;
		}
		a_orig = reinterpret_cast<T>(a_vtbl[a_slot]);
		a_vtbl[a_slot] = reinterpret_cast<void*>(a_hook);
		VirtualProtect(&a_vtbl[a_slot], sizeof(void*), old, &old);
		Log("camera: hooked %s (slot %d)", a_name, a_slot);
	}
}

namespace Camera
{
	bool Install(LogFn a_log)
	{
		g_log = a_log;

		const HINSTANCE inst = GetModuleHandleW(nullptr);
		WNDCLASSEXW     wc{};
		wc.cbSize = sizeof(wc);
		wc.lpfnWndProc = DefWindowProcW;
		wc.hInstance = inst;
		wc.lpszClassName = L"PhoneTestDummySwapChain";
		RegisterClassExW(&wc);
		HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"", WS_OVERLAPPEDWINDOW, 0, 0, 64, 64, nullptr, nullptr, inst, nullptr);

		ID3D12Device*       dev = nullptr;
		ID3D12CommandQueue* queue = nullptr;
		IDXGIFactory2*      factory = nullptr;
		IDXGISwapChain1*    sc = nullptr;
		bool                ok = false;
		D3D12_COMMAND_QUEUE_DESC qd{};
		qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
		DXGI_SWAP_CHAIN_DESC1 sd{};
		sd.Width = 64;
		sd.Height = 64;
		sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
		sd.SampleDesc.Count = 1;
		sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
		sd.BufferCount = 2;
		sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
		if (hwnd && SUCCEEDED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&dev))) &&
			SUCCEEDED(dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue))) &&
			SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))) &&
			SUCCEEDED(factory->CreateSwapChainForHwnd(queue, hwnd, &sd, nullptr, nullptr, &sc))) {
			auto** vt = *reinterpret_cast<void***>(sc);
			Patch(vt, kSlotPresent, &Present_Hook, g_present, "IDXGISwapChain::Present");
			Patch(vt, kSlotPresent1, &Present1_Hook, g_present1, "IDXGISwapChain1::Present1");
			ok = g_present && g_present1;
		} else {
			Log("camera: dummy swap chain failed; camera viewfinder disabled");
		}
		if (sc) sc->Release();
		if (factory) factory->Release();
		if (queue) queue->Release();
		if (dev) dev->Release();
		if (hwnd) DestroyWindow(hwnd);
		UnregisterClassW(wc.lpszClassName, inst);
		return ok;
	}

	void SetActive(bool a_open)
	{
		g_open = a_open;
	}

	void RequestPhoto()
	{
		if (!g_photoSaving.load()) {
			g_photoRequested = true;
		}
	}
}

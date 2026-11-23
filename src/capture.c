#define WIN32_LEAN_AND_MEAN
#define COBJMACROS
#define INITGUID
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <wincodec.h>
#include <math.h>
#include <string.h>
#include "exr.h"
#include "projection.h"
#include "print_spool.h"
#include "framing.h"
#include "optics.h"
#include "capture.h"
#include "overlay_gpu.h"
#include "studio.h"

typedef struct {
    ID3D12Resource *buffer;
    ID3D12CommandAllocator *allocator;
    ID3D12GraphicsCommandList *list;
    ID3D12Fence *fence;
    DXGI_FORMAT format;
    UINT width, height, stride;
    SIZE_T bytes;
    UINT logical_width, logical_height;
    float sample_scale, uv_x, uv_y;
} Readback;

typedef struct {
    WCHAR path[MAX_PATH];
    int frame;
    BOOL portrait, want_exr;
    PhotoSettings settings;
    unsigned duration_ms;
    unsigned initial_frame;
    ULONGLONG requested_at;
    ULONGLONG start, next_sample;
    unsigned samples, width, height;
    uint32_t *sums;
    BYTE *pixels, *scene;
    float *scene_sums;
    BOOL scene_failed;
    unsigned scale, phase, path_length, phase_token, pose_token;
    unsigned viewport_width, viewport_height;
    ULONGLONG phase_requested_at;
    PrintSpool spool;
} Shot;

enum { IDLE, PREPARING, PENDING, WRITING };
enum { METER_WIDTH = 12, METER_HEIGHT = 8, MAX_SAMPLES = 64 };
static Shot shot;
static volatile LONG state, result, cancel_requested;
static volatile LONG meter_pending;
static SRWLOCK meter_lock = SRWLOCK_INIT;
static uint32_t meter_seq, meter_completed;
static float meter_reading = -1;
static int meter_frame;
static BOOL meter_portrait;
static unsigned meter_hdr_frame;
static ULONGLONG meter_requested_at;
static Readback meter_readback;
static BOOL installed, queue_fault;
static IDXGISwapChain3 **game_chain;
static BYTE **game_scene, *scene_vtable;
static void (*game_signal)(void *, ID3D12CommandQueue *);
static void (*log_event)(const char *);
static uint32_t linear16[256];

/* The game thread records the requested motion once, then supplies the
   exact same sequence of views for every spatial phase on the render thread.
   Each pose must be applied before we request its new projection token. */
static SRWLOCK path_lock = SRWLOCK_INIT;
static struct {
    Camera poses[MAX_SAMPLES];
    unsigned count, wanted, ticket, applied, duration;
    BOOL recording;
    ULONGLONG start;
} replay;
static volatile LONG print_progress;

static void progress(unsigned done, unsigned total) {
    InterlockedExchange(&print_progress, (LONG)((done << 16) | total));
}

BOOL capture_print_progress(unsigned *done, unsigned *total) {
    unsigned value = (unsigned)InterlockedCompareExchange(&print_progress, 0, 0);
    if (done)
        *done = value >> 16;
    if (total)
        *total = value & 0xffffu;
    return value != 0;
}

void capture_print_camera(Camera *camera) {
    if (!camera || !capture_print_progress(NULL, NULL))
        return;
    AcquireSRWLockExclusive(&path_lock);
    if (replay.recording) {
        ULONGLONG now = GetTickCount64();
        if (!replay.count)
            replay.start = now;
        ULONGLONG elapsed = now - replay.start;
        if (elapsed * (MAX_SAMPLES - 1) >= (uint64_t)replay.duration * replay.count) {
            replay.poses[replay.count++] = *camera;
            if (elapsed >= replay.duration || replay.count == MAX_SAMPLES)
                replay.recording = FALSE;
        }
    } else if (replay.ticket && replay.wanted < replay.count) {
        *camera = replay.poses[replay.wanted];
        replay.applied = replay.ticket;
    }
    ReleaseSRWLockExclusive(&path_lock);
}

static BOOL canceled(void) {
    return (shot.duration_ms || shot.scale > 1) && InterlockedCompareExchange(&cancel_requested, 0, 0);
}

#define RELEASE(o)                                                                                           \
    do {                                                                                                     \
        if (o)                                                                                               \
            (o)->lpVtbl->Release(o);                                                                         \
    } while (0)

static void release_readback(Readback *r) {
    RELEASE(r->list);
    RELEASE(r->allocator);
    RELEASE(r->buffer);
    RELEASE(r->fence);
    memset(r, 0, sizeof *r);
}

static BOOL is_rgba(DXGI_FORMAT f) {
    return f == DXGI_FORMAT_R8G8B8A8_UNORM || f == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
}

static BOOL is_bgra(DXGI_FORMAT f) {
    return f == DXGI_FORMAT_B8G8R8A8_UNORM || f == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB ||
           f == DXGI_FORMAT_B8G8R8X8_UNORM || f == DXGI_FORMAT_B8G8R8X8_UNORM_SRGB;
}

static void convert_row(BYTE *dst, const BYTE *src, UINT count, DXGI_FORMAT format) {
    if (is_bgra(format)) {
        memcpy(dst, src, count * 4);
        return;
    }
    if (is_rgba(format)) {
        for (UINT x = 0; x < count; ++x, src += 4, dst += 4) {
            dst[0] = src[2];
            dst[1] = src[1];
            dst[2] = src[0];
            dst[3] = 255;
        }
    } else {
        for (UINT x = 0; x < count; ++x, src += 4, dst += 4) {
            uint32_t value;
            memcpy(&value, src, 4);
            dst[0] = (BYTE)((((value >> 20) & 1023) * 255 + 511) / 1023);
            dst[1] = (BYTE)((((value >> 10) & 1023) * 255 + 511) / 1023);
            dst[2] = (BYTE)(((value & 1023) * 255 + 511) / 1023);
            dst[3] = 255;
        }
    }
}

/* The target's direct queue has already submitted the frame and its final
   PRESENT barrier. Sampling and restoring it on that same queue orders both
   barriers against the game's work and the immediately following Present. */
static BOOL submit_copy(ID3D12CommandQueue *queue, int frame, BOOL portrait, BOOL meter, BOOL scene,
                        Readback *r) {
    IDXGISwapChain3 *chain = *game_chain;
    if (!chain || queue_fault)
        return FALSE;
    ID3D12Resource *source = NULL;
    ID3D12Device *device = NULL;
    HRESULT hr = S_OK;
    D3D12_RESOURCE_STATES prior = D3D12_RESOURCE_STATE_PRESENT;
    if (scene) {
        BYTE *object = *game_scene;
        if (!object || *(BYTE **)object != scene_vtable)
            return FALSE;
        source = *(ID3D12Resource **)(object + 0x38);
        if (!source)
            return FALSE;
        ID3D12Resource_AddRef(source);
        prior = *(const D3D12_RESOURCE_STATES *)(object + 0x40);
    } else {
        UINT index = IDXGISwapChain3_GetCurrentBackBufferIndex(chain);
        hr = IDXGISwapChain3_GetBuffer(chain, index, &IID_ID3D12Resource, (void **)&source);
        if (FAILED(hr))
            goto done;
    }
    D3D12_RESOURCE_DESC desc;
    source->lpVtbl->GetDesc(source, &desc);
    /* The scene surface allocation and its UNORM shader view are checked at
       install. The texture storage itself can be typeless. */
    if (scene && desc.Format != DXGI_FORMAT_R8G8B8A8_TYPELESS && desc.Format != DXGI_FORMAT_R8G8B8A8_UNORM) {
        hr = E_INVALIDARG;
        goto done;
    }
    if (scene)
        desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    if (desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || desc.SampleDesc.Count != 1 || !desc.Width ||
        !desc.Height || desc.Width > 16384 || desc.Height > 16384 ||
        (!is_rgba(desc.Format) && !is_bgra(desc.Format) && desc.Format != DXGI_FORMAT_R10G10B10A2_UNORM)) {
        hr = E_INVALIDARG;
        goto done;
    }
    RECT crop = studio_crop((int)desc.Width, (int)desc.Height, frame, portrait);
    if (crop.left < 0 || crop.top < 0 || crop.right > (LONG)desc.Width || crop.bottom > (LONG)desc.Height ||
        crop.right <= crop.left || crop.bottom <= crop.top) {
        hr = E_INVALIDARG;
        goto done;
    }
    UINT width = crop.right - crop.left, height = crop.bottom - crop.top;
    r->logical_width = width;
    r->logical_height = height;
    float scale = scene ? optics_scene_scale() : 1.f;
    if (!(scale > 0 && scale <= 1.0f)) {
        hr = E_INVALIDARG;
        goto done;
    }
    r->sample_scale = scale;
    /* Scene texture lookup in PostProcessHDR is Sample(linearClamp, UV*c2.w).
       Keep its one-texel interpolation halo at both crop edges. */
    RECT source_crop = crop;
    if (scene && scale != 1) {
        source_crop.left = (LONG)fmaxf(0, floorf(crop.left * scale - .5f));
        source_crop.top = (LONG)fmaxf(0, floorf(crop.top * scale - .5f));
        source_crop.right = (LONG)fminf((float)desc.Width, ceilf(crop.right * scale + .5f));
        source_crop.bottom = (LONG)fminf((float)desc.Height, ceilf(crop.bottom * scale + .5f));
        r->uv_x = crop.left * scale - source_crop.left;
        r->uv_y = crop.top * scale - source_crop.top;
    }
    r->width = meter ? METER_WIDTH : (UINT)(source_crop.right - source_crop.left);
    r->height = meter ? METER_HEIGHT : (UINT)(source_crop.bottom - source_crop.top);
    r->stride = (r->width * 4 + 255u) & ~255u;
    r->bytes = (SIZE_T)r->stride * r->height;
    r->format = desc.Format;
    hr = ID3D12Resource_GetDevice(source, &IID_ID3D12Device, (void **)&device);
    if (FAILED(hr))
        goto done;
    D3D12_HEAP_PROPERTIES heap = {
        .Type = D3D12_HEAP_TYPE_READBACK, .CreationNodeMask = 1, .VisibleNodeMask = 1};
    D3D12_RESOURCE_DESC buffer = {.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER,
                                  .Width = r->bytes,
                                  .Height = 1,
                                  .DepthOrArraySize = 1,
                                  .MipLevels = 1,
                                  .SampleDesc = {1, 0},
                                  .Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR};
    hr = ID3D12Device_CreateCommittedResource(device, &heap, D3D12_HEAP_FLAG_NONE, &buffer,
                                              D3D12_RESOURCE_STATE_COPY_DEST, NULL, &IID_ID3D12Resource,
                                              (void **)&r->buffer);
    if (FAILED(hr))
        goto done;
    hr = ID3D12Device_CreateCommandAllocator(device, D3D12_COMMAND_LIST_TYPE_DIRECT,
                                             &IID_ID3D12CommandAllocator, (void **)&r->allocator);
    if (FAILED(hr))
        goto done;
    hr = ID3D12Device_CreateCommandList(device, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, r->allocator, NULL,
                                        &IID_ID3D12GraphicsCommandList, (void **)&r->list);
    if (FAILED(hr))
        goto done;
    hr = ID3D12Device_CreateFence(device, 0, D3D12_FENCE_FLAG_NONE, &IID_ID3D12Fence, (void **)&r->fence);
    if (FAILED(hr))
        goto done;
    D3D12_RESOURCE_BARRIER barrier = {.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION};
    barrier.Transition.pResource = source;
    barrier.Transition.Subresource = 0;
    barrier.Transition.StateBefore = prior;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    if (prior != D3D12_RESOURCE_STATE_COPY_SOURCE)
        ID3D12GraphicsCommandList_ResourceBarrier(r->list, 1, &barrier);
    D3D12_TEXTURE_COPY_LOCATION dst = {.pResource = r->buffer,
                                       .Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
    dst.PlacedFootprint.Footprint =
        (D3D12_SUBRESOURCE_FOOTPRINT){desc.Format, r->width, r->height, 1, r->stride};
    D3D12_TEXTURE_COPY_LOCATION src = {.pResource = source,
                                       .Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
    if (meter) {
        for (UINT y = 0; y < METER_HEIGHT; ++y)
            for (UINT x = 0; x < METER_WIDTH; ++x) {
                UINT sx = (UINT)((crop.left + (x + .5f) * width / METER_WIDTH) * scale);
                UINT sy = (UINT)((crop.top + (y + .5f) * height / METER_HEIGHT) * scale);
                D3D12_BOX pixel = {sx, sy, 0, sx + 1, sy + 1, 1};
                ID3D12GraphicsCommandList_CopyTextureRegion(r->list, &dst, x, y, 0, &src, &pixel);
            }
    } else {
        D3D12_BOX box = {(UINT)source_crop.left,  (UINT)source_crop.top,    0,
                         (UINT)source_crop.right, (UINT)source_crop.bottom, 1};
        ID3D12GraphicsCommandList_CopyTextureRegion(r->list, &dst, 0, 0, 0, &src, &box);
    }
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
    barrier.Transition.StateAfter = prior;
    if (prior != D3D12_RESOURCE_STATE_COPY_SOURCE)
        ID3D12GraphicsCommandList_ResourceBarrier(r->list, 1, &barrier);
    hr = ID3D12GraphicsCommandList_Close(r->list);
    if (FAILED(hr))
        goto done;
    ID3D12CommandList *list = (ID3D12CommandList *)r->list;
    ID3D12CommandQueue_ExecuteCommandLists(queue, 1, &list);
    hr = ID3D12CommandQueue_Signal(queue, r->fence, 1);
    if (FAILED(hr)) {
        /* The submitted resources cannot be recycled without a completion
           signal. Keep this one allocation alive and stop submitting work. */
        memset(r, 0, sizeof *r);
        queue_fault = TRUE;
        log_event("Capture queue signal failed.\n");
    }
done:
    RELEASE(device);
    RELEASE(source);
    if (FAILED(hr))
        release_readback(r);
    return SUCCEEDED(hr);
}

static BYTE *map_readback(Readback *r) {
    BYTE *mapped = NULL;
    D3D12_RANGE read = {0, r->bytes};
    return SUCCEEDED(ID3D12Resource_Map(r->buffer, 0, &read, (void **)&mapped)) ? mapped : NULL;
}

static void unmap_readback(Readback *r) {
    D3D12_RANGE written = {0, 0};
    ID3D12Resource_Unmap(r->buffer, 0, &written);
}

static void finish_meter(float reading) {
    AcquireSRWLockExclusive(&meter_lock);
    meter_reading = reading;
    meter_completed = meter_seq;
    ReleaseSRWLockExclusive(&meter_lock);
    InterlockedExchange(&meter_pending, 0);
}

static void tick_meter(ID3D12CommandQueue *queue) {
    if (!meter_readback.fence) {
        if (InterlockedCompareExchange(&meter_pending, 0, 0) != 1)
            return;
        if (optics_grade_supported() && optics_frame_id() == meter_hdr_frame) {
            if (GetTickCount64() - meter_requested_at >= 2000)
                finish_meter(-1);
            return;
        }
        if (InterlockedCompareExchange(&meter_pending, 2, 1) != 1)
            return;
        if (!submit_copy(queue, meter_frame, meter_portrait, TRUE, TRUE, &meter_readback))
            finish_meter(-1);
        return;
    }
    UINT64 complete = ID3D12Fence_GetCompletedValue(meter_readback.fence);
    if (!complete)
        return;
    BYTE *mapped = complete != UINT64_MAX ? map_readback(&meter_readback) : NULL;
    float value = -1;
    if (mapped) {
        BYTE pixels[METER_WIDTH * METER_HEIGHT * 4];
        for (UINT y = 0; y < METER_HEIGHT; ++y)
            convert_row(pixels + y * METER_WIDTH * 4, mapped + y * meter_readback.stride, METER_WIDTH,
                        meter_readback.format);
        value = photo_meter_scene(pixels, METER_WIDTH, METER_HEIGHT, METER_WIDTH * 4, TRUE);
        unmap_readback(&meter_readback);
    }
    release_readback(&meter_readback);
    finish_meter(value);
}

static BOOL save_png(const WCHAR *path, BYTE *pixels, UINT width, UINT height) {
    WCHAR directory[MAX_PATH], temporary[MAX_PATH];
    lstrcpyW(directory, path);
    WCHAR *sep = NULL;
    for (WCHAR *c = directory; *c; ++c)
        if (*c == L'\\' || *c == L'/')
            sep = c;
    if (!sep)
        return FALSE;
    sep[1] = 0;
    if (!GetTempFileNameW(directory, L"WPC", 0, temporary))
        return FALSE;
    IWICImagingFactory *factory = NULL;
    IWICBitmapEncoder *encoder = NULL;
    IWICBitmapFrameEncode *frame = NULL;
    IWICStream *stream = NULL;
    HRESULT init = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    HRESULT hr = CoCreateInstance(&CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER,
                                  &IID_IWICImagingFactory, (void **)&factory);
    if (FAILED(hr))
        goto done;
    hr = IWICImagingFactory_CreateStream(factory, &stream);
    if (FAILED(hr))
        goto done;
    hr = IWICStream_InitializeFromFilename(stream, temporary, GENERIC_WRITE);
    if (FAILED(hr))
        goto done;
    hr = IWICImagingFactory_CreateEncoder(factory, &GUID_ContainerFormatPng, NULL, &encoder);
    if (FAILED(hr))
        goto done;
    hr = IWICBitmapEncoder_Initialize(encoder, (IStream *)stream, WICBitmapEncoderNoCache);
    if (FAILED(hr))
        goto done;
    hr = IWICBitmapEncoder_CreateNewFrame(encoder, &frame, NULL);
    if (FAILED(hr))
        goto done;
    hr = IWICBitmapFrameEncode_Initialize(frame, NULL);
    if (FAILED(hr))
        goto done;
    hr = IWICBitmapFrameEncode_SetSize(frame, width, height);
    if (FAILED(hr))
        goto done;
    WICPixelFormatGUID format = GUID_WICPixelFormat24bppBGR;
    hr = IWICBitmapFrameEncode_SetPixelFormat(frame, &format);
    if (SUCCEEDED(hr) && memcmp(&format, &GUID_WICPixelFormat24bppBGR, sizeof format))
        hr = E_FAIL;
    if (FAILED(hr))
        goto done;
    if (shot.scale > 1) {
        BYTE *row = HeapAlloc(GetProcessHeap(), 0, width * 3u);
        hr = row ? S_OK : E_OUTOFMEMORY;
        for (UINT y = 0; SUCCEEDED(hr) && y < height; ++y) {
            if (canceled() || !print_png_row(&shot.spool, y, row)) {
                hr = E_ABORT;
                break;
            }
            hr = IWICBitmapFrameEncode_WritePixels(frame, 1, width * 3, width * 3, row);
        }
        if (row)
            HeapFree(GetProcessHeap(), 0, row);
    } else {
        for (SIZE_T i = 0; i < (SIZE_T)width * height; ++i) {
            BYTE b = pixels[i * 4], g = pixels[i * 4 + 1], r = pixels[i * 4 + 2];
            pixels[i * 3] = b;
            pixels[i * 3 + 1] = g;
            pixels[i * 3 + 2] = r;
        }
        hr = IWICBitmapFrameEncode_WritePixels(frame, height, width * 3, width * height * 3, pixels);
    }
    if (SUCCEEDED(hr))
        hr = IWICBitmapFrameEncode_Commit(frame);
    if (SUCCEEDED(hr))
        hr = IWICBitmapEncoder_Commit(encoder);
done:
    RELEASE(frame);
    RELEASE(encoder);
    RELEASE(stream);
    RELEASE(factory);
    if (SUCCEEDED(init))
        CoUninitialize();
    if (SUCCEEDED(hr) && !canceled() && MoveFileExW(temporary, path, MOVEFILE_WRITE_THROUGH))
        return TRUE;
    DeleteFileW(temporary);
    return FALSE;
}

static void release_pixels(void) {
    if (shot.pixels)
        HeapFree(GetProcessHeap(), 0, shot.pixels);
    if (shot.sums)
        HeapFree(GetProcessHeap(), 0, shot.sums);
    if (shot.scene)
        HeapFree(GetProcessHeap(), 0, shot.scene);
    if (shot.scene_sums)
        HeapFree(GetProcessHeap(), 0, shot.scene_sums);
    shot.pixels = shot.scene = NULL;
    shot.sums = NULL;
    shot.scene_sums = NULL;
}
static void finish(int status) {
    release_pixels();
    if (shot.scale > 1) {
        projection_reset();
        AcquireSRWLockExclusive(&path_lock);
        memset(&replay, 0, sizeof replay);
        ReleaseSRWLockExclusive(&path_lock);
        print_close(&shot.spool);
        progress(0, 0);
    }
    InterlockedExchange(&result, status);
    InterlockedExchange(&state, IDLE);
}

static BOOL resolve_display(void) {
    if (shot.duration_ms) {
        /* Invert the same sRGB table used during accumulation. Midpoint
           rounding preserves a static composition exactly across samples. */
        BYTE *encode = HeapAlloc(GetProcessHeap(), 0, 65536);
        if (!encode)
            return FALSE;
        UINT value = 0;
        for (UINT i = 0; i < 65536; ++i) {
            while (value < 255 && 2 * i > linear16[value] + linear16[value + 1])
                ++value;
            encode[i] = (BYTE)value;
        }
        for (SIZE_T i = 0; i < (SIZE_T)shot.width * shot.height; ++i) {
            for (UINT c = 0; c < 3; ++c)
                shot.pixels[4 * i + c] = encode[(shot.sums[3 * i + c] + shot.samples / 2) / shot.samples];
            shot.pixels[4 * i + 3] = 255;
        }
        HeapFree(GetProcessHeap(), 0, encode);
    }
    photo_develop(&shot.settings, shot.pixels, shot.width, shot.height, shot.width * 4, TRUE);
    return TRUE;
}
static BOOL read_print_scene(void *context, unsigned y, float *row, float gain) {
    return !canceled() && print_exr_row(context, y, row, gain);
}
static DWORD WINAPI develop_worker(void *unused) {
    (void)unused;
    BOOL print = shot.scale > 1;
    if (!print && !resolve_display()) {
        finish(-1);
        return 0;
    }
    if (canceled()) {
        finish(-2);
        return 0;
    }
    unsigned width = shot.width * shot.scale, height = shot.height * shot.scale;
    BOOL saved = save_png(shot.path, shot.pixels, width, height);
    if (!saved && canceled()) {
        finish(-2);
        return 0;
    }
    BOOL exr = FALSE;
    if (saved) {
        log_event("PNG saved.\n");
        if (shot.want_exr && !shot.scene_failed) {
            if (print)
                exr = photo_write_exr_rows(shot.path, &shot.settings, width, height, shot.path_length,
                                           shot.scale, read_print_scene, &shot.spool);
            else if (shot.scene_sums)
                exr = photo_write_exr_sum(shot.path, &shot.settings, shot.scene_sums, shot.width, shot.height,
                                          shot.samples);
            else if (shot.scene)
                exr = photo_write_exr(shot.path, &shot.settings, shot.scene, shot.width, shot.height,
                                      shot.width * 4, FALSE);
        }
        if (shot.want_exr)
            log_event(exr ? "Scene-linear EXR saved.\n" : "Scene-linear EXR unavailable.\n");
    } else
        log_event("PNG write failed.\n");
    finish(saved ? (shot.want_exr ? (exr ? 2 : 3) : 1) : -1);
    return 0;
}

/* A capped temporal print can have a nonintegral ratio between the native
   raster and its accumulation grid. Keep sample centers aligned with the
   shifted lens, filtering display values in linear light as we accumulate. */
static BOOL accumulate_resized_print(const Readback *r, const BYTE *mapped) {
    BYTE *rows = HeapAlloc(GetProcessHeap(), 0, (SIZE_T)r->width * 8);
    if (!rows)
        return FALSE;
    BYTE *bottom = rows + r->width * 4;
    double step_x = (double)r->width / shot.width, step_y = (double)r->height / shot.height;
    for (UINT y = 0; y < shot.height; ++y) {
        double fy = (y + .5) * step_y - .5;
        UINT y0 = (UINT)fy, y1 = y0 + 1 < r->height ? y0 + 1 : y0;
        float wy = (float)(fy - y0);
        convert_row(rows, mapped + (SIZE_T)y0 * r->stride, r->width, r->format);
        convert_row(bottom, mapped + (SIZE_T)y1 * r->stride, r->width, r->format);
        uint32_t *sum = shot.sums + (SIZE_T)y * shot.width * 3;
        for (UINT x = 0; x < shot.width; ++x, sum += 3) {
            double fx = (x + .5) * step_x - .5;
            UINT x0 = (UINT)fx, x1 = x0 + 1 < r->width ? x0 + 1 : x0;
            float wx = (float)(fx - x0);
            for (UINT c = 0; c < 3; ++c) {
                float a = linear16[rows[4 * x0 + c]], b = linear16[rows[4 * x1 + c]];
                float d = linear16[bottom[4 * x0 + c]], e = linear16[bottom[4 * x1 + c]];
                float top = a + wx * (b - a), lower = d + wx * (e - d);
                sum[c] += (uint32_t)(top + wy * (lower - top) + .5f);
            }
        }
    }
    HeapFree(GetProcessHeap(), 0, rows);
    return TRUE;
}

static BOOL ingest_sample(Readback *r) {
    PhotoSize base = frame_output(r->width, r->height, 1, shot.duration_ms != 0);
    UINT width = (UINT)base.width, height = (UINT)base.height;
    if (!width || !height)
        return FALSE;
    if (shot.samples && (width != shot.width || height != shot.height))
        return FALSE;
    SIZE_T count = (SIZE_T)width * height;
    if (!shot.samples) {
        shot.width = width;
        shot.height = height;
        shot.pixels = HeapAlloc(GetProcessHeap(), 0, count * 4);
        if (!shot.pixels)
            return FALSE;
        if (shot.duration_ms) {
            shot.sums = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, count * 3 * sizeof *shot.sums);
            if (!shot.sums)
                return FALSE;
        }
    }
    BYTE *mapped = map_readback(r);
    if (!mapped)
        return FALSE;
    if (shot.scale > 1 && (width != r->width || height != r->height)) {
        BOOL ok = accumulate_resized_print(r, mapped);
        unmap_readback(r);
        if (ok)
            ++shot.samples;
        return ok;
    }
    for (UINT y = 0; y < height; ++y) {
        BYTE *dst = shot.pixels + (SIZE_T)y * width * 4;
        UINT sy = (UINT)(((uint64_t)(2 * y + 1) * r->height) / (2 * height));
        const BYTE *src = mapped + (SIZE_T)sy * r->stride;
        if (width == r->width)
            convert_row(dst, src, width, r->format);
        else
            for (UINT x = 0; x < width; ++x) {
                UINT sx = (UINT)(((uint64_t)(2 * x + 1) * r->width) / (2 * width));
                convert_row(dst + 4 * x, src + 4 * sx, 1, r->format);
            }
        if (shot.duration_ms) {
            uint32_t *sum = shot.sums + (SIZE_T)y * width * 3;
            for (UINT x = 0; x < width; ++x) {
                sum[3 * x] += linear16[dst[4 * x]];
                sum[3 * x + 1] += linear16[dst[4 * x + 1]];
                sum[3 * x + 2] += linear16[dst[4 * x + 2]];
            }
        }
    }
    ++shot.samples;
    unmap_readback(r);
    return TRUE;
}

/* SceneResolveSurface is t0 of the HDR tone mapper. Its linear RGB samples
   are 8-bit UNORM and can be exported without reversing a display curve. */
static BOOL ingest_scene(Readback *r, UINT expected_width, UINT expected_height) {
    if (!r->buffer || r->logical_width != expected_width || r->logical_height != expected_height)
        return FALSE;
    BOOL native = r->sample_scale == 1 && r->width == shot.width && r->height == shot.height;
    SIZE_T count = (SIZE_T)shot.width * shot.height;
    if (!shot.duration_ms && native) {
        shot.scene = HeapAlloc(GetProcessHeap(), 0, count * 4);
        if (!shot.scene)
            return FALSE;
    } else if (!shot.scene_sums) {
        shot.scene_sums = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, count * 3 * sizeof *shot.scene_sums);
        if (!shot.scene_sums)
            return FALSE;
    }
    BYTE *mapped = map_readback(r);
    if (!mapped)
        return FALSE;
    const float step_x = (float)r->logical_width / shot.width * r->sample_scale;
    const float step_y = (float)r->logical_height / shot.height * r->sample_scale;
    for (UINT y = 0; y < shot.height; ++y) {
        SIZE_T row = (SIZE_T)y * shot.width;
        if (native) {
            const BYTE *p = mapped + (SIZE_T)y * r->stride;
            if (shot.scene)
                memcpy(shot.scene + row * 4, p, shot.width * 4);
            else {
                float *sum = shot.scene_sums + row * 3;
                for (UINT x = 0; x < shot.width; ++x, p += 4, sum += 3) {
                    sum[0] += p[0];
                    sum[1] += p[1];
                    sum[2] += p[2];
                }
            }
            continue;
        }
        float fy = fminf((float)(r->height - 1), fmaxf(0, r->uv_y + (y + .5f) * step_y - .5f));
        UINT y0 = (UINT)fy, y1 = y0 + 1 < r->height ? y0 + 1 : y0;
        const BYTE *top = mapped + (SIZE_T)y0 * r->stride, *bottom = mapped + (SIZE_T)y1 * r->stride;
        float wy = fy - y0;
        float *sum = shot.scene_sums + row * 3;
        for (UINT x = 0; x < shot.width; ++x, sum += 3) {
            float fx = fminf((float)(r->width - 1), fmaxf(0, r->uv_x + (x + .5f) * step_x - .5f));
            UINT x0 = (UINT)fx, x1 = x0 + 1 < r->width ? x0 + 1 : x0;
            float wx = fx - x0;
            for (UINT c = 0; c < 3; ++c) {
                float a = top[4 * x0 + c] + wx * (top[4 * x1 + c] - top[4 * x0 + c]);
                float b = bottom[4 * x0 + c] + wx * (bottom[4 * x1 + c] - bottom[4 * x0 + c]);
                sum[c] += a + wy * (b - a);
            }
        }
    }
    unmap_readback(r);
    return TRUE;
}

static void start_developing(void) {
    InterlockedExchange(&state, WRITING);
    HANDLE thread = CreateThread(NULL, 0, develop_worker, NULL, 0, NULL);
    if (thread) CloseHandle(thread);
    else finish(-1);
}

static BOOL print_dimensions(void) {
    IDXGISwapChain3 *chain = *game_chain;
    DXGI_SWAP_CHAIN_DESC1 desc;
    if (!chain || FAILED(IDXGISwapChain3_GetDesc1(chain, &desc)) ||
        !frame_scale_fits((int)desc.Width, (int)desc.Height, (int)shot.scale, shot.duration_ms != 0))
        return FALSE;
    if (shot.viewport_width && (desc.Width != shot.viewport_width || desc.Height != shot.viewport_height))
        return FALSE;
    shot.viewport_width = desc.Width;
    shot.viewport_height = desc.Height;
    return TRUE;
}

static BOOL phase_ready(ULONGLONG now) {
    Camera pose;
    if (!print_dimensions()) {
        log_event("Print capture dimensions unavailable or changed.\n");
        finish(-1);
        return FALSE;
    }
    if (shot.duration_ms) {
        AcquireSRWLockExclusive(&path_lock);
        if (replay.recording) {
            ReleaseSRWLockExclusive(&path_lock);
            if (now - shot.requested_at > shot.duration_ms + 5000) {
                log_event("No camera updates while recording the print exposure.\n");
                finish(-1);
            }
            return FALSE;
        }
        shot.path_length = replay.count;
        if (!shot.pose_token) {
            replay.wanted = shot.samples;
            shot.pose_token = ++replay.ticket;
            shot.phase_requested_at = now;
        }
        BOOL applied = replay.applied == shot.pose_token;
        if (applied)
            pose = replay.poses[replay.wanted];
        ReleaseSRWLockExclusive(&path_lock);
        if (!applied) {
            if (now - shot.phase_requested_at > 5000) {
                log_event("Timed print camera did not apply its requested pose.\n");
                finish(-1);
            }
            return FALSE;
        }
    }
    if (!shot.phase_token) {
        unsigned width = shot.viewport_width, height = shot.viewport_height;
        if (shot.duration_ms) {
            RECT crop = studio_crop(width, height, shot.frame, shot.portrait);
            unsigned w = crop.right - crop.left, h = crop.bottom - crop.top;
            PhotoSize base = frame_output(w, h, 1, TRUE);
            if (!base.width || !base.height) {
                finish(-1);
                return FALSE;
            }
            width = (width * base.width + w / 2) / w;
            height = (height * base.height + h / 2) / h;
        }
        shot.phase_requested_at = now;
        shot.phase_token =
            projection_phase_pose(shot.scale, shot.phase, width, height, shot.duration_ms ? &pose : NULL);
        if (!shot.phase_token) {
            log_event("Print projection unavailable.\n");
            finish(-1);
        }
        return FALSE;
    }
    if (projection_ready(shot.phase_token))
        return TRUE;
    if (now - shot.phase_requested_at >= 3000) {
        log_event("No rendered frame for print projection.\n");
        finish(-1);
    }
    return FALSE;
}

static BOOL collect_print_sample(void) {
    unsigned phases = shot.scale * shot.scale;
    shot.pose_token = shot.phase_token = 0;
    progress(shot.phase * shot.path_length + shot.samples, phases * shot.path_length);
    if (shot.samples < shot.path_length)
        return TRUE;
    if (!resolve_display() || !print_store_display(&shot.spool, shot.path, shot.phase, shot.width,
                                                   shot.height, shot.scale, shot.pixels))
        return FALSE;
    if (shot.want_exr && !shot.scene_failed)
        shot.scene_failed =
            !print_store_scene(&shot.spool, shot.path, shot.phase, shot.scene, shot.scene_sums);
    release_pixels();
    shot.samples = 0;
    if (++shot.phase == phases) {
        projection_reset();
        AcquireSRWLockExclusive(&path_lock);
        replay.ticket = replay.applied = 0;
        ReleaseSRWLockExclusive(&path_lock);
        start_developing();
    }
    return TRUE;
}

static void tick_shot(ID3D12CommandQueue *queue) {
    static Readback readback, scene;
    static ULONGLONG sampled_at;
    if (InterlockedCompareExchange(&state, 0, 0) != PENDING)
        return;
    ULONGLONG now = GetTickCount64();
    if (readback.fence) {
        UINT64 complete = ID3D12Fence_GetCompletedValue(readback.fence);
        UINT64 scene_complete = scene.fence ? ID3D12Fence_GetCompletedValue(scene.fence) : 1;
        if (!complete || !scene_complete)
            return;
        BOOL stopped = canceled();
        BOOL ok = !stopped && complete != UINT64_MAX && ingest_sample(&readback);
        if (ok && shot.want_exr && !shot.scene_failed)
            shot.scene_failed =
                scene_complete == UINT64_MAX || !ingest_scene(&scene, readback.width, readback.height);
        release_readback(&scene);
        release_readback(&readback);
        if (!ok) {
            log_event(stopped ? "Exposure canceled.\n" : "GPU readback failed.\n");
            finish(stopped ? -2 : -1);
            return;
        }
        if (shot.scale > 1) {
            if (!collect_print_sample()) {
                log_event("Could not store the print exposure.\n");
                finish(-1);
            }
            return;
        }
        if (!shot.duration_ms || sampled_at - shot.start >= shot.duration_ms || shot.samples >= MAX_SAMPLES) {
            start_developing();
            return;
        }
    }
    if (canceled()) {
        finish(-2);
        return;
    }
    if (shot.scale > 1 && !phase_ready(now))
        return;
    /* The game thread can release the shutter while the render thread is
       still presenting the previous camera pose and photo settings. */
    if (shot.scale == 1 && !shot.samples && optics_grade_supported() &&
        optics_frame_id() == shot.initial_frame) {
        if (now - shot.requested_at >= 2000) {
            log_event("No new HDR frame after shutter release.\n");
            finish(-1);
        }
        return;
    }
    if (shot.scale == 1 && now < shot.next_sample)
        return;
    if (!submit_copy(queue, shot.frame, shot.portrait, FALSE, FALSE, &readback)) {
        log_event("Capture source unavailable.\n");
        finish(-1);
        return;
    }
    if (shot.want_exr && !shot.scene_failed)
        shot.scene_failed = !submit_copy(queue, shot.frame, shot.portrait, FALSE, TRUE, &scene);
    sampled_at = now;
    if (!shot.samples)
        shot.start = now;
    shot.next_sample =
        shot.start + ((uint64_t)shot.duration_ms * (shot.samples + 1) + MAX_SAMPLES - 2) / (MAX_SAMPLES - 1);
}

static void signal_frame(void *owner, ID3D12CommandQueue *queue) {
    /* The game waits on this fence before releasing frame resources. Put
       photo readbacks ahead of that signal so resize and teardown also
       wait for our copies of the application's source textures. */
    tick_shot(queue);
    tick_meter(queue);
    overlay_draw(queue, *game_chain, log_event);
    game_signal(owner, queue);
}

BOOL capture_install(unsigned char *base, BOOL (*redirect)(unsigned char *, unsigned char *, uintptr_t),
                     void (*log)(const char *)) {
    /* Signal follows frame submission, immediately before Present(0,0).
       Check the game-owned queue load and swapchain load as well as the call. */
    static const BYTE site[] = {0x48, 0x8b, 0x15, 0x73, 0xe7, 0x59, 0x01, 0x48, 0x8b, 0xe9,
                                0x48, 0x8b, 0x89, 0x90, 0,    0,    0,    0xe8, 0x7c, 0x5b,
                                0x01, 0,    0x48, 0x8b, 0x0d, 0x35, 0xd5, 0x59, 0x01, 0x45,
                                0x33, 0xc0, 0x33, 0xd2, 0x48, 0x8b, 0x01, 0xff, 0x50, 0x40};
    static const BYTE scene_format[] = {0xc7, 0x44, 0x24, 0x20, 0x1c, 0, 0, 0, 0xe8, 0xb0, 0xa8, 0x01, 0};
    if (memcmp(base + 0x7b685e, site, sizeof site) ||
        memcmp(base + 0x7a5ee3, scene_format, sizeof scene_format))
        return FALSE;
    game_scene = (BYTE **)(base + 0x1d53a38);
    scene_vtable = base + 0xa4bff8;
    log_event = log;
    game_chain = (IDXGISwapChain3 **)(base + 0x1d53db0);
    game_signal = (void (*)(void *, ID3D12CommandQueue *))(base + 0x7cc3f0);
    installed = redirect(base, base + 0x7b686f, (uintptr_t)signal_frame);
    return installed;
}

BOOL capture_request(const WCHAR *path, int frame, BOOL portrait, BOOL want_exr,
                     const PhotoSettings *settings, float seconds, unsigned scale) {
    if (!installed || !path || !*path || !settings || lstrlenW(path) >= MAX_PATH || scale < 1 || scale > 4 ||
        !(seconds >= 0 && seconds <= 8) || InterlockedCompareExchange(&state, PREPARING, IDLE) != IDLE)
        return FALSE;
    memset(&shot, 0, sizeof shot);
    lstrcpyW(shot.path, path);
    shot.frame = frame;
    shot.portrait = portrait;
    shot.want_exr = want_exr;
    shot.duration_ms = (unsigned)(seconds * 1000.0f + 0.5f);
    shot.scale = scale;
    shot.path_length = 1;
    if (scale > 1) {
        AcquireSRWLockExclusive(&path_lock);
        memset(&replay, 0, sizeof replay);
        replay.duration = shot.duration_ms;
        replay.recording = replay.duration != 0;
        ReleaseSRWLockExclusive(&path_lock);
        progress(0, scale * scale);
    }
    shot.settings = *settings;
    shot.requested_at = GetTickCount64();
    shot.initial_frame = optics_frame_id();
    if (shot.duration_ms && linear16[255] == 0)
        for (unsigned i = 0; i < 256; ++i) {
            double v = i / 255.0;
            linear16[i] =
                (uint32_t)(65535.0 * (v <= 0.04045 ? v / 12.92 : pow((v + 0.055) / 1.055, 2.4)) + 0.5);
        }
    InterlockedExchange(&cancel_requested, 0);
    InterlockedExchange(&result, 0);
    InterlockedExchange(&state, PENDING);
    return TRUE;
}

BOOL capture_busy(void) {
    return InterlockedCompareExchange(&state, 0, 0) != IDLE;
}
int capture_result(void) {
    return (int)InterlockedCompareExchange(&result, 0, 0);
}
void capture_cancel(void) {
    InterlockedExchange(&cancel_requested, 1);
}

uint32_t capture_meter(int frame, BOOL portrait) {
    if (!installed || InterlockedCompareExchange(&meter_pending, -1, 0) != 0)
        return 0;
    meter_frame = frame;
    meter_portrait = portrait;
    meter_hdr_frame = optics_frame_id();
    meter_requested_at = GetTickCount64();
    if (!++meter_seq)
        ++meter_seq;
    uint32_t requested = meter_seq;
    InterlockedExchange(&meter_pending, 1);
    return requested;
}

uint32_t capture_meter_result(float *luminance) {
    AcquireSRWLockShared(&meter_lock);
    uint32_t completed = meter_completed;
    if (luminance)
        *luminance = meter_reading;
    ReleaseSRWLockShared(&meter_lock);
    return completed;
}

float capture_luminance(void) {
    float value;
    capture_meter_result(&value);
    return value;
}

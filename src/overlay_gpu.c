#define WIN32_LEAN_AND_MEAN
#define COBJMACROS
#include "overlay_gpu.h"
#include "studio.h"
#include "overlay_bytecode.h"
#include <string.h>
#include <stdint.h>

enum { FRAMES = 4 };
typedef struct {
    ID3D12CommandAllocator *allocator;
    ID3D12GraphicsCommandList *list;
    ID3D12Resource *upload;
    BYTE *pixels;
    UINT64 last_use;
} Frame;
static struct {
    ID3D12Device *device;
    ID3D12Fence *fence;
    ID3D12RootSignature *root;
    ID3D12PipelineState *pipeline;
    ID3D12DescriptorHeap *rtvs, *srv;
    ID3D12Resource *texture;
    Frame frame[FRAMES];
    DXGI_FORMAT format;
    UINT width, height, source_width, source_height, pitch, rtv_stride, generation;
    UINT64 submitted;
} gpu;
static BOOL online, fault;
static ULONGLONG retry_at;
#define RELEASE(o)                                                                                           \
    do {                                                                                                     \
        if (o)                                                                                               \
            (o)->lpVtbl->Release(o);                                                                         \
    } while (0)

static void destroy(void) {
    for (int i = 0; i < FRAMES; ++i) {
        Frame *f = &gpu.frame[i];
        if (f->pixels)
            ID3D12Resource_Unmap(f->upload, 0, NULL);
        RELEASE(f->list);
        RELEASE(f->allocator);
        RELEASE(f->upload);
    }
    RELEASE(gpu.texture);
    RELEASE(gpu.srv);
    RELEASE(gpu.rtvs);
    RELEASE(gpu.pipeline);
    RELEASE(gpu.root);
    RELEASE(gpu.fence);
    RELEASE(gpu.device);
    memset(&gpu, 0, sizeof gpu);
}

static HRESULT make_pipeline(void) {
    D3D12_DESCRIPTOR_RANGE range = {.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV, .NumDescriptors = 1};
    D3D12_ROOT_PARAMETER table = {.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE,
                                  .ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL};
    table.DescriptorTable.NumDescriptorRanges = 1;
    table.DescriptorTable.pDescriptorRanges = &range;
    D3D12_STATIC_SAMPLER_DESC sampler = {.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT,
                                         .AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
                                         .AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
                                         .AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
                                         .MaxAnisotropy = 1,
                                         .ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS,
                                         .MaxLOD = D3D12_FLOAT32_MAX,
                                         .ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL};
    D3D12_ROOT_SIGNATURE_DESC signature = {
        .NumParameters = 1, .pParameters = &table, .NumStaticSamplers = 1, .pStaticSamplers = &sampler};
    ID3DBlob *blob = NULL;
    HRESULT hr = D3D12SerializeRootSignature(&signature, D3D_ROOT_SIGNATURE_VERSION_1, &blob, NULL);
    if (FAILED(hr))
        return hr;
    hr = ID3D12Device_CreateRootSignature(gpu.device, 0, blob->lpVtbl->GetBufferPointer(blob),
                                          blob->lpVtbl->GetBufferSize(blob), &IID_ID3D12RootSignature,
                                          (void **)&gpu.root);
    RELEASE(blob);
    if (FAILED(hr))
        return hr;
    D3D12_GRAPHICS_PIPELINE_STATE_DESC p = {0};
    p.pRootSignature = gpu.root;
    p.VS = (D3D12_SHADER_BYTECODE){finder_vs, sizeof finder_vs};
    p.PS = (D3D12_SHADER_BYTECODE){finder_ps, sizeof finder_ps};
    p.BlendState.RenderTarget[0] =
        (D3D12_RENDER_TARGET_BLEND_DESC){.BlendEnable = TRUE,
                                         .SrcBlend = D3D12_BLEND_SRC_ALPHA,
                                         .DestBlend = D3D12_BLEND_INV_SRC_ALPHA,
                                         .BlendOp = D3D12_BLEND_OP_ADD,
                                         .SrcBlendAlpha = D3D12_BLEND_ONE,
                                         .DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA,
                                         .BlendOpAlpha = D3D12_BLEND_OP_ADD,
                                         .LogicOp = D3D12_LOGIC_OP_NOOP,
                                         .RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL};
    p.SampleMask = ~0u;
    p.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    p.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    p.RasterizerState.DepthClipEnable = TRUE;
    p.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    p.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
    p.DepthStencilState.FrontFace = (D3D12_DEPTH_STENCILOP_DESC){
        D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_KEEP, D3D12_COMPARISON_FUNC_ALWAYS};
    p.DepthStencilState.BackFace = p.DepthStencilState.FrontFace;
    p.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    p.NumRenderTargets = 1;
    p.RTVFormats[0] = gpu.format;
    p.SampleDesc.Count = 1;
    return ID3D12Device_CreateGraphicsPipelineState(gpu.device, &p, &IID_ID3D12PipelineState,
                                                    (void **)&gpu.pipeline);
}

static HRESULT initialize(ID3D12Device *device, DXGI_FORMAT format, UINT w, UINT h) {
    gpu.device = device;
    ID3D12Device_AddRef(device);
    gpu.format = format;
    gpu.width = w;
    gpu.height = h;
    gpu.pitch = (w * 4 + 255u) & ~255u;
    HRESULT hr = make_pipeline();
    if (FAILED(hr))
        return hr;
    hr = ID3D12Device_CreateFence(device, 0, D3D12_FENCE_FLAG_NONE, &IID_ID3D12Fence, (void **)&gpu.fence);
    if (FAILED(hr))
        return hr;
    D3D12_DESCRIPTOR_HEAP_DESC hd = {.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV, .NumDescriptors = FRAMES};
    hr = ID3D12Device_CreateDescriptorHeap(device, &hd, &IID_ID3D12DescriptorHeap, (void **)&gpu.rtvs);
    if (FAILED(hr))
        return hr;
    gpu.rtv_stride = ID3D12Device_GetDescriptorHandleIncrementSize(device, hd.Type);
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    hd.NumDescriptors = 1;
    hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    hr = ID3D12Device_CreateDescriptorHeap(device, &hd, &IID_ID3D12DescriptorHeap, (void **)&gpu.srv);
    if (FAILED(hr))
        return hr;
    D3D12_RESOURCE_DESC desc = {.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D,
                                .Width = w,
                                .Height = h,
                                .DepthOrArraySize = 1,
                                .MipLevels = 1,
                                .Format = DXGI_FORMAT_R8G8B8A8_UNORM,
                                .SampleDesc = {1, 0}};
    D3D12_HEAP_PROPERTIES heap = {
        .Type = D3D12_HEAP_TYPE_DEFAULT, .CreationNodeMask = 1, .VisibleNodeMask = 1};
    hr = ID3D12Device_CreateCommittedResource(device, &heap, D3D12_HEAP_FLAG_NONE, &desc,
                                              D3D12_RESOURCE_STATE_COPY_DEST, NULL, &IID_ID3D12Resource,
                                              (void **)&gpu.texture);
    if (FAILED(hr))
        return hr;
    D3D12_CPU_DESCRIPTOR_HANDLE cpu;
    gpu.srv->lpVtbl->GetCPUDescriptorHandleForHeapStart(gpu.srv, &cpu);
    ID3D12Device_CreateShaderResourceView(device, gpu.texture, NULL, cpu);
    heap.Type = D3D12_HEAP_TYPE_UPLOAD;
    desc = (D3D12_RESOURCE_DESC){.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER,
                                 .Width = (UINT64)gpu.pitch * h,
                                 .Height = 1,
                                 .DepthOrArraySize = 1,
                                 .MipLevels = 1,
                                 .Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR,
                                 .SampleDesc = {1, 0}};
    for (int i = 0; i < FRAMES; ++i) {
        Frame *f = &gpu.frame[i];
        hr = ID3D12Device_CreateCommandAllocator(device, D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                 &IID_ID3D12CommandAllocator, (void **)&f->allocator);
        if (FAILED(hr))
            return hr;
        hr = ID3D12Device_CreateCommandList(device, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, f->allocator, NULL,
                                            &IID_ID3D12GraphicsCommandList, (void **)&f->list);
        if (FAILED(hr))
            return hr;
        hr = ID3D12GraphicsCommandList_Close(f->list);
        if (FAILED(hr))
            return hr;
        hr = ID3D12Device_CreateCommittedResource(device, &heap, D3D12_HEAP_FLAG_NONE, &desc,
                                                  D3D12_RESOURCE_STATE_GENERIC_READ, NULL,
                                                  &IID_ID3D12Resource, (void **)&f->upload);
        if (FAILED(hr))
            return hr;
        D3D12_RANGE read = {0, 0};
        hr = ID3D12Resource_Map(f->upload, 0, &read, (void **)&f->pixels);
        if (FAILED(hr))
            return hr;
    }
    return S_OK;
}

static void barrier(ID3D12GraphicsCommandList *list, ID3D12Resource *resource, D3D12_RESOURCE_STATES before,
                    D3D12_RESOURCE_STATES after) {
    D3D12_RESOURCE_BARRIER b = {.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION};
    b.Transition.pResource = resource;
    b.Transition.StateBefore = before;
    b.Transition.StateAfter = after;
    ID3D12GraphicsCommandList_ResourceBarrier(list, 1, &b);
}

static HRESULT draw(ID3D12CommandQueue *queue, ID3D12Resource *target, UINT width, UINT height) {
    UINT64 complete = ID3D12Fence_GetCompletedValue(gpu.fence);
    if (complete == UINT64_MAX)
        return DXGI_ERROR_DEVICE_REMOVED;
    UINT slot = 0;
    while (slot < FRAMES && gpu.frame[slot].last_use > complete)
        ++slot;
    if (slot == FRAMES)
        return S_FALSE;
    Frame *f = &gpu.frame[slot];
    unsigned version = gpu.generation;
    BOOL changed =
        studio_overlay_bitmap(gpu.width, gpu.height, width, height, &version, f->pixels, gpu.pitch);
    if (!gpu.generation && !changed)
        return S_FALSE;
    HRESULT hr = ID3D12CommandAllocator_Reset(f->allocator);
    if (FAILED(hr))
        return hr;
    ID3D12GraphicsCommandList *list = f->list;
    hr = ID3D12GraphicsCommandList_Reset(list, f->allocator, gpu.pipeline);
    if (FAILED(hr))
        return hr;
    if (changed) {
        if (gpu.generation)
            barrier(list, gpu.texture, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                    D3D12_RESOURCE_STATE_COPY_DEST);
        D3D12_TEXTURE_COPY_LOCATION src = {.pResource = f->upload,
                                           .Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
        src.PlacedFootprint.Footprint =
            (D3D12_SUBRESOURCE_FOOTPRINT){DXGI_FORMAT_R8G8B8A8_UNORM, gpu.width, gpu.height, 1, gpu.pitch};
        D3D12_TEXTURE_COPY_LOCATION dst = {.pResource = gpu.texture,
                                           .Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
        ID3D12GraphicsCommandList_CopyTextureRegion(list, &dst, 0, 0, 0, &src, NULL);
        barrier(list, gpu.texture, D3D12_RESOURCE_STATE_COPY_DEST,
                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    }
    D3D12_CPU_DESCRIPTOR_HANDLE rtv;
    gpu.rtvs->lpVtbl->GetCPUDescriptorHandleForHeapStart(gpu.rtvs, &rtv);
    rtv.ptr += slot * gpu.rtv_stride;
    ID3D12Device_CreateRenderTargetView(gpu.device, target, NULL, rtv);
    barrier(list, target, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
    ID3D12GraphicsCommandList_OMSetRenderTargets(list, 1, &rtv, FALSE, NULL);
    D3D12_VIEWPORT vp = {0, 0, (float)width, (float)height, 0, 1};
    D3D12_RECT scissor = {0, 0, (LONG)width, (LONG)height};
    ID3D12GraphicsCommandList_RSSetViewports(list, 1, &vp);
    ID3D12GraphicsCommandList_RSSetScissorRects(list, 1, &scissor);
    ID3D12GraphicsCommandList_SetGraphicsRootSignature(list, gpu.root);
    ID3D12GraphicsCommandList_SetDescriptorHeaps(list, 1, &gpu.srv);
    D3D12_GPU_DESCRIPTOR_HANDLE srv;
    gpu.srv->lpVtbl->GetGPUDescriptorHandleForHeapStart(gpu.srv, &srv);
    ID3D12GraphicsCommandList_SetGraphicsRootDescriptorTable(list, 0, srv);
    ID3D12GraphicsCommandList_IASetPrimitiveTopology(list, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D12GraphicsCommandList_DrawInstanced(list, 3, 1, 0, 0);
    barrier(list, target, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
    hr = ID3D12GraphicsCommandList_Close(list);
    if (FAILED(hr))
        return hr;
    ID3D12CommandList *submit = (ID3D12CommandList *)list;
    ID3D12CommandQueue_ExecuteCommandLists(queue, 1, &submit);
    f->last_use = ++gpu.submitted;
    hr = ID3D12CommandQueue_Signal(queue, gpu.fence, f->last_use);
    if (FAILED(hr)) {
        fault = TRUE;
        return hr;
    } /* Retain submitted resources. */
    gpu.generation = version;
    return S_OK;
}

void overlay_draw(ID3D12CommandQueue *queue, IDXGISwapChain3 *chain, void (*log)(const char *)) {
    if (fault || !chain || !studio_overlay_active() || GetTickCount64() < retry_at)
        return;
    ID3D12Resource *target = NULL;
    ID3D12Device *device = NULL;
    UINT index = IDXGISwapChain3_GetCurrentBackBufferIndex(chain);
    HRESULT hr = IDXGISwapChain3_GetBuffer(chain, index, &IID_ID3D12Resource, (void **)&target);
    if (FAILED(hr))
        goto done;
    D3D12_RESOURCE_DESC d;
    target->lpVtbl->GetDesc(target, &d);
    if (!d.Width || !d.Height || d.Width > 16384 || d.Height > 16384 || d.SampleDesc.Count != 1) {
        hr = E_INVALIDARG;
        goto done;
    }
    UINT width = (UINT)d.Width, height = d.Height;
    UINT w = width, h = height;
    if (w > 1920) {
        h = (h * 1920 + w / 2) / w;
        w = 1920;
    }
    if (h > 1080) {
        w = (w * 1080 + h / 2) / h;
        h = 1080;
    }
    if (!w || !h) {
        hr = E_INVALIDARG;
        goto done;
    }
    hr = ID3D12Resource_GetDevice(target, &IID_ID3D12Device, (void **)&device);
    if (FAILED(hr))
        goto done;
    if (gpu.device && (gpu.device != device || gpu.format != d.Format || gpu.source_width != width ||
                       gpu.source_height != height)) {
        if (gpu.fence && ID3D12Fence_GetCompletedValue(gpu.fence) < gpu.submitted)
            goto done;
        destroy();
    }
    if (!gpu.device) {
        hr = initialize(device, d.Format, w, h);
        if (FAILED(hr)) {
            destroy();
            goto done;
        }
        gpu.source_width = width;
        gpu.source_height = height;
    }
    hr = draw(queue, target, width, height);
    if (hr == S_OK && !online) {
        online = TRUE;
        studio_renderer_ready(TRUE);
        log("Fullscreen photo finder active.\n");
    }
done:
    RELEASE(device);
    RELEASE(target);
    if (FAILED(hr)) {
        if (online) {
            online = FALSE;
            studio_renderer_ready(FALSE);
        }
        if (!fault && gpu.fence && ID3D12Fence_GetCompletedValue(gpu.fence) >= gpu.submitted)
            destroy();
        retry_at = GetTickCount64() + 2000;
        char message[80];
        wsprintfA(message, "Fullscreen photo finder draw failed (HRESULT 0x%08lx).\n", (unsigned long)hr);
        log(message);
    }
}

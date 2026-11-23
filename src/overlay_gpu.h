#ifndef WITN_OVERLAY_GPU_H
#define WITN_OVERLAY_GPU_H
#include <d3d12.h>
#include <dxgi1_4.h>

/* Runs on the render thread after all capture copies and before Present. */
void overlay_draw(ID3D12CommandQueue *queue, IDXGISwapChain3 *chain, void (*log)(const char *));
#endif

/* ------------------------------------------------------------------ *
 *  hwdetect.c -- enumerate the machine's graphics adapters via DXGI so
 *  the Encoder settings can offer only the engines the hardware supports
 *  (CUDA on NVIDIA, Vulkan on any real GPU/iGPU, CPU always).
 * ------------------------------------------------------------------ */

#define COBJMACROS
#include "common.h"
#include <dxgi.h>

/* iGPUs expose little/no *dedicated* VRAM (they share system RAM); a
 * discrete card reports its own VRAM.  Half a gig is a safe divider. */
#define DISCRETE_VRAM_MIN  (512ull * 1024 * 1024)

void DetectHardware(HwInfo *out)
{
    ZeroMemory(out, sizeof *out);

    IDXGIFactory1 *factory = NULL;
    if (FAILED(CreateDXGIFactory1(&IID_IDXGIFactory1, (void **)&factory)) || !factory)
        return;

    IDXGIAdapter1 *ad = NULL;
    for (UINT i = 0;
         out->count < (int)(sizeof(out->gpus) / sizeof(out->gpus[0])) &&
         IDXGIFactory1_EnumAdapters1(factory, i, &ad) != DXGI_ERROR_NOT_FOUND;
         ++i)
    {
        DXGI_ADAPTER_DESC1 d;
        if (ad && SUCCEEDED(IDXGIAdapter1_GetDesc1(ad, &d))) {
            /* Skip the Microsoft Basic Render Driver / WARP software adapter. */
            if (!(d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) && d.VendorId != 0x1414) {
                GpuInfo *g = &out->gpus[out->count++];
                lstrcpynW(g->name, d.Description, 128);
                g->vram       = (ULONGLONG)d.DedicatedVideoMemory;
                g->isDiscrete = g->vram >= DISCRETE_VRAM_MIN;

                switch (d.VendorId) {
                    case 0x10DE: g->vendor = GPU_VENDOR_NVIDIA;
                                 out->hasNvidia = TRUE;            break;
                    case 0x1002: g->vendor = GPU_VENDOR_AMD;       break;
                    case 0x8086: g->vendor = GPU_VENDOR_INTEL;     break;
                    default:     g->vendor = GPU_VENDOR_OTHER;     break;
                }
                out->hasAnyGpu = TRUE;
            }
        }
        if (ad) { IDXGIAdapter1_Release(ad); ad = NULL; }
    }

    IDXGIFactory1_Release(factory);
}

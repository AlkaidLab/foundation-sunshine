/**
 * @file src/nvenc/win/impl/nvenc_d3d11_on_cuda.h
 * @brief Declarations for CUDA NVENC encoder with Direct3D11 input surfaces.
 */
#pragma once

#include "nvenc_d3d11_base.h"

#include <cstdint>

#ifdef NVENC_NAMESPACE
namespace NVENC_NAMESPACE {
#else
  #include <ffnvcodec/dynlink_cuda.h>
namespace nvenc {
#endif

  /**
   * @brief Function pointers from nvcuda.dll used by the CUDA interop path.
   */
  struct cuda_function_table {
    tcuInit *cuInit;
    tcuD3D11GetDevice *cuD3D11GetDevice;
    tcuCtxCreate_v2 *cuCtxCreate;
    tcuCtxDestroy_v2 *cuCtxDestroy;
    tcuCtxPushCurrent_v2 *cuCtxPushCurrent;
    tcuCtxPopCurrent_v2 *cuCtxPopCurrent;
    tcuMemAllocPitch_v2 *cuMemAllocPitch;
    tcuMemFree_v2 *cuMemFree;
    tcuGraphicsD3D11RegisterResource *cuGraphicsD3D11RegisterResource;
    tcuGraphicsUnregisterResource *cuGraphicsUnregisterResource;
    tcuGraphicsMapResources *cuGraphicsMapResources;
    tcuGraphicsUnmapResources *cuGraphicsUnmapResources;
    tcuGraphicsSubResourceGetMappedArray *cuGraphicsSubResourceGetMappedArray;
    tcuMemcpy2D_v2 *cuMemcpy2D;
#if NVENCAPI_MAJOR_VERSION * 100 + NVENCAPI_MINOR_VERSION >= 1301
    tcuArray3DCreate *cuArray3DCreate;
    tcuArrayDestroy *cuArrayDestroy;
    tcuArrayGetPlane *cuArrayGetPlane;
#endif
    shared_dll dll;
  };

  /**
   * @brief CUDA interop context shared by every 4:4:4 encoder on one adapter.
   *        Creating and destroying a CUDA context per encoder object exercised
   *        the driver lifecycle several times per probe/session start for no
   *        benefit, so contexts are cached per adapter for the lifetime of the
   *        process instead.
   */
  struct cuda_interop_context {
    ~cuda_interop_context();

    cuda_function_table functions = {};
    CUcontext context = nullptr;
  };

  /**
   * @brief Interop Direct3D11 on CUDA NVENC encoder.
   *        Input surface is Direct3D11, encoding is performed by CUDA.
   */
  class nvenc_d3d11_on_cuda final: public nvenc_d3d11_base {
  public:
    /**
     * @param d3d_device Direct3D11 device that will create input surface texture.
     *                   CUDA encoding device will be derived from it.
     */
    nvenc_d3d11_on_cuda(ID3D11Device *d3d_device, shared_dll dll);
    ~nvenc_d3d11_on_cuda();

    ID3D11Texture2D *
    get_input_texture() override;

  private:
    static constexpr std::uint32_t planar_yuv_plane_count = 3;
    static constexpr std::uint32_t planar_yuv_bytes_per_sample = 2;

    bool
    init_library() override;

    bool
    create_and_register_input_buffer() override;

    bool
    synchronize_input_buffer() override;

#if NVENCAPI_MAJOR_VERSION * 100 + NVENCAPI_MINOR_VERSION >= 1301
    bool
    create_and_register_cuda_array_input();

    void
    destroy_cuda_array_input();
#endif

    bool
    create_and_register_cuda_device_pointer_input();

    bool
    register_cuda_input(NV_ENC_INPUT_RESOURCE_TYPE resource_type, void *resource, std::uint32_t pitch);

    bool
    cuda_succeeded(CUresult result);

    bool
    cuda_failed(CUresult result);

    struct autopop_context {
      autopop_context(nvenc_d3d11_on_cuda &parent, CUcontext pushed_context):
          parent(parent),
          pushed_context(pushed_context) {
      }

      ~autopop_context();

      explicit
      operator bool() const {
        return pushed_context != nullptr;
      }

      nvenc_d3d11_on_cuda &parent;
      CUcontext pushed_context = nullptr;
    };

    autopop_context
    push_context();

    HMODULE dll = NULL;
    const ID3D11DevicePtr d3d_device;
    ID3D11Texture2DPtr d3d_input_texture;

    // Keeps the per-adapter shared CUDA context alive for as long as this
    // encoder object lives. `cuda_functions` is a copy of the holder's table;
    // its `shared_dll` member carries its own library reference.
    std::shared_ptr<cuda_interop_context> interop_context;
    cuda_function_table cuda_functions = {};

    CUresult last_cuda_error = CUDA_SUCCESS;
    CUcontext cuda_context = nullptr;
    CUgraphicsResource cuda_d3d_input_texture = nullptr;
    CUdeviceptr cuda_surface = 0;
    size_t cuda_surface_pitch = 0;
#if NVENCAPI_MAJOR_VERSION * 100 + NVENCAPI_MINOR_VERSION >= 1301
    CUarray cuda_array_surface = nullptr;
    CUarray cuda_array_planes[planar_yuv_plane_count] = {};
#endif
  };
}

/*
* This copyright notice applies to this header file only:
*
* Copyright (c) 2019-2023 NVIDIA Corporation
*
* Permission is hereby granted, free of charge, to any person
* obtaining a copy of this software and associated documentation
* files (the "Software"), to deal in the Software without
* restriction, including without limitation the rights to use,
* copy, modify, merge, publish, distribute, sublicense, and/or sell
* copies of the software, and to permit persons to whom the
* software is furnished to do so, subject to the following
* conditions:
*
* The above copyright notice and this permission notice shall be
* included in all copies or substantial portions of the Software.
*
* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
* EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES
* OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
* NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT
* HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
* WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
* FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
* OTHER DEALINGS IN THE SOFTWARE.
*/
/**
* \file nvOpticalFlowD3D11.h
*   NVIDIA GPUs - Turing and above contains a hardware-based optical flow engine
*   which provides fully-accelerated hardware-based optical flow and stereo estimation.
*   nvOpticalFlowD3D11.h provides D3D11 specific enums, structure definitions and function prototypes.
*  for Direct3D11 resource.
*/
#ifndef _NV_OPTICALFLOW_D3D11_H_
#define _NV_OPTICALFLOW_D3D11_H_
#include "nvOpticalFlowCommon.h"
#include <d3d11.h>

#if defined(__cplusplus)
extern "C"
{
#endif /* __cplusplus */
    /**
    * \brief Create an instance of NvOFHandle object.
    *
    * This function creates an instance of NvOFHandle object and returns status.
    * Client is expected to release NvOFHandle resource using Destroy function call.
    *
    * \param [in] pD3D11Device
    *   Client must provide set this to the ID3D11Device created by the client.
    * \param [in] pD3D11DeviceContext
    *   Client must provide ID3D11DeviceContext* for the ID3D11Device created by the client.
    * \param [out] NvOFHandle*
    *   Pointer of class ::NvOFHandle object.
    *
    * \return
    * ::NV_OF_SUCCESS \n
    * ::NV_OF_ERR_OUT_OF_MEMORY \n
    * ::NV_OF_ERR_INVALID_VERSION \n
    * ::NV_OF_ERR_UNSUPPORTED_PARAM \n
    * ::NV_OF_ERR_OF_NOT_AVAILABLE \n
    */
    typedef NV_OF_STATUS(NVOFAPI* PFNNVCREATEOPTICALFLOWD3D11) (ID3D11Device* const pD3D11Device, ID3D11DeviceContext* const pD3D11DeviceContext, NvOFHandle *hOFInstance);

    /**
    * \brief Populate the count of the DXGI_FORMATs supported.
    *
    * This function returns the count of the DXGI_FORMAT supported for the given NV_OF_BUFFER_USAGE
    * and the NV_OF_MODE. Client can use this count to allocate a sufficiently large array for 
    * querying the DXGI_FORMATS supported.
    *
    * \param [in] hOf
    *   Object of ::NvOFHandle type.
    * \param [in] bufUsage
    *  ::NV_OF_BUFFER_USAGE needs to be provided by the client.
    * \param [in] ofMode
    *  ::NV_OF_MODE needs to be provided by the client.
    * \param [out] pCount
    *  The count is returned by uint32_t* pCount
    *
    * \return
    * ::NV_OF_SUCCESS \n
    * ::NV_OF_ERR_INVALID_DEVICE \n
    * ::NV_OF_ERR_DEVICE_DOES_NOT_EXIST \n
    * ::NV_OF_ERR_UNSUPPORTED_PARAM \n
    * ::NV_OF_ERR_INVALID_PARAM \n
    * ::NV_OF_ERR_GENERIC \n
    */
    typedef NV_OF_STATUS(NVOFAPI* PFNNVOFGETSURFACEFORMATCOUNTD3D11) (NvOFHandle hOf, const NV_OF_BUFFER_USAGE bufUsage, const NV_OF_MODE ofMode, uint32_t* const pCount);

    /**
    * \brief Populate the array of the DXGI_FORMATs supported.
    *
    * This function returns the array of the DXGI_FORMATs supported for the given NV_OF_BUFFER_USAGE
    * and the NV_OF_MODE. Client must populate surface format count prior to this call.
    * Client must allocate sufficiently large array size for this function call.
    *
    * \param [in] hOf
    *   Object of ::NvOFHandle type.
    * \param [in] bufUsage
    *  ::NV_OF_BUFFER_USAGE needs to be provided by the client.
    * \param [in] ofMode
    *  ::NV_OF_MODE needs to be provided by the client.
    * \param [out] pFormat
    *  The list is returned through client allocated array of DXGI_FORMAT.
    *
    * \return
    * ::NV_OF_SUCCESS \n
    * ::NV_OF_ERR_INVALID_DEVICE \n
    * ::NV_OF_ERR_DEVICE_DOES_NOT_EXIST \n
    * ::NV_OF_ERR_UNSUPPORTED_PARAM \n
    * ::NV_OF_ERR_INVALID_PARAM \n
    * ::NV_OF_ERR_GENERIC \n
    */
    typedef NV_OF_STATUS(NVOFAPI* PFNNVOFGETSURFACEFORMATD3D11) (NvOFHandle hOf, const NV_OF_BUFFER_USAGE bufUsage, const NV_OF_MODE ofMode, DXGI_FORMAT* const pFormat);

    /**
    * \brief Register the app allocated ID3D11Resource with the NvOFAPI.
    *
    * This function registers the app allocated ID3D11Resource with the NvOFAPI and gets an opaque handle NvOFGPUBufferHandle
    * in return. The client should call ::NvOFUnregisterResourceD3D11 with NvOFGPUBufferHandle to unregister the resource.
    * Client need to register the ID3D11Resource with OFAPI to be able to use it in the ::NvOFExecute call.
    *
    * \param [in] hOf
    *   Object of ::NvOFHandle type.
    * \param [in] pResource
    *  ID3D11Resource* allocated by the client.
    * \param [out] hOFGpuBuffer
    *   Output pointer of ::NvOFGPUBufferHandle type.
    *
    * \return
    * ::NV_OF_SUCCESS \n
    * ::NV_OF_ERR_INVALID_DEVICE \n
    * ::NV_OF_ERR_DEVICE_DOES_NOT_EXIST \n
    * ::NV_OF_ERR_UNSUPPORTED_PARAM \n
    * ::NV_OF_ERR_INVALID_PARAM \n
    * ::NV_OF_ERR_OUT_OF_MEMORY \n
    * ::NV_OF_ERR_GENERIC \n
    */
    typedef NV_OF_STATUS(NVOFAPI* PFNNVOFREGISTERRESOURCED3D11) (NvOFHandle hOf, ID3D11Resource* pResource, NvOFGPUBufferHandle* const hOFGpuBuffer);

    /**
    * \brief Unregister the app allocated ID3D11Resource with the NvOFAPI.
    *
    * This function unregisters the app allocated ID3D11Resource which is already registered with the NvOFAPI.
    * Client needs to provied the NvOFGPUBufferHandle, which it has received in ::NvOFRegisterResourceD3D11 call.

    * \param in] hOFGpuBuffer
    *   Input pointer of ::NvOFGPUBufferHandle type.
    *
    * \return
    * ::NV_OF_SUCCESS \n
    * ::NV_OF_ERR_INVALID_DEVICE \n
    * ::NV_OF_ERR_DEVICE_DOES_NOT_EXIST \n
    * ::NV_OF_ERR_UNSUPPORTED_PARAM \n
    * ::NV_OF_ERR_INVALID_PARAM \n
    * ::NV_OF_ERR_OUT_OF_MEMORY \n
    * ::NV_OF_ERR_GENERIC \n
    */
    typedef NV_OF_STATUS(NVOFAPI* PFNNVOFUNREGISTERRESOURCED3D11) (NvOFGPUBufferHandle hOFGpuBuffer);

    /*
    * Defining D3D11 specific API function pointers.
    */
    typedef struct _NV_OF_D3D11_API_FUNCTION_LIST
    {
        PFNNVCREATEOPTICALFLOWD3D11                                nvCreateOpticalFlowD3D11;
        PFNNVOFINIT                                                nvOFInit;
        PFNNVOFGETSURFACEFORMATCOUNTD3D11                          nvOFGetSurfaceFormatCountD3D11;
        PFNNVOFGETSURFACEFORMATD3D11                               nvOFGetSurfaceFormatD3D11;
        PFNNVOFREGISTERRESOURCED3D11                               nvOFRegisterResourceD3D11;
        PFNNVOFUNREGISTERRESOURCED3D11                             nvOFUnregisterResourceD3D11;
        PFNNVOFEXECUTE                                             nvOFExecute;
        PFNNVOFDESTROY                                             nvOFDestroy;
        PFNNVOFGETLASTERROR                                        nvOFGetLastError;
        PFNNVOFGETCAPS                                             nvOFGetCaps;
    } NV_OF_D3D11_API_FUNCTION_LIST;


    /**
    * \brief ::NvOFAPICreateInstanceD3D11() API is the entry point to the NvOFAPI interface.
    *
    * ::NvOFAPICreateInstanceD3D11() API populates functionList with function pointers to the API routines implemented by the
    * NvOFAPI interface.
    *
    * \return
    * ::NV_OF_SUCCESS \n
    * ::NV_OF_ERR_INVALID_VERSION \n
    * ::NV_OF_ERR_INVALID_PTR \n
    */
    NV_OF_STATUS NVOFAPI NvOFAPICreateInstanceD3D11(uint32_t apiVer, NV_OF_D3D11_API_FUNCTION_LIST  *functionList);

#if defined(__cplusplus)
}
#endif /* __cplusplus */

#endif

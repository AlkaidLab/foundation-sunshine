/**
 * @file src/nvenc/win/impl/nvenc_dynamic_factory_blueprint.h
 * @brief Special blueprint used for declaring and defining factories for specific NVENC SDK versions.
 */
#include <boost/preprocessor/cat.hpp>

#ifndef NVENC_FACTORY_VERSION
  #error Missing NVENC_FACTORY_VERSION preprocessor definition
#endif

#define NVENC_FACTORY_CLASS BOOST_PP_CAT(nvenc_dynamic_factory_, NVENC_FACTORY_VERSION)

#include "nvenc_shared_dll.h"

#include "../nvenc_dynamic_factory.h"

namespace nvenc {

  class NVENC_FACTORY_CLASS: public nvenc_dynamic_factory {
  public:
    NVENC_FACTORY_CLASS(shared_dll dll):
        dll(dll) {}

    static std::shared_ptr<nvenc_dynamic_factory>
    get(shared_dll dll) {
      return std::make_shared<NVENC_FACTORY_CLASS>(dll);
    }

    /// SDK version (major*100 + minor) this factory was compiled against.
    /// Defined in the corresponding _XXXX.cpp where the SDK headers are included,
    /// so the value is always derived from the actual SDK headers in use.
    static const int sdk_version;

    std::unique_ptr<nvenc_d3d11>
    create_nvenc_d3d11_native(ID3D11Device *d3d_device) override;

    std::unique_ptr<nvenc_d3d11>
    create_nvenc_d3d11_on_cuda(ID3D11Device *d3d_device) override;

  private:
    shared_dll dll;
  };

}  // namespace nvenc

#ifdef NVENC_FACTORY_DEFINITION

  #define NVENC_NAMESPACE BOOST_PP_CAT(nvenc_, NVENC_FACTORY_VERSION)
  #define NVENC_FACTORY_INCLUDE(x) <NVENC_FACTORY_VERSION/include/ffnvcodec/x>

namespace NVENC_NAMESPACE {
  #include NVENC_FACTORY_INCLUDE(dynlink_cuda.h)
  #include NVENC_FACTORY_INCLUDE(nvEncodeAPI.h)
}  // namespace NVENC_NAMESPACE

// Derive the SDK version automatically from the headers we just included,
// so the priority key in nvenc_dynamic_factory.cpp can never go out of sync
// with the actual SDK this factory is built against (e.g. when the `1202`
// submodule tracks master and bumps from SDK 12.2 to 13.0 to 14.0...).
namespace nvenc {
  const int NVENC_FACTORY_CLASS::sdk_version =
    NVENCAPI_MAJOR_VERSION * 100 + NVENCAPI_MINOR_VERSION;
}

using namespace nvenc;

  #include "../../common_impl/nvenc_base.cpp"
  #include "../../common_impl/nvenc_utils.cpp"
  #include "nvenc_d3d11_base.cpp"
  #include "nvenc_d3d11_native.cpp"
  #include "nvenc_d3d11_on_cuda.cpp"

namespace nvenc {

  std::unique_ptr<nvenc_d3d11>
  NVENC_FACTORY_CLASS::create_nvenc_d3d11_native(ID3D11Device *d3d_device) {
    return std::make_unique<NVENC_NAMESPACE::nvenc_d3d11_native>(d3d_device, dll);
  }

  std::unique_ptr<nvenc_d3d11>
  NVENC_FACTORY_CLASS::create_nvenc_d3d11_on_cuda(ID3D11Device *d3d_device) {
    return std::make_unique<NVENC_NAMESPACE::nvenc_d3d11_on_cuda>(d3d_device, dll);
  }

}  // namespace nvenc

#endif

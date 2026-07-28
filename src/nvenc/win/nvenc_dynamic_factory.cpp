/**
 * @file src/nvenc/win/nvenc_dynamic_factory.cpp
 * @brief Definitions for Windows NVENC encoder factory.
 */
#include "nvenc_dynamic_factory.h"

#include "impl/nvenc_dynamic_factory_1100.h"
#include "impl/nvenc_dynamic_factory_1200.h"
#include "impl/nvenc_dynamic_factory_1300.h"
#include "impl/nvenc_dynamic_factory_1301.h"

#include "src/logging.h"

#include <windows.h>

#include <array>
#include <bit>
#include <cstdint>
#include <tuple>

namespace {
  using namespace nvenc;

  const std::array factory_priorities = {
#define SUNSHINE_NVENC_SDK(version, name) \
  std::tuple(&nvenc_dynamic_factory_##version::get, nvenc_sdk_version::sdk_##name),
#include "../nvenc_sdk_versions.def"
#undef SUNSHINE_NVENC_SDK
  };
  constexpr auto min_driver_version = "456.71";
  using get_max_supported_version_fn = std::uint32_t(WINAPI *)(std::uint32_t *);

#ifdef _WIN64
  constexpr auto dll_name = "nvEncodeAPI64.dll";
#else
  constexpr auto dll_name = "nvEncodeAPI.dll";
#endif

  std::tuple<shared_dll, uint32_t>
  load_dll(const nvenc_runtime_api &runtime_api) {
    auto dll = runtime_api.load_driver();
    if (!dll) {
      BOOST_LOG(debug) << "NvEnc: Couldn't load NvEnc library " << dll_name;
      return {};
    }

    auto get_max_version = std::bit_cast<get_max_supported_version_fn>(
      runtime_api.get_symbol(dll.get(), "NvEncodeAPIGetMaxSupportedVersion"));
    if (!get_max_version) {
      BOOST_LOG(error) << "NvEnc: No NvEncodeAPIGetMaxSupportedVersion() in " << dll_name;
      return {};
    }

    uint32_t max_version = 0;
    if (get_max_version(&max_version) != 0) {
      BOOST_LOG(error) << "NvEnc: NvEncodeAPIGetMaxSupportedVersion() failed";
      return {};
    }
    max_version = decode_nvenc_driver_version(max_version);

    return { dll, max_version };
  }

}  // namespace

namespace nvenc {

  std::shared_ptr<nvenc_dynamic_factory>
  nvenc_dynamic_factory::get() {
    return get({
      []() {
        return make_shared_dll(LoadLibraryEx(dll_name, nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32));
      },
      [](HMODULE dll, const char *symbol) {
        return GetProcAddress(dll, symbol);
      },
    });
  }

  std::shared_ptr<nvenc_dynamic_factory>
  nvenc_dynamic_factory::get(const nvenc_runtime_api &runtime_api) {
    auto [dll, max_version] = load_dll(runtime_api);
    if (!dll) return {};

    const auto selected_version = select_nvenc_sdk_version(max_version);
    for (const auto &[factory_init, version] : factory_priorities) {
      if (version == selected_version) {
        BOOST_LOG(info) << "NvEnc: driver supports API "
                        << max_version / 100 << '.' << max_version % 100
                        << ", selecting SDK "
                        << nvenc_sdk_version_number(version) / 100 << '.'
                        << nvenc_sdk_version_number(version) % 100;
        return factory_init(dll);
      }
    }

    BOOST_LOG(error) << "NvEnc: driver API " << max_version / 100 << '.' << max_version % 100
                     << " is unsupported; minimum required driver version is " << min_driver_version;
    return {};
  }

}  // namespace nvenc

#ifdef SUNSHINE_TESTS
  #include "tests/tests_common.h"

  #include <comdef.h>
  #include <d3d11.h>

namespace {
  _COM_SMARTPTR_TYPEDEF(IDXGIFactory1, IID_IDXGIFactory1);
  _COM_SMARTPTR_TYPEDEF(IDXGIAdapter, IID_IDXGIAdapter);
  _COM_SMARTPTR_TYPEDEF(ID3D11Device, IID_ID3D11Device);
}  // namespace

struct NvencVersionTests: testing::TestWithParam<decltype(factory_priorities)::value_type> {
  static void
  SetUpTestSuite() {
    nvenc_runtime_api runtime_api {
      []() {
        return make_shared_dll(LoadLibraryEx(dll_name, nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32));
      },
      [](HMODULE dll, const char *symbol) {
        return GetProcAddress(dll, symbol);
      },
    };
    std::tie(suite.dll, suite.max_version) = load_dll(runtime_api);
    if (!suite.dll) {
      GTEST_SKIP() << "Can't load " << dll_name;
    }

    IDXGIFactory1Ptr dxgi_factory;
    ASSERT_HRESULT_SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&dxgi_factory)));

    IDXGIAdapterPtr dxgi_adapter;
    for (UINT i = 0; dxgi_factory->EnumAdapters(i, &dxgi_adapter) != DXGI_ERROR_NOT_FOUND; i++) {
      DXGI_ADAPTER_DESC desc;
      ASSERT_HRESULT_SUCCEEDED(dxgi_adapter->GetDesc(&desc));
      if (desc.VendorId == 0x10de) break;
    }
    if (!dxgi_adapter) GTEST_SKIP();

    ASSERT_HRESULT_SUCCEEDED(D3D11CreateDevice(dxgi_adapter, D3D_DRIVER_TYPE_UNKNOWN, NULL, 0,
      nullptr, 0, D3D11_SDK_VERSION, &suite.device, nullptr, nullptr));
  }

  static void
  TearDownTestSuite() {
    suite = {};
  }

  inline static struct {
    nvenc::shared_dll dll;
    uint32_t max_version;
    ID3D11DevicePtr device;
  } suite = {};
};

TEST_P(NvencVersionTests, CreateAndEncode) {
  auto [factory_init, version] = GetParam();
  if (nvenc_sdk_version_number(version) > suite.max_version) {
    GTEST_SKIP() << "Need dll version " << nvenc_sdk_version_number(version) << ", have " << suite.max_version;
  }

  auto factory = factory_init(suite.dll);
  ASSERT_TRUE(factory);

  auto nvenc = factory->create_nvenc_d3d11_native(suite.device);
  ASSERT_TRUE(nvenc);

  video::config_t config = {
    .width = 1920,
    .height = 1080,
    .framerate = 60,
    .bitrate = 10 * 1000,
  };
  video::sunshine_colorspace_t colorspace = {
    .colorspace = video::colorspace_e::rec601,
    .bit_depth = 8,
  };
  ASSERT_TRUE(nvenc->create_encoder({}, config, colorspace, platf::pix_fmt_e::nv12));
  ASSERT_FALSE(nvenc->encode_frame(0, false).data.empty());
}

INSTANTIATE_TEST_SUITE_P(NvencFactoryTestsPrivate, NvencVersionTests, testing::ValuesIn(factory_priorities),
  [](const auto &info) { return std::to_string(nvenc_sdk_version_number(std::get<1>(info.param))); });

#endif

/**
 * @file src/platform/windows/display_vram_capture.cpp
 * @brief D3D11 VRAM capture lifecycle, images and cursor composition.
 */
#include "display_vram_internal.h"
#include "display_vram_shaders.h"
#include "display_cursor.h"
#include "misc.h"
#include "src/config.h"
#include "src/logging.h"
#include "src/video.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <AMF/components/DisplayCapture.h>

namespace platf::dxgi {
  capture_e
  display_ddup_vram_t::snapshot(const pull_free_image_cb_t &pull_free_image_cb, std::shared_ptr<platf::img_t> &img_out, std::chrono::milliseconds timeout, bool cursor_visible) {
    HRESULT status;
    DXGI_OUTDUPL_FRAME_INFO frame_info;

    const bool use_local_cursor = sync_local_cursor_mode(dup);

    resource_t::pointer res_p {};
    auto capture_status = dup.next_frame(frame_info, timeout, &res_p);
    resource_t res { res_p };

    if (capture_status != capture_e::ok) {
      return capture_status;
    }

    const bool mouse_update_flag = frame_info.LastMouseUpdateTime.QuadPart != 0 || frame_info.PointerShapeBufferSize > 0;
    const bool frame_update_flag = frame_info.LastPresentTime.QuadPart != 0;
    const bool update_flag = mouse_update_flag || frame_update_flag;

    if (!update_flag) {
      return capture_e::timeout;
    }

    std::optional<std::chrono::steady_clock::time_point> frame_timestamp;
    if (auto qpc_displayed = std::max(frame_info.LastPresentTime.QuadPart, frame_info.LastMouseUpdateTime.QuadPart)) {
      // Translate QueryPerformanceCounter() value to steady_clock time point
      frame_timestamp = std::chrono::steady_clock::now() - qpc_time_difference(qpc_counter(), qpc_displayed);
    }

    bool shape_updated;
    if (dup.update_cursor(frame_info, shape_updated) != capture_e::ok) {
      return capture_e::error;
    }
    auto &cursor = dup.cursor;
    if (use_local_cursor) {
      publish_local_cursor(cursor, shape_updated);
    }

    if (shape_updated) {
      normalized_cursor_shape_t normalized;
      if (!normalize_cursor_shape(
            cursor.img_data,
            cursor.shape_info,
            true,
            normalized
          )) {
        return capture_e::error;
      }

      if (!set_cursor_texture(device.get(), cursor_alpha, std::move(normalized.alpha), normalized.info) ||
          !set_cursor_texture(device.get(), cursor_xor, std::move(normalized.xor_mask), normalized.info)) {
        return capture_e::error;
      }
    }

    if (frame_info.LastMouseUpdateTime.QuadPart) {
      cursor_alpha.set_pos(cursor.x, cursor.y,
        width, height, display_rotation, cursor.visible);

      cursor_xor.set_pos(cursor.x, cursor.y,
        width, height, display_rotation, cursor.visible);
    }

    const bool blend_mouse_cursor_flag =
      !use_local_cursor &&
      (cursor_alpha.visible || cursor_xor.visible) &&
      cursor_visible;

    texture2d_t src {};
    if (frame_update_flag) {
      // Get the texture object from this frame
      status = res->QueryInterface(IID_ID3D11Texture2D, (void **) &src);
      if (FAILED(status)) {
        BOOST_LOG(error) << "Couldn't query interface [0x"sv << util::hex(status).to_string_view() << ']';
        return capture_e::error;
      }

      D3D11_TEXTURE2D_DESC desc;
      src->GetDesc(&desc);

      // It's possible for our display enumeration to race with mode changes and result in
      // mismatched image pool and desktop texture sizes. If this happens, just reinit again.
      if (desc.Width != width_before_rotation || desc.Height != height_before_rotation) {
        BOOST_LOG(info) << "Capture size changed ["sv << width << 'x' << height << " -> "sv << desc.Width << 'x' << desc.Height << ']';
        return capture_e::reinit;
      }

      // If we don't know the capture format yet, grab it from this texture
      if (capture_format == DXGI_FORMAT_UNKNOWN) {
        capture_format = desc.Format;
        BOOST_LOG(info) << "Capture format ["sv << dxgi_format_to_string(capture_format) << ']';
      }

      // It's also possible for the capture format to change on the fly. If that happens,
      // reinitialize capture to try format detection again and create new images.
      if (capture_format != desc.Format) {
        BOOST_LOG(info) << "Capture format changed ["sv << dxgi_format_to_string(capture_format) << " -> "sv << dxgi_format_to_string(desc.Format) << ']';
        return capture_e::reinit;
      }
    }

    enum class lfa {
      nothing,
      replace_surface_with_img,
      replace_img_with_surface,
      copy_src_to_img,
      copy_src_to_surface,
    };

    enum class ofa {
      forward_last_img,
      copy_last_surface_and_blend_cursor,
      dummy_fallback,
    };

    auto last_frame_action = lfa::nothing;
    auto out_frame_action = ofa::dummy_fallback;

    if (capture_format == DXGI_FORMAT_UNKNOWN) {
      // We don't know the final capture format yet, so we will encode a black dummy image
      last_frame_action = lfa::nothing;
      out_frame_action = ofa::dummy_fallback;
    }
    else {
      if (src) {
        // We got a new frame from DesktopDuplication...
        if (blend_mouse_cursor_flag) {
          // ...and we need to blend the mouse cursor onto it.
          // Copy the frame to intermediate surface so we can blend this and future mouse cursor updates
          // without new frames from DesktopDuplication. We use direct3d surface directly here and not
          // an image from pull_free_image_cb mainly because it's lighter (surface sharing between
          // direct3d devices produce significant memory overhead).
          //
          // The intermediate surface must hold a *cursor-free* copy of the desktop frame: every output
          // image is built as "clean frame copy + this frame's cursor". Blending directly into the image
          // saved in last_frame_variant would bake the cursor into it and leave a trail behind the cursor
          // on subsequent cursor-only updates.
          last_frame_action = lfa::copy_src_to_surface;
          // Copy the intermediate surface to a new image from pull_free_image_cb and blend the mouse cursor onto it.
          out_frame_action = ofa::copy_last_surface_and_blend_cursor;
        }
        else {
          // ...and we don't need to blend the mouse cursor.
          // Copy the frame to a new image from pull_free_image_cb and save the shared pointer to the image
          // in case the mouse cursor appears without a new frame from DesktopDuplication.
          last_frame_action = lfa::copy_src_to_img;
          // Use saved last image shared pointer as output image evading copy.
          out_frame_action = ofa::forward_last_img;
        }
      }
      else if (!std::holds_alternative<std::monostate>(last_frame_variant)) {
        // We didn't get a new frame from DesktopDuplication...
        if (blend_mouse_cursor_flag) {
          // ...but we need to blend the mouse cursor.
          if (std::holds_alternative<std::shared_ptr<platf::img_t>>(last_frame_variant)) {
            // We have the shared pointer of the last image, replace it with intermediate surface
            // while copying contents so we can blend this and future mouse cursor updates.
            last_frame_action = lfa::replace_img_with_surface;
          }
          // Copy the intermediate surface which contains last DesktopDuplication frame
          // to a new image from pull_free_image_cb and blend the mouse cursor onto it.
          out_frame_action = ofa::copy_last_surface_and_blend_cursor;
        }
        else {
          // ...and we don't need to blend the mouse cursor.
          // This happens when the mouse cursor disappears from screen,
          // or there's mouse cursor on screen, but its drawing is disabled in sunshine.
          if (std::holds_alternative<texture2d_t>(last_frame_variant)) {
            // We have the intermediate surface that was used as the mouse cursor blending base.
            // Replace it with an image from pull_free_image_cb copying contents and freeing up the surface memory.
            // Save the shared pointer to the image in case the mouse cursor reappears.
            last_frame_action = lfa::replace_surface_with_img;
          }
          // Use saved last image shared pointer as output image evading copy.
          out_frame_action = ofa::forward_last_img;
        }
      }
    }

    auto create_surface = [&](texture2d_t &surface) -> bool {
      // Try to reuse the old surface if it hasn't been destroyed yet.
      if (old_surface_delayed_destruction) {
        surface.reset(old_surface_delayed_destruction.release());
        return true;
      }

      // Otherwise create a new surface.
      D3D11_TEXTURE2D_DESC t {};
      t.Width = width_before_rotation;
      t.Height = height_before_rotation;
      t.MipLevels = 1;
      t.ArraySize = 1;
      t.SampleDesc.Count = 1;
      t.Usage = D3D11_USAGE_DEFAULT;
      t.Format = capture_format;
      t.BindFlags = 0;
      status = device->CreateTexture2D(&t, nullptr, &surface);
      if (FAILED(status)) {
        BOOST_LOG(error) << "Failed to create frame copy texture [0x"sv << util::hex(status).to_string_view() << ']';
        return false;
      }

      return true;
    };

    auto get_locked_d3d_img = [&](std::shared_ptr<platf::img_t> &img, bool dummy = false) -> std::tuple<std::shared_ptr<img_d3d_t>, texture_lock_helper> {
      auto d3d_img = std::static_pointer_cast<img_d3d_t>(img);

      // Finish creating the image (if it hasn't happened already),
      // also creates synchronization primitives for shared access from multiple direct3d devices.
      if (complete_img(d3d_img.get(), dummy)) return { nullptr, nullptr };

      // This image is shared between capture direct3d device and encoders direct3d devices,
      // we must acquire lock before doing anything to it.
      texture_lock_helper lock_helper(d3d_img->capture_mutex.get());
      if (!lock_helper.lock()) {
        BOOST_LOG(error) << "Failed to lock capture texture";
        return { nullptr, nullptr };
      }

      // Clear the blank flag now that we're ready to capture into the image
      d3d_img->blank = false;

      return { std::move(d3d_img), std::move(lock_helper) };
    };

    switch (last_frame_action) {
      case lfa::nothing: {
        break;
      }

      case lfa::replace_surface_with_img: {
        auto p_surface = std::get_if<texture2d_t>(&last_frame_variant);
        if (!p_surface) {
          BOOST_LOG(error) << "Logical error at " << __FILE__ << ":" << __LINE__;
          return capture_e::error;
        }

        std::shared_ptr<platf::img_t> img;
        if (!pull_free_image_cb(img)) return capture_e::interrupted;

        auto [d3d_img, lock] = get_locked_d3d_img(img);
        if (!d3d_img) return capture_e::error;

        device_ctx->CopyResource(d3d_img->capture_texture.get(), p_surface->get());

        // We delay the destruction of intermediate surface in case the mouse cursor reappears shortly.
        old_surface_delayed_destruction.reset(p_surface->release());
        old_surface_timestamp = std::chrono::steady_clock::now();

        last_frame_variant = img;
        break;
      }

      case lfa::replace_img_with_surface: {
        auto p_img = std::get_if<std::shared_ptr<platf::img_t>>(&last_frame_variant);
        if (!p_img) {
          BOOST_LOG(error) << "Logical error at " << __FILE__ << ":" << __LINE__;
          return capture_e::error;
        }
        auto [d3d_img, lock] = get_locked_d3d_img(*p_img);
        if (!d3d_img) return capture_e::error;

        p_img = nullptr;
        last_frame_variant = texture2d_t {};
        auto &surface = std::get<texture2d_t>(last_frame_variant);
        if (!create_surface(surface)) return capture_e::error;

        device_ctx->CopyResource(surface.get(), d3d_img->capture_texture.get());
        break;
      }

      case lfa::copy_src_to_img: {
        last_frame_variant = {};

        std::shared_ptr<platf::img_t> img;
        if (!pull_free_image_cb(img)) return capture_e::interrupted;

        auto [d3d_img, lock] = get_locked_d3d_img(img);
        if (!d3d_img) return capture_e::error;

        device_ctx->CopyResource(d3d_img->capture_texture.get(), src.get());
        last_frame_variant = img;
        break;
      }

      case lfa::copy_src_to_surface: {
        auto p_surface = std::get_if<texture2d_t>(&last_frame_variant);
        if (!p_surface) {
          last_frame_variant = texture2d_t {};
          p_surface = std::get_if<texture2d_t>(&last_frame_variant);
          if (!create_surface(*p_surface)) return capture_e::error;
        }
        device_ctx->CopyResource(p_surface->get(), src.get());
        break;
      }
    }

    switch (out_frame_action) {
      case ofa::forward_last_img: {
        auto p_img = std::get_if<std::shared_ptr<platf::img_t>>(&last_frame_variant);
        if (!p_img) {
          BOOST_LOG(error) << "Logical error at " << __FILE__ << ":" << __LINE__;
          return capture_e::error;
        }
        img_out = *p_img;
        break;
      }

      case ofa::copy_last_surface_and_blend_cursor: {
        auto p_surface = std::get_if<texture2d_t>(&last_frame_variant);
        if (!p_surface) {
          BOOST_LOG(error) << "Logical error at " << __FILE__ << ":" << __LINE__;
          return capture_e::error;
        }
        if (!blend_mouse_cursor_flag) {
          BOOST_LOG(error) << "Logical error at " << __FILE__ << ":" << __LINE__;
          return capture_e::error;
        }

        if (!pull_free_image_cb(img_out)) return capture_e::interrupted;

        auto [d3d_img, lock] = get_locked_d3d_img(img_out);
        if (!d3d_img) return capture_e::error;

        device_ctx->CopyResource(d3d_img->capture_texture.get(), p_surface->get());
        blend_cursor(d3d_img->capture_rt.get());
        break;
      }

      case ofa::dummy_fallback: {
        if (!pull_free_image_cb(img_out)) return capture_e::interrupted;

        // Clear the image if it has been used as a dummy.
        // It can have the mouse cursor blended onto it.
        auto old_d3d_img = (img_d3d_t *) img_out.get();
        bool reclear_dummy = !old_d3d_img->blank && old_d3d_img->capture_texture;

        auto [d3d_img, lock] = get_locked_d3d_img(img_out, true);
        if (!d3d_img) return capture_e::error;

        if (reclear_dummy) {
          const float rgb_black[] = { 0.0f, 0.0f, 0.0f, 0.0f };
          device_ctx->ClearRenderTargetView(d3d_img->capture_rt.get(), rgb_black);
        }

        if (blend_mouse_cursor_flag) {
          blend_cursor(d3d_img->capture_rt.get());
        }

        break;
      }
    }

    // Perform delayed destruction of the unused surface if the time is due.
    if (old_surface_delayed_destruction && old_surface_timestamp + 10s < std::chrono::steady_clock::now()) {
      old_surface_delayed_destruction.reset();
    }

    if (img_out) {
      img_out->frame_timestamp = frame_timestamp;
    }

    return capture_e::ok;
  }

  capture_e
  display_ddup_vram_t::release_snapshot() {
    return dup.release_frame();
  }

  int
  display_vram_t::init_cursor_pipeline(const ::video::config_t &config) {
    cursor_pipeline_ready = false;
    cursor_white_normalization_enabled = false;
    cursor_white_multiplier.reset();
    cursor_white_multiplier_value = 300.0f / 80.0f;
    producer_sdr_white_nits = 0.0f;

    if (const auto windows_white = sdr_white_nits()) {
      cursor_white_multiplier_value = *windows_white / 80.0f;
      BOOST_LOG(info) << "Windows SDR reference white: " << *windows_white << " nits";
    }

    D3D11_SAMPLER_DESC sampler_desc {};
    sampler_desc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sampler_desc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler_desc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler_desc.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
    sampler_desc.ComparisonFunc = D3D11_COMPARISON_NEVER;
    sampler_desc.MinLOD = 0;
    sampler_desc.MaxLOD = D3D11_FLOAT32_MAX;

    auto status = device->CreateSamplerState(&sampler_desc, &sampler_linear);
    if (FAILED(status)) {
      BOOST_LOG(error) << "Failed to create linear sampler state [0x"sv << util::hex(status).to_string_view() << ']';
      return -1;
    }

    sampler_desc.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    status = device->CreateSamplerState(&sampler_desc, &sampler_point);
    if (FAILED(status)) {
      BOOST_LOG(error) << "Failed to create point sampler state [0x"sv << util::hex(status).to_string_view() << ']';
      return -1;
    }

    status = device->CreateVertexShader(cursor_vs_hlsl->GetBufferPointer(), cursor_vs_hlsl->GetBufferSize(), nullptr, &cursor_vs);
    if (status) {
      BOOST_LOG(error) << "Failed to create scene vertex shader [0x"sv << util::hex(status).to_string_view() << ']';
      return -1;
    }

    {
      int32_t rotation_modifier = display_rotation == DXGI_MODE_ROTATION_UNSPECIFIED ? 0 : display_rotation - 1;
      int32_t rotation_data[16 / sizeof(int32_t)] { rotation_modifier };  // aligned to 16-byte
      auto rotation = make_buffer(device.get(), rotation_data);
      if (!rotation) {
        BOOST_LOG(error) << "Failed to create display rotation vertex constant buffer";
        return -1;
      }
      device_ctx->VSSetConstantBuffers(2, 1, &rotation);
    }

    if (config.dynamicRange && is_hdr()) {
      // This shader will normalize scRGB white levels to a user-defined white level
      status = device->CreatePixelShader(cursor_ps_normalize_white_hlsl->GetBufferPointer(), cursor_ps_normalize_white_hlsl->GetBufferSize(), nullptr, &cursor_ps);
      if (status) {
        BOOST_LOG(error) << "Failed to create cursor blending (normalized white) pixel shader [0x"sv << util::hex(status).to_string_view() << ']';
        return -1;
      }

      // DisplayConfig supplied the initial physical-output value above. Keep the
      // established 300-nit fallback only when that query failed; VDD may replace
      // either value with fresher producer metadata.
      float white_multiplier_data[16 / sizeof(float)] { cursor_white_multiplier_value.load(std::memory_order_relaxed) };  // aligned to 16-byte
      cursor_white_multiplier = make_buffer(device.get(), white_multiplier_data);
      if (!cursor_white_multiplier) {
        BOOST_LOG(warning) << "Failed to create cursor blending (normalized white) white multiplier constant buffer";
        return -1;
      }
      cursor_white_normalization_enabled = true;
    }
    else {
      status = device->CreatePixelShader(cursor_ps_hlsl->GetBufferPointer(), cursor_ps_hlsl->GetBufferSize(), nullptr, &cursor_ps);
      if (status) {
        BOOST_LOG(error) << "Failed to create cursor blending pixel shader [0x"sv << util::hex(status).to_string_view() << ']';
        return -1;
      }
    }

    blend_alpha = make_blend(device.get(), true, false);
    blend_invert = make_blend(device.get(), true, true);
    blend_disable = make_blend(device.get(), false, false);

    if (!blend_disable || !blend_alpha || !blend_invert) {
      return -1;
    }

    device_ctx->OMSetBlendState(blend_disable.get(), nullptr, 0xFFFFFFFFu);
    ID3D11SamplerState *samplers[] = { sampler_linear.get(), sampler_point.get() };
    device_ctx->PSSetSamplers(0, 2, samplers);
    device_ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    cursor_pipeline_ready = true;
    return 0;
  }

  void
  display_vram_t::set_cursor_sdr_white_level(UINT32 sdr_white_level_x1000) {
    if (sdr_white_level_x1000 == 0) {
      return;
    }

    const float sdr_white_nits = static_cast<float>(sdr_white_level_x1000) / 1000.0f;
    if (!std::isfinite(sdr_white_nits) || sdr_white_nits < 1.0f || sdr_white_nits > 10000.0f) {
      return;
    }

    producer_sdr_white_nits.store(sdr_white_nits, std::memory_order_release);
    if (!cursor_white_normalization_enabled) {
      cursor_white_multiplier_value = sdr_white_nits / 80.0f;
      return;
    }

    const float next_multiplier = sdr_white_nits / 80.0f;
    if (std::abs(next_multiplier - cursor_white_multiplier_value.load(std::memory_order_relaxed)) < 0.0001f) {
      return;
    }

    float white_multiplier_data[16 / sizeof(float)] { next_multiplier };  // aligned to 16-byte
    auto next_buffer = make_buffer(device.get(), white_multiplier_data);
    if (!next_buffer) {
      BOOST_LOG(warning) << "Failed to update cursor SDR white-level multiplier; retaining previous value"sv;
      return;
    }

    cursor_white_multiplier = std::move(next_buffer);
    cursor_white_multiplier_value = next_multiplier;
  }

  std::optional<float>
  display_vram_t::capture_sdr_white_nits() const {
    const float producer_white = producer_sdr_white_nits.load(std::memory_order_acquire);
    if (producer_white > 0.0f) {
      return producer_white;
    }
    if (const auto windows_white = sdr_white_nits()) {
      return windows_white;
    }
    // Preserve the established fallback for outputs where neither DisplayConfig
    // nor a producer-side white-level report is available.
    return cursor_white_multiplier_value.load(std::memory_order_relaxed) * 80.0f;
  }

  void
  display_vram_t::blend_cursor(ID3D11RenderTargetView *capture_rt) {
    device_ctx->VSSetShader(cursor_vs.get(), nullptr, 0);
    device_ctx->PSSetShader(cursor_ps.get(), nullptr, 0);
    if (cursor_white_normalization_enabled && cursor_white_multiplier) {
      ID3D11Buffer *white_multiplier = cursor_white_multiplier.get();
      device_ctx->PSSetConstantBuffers(1, 1, &white_multiplier);
    }
    device_ctx->OMSetRenderTargets(1, &capture_rt, nullptr);

    if (cursor_alpha.texture.get()) {
      // Perform an alpha blending operation
      device_ctx->OMSetBlendState(blend_alpha.get(), nullptr, 0xFFFFFFFFu);

      device_ctx->PSSetShaderResources(0, 1, &cursor_alpha.input_res);
      device_ctx->RSSetViewports(1, &cursor_alpha.cursor_view);
      device_ctx->Draw(3, 0);
    }

    if (cursor_xor.texture.get()) {
      // Perform an invert blending without touching alpha values
      device_ctx->OMSetBlendState(blend_invert.get(), nullptr, 0x00FFFFFFu);

      device_ctx->PSSetShaderResources(0, 1, &cursor_xor.input_res);
      device_ctx->RSSetViewports(1, &cursor_xor.cursor_view);
      device_ctx->Draw(3, 0);
    }

    device_ctx->OMSetBlendState(blend_disable.get(), nullptr, 0xFFFFFFFFu);

    ID3D11RenderTargetView *emptyRenderTarget = nullptr;
    device_ctx->OMSetRenderTargets(1, &emptyRenderTarget, nullptr);
    device_ctx->RSSetViewports(0, nullptr);
    ID3D11ShaderResourceView *emptyShaderResourceView = nullptr;
    device_ctx->PSSetShaderResources(0, 1, &emptyShaderResourceView);
  }

  int
  display_ddup_vram_t::init(const ::video::config_t &config, const std::string &display_name) {
    if (display_base_t::init(config, display_name) || dup.init(this, config)) {
      return -1;
    }

    if (init_cursor_pipeline(config) != 0) {
      return -1;
    }

    return 0;
  }

  int
  display_amd_vram_t::init(const ::video::config_t &config, const std::string &display_name) {
    if (display_base_t::init(config, display_name) || dup.init(this, config, output_index)) {
      BOOST_LOG(error) << "AMD VRAM() failed";
      return -1;
    }

    auto status = device->CreateVertexShader(simple_cursor_vs_hlsl->GetBufferPointer(), simple_cursor_vs_hlsl->GetBufferSize(), nullptr, &cursor_vs);
    if (status) {
      BOOST_LOG(error) << "Failed to create simple cursor vertex shader [0x"sv << util::hex(status).to_string_view() << ']';
      return -1;
    }
    status = device->CreatePixelShader(simple_cursor_ps_hlsl->GetBufferPointer(), simple_cursor_ps_hlsl->GetBufferSize(), nullptr, &cursor_ps);
    if (status) {
      BOOST_LOG(error) << "Failed to create simple cursor pixel shader [0x"sv << util::hex(status).to_string_view() << ']';
      return -1;
    }

    blend_invert = make_blend(device.get(), true, true);
    blend_disable = make_blend(device.get(), false, false);

    if (!blend_disable || !blend_invert) {
      return -1;
    }

    D3D11_BUFFER_DESC buffer_desc {
      sizeof(float[16 / sizeof(float)]),
      D3D11_USAGE_DEFAULT,
      D3D11_BIND_CONSTANT_BUFFER,
      0
    };

    buf_t::pointer cursor_info_p;
    status = device->CreateBuffer(&buffer_desc, nullptr, &cursor_info_p);
    if (status) {
      BOOST_LOG(error) << "Failed to create cursor position buffer: [0x"sv << util::hex(status).to_string_view() << ']';
      return -1;
    }
    cursor_info = buf_t { cursor_info_p };

    return 0;
  }

  /**
   * @brief Get the next frame from the Windows.Graphics.Capture API and copy it into a new snapshot texture.
   * @param pull_free_image_cb call this to get a new free image from the video subsystem.
   * @param img_out the captured frame is returned here
   * @param timeout how long to wait for the next frame
   * @param cursor_visible whether to capture the cursor
   */
  capture_e
  display_amd_vram_t::snapshot(const pull_free_image_cb_t &pull_free_image_cb, std::shared_ptr<platf::img_t> &img_out, std::chrono::milliseconds timeout, bool cursor_visible) {
    amf::AMFSurfacePtr output;
    D3D11_TEXTURE2D_DESC desc;

    CURSORINFO pt;
    pt.cbSize = sizeof(CURSORINFO);

    // Check for display configuration change
    auto capture_status = dup.next_frame(timeout, (amf::AMFData **) &output);
    if (capture_status != capture_e::ok) {
      return capture_status;
    }
    dup.capturedSurface = output;

    texture2d_t src = (ID3D11Texture2D *) dup.capturedSurface->GetPlaneAt(0)->GetNative();
    src->GetDesc(&desc);

    // It's possible for our display enumeration to race with mode changes and result in
    // mismatched image pool and desktop texture sizes. If this happens, just reinit again.
    if (desc.Width != width_before_rotation || desc.Height != height_before_rotation) {
      BOOST_LOG(info) << "Capture size changed ["sv << width << 'x' << height << " -> "sv << desc.Width << 'x' << desc.Height << ']';
      return capture_e::reinit;
    }

    // If we don't know the capture format yet, grab it from this texture
    if (capture_format == DXGI_FORMAT_UNKNOWN) {
      capture_format = desc.Format;
      BOOST_LOG(info) << "AMD Capture format ["sv << dxgi_format_to_string(capture_format) << ']';
    }

    // It's also possible for the capture format to change on the fly. If that happens,
    // reinitialize capture to try format detection again and create new images.
    if (capture_format != desc.Format) {
      BOOST_LOG(info) << "AMD Capture format changed ["sv << dxgi_format_to_string(capture_format) << " -> "sv << dxgi_format_to_string(desc.Format) << ']';
      return capture_e::reinit;
    }

    std::shared_ptr<platf::img_t> img;
    if (!pull_free_image_cb(img))
      return capture_e::interrupted;

    auto blend_cursor = [&](img_d3d_t &d3d_img) {
      float new_cursor_data[16/ sizeof(float)] = { (float)pt.ptScreenPos.x, (float)pt.ptScreenPos.y, (float)width, (float)height };
      device_ctx->UpdateSubresource(cursor_info.get(), 0, nullptr, &new_cursor_data, 0, 0);

      device_ctx->VSSetConstantBuffers(0, 1, &cursor_info);
      device_ctx->VSSetShader(cursor_vs.get(), nullptr, 0);
      device_ctx->PSSetShader(cursor_ps.get(), nullptr, 0);
      device_ctx->OMSetRenderTargets(1, &d3d_img.capture_rt, nullptr);
      device_ctx->IASetInputLayout(nullptr);
      device_ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
      device_ctx->OMSetBlendState(blend_invert.get(), nullptr, 0x00FFFFFFu);

      device_ctx->Draw(3, 0);

      ID3D11RenderTargetView *emptyRenderTarget = nullptr;
      device_ctx->OMSetRenderTargets(1, &emptyRenderTarget, nullptr);
      device_ctx->RSSetViewports(0, nullptr);
      ID3D11ShaderResourceView *emptyShaderResourceView = nullptr;
      device_ctx->PSSetShaderResources(0, 1, &emptyShaderResourceView);
        device_ctx->OMSetBlendState(blend_disable.get(), nullptr, 0x00FFFFFFu);
    };

    auto d3d_img = std::static_pointer_cast<img_d3d_t>(img);
    d3d_img->blank = false;  // image is always ready for capture
    if (complete_img(d3d_img.get(), false) == 0) {
      texture_lock_helper lock_helper(d3d_img->capture_mutex.get());
      if (lock_helper.lock()) {
        device_ctx->CopyResource(d3d_img->capture_texture.get(), src.get());
        if (cursor_visible && config::input.amf_draw_mouse_cursor) {
          GetCursorInfo(&pt);
          if (pt.flags == CURSOR_SHOWING) {
            blend_cursor(*d3d_img);
          }
        }

      }
      else {
        return capture_e::error;
      }
    }
    else {
      return capture_e::error;
    }

    img_out = img;
    if (img_out) {
      img_out->frame_timestamp = std::chrono::steady_clock::now();
    }

    src.release();
    return capture_e::ok;
  }

  capture_e
  display_amd_vram_t::release_snapshot() {
    dup.release_frame();
    return capture_e::ok;
  }

  /**
   * Get the next frame from the Windows.Graphics.Capture API and copy it into a new snapshot texture.
   * @param pull_free_image_cb call this to get a new free image from the video subsystem.
   * @param img_out the captured frame is returned here
   * @param timeout how long to wait for the next frame
   * @param cursor_visible
   */
  capture_e
  display_wgc_vram_t::snapshot(const pull_free_image_cb_t &pull_free_image_cb, std::shared_ptr<platf::img_t> &img_out, std::chrono::milliseconds timeout, bool cursor_visible) {
    // Check if window is still valid (if capturing a window)
    // If window becomes invalid (closed, minimized, hidden), fall back to display capture
    if (!dup.is_window_valid()) {
      BOOST_LOG(warning) << "Captured window is no longer valid (closed, minimized, or hidden), falling back to display capture"sv;
      return capture_e::reinit;
    }

    texture2d_t src;
    uint64_t frame_qpc;
    dup.set_cursor_visible(cursor_visible);
    auto capture_status = dup.next_frame(timeout, &src, frame_qpc);
    if (capture_status != capture_e::ok) {
      // If we're capturing a window and getting timeouts/errors, check if window is still valid
      if (dup.captured_window_hwnd != nullptr) {
        // Simplified: Any error or timeout means window might have changed, check validity
        if (!dup.is_window_valid()) {
          BOOST_LOG(warning) << "Captured window is no longer valid, reinitializing capture"sv;
          return capture_e::reinit;
        }
      }
      return capture_status;
    }

    auto frame_timestamp = std::chrono::steady_clock::now() - qpc_time_difference(qpc_counter(), frame_qpc);
    D3D11_TEXTURE2D_DESC desc;
    src->GetDesc(&desc);

    // Get the actual captured frame dimensions
    int frame_width = static_cast<int>(desc.Width);
    int frame_height = static_cast<int>(desc.Height);

    // For window capture, check if size changed and handle it
    if (dup.captured_window_hwnd != nullptr) {
      int expected_width = dup.window_capture_width > 0 ? dup.window_capture_width : width_before_rotation;
      int expected_height = dup.window_capture_height > 0 ? dup.window_capture_height : height_before_rotation;

      if (frame_width != expected_width || frame_height != expected_height) {
        BOOST_LOG(info) << "Window capture size changed ["sv << expected_width << 'x' << expected_height
                         << " -> "sv << frame_width << 'x' << frame_height << ']';
        // Update stored dimensions
        dup.window_capture_width = frame_width;
        dup.window_capture_height = frame_height;
        // Trigger reinit to recreate all resources (images, textures, etc.) with new size
        return capture_e::reinit;
      }
    }
    else {
      // For display capture with WGC, the frame dimensions are in "display orientation"
      // (i.e., after rotation). Our `width`/`height` are derived from DesktopCoordinates
      // and match that orientation. Using width_before_rotation/height_before_rotation
      // here can cause an infinite reinit loop on rotation.
      if (frame_width != width || frame_height != height) {
        BOOST_LOG(info) << "Capture size changed ["sv << width << 'x' << height << " -> "sv << frame_width << 'x' << frame_height << ']';
        return capture_e::reinit;
      }
    }

    // It's also possible for the capture format to change on the fly. If that happens,
    // reinitialize capture to try format detection again and create new images.
    if (capture_format != desc.Format) {
      BOOST_LOG(info) << "Capture format changed ["sv << dxgi_format_to_string(capture_format) << " -> "sv << dxgi_format_to_string(desc.Format) << ']';
      return capture_e::reinit;
    }

    std::shared_ptr<platf::img_t> img;
    if (!pull_free_image_cb(img))
      return capture_e::interrupted;

    auto d3d_img = std::static_pointer_cast<img_d3d_t>(img);
    d3d_img->blank = false;  // image is always ready for capture
    if (complete_img(d3d_img.get(), false) == 0) {
      texture_lock_helper lock_helper(d3d_img->capture_mutex.get());
      if (lock_helper.lock()) {
        device_ctx->CopyResource(d3d_img->capture_texture.get(), src.get());
      }
      else {
        BOOST_LOG(error) << "Failed to lock capture texture";
        return capture_e::error;
      }
    }
    else {
      return capture_e::error;
    }
    img_out = img;
    if (img_out) {
      img_out->frame_timestamp = frame_timestamp;
    }

    return capture_e::ok;
  }

  capture_e
  display_wgc_vram_t::release_snapshot() {
    return dup.release_frame();
  }

  std::shared_ptr<platf::img_t>
  display_wgc_vram_t::alloc_img() {
    auto img = std::make_shared<img_d3d_t>();

    // For window capture, use window capture dimensions; for display capture, use display dimensions
    int img_width = dup.window_capture_width > 0 ? dup.window_capture_width : width;
    int img_height = dup.window_capture_height > 0 ? dup.window_capture_height : height;

    img->width = img_width;
    img->height = img_height;
    img->id = next_image_id++;
    img->blank = true;

    return img;
  }

  int
  display_wgc_vram_t::init(const ::video::config_t &config, const std::string &display_name) {
    if (display_base_t::init(config, display_name) || dup.init(this, config))
      return -1;

    // WGC frames are typically delivered in the current display orientation.
    // The DXGI rotation flag comes from the output descriptor and is needed for DDX,
    // but for WGC it can lead to applying rotation twice (client sees flipped/stretched).
    if (display_rotation != DXGI_MODE_ROTATION_UNSPECIFIED &&
        display_rotation != DXGI_MODE_ROTATION_IDENTITY) {
      BOOST_LOG(info) << "WGC: disabling DXGI rotation handling for oriented frames";
      display_rotation = DXGI_MODE_ROTATION_UNSPECIFIED;
      width_before_rotation = width;
      height_before_rotation = height;
    }

    return 0;
  }

  std::shared_ptr<platf::img_t>
  display_vram_t::alloc_img() {
    auto img = std::make_shared<img_d3d_t>();

    // Initialize format-independent fields
    img->width = width_before_rotation;
    img->height = height_before_rotation;
    img->id = next_image_id++;
    img->blank = true;

    return img;
  }

  // This cannot use ID3D11DeviceContext because it can be called concurrently by the encoding thread
  int
  display_vram_t::complete_img(platf::img_t *img_base, bool dummy) {
    auto img = (img_d3d_t *) img_base;

    // If this already has a capture texture and it's not switching dummy state, nothing to do
    if (!img->borrowed_vdd_texture && !img->borrowed_vdd_frame &&
        img->capture_texture && img->capture_rt && img->capture_mutex &&
        img->encoder_texture_handle && img->dummy == dummy) {
      return 0;
    }

    // If this is not a dummy image, we must know the format by now
    if (!dummy && capture_format == DXGI_FORMAT_UNKNOWN) {
      BOOST_LOG(error) << "display_vram_t::complete_img() called with unknown capture format!";
      return -1;
    }

    // Reset the image (in case this was previously a dummy or borrowed VDD slot)
    if (!img->abandon_borrowed_vdd_frame()) {
      return -1;
    }
    img->capture_texture.reset();
    img->capture_rt.reset();
    img->capture_mutex.reset();
    img->data = nullptr;
    if (img->encoder_texture_handle) {
      CloseHandle(img->encoder_texture_handle);
      img->encoder_texture_handle = NULL;
    }

    // Initialize format-dependent fields
    img->pixel_pitch = get_pixel_pitch();
    img->row_pitch = img->pixel_pitch * img->width;
    img->dummy = dummy;
    img->format = (capture_format == DXGI_FORMAT_UNKNOWN) ? DXGI_FORMAT_B8G8R8A8_UNORM : capture_format;
    img->linear_gamma = capture_linear_gamma;
    img->borrowed_vdd_texture = false;
    img->frame_desc = dummy ? captured_frame_desc_t {} : describe_captured_frame(img->format, false);

    D3D11_TEXTURE2D_DESC t {};
    t.Width = img->width;
    t.Height = img->height;
    t.MipLevels = 1;
    t.ArraySize = 1;
    t.SampleDesc.Count = 1;
    t.Usage = D3D11_USAGE_DEFAULT;
    t.Format = img->format;
    t.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    t.MiscFlags = D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;

    auto status = device->CreateTexture2D(&t, nullptr, &img->capture_texture);
    if (FAILED(status)) {
      BOOST_LOG(error) << "Failed to create img buf texture [0x"sv << util::hex(status).to_string_view() << ']';
      return -1;
    }

    status = device->CreateRenderTargetView(img->capture_texture.get(), nullptr, &img->capture_rt);
    if (FAILED(status)) {
      BOOST_LOG(error) << "Failed to create render target view [0x"sv << util::hex(status).to_string_view() << ']';
      return -1;
    }

    // Get the keyed mutex to synchronize with the encoding code
    status = img->capture_texture->QueryInterface(__uuidof(IDXGIKeyedMutex), (void **) &img->capture_mutex);
    if (FAILED(status)) {
      BOOST_LOG(error) << "Failed to query IDXGIKeyedMutex [0x"sv << util::hex(status).to_string_view() << ']';
      return -1;
    }

    resource1_t resource;
    status = img->capture_texture->QueryInterface(__uuidof(IDXGIResource1), (void **) &resource);
    if (FAILED(status)) {
      BOOST_LOG(error) << "Failed to query IDXGIResource1 [0x"sv << util::hex(status).to_string_view() << ']';
      return -1;
    }

    // Create a handle for the encoder device to use to open this texture
    status = resource->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ, nullptr, &img->encoder_texture_handle);
    if (FAILED(status)) {
      BOOST_LOG(error) << "Failed to create shared texture handle [0x"sv << util::hex(status).to_string_view() << ']';
      return -1;
    }

    img->data = (std::uint8_t *) img->capture_texture.get();

    return 0;
  }

  // This cannot use ID3D11DeviceContext because it can be called concurrently by the encoding thread
  /**
   * @memberof platf::dxgi::display_vram_t
   */
  int
  display_vram_t::dummy_img(platf::img_t *img_base) {
    return complete_img(img_base, true);
  }

  std::vector<DXGI_FORMAT>
  display_vram_t::get_supported_capture_formats() {
    return {
      // scRGB FP16 is the ideal format for Wide Color Gamut and Advanced Color
      // displays (both SDR and HDR). This format uses linear gamma, so we will
      // use a linear->PQ shader for HDR and a linear->sRGB shader for SDR.
      DXGI_FORMAT_R16G16B16A16_FLOAT,

      // DXGI_FORMAT_R10G10B10A2_UNORM seems like it might give us frames already
      // converted to SMPTE 2084 PQ, however it seems to actually just clamp the
      // scRGB FP16 values that DWM is using when the desktop format is scRGB FP16.
      //
      // If there is a case where the desktop format is really SMPTE 2084 PQ, it
      // might make sense to support capturing it without conversion to scRGB,
      // but we avoid it for now.

      // We include the 8-bit modes too for when the display is in SDR mode,
      // while the client stream is HDR-capable. These UNORM formats can
      // use our normal pixel shaders that expect sRGB input.
      DXGI_FORMAT_B8G8R8A8_UNORM,
      DXGI_FORMAT_B8G8R8X8_UNORM,
      DXGI_FORMAT_R8G8B8A8_UNORM,
    };
  }

}  // namespace platf::dxgi

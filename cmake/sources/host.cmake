# Explicit host inventory shared by the application and its test variant.
# Header entries retain IDE visibility; main.cpp belongs to the application.
set(SUNSHINE_HOST_SOURCES
    # third-party
    third-party/moonlight-common-c/src/RtspParser.c
    third-party/moonlight-common-c/src/EndpointRefreshProtocol.c
    third-party/moonlight-common-c/src/EndpointRefreshProtocol.h
    third-party/nanors/deps/obl/oblas_common.c
    third-party/nanors/deps/obl/oblas_lite.c
    third-party/nanors/rs.c
    third-party/moonlight-common-c/src/PyrowaveProtocol.c third-party/moonlight-common-c/src/PyrowaveProtocol.h
    third-party/moonlight-common-c/src/Input.h
    third-party/moonlight-common-c/src/Rtsp.h
    third-party/moonlight-common-c/src/Video.h
    third-party/tray/src/tray.h

    # src/haptics
    src/haptics/authored_ir.cpp src/haptics/authored_ir.h
    src/haptics/authored_input.h src/haptics/wire.h
    src/haptics/legacy_rumble.cpp
    src/haptics/authored_ir_extended.cpp src/haptics/authored_ir_extended.h

    # src/hdr
    src/hdr/client_display_capabilities.cpp src/hdr/client_display_capabilities.h
    src/hdr/calibration.cpp src/hdr/calibration.h
    src/hdr/dynamic_hdr_selection.cpp src/hdr/dynamic_hdr_selection.h
    src/hdr/session_target.cpp src/hdr/session_target.h

    # src/display_device
    src/display_device/color_profile.cpp src/display_device/color_profile.h
    src/display_device/parsed_config.cpp src/display_device/parsed_config.h
    src/display_device/session.cpp src/display_device/session.h
    src/display_device/settings.cpp src/display_device/settings.h
    src/display_device/to_string.cpp src/display_device/to_string.h
    src/display_device/vdd_capability.cpp src/display_device/vdd_capability.h
    src/display_device/vdd_utils.cpp src/display_device/vdd_utils.h
    src/display_device/vdd_ioctl.cpp src/display_device/vdd_ioctl.h
    src/display_device/vdd_mode_list.h
    src/display_device/vdd_settings.cpp src/display_device/vdd_settings.h
    src/display_device/display_device.h
    src/display_device/vdd_control_ioctl.h

    # src
    src/upnp.cpp src/upnp.h
    src/cbs.cpp
    src/client_family.cpp src/client_family.h
    src/client_fingerprint.cpp src/client_fingerprint.h
    src/config.cpp src/config.h
    src/entry_handler.cpp src/entry_handler.h
    src/endpoint_refresh.h
    src/file_handler.cpp src/file_handler.h
    src/globals.cpp src/globals.h
    src/logging.cpp src/logging.h
    src/nvhttp.cpp src/nvhttp.h
    src/nvhttp_stream_start.cpp src/nvhttp_stream_start.h
    src/httpcommon.cpp src/httpcommon.h
    src/http_json.cpp
    src/launch_session_manager.cpp src/launch_session_manager.h
    src/confighttp.cpp src/confighttp.h
    src/widget_http.cpp src/widget_http.h
    src/rtsp.cpp src/rtsp.h
    src/session_info.cpp src/session_info.h
    src/system_load.cpp src/system_load.h src/system_load_stub.cpp
    src/stream.cpp src/stream.h
    src/clipboard_bridge.cpp src/clipboard_bridge.h
    src/cursor_channel.cpp src/cursor_channel.h
    src/clipboard_blob_store.cpp src/clipboard_blob_store.h
    src/clipboard_http.cpp src/clipboard_http.h
    src/video.cpp src/video.h
    src/video_hdr_metadata.cpp src/video_hdr_metadata.h
    src/video_hdr_bitstream.cpp src/video_hdr_bitstream.h
    src/video_dolby_vision.cpp src/video_dolby_vision.h
    src/video_colorspace.cpp src/video_colorspace.h
    src/input.cpp src/input.h
    src/input_activity.cpp src/input_activity.h
    src/audio.cpp src/audio.h
    src/audio_devices.cpp src/audio_devices.h
    src/audio_vb_cable.cpp src/audio_vb_cable.h
    src/mic_mixer.cpp src/mic_mixer.h
    src/process.cpp src/process.h
    src/client_interface.cpp src/client_interface.h
    src/default_apps.cpp src/default_apps.h
    src/network.cpp src/network.h
    src/network_reachability.cpp src/network_reachability.h
    src/network_reachability_probe.cpp
    src/nat_discovery.cpp src/nat_discovery.h
    src/nat_discovery_probe.cpp
    src/public_address.cpp src/public_address.h
    src/system_info.cpp src/system_info.h
    src/build_info.cpp src/build_info.h
    src/system_info_cache.cpp
    src/system_info_probe.cpp
    src/stat_trackers.cpp src/stat_trackers.h
    src/utility.h
    src/uuid.h
    src/main.h
    src/crypto.h
    src/http_util.h
    src/move_by_copy.h
    src/task_pool.h
    src/thread_pool.h
    src/thread_safe.h
    src/sync.h
    src/round_robin.h

    # src/ds5
    src/ds5/config.cpp src/ds5/config.h
    src/ds5/config_api.cpp src/ds5/config_api.h

    # src/image_enhancement
    src/image_enhancement/config.cpp src/image_enhancement/config.h
    src/image_enhancement/nr_defaults.cpp src/image_enhancement/nr_defaults.h
    src/image_enhancement/api.cpp src/image_enhancement/api.h

    # src/pyrowave
    src/pyrowave/types.cpp src/pyrowave/types.h
    src/pyrowave/config.cpp src/pyrowave/config.h
    src/pyrowave/capabilities.cpp src/pyrowave/capabilities.h
    src/pyrowave/gpu_interop.cpp src/pyrowave/gpu_interop.h
    src/pyrowave/packet.cpp src/pyrowave/packet.h
    src/pyrowave/packetizer.cpp src/pyrowave/packetizer.h
    src/pyrowave/dynamic_hdr.cpp src/pyrowave/dynamic_hdr.h
    src/pyrowave/session.cpp src/pyrowave/session.h
    src/pyrowave/runtime.cpp src/pyrowave/runtime.h
    src/pyrowave/encoder.h
    src/pyrowave/pyrowave.h

    # src/file_mapping
    src/file_mapping/file_mapping.cpp src/file_mapping/file_mapping.h
    src/file_mapping/file_mapping_config.cpp src/file_mapping/file_mapping_config.h
    src/file_mapping/file_mapping_http.cpp src/file_mapping/file_mapping_http.h
    src/file_mapping/file_mapping_operations.cpp src/file_mapping/file_mapping_operations.h
    src/file_mapping/file_mapping_rpc.cpp src/file_mapping/file_mapping_rpc.h
    src/file_mapping/file_mapping_store.cpp src/file_mapping/file_mapping_store.h
    src/file_mapping/file_mapping_token.cpp src/file_mapping/file_mapping_token.h
    src/file_mapping/file_mapping_ws.cpp src/file_mapping/file_mapping_ws.h
    src/file_mapping/file_mapping_ws_session.cpp src/file_mapping/file_mapping_ws_session.h
    src/file_mapping/file_mapping_ws_server.cpp src/file_mapping/file_mapping_ws_server.h
    src/file_mapping/service.cpp src/file_mapping/service.h
    src/file_mapping/config_http.cpp src/file_mapping/config_http.h
    src/file_mapping/file_mapping_base64.h

    # src/remote_usb
    src/remote_usb/loopback_usbip_bridge.cpp src/remote_usb/loopback_usbip_bridge.h
    src/remote_usb/reverse_tunnel_service.cpp src/remote_usb/reverse_tunnel_service.h
    src/remote_usb/remote_usb_host_controller.cpp src/remote_usb/remote_usb_host_controller.h
    src/remote_usb/usbip_stale_port_sweep.cpp
    src/remote_usb/usbip_endpoint.h

    # src/virtual_usb
    src/virtual_usb/device.h
    src/virtual_usb/uac1_microphone.cpp src/virtual_usb/uac1_microphone.h

    # src/ai
    src/ai/credential_store.cpp src/ai/credential_store.h
    src/ai/http.cpp src/ai/http.h
    src/ai/config_store.cpp src/ai/config_store.h
    src/ai/proxy.cpp src/ai/proxy.h
    src/ai/http_stream.h
    src/ai/provider.h

    # src/webhook
    src/webhook/webhook_auth.cpp src/webhook/webhook_auth.h
    src/webhook/webhook_api.cpp src/webhook/webhook_api.h
    src/webhook/webhook_format.cpp src/webhook/webhook_format.h
    src/webhook/webhook.cpp src/webhook/webhook.h

    # src/nvhttp
    src/nvhttp/abr_api.cpp src/nvhttp/abr_api.h
    src/nvhttp/ai_api.cpp src/nvhttp/ai_api.h
    src/nvhttp/apps.cpp src/nvhttp/apps.h
    src/nvhttp/poster.cpp src/nvhttp/poster.h
    src/nvhttp/clipboard_api.cpp src/nvhttp/clipboard_api.h
    src/nvhttp/display_control.cpp src/nvhttp/display_control.h
    src/nvhttp/display_scale.cpp src/nvhttp/display_scale.h
    src/nvhttp/dynamic_params.cpp src/nvhttp/dynamic_params.h
    src/nvhttp/network_probe.cpp src/nvhttp/network_probe.h
    src/nvhttp/network_probe_limiter.cpp src/nvhttp/network_probe_limiter.h
    src/nvhttp/pairing.cpp src/nvhttp/pairing.h
    src/nvhttp/pairing_pending.cpp src/nvhttp/pairing_pending.h
    src/nvhttp/preset_pin.cpp
    src/nvhttp/sessions.cpp src/nvhttp/sessions.h

    # src/streaming
    src/streaming/abr.cpp src/streaming/abr.h
    src/streaming/bitrate.cpp src/streaming/bitrate.h
    src/streaming/fec.cpp src/streaming/fec.h
    src/streaming/perf_recorder.cpp src/streaming/perf_recorder.h
    src/streaming/frame_rate_tracker.h

    # src/confighttp
    src/confighttp/common.cpp src/confighttp/common.h
    src/confighttp/auth.cpp src/confighttp/auth.h
    src/confighttp/local_auth_token.cpp src/confighttp/local_auth_token.h
    src/confighttp/session_store.cpp src/confighttp/session_store.h
    src/confighttp/login_throttle.cpp src/confighttp/login_throttle.h
    src/confighttp/assets.cpp src/confighttp/assets.h
    src/confighttp/apps.cpp src/confighttp/apps.h
    src/confighttp/configuration.cpp src/confighttp/configuration.h
    src/confighttp/clients.cpp src/confighttp/clients.h
    src/confighttp/pair_link.cpp src/confighttp/pair_link.h
    src/confighttp/diagnostics.cpp src/confighttp/diagnostics.h
    src/confighttp/network.cpp src/confighttp/network.h
    src/confighttp/systeminfo.cpp src/confighttp/systeminfo.h
    src/confighttp/runtime.cpp src/confighttp/runtime.h
    src/confighttp/integrations.cpp src/confighttp/integrations.h
    src/confighttp/pairing.cpp src/confighttp/pairing.h
    src/confighttp/proxies.cpp src/confighttp/proxies.h
    src/confighttp/routes.cpp src/confighttp/routes.h
    src/confighttp/types.h
    src/confighttp/asset_paths.h
    # src/app_scan
    src/app_scan/scanner.cpp src/app_scan/libraries.cpp src/app_scan/scanner.h

    # src/text_context
    src/text_context/bridge.cpp src/text_context/bridge.h
    src/text_context/http.cpp src/text_context/http.h

    # src/platform
    src/platform/frame_contract.cpp src/platform/frame_contract.h
    src/platform/common.h

    # src/tray
    src/tray/system_tray.cpp src/tray/system_tray.h
    src/tray/system_tray_i18n.cpp src/tray/system_tray_i18n.h
    src/tray/tray_http.cpp src/tray/tray_http.h
    src/tray/tray_state.cpp src/tray/tray_state.h
    # src/launch_splash
    src/launch_splash/launch_splash.cpp src/launch_splash/launch_splash.h
    src/launch_splash/launch_splash_model.cpp src/launch_splash/launch_splash_model.h
    src/launch_splash/shown_gate.h
    # src/sparxie
    src/sparxie/model.cpp src/sparxie/model.h
    src/sparxie/region.cpp src/sparxie/region.h
    src/sparxie/storefront.cpp src/sparxie/storefront.h
    src/sparxie/vendors.cpp src/sparxie/vendors.h
    src/sparxie/scan.cpp src/sparxie/scan.h
    src/sparxie/store.cpp src/sparxie/store.h
    src/sparxie/settings.cpp src/sparxie/settings.h src/sparxie/hoyo_settings.cpp src/sparxie/hoyo_settings.h src/sparxie/ini_settings.cpp src/sparxie/ini_settings.h
    src/sparxie/zzz_file.cpp src/sparxie/zzz_file.h src/sparxie/zzz_extra.cpp
    src/sparxie/art.cpp src/sparxie/art.h
    src/sparxie/art_sources.cpp src/sparxie/art_sources.h
    src/sparxie/icon_file.cpp src/sparxie/icon_file.h
    src/sparxie/svg_path.cpp src/sparxie/svg_path.h
    src/sparxie/host.cpp src/sparxie/host.h
    src/sparxie/host_refresh.cpp
    src/sparxie/platform.cpp src/sparxie/platform.h
    src/sparxie/runtime.cpp src/sparxie/runtime.h
    src/sparxie/stream_display.cpp src/sparxie/stream_display.h
    src/sparxie/user_session.cpp src/sparxie/user_session.h
    src/sparxie/sha256.h
    src/sparxie/zip_extract.cpp src/sparxie/zip_extract.h
    src/sparxie/install.cpp src/sparxie/install.h
    src/sparxie/install_platform.cpp
    third-party/puff/puff.c third-party/puff/puff.h
    src/confighttp/sparxie.cpp src/confighttp/sparxie.h
)
list(TRANSFORM SUNSHINE_HOST_SOURCES PREPEND "${CMAKE_SOURCE_DIR}/")

# Explicit host inventory shared by the application and its test variant.
# Header entries retain IDE visibility; main.cpp belongs to the application.
set(SUNSHINE_HOST_SOURCES
    # third-party
    third-party/moonlight-common-c/src/RtspParser.c
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

    # src/hdr
    src/hdr/client_display_capabilities.cpp src/hdr/client_display_capabilities.h
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
    src/display_device/vdd_settings.cpp src/display_device/vdd_settings.h
    src/display_device/display_device.h
    src/display_device/vdd_control_ioctl.h

    # src
    src/upnp.cpp src/upnp.h
    src/cbs.cpp
    src/client_fingerprint.cpp src/client_fingerprint.h
    src/config.cpp src/config.h
    src/entry_handler.cpp src/entry_handler.h
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
    src/mic_mixer.cpp src/mic_mixer.h
    src/process.cpp src/process.h
    src/network.cpp src/network.h
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
    src/remote_usb/usbip_endpoint.h

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
    src/nvhttp/clipboard_api.cpp src/nvhttp/clipboard_api.h
    src/nvhttp/display_control.cpp src/nvhttp/display_control.h
    src/nvhttp/display_scale.cpp src/nvhttp/display_scale.h
    src/nvhttp/dynamic_params.cpp src/nvhttp/dynamic_params.h
    src/nvhttp/network_probe.cpp src/nvhttp/network_probe.h
    src/nvhttp/network_probe_limiter.cpp src/nvhttp/network_probe_limiter.h
    src/nvhttp/pairing.cpp src/nvhttp/pairing.h
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
    src/confighttp/assets.cpp src/confighttp/assets.h
    src/confighttp/apps.cpp src/confighttp/apps.h
    src/confighttp/configuration.cpp src/confighttp/configuration.h
    src/confighttp/clients.cpp src/confighttp/clients.h
    src/confighttp/diagnostics.cpp src/confighttp/diagnostics.h
    src/confighttp/runtime.cpp src/confighttp/runtime.h
    src/confighttp/integrations.cpp src/confighttp/integrations.h
    src/confighttp/proxies.cpp src/confighttp/proxies.h
    src/confighttp/routes.cpp src/confighttp/routes.h
    src/confighttp/types.h
    src/confighttp/asset_paths.h

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
)
list(TRANSFORM SUNSHINE_HOST_SOURCES PREPEND "${CMAKE_SOURCE_DIR}/")

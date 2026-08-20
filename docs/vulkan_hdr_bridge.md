# Vulkan HDR on Zako virtual displays

Some Windows Vulkan ICDs accept HDR10 and scRGB swapchains on an HDR-capable
IddCx display but omit those pairs from
`vkGetPhysicalDeviceSurfaceFormatsKHR`. Games commonly treat the enumeration
as a feature gate and hide HDR before trying to create a swapchain.

Foundation Sunshine uses the Zako Vulkan HDR bridge only for an active stream
that both uses ZakoVDD and requests HDR:

1. display configuration activates the VDD and enables Windows HDR;
2. Sunshine launches `vulkan_hdr_probe.exe` in the interactive user session;
3. the probe forces a short scRGB present and writes a cache under the
   interactive user's `%LOCALAPPDATA%\Sunshine` directory, keyed by GPU,
   Vulkan driver/API version, adapter LUID, and Windows build;
4. the same helper registers `VkLayer_zako_virtual_hdr.json` under the
   interactive user's 64-bit `HKCU` Vulkan implicit-layer registry; and
5. stream restore, normal shutdown, and next-start recovery remove the value.

The layer is a pass-through unless the Win32 surface belongs to an active HDR
Zako display and the cache contains both HDR-active validation and a successful
present. It only appends missing complete `(format, colorSpace)` pairs.

The backend owns registration and cleanup because it owns display-session
lifetime. No administrator registry write is required for the current per-user
path. WebUI exposes an Automatic/Disabled setting, live state, and an immediate
validation action; GUI big-screen mode does not duplicate this host-level setting.

Current scope is 64-bit Windows Vulkan applications. A separate 32-bit layer
binary and registry view are required for 32-bit games.

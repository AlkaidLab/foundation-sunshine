// Which settings of the configuration page need a restart of Sunshine to take effect, derived from the host (src/config.cpp):
//
//  - `config::parse` -> `apply_config` reads the configuration file once, at start, into config::video / audio / input / nvhttp / sunshine. Saving through
//    POST /api/config (confighttp/configuration.cpp saveConfig -> config::update_full_config) rewrites the file, and only the exceptions below also change
//    the running process, so every key apply_config reads takes effect at the next start. That is the default here.
//  - These are the exceptions, the keys that do not wait for a restart:
const HOT = {
  // nvprefs::get_nvprefs_options re-reads the file (get_effective_config_snapshot) at every launch: "apply to the next launch after saving, without
  // restarting Sunshine". No reader of that snapshot exists on the other platforms, so there is nothing to restart for anywhere.
  nvenc_manage_driver_settings: 'read from the file at every launch on Windows (nvprefs_common.cpp)',
  nvenc_vdd_force_vsync: 'read from the file at every launch on Windows (nvprefs_common.cpp)',
  // parsed at start (config.cpp) but nothing in the running process reads it afterwards: the About page reads the saved file, so the save is the whole effect
  notify_pre_releases: 'no reader in the running process; the page reads the saved file',
  // config::update_config / update_full_config call apply_live_launch_splash: the saved value is in force at the next launch, no restart
  launch_splash: 'applied by config::update_config and update_full_config (apply_live_launch_splash)',
  // (external_endpoints, the public address of the reachability card, is read by apply_config into config::nvhttp since the network overview: clients.cpp and
  // confighttp/network.cpp read that startup copy, and nothing refreshes it on save, so it waits for the restart like the rest)
}

// Keys that only some platforms apply without a restart. The platform is the one GET /api/config reports (windows, linux, macos); an unknown platform
// keeps the restart, the conservative answer.
const PLATFORM_HOT = {
  // update_full_config calls platf::set_global_gamepad_mode as soon as the file is written, and only the Windows input backend implements that setter:
  // the next virtual gamepad created in this process uses the new mode. The Linux and macOS setters are empty and their alloc paths still read the mode
  // parsed at start, so there the save waits for the restart like every other key.
  gamepad: { windows: 'applied by config::update_full_config on Windows (platf::set_global_gamepad_mode); the Linux and macOS setters are empty' },
}

// The keys whose stream value the client decides at every launch. The host copy is the cap, the advertised list or the default, and that copy is still
// the startup one, so they remain restart keys above: the restart alone does not change a stream, the client picks the rate, the size, the bitrate and
// the format inside what the host advertised when it launches the next session.
export const CLIENT_KEYS = Object.freeze(['fps', 'resolutions', 'max_bitrate', 'hevc_mode', 'av1_mode'])

let hostPlatform = ''
/** Records the platform GET /api/config reports; the configuration model calls it when it loads. */
export const setRestartPlatform = (platform) => { hostPlatform = String(platform || '') }

export const HOT_KEYS = Object.freeze([...Object.keys(HOT), ...Object.keys(PLATFORM_HOT)])
export const hotReason = (key) => HOT[key] || PLATFORM_HOT[key]?.[hostPlatform] || ''
/** true when the change of this key only takes effect after Sunshine restarts */
export const needsRestart = (key) => !hotReason(key)

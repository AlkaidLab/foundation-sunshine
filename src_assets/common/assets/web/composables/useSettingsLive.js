import { useHostLive } from './useHostLive.js'

// The live host state the settings pictures draw (the session in progress, its bitrate, the microphone route, the virtual
// display): the same polling as the home tiles, asked for only what the page's pictures use.
export function useSettingsLive({ platform, want = ['session', 'bitrate', 'mic', 'vdd'] }) {
  const ids = new Set(want)
  return useHostLive({ want: () => ids, platform: () => platform })
}

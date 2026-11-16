// The update channel of the Web UI (稳定版 or 测试版) is the host's own setting `notify_pre_releases` (配置 > 常规, and 关于 › 更新通道). Whether the person ever chose one: a mark left by a choice made in
// this browser (the guide or 关于), or the host already offering pre-releases (it can only have been switched on). The upgrade flow of the guide skips the channel step for a person who chose before.
export const CHANNEL_CHOSEN_KEY = 'foundation.update-channel.chosen'

const truthy = (value) => value === true || value === 'true' || value === 'enabled' || value === 1 || value === '1'

/** The channel the host's configuration holds: `prerelease` when the host notifies about pre-releases. */
export const channelOfConfig = (config) => (truthy(config?.notify_pre_releases) ? 'prerelease' : 'stable')

/** True when the person chose the channel before. */
export function channelChosen(config, storage) {
  try { if ((storage || globalThis.localStorage)?.getItem(CHANNEL_CHOSEN_KEY) === '1') return true } catch { /* no storage */ }
  return channelOfConfig(config) === 'prerelease'
}

/** Leaves the mark that the channel was chosen here. */
export function markChannelChosen(storage) {
  try { (storage || globalThis.localStorage)?.setItem(CHANNEL_CHOSEN_KEY, '1') } catch { /* the host setting still says prerelease when it was switched on */ }
}

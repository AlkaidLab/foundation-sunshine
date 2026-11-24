import { mergeSetupConfig } from 'foundation-ui/vue/guide.js'
import { apiFetch, apiJson } from '../utils/apiFetch.js'

/**
 * 保存向导选择时保留完整配置。
 *
 * `/api/config` 的 POST 是完整替换，因此总是先读出当前配置再合并。
 * 设置向导和新手引导共用这一处：只传入要改的项，`completed` 为 `false` 时不改完成标记以外的内容。
 */
export async function saveSetupChoices({ locale, adapter, display, prep, hdr, completed = true, extra = {} } = {}) {
  const config = { ...mergeSetupConfig(await apiJson('/api/config'), { locale, adapter, display, prep, hdr, completed }), ...extra }

  return { config, response: await apiFetch('/api/config', { method: 'POST', body: config }) }
}

/** 只把向导标记为已完成（跳过向导），不改其他设置。 */
export const markSetupCompleted = () => saveSetupChoices({})

export async function saveSetupWizardLocale(locale) {
  const currentConfig = await apiJson('/api/config')

  return apiFetch('/api/config', {
    method: 'POST',
    body: {
      ...currentConfig,
      locale,
    },
  })
}

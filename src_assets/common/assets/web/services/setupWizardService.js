import { apiFetch, apiJson } from '../utils/apiFetch.js'

/**
 * 保存向导语言时保留完整配置。
 *
 * `/api/config` 的 POST 是完整替换，因此不能只提交 locale。
 */
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

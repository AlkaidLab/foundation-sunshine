export function installPayload(path, { confirmUnverified = false, acknowledged = true } = {}) {
  return { path, confirm_unverified: !!confirmUnverified, acknowledged: acknowledged !== false }
}

export async function installFromFile({ status, pick, ask, post, texts }) {
  if (!status || status.noticeAck !== true) {
    const accepted = await ask(status?.statement || texts.bring, texts.noticeTitle, texts.noticeYes)
    if (!accepted) return { ok: false, error_code: 'cancelled' }
  }
  const path = await pick()
  if (!path) return { ok: false, error_code: 'cancelled' }
  let result = await post(installPayload(path, { acknowledged: true }))
  if (result && (result.error_code === 'unverified' || result.error_code === 'hash_mismatch')) {
    const accepted = await ask(texts.unverified, texts.unverifiedTitle, texts.unverifiedYes)
    if (!accepted) return { ok: false, error_code: 'cancelled' }
    result = await post(installPayload(path, { confirmUnverified: true, acknowledged: true }))
  }
  return result
}

export async function uninstallChoice(post, ask, texts) {
  let result = await post({})
  if (result && result.error_code === 'confirm_config') {
    const choice = await ask(texts.removeText, texts.removeTitle, texts.removeDelete, texts.removeKeep)
    if (choice === 'dismiss') return { ok: false, error_code: 'cancelled' }
    result = await post({ delete_config: choice === 'delete' })
  }
  return result
}

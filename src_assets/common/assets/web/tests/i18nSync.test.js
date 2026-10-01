import test from 'node:test'
import assert from 'node:assert/strict'
import { copyFileSync, mkdirSync, mkdtempSync, readFileSync, rmSync, writeFileSync } from 'node:fs'
import { tmpdir } from 'node:os'
import { join } from 'node:path'
import { execFileSync } from 'node:child_process'

test('i18n sync preserves translated shutdown actions while normalizing protocol names', context => {
  const fixture = mkdtempSync(join(tmpdir(), 'sunshine-i18n-sync-'))
  context.after(() => rmSync(fixture, { recursive: true, force: true }))
  const scripts = join(fixture, 'scripts')
  const locales = join(fixture, 'src_assets/common/assets/web/public/assets/locale')
  mkdirSync(scripts)
  mkdirSync(locales, { recursive: true })
  const script = join(scripts, 'validate-i18n.mjs')
  copyFileSync(new URL('../../../../../scripts/validate-i18n.js', import.meta.url), script)

  const translations = { en: 'Boom! Stop Sunshine', fr: 'Boom! Arrêter Sunshine', zh: 'Boom! 停止 Sunshine' }
  for (const [locale, label] of Object.entries(translations)) {
    writeFileSync(join(locales, `${locale}.json`), JSON.stringify({
      troubleshooting: { boom_sunshine: label },
      config: { port_tcp: locale === 'en' ? 'TCP' : 'localized protocol' },
    }))
  }

  execFileSync(process.execPath, [script, '--sync'], { encoding: 'utf8' })
  for (const [locale, label] of Object.entries(translations)) {
    const result = JSON.parse(readFileSync(join(locales, `${locale}.json`), 'utf8'))
    assert.equal(result.troubleshooting.boom_sunshine, label)
    assert.equal(result.config.port_tcp, 'TCP')
  }
})

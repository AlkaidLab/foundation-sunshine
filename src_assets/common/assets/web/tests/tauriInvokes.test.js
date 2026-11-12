import test from 'node:test'
import assert from 'node:assert/strict'
import fs from 'node:fs'
import path from 'node:path'

// The Web UI runs in the control panel's frame as a remote origin of the main window. Tauri v2 refuses a command the origin's capability does not list (「Command x not allowed by ACL」), and the
// capability is src-tauri/webui-permissions.json in the control panel's repository, which this repository does not contain. So the commands the Web UI invokes are written down here, and a new
// one fails this test until it is added here AND to webui-permissions.json (the panel's capabilityAudit.test.js checks that list against the file).
const WEBUI_INVOKES = ['scan_directory_for_apps', 'scan_game_libraries', 'list_display_modes', 'set_display_mode', 'get_current_dpi', 'set_desktop_dpi', 'is_sunshine_running_in_user_mode', 'toggle_sunshine_mode',
  'launch_windows_hdr_calibration', 'set_vdd_keep_enabled', 'set_vdd_headless_create_enabled', 'refresh_sunshine_target', 'open_external_url', 'open_device_manager', 'save_text_file']
const root = new URL('../', import.meta.url).pathname
const walk = (dir, out = []) => {
  for (const e of fs.readdirSync(dir, { withFileTypes: true })) {
    if (['node_modules', 'tests', 'public'].includes(e.name)) continue
    const f = path.join(dir, e.name)
    if (e.isDirectory()) walk(f, out)
    else if (/\.(vue|js)$/.test(e.name)) out.push(f)
  }
  return out
}

test('the commands the Web UI invokes are the ones the control panel grants to its origin', () => {
  const used = new Set()
  for (const file of walk(root)) {
    const src = fs.readFileSync(file, 'utf8')
    for (const m of src.matchAll(/\b\w*[iI]nvoke\(\s*['"`]([a-z][a-z0-9_]*)['"`]/g)) used.add(m[1])
    for (const m of src.matchAll(/\b\w*[iI]nvoke\(\s*[^?\n]*\?\s*['"`]([a-z][a-z0-9_]*)['"`]\s*:\s*['"`]([a-z][a-z0-9_]*)['"`]/g)) { used.add(m[1]); used.add(m[2]) }
    for (const m of src.matchAll(/saveThroughDialog\([^)]*?,\s*['"`](save_text_file)['"`]/g)) used.add(m[1])
  }
  assert.deepEqual([...used].filter((c) => !WEBUI_INVOKES.includes(c)).sort(), [], 'a command the Web UI invokes is not in WEBUI_INVOKES: add it here and to src-tauri/webui-permissions.json of the control panel')
  assert.deepEqual(WEBUI_INVOKES.filter((c) => !used.has(c)).sort(), [], 'WEBUI_INVOKES lists a command the Web UI no longer invokes')
})

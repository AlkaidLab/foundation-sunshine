import { computed, onBeforeUnmount, onMounted, ref } from 'vue'

// What the host reports about HDR right now (/api/runtime/hdr): the pipelines of the streams in progress. The page shows one state: streaming in HDR, fallen back to SDR, unavailable (with the
// reason), or ready. Polled while the page is open and visible; nothing is guessed: no report is "unknown".
export function useHdrRuntime({ platform, config }) {
  const status = ref(null)
  const loaded = ref(false)
  let timer
  let active = false
  const pipeline = computed(() => { const list = status.value?.pipelines ?? []; return list.find((p) => p.hdr_mode !== 'sdr') ?? list[0] ?? null })
  const allowed = computed(() => Number(config.hevc_mode) === 3 || Number(config.av1_mode) === 3)

  const view = computed(() => {
    if (!loaded.value) return { state: 'unknown', reason: '' }
    if (!allowed.value) return { state: 'off', reason: '' }
    const p = pipeline.value
    if (!status.value?.available || !p) return { state: 'idle', reason: '' }
    if (p.hdr_mode === 'sdr') return { state: 'fallback', reason: p.analysis_failure_reason === 'encoder_unsupported' ? 'encoder' : p.analysis_failure_reason === 'format_unsupported' ? 'codec' : '' }
    return { state: 'running', reason: '', mode: String(p.hdr_mode || '').toUpperCase() }
  })

  async function refresh() {
    try {
      const response = await fetch('/api/runtime/hdr')
      if (!response.ok) throw new Error(`HTTP ${response.status}`)
      status.value = await response.json()
    } catch { status.value = { available: false, pipelines: [] } } finally { loaded.value = true }
  }
  function schedule() {
    if (!active) return
    clearTimeout(timer)
    timer = setTimeout(async () => { if (!document.hidden) await refresh(); schedule() }, pipeline.value ? 3000 : 10000)
  }
  onMounted(async () => { if (platform !== 'windows') return; active = true; await refresh(); schedule() })
  onBeforeUnmount(() => { active = false; clearTimeout(timer) })
  return { status, view, pipeline }
}

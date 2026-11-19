import { onBeforeUnmount, onMounted, ref } from 'vue'
import { createDriverGuardClient } from 'foundation-ui/driverGuard'
import { apiJson, apiPostJson } from '../utils/apiFetch.js'

// The state of the graphics driver guard of the host (GET /api/gpu-driver-guard): the safe mode, the encoder check that runs after the start, and the notice that
// the last failure lay in a graphics driver. The home page draws it with MDriverGuardNotice (foundation-ui); this file only reads and answers.
export function useDriverGuard() {
  const guard = ref(null)
  let timer = 0
  let closed = false
  const client = createDriverGuardClient(
    { get: () => apiJson('/api/gpu-driver-guard'), post: (body) => apiPostJson('/api/gpu-driver-guard', body) },
    (next) => { guard.value = next }
  )
  // one place makes the timer: an older one is cleared first, and nothing runs after the unmount
  const schedule = () => {
    clearTimeout(timer)
    if (!closed) timer = setTimeout(loop, client.nextDelay())
  }
  const loop = async () => {
    if (closed) return
    await client.refresh()
    schedule()
  }
  onMounted(loop)
  onBeforeUnmount(() => { closed = true; clearTimeout(timer) })
  // an answer to a button is a new state: the next look comes soon, the host may be checking the encoders now
  const answer = (call) => async (...args) => { try { await call(...args) } finally { schedule() } }
  return { guard, dismiss: answer(client.dismiss), retry: answer(client.retry) }
}

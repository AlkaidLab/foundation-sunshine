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
  const loop = async () => {
    await client.refresh()
    if (!closed) timer = setTimeout(loop, client.nextDelay())
  }
  onMounted(loop)
  onBeforeUnmount(() => { closed = true; clearTimeout(timer) })
  // an answer to a button is a new state: the next look comes soon, the host may be checking the encoders now
  const answer = (call) => async (...args) => { await call(...args); clearTimeout(timer); if (!closed) timer = setTimeout(loop, client.nextDelay()) }
  return { guard, dismiss: answer(client.dismiss), retry: answer(client.retry) }
}

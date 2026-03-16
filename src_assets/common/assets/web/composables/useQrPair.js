import { ref, computed, onUnmounted } from 'vue'
import QRCode from 'qrcode'

export function useQrPair() {
  const qrDataUrl = ref('')
  const qrPin = ref('')
  const qrUrl = ref('')
  const qrExpiresAt = ref(0)
  const qrRemaining = ref(0)
  const qrLoading = ref(false)
  const qrError = ref('')
  const qrPaired = ref(false)

  let countdownTimer = null
  let pollTick = 0

  const qrActive = computed(() => qrRemaining.value > 0 && qrDataUrl.value !== '')

  const resetQrDisplay = () => {
    qrDataUrl.value = ''
    qrPin.value = ''
    qrUrl.value = ''
  }

  const stopCountdown = () => {
    if (countdownTimer) {
      clearInterval(countdownTimer)
      countdownTimer = null
    }
  }

  const startCountdown = () => {
    stopCountdown()
    pollTick = 0
    countdownTimer = setInterval(async () => {
      const now = Date.now()
      const remaining = Math.max(0, Math.floor((qrExpiresAt.value - now) / 1000))
      qrRemaining.value = remaining
      if (remaining <= 0) {
        stopCountdown()
        resetQrDisplay()
        return
      }

      // Poll status every 2 ticks
      if (++pollTick % 2 === 0) {
        try {
          const res = await fetch('/api/qr-pair')
          const data = await res.json()
          if (data.status === 'paired') {
            stopCountdown()
            resetQrDisplay()
            qrPaired.value = true
          }
        } catch (e) { /* ignore */ }
      }
    }, 1000)
  }

  const generateQrCode = async () => {
    qrLoading.value = true
    qrError.value = ''
    qrPaired.value = false
    try {
      const response = await fetch('/api/qr-pair', { method: 'POST' })
      const data = await response.json()

      if (data.status?.toString() !== 'true') {
        qrError.value = data.error || 'Failed to generate QR code'
        return
      }

      qrPin.value = data.pin
      qrUrl.value = data.url
      qrExpiresAt.value = Date.now() + data.expires_in * 1000
      qrRemaining.value = data.expires_in

      qrDataUrl.value = await QRCode.toDataURL(data.url, {
        width: 280,
        margin: 2,
        color: { dark: '#000000', light: '#ffffff' },
      })

      startCountdown()
    } catch (error) {
      console.error('Failed to generate QR pair info:', error)
      qrError.value = 'Network error'
    } finally {
      qrLoading.value = false
    }
  }

  const cancelQrCode = async () => {
    stopCountdown()
    resetQrDisplay()
    qrRemaining.value = 0
    try {
      await fetch('/api/qr-pair/cancel', { method: 'POST' })
    } catch (e) {
      // Best-effort cancel, ignore network errors
    }
  }

  onUnmounted(() => {
    stopCountdown()
  })

  return {
    qrDataUrl,
    qrPin,
    qrUrl,
    qrRemaining,
    qrLoading,
    qrError,
    qrPaired,
    qrActive,
    generateQrCode,
    cancelQrCode,
  }
}

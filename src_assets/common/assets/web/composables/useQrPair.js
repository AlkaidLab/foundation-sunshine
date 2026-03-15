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

  let countdownTimer = null

  const qrActive = computed(() => qrRemaining.value > 0 && qrDataUrl.value !== '')

  const stopCountdown = () => {
    if (countdownTimer) {
      clearInterval(countdownTimer)
      countdownTimer = null
    }
  }

  const startCountdown = () => {
    stopCountdown()
    countdownTimer = setInterval(() => {
      const now = Date.now()
      const remaining = Math.max(0, Math.floor((qrExpiresAt.value - now) / 1000))
      qrRemaining.value = remaining
      if (remaining <= 0) {
        stopCountdown()
        qrDataUrl.value = ''
        qrPin.value = ''
        qrUrl.value = ''
      }
    }, 1000)
  }

  const generateQrCode = async () => {
    qrLoading.value = true
    qrError.value = ''
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

  const cancelQrCode = () => {
    stopCountdown()
    qrDataUrl.value = ''
    qrPin.value = ''
    qrUrl.value = ''
    qrRemaining.value = 0
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
    qrActive,
    generateQrCode,
    cancelQrCode,
  }
}

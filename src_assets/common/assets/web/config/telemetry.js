import { createEventSender, installEventListeners, setReportTransport } from 'foundation-ui/telemetry'
import { apiFetch } from '../utils/apiFetch.js'

// Usage counts, experience numbers and errors of the Web UI (telemetry v1, docs/privacy_telemetry.md). The sender and its rules are the library's (foundation-ui/telemetry/events.js): the page hands its events to the
// host it is served from, which sends them with its own to the developers' server. No Google script is loaded.
const sender = createEventSender({
  product: 'web',
  post: (body, { keepalive }) => apiFetch('/api/telemetry/events', { method: 'POST', body, keepalive, credentials: 'same-origin' }),
})
installEventListeners(sender)
// the reports the person sends by hand (the error dialog's form) and the automatic issues travel the same way: to this host, which sends them on
setReportTransport((events) => sender.postNow(events))

/** The version of the shell as shown, and whether this page is embedded in the control panel, which counts for itself. */
export const setTelemetryContext = (context) => sender.setContext(context)
export const flushTelemetry = () => sender.flush({ keepalive: true })

export const trackEvents = {
  appStart: () => sender.track.appStart({ startMs: typeof performance !== 'undefined' ? performance.now() : undefined }),
  pageView: (page) => sender.track.pageView(page),
  // a fixed word for a feature, never a name the person chose
  userAction: (action) => sender.feature(`web.${action}`),
  appAdded: () => sender.feature('web.app_added'),
  appDeleted: () => sender.feature('web.app_deleted'),
  configChanged: (section) => sender.feature(`web.config_saved.${String(section ?? '').slice(0, 20)}`),
  errorOccurred: (type, message) => sender.track.error(type, message),
  devicePaired: () => sender.feature('web.device_paired'),
  // the version check and the GPU are known to the host, which reports its own version and GPU vendor with every batch
  versionChecked: () => {},
  gpuReported: () => {},
}

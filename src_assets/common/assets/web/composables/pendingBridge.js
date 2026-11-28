// Pending results can contain nested Vue proxies. postMessage only accepts
// structured-cloneable values, so send the same plain JSON data as the bridge.
export function postPendingMessage(target, message) {
  target.postMessage(JSON.parse(JSON.stringify(message)), '*')
}

// What the overview keeps between visits, in memory (stale-while-revalidate): the Web UI leaves the page when another one is opened and builds it again when the person comes back,
// and everything the first frame is drawn from has to be there at once, or the tiles are drawn from nothing and then laid out again when the host's answers arrive. The host's own answers
// (the session, the load, the virtual display) are kept by useHostLive, its config and its apps by hostData; what is left is here.
// The same rule as the control panel's overview (usePanelHome.js).

/** The first reads of a visit that has nothing kept may take this long before the overview is drawn anyway (its skeleton stands until then). */
export const SETTLE_CAP_MS = 1500

export const kept = {
  /** an earlier visit drew the overview from the host's answers: this one starts from what it kept and needs no waiting */
  settled: false,
  gamepad: '',
  logs: null,
  nics: null,
  series: null,
}

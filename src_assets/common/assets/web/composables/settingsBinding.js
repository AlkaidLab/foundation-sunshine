// The settings of the configuration page (the search index) and the configuration keys their controls write.
//
// Most controls are a Cfg row and write config[id]. These are the ones that are not that:

/** controls whose id is not the one key they write: id -> the keys */
export const WRITES = {
  // the codec strategy cards and the HDR switch are one model over the two codec modes the host reads (hevc_mode, av1_mode)
  codec_strategy: ['hevc_mode', 'av1_mode'],
  codec_enable_hdr: ['hevc_mode', 'av1_mode'],
}

/** controls that save through an action of their own (a card with its own button) and so are never pending changes of the page */
export const OWN_SAVE = new Set(['external_endpoints'])

/** rows of the page that are not a setting: they open the component manager of the enhancement components */
export const NOT_A_KEY = new Set(['hdr_enhanced'])

/** the keys a control writes */
export const keysOf = (id) => (NOT_A_KEY.has(id) ? [] : WRITES[id] || [id])

// The first frame of the sign-in page (inline in login.html, before the first paint; the host serves the page before anyone is signed in, so nothing can be fetched).
// It paints what the last authenticated load of this browser left in localStorage (foundation-ui.boot, see foundation-ui appearance/boot.js): the colours, light or dark,
// the wallpaper (a tiny copy, blurred) and the frost over it. With no record it paints the bundled default: the wallpaper of the window's shape and its colours (a record
// made from a real first start). The real appearance arrives with the app and replaces this without a change of colour (appearanceBoot.js ends it).
// Kept in sync with parseBoot / prefixThemeCss / bootLayersCss of foundation-ui; the defaults below are generated (tools: gen-login-boot-defaults).
(function () {
  try {
    var DEFAULTS = /*DEFAULTS*/null;
    var doc = document, root = doc.documentElement, record = null;
    try { record = JSON.parse(localStorage.getItem('foundation-ui.boot') || 'null'); } catch (e) { record = null; }
    var ok = function (r) { return r && r.v === 1 && typeof r.css === 'string' && r.css.indexOf('--md-sys-color-surface:') > 0; };
    if (!ok(record)) {
      var wide = innerHeight > 0 && innerWidth / innerHeight >= 1.6;
      record = DEFAULTS && (wide ? DEFAULTS.vivi2 : DEFAULTS.vivi1);
      if (!ok(record)) return;
    }
    var mode = record.mode === 'light' || record.mode === 'dark' ? record.mode : 'system';
    var stored = null; try { stored = localStorage.getItem('theme'); } catch (e) { stored = null; }
    // the Web UI's own light/dark switch (kept by the appearance) wins when it says light or dark
    if (stored === 'light' || stored === 'dark') mode = mode === 'system' ? stored : mode;
    var dark = mode === 'dark' || (mode === 'system' && window.matchMedia && matchMedia('(prefers-color-scheme: dark)').matches);
    root.setAttribute('data-bs-theme', dark ? 'dark' : 'light');
    root.setAttribute('data-dark', record.dark === 'black' ? 'black' : 'tinted');
    var theme = doc.createElement('style');
    theme.id = 'foundation-ui-boot-theme';
    theme.textContent = String(record.css).replace(/(^|\n)(:root|\[data-bs-theme=)/g, '$1html$2');
    doc.head.appendChild(theme);
    var a = Math.round((dark ? record.scrim.dark : record.scrim.light) * 100);
    var picture = record.thumb ? 'url("' + record.thumb + '") center / cover' : (record.gradient || '');
    var top = picture ? Math.max(35, a) : 0;
    // the frost of the app (appearance.css): the page tone at the strength of the scrim at the top, paler towards the bottom (the fade tone, at 94 % at the least)
    var frost = 'linear-gradient(to bottom, color-mix(in srgb, var(--m-page-solid) ' + top + '%, transparent), color-mix(in srgb, var(--m-fade-tone) max(' + top + '%, 94%), transparent))';
    // one layer, the root's ::after (the app's own stylesheet takes the root's ::before away when its wallpaper starts): the frost over the picture
    var css = 'html.m-boot { background: var(--md-sys-color-surface); }' +
      'html.m-boot::after { content: ""; position: fixed; inset: 0; z-index: -1; pointer-events: none; background: ' + (picture ? frost + ', ' + picture : frost) + '; }' +
      'html.m-boot body, html.m-boot .m-app, html.m-boot .m-login { background: transparent !important; }' +
      'html.m-boot-out::after { opacity: 0; transition: opacity 450ms ease; }';
    var layers = doc.createElement('style');
    layers.id = 'foundation-ui-boot-layers';
    layers.textContent = css;
    doc.head.appendChild(layers);
    root.classList.add('m-boot');
  } catch (e) { /* no record, no storage: the page starts as it did */ }
})();

import { defineAsyncComponent, defineComponent, h } from 'vue'
import { MBanner, MButton, MPageLoading } from 'foundation-ui/vue'
import { useI18n } from 'vue-i18n'

// A page or a tab of a page that loads on demand is never a blank pane: while it loads the caller's loading state shows, a chunk that fails (a server that restarted with another build, a dropped connection) is tried once more,
// and one that still does not load shows the failure and a button to load it again.
const PaneFailed = defineComponent({
  name: 'PaneFailed',
  setup() {
    const { t } = useI18n()
    return () => h('div', { class: 'm-page' }, [h('div', { class: 'm-page__column', style: 'padding-top: 24px' }, [
      h(MBanner, { tone: 'error', icon: 'error' }, { default: () => t('shell.pane_failed'), actions: () => h(MButton, { variant: 'text', size: 'sm', onClick: () => window.location.reload() }, () => t('shell.pane_reload')) }),
    ])])
  },
})

// a caller that names no loading state gets the library's page skeleton (it fades in after its own short delay, so a chunk that is there in a moment never shows it)
const DefaultLoading = defineComponent({ name: 'PaneLoading', setup: () => () => h(MPageLoading) })

export function lazyPane(loader, { loading = DefaultLoading, timeout = 20000 } = {}) {
  return defineAsyncComponent({ loader: () => loader().catch(() => loader()), loadingComponent: loading, errorComponent: PaneFailed, delay: 0, timeout, suspensible: false })
}

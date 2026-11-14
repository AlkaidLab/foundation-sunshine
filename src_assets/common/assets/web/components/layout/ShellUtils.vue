<script setup>
import { computed, inject, onMounted, ref, useId } from 'vue'
import { useI18n } from 'vue-i18n'
import { MIcon, MIconButton, MSheet, MAppearance, MSponsorButton, aboutTexts, appearanceTexts, useAppearance } from 'foundation-ui/vue'
import { SPONSORS } from '../../config/sponsors.js'
import { labelOf } from 'foundation-ui/settings/index.js'
import { loadAutoTheme, setStoredTheme, setTheme } from '../../utils/theme.js'

// Theme and account as two round icon buttons with a menu each. `rail` sits in the foot of the rail (menus open to the
// side and upwards); `bar` sits in a page header when the rail is not shown (bottom bar mode).
const props = defineProps({ placement: { type: String, default: 'rail' } })
const { t, locale } = useI18n()
const shell = inject('webShell', null)
const themeAnchor = useId()
const accountAnchor = useId()
const themeMenu = ref(null)
const accountMenu = ref(null)
const personalise = ref(false)
const zh = () => locale.value && String(locale.value).startsWith('zh')
const appearance = useAppearance()
const supportSections = () => [{ title: zh() ? '赞助' : 'Sponsor', items: SPONSORS.map((x) => ({ id: x.id, icon: 'favorite', title: x.label, url: x.url })) }, { items: [{ id: 'star', icon: 'star', title: zh() ? '在 GitHub 点 Star' : 'Star on GitHub', url: 'https://github.com/AlkaidLab/foundation-sunshine' }, { id: 'qq', icon: 'groups', title: zh() ? 'QQ 交流群' : 'QQ group', url: 'https://qm.qq.com/q/AfMQoyKrkc' }] }]
const openLink = (it) => window.open(it.url, '_blank', 'noopener,noreferrer')
const theme = ref('auto')
const THEMES = [{ id: 'light', icon: 'light_mode', label: 'navbar.theme_light' }, { id: 'dark', icon: 'dark_mode', label: 'navbar.theme_dark' }, { id: 'auto', icon: 'contrast', label: 'navbar.theme_auto' }]
const corners = props.placement === 'rail' ? { 'anchor-corner': 'start-end', 'menu-corner': 'end-start' } : { 'anchor-corner': 'end-end', 'menu-corner': 'start-end' }

onMounted(() => {
  loadAutoTheme()
  const stored = localStorage.getItem('theme')
  theme.value = stored || 'auto'
})
function pick(id) {
  theme.value = id
  setStoredTheme(id)
  setTheme(id)
  // the shared appearance keeps the same choice, so the next start applies it together with the wallpaper
  void appearance.set({ mode: id === 'auto' ? 'system' : id })
}
// the footer's buttons are first-level pages (an entry of the rail, no Back); 关于 is also pushed from 帮助与关于 and the status pages, where it keeps its Back
const go = (id) => (shell?.nav ? shell.nav.go(id, undefined, { root: true }) : (window.location.href = id === 'password' ? '/password' : '/troubleshooting'))
const appearanceTitle = computed(() => labelOf(String(locale.value), 'shell.appearance'))
const aboutTitle = computed(() => aboutTexts(String(locale.value)).title)
// one name for one thing: where the page 个性化 has its own button (the rail) this one is the light or dark switch (明暗, the name it has on that page); where it has not (the bar of a phone) this button is 个性化, and its menu leads there
const themeTitle = computed(() => (props.placement === 'rail' ? appearanceTexts(String(locale.value)).mode : appearanceTitle.value))
const current = () => THEMES.find((x) => x.id === theme.value) || THEMES[2]
</script>

<template>
  <div class="m-web-utils" :class="`m-web-utils--${placement}`">
    <span v-if="placement === 'rail'" class="m-web-utils__cell"><MIconButton icon="palette" :label="appearanceTitle" :current="shell?.nav?.current.value === 'appearance'" @click="go('appearance')" /></span>
    <span v-if="placement === 'rail'" class="m-web-utils__cell"><MIconButton icon="info" :label="aboutTitle" :current="shell?.nav?.current.value === 'about'" @click="go('about')" /></span>
    <span v-if="placement === 'rail'" class="m-web-utils__cell"><MSponsorButton :label="zh() ? '支持开发者' : 'Support the developers'" :title="zh() ? '支持开发者' : 'Support the developers'" :sections="supportSections()" :close-label="t('_common.close')" @open="openLink" /></span>
    <span v-if="placement === 'rail'" class="m-web-utils__cell">
      <MIconButton :id="themeAnchor" :icon="current().icon" :label="`${themeTitle} (${t(current().label)})`" aria-haspopup="menu" @click="themeMenu.open = !themeMenu.open" />
      <md-menu ref="themeMenu" :anchor="themeAnchor" positioning="fixed" v-bind="corners">
        <md-menu-item v-for="item in THEMES" :key="item.id" :selected="theme === item.id" @click="pick(item.id)">
          <MIcon slot="start" :name="item.icon" />
          <div slot="headline">{{ t(item.label) }}</div>
          <MIcon v-if="theme === item.id" slot="end" name="check" />
        </md-menu-item>
        <md-divider v-if="placement !== 'rail'" />
        <md-menu-item v-if="placement !== 'rail'" @click="personalise = true"><MIcon slot="start" name="palette" /><div slot="headline">{{ locale && String(locale).startsWith('zh') ? '个性化' : 'Personalise' }}</div></md-menu-item>
      </md-menu>
    </span>
    <span class="m-web-utils__cell">
      <MIconButton :id="accountAnchor" icon="account_circle" :label="t('shell.account')" aria-haspopup="menu" @click="accountMenu.open = !accountMenu.open" />
      <md-menu ref="accountMenu" :anchor="accountAnchor" positioning="fixed" v-bind="corners">
        <template v-if="placement !== 'rail'">
          <md-menu-item v-for="item in THEMES" :key="item.id" :selected="theme === item.id" @click="pick(item.id)"><MIcon slot="start" :name="item.icon" /><div slot="headline">{{ t(item.label) }}</div><MIcon v-if="theme === item.id" slot="end" name="check" /></md-menu-item>
          <md-menu-item @click="personalise = true"><MIcon slot="start" name="palette" /><div slot="headline">{{ appearanceTitle }}</div></md-menu-item>
          <md-divider />
        </template>
        <md-menu-item @click="go('password')"><MIcon slot="start" name="key" /><div slot="headline">{{ t('navbar.password') }}</div></md-menu-item>
        <md-menu-item @click="go('troubleshooting')"><MIcon slot="start" name="stethoscope" /><div slot="headline">{{ t('navbar.troubleshoot') }}</div></md-menu-item>
        <md-divider />
        <md-menu-item class="m-danger" @click="shell?.askLogout()"><MIcon slot="start" name="logout" /><div slot="headline">{{ t('troubleshooting.logout') }}</div></md-menu-item>
      </md-menu>
    </span>
    <MSheet v-model="personalise" :title="locale && String(locale).startsWith('zh') ? '个性化' : 'Personalise'" side="end" :close-label="t('_common.close')"><MAppearance :locale="String(locale)" /></MSheet>
  </div>
</template>

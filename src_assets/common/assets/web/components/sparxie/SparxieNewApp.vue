<template>
  <MPage ref="page" wide class="spx-new" :title="isEdit ? (edit.name || C.editTitle) : C.newTitle" back :back-label="backLabel" @back="$emit('close')">
    <!-- the risk statement is about touch launch: shown with it, until the first such app was created -->
    <SparxieNotice v-if="touch && !status.warned" />
    <MSkeleton v-if="!loaded" :rows="3" />

    <template v-else>
      <!-- 1 · game: the titles the host sends, found by typing a name, an English name or a short name (星铁, HSR, ZZZ) -->
      <MSection v-if="!isEdit" :title="C.secGame">
        <label class="m-filterbar spx-search">
          <MIcon name="search" />
          <input v-model="query" type="search" :placeholder="C.searchGames" :aria-label="C.searchGames" autocomplete="off" spellcheck="false">
          <button v-if="query" type="button" class="m-filterbar__clear" :aria-label="C.clearSearch" @click="query = ''"><MIcon name="close" /></button>
        </label>
        <MGroup v-if="matches.length" role="radiogroup" :aria-label="C.secGame">
          <MItem v-for="g in matches" :key="g" action class="spx-title" :selected="game === g" role="radio" :aria-checked="game === g" :title="names[g]" :text="subtitleOf(g)" @click="chooseGame(g)">
            <template #lead><span class="spx-icon-box"><MIcon name="sports_esports" /><img :key="titleIcon(g)" class="spx-icon" :src="titleIcon(g)" alt="" draggable="false" @error="hide"></span></template>
            <template #trail><MChip v-if="knownTag(g)" tone="tertiary">{{ knownTag(g) }}</MChip><MIcon v-if="game === g" name="check" class="spx-check" /></template>
          </MItem>
        </MGroup>
        <MEmpty v-else icon="search" :title="C.noTitle(query)" />
      </MSection>

      <!-- the hero opens under the tiles once the official art of the chosen game has loaded (it does not push the tile that was just tapped); until then nothing is drawn -->
      <div class="spx-hero" :class="{ 'is-open': heroReady }" :aria-hidden="!heroReady">
        <div class="spx-hero__clip">
          <div class="spx-new__hero" :style="heroStyle">
            <div class="spx-new__chips">
              <MChip v-if="touch" tone="tertiary" :image="SPARXIE_MARK">Sparxie</MChip>
              <MChip v-if="region" icon="public">{{ C.regions[region] }}</MChip>
            </div>
          </div>
        </div>
      </div>

      <!-- 2 · region -->
      <MSection v-if="reach(2) && !isEdit" :title="C.secRegion">
        <MGroup>
          <MItem stack>
            <template #control><MSeg v-model="region" block :label="C.secRegion" :options="regionOptions" /></template>
          </MItem>
        </MGroup>
      </MSection>

      <!-- 3 · where the game is: one row per install (icon, game, region, path); 更换 opens the two ways to choose another -->
      <MSection v-if="reach(3)" :title="C.secInstall">
        <MGroup v-if="choices.length">
          <SparxieInstallRow v-for="i in choices" :key="i.dir + i.exe" :action="!isEdit" :install="i" :title="names[i.game]" :region="region" :version="artMap[artIdOf(i)]" :selected="install === i" @click="select(i)">
            <template #trail><MChip v-if="sourceLabel(i)">{{ sourceLabel(i) }}</MChip></template>
          </SparxieInstallRow>
        </MGroup>
        <!-- nothing was found by itself: a plain note, not an error; red only comes after the person picked something and it failed -->
        <MBanner v-else-if="!isEdit && !browsing && !scanning && !pickError" tone="info" icon="info">{{ C.installNone }}</MBanner>
        <MFooter v-if="choices.length > 1 && !install">{{ C.installChoose }}</MFooter>
        <MBanner v-if="pickError" tone="error" icon="error">{{ pickError }}</MBanner>
        <MSkeleton v-if="scanning" :rows="1" />

        <MGroup v-if="!isEdit && !browsing && pickersOpen">
          <MItem action icon="sports_esports" :title="C.pickExe" :text="exeNames" :disabled="scanning" @click="pick('file')" />
          <MItem action icon="folder_open" :title="C.pickFolder" :text="C.pickFolderText" :disabled="scanning" @click="pick('folder')" />
        </MGroup>
        <MGroup v-else-if="!isEdit && !browsing">
          <MItem action icon="swap_horiz" :title="C.changeInstall" @click="showPickers = true" />
        </MGroup>
      </MSection>
      <SparxieBrowse v-if="reach(3) && browsing" :key="browsing" :kind="browsing" @choose="onBrowsed" @cancel="browsing = ''" />

      <!-- 4 · Sparxie's own settings: an app of a Sparxie title always starts through Sparxie while the component is installed, so there is no launch choice;
           the input (touch, or keyboard, mouse and gamepad) is one of these settings and hides only what is for touch alone -->
      <template v-if="reach(4) && touch">
        <SparxieOptions :block="optBlock" :fields="sparxieFields" />
        <MBanner v-if="!viaSparxie && !status.touchUiSwitch && KBM_SPARXIE_GAMES.includes(game)" tone="info" icon="info">{{ C.kbmNeedsTouch }}</MBanner>
        <MFooter>{{ C.noticeFoot }}</MFooter>
      </template>

      <!-- 5 · the game's own settings: they follow the game's current values until one is changed -->
      <template v-if="reach(5)">
        <SparxieDisplay :block="gameBlock" :install-id="install?.id || ''" :exe="install ? fullPath(install) : ''" :region="region" />
        <SparxieSettings :block="gameBlock" :row="catalog?.[game]" :install-id="install?.id || ''" :exe="install ? fullPath(install) : ''" :region="region" :fps-by-sparxie="fpsBySparxie" :sparxie-fps="sparxieFpsAvailable" @problem="settingsProblem = $event" />
      </template>

      <!-- 6 · name and cover: the official poster, kept up to date, or a cover of the person's own -->
      <MSection v-if="reach(6)" :title="C.secNameCover">
        <MGroup>
          <MFieldItem v-model="name" :label="C.appName" />
        </MGroup>
        <SparxieCover :form="coverForm" :touch="badgeOn" :updated="install?.art?.updated || artMap[artIdOf(install)] || ''" :art-id="install ? artIdOf(install) : ''" :art-version="install ? artMap[artIdOf(install)] || '' : ''" />
        <template v-if="coverForm.sparxie.cover === 'custom'">
          <div class="cover-pair">
          <ImageSelector kind="poster" :badge="badgeOn && coverForm.sparxie.badge" :title="t('apps.cover_portrait')" :image-path="coverForm['image-path']" :app-name="name" @update-image="(v) => { coverForm['image-path'] = v; imageError = '' }" @image-error="(e) => { imageError = e }" />
          <ImageSelector kind="hero" :badge="badgeOn && coverForm.sparxie.badge" :title="t('apps.cover_landscape')" :image-path="coverForm.hero" :app-name="name" @update-image="(v) => { coverForm.hero = v; imageError = '' }" @image-error="(e) => { imageError = e }" />
          </div>
          <MBanner v-if="imageError" tone="error" icon="error">{{ imageError }}</MBanner>
        </template>
        <MBanner v-if="isEdit && editBlock.mode === 'touch' && !touchAllowed" tone="info" icon="info">{{ C.touchNeeds }}</MBanner>
        <MBanner v-if="block === 'running'" tone="warn" icon="lock">
          {{ C.closeSparxie }}
          <template #actions><MButton variant="text" size="sm" icon="refresh" @click="recheck">{{ C.retry }}</MButton></template>
        </MBanner>
        <MBanner v-else-if="block === 'unreadable'" tone="error" icon="error">{{ C.unreadable }}</MBanner>
        <MFooter>{{ writeLine }}</MFooter>
      </MSection>
    </template>

    <!-- 创建 bar: the last thing of the page, pinned to the bottom edge of the window (the column is as tall as the window) and as wide as the content column -->
    <div class="spx-bar" role="toolbar" :aria-label="summaryText">
      <MBanner v-if="createError" class="spx-bar__error" tone="error" icon="error">{{ createError }}</MBanner>
      <div class="spx-bar__row">
        <span class="spx-bar__sum" :title="summaryText">
          <template v-for="(p, i) in summary" :key="i"><i v-if="i" aria-hidden="true">·</i><span :class="{ 'is-open': p.open }">{{ p.text }}</span></template>
        </span>
        <span class="spx-bar__act">
          <MButton variant="text" @click="$emit('close')">{{ C.cancel }}</MButton>
          <span class="spx-bar__go" :class="{ 'is-blocked': !!missing, 'is-tip': tipOpen }" @pointerdown="holdStart" @pointerup="holdEnd" @pointerleave="holdEnd" @pointercancel="holdEnd" @contextmenu.prevent>
            <MButton :icon="isEdit ? 'save' : 'add'" :disabled="!!missing || creating || unchanged" :loading="creating" :aria-describedby="missing ? 'spx-bar-tip' : undefined" @click="create">{{ isEdit ? C.save : C.create }}</MButton>
            <span v-if="missing" id="spx-bar-tip" class="spx-bar__tip" role="tooltip">{{ missing }}</span>
          </span>
        </span>
      </div>
    </div>
  </MPage>
</template>

<script setup>
import { computed, nextTick, onBeforeUnmount, onMounted, reactive, ref, watch } from 'vue'
import { useI18n } from 'vue-i18n'
import { MBanner, MButton, MChip, MEmpty, MFieldItem, MFooter, MGroup, MIcon, MItem, MPage, MSection, MSeg, MSkeleton } from 'foundation-ui/vue'
import ImageSelector from '../ImageSelector.vue'
import SparxieBrowse from './SparxieBrowse.vue'
import SparxieCover from './SparxieCover.vue'
import SparxieInstallRow from './SparxieInstallRow.vue'
import SparxieSettings from './SparxieSettings.vue'
import SparxieDisplay from './SparxieDisplay.vue'
import SparxieOptions from './SparxieOptions.vue'
import SparxieNotice from './SparxieNotice.vue'
import { useSparxie } from '../../composables/useSparxie.js'
import { useSparxieCopy } from '../../composables/useSparxieCopy.js'
import { GAME_IDS, KBM_SPARXIE_GAMES, SPARXIE_MARK, artUrl, badgeApplies, badgeOf, createBlock, defaultName, exesOf, fitsRegion, fullPath, gameArtId, matchTitle, mergeInstalls, needsWriteBack, regionsOf, withDefaults, SPARXIE_DEFAULTS, sparxieFieldsOf, stableJson } from '../../utils/sparxie/model.js'
import { nativePicker } from '../../utils/sparxie/pick.js'
import { displayBody, displayState, hasDisplay, normalizeDisplay } from '../../utils/sparxie/display.js'
import { failureText } from '../../utils/sparxie/copy.js'

// 「新建 Sparxie 应用」 follows Sparxie's own way of adding a game: the game, the region, where the install is, how it starts, the name.
// Installs the host already knows (Sparxie's profiles, the launcher registry) are offered first; two buttons add another one: the game executable, or a folder the host searches level by level from its first level.
// The game's own settings are a section of this page and of the page of an existing application (SparxieSettings): they follow the game until one is changed. Creating hands the application to the page that owns the list (it saves it).
const emit = defineEmits(['close', 'created', 'saved'])
// `edit`: a game app of the list ({ ...app, index }): the same page with the answers filled in; `prefill`: the path of a program the person chose in 添加应用, which the page identifies and starts from
const props = defineProps({ backLabel: { type: String, default: '' }, edit: { type: Object, default: null }, prefill: { type: String, default: '' } })
const isEdit = !!props.edit
const C = useSparxieCopy()
const { t, locale } = useI18n()
const { state: status, client, refresh } = useSparxie()

const page = ref(null)

const loaded = ref(false)
const stored = ref([])
const detected = ref([])
const catalog = ref(null)
const game = ref('')
const region = ref('')
const install = ref(null)
const picked = ref([])
const browsing = ref('')
const scanning = ref(false)
const pickError = ref('')
const createError = ref('')
const creating = ref(false)
const name = ref('')
const heroReady = ref(false)
const showPickers = ref(false)
const artMap = ref({})
const imageError = ref('')
// the game's own settings (only what is changed), the restore policy and the cover of the new application
const gameBlock = reactive({ game: '', settings: {}, restore: 'restore', profile: '', options: { ...SPARXIE_DEFAULTS }, display: { mode: 'follow' } })
const query = ref('')
const pendingPrefill = ref(null)
const editBlock = isEdit ? reactive(withDefaults(props.edit.sparxie)) : null
const editOptions = isEdit ? JSON.stringify(editBlock.options) : ''
// the Sparxie settings of this page: the app's own when it is edited, the new app's otherwise
const optBlock = isEdit ? editBlock : gameBlock
const coverForm = reactive({ sparxie: { cover: 'official', badge: true }, 'image-path': '', hero: '' })
let nameTouched = false

// the titles the host sends (a new title is a new row on its side), with their names
const titles = computed(() => Object.keys(catalog.value || {}).length ? Object.keys(catalog.value) : [...GAME_IDS])
const names = computed(() => {
  const out = { ...C.value.games }
  for (const [id, row] of Object.entries(catalog.value || {})) out[id] = (locale.value.toLowerCase().startsWith('zh') ? row.name : row.nameEn) || row.name || row.nameEn || out[id] || id
  return out
})
const matches = computed(() => titles.value.filter((id) => matchTitle(id, { ...catalog.value?.[id], name: names.value[id] }, query.value)))
const subtitleOf = (g) => { const row = catalog.value?.[g]; const other = locale.value.toLowerCase().startsWith('zh') ? row?.nameEn : row?.name; return other && other !== names.value[g] ? other : '' }
const titleIcon = (g) => artUrl('icon', { id: gameArtId(g) }, artMap.value[gameArtId(g)])
const copyForNames = computed(() => ({ games: names.value, regions: C.value.regions }))
// an app of a Sparxie title starts through Sparxie whenever the component is installed (no launch choice); without it the game starts directly
const touchAllowed = computed(() => !!status.installed && !!catalog.value?.[game.value]?.touch)
const touch = computed(() => touchAllowed.value)
// keyboard, mouse and gamepad runs a Sparxie session without the touch interface: a Sparxie build without that switch cannot serve it, and the host then starts the game directly
const viaSparxie = computed(() => touch.value && (optBlock.options.input !== 'kbm' || (!!status.touchUiSwitch && KBM_SPARXIE_GAMES.includes(game.value))))
// how the app starts: an app that is saved as a direct launch (mode pc) stays one when it is edited, whatever the Sparxie items on this page show; creating follows the component
const launchMode = computed(() => (viaSparxie.value ? 'touch' : 'pc'))
// the poster carries the Sparxie mark only for an application that runs Sparxie's touch interface; keyboard, mouse and gamepad (through Sparxie's PC session too) have the plain poster
const badgeOn = computed(() => badgeApplies(launchMode.value, optBlock.options.input))
const sparxieFields = computed(() => sparxieFieldsOf(catalog.value?.[game.value], game.value))
// Sparxie's frame-rate unlock takes the game's own frame rate over, whichever input is chosen
const sparxieFpsAvailable = computed(() => touch.value && sparxieFields.value.some((f) => f.key === 'fps'))
const fpsBySparxie = computed(() => viaSparxie.value && !!optBlock.options.fpsUnlock && sparxieFields.value.some((f) => f.key === 'fpsUnlock'))
const inputText = computed(() => (optBlock.options.input === 'kbm' ? C.value.inputKbm : C.value.inputTouch))
const regionOptions = computed(() => regionsOf(catalog.value, game.value).map((r) => ({ value: r, label: C.value.regions[r] })))
const exeNames = computed(() => C.value.pickExeText(exesOf(catalog.value, game.value, region.value).join(' · ')))
// every install of this game and region, from the host's own list (Sparxie's profiles, the launcher registry, a store) and from what the person chose; one found by two sources is one row with both
const entries = computed(() => mergeInstalls([{ list: stored.value, stored: true }, { list: detected.value }, { list: picked.value, manual: true }]).filter((e) => fitsRegion(e.install, game.value, region.value)))
const choices = computed(() => entries.value.map((e) => e.install))
const known = computed(() => entries.value.filter((e) => !e.sources.includes('manual') || e.sources.length > 1).map((e) => e.install))
const sourceLabel = (i) => (entries.value.find((e) => e.install === i)?.sources || []).map((k) => C.value.src[k] || k).join(' · ')
const block = computed(() => createBlock({ install: install.value, region: region.value, mode: viaSparxie.value ? 'touch' : 'pc', store: { running: status.running, state: status.store.state } }))
const writeLine = computed(() => (!viaSparxie.value ? '' : !needsWriteBack(install.value) ? C.value.writeBackNone : status.store.state === 'unused' ? C.value.writeBackNew : C.value.writeBack))
const artId = computed(() => gameArtId(game.value, region.value || 'cn'))
const bannerUrl = computed(() => artUrl('banner', { id: artId.value }, artMap.value[artId.value]))
const heroStyle = computed(() => (heroReady.value ? { backgroundImage: `url(${bannerUrl.value})` } : {}))
const pickersOpen = computed(() => showPickers.value || !choices.value.length || !!pickError.value)
const artIdOf = (i) => (i ? gameArtId(i.game, i.region || region.value || 'cn') : '')

// how far the answers allow the page to open: 1 game, 2 region, 3 install, then the launch mode, the game's settings and the name are always answerable
const maxStep = computed(() => (!game.value ? 1 : !region.value ? 2 : !install.value ? 3 : 6))
// every answered step opens the next one; the bar at the bottom of the window says what is chosen and what is still open
const reach = (n) => maxStep.value >= n

// the one line the bar shows: what is chosen, and what is still open
const summary = computed(() => {
  const parts = []
  if (!game.value) return [{ text: C.value.barNoGame, open: true }]
  parts.push({ text: names.value[game.value] })
  if (!region.value) return [...parts, { text: C.value.barNoRegion, open: true }]
  parts.push({ text: C.value.regions[region.value] })
  if (!install.value) return [...parts, { text: C.value.barNoInstall, open: true }]
  parts.push({ text: install.value.exe || fullPath(install.value) }, { text: touch.value ? inputText.value : C.value.pc })
  return parts
})
const summaryText = computed(() => summary.value.map((p) => p.text).join(' · '))
// 保存 of an existing application is available when something differs from what the page opened with (and again only after the next change); a new application is always a change
const state = () => stableJson({ name: name.value, settings: gameBlock.settings, display: hasDisplay(gameBlock.game) ? displayState(gameBlock.display) : null, restore: gameBlock.restore, cover: coverForm.sparxie.cover, badge: badgeOn.value ? coverForm.sparxie.badge : null, image: coverForm['image-path'], hero: coverForm.hero, options: isEdit && touch.value ? editBlock.options : null })
const baseline = ref('')
const unchanged = computed(() => isEdit && loaded.value && baseline.value !== '' && state() === baseline.value)
// why 创建 is not available yet
// the first value of the game settings that the host would refuse (a launch argument it does not allow, a number outside its range), said by the settings section itself
const settingsProblem = ref('')
const missing = computed(() => (!game.value ? C.value.needGame : !region.value ? C.value.needRegion : !install.value ? C.value.needInstall : block.value === 'running' ? C.value.closeSparxie : block.value === 'unreadable' ? C.value.unreadable : settingsProblem.value))
// a long press (touch) or a hover (mouse) on the blocked button says what is missing
const tipOpen = ref(false)
let holdTimer = 0
let tipTimer = 0
const holdStart = () => { if (!missing.value) return; clearTimeout(holdTimer); holdTimer = setTimeout(() => { tipOpen.value = true; clearTimeout(tipTimer); tipTimer = setTimeout(() => { tipOpen.value = false }, 2600) }, 420) }
const holdEnd = () => clearTimeout(holdTimer)
onBeforeUnmount(() => { clearTimeout(holdTimer); clearTimeout(tipTimer) })

// the section that just opened scrolls into view (the bar keeps clear of it: .m-section has a scroll margin)
function reveal() {
  nextTick(() => {
    const sections = page.value?.$el?.querySelectorAll?.('.m-section')
    const last = sections?.[sections.length - 1]
    if (last?.scrollIntoView) last.scrollIntoView({ block: 'nearest', behavior: 'smooth' })
  })
}
watch(() => [reach(2), reach(3), reach(4)].filter(Boolean).length, (now, before) => { if (now > before) reveal() })

const hide = (event) => { event.target.style.visibility = 'hidden' }
// a game is marked when the host already has an install of it: in Sparxie, or from the launcher
const knownTag = (g) => {
  const all = [...stored.value, ...detected.value].filter((i) => i.game === g)
  return all.some((i) => i.profile) ? C.value.inSparxie : all.length ? C.value.detectedTag : ''
}

// the hero appears when the official art of the chosen game has loaded
watch([game, bannerUrl], () => {
  heroReady.value = false
  if (!game.value || typeof Image === 'undefined') return
  const id = artId.value
  const probe = new Image()
  probe.onload = () => { if (artId.value === id) heroReady.value = true }
  probe.src = bannerUrl.value
})

const followAllSettings = () => { for (const k of Object.keys(gameBlock.settings)) delete gameBlock.settings[k] }
function chooseGame(g) {
  if (game.value === g) return
  game.value = g
  gameBlock.game = g
  followAllSettings()
  region.value = ''
  install.value = null
  picked.value = []
  pickError.value = ''
  createError.value = ''
  browsing.value = ''
  // the input a new application of this title starts with is the host's default for it (touch)
  gameBlock.options.input = sparxieFieldsOf(catalog.value?.[g], g).find((f) => f.key === 'input')?.def ?? SPARXIE_DEFAULTS.input
  // one region offered: no question
  const only = regionsOf(catalog.value, g)
  if (only.length === 1) region.value = only[0]
}
watch(region, () => {
  if (isEdit) return
  // the program that 添加应用 handed over is the install, once its region is the one chosen
  if (pendingPrefill.value && fitsRegion(pendingPrefill.value, game.value, region.value)) {
    picked.value = [pendingPrefill.value]
    install.value = pendingPrefill.value
    pendingPrefill.value = null
    showPickers.value = false
    return
  }
  install.value = null
  showPickers.value = false
  picked.value = []
  pickError.value = ''
  createError.value = ''
  browsing.value = ''
  // the install the host already knows is the answer unless there are several
  if (known.value.length === 1) install.value = known.value[0]
})
const select = (i) => { install.value = i; showPickers.value = false; pickError.value = ''; createError.value = '' }

// an existing game app: every answer is filled in from it, and the page changes that app
function startEdit() {
  const b = editBlock
  const exePath = String(b.exe || '')
  const inst = { id: b.install, game: b.game, region: b.region, candidates: [], markers: [], sources: [], dir: b.dir || exePath.replace(/[\\/][^\\/]*$/, ''), exe: exePath.split(/[\\/]/).pop(), version: '', biz: '', profile: b.profile || '', art: { version: '', updated: b.artUpdated || '' } }
  nameTouched = true
  game.value = b.game
  gameBlock.game = b.game
  region.value = b.region
  picked.value = [inst]
  install.value = inst
  followAllSettings()
  Object.assign(gameBlock.settings, b.settings)
  for (const k of Object.keys(gameBlock.display)) delete gameBlock.display[k]
  Object.assign(gameBlock.display, normalizeDisplay(b.display))
  gameBlock.restore = b.restore
  name.value = props.edit.name || ''
  coverForm.sparxie.cover = b.cover === 'custom' ? 'custom' : 'official'
  coverForm.sparxie.badge = badgeOf(b)
  // with the badge on, the application points at a badged copy of the person's own image; the page shows (and saves) the original
  coverForm['image-path'] = (b.cover === 'custom' && b.customImage) || props.edit['image-path'] || ''
  coverForm.hero = (b.cover === 'custom' && b.customHero) || props.edit.hero || ''
}
// the program that 添加应用 handed over: the host says which game and region it is
async function startPrefill() {
  try {
    const found = await client().identify(props.prefill)
    if (found.error || !found.install) return
    chooseGame(found.install.game)
    pendingPrefill.value = { ...found.install, sources: [...(found.install.sources || [])] }
    if (found.install.region) region.value = found.install.region
  } catch { /* the page starts empty */ }
}

async function load() {
  try {
    await refresh()
    const games = await client().games()
    stored.value = games.stored
    detected.value = games.detected
    catalog.value = games.catalog
    artMap.value = games.art
    if (isEdit) startEdit()
    else if (props.prefill) await startPrefill()
  } finally { loaded.value = true }
  if (isEdit) { await nextTick(); baseline.value = state() }
  // the official posters (also of games that are not installed) are refreshed by the host; a poster that changed is a new address, so the tiles take the new versions
  client().refreshArt().then(async (updated) => { if (updated) artMap.value = (await client().games()).art }).catch(() => {})
}
onMounted(load)

const nativeChoose = nativePicker()
async function pick(kind) {
  pickError.value = ''
  if (!nativeChoose) { browsing.value = kind; return }
  try {
    const path = await nativeChoose(kind, { title: kind === 'file' ? C.value.browseTitleFile : C.value.browseTitleFolder, exes: exesOf(catalog.value, game.value, region.value) })
    if (path) await onPath(kind, path)
  } catch { pickError.value = C.value.pickFailed }
}
const onBrowsed = async (path) => { const kind = browsing.value; browsing.value = ''; await onPath(kind, path) }

const errorText = (e) => e?.code === 'no_user_session' ? C.value.noUser : C.value.identifyFailed
function adopt(list) {
  const fit = list.filter((i) => fitsRegion(i, game.value, region.value))
  if (!fit.length) {
    const other = list.find((i) => i.region)
    pickError.value = other ? C.value.mismatchRegion(C.value.regions[other.region], C.value.regions[region.value]) : C.value.scanNone
    return
  }
  const keep = picked.value.filter((p) => !fit.some((i) => `${i.dir}\\${i.exe}`.toLowerCase() === `${p.dir}\\${p.exe}`.toLowerCase()))
  picked.value = [...keep, ...fit]
  const chosen = fit.length === 1 ? choices.value.find((i) => `${i.dir}\\${i.exe}`.toLowerCase() === `${fit[0].dir}\\${fit[0].exe}`.toLowerCase()) : null
  install.value = chosen || null
  showPickers.value = false
  if (fit.length > 1) pickError.value = ''
}
async function onPath(kind, path) {
  scanning.value = true; pickError.value = ''
  try {
    const filter = { game: game.value, region: region.value }
    if (kind === 'file') {
      const result = await client().identify(path, filter)
      if (result.error === 'wrong_game') pickError.value = C.value.wrongGame((result.allowed?.length ? result.allowed : exesOf(catalog.value, game.value, region.value)).join('、'))
      else if (result.error === 'is_sparxie') pickError.value = C.value.isSparxie
      else if (result.error) pickError.value = C.value.scanNone
      else adopt([result.install])
    } else {
      const result = await client().scan(path, filter)
      if (result.error === 'is_sparxie') pickError.value = C.value.isSparxie
      else if (result.error) pickError.value = result.timedOut ? C.value.scanTimeout : C.value.scanNone
      else adopt(result.installs)
    }
  } catch (e) { pickError.value = errorText(e) } finally { scanning.value = false }
}

// the name follows the game and the region until the person has typed their own
watch([install, region, names], () => { if (!nameTouched) name.value = defaultName(install.value, region.value, copyForNames.value) }, { immediate: true })
watch(name, (v) => { if (v !== defaultName(install.value, region.value, copyForNames.value)) nameTouched = true })

async function recheck() { await refresh() }

// the game's frame rate is Sparxie's while the unlock is on: it is not written
const settingsBody = () => { const out = { ...gameBlock.settings }; if (fpsBySparxie.value) delete out.fps; return out }
async function create() {
  if (!install.value || missing.value || creating.value || unchanged.value) return
  creating.value = true; createError.value = ''
  try {
    if (isEdit) {
      const body = { name: name.value || props.edit.name, mode: launchMode.value, settings: settingsBody(), display: displayBody(gameBlock.display), restore: gameBlock.restore, cover: coverForm.sparxie.cover, 'image-path': coverForm['image-path'], hero: coverForm.hero }
      if (badgeOn.value) body.badge = coverForm.sparxie.badge
      // Sparxie's own options are written to its config only when they were changed
      if (touch.value && JSON.stringify(editBlock.options) !== editOptions) body.options = { ...editBlock.options }
      const saved = await client().updateApp(props.edit.index, body)
      baseline.value = state()
      await refresh()
      emit('saved', saved)
      return
    }
    const created = await client().createApp({ install: install.value.id || '', path: fullPath(install.value), region: region.value, mode: launchMode.value, name: name.value || defaultName(install.value, region.value, copyForNames.value), settings: settingsBody(), display: displayBody(gameBlock.display), restore: gameBlock.restore, cover: coverForm.sparxie.cover, badge: coverForm.sparxie.badge, image: coverForm['image-path'], hero: coverForm.hero, options: touch.value ? { ...gameBlock.options } : undefined })
    if (!status.warned) await client().acknowledgeNotice().catch(() => {})
    await refresh()
    emit('created', created)
  } catch (e) {
    if (e?.code === 'sparxie_running') { status.running = true; await refresh() }
    createError.value = failureText(C.value, isEdit ? C.value.saveFailed : C.value.createFailed, e)
  } finally { creating.value = false }
}
</script>

<style scoped>
.spx-search { margin-bottom: 8px; }
.spx-icon-box { position: relative; display: grid; place-items: center; width: 40px; height: 40px; border-radius: 10px; background: var(--md-sys-color-surface-container-highest); color: var(--md-sys-color-on-surface-variant); }
.spx-icon { position: absolute; inset: 0; width: 40px; height: 40px; border-radius: 10px; object-fit: cover; display: block; }
.spx-title :deep(.m-item__lead:has(.spx-icon-box)) { width: 40px; height: 40px; }
.spx-check { color: var(--md-sys-color-primary); }
.spx-new__hero { position: relative; min-height: 190px; border-radius: 28px; background: center 18% / cover no-repeat; display: flex; align-items: flex-end; padding: 16px; overflow: hidden; margin: 12px 0 4px; }
.spx-new__chips { display: flex; gap: 8px; flex-wrap: wrap; }

/* a section that opens scrolls clear of the bar */
.spx-new :deep(.m-section) { scroll-margin: 12px 0 112px; }

/* the column is as tall as the window, so the bar's auto margin puts it on the bottom edge even when the page is short; sticky keeps it there when the page is long.
   The page ends in 48px of padding: the bar's negative bottom margin makes it flush with the end of the page. */
.spx-new :deep(.m-page__column) { min-height: calc(100dvh - 96px); }
.spx-bar { position: sticky; bottom: 0; z-index: 8; margin: auto 0 -48px; display: flex; flex-direction: column; gap: 8px; padding: 10px 12px calc(10px + env(safe-area-inset-bottom, 0px)) 24px; border-radius: 28px 28px 0 0; background: var(--md-sys-color-surface-container-high); box-shadow: 0 -1px 0 var(--md-sys-color-outline-variant), 0 -8px 20px rgb(0 0 0 / .08); }
.spx-bar__error { margin: 4px 12px 0 0; }
.spx-bar__row { display: flex; align-items: center; gap: 12px; min-height: 48px; }
.spx-bar__sum { flex: 1; min-width: 0; overflow: hidden; text-overflow: ellipsis; white-space: nowrap; font: var(--m-label-large); color: var(--md-sys-color-on-surface); }
.spx-bar__sum i { font-style: normal; margin: 0 .5em; color: var(--md-sys-color-outline); }
.spx-bar__sum .is-open { color: var(--md-sys-color-on-surface-variant); }
.spx-bar__act { display: flex; align-items: center; gap: 8px; flex: 0 0 auto; }
.spx-bar :deep(.m-btn) { min-height: 48px; min-width: 48px; }
.spx-bar__go { position: relative; display: inline-flex; }
/* a disabled button takes no pointer events, so the wrapper answers the hover and the long press */
.spx-bar__go.is-blocked :deep(.m-btn) { pointer-events: none; }
.spx-bar__tip { position: absolute; right: 0; bottom: calc(100% + 8px); max-width: 260px; width: max-content; padding: 8px 12px; border-radius: 12px; background: var(--md-sys-color-inverse-surface); color: var(--md-sys-color-inverse-on-surface); font: var(--m-body-small); opacity: 0; transform: translateY(4px); pointer-events: none; transition: opacity .12s, transform .12s; }
.spx-bar__go.is-blocked:hover .spx-bar__tip, .spx-bar__go.is-tip .spx-bar__tip { opacity: 1; transform: none; }
@container (max-width: 560px) {
  .spx-bar { padding-left: 16px; }
  .spx-bar__row { flex-wrap: wrap; gap: 4px 8px; }
  .spx-bar__sum { flex: 1 1 100%; }
  .spx-bar__act { flex: 1 1 100%; }
  .spx-bar__go { flex: 1; }
  .spx-bar__go :deep(.m-btn) { width: 100%; }
}
@media (max-width: 599.98px) {
  .spx-bar { padding-left: 16px; }
  .spx-bar__row { flex-wrap: wrap; gap: 4px 8px; }
  .spx-bar__sum { flex: 1 1 100%; }
  .spx-bar__act { flex: 1 1 100%; }
  .spx-bar__go { flex: 1; }
  .spx-bar__go :deep(.m-btn) { width: 100%; }
}

/* the hero opens by height, softly: no jump */
.spx-hero { display: grid; grid-template-rows: 0fr; transition: grid-template-rows .28s ease; }
.spx-hero.is-open { grid-template-rows: 1fr; }
.spx-hero__clip { min-height: 0; overflow: hidden; }
@media (prefers-reduced-motion: reduce) { .spx-hero { transition: none; } }
</style>

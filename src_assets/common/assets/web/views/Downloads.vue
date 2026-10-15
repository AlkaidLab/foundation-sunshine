<script setup>
import { computed, onBeforeUnmount, onMounted, ref, watch } from 'vue'
import { useI18n } from 'vue-i18n'
import { downloadMessages } from '../i18n/downloads/index.js'
import english from '../i18n/downloads/en.json'
import Navbar from '../components/layout/Navbar.vue'
import ConfirmDialog from '../components/common/ConfirmDialog.vue'
import { openExternalUrl } from '../utils/helpers.js'
import { CLIENT_DEVICE_GROUPS, CLIENT_FEATURE_ICONS, CLIENT_PLATFORMS } from '../config/clientDownloads.js'


const { locale } = useI18n({ useScope: 'global' })
const { t, tm, rt } = useI18n({ useScope: 'local', messages: downloadMessages, fallbackLocale: 'en' })
const isEmbeddedGui = window.isTauri === true && window.parent !== window
const selectedPlatform = ref(CLIENT_PLATFORMS[0])
const selectedGroup = ref(CLIENT_DEVICE_GROUPS[0])
const groupPlatforms = computed(() => selectedGroup.value.platforms.map((id) => CLIENT_PLATFORMS.find((platform) => platform.id === id)))
const downloadClient = ref(CLIENT_PLATFORMS[0])
const supportsQr = (client) => ['android', 'harmony', 'ios'].includes(client.id)
const showQr = computed(() => supportsQr(downloadClient.value))
const selectedClients = computed(() => [selectedPlatform.value, ...(selectedPlatform.value.alternatives || [])]
  .sort((first, second) => Number(Boolean(second.recommended)) - Number(Boolean(first.recommended))))
const menuElements = ref([])
const showDownload = ref(false)
const downloadPending = ref(false)
const qrCode = ref('')
const downloadUrls = ref(null)
const openFailed = ref(false)
let downloadRequestId = 0

const isChinese = computed(() => String(locale.value).toLowerCase().startsWith('zh'))
const copy = computed(() => ({
  ...Object.fromEntries(Object.keys(english.ui).filter(key => !Array.isArray(english.ui[key])).map(key => [key, t(`ui.${key}`)])),
  setupTitles: tm('ui.setupTitles').map(message => rt(message)),
  setupSteps: tm('ui.setupSteps').map(message => rt(message)),
}))

const platformName = (platform) => t(`${platform.platforms ? 'groups' : 'platforms'}.${platform.id}${platform.platforms ? '' : '.name'}`)
const productName = (platform) => t(`platforms.${platform.id}.product`)
const platformDetail = (platform) => t(`platforms.${platform.id}.detail`)
const clientIntroduction = (client) => {
  const key = `platforms.${client.id}`
  const captions = english.platforms[client.id].captions ? tm(`${key}.captions`) : []
  return {
    description: t(`${key}.description`),
    features: tm(`${key}.features`).map(message => rt(message)),
    captions: Array.isArray(captions) ? captions.map(message => rt(message)) : [],
    note: t(`${key}.note`),
  }
}
const platformIcon = (platform) => [
  ['fa-mobile-screen-button', 'fa-tv', 'fa-vr-cardboard'].includes(platform.icon) ? 'fas' : 'fab',
  platform.icon,
]
const featureIcon = (client, index) => CLIENT_FEATURE_ICONS[client.id][index] || 'fa-check'
const availableUrls = (client) => client.getDownloadUrls(isChinese.value ? 'zh' : 'en')
const closeMenus = () => menuElements.value.forEach((menu) => { menu.open = false })
const onOutsideClick = (event) => {
  if (!menuElements.value.some((menu) => menu.contains(event.target))) closeMenus()
}
const onMenuKeydown = (event) => {
  if (event.key !== 'Escape') return
  const openMenu = menuElements.value.find((menu) => menu.open)
  if (!openMenu) return
  closeMenus()
  openMenu.querySelector('summary')?.focus()
}
const downloadLabel = computed(() => {
  if (downloadClient.value.kind === 'store') return copy.value.openStore
  return /\/releases\/(?:latest\/)?download\//.test(downloadUrls.value?.primary || '')
    ? copy.value.download : copy.value.openPage
})

const selectPlatform = (platform) => {
  closeMenus()
  closeDownload()
  selectedPlatform.value = platform
}

const selectGroup = (group) => {
  selectedGroup.value = group
  selectPlatform(CLIENT_PLATFORMS.find((platform) => platform.id === group.platforms[0]))
}

const openDownload = async (platform, channel = 'primary') => {
  closeMenus()
  const requestId = ++downloadRequestId
  downloadClient.value = platform
  const language = isChinese.value ? 'zh' : 'en'
  showDownload.value = true
  downloadPending.value = true
  qrCode.value = ''
  downloadUrls.value = null
  openFailed.value = false

  try {
    const urls = platform.resolveDownloadUrls
      ? await platform.resolveDownloadUrls(language)
      : platform.getDownloadUrls(language)
    if (requestId !== downloadRequestId) return
    downloadUrls.value = channel === 'alternate'
      ? { primary: urls.alternate, alternate: urls.primary }
      : urls
    if (!supportsQr(platform)) return
    const { default: QRCode } = await import('qrcode')
    if (requestId !== downloadRequestId) return
    const qr = await QRCode.toDataURL(downloadUrls.value.primary, {
      errorCorrectionLevel: 'M',
      margin: 4,
      width: 256,
    })
    if (requestId === downloadRequestId) qrCode.value = qr
  } catch (error) {
    console.error('Failed to prepare client download:', error)
  } finally {
    if (requestId === downloadRequestId) downloadPending.value = false
  }
}

const download = async () => {
  if (!downloadUrls.value?.primary) return
  const requestId = downloadRequestId
  openFailed.value = false
  try {
    const opened = await openExternalUrl(downloadUrls.value.primary)
    if (requestId === downloadRequestId) openFailed.value = opened === false
  } catch (error) {
    if (requestId === downloadRequestId) openFailed.value = true
    console.error('Failed to open client download URL:', error)
  }
}

const closeDownload = () => {
  ++downloadRequestId
  showDownload.value = false
  downloadPending.value = false
  downloadUrls.value = null
  qrCode.value = ''
  openFailed.value = false
}
watch(locale, () => {
  closeMenus()
  closeDownload()
})
onMounted(() => {
  document.addEventListener('click', onOutsideClick)
  document.addEventListener('keydown', onMenuKeydown)
})
onBeforeUnmount(() => {
  closeDownload()
  document.removeEventListener('click', onOutsideClick)
  document.removeEventListener('keydown', onMenuKeydown)
})
</script>

<template>
  <div class="clients-page">
    <Navbar v-show="!isEmbeddedGui" />

    <main id="content" class="clients-content container">
      <h1 class="clients-page-title">
        <i class="fas fa-download" aria-hidden="true"></i>
        <span>{{ copy.title }}</span>
      </h1>
      <section class="clients-guide" aria-labelledby="clients-guide-title">
        <div class="clients-selector">
          <div class="clients-step-heading">
            <h2 id="clients-guide-title"><i class="fas fa-laptop" aria-hidden="true"></i>{{ copy.step }}</h2>
          </div>
          <div class="clients-device-groups" role="group" :aria-label="copy.step">
            <button
              v-for="group in CLIENT_DEVICE_GROUPS"
              :key="group.id"
              class="clients-platform clients-device-group"
              :class="{ 'clients-platform-active': selectedGroup.id === group.id }"
              type="button"
              :aria-pressed="selectedGroup.id === group.id"
              @click="selectGroup(group)"
            >
              <i :class="['fas', group.icon, 'clients-device-icon']" aria-hidden="true"></i>
              <span class="clients-device-name">{{ platformName(group) }}</span>
              <i v-if="selectedGroup.id === group.id" class="fas fa-check clients-device-check" aria-hidden="true"></i>
            </button>
          </div>
          <div class="clients-system-row">
            <h3 class="clients-system-label">{{ copy.system }}</h3>
            <div class="clients-platforms" role="group" :aria-label="copy.system" :style="{ '--platform-count': groupPlatforms.length }">
              <button
                v-for="platform in groupPlatforms"
                :key="platform.id"
                class="clients-platform"
                :class="{ 'clients-platform-active': selectedPlatform.id === platform.id }"
                type="button"
                :aria-pressed="selectedPlatform.id === platform.id"
                @click="selectPlatform(platform)"
              >
                <i :class="platformIcon(platform)" aria-hidden="true"></i>
                <span>{{ platformName(platform) }}</span>
                <span v-if="platform.isNew" class="clients-new-badge clients-platform-new">{{ copy.newLabel }}</span>
              </button>
            </div>
          </div>
        </div>
        <div class="clients-results">
          <h2 class="visually-hidden">{{ copy.clientLabel }}</h2>
          <div class="clients-result-cards">
            <article v-for="client in selectedClients" :key="client.id" class="clients-card">
              <div class="clients-card-header">
                <span class="clients-product-mark" aria-hidden="true"><i :class="platformIcon(client)"></i></span>
                <div class="clients-card-copy">
                  <div class="clients-product-title">
                    <h3 class="clients-product-heading">{{ productName(client) }}</h3>
                    <div class="clients-card-labels">
                      <span v-if="client.recommended" class="clients-recommended-label">{{ copy.recommended }}</span>
                      <span v-if="client.id === 'macos-enhanced'" class="clients-edition-label">{{ copy.enhanced }}</span>
                      <span v-if="client.edition === 'standard'" class="clients-edition-label">{{ copy.standard }}</span>
                      <span v-if="client.friendLink" class="clients-friend-label">{{ copy.friendLink }}</span>
                      <span v-if="client.isNew && !client.platformNew" class="clients-new-badge">{{ copy.newLabel }}</span>
                    </div>
                  </div>
                  <p class="clients-core-value">{{ clientIntroduction(client).description }}</p>
                  <p class="clients-platform-detail">{{ platformDetail(client) }}</p>
                </div>
              </div>
              <div class="clients-card-actions">
                <button type="button" class="btn btn-primary clients-primary-button" @click="openDownload(client)">
                  <i class="fas fa-download" aria-hidden="true"></i>{{ copy.getApp }}
                </button>
                <details v-if="client.project || client.release || availableUrls(client).alternate" ref="menuElements" class="clients-more">
                  <summary>{{ copy.more }} <i class="fas fa-chevron-down" aria-hidden="true"></i></summary>
                  <div class="clients-more-menu">
                    <a v-if="client.project" :href="client.project" target="_blank" rel="noopener noreferrer" @click="closeMenus">{{ copy.project }} ↗</a>
                    <a v-if="client.release" :href="client.release" target="_blank" rel="noopener noreferrer" @click="closeMenus">{{ copy.releases }} ↗</a>
                    <a v-if="availableUrls(client).alternate" :href="availableUrls(client).alternate" @click.prevent="openDownload(client, 'alternate')">{{ copy.mirror }} ↗</a>
                  </div>
                </details>
              </div>
              <ul class="clients-features" :style="{ '--feature-count': clientIntroduction(client).features.length }">
                <li v-for="(feature, index) in clientIntroduction(client).features" :key="feature" class="clients-feature" :title="clientIntroduction(client).captions?.[index]">
                  <i :class="['fas', featureIcon(client, index)]" aria-hidden="true"></i>
                  <span>{{ feature }}</span>
                </li>
              </ul>
              <p v-if="clientIntroduction(client).note" class="clients-card-note">{{ clientIntroduction(client).note }}</p>
            </article>
          </div>
        </div>
        <div class="clients-setup">
          <div class="clients-setup-heading">
            <h2><i class="fas fa-gamepad" aria-hidden="true"></i>{{ copy.setup }}</h2>
          </div>
          <ol>
            <li v-for="(step, index) in copy.setupSteps" :key="index">
              <span class="clients-step-number" aria-hidden="true">{{ index + 1 }}</span>
              <span class="clients-step-copy"><strong>{{ copy.setupTitles[index] }}</strong><span>{{ step }}</span></span>
              <i v-if="index < copy.setupSteps.length - 1" class="fas fa-chevron-right clients-step-arrow" aria-hidden="true"></i>
            </li>
          </ol>
        </div>
      </section>
    </main>

    <ConfirmDialog
      :show="showDownload"
      dialog-id="client-download"
      :title="productName(downloadClient)"
      title-icon="fas fa-download"
      :close-label="copy.close"
      max-width="420px"
      @close="closeDownload"
    >
      <div class="clients-download-body">
        <p>{{ platformDetail(downloadClient) }}</p>
        <div v-if="downloadPending" class="clients-download-pending" :class="{ 'clients-qr-placeholder': showQr }" aria-live="polite">
          {{ copy.resolving }}
        </div>
        <img v-else-if="qrCode" class="clients-qr" :src="qrCode" :alt="copy.scan" width="160" height="160" />
        <p v-if="qrCode" class="clients-qr-hint">{{ copy.scan }}</p>
        <p v-else-if="!downloadPending && !downloadUrls" role="alert">{{ copy.resolveFailed }}</p>
        <p v-else-if="!downloadPending && showQr" class="clients-qr-failed">{{ copy.failed }}</p>
        <p v-if="!showQr && !downloadPending && downloadUrls">{{ copy.desktopHint }}</p>
        <div v-if="openFailed" class="clients-open-error" role="alert">
          <p>{{ copy.openFailed }}</p>
          <input class="form-control" type="text" readonly :value="downloadUrls.primary" :aria-label="copy.downloadAddress" @focus="$event.target.select()" />
        </div>
      </div>
      <template #actions>
        <button type="button" class="btn btn-primary clients-primary-button clients-modal-download" :disabled="downloadPending || !downloadUrls?.primary" @click="download">
          <i class="fas fa-download" aria-hidden="true"></i>
          {{ downloadLabel }}
        </button>
      </template>
    </ConfirmDialog>
  </div>
</template>

<style lang="less">
@import '../styles/global.less';

.confirm-dialog-panel[aria-labelledby='client-download-title'] {
  background: linear-gradient(var(--ui-surface-strong), var(--ui-surface-strong)), var(--ui-page-bg);
  box-shadow: var(--ui-shadow-lg);
}
</style>

<style scoped>
.clients-page,
.clients-download-body {
  --clients-copy-color: color-mix(in srgb, var(--ui-text-secondary) 80%, var(--ui-text-primary));
  --clients-meta-color: color-mix(in srgb, var(--ui-text-secondary) 90%, var(--ui-text-primary));
}

.clients-page {
  min-height: 100vh;
  color: var(--ui-text-primary);
}

.clients-content {
  max-width: 1180px;
  padding-top: 1.25rem;
  padding-bottom: 1.5rem;
}

.clients-page-title {
  display: flex;
  align-items: center;
  gap: 0.75rem;
  margin: 0 0 1.25rem;
  color: var(--ui-text-primary);
  font-size: clamp(1.625rem, 3vw, 2rem);
  font-weight: 650;
  line-height: 1.3;
}

.clients-page-title i {
  color: var(--ui-accent);
  font-size: 0.85em;
}

.clients-guide {
  container-type: inline-size;
  container-name: client-selector;
  padding: 1rem;
  border: 1px solid var(--ui-border);
  border-radius: var(--ui-radius-lg);
  background: var(--ui-surface);
  box-shadow: var(--ui-shadow-md);
}

.clients-step-heading {
  display: flex;
  flex-wrap: wrap;
  align-items: baseline;
  gap: 0.3rem 0.75rem;
  margin-bottom: 0.75rem;
}

.clients-step-heading h2,
.clients-setup h2,
.clients-system-label {
  display: flex;
  align-items: center;
  gap: 0.5rem;
  margin: 0;
  color: var(--ui-text-primary);
  font-size: 1rem;
  font-weight: 600;
  line-height: 1.4;
}

.clients-step-heading h2 i,
.clients-setup h2 i {
  color: var(--ui-accent-text);
}

.clients-device-groups {
  display: grid;
  grid-template-columns: repeat(4, minmax(0, 1fr));
  gap: 0.65rem;
}

.clients-platform {
  --clients-option-base: color-mix(in srgb, var(--ui-page-bg) 75%, transparent);
  position: relative;
  display: flex;
  flex-wrap: wrap;
  align-items: center;
  justify-content: center;
  gap: 0.45rem;
  min-width: 0;
  min-height: 38px;
  padding: 0.45rem 0.75rem;
  border: 1px solid var(--ui-border);
  border-radius: var(--ui-radius-sm);
  background: linear-gradient(var(--ui-surface-strong), var(--ui-surface-strong)), var(--clients-option-base);
  -webkit-backdrop-filter: blur(10px);
  backdrop-filter: blur(10px);
  color: var(--ui-text-primary);
  font: inherit;
  font-size: 0.875rem;
  font-weight: 500;
  line-height: 1.4;
  cursor: pointer;
  transition: border-color 0.15s ease, background-color 0.15s ease;
}

.clients-platform:not(.clients-platform-active):hover {
  border-color: var(--ui-border-strong);
  background: linear-gradient(var(--ui-surface-hover), var(--ui-surface-hover)), var(--clients-option-base);
}

.clients-platform-active {
  border-color: var(--ui-accent);
  background: linear-gradient(var(--ui-accent-soft), var(--ui-accent-soft)),
    linear-gradient(var(--ui-surface-strong), var(--ui-surface-strong)), var(--clients-option-base);
  box-shadow: inset 0 0 0 1px var(--ui-accent-soft);
  color: var(--ui-accent-text);
}

.clients-device-group {
  flex-wrap: nowrap;
  justify-content: center;
  gap: 0.75rem;
  min-height: 56px;
  padding: 0.75rem;
  text-align: left;
}

.clients-device-icon {
  flex: 0 0 30px;
  font-size: 1.5rem;
  text-align: center;
}

.clients-device-name {
  min-width: 0;
  font-size: 1rem;
  font-weight: 600;
}

.clients-device-check {
  position: absolute;
  top: 6px;
  right: 7px;
  color: var(--ui-accent-text);
  font-size: 0.65rem;
}

.clients-system-row {
  display: grid;
  gap: 0.6rem;
  margin-top: 1rem;
}

.clients-platforms {
  display: grid;
  grid-template-columns: repeat(var(--platform-count), minmax(0, 1fr));
  gap: 0.65rem;
  max-width: 38rem;
}

.clients-new-badge {
  display: inline-flex;
  flex-shrink: 0;
  padding: 3px 5px;
  border-radius: 4px;
  background: #c62828;
  color: white;
  font-size: 0.75rem;
  font-weight: 600;
  line-height: 1;
}

.clients-platform-new {
  pointer-events: none;
}

.clients-results {
  margin-top: 1rem;
}

.clients-result-cards {
  display: grid;
  grid-template-columns: minmax(0, 1fr);
  gap: 0.75rem;
}

.clients-card {
  position: relative;
  min-width: 0;
  padding: 1rem;
  border: 1px solid var(--ui-border);
  border-radius: var(--ui-radius-md);
  background: var(--ui-surface-strong);
}

.clients-card:has(.clients-more[open]) {
  z-index: 2;
}

.clients-card-header {
  display: flex;
  align-items: flex-start;
  gap: 1rem;
}

.clients-product-mark {
  display: grid;
  flex: 0 0 54px;
  height: 54px;
  place-items: center;
  border-radius: var(--ui-radius-md);
  background: var(--ui-accent-soft);
  color: var(--ui-accent-text);
  font-size: 1.65rem;
}

.clients-card-copy {
  min-width: 0;
}

.clients-product-title {
  display: flex;
  flex-wrap: wrap;
  align-items: center;
  gap: 0.45rem 0.75rem;
}

.clients-product-heading {
  margin: 0;
  color: var(--ui-text-primary);
  font-size: 1.375rem;
  font-weight: 650;
  letter-spacing: -0.025em;
  line-height: 1.35;
  overflow-wrap: anywhere;
}

.clients-card-labels {
  display: flex;
  flex-wrap: wrap;
  align-items: center;
  gap: 0.45rem;
}

.clients-edition-label,
.clients-friend-label,
.clients-recommended-label {
  padding: 3px 6px;
  border: 1px solid var(--ui-border);
  border-radius: 4px;
  color: var(--clients-copy-color);
  font-size: 0.75rem;
  line-height: 1.3;
}

.clients-recommended-label {
  border-color: var(--ui-border-strong);
  background: var(--ui-accent-soft);
  color: var(--ui-accent-text);
  font-weight: 600;
}

.clients-edition-label,
.clients-friend-label {
  background: var(--ui-surface);
}

.clients-core-value {
  margin: 0.45rem 0 0;
  color: var(--clients-copy-color);
  font-size: 0.9375rem;
  line-height: 1.65;
}

.clients-platform-detail {
  margin: 0.3rem 0 0;
  color: var(--clients-meta-color);
  font-size: 0.8125rem;
  line-height: 1.6;
}

.clients-card-actions {
  display: flex;
  flex-wrap: wrap;
  align-items: center;
  gap: 0.5rem 1rem;
  margin: 0.75rem 0 0 70px;
}

.clients-primary-button {
  display: inline-flex;
  align-items: center;
  justify-content: center;
  gap: 0.5rem;
  min-height: 40px;
  padding: 0.5rem 1.25rem;
  border-radius: var(--ui-radius-sm);
  font-family: inherit;
  font-size: 0.9375rem;
  font-weight: 600;
}

.clients-primary-button.btn-primary {
  --clients-button-bg: color-mix(in srgb, var(--ui-accent) 60%, var(--ui-text-primary));
  border-color: var(--clients-button-bg);
  background: var(--clients-button-bg);
  color: var(--ui-accent-contrast);
}

.clients-primary-button.btn-primary:not(:disabled):is(:hover, :focus-visible, :active) {
  border-color: var(--clients-button-bg);
  background: var(--clients-button-bg);
  color: var(--ui-accent-contrast);
  box-shadow: 0 0 0 3px var(--ui-accent-soft);
}

.clients-card-actions > .clients-primary-button {
  min-width: 150px;
}

.clients-more {
  position: relative;
}

.clients-more summary {
  display: inline-flex;
  align-items: center;
  justify-content: center;
  gap: 0.5rem;
  min-height: 40px;
  padding: 0.4rem 0.2rem;
  border-radius: var(--ui-radius-sm);
  color: var(--ui-accent-text);
  font-size: 0.875rem;
  font-weight: 500;
  list-style: none;
  cursor: pointer;
}

.clients-more summary:hover {
  color: var(--ui-text-primary);
}

.clients-more summary::-webkit-details-marker {
  display: none;
}

.clients-more-menu {
  position: absolute;
  z-index: 10;
  top: calc(100% + 4px);
  left: 0;
  display: grid;
  min-width: 9rem;
  padding: 5px;
  border: 1px solid var(--ui-border);
  border-radius: var(--ui-radius-sm);
  background: linear-gradient(var(--ui-surface-strong), var(--ui-surface-strong)), var(--ui-page-bg);
  box-shadow: var(--ui-shadow-md);
  backdrop-filter: blur(20px);
}

.clients-more-menu a {
  padding: 8px;
  border-radius: 4px;
  color: var(--ui-text-primary);
  font-size: 0.875rem;
  text-decoration: none;
  white-space: nowrap;
}

.clients-more-menu a:hover {
  background: var(--ui-surface-hover);
}

.clients-features {
  display: grid;
  grid-template-columns: repeat(var(--feature-count), minmax(0, 1fr));
  gap: 0.6rem;
  margin: 0.85rem 0 0;
  padding: 0;
  list-style: none;
}

.clients-feature {
  display: flex;
  align-items: center;
  gap: 0.5rem;
  min-width: 0;
  min-height: 36px;
  padding: 0.45rem 0.65rem;
  border: 1px solid var(--ui-border);
  border-radius: var(--ui-radius-sm);
  background: var(--ui-accent-soft);
  color: var(--ui-text-primary);
  font-size: 0.875rem;
  font-weight: 500;
  line-height: 1.5;
}

.clients-feature i {
  flex-shrink: 0;
  color: var(--ui-accent-text);
  font-size: 1rem;
}

.clients-card-note {
  margin: 0.65rem 0 0;
  color: var(--clients-meta-color);
  font-size: 0.8125rem;
  line-height: 1.6;
}

.clients-setup {
  margin-top: 1.1rem;
  padding: 1rem;
  border: 1px solid var(--ui-border);
  border-radius: var(--ui-radius-md);
  background: var(--ui-surface-strong);
}

.clients-setup-heading {
  margin-bottom: 0.8rem;
}

.clients-setup ol {
  display: grid;
  grid-template-columns: repeat(3, minmax(0, 1fr));
  gap: 1.5rem;
  list-style: none;
  padding: 0;
  margin: 0;
}

.clients-setup li {
  position: relative;
  display: flex;
  align-items: center;
  gap: 0.65rem;
  min-width: 0;
  padding: 0.75rem;
  border: 1px solid var(--ui-border);
  border-radius: var(--ui-radius-sm);
  background: var(--ui-surface);
}

.clients-step-number {
  display: grid;
  flex: 0 0 38px;
  height: 38px;
  place-items: center;
  border-radius: 50%;
  background: var(--ui-accent-soft);
  color: var(--ui-accent-text);
  font-size: 1.25rem;
  font-weight: 600;
}

.clients-step-copy {
  display: grid;
  gap: 0.2rem;
  min-width: 0;
  font-size: 0.8125rem;
  line-height: 1.6;
  color: var(--clients-copy-color);
}

.clients-step-copy strong {
  color: var(--ui-text-primary);
  font-size: 0.875rem;
  font-weight: 600;
}

.clients-step-arrow {
  position: absolute;
  right: -1rem;
  color: var(--ui-text-muted);
  font-size: 0.75rem;
}

.clients-download-body {
  text-align: center;
}

.clients-download-body p {
  margin: 0.4rem 0;
  color: var(--clients-copy-color);
  font-size: 0.9375rem;
  line-height: 1.6;
}

.clients-download-pending {
  padding: 1rem 0;
  color: var(--ui-text-secondary);
}

.clients-qr,
.clients-qr-placeholder {
  display: grid;
  width: min(160px, 100%);
  aspect-ratio: 1;
  height: auto;
  margin: 1rem auto 0.5rem;
  place-items: center;
  background: white;
}

.clients-qr-placeholder {
  color: #46566b;
  font-size: 0.85rem;
}

.clients-modal-download {
  width: 100%;
}

.clients-platform:focus-visible,
.clients-primary-button:focus-visible,
.clients-more summary:focus-visible,
.clients-more-menu a:focus-visible {
  outline: 2px solid var(--ui-accent);
  outline-offset: 3px;
}

@container client-selector (max-width: 820px) {
  .clients-features {
    grid-template-columns: repeat(2, minmax(0, 1fr));
  }
}

@container client-selector (max-width: 660px) {
  .clients-device-groups {
    grid-template-columns: repeat(2, minmax(0, 1fr));
  }

  .clients-platforms {
    max-width: none;
  }

  .clients-setup ol {
    grid-template-columns: minmax(0, 1fr);
    gap: 0.8rem;
  }

  .clients-step-arrow {
    display: none;
  }
}

@media (max-width: 576px) {
  .clients-content {
    padding: 1rem 0.75rem;
  }

  .clients-guide {
    padding: 0.75rem;
  }

  .clients-card {
    padding: 0.75rem;
  }

  .clients-setup {
    padding: 0.75rem;
  }

  .clients-card-header {
    gap: 0.65rem;
  }

  .clients-product-mark {
    flex-basis: 42px;
    height: 42px;
    font-size: 1.35rem;
  }

  .clients-product-heading {
    font-size: 1.25rem;
  }

  .clients-card-actions {
    margin-left: 0;
  }

  .clients-platforms .clients-platform {
    padding-inline: 0.35rem;
  }

  .clients-feature {
    padding: 0.45rem;
    font-size: 0.8125rem;
  }
}

@media (prefers-reduced-motion: reduce) {
  .clients-platform {
    transition: none;
  }
}
</style>

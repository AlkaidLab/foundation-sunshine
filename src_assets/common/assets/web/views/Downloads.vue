<script setup>
import QRCode from 'qrcode'
import { computed, nextTick, onMounted, reactive, ref, watch } from 'vue'
import { useI18n } from 'vue-i18n'
import { MButton, MChipSet, MClientCard, MClientTag, MDialog, MFooter, MGroup, MIcon, MItem, MPage, MSection } from 'foundation-ui/vue'
import { CLIENTS, CLIENT_OS, OS_ICON, clientSections, clientTags, clientTexts, clientUrl, clientsForOs, connectTexts, mirrorName, readOs, writeOs, osChipName, osName, osOfClient } from 'foundation-ui/clients'
import { MClientAssets, useReleaseAssets } from 'foundation-ui/vue'
import { fetchReleaseAssetTypes } from '../utils/clientReleaseDownloads.js'
import { extraLinks, fileGroups, targetName, useClientDialog } from '../utils/clientDialog.js'
import { downloadMessages } from '../i18n/downloads/index.js'
import english from '../i18n/downloads/en.json'
import { openExternalUrl } from '../utils/helpers.js'
import { CLIENT_DEVICE_GROUPS, CLIENT_FEATURE_ICONS, CLIENT_PLATFORMS } from '../config/clientDownloads.js'
import { PARTNER_CARDS, PROJECT_LINKS, communityTexts, linkLabel, resolveLink } from '../config/communityLinks.js'

const { locale } = useI18n({ useScope: 'global' })
const { t, tm, rt } = useI18n({ useScope: 'local', messages: downloadMessages, fallbackLocale: 'en' })
const { t: tg } = useI18n({ useScope: 'global' })
// other links of a client: a title of the global messages (or a name that is the same in every language) and a short destination
const linkTitle = (link) => (link.titleKey ? tg(link.titleKey) : link.title)
const linkDetail = (link) => (link.detailKey ? tg(link.detailKey) : link.detail)

const GROUP_ICONS = { phone: 'mobile', computer: 'desktop_windows', tv: 'tv', other: 'sports_esports' }
const groups = computed(() => CLIENT_DEVICE_GROUPS.map((group) => ({
  ...group,
  platforms: group.platforms.map((id) => CLIENT_PLATFORMS.find((platform) => platform.id === id)),
})))

// The list comes from the one client list of the package (foundation-ui/clients): the clients of the Foundation team first, then the partners', in a fixed order.
// Without a filter: the two sections. Filtered by a system: one list of what runs on it, the team's clients first.
const ids = new Set(['all', ...CLIENT_OS])
const queryOs = new URLSearchParams(window.location.search).get('os')
const queryPlatform = new URLSearchParams(window.location.search).get('platform')
const queryType = new URLSearchParams(window.location.search).get('type')
const TYPE_OS = { phone: 'android', tablet: 'ios', computer: 'windows', tv: 'tv' }
const osOfPlatform = (id) => CLIENT_OS.find((os) => clientsForOs(os).some((x) => x.target.platform === id))
const osFilter = ref(ids.has(queryOs) ? queryOs : osOfPlatform(queryPlatform) || TYPE_OS[queryType] || readOs())
watch(osFilter, (v) => { if (v !== 'all') writeOs(v) })
const ui = computed(() => clientTexts(locale.value))
const osOptions = computed(() => [{ value: 'all', label: ui.value.all, icon: 'apps' }, ...CLIENT_OS.map((os) => ({ value: os, label: osChipName(os, locale.value), icon: OS_ICON[os] }))])
// the files of a client are read once per product (the website's release metadata, CNB first for Chinese) and listed in the popup of its card
const releases = useReleaseAssets()
const conn = computed(() => connectTexts(locale.value))
const qrMatrix = (text) => QRCode.create(text, { errorCorrectionLevel: 'M' }).modules
const targetPlatform = (target) => CLIENT_PLATFORMS.find((platform) => platform.id === target.platform)
const partnerCard = (target) => PARTNER_CARDS.find((card) => card.id === target.platform)
// one card per client in the full list; one card per target of the chosen system in the filtered one
const clientCard = (client, target, partner) => ({ key: `${client.id}:${target?.platform || ''}`, client, target, partner })
const sections = computed(() => {
  if (osFilter.value !== 'all') return [{ id: 'list', cards: clientsForOs(osFilter.value).map(({ client, target }) => clientCard(client, target, client.group === 'partner')) }]
  const { first, partner } = clientSections()
  return [{ id: 'first', title: ui.value.first, cards: first.map((client) => clientCard(client, null, false)) }, { id: 'partner', title: ui.value.partner, cards: partner.map((client) => clientCard(client, null, true)) }]
})
const cardName = (card) => card.client.label || card.client.name
const cardProduct = (card) => (card.target ? targetName(card.target, locale.value) : osOfClient(card.client).map((os) => osName(os, locale.value)).join(' · '))
const CARD_ICON = { 'macos-enhanced': 'laptop_mac', 'vplus-desktop': 'desktop_windows', 'vplus-android': 'android', 'vplus-harmony': 'mobile', voidlink: 'mobile', crown: 'android', baozi: 'mobile' }
// A card of a system shows the shared Material symbol for that system.
const cardIcon = (card) => (card.target && targetPlatform(card.target)?.icon) || CARD_ICON[card.client.id] || 'devices'
// the tags of a card come from the release data of its client (the metadata or the GitHub release), read when the page opens and again when the language changes
const releaseAssets = reactive({})
const loadAssets = () => { for (const file of new Set(CLIENTS.map((c) => c.product).filter(Boolean))) fetchReleaseAssetTypes(file, isChinese.value ? 'zh' : 'en').then((assets) => { releaseAssets[file] = assets }) }
// the card of a client with release data keeps room for its tags (one line per system, the official tag first) whether they are read yet or not, so nothing below it moves when they arrive
const tagsReserve = (card) => (card.client.product ? { minHeight: `${28 + (card.target ? 1 : card.client.targets.length) * 28}px` } : undefined)
const tagsOf = (card) => clientTags(card.client, (c) => releaseAssets[c.product], locale.value, card.target || null)
// a phone scans the card: the QR code of the download or store link of the client's phone system, made when the page opens
const PHONE = new Set(['android', 'ios', 'harmony', 'crown', 'baozi'])
const cardQr = reactive({})
const phoneTarget = (card) => card.target ? (PHONE.has(card.target.platform) ? card.target : null) : card.client.targets.find((t) => PHONE.has(t.platform)) || null
const qrKey = (card) => { const t = phoneTarget(card); return t ? `${t.platform}:${isChinese.value ? 'zh' : 'en'}` : '' }
const qrUrlOf = async (target) => {
  const partner = partnerCard(target)
  if (partner) return partner.links[0].url
  const platform = targetPlatform(target)
  const language = isChinese.value ? 'zh' : 'en'
  const urls = platform.resolveDownloadUrls ? await platform.resolveDownloadUrls(language) : platform.getDownloadUrls(language)
  return urls.primary
}
const loadQr = async () => {
  const { default: QRCode } = await import('qrcode')
  for (const section of sections.value) for (const card of section.cards) {
    const key = qrKey(card)
    if (!key || cardQr[key]) continue
    cardQr[key] = 'pending'
    try { cardQr[key] = await QRCode.toDataURL(await qrUrlOf(phoneTarget(card)), { errorCorrectionLevel: 'M', margin: 2, width: 176 }) } catch { cardQr[key] = 'failed' }
  }
}
const qrOf = (card) => { const v = cardQr[qrKey(card)]; return v && v !== 'pending' && v !== 'failed' ? v : '' }
const linksOf = (client) => [{ id: 'project', label: client.links.host === 'gitee' ? 'Gitee' : 'GitHub', icon: 'code', url: client.links.project }, { id: 'releases', label: ui.value.releases, icon: 'history', url: client.links.releases }]
const isChinese = computed(() => String(locale.value).toLowerCase().startsWith('zh'))
const copy = computed(() => ({
  ...Object.fromEntries(Object.keys(english.ui).filter((key) => !Array.isArray(english.ui[key])).map((key) => [key, t(`ui.${key}`)])),
  setupTitles: tm('ui.setupTitles').map((message) => rt(message)),
  setupSteps: tm('ui.setupSteps').map((message) => rt(message)),
}))

const clientIntroduction = (client) => {
  const key = `platforms.${client.id}`
  const captions = english.platforms[client.id].captions ? tm(`${key}.captions`) : []
  return {
    features: tm(`${key}.features`).map((message) => rt(message)),
    captions: Array.isArray(captions) ? captions.map((message) => rt(message)) : [],
    note: t(`${key}.note`),
  }
}
const featureIcon = (client, index) => CLIENT_FEATURE_ICONS[client.id][index] || 'check'

const community = computed(() => communityTexts(locale.value))
const openLink = (url) => openExternalUrl(url).catch((error) => console.error('Failed to open link:', error))

// The popup of a card: that client's files (read once per product, the popup fills in place when they arrive), and the pages that are not files
const loaded = reactive({})
const loadReleases = () => { for (const product of new Set(CLIENTS.map((c) => c.product).filter(Boolean))) releases.load(product).then(() => { loaded[product] = true }) }
// the card that opened the popup gets the focus back
const focusCard = (key) => nextTick(() => { for (const el of document.querySelectorAll('[data-card]')) if (el.dataset.card === key) el.focus() })
const { card: dialogCard, groups: dialogGroups, open: openDialog, close: closeDialog } = useClientDialog({ assetsOf: (client) => releases.assetsOf(client), loadedOf: (client) => !!loaded[client.product], locale, restoreFocus: focusCard })
// the platform of the card (the single target of the list, or the card's own) gives the feature list and the note
const cardPlatform = (card) => {
  const target = card.target || (card.client.targets.length === 1 ? card.client.targets[0] : null)
  const platform = target && targetPlatform(target)
  return platform && target.client ? [platform, ...(platform.alternatives || [])].find((c) => c.id === target.client) || platform : platform
}
const dialogIntro = computed(() => {
  const platform = dialogCard.value && cardPlatform(dialogCard.value)
  return platform && english.platforms[platform.id] && CLIENT_FEATURE_ICONS[platform.id] ? { platform, ...clientIntroduction(platform) } : null
})
const dialogExtras = computed(() => (dialogCard.value ? extraLinks(dialogCard.value, { platformOf: targetPlatform, partnerOf: partnerCard }) : []))
const extraTitle = (link) => (link.title || link.titleKey ? linkTitle(link) : linkLabel(community.value, link))
const extraDetail = (link) => (link.detail || link.detailKey ? linkDetail(link) : '')
const pageLabel = computed(() => (dialogCard.value?.client.targets.some((t) => targetPlatform(t)?.kind === 'store') ? copy.value.openStore : copy.value.openPage))
// /downloads?platform=android opens the popup of that platform's first client (the home's download card links here)
const openWanted = () => { const card = sections.value.flatMap((section) => section.cards).find((c) => c.target?.platform === queryPlatform); if (card) openDialog(card) }
// The list without a filter (the cards of MClientCard): each card carries its recommended build and one download, the other builds in the card's menu; the details stay in the popup of the card.
// The primary build follows the system the visitor chose last (readOs), else the client's first system. A card without a file of its own (a store, a partner's page) opens that page.
const primaryOs = readOs()
const groupsOf = (card) => fileGroups(card, { assets: releases.assetsOf(card.client), loaded: !!loaded[card.client.product], locale: locale.value })
const extrasOf = (card) => extraLinks(card, { platformOf: targetPlatform, partnerOf: partnerCard })
const fallbackLabel = (card) => (card.client.targets.some((t) => targetPlatform(t)?.kind === 'store') ? copy.value.openStore : copy.value.openPage)
const fallbackGo = (card) => { const links = extrasOf(card); if (links.length === 1) openLink(links[0].url); else if (!links.length) openLink(clientUrl(card.client, locale.value)); else openDialog(card) }
const showsQr = (card) => !!phoneTarget(card) && cardQr[qrKey(card)] !== 'failed'
watch(locale, () => { loadAssets(); void loadQr() })
onMounted(() => { loadAssets(); loadReleases(); void loadQr(); if (queryPlatform) openWanted() })
watch([osFilter], () => { void loadQr() })
</script>

<template>
  <div class="dl-page" :class="{ 'dl-page--all': osFilter === 'all' }">
    <MPage :title="copy.title">
      <!-- the system filter: all (two sections) or one system (one list, the team's clients first) -->
      <MChipSet class="dl-types" :model-value="osFilter" :label="ui.os" :options="osOptions" @update:model-value="(v) => (osFilter = v)" />
      <MSection v-for="section in sections" :key="section.id" :title="section.title">
        <p v-if="section.id === 'first'" class="dl-by">{{ ui.firstBy }}</p>
        <div v-if="osFilter === 'all'" class="m-cgrid">
          <MClientCard v-for="card in section.cards" :key="card.key" :card-key="card.key" :name="cardName(card)" :icon="cardIcon(card)" :systems="cardProduct(card)" :groups="groupsOf(card)" :primary-os="primaryOs"
            :version="releases.versionOf(card.client)" :texts="conn" :qr="qrMatrix" :fallback-label="fallbackLabel(card)" @open="openDialog(card)" @open-url="openLink" @primary-fallback="fallbackGo(card)">
            <template #tag><MClientTag :official="!card.partner" :locale="locale" /></template>
            <template v-if="showsQr(card)" #qr><img v-if="qrOf(card)" :src="qrOf(card)" :alt="ui.scan" :title="ui.scan" width="72" height="72"><span v-else class="m-skeleton" aria-hidden="true" /></template>
            <template #foot><button v-for="link in linksOf(card.client)" :key="link.id" type="button" class="dl-link" @click="openLink(link.url)"><MIcon :name="link.icon" />{{ link.label }}</button></template>
          </MClientCard>
        </div>
        <div v-else class="dl-grid">
          <article v-for="card in section.cards" :key="card.key" class="dl-card">
            <button type="button" class="dl-card__main" :data-card="card.key" aria-haspopup="dialog" @click="openDialog(card)">
              <span class="dl-card__mark"><MIcon :name="cardIcon(card)" /></span>
              <span class="dl-card__name">{{ cardName(card) }}</span>
              <span class="dl-card__tags" :style="tagsReserve(card)">
                <MClientTag :official="!card.partner" :locale="locale" />
                <span v-for="tag in tagsOf(card)" :key="tag" class="dl-tag" :title="tag">{{ tag }}</span>
                <span v-if="!tagsOf(card).length" class="dl-tag dl-tag--plain">{{ cardProduct(card) }}</span>
              </span>
            </button>
            <figure v-if="phoneTarget(card) && cardQr[qrKey(card)] !== 'failed'" class="dl-card__qr"><img v-if="qrOf(card)" :src="qrOf(card)" :alt="ui.scan" width="88" height="88"><span v-else class="dl-card__qr-slot m-skeleton" aria-hidden="true" /><figcaption :style="qrOf(card) ? undefined : { visibility: 'hidden' }">{{ ui.scan }}</figcaption></figure>
            <span class="dl-card__links"><button v-for="link in linksOf(card.client)" :key="link.id" type="button" class="dl-link" @click="openLink(link.url)"><MIcon :name="link.icon" />{{ link.label }}</button></span>
          </article>
        </div>
      </MSection>

      <MSection :title="copy.setup">
        <MGroup>
          <MItem v-for="(title, index) in copy.setupTitles" :key="index" :title="title" :text="copy.setupSteps[index]">
            <template #lead><span class="m-lead-circle m-lead-circle--secondary dl-step">{{ index + 1 }}</span></template>
          </MItem>
        </MGroup>
      </MSection>

      <MSection :title="community.project">
        <div class="dl-links dl-links--project">
          <MButton v-for="link in PROJECT_LINKS" :key="link.id" variant="tonal" :icon="link.icon" @click="openLink(resolveLink(link, locale))">{{ linkLabel(community, link) }}</MButton>
        </div>
      </MSection>
    </MPage>

    <!-- the files of the clicked client; nothing of them is on the page before the click -->
    <MDialog class="dl-dialog" :model-value="!!dialogCard" :title="dialogCard ? cardName(dialogCard) : ''" @update:model-value="(open) => !open && closeDialog()">
      <div v-if="dialogCard" class="dl-dialog__body">
        <p class="dl-dialog__tags"><MClientTag :official="!dialogCard.partner" :locale="locale" /></p>
        <section v-for="group in dialogGroups" :key="group.os" class="dl-dialog__group">
          <h3 v-if="dialogGroups.length > 1" class="dl-dialog__os">{{ group.name }}</h3>
          <MClientAssets v-if="group.state === 'ready'" :rows="group.rows" :texts="conn" :qr="qrMatrix" :mirror="mirrorName(locale)" :version="releases.versionOf(dialogCard.client)" @open-url="openLink" />
          <div v-else-if="group.state === 'loading'" class="dl-dialog__skeleton" role="status" :aria-label="conn.loadingFiles"><span class="m-skeleton" /><span class="m-skeleton" /></div>
          <p v-else class="dl-dialog__failed" role="alert">{{ copy.filesFailed }}</p>
        </section>
        <MGroup v-if="dialogExtras.length">
          <MItem v-for="link in dialogExtras" :key="link.url" action :icon="link.icon || 'open_in_new'" :title="extraTitle(link)" :text="extraDetail(link) || undefined" @click="openLink(link.url)"><template #trail><MIcon name="open_in_new" /></template></MItem>
        </MGroup>
        <MButton v-else-if="!dialogGroups.length" variant="tonal" icon="open_in_new" @click="openLink(clientUrl(dialogCard.client, locale))">{{ pageLabel }}</MButton>
        <ul v-if="dialogIntro" class="dl-features" :aria-label="cardName(dialogCard)">
          <li v-for="(feature, index) in dialogIntro.features" :key="feature" :title="dialogIntro.captions[index]"><MIcon :name="featureIcon(dialogIntro.platform, index)" /><span>{{ feature }}</span></li>
        </ul>
        <MFooter v-if="dialogIntro?.note">{{ dialogIntro.note }}</MFooter>
        <span class="dl-dialog__links"><button v-for="link in linksOf(dialogCard.client)" :key="link.id" type="button" class="dl-link" @click="openLink(link.url)"><MIcon :name="link.icon" />{{ link.label }}</button></span>
      </div>
      <template #actions>
        <MButton variant="text" @click="closeDialog">{{ copy.close }}</MButton>
      </template>
    </MDialog>
  </div>
</template>

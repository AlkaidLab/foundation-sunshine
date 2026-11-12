// The list of paired clients: the kind of a device (from its name, with the keyword table of the home) and the filter and grouping of the page.
// The host sends the system a client runs on (`os`) and no device type: the kind comes from that system and the name (see homeData.js); neither says: "other".
import { deviceKindFor } from './homeData.js'

export const KIND_ORDER = ['phone', 'tablet', 'computer', 'mac', 'handheld', 'tv', 'other']
export const KIND_ICON = { phone: 'mobile', tablet: 'tablet', computer: 'desktop_windows', mac: 'laptop_mac', handheld: 'sports_esports', tv: 'tv', other: 'devices' }

/** The kind of a client (the record of the host, or just its name): phone, tablet, computer (a PC), mac, handheld, tv; a headset or a device nothing is known of is other. */
export function clientKind(client) {
  const kind = typeof client === 'string' || client == null ? deviceKindFor(client) : deviceKindFor(client.name, client.os)
  return KIND_ORDER.includes(kind) ? kind : 'other'
}

const TEXTS = {
  en: { pinPair: 'Pair with a PIN', pinPairText: 'Type the 4 digits the client shows', qrPair: 'Pair with a QR code', qrPairText: 'Scan it with the client', all: 'All', other: 'Other', filter: 'Device type', search: 'Filter clients', noMatch: 'No client matches',
    phone: 'Phones', tablet: 'Tablets', computer: 'PCs', mac: 'Macs', handheld: 'Handhelds', tv: 'TVs' },
  zh: { pinPair: 'PIN 配对', pinPairText: '输入客户端显示的 4 位数字', qrPair: '二维码配对', qrPairText: '用客户端扫描', all: '全部', other: '其他', filter: '设备类型', search: '筛选客户端', noMatch: '没有匹配的客户端',
    phone: '手机', tablet: '平板', computer: 'PC', mac: 'Mac', handheld: '掌机', tv: '电视' },
}
const KINDS = {
  zh_TW: { phone: '手機', tablet: '平板', computer: 'PC', mac: 'Mac', handheld: '掌機', tv: '電視' },
  de: { phone: 'Telefone', tablet: 'Tablets', computer: 'PCs', mac: 'Macs', handheld: 'Handhelds', tv: 'Fernseher' },
  fr: { phone: 'Téléphones', tablet: 'Tablettes', computer: 'PC', mac: 'Mac', handheld: 'Consoles portables', tv: 'TV' },
  es: { phone: 'Teléfonos', tablet: 'Tabletas', computer: 'PC', mac: 'Mac', handheld: 'Portátiles de juego', tv: 'TV' },
  it: { phone: 'Telefoni', tablet: 'Tablet', computer: 'PC', mac: 'Mac', handheld: 'Portatili da gioco', tv: 'TV' },
  ja: { phone: '電話', tablet: 'タブレット', computer: 'PC', mac: 'Mac', handheld: '携帯ゲーム機', tv: 'テレビ' },
  ko: { phone: '휴대폰', tablet: '태블릿', computer: 'PC', mac: 'Mac', handheld: '휴대용 게임기', tv: 'TV' },
  ru: { phone: 'Телефоны', tablet: 'Планшеты', computer: 'ПК', mac: 'Mac', handheld: 'Портативные', tv: 'ТВ' },
  pt_BR: { phone: 'Celulares', tablet: 'Tablets', computer: 'PCs', mac: 'Macs', handheld: 'Portáteis', tv: 'TVs' },
  pt: { phone: 'Telemóveis', tablet: 'Tablets', computer: 'PCs', mac: 'Macs', handheld: 'Portáteis', tv: 'TV' },
  bg: { phone: 'Телефони', tablet: 'Таблети', computer: 'PC', mac: 'Mac', handheld: 'Портативни', tv: 'ТВ' },
  cs: { phone: 'Telefony', tablet: 'Tablety', computer: 'PC', mac: 'Macy', handheld: 'Přenosné', tv: 'TV' },
  pl: { phone: 'Telefony', tablet: 'Tablety', computer: 'PC', mac: 'Maki', handheld: 'Przenośne', tv: 'TV' },
  sv: { phone: 'Telefoner', tablet: 'Surfplattor', computer: 'PC', mac: 'Mac', handheld: 'Handhållna', tv: 'TV' },
  tr: { phone: 'Telefonlar', tablet: 'Tabletler', computer: 'PC', mac: 'Mac', handheld: 'El konsolları', tv: 'TV' },
  uk: { phone: 'Телефони', tablet: 'Планшети', computer: 'ПК', mac: 'Mac', handheld: 'Портативні', tv: 'ТБ' },
}
export function clientTexts(locale = 'en') {
  const tag = String(locale || 'en').replace(/-/g, '_')
  if (/^zh/i.test(tag)) {
    const base = { ...TEXTS.en, ...TEXTS.zh }
    return /(_TW|_HK|_MO|Hant)/i.test(tag) ? { ...base, ...KINDS.zh_TW } : base
  }
  const primary = tag.split('_')[0]
  const kinds = KINDS[tag] || KINDS[primary]
  return kinds ? { ...TEXTS.en, ...kinds } : TEXTS.en
}

/** Clients whose name contains the query (case and width insensitive) and are of the kind ('all' keeps every kind). */
export function filterClients(clients, kind = 'all', query = '') {
  const q = String(query).trim().toLowerCase()
  return clients.filter((c) => (kind === 'all' || clientKind(c) === kind) && (!q || String(c.name || '').toLowerCase().includes(q)))
}

/** [{ kind, items }] in the order of KIND_ORDER, kinds without clients left out; the order of the host is kept inside a group. */
export function groupClients(clients) {
  const groups = KIND_ORDER.map((kind) => ({ kind, items: clients.filter((c) => clientKind(c) === kind) }))
  return groups.filter((g) => g.items.length)
}

/** The filter options: 'all' and every kind that has a client, each with its count. */
export function kindOptions(clients, texts) {
  const groups = groupClients(clients)
  return [{ value: 'all', label: `${texts.all} ${clients.length}`, icon: 'apps' }, ...groups.map((g) => ({ value: g.kind, label: `${texts[g.kind]} ${g.items.length}`, icon: KIND_ICON[g.kind] }))]
}

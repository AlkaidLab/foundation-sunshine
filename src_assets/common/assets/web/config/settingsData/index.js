// SPDX-License-Identifier: GPL-3.0-only
// The settings data of this product: the search index of its settings and the text of its interface languages, generated from the tab templates and
// the locale files (scripts/gen-config-index.mjs). foundation-ui carries none of it; importing this module hands it over (provideSettingsData), so
// the shared list pane, the search, the recent list and the pins speak this product's words. Import it before anything draws.
import { provideSettingsData } from 'foundation-ui/settings/provider.js'
import { SEARCH_INDEX } from './searchIndex.js'
import { LABELS } from './labels.js'

provideSettingsData({ searchIndex: SEARCH_INDEX, labels: LABELS })

export { SEARCH_INDEX, LABELS }

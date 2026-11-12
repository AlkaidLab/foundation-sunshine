import assert from "node:assert/strict"
import test from "node:test"
import { computed } from "vue"
import { useApps } from "../composables/useApps.js"
import { GAME_LIBRARY_SKILL_IDS } from "../utils/agents/gameLibrary/gameLibraryCuratorAgent.js"

// The scan sheet draws its AI labels before the page hands over its own translator (`init` runs on mount): they must follow the translator afterwards,
// not stay in English in a Chinese interface.
test("the AI labels of the scan sheet follow the page translator once it is set", () => {
  const apps = useApps()
  const label = computed(() => apps.getGameLibrarySkillLabel(GAME_LIBRARY_SKILL_IDS.titleNormalize))
  const cover = computed(() => apps.getGameLibrarySkillLabel(GAME_LIBRARY_SKILL_IDS.coverSelection))
  assert.equal(label.value, "AI name cleanup")
  apps.init((key) => ({ "apps.scan_options.ai_name_cleanup": "AI 名称清洗", "apps.scan_options.ai_cover_matching": "AI 封面匹配" })[key] ?? key)
  assert.equal(label.value, "AI 名称清洗")
  assert.equal(cover.value, "AI 封面匹配")
})

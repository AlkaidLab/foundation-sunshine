<script setup>
import { useI18n } from 'vue-i18n'
import { useSettingsLive } from '../../composables/useSettingsLive.js'
import { useApplyNow } from '../../composables/useApplyNow.js'
import CfgSub from '../../components/cfg/CfgSub.vue'
import DisplayOutput from './display/DisplayOutput.vue'
import DisplayModes from './display/DisplayModes.vue'
import DisplayHdr from './display/DisplayHdr.vue'
import DisplayQuality from './display/DisplayQuality.vue'
import DisplayEncoding from './display/DisplayEncoding.vue'
import DisplayVirtual from './display/DisplayVirtual.vue'
import DisplayAdvanced from './display/DisplayAdvanced.vue'

// 显示与视频, the approved tree (foundation-ui settings/displayTree.js): 显示器 · 分辨率与刷新率 · HDR · 画质增强 · 编码与码率 · 基地虚拟显示器 · 高级. A place is a page; the
// sections of a page are in its component. The resolutions, the frame rates and the bitrate cap apply when they are changed (one decision, with an undo), not with the bar of unsaved changes.
const props = defineProps(['platform', 'config', 'resolutions', 'fps', 'focus', 'encoders'])
const { t } = useI18n()
const live = useSettingsLive({ platform: props.platform, want: ['session', 'bitrate'] })
useApplyNow()
</script>

<template>
  <CfgSub id="output" :title="$t('shell.display_output')">
    <DisplayOutput :platform="platform" :config="config" :session="live.sessions.value[0]" :focus="focus" />
  </CfgSub>

  <CfgSub id="modes" :title="$t('shell.display_modes')">
    <DisplayModes :config="config" :resolutions="resolutions" :fps="fps" />
  </CfgSub>

  <CfgSub v-if="platform === 'windows'" id="hdr" :title="$t('shell.display_hdr')">
    <DisplayHdr :platform="platform" :config="config" :focus="focus" />
  </CfgSub>

  <CfgSub v-if="platform === 'windows'" id="quality" :title="$t('config.hdr_enhanced')">
    <DisplayQuality :platform="platform" />
  </CfgSub>

  <CfgSub id="encoders" :title="$t('shell.display_encoders')">
    <DisplayEncoding :platform="platform" :config="config" :encoders="encoders" :focus="focus" />
  </CfgSub>

  <CfgSub v-if="platform === 'windows'" id="virtual" :title="$t('shell.virtual_display')">
    <DisplayVirtual :config="config" :platform="platform" />
  </CfgSub>

  <CfgSub id="advanced" :title="$t('shell.display_advanced')">
    <DisplayAdvanced :platform="platform" :config="config" />
  </CfgSub>
</template>

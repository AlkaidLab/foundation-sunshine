<template>
  <div class="cmd-table">
    <MTable v-if="localCommands.length" dense row-key="id" :columns="columns" :rows="rows">
      <template #cell-order="{ row }">
        <span class="cmd-table__order">
          <MIconButton icon="arrow_upward" :label="$t('apps.move_earlier')" :disabled="row.id === 0" @click.stop="moveCommand(row.id, -1)" />
          <MIconButton icon="arrow_downward" :label="$t('apps.move_later')" :disabled="row.id === localCommands.length - 1" @click.stop="moveCommand(row.id, 1)" />
        </span>
      </template>
      <template #cell-do="{ row }"><input type="text" class="cmd-cell cmd-cell--code m-input" :value="row.do" :aria-label="$t('_common.do_cmd')" :placeholder="$t('apps.menu_cmd_placeholder_execute')" @input="updateCommandField(row.id, 'do', $event.target.value)" /></template>
      <template #cell-undo="{ row }"><input type="text" class="cmd-cell cmd-cell--code m-input" :value="row.undo" :aria-label="$t('_common.undo_cmd')" :placeholder="$t('apps.menu_cmd_placeholder_undo')" @input="updateCommandField(row.id, 'undo', $event.target.value)" /></template>
      <template #cell-name="{ row }"><input type="text" class="cmd-cell m-input" :value="row.name" :aria-label="$t('apps.menu_cmd_display_name')" :placeholder="$t('apps.menu_cmd_placeholder_display_name')" @input="updateCommandField(row.id, 'name', $event.target.value)" /></template>
      <template #cell-cmd="{ row }"><input type="text" class="cmd-cell cmd-cell--code m-input" :value="row.cmd" :aria-label="$t('apps.menu_cmd_command')" :placeholder="$t('apps.menu_cmd_placeholder_command')" @input="updateCommandField(row.id, 'cmd', $event.target.value)" /></template>
      <template #cell-elevated="{ row }"><MSwitch :model-value="isElevated(row)" :label="$t('_common.elevated')" @update:model-value="(on) => updateCommandField(row.id, 'elevated', on ? 'true' : 'false')" /></template>
      <template #cell-actions="{ row }">
        <span class="cmd-table__actions">
          <MIconButton v-if="isMenuType" icon="play_arrow" tonal :label="$t('apps.test_menu_cmd')" :disabled="!row.cmd" @click.stop="testCommand(row.id)" />
          <MIconButton icon="delete" :label="removeButtonTitle" @click.stop="removeCommand(row.id)" />
        </span>
      </template>
    </MTable>

    <MButton variant="tonal" icon="add" @click="addCommand">{{ addButtonText }}</MButton>
  </div>
</template>

<script setup>
import { ref, computed, watch } from 'vue'
import { useI18n } from 'vue-i18n'
import { MButton, MIconButton, MSwitch, MTable } from 'foundation-ui/vue'
import { deepClone } from '../utils/helpers.js'

const { t } = useI18n()

const props = defineProps({
  commands: {
    type: Array,
    required: true,
  },
  platform: {
    type: String,
    default: 'linux',
  },
  type: {
    type: String,
    required: true,
    validator: (value) => ['prep', 'menu', 'detached'].includes(value),
  },
})

const emit = defineEmits(['add-command', 'remove-command', 'test-command', 'order-changed'])

const localCommands = ref([])

const isWindows = computed(() => props.platform === 'windows')
const isPrepType = computed(() => props.type === 'prep')
const isMenuType = computed(() => props.type === 'menu')
const isDetachedType = computed(() => props.type === 'detached')
const showElevatedColumn = computed(() => isWindows.value && !isDetachedType.value)

const addButtonText = computed(() => {
  const textMap = {
    prep: 'apps.add_cmds',
    detached: 'apps.detached_cmds_add',
    menu: 'apps.menu_cmd_add',
  }
  return t(textMap[props.type] || textMap.menu)
})

const removeButtonTitle = computed(() => {
  const titleMap = {
    prep: 'apps.menu_cmd_remove_prep',
    detached: 'apps.detached_cmds_remove',
    menu: 'apps.menu_cmd_remove_menu',
  }
  return t(titleMap[props.type] || titleMap.menu)
})

const normalizeCommand = (cmd) => {
  if (typeof cmd === 'string') return { cmd }
  if (cmd && typeof cmd === 'object' && 'cmd' in cmd) return { cmd: cmd.cmd || '' }
  return { cmd: '' }
}

watch(
  () => props.commands,
  (newVal) => {
    const commands = newVal || []
    localCommands.value = isDetachedType.value ? commands.map(normalizeCommand) : deepClone(commands)
  },
  { immediate: true, deep: true }
)

const columns = computed(() => {
  const cols = [{ key: 'order', label: '', width: '76px' }]
  if (isPrepType.value) cols.push({ key: 'do', label: t('_common.do_cmd') }, { key: 'undo', label: t('_common.undo_cmd') })
  else if (isMenuType.value) cols.push({ key: 'name', label: t('apps.menu_cmd_display_name') }, { key: 'cmd', label: t('apps.menu_cmd_command') })
  else cols.push({ key: 'cmd', label: t('apps.menu_cmd_command') })
  if (showElevatedColumn.value) cols.push({ key: 'elevated', label: t('_common.run_as'), width: '84px', align: 'center' })
  cols.push({ key: 'actions', label: t('apps.menu_cmd_actions'), width: isMenuType.value ? '96px' : '56px', align: 'right' })
  return cols
})
// The row id is its position: the list keeps no other identity
const rows = computed(() => localCommands.value.map((command, id) => ({ ...command, id })))

const moveCommand = (index, step) => {
  const target = index + step
  if (target < 0 || target >= localCommands.value.length) return
  const next = [...localCommands.value]
  ;[next[index], next[target]] = [next[target], next[index]]
  localCommands.value = next
  emitOrderChanged()
}

const isElevated = (command) => command.elevated === 'true' || command.elevated === true

const emitOrderChanged = () => {
  const data = isDetachedType.value ? localCommands.value.map((cmd) => cmd.cmd || '') : localCommands.value
  emit('order-changed', data)
}

const updateCommandField = (index, field, value) => {
  if (isDetachedType.value) {
    localCommands.value[index].cmd = value
  } else {
    localCommands.value[index][field] = value
  }
  emitOrderChanged()
}

const addCommand = () => emit('add-command')
const removeCommand = (index) => emit('remove-command', index)
const testCommand = (index) => emit('test-command', index)
</script>

<style scoped lang="less">
.cmd-table {
  display: flex;
  flex-direction: column;
  align-items: flex-start;
  gap: 8px;
  min-width: 0;

  :deep(.m-table-wrap) {
    width: 100%;
  }

  :deep(.m-table tbody tr:hover) {
    background: transparent;
  }

  :deep(.m-table th) {
    white-space: normal;
  }

  :deep(.m-table th),
  :deep(.m-table td) {
    padding-inline: 8px;
  }
}

.cmd-table__order,
.cmd-table__actions {
  display: inline-flex;
  gap: 0;
}

.cmd-cell {
  width: 100%;
  min-width: 104px;
  padding: 8px 12px;
  border-radius: 12px;
  font: var(--m-body-medium);

  &--code {
    font-family: ui-monospace, SFMono-Regular, Menlo, Monaco, Consolas, monospace;
  }
}
</style>

<template>
  <div class="form-group-enhanced">
    <div v-if="localCommands.length > 0" class="command-table">
      <table class="table table-sm">
        <thead>
          <tr>
            <th class="drag-column"></th>
            <template v-if="isPrepType">
              <th><i class="fas fa-play"></i> {{ $t('_common.do_cmd') }}</th>
              <th><i class="fas fa-undo"></i> {{ $t('_common.undo_cmd') }}</th>
            </template>
            <template v-else-if="isMenuType">
              <th><i class="fas fa-tag"></i> {{ $t('apps.menu_cmd_display_name') }}</th>
              <th><i class="fas fa-terminal"></i> {{ $t('apps.menu_cmd_command') }}</th>
            </template>
            <template v-else-if="isDetachedType">
              <th><i class="fas fa-terminal"></i> {{ $t('apps.menu_cmd_command') }}</th>
            </template>
            <th v-if="showElevatedColumn"><i class="fas fa-shield-alt"></i> {{ $t('_common.run_as') }}</th>
            <th class="actions-column">{{ $t('apps.menu_cmd_actions') }}</th>
          </tr>
        </thead>
        <draggable
          v-model="localCommands"
          tag="tbody"
          :item-key="getItemKey"
          :animation="300"
          :delay="0"
          :disabled="localCommands.length <= 1"
          filter="input, button, .form-check-input"
          :prevent-on-filter="false"
          ghost-class="command-row-ghost"
          chosen-class="command-row-chosen"
          drag-class="command-row-drag"
          @end="onDragEnd"
        >
          <template #item="{ element: command, index }">
            <tr>
              <td class="drag-handle-cell">
                <div class="drag-handle" :class="{ 'drag-disabled': localCommands.length <= 1 }" :title="$t('apps.menu_cmd_drag_sort')">
                  <i class="fas fa-grip-vertical"></i>
                </div>
              </td>

              <template v-if="isPrepType">
                <td>
                  <input
                    type="text"
                    class="form-control form-control-sm monospace"
                    :value="command.do"
                    :placeholder="$t('apps.menu_cmd_placeholder_execute')"
                    @input="updateCommandField(index, 'do', $event.target.value)"
                  />
                </td>
                <td>
                  <input
                    type="text"
                    class="form-control form-control-sm monospace"
                    :value="command.undo"
                    :placeholder="$t('apps.menu_cmd_placeholder_undo')"
                    @input="updateCommandField(index, 'undo', $event.target.value)"
                  />
                </td>
              </template>

              <template v-else-if="isMenuType">
                <td>
                  <input
                    type="text"
                    class="form-control form-control-sm"
                    :value="command.name"
                    :placeholder="$t('apps.menu_cmd_placeholder_display_name')"
                    @input="updateCommandField(index, 'name', $event.target.value)"
                  />
                </td>
                <td>
                  <input
                    type="text"
                    class="form-control form-control-sm monospace"
                    :value="command.cmd"
                    :placeholder="$t('apps.menu_cmd_placeholder_command')"
                    @input="updateCommandField(index, 'cmd', $event.target.value)"
                  />
                </td>
              </template>

              <template v-else-if="isDetachedType">
                <td>
                  <input
                    type="text"
                    class="form-control form-control-sm monospace"
                    :value="command.cmd"
                    :placeholder="$t('apps.menu_cmd_placeholder_command')"
                    @input="updateCommandField(index, 'cmd', $event.target.value)"
                  />
                </td>
              </template>

              <td v-if="showElevatedColumn">
                <div class="form-check">
                  <input
                    :id="`${type}-cmd-admin-${index}`"
                    type="checkbox"
                    class="form-check-input"
                    :checked="isElevated(command)"
                    @change="updateCommandField(index, 'elevated', $event.target.checked ? 'true' : 'false')"
                  />
                  <label :for="`${type}-cmd-admin-${index}`" class="form-check-label">
                    {{ $t('_common.elevated') }}
                  </label>
                </div>
              </td>

              <td>
                <div class="action-buttons-group">
                  <button
                    v-if="isMenuType"
                    type="button"
                    class="btn btn-success btn-sm me-1"
                    :title="$t('apps.test_menu_cmd')"
                    :disabled="!command.cmd"
                    @click="testCommand(index)"
                  >
                    <i class="fas fa-play"></i>
                  </button>
                  <button
                    type="button"
                    class="btn btn-sm"
                    :title="removeButtonTitle"
                    @click="removeCommand(index)"
                  >
                    <i class="fas fa-trash"></i>
                  </button>
                </div>
              </td>
            </tr>
          </template>
        </draggable>
      </table>
    </div>

    <button type="button" class="btn btn-outline-success add-command-btn" @click="addCommand">
      <i class="fas fa-plus me-1"></i>{{ addButtonText }}
    </button>
  </div>
</template>

<script setup>
import { ref, computed, watch } from 'vue'
import { useI18n } from 'vue-i18n'
import draggable from 'vuedraggable-es'

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
    localCommands.value = isDetachedType.value
      ? commands.map(normalizeCommand)
      : JSON.parse(JSON.stringify(commands))
  },
  { immediate: true, deep: true }
)

const getItemKey = (_, index) => `${props.type}-${index}`

const isElevated = (command) => command.elevated === 'true' || command.elevated === true

const emitOrderChanged = () => {
  const data = isDetachedType.value
    ? localCommands.value.map((cmd) => cmd.cmd || '')
    : localCommands.value
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
const onDragEnd = () => emitOrderChanged()
</script>

<style scoped lang="less">
.command-table {
  margin-bottom: var(--spacing-md);
}

.monospace {
  font-family: 'Courier New', monospace;
}

.drag-column {
  width: 40px;
}

.actions-column {
  width: 100px;
}

.table {
  color: var(--modal-text-color, #fff);
  border-color: var(--modal-border-color, rgba(255, 255, 255, 0.15));
  margin-bottom: 0;

  th {
    border-top: none;
    border-bottom: 1px solid var(--glass-border, rgba(255, 255, 255, 0.2));
    font-weight: 600;
    font-size: 0.875rem;
    padding: 1rem 0.75rem;
    background: var(--glass-medium, rgba(255, 255, 255, 0.1));
    color: var(--modal-text-color, #fff);
  }

  thead th {
    &:first-child {
      border-radius: 12px 0 0 0;
    }

    &:last-child {
      border-radius: 0 12px 0 0;
      text-align: center;
    }
  }

  tr:last-child td {
    border-bottom: none;
  }

  td {
    vertical-align: middle;
    border-color: var(--modal-border-color, rgba(255, 255, 255, 0.1));
    padding: 0.75rem;
    background: var(--glass-light, rgba(255, 255, 255, 0.05));
    transition: background 0.3s ease;
  }

  tbody tr {
    &:hover td {
      background: var(--glass-medium, rgba(255, 255, 255, 0.1));
    }

    &:last-child td {
      &:first-child {
        border-radius: 0 0 0 12px;
      }

      &:last-child {
        border-radius: 0 0 12px 0;
      }
    }

    &:hover .drag-handle:not(.drag-disabled) {
      opacity: 1;
    }
  }
}

.form-control {
  max-width: 480px;
}

.form-control-sm {
  font-size: 0.875rem;
  background: var(--glass-light, rgba(255, 255, 255, 0.1));
  border: 1px solid var(--glass-border, rgba(255, 255, 255, 0.2));
  border-radius: 8px;
  color: var(--modal-text-color, #fff);
  backdrop-filter: blur(5px);
  transition: all 0.3s ease;

  &:focus {
    background: var(--glass-medium, rgba(255, 255, 255, 0.15));
    border-color: var(--btn-outline-primary-border, rgba(255, 255, 255, 0.4));
    box-shadow: 0 0 0 0.2rem var(--btn-outline-primary-hover, rgba(255, 255, 255, 0.25));
    color: var(--modal-text-color, #fff);
  }

  &::placeholder {
    color: var(--modal-text-muted, rgba(255, 255, 255, 0.6));
  }
}

.btn-sm {
  padding: 0.25rem 0.5rem;
  font-size: 0.75rem;
  border-radius: 8px;
  transition: all 0.3s ease;
}

.form-check {
  display: flex;
  align-items: center;
  justify-content: center;
}

.form-check-label {
  color: var(--modal-text-color, #fff);
  font-size: 0.875rem;
  font-weight: 500;
  margin-left: 0.5rem;
}

.action-buttons-group {
  display: flex;
  gap: 0.25rem;
  align-items: center;
  justify-content: center;
}

.drag-handle-cell {
  width: 40px;
  padding: 0.5rem !important;
  text-align: center;
  cursor: move;
  user-select: none;
}

.drag-handle {
  color: var(--modal-text-muted, rgba(255, 255, 255, 0.5));
  font-size: 1.2rem;
  display: inline-block;
  padding: 0.5rem;
  opacity: 0;
  cursor: move;
  transition: opacity 0.3s ease, color 0.3s ease;

  &:hover {
    color: var(--modal-text-secondary, rgba(255, 255, 255, 0.9));
  }

  &.drag-disabled {
    cursor: not-allowed;
    opacity: 0.3 !important;
    pointer-events: none;
  }
}

.command-row-ghost {
  opacity: 0;
  pointer-events: none;
}

.command-row-chosen {
  background: var(--glass-medium, rgba(255, 255, 255, 0.15));
  z-index: 1000;
  position: relative;
}

.command-row-drag {
  opacity: 0.95;
  transform: rotate(2deg);
  box-shadow: 0 10px 30px var(--modal-shadow, rgba(0, 0, 0, 0.3));
  z-index: 1001;
  position: relative;
}

@media (max-width: 768px) {
  .command-table {
    padding: 1rem;
    margin-top: 0.5rem;
  }

  .table {
    th,
    td {
      padding: 0.5rem;
      font-size: 0.8rem;
    }
  }

  .form-control-sm {
    font-size: 0.8rem;
    padding: 0.25rem 0.5rem;
  }

  .btn-sm {
    padding: 0.2rem 0.4rem;
    font-size: 0.7rem;
  }

  .add-command-btn {
    padding: 0.4rem 0.8rem;
    font-size: 0.8rem;
  }
}
</style>

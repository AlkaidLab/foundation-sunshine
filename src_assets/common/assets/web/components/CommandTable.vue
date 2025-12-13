<template>
  <div class="form-group-enhanced">
    <label class="form-label-enhanced">{{ tableTitle }}</label>
    <div class="field-hint">{{ tableDescription }}</div>

    <div class="command-table" v-if="localCommands.length > 0">
      <table class="table table-sm">
        <thead>
          <tr>
            <th style="width: 40px"></th>
            <th v-if="type === 'prep'"><i class="fas fa-play"></i> {{ $t('_common.do_cmd') }}</th>
            <th v-if="type === 'prep'"><i class="fas fa-undo"></i> {{ $t('_common.undo_cmd') }}</th>
            <th v-if="type === 'menu'"><i class="fas fa-tag"></i> {{ $t('apps.menu_cmd_display_name') }}</th>
            <th v-if="type === 'menu'"><i class="fas fa-terminal"></i> {{ $t('apps.menu_cmd_command') }}</th>
            <th v-if="platform === 'windows'"><i class="fas fa-shield-alt"></i> {{ $t('_common.run_as') }}</th>
            <th style="width: 100px">{{ $t('apps.menu_cmd_actions') }}</th>
          </tr>
        </thead>
        <draggable
          tag="tbody"
          v-model="localCommands"
          :item-key="getItemKey"
          :animation="300"
          :delay="0"
          handle=".drag-handle"
          ghost-class="command-row-ghost"
          chosen-class="command-row-chosen"
          drag-class="command-row-drag"
          :force-fallback="false"
          @end="onDragEnd"
        >
          <template #item="{ element: command, index }">
            <tr :key="index">
              <!-- 拖拽手柄 -->
              <td class="drag-handle-cell">
                <div class="drag-handle" @dragstart.stop @dragend.stop :title="$t('apps.menu_cmd_drag_sort')">
                  <i class="fas fa-grip-vertical"></i>
                </div>
              </td>
              <!-- 准备命令字段 -->
              <template v-if="type === 'prep'">
                <td>
                  <input
                    type="text"
                    class="form-control form-control-sm monospace"
                    :value="command.do"
                    @input="updateCommandField(index, 'do', $event.target.value)"
                    :placeholder="$t('apps.menu_cmd_placeholder_execute')"
                  />
                </td>
                <td>
                  <input
                    type="text"
                    class="form-control form-control-sm monospace"
                    :value="command.undo"
                    @input="updateCommandField(index, 'undo', $event.target.value)"
                    :placeholder="$t('apps.menu_cmd_placeholder_undo')"
                  />
                </td>
              </template>

              <!-- 菜单命令字段 -->
              <template v-if="type === 'menu'">
                <td>
                  <input
                    type="text"
                    class="form-control form-control-sm"
                    :value="command.name"
                    @input="updateCommandField(index, 'name', $event.target.value)"
                    :placeholder="$t('apps.menu_cmd_placeholder_display_name')"
                  />
                </td>
                <td>
                  <input
                    type="text"
                    class="form-control form-control-sm monospace"
                    :value="command.cmd"
                    @input="updateCommandField(index, 'cmd', $event.target.value)"
                    :placeholder="$t('apps.menu_cmd_placeholder_command')"
                  />
                </td>
              </template>

              <!-- Windows权限设置 -->
              <td v-if="platform === 'windows'">
                <div class="form-check">
                  <input
                    type="checkbox"
                    class="form-check-input"
                    :id="`${type}-cmd-admin-${index}`"
                    :checked="command.elevated === 'true' || command.elevated === true"
                    @change="updateCommandField(index, 'elevated', $event.target.checked ? 'true' : 'false')"
                  />
                  <label :for="`${type}-cmd-admin-${index}`" class="form-check-label">
                    {{ $t('_common.elevated') }}
                  </label>
                </div>
              </td>

              <!-- 操作按钮 -->
              <td>
                <div class="action-buttons-group">
                  <button
                    v-if="type === 'menu'"
                    type="button"
                    class="btn btn-success btn-sm me-1"
                    @click="testCommand(index)"
                    :title="$t('apps.test_menu_cmd')"
                    :disabled="!command.cmd"
                  >
                    <i class="fas fa-play"></i>
                  </button>
                  <button
                    type="button"
                    class="btn btn-outline-danger btn-sm"
                    @click="removeCommand(index)"
                    :title="type === 'prep' ? $t('apps.menu_cmd_remove_prep') : $t('apps.menu_cmd_remove_menu')"
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

<script>
import draggable from 'vuedraggable-es'

export default {
  name: 'CommandTable',
  components: {
    draggable,
  },
  props: {
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
      validator: (value) => ['prep', 'menu'].includes(value),
    },
  },
  data() {
    return {
      localCommands: [],
    }
  },
  watch: {
    commands: {
      immediate: true,
      deep: true,
      handler(newVal) {
        // 深拷贝以避免直接修改 props
        this.localCommands = JSON.parse(JSON.stringify(newVal || []))
      },
    },
  },
  computed: {
    tableTitle() {
      return this.type === 'prep' ? this.$t('apps.cmd_prep_name') : this.$t('apps.menu_cmd_name')
    },
    tableDescription() {
      if (this.type === 'prep') {
        return this.$t('apps.cmd_prep_desc')
      }
      return this.$t('apps.menu_cmd_desc')
    },
    addButtonText() {
      return this.type === 'prep' ? this.$t('apps.add_cmds') : this.$t('apps.menu_cmd_add')
    },
  },
  methods: {
    /**
     * 获取命令的唯一键（用于 draggable）
     * 使用稳定的索引作为 key，避免输入时重新渲染导致失焦
     */
    getItemKey(command) {
      const index = this.localCommands.indexOf(command)
      return `${this.type}-${index}`
    },

    /**
     * 更新命令字段
     */
    updateCommandField(index, field, value) {
      this.localCommands[index][field] = value
      this.$emit('order-changed', this.localCommands)
    },

    /**
     * 添加命令
     */
    addCommand() {
      this.$emit('add-command')
    },

    /**
     * 移除命令
     */
    removeCommand(index) {
      this.$emit('remove-command', index)
    },

    /**
     * 测试菜单命令
     */
    testCommand(index) {
      this.$emit('test-command', index)
    },

    /**
     * 拖拽结束处理
     */
    onDragEnd() {
      // 通知父组件顺序已改变
      this.$emit('order-changed', this.localCommands)
    },
  },
}
</script>

<style scoped>
.monospace {
  font-family: 'Courier New', monospace;
}

.table {
  color: #ffffff;
  border-color: rgba(255, 255, 255, 0.15);
  margin-bottom: 0;
}

.table th {
  border-top: none;
  border-bottom: 2px solid rgba(255, 255, 255, 0.2);
  font-weight: 600;
  font-size: 0.875rem;
  padding: 1rem 0.75rem;
  background: rgba(255, 255, 255, 0.1);
  color: #ffffff;
  text-shadow: 0 1px 2px rgba(0, 0, 0, 0.3);
}

.table thead th:first-child {
  border-radius: 12px 0 0 0;
}

.table thead th:last-child {
  border-radius: 0 12px 0 0;
}

.table td {
  vertical-align: middle;
  border-color: rgba(255, 255, 255, 0.1);
  padding: 0.75rem;
  background: rgba(255, 255, 255, 0.05);
  transition: all 0.3s ease;
}

.table tbody tr:hover td {
  background: rgba(255, 255, 255, 0.1);
  backdrop-filter: blur(5px);
}

.table tbody tr:last-child td:first-child {
  border-radius: 0 0 0 12px;
}

.table tbody tr:last-child td:last-child {
  border-radius: 0 0 12px 0;
}

.form-control-sm {
  font-size: 0.875rem;
  background: rgba(255, 255, 255, 0.1);
  border: 1px solid rgba(255, 255, 255, 0.2);
  border-radius: 8px;
  color: #ffffff;
  backdrop-filter: blur(5px);
  transition: all 0.3s ease;
}

.form-control-sm:focus {
  background: rgba(255, 255, 255, 0.15);
  border-color: rgba(255, 255, 255, 0.4);
  box-shadow: 0 0 0 0.2rem rgba(255, 255, 255, 0.25);
  color: #ffffff;
}

.form-control-sm::placeholder {
  color: rgba(255, 255, 255, 0.6);
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
  color: #ffffff;
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

/* 拖拽手柄样式 */
.drag-handle-cell {
  width: 40px;
  padding: 0.5rem !important;
  text-align: center;
  cursor: move;
  user-select: none;
}

.drag-handle {
  color: rgba(255, 255, 255, 0.5);
  font-size: 1rem;
  transition: color 0.3s ease;
  display: inline-block;
  padding: 0.25rem;
}

.drag-handle:hover {
  color: rgba(255, 255, 255, 0.9);
}

/* 拖拽状态样式 */
.command-row-ghost {
  opacity: 0;
  pointer-events: none;
}

.command-row-chosen {
  background: rgba(255, 255, 255, 0.15);
  z-index: 1000;
  position: relative;
}

.command-row-drag {
  opacity: 0.95;
  transform: rotate(2deg);
  box-shadow: 0 10px 30px rgba(0, 0, 0, 0.3);
  z-index: 1001;
  position: relative;
}

/* 动画效果 */
@keyframes fadeIn {
  from {
    opacity: 0;
    transform: translateY(20px);
  }
  to {
    opacity: 1;
    transform: translateY(0);
  }
}

/* 响应式设计 */
@media (max-width: 768px) {
  .command-table {
    padding: 1rem;
    margin-top: 0.5rem;
  }

  .table th,
  .table td {
    padding: 0.5rem;
    font-size: 0.8rem;
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

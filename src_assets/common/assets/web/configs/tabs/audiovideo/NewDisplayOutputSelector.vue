<script setup>
import { ref, computed } from "vue";
import { $tp } from "../../../platform-i18n";
import PlatformLayout from "../../../components/layout/PlatformLayout.vue";
import VddPrerequisiteNotice from "../../../components/common/VddPrerequisiteNotice.vue";

const props = defineProps({
  platform: String,
  config: {
    type: Object,
    default: null,
  },
  modelValue: {
    type: String,
    default: undefined,
  },
  displays: {
    type: Array,
    default: undefined,
  },
  context: {
    type: String,
    default: 'global',
    validator: (value) => ['global', 'app'].includes(value),
  },
});

const emit = defineEmits(['update:modelValue']);

const config = ref(props.config);
const isAppContext = computed(() => props.context === 'app');
const outputName = computed({
  get: () => isAppContext.value ? props.modelValue : config.value?.output_name,
  set: (value) => {
    if (isAppContext.value) {
      emit('update:modelValue', value);
    } else if (config.value) {
      config.value.output_name = value;
    }
  },
});

// Check if VDD mode is enabled (output_name is 'ZakoHDR')
const isVddMode = computed(() => {
  return outputName.value === 'ZakoHDR'
});

// "DISPLAY NAME: \\\\.\\DISPLAY1\nFRIENDLY NAME: F32D80U\nDEVICE STATE: PRIMARY\nHDR STATE: ENABLED"
const displayDevices = computed(() => {
  const devices = props.displays ?? config.value?.display_devices;
  if (!Array.isArray(devices)) {
    return [];
  }
  return devices.map(({ device_id, data = "" }) => ({
    id: device_id,
    name: data
      .replace(
        /.*?(DISPLAY\d+)?\nFRIENDLY NAME: (.*[^\n])*?\n.*\n.*/g,
        "$2 ($1)"
      )
      .replace("()", ""),
  }));
});

const selectedPhysicalDevice = computed(() => {
  if (!isAppContext.value || !outputName.value || ['__inherit__', '__physical__', 'ZakoHDR'].includes(outputName.value)) {
    return null;
  }
  return displayDevices.value.some((device) => device.id === outputName.value)
    ? null
    : { id: outputName.value, name: outputName.value };
});
</script>

<template>
  <div class="mb-3">
    <label :for="isAppContext ? 'app_output_name' : 'output_name'" class="form-label">{{
      $t("config.output_name_windows")
    }}</label>
    <select :id="isAppContext ? 'app_output_name' : 'output_name'" class="form-select" v-model="outputName">
      <template v-if="isAppContext">
        <option value="__inherit__">{{ $t("apps.display_profile_inherit") }}</option>
        <option value="__physical__">
          {{ $t("apps.display_profile_physical") }}
        </option>
      </template>
      <option v-else value="">{{ $t("_common.autodetect") }}</option>
      <option value="ZakoHDR">{{ $t("config.output_name_vdd_option") }}</option>
      <option
        v-if="selectedPhysicalDevice"
        :value="selectedPhysicalDevice.id"
      >
        {{ selectedPhysicalDevice.name }}
      </option>
      <option
        v-for="device in displayDevices"
        :value="device.id"
        :key="device.id"
      >
        {{ device.name }}
      </option>
    </select>
    <div class="form-text">
      <p class="pre-line">{{ isAppContext ? $t("apps.display_profile_policy_desc") : $tp("config.output_name_desc") }}</p>
      <PlatformLayout :platform="platform">
        <template #windows></template>
        <template #linux> </template>
        <template #macos> </template>
      </PlatformLayout>
    </div>
  </div>

  <VddPrerequisiteNotice :active="isVddMode && platform === 'windows'" />

  <!-- VDD mode: Reuse VDD for all clients (only shown in VDD mode, Windows only) -->
  <div class="mb-3 form-check" v-if="!isAppContext && isVddMode && platform === 'windows'">
    <input
      type="checkbox"
      class="form-check-input"
      id="vdd_reuse"
      v-model="config.vdd_reuse"
      true-value="enabled"
      false-value="disabled"
    />
    <label class="form-check-label" for="vdd_reuse">
      {{ $tp('config.vdd_reuse') }}
    </label>
    <div class="form-text">
      {{ $tp('config.vdd_reuse_desc') }}
    </div>
  </div>

  <div class="mb-3" v-if="platform === 'linux' || platform === 'macos'">
    <label for="output_name" class="form-label">{{
      $t("config.output_name_unix")
    }}</label>
    <input
      type="text"
      class="form-control"
      id="output_name"
      placeholder="0"
      v-model="config.output_name"
    />
    <div class="form-text">
      {{ $t("config.output_name_desc_unix") }}<br />
      <br />
      <pre class="pre-line" v-if="platform === 'linux'">
              Info: Detecting displays
              Info: Detected display: DVI-D-0 (id: 0) connected: false
              Info: Detected display: HDMI-0 (id: 1) connected: true
              Info: Detected display: DP-0 (id: 2) connected: true
              Info: Detected display: DP-1 (id: 3) connected: false
              Info: Detected display: DVI-D-1 (id: 4) connected: false
            </pre
      >
      <pre class="pre-line" v-if="platform === 'macos'">
              Info: Detecting displays
              Info: Detected display: Monitor-0 (id: 3) connected: true
              Info: Detected display: Monitor-1 (id: 2) connected: true
            </pre
      >
    </div>
  </div>
</template>

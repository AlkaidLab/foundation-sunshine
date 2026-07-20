<script setup>
import { computed } from 'vue'

const props = defineProps({
  href: { type: String, required: true },
  title: { type: String, required: true },
  description: { type: String, default: '' },
  icon: { type: String, default: '' },
  imageSrc: { type: String, default: '' },
  imageAlt: { type: String, default: '' },
  variant: { type: String, default: 'accent' },
  arrowIcon: { type: String, default: 'fas fa-external-link-alt' },
  arrowClass: { type: String, default: '' },
  compact: { type: Boolean, default: false },
  target: { type: String, default: '_blank' },
})

defineEmits(['activate'])

const classes = computed(() => [
  `resource-link--${props.variant}`,
  { 'resource-link--compact': props.compact },
])

const rel = computed(() => (props.target === '_blank' ? 'noopener noreferrer' : undefined))
</script>

<template>
  <a
    class="resource-link"
    :class="classes"
    :href="href"
    :target="target || undefined"
    :rel="rel"
    @click="$emit('activate', $event)"
  >
    <span class="resource-icon" :class="{ 'resource-icon--logo': imageSrc }">
      <img v-if="imageSrc" class="resource-logo" :src="imageSrc" :alt="imageAlt" />
      <i v-else :class="icon" aria-hidden="true"></i>
    </span>
    <span class="resource-content">
      <span class="resource-title">{{ title }}</span>
      <span v-if="description" class="resource-desc">{{ description }}</span>
    </span>
    <i class="resource-arrow" :class="[arrowIcon, arrowClass]" aria-hidden="true"></i>
  </a>
</template>

<style scoped>
.resource-link {
  --link-color: var(--ui-accent-rgb);
  --icon-gradient: linear-gradient(135deg, var(--ui-accent), var(--ui-accent-secondary));
  display: flex;
  align-items: center;
  padding: 0.6em 0.8em;
  border: 1px solid var(--ui-border);
  border-radius: var(--ui-radius-sm);
  background: linear-gradient(135deg, rgba(var(--link-color), 0.15), rgba(var(--link-color), 0.08));
  color: var(--ui-text-primary);
  text-decoration: none;
  transition: transform 0.2s ease, box-shadow 0.2s ease, border-color 0.2s ease;
}

.resource-link:hover,
.resource-link:focus-visible {
  border-color: rgba(var(--link-color), 0.4);
  box-shadow: var(--ui-shadow-sm);
  color: var(--ui-text-primary);
  text-decoration: none;
  transform: translateY(-1px);
}

.resource-link--compact {
  min-height: 2.75rem;
  padding: 0.35rem 0.45rem;
}

.resource-icon {
  width: 36px;
  height: 36px;
  display: flex;
  flex: 0 0 auto;
  align-items: center;
  justify-content: center;
  margin-right: 0.8em;
  border-radius: 8px;
  background: var(--icon-gradient);
  color: #fff;
  font-size: 1.1rem;
}

.resource-link--compact .resource-icon {
  width: 28px;
  height: 28px;
  margin-right: 0.5rem;
  border-radius: 7px;
  font-size: 0.85rem;
}

.resource-icon--logo {
  width: 86px;
  height: 44px;
  padding: 4px;
  overflow: visible;
  background: #fff;
  box-shadow: inset 0 0 0 1px rgba(0, 0, 0, 0.06);
}

.resource-logo {
  width: auto;
  max-width: 100%;
  height: auto;
  max-height: 100%;
  object-fit: contain;
}

.resource-content {
  min-width: 0;
  flex: 1;
}

.resource-title,
.resource-desc {
  display: block;
}

.resource-title {
  margin-bottom: 1px;
  color: var(--ui-text-primary);
  font-size: 0.9rem;
  font-weight: 600;
}

.resource-desc {
  color: var(--ui-text-secondary);
  font-size: 0.75rem;
}

.resource-link--compact .resource-title {
  margin-bottom: 2px;
  font-size: 0.76rem;
}

.resource-link--compact .resource-desc {
  overflow: hidden;
  font-size: 0.65rem;
  line-height: 1.25;
  text-overflow: ellipsis;
  white-space: nowrap;
}

.resource-arrow {
  margin-left: 0.5rem;
  color: var(--ui-text-muted);
  font-size: 0.8rem;
  transition: transform 0.2s ease;
}

.resource-link:hover .resource-arrow,
.resource-link:focus-visible .resource-arrow {
  transform: translateX(3px);
}

.resource-link--accent-alt {
  --icon-gradient: linear-gradient(135deg, var(--ui-accent-secondary), var(--ui-accent));
}

.resource-link--android {
  --link-color: 61, 220, 132;
  --icon-gradient: linear-gradient(135deg, #3ddc84, #00c853);
}

.resource-link--apple {
  --link-color: 128, 128, 128;
  --icon-gradient: linear-gradient(135deg, #555, #777);
}

.resource-link--desktop,
.resource-link--github {
  --link-color: 108, 117, 125;
}

.resource-link--desktop {
  --icon-gradient: linear-gradient(135deg, #6c757d, #495057);
}

.resource-link--github {
  --icon-gradient: linear-gradient(135deg, #6c757d, #868e96);
}

.resource-link--harmony {
  --link-color: 206, 48, 48;
  --icon-gradient: linear-gradient(135deg, #ce3030, #e74c3c);
}

.resource-link--moonlink {
  --link-color: 111, 66, 193;
  --icon-gradient: linear-gradient(135deg, #6f42c1, #4c2f8f);
}

[data-bs-theme='dark'] .resource-link {
  background: linear-gradient(135deg, rgba(var(--link-color), 0.2), rgba(var(--link-color), 0.1));
}

[data-bs-theme='dark'] .resource-link--apple {
  --link-color: 170, 170, 170;
  --icon-gradient: linear-gradient(135deg, #aaa, #ccc);
}

[data-bs-theme='dark'] .resource-link--apple .resource-icon {
  color: #222;
}

[data-bs-theme='dark'] .resource-link--desktop,
[data-bs-theme='dark'] .resource-link--github {
  --link-color: 150, 160, 170;
}

[data-bs-theme='dark'] .resource-link--desktop {
  --icon-gradient: linear-gradient(135deg, #8c959d, #6c757d);
}

[data-bs-theme='dark'] .resource-link--github {
  --icon-gradient: linear-gradient(135deg, #8c959d, #adb5bd);
}

@media (max-width: 575.98px) {
  .resource-link--compact .resource-desc {
    white-space: normal;
  }
}
</style>

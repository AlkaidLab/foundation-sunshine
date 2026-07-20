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
  action: { type: Boolean, default: false },
})

defineEmits(['activate'])

const classes = computed(() => [
  `resource-link--${props.variant}`,
  { 'resource-link--compact': props.compact },
])

const rel = computed(() => (props.target === '_blank' ? 'noopener noreferrer' : undefined))
</script>

<template>
  <component
    :is="action ? 'button' : 'a'"
    class="resource-link"
    :class="classes"
    :href="action ? undefined : href"
    :type="action ? 'button' : undefined"
    :target="action ? undefined : target || undefined"
    :rel="action ? undefined : rel"
    :aria-haspopup="action ? 'dialog' : undefined"
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
  </component>
</template>

<style scoped>
.resource-link {
  --link-color: var(--ui-accent-rgb);
  --link-foreground: var(--ui-accent);
  display: flex;
  align-items: center;
  padding: 0.6em 0.8em;
  border: 1px solid var(--ui-border);
  border-radius: var(--ui-radius-sm);
  background: linear-gradient(
    135deg,
    color-mix(in srgb, var(--ui-surface-strong) 86%, rgba(var(--link-color), 0.18)),
    color-mix(in srgb, var(--ui-surface) 92%, transparent)
  );
  color: var(--ui-text-primary);
  isolation: isolate;
  position: relative;
  text-decoration: none;
  font: inherit;
  text-align: left;
  cursor: pointer;
  appearance: none;
  transition: transform 0.2s ease, box-shadow 0.2s ease, border-color 0.2s ease;
}

.resource-link::before {
  position: absolute;
  z-index: -1;
  inset: 0;
  border-radius: inherit;
  background: color-mix(in srgb, var(--ui-surface-hover) 90%, rgba(var(--link-color), 0.12));
  content: '';
  opacity: 0;
  pointer-events: none;
  transition: opacity 0.2s ease;
}

.resource-link:hover,
.resource-link:focus-visible {
  border-color: rgba(var(--link-color), 0.4);
  box-shadow: var(--ui-shadow-sm);
  color: var(--ui-text-primary);
  text-decoration: none;
  transform: translateY(-1px);
}

.resource-link:hover::before,
.resource-link:focus-visible::before {
  opacity: 1;
}

.resource-link--compact {
  min-height: 2.6rem;
  padding: 0.3rem 0.4rem;
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
  border: 1px solid rgba(var(--link-color), 0.2);
  background: rgba(var(--link-color), 0.12);
  color: var(--link-foreground);
  font-size: 1.1rem;
}

.resource-link--compact .resource-icon {
  width: 27px;
  height: 27px;
  margin-right: 0.45rem;
  border-radius: 0.56rem;
  font-size: 0.76rem;
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
  margin-bottom: 1px;
  font-size: 0.72rem;
}

.resource-link--compact .resource-desc {
  overflow: hidden;
  font-size: 0.61rem;
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
  --link-color: var(--ui-info-rgb);
  --link-foreground: var(--ui-info-text);
}

.resource-link--android {
  --link-color: var(--ui-success-rgb);
  --link-foreground: var(--ui-success-text);
}

.resource-link--apple {
  --link-color: 100, 116, 139;
  --link-foreground: var(--ui-text-secondary);
}

.resource-link--desktop,
.resource-link--github {
  --link-color: 100, 116, 139;
  --link-foreground: var(--ui-text-secondary);
}

.resource-link--harmony {
  --link-color: var(--ui-info-rgb);
  --link-foreground: var(--ui-info-text);
}

.resource-link--moonlink {
  --link-color: 142, 126, 173;
  --link-foreground: var(--ui-text-secondary);
}

[data-bs-theme='dark'] .resource-link {
  background: linear-gradient(135deg, rgba(var(--link-color), 0.13), color-mix(in srgb, var(--ui-surface) 86%, transparent));
}

[data-bs-theme='dark'] .resource-link--apple {
  --link-color: 184, 171, 154;
}

[data-bs-theme='dark'] .resource-link--desktop,
[data-bs-theme='dark'] .resource-link--github {
  --link-color: 184, 171, 154;
}

@media (max-width: 575.98px) {
  .resource-link--compact .resource-desc {
    white-space: normal;
  }
}
</style>

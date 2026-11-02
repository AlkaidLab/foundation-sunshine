<template>
  <aside v-if="fatalLogs.length" class="startup-alert" role="alert">
    <p class="startup-alert-heading">『<span v-html="$t('index.startup_errors')"></span>』</p>
    <ul class="startup-alert-list">
      <li v-for="log in fatalLogs" :key="log.timestamp" class="startup-alert-item">
        <span class="startup-alert-marker" aria-hidden="true"></span>
        <span>{{ log.value }}</span>
      </li>
    </ul>
    <p class="startup-alert-foot">
      <a class="startup-alert-action" href="/troubleshooting/#logs">
        <span>{{ $t('index.view_logs') }}</span>
        <i class="fas fa-chevron-right startup-alert-arrow" aria-hidden="true"></i>
      </a>
    </p>
  </aside>
</template>

<script setup>
defineProps({
  fatalLogs: {
    type: Array,
    required: true
  }
})
</script>

<style scoped lang="less">
.startup-alert {
  margin-top: 2.4rem;
  margin-bottom: 0.9rem;
  padding-left: 1.15rem;
  border-left: 3px solid color-mix(in srgb, var(--ui-danger) 70%, transparent);
  color: var(--home-heading, var(--ui-text-primary));
}

.startup-alert-heading {
  margin: 0;
  display: inline-block;
  transform: skewX(-5deg);
  color: var(--ui-danger-text);
  font-size: 16px;
  font-weight: 800;
  letter-spacing: 0.08em;
  text-shadow:
    0 0 10px color-mix(in srgb, var(--ui-danger-text) 55%, transparent),
    0 0 26px color-mix(in srgb, var(--ui-danger-text) 30%, transparent);
}

.startup-alert-heading :deep(b),
.startup-alert-heading :deep(strong) {
  color: inherit;
}

.startup-alert-list {
  margin: 0.7rem 0 0;
  padding: 0;
  list-style: none;
  display: grid;
  gap: 0.45rem;
}

.startup-alert-item {
  display: grid;
  min-width: 0;
  grid-template-columns: 0.42rem 1fr;
  gap: 0.55rem;
  align-items: baseline;
  color: var(--home-body, var(--ui-text-secondary));
  font-size: 13px;
  line-height: 1.4;
  overflow-wrap: anywhere;
}

.startup-alert-marker {
  width: 0.36rem;
  height: 0.36rem;
  border-radius: 50%;
  background: var(--ui-danger-text);
  box-shadow:
    0 0 8px color-mix(in srgb, var(--ui-danger-text) 55%, transparent),
    0 0 0 0 color-mix(in srgb, var(--ui-danger-text) 40%, transparent);
  animation: startup-alert-pulse 2.4s ease-in-out infinite;
}

@keyframes startup-alert-pulse {
  0%,
  100% {
    box-shadow:
      0 0 8px color-mix(in srgb, var(--ui-danger-text) 55%, transparent),
      0 0 0 0 color-mix(in srgb, var(--ui-danger-text) 40%, transparent);
  }
  55% {
    box-shadow:
      0 0 8px color-mix(in srgb, var(--ui-danger-text) 55%, transparent),
      0 0 0 6px transparent;
  }
}

.startup-alert-foot {
  margin: 0.75rem 0 0;
}

.startup-alert-action {
  display: inline-flex;
  align-items: center;
  gap: 0.42rem;
  padding: 0.35rem 0;
  color: var(--ui-danger-text);
  font-size: 13px;
  font-weight: 600;
  text-decoration: none;
  text-shadow: 0 0 10px color-mix(in srgb, var(--ui-danger-text) 35%, transparent);
  white-space: nowrap;
}

.startup-alert-action:hover,
.startup-alert-action:focus-visible {
  text-decoration: underline;
  text-underline-offset: 3px;
}

.startup-alert-action:focus-visible {
  outline: 2px solid var(--ui-accent);
  outline-offset: 3px;
}

.startup-alert-arrow {
  font-size: 0.56rem;
  opacity: 0.72;
  transition: transform 0.2s ease;
}

.startup-alert-action:hover .startup-alert-arrow,
.startup-alert-action:focus-visible .startup-alert-arrow {
  transform: translateX(2px);
}

@media (prefers-reduced-motion: reduce) {
  .startup-alert-marker {
    animation: none;
  }
}
</style>


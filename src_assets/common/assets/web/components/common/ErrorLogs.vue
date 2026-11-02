<template>
  <aside v-if="fatalLogs.length" class="startup-alert" role="alert">
    <span class="startup-alert-icon" aria-hidden="true">
      <i class="fas fa-triangle-exclamation"></i>
    </span>

    <div class="startup-alert-content">
      <p class="startup-alert-heading" v-html="$t('index.startup_errors')"></p>
      <ul class="startup-alert-list">
        <li v-for="log in fatalLogs" :key="log.timestamp" class="startup-alert-item">
          <span class="startup-alert-marker" aria-hidden="true"></span>
          <span>{{ log.value }}</span>
        </li>
      </ul>
    </div>

    <a class="startup-alert-action" href="/troubleshooting/#logs">
      <i class="fas fa-list-ul" aria-hidden="true"></i>
      <span>{{ $t('index.view_logs') }}</span>
      <i class="fas fa-chevron-right startup-alert-arrow" aria-hidden="true"></i>
    </a>
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
  display: grid;
  grid-template-columns: auto minmax(0, 1fr) auto;
  gap: 0.72rem;
  align-items: start;
  margin-bottom: 0.9rem;
  padding: 0.85rem 1rem;
  border-radius: var(--ui-radius-md);
  background: color-mix(in srgb, var(--ui-danger-soft) 62%, transparent);
  color: var(--ui-text-primary);
}

.startup-alert-icon {
  display: inline-flex;
  padding-top: 0.12rem;
  color: var(--ui-danger-text);
  font-size: 0.95rem;
}

.startup-alert-content {
  min-width: 0;
}

.startup-alert-heading {
  margin: 0.05rem 0 0;
  color: var(--ui-text-primary);
  font-size: var(--font-size-sm);
  line-height: 1.45;
}

.startup-alert-heading :deep(b),
.startup-alert-heading :deep(strong) {
  color: var(--ui-danger-text);
  font-weight: 700;
}

.startup-alert-list {
  display: grid;
  gap: 0.3rem;
  margin: 0.5rem 0 0;
  padding: 0;
  list-style: none;
}

.startup-alert-item {
  display: grid;
  min-width: 0;
  grid-template-columns: 0.42rem minmax(0, 1fr);
  gap: 0.5rem;
  align-items: baseline;
  padding: 0;
  color: var(--ui-text-secondary);
  font-size: var(--font-size-xs);
  line-height: 1.4;
  overflow-wrap: anywhere;
}

.startup-alert-marker {
  width: 0.34rem;
  height: 0.34rem;
  border-radius: 50%;
  background: var(--ui-danger-text);
}

.startup-alert-action {
  display: inline-flex;
  min-height: 2.15rem;
  align-items: center;
  gap: 0.42rem;
  padding: 0.35rem 0;
  border: 0;
  background: none;
  color: var(--ui-danger-text);
  font-size: var(--font-size-xs);
  font-weight: 650;
  text-decoration: none;
  white-space: nowrap;
  transition: color 150ms ease;
}

.startup-alert-action:hover,
.startup-alert-action:focus-visible {
  color: var(--ui-danger-text);
  text-decoration: underline;
  text-underline-offset: 3px;
}

.startup-alert-action:focus-visible {
  outline: 2px solid var(--ui-danger-text);
  outline-offset: 2px;
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

@media (max-width: 767.98px) {
  .startup-alert {
    grid-template-columns: auto minmax(0, 1fr);
    padding: 0.8rem;
  }

  .startup-alert-action {
    grid-column: 2;
    justify-self: start;
  }
}
</style>


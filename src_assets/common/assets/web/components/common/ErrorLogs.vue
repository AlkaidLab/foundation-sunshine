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
  padding: 0.78rem 0.85rem;
  border: 1px solid color-mix(in srgb, var(--ui-danger-border) 72%, var(--ui-border));
  border-radius: var(--ui-radius-lg);
  background: linear-gradient(
    135deg,
    color-mix(in srgb, var(--ui-danger-soft) 66%, var(--ui-surface-strong)),
    color-mix(in srgb, var(--ui-surface) 92%, transparent)
  );
  box-shadow: var(--ui-shadow-sm);
  color: var(--ui-text-primary);
  backdrop-filter: blur(14px) saturate(1.05);
  -webkit-backdrop-filter: blur(14px) saturate(1.05);
}

.startup-alert-icon {
  display: inline-flex;
  width: 2.15rem;
  height: 2.15rem;
  align-items: center;
  justify-content: center;
  border: 1px solid var(--ui-danger-border);
  border-radius: 0.75rem;
  background: var(--ui-danger-soft);
  color: var(--ui-danger-text);
  font-size: 0.88rem;
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
  gap: 0.28rem;
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
  padding: 0.38rem 0.5rem;
  border-radius: var(--ui-radius-sm);
  background: color-mix(in srgb, var(--ui-surface-strong) 76%, transparent);
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
  box-shadow: 0 0 0 0.18rem var(--ui-danger-soft);
}

.startup-alert-action {
  display: inline-flex;
  min-height: 2.15rem;
  align-items: center;
  justify-content: center;
  gap: 0.42rem;
  padding: 0.42rem 0.62rem;
  border: 1px solid var(--ui-danger-border);
  border-radius: var(--ui-radius-md);
  background: color-mix(in srgb, var(--ui-surface-strong) 82%, transparent);
  color: var(--ui-danger-text);
  font-size: var(--font-size-xs);
  font-weight: 650;
  text-decoration: none;
  white-space: nowrap;
  transition:
    transform 0.2s ease,
    background-color 0.2s ease,
    border-color 0.2s ease,
    box-shadow 0.2s ease;
}

.startup-alert-action:hover,
.startup-alert-action:focus-visible {
  border-color: var(--ui-danger-text);
  background: var(--ui-danger-soft);
  color: var(--ui-danger-text);
  box-shadow: 0 4px 14px color-mix(in srgb, var(--ui-danger-soft) 72%, transparent);
  transform: translateY(-1px);
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
    padding: 0.72rem;
  }

  .startup-alert-action {
    grid-column: 2;
    justify-self: start;
  }
}
</style>


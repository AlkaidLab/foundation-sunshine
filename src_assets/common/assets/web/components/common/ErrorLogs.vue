<template>
  <div v-if="fatalLogs.length > 0" class="alert alert-danger">
    <div style="line-height: 32px">
      <i class="fas fa-circle-exclamation" style="font-size: 32px; margin-right: 0.25em"></i>
      <p v-html="$t('index.startup_errors')"></p>
      <br />
    </div>
    <ul>
      <li v-for="log in fatalLogs" :key="log.timestamp">{{ log.value }}</li>
    </ul>
    <a class="btn btn-danger" href="/troubleshooting/#logs">
      {{ $t('index.view_logs') || 'View Logs' }}
    </a>
  </div>
</template>

<script setup>
defineProps({
  fatalLogs: {
    type: Array,
    required: true,
    default: () => []
  }
})
</script>

<style scoped>
.alert-danger {
  margin-bottom: 1.5rem;
}
</style>


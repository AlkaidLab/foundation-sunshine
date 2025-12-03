<template>
  <div class="card shadow-sm mb-4" v-if="version">
    <div class="card-header bg-info bg-opacity-10 border-bottom-0">
      <h5 class="card-title mb-0">
        <i class="fas fa-code-branch text-info me-2"></i>
        Version {{ version.version }}
      </h5>
    </div>
    <div class="card-body">
      <!-- 加载状态 -->
      <div v-if="loading" class="version-loading">
        <i class="fas fa-spinner fa-spin me-2"></i>
        {{ $t('index.loading_latest') }}
      </div>

      <!-- 开发版本标识 -->
      <div class="version-alert version-alert-success" v-if="buildVersionIsDirty">
        <i class="fas fa-code me-2"></i>
        {{ $t('index.version_dirty') }} 🌇
      </div>

      <!-- 已安装版本不是稳定版 -->
      <div class="version-alert version-alert-info" v-if="installedVersionNotStable">
        <i class="fas fa-info-circle me-2"></i>
        {{ $t('index.installed_version_not_stable') }}
      </div>

      <!-- 已是最新版本 -->
      <div
        v-else-if="(!preReleaseBuildAvailable || !notifyPreReleases) && !stableBuildAvailable && !buildVersionIsDirty"
        class="version-alert version-alert-success"
      >
        <i class="fas fa-check-circle me-2"></i>
        {{ $t('index.version_latest') }}
      </div>

      <!-- 预发布版本可用 -->
      <div v-if="notifyPreReleases && preReleaseBuildAvailable" class="version-update">
        <div class="version-update-header">
          <div class="version-update-title">
            <i class="fas fa-rocket text-warning me-2"></i>
            <span>有新的 <b>基地版</b> sunshine可以更新!</span>
          </div>
          <a class="btn btn-success btn-download" :href="preReleaseVersion.release.html_url" target="_blank">
            <i class="fas fa-download me-2"></i>
            {{ $t('index.download') }}
          </a>
        </div>
        <h3 class="version-release-name">{{ preReleaseVersion.release.name }}</h3>
        <div class="markdown-content" v-html="parsedPreReleaseBody"></div>
      </div>

      <!-- 稳定版本可用 -->
      <div v-if="stableBuildAvailable" class="version-update">
        <div class="version-update-header">
          <div class="version-update-title">
            <i class="fas fa-star text-warning me-2"></i>
            <span>{{ $t('index.new_stable') }}</span>
          </div>
          <a class="btn btn-success btn-download" :href="githubVersion.release.html_url" target="_blank">
            <i class="fas fa-download me-2"></i>
            {{ $t('index.download') }}
          </a>
        </div>
        <h3 class="version-release-name">{{ githubVersion.release.name }}</h3>
        <div class="markdown-content" v-html="parsedStableBody"></div>
      </div>
    </div>
  </div>
</template>

<script setup>
defineProps({
  version: Object,
  githubVersion: Object,
  preReleaseVersion: Object,
  notifyPreReleases: Boolean,
  loading: Boolean,
  installedVersionNotStable: Boolean,
  stableBuildAvailable: Boolean,
  preReleaseBuildAvailable: Boolean,
  buildVersionIsDirty: Boolean,
  parsedStableBody: String,
  parsedPreReleaseBody: String,
})
</script>

<style scoped>
/* Loading State */
.version-loading {
  display: flex;
  align-items: center;
  padding: 1rem;
  color: var(--bs-secondary-color, #6c757d);
  font-size: 0.95rem;
}

/* Version Alerts */
.version-alert {
  border-radius: 8px;
  font-size: 0.9rem;
  padding: 0.75rem 1rem;
  margin-bottom: 1rem;
  display: flex;
  align-items: center;
  border: none;
}

.version-alert-success {
  background: rgba(40, 167, 69, 0.1);
  color: #155724;
  border-left: 4px solid #28a745;
}

.version-alert-info {
  background: rgba(0, 123, 255, 0.1);
  color: #004085;
  border-left: 4px solid #007bff;
}

[data-bs-theme='dark'] .version-alert-success {
  background: rgba(40, 167, 69, 0.2);
  color: #6cff8f;
}

[data-bs-theme='dark'] .version-alert-info {
  background: rgba(0, 123, 255, 0.2);
  color: #6cb2ff;
}

/* Version Update Section */
.version-update {
  background: linear-gradient(135deg, rgba(255, 193, 7, 0.1) 0%, rgba(255, 193, 7, 0.05) 100%);
  border: 1px solid rgba(255, 193, 7, 0.3);
  border-radius: 10px;
  padding: 1.25rem;
  margin-top: 1rem;
}

.version-update-header {
  display: flex;
  justify-content: space-between;
  align-items: center;
  flex-wrap: wrap;
  gap: 1rem;
  margin-bottom: 1rem;
}

.version-update-title {
  display: flex;
  align-items: center;
  font-size: 1rem;
  font-weight: 600;
  color: var(--bs-body-color, #2c3e50);
  flex: 1;
  min-width: 200px;
}

.btn-download {
  border-radius: 8px;
  padding: 0.5rem 1.25rem;
  font-weight: 600;
  transition: transform 0.2s ease, box-shadow 0.2s ease;
  white-space: nowrap;
}

.btn-download:hover {
  transform: translateY(-2px);
  box-shadow: 0 4px 12px rgba(40, 167, 69, 0.4);
}

.version-release-name {
  font-size: 1.3rem;
  font-weight: 600;
  margin: 1rem 0 0.75rem 0;
  color: var(--bs-body-color, #2c3e50);
}

/* Markdown Content */
.markdown-content {
  background: rgba(0, 0, 0, 0.03);
  border-radius: 8px;
  padding: 1.25rem;
  margin-top: 1rem;
  line-height: 1.6;
  border: 1px solid rgba(0, 0, 0, 0.05);
}

[data-bs-theme='dark'] .markdown-content {
  background: rgba(255, 255, 255, 0.05);
  border-color: rgba(255, 255, 255, 0.1);
}

.markdown-content h1,
.markdown-content h2,
.markdown-content h3,
.markdown-content h4,
.markdown-content h5,
.markdown-content h6 {
  margin-top: 1.25rem;
  margin-bottom: 0.75rem;
  font-weight: 600;
  line-height: 1.25;
  color: var(--bs-body-color, #2c3e50);
}

.markdown-content h1:first-child,
.markdown-content h2:first-child,
.markdown-content h3:first-child {
  margin-top: 0;
}

.markdown-content h1 {
  font-size: 1.5em;
}

.markdown-content h2 {
  font-size: 1.3em;
}

.markdown-content h3 {
  font-size: 1.1em;
}

.markdown-content p {
  margin-bottom: 0.75rem;
  white-space: pre-line;
  color: var(--bs-body-color, #495057);
}

.markdown-content ul,
.markdown-content ol {
  margin-bottom: 0.75rem;
  padding-left: 1.5rem;
}

.markdown-content li {
  margin-bottom: 0.5rem;
  color: var(--bs-body-color, #495057);
}

.markdown-content code {
  background: rgba(0, 0, 0, 0.08);
  padding: 0.2em 0.4em;
  border-radius: 4px;
  font-family: 'Courier New', 'Consolas', 'Monaco', monospace;
  font-size: 0.9em;
  color: #e83e8c;
}

[data-bs-theme='dark'] .markdown-content code {
  background: rgba(255, 255, 255, 0.15);
  color: #ff6b9d;
}

.markdown-content pre {
  background: rgba(0, 0, 0, 0.08);
  padding: 1rem;
  border-radius: 8px;
  overflow-x: auto;
  margin: 1rem 0;
  border: 1px solid rgba(0, 0, 0, 0.1);
}

[data-bs-theme='dark'] .markdown-content pre {
  background: rgba(255, 255, 255, 0.1);
  border-color: rgba(255, 255, 255, 0.15);
}

.markdown-content pre code {
  background: none;
  padding: 0;
  color: inherit;
}

.markdown-content blockquote {
  border-left: 4px solid #007bff;
  margin: 1rem 0;
  padding-left: 1rem;
  color: var(--bs-secondary-color, #6c757d);
  font-style: italic;
}

.markdown-content a {
  color: #007bff;
  text-decoration: none;
  font-weight: 500;
  transition: color 0.2s ease;
}

.markdown-content a:hover {
  color: #0056b3;
  text-decoration: underline;
}

.markdown-content table {
  border-collapse: collapse;
  width: 100%;
  margin: 1rem 0;
  border-radius: 8px;
  overflow: hidden;
}

.markdown-content th,
.markdown-content td {
  border: 1px solid rgba(0, 0, 0, 0.1);
  padding: 0.75rem 1rem;
  text-align: left;
}

[data-bs-theme='dark'] .markdown-content th,
[data-bs-theme='dark'] .markdown-content td {
  border-color: rgba(255, 255, 255, 0.15);
}

.markdown-content th {
  background: rgba(0, 0, 0, 0.05);
  font-weight: 600;
}

[data-bs-theme='dark'] .markdown-content th {
  background: rgba(255, 255, 255, 0.1);
}

/* Dark Mode Adjustments */
[data-bs-theme='dark'] .version-update {
  background: linear-gradient(135deg, rgba(255, 193, 7, 0.15) 0%, rgba(255, 193, 7, 0.08) 100%);
  border-color: rgba(255, 193, 7, 0.3);
}

[data-bs-theme='dark'] .version-update-title {
  color: #e0e0e0;
}

[data-bs-theme='dark'] .version-release-name {
  color: #e0e0e0;
}
</style>

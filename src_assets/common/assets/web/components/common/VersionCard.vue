<template>
  <div class="card p-2 my-4">
    <div class="card-body" v-if="version">
      <h2>Version {{ version.version }}</h2>
      <br />
      
      <!-- 加载状态 -->
      <div v-if="loading">{{ $t('index.loading_latest') }}</div>
      
      <!-- 开发版本标识 -->
      <div class="alert alert-success" v-if="buildVersionIsDirty">
        {{ $t('index.version_dirty') }} 🌇
      </div>
      
      <!-- 已安装版本不是稳定版 -->
      <div class="alert alert-info" v-if="installedVersionNotStable">
        {{ $t('index.installed_version_not_stable') }}
      </div>
      
      <!-- 已是最新版本 -->
      <div
        v-else-if="(!preReleaseBuildAvailable || !notifyPreReleases) && !stableBuildAvailable && !buildVersionIsDirty"
      >
        <div class="alert alert-success">{{ $t('index.version_latest') }}</div>
      </div>
      
      <!-- 预发布版本可用 -->
      <div v-if="notifyPreReleases && preReleaseBuildAvailable">
        <div class="alert alert-warning">
          <div class="d-flex justify-content-between">
            <div class="my-2">有新的 <b>基地版</b> sunshine可以更新!</div>
            <a 
              class="btn btn-success m-1" 
              :href="preReleaseVersion.release.html_url" 
              target="_blank"
            >
              {{ $t('index.download') }}
            </a>
          </div>
          <h3>{{ preReleaseVersion.release.name }}</h3>
          <div class="markdown-content" v-html="parsedPreReleaseBody"></div>
        </div>
      </div>
      
      <!-- 稳定版本可用 -->
      <div v-if="stableBuildAvailable">
        <div class="alert alert-warning">
          <div class="d-flex justify-content-between">
            <div class="my-2">{{ $t('index.new_stable') }}</div>
            <a 
              class="btn btn-success m-1" 
              :href="githubVersion.release.html_url" 
              target="_blank"
            >
              {{ $t('index.download') }}
            </a>
          </div>
          <h3>{{ githubVersion.release.name }}</h3>
          <div class="markdown-content" v-html="parsedStableBody"></div>
        </div>
      </div>
    </div>
  </div>
</template>

<script setup>
defineProps({
  version: {
    type: Object,
    default: null
  },
  githubVersion: {
    type: Object,
    default: null
  },
  preReleaseVersion: {
    type: Object,
    default: null
  },
  notifyPreReleases: {
    type: Boolean,
    default: false
  },
  loading: {
    type: Boolean,
    default: false
  },
  installedVersionNotStable: {
    type: Boolean,
    default: false
  },
  stableBuildAvailable: {
    type: Boolean,
    default: false
  },
  preReleaseBuildAvailable: {
    type: Boolean,
    default: false
  },
  buildVersionIsDirty: {
    type: Boolean,
    default: false
  },
  parsedStableBody: {
    type: String,
    default: ''
  },
  parsedPreReleaseBody: {
    type: String,
    default: ''
  }
})
</script>

<style scoped>
.markdown-content {
  background: rgba(0, 0, 0, 0.05);
  border-radius: 6px;
  padding: 16px;
  margin-top: 12px;
  line-height: 1.6;
}

.markdown-content h1,
.markdown-content h2,
.markdown-content h3,
.markdown-content h4,
.markdown-content h5,
.markdown-content h6 {
  margin-top: 16px;
  margin-bottom: 8px;
  font-weight: 600;
  line-height: 1.25;
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
  margin-bottom: 12px;
  white-space: pre-line;
}

.markdown-content ul,
.markdown-content ol {
  margin-bottom: 12px;
  padding-left: 24px;
}

.markdown-content li {
  margin-bottom: 4px;
}

.markdown-content code {
  background: rgba(0, 0, 0, 0.1);
  padding: 2px 6px;
  border-radius: 3px;
  font-family: 'Courier New', monospace;
  font-size: 0.9em;
}

.markdown-content pre {
  background: rgba(0, 0, 0, 0.1);
  padding: 12px;
  border-radius: 6px;
  overflow-x: auto;
  margin: 12px 0;
}

.markdown-content pre code {
  background: none;
  padding: 0;
}

.markdown-content blockquote {
  border-left: 4px solid #007bff;
  margin: 12px 0;
  padding-left: 16px;
  color: #6c757d;
}

.markdown-content a {
  color: #007bff;
  text-decoration: none;
}

.markdown-content a:hover {
  text-decoration: underline;
}

.markdown-content table {
  border-collapse: collapse;
  width: 100%;
  margin: 12px 0;
}

.markdown-content th,
.markdown-content td {
  border: 1px solid #dee2e6;
  padding: 8px 12px;
  text-align: left;
}

.markdown-content th {
  background: rgba(0, 0, 0, 0.05);
  font-weight: 600;
}
</style>


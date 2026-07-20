<template>
  <div class="resource-section">
    <!-- Resources Card -->
    <div class="card shadow-sm">
      <div class="card-header resource-card-header">
        <h5 class="card-title mb-0">
          <i class="fas fa-book-open me-2"></i>
          {{ $t('resource_card.resources') }}
        </h5>
      </div>
      <div class="card-body">
        <div class="resource-groups">

        <!-- 基地官网 -->
        <div class="resource-group resource-group--official">
          <h6 class="resource-group-title">
            <i class="fas fa-globe text-primary me-2"></i>
            {{ $t('resource_card.official_website') }}
          </h6>
          <div class="row g-3">
            <div class="col-md-6">
              <a class="resource-link resource-link-primary" :href="officialWebsiteUrl" target="_blank">
                <div class="resource-icon">
                  <i class="fas fa-globe"></i>
                </div>
                <div class="resource-content">
                  <span class="resource-title">{{ $t('resource_card.official_website_title') }}</span>
                </div>
                <i class="fas fa-external-link-alt resource-arrow"></i>
              </a>
            </div>
            <div class="col-md-6">
              <a
                class="resource-link resource-link-github"
                href="https://github.com/AlkaidLab/foundation-sunshine"
                target="_blank"
              >
                <div class="resource-icon">
                  <i class="fab fa-github"></i>
                </div>
                <div class="resource-content">
                  <span class="resource-title">Sunshine Foundation</span>
                  <span class="resource-desc">{{ $t('resource_card.open_source_desc') }}</span>
                </div>
                <i class="fas fa-star resource-arrow text-warning"></i>
              </a>
            </div>
          </div>
        </div>

        <!-- 快速入门 -->
        <div class="resource-group resource-group--quick-start">
          <h6 class="resource-group-title">
            <i class="fas fa-rocket me-2"></i>
            {{ $t('resource_card.quick_start') }}
          </h6>
          <div class="row g-3">
            <div class="col-md-6">
              <a
                class="resource-link resource-link-primary"
                href="https://docs.qq.com/aio/DSGdQc3htbFJjSFdO"
                target="_blank"
              >
                <div class="resource-icon">
                  <i class="fas fa-file-alt"></i>
                </div>
                <div class="resource-content">
                  <span class="resource-title">{{ $t('resource_card.tutorial') }}</span>
                  <span class="resource-desc">{{ $t('resource_card.tutorial_desc') }}</span>
                </div>
                <i class="fas fa-external-link-alt resource-arrow"></i>
              </a>
            </div>
            <div class="col-md-6">
              <a class="resource-link resource-link-info" href="https://qm.qq.com/q/3tWBFVNZ" target="_blank">
                <div class="resource-icon">
                  <i class="fab fa-qq"></i>
                </div>
                <div class="resource-content">
                  <span class="resource-title">{{ $t('resource_card.join_group') }}</span>
                  <span class="resource-desc">{{ $t('resource_card.join_group_desc') }}</span>
                </div>
                <i class="fas fa-external-link-alt resource-arrow"></i>
              </a>
            </div>
          </div>
        </div>

        <!-- 客户端下载 -->
        <div class="resource-group resource-group--clients">
          <h6 class="resource-group-title">
            <i class="fas fa-download text-primary me-2"></i>
            {{ $t('resource_card.client_downloads') }}
          </h6>
          <div class="row g-3">
            <div class="col-md-6 col-lg-4">
              <a
                class="resource-link resource-link-android"
                href="https://github.com/qiin2333/moonlight-vplus"
                target="_blank"
              >
                <div class="resource-icon">
                  <i class="fab fa-android"></i>
                </div>
                <div class="resource-content">
                  <span class="resource-title">{{ $t('resource_card.android_vplus_title') }}</span>
                  <span class="resource-desc">Android / Android TV</span>
                </div>
                <i class="fas fa-external-link-alt resource-arrow"></i>
              </a>
            </div>
            <div class="col-md-6 col-lg-4">
              <a
                class="resource-link resource-link-harmony"
                href="javascript:void(0)"
                @click.prevent="openHarmonyModal"
              >
                <div class="resource-icon">
                  <i class="fas fa-mobile-alt"></i>
                </div>
                <div class="resource-content">
                  <span class="resource-title">{{ $t('resource_card.harmony_client') }}</span>
                  <span class="resource-desc">HarmonyOS NEXT</span>
                </div>
                <i class="fas fa-external-link-alt resource-arrow"></i>
              </a>
            </div>
            <div class="col-md-6 col-lg-4">
              <a
                class="resource-link resource-link-desktop"
                href="https://github.com/qiin2333/moonlight-qt"
                target="_blank"
              >
                <div class="resource-icon">
                  <i class="fas fa-desktop"></i>
                </div>
                <div class="resource-content">
                  <span class="resource-title">{{ $t('resource_card.moonlight_pc_title') }}</span>
                  <span class="resource-desc">Windows / macOS / Linux</span>
                </div>
                <i class="fas fa-external-link-alt resource-arrow"></i>
              </a>
            </div>
            <div class="col-md-6 col-lg-4">
              <a
                class="resource-link resource-link-apple"
                href="https://github.com/skyhua0224/moonlight-macos-enhanced"
                target="_blank"
                rel="noopener noreferrer"
              >
                <div class="resource-icon">
                  <i class="fab fa-apple"></i>
                </div>
                <div class="resource-content">
                  <span class="resource-title">{{ $t('resource_card.moonlight_macos_enhanced') }}</span>
                  <span class="resource-desc">{{ $t('resource_card.moonlight_macos_enhanced_desc') }}</span>
                </div>
                <i class="fas fa-external-link-alt resource-arrow"></i>
              </a>
            </div>
            <div class="col-md-6 col-lg-4">
              <a
                class="resource-link resource-link-apple"
                href="https://apps.apple.com/cn/app/voidlink/id6747717070"
                target="_blank"
              >
                <div class="resource-icon">
                  <i class="fab fa-apple"></i>
                </div>
                <div class="resource-content">
                  <span class="resource-title">{{ $t('resource_card.voidlink_title') }}</span>
                  <span class="resource-desc">iOS / iPadOS</span>
                </div>
                <i class="fas fa-external-link-alt resource-arrow"></i>
              </a>
            </div>
          </div>
        </div>

        <!-- 友情链接 -->
        <div class="resource-group resource-group--community">
          <h6 class="resource-group-title">
            <i class="fas fa-code-branch me-2"></i>
            {{ $t('resource_card.third_party_moonlight') }}
          </h6>
          <div class="row g-3">
            <div class="col-12">
              <a
                class="resource-link resource-link-android"
                href="https://github.com/WACrown/moonlight-android"
                target="_blank"
              >
                <div class="resource-icon">
                  <i class="fas fa-crown"></i>
                </div>
                <div class="resource-content">
                  <span class="resource-title">{{ $t('resource_card.crown_edition') }}</span>
                  <span class="resource-desc">{{ $t('resource_card.crown_edition_desc') }}</span>
                </div>
                <i class="fas fa-external-link-alt resource-arrow"></i>
              </a>
            </div>
            <div class="col-12">
              <a
                class="resource-link resource-link-harmony"
                href="https://gitee.com/smdsbz/moonlight-ohos"
                target="_blank"
              >
                <div class="resource-icon">
                  <i class="fas fa-mobile-alt"></i>
                </div>
                <div class="resource-content">
                  <span class="resource-title">{{ $t('resource_card.moonlight_ohos') }}</span>
                  <span class="resource-desc">{{ $t('resource_card.moonlight_ohos_desc') }}</span>
                </div>
                <i class="fas fa-external-link-alt resource-arrow"></i>
              </a>
            </div>
          </div>
        </div>
        </div>
      </div>
    </div>

    <!-- Legal Card -->
    <div class="card shadow-sm mt-2 legal-card">
      <div class="card-body resource-legal-body">
        <div class="legal-summary">
          <h5 class="legal-title">
            <i class="fas fa-gavel me-2"></i>
            {{ $t('resource_card.legal') }}
          </h5>
          <p class="legal-copy">{{ $t('resource_card.legal_desc') }}</p>
        </div>

        <!-- GPL v3.0 Badge -->
        <div class="gpl-badge">
          <div class="d-flex align-items-center justify-content-center">
            <span class="badge-gpl">
              <i class="fas fa-balance-scale me-2"></i>
              GNU General Public License v3.0
            </span>
          </div>
          <p class="text-center small mt-2 mb-0">
            {{ $t('resource_card.gpl_license_text_1') }}
            <br />
            {{ $t('resource_card.gpl_license_text_2') }}
          </p>
        </div>

        <div class="row g-2 legal-links">
          <div class="col-md-6">
            <a
              class="resource-link resource-link-primary"
              href="https://github.com/qiin2333/Sunshine/blob/master/LICENSE"
              target="_blank"
            >
              <div class="resource-icon">
                <i class="fas fa-file-alt"></i>
              </div>
              <div class="resource-content">
                <span class="resource-title">{{ $t('resource_card.license') }}</span>
                <span class="resource-desc">{{ $t('resource_card.view_license') }}</span>
              </div>
              <i class="fas fa-external-link-alt resource-arrow"></i>
            </a>
          </div>
          <div class="col-md-6">
            <a
              class="resource-link resource-link-primary"
              href="https://github.com/qiin2333/Sunshine/blob/master/NOTICE"
              target="_blank"
            >
              <div class="resource-icon">
                <i class="fas fa-exclamation-triangle"></i>
              </div>
              <div class="resource-content">
                <span class="resource-title">{{ $t('resource_card.third_party_notice') }}</span>
                <span class="resource-desc">{{ $t('resource_card.third_party_desc') }}</span>
              </div>
              <i class="fas fa-external-link-alt resource-arrow"></i>
            </a>
          </div>
        </div>
      </div>
    </div>

    <!-- Harmony Link Modal -->
    <Transition name="fade">
      <div v-if="showHarmonyModal" class="harmony-modal-overlay" @click.self="closeHarmonyModal">
        <div class="harmony-modal">
          <div class="harmony-modal-header">
            <h5>{{ $t('resource_card.harmony_client') }}</h5>
            <button class="btn-close" @click="closeHarmonyModal"></button>
          </div>
          <div class="harmony-modal-body">
            <p>{{ $t('setup.harmony_modal_link_notice') }}</p>
            <p>{{ $t('setup.harmony_modal_desc') }}</p>
          </div>
          <div class="harmony-modal-footer">
            <button type="button" class="btn btn-secondary" @click="closeHarmonyModal">{{ $t('_common.cancel') }}</button>
            <button type="button" class="btn btn-primary" @click="confirmHarmonyLink">
              <i class="fas fa-external-link-alt me-1"></i>
              {{ $t('setup.harmony_goto_repo') }}
            </button>
          </div>
        </div>
      </div>
    </Transition>
  </div>
</template>

<script>
export default {
  name: 'ResourceCard',
  data() {
    return {
      showHarmonyModal: false,
    }
  },
  computed: {
    officialWebsiteUrl() {
      return 'https://www.alkaidlab.com/'
    }
  },
  methods: {
    openHarmonyModal() {
      this.showHarmonyModal = true
    },
    closeHarmonyModal() {
      this.showHarmonyModal = false
    },
    confirmHarmonyLink() {
      window.open('https://github.com/AlkaidLab/moonlight-harmony', '_blank')
      this.closeHarmonyModal()
    }
  }
}
</script>

<style scoped>
.resource-section > .card > .card-body {
  padding: 0.5rem;
}

.resource-groups {
  display: grid;
  grid-template-columns: repeat(12, minmax(0, 1fr));
  gap: 0.5rem;
}

.resource-group {
  min-width: 0;
  margin: 0 !important;
  padding: 0.4rem;
  border: 1px solid var(--ui-border);
  border-radius: var(--ui-radius-sm);
  background: color-mix(in srgb, var(--ui-surface) 70%, transparent);
}

.resource-group--official,
.resource-group--quick-start {
  grid-column: span 6;
}

.resource-group--clients {
  grid-column: span 8;
}

.resource-group--community {
  grid-column: span 4;
}

.resource-group .row {
  --bs-gutter-x: 0.5rem;
  --bs-gutter-y: 0.5rem;
}

.resource-group-title {
  font-size: 0.75rem;
  font-weight: 600;
  color: var(--ui-text-secondary);
  text-transform: uppercase;
  letter-spacing: 0.5px;
  margin-bottom: 0.35rem;
}

/* Resource Link Base */
.resource-link {
  display: flex;
  align-items: center;
  min-height: 2.75rem;
  padding: 0.35rem 0.45rem;
  border-radius: var(--ui-radius-sm);
  text-decoration: none;
  background: var(--ui-surface);
  border: 1px solid var(--ui-border);
  transition: transform 0.2s ease, box-shadow 0.2s ease, border-color 0.2s ease;
}

.resource-link:hover {
  transform: translateY(-1px);
  box-shadow: var(--ui-shadow-sm);
  text-decoration: none;
}

.resource-icon {
  width: 28px;
  height: 28px;
  border-radius: 7px;
  display: flex;
  align-items: center;
  justify-content: center;
  font-size: 0.85rem;
  flex-shrink: 0;
  margin-right: 0.5rem;
  color: white;
}

.resource-content {
  flex: 1;
  min-width: 0;
}

.resource-title {
  display: block;
  font-weight: 600;
  font-size: 0.76rem;
  color: var(--ui-text-primary);
  margin-bottom: 2px;
}

.resource-desc {
  display: block;
  overflow: hidden;
  font-size: 0.65rem;
  line-height: 1.25;
  text-overflow: ellipsis;
  white-space: nowrap;
  color: var(--ui-text-secondary);
}

.resource-arrow {
  font-size: 0.875rem;
  color: var(--ui-text-muted);
  margin-left: 0.5rem;
  transition: transform 0.2s ease;
}

.resource-link:hover .resource-arrow {
  transform: translateX(3px);
}

/* Color Variants - Using CSS Custom Properties */
.resource-link-primary {
  --link-color: var(--ui-accent-rgb);
  --icon-gradient: linear-gradient(135deg, var(--ui-accent) 0%, var(--ui-accent-secondary) 100%);
}

.resource-link-info {
  --link-color: var(--ui-accent-rgb);
  --icon-gradient: linear-gradient(135deg, var(--ui-accent-secondary) 0%, var(--ui-accent) 100%);
}

.resource-link-android {
  --link-color: 61, 220, 132;
  --icon-gradient: linear-gradient(135deg, #3ddc84 0%, #00c853 100%);
}

.resource-link-apple {
  --link-color: 128, 128, 128;
  --icon-gradient: linear-gradient(135deg, #555 0%, #777 100%);
}

.resource-link-desktop,
.resource-link-github {
  --link-color: 108, 117, 125;
}

.resource-link-desktop {
  --icon-gradient: linear-gradient(135deg, #6c757d 0%, #495057 100%);
}

.resource-link-harmony {
  --link-color: 206, 48, 48;
  --icon-gradient: linear-gradient(135deg, #ce3030 0%, #e74c3c 100%);
}

.resource-link-github {
  --icon-gradient: linear-gradient(135deg, #6c757d 0%, #868e96 100%);
}

.resource-link-danger {
  --link-color: var(--ui-danger-rgb);
  --icon-gradient: linear-gradient(135deg, var(--ui-danger) 0%, var(--ui-danger-text) 100%);
}

/* Apply variant styles */
.resource-link-primary,
.resource-link-info,
.resource-link-android,
.resource-link-apple,
.resource-link-desktop,
.resource-link-github,
.resource-link-harmony,
.resource-link-danger {
  background: linear-gradient(135deg, rgba(var(--link-color), 0.15) 0%, rgba(var(--link-color), 0.08) 100%);
}

.resource-link-primary .resource-icon,
.resource-link-info .resource-icon,
.resource-link-android .resource-icon,
.resource-link-apple .resource-icon,
.resource-link-desktop .resource-icon,
.resource-link-github .resource-icon,
.resource-link-harmony .resource-icon,
.resource-link-danger .resource-icon {
  background: var(--icon-gradient);
}

.resource-link-primary:hover,
.resource-link-info:hover,
.resource-link-android:hover,
.resource-link-apple:hover,
.resource-link-desktop:hover,
.resource-link-github:hover,
.resource-link-harmony:hover,
.resource-link-danger:hover {
  border-color: rgba(var(--link-color), 0.4);
}

/* GPL Badge */
.gpl-badge {
  min-width: 16rem;
  padding: 0;
}

.badge-gpl {
  display: inline-flex;
  align-items: center;
  border: 1px solid var(--ui-border-strong);
  background: var(--ui-accent-soft);
  color: var(--ui-accent);
  font-size: 0.72rem;
  font-weight: 700;
  padding: 0.4rem 0.7rem;
  border-radius: 50px;
  box-shadow: none;
  letter-spacing: 0.5px;
}

.gpl-badge .text-muted {
  color: var(--ui-text-secondary) !important;
}

/* GPL Violation Warning */
.violation-warning {
  background: var(--ui-danger-soft);
  border: 1px solid var(--ui-danger-border);
  border-radius: 12px;
  padding: 1.25rem;
}

.violation-header {
  display: flex;
  align-items: center;
  margin-bottom: 0.75rem;
}

.violation-title {
  font-size: 1rem;
  font-weight: 700;
  color: var(--ui-danger-text);
}

.violation-desc {
  font-size: 0.875rem;
  color: var(--ui-text-secondary);
  margin-bottom: 1rem;
}

.violation-list {
  display: flex;
  flex-direction: column;
  gap: 0.75rem;
  margin-bottom: 1rem;
}

.violation-item {
  display: flex;
  align-items: flex-start;
  padding: 0.875rem;
  background: color-mix(in srgb, var(--ui-danger) 8%, var(--ui-surface));
  border-radius: 8px;
  border-left: 3px solid var(--ui-danger);
}

.violation-icon {
  width: 32px;
  height: 32px;
  border-radius: 50%;
  background: var(--ui-danger);
  display: flex;
  align-items: center;
  justify-content: center;
  color: var(--ui-danger-contrast);
  font-size: 0.875rem;
  flex-shrink: 0;
  margin-right: 0.875rem;
}

.violation-content {
  flex: 1;
  min-width: 0;
}

.violation-name {
  display: block;
  font-weight: 600;
  font-size: 0.9rem;
  color: var(--ui-danger-text);
  margin-bottom: 2px;
}

.violation-reason {
  display: block;
  font-size: 0.8rem;
  color: var(--ui-text-secondary);
}

.violation-notice {
  font-size: 0.8rem;
  color: var(--ui-text-secondary);
  margin-bottom: 0;
  padding: 0.75rem;
  background: var(--ui-surface);
  border-radius: 8px;
}

/* Dark Mode */
[data-bs-theme='dark'] .resource-link-primary,
[data-bs-theme='dark'] .resource-link-info,
[data-bs-theme='dark'] .resource-link-android,
[data-bs-theme='dark'] .resource-link-harmony,
[data-bs-theme='dark'] .resource-link-danger {
  background: linear-gradient(135deg, rgba(var(--link-color), 0.25) 0%, rgba(var(--link-color), 0.12) 100%);
}

[data-bs-theme='dark'] .resource-link-apple {
  --link-color: 170, 170, 170;
  background: linear-gradient(135deg, rgba(var(--link-color), 0.2) 0%, rgba(var(--link-color), 0.1) 100%);
}

[data-bs-theme='dark'] .resource-link-apple .resource-icon {
  background: linear-gradient(135deg, #aaa 0%, #ccc 100%);
  color: #222;
}

[data-bs-theme='dark'] .resource-link-desktop,
[data-bs-theme='dark'] .resource-link-github {
  --link-color: 150, 160, 170;
  background: linear-gradient(135deg, rgba(var(--link-color), 0.2) 0%, rgba(var(--link-color), 0.1) 100%);
}

[data-bs-theme='dark'] .resource-link-desktop .resource-icon {
  background: linear-gradient(135deg, #8c959d 0%, #6c757d 100%);
}

[data-bs-theme='dark'] .resource-link-github .resource-icon {
  background: linear-gradient(135deg, #8c959d 0%, #adb5bd 100%);
}

[data-bs-theme='dark'] .resource-link:hover {
  box-shadow: 0 4px 12px rgba(0, 0, 0, 0.3);
}

.resource-card-header .card-title i,
.resource-group-title i {
  color: var(--ui-accent) !important;
}

.gpl-badge p {
  color: var(--ui-text-muted);
  font-size: 0.65rem;
  line-height: 1.25;
}

.resource-legal-body {
  display: grid;
  grid-template-columns: minmax(0, 1.1fr) auto minmax(20rem, 1.15fr);
  align-items: center;
  gap: 0.65rem;
  padding: 0.5rem 0.65rem !important;
}

.legal-title {
  margin: 0 0 0.25rem;
  color: var(--ui-text-primary);
  font-size: 0.9rem;
  font-weight: 600;
}

.legal-title i {
  color: var(--ui-accent);
}

.legal-copy {
  margin: 0;
  color: var(--ui-text-secondary);
  font-size: 0.78rem;
  line-height: 1.35;
}

.legal-links {
  margin: 0;
}

@media (max-width: 991.98px) {
  .resource-group--official,
  .resource-group--quick-start,
  .resource-group--clients,
  .resource-group--community {
    grid-column: span 12;
  }

  .resource-legal-body {
    grid-template-columns: 1fr;
  }

  .gpl-badge {
    min-width: 0;
  }
}

@media (max-width: 575.98px) {
  .resource-groups {
    display: block;
  }

  .resource-group + .resource-group {
    margin-top: 0.65rem !important;
  }

  .resource-desc {
    white-space: normal;
  }
}

/* Harmony Modal */
.harmony-modal-overlay {
  position: fixed;
  top: 0;
  left: 0;
  right: 0;
  bottom: 0;
  width: 100vw;
  height: 100vh;
  margin: 0;
  background: rgba(45, 38, 40, 0.72);
  backdrop-filter: blur(8px);
  z-index: 9999;
  display: flex;
  align-items: center;
  justify-content: center;
  padding: 20px;
  overflow: hidden;
}

.harmony-modal {
  background: var(--ui-surface-strong);
  border: 1px solid var(--ui-border);
  border-radius: var(--ui-radius-lg);
  width: 100%;
  max-width: 500px;
  display: flex;
  flex-direction: column;
  backdrop-filter: blur(20px);
  box-shadow: var(--ui-shadow-md);
  animation: harmonyModalSlideUp 0.3s ease;
}

@keyframes harmonyModalSlideUp {
  from { transform: translateY(20px); opacity: 0; }
  to { transform: translateY(0); opacity: 1; }
}

.harmony-modal-header {
  display: flex;
  justify-content: space-between;
  align-items: center;
  padding: 20px 24px;
  border-bottom: 1px solid var(--ui-border);
}

.harmony-modal-header h5 {
  margin: 0;
  color: var(--ui-text-primary);
  font-size: 1.1rem;
  font-weight: 600;
}

.harmony-modal-body {
  padding: 24px;
  font-size: 0.95rem;
  line-height: 1.5;
  color: var(--ui-text-primary);
}

.harmony-modal-footer {
  display: flex;
  justify-content: flex-end;
  gap: 10px;
  padding: 16px 24px;
  border-top: 1px solid var(--ui-border);
  background: color-mix(in srgb, var(--ui-surface) 70%, transparent);
}

.fade-enter-active,
.fade-leave-active {
  transition: opacity 0.3s ease;
}

.fade-enter-from,
.fade-leave-to {
  opacity: 0;
}
</style>

<template>
  <div class="resource-section">
    <!-- Resources Card -->
    <div class="card shadow-sm">
      <div class="card-header bg-primary bg-opacity-10 border-bottom-0">
        <h5 class="card-title mb-0">
          <i class="fas fa-book-open text-primary me-2"></i>
          {{ $t('resource_card.resources') }}
        </h5>
      </div>
      <div class="card-body">
        <p class="text-muted mb-4">{{ $t('resource_card.resources_desc') }}</p>

        <!-- 快速入门 -->
        <div class="resource-group mb-4">
          <h6 class="resource-group-title">
            <i class="fas fa-rocket text-success me-2"></i>
            快速入门
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
                  <span class="resource-title">使用教程</span>
                  <span class="resource-desc">详细的配置与使用指南</span>
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
                  <span class="resource-title">加入串流裙</span>
                  <span class="resource-desc">获取帮助与交流经验</span>
                </div>
                <i class="fas fa-external-link-alt resource-arrow"></i>
              </a>
            </div>
          </div>
        </div>

        <!-- 客户端下载 -->
        <div class="resource-group mb-4">
          <h6 class="resource-group-title">
            <i class="fas fa-download text-primary me-2"></i>
            客户端下载
          </h6>
          <div class="row g-3">
            <div class="col-md-6 col-lg-4">
              <a
                class="resource-link resource-link-android"
                href="https://github.com/qiin2333/moonlight-android/releases/tag/shortcut"
                target="_blank"
              >
                <div class="resource-icon">
                  <i class="fab fa-android"></i>
                </div>
                <div class="resource-content">
                  <span class="resource-title">威力加强版</span>
                  <span class="resource-desc">Android 推荐</span>
                </div>
                <i class="fas fa-external-link-alt resource-arrow"></i>
              </a>
            </div>
            <div class="col-md-6 col-lg-4">
              <a
                class="resource-link resource-link-android"
                href="https://github.com/WACrown/moonlight-android"
                target="_blank"
              >
                <div class="resource-icon">
                  <i class="fas fa-crown"></i>
                </div>
                <div class="resource-content">
                  <span class="resource-title">王冠版</span>
                  <span class="resource-desc">Android</span>
                </div>
                <i class="fas fa-external-link-alt resource-arrow"></i>
              </a>
            </div>
            <div class="col-md-6 col-lg-4">
              <a
                class="resource-link resource-link-apple"
                href="https://apps.apple.com/app/voidlink/id6747717070"
                target="_blank"
              >
                <div class="resource-icon">
                  <i class="fab fa-apple"></i>
                </div>
                <div class="resource-content">
                  <span class="resource-title">虚空终端</span>
                  <span class="resource-desc">iOS / iPadOS</span>
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
                  <span class="resource-title">Moonlight-PC</span>
                  <span class="resource-desc">Windows / macOS / Linux</span>
                </div>
                <i class="fas fa-external-link-alt resource-arrow"></i>
              </a>
            </div>
          </div>
        </div>

        <!-- 开源项目 -->
        <div class="resource-group">
          <h6 class="resource-group-title">
            <i class="fab fa-github text-dark me-2"></i>
            开源项目
          </h6>
          <div class="row g-3">
            <div class="col-12">
              <a
                class="resource-link resource-link-github"
                href="https://github.com/qiin2333/Sunshine-Foundation"
                target="_blank"
              >
                <div class="resource-icon">
                  <i class="fab fa-github"></i>
                </div>
                <div class="resource-content">
                  <span class="resource-title">Sunshine Foundation</span>
                  <span class="resource-desc">Star & Fork 支持项目发展</span>
                </div>
                <i class="fas fa-star resource-arrow text-warning"></i>
              </a>
            </div>
          </div>
        </div>
      </div>
    </div>

    <!-- Legal Card -->
    <div class="card shadow-sm mt-4">
      <div class="card-header bg-danger bg-opacity-10 border-bottom-0">
        <h5 class="card-title mb-0">
          <i class="fas fa-gavel text-danger me-2"></i>
          {{ $t('resource_card.legal') }}
        </h5>
      </div>
      <div class="card-body">
        <p class="text-muted mb-4">{{ $t('resource_card.legal_desc') }}</p>

        <!-- GPL v3.0 Badge -->
        <div class="gpl-badge mb-4">
          <div class="d-flex align-items-center justify-content-center">
            <span class="badge-gpl">
              <i class="fas fa-balance-scale me-2"></i>
              GNU General Public License v3.0
            </span>
          </div>
          <p class="text-center text-muted small mt-2 mb-0">
            {{ $t('resource_card.gpl_license_text_1') }}
            <br />
            {{ $t('resource_card.gpl_license_text_2') }}
          </p>
        </div>

        <div class="row g-3">
          <div class="col-md-6">
            <a
              class="resource-link resource-link-danger"
              href="https://github.com/LizardByte/Sunshine/blob/master/LICENSE"
              target="_blank"
            >
              <div class="resource-icon">
                <i class="fas fa-file-alt"></i>
              </div>
              <div class="resource-content">
                <span class="resource-title">{{ $t('resource_card.license') }}</span>
                <span class="resource-desc">查看完整许可证</span>
              </div>
              <i class="fas fa-external-link-alt resource-arrow"></i>
            </a>
          </div>
          <div class="col-md-6">
            <a
              class="resource-link resource-link-danger"
              href="https://github.com/LizardByte/Sunshine/blob/master/NOTICE"
              target="_blank"
            >
              <div class="resource-icon">
                <i class="fas fa-exclamation-triangle"></i>
              </div>
              <div class="resource-content">
                <span class="resource-title">{{ $t('resource_card.third_party_notice') }}</span>
                <span class="resource-desc">第三方组件声明</span>
              </div>
              <i class="fas fa-external-link-alt resource-arrow"></i>
            </a>
          </div>
        </div>
      </div>
    </div>
  </div>
</template>

<style scoped>
.resource-group {
  padding-bottom: 1rem;
  border-bottom: 1px solid rgba(128, 128, 128, 0.15);
}

.resource-group:last-child {
  padding-bottom: 0;
  border-bottom: none;
}

.resource-group-title {
  font-size: 0.875rem;
  font-weight: 600;
  color: var(--bs-secondary-color, #6c757d);
  text-transform: uppercase;
  letter-spacing: 0.5px;
  margin-bottom: 1rem;
}

/* Resource Link Base */
.resource-link {
  display: flex;
  align-items: center;
  padding: 1rem;
  border-radius: 10px;
  text-decoration: none;
  background: var(--bs-tertiary-bg, #f8f9fa);
  border: 1px solid transparent;
  transition: transform 0.2s ease, box-shadow 0.2s ease, border-color 0.2s ease;
}

.resource-link:hover {
  transform: translateY(-2px);
  box-shadow: 0 4px 12px rgba(0, 0, 0, 0.15);
  text-decoration: none;
}

.resource-icon {
  width: 44px;
  height: 44px;
  border-radius: 10px;
  display: flex;
  align-items: center;
  justify-content: center;
  font-size: 1.25rem;
  flex-shrink: 0;
  margin-right: 1rem;
  color: white;
}

.resource-content {
  flex: 1;
  min-width: 0;
}

.resource-title {
  display: block;
  font-weight: 600;
  font-size: 0.95rem;
  color: var(--bs-body-color, #2c3e50);
  margin-bottom: 2px;
}

.resource-desc {
  display: block;
  font-size: 0.8rem;
  color: var(--bs-secondary-color, #6c757d);
}

.resource-arrow {
  font-size: 0.875rem;
  color: var(--bs-secondary-color, #adb5bd);
  margin-left: 0.5rem;
  transition: transform 0.2s ease;
}

.resource-link:hover .resource-arrow {
  transform: translateX(3px);
}

/* Color Variants - Using CSS Custom Properties */
.resource-link-primary {
  --link-color: 40, 167, 69;
  --icon-gradient: linear-gradient(135deg, #28a745 0%, #20c997 100%);
}

.resource-link-info {
  --link-color: 0, 123, 255;
  --icon-gradient: linear-gradient(135deg, #007bff 0%, #17a2b8 100%);
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

.resource-link-github {
  --icon-gradient: linear-gradient(135deg, #6c757d 0%, #868e96 100%);
}

.resource-link-danger {
  --link-color: 220, 53, 69;
  --icon-gradient: linear-gradient(135deg, #dc3545 0%, #e94560 100%);
}

/* Apply variant styles */
.resource-link-primary,
.resource-link-info,
.resource-link-android,
.resource-link-apple,
.resource-link-desktop,
.resource-link-github,
.resource-link-danger {
  background: linear-gradient(135deg, rgba(var(--link-color), 0.15) 0%, rgba(var(--link-color), 0.08) 100%);
}

.resource-link-primary .resource-icon,
.resource-link-info .resource-icon,
.resource-link-android .resource-icon,
.resource-link-apple .resource-icon,
.resource-link-desktop .resource-icon,
.resource-link-github .resource-icon,
.resource-link-danger .resource-icon {
  background: var(--icon-gradient);
}

.resource-link-primary:hover,
.resource-link-info:hover,
.resource-link-android:hover,
.resource-link-apple:hover,
.resource-link-desktop:hover,
.resource-link-github:hover,
.resource-link-danger:hover {
  border-color: rgba(var(--link-color), 0.4);
}

/* GPL Badge */
.gpl-badge {
  border-radius: 12px;
  padding: 1.25rem;
}

.badge-gpl {
  display: inline-flex;
  align-items: center;
  background: linear-gradient(135deg, #e94560 0%, #ff6b6b 100%);
  color: white;
  font-size: 1.1rem;
  font-weight: 700;
  padding: 0.75rem 1.5rem;
  border-radius: 50px;
  box-shadow: 0 4px 15px rgba(233, 69, 96, 0.4);
  letter-spacing: 0.5px;
}

.gpl-badge .text-muted {
  color: var(--bs-secondary-color, rgba(255, 255, 255, 0.7)) !important;
}

/* GPL Violation Warning */
.violation-warning {
  background: linear-gradient(135deg, rgba(220, 53, 69, 0.1) 0%, rgba(220, 53, 69, 0.05) 100%);
  border: 1px solid rgba(220, 53, 69, 0.3);
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
  color: #dc3545;
}

.violation-desc {
  font-size: 0.875rem;
  color: var(--bs-secondary-color, #6c757d);
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
  background: rgba(220, 53, 69, 0.08);
  border-radius: 8px;
  border-left: 3px solid #dc3545;
}

.violation-icon {
  width: 32px;
  height: 32px;
  border-radius: 50%;
  background: linear-gradient(135deg, #dc3545 0%, #e94560 100%);
  display: flex;
  align-items: center;
  justify-content: center;
  color: white;
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
  color: #dc3545;
  margin-bottom: 2px;
}

.violation-reason {
  display: block;
  font-size: 0.8rem;
  color: var(--bs-secondary-color, #6c757d);
}

.violation-notice {
  font-size: 0.8rem;
  color: var(--bs-secondary-color, #6c757d);
  margin-bottom: 0;
  padding: 0.75rem;
  background: rgba(0, 0, 0, 0.03);
  border-radius: 8px;
}

/* Dark Mode */
[data-bs-theme='dark'] .resource-link-primary,
[data-bs-theme='dark'] .resource-link-info,
[data-bs-theme='dark'] .resource-link-android,
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

[data-bs-theme='dark'] .resource-group {
  border-bottom-color: rgba(255, 255, 255, 0.1);
}
</style>

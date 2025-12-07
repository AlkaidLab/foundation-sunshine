/**
 * 表单验证工具模块
 * 提供应用表单的各种验证规则和方法
 */

/**
 * 验证规则对象
 */
export const validationRules = {
  appName: {
    required: true,
    minLength: 1,
    maxLength: 100,
    pattern: /^[^<>:"\\|?*\x00-\x1F]+$/,
    message: '应用名称不能为空，且不能包含特殊字符',
  },
  command: {
    required: false,
    minLength: 0,
    maxLength: 1000,
    message: '命令不规范，请输入正确的命令',
  },
  workingDir: {
    required: false,
    maxLength: 500,
    message: '工作目录路径过长',
  },
  outputName: {
    required: false,
    maxLength: 100,
    pattern: /^[a-zA-Z0-9_\-\.]*$/,
    message: '输出名称只能包含字母、数字、下划线、连字符和点',
  },
  timeout: {
    required: false,
    min: 0,
    max: 3600,
    message: '超时时间必须在0-3600秒之间',
  },
  imagePath: {
    required: false,
    maxLength: 500,
    allowedTypes: ['png', 'jpg', 'jpeg', 'gif', 'bmp', 'webp'],
    message: '图片路径无效或格式不支持',
  },
}

/**
 * 验证单个字段
 * @param {string} fieldName 字段名称
 * @param {any} value 字段值
 * @param {Object} customRules 自定义验证规则
 * @returns {Object} 验证结果 {isValid: boolean, message: string}
 */
export function validateField(fieldName, value, customRules = {}) {
  const rules = { ...validationRules[fieldName], ...customRules }

  if (!rules) {
    return { isValid: true, message: '' }
  }

  const strValue = value?.toString().trim() ?? ''
  const isEmpty = strValue === ''

  // 必填验证
  if (rules.required && isEmpty) {
    return { isValid: false, message: rules.message || '此字段为必填项' }
  }

  // 如果字段为空且不是必填，则跳过其他验证
  if (isEmpty) {
    return { isValid: true, message: '' }
  }

  // 长度验证
  if (rules.minLength && strValue.length < rules.minLength) {
    return { isValid: false, message: `最少需要${rules.minLength}个字符` }
  }

  if (rules.maxLength && strValue.length > rules.maxLength) {
    return { isValid: false, message: `最多允许${rules.maxLength}个字符` }
  }

  // 数值验证
  if (rules.min !== undefined || rules.max !== undefined) {
    const numValue = Number(value)
    if (isNaN(numValue)) {
      return { isValid: false, message: '请输入有效的数字' }
    }
    if (rules.min !== undefined && numValue < rules.min) {
      return { isValid: false, message: `最小值为${rules.min}` }
    }
    if (rules.max !== undefined && numValue > rules.max) {
      return { isValid: false, message: `最大值为${rules.max}` }
    }
  }

  // 正则表达式验证
  if (rules.pattern && !rules.pattern.test(strValue)) {
    return { isValid: false, message: rules.message || '格式不正确' }
  }

  // 文件类型验证
  if (rules.allowedTypes && fieldName === 'imagePath' && strValue !== 'desktop') {
    const lastDotIndex = strValue.lastIndexOf('.')
    if (lastDotIndex > 0) {
      const extension = strValue
        .slice(lastDotIndex + 1)
        .split(/[?#]/)[0]
        .toLowerCase()
      if (extension && !rules.allowedTypes.includes(extension)) {
        return {
          isValid: false,
          message: `只支持以下格式：${rules.allowedTypes.join(', ')}`,
        }
      }
    }
  }

  return { isValid: true, message: '' }
}

// 字段映射配置
const FIELD_MAPPINGS = [
  { key: 'name', rule: 'appName', label: '应用名称' },
  { key: 'cmd', rule: 'command', label: '命令' },
  { key: 'working-dir', rule: 'workingDir', label: '工作目录' },
  { key: 'output', rule: 'outputName', label: '输出名称' },
  { key: 'exit-timeout', rule: 'timeout', label: '超时时间' },
  { key: 'image-path', rule: 'imagePath', label: '图片路径' },
]

/**
 * 验证应用表单
 * @param {Object} formData 表单数据
 * @returns {Object} 验证结果
 */
export function validateAppForm(formData) {
  const results = {}
  const errors = []

  // 验证基础字段
  for (const { key, rule, label } of FIELD_MAPPINGS) {
    const result = validateField(rule, formData[key])
    results[key] = result
    if (!result.isValid) {
      errors.push(`${label}: ${result.message}`)
    }
  }

  // 验证准备命令
  formData['prep-cmd']?.forEach((cmd, index) => {
    if (!cmd.do?.trim() && !cmd.undo?.trim()) {
      errors.push(`准备命令 ${index + 1}: 打开时执行命令或退出应用时要执行的命令至少需要填写一个`)
    }
  })

  // 验证菜单命令
  formData['menu-cmd']?.forEach((cmd, index) => {
    if (!cmd.name?.trim()) {
      errors.push(`菜单命令 ${index + 1}: 显示名称不能为空`)
    }
    if (!cmd.cmd?.trim()) {
      errors.push(`菜单命令 ${index + 1}: 命令不能为空`)
    }
  })

  // 验证独立命令
  formData.detached?.forEach((cmd, index) => {
    if (cmd && !cmd.trim()) {
      errors.push(`独立命令 ${index + 1}: 命令不能为空`)
    }
  })

  return {
    isValid: errors.length === 0,
    errors,
    fieldResults: results,
  }
}

/**
 * 验证文件
 * @param {File} file 文件对象
 * @param {Object} options 验证选项
 * @returns {Object} 验证结果
 */
export function validateFile(file, options = {}) {
  const {
    allowedTypes = ['image/png', 'image/jpg', 'image/jpeg', 'image/gif', 'image/bmp', 'image/webp'],
    maxSize = 10 * 1024 * 1024,
    minSize = 0,
  } = options

  if (!file) {
    return { isValid: false, message: '请选择文件' }
  }

  if (!allowedTypes.includes(file.type)) {
    return {
      isValid: false,
      message: `不支持的文件类型。支持的格式：${allowedTypes.join(', ')}`,
    }
  }

  if (file.size > maxSize) {
    return {
      isValid: false,
      message: `文件大小不能超过 ${(maxSize / (1024 * 1024)).toFixed(1)}MB`,
    }
  }

  if (file.size < minSize) {
    return {
      isValid: false,
      message: `文件大小不能小于 ${(minSize / 1024).toFixed(1)}KB`,
    }
  }

  return { isValid: true, message: '' }
}

/**
 * 实时验证混合器
 * @param {Object} formData 表单数据
 * @param {Array} watchFields 需要监听的字段
 * @returns {Object} 验证状态
 */
export function createFormValidator(formData, watchFields = []) {
  const validationStates = Object.fromEntries(watchFields.map((field) => [field, { isValid: true, message: '' }]))

  return {
    validateField(fieldName, value) {
      const result = validateField(fieldName, value)
      validationStates[fieldName] = result
      return result
    },

    validateForm() {
      return validateAppForm(formData)
    },

    getFieldState(fieldName) {
      return validationStates[fieldName] ?? { isValid: true, message: '' }
    },

    getAllStates() {
      return { ...validationStates }
    },

    resetValidation() {
      for (const key of Object.keys(validationStates)) {
        validationStates[key] = { isValid: true, message: '' }
      }
    },
  }
}

/**
 * 创建防抖验证器
 * @param {Function} validationFn 验证函数
 * @param {number} delay 防抖延迟时间
 * @returns {Function} 防抖后的验证函数
 */
export function createDebouncedValidator(validationFn, delay = 300) {
  let timeoutId

  return function (...args) {
    clearTimeout(timeoutId)
    return new Promise((resolve) => {
      timeoutId = setTimeout(() => resolve(validationFn(...args)), delay)
    })
  }
}

export default {
  validationRules,
  validateField,
  validateAppForm,
  validateFile,
  createFormValidator,
  createDebouncedValidator,
}

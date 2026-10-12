/**
 * 表单验证工具模块
 * 提供应用表单的各种验证规则和方法
 */

import { localizedMessage } from './localizedMessage.js'

/**
 * 验证规则对象
 */
export const validationRules = {
  appName: {
    required: true,
    minLength: 1,
    maxLength: 100,
    pattern: /^[^<>:"\\|?*\x00-\x1F]+$/,
    messageKey: 'apps.validation.app_name',
  },
  command: {
    required: false,
    minLength: 0,
    maxLength: 1000,
  },
  workingDir: {
    required: false,
    maxLength: 500,
  },
  outputName: {
    required: false,
    maxLength: 100,
    pattern: /^[a-zA-Z0-9_\-\.]*$/,
    messageKey: 'apps.validation.output_name',
  },
  timeout: {
    required: false,
    min: 0,
    max: 3600,
  },
  imagePath: {
    required: false,
    maxLength: 500,
    allowedTypes: ['png', 'jpg', 'jpeg', 'gif', 'bmp', 'webp'],
  },
}

/**
 * 验证单个字段
 * @param {string} fieldName 字段名称
 * @param {any} value 字段值
 * @param {Object} customRules 自定义验证规则
 * @param {Function} [translate] 当前界面的翻译函数；省略时使用英文
 * @returns {Object} 验证结果 {isValid: boolean, message: string}
 */
export function validateField(fieldName, value, customRules = {}, translate) {
  const rules = { ...validationRules[fieldName], ...customRules }
  const message = (key, params) => localizedMessage(translate, key, params)
  const ruleMessage = (fallback) => rules.message || message(rules.messageKey || fallback)

  const strValue = value?.toString().trim() ?? ''
  const isEmpty = strValue === ''

  // 必填验证
  if (rules.required && isEmpty) {
    return { isValid: false, message: rules.message || message('apps.validation.required') }
  }

  // 如果字段为空且不是必填，则跳过其他验证
  if (isEmpty) {
    return { isValid: true, message: '' }
  }

  // 长度验证
  if (rules.minLength && strValue.length < rules.minLength) {
    return { isValid: false, message: message('apps.validation.min_length', { count: rules.minLength }) }
  }

  if (rules.maxLength && strValue.length > rules.maxLength) {
    return { isValid: false, message: message('apps.validation.max_length', { count: rules.maxLength }) }
  }

  // 数值验证
  if (rules.min !== undefined || rules.max !== undefined) {
    const numValue = Number(value)
    if (isNaN(numValue)) {
      return { isValid: false, message: message('apps.validation.number') }
    }
    if (rules.min !== undefined && numValue < rules.min) {
      return { isValid: false, message: message('apps.validation.min_value', { value: rules.min }) }
    }
    if (rules.max !== undefined && numValue > rules.max) {
      return { isValid: false, message: message('apps.validation.max_value', { value: rules.max }) }
    }
  }

  // 正则表达式验证
  if (rules.pattern && !rules.pattern.test(strValue)) {
    return { isValid: false, message: ruleMessage('apps.validation.format') }
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
          message: message('apps.validation.image_types', { types: rules.allowedTypes.join(', ') }),
        }
      }
    }
  }

  return { isValid: true, message: '' }
}

// 字段映射配置
const FIELD_MAPPINGS = [
  { key: 'name', rule: 'appName', label: 'apps.app_name' },
  { key: 'cmd', rule: 'command', label: 'apps.cmd' },
  { key: 'working-dir', rule: 'workingDir', label: 'apps.working_dir' },
  { key: 'output', rule: 'outputName', label: 'apps.output_name' },
  { key: 'exit-timeout', rule: 'timeout', label: 'apps.exit_timeout' },
  { key: 'image-path', rule: 'imagePath', label: 'apps.image' },
]

/**
 * 验证应用表单
 * @param {Object} formData 表单数据
 * @param {Function} [translate] 当前界面的翻译函数；省略时使用英文
 * @returns {Object} 验证结果
 */
export function validateAppForm(formData, translate) {
  const results = {}
  const errors = []
  const message = (key, params) => localizedMessage(translate, key, params)

  // 验证基础字段
  for (const { key, rule, label } of FIELD_MAPPINGS) {
    const result = validateField(rule, formData[key], {}, translate)
    results[key] = result
    if (!result.isValid) {
      errors.push(`${message(label)}: ${result.message}`)
    }
  }

  // 验证准备命令
  formData['prep-cmd']?.forEach((cmd, index) => {
    if (!cmd.do?.trim() && !cmd.undo?.trim()) {
      errors.push(message('apps.validation.prep_command', { index: index + 1 }))
    }
  })

  // 验证菜单命令
  formData['menu-cmd']?.forEach((cmd, index) => {
    if (!cmd.name?.trim()) {
      errors.push(message('apps.validation.menu_name', { index: index + 1 }))
    }
    if (!cmd.cmd?.trim()) {
      errors.push(message('apps.validation.menu_command', { index: index + 1 }))
    }
  })

  // 验证独立命令
  formData.detached?.forEach((cmd, index) => {
    if (cmd && !cmd.trim()) {
      errors.push(message('apps.validation.detached_command', { index: index + 1 }))
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
    translate,
  } = options
  const message = (key, params) => localizedMessage(translate, key, params)

  if (!file) {
    return { isValid: false, message: message('apps.validation.select_file') }
  }

  if (!allowedTypes.includes(file.type)) {
    return {
      isValid: false,
      message: message('apps.validation.file_types', { types: allowedTypes.join(', ') }),
    }
  }

  if (file.size > maxSize) {
    return {
      isValid: false,
      message: message('apps.validation.file_max_size', { size: (maxSize / (1024 * 1024)).toFixed(1) }),
    }
  }

  if (file.size < minSize) {
    return {
      isValid: false,
      message: message('apps.validation.file_min_size', { size: (minSize / 1024).toFixed(1) }),
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
export function createFormValidator(formData, watchFields = [], translate) {
  const validationStates = Object.fromEntries(watchFields.map((field) => [field, { isValid: true, message: '' }]))

  return {
    validateField(fieldName, value) {
      const result = validateField(fieldName, value, {}, translate)
      validationStates[fieldName] = result
      return result
    },

    validateForm() {
      return validateAppForm(formData, translate)
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

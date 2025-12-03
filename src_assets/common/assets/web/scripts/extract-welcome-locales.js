#!/usr/bin/env node
/**
 * 从模板文件生成完整的 welcome.html
 * 提取所有语言的 welcome 翻译并静态嵌入到 HTML 中
 * 注意：只提取 welcome 部分，因为需要的 _common 键已经添加到 welcome 中了
 */

import fs from 'fs'
import path from 'path'
import { fileURLToPath } from 'url'

const __filename = fileURLToPath(import.meta.url)
const __dirname = path.dirname(__filename)

const localeDir = path.join(__dirname, '../public/assets/locale')
const templatePath = path.join(__dirname, '../welcome.html.template')
const outputPath = path.join(__dirname, '../welcome.html')
const output = {}

// 获取所有语言文件
const localeFiles = fs.readdirSync(localeDir).filter(file => file.endsWith('.json'))

localeFiles.forEach(file => {
  const locale = file.replace('.json', '')
  const filePath = path.join(localeDir, file)
  
  try {
    const content = JSON.parse(fs.readFileSync(filePath, 'utf8'))
    // 只提取 welcome 部分（username, password, error, success 已经在 welcome 中了）
    if (content.welcome) {
      output[locale] = {
        welcome: content.welcome
      }
    }
  } catch (e) {
    console.error(`Failed to parse ${file}:`, e.message)
  }
})

// 生成内联的 script 标签内容
const inlineScript = `<script>
// 自动生成的 welcome 页面翻译数据（构建时生成）
window.__WELCOME_LOCALES__ = ${JSON.stringify(output, null, 2)};
</script>`

// 读取模板文件并生成完整的 welcome.html
if (!fs.existsSync(templatePath)) {
  console.error(`Error: Template file not found: ${templatePath}`)
  process.exit(1)
}

let template = fs.readFileSync(templatePath, 'utf8')

// 替换占位符
if (template.includes('WELCOME_LOCALES_INLINE_PLACEHOLDER')) {
  template = template.replace('<!-- WELCOME_LOCALES_INLINE_PLACEHOLDER -->', inlineScript)
  fs.writeFileSync(outputPath, template, 'utf8')
  console.log(`Generated welcome.html from template`)
  console.log(`  Languages: ${Object.keys(output).join(', ')}`)
} else {
  console.error(`Error: Placeholder not found in template file`)
  process.exit(1)
}


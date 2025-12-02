#!/usr/bin/env node
/**
 * 提取所有语言的 welcome 翻译并生成 JavaScript 对象
 * 用于在构建时静态嵌入到 welcome.html 中
 * 注意：只提取 welcome 部分，因为需要的 _common 键已经添加到 welcome 中了
 */

import fs from 'fs'
import path from 'path'
import { fileURLToPath } from 'url'

const __filename = fileURLToPath(import.meta.url)
const __dirname = path.dirname(__filename)

const localeDir = path.join(__dirname, '../public/assets/locale')
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

// 生成内联的 script 标签内容（用于替换 welcome.html 中的占位符）
const inlineScript = `<script>
// 自动生成的 welcome 页面翻译数据（构建时生成）
window.__WELCOME_LOCALES__ = ${JSON.stringify(output, null, 2)};
</script>`

// 更新 welcome.html，替换现有的翻译数据
const welcomeHtmlPath = path.join(__dirname, '../welcome.html')
if (fs.existsSync(welcomeHtmlPath)) {
  let welcomeHtml = fs.readFileSync(welcomeHtmlPath, 'utf8')
  
  // 检查是否有占位符（首次运行）
  if (welcomeHtml.includes('WELCOME_LOCALES_INLINE_PLACEHOLDER')) {
    welcomeHtml = welcomeHtml.replace('<!-- WELCOME_LOCALES_INLINE_PLACEHOLDER -->', inlineScript)
    fs.writeFileSync(welcomeHtmlPath, welcomeHtml, 'utf8')
    console.log(`Updated welcome.html with embedded locales (placeholder replaced)`)
  } 
  // 检查是否已有翻译数据（更新现有数据）
  else if (welcomeHtml.includes('window.__WELCOME_LOCALES__')) {
    // 使用正则表达式替换整个 script 标签及其内容
    const scriptRegex = /<script>\s*\/\/\s*自动生成的 welcome 页面翻译数据.*?<\/script>/s
    if (scriptRegex.test(welcomeHtml)) {
      welcomeHtml = welcomeHtml.replace(scriptRegex, inlineScript)
      fs.writeFileSync(welcomeHtmlPath, welcomeHtml, 'utf8')
      console.log(`Updated welcome.html with embedded locales (existing data replaced)`)
    } else {
      console.warn(`Warning: Found __WELCOME_LOCALES__ but couldn't replace (format may have changed)`)
    }
  } else {
    console.warn(`Warning: No placeholder or existing translation data found in welcome.html`)
  }
}

console.log(`  Languages: ${Object.keys(output).join(', ')}`)


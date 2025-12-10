#!/usr/bin/env node
/**
 * i18n JSON Sorting and Formatting Script
 * 
 * This script sorts all locale JSON files alphabetically by key and applies
 * consistent formatting. This helps reduce Git conflicts and makes files easier
 * to review and maintain.
 * 
 * Usage:
 *   node scripts/format-i18n.js                # Format all locale files
 *   node scripts/format-i18n.js --check        # Check if files are properly formatted (for CI)
 */

import fs from 'fs'
import path from 'path'
import { fileURLToPath } from 'url'

const __filename = fileURLToPath(import.meta.url)
const __dirname = path.dirname(__filename)

const localeDir = path.join(__dirname, '../src_assets/common/assets/web/public/assets/locale')

// Parse command line arguments
const args = process.argv.slice(2)
const checkMode = args.includes('--check')

/**
 * Sort object keys recursively
 */
function sortObjectKeys(obj) {
  if (typeof obj !== 'object' || obj === null || Array.isArray(obj)) {
    return obj
  }
  
  const sorted = {}
  const keys = Object.keys(obj).sort()
  
  for (const key of keys) {
    sorted[key] = sortObjectKeys(obj[key])
  }
  
  return sorted
}

/**
 * Format a JSON file with sorted keys
 */
function formatJsonFile(filePath) {
  try {
    const content = fs.readFileSync(filePath, 'utf8')
    const parsed = JSON.parse(content)
    const sorted = sortObjectKeys(parsed)
    const formatted = JSON.stringify(sorted, null, 2) + '\n'
    
    // In check mode, compare formatted content with original
    if (checkMode) {
      return content === formatted
    } else {
      fs.writeFileSync(filePath, formatted, 'utf8')
      return true
    }
  } catch (e) {
    console.error(`❌ Error processing ${path.basename(filePath)}: ${e.message}`)
    return false
  }
}

/**
 * Main formatting function
 */
function formatLocales() {
  if (checkMode) {
    console.log('🔍 Checking i18n file formatting...\n')
  } else {
    console.log('🔄 Formatting i18n files...\n')
  }
  
  if (!fs.existsSync(localeDir)) {
    console.error(`❌ Locale directory not found: ${localeDir}`)
    process.exit(1)
  }
  
  const localeFiles = fs.readdirSync(localeDir)
    .filter(file => file.endsWith('.json'))
    .sort()
  
  if (localeFiles.length === 0) {
    console.error('❌ No locale JSON files found')
    process.exit(1)
  }
  
  let allFormatted = true
  const results = []
  
  for (const localeFile of localeFiles) {
    const filePath = path.join(localeDir, localeFile)
    const isFormatted = formatJsonFile(filePath)
    
    if (checkMode) {
      if (isFormatted) {
        console.log(`✅ ${localeFile}: Properly formatted`)
      } else {
        console.log(`❌ ${localeFile}: Needs formatting`)
        allFormatted = false
      }
    } else {
      if (isFormatted) {
        console.log(`✅ ${localeFile}: Formatted successfully`)
      } else {
        console.log(`❌ ${localeFile}: Failed to format`)
        allFormatted = false
      }
    }
    
    results.push({ file: localeFile, formatted: isFormatted })
  }
  
  // Summary
  console.log('\n' + '━'.repeat(60))
  console.log('📊 Summary:')
  console.log(`   Total files processed: ${localeFiles.length}`)
  
  if (checkMode) {
    console.log(`   Properly formatted: ${results.filter(r => r.formatted).length}`)
    console.log(`   Need formatting: ${results.filter(r => !r.formatted).length}`)
  } else {
    console.log(`   Successfully formatted: ${results.filter(r => r.formatted).length}`)
    console.log(`   Failed to format: ${results.filter(r => !r.formatted).length}`)
  }
  
  console.log('━'.repeat(60))
  
  if (checkMode && !allFormatted) {
    console.log('\n💡 Tip: Run without --check flag to auto-format all files')
    console.error('\n❌ Formatting check failed')
    process.exit(1)
  }
  
  if (!checkMode && allFormatted) {
    console.log('\n✅ All locale files have been formatted and sorted')
  }
  
  if (!allFormatted && !checkMode) {
    process.exit(1)
  }
}

// Run formatting
formatLocales()

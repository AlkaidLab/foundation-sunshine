import vue from 'eslint-plugin-vue'
import globals from 'globals'

export default [
  {
    ignores: [
      'build/**',
      'node_modules/**',
      'src_assets/common/assets/web/dist/**',
      'src_assets/common/assets/web/welcome.html',
    ],
  },
  ...vue.configs['flat/base'],
  {
    files: ['src_assets/common/assets/web/**/*.{js,vue}'],
    languageOptions: {
      ecmaVersion: 'latest',
      sourceType: 'module',
      globals: {
        ...globals.browser,
      },
    },
    rules: {
      'no-undef': 'error',
    },
  },
  {
    files: ['src_assets/common/assets/web/utils/fileSelection.js'],
    languageOptions: {
      globals: {
        process: 'readonly',
      },
    },
  },
  {
    files: [
      'src_assets/common/assets/web/scripts/**/*.js',
      'src_assets/common/assets/web/tests/**/*.js',
    ],
    languageOptions: {
      globals: {
        ...globals.node,
      },
    },
  },
]

import ejs from 'ejs'
import fs from 'fs'

/**
 * Vite EJS Plugin for Vite 7
 * Compatible replacement for vite-plugin-ejs that works with Vite 7's new transformIndexHtml API
 * @param {Record<string, any> | ((config: any) => Record<string, any>)} data - Data to pass to EJS template
 * @param {object} options - Optional EJS options
 * @returns {import('vite').Plugin}
 */
export function ViteEjsPlugin(data = {}, options = {}) {
  let config
  
  return {
    name: 'vite-plugin-ejs-v7',
    // Get Resolved config
    configResolved(resolvedConfig) {
      config = resolvedConfig
    },
    // Use load hook to preprocess HTML files before they're parsed
    load(id) {
      // Only process HTML files
      if (id.endsWith('.html')) {
        try {
          const html = fs.readFileSync(id, 'utf-8')
          
          // Resolve data if it's a function
          const resolvedData = typeof data === 'function' ? data(config) : data
          
          // Resolve EJS options if it's a function
          let ejsOptions = options && options.ejs ? options.ejs : {}
          if (typeof ejsOptions === 'function') {
            ejsOptions = ejsOptions(config)
          }
          
          // Render EJS template with data
          const rendered = ejs.render(
            html,
            Object.assign(
              {
                NODE_ENV: config.mode,
                isDev: config.mode === 'development',
              },
              resolvedData
            ),
            Object.assign(
              {
                // Setting views enables includes support
                views: [config.root],
              },
              ejsOptions,
              {
                async: false, // Force sync
              }
            )
          )
          
          return {
            code: rendered,
            map: null,
          }
        } catch (error) {
          // If there's an error, let Vite handle it normally
          return null
        }
      }
      return null
    },
    transformIndexHtml: {
      // Use 'pre' order to ensure EJS is processed before other HTML transformations
      order: 'pre',
      // Vite 7 uses 'handler' instead of 'transform'
      handler(html) {
        // Resolve data if it's a function
        const resolvedData = typeof data === 'function' ? data(config) : data
        
        // Resolve EJS options if it's a function
        let ejsOptions = options && options.ejs ? options.ejs : {}
        if (typeof ejsOptions === 'function') {
          ejsOptions = ejsOptions(config)
        }
        
        // Render EJS template with data
        const rendered = ejs.render(
          html,
          Object.assign(
            {
              NODE_ENV: config.mode,
              isDev: config.mode === 'development',
            },
            resolvedData
          ),
          Object.assign(
            {
              // Setting views enables includes support
              views: [config.root],
            },
            ejsOptions,
            {
              async: false, // Force sync
            }
          )
        )
        
        return rendered
      },
    },
  }
}

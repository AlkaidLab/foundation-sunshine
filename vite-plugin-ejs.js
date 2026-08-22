import ejs from 'ejs'

/**
 * Local EJS transform for Vite HTML entry points.
 * @param {Record<string, any> | ((config: any) => Record<string, any>)} data - Data to pass to EJS template
 * @param {object} options - Optional EJS options
 * @returns {import('vite').Plugin}
 */
export function ViteEjsPlugin(data = {}, options = {}) {
  let config
  
  return {
    name: 'sunshine-ejs-transform',
    // Get Resolved config
    configResolved(resolvedConfig) {
      config = resolvedConfig
    },
    transformIndexHtml: {
      // Use 'pre' order to ensure EJS is processed before other HTML transformations
      order: 'pre',
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

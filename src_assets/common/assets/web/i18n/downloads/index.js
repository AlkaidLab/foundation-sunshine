const catalogs = import.meta.glob('./*.json', { eager: true, import: 'default' })

// Only the downloads entry imports these page-local messages.
export const downloadMessages = Object.fromEntries(
  Object.entries(catalogs).map(([path, messages]) => [path.slice(2, -5), messages]),
)

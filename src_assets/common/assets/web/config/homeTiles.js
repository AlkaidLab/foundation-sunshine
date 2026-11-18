// Catalogue of the home page tiles. `titleKey`: locale key; `sizes`: allowed sizes, `size`: default size;
// `needs`: what the tile reads, so it can be left out where the host has no such source:
//   'local'  - routes that only answer on the host itself (sessions, performance, HDR)
//   'windows' - routes that exist on Windows hosts only (virtual display)
//   'system' - /api/runtime/system (CPU, GPU, memory); newer hosts only, hidden when the host answers 404 or reports supported:false
// A tile that needs a source the host does not have is neither shown nor offered in the add list.
export const HOME_TILES = [
  { id: 'host', icon: 'dns', tone: '', titleKey: 'home.t.host', sizes: ['2x1', '2x2'], size: '2x1' },
  { id: 'session', icon: 'cast_connected', tone: 'secondary', titleKey: 'home.t.session', sizes: ['2x1', '2x2'], size: '2x1', needs: 'local' },
  { id: 'latency', icon: 'speed', tone: '', titleKey: 'home.t.latency', sizes: ['1x1', '2x1', '2x2'], size: '2x1', needs: 'local' },
  { id: 'memory', icon: 'storage', tone: '', titleKey: 'home.t.memory', sizes: ['1x1', '2x1'], size: '2x1', needs: 'system' },
  { id: 'cpu', icon: 'memory', tone: '', titleKey: 'home.t.cpu', sizes: ['1x1', '2x1'], size: '1x1', needs: 'system' },
  { id: 'gpu', icon: 'developer_board', tone: '', titleKey: 'home.t.gpu', sizes: ['1x1', '2x1'], size: '1x1', needs: 'system' },
  { id: 'clients', icon: 'devices', tone: '', titleKey: 'home.t.clients', sizes: ['2x1', '2x2'], size: '2x1' },
  { id: 'apps', icon: 'apps', tone: '', titleKey: 'home.t.apps', sizes: ['1x1'], size: '1x1' },
  { id: 'encoder', icon: 'memory', tone: '', titleKey: 'home.t.encoder', sizes: ['1x1'], size: '1x1' },
  { id: 'bitrate', icon: 'network_check', tone: '', titleKey: 'home.t.bitrate', sizes: ['1x1', '2x1'], size: '2x1', needs: 'local' },
  { id: 'fps', icon: 'videocam', tone: '', titleKey: 'home.t.fps', sizes: ['1x1', '2x1'], size: '2x1', needs: 'local' },
  { id: 'loss', icon: 'network_check', tone: '', titleKey: 'home.t.loss', sizes: ['1x1'], size: '1x1', needs: 'local' },
  { id: 'hdr', icon: 'hdr_on', tone: '', titleKey: 'home.t.hdr', sizes: ['1x1'], size: '1x1', needs: 'local' },
  { id: 'vdd', icon: 'monitor', tone: '', titleKey: 'home.t.vdd', sizes: ['1x1'], size: '1x1', needs: 'windows' },
  { id: 'mic', icon: 'mic', tone: '', titleKey: 'home.t.mic', sizes: ['1x1'], size: '1x1' },
  { id: 'errors', icon: 'bug_report', tone: '', titleKey: 'home.t.errors', sizes: ['2x1'], size: '2x1' },
  { id: 'pair', icon: 'person_add', tone: 'primary', titleKey: 'home.t.pair', sizes: ['1x1'], size: '1x1' },
  { id: 'restart', icon: 'restart_alt', tone: '', titleKey: 'home.t.restart', sizes: ['1x1'], size: '1x1' },
  { id: 'logs', icon: 'terminal', tone: '', titleKey: 'home.t.logs', sizes: ['1x1'], size: '1x1' },
  { id: 'downloads', icon: 'download', tone: 'success', titleKey: 'home.t.downloads', sizes: ['1x1'], size: '1x1' },
  { id: 'sponsor', icon: 'favorite', tone: 'warn', titleKey: 'home.t.sponsor', sizes: ['1x1'], size: '1x1' },
  { id: 'links', icon: 'groups', tone: 'secondary', titleKey: 'home.t.links', sizes: ['1x1'], size: '1x1' },
]

// Order on first visit. At 6 columns this is four rows of six cells, so it fits one screen of 1440 x 900.
export const DEFAULT_TILES = ['host', 'session', 'latency', 'memory', 'cpu', 'gpu', 'clients', 'apps', 'encoder', 'bitrate', 'fps', 'loss', 'errors', 'pair', 'downloads', 'sponsor', 'links']

export const TILE_STORAGE_KEY = 'web-home'

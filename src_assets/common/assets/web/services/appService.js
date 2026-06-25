import { API_ENDPOINTS, DEFAULT_BUILT_IN_APPS } from '../utils/constants.js';
import { apiJson, apiPostJson } from '../utils/apiFetch.js';
import { deepClone, formatError } from '../utils/helpers.js';

const cloneData = deepClone;
const normalizeAppName = (name) => String(name || '').trim().toLowerCase();
const withoutIndex = (app) => {
  const { index, ...rest } = app || {};
  return rest;
};

/**
 * 应用服务类
 */
export class AppService {
  /**
   * 获取应用列表
   * @returns {Promise<Array>} 应用列表
   */
  static async getApps() {
    try {
      const data = await apiJson(API_ENDPOINTS.APPS);
      return data.apps || [];
    } catch (error) {
      console.error('获取应用列表失败:', error);
      throw new Error(formatError(error));
    }
  }

  /**
   * 保存应用
   * @param {Array} apps 应用列表
   * @param {Object} editApp 编辑的应用（可选）
   * @returns {Promise<boolean>} 是否保存成功
   */
  static async saveApps(apps, editApp = null) {
    try {
      await apiPostJson(API_ENDPOINTS.APPS, { apps, editApp });
      return true;
    } catch (error) {
      console.error('保存应用失败:', error);
      throw new Error(formatError(error));
    }
  }

  /**
   * 删除应用
   * @param {number} index 应用索引
   * @returns {Promise<boolean>} 是否删除成功
   */
  static async deleteApp(index) {
    try {
      await apiJson(API_ENDPOINTS.APP_DELETE(index), { method: 'DELETE' });
      return true;
    } catch (error) {
      console.error('删除应用失败:', error);
      throw new Error(formatError(error));
    }
  }

  /**
   * 批量删除应用（原子操作，按单次快照解释 indices）
   * @param {number[]} indices 应用索引数组
   * @returns {Promise<{deleted:number, remaining:number}>}
   */
  static async batchDeleteApps(indices) {
    try {
      const data = await apiPostJson(API_ENDPOINTS.APPS_BATCH_DELETE, { indices });
      if (data.status === false || data.status === 'false') {
        throw new Error(data.error || '批量删除失败');
      }
      return {
        deleted: Number(data.deleted) || 0,
        remaining: Number(data.remaining) || 0
      };
    } catch (error) {
      console.error('批量删除应用失败:', error);
      throw new Error(formatError(error));
    }
  }

  /**
   * 获取平台信息
   * @returns {Promise<string>} 平台信息
   */
  static async getPlatform() {
    try {
      const data = await apiJson(API_ENDPOINTS.CONFIG);
      return data.platform || 'windows';
    } catch (error) {
      console.error('获取平台信息失败:', error);
      // 默认返回windows平台
      return 'windows';
    }
  }

  /**
   * Get the platform default built-in apps.
   */
  static getDefaultBuiltInApps(platform) {
    return cloneData(DEFAULT_BUILT_IN_APPS[platform] || []);
  }

  /**
   * Restore matching built-in app defaults and append missing built-in apps.
   */
  static restoreDefaultBuiltInApps(apps, platform) {
    const defaults = AppService.getDefaultBuiltInApps(platform);
    const nextApps = cloneData(Array.isArray(apps) ? apps : []);
    let added = 0;
    let restored = 0;

    defaults.forEach((defaultApp) => {
      const defaultName = normalizeAppName(defaultApp.name);
      const existingIndex = nextApps.findIndex((app) => normalizeAppName(app.name) === defaultName);
      const cleanDefault = cloneData(defaultApp);

      if (existingIndex === -1) {
        nextApps.push(cleanDefault);
        added++;
        return;
      }

      const currentComparable = JSON.stringify(withoutIndex(nextApps[existingIndex]));
      const defaultComparable = JSON.stringify(cleanDefault);
      if (currentComparable !== defaultComparable) {
        nextApps[existingIndex] = cleanDefault;
        restored++;
      }
    });

    return {
      apps: nextApps,
      added,
      restored,
      changed: added + restored
    };
  }

  /**
   * 搜索应用
   * @param {Array} apps 应用列表
   * @param {string} query 搜索关键词
   * @returns {Array} 搜索结果
   */
  static searchApps(apps, query) {
    if (!query || !query.trim()) {
      return [...apps];
    }
    
    const searchTerm = query.toLowerCase().trim();
    return apps.filter(app => 
      app.name.toLowerCase().includes(searchTerm) || 
      (app.cmd && app.cmd.toLowerCase().includes(searchTerm))
    );
  }

  /**
   * 验证应用数据
   * @param {Object} app 应用对象
   * @returns {Object} 验证结果
   */
  static validateApp(app) {
    const errors = [];
    
    if (!app.name || !app.name.trim()) {
      errors.push('应用名称不能为空');
    }
    
    if (!app.cmd || !app.cmd.trim()) {
      errors.push('应用命令不能为空');
    }
    
    // 验证退出超时时间
    if (app['exit-timeout'] !== undefined && 
        (isNaN(app['exit-timeout']) || app['exit-timeout'] < 0)) {
      errors.push('退出超时时间必须是非负数');
    }
    
    return {
      isValid: errors.length === 0,
      errors
    };
  }

  /**
   * 格式化应用数据
   * @param {Object} app 原始应用数据
   * @returns {Object} 格式化后的应用数据
   */
  static formatAppData(app) {
    // 过滤掉 do 和 undo 都为空或只包含空格的 prep-cmd 项
    const filteredPrepCmd = Array.isArray(app['prep-cmd']) 
      ? app['prep-cmd'].filter(cmd => {
          const hasDo = cmd.do && cmd.do.trim() !== '';
          const hasUndo = cmd.undo && cmd.undo.trim() !== '';
          // 至少有一个不为空才保留
          return hasDo || hasUndo;
        })
      : [];
    
    return {
      name: app.name?.trim() || '',
      output: app.output?.trim() || '',
      cmd: app.cmd?.trim() || '',
      'exclude-global-prep-cmd': Boolean(app['exclude-global-prep-cmd']),
      elevated: Boolean(app.elevated),
      'auto-detach': Boolean(app['auto-detach']),
      'wait-all': Boolean(app['wait-all']),
      'exit-timeout': parseInt(app['exit-timeout']) || 5,
      'prep-cmd': filteredPrepCmd,
      'menu-cmd': Array.isArray(app['menu-cmd']) ? app['menu-cmd'] : [],
      detached: Array.isArray(app.detached) ? app.detached : [],
      'image-path': app['image-path']?.trim() || '',
      'working-dir': app['working-dir']?.trim() || ''
    };
  }
}

// The words of the 串流计时器 page, the control panel's own (desktop/i18n: sidebar.streamTimer, timer.*); any language but Chinese reads the English.
const ZH = { title: '串流计时器', scale: '缩放', adjust: '调整计时器大小', decrease: '缩小', increase: '放大', reset: '重置' }
const EN = { title: 'Stream Timer', scale: 'Scale', adjust: 'Adjust timer size', decrease: 'Decrease', increase: 'Increase', reset: 'Reset' }
export const streamTimerTexts = (locale = 'en') => (/^zh/i.test(String(locale)) ? ZH : EN)

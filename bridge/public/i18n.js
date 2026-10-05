import { messages } from './i18n-messages.js';
import { staticMessages } from './i18n-static.js';

export const supportedLocales = ['zh-CN', 'en'];
const catalog = { ...messages, ...staticMessages };
let locale = globalThis.navigator?.language?.toLowerCase().startsWith('zh') ? 'zh-CN' : 'en';
const uiMessages = new Map();

export const getLocale = () => locale;
export function setLocale(value) {
  if (!supportedLocales.includes(value)) return false;
  const changed = locale !== value;
  locale = value;
  return changed;
}
export function t(key, params = {}) {
  const message = catalog[key]?.[locale];
  if (typeof message !== 'string') throw new Error(`Missing interface translation: ${key}/${locale}`);
  // Replacement is one pass: user-provided parameter text is never treated as a template.
  return message.replace(/\{([a-zA-Z0-9_]+)\}/g, (match, name) => Object.hasOwn(params, name) ? String(params[name] ?? '') : match);
}
export const m = (key, params = {}) => ({ key, params });
export function setUiMessage(node, value) {
  if (!node) return;
  uiMessages.set(node, value);
  node.textContent = value && typeof value === 'object' && Object.hasOwn(value, 'key') ? t(value.key, value.params) : String(value ?? '');
}
export function refreshUiMessages() {
  for (const [node, value] of uiMessages) {
    if (!node.isConnected) { uiMessages.delete(node); continue; }
    // Raw service errors belong to the response's locale; discard them on a language change.
    setUiMessage(node, value && typeof value === 'object' ? value : '');
  }
}
export function applyStaticTranslations(root = document) {
  document.documentElement.lang = locale;
  for (const node of root.querySelectorAll('[data-i18n]')) node.textContent = t(node.dataset.i18n);
  for (const attribute of ['title', 'placeholder', 'aria-label', 'alt']) {
    for (const node of root.querySelectorAll(`[data-i18n-${attribute}]`)) {
      node.setAttribute(attribute, t(node.getAttribute(`data-i18n-${attribute}`)));
    }
  }
}
export function formatDateTime(value, options = {
  year: 'numeric', month: 'numeric', day: 'numeric',
  hour: '2-digit', minute: '2-digit', second: '2-digit',
}) {
  const date = value instanceof Date ? value : new Date(value);
  return Number.isNaN(date.getTime()) ? '' : new Intl.DateTimeFormat(locale, options).format(date);
}

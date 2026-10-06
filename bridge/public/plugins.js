import { t, getLocale, setLocale, supportedLocales, applyStaticTranslations, setUiMessage } from './i18n.js';

const $ = (id) => document.getElementById(id);
let token = null;
let toastTimer;
let busy = false;
let installedPlugins = [];
let openKey = '';

function showToast(message, error = false) {
  const toast = $('toast');
  setUiMessage(toast, message);
  toast.classList.toggle('error', error);
  toast.classList.add('visible');
  clearTimeout(toastTimer);
  toastTimer = setTimeout(() => toast.classList.remove('visible'), 4200);
}

async function request(url, options = {}) {
  const response = await fetch(url, { cache: 'no-store', ...options });
  let data = {};
  try { data = await response.json(); } catch { /* A non-JSON error still uses the status. */ }
  if (!response.ok) throw new Error(data.error || t('app.017', { v0: response.status }));
  return data;
}

async function sessionToken() {
  if (token) return token;
  const session = await request('/api/session');
  token = session.token;
  return token;
}

async function post(url, body) {
  const headers = { 'Content-Type': 'application/json', 'X-Vibe-Token': await sessionToken() };
  try {
    return await request(url, { method: 'POST', headers, body: JSON.stringify(body || {}) });
  } catch (error) {
    token = null;
    throw error;
  }
}

function button(label, className, action) {
  const node = document.createElement('button');
  node.type = 'button';
  node.className = className;
  node.textContent = label;
  node.addEventListener('click', action);
  return node;
}

function meta(text) {
  const node = document.createElement('p');
  node.className = 'catalog-meta';
  node.textContent = text;
  return node;
}

function pluginKey(plugin) {
  return plugin.id || plugin.file;
}

function hashKey() {
  const raw = location.hash.replace(/^#/, '');
  if (!raw) return '';
  try { return decodeURIComponent(raw); } catch { return ''; }
}

function pluginState(plugin) {
  return plugin.error ? plugin.error : plugin.enabled ? (plugin.available ? t('plug.003') : t('plug.005')) : t('plug.004');
}

function pluginSummary(plugin) {
  const details = [pluginState(plugin), plugin.file];
  if (plugin.events?.length) details.push(`${t('plug.024')} ${plugin.events.join('、')}`);
  if (plugin.commands?.length) details.push(`${t('plug.025')} ${plugin.commands.join('、')}`);
  return details.join(' · ');
}

function pluginCard(plugin) {
  const card = document.createElement('article');
  card.className = 'plugin-tile';
  if (!plugin.enabled || plugin.error) card.classList.add('is-off');
  const open = document.createElement('button');
  open.type = 'button';
  open.className = 'plugin-tile-open';
  const title = document.createElement('span');
  title.className = 'catalog-title';
  title.textContent = plugin.label || plugin.file;
  const subtitle = document.createElement('span');
  subtitle.className = 'catalog-meta';
  subtitle.textContent = pluginState(plugin);
  open.append(title, subtitle);
  open.addEventListener('click', () => openPlugin(plugin));
  card.append(open);
  if (plugin.id) {
    card.append(button(plugin.enabled ? t('plug.006') : t('plug.007'), 'small-button plugin-tile-toggle', () => changePlugin(plugin, !plugin.enabled)));
  }
  return card;
}

function openPlugin(plugin) {
  const key = pluginKey(plugin);
  openKey = key;
  if (hashKey() !== key) history.pushState({ plugin: key }, '', `#${encodeURIComponent(key)}`);
  syncView();
  window.scrollTo(0, 0);
}

function syncView() {
  const plugin = installedPlugins.find((item) => pluginKey(item) === openKey);
  const groups = $('pluginGroups');
  const detail = $('pluginDetail');
  if (!plugin) {
    if (openKey && location.hash) history.replaceState(null, '', location.pathname);
    openKey = '';
    groups.hidden = false;
    detail.hidden = true;
    return;
  }
  groups.hidden = true;
  detail.hidden = false;
  renderDetail(plugin);
}

function renderDetail(plugin) {
  $('pluginDetailTitle').textContent = plugin.label || plugin.file;
  $('pluginDetailMeta').textContent = pluginSummary(plugin);
  const actions = $('pluginDetailActions');
  actions.replaceChildren();
  if (plugin.id) {
    actions.append(button(plugin.enabled ? t('plug.006') : t('plug.007'), 'small-button', () => changePlugin(plugin, !plugin.enabled)));
  }
  actions.append(button(t('plug.008'), 'small-button', () => removeInstalledPlugin(plugin)));
  const body = $('pluginDetailBody');
  body.replaceChildren();
  if (plugin.settingsError) body.append(meta(plugin.settingsError));
  else if (plugin.settingsSpec?.length) body.append(settingsForm(plugin));
  else body.append(meta(t('plug.032')));
}

function fillList(id, items, emptyKey) {
  const list = $(id);
  list.replaceChildren();
  if (!items.length) {
    list.append(meta(t(emptyKey)));
    return;
  }
  for (const item of items) list.append(pluginCard(item));
}

function renderPlugins(plugins) {
  const board = [];
  const agents = [];
  const broken = [];
  for (const plugin of plugins) {
    if (plugin.kind === 'board-plugin') board.push(plugin);
    else if (plugin.kind === 'coding-agent') agents.push(plugin);
    else broken.push(plugin);
  }
  fillList('boardPluginList', board, 'plug.029');
  fillList('agentPluginList', agents, 'plug.030');
  const brokenSection = $('brokenPlugins');
  brokenSection.hidden = broken.length === 0;
  if (broken.length) fillList('brokenPluginList', broken, 'plug.023');
}

function settingsForm(plugin) {
  const form = document.createElement('form');
  form.className = 'voice-settings-form';
  form.append(meta(t('plug.028')));
  const inputs = new Map();
  for (const field of plugin.settingsSpec) {
    const wrap = document.createElement('div');
    const label = document.createElement('label');
    label.className = 'field-label';
    label.textContent = field.label;
    const input = document.createElement('input');
    input.className = 'secret-input';
    input.type = 'text';
    input.value = plugin.settings?.[field.key] ?? '';
    input.spellcheck = false;
    if (field.help) input.title = field.help;
    wrap.append(label, input);
    if (field.help) wrap.append(meta(field.help));
    inputs.set(field.key, input);
    form.append(wrap);
  }
  const save = button(t('plug.026'), 'small-button confirm', () => {});
  save.type = 'submit';
  form.append(save);
  form.addEventListener('submit', async (event) => {
    event.preventDefault();
    const settings = {};
    for (const [key, input] of inputs) settings[key] = input.value;
    try {
      const data = await post('/api/plugins/settings', { id: plugin.id, settings });
      applyInstalled(data);
      showToast(t('plug.027'));
    } catch (error) {
      showToast(error.message, true);
    }
  });
  return form;
}

function renderThemes(themes) {
  const list = $('themeList');
  list.replaceChildren();
  if (!themes.length) {
    list.append(meta(t('plug.012')));
    return;
  }
  for (const theme of themes) {
    const card = document.createElement('article');
    card.className = 'catalog-card';
    const title = document.createElement('h3');
    title.textContent = theme.title || theme.name;
    const assets = [theme.name];
    if (theme.sound) assets.push(t('plug.013'));
    if (theme.icon) assets.push(t('plug.014'));
    if (theme.background) assets.push(t('plug.015'));
    const actions = document.createElement('div');
    actions.className = 'catalog-actions';
    actions.append(button(t('plug.009'), 'small-button confirm', () => applyTheme(theme)));
    actions.append(button(t('plug.010'), 'small-button', () => deleteTheme(theme)));
    card.append(title, meta(assets.join(' · ')), actions);
    list.append(card);
  }
}

function applyInstalled(data) {
  installedPlugins = Array.isArray(data.plugins) ? data.plugins : [];
  renderPlugins(installedPlugins);
  renderThemes(Array.isArray(data.themes) ? data.themes : []);
  if (!openKey) openKey = hashKey();
  syncView();
}

async function refresh() {
  if (busy) return;
  busy = true;
  try {
    applyInstalled(await request('/api/installed'));
    const header = $('headerConnection');
    header.classList.add('online');
    header.querySelector('span:last-child').textContent = t('app.099');
    $('lastUpdated').textContent = new Intl.DateTimeFormat(getLocale(), { hour: '2-digit', minute: '2-digit', second: '2-digit' }).format(new Date());
  } catch (error) {
    const header = $('headerConnection');
    header.classList.remove('online');
    header.querySelector('span:last-child').textContent = t('app.098');
    showToast(error.message, true);
  } finally {
    busy = false;
  }
}

async function changePlugin(plugin, enabled) {
  try {
    const data = await post('/api/plugins/enabled', { id: plugin.id, enabled });
    applyInstalled(data);
    showToast(t('plug.019'));
  } catch (error) {
    showToast(error.message, true);
  }
}

async function removeInstalledPlugin(plugin) {
  const name = plugin.label || plugin.file;
  if (!window.confirm(t('plug.016', { v0: name }))) return;
  try {
    const data = await post('/api/plugins/remove', plugin.id ? { id: plugin.id } : { file: plugin.file });
    applyInstalled(data);
    showToast(t('plug.021'));
  } catch (error) {
    showToast(error.message, true);
  }
}

async function applyTheme(theme) {
  try {
    const data = await post(`/api/themes/${encodeURIComponent(theme.name)}/apply`);
    showToast(t('plug.018', { v0: data.applied }));
  } catch (error) {
    showToast(error.message, true);
  }
}

async function deleteTheme(theme) {
  if (!window.confirm(t('plug.017', { v0: theme.title || theme.name }))) return;
  try {
    const data = await post(`/api/themes/${encodeURIComponent(theme.name)}/remove`);
    renderThemes(data.themes || []);
    showToast(t('plug.020'));
  } catch (error) {
    showToast(error.message, true);
  }
}

$('pluginBack').addEventListener('click', () => {
  if (hashKey()) history.back();
  else {
    openKey = '';
    syncView();
  }
});
window.addEventListener('popstate', () => {
  openKey = hashKey();
  syncView();
});
$('refreshButton').addEventListener('click', refresh);
$('languageSelect').addEventListener('change', async (event) => {
  const requested = event.target.value;
  if (!supportedLocales.includes(requested)) return;
  event.target.disabled = true;
  try {
    const response = await post('/api/settings/language', { locale: requested });
    setLocale(supportedLocales.includes(response.locale) ? response.locale : requested);
    applyStaticTranslations();
    $('languageSelect').value = getLocale();
    await refresh();
  } catch (error) {
    $('languageSelect').value = getLocale();
    showToast(error.message || t('language.failed'), true);
  } finally {
    event.target.disabled = false;
  }
});

applyStaticTranslations();
$('languageSelect').value = getLocale();
for (const id of ['boardPluginList', 'agentPluginList', 'themeList']) $(id).append(meta(t('plug.022')));
refresh();

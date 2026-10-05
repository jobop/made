import { renderProviderIcon, clearProviderIcon } from './provider-icon.js';
import { t, m, getLocale, setLocale, supportedLocales, applyStaticTranslations, setUiMessage, refreshUiMessages, formatDateTime } from './i18n.js';

    (() => {
      'use strict';
      const $ = (id) => document.getElementById(id);
      const taskPage = document.body.dataset.page === 'tasks';
      const assistantsPage = document.body.dataset.page === 'assistants';
      const state = { jobs: [], sessions: [], providers: [], projects: [], connection: {}, voice: {}, models: {}, pairing: { open: false, pending: [], paired: [] }, tunnel: { status: 'off' }, receiver: { status: 'disabled' }, receiverPasswordVisible: false, token: null, lastSyncedAt: null, loaded: false, error: '', busy: new Set(), pairBusy: false, pairWindowBusy: false, tunnelBusy: false, dismissedPairing: new Set(), selectedSessionId: null, previewProvider: '', previewSessionId: null, sessionFormOpen: false, legacySessionsOpen: false, renamingSession: false, refreshing: false };
      if (taskPage) {
        try { state.selectedSessionId = sessionStorage.getItem('vibe-selected-session'); } catch { /* Selection works without storage. */ }
        const params = new URLSearchParams(window.location.search);
        state.sessionFormOpen = params.get('newSession') === '1';
        state.newSessionProvider = params.get('provider') || '';
      }
      if ($('boardScreen')) {
        try {
          const saved = JSON.parse(sessionStorage.getItem('vibe-preview-job') || 'null');
          if (saved && typeof saved.provider === 'string') {
            state.previewProvider = saved.provider;
            state.previewSessionId = typeof saved.sessionId === 'string' ? saved.sessionId : null;
          }
          sessionStorage.removeItem('vibe-preview-job');
        } catch { /* The preview works without a saved selection. */ }
      }
      const statusMap = () => ({
        waiting_confirmation: { label: t('app.001'), css: 'status-waiting', mark: '?' },
        queued: { label: t('app.002'), css: 'status-active', mark: '→' },
        running: { label: t('app.003'), css: 'status-active', mark: '↗' },
        completed: { label: t('app.004'), css: 'status-completed', mark: '✓' },
        handed_off: { label: t('app.005'), css: 'status-handed', mark: '↗' },
        failed: { label: t('app.006'), css: 'status-failed', mark: '!' },
        cancelled: { label: t('app.007'), css: 'status-cancelled', mark: '×' }
      });
      const text = (tag, className, value) => { const node = document.createElement(tag); if (className) node.className = className; node.textContent = value ?? ''; return node; };
      const safeString = (value) => value == null ? '' : String(value);
      const defaultDeviceNames = new Set(['码得', '码达', 'made', '码豆', 'ESP32-S3 Round Display', 'ESP32-S3 \u5706\u5c4f']);
      const deviceDisplayName = (value, fallback = t('app.008')) => {
        const name = safeString(value);
        return defaultDeviceNames.has(name) ? t('app.008') : name || fallback;
      };
      const jobStatus = (job) => job.provider === 'codex' && job.retryPending ?
        { label: t('app.009'), css: 'status-waiting', mark: '?' } :
        statusMap()[job.status] || { label: safeString(job.status) || t('app.010'), css: 'status-cancelled', mark: '·' };
      function userFacingJobError(job) {
        if (job.provider === 'codex' && job.retryPending) return t('app.011');
        const clean = safeString(job.error).replace(/\x1b\[[0-?]*[ -/]*[@-~]/g, '').replace(/\r/g, '\n');
        return clean.split('\n').map((line) => line.trim()).find((line) => line && !/^at\s+/.test(line))?.slice(0, 240) || '';
      }
      const providerById = (id) => state.providers.find((item) => item.id === id);
      const unscopedProvider = (id) => providerById(id)?.capabilities?.session === 'external_unscoped';
      const providerName = (id) => state.providers.find((item) => item.id === id)?.label || safeString(id) || t('app.012');
      const projectName = (id) => state.projects.find((item) => item.id === id)?.label || safeString(id) || t('app.013');
      const projectPath = (id) => state.projects.find((item) => item.id === id)?.path || '';
      const sessionById = (id) => state.sessions.find((item) => item.id === id);
      const selectedSession = () => sessionById(state.selectedSessionId);
      const jobsForSession = (id) => state.jobs.filter((job) => job.vibeSessionId === id);
      const latestJobForSession = (id) => jobsForSession(id).sort((a, b) => new Date(b.createdAt || 0) - new Date(a.createdAt || 0))[0];
      const sessionContextLabel = (session) => unscopedProvider(session.provider) ?
        t('app.014') :
        session.contextMode === 'native' ? t('app.015') :
        t('app.016');
      const sessionId = (job) => /^[A-Za-z0-9_-]{6,128}$/.test(safeString(job.sessionId)) ? job.sessionId : '';
      const canConfirm = (job) => job.status === 'waiting_confirmation';
      const canCancel = (job) => ['waiting_confirmation', 'queued', 'running'].includes(job.status);
      const timeText = (value) => { const date = new Date(value); return Number.isNaN(date.getTime()) ? '' : new Intl.DateTimeFormat(getLocale(), { hour: '2-digit', minute: '2-digit' }).format(date); };
      const setFormMessage = (message, success = false) => { setUiMessage($('formMessage'), message); $('formMessage').classList.toggle('success', success); };
      let toastTimer;
      function showToast(message, error = false) { const toast = $('toast'); setUiMessage(toast, message); toast.classList.toggle('error', error); toast.classList.add('visible'); clearTimeout(toastTimer); toastTimer = setTimeout(() => toast.classList.remove('visible'), 4200); }

      async function request(url, options = {}) {
        const response = await fetch(url, { cache: 'no-store', ...options });
        let data;
        try { data = await response.json(); } catch { data = {}; }
        if (!response.ok) throw new Error(data.error || data.message || t('app.017', { v0: (response.status) }));
        return data;
      }
      async function sessionToken() {
        if (state.token) return state.token;
        const session = await request('/api/session');
        if (!session.token) throw new Error(t('app.018'));
        state.token = session.token;
        return state.token;
      }
      async function post(url, body) {
        const token = await sessionToken();
        try {
          return await request(url, { method: 'POST', headers: { 'Content-Type': 'application/json', 'X-Vibe-Token': token }, body: JSON.stringify(body || {}) });
        } catch (error) {
          if (String(error.message).includes('401') || String(error.message).includes('403')) state.token = null;
          throw error;
        }
      }
      let languageBusy = false;
      let languageEpoch = 0;
      async function refresh() {
        if (state.refreshing) return;
        state.refreshing = true;
        const refreshEpoch = languageEpoch;
        try {
          const data = await request('/api/state');
          state.jobs = Array.isArray(data.jobs) ? data.jobs : [];
          state.sessions = Array.isArray(data.sessions) ? data.sessions : [];
          if (state.selectedSessionId && !state.sessions.some((item) => item.id === state.selectedSessionId)) state.selectedSessionId = null;
          if (!state.selectedSessionId) state.selectedSessionId = state.sessions.find((item) => !item.legacy)?.id || null;
          if (!state.loaded && sessionById(state.selectedSessionId)?.legacy) state.legacySessionsOpen = true;
          if (taskPage && state.selectedSessionId) {
            try { sessionStorage.setItem('vibe-selected-session', state.selectedSessionId); } catch { /* Volatile selection is fine. */ }
          }
          state.providers = Array.isArray(data.providers) ? data.providers : [];
          state.projects = Array.isArray(data.projects) ? data.projects : [];
          state.connection = data.connection && typeof data.connection === 'object' ? data.connection : {};
          state.voice = data.voice && typeof data.voice === 'object' ? data.voice : {};
          state.models = data.models && typeof data.models === 'object' ? data.models : {};
          state.pairing = data.pairing && typeof data.pairing === 'object' ? data.pairing : { open: false, pending: [], paired: [] };
          state.tunnel = data.tunnel && typeof data.tunnel === 'object' ? data.tunnel : { status: 'off' };
          state.receiver = data.receiver && typeof data.receiver === 'object' ? data.receiver : { status: 'disabled' };
          state.loaded = true;
          state.error = '';
          if (!languageBusy && refreshEpoch === languageEpoch && supportedLocales.includes(data.locale)) applyLanguage(data.locale);
          state.lastSyncedAt = Date.now();
        } catch (error) { state.error = error.message || t('app.020'); }
        finally { state.refreshing = false; render(); }
      }
      async function act(job, action) {
        if (state.busy.has(job.id)) return;
        state.busy.add(job.id); render();
        try {
          await post(`/api/jobs/${encodeURIComponent(job.id)}/${action}`);
          showToast(action === 'confirm' ?
            job.retryPending ? t('app.021') : t('app.022') :
            t('app.023'));
          await refresh();
        } catch (error) { showToast(error.message || t('app.024'), true); }
        finally { state.busy.delete(job.id); render(); }
      }
      function actionButton(job, action, compact = false) {
        const isConfirm = action === 'confirm';
        const label = isConfirm ? (job.retryPending ? t('app.025') : t('app.026')) : t('app.027');
        const button = text('button', compact ? `board-button ${isConfirm ? 'confirm' : ''}` : `small-button ${isConfirm ? 'confirm' : ''}`, compact ? (isConfirm ? t('app.028') : t('app.029')) : label);
        button.type = 'button'; button.disabled = state.busy.has(job.id);
        button.addEventListener('click', () => act(job, action));
        return button;
      }
      async function copyText(value, label) {
        try {
          if (!navigator.clipboard?.writeText) throw new Error(t('app.030'));
          await navigator.clipboard.writeText(value);
          showToast(t('app.031', { v0: (label) }));
        } catch { showToast(t('app.032'), true); }
      }
      function copyLine(value, label) {
        const line = text('div', 'handoff-line');
        const code = text('code', '', value);
        const button = text('button', 'small-button', t('app.033', { v0: (label) })); button.type = 'button';
        button.addEventListener('click', () => copyText(value, label));
        line.append(code, button);
        return line;
      }
      function handoffPanel(job) {
        const id = sessionId(job);
        if (!id || !['codex', 'cursor'].includes(job.provider)) return null;
        const panel = text('div', 'job-handoff');
        if (job.provider === 'codex') {
          const link = text('a', 'small-button handoff-open', t('app.034'));
          link.href = `codex://threads/${encodeURIComponent(id)}`;
          panel.append(link, copyLine(`codex resume --include-non-interactive ${id}`, t('app.035')),
            text('span', 'handoff-note', t('app.036')));
        } else {
          panel.append(copyLine(`cursor-agent --resume ${id}`, t('app.035')));
          const path = projectPath(job.projectId);
          if (path) panel.append(copyLine(path, t('app.037')));
          panel.append(text('span', 'handoff-note', t('app.038')));
        }
        return panel;
      }
      function renderSelects() {
        if (!$('provider')) return;
        const provider = $('provider'), project = $('project');
        const currentProvider = provider.value, currentProject = project.value;
        provider.replaceChildren(); project.replaceChildren();
        const available = state.providers.filter((item) => item.available);
        if (!available.length) provider.append(new Option(state.loaded ? t('app.039') : t('app.040'), ''));
        else available.forEach((item) => provider.append(new Option(item.label || item.id, item.id)));
        if (!state.projects.length) project.append(new Option(state.loaded ? t('app.041') : t('app.040'), ''));
        else state.projects.forEach((item) => project.append(new Option(item.label || item.id, item.id)));
        if (state.newSessionProvider && [...provider.options].some((option) => option.value === state.newSessionProvider)) {
          provider.value = state.newSessionProvider;
          state.newSessionProvider = '';
        } else if ([...provider.options].some((option) => option.value === currentProvider)) provider.value = currentProvider;
        if ([...project.options].some((option) => option.value === currentProject)) project.value = currentProject;
        $('createSessionButton').disabled = !available.length || !state.projects.length || !!state.error || state.busy.has('new-session');
        $('createButton').disabled = !selectedSession() || !!state.error || state.busy.has('new-turn');
        const notice = $('sessionContextNotice');
        if (notice) notice.textContent = unscopedProvider(provider.value) ?
          t('app.042', { v0: (providerName(provider.value)) }) :
          providerById(provider.value)?.capabilities?.session === 'native' ? t('app.043') : t('app.044');
      }
      function appendTaskCard(list, session, historical = false) {
        const button = text('button', `session-item ${historical ? 'legacy-session-item' : ''} ${session.id === state.selectedSessionId ? 'active' : ''}`);
        button.type = 'button';
        button.setAttribute('aria-current', session.id === state.selectedSessionId ? 'true' : 'false');
        button.append(text('strong', 'session-item-title', session.title || t('app.045')));
        const latest = latestJobForSession(session.id);
        button.append(text('span', 'session-item-meta', `${providerName(session.provider)} · ${projectName(session.projectId)}`));
        button.append(text('span', 'session-item-updated', t('app.048', { v0: (latest ? jobStatus(latest).label : t('app.046')), v1: (timeText(session.updatedAt) || t('app.047')) })));
        button.addEventListener('click', () => {
          state.selectedSessionId = session.id;
          state.renamingSession = false;
          try { sessionStorage.setItem('vibe-selected-session', session.id); } catch { /* Volatile selection is fine. */ }
          render();
        });
        list.append(button);
      }
      function renderSessions() {
        if (!$('sessionList')) return;
        const current = state.sessions.filter((session) => !session.legacy);
        const historical = state.sessions.filter((session) => session.legacy);
        const open = state.sessionFormOpen || (state.loaded && !current.length);
        $('sessionForm').hidden = !open;
        $('newSessionButton').hidden = state.loaded && !current.length;
        $('newSessionButton').setAttribute('aria-expanded', String(open));
        $('newSessionButton').textContent = open ? t('app.049') : t('app.050');
        const list = $('sessionList'); list.replaceChildren();
        if (state.error) {
          list.append(text('p', 'session-list-empty', t('app.051')));
        } else if (!current.length) {
          list.append(text('p', 'session-list-empty', state.loaded ?
            historical.length ? t('app.052') : t('app.053') :
            t('app.054')));
        } else {
          current.forEach((session) => appendTaskCard(list, session));
        }
        const legacySection = $('legacySessions');
        const legacyList = $('legacySessionList');
        legacySection.hidden = !historical.length || !!state.error;
        $('legacySessionToggle').textContent = t('app.055', { v0: (historical.length) });
        $('legacySessionToggle').setAttribute('aria-expanded', String(state.legacySessionsOpen));
        legacyList.hidden = !state.legacySessionsOpen;
        legacyList.replaceChildren();
        if (state.legacySessionsOpen) historical.forEach((session) => appendTaskCard(legacyList, session, true));
        const selected = selectedSession();
        $('currentSessionTitle').textContent = selected?.title || t('app.056');
        $('currentSessionMeta').textContent = selected ?
          `${providerName(selected.provider)} · ${projectName(selected.projectId)} · ${sessionContextLabel(selected)}` :
          t('app.057');
        $('currentSessionWarning').hidden = !unscopedProvider(selected?.provider);
        $('currentSessionWarning').textContent = unscopedProvider(selected?.provider) ?
          t('app.058', { v0: (providerName(selected.provider)) }) : '';
        $('renameSessionButton').hidden = !selected;
        $('renameSessionForm').hidden = !selected || !state.renamingSession;
        $('renameSessionButton').disabled = !!state.error || state.busy.has('rename-session');
        $('renameSessionSubmit').disabled = !!state.error || state.busy.has('rename-session');
      }
      async function createSession() {
        const provider = $('provider').value;
        const projectId = $('project').value;
        if (!provider || !projectId) {
          setUiMessage($('sessionFormMessage'), m('app.059'));
          return;
        }
        if (state.busy.has('new-session')) return;
        state.busy.add('new-session'); renderSelects();
        setUiMessage($('sessionFormMessage'), m('app.060'));
        try {
          const session = await post('/api/sessions', { provider, projectId });
          state.selectedSessionId = session.id;
          state.sessionFormOpen = false;
          state.newSessionProvider = '';
          setUiMessage($('sessionFormMessage'), '');
          try { sessionStorage.setItem('vibe-selected-session', session.id); } catch { /* Volatile selection is fine. */ }
          showToast(t('app.061'));
          await refresh();
          $('instruction').focus();
        } catch (error) {
          setUiMessage($('sessionFormMessage'), error.message || m('app.062'));
          showToast(error.message || t('app.062'), true);
        } finally { state.busy.delete('new-session'); render(); }
      }
      async function renameSession() {
        const session = selectedSession();
        const title = $('renameSessionTitle').value.trim();
        if (!session || !title || state.busy.has('rename-session')) return;
        state.busy.add('rename-session'); renderSessions();
        setUiMessage($('renameSessionMessage'), m('app.063'));
        try {
          await post(`/api/sessions/${encodeURIComponent(session.id)}/rename`, { title });
          state.renamingSession = false;
          setUiMessage($('renameSessionMessage'), '');
          showToast(t('app.064'));
          await refresh();
        } catch (error) {
          setUiMessage($('renameSessionMessage'), error.message || m('app.065'));
          showToast(error.message || t('app.065'), true);
        } finally { state.busy.delete('rename-session'); render(); }
      }
      function renderJobs() {
        if (!$('jobList')) return;
        const list = $('jobList'); list.replaceChildren();
        if (state.error) {
          const empty = text('div', 'empty'); empty.append(text('div', 'empty-icon', '!'), text('strong', '', t('app.066')), text('span', '', state.error)); list.append(empty); return;
        }
        const session = selectedSession();
        if (!session) {
          const empty = text('div', 'empty'); empty.append(text('div', 'empty-icon', '+'), text('strong', '', t('app.067')), text('span', '', t('app.068')));
          list.append(empty); return;
        }
        const job = latestJobForSession(session.id);
        if (!job) {
          const empty = text('div', 'empty'); empty.append(text('div', 'empty-icon', '⌁'), text('strong', '', t('app.069')), text('span', '', t('app.070'))); list.append(empty); return;
        }
        const info = jobStatus(job);
        const card = text('article', 'job-card task-detail-card');
        const top = text('div', 'job-top');
        top.append(text('div', 'job-meta', t('app.071', { v0: (timeText(job.createdAt) || t('app.047')) })), text('span', `job-status ${info.css}`, info.label));
        card.append(top, text('div', 'job-instruction', t('app.073', { v0: (job.instruction || t('app.072')) })));
        const answer = userFacingJobError(job) || (job.status === 'running' ? job.progress : '') || job.result ||
          (job.status === 'waiting_confirmation' ? t('app.074') :
            job.status === 'queued' ? t('app.075') : t('app.076'));
        card.append(text('p', `job-summary turn-answer ${job.error && !job.retryPending ? 'error' : ''}`, t('app.077', { v0: (providerName(job.provider)), v1: (safeString(answer)) })));
        if (job.provider === 'codex' && job.retryPending) {
          card.append(text('p', 'job-retry-notice', t('app.078')));
        }
        const handoff = handoffPanel(job); if (handoff) card.append(handoff);
        const actions = text('div', 'job-actions');
        if (canConfirm(job)) actions.append(actionButton(job, 'confirm'));
        if (canCancel(job)) actions.append(actionButton(job, 'cancel'));
        if (actions.children.length) { const bottom = text('div', 'job-bottom'); bottom.append(actions); card.append(bottom); }
        list.append(card);
      }
      const boardEncoder = new TextEncoder();
      function boardCleanText(value) {
        return safeString(value)
          .replace(/\x1b\[[0-?]*[ -/]*[@-~]/g, '')
          .replace(/\r\n?/g, '\n')
          .replace(/\t/g, '  ')
          .replace(/[\u0000-\u0009\u000b-\u001f\u007f]/g, ' ')
          .replace(/[\p{Extended_Pictographic}\p{Emoji_Modifier}\uFE0E\uFE0F\u200D]/gu, '');
      }
      function boardDeviceText(value, maxBytes) {
        const clean = boardCleanText(value);
        let content = '', bytes = 0;
        for (const character of clean) {
          const size = boardEncoder.encode(character).length;
          if (bytes + size > maxBytes) break;
          content += character;
          bytes += size;
        }
        return { content, truncated: bytes < boardEncoder.encode(clean).length };
      }
      function boardResultPreview(value, maxBytes) {
        const full = boardCleanText(value);
        if (boardEncoder.encode(full).length <= maxBytes) return { content: full, truncated: false };
        const separator = '\n…\n';
        const prefix = boardDeviceText(full, Math.floor(maxBytes / 3)).content;
        const remaining = maxBytes - boardEncoder.encode(prefix).length - boardEncoder.encode(separator).length;
        const suffix = [];
        let bytes = 0;
        for (const character of Array.from(full).reverse()) {
          const size = boardEncoder.encode(character).length;
          if (bytes + size > remaining) break;
          suffix.push(character);
          bytes += size;
        }
        return { content: prefix + separator + suffix.reverse().join(''), truncated: true };
      }
      let boardRenderedJobKey = null;
      function previewSessions() {
        return state.sessions.filter((session) => session.provider === state.previewProvider && !session.legacy);
      }
      function previewSession() {
        const sessions = previewSessions();
        return sessions.find((session) => session.id === state.previewSessionId) || sessions[0];
      }
      function renderBoard() {
        if (!$('boardScreen')) return;
        const provider = state.error ? null : providerById(state.previewProvider) || state.providers[0];
        $('boardNewSession').hidden = !provider;
        $('boardNewSession').disabled = !provider;
        $('boardAnimal').hidden = !provider;
        if (!provider) {
          state.previewProvider = '';
          state.previewSessionId = null;
          boardRenderedJobKey = null;
          clearProviderIcon($('boardAnimal'));
          $('boardProviderName').textContent = state.loaded && !state.error ? t('app.079') : t('app.040');
          $('boardPage').textContent = '—';
          $('boardStatus').textContent = state.loaded && !state.error ? t('app.080') : t('app.081');
          $('boardStatus').title = '';
          $('boardTitle').textContent = '';
          $('boardTitle').title = '';
          $('boardResult').textContent = '';
          $('boardResult').scrollTop = 0;
          return;
        }
        state.previewProvider = provider.id;
        const page = state.providers.indexOf(provider);
        const available = provider.available;
        const sessions = previewSessions();
        const session = previewSession();
        state.previewSessionId = session?.id || null;
        const job = session ? latestJobForSession(session.id) : null;
        const renderedJobKey = `${provider.id}:${session?.id || ''}:${job?.id || ''}:${job?.status || ''}`;
        const resetAnswerScroll = renderedJobKey !== boardRenderedJobKey;
        boardRenderedJobKey = renderedJobKey;
        const answerNode = $('boardResult');
        const showAnswer = (value) => {
          const content = safeString(value);
          const previousScroll = answerNode.scrollTop;
          if (answerNode.textContent !== content) answerNode.textContent = content;
          answerNode.scrollTop = resetAnswerScroll ? 0 : previousScroll;
        };
        renderProviderIcon($('boardAnimal'), provider.icon);
        $('boardProviderName').textContent = provider.label;
        $('boardPage').textContent = `${page + 1} / ${state.providers.length}`;
        $('boardStatus').textContent = session ? t('app.082', { v0: (sessions.indexOf(session) + 1), v1: (sessions.length), v2: (session.title || t('app.045')) }) : t('app.083');
        $('boardStatus').title = session?.title || t('app.083');
        if (!job) {
          const title = !session ? t('app.084') : t('app.085');
          $('boardTitle').textContent = title;
          $('boardTitle').title = title;
          showAnswer(state.error ? t('app.086') :
            available === false ? t('app.087') :
            !state.voice.available ? t('app.088') :
            !session ? t('app.089') :
            t('app.090'));
          return;
        }
        const instruction = boardDeviceText(job.instruction, 1200);
        const title = t('app.091', { v0: (jobStatus(job).label), v1: (instruction.content || t('app.072')) });
        $('boardTitle').textContent = title;
        $('boardTitle').title = t('app.073', { v0: (job.instruction || t('app.072')) });
        const result = boardResultPreview(job.result, 1200);
        const deviceResult = result.content + ((result.truncated || job.resultTruncated) ? t('app.092') : '');
        const pendingNotice = (instruction.truncated || job.instructionTruncated) ? t('app.093') :
          boardDeviceText(userFacingJobError(job), 96).content || t('app.094');
        const pendingInstruction = `${pendingNotice}\n${instruction.content || t('app.095')}`;
        showAnswer((job.status === 'waiting_confirmation' ? pendingInstruction : '') ||
          boardDeviceText(userFacingJobError(job), 96).content ||
          (job.status === 'running' ? boardDeviceText(job.progress, 160).content : '') ||
          (job.status === 'queued' ? t('app.096') : '') ||
          deviceResult || t('app.097'));
      }
      let boardPointerStart = null;
      function changeBoardProvider(direction) {
        if (!state.providers.length) return;
        const current = Math.max(0, state.providers.findIndex((item) => item.id === state.previewProvider));
        state.previewProvider = state.providers[(current + direction + state.providers.length) % state.providers.length].id;
        state.previewSessionId = null;
        renderBoard();
      }
      function changeBoardSession(direction) {
        const sessions = previewSessions();
        if (sessions.length < 2) return;
        const current = Math.max(0, sessions.findIndex((session) => session.id === state.previewSessionId));
        state.previewSessionId = sessions[(current + direction + sessions.length) % sessions.length].id;
        renderBoard();
      }
      function renderConnection() {
        if (state.lastSyncedAt) $('lastUpdated').textContent = t('app.019', { v0: formatDateTime(state.lastSyncedAt, { hour: '2-digit', minute: '2-digit', second: '2-digit' }) });
        const header = $('headerConnection'); header.classList.toggle('online', !state.error && state.loaded); header.querySelector('span:last-child').textContent = state.error ? t('app.098') : state.loaded ? t('app.099') : t('app.100');
        if (!$('connectionRows')) return;
        const rows = $('connectionRows'); rows.replaceChildren();
        const entries = [
          { label: t('app.101'), value: state.error ? t('app.102') : state.loaded ? t('app.103') : t('app.104'), css: state.error ? 'warn' : state.loaded ? 'ok' : 'off', on: !state.error && state.loaded },
          { label: t('app.105'), value: state.voice.available ? t('app.106') : state.voice.reason || t('app.107'), css: state.voice.available ? 'ok' : 'warn', on: state.voice.available },
          { label: t('app.108'), value: state.connection.deviceApi === 'enabled' ? t('app.109') : t('app.110'), css: state.connection.deviceApi === 'enabled' ? 'ok' : 'off', on: state.connection.deviceApi === 'enabled' },
          { label: t('app.111'), value: t('app.112', { v0: ((state.pairing.paired || []).filter((item) => item.online).length), v1: ((state.pairing.paired || []).length) }), css: state.pairing.paired?.some((item) => item.online) ? 'ok' : 'off', on: state.pairing.paired?.some((item) => item.online) }
        ];
        if (state.receiver.status === 'connected') entries.splice(3, 0, { label: state.receiver.mode === 'direct' ? t('app.113') : t('app.114'), value: t('app.115'), css: 'ok', on: true });
        else if (state.receiver.status === 'waiting') entries.splice(3, 0, { label: t('app.116'), value: t('app.117'), css: 'off', on: false });
        entries.forEach((item) => { const row = text('div', 'info-row'); const name = text('span', 'info-name'); const dot = text('span', `status-dot ${item.on ? 'on' : ''}`); name.append(dot, document.createTextNode(item.label)); row.append(name, text('span', `info-value ${item.css}`, item.value)); rows.append(row); });
      }
      function renderReceiver() {
        if (!$('receiverStatus')) return;
        const receiver = state.receiver || {};
        const connected = receiver.status === 'connected';
        const direct = receiver.mode === 'direct';
        const status = $('receiverStatus');
        status.classList.toggle('open', connected);
        if (state.error) status.textContent = t('app.118');
        else if (connected && direct) status.textContent = t('app.119', { v0: (receiver.port ? ` · ${receiver.port}` : '') });
        else if (connected) status.textContent = t('app.120', { v0: (receiver.port ? ` · ${receiver.port}` : '') });
        else if (receiver.status === 'waiting') status.textContent = direct ? t('app.121') : t('app.122');
        else if (receiver.status === 'disconnected') status.textContent = receiver.error ? t('app.123', { v0: (receiver.error) }) : t('app.124');
        else status.textContent = t('app.125');
        const details = $('receiverDetails');
        details.hidden = direct || !connected || !receiver.ssid || !receiver.password;
        const directDetails = $('receiverDirectDetails');
        if (directDetails) {
          directDetails.hidden = !direct || !connected;
          $('receiverDeviceName').textContent = deviceDisplayName(receiver.deviceName);
          $('receiverDeviceId').textContent = receiver.deviceId || '';
        }
        const help = $('receiverHelp');
        if (help) help.textContent = direct ? t('app.126') : t('app.127');
        if (details.hidden) return;
        $('receiverSsid').textContent = receiver.ssid;
        $('receiverPassword').textContent = state.receiverPasswordVisible ? receiver.password : '••••••••••••';
        $('receiverRevealPassword').textContent = state.receiverPasswordVisible ? t('app.128') : t('app.129');
        $('receiverRevealPassword').setAttribute('aria-pressed', String(state.receiverPasswordVisible));
      }
      let modelSettingsBusy = false;
      const modelDrafts = new Map();
      let modelFieldsSignature = '';
      const modelInputId = (id) => ['codex', 'cursor', 'qoder'].includes(id) ? `${id}Model` : `model-${id}`;
      function renderModelSettings() {
        if (!$('modelSettingsForm') || !state.loaded) return;
        const providers = state.providers.filter((provider) => provider.capabilities?.model);
        const signature = JSON.stringify([getLocale(), providers.map(({ id, label, model }) => ({ id, label, model }))]);
        if (signature !== modelFieldsSignature) {
          const fields = $('modelPluginFields'); fields.replaceChildren();
          providers.forEach((provider) => {
            const wrapper = text('div');
            const id = modelInputId(provider.id);
            const label = text('label', 'field-label', provider.label); label.htmlFor = id;
            const input = text('input', 'secret-input');
            input.id = id; input.type = 'text'; input.spellcheck = false; input.autocapitalize = 'off';
            input.required = Boolean(provider.model?.required);
            input.placeholder = provider.model?.default || t('app.130', { v0: (provider.label) });
            input.dataset.modelProvider = provider.id;
            input.addEventListener('input', () => modelDrafts.set(provider.id, input.value));
            wrapper.append(label, input); fields.append(wrapper);
          });
          if (!providers.length) fields.append(text('p', 'panel-intro', t('app.131')));
          modelFieldsSignature = signature;
        }
        providers.forEach((provider) => {
          const input = $(modelInputId(provider.id));
          if (!input) return;
          if (document.activeElement !== input) input.value = modelDrafts.get(provider.id) ?? state.models[provider.id] ?? provider.model?.default ?? '';
          input.disabled = modelSettingsBusy || !!state.error;
        });
        const unsupported = state.providers.filter((provider) => !provider.capabilities?.model).map((provider) => provider.label);
        $('modelPluginHelp').textContent = t('app.134', { v0: (unsupported.length ? t('app.133', { v0: (unsupported.join(t('app.132'))) }) : '') });
        $('saveModels').disabled = !providers.length || modelSettingsBusy || !!state.error;
      }
      async function saveModels() {
        if (modelSettingsBusy) return;
        const models = Object.fromEntries(state.providers.filter((provider) => provider.capabilities?.model)
          .map((provider) => [provider.id, $(modelInputId(provider.id)).value.trim()]));
        modelSettingsBusy = true;
        renderModelSettings();
        setUiMessage($('modelSettingsMessage'), m('app.063'));
        try {
          const response = await post('/api/settings/models', models);
          state.models = response.models || models;
          modelDrafts.clear();
          setUiMessage($('modelSettingsMessage'), m('app.135'));
          showToast(t('app.135'));
          await refresh();
        } catch (error) {
          setUiMessage($('modelSettingsMessage'), error.message || m('app.136'));
          showToast(error.message || t('app.136'), true);
        } finally { modelSettingsBusy = false; renderModelSettings(); }
      }
      let voiceSettingsBusy = false;
      let voiceFieldsTouched = false;
      function renderVoiceSettings() {
        if (!$('voiceSettingsForm') || !state.loaded) return;
        if (!voiceFieldsTouched && !voiceSettingsBusy) {
          $('voiceEndpoint').value = state.voice.url || 'https://api.openai.com/v1/audio/transcriptions';
          $('voiceModel').value = state.voice.model || 'gpt-transcribe';
        }
        const source = state.voice.keySource || 'none';
        const compatible = (state.voice.pluginId || state.voice.mode) === 'openai';
        $('voiceKeyStatus').textContent = state.error ? t('app.137') : !compatible ? t('app.138') :
          source === 'none' ? t('app.139') :
          source === 'environment' ? t('app.140') : t('app.141');
        for (const id of ['voiceEndpoint', 'voiceModel', 'openaiKey', 'saveOpenaiKey']) $(id).disabled = voiceSettingsBusy || !!state.error;
        $('openaiKey').required = source === 'none';
        $('clearOpenaiKey').disabled = voiceSettingsBusy || !!state.error || source === 'none';
      }
      function setVoiceSettingsMessage(message, success = false) {
        const node = $('voiceSettingsMessage');
        setUiMessage(node, message);
        node.classList.toggle('success', success);
      }
      function openVoiceSettings() {
        $('voiceSettings').scrollIntoView({ behavior: 'smooth', block: 'center' });
        setTimeout(() => $('voiceEndpoint').focus({ preventScroll: true }), 350);
      }
      async function saveVoiceSettings(clearKey = false) {
        if (voiceSettingsBusy) return;
        const settings = {url: $('voiceEndpoint').value.trim(), model: $('voiceModel').value.trim(),
          apiKey: clearKey ? '' : $('openaiKey').value.trim(), clearKey};
        voiceSettingsBusy = true; renderVoiceSettings(); setVoiceSettingsMessage(m('app.063'));
        try {
          const result = await post('/api/settings/voice', settings);
          if (result.voice) state.voice = result.voice;
          $('openaiKey').value = '';
          voiceFieldsTouched = false;
          const message = clearKey ? (result.voice?.keySource === 'environment' ? m('app.142') : m('app.143')) : m('app.144');
          setVoiceSettingsMessage(message, true); showToast(message);
          await refresh();
        } catch (error) {
          setVoiceSettingsMessage(error.message || m('app.145'));
          showToast(error.message || t('app.145'), true);
        } finally { voiceSettingsBusy = false; renderVoiceSettings(); }
      }
      function pairingTime(value) {
        const timestamp = typeof value === 'number' && value < 1e12 ? value * 1000 : new Date(value).getTime();
        return Number.isFinite(timestamp) ? timestamp : null;
      }
      function pairingRemaining(value) {
        const until = pairingTime(value);
        if (!until) return '';
        const seconds = Math.max(0, Math.ceil((until - Date.now()) / 1000));
        return t('app.146', { v0: (Math.floor(seconds / 60)), v1: (String(seconds % 60).padStart(2, '0')) });
      }
      function currentPairingRequest() {
        const pending = Array.isArray(state.pairing.pending) ? state.pairing.pending : [];
        return pending.find((item) => item && item.deviceId && item.nonce && !state.dismissedPairing.has(item.nonce));
      }
      function renderPairing() {
        const pairing = state.pairing || {};
        const paired = Array.isArray(pairing.paired) ? pairing.paired : [];
        const pending = Array.isArray(pairing.pending) ? pairing.pending : [];
        const openButton = $('pairingOpenButton');
        if (openButton) {
        const closeButton = $('pairingCloseButton');
        $('pairingShowButton').hidden = !pending.length || !!currentPairingRequest();
        openButton.hidden = !!pairing.open;
        openButton.disabled = state.pairWindowBusy || !!state.error;
        closeButton.hidden = !pairing.open;
        closeButton.disabled = state.pairWindowBusy || !!state.error;
        const status = $('pairingState');
        status.classList.toggle('open', !!pairing.open && !state.error);
        if (state.error) status.textContent = t('app.147');
        else if (pending.length) status.textContent = t('app.148', { v0: (pending.length) });
        else if (pairing.open) status.textContent = t('app.150', { v0: (pairing.openUntil ? t('app.149', { v0: (pairingRemaining(pairing.openUntil)) }) : '') });
        else status.textContent = paired.length ? t('app.151') : t('app.152');
        const list = $('pairedDevices'); list.replaceChildren();
        paired.forEach((device) => {
          const row = text('div', 'pairing-device');
          const details = text('div', 'pairing-device-text');
          const name = deviceDisplayName(device.deviceName, t('app.153', { v0: (safeString(device.deviceId).slice(0, 8)) }));
          const seen = pairingTime(device.lastSeen);
          details.append(text('span', 'pairing-device-name', name), text('span', 'pairing-device-meta', `${device.online ? t('app.103') : t('app.102')}${seen ? t('app.154', { v0: (formatDateTime(seen)) }) : ''}`));
          const remove = text('button', 'small-button', t('app.155'));
          remove.type = 'button'; remove.title = t('app.156', { v0: (name) });
          remove.disabled = state.busy.has(`pair:${device.deviceId}`);
          remove.addEventListener('click', () => removePairing(device));
          row.append(text('span', `status-dot ${device.online ? 'on' : ''}`), details, remove); list.append(row);
        });
        }
        const request = currentPairingRequest();
        const overlay = $('pairingOverlay');
        const wasHidden = overlay.hidden;
        overlay.hidden = !request;
        document.title = request ? t('app.157') : taskPage ? t('app.158') : assistantsPage ? t('app.159') : t('app.160');
        if (!request) return;
        $('pairingDialogTitle').textContent = t('app.161', { v0: (deviceDisplayName(request.deviceName)) });
        $('pairingEndpoint').textContent = request.endpoint === 'usb.vibe.local:8788' ? t('app.162') : t('app.164', { v0: (safeString(request.endpoint) || t('app.163')) });
        $('pairingCode').textContent = safeString(request.code).padStart(6, '0');
        $('pairingRemaining').textContent = t('app.166', { v0: (pairingRemaining(request.expiresAt) || t('app.165')) });
        $('pairingConfirmButton').disabled = state.pairBusy;
        $('pairingRejectButton').disabled = state.pairBusy;
        $('pairingDismissButton').disabled = state.pairBusy;
        if (wasHidden) $('pairingRejectButton').focus();
      }
      async function setPairingWindow(open) {
        if (state.pairWindowBusy) return;
        state.pairWindowBusy = true; renderPairing();
        try {
          await post(open ? '/api/pair/open' : '/api/pair/close');
          showToast(open ? t('app.167') : t('app.168'));
          await refresh();
        } catch (error) { showToast(error.message || t('app.169'), true); }
        finally { state.pairWindowBusy = false; renderPairing(); }
      }
      async function decidePairing(action) {
        const request = currentPairingRequest();
        if (!request || state.pairBusy) return;
        state.pairBusy = true; renderPairing();
        try {
          await post(`/api/pair/${encodeURIComponent(request.deviceId)}/${action}`, { nonce: request.nonce });
          showToast(action === 'confirm' ? t('app.170') : t('app.171'));
          await refresh();
        } catch (error) { showToast(error.message || t('app.172'), true); }
        finally { state.pairBusy = false; renderPairing(); }
      }
      async function removePairing(device) {
        const id = safeString(device.deviceId);
        if (!id || state.busy.has(`pair:${id}`)) return;
        const name = deviceDisplayName(device.deviceName, t('app.173'));
        if (!window.confirm(t('app.174', { v0: (name) }))) return;
        state.busy.add(`pair:${id}`); renderPairing();
        try {
          await post(`/api/pair/${encodeURIComponent(id)}/remove`);
          showToast(t('app.175'));
          await refresh();
        } catch (error) { showToast(error.message || t('app.176'), true); }
        finally { state.busy.delete(`pair:${id}`); renderPairing(); }
      }
      // Fixed, public vendor entry points. No local state or credentials are added to these URLs.
      const tunnelOfficialEntries = {
        quick: { name: 'static.060', links: [
          ['tunnel.official.download', 'https://developers.cloudflare.com/tunnel/downloads/'],
          ['tunnel.official.guide', 'https://developers.cloudflare.com/tunnel/get-started/#quick-tunnels-development'],
        ] },
        named: { name: 'static.061', links: [
          ['tunnel.official.signup', 'https://dash.cloudflare.com/sign-up'],
          ['tunnel.official.console', 'https://dash.cloudflare.com/'],
          ['tunnel.official.download', 'https://developers.cloudflare.com/tunnel/downloads/'],
          ['tunnel.official.guide', 'https://developers.cloudflare.com/tunnel/get-started/'],
        ] },
        ngrok: { name: 'static.063', links: [
          ['tunnel.official.signup', 'https://dashboard.ngrok.com/signup'],
          ['tunnel.official.authtoken', 'https://dashboard.ngrok.com/get-started/your-authtoken'],
          ['tunnel.official.download', 'https://ngrok.com/download'],
          ['tunnel.official.guide', 'https://ngrok.com/docs/share-localhost/quickstart'],
        ] },
        oray: { name: 'static.064', links: [
          ['tunnel.official.websiteSignup', 'https://hsk.oray.com/'],
          ['tunnel.official.console', 'https://console.hsk.oray.com/'],
          ['tunnel.official.download', 'https://hsk.oray.com/download'],
        ] },
        tailscale: { name: 'static.062', links: [
          ['tunnel.official.signup', 'https://login.tailscale.com/start'],
          ['tunnel.official.console', 'https://console.tailscale.com/'],
          ['tunnel.official.download', 'https://tailscale.com/download'],
          ['tunnel.official.guide', 'https://tailscale.com/docs/features/tailscale-funnel'],
        ] },
      };
      let tunnelOfficialKey = '';
      function renderTunnelOfficialLinks(mode) {
        const container = $('tunnelOfficialLinks');
        if (!container) return;
        const signature = `${getLocale()}:${mode}`;
        if (signature === tunnelOfficialKey) return;
        const service = tunnelOfficialEntries[mode] || tunnelOfficialEntries.quick;
        $('tunnelServiceName').textContent = t(service.name);
        container.replaceChildren(...service.links.map(([label, url]) => {
          const link = text('a', 'tunnel-official-link', t(label));
          link.href = url;
          link.target = '_blank';
          link.rel = 'noopener noreferrer';
          link.title = t('tunnel.official.newTab');
          return link;
        }));
        tunnelOfficialKey = signature;
      }
      let tunnelFieldsTouched = false;
      function renderTunnel() {
        if (!$('tunnelStatus')) return;
        const tunnel = state.tunnel || {};
        const active = tunnel.status === 'starting' || tunnel.status === 'online' ||
          tunnel.running || tunnel.cleanupAvailable;
        if (!tunnelFieldsTouched && !state.tunnelBusy) {
          $('tunnelMode').value = ['named', 'tailscale', 'ngrok', 'oray'].includes(tunnel.mode) ? tunnel.mode : 'quick';
          $('tunnelHostname').value = tunnel.configuredUrl || '';
          $('tunnelNgrokHostname').value = tunnel.ngrokConfiguredUrl || '';
          $('tunnelOrayHostname').value = tunnel.mode === 'oray' ? (tunnel.configuredUrl || '') : '';
        }
        const mode = $('tunnelMode').value;
        renderTunnelOfficialLinks(mode);
        const named = mode === 'named';
        $('tunnelNamedFields').hidden = !named;
        $('tunnelNgrokFields').hidden = mode !== 'ngrok';
        $('tunnelOrayFields').hidden = mode !== 'oray';
        $('tunnelModeHelp').textContent = mode === 'tailscale' ?
          t('app.177') :
          mode === 'ngrok' ? t('app.178') :
          mode === 'oray' ? t('app.179') :
          mode === 'named' ? t('app.180') :
            t('app.181');
        $('tunnelTokenStatus').textContent = tunnel.tokenConfigured ? t('app.182') : t('app.183');
        $('tunnelNgrokTokenStatus').textContent = tunnel.ngrokTokenConfigured ? t('app.184') : t('app.185');
        $('tunnelOrayKeyStatus').textContent = tunnel.apiKeyConfigured ? t('app.186') : t('app.187');
        for (const id of ['tunnelMode', 'tunnelHostname', 'tunnelToken', 'tunnelNgrokHostname', 'tunnelNgrokToken', 'tunnelOrayHostname', 'tunnelOrayKey', 'tunnelSaveButton']) {
          $(id).disabled = active || state.tunnelBusy || !!state.error;
        }
        $('tunnelStartButton').hidden = active;
        $('tunnelStopButton').hidden = !active;
        $('tunnelStopButton').textContent = mode === 'oray' ?
          (tunnel.mappingEnabled ? t('app.188') : t('app.189')) :
          tunnel.cleanupAvailable && !tunnel.running ? t('app.190') : t('app.191');
        $('tunnelStartButton').disabled = state.tunnelBusy || !!state.error || tunnelFieldsTouched;
        $('tunnelStopButton').disabled = state.tunnelBusy || !!state.error;
        const status = $('tunnelStatus');
        status.classList.toggle('open', tunnel.status === 'online');
        status.textContent = state.error ? t('app.192') :
          tunnelFieldsTouched && !active ? t('app.193') :
          tunnel.status === 'online' ? (tunnel.mode === 'named' ?
            t('app.194') :
            t('app.195')) :
          tunnel.status === 'starting' ? (tunnel.mode === 'tailscale' ?
            t('app.196') :
            tunnel.mode === 'ngrok' ? t('app.197') :
            tunnel.mode === 'oray' ? t('app.198') :
            t('app.199')) :
          tunnel.status === 'stopping' ? t('app.200') :
          tunnel.status === 'error' ? (tunnel.error || t('app.201')) :
          t('app.202');
        const address = typeof tunnel.url === 'string' && /^https:\/\/[a-z0-9.-]+$/i.test(tunnel.url) ? tunnel.url : '';
        $('tunnelAddressRow').hidden = !address;
        $('tunnelAddress').textContent = address;
        $('tunnelCopyButton').disabled = !address;
      }
      async function setTunnel(enabled) {
        if (state.tunnelBusy) return;
        state.tunnelBusy = true; renderTunnel();
        try {
          await post(enabled ? '/api/tunnel/start' : '/api/tunnel/stop');
          await refresh();
          showToast(enabled ? (state.tunnel?.status === 'online' ? t('app.203') :
            t('app.204')) :
            state.tunnel?.status === 'off' ? t('app.205') : t('app.206'));
        } catch (error) { showToast(error.message || t('app.207'), true); }
        finally { state.tunnelBusy = false; renderTunnel(); }
      }
      async function saveTunnelSettings() {
        if (state.tunnelBusy) return;
        const mode = $('tunnelMode').value;
        const url = $(mode === 'ngrok' ? 'tunnelNgrokHostname' : mode === 'oray' ? 'tunnelOrayHostname' : 'tunnelHostname').value.trim();
        const token = $(mode === 'ngrok' ? 'tunnelNgrokToken' : mode === 'oray' ? 'tunnelOrayKey' : 'tunnelToken').value.trim();
        state.tunnelBusy = true; renderTunnel();
        try {
          await post('/api/tunnel/settings', { mode, url, token });
          $('tunnelToken').value = '';
          $('tunnelNgrokToken').value = '';
          $('tunnelOrayKey').value = '';
          tunnelFieldsTouched = false;
          showToast(t('app.208'));
          await refresh();
        } catch (error) { showToast(error.message || t('app.209'), true); }
        finally { state.tunnelBusy = false; renderTunnel(); }
      }
      const providerCards = new Map();
      function renderProviders() {
        if (!$('providerGrid')) return;
        const grid = $('providerGrid');
        if (!state.providers.length) {
          providerCards.clear();
          grid.replaceChildren(text('span', 'provider-desc', state.error ? t('app.210') : state.loaded ? t('app.211') : t('app.212')));
          return;
        }
        const activeIds = new Set(state.providers.map((item) => item.id));
        for (const id of providerCards.keys()) if (!activeIds.has(id)) providerCards.delete(id);
        const cards = state.providers.map((item) => {
          let entry = providerCards.get(item.id);
          if (!entry) {
            const card = text('div', 'provider');
            const top = text('div', 'provider-top');
            const mark = text('span', 'provider-mark');
            const dot = text('span', 'status-dot');
            const name = text('span', 'provider-name');
            const description = text('span', 'provider-desc');
            top.append(mark, dot);
            card.append(top, name, description);
            entry = { card, mark, dot, name, description };
            providerCards.set(item.id, entry);
          }
          entry.mark.classList.toggle('off', !item.available);
          entry.dot.classList.toggle('on', Boolean(item.available));
          renderProviderIcon(entry.mark, item.icon);
          entry.name.textContent = item.label || item.id;
          entry.description.textContent = item.available ? (item.mode === 'dispatch_only' ? t('app.213') : t('app.214')) : (item.reason || t('app.215'));
          return entry.card;
        });
        if (cards.length !== grid.children.length || cards.some((card, index) => grid.children[index] !== card)) grid.replaceChildren(...cards);
      }
      function render() { renderSelects(); renderSessions(); renderJobs(); renderBoard(); renderPairing(); renderTunnel(); renderReceiver(); renderConnection(); renderModelSettings(); renderVoiceSettings(); renderProviders(); }
      function applyLanguage(locale) {
        const changed = setLocale(locale);
        if (!changed && document.documentElement.lang === locale) {
          if (!languageBusy) $('languageSelect').value = locale;
          return;
        }
        const active = document.activeElement;
        const draft = [...document.querySelectorAll('input[id], textarea[id], select[id]')]
          .filter((node) => node.id !== 'languageSelect')
          .map((node) => ({ id: node.id, value: node.value, checked: node.checked, start: node.selectionStart, end: node.selectionEnd }));
        applyStaticTranslations();
        refreshUiMessages();
        $('toast').classList.remove('visible');
        render();
        for (const field of draft) {
          const node = $(field.id);
          if (!node) continue;
          node.value = field.value;
          if (typeof field.checked === 'boolean') node.checked = field.checked;
          if (active?.id === field.id) {
            node.focus({ preventScroll: true });
            if (typeof field.start === 'number' && typeof field.end === 'number') {
              try { node.setSelectionRange(field.start, field.end); } catch { /* Some input types have no selection API. */ }
            }
          }
        }
        $('languageSelect').value = locale;
      }
      $('languageSelect').addEventListener('change', async (event) => {
        const requested = event.target.value;
        if (languageBusy || !supportedLocales.includes(requested)) return;
        languageBusy = true;
        languageEpoch++;
        event.target.disabled = true;
        try {
          const response = await post('/api/settings/language', { locale: requested });
          languageEpoch++;
          applyLanguage(supportedLocales.includes(response.locale) ? response.locale : requested);
          showToast(t('language.saved'));
        } catch (error) {
          $('languageSelect').value = getLocale();
          showToast(error.message || t('language.failed'), true);
        } finally {
          languageBusy = false;
          $('languageSelect').disabled = false;
          await refresh();
        }
      });
      $('refreshButton').addEventListener('click', refresh);
      $('pairingOpenButton')?.addEventListener('click', () => setPairingWindow(true));
      $('pairingCloseButton')?.addEventListener('click', () => setPairingWindow(false));
      $('pairingShowButton')?.addEventListener('click', () => { state.dismissedPairing.clear(); renderPairing(); });
      $('pairingConfirmButton').addEventListener('click', () => decidePairing('confirm'));
      $('pairingRejectButton').addEventListener('click', () => decidePairing('reject'));
      $('pairingDismissButton').addEventListener('click', () => { const request = currentPairingRequest(); if (request) state.dismissedPairing.add(request.nonce); renderPairing(); });
      $('receiverRevealPassword')?.addEventListener('click', () => { state.receiverPasswordVisible = !state.receiverPasswordVisible; renderReceiver(); });
      $('receiverCopySsid')?.addEventListener('click', () => copyText(state.receiver.ssid || '', t('app.216')));
      $('receiverCopyPassword')?.addEventListener('click', () => copyText(state.receiver.password || '', t('app.217')));
      $('receiverCopyAddress')?.addEventListener('click', () => copyText($('receiverAddress').textContent, t('app.218')));
      $('tunnelStartButton')?.addEventListener('click', () => setTunnel(true));
      $('tunnelStopButton')?.addEventListener('click', () => setTunnel(false));
      $('tunnelCopyButton')?.addEventListener('click', () => copyText($('tunnelAddress').textContent, t('app.219')));
      $('tunnelMode')?.addEventListener('change', () => { tunnelFieldsTouched = true; renderTunnel(); });
      $('tunnelHostname')?.addEventListener('input', () => { tunnelFieldsTouched = true; renderTunnel(); });
      $('tunnelToken')?.addEventListener('input', () => { tunnelFieldsTouched = true; renderTunnel(); });
      $('tunnelNgrokHostname')?.addEventListener('input', () => { tunnelFieldsTouched = true; renderTunnel(); });
      $('tunnelNgrokToken')?.addEventListener('input', () => { tunnelFieldsTouched = true; renderTunnel(); });
      $('tunnelOrayHostname')?.addEventListener('input', () => { tunnelFieldsTouched = true; renderTunnel(); });
      $('tunnelOrayKey')?.addEventListener('input', () => { tunnelFieldsTouched = true; renderTunnel(); });
      $('tunnelSettingsForm')?.addEventListener('submit', (event) => { event.preventDefault(); saveTunnelSettings(); });
      $('newSessionButton')?.addEventListener('click', () => {
        state.sessionFormOpen = !state.sessionFormOpen;
        renderSessions();
        if (state.sessionFormOpen) $('provider').focus();
      });
      $('legacySessionToggle')?.addEventListener('click', () => {
        state.legacySessionsOpen = !state.legacySessionsOpen;
        renderSessions();
      });
      $('sessionForm')?.addEventListener('submit', (event) => { event.preventDefault(); createSession(); });
      $('renameSessionButton')?.addEventListener('click', () => {
        const session = selectedSession();
        if (!session) return;
        state.renamingSession = true;
        $('renameSessionTitle').value = session.title || '';
        setUiMessage($('renameSessionMessage'), '');
        renderSessions();
        $('renameSessionTitle').focus();
      });
      $('renameSessionForm')?.addEventListener('submit', (event) => { event.preventDefault(); renameSession(); });
      $('renameSessionCancel')?.addEventListener('click', () => {
        state.renamingSession = false;
        setUiMessage($('renameSessionMessage'), '');
        renderSessions();
      });
      $('boardNewSession')?.addEventListener('click', () => {
        if (!providerById(state.previewProvider) || state.error) return;
        window.location.assign(`/tasks.html?newSession=1&provider=${encodeURIComponent(state.previewProvider)}`);
      });
      $('boardScreen')?.addEventListener('pointerdown', (event) => {
        if (event.target.closest('#boardNewSession')) return;
        boardPointerStart = {
          x: event.clientX, y: event.clientY,
          inAnswer: Boolean(event.target.closest('#boardResult')),
          inHeader: event.clientY - $('boardScreen').getBoundingClientRect().top < $('boardScreen').getBoundingClientRect().height * .53,
          answerScrollTop: $('boardResult').scrollTop,
        };
      });
      $('boardScreen')?.addEventListener('pointermove', (event) => {
        if (!boardPointerStart?.inAnswer) return;
        const dx = event.clientX - boardPointerStart.x, dy = event.clientY - boardPointerStart.y;
        if (Math.abs(dy) > Math.abs(dx)) $('boardResult').scrollTop = boardPointerStart.answerScrollTop - dy;
      });
      $('boardScreen')?.addEventListener('pointerup', (event) => {
        if (!boardPointerStart) return;
        const { inAnswer, inHeader } = boardPointerStart;
        const dx = event.clientX - boardPointerStart.x, dy = event.clientY - boardPointerStart.y;
        boardPointerStart = null;
        if (Math.abs(dx) >= 35 && Math.abs(dx) >= Math.abs(dy) * 1.25) changeBoardProvider(dx < 0 ? 1 : -1);
        else if (inHeader && !inAnswer && Math.abs(dy) >= 35 && Math.abs(dy) >= Math.abs(dx) * 1.25) changeBoardSession(dy < 0 ? 1 : -1);
      });
      $('boardScreen')?.addEventListener('pointercancel', () => { boardPointerStart = null; });
      $('boardScreen')?.addEventListener('keydown', (event) => {
        if (!['ArrowLeft', 'ArrowRight', 'ArrowUp', 'ArrowDown'].includes(event.key)) return;
        event.preventDefault();
        if (event.key === 'ArrowLeft' || event.key === 'ArrowRight') changeBoardProvider(event.key === 'ArrowRight' ? 1 : -1);
        else changeBoardSession(event.key === 'ArrowDown' ? 1 : -1);
      });
      $('modelSettingsForm')?.addEventListener('submit', (event) => { event.preventDefault(); saveModels(); });
      $('provider')?.addEventListener('change', (event) => {
        if (state.providers.some((item) => item.id === event.target.value)) { state.previewProvider = event.target.value; state.previewSessionId = null; renderBoard(); renderSelects(); }
      });
      $('voiceSettingsForm')?.addEventListener('submit', (event) => { event.preventDefault(); saveVoiceSettings(); });
      for (const id of ['voiceEndpoint', 'voiceModel', 'openaiKey']) $(id)?.addEventListener('input', () => { voiceFieldsTouched = true; });
      document.querySelectorAll('[data-voice-preset]').forEach((button) => button.addEventListener('click', () => {
        const siliconflow = button.dataset.voicePreset === 'siliconflow';
        $('voiceEndpoint').value = siliconflow ? 'https://api.siliconflow.cn/v1/audio/transcriptions' : 'https://api.openai.com/v1/audio/transcriptions';
        $('voiceModel').value = siliconflow ? 'FunAudioLLM/SenseVoiceSmall' : 'gpt-transcribe';
        voiceFieldsTouched = true;
        setVoiceSettingsMessage(m('app.220'));
      }));
      $('clearOpenaiKey')?.addEventListener('click', () => saveVoiceSettings(true));
      $('jobForm')?.addEventListener('submit', async (event) => {
        event.preventDefault();
        const instruction = $('instruction').value.trim(), session = selectedSession();
        if (!instruction || !session) { setFormMessage(m('app.221')); return; }
        if (state.busy.has('new-turn')) return;
        state.busy.add('new-turn'); renderSelects(); setFormMessage(m('app.222'), true);
        try {
          await post('/api/jobs', { sessionId: session.id, instruction });
          state.previewProvider = session.provider;
          state.previewSessionId = session.id;
          $('instruction').value = '';
          setFormMessage(m('app.223'), true);
          showToast(t('app.224'));
          await refresh();
        }
        catch (error) { setFormMessage(error.message || m('app.225')); showToast(error.message || t('app.225'), true); }
        finally { state.busy.delete('new-turn'); render(); }
      });
      applyStaticTranslations();
      $('languageSelect').value = getLocale();
      render(); refresh(); setInterval(refresh, 4000);
    })();

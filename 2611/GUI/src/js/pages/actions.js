// Eylemler sekmesi: eylem kartları (dokununca aç/kapa, anında kaydedilir) ve ayar görünümleri:
// Telegram, erken uyarı postası, webhook + MQTT, röle. Posta grupları groups.js'te.
import { api } from '../api.js';
import { Section, saveAll } from '../form.js';
import { t } from '../i18n.js';
import { openSub, registerTab } from '../nav.js';
import { $$, byId, errText, runJob, show, toast } from '../ui.js';
import { openGroups } from './groups.js';

const actions = new Section('actions', {
  early_mail: 'x', early_telegram: 'x', early_webhook: 'x', early_mqtt: 'x',
  trig_mail: 'x', trig_webhook: 'x', trig_mqtt: 'x', trig_relay: 'x',
});
// kartlar Section alanı değil; değerler actions.data üzerinden yürür
for (const k in actions.fields) actions.fields[k] = { get: () => actions.data[k], set: () => {} };

const CARDS = {
  'early-telegram': ['early_telegram'], 'early-mail': ['early_mail'], 'early-api': ['early_webhook', 'early_mqtt'],
  'trigger-mail': ['trig_mail'], 'trigger-api': ['trig_webhook', 'trig_mqtt'], 'trigger-relay': ['trig_relay'],
};

const telegram = new Section('telegram', { token: 'telegramToken', chat_id: 'telegramChat', message: 'telegramMessage' });
const earlyMail = new Section('early_mail', { email: 'earlyMailRecipient', subject: 'earlyMailSubject', body: 'earlyReminderMessage' });
const webhook = new Section('webhook', { url: 'webhookUrl', method: 'webhookMethod', headers: 'webhookHeaders', body: 'webhookBody' });
const mqtt = new Section('mqtt', { url: 'mqttBroker', topic: 'mqttTopic', user: 'mqttUser', pass: 'mqttPass', payload: 'mqttPayload' });

/* ---------- röle ---------- */

function ensureOption(sel, v) {
  if (![...sel.options].some(o => +o.value === v)) sel.add(new Option(v + ' s', v));
  sel.value = v;
}

const relay = new Section('relay', {
  inverted: 'relayInverted',
  delay_s: { get: () => +byId('relayDelay').value, set: v => ensureOption(byId('relayDelay'), v || 0) },
  pulse_s: {
    get: () => (byId('relayPulseEnabled').checked ? +byId('relayPulseDuration').value : 0),
    set: v => { byId('relayPulseEnabled').checked = v > 0; if (v > 0) ensureOption(byId('relayPulseDuration'), v); },
  },
  duration_s: {
    get: () => {
      const u = byId('relayDurationUnit').value, n = Math.max(1, +byId('relayDurationValue').value || 1);
      return u === 'none' ? 0 : u === 'minutes' ? Math.min(60, n) * 60 : Math.min(59, n);
    },
    set: v => {
      const u = !v ? 'none' : v % 60 === 0 ? 'minutes' : 'seconds';
      byId('relayDurationUnit').value = u;
      byId('relayDurationValue').value = !v ? 1 : u === 'minutes' ? v / 60 : v;
    },
  },
});

function relayUi() {
  const inv = byId('relayInverted').checked;
  byId('relayIdleState').textContent = t(inv ? 'relay.power_on' : 'relay.power_off');
  byId('relayActiveState').textContent = t(inv ? 'relay.power_off' : 'relay.power_on');
  const unlimited = byId('relayDurationUnit').value === 'none';
  byId('relayDurationValue').disabled = unlimited;
  const pulse = byId('relayPulseEnabled').checked;
  show(byId('relayPulseFields'), pulse);
  const total = relay.read().duration_s, p = +byId('relayPulseDuration').value;
  show(byId('relayCycleInfo'), pulse && total > 0);
  if (pulse && total > 0) {
    byId('relayCycleText').textContent = t('relay.cycle_text', { pulse: p, cycles: Math.max(1, Math.floor(total / (2 * p))), total });
  }
}

/* ---------- kartlar ---------- */

function renderCards() {
  $$('.action-card').forEach(c => {
    const keys = CARDS[c.dataset.action];
    c.dataset.active = String(keys.some(k => actions.data[k]));
  });
}

async function toggleCard(card) {
  const keys = CARDS[card.dataset.action], on = card.dataset.active !== 'true';
  const body = {};
  if (keys.length === 2) {                 // API kartı: yalnız ayarlı kanalları aç; hiçbiri ayarlı değilse ikisini de
    const has = [!!webhook.data.url, !!mqtt.data.url];
    keys.forEach((k, i) => { body[k] = on && (has[i] || !(has[0] || has[1])); });
  } else {
    body[keys[0]] = on;
  }
  try {
    actions.set(await api('PUT', '/api/config/actions', body));
    renderCards();
  } catch (e) {
    toast(errText(e), 'err');
  }
}

/* ---------- testler: önce kaydet, sonra iş ---------- */

const poll = id => api('GET', '/api/jobs/' + id);

// başlıklar boş ya da JSON nesnesi olmalı (ch_http.c cJSON ile ayrıştırır)
function checkHeaders() {
  const v = byId('webhookHeaders').value.trim();
  if (!v) return;
  let o = null;
  try { o = JSON.parse(v); } catch (e) { /* aşağıda */ }
  if (!o || typeof o !== 'object' || Array.isArray(o)) throw new Error(t('api.headers_invalid'));
}
const saveApi = () => { checkHeaders(); return saveAll([webhook, mqtt]); };
const test = (sec, ch, statusId) => () => runJob(async () => {
  await saveAll(Array.isArray(sec) ? sec : [sec]);
  return api('POST', '/api/test/' + ch);
}, byId(statusId), poll);

export function initActions() {
  $$('.action-card-main').forEach(m => {
    m.setAttribute('role', 'button');
    m.tabIndex = 0;
    m.onclick = () => toggleCard(m.parentElement);
    m.onkeydown = e => { if (e.key === 'Enter' || e.key === ' ') { e.preventDefault(); m.click(); } };
  });
  const views = {
    telegram: ['telegramConfigView', [telegram]],
    'early-mail': ['earlyMailConfigView', [earlyMail]],
    'early-api': ['apiConfigView', [webhook, mqtt]],
    'trigger-api': ['apiConfigView', [webhook, mqtt]],
    relay: ['relayConfigView', [relay]],
  };
  $$('.action-card-settings').forEach(b => {
    b.onclick = async e => {
      e.stopPropagation();
      const cfg = b.dataset.config;
      if (cfg === 'trigger-mail') { openGroups(); return; }
      const [view, secs] = views[cfg];
      try {
        for (const s of secs) await s.load();
      } catch (err) { toast(errText(err), 'err'); return; }
      if (cfg === 'relay') relayUi();
      $$('.smtp-status', byId(view)).forEach(el => { el.textContent = ''; el.className = 'smtp-status'; });
      openSub(view, { save: view === 'apiConfigView' ? saveApi : () => saveAll(secs), back: () => actions.load().then(renderCards).catch(() => {}) });
    };
  });
  ['relayInverted', 'relayDurationUnit', 'relayDurationValue', 'relayPulseEnabled', 'relayPulseDuration']
    .forEach(id => byId(id).addEventListener('input', relayUi));
  byId('relayDurationUnit').addEventListener('change', relayUi);
  byId('testTelegramBtn').onclick = test(telegram, 'telegram', 'telegramStatus');
  byId('testWebhookBtn').onclick = () => { try { checkHeaders(); } catch (e) { toast(e.message, 'err'); return; } test(webhook, 'webhook', 'webhookStatus')(); };
  byId('testMqttBtn').onclick = test(mqtt, 'mqtt', 'mqttStatus');
  byId('testRelayBtn').onclick = async () => {
    try {
      await relay.save();
      await api('POST', '/api/test/relay');
      toast(t('alert.relay_test_ok'));
    } catch (e) { toast(errText(e), 'err'); }
  };
  window.addEventListener('ls-lang', relayUi);
  registerTab('panel-actions', {
    show: async () => {
      await actions.load();
      await webhook.load();
      await mqtt.load();
      renderCards();
    },
  });
}

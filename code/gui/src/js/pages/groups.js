// Tetikleme posta grupları: liste → ayrıntı (ad, konu, içerik, alıcılar, ekler). En çok 10 grup.
// Ekler ham gövdeyle PUT /api/groups/{id}/files/{ad} (≤ 2 MB, grup başına ≤ 5 dosya / 4 MB).
import { ApiError, api } from '../api.js';
import { t } from '../i18n.js';
import { closeSub, openSub } from '../nav.js';
import { ask, byId, errText, fmtKB, h, removeBtn, toast } from '../ui.js';

const MAX_GROUPS = 10, FILE_MAX = 2 * 1024 * 1024;
let groups = [];
let gid = -1;                 // açık grup
let saved = false;            // açık grup sunucuda var mı
let rcpts = [];

async function loadList() {
  groups = await api('GET', '/api/groups');
  const box = byId('mailGroupList');
  box.replaceChildren(...groups.map(g => h('div', { class: 'mail-group-item' },
    h('div', { class: 'mail-group-info' },
      h('span', { class: 'mail-group-name' }, g.name || t('mail_group.unnamed')),
      h('span', { class: 'mail-group-count' }, t('mail_group.recipient_count', { count: g.recipients }))),
    h('button', { class: 'btn-cancel', type: 'button', style: 'width:auto;margin-top:0;padding:6px 12px',
      onclick: () => openDetail(g.id) }, t('mail_group.view')))));
  byId('createMailGroup').disabled = groups.length >= MAX_GROUPS;
}

export async function openGroups() {
  try { await loadList(); } catch (e) { toast(errText(e), 'err'); return; }
  openSub('triggerMailConfigView');
}

function renderRcpts() {
  byId('mailGroupRecipients').replaceChildren(...rcpts.map((r, i) => h('div', { class: 'recipient-item' },
    h('span', { class: 'recipient-email' }, r),
    removeBtn(t('mail_group.remove'), () => { rcpts.splice(i, 1); renderRcpts(); }))));
}

async function loadFiles() {
  const box = byId('mailGroupFileList');
  if (!saved) { box.replaceChildren(); return; }
  try {
    const d = await api('GET', '/api/groups/' + gid + '/files');
    box.replaceChildren(...d.files.map(f => h('div', { class: 'recipient-item' },
      h('span', { class: 'recipient-email', style: 'flex:1' }, f.name),
      h('span', { class: 'file-size' }, fmtKB(f.size)),
      removeBtn(t('mail_group.delete'), () => delFile(f.name)))));
  } catch (e) {
    box.replaceChildren(h('p', { class: 'field-hint warn' }, errText(e)));
  }
}

async function openDetail(id) {
  gid = id;
  saved = id >= 0;
  let g = { name: '', subject: '', body: '', recipients: '' };
  if (saved) {
    try { g = await api('GET', '/api/groups/' + id); } catch (e) { toast(errText(e), 'err'); return; }
  }
  byId('mailGroupName').value = g.name;
  byId('mailGroupSubject').value = g.subject;
  byId('mailGroupContent').value = g.body;
  rcpts = g.recipients.split(/[,; ]+/).filter(Boolean);
  byId('newRecipientEmail').value = '';
  renderRcpts();
  byId('mailGroupDeleteBtn').hidden = !saved;
  openSub('mailGroupDetailView', { back: () => loadList().catch(() => {}) });
  loadFiles();
}

function addRcpt() {
  const el = byId('newRecipientEmail'), v = el.value.trim();
  if (!/^[^@\s,;]+@[^@\s,;]+\.[^@\s,;]+$/.test(v)) { toast(t('alert.email_invalid'), 'err'); return; }
  if (!rcpts.includes(v)) rcpts.push(v);
  el.value = '';
  renderRcpts();
}

async function saveGroup(stay) {
  const name = byId('mailGroupName').value.trim();
  if (!name) { toast(t('alert.group_name_required'), 'err'); return false; }
  if (gid < 0) {                                     // ilk boş yuva
    const used = new Set(groups.map(g => g.id));
    gid = [...Array(MAX_GROUPS).keys()].find(i => !used.has(i));
    if (gid === undefined) { gid = -1; return false; }
  }
  try {
    await api('PUT', '/api/groups/' + gid, {
      name, subject: byId('mailGroupSubject').value.trim(), body: byId('mailGroupContent').value,
      recipients: rcpts.join(','),
    });
  } catch (e) {
    if (!saved) gid = -1;
    toast(errText(e), 'err');
    return false;
  }
  saved = true;
  byId('mailGroupDeleteBtn').hidden = false;
  toast(t('common.saved'));
  if (!stay) closeSub();
  return true;
}

// sunucu kuralı: [A-Za-z0-9._- ] ve ≤ 60 karakter (uzantı korunur)
function safeName(n) {
  n = n.normalize('NFKD').replace(/[̀-ͯ]/g, '').replace(/[^A-Za-z0-9._ -]/g, '_');
  if (n.length > 60) { const dot = n.lastIndexOf('.'); const ext = dot > 0 ? n.slice(dot, dot + 10) : ''; n = n.slice(0, 60 - ext.length) + ext; }
  return n || 'file';
}

async function upload(files) {
  if (!saved && !(await saveGroup(true))) return;
  for (const f of files) {
    if (f.size > FILE_MAX) { toast(t('alert.file_too_large', { name: f.name }), 'err'); continue; }
    try {
      const r = await fetch('/api/groups/' + gid + '/files/' + encodeURIComponent(safeName(f.name)), {
        method: 'PUT', body: f, credentials: 'same-origin', signal: AbortSignal.timeout(120000),
        headers: { 'X-LS-Request': '1', 'Content-Type': 'application/octet-stream' },
      });
      const j = await r.json().catch(() => null);
      if (!r.ok || !j || !j.ok) {
        const e = (j && j.error) || {};
        throw new ApiError(r.status, e.code || 'internal', e.message);
      }
    } catch (e) {
      toast(t('alert.file_upload_fail', { msg: errText(e) }), 'err');
    }
  }
  loadFiles();
}

async function delFile(name) {
  try { await api('DELETE', '/api/groups/' + gid + '/files/' + encodeURIComponent(name)); }
  catch (e) { toast(t('alert.delete_error', { msg: errText(e) }), 'err'); }
  loadFiles();
}

export function initGroups() {
  byId('createMailGroup').onclick = () => openDetail(-1);
  byId('addRecipientBtn').onclick = addRcpt;
  byId('newRecipientEmail').onkeydown = e => { if (e.key === 'Enter') { e.preventDefault(); addRcpt(); } };
  byId('mailGroupSaveBtn').onclick = () => saveGroup(false);
  byId('mailGroupCancelBtn').onclick = closeSub;
  byId('mailGroupDeleteBtn').onclick = async () => {
    if (!(await ask(t('confirm.delete_group'), { ok: t('mail_group.delete'), danger: true }))) return;
    try { await api('DELETE', '/api/groups/' + gid); closeSub(); }
    catch (e) { toast(t('alert.group_delete_err') + ': ' + errText(e), 'err'); }
  };
  byId('mailGroupFileBtn').onclick = () => byId('mailGroupFile').click();
  byId('mailGroupFile').onchange = e => { upload([...e.target.files]); e.target.value = ''; };
}

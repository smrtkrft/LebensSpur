// Ayarlar penceresi: aç/kapa, carousel sekmeleri, akordeonlar, alt sayfa yığını, alt bilgi düğmeleri.
// Her sekme registerTab ile { show(), save() } verir; alt sayfalar openSub ile { save(), back() } verir.
import { t } from './i18n.js';
import { $$, byId, errText, show, toast } from './ui.js';

const tabs = {};
let cur = 0;
const stack = [];                    // [{ id, main, save, back }]
let loaded = new Set();              // bu açılışta yüklenen sekmeler

export function registerTab(panelId, h) { tabs[panelId] = h; }
export const isOpen = () => byId('popup').classList.contains('show');

const panels = () => $$('.settings-panel');
const navItems = () => $$('.nav-item');

function footer() {
  const top = stack[stack.length - 1];
  byId('footerCancelBtn').textContent = t(stack.length ? 'footer.back' : 'footer.cancel');
  const h = top || tabs[panels()[cur].id];
  byId('footerSaveBtn').hidden = !(h && h.save);
  byId('settingsCarousel').style.display = stack.length ? 'none' : '';
}

async function enterTab() {
  const id = panels()[cur].id, h = tabs[id];
  if (h && h.show && !loaded.has(id)) {
    loaded.add(id);
    try { await h.show(); } catch (e) { loaded.delete(id); toast(errText(e), 'err'); }
  }
}

function setTab(i) {
  while (stack.length) closeSub();
  cur = (i + panels().length) % panels().length;
  navItems().forEach((n, k) => n.classList.toggle('active', k === cur));
  panels().forEach((p, k) => p.classList.toggle('active', k === cur));
  const items = byId('carouselItems'), el = navItems()[cur];
  const w = items.parentElement.offsetWidth || 240;
  items.style.transform = 'translateX(' + (w / 2 - (el.offsetLeft + el.offsetWidth / 2)) + 'px)';
  footer();
  enterTab();
}

export function openPopup() {
  loaded = new Set();
  byId('overlay').classList.add('show');
  byId('popup').classList.add('show');
  setTab(cur);
}

export function closePopup() {
  while (stack.length) closeSub();
  byId('overlay').classList.remove('show');
  byId('popup').classList.remove('show');
}

// Alt sayfa: aynı paneldeki ana içerik (…MainContent) gizlenir, görünüm gösterilir.
export function openSub(id, h = {}) {
  const view = byId(id), panel = view.closest('.settings-panel');
  const main = panel.querySelector('[id$="MainContent"]');
  const prev = stack[stack.length - 1];
  show(prev ? byId(prev.id) : main, false);
  show(view, true);
  stack.push({ id, main, save: h.save, back: h.back });
  footer();
  byId('popup').querySelector('.modal-body').scrollTop = 0;
}

export function closeSub() {
  const top = stack.pop();
  if (!top) return;
  show(byId(top.id), false);
  const prev = stack[stack.length - 1];
  show(prev ? byId(prev.id) : top.main, true);
  if (top.back) top.back();
  footer();
}

async function onSave() {
  const top = stack[stack.length - 1];
  const h = top || tabs[panels()[cur].id];
  if (!isOpen() || !h || !h.save || (!top && !loaded.has(panels()[cur].id))) return;   // sekme yüklenmeden kaydetme
  const btn = byId('footerSaveBtn');
  btn.disabled = true;
  try {
    const changed = await h.save();
    toast(t(changed === false ? 'common.no_change' : 'common.saved'));
    if (top) closeSub(); else closePopup();
  } catch (e) {
    toast(errText(e), 'err');
  } finally {
    btn.disabled = false;
  }
}

export function initNav() {
  byId('settingsBtn').onclick = openPopup;
  byId('overlay').onclick = closePopup;
  byId('carouselPrev').onclick = () => setTab(cur - 1);
  byId('carouselNext').onclick = () => setTab(cur + 1);
  navItems().forEach((n, k) => { n.onclick = () => setTab(k); });
  $$('.accordion-header').forEach(h => {
    h.setAttribute('role', 'button');
    h.tabIndex = 0;
    h.onclick = () => h.parentElement.classList.toggle('open');
    h.onkeydown = e => { if (e.key === 'Enter' || e.key === ' ') { e.preventDefault(); h.click(); } };
  });
  byId('footerCancelBtn').onclick = () => (stack.length ? closeSub() : closePopup());
  byId('footerSaveBtn').onclick = onSave;
  document.addEventListener('keydown', e => {
    if (e.key === 'Escape' && isOpen() && !byId('askDlg').open) (stack.length ? closeSub() : closePopup());
  });
  window.addEventListener('ls-lang', footer);
}

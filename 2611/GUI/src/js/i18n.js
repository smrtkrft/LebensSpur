// i18n: derlemede doğrulanmış keys.json + dil başına değer dizisi. HTML'deki İngilizce metin yedektir.
const LANGS = ['tr', 'en', 'de', 'es', 'fr', 'it'];
let dict = {};
let keysP = null;

export function currentLang() {
  const saved = localStorage.getItem('ls-lang');
  if (LANGS.includes(saved)) return saved;
  const nav = (navigator.language || 'en').slice(0, 2);
  return LANGS.includes(nav) ? nav : 'en';
}

export async function loadLang(lang) {
  const base = '/s/' + document.body.dataset.hash + '/lang/';
  if (!keysP) keysP = fetch(base + 'keys.json').then(r => { if (!r.ok) throw new Error(r.status); return r.json(); });
  let keys, vals;
  try {
    [keys, vals] = await Promise.all([keysP, fetch(base + lang + '.json').then(r => r.json())]);
  } catch (e) {
    keysP = null;
    throw e;
  }
  dict = {};
  keys.forEach((k, i) => { dict[k] = vals[i]; });
  localStorage.setItem('ls-lang', lang);
  document.documentElement.lang = lang;
  apply(document);
  window.dispatchEvent(new Event('ls-lang'));
}

export const has = key => dict[key] !== undefined;

export function t(key, params) {
  let s = dict[key];
  if (s === undefined) return key;
  if (params) for (const p in params) s = s.replaceAll('{' + p + '}', String(params[p]));
  return s;
}

export function apply(root) {
  if (!Object.keys(dict).length) return;
  root.querySelectorAll('[data-i18n]').forEach(el => { el.textContent = t(el.dataset.i18n); });
  root.querySelectorAll('[data-i18n-ph]').forEach(el => { el.placeholder = t(el.dataset.i18nPh); });
  root.querySelectorAll('[data-i18n-title]').forEach(el => {
    el.title = t(el.dataset.i18nTitle);
    el.setAttribute('aria-label', el.title);
  });
}

export { LANGS };

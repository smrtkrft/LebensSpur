// Yapılandırma bölümü ↔ form bağı. Yüklemede taban alınır; kaydette yalnız değişen alanlar PUT edilir.
// Gizli alanlar sunucudan "***" gelir: kutu boş kalır, yer tutucu "kayıtlı" der; boş bırakılırsa gönderilmez.
import { api } from './api.js';
import { t } from './i18n.js';
import { byId } from './ui.js';

const SECRET = '***';

// kayıtlı gizli alanın yanında "Kaldır": boş kutu "koru" demek olduğundan silmenin tek yolu budur
function clearBtn(el) {
  let b = el.nextElementSibling;
  if (!b || !b.classList.contains('secret-clear')) {
    b = document.createElement('button');
    b.type = 'button';
    b.className = 'btn-cancel secret-clear';
    b.onclick = () => {
      el.dataset.clear = '1';
      el.value = '';
      el.placeholder = t('common.secret_cleared');
      b.hidden = true;
    };
    el.after(b);
  }
  b.textContent = t('mail_group.remove');
  b.hidden = !el.dataset.secret;
}

export class Section {
  // fields: { anahtar: 'elemanId' | { get(), set(v, data) } }
  constructor(name, fields) {
    this.name = name;
    this.fields = fields;
    this.base = null;
    this.data = {};
  }

  el(k) { return typeof this.fields[k] === 'string' ? byId(this.fields[k]) : null; }

  set(d) {
    this.data = d;
    for (const k in this.fields) {
      const el = this.el(k), v = d[k];
      if (!el) { this.fields[k].set(v, d); continue; }
      if (el.type === 'checkbox') { el.checked = !!v; continue; }
      if (el.dataset.ph0 === undefined) el.dataset.ph0 = el.placeholder;
      delete el.dataset.clear;
      if (v === SECRET) { el.value = ''; el.placeholder = t('common.secret_set'); el.dataset.secret = '1'; }
      else {
        el.value = v === undefined ? '' : v;
        if (el.dataset.secret) { delete el.dataset.secret; el.placeholder = el.dataset.ph0; }
      }
      clearBtn(el);
    }
    this.base = this.read();
  }

  read() {
    const o = {};
    for (const k in this.fields) {
      const el = this.el(k);
      if (!el) { o[k] = this.fields[k].get(); continue; }
      o[k] = el.type === 'checkbox' ? el.checked
        : el.type === 'number' || el.type === 'range' ? +el.value : el.value.trim();
    }
    return o;
  }

  // diff(): kutu boşsa ve sır kayıtlıysa alan gönderilmez; "Kaldır"a basıldıysa null gider
  diff() {
    if (!this.base) return {};                                   // hiç yüklenmedi: varsayılanları yazma
    const now = this.read(), o = {};
    for (const k in now) {
      const el = this.el(k);
      if (el && el.dataset.clear) { o[k] = null; continue; }      // "Kaldır": kutu boş olsa da silme gönderilir
      if (JSON.stringify(now[k]) === JSON.stringify(this.base[k])) continue;
      if (el && el.dataset.secret && now[k] === '') continue;      // kayıtlı sır korunur
      o[k] = now[k];
    }
    return o;
  }

  dirty() { return Object.keys(this.diff()).length > 0; }

  async load() { this.set(await api('GET', '/api/config/' + this.name)); return this; }

  // değişiklik yoksa false; varsa PUT eder, yanıt yapılandırmaysa formu tazeler
  async save() {
    const ch = this.diff();
    if (!Object.keys(ch).length) return false;
    const r = await api('PUT', '/api/config/' + this.name, ch);
    if (r && typeof r === 'object' && Object.keys(this.fields).some(k => k in r)) this.set(r);
    else this.base = this.read();
    return r || true;
  }
}

// bölümleri sırayla kaydet (aynı anda tek flash yazımı); hiçbiri değişmediyse false
export async function saveAll(sections) {
  let any = false;
  for (const s of sections) if (await s.save()) any = true;
  return any;
}

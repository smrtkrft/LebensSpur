// API sarmalayıcısı: zarfı çözer, X-LS-Request ekler, 401'de oturum olayı yayar.
export class ApiError extends Error {
  constructor(status, code, message, field) {
    super(message || code);
    this.status = status; this.code = code; this.field = field;
  }
}

export async function api(method, path, body) {
  const opt = { method, headers: {}, credentials: 'same-origin' };
  if (method !== 'GET') opt.headers['X-LS-Request'] = '1';
  if (body !== undefined) {
    opt.headers['Content-Type'] = 'application/json';
    opt.body = JSON.stringify(body);
  }
  let res;
  try {
    res = await fetch(path, opt);
  } catch (e) {
    throw new ApiError(0, 'offline', '');
  }
  let j = null;
  try { j = await res.json(); } catch (e) { /* gövdesiz yanıt */ }
  if (res.status === 401 && j && j.error && j.error.code === 'auth_required') {
    window.dispatchEvent(new Event('ls-auth-required'));
  }
  if (!res.ok || !j || !j.ok) {
    const e = (j && j.error) || {};
    throw new ApiError(res.status, e.code || 'internal', e.message || '', e.field);
  }
  return j.data;
}

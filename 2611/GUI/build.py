#!/usr/bin/env python3
"""
LebensSpur 2611 - web arayuzu derleyicisi.

Kullanim:
  build.py --src 2611/GUI/src --out <build>/gui --ver 1.0.0 --schema <build>/gen/cfg/schema.gen.json

Adimlar: esbuild (JS bundle+minify, CSS minify) -> HTML minify + sprite + surum/hash ->
i18n dogrulama ve donusturme (keys.json + deger dizileri) -> gzip -9 (mtime=0) -> butce denetimi.
Python 3.9 + stdlib (IDF python ortami). Node yalniz esbuild icin (npx, once cevrimdisi).

Cikti adlari SABIT (CMake OUTPUT ile eslesir); onbellek kirma icin icerik ozeti URL yolunda:
/s/<hash>/app.js. Ozet gui_hash.txt dosyasina yazilir.
"""
import argparse
import gzip
import hashlib
import json
import os
import re
import subprocess
import sys

ESBUILD = "esbuild@0.28.2"
LANGS = ["tr", "en", "de", "es", "fr", "it"]
PLACEHOLDER = re.compile(r"\{[a-z_]+\}")


def fail(msg):
    sys.exit(f"build.py: HATA: {msg}")


def esbuild(args):
    base = ["npx", "--yes"]
    for extra in (["--offline"], []):   # once onbellekten; yoksa agdan bir kez indir
        cmd = base + extra + [ESBUILD] + args
        r = subprocess.run(cmd, capture_output=True, text=True)
        if r.returncode == 0:
            return
        if "ENOTCACHED" not in r.stderr and extra:
            break
    fail("esbuild calismadi (Node/npx kurulu mu?):\n" + r.stderr[-800:])


def minify_html(s):
    s = re.sub(r"<!--(?!SPRITE).*?-->", "", s, flags=re.S)
    s = re.sub(r">\s+<", "><", s)
    s = re.sub(r"\s{2,}", " ", s)
    return s.strip()


def sprite(icon_dir):
    out = []
    for name in sorted(os.listdir(icon_dir)):
        if not name.endswith(".svg"):
            continue
        with open(os.path.join(icon_dir, name), encoding="utf-8") as fh:
            svg = fh.read()
        m = re.search(r"<svg([^>]*)>(.*)</svg>", svg, flags=re.S)
        if not m:
            fail(f"gecersiz SVG: {name}")
        attrs, body = m.group(1), m.group(2)
        vb = re.search(r'viewBox="([^"]+)"', attrs)
        keep = " ".join(a for a in re.findall(r'(?:fill|stroke|stroke-width|stroke-linecap|stroke-linejoin)="[^"]*"', attrs))
        out.append(f'<symbol id="i-{name[:-4]}" viewBox="{vb.group(1) if vb else "0 0 24 24"}" {keep}>{body.strip()}</symbol>')
    return '<svg xmlns="http://www.w3.org/2000/svg" style="display:none">' + "".join(out) + "</svg>"


def load_langs(lang_dir):
    data = {}
    for lg in LANGS:
        p = os.path.join(lang_dir, f"{lg}.json")
        if not os.path.exists(p):
            fail(f"dil dosyasi yok: {p}")
        with open(p, encoding="utf-8") as fh:
            d = json.load(fh)
        if not all(isinstance(v, str) for v in d.values()):
            fail(f"{lg}.json duz anahtar->metin olmali")
        data[lg] = d
    return data


def check_langs(data, used_keys):
    ref = data["en"]
    errors = []
    for lg, d in data.items():
        if set(d) != set(ref):
            miss = sorted(set(ref) - set(d))[:5]
            extra = sorted(set(d) - set(ref))[:5]
            errors.append(f"{lg}: anahtar kumesi farkli (eksik {miss}, fazla {extra})")
            continue
        for k, v in d.items():
            if not v.strip():
                errors.append(f"{lg}.{k}: bos metin")
            if set(PLACEHOLDER.findall(v)) != set(PLACEHOLDER.findall(ref[k])):
                errors.append(f"{lg}.{k}: yer tutucular en.json ile farkli")
    for k in sorted(used_keys - set(ref)):
        errors.append(f"kullanilan anahtar dil dosyasinda yok: {k}")
    if errors:
        fail("i18n dogrulama:\n  " + "\n  ".join(errors[:30]))
    return sorted(ref)


def used_i18n_keys(html, js):
    keys = set(re.findall(r'''data-i18n(?:-[a-z]+)?=["']([^"']+)["']''', html))
    # yalnız tam sabit anahtarlar: t('a.b', …) / t('a.b'); t('state.' + x) gibi dinamikler hariç
    keys |= set(re.findall(r"""\bt\(\s*['"]([a-z0-9_]+(?:\.[a-z0-9_]+)+)['"]\s*[,)]""", js))
    return keys


def gz(data):
    return gzip.compress(data, compresslevel=9, mtime=0)


def write(path, data):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    if os.path.exists(path):
        with open(path, "rb") as fh:
            if fh.read() == data:
                return
    with open(path, "wb") as fh:
        fh.write(data)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--src", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--ver", required=True)
    ap.add_argument("--schema", required=True)
    ap.add_argument("--budget", default=os.path.join(os.path.dirname(os.path.abspath(__file__)), "budget.json"))
    a = ap.parse_args()

    tmp = os.path.join(a.out, "tmp")
    os.makedirs(tmp, exist_ok=True)
    esbuild([os.path.join(a.src, "js", "main.js"), "--bundle", "--minify", "--format=iife",
             "--target=es2020", "--legal-comments=none", f"--outfile={os.path.join(tmp, 'app.js')}"])
    esbuild([os.path.join(a.src, "css", "app.css"), "--minify", "--target=chrome90,safari14,firefox90",
             f"--outfile={os.path.join(tmp, 'app.css')}"])
    with open(os.path.join(tmp, "app.js"), "rb") as fh:
        js = fh.read()
    with open(os.path.join(tmp, "app.css"), "rb") as fh:
        css = fh.read()
    with open(a.schema, "rb") as fh:
        schema = fh.read()

    langs = load_langs(os.path.join(a.src, "lang"))
    with open(os.path.join(a.src, "index.html"), encoding="utf-8") as fh:
        html_src = fh.read()
    js_src = ""
    for root, _, files in os.walk(os.path.join(a.src, "js")):
        for f in files:
            if f.endswith(".js"):
                with open(os.path.join(root, f), encoding="utf-8") as fh:
                    js_src += fh.read()
    keys = check_langs(langs, used_i18n_keys(html_src, js_src))
    lang_files = {"keys.json": json.dumps(keys, separators=(",", ":"), ensure_ascii=False).encode()}
    for lg in LANGS:
        lang_files[f"{lg}.json"] = json.dumps([langs[lg][k] for k in keys], separators=(",", ":"),
                                              ensure_ascii=False).encode()

    h = hashlib.sha1()
    for part in [js, css, schema] + [lang_files[k] for k in sorted(lang_files)]:
        h.update(part)
    digest = h.hexdigest()[:10]

    html = minify_html(html_src).replace("{{HASH}}", digest).replace("{{VER}}", a.ver)
    html = html.replace("<!--SPRITE-->", sprite(os.path.join(a.src, "icons")))
    with open(os.path.join(a.src, "favicon.svg"), "rb") as fh:
        favicon = fh.read()

    files = {"index.html.gz": gz(html.encode()), "app.js.gz": gz(js), "app.css.gz": gz(css),
             "schema.json.gz": gz(schema), "favicon.svg.gz": gz(favicon)}
    for name, data in lang_files.items():
        files[f"lang/{name}.gz"] = gz(data)
    for name, data in files.items():
        write(os.path.join(a.out, name), data)
    write(os.path.join(a.out, "gui_hash.txt"), digest.encode())

    with open(a.budget, encoding="utf-8") as fh:
        budget = json.load(fh)
    sizes = {"index.html": len(files["index.html.gz"]), "app.js": len(files["app.js.gz"]),
             "app.css": len(files["app.css.gz"]), "schema.json": len(files["schema.json.gz"]),
             "lang_total": sum(len(v) for k, v in files.items() if k.startswith("lang/")),
             "favicon": len(files["favicon.svg.gz"])}
    sizes["total"] = sum(len(v) for v in files.values())
    over = [f"{k} {v} > {budget[k]}" for k, v in sizes.items() if v > budget.get(k, 1 << 30)]
    print("build.py: GUI gz " + ", ".join(f"{k}={v}" for k, v in sizes.items()) + f", hash={digest}")
    if over:
        fail("boyut butcesi asildi: " + "; ".join(over))


if __name__ == "__main__":
    main()

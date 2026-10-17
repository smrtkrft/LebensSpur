#!/usr/bin/env python3
"""
LebensSpur - GUI dosyalarini derleme aninda gzip'ler.

Kullanim: gzip_gui.py <GUI_DIR> <OUT_DIR> <dosya> [<dosya> ...]
Cikti: <OUT_DIR>/<dosya>.gz  (mtime=0 -> deterministik, gereksiz yeniden derleme yok)

Neden: GUI yuklenirken WiFi TX suresi, gonderilen byte sayisiyla orantilidir.
Gzip, HTML/CSS/JS'i ~%70 kucultur; TX doluluk orani ve ortalama akim duser.
"""
import gzip
import os
import sys


def main() -> int:
    if len(sys.argv) < 4:
        print("kullanim: gzip_gui.py <GUI_DIR> <OUT_DIR> <dosya>...", file=sys.stderr)
        return 2
    gui_dir, out_dir, names = sys.argv[1], sys.argv[2], sys.argv[3:]
    os.makedirs(out_dir, exist_ok=True)
    for name in names:
        src = os.path.join(gui_dir, name)
        dst = os.path.join(out_dir, name + ".gz")
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        with open(src, "rb") as f:
            data = f.read()
        # Kaynak degismediyse yeniden yazma (build cache)
        if os.path.exists(dst) and os.path.getmtime(dst) >= os.path.getmtime(src):
            continue
        with open(dst, "wb") as f:
            with gzip.GzipFile(fileobj=f, mode="wb", compresslevel=9, mtime=0) as gz:
                gz.write(data)
        print(f"gzip: {name} {len(data)} -> {os.path.getsize(dst)} byte")
    return 0


if __name__ == "__main__":
    sys.exit(main())

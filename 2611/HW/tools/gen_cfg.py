#!/usr/bin/env python3
"""
LebensSpur 2611 - yapilandirma semasi ureteci.

Kullanim: gen_cfg.py <cfg_schema.json> <cikti_dizini>
Uretir:   <cikti>/cfg_schema.gen.h, <cikti>/cfg_schema.gen.c, <cikti>/schema.gen.json

Python 3.9 + stdlib (IDF python ortami ve sistem python3 ile calisir).
Ciktilar yalniz icerik degisince yazilir (gereksiz yeniden derleme olmasin).
"""
import json
import os
import sys

TYPES = {"str": "CFG_T_STR", "bool": "CFG_T_BOOL", "u8": "CFG_T_U8",
         "u16": "CFG_T_U16", "u32": "CFG_T_U32", "enum": "CFG_T_ENUM"}
CTYPE = {"bool": "bool", "u8": "uint8_t", "u16": "uint16_t", "u32": "uint32_t", "enum": "uint8_t"}
LIMITS = {"u8": 255, "u16": 65535, "u32": 4294967295}
BLOB_MAX = 4000


def fnv16(text):
    h = 0x811C9DC5
    for b in text.encode():
        h = ((h ^ b) * 0x01000193) & 0xFFFFFFFF
    return (h ^ (h >> 16)) & 0xFFFF


def check_field(sec, f):
    t = f["type"]
    if t not in TYPES:
        sys.exit(f"gen_cfg: {sec}.{f['name']}: bilinmeyen tip {t}")
    if t == "str" and not (0 < f.get("len", 0) <= 2048):
        sys.exit(f"gen_cfg: {sec}.{f['name']}: str len 1..2048 olmali")
    if t in LIMITS:
        lo, hi = f.get("min", 0), f.get("max", LIMITS[t])
        d = f.get("default", lo)
        if not (0 <= lo <= hi <= LIMITS[t]) or not (lo <= d <= hi):
            sys.exit(f"gen_cfg: {sec}.{f['name']}: min/max/default tutarsiz")
    if t == "enum" and f.get("default", f["values"][0]) not in f["values"]:
        sys.exit(f"gen_cfg: {sec}.{f['name']}: enum default degerlerde yok")


def c_struct(ctype, fields):
    out = ["typedef struct {"]
    for f in fields:
        if f["type"] == "str":
            out.append(f"    char {f['name']}[{f['len'] + 1}];")
        else:
            out.append(f"    {CTYPE[f['type']]} {f['name']};")
    out.append(f"}} {ctype};")
    return out


def c_fields(var, ctype, fields):
    out = []
    for f in fields:
        if f["type"] == "enum":
            vals = ", ".join(f'"{v}"' for v in f["values"])
            out.append(f"static const char *const {var}_{f['name']}_enum[] = {{ {vals} }};")
    out.append(f"static const cfg_field_t {var}[] = {{")
    for f in fields:
        t = f["type"]
        flags = []
        if f.get("secret"):
            flags.append("CFG_F_SECRET")
        if f.get("internal"):
            flags.append("CFG_F_INTERNAL")
        flags = " | ".join(flags) or "0"
        mn, mx, dn, ds, ev, ec = 0, 0, 0, "NULL", "NULL", 0
        if t == "str":
            mx = f["len"]
            ds = json.dumps(f.get("default", ""))
        elif t == "bool":
            mx, dn = 1, 1 if f.get("default", False) else 0
        elif t == "enum":
            ec = len(f["values"])
            mx = ec - 1
            dn = f["values"].index(f.get("default", f["values"][0]))
            ev = f"{var}_{f['name']}_enum"
        else:
            mn, mx = f.get("min", 0), f.get("max", LIMITS[t])
            dn = f.get("default", mn)
        out.append(
            f'    {{ "{f["name"]}", {TYPES[t]}, offsetof({ctype}, {f["name"]}), '
            f'sizeof((({ctype} *)0)->{f["name"]}), {mn}u, {mx}u, {dn}u, {ds}, {ev}, {ec}, {flags} }},')
    out.append("};")
    return out


def gui_field(f):
    g = {"name": f["name"], "type": f["type"]}
    for k in ("len", "min", "max", "values", "default"):
        if k in f:
            g[k] = f[k]
    if f.get("secret"):
        g["secret"] = True
    return g


def write_if_changed(path, text):
    if os.path.exists(path):
        with open(path, encoding="utf-8") as fh:
            if fh.read() == text:
                return
    with open(path, "w", encoding="utf-8") as fh:
        fh.write(text)


def main():
    if len(sys.argv) != 3:
        sys.exit("kullanim: gen_cfg.py <cfg_schema.json> <cikti_dizini>")
    with open(sys.argv[1], encoding="utf-8") as fh:
        schema = json.load(fh)
    out_dir = sys.argv[2]
    os.makedirs(out_dir, exist_ok=True)

    sections = schema["sections"]
    group = schema["group"]
    for sec, fields in sections.items():
        names = [f["name"] for f in fields]
        if len(names) != len(set(names)):
            sys.exit(f"gen_cfg: {sec}: tekrar eden alan adi")
        for f in fields:
            check_field(sec, f)
    for f in group["fields"]:
        check_field("group", f)

    h = ["/* URETILDI: tools/gen_cfg.py — elle duzenleme. Kaynak: cfg_schema.json */",
         "#pragma once", "#include <stdbool.h>", "#include <stddef.h>", "#include <stdint.h>", "",
         "typedef enum { " + ", ".join(TYPES.values()) + " } cfg_type_t;",
         "#define CFG_F_SECRET   1u",
         "#define CFG_F_INTERNAL 2u", "",
         "typedef struct {",
         "    const char *name;", "    uint8_t type;", "    uint16_t offset;", "    uint16_t size;",
         "    uint32_t min, max, def_num;", "    const char *def_str;",
         "    const char *const *enum_vals;", "    uint8_t enum_count;", "    uint8_t flags;",
         "} cfg_field_t;", "",
         "typedef struct {",
         "    const char *name;      /* API ve yedek anahtari */",
         "    const char *nvs_key;   /* NVS blob anahtari (<=15) */",
         "    uint16_t offset;       /* cfg_t icinde (grup icin 0) */",
         "    uint16_t size;", "    uint16_t schema;       /* alan duzeni ozeti: degisirse bolum varsayilana doner */",
         "    const cfg_field_t *fields;", "    uint8_t nfields;",
         "} cfg_section_desc_t;", ""]
    h.append("typedef enum {")
    for sec in sections:
        h.append(f"    CFG_SEC_{sec.upper()},")
    h += ["    CFG_SEC__COUNT", "} cfg_section_t;", ""]
    for sec, fields in sections.items():
        h += c_struct(f"cfg_{sec}_t", fields) + [""]
    h.append("typedef struct {")
    for sec in sections:
        h.append(f"    cfg_{sec}_t {sec};")
    h += ["} cfg_t;", ""]
    h += c_struct("cfg_group_t", group["fields"]) + [""]
    h += [f"#define CFG_GROUP_COUNT {group['count']}",
          "extern const cfg_section_desc_t cfg_sections[CFG_SEC__COUNT];",
          "extern const cfg_section_desc_t cfg_group_desc;", ""]

    c = ["/* URETILDI: tools/gen_cfg.py — elle duzenleme. */",
         '#include "cfg_schema.gen.h"', ""]
    for sec, fields in sections.items():
        c += c_fields(f"f_{sec}", f"cfg_{sec}_t", fields) + [
            f"_Static_assert(sizeof(cfg_{sec}_t) <= {BLOB_MAX}, \"{sec} blob cok buyuk\");", ""]
    c += c_fields("f_group", "cfg_group_t", group["fields"]) + [
        f"_Static_assert(sizeof(cfg_group_t) <= {BLOB_MAX}, \"group blob cok buyuk\");", ""]
    c.append("const cfg_section_desc_t cfg_sections[CFG_SEC__COUNT] = {")
    for sec, fields in sections.items():
        if len(sec) > 15:
            sys.exit(f"gen_cfg: bolum adi NVS anahtari icin uzun: {sec}")
        sig = fnv16(json.dumps(fields, sort_keys=True))
        c.append(f'    {{ "{sec}", "{sec}", offsetof(cfg_t, {sec}), sizeof(cfg_{sec}_t), 0x{sig:04X}, '
                 f'f_{sec}, {len(fields)} }},')
    c.append("};")
    gsig = fnv16(json.dumps(group["fields"], sort_keys=True))
    c.append(f'const cfg_section_desc_t cfg_group_desc = {{ "group", "grp", 0, sizeof(cfg_group_t), '
             f'0x{gsig:04X}, f_group, {len(group["fields"])} }};')

    gui = {"sections": {s: [gui_field(f) for f in fs if not f.get("internal")] for s, fs in sections.items()},
           "group": {"count": group["count"],
                     "fields": [gui_field(f) for f in group["fields"] if not f.get("internal")]}}

    write_if_changed(os.path.join(out_dir, "cfg_schema.gen.h"), "\n".join(h) + "\n")
    write_if_changed(os.path.join(out_dir, "cfg_schema.gen.c"), "\n".join(c) + "\n")
    write_if_changed(os.path.join(out_dir, "schema.gen.json"),
                     json.dumps(gui, ensure_ascii=False, separators=(",", ":")) + "\n")


if __name__ == "__main__":
    main()

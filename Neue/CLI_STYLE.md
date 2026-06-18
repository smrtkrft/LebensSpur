# LebensSpur CLI Style Guide

Audience: every developer adding or refactoring a `sk_cli_command_t` handler
in `esp32/ls/`. The CLI is split into two modes that share the same
handler bodies:

- **Human mode** - what the user types in `idf.py monitor` or a USB
  terminal. Plain space-separated words, no flags.
- **Machine mode (NDJSON)** - what SKAPP and other clients speak.
  `{"id":N,"cmd":"timer.set","args":{...}}` envelopes.

A single command definition (one row in your `s_cmds[]` table) drives
both. The dispatcher in `sk_core/src/sk_cli.c` figures out which mode a
line is in and gives your handler a `sk_cli_ctx_t *` you can query
uniformly.

## Table of contents

1. [Syntax rules](#1-syntax-rules)
2. [Usage hints (`sk_cli_usage`)](#2-usage-hints-sk_cli_usage)
3. [Human output (`sk_cli_kv` / `sk_cli_ok`)](#3-human-output-sk_cli_kv--sk_cli_ok)
4. [The `usage` field on `sk_cli_command_t`](#4-the-usage-field-on-sk_cli_command_t)
5. [English style rules](#5-english-style-rules)
6. [Skeleton handler](#6-skeleton-handler)
7. [Helper API quick reference](#7-helper-api-quick-reference)

---

## 1. Syntax rules

### 1.1 Canonical name (dot-notation) is unchanged
Internal names stay dotted: `timer.set`, `mail.group.recipient.add`,
`api.endpoint.add`. SKAPP NDJSON still ships `{"cmd":"timer.set",...}`.
The registry, help system, and capability/topic walk all key off the
dotted name.

### 1.2 Human input is space-separated, NO `--flag value`
What the user types:

```
LS-X> timer set minute 2
LS-X> timer set minute 2 alarm 1
LS-X> timer start
LS-X> relay duration 60
LS-X> mail group add Family
LS-X> mail group recipient add 0 alice@example.com
LS-X> device info
```

How the dispatcher reads it (longest-prefix match on the command table):

| Input                                   | Match                       | argv after match              |
| --------------------------------------- | --------------------------- | ----------------------------- |
| `timer set minute 2`                    | `timer.set`                 | `["minute","2"]`              |
| `timer set minute 2 alarm 1`            | `timer.set`                 | `["minute","2","alarm","1"]`  |
| `timer start`                           | `timer.start`               | `[]`                          |
| `relay duration 60`                     | `relay.duration`            | `["60"]`                      |
| `mail group add Family`                 | `mail.group.add`            | `["Family"]`                  |
| `mail group recipient add 0 alice@x.y`  | `mail.group.recipient.add`  | `["0","alice@x.y"]`           |
| `device info`                           | `device.info`               | `[]`                          |

The longest matching prefix wins, so a deeper subcommand is preferred
over its ancestor. The dispatcher tries down from 6 segments to 1
segment until a registered name is found.

### 1.3 Positional first, keyword pairs after
After the command name, arguments are either:

- **Positional** - first 1-2 plain values that the handler reads with
  `sk_cli_arg(ctx, idx)`. Example: `timer set <unit> <value>` ->
  `sk_cli_arg(ctx, 0)` is the unit, `sk_cli_arg(ctx, 1)` is the value.
- **Keyword pairs** - optional `<keyword> <value>` extras read with
  `sk_cli_arg_after(ctx, "alarm")`. The keyword and its value are
  adjacent tokens. Example: `timer set minute 2 alarm 1` ->
  `sk_cli_arg_after(ctx, "alarm")` returns `"1"`.

**Never** introduce `--flag value` in the user-facing syntax. The
old `sk_cli_arg_named` helper still works for backward compatibility
(it scans for `--key`), but new handlers must use:

- `sk_cli_arg(ctx, 0)` / `sk_cli_arg(ctx, 1)` for positional
- `sk_cli_arg_after(ctx, "keyword")` for optional keyword pairs
- `sk_cli_arg_after_long(ctx, "keyword", &n)` for the numeric variant

In **machine mode**, the same `sk_cli_arg_after*` helpers fall back to
JSON key lookup, so one code path handles both modes.

### 1.4 Machine mode is unchanged
SKAPP still sends:

```json
{"id":1,"cmd":"timer.set","args":{"unit":"minute","value":2,"alarms":1}}
```

In machine mode there are no positionals, so handlers should also support
named lookup as a fallback. Pattern:

```c
const char *unit = sk_cli_arg_after(ctx, "unit");
if (!unit) unit = sk_cli_arg(ctx, 0);   // human positional fallback
```

`sk_cli_arg_after` already does JSON key lookup in machine mode, so the
first line covers SKAPP. The second line covers `timer set minute 2`.

---

## 2. Usage hints (`sk_cli_usage`)

When a handler detects missing or invalid arguments, call
`sk_cli_usage(ctx, usage, params_desc, example)` to emit a structured
hint, then return a non-`SK_OK` status so the dispatcher's error trailer
fires (or `SK_OK` if you want the hint to be the only output).

```c
sk_cli_usage(ctx,
    "timer set <unit> <value> [alarm <N>]",
    "unit:  minute | hour | day\n"
    "value: 1..60\n"
    "alarm: 1..10 (default 3)",
    "timer set hour 8\n"
    "timer set hour 8 alarm 3");
return SK_ERR_MISSING_ARG;
```

**Human mode** renders:

```
Usage: timer set <unit> <value> [alarm <N>]
  unit:  minute | hour | day
  value: 1..60
  alarm: 1..10 (default 3)
Example: timer set hour 8
Example: timer set hour 8 alarm 3
error: ERR_MISSING_ARG - required argument is missing
```

**Machine mode** renders an error envelope:

```json
{"id":7,"ok":false,"err":"ERR_MISSING_ARG",
 "params":{"usage":"timer set <unit> <value> [alarm <N>]",
           "params":"unit: minute | hour | day\nvalue: 1..60\nalarm: 1..10 (default 3)",
           "example":"timer set hour 8\ntimer set hour 8 alarm 3"}}
```

In machine mode `sk_cli_usage` sets `wrote_envelope = true`, so do NOT
also call `sk_cli_err` or `sk_cli_ok` afterwards.

### 2.1 Writing good hints
- **`usage`**: one line, no quotes around literals. Placeholders go in
  angle brackets (`<unit>`), optional pieces in square brackets
  (`[alarm <N>]`).
- **`params_desc`**: one indented line per parameter, format
  `name: value-list-or-range`. Keep it short - the example is what the
  user copies. Multi-line via `\n`.
- **`example`**: one or more concrete, runnable lines. Multi-line via
  `\n`; each line becomes its own `Example:` row.
- **Language**: English. Title Case is reserved for `sk_cli_kv` labels;
  hint prose stays lowercase imperative ("Set the countdown duration").
- **No em-dash**: use `:` for label separation or `-` for asides. Never
  use the U+2014 character.
- Pass `NULL` for any field you want to skip. All-NULL is legal (rare).

---

## 3. Human output (`sk_cli_kv` / `sk_cli_ok`)

There are two output styles, and the choice depends on the mode.

### 3.1 Machine mode: ALWAYS `sk_cli_ok(ctx, json_string)`
Build a JSON object string with `snprintf` and hand it to
`sk_cli_ok`. The dispatcher wraps it in the success envelope.
`sk_cli_kv` is intentionally a **no-op** in machine mode - SKAPP must
get structured data, not labeled rows.

```c
char json[256];
snprintf(json, sizeof(json),
         "{\"state\":\"%s\",\"unit\":\"%s\",\"value\":%u,\"remaining_sec\":%u}",
         state_str, unit_str, value, remaining);
sk_cli_ok(ctx, json);
```

The built-in pretty-printer renders this as a multi-line human block
automatically - you do NOT need to write two versions for one
status command.

### 3.2 Human mode: prefer `sk_cli_kv` for ad-hoc inline output
When you want to print labeled rows that don't fit cleanly into the
JSON shape (or you want to mix in commentary), use `sk_cli_kv` /
`sk_cli_kvf`:

```c
if (!sk_cli_is_machine_mode(ctx)) {
    sk_cli_kv (ctx, "Device",         "LebensSpur");
    sk_cli_kvf(ctx, "State",          "%s", running ? "active" : "off");
    sk_cli_kvf(ctx, "Remaining time", "%s", duration_buf);
    sk_cli_write(ctx, "\n", 1);
}
```

Each row renders as `  <label>: <value>\n`. Sample rendered block:

```
  Device: LebensSpur
  State: countdown
  Remaining time: 1h 30m
  Alarm count: 3
```

### 3.3 When to use which
| Situation                                | Use                                       |
| ---------------------------------------- | ----------------------------------------- |
| Standard `*.get` / `*.status` command    | `sk_cli_ok(ctx, json)` - works both modes |
| Detailed human-only diagnostic           | `sk_cli_kv` / `sk_cli_kvf` + `sk_cli_ok(ctx, NULL)` |
| Streaming progress (e.g. WiFi scan rows) | `sk_cli_writef` directly                  |
| Error                                    | `sk_cli_err(ctx, SK_ERR_..., params)`     |
| Missing/invalid args                     | `sk_cli_usage(...)` then return non-SK_OK |

### 3.4 Duration formatting
Use `sk_cli_fmt_duration(buf, sizeof(buf), seconds)` to turn a `uint32_t`
into a string like `1h 30m` / `1d 2h` / `45s`. Don't roll your own in
every handler.

Unit suffixes (single-letter, lowercase):

| Unit    | Suffix | Example       |
| ------- | ------ | ------------- |
| Days    | `d`    | `86400 -> 1d` |
| Hours   | `h`    | `3600 -> 1h`  |
| Minutes | `m`    | `90 -> 1m 30s` |
| Seconds | `s`    | `45 -> 45s`   |

Combined: `5430 -> 1h 30m 30s`. Zero is rendered as `0s`. Components
with value `0` are omitted (e.g. exactly one hour is `1h`, not
`1h 0m 0s`).

---

## 4. The `usage` field on `sk_cli_command_t`

The `usage` field on every command registration must match the new
syntax. It feeds `help <cmd>`, the confirm-token retry hint, and the
machine-mode `help` discovery output.

**Good** (new style - space-separated, positional first):

```c
.usage = "timer set <unit> <value> [alarm <N>]"
.usage = "relay duration <seconds>"
.usage = "mail group add <name>"
.usage = "mail group recipient add <group-id> <email>"
.usage = "wifi connect <ssid> [password <secret>]"
```

**Bad** (legacy `--flag` style - DO NOT use in new handlers):

```c
.usage = "timer.set --unit <u> --value <v> --alarms <n>"   // NO
.usage = "relay.duration --duration <s>"                   // NO
```

Rules:

- Start with the human-form command name (dots become spaces).
- Use `<placeholder>` for required values, `[optional <x>]` for optional
  keyword pairs.
- Single-property setters (one positional arg, no choice) drop the
  keyword: `relay duration <seconds>`, not `relay duration duration <s>`.

The `summary` field stays one short English line (the help overview
uses it across categories). Long-form guidance goes in `help_block` and
`sk_cli_usage` hints.

---

## 5. English style rules

This section is the contract every component handler must follow so
the user sees one coherent CLI across `timer`, `relay`, `mail`, `smtp`,
`reset`, `device`, `wifi`, `ble`, etc.

### 5.1 Label case (`sk_cli_kv` first argument)
Use **Title Case** for labels. Multi-word labels capitalize only the
first word.

| Good             | Bad                |
| ---------------- | ------------------ |
| `State`          | `state`, `STATE`   |
| `Remaining time` | `Remaining Time`   |
| `Alarm count`    | `Alarm Count`      |
| `Last error`     | `last_error`       |
| `Active`         | `active`           |

### 5.2 Value case (`sk_cli_kv` second argument)
Use **lowercase** for values. Values are machine-y tokens; capitalize
only proper nouns (e.g. device names, SSIDs).

| Good                | Bad                  |
| ------------------- | -------------------- |
| `countdown`         | `Countdown`          |
| `off`               | `Off`                |
| `active`            | `Active`             |
| `connected`         | `Connected`          |
| `LebensSpur`        | `lebensspur`         |

### 5.3 Booleans
Display booleans as **`yes` / `no`**, not `true` / `false`. The latter
looks machine-y in human mode.

```c
sk_cli_kv(ctx, "Active", enabled ? "yes" : "no");
```

The JSON path (machine mode) still uses native `true` / `false`.

### 5.4 Empty / unset values
Use **`(not set)`** rather than an empty string. Avoids the confusing
`Label: ` row.

```c
sk_cli_kv(ctx, "SSID", ssid[0] ? ssid : "(not set)");
```

### 5.5 Durations
Always render via `sk_cli_fmt_duration`. Never hand-format
`"%d sec"`-style strings.

### 5.6 Command summaries and help_block
- **Summary** (one line): imperative, no trailing period.
  Good: `"Set the countdown duration"`. Bad: `"Sets..."`,
  `"Configures the timer."`.
- **help_block**: prose may use sentences with periods. Place
  `Example:` (single) or `Examples:` (multiple) at the very end,
  followed by indented runnable lines. Use the actual `LS-X>` prompt
  or just the bare command form (no `$ ` prefix).

```
help_block =
  "timer set <unit> <value> [alarm <N>]\n"
  "  unit:  minute | hour | day\n"
  "  value: 1..60\n"
  "  alarm: 1..10 (default 3)\n"
  "Examples:\n"
  "  timer set hour 8\n"
  "  timer set hour 8 alarm 3"
```

### 5.7 Common field labels - REQUIRED vocabulary
Use exactly these labels (Title Case) for shared concepts so the help
output looks consistent across components:

| Concept            | Label             |
| ------------------ | ----------------- |
| Current state      | `State`           |
| Time unit          | `Unit`            |
| Configured value   | `Value`           |
| Time remaining     | `Remaining time`  |
| Total duration     | `Duration`        |
| Connectivity / op  | `Status`          |
| Boolean enable     | `Active`          |
| TCP/UDP port       | `Port`            |
| Server / IP host   | `Host`            |
| Email address      | `Email`           |
| Network identifier | `SSID`            |
| Device identifier  | `Device`          |
| Error message      | `Last error`      |
| Counter            | `Count` / `<X> count` (e.g. `Alarm count`) |

For values:

| Concept                 | Value              |
| ----------------------- | ------------------ |
| Countdown running       | `countdown`        |
| Off / disabled          | `off`              |
| On / running            | `active`           |
| Idle                    | `idle`             |
| Connected               | `connected`        |
| Disconnected            | `disconnected`     |
| Unset / not configured  | `(not set)`        |

---

## 6. Skeleton handler

Complete example for a new `timer.set` showing every convention:

```c
// Public usage row attached to the registration.
static const sk_cli_command_t s_cmd_timer_set = {
    .name       = "timer.set",
    .summary    = "Configure countdown unit/value/alarms",
    .usage      = "timer set <unit> <value> [alarm <N>]",
    .help_block =
        "timer set <unit> <value> [alarm <N>]\n"
        "  unit:  minute | hour | day\n"
        "  value: 1..60\n"
        "  alarm: 1..10 (default 3)\n"
        "Examples:\n"
        "  timer set hour 8\n"
        "  timer set hour 8 alarm 3",
    .handler    = on_timer_set,
};

static sk_err_t on_timer_set(sk_cli_ctx_t *ctx)
{
    // --- 1) Read args (positional first, keyword pair second, fall back to
    //         machine-mode JSON keys via the same helpers) -----------------
    const char *unit_s  = sk_cli_arg_after(ctx, "unit");
    const char *value_s = sk_cli_arg_after(ctx, "value");
    if (!unit_s)  unit_s  = sk_cli_arg(ctx, 0);
    if (!value_s) value_s = sk_cli_arg(ctx, 1);

    long alarms = -1;     // -1 = leave as-is
    sk_cli_arg_after_long(ctx, "alarm", &alarms);

    // --- 2) Validate, emit usage hint on failure ---------------------------
    if (!unit_s || !value_s) {
        sk_cli_usage(ctx,
            "timer set <unit> <value> [alarm <N>]",
            "unit:  minute | hour | day\n"
            "value: 1..60\n"
            "alarm: 1..10",
            "timer set hour 8\n"
            "timer set hour 8 alarm 3");
        return SK_ERR_MISSING_ARG;
    }

    ls_timer_unit_t unit;
    if      (strcmp(unit_s, "minute") == 0) unit = LS_TIMER_UNIT_MINUTE;
    else if (strcmp(unit_s, "hour")   == 0) unit = LS_TIMER_UNIT_HOUR;
    else if (strcmp(unit_s, "day")    == 0) unit = LS_TIMER_UNIT_DAY;
    else {
        sk_cli_usage(ctx, NULL, "unit: minute | hour | day", NULL);
        return SK_ERR_INVALID_ARG;
    }
    int value = atoi(value_s);
    if (value < 1 || value > 60) {
        sk_cli_usage(ctx, NULL, "value: 1..60", NULL);
        return SK_ERR_INVALID_ARG;
    }

    // --- 3) Dispatch to engine via queue (NEVER mutate state inline) ------
    evt_t evt = { .type = EVT_CMD_SET, .i_arg1 = unit, .i_arg2 = value,
                  .i_arg3 = (alarms >= 0) ? (int)alarms : -1 };
    if (xQueueSend(s_evt_q, &evt, pdMS_TO_TICKS(100)) != pdTRUE) {
        return SK_ERR_BUSY;
    }

    // --- 4) Emit response - same JSON shape for both modes -----------------
    char data[160];
    snprintf(data, sizeof(data),
             "{\"unit\":\"%s\",\"value\":%d,\"alarms\":%ld}",
             unit_s, value, alarms >= 0 ? alarms : (long)s_cfg.alarms);
    sk_cli_ok(ctx, data);   // pretty-prints in human mode, JSON in machine mode

    return SK_OK;
}
```

Notes embedded in the skeleton:

- Step 1: always check `sk_cli_arg_after` first, then positional. Order
  doesn't matter for correctness (only one will return non-NULL for a
  given mode + line), but the convention helps readers spot the
  expected keyword names.
- Step 2: prefer `sk_cli_usage` over hand-rolled `sk_cli_writef`
  hints. Consistent format helps the user learn the CLI.
- Step 3: NEVER call NVS / driver / radio code from a CLI handler.
  Push an event onto your component's queue and return - the worker
  task owns the state.
- Step 4: one `snprintf` -> `sk_cli_ok`. The pretty-printer renders the
  JSON as multi-line human output automatically.

---

## 7. Helper API quick reference

```c
// Args
int          sk_cli_argc        (sk_cli_ctx_t *ctx);
const char  *sk_cli_arg         (sk_cli_ctx_t *ctx, int idx);              // human positional
const char  *sk_cli_arg_after   (sk_cli_ctx_t *ctx, const char *keyword);  // "alarm 1" -> "1"
bool         sk_cli_arg_after_long(sk_cli_ctx_t *ctx, const char *keyword, long *out);
bool         sk_cli_arg_long    (sk_cli_ctx_t *ctx, const char *key, long *out_value);
const char  *sk_cli_arg_named   (sk_cli_ctx_t *ctx, const char *key);      // legacy --key
bool         sk_cli_is_machine_mode(sk_cli_ctx_t *ctx);
bool         sk_cli_is_authenticated(sk_cli_ctx_t *ctx);
const char  *sk_cli_confirm_token(sk_cli_ctx_t *ctx);

// Output
void         sk_cli_write       (sk_cli_ctx_t *ctx, const char *chunk, size_t len);
void         sk_cli_writef      (sk_cli_ctx_t *ctx, const char *fmt, ...);
void         sk_cli_kv          (sk_cli_ctx_t *ctx, const char *label, const char *value);
void         sk_cli_kvf         (sk_cli_ctx_t *ctx, const char *label, const char *fmt, ...);
void         sk_cli_usage       (sk_cli_ctx_t *ctx,
                                 const char *usage,
                                 const char *params_desc,
                                 const char *example);
size_t       sk_cli_fmt_duration(char *out, size_t cap, uint32_t seconds);

// Response envelopes
void         sk_cli_ok          (sk_cli_ctx_t *ctx, const char *data_json_or_null);
void         sk_cli_err         (sk_cli_ctx_t *ctx, sk_err_t err, const char *params_json_or_null);
```

Reach out to the foundation maintainer before adding new helpers - the
goal is for every component to share the same vocabulary so the user
sees a single coherent CLI across `timer`, `relay`, `mail`, `smtp`,
`reset`, `device`, `wifi`, `ble`, etc.

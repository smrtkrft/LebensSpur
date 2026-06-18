#pragma once

#include "sk_cli.h"
#include "cJSON.h"

// Internal CLI context, exposed only to sk_cli_*.c files via private_include.

struct sk_cli_ctx {
    bool              is_machine_mode;
    sk_cli_writer_t   writer;
    void             *writer_user;
    const char       *command_name;      // canonical name ("timer.set")
    const char       *confirm_token;
    bool              authenticated;     // dispatched via *_authenticated path

    // Machine mode: parsed top-level JSON object (owned by dispatcher).
    cJSON            *machine_msg;
    cJSON            *machine_args;      // borrowed
    int               machine_id;        // request id, -1 if absent

    // Human mode: tokenized args past the command words.
    const char      **human_argv;
    int               human_argc;

    // Set once the handler called sk_cli_ok/err so the dispatcher does not
    // auto-emit.
    bool              wrote_envelope;
};

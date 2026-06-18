#include "sk_errors.h"

#include <stddef.h>

// Parallel tables generated from the X-macro so adding a new code in one
// place (sk_errors.h) keeps strings and messages in sync.

static const char *s_code_strings[] = {
    [SK_OK] = "OK",
#define X(sym, code_str, msg_str) [sym] = code_str,
    SK_ERROR_TABLE(X)
#undef X
};

static const char *s_messages[] = {
    [SK_OK] = "",
#define X(sym, code_str, msg_str) [sym] = msg_str,
    SK_ERROR_TABLE(X)
#undef X
};

const char *sk_err_code_string(sk_err_t code)
{
    if ((int)code < 0 || (int)code >= SK_ERR__COUNT) return "ERR_UNKNOWN";
    return s_code_strings[code] ? s_code_strings[code] : "ERR_UNKNOWN";
}

const char *sk_err_message(sk_err_t code)
{
    if ((int)code < 0 || (int)code >= SK_ERR__COUNT) return "Unknown error";
    return s_messages[code] ? s_messages[code] : "Unknown error";
}

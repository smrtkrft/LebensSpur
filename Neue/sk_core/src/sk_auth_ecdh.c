#include "sk_auth.h"

#include <string.h>

#include "esp_log.h"
#include "esp_random.h"
#include "mbedtls/ecdh.h"
#include "mbedtls/ecp.h"
#include "mbedtls/sha256.h"

static const char *TAG = "sk_auth_ecdh";

// Token derivation: SHA256("sk_auth_token_v1" || shared_secret).
static void derive_token(const uint8_t shared[32], uint8_t token[32])
{
    static const char *LABEL = "sk_auth_token_v1";
    mbedtls_sha256_context sha;
    mbedtls_sha256_init(&sha);
    mbedtls_sha256_starts(&sha, 0);
    mbedtls_sha256_update(&sha, (const uint8_t *)LABEL, strlen(LABEL));
    mbedtls_sha256_update(&sha, shared, 32);
    mbedtls_sha256_finish(&sha, token);
    mbedtls_sha256_free(&sha);
}

extern esp_err_t sk_auth__install_token(const uint8_t token[SK_AUTH_TOKEN_LEN]);

static int rng_wrapper(void *ctx, unsigned char *buf, size_t len)
{
    (void)ctx;
    esp_fill_random(buf, len);
    return 0;
}

esp_err_t sk_auth_ecdh_begin(sk_auth_ecdh_ctx_t *ctx)
{
    if (!ctx) return ESP_ERR_INVALID_ARG;

    mbedtls_ecp_group grp;
    mbedtls_mpi       d;
    mbedtls_ecp_point Q;
    mbedtls_ecp_group_init(&grp);
    mbedtls_mpi_init(&d);
    mbedtls_ecp_point_init(&Q);

    int rc = mbedtls_ecp_group_load(&grp, MBEDTLS_ECP_DP_CURVE25519);
    if (rc != 0) { ESP_LOGE(TAG, "group load: -0x%04X", -rc); goto out; }

    rc = mbedtls_ecdh_gen_public(&grp, &d, &Q, rng_wrapper, NULL);
    if (rc != 0) { ESP_LOGE(TAG, "gen_public: -0x%04X", -rc); goto out; }

    size_t olen = 0;
    rc = mbedtls_ecp_point_write_binary(&grp, &Q, MBEDTLS_ECP_PF_COMPRESSED,
                                        &olen, ctx->our_public,
                                        SK_AUTH_ECDH_PUBLIC_LEN);
    if (rc != 0 || olen != SK_AUTH_ECDH_PUBLIC_LEN) {
        ESP_LOGE(TAG, "point_write_binary: rc=-0x%04X olen=%u", -rc, (unsigned)olen);
        rc = (rc != 0) ? rc : -1;
        goto out;
    }

    rc = mbedtls_mpi_write_binary_le(&d, ctx->our_secret, 32);

out:
    mbedtls_ecp_point_free(&Q);
    mbedtls_mpi_free(&d);
    mbedtls_ecp_group_free(&grp);
    return rc == 0 ? ESP_OK : ESP_FAIL;
}

// Compute shared secret + KDF; returns the 32-byte token. Does NOT touch
// NVS — caller decides when to commit. Wipes ctx->our_secret on success.
esp_err_t sk_auth_ecdh_derive(sk_auth_ecdh_ctx_t *ctx,
                              const uint8_t peer_public[SK_AUTH_ECDH_PUBLIC_LEN],
                              uint8_t       out_bond_key[SK_AUTH_TOKEN_LEN])
{
    if (!ctx || !peer_public || !out_bond_key) return ESP_ERR_INVALID_ARG;

    mbedtls_ecp_group grp;
    mbedtls_mpi       d;
    mbedtls_mpi       z;
    mbedtls_ecp_point Qp;
    uint8_t shared[32] = {0};

    mbedtls_ecp_group_init(&grp);
    mbedtls_mpi_init(&d);
    mbedtls_mpi_init(&z);
    mbedtls_ecp_point_init(&Qp);

    int rc = mbedtls_ecp_group_load(&grp, MBEDTLS_ECP_DP_CURVE25519);
    if (rc != 0) goto out;

    rc = mbedtls_mpi_read_binary_le(&d, ctx->our_secret, 32);
    if (rc != 0) goto out;

    rc = mbedtls_ecp_point_read_binary(&grp, &Qp, peer_public,
                                       SK_AUTH_ECDH_PUBLIC_LEN);
    if (rc != 0) { ESP_LOGE(TAG, "peer point read: -0x%04X", -rc); goto out; }

    rc = mbedtls_ecdh_compute_shared(&grp, &z, &Qp, &d, rng_wrapper, NULL);
    if (rc != 0) { ESP_LOGE(TAG, "compute_shared: -0x%04X", -rc); goto out; }

    rc = mbedtls_mpi_write_binary_le(&z, shared, sizeof(shared));

out:
    mbedtls_ecp_point_free(&Qp);
    mbedtls_mpi_free(&z);
    mbedtls_mpi_free(&d);
    mbedtls_ecp_group_free(&grp);

    if (rc != 0) {
        memset(shared, 0, sizeof(shared));
        return ESP_FAIL;
    }

    derive_token(shared, out_bond_key);
    memset(shared, 0, sizeof(shared));
    memset(ctx->our_secret, 0, sizeof(ctx->our_secret));
    return ESP_OK;
}

// Legacy convenience: derive + immediately install via sk_auth__install_token
// (which lands in slot 0 with a zero peer_id). New code uses ecdh_derive +
// sk_auth_bond_add with a real peer_id.
esp_err_t sk_auth_ecdh_complete(sk_auth_ecdh_ctx_t *ctx,
                                const uint8_t peer_public[SK_AUTH_ECDH_PUBLIC_LEN])
{
    uint8_t token[SK_AUTH_TOKEN_LEN];
    esp_err_t err = sk_auth_ecdh_derive(ctx, peer_public, token);
    if (err != ESP_OK) return err;
    err = sk_auth__install_token(token);
    memset(token, 0, sizeof(token));
    return err;
}

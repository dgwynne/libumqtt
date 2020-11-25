/*
 * MIT License
 *
 * Copyright (c) 2019 Jianhui Zhao <zhaojh329@gmail.com>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include <errno.h>
#include <stdlib.h>

#include "log.h"
#include "ssl.h"
#include "buffer.h"

#if UMQTT_SSL_SUPPORT

#if UMQTT_HAVE_MBEDTLS
#include <mbedtls/ssl.h>
#include <mbedtls/entropy.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/net_sockets.h>

struct umqtt_ssl_ctx {
    mbedtls_net_context      net;
    mbedtls_ssl_context      ssl;
    mbedtls_ssl_config       cfg;
    mbedtls_ctr_drbg_context drbg;
    mbedtls_entropy_context  etpy;
    mbedtls_x509_crt         x509;
    bool last_read_ok;
};

#else

#if UMQTT_HAVE_OPENSSL
#include <openssl/ssl.h>
#include <openssl/err.h>
#elif UMQTT_HAVE_WOLFSSL
#define WC_NO_HARDEN
#include <wolfssl/openssl/ssl.h>
#include <wolfssl/openssl/err.h>
#endif

struct umqtt_ssl_ctx {
    SSL_CTX *ctx;
    SSL *ssl;
};

#endif

int umqtt_ssl_init(struct umqtt_ssl_ctx **ctx, int sock)
{
    struct umqtt_ssl_ctx *c = calloc(1, sizeof(struct umqtt_ssl_ctx));

    if (!ctx) {
        umqtt_log_err("calloc failed: %s\n", strerror(errno));
        return -1;
    }

#if UMQTT_HAVE_MBEDTLS
    mbedtls_net_init(&c->net);
    mbedtls_ssl_init(&c->ssl);
    mbedtls_ssl_config_init(&c->cfg);
    mbedtls_ctr_drbg_init(&c->drbg);
    mbedtls_x509_crt_init(&c->x509);

    mbedtls_entropy_init(&c->etpy);
    mbedtls_ctr_drbg_seed(&c->drbg, mbedtls_entropy_func, &c->etpy, NULL, 0);

    mbedtls_ssl_config_defaults(&c->cfg, MBEDTLS_SSL_IS_CLIENT,
        MBEDTLS_SSL_TRANSPORT_STREAM, MBEDTLS_SSL_PRESET_DEFAULT);

    mbedtls_ssl_conf_authmode(&c->cfg, MBEDTLS_SSL_VERIFY_OPTIONAL);
    mbedtls_ssl_conf_ca_chain(&c->cfg, &c->x509, NULL);
    mbedtls_ssl_conf_rng(&c->cfg, mbedtls_ctr_drbg_random, &c->drbg);

    mbedtls_ssl_set_bio(&c->ssl, &c->net, mbedtls_net_send,
        mbedtls_net_recv, mbedtls_net_recv_timeout);

    mbedtls_ssl_setup(&c->ssl, &c->cfg);

    c->net.fd = sock;
#else
    SSL_library_init();
    SSL_load_error_strings();

    c->ctx = SSL_CTX_new(SSLv23_client_method());
    SSL_CTX_set_verify(c->ctx, SSL_VERIFY_NONE, NULL);

    c->ssl = SSL_new(c->ctx);
    SSL_set_fd(c->ssl, sock);
#endif

    *ctx = c;
    return 0;
}

int umqtt_ssl_handshake(struct umqtt_ssl_ctx *ctx)
{
#if UMQTT_HAVE_MBEDTLS
    int ret = mbedtls_ssl_handshake(&ctx->ssl);
    if (ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE)
        return 0;
    if (ret == 0)
        return 1;
    return -1;
#else
    int ret = SSL_connect(ctx->ssl);
    if (ret == 1) {
        return 1;
    } else {
        int err = SSL_get_error(ctx->ssl, ret);
        if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE)
            return 0;
        umqtt_log_err("%s\n", ERR_error_string(err, NULL));
        return -1;
    }
#endif
}

void umqtt_ssl_free(struct umqtt_ssl_ctx *ctx)
{
    if (!ctx)
        return;

#if UMQTT_HAVE_MBEDTLS
    mbedtls_ssl_free(&ctx->ssl);
    mbedtls_ssl_config_free(&ctx->cfg);
#else
    SSL_shutdown(ctx->ssl);
    SSL_CTX_free(ctx->ctx);
#endif
    free(ctx);
}

int umqtt_ssl_read(int fd, void *buf, size_t count, void *arg)
{
    struct umqtt_ssl_ctx *ctx = arg;

#if UMQTT_HAVE_MBEDTLS
    int ret;

    if (ctx->last_read_ok) {
        ctx->last_read_ok = false;
        return P_FD_PENDING;
    }

    ret = mbedtls_ssl_read(&ctx->ssl, buf, count);
    if (ret < 0) {
        if (ret == MBEDTLS_ERR_SSL_WANT_READ)
            return P_FD_PENDING;
        return P_FD_ERR;
    }
    if (ret > 0)
        ctx->last_read_ok = true;
#else
    int ret = SSL_read(ctx->ssl, buf, count);
    if (ret < 0) {
        int err = SSL_get_error(ctx->ssl, ret);
        if (err == SSL_ERROR_WANT_READ)
            return P_FD_PENDING;
        umqtt_log_err("%s\n", ERR_error_string(err, NULL));
        return P_FD_ERR;
    }
#endif
    return ret;
}

int umqtt_ssl_write(int fd, void *buf, size_t count, void *arg)
{
    struct umqtt_ssl_ctx *ctx = arg;

#if UMQTT_HAVE_MBEDTLS
    int ret = mbedtls_ssl_write(&ctx->ssl, buf, count);
    if (ret < 0) {
        if (ret == MBEDTLS_ERR_SSL_WANT_WRITE)
            return P_FD_PENDING;
        return P_FD_ERR;
    }
#else
    int ret = SSL_write(ctx->ssl, buf, count);
    if (ret < 0) {
        int err = SSL_get_error(ctx->ssl, ret);
        if (err == SSL_ERROR_WANT_WRITE)
            return P_FD_PENDING;
        umqtt_log_err("%s\n", ERR_error_string(err, NULL));
        return P_FD_ERR;
    }
#endif
    return ret;
}

#endif

/*
 * Copyright (c) 2014 DeNA Co., Ltd.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to
 * deal in the Software without restriction, including without limitation the
 * rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 * sell copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */
#include <stddef.h>
#include <stdlib.h>
#include <sys/time.h>
#include "h2o.h"
#include "h2o/memcached.h"

void h2o_context_init_pathconf_context(h2o_context_t *ctx, h2o_pathconf_t *pathconf)
{
    /* add pathconf to the inited list (or return if already inited) */
    size_t i;
    for (i = 0; i != ctx->_pathconfs_inited.size; ++i)
        if (ctx->_pathconfs_inited.entries[i] == pathconf)
            return;
    h2o_vector_reserve(NULL, &ctx->_pathconfs_inited, ctx->_pathconfs_inited.size + 1);
    ctx->_pathconfs_inited.entries[ctx->_pathconfs_inited.size++] = pathconf;

#define DOIT(type, list)                                                                                                           \
    do {                                                                                                                           \
        size_t i;                                                                                                                  \
        for (i = 0; i != pathconf->list.size; ++i) {                                                                               \
            type *o = pathconf->list.entries[i];                                                                                   \
            if (o->on_context_init != NULL)                                                                                        \
                o->on_context_init(o, ctx);                                                                                        \
        }                                                                                                                          \
    } while (0)

    DOIT(h2o_handler_t, handlers);
    DOIT(h2o_filter_t, filters);
    DOIT(h2o_logger_t, loggers);

#undef DOIT
}

void h2o_context_dispose_pathconf_context(h2o_context_t *ctx, h2o_pathconf_t *pathconf)
{
    /* nullify pathconf in the inited list (or return if already disposed) */
    size_t i;
    for (i = 0; i != ctx->_pathconfs_inited.size; ++i)
        if (ctx->_pathconfs_inited.entries[i] == pathconf)
            break;
    if (i == ctx->_pathconfs_inited.size)
        return;
    ctx->_pathconfs_inited.entries[i] = NULL;

#define DOIT(type, list)                                                                                                           \
    do {                                                                                                                           \
        size_t i;                                                                                                                  \
        for (i = 0; i != pathconf->list.size; ++i) {                                                                               \
            type *o = pathconf->list.entries[i];                                                                                   \
            if (o->on_context_dispose != NULL)                                                                                     \
                o->on_context_dispose(o, ctx);                                                                                     \
        }                                                                                                                          \
    } while (0)

    DOIT(h2o_handler_t, handlers);
    DOIT(h2o_filter_t, filters);
    DOIT(h2o_logger_t, loggers);

#undef DOIT
}

void h2o_context_init(h2o_context_t *ctx, h2o_loop_t *loop, h2o_globalconf_t *config)
{
    size_t i, j;

    assert(config->hosts[0] != NULL);

    memset(ctx, 0, sizeof(*ctx));
    ctx->loop = loop;
    ctx->globalconf = config;
    h2o_timeout_init(ctx->loop, &ctx->zero_timeout, 0);
    h2o_timeout_init(ctx->loop, &ctx->one_sec_timeout, 1000);
    ctx->queue = h2o_multithread_create_queue(loop);
    h2o_multithread_register_receiver(ctx->queue, &ctx->receivers.hostinfo_getaddr, h2o_hostinfo_getaddr_receiver);
    ctx->filecache = h2o_filecache_create(config->filecache.capacity);

    h2o_timeout_init(ctx->loop, &ctx->handshake_timeout, config->handshake_timeout);
    h2o_timeout_init(ctx->loop, &ctx->http1.req_timeout, config->http1.req_timeout);
    h2o_linklist_init_anchor(&ctx->http1._conns);
    h2o_timeout_init(ctx->loop, &ctx->http2.idle_timeout, config->http2.idle_timeout);
    h2o_linklist_init_anchor(&ctx->http2._conns);
    ctx->proxy.client_ctx.loop = loop;
    h2o_timeout_init(ctx->loop, &ctx->proxy.io_timeout, config->proxy.io_timeout);
    ctx->proxy.client_ctx.getaddr_receiver = &ctx->receivers.hostinfo_getaddr;
    ctx->proxy.client_ctx.io_timeout = &ctx->proxy.io_timeout;
    ctx->proxy.client_ctx.ssl_ctx = config->proxy.ssl_ctx;

    ctx->_module_configs = h2o_mem_alloc(sizeof(*ctx->_module_configs) * config->_num_config_slots);
    memset(ctx->_module_configs, 0, sizeof(*ctx->_module_configs) * config->_num_config_slots);

    static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
    pthread_mutex_lock(&mutex);
    for (i = 0; config->hosts[i] != NULL; ++i) {
        h2o_hostconf_t *hostconf = config->hosts[i];
        for (j = 0; j != hostconf->paths.size; ++j) {
            h2o_pathconf_t *pathconf = hostconf->paths.entries + j;
            h2o_context_init_pathconf_context(ctx, pathconf);
        }
        h2o_context_init_pathconf_context(ctx, &hostconf->fallback_path);
    }
    pthread_mutex_unlock(&mutex);
}

void h2o_context_dispose(h2o_context_t *ctx)
{
    h2o_globalconf_t *config = ctx->globalconf;
    size_t i, j;

    for (i = 0; config->hosts[i] != NULL; ++i) {
        h2o_hostconf_t *hostconf = config->hosts[i];
        for (j = 0; j != hostconf->paths.size; ++j) {
            h2o_pathconf_t *pathconf = hostconf->paths.entries + j;
            h2o_context_dispose_pathconf_context(ctx, pathconf);
        }
        h2o_context_dispose_pathconf_context(ctx, &hostconf->fallback_path);
    }
    free(ctx->_pathconfs_inited.entries);
    free(ctx->_module_configs);
    h2o_timeout_dispose(ctx->loop, &ctx->zero_timeout);
    h2o_timeout_dispose(ctx->loop, &ctx->one_sec_timeout);
    h2o_timeout_dispose(ctx->loop, &ctx->handshake_timeout);
    h2o_timeout_dispose(ctx->loop, &ctx->http1.req_timeout);
    h2o_timeout_dispose(ctx->loop, &ctx->http2.idle_timeout);
    h2o_timeout_dispose(ctx->loop, &ctx->proxy.io_timeout);
    /* what should we do here? assert(!h2o_linklist_is_empty(&ctx->http2._conns); */

    h2o_filecache_destroy(ctx->filecache);
    ctx->filecache = NULL;

    /* TODO assert that the all the getaddrinfo threads are idle */
    h2o_multithread_unregister_receiver(ctx->queue, &ctx->receivers.hostinfo_getaddr);
    h2o_multithread_destroy_queue(ctx->queue);

#if H2O_USE_LIBUV
    /* make sure the handles released by h2o_timeout_dispose get freed */
    uv_run(ctx->loop, UV_RUN_NOWAIT);
#endif
}

void h2o_context_request_shutdown(h2o_context_t *ctx)
{
    ctx->shutdown_requested = 1;
    if (ctx->globalconf->http1.callbacks.request_shutdown != NULL)
        ctx->globalconf->http1.callbacks.request_shutdown(ctx);
    if (ctx->globalconf->http2.callbacks.request_shutdown != NULL)
        ctx->globalconf->http2.callbacks.request_shutdown(ctx);
}

void h2o_context_update_timestamp_cache(h2o_context_t *ctx)
{
    time_t prev_sec = ctx->_timestamp_cache.tv_at.tv_sec;
    ctx->_timestamp_cache.uv_now_at = h2o_now(ctx->loop);
    gettimeofday(&ctx->_timestamp_cache.tv_at, NULL);
    if (ctx->_timestamp_cache.tv_at.tv_sec != prev_sec) {
        struct tm gmt;
        /* update the string cache */
        if (ctx->_timestamp_cache.value != NULL)
            h2o_mem_release_shared(ctx->_timestamp_cache.value);
        ctx->_timestamp_cache.value = h2o_mem_alloc_shared(NULL, sizeof(h2o_timestamp_string_t), NULL);
        gmtime_r(&ctx->_timestamp_cache.tv_at.tv_sec, &gmt);
        h2o_time2str_rfc1123(ctx->_timestamp_cache.value->rfc1123, &gmt);
        h2o_time2str_log(ctx->_timestamp_cache.value->log, ctx->_timestamp_cache.tv_at.tv_sec);
    }
}

void h2o_context_report_emitted_error(h2o_context_t *ctx, int status, int http)
{
    int s100;

#define INC_PREDEF_ERROR(x) \
    do { \
        if (status == x) { \
            ctx->emitted_errors.emitted_errors_cnt[E_HTTP_ ## x]++; \
            return; \
        } \
    } while(0)

    if (http == 1) {
        INC_PREDEF_ERROR(400);
        INC_PREDEF_ERROR(403);
        INC_PREDEF_ERROR(404);
        INC_PREDEF_ERROR(405);
        INC_PREDEF_ERROR(416);
        INC_PREDEF_ERROR(417);
        INC_PREDEF_ERROR(500);
        INC_PREDEF_ERROR(502);
        INC_PREDEF_ERROR(503);

#undef INC_PREDEF_ERROR

        s100 = status / 100;
        switch(s100) {
            case 4:
                ctx->emitted_errors.emitted_errors_cnt[E_HTTP_4XX]++;
                return;
            case 5:
                ctx->emitted_errors.emitted_errors_cnt[E_HTTP_5XX]++;
                return;
            default:
                ctx->emitted_errors.emitted_errors_cnt[E_HTTP_XXX]++;
                return;
        }
    } else {
#define INC_PREDEF_ERROR2(name, x) \
    do { \
        if (status == x) { \
            ctx->emitted_errors.emitted_errors_cnt[E_HTTP2_ ## name]++; \
            return; \
        } \
    } while(0)

        INC_PREDEF_ERROR2(PROTOCOL, -1);
        INC_PREDEF_ERROR2(INTERNAL, -2);
        INC_PREDEF_ERROR2(FLOW_CONTROL, -3);
        INC_PREDEF_ERROR2(SETTINGS_TIMEOUT, -4);
        INC_PREDEF_ERROR2(STREAM_CLOSED, -5);
        INC_PREDEF_ERROR2(FRAME_SIZE, -6);
        INC_PREDEF_ERROR2(REFUSED_STREAM, -7);
        INC_PREDEF_ERROR2(CANCEL, -8);
        INC_PREDEF_ERROR2(COMPRESSION, -9);
        INC_PREDEF_ERROR2(CONNECT, -10);
        INC_PREDEF_ERROR2(ENHANCE_YOUR_CALM, -11);
        INC_PREDEF_ERROR2(INADEQUATE_SECURITY, -12);

        ctx->emitted_errors.emitted_errors_cnt[E_HTTP2_OTHER]++;
        return;
#undef INC_PREDEF_ERROR
    }
}

/*
 * Copyright (c) 2010, FRiCKLE Piotr Sikora <info@frickle.com>
 * Copyright (c) 2009-2010, Xiaozhe Wang <chaoslawful@gmail.com>
 * Copyright (c) 2009-2010, Yichun Zhang <agentzh@gmail.com>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * HOLDERS OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "ngx_postgres_handler.h"
#include "ngx_postgres_module.h"
#include "ngx_postgres_output.h"
#include "ngx_postgres_processor.h"
#include "ngx_postgres_util.h"


static void ngx_postgres_wev_handler(ngx_http_request_t *r, ngx_http_upstream_t *u);
static void ngx_postgres_rev_handler(ngx_http_request_t *r, ngx_http_upstream_t *u);
static ngx_int_t ngx_postgres_create_request(ngx_http_request_t *r);
static ngx_int_t ngx_postgres_reinit_request(ngx_http_request_t *r);
static void ngx_postgres_abort_request(ngx_http_request_t *r);
static void ngx_postgres_finalize_request(ngx_http_request_t *r, ngx_int_t rc);
static ngx_int_t ngx_postgres_process_header(ngx_http_request_t *r);
static ngx_int_t ngx_postgres_input_filter_init(void *data);
static ngx_int_t ngx_postgres_input_filter(void *data, ssize_t bytes);
static ngx_http_upstream_srv_conf_t *ngx_postgres_find_upstream(ngx_http_request_t *, ngx_url_t *);


ngx_int_t ngx_postgres_handler(ngx_http_request_t *r) {
    /* TODO: add support for subrequest in memory by emitting output into u->buffer instead */
    if (r->subrequest_in_memory) { ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "postgres: ngx_postgres module does not support subrequests in memory"); return NGX_HTTP_INTERNAL_SERVER_ERROR; }
    ngx_postgres_location_conf_t *location_conf = ngx_http_get_module_loc_conf(r, ngx_postgres_module);
    if (!location_conf->query && !(location_conf->methods_set & r->method)) {
        if (location_conf->methods_set) return NGX_HTTP_NOT_ALLOWED;
        ngx_http_core_loc_conf_t *core_loc_conf = ngx_http_get_module_loc_conf(r, ngx_http_core_module);
        ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "postgres: missing \"postgres_query\" in location \"%V\"", &core_loc_conf->name);
        return NGX_HTTP_INTERNAL_SERVER_ERROR;
    }
    ngx_int_t rc = ngx_http_discard_request_body(r);
    if (rc != NGX_OK) return rc;
    if (ngx_http_upstream_create(r) != NGX_OK) { ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "%s:%d", __FILE__, __LINE__); return NGX_HTTP_INTERNAL_SERVER_ERROR; }
    ngx_http_upstream_t *u = r->upstream;
    if (location_conf->upstream_cv) { /* use complex value */
        ngx_str_t host;
        if (ngx_http_complex_value(r, location_conf->upstream_cv, &host) != NGX_OK) { ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "%s:%d", __FILE__, __LINE__); return NGX_HTTP_INTERNAL_SERVER_ERROR; }
        if (!host.len) {
            ngx_http_core_loc_conf_t *core_loc_conf = ngx_http_get_module_loc_conf(r, ngx_http_core_module);
            ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "postgres: empty \"postgres_pass\" (was: \"%V\") in location \"%V\"", &location_conf->upstream_cv->value, &core_loc_conf->name);
            return NGX_HTTP_INTERNAL_SERVER_ERROR;
        }
        ngx_url_t url;
        ngx_memzero(&url, sizeof(ngx_url_t));
        url.host = host;
        url.no_resolve = 1;
        if (!(location_conf->upstream.upstream = ngx_postgres_find_upstream(r, &url))) { ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "postgres: upstream name \"%V\" not found", &host); return NGX_HTTP_INTERNAL_SERVER_ERROR; }
    }
    ngx_postgres_context_t *context = ngx_pcalloc(r->pool, sizeof(ngx_postgres_context_t));
    if (!context) { ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "%s:%d", __FILE__, __LINE__); return NGX_HTTP_INTERNAL_SERVER_ERROR; }
    context->var_cols = NGX_ERROR;
    context->var_rows = NGX_ERROR;
    context->var_affected = NGX_ERROR;
    if (location_conf->variables) {
        if (!(context->variables = ngx_array_create(r->pool, location_conf->variables->nelts, sizeof(ngx_str_t)))) { ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "%s:%d", __FILE__, __LINE__); return NGX_HTTP_INTERNAL_SERVER_ERROR; }
        /* fake ngx_array_push'ing */
        context->variables->nelts = location_conf->variables->nelts;
        ngx_memzero(context->variables->elts, context->variables->nelts * context->variables->size);
    }
    ngx_http_set_ctx(r, context, ngx_postgres_module);
    u->schema.len = sizeof("postgres://") - 1;
    u->schema.data = (u_char *) "postgres://";
    u->output.tag = (ngx_buf_tag_t) &ngx_postgres_module;
    u->conf = &location_conf->upstream;
    u->create_request = ngx_postgres_create_request;
    u->reinit_request = ngx_postgres_reinit_request;
    u->process_header = ngx_postgres_process_header;
    u->abort_request = ngx_postgres_abort_request;
    u->finalize_request = ngx_postgres_finalize_request;
    /* we bypass the upstream input filter mechanism in ngx_http_upstream_process_headers */
    u->input_filter_init = ngx_postgres_input_filter_init;
    u->input_filter = ngx_postgres_input_filter;
    u->input_filter_ctx = NULL;
    r->main->count++;
    ngx_http_upstream_init(r);
    /* override the read/write event handler to our own */
    u->write_event_handler = ngx_postgres_wev_handler;
    u->read_event_handler = ngx_postgres_rev_handler;
    /* a bit hack-ish way to return error response (clean-up part) */
    if (u->peer.connection && !u->peer.connection->fd) {
        ngx_connection_t *c = u->peer.connection;
        u->peer.connection = NULL;
        if (c->write->timer_set) ngx_del_timer(c->write);
        if (c->pool) ngx_destroy_pool(c->pool);
        ngx_free_connection(c);
        ngx_postgres_upstream_finalize_request(r, u, NGX_HTTP_SERVICE_UNAVAILABLE);
    }
    return NGX_DONE;
}


static void ngx_postgres_wev_handler(ngx_http_request_t *r, ngx_http_upstream_t *u) {
    u->request_sent = 1; /* just to ensure u->reinit_request always gets called for upstream_next */
    if (u->peer.connection->write->timedout) { ngx_postgres_upstream_next(r, u, NGX_HTTP_UPSTREAM_FT_TIMEOUT); return; }
    if (ngx_postgres_upstream_test_connect(u->peer.connection) != NGX_OK) { ngx_postgres_upstream_next(r, u, NGX_HTTP_UPSTREAM_FT_ERROR); return; }
    ngx_postgres_process_events(r);
}


static void ngx_postgres_rev_handler(ngx_http_request_t *r, ngx_http_upstream_t *u) {
    u->request_sent = 1; /* just to ensure u->reinit_request always gets called for upstream_next */
    if (u->peer.connection->read->timedout) { ngx_postgres_upstream_next(r, u, NGX_HTTP_UPSTREAM_FT_TIMEOUT); return; }
    if (ngx_postgres_upstream_test_connect(u->peer.connection) != NGX_OK) { ngx_postgres_upstream_next(r, u, NGX_HTTP_UPSTREAM_FT_ERROR); return; }
    ngx_postgres_process_events(r);
}


static ngx_int_t ngx_postgres_create_request(ngx_http_request_t *r) {
    r->upstream->request_bufs = NULL;
    return NGX_OK;
}


static ngx_int_t ngx_postgres_reinit_request(ngx_http_request_t *r) {
    ngx_http_upstream_t *u = r->upstream;
    /* override the read/write event handler to our own */
    u->write_event_handler = ngx_postgres_wev_handler;
    u->read_event_handler = ngx_postgres_rev_handler;
    return NGX_OK;
}


static void ngx_postgres_abort_request(ngx_http_request_t *r) { }


static void ngx_postgres_finalize_request(ngx_http_request_t *r, ngx_int_t rc) {
    if (rc == NGX_OK) ngx_postgres_output_chain(r);
}


static ngx_int_t ngx_postgres_process_header(ngx_http_request_t *r) {
    ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "postgres: ngx_postgres_process_header should not be called by the upstream");
    return NGX_ERROR;
}


static ngx_int_t ngx_postgres_input_filter_init(void *data) {
    ngx_http_request_t *r = data;
    ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "postgres: ngx_postgres_input_filter_init should not be called by the upstream");
    return NGX_ERROR;
}


static ngx_int_t ngx_postgres_input_filter(void *data, ssize_t bytes) {
    ngx_http_request_t *r = data;
    ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "postgres: ngx_postgres_input_filter should not be called by the upstream");
    return NGX_ERROR;
}


ngx_http_upstream_srv_conf_t *ngx_postgres_find_upstream(ngx_http_request_t *r, ngx_url_t *url) {
    ngx_http_upstream_main_conf_t *umcf = ngx_http_get_module_main_conf(r, ngx_http_upstream_module);
    ngx_http_upstream_srv_conf_t **uscfp = umcf->upstreams.elts;
    for (ngx_uint_t i = 0; i < umcf->upstreams.nelts; i++) {
        if (uscfp[i]->host.len != url->host.len || ngx_strncasecmp(uscfp[i]->host.data, url->host.data, url->host.len)) continue;
        return uscfp[i];
    }
    return NULL;
}

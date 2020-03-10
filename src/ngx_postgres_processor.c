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


#include <postgresql/server/catalog/pg_type_d.h>
#include <avcall.h>

#include "ngx_postgres_handler.h"
#include "ngx_postgres_module.h"
#include "ngx_postgres_output.h"
#include "ngx_postgres_processor.h"
#include "ngx_postgres_upstream.h"
#include "ngx_postgres_variable.h"


#if (NGX_DEBUG)
static const char *PostgresPollingStatusType2string(PostgresPollingStatusType status) {
    switch (status) {
        case PGRES_POLLING_FAILED: return "PGRES_POLLING_FAILED";
        case PGRES_POLLING_READING: return "PGRES_POLLING_READING";
        case PGRES_POLLING_WRITING: return "PGRES_POLLING_WRITING";
        case PGRES_POLLING_OK: return "PGRES_POLLING_OK";
        case PGRES_POLLING_ACTIVE: return "PGRES_POLLING_ACTIVE";
        default: return NULL;
    }
    return NULL;
}


static const char *ConnStatusType2string(ConnStatusType status) {
    switch (status) {
        case CONNECTION_OK: return "CONNECTION_OK";
        case CONNECTION_BAD: return "CONNECTION_BAD";
        case CONNECTION_STARTED: return "CONNECTION_STARTED";
        case CONNECTION_MADE: return "CONNECTION_MADE";
        case CONNECTION_AWAITING_RESPONSE: return "CONNECTION_AWAITING_RESPONSE";
        case CONNECTION_AUTH_OK: return "CONNECTION_AUTH_OK";
        case CONNECTION_SETENV: return "CONNECTION_SETENV";
        case CONNECTION_SSL_STARTUP: return "CONNECTION_SSL_STARTUP";
        case CONNECTION_NEEDED: return "CONNECTION_NEEDED";
        case CONNECTION_CHECK_WRITABLE: return "CONNECTION_CHECK_WRITABLE";
        case CONNECTION_CONSUME: return "CONNECTION_CONSUME";
        default: return NULL;
    }
    return NULL;
}
#endif


static ngx_int_t ngx_postgres_done(ngx_http_request_t *r) {
    ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0, "%s", __func__);
    r->upstream->headers_in.status_n = NGX_HTTP_OK; /* flag for keepalive */
    ngx_postgres_data_t *pd = r->upstream->peer.data;
    ngx_postgres_finalize_upstream(r, r->upstream, pd->status >= NGX_HTTP_SPECIAL_RESPONSE ? pd->status : NGX_OK);
    pd->state = state_db_idle;
    return NGX_DONE;
}


static ngx_int_t ngx_postgres_send_query(ngx_http_request_t *r) {
    ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0, "%s", __func__);
    ngx_postgres_data_t *pd = r->upstream->peer.data;
    if (!PQconsumeInput(pd->common.conn)) { ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "failed to consume input: %s", PQerrorMessage(pd->common.conn)); return NGX_ERROR; }
    if (PQisBusy(pd->common.conn)) { ngx_log_debug0(NGX_LOG_DEBUG_HTTP, r->connection->log, 0, "busy while send query"); return NGX_AGAIN; }
    if (pd->state == state_db_connect || pd->state == state_db_idle) {
        ngx_postgres_location_conf_t *location_conf = ngx_http_get_module_loc_conf(r, ngx_postgres_module);
        ngx_postgres_query_t *query = location_conf->query;
        ngx_str_t sql;
        sql.len = query->sql.len - 2 * query->ids->nelts - query->percent;
    //    ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0, "sql = `%V`", &query->sql);
        ngx_str_t *ids = NULL;
        if (query->ids->nelts) {
            ngx_uint_t *id = query->ids->elts;
            if (!(ids = ngx_pnalloc(r->pool, query->ids->nelts * sizeof(ngx_str_t)))) { ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "!ngx_pnalloc"); return NGX_ERROR; }
            for (ngx_uint_t i = 0; i < query->ids->nelts; i++) {
                ngx_http_variable_value_t *value = ngx_http_get_indexed_variable(r, id[i]);
                if (!value || !value->data || !value->len) { ngx_str_set(&ids[i], "NULL"); } else {
                    char *str = PQescapeIdentifier(pd->common.conn, (const char *)value->data, value->len);
                    if (!str) { ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "failed to escape %*.*s: %s", value->len, value->len, value->data, PQerrorMessage(pd->common.conn)); return NGX_ERROR; }
                    ngx_str_t id = {ngx_strlen(str), NULL};
                    if (!(id.data = ngx_pnalloc(r->pool, id.len))) { ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "!ngx_pnalloc"); PQfreemem(str); return NGX_ERROR; }
                    ngx_memcpy(id.data, str, id.len);
                    PQfreemem(str);
                    ids[i] = id;
                    if (!ids[i].len) { ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "failed to escape %*.*s", value->len, value->len, value->data); return NGX_ERROR; }
                }
                sql.len += ids[i].len;
            }
        }
        if (!(sql.data = ngx_pnalloc(r->pool, sql.len + 1))) { ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "!ngx_pnalloc"); return NGX_ERROR; }
        av_alist alist;
        u_char *last = NULL;
        av_start_ptr(alist, &ngx_snprintf, u_char *, &last);
        if (av_ptr(alist, u_char *, sql.data)) { ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "av_ptr"); return NGX_ERROR; }
        if (av_ulong(alist, sql.len)) { ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "av_ulong"); return NGX_ERROR; }
        if (av_ptr(alist, char *, query->sql.data)) { ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "av_ptr"); return NGX_ERROR; }
        for (ngx_uint_t i = 0; i < query->ids->nelts; i++) if (av_ptr(alist, ngx_str_t *, &ids[i])) { ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "av_ptr"); return NGX_ERROR; }
        if (av_call(alist)) { ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "av_call"); return NGX_ERROR; }
        if (last != sql.data + sql.len) { ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "ngx_snprintf"); return NGX_ERROR; }
        *last = '\0';
    //    ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0, "sql = `%V`", &sql);
        pd->sql = sql; /* set $postgres_query */
        if (pd->common.server_conf->prepare && !query->listen) {
            if (!(pd->stmtName = ngx_pnalloc(r->pool, 32))) { ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "ngx_pnalloc"); return NGX_ERROR; }
            u_char *last = ngx_snprintf(pd->stmtName, 31, "ngx_%ul", (unsigned long)(pd->hash = ngx_hash_key(sql.data, sql.len)));
            *last = '\0';
        }
        pd->state = pd->common.server_conf->prepare ? state_db_send_prepare : state_db_send_query;
    }
    for (PGresult *res; (res = PQgetResult(pd->common.conn)); PQclear(res)) {
        if (PQresultStatus(res) == PGRES_FATAL_ERROR) {
            ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "received error on send query: %s: %s", PQresStatus(PQresultStatus(res)), PQresultErrorMessage(res));
            PQclear(res);
            pd->status = NGX_HTTP_INTERNAL_SERVER_ERROR;
            if (pd->stmtName && pd->common.prepare) {
                for (ngx_queue_t *queue = ngx_queue_head(pd->common.prepare); queue != ngx_queue_sentinel(pd->common.prepare); queue = ngx_queue_next(queue)) {
                    ngx_postgres_prepare_t *prepare = ngx_queue_data(queue, ngx_postgres_prepare_t, queue);
                    if (prepare->hash == pd->hash) { ngx_queue_remove(queue); break; }
                }
            }
            return ngx_postgres_done(r);
        }
        ngx_log_debug2(NGX_LOG_DEBUG_HTTP, r->connection->log, 0, "received result on send query: %s: %s", PQresStatus(PQresultStatus(res)), PQresultErrorMessage(res));
    }
    if (!pd->stmtName) {
        if (!PQsendQueryParams(pd->common.conn, (const char *)pd->sql.data, pd->nParams, pd->paramTypes, (const char *const *)pd->paramValues, NULL, NULL, pd->resultFormat)) { ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "failed to send query: %s", PQerrorMessage(pd->common.conn)); return NGX_ERROR; }
        ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0, "query %s sent successfully", pd->sql.data);
    } else switch (pd->state) {
        case state_db_send_prepare: {
            ngx_uint_t hash = 0;
            if (pd->common.prepare) for (ngx_queue_t *queue = ngx_queue_head(pd->common.prepare); queue != ngx_queue_sentinel(pd->common.prepare); queue = ngx_queue_next(queue)) {
                ngx_postgres_prepare_t *prepare = ngx_queue_data(queue, ngx_postgres_prepare_t, queue);
                if (prepare->hash == pd->hash) { hash = prepare->hash; break; }
            }
            if (hash) pd->state = state_db_send_query; else {
                if (!PQsendPrepare(pd->common.conn, (const char *)pd->stmtName, (const char *)pd->sql.data, pd->nParams, pd->paramTypes)) { ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "failed to send prepare: %s", PQerrorMessage(pd->common.conn)); return NGX_ERROR; }
                ngx_log_debug2(NGX_LOG_DEBUG_HTTP, r->connection->log, 0, "prepare %s:%s sent successfully", pd->stmtName, pd->sql.data);
                if (!pd->common.prepare) {
                    if (!(pd->common.prepare = ngx_pcalloc(r->upstream->peer.connection->pool, sizeof(ngx_queue_t)))) { ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "!ngx_pcalloc"); return NGX_ERROR; }
                    ngx_queue_init(pd->common.prepare);
                }
                ngx_postgres_prepare_t *prepare = ngx_pcalloc(r->upstream->peer.connection->pool, sizeof(ngx_postgres_prepare_t));
                if (!prepare) { ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "!ngx_pcalloc"); return NGX_ERROR; }
                prepare->hash = pd->hash;
                ngx_queue_insert_head(pd->common.prepare, &prepare->queue);
                pd->state = state_db_send_query;
                return NGX_DONE;
            }
        } // fall through
        case state_db_send_query: {
            if (!PQsendQueryPrepared(pd->common.conn, (const char *)pd->stmtName, pd->nParams, (const char *const *)pd->paramValues, NULL, NULL, pd->resultFormat)) { ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "failed to send prepared query: %s", PQerrorMessage(pd->common.conn)); return NGX_ERROR; }
            ngx_log_debug2(NGX_LOG_DEBUG_HTTP, r->connection->log, 0, "query %s:%s sent successfully", pd->stmtName, pd->sql.data);
        } break;
        default: { ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "pd->state"); return NGX_ERROR; }
    }
    ngx_add_timer(r->upstream->peer.connection->read, r->upstream->conf->read_timeout); /* set result timeout */
    pd->state = state_db_get_result;
    return NGX_DONE;
}


static ngx_int_t ngx_postgres_connect(ngx_http_request_t *r) {
    ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0, "%s", __func__);
    ngx_postgres_data_t *pd = r->upstream->peer.data;
    PostgresPollingStatusType poll_status = PQconnectPoll(pd->common.conn);
    ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0, "polling while connecting, %s", PostgresPollingStatusType2string(poll_status));
    if (poll_status == PGRES_POLLING_READING || poll_status == PGRES_POLLING_WRITING) {
        if (PQstatus(pd->common.conn) == CONNECTION_MADE && r->upstream->peer.connection->write->ready) {
            ngx_log_debug0(NGX_LOG_DEBUG_HTTP, r->connection->log, 0, "re-polling while connecting");
            return ngx_postgres_connect(r);
        }
        switch (PQstatus(pd->common.conn)) {
            case CONNECTION_NEEDED: ngx_log_debug0(NGX_LOG_DEBUG_HTTP, r->connection->log, 0, "CONNECTION_NEEDED"); break;
            case CONNECTION_STARTED: ngx_log_debug0(NGX_LOG_DEBUG_HTTP, r->connection->log, 0, "CONNECTION_STARTED"); break;
            case CONNECTION_MADE: ngx_log_debug0(NGX_LOG_DEBUG_HTTP, r->connection->log, 0, "CONNECTION_MADE"); break;
            case CONNECTION_AWAITING_RESPONSE: ngx_log_debug0(NGX_LOG_DEBUG_HTTP, r->connection->log, 0, "CONNECTION_AWAITING_RESPONSE"); break;
            case CONNECTION_AUTH_OK: ngx_log_debug0(NGX_LOG_DEBUG_HTTP, r->connection->log, 0, "CONNECTION_AUTH_OK"); break;
            case CONNECTION_SETENV: ngx_log_debug0(NGX_LOG_DEBUG_HTTP, r->connection->log, 0, "CONNECTION_SETENV"); break;
            case CONNECTION_SSL_STARTUP: ngx_log_debug0(NGX_LOG_DEBUG_HTTP, r->connection->log, 0, "CONNECTION_SSL_STARTUP"); break;
            default: ngx_log_debug1(NGX_LOG_ERR, r->connection->log, 0, "unknown state: %s", ConnStatusType2string(PQstatus(pd->common.conn))); return NGX_ERROR;
        }
        ngx_log_debug0(NGX_LOG_DEBUG_HTTP, r->connection->log, 0, "busy while connecting");
        return NGX_AGAIN;
    }
    if (r->upstream->peer.connection->write->timer_set) ngx_del_timer(r->upstream->peer.connection->write); /* remove connection timeout from new connection */
    if (poll_status != PGRES_POLLING_OK) { ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "connection failed: %s", PQerrorMessage(pd->common.conn)); return NGX_ERROR; }
    ngx_log_debug0(NGX_LOG_DEBUG_HTTP, r->connection->log, 0, "connected successfully");
    const char *charset = PQparameterStatus(pd->common.conn, "client_encoding");
    if (charset) {
        pd->common.charset.len = ngx_strlen(charset);
        if (pd->common.charset.len == sizeof("UTF8") - 1 && !ngx_strncasecmp((u_char *)charset, (u_char *)"UTF8", sizeof("UTF8") - 1)) {
            ngx_str_set(&pd->common.charset, "utf-8");
        } else {
            if (!(pd->common.charset.data = ngx_pnalloc(r->pool, pd->common.charset.len))) { ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "!ngx_pnalloc"); return NGX_ERROR; }
            ngx_memcpy(pd->common.charset.data, charset, pd->common.charset.len);
        }
    }
    return ngx_postgres_send_query(r);
}


static ngx_int_t ngx_postgres_process_response(ngx_http_request_t *r) {
    ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0, "%s", __func__);
    if (ngx_postgres_variable_set(r) == NGX_ERROR) {
        ngx_postgres_data_t *pd = r->upstream->peer.data;
        pd->status = NGX_HTTP_INTERNAL_SERVER_ERROR;
        ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "ngx_postgres_variable_set == NGX_ERROR");
        return NGX_DONE;
    }
    ngx_postgres_location_conf_t *location_conf = ngx_http_get_module_loc_conf(r, ngx_postgres_module);
    if (location_conf->output.handler) return location_conf->output.handler(r);
    return NGX_DONE;
}


static ngx_int_t ngx_postgres_get_ack(ngx_http_request_t *r) {
    ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0, "%s", __func__);
    ngx_postgres_data_t *pd = r->upstream->peer.data;
    if (!PQconsumeInput(pd->common.conn)) { ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "failed to consume input: %s", PQerrorMessage(pd->common.conn)); return NGX_ERROR; }
    if (PQisBusy(pd->common.conn)) { ngx_log_debug0(NGX_LOG_DEBUG_HTTP, r->connection->log, 0, "busy while get ack"); pd->state = state_db_get_ack; return NGX_AGAIN; }
    if (r->upstream->peer.connection->read->timer_set) ngx_del_timer(r->upstream->peer.connection->read); /* remove result timeout */
    PGresult *res = PQgetResult(pd->common.conn);
    if (res) {
        ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "receiving ACK failed: multiple queries(?)");
        PQclear(res);
        pd->status = NGX_HTTP_INTERNAL_SERVER_ERROR;
    }
    return ngx_postgres_done(r);
}


static ngx_int_t ngx_postgres_get_result(ngx_http_request_t *r) {
    ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0, "%s", __func__);
    ngx_postgres_data_t *pd = r->upstream->peer.data;
    if (r->upstream->peer.connection->write->timer_set) ngx_del_timer(r->upstream->peer.connection->write); /* remove connection timeout from re-used keepalive connection */
    if (!PQconsumeInput(pd->common.conn)) { ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "failed to consume input: %s", PQerrorMessage(pd->common.conn)); return NGX_ERROR; }
    if (PQisBusy(pd->common.conn)) { ngx_log_debug0(NGX_LOG_DEBUG_HTTP, r->connection->log, 0, "busy while receiving result"); return NGX_AGAIN; }
    PGresult *res = PQgetResult(pd->common.conn);
    if (!res) { ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "failed to receive result: %s", PQerrorMessage(pd->common.conn)); return NGX_ERROR; }
    if (PQresultStatus(res) != PGRES_COMMAND_OK && PQresultStatus(res) != PGRES_TUPLES_OK) {
        ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "failed to receive result: %s: %s", PQresStatus(PQresultStatus(res)), PQresultErrorMessage(res));
        PQclear(res);
        pd->status = NGX_HTTP_INTERNAL_SERVER_ERROR;
        goto ret;
    }
    ngx_log_debug2(NGX_LOG_DEBUG_HTTP, r->connection->log, 0, "result received successfully, cols:%d rows:%d", PQnfields(res), PQntuples(res));
    pd->res = res;
    ngx_int_t rc = ngx_postgres_process_response(r);
    PQclear(res);
    if (rc != NGX_DONE) return rc;
ret:
    return ngx_postgres_get_ack(r);
}


void ngx_postgres_process_events(ngx_http_request_t *r) {
    ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0, "%s", __func__);
    if (!ngx_postgres_is_my_peer(&r->upstream->peer)) { ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "trying to connect to something that is not PostgreSQL database"); goto failed; }
    ngx_postgres_data_t *pd = r->upstream->peer.data;
    ngx_int_t rc;
    switch (pd->state) {
        case state_db_connect: ngx_log_debug0(NGX_LOG_DEBUG_HTTP, r->connection->log, 0, "state_db_connect"); rc = ngx_postgres_connect(r); break;
        case state_db_send_prepare: ngx_log_debug0(NGX_LOG_DEBUG_HTTP, r->connection->log, 0, "state_db_send_prepare"); rc = ngx_postgres_send_query(r); break;
        case state_db_send_query: ngx_log_debug0(NGX_LOG_DEBUG_HTTP, r->connection->log, 0, "state_db_send_query"); rc = ngx_postgres_send_query(r); break;
        case state_db_get_result: ngx_log_debug0(NGX_LOG_DEBUG_HTTP, r->connection->log, 0, "state_db_get_result"); rc = ngx_postgres_get_result(r); break;
        case state_db_get_ack: ngx_log_debug0(NGX_LOG_DEBUG_HTTP, r->connection->log, 0, "state_db_get_ack"); rc = ngx_postgres_get_ack(r); break;
        case state_db_idle: ngx_log_debug0(NGX_LOG_DEBUG_HTTP, r->connection->log, 0, "state_db_idle, re-using keepalive connection"); /*pd->state = pd->common.server_conf->prepare ? state_db_send_prepare : state_db_send_query; */rc = ngx_postgres_send_query(r); break;
        default: ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "unknown state: %d", pd->state); goto failed;
    }
    if (rc >= NGX_HTTP_SPECIAL_RESPONSE) ngx_postgres_finalize_upstream(r, r->upstream, rc);
    else if (rc == NGX_ERROR) goto failed;
    return;
failed:
    ngx_postgres_next_upstream(r, r->upstream, NGX_HTTP_UPSTREAM_FT_ERROR);
}

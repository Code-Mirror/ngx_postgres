#include <postgresql/server/catalog/pg_type_d.h>

#include "ngx_postgres_handler.h"
#include "ngx_postgres_module.h"
#include "ngx_postgres_processor.h"
#include "ngx_postgres_upstream.h"


static void ngx_postgres_save_to_free(ngx_postgres_data_t *pd, ngx_postgres_save_t *ps) {
    ngx_http_request_t *r = pd->request;
    ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0, "%s", __func__);
    ngx_queue_remove(&ps->queue);
    ngx_queue_insert_tail(&ps->common.server->free, &ps->queue);
    pd->common = ps->common;
    ngx_peer_connection_t *pc = &r->upstream->peer;
    ngx_postgres_common_t *common = &pd->common;
    ngx_connection_t *c = pc->connection = common->connection;
    c->data = NULL;
    c->idle = 0;
    c->log_error = pc->log_error;
    c->log = pc->log;
    if (c->pool) c->pool->log = pc->log;
    c->read->log = pc->log;
    if (c->read->timer_set) ngx_del_timer(c->read);
    c->sent = 0;
    c->write->log = pc->log;
    pc->cached = 1;
    pc->name = &common->name;
    pc->sockaddr = common->sockaddr;
    pc->socklen = common->socklen;
}


static ngx_int_t ngx_postgres_peer_multi(ngx_http_request_t *r) {
    ngx_postgres_data_t *pd = r->upstream->peer.data;
    ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0, "%s", __func__);
    ngx_postgres_common_t *common = &pd->common;
    ngx_postgres_server_t *server = common->server;
    for (ngx_queue_t *queue = ngx_queue_head(&server->save); queue != ngx_queue_sentinel(&server->save); queue = ngx_queue_next(queue)) {
        ngx_postgres_save_t *ps = ngx_queue_data(queue, ngx_postgres_save_t, queue);
        if (ngx_memn2cmp((u_char *)pd->common.sockaddr, (u_char *)ps->common.sockaddr, pd->common.socklen, ps->common.socklen)) continue;
        ngx_postgres_save_to_free(pd, ps);
        return NGX_DONE;
    }
    return NGX_DECLINED;
}


static ngx_int_t ngx_postgres_peer_get(ngx_peer_connection_t *pc, void *data) {
    ngx_postgres_data_t *pd = pc->data;
    ngx_http_request_t *r = pd->request;
    ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0, "%s", __func__);
    ngx_postgres_common_t *common = &pd->common;
    ngx_postgres_server_t *server = common->server;
    ngx_queue_t *queue = ngx_queue_head(&server->peer);
    ngx_postgres_peer_t *peer = ngx_queue_data(queue, ngx_postgres_peer_t, queue);
    ngx_queue_remove(&peer->queue);
    ngx_queue_insert_tail(&server->peer, &peer->queue);
//    pc->cached = 0;
    common->name = peer->name;
    common->sockaddr = peer->sockaddr;
    common->socklen = peer->socklen;
    if (server->nsave) {
        ngx_log_debug0(NGX_LOG_DEBUG_HTTP, r->connection->log, 0, "nsave");
        if (ngx_postgres_peer_multi(r) != NGX_DECLINED) { ngx_postgres_process_events(r); return NGX_AGAIN; }
        if (ngx_queue_empty(&server->free)) {
            ngx_log_error(NGX_LOG_WARN, r->connection->log, 0, "ngx_queue_empty(free)");
#ifdef NGX_YIELD
            if (server->ndata) {
                ngx_log_error(NGX_LOG_INFO, r->connection->log, 0, "pd = %p", pd);
                ngx_queue_insert_tail(&server->data, &pd->queue);
                ngx_add_timer(&pd->timeout, server->timeout);
                return NGX_YIELD;
            }
#endif
//        ngx_log_error(NGX_LOG_WARN, r->connection->log, 0, "nsave and ngx_queue_empty(free)");
            return NGX_DECLINED;
        }
    }
    const char *host = peer->values[0];
    peer->values[0] = (const char *)peer->value;
    const char *options = peer->values[2];
    ngx_postgres_location_t *location = ngx_http_get_module_loc_conf(r, ngx_postgres_module);
    if (location->output.append) {
        size_t len = options ? ngx_strlen(options) : 0;
        u_char *buf = ngx_pnalloc(r->pool, len + (len ? 1 : 0) + sizeof("-c config.append_type_to_column_name=true") - 1 + 1);
        if (!buf) { ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "!ngx_pnalloc"); return NGX_DECLINED; }
        u_char *p = buf;
        if (options) {
            p = ngx_copy(p, options, len);
            *p++ = ' ';
        }
        p = ngx_copy(p, "-c config.append_type_to_column_name=true", sizeof("-c config.append_type_to_column_name=true") - 1);
        *p = '\0';
        peer->values[2] = (const char *)buf;
    }
    common->conn = PQconnectStartParams(peer->keywords, peer->values, 0); /* internal checks in PQsetnonblocking are taking care of any PQconnectStart failures, so we don't need to check them here. */
    peer->values[0] = host;
    peer->values[2] = options;
    if (PQstatus(common->conn) == CONNECTION_BAD || PQsetnonblocking(common->conn, 1) == -1) {
        ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "PQstatus == CONNECTION_BAD or PQsetnonblocking == -1 and %s in upstream \"%V\"", PQerrorMessageMy(common->conn), peer->name);
        PQfinish(common->conn);
        common->conn = NULL;
        return NGX_DECLINED;
    }
//    PQtrace(common->conn, stderr);
    int fd;
    if ((fd = PQsocket(common->conn)) == -1) { ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "PQsocket == -1"); goto invalid; }
    ngx_connection_t *c = ngx_get_connection(fd, pc->log);
    if (!(common->connection = c)) { ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "!ngx_get_connection"); goto invalid; }
    c->log_error = pc->log_error;
    c->log = pc->log;
    c->number = ngx_atomic_fetch_add(ngx_connection_counter, 1);
    if (c->pool) c->pool->log = pc->log;
    c->read->log = pc->log;
    c->write->log = pc->log;
    /* register the connection with postgres connection fd into the nginx event model */
    if (ngx_event_flags & NGX_USE_RTSIG_EVENT) {
        if (ngx_add_conn(c) != NGX_OK) { ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "ngx_add_conn != NGX_OK"); goto invalid; }
    } else if (ngx_event_flags & NGX_USE_CLEAR_EVENT) {
        if (ngx_add_event(c->read, NGX_READ_EVENT, NGX_CLEAR_EVENT) != NGX_OK) { ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "ngx_add_event != NGX_OK"); goto invalid; }
        if (ngx_add_event(c->write, NGX_WRITE_EVENT, NGX_CLEAR_EVENT) != NGX_OK) { ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "ngx_add_event != NGX_OK"); goto invalid; }
    } else if (ngx_event_flags & NGX_USE_LEVEL_EVENT) {
        if (ngx_add_event(c->read, NGX_READ_EVENT, NGX_LEVEL_EVENT) != NGX_OK) { ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "ngx_add_event != NGX_OK"); goto invalid; }
        if (ngx_add_event(c->write, NGX_WRITE_EVENT, NGX_LEVEL_EVENT) != NGX_OK) { ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "ngx_add_event != NGX_OK"); goto invalid; }
    } else goto bad_add;
    common->state = state_db_connect;
    pc->connection = c;
    return NGX_AGAIN;
bad_add:
    ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "ngx_event_flags not NGX_USE_RTSIG_EVENT or NGX_USE_CLEAR_EVENT or NGX_USE_LEVEL_EVENT");
invalid:
    ngx_postgres_free_connection(&pd->common, 0);
    return NGX_ERROR;
}


static void ngx_postgres_write_handler(ngx_event_t *ev) {
    ngx_log_debug1(NGX_LOG_DEBUG_HTTP, ev->log, 0, "%s", __func__);
}


static void ngx_postgres_process_notify(ngx_postgres_save_t *ps) {
    ngx_postgres_common_t *common = &ps->common;
    ngx_connection_t *c = common->connection;
    ngx_log_debug1(NGX_LOG_DEBUG_HTTP, c->log, 0, "%s", __func__);
    ngx_array_t *array = NULL;
    size_t len = 0;
    for (PGnotify *notify; (notify = PQnotifies(common->conn)); PQfreemem(notify)) {
        ngx_log_debug3(NGX_LOG_DEBUG_HTTP, c->log, 0, "relname=%s, extra=%s, be_pid=%i", notify->relname, notify->extra, notify->be_pid);
        if (!ngx_http_push_stream_add_msg_to_channel_my) continue;
        ngx_str_t id = { ngx_strlen(notify->relname), (u_char *) notify->relname };
        ngx_str_t text = { ngx_strlen(notify->extra), (u_char *) notify->extra };
        ngx_pool_t *temp_pool = ngx_create_pool(id.len + text.len, c->log);
        if (!temp_pool) { ngx_log_error(NGX_LOG_ERR, c->log, 0, "!ngx_create_pool"); continue; }
        switch (ngx_http_push_stream_add_msg_to_channel_my(c->log, &id, &text, NULL, NULL, 0, temp_pool)) {
            case NGX_ERROR: ngx_log_error(NGX_LOG_ERR, c->log, 0, "ngx_http_push_stream_add_msg_to_channel_my == NGX_ERROR"); break;
            case NGX_DECLINED:
                ngx_log_error(NGX_LOG_WARN, c->log, 0, "ngx_http_push_stream_add_msg_to_channel_my == NGX_DECLINED");
                if (common->listen) for (ngx_queue_t *queue = ngx_queue_head(common->listen); queue != ngx_queue_sentinel(common->listen); queue = ngx_queue_next(queue)) {
                    ngx_postgres_listen_t *listen = ngx_queue_data(queue, ngx_postgres_listen_t, queue);
                    if (id.len == listen->channel.len && !ngx_strncmp(id.data, listen->channel.data, id.len)) {
                        if (!array && !(array = ngx_array_create(c->pool, 1, sizeof(ngx_str_t)))) { ngx_log_error(NGX_LOG_ERR, c->log, 0, "!ngx_array_create"); break; }
                        ngx_str_t *unlisten = ngx_array_push(array);
                        if (!listen) { ngx_log_error(NGX_LOG_ERR, c->log, 0, "!ngx_array_push"); break; }
                        *unlisten = listen->command;
                        len += unlisten->len;
                        ngx_queue_remove(&listen->queue);
                        break;
                    }
                }
                break;
            case NGX_OK: ngx_log_debug0(NGX_LOG_DEBUG_HTTP, c->log, 0, "ngx_http_push_stream_add_msg_to_channel_my == NGX_OK"); break;
            default: ngx_log_error(NGX_LOG_ERR, c->log, 0, "ngx_http_push_stream_add_msg_to_channel_my == unknown"); break;
        }
        ngx_destroy_pool(temp_pool);
    }
    if (len && array && array->nelts) {
        u_char *unlisten = ngx_pnalloc(c->pool, len + 2 * array->nelts - 1);
        if (!unlisten) { ngx_log_error(NGX_LOG_ERR, c->log, 0, "!ngx_pnalloc"); goto destroy; }
        ngx_str_t *elts = array->elts;
        u_char *p = unlisten;
        for (ngx_uint_t i = 0; i < array->nelts; i++) {
            if (i) { *p++ = ';'; *p++ = '\n'; }
            p = ngx_cpymem(p, elts[i].data, elts[i].len);
        }
        *p = '\0';
        if (!PQsendQuery(common->conn, (const char *)unlisten)) { ngx_log_error(NGX_LOG_ERR, c->log, 0, "!PQsendQuery(%s) and %s", unlisten, PQerrorMessageMy(common->conn)); }
        else { ngx_log_debug1(NGX_LOG_DEBUG_HTTP, c->log, 0, "PQsendQuery(%s)", unlisten); }
        ngx_pfree(c->pool, unlisten);
    }
destroy:
    if (array) ngx_array_destroy(array);
}


static void ngx_postgres_read_handler(ngx_event_t *ev) {
    ngx_log_debug1(NGX_LOG_DEBUG_HTTP, ev->log, 0, "%s", __func__);
    ngx_connection_t *c = ev->data;
    ngx_postgres_save_t *ps = c->data;
    ngx_postgres_common_t *common = &ps->common;
    if (c->close) { ngx_log_debug0(NGX_LOG_DEBUG_HTTP, ev->log, 0, "close"); goto close; }
    if (c->read->timedout) { ngx_log_debug0(NGX_LOG_DEBUG_HTTP, ev->log, 0, "timedout"); goto close; }
    if (!PQconsumeInput(common->conn)) { ngx_log_error(NGX_LOG_ERR, ev->log, 0, "!PQconsumeInput and %s", PQerrorMessageMy(common->conn)); goto close; }
    if (PQisBusy(common->conn)) { ngx_log_debug0(NGX_LOG_DEBUG_HTTP, ev->log, 0, "PQisBusy"); return; }
    for (PGresult *res; (res = PQgetResult(common->conn)); PQclear(res)) switch(PQresultStatus(res)) {
        case PGRES_FATAL_ERROR: ngx_log_error(NGX_LOG_ERR, ev->log, 0, "PQresultStatus == PGRES_FATAL_ERROR and %s", PQresultErrorMessageMy(res)); break;
        default: ngx_log_debug1(NGX_LOG_DEBUG_HTTP, ev->log, 0, "PQresultStatus == %s", PQresStatus(PQresultStatus(res))); break;
    }
    ngx_postgres_process_notify(ps);
    return;
close:
    ngx_postgres_free_connection(&ps->common, 0);
    ngx_queue_remove(&ps->queue);
    ngx_postgres_server_t *server = common->server;
    ngx_queue_insert_tail(&server->free, &ps->queue);
}


static u_char *ngx_postgres_listen(ngx_postgres_data_t *pd, ngx_postgres_save_t *ps) {
    ngx_http_request_t *r = pd->request;
    ngx_postgres_common_t *common = &pd->common;
    ngx_connection_t *c = common->connection;
    u_char *listen = NULL;
    ngx_array_t *array = NULL;
    size_t len = 0;
    if (common->listen) for (ngx_queue_t *queue_pd = ngx_queue_head(common->listen); queue_pd != ngx_queue_sentinel(common->listen); queue_pd = ngx_queue_next(queue_pd)) {
        ngx_postgres_listen_t *listen_pd = ngx_queue_data(queue_pd, ngx_postgres_listen_t, queue);
        ngx_postgres_common_t *common = &ps->common;
        if (!common->listen) {
            if (!(common->listen = ngx_pcalloc(c->pool, sizeof(ngx_queue_t)))) { ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "!ngx_pcalloc"); return NULL; }
            ngx_queue_init(common->listen);
        }
        for (ngx_queue_t *queue_ps = ngx_queue_head(common->listen); queue_ps != ngx_queue_sentinel(common->listen); queue_ps = ngx_queue_next(queue_ps)) {
            ngx_postgres_listen_t *listen_ps = ngx_queue_data(queue_ps, ngx_postgres_listen_t, queue);
            if (listen_ps->channel.len == listen_pd->channel.len && !ngx_strncmp(listen_ps->channel.data, listen_pd->channel.data, listen_pd->channel.len)) goto cont;
        }
        if (!array && !(array = ngx_array_create(r->pool, 1, sizeof(ngx_postgres_listen_t)))) { ngx_log_error(NGX_LOG_ERR, c->log, 0, "!ngx_array_create"); return NULL; }
        ngx_postgres_listen_t *listen = ngx_array_push(array);
        if (!listen) { ngx_log_error(NGX_LOG_ERR, c->log, 0, "!ngx_array_push"); return NULL; }
        if (!(listen->channel.data = ngx_pstrdup(c->pool, &listen_pd->channel))) { ngx_log_error(NGX_LOG_ERR, c->log, 0, "!ngx_pstrdup"); return NULL; }
        listen->channel.len = listen_pd->channel.len;
        if (!(listen->command.data = ngx_pstrdup(c->pool, &listen_pd->command))) { ngx_log_error(NGX_LOG_ERR, c->log, 0, "!ngx_pstrdup"); return NULL; }
        listen->command.len = listen_pd->command.len;
        len += listen_pd->command.len - 2;
        ngx_queue_insert_tail(common->listen, &listen->queue);
        cont:;
    }
    if (len && array && array->nelts) {
        listen = ngx_pnalloc(r->pool, len + 2 * array->nelts - 1);
        if (!listen) { ngx_log_error(NGX_LOG_ERR, c->log, 0, "!ngx_pnalloc"); return NULL; }
        ngx_postgres_listen_t *elts = array->elts;
        u_char *p = listen;
        for (ngx_uint_t i = 0; i < array->nelts; i++) {
            if (i) { *p++ = ';'; *p++ = '\n'; }
            p = ngx_cpymem(p, elts[i].command.data + 2, elts[i].command.len - 2);
        }
        *p = '\0';
    }
    return listen;
}


static void ngx_postgres_free_to_save(ngx_postgres_data_t *pd, ngx_postgres_save_t *ps) {
    ngx_http_request_t *r = pd->request;
    ngx_postgres_common_t *common = &pd->common;
    ngx_queue_remove(&ps->queue);
    ngx_postgres_server_t *server = common->server;
    ngx_queue_insert_tail(&server->save, &ps->queue);
    ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0, "connection = %p", common->connection);
    ngx_peer_connection_t *pc = &r->upstream->peer;
    pc->connection = NULL;
    ps->common = pd->common;
    ngx_connection_t *c = ps->common.connection;
    c->data = ps;
    c->idle = 1;
    c->log = server->log ? server->log : ngx_cycle->log;
    if (c->pool) c->pool->log = c->log;
    ngx_add_timer(c->read, server->keepalive);
    c->read->delayed = 0;
    c->read->handler = ngx_postgres_read_handler;
    c->read->log = c->log;
    c->write->handler = ngx_postgres_write_handler;
    c->write->log = c->log;
}


static void ngx_postgres_free_peer(ngx_postgres_data_t *pd) {
    ngx_http_request_t *r = pd->request;
    ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0, "%s", __func__);
    if (r->upstream->headers_in.status_n != NGX_HTTP_OK) { ngx_log_error(NGX_LOG_WARN, r->connection->log, 0, "status_n != NGX_HTTP_OK"); return; }
    ngx_postgres_common_t *common = &pd->common;
    ngx_connection_t *c = common->connection;
    if (!c) { ngx_log_error(NGX_LOG_WARN, r->connection->log, 0, "!connection"); return; }
    if (c->read->timer_set) ngx_del_timer(c->read);
    if (c->write->timer_set) ngx_del_timer(c->write);
//    if (c->read->active && ngx_event_flags & NGX_USE_LEVEL_EVENT && ngx_del_event(c->read, NGX_READ_EVENT, 0) != NGX_OK) { ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "ngx_del_event != NGX_OK"); return; }
//    if (c->write->active && ngx_event_flags & NGX_USE_LEVEL_EVENT && ngx_del_event(c->write, NGX_WRITE_EVENT, 0) != NGX_OK) { ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "ngx_del_event != NGX_OK"); return; }
    ngx_postgres_server_t *server = common->server;
    if (c->requests >= server->requests) { ngx_log_error(NGX_LOG_WARN, r->connection->log, 0, "requests"); return; }
    if (ngx_terminate) { ngx_log_error(NGX_LOG_WARN, r->connection->log, 0, "ngx_terminate"); return; }
    if (ngx_exiting) { ngx_log_error(NGX_LOG_WARN, r->connection->log, 0, "ngx_exiting"); return; }
    u_char *listen = NULL;
    ngx_postgres_save_t *ps;
    if (ngx_queue_empty(&server->free)) {
        ngx_log_error(NGX_LOG_WARN, r->connection->log, 0, "ngx_queue_empty(free)");
        ngx_queue_t *queue = ngx_queue_last(&server->save);
        ps = ngx_queue_data(queue, ngx_postgres_save_t, queue);
        if (ngx_http_push_stream_add_msg_to_channel_my && ngx_http_push_stream_delete_channel_my) listen = ngx_postgres_listen(pd, ps);
        ngx_postgres_free_connection(&ps->common, 1);
    } else {
        ngx_queue_t *queue = ngx_queue_head(&server->free);
        ps = ngx_queue_data(queue, ngx_postgres_save_t, queue);
    }
    ngx_postgres_free_to_save(pd, ps);
    if (listen) {
        ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0, "listen = %s", listen);
        if (!PQsendQuery(common->conn, (const char *)listen)) { ngx_log_error(NGX_LOG_WARN, r->connection->log, 0, "!PQsendQuery(%s) and %s", listen, PQerrorMessageMy(common->conn)); }
    }
#ifdef NGX_YIELD
    if (!ngx_queue_empty(&server->data)) {
        ngx_queue_t *queue = ngx_queue_head(&server->data);
        ngx_postgres_data_t *pd = ngx_queue_data(queue, ngx_postgres_data_t, queue);
        ngx_http_request_t *r = pd->request;
        ngx_log_error(NGX_LOG_INFO, r->connection->log, 0, "dequeue peer %p", pd);
        ngx_queue_remove(&pd->queue);
        if (pd->timeout.timer_set) ngx_del_timer(&pd->timeout);
        ngx_http_upstream_re_init(r);
        /* override the read/write event handler to our own */
        if (r->upstream->reinit_request(r) != NGX_OK) { ngx_log_error(NGX_LOG_WARN, r->connection->log, 0, "reinit_request != NGX_OK"); }
    }
#endif
}


static void ngx_postgres_peer_free(ngx_peer_connection_t *pc, void *data, ngx_uint_t state) {
    ngx_log_debug1(NGX_LOG_DEBUG_HTTP, pc->log, 0, "%s", __func__);
    ngx_connection_t *c = pc->connection;
    ngx_postgres_data_t *pd = pc->data;
    ngx_postgres_common_t *common = &pd->common;
    ngx_postgres_server_t *server = common->server;
    if (state & NGX_PEER_FAILED || !c || c->read->eof || c->read->error || c->read->timedout || c->write->error || c->write->timedout); else
    if (server->nsave) ngx_postgres_free_peer(pd);
    if (pc->connection) ngx_postgres_free_connection(&pd->common, 1);
    pc->connection = NULL;
}


typedef struct {
    ngx_uint_t index;
    ngx_uint_t oid;
} ngx_postgres_param_t;


#ifdef NGX_YIELD
static void ngx_postgres_timeout(ngx_event_t *ev) {
    ngx_log_debug1(NGX_LOG_DEBUG_HTTP, ev->log, 0, "%s", __func__);
    ngx_connection_t *c = ev->data;
    ngx_http_request_t *r = c->data;
    ngx_postgres_data_t *pd = r->upstream->peer.data;
    if (pd->timeout.timer_set) ngx_del_timer(&pd->timeout);
    ngx_queue_remove(&pd->queue);
    ngx_postgres_next_upstream(r, NGX_HTTP_UPSTREAM_FT_TIMEOUT);
}
#endif


ngx_int_t ngx_postgres_peer_init(ngx_http_request_t *r, ngx_http_upstream_srv_conf_t *upstream_srv_conf) {
    ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0, "%s", __func__);
    ngx_postgres_data_t *pd = ngx_pcalloc(r->pool, sizeof(ngx_postgres_data_t));
    if (!pd) { ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "!ngx_pcalloc"); return NGX_ERROR; }
    ngx_postgres_common_t *common = &pd->common;
    common->server = ngx_http_conf_upstream_srv_conf(upstream_srv_conf, ngx_postgres_module);
#ifdef NGX_YIELD
    ngx_postgres_server_t *server = common->server;
    if (server->ndata) {
        pd->timeout.log = r->connection->log;
        pd->timeout.data = r->connection;
        pd->timeout.handler = ngx_postgres_timeout;
    }
#endif
    pd->request = r;
    r->upstream->peer.data = pd;
    r->upstream->peer.get = ngx_postgres_peer_get;
    r->upstream->peer.free = ngx_postgres_peer_free;
    ngx_postgres_location_t *location = ngx_http_get_module_loc_conf(r, ngx_postgres_module);
    ngx_postgres_query_t *query = &location->query;
    if (query->params.nelts) {
        ngx_postgres_param_t *param = query->params.elts;
        pd->nParams = query->params.nelts;
        if (!(pd->paramTypes = ngx_pnalloc(r->pool, query->params.nelts * sizeof(Oid)))) { ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "!ngx_pnalloc"); return NGX_ERROR; }
        if (!(pd->paramValues = ngx_pnalloc(r->pool, query->params.nelts * sizeof(char *)))) { ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "!ngx_pnalloc"); return NGX_ERROR; }
        for (ngx_uint_t i = 0; i < query->params.nelts; i++) {
            pd->paramTypes[i] = param[i].oid;
            ngx_http_variable_value_t *value = ngx_http_get_indexed_variable(r, param[i].index);
            if (!value || !value->data || !value->len) pd->paramValues[i] = NULL; else {
                if (!(pd->paramValues[i] = ngx_pnalloc(r->pool, value->len + 1))) { ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "!ngx_pnalloc"); return NGX_ERROR; }
                (void) ngx_cpystrn(pd->paramValues[i], value->data, value->len + 1);
            }
        }
    }
    pd->resultFormat = location->output.binary;
    if (location->variables.elts && location->variables.nelts) {
        if (ngx_array_init(&pd->variables, r->pool, location->variables.nelts, sizeof(ngx_str_t)) != NGX_OK) { ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "ngx_array_init != NGX_OK"); return NGX_ERROR; }
        pd->variables.nelts = location->variables.nelts;
        ngx_memzero(&pd->variables.elts, pd->variables.nelts * pd->variables.size); /* fake ngx_array_push'ing */
    }
    return NGX_OK;
}


ngx_flag_t ngx_postgres_is_my_peer(const ngx_peer_connection_t *pc) {
    return (pc->get == ngx_postgres_peer_get);
}


void ngx_postgres_free_connection(ngx_postgres_common_t *common, ngx_flag_t delete) {
    ngx_connection_t *c = common->connection;
    ngx_log_debug1(NGX_LOG_DEBUG_HTTP, c->log, 0, "%s", __func__);
    if (!c) {
        if (common->conn) {
            PQfinish(common->conn);
            common->conn = NULL;
        }
        return;
    }
    if (common->conn) {
        if (/*delete && */!c->close && common->listen && ngx_http_push_stream_delete_channel_my) {
            while (!ngx_queue_empty(common->listen)) {
                ngx_queue_t *queue = ngx_queue_head(common->listen);
                ngx_postgres_listen_t *listen = ngx_queue_data(queue, ngx_postgres_listen_t, queue);
                ngx_log_error(NGX_LOG_INFO, c->log, 0, "delete channel = %V", &listen->channel);
                ngx_http_push_stream_delete_channel_my(c->log, &listen->channel, (u_char *)"channel unlisten", sizeof("channel unlisten") - 1, c->pool);
                ngx_queue_remove(&listen->queue);
            }
        }
        PQfinish(common->conn);
        common->conn = NULL;
    }
    if (c->read->timer_set) ngx_del_timer(c->read);
    if (c->write->timer_set) ngx_del_timer(c->write);
    if (ngx_del_conn) ngx_del_conn(c, NGX_CLOSE_EVENT); else {
        if (c->read->active || c->read->disabled) ngx_del_event(c->read, NGX_READ_EVENT, NGX_CLOSE_EVENT);
        if (c->write->active || c->write->disabled) ngx_del_event(c->write, NGX_WRITE_EVENT, NGX_CLOSE_EVENT);
    }
    if (c->read->posted) { ngx_delete_posted_event(c->read); }
    if (c->write->posted) { ngx_delete_posted_event(c->write); }
    c->read->closed = 1;
    c->write->closed = 1;
    if (c->pool && !c->close) {
        ngx_destroy_pool(c->pool);
        c->pool = NULL;
    }
    ngx_free_connection(c);
    c->fd = (ngx_socket_t) -1;
}


static ngx_flag_t is_variable_character(u_char p) {
    return ((p >= '0' && p <= '9') || (p >= 'a' && p <= 'z') || (p >= 'A' && p <= 'Z') || p == '_');
}


#define IDOID 9999


ngx_conf_enum_t ngx_postgres_oids[] = {
    { ngx_string("IDOID"), IDOID },
    { ngx_string("BOOLOID"), BOOLOID },
    { ngx_string("BYTEAOID"), BYTEAOID },
    { ngx_string("CHAROID"), CHAROID },
    { ngx_string("NAMEOID"), NAMEOID },
    { ngx_string("INT8OID"), INT8OID },
    { ngx_string("INT2OID"), INT2OID },
    { ngx_string("INT2VECTOROID"), INT2VECTOROID },
    { ngx_string("INT4OID"), INT4OID },
    { ngx_string("REGPROCOID"), REGPROCOID },
    { ngx_string("TEXTOID"), TEXTOID },
    { ngx_string("OIDOID"), OIDOID },
    { ngx_string("TIDOID"), TIDOID },
    { ngx_string("XIDOID"), XIDOID },
    { ngx_string("CIDOID"), CIDOID },
    { ngx_string("OIDVECTOROID"), OIDVECTOROID },
    { ngx_string("JSONOID"), JSONOID },
    { ngx_string("XMLOID"), XMLOID },
    { ngx_string("PGNODETREEOID"), PGNODETREEOID },
    { ngx_string("PGNDISTINCTOID"), PGNDISTINCTOID },
    { ngx_string("PGDEPENDENCIESOID"), PGDEPENDENCIESOID },
    { ngx_string("PGMCVLISTOID"), PGMCVLISTOID },
    { ngx_string("PGDDLCOMMANDOID"), PGDDLCOMMANDOID },
    { ngx_string("POINTOID"), POINTOID },
    { ngx_string("LSEGOID"), LSEGOID },
    { ngx_string("PATHOID"), PATHOID },
    { ngx_string("BOXOID"), BOXOID },
    { ngx_string("POLYGONOID"), POLYGONOID },
    { ngx_string("LINEOID"), LINEOID },
    { ngx_string("FLOAT4OID"), FLOAT4OID },
    { ngx_string("FLOAT8OID"), FLOAT8OID },
    { ngx_string("UNKNOWNOID"), UNKNOWNOID },
    { ngx_string("CIRCLEOID"), CIRCLEOID },
    { ngx_string("CASHOID"), CASHOID },
    { ngx_string("MACADDROID"), MACADDROID },
    { ngx_string("INETOID"), INETOID },
    { ngx_string("CIDROID"), CIDROID },
    { ngx_string("MACADDR8OID"), MACADDR8OID },
    { ngx_string("ACLITEMOID"), ACLITEMOID },
    { ngx_string("BPCHAROID"), BPCHAROID },
    { ngx_string("VARCHAROID"), VARCHAROID },
    { ngx_string("DATEOID"), DATEOID },
    { ngx_string("TIMEOID"), TIMEOID },
    { ngx_string("TIMESTAMPOID"), TIMESTAMPOID },
    { ngx_string("TIMESTAMPTZOID"), TIMESTAMPTZOID },
    { ngx_string("INTERVALOID"), INTERVALOID },
    { ngx_string("TIMETZOID"), TIMETZOID },
    { ngx_string("BITOID"), BITOID },
    { ngx_string("VARBITOID"), VARBITOID },
    { ngx_string("NUMERICOID"), NUMERICOID },
    { ngx_string("REFCURSOROID"), REFCURSOROID },
    { ngx_string("REGPROCEDUREOID"), REGPROCEDUREOID },
    { ngx_string("REGOPEROID"), REGOPEROID },
    { ngx_string("REGOPERATOROID"), REGOPERATOROID },
    { ngx_string("REGCLASSOID"), REGCLASSOID },
    { ngx_string("REGTYPEOID"), REGTYPEOID },
    { ngx_string("REGROLEOID"), REGROLEOID },
    { ngx_string("REGNAMESPACEOID"), REGNAMESPACEOID },
    { ngx_string("UUIDOID"), UUIDOID },
    { ngx_string("LSNOID"), LSNOID },
    { ngx_string("TSVECTOROID"), TSVECTOROID },
    { ngx_string("GTSVECTOROID"), GTSVECTOROID },
    { ngx_string("TSQUERYOID"), TSQUERYOID },
    { ngx_string("REGCONFIGOID"), REGCONFIGOID },
    { ngx_string("REGDICTIONARYOID"), REGDICTIONARYOID },
    { ngx_string("JSONBOID"), JSONBOID },
    { ngx_string("JSONPATHOID"), JSONPATHOID },
    { ngx_string("TXID_SNAPSHOTOID"), TXID_SNAPSHOTOID },
    { ngx_string("INT4RANGEOID"), INT4RANGEOID },
    { ngx_string("NUMRANGEOID"), NUMRANGEOID },
    { ngx_string("TSRANGEOID"), TSRANGEOID },
    { ngx_string("TSTZRANGEOID"), TSTZRANGEOID },
    { ngx_string("DATERANGEOID"), DATERANGEOID },
    { ngx_string("INT8RANGEOID"), INT8RANGEOID },
    { ngx_string("RECORDOID"), RECORDOID },
    { ngx_string("RECORDARRAYOID"), RECORDARRAYOID },
    { ngx_string("CSTRINGOID"), CSTRINGOID },
    { ngx_string("ANYOID"), ANYOID },
    { ngx_string("ANYARRAYOID"), ANYARRAYOID },
    { ngx_string("VOIDOID"), VOIDOID },
    { ngx_string("TRIGGEROID"), TRIGGEROID },
    { ngx_string("EVTTRIGGEROID"), EVTTRIGGEROID },
    { ngx_string("LANGUAGE_HANDLEROID"), LANGUAGE_HANDLEROID },
    { ngx_string("INTERNALOID"), INTERNALOID },
    { ngx_string("OPAQUEOID"), OPAQUEOID },
    { ngx_string("ANYELEMENTOID"), ANYELEMENTOID },
    { ngx_string("ANYNONARRAYOID"), ANYNONARRAYOID },
    { ngx_string("ANYENUMOID"), ANYENUMOID },
    { ngx_string("FDW_HANDLEROID"), FDW_HANDLEROID },
    { ngx_string("INDEX_AM_HANDLEROID"), INDEX_AM_HANDLEROID },
    { ngx_string("TSM_HANDLEROID"), TSM_HANDLEROID },
    { ngx_string("TABLE_AM_HANDLEROID"), TABLE_AM_HANDLEROID },
    { ngx_string("ANYRANGEOID"), ANYRANGEOID },
    { ngx_string("BOOLARRAYOID"), BOOLARRAYOID },
    { ngx_string("BYTEAARRAYOID"), BYTEAARRAYOID },
    { ngx_string("CHARARRAYOID"), CHARARRAYOID },
    { ngx_string("NAMEARRAYOID"), NAMEARRAYOID },
    { ngx_string("INT8ARRAYOID"), INT8ARRAYOID },
    { ngx_string("INT2ARRAYOID"), INT2ARRAYOID },
    { ngx_string("INT2VECTORARRAYOID"), INT2VECTORARRAYOID },
    { ngx_string("INT4ARRAYOID"), INT4ARRAYOID },
    { ngx_string("REGPROCARRAYOID"), REGPROCARRAYOID },
    { ngx_string("TEXTARRAYOID"), TEXTARRAYOID },
    { ngx_string("OIDARRAYOID"), OIDARRAYOID },
    { ngx_string("TIDARRAYOID"), TIDARRAYOID },
    { ngx_string("XIDARRAYOID"), XIDARRAYOID },
    { ngx_string("CIDARRAYOID"), CIDARRAYOID },
    { ngx_string("OIDVECTORARRAYOID"), OIDVECTORARRAYOID },
    { ngx_string("JSONARRAYOID"), JSONARRAYOID },
    { ngx_string("XMLARRAYOID"), XMLARRAYOID },
    { ngx_string("POINTARRAYOID"), POINTARRAYOID },
    { ngx_string("LSEGARRAYOID"), LSEGARRAYOID },
    { ngx_string("PATHARRAYOID"), PATHARRAYOID },
    { ngx_string("BOXARRAYOID"), BOXARRAYOID },
    { ngx_string("POLYGONARRAYOID"), POLYGONARRAYOID },
    { ngx_string("LINEARRAYOID"), LINEARRAYOID },
    { ngx_string("FLOAT4ARRAYOID"), FLOAT4ARRAYOID },
    { ngx_string("FLOAT8ARRAYOID"), FLOAT8ARRAYOID },
    { ngx_string("CIRCLEARRAYOID"), CIRCLEARRAYOID },
    { ngx_string("MONEYARRAYOID"), MONEYARRAYOID },
    { ngx_string("MACADDRARRAYOID"), MACADDRARRAYOID },
    { ngx_string("INETARRAYOID"), INETARRAYOID },
    { ngx_string("CIDRARRAYOID"), CIDRARRAYOID },
    { ngx_string("MACADDR8ARRAYOID"), MACADDR8ARRAYOID },
    { ngx_string("ACLITEMARRAYOID"), ACLITEMARRAYOID },
    { ngx_string("BPCHARARRAYOID"), BPCHARARRAYOID },
    { ngx_string("VARCHARARRAYOID"), VARCHARARRAYOID },
    { ngx_string("DATEARRAYOID"), DATEARRAYOID },
    { ngx_string("TIMEARRAYOID"), TIMEARRAYOID },
    { ngx_string("TIMESTAMPARRAYOID"), TIMESTAMPARRAYOID },
    { ngx_string("TIMESTAMPTZARRAYOID"), TIMESTAMPTZARRAYOID },
    { ngx_string("INTERVALARRAYOID"), INTERVALARRAYOID },
    { ngx_string("TIMETZARRAYOID"), TIMETZARRAYOID },
    { ngx_string("BITARRAYOID"), BITARRAYOID },
    { ngx_string("VARBITARRAYOID"), VARBITARRAYOID },
    { ngx_string("NUMERICARRAYOID"), NUMERICARRAYOID },
    { ngx_string("REFCURSORARRAYOID"), REFCURSORARRAYOID },
    { ngx_string("REGPROCEDUREARRAYOID"), REGPROCEDUREARRAYOID },
    { ngx_string("REGOPERARRAYOID"), REGOPERARRAYOID },
    { ngx_string("REGOPERATORARRAYOID"), REGOPERATORARRAYOID },
    { ngx_string("REGCLASSARRAYOID"), REGCLASSARRAYOID },
    { ngx_string("REGTYPEARRAYOID"), REGTYPEARRAYOID },
    { ngx_string("REGROLEARRAYOID"), REGROLEARRAYOID },
    { ngx_string("REGNAMESPACEARRAYOID"), REGNAMESPACEARRAYOID },
    { ngx_string("UUIDARRAYOID"), UUIDARRAYOID },
    { ngx_string("PG_LSNARRAYOID"), PG_LSNARRAYOID },
    { ngx_string("TSVECTORARRAYOID"), TSVECTORARRAYOID },
    { ngx_string("GTSVECTORARRAYOID"), GTSVECTORARRAYOID },
    { ngx_string("TSQUERYARRAYOID"), TSQUERYARRAYOID },
    { ngx_string("REGCONFIGARRAYOID"), REGCONFIGARRAYOID },
    { ngx_string("REGDICTIONARYARRAYOID"), REGDICTIONARYARRAYOID },
    { ngx_string("JSONBARRAYOID"), JSONBARRAYOID },
    { ngx_string("JSONPATHARRAYOID"), JSONPATHARRAYOID },
    { ngx_string("TXID_SNAPSHOTARRAYOID"), TXID_SNAPSHOTARRAYOID },
    { ngx_string("INT4RANGEARRAYOID"), INT4RANGEARRAYOID },
    { ngx_string("NUMRANGEARRAYOID"), NUMRANGEARRAYOID },
    { ngx_string("TSRANGEARRAYOID"), TSRANGEARRAYOID },
    { ngx_string("TSTZRANGEARRAYOID"), TSTZRANGEARRAYOID },
    { ngx_string("DATERANGEARRAYOID"), DATERANGEARRAYOID },
    { ngx_string("INT8RANGEARRAYOID"), INT8RANGEARRAYOID },
    { ngx_string("CSTRINGARRAYOID"), CSTRINGARRAYOID },
    { ngx_null_string, 0 }
};


static ngx_uint_t type2oid(ngx_str_t *type) {
    ngx_conf_enum_t *e = ngx_postgres_oids;
    for (ngx_uint_t i = 0; e[i].name.len; i++) if (e[i].name.len - 3 == type->len && !ngx_strncasecmp(e[i].name.data, type->data, type->len)) return e[i].value;
    return 0;
}


char *ngx_postgres_query_conf(ngx_conf_t *cf, ngx_command_t *cmd, void *conf) {
    ngx_str_t *elts = cf->args->elts;
    ngx_str_t sql = elts[cf->args->nelts - 1];
    if (!sql.len) return "empty query";
    ngx_postgres_location_t *location = conf;
    ngx_postgres_query_t *query = &location->query;
    if (query->sql.data) return "is duplicate";
    if (sql.len > sizeof("file://") - 1 && !ngx_strncasecmp(sql.data, (u_char *)"file://", sizeof("file://") - 1)) {
        sql.data += sizeof("file://") - 1;
        sql.len -= sizeof("file://") - 1;
        if (ngx_conf_full_name(cf->cycle, &sql, 0) != NGX_OK) return "ngx_conf_full_name != NGX_OK";
        ngx_fd_t fd = ngx_open_file(sql.data, NGX_FILE_RDONLY, NGX_FILE_OPEN, 0);
        if (fd == NGX_INVALID_FILE) return "ngx_open_file == NGX_INVALID_FILE";
        ngx_file_info_t fi;
        if (ngx_fd_info(fd, &fi) == NGX_FILE_ERROR) { if (ngx_close_file(fd) == NGX_FILE_ERROR) return "ngx_close_file == NGX_FILE_ERROR"; return "ngx_fd_info == NGX_FILE_ERROR"; }
        size_t len = ngx_file_size(&fi);
        u_char *data = ngx_pnalloc(cf->pool, len);
        if (!data) { if (ngx_close_file(fd) == NGX_FILE_ERROR) return "ngx_close_file == NGX_FILE_ERROR"; return "!ngx_pnalloc"; }
        ssize_t n = ngx_read_fd(fd, data, len);
        if (n == -1) { if (ngx_close_file(fd) == NGX_FILE_ERROR) return "ngx_close_file == NGX_FILE_ERROR"; return "ngx_read_fd == -1"; }
        if ((size_t) n != len) { if (ngx_close_file(fd) == NGX_FILE_ERROR) return "ngx_close_file == NGX_FILE_ERROR"; return "ngx_read_fd != len"; }
        if (ngx_close_file(fd) == NGX_FILE_ERROR) return "ngx_close_file == NGX_FILE_ERROR";
        sql.data = data;
        sql.len = len;
    }
    if (!(query->sql.data = ngx_palloc(cf->pool, sql.len))) return "!ngx_palloc";
    if (ngx_array_init(&query->params, cf->pool, 1, sizeof(ngx_postgres_param_t)) != NGX_OK) return "ngx_array_init != NGX_OK";
    if (ngx_array_init(&query->ids, cf->pool, 1, sizeof(ngx_uint_t)) != NGX_OK) return "ngx_array_init != NGX_OK";
    u_char *p = query->sql.data, *s = sql.data, *e = sql.data + sql.len;
    query->percent = 0;
    for (ngx_uint_t k = 0; s < e; *p++ = *s++) {
        if (*s == '%') {
            *p++ = '%';
            query->percent++;
        } else if (*s == '$') {
            ngx_str_t name;
            for (name.data = ++s, name.len = 0; s < e && is_variable_character(*s); s++, name.len++);
            if (!name.len) { *p++ = '$'; continue; }
            ngx_str_t type = {0, NULL};
            if (s[0] == ':' && s[1] == ':') for (s += 2, type.data = s, type.len = 0; s < e && is_variable_character(*s); s++, type.len++);
            if (!type.len) { *p++ = '$'; p = ngx_copy(p, name.data, name.len); continue; }
            ngx_int_t index = ngx_http_get_variable_index(cf, &name);
            if (index == NGX_ERROR) return "ngx_http_get_variable_index == NGX_ERROR";
            ngx_uint_t oid = type2oid(&type);
            if (!oid) return "!type2oid";
            if (oid == IDOID) {
                ngx_uint_t *id = ngx_array_push(&query->ids);
                if (!id) return "!ngx_array_push";
                *id = (ngx_uint_t) index;
                *p++ = '%';
                *p++ = 'V';
            } else {
                ngx_postgres_param_t *param = ngx_array_push(&query->params);
                if (!param) return "!ngx_array_push";
                param->index = (ngx_uint_t) index;
                param->oid = oid;
                p += ngx_sprintf(p, "$%i", ++k) - p;
            }
            if (s >= e) break;
        }
    }
    query->sql.len = p - query->sql.data;
    query->listen = query->sql.len > sizeof("LISTEN ") - 1 && !ngx_strncasecmp(query->sql.data, (u_char *)"LISTEN ", sizeof("LISTEN ") - 1);
    if (query->listen && !ngx_http_push_stream_add_msg_to_channel_my && !ngx_http_push_stream_delete_channel_my) return "LISTEN requires ngx_http_push_stream_module!";
//    ngx_conf_log_error(NGX_LOG_EMERG, cf, 0, "sql = `%V`", &query->sql);
    return NGX_CONF_OK;
}


char *PQerrorMessageMy(const PGconn *conn) {
    char *err = PQerrorMessage(conn);
    if (!err) return err;
    int len = strlen(err);
    if (!len) return err;
    err[len - 1] = '\0';
    return err;
}


char *PQresultErrorMessageMy(const PGresult *res) {
    char *err = PQresultErrorMessage(res);
    if (!err) return err;
    int len = strlen(err);
    if (!len) return err;
    err[len - 1] = '\0';
    return err;
}

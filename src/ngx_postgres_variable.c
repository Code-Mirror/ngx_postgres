/*
 * Copyright (c) 2010, FRiCKLE Piotr Sikora <info@frickle.com>
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

#include "ngx_postgres_module.h"
#include "ngx_postgres_variable.h"


ngx_int_t
ngx_postgres_variable_columns(ngx_http_request_t *r,
    ngx_http_variable_value_t *v, uintptr_t data)
{
    ngx_postgres_ctx_t  *pgctx;

    ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0, "%s entering: \"$postgres_columns\"", __func__);

    pgctx = ngx_http_get_module_ctx(r, ngx_postgres_module);

    if ((pgctx == NULL) || (pgctx->var_cols == NGX_ERROR)) {
        v->not_found = 1;
        ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0, "%s returning NGX_OK (not_found)", __func__);
        return NGX_OK;
    }

    v->data = ngx_pnalloc(r->pool, NGX_INT32_LEN);
    if (v->data == NULL) {
        ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0, "%s returning NGX_ERROR", __func__);
        return NGX_ERROR;
    }

    v->len = ngx_sprintf(v->data, "%i", pgctx->var_cols) - v->data;
    v->valid = 1;
    v->no_cacheable = 0;
    v->not_found = 0;

    ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0, "%s returning NGX_OK", __func__);
    return NGX_OK;
}

ngx_int_t
ngx_postgres_variable_rows(ngx_http_request_t *r,
    ngx_http_variable_value_t *v, uintptr_t data)
{
    ngx_postgres_ctx_t  *pgctx;

    ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0, "%s entering: \"$postgres_rows\"", __func__);

    pgctx = ngx_http_get_module_ctx(r, ngx_postgres_module);

    if ((pgctx == NULL) || (pgctx->var_rows == NGX_ERROR)) {
        v->not_found = 1;
        ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0, "%s returning NGX_OK (not_found)", __func__);
        return NGX_OK;
    }

    v->data = ngx_pnalloc(r->pool, NGX_INT32_LEN);
    if (v->data == NULL) {
        ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0, "%s returning NGX_ERROR", __func__);
        return NGX_ERROR;
    }

    v->len = ngx_sprintf(v->data, "%i", pgctx->var_rows) - v->data;
    v->valid = 1;
    v->no_cacheable = 0;
    v->not_found = 0;

    ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0, "%s returning NGX_OK", __func__);
    return NGX_OK;
}

ngx_int_t
ngx_postgres_variable_affected(ngx_http_request_t *r,
    ngx_http_variable_value_t *v, uintptr_t data)
{
    ngx_postgres_ctx_t  *pgctx;

    ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0, "%s entering: \"$postgres_affected\"", __func__);

    pgctx = ngx_http_get_module_ctx(r, ngx_postgres_module);

    if ((pgctx == NULL) || (pgctx->var_affected == NGX_ERROR)) {
        v->not_found = 1;
        ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0, "%s returning NGX_OK (not_found)", __func__);
        return NGX_OK;
    }

    v->data = ngx_pnalloc(r->pool, NGX_INT32_LEN);
    if (v->data == NULL) {
        ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0, "%s returning NGX_ERROR", __func__);
        return NGX_ERROR;
    }

    v->len = ngx_sprintf(v->data, "%i", pgctx->var_affected) - v->data;
    v->valid = 1;
    v->no_cacheable = 0;
    v->not_found = 0;

    ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0, "%s returning NGX_OK", __func__);
    return NGX_OK;
}

ngx_int_t
ngx_postgres_variable_query(ngx_http_request_t *r,
    ngx_http_variable_value_t *v, uintptr_t data)
{
    ngx_postgres_ctx_t  *pgctx;

    ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0, "%s entering: \"$postgres_query\"", __func__);

    pgctx = ngx_http_get_module_ctx(r, ngx_postgres_module);

    if ((pgctx == NULL) || (pgctx->var_query.len == 0)) {
        v->not_found = 1;
        ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0, "%s returning NGX_OK (not_found)", __func__);
        return NGX_OK;
    }

    v->valid = 1;
    v->no_cacheable = 0;
    v->not_found = 0;
    v->len = pgctx->var_query.len;
    v->data = pgctx->var_query.data;

    ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0, "%s returning NGX_OK", __func__);
    return NGX_OK;
}

ngx_int_t
ngx_postgres_variable_get_custom(ngx_http_request_t *r,
    ngx_http_variable_value_t *v, uintptr_t data)
{
    ngx_postgres_variable_t  *pgvar = (ngx_postgres_variable_t *) data;
    ngx_postgres_ctx_t       *pgctx;
    ngx_str_t                *store;

    ngx_log_debug3(NGX_LOG_DEBUG_HTTP, r->connection->log, 0, "%s entering: \"$%.*s\"", __func__,
                              (int) pgvar->var->name.len,
                              pgvar->var->name.data);

    pgctx = ngx_http_get_module_ctx(r, ngx_postgres_module);

    if ((pgctx == NULL) || (pgctx->variables == NULL)) {
        v->not_found = 1;
        ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0, "%s returning NGX_OK (not_found)", __func__);
        return NGX_OK;
    }

    store = pgctx->variables->elts;

    /* idx is always valid */
    if (store[pgvar->idx].len == 0) {
        v->not_found = 1;
        ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0, "%s returning NGX_OK (not_found)", __func__);
        return NGX_OK;
    }

    v->valid = 1;
    v->no_cacheable = 0;
    v->not_found = 0;
    v->len = store[pgvar->idx].len;
    v->data = store[pgvar->idx].data;

    ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0, "%s returning NGX_OK", __func__);
    return NGX_OK;
}

ngx_str_t
ngx_postgres_variable_set_custom(ngx_http_request_t *r, PGresult *res,
    ngx_postgres_variable_t *pgvar)
{
    ngx_http_core_loc_conf_t  *clcf;
    ngx_postgres_value_t      *pgv;
    ngx_int_t                  col_count, row_count, col, len;
    ngx_str_t                  value = ngx_null_string;

    ngx_log_debug3(NGX_LOG_DEBUG_HTTP, r->connection->log, 0, "%s entering: \"$%.*s\"", __func__,
                              (int) pgvar->var->name.len,
                              pgvar->var->name.data);

    col_count = PQnfields(res);
    row_count = PQntuples(res);

    pgv = &pgvar->value;

    if (pgv->column != NGX_ERROR) {
        /* get column by number */
        col = pgv->column;
    } else {
        /* get column by name */
        col = PQfnumber(res, (char const *) pgv->col_name);
        if (col == NGX_ERROR) {
            if (pgv->required) {
                clcf = ngx_http_get_module_loc_conf(r, ngx_http_core_module);

                ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
                              "postgres: \"postgres_set\" for variable \"$%V\""
                              " requires value from column \"%s\" that wasn't"
                              " found in the received result-set in location"
                              " \"%V\"",
                              &pgvar->var->name, pgv->col_name, &clcf->name);
            }

            ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0, "%s returning empty value", __func__);
            return value;
        }
    }

    if ((pgv->row >= row_count) || (col >= col_count)) {
        if (pgv->required) {
            clcf = ngx_http_get_module_loc_conf(r, ngx_http_core_module);

            ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
                          "postgres: \"postgres_set\" for variable \"$%V\""
                          " requires value out of range of the received"
                          " result-set (rows:%d cols:%d) in location \"%V\"",
                          &pgvar->var->name, row_count, col_count, &clcf->name);
        }

        ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0, "%s returning empty value", __func__);
        return value;
    }

    if (PQgetisnull(res, pgv->row, col)) {
        if (pgv->required) {
            clcf = ngx_http_get_module_loc_conf(r, ngx_http_core_module);

            ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
                          "postgres: \"postgres_set\" for variable \"$%V\""
                          " requires non-NULL value in location \"%V\"",
                          &pgvar->var->name, &clcf->name);
        }

        ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0, "%s returning empty value", __func__);
        return value;
    }

    len = PQgetlength(res, pgv->row, col);
    if (len == 0) {
        if (pgv->required) {
            clcf = ngx_http_get_module_loc_conf(r, ngx_http_core_module);

            ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
                          "postgres: \"postgres_set\" for variable \"$%V\""
                          " requires non-zero length value in location \"%V\"",
                          &pgvar->var->name, &clcf->name);
        }

        ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0, "%s returning empty value", __func__);
        return value;
    }

    value.data = ngx_pnalloc(r->pool, len);
    if (value.data == NULL) {
        ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0, "%s returning empty value", __func__);
        return value;
    }

    ngx_memcpy(value.data, PQgetvalue(res, pgv->row, col), len);
    value.len = len;

    ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0, "%s returning non-empty value", __func__);
    return value;
}

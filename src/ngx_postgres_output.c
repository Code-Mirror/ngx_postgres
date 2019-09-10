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

#include "ngx_postgres_module.h"
#include "ngx_postgres_output.h"
#include <math.h>
#include <postgresql/server/catalog/pg_type_d.h>


ngx_int_t
ngx_postgres_output_value(ngx_http_request_t *r, PGresult *res)
{
    ngx_postgres_ctx_t        *pgctx;
    ngx_http_core_loc_conf_t  *clcf;
    ngx_chain_t               *cl;
    ngx_buf_t                 *b;
    size_t                     size;

    ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0, "%s entering", __func__);

    pgctx = ngx_http_get_module_ctx(r, ngx_postgres_module);

    if ((pgctx->var_rows != 1) || (pgctx->var_cols != 1)) {
        clcf = ngx_http_get_module_loc_conf(r, ngx_http_core_module);

        ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
                      "postgres: \"postgres_output value\" received %d value(s)"
                      " instead of expected single value in location \"%V\"",
                      pgctx->var_rows * pgctx->var_cols, &clcf->name);

        ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0, "%s returning NGX_DONE, status NGX_HTTP_INTERNAL_SERVER_ERROR", __func__);
        pgctx->status = NGX_HTTP_INTERNAL_SERVER_ERROR;
        return NGX_DONE;
    }

    if (PQgetisnull(res, 0, 0)) {
        clcf = ngx_http_get_module_loc_conf(r, ngx_http_core_module);

        ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
                      "postgres: \"postgres_output value\" received NULL value"
                      " in location \"%V\"", &clcf->name);

        ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0, "%s returning NGX_DONE, status NGX_HTTP_INTERNAL_SERVER_ERROR", __func__);
        pgctx->status = NGX_HTTP_INTERNAL_SERVER_ERROR;
        return NGX_DONE;
    }

    size = PQgetlength(res, 0, 0);
    if (size == 0) {
        clcf = ngx_http_get_module_loc_conf(r, ngx_http_core_module);

        ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
                      "postgres: \"postgres_output value\" received empty value"
                      " in location \"%V\"", &clcf->name);

        ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0, "%s returning NGX_DONE, status NGX_HTTP_INTERNAL_SERVER_ERROR", __func__);
        pgctx->status = NGX_HTTP_INTERNAL_SERVER_ERROR;
        return NGX_DONE;
    }

    b = ngx_create_temp_buf(r->pool, size);
    if (b == NULL) {
        ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0, "%s returning NGX_ERROR", __func__);
        return NGX_ERROR;
    }

    cl = ngx_alloc_chain_link(r->pool);
    if (cl == NULL) {
        ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0, "%s returning NGX_ERROR", __func__);
        return NGX_ERROR;
    }

    cl->buf = b;
    b->memory = 1;
    b->tag = r->upstream->output.tag;

    b->last = ngx_copy(b->last, PQgetvalue(res, 0, 0), size);

    if (b->last != b->end) {
        ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0, "%s returning NGX_ERROR", __func__);
        return NGX_ERROR;
    }

    cl->next = NULL;

    /* set output response */
    pgctx->response = cl;

    ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0, "%s returning NGX_DONE", __func__);
    return NGX_DONE;
}


int hex2bin( const char *s )
{
    int ret=0;
    int i;
    for( i=0; i<2; i++ )
    {
        char c = *s++;
        int n=0;
        if( '0'<=c && c<='9' )
            n = c-'0';
        else if( 'a'<=c && c<='f' )
            n = 10 + c-'a';
        else if( 'A'<=c && c<='F' )
            n = 10 + c-'A';
        ret = n + ret*16;
    }
    return ret;
}


ngx_int_t
ngx_postgres_output_hex(ngx_http_request_t *r, PGresult *res)
{
    ngx_postgres_ctx_t        *pgctx;
    ngx_http_core_loc_conf_t  *clcf;
    ngx_chain_t               *cl;
    ngx_buf_t                 *b;
    size_t                     size;

    ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0, "%s entering", __func__);

    pgctx = ngx_http_get_module_ctx(r, ngx_postgres_module);

    if ((pgctx->var_rows != 1) || (pgctx->var_cols != 1)) {
        clcf = ngx_http_get_module_loc_conf(r, ngx_http_core_module);

        ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
                      "postgres: \"postgres_output value\" received %d value(s)"
                      " instead of expected single value in location \"%V\"",
                      pgctx->var_rows * pgctx->var_cols, &clcf->name);

        ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0, "%s returning NGX_DONE, status NGX_HTTP_INTERNAL_SERVER_ERROR", __func__);
        pgctx->status = NGX_HTTP_INTERNAL_SERVER_ERROR;
        return NGX_DONE;
    }

    if (PQgetisnull(res, 0, 0)) {
        clcf = ngx_http_get_module_loc_conf(r, ngx_http_core_module);

        ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
                      "postgres: \"postgres_output value\" received NULL value"
                      " in location \"%V\"", &clcf->name);

        ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0, "%s returning NGX_DONE, status NGX_HTTP_INTERNAL_SERVER_ERROR", __func__);
        pgctx->status = NGX_HTTP_INTERNAL_SERVER_ERROR;
        return NGX_DONE;
    }

    size = PQgetlength(res, 0, 0);
    if (size == 0) {
        clcf = ngx_http_get_module_loc_conf(r, ngx_http_core_module);

        ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
                      "postgres: \"postgres_output value\" received empty value"
                      " in location \"%V\"", &clcf->name);

        ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0, "%s returning NGX_DONE, status NGX_HTTP_INTERNAL_SERVER_ERROR", __func__);
        pgctx->status = NGX_HTTP_INTERNAL_SERVER_ERROR;
        return NGX_DONE;
    }

    b = ngx_create_temp_buf(r->pool, floor(size / 2));
    if (b == NULL) {
        ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0, "%s returning NGX_ERROR", __func__);
        return NGX_ERROR;
    }

    cl = ngx_alloc_chain_link(r->pool);
    if (cl == NULL) {
        ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0, "%s returning NGX_ERROR", __func__);
        return NGX_ERROR;
    }

    cl->buf = b;
    b->memory = 1;
    b->tag = r->upstream->output.tag;

    char *value = PQgetvalue(res, 0, 0);

    unsigned int start = 0;
    if (value[start] == '\\')
        start++;
    if (value[start] == 'x')
        start++;

    for (; start < size; start += 2)
        *(b->last++) = hex2bin(value + start);
    //if (b->last != b->end) {
    //    ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0, "%s returning NGX_ERROR", __func__);
    //    return NGX_ERROR;
    //}

    cl->next = NULL;

    /* set output response */
    pgctx->response = cl;

    ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0, "%s returning NGX_DONE", __func__);
    return NGX_DONE;
}


ngx_int_t
ngx_postgres_output_text(ngx_http_request_t *r, PGresult *res)
{
    ngx_postgres_ctx_t        *pgctx;
    ngx_chain_t               *cl;
    ngx_buf_t                 *b;
    size_t                     size;
    ngx_int_t                  col_count, row_count, col, row;

    ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0, "%s entering", __func__);

    pgctx = ngx_http_get_module_ctx(r, ngx_postgres_module);

    col_count = pgctx->var_cols;
    row_count = pgctx->var_rows;

    /* pre-calculate total length up-front for single buffer allocation */
    size = 0;

    for (row = 0; row < row_count; row++) {
        for (col = 0; col < col_count; col++) {
            if (PQgetisnull(res, row, col)) {
                size += sizeof("(null)") - 1;
            } else {
                size += PQgetlength(res, row, col);  /* field string data */
            }
        }
    }

    size += row_count * col_count - 1;               /* delimiters */

    if ((row_count == 0) || (size == 0)) {
        ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0, "%s returning NGX_DONE (empty result)", __func__);
        return NGX_DONE;
    }

    b = ngx_create_temp_buf(r->pool, size);
    if (b == NULL) {
        ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0, "%s returning NGX_ERROR", __func__);
        return NGX_ERROR;
    }

    cl = ngx_alloc_chain_link(r->pool);
    if (cl == NULL) {
        ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0, "%s returning NGX_ERROR", __func__);
        return NGX_ERROR;
    }

    cl->buf = b;
    b->memory = 1;
    b->tag = r->upstream->output.tag;

    /* fill data */
    for (row = 0; row < row_count; row++) {
        for (col = 0; col < col_count; col++) {
            if (PQgetisnull(res, row, col)) {
                b->last = ngx_copy(b->last, "(null)", sizeof("(null)") - 1);
            } else {
                size = PQgetlength(res, row, col);
                if (size) {
                    b->last = ngx_copy(b->last, PQgetvalue(res, row, col),
                                       size);
                }
            }

            if ((row != row_count - 1) || (col != col_count - 1)) {
                b->last = ngx_copy(b->last, "\n", 1);
            }
        }
    }

    if (b->last != b->end) {
        ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0, "%s returning NGX_ERROR", __func__);
        return NGX_ERROR;
    }

    cl->next = NULL;

    /* set output response */
    pgctx->response = cl;

    ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0, "%s returning NGX_DONE", __func__);
    return NGX_DONE;
}


ngx_int_t
ngx_postgres_output_chain(ngx_http_request_t *r, ngx_chain_t *cl)
{
    ngx_http_upstream_t       *u = r->upstream;
    ngx_http_core_loc_conf_t  *clcf;
    ngx_postgres_loc_conf_t   *pglcf;
    ngx_postgres_ctx_t        *pgctx;
    ngx_int_t                  rc;

    ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0, "%s entering", __func__);

    if (!r->header_sent) {
        ngx_http_clear_content_length(r);

        pglcf = ngx_http_get_module_loc_conf(r, ngx_postgres_module);
        pgctx = ngx_http_get_module_ctx(r, ngx_postgres_module);

        r->headers_out.status = pgctx->status ? ngx_abs(pgctx->status)
                                              : NGX_HTTP_OK;


        if (pglcf->output_handler == &ngx_postgres_output_json) {
        //    This thing crashes nginx for some reason...
            ngx_str_set(&r->headers_out.content_type, "application/json");
            r->headers_out.content_type_len = r->headers_out.content_type.len;
        } else if (pglcf->output_handler != NULL) {
            /* default type for output value|row */
            clcf = ngx_http_get_module_loc_conf(r, ngx_http_core_module);

            r->headers_out.content_type = clcf->default_type;
            r->headers_out.content_type_len = clcf->default_type.len;
        }

        r->headers_out.content_type_lowcase = NULL;

        rc = ngx_http_send_header(r);
        if (rc == NGX_ERROR || rc > NGX_OK || r->header_only) {
            ngx_log_debug2(NGX_LOG_DEBUG_HTTP, r->connection->log, 0, "%s returning rc:%d", __func__, (int) rc);
            return rc;
        }
    }

    if (cl == NULL) {
        ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0, "%s returning NGX_DONE", __func__);
        return NGX_DONE;
    }

    rc = ngx_http_output_filter(r, cl);
    if (rc == NGX_ERROR || rc > NGX_OK) {
        ngx_log_debug2(NGX_LOG_DEBUG_HTTP, r->connection->log, 0, "%s returning rc:%d", __func__, (int) rc);
        return rc;
    }

    ngx_chain_update_chains(r->pool, &u->free_bufs, &u->busy_bufs, &cl,
                            u->output.tag);

    ngx_log_debug2(NGX_LOG_DEBUG_HTTP, r->connection->log, 0, "%s returning rc:%d", __func__, (int) rc);
    return rc;
}


ngx_int_t
ngx_postgres_output_json(ngx_http_request_t *r, PGresult *res)
{
    ngx_postgres_ctx_t        *pgctx;
    ngx_chain_t               *cl;
    ngx_buf_t                 *b;
    size_t                     size;
    ngx_int_t                  col_count, row_count, col, row;

    ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0, "%s entering", __func__);

    pgctx = ngx_http_get_module_ctx(r, ngx_postgres_module);

    col_count = pgctx->var_cols;
    row_count = pgctx->var_rows;

    int col_type = 0;

    // single row with single json column, return that column
    if (row_count == 1 && col_count == 1 && (PQftype(res, 0) == JSONOID || PQftype(res, 0) == JSONBOID)) {
        size = PQgetlength(res, 0, 0);
    } else {
        /* pre-calculate total length up-front for single buffer allocation */
        size = 2; // [] + \0


        for (row = 0; row < row_count; row++) {
            size += sizeof("{}") - 1;
            for (col = 0; col < col_count; col++) {
                if (PQgetisnull(res, row, col)) {
                    size += sizeof("null") - 1;
                } else {
                    col_type = PQftype(res, col);
                    int col_length = PQgetlength(res, row, col);

                    if ((col_type < INT8OID || col_type > INT4OID) && (col_type != JSONBOID && col_type != JSONOID)) { //not numbers or json
                        char *col_value = PQgetvalue(res, row, col);
                        if (col_type == BOOLOID) {
                            switch (col_value[0]) {
                                case 't': case 'T': col_length = sizeof("true") - 1; break;
                                case 'f': case 'F': col_length = sizeof("false") - 1; break;
                            }
                        } else {
                            size += sizeof("\"\"") - 1;
                            col_length += ngx_escape_json(NULL, (u_char *) col_value, col_length);
                        }

                    }

                    size += col_length;  /* field string data */
                }
            }
        }
        for (col = 0; col < col_count; col++) {
            char *col_name = PQfname(res, col);
            size += (strlen(col_name) + 3) * row_count; // extra "":
        }

        size += row_count * (col_count - 1);               /* column delimeters */
        size += row_count - 1;                            /* row delimeters */

    }

    if ((row_count == 0) || (size == 0)) {
        ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0, "%s returning NGX_DONE (empty result)", __func__);
        return NGX_DONE;
    }

    b = ngx_create_temp_buf(r->pool, size);
    if (b == NULL) {
        ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0, "%s returning NGX_ERROR", __func__);
        return NGX_ERROR;
    }

    cl = ngx_alloc_chain_link(r->pool);
    if (cl == NULL) {
        ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0, "%s returning NGX_ERROR", __func__);
        return NGX_ERROR;
    }

    cl->buf = b;
    b->memory = 1;
    b->tag = r->upstream->output.tag;


    if (row_count == 1 && col_count == 1 && (PQftype(res, 0) == JSONOID || PQftype(res, 0) == JSONBOID)) {

        b->last = ngx_copy(b->last, PQgetvalue(res, 0, 0),
                           size);
    } else {
        // YF: Populate empty parent req variables with names of columns, if in subrequest
        // HACK, LOL! Better move me out
        if (r != r->main) {

            ngx_str_t export_variable;
            for (col = 0; col < col_count; col++) {
                char *col_name = PQfname(res, col);
                export_variable.data = (unsigned char*)col_name;
                export_variable.len = strlen(col_name);

                ngx_uint_t meta_variable_hash = ngx_hash_key(export_variable.data, export_variable.len);
                ngx_http_variable_value_t *raw_meta = ngx_http_get_variable( r->main, &export_variable, meta_variable_hash  );
                if (!raw_meta->not_found && raw_meta->len == 0) {
                    raw_meta->valid = 1;
                    int exported_length = PQgetlength(res, 0, col);
                    char *exported_value = ngx_palloc(r->main->pool, exported_length);
                    ngx_memcpy(exported_value, PQgetvalue(res, 0, col), exported_length);
                    raw_meta->len = exported_length;
                    raw_meta->data = (unsigned char*)exported_value;
                }
            }
        }

        /* fill data */
        b->last = ngx_copy(b->last, "[", sizeof("[") - 1);
        for (row = 0; row < row_count; row++) {
            if (row > 0)
                b->last = ngx_copy(b->last, ",", 1);

            b->last = ngx_copy(b->last, "{", sizeof("{") - 1);
            for (col = 0; col < col_count; col++) {
                if (col > 0)
                    b->last = ngx_copy(b->last, ",", 1);

                char *col_name = PQfname(res, col);
                b->last = ngx_copy(b->last, "\"", sizeof("\"") - 1);
                b->last = ngx_copy(b->last, col_name, strlen(col_name));
                b->last = ngx_copy(b->last, "\":", sizeof("\":") - 1);

                if (PQgetisnull(res, row, col)) {
                    b->last = ngx_copy(b->last, "null", sizeof("null") - 1);
                } else {
                    size = PQgetlength(res, row, col);

                    col_type = PQftype(res, col);
                    //not numbers or json
                    if (((col_type < INT8OID || col_type > INT4OID) && (col_type != JSONBOID && col_type != JSONOID)) || size == 0) {
                        if (col_type == BOOLOID) {
                            switch (PQgetvalue(res, row, col)[0]) {
                                case 't': case 'T': b->last = ngx_copy(b->last, "true", sizeof("true") - 1); break;
                                case 'f': case 'F': b->last = ngx_copy(b->last, "false", sizeof("false") - 1); break;
                            }
                        } else {
                            b->last = ngx_copy(b->last, "\"", sizeof("\"") - 1);
                            if (size > 0) b->last = (u_char *) ngx_escape_json(b->last, (u_char *) PQgetvalue(res, row, col), size);
                            b->last = ngx_copy(b->last, "\"", sizeof("\"") - 1);
                        }
                    } else {
                        b->last = ngx_copy(b->last, PQgetvalue(res, row, col),
                                           size);
                    }
                }

            }
            b->last = ngx_copy(b->last, "}", sizeof("}") - 1);
        }
        b->last = ngx_copy(b->last, "]", sizeof("]") - 1);
    }

    //fprintf(stdout, "PRINTING %d\n", b->end - b->last);
    //fprintf(stdout, "PRINTING %s\n", b->pos);
    if (b->last != b->end) {
        ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0, "%s returning NGX_ERROR", __func__);
        return NGX_ERROR;
    }

    cl->next = NULL;

    /* set output response */
    pgctx->response = cl;

    ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0, "%s returning NGX_DONE", __func__);
    return NGX_DONE;
}

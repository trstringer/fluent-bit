/* -*- Mode: C; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */

/*  Fluent Bit
 *  ==========
 *  Copyright (C) 2015-2018 Treasure Data Inc.
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  you may not use this file except in compliance with the License.
 *  You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 *  Unless required by applicable law or agreed to in writing, software
 *  distributed under the License is distributed on an "AS IS" BASIS,
 *  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *  See the License for the specific language governing permissions and
 *  limitations under the License.
 */

#include <fluent-bit/flb_info.h>
#include <fluent-bit/flb_input.h>
#include <fluent-bit/flb_stats.h>
#include <fluent-bit/flb_kernel.h>
#include <fluent-bit/flb_pack.h>

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/sysinfo.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

#include "proc.h"

#define DEFAULT_INTERVAL_SEC  1
#define DEFAULT_INTERVAL_NSEC 0

struct flb_in_mem_config {
    int    idx;
    int    page_size;
    int    interval_sec;
    int    interval_nsec;
    pid_t  pid;
};

struct flb_in_mem_info {
    uint64_t mem_total;
    uint64_t mem_used;
    uint64_t mem_free;
    uint64_t swap_total;
    uint64_t swap_used;
    uint64_t swap_free;
};

struct flb_input_plugin in_mem_plugin;

static int in_mem_collect(struct flb_input_instance *i_ins,
                          struct flb_config *config, void *in_context);
#if 0
/* Locate a specific key into the buffer */
static char *field(char *data, char *field)
{
    char *p;
    char *q;
    char *sep;
    char *value;
    int len = strlen(field);

    p = strstr(data, field);
    if (!p) {
        return NULL;
    }

    sep = strchr(p, ':');
    p = ++sep;
    p++;

    while (*p == ' ') p++;

    q = strchr(p, ' ');
    len = q - p;
    value = flb_malloc(len + 1);
    strncpy(value, p, len);
    value[len] = '\0';

    return value;
}
#endif
static int mem_calc(struct flb_in_mem_info *m_info)
{
    int ret;
    struct sysinfo info;

    ret = sysinfo(&info);
    if (ret == -1) {
        perror("sysinfo");
        return -1;
    }

    /* set values in KBs */
    m_info->mem_total     = info.totalram / 1024;

    /* This value seems to be MemAvailable if it is supported */
    /*     or MemFree on legacy linux */
    m_info->mem_free      = info.freeram  / 1024;

    m_info->mem_used      = m_info->mem_total - m_info->mem_free;

    m_info->swap_total    = info.totalswap / 1024;
    m_info->swap_free     = info.freeswap  / 1024;
    m_info->swap_used     = m_info->swap_total - m_info->swap_free;

    return 0;
}

static int in_mem_init(struct flb_input_instance *in,
                       struct flb_config *config, void *data)
{
    int ret;
    char *tmp;
    struct flb_in_mem_config *ctx;
    (void) data;
    char *pval = NULL;

    /* Initialize context */
    ctx = flb_malloc(sizeof(struct flb_in_mem_config));
    if (!ctx) {
        return -1;
    }
    ctx->idx = 0;
    ctx->pid = 0;
    ctx->page_size = sysconf(_SC_PAGESIZE);

    /* Collection time setting */
    pval = flb_input_get_property("interval_sec", in);
    if (pval != NULL && atoi(pval) > 0) {
        ctx->interval_sec = atoi(pval);
    }
    else {
        ctx->interval_sec = DEFAULT_INTERVAL_SEC;
    }
    ctx->interval_nsec = DEFAULT_INTERVAL_NSEC;

    /* Check if the caller want's to trace a specific Process ID */
    tmp = flb_input_get_property("pid", in);
    if (tmp) {
        ctx->pid = atoi(tmp);
    }

    /* Set the context */
    flb_input_set_context(in, ctx);

    /* Set the collector */
    ret = flb_input_set_collector_time(in,
                                       in_mem_collect,
                                       ctx->interval_sec,
                                       ctx->interval_nsec,
                                       config);
    if (ret == -1) {
        flb_error("Could not set collector for memory input plugin");
    }

    return 0;
}

static int in_mem_collect(struct flb_input_instance *i_ins,
                          struct flb_config *config, void *in_context)
{
    int ret;
    int len;
    int entries = 6;/* (total,used,free) * (memory, swap) */
    struct proc_task *task = NULL;
    struct flb_in_mem_config *ctx = in_context;
    struct flb_in_mem_info info;

    if (ctx->pid) {
        task = proc_stat(ctx->pid, ctx->page_size);
        if (!task) {
            flb_warn("[in_mem] could not measure PID %i", ctx->pid);
            ctx->pid = 0;
        }
    }

    ret = mem_calc(&info);

    if (ret == -1) {
        if (task) {
            proc_free(task);
        }
        return -1;
    }

    if (task) {
        entries += 2;
    }

    /* Mark the start of a 'buffer write' operation */
    flb_input_buf_write_start(i_ins);

    msgpack_pack_array(&i_ins->mp_pck, 2);
    flb_pack_time_now(&i_ins->mp_pck);
    msgpack_pack_map(&i_ins->mp_pck, entries);

    msgpack_pack_str(&i_ins->mp_pck, 9);
    msgpack_pack_str_body(&i_ins->mp_pck, "Mem.total", 9);
    msgpack_pack_uint64(&i_ins->mp_pck, info.mem_total);

    msgpack_pack_str(&i_ins->mp_pck, 8);
    msgpack_pack_str_body(&i_ins->mp_pck, "Mem.used", 8);
    msgpack_pack_uint64(&i_ins->mp_pck, info.mem_used);

    msgpack_pack_str(&i_ins->mp_pck, 8);
    msgpack_pack_str_body(&i_ins->mp_pck, "Mem.free", 8);
    msgpack_pack_uint64(&i_ins->mp_pck, info.mem_free);

    msgpack_pack_str(&i_ins->mp_pck, 10);
    msgpack_pack_str_body(&i_ins->mp_pck, "Swap.total", 10);
    msgpack_pack_uint64(&i_ins->mp_pck, info.swap_total);

    msgpack_pack_str(&i_ins->mp_pck, 9);
    msgpack_pack_str_body(&i_ins->mp_pck, "Swap.used", 9);
    msgpack_pack_uint64(&i_ins->mp_pck, info.swap_used);

    msgpack_pack_str(&i_ins->mp_pck, 9);
    msgpack_pack_str_body(&i_ins->mp_pck, "Swap.free", 9);
    msgpack_pack_uint64(&i_ins->mp_pck, info.swap_free);


    if (task) {
        /* RSS bytes */
        msgpack_pack_str(&i_ins->mp_pck, 10);
        msgpack_pack_str_body(&i_ins->mp_pck, "proc_bytes", 10);
        msgpack_pack_uint64(&i_ins->mp_pck, task->proc_rss);

        /* RSS Human readable format */
        len = strlen(task->proc_rss_hr);
        msgpack_pack_str(&i_ins->mp_pck, 7);
        msgpack_pack_str_body(&i_ins->mp_pck, "proc_hr", 7);
        msgpack_pack_str(&i_ins->mp_pck, len);
        msgpack_pack_str_body(&i_ins->mp_pck, task->proc_rss_hr, len);

        proc_free(task);
    }

    flb_trace("[in_mem] memory total=%lu kb, used=%lu kb, free=%lu kb",
              info.mem_total, info.mem_used, info.mem_free);
    flb_trace("[in_mem] swap total=%lu kb, used=%lu kb, free=%lu kb",
              info.swap_total, info.swap_used, info.swap_free);
    ++ctx->idx;

    flb_input_buf_write_end(i_ins);
    flb_stats_update(in_mem_plugin.stats_fd, 0, 1);
    return 0;
}

static int in_mem_exit(void *data, struct flb_config *config)
{
    (void) *config;
    struct flb_in_mem_config *ctx = data;

    /* done */
    flb_free(ctx);

    return 0;
}

struct flb_input_plugin in_mem_plugin = {
    .name         = "mem",
    .description  = "Memory Usage",
    .cb_init      = in_mem_init,
    .cb_pre_run   = NULL,
    .cb_collect   = in_mem_collect,
    .cb_flush_buf = NULL,
    .cb_exit      = in_mem_exit
};

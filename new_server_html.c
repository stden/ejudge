/* -*- mode: c -*- */
/* $Id: new_server_html.c 7670 2013-12-11 08:28:55Z cher $ */

/* Copyright (C) 2006-2013 Alexander Chernov <cher@ejudge.ru> */

/*
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include "config.h"
#include "ej_types.h"
#include "ej_limits.h"

#include "new-server.h"
#include "new_server_proto.h"
#include "pathutl.h"
#include "xml_utils.h"
#include "misctext.h"
#include "copyright.h"
#include "userlist_clnt.h"
#include "ejudge_cfg.h"
#include "errlog.h"
#include "userlist_proto.h"
#include "contests.h"
#include "nsdb_plugin.h"
#include "l10n.h"
#include "fileutl.h"
#include "userlist.h"
#include "mischtml.h"
#include "serve_state.h"
#include "teamdb.h"
#include "prepare.h"
#include "runlog.h"
#include "html.h"
#include "watched_file.h"
#include "mime_type.h"
#include "sha.h"
#include "archive_paths.h"
#include "curtime.h"
#include "clarlog.h"
#include "team_extra.h"
#include "diff.h"
#include "protocol.h"
#include "printing.h"
#include "sformat.h"
#include "charsets.h"
#include "compat.h"
#include "ej_uuid.h"
#include "prepare_dflt.h"

#include "reuse_xalloc.h"
#include "reuse_logger.h"
#include "reuse_osdeps.h"

#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <ctype.h>
#include <time.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>

#if CONF_HAS_LIBINTL - 0 == 1
#include <libintl.h>
#define _(x) gettext(x)
#else
#define _(x) x
#endif
#define __(x) x

#if !defined CONF_STYLE_PREFIX
#define CONF_STYLE_PREFIX "/ejudge/"
#endif

#define ARMOR(s)  html_armor_buf(&ab, (s))
#define URLARMOR(s)  url_armor_buf(&ab, s)
#define FAIL(c) do { retval = -(c); goto cleanup; } while (0)

enum { CONTEST_EXPIRE_TIME = 300 };
static struct contest_extra **extras = 0;
static size_t extra_a = 0, extra_u = 0;

static struct server_framework_job *job_first, *job_last;
static int job_count, job_serial;

static void unprivileged_page_login(FILE *fout,
                                    struct http_request_info *phr,
                                    int orig_locale_id);
static void
unpriv_page_header(FILE *fout,
                   struct http_request_info *phr,
                   const struct contest_desc *cnts,
                   struct contest_extra *extra,
                   time_t start_time, time_t stop_time);
static void
do_json_user_state(FILE *fout, const serve_state_t cs, int user_id,
                   int need_reload_check);
static int
get_register_url(
        unsigned char *buf,
        size_t size,
        const struct contest_desc *cnts,
        const unsigned char *self_url);

struct contest_extra *
ns_get_contest_extra(int contest_id)
{
  struct contest_extra *p;
  size_t i, j, k;

  ASSERT(contest_id > 0 && contest_id <= EJ_MAX_CONTEST_ID);

  if (!extra_u) {
    if (!extra_a) {
      extra_a = 16;
      XCALLOC(extras, extra_a);
    }
    XCALLOC(p, 1);
    extras[extra_u++] = p;
    p->contest_id = contest_id;
    p->last_access_time = time(0);
    return p;
  }

  if (contest_id > extras[extra_u - 1]->contest_id) {
    if (extra_u == extra_a) {
      extra_a *= 2;
      XREALLOC(extras, extra_a);
    }
    XCALLOC(p, 1);
    extras[extra_u++] = p;
    p->contest_id = contest_id;
    p->last_access_time = time(0);
    return p;
  }

  i = 0; j = extra_u;
  while (i < j) {
    k = (i + j) / 2;
    if ((p = extras[k])->contest_id == contest_id) {
      p->last_access_time = time(0);
      return p;
    }
    if (p->contest_id < contest_id) {
      i = k + 1;
    } else {
      j = k;
    }
  }
  ASSERT(j < extra_u);
  ASSERT(extras[j]->contest_id > contest_id);
  if (!j) {
    if (extra_u == extra_a) {
      extra_a *= 2;
      XREALLOC(extras, extra_a);
    }
    memmove(&extras[j + 1], &extras[j], extra_u * sizeof(extras[0]));
    extra_u++;
    XCALLOC(p, 1);
    extras[j] = p;
    p->contest_id = contest_id;
    p->last_access_time = time(0);
    return p;
  }
  ASSERT(i > 0);
  ASSERT(extras[i - 1]->contest_id < contest_id);
  if (extra_u == extra_a) {
    extra_a *= 2;
    XREALLOC(extras, extra_a);
  }
  memmove(&extras[j + 1], &extras[j], (extra_u - j) * sizeof(extras[0]));
  extra_u++;
  XCALLOC(p, 1);
  extras[j] = p;
  p->contest_id = contest_id;
  p->last_access_time = time(0);
  return p;
}

struct contest_extra *
ns_try_contest_extra(int contest_id)
{
  struct contest_extra *p;
  size_t i, j, k;

  if (contest_id <= 0 || contest_id > EJ_MAX_CONTEST_ID) return 0;
  if (!extra_u) return 0;
  if (contest_id < extras[0]->contest_id) return 0;
  if (contest_id > extras[extra_u - 1]->contest_id) return 0;
  i = 0; j = extra_u;
  while (i < j) {
    k = (i + j) / 2;
    if ((p = extras[k])->contest_id == contest_id) {
      return p;
    }
    if (p->contest_id < contest_id) {
      i = k + 1;
    } else {
      j = k;
    }
  }
  return 0;
}

void
ns_contest_unload_callback(serve_state_t cs)
{
  struct client_state *p;

  if (cs->client_id < 0 || !cs->pending_xml_import
      || !(p = ns_get_client_by_id(cs->client_id)))
    return;

  p->contest_id = 0;
  p->destroy_callback = 0;
  nsf_close_client_fds(p);
  ns_send_reply(p, -NEW_SRV_ERR_CONTEST_UNLOADED);
}

void
ns_client_destroy_callback(struct client_state *p)
{
  struct contest_extra *extra;
  const struct contest_desc *cnts = 0;
  serve_state_t cs;

  if (p->contest_id <= 0) return;
  if (contests_get(p->contest_id, &cnts) < 0) return;
  if (!(extra = ns_try_contest_extra(p->contest_id))) return;
  if (!(cs = extra->serve_state)) return;
  if (!cs->pending_xml_import || cs->client_id < 0) return;
  if (cs->saved_testing_suspended != cs->testing_suspended) {
    cs->testing_suspended = cs->saved_testing_suspended;
    serve_update_status_file(cs, 1);
    if (!cs->testing_suspended)
      serve_judge_suspended(ejudge_config, cnts, cs, 0, 0, 0,
                            DFLT_G_REJUDGE_PRIORITY_ADJUSTMENT, 0);
  }
  xfree(cs->pending_xml_import); cs->pending_xml_import = 0;
  cs->client_id = -1;
  cs->destroy_callback = 0;
}

static void
do_unload_contest(int idx)
{
  struct contest_extra *extra;
  const struct contest_desc *cnts = 0;
  int i, contest_id;

  ASSERT(idx >= 0 && idx < extra_u);
  extra = extras[idx];
  contest_id = extra->contest_id;

  contests_get(contest_id, &cnts);

  if (extra->serve_state) {
    serve_check_stat_generation(ejudge_config, extra->serve_state, cnts, 1, utf8_mode);
    serve_update_status_file(extra->serve_state, 1);
    team_extra_flush(extra->serve_state->team_extra_state);
    extra->serve_state = serve_state_destroy(ejudge_config, extra->serve_state, cnts, ul_conn);
  }

  xfree(extra->contest_arm);
  watched_file_clear(&extra->header);
  watched_file_clear(&extra->menu_1);
  watched_file_clear(&extra->menu_2);
  watched_file_clear(&extra->separator);
  watched_file_clear(&extra->footer);
  watched_file_clear(&extra->priv_header);
  watched_file_clear(&extra->priv_footer);
  watched_file_clear(&extra->copyright);
  watched_file_clear(&extra->welcome);
  watched_file_clear(&extra->reg_welcome);

  for (i = 0; i < USER_ROLE_LAST; i++) {
    xfree(extra->user_access[i].v);
  }
  xfree(extra->user_access_idx.v);

  memset(extra, 0, sizeof(*extra));
  xfree(extra);
  extras[idx] = 0;
}

void
ns_unload_contest(int contest_id)
{
  struct contest_extra *extra = 0;
  int i, j, k = 0;

  if (contest_id <= 0 || contest_id > EJ_MAX_CONTEST_ID) return;
  if (!extra_u) return;
  if (contest_id < extras[0]->contest_id) return;
  if (contest_id > extras[extra_u - 1]->contest_id) return;
  i = 0; j = extra_u;
  while (i < j) {
    k = (i + j) / 2;
    if ((extra = extras[k])->contest_id == contest_id) {
      break;
    }
    if (extra->contest_id < contest_id) {
      i = k + 1;
    } else {
      j = k;
    }
  }
  if (i >= j) return;

  do_unload_contest(k);
  if (k < extra_u - 1)
    memmove(&extras[k], &extras[k + 1], (extra_u-k-1)*sizeof(extras[0]));
  extra_u--;
  extras[extra_u] = 0;

  info("contest %d is unloaded", contest_id);
}

void
ns_unload_contests(void)
{
  int i;

  for (i = 0; i < extra_u; i++)
    do_unload_contest(i);
  extra_u = 0;
}

void
ns_unload_expired_contests(time_t cur_time)
{
  int i, j;

  if (cur_time <= 0) cur_time = time(0);

  for (i = 0, j = 0; i < extra_u; i++)
    if (extras[i]
        && extras[i]->last_access_time + CONTEST_EXPIRE_TIME < cur_time
        && (!extras[i]->serve_state
            || !extras[i]->serve_state->pending_xml_import)) {
      do_unload_contest(i);
    } else {
      extras[j++] = extras[i];
      //extras[i] = 0;
    }
  extra_u = j;
}

void
ns_add_job(struct server_framework_job *job)
{
  if (!job) return;
  job->id = ++job_serial;
  job->start_time = time(NULL);
  ++job_count;
  job->prev = job_last;
  job->next = NULL;
  if (job_last) {
    job_last->next = job;
  } else {
    job_first = job;
  }
  job_last = job;
}

void
ns_remove_job(struct server_framework_job *job)
{
  if (job->next) {
    job->next->prev = job->prev;
  } else {
    job_last = job->prev;
  }
  if (job->prev) {
    job->prev->next = job->next;
  } else {
    job_first = job->next;
  }
  job->next = NULL;
  job->prev = NULL;
  job->vt->destroy(job);
  --job_count;
}

static void
handle_pending_xml_import(const struct contest_desc *cnts, serve_state_t cs)
{
  struct client_state *p;
  FILE *fout = 0;
  char *out_text = 0;
  size_t out_size = 0;

  if (cs->client_id < 0 || !(p = ns_get_client_by_id(cs->client_id))) {
    if (cs->saved_testing_suspended != cs->testing_suspended) {
      cs->testing_suspended = cs->saved_testing_suspended;
      serve_update_status_file(cs, 1);
      if (!cs->testing_suspended)
        serve_judge_suspended(ejudge_config, cnts, cs, 0, 0, 0,
                              DFLT_G_REJUDGE_PRIORITY_ADJUSTMENT, 0);
    }
    xfree(cs->pending_xml_import); cs->pending_xml_import = 0;
    cs->client_id = -1; cs->destroy_callback = 0;
    return;
  }

  fout = open_memstream(&out_text, &out_size);
  runlog_import_xml(cs, cs->runlog_state, fout, 1, cs->pending_xml_import);
  close_memstream(fout); fout = 0;
  if (out_size > 0) {
    ns_new_autoclose(p, out_text, out_size);
    out_text = 0;
  } else {
    nsf_close_client_fds(p);
    xfree(out_text); out_text = 0;
  }
  ns_send_reply(p, NEW_SRV_RPL_OK);

  if (cs->saved_testing_suspended != cs->testing_suspended) {
    cs->testing_suspended = cs->saved_testing_suspended;
    serve_update_status_file(cs, 1);
    if (!cs->testing_suspended)
      serve_judge_suspended(ejudge_config, cnts, cs, 0, 0, 0,
                            DFLT_G_REJUDGE_PRIORITY_ADJUSTMENT, 0);
  }
  xfree(cs->pending_xml_import); cs->pending_xml_import = 0;
  cs->client_id = -1; cs->destroy_callback = 0;
  p->contest_id = 0;
  p->destroy_callback = 0;
}

enum { MAX_WORK_BATCH = 10 };

int
ns_loop_callback(struct server_framework_state *state)
{
  time_t cur_time = time(0);
  struct contest_extra *e;
  serve_state_t cs;
  const struct contest_desc *cnts;
  int contest_id, i, eind;
  strarray_t files;
  int count = 0;

  memset(&files, 0, sizeof(files));

  if (job_first) {
    if (job_first->contest_id > 0) {
      e = ns_try_contest_extra(job_first->contest_id);
      e->last_access_time = cur_time;
    }
    if (job_first->vt->run(job_first, &count, MAX_WORK_BATCH)) {
      ns_remove_job(job_first);
    }
  }

  for (eind = 0; eind < extra_u; eind++) {
    e = extras[eind];
    ASSERT(e);
    contest_id = e->contest_id;
    if (!(cs = e->serve_state)) continue;
    if (contests_get(contest_id, &cnts) < 0 || !cnts) continue;

    e->serve_state->current_time = cur_time;
    ns_check_contest_events(e->serve_state, cnts);

    serve_update_public_log_file(e->serve_state, cnts);
    serve_update_external_xml_log(e->serve_state, cnts);
    serve_update_internal_xml_log(e->serve_state, cnts);

    for (i = 0; i < cs->compile_dirs_u; i++) {
      if (get_file_list(cs->compile_dirs[i].status_dir, &files) < 0)
        continue;
      if (files.u <= 0) continue;
      for (int j = 0; j < files.u && count < MAX_WORK_BATCH; ++j) {
        ++count;
        serve_read_compile_packet(ejudge_config, cs, cnts,
                                  cs->compile_dirs[i].status_dir,
                                  cs->compile_dirs[i].report_dir,
                                  files.v[j]);
      }
      e->last_access_time = cur_time;
      xstrarrayfree(&files);
    }

    for (i = 0; i < cs->run_dirs_u; i++) {
      if (get_file_list(cs->run_dirs[i].status_dir, &files) < 0
          || files.u <= 0)
        continue;
      for (int j = 0; j < files.u && count < MAX_WORK_BATCH; ++j) {
        ++count;
        serve_read_run_packet(ejudge_config, cs, cnts,
                              cs->run_dirs[i].status_dir,
                              cs->run_dirs[i].report_dir,
                              cs->run_dirs[i].full_report_dir,
                              files.v[j]);
      }
      e->last_access_time = cur_time;
      xstrarrayfree(&files);
    }

    if (cs->pending_xml_import && !serve_count_transient_runs(cs))
      handle_pending_xml_import(cnts, cs);
  }

  ns_unload_expired_contests(cur_time);
  xstrarrayfree(&files);
  return count < MAX_WORK_BATCH;
}

void
ns_post_select_callback(struct server_framework_state *state)
{
  time_t cur_time = time(0);
  struct contest_extra *e;
  serve_state_t cs;
  const struct contest_desc *cnts;
  int contest_id, eind;

  for (eind = 0; eind < extra_u; eind++) {
    e = extras[eind];
    ASSERT(e);
    contest_id = e->contest_id;
    if (!(cs = e->serve_state)) continue;
    if (contests_get(contest_id, &cnts) < 0 || !cnts) continue;

    e->serve_state->current_time = cur_time;
    ns_check_contest_events(e->serve_state, cnts);
  }
}

static const unsigned char*
ns_getenv(const struct http_request_info *phr, const unsigned char *var)
{
  int i;
  size_t var_len;

  if (!var) return 0;
  var_len = strlen(var);
  for (i = 0; i < phr->env_num; i++)
    if (!strncmp(phr->envs[i], var, var_len) && phr->envs[i][var_len] == '=')
      break;
  if (i < phr->env_num)
    return phr->envs[i] + var_len + 1;
  return 0;
}

int
ns_cgi_param(const struct http_request_info *phr, const unsigned char *param,
             const unsigned char **p_value)
{
  int i;

  if (!param) return -1;
  for (i = 0; i < phr->param_num; i++)
    if (!strcmp(phr->param_names[i], param))
      break;
  if (i >= phr->param_num) return 0;
  if (strlen(phr->params[i]) != phr->param_sizes[i]) return -1;
  *p_value = phr->params[i];
  return 1;
}

int
ns_cgi_param_bin(const struct http_request_info *phr,
                 const unsigned char *param,
                 const unsigned char **p_value,
                 size_t *p_size)
{
  int i;

  if (!param) return -1;
  for (i = 0; i < phr->param_num; i++)
    if (!strcmp(phr->param_names[i], param))
      break;
  if (i >= phr->param_num) return 0;
  *p_value = phr->params[i];
  *p_size = phr->param_sizes[i];
  return 1;
}

static const unsigned char *
ns_cgi_nname(const struct http_request_info *phr,
             const unsigned char *prefix, size_t pflen)
{
  int i;

  if (!prefix || !pflen) return 0;
  for (i = 0; i < phr->param_num; i++)
    if (!strncmp(phr->param_names[i], prefix, pflen))
      return phr->param_names[i];
  return 0;
}

int
ns_cgi_param_int(
        struct http_request_info *phr,
        const unsigned char *name,
        int *p_val)
{
  const unsigned char *s = 0, *p = 0;
  char *eptr = 0;
  int x;

  if (ns_cgi_param(phr, name, &s) <= 0) return -1;

  p = s;
  while (*p && isspace(*p)) p++;
  if (!*p) return -1;

  errno = 0;
  x = strtol(s, &eptr, 10);
  if (errno || *eptr) return -1;
  if (p_val) *p_val = x;
  return 0;
}

int
ns_cgi_param_int_opt(
        struct http_request_info *phr,
        const unsigned char *name,
        int *p_val,
        int default_value)
{
  const unsigned char *s = 0, *p;
  char *eptr = 0;
  int x;

  if (!(x = ns_cgi_param(phr, name, &s))) {
    if (p_val) *p_val = default_value;
    return 0;
  } else if (x < 0) return -1;
  p = s;
  while (*p && isspace(*p)) p++;
  if (!*p) {
    if (p_val) *p_val = default_value;
    return 0;
  }
  errno = 0;
  x = strtol(s, &eptr, 10);
  if (errno || *eptr) return -1;
  if (p_val) *p_val = x;
  return 0;
}

int
ns_cgi_param_int_opt_2(
        struct http_request_info *phr,
        const unsigned char *name,
        int *p_val,
        int *p_set_flag)
{
  const unsigned char *s = 0, *p;
  char *eptr = 0;
  int x;

  ASSERT(p_val);
  ASSERT(p_set_flag);

  *p_val = 0;
  *p_set_flag = 0;

  if (!(x = ns_cgi_param(phr, name, &s))) return 0;
  else if (x < 0) return -1;

  p = s;
  while (*p && isspace(*p)) p++;
  if (!*p) return 0;

  errno = 0;
  x = strtol(s, &eptr, 10);
  if (errno || *eptr) return -1;
  *p_val = x;
  *p_set_flag = 1;
  return 0;
}

static void
close_ul_connection(struct server_framework_state *state)
{
  if (!ul_conn) return;

  nsf_remove_watch(state, userlist_clnt_get_fd(ul_conn));
  ul_conn = userlist_clnt_close(ul_conn);
}

static void
ul_conn_callback(struct server_framework_state *state,
                 struct server_framework_watch *pw,
                 int events)
{
  int r, contest_id = 0;
  struct contest_extra *e;

  info("userlist-server fd ready");
  while (1) {
    r = userlist_clnt_read_notification(ul_conn, &contest_id);
    if (r == ULS_ERR_UNEXPECTED_EOF) {
      info("userlist-server disconnect");
      close_ul_connection(state);
      break;
    } else if (r < 0) {
      err("userlist-server error: %s", userlist_strerror(-r));
      close_ul_connection(state);
      break;
    } else {
      e = ns_try_contest_extra(contest_id);
      if (!e) {
        err("userlist-server notification: %d - no such contest", contest_id);
        break;
      } else {
        info("userlist-server notification: %d", contest_id);
        if (e->serve_state && e->serve_state->teamdb_state)
          teamdb_set_update_flag(e->serve_state->teamdb_state);
        if (userlist_clnt_bytes_available(ul_conn) <= 0) break;
      }
    }
    info("userlist-server fd has more data");
  }
}

static void
ul_notification_callback(void *user_data, int contest_id)
{
  struct contest_extra *e;

  e = ns_try_contest_extra(contest_id);
  if (!e) {
    err("userlist-server notification: %d - no such contest", contest_id);
  } else {
    info("userlist-server notification: %d", contest_id);
    if (e->serve_state && e->serve_state->teamdb_state)
      teamdb_set_update_flag(e->serve_state->teamdb_state);
  }
}

int
ns_open_ul_connection(struct server_framework_state *state)
{
  struct server_framework_watch w;
  int r, contest_id, eind;
  struct contest_extra *e;

  if (ul_conn) return 0;

  if (!(ul_conn = userlist_clnt_open(ejudge_config->socket_path))) {
    err("ns_open_ul_connection: connect to server failed");
    return -1;
  }

  memset(&w, 0, sizeof(w));
  w.fd = userlist_clnt_get_fd(ul_conn);
  w.mode = NSF_READ;
  w.callback = ul_conn_callback;
  nsf_add_watch(state, &w);

  xfree(ul_login); ul_login = 0;
  if ((r = userlist_clnt_admin_process(ul_conn, &ul_uid, &ul_login, 0)) < 0) {
    err("open_connection: cannot became an admin process: %s",
        userlist_strerror(-r));
    close_ul_connection(state);
    return -1;
  }

  userlist_clnt_set_notification_callback(ul_conn, ul_notification_callback, 0);

  // add notifications for all the active contests
  for (eind = 0; eind < extra_u; eind++) {
    e = extras[eind];
    ASSERT(e);
    contest_id = e->contest_id;
    if (!e->serve_state) continue;
    if ((r = userlist_clnt_notify(ul_conn, ULS_ADD_NOTIFY, contest_id)) < 0) {
      err("open_connection: cannot add notification: %s",
          userlist_strerror(-r));
      close_ul_connection(state);
      return -1;
    }
  }

  info("running as %s (%d)", ul_login, ul_uid);
  return 0;
}

static void
load_problem_plugin(serve_state_t cs, int prob_id)
{
  struct section_problem_data *prob = 0;
  struct problem_extra_info *extra;
  struct problem_plugin_iface *iface;
  unsigned char plugin_name[1024];
  int len, i;
  const unsigned char *f = __FUNCTION__;
  const size_t *sza;
  path_t plugin_path;

  if (prob_id <= 0 || prob_id > cs->max_prob) return;
  if (!(prob = cs->probs[prob_id])) return;
  extra = &cs->prob_extras[prob_id];

  if (!prob->plugin_file[0]) return;
  if (extra->plugin || extra->plugin_error) return;

  if (cs->global->advanced_layout > 0) {
    get_advanced_layout_path(plugin_path, sizeof(plugin_path), cs->global,
                             prob, prob->plugin_file, -1);
  } else {
    snprintf(plugin_path, sizeof(plugin_path), "%s", prob->plugin_file);
  }

  snprintf(plugin_name, sizeof(plugin_name), "problem_%s", prob->short_name);
  len = strlen(plugin_name);
  for (i = 0; i < len; i++)
    if (plugin_name[i] == '-')
      plugin_name[i] = '_';

  iface = (struct problem_plugin_iface*) plugin_load(plugin_path,
                                                     "problem",
                                                     plugin_name);
  if (!iface) {
    extra->plugin_error = 1;
    return;
  }

  if (iface->problem_version != PROBLEM_PLUGIN_IFACE_VERSION) {
    err("%s: plugin version mismatch", f);
    return;
  }
  if (!(sza = iface->sizes_array)) {
    err("%s: plugin sizes array is NULL", f);
    return;
  }
  if (iface->sizes_array_size != serve_struct_sizes_array_size) {
    err("%s: plugin sizes array size mismatch: %zu instead of %zu",
        f, iface->sizes_array_size, serve_struct_sizes_array_size);
    return;
  }
  for (i = 0; i < serve_struct_sizes_array_num; ++i) {
    if (sza[i] && sza[i] != serve_struct_sizes_array[i]) {
      err("%s: plugin sizes array element %d mismatch: %zu instead of %zu",
          f, i, sza[i], serve_struct_sizes_array[i]);
      return;
    }
  }

  extra->plugin = iface;
  extra->plugin_data = (*extra->plugin->init)();
  info("loaded plugin %s", plugin_name);
}

int
ns_list_all_users_callback(
        void *user_data,
        int contest_id,
        unsigned char **p_xml)
{
  struct server_framework_state *state = (struct server_framework_state *) user_data;
  if (ns_open_ul_connection(state) < 0) return -1;

  if (userlist_clnt_list_all_users(ul_conn, ULS_LIST_STANDINGS_USERS,
                                   contest_id, p_xml) < 0) return -1;
  return 0;
}

static const unsigned char *role_strs[] =
  {
    __("Contestant"),
    __("Observer"),
    __("Examiner"),
    __("Chief examiner"),
    __("Coordinator"),
    __("Judge"),
    __("Administrator"),
    0,
  };
const unsigned char *
ns_unparse_role(int role)
{
  static unsigned char buf[32];
  if (role < 0 || role >= USER_ROLE_LAST) {
    snprintf(buf, sizeof(buf), "role_%d", role);
    return buf;
  }
  return gettext(role_strs[role]);
}

static void
html_role_select(FILE *fout, int role, int allow_admin,
                 const unsigned char *var_name)
{
  int i;
  const unsigned char *ss;
  int last_role = USER_ROLE_ADMIN;

  if (!var_name) var_name = "role";
  if (!allow_admin) last_role = USER_ROLE_COORDINATOR;
  if (role <= 0 || role > last_role) role = USER_ROLE_OBSERVER;
  fprintf(fout, "<select name=\"%s\">", var_name);
  for (i = 1; i <= last_role; i++) {
    ss = "";
    if (i == role) ss = " selected=\"1\"";
    fprintf(fout, "<option value=\"%d\"%s>%s</option>",
            i, ss, gettext(role_strs[i]));
  }
  fprintf(fout, "</select>\n");
}

unsigned char *
ns_url(unsigned char *buf, size_t size,
       const struct http_request_info *phr,
       int action, const char *format, ...)
{
  unsigned char fbuf[1024];
  unsigned char abuf[64];
  const unsigned char *sep = "";
  va_list args;

  fbuf[0] = 0;
  if (format && *format) {
    va_start(args, format);
    vsnprintf(fbuf, sizeof(fbuf), format, args);
    va_end(args);
  }
  if (fbuf[0]) sep = "&amp;";

  abuf[0] = 0;
  if (action > 0) snprintf(abuf, sizeof(abuf), "&amp;action=%d", action);

  snprintf(buf, size, "%s?SID=%016llx%s%s%s", phr->self_url,
           phr->session_id, abuf, sep, fbuf);
  return buf;
}

unsigned char *
ns_url_unescaped(
        unsigned char *buf,
        size_t size,
        const struct http_request_info *phr,
        int action,
        const char *format,
        ...)
{
  unsigned char fbuf[1024];
  unsigned char abuf[64];
  const unsigned char *sep = "";
  va_list args;

  fbuf[0] = 0;
  if (format && *format) {
    va_start(args, format);
    vsnprintf(fbuf, sizeof(fbuf), format, args);
    va_end(args);
  }
  if (fbuf[0]) sep = "&";

  abuf[0] = 0;
  if (action > 0) snprintf(abuf, sizeof(abuf), "&action=%d", action);

  snprintf(buf, size, "%s?SID=%016llx%s%s%s", phr->self_url,
           phr->session_id, abuf, sep, fbuf);
  return buf;
}

unsigned char *
ns_aref(unsigned char *buf, size_t size,
        const struct http_request_info *phr,
        int action, const char *format, ...)
{
  unsigned char fbuf[1024];
  unsigned char abuf[64];
  const unsigned char *sep = "";
  va_list args;

  fbuf[0] = 0;
  if (format && *format) {
    va_start(args, format);
    vsnprintf(fbuf, sizeof(fbuf), format, args);
    va_end(args);
  }
  if (fbuf[0]) sep = "&amp;";

  abuf[0] = 0;
  if (action > 0) snprintf(abuf, sizeof(abuf), "&amp;action=%d", action);

  snprintf(buf, size, "<a href=\"%s?SID=%016llx%s%s%s\">", phr->self_url,
           phr->session_id, abuf, sep, fbuf);
  return buf;
}

unsigned char *
ns_aref_2(unsigned char *buf, size_t size,
          const struct http_request_info *phr,
          const unsigned char *style,
          int action, const char *format, ...)
{
  unsigned char fbuf[1024];
  unsigned char abuf[64];
  unsigned char stbuf[128];
  const unsigned char *sep = "";
  va_list args;

  fbuf[0] = 0;
  if (format && *format) {
    va_start(args, format);
    vsnprintf(fbuf, sizeof(fbuf), format, args);
    va_end(args);
  }
  if (fbuf[0]) sep = "&amp;";

  abuf[0] = 0;
  if (action > 0) snprintf(abuf, sizeof(abuf), "&amp;action=%d", action);

  stbuf[0] = 0;
  if (style && *style) {
    snprintf(stbuf, sizeof(stbuf), " class=\"%s\"", style);
  }

  snprintf(buf, size, "<a href=\"%s?SID=%016llx%s%s%s\"%s>", phr->self_url,
           phr->session_id, abuf, sep, fbuf, stbuf);
  return buf;
}

#define BUTTON(a) ns_submit_button(bb, sizeof(bb), 0, a, 0)

unsigned char *
ns_submit_button(unsigned char *buf, size_t size,
                 const unsigned char *var_name, int action,
                 const unsigned char *label)
{
  unsigned char name_buf[64];
  const unsigned char *name_ptr;

  if (!var_name) var_name = "action";
  if (!label && action > 0 && action < NEW_SRV_ACTION_LAST)
    label = gettext(ns_submit_button_labels[action]);
  if (!label) label = "Submit";
  name_ptr = var_name;
  if (action > 0) {
    // IE bug mode :(
    snprintf(name_buf, sizeof(name_buf), "%s_%d", var_name, action);
    name_ptr = name_buf;
  }
  snprintf(buf, size,
           "<input type=\"submit\" name=\"%s\" value=\"%s\"/>",
           name_ptr, label);
  return buf;
}

void
ns_refresh_page(
        FILE *fout,
        struct http_request_info *phr,
        int new_action,
        const unsigned char *extra)
{
  unsigned char url[1024];

  if (extra && *extra) {
    ns_url_unescaped(url, sizeof(url), phr, new_action, "%s", extra);
  } else {
    ns_url_unescaped(url, sizeof(url), phr, new_action, 0);
  }

  if (phr->client_key) {
    fprintf(fout, "Set-Cookie: EJSID=%016llx; Path=/\n", phr->client_key);
  }
  fprintf(fout, "Location: %s\n\n", url);
}

void
ns_refresh_page_2(
        FILE *fout,
        ej_cookie_t client_key,
        const unsigned char *url)
{
  if (client_key) {
    fprintf(fout, "Set-Cookie: EJSID=%016llx; Path=/\n", client_key);
  }
  fprintf(fout, "Location: %s\n\n", url);
}

void
ns_check_contest_events(serve_state_t cs, const struct contest_desc *cnts)
{
  const struct section_global_data *global = cs->global;
  time_t start_time, stop_time, sched_time, duration, finish_time;

  run_get_times(cs->runlog_state, &start_time, &sched_time,
                &duration, &stop_time, &finish_time);

  if (start_time > 0 && finish_time > 0 && finish_time < start_time) {
    // this is not right, ignore this situation
    finish_time = 0;
  }

  if (!global->is_virtual) {
    if (start_time > 0 && stop_time <= 0 && duration <= 0 && finish_time > 0
        && cs->current_time >= finish_time) {
      /* the contest is over: contest_finish_time is expired! */
      info("CONTEST IS OVER");
      run_stop_contest(cs->runlog_state, finish_time);
      serve_invoke_stop_script(cs);
    } else if (start_time > 0 && stop_time <= 0 && duration > 0
               && cs->current_time >= start_time + duration){
      /* the contest is over: duration is expired! */
      info("CONTEST IS OVER");
      run_stop_contest(cs->runlog_state, start_time + duration);
      serve_invoke_stop_script(cs);
    } else if (sched_time > 0 && start_time <= 0
               && cs->current_time >= sched_time) {
      /* it's time to start! */
      info("CONTEST IS STARTED");
      run_start_contest(cs->runlog_state, sched_time);
      serve_invoke_start_script(cs);
      serve_update_standings_file(cs, cnts, 0);
    }
  }

  if (cs->event_first) serve_handle_events(ejudge_config, cnts, cs);
}

static void
privileged_page_login_page(FILE *fout, struct http_request_info *phr)
{
  const unsigned char *s;
  int r, n;
  unsigned char bbuf[1024];
  struct html_armor_buffer ab = HTML_ARMOR_INITIALIZER;

  l10n_setlocale(phr->locale_id);
  ns_header(fout, ns_fancy_priv_header, 0, 0, 0, 0, phr->locale_id, NULL, NULL_CLIENT_KEY, "Login page");
  html_start_form(fout, 1, phr->self_url, "");
  fprintf(fout, "<table>\n");
  fprintf(fout, "<tr><td>%s:</td><td><input type=\"text\" size=\"32\" name=\"login\"", _("Login"));
  if (ns_cgi_param(phr, "login", &s) > 0) {
    fprintf(fout, " value=\"%s\"", ARMOR(s));
  }
  fprintf(fout, "/></td></tr>\n");
  fprintf(fout, "<tr><td>%s:</td><td><input type=\"password\" size=\"32\" name=\"password\"", _("Password"));
  if (ns_cgi_param(phr, "password", &s) > 0) {
    fprintf(fout, " value=\"%s\"", ARMOR(s));
  }
  fprintf(fout, "/></td></tr>\n");
  fprintf(fout, "<tr><td>%s:</td><td><input type=\"text\" size=\"32\" name=\"contest_id\"", _("Contest"));
  if (phr->contest_id > 0) {
    fprintf(fout, " value=\"%d\"", phr->contest_id);
  }
  fprintf(fout, "/></td></tr>\n");
  if (!phr->role) {
    phr->role = USER_ROLE_OBSERVER;
    if (ns_cgi_param(phr, "role", &s) > 0) {
      if (sscanf(s, "%d%n", &r, &n) == 1 && !s[n]
          && r >= USER_ROLE_CONTESTANT && r < USER_ROLE_LAST)
        phr->role = r;
    }
  }
  fprintf(fout, "<tr><td>%s:</td><td>", _("Role"));
  html_role_select(fout, phr->role, 1, 0);
  fprintf(fout, "</td></tr>\n");
  fprintf(fout, "<tr><td>%s:</td><td>", _("Language"));
  l10n_html_locale_select(fout, phr->locale_id);
  fprintf(fout, "</td></tr>\n");
  fprintf(fout, "<tr><td>&nbsp;</td><td>%s</td></tr>\n",
          ns_submit_button(bbuf, sizeof(bbuf), "submit", 0, _("Submit")));
  fprintf(fout, "</table></form>\n");
  ns_footer(fout, 0, 0, phr->locale_id);
  l10n_setlocale(0);
  html_armor_free(&ab);
}

static void
html_error_status_page(FILE *fout,
                       struct http_request_info *phr,
                       const struct contest_desc *cnts,
                       struct contest_extra *extra,
                       const unsigned char *log_txt,
                       int back_action,
                       const char *format,
                       ...)
  __attribute__((format(printf,7,8)));
static void
html_error_status_page(FILE *fout,
                       struct http_request_info *phr,
                       const struct contest_desc *cnts,
                       struct contest_extra *extra,
                       const unsigned char *log_txt,
                       int back_action,
                       const char *format,
                       ...)
{
  unsigned char url[1024];
  struct html_armor_buffer ab = HTML_ARMOR_INITIALIZER;
  unsigned char urlextra[1024];
  va_list args;

  urlextra[0] = 0;
  if (format && *format) {
    va_start(args, format);
    vsnprintf(urlextra, sizeof(urlextra), format, args);
    va_end(args);
  }

  l10n_setlocale(phr->locale_id);
  ns_header(fout, extra->header_txt, 0, 0, 0, 0, phr->locale_id, NULL,
            NULL_CLIENT_KEY,
            _("Operation completed with errors"));
  if (extra->separator_txt && *extra->separator_txt) {
    fprintf(fout, "%s", ns_fancy_empty_status);
    ns_separator(fout, extra->separator_txt, cnts);
  }
  fprintf(fout, "<font color=\"red\"><pre>%s</pre></font>\n", ARMOR(log_txt));
  fprintf(fout, "<hr>%s%s</a>\n",
          ns_aref(url, sizeof(url), phr, back_action, "%s", urlextra),
          _("Back"));
  ns_footer(fout, extra->footer_txt, extra->copyright_txt, phr->locale_id);
  l10n_setlocale(0);
  html_armor_free(&ab);
}
                       
static void
privileged_page_cookie_login(FILE *fout,
                             struct http_request_info *phr)
{
  const struct contest_desc *cnts = 0;
  opcap_t caps;
  int r, n;
  const unsigned char *s = 0;

  if (phr->contest_id<=0 || contests_get(phr->contest_id, &cnts)<0 || !cnts)
    return ns_html_err_inv_param(fout, phr, 1, "invalid contest_id");
  if (!cnts->managed)
    return ns_html_err_inv_param(fout, phr, 1, "contest is not managed");
  if (!phr->role) {
    phr->role = USER_ROLE_OBSERVER;
    if (ns_cgi_param(phr, "role", &s) > 0) {
      if (sscanf(s, "%d%n", &r, &n) == 1 && !s[n]
          && r >= USER_ROLE_CONTESTANT && r < USER_ROLE_LAST)
        phr->role = r;
    }
  }
  if (phr->role <= USER_ROLE_CONTESTANT || phr->role >= USER_ROLE_LAST)
      return ns_html_err_no_perm(fout, phr, 1, "invalid role");
  if (!phr->session_id)
      return ns_html_err_no_perm(fout, phr, 1, "SID is undefined");    

  // analyze IP limitations
  if (phr->role == USER_ROLE_ADMIN) {
    // as for the master program
    if (!contests_check_master_ip(phr->contest_id, &phr->ip, phr->ssl_flag))
      return ns_html_err_no_perm(fout, phr, 1, "%s://%s is not allowed for MASTER for contest %d", ns_ssl_flag_str[phr->ssl_flag],
                                 xml_unparse_ipv6(&phr->ip), phr->contest_id);
  } else {
    // as for judge program
    if (!contests_check_judge_ip(phr->contest_id, &phr->ip, phr->ssl_flag))
      return ns_html_err_no_perm(fout, phr, 1, "%s://%s is not allowed for JUDGE for contest %d", ns_ssl_flag_str[phr->ssl_flag],
                                 xml_unparse_ipv6(&phr->ip), phr->contest_id);
  }

  if (ns_open_ul_connection(phr->fw_state) < 0)
    return ns_html_err_ul_server_down(fout, phr, 1, 0);

  xfree(phr->login); phr->login = 0;
  xfree(phr->name); phr->name = 0;
  if ((r = userlist_clnt_priv_cookie_login(ul_conn, ULS_PRIV_COOKIE_LOGIN,
                                           &phr->ip, phr->ssl_flag,
                                           phr->contest_id, phr->session_id, phr->client_key,
                                           phr->locale_id,
                                           phr->role, &phr->user_id,
                                           &phr->session_id, &phr->client_key,
                                           &phr->login,
                                           &phr->name)) < 0) {
    switch (-r) {
    case ULS_ERR_BAD_CONTEST_ID:
    case ULS_ERR_IP_NOT_ALLOWED:
    case ULS_ERR_NO_PERMS:
    case ULS_ERR_NOT_REGISTERED:
    case ULS_ERR_CANNOT_PARTICIPATE:
    case ULS_ERR_NO_COOKIE:
      return ns_html_err_no_perm(fout, phr, 1, "priv_login failed: %s",
                                 userlist_strerror(-r));
    case ULS_ERR_DISCONNECT:
      return ns_html_err_ul_server_down(fout, phr, 1, 0);
    default:
      return ns_html_err_internal_error(fout, phr, 1,
                                        "priv_login failed: %s",
                                        userlist_strerror(-r));
    }
  }

  // analyze permissions
  if (phr->role == USER_ROLE_ADMIN) {
    // as for the master program
    if (opcaps_find(&cnts->capabilities, phr->login, &caps) < 0
        || opcaps_check(caps, OPCAP_MASTER_LOGIN) < 0)
      return ns_html_err_no_perm(fout, phr, 1, "user %s does not have MASTER_LOGIN bit for contest %d", phr->login, phr->contest_id);
  } else if (phr->role == USER_ROLE_JUDGE) {
    // as for the judge program
    if (opcaps_find(&cnts->capabilities, phr->login, &caps) < 0
        || opcaps_check(caps, OPCAP_JUDGE_LOGIN) < 0)
      return ns_html_err_no_perm(fout, phr, 1, "user %s does not have JUDGE_LOGIN bit for contest %d", phr->login, phr->contest_id);
  } else {
    // user privileges checked locally
    if (nsdb_check_role(phr->user_id, phr->contest_id, phr->role) < 0)
      return ns_html_err_no_perm(fout, phr, 1, "user %s has no permission to login as role %d for contest %d", phr->login, phr->role, phr->contest_id);
  }

  ns_get_session(phr->session_id, phr->client_key, 0);
  ns_refresh_page(fout, phr, NEW_SRV_ACTION_MAIN_PAGE, 0);
}

static void
privileged_page_login(FILE *fout,
                      struct http_request_info *phr)
{
  const unsigned char *login, *password, *s;
  int r, n;
  const struct contest_desc *cnts = 0;
  opcap_t caps;

  if ((r = ns_cgi_param(phr, "login", &login)) < 0)
    return ns_html_err_inv_param(fout, phr, 1, "cannot parse login");
  if (!r || phr->action == NEW_SRV_ACTION_LOGIN_PAGE)
    return privileged_page_login_page(fout, phr);

  phr->login = xstrdup(login);
  if ((r = ns_cgi_param(phr, "password", &password)) <= 0)
    return ns_html_err_inv_param(fout, phr, 1, "cannot parse password");
  if (phr->contest_id<=0 || contests_get(phr->contest_id, &cnts)<0 || !cnts)
    return ns_html_err_inv_param(fout, phr, 1, "invalid contest_id");
  if (!cnts->managed)
    return ns_html_err_inv_param(fout, phr, 1, "contest is not managed");

  if (!phr->role) {
    phr->role = USER_ROLE_OBSERVER;
    if (ns_cgi_param(phr, "role", &s) > 0) {
      if (sscanf(s, "%d%n", &r, &n) == 1 && !s[n]
          && r >= USER_ROLE_CONTESTANT && r < USER_ROLE_LAST)
        phr->role = r;
    }
  }
  if (phr->role == USER_ROLE_CONTESTANT)
    return unprivileged_page_login(fout, phr, phr->locale_id);

  // analyze IP limitations
  if (phr->role == USER_ROLE_ADMIN) {
    // as for the master program
    if (!contests_check_master_ip(phr->contest_id, &phr->ip, phr->ssl_flag))
      return ns_html_err_no_perm(fout, phr, 1, "%s://%s is not allowed for MASTER for contest %d", ns_ssl_flag_str[phr->ssl_flag],
                                 xml_unparse_ipv6(&phr->ip), phr->contest_id);
  } else {
    // as for judge program
    if (!contests_check_judge_ip(phr->contest_id, &phr->ip, phr->ssl_flag))
      return ns_html_err_no_perm(fout, phr, 1, "%s://%s is not allowed for JUDGE for contest %d", ns_ssl_flag_str[phr->ssl_flag],
                                 xml_unparse_ipv6(&phr->ip), phr->contest_id);
  }

  if (ns_open_ul_connection(phr->fw_state) < 0)
    return ns_html_err_ul_server_down(fout, phr, 1, 0);
  if ((r = userlist_clnt_priv_login(ul_conn, ULS_PRIV_CHECK_USER,
                                    &phr->ip, phr->client_key,
                                    phr->ssl_flag, phr->contest_id,
                                    phr->locale_id, phr->role, login,
                                    password, &phr->user_id,
                                    &phr->session_id, &phr->client_key,
                                    0, &phr->name)) < 0) {
    switch (-r) {
    case ULS_ERR_INVALID_LOGIN:
    case ULS_ERR_INVALID_PASSWORD:
    case ULS_ERR_BAD_CONTEST_ID:
    case ULS_ERR_IP_NOT_ALLOWED:
    case ULS_ERR_NO_PERMS:
    case ULS_ERR_NOT_REGISTERED:
    case ULS_ERR_CANNOT_PARTICIPATE:
    case ULS_ERR_NO_COOKIE:
      return ns_html_err_no_perm(fout, phr, 1, "priv_login failed: %s",
                                 userlist_strerror(-r));
    case ULS_ERR_DISCONNECT:
      return ns_html_err_ul_server_down(fout, phr, 1, 0);
    default:
      return ns_html_err_internal_error(fout, phr, 1,
                                        "priv_login failed: %s",
                                        userlist_strerror(-r));
    }
  }

  // analyze permissions
  if (phr->role == USER_ROLE_ADMIN) {
    // as for the master program
    if (opcaps_find(&cnts->capabilities, phr->login, &caps) < 0
        || opcaps_check(caps, OPCAP_MASTER_LOGIN) < 0)
      return ns_html_err_no_perm(fout, phr, 1, "user %s does not have MASTER_LOGIN bit for contest %d", phr->login, phr->contest_id);
  } else if (phr->role == USER_ROLE_JUDGE) {
    // as for the judge program
    if (opcaps_find(&cnts->capabilities, phr->login, &caps) < 0
        || opcaps_check(caps, OPCAP_JUDGE_LOGIN) < 0)
      return ns_html_err_no_perm(fout, phr, 1, "user %s does not have JUDGE_LOGIN bit for contest %d", phr->login, phr->contest_id);
  } else {
    // user privileges checked locally
    if (nsdb_check_role(phr->user_id, phr->contest_id, phr->role) < 0)
      return ns_html_err_no_perm(fout, phr, 1, "user %s has no permission to login as role %d for contest %d", phr->login, phr->role, phr->contest_id);
  }

  ns_get_session(phr->session_id, phr->client_key, 0);
  ns_refresh_page(fout, phr, NEW_SRV_ACTION_MAIN_PAGE, 0);
}

static void
priv_parse_user_id_range(
        struct http_request_info *phr,
        int *p_first_id,
        int *p_last_id)
{
  int first = 0, last = -1, x, y;

  if (ns_cgi_param_int_opt(phr, "first_user_id", &x, 0) < 0) goto done;
  if (ns_cgi_param_int_opt(phr, "last_user_id", &y, -1) < 0) goto done;
  if (x <= 0 || y <= 0 || x > y || y - x > 100000) goto done;

  first = x;
  last = y;

 done:
  if (p_first_id) *p_first_id = first;
  if (p_last_id) *p_last_id = last;
}

static int
priv_registration_operation(FILE *fout,
                            FILE *log_f,
                            struct http_request_info *phr,
                            const struct contest_desc *cnts,
                            struct contest_extra *extra)
{
  const serve_state_t cs = extra->serve_state;
  int i, x, n, new_status, cmd, flag;
  intarray_t uset;
  const unsigned char *s;
  int retcode = 0;
  struct html_armor_buffer ab = HTML_ARMOR_INITIALIZER;
  unsigned char *disq_comment = 0;
  int first_user_id = 0, last_user_id  = -1;

  // extract the selected set of users
  memset(&uset, 0, sizeof(uset));
  for (i = 0; i < phr->param_num; i++) {
    if (strncmp(phr->param_names[i], "user_", 5) != 0) continue;
    if (sscanf((s = phr->param_names[i] + 5), "%d%n", &x, &n) != 1
        || s[n] || x <= 0) {
      ns_html_err_inv_param(fout, phr, 1, "invalid parameter name %s",
                            ARMOR(phr->param_names[i]));
      retcode = -1;
      goto cleanup;
    }
    XEXPAND2(uset);
    uset.v[uset.u++] = x;
  }

  priv_parse_user_id_range(phr, &first_user_id, &last_user_id);
  if (first_user_id > 0) {
    for (i = first_user_id; i <= last_user_id; i++) {
      XEXPAND2(uset);
      uset.v[uset.u++] = i;
    }
  }

  if (phr->action == NEW_SRV_ACTION_USERS_SET_DISQUALIFIED) {
    if (ns_cgi_param(phr, "disq_comment", &s) < 0) {
      ns_html_err_inv_param(fout, phr, 1, "invalid parameter disq_comment");
      retcode = -1;
      goto cleanup;
    }
    disq_comment = text_area_process_string(s, 0, 0);
  }

  // FIXME: probably we need to sort user_ids and remove duplicates

  if (ns_open_ul_connection(phr->fw_state) < 0) {
    ns_html_err_ul_server_down(fout, phr, 1, 0);
    retcode = -1;
    goto cleanup;
  }

  for (i = 0; i < uset.u; i++) {
    switch (phr->action) {
    case NEW_SRV_ACTION_USERS_REMOVE_REGISTRATIONS:
      n = userlist_clnt_change_registration(ul_conn, uset.v[i],
                                            phr->contest_id, -2, 0, 0);
      if (n < 0) {
        ns_error(log_f, NEW_SRV_ERR_USER_REMOVAL_FAILED,
                 uset.v[i], phr->contest_id, userlist_strerror(-n));
      }
      break;
    case NEW_SRV_ACTION_USERS_SET_PENDING:
    case NEW_SRV_ACTION_USERS_SET_OK:
    case NEW_SRV_ACTION_USERS_SET_REJECTED:
      switch (phr->action) {
      case NEW_SRV_ACTION_USERS_SET_PENDING: 
        new_status = USERLIST_REG_PENDING;
        break;
      case NEW_SRV_ACTION_USERS_SET_OK:
        new_status = USERLIST_REG_OK;
        break;
      case NEW_SRV_ACTION_USERS_SET_REJECTED:
        new_status = USERLIST_REG_REJECTED;
        break;
      default:
        abort();
      }
      n = userlist_clnt_change_registration(ul_conn, uset.v[i],
                                            phr->contest_id, new_status, 0, 0);
      if (n < 0) {
        ns_error(log_f, NEW_SRV_ERR_USER_STATUS_CHANGE_FAILED,
                 uset.v[i], phr->contest_id, userlist_strerror(-n));
      }
      break;

    case NEW_SRV_ACTION_USERS_SET_INVISIBLE:
    case NEW_SRV_ACTION_USERS_CLEAR_INVISIBLE:
    case NEW_SRV_ACTION_USERS_SET_BANNED:
    case NEW_SRV_ACTION_USERS_CLEAR_BANNED:
    case NEW_SRV_ACTION_USERS_SET_LOCKED:
    case NEW_SRV_ACTION_USERS_CLEAR_LOCKED:
    case NEW_SRV_ACTION_USERS_SET_INCOMPLETE:
    case NEW_SRV_ACTION_USERS_CLEAR_INCOMPLETE:
    case NEW_SRV_ACTION_USERS_SET_DISQUALIFIED:
    case NEW_SRV_ACTION_USERS_CLEAR_DISQUALIFIED:
      switch (phr->action) {
      case NEW_SRV_ACTION_USERS_SET_INVISIBLE:
        cmd = 1;
        flag = USERLIST_UC_INVISIBLE;
        break;
      case NEW_SRV_ACTION_USERS_CLEAR_INVISIBLE:
        cmd = 2;
        flag = USERLIST_UC_INVISIBLE;
        break;
      case NEW_SRV_ACTION_USERS_SET_BANNED:
        cmd = 1;
        flag = USERLIST_UC_BANNED;
        break;
      case NEW_SRV_ACTION_USERS_CLEAR_BANNED:
        cmd = 2;
        flag = USERLIST_UC_BANNED;
        break;
      case NEW_SRV_ACTION_USERS_SET_LOCKED:
        cmd = 1;
        flag = USERLIST_UC_LOCKED;
        break;
      case NEW_SRV_ACTION_USERS_CLEAR_LOCKED:
        cmd = 2;
        flag = USERLIST_UC_LOCKED;
        break;
      case NEW_SRV_ACTION_USERS_SET_INCOMPLETE:
        cmd = 1;
        flag = USERLIST_UC_INCOMPLETE;
        break;
      case NEW_SRV_ACTION_USERS_CLEAR_INCOMPLETE:
        cmd = 2;
        flag = USERLIST_UC_INCOMPLETE;
        break;
      case NEW_SRV_ACTION_USERS_SET_DISQUALIFIED:
        cmd = 1;
        flag = USERLIST_UC_DISQUALIFIED;
        team_extra_set_disq_comment(cs->team_extra_state, uset.v[i],
                                    disq_comment);
        break;
      case NEW_SRV_ACTION_USERS_CLEAR_DISQUALIFIED:
        cmd = 2;
        flag = USERLIST_UC_DISQUALIFIED;
        break;
      default:
        abort();
      }
      n = userlist_clnt_change_registration(ul_conn, uset.v[i],
                                            phr->contest_id, -1, cmd,
                                            flag);
      if (n < 0) {
        ns_error(log_f, NEW_SRV_ERR_USER_FLAGS_CHANGE_FAILED,
                 uset.v[i], phr->contest_id, userlist_strerror(-n));
      }
      break;

    default:
      ns_html_err_inv_param(fout, phr, 1, "invalid action %d", phr->action);
      retcode = -1;
      goto cleanup;
    }
  }

  if (phr->action == NEW_SRV_ACTION_USERS_SET_DISQUALIFIED) {
    team_extra_flush(cs->team_extra_state);
  }

 cleanup:
  xfree(disq_comment);
  xfree(uset.v);
  html_armor_free(&ab);
  return retcode;
}

static int
priv_add_user_by_user_id(FILE *fout,
                         FILE *log_f,
                         struct http_request_info *phr,
                         const struct contest_desc *cnts,
                         struct contest_extra *extra)
{
  const unsigned char *s;
  int x, n, r;
  int retval = 0;

  if ((r = ns_cgi_param(phr, "add_user_id", &s)) < 0 || !s
      || sscanf(s, "%d%n", &x, &n) != 1 || s[n] || x <= 0)
    FAIL(NEW_SRV_ERR_INV_USER_ID);

  if (ns_open_ul_connection(phr->fw_state) < 0) {
    ns_html_err_ul_server_down(fout, phr, 1, 0);
    retval = -1;
    goto cleanup;
  }

  r = userlist_clnt_register_contest(ul_conn, ULS_PRIV_REGISTER_CONTEST,
                                     x, phr->contest_id, &phr->ip,
                                     phr->ssl_flag);
  if (r < 0) {
    ns_error(log_f, NEW_SRV_ERR_REGISTRATION_FAILED, userlist_strerror(-r));
    goto cleanup;
  }

 cleanup:
  return retval;
}

static int
priv_add_user_by_login(FILE *fout,
                       FILE *log_f,
                       struct http_request_info *phr,
                       const struct contest_desc *cnts,
                       struct contest_extra *extra)
{
  const unsigned char *s;
  int r, user_id;
  struct html_armor_buffer ab = HTML_ARMOR_INITIALIZER;
  int retval = 0;

  if ((r = ns_cgi_param(phr, "add_login", &s)) < 0 || !s) {
    ns_error(log_f, NEW_SRV_ERR_INV_USER_LOGIN);
    goto cleanup;
  }
  if (ns_open_ul_connection(phr->fw_state) < 0) {
    ns_html_err_ul_server_down(fout, phr, 1, 0);
    retval = -1;
    goto cleanup;
  }
  if ((r = userlist_clnt_lookup_user(ul_conn, s, 0, &user_id, 0)) < 0) {
    ns_error(log_f, NEW_SRV_ERR_USER_LOGIN_NONEXISTANT, ARMOR(s));
    goto cleanup;
  }
  if ((r = userlist_clnt_register_contest(ul_conn, ULS_PRIV_REGISTER_CONTEST,
                                          user_id, phr->contest_id,
                                          &phr->ip, phr->ssl_flag)) < 0) {
    ns_error(log_f, NEW_SRV_ERR_REGISTRATION_FAILED, userlist_strerror(-r));
    goto cleanup;
  }

 cleanup:
  html_armor_free(&ab);
  return retval;
}

static int
priv_priv_user_operation(FILE *fout,
                         FILE *log_f,
                         struct http_request_info *phr,
                         const struct contest_desc *cnts,
                         struct contest_extra *extra)
{
  int i, x, n, role = 0;
  intarray_t uset;
  const unsigned char *s;
  struct html_armor_buffer ab = HTML_ARMOR_INITIALIZER;
  int retval = 0;
  int first_user_id = 0, last_user_id = -1;

  // extract the selected set of users
  memset(&uset, 0, sizeof(uset));
  for (i = 0; i < phr->param_num; i++) {
    if (strncmp(phr->param_names[i], "user_", 5) != 0) continue;
    if (sscanf((s = phr->param_names[i] + 5), "%d%n", &x, &n) != 1
        || s[n] || x <= 0) {
      ns_html_err_inv_param(fout, phr, 1, "invalid parameter name %s",
                            ARMOR(phr->param_names[i]));
      retval = -1;
      goto cleanup;
    }
    XEXPAND2(uset);
    uset.v[uset.u++] = x;
  }

  priv_parse_user_id_range(phr, &first_user_id, &last_user_id);
  if (first_user_id > 0) {
    for (i = first_user_id; i <= last_user_id; i++) {
      XEXPAND2(uset);
      uset.v[uset.u++] = i;
    }
  }

  // FIXME: probably we need to sort user_ids and remove duplicates

  switch (phr->action) {
  case NEW_SRV_ACTION_PRIV_USERS_ADD_OBSERVER:
  case NEW_SRV_ACTION_PRIV_USERS_DEL_OBSERVER:
    role = USER_ROLE_OBSERVER;
    break;
  case NEW_SRV_ACTION_PRIV_USERS_ADD_EXAMINER:
  case NEW_SRV_ACTION_PRIV_USERS_DEL_EXAMINER:
    role = USER_ROLE_EXAMINER;
    break;
  case NEW_SRV_ACTION_PRIV_USERS_ADD_CHIEF_EXAMINER:
  case NEW_SRV_ACTION_PRIV_USERS_DEL_CHIEF_EXAMINER:
    role = USER_ROLE_CHIEF_EXAMINER;
    break;
  case NEW_SRV_ACTION_PRIV_USERS_ADD_COORDINATOR:
  case NEW_SRV_ACTION_PRIV_USERS_DEL_COORDINATOR:
    role = USER_ROLE_COORDINATOR;
    break;
  }

  for (i = 0; i < uset.u; i++) {
    switch (phr->action) {
    case NEW_SRV_ACTION_PRIV_USERS_REMOVE:
      if (nsdb_priv_remove_user(uset.v[i], phr->contest_id) < 0) {
        ns_error(log_f, NEW_SRV_ERR_PRIV_USER_REMOVAL_FAILED,
                 uset.v[i], phr->contest_id);
      }
      break;

    case NEW_SRV_ACTION_PRIV_USERS_ADD_OBSERVER:
    case NEW_SRV_ACTION_PRIV_USERS_ADD_EXAMINER:
    case NEW_SRV_ACTION_PRIV_USERS_ADD_CHIEF_EXAMINER:
    case NEW_SRV_ACTION_PRIV_USERS_ADD_COORDINATOR:
      if (nsdb_add_role(uset.v[i], phr->contest_id, role) < 0) {
        ns_error(log_f, NEW_SRV_ERR_PRIV_USER_ROLE_ADD_FAILED,
                 role, uset.v[i], phr->contest_id);
      }
      break;

    case NEW_SRV_ACTION_PRIV_USERS_DEL_OBSERVER:
    case NEW_SRV_ACTION_PRIV_USERS_DEL_EXAMINER:
    case NEW_SRV_ACTION_PRIV_USERS_DEL_CHIEF_EXAMINER:
    case NEW_SRV_ACTION_PRIV_USERS_DEL_COORDINATOR:
      if (nsdb_del_role(uset.v[i], phr->contest_id, role) < 0) {
        ns_error(log_f, NEW_SRV_ERR_PRIV_USER_ROLE_DEL_FAILED,
                 role, uset.v[i], phr->contest_id);
      }
      break;

    default:
      ns_html_err_inv_param(fout, phr, 1, "invalid action %d", phr->action);
      retval = -1;
      goto cleanup;
    }
  }

 cleanup:
  xfree(uset.v);
  html_armor_free(&ab);
  return retval;
}

static int
priv_add_priv_user_by_user_id(FILE *fout,
                              FILE *log_f,
                              struct http_request_info *phr,
                              const struct contest_desc *cnts,
                              struct contest_extra *extra)
{
  const unsigned char *s;
  int user_id, n, r, add_role;

  if ((r = ns_cgi_param(phr, "add_user_id", &s)) < 0 || !s
      || sscanf(s, "%d%n", &user_id, &n) != 1 || s[n] || user_id <= 0) {
    ns_error(log_f, NEW_SRV_ERR_INV_USER_ID);
    goto cleanup;
  }
  if ((r = ns_cgi_param(phr, "add_role_2", &s)) < 0 || !s
      || sscanf(s, "%d%n", &add_role, &n) != 1 || s[n]
      || add_role < USER_ROLE_OBSERVER || add_role > USER_ROLE_COORDINATOR) {
    ns_error(log_f, NEW_SRV_ERR_INV_USER_ROLE);
    goto cleanup;
  }

  if (nsdb_add_role(user_id, phr->contest_id, add_role) < 0) {
    ns_error(log_f, NEW_SRV_ERR_PRIV_USER_ROLE_ADD_FAILED,
             add_role, user_id, phr->contest_id);
    goto cleanup;
  }

 cleanup:
  return 0;
}

static int
priv_add_priv_user_by_login(FILE *fout,
                            FILE *log_f,
                            struct http_request_info *phr,
                            const struct contest_desc *cnts,
                            struct contest_extra *extra)
{
  const unsigned char *s, *login;
  int r, user_id, add_role, n;
  struct html_armor_buffer ab = HTML_ARMOR_INITIALIZER;
  int retval = 0;

  if ((r = ns_cgi_param(phr, "add_login", &login)) < 0 || !s) {
    ns_error(log_f, NEW_SRV_ERR_INV_USER_LOGIN);
    goto cleanup;
  }
  if ((r = ns_cgi_param(phr, "add_role_1", &s)) < 0 || !s
      || sscanf(s, "%d%n", &add_role, &n) != 1 || s[n]
      || add_role < USER_ROLE_OBSERVER || add_role > USER_ROLE_COORDINATOR) {
    ns_error(log_f, NEW_SRV_ERR_INV_USER_ROLE);
    goto cleanup;
  }
  if (ns_open_ul_connection(phr->fw_state) < 0) {
    ns_html_err_ul_server_down(fout, phr, 1, 0);
    retval = -1;
    goto cleanup;
  }
  if ((r = userlist_clnt_lookup_user(ul_conn, login, 0, &user_id, 0)) < 0) {
    ns_error(log_f, NEW_SRV_ERR_USER_LOGIN_NONEXISTANT, ARMOR(s));
    goto cleanup;
  }
  if (nsdb_add_role(user_id, phr->contest_id, add_role) < 0) {
    ns_error(log_f, NEW_SRV_ERR_PRIV_USER_ROLE_ADD_FAILED,
                    add_role, user_id, phr->contest_id);
    goto cleanup;
  }

 cleanup:
  html_armor_free(&ab);
  return retval;
}

static int
priv_user_operation(FILE *fout,
                    FILE *log_f,
                    struct http_request_info *phr,
                    const struct contest_desc *cnts,
                    struct contest_extra *extra)
{
  serve_state_t cs = extra->serve_state;
  const unsigned char *s;
  int retval = 0, user_id, n, new_status;
  const struct team_extra *t_extra = 0;

  if (ns_cgi_param(phr, "user_id", &s) <= 0
      || sscanf(s, "%d%n", &user_id, &n) != 1 || s[n]
      || user_id <= 0 || !teamdb_lookup(cs->teamdb_state, user_id))
    FAIL(NEW_SRV_ERR_INV_USER_ID);

  switch (phr->action) {
  case NEW_SRV_ACTION_USER_CHANGE_STATUS:
    if (ns_cgi_param(phr, "status", &s) <= 0
        || sscanf(s, "%d%n", &new_status, &n) != 1 || s[n]
        || new_status < 0 || new_status >= cs->global->contestant_status_num)
      FAIL(NEW_SRV_ERR_INV_STATUS);
    if (opcaps_check(phr->caps, OPCAP_EDIT_REG) < 0)
      FAIL(NEW_SRV_ERR_PERMISSION_DENIED);
    if (!(t_extra = team_extra_get_entry(cs->team_extra_state, user_id)))
      FAIL(NEW_SRV_ERR_DISK_READ_ERROR);
    if (t_extra->status == new_status) goto cleanup;
    team_extra_set_status(cs->team_extra_state, user_id, new_status);
    team_extra_flush(cs->team_extra_state);
    break;
  }

 cleanup:
  return retval;
}

static int
priv_user_issue_warning(
        FILE *fout,
        FILE *log_f,
        struct http_request_info *phr,
        const struct contest_desc *cnts,
        struct contest_extra *extra)
{
  serve_state_t cs = extra->serve_state;
  int retval = 0;
  const unsigned char *s;
  int user_id, n;
  unsigned char *warn_txt = 0, *cmt_txt = 0;
  size_t warn_len = 0, cmt_len = 0;

  if (opcaps_check(phr->caps, OPCAP_EDIT_REG) < 0)
    FAIL(NEW_SRV_ERR_PERMISSION_DENIED);

  /* user_id, warn_text, warn_comment */
  if (ns_cgi_param(phr, "user_id", &s) <= 0
      || sscanf(s, "%d%n", &user_id, &n) != 1 || s[n]
      || teamdb_lookup(cs->teamdb_state, user_id) <= 0)
    FAIL(NEW_SRV_ERR_INV_USER_ID);
  if ((n = ns_cgi_param(phr, "warn_text", &s)) < 0)
    FAIL(NEW_SRV_ERR_INV_WARN_TEXT);
  if (!n) FAIL(NEW_SRV_ERR_WARN_TEXT_EMPTY);
  warn_len = strlen(warn_txt = dos2unix_str(s));
  while (warn_len > 0 && isspace(warn_txt[warn_len - 1])) warn_len--;
  warn_txt[warn_len] = 0;
  if (!warn_len) FAIL(NEW_SRV_ERR_WARN_TEXT_EMPTY);
  if ((n = ns_cgi_param(phr, "warn_comment", &s)) < 0)
    FAIL(NEW_SRV_ERR_INV_WARN_CMT);
  if (!n) {
    cmt_len = strlen(cmt_txt = xstrdup(""));
  } else {
    cmt_len = strlen(cmt_txt = dos2unix_str(s));
    while (cmt_len > 0 && isspace(cmt_txt[cmt_len - 1])) cmt_len--;
    cmt_txt[cmt_len] = 0;
  }

  team_extra_append_warning(cs->team_extra_state, user_id, phr->user_id,
                            &phr->ip, cs->current_time, warn_txt, cmt_txt);
  team_extra_flush(cs->team_extra_state);

 cleanup:
  xfree(warn_txt);
  xfree(cmt_txt);
  return retval;
}

static int
priv_user_toggle_flags(
        FILE *fout,
        FILE *log_f,
        struct http_request_info *phr,
        const struct contest_desc *cnts,
        struct contest_extra *extra)
{
  int retval = 0, flag, user_id, n;

  if (phr->role < USER_ROLE_JUDGE)
    FAIL(NEW_SRV_ERR_PERMISSION_DENIED);
  if (opcaps_check(phr->caps, OPCAP_EDIT_REG) < 0)
    FAIL(NEW_SRV_ERR_PERMISSION_DENIED);
  if (ns_cgi_param_int(phr, "user_id", &user_id) < 0)
    FAIL(NEW_SRV_ERR_INV_USER_ID);

  switch (phr->action) {
  case NEW_SRV_ACTION_TOGGLE_VISIBILITY:
    flag = USERLIST_UC_INVISIBLE;
    break;
  case NEW_SRV_ACTION_TOGGLE_BAN:
    flag = USERLIST_UC_BANNED;
    break;
  case NEW_SRV_ACTION_TOGGLE_LOCK:
    flag = USERLIST_UC_LOCKED;
    break;
  case NEW_SRV_ACTION_TOGGLE_INCOMPLETENESS:
    flag = USERLIST_UC_INCOMPLETE;
    break;
  default:
    abort();
  }

  if (ns_open_ul_connection(phr->fw_state) < 0) {
    ns_html_err_ul_server_down(fout, phr, 1, 0);
    retval = -1;
    goto cleanup;
  }
  n = userlist_clnt_change_registration(ul_conn, user_id, phr->contest_id,
                                        -1, 3, flag);
  if (n < 0) {
    ns_error(log_f, NEW_SRV_ERR_USER_FLAGS_CHANGE_FAILED,
             user_id, phr->contest_id, userlist_strerror(-n));
    retval = -1;
    goto cleanup;
  }

  snprintf(phr->next_extra, sizeof(phr->next_extra), "user_id=%d", user_id);
  retval = NEW_SRV_ACTION_VIEW_USER_INFO;

 cleanup:
  return retval;
}

static int
priv_force_start_virtual(
        FILE *fout,
        FILE *log_f,
        struct http_request_info *phr,
        const struct contest_desc *cnts,
        struct contest_extra *extra)
{
  serve_state_t cs = extra->serve_state;
  const struct section_global_data *global = cs->global;
  const unsigned char *s;
  int retval = 0, i, n, x;
  struct html_armor_buffer ab = HTML_ARMOR_INITIALIZER;
  intarray_t uset;
  struct timeval tt;
  long nsec;
  int run_id;
  int first_user_id = 0, last_user_id = -1;

  if (phr->role < USER_ROLE_JUDGE)
    FAIL(NEW_SRV_ERR_PERMISSION_DENIED);
  if (opcaps_check(phr->caps, OPCAP_EDIT_REG) < 0)
    FAIL(NEW_SRV_ERR_PERMISSION_DENIED);
  if (!global->is_virtual)
    FAIL(NEW_SRV_ERR_NOT_VIRTUAL);

  memset(&uset, 0, sizeof(uset));
  for (i = 0; i < phr->param_num; i++) {
    if (strncmp(phr->param_names[i], "user_", 5) != 0) continue;
    if (sscanf((s = phr->param_names[i] + 5), "%d%n", &x, &n) != 1
        || s[n] || x <= 0) {
      ns_html_err_inv_param(fout, phr, 1, "invalid parameter name %s",
                            ARMOR(phr->param_names[i]));
      retval = -1;
      goto cleanup;
    }
    if (teamdb_lookup(cs->teamdb_state, x) <= 0)
      FAIL(NEW_SRV_ERR_INV_USER_ID);

    XEXPAND2(uset);
    uset.v[uset.u++] = x;
  }

  priv_parse_user_id_range(phr, &first_user_id, &last_user_id);
  if (first_user_id > 0) {
    for (i = first_user_id; i <= last_user_id; i++) {
      XEXPAND2(uset);
      uset.v[uset.u++] = i;
    }
  }

  gettimeofday(&tt, 0);
  nsec = tt.tv_usec * 1000;
  // FIXME: it's a bit risky, need to check the database...
  if (nsec + uset.u >= NSEC_MAX + 1) nsec = NSEC_MAX - 1 - uset.u;

  for (i = 0; i < uset.u; i++, nsec++) {
    run_id = run_virtual_start(cs->runlog_state, uset.v[i], tt.tv_sec,0,0,nsec);
    if (run_id >= 0) serve_move_files_to_insert_run(cs, run_id);
  }

 cleanup:
  xfree(uset.v);
  html_armor_free(&ab);
  return retval;
}

static int
priv_user_disqualify(
        FILE *fout,
        FILE *log_f,
        struct http_request_info *phr,
        const struct contest_desc *cnts,
        struct contest_extra *extra)
{
  serve_state_t cs = extra->serve_state;
  int retval = 0;
  const unsigned char *s;
  int user_id, n;
  unsigned char *warn_txt = 0;

  if (opcaps_check(phr->caps, OPCAP_EDIT_REG) < 0)
    FAIL(NEW_SRV_ERR_PERMISSION_DENIED);

  /* user_id, disq_comment */
  if (ns_cgi_param(phr, "user_id", &s) <= 0
      || sscanf(s, "%d%n", &user_id, &n) != 1 || s[n]
      || teamdb_lookup(cs->teamdb_state, user_id) <= 0)
    FAIL(NEW_SRV_ERR_INV_USER_ID);
  if ((n = ns_cgi_param(phr, "disq_comment", &s)) < 0)
    FAIL(NEW_SRV_ERR_INV_WARN_TEXT);
  warn_txt = text_area_process_string(s, 0, 0);

  if (ns_open_ul_connection(phr->fw_state) < 0) {
    ns_html_err_ul_server_down(fout, phr, 1, 0);
    retval = -1;
    goto cleanup;
  }
  n = userlist_clnt_change_registration(ul_conn, user_id,
                                        phr->contest_id, -1, 1,
                                        USERLIST_UC_DISQUALIFIED);
  if (n < 0) {
    ns_error(log_f, NEW_SRV_ERR_USER_FLAGS_CHANGE_FAILED,
             user_id, phr->contest_id, userlist_strerror(-n));
    retval = -1;
    goto cleanup;
  }

  team_extra_set_disq_comment(cs->team_extra_state, user_id, warn_txt);
  team_extra_flush(cs->team_extra_state);

 cleanup:
  xfree(warn_txt);
  return retval;
}

static void
do_schedule(FILE *log_f,
            struct http_request_info *phr,
            serve_state_t cs,
            const struct contest_desc *cnts)
{
  const unsigned char *s = 0;
  time_t sloc = 0, start_time, stop_time;

  if (ns_cgi_param(phr, "sched_time", &s) <= 0) {
    ns_error(log_f, NEW_SRV_ERR_INV_TIME_SPEC);
    return;
  }

  if (xml_parse_date(NULL, 0, 0, 0, s, &sloc) < 0 || sloc < 0) {
    ns_error(log_f, NEW_SRV_ERR_INV_TIME_SPEC);
    return;
  }

  if (sloc > 0) {
    run_get_times(cs->runlog_state, &start_time, 0, 0, &stop_time, 0);
    if (stop_time > 0) {
      ns_error(log_f, NEW_SRV_ERR_CONTEST_ALREADY_FINISHED);
      return;
    }
    if (start_time > 0) {
      ns_error(log_f, NEW_SRV_ERR_CONTEST_ALREADY_STARTED);
      return;
    }
  }
  run_sched_contest(cs->runlog_state, sloc);
  serve_update_standings_file(cs, cnts, 0);
  serve_update_status_file(cs, 1);
}

static void
do_change_duration(FILE *log_f,
                   struct http_request_info *phr,
                   serve_state_t cs,
                   const struct contest_desc *cnts)
{
  const unsigned char *s = 0;
  int dh = 0, dm = 0, n, d;
  time_t start_time, stop_time;

  if (ns_cgi_param(phr, "dur", &s) <= 0) {
    ns_error(log_f, NEW_SRV_ERR_INV_DUR_SPEC);
    return;
  }
  if (sscanf(s, "%d:%d%n", &dh, &dm, &n) == 2 && !s[n]) {
  } else if (sscanf(s, "%d%n", &dh, &n) == 1 && !s[n]) {
    dm = 0;
  } else {
    ns_error(log_f, NEW_SRV_ERR_INV_DUR_SPEC);
    return;
  }
  d = dh * 60 + dm;
  if (d < 0 || d > 1000000) {
    ns_error(log_f, NEW_SRV_ERR_INV_DUR_SPEC);
    return;
  }
  d *= 60;

  run_get_times(cs->runlog_state, &start_time, 0, 0, &stop_time, 0);

  if (stop_time > 0 && !cs->global->enable_continue) {
    ns_error(log_f, NEW_SRV_ERR_CONTEST_ALREADY_FINISHED);
    return;
  }
  if (d > 0 && start_time && start_time + d < cs->current_time) {
    ns_error(log_f, NEW_SRV_ERR_DUR_TOO_SMALL);
    return;
  }

  run_set_duration(cs->runlog_state, d);
  serve_update_standings_file(cs, cnts, 0);
  serve_update_status_file(cs, 1);
  return;
}

static void
do_change_finish_time(
        FILE *log_f,
        struct http_request_info *phr,
        serve_state_t cs,
        const struct contest_desc *cnts)
{
  const unsigned char *s = 0;
  time_t ft = 0, start_time = 0, stop_time = 0;

  if (ns_cgi_param(phr, "finish_time", &s) <= 0) {
    ns_error(log_f, NEW_SRV_ERR_INV_TIME_SPEC);
    return;
  }
  if (!is_empty_string(s)) {
    if (xml_parse_date(NULL, 0, 0, 0, s, &ft) < 0 || ft < 0) {
      ns_error(log_f, NEW_SRV_ERR_INV_TIME_SPEC);
      return;
    }
    if (ft < cs->current_time) {
      ns_error(log_f, NEW_SRV_ERR_INV_TIME_SPEC);
      return;
    }
  }

  run_get_times(cs->runlog_state, &start_time, 0, 0, &stop_time, 0);
  if (stop_time > 0) {
    ns_error(log_f, NEW_SRV_ERR_CONTEST_ALREADY_FINISHED);
    return;
  }

  run_set_finish_time(cs->runlog_state, ft);
  serve_update_standings_file(cs, cnts, 0);
  serve_update_status_file(cs, 1);
}

static int
priv_contest_operation(FILE *fout,
                       FILE *log_f,
                       struct http_request_info *phr,
                       const struct contest_desc *cnts,
                       struct contest_extra *extra)
{
  serve_state_t cs = extra->serve_state;
  const struct section_global_data *global = cs->global;
  opcap_t caps;
  time_t start_time, stop_time, duration;
  int param = 0;

  if (opcaps_find(&cnts->capabilities, phr->login, &caps) < 0
      || opcaps_check(caps, OPCAP_CONTROL_CONTEST) < 0) {
    ns_error(log_f, NEW_SRV_ERR_PERMISSION_DENIED);
    goto cleanup;
  }

  run_get_times(cs->runlog_state, &start_time, 0, &duration, &stop_time, 0);

  switch (phr->action) {
  case NEW_SRV_ACTION_START_CONTEST:
    if (stop_time > 0) {
      ns_error(log_f, NEW_SRV_ERR_CONTEST_ALREADY_FINISHED);
      goto cleanup;
    }
    if (start_time > 0) {
      ns_error(log_f, NEW_SRV_ERR_CONTEST_ALREADY_STARTED);
      goto cleanup;
    }
    run_start_contest(cs->runlog_state, cs->current_time);
    serve_update_status_file(cs, 1);
    serve_invoke_start_script(cs);
    serve_update_standings_file(cs, cnts, 0);
    break;

  case NEW_SRV_ACTION_STOP_CONTEST:
    if (stop_time > 0) {
      ns_error(log_f, NEW_SRV_ERR_CONTEST_ALREADY_FINISHED);
      goto cleanup;
    }
    if (start_time <= 0) {
      ns_error(log_f, NEW_SRV_ERR_CONTEST_NOT_STARTED);
      goto cleanup;
    }
    run_stop_contest(cs->runlog_state, cs->current_time);
    serve_invoke_stop_script(cs);
    serve_update_status_file(cs, 1);
    break;

  case NEW_SRV_ACTION_CONTINUE_CONTEST:
    if (!global->enable_continue) {
      ns_error(log_f, NEW_SRV_ERR_CANNOT_CONTINUE_CONTEST);
      goto cleanup;
    }
    if (start_time <= 0) {
      ns_error(log_f, NEW_SRV_ERR_CONTEST_NOT_STARTED);
      goto cleanup;
    }
    if (stop_time <= 0) {
      ns_error(log_f, NEW_SRV_ERR_CONTEST_NOT_FINISHED);
      goto cleanup;
    }
    if (duration > 0 && cs->current_time >= start_time + duration) {
      ns_error(log_f, NEW_SRV_ERR_INSUFFICIENT_DURATION);
      goto cleanup;
    }
    run_set_finish_time(cs->runlog_state, 0);
    run_stop_contest(cs->runlog_state, 0);
    serve_invoke_stop_script(cs);
    serve_update_status_file(cs, 1);
    break;

  case NEW_SRV_ACTION_SCHEDULE:
    do_schedule(log_f, phr, cs, cnts);
    break;

  case NEW_SRV_ACTION_CHANGE_DURATION:
    do_change_duration(log_f, phr, cs, cnts);
    break;

  case NEW_SRV_ACTION_CHANGE_FINISH_TIME:
    do_change_finish_time(log_f, phr, cs, cnts);
    break;

  case NEW_SRV_ACTION_SUSPEND:
    cs->clients_suspended = 1;
    serve_update_status_file(cs, 1);
    break;

  case NEW_SRV_ACTION_RESUME:
    cs->clients_suspended = 0;
    serve_update_status_file(cs, 1);
    break;

  case NEW_SRV_ACTION_TEST_SUSPEND:
    cs->testing_suspended = 1;
    serve_update_status_file(cs, 1);
    break;

  case NEW_SRV_ACTION_TEST_RESUME:
    cs->testing_suspended = 0;
    serve_update_status_file(cs, 1);
    break;

  case NEW_SRV_ACTION_PRINT_SUSPEND:
    if (!global->enable_printing) break;
    cs->printing_suspended = 1;
    serve_update_status_file(cs, 1);
    break;

  case NEW_SRV_ACTION_PRINT_RESUME:
    if (!global->enable_printing) break;
    cs->printing_suspended = 0;
    serve_update_status_file(cs, 1);
    break;

  case NEW_SRV_ACTION_SET_JUDGING_MODE:
    if (global->score_system != SCORE_OLYMPIAD) break;
    cs->accepting_mode = 0;
    serve_update_status_file(cs, 1);
    break;

  case NEW_SRV_ACTION_SET_ACCEPTING_MODE:
    if (global->score_system != SCORE_OLYMPIAD) break;
    cs->accepting_mode = 1;
    serve_update_status_file(cs, 1);
    break;

  case NEW_SRV_ACTION_SET_TESTING_FINISHED_FLAG:
    if (global->score_system != SCORE_OLYMPIAD) break;
    if ((!global->is_virtual && cs->accepting_mode)
        ||(global->is_virtual && global->disable_virtual_auto_judge <= 0))
      break;
    cs->testing_finished = 1;
    serve_update_status_file(cs, 1);
    break;

  case NEW_SRV_ACTION_CLEAR_TESTING_FINISHED_FLAG:
    if (global->score_system != SCORE_OLYMPIAD) break;
    cs->testing_finished = 0;
    serve_update_status_file(cs, 1);
    break;

  case NEW_SRV_ACTION_RELOAD_SERVER:
    extra->last_access_time = 0;
    break;

  case NEW_SRV_ACTION_UPDATE_STANDINGS_2:
    serve_update_standings_file(cs, cnts, 1);
    break;

  case NEW_SRV_ACTION_RESET_2:
    serve_reset_contest(cnts, cs);
    extra->last_access_time = 0;
    break;

  case NEW_SRV_ACTION_SQUEEZE_RUNS:
    serve_squeeze_runs(cs);
    break;

  case NEW_SRV_ACTION_ADMIN_CHANGE_ONLINE_VIEW_SOURCE:
  case NEW_SRV_ACTION_ADMIN_CHANGE_ONLINE_VIEW_REPORT:
  case NEW_SRV_ACTION_ADMIN_CHANGE_ONLINE_VIEW_JUDGE_SCORE:
  case NEW_SRV_ACTION_ADMIN_CHANGE_ONLINE_FINAL_VISIBILITY:
    if (ns_cgi_param_int(phr, "param", &param) < 0) {
      ns_error(log_f, NEW_SRV_ERR_INV_PARAM);
      goto cleanup;
    }

    switch (phr->action) {
    case NEW_SRV_ACTION_ADMIN_CHANGE_ONLINE_VIEW_SOURCE:
      if (param < 0) param = -1;
      else if (param > 0) param = 1;
      cs->online_view_source = param;
      break;
    case NEW_SRV_ACTION_ADMIN_CHANGE_ONLINE_VIEW_REPORT:
      if (param < 0) param = -1;
      else if (param > 0) param = 1;
      cs->online_view_report = param;
      break;
    case NEW_SRV_ACTION_ADMIN_CHANGE_ONLINE_VIEW_JUDGE_SCORE:
      if (param) param = 1;
      cs->online_view_judge_score = param;
      break;
    case NEW_SRV_ACTION_ADMIN_CHANGE_ONLINE_FINAL_VISIBILITY:
      if (param) param = 1;
      cs->online_final_visibility = param;
      break;
    }

    serve_update_status_file(cs, 1);
    break;
  }

 cleanup:
  return 0;
}

static int
priv_password_operation(FILE *fout,
                        FILE *log_f,
                        struct http_request_info *phr,
                        const struct contest_desc *cnts,
                        struct contest_extra *extra)
{
  int retval = 0, r = 0;

  if (ns_open_ul_connection(phr->fw_state) < 0) {
    ns_html_err_ul_server_down(fout, phr, 0, 0);
    FAIL(1);
  }

  switch (phr->action) {
  case NEW_SRV_ACTION_GENERATE_PASSWORDS_2:
    if (opcaps_check(phr->caps, OPCAP_EDIT_USER) < 0
        && opcaps_check(phr->dbcaps, OPCAP_EDIT_USER) < 0)
      FAIL(NEW_SRV_ERR_PERMISSION_DENIED);
    if (cnts->disable_team_password) FAIL(NEW_SRV_ERR_TEAM_PWD_DISABLED);
    r = userlist_clnt_cnts_passwd_op(ul_conn,
                                     ULS_GENERATE_TEAM_PASSWORDS_2,
                                     cnts->id);
    break;
  case NEW_SRV_ACTION_GENERATE_REG_PASSWORDS_2:
    if (opcaps_check(phr->dbcaps, OPCAP_EDIT_USER) < 0)
      FAIL(NEW_SRV_ERR_PERMISSION_DENIED);
    r = userlist_clnt_cnts_passwd_op(ul_conn,
                                     ULS_GENERATE_PASSWORDS_2,
                                     cnts->id);
    break;
  case NEW_SRV_ACTION_CLEAR_PASSWORDS_2:
    if (opcaps_check(phr->caps, OPCAP_EDIT_USER) < 0
        && opcaps_check(phr->dbcaps, OPCAP_EDIT_USER) < 0)
      FAIL(NEW_SRV_ERR_PERMISSION_DENIED);
    if (cnts->disable_team_password) FAIL(NEW_SRV_ERR_TEAM_PWD_DISABLED);
    r = userlist_clnt_cnts_passwd_op(ul_conn,
                                     ULS_CLEAR_TEAM_PASSWORDS,
                                     cnts->id);
    break;
  }
  if (r < 0) {
    ns_error(log_f, NEW_SRV_ERR_PWD_UPDATE_FAILED, userlist_strerror(-r));
    goto cleanup;
  }

 cleanup:
  return retval;
}

static int
priv_change_language(FILE *fout,
                     FILE *log_f,
                     struct http_request_info *phr,
                     const struct contest_desc *cnts,
                     struct contest_extra *extra)
{
  const unsigned char *s;
  int r, n;
  int new_locale_id;

  if ((r = ns_cgi_param(phr, "locale_id", &s)) < 0) goto invalid_param;
  if (r > 0) {
    if (sscanf(s, "%d%n", &new_locale_id, &n) != 1 || s[n] || new_locale_id < 0)
      goto invalid_param;
  }

  if (ns_open_ul_connection(phr->fw_state) < 0) {
    ns_html_err_ul_server_down(fout, phr, 0, 0);
    return -1;
  }
  if ((r = userlist_clnt_set_cookie(ul_conn, ULS_SET_COOKIE_LOCALE,
                                    phr->session_id,
                                    phr->client_key,
                                    new_locale_id)) < 0) {
    ns_error(log_f, NEW_SRV_ERR_SESSION_UPDATE_FAILED, userlist_strerror(-r));
  }
  return 0;

 invalid_param:
  ns_error(log_f, NEW_SRV_ERR_INV_LOCALE_ID);
  return 0;
}

static void
priv_change_password(FILE *fout,
                     struct http_request_info *phr,
                     const struct contest_desc *cnts,
                     struct contest_extra *extra)
{
  const unsigned char *p0 = 0, *p1 = 0, *p2 = 0;
  char *log_txt = 0;
  size_t log_len = 0;
  FILE *log_f = 0;
  int cmd, r;
  unsigned char url[1024];
  unsigned char login_buf[256];

  if (ns_cgi_param(phr, "oldpasswd", &p0) <= 0)
    return ns_html_err_inv_param(fout, phr, 1, "cannot parse oldpasswd");
  if (ns_cgi_param(phr, "newpasswd1", &p1) <= 0)
    return ns_html_err_inv_param(fout, phr, 1, "cannot parse newpasswd1");
  if (ns_cgi_param(phr, "newpasswd2", &p2) <= 0)
    return ns_html_err_inv_param(fout, phr, 1, "cannot parse newpasswd2");

  log_f = open_memstream(&log_txt, &log_len);

  if (strlen(p0) >= 256) {
    ns_error(log_f, NEW_SRV_ERR_OLD_PWD_TOO_LONG);
    goto done;
  }
  if (strcmp(p1, p2)) {
    ns_error(log_f, NEW_SRV_ERR_NEW_PWD_MISMATCH);
    goto done;
  }
  if (strlen(p1) >= 256) {
    ns_error(log_f, NEW_SRV_ERR_NEW_PWD_TOO_LONG);
    goto done;
  }

  cmd = ULS_PRIV_SET_REG_PASSWD;

  if (ns_open_ul_connection(phr->fw_state) < 0) {
    ns_html_err_ul_server_down(fout, phr, 1, 0);
    goto cleanup;
  }
  r = userlist_clnt_set_passwd(ul_conn, cmd, phr->user_id, phr->contest_id,
                               p0, p1);
  if (r < 0) {
    ns_error(log_f, NEW_SRV_ERR_PWD_UPDATE_FAILED, userlist_strerror(-r));
    goto done;
  }

 done:;
  close_memstream(log_f); log_f = 0;
  if (!log_txt || !*log_txt) {
    url_armor_string(login_buf, sizeof(login_buf), phr->login);
    snprintf(url, sizeof(url),
             "%s?contest_id=%d&role=%d&login=%s&locale_id=%d&action=%d",
             phr->self_url, phr->contest_id, phr->role,
             login_buf, phr->locale_id,
             NEW_SRV_ACTION_LOGIN_PAGE);
    ns_refresh_page_2(fout, phr->client_key, url);
  } else {
    html_error_status_page(fout, phr, cnts, extra, log_txt,
                           NEW_SRV_ACTION_MAIN_PAGE, 0);
  }

 cleanup:;
  if (log_f) fclose(log_f);
  xfree(log_txt);
}

static int
priv_reset_filter(FILE *fout,
                  FILE *log_f,
                  struct http_request_info *phr,
                  const struct contest_desc *cnts,
                  struct contest_extra *extra)
{
  serve_state_t cs = extra->serve_state;

  switch (phr->action) {
  case NEW_SRV_ACTION_RESET_FILTER:
    html_reset_filter(cs, phr->user_id, phr->session_id);
    break;

  case NEW_SRV_ACTION_RESET_CLAR_FILTER:
    html_reset_clar_filter(cs, phr->user_id, phr->session_id);
    break;
  }
  return 0;
}

static int
priv_submit_run(FILE *fout,
                FILE *log_f,
                struct http_request_info *phr,
                const struct contest_desc *cnts,
                struct contest_extra *extra)
{
  const serve_state_t cs = extra->serve_state;
  const struct section_global_data *global = cs->global;
  const struct section_problem_data *prob = 0;
  const struct section_language_data *lang = 0;
  const unsigned char *s;
  int prob_id = 0, variant = 0, lang_id = 0, n, max_ans, ans, i, mime_type = 0, r;
  const unsigned char *errmsg = 0;
  const unsigned char *run_text;
  size_t run_size, ans_size;
  unsigned char *ans_map = 0, *ans_buf = 0, *ans_tmp = 0;
  char **lang_list = 0;
  const unsigned char *mime_type_str = 0;
  int run_id, arch_flags, retval = 0;
  ruint32_t shaval[5];
  struct timeval precise_time;
  path_t run_path;
  struct problem_plugin_iface *plg = 0;
  int skip_mime_type_test = 0;
  const unsigned char *text_form_text = 0;
  size_t text_form_size = 0;
  unsigned char *utf8_str = 0;
  int utf8_len = 0;
  int eoln_type = 0;

  if (opcaps_check(phr->caps, OPCAP_SUBMIT_RUN) < 0) {
    ns_error(log_f, NEW_SRV_ERR_PERMISSION_DENIED);
    goto cleanup;
  }

  if (ns_cgi_param_int(phr, "problem", &prob_id) < 0) {
    errmsg = "problem is not set or binary";
    goto invalid_param;
  }
  if (prob_id <= 0 || prob_id > cs->max_prob
      || !(prob = cs->probs[prob_id])) {
    errmsg = "invalid prob_id";
    goto invalid_param;
  }
  if (ns_cgi_param_int_opt(phr, "variant", &variant, 0) < 0) {
    errmsg = "variant is invalid";
    goto invalid_param;
  }
  if (prob->variant_num <= 0 && variant != 0) {
    errmsg = "variant is invalid";
    goto invalid_param;
  } else if (prob->variant_num > 0
             && (variant <= 0 || variant > prob->variant_num)) {
    errmsg = "variant is invalid";
    goto invalid_param;
  }

  /*
  if (ns_cgi_param(phr, "problem", &s) <= 0) {
    errmsg = "problem is not set or binary";
    goto invalid_param;
  }
  if (sscanf(s, "%d_%d%n", &prob_id, &variant, &n) == 2 && !s[n]) {
    if (prob_id <= 0 || prob_id > cs->max_prob
        || !(prob = cs->probs[prob_id])) {
      errmsg = "invalid prob_id";
      goto invalid_param;
    }
    if (prob->variant_num <= 0 || variant <= 0 || variant > prob->variant_num) {
      errmsg = "invalid variant";
      goto invalid_param;
    }
  } else if (sscanf(s, "%d%n", &prob_id, &n) == 1 && !s[n]) {
    if (prob_id <= 0 || prob_id > cs->max_prob
        || !(prob = cs->probs[prob_id])) {
      errmsg = "invalid prob_id";
      goto invalid_param;
    }
    if (prob->variant_num > 0) {
      errmsg = "invalid variant";
      goto invalid_param;
    }
  } else {
    errmsg = "cannot parse problem";
    goto invalid_param;
  }
  */

  if (prob->type == PROB_TYPE_STANDARD) {
    if (ns_cgi_param(phr, "lang_id", &s) <= 0) {
      errmsg = "lang_id is not set or binary";
      goto invalid_param;
    }
    if (sscanf(s, "%d%n", &lang_id, &n) != 1 || s[n]) {
      errmsg = "cannot parse lang_id";
      goto invalid_param;
    }
    if (lang_id <= 0 || lang_id > cs->max_lang || !(lang = cs->langs[lang_id])){
      errmsg = "lang_id is invalid";
      goto invalid_param;
    }
    if (cs->global->enable_eoln_select > 0) {
      ns_cgi_param_int_opt(phr, "eoln_type", &eoln_type, 0);
      if (eoln_type < 0 || eoln_type > EOLN_CRLF) eoln_type = 0;
    }
  }

  /* get the submission text */
  switch (prob->type) {
    /*
  case PROB_TYPE_STANDARD:      // "file"
    if (!ns_cgi_param_bin(phr, "file", &run_text, &run_size)) {
      errmsg = "\"file\" parameter is not set";
      goto invalid_param;
    }
    break;
    */
  case PROB_TYPE_STANDARD:
  case PROB_TYPE_OUTPUT_ONLY:
  case PROB_TYPE_TESTS:
    if (prob->enable_text_form > 0) {
      int r1 = ns_cgi_param_bin(phr, "file", &run_text, &run_size);
      int r2 = ns_cgi_param_bin(phr, "text_form", &text_form_text,
                                &text_form_size);
      if (!r1 && !r2) {
        errmsg = "neither \"file\" nor \"text\" parameters are set";
        goto invalid_param;
      }
    } else {
      if (!ns_cgi_param_bin(phr, "file", &run_text, &run_size)) {
        errmsg = "\"file\" parameter is not set";
        goto invalid_param;
      }
    }
    break;
  case PROB_TYPE_TEXT_ANSWER:
  case PROB_TYPE_SHORT_ANSWER:
  case PROB_TYPE_SELECT_ONE:
    if (!ns_cgi_param_bin(phr, "file", &run_text, &run_size)) {
      errmsg = "\"file\" parameter is not set";
      goto invalid_param;
    }
    break;
  case PROB_TYPE_SELECT_MANY:   // "ans_*"
    for (i = 0, max_ans = -1, ans_size = 0; i < phr->param_num; i++)
      if (!strncmp(phr->param_names[i], "ans_", 4)) {
        if (sscanf(phr->param_names[i] + 4, "%d%n", &ans, &n) != 1
            || phr->param_names[i][4 + n]) {
          errmsg = "\"ans_*\" parameter is invalid";
          goto invalid_param;
        }
        if (ans < 0 || ans > 65535) {
          errmsg = "\"ans_*\" parameter is out of range";
          goto invalid_param;
        }
        if (ans > max_ans) max_ans = ans;
        ans_size += 7;
      }
    if (max_ans < 0) {
      run_text = "";
      run_size = 0;
      break;
    }
    XALLOCAZ(ans_map, max_ans + 1);
    for (i = 0; i < phr->param_num; i++)
      if (!strncmp(phr->param_names[i], "ans_", 4)) {
        sscanf(phr->param_names[i] + 4, "%d", &ans);
        ans_map[ans] = 1;
      }
    XALLOCA(ans_buf, ans_size);
    run_text = ans_buf;
    for (i = 0, run_size = 0; i <= max_ans; i++)
      if (ans_map[i]) {
        if (run_size > 0) ans_buf[run_size++] = ' ';
        run_size += sprintf(ans_buf + run_size, "%d", i);
      }
    ans_buf[run_size++] = '\n';
    ans_buf[run_size] = 0;
    break;
  case PROB_TYPE_CUSTOM:   // use problem plugin
    load_problem_plugin(cs, prob_id);
    if (!(plg = cs->prob_extras[prob_id].plugin) || !plg->parse_form) {
      errmsg = "problem plugin is not available";
      goto invalid_param;
    }
    ans_tmp = (*plg->parse_form)(cs->prob_extras[prob_id].plugin_data,
                                 log_f, phr, cnts, extra);
    if (!ans_tmp) goto cleanup;
    run_size = strlen(ans_tmp);
    ans_buf = (unsigned char*) alloca(run_size + 1);
    strcpy(ans_buf, ans_tmp);
    run_text = ans_buf;
    xfree(ans_tmp);
    break;
  default:
    abort();
  }

  switch (prob->type) {
  case PROB_TYPE_STANDARD:
    if (!lang->binary && strlen(run_text) != run_size) {
      // guess utf-16/ucs-2
      if (((int) run_size) < 0) goto binary_submission;
      if ((utf8_len = ucs2_to_utf8(&utf8_str, run_text, run_size)) < 0)
        goto binary_submission;
      run_text = utf8_str;
      run_size = (size_t) utf8_len;
    }
    if (prob->enable_text_form > 0 && text_form_text
        && strlen(text_form_text) != text_form_size)
      goto binary_submission;
    if (prob->enable_text_form) {
      if (!run_size) {
        run_text = text_form_text; text_form_text = 0;
        run_size = text_form_size; text_form_size = 0;
        skip_mime_type_test = 1;
      } else {
        text_form_text = 0;
        text_form_size = 0;
      }
    }
    if (prob->disable_ctrl_chars > 0 && has_control_characters(run_text))
      goto invalid_characters;
    break;
  case PROB_TYPE_OUTPUT_ONLY:
  case PROB_TYPE_TESTS:
    if (!prob->binary_input && !prob->binary && strlen(run_text) != run_size)
      goto binary_submission;
    if (prob->enable_text_form > 0 && text_form_text
        && strlen(text_form_text) != text_form_size)
      goto binary_submission;
    if (prob->enable_text_form) {
      if (!run_size) {
        run_text = text_form_text; text_form_text = 0;
        run_size = text_form_size; text_form_size = 0;
        skip_mime_type_test = 1;
      } else {
        text_form_text = 0;
        text_form_size = 0;
      }
    }
    break;

  case PROB_TYPE_TEXT_ANSWER:
  case PROB_TYPE_SHORT_ANSWER:
  case PROB_TYPE_SELECT_ONE:
    if (strlen(run_text) != run_size) goto binary_submission;
    break;

  case PROB_TYPE_SELECT_MANY:
  case PROB_TYPE_CUSTOM:
    break;

  binary_submission:
    errmsg = "binary submission";
    goto invalid_param;

  invalid_characters:
    errmsg = "invalid characters";
    goto invalid_param;
  }

  // ignore BOM
  if (global->ignore_bom > 0 && !prob->binary && (!lang || !lang->binary)) {
    if (run_text && run_size >= 3 && run_text[0] == 0xef
        && run_text[1] == 0xbb && run_text[2] == 0xbf) {
      run_text += 3; run_size -= 3;
    }
  }

  /* check for disabled languages */
  if (lang_id > 0) {
    if (lang->disabled) {
      ns_error(log_f, NEW_SRV_ERR_LANG_DISABLED);
      goto cleanup;
    }

    if (prob->enable_language) {
      lang_list = prob->enable_language;
      for (i = 0; lang_list[i]; i++)
        if (!strcmp(lang_list[i], lang->short_name))
          break;
      if (!lang_list[i]) {
        ns_error(log_f, NEW_SRV_ERR_LANG_NOT_AVAIL_FOR_PROBLEM);
        goto cleanup;
      }
    } else if (prob->disable_language) {
      lang_list = prob->disable_language;
      for (i = 0; lang_list[i]; i++)
        if (!strcmp(lang_list[i], lang->short_name))
          break;
      if (lang_list[i]) {
        ns_error(log_f, NEW_SRV_ERR_LANG_DISABLED_FOR_PROBLEM);
        goto cleanup;
      }
    }
  } else if (skip_mime_type_test) {
    mime_type = 0;
    mime_type_str = mime_type_get_type(mime_type);
  } else {
    // guess the content-type and check it against the list
    if ((mime_type = mime_type_guess(global->diff_work_dir,
                                     run_text, run_size)) < 0) {
      ns_error(log_f, NEW_SRV_ERR_CANNOT_DETECT_CONTENT_TYPE);
      goto cleanup;
    }
    mime_type_str = mime_type_get_type(mime_type);
    if (prob->enable_language) {
      lang_list = prob->enable_language;
      for (i = 0; lang_list[i]; i++)
        if (!strcmp(lang_list[i], mime_type_str))
          break;
      if (!lang_list[i]) {
        ns_error(log_f, NEW_SRV_ERR_CONTENT_TYPE_NOT_AVAILABLE, mime_type_str);
        goto cleanup;
      }
    } else if (prob->disable_language) {
      lang_list = prob->disable_language;
      for (i = 0; lang_list[i]; i++)
        if (!strcmp(lang_list[i], mime_type_str))
          break;
      if (lang_list[i]) {
        ns_error(log_f, NEW_SRV_ERR_CONTENT_TYPE_DISABLED, mime_type_str);
        goto cleanup;
      }
    }
  }

  // OK, so all checks are done, now we add this submit to the database
  sha_buffer(run_text, run_size, shaval);
  gettimeofday(&precise_time, 0);

  ruint32_t run_uuid[4];
  int store_flags = 0;
  ej_uuid_generate(run_uuid);
  if (global->uuid_run_store > 0 && run_get_uuid_hash_state(cs->runlog_state) >= 0 && ej_uuid_is_nonempty(run_uuid)) {
    store_flags = 1;
  }
  run_id = run_add_record(cs->runlog_state, 
                          precise_time.tv_sec, precise_time.tv_usec * 1000,
                          run_size, shaval, run_uuid,
                          &phr->ip, phr->ssl_flag,
                          phr->locale_id, phr->user_id,
                          prob_id, lang_id, eoln_type,
                          variant, 1, mime_type, store_flags);
  if (run_id < 0) {
    ns_error(log_f, NEW_SRV_ERR_RUNLOG_UPDATE_FAILED);
    goto cleanup;
  }
  serve_move_files_to_insert_run(cs, run_id);

  if (store_flags == 1) {
    arch_flags = uuid_archive_prepare_write_path(cs, run_path, sizeof(run_path),
                                                 run_uuid, run_size, DFLT_R_UUID_SOURCE, 0, 0);
  } else {
    arch_flags = archive_prepare_write_path(cs, run_path, sizeof(run_path),
                                            global->run_archive_dir, run_id,
                                            run_size, NULL, 0, 0);
  }
  if (arch_flags < 0) {
    run_undo_add_record(cs->runlog_state, run_id);
    ns_error(log_f, NEW_SRV_ERR_DISK_WRITE_ERROR);
    goto cleanup;
  }

  if (generic_write_file(run_text, run_size, arch_flags, 0, run_path, "") < 0) {
    run_undo_add_record(cs->runlog_state, run_id);
    ns_error(log_f, NEW_SRV_ERR_DISK_WRITE_ERROR);
    goto cleanup;
  }

  if (prob->type == PROB_TYPE_STANDARD) {
    // automatically tested programs
    if (prob->disable_auto_testing > 0
        || (prob->disable_testing > 0 && prob->enable_compilation <= 0)
        || lang->disable_auto_testing || lang->disable_testing) {
      serve_audit_log(cs, run_id, NULL, phr->user_id, &phr->ip, phr->ssl_flag,
                      "priv-submit", "ok", RUN_PENDING,
                      "  Testing disabled for this problem or language");
      run_change_status_4(cs->runlog_state, run_id, RUN_PENDING);
    } else {
      serve_audit_log(cs, run_id, NULL, phr->user_id, &phr->ip, phr->ssl_flag,
                      "priv-submit", "ok", RUN_COMPILING, NULL);
      if ((r = serve_compile_request(cs, run_text, run_size, global->contest_id,
                                     run_id, phr->user_id,
                                     lang->compile_id, variant,
                                     phr->locale_id, 0,
                                     lang->src_sfx,
                                     lang->compiler_env,
                                     0, prob->style_checker_cmd,
                                     prob->style_checker_env,
                                     -1, 0, 0, prob, lang, 0, run_uuid, store_flags)) < 0) {
        serve_report_check_failed(ejudge_config, cnts, cs, run_id, serve_err_str(r));
      }
    }
  } else if (prob->manual_checking > 0) {
    // manually tested outputs
    if (prob->check_presentation <= 0) {
      serve_audit_log(cs, run_id, NULL, phr->user_id, &phr->ip, phr->ssl_flag,
                      "priv-submit", "ok", RUN_ACCEPTED, 
                      "  This problem is checked manually");
      run_change_status_4(cs->runlog_state, run_id, RUN_ACCEPTED);
    } else {
      serve_audit_log(cs, run_id, NULL, phr->user_id, &phr->ip, phr->ssl_flag,
                      "priv-submit", "ok", RUN_COMPILING, NULL);
      if (prob->style_checker_cmd && prob->style_checker_cmd[0]) {
        r = serve_compile_request(cs, run_text, run_size, global->contest_id, 
                                  run_id, phr->user_id, 0 /* lang_id */, variant,
                                  0 /* locale_id */, 1 /* output_only*/,
                                  mime_type_get_suffix(mime_type),
                                  NULL /* compiler_env */,
                                  1 /* style_check_only */,
                                  prob->style_checker_cmd,
                                  prob->style_checker_env,
                                  0 /* accepting_mode */,
                                  0 /* priority_adjustment */,
                                  0 /* notify flag */,
                                  prob, NULL /* lang */,
                                  0 /* no_db_flag */,
                                  run_uuid,
                                  store_flags);
        if (r < 0) {
          serve_report_check_failed(ejudge_config, cnts, cs, run_id, serve_err_str(r));
        }
      } else {
        if (serve_run_request(cs, cnts, log_f, run_text, run_size,
                              global->contest_id, run_id,
                              phr->user_id, prob_id, 0, variant, 0, -1, -1, 0,
                              mime_type, 0, phr->locale_id, 0, 0, 0, run_uuid) < 0) {
          ns_error(log_f, NEW_SRV_ERR_DISK_WRITE_ERROR);
          goto cleanup;
        }
      }
    }
  } else {
    // automatically tested outputs
    if (prob->disable_auto_testing > 0
        || (prob->disable_testing > 0 && prob->enable_compilation <= 0)) {
      serve_audit_log(cs, run_id, NULL, phr->user_id, &phr->ip, phr->ssl_flag,
                      "priv-submit", "ok", RUN_PENDING,
                      "  Testing disabled for this problem");
      run_change_status_4(cs->runlog_state, run_id, RUN_PENDING);
    } else {
      serve_audit_log(cs, run_id, NULL, phr->user_id, &phr->ip, phr->ssl_flag,
                      "priv-submit", "ok", RUN_COMPILING, NULL);
      /* FIXME: check for XML problem */
      if (prob->style_checker_cmd && prob->style_checker_cmd[0]) {
        r = serve_compile_request(cs, run_text, run_size, global->contest_id,
                                  run_id, phr->user_id, 0 /* lang_id */, variant,
                                  0 /* locale_id */, 1 /* output_only*/,
                                  mime_type_get_suffix(mime_type),
                                  NULL /* compiler_env */,
                                  1 /* style_check_only */,
                                  prob->style_checker_cmd,
                                  prob->style_checker_env,
                                  0 /* accepting_mode */,
                                  0 /* priority_adjustment */,
                                  0 /* notify flag */,
                                  prob, NULL /* lang */,
                                  0 /* no_db_flag */,
                                  run_uuid,
                                  store_flags);
        if (r < 0) {
          serve_report_check_failed(ejudge_config, cnts, cs, run_id, serve_err_str(r));
        }
      } else {      
        if (serve_run_request(cs, cnts, log_f, run_text, run_size,
                              global->contest_id, run_id,
                              phr->user_id, prob_id, 0, variant, 0, -1, -1, 0,
                              mime_type, 0, phr->locale_id, 0, 0, 0, run_uuid) < 0) {
          ns_error(log_f, NEW_SRV_ERR_DISK_WRITE_ERROR);
          goto cleanup;
        }
      }
    }
  }

 cleanup:
  xfree(utf8_str);
  return retval;

 invalid_param:
  ns_html_err_inv_param(fout, phr, 0, errmsg);
  xfree(utf8_str);
  return -1;
}

static int
priv_submit_clar(
        FILE *fout,
        FILE *log_f,
        struct http_request_info *phr,
        const struct contest_desc *cnts,
        struct contest_extra *extra)
{
  const serve_state_t cs = extra->serve_state;
  const struct section_global_data *global = cs->global;
  int n, user_id = -1, hide_flag = 0, clar_id;
  const unsigned char *s;
  struct html_armor_buffer ab;
  const unsigned char *errmsg;
  const unsigned char *subject = 0, *text = 0;
  size_t subj_len, text_len, text3_len;
  unsigned char *subj2, *text2, *text3;
  struct timeval precise_time;
  int msg_dest_id_empty = 0, msg_dest_login_empty = 0;

  html_armor_init(&ab);

  if (opcaps_check(phr->caps, OPCAP_NEW_MESSAGE) < 0) {
    ns_error(log_f, NEW_SRV_ERR_PERMISSION_DENIED);
    goto cleanup;
  }

  // msg_dest_id, msg_dest_login, msg_subj, msg_hide_flag, msg_text
  if ((n = ns_cgi_param(phr, "msg_dest_id", &s)) < 0) {
    errmsg = "msg_dest_id is binary";
    goto invalid_param;
  }
  if (n <= 0 || is_empty_string(s)) {
    msg_dest_id_empty = 1;
  } else {
    if (sscanf(s, "%d%n", &user_id, &n) != 1 || s[n]) {
      errmsg = "msg_dest_id is invalid";
      goto invalid_param;
    }
    if (user_id && !teamdb_lookup(cs->teamdb_state, user_id)) {
      ns_error(log_f, NEW_SRV_ERR_USER_ID_NONEXISTANT, user_id);
      goto cleanup;
    }
  }
  if ((n = ns_cgi_param(phr, "msg_dest_login", &s)) < 0) {
    errmsg = "msg_dest_login is binary";
    goto invalid_param;
  }
  if (n <= 0 || is_empty_string(s)) {
    msg_dest_login_empty = 1;
  } else {
    if (!strcasecmp(s, "all")) {
      if (user_id > 0) {
        ns_error(log_f, NEW_SRV_ERR_CONFLICTING_USER_ID_LOGIN,
                 user_id, ARMOR(s));
        goto cleanup;
      }
      user_id = 0;
    } else {
      if ((n = teamdb_lookup_login(cs->teamdb_state, s)) <= 0) {
        ns_error(log_f, NEW_SRV_ERR_USER_LOGIN_NONEXISTANT, ARMOR(s));
        goto cleanup;
      }
      if (user_id >= 0 && user_id != n) {
        ns_error(log_f, NEW_SRV_ERR_CONFLICTING_USER_ID_LOGIN,
                 user_id, ARMOR(s));
        goto cleanup;
      }
      user_id = n;
    }
  }
  if (msg_dest_id_empty && msg_dest_login_empty) {
    errmsg = "neither user_id nor login are not specified";
    goto invalid_param;
  }
  if ((n = ns_cgi_param(phr, "msg_subj", &subject)) < 0) {
    errmsg = "msg_subj is binary";
    goto invalid_param;
  }
  if (!subject) subject = "";
  if ((n = ns_cgi_param(phr, "msg_text", &text)) < 0) {
    errmsg = "msg_text is binary";
    goto invalid_param;
  }
  if (!text) text = "";
  if ((n = ns_cgi_param(phr, "msg_hide_flag", &s)) < 0) {
    errmsg = "msg_hide_flag is binary";
    goto invalid_param;
  }
  if (n > 0) {
    if (sscanf(s, "%d%n", &hide_flag, &n) != 1 || s[n]
        || hide_flag < 0 || hide_flag > 1) {
      errmsg = "msg_hide_flag is invalid";
      goto invalid_param;
    }
  }

  subj_len = strlen(subject);
  if (subj_len > 1024) {
    ns_error(log_f, NEW_SRV_ERR_SUBJECT_TOO_LONG, subj_len);
    goto cleanup;
  }
  subj2 = alloca(subj_len + 1);
  memcpy(subj2, subject, subj_len + 1);
  while (subj_len > 0 && isspace(subj2[subj_len - 1])) subj2[--subj_len] = 0;
  if (!subj_len) {
    ns_error(log_f, NEW_SRV_ERR_SUBJECT_EMPTY);
    goto cleanup;
  }

  text_len = strlen(text);
  if (text_len > 128 * 1024 * 1024) {
    ns_error(log_f, NEW_SRV_ERR_MESSAGE_TOO_LONG, subj_len);
    goto cleanup;
  }
  text2 = alloca(text_len + 1);
  memcpy(text2, text, text_len + 1);
  while (text_len > 0 && isspace(text2[text_len - 1])) text2[--text_len] = 0;
  if (!text_len) {
    ns_error(log_f, NEW_SRV_ERR_MESSAGE_EMPTY);
    goto cleanup;
  }

  text3 = alloca(subj_len + text_len + 32);
  text3_len = sprintf(text3, "Subject: %s\n\n%s\n", subj2, text2);

  gettimeofday(&precise_time, 0);
  if ((clar_id = clar_add_record(cs->clarlog_state,
                                 precise_time.tv_sec,
                                 precise_time.tv_usec * 1000,
                                 text3_len,
                                 &phr->ip,
                                 phr->ssl_flag,
                                 0, user_id, 0, phr->user_id,
                                 hide_flag, phr->locale_id, 0, 0, 0,
                                 utf8_mode, NULL, subj2)) < 0) {
    ns_error(log_f, NEW_SRV_ERR_CLARLOG_UPDATE_FAILED);
    goto cleanup;
  }

  if (clar_add_text(cs->clarlog_state, clar_id, text3, text3_len) < 0) {
    ns_error(log_f, NEW_SRV_ERR_DISK_WRITE_ERROR);
    goto cleanup;
  }

  if (global->notify_clar_reply && user_id > 0) {
    unsigned char nsubj[1024];
    FILE *msg_f = 0;
    char *msg_t = 0;
    size_t msg_z = 0;

    if (cnts->default_locale_num > 0)
      l10n_setlocale(cnts->default_locale_num);
    snprintf(nsubj, sizeof(nsubj),
             _("You have received a message from judges in contest %d"),
             cnts->id);
    msg_f = open_memstream(&msg_t, &msg_z);
    fprintf(msg_f, _("You have received a message from judges\n"));
    fprintf(msg_f, _("Contest: %d (%s)\n"), cnts->id, cnts->name);
    if (cnts->team_url) {
      fprintf(msg_f, "URL: %s?contest_id=%d&login=%s\n", cnts->team_url,
              cnts->id, teamdb_get_login(cs->teamdb_state, user_id));
    }
    fprintf(msg_f, "%s\n", text3);
    fprintf(msg_f, "\n-\nRegards,\nthe ejudge contest management system (www.ejudge.ru)\n");
    close_memstream(msg_f); msg_f = 0;
    if (cnts->default_locale_num > 0) {
      l10n_setlocale(cnts->default_locale_num);
    }
    serve_send_email_to_user(ejudge_config, cnts, cs, user_id, nsubj, msg_t);
    xfree(msg_t); msg_t = 0; msg_z = 0;
  }

  /*
  serve_send_clar_notify_email(cs, cnts, phr->user_id, phr->name, subj3, text2);
  */

 cleanup:
  html_armor_free(&ab);
  return 0;

 invalid_param:
  ns_html_err_inv_param(fout, phr, 0, errmsg);
  return -1;
}

static int
parse_run_id(FILE *fout, struct http_request_info *phr,
             const struct contest_desc *cnts,
             struct contest_extra *extra, int *p_run_id, struct run_entry *pe);

static int
priv_set_run_style_error_status(
        FILE *fout,
        FILE *log_f,
        struct http_request_info *phr,
        const struct contest_desc *cnts,
        struct contest_extra *extra)
{
  const serve_state_t cs = extra->serve_state;
  const struct section_global_data *global = cs->global;
  int run_id = 0, rep_flags;
  struct run_entry re;
  const unsigned char *text = 0;
  unsigned char *text2 = 0;
  size_t text_len, text2_len;
  unsigned char errmsg[1024];
  unsigned char rep_path[PATH_MAX];

  if (opcaps_check(phr->caps, OPCAP_COMMENT_RUN) < 0) {
    ns_error(log_f, NEW_SRV_ERR_PERMISSION_DENIED);
    goto cleanup;
  }

  errmsg[0] = 0;
  if (parse_run_id(fout, phr, cnts, extra, &run_id, &re) < 0) return -1;
  if (re.user_id && !teamdb_lookup(cs->teamdb_state, re.user_id)) {
    ns_error(log_f, NEW_SRV_ERR_USER_ID_NONEXISTANT, re.user_id);
    goto cleanup;
  }
  if ((re.status != RUN_ACCEPTED && re.status != RUN_PENDING_REVIEW) && opcaps_check(phr->caps, OPCAP_EDIT_RUN)<0){
    ns_error(log_f, NEW_SRV_ERR_PERMISSION_DENIED);
    goto cleanup;    
  }
  if (ns_cgi_param(phr, "msg_text", &text) < 0) {
    snprintf(errmsg, sizeof(errmsg), "%s", "msg_text is binary");
    goto invalid_param;
  }
  if (!text) text = "";
  text_len = strlen(text);
  if (text_len > 128 * 1024) {
    ns_error(log_f, NEW_SRV_ERR_MESSAGE_TOO_LONG, text_len);
    goto cleanup;
  }
  text2 = text_area_process_string(text, 0, 0);
  text2_len = strlen(text2);

  if (re.store_flags == 1) {
    rep_flags = uuid_archive_prepare_write_path(cs, rep_path, sizeof(rep_path),
                                                re.run_uuid, text2_len, DFLT_R_UUID_XML_REPORT, 0, 0);
  } else {
    rep_flags = archive_prepare_write_path(cs, rep_path, sizeof(rep_path),
                                           global->xml_report_archive_dir, run_id,
                                           text2_len, NULL, 0, 0);
  }
  if (rep_flags < 0) {
    snprintf(errmsg, sizeof(errmsg),
             "archive_make_write_path: %s, %d, %zu failed\n",
             global->xml_report_archive_dir, run_id,
             text2_len);
    goto invalid_param;
  }

  if (generic_write_file(text2, text2_len, rep_flags, 0, rep_path, "") < 0) {
    snprintf(errmsg, sizeof(errmsg), "generic_write_file: %s, %d, %zu failed\n",
             global->xml_report_archive_dir, run_id, text2_len);
    goto invalid_param;
  }
  if (run_change_status_4(cs->runlog_state, run_id, RUN_REJECTED) < 0)
    goto invalid_param;

  serve_audit_log(cs, run_id, &re, phr->user_id, &phr->ip, phr->ssl_flag,
                  "set-rejected", "ok", RUN_REJECTED, NULL);

  if (global->notify_status_change > 0 && !re.is_hidden) {
    serve_notify_user_run_status_change(ejudge_config, cnts, cs, re.user_id,
                                        run_id, RUN_REJECTED);
  }

 cleanup:
  xfree(text2);
  return 0;

 invalid_param:
  xfree(text2);
  ns_html_err_inv_param(fout, phr, 0, errmsg);
  return -1;
}

static int
priv_submit_run_comment(
        FILE *fout,
        FILE *log_f,
        struct http_request_info *phr,
        const struct contest_desc *cnts,
        struct contest_extra *extra)
{
  const serve_state_t cs = extra->serve_state;
  const struct section_global_data *global = cs->global;
  int run_id = 0, clar_id = 0;
  struct run_entry re;
  struct html_armor_buffer ab = HTML_ARMOR_INITIALIZER;
  const unsigned char *text = 0;
  const unsigned char *errmsg = 0;
  size_t text_len, subj_len, text3_len;
  unsigned char *text2 = 0, *text3 = 0;
  unsigned char subj2[1024];
  struct timeval precise_time;

  if (parse_run_id(fout, phr, cnts, extra, &run_id, &re) < 0) return -1;
  if (re.user_id && !teamdb_lookup(cs->teamdb_state, re.user_id)) {
    ns_error(log_f, NEW_SRV_ERR_USER_ID_NONEXISTANT, re.user_id);
    goto cleanup;
  }
  if (ns_cgi_param(phr, "msg_text", &text) < 0) {
    errmsg = "msg_text is binary";
    goto invalid_param;
  }
  if (!text) text = "";
  text_len = strlen(text);
  if (text_len > 128 * 1024) {
    ns_error(log_f, NEW_SRV_ERR_MESSAGE_TOO_LONG, text_len);
    goto cleanup;
  }
  text2 = alloca(text_len + 1);
  memcpy(text2, text, text_len + 1);
  while (text_len > 0 && isspace(text2[text_len - 1])) text2[--text_len] = 0;
  if (!text_len) {
    ns_error(log_f, NEW_SRV_ERR_MESSAGE_EMPTY);
    goto cleanup;
  }

  snprintf(subj2, sizeof(subj2), "%d %s", run_id, _("is commented"));
  subj_len = strlen(subj2);

  text3 = alloca(subj_len + text_len + 32);
  text3_len = sprintf(text3, "Subject: %s\n\n%s\n", subj2, text2);

  gettimeofday(&precise_time, 0);
  if ((clar_id = clar_add_record(cs->clarlog_state,
                                 precise_time.tv_sec,
                                 precise_time.tv_usec * 1000,
                                 text3_len,
                                 &phr->ip,
                                 phr->ssl_flag,
                                 0, re.user_id, 0, phr->user_id,
                                 0, phr->locale_id, 0, run_id + 1, 0,
                                 utf8_mode, NULL, subj2)) < 0) {
    ns_error(log_f, NEW_SRV_ERR_CLARLOG_UPDATE_FAILED);
    goto cleanup;
  }

  if (clar_add_text(cs->clarlog_state, clar_id, text3, text3_len) < 0) {
    ns_error(log_f, NEW_SRV_ERR_DISK_WRITE_ERROR);
    goto cleanup;
  }

  if (phr->action == NEW_SRV_ACTION_PRIV_SUBMIT_RUN_COMMENT_AND_IGNORE) {
    run_change_status_4(cs->runlog_state, run_id, RUN_IGNORED);
  } else if (phr->action == NEW_SRV_ACTION_PRIV_SUBMIT_RUN_COMMENT_AND_OK) {
    struct section_problem_data *prob = 0;
    int full_score = 0;
    int user_status = 0, user_score = 0;
    if (re.prob_id > 0 && re.prob_id <= cs->max_prob) prob = cs->probs[re.prob_id];
    if (prob) full_score = prob->full_score;
    if (global->separate_user_score > 0 && re.is_saved) {
      user_status = RUN_OK;
      user_score = -1;
      if (prob) user_score = prob->full_user_score;
      if (prob && user_score < 0) user_score = prob->full_score;
      if (user_score < 0) user_score = 0;
    }
    run_change_status_3(cs->runlog_state, run_id, RUN_OK,
                        full_score, re.test, re.passed_mode, 0, 0,
                        re.saved_score, user_status, re.saved_test,
                        user_score);
  }

  const unsigned char *audit_cmd = NULL;
  int status = -1;
  if (phr->action == NEW_SRV_ACTION_PRIV_SUBMIT_RUN_COMMENT) {
    audit_cmd = "comment-run";
  } else if (phr->action == NEW_SRV_ACTION_PRIV_SUBMIT_RUN_COMMENT_AND_OK) {
    audit_cmd = "comment-run-ok";
    status = RUN_OK;
  } else if (phr->action == NEW_SRV_ACTION_PRIV_SUBMIT_RUN_COMMENT_AND_IGNORE) {
    audit_cmd = "comment-run-ignore";
    status = RUN_IGNORED;
  } else {
    abort();
  }

  serve_audit_log(cs, run_id, &re, phr->user_id, &phr->ip, phr->ssl_flag,
                  audit_cmd, "ok", status, NULL);

  if (global->notify_clar_reply) {
    unsigned char nsubj[1024];
    FILE *msg_f = 0;
    char *msg_t = 0;
    size_t msg_z = 0;

    if (cnts->default_locale_num > 0)
      l10n_setlocale(cnts->default_locale_num);
    snprintf(nsubj, sizeof(nsubj),
             _("Your submit has been commented in contest %d"),
             cnts->id);
    msg_f = open_memstream(&msg_t, &msg_z);
    if (phr->action == NEW_SRV_ACTION_PRIV_SUBMIT_RUN_COMMENT_AND_IGNORE) {
      fprintf(msg_f, _("You submit has been commented and ignored\n"));
    } else if (phr->action == NEW_SRV_ACTION_PRIV_SUBMIT_RUN_COMMENT_AND_OK) {
      fprintf(msg_f, _("You submit has been commented and accepted\n"));
    } else {
      fprintf(msg_f, _("You submit has been commented\n"));
    }
    fprintf(msg_f, _("Contest: %d (%s)\n"), cnts->id, cnts->name);
    fprintf(msg_f, "Run Id: %d\n", run_id);
    if (cnts->team_url) {
      fprintf(msg_f, "URL: %s?contest_id=%d&login=%s\n", cnts->team_url,
              cnts->id, teamdb_get_login(cs->teamdb_state, re.user_id));
    }
    fprintf(msg_f, "%s\n", text3);
    fprintf(msg_f, "\n-\nRegards,\nthe ejudge contest management system (www.ejudge.ru)\n");
    close_memstream(msg_f); msg_f = 0;
    if (cnts->default_locale_num > 0) {
      l10n_setlocale(cnts->default_locale_num);
    }
    serve_send_email_to_user(ejudge_config, cnts, cs, re.user_id, nsubj, msg_t);
    xfree(msg_t); msg_t = 0; msg_z = 0;
  }

 cleanup:
  html_armor_free(&ab);
  return 0;

 invalid_param:
  ns_html_err_inv_param(fout, phr, 0, errmsg);
  return -1;
}

static int
priv_clar_reply(
        FILE *fout,
        FILE *log_f,
        struct http_request_info *phr,
        const struct contest_desc *cnts,
        struct contest_extra *extra)
{
  serve_state_t cs = extra->serve_state;
  const struct section_global_data *global = cs->global;
  const unsigned char *errmsg;
  const unsigned char *s, *reply_txt;
  int in_reply_to, n, clar_id, from_id;
  struct clar_entry_v1 clar;
  unsigned char *reply_txt_2;
  size_t reply_len;
  unsigned char *orig_txt = 0;
  size_t orig_len = 0;
  unsigned char *new_subj, *quoted, *msg;
  size_t new_subj_len, quoted_len, msg_len;
  struct timeval precise_time;

  // reply, in_reply_to
  if (ns_cgi_param(phr, "in_reply_to", &s) <= 0
      || sscanf(s, "%d%n", &in_reply_to, &n) != 1 || s[n]
      || in_reply_to < 0 || in_reply_to >= clar_get_total(cs->clarlog_state)) {
    errmsg = "in_reply_to parameter is invalid";
    goto invalid_param;
  }

  switch (phr->action) {
  case NEW_SRV_ACTION_CLAR_REPLY:
  case NEW_SRV_ACTION_CLAR_REPLY_ALL:
    if (ns_cgi_param(phr, "reply", &reply_txt) <= 0) {
      errmsg = "reply parameter is invalid";
      goto invalid_param;
    }
  }

  if (opcaps_check(phr->caps, OPCAP_REPLY_MESSAGE) < 0) {
    ns_error(log_f, NEW_SRV_ERR_PERMISSION_DENIED);
    goto cleanup;
  }

  if (clar_get_record(cs->clarlog_state, in_reply_to, &clar) < 0
      || clar.id < 0) {
    ns_error(log_f, NEW_SRV_ERR_INV_CLAR_ID);
    goto cleanup;
  }

  if (!clar.from) {
    ns_error(log_f, NEW_SRV_ERR_CANNOT_REPLY_TO_JUDGE);
    goto cleanup;
  }

  l10n_setlocale(clar.locale_id);
  switch (phr->action) {
  case NEW_SRV_ACTION_CLAR_REPLY_READ_PROBLEM:
    reply_txt = _("Read the problem.");
    break;
  case NEW_SRV_ACTION_CLAR_REPLY_NO_COMMENTS:
    reply_txt = _("No comments.");
    break;
  case NEW_SRV_ACTION_CLAR_REPLY_YES:
    reply_txt = _("Yes.");
    break;
  case NEW_SRV_ACTION_CLAR_REPLY_NO:
    reply_txt = _("No.");
    break;
  }
  l10n_setlocale(0);

  reply_len = strlen(reply_txt);
  if (reply_len > 128 * 1024 * 1024) {
    ns_error(log_f, NEW_SRV_ERR_MESSAGE_TOO_LONG, reply_len);
    goto cleanup;
  }
  reply_txt_2 = (unsigned char*) alloca(reply_len + 1);
  memcpy(reply_txt_2, reply_txt, reply_len + 1);
  while (reply_len > 0 && isspace(reply_txt_2[reply_len - 1])) reply_len--;
  reply_txt_2[reply_len] = 0;
  if (!reply_len) {
    ns_error(log_f, NEW_SRV_ERR_MESSAGE_EMPTY);
    goto cleanup;
  }

  if (clar_get_text(cs->clarlog_state, in_reply_to, &orig_txt, &orig_len) < 0) {
    ns_error(log_f, NEW_SRV_ERR_DISK_READ_ERROR);
    goto cleanup;
  }

  l10n_setlocale(clar.locale_id);
  new_subj = alloca(orig_len + 64);
  new_subj_len = message_reply_subj(orig_txt, new_subj);
  l10n_setlocale(0);

  quoted_len = message_quoted_size(orig_txt);
  quoted = alloca(quoted_len + 16);
  message_quote(orig_txt, quoted);

  msg = alloca(reply_len + quoted_len + new_subj_len + 64);
  msg_len = sprintf(msg, "%s%s\n%s\n", new_subj, quoted, reply_txt_2);

  from_id = clar.from;
  if (phr->action == NEW_SRV_ACTION_CLAR_REPLY_ALL) from_id = 0;

  gettimeofday(&precise_time, 0);
  clar_id = clar_add_record(cs->clarlog_state,
                            precise_time.tv_sec,
                            precise_time.tv_usec * 1000,
                            msg_len,
                            &phr->ip,
                            phr->ssl_flag,
                            0, from_id, 0, phr->user_id, 0,
                            clar.locale_id, in_reply_to + 1, 0, 0,
                            utf8_mode, NULL,
                            clar_get_subject(cs->clarlog_state,
                                             in_reply_to));

  if (clar_id < 0) {
    ns_error(log_f, NEW_SRV_ERR_CLARLOG_UPDATE_FAILED);
    goto cleanup;
  }

  if (clar_add_text(cs->clarlog_state, clar_id, msg, msg_len) < 0) {
    ns_error(log_f, NEW_SRV_ERR_DISK_WRITE_ERROR);
    goto cleanup;
  }

  clar_update_flags(cs->clarlog_state, in_reply_to, 2);

  if (global->notify_clar_reply) {
    unsigned char nsubj[1024];
    FILE *msg_f = 0;
    char *msg_t = 0;
    size_t msg_z = 0;

    if (cnts->default_locale_num > 0)
      l10n_setlocale(cnts->default_locale_num);
    snprintf(nsubj, sizeof(nsubj),
             _("You have received a reply from judges in contest %d"),
             cnts->id);
    msg_f = open_memstream(&msg_t, &msg_z);
    fprintf(msg_f, _("You have received a reply from judges\n"));
    fprintf(msg_f, _("Contest: %d (%s)\n"), cnts->id, cnts->name);
    fprintf(msg_f, "Clar Id: %d\n", in_reply_to);
    if (cnts->team_url) {
      fprintf(msg_f, "URL: %s?contest_id=%d&login=%s\n", cnts->team_url,
              cnts->id, teamdb_get_login(cs->teamdb_state, from_id));
    }
    fprintf(msg_f, "%s\n", msg);
    fprintf(msg_f, "\n-\nRegards,\nthe ejudge contest management system (www.ejudge.ru)\n");
    close_memstream(msg_f); msg_f = 0;
    if (cnts->default_locale_num > 0) {
      l10n_setlocale(cnts->default_locale_num);
    }
    serve_send_email_to_user(ejudge_config, cnts, cs, from_id, nsubj, msg_t);
    xfree(msg_t); msg_t = 0; msg_z = 0;
  }

 cleanup:
  xfree(orig_txt);
  return 0;

 invalid_param:
  ns_html_err_inv_param(fout, phr, 0, errmsg);
  return -1;
}

static int
parse_run_id(FILE *fout, struct http_request_info *phr,
             const struct contest_desc *cnts,
             struct contest_extra *extra, int *p_run_id, struct run_entry *pe)
{
  const serve_state_t cs = extra->serve_state;
  int n, run_id;
  const unsigned char *s = 0, *errmsg = 0;
  unsigned char msgbuf[1024];
  
  if (!(n = ns_cgi_param(phr, "run_id", &s))) {
    errmsg = ns_strerror_r(msgbuf, sizeof(msgbuf),
                           NEW_SRV_ERR_RUN_ID_UNDEFINED);
    goto failure;
  }
  if (n < 0) {
    snprintf(msgbuf, sizeof(msgbuf), "`run_id' value is binary.\n");
    errmsg = msgbuf;
    goto failure;
  }
  if (n < 0 || sscanf(s, "%d%n", &run_id, &n) != 1 || s[n]) {
    //errmsg = ns_strerror_r(msgbuf, sizeof(msgbuf), NEW_SRV_ERR_INV_RUN_ID);
    snprintf(msgbuf, sizeof(msgbuf), "`run_id' value is invalid: |%s|.\n",
             s);
    errmsg = msgbuf;
    goto failure;
  }
  if (run_id < 0 || run_id >= run_get_total(cs->runlog_state)) {
          /*
    errmsg = ns_strerror_r(msgbuf, sizeof(msgbuf),
                           NEW_SRV_ERR_INV_RUN_ID, run_id);
                           */
    snprintf(msgbuf, sizeof(msgbuf), "`run_id' value %d is out of range.\n",
             run_id);
    errmsg = msgbuf;
    goto failure;
  }

  if (p_run_id) *p_run_id = run_id;
  if (pe && run_get_entry(cs->runlog_state, run_id, pe) < 0) {
    errmsg = ns_strerror_r(msgbuf, sizeof(msgbuf),
                           NEW_SRV_ERR_RUNLOG_READ_FAILED, run_id);
    goto failure;
  }

  return 0;

 failure:
  html_error_status_page(fout, phr, cnts, extra, errmsg,
                         ns_priv_prev_state[phr->action], 0);
  return -1;
}

static int
priv_print_run_cmd(FILE *fout, FILE *log_f,
                   struct http_request_info *phr,
                   const struct contest_desc *cnts,
                   struct contest_extra *extra)
{
  serve_state_t cs = extra->serve_state;
  int retval = 0, run_id = -1;

  if (parse_run_id(fout, phr, cnts, extra, &run_id, 0) < 0) {
    retval = -1;
    goto cleanup;
  }
  if (opcaps_check(phr->caps, OPCAP_PRINT_RUN) < 0)
    FAIL(NEW_SRV_ERR_PERMISSION_DENIED);
  if (priv_print_run(cs, run_id, phr->user_id) < 0)
    FAIL(NEW_SRV_ERR_PRINTING_FAILED);

 cleanup:
  return retval;
}

static int
priv_clear_run(FILE *fout, FILE *log_f,
               struct http_request_info *phr,
               const struct contest_desc *cnts,
               struct contest_extra *extra)
{
  serve_state_t cs = extra->serve_state;
  const struct section_global_data *global = cs->global;
  int retval = 0, run_id = -1;
  struct run_entry re;

  if (parse_run_id(fout, phr, cnts, extra, &run_id, 0) < 0) {
    retval = -1;
    goto cleanup;
  }
  if (opcaps_check(phr->caps, OPCAP_CONTROL_CONTEST) < 0)
    FAIL(NEW_SRV_ERR_PERMISSION_DENIED);
  if (run_is_readonly(cs->runlog_state, run_id))
    FAIL(NEW_SRV_ERR_RUN_READ_ONLY);
  if (run_get_entry(cs->runlog_state, run_id, &re) < 0)
    FAIL(NEW_SRV_ERR_INV_RUN_ID);
  if (run_clear_entry(cs->runlog_state, run_id) < 0)
    FAIL(NEW_SRV_ERR_RUNLOG_UPDATE_FAILED);

  if (re.store_flags == 1) {
    uuid_archive_remove(cs, re.run_uuid);
  } else {
    archive_remove(cs, global->run_archive_dir, run_id, 0);
    archive_remove(cs, global->xml_report_archive_dir, run_id, 0);
    archive_remove(cs, global->report_archive_dir, run_id, 0);
    archive_remove(cs, global->team_report_archive_dir, run_id, 0);
    archive_remove(cs, global->full_archive_dir, run_id, 0);
    //archive_remove(cs, global->audit_log_dir, run_id, 0);
  }

  serve_audit_log(cs, run_id, &re, phr->user_id, &phr->ip, phr->ssl_flag,
                  "clear-run", "ok", -1, NULL);

 cleanup:
  return retval;
}

/*
 * what we gonna handle here
 * NEW_SRV_ACTION_CHANGE_RUN_USER_ID
 * NEW_SRV_ACTION_CHANGE_RUN_USER_LOGIN
 * NEW_SRV_ACTION_CHANGE_RUN_PROB_ID
 * NEW_SRV_ACTION_CHANGE_RUN_VARIANT
 * NEW_SRV_ACTION_CHANGE_RUN_LANG_ID
 * NEW_SRV_ACTION_CHANGE_RUN_IS_IMPORTED
 * NEW_SRV_ACTION_CHANGE_RUN_IS_HIDDEN
 * NEW_SRV_ACTION_CHANGE_RUN_IS_EXAMINABLE
 * NEW_SRV_ACTION_CHANGE_RUN_IS_READONLY
 * NEW_SRV_ACTION_CHANGE_RUN_IS_MARKED
 * NEW_SRV_ACTION_CHANGE_RUN_IS_SAVED
 * NEW_SRV_ACTION_CHANGE_RUN_TEST
 * NEW_SRV_ACTION_CHANGE_RUN_SCORE
 * NEW_SRV_ACTION_CHANGE_RUN_SCORE_ADJ
 * NEW_SRV_ACTION_CHANGE_RUN_PAGES
 */
static int
priv_edit_run(FILE *fout, FILE *log_f,
              struct http_request_info *phr,
              const struct contest_desc *cnts,
              struct contest_extra *extra)
{
  serve_state_t cs = extra->serve_state;
  const struct section_global_data *global = cs->global;
  const struct section_problem_data *prob = 0;
  int retval = 0, run_id = -1, n;
  struct run_entry re, ne;
  const unsigned char *s, *param_str = 0;
  int param_int = 0, param_bool = 0;
  int ne_mask = 0;
  const unsigned char *audit_cmd = NULL;
  unsigned char old_buf[1024];
  unsigned char new_buf[1024];

  old_buf[0] = 0;
  new_buf[0] = 0;

  if (parse_run_id(fout, phr, cnts, extra, &run_id, &re) < 0) return -1;
  if (ns_cgi_param(phr, "param", &s) <= 0) {
    ns_html_err_inv_param(fout, phr, 1, "param is not set");
    return -1;
  }
  snprintf(phr->next_extra, sizeof(phr->next_extra), "run_id=%d", run_id);

  if (opcaps_check(phr->caps, OPCAP_EDIT_RUN) < 0)
    FAIL(NEW_SRV_ERR_PERMISSION_DENIED);

  switch (phr->action) {
  case NEW_SRV_ACTION_CHANGE_RUN_USER_LOGIN:
    param_str = s;
    break;
  case NEW_SRV_ACTION_CHANGE_RUN_USER_ID:
  case NEW_SRV_ACTION_CHANGE_RUN_PROB_ID:
  case NEW_SRV_ACTION_CHANGE_RUN_VARIANT:
  case NEW_SRV_ACTION_CHANGE_RUN_LANG_ID:
  case NEW_SRV_ACTION_CHANGE_RUN_TEST:
  case NEW_SRV_ACTION_CHANGE_RUN_SCORE:
  case NEW_SRV_ACTION_CHANGE_RUN_SCORE_ADJ:
  case NEW_SRV_ACTION_CHANGE_RUN_PAGES:
    if (sscanf(s, "%d%n", &param_int, &n) != 1 || s[n]) {
      ns_html_err_inv_param(fout, phr, 1, "invalid integer param");
      return -1;
    }
    break;
  case NEW_SRV_ACTION_CHANGE_RUN_IS_IMPORTED:
  case NEW_SRV_ACTION_CHANGE_RUN_IS_HIDDEN:
  case NEW_SRV_ACTION_CHANGE_RUN_IS_EXAMINABLE:
  case NEW_SRV_ACTION_CHANGE_RUN_IS_READONLY:
  case NEW_SRV_ACTION_CHANGE_RUN_IS_MARKED:
  case NEW_SRV_ACTION_CHANGE_RUN_IS_SAVED:
    if (sscanf(s, "%d%n", &param_bool, &n) != 1 || s[n]
        || param_bool < 0 || param_bool > 1) {
      ns_html_err_inv_param(fout, phr, 1, "invalid boolean param");
      return -1;
    }
    break;
  default:
    ns_error(log_f, NEW_SRV_ERR_UNHANDLED_ACTION, phr->action);
    goto cleanup;
  }

  if (re.is_readonly && phr->action != NEW_SRV_ACTION_CHANGE_RUN_IS_READONLY)
    FAIL(NEW_SRV_ERR_RUN_READ_ONLY);

  memset(&ne, 0, sizeof(ne));
  switch (phr->action) {
  case NEW_SRV_ACTION_CHANGE_RUN_USER_LOGIN:
    if ((ne.user_id = teamdb_lookup_login(cs->teamdb_state, param_str)) <= 0)
      FAIL(NEW_SRV_ERR_INV_USER_LOGIN);
    ne_mask = RE_USER_ID;
    audit_cmd = "change-user-id";
    snprintf(old_buf, sizeof(old_buf), "%d", re.user_id);
    snprintf(new_buf, sizeof(new_buf), "%d", ne.user_id);
    break;
  case NEW_SRV_ACTION_CHANGE_RUN_USER_ID:
    if (teamdb_lookup(cs->teamdb_state, param_int) <= 0)
      FAIL(NEW_SRV_ERR_INV_USER_ID);
    ne.user_id = param_int;
    ne_mask = RE_USER_ID;
    audit_cmd = "change-user-id";
    snprintf(old_buf, sizeof(old_buf), "%d", re.user_id);
    snprintf(new_buf, sizeof(new_buf), "%d", ne.user_id);
    break;
  case NEW_SRV_ACTION_CHANGE_RUN_PROB_ID:
    if (param_int <= 0 || param_int > cs->max_prob || !cs->probs[param_int])
      FAIL(NEW_SRV_ERR_INV_PROB_ID);
    ne.prob_id = param_int;
    ne_mask = RE_PROB_ID;
    audit_cmd = "change-prob-id";
    snprintf(old_buf, sizeof(old_buf), "%d", re.prob_id);
    snprintf(new_buf, sizeof(new_buf), "%d", ne.prob_id);
    break;
  case NEW_SRV_ACTION_CHANGE_RUN_VARIANT:
    if (re.prob_id <= 0 || re.prob_id > cs->max_prob
        || !(prob = cs->probs[re.prob_id]))
      FAIL(NEW_SRV_ERR_INV_PROB_ID);
    if (prob->variant_num <= 0) {
      if (param_int)
        FAIL(NEW_SRV_ERR_INV_VARIANT);
    } else {
      if (param_int < 0 || param_int > prob->variant_num)
        FAIL(NEW_SRV_ERR_INV_VARIANT);
      if (!param_int && find_variant(cs, re.user_id, re.prob_id, 0) <= 0)
        FAIL(NEW_SRV_ERR_VARIANT_UNASSIGNED);
    }
    ne.variant = param_int;
    ne_mask = RE_VARIANT;
    audit_cmd = "change-variant";
    snprintf(old_buf, sizeof(old_buf), "%d", re.variant);
    snprintf(new_buf, sizeof(new_buf), "%d", ne.variant);
    break;
  case NEW_SRV_ACTION_CHANGE_RUN_LANG_ID:
    if (param_int <= 0 || param_int > cs->max_lang || !cs->langs[param_int])
      FAIL(NEW_SRV_ERR_INV_LANG_ID);
    ne.lang_id = param_int;
    ne_mask = RE_LANG_ID;
    audit_cmd = "change-lang-id";
    snprintf(old_buf, sizeof(old_buf), "%d", re.lang_id);
    snprintf(new_buf, sizeof(new_buf), "%d", ne.lang_id);
    break;
  case NEW_SRV_ACTION_CHANGE_RUN_TEST:
    if (param_int < -1 || param_int >= 100000)
      FAIL(NEW_SRV_ERR_INV_TEST);
    if (global->score_system == SCORE_KIROV
        || global->score_system == SCORE_OLYMPIAD)
      param_int++;
    ne.test = param_int;
    ne.passed_mode = 1;
    ne_mask = RE_TEST | RE_PASSED_MODE;
    audit_cmd = "change-test";
    snprintf(old_buf, sizeof(old_buf), "%d", re.test);
    snprintf(new_buf, sizeof(new_buf), "%d", ne.test);
    break;
  case NEW_SRV_ACTION_CHANGE_RUN_SCORE:
    /*
    if (global->score_system == SCORE_ACM
        || (global->score_system == SCORE_OLYMPIAD && cs->accepting_mode))
      FAIL(NEW_SRV_ERR_INV_PARAM);
    */
    if (re.prob_id <= 0 || re.prob_id > cs->max_prob
        || !(prob = cs->probs[re.prob_id]))
      FAIL(NEW_SRV_ERR_INV_PROB_ID);
    if (param_int < 0 || param_int > prob->full_score)
      FAIL(NEW_SRV_ERR_INV_SCORE);
    ne.score = param_int;
    ne_mask = RE_SCORE;
    audit_cmd = "change-score";
    snprintf(old_buf, sizeof(old_buf), "%d", re.score);
    snprintf(new_buf, sizeof(new_buf), "%d", ne.score);
    break;
  case NEW_SRV_ACTION_CHANGE_RUN_SCORE_ADJ:
    if (global->score_system != SCORE_KIROV
        && (global->score_system != SCORE_OLYMPIAD || cs->accepting_mode))
      FAIL(NEW_SRV_ERR_INV_PARAM);
    if (param_int <= -100000 || param_int >= 100000)
      FAIL(NEW_SRV_ERR_INV_SCORE_ADJ);
    ne.score_adj = param_int;
    ne_mask = RE_SCORE_ADJ;
    audit_cmd = "change-score-adj";
    snprintf(old_buf, sizeof(old_buf), "%d", re.score_adj);
    snprintf(new_buf, sizeof(new_buf), "%d", ne.score_adj);
    break;
  case NEW_SRV_ACTION_CHANGE_RUN_PAGES:
    if (param_int < 0 || param_int >= 100000)
      FAIL(NEW_SRV_ERR_INV_PAGES);
    ne.pages = param_int;
    ne_mask = RE_PAGES;
    audit_cmd = "change-pages";
    snprintf(old_buf, sizeof(old_buf), "%d", re.pages);
    snprintf(new_buf, sizeof(new_buf), "%d", ne.pages);
    break;
  case NEW_SRV_ACTION_CHANGE_RUN_IS_IMPORTED:
    ne.is_imported = param_bool;
    ne_mask = RE_IS_IMPORTED;
    audit_cmd = "change-is-imported";
    snprintf(old_buf, sizeof(old_buf), "%d", re.is_imported);
    snprintf(new_buf, sizeof(new_buf), "%d", ne.is_imported);
    break;
  case NEW_SRV_ACTION_CHANGE_RUN_IS_HIDDEN:
    ne.is_hidden = param_bool;
    ne_mask = RE_IS_HIDDEN;
    audit_cmd = "change-is-hidden";
    snprintf(old_buf, sizeof(old_buf), "%d", re.is_hidden);
    snprintf(new_buf, sizeof(new_buf), "%d", ne.is_hidden);
    break;
  case NEW_SRV_ACTION_CHANGE_RUN_IS_EXAMINABLE:
    /*
    ne.is_examinable = param_bool;
    ne_mask = RE_IS_EXAMINABLE;
    audit_cmd = "change-is-examinable";
    snprintf(old_buf, sizeof(old_buf), "%d", re.is_examinable);
    snprintf(new_buf, sizeof(new_buf), "%d", ne.is_examinable);
    */
    break;
  case NEW_SRV_ACTION_CHANGE_RUN_IS_READONLY:
    ne.is_readonly = param_bool;
    ne_mask = RE_IS_READONLY;
    audit_cmd = "change-is-readonly";
    snprintf(old_buf, sizeof(old_buf), "%d", re.is_readonly);
    snprintf(new_buf, sizeof(new_buf), "%d", ne.is_readonly);
    break;
  case NEW_SRV_ACTION_CHANGE_RUN_IS_MARKED:
    ne.is_marked = param_bool;
    ne_mask = RE_IS_MARKED;
    audit_cmd = "change-is-marked";
    snprintf(old_buf, sizeof(old_buf), "%d", re.is_marked);
    snprintf(new_buf, sizeof(new_buf), "%d", ne.is_marked);
    break;
  case NEW_SRV_ACTION_CHANGE_RUN_IS_SAVED:
    ne.is_saved = param_bool;
    ne_mask = RE_IS_SAVED;
    audit_cmd = "change-is-saved";
    snprintf(old_buf, sizeof(old_buf), "%d", re.is_saved);
    snprintf(new_buf, sizeof(new_buf), "%d", ne.is_saved);
    break;
  }

  if (!ne_mask) goto cleanup;

  if (run_set_entry(cs->runlog_state, run_id, ne_mask, &ne) < 0)
    FAIL(NEW_SRV_ERR_RUNLOG_UPDATE_FAILED);

  serve_audit_log(cs, run_id, &re, phr->user_id, &phr->ip, phr->ssl_flag,
                  audit_cmd, "ok", -1,
                  "  Old value: %s\n"
                  "  New value: %s\n",
                  old_buf, new_buf);

 cleanup:
  return retval;
}

/*
 * NEW_SRV_ACTION_CHANGE_RUN_STATUS:
 */
static int
priv_change_status(
        FILE *fout,
        FILE *log_f,
        struct http_request_info *phr,
        const struct contest_desc *cnts,
        struct contest_extra *extra)
{
  serve_state_t cs = extra->serve_state;
  const unsigned char *errmsg = 0, *s;
  int run_id, n, status, flags;
  struct run_entry new_run, re;
  const struct section_problem_data *prob = 0;

  // run_id, status
  if (parse_run_id(fout, phr, cnts, extra, &run_id, 0) < 0) goto failure;
  snprintf(phr->next_extra, sizeof(phr->next_extra), "run_id=%d", run_id);
  if (ns_cgi_param(phr, "status", &s) <= 0
      || sscanf(s, "%d%n", &status, &n) != 1 || s[n]
      || status < 0 || status > RUN_LAST) {
    errmsg = "invalid status";
    goto invalid_param;
  }
  if (opcaps_check(phr->caps, OPCAP_EDIT_RUN) < 0
      && ((status != RUN_REJUDGE && status != RUN_FULL_REJUDGE)
          || opcaps_check(phr->caps, OPCAP_REJUDGE_RUN))) {
    ns_error(log_f, NEW_SRV_ERR_PERMISSION_DENIED);
    goto cleanup;
  }
  if (status == RUN_REJUDGE || status == RUN_FULL_REJUDGE) {
    serve_rejudge_run(ejudge_config, cnts, cs, run_id, phr->user_id, &phr->ip, phr->ssl_flag,
                      (status == RUN_FULL_REJUDGE),
                      DFLT_G_REJUDGE_PRIORITY_ADJUSTMENT);
    goto cleanup;
  }
  if (!serve_is_valid_status(cs, status, 1)) {
    ns_error(log_f, NEW_SRV_ERR_INV_STATUS);
    goto cleanup;
  }

  if (run_get_entry(cs->runlog_state, run_id, &re) < 0) {
    ns_error(log_f, NEW_SRV_ERR_INV_RUN_ID);
    goto cleanup;
  }
  if (re.prob_id <= 0 || re.prob_id > cs->max_prob
      || !(prob = cs->probs[re.prob_id])) {
    ns_error(log_f, NEW_SRV_ERR_INV_RUN_ID);
    goto cleanup;
  }

  memset(&new_run, 0, sizeof(new_run));
  new_run.status = status;
  flags = RE_STATUS;
  if (status == RUN_OK && prob->variable_full_score <= 0) {
    new_run.score = prob->full_score;
    flags |= RE_SCORE;
  }

  if (prob->type >= PROB_TYPE_OUTPUT_ONLY
      && prob->type <= PROB_TYPE_SELECT_MANY) {
    if (status == RUN_OK) {
      new_run.test = 1;
      new_run.passed_mode = 1;
      flags |= RE_TEST | RE_PASSED_MODE;
    } else if (status == RUN_WRONG_ANSWER_ERR
               || status == RUN_PRESENTATION_ERR
               || status == RUN_PARTIAL) {
      new_run.test = 0;
      new_run.passed_mode = 1;
      new_run.score = 0;
      flags |= RE_TEST | RE_PASSED_MODE | RE_SCORE;
    }
  }

  if (run_set_entry(cs->runlog_state, run_id, flags, &new_run) < 0) {
    ns_error(log_f, NEW_SRV_ERR_RUNLOG_UPDATE_FAILED);
    goto cleanup;
  }

  serve_audit_log(cs, run_id, &re, phr->user_id, &phr->ip, phr->ssl_flag,
                  "change-status", "ok", status, NULL);

  if (cs->global->notify_status_change > 0) {
    if (!re.is_hidden)
      serve_notify_user_run_status_change(ejudge_config, cnts, cs, re.user_id, run_id,
                                          status);
  }

 cleanup:
  return 0;

 invalid_param:
  ns_html_err_inv_param(fout, phr, 0, errmsg);
 failure:
  return -1;
}

static int
priv_simple_change_status(
        FILE *fout,
        FILE *log_f,
        struct http_request_info *phr,
        const struct contest_desc *cnts,
        struct contest_extra *extra)
{
  serve_state_t cs = extra->serve_state;
  const unsigned char *errmsg = 0;
  int run_id, status, flags;
  struct run_entry new_run, re;
  const struct section_problem_data *prob = 0;
  const unsigned char *audit_cmd = NULL;

  if (parse_run_id(fout, phr, cnts, extra, &run_id, 0) < 0) goto failure;

  if (phr->action == NEW_SRV_ACTION_PRIV_SUBMIT_RUN_JUST_IGNORE) {
    status = RUN_IGNORED;
    audit_cmd = "set-ignored";
  } else if (phr->action == NEW_SRV_ACTION_PRIV_SUBMIT_RUN_JUST_OK) {
    status = RUN_OK;
    audit_cmd = "set-ok";
  } else {
    errmsg = "invalid status";
    goto invalid_param;
  }

  if (opcaps_check(phr->caps, OPCAP_COMMENT_RUN) < 0) {
    ns_error(log_f, NEW_SRV_ERR_PERMISSION_DENIED);
    goto cleanup;
  }

  if (run_get_entry(cs->runlog_state, run_id, &re) < 0) {
    ns_error(log_f, NEW_SRV_ERR_INV_RUN_ID);
    goto cleanup;
  }
  if (re.prob_id <= 0 || re.prob_id > cs->max_prob
      || !(prob = cs->probs[re.prob_id])) {
    ns_error(log_f, NEW_SRV_ERR_INV_RUN_ID);
    goto cleanup;
  }
  if ((re.status != RUN_ACCEPTED && re.status != RUN_PENDING_REVIEW) && opcaps_check(phr->caps, OPCAP_EDIT_RUN)<0){
    ns_error(log_f, NEW_SRV_ERR_PERMISSION_DENIED);
    goto cleanup;    
  }

  memset(&new_run, 0, sizeof(new_run));
  new_run.status = status;
  flags = RE_STATUS;
  if (status == RUN_OK && prob->variable_full_score <= 0) {
    new_run.score = prob->full_score;
    flags |= RE_SCORE;
  }
  if (run_set_entry(cs->runlog_state, run_id, flags, &new_run) < 0) {
    ns_error(log_f, NEW_SRV_ERR_RUNLOG_UPDATE_FAILED);
    goto cleanup;
  }

  serve_audit_log(cs, run_id, &re, phr->user_id, &phr->ip, phr->ssl_flag,
                  audit_cmd, "ok", status, NULL);

  if (cs->global->notify_status_change > 0) {
    if (!re.is_hidden)
      serve_notify_user_run_status_change(ejudge_config, cnts, cs, re.user_id, run_id,
                                          status);
  }

 cleanup:
  return 0;

 invalid_param:
  ns_html_err_inv_param(fout, phr, 0, errmsg);
 failure:
  return -1;
}

static int
parse_run_mask(struct http_request_info *phr,
               const unsigned char **p_size_str,
               const unsigned char **p_mask_str,
               size_t *p_size,
               unsigned long **p_mask)
{
  const unsigned char *size_str = 0;
  const unsigned char *mask_str = 0;
  size_t size = 0, mask_len;
  unsigned long *mask = 0;
  int n, i;
  unsigned char *s;

  if (p_size_str) *p_size_str = 0;
  if (p_mask_str) *p_mask_str = 0;
  if (p_size) *p_size = 0;
  if (p_mask) *p_mask = 0;

  if (ns_cgi_param(phr, "run_mask_size", &size_str) <= 0) {
    err("parse_run_mask: `run_mask_size' is not defined or binary");
    goto invalid_param;
  }
  if (sscanf(size_str, "%zu%n", &size, &n) != 1
      || size_str[n] || size > 100000) {
    err("parse_run_mask: `run_mask_size' value is invalid");
    goto invalid_param;
  }
  if (!size) {
    if (p_size_str) *p_size_str = "0";
    if (p_mask_str) *p_mask_str = "";
    return 0;
  }

  if (ns_cgi_param(phr, "run_mask", &mask_str) <= 0) {
    err("parse_run_mask: `run_mask' is not defined or binary");
    goto invalid_param;
  }

  XCALLOC(mask, size);
  mask_len = strlen(mask_str);
  s = (unsigned char*) alloca(mask_len + 1);
  memcpy(s, mask_str, mask_len + 1);
  while (mask_len > 0 && isspace(s[mask_len - 1])) mask_len--;
  s[mask_len] = 0;
  for (i = 0; i < size; i++) {
    if (sscanf(s, "%lx%n", &mask[i], &n) != 1) {
      err("parse_run_mask: cannot parse mask[%d]", i);
      goto invalid_param;
    }
    s += n;
  }
  if (*s) {
    err("parse_run_mask: garbage at end");
    goto invalid_param;
  }

  if (p_size_str) *p_size_str = size_str;
  if (p_mask_str) *p_mask_str = mask_str;
  if (p_size) *p_size = size;
  if (p_mask) {
    *p_mask = mask;
    mask = 0;
  }
  xfree(mask);
  return 1;

 invalid_param:
  xfree(mask);
  return -1;
}

static int
priv_clear_displayed(FILE *fout,
                     FILE *log_f,
                     struct http_request_info *phr,
                     const struct contest_desc *cnts,
                     struct contest_extra *extra)
{
  serve_state_t cs = extra->serve_state;
  unsigned long *mask = 0;
  size_t mask_size;
  int retval = 0;

  if (parse_run_mask(phr, 0, 0, &mask_size, &mask) < 0) goto invalid_param;
  if (!mask_size) FAIL(NEW_SRV_ERR_NO_RUNS_TO_REJUDGE);
  if (opcaps_check(phr->caps, OPCAP_EDIT_RUN) < 0)
    FAIL(NEW_SRV_ERR_PERMISSION_DENIED);

  switch (phr->action) {
  case NEW_SRV_ACTION_CLEAR_DISPLAYED_2:
    serve_clear_by_mask(cs, phr->user_id, &phr->ip, phr->ssl_flag,
                        mask_size, mask);
    break;
  case NEW_SRV_ACTION_IGNORE_DISPLAYED_2:
    serve_ignore_by_mask(cs, phr->user_id, &phr->ip, phr->ssl_flag,
                         mask_size, mask, RUN_IGNORED);
    break;
  case NEW_SRV_ACTION_DISQUALIFY_DISPLAYED_2:
    serve_ignore_by_mask(cs, phr->user_id, &phr->ip, phr->ssl_flag,
                         mask_size, mask, RUN_DISQUALIFIED);
    break;
  case NEW_SRV_ACTION_MARK_DISPLAYED_2:
    serve_mark_by_mask(cs, phr->user_id, &phr->ip, phr->ssl_flag,
                       mask_size, mask, 1);
    break;
  case NEW_SRV_ACTION_UNMARK_DISPLAYED_2:
    serve_mark_by_mask(cs, phr->user_id, &phr->ip, phr->ssl_flag,
                       mask_size, mask, 0);
    break;
  default:
    abort();
  }

 cleanup:
  xfree(mask);
  return retval;

 invalid_param:
  ns_html_err_inv_param(fout, phr, 0, 0);
  xfree(mask);
  return -1;
}

static int
priv_rejudge_displayed(FILE *fout,
                       FILE *log_f,
                       struct http_request_info *phr,
                       const struct contest_desc *cnts,
                       struct contest_extra *extra)
{
  serve_state_t cs = extra->serve_state;
  const struct section_global_data *global = cs->global;
  unsigned long *mask = 0;
  size_t mask_size;
  int force_full = 0;
  int prio_adj = DFLT_G_REJUDGE_PRIORITY_ADJUSTMENT;
  int retval = 0;
  int background_mode = 0;

  if (parse_run_mask(phr, 0, 0, &mask_size, &mask) < 0) goto invalid_param;
  if (!mask_size) FAIL(NEW_SRV_ERR_NO_RUNS_TO_REJUDGE);
  ns_cgi_param_int_opt(phr, "background_mode", &background_mode, 0);
  if (background_mode != 1) background_mode = 0;

  if (opcaps_check(phr->caps, OPCAP_REJUDGE_RUN) < 0)
    FAIL(NEW_SRV_ERR_PERMISSION_DENIED);

  if (global->score_system == SCORE_OLYMPIAD
      && cs->accepting_mode
      && phr->action == NEW_SRV_ACTION_FULL_REJUDGE_DISPLAYED_2) {
    force_full = 1;
    prio_adj = 10;
  }

  ns_add_job(serve_rejudge_by_mask(ejudge_config, cnts, cs, phr->user_id,
                                   &phr->ip, phr->ssl_flag,
                                   mask_size, mask, force_full, prio_adj,
                                   background_mode));

 cleanup:
  xfree(mask);
  return retval;

 invalid_param:
  ns_html_err_inv_param(fout, phr, 0, 0);
  xfree(mask);
  return -1;
}

static int
priv_rejudge_problem(FILE *fout,
                     FILE *log_f,
                     struct http_request_info *phr,
                     const struct contest_desc *cnts,
                     struct contest_extra *extra)
{
  serve_state_t cs = extra->serve_state;
  const struct section_problem_data *prob = 0;
  const unsigned char *s;
  int prob_id, n;
  int background_mode = 0;

  if (ns_cgi_param(phr, "prob_id", &s) <= 0
      || sscanf(s, "%d%n", &prob_id, &n) != 1 || s[n]
      || prob_id <= 0 || prob_id > cs->max_prob
      || !(prob = cs->probs[prob_id])
      || prob->disable_testing)
    goto invalid_param;
  ns_cgi_param_int_opt(phr, "background_mode", &background_mode, 0);
  if (background_mode != 1) background_mode = 0;

  if (opcaps_check(phr->caps, OPCAP_REJUDGE_RUN) < 0) {
    ns_error(log_f, NEW_SRV_ERR_PERMISSION_DENIED);
    goto cleanup;
  }

  ns_add_job(serve_rejudge_problem(ejudge_config, cnts, cs, phr->user_id,
                                   &phr->ip, phr->ssl_flag, prob_id,
                                   DFLT_G_REJUDGE_PRIORITY_ADJUSTMENT,
                                   background_mode));

 cleanup:
  return 0;

 invalid_param:
  ns_html_err_inv_param(fout, phr, 0, 0);
  return -1;
}

static int
priv_rejudge_all(FILE *fout,
                 FILE *log_f,
                 struct http_request_info *phr,
                 const struct contest_desc *cnts,
                 struct contest_extra *extra)
{
  serve_state_t cs = extra->serve_state;
  int background_mode = 0;

  ns_cgi_param_int_opt(phr, "background_mode", &background_mode, 0);
  if (background_mode != 1) background_mode = 0;

  if (opcaps_check(phr->caps, OPCAP_REJUDGE_RUN) < 0) {
    ns_error(log_f, NEW_SRV_ERR_PERMISSION_DENIED);
    goto cleanup;
  }

  switch (phr->action) {
  case NEW_SRV_ACTION_REJUDGE_SUSPENDED_2:
    ns_add_job(serve_judge_suspended(ejudge_config, cnts, cs, phr->user_id, &phr->ip, phr->ssl_flag, DFLT_G_REJUDGE_PRIORITY_ADJUSTMENT, background_mode));
    break;
  case NEW_SRV_ACTION_REJUDGE_ALL_2:
    ns_add_job(serve_rejudge_all(ejudge_config, cnts, cs, phr->user_id, &phr->ip, phr->ssl_flag, DFLT_G_REJUDGE_PRIORITY_ADJUSTMENT, background_mode));
    
    break;
  default:
    abort();
  }

 cleanup:
  return 0;
}

static int
priv_new_run(FILE *fout,
             FILE *log_f,
             struct http_request_info *phr,
             const struct contest_desc *cnts,
             struct contest_extra *extra)
{
  serve_state_t cs = extra->serve_state;
  const struct section_global_data *global = cs->global;
  const struct section_problem_data *prob = 0;
  const struct section_language_data *lang = 0;
  int retval = 0;
  const unsigned char *s = 0;
  int user_id = 0, n, x, i;
  int prob_id = 0, variant = 0, lang_id = 0;
  int is_imported = 0, is_hidden = 0, is_readonly = 0, status = 0;
  int tests = 0, score = 0, mime_type = 0;
  const unsigned char *run_text = 0;
  size_t run_size = 0;
  char **lang_list = 0;
  ruint32_t shaval[5];
  const unsigned char *mime_type_str = 0;
  struct timeval precise_time;
  int arch_flags = 0, run_id;
  path_t run_path;
  struct run_entry re;
  int re_flags = 0;

  memset(&re, 0, sizeof(re));

  // run_user_id, run_user_login, prob_id, variant, language,
  // is_imported, is_hidden, is_readonly, status,
  // tests, score, file
  if (ns_cgi_param(phr, "run_user_id", &s) > 0
      && sscanf(s, "%d%n", &x, &n) == 1 && !s[n]
      && teamdb_lookup(cs->teamdb_state, x))
    user_id = x;
  x = 0;
  if (ns_cgi_param(phr, "run_user_login", &s) > 0 && *s)
    x = teamdb_lookup_login(cs->teamdb_state, s);
  if (user_id <= 0 && x <= 0)
    FAIL(NEW_SRV_ERR_UNDEFINED_USER_ID_LOGIN);
  if (user_id > 0 && x > 0 && user_id != x)
    FAIL(NEW_SRV_ERR_CONFLICTING_USER_ID_LOGIN);
  if (user_id <= 0) user_id = x;

  if (ns_cgi_param(phr, "prob_id", &s) <= 0
      || sscanf(s, "%d%n", &prob_id, &n) != 1 || s[n]
      || prob_id <= 0 || prob_id > cs->max_prob
      || !(prob = cs->probs[prob_id]))
    FAIL(NEW_SRV_ERR_INV_PROB_ID);
  if (ns_cgi_param(phr, "variant", &s) > 0 && *s) {
    if (sscanf(s, "%d%n", &variant, &n) != 1 || s[n]
        || prob->variant_num <= 0 || variant < 0
        || variant > prob->variant_num)
      FAIL(NEW_SRV_ERR_INV_VARIANT);
  }

  // check language, content-type, binariness and other stuff
  if (prob->type == PROB_TYPE_STANDARD) {
    if (ns_cgi_param(phr, "language", &s) <= 0
        || sscanf(s, "%d%n", &lang_id, &n) != 1 || s[n]
        || lang_id <= 0 || lang_id > cs->max_lang
        || !(lang = cs->langs[lang_id]))
      FAIL(NEW_SRV_ERR_INV_LANG_ID);
  }
  switch (prob->type) {
  case PROB_TYPE_STANDARD:      // "file"
  case PROB_TYPE_TESTS:
  case PROB_TYPE_OUTPUT_ONLY:
  case PROB_TYPE_TEXT_ANSWER:
  case PROB_TYPE_SHORT_ANSWER:
    if (!ns_cgi_param_bin(phr, "file", &run_text, &run_size)) {
      run_text = "";
      run_size = 0;
    }
    break;
  default:
    FAIL(NEW_SRV_ERR_INV_PROB_ID);
  }

  switch (prob->type) {
  case PROB_TYPE_STANDARD:
    if (!lang->binary && strlen(run_text) != run_size)
      FAIL(NEW_SRV_ERR_BINARY_FILE);
    if (prob->disable_ctrl_chars > 0 && has_control_characters(run_text))
      FAIL(NEW_SRV_ERR_INV_CHAR);
    break;

  case PROB_TYPE_OUTPUT_ONLY:
  case PROB_TYPE_TESTS:
    if (!prob->binary_input && !prob->binary && strlen(run_text) != run_size)
      FAIL(NEW_SRV_ERR_BINARY_FILE);
    break;

  case PROB_TYPE_TEXT_ANSWER:
  case PROB_TYPE_SHORT_ANSWER:
  case PROB_TYPE_SELECT_ONE:
    if (strlen(run_text) != run_size)
      FAIL(NEW_SRV_ERR_BINARY_FILE);
    break;

  case PROB_TYPE_SELECT_MANY:
  case PROB_TYPE_CUSTOM:
    break;
  }

  if (lang) {
    if (lang->disabled) FAIL(NEW_SRV_ERR_LANG_DISABLED);

    if (prob->enable_language) {
      lang_list = prob->enable_language;
      for (i = 0; lang_list[i]; i++)
        if (!strcmp(lang_list[i], lang->short_name))
          break;
      if (!lang_list[i]) FAIL(NEW_SRV_ERR_LANG_NOT_AVAIL_FOR_PROBLEM);
    } else if (prob->disable_language) {
      lang_list = prob->disable_language;
      for (i = 0; lang_list[i]; i++)
        if (!strcmp(lang_list[i], lang->short_name))
          break;
      if (lang_list[i]) FAIL(NEW_SRV_ERR_LANG_DISABLED_FOR_PROBLEM);
    }
  } else {
    // guess the content-type and check it against the list
    if ((mime_type = mime_type_guess(global->diff_work_dir,
                                     run_text, run_size)) < 0)
      FAIL(NEW_SRV_ERR_CANNOT_DETECT_CONTENT_TYPE);
    mime_type_str = mime_type_get_type(mime_type);
    if (prob->enable_language) {
      lang_list = prob->enable_language;
      for (i = 0; lang_list[i]; i++)
        if (!strcmp(lang_list[i], mime_type_str))
          break;
      if (!lang_list[i]) {
        ns_error(log_f, NEW_SRV_ERR_CONTENT_TYPE_NOT_AVAILABLE, mime_type_str);
        goto cleanup;
      }
    } else if (prob->disable_language) {
      lang_list = prob->disable_language;
      for (i = 0; lang_list[i]; i++)
        if (!strcmp(lang_list[i], mime_type_str))
          break;
      if (lang_list[i]) {
        ns_error(log_f, NEW_SRV_ERR_CONTENT_TYPE_DISABLED, mime_type_str);
        goto cleanup;
      }
    }
  }
  sha_buffer(run_text, run_size, shaval);

  if (ns_cgi_param(phr, "is_imported", &s) > 0 && *s) {
    if (sscanf(s, "%d%n", &is_imported, &n) != 1 || s[n]
        || is_imported < 0 || is_imported > 1)
      FAIL(NEW_SRV_ERR_INV_PARAM);
    re.is_imported = is_imported;
    re_flags |= RE_IS_IMPORTED;
  }
  if (ns_cgi_param(phr, "is_hidden", &s) > 0 && *s) {
    if (sscanf(s, "%d%n", &is_hidden, &n) != 1 || s[n]
        || is_hidden < 0 || is_hidden > 1)
      FAIL(NEW_SRV_ERR_INV_PARAM);
  }
  if (ns_cgi_param(phr, "is_readonly", &s) > 0 && *s) {
    if (sscanf(s, "%d%n", &is_readonly, &n) != 1 || s[n]
        || is_readonly < 0 || is_readonly > 1)
      FAIL(NEW_SRV_ERR_INV_PARAM);
    re.is_readonly = is_readonly;
    re_flags |= RE_IS_READONLY;
  }
  if (ns_cgi_param(phr, "status", &s) > 0 && *s) {
    if (sscanf(s, "%d%n", &status, &n) != 1 || s[n]
        || status < 0 || status > RUN_MAX_STATUS
        || !serve_is_valid_status(cs, status, 1))
      FAIL(NEW_SRV_ERR_INV_STATUS);
    re.status = status;
    re_flags |= RE_STATUS;
  }
  if (ns_cgi_param(phr, "tests", &s) > 0 && *s) {
    if (sscanf(s, "%d%n", &tests, &n) != 1 || s[n]
        || tests < -1 || tests > 100000)
      FAIL(NEW_SRV_ERR_INV_TEST);
    re.test = tests;
    re.passed_mode = 1;
    re_flags |= RE_TEST | RE_PASSED_MODE;
  }
  if (ns_cgi_param(phr, "score", &s) > 0 && *s) {
    if (sscanf(s, "%d%n", &score, &n) != 1 || s[n]
        || score < 0 || score > 100000)
      FAIL(NEW_SRV_ERR_INV_PARAM);
    re.score = score;
    re_flags |= RE_SCORE;
  }

  if (!lang) lang_id = 0;
  gettimeofday(&precise_time, 0);

  ruint32_t run_uuid[4];
  int store_flags = 0;
  ej_uuid_generate(run_uuid);
  if (global->uuid_run_store > 0 && run_get_uuid_hash_state(cs->runlog_state) >= 0 && ej_uuid_is_nonempty(run_uuid)) {
    store_flags = 1;
  }
  run_id = run_add_record(cs->runlog_state, 
                          precise_time.tv_sec, precise_time.tv_usec * 1000,
                          run_size, shaval, run_uuid,
                          &phr->ip, phr->ssl_flag, phr->locale_id,
                          user_id, prob_id, lang_id, 0, variant,
                          is_hidden, mime_type, store_flags);
  if (run_id < 0) FAIL(NEW_SRV_ERR_RUNLOG_UPDATE_FAILED);
  serve_move_files_to_insert_run(cs, run_id);

  if (store_flags == 1) {
    arch_flags = uuid_archive_prepare_write_path(cs, run_path, sizeof(run_path),
                                                 run_uuid, run_size, DFLT_R_UUID_SOURCE, 0, 0);
  } else {
    arch_flags = archive_prepare_write_path(cs, run_path, sizeof(run_path),
                                            global->run_archive_dir, run_id,
                                            run_size, NULL, 0, 0);
  }
  if (arch_flags < 0) {
    run_undo_add_record(cs->runlog_state, run_id);
    ns_error(log_f, NEW_SRV_ERR_DISK_WRITE_ERROR);
    goto cleanup;
  }

  if (generic_write_file(run_text, run_size, arch_flags, 0, run_path, "") < 0) {
    run_undo_add_record(cs->runlog_state, run_id);
    ns_error(log_f, NEW_SRV_ERR_DISK_WRITE_ERROR);
    goto cleanup;
  }
  run_set_entry(cs->runlog_state, run_id, re_flags, &re);

  serve_audit_log(cs, run_id, NULL, phr->user_id, &phr->ip, phr->ssl_flag,
                  "priv-new-run", "ok", RUN_PENDING, NULL);

 cleanup:
  return retval;
}

static const unsigned char * const form_row_attrs[]=
{
  " bgcolor=\"#d0d0d0\"",
  " bgcolor=\"#e0e0e0\"",
};

static const unsigned char * const confirmation_headers[NEW_SRV_ACTION_LAST] =
{
  [NEW_SRV_ACTION_REJUDGE_DISPLAYED_1] = __("Rejudge displayed runs"),
  [NEW_SRV_ACTION_FULL_REJUDGE_DISPLAYED_1] = __("Fully rejudge displayed runs"),
  [NEW_SRV_ACTION_REJUDGE_PROBLEM_1] = __("Rejudge problem"),
  [NEW_SRV_ACTION_REJUDGE_SUSPENDED_1] = __("Judge suspended runs"),
  [NEW_SRV_ACTION_REJUDGE_ALL_1] = __("Rejudge all runs"),
  [NEW_SRV_ACTION_UPDATE_STANDINGS_1] = __("Update the public standings"),
  [NEW_SRV_ACTION_RESET_1] = __("Reset the contest"),
  [NEW_SRV_ACTION_GENERATE_PASSWORDS_1] = __("Generate random contest passwords"),
  [NEW_SRV_ACTION_GENERATE_REG_PASSWORDS_1] = __("Generate random registration passwords"),
  [NEW_SRV_ACTION_CLEAR_PASSWORDS_1] = __("Clear contest passwords"),
  [NEW_SRV_ACTION_CLEAR_DISPLAYED_1] = __("Clear displayed runs"),
  [NEW_SRV_ACTION_IGNORE_DISPLAYED_1] = __("Ignore displayed runs"),
  [NEW_SRV_ACTION_DISQUALIFY_DISPLAYED_1] = __("Disqualify displayed runs"),
};

static const int confirm_next_action[NEW_SRV_ACTION_LAST] =
{
  [NEW_SRV_ACTION_REJUDGE_DISPLAYED_1] = NEW_SRV_ACTION_REJUDGE_DISPLAYED_2,
  [NEW_SRV_ACTION_FULL_REJUDGE_DISPLAYED_1] = NEW_SRV_ACTION_FULL_REJUDGE_DISPLAYED_2,
  [NEW_SRV_ACTION_REJUDGE_PROBLEM_1] = NEW_SRV_ACTION_REJUDGE_PROBLEM_2,
  [NEW_SRV_ACTION_REJUDGE_SUSPENDED_1] = NEW_SRV_ACTION_REJUDGE_SUSPENDED_2,
  [NEW_SRV_ACTION_REJUDGE_ALL_1] = NEW_SRV_ACTION_REJUDGE_ALL_2,
  [NEW_SRV_ACTION_UPDATE_STANDINGS_1] = NEW_SRV_ACTION_UPDATE_STANDINGS_2,
  [NEW_SRV_ACTION_RESET_1] = NEW_SRV_ACTION_RESET_2,
  [NEW_SRV_ACTION_GENERATE_PASSWORDS_1] = NEW_SRV_ACTION_GENERATE_PASSWORDS_2,
  [NEW_SRV_ACTION_GENERATE_REG_PASSWORDS_1] = NEW_SRV_ACTION_GENERATE_REG_PASSWORDS_2,
  [NEW_SRV_ACTION_CLEAR_PASSWORDS_1] = NEW_SRV_ACTION_CLEAR_PASSWORDS_2,
  [NEW_SRV_ACTION_CLEAR_DISPLAYED_1] = NEW_SRV_ACTION_CLEAR_DISPLAYED_2,
  [NEW_SRV_ACTION_IGNORE_DISPLAYED_1] = NEW_SRV_ACTION_IGNORE_DISPLAYED_2,
  [NEW_SRV_ACTION_DISQUALIFY_DISPLAYED_1]=NEW_SRV_ACTION_DISQUALIFY_DISPLAYED_2,
};

static const unsigned char * const confirmation_message[NEW_SRV_ACTION_LAST] =
{
  [NEW_SRV_ACTION_REJUDGE_DISPLAYED_1] = __("Rejudge runs"),
  [NEW_SRV_ACTION_FULL_REJUDGE_DISPLAYED_1] = __("Fully rejudge runs"),
  [NEW_SRV_ACTION_CLEAR_DISPLAYED_1] = __("Clear runs"),
  [NEW_SRV_ACTION_IGNORE_DISPLAYED_1] = __("Ignore runs"),
  [NEW_SRV_ACTION_DISQUALIFY_DISPLAYED_1] = __("Disqualify runs"),
};

static int
priv_confirmation_page(FILE *fout,
                       FILE *log_f,
                       struct http_request_info *phr,
                       const struct contest_desc *cnts,
                       struct contest_extra *extra)
{
  serve_state_t cs = extra->serve_state;
  const struct section_problem_data *prob = 0;
  unsigned char bb[1024];
  const unsigned char *errmsg = 0;
  const unsigned char *run_mask_size_str = 0;
  const unsigned char *run_mask_str = 0;
  int n, i, prob_id = 0;
  size_t run_mask_size = 0;
  unsigned long *run_mask = 0, m;
  const unsigned char *s;
  int disable_ok = 0, runs_count = 0;
  struct html_armor_buffer ab = HTML_ARMOR_INITIALIZER;
  int total_runs = run_get_total(cs->runlog_state);

  switch (phr->action) {
  case NEW_SRV_ACTION_REJUDGE_DISPLAYED_1:
  case NEW_SRV_ACTION_FULL_REJUDGE_DISPLAYED_1:
  case NEW_SRV_ACTION_CLEAR_DISPLAYED_1:
  case NEW_SRV_ACTION_IGNORE_DISPLAYED_1:
  case NEW_SRV_ACTION_DISQUALIFY_DISPLAYED_1:
    // run_mask_size, run_mask
    errmsg = "cannot parse run mask";
    if (parse_run_mask(phr, &run_mask_size_str, &run_mask_str,
                       &run_mask_size, &run_mask) < 0)
      goto invalid_param;
    break;
  case NEW_SRV_ACTION_REJUDGE_PROBLEM_1:
    if (ns_cgi_param(phr, "prob_id", &s) <= 0
        || sscanf(s, "%d%n", &prob_id, &n) != 1 || s[n]
        || prob_id <= 0 || prob_id > cs->max_prob
        || !(prob = cs->probs[prob_id])
        || prob->disable_testing)
      goto invalid_param;
    break;
  }

  l10n_setlocale(phr->locale_id);
  ns_header(fout, extra->header_txt, 0, 0, 0, 0, phr->locale_id, cnts,
            phr->client_key,
            "%s [%s, %d, %s]: %s &quot;%s&quot;",
            ns_unparse_role(phr->role), phr->name_arm, phr->contest_id,
            extra->contest_arm, _("Confirm action"),
            gettext(confirmation_headers[phr->action]));

  switch (phr->action) {
  case NEW_SRV_ACTION_REJUDGE_DISPLAYED_1:
  case NEW_SRV_ACTION_FULL_REJUDGE_DISPLAYED_1:
  case NEW_SRV_ACTION_CLEAR_DISPLAYED_1:
  case NEW_SRV_ACTION_IGNORE_DISPLAYED_1:
  case NEW_SRV_ACTION_DISQUALIFY_DISPLAYED_1:
    fprintf(fout, "<p>%s ", gettext(confirmation_message[phr->action]));
    s = "";
    for (n = 0; n < 8 * sizeof(run_mask[0]) * run_mask_size; n++) {
      i = n / (8 * sizeof(run_mask[0]));
      m = 1L << (n % (8 * sizeof(run_mask[0])));
      if ((run_mask[i] & m)) {
        fprintf(fout, "%s%d", s, n);
        s = ", ";
        runs_count++;
      }
    }
    if (!runs_count) {
      fprintf(fout, "<i>no runs!</i></p>\n");
      disable_ok = 1;
    } else {
      fprintf(fout, " (<b>%d total</b>)?</p>\n", runs_count);
    }
    break;
  case NEW_SRV_ACTION_REJUDGE_PROBLEM_1:
    fprintf(fout, "<p>%s %s(%s)?</p>\n", _("Rejudge problem"),
            prob->short_name, ARMOR(prob->long_name));
    break;
  case NEW_SRV_ACTION_REJUDGE_ALL_1:
    fprintf(fout, "<p><b>Attention! %d runs will be rejudged.</b></p>\n",
            total_runs);
    break;
  }

  fprintf(fout, "<table border=\"0\"><tr><td>");
  html_start_form(fout, 0, phr->self_url, phr->hidden_vars);
  fprintf(fout, "%s", ns_submit_button(bb, sizeof(bb), "nop", 0,
                                              "Cancel"));
  fprintf(fout, "</form></td><td>");
  html_start_form(fout, 1, phr->self_url, phr->hidden_vars);

  switch (phr->action) {
  case NEW_SRV_ACTION_REJUDGE_DISPLAYED_1:
  case NEW_SRV_ACTION_FULL_REJUDGE_DISPLAYED_1:
  case NEW_SRV_ACTION_CLEAR_DISPLAYED_1:
  case NEW_SRV_ACTION_IGNORE_DISPLAYED_1:
  case NEW_SRV_ACTION_DISQUALIFY_DISPLAYED_1:
    html_hidden(fout, "run_mask_size", "%s", run_mask_size_str);
    html_hidden(fout, "run_mask", "%s", run_mask_str);
    break;
  case NEW_SRV_ACTION_REJUDGE_PROBLEM_1:
    html_hidden(fout, "prob_id", "%d", prob_id);
    break;
  case NEW_SRV_ACTION_REJUDGE_ALL_1:
    fprintf(fout, "<select name=\"background_mode\">");
    s = "";
    if (total_runs < 5000) s = " selected=\"selected\"";
    fprintf(fout, "<option value=\"0\"%s>Foreground Mode</option>", s);
    s = "";
    if (total_runs >= 5000) s = " selected=\"selected\"";
    fprintf(fout, "<option value=\"1\"%s>Background Mode</option>", s);
    fprintf(fout, "</select>\n");
    break;
  }

  if (!disable_ok) {
    fprintf(fout, "%s", BUTTON(confirm_next_action[phr->action]));
  }
  fprintf(fout, "</form></td></tr></table>\n");

  ns_footer(fout, extra->footer_txt, extra->copyright_txt, phr->locale_id);
  l10n_setlocale(0);
  html_armor_free(&ab);
  xfree(run_mask);
  return 0;

 invalid_param:
  ns_html_err_inv_param(fout, phr, 0, errmsg);
  html_armor_free(&ab);
  xfree(run_mask);
  return -1;
}

static int
priv_view_user_dump(FILE *fout,
                    FILE *log_f,
                    struct http_request_info *phr,
                    const struct contest_desc *cnts,
                    struct contest_extra *extra)
{
  int retval = 0, r;
  unsigned char *db_text = 0;

  if (opcaps_check(phr->caps, OPCAP_DUMP_USERS) < 0)
    FAIL(NEW_SRV_ERR_PERMISSION_DENIED);

  if (ns_open_ul_connection(phr->fw_state) < 0) {
    ns_html_err_ul_server_down(fout, phr, 1, 0);
    return -1;
  }
  if ((r = userlist_clnt_get_database(ul_conn, ULS_GET_DATABASE,
                                      phr->contest_id, &db_text)) < 0) {
    switch (-r) {
    case ULS_ERR_INVALID_LOGIN:
    case ULS_ERR_INVALID_PASSWORD:
    case ULS_ERR_BAD_CONTEST_ID:
    case ULS_ERR_IP_NOT_ALLOWED:
    case ULS_ERR_NO_PERMS:
    case ULS_ERR_NOT_REGISTERED:
    case ULS_ERR_CANNOT_PARTICIPATE:
      ns_html_err_no_perm(fout, phr, 1, "operation failed: %s",
                          userlist_strerror(-r));
      return -1;
    case ULS_ERR_DISCONNECT:
      ns_html_err_ul_server_down(fout, phr, 1, 0);
      return -1;
    default:
      ns_html_err_internal_error(fout, phr, 1, "operation failed: %s",
                                 userlist_strerror(-r));
      return -1;
    }
  }

  fprintf(fout, "Content-type: text/plain; charset=%s\n\n%s\n",
          EJUDGE_CHARSET, db_text);
  xfree(db_text);

 cleanup:
  return retval;
}

static int
priv_view_runs_dump(FILE *fout,
                    FILE *log_f,
                    struct http_request_info *phr,
                    const struct contest_desc *cnts,
                    struct contest_extra *extra)
{
  serve_state_t cs = extra->serve_state;
  const struct section_global_data *global = cs->global;
  int retval = 0;

  if (phr->role < USER_ROLE_JUDGE
      || opcaps_check(phr->caps, OPCAP_DUMP_RUNS) < 0)
    FAIL(NEW_SRV_ERR_PERMISSION_DENIED);

  switch (phr->action) {
  case NEW_SRV_ACTION_VIEW_RUNS_DUMP:
    write_runs_dump(cs, fout, phr->self_url, global->charset);
    break;

  case NEW_SRV_ACTION_EXPORT_XML_RUNS:
    fprintf(fout, "Content-type: text/plain; charset=%s\n\n", EJUDGE_CHARSET);
    if (run_write_xml(cs->runlog_state, cs, cnts, fout, 1, 0,
                      cs->current_time) < 0)
      FAIL(NEW_SRV_ERR_TRY_AGAIN);
    break;

  case NEW_SRV_ACTION_WRITE_XML_RUNS:
    fprintf(fout, "Content-type: text/plain; charset=%s\n\n", EJUDGE_CHARSET);
    if (run_write_xml(cs->runlog_state, cs, cnts, fout, 0, 0,
                      cs->current_time) < 0)
      FAIL(NEW_SRV_ERR_TRY_AGAIN);
    break;

  case NEW_SRV_ACTION_WRITE_XML_RUNS_WITH_SRC:
    fprintf(fout, "Content-type: text/plain; charset=%s\n\n", EJUDGE_CHARSET);
    if (run_write_xml(cs->runlog_state, cs, cnts, fout, 0, 1,
                      cs->current_time) < 0)
      FAIL(NEW_SRV_ERR_TRY_AGAIN);
    break;

  default:
    abort();
  }

 cleanup:
  return retval;
}

static int
priv_view_audit_log(FILE *fout,
                    FILE *log_f,
                    struct http_request_info *phr,
                    const struct contest_desc *cnts,
                    struct contest_extra *extra)
{
  int run_id;
  int retval = 0;

  if (parse_run_id(fout, phr, cnts, extra, &run_id, 0) < 0) FAIL(1);

  if (opcaps_check(phr->caps, OPCAP_CONTROL_CONTEST) < 0)
    FAIL(NEW_SRV_ERR_PERMISSION_DENIED);

  ns_write_audit_log(extra->serve_state, fout, log_f, phr, cnts, extra,
                     run_id);

 cleanup:
  return retval;
}

static int
priv_diff_page(FILE *fout,
               FILE *log_f,
               struct http_request_info *phr,
               const struct contest_desc *cnts,
               struct contest_extra *extra)
{
  const serve_state_t cs = extra->serve_state;
  const unsigned char *s;
  int run_id1, run_id2, n, total_runs;
  int retval = 0;

  total_runs = run_get_total(cs->runlog_state);
  if (parse_run_id(fout, phr, cnts, extra, &run_id1, 0) < 0) goto failure;
  if (!(n = ns_cgi_param(phr, "run_id2", &s)) || (n > 0 && !*s))
    FAIL(NEW_SRV_ERR_RUN_TO_COMPARE_UNSPECIFIED);
  if (n < 0 || sscanf(s, "%d%n", &run_id2, &n) != 1 || s[n]
      || run_id2 < 0 || run_id2 >= total_runs)
    FAIL(NEW_SRV_ERR_INV_RUN_TO_COMPARE);
  if (opcaps_check(phr->caps, OPCAP_VIEW_SOURCE) < 0)
    FAIL(NEW_SRV_ERR_PERMISSION_DENIED);

  if (compare_runs(cs, fout, run_id1, run_id2) < 0)
    FAIL(NEW_SRV_ERR_RUN_COMPARE_FAILED);

 cleanup:
  return retval;

 failure:
  return -1;
}

static int
priv_user_detail_page(FILE *fout,
                      FILE *log_f,
                      struct http_request_info *phr,
                      const struct contest_desc *cnts,
                      struct contest_extra *extra)
{
  serve_state_t cs = extra->serve_state;
  int retval = 0;
  int user_id, n;
  const unsigned char *s = 0;

  if (ns_cgi_param(phr, "user_id", &s) <= 0
      || sscanf(s, "%d%n", &user_id, &n) != 1 || s[n]
      || !teamdb_lookup(cs->teamdb_state, user_id))
    FAIL(NEW_SRV_ERR_INV_USER_ID);

  if (opcaps_check(phr->caps, OPCAP_GET_USER) < 0)
    FAIL(NEW_SRV_ERR_PERMISSION_DENIED);

  l10n_setlocale(phr->locale_id);
  ns_header(fout, extra->header_txt, 0, 0, 0, 0, phr->locale_id, cnts,
            phr->client_key,
            "%s [%s, %d, %s]: %s %d", ns_unparse_role(phr->role),
            phr->name_arm, phr->contest_id, extra->contest_arm,
            _("Details for user "), user_id);
  ns_user_info_page(fout, log_f, phr, cnts, extra, user_id);
  ns_footer(fout, extra->footer_txt, extra->copyright_txt, phr->locale_id);
  l10n_setlocale(0);

 cleanup:
  return retval;
}

static int
priv_new_run_form_page(FILE *fout,
                       FILE *log_f,
                       struct http_request_info *phr,
                       const struct contest_desc *cnts,
                       struct contest_extra *extra)
{
  int retval = 0;

  if (opcaps_check(phr->caps, OPCAP_SUBMIT_RUN) < 0
      || opcaps_check(phr->caps, OPCAP_EDIT_RUN))
    FAIL(NEW_SRV_ERR_PERMISSION_DENIED);

  l10n_setlocale(phr->locale_id);
  ns_header(fout, extra->header_txt, 0, 0, 0, 0, phr->locale_id, cnts,
            phr->client_key,
            "%s [%s, %d, %s]: %s", ns_unparse_role(phr->role), phr->name_arm,
            phr->contest_id, extra->contest_arm, _("Add new run"));
  ns_new_run_form(fout, log_f, phr, cnts, extra);
  ns_footer(fout, extra->footer_txt, extra->copyright_txt, phr->locale_id);
  l10n_setlocale(0);

 cleanup:
  return retval;
}

static int
priv_examiners_page(
        FILE *fout,
        FILE *log_f,
        struct http_request_info *phr,
        const struct contest_desc *cnts,
        struct contest_extra *extra)
{
  int retval = 0;

  /*
  if (opcaps_check(phr->caps, OPCAP_SUBMIT_RUN) < 0
      || opcaps_check(phr->caps, OPCAP_EDIT_RUN))
    FAIL(NEW_SRV_ERR_PERMISSION_DENIED);
  */

  l10n_setlocale(phr->locale_id);
  ns_header(fout, extra->header_txt, 0, 0, 0, 0, phr->locale_id, cnts,
            phr->client_key,
            "%s [%s, %d, %s]: %s", ns_unparse_role(phr->role), phr->name_arm,
            phr->contest_id, extra->contest_arm, _("Add new run"));
  ns_examiners_page(fout, log_f, phr, cnts, extra);
  ns_footer(fout, extra->footer_txt, extra->copyright_txt, phr->locale_id);
  l10n_setlocale(0);

  //cleanup:
  return retval;
}

static int
priv_assign_chief_examiner(
        FILE *fout,
        FILE *log_f,
        struct http_request_info *phr,
        const struct contest_desc *cnts,
        struct contest_extra *extra)
{
  const serve_state_t cs = extra->serve_state;
  int retval = 0;
  int prob_id = 0;
  int user_id = 0;

  if (phr->role != USER_ROLE_ADMIN && phr->role != USER_ROLE_COORDINATOR)
    FAIL(NEW_SRV_ERR_PERMISSION_DENIED);

  if (ns_cgi_param_int(phr, "prob_id", &prob_id) < 0
      || prob_id <= 0 || prob_id > cs->max_prob || !cs->probs[prob_id]
      || cs->probs[prob_id]->manual_checking <= 0)
    FAIL(NEW_SRV_ERR_INV_PROB_ID);

  if (ns_cgi_param_int(phr, "chief_user_id", &user_id) < 0 || user_id < 0)
    FAIL(NEW_SRV_ERR_INV_USER_ID);
  if (!user_id) {
    user_id = nsdb_find_chief_examiner(phr->contest_id, prob_id);
    if (user_id > 0) {
      nsdb_remove_examiner(user_id, phr->contest_id, prob_id);
    }
    retval = NEW_SRV_ACTION_EXAMINERS_PAGE;
    goto cleanup;
  }
  if (nsdb_check_role(user_id, phr->contest_id, USER_ROLE_CHIEF_EXAMINER) < 0)
    FAIL(NEW_SRV_ERR_INV_USER_ID);
  nsdb_assign_chief_examiner(user_id, phr->contest_id, prob_id);
  retval = NEW_SRV_ACTION_EXAMINERS_PAGE;

 cleanup:
  return retval;
}

static int
priv_assign_examiner(
        FILE *fout,
        FILE *log_f,
        struct http_request_info *phr,
        const struct contest_desc *cnts,
        struct contest_extra *extra)
{
  const serve_state_t cs = extra->serve_state;
  int retval = 0;
  int prob_id = 0;
  int user_id = 0;

  if (phr->role != USER_ROLE_ADMIN && phr->role != USER_ROLE_COORDINATOR)
    FAIL(NEW_SRV_ERR_PERMISSION_DENIED);

  if (ns_cgi_param_int(phr, "prob_id", &prob_id) < 0
      || prob_id <= 0 || prob_id > cs->max_prob || !cs->probs[prob_id]
      || cs->probs[prob_id]->manual_checking <= 0)
    FAIL(NEW_SRV_ERR_INV_PROB_ID);

  if (ns_cgi_param_int(phr, "exam_add_user_id", &user_id) < 0 || user_id < 0)
    FAIL(NEW_SRV_ERR_INV_USER_ID);
  if (!user_id) {
    retval = NEW_SRV_ACTION_EXAMINERS_PAGE;
    goto cleanup;
  }
  if (nsdb_check_role(user_id, phr->contest_id, USER_ROLE_EXAMINER) < 0)
    FAIL(NEW_SRV_ERR_INV_USER_ID);
  nsdb_assign_examiner(user_id, phr->contest_id, prob_id);
  retval = NEW_SRV_ACTION_EXAMINERS_PAGE;

 cleanup:
  return retval;
}

static int
priv_unassign_examiner(
        FILE *fout,
        FILE *log_f,
        struct http_request_info *phr,
        const struct contest_desc *cnts,
        struct contest_extra *extra)
{
  const serve_state_t cs = extra->serve_state;
  int retval = 0;
  int prob_id = 0;
  int user_id = 0;

  if (phr->role != USER_ROLE_ADMIN && phr->role != USER_ROLE_COORDINATOR)
    FAIL(NEW_SRV_ERR_PERMISSION_DENIED);

  if (ns_cgi_param_int(phr, "prob_id", &prob_id) < 0
      || prob_id <= 0 || prob_id > cs->max_prob || !cs->probs[prob_id]
      || cs->probs[prob_id]->manual_checking <= 0)
    FAIL(NEW_SRV_ERR_INV_PROB_ID);

  if (ns_cgi_param_int(phr, "exam_del_user_id", &user_id) < 0 || user_id < 0)
    FAIL(NEW_SRV_ERR_INV_USER_ID);
  if (!user_id) {
    retval = NEW_SRV_ACTION_EXAMINERS_PAGE;
    goto cleanup;
  }
  /*
  if (nsdb_check_role(user_id, phr->contest_id, USER_ROLE_EXAMINER) < 0)
    FAIL(NEW_SRV_ERR_INV_USER_ID);
  */
  nsdb_remove_examiner(user_id, phr->contest_id, prob_id);
  retval = NEW_SRV_ACTION_EXAMINERS_PAGE;

 cleanup:
  return retval;
}

static void
priv_view_users_page(
        FILE *fout,
        struct http_request_info *phr,
        const struct contest_desc *cnts,
        struct contest_extra *extra)
{
  int r;
  unsigned char *xml_text = 0;
  struct userlist_list *users = 0;
  const struct userlist_user *u = 0;
  const struct userlist_contest *uc = 0;
  int uid;
  int row = 1, serial = 1;
  char url[1024];
  unsigned char bb[1024];
  struct html_armor_buffer ab = HTML_ARMOR_INITIALIZER;
  int details_allowed = 0;
  unsigned char cl[128];
  unsigned char b1[1024], b2[1024];
  int new_contest_id = cnts->id;
  const struct section_global_data *global = extra->serve_state->global;
  int *run_counts = 0;
  size_t *run_sizes = 0;

  if (cnts->user_contest_num > 0) new_contest_id = cnts->user_contest_num;
  if (ns_open_ul_connection(phr->fw_state) < 0)
    return ns_html_err_ul_server_down(fout, phr, 1, 0);
  if ((r = userlist_clnt_list_all_users(ul_conn, ULS_LIST_ALL_USERS,
                                        phr->contest_id, &xml_text)) < 0)
    return ns_html_err_internal_error(fout, phr, 1,
                                      "list_all_users failed: %s",
                                      userlist_strerror(-r));
  users = userlist_parse_str(xml_text);
  xfree(xml_text); xml_text = 0;
  if (!users)
    return ns_html_err_internal_error(fout, phr, 1, "XML parsing failed");

  if (users->user_map_size > 0) {
    XCALLOC(run_counts, users->user_map_size);
    XCALLOC(run_sizes, users->user_map_size);
    run_get_all_statistics(extra->serve_state->runlog_state,
                           users->user_map_size, run_counts, run_sizes);
  }

  if (opcaps_check(phr->caps, OPCAP_GET_USER) >= 0) details_allowed = 1;

  l10n_setlocale(phr->locale_id);
  ns_header(fout, extra->header_txt, 0, 0, 0, 0, phr->locale_id, cnts,
            phr->client_key,
            "%s [%s, %d, %s]: %s", ns_unparse_role(phr->role), phr->name_arm,
            phr->contest_id, extra->contest_arm, _("Users page"));

  fprintf(fout, "<h2>Registered users</h2>");

  snprintf(cl, sizeof(cl), " class=\"b1\"");

  html_start_form(fout, 1, phr->self_url, phr->hidden_vars);
  fprintf(fout, "<table%s><tr><th%s>NN</th><th%s>Id</th><th%s>Login</th><th%s>Name</th><th%s>Status</th><th%s>Flags</th><th%s>Reg. date</th><th%s>Login date</th><th%s>No. of submits</th><th%s>Size of submits</th>", cl, cl, cl, cl, cl, cl, cl, cl, cl, cl, cl);
  if (global->memoize_user_results > 0) {
    fprintf(fout, "<th%s>Score</th>", cl);
  }
  fprintf(fout, "<th%s>Select</th></tr>\n", cl);
  for (uid = 1; uid < users->user_map_size; uid++) {
    if (!(u = users->user_map[uid])) continue;
    if (!(uc = userlist_get_user_contest(u, new_contest_id))) continue;

    fprintf(fout, "<tr%s>", form_row_attrs[row ^= 1]);
    fprintf(fout, "<td%s>%d</td>", cl, serial++);

    snprintf(b1, sizeof(b1), "uid == %d", uid);
    url_armor_string(b2, sizeof(b2), b1);
    fprintf(fout, "<td%s>%s%d</a></td>", cl,
            ns_aref(bb, sizeof(bb), phr,
                    NEW_SRV_ACTION_MAIN_PAGE, "filter_expr=%s", b2),
            uid);

    if (details_allowed) {
      fprintf(fout, "<td%s>%s%s</a></td>", cl,
              ns_aref(bb, sizeof(bb), phr,
                      NEW_SRV_ACTION_VIEW_USER_INFO, "user_id=%d", uid),
              ARMOR(u->login));
    } else {
      fprintf(fout, "<td%s>%s</td>", cl, ARMOR(u->login));
    }
    if (u->cnts0 && u->cnts0->name && *u->cnts0->name) {
      fprintf(fout, "<td%s>%s</td>", cl, ARMOR(u->cnts0->name));
    } else {
      fprintf(fout, "<td%s>&nbsp;</td>", cl);
    }
    fprintf(fout, "<td%s>%s</td>", cl, userlist_unparse_reg_status(uc->status));
    if ((uc->flags & USERLIST_UC_ALL)) {
      r = 0;
      fprintf(fout, "<td%s>", cl);
      if ((uc->flags & USERLIST_UC_BANNED))
        fprintf(fout, "%s%s", r++?",":"", "banned");
      if ((uc->flags & USERLIST_UC_INVISIBLE))
        fprintf(fout, "%s%s", r++?",":"", "invisible");
      if ((uc->flags & USERLIST_UC_LOCKED))
        fprintf(fout, "%s%s", r++?",":"", "locked");
      if ((uc->flags & USERLIST_UC_INCOMPLETE))
        fprintf(fout, "%s%s", r++?",":"", "incomplete");
      if ((uc->flags & USERLIST_UC_DISQUALIFIED))
        fprintf(fout, "%s%s", r++?",":"", "disqualified");
      fprintf(fout, "</td>");
    } else {
      fprintf(fout, "<td%s>&nbsp;</td>", cl);
    }
    if (uc->create_time > 0) {
      fprintf(fout, "<td%s>%s</td>", cl, xml_unparse_date(uc->create_time));
    } else {
      fprintf(fout, "<td%s>&nbsp;</td>", cl);
    }
    if (u->cnts0 && u->cnts0->last_login_time > 0) {
      fprintf(fout, "<td%s>%s</td>", cl,
              xml_unparse_date(u->cnts0->last_login_time));
    } else {
      fprintf(fout, "<td%s>&nbsp;</td>", cl);
    }
    if (run_counts[uid] > 0) {
      fprintf(fout, "<td%s>%d</td><td%s>%zu</td>", cl, run_counts[uid],
              cl, run_sizes[uid]);
    } else {
      fprintf(fout, "<td%s>&nbsp;</td><td%s>&nbsp;</td>", cl, cl);
    }
    if (global->memoize_user_results > 0) {
      fprintf(fout, "<td%s>%d</td>", cl, 
              serve_get_user_result_score(extra->serve_state, uid));
    }
    fprintf(fout, "<td%s><input type=\"checkbox\" name=\"user_%d\"/></td>",
            cl, uid);
    fprintf(fout, "</tr>\n");
  }
  fprintf(fout, "</table>\n");

  fprintf(fout, "<h2>Users range</h2>\n");

  fprintf(fout, "<table>\n");
  fprintf(fout, "<tr><td>%s:</td><td>%s</td></tr>\n",
          _("First User_Id"),
          html_input_text(bb, sizeof(bb), "first_user_id", 16, 0, 0));
  fprintf(fout, "<tr><td>%s:</td><td>%s</td></tr>\n",
          _("Last User_Id (incl.)"),
          html_input_text(bb, sizeof(bb), "last_user_id", 16, 0, 0));
  fprintf(fout, "</table>\n");

  fprintf(fout, "<h2>Available actions</h2>\n");

  fprintf(fout, "<table>\n");
  fprintf(fout, "<tr><td>%s%s</a></td><td>%s</td></tr>\n",
          ns_aref(url, sizeof(url), phr, 0, 0),
          _("Back"), _("Return to the main page"));
  fprintf(fout, "<tr><td>%s</td><td>%s</td></tr>\n",
          BUTTON(NEW_SRV_ACTION_USERS_REMOVE_REGISTRATIONS),
          _("Remove the selected users from the list"));
  fprintf(fout, "<tr><td>%s</td><td>%s</td></tr>\n",
          BUTTON(NEW_SRV_ACTION_USERS_SET_PENDING),
          _("Set the registration status of the selected users to PENDING"));
  fprintf(fout, "<tr><td>%s</td><td>%s</td></tr>\n",
          BUTTON(NEW_SRV_ACTION_USERS_SET_OK),
          _("Set the registration status of the selected users to OK"));
  fprintf(fout, "<tr><td>%s</td><td>%s</td></tr>\n",
          BUTTON(NEW_SRV_ACTION_USERS_SET_REJECTED), 
          _("Set the registration status of the selected users to REJECTED"));
  fprintf(fout, "<tr><td>%s</td><td>%s</td></tr>\n",
          BUTTON(NEW_SRV_ACTION_USERS_SET_INVISIBLE),
          _("Set the INVISIBLE flag for the selected users"));
  fprintf(fout, "<tr><td>%s</td><td>%s</td></tr>\n",
          BUTTON(NEW_SRV_ACTION_USERS_CLEAR_INVISIBLE),
          _("Clear the INVISIBLE flag for the selected users"));
  fprintf(fout, "<tr><td>%s</td><td>%s</td></tr>\n",
          BUTTON(NEW_SRV_ACTION_USERS_SET_BANNED),
          _("Set the BANNED flag for the selected users"));
  fprintf(fout, "<tr><td>%s</td><td>%s</td></tr>\n",
          BUTTON(NEW_SRV_ACTION_USERS_CLEAR_BANNED),
          _("Clear the BANNED flag for the selected users"));
  fprintf(fout, "<tr><td>%s</td><td>%s</td></tr>\n",
          BUTTON(NEW_SRV_ACTION_USERS_SET_LOCKED),
          _("Set the LOCKED flag for the selected users"));
  fprintf(fout, "<tr><td>%s</td><td>%s</td></tr>\n",
          BUTTON(NEW_SRV_ACTION_USERS_CLEAR_LOCKED),
          _("Clear the LOCKED flag for the selected users"));
  fprintf(fout, "<tr><td>%s</td><td>%s</td></tr>\n",
          BUTTON(NEW_SRV_ACTION_USERS_SET_INCOMPLETE),
          _("Set the INCOMPLETE flag for the selected users"));
  fprintf(fout, "<tr><td>%s</td><td>%s</td></tr>\n",
          BUTTON(NEW_SRV_ACTION_USERS_CLEAR_INCOMPLETE),
          _("Clear the INCOMPLETE flag for the selected users"));
  fprintf(fout, "<tr><td>%s</td><td>%s</td></tr>\n",
          BUTTON(NEW_SRV_ACTION_USERS_CLEAR_DISQUALIFIED),
          _("Clear the DISQUALIFIED flag for the selected users"));
  if (global->is_virtual) {
    fprintf(fout, "<tr><td>%s</td><td>%s</td></tr>\n",
            BUTTON(NEW_SRV_ACTION_FORCE_START_VIRTUAL),
            _("Force virtual contest start for the selected users"));
  }

  if (global->user_exam_protocol_header_txt)
    fprintf(fout, "<tr><td>%s</td><td>%s</td></tr>\n",
            BUTTON(NEW_SRV_ACTION_PRINT_SELECTED_USER_PROTOCOL),
            _("Print the user examination protocols for the selected users"));
  if (global->full_exam_protocol_header_txt)
    fprintf(fout, "<tr><td>%s</td><td>%s</td></tr>\n",
            BUTTON(NEW_SRV_ACTION_PRINT_SELECTED_USER_FULL_PROTOCOL),
            _("Print the user full examination protocols for the selected users"));
  if (global->full_exam_protocol_header_txt)
    fprintf(fout, "<tr><td>%s</td><td>%s</td></tr>\n",
            BUTTON(NEW_SRV_ACTION_PRINT_SELECTED_UFC_PROTOCOL),
            _("Print the user full cyphered examination protocols for the selected users"));
  fprintf(fout, "</table>\n");

  fprintf(fout, "<h2>%s</h3>\n", _("Disqualify selected users"));
  fprintf(fout, "<p>%s:<br>\n",
          _("Disqualification explanation"));
  fprintf(fout, "<p><textarea name=\"disq_comment\" rows=\"5\" cols=\"60\">");
  fprintf(fout, "</textarea></p>\n");

  fprintf(fout, "<table class=\"b0\"><tr>");
  fprintf(fout, "<td class=\"b0\">%s</td>",
          BUTTON(NEW_SRV_ACTION_USERS_SET_DISQUALIFIED));
  fprintf(fout, "</tr></table>\n");

  fprintf(fout, "<h2>%s</h2>\n", _("Add new user"));
  fprintf(fout, "<table>\n");
  fprintf(fout, "<tr><td><input type=\"text\" size=\"32\" name=\"add_login\"/></td><td>%s</td><td>%s</td></tr>\n",
          BUTTON(NEW_SRV_ACTION_USERS_ADD_BY_LOGIN),
          _("Add a new user specifying his/her login"));
  fprintf(fout, "<tr><td><input type=\"text\" size=\"32\" name=\"add_user_id\"/></td><td>%s</td><td>%s</td></tr>\n",
          BUTTON(NEW_SRV_ACTION_USERS_ADD_BY_USER_ID),
          _("Add a new user specifying his/her User Id"));
  fprintf(fout, "</table>\n");

  fprintf(fout, "</form>\n");

  ns_footer(fout, extra->footer_txt, extra->copyright_txt, phr->locale_id);
  l10n_setlocale(0);

  if (users) userlist_free(&users->b);
  html_armor_free(&ab);
  xfree(run_counts);
  xfree(run_sizes);
}

struct priv_user_info
{
  int user_id;
  unsigned char *login;
  unsigned char *name;
  unsigned int role_mask;
};
static int
priv_user_info_sort_func(const void *v1, const void *v2)
{
  const struct priv_user_info *p1 = *(const struct priv_user_info**) v1;
  const struct priv_user_info *p2 = *(const struct priv_user_info**) v2;

  if (v1 == v2) return 0;
  ASSERT(p1 != p2);
  if (p1->user_id < p2->user_id) return -1;
  if (p1->user_id > p2->user_id) return 1;
  abort();
}

static void
priv_view_priv_users_page(FILE *fout,
                          struct http_request_info *phr,
                          const struct contest_desc *cnts,
                          struct contest_extra *extra)
{
  struct ptrarray_t
  {
    int a, u;
    struct priv_user_info **v;
  };
  struct ptrarray_t users;
  const struct opcap_list_item *op;
  int user_id, i;
  unsigned char *name = 0, *login = 0;
  struct priv_user_info *pp;
  int_iterator_t iter;
  unsigned int role_mask;
  int row = 1, cnt, r;
  unsigned char url[1024];
  unsigned char bb[1024];
  struct html_armor_buffer ab = HTML_ARMOR_INITIALIZER;
  unsigned char cl[128];

  XMEMZERO(&users, 1);

  if (ns_open_ul_connection(phr->fw_state) < 0) {
    ns_html_err_ul_server_down(fout, phr, 1, 0);
    goto cleanup;
  }

  // collect all information about allowed MASTER and JUDGE logins
  for (op = CNTS_FIRST_PERM(cnts); op; op = CNTS_NEXT_PERM(op)) {
    role_mask = 0;
    if (opcaps_check(op->caps, OPCAP_MASTER_LOGIN) >= 0) {
      role_mask |= (1 << USER_ROLE_ADMIN);
    }
    if (opcaps_check(op->caps, OPCAP_JUDGE_LOGIN) >= 0) {
      role_mask |= (1 << USER_ROLE_JUDGE);
    }
    if (!role_mask) continue;
    if (userlist_clnt_lookup_user(ul_conn, op->login, 0, &user_id, &name) < 0)
      continue;
    for (i = 0; i < users.u; i++)
      if (users.v[i]->user_id == user_id)
        break;
    if (i < users.u) {
      xfree(name);
      continue;
    }
    XEXPAND2(users);
    XCALLOC(users.v[users.u], 1);
    pp = users.v[users.u++];
    pp->user_id = user_id;
    pp->login = xstrdup(op->login);
    pp->name = name;
    pp->role_mask |= role_mask;
  }

  // collect information about other roles
  for (iter = nsdb_get_contest_user_id_iterator(phr->contest_id);
       iter->has_next(iter);
       iter->next(iter)) {
    user_id = iter->get(iter);
    if (nsdb_get_priv_role_mask_by_iter(iter, &role_mask) < 0) continue;
    if (userlist_clnt_lookup_user_id(ul_conn, user_id, phr->contest_id,
                                     &login, &name) < 0)
      continue;
    for (i = 0; i < users.u; i++)
      if (users.v[i]->user_id == user_id)
        break;
    if (i < users.u) {
      xfree(login);
      xfree(name);
      users.v[i]->role_mask |= role_mask;
      continue;
    }
    XEXPAND2(users);
    XCALLOC(users.v[users.u], 1);
    pp = users.v[users.u++];
    pp->user_id = user_id;
    pp->login = login;
    pp->name = name;
    pp->role_mask |= role_mask;
  }
  iter->destroy(iter); iter = 0;

  qsort(users.v, users.u, sizeof(users.v[0]), priv_user_info_sort_func);

  l10n_setlocale(phr->locale_id);
  ns_header(fout, extra->header_txt, 0, 0, 0, 0, phr->locale_id, cnts,
            phr->client_key,
            "%s [%s, %d, %s]: %s", ns_unparse_role(phr->role), phr->name_arm,
            phr->contest_id, extra->contest_arm, _("Privileged users page"));

  snprintf(cl, sizeof(cl), " class=\"b1\"");

  fprintf(fout, "<h2>Privileged users</h2>");

  html_start_form(fout, 1, phr->self_url, phr->hidden_vars);
  fprintf(fout, "<table%s><tr><th%s>NN</th><th%s>Id</th><th%s>Login</th><th%s>Name</th><th%s>Roles</th><th%s>Select</th></tr>\n", cl, cl, cl, cl, cl, cl, cl);
  for (i = 0; i < users.u; i++) {
    fprintf(fout, "<tr%s><td%s>%d</td>", form_row_attrs[row ^= 1], cl, i + 1);
    fprintf(fout, "<td%s>%d</td>", cl, users.v[i]->user_id);
    fprintf(fout, "<td%s>%s</td>", cl, ARMOR(users.v[i]->login));
    fprintf(fout, "<td%s>%s</td>", cl, ARMOR(users.v[i]->name));
    if ((role_mask = users.v[i]->role_mask)) {
      fprintf(fout, "<td%s>", cl);
      for (cnt = 0, r = USER_ROLE_OBSERVER; r <= USER_ROLE_ADMIN; r++)
        if ((role_mask & (1 << r)))
          fprintf(fout, "%s%s", cnt++?",":"", ns_unparse_role(r));
      fprintf(fout, "</td>");
    } else {
      fprintf(fout, "<td%s>&nbsp;</td>", cl);
    }
    fprintf(fout, "<td%s><input type=\"checkbox\" name=\"user_%d\"/></td>",
            cl, users.v[i]->user_id);
    fprintf(fout, "</tr>\n");
  }
  fprintf(fout, "</table>\n");

  fprintf(fout, "<h2>Available actions</h2>\n");

  fprintf(fout, "<table>\n");
  fprintf(fout, "<tr><td>%s%s</a></td><td>%s</td></tr>\n",
          ns_aref(url, sizeof(url), phr, 0, 0),
          _("Back"), _("Return to the main page"));
  fprintf(fout, "<tr><td>%s</td><td>%s</td></tr>\n",
          BUTTON(NEW_SRV_ACTION_PRIV_USERS_REMOVE),
          _("Remove the selected users from the list (ADMINISTRATORs cannot be removed)"));
  fprintf(fout, "<tr><td>%s</td><td>%s</td></tr>\n",
          BUTTON(NEW_SRV_ACTION_PRIV_USERS_ADD_OBSERVER),
          _("Add the OBSERVER role to the selected users"));
  fprintf(fout, "<tr><td>%s</td><td>%s</td></tr>\n",
          BUTTON(NEW_SRV_ACTION_PRIV_USERS_DEL_OBSERVER),
          _("Remove the OBSERVER role from the selected users"));
  fprintf(fout, "<tr><td>%s</td><td>%s</td></tr>\n",
          BUTTON(NEW_SRV_ACTION_PRIV_USERS_ADD_EXAMINER),
          _("Add the EXAMINER role to the selected users"));
  fprintf(fout, "<tr><td>%s</td><td>%s</td></tr>\n",
          BUTTON(NEW_SRV_ACTION_PRIV_USERS_DEL_EXAMINER),
          _("Remove the EXAMINER role from the selected users"));
  fprintf(fout, "<tr><td>%s</td><td>%s</td></tr>\n",
          BUTTON(NEW_SRV_ACTION_PRIV_USERS_ADD_CHIEF_EXAMINER),
          _("Add the CHIEF EXAMINER role to the selected users"));
  fprintf(fout, "<tr><td>%s</td><td>%s</td></tr>\n",
          BUTTON(NEW_SRV_ACTION_PRIV_USERS_DEL_CHIEF_EXAMINER),
          _("Remove the CHIEF EXAMINER role from the selected users"));
  fprintf(fout, "<tr><td>%s</td><td>%s</td></tr>\n",
          BUTTON(NEW_SRV_ACTION_PRIV_USERS_ADD_COORDINATOR),
          _("Add the COORDINATOR role to the selected users"));
  fprintf(fout, "<tr><td>%s</td><td>%s</td></tr>\n",
          BUTTON(NEW_SRV_ACTION_PRIV_USERS_DEL_COORDINATOR),
          _("Remove the COORDINATOR role from the selected users"));
  fprintf(fout, "</table>\n");

  fprintf(fout, "<h2>%s</h2>\n", _("Add new user"));
  fprintf(fout, "<table>\n");
  fprintf(fout, "<tr><td><input type=\"text\" size=\"32\" name=\"add_login\"/></td><td>");
  html_role_select(fout, USER_ROLE_OBSERVER, 0, "add_role_1");
  fprintf(fout, "</td><td>%s</td><td>%s</td></tr>\n",
          BUTTON(NEW_SRV_ACTION_PRIV_USERS_ADD_BY_LOGIN),
          _("Add a new user specifying his/her login"));
  fprintf(fout, "<tr><td><input type=\"text\" size=\"32\" name=\"add_user_id\"/></td><td>");
  html_role_select(fout, USER_ROLE_OBSERVER, 0, "add_role_2");
  fprintf(fout, "</td><td>%s</td><td>%s</td></tr>\n",
          BUTTON(NEW_SRV_ACTION_PRIV_USERS_ADD_BY_USER_ID),
          _("Add a new user specifying his/her User Id"));
  fprintf(fout, "</table>\n");

  ns_footer(fout, extra->footer_txt, extra->copyright_txt, phr->locale_id);
  l10n_setlocale(0);

 cleanup:
  for (i = 0; i < users.u; i++) {
    if (users.v[i]) {
      xfree(users.v[i]->login);
      xfree(users.v[i]->name);
    }
    xfree(users.v[i]);
  }
  xfree(users.v);
  if (iter) iter->destroy(iter);
  html_armor_free(&ab);
}

static int
priv_view_report(FILE *fout,
                 FILE *log_f,
                 struct http_request_info *phr,
                 const struct contest_desc *cnts,
                 struct contest_extra *extra)
{
  serve_state_t cs = extra->serve_state;
  int run_id;
  int user_mode = 0;

  if (parse_run_id(fout, phr, cnts, extra, &run_id, 0) < 0) goto failure;

  if (opcaps_check(phr->caps, OPCAP_VIEW_REPORT) < 0) {
    ns_error(log_f, NEW_SRV_ERR_PERMISSION_DENIED);
    goto cleanup;
  }
  if (phr->action == NEW_SRV_ACTION_VIEW_USER_REPORT) user_mode = 1;

  ns_write_priv_report(cs, fout, log_f, phr, cnts, extra, user_mode, run_id);

 cleanup:
  return 0;

 failure:
  return -1;
}

static int
priv_view_source(FILE *fout,
                 FILE *log_f,
                 struct http_request_info *phr,
                 const struct contest_desc *cnts,
                 struct contest_extra *extra)
{
  serve_state_t cs = extra->serve_state;
  int run_id;

  if (parse_run_id(fout, phr, cnts, extra, &run_id, 0) < 0) goto failure;

  if (opcaps_check(phr->caps, OPCAP_VIEW_SOURCE) < 0) {
    ns_error(log_f, NEW_SRV_ERR_PERMISSION_DENIED);
    goto cleanup;
  }

  ns_write_priv_source(cs, fout, log_f, phr, cnts, extra, run_id);

 cleanup:
  return 0;

 failure:
  return -1;
}

static int
priv_download_source(
        FILE *fout,
        FILE *log_f,
        struct http_request_info *phr,
        const struct contest_desc *cnts,
        struct contest_extra *extra)
{
  serve_state_t cs = extra->serve_state;
  const struct section_global_data *global = cs->global;
  int run_id, n, src_flags, no_disp = 0, x;
  const unsigned char *s;
  const struct section_problem_data *prob = 0;
  const struct section_language_data *lang = 0;
  struct run_entry re;
  path_t src_path;
  char *run_text = 0;
  size_t run_size = 0;
  int retval = 0;
  const unsigned char *src_sfx = "";
  const unsigned char *content_type = "text/plain";

  if (parse_run_id(fout, phr, cnts, extra, &run_id, &re) < 0) goto failure;
  if (ns_cgi_param(phr, "no_disp", &s) > 0
      && sscanf(s, "%d%n", &x, &n) == 1 && !s[n]
      && x >= 0 && x <= 1)
    no_disp = x;

  if (opcaps_check(phr->caps, OPCAP_VIEW_SOURCE) < 0)
    FAIL(NEW_SRV_ERR_PERMISSION_DENIED);
  if (re.prob_id <= 0 || re.prob_id > cs->max_prob ||
      !(prob = cs->probs[re.prob_id]))
    FAIL(NEW_SRV_ERR_INV_PROB_ID);
  if (re.status > RUN_LAST
      || (re.status > RUN_MAX_STATUS && re.status < RUN_TRANSIENT_FIRST))
    FAIL(NEW_SRV_ERR_SOURCE_UNAVAILABLE);

  if ((src_flags = serve_make_source_read_path(cs, src_path, sizeof(src_path), &re)) < 0) {
    FAIL(NEW_SRV_ERR_SOURCE_NONEXISTANT);
  }
  if (generic_read_file(&run_text, 0, &run_size, src_flags, 0, src_path, 0)<0)
    FAIL(NEW_SRV_ERR_DISK_READ_ERROR);

  if (prob->type > 0) {
    content_type = mime_type_get_type(re.mime_type);
    src_sfx = mime_type_get_suffix(re.mime_type);
  } else {
    if(re.lang_id <= 0 || re.lang_id > cs->max_lang ||
       !(lang = cs->langs[re.lang_id]))
      FAIL(NEW_SRV_ERR_INV_LANG_ID);
    src_sfx = lang->src_sfx;
    if (!src_sfx) src_sfx = "";

    if (lang->content_type && lang->content_type[0]) {
      content_type = lang->content_type;
    } else if (lang->binary) {
      if (re.mime_type <= 0 && !strcmp(src_sfx, ".tar")) {
        int mime_type = mime_type_guess(global->diff_work_dir, 
                                        run_text, run_size);
        switch (mime_type) {
        case MIME_TYPE_APPL_GZIP: // application/x-gzip
          src_sfx = ".tar.gz";
          break;
        case MIME_TYPE_APPL_TAR:  // application/x-tar
          src_sfx = ".tar";
          break;
        case MIME_TYPE_APPL_ZIP:  // application/zip
          src_sfx = ".zip";
          break;
        case MIME_TYPE_APPL_BZIP2: // application/x-bzip2
          src_sfx = ".tar.bz2";
          break;
        case MIME_TYPE_APPL_7ZIP:  // application/x-7zip
          src_sfx = ".tar.7z";
          break;
        default:
          mime_type = MIME_TYPE_BINARY;
          break;
        }
        content_type = mime_type_get_type(mime_type);
      } else {
        content_type = "application/octet-stream";
      }
    } else {
      content_type = "text/plain";
    }
  }

  fprintf(fout, "Content-type: %s\n", content_type);
  if (!no_disp) {
    fprintf(fout, "Content-Disposition: attachment; filename=\"%06d%s\"\n",
            run_id, src_sfx);
  }
  putc_unlocked('\n', fout);

  fwrite(run_text, 1, run_size, fout);

 cleanup:
  xfree(run_text);
  return retval;

 failure:
  xfree(run_text);
  return -1;
}

static int
priv_view_clar(FILE *fout,
               FILE *log_f,
               struct http_request_info *phr,
               const struct contest_desc *cnts,
               struct contest_extra *extra)
{
  serve_state_t cs = extra->serve_state;
  int clar_id, n;
  const unsigned char *s;

  if (ns_cgi_param(phr, "clar_id", &s) <= 0
      || sscanf(s, "%d%n", &clar_id, &n) != 1 || s[n]
      || clar_id < 0 || clar_id >= clar_get_total(cs->clarlog_state)) {
    ns_html_err_inv_param(fout, phr, 1, "cannot parse clar_id");
    return -1;
  }

  if (opcaps_check(phr->caps, OPCAP_VIEW_CLAR) < 0) {
    ns_error(log_f, NEW_SRV_ERR_PERMISSION_DENIED);
    goto cleanup;
  }

  l10n_setlocale(phr->locale_id);
  ns_header(fout, extra->header_txt, 0, 0, 0, 0, phr->locale_id, cnts,
            phr->client_key,
            "%s [%s, %d, %s]: %s %d", ns_unparse_role(phr->role),
            phr->name_arm, phr->contest_id, extra->contest_arm,
            _("Viewing clar"), clar_id);

  ns_write_priv_clar(cs, fout, log_f, phr, cnts, extra, clar_id);

  ns_footer(fout, extra->footer_txt, extra->copyright_txt, phr->locale_id);
  l10n_setlocale(0);

 cleanup:
  return 0;
}

static int
priv_edit_clar_page(
        FILE *fout,
        FILE *log_f,
        struct http_request_info *phr,
        const struct contest_desc *cnts,
        struct contest_extra *extra)
{
  serve_state_t cs = extra->serve_state;
  int clar_id, n;
  const unsigned char *s;

  if (ns_cgi_param(phr, "clar_id", &s) <= 0
      || sscanf(s, "%d%n", &clar_id, &n) != 1 || s[n]
      || clar_id < 0 || clar_id >= clar_get_total(cs->clarlog_state)) {
    ns_html_err_inv_param(fout, phr, 1, "cannot parse clar_id");
    return -1;
  }

  if (opcaps_check(phr->caps, OPCAP_EDIT_RUN) < 0) {
    ns_error(log_f, NEW_SRV_ERR_PERMISSION_DENIED);
    goto cleanup;
  }

  l10n_setlocale(phr->locale_id);
  ns_header(fout, extra->header_txt, 0, 0, 0, 0, phr->locale_id, cnts,
            phr->client_key,
            "%s [%s, %d, %s]: %s %d", ns_unparse_role(phr->role),
            phr->name_arm, phr->contest_id, extra->contest_arm,
            _("Editing clar"), clar_id);

  ns_priv_edit_clar_page(cs, fout, log_f, phr, cnts, extra, clar_id);

  ns_footer(fout, extra->footer_txt, extra->copyright_txt, phr->locale_id);
  l10n_setlocale(0);

 cleanup:
  return 0;
}

static int
priv_edit_run_page(
        FILE *fout,
        FILE *log_f,
        struct http_request_info *phr,
        const struct contest_desc *cnts,
        struct contest_extra *extra)
{
  serve_state_t cs = extra->serve_state;
  int run_id, n;
  const unsigned char *s;

  if (ns_cgi_param(phr, "run_id", &s) <= 0
      || sscanf(s, "%d%n", &run_id, &n) != 1 || s[n]
      || run_id < 0 || run_id >= run_get_total(cs->runlog_state)) {
    ns_html_err_inv_param(fout, phr, 1, "cannot parse run_id");
    return -1;
  }

  if (opcaps_check(phr->caps, OPCAP_EDIT_RUN) < 0) {
    ns_error(log_f, NEW_SRV_ERR_PERMISSION_DENIED);
    goto cleanup;
  }

  l10n_setlocale(phr->locale_id);
  ns_header(fout, extra->header_txt, 0, 0, 0, 0, phr->locale_id, cnts,
            phr->client_key,
            "%s [%s, %d, %s]: %s %d", ns_unparse_role(phr->role),
            phr->name_arm, phr->contest_id, extra->contest_arm,
            _("Editing run"), run_id);

  ns_priv_edit_run_page(cs, fout, log_f, phr, cnts, extra, run_id);

  ns_footer(fout, extra->footer_txt, extra->copyright_txt, phr->locale_id);
  l10n_setlocale(0);

 cleanup:
  return 0;
}

static int
priv_standings(FILE *fout,
               FILE *log_f,
               struct http_request_info *phr,
               const struct contest_desc *cnts,
               struct contest_extra *extra)
{
  serve_state_t cs = extra->serve_state;

  if (phr->role < USER_ROLE_JUDGE) {
    ns_error(log_f, NEW_SRV_ERR_PERMISSION_DENIED);
    goto cleanup;
  }
  if (opcaps_check(phr->caps, OPCAP_VIEW_STANDINGS) < 0) {
    ns_error(log_f, NEW_SRV_ERR_PERMISSION_DENIED);
    goto cleanup;
  }

  l10n_setlocale(phr->locale_id);
  ns_header(fout, extra->header_txt, 0, 0, 0, 0, phr->locale_id, cnts,
            phr->client_key,
            "%s [%s, %d, %s]: %s", ns_unparse_role(phr->role), phr->name_arm,
            phr->contest_id, extra->contest_arm, _("Current standings"));
  ns_write_priv_standings(cs, phr, cnts, fout, cs->accepting_mode);
  ns_footer(fout, extra->footer_txt, extra->copyright_txt, phr->locale_id);
  l10n_setlocale(0);
  
 cleanup:
  return 0;
}

static int
priv_view_test(FILE *fout,
               FILE *log_f,
               struct http_request_info *phr,
               const struct contest_desc *cnts,
               struct contest_extra *extra)
{
  const serve_state_t cs = extra->serve_state;
  int run_id, test_num, n, retval = 0;
  const unsigned char *s = 0;

  // run_id, test_num
  if (parse_run_id(fout, phr, cnts, extra, &run_id, 0) < 0) goto failure;
  if (ns_cgi_param(phr, "test_num", &s) <= 0
      || sscanf(s, "%d%n", &test_num, &n) != 1 || s[n]) {
    ns_html_err_inv_param(fout, phr, 1, "cannot parse test_num");
    return -1;
  }

  if (opcaps_check(phr->caps, OPCAP_VIEW_REPORT) < 0)
    FAIL(NEW_SRV_ERR_PERMISSION_DENIED);

  if (test_num <= 0) FAIL(NEW_SRV_ERR_INV_TEST);

  ns_write_tests(cs, fout, log_f, phr->action, run_id, test_num);

 cleanup:
  return retval;

 failure:
  return -1;
}

static int
priv_upload_runlog_csv_1(
        FILE *fout,
        FILE *log_f,
        struct http_request_info *phr,
        const struct contest_desc *cnts,
        struct contest_extra *extra)
{
  int retval = 0;
  unsigned char bb[1024];

  if (opcaps_check(phr->caps, OPCAP_IMPORT_XML_RUNS) < 0)
    FAIL(NEW_SRV_ERR_PERMISSION_DENIED);  

  l10n_setlocale(phr->locale_id);
  ns_header(fout, extra->header_txt, 0, 0, 0, 0, phr->locale_id, cnts,
            phr->client_key,
            "%s [%s, %d, %s]: %s", ns_unparse_role(phr->role), phr->name_arm,
            phr->contest_id, extra->contest_arm, "Add new runs in CSV format");
  html_start_form(fout, 2, phr->self_url, phr->hidden_vars);

  fprintf(fout, "<table>");
  /*
  fprintf(fout, "<tr><td>%s</td><td><input type=\"checkbox\" name=\"results_only\"/></td></tr>\n", _("Import results for existing runs"));
  */
  fprintf(fout, "<tr><td><input type=\"radio\" name=\"import_mode\" value=\"0\" checked=\"yes\" /></td><td>%s</td></tr>\n",
          "Create new submits, fail if a submit already exists");
  fprintf(fout, "<tr><td><input type=\"radio\" name=\"import_mode\" value=\"1\"  /></td><td>%s</td></tr>\n",
          "Modify existing submits, fail if a submit does not exist");
  fprintf(fout, "<tr><td><input type=\"radio\" name=\"import_mode\" value=\"2\"  /></td><td>%s</td></tr>\n",
          "Create non-existent submits and modify existent submits");
  fprintf(fout, "<tr><td>%s</td><td><input type=\"file\" name=\"file\"/></td></tr>\n",
          _("File"));
  fprintf(fout, "<tr><td>&nbsp;</td><td>%s</td></tr></table>\n",
          BUTTON(NEW_SRV_ACTION_UPLOAD_RUNLOG_CSV_2));

  fprintf(fout, "</form>\n");
  ns_footer(fout, extra->footer_txt, extra->copyright_txt, phr->locale_id);
  l10n_setlocale(0);

 cleanup:
  return retval;
}

static int
priv_upload_runlog_csv_2(
        FILE *fout,
        FILE *log_f,
        struct http_request_info *phr,
        const struct contest_desc *cnts,
        struct contest_extra *extra)
{
  const serve_state_t cs = extra->serve_state;
  int retval = 0, r;
  unsigned char bb[1024];
  const unsigned char *s = 0, *p;
  char *log_text = 0;
  size_t log_size = 0;
  FILE *ff = 0;
  unsigned char *ss = 0;
  int import_mode = -1;

  if (opcaps_check(phr->caps, OPCAP_IMPORT_XML_RUNS) < 0)
    FAIL(NEW_SRV_ERR_PERMISSION_DENIED);  

  if (!(r = ns_cgi_param(phr, "file", &s)))
    FAIL(NEW_SRV_ERR_FILE_UNSPECIFIED);
  else if (r < 0)
    FAIL(NEW_SRV_ERR_BINARY_FILE);

  for (p = s; *p && isspace(*p); p++);
  if (!*p) FAIL(NEW_SRV_ERR_FILE_EMPTY);

  // import_mode:
  //  0 - new submits
  //  1 - existing submits
  //  2 - both
  if (ns_cgi_param_int(phr, "import_mode", &import_mode) < 0
      || import_mode < 0 || import_mode > 2)
    FAIL(NEW_SRV_ERR_INV_PARAM);

  ff = open_memstream(&log_text, &log_size);
  switch (import_mode) {
  case 0:
    r = ns_upload_csv_runs(phr, cs, ff, s);
    break;
  case 1:
    r = ns_upload_csv_results(phr, cs, ff, s, 0);
    break;
  case 2:
    r = ns_upload_csv_results(phr, cs, ff, s, 1);
    break;
  default:
    FAIL(NEW_SRV_ERR_INV_PARAM);
  }
  close_memstream(ff); ff = 0;

  l10n_setlocale(phr->locale_id);
  ns_header(fout, extra->header_txt, 0, 0, 0, 0, phr->locale_id, cnts,
            phr->client_key,
            "%s [%s, %d, %s]: %s", ns_unparse_role(phr->role), phr->name_arm,
            phr->contest_id, extra->contest_arm, _("Adding new runs"));

  fprintf(fout, "<h2>%s</h2>\n",
          (r >= 0)?_("Operation succeeded"):_("Operation failed"));

  fprintf(fout, "<table>");
  fprintf(fout, "<tr><td>%s%s</a></td></tr></table>",
          ns_aref(bb, sizeof(bb), phr, NEW_SRV_ACTION_MAIN_PAGE, 0),
          _("Main page"));

  ss = html_armor_string_dup(log_text);
  fprintf(fout, "<hr/><pre>");
  if (r < 0) fprintf(fout, "<font color=\"red\">");
  fprintf(fout, "%s", ss);
  if (r < 0) fprintf(fout, "</font>");
  fprintf(fout, "</pre>\n");
  xfree(ss); ss = 0;
  xfree(log_text); log_text = 0;

  ns_footer(fout, extra->footer_txt, extra->copyright_txt, phr->locale_id);
  l10n_setlocale(0);

 cleanup:
  if (ff) fclose(ff);
  xfree(log_text);
  xfree(ss);
  return retval;
}

static int
priv_upload_runlog_xml_1(
        FILE *fout,
        FILE *log_f,
        struct http_request_info *phr,
        const struct contest_desc *cnts,
        struct contest_extra *extra)
{
  int retval = 0;
  unsigned char bb[1024];

  if (opcaps_check(phr->caps, OPCAP_IMPORT_XML_RUNS) < 0)
    FAIL(NEW_SRV_ERR_PERMISSION_DENIED);  

  l10n_setlocale(phr->locale_id);
  ns_header(fout, extra->header_txt, 0, 0, 0, 0, phr->locale_id, cnts,
            phr->client_key,
            "%s [%s, %d, %s]: %s", ns_unparse_role(phr->role), phr->name_arm,
            phr->contest_id, extra->contest_arm, "Merge XML runlog");
  html_start_form(fout, 2, phr->self_url, phr->hidden_vars);

  fprintf(fout, "<table><tr><td>%s</td><td><input type=\"file\" name=\"file\"/></td></tr>\n", _("File"));
  fprintf(fout, "<tr><td>&nbsp;</td><td>%s</td></tr></table>\n",
          BUTTON(NEW_SRV_ACTION_UPLOAD_RUNLOG_XML_2));

  fprintf(fout, "</form>\n");
  ns_footer(fout, extra->footer_txt, extra->copyright_txt, phr->locale_id);
  l10n_setlocale(0);

 cleanup:
  return retval;
}

static int
priv_upload_runlog_xml_2(
        FILE *fout,
        FILE *log_f,
        struct http_request_info *phr,
        const struct contest_desc *cnts,
        struct contest_extra *extra)
{
  const serve_state_t cs = extra->serve_state;
  int retval = 0, r;
  unsigned char bb[1024];
  const unsigned char *s = 0, *p;
  char *log_text = 0;
  size_t log_size = 0;
  FILE *ff = 0;
  unsigned char *ss = 0;

  if (phr->role < USER_ROLE_ADMIN
      || opcaps_check(phr->caps, OPCAP_IMPORT_XML_RUNS) < 0)
    FAIL(NEW_SRV_ERR_PERMISSION_DENIED);
  if (cs->global->enable_runlog_merge <= 0)
    FAIL(NEW_SRV_ERR_NOT_SUPPORTED);

  if (!(r = ns_cgi_param(phr, "file", &s)))
    FAIL(NEW_SRV_ERR_FILE_UNSPECIFIED);
  else if (r < 0)
    FAIL(NEW_SRV_ERR_BINARY_FILE);

  for (p = s; *p && isspace(*p); p++);
  if (!*p) FAIL(NEW_SRV_ERR_FILE_EMPTY);

  ff = open_memstream(&log_text, &log_size);
  runlog_import_xml(cs, cs->runlog_state, ff, 1, s);
  close_memstream(ff); ff = 0;

  l10n_setlocale(phr->locale_id);
  ns_header(fout, extra->header_txt, 0, 0, 0, 0, phr->locale_id, cnts,
            phr->client_key,
            "%s [%s, %d, %s]: %s", ns_unparse_role(phr->role), phr->name_arm,
            phr->contest_id, extra->contest_arm, _("Merging runs"));

  fprintf(fout, "<h2>%s</h2>\n", _("Operation completed"));

  fprintf(fout, "<table>");
  fprintf(fout, "<tr><td>%s%s</a></td></tr></table>",
          ns_aref(bb, sizeof(bb), phr, NEW_SRV_ACTION_MAIN_PAGE, 0),
          _("Main page"));

  ss = html_armor_string_dup(log_text);
  fprintf(fout, "<hr/><pre>");
  fprintf(fout, "%s", ss);
  fprintf(fout, "</pre>\n");
  xfree(ss); ss = 0;
  xfree(log_text); log_text = 0;

  ns_footer(fout, extra->footer_txt, extra->copyright_txt, phr->locale_id);
  l10n_setlocale(0);

 cleanup:
  if (ff) fclose(ff);
  xfree(log_text);
  xfree(ss);
  return retval;
}

static int
priv_download_runs_confirmation(
        FILE *fout,
        FILE *log_f,
        struct http_request_info *phr,
        const struct contest_desc *cnts,
        struct contest_extra *extra)
{
  //const serve_state_t cs = extra->serve_state;
  int retval = 0;
  unsigned char bb[1024];
  unsigned long *mask = 0, mval;
  size_t mask_size = 0;
  const unsigned char *mask_size_str = 0;
  const unsigned char *mask_str = 0;
  size_t mask_count = 0;
  int i, j;

  if (opcaps_check(phr->caps, OPCAP_DUMP_RUNS) < 0)
    FAIL(NEW_SRV_ERR_PERMISSION_DENIED);  

  if (parse_run_mask(phr, &mask_size_str, &mask_str, &mask_size, &mask) < 0)
    goto invalid_param;

  for (i = 0; i < mask_size; i++) {
    mval = mask[i];
    for (j = 0; j < 8 * sizeof(mask[0]); j++, mval >>= 1)
      if ((mval & 1)) mask_count++;
  }

  l10n_setlocale(phr->locale_id);
  ns_header(fout, extra->header_txt, 0, 0, 0, 0, phr->locale_id, cnts,
            phr->client_key,
            "%s [%s, %d, %s]: %s", ns_unparse_role(phr->role), phr->name_arm,
            phr->contest_id, extra->contest_arm, "Download runs configuration");

  html_start_form(fout, 1, phr->self_url, phr->hidden_vars);
  html_hidden(fout, "run_mask_size", "%s", mask_size_str);
  html_hidden(fout, "run_mask", "%s", mask_str);
  fprintf(fout, "<h2>%s</h2>\n", _("Run selection"));
  fprintf(fout, "<table>");
  fprintf(fout, "<tr><td><input type=\"radio\" name=\"run_selection\" value=\"0\" checked=\"yes\"/></td><td>%s</td></tr>\n", _("Download all runs"));
  fprintf(fout, "<tr><td><input type=\"radio\" name=\"run_selection\" value=\"1\"/></td><td>%s (%zu)</td></tr>\n", _("Download selected runs"), mask_count);
  fprintf(fout, "<tr><td><input type=\"radio\" name=\"run_selection\" value=\"2\"/></td><td>%s</td></tr>\n", _("Download OK runs"));
  fprintf(fout, "</table>\n");

  fprintf(fout, "<h2>%s</h2>\n", _("File name pattern"));
  fprintf(fout, "<table>");
  fprintf(fout, "<tr><td><input type=\"checkbox\" name=\"file_pattern_run\" checked=\"yes\"/></td><td>%s</td></tr>\n", _("Use run number"));
  fprintf(fout, "<tr><td><input type=\"checkbox\" name=\"file_pattern_uid\"/></td><td>%s</td></tr>\n", _("Use user Id"));
  fprintf(fout, "<tr><td><input type=\"checkbox\" name=\"file_pattern_login\"/></td><td>%s</td></tr>\n", _("Use user Login"));
  fprintf(fout, "<tr><td><input type=\"checkbox\" name=\"file_pattern_name\"/></td><td>%s</td></tr>\n", _("Use user Name"));
  fprintf(fout, "<tr><td><input type=\"checkbox\" name=\"file_pattern_prob\"/></td><td>%s</td></tr>\n", _("Use problem short name"));
  fprintf(fout, "<tr><td><input type=\"checkbox\" name=\"file_pattern_lang\"/></td><td>%s</td></tr>\n", _("Use programming language short name"));
  fprintf(fout, "<tr><td><input type=\"checkbox\" name=\"file_pattern_suffix\" checked=\"yes\"/></td><td>%s</td></tr>\n", _("Use source language or content type suffix"));
  fprintf(fout, "</table>\n");

  fprintf(fout, "<h2>%s</h2>\n", _("Directory structure"));
  fprintf(fout, "<table>");
  fprintf(fout, "<tr><td><input type=\"radio\" name=\"dir_struct\" value=\"0\" checked=\"yes\"/></td><td>%s</td></tr>\n", _("No directory structure"));
  fprintf(fout, "<tr><td><input type=\"radio\" name=\"dir_struct\" value=\"1\"/></td><td>%s</td></tr>\n", _("/&lt;Problem&gt;/&lt;File&gt;"));
  fprintf(fout, "<tr><td><input type=\"radio\" name=\"dir_struct\" value=\"2\"/></td><td>%s</td></tr>\n", _("/&lt;User_Id&gt;/&lt;File&gt;"));
  fprintf(fout, "<tr><td><input type=\"radio\" name=\"dir_struct\" value=\"3\"/></td><td>%s</td></tr>\n", _("/&lt;User_Login&gt;/&lt;File&gt;"));
  fprintf(fout, "<tr><td><input type=\"radio\" name=\"dir_struct\" value=\"8\"/></td><td>%s</td></tr>\n", _("/&lt;User_Name&gt;/&lt;File&gt;"));
  fprintf(fout, "<tr><td><input type=\"radio\" name=\"dir_struct\" value=\"4\"/></td><td>%s</td></tr>\n", _("/&lt;Problem&gt;/&lt;User_Id&gt;/&lt;File&gt;"));
  fprintf(fout, "<tr><td><input type=\"radio\" name=\"dir_struct\" value=\"5\"/></td><td>%s</td></tr>\n", _("/&lt;Problem&gt;/&lt;User_Login&gt;/&lt;File&gt;"));
  fprintf(fout, "<tr><td><input type=\"radio\" name=\"dir_struct\" value=\"9\"/></td><td>%s</td></tr>\n", _("/&lt;Problem&gt;/&lt;User_Name&gt;/&lt;File&gt;"));
  fprintf(fout, "<tr><td><input type=\"radio\" name=\"dir_struct\" value=\"6\"/></td><td>%s</td></tr>\n", _("/&lt;User_Id&gt;/&lt;Problem&gt;/&lt;File&gt;"));
  fprintf(fout, "<tr><td><input type=\"radio\" name=\"dir_struct\" value=\"7\"/></td><td>%s</td></tr>\n", _("/&lt;User_Login&gt;/&lt;Problem&gt;/&lt;File&gt;"));
  fprintf(fout, "<tr><td><input type=\"radio\" name=\"dir_struct\" value=\"10\"/></td><td>%s</td></tr>\n", _("/&lt;User_Name&gt;/&lt;Problem&gt;/&lt;File&gt;"));
  fprintf(fout, "</table>\n");

  fprintf(fout, "<h2>%s</h2>\n", _("Download runs"));
  fprintf(fout, "<table>");
  fprintf(fout, "<tr><td>%s</td></tr>",
          BUTTON(NEW_SRV_ACTION_DOWNLOAD_ARCHIVE_2));
  fprintf(fout, "<tr><td>%s%s</a></td></tr></table>",
          ns_aref(bb, sizeof(bb), phr, NEW_SRV_ACTION_MAIN_PAGE, 0),
          _("Main page"));
  fprintf(fout, "</form>\n");

  ns_footer(fout, extra->footer_txt, extra->copyright_txt, phr->locale_id);
  l10n_setlocale(0);

 cleanup:
  xfree(mask);
  return retval;

 invalid_param:
  ns_html_err_inv_param(fout, phr, 0, 0);
  xfree(mask);
  return -1;
}

static int
priv_download_runs(
        FILE *fout,
        FILE *log_f,
        struct http_request_info *phr,
        const struct contest_desc *cnts,
        struct contest_extra *extra)
{
  const serve_state_t cs = extra->serve_state;
  int retval = 0;
  unsigned long *mask = 0;
  size_t mask_size = 0;
  int x;
  int dir_struct = 0;
  int run_selection = 0;
  int file_name_mask = 0;
  const unsigned char *s;
  char *ss = 0;

  if (opcaps_check(phr->caps, OPCAP_DUMP_RUNS) < 0)
    FAIL(NEW_SRV_ERR_PERMISSION_DENIED);

  // run_selection
  // dir_struct
  // file_pattern_run
  // file_pattern_uid
  // file_pattern_login
  // file_pattern_name
  // file_pattern_prob
  // file_pattern_lang
  // file_pattern_suffix
  if (ns_cgi_param(phr, "run_selection", &s) <= 0)
    FAIL(NEW_SRV_ERR_INV_RUN_SELECTION);
  errno = 0;
  x = strtol(s, &ss, 10);
  if (errno || *ss || x < 0 || x > 2) FAIL(NEW_SRV_ERR_INV_RUN_SELECTION);
  run_selection = x;

  if (ns_cgi_param(phr, "dir_struct", &s) <= 0)
    FAIL(NEW_SRV_ERR_INV_DIR_STRUCT);
  errno = 0;
  x = strtol(s, &ss, 10);
  if (errno || *ss || x < 0 || x > 10) FAIL(NEW_SRV_ERR_INV_DIR_STRUCT);
  dir_struct = x;

  if (ns_cgi_param(phr, "file_pattern_run", &s) > 0)
    file_name_mask |= NS_FILE_PATTERN_RUN;
  if (ns_cgi_param(phr, "file_pattern_uid", &s) > 0)
    file_name_mask |= NS_FILE_PATTERN_UID;
  if (ns_cgi_param(phr, "file_pattern_login", &s) > 0)
    file_name_mask |= NS_FILE_PATTERN_LOGIN;
  if (ns_cgi_param(phr, "file_pattern_name", &s) > 0)
    file_name_mask |= NS_FILE_PATTERN_NAME;
  if (ns_cgi_param(phr, "file_pattern_prob", &s) > 0)
    file_name_mask |= NS_FILE_PATTERN_PROB;
  if (ns_cgi_param(phr, "file_pattern_lang", &s) > 0)
    file_name_mask |= NS_FILE_PATTERN_LANG;
  if (ns_cgi_param(phr, "file_pattern_suffix", &s) > 0)
    file_name_mask |= NS_FILE_PATTERN_SUFFIX;
  if (!file_name_mask) file_name_mask = NS_FILE_PATTERN_RUN;

  if (parse_run_mask(phr, 0, 0, &mask_size, &mask) < 0)
    goto invalid_param;

  ns_download_runs(cs, fout, log_f, run_selection, dir_struct, file_name_mask,
                   mask_size, mask);

 cleanup:
  return retval;

 invalid_param:
  ns_html_err_inv_param(fout, phr, 0, 0);
  xfree(mask);
  return -1;
}

static int
priv_upsolving_configuration_1(
        FILE *fout,
        FILE *log_f,
        struct http_request_info *phr,
        const struct contest_desc *cnts,
        struct contest_extra *extra)
{
  const serve_state_t cs = extra->serve_state;
  int retval = 0;
  unsigned char bb[1024];
  const unsigned char *freeze_standings = 0;
  const unsigned char *view_source = 0;
  const unsigned char *view_protocol = 0;
  const unsigned char *full_proto = 0;
  const unsigned char *disable_clars = 0;

  if (opcaps_check(phr->caps, OPCAP_CONTROL_CONTEST) < 0)
    FAIL(NEW_SRV_ERR_PERMISSION_DENIED);  

  if (cs->upsolving_mode) {
    ns_cgi_param(phr, "freeze_standings", &freeze_standings);
    ns_cgi_param(phr, "view_source", &view_source);
    ns_cgi_param(phr, "view_protocol", &view_protocol);
    ns_cgi_param(phr, "full_protocol", &full_proto);
    ns_cgi_param(phr, "disable_clars", &disable_clars);
  } else {
    freeze_standings = "1";
    view_source = "1";
    view_protocol = "1";
    full_proto = 0;
    disable_clars = "1";
  }

  l10n_setlocale(phr->locale_id);
  ns_header(fout, extra->header_txt, 0, 0, 0, 0, phr->locale_id, cnts,
            phr->client_key,
            "%s [%s, %d, %s]: %s", ns_unparse_role(phr->role), phr->name_arm,
            phr->contest_id, extra->contest_arm, "Upsolving configuration");

  html_start_form(fout, 1, phr->self_url, phr->hidden_vars);
  fprintf(fout, "<table>");
  fprintf(fout, "<tr><td>%s</td><td>%s</td></tr>\n",
          html_checkbox(bb, sizeof(bb), "freeze_standings", NULL,
                        freeze_standings?1:0, 0),
          _("Freeze contest standings"));
  fprintf(fout, "<tr><td>%s</td><td>%s</td></tr>\n",
          html_checkbox(bb, sizeof(bb), "view_source", NULL,
                        view_source?1:0, 0),
          _("Allow viewing source code"));
  fprintf(fout, "<tr><td>%s</td><td>%s</td></tr>\n",
          html_checkbox(bb, sizeof(bb), "view_protocol", NULL,
                        view_protocol?1:0, 0),
          _("Allow viewing run report"));
  fprintf(fout, "<tr><td>%s</td><td>%s</td></tr>\n",
          html_checkbox(bb, sizeof(bb), "full_protocol", NULL,
                        full_proto?1:0, 0),
          _("Allow viewing full protocol"));
  fprintf(fout, "<tr><td>%s</td><td>%s</td></tr>\n",
          html_checkbox(bb, sizeof(bb), "disable_clars", NULL,
                        disable_clars?1:0, 0),
          _("Disable clarifications"));
  fprintf(fout, "</table>\n");

  fprintf(fout, "<table><tr>");
  fprintf(fout, "<td>%s</td>",
          BUTTON(NEW_SRV_ACTION_UPSOLVING_CONFIG_2));
  fprintf(fout, "<td>%s</td>",
          BUTTON(NEW_SRV_ACTION_UPSOLVING_CONFIG_3));
  fprintf(fout, "<td>%s</td>",
          BUTTON(NEW_SRV_ACTION_UPSOLVING_CONFIG_4));
  fprintf(fout, "</tr></table>\n");

  fprintf(fout, "</form>\n");

  ns_footer(fout, extra->footer_txt, extra->copyright_txt, phr->locale_id);
  l10n_setlocale(0);

 cleanup:
  return retval;
}

static int
priv_upsolving_operation(
        FILE *fout,
        FILE *log_f,
        struct http_request_info *phr,
        const struct contest_desc *cnts,
        struct contest_extra *extra)
{
  const serve_state_t cs = extra->serve_state;
  int retval = 0;
  const unsigned char *freeze_standings = 0;
  const unsigned char *view_source = 0;
  const unsigned char *view_protocol = 0;
  const unsigned char *full_proto = 0;
  const unsigned char *disable_clars = 0;
  time_t duration = 0, saved_stop_time = 0, stop_time = 0;

  if (opcaps_check(phr->caps, OPCAP_CONTROL_CONTEST) < 0)
    FAIL(NEW_SRV_ERR_PERMISSION_DENIED);

  /* check that the contest is stopped */
  run_get_saved_times(cs->runlog_state, &duration, &saved_stop_time, 0);
  stop_time = run_get_stop_time(cs->runlog_state);
  if (stop_time <= 0 && saved_stop_time <= 0) return 0;

  ns_cgi_param(phr, "freeze_standings", &freeze_standings);
  ns_cgi_param(phr, "view_source", &view_source);
  ns_cgi_param(phr, "view_protocol", &view_protocol);
  ns_cgi_param(phr, "full_protocol", &full_proto);
  ns_cgi_param(phr, "disable_clars", &disable_clars);

  switch (phr->action) {
  case NEW_SRV_ACTION_UPSOLVING_CONFIG_2: // back to main page
    break;
  case NEW_SRV_ACTION_UPSOLVING_CONFIG_3: // stop upsolving
    if (!cs->upsolving_mode) break;
    run_stop_contest(cs->runlog_state, cs->current_time);
    serve_invoke_stop_script(cs);
    cs->upsolving_mode = 0;
    cs->upsolving_freeze_standings = 0;
    cs->upsolving_view_source = 0;
    cs->upsolving_view_protocol = 0;
    cs->upsolving_full_protocol = 0;
    cs->upsolving_disable_clars = 0;
    serve_update_status_file(cs, 1);
    extra->last_access_time = 0;          // force reload
    break;
  case NEW_SRV_ACTION_UPSOLVING_CONFIG_4: // start upsolving
    run_save_times(cs->runlog_state);
    run_set_duration(cs->runlog_state, 0);
    run_stop_contest(cs->runlog_state, 0);
    run_set_finish_time(cs->runlog_state, 0);
    cs->upsolving_mode = 1;
    cs->upsolving_freeze_standings = 0;
    cs->upsolving_view_source = 0;
    cs->upsolving_view_protocol = 0;
    cs->upsolving_full_protocol = 0;
    cs->upsolving_disable_clars = 0;
    if (freeze_standings && *freeze_standings) cs->upsolving_freeze_standings = 1;
    if (view_source && *view_source) cs->upsolving_view_source = 1;
    if (view_protocol && *view_protocol) cs->upsolving_view_protocol = 1;
    if (full_proto && *full_proto) cs->upsolving_full_protocol = 1;
    if (disable_clars && *disable_clars) cs->upsolving_disable_clars = 1;
    serve_update_status_file(cs, 1);
    extra->last_access_time = 0;          // force reload
    break;
  default:
    abort();
  }

 cleanup:
  return retval;
}

static int
priv_assign_cyphers_1(
        FILE *fout,
        FILE *log_f,
        struct http_request_info *phr,
        const struct contest_desc *cnts,
        struct contest_extra *extra)
{
  int retval = 0;
  unsigned char bb[1024];

  if (opcaps_check(phr->caps, OPCAP_CONTROL_CONTEST) < 0)
    FAIL(NEW_SRV_ERR_PERMISSION_DENIED);  

  l10n_setlocale(phr->locale_id);
  ns_header(fout, extra->header_txt, 0, 0, 0, 0, phr->locale_id, cnts,
            phr->client_key,
            "%s [%s, %d, %s]: %s", ns_unparse_role(phr->role), phr->name_arm,
            phr->contest_id, extra->contest_arm, "Assign cyphers");
  html_start_form(fout, 1, phr->self_url, phr->hidden_vars);
  fprintf(fout, "<table>\n");

  fprintf(fout, "<tr><td>%s</td><td>%s</td></tr>\n",
          html_input_text(bb, sizeof(bb), "prefix", 16, 0, 0),
          _("Cypher prefix"));
  fprintf(fout, "<tr><td>%s</td><td>%s</td></tr>\n",
          html_input_text(bb, sizeof(bb), "min_num", 16, 0, 0),
          _("Minimal random number"));
  fprintf(fout, "<tr><td>%s</td><td>%s</td></tr>\n",
          html_input_text(bb, sizeof(bb), "max_num", 16, 0, 0),
          _("Maximal random number"));
  fprintf(fout, "<tr><td>%s</td><td>%s</td></tr>\n",
          html_input_text(bb, sizeof(bb), "seed", 16, 0, 0),
          _("Random seed"));
  fprintf(fout, "<tr><td>%s</td><td>%s</td></tr>\n",
          html_input_text(bb, sizeof(bb), "mult", 16, 0, 0),
          _("Mult parameter"));
  fprintf(fout, "<tr><td>%s</td><td>%s</td></tr>\n",
          html_input_text(bb, sizeof(bb), "shift", 16, 0, 0),
          _("Shift parameter"));
  fprintf(fout, "<tr><td>%s</td><td>&nbsp;</td></tr>\n",
          BUTTON(NEW_SRV_ACTION_ASSIGN_CYPHERS_2));

  fprintf(fout, "</table>\n");
  fprintf(fout, "</form>\n");
  fprintf(fout, "<p>The following formula is applied: mult * X + shift.</p>\n");
  ns_footer(fout, extra->footer_txt, extra->copyright_txt, phr->locale_id);
  l10n_setlocale(0);

 cleanup:
  return retval;
}

static int
priv_assign_cyphers_2(
        FILE *fout,
        FILE *log_f,
        struct http_request_info *phr,
        const struct contest_desc *cnts,
        struct contest_extra *extra)
{
  serve_state_t cs = extra->serve_state;
  //const struct section_global_data *global = cs->global;
  int retval = 0;
  const unsigned char *prefix = 0;
  int min_num = 0, max_num = 0, seed = 0, total_users = 0, user_count, user_id;
  int max_user_id, i, j, r;
  int mult = 1, shift = 0;
  int *user_ids = 0, *rand_map = 0, *user_cyphers = 0;
  char *msg_txt = 0;
  size_t msg_len = 0;
  FILE *msg_f = 0;
  unsigned char **user_logins = 0;
  struct html_armor_buffer ab = HTML_ARMOR_INITIALIZER;
  unsigned char *csv_reply = 0;

  if (cs->global->disable_user_database > 0)
    FAIL(NEW_SRV_ERR_INV_ACTION);

  if (phr->role < USER_ROLE_ADMIN)
    FAIL(NEW_SRV_ERR_PERMISSION_DENIED);
  if (opcaps_check(phr->caps, OPCAP_EDIT_REG) < 0)
    FAIL(NEW_SRV_ERR_PERMISSION_DENIED);

  if (ns_cgi_param(phr, "prefix", &prefix) <= 0)
    FAIL(NEW_SRV_ERR_INV_PARAM);
  if (ns_cgi_param_int(phr, "min_num", &min_num) < 0)
    FAIL(NEW_SRV_ERR_INV_PARAM);
  if (ns_cgi_param_int(phr, "max_num", &max_num) < 0)
    FAIL(NEW_SRV_ERR_INV_PARAM);
  if (ns_cgi_param_int(phr, "seed", &seed) < 0)
    FAIL(NEW_SRV_ERR_INV_PARAM);
  if (ns_cgi_param_int_opt(phr, "mult", &mult, 1) < 0)
    FAIL(NEW_SRV_ERR_INV_PARAM);
  if (ns_cgi_param_int_opt(phr, "shift", &shift, 1) < 0)
    FAIL(NEW_SRV_ERR_INV_PARAM);
  if (min_num < 0 || max_num < 0 || min_num > max_num || seed < 0)
    FAIL(NEW_SRV_ERR_INV_PARAM);

  total_users = teamdb_get_total_teams(cs->teamdb_state);
  if (total_users >= max_num - min_num)
    FAIL(NEW_SRV_ERR_INV_PARAM);
  XCALLOC(user_ids, total_users + 2);
  XCALLOC(user_cyphers, total_users + 2);
  XCALLOC(user_logins, total_users + 2);
  max_user_id = teamdb_get_max_team_id(cs->teamdb_state);
  for (user_id = 1, user_count = 0;
       user_id <= max_user_id && user_count <= total_users; user_id++) {
    if (teamdb_lookup(cs->teamdb_state, user_id) <= 0) continue;
    if (teamdb_get_flags(cs->teamdb_state, user_id) != 0) continue;
    user_logins[user_count] = xstrdup(teamdb_get_login(cs->teamdb_state, user_id));
    user_ids[user_count++] = user_id;
  }

  if (!seed) {
    struct timeval tv;
    gettimeofday(&tv, 0);
    seed = (tv.tv_sec ^ tv.tv_usec) & INT_MAX;
    if (!seed) seed = tv.tv_sec;
  }
  srand(seed);
  XCALLOC(rand_map, max_num - min_num + 1);

  for (i = 0; i < user_count; i++) {
    do {
      j = min_num + (int)((rand() / (RAND_MAX + 1.0)) * (max_num - min_num + 1));
    } while (rand_map[j - min_num]);
    rand_map[j - min_num] = user_ids[i];
    user_cyphers[i] = j;
  }

  if (!prefix) prefix = "";
  msg_f = open_memstream(&msg_txt, &msg_len);
  fprintf(msg_f, "Login;Exam_Cypher\n");
  for (i = 0; i < user_count; i++) {
    fprintf(msg_f, "%s;%s%d\n", user_logins[i], prefix,
            mult * user_cyphers[i] + shift);
  }
  close_memstream(msg_f); msg_f = 0;

  if (ns_open_ul_connection(phr->fw_state) < 0)
    FAIL(NEW_SRV_ERR_TRY_AGAIN);
  r = userlist_clnt_import_csv_users(ul_conn, ULS_IMPORT_CSV_USERS,
                                     phr->contest_id, ';', 0, msg_txt,
                                     &csv_reply);
  if (r < 0) FAIL(NEW_SRV_ERR_INTERNAL);

  l10n_setlocale(phr->locale_id);
  ns_header(fout, extra->header_txt, 0, 0, 0, 0, phr->locale_id, cnts,
            phr->client_key,
            "%s [%s, %d, %s]: %s", ns_unparse_role(phr->role),
            phr->name_arm, phr->contest_id, extra->contest_arm,
            _("Assigned cyphers"));

  fprintf(fout, "<table class=\"b1\">\n");
  fprintf(fout, "<tr><td class=\"b1\">NN</td><td class=\"b1\">%s</td><td class=\"b1\">%s</td><td class=\"b1\">%s</td></tr>\n",
          _("User Id"), _("Login"), _("Cypher"));
  for (i = 0; i < user_count; i++) {
    fprintf(fout, "<tr><td class=\"b1\">%d</td><td class=\"b1\">%d</td><td class=\"b1\">%s</td>",
            i + 1, user_ids[i], ARMOR(user_logins[i]));
    fprintf(fout, "<td class=\"b1\">%s%d</td></tr>\n",
            ARMOR(prefix), mult * user_cyphers[i] + shift);
  }
  fprintf(fout, "</table>\n");

  if (csv_reply && *csv_reply) {
    fprintf(fout, "<h2>Operation status</h2>\n");
    fprintf(fout, "<pre>%s</pre>\n", ARMOR(csv_reply));
  }

  ns_footer(fout, extra->footer_txt, extra->copyright_txt, phr->locale_id);
  l10n_setlocale(0);

 cleanup:
  xfree(csv_reply);
  if (user_logins) {
    for (i = 0; i < total_users + 2; i++)
      xfree(user_logins[i]);
    xfree(user_logins);
  }
  xfree(msg_txt);
  if (msg_f) fclose(msg_f);
  xfree(rand_map);
  xfree(user_cyphers);
  xfree(user_ids);
  html_armor_free(&ab);
  return retval;
}

static int
priv_set_priorities(
        FILE *fout,
        FILE *log_f,
        struct http_request_info *phr,
        const struct contest_desc *cnts,
        struct contest_extra *extra)
{
  serve_state_t cs = extra->serve_state;
  //const struct section_global_data *global = cs->global;
  const struct section_problem_data *prob;
  int retval = 0;
  int prob_id, prio;
  unsigned char varname[64];

  if (phr->role != USER_ROLE_ADMIN)
    FAIL(NEW_SRV_ERR_PERMISSION_DENIED);
  if (opcaps_check(phr->caps, OPCAP_REJUDGE_RUN) < 0)
    FAIL(NEW_SRV_ERR_PERMISSION_DENIED);

  for (prob_id = 1;
       prob_id <= cs->max_prob && prob_id < EJ_SERVE_STATE_TOTAL_PROBS;
       ++prob_id) {
    if (!(prob = cs->probs[prob_id])) continue;
    snprintf(varname, sizeof(varname), "prio_%d", prob_id);
    prio = 0;
    if (ns_cgi_param_int(phr, varname, &prio) < 0) continue;
    if (prio < -16) prio = -16;
    if (prio > 15) prio = 15;
    cs->prob_prio[prob_id] = prio;
  }

 cleanup:
  return retval;
}

static int
priv_view_passwords(FILE *fout,
                    FILE *log_f,
                    struct http_request_info *phr,
                    const struct contest_desc *cnts,
                    struct contest_extra *extra)
{
  int retval = 0;
  const unsigned char *s = 0;

  if (phr->role < USER_ROLE_JUDGE
      || opcaps_check(phr->caps, OPCAP_EDIT_PASSWD) < 0)
    FAIL(NEW_SRV_ERR_PERMISSION_DENIED);
  if (phr->action == NEW_SRV_ACTION_VIEW_CNTS_PWDS
      && cnts->disable_team_password)
    FAIL(NEW_SRV_ERR_TEAM_PWD_DISABLED);

  l10n_setlocale(phr->locale_id);
  if (phr->action == NEW_SRV_ACTION_VIEW_CNTS_PWDS) {
    s = _("Contest passwords");
  } else {
    s = _("Registration passwords");
  }
  ns_header(fout, extra->header_txt, 0, 0, 0, 0, phr->locale_id, cnts,
            phr->client_key,
            "%s [%s, %d, %s]: %s", ns_unparse_role(phr->role),
            phr->name_arm, phr->contest_id, extra->contest_arm, s);

  ns_write_passwords(fout, log_f, phr, cnts, extra);

  ns_footer(fout, extra->footer_txt, extra->copyright_txt, phr->locale_id);
  l10n_setlocale(0);

 cleanup:
  return retval;
}

static int
priv_view_online_users(
        FILE *fout,
        FILE *log_f,
        struct http_request_info *phr,
        const struct contest_desc *cnts,
        struct contest_extra *extra)
{
  int retval = 0;

  if (phr->role < USER_ROLE_JUDGE) FAIL(NEW_SRV_ERR_PERMISSION_DENIED);

  l10n_setlocale(phr->locale_id);
  ns_header(fout, extra->header_txt, 0, 0, 0, 0, phr->locale_id, cnts,
            phr->client_key,
            "%s [%s, %d, %s]: %s", ns_unparse_role(phr->role),
            phr->name_arm, phr->contest_id, extra->contest_arm,
            _("Online users"));
  ns_write_online_users(fout, log_f, phr, cnts, extra);
  ns_footer(fout, extra->footer_txt, extra->copyright_txt, phr->locale_id);
  l10n_setlocale(0);

 cleanup:
  return retval;
}

static int
priv_view_user_ips(
        FILE *fout,
        FILE *log_f,
        struct http_request_info *phr,
        const struct contest_desc *cnts,
        struct contest_extra *extra)
{
  int retval = 0;

  if (phr->role < USER_ROLE_JUDGE) FAIL(NEW_SRV_ERR_PERMISSION_DENIED);

  l10n_setlocale(phr->locale_id);
  ns_header(fout, extra->header_txt, 0, 0, 0, 0, phr->locale_id, cnts,
            phr->client_key,
            "%s [%s, %d, %s]: %s", ns_unparse_role(phr->role),
            phr->name_arm, phr->contest_id, extra->contest_arm,
            _("IP addresses for users"));
  ns_write_user_ips(fout, log_f, phr, cnts, extra);
  ns_footer(fout, extra->footer_txt, extra->copyright_txt, phr->locale_id);
  l10n_setlocale(0);

 cleanup:
  return retval;
}

static int
priv_view_ip_users(
        FILE *fout,
        FILE *log_f,
        struct http_request_info *phr,
        const struct contest_desc *cnts,
        struct contest_extra *extra)
{
  int retval = 0;

  if (phr->role < USER_ROLE_JUDGE) FAIL(NEW_SRV_ERR_PERMISSION_DENIED);

  l10n_setlocale(phr->locale_id);
  ns_header(fout, extra->header_txt, 0, 0, 0, 0, phr->locale_id, cnts,
            phr->client_key,
            "%s [%s, %d, %s]: %s", ns_unparse_role(phr->role),
            phr->name_arm, phr->contest_id, extra->contest_arm,
            _("Users for IP addresses"));
  ns_write_ip_users(fout, log_f, phr, cnts, extra);
  ns_footer(fout, extra->footer_txt, extra->copyright_txt, phr->locale_id);
  l10n_setlocale(0);

 cleanup:
  return retval;
}

static int
priv_view_exam_info(
        FILE *fout,
        FILE *log_f,
        struct http_request_info *phr,
        const struct contest_desc *cnts,
        struct contest_extra *extra)
{
  int retval = 0;

  if (phr->role < USER_ROLE_JUDGE) FAIL(NEW_SRV_ERR_PERMISSION_DENIED);

  l10n_setlocale(phr->locale_id);
  ns_header(fout, extra->header_txt, 0, 0, 0, 0, phr->locale_id, cnts,
            phr->client_key,
            "%s [%s, %d, %s]: %s", ns_unparse_role(phr->role),
            phr->name_arm, phr->contest_id, extra->contest_arm,
            _("Examination information"));
  ns_write_exam_info(fout, log_f, phr, cnts, extra);
  ns_footer(fout, extra->footer_txt, extra->copyright_txt, phr->locale_id);
  l10n_setlocale(0);

 cleanup:
  return retval;
}

static int
priv_priority_form(
        FILE *fout,
        FILE *log_f,
        struct http_request_info *phr,
        const struct contest_desc *cnts,
        struct contest_extra *extra)
{
  int retval = 0;

  if (phr->role != USER_ROLE_ADMIN) FAIL(NEW_SRV_ERR_PERMISSION_DENIED);

  l10n_setlocale(phr->locale_id);
  ns_header(fout, extra->header_txt, 0, 0, 0, 0, phr->locale_id, cnts,
            phr->client_key,
            "%s [%s, %d, %s]: %s", ns_unparse_role(phr->role),
            phr->name_arm, phr->contest_id, extra->contest_arm,
            _("Judging priorities"));
  ns_write_judging_priorities(fout, log_f, phr, cnts, extra);
  ns_footer(fout, extra->footer_txt, extra->copyright_txt, phr->locale_id);
  l10n_setlocale(0);

 cleanup:
  return retval;
}

static int
priv_view_testing_queue(
        FILE *fout,
        FILE *log_f,
        struct http_request_info *phr,
        const struct contest_desc *cnts,
        struct contest_extra *extra)
{
  int retval = 0;

  if (phr->role != USER_ROLE_ADMIN) FAIL(NEW_SRV_ERR_PERMISSION_DENIED);

  l10n_setlocale(phr->locale_id);
  ns_header(fout, extra->header_txt, 0, 0, 0, 0, phr->locale_id, cnts,
            phr->client_key,
            "%s [%s, %d, %s]: %s", ns_unparse_role(phr->role),
            phr->name_arm, phr->contest_id, extra->contest_arm,
            _("Testing queue"));
  ns_write_testing_queue(fout, log_f, phr, cnts, extra);
  ns_footer(fout, extra->footer_txt, extra->copyright_txt, phr->locale_id);
  l10n_setlocale(0);

 cleanup:
  return retval;
}

static int
priv_testing_queue_operation(
        FILE *fout,
        FILE *log_f,
        struct http_request_info *phr,
        const struct contest_desc *cnts,
        struct contest_extra *extra)
{
  int retval = 0;
  const unsigned char *packet_name = 0, *s;
  const serve_state_t cs = extra->serve_state;

  if (opcaps_check(phr->caps, OPCAP_CONTROL_CONTEST) < 0)
    FAIL(NEW_SRV_ERR_PERMISSION_DENIED);
  if (ns_cgi_param(phr, "packet", &packet_name) <= 0 || !packet_name)
    FAIL(NEW_SRV_ERR_INV_PARAM);
  for (s = packet_name; *s; ++s) {
    if (!isalnum(*s)) {
      FAIL(NEW_SRV_ERR_INV_PARAM);
    }
  }

  switch (phr->action) {
  case NEW_SRV_ACTION_TESTING_DELETE:
    serve_testing_queue_delete(cnts, cs, packet_name, phr->login);
    break;
  case NEW_SRV_ACTION_TESTING_UP:
    serve_testing_queue_change_priority(cnts, cs, packet_name, -1, phr->login);
    break;
  case NEW_SRV_ACTION_TESTING_DOWN:
    serve_testing_queue_change_priority(cnts, cs, packet_name, 1, phr->login);
    break;
  default:
    FAIL(NEW_SRV_ERR_INV_PARAM);
  }

 cleanup:
  return retval;
}

static int
priv_whole_testing_queue_operation(
        FILE *fout,
        FILE *log_f,
        struct http_request_info *phr,
        const struct contest_desc *cnts,
        struct contest_extra *extra)
{
  int retval = 0;
  const serve_state_t cs = extra->serve_state;

  if (opcaps_check(phr->caps, OPCAP_CONTROL_CONTEST) < 0)
    FAIL(NEW_SRV_ERR_PERMISSION_DENIED);

  switch (phr->action) {
  case NEW_SRV_ACTION_TESTING_DELETE_ALL:
    serve_testing_queue_delete_all(cnts, cs, phr->login);
    break;
  case NEW_SRV_ACTION_TESTING_UP_ALL:
    serve_testing_queue_change_priority_all(cnts, cs, -1, phr->login);
    break;
  case NEW_SRV_ACTION_TESTING_DOWN_ALL:
    serve_testing_queue_change_priority_all(cnts, cs, 1, phr->login);
    break;
  default:
    FAIL(NEW_SRV_ERR_INV_PARAM);
  }

 cleanup:
  return retval;
}

static int
priv_stand_filter_operation(
        FILE *fout,
        FILE *log_f,
        struct http_request_info *phr,
        const struct contest_desc *cnts,
        struct contest_extra *extra)
{
  int retval = 0;
  const serve_state_t cs = extra->serve_state;

  /*
  if (opcaps_check(phr->caps, OPCAP_CONTROL_CONTEST) < 0)
    FAIL(NEW_SRV_ERR_PERMISSION_DENIED);
  */

  switch (phr->action) {
  case NEW_SRV_ACTION_SET_STAND_FILTER:
    ns_set_stand_filter(cs, phr);
    break;
  case NEW_SRV_ACTION_RESET_STAND_FILTER:
    ns_reset_stand_filter(cs, phr);
    break;
  default:
    FAIL(NEW_SRV_ERR_INV_PARAM);
  }

 cleanup:
  return retval;
}

static int
priv_change_run_fields(
        FILE *fout,
        FILE *log_f,
        struct http_request_info *phr,
        const struct contest_desc *cnts,
        struct contest_extra *extra)
{
  int retval = 0;
  const serve_state_t cs = extra->serve_state;

  if (phr->role <= 0) FAIL(NEW_SRV_ERR_PERMISSION_DENIED);

  const unsigned char *s = NULL;
  if (ns_cgi_param(phr, "cancel", &s) > 0 && s) goto cleanup;

  struct user_filter_info *u = user_filter_info_allocate(cs, phr->user_id, phr->session_id);
  if (!u) goto cleanup;

  if (ns_cgi_param(phr, "reset", &s) > 0 && s) {
    if (u->run_fields <= 0) goto cleanup;
    u->run_fields = 0;
    team_extra_set_run_fields(cs->team_extra_state, phr->user_id, 0);
    team_extra_flush(cs->team_extra_state);
    goto cleanup;
  }

  int new_fields = 0;
  for (int i = 0; i < RUN_VIEW_LAST; ++i) {
    unsigned char nbuf[64];
    snprintf(nbuf, sizeof(nbuf), "field_%d", i);
    if (ns_cgi_param(phr, nbuf, &s) > 0 && s) {
      new_fields |= 1 << i;
    }
  }
  if (new_fields == u->run_fields) goto cleanup;
  u->run_fields = new_fields;
  team_extra_set_run_fields(cs->team_extra_state, phr->user_id, u->run_fields);
  team_extra_flush(cs->team_extra_state);

cleanup:
  return retval;
}

static int
priv_admin_contest_settings(
        FILE *fout,
        FILE *log_f,
        struct http_request_info *phr,
        const struct contest_desc *cnts,
        struct contest_extra *extra)
{
  int retval = 0;

  if (phr->role != USER_ROLE_ADMIN) FAIL(NEW_SRV_ERR_PERMISSION_DENIED);

  l10n_setlocale(phr->locale_id);
  ns_header(fout, extra->header_txt, 0, 0, 0, 0, phr->locale_id, cnts,
            phr->client_key,
            "%s [%s, %d, %s]: %s", ns_unparse_role(phr->role),
            phr->name_arm, phr->contest_id, extra->contest_arm,
            _("Contest settings"));
  ns_write_admin_contest_settings(fout, log_f, phr, cnts, extra);
  ns_footer(fout, extra->footer_txt, extra->copyright_txt, phr->locale_id);
  l10n_setlocale(0);

 cleanup:
  return retval;
}

static int
priv_print_user_exam_protocol(
        FILE *fout,
        FILE *log_f,
        struct http_request_info *phr,
        const struct contest_desc *cnts,
        struct contest_extra *extra)
{
  const serve_state_t cs = extra->serve_state;
  int retval = 0, user_id, r;
  char *log_text = 0;
  size_t log_size = 0;
  FILE *ff = 0;
  unsigned char bb[1024];
  unsigned char *ss = 0;
  int locale_id = 0;
  int use_user_printer = 0;
  int full_report = 0;
  int use_cypher = 0;

  if (opcaps_check(phr->caps, OPCAP_PRINT_RUN) < 0)
    FAIL(NEW_SRV_ERR_PERMISSION_DENIED);
  if (ns_cgi_param_int(phr, "user_id", &user_id) < 0)
    FAIL(NEW_SRV_ERR_INV_USER_ID);
  if (!teamdb_lookup(cs->teamdb_state, user_id))
    FAIL(NEW_SRV_ERR_INV_USER_ID);

  if (phr->action == NEW_SRV_ACTION_PRINT_UFC_PROTOCOL) {
    full_report = 1;
    use_cypher = 1;
  } else if (phr->action == NEW_SRV_ACTION_PRINT_USER_FULL_PROTOCOL) {
    full_report = 1;
  } else {
    use_user_printer = 1;
  }

  if (cnts->default_locale_num > 0) locale_id = cnts->default_locale_num;
  if (locale_id > 0) l10n_setlocale(locale_id);
  ff = open_memstream(&log_text, &log_size);
  r = ns_print_user_exam_protocol(cnts, cs, ff, user_id, locale_id,
                                  use_user_printer, full_report, use_cypher);
  close_memstream(ff); ff = 0;
  if (locale_id > 0) l10n_setlocale(0);

  l10n_setlocale(phr->locale_id);
  ns_header(fout, extra->header_txt, 0, 0, 0, 0, phr->locale_id, cnts,
            phr->client_key,
            "%s [%s, %d, %s]: %s", ns_unparse_role(phr->role), phr->name_arm,
            phr->contest_id, extra->contest_arm, _("Printing user protocol"));

  fprintf(fout, "<h2>%s</h2>\n",
          (r >= 0)?_("Operation succeeded"):_("Operation failed"));

  fprintf(fout, "<table>");
  fprintf(fout, "<tr><td>%s%s</a></td></tr></table>",
          ns_aref(bb, sizeof(bb), phr, NEW_SRV_ACTION_MAIN_PAGE, 0),
          _("Main page"));

  ss = html_armor_string_dup(log_text);
  fprintf(fout, "<hr/><pre>");
  if (r < 0) fprintf(fout, "<font color=\"red\">");
  fprintf(fout, "%s", ss);
  if (r < 0) fprintf(fout, "</font>");
  fprintf(fout, "</pre>\n");
  xfree(ss); ss = 0;
  xfree(log_text); log_text = 0;

  ns_footer(fout, extra->footer_txt, extra->copyright_txt, phr->locale_id);
  l10n_setlocale(0);

 cleanup:
  if (ff) fclose(ff);
  xfree(ss);
  xfree(log_text);
  return retval;
}

static int
priv_print_users_exam_protocol(
        FILE *fout,
        FILE *log_f,
        struct http_request_info *phr,
        const struct contest_desc *cnts,
        struct contest_extra *extra)
{
  const serve_state_t cs = extra->serve_state;
  int retval = 0, r;
  char *log_text = 0;
  size_t log_size = 0;
  FILE *ff = 0;
  unsigned char bb[1024];
  unsigned char *ss = 0;
  int locale_id = 0, i, x, n;
  intarray_t uset;
  const unsigned char *s;
  int use_user_printer = 0;
  int full_report = 0;
  int use_cypher = 0;
  int first_user_id = 0, last_user_id = -1;

  if (opcaps_check(phr->caps, OPCAP_PRINT_RUN) < 0)
    FAIL(NEW_SRV_ERR_PERMISSION_DENIED);

  memset(&uset, 0, sizeof(uset));
  for (i = 0; i < phr->param_num; i++) {
    if (strncmp(phr->param_names[i], "user_", 5) != 0) continue;
    if (sscanf((s = phr->param_names[i] + 5), "%d%n", &x, &n) != 1
        || s[n] || x <= 0)
      FAIL(NEW_SRV_ERR_INV_USER_ID);
    if (teamdb_lookup(cs->teamdb_state, x) <= 0)
      FAIL(NEW_SRV_ERR_INV_USER_ID);

    XEXPAND2(uset);
    uset.v[uset.u++] = x;
  }

  priv_parse_user_id_range(phr, &first_user_id, &last_user_id);
  if (first_user_id > 0) {
    for (i = first_user_id; i <= last_user_id; i++) {
      XEXPAND2(uset);
      uset.v[uset.u++] = i;
    }
  }

  if (phr->action == NEW_SRV_ACTION_PRINT_SELECTED_UFC_PROTOCOL) {
    full_report = 1;
    use_cypher = 1;
  } else if (phr->action == NEW_SRV_ACTION_PRINT_SELECTED_USER_FULL_PROTOCOL) {
    full_report = 1;
  } else {
    use_user_printer = 1;
  }

  if (cnts->default_locale_num > 0) locale_id = cnts->default_locale_num;
  if (locale_id > 0) l10n_setlocale(locale_id);
  ff = open_memstream(&log_text, &log_size);
  if (cs->contest_plugin && cs->contest_plugin->print_user_reports) {
    r = (*cs->contest_plugin->print_user_reports)
      (cs->contest_plugin_data, ff, cnts, cs, uset.u, uset.v, locale_id,
       use_user_printer, full_report, use_cypher);
  } else {
    r = ns_print_user_exam_protocols(cnts, cs, ff, uset.u, uset.v, locale_id,
                                     use_user_printer, full_report, use_cypher);
  }
  close_memstream(ff); ff = 0;
  if (locale_id > 0) l10n_setlocale(0);

  l10n_setlocale(phr->locale_id);
  ns_header(fout, extra->header_txt, 0, 0, 0, 0, phr->locale_id, cnts,
            phr->client_key,
            "%s [%s, %d, %s]: %s", ns_unparse_role(phr->role), phr->name_arm,
            phr->contest_id, extra->contest_arm, _("Printing user protocol"));

  fprintf(fout, "<h2>%s</h2>\n",
          (r >= 0)?_("Operation succeeded"):_("Operation failed"));

  fprintf(fout, "<table>");
  fprintf(fout, "<tr><td>%s%s</a></td></tr></table>",
          ns_aref(bb, sizeof(bb), phr, NEW_SRV_ACTION_MAIN_PAGE, 0),
          _("Main page"));

  ss = html_armor_string_dup(log_text);
  fprintf(fout, "<hr/><pre>");
  if (r < 0) fprintf(fout, "<font color=\"red\">");
  fprintf(fout, "%s", ss);
  if (r < 0) fprintf(fout, "</font>");
  fprintf(fout, "</pre>\n");
  xfree(ss); ss = 0;
  xfree(log_text); log_text = 0;

  ns_footer(fout, extra->footer_txt, extra->copyright_txt, phr->locale_id);
  l10n_setlocale(0);

 cleanup:
  if (ff) fclose(ff);
  xfree(uset.v);
  xfree(ss);
  xfree(log_text);
  return retval;
}

static int
priv_print_problem_exam_protocol(
        FILE *fout,
        FILE *log_f,
        struct http_request_info *phr,
        const struct contest_desc *cnts,
        struct contest_extra *extra)
{
  const serve_state_t cs = extra->serve_state;
  int retval = 0, r;
  char *log_text = 0;
  size_t log_size = 0;
  FILE *ff = 0;
  unsigned char bb[1024];
  unsigned char *ss = 0;
  int locale_id = 0;
  int prob_id;

  if (opcaps_check(phr->caps, OPCAP_PRINT_RUN) < 0)
    FAIL(NEW_SRV_ERR_PERMISSION_DENIED);
  if (ns_cgi_param_int(phr, "prob_id", &prob_id) < 0)
    FAIL(NEW_SRV_ERR_INV_PROB_ID);
  if (prob_id <= 0 || prob_id > cs->max_prob || !cs->probs[prob_id])
    FAIL(NEW_SRV_ERR_INV_PROB_ID);

  if (cnts->default_locale_num > 0) locale_id = cnts->default_locale_num;
  if (locale_id > 0) l10n_setlocale(locale_id);
  ff = open_memstream(&log_text, &log_size);
  r = ns_print_prob_exam_protocol(cnts, cs, ff, prob_id, locale_id, 1);
  close_memstream(ff); ff = 0;
  if (locale_id > 0) l10n_setlocale(0);

  l10n_setlocale(phr->locale_id);
  ns_header(fout, extra->header_txt, 0, 0, 0, 0, phr->locale_id, cnts,
            phr->client_key,
            "%s [%s, %d, %s]: %s", ns_unparse_role(phr->role), phr->name_arm,
            phr->contest_id, extra->contest_arm,_("Printing problem protocol"));

  fprintf(fout, "<h2>%s</h2>\n",
          (r >= 0)?_("Operation succeeded"):_("Operation failed"));

  fprintf(fout, "<table>");
  fprintf(fout, "<tr><td>%s%s</a></td></tr></table>",
          ns_aref(bb, sizeof(bb), phr, NEW_SRV_ACTION_MAIN_PAGE, 0),
          _("Main page"));

  ss = html_armor_string_dup(log_text);
  fprintf(fout, "<hr/><pre>");
  if (r < 0) fprintf(fout, "<font color=\"red\">");
  fprintf(fout, "%s", ss);
  if (r < 0) fprintf(fout, "</font>");
  fprintf(fout, "</pre>\n");
  xfree(ss); ss = 0;
  xfree(log_text); log_text = 0;

  ns_footer(fout, extra->footer_txt, extra->copyright_txt, phr->locale_id);
  l10n_setlocale(0);

 cleanup:
  if (ff) fclose(ff);
  xfree(ss);
  xfree(log_text);
  return retval;
}

static int
ping_page(
        FILE *fout,
        FILE *log_f,
        struct http_request_info *phr,
        const struct contest_desc *cnts,
        struct contest_extra *extra)
{
  fprintf(fout, "Content-type: text/plain\n\nOK\n");
  return 0;
}

static int
priv_submit_run_batch_page(
        FILE *fout,
        FILE *log_f,
        struct http_request_info *phr,
        const struct contest_desc *cnts,
        struct contest_extra *extra)
{
  int retval = 0;
  int run_id = 0;

  if (opcaps_check(phr->caps, OPCAP_SUBMIT_RUN) < 0)
    FAIL(NEW_SRV_ERR_PERMISSION_DENIED);

  retval = ns_submit_run(log_f, phr, cnts, extra, NULL, NULL, 1, 1, 1, 1, 1, 1, 0, &run_id, NULL, NULL);
  if (retval >= 0) retval = run_id;

cleanup:
  fprintf(fout, "Content-type: text/plain\n\n%d\n", retval);
  return 0;
}

static void
unpriv_print_status(
        FILE *fout,
        struct http_request_info *phr,
        const struct contest_desc *cnts,
        struct contest_extra *extra,
        time_t start_time,
        time_t stop_time,
        time_t duration,
        time_t sched_time,
        time_t fog_start_time,
        time_t finish_time)
{
  const serve_state_t cs = extra->serve_state;
  const struct section_global_data *global = cs->global;
  const unsigned char *s = 0;
  unsigned char duration_buf[128];
  unsigned char bb[1024];
  time_t tmpt;
  int enable_virtual_start = 0;
  const unsigned char *cl = " class=\"b0\"";

  if (!cnts->exam_mode) {
    fprintf(fout, "<%s>%s</%s>\n",
            cnts->team_head_style, _("Server status"),
            cnts->team_head_style);
    if (stop_time > 0) {
      if (duration > 0 && global->board_fog_time > 0
          && global->board_unfog_time > 0
          && cs->current_time < stop_time + global->board_unfog_time
          && !cs->standings_updated) {
        if (cnts->exam_mode) {
          s = _("The exam is over (standings are frozen)");
        } else {
          s = _("The contest is over (standings are frozen)");
        }
      } else if (cnts->exam_mode) {
        s = _("The exam is over");
      } else {
        s = _("The contest is over");
      }
    } else if (start_time > 0) {
      if (fog_start_time > 0 && cs->current_time >= fog_start_time) {
        if (cnts->exam_mode) {
          s = _("The exam is in progress (standings are frozen)");
        } else {
          s = _("The contest is in progress (standings are frozen)");
        }
      } else {
        if (cnts->exam_mode) {
          s = _("The exam is in progress");
        } else {
          s = _("The contest is in progress");
        }
      }
    } else {
      if (cnts->exam_mode) {
        s = _("The exam is not started");
      } else {
        s = _("The contest is not started");
      }
    }
    fprintf(fout, "<p><b>%s</b></p>\n", s);

    if (cs->upsolving_mode) {
      fprintf(fout, "<p><b>%s</b></p>\n", _("Upsolving mode"));
    }

    if (start_time > 0) {
      if (global->score_system == SCORE_OLYMPIAD && !global->is_virtual) {
        if (cs->accepting_mode)
          s = _("Participants' solutions are being accepted");
        else if (!cs->testing_finished)
          s = _("Participants' solutions are being judged");
        else
          s = _("Participants' solutions are judged");
        fprintf(fout, "<p><b>%s</b></p>\n", s);
      }
    }

    if (cs->clients_suspended) {
      fprintf(fout, "<p><b>%s</b></p>\n",
              _("Participants' requests are suspended"));
    }

    if (start_time > 0) {
      if (cs->testing_suspended) {
        fprintf(fout, "<p><b>%s</b></p>\n",
                _("Testing of participants' submits is suspended"));
      }
      if (cs->printing_suspended) {
        fprintf(fout, "<p><b>%s</b></p>\n",
                _("Print requests are suspended"));
      }
    }

    fprintf(fout, "<table%s>", cl);
    fprintf(fout, "<tr><td%s>%s:</td><td%s>%s</td></tr>\n",
            cl, _("Server time"), cl, xml_unparse_date(cs->current_time));
    if (start_time > 0) {
      if (cnts->exam_mode) {
        s = _("Exam start time");
      } else {
        s = _("Contest start time");
      }
      fprintf(fout, "<tr><td%s>%s:</td><td%s>%s</td></tr>\n",
              cl, s, cl, xml_unparse_date(start_time));
    }
    if (!global->is_virtual && start_time <= 0 && sched_time > 0) {
      fprintf(fout, "<tr><td%s>%s:</td><td%s>%s</td></tr>\n",
              cl, _("Planned start time"), cl, xml_unparse_date(sched_time));
    }
    if (stop_time <= 0 && (duration > 0 || finish_time <= 0)) {
      if (duration > 0) {
        duration_str(0, duration, 0, duration_buf, 0);
      } else {
        snprintf(duration_buf, sizeof(duration_buf), "%s", _("Unlimited"));
      }
      fprintf(fout, "<tr><td%s>%s:</td><td%s>%s</td></tr>\n",
              cl, _("Duration"), cl, duration_buf);
    }
    if (start_time > 0 && stop_time <= 0 && duration > 0) {
      tmpt = start_time + duration;
      fprintf(fout, "<tr><td%s>%s:</td><td%s>%s</td></tr>\n",
              cl, _("Scheduled end time"), cl, xml_unparse_date(tmpt));
    } else if (start_time > 0 && stop_time <= 0 && duration <= 0
               && finish_time > 0) {
      fprintf(fout, "<tr><td%s>%s:</td><td%s>%s</td></tr>\n",
              cl, _("Scheduled end time"), cl, xml_unparse_date(finish_time));
    } else if (stop_time) {
      fprintf(fout, "<tr><td%s>%s:</td><td%s>%s</td></tr>\n",
              cl, _("End time"), cl, xml_unparse_date(stop_time));
    }

    if (start_time > 0 && stop_time <= 0 && fog_start_time > 0) {
      fprintf(fout, "<tr><td%s>%s:</td><td%s>%s</td></tr>\n",
              cl, _("Standings freeze time"), cl, xml_unparse_date(fog_start_time));
    } else if (stop_time > 0 && duration > 0 && global->board_fog_time > 0
               && global->board_unfog_time > 0 && !cs->standings_updated
               && cs->current_time < stop_time + global->board_unfog_time) {
      tmpt = stop_time + global->board_unfog_time;
      fprintf(fout, "<tr><td%s>%s:</td><td%s>%s</td></tr>\n",
              cl, _("Standings unfreeze time"), cl, xml_unparse_date(tmpt));
    }

    if (start_time > 0 && stop_time <= 0 && duration > 0) {
      duration_str(0, cs->current_time, start_time, duration_buf, 0);
      fprintf(fout, "<tr><td%s>%s:</td><td%s>%s</td></tr>\n",
              cl, _("Elapsed time"), cl, duration_buf);
      duration_str(0, start_time + duration - cs->current_time, 0,
                   duration_buf, 0);
      fprintf(fout, "<tr><td%s>%s:</td><td%s>%s</td></tr>\n",
              cl, _("Remaining time"), cl, duration_buf);
    }
    if (start_time <= 0 && global->is_virtual > 0 && cnts->open_time > 0 && cs->current_time < cnts->open_time) {
      fprintf(fout, "<tr><td%s>%s:</td><td%s>%s</td></tr>\n",
              cl, _("Virtual contest open time"), cl, xml_unparse_date(cnts->open_time));
    }
    if (start_time <= 0 && global->is_virtual > 0 && cnts->close_time > 0) {
      fprintf(fout, "<tr><td%s>%s:</td><td%s>%s</td></tr>\n",
              cl, _("Virtual contest close time"), cl, xml_unparse_date(cnts->close_time));
    }
    fprintf(fout, "</table>\n");
  }

  if (global->description_file[0]) {
    watched_file_update(&cs->description, global->description_file,
                        cs->current_time);
    if (cs->description.text) {
      fprintf(fout, "%s", cs->description.text);
    }
  }

  if (!cnts->exam_mode) {
    fprintf(fout, "<p><b>%s: %d</b></p>\n",
            _("On-line users in this contest"), phr->online_users);
    if (cs->max_online_count > 0) {
      fprintf(fout, "<p><b>%s: %d, %s</b></p>\n",
              _("Max number of users was"), cs->max_online_count,
              xml_unparse_date(cs->max_online_time));
    }
  }

  if (!cnts->exam_mode && global->is_virtual && start_time <= 0) {
    enable_virtual_start = 1;
    if (global->disable_virtual_start > 0) {
      enable_virtual_start = 0;
    } else if (cnts->open_time > 0 && cs->current_time < cnts->open_time) {
      enable_virtual_start = 0;
    } else if (cnts->close_time > 0 && cs->current_time >= cnts->close_time) {
      enable_virtual_start = 0;
    }
    if (enable_virtual_start) {
      html_start_form(fout, 1, phr->self_url, phr->hidden_vars);
      if (cnts->exam_mode) {
        fprintf(fout, "<p>%s</p></form>",
                ns_submit_button(bb, sizeof(bb), 0,
                                 NEW_SRV_ACTION_VIRTUAL_START,
                                 _("Start exam")));
      } else {
        fprintf(fout, "<p>%s</p></form>",
                BUTTON(NEW_SRV_ACTION_VIRTUAL_START));
      }
    }
  } else if (!cnts->exam_mode && global->is_virtual && stop_time <= 0) {
    html_start_form(fout, 1, phr->self_url, phr->hidden_vars);
    if (cnts->exam_mode) {
      fprintf(fout, "<p>%s</p></form>",
              ns_submit_button(bb, sizeof(bb), 0, NEW_SRV_ACTION_VIRTUAL_STOP,
                               _("Stop exam")));
    } else {
      fprintf(fout, "<p>%s</p></form>",
              BUTTON(NEW_SRV_ACTION_VIRTUAL_STOP));
    }
  }
}

typedef int (*action_handler2_t)(FILE *fout,
                                 FILE *log_f,
                                 struct http_request_info *phr,
                                 const struct contest_desc *cnts,
                                 struct contest_extra *extra);

static action_handler2_t priv_actions_table_2[NEW_SRV_ACTION_LAST] =
{
#if 0
  [NEW_SRV_ACTION_VIEW_USERS] = priv_view_users_page,
  [NEW_SRV_ACTION_PRIV_USERS_VIEW] = priv_view_priv_users_page,
#endif
  /* for priv_generic_operation */
  [NEW_SRV_ACTION_USERS_REMOVE_REGISTRATIONS] = priv_registration_operation,
  [NEW_SRV_ACTION_USERS_SET_PENDING] = priv_registration_operation,
  [NEW_SRV_ACTION_USERS_SET_OK] = priv_registration_operation,
  [NEW_SRV_ACTION_USERS_SET_REJECTED] = priv_registration_operation,
  [NEW_SRV_ACTION_USERS_SET_INVISIBLE] = priv_registration_operation,
  [NEW_SRV_ACTION_USERS_CLEAR_INVISIBLE] = priv_registration_operation,
  [NEW_SRV_ACTION_USERS_SET_BANNED] = priv_registration_operation,
  [NEW_SRV_ACTION_USERS_CLEAR_BANNED] = priv_registration_operation,
  [NEW_SRV_ACTION_USERS_SET_LOCKED] = priv_registration_operation,
  [NEW_SRV_ACTION_USERS_CLEAR_LOCKED] = priv_registration_operation,
  [NEW_SRV_ACTION_USERS_SET_INCOMPLETE] = priv_registration_operation,
  [NEW_SRV_ACTION_USERS_CLEAR_INCOMPLETE] = priv_registration_operation,
  [NEW_SRV_ACTION_USERS_SET_DISQUALIFIED] = priv_registration_operation,
  [NEW_SRV_ACTION_USERS_CLEAR_DISQUALIFIED] = priv_registration_operation,
  [NEW_SRV_ACTION_USERS_ADD_BY_LOGIN] = priv_add_user_by_login,
  [NEW_SRV_ACTION_USERS_ADD_BY_USER_ID] = priv_add_user_by_user_id,
  [NEW_SRV_ACTION_PRIV_USERS_REMOVE] = priv_priv_user_operation,
  [NEW_SRV_ACTION_PRIV_USERS_ADD_OBSERVER] = priv_priv_user_operation,
  [NEW_SRV_ACTION_PRIV_USERS_DEL_OBSERVER] = priv_priv_user_operation,
  [NEW_SRV_ACTION_PRIV_USERS_ADD_EXAMINER] = priv_priv_user_operation,
  [NEW_SRV_ACTION_PRIV_USERS_DEL_EXAMINER] = priv_priv_user_operation,
  [NEW_SRV_ACTION_PRIV_USERS_ADD_CHIEF_EXAMINER] = priv_priv_user_operation,
  [NEW_SRV_ACTION_PRIV_USERS_DEL_CHIEF_EXAMINER] = priv_priv_user_operation,
  [NEW_SRV_ACTION_PRIV_USERS_ADD_COORDINATOR] = priv_priv_user_operation,
  [NEW_SRV_ACTION_PRIV_USERS_DEL_COORDINATOR] = priv_priv_user_operation,
  [NEW_SRV_ACTION_PRIV_USERS_ADD_BY_USER_ID] = priv_add_priv_user_by_user_id,
  [NEW_SRV_ACTION_PRIV_USERS_ADD_BY_LOGIN] = priv_add_priv_user_by_login,
  [NEW_SRV_ACTION_START_CONTEST] = priv_contest_operation,
  [NEW_SRV_ACTION_STOP_CONTEST] = priv_contest_operation,
  [NEW_SRV_ACTION_CONTINUE_CONTEST] = priv_contest_operation,
  [NEW_SRV_ACTION_SCHEDULE] = priv_contest_operation,
  [NEW_SRV_ACTION_CHANGE_DURATION] = priv_contest_operation,
  [NEW_SRV_ACTION_CHANGE_FINISH_TIME] = priv_contest_operation,
  [NEW_SRV_ACTION_SUSPEND] = priv_contest_operation,
  [NEW_SRV_ACTION_RESUME] = priv_contest_operation,
  [NEW_SRV_ACTION_TEST_SUSPEND] = priv_contest_operation,
  [NEW_SRV_ACTION_TEST_RESUME] = priv_contest_operation,
  [NEW_SRV_ACTION_PRINT_SUSPEND] = priv_contest_operation,
  [NEW_SRV_ACTION_PRINT_RESUME] = priv_contest_operation,
  [NEW_SRV_ACTION_SET_JUDGING_MODE] = priv_contest_operation,
  [NEW_SRV_ACTION_SET_ACCEPTING_MODE] = priv_contest_operation,
  [NEW_SRV_ACTION_SET_TESTING_FINISHED_FLAG] = priv_contest_operation,
  [NEW_SRV_ACTION_CLEAR_TESTING_FINISHED_FLAG] = priv_contest_operation,
  [NEW_SRV_ACTION_SQUEEZE_RUNS] = priv_contest_operation,
  [NEW_SRV_ACTION_RESET_FILTER] = priv_reset_filter,
  [NEW_SRV_ACTION_RESET_CLAR_FILTER] = priv_reset_filter,
  [NEW_SRV_ACTION_CHANGE_LANGUAGE] = priv_change_language,
  [NEW_SRV_ACTION_SUBMIT_RUN] = priv_submit_run,
  [NEW_SRV_ACTION_PRIV_SUBMIT_CLAR] = priv_submit_clar,
  [NEW_SRV_ACTION_PRIV_SUBMIT_RUN_COMMENT] = priv_submit_run_comment,
  [NEW_SRV_ACTION_CLAR_REPLY] = priv_clar_reply,
  [NEW_SRV_ACTION_CLAR_REPLY_ALL] = priv_clar_reply,
  [NEW_SRV_ACTION_CLAR_REPLY_READ_PROBLEM] = priv_clar_reply,
  [NEW_SRV_ACTION_CLAR_REPLY_NO_COMMENTS] = priv_clar_reply,
  [NEW_SRV_ACTION_CLAR_REPLY_YES] = priv_clar_reply,
  [NEW_SRV_ACTION_CLAR_REPLY_NO] = priv_clar_reply,
  [NEW_SRV_ACTION_RELOAD_SERVER] = priv_contest_operation,
  [NEW_SRV_ACTION_CHANGE_STATUS] = priv_change_status,
  [NEW_SRV_ACTION_CHANGE_RUN_STATUS] = priv_change_status,
  [NEW_SRV_ACTION_REJUDGE_DISPLAYED_2] = priv_rejudge_displayed,
  [NEW_SRV_ACTION_FULL_REJUDGE_DISPLAYED_2] = priv_rejudge_displayed,
  [NEW_SRV_ACTION_REJUDGE_PROBLEM_2] = priv_rejudge_problem,
  [NEW_SRV_ACTION_REJUDGE_SUSPENDED_2] = priv_rejudge_all,
  [NEW_SRV_ACTION_REJUDGE_ALL_2] = priv_rejudge_all,
  [NEW_SRV_ACTION_UPDATE_STANDINGS_2] = priv_contest_operation,
  [NEW_SRV_ACTION_RESET_2] = priv_contest_operation,
  [NEW_SRV_ACTION_GENERATE_PASSWORDS_2] = priv_password_operation,
  [NEW_SRV_ACTION_GENERATE_REG_PASSWORDS_2] = priv_password_operation,
  [NEW_SRV_ACTION_CLEAR_PASSWORDS_2] = priv_password_operation,
  [NEW_SRV_ACTION_USER_CHANGE_STATUS] = priv_user_operation,
  [NEW_SRV_ACTION_NEW_RUN] = priv_new_run,
  [NEW_SRV_ACTION_CHANGE_RUN_USER_ID] = priv_edit_run,
  [NEW_SRV_ACTION_CHANGE_RUN_USER_LOGIN] = priv_edit_run,
  [NEW_SRV_ACTION_CHANGE_RUN_PROB_ID] = priv_edit_run,
  [NEW_SRV_ACTION_CHANGE_RUN_VARIANT] = priv_edit_run,
  [NEW_SRV_ACTION_CHANGE_RUN_LANG_ID] = priv_edit_run,
  [NEW_SRV_ACTION_CHANGE_RUN_IS_IMPORTED] = priv_edit_run,
  [NEW_SRV_ACTION_CHANGE_RUN_IS_HIDDEN] = priv_edit_run,
  [NEW_SRV_ACTION_CHANGE_RUN_IS_EXAMINABLE] = priv_edit_run,
  [NEW_SRV_ACTION_CHANGE_RUN_IS_READONLY] = priv_edit_run,
  [NEW_SRV_ACTION_CHANGE_RUN_IS_MARKED] = priv_edit_run,
  [NEW_SRV_ACTION_CHANGE_RUN_IS_SAVED] = priv_edit_run,
  [NEW_SRV_ACTION_CHANGE_RUN_TEST] = priv_edit_run,
  [NEW_SRV_ACTION_CHANGE_RUN_SCORE] = priv_edit_run,
  [NEW_SRV_ACTION_CHANGE_RUN_SCORE_ADJ] = priv_edit_run,
  [NEW_SRV_ACTION_CHANGE_RUN_PAGES] = priv_edit_run,
  [NEW_SRV_ACTION_CLEAR_RUN] = priv_clear_run,
  [NEW_SRV_ACTION_PRINT_RUN] = priv_print_run_cmd,
  [NEW_SRV_ACTION_ISSUE_WARNING] = priv_user_issue_warning,
  [NEW_SRV_ACTION_SET_DISQUALIFICATION] = priv_user_disqualify,
  [NEW_SRV_ACTION_CLEAR_DISPLAYED_2] = priv_clear_displayed,
  [NEW_SRV_ACTION_IGNORE_DISPLAYED_2] = priv_clear_displayed,
  [NEW_SRV_ACTION_DISQUALIFY_DISPLAYED_2] = priv_clear_displayed,
  [NEW_SRV_ACTION_UPSOLVING_CONFIG_2] = priv_upsolving_operation,
  [NEW_SRV_ACTION_UPSOLVING_CONFIG_3] = priv_upsolving_operation,
  [NEW_SRV_ACTION_UPSOLVING_CONFIG_4] = priv_upsolving_operation,
  [NEW_SRV_ACTION_ASSIGN_CHIEF_EXAMINER] = priv_assign_chief_examiner,
  [NEW_SRV_ACTION_ASSIGN_EXAMINER] = priv_assign_examiner,
  [NEW_SRV_ACTION_UNASSIGN_EXAMINER] = priv_unassign_examiner,
  [NEW_SRV_ACTION_TOGGLE_VISIBILITY] = priv_user_toggle_flags,
  [NEW_SRV_ACTION_TOGGLE_BAN] = priv_user_toggle_flags,
  [NEW_SRV_ACTION_TOGGLE_LOCK] = priv_user_toggle_flags,
  [NEW_SRV_ACTION_TOGGLE_INCOMPLETENESS] = priv_user_toggle_flags,
  [NEW_SRV_ACTION_FORCE_START_VIRTUAL] = priv_force_start_virtual,
  [NEW_SRV_ACTION_ASSIGN_CYPHERS_2] = priv_assign_cyphers_2,
  [NEW_SRV_ACTION_SET_PRIORITIES] = priv_set_priorities,
  [NEW_SRV_ACTION_PRIV_SUBMIT_RUN_COMMENT_AND_IGNORE] = priv_submit_run_comment,
  [NEW_SRV_ACTION_PRIV_SUBMIT_RUN_COMMENT_AND_OK] = priv_submit_run_comment,
  [NEW_SRV_ACTION_PRIV_SUBMIT_RUN_JUST_IGNORE] = priv_simple_change_status,
  [NEW_SRV_ACTION_PRIV_SUBMIT_RUN_JUST_OK] = priv_simple_change_status,
  [NEW_SRV_ACTION_PRIV_SET_RUN_REJECTED] = priv_set_run_style_error_status,
  [NEW_SRV_ACTION_TESTING_DELETE] = priv_testing_queue_operation,
  [NEW_SRV_ACTION_TESTING_UP] = priv_testing_queue_operation,
  [NEW_SRV_ACTION_TESTING_DOWN] = priv_testing_queue_operation,
  [NEW_SRV_ACTION_TESTING_DELETE_ALL] = priv_whole_testing_queue_operation,
  [NEW_SRV_ACTION_TESTING_UP_ALL] = priv_whole_testing_queue_operation,
  [NEW_SRV_ACTION_TESTING_DOWN_ALL] = priv_whole_testing_queue_operation,
  [NEW_SRV_ACTION_SET_STAND_FILTER] = priv_stand_filter_operation,
  [NEW_SRV_ACTION_RESET_STAND_FILTER] = priv_stand_filter_operation,
  [NEW_SRV_ACTION_ADMIN_CHANGE_ONLINE_VIEW_SOURCE] = priv_contest_operation,
  [NEW_SRV_ACTION_ADMIN_CHANGE_ONLINE_VIEW_REPORT] = priv_contest_operation,
  [NEW_SRV_ACTION_ADMIN_CHANGE_ONLINE_VIEW_JUDGE_SCORE] = priv_contest_operation,
  [NEW_SRV_ACTION_ADMIN_CHANGE_ONLINE_FINAL_VISIBILITY] = priv_contest_operation,
  [NEW_SRV_ACTION_CHANGE_RUN_FIELDS] = priv_change_run_fields,
  [NEW_SRV_ACTION_PRIV_EDIT_CLAR_ACTION] = ns_priv_edit_clar_action,
  [NEW_SRV_ACTION_PRIV_EDIT_RUN_ACTION] = ns_priv_edit_run_action,

  /* for priv_generic_page */
  [NEW_SRV_ACTION_VIEW_REPORT] = priv_view_report,
  [NEW_SRV_ACTION_VIEW_SOURCE] = priv_view_source,
  [NEW_SRV_ACTION_PRIV_DOWNLOAD_RUN] = priv_download_source,
  [NEW_SRV_ACTION_STANDINGS] = priv_standings,
  [NEW_SRV_ACTION_VIEW_CLAR] = priv_view_clar,
  [NEW_SRV_ACTION_REJUDGE_DISPLAYED_1] = priv_confirmation_page,
  [NEW_SRV_ACTION_FULL_REJUDGE_DISPLAYED_1] = priv_confirmation_page,
  [NEW_SRV_ACTION_REJUDGE_PROBLEM_1] = priv_confirmation_page,
  [NEW_SRV_ACTION_REJUDGE_SUSPENDED_1] = priv_confirmation_page,
  [NEW_SRV_ACTION_REJUDGE_ALL_1] = priv_confirmation_page,
  [NEW_SRV_ACTION_COMPARE_RUNS] = priv_diff_page,
  [NEW_SRV_ACTION_VIEW_TEST_INPUT] = priv_view_test,
  [NEW_SRV_ACTION_VIEW_TEST_ANSWER] = priv_view_test,
  [NEW_SRV_ACTION_VIEW_TEST_INFO] = priv_view_test,
  [NEW_SRV_ACTION_VIEW_TEST_OUTPUT] = priv_view_test,
  [NEW_SRV_ACTION_VIEW_TEST_ERROR] = priv_view_test,
  [NEW_SRV_ACTION_VIEW_TEST_CHECKER] = priv_view_test,
  [NEW_SRV_ACTION_VIEW_AUDIT_LOG] = priv_view_audit_log,
  [NEW_SRV_ACTION_UPDATE_STANDINGS_1] = priv_confirmation_page,
  [NEW_SRV_ACTION_RESET_1] = priv_confirmation_page,
  [NEW_SRV_ACTION_GENERATE_PASSWORDS_1] = priv_confirmation_page,
  [NEW_SRV_ACTION_GENERATE_REG_PASSWORDS_1] = priv_confirmation_page,
  [NEW_SRV_ACTION_CLEAR_PASSWORDS_1] = priv_confirmation_page,
  [NEW_SRV_ACTION_VIEW_CNTS_PWDS] = priv_view_passwords,
  [NEW_SRV_ACTION_VIEW_REG_PWDS] = priv_view_passwords,
  [NEW_SRV_ACTION_VIEW_USER_INFO] = priv_user_detail_page,
  [NEW_SRV_ACTION_NEW_RUN_FORM] = priv_new_run_form_page,
  [NEW_SRV_ACTION_VIEW_USER_DUMP] = priv_view_user_dump,
  [NEW_SRV_ACTION_VIEW_USER_REPORT] = priv_view_report,
  [NEW_SRV_ACTION_DOWNLOAD_ARCHIVE_1] = priv_download_runs_confirmation,
  [NEW_SRV_ACTION_DOWNLOAD_ARCHIVE_2] = priv_download_runs,
  [NEW_SRV_ACTION_UPLOAD_RUNLOG_CSV_1] = priv_upload_runlog_csv_1,
  [NEW_SRV_ACTION_UPLOAD_RUNLOG_CSV_2] = priv_upload_runlog_csv_2,
  [NEW_SRV_ACTION_VIEW_RUNS_DUMP] = priv_view_runs_dump,
  [NEW_SRV_ACTION_EXPORT_XML_RUNS] = priv_view_runs_dump,
  [NEW_SRV_ACTION_WRITE_XML_RUNS] = priv_view_runs_dump,
  [NEW_SRV_ACTION_WRITE_XML_RUNS_WITH_SRC] = priv_view_runs_dump,
  [NEW_SRV_ACTION_UPLOAD_RUNLOG_XML_1] = priv_upload_runlog_xml_1,
  [NEW_SRV_ACTION_UPLOAD_RUNLOG_XML_2] = priv_upload_runlog_xml_2,
  [NEW_SRV_ACTION_CLEAR_DISPLAYED_1] = priv_confirmation_page,
  [NEW_SRV_ACTION_IGNORE_DISPLAYED_1] = priv_confirmation_page,
  [NEW_SRV_ACTION_DISQUALIFY_DISPLAYED_1] = priv_confirmation_page,
  [NEW_SRV_ACTION_UPSOLVING_CONFIG_1] = priv_upsolving_configuration_1,
  [NEW_SRV_ACTION_EXAMINERS_PAGE] = priv_examiners_page,
  [NEW_SRV_ACTION_VIEW_ONLINE_USERS] = priv_view_online_users,
  [NEW_SRV_ACTION_PRINT_USER_PROTOCOL] = priv_print_user_exam_protocol,
  [NEW_SRV_ACTION_PRINT_USER_FULL_PROTOCOL] = priv_print_user_exam_protocol,
  [NEW_SRV_ACTION_PRINT_UFC_PROTOCOL] = priv_print_user_exam_protocol,
  [NEW_SRV_ACTION_PRINT_SELECTED_USER_PROTOCOL] =priv_print_users_exam_protocol,
  [NEW_SRV_ACTION_PRINT_SELECTED_USER_FULL_PROTOCOL] =priv_print_users_exam_protocol,
  [NEW_SRV_ACTION_PRINT_SELECTED_UFC_PROTOCOL] =priv_print_users_exam_protocol,
  [NEW_SRV_ACTION_PRINT_PROBLEM_PROTOCOL] = priv_print_problem_exam_protocol,
  [NEW_SRV_ACTION_ASSIGN_CYPHERS_1] = priv_assign_cyphers_1,
  [NEW_SRV_ACTION_VIEW_EXAM_INFO] = priv_view_exam_info,
  [NEW_SRV_ACTION_PRIO_FORM] = priv_priority_form,
  [NEW_SRV_ACTION_VIEW_USER_IPS] = priv_view_user_ips,
  [NEW_SRV_ACTION_VIEW_IP_USERS] = priv_view_ip_users,
  [NEW_SRV_ACTION_VIEW_TESTING_QUEUE] = priv_view_testing_queue,
  [NEW_SRV_ACTION_MARK_DISPLAYED_2] = priv_clear_displayed,
  [NEW_SRV_ACTION_UNMARK_DISPLAYED_2] = priv_clear_displayed,
  [NEW_SRV_ACTION_ADMIN_CONTEST_SETTINGS] = priv_admin_contest_settings,
  [NEW_SRV_ACTION_PRIV_EDIT_CLAR_PAGE] = priv_edit_clar_page,
  [NEW_SRV_ACTION_PRIV_EDIT_RUN_PAGE] = priv_edit_run_page,
  [NEW_SRV_ACTION_PING] = ping_page,
  [NEW_SRV_ACTION_SUBMIT_RUN_BATCH] = priv_submit_run_batch_page,
};

static void
priv_generic_operation(FILE *fout,
                       struct http_request_info *phr,
                       const struct contest_desc *cnts,
                       struct contest_extra *extra)
{
  FILE *log_f = 0;
  char *log_txt = 0;
  size_t log_len = 0;
  int r, rr;

  log_f = open_memstream(&log_txt, &log_len);

  r = priv_actions_table_2[phr->action](fout, log_f, phr, cnts, extra);
  if (r == -1) {
    close_memstream(log_f);
    xfree(log_txt);
    return;
  }
  if (r < 0) {
    ns_error(log_f, r);
    r = 0;
  }
  rr = r;
  if (!r) r = ns_priv_next_state[phr->action];
  if (!rr) rr = ns_priv_prev_state[phr->action];

  close_memstream(log_f); log_f = 0;
  if (!log_txt || !*log_txt) {
    /*
    if (r == NEW_SRV_ACTION_VIEW_SOURCE) {
      if (phr->next_run_id < 0) r = 0;
      else snprintf(next_extra, sizeof(next_extra), "run_id=%d",
                    phr->next_run_id);
    }
    */
    if (phr->plain_text) {
      fprintf(fout, "Content-type: text/plain\n\n%d\n", 0);
    } else {
      ns_refresh_page(fout, phr, r, phr->next_extra);
    }
  } else {
    html_error_status_page(fout, phr, cnts, extra, log_txt, rr, 0);
  }
  xfree(log_txt);
}

static void
priv_generic_page(FILE *fout,
                  struct http_request_info *phr,
                  const struct contest_desc *cnts,
                  struct contest_extra *extra)
{
  FILE *log_f = 0;
  char *log_txt = 0;
  size_t log_len = 0;
  int r;

  log_f = open_memstream(&log_txt, &log_len);

  r = priv_actions_table_2[phr->action](fout, log_f, phr, cnts, extra);
  if (r == -1) {
    close_memstream(log_f);
    xfree(log_txt);
    return;
  }
  if (r < 0) {
    ns_error(log_f, r);
    r = 0;
  }
  if (!r) r = ns_priv_prev_state[phr->action];

  close_memstream(log_f); log_f = 0;
  if (log_txt && *log_txt) {
    html_error_status_page(fout, phr, cnts, extra, log_txt, r, 0);
  }
  xfree(log_txt);
}

static void
priv_logout(FILE *fout,
            struct http_request_info *phr,
            const struct contest_desc *cnts,
            struct contest_extra *extra)
{
  //unsigned char locale_buf[64];
  unsigned char urlbuf[1024];

  if (ns_open_ul_connection(phr->fw_state) < 0)
    return ns_html_err_ul_server_down(fout, phr, 0, 0);
  userlist_clnt_delete_cookie(ul_conn, phr->user_id,
                              phr->contest_id,
                              phr->client_key,
                              phr->session_id);
  ns_remove_session(phr->session_id);
  snprintf(urlbuf, sizeof(urlbuf),
           "%s?contest_id=%d&locale_id=%d&role=%d",
           phr->self_url, phr->contest_id, phr->locale_id, phr->role);
  ns_refresh_page_2(fout, phr->client_key, urlbuf);
}


static void
write_alternatives_file(FILE *fout, int is_radio, const unsigned char *txt,
                        int last_answer, int prob_id, int next_prob_id,
                        int enable_js, const unsigned char *class_name)
{
  const unsigned char *s, *p;
  unsigned char *txt2;
  size_t txt_len, t_len;
  int line_max_count = 0, line_count = 0, i;
  unsigned char **lines = 0;
  unsigned char *t;
  unsigned char *cl = "";
  unsigned char jsbuf[1024];

  if (!txt) return;

  if (class_name && *class_name) {
    cl = (unsigned char *) alloca(strlen(class_name) + 32);
    sprintf(cl, " class=\"%s\"", class_name);
  }

  // normalize the file
  txt_len = strlen(txt);
  txt2 = (unsigned char*) alloca(txt_len + 2);
  memcpy(txt2, txt, txt_len + 1);
  while (txt_len > 0 && isspace(txt2[txt_len - 1])) txt_len--;
  if (!txt_len) return;
  txt2[txt_len++] = '\n';
  txt2[txt_len] = 0;

  // count number of lines
  for (s = txt2; *s; s++)
    if (*s == '\n') line_max_count++;

  lines = (unsigned char**) alloca((line_max_count + 1) * sizeof(lines[0]));
  memset(lines, 0, (line_max_count + 1) * sizeof(lines[0]));

  s = txt2;
  while (*s) {
    while (*s != '\n' && isspace(*s)) s++;
    if (*s == '#') while (*s != '\n') s++;
    if (*s == '\n') {
      s++;
      continue;
    }
    p = s;
    while (*s != '\n') s++;
    t_len = s - p;
    t = (unsigned char*) alloca(t_len + 1);
    memcpy(t, p, t_len);
    while (t_len > 0 && isspace(t[t_len - 1])) t_len--;
    t[t_len] = 0;
    lines[line_count++] = t;
  }

  for (i = 0; i < line_count; i++) {
    if (is_radio) {
      jsbuf[0] = 0;
      if (prob_id > 0 && enable_js) {
        snprintf(jsbuf, sizeof(jsbuf), " onclick=\"submitAnswer(%d,%d,%d,%d,%d)\"", NEW_SRV_ACTION_UPDATE_ANSWER, prob_id, i + 1, NEW_SRV_ACTION_VIEW_PROBLEM_SUBMIT, next_prob_id);
      }
      s = "";
      if (last_answer == i + 1) s = " checked=\"1\"";
      fprintf(fout, "<tr><td%s>%d</td><td%s><input type=\"radio\" name=\"file\" value=\"%d\"%s%s/></td><td%s>%s</td></tr>\n", cl, i + 1, cl, i + 1, s, jsbuf, cl, lines[i]);
    } else {
      fprintf(fout, "<tr><td%s>%d</td><td%s><input type=\"checkbox\" name=\"ans_%d\"/></td><td%s>%s</td></tr>\n", cl, i + 1, cl, i + 1, cl, lines[i]);
    }
  }
}

static void
unparse_statement(
        FILE *fout,
        struct http_request_info *phr,
        const struct contest_desc *cnts,
        struct contest_extra *extra,
        const struct section_problem_data *prob,
        int variant,
        problem_xml_t px,
        const unsigned char *bb,
        int is_submittable);
static void
unparse_answers(
        FILE *fout,
        struct http_request_info *phr,
        const struct contest_desc *cnts,
        struct contest_extra *extra,
        const struct section_problem_data *prob,
        int variant,
        problem_xml_t px,
        const unsigned char *lang,
        int is_radio,
        int last_answer,
        int next_prob_id,
        int enable_js,
        const unsigned char *class_name);

static void
priv_submit_page(
        FILE *fout,
        struct http_request_info *phr, 
        const struct contest_desc *cnts,
        struct contest_extra *extra)
{
  serve_state_t cs = extra->serve_state;
  //const struct section_global_data *global = cs->global;
  const struct section_problem_data *prob = 0;
  int prob_id = 0, variant = 0, i;
  FILE *log_f = 0;
  char *log_t = 0;
  size_t log_z = 0;
  struct html_armor_buffer ab = HTML_ARMOR_INITIALIZER;
  unsigned char bb[1024];
  const unsigned char *sel_flag = 0;
  unsigned char optval[128];
  problem_xml_t px = 0;
  struct watched_file *pw = 0;
  const unsigned char *pw_path = 0;
  path_t variant_stmt_file;
  const unsigned char *alternatives = 0;
  const unsigned char *cl = " class=\"b0\"";

  log_f = open_memstream(&log_t, &log_z);
  if (ns_cgi_param_int_opt(phr, "problem", &prob_id, 0) < 0) {
    fprintf(log_f, "Invalid problem.\n");
    goto cleanup;
  }
  if (prob_id < 0 || prob_id > cs->max_prob) {
    fprintf(log_f, "Invalid problem.\n");
    goto cleanup;
  }
  if (prob_id > 0 && !(prob = cs->probs[prob_id])) {
    fprintf(log_f, "Invalid problem.\n");
    goto cleanup;
  }
  if (ns_cgi_param_int_opt(phr, "variant", &variant, 0) < 0) {
    fprintf(log_f, "Invalid variant.\n");
    goto cleanup;
  }
  if (!prob) variant = 0;
  if (prob && prob->variant_num <= 0) variant = 0;
  if (variant < 0
      || (prob && prob->variant_num <= 0 && variant > 0)
      || (prob && prob->variant_num > 0 && variant > prob->variant_num)) {
    fprintf(log_f, "Invalid variant.\n");
    goto cleanup;
  }
  if (opcaps_check(phr->caps, OPCAP_SUBMIT_RUN) < 0) {
    fprintf(log_f, "Permission denied.\n");
    goto cleanup;
  }

  l10n_setlocale(phr->locale_id);
  if (prob && variant > 0) {
    snprintf(bb, sizeof(bb), "%s %s-%d", _("Submit a solution for"),
             prob->short_name, variant);
  } else if (prob) {
    snprintf(bb, sizeof(bb), "%s %s", _("Submit a solution for"),
             prob->short_name);
  } else {
    snprintf(bb, sizeof(bb), "%s", _("Submit a solution"));
  }
  ns_header(fout, extra->header_txt, 0, 0, 0, 0, phr->locale_id, cnts,
            phr->client_key,
            "%s [%s, %d, %s]: %s", ns_unparse_role(phr->role),
            phr->name_arm, phr->contest_id, extra->contest_arm, bb);

  if (prob) {
    fprintf(fout, "<h2>%s %s: %s</h2>\n",
            _("Problem"), prob->short_name, ARMOR(prob->long_name));
  }

  html_start_form(fout, 2, phr->self_url, phr->hidden_vars);

  /* output the problem selection dialog */
  fprintf(fout, "<table%s><tr>\n", cl);
  fprintf(fout, "<tr><td%s>%s:</td><td%s><select name=\"problem\">",
          cl, _("Problem"), cl);
  for (i = 1; i <= cs->max_prob; i++) {
    if (!(cs->probs[i])) continue;
    sel_flag = "";
    if (prob_id > 0 && i == prob_id) sel_flag = " selected=\"selected\"";
    fprintf(fout, "<option value=\"%d\"%s>%s - %s</option>",
            i, sel_flag, cs->probs[i]->short_name,
            ARMOR(cs->probs[i]->long_name));
  }
  fprintf(fout, "</select></td><td%s>%s</td>\n", cl,
          ns_submit_button(bb, sizeof(bb), 0,
                           NEW_SRV_ACTION_PRIV_SUBMIT_PAGE,
                           _("Select problem")));
  fprintf(fout, "</tr>\n");

  if (prob && prob->variant_num > 0) {
    fprintf(fout, "<tr>\n");
    fprintf(fout, "<td%s>%s:</td><td%s>", cl, _("Variant"), cl);
    fprintf(fout, "<select name=\"variant\">");
    for (i = 0; i <= prob->variant_num; i++) {
      sel_flag = "";
      if (i == variant) sel_flag = " selected=\"selected\"";
      optval[0] = 0;
      if (i > 0) snprintf(optval, sizeof(optval), "%d", i);
      fprintf(fout, "<option value=\"%d\"%s>%s</option>",
              i, sel_flag, optval);
    }
    fprintf(fout, "</select></td><td%s>%s</td>", cl,
            ns_submit_button(bb, sizeof(bb), 0,
                             NEW_SRV_ACTION_PRIV_SUBMIT_PAGE,
                             _("Select variant")));
    fprintf(fout, "</tr>\n");
  }
  fprintf(fout, "</table>\n");

  /* output the problem statement */
  px = 0; pw = 0; pw_path = 0;
  if (prob && prob->variant_num > 0 && variant > 0 && prob->xml.a
      && prob->xml.a[variant - 1]) {
    px = prob->xml.a[variant - 1];
  } else if (prob && prob->variant_num <= 0 && prob->xml.p) {
    px = prob->xml.p;
  }
  if (px && px->stmts) {
    unparse_statement(fout, phr, cnts, extra, prob, variant, px, NULL, 1);
  }

  if (!px && prob && prob->statement_file[0]) {
    if (prob->variant_num > 0 && variant > 0) {
      prepare_insert_variant_num(variant_stmt_file,
                                 sizeof(variant_stmt_file),
                                 prob->statement_file, variant);
      pw = &cs->prob_extras[prob_id].v_stmts[variant];
      pw_path = variant_stmt_file;
    } else if (prob->variant_num <= 0) {
      pw = &cs->prob_extras[prob_id].stmt;
      pw_path = prob->statement_file;
    }
    watched_file_update(pw, pw_path, cs->current_time);
    if (!pw->text) {
      fprintf(fout, "<big><font color=\"red\"><p>%s</p></font></big>\n",
              _("The problem statement is not available"));
     } else {
      fprintf(fout, "%s", pw->text);
    }
  }

  /* update the alternatives */
  alternatives = 0;
  if (prob && (prob->type == PROB_TYPE_SELECT_ONE
               || prob->type == PROB_TYPE_SELECT_MANY)
      && prob->alternatives_file[0]) {
    if (prob->variant_num > 0 && variant > 0) {
      prepare_insert_variant_num(variant_stmt_file,
                                 sizeof(variant_stmt_file),
                                 prob->alternatives_file, variant);
      pw = &cs->prob_extras[prob->id].v_alts[variant];
      pw_path = variant_stmt_file;
    } else if (prob->variant_num <= 0) {
      pw = &cs->prob_extras[prob->id].alt;
      pw_path = prob->alternatives_file;
    }
    watched_file_update(pw, pw_path, cs->current_time);
    alternatives = pw->text;
  }

  fprintf(fout, "<table%s>\n", cl);

  /* language selection */
  if (!prob || !prob->type) {
    fprintf(fout, "<tr>");
    fprintf(fout, "<td%s>%s:</td>", cl, _("Language"));
    fprintf(fout, "<td%s><select name=\"lang_id\"><option value=\"\"></option>",
            cl);
    for (i = 1; i <= cs->max_lang; i++) {
      if (cs->langs[i]) {
        fprintf(fout, "<option value=\"%d\">%s - %s</option>",
                i, cs->langs[i]->short_name, ARMOR(cs->langs[i]->long_name));
      }
    }
    fprintf(fout, "</td></tr>\n");

    if (cs->global->enable_eoln_select > 0) {
      fprintf(fout, "<tr><td%s>%s:</td><td%s><select name=\"eoln_type\"%s>",
              "", "EOLN Type", "", "");
      fprintf(fout, "<option value=\"0\"></option>");
      fprintf(fout, "<option value=\"1\"%s>LF (Unix/MacOS)</option>", "");
      fprintf(fout, "<option value=\"2\"%s>CRLF (Windows/DOS)</option>", "");
      fprintf(fout, "</select></td></tr>\n");
    }
  }

  /* solution/answer form */
  if (!prob /*|| !prob->type*/) {
    fprintf(fout, "<tr><td%s>%s</td><td%s><input type=\"file\" name=\"file\"/></td></tr>\n", cl, _("File"), cl);
   } else {
    switch (prob->type) {
    case PROB_TYPE_STANDARD:
    case PROB_TYPE_OUTPUT_ONLY:
    case PROB_TYPE_TESTS:
      if (prob->enable_text_form > 0) {
        fprintf(fout, "<tr><td colspan=\"2\"%s><textarea name=\"text_form\" rows=\"20\" cols=\"60\"></textarea></td></tr>\n", cl);
      }
      fprintf(fout, "<tr><td%s>%s</td><td%s><input type=\"file\" name=\"file\"/></td></tr>\n", cl, _("File"), cl);
      break;
    case PROB_TYPE_SHORT_ANSWER:
      fprintf(fout, "<tr><td%s>%s</td><td%s><input type=\"text\" name=\"file\"/></td></tr>\n", cl, _("Answer"), cl);
      break;
    case PROB_TYPE_TEXT_ANSWER:
      fprintf(fout, "<tr><td colspan=\"2\"%s><textarea name=\"file\" rows=\"20\" cols=\"60\"></textarea></td></tr>\n", cl);
      break;
    case PROB_TYPE_SELECT_ONE:
      if (px) {
        unparse_answers(fout, phr, cnts, extra, prob, variant,
                        px, 0 /* lang */, 1 /* is_radio */,
                        -1, prob_id, 0 /* js_flag */, "b0");
      } else if (alternatives) {
        write_alternatives_file(fout, 1, alternatives, -1, 0, 0, 0, "b0");
      } else if (prob->alternative) {
        for (i = 0; prob->alternative[i]; i++) {
          fprintf(fout, "<tr><td%s>%d</td><td%s><input type=\"radio\" name=\"file\" value=\"%d\"/></td><td%s>%s</td></tr>\n", cl, i + 1, cl, i + 1, cl, prob->alternative[i]);
        }
      }
      break;
    case PROB_TYPE_SELECT_MANY:
      if (alternatives) {
        write_alternatives_file(fout, 0, alternatives, -1, 0, 0, 0, "b0");
      } else if (prob->alternative) {
        for (i = 0; prob->alternative[i]; i++) {
          fprintf(fout, "<tr><td%s>%d</td><td%s><input type=\"checkbox\" name=\"ans_%d\"/></td><td%s>%s</td></tr>\n", cl, i + 1, cl, i + 1,
                  cl, prob->alternative[i]);
        }
      }
      break;
    case PROB_TYPE_CUSTOM:
      break;

    default:
      abort();
    }
  }

  fprintf(fout, "<tr><td%s>&nbsp;</td><td%s>%s</td></tr>\n",
          cl, cl, BUTTON(NEW_SRV_ACTION_SUBMIT_RUN));
  fprintf(fout, "<tr><td%s>&nbsp;</td><td%s>%s%s</a></td></tr>",
          cl, cl, ns_aref(bb, sizeof(bb), phr, NEW_SRV_ACTION_MAIN_PAGE, 0),
          _("Main page"));
  fprintf(fout, "</table>\n");

  fprintf(fout, "</form>\n");
  ns_footer(fout, extra->footer_txt, extra->copyright_txt, phr->locale_id);
  l10n_setlocale(0);

cleanup:
  html_armor_free(&ab);
  close_memstream(log_f); log_f = 0;
  if (log_t && *log_t) {
    html_error_status_page(fout, phr, cnts, extra, log_t, 0, 0);
  }
  xfree(log_t); log_t = 0;
}

static void
priv_get_file(
        FILE *fout,
        struct http_request_info *phr,
        const struct contest_desc *cnts,
        struct contest_extra *extra)
{
  const serve_state_t cs = extra->serve_state;
  const struct section_global_data *global = cs->global;
  const struct section_problem_data *prob = 0;
  int retval = 0, prob_id, n, variant = 0, mime_type = 0;
  const unsigned char *s = 0;
  path_t fpath, sfx;
  char *file_bytes = 0;
  size_t file_size = 0;
  const unsigned char *content_type = 0;

  if (opcaps_check(phr->caps, OPCAP_SUBMIT_RUN) < 0)
    FAIL(NEW_SRV_ERR_PERMISSION_DENIED);

  if (ns_cgi_param(phr, "prob_id", &s) <= 0
      || sscanf(s, "%d%n", &prob_id, &n) != 1 || s[n]
      || prob_id <= 0 || prob_id > cs->max_prob
      || !(prob = cs->probs[prob_id]))
    FAIL(NEW_SRV_ERR_INV_PROB_ID);
  if (ns_cgi_param_int_opt(phr, "variant", &variant, 0) < 0)
    FAIL(NEW_SRV_ERR_INV_VARIANT);
  if (prob->variant_num <= 0) {
    variant = 0;
  } else {
    if (variant <= 0 || variant > prob->variant_num)
      FAIL(NEW_SRV_ERR_INV_VARIANT);
  }

  if (ns_cgi_param(phr, "file", &s) <= 0 || strchr(s, '/'))
    FAIL(NEW_SRV_ERR_INV_FILE_NAME);

  os_rGetSuffix(s, sfx, sizeof(sfx));
  if (global->advanced_layout) {
    get_advanced_layout_path(fpath, sizeof(fpath), global, prob, s, variant);
  } else {
    if (variant > 0) {
      snprintf(fpath, sizeof(fpath), "%s/%s-%d/%s",
               global->statement_dir, prob->short_name, variant, s);
    } else {
      snprintf(fpath, sizeof(fpath), "%s/%s/%s",
               global->statement_dir, prob->short_name, s);
    }
  }
  mime_type = mime_type_parse_suffix(sfx);
  content_type = mime_type_get_type(mime_type);

  if (generic_read_file(&file_bytes, 0, &file_size, 0, 0, fpath, "") < 0)
    FAIL(NEW_SRV_ERR_INV_FILE_NAME);

  fprintf(fout, "Content-type: %s\n\n", content_type);
  fwrite(file_bytes, 1, file_size, fout);

 cleanup:
  if (retval) {
    snprintf(fpath, sizeof(fpath), "Error %d", -retval);
    html_error_status_page(fout, phr, cnts, extra, fpath,
                           NEW_SRV_ACTION_MAIN_PAGE, 0);
  }
  xfree(file_bytes);
}

static void
priv_main_page(FILE *fout,
               struct http_request_info *phr,
               const struct contest_desc *cnts,
               struct contest_extra *extra)
{
  serve_state_t cs = extra->serve_state;
  const struct section_global_data *global = cs->global;
  time_t start_time, sched_time, duration, stop_time, fog_start_time = 0, tmpt;
  time_t finish_time;
  unsigned char hbuf[1024];
  unsigned char duration_buf[128];
  const unsigned char *s;
  unsigned char bb[1024];
  int action;
  long long tdiff;
  int filter_first_run = 0, filter_last_run = 0, filter_mode_clar = 0;
  int filter_first_run_set = 0, filter_last_run_set = 0;
  const unsigned char *filter_expr = 0;
  int i, x, y, n, variant = 0, need_examiners = 0, online_users = 0;
  const struct section_problem_data *prob = 0;
  path_t variant_stmt_file;
  struct watched_file *pw = 0;
  const unsigned char *pw_path;
  const unsigned char *alternatives;
  struct html_armor_buffer ab = HTML_ARMOR_INITIALIZER;
  int skip_start_form = 0;
  struct last_access_info *pa;
  const unsigned char *filter_first_clar_str = 0;
  const unsigned char *filter_last_clar_str = 0;

  if (ns_cgi_param(phr, "filter_expr", &s) > 0) filter_expr = s;

  ns_cgi_param_int_opt_2(phr, "filter_first_run", &filter_first_run, &filter_first_run_set);
  ns_cgi_param_int_opt_2(phr, "filter_last_run", &filter_last_run, &filter_last_run_set);

  if (ns_cgi_param(phr, "filter_first_clar", &s) > 0 && s)
    filter_first_clar_str = s;
  if (ns_cgi_param(phr, "filter_last_clar", &s) > 0 && s)
    filter_last_clar_str = s;
  if (ns_cgi_param(phr, "filter_mode_clar", &s) > 0
      && sscanf(s, "%d%n", &x, &n) == 1 && !s[n] && x >= 1 && x <= 2)
    filter_mode_clar = x;
  if (ns_cgi_param(phr, "problem", &s) > 0) {
    if (sscanf(s, "%d_%d%n", &x, &y, &n) == 2 && !s[n]
        && x > 0 && x <= cs->max_prob && cs->probs[x]
        && cs->probs[x]->variant_num > 0 && y > 0
        && y <= cs->probs[x]->variant_num) {
      prob = cs->probs[x];
      variant = y;
    } else if (sscanf(s, "%d%n", &x, &n) == 1 && !s[n]
               && x > 0 && x <= cs->max_prob && cs->probs[x]
               && cs->probs[x]->variant_num <= 0) {
      prob = cs->probs[x];
    }
  }

  run_get_times(cs->runlog_state, &start_time, &sched_time, &duration,
                &stop_time, &finish_time);
  if (duration > 0 && start_time && !stop_time && global->board_fog_time > 0)
    fog_start_time = start_time + duration - global->board_fog_time;
  if (fog_start_time < 0) fog_start_time = 0;

  for (i = 1; i <= cs->max_prob; i++)
    if (cs->probs[i] && cs->probs[i]->manual_checking)
      need_examiners = 1;

  l10n_setlocale(phr->locale_id);
  ns_header(fout, extra->header_txt, 0, 0, 0, 0, phr->locale_id, cnts,
            phr->client_key,
            "%s [%s, %d, %s]: %s", ns_unparse_role(phr->role),
            phr->name_arm, phr->contest_id, extra->contest_arm, _("Main page"));

  fprintf(fout,
          "<script language=\"javascript\">\n"
          "var self_url='%s';\n"
          "var SID='%016llx';\n"
          "</script>\n",
          phr->self_url,
          phr->session_id);

  fprintf(fout, "<ul>\n");
  fprintf(fout, "<li>%s%s</a></li>\n",
          ns_aref(hbuf, sizeof(hbuf), phr, NEW_SRV_ACTION_VIEW_USERS, 0),
          _("View regular users"));
  fprintf(fout, "<li>%s%s</a></li>\n",
          ns_aref(hbuf, sizeof(hbuf), phr, NEW_SRV_ACTION_PRIV_USERS_VIEW, 0),
          _("View privileged users"));
  fprintf(fout, "<li>%s%s</a></li>\n",
          ns_aref(hbuf, sizeof(hbuf), phr, NEW_SRV_ACTION_VIEW_ONLINE_USERS, 0),
          _("View who is currently online"));
  fprintf(fout, "<li>%s%s</a></li>\n",
          ns_aref(hbuf, sizeof(hbuf), phr, NEW_SRV_ACTION_VIEW_EXAM_INFO, 0),
          _("View examination information"));
  if (need_examiners)
    fprintf(fout, "<li>%s%s</a></li>\n",
            ns_aref(hbuf, sizeof(hbuf), phr, NEW_SRV_ACTION_EXAMINERS_PAGE, 0),
            _("Examiners assignments"));
  fprintf(fout, "<li>%s%s</a></li>\n",
          ns_aref(hbuf, sizeof(hbuf), phr, NEW_SRV_ACTION_STANDINGS, 0),
          _("View standings"));
  if (phr->role >= USER_ROLE_JUDGE
      && opcaps_check(phr->caps, OPCAP_EDIT_PASSWD) >= 0) {
    fprintf(fout, "<li>%s%s</a></li>\n",
            ns_aref(hbuf, sizeof(hbuf), phr, NEW_SRV_ACTION_VIEW_REG_PWDS, 0),
            _("View registration passwords"));
    if (!cnts->disable_team_password) {
      fprintf(fout, "<li>%s%s</a></li>\n",
              ns_aref(hbuf, sizeof(hbuf), phr, NEW_SRV_ACTION_VIEW_CNTS_PWDS,0),
              _("View contest passwords"));
    }
  }
  if (phr->role >= USER_ROLE_JUDGE
      && opcaps_check(phr->caps, OPCAP_DUMP_USERS) >= 0) {
    fprintf(fout, "<li>%s%s</a></li>\n",
            ns_aref(hbuf, sizeof(hbuf), phr, NEW_SRV_ACTION_VIEW_USER_DUMP, 0),
            _("Dump users in CSV format"));
  }
  if (phr->role >= USER_ROLE_JUDGE
      && opcaps_check(phr->caps, OPCAP_DUMP_RUNS) >= 0) {
    fprintf(fout, "<li>%s%s</a></li>\n",
            ns_aref(hbuf, sizeof(hbuf), phr, NEW_SRV_ACTION_VIEW_RUNS_DUMP, 0),
            _("Dump runs in CSV format"));
    fprintf(fout, "<li>%s%s</a></li>\n",
            ns_aref(hbuf, sizeof(hbuf), phr, NEW_SRV_ACTION_EXPORT_XML_RUNS, 0),
            _("Export runs in XML external format"));
    fprintf(fout, "<li>%s%s</a></li>\n",
            ns_aref(hbuf, sizeof(hbuf), phr, NEW_SRV_ACTION_WRITE_XML_RUNS, 0),
            _("Write runs in XML internal format"));
    fprintf(fout, "<li>%s%s</a></li>\n",
            ns_aref(hbuf, sizeof(hbuf), phr, NEW_SRV_ACTION_WRITE_XML_RUNS_WITH_SRC, 0),
            _("Write runs in XML internal format with source"));
  }
  fprintf(fout, "<li>%s%s</a></li>\n",
          ns_aref(hbuf, sizeof(hbuf), phr, NEW_SRV_ACTION_ASSIGN_CYPHERS_1, 0),
          _("Assign random cyphers"));
  fprintf(fout, "<li>%s%s</a></li>\n",
          ns_aref(hbuf, sizeof(hbuf), phr, NEW_SRV_ACTION_VIEW_IP_USERS, 0),
          _("View users for IP addresses"));
  fprintf(fout, "<li>%s%s</a></li>\n",
          ns_aref(hbuf, sizeof(hbuf), phr, NEW_SRV_ACTION_VIEW_USER_IPS, 0),
          _("View IP addresses for users"));
  fprintf(fout, "<li>%s%s</a></li>\n",
          ns_aref(hbuf, sizeof(hbuf), phr, NEW_SRV_ACTION_VIEW_TESTING_QUEUE,0),
          _("View testing queue"));
  if (phr->role >= USER_ROLE_ADMIN) {
    fprintf(fout, "<li>%s%s</a></li>\n",
            ns_aref(hbuf, sizeof(hbuf), phr, NEW_SRV_ACTION_ADMIN_CONTEST_SETTINGS, 0),
            _("Contest settings"));
  }
  if (cnts->problems_url) {
    fprintf(fout, "<li><a href=\"%s\" target=_blank>%s</a>\n",
            cnts->problems_url, _("Problems"));
  }
  fprintf(fout, "<li>%s%s</a></li>\n",
          ns_aref(hbuf, sizeof(hbuf), phr, NEW_SRV_ACTION_PRIV_SUBMIT_PAGE, 0),
          _("Submit a solution"));
  fprintf(fout, "<li>%s%s</a></li>\n",
          ns_aref(hbuf, sizeof(hbuf), phr, NEW_SRV_ACTION_LOGOUT, 0),
          _("Logout"));

  fprintf(fout, "</ul>\n");

  /* if role == ADMIN and capability CONTROL_CONTEST */

  fprintf(fout, "<hr><a name=\"status\"></a><%s>%s</%s>\n",
          /*cnts->priv_head_style*/ "h2", _("Server status"),
          /*cnts->priv_head_style*/ "h2");
  if (stop_time > 0 && !global->is_virtual) {
    if (duration > 0 && global->board_fog_time > 0
        && global->board_unfog_time > 0
        && cs->current_time < stop_time + global->board_unfog_time
        && !cs->standings_updated) {
      s = _("The contest is over (standings are frozen)");
    } else {
      s = _("The contest is over");
    }
  } else if (start_time > 0) {
    if (fog_start_time > 0 && cs->current_time >= fog_start_time)
      s = _("The contest is in progress (standings are frozen)");
    else
      s = _("The contest is in progress");
  } else {
    s = _("The contest is not started");
  }
  fprintf(fout, "<p><big><b>%s</b></big></p>\n", s);

  if (global->score_system == SCORE_OLYMPIAD && !global->is_virtual) {
    if (cs->accepting_mode)
      s = _("Participants' solutions are being accepted");
    else if (!cs->testing_finished)
      s = _("Participants' solutions are being judged");
    else
      s = _("Participants' solutions are judged");
    fprintf(fout, "<p><big><b>%s</b></big></p>\n", s);
  }

  if (cs->upsolving_mode) {
    fprintf(fout, "<p><big><b>%s</b></big></p>\n", _("Upsolving mode"));
  }

  if (cs->clients_suspended) {
    fprintf(fout, "<p><big><b>%s</b></big></p>\n",
            _("Participants' requests are suspended"));
  }

  if (cs->testing_suspended) {
    fprintf(fout, "<p><big><b>%s</b></big></p>\n",
            _("Testing of participants' submits is suspended"));
  }
  if (cs->printing_suspended) {
    fprintf(fout, "<p><big><b>%s</b></big></p>\n",
            _("Print requests are suspended"));
  }
  if (cs->online_view_source < 0) {
    fprintf(fout, "<p><big><b>%s</b></big></p>\n",
            _("Source code is closed"));
  } else if (cs->online_view_source > 0) {
    fprintf(fout, "<p><big><b>%s</b></big></p>\n",
            _("Source code is open"));
  }
  if (cs->online_view_report < 0) {
    fprintf(fout, "<p><big><b>%s</b></big></p>\n",
            _("Testing reports are closed"));
  } else if (cs->online_view_report > 0) {
    fprintf(fout, "<p><big><b>%s</b></big></p>\n",
            _("Testing reports are open"));
  }
  if (cs->online_view_judge_score > 0) {
    fprintf(fout, "<p><big><b>%s</b></big></p>\n",
            _("Judge scores are opened"));
  }
  if (cs->online_final_visibility > 0) {
    fprintf(fout, "<p><big><b>%s</b></big></p>\n",
            _("Final visibility rules are active"));
  }

  // count online users
  online_users = 0;
  for (i = 0; i < extra->user_access[USER_ROLE_CONTESTANT].u; i++) {
    pa = &extra->user_access[USER_ROLE_CONTESTANT].v[i];
    if (pa->time + 65 >= cs->current_time) online_users++;
  }
  fprintf(fout, "<p><big><b>%s: %d</b></big></p>\n",
          _("On-line users in this contest"), online_users);
  if (cs->max_online_count > 0) {
    fprintf(fout, "<p><big><b>%s: %d, %s</b></big></p>\n",
            _("Max number of users was"), cs->max_online_count,
            xml_unparse_date(cs->max_online_time));
  }

  if (job_count > 0) {
    fprintf(fout, "<p><b>%s: %d</b></p>\n", "Background jobs", job_count);
    fprintf(fout, "<table class=\"b1\">");
    for (struct server_framework_job *job = job_first; job; job = job->next) {
      fprintf(fout, "<tr><td%s>%d</td><td%s>%s</td><td%s>%s</td><td%s>",
              " class=\"b1\"", job->id,
              " class=\"b1\"", xml_unparse_date(job->start_time),
              " class=\"b1\"", job->title,
              " class=\"b1\"");
      if (job->vt->get_status) {
        unsigned char *str = job->vt->get_status(job);
        if (str && *str) {
          fprintf(fout, "%s", str);
        } else {
          fprintf(fout, "&nbsp;");
        }
        xfree(str);
      } else {
        fprintf(fout, "&nbsp;");
      }
      fprintf(fout, "</td></tr>\n");
    }
    fprintf(fout, "</table>\n");
  }

  if (phr->role == USER_ROLE_ADMIN
      && opcaps_check(phr->caps, OPCAP_CONTROL_CONTEST) >= 0) {
    html_start_form(fout, 1, phr->self_url, phr->hidden_vars);
    fprintf(fout, "<table border=\"0\">");

    fprintf(fout,
            "<tr><td>%s:</td><td>%s</td><td>&nbsp;</td><td>&nbsp;</td></tr>\n",
            _("Server time"), ctime(&cs->current_time));

    if (start_time <= 0) {
      fprintf(fout, "<tr><td colspan=\"2\"><b>%s</b></td><td>&nbsp;</td><td>%s</td></tr>\n",
              _("Contest is not started"),
              BUTTON(NEW_SRV_ACTION_START_CONTEST));
    } else {
      fprintf(fout, "<tr><td>%s:</td><td>%s</td><td>&nbsp;</td>",
              _("Contest start time"), ctime(&start_time));
      if (stop_time <= 0) {
        fprintf(fout, "<td>%s</td></tr>\n",
                BUTTON(NEW_SRV_ACTION_STOP_CONTEST));
      } else if (global->enable_continue
                 && (!duration || stop_time < start_time + duration)) {
        fprintf(fout, "<td>%s</td></tr>\n",
                BUTTON(NEW_SRV_ACTION_CONTINUE_CONTEST));
      }
    }

    if (!global->is_virtual && start_time <= 0) {
      fprintf(fout, "<tr><td>%s:</td><td>%s</td>"
              "<td><input type=\"text\" name=\"sched_time\" size=\"16\"/></td>"
              "<td>%s</td></tr>\n",
              _("Planned start time"),
              sched_time <= 0?_("Not set"):ctime(&sched_time),
              BUTTON(NEW_SRV_ACTION_SCHEDULE));
    }

    if (finish_time <= 0) {
      if (duration > 0) {
        duration_str(0, duration, 0, duration_buf, 0);
      } else {
        snprintf(duration_buf, sizeof(duration_buf), "%s", _("Unlimited"));
      }

      fprintf(fout, "<tr><td>%s:</td><td>%s</td>",_("Duration"), duration_buf);
      if ((stop_time <= 0 || global->enable_continue) && !global->is_virtual) {
        fprintf(fout, "<td><input type=\"text\" name=\"dur\" size=\"16\"/></td>"
                "<td>%s</td></tr>\n",
                BUTTON(NEW_SRV_ACTION_CHANGE_DURATION));
      } else {
        fprintf(fout, "<td>&nbsp;</td><td>&nbsp;</td></tr>\n");
      }

      if (duration <= 0 && (stop_time <= 0 || global->enable_continue)
          && !global->is_virtual) {
        fprintf(fout,
                "<tr><td>%s:</td><td>&nbsp;</td>"
                "<td><input type=\"text\" name=\"finish_time\" size=\"16\" /></td>"
                "<td>%s</td></tr>\n",
                _("Finish time"),
                BUTTON(NEW_SRV_ACTION_CHANGE_FINISH_TIME));
      }
    }

    if (!global->is_virtual) {
      if (start_time > 0 && stop_time <= 0 && duration > 0) {
        tmpt = start_time + duration;
        fprintf(fout, "<tr><td>%s:</td><td>%s</td></tr>\n",
                _("Scheduled end time"), ctime(&tmpt));
      } else if (start_time > 0 && stop_time <= 0 && duration <= 0
                 && finish_time > 0) {
        fprintf(fout, "<tr><td>%s:</td><td>%s</td>\n",
                _("Scheduled end time"), ctime(&finish_time));
        fprintf(fout,
                "<td><input type=\"text\" name=\"finish_time\" size=\"16\" /></td>"
                "<td>%s</td></tr>\n",
                BUTTON(NEW_SRV_ACTION_CHANGE_FINISH_TIME));
        fprintf(fout, "</tr>\n");
      } else if (stop_time) {
        fprintf(fout, "<tr><td>%s:</td><td>%s</td></tr>\n",
                _("End time"), ctime(&stop_time));
      }

      if (start_time > 0 && stop_time <= 0 && fog_start_time > 0) {
        fprintf(fout, "<tr><td>%s:</td><td>%s</td></tr>\n",
                _("Standings freeze time"), ctime(&fog_start_time));
      } else if (stop_time > 0 && duration > 0 && global->board_fog_time > 0
                 && global->board_unfog_time > 0 && !cs->standings_updated
                 && cs->current_time < stop_time + global->board_unfog_time) {
        tmpt = stop_time + global->board_unfog_time;
        fprintf(fout, "<tr><td>%s:</td><td>%s</td></tr>\n",
                _("Standings unfreeze time"), ctime(&tmpt));
      }

      if (start_time > 0 && stop_time <= 0 && duration > 0) {
        duration_str(0, cs->current_time, start_time, duration_buf, 0);
        fprintf(fout, "<tr><td>%s:</td><td>%s</td></tr>\n",
                _("Elapsed time"), duration_buf);
        duration_str(0, start_time + duration - cs->current_time, 0,
                     duration_buf, 0);
        fprintf(fout, "<tr><td>%s:</td><td>%s</td></tr>\n",
                _("Remaining time"), duration_buf);
      }
    }
    fprintf(fout, "<tr><td>%s</td><td>%s</td></tr>\n",
            "Contest load time", ctime(&cs->load_time));
    fprintf(fout, "<tr><td>%s</td><td>%s</td></tr>\n",
            "Server start time", ctime(&server_start_time));

    fprintf(fout, "</table></form>\n");

    fprintf(fout, "<hr>\n");

    html_start_form(fout, 1, phr->self_url, phr->hidden_vars);
    fprintf(fout, "%s\n",  BUTTON(NEW_SRV_ACTION_UPDATE_STANDINGS_1));
    fprintf(fout, "%s\n", BUTTON(NEW_SRV_ACTION_RESET_1));
    action = NEW_SRV_ACTION_SUSPEND;
    if (cs->clients_suspended) action = NEW_SRV_ACTION_RESUME;
    fprintf(fout, "%s\n", BUTTON(action));
    action = NEW_SRV_ACTION_TEST_SUSPEND;
    if (cs->testing_suspended) action = NEW_SRV_ACTION_TEST_RESUME;
    fprintf(fout, "%s\n", BUTTON(action));
    if (global->enable_printing) {
      action = NEW_SRV_ACTION_PRINT_SUSPEND;
      if (cs->printing_suspended) action = NEW_SRV_ACTION_PRINT_RESUME;
      fprintf(fout, "%s\n", BUTTON(action));
    }
    if (global->score_system == SCORE_OLYMPIAD && !global->is_virtual) {
      action = NEW_SRV_ACTION_SET_JUDGING_MODE;
      if (!cs->accepting_mode) action = NEW_SRV_ACTION_SET_ACCEPTING_MODE;
      fprintf(fout, "%s\n", BUTTON(action));
    }
    if (global->score_system == SCORE_OLYMPIAD
        && ((!global->is_virtual && !cs->accepting_mode)
            || (global->is_virtual && global->disable_virtual_auto_judge >0))) {
      action = NEW_SRV_ACTION_SET_TESTING_FINISHED_FLAG;
      if (cs->testing_finished)
        action = NEW_SRV_ACTION_CLEAR_TESTING_FINISHED_FLAG;
      fprintf(fout, "%s\n", BUTTON(action));
    }
    if (!cnts->disable_team_password) {
      fprintf(fout, "%s\n", BUTTON(NEW_SRV_ACTION_GENERATE_PASSWORDS_1));
      fprintf(fout, "%s\n", BUTTON(NEW_SRV_ACTION_CLEAR_PASSWORDS_1));
    }
    fprintf(fout, "%s\n", BUTTON(NEW_SRV_ACTION_GENERATE_REG_PASSWORDS_1));
    fprintf(fout, "%s\n", BUTTON(NEW_SRV_ACTION_UPSOLVING_CONFIG_1));
    fprintf(fout, "%s\n", BUTTON(NEW_SRV_ACTION_RELOAD_SERVER));
    fprintf(fout, "</form>\n");
  } else {
    // judge mode
    fprintf(fout, "<table border=\"0\">");

    fprintf(fout,
            "<tr><td>%s:</td><td>%s</td><td>&nbsp;</td><td>&nbsp;</td></tr>\n",
            _("Server time"), ctime(&cs->current_time));

    if (start_time <= 0) {
      fprintf(fout, "<tr><td colspan=\"2\"><b>%s</b></td></tr>\n",
              _("Contest is not started"));
    } else {
      fprintf(fout, "<tr><td>%s:</td><td>%s</td></tr>\n",
              _("Contest start time"), ctime(&start_time));
    }

    if (!global->is_virtual && start_time <= 0) {
      fprintf(fout, "<tr><td>%s:</td><td>%s</td></tr>\n",
              _("Planned start time"),
              sched_time <= 0?_("Not set"):ctime(&sched_time));
    }

    if (finish_time <= 0) {
      if (duration > 0) {
        duration_str(0, duration, 0, duration_buf, 0);
      } else {
        snprintf(duration_buf, sizeof(duration_buf), "%s", _("Unlimited"));
      }

      fprintf(fout, "<tr><td>%s:</td><td>%s</td></tr>\n",
              _("Duration"), duration_buf);
    }

    if (!global->is_virtual) {
      if (start_time > 0 && stop_time <= 0 && duration > 0) {
        tmpt = start_time + duration;
        fprintf(fout, "<tr><td>%s:</td><td>%s</td></tr>\n",
                _("Scheduled end time"), ctime(&tmpt));
      } else if (start_time > 0 && stop_time <= 0 && duration <= 0
                 && finish_time > 0) {
        fprintf(fout, "<tr><td>%s:</td><td>%s</td></tr>\n",
                _("Scheduled end time"), ctime(&finish_time));
      } else if (stop_time) {
        fprintf(fout, "<tr><td>%s:</td><td>%s</td></tr>\n",
                _("End time"), ctime(&stop_time));
      }

      if (start_time > 0 && stop_time <= 0 && fog_start_time > 0) {
        fprintf(fout, "<tr><td>%s:</td><td>%s</td></tr>\n",
                _("Standings freeze time"), ctime(&fog_start_time));
      } else if (stop_time > 0 && duration > 0 && global->board_fog_time > 0
                 && global->board_unfog_time > 0 && !cs->standings_updated
                 && cs->current_time < stop_time + global->board_unfog_time) {
        tmpt = stop_time + global->board_unfog_time;
        fprintf(fout, "<tr><td>%s:</td><td>%s</td></tr>\n",
                _("Standings unfreeze time"), ctime(&tmpt));
      }

      if (start_time > 0 && stop_time <= 0 && duration > 0) {
        duration_str(0, cs->current_time, start_time, duration_buf, 0);
        fprintf(fout, "<tr><td>%s:</td><td>%s</td></tr>\n",
                _("Elapsed time"), duration_buf);
        duration_str(0, start_time + duration - cs->current_time, 0,
                     duration_buf, 0);
        fprintf(fout, "<tr><td>%s:</td><td>%s</td></tr>\n",
                _("Remaining time"), duration_buf);
      }
    }
    fprintf(fout, "</table>\n");
  }

  ns_write_priv_all_runs(fout, phr, cnts, extra,
                         filter_first_run_set, filter_first_run,
                         filter_last_run_set, filter_last_run,
                         filter_expr);

  if (opcaps_check(phr->caps, OPCAP_SUBMIT_RUN) >= 0) {
    if (!prob) {
      // no problem is selected yet
      fprintf(fout, "<hr><a name=\"submit\"></a><%s>%s</%s>\n",
              /*cnts->priv_head_style*/ "h2",
              _("View the problem statement and send a submission"),
              /*cnts->priv_head_style*/ "h2");
      html_start_form(fout, 0, phr->self_url, phr->hidden_vars);
      fprintf(fout, "<table>\n");
      fprintf(fout, "<tr><td>%s:</td><td><select name=\"problem\">",
              _("Problem"));
      for (x = 1; x <= cs->max_prob; x++) {
        if (!(prob = cs->probs[x])) continue;
        fprintf(fout, "<option value=\"%d\">%s - %s</option>",
                x, prob->short_name, ARMOR(prob->long_name));
        /*
        if (prob->variant_num <= 0) {
        } else {
          for (y = 1; y <= prob->variant_num; y++) {
            fprintf(fout, "<option value=\"%d_%d\">%s - %s, %s %d</option>",
                    x, y, prob->short_name,  ARMOR(prob->long_name),
                    _("Variant"), y);
          }
        }
        */
      }
      fprintf(fout, "</select></td><td>%s</td></tr></table></form>\n",
              ns_submit_button(bb, sizeof(bb), 0,
                               NEW_SRV_ACTION_PRIV_SUBMIT_PAGE,
                               _("Select problem")));
      prob = 0;
    } else {
      // a problem is already selected
      // prob and variant have correct values
      if (variant > 0) {
        fprintf(fout, "<hr><a name=\"submit\"></a><%s>%s %s-%s (%s %d)</%s>\n",
                /*cnts->team_head_style*/ "h2", _("Submit a solution for"),
                prob->short_name, ARMOR(prob->long_name), _("Variant"), variant,
                /*cnts->team_head_style*/ "h2");
      } else {
        fprintf(fout, "<hr><a name=\"submit\"></a><%s>%s %s-%s</%s>\n",
                /*cnts->team_head_style*/ "h2", _("Submit a solution for"),
                prob->short_name,  ARMOR(prob->long_name),
                /*cnts->team_head_style*/ "h2");
      }

      /* FIXME: handle problem XML */

      /* put problem statement */
      if (prob->statement_file[0]) {
        if (variant > 0) {
          prepare_insert_variant_num(variant_stmt_file,
                                     sizeof(variant_stmt_file),
                                     prob->statement_file, variant);
          pw = &cs->prob_extras[prob->id].v_stmts[variant];
          pw_path = variant_stmt_file;
        } else {
          pw = &cs->prob_extras[prob->id].stmt;
          pw_path = prob->statement_file;
        }
        watched_file_update(pw, pw_path, cs->current_time);
        if (!pw->text) {
          fprintf(fout, "<big><font color=\"red\"><p>%s</p></font></big>\n",
                  _("The problem statement is not available"));
        } else {
          if (prob->type == PROB_TYPE_CUSTOM) {
            html_start_form(fout, 2, phr->self_url, phr->hidden_vars);
            skip_start_form = 1;
          }
          fprintf(fout, "%s", pw->text);
        }
      }
      alternatives = 0;
      if ((prob->type == PROB_TYPE_SELECT_ONE
           || prob->type == PROB_TYPE_SELECT_MANY)
          && prob->alternatives_file[0]) {
        if (variant > 0) {
          prepare_insert_variant_num(variant_stmt_file,
                                     sizeof(variant_stmt_file),
                                     prob->alternatives_file, variant);
          pw = &cs->prob_extras[prob->id].v_alts[variant];
          pw_path = variant_stmt_file;
        } else {
          pw = &cs->prob_extras[prob->id].alt;
          pw_path = prob->alternatives_file;
        }
        watched_file_update(pw, pw_path, cs->current_time);
        alternatives = pw->text;
      }

      if (!skip_start_form) {
        html_start_form(fout, 2, phr->self_url, phr->hidden_vars);
      }
      if (variant <= 0) {
        html_hidden(fout, "problem", "%d", prob->id);
      } else {
        html_hidden(fout, "problem", "%d_%d", prob->id, variant);
      }
      fprintf(fout, "<table>\n");
      if (!prob->type) {
        fprintf(fout, "<tr><td>%s:</td><td>", _("Language"));
        fprintf(fout, "<select name=\"lang_id\"><option value=\"\">\n");
        for (i = 1; i <= cs->max_lang; i++) {
          if (!cs->langs[i]) continue;
          fprintf(fout, "<option value=\"%d\">%s - %s</option>\n",
                  i, cs->langs[i]->short_name, ARMOR(cs->langs[i]->long_name));
        }
        fprintf(fout, "</select></td></tr>\n");

        if (global->enable_eoln_select > 0) {
          fprintf(fout, "<tr><td%s>%s:</td><td%s><select name=\"eoln_type\"%s>",
                  "", "EOLN Type", "", "");
          fprintf(fout, "<option value=\"0\"></option>");
          fprintf(fout, "<option value=\"1\"%s>LF (Unix/MacOS)</option>", "");
          fprintf(fout, "<option value=\"2\"%s>CRLF (Windows/DOS)</option>", "");
          fprintf(fout, "</select></td></tr>\n");
        }
      }

      switch (prob->type) {
      case PROB_TYPE_STANDARD:
      case PROB_TYPE_OUTPUT_ONLY:
      case PROB_TYPE_TESTS:
        fprintf(fout, "<tr><td>%s</td><td><input type=\"file\" name=\"file\"/></td></tr>\n", _("File"));
        break;
      case PROB_TYPE_SHORT_ANSWER:
        fprintf(fout, "<tr><td>%s</td><td><input type=\"text\" name=\"file\"/></td></tr>\n", _("Answer"));
        break;
      case PROB_TYPE_TEXT_ANSWER:
        fprintf(fout, "<tr><td colspan=\"2\"><textarea name=\"file\" rows=\"20\" cols=\"60\"></textarea></td></tr>\n");
        break;
      case PROB_TYPE_SELECT_ONE:
        /* FIXME: handle problem XML */
        if (alternatives) {
          write_alternatives_file(fout, 1, alternatives, -1, 0, 0, 0, "b0");
        } else if (prob->alternative) {
          for (i = 0; prob->alternative[i]; i++) {
            fprintf(fout, "<tr><td>%d</td><td><input type=\"radio\" name=\"file\" value=\"%d\"/></td><td>%s</td></tr>\n", i + 1, i + 1, prob->alternative[i]);
          }
        }
        break;
      case PROB_TYPE_SELECT_MANY:
        if (alternatives) {
          write_alternatives_file(fout, 0, alternatives, -1, 0, 0, 0, "b0");
        } else if (prob->alternative) {
          for (i = 0; prob->alternative[i]; i++) {
            fprintf(fout, "<tr><td>%d</td><td><input type=\"checkbox\" name=\"ans_%d\"/></td><td>%s</td></tr>\n", i + 1, i + 1, prob->alternative[i]);
          }
        }
        break;
      case PROB_TYPE_CUSTOM:    /* form is a part of problem statement */
        break;
      }
      fprintf(fout, "<tr><td>%s</td><td>%s</td></tr></table></form>\n",
              _("Send!"), BUTTON(NEW_SRV_ACTION_SUBMIT_RUN));
     
      fprintf(fout, "<hr><a name=\"submit\"></a><%s>%s</%s>\n",
              /*cnts->team_head_style*/ "h2", _("Select another problem"),
              /*cnts->team_head_style*/ "h2");

      html_start_form(fout, 0, phr->self_url, phr->hidden_vars);
      fprintf(fout, "<table>\n");
      fprintf(fout, "<tr><td>%s:</td><td><select name=\"problem\">",
              _("Problem"));
      for (x = 1; x <= cs->max_prob; x++) {
        if (!(prob = cs->probs[x])) continue;
        if (prob->variant_num <= 0) {
          fprintf(fout, "<option value=\"%d\">%s - %s</option>",
                  x, prob->short_name, ARMOR(prob->long_name));
        } else {
          for (y = 1; y <= prob->variant_num; y++) {
            fprintf(fout, "<option value=\"%d_%d\">%s - %s, %s %d</option>",
                    x, y, prob->short_name, ARMOR(prob->long_name),
                    _("Variant"), y);
          }
        }
      }
      fprintf(fout, "</select></td><td>%s</td></tr></table></form>\n",
              ns_submit_button(bb, sizeof(bb), 0, NEW_SRV_ACTION_MAIN_PAGE,
                               _("Select problem")));
      prob = 0;
    }
  }

  if (opcaps_check(phr->caps, OPCAP_VIEW_CLAR) >= 0) {
    ns_write_all_clars(fout, phr, cnts, extra, filter_mode_clar,
                       filter_first_clar_str, filter_last_clar_str);
  }

  if (opcaps_check(phr->caps, OPCAP_NEW_MESSAGE) >= 0) {
    fprintf(fout, "<hr><h2>%s</h2>", _("Compose a message to all participants"));
    html_start_form(fout, 1, phr->self_url, phr->hidden_vars);
    fprintf(fout, "<table>\n"
            "<tr>"
            "<td>%s:</td>"
            "<td><input type=\"text\" size=\"16\" name=\"msg_dest_id\"/></td>"
            "</tr>\n"
            "<tr>"
            "<td>%s:</td>"
            "<td><input type=\"text\" size=\"32\" name=\"msg_dest_login\"/></td>"
            "</tr>\n"
            "<tr>"
            "<td>%s:</td>"
            "<td><input type=\"text\" size=\"64\" name=\"msg_subj\"/></td>"
            "</tr>\n",
            _("To user id"),
            _("To user login"),
            _("Subject"));
    if (start_time <= 0) {
      fprintf(fout, "<tr><td>%s</td><td><select name=\"msg_hide_flag\"><option value=\"0\">NO</option><option value=\"1\">YES</option></select></td></tr>\n",
              _("Do not show before the contest starts?"));
    }
    fprintf(fout, "</table>\n"
            "<p><textarea name=\"msg_text\" rows=\"20\" cols=\"60\">"
            "</textarea></p>"
            "<p>%s\n</form>\n",
            BUTTON(NEW_SRV_ACTION_PRIV_SUBMIT_CLAR));
  }

  /* change the password */
  fprintf(fout, "<hr><a name=\"chgpasswd\"></a>\n<%s>%s</%s>\n",
          /*cnts->priv_head_style*/ "h2",
          _("Change password"),
          /*cnts->team_head_style*/ "h2");
  html_start_form(fout, 1, phr->self_url, phr->hidden_vars);

  fprintf(fout, "<table>\n"
          "<tr><td>%s:</td><td><input type=\"password\" name=\"oldpasswd\" size=\"16\"/></td></tr>\n"
          "<tr><td>%s:</td><td><input type=\"password\" name=\"newpasswd1\" size=\"16\"/></td></tr>\n"
          "<tr><td>%s:</td><td><input type=\"password\" name=\"newpasswd2\" size=\"16\"/></td></tr>\n"
          "<tr><td colspan=\"2\">%s</td></tr>\n"
          "</table></form>",
          _("Old password"),
          _("New password"), _("Retype new password"),
          BUTTON(NEW_SRV_ACTION_CHANGE_PASSWORD));

#if CONF_HAS_LIBINTL - 0 == 1
  if (cs->global->enable_l10n) {
    fprintf(fout, "<hr><a name=\"chglanguage\"></a><%s>%s</%s>\n",
            cnts->team_head_style, _("Change language"),
            cnts->team_head_style);
    html_start_form(fout, 1, phr->self_url, phr->hidden_vars);
    fprintf(fout, "<table><tr><td>%s</td><td>", _("Change language"));
    l10n_html_locale_select(fout, phr->locale_id);
    fprintf(fout, "</td><td>%s</td></tr></table></form>\n",
            BUTTON(NEW_SRV_ACTION_CHANGE_LANGUAGE));
  }
#endif /* CONF_HAS_LIBINTL */

  if (1 /*cs->global->show_generation_time*/) {
  gettimeofday(&phr->timestamp2, 0);
  tdiff = ((long long) phr->timestamp2.tv_sec) * 1000000;
  tdiff += phr->timestamp2.tv_usec;
  tdiff -= ((long long) phr->timestamp1.tv_sec) * 1000000;
  tdiff -= phr->timestamp1.tv_usec;
  fprintf(fout, "<div class=\"dotted\"><p%s>%s: %lld %s</p></div>",
          cnts->team_par_style,
          _("Page generation time"), tdiff / 1000,
          _("msec"));
  }

  ns_footer(fout, extra->footer_txt, extra->copyright_txt, phr->locale_id);
  l10n_setlocale(0);
  html_armor_free(&ab);
}

static void
priv_reload_server_2(
        FILE *fout,
        struct http_request_info *phr,
        const struct contest_desc *cnts,
        struct contest_extra *extra)
{
}

typedef void (*action_handler_t)(FILE *fout,
                                 struct http_request_info *phr,
                                 const struct contest_desc *cnts,
                                 struct contest_extra *extra);

static action_handler_t actions_table[NEW_SRV_ACTION_LAST] =
{
  [NEW_SRV_ACTION_VIEW_USERS] = priv_view_users_page,
  [NEW_SRV_ACTION_USERS_REMOVE_REGISTRATIONS] = priv_generic_operation,
  [NEW_SRV_ACTION_USERS_SET_PENDING] = priv_generic_operation,
  [NEW_SRV_ACTION_USERS_SET_OK] = priv_generic_operation,
  [NEW_SRV_ACTION_USERS_SET_REJECTED] = priv_generic_operation,
  [NEW_SRV_ACTION_USERS_SET_INVISIBLE] = priv_generic_operation,
  [NEW_SRV_ACTION_USERS_CLEAR_INVISIBLE] = priv_generic_operation,
  [NEW_SRV_ACTION_USERS_SET_BANNED] = priv_generic_operation,
  [NEW_SRV_ACTION_USERS_CLEAR_BANNED] = priv_generic_operation,
  [NEW_SRV_ACTION_USERS_SET_LOCKED] = priv_generic_operation,
  [NEW_SRV_ACTION_USERS_CLEAR_LOCKED] = priv_generic_operation,
  [NEW_SRV_ACTION_USERS_SET_INCOMPLETE] = priv_generic_operation,
  [NEW_SRV_ACTION_USERS_CLEAR_INCOMPLETE] = priv_generic_operation,
  [NEW_SRV_ACTION_USERS_SET_DISQUALIFIED] = priv_generic_operation,
  [NEW_SRV_ACTION_USERS_CLEAR_DISQUALIFIED] = priv_generic_operation,
  [NEW_SRV_ACTION_USERS_ADD_BY_LOGIN] = priv_generic_operation,
  [NEW_SRV_ACTION_USERS_ADD_BY_USER_ID] = priv_generic_operation,
  [NEW_SRV_ACTION_PRIV_USERS_VIEW] = priv_view_priv_users_page,
  [NEW_SRV_ACTION_PRIV_USERS_REMOVE] = priv_generic_operation,
  [NEW_SRV_ACTION_PRIV_USERS_ADD_OBSERVER] = priv_generic_operation,
  [NEW_SRV_ACTION_PRIV_USERS_DEL_OBSERVER] = priv_generic_operation,
  [NEW_SRV_ACTION_PRIV_USERS_ADD_EXAMINER] = priv_generic_operation,
  [NEW_SRV_ACTION_PRIV_USERS_DEL_EXAMINER] = priv_generic_operation,
  [NEW_SRV_ACTION_PRIV_USERS_ADD_CHIEF_EXAMINER] = priv_generic_operation,
  [NEW_SRV_ACTION_PRIV_USERS_DEL_CHIEF_EXAMINER] = priv_generic_operation,
  [NEW_SRV_ACTION_PRIV_USERS_ADD_COORDINATOR] = priv_generic_operation,
  [NEW_SRV_ACTION_PRIV_USERS_DEL_COORDINATOR] = priv_generic_operation,
  [NEW_SRV_ACTION_PRIV_USERS_ADD_BY_LOGIN] = priv_generic_operation,
  [NEW_SRV_ACTION_PRIV_USERS_ADD_BY_USER_ID] = priv_generic_operation,
  [NEW_SRV_ACTION_START_CONTEST] = priv_generic_operation,
  [NEW_SRV_ACTION_STOP_CONTEST] = priv_generic_operation,
  [NEW_SRV_ACTION_CONTINUE_CONTEST] = priv_generic_operation,
  [NEW_SRV_ACTION_SCHEDULE] = priv_generic_operation,
  [NEW_SRV_ACTION_CHANGE_DURATION] = priv_generic_operation,
  [NEW_SRV_ACTION_CHANGE_FINISH_TIME] = priv_generic_operation,
  [NEW_SRV_ACTION_SUSPEND] = priv_generic_operation,
  [NEW_SRV_ACTION_RESUME] = priv_generic_operation,
  [NEW_SRV_ACTION_TEST_SUSPEND] = priv_generic_operation,
  [NEW_SRV_ACTION_TEST_RESUME] = priv_generic_operation,
  [NEW_SRV_ACTION_PRINT_SUSPEND] = priv_generic_operation,
  [NEW_SRV_ACTION_PRINT_RESUME] = priv_generic_operation,
  [NEW_SRV_ACTION_SET_JUDGING_MODE] = priv_generic_operation,
  [NEW_SRV_ACTION_SET_ACCEPTING_MODE] = priv_generic_operation,
  [NEW_SRV_ACTION_SET_TESTING_FINISHED_FLAG] = priv_generic_operation,
  [NEW_SRV_ACTION_CLEAR_TESTING_FINISHED_FLAG] = priv_generic_operation,
  [NEW_SRV_ACTION_SQUEEZE_RUNS] = priv_generic_operation,
  [NEW_SRV_ACTION_RESET_FILTER] = priv_generic_operation,
  [NEW_SRV_ACTION_RESET_CLAR_FILTER] = priv_generic_operation,
  [NEW_SRV_ACTION_VIEW_SOURCE] = priv_generic_page,
  [NEW_SRV_ACTION_VIEW_REPORT] = priv_generic_page,
  [NEW_SRV_ACTION_PRIV_DOWNLOAD_RUN] = priv_generic_page,
  [NEW_SRV_ACTION_STANDINGS] = priv_generic_page,
  [NEW_SRV_ACTION_CHANGE_LANGUAGE] = priv_generic_operation,
  [NEW_SRV_ACTION_SUBMIT_RUN] = priv_generic_operation,
  [NEW_SRV_ACTION_PRIV_SUBMIT_CLAR] = priv_generic_operation,
  [NEW_SRV_ACTION_PRIV_SUBMIT_RUN_COMMENT] = priv_generic_operation,
  [NEW_SRV_ACTION_CLAR_REPLY] = priv_generic_operation,
  [NEW_SRV_ACTION_CLAR_REPLY_ALL] = priv_generic_operation,
  [NEW_SRV_ACTION_CLAR_REPLY_READ_PROBLEM] = priv_generic_operation,
  [NEW_SRV_ACTION_CLAR_REPLY_NO_COMMENTS] = priv_generic_operation,
  [NEW_SRV_ACTION_CLAR_REPLY_YES] = priv_generic_operation,
  [NEW_SRV_ACTION_CLAR_REPLY_NO] = priv_generic_operation,
  [NEW_SRV_ACTION_VIEW_CLAR] = priv_generic_page,
  [NEW_SRV_ACTION_RELOAD_SERVER] = priv_generic_operation,
  [NEW_SRV_ACTION_CHANGE_STATUS] = priv_generic_operation,
  [NEW_SRV_ACTION_CHANGE_RUN_STATUS] = priv_generic_operation,
  [NEW_SRV_ACTION_REJUDGE_DISPLAYED_1] = priv_generic_page,
  [NEW_SRV_ACTION_REJUDGE_DISPLAYED_2] = priv_generic_operation,
  [NEW_SRV_ACTION_FULL_REJUDGE_DISPLAYED_1] = priv_generic_page,
  [NEW_SRV_ACTION_FULL_REJUDGE_DISPLAYED_2] = priv_generic_operation,
  [NEW_SRV_ACTION_REJUDGE_PROBLEM_1] = priv_generic_page,
  [NEW_SRV_ACTION_REJUDGE_PROBLEM_2] = priv_generic_operation,
  [NEW_SRV_ACTION_REJUDGE_SUSPENDED_1] = priv_generic_page,
  [NEW_SRV_ACTION_REJUDGE_SUSPENDED_2] = priv_generic_operation,
  [NEW_SRV_ACTION_REJUDGE_ALL_1] = priv_generic_page,
  [NEW_SRV_ACTION_REJUDGE_ALL_2] = priv_generic_operation,
  [NEW_SRV_ACTION_COMPARE_RUNS] = priv_generic_page,
  [NEW_SRV_ACTION_VIEW_TEST_INPUT] = priv_generic_page,
  [NEW_SRV_ACTION_VIEW_TEST_ANSWER] = priv_generic_page,
  [NEW_SRV_ACTION_VIEW_TEST_INFO] = priv_generic_page,
  [NEW_SRV_ACTION_VIEW_TEST_OUTPUT] = priv_generic_page,
  [NEW_SRV_ACTION_VIEW_TEST_ERROR] = priv_generic_page,
  [NEW_SRV_ACTION_VIEW_TEST_CHECKER] = priv_generic_page,
  [NEW_SRV_ACTION_VIEW_AUDIT_LOG] = priv_generic_page,
  [NEW_SRV_ACTION_UPDATE_STANDINGS_2] = priv_generic_operation,
  [NEW_SRV_ACTION_UPDATE_STANDINGS_1] = priv_generic_page,
  [NEW_SRV_ACTION_RESET_2] = priv_generic_operation,
  [NEW_SRV_ACTION_RESET_1] = priv_generic_page,
  [NEW_SRV_ACTION_GENERATE_PASSWORDS_1] = priv_generic_page,
  [NEW_SRV_ACTION_GENERATE_PASSWORDS_2] = priv_generic_operation,
  [NEW_SRV_ACTION_GENERATE_REG_PASSWORDS_1] = priv_generic_page,
  [NEW_SRV_ACTION_GENERATE_REG_PASSWORDS_2] = priv_generic_operation,
  [NEW_SRV_ACTION_CLEAR_PASSWORDS_1] = priv_generic_page,
  [NEW_SRV_ACTION_CLEAR_PASSWORDS_2] = priv_generic_operation,
  [NEW_SRV_ACTION_VIEW_CNTS_PWDS] = priv_generic_page,
  [NEW_SRV_ACTION_VIEW_REG_PWDS] = priv_generic_page,
  [NEW_SRV_ACTION_VIEW_USER_INFO] = priv_generic_page,
  [NEW_SRV_ACTION_NEW_RUN_FORM] = priv_generic_page,
  [NEW_SRV_ACTION_VIEW_USER_DUMP] = priv_generic_page,
  [NEW_SRV_ACTION_USER_CHANGE_STATUS] = priv_generic_operation,
  [NEW_SRV_ACTION_NEW_RUN] = priv_generic_operation,
  [NEW_SRV_ACTION_CHANGE_RUN_USER_ID] = priv_generic_operation,
  [NEW_SRV_ACTION_CHANGE_RUN_USER_LOGIN] = priv_generic_operation,
  [NEW_SRV_ACTION_CHANGE_RUN_PROB_ID] = priv_generic_operation,
  [NEW_SRV_ACTION_CHANGE_RUN_VARIANT] = priv_generic_operation,
  [NEW_SRV_ACTION_CHANGE_RUN_LANG_ID] = priv_generic_operation,
  [NEW_SRV_ACTION_CHANGE_RUN_IS_IMPORTED] = priv_generic_operation,
  [NEW_SRV_ACTION_CHANGE_RUN_IS_HIDDEN] = priv_generic_operation,
  [NEW_SRV_ACTION_CHANGE_RUN_IS_EXAMINABLE] = priv_generic_operation,
  [NEW_SRV_ACTION_CHANGE_RUN_IS_READONLY] = priv_generic_operation,
  [NEW_SRV_ACTION_CHANGE_RUN_IS_MARKED] = priv_generic_operation,
  [NEW_SRV_ACTION_CHANGE_RUN_IS_SAVED] = priv_generic_operation,
  [NEW_SRV_ACTION_CHANGE_RUN_TEST] = priv_generic_operation,
  [NEW_SRV_ACTION_CHANGE_RUN_SCORE] = priv_generic_operation,
  [NEW_SRV_ACTION_CHANGE_RUN_SCORE_ADJ] = priv_generic_operation,
  [NEW_SRV_ACTION_CHANGE_RUN_PAGES] = priv_generic_operation,
  [NEW_SRV_ACTION_CLEAR_RUN] = priv_generic_operation,
  [NEW_SRV_ACTION_PRINT_RUN] = priv_generic_operation,
  [NEW_SRV_ACTION_ISSUE_WARNING] = priv_generic_operation,
  [NEW_SRV_ACTION_SET_DISQUALIFICATION] = priv_generic_operation,
  [NEW_SRV_ACTION_LOGOUT] = priv_logout,
  [NEW_SRV_ACTION_CHANGE_PASSWORD] = priv_change_password,
  [NEW_SRV_ACTION_VIEW_USER_REPORT] = priv_generic_page,
  [NEW_SRV_ACTION_DOWNLOAD_ARCHIVE_1] = priv_generic_page,
  [NEW_SRV_ACTION_DOWNLOAD_ARCHIVE_2] = priv_generic_page,
  [NEW_SRV_ACTION_UPLOAD_RUNLOG_CSV_1] = priv_generic_page,
  [NEW_SRV_ACTION_UPLOAD_RUNLOG_CSV_2] = priv_generic_page, /// FIXME: do audit logging
  [NEW_SRV_ACTION_VIEW_RUNS_DUMP] = priv_generic_page,
  [NEW_SRV_ACTION_EXPORT_XML_RUNS] = priv_generic_page,
  [NEW_SRV_ACTION_WRITE_XML_RUNS] = priv_generic_page,
  [NEW_SRV_ACTION_WRITE_XML_RUNS_WITH_SRC] = priv_generic_page,
  [NEW_SRV_ACTION_UPLOAD_RUNLOG_XML_1] = priv_generic_page,
  [NEW_SRV_ACTION_UPLOAD_RUNLOG_XML_2] = priv_generic_page,
  [NEW_SRV_ACTION_CLEAR_DISPLAYED_1] = priv_generic_page,
  [NEW_SRV_ACTION_CLEAR_DISPLAYED_2] = priv_generic_operation,
  [NEW_SRV_ACTION_IGNORE_DISPLAYED_1] = priv_generic_page,
  [NEW_SRV_ACTION_IGNORE_DISPLAYED_2] = priv_generic_operation,
  [NEW_SRV_ACTION_DISQUALIFY_DISPLAYED_1] = priv_generic_page,
  [NEW_SRV_ACTION_DISQUALIFY_DISPLAYED_2] = priv_generic_operation,
  [NEW_SRV_ACTION_UPSOLVING_CONFIG_1] = priv_generic_page,
  [NEW_SRV_ACTION_UPSOLVING_CONFIG_2] = priv_generic_operation,
  [NEW_SRV_ACTION_UPSOLVING_CONFIG_3] = priv_generic_operation,
  [NEW_SRV_ACTION_UPSOLVING_CONFIG_4] = priv_generic_operation,
  [NEW_SRV_ACTION_EXAMINERS_PAGE] = priv_generic_page,
  [NEW_SRV_ACTION_ASSIGN_CHIEF_EXAMINER] = priv_generic_operation,
  [NEW_SRV_ACTION_ASSIGN_EXAMINER] = priv_generic_operation,
  [NEW_SRV_ACTION_UNASSIGN_EXAMINER] = priv_generic_operation,
  [NEW_SRV_ACTION_TOGGLE_VISIBILITY] = priv_generic_operation,
  [NEW_SRV_ACTION_TOGGLE_BAN] = priv_generic_operation,
  [NEW_SRV_ACTION_TOGGLE_LOCK] = priv_generic_operation,
  [NEW_SRV_ACTION_TOGGLE_INCOMPLETENESS] = priv_generic_operation,
  [NEW_SRV_ACTION_VIEW_ONLINE_USERS] = priv_generic_page,
  [NEW_SRV_ACTION_PRINT_USER_PROTOCOL] = priv_generic_page,
  [NEW_SRV_ACTION_PRINT_USER_FULL_PROTOCOL] = priv_generic_page,
  [NEW_SRV_ACTION_PRINT_UFC_PROTOCOL] = priv_generic_page,
  [NEW_SRV_ACTION_FORCE_START_VIRTUAL] = priv_generic_operation,
  [NEW_SRV_ACTION_PRINT_SELECTED_USER_PROTOCOL] = priv_generic_page,
  [NEW_SRV_ACTION_PRINT_SELECTED_USER_FULL_PROTOCOL] = priv_generic_page,
  [NEW_SRV_ACTION_PRINT_SELECTED_UFC_PROTOCOL] = priv_generic_page,
  [NEW_SRV_ACTION_PRINT_PROBLEM_PROTOCOL] = priv_generic_page,
  [NEW_SRV_ACTION_ASSIGN_CYPHERS_1] = priv_generic_page,
  [NEW_SRV_ACTION_ASSIGN_CYPHERS_2] = priv_generic_page,
  [NEW_SRV_ACTION_VIEW_EXAM_INFO] = priv_generic_page,
  [NEW_SRV_ACTION_PRIV_SUBMIT_PAGE] = priv_submit_page,
  [NEW_SRV_ACTION_GET_FILE] = priv_get_file,
  [NEW_SRV_ACTION_PRIO_FORM] = priv_generic_page,
  [NEW_SRV_ACTION_SET_PRIORITIES] = priv_generic_operation,
  [NEW_SRV_ACTION_PRIV_SUBMIT_RUN_COMMENT_AND_IGNORE] = priv_generic_operation,
  [NEW_SRV_ACTION_PRIV_SUBMIT_RUN_COMMENT_AND_OK] = priv_generic_operation,
  [NEW_SRV_ACTION_VIEW_USER_IPS] = priv_generic_page,
  [NEW_SRV_ACTION_VIEW_IP_USERS] = priv_generic_page,
  [NEW_SRV_ACTION_PRIV_SUBMIT_RUN_JUST_IGNORE] = priv_generic_operation,
  [NEW_SRV_ACTION_PRIV_SUBMIT_RUN_JUST_OK] = priv_generic_operation,
  [NEW_SRV_ACTION_PRIV_SET_RUN_REJECTED] = priv_generic_operation,
  [NEW_SRV_ACTION_VIEW_TESTING_QUEUE] = priv_generic_page,
  [NEW_SRV_ACTION_TESTING_DELETE] = priv_generic_operation,
  [NEW_SRV_ACTION_TESTING_UP] = priv_generic_operation,
  [NEW_SRV_ACTION_TESTING_DOWN] = priv_generic_operation,
  [NEW_SRV_ACTION_TESTING_DELETE_ALL] = priv_generic_operation,
  [NEW_SRV_ACTION_TESTING_UP_ALL] = priv_generic_operation,
  [NEW_SRV_ACTION_TESTING_DOWN_ALL] = priv_generic_operation,
  [NEW_SRV_ACTION_MARK_DISPLAYED_2] = priv_generic_operation,
  [NEW_SRV_ACTION_UNMARK_DISPLAYED_2] = priv_generic_operation,
  [NEW_SRV_ACTION_SET_STAND_FILTER] = priv_generic_operation,
  [NEW_SRV_ACTION_RESET_STAND_FILTER] = priv_generic_operation,
  [NEW_SRV_ACTION_ADMIN_CONTEST_SETTINGS] = priv_generic_page,
  [NEW_SRV_ACTION_ADMIN_CHANGE_ONLINE_VIEW_SOURCE] = priv_generic_operation,
  [NEW_SRV_ACTION_ADMIN_CHANGE_ONLINE_VIEW_REPORT] = priv_generic_operation,
  [NEW_SRV_ACTION_ADMIN_CHANGE_ONLINE_VIEW_JUDGE_SCORE] = priv_generic_operation,
  [NEW_SRV_ACTION_ADMIN_CHANGE_ONLINE_FINAL_VISIBILITY] = priv_generic_operation,
  [NEW_SRV_ACTION_RELOAD_SERVER_2] = priv_reload_server_2,
  [NEW_SRV_ACTION_CHANGE_RUN_FIELDS] = priv_generic_operation,
  [NEW_SRV_ACTION_PRIV_EDIT_CLAR_PAGE] = priv_generic_page,
  [NEW_SRV_ACTION_PRIV_EDIT_CLAR_ACTION] = priv_generic_operation,
  [NEW_SRV_ACTION_PRIV_EDIT_RUN_PAGE] = priv_generic_page,
  [NEW_SRV_ACTION_PRIV_EDIT_RUN_ACTION] = priv_generic_operation, ///
  [NEW_SRV_ACTION_PING] = priv_generic_page,
  [NEW_SRV_ACTION_SUBMIT_RUN_BATCH] = priv_generic_page,
};

static unsigned char *
read_file_range(
        const unsigned char *path,
        long long begpos,
        long long endpos)
{
  FILE *f = NULL;
  unsigned char *str = NULL, *s;
  int count, c;

  if (begpos < 0 || endpos < 0 || begpos > endpos || (endpos - begpos) > 16777216LL) return NULL;
  count = endpos - begpos;
  if (!(f = fopen(path, "rb"))) return NULL;
  if (fseek(f, begpos, SEEK_SET) < 0) {
    fclose(f);
    return NULL;
  }
  s = str = xmalloc(count + 1);
  while ((c = getc(f)) != EOF && count) {
    *s++ = c;
    --count;
  }
  *s = 0;
  fclose(f); f = NULL;
  return str;
}

static void
privileged_entry_point(
        FILE *fout,
        struct http_request_info *phr)
{
  int r;
  opcap_t caps;
  const struct contest_desc *cnts = 0;
  struct contest_extra *extra = 0;
  time_t cur_time = time(0);
  unsigned char hid_buf[1024];
  struct teamdb_db_callbacks callbacks;
  long long log_file_pos_1 = -1LL;
  long long log_file_pos_2 = -1LL;
  unsigned char *msg = NULL;

  if (phr->action == NEW_SRV_ACTION_COOKIE_LOGIN)
    return privileged_page_cookie_login(fout, phr);

  if (!phr->session_id || phr->action == NEW_SRV_ACTION_LOGIN_PAGE)
    return privileged_page_login(fout, phr);

  // validate cookie
  if (ns_open_ul_connection(phr->fw_state) < 0)
    return ns_html_err_ul_server_down(fout, phr, 1, 0);
  if ((r = userlist_clnt_get_cookie(ul_conn, ULS_PRIV_GET_COOKIE,
                                    &phr->ip, phr->ssl_flag,
                                    phr->session_id,
                                    phr->client_key,
                                    &phr->user_id, &phr->contest_id,
                                    &phr->locale_id, 0, &phr->role, 0, 0, 0,
                                    &phr->login, &phr->name)) < 0) {
    switch (-r) {
    case ULS_ERR_NO_COOKIE:
      return ns_html_err_inv_session(fout, phr, 1,
                                     "priv_login failed: %s",
                                     userlist_strerror(-r));
    case ULS_ERR_DISCONNECT:
      return ns_html_err_ul_server_down(fout, phr, 1, 0);
    default:
      return ns_html_err_internal_error(fout, phr, 1, "priv_login failed: %s",
                                        userlist_strerror(-r));
    }
  }

  if (phr->contest_id < 0 || contests_get(phr->contest_id, &cnts) < 0 || !cnts)
    return ns_html_err_no_perm(fout, phr, 1, "invalid contest_id %d",
                               phr->contest_id);
  if (!cnts->managed)
    return ns_html_err_inv_param(fout, phr, 1, "contest is not managed");
  extra = ns_get_contest_extra(phr->contest_id);
  ASSERT(extra);

  // analyze IP limitations
  if (phr->role == USER_ROLE_ADMIN) {
    // as for the master program
    if (!contests_check_master_ip(phr->contest_id, &phr->ip, phr->ssl_flag))
      return ns_html_err_no_perm(fout, phr, 1, "%s://%s is not allowed for MASTER for contest %d", ns_ssl_flag_str[phr->ssl_flag],
                                 xml_unparse_ipv6(&phr->ip), phr->contest_id);
  } else {
    // as for judge program
    if (!contests_check_judge_ip(phr->contest_id, &phr->ip, phr->ssl_flag))
      return ns_html_err_no_perm(fout, phr, 1, "%s://%s is not allowed for MASTER for contest %d", ns_ssl_flag_str[phr->ssl_flag], xml_unparse_ipv6(&phr->ip), phr->contest_id);
  }

  // analyze permissions
  if (phr->role <= 0 || phr->role >= USER_ROLE_LAST)
    return ns_html_err_no_perm(fout, phr, 1, "invalid role %d", phr->role);
  if (phr->role == USER_ROLE_ADMIN) {
    // as for the master program
    if (opcaps_find(&cnts->capabilities, phr->login, &caps) < 0
        || opcaps_check(caps, OPCAP_MASTER_LOGIN) < 0)
      return ns_html_err_no_perm(fout, phr, 1, "user %s does not have MASTER_LOGIN bit for contest %d", phr->login, phr->contest_id);
  } else if (phr->role == USER_ROLE_JUDGE) {
    // as for the judge program
    if (opcaps_find(&cnts->capabilities, phr->login, &caps) < 0
        || opcaps_check(caps, OPCAP_JUDGE_LOGIN) < 0)
      return ns_html_err_no_perm(fout, phr, 1, "user %s does not have JUDGE_LOGIN bit for contest %d", phr->login, phr->contest_id);
  } else {
    // user privileges checked locally
    if (nsdb_check_role(phr->user_id, phr->contest_id, phr->role) < 0)
      return ns_html_err_no_perm(fout, phr, 1, "user %s has no permission to login as role %d for contest %d", phr->login, phr->role, phr->contest_id);
  }

  if (ejudge_config->new_server_log && ejudge_config->new_server_log[0]) {
    log_file_pos_1 = generic_file_size(NULL, ejudge_config->new_server_log, NULL);
  }

  watched_file_update(&extra->priv_header, cnts->priv_header_file, cur_time);
  watched_file_update(&extra->priv_footer, cnts->priv_footer_file, cur_time);
  extra->header_txt = extra->priv_header.text;
  extra->footer_txt = extra->priv_footer.text;
  if (!extra->header_txt || !extra->footer_txt) {
    extra->header_txt = ns_fancy_priv_header;
    extra->footer_txt = ns_fancy_priv_footer;
    extra->separator_txt = ns_fancy_priv_separator;
  }

  if (phr->name && *phr->name) {
    phr->name_arm = html_armor_string_dup(phr->name);
  } else {
    phr->name_arm = html_armor_string_dup(phr->login);
  }
  if (extra->contest_arm) xfree(extra->contest_arm);
  if (phr->locale_id == 0 && cnts->name_en) {
    extra->contest_arm = html_armor_string_dup(cnts->name_en);
  } else {
    extra->contest_arm = html_armor_string_dup(cnts->name);
  }

  snprintf(hid_buf, sizeof(hid_buf),
           "<input type=\"hidden\" name=\"SID\" value=\"%016llx\"/>",
           phr->session_id);
  phr->hidden_vars = hid_buf;
  phr->session_extra = ns_get_session(phr->session_id, phr->client_key, cur_time);
  phr->caps = 0;
  if (opcaps_find(&cnts->capabilities, phr->login, &caps) >= 0) {
    phr->caps = caps;
  }
  phr->dbcaps = 0;
  if (ejudge_cfg_opcaps_find(ejudge_config, phr->login, &caps) >= 0) {
    phr->dbcaps = caps;
  }

  memset(&callbacks, 0, sizeof(callbacks));
  callbacks.user_data = (void*) phr->fw_state;
  callbacks.list_all_users = ns_list_all_users_callback;

  // invoke the contest
  if (serve_state_load_contest(ejudge_config, phr->contest_id,
                               ul_conn,
                               &callbacks,
                               &extra->serve_state, 0, 0) < 0) {
    if (log_file_pos_1 >= 0) {
      log_file_pos_2 = generic_file_size(NULL, ejudge_config->new_server_log, NULL);
    }
    if (log_file_pos_1 >= 0 && log_file_pos_2 >= 0) {
      msg = read_file_range(ejudge_config->new_server_log, log_file_pos_1, log_file_pos_2);
    }
    ns_html_err_cnts_unavailable(fout, phr, 0, msg, 0);
    xfree(msg);
    return;
  }

  extra->serve_state->current_time = time(0);
  ns_check_contest_events(extra->serve_state, cnts);
  
  if (phr->action > 0 && phr->action < NEW_SRV_ACTION_LAST
      && actions_table[phr->action]) {
    actions_table[phr->action](fout, phr, cnts, extra);
  } else {
    if (phr->action < 0 || phr->action >= NEW_SRV_ACTION_LAST)
      phr->action = 0;
    priv_main_page(fout, phr, cnts, extra);
  }
}

static void
unpriv_load_html_style(struct http_request_info *phr,
                       const struct contest_desc *cnts,
                       struct contest_extra **p_extra,
                       time_t *p_cur_time)
{
  struct contest_extra *extra = 0;
  time_t cur_time = 0;
#if defined CONF_ENABLE_AJAX && CONF_ENABLE_AJAX
  unsigned char bb[8192];
  char *state_json_txt = 0;
  size_t state_json_len = 0;
  FILE *state_json_f = 0;
#endif

  extra = ns_get_contest_extra(phr->contest_id);
  ASSERT(extra);

  cur_time = time(0);
  watched_file_update(&extra->header, cnts->team_header_file, cur_time);
  watched_file_update(&extra->menu_1, cnts->team_menu_1_file, cur_time);
  watched_file_update(&extra->menu_2, cnts->team_menu_2_file, cur_time);
  watched_file_update(&extra->separator, cnts->team_separator_file, cur_time);
  watched_file_update(&extra->footer, cnts->team_footer_file, cur_time);
  watched_file_update(&extra->copyright, cnts->copyright_file, cur_time);
  extra->header_txt = extra->header.text;
  extra->footer_txt = extra->footer.text;
  extra->separator_txt = extra->separator.text;
  extra->copyright_txt = extra->copyright.text;
  if (!extra->header_txt || !extra->footer_txt || !extra->separator_txt) {
    extra->header_txt = ns_fancy_header;
    extra->separator_txt = ns_fancy_separator;
    if (extra->copyright_txt) extra->footer_txt = ns_fancy_footer_2;
    else extra->footer_txt = ns_fancy_footer;
  }

  if (extra->contest_arm) xfree(extra->contest_arm);
  if (phr->locale_id == 0 && cnts->name_en) {
    extra->contest_arm = html_armor_string_dup(cnts->name_en);
  } else {
    extra->contest_arm = html_armor_string_dup(cnts->name);
  }

  if (p_extra) *p_extra = extra;
  if (p_cur_time) *p_cur_time = cur_time;

  // js part
#if defined CONF_ENABLE_AJAX && CONF_ENABLE_AJAX
  if (extra->serve_state && phr->user_id > 0) {
    state_json_f = open_memstream(&state_json_txt, &state_json_len);
    do_json_user_state(state_json_f, extra->serve_state, phr->user_id, 0);
    close_memstream(state_json_f); state_json_f = 0;
  } else {
    state_json_txt = xstrdup("");
  }

  snprintf(bb, sizeof(bb),
           "<script type=\"text/javascript\" src=\"" CONF_STYLE_PREFIX "dojo/dojo.js\" djConfig=\"isDebug: false, parseOnLoad: true, dojoIframeHistoryUrl:'" CONF_STYLE_PREFIX "dojo/resources/iframe_history.html'\"></script>\n"
           "<script type=\"text/javascript\" src=\"" CONF_STYLE_PREFIX "unpriv.js\"></script>\n"
           "<script type=\"text/javascript\">\n"
           "  var SID=\"%016llx\";\n"
           "  var NEW_SRV_ACTION_JSON_USER_STATE=%d;\n"
           "  var NEW_SRV_ACTION_VIEW_PROBLEM_SUMMARY=%d;\n"
           "  var self_url=\"%s\";\n"
           "  var script_name=\"%s\";\n"
           "  dojo.require(\"dojo.parser\");\n"
           "  var jsonState = %s;\n"
           "  var updateFailedMessage = \"%s\";\n"
           "  var testingInProgressMessage = \"%s\";\n"
           "  var testingCompleted = \"%s\";\n"
           "  var waitingTooLong = \"%s\";\n"
           "</script>\n", phr->session_id, NEW_SRV_ACTION_JSON_USER_STATE,
           NEW_SRV_ACTION_VIEW_PROBLEM_SUMMARY,
           phr->self_url, phr->script_name, state_json_txt,
           _("STATUS UPDATE FAILED!"), _("TESTING IN PROGRESS..."),
           _("TESTING COMPLETED"), _("REFRESH PAGE MANUALLY!"));
  xfree(state_json_txt); state_json_txt = 0;
  phr->script_part = xstrdup(bb);
  snprintf(bb, sizeof(bb), " onload=\"startClock()\"");
  phr->body_attr = xstrdup(bb);
#endif
}

static int
unpriv_parse_run_id(FILE *fout, struct http_request_info *phr,
                    const struct contest_desc *cnts,
                    struct contest_extra *extra, int *p_run_id,
                    struct run_entry *pe)
{
  const serve_state_t cs = extra->serve_state;
  int n, run_id;
  const unsigned char *s = 0, *errmsg = 0;
  unsigned char msgbuf[1024];
  
  if (!(n = ns_cgi_param(phr, "run_id", &s))) {
    errmsg = ns_strerror_r(msgbuf, sizeof(msgbuf),
                           NEW_SRV_ERR_RUN_ID_UNDEFINED);
    goto failure;
  }
  if (n < 0 || sscanf(s, "%d%n", &run_id, &n) != 1 || s[n]) {
    errmsg = ns_strerror_r(msgbuf, sizeof(msgbuf), NEW_SRV_ERR_INV_RUN_ID);
    goto failure;
  }
  if (run_id < 0 || run_id >= run_get_total(cs->runlog_state)) {
    errmsg = ns_strerror_r(msgbuf, sizeof(msgbuf), NEW_SRV_ERR_INV_RUN_ID);
    errmsg = msgbuf;
    goto failure;
  }

  if (p_run_id) *p_run_id = run_id;
  if (pe && run_get_entry(cs->runlog_state, run_id, pe) < 0) {
    errmsg = ns_strerror_r(msgbuf, sizeof(msgbuf),
                           NEW_SRV_ERR_RUNLOG_READ_FAILED, run_id);
    goto failure;
  }

  return 0;

 failure:
  html_error_status_page(fout, phr, cnts, extra, errmsg,
                         ns_unpriv_prev_state[phr->action], 0);
  return -1;
}

/* FIXME: this should be moved to `new-register' part */
static void
unpriv_page_forgot_password_1(FILE *fout, struct http_request_info *phr,
                              int orig_locale_id)
{
  const struct contest_desc *cnts = 0;
  struct contest_extra *extra = 0;
  time_t cur_time = 0;
  unsigned char bb[1024];
  struct html_armor_buffer ab = HTML_ARMOR_INITIALIZER;

  if (phr->contest_id <= 0 || contests_get(phr->contest_id, &cnts) < 0 || !cnts)
    return ns_html_err_service_not_available(fout, phr, 0,
                                             "contest_id is invalid");
  if (orig_locale_id < 0 && cnts->default_locale_num >= 0)
    phr->locale_id = cnts->default_locale_num;
  if (!contests_check_team_ip(phr->contest_id, &phr->ip, phr->ssl_flag))
    return ns_html_err_service_not_available(fout, phr, 0, "%s://%s is not allowed for USER for contest %d", ns_ssl_flag_str[phr->ssl_flag], xml_unparse_ipv6(&phr->ip), phr->contest_id);
  if (cnts->closed)
    return ns_html_err_service_not_available(fout, phr, 0,
                                             "contest %d is closed", cnts->id);
  if (!cnts->managed)
    return ns_html_err_service_not_available(fout, phr, 0,
                                             "contest %d is not managed",
                                             cnts->id);
  if (!cnts->enable_password_recovery
      || (cnts->simple_registration && !cnts->send_passwd_email))
    return ns_html_err_service_not_available(fout, phr, 0,
                                             "contest %d password recovery disabled",
                                             cnts->id);

  unpriv_load_html_style(phr, cnts, &extra, &cur_time);

  l10n_setlocale(phr->locale_id);
  ns_header(fout, extra->header_txt, 0, 0, 0, 0, phr->locale_id, cnts,
            phr->client_key,
            _("Lost password recovery [%s]"), extra->contest_arm);

  // change language button
  fprintf(fout, "<div class=\"user_actions\"><table class=\"menu\"><tr>\n");
  html_start_form(fout, 1, phr->self_url, "");
  html_hidden(fout, "contest_id", "%d", phr->contest_id);
  html_hidden(fout, "action", "%d", NEW_SRV_ACTION_FORGOT_PASSWORD_1);
  if (cnts->disable_locale_change) 
    html_hidden(fout, "locale_id", "%d", phr->locale_id);

  if (!cnts->disable_locale_change) {
    fprintf(fout, "<td class=\"menu\"><div class=\"user_action_item\">%s: ",
            _("language"));
    l10n_html_locale_select(fout, phr->locale_id);
    fprintf(fout, "</div></td>\n");
  }

  fprintf(fout, "<td class=\"menu\"><div class=\"user_action_item\">%s</div></td>\n", ns_submit_button(bb, sizeof(bb), "submit", 0, _("Change Language")));

  fprintf(fout, "</tr></table></div>\n"
          "<div class=\"white_empty_block\">&nbsp;</div>\n"
          "<div class=\"contest_actions\"><table class=\"menu\"><tr>\n");

  fprintf(fout, "<td class=\"menu\"><div class=\"contest_actions_item\">&nbsp;</div></td></tr></table></div>\n");

  //fprintf(fout, "<div class=\"l13\">\n");
  if (extra->separator_txt && *extra->separator_txt)
    ns_separator(fout, extra->separator_txt, cnts);

  fprintf(fout, _("<p class=\"fixed_width\">Password recovery requires several steps. Now, please, specify the <b>login</b> and the <b>e-mail</b>, which was specified when the login was created.</p>\n<p class=\"fixed_width\">Note, that automatic password recovery is not possible for invisible, banned, locked, or privileged users!</p>\n"));

  html_start_form(fout, 1, phr->self_url, "");
  html_hidden(fout, "contest_id", "%d", phr->contest_id);
  fprintf(fout, "<table><tr><td class=\"menu\">%s:</td><td class=\"menu\">%s</td></tr>\n",
          _("Login"), html_input_text(bb, sizeof(bb), "login", 16, 0, 0));
  fprintf(fout, "<tr><td class=\"menu\">%s:</td><td class=\"menu\">%s</td></tr>\n",
          _("E-mail"), html_input_text(bb, sizeof(bb), "email", 16, 0, NULL));
  fprintf(fout, "<tr><td class=\"menu\">&nbsp;</td><td class=\"menu\">%s</td></tr></table></form>\n",
          BUTTON(NEW_SRV_ACTION_FORGOT_PASSWORD_2));


  ns_footer(fout, extra->footer_txt, extra->copyright_txt, phr->locale_id);
  l10n_setlocale(0);
  html_armor_free(&ab);
}

/* FIXME: this should be moved to `new-register' part */
static void
unpriv_page_forgot_password_2(FILE *fout, struct http_request_info *phr,
                              int orig_locale_id)
{
  const struct contest_desc *cnts = 0;
  struct contest_extra *extra = 0;
  time_t cur_time;
  struct html_armor_buffer ab = HTML_ARMOR_INITIALIZER;
  const unsigned char *login = 0, *email = 0;
  int r;
  FILE *log_f = 0;
  char *log_txt = 0;
  size_t log_len = 0;

  if (phr->contest_id <= 0 || contests_get(phr->contest_id, &cnts) < 0 || !cnts)
    return ns_html_err_service_not_available(fout, phr, 0, "contest_id is invalid");
  if (orig_locale_id < 0 && cnts->default_locale_num >= 0)
    phr->locale_id = cnts->default_locale_num;
  if (!contests_check_team_ip(phr->contest_id, &phr->ip, phr->ssl_flag))
    return ns_html_err_service_not_available(fout, phr, 0, "%s://%s is not allowed for USER for contest %d", ns_ssl_flag_str[phr->ssl_flag], xml_unparse_ipv6(&phr->ip), phr->contest_id);
  if (cnts->closed)
    return ns_html_err_service_not_available(fout, phr, 0,
                                             "contest %d is closed", cnts->id);
  if (!cnts->managed)
    return ns_html_err_service_not_available(fout, phr, 0,
                                             "contest %d is not managed",
                                             cnts->id);
  if (!cnts->enable_password_recovery
      || (cnts->simple_registration && !cnts->send_passwd_email))
    return ns_html_err_service_not_available(fout, phr, 0,
                                             "contest %d password recovery disabled",
                                             cnts->id);

  if (ns_cgi_param(phr, "login", &login) <= 0) {
    return ns_html_err_inv_param(fout, phr, 0, "login is not specified");
  }
  if (ns_cgi_param(phr, "email", &email) <= 0) {
    return ns_html_err_inv_param(fout, phr, 0, "email is not specified");
  }

  unpriv_load_html_style(phr, cnts, &extra, &cur_time);

  if (ns_open_ul_connection(phr->fw_state) < 0) {
    ns_html_err_ul_server_down(fout, phr, 0, 0);
    goto cleanup;
  }
  r = userlist_clnt_register_new(ul_conn, ULS_RECOVER_PASSWORD_1,
                                 &phr->ip, phr->ssl_flag,
                                 phr->contest_id,
                                 phr->locale_id,
                                 NEW_SRV_ACTION_FORGOT_PASSWORD_3,
                                 login, email, phr->self_url);

  if (r < 0) {
    log_f = open_memstream(&log_txt, &log_len);

    if (r == -ULS_ERR_EMAIL_FAILED) {
      fprintf(log_f, "%s",
              _("The server was unable to send a registration e-mail\n"
                "to the specified address. This is probably due\n"
                "to heavy server load rather than to an invalid\n"
                "e-mail address. You should try to register later.\n"));
    } else {
      fprintf(log_f, gettext(userlist_strerror(-r)));
    }

    close_memstream(log_f); log_f = 0;

    l10n_setlocale(phr->locale_id);
    ns_header(fout, extra->header_txt, 0, 0, 0, 0, phr->locale_id, cnts,
              phr->client_key,
              _("Password recovery error"));
    fprintf(fout, "%s", ns_fancy_empty_status);
    if (extra->separator_txt && *extra->separator_txt)
      ns_separator(fout, extra->separator_txt, cnts);
    fprintf(fout, "<p>Password recovery is not possible because of the following error.</p>\n");
    //fprintf(fout, "%s", extra->separator_txt);
    fprintf(fout, "<font color=\"red\"><pre>%s</pre></font>\n", ARMOR(log_txt));
    ns_footer(fout, extra->footer_txt, extra->copyright_txt, phr->locale_id);
    l10n_setlocale(0);
    goto cleanup;
  }

  l10n_setlocale(phr->locale_id);
  ns_header(fout, extra->header_txt, 0, 0, 0, 0, phr->locale_id, cnts,
            phr->client_key,
            _("Password recovery, stage 1 [%s, %s]"),
            ARMOR(login), extra->contest_arm);
  fprintf(fout, "%s", ns_fancy_empty_status);
  if (extra->separator_txt && *extra->separator_txt)
    ns_separator(fout, extra->separator_txt, cnts);

  fprintf(fout, _("<p class=\"fixed_width\">First stage of password recovery is successful. You should receive an e-mail message with further instructions. <b>Note,</b> that you should confirm password recovery in 24 hours, or operation will be cancelled.</p>"));

  ns_footer(fout, extra->footer_txt, extra->copyright_txt, phr->locale_id);
  l10n_setlocale(0);

 cleanup:
  if (log_f) fclose(log_f);
  xfree(log_txt);
  html_armor_free(&ab);
}

/* FIXME: this should be moved to `new-register' part */
static void
unpriv_page_forgot_password_3(FILE *fout, struct http_request_info *phr,
                              int orig_locale_id)
{
  const struct contest_desc *cnts = 0;
  struct contest_extra *extra = 0;
  time_t cur_time = 0;
  int user_id = 0;
  unsigned char *login = 0, *name = 0, *passwd = 0;
  struct html_armor_buffer ab = HTML_ARMOR_INITIALIZER;
  int r, regstatus = -1, blen;
  FILE *log_f = 0;
  char *log_txt = 0;
  size_t log_len = 0;
  unsigned char bb[1024];
  const unsigned char *s = 0;
  unsigned char urlbuf[1024];

  if (phr->contest_id <= 0 || contests_get(phr->contest_id, &cnts) < 0 || !cnts)
    return ns_html_err_service_not_available(fout, phr, 0, "contest_id is invalid");
  if (orig_locale_id < 0 || cnts->default_locale_num >= 0)
    phr->locale_id = cnts->default_locale_num;
  if (!contests_check_team_ip(phr->contest_id, &phr->ip, phr->ssl_flag))
    return ns_html_err_service_not_available(fout, phr, 0, "%s://%s is not allowed for USER for contest %d", ns_ssl_flag_str[phr->ssl_flag], xml_unparse_ipv6(&phr->ip), phr->contest_id);
  if (cnts->closed)
    return ns_html_err_service_not_available(fout, phr, 0,
                                             "contest %d is closed", cnts->id);
  if (!cnts->managed)
    return ns_html_err_service_not_available(fout, phr, 0,
                                             "contest %d is not managed",
                                             cnts->id);
  if (!cnts->enable_password_recovery
      || (cnts->simple_registration && !cnts->send_passwd_email))
    return ns_html_err_service_not_available(fout, phr, 0,
                                             "contest %d password recovery disabled",
                                             cnts->id);

  unpriv_load_html_style(phr, cnts, &extra, &cur_time);

  if (ns_open_ul_connection(phr->fw_state) < 0) {
    ns_html_err_ul_server_down(fout, phr, 0, 0);
    goto cleanup;
  }
  r = userlist_clnt_recover_passwd_2(ul_conn, ULS_RECOVER_PASSWORD_2,
                                     &phr->ip, phr->ssl_flag,
                                     phr->contest_id, phr->session_id,
                                     &user_id, &regstatus, 
                                     &login, &name, &passwd);

  if (r < 0) {
    log_f = open_memstream(&log_txt, &log_len);

    if (r == -ULS_ERR_EMAIL_FAILED) {
      fprintf(log_f, "%s",
              _("The server was unable to send a registration e-mail\n"
                "to the specified address. This is probably due\n"
                "to heavy server load rather than to an invalid\n"
                "e-mail address. You should try to register later.\n"));
    } else {
      fprintf(log_f, gettext(userlist_strerror(-r)));
    }

    close_memstream(log_f); log_f = 0;

    l10n_setlocale(phr->locale_id);
    ns_header(fout, extra->header_txt, 0, 0, 0, 0, phr->locale_id, cnts,
              phr->client_key,
              _("Password recovery error"));
    fprintf(fout, "%s", ns_fancy_empty_status);
    if (extra->separator_txt && *extra->separator_txt)
      ns_separator(fout, extra->separator_txt, cnts);
    fprintf(fout, "<p>Password recovery is not possible because of the following error.</p>\n");
    //fprintf(fout, "%s", extra->separator_txt);
    fprintf(fout, "<font color=\"red\"><pre>%s</pre></font>\n", ARMOR(log_txt));
    ns_footer(fout, extra->footer_txt, extra->copyright_txt, phr->locale_id);
    l10n_setlocale(0);
    goto cleanup;
  }

  s = name;
  if (!s || !*s) s = login;

  l10n_setlocale(phr->locale_id);
  ns_header(fout, extra->header_txt, 0, 0, 0, 0, phr->locale_id, cnts,
            phr->client_key,
            _("Password recovery completed [%s, %s]"),
            ARMOR(s), extra->contest_arm);
  fprintf(fout, "%s", ns_fancy_empty_status);
  if (extra->separator_txt && *extra->separator_txt)
    ns_separator(fout, extra->separator_txt, cnts);

  fprintf(fout, _("<p>New password is generated.</p>"));
  fprintf(fout, "<table><tr><td class=\"menu\">%s</td><td class=\"menu\"><tt>%s</tt></td></tr>\n",
          _("Login"), ARMOR(login));
  fprintf(fout, "<tr><td class=\"menu\">%s</td><td class=\"menu\"><tt>%s</tt></td></tr></table>\n", _("Password"), ARMOR(passwd));

  if (regstatus >= 0) {
    snprintf(urlbuf, sizeof(urlbuf), "%s", phr->self_url);
  } else if (cnts->register_url) {
    snprintf(urlbuf, sizeof(urlbuf), "%s", cnts->register_url);
  } else {
    snprintf(bb, sizeof(bb), "%s", phr->self_url);
    blen = strlen(bb);
    while (blen > 0 && bb[blen - 1] != '/') blen--;
    bb[blen] = 0;
    snprintf(urlbuf, sizeof(urlbuf), "%snew-register", bb);
  }

  html_start_form(fout, 1, urlbuf, "");
  html_hidden(fout, "contest_id", "%d", phr->contest_id);
  html_hidden(fout, "role", "%d", 0);
  html_hidden(fout, "locale_id", "%d", phr->locale_id);
  if (regstatus < 0) {
    html_hidden(fout, "action", "%d", NEW_SRV_ACTION_REG_LOGIN);
  }
  fprintf(fout, "<table><tr><td class=\"menu\">%s:</td><td class=\"menu\">%s</td></tr>\n",
          _("Login"), html_input_text(bb, sizeof(bb), "login", 16, 0, "%s", ARMOR(login)));
  fprintf(fout, "<tr><td class=\"menu\">%s:</td><td class=\"menu\"><input type=\"password\" size=\"16\" name=\"password\" value=\"%s\"/></td></tr>\n",
          _("Password"), ARMOR(passwd));
  fprintf(fout, "<tr><td class=\"menu\">&nbsp;</td><td class=\"menu\">%s</td></tr></table></form>\n",
          ns_submit_button(bb, sizeof(bb), "submit", 0, _("Submit")));
  ns_footer(fout, extra->footer_txt, extra->copyright_txt, phr->locale_id);
  l10n_setlocale(0);

 cleanup:
  xfree(login);
  xfree(name);
  xfree(passwd);
  if (log_f) fclose(log_f);
  xfree(log_txt);
  html_armor_free(&ab);
}

void
unprivileged_page_login_page(FILE *fout, struct http_request_info *phr,
                             int orig_locale_id)
{
  const struct contest_desc *cnts = 0;
  struct contest_extra *extra = 0;
  time_t cur_time;
  const unsigned char *s, *ss;
  unsigned char bb[1024];
  struct html_armor_buffer ab = HTML_ARMOR_INITIALIZER;
  int vis_flag = 0;

  if (phr->contest_id <= 0 || contests_get(phr->contest_id, &cnts) < 0 || !cnts)
    return ns_html_err_service_not_available(fout, phr, 0, "contest_id is invalid");
  if (orig_locale_id < 0 && cnts->default_locale_num >= 0)
    phr->locale_id = cnts->default_locale_num;
  if (!contests_check_team_ip(phr->contest_id, &phr->ip, phr->ssl_flag))
    return ns_html_err_service_not_available(fout, phr, 0, "%s://%s is not allowed for USER for contest %d", ns_ssl_flag_str[phr->ssl_flag], xml_unparse_ipv6(&phr->ip), phr->contest_id);
  if (cnts->closed)
    return ns_html_err_service_not_available(fout, phr, 0,
                                             "contest %d is closed", cnts->id);
  if (!cnts->managed)
    return ns_html_err_service_not_available(fout, phr, 0,
                                             "contest %d is not managed",
                                             cnts->id);

  extra = ns_get_contest_extra(phr->contest_id);
  ASSERT(extra);

  cur_time = time(0);
  watched_file_update(&extra->header, cnts->team_header_file, cur_time);
  watched_file_update(&extra->menu_1, cnts->team_menu_1_file, cur_time);
  watched_file_update(&extra->menu_2, cnts->team_menu_2_file, cur_time);
  watched_file_update(&extra->separator, cnts->team_separator_file, cur_time);
  watched_file_update(&extra->footer, cnts->team_footer_file, cur_time);
  watched_file_update(&extra->copyright, cnts->copyright_file, cur_time);
  extra->header_txt = extra->header.text;
  extra->menu_1_txt = extra->menu_1.text;
  extra->menu_2_txt = extra->menu_2.text;
  extra->footer_txt = extra->footer.text;
  extra->separator_txt = extra->separator.text;
  extra->copyright_txt = extra->copyright.text;
  if (!extra->header_txt || !extra->footer_txt || !extra->separator_txt) {
    extra->header_txt = ns_fancy_header;
    if (extra->copyright_txt) extra->footer_txt = ns_fancy_footer_2;
    else extra->footer_txt = ns_fancy_footer;
    extra->separator_txt = ns_fancy_separator;
  }

  if (extra->contest_arm) xfree(extra->contest_arm);
  if (phr->locale_id == 0 && cnts->name_en) {
    extra->contest_arm = html_armor_string_dup(cnts->name_en);
  } else {
    extra->contest_arm = html_armor_string_dup(cnts->name);
  }

  l10n_setlocale(phr->locale_id);
  ns_header(fout, extra->header_txt, 0, 0, 0, 0, phr->locale_id, cnts,
            phr->client_key,
            _("User login [%s]"), extra->contest_arm);


  html_start_form(fout, 1, phr->self_url, "");
  fprintf(fout, "<div class=\"user_actions\">");
  html_hidden(fout, "contest_id", "%d", phr->contest_id);
  html_hidden(fout, "role", "%s", "0");
  if (cnts->disable_locale_change)
    html_hidden(fout, "locale_id", "%d", phr->locale_id);
  fprintf(fout, "<table class=\"menu\"><tr>\n");

  ss = 0;
  if (ns_cgi_param(phr, "login", &s) > 0) ss = ARMOR(s);
  if (!ss) ss = "";
  fprintf(fout, "<td class=\"menu\"><div class=\"user_action_item\">%s: %s</div></td>\n", _("login"),
          html_input_text(bb, sizeof(bb), "login", 8, 0, "%s", ss));

  ss = 0;
  if (ns_cgi_param(phr, "password", &s) > 0) ss = ARMOR(s);
  if (!ss) ss = "";
  fprintf(fout, "<td class=\"menu\"><div class=\"user_action_item\">%s: <input type=\"password\" size=\"8\" name=\"password\" value=\"%s\"/></div></td>\n", _("password"), ss);

  if (!cnts->disable_locale_change) {
    fprintf(fout, "<td class=\"menu\"><div class=\"user_action_item\">%s: ",
            _("language"));
    l10n_html_locale_select(fout, phr->locale_id);
    fprintf(fout, "</div></td>\n");
  }

  fprintf(fout, "<td class=\"menu\"><div class=\"user_action_item\">%s</div></td>\n", ns_submit_button(bb, sizeof(bb), "submit", 0, _("Log in")));

  fprintf(fout, "</tr></table>");
  fprintf(fout, "</div></form>\n"
          "<div class=\"white_empty_block\">&nbsp;</div>\n"
          "<div class=\"contest_actions\"><table class=\"menu\"><tr>\n");

  if (cnts && cnts->assign_logins && cnts->force_registration
      && cnts->register_url
      && (cnts->reg_deadline <= 0 || cur_time < cnts->reg_deadline)) {
    fprintf(fout, "<td class=\"menu\"><div class=\"contest_actions_item\">");
    if (ejudge_config->disable_new_users <= 0) {
      if (cnts->assign_logins) {
        fprintf(fout,
                "<a class=\"menu\" href=\"%s?contest_id=%d&amp;locale_id=%d&amp;action=%d\">%s</a>",
                cnts->register_url, phr->contest_id, phr->locale_id,
                NEW_SRV_ACTION_REG_CREATE_ACCOUNT_PAGE,
                _("Registration"));
      } else {
        fprintf(fout,
                "<a class=\"menu\" href=\"%s?contest_id=%d&amp;locale_id=%d&amp;action=2\">%s</a>",
                cnts->register_url, phr->contest_id, phr->locale_id,
                _("Registration"));
      }
    }
    fprintf(fout, "</div></td>\n");
    vis_flag++;
  } else if (cnts && cnts->register_url
             && (cnts->reg_deadline <= 0 || cur_time < cnts->reg_deadline)) {
    fprintf(fout, "<td class=\"menu\"><div class=\"contest_actions_item\">");
    if (ejudge_config->disable_new_users <= 0) {
      fprintf(fout,
              "<a class=\"menu\" href=\"%s?contest_id=%d&amp;locale_id=%d\">%s</a>",
              cnts->register_url, phr->contest_id, phr->locale_id,
              _("Registration"));
    }
    fprintf(fout, "</div></td>\n");
    vis_flag++;
  }

  if (cnts && cnts->enable_password_recovery && cnts->disable_team_password) {
    fprintf(fout, "<td class=\"menu\"><div class=\"contest_actions_item\"><a class=\"menu\" href=\"%s?contest_id=%d&amp;locale_id=%d&amp;action=%d\">%s</a></div></td>", phr->self_url, phr->contest_id, phr->locale_id, NEW_SRV_ACTION_FORGOT_PASSWORD_1, _("Forgot password?"));
    vis_flag++;
  }

  if (!vis_flag) {
    fprintf(fout, "<td class=\"menu\"><div class=\"contest_actions_item\">&nbsp;</div></td>");
  }

  /*
  fprintf(fout, "<div class=\"search_actions\"><a href=\"\">%s</a>&nbsp;&nbsp;<a href=\"\">%s</a></div>", _("Registration"), _("Forgot the password?"));
  */

  fprintf(fout, "</tr></table></div>\n");
  if (extra->separator_txt && *extra->separator_txt)
    ns_separator(fout, extra->separator_txt, cnts);

  watched_file_update(&extra->welcome, cnts->welcome_file, cur_time);
  if (extra->welcome.text && extra->welcome.text[0])
    fprintf(fout, "%s", extra->welcome.text);

  ns_footer(fout, extra->footer_txt, extra->copyright_txt, phr->locale_id);
  l10n_setlocale(0);
  html_armor_free(&ab);
}

static void
unprivileged_page_login(FILE *fout, struct http_request_info *phr,
                        int orig_locale_id)
{
  const unsigned char *login = 0;
  const unsigned char *password = 0;
  int r;
  const struct contest_desc *cnts = 0;

  if ((r = ns_cgi_param(phr, "login", &login)) < 0)
    return ns_html_err_inv_param(fout, phr, 0, "cannot parse login");
  if (!r || phr->action == NEW_SRV_ACTION_LOGIN_PAGE)
    return unprivileged_page_login_page(fout, phr, orig_locale_id);

  if (phr->contest_id<=0 || contests_get(phr->contest_id, &cnts)<0 || !cnts)
    return ns_html_err_inv_param(fout, phr, 0, "invalid contest_id");
  if (orig_locale_id < 0 && cnts->default_locale_num >= 0)
    phr->locale_id = cnts->default_locale_num;

  phr->login = xstrdup(login);
  if ((r = ns_cgi_param(phr, "password", &password)) <= 0)
    return ns_html_err_inv_param(fout, phr, 0, "cannot parse password");
  if (!contests_check_team_ip(phr->contest_id, &phr->ip, phr->ssl_flag))
    return ns_html_err_no_perm(fout, phr, 0, "%s://%s is not allowed for USER for contest %d", ns_ssl_flag_str[phr->ssl_flag], xml_unparse_ipv6(&phr->ip), phr->contest_id);
  if (cnts->closed)
    return ns_html_err_service_not_available(fout, phr, 0,
                                             "contest %d is closed", cnts->id);
  if (!cnts->managed)
    return ns_html_err_service_not_available(fout, phr, 0,
                                             "contest %d is not managed",
                                             cnts->id);

  if (ns_open_ul_connection(phr->fw_state) < 0)
    return ns_html_err_ul_server_down(fout, phr, 0, 0);

  if ((r = userlist_clnt_login(ul_conn, ULS_TEAM_CHECK_USER,
                               &phr->ip, phr->client_key,
                               phr->ssl_flag, phr->contest_id,
                               phr->locale_id, login, password,
                               &phr->user_id,
                               &phr->session_id, &phr->client_key,
                               &phr->name)) < 0) {
    switch (-r) {
    case ULS_ERR_INVALID_LOGIN:
    case ULS_ERR_INVALID_PASSWORD:
    case ULS_ERR_BAD_CONTEST_ID:
    case ULS_ERR_IP_NOT_ALLOWED:
    case ULS_ERR_NO_PERMS:
    case ULS_ERR_NOT_REGISTERED:
    case ULS_ERR_CANNOT_PARTICIPATE:
      return ns_html_err_no_perm(fout, phr, 0, "user_login failed: %s",
                                 userlist_strerror(-r));
    case ULS_ERR_DISCONNECT:
      return ns_html_err_ul_server_down(fout, phr, 0, 0);
    case ULS_ERR_INCOMPLETE_REG:
      return ns_html_err_registration_incomplete(fout, phr);
    default:
      return ns_html_err_internal_error(fout, phr, 0, "user_login failed: %s",
                                        userlist_strerror(-r));
    }
  }

  ns_get_session(phr->session_id, phr->client_key, 0);
  ns_refresh_page(fout, phr, NEW_SRV_ACTION_MAIN_PAGE, "lt=1");
}

static void
unpriv_change_language(FILE *fout,
                       struct http_request_info *phr,
                       const struct contest_desc *cnts,
                       struct contest_extra *extra)
{
  const unsigned char *s;
  int r, n;
  char *log_txt = 0;
  size_t log_len = 0;
  FILE *log_f = 0;
  int new_locale_id;

  if ((r = ns_cgi_param(phr, "locale_id", &s)) < 0)
    return ns_html_err_inv_param(fout, phr, 0, "cannot parse locale_id");
  if (r > 0) {
    if (sscanf(s, "%d%n", &new_locale_id, &n) != 1 || s[n] || new_locale_id < 0)
      return ns_html_err_inv_param(fout, phr, 0, "cannot parse locale_id");
  }

  log_f = open_memstream(&log_txt, &log_len);

  if (ns_open_ul_connection(phr->fw_state) < 0) {
    ns_html_err_ul_server_down(fout, phr, 0, 0);
    goto cleanup;
  }
  if ((r = userlist_clnt_set_cookie(ul_conn, ULS_SET_COOKIE_LOCALE,
                                    phr->session_id,
                                    phr->client_key,
                                    new_locale_id)) < 0) {
    fprintf(log_f, "set_cookie failed: %s", userlist_strerror(-r));
  }

  //done:
  close_memstream(log_f); log_f = 0;
  if (!log_txt || !*log_txt) {
    ns_refresh_page(fout, phr, NEW_SRV_ACTION_MAIN_PAGE, 0);
  } else {
    html_error_status_page(fout, phr, cnts, extra, log_txt,
                           NEW_SRV_ACTION_MAIN_PAGE, 0);
  }

 cleanup:
  if (log_f) close_memstream(log_f);
  xfree(log_txt);
}

static void
unpriv_change_password(FILE *fout,
                       struct http_request_info *phr,
                       const struct contest_desc *cnts,
                       struct contest_extra *extra)
{
  const unsigned char *p0 = 0, *p1 = 0, *p2 = 0;
  char *log_txt = 0;
  size_t log_len = 0;
  FILE *log_f = 0;
  int cmd, r;
  unsigned char url[1024];
  unsigned char login_buf[256];

  if (ns_cgi_param(phr, "oldpasswd", &p0) <= 0)
    return ns_html_err_inv_param(fout, phr, 0, "cannot parse oldpasswd");
  if (ns_cgi_param(phr, "newpasswd1", &p1) <= 0)
    return ns_html_err_inv_param(fout, phr, 0, "cannot parse newpasswd1");
  if (ns_cgi_param(phr, "newpasswd2", &p2) <= 0)
    return ns_html_err_inv_param(fout, phr, 0, "cannot parse newpasswd2");

  log_f = open_memstream(&log_txt, &log_len);

  if (strlen(p0) >= 256) {
    ns_error(log_f, NEW_SRV_ERR_OLD_PWD_TOO_LONG);
    goto done;
  }
  if (strcmp(p1, p2)) {
    ns_error(log_f, NEW_SRV_ERR_NEW_PWD_MISMATCH);
    goto done;
  }
  if (strlen(p1) >= 256) {
    ns_error(log_f, NEW_SRV_ERR_NEW_PWD_TOO_LONG);
    goto done;
  }

  cmd = ULS_PRIV_SET_TEAM_PASSWD;
  if (cnts->disable_team_password) cmd = ULS_PRIV_SET_REG_PASSWD;

  if (ns_open_ul_connection(phr->fw_state) < 0) {
    ns_html_err_ul_server_down(fout, phr, 0, 0);
    goto cleanup;
  }
  r = userlist_clnt_set_passwd(ul_conn, cmd, phr->user_id, phr->contest_id,
                               p0, p1);
  if (r < 0) {
    ns_error(log_f, NEW_SRV_ERR_PWD_UPDATE_FAILED, userlist_strerror(-r));
    goto done;
  }

 done:;
  close_memstream(log_f); log_f = 0;
  if (!log_txt || !*log_txt) {
    url_armor_string(login_buf, sizeof(login_buf), phr->login);
    snprintf(url, sizeof(url),
             "%s?contest_id=%d&login=%s&locale_id=%d&action=%d",
             phr->self_url, phr->contest_id, login_buf, phr->locale_id,
             NEW_SRV_ACTION_LOGIN_PAGE);
    ns_refresh_page_2(fout, phr->client_key, url);
  } else {
    html_error_status_page(fout, phr, cnts, extra, log_txt,
                           NEW_SRV_ACTION_MAIN_PAGE, 0);
  }

 cleanup:;
  if (log_f) close_memstream(log_f);
  xfree(log_txt);
}

static void
unpriv_print_run(FILE *fout,
                 struct http_request_info *phr,
                 const struct contest_desc *cnts,
                 struct contest_extra *extra)
{
  serve_state_t cs = extra->serve_state;
  char *log_txt = 0;
  size_t log_len = 0;
  FILE *log_f = 0;
  int run_id, n;
  struct run_entry re;

  if (unpriv_parse_run_id(fout, phr, cnts, extra, &run_id, &re) < 0)
    goto cleanup;

  log_f = open_memstream(&log_txt, &log_len);

  if (!cs->global->enable_printing || cs->printing_suspended) {
    ns_error(log_f, NEW_SRV_ERR_PRINTING_DISABLED);
    goto done;
  }

  if (re.status > RUN_LAST
      || (re.status > RUN_MAX_STATUS && re.status < RUN_TRANSIENT_FIRST)
      || re.user_id != phr->user_id) {
    ns_error(log_f, NEW_SRV_ERR_PERMISSION_DENIED);
    goto done;
  }

  if (re.pages > 0) {
    ns_error(log_f, NEW_SRV_ERR_ALREADY_PRINTED);
    goto done;
  }

  if ((n = team_print_run(cs, run_id, phr->user_id)) < 0) {
    switch (-n) {
    case SRV_ERR_PAGES_QUOTA:
      ns_error(log_f,NEW_SRV_ERR_ALREADY_PRINTED, cs->global->team_page_quota);
      goto done;
    default:
      ns_error(log_f, NEW_SRV_ERR_PRINTING_FAILED, -n, protocol_strerror(-n));
      goto done;
    }
  }

  serve_audit_log(cs, run_id, &re, phr->user_id, &phr->ip, phr->ssl_flag,
                  "print", "ok", -1, "  %d pages printed\n", n);

 done:
  close_memstream(log_f); log_f = 0;
  if (!log_txt || !*log_txt) {
    ns_refresh_page(fout, phr, NEW_SRV_ACTION_MAIN_PAGE, 0);
  } else {
    html_error_status_page(fout, phr, cnts, extra, log_txt,
                           NEW_SRV_ACTION_MAIN_PAGE, 0);
  }

 cleanup:;
  if (log_f) close_memstream(log_f);
  xfree(log_txt);
}

int
ns_submit_run(
        FILE *log_f,
        struct http_request_info *phr,
        const struct contest_desc *cnts,
        struct contest_extra *extra,
        const unsigned char *prob_param_name,
        const unsigned char *lang_param_name,
        int enable_ans_collect,
        int enable_path,
        int enable_uuid,
        int enable_user_id,
        int enable_status,
        int admin_mode,
        int is_hidden,
        int *p_run_id,
        int *p_mime_type,
        int *p_next_prob_id)
{
  int retval = 0, r;
  int user_id = 0, prob_id = 0, lang_id = 0, status = -1;
  serve_state_t cs = extra->serve_state;
  const struct section_global_data *global = cs->global;
  const struct section_problem_data *prob = NULL;
  const struct section_language_data *lang = NULL;
  unsigned char *utf8_str = NULL;
  ssize_t utf8_len = 0;
  const unsigned char *s;
  const unsigned char *run_text = NULL;
  ssize_t run_size = 0;
  size_t tmpsz;
  char *ans_text = NULL;
  int skip_mime_type_test = 0;
  char *run_file = NULL;
  ruint32_t run_uuid[4] = { 0, 0, 0, 0 };
  ruint32_t *uuid_ptr = NULL;
  int eoln_type = 0;

  if (!prob_param_name) prob_param_name = "prob_id";
  if (ns_cgi_param(phr, prob_param_name, &s) <= 0 || !s) {
    FAIL(NEW_SRV_ERR_INV_PROB_ID);
  }
  for (prob_id = 1; prob_id <= cs->max_prob; ++prob_id) {
    if ((prob = cs->probs[prob_id]) && prob->short_name && !strcmp(s, prob->short_name))
      break;
  }
  if (prob_id > cs->max_prob) {
    char *eptr = NULL;
    errno = 0;
    prob_id = strtol(s, &eptr, 10);
    if (errno || *eptr || prob_id <= 0 || prob_id > cs->max_prob || !(prob = cs->probs[prob_id])) {
      FAIL(NEW_SRV_ERR_INV_PROB_ID);
    }
  }

  if (prob->type == PROB_TYPE_STANDARD) {
    // "STANDARD" problems need programming language identifier
    if (!lang_param_name) lang_param_name = "lang_id";
    if (ns_cgi_param(phr, lang_param_name, &s) <= 0 || !s) {
      FAIL(NEW_SRV_ERR_INV_LANG_ID);
    }
    for (lang_id = 1; lang_id <= cs->max_lang; ++lang_id) {
      if ((lang = cs->langs[lang_id]) && lang->short_name && !strcmp(lang->short_name, s))
        break;
    }
    if (lang_id > cs->max_lang) {
      char *eptr = NULL;
      errno = 0;
      lang_id = strtol(s, &eptr, 10);
      if (errno || *eptr || lang_id <= 0 || lang_id > cs->max_lang || !(lang = cs->langs[lang_id])) {
        FAIL(NEW_SRV_ERR_INV_LANG_ID);
      }
    }
    if (cs->global->enable_eoln_select > 0) {
      ns_cgi_param_int_opt(phr, "eoln_type", &eoln_type, 0);
      if (eoln_type < 0 || eoln_type > EOLN_CRLF) eoln_type = 0;
    }
  }

  switch (prob->type) {
  case PROB_TYPE_STANDARD:
  case PROB_TYPE_OUTPUT_ONLY:
    if (enable_path > 0) {
      const unsigned char *path = NULL;
      if (ns_cgi_param(phr, "path", &path) <= 0 || !path) FAIL(NEW_SRV_ERR_FILE_UNSPECIFIED);
      if (generic_read_file(&run_file, 0, &tmpsz, 0, NULL, path, NULL) < 0)
        FAIL(NEW_SRV_ERR_FILE_UNSPECIFIED);
      run_text = run_file;
      run_size = tmpsz;
      r = 1;
    } else {
      r = ns_cgi_param_bin(phr, "file", &run_text, &tmpsz); run_size = tmpsz;
    }
    if (r <= 0 || !run_text || run_size <= 0) {
      if (prob->enable_text_form > 0) {
        r = ns_cgi_param_bin(phr, "text_form", &run_text, &tmpsz); run_size = tmpsz;
        if (r <= 0 || !run_text || run_size <= 0) {
          FAIL(NEW_SRV_ERR_FILE_EMPTY);
        }
        if (run_size != strlen(run_text)) {
          FAIL(NEW_SRV_ERR_BINARY_FILE);
        }
        skip_mime_type_test = 1;
      } else {
        FAIL(NEW_SRV_ERR_FILE_UNSPECIFIED);
      }
    }
    break;

  case PROB_TYPE_TESTS:
  case PROB_TYPE_TEXT_ANSWER:
  case PROB_TYPE_SHORT_ANSWER:
  case PROB_TYPE_SELECT_ONE:
    if (enable_path > 0) {
      const unsigned char *path = NULL;
      if (ns_cgi_param(phr, "path", &path) <= 0 || !path) FAIL(NEW_SRV_ERR_FILE_UNSPECIFIED);
      if (generic_read_file(&run_file, 0, &tmpsz, 0, NULL, path, NULL) < 0)
        FAIL(NEW_SRV_ERR_FILE_UNSPECIFIED);
      run_text = run_file;
      run_size = tmpsz;
      r = 1;
    } else {
      r = ns_cgi_param_bin(phr, "file", &run_text, &tmpsz); run_size = tmpsz;
    }
    if (r <= 0 || !run_text || run_size <= 0) {
      FAIL(NEW_SRV_ERR_FILE_UNSPECIFIED);
    }
    break;

  case PROB_TYPE_SELECT_MANY: 
    if (enable_ans_collect > 0) {
      // "ans_*"
      int max_ans = -1;
      for (int i = 0; i < phr->param_num; ++i) {
        if (!strncmp(phr->param_names[i], "ans_", 4) && isdigit(phr->param_names[i][4])) {
          char *eptr = NULL;
          errno = 0;
          int ans = strtol(phr->param_names[i] + 4, &eptr, 10);
          if (errno || *eptr || ans < 0 || ans > 65535) {
            FAIL(NEW_SRV_ERR_INV_ANSWER);
          }
          if (ans > max_ans) max_ans = ans;
        }
      }
      if (max_ans >= 0) {
        unsigned char *ans_map = NULL;
        XCALLOC(ans_map, max_ans + 1);
        for (int i = 0; i < phr->param_num; ++i) {
          if (!strncmp(phr->param_names[i], "ans_", 4) && isdigit(phr->param_names[i][4])) {
            char *eptr = NULL;
            errno = 0;
            int ans = strtol(phr->param_names[i] + 4, &eptr, 10);
            if (!errno && !*eptr && ans >= 0 && ans <= max_ans) {
              ans_map[ans] = 1;
            }
          }
        }
        int nonfirst = 0;
        FILE *f = open_memstream(&ans_text, &tmpsz);
        for (int ans = 0; ans <= max_ans; ++ans) {
          if (ans_map[ans]) {
            if (nonfirst) putc(' ', f);
            nonfirst = 1;
            fprintf(f, "%d", ans);
          }
        }
        if (nonfirst) putc('\n', f);
        fclose(f); f = NULL;
        xfree(ans_map); ans_map = NULL;
        run_text = ans_text; run_size = tmpsz;
      } else {
        run_text = ""; run_size = 0;
      }
    } else {
      if (enable_path > 0) {
        const unsigned char *path = NULL;
        if (ns_cgi_param(phr, "path", &path) <= 0 || !path) FAIL(NEW_SRV_ERR_FILE_UNSPECIFIED);
        if (generic_read_file(&run_file, 0, &tmpsz, 0, NULL, path, NULL) < 0)
          FAIL(NEW_SRV_ERR_FILE_UNSPECIFIED);
        run_text = run_file;
        run_size = tmpsz;
        r = 1;
      } else {
        r = ns_cgi_param_bin(phr, "file", &run_text, &tmpsz); run_size = tmpsz;
      }
      if (r <= 0 || !run_text || run_size <= 0) {
        run_text = ""; run_size = 0;
      }
    }
    break;
  case PROB_TYPE_CUSTOM:
    {
      // invoke problem plugin
      struct problem_plugin_iface *plg = NULL;
      load_problem_plugin(cs, prob_id);
      if (!(plg = cs->prob_extras[prob_id].plugin) || !plg->parse_form) {
        FAIL(NEW_SRV_ERR_PLUGIN_NOT_AVAIL);
      }
      if ((ans_text = (*plg->parse_form)(cs->prob_extras[prob_id].plugin_data, log_f, phr, cnts, extra))) {
        run_text = ans_text;
        run_size = strlen(ans_text);
      } else {
        // FIXME: ERROR?
        run_text = ""; run_size = 0;
      }
    }
    break;
  }

  switch (prob->type) {
  case PROB_TYPE_STANDARD:
    if (lang->binary <= 0 && strlen(run_text) != run_size) {
      if ((utf8_len = ucs2_to_utf8(&utf8_str, run_text, run_size)) < 0) {
        FAIL(NEW_SRV_ERR_BINARY_FILE);
      }
      run_text = utf8_str;
      run_size = utf8_len;
    }
    if (prob->disable_ctrl_chars > 0 && has_control_characters(run_text)) {
      FAIL(NEW_SRV_ERR_INV_CHAR);
    }
    break;
  case PROB_TYPE_OUTPUT_ONLY:
    if (prob->binary_input <= 0 && prob->binary <= 0 && strlen(run_text) != run_size) {
      FAIL(NEW_SRV_ERR_BINARY_FILE);
    }
    break;
  case PROB_TYPE_TEXT_ANSWER:
  case PROB_TYPE_SHORT_ANSWER:
  case PROB_TYPE_SELECT_MANY:
    if (strlen(run_text) != run_size) {
      FAIL(NEW_SRV_ERR_BINARY_FILE);
    }
    break;

  case PROB_TYPE_SELECT_ONE:
    {
      if (strlen(run_text) != run_size) {
        FAIL(NEW_SRV_ERR_BINARY_FILE);
      }
      const unsigned char *eptr1 = run_text + run_size;
      while (eptr1 > run_text && isspace(eptr1[-1])) --eptr1;
      if (eptr1 == run_text) {
        FAIL(NEW_SRV_ERR_ANSWER_UNSPECIFIED);
      }
      char *eptr2 = NULL;
      errno = 0;
      int ans_val = strtol(run_text, &eptr2, 10);
      if (errno || eptr1 != (const unsigned char *) eptr2 || ans_val < 0) {
        FAIL(NEW_SRV_ERR_INV_ANSWER);
      }
    }
    break;

  case PROB_TYPE_TESTS:
  case PROB_TYPE_CUSTOM:
    break;
  }

  // ignore BOM
  if (global->ignore_bom > 0 && prob->binary <= 0 && (!lang || lang->binary <= 0)) {
    if (run_text && run_size >= 3 && run_text[0] == 0xef && run_text[1] == 0xbb && run_text[2] == 0xbf) {
      run_text += 3; run_size -= 3;
    }
  }

  if (enable_user_id > 0) {
    if (ns_cgi_param_int(phr, "user_id", &user_id) < 0 || user_id <= 0)
      FAIL(NEW_SRV_ERR_INV_USER_ID);
  } else {
    user_id = phr->user_id;
  }
  if (enable_status > 0) {
    if (ns_cgi_param_int(phr, "status", &status) < 0 || status < 0)
      FAIL(NEW_SRV_ERR_INV_STATUS);
  }

  time_t start_time = 0;
  time_t stop_time = 0;
  if (global->is_virtual > 0 && !admin_mode) {
    start_time = run_get_virtual_start_time(cs->runlog_state, user_id);
    stop_time = run_get_virtual_stop_time(cs->runlog_state, user_id, cs->current_time);
  } else {
    start_time = run_get_start_time(cs->runlog_state);
    stop_time = run_get_stop_time(cs->runlog_state);
  }

  // availability checks
  if (!admin_mode && cs->clients_suspended) {
    FAIL(NEW_SRV_ERR_CLIENTS_SUSPENDED);
  }
  if (!admin_mode && start_time <= 0) {
    FAIL(NEW_SRV_ERR_CONTEST_NOT_STARTED);
  }
  if (!admin_mode && stop_time > 0) {
    FAIL(NEW_SRV_ERR_CONTEST_ALREADY_FINISHED);
  }
  if (!admin_mode && serve_check_user_quota(cs, user_id, run_size) < 0) {
    FAIL(NEW_SRV_ERR_RUN_QUOTA_EXCEEDED);
  }
  if (!admin_mode && !serve_is_problem_started(cs, user_id, prob)) {
    FAIL(NEW_SRV_ERR_PROB_UNAVAILABLE);
  }
  time_t user_deadline = 0;
  if (!admin_mode && serve_is_problem_deadlined(cs, user_id, phr->login, prob, &user_deadline)) {
    FAIL(NEW_SRV_ERR_PROB_DEADLINE_EXPIRED);
  }

  int mime_type = 0;
  if (p_mime_type) *p_mime_type = 0;
  if (!admin_mode && lang_id > 0) {
    if (lang->disabled > 0) {
      FAIL(NEW_SRV_ERR_LANG_DISABLED);
    }
    if (lang->insecure > 0 && global->secure_run > 0 && prob->disable_security <= 0) {
      FAIL(NEW_SRV_ERR_LANG_DISABLED);
    }
    if (prob->enable_language) {
      char **lang_list = prob->enable_language;
      int i;
      for (i = 0; lang_list[i]; ++i)
        if (!strcmp(lang_list[i], lang->short_name))
          break;
      if (!lang_list[i]) {
        FAIL(NEW_SRV_ERR_LANG_NOT_AVAIL_FOR_PROBLEM);
      }
    } else if (prob->disable_language) {
      char **lang_list = prob->disable_language;
      int i;
      for (i = 0; lang_list[i]; ++i)
        if (!strcmp(lang_list[i], lang->short_name))
          break;
      if (lang_list[i]) {
        FAIL(NEW_SRV_ERR_LANG_DISABLED_FOR_PROBLEM);
      }
    }
  } else if (!admin_mode && !skip_mime_type_test) {
    // guess the content-type and check it against the list
    if ((mime_type = mime_type_guess(global->diff_work_dir, run_text, run_size)) < 0) {
      FAIL(NEW_SRV_ERR_CANNOT_DETECT_CONTENT_TYPE);
    }
    if (p_mime_type) *p_mime_type = mime_type;
    const unsigned char *mime_type_str = mime_type_get_type(mime_type);
    if (prob->enable_language) {
      char **lang_list = prob->enable_language;
      int i;
      for (i = 0; lang_list[i]; i++)
        if (!strcmp(lang_list[i], mime_type_str))
          break;
      if (!lang_list[i]) {
        FAIL(NEW_SRV_ERR_CONTENT_TYPE_NOT_AVAILABLE);
      }
    } else if (prob->disable_language) {
      char **lang_list = prob->disable_language;
      int i;
      for (i = 0; lang_list[i]; i++)
        if (!strcmp(lang_list[i], mime_type_str))
          break;
      if (lang_list[i]) {
        FAIL(NEW_SRV_ERR_CONTENT_TYPE_DISABLED);
      }
    }
  }

  int variant = 0;
  if (prob->variant_num > 0) {
    if (admin_mode) {
      if (ns_cgi_param_int_opt(phr, "variant", &variant, 0) < 0) {
        FAIL(NEW_SRV_ERR_INV_VARIANT);
      }
      if (!variant && (variant = find_variant(cs, user_id, prob_id, 0)) <= 0) {
        FAIL(NEW_SRV_ERR_VARIANT_UNASSIGNED);
      }
      if (variant < 0 || variant > prob->variant_num) {
        FAIL(NEW_SRV_ERR_INV_VARIANT);
      }
    } else {
      if ((variant = find_variant(cs, user_id, prob_id, 0)) <= 0) {
        FAIL(NEW_SRV_ERR_VARIANT_UNASSIGNED);
      }
    }
  }

  ruint32_t shaval[5];
  sha_buffer(run_text, run_size, shaval);

  if (enable_uuid) {
    const unsigned char *uuid_str = NULL;
    if (ns_cgi_param(phr, "uuid", &uuid_str) > 0 && uuid_str && *uuid_str) {
      if (ej_uuid_parse(uuid_str, run_uuid) < 0) {
        FAIL(NEW_SRV_ERR_INV_PARAM);
      }
      uuid_ptr = run_uuid;
    }
  }

  int run_id = 0;
  if (!admin_mode && global->ignore_duplicated_runs != 0) {
    if ((run_id = run_find_duplicate(cs->runlog_state, user_id, prob_id,
                                     lang_id, variant, run_size, shaval)) >= 0) {
      if (p_run_id) *p_run_id = run_id;
      FAIL(NEW_SRV_ERR_DUPLICATE_SUBMIT);
    }
  }

  unsigned char *acc_probs = NULL;
  if (!admin_mode && prob->disable_submit_after_ok > 0
      && global->score_system != SCORE_OLYMPIAD && !cs->accepting_mode) {
    if (!acc_probs) {
      XALLOCAZ(acc_probs, cs->max_prob + 1);
      run_get_accepted_set(cs->runlog_state, user_id,
                           cs->accepting_mode, cs->max_prob, acc_probs);
    }
    if (acc_probs[prob_id]) {
      FAIL(NEW_SRV_ERR_PROB_ALREADY_SOLVED);
    }
  }

  if (!admin_mode && prob->require) {
    if (!acc_probs) {
      XALLOCAZ(acc_probs, cs->max_prob + 1);
      run_get_accepted_set(cs->runlog_state, user_id,
                           cs->accepting_mode, cs->max_prob, acc_probs);
    }
    int i;
    for (i = 0; prob->require[i]; ++i) {
      int j;
      for (j = 1; j <= cs->max_prob; ++j)
        if (cs->probs[j] && !strcmp(cs->probs[j]->short_name, prob->require[i]))
          break;
      if (j > cs->max_prob || !acc_probs[j]) break;
    }
    if (prob->require[i]) {
      FAIL(NEW_SRV_ERR_NOT_ALL_REQ_SOLVED);
    }
  }

  int accept_immediately = 0;
  if (prob->type == PROB_TYPE_SELECT_ONE || prob->type == PROB_TYPE_SELECT_MANY) {
    // add this run and if we're in olympiad accepting mode mark as accepted
    if (global->score_system == SCORE_OLYMPIAD && cs->accepting_mode)
      accept_immediately = 1;
  }

  // OK, so all checks are done, now we add this submit to the database
  int db_variant = variant;
  struct timeval precise_time;
  gettimeofday(&precise_time, 0);
  if (admin_mode) {
    if (is_hidden < 0) is_hidden = 0;
    if (is_hidden > 1) is_hidden = 1;
  } else {
    is_hidden = 0;
    db_variant = 0;
  }

  int store_flags = 0;
  if (uuid_ptr == NULL) {
    ej_uuid_generate(run_uuid);
    uuid_ptr = run_uuid;
  }
  if (global->uuid_run_store > 0 && run_get_uuid_hash_state(cs->runlog_state) >= 0 && ej_uuid_is_nonempty(run_uuid)) {
    store_flags = 1;
  }
  run_id = run_add_record(cs->runlog_state, 
                          precise_time.tv_sec, precise_time.tv_usec * 1000,
                          run_size, shaval, uuid_ptr,
                          &phr->ip, phr->ssl_flag,
                          phr->locale_id, user_id,
                          prob_id, lang_id, eoln_type,
                          db_variant, is_hidden, mime_type, store_flags);
  if (run_id < 0) {
    FAIL(NEW_SRV_ERR_RUNLOG_UPDATE_FAILED);
  }
  serve_move_files_to_insert_run(cs, run_id);

  unsigned char run_path[PATH_MAX];
  run_path[0] = 0;
  int arch_flags = 0;
  if (store_flags == 1) {
    arch_flags = uuid_archive_prepare_write_path(cs, run_path, sizeof(run_path),
                                                 uuid_ptr, run_size, DFLT_R_UUID_SOURCE,
                                                 0, 0);
  } else {
    arch_flags = archive_prepare_write_path(cs, run_path, sizeof(run_path),
                                            global->run_archive_dir, run_id,
                                            run_size, NULL, 0, 0);
  }
  if (arch_flags < 0) {
    run_undo_add_record(cs->runlog_state, run_id);
    FAIL(NEW_SRV_ERR_DISK_WRITE_ERROR);
  }

  if (generic_write_file(run_text, run_size, arch_flags, 0, run_path, "") < 0) {
    run_undo_add_record(cs->runlog_state, run_id);
    FAIL(NEW_SRV_ERR_DISK_WRITE_ERROR);
  }
  if (p_run_id) *p_run_id = run_id;

  if (accept_immediately) {
    serve_audit_log(cs, run_id, NULL, user_id, &phr->ip, phr->ssl_flag,
                    "submit", "ok", RUN_ACCEPTED, NULL);
    run_change_status_4(cs->runlog_state, run_id, RUN_ACCEPTED);
    goto done;
  }

  if ((status >= 0 && status == RUN_PENDING)
      || prob->disable_auto_testing > 0
      || (prob->disable_testing > 0 && prob->enable_compilation <= 0)
      || cs->testing_suspended) {
    serve_audit_log(cs, run_id, NULL, user_id, &phr->ip, phr->ssl_flag,
                    "submit", "ok", RUN_PENDING,
                    "  Testing disabled for this problem");
    run_change_status_4(cs->runlog_state, run_id, RUN_PENDING);
    goto done;
  }

  if (prob->type == PROB_TYPE_STANDARD) {
    if (lang->disable_auto_testing > 0 || lang->disable_testing > 0) {
      serve_audit_log(cs, run_id, NULL, user_id, &phr->ip, phr->ssl_flag,
                      "submit", "ok", RUN_PENDING,
                      "  Testing disabled for this language");
      run_change_status_4(cs->runlog_state, run_id, RUN_PENDING);
      goto done;
    }

    serve_audit_log(cs, run_id, NULL, user_id, &phr->ip, phr->ssl_flag,
                    "submit", "ok", RUN_COMPILING, NULL);
    r = serve_compile_request(cs, run_text, run_size, global->contest_id,
                              run_id, user_id,
                              lang->compile_id, variant,
                              phr->locale_id, 0 /* output_only */,
                              lang->src_sfx,
                              lang->compiler_env,
                              0 /* style_check_only */,
                              prob->style_checker_cmd,
                              prob->style_checker_env,
                              -1 /* accepting_mode */, 0 /* priority_adjustment */,
                              1 /* notify_flag */, prob, lang,
                              0 /* no_db_flag */, run_uuid, store_flags);
    if (r < 0) {
      serve_report_check_failed(ejudge_config, cnts, cs, run_id, serve_err_str(r));
      goto cleanup;
    }
    goto done;
  }

  /* manually checked problems */
  if (prob->manual_checking > 0) {
    if (prob->check_presentation <= 0) {
      serve_audit_log(cs, run_id, NULL, user_id, &phr->ip, phr->ssl_flag,
                      "submit", "ok", RUN_ACCEPTED,
                      "  This problem is checked manually");
      run_change_status_4(cs->runlog_state, run_id, RUN_ACCEPTED);
      goto done;
    }

    if (prob->style_checker_cmd && prob->style_checker_cmd[0]) {
      serve_audit_log(cs, run_id, NULL, user_id, &phr->ip, phr->ssl_flag,
                      "submit", "ok", RUN_COMPILING, NULL);
      r = serve_compile_request(cs, run_text, run_size, global->contest_id,
                                run_id, user_id, 0 /* lang_id */, variant,
                                0 /* locale_id */, 1 /* output_only */,
                                mime_type_get_suffix(mime_type),
                                NULL /* compiler_env */,
                                1 /* style_check_only */,
                                prob->style_checker_cmd,
                                prob->style_checker_env,
                                0 /* accepting_mode */,
                                0 /* priority_adjustment */,
                                0 /* notify flag */,
                                prob, NULL /* lang */,
                                0 /* no_db_flag */, run_uuid, store_flags);
      if (r < 0) {
        serve_report_check_failed(ejudge_config, cnts, cs, run_id, serve_err_str(r));
        goto cleanup;
      }
      goto done;
    }

    serve_audit_log(cs, run_id, NULL, user_id, &phr->ip, phr->ssl_flag,
                    "submit", "ok", RUN_RUNNING, NULL);
    r = serve_run_request(cs, cnts, log_f, run_text, run_size,
                          global->contest_id, run_id,
                          user_id, prob_id, 0, variant, 0, -1, -1, 1,
                          mime_type, 0, phr->locale_id, 0, 0, 0, run_uuid);
    if (r < 0) {
      serve_report_check_failed(ejudge_config, cnts, cs, run_id, serve_err_str(r));
      goto cleanup;
    }
    goto done;
  }

  /* built-in problem checker */
  problem_xml_t px = NULL;
  if (prob->variant_num > 0 && prob->xml.a && variant > 0) {
    px = prob->xml.a[variant - 1];
  } else if (prob->variant_num <= 0) {
    px = prob->xml.p;
  }
  if (px && px->ans_num > 0) {
    struct run_entry re;
    run_get_entry(cs->runlog_state, run_id, &re);
    serve_audit_log(cs, run_id, &re, user_id, &phr->ip, phr->ssl_flag,
                    "submit", "ok", RUN_RUNNING, NULL);
    serve_judge_built_in_problem(ejudge_config, cs, cnts, run_id, 1 /* judge_id */,
                                 variant, cs->accepting_mode, &re,
                                 prob, px, user_id, &phr->ip,
                                 phr->ssl_flag);
    goto done;
  }

  if (prob->style_checker_cmd && prob->style_checker_cmd[0]) {
    r = serve_compile_request(cs, run_text, run_size, cnts->id,
                              run_id, user_id, 0 /* lang_id */, variant,
                              0 /* locale_id */, 1 /* output_only */,
                              mime_type_get_suffix(mime_type),
                              NULL /* compiler_env */,
                              1 /* style_check_only */,
                              prob->style_checker_cmd,
                              prob->style_checker_env,
                              0 /* accepting_mode */,
                              0 /* priority_adjustment */,
                              0 /* notify flag */,
                              prob, NULL /* lang */,
                              0 /* no_db_flag */, run_uuid, store_flags);
    if (r < 0) {
      serve_report_check_failed(ejudge_config, cnts, cs, run_id, serve_err_str(r));
      goto cleanup;
    }
    goto done;
  }

  r = serve_run_request(cs, cnts, log_f, run_text, run_size,
                        global->contest_id, run_id,
                        user_id, prob_id, 0, variant, 0, -1, -1, 1,
                        mime_type, 0, phr->locale_id, 0, 0, 0, run_uuid);
  if (r < 0) {
    serve_report_check_failed(ejudge_config, cnts, cs, run_id, serve_err_str(r));
    goto cleanup;
  }

done:
  if (global->problem_navigation > 0) {
    int i = prob->id;
    if (prob->advance_to_next > 0) {
      for (++i; i <= cs->max_prob; ++i) {
        const struct section_problem_data *prob2 = cs->probs[i];
        if (!prob2) continue;
        if (!serve_is_problem_started(cs, user_id, prob2)) continue;
        // FIXME: standard applicability checks
        break;
      }
      if (i > cs->max_prob) i = 0;
    }
    if (p_next_prob_id) *p_next_prob_id = i;
  }

cleanup:
  xfree(ans_text);
  xfree(utf8_str);
  xfree(run_file);
  return retval;
}

static void
unpriv_submit_run(FILE *fout,
                  struct http_request_info *phr,
                  const struct contest_desc *cnts,
                  struct contest_extra *extra)
{
  serve_state_t cs = extra->serve_state;
  const struct section_global_data *global = cs->global;
  const struct section_problem_data *prob = 0, *prob2;
  const struct section_language_data *lang = 0;
  char *log_txt = 0;
  size_t log_len = 0;
  FILE *log_f = 0;
  int prob_id, n, lang_id = 0, i, ans, max_ans, j, r;
  const unsigned char *s, *run_text = 0, *text_form_text = 0;
  size_t run_size = 0, ans_size, text_form_size = 0;
  unsigned char *ans_buf, *ans_map, *ans_tmp;
  time_t start_time, stop_time, user_deadline = 0;
  const unsigned char *mime_type_str = 0;
  char **lang_list;
  int mime_type = 0;
  ruint32_t shaval[5];
  int variant = 0, run_id, arch_flags = 0;
  unsigned char *acc_probs = 0;
  struct timeval precise_time;
  path_t run_path;
  unsigned char bb[1024];
  unsigned char *tmp_run = 0;
  char *tmp_ptr = 0;
  int ans_val = 0, accept_immediately = 0;
  struct problem_plugin_iface *plg = 0;
  problem_xml_t px = 0;
  struct run_entry re;
  int skip_mime_type_test = 0;
  unsigned char *utf8_str = 0;
  int utf8_len = 0;
  int eoln_type = 0;

  l10n_setlocale(phr->locale_id);
  log_f = open_memstream(&log_txt, &log_len);

  if (ns_cgi_param(phr, "prob_id", &s) <= 0
      || sscanf(s, "%d%n", &prob_id, &n) != 1 || s[n]
      || prob_id <= 0 || prob_id > cs->max_prob
      || !(prob = cs->probs[prob_id])) {
    ns_error(log_f, NEW_SRV_ERR_INV_PROB_ID);
    goto done;
  }

  // "STANDARD" problems need programming language identifier
  if (prob->type == PROB_TYPE_STANDARD) {
    if (ns_cgi_param(phr, "lang_id", &s) <= 0
        || sscanf(s, "%d%n", &lang_id, &n) != 1 || s[n]
        || lang_id <= 0 || lang_id > cs->max_lang
        || !(lang = cs->langs[lang_id])) {
      ns_error(log_f, NEW_SRV_ERR_INV_LANG_ID);
      goto done;
    }
    if (global->enable_eoln_select > 0) {
      ns_cgi_param_int_opt(phr, "eoln_type", &eoln_type, 0);
      if (eoln_type < 0 || eoln_type > EOLN_CRLF) eoln_type = 0;
    }
  }

  switch (prob->type) {
    /*
  case PROB_TYPE_STANDARD:      // "file"
    if (!ns_cgi_param_bin(phr, "file", &run_text, &run_size)) {
      ns_error(log_f, NEW_SRV_ERR_FILE_UNSPECIFIED);
      goto done;
    }
    break;
    */
  case PROB_TYPE_STANDARD:
  case PROB_TYPE_OUTPUT_ONLY:
  case PROB_TYPE_TESTS:
    if (prob->enable_text_form > 0) {
      int r1 = ns_cgi_param_bin(phr, "file", &run_text, &run_size);
      int r2 =ns_cgi_param_bin(phr,"text_form",&text_form_text,&text_form_size);
      if (!r1 && !r2) {
        ns_error(log_f, NEW_SRV_ERR_FILE_UNSPECIFIED);
        goto done;
      }
    } else {
      if (!ns_cgi_param_bin(phr, "file", &run_text, &run_size)) {
        ns_error(log_f, NEW_SRV_ERR_FILE_UNSPECIFIED);
        goto done;
      }
    }
    break;
  case PROB_TYPE_TEXT_ANSWER:
  case PROB_TYPE_SHORT_ANSWER:
  case PROB_TYPE_SELECT_ONE:
    if (!ns_cgi_param_bin(phr, "file", &run_text, &run_size)) {
      ns_error(log_f, NEW_SRV_ERR_ANSWER_UNSPECIFIED);
      goto done;
    }
    break;
  case PROB_TYPE_SELECT_MANY:   // "ans_*"
    for (i = 0, max_ans = -1, ans_size = 0; i < phr->param_num; i++)
      if (!strncmp(phr->param_names[i], "ans_", 4)) {
        if (sscanf(phr->param_names[i] + 4, "%d%n", &ans, &n) != 1
            || phr->param_names[i][4 + n]
            || ans < 0 || ans > 65535) {
          ns_error(log_f, NEW_SRV_ERR_INV_ANSWER);
          goto done;
        }
        if (ans > max_ans) max_ans = ans;
        ans_size += 7;
      }
    if (max_ans < 0) {
      run_text = "";
      run_size = 0;
      break;
    }
    XALLOCAZ(ans_map, max_ans + 1);
    for (i = 0; i < phr->param_num; i++)
      if (!strncmp(phr->param_names[i], "ans_", 4)) {
        sscanf(phr->param_names[i] + 4, "%d", &ans);
        ans_map[ans] = 1;
      }
    XALLOCA(ans_buf, ans_size);
    run_text = ans_buf;
    for (i = 0, run_size = 0; i <= max_ans; i++)
      if (ans_map[i]) {
        if (run_size > 0) ans_buf[run_size++] = ' ';
        run_size += sprintf(ans_buf + run_size, "%d", i);
      }
    ans_buf[run_size++] = '\n';
    ans_buf[run_size] = 0;
    break;
  case PROB_TYPE_CUSTOM:
    // invoke problem plugin
    load_problem_plugin(cs, prob_id);
    if (!(plg = cs->prob_extras[prob_id].plugin) || !plg->parse_form) {
      ns_error(log_f, NEW_SRV_ERR_PLUGIN_NOT_AVAIL);
      goto done;
    }
    ans_tmp = (*plg->parse_form)(cs->prob_extras[prob_id].plugin_data,
                                 log_f, phr, cnts, extra);
    if (!ans_tmp) goto done;
    run_size = strlen(ans_tmp);
    ans_buf = (unsigned char*) alloca(run_size + 1);
    strcpy(ans_buf, ans_tmp);
    run_text = ans_buf;
    xfree(ans_tmp);
    break;
  default:
    abort();
  }

  switch (prob->type) {
  case PROB_TYPE_STANDARD:
    if (!lang->binary && strlen(run_text) != run_size) {
      // guess utf-16/ucs-2
      if (((int) run_size) < 0
          || (utf8_len = ucs2_to_utf8(&utf8_str, run_text, run_size)) < 0) {
        ns_error(log_f, NEW_SRV_ERR_BINARY_FILE);
        goto done;
      }
      run_text = utf8_str;
      run_size = (size_t) utf8_len;
    }
    if (prob->enable_text_form > 0 && text_form_text
        && strlen(text_form_text) != text_form_size) {
      ns_error(log_f, NEW_SRV_ERR_BINARY_FILE);
      goto done;
    }
    if (prob->enable_text_form) {
      if (!run_size && !text_form_size) {
        ns_error(log_f, NEW_SRV_ERR_SUBMIT_EMPTY);
        goto done;
      }
      if (!run_size) {
        run_text = text_form_text;
        run_size = text_form_size;
        skip_mime_type_test = 1;
      } else {
        text_form_text = 0;
        text_form_size = 0;
      }
    } else if (!run_size) {
      ns_error(log_f, NEW_SRV_ERR_SUBMIT_EMPTY);
      goto done;
    }
    if (prob->disable_ctrl_chars > 0 && has_control_characters(run_text)) {
      ns_error(log_f, NEW_SRV_ERR_INV_CHAR);
      goto done;
    }
    break;
  case PROB_TYPE_OUTPUT_ONLY:
  case PROB_TYPE_TESTS:
    if (!prob->binary_input && !prob->binary && strlen(run_text) != run_size) {
      ns_error(log_f, NEW_SRV_ERR_BINARY_FILE);
      goto done;
    }
    if (prob->enable_text_form > 0 && text_form_text
        && strlen(text_form_text) != text_form_size) {
      ns_error(log_f, NEW_SRV_ERR_BINARY_FILE);
      goto done;
    }
    if (prob->enable_text_form > 0) {
      if (!run_size && !text_form_size) {
        ns_error(log_f, NEW_SRV_ERR_SUBMIT_EMPTY);
        goto done;
      }
      if (!run_size) {
        run_text = text_form_text;
        run_size = text_form_size;
        skip_mime_type_test = 1;
      } else {
        text_form_text = 0;
        text_form_size = 0;
      }
    }
    break;

  case PROB_TYPE_TEXT_ANSWER:
  case PROB_TYPE_SHORT_ANSWER:
  case PROB_TYPE_SELECT_ONE:
    if (strlen(run_text) != run_size) {
      ns_error(log_f, NEW_SRV_ERR_BINARY_FILE);
      goto done;
    }
    if (!run_size) {
      ns_error(log_f, NEW_SRV_ERR_SUBMIT_EMPTY);
      goto done;
    }
    break;

  case PROB_TYPE_SELECT_MANY:
    if (strlen(run_text) != run_size) {
      ns_error(log_f, NEW_SRV_ERR_BINARY_FILE);
      goto done;
    }
    break;

  case PROB_TYPE_CUSTOM:
    break;
  }

  // ignore BOM
  if (global->ignore_bom > 0 && !prob->binary && (!lang || !lang->binary)) {
    if (run_text && run_size >= 3 && run_text[0] == 0xef
        && run_text[1] == 0xbb && run_text[2] == 0xbf) {
      run_text += 3; run_size -= 3;
    }
  }

  if (global->is_virtual) {
    start_time = run_get_virtual_start_time(cs->runlog_state, phr->user_id);
    stop_time = run_get_virtual_stop_time(cs->runlog_state, phr->user_id,
                                          cs->current_time);
  } else {
    start_time = run_get_start_time(cs->runlog_state);
    stop_time = run_get_stop_time(cs->runlog_state);
  }

  if (cs->clients_suspended) {
    ns_error(log_f, NEW_SRV_ERR_CLIENTS_SUSPENDED);
    goto done;
  }
  if (!start_time) {
    ns_error(log_f, NEW_SRV_ERR_CONTEST_NOT_STARTED);
    goto done;
  }
  if (stop_time) {
    ns_error(log_f, NEW_SRV_ERR_CONTEST_ALREADY_FINISHED);
    goto done;
  }
  if (serve_check_user_quota(cs, phr->user_id, run_size) < 0) {
    ns_error(log_f, NEW_SRV_ERR_RUN_QUOTA_EXCEEDED);
    goto done;
  }
  // problem submit start time
  if (!serve_is_problem_started(cs, phr->user_id, prob)) {
    ns_error(log_f, NEW_SRV_ERR_PROB_UNAVAILABLE);
    goto done;
  }
  if (serve_is_problem_deadlined(cs, phr->user_id, phr->login, prob,
                                 &user_deadline)) {
    ns_error(log_f, NEW_SRV_ERR_PROB_DEADLINE_EXPIRED);
    goto done;
  }

  if (prob->max_user_run_count > 0) {
    int ignored_set = 0;
    if (prob->ignore_compile_errors > 0) ignored_set |= 1 << RUN_COMPILE_ERR;
    ignored_set |= 1 << RUN_IGNORED;
    if (run_count_all_attempts_2(cs->runlog_state, phr->user_id, prob_id, ignored_set) >= prob->max_user_run_count) {
      ns_error(log_f, NEW_SRV_ERR_PROB_TOO_MANY_ATTEMPTS);
      goto done;
    }
  }

  /* check for disabled languages */
  if (lang_id > 0) {
    if (lang->disabled || (lang->insecure > 0 && global->secure_run)) {
      ns_error(log_f, NEW_SRV_ERR_LANG_DISABLED);
      goto done;
    }

    if (prob->enable_language) {
      lang_list = prob->enable_language;
      for (i = 0; lang_list[i]; i++)
        if (!strcmp(lang_list[i], lang->short_name))
          break;
      if (!lang_list[i]) {
        ns_error(log_f, NEW_SRV_ERR_LANG_NOT_AVAIL_FOR_PROBLEM);
        goto done;
      }
    } else if (prob->disable_language) {
      lang_list = prob->disable_language;
      for (i = 0; lang_list[i]; i++)
        if (!strcmp(lang_list[i], lang->short_name))
          break;
      if (lang_list[i]) {
        ns_error(log_f, NEW_SRV_ERR_LANG_DISABLED_FOR_PROBLEM);
        goto done;
      }
    }
  } else if (skip_mime_type_test) {
    mime_type = 0;
    mime_type_str = mime_type_get_type(mime_type);
  } else {
    // guess the content-type and check it against the list
    if ((mime_type = mime_type_guess(cs->global->diff_work_dir,
                                     run_text, run_size)) < 0) {
      ns_error(log_f, NEW_SRV_ERR_CANNOT_DETECT_CONTENT_TYPE);
      goto done;
    }
    mime_type_str = mime_type_get_type(mime_type);
    if (prob->enable_language) {
      lang_list = prob->enable_language;
      for (i = 0; lang_list[i]; i++)
        if (!strcmp(lang_list[i], mime_type_str))
          break;
      if (!lang_list[i]) {
        ns_error(log_f, NEW_SRV_ERR_CONTENT_TYPE_NOT_AVAILABLE, mime_type_str);
        goto done;
      }
    } else if (prob->disable_language) {
      lang_list = prob->disable_language;
      for (i = 0; lang_list[i]; i++)
        if (!strcmp(lang_list[i], mime_type_str))
          break;
      if (lang_list[i]) {
        ns_error(log_f, NEW_SRV_ERR_CONTENT_TYPE_DISABLED, mime_type_str);
        goto done;
      }
    }
  }

  if (prob->variant_num > 0) {
    if ((variant = find_variant(cs, phr->user_id, prob_id, 0)) <= 0) {
      ns_error(log_f, NEW_SRV_ERR_VARIANT_UNASSIGNED);
      goto done;
    }
  }

  sha_buffer(run_text, run_size, shaval);
  if (global->ignore_duplicated_runs != 0) {
    if ((run_id = run_find_duplicate(cs->runlog_state, phr->user_id, prob_id,
                                     lang_id, variant, run_size, shaval)) >= 0){
      ns_error(log_f, NEW_SRV_ERR_DUPLICATE_SUBMIT, run_id);
      goto done;
    }
  }

  if (prob->disable_submit_after_ok
      && global->score_system != SCORE_OLYMPIAD && !cs->accepting_mode) {
    XALLOCAZ(acc_probs, cs->max_prob + 1);
    run_get_accepted_set(cs->runlog_state, phr->user_id,
                         cs->accepting_mode, cs->max_prob, acc_probs);
    if (acc_probs[prob_id]) {
      ns_error(log_f, NEW_SRV_ERR_PROB_ALREADY_SOLVED);
      goto done;
    }
  }

  if (prob->require) {
    if (!acc_probs) {
      XALLOCAZ(acc_probs, cs->max_prob + 1);
      run_get_accepted_set(cs->runlog_state, phr->user_id,
                           cs->accepting_mode, cs->max_prob, acc_probs);
    }
    for (i = 0; prob->require[i]; i++) {
      for (j = 1; j <= cs->max_prob; j++)
        if (cs->probs[j] && !strcmp(cs->probs[j]->short_name, prob->require[i]))
          break;
      if (j > cs->max_prob || !acc_probs[j]) break;
    }
    if (prob->require[i]) {
      ns_error(log_f, NEW_SRV_ERR_NOT_ALL_REQ_SOLVED);
      goto done;
    }
  }

  if (prob->type == PROB_TYPE_SELECT_ONE) {
    // check that answer is valid
    tmp_run = (unsigned char*) alloca(run_size + 1);
    memcpy(tmp_run, run_text, run_size);
    tmp_run[run_size] = 0;
    while (run_size > 0 && isspace(tmp_run[run_size - 1])) run_size--;
    tmp_run[run_size] = 0;
    errno = 0;
    ans_val = strtol(tmp_run, &tmp_ptr, 10);
    if (errno || *tmp_ptr || tmp_run + run_size != (unsigned char*) tmp_ptr
        || ans_val < 0) {
      ns_error(log_f, NEW_SRV_ERR_INV_ANSWER);
      goto done;
    }

    // add this run and if we're in olympiad accepting mode mark
    // as accepted
    if (global->score_system == SCORE_OLYMPIAD && cs->accepting_mode)
      accept_immediately = 1;
  }

  // OK, so all checks are done, now we add this submit to the database
  gettimeofday(&precise_time, 0);

  ruint32_t run_uuid[4];
  int store_flags = 0;
  ej_uuid_generate(run_uuid);
  if (global->uuid_run_store > 0 && run_get_uuid_hash_state(cs->runlog_state) >= 0 && ej_uuid_is_nonempty(run_uuid)) {
    store_flags = 1;
  }
  run_id = run_add_record(cs->runlog_state, 
                          precise_time.tv_sec, precise_time.tv_usec * 1000,
                          run_size, shaval, run_uuid,
                          &phr->ip, phr->ssl_flag,
                          phr->locale_id, phr->user_id,
                          prob_id, lang_id, eoln_type, 0, 0, mime_type, store_flags);
  if (run_id < 0) {
    ns_error(log_f, NEW_SRV_ERR_RUNLOG_UPDATE_FAILED);
    goto done;
  }
  serve_move_files_to_insert_run(cs, run_id);

  if (store_flags == 1) {
    arch_flags = uuid_archive_prepare_write_path(cs, run_path, sizeof(run_path),
                                                 run_uuid, run_size, DFLT_R_UUID_SOURCE,
                                                 0, 0);
  } else {
    arch_flags = archive_prepare_write_path(cs, run_path, sizeof(run_path),
                                            global->run_archive_dir, run_id,
                                            run_size, NULL, 0, 0);

  }
  if (arch_flags < 0) {
    run_undo_add_record(cs->runlog_state, run_id);
    ns_error(log_f, NEW_SRV_ERR_DISK_WRITE_ERROR);
    goto done;
  }

  if (generic_write_file(run_text, run_size, arch_flags, 0, run_path, "") < 0) {
    run_undo_add_record(cs->runlog_state, run_id);
    ns_error(log_f, NEW_SRV_ERR_DISK_WRITE_ERROR);
    goto done;
  }

  if (prob->type == PROB_TYPE_STANDARD) {
    if (prob->disable_auto_testing > 0
        || (prob->disable_testing > 0 && prob->enable_compilation <= 0)
        || lang->disable_auto_testing || lang->disable_testing
        || cs->testing_suspended) {
      serve_audit_log(cs, run_id, NULL, phr->user_id, &phr->ip, phr->ssl_flag,
                      "submit", "ok", RUN_PENDING,
                      "  Testing disabled for this problem or language");
      run_change_status_4(cs->runlog_state, run_id, RUN_PENDING);
    } else {
      serve_audit_log(cs, run_id, NULL, phr->user_id, &phr->ip, phr->ssl_flag,
                      "submit", "ok", RUN_COMPILING, NULL);
      if ((r = serve_compile_request(cs, run_text, run_size, global->contest_id,
                                     run_id, phr->user_id,
                                     lang->compile_id, variant,
                                     phr->locale_id, 0,
                                     lang->src_sfx,
                                     lang->compiler_env,
                                     0, prob->style_checker_cmd,
                                     prob->style_checker_env,
                                     -1, 0, 1, prob, lang, 0, run_uuid, store_flags)) < 0) {
        serve_report_check_failed(ejudge_config, cnts, cs, run_id, serve_err_str(r));
      }
    }
  } else if (prob->manual_checking > 0 && !accept_immediately) {
    // manually tested outputs
    if (prob->check_presentation <= 0) {
      serve_audit_log(cs, run_id, NULL, phr->user_id, &phr->ip, phr->ssl_flag,
                      "submit", "ok", RUN_ACCEPTED,
                      "  This problem is checked manually");
      run_change_status_4(cs->runlog_state, run_id, RUN_ACCEPTED);
    } else {
      serve_audit_log(cs, run_id, NULL, phr->user_id, &phr->ip, phr->ssl_flag,
                      "submit", "ok", RUN_COMPILING, NULL);
      if (prob->style_checker_cmd && prob->style_checker_cmd[0]) {
        r = serve_compile_request(cs, run_text, run_size, global->contest_id,
                                  run_id, phr->user_id, 0 /* lang_id */, variant,
                                  0 /* locale_id */, 1 /* output_only*/,
                                  mime_type_get_suffix(mime_type),
                                  NULL /* compiler_env */,
                                  1 /* style_check_only */,
                                  prob->style_checker_cmd,
                                  prob->style_checker_env,
                                  0 /* accepting_mode */,
                                  0 /* priority_adjustment */,
                                  0 /* notify flag */,
                                  prob, NULL /* lang */,
                                  0 /* no_db_flag */, run_uuid, store_flags);
        if (r < 0) {
          serve_report_check_failed(ejudge_config, cnts, cs, run_id, serve_err_str(r));
        }
      } else {
        if (serve_run_request(cs, cnts, log_f, run_text, run_size,
                              global->contest_id, run_id,
                              phr->user_id, prob_id, 0, variant, 0, -1, -1, 1,
                              mime_type, 0, phr->locale_id, 0, 0, 0, run_uuid) < 0) {
          ns_error(log_f, NEW_SRV_ERR_DISK_WRITE_ERROR);
          goto done;
        }
      }
    }
  } else {
    if (accept_immediately) {
      serve_audit_log(cs, run_id, NULL, phr->user_id, &phr->ip, phr->ssl_flag,
                      "submit", "ok", RUN_ACCEPTED, NULL);
      run_change_status_4(cs->runlog_state, run_id, RUN_ACCEPTED);
    } else if (prob->disable_auto_testing > 0
        || (prob->disable_testing > 0 && prob->enable_compilation <= 0)) {
      serve_audit_log(cs, run_id, NULL, phr->user_id, &phr->ip, phr->ssl_flag,
                      "submit", "ok", RUN_PENDING,
                      "  Testing disabled for this problem");
      run_change_status_4(cs->runlog_state, run_id, RUN_PENDING);
    } else {
      if (prob->variant_num > 0 && prob->xml.a) {
        px = prob->xml.a[variant -  1];
      } else {
        px = prob->xml.p;
      }
      if (px && px->ans_num > 0) {
        run_get_entry(cs->runlog_state, run_id, &re);
        serve_audit_log(cs, run_id, NULL, phr->user_id, &phr->ip, phr->ssl_flag,
                        "submit", "ok", RUN_RUNNING, NULL);
        serve_judge_built_in_problem(ejudge_config, cs, cnts, run_id, 1 /* judge_id */,
                                     variant, cs->accepting_mode, &re,
                                     prob, px, phr->user_id, &phr->ip,
                                     phr->ssl_flag);
        goto done;
      }

      if (prob->style_checker_cmd && prob->style_checker_cmd[0]) {
        serve_audit_log(cs, run_id, NULL, phr->user_id, &phr->ip, phr->ssl_flag,
                        "submit", "ok", RUN_COMPILING, NULL);

        r = serve_compile_request(cs, run_text, run_size, global->contest_id,
                                  run_id, phr->user_id, 0 /* lang_id */, variant,
                                  0 /* locale_id */, 1 /* output_only*/,
                                  mime_type_get_suffix(mime_type),
                                  NULL /* compiler_env */,
                                  1 /* style_check_only */,
                                  prob->style_checker_cmd,
                                  prob->style_checker_env,
                                  0 /* accepting_mode */,
                                  0 /* priority_adjustment */,
                                  0 /* notify flag */,
                                  prob, NULL /* lang */,
                                  0 /* no_db_flag */, run_uuid, store_flags);
        if (r < 0) {
          serve_report_check_failed(ejudge_config, cnts, cs, run_id, serve_err_str(r));
        }
      } else {
        serve_audit_log(cs, run_id, NULL, phr->user_id, &phr->ip, phr->ssl_flag,
                        "submit", "ok", RUN_RUNNING, NULL);

        if (serve_run_request(cs, cnts, log_f, run_text, run_size,
                              global->contest_id, run_id,
                              phr->user_id, prob_id, 0, variant, 0, -1, -1, 1,
                              mime_type, 0, phr->locale_id, 0, 0, 0, run_uuid) < 0) {
          ns_error(log_f, NEW_SRV_ERR_DISK_WRITE_ERROR);
          goto done;
        }
      }
    }
  }

 done:;
  l10n_setlocale(0);
  close_memstream(log_f); log_f = 0;
  if (!log_txt || !*log_txt) {
    i = 0;
    if (global->problem_navigation) {
      i = prob->id;
      if (prob->advance_to_next > 0) {
        for (i++; i <= cs->max_prob; i++) {
          if (!(prob2 = cs->probs[i])) continue;
          if (!serve_is_problem_started(cs, phr->user_id, prob2))
            continue;
          // FIXME: standard applicability checks
          break;
        }
        if (i > cs->max_prob) i = 0;
      }
    }
    if (i > 0) {
      snprintf(bb, sizeof(bb), "prob_id=%d", i);
      ns_refresh_page(fout, phr, NEW_SRV_ACTION_VIEW_PROBLEM_SUBMIT, bb);
    }  else {
      ns_refresh_page(fout, phr, NEW_SRV_ACTION_VIEW_SUBMISSIONS, 0);
    }
  } else {
    unpriv_load_html_style(phr, cnts, 0, 0);
    html_error_status_page(fout, phr, cnts, extra, log_txt,
                           NEW_SRV_ACTION_VIEW_PROBLEM_SUBMIT,
                           "prob_id=%d", prob_id);
  }

  //cleanup:;
  if (log_f) fclose(log_f);
  xfree(log_txt);
  xfree(utf8_str);
}

static void
unpriv_submit_clar(FILE *fout,
                   struct http_request_info *phr,
                   const struct contest_desc *cnts,
                   struct contest_extra *extra)
{
  serve_state_t cs = extra->serve_state;
  const struct section_global_data *global = cs->global;
  const struct section_problem_data *prob = 0;
  const unsigned char *s, *subject = 0, *text = 0;
  int prob_id = 0, n;
  time_t start_time, stop_time;
  FILE *log_f = 0;
  char *log_txt = 0;
  size_t log_len = 0;
  size_t subj_len, text_len, subj3_len, text3_len;
  unsigned char *subj2, *text2, *subj3, *text3;
  struct timeval precise_time;
  int clar_id;

  // parameters: prob_id, subject, text,  

  if ((n = ns_cgi_param(phr, "prob_id", &s)) < 0)
    return ns_html_err_inv_param(fout, phr, 0, "prob_id is binary");
  if (n > 0 && *s) {
    if (sscanf(s, "%d%n", &prob_id, &n) != 1 || s[n])
      return ns_html_err_inv_param(fout, phr, 0, "cannot parse prob_id");
    if (prob_id <= 0 || prob_id > cs->max_prob || !(prob = cs->probs[prob_id]))
      return ns_html_err_inv_param(fout, phr, 0, "prob_id is invalid");
  }
  if (ns_cgi_param(phr, "subject", &subject) < 0)
    return ns_html_err_inv_param(fout, phr, 0, "subject is binary");
  if (ns_cgi_param(phr, "text", &text) <= 0)
    return ns_html_err_inv_param(fout, phr, 0,
                                 "text is not set or binary");

  if (global->is_virtual) {
    start_time = run_get_virtual_start_time(cs->runlog_state, phr->user_id);
    stop_time = run_get_virtual_stop_time(cs->runlog_state, phr->user_id,
                                          cs->current_time);
  } else {
    start_time = run_get_start_time(cs->runlog_state);
    stop_time = run_get_stop_time(cs->runlog_state);
  }

  log_f = open_memstream(&log_txt, &log_len);

  if (cs->clients_suspended) {
    ns_error(log_f, NEW_SRV_ERR_CLIENTS_SUSPENDED);
    goto done;
  }
  if (global->disable_team_clars) {
    ns_error(log_f, NEW_SRV_ERR_CLARS_DISABLED);
    goto done;
  }
  if (!start_time) {
    ns_error(log_f, NEW_SRV_ERR_CONTEST_NOT_STARTED);
    goto done;
  }
  if (stop_time) {
    ns_error(log_f, NEW_SRV_ERR_CONTEST_ALREADY_FINISHED);
    goto done;
  }

  if (!subject) subject = "";
  subj_len = strlen(subject);
  if (subj_len > 128 * 1024 * 1024) {
    ns_error(log_f, NEW_SRV_ERR_SUBJECT_TOO_LONG, subj_len);
    goto done;
  }
  subj2 = alloca(subj_len + 1);
  strcpy(subj2, subject);
  while (subj_len > 0 && isspace(subj2[subj_len - 1])) subj2[--subj_len] = 0;
  if (!subj_len) {
    subj2 = "(no subject)";
    subj_len = strlen(subj2);
  }

  if (!text) text = "";
  text_len = strlen(text);
  if (text_len > 128 * 1024 * 1024) {
    ns_error(log_f, NEW_SRV_ERR_MESSAGE_TOO_LONG, text_len);
    goto done;
  }
  text2 = alloca(text_len + 1);
  strcpy(text2, text);
  while (text_len > 0 && isspace(text2[text_len - 1])) text2[--text_len] = 0;
  if (!text_len) {
    ns_error(log_f, NEW_SRV_ERR_MESSAGE_EMPTY);
    goto done;
  }

  if (prob) {
    subj3 = alloca(strlen(prob->short_name) + subj_len + 10);
    subj3_len = sprintf(subj3, "%s: %s", prob->short_name, subj2);
  } else {
    subj3 = subj2;
    subj3_len = subj_len;
  }

  text3 = alloca(subj3_len + text_len + 32);
  text3_len = sprintf(text3, "Subject: %s\n\n%s\n", subj3, text2);

  if (serve_check_clar_quota(cs, phr->user_id, text3_len) < 0) {
    ns_error(log_f, NEW_SRV_ERR_CLAR_QUOTA_EXCEEDED);
    goto done;
  }

  gettimeofday(&precise_time, 0);
  if ((clar_id = clar_add_record(cs->clarlog_state,
                                 precise_time.tv_sec,
                                 precise_time.tv_usec * 1000,
                                 text3_len,
                                 &phr->ip,
                                 phr->ssl_flag,
                                 phr->user_id, 0, 0, 0, 0,
                                 phr->locale_id, 0, 0, 0,
                                 utf8_mode, NULL, subj3)) < 0) {
    ns_error(log_f, NEW_SRV_ERR_CLARLOG_UPDATE_FAILED);
    goto done;
  }

  if (clar_add_text(cs->clarlog_state, clar_id, text3, text3_len) < 0) {
    ns_error(log_f, NEW_SRV_ERR_DISK_WRITE_ERROR);
    goto done;
  }

  serve_send_clar_notify_email(ejudge_config, cs, cnts, phr->user_id, phr->name, subj3, text2);

 done:;
  close_memstream(log_f); log_f = 0;
  if (!log_txt || !*log_txt) {
    ns_refresh_page(fout, phr, NEW_SRV_ACTION_VIEW_CLARS, 0);
  } else {
    html_error_status_page(fout, phr, cnts, extra, log_txt,
                           NEW_SRV_ACTION_VIEW_CLAR_SUBMIT, 0);
  }

  //cleanup:;
  if (log_f) close_memstream(log_f);
  xfree(log_txt);
}

static void
unpriv_submit_appeal(FILE *fout,
                     struct http_request_info *phr,
                     const struct contest_desc *cnts,
                     struct contest_extra *extra)
{
  serve_state_t cs = extra->serve_state;
  const struct section_global_data *global = cs->global;
  const struct section_problem_data *prob = 0;
  const unsigned char *s, *text = 0;
  int prob_id = 0, n;
  time_t start_time, stop_time;
  FILE *log_f = 0;
  char *log_txt = 0;
  size_t log_len = 0;
  size_t text_len, subj3_len, text3_len;
  unsigned char *text2, *subj3, *text3;
  struct timeval precise_time;
  int clar_id, test;

  // parameters: prob_id, subject, text,  

  if ((n = ns_cgi_param(phr, "prob_id", &s)) < 0)
    return ns_html_err_inv_param(fout, phr, 0, "prob_id is binary");
  if (n > 0 && *s) {
    if (sscanf(s, "%d%n", &prob_id, &n) != 1 || s[n])
      return ns_html_err_inv_param(fout, phr, 0, "cannot parse prob_id");
    if (prob_id <= 0 || prob_id > cs->max_prob || !(prob = cs->probs[prob_id]))
      return ns_html_err_inv_param(fout, phr, 0, "prob_id is invalid");
  }
  if ((n = ns_cgi_param(phr, "test", &s)) < 0)
    return ns_html_err_inv_param(fout, phr, 0, "test is binary");
  if (ns_cgi_param(phr, "text", &text) <= 0)
    return ns_html_err_inv_param(fout, phr, 0,
                                 "text is not set or binary");

  if (global->is_virtual) {
    start_time = run_get_virtual_start_time(cs->runlog_state, phr->user_id);
    stop_time = run_get_virtual_stop_time(cs->runlog_state, phr->user_id,
                                          cs->current_time);
  } else {
    start_time = run_get_start_time(cs->runlog_state);
    stop_time = run_get_stop_time(cs->runlog_state);
  }

  log_f = open_memstream(&log_txt, &log_len);

  if (cs->clients_suspended) {
    ns_error(log_f, NEW_SRV_ERR_CLIENTS_SUSPENDED);
    goto done;
  }
  if (global->disable_team_clars) {
    ns_error(log_f, NEW_SRV_ERR_CLARS_DISABLED);
    goto done;
  }
  if (!start_time) {
    ns_error(log_f, NEW_SRV_ERR_CONTEST_NOT_STARTED);
    goto done;
  }
  if (!stop_time) {
    ns_error(log_f, NEW_SRV_ERR_CONTEST_NOT_FINISHED);
    goto done;
  }
  if (global->appeal_deadline <= 0) {
    ns_error(log_f, NEW_SRV_ERR_APPEALS_DISABLED);
    goto done;
  }
  if (cs->current_time >= global->appeal_deadline) {
    ns_error(log_f, NEW_SRV_ERR_APPEALS_FINISHED);
    goto done;
  }
  if (ns_cgi_param(phr, "test", &s) <= 0
      || sscanf(s, "%d%n", &test, &n) != 1 || s[n]
      || test <= 0 || test > 100000) {
    ns_error(log_f, NEW_SRV_ERR_INV_TEST);
    goto done;
  }
  if (!prob) {
    ns_error(log_f, NEW_SRV_ERR_INV_PROB_ID);
    goto done;
  }

  if (!text) text = "";
  text_len = strlen(text);
  if (text_len > 128 * 1024 * 1024) {
    ns_error(log_f, NEW_SRV_ERR_MESSAGE_TOO_LONG, text_len);
    goto done;
  }
  text2 = alloca(text_len + 1);
  strcpy(text2, text);
  while (text_len > 0 && isspace(text2[text_len - 1])) text2[--text_len] = 0;
  if (!text_len) {
    ns_error(log_f, NEW_SRV_ERR_MESSAGE_EMPTY);
    goto done;
  }

  subj3 = alloca(strlen(prob->short_name) + 128);
  subj3_len = sprintf(subj3, "Appeal: %s, %d", prob->short_name, test);

  text3 = alloca(subj3_len + text_len + 32);
  text3_len = sprintf(text3, "Subject: %s\n\n%s\n", subj3, text2);

  if (serve_check_clar_quota(cs, phr->user_id, text3_len) < 0) {
    ns_error(log_f, NEW_SRV_ERR_CLAR_QUOTA_EXCEEDED);
    goto done;
  }

  gettimeofday(&precise_time, 0);
  if ((clar_id = clar_add_record(cs->clarlog_state,
                                 precise_time.tv_sec,
                                 precise_time.tv_usec * 1000,
                                 text3_len,
                                 &phr->ip,
                                 phr->ssl_flag,
                                 phr->user_id, 0, 0, 0, 0,
                                 phr->locale_id, 0, 0, 1,
                                 utf8_mode, NULL, subj3)) < 0) {
    ns_error(log_f, NEW_SRV_ERR_CLARLOG_UPDATE_FAILED);
    goto done;
  }

  if (clar_add_text(cs->clarlog_state, clar_id, text3, text3_len) < 0) {
    ns_error(log_f, NEW_SRV_ERR_DISK_WRITE_ERROR);
    goto done;
  }

  serve_send_clar_notify_email(ejudge_config, cs, cnts, phr->user_id, phr->name, subj3, text2);

 done:;
  close_memstream(log_f); log_f = 0;
  if (!log_txt || !*log_txt) {
    ns_refresh_page(fout, phr, NEW_SRV_ACTION_VIEW_CLARS, 0);
  } else {
    html_error_status_page(fout, phr, cnts, extra, log_txt,
                           NEW_SRV_ACTION_VIEW_CLAR_SUBMIT, 0);
  }

  //cleanup:;
  if (log_f) close_memstream(log_f);
  xfree(log_txt);
}

static void
virtual_stop_callback(
        const struct contest_desc *cnts,
        struct serve_state *cs,
        struct serve_event_queue *p)
{
  const struct section_global_data *global = cs->global;

  char *tmps = 0;
  size_t tmpz = 0;
  FILE *tmpf = 0;
  int locale_id = 0;

  if (global->enable_auto_print_protocol <= 0) return;

  // Note, that all printing errors are ignored... 
  if (cnts->default_locale_num > 0) locale_id = cnts->default_locale_num;
  if (locale_id > 0) l10n_setlocale(locale_id);
  tmpf = open_memstream(&tmps, &tmpz);
  ns_print_user_exam_protocol(cnts, cs, tmpf, p->user_id, locale_id, 1, 0, 0);
  close_memstream(tmpf); tmpf = 0;
  xfree(tmps); tmps = 0; tmpz = 0;
  if (locale_id > 0) l10n_setlocale(0);
}

static void
unpriv_command(FILE *fout,
               struct http_request_info *phr,
               const struct contest_desc *cnts,
               struct contest_extra *extra)
{
  serve_state_t cs = extra->serve_state;
  const struct section_global_data *global = cs->global;
  const struct section_problem_data *prob;
  char *log_txt = 0;
  size_t log_size = 0;
  FILE *log_f = 0;
  time_t start_time, stop_time;
  struct timeval precise_time;
  int run_id, i;
  unsigned char bb[1024];

  l10n_setlocale(phr->locale_id);
  log_f = open_memstream(&log_txt, &log_size);

  switch (phr->action) {
  case NEW_SRV_ACTION_VIRTUAL_START:
  case NEW_SRV_ACTION_VIRTUAL_STOP:
    if (global->is_virtual <= 0) {
      ns_error(log_f, NEW_SRV_ERR_NOT_VIRTUAL);
      goto done;
    }
    if (run_get_start_time(cs->runlog_state) <= 0) {
      ns_error(log_f, NEW_SRV_ERR_VIRTUAL_NOT_STARTED);
      goto done;
    }
    break;
  default:
    ns_error(log_f, NEW_SRV_ERR_UNHANDLED_ACTION, phr->action);
    goto done;
  }

  switch (phr->action) {
  case NEW_SRV_ACTION_VIRTUAL_START:
    if (global->disable_virtual_start) {
      ns_error(log_f, NEW_SRV_ERR_PERMISSION_DENIED);
      goto done;
    }
    if (cnts->open_time > 0 && cs->current_time < cnts->open_time) {
      ns_error(log_f, NEW_SRV_ERR_PERMISSION_DENIED);
      goto done;
    }
    if (cnts->close_time > 0 && cs->current_time >= cnts->close_time) {
      ns_error(log_f, NEW_SRV_ERR_PERMISSION_DENIED);
      goto done;
    }
    start_time = run_get_virtual_start_time(cs->runlog_state, phr->user_id);
    if (start_time > 0) {
      ns_error(log_f, NEW_SRV_ERR_CONTEST_ALREADY_STARTED);
      goto done;
    }
    gettimeofday(&precise_time, 0);
    run_id = run_virtual_start(cs->runlog_state, phr->user_id,
                               precise_time.tv_sec, &phr->ip, phr->ssl_flag,
                               precise_time.tv_usec * 1000);
    if (run_id < 0) {
      ns_error(log_f, NEW_SRV_ERR_RUNLOG_UPDATE_FAILED);
      goto done;
    }
    serve_move_files_to_insert_run(cs, run_id);
    serve_event_add(cs,
                    precise_time.tv_sec + run_get_duration(cs->runlog_state),
                    SERVE_EVENT_VIRTUAL_STOP, phr->user_id,
                    virtual_stop_callback);
    break;
  case NEW_SRV_ACTION_VIRTUAL_STOP:
    start_time = run_get_virtual_start_time(cs->runlog_state, phr->user_id);
    if (start_time <= 0) {
      ns_error(log_f, NEW_SRV_ERR_CONTEST_NOT_STARTED);
      goto done;
    }
    stop_time = run_get_virtual_stop_time(cs->runlog_state, phr->user_id,
                                          cs->current_time);
    if (stop_time > 0) {
      ns_error(log_f, NEW_SRV_ERR_CONTEST_ALREADY_FINISHED);
      goto done;
    }
    gettimeofday(&precise_time, 0);
    run_id = run_virtual_stop(cs->runlog_state, phr->user_id,
                              precise_time.tv_sec, &phr->ip, phr->ssl_flag,
                              precise_time.tv_usec * 1000);
    if (run_id < 0) {
      ns_error(log_f, NEW_SRV_ERR_RUNLOG_UPDATE_FAILED);
      goto done;
    }
    serve_move_files_to_insert_run(cs, run_id);
    if (global->score_system == SCORE_OLYMPIAD && global->is_virtual > 0) {
      serve_event_remove_matching(cs, 0, 0, phr->user_id);
      if (global->disable_virtual_auto_judge <= 0) {
        serve_event_add(cs, precise_time.tv_sec + 1,
                        SERVE_EVENT_JUDGE_OLYMPIAD, phr->user_id, 0);
      }
    }

    if (global->enable_auto_print_protocol > 0) {
      char *tmps = 0;
      size_t tmpz = 0;
      FILE *tmpf = 0;
      int locale_id = 0;

      /* Note, that all printing errors are ignored... */
      if (cnts->default_locale_num > 0) locale_id = cnts->default_locale_num;
      if (locale_id > 0) l10n_setlocale(locale_id);
      tmpf = open_memstream(&tmps, &tmpz);
      ns_print_user_exam_protocol(cnts, cs, tmpf, phr->user_id, locale_id, 1,
                                  0, 0);
      fclose(tmpf); tmpf = 0;
      xfree(tmps); tmps = 0; tmpz = 0;
      if (locale_id > 0) l10n_setlocale(0);
    }

    break;
  }

 done:;
  l10n_setlocale(0);
  close_memstream(log_f); log_f = 0;
  if (!log_txt || !*log_txt) {
    i = 0;
    if (phr->action == NEW_SRV_ACTION_VIRTUAL_START
        && global->problem_navigation) {
      for (i = 1; i <= cs->max_prob; i++) {
        if (!(prob = cs->probs[i])) continue;
        if (!serve_is_problem_started(cs, phr->user_id, prob))
          continue;
        // FIXME: standard applicability checks
        break;
      }
      if (i > cs->max_prob) i = 0;
    }
    if (i > 0) {
      snprintf(bb, sizeof(bb), "prob_id=%d", i);
      ns_refresh_page(fout, phr, NEW_SRV_ACTION_VIEW_PROBLEM_SUBMIT, bb);
    } else if (phr->action == NEW_SRV_ACTION_VIRTUAL_STOP) {
      ns_refresh_page(fout, phr, NEW_SRV_ACTION_VIEW_PROBLEM_SUMMARY, 0);
    } else {
      ns_refresh_page(fout, phr, NEW_SRV_ACTION_MAIN_PAGE, 0);
    }
  } else {
    unpriv_load_html_style(phr, cnts, 0, 0);
    html_error_status_page(fout, phr, cnts, extra, log_txt,
                           NEW_SRV_ACTION_MAIN_PAGE, 0);
  }

  //cleanup:;
  if (log_f) close_memstream(log_f);
  xfree(log_txt);
}

static void
unpriv_view_source(FILE *fout,
                   struct http_request_info *phr,
                   const struct contest_desc *cnts,
                   struct contest_extra *extra)
{
  serve_state_t cs = extra->serve_state;
  const struct section_global_data *global = cs->global;
  int run_id, src_flags;
  char *log_txt = 0;
  size_t log_len = 0;
  FILE *log_f = 0;
  struct run_entry re;
  const struct section_language_data *lang = 0;
  const struct section_problem_data *prob = 0;
  char *run_text = 0;
  size_t run_size = 0;
  path_t src_path;

  if (unpriv_parse_run_id(fout, phr, cnts, extra, &run_id, &re) < 0)
    goto cleanup;

  log_f = open_memstream(&log_txt, &log_len);

  if (cs->clients_suspended) {
    ns_error(log_f, NEW_SRV_ERR_CLIENTS_SUSPENDED);
    goto done;
  }
  if (cs->online_view_source < 0 || (!cs->online_view_source && global->team_enable_src_view <= 0)) {
    ns_error(log_f, NEW_SRV_ERR_SOURCE_VIEW_DISABLED);
    goto done;
  }
  if (re.user_id != phr->user_id) {
    ns_error(log_f, NEW_SRV_ERR_PERMISSION_DENIED);
    goto done;
  }
  if (re.prob_id <= 0 || re.prob_id > cs->max_prob ||
      !(prob = cs->probs[re.prob_id])) {
    ns_error(log_f, NEW_SRV_ERR_INV_PROB_ID);
    goto done;
  }
  if (re.status > RUN_LAST
      || (re.status > RUN_MAX_STATUS && re.status < RUN_TRANSIENT_FIRST)) {
    ns_error(log_f, NEW_SRV_ERR_SOURCE_UNAVAILABLE);
    goto done;
  }

  if ((src_flags = serve_make_source_read_path(cs, src_path, sizeof(src_path), &re)) < 0) {
    ns_error(log_f, NEW_SRV_ERR_SOURCE_NONEXISTANT);
    goto done;
  }
  if (generic_read_file(&run_text, 0, &run_size, src_flags, 0, src_path, 0)<0) {
    ns_error(log_f, NEW_SRV_ERR_DISK_READ_ERROR);
    goto done;
  }

  if (prob->type > 0) {
    fprintf(fout, "Content-type: %s\n", mime_type_get_type(re.mime_type));
    /*
    fprintf(fout, "Content-Disposition: attachment; filename=\"%06d%s\"\n",
            run_id, mime_type_get_suffix(re.mime_type));
    */
    putc_unlocked('\n', fout);
  } else {
    if(re.lang_id <= 0 || re.lang_id > cs->max_lang ||
       !(lang = cs->langs[re.lang_id])) {
      ns_error(log_f, NEW_SRV_ERR_INV_LANG_ID);
      goto done;
    }

    if (lang->content_type) {
      fprintf(fout, "Content-type: %s\n", lang->content_type);
      fprintf(fout, "Content-Disposition: attachment; filename=\"%06d%s\"\n\n",
              run_id, lang->src_sfx);
    } else if (lang->binary) {
      fprintf(fout, "Content-type: application/octet-stream\n\n");
      fprintf(fout, "Content-Disposition: attachment; filename=\"%06d%s\"\n\n",
              run_id, lang->src_sfx);
    } else {
      fprintf(fout, "Content-type: text/plain\n");
    }
  }
  fwrite(run_text, 1, run_size, fout);

 done:;
  close_memstream(log_f); log_f = 0;
  if (log_txt && *log_txt) {
    html_error_status_page(fout, phr, cnts, extra, log_txt,
                           NEW_SRV_ACTION_MAIN_PAGE, 0);
  }

 cleanup:
  if (log_f) close_memstream(log_f);
  xfree(log_txt);
  xfree(run_text);
}

static void
unpriv_view_test(FILE *fout,
                 struct http_request_info *phr,
                 const struct contest_desc *cnts,
                 struct contest_extra *extra)
{
  const serve_state_t cs = extra->serve_state;
  const struct section_problem_data *prob = 0;
  int run_id, test_num, n;
  const unsigned char *s = 0;
  struct run_entry re;
  FILE *log_f = 0;
  char *log_txt = 0;
  size_t log_len = 0;
  int enable_rep_view = -1;

  // run_id, test_num
  if (unpriv_parse_run_id(fout, phr, cnts, extra, &run_id, &re) < 0)
    goto cleanup;
  if (ns_cgi_param(phr, "test_num", &s) <= 0
      || sscanf(s, "%d%n", &test_num, &n) != 1 || s[n] || test_num <= 0) {
    ns_html_err_inv_param(fout, phr, 0, "cannot parse test_num");
    goto cleanup;
  }
  if (re.prob_id <= 0 || re.prob_id > cs->max_prob || !(prob = cs->probs[re.prob_id])) {
    ns_html_err_inv_param(fout, phr, 0, "invalid problem");
    goto cleanup;
  }

  // report view is explicitly disabled by the current contest setting
  if (cs->online_view_report < 0) enable_rep_view = 0;
  // report view is explicitly enabled by the current contest setting
  //if (cs->online_view_report > 0) enable_rep_view = 1;
  // report view is disabled by the problem configuration
  if (enable_rep_view < 0 && prob->team_enable_rep_view <= 0) enable_rep_view = 0;
  // report view is enabled by the problem configuration
  if (enable_rep_view < 0 && prob->team_show_judge_report > 0) enable_rep_view = 1;
  if (enable_rep_view < 0) {
    int visibility = cntsprob_get_test_visibility(prob, test_num, cs->online_final_visibility);
    if (visibility == TV_FULLIFMARKED) {
      visibility = TV_HIDDEN;
      if (re.is_marked) visibility = TV_FULL;
    }
    if (visibility == TV_FULL) enable_rep_view = 1;
  }

  if (enable_rep_view < 0) enable_rep_view = 0;

  log_f = open_memstream(&log_txt, &log_len);

  if (cs->clients_suspended) {
    ns_error(log_f, NEW_SRV_ERR_CLIENTS_SUSPENDED);
    goto done;
  }
  if (enable_rep_view <= 0) {
    ns_error(log_f, NEW_SRV_ERR_PERMISSION_DENIED);
    goto done;
  }
  if (re.user_id != phr->user_id) {
    ns_error(log_f, NEW_SRV_ERR_PERMISSION_DENIED);
    goto done;
  }
  switch (re.status) {
  case RUN_OK:
  case RUN_RUN_TIME_ERR:
  case RUN_TIME_LIMIT_ERR:
  case RUN_WALL_TIME_LIMIT_ERR:
  case RUN_PRESENTATION_ERR:
  case RUN_WRONG_ANSWER_ERR:
  case RUN_PARTIAL:
  case RUN_ACCEPTED:
  case RUN_PENDING_REVIEW:
  case RUN_MEM_LIMIT_ERR:
  case RUN_SECURITY_ERR:
    break;
  default:
    ns_error(log_f, NEW_SRV_ERR_PERMISSION_DENIED);
    goto done;
  }    

  ns_write_tests(cs, fout, log_f, phr->action, run_id, test_num);

 done:;
  close_memstream(log_f); log_f = 0;
  if (log_txt && *log_txt) {
    html_error_status_page(fout, phr, cnts, extra, log_txt,
                           NEW_SRV_ACTION_MAIN_PAGE, 0);
  }

 cleanup:
  if (log_f) close_memstream(log_f);
  xfree(log_txt);
}

static void
unpriv_view_report(FILE *fout,
                   struct http_request_info *phr,
                   const struct contest_desc *cnts,
                   struct contest_extra *extra)
{
  serve_state_t cs = extra->serve_state;
  const struct section_global_data *global = cs->global;
  const struct section_problem_data *prob;
  int run_id, flags, content_type;
  const unsigned char *rep_start = 0;
  FILE *log_f = 0;
  char *log_txt = 0, *rep_text = 0;
  size_t log_len = 0, rep_size = 0, html_len;
  struct run_entry re;
  path_t rep_path;
  unsigned char *html_report;
  time_t start_time, stop_time;
  int accepting_mode = 0;
  int enable_rep_view = 0;

  static const int new_actions_vector[] =
  {
    NEW_SRV_ACTION_VIEW_TEST_INPUT,
    NEW_SRV_ACTION_VIEW_TEST_OUTPUT,
    NEW_SRV_ACTION_VIEW_TEST_ANSWER,
    NEW_SRV_ACTION_VIEW_TEST_ERROR,
    NEW_SRV_ACTION_VIEW_TEST_CHECKER,
    NEW_SRV_ACTION_VIEW_TEST_INFO,
  };

  start_time = run_get_start_time(cs->runlog_state);
  stop_time = run_get_stop_time(cs->runlog_state);
  if (global->is_virtual) {
    start_time = run_get_virtual_start_time(cs->runlog_state, phr->user_id);
    stop_time = run_get_virtual_stop_time(cs->runlog_state, phr->user_id,
                                          cs->current_time);
    if (global->score_system == SCORE_OLYMPIAD) {
      if (global->disable_virtual_auto_judge <= 0 && stop_time <= 0)
        accepting_mode = 1;
      else if (global->disable_virtual_auto_judge > 0
               && cs->testing_finished <= 0)
        accepting_mode = 1;
    }
  } else {
    accepting_mode = cs->accepting_mode;
  }

  if (unpriv_parse_run_id(fout, phr, cnts, extra, &run_id, &re) < 0)
    goto cleanup;

  enable_rep_view = (cs->online_view_report > 0 || (!cs->online_view_report && global->team_enable_rep_view > 0));

  log_f = open_memstream(&log_txt, &log_len);

  if (cs->clients_suspended) {
    ns_error(log_f, NEW_SRV_ERR_CLIENTS_SUSPENDED);
    goto done;
  }

  if (re.user_id != phr->user_id) {
    ns_error(log_f, NEW_SRV_ERR_PERMISSION_DENIED);
    goto done;
  }
  if (re.prob_id <= 0 || re.prob_id > cs->max_prob ||
      !(prob = cs->probs[re.prob_id])) {
    ns_error(log_f, NEW_SRV_ERR_INV_PROB_ID);
    goto done;
  }
  // check viewable statuses
  switch (re.status) {
  case RUN_OK:
  case RUN_COMPILE_ERR:
  case RUN_RUN_TIME_ERR:
  case RUN_TIME_LIMIT_ERR:
  case RUN_WALL_TIME_LIMIT_ERR:
  case RUN_PRESENTATION_ERR:
  case RUN_WRONG_ANSWER_ERR:
  case RUN_PARTIAL:
  case RUN_ACCEPTED:
  case RUN_PENDING_REVIEW:
  case RUN_MEM_LIMIT_ERR:
  case RUN_SECURITY_ERR:
  case RUN_STYLE_ERR:
  case RUN_REJECTED:
    // these statuses have viewable reports
    break;
  default:
    ns_error(log_f, NEW_SRV_ERR_REPORT_UNAVAILABLE);
    goto done;
  }

  if (accepting_mode && prob->type != PROB_TYPE_STANDARD) {
    ns_error(log_f, NEW_SRV_ERR_PERMISSION_DENIED);
    goto done;
  }

  if (enable_rep_view) enable_rep_view = prob->team_enable_rep_view;
  if (!enable_rep_view
      && (!prob->team_enable_ce_view
          || (re.status != RUN_COMPILE_ERR
              && re.status != RUN_STYLE_ERR
              && re.status != RUN_REJECTED))) {
    ns_error(log_f, NEW_SRV_ERR_REPORT_VIEW_DISABLED);
    goto done;
  }

  flags = serve_make_xml_report_read_path(cs, rep_path, sizeof(rep_path), &re);
  if (flags >= 0) {
    if (generic_read_file(&rep_text, 0, &rep_size, flags, 0, rep_path, 0) < 0) {
      ns_error(log_f, NEW_SRV_ERR_DISK_READ_ERROR);
      goto done;
    }
    content_type = get_content_type(rep_text, &rep_start);
    if (content_type != CONTENT_TYPE_XML
        && re.status != RUN_COMPILE_ERR
        && re.status != RUN_STYLE_ERR
        && re.status != RUN_REJECTED) {
      ns_error(log_f, NEW_SRV_ERR_REPORT_UNAVAILABLE);
      goto done;
    }
  } else {
    int user_mode = 0;
    if (prob->team_enable_ce_view
        && (re.status == RUN_COMPILE_ERR
            || re.status == RUN_STYLE_ERR
            || re.status == RUN_REJECTED)) {
    } else if (prob->team_show_judge_report) {
    } else {
      user_mode = 1;
    }

    if (user_mode) {
      flags = archive_make_read_path(cs, rep_path, sizeof(rep_path),
                                     global->team_report_archive_dir, run_id, 0, 1);
    } else {
      flags = serve_make_report_read_path(cs, rep_path, sizeof(rep_path), &re);
      
    }
    if (flags < 0) {
      ns_error(log_f, NEW_SRV_ERR_REPORT_NONEXISTANT);
      goto done;
    }

    if (generic_read_file(&rep_text,0,&rep_size,flags,0,rep_path, 0) < 0) {
      ns_error(log_f, NEW_SRV_ERR_DISK_READ_ERROR);
      goto done;
    }
    content_type = get_content_type(rep_text, &rep_start);
  }

  unpriv_load_html_style(phr, cnts, 0, 0);
  l10n_setlocale(phr->locale_id);
  ns_header(fout, extra->header_txt, 0, 0, phr->script_part, phr->body_attr,
            phr->locale_id, cnts,
            phr->client_key,
            "%s [%s]: %s %d",
            phr->name_arm, extra->contest_arm, _("Report for run"),
            run_id);
  unpriv_page_header(fout, phr, cnts, extra, start_time, stop_time);

  switch (content_type) {
  case CONTENT_TYPE_TEXT:
    html_len = html_armored_memlen(rep_text, rep_size);
    if (html_len > 2 * 1024 * 1024) {
      html_report = xmalloc(html_len + 16);
      html_armor_text(rep_text, rep_size, html_report);
      html_report[html_len] = 0;
      fprintf(fout, "<pre>%s</pre>", html_report);
      xfree(html_report);
    } else {
      html_report = alloca(html_len + 16);
      html_armor_text(rep_text, rep_size, html_report);
      html_report[html_len] = 0;
      fprintf(fout, "<pre>%s</pre>", html_report);
    }
    break;
  case CONTENT_TYPE_HTML:
    fprintf(fout, "%s", rep_start);
    break;
  case CONTENT_TYPE_XML:
    if (prob->type == PROB_TYPE_TESTS) {
      if (prob->team_show_judge_report) {
        write_xml_tests_report(fout, 1, rep_start, phr->session_id,
                                 phr->self_url, "", "b1", "b0"); 
      } else {
        write_xml_team_tests_report(cs, prob, fout, rep_start, "b1");
      }
    } else {
      if (global->score_system == SCORE_OLYMPIAD && accepting_mode) {
        write_xml_team_accepting_report(fout, rep_start, run_id, &re, prob,
                                        new_actions_vector,
                                        phr->session_id, cnts->exam_mode,
                                        phr->self_url, "", "b1");
      } else if (prob->team_show_judge_report) {
        write_xml_testing_report(fout, 1, rep_start, phr->session_id,
                                 phr->self_url, "", new_actions_vector, "b1",
                                 "b0");
      } else {
        write_xml_team_testing_report(cs, prob, fout,
                                      prob->type != PROB_TYPE_STANDARD,
                                      re.is_marked,
                                      rep_start, "b1", phr->session_id, phr->self_url, "", new_actions_vector);
      }
    }
    break;
  default:
    abort();
  }

  ns_footer(fout, extra->footer_txt, extra->copyright_txt, phr->locale_id);
  l10n_setlocale(0);

 done:;
  close_memstream(log_f); log_f = 0;
  if (log_txt && *log_txt) {
    html_error_status_page(fout, phr, cnts, extra, log_txt,
                           NEW_SRV_ACTION_MAIN_PAGE, 0);
  }

 cleanup:
  if (log_f) close_memstream(log_f);
  xfree(log_txt);
  xfree(rep_text);
}

static void
unpriv_view_clar(FILE *fout,
                 struct http_request_info *phr,
                 const struct contest_desc *cnts,
                 struct contest_extra *extra)
{
  serve_state_t cs = extra->serve_state;
  const struct section_global_data *global = cs->global;
  int n, clar_id, show_astr_time;
  const unsigned char *s;
  FILE *log_f = 0;
  char *log_txt = 0;
  size_t log_len = 0, clar_size = 0, html_subj_len, html_text_len;
  struct clar_entry_v1 ce;
  time_t start_time, clar_time, stop_time;
  unsigned char *clar_text = 0;
  unsigned char *html_subj, *html_text;
  unsigned char dur_str[64];
  const unsigned char *clar_subj = 0;
  struct html_armor_buffer ab = HTML_ARMOR_INITIALIZER;

  if ((n = ns_cgi_param(phr, "clar_id", &s)) <= 0)
    return ns_html_err_inv_param(fout, phr, 0, "clar_id is binary or not set");
  if (sscanf(s, "%d%n", &clar_id, &n) != 1 || s[n])
    return ns_html_err_inv_param(fout, phr, 0, "cannot parse clar_id");

  log_f = open_memstream(&log_txt, &log_len);

  if (cs->clients_suspended) {
    ns_error(log_f, NEW_SRV_ERR_CLIENTS_SUSPENDED);
    goto done;
  }
  if (global->disable_clars) {
    ns_error(log_f, NEW_SRV_ERR_CLARS_DISABLED);
    goto done;
  }
  if (clar_id < 0 || clar_id >= clar_get_total(cs->clarlog_state)
      || clar_get_record(cs->clarlog_state, clar_id, &ce) < 0
      || ce.id < 0) {
    ns_error(log_f, NEW_SRV_ERR_INV_CLAR_ID);
    goto done;
  }

  show_astr_time = global->show_astr_time;
  if (global->is_virtual) show_astr_time = 1;
  start_time = run_get_start_time(cs->runlog_state);
  stop_time = run_get_stop_time(cs->runlog_state);
  if (global->is_virtual) {
    start_time = run_get_virtual_start_time(cs->runlog_state, phr->user_id);
    stop_time = run_get_virtual_stop_time(cs->runlog_state, phr->user_id,
                                          cs->current_time);
  }

  if ((ce.from > 0 && ce.from != phr->user_id)
      || (ce.to > 0 && ce.to != phr->user_id)
      || (start_time <= 0 && ce.hide_flag)) {
    ns_error(log_f, NEW_SRV_ERR_PERMISSION_DENIED);
    goto done;
  }

  if (ce.from != phr->user_id) {
    team_extra_set_clar_status(cs->team_extra_state, phr->user_id, clar_id);
  }

  if (clar_get_text(cs->clarlog_state, clar_id, &clar_text, &clar_size) < 0) {
    ns_error(log_f, NEW_SRV_ERR_DISK_READ_ERROR);
    goto done;
  }

  clar_subj = clar_get_subject(cs->clarlog_state, clar_id);
  html_subj_len = html_armored_strlen(clar_subj);
  html_subj = alloca(html_subj_len + 1);
  html_armor_string(clar_subj, html_subj);
  html_text_len = html_armored_strlen(clar_text);
  html_text = alloca(html_text_len + 1);
  html_armor_string(clar_text, html_text);

  clar_time = ce.time;
  if (start_time < 0) start_time = 0;
  if (!start_time) clar_time = start_time;
  if (clar_time < start_time) clar_time = start_time;
  duration_str(show_astr_time, clar_time, start_time, dur_str, 0);

  unpriv_load_html_style(phr, cnts, 0, 0);
  l10n_setlocale(phr->locale_id);
  ns_header(fout, extra->header_txt, 0, 0, phr->script_part, phr->body_attr, phr->locale_id, cnts,
            phr->client_key,
            "%s [%s]: %s %d",
            phr->name_arm, extra->contest_arm, _("Clarification"),
            clar_id);
  unpriv_page_header(fout, phr, cnts, extra, start_time, stop_time);

  fprintf(fout, "<%s>%s #%d</%s>\n", cnts->team_head_style,
          _("Message"), clar_id, cnts->team_head_style);
  fprintf(fout, "<table class=\"b0\">\n");
  fprintf(fout, "<tr><td class=\"b0\">%s:</td><td class=\"b0\">%d</td></tr>\n", _("Number"), clar_id);
  fprintf(fout, "<tr><td class=\"b0\">%s:</td><td class=\"b0\">%s</td></tr>\n", _("Time"), dur_str);
  fprintf(fout, "<tr><td class=\"b0\">%s:</td><td class=\"b0\">%u</td></tr>\n", _("Size"), ce.size);
  fprintf(fout, "<tr><td class=\"b0\">%s:</td>", _("Sender"));
  if (!ce.from) {
    fprintf(fout, "<td class=\"b0\"><b>%s</b></td>", _("judges"));
  } else {
    fprintf(fout, "<td class=\"b0\">%s</td>", ARMOR(teamdb_get_name(cs->teamdb_state, ce.from)));
  }
  fprintf(fout, "</tr>\n<tr><td class=\"b0\">%s:</td>", _("To"));
  if (!ce.to && !ce.from) {
    fprintf(fout, "<td class=\"b0\"><b>%s</b></td>", _("all"));
  } else if (!ce.to) {
    fprintf(fout, "<td class=\"b0\"><b>%s</b></td>", _("judges"));
  } else {
    fprintf(fout, "<td class=\"b0\">%s</td>", ARMOR(teamdb_get_name(cs->teamdb_state, ce.to)));
  }
  fprintf(fout, "</tr>\n");
  fprintf(fout, "<tr><td class=\"b0\">%s:</td><td class=\"b0\">%s</td></tr>", _("Subject"), html_subj);
  fprintf(fout, "</table>\n");
  fprintf(fout, "<hr><pre>");
  fprintf(fout, "%s", html_text);
  fprintf(fout, "</pre><hr>");

  ns_footer(fout, extra->footer_txt, extra->copyright_txt, phr->locale_id);
  l10n_setlocale(0);

 done:;
  close_memstream(log_f); log_f = 0;
  if (log_txt && *log_txt) {
    html_error_status_page(fout, phr, cnts, extra, log_txt,
                           NEW_SRV_ACTION_MAIN_PAGE, 0);
  }

  if (log_f) close_memstream(log_f);
  xfree(log_txt);
  xfree(clar_text);
  html_armor_free(&ab);
}

static void
unpriv_view_standings(FILE *fout,
                      struct http_request_info *phr,
                      const struct contest_desc *cnts,
                      struct contest_extra *extra)
{
  serve_state_t cs = extra->serve_state;
  const struct section_global_data *global = cs->global;
  time_t start_time, stop_time, cur_time, fog_start_time = 0, fog_stop_time = 0;
  time_t sched_time = 0, duration = 0;
  long long tdiff;
  unsigned char comment[1024] = { 0 };
  unsigned char dur_buf[128];

  if (global->is_virtual) {
    start_time = run_get_virtual_start_time(cs->runlog_state, phr->user_id);
    stop_time = run_get_virtual_stop_time(cs->runlog_state, phr->user_id,
                                          cs->current_time);
  } else {
    start_time = run_get_start_time(cs->runlog_state);
    stop_time = run_get_stop_time(cs->runlog_state);
  }
  run_get_times(cs->runlog_state, 0, &sched_time, &duration, 0, 0);
  if (duration > 0 && start_time > 0 && global->board_fog_time > 0)
    fog_start_time = start_time + duration - global->board_fog_time;
  if (fog_start_time < 0) fog_start_time = 0;
  if (fog_start_time > 0 && stop_time > 0) {
    if (global->board_unfog_time > 0)
      fog_stop_time = stop_time + global->board_unfog_time;
    else
      fog_stop_time = stop_time;
  }
  /* FIXME: if a virtual contest is over, display the final
   * standings at the current time! */

  unpriv_load_html_style(phr, cnts, 0, 0);
  l10n_setlocale(phr->locale_id);
  if (start_time <= 0) {
    ns_header(fout, extra->header_txt, 0, 0, phr->script_part, phr->body_attr, phr->locale_id, cnts,
              phr->client_key,
              "%s [%s]: %s",
              phr->name_arm, extra->contest_arm, _("Standings [not started]"));
    unpriv_page_header(fout, phr, cnts, extra, start_time, stop_time);
    goto done;
  }

  cur_time = cs->current_time;
  if (cur_time < start_time) cur_time = start_time;
  if (duration <= 0) {
    if (stop_time > 0 && cur_time >= stop_time)
      snprintf(comment, sizeof(comment), _(" [over]"));
    else if (global->stand_ignore_after > 0
             && cur_time >= global->stand_ignore_after) {
      cur_time = global->stand_ignore_after;
      snprintf(comment, sizeof(comment), " [%s, frozen]",
               xml_unparse_date(cur_time));
    } else
      snprintf(comment, sizeof(comment), " [%s]", xml_unparse_date(cur_time));
  } else {
    if (stop_time > 0 && cur_time >= stop_time) {
      if (fog_stop_time > 0 && cur_time < fog_stop_time) {
        cur_time = fog_start_time;
        snprintf(comment, sizeof(comment), _(" [over, frozen]"));
      }
      else
        snprintf(comment, sizeof(comment), _(" [over]"));
    } else {
      if (fog_start_time > 0 && cur_time >= fog_start_time) {
        cur_time = fog_start_time;
        snprintf(comment, sizeof(comment), _(" [%s, frozen]"),
                 duration_str(global->show_astr_time, cur_time, start_time,
                              dur_buf, sizeof(dur_buf)));
      } else
        snprintf(comment, sizeof(comment), " [%s]",
                 duration_str(global->show_astr_time, cur_time, start_time,
                              dur_buf, sizeof(dur_buf)));
    }
  }

  ns_header(fout, extra->header_txt, 0, 0, phr->script_part, phr->body_attr, phr->locale_id, cnts,
            phr->client_key,
            "%s [%s]: %s%s",
            phr->name_arm, extra->contest_arm, _("Standings"), comment);

  unpriv_page_header(fout, phr, cnts, extra, start_time, stop_time);
  fprintf(fout, "<%s>%s%s</%s>\n",
          cnts->team_head_style, _("Standings"), comment,
          cnts->team_head_style);

  if (global->disable_user_standings > 0) {
    fprintf(fout, _("<p>Information is not available.</p>"));
  } else if (global->is_virtual) {
    do_write_standings(cs, cnts, fout, 1, 1, phr->user_id, 0, 0, 0, 0, 1,
                       cur_time, NULL);
  } else if (global->score_system == SCORE_ACM) {
    do_write_standings(cs, cnts, fout, 1, 1, phr->user_id, 0, 0, 0, 0, 1,
                       cur_time, NULL);
  } else if (global->score_system == SCORE_OLYMPIAD && cs->accepting_mode) {
    fprintf(fout, _("<p>Information is not available.</p>"));
  } else if (global->score_system == SCORE_OLYMPIAD) {
    //fprintf(fout, _("<p>Information is not available.</p>"));
    do_write_kirov_standings(cs, cnts, fout, 0, 1, 1, phr->user_id, 0, 0, 0, 0, 1, cur_time,
                             0, NULL, 1 /* user_mode */);
  } else if (global->score_system == SCORE_KIROV) {
    do_write_kirov_standings(cs, cnts, fout, 0, 1, 1, phr->user_id, 0, 0, 0, 0, 1, cur_time,
                             0, NULL, 1 /* user_mode */);
  } else if (global->score_system == SCORE_MOSCOW) {
    do_write_moscow_standings(cs, cnts, fout, 0, 1, 1, phr->user_id,
                              0, 0, 0, 0, 1, cur_time, 0, NULL);
  }

 done:
  if (1 /*cs->global->show_generation_time*/) {
  gettimeofday(&phr->timestamp2, 0);
  tdiff = ((long long) phr->timestamp2.tv_sec) * 1000000;
  tdiff += phr->timestamp2.tv_usec;
  tdiff -= ((long long) phr->timestamp1.tv_sec) * 1000000;
  tdiff -= phr->timestamp1.tv_usec;
  fprintf(fout, "<div class=\"dotted\"><p%s>%s: %lld %s</p></div>",
          cnts->team_par_style,
          _("Page generation time"), tdiff / 1000,
          _("msec"));
  }

  ns_footer(fout, extra->footer_txt, extra->copyright_txt, phr->locale_id);
  l10n_setlocale(0);
}

static void
html_problem_selection(serve_state_t cs,
                       FILE *fout,
                       struct http_request_info *phr,
                       const unsigned char *solved_flag,
                       const unsigned char *accepted_flag,
                       const unsigned char *var_name,
                       int light_mode,
                       time_t start_time)
{
  int i, dpi, j, k;
  time_t user_deadline = 0;
  int user_penalty = 0, variant = 0;
  unsigned char deadline_str[64];
  unsigned char penalty_str[64];
  unsigned char problem_str[128];
  const unsigned char *problem_ptr = 0;
  const struct section_problem_data *prob;

  if (!var_name) var_name = "prob_id";

  fprintf(fout, "<select name=\"%s\"><option value=\"\"></option>\n", var_name);

  for (i = 1; i <= cs->max_prob; i++) {
    if (!(prob = cs->probs[i])) continue;
    if (!light_mode && prob->disable_submit_after_ok>0 && solved_flag[i])
      continue;
    if (!serve_is_problem_started(cs, phr->user_id, prob))
      continue;
    if (start_time <= 0) continue;
    //if (prob->disable_user_submit) continue;

    penalty_str[0] = 0;
    deadline_str[0] = 0;
    if (!light_mode) {
      // try to find personal rules
      user_deadline = 0;
      user_penalty = 0;
      if (serve_is_problem_deadlined(cs, phr->user_id, phr->login,
                                     prob, &user_deadline))
        continue;

      // check `require' variable
      if (prob->require) {
        for (j = 0; prob->require[j]; j++) {
          for (k = 1; k <= cs->max_prob; k++) {
            if (cs->probs[k]
                && !strcmp(cs->probs[k]->short_name, prob->require[j]))
              break;
          }
          // no such problem :(
          if (k > cs->max_prob) break;
          // this problem is not yet accepted or solved
          if (!solved_flag[k] && !accepted_flag[k]) break;
        }
        if (prob->require[j]) continue;
      }

      // find date penalty
      for (dpi = 0; dpi < prob->dp_total; dpi++)
        if (cs->current_time < prob->dp_infos[dpi].date)
          break;
      if (dpi < prob->dp_total)
        user_penalty = prob->dp_infos[dpi].penalty;

      if (user_deadline > 0 && cs->global->show_deadline)
        snprintf(deadline_str, sizeof(deadline_str),
                 " (%s)", xml_unparse_date(user_deadline));
      if (user_penalty && cs->global->show_deadline)
        snprintf(penalty_str, sizeof(penalty_str), " [%d]", user_penalty);
    }

    if (prob->variant_num > 0) {
      if ((variant = find_variant(cs, phr->user_id, i, 0)) <= 0) continue;
      snprintf(problem_str, sizeof(problem_str),
               "%s-%d", prob->short_name, variant);
      problem_ptr = problem_str;
    } else {
      problem_ptr = prob->short_name;
    }

    fprintf(fout, "<option value=\"%d\">%s - %s%s%s</option>\n",
            i, problem_ptr, prob->long_name, penalty_str,
            deadline_str);
  }

  fprintf(fout, "</select>");
}

// for "Statements" section
static void
html_problem_selection_2(serve_state_t cs,
                         FILE *fout,
                         struct http_request_info *phr,
                         const unsigned char *var_name,
                         time_t start_time)
{
  int i, dpi;
  time_t user_deadline = 0;
  int variant = 0;
  unsigned char deadline_str[64];
  unsigned char problem_str[128];
  const unsigned char *problem_ptr = 0;
  const struct section_problem_data *prob;

  if (!var_name) var_name = "prob_id";

  fprintf(fout, "<select name=\"%s\"><option value=\"\"></option>\n", var_name);
  fprintf(fout, "<option value=\"-1\">%s</option>\n", _("View all"));

  for (i = 1; i <= cs->max_prob; i++) {
    if (!(prob = cs->probs[i])) continue;
    if (!serve_is_problem_started(cs, phr->user_id, prob))
      continue;
    if (start_time <= 0) continue;

    if (serve_is_problem_deadlined(cs, phr->user_id, phr->login,
                                   prob, &user_deadline))
      continue;

    // find date penalty
    for (dpi = 0; dpi < prob->dp_total; dpi++)
      if (cs->current_time < prob->dp_infos[dpi].date)
        break;

    if (user_deadline > 0 && cs->global->show_deadline)
      snprintf(deadline_str, sizeof(deadline_str),
               " (%s)", xml_unparse_date(user_deadline));

    if (prob->variant_num > 0) {
      if ((variant = find_variant(cs, phr->user_id, i, 0)) <= 0) continue;
      snprintf(problem_str, sizeof(problem_str),
               "%s-%d", prob->short_name, variant);
      problem_ptr = problem_str;
    } else {
      problem_ptr = prob->short_name;
    }

    fprintf(fout, "<option value=\"%d\">%s - %s%s</option>\n",
            i, problem_ptr, prob->long_name, deadline_str);
  }

  fprintf(fout, "</select>");
}

static unsigned char *
brief_time(unsigned char *buf, size_t size, time_t time)
{
  struct tm *ptm = localtime(&time);
  snprintf(buf, size, "%02d:%02d:%02d",
           ptm->tm_hour, ptm->tm_min, ptm->tm_sec);
  return buf;
}

static void
unpriv_page_header(FILE *fout,
                   struct http_request_info *phr,
                   const struct contest_desc *cnts,
                   struct contest_extra *extra,
                   time_t start_time, time_t stop_time)
{
  static int top_action_list[] =
  {
    NEW_SRV_ACTION_VIEW_SETTINGS,
    NEW_SRV_ACTION_REG_DATA_EDIT,
    NEW_SRV_ACTION_LOGOUT,

    -1,
  };

  static const unsigned char *top_action_names[] =
  {
    __("Settings"),
    __("Registration data"),
    __("Logout"),
  };

  static int action_list[] =
  {
    NEW_SRV_ACTION_MAIN_PAGE,
    NEW_SRV_ACTION_VIEW_STARTSTOP,
    NEW_SRV_ACTION_VIEW_PROBLEM_SUMMARY,
    NEW_SRV_ACTION_VIEW_PROBLEM_STATEMENTS,
    NEW_SRV_ACTION_VIEW_PROBLEM_SUBMIT,
    NEW_SRV_ACTION_VIEW_SUBMISSIONS,
    NEW_SRV_ACTION_STANDINGS,
    NEW_SRV_ACTION_VIEW_CLAR_SUBMIT,
    NEW_SRV_ACTION_VIEW_CLARS,

    -1,
  };

  static const unsigned char *action_names[] =
  {
    __("Info"),
    0,
    __("Summary"),
    __("Statements"),
    __("Submit"),
    __("Submissions"),
    __("Standings"),
    __("Submit clar"),
    __("Clars"),
    __("Settings"),
    __("Logout"),
  };

  int i, prob_id, has_prob_stmt = 0;
  serve_state_t cs = extra->serve_state;
  const unsigned char *forced_url = 0;
  const unsigned char *target = 0;
  const unsigned char *forced_text = 0;
  const struct section_global_data *global = cs->global;
  int unread_clars = 0;
  const unsigned char *status_style = "", *s;
  unsigned char time_buf[64];
  time_t duration = 0, sched_time = 0, fog_start_time = 0;
  int shown_items = 0;
  const unsigned char *template_ptr;
  unsigned char stand_url_buf[1024];
  struct teamdb_export tdb;
  struct sformat_extra_data fe;
  const unsigned char *visibility;

  template_ptr = extra->menu_2_txt;
  if (!template_ptr || !*template_ptr)
    template_ptr = ns_fancy_unpriv_content_header;

  if (!phr->action) phr->action = NEW_SRV_ACTION_MAIN_PAGE;

  while (*template_ptr) {
    if (*template_ptr != '%') {
      putc(*template_ptr, fout);
      template_ptr++;
      continue;
    }
    template_ptr++;
    if (!*template_ptr) {
      putc('%', fout);
      break;
    } else if (*template_ptr == '%') {
      putc('%', fout);
      template_ptr++;
      continue;
    }

    switch (*template_ptr++) {
    case '1':
      for (i = 0; top_action_list[i] != -1; i++) {
        // phew ;)
        if (cnts->exam_mode) continue;
        if (phr->action == top_action_list[i]) {
          fprintf(fout, "<td class=\"menu\"><div class=\"contest_actions_item\">%s</div></td>", gettext(top_action_names[i]));
          shown_items++;
        } else if (top_action_list[i] == NEW_SRV_ACTION_REG_DATA_EDIT) {
          if (!cnts->allow_reg_data_edit) continue;
          if (!contests_check_register_ip_2(cnts, &phr->ip, phr->ssl_flag))
            continue;
          if (cnts->reg_deadline > 0 && cs->current_time >= cnts->reg_deadline)
            continue;
          get_register_url(stand_url_buf, sizeof(stand_url_buf), cnts,
                           phr->self_url);
          fprintf(fout, "<td class=\"menu\"><div class=\"contest_actions_item\"><a class=\"menu\" href=\"%s?SID=%016llx\">%s</a></div></td>",
                  stand_url_buf, phr->session_id,
                  gettext(top_action_names[i]));
          shown_items++;
        } else if (top_action_list[i] == NEW_SRV_ACTION_LOGOUT) {
          forced_text = 0;
          if (cnts->exam_mode) forced_text = _("Finish session");
          if (!forced_text) forced_text = gettext(top_action_names[i]);
          fprintf(fout, "<td class=\"menu\"><div class=\"contest_actions_item\"><a class=\"menu\" href=\"%s?SID=%016llx&amp;action=%d\">%s [%s]</a></div></td>",
                  phr->self_url, phr->session_id, top_action_list[i],
                  forced_text, phr->login);
          shown_items++;
        } else {
          fprintf(fout, "<td class=\"menu\"><div class=\"contest_actions_item\"><a class=\"menu\" href=\"%s?SID=%016llx&amp;action=%d\">%s</a></div></td>",
                  phr->self_url, phr->session_id, top_action_list[i],
                  gettext(top_action_names[i]));
          shown_items++;
        }
      }
      if (!shown_items)
        fprintf(fout, "<td class=\"menu\"><div class=\"contest_actions_item\">&nbsp;</div></td>");
      break;
    case '2':
      for (i = 0; action_list[i] != -1; i++) {
        forced_url = 0;
        forced_text = 0;
        target = "";
        // conditions when the corresponding menu item is shown
        switch (action_list[i]) {
        case NEW_SRV_ACTION_MAIN_PAGE:
          if (cnts->exam_mode) forced_text = _("Instructions");
          break;
        case NEW_SRV_ACTION_VIEW_STARTSTOP:
          if (!global->is_virtual) continue;
          if (start_time <= 0) {
            if (global->disable_virtual_start > 0) continue;
            if (cnts->exam_mode) forced_text = _("Start exam");
            else forced_text = _("Start virtual contest");
          } else if (stop_time <= 0) {
            if (cnts->exam_mode) forced_text = _("Stop exam");
            else forced_text = _("Stop virtual contest");
          } else {
            continue;
          }
          break;
        case NEW_SRV_ACTION_VIEW_PROBLEM_SUMMARY:
          if (start_time <= 0) continue;
          if (cnts->exam_mode && stop_time <= 0) continue;
          break;      
        case NEW_SRV_ACTION_VIEW_PROBLEM_STATEMENTS:
          if (start_time <= 0) continue;
          if (stop_time > 0 && !cnts->problems_url) continue;
          for (prob_id = 1; prob_id <= cs->max_prob; prob_id++)
            if (cs->probs[prob_id] && cs->probs[prob_id]->statement_file[0])
              break;
          if (prob_id <= cs->max_prob)
            has_prob_stmt = 1;
          if (!has_prob_stmt && !cnts->problems_url) continue;
          if (cnts->problems_url && (stop_time > 0 || !has_prob_stmt)) {
            forced_url = cnts->problems_url;
            target = " target=\"_blank\"";
          }
          if (global->problem_navigation && !cnts->problems_url) continue;
          break;
        case NEW_SRV_ACTION_VIEW_PROBLEM_SUBMIT:
          if (start_time <= 0 || stop_time > 0) continue;
          if (global->problem_navigation > 0) continue;
          break;
        case NEW_SRV_ACTION_VIEW_SUBMISSIONS:
          if (start_time <= 0) continue;
          if (cnts->exam_mode && stop_time <= 0) continue;
          break;
        case NEW_SRV_ACTION_STANDINGS:
          if (start_time <= 0) continue;
          if (global->disable_user_standings > 0) continue;
          //if (global->score_system == SCORE_OLYMPIAD) continue;
          if (cnts->standings_url) {
            memset(&tdb, 0, sizeof(tdb));
            teamdb_export_team(cs->teamdb_state, phr->user_id, &tdb);
            memset(&fe, 0, sizeof(fe));
            fe.locale_id = phr->locale_id;
            fe.sid = phr->session_id;
            sformat_message(stand_url_buf, sizeof(stand_url_buf), 0,
                            cnts->standings_url, global, 0, 0, 0, &tdb,
                            tdb.user, cnts, &fe);
            forced_url = stand_url_buf;
            target = " target=\"_blank\"";
          }
          if (cnts->personal) forced_text = _("User standings");
          break;
        case NEW_SRV_ACTION_VIEW_CLAR_SUBMIT:
          if (global->disable_team_clars) continue;
          if (global->disable_clars) continue;
          if (start_time <= 0) continue;
          if (stop_time > 0
              && (global->appeal_deadline <= 0
                  || cs->current_time >= global->appeal_deadline))
            continue;
          break;
        case NEW_SRV_ACTION_VIEW_CLARS:
          if (global->disable_clars) continue;
          break;
        case NEW_SRV_ACTION_VIEW_SETTINGS:
          break;
        }
        if (!forced_text) forced_text = gettext(action_names[i]);
        if (phr->action == action_list[i]) {
          fprintf(fout, "<td class=\"menu\"><div class=\"contest_actions_item\">%s</div></td>", forced_text);
        } else if (forced_url) {
          fprintf(fout, "<td class=\"menu\"><div class=\"contest_actions_item\"><a class=\"menu\" href=\"%s\"%s>%s</a></div></td>",
                  forced_url, target, forced_text);
        } else {
          fprintf(fout, "<td class=\"menu\"><div class=\"contest_actions_item\"><a class=\"menu\" href=\"%s?SID=%016llx&amp;action=%d\">%s</a></div></td>",
                  phr->self_url, phr->session_id, action_list[i], forced_text);
        }
      }
      break;

    case '3':
      if (extra->separator_txt && *extra->separator_txt) {
        ns_separator(fout, extra->separator_txt, cnts);
      }
      break;

    case '4':
      run_get_times(cs->runlog_state, 0, &sched_time, &duration, 0, 0);
      if (duration > 0 && start_time && !stop_time
          && global->board_fog_time > 0)
        fog_start_time = start_time + duration - global->board_fog_time;
      if (fog_start_time < 0) fog_start_time = 0;
      if (!cs->global->disable_clars || !cs->global->disable_team_clars)
        unread_clars = serve_count_unread_clars(cs, phr->user_id, start_time);
      if (cs->clients_suspended) {
        status_style = "server_status_off";
      } else if (unread_clars > 0) {
        status_style = "server_status_alarm";
      } else {
        status_style = "server_status_on";
      }
      fprintf(fout, "<div class=\"%s\" id=\"statusLine\">\n", status_style);
      fprintf(fout, "<div id=\"currentTime\">%s</div>",
              brief_time(time_buf, sizeof(time_buf), cs->current_time));
      if (unread_clars > 0) {
        fprintf(fout, _(" / <b>%d unread message(s)</b>"),
                unread_clars);
      }

      if (stop_time > 0) {
        if (duration > 0 && global->board_fog_time > 0
            && global->board_unfog_time > 0
            && cs->current_time < stop_time + global->board_unfog_time
            && !cs->standings_updated) {
          s = _("OVER (frozen)");
        } else {
          s = _("OVER");
        }
      } else if (start_time > 0) {
        if (fog_start_time > 0 && cs->current_time >= fog_start_time) {
          if (cnts->exam_mode)
            s = _("EXAM IS RUNNING (frozen)");
          else
            s = _("RUNNING (frozen)");
        } else {
          if (cnts->exam_mode)
            s = _("EXAM IS RUNNING");
          else
            s = _("RUNNING");
        }
      } else {
        s = _("NOT STARTED");
      }
      fprintf(fout, " / <b>%s</b>", s);

      if (start_time > 0) {
        if (global->score_system == SCORE_OLYMPIAD && !global->is_virtual) {
          if (cs->accepting_mode)
            s = _("accepting");
          else if (!cs->testing_finished)
            s = _("judging");
          else
            s = _("judged");
          fprintf(fout, " / <b>%s</b>", s);
        }
      }

      if (cs->upsolving_mode) {
        fprintf(fout, " / <b>%s</b>", _("UPSOLVING"));
      }

      if (cs->clients_suspended) {
        fprintf(fout, " / <b><font color=\"red\">%s</font></b>",
                _("clients suspended"));
      }

      if (start_time > 0) {
        if (cs->testing_suspended) {
          fprintf(fout, " / <b><font color=\"red\">%s</font></b>",
                  _("testing suspended"));
        }
        if (cs->printing_suspended) {
          fprintf(fout, " / <b><font color=\"red\">%s</font></b>",
                  _("printing suspended"));
        }
      }

      if (!global->is_virtual && start_time <= 0 && sched_time > 0) {
        fprintf(fout, " / %s: %s",
                _("Start at"),
                brief_time(time_buf, sizeof(time_buf), sched_time));
      }

      if (start_time > 0 && stop_time <= 0 && duration > 0) {
        duration_str(0, start_time + duration - cs->current_time, 0,
                     time_buf, 0);
        fprintf(fout, " / %s: <div id=\"remainingTime\">%s</div>",
                _("Remaining"), time_buf);
      }

      visibility = "hidden";
      if (global->disable_auto_refresh > 0) {
        visibility = "visible";
      }

      fprintf(fout, "<div id=\"reloadButton\" style=\"visibility: %s\">/ <a class=\"menu\" onclick=\"reloadPage()\"><b>[ %s ]</b></a></div><div id=\"statusString\" style=\"visibility: hidden\"></div></div>\n", visibility, _("REFRESH"));
      break;

    default:
      break;
    }
  }
}

static int
get_last_language(serve_state_t cs, int user_id, int *p_last_eoln_type)
{
  int total_runs = run_get_total(cs->runlog_state), run_id;
  struct run_entry re;

  if (p_last_eoln_type) *p_last_eoln_type = 0;
  for (run_id = total_runs - 1; run_id >= 0; run_id--) {
    if (run_get_entry(cs->runlog_state, run_id, &re) < 0) continue;
    if (!run_is_source_available(re.status)) continue;
    if (re.user_id != user_id) continue;
    if (re.lang_id <= 0 || re.lang_id > cs->max_lang || !cs->langs[re.lang_id])
      continue;
    if (p_last_eoln_type) *p_last_eoln_type = re.eoln_type;
    return re.lang_id;
  }
  return 0;
}

static unsigned char *
get_last_source(serve_state_t cs, int user_id, int prob_id)
{
  int total_runs = run_get_total(cs->runlog_state), run_id;
  struct run_entry re;
  int src_flag = 0;
  path_t src_path;
  char *src_txt = 0;
  size_t src_len = 0;
  unsigned char *s;

  for (run_id = total_runs - 1; run_id >= 0; run_id--) {
    if (run_get_entry(cs->runlog_state, run_id, &re) < 0) continue;
    if (!run_is_source_available(re.status)) continue;
    if (re.user_id != user_id || re.prob_id != prob_id) continue;
    break;
  }
  if (run_id < 0) return 0;

  if ((src_flag = serve_make_source_read_path(cs, src_path, sizeof(src_path), &re)) < 0)
    return 0;
  if (generic_read_file(&src_txt, 0, &src_len, src_flag, 0, src_path, 0) < 0)
    return 0;

  s = src_txt;
  while (src_len > 0 && isspace(s[src_len])) src_len--;
  s[src_len] = 0;

  return s;
}

static int
get_last_answer_select_one(serve_state_t cs, int user_id, int prob_id)
{
  unsigned char *s = get_last_source(cs, user_id, prob_id);
  int val;
  char *eptr = 0;

  if (!s || !*s) return -1;
  errno = 0;
  val = strtol(s, &eptr, 10);
  if (*eptr || errno || val <= 0) val = -1;
  xfree(s);
  return val;
}

static int
is_judged_virtual_olympiad(serve_state_t cs, int user_id)
{
  struct run_entry vs, ve;

  if (run_get_virtual_info(cs->runlog_state, user_id, &vs, &ve) < 0) return 0;
  return (vs.judge_id > 0);
}

// problem status flags
enum
{
  PROB_STATUS_VIEWABLE = 1,
  PROB_STATUS_SUBMITTABLE = 2,
  PROB_STATUS_TABABLE = 4,

  PROB_STATUS_GOOD = PROB_STATUS_VIEWABLE | PROB_STATUS_SUBMITTABLE,
};

/*
  *PROBLEM_PARAM(disable_user_submit, "d"),
  *PROBLEM_PARAM(disable_tab, "d"),
  *PROBLEM_PARAM(restricted_statement, "d"),
  *PROBLEM_PARAM(disable_submit_after_ok, "d"),
  *PROBLEM_PARAM(deadline, "s"),
  *PROBLEM_PARAM(start_date, "s"),
  *PROBLEM_PARAM(require, "x"),
  *PROBLEM_PARAM(personal_deadline, "x"),
*/

static void
get_problem_status(serve_state_t cs, int user_id,
                   const unsigned char *user_login,
                   int accepting_mode,
                   time_t start_time,
                   time_t stop_time,
                   const unsigned char *solved_flag,
                   const unsigned char *accepted_flag,
                   unsigned char *pstat)
{
  const struct section_problem_data *prob;
  int prob_id, is_deadlined, k, j;
  time_t user_deadline;

  // nothing before contest start
  if (start_time <= 0) return;

  for (prob_id = 1; prob_id <= cs->max_prob; prob_id++) {
    if (!(prob = cs->probs[prob_id])) continue;

    // the problem is completely disabled before its start_date
    if (!serve_is_problem_started(cs, user_id, prob))
      continue;

    // the problem is completely disabled before requirements are met
    // check requirements
    if (prob->require) {
      for (j = 0; prob->require[j]; j++) {
        for (k = 1; k <= cs->max_prob; k++) {
          if (cs->probs[k]
              && !strcmp(cs->probs[k]->short_name, prob->require[j]))
            break;
        }
        // no such problem :(
        if (k > cs->max_prob) break;
        // this problem is not yet accepted or solved
        if (!solved_flag[k] && !accepted_flag[k]) break;
      }
      // if the requirements are not met, skip this problem
      if (prob->require[j]) continue;
    }

    // check problem deadline
    is_deadlined = serve_is_problem_deadlined(cs, user_id, user_login,
                                              prob, &user_deadline);

    if (prob->restricted_statement <= 0 || !is_deadlined)
      pstat[prob_id] |= PROB_STATUS_VIEWABLE;

    if (!is_deadlined && prob->disable_user_submit <= 0
        && (prob->disable_submit_after_ok <= 0 || !solved_flag[prob_id]))
      pstat[prob_id] |= PROB_STATUS_SUBMITTABLE;

    if (prob->disable_tab <= 0)
      pstat[prob_id] |= PROB_STATUS_TABABLE;
  }
}

static void
write_row(
        FILE *fout,
        const unsigned char *row_label,
        char *format,
        ...)
{
  va_list args;
  char buf[1024];

  va_start(args, format);
  vsnprintf(buf, sizeof(buf), format, args);
  va_end(args);

  fprintf(fout, "<tr><td class=\"b0\"><b>%s:</b></td><td class=\"b0\">%s</td></tr>\n",
          row_label, buf);
}

static void
unparse_statement(
        FILE *fout,
        struct http_request_info *phr,
        const struct contest_desc *cnts,
        struct contest_extra *extra,
        const struct section_problem_data *prob,
        int variant,
        problem_xml_t px,
        const unsigned char *bb,
        int is_submittable)
{
  struct problem_stmt *pp = 0;
  struct xml_tree *p, *q;
  unsigned char b1[1024];
  unsigned char b2[1024];
  unsigned char b3[1024];
  unsigned char b4[1024];
  unsigned char b5[1024];
  unsigned char b6[1024];
  unsigned char b7[1024];
  const unsigned char *vars[8] = { "self", "prob", "get", "getfile", "input_file", "output_file", "variant", 0 };
  const unsigned char *vals[8] = { b1, b2, b3, b4, b5, b6, b7, 0 };
  struct html_armor_buffer ab = HTML_ARMOR_INITIALIZER;

  snprintf(b1, sizeof(b1), "%s?SID=%016llx", phr->self_url, phr->session_id);
  snprintf(b2, sizeof(b2), "&prob_id=%d", prob->id);
  snprintf(b3, sizeof(b3), "&action=%d", NEW_SRV_ACTION_GET_FILE);
  b7[0] = 0;
  if (variant > 0) snprintf(b7, sizeof(b7), "&variant=%d", variant);
  snprintf(b4, sizeof(b4), "%s%s%s%s&file", b1, b2, b3, b7);
  snprintf(b5, sizeof(b5), "%s", prob->input_file);
  snprintf(b6, sizeof(b6), "%s", prob->output_file);

  if (bb && *bb && !cnts->exam_mode) fprintf(fout, "%s", bb);

  pp = problem_xml_find_statement(px, 0);
  if (pp->title) {
    fprintf(fout, "<h3>");
    problem_xml_unparse_node(fout, pp->title, vars, vals);
    fprintf(fout, "</h3>");
  }

  if (prob->type == PROB_TYPE_STANDARD) {
    fprintf(fout, "<table class=\"b0\">\n");
    if (prob->use_stdin <= 0 && prob->input_file[0]) {
      write_row(fout, _("Input file name"), "<tt>%s</tt>",
                ARMOR(prob->input_file));
    }
    if (prob->use_stdout <= 0 && prob->output_file[0]) {
      write_row(fout, _("Output file name"), "<tt>%s</tt>",
                ARMOR(prob->output_file));
    }
    if (prob->time_limit_millis > 0) {
      write_row(fout, _("Time limit"), "%d %s",
                prob->time_limit_millis, _("ms"));
    } else if (prob->time_limit > 0) {
      write_row(fout, _("Time limit"), "%d %s", prob->time_limit, _("s"));
    }
    if (prob->max_vm_size > 0) {
      if (!(prob->max_vm_size % (1024 * 1024))) {
        write_row(fout, _("Memory limit"), "%zu M",
                  prob->max_vm_size / (1024*1024));
      } else {
        write_row(fout, _("Memory limit"), "%zu",
                  prob->max_vm_size);
      }
    }
    fprintf(fout, "</table>\n");
  }

  if (pp->desc) {
    problem_xml_unparse_node(fout, pp->desc, vars, vals);
  }

  if (pp->input_format) {
    fprintf(fout, "<h3>%s</h3>", _("Input format"));
    problem_xml_unparse_node(fout, pp->input_format, vars, vals);
  }
  if (pp->output_format) {
    fprintf(fout, "<h3>%s</h3>", _("Output format"));
    problem_xml_unparse_node(fout, pp->output_format, vars, vals);
  }

  if (px->examples) {
    fprintf(fout, "<h3>%s</h3>", _("Examples"));
    fprintf(fout, "<table class=\"b1\">");
    fprintf(fout, "<tr><td class=\"b1\" align=\"center\"><b>");
    if (prob->use_stdin) {
      fprintf(fout, "%s", _("Input"));
    } else {
      fprintf(fout, "%s <tt>%s</tt>", _("Input in"), prob->input_file);
    }
    fprintf(fout, "</b></td><td class=\"b1\" align=\"center\"><b>");
    if (prob->use_stdout) {
      fprintf(fout, "%s", _("Output"));
    } else {
      fprintf(fout, "%s <tt>%s</tt>", _("Output in"), prob->output_file);
    }
    fprintf(fout, "</b></td></tr>");
    for (p = px->examples->first_down; p; p = p->right) {
      if (p->tag != PROB_T_EXAMPLE) continue;
      fprintf(fout, "<tr><td class=\"b1\" valign=\"top\"><pre>");
      for (q = p->first_down; q && q->tag != PROB_T_INPUT; q = q->right);
      if (q && q->tag == PROB_T_INPUT) problem_xml_unparse_node(fout, q, 0, 0);
      fprintf(fout, "</pre></td><td class=\"b1\" valign=\"top\"><pre>");
      for (q = p->first_down; q && q->tag != PROB_T_OUTPUT; q = q->right);
      if (q && q->tag == PROB_T_OUTPUT) problem_xml_unparse_node(fout, q, 0, 0);
      fprintf(fout, "</pre></td></tr>");
    }
    fprintf(fout, "</table>");
  }

  if (pp->notes) {
    fprintf(fout, "<h3>%s</h3>", _("Notes"));
    problem_xml_unparse_node(fout, pp->notes, vars, vals);
  }

  if (is_submittable) {
    if (prob->type == PROB_TYPE_SELECT_ONE) {
      fprintf(fout, "<h3>%s</h3>", _("Choose an answer"));
    } else {
      fprintf(fout, "<h3>%s</h3>", _("Submit a solution"));
    }
  }

  html_armor_free(&ab);
}

static void
unparse_answers(
        FILE *fout,
        struct http_request_info *phr,
        const struct contest_desc *cnts,
        struct contest_extra *extra,
        const struct section_problem_data *prob,
        int variant,
        problem_xml_t px,
        const unsigned char *lang,
        int is_radio,
        int last_answer,
        int next_prob_id,
        int enable_js,
        const unsigned char *class_name)
{
  unsigned char *cl = "";
  unsigned char jsbuf[128];
  int l, i;
  const unsigned char *s;

  unsigned char b1[1024];
  unsigned char b2[1024];
  unsigned char b3[1024];
  unsigned char b4[1024];
  unsigned char b5[1024];
  unsigned char b6[1024];
  unsigned char b7[1024];
  const unsigned char *vars[8] = { "self", "prob", "get", "getfile", "input_file", "output_file", "variant", 0 };
  const unsigned char *vals[8] = { b1, b2, b3, b4, b5, b6, b7, 0 };

  snprintf(b1, sizeof(b1), "%s?SID=%016llx", phr->self_url, phr->session_id);
  snprintf(b2, sizeof(b2), "&prob_id=%d", prob->id);
  snprintf(b3, sizeof(b3), "&action=%d", NEW_SRV_ACTION_GET_FILE);
  b7[0] = 0;
  if (variant > 0) snprintf(b7, sizeof(b7), "&variant=%d", variant);
  snprintf(b4, sizeof(b4), "%s%s%s%s&file", b1, b2, b3, b7);
  snprintf(b5, sizeof(b5), "%s", prob->input_file);
  snprintf(b6, sizeof(b6), "%s", prob->output_file);

  if (class_name && *class_name) {
    cl = (unsigned char *) alloca(strlen(class_name) + 32);
    sprintf(cl, " class=\"%s\"", class_name);
  }

  l = problem_xml_find_language(lang, px->tr_num, px->tr_names);
  for (i = 0; i < px->ans_num; i++) {
    if (is_radio) {
      jsbuf[0] = 0;
      if (prob->id > 0 && enable_js) {
        snprintf(jsbuf, sizeof(jsbuf), " onclick=\"submitAnswer(%d,%d,%d,%d,%d)\"", NEW_SRV_ACTION_UPDATE_ANSWER, prob->id, i + 1, NEW_SRV_ACTION_VIEW_PROBLEM_SUBMIT, next_prob_id);
      }
      s = "";
      if (last_answer == i + 1) s = " checked=\"1\"";
      fprintf(fout, "<tr><td%s>%d)</td><td%s><input type=\"radio\" name=\"file\" value=\"%d\"%s%s/></td><td%s>", cl, i + 1, cl, i + 1, s, jsbuf, cl);
      problem_xml_unparse_node(fout, px->answers[i][l], vars, vals);
      fprintf(fout, "</td></tr>\n");
    } else {
      fprintf(fout, "<tr><td%s>%d)</td><td%s><input type=\"checkbox\" name=\"ans_%d\"/></td><td%s>", cl, i + 1, cl, i + 1, cl);
      problem_xml_unparse_node(fout, px->answers[i][l], vars, vals);
      fprintf(fout, "</td></tr>\n");
    }
  }
}

static const unsigned char *main_page_headers[NEW_SRV_ACTION_LAST] =
{
  [NEW_SRV_ACTION_MAIN_PAGE] = __("Contest status"),
  [NEW_SRV_ACTION_VIEW_PROBLEM_SUMMARY] = __("Problem summary"),
  [NEW_SRV_ACTION_VIEW_PROBLEM_STATEMENTS] = __("Statements"),
  [NEW_SRV_ACTION_VIEW_PROBLEM_SUBMIT] = __("Submit a solution"),
  [NEW_SRV_ACTION_VIEW_SUBMISSIONS] = __("Submissions"),
  [NEW_SRV_ACTION_VIEW_CLAR_SUBMIT] = __("Send a message"),
  [NEW_SRV_ACTION_VIEW_CLARS] = __("Messages"),
  [NEW_SRV_ACTION_VIEW_SETTINGS] = __("Settings"),
};

static void
unpriv_main_page(FILE *fout,
                 struct http_request_info *phr,
                 const struct contest_desc *cnts,
                 struct contest_extra *extra)
{
  serve_state_t cs = extra->serve_state;
  struct section_global_data *global = cs->global;
  //long long tdiff;
  time_t start_time, stop_time, duration, sched_time, fog_start_time = 0;
  time_t finish_time = 0;
  const unsigned char *s;
  int all_runs = 0, all_clars = 0;
  unsigned char *solved_flag = 0;
  unsigned char *accepted_flag = 0;
  unsigned char *pending_flag = 0;
  unsigned char *trans_flag = 0;
  unsigned char *pr_flag = NULL;
  unsigned char *prob_status = 0;
  time_t *prob_deadline = 0;
  int *best_run = 0;
  int *attempts = 0;
  int *disqualified = 0;
  int *best_score = 0;
  int *prev_successes = 0;
  int *all_attempts = 0;
  int n, v, prob_id = 0, i, j, variant = 0;
  char **lang_list;
  path_t variant_stmt_file;
  struct watched_file *pw = 0;
  const unsigned char *pw_path;
  const struct section_problem_data *prob = 0, *prob2;
  unsigned char bb[1024];
  const unsigned char *alternatives = 0, *header = 0;
  int lang_count = 0, lang_id = 0;
  int first_prob_id, last_prob_id;
  int accepting_mode = 0;
  const unsigned char *hh = 0;
  const unsigned char *cc = 0;
  int last_answer = -1, last_lang_id, skip_start_form = 0;
  struct html_armor_buffer ab = HTML_ARMOR_INITIALIZER;
  unsigned char *last_source = 0;
  unsigned char dbuf[1024];
  int upper_tab_id = 0, next_prob_id;
  problem_xml_t px;
  unsigned char prev_group_name[256] = { 0 };
  const unsigned char *li_attr;
  const unsigned char *div_class;

  if (ns_cgi_param(phr, "all_runs", &s) > 0
      && sscanf(s, "%d%n", &v, &n) == 1 && !s[n] && v >= 0 && v <= 1) {
    phr->session_extra->user_view_all_runs = v;
  }
  all_runs = phr->session_extra->user_view_all_runs;
  if (ns_cgi_param(phr, "all_clars", &s) > 0
      && sscanf(s, "%d%n", &v, &n) == 1 && !s[n] && v >= 0 && v <= 1) {
    phr->session_extra->user_view_all_clars = v;
  }
  all_clars = phr->session_extra->user_view_all_clars;
  if (ns_cgi_param(phr, "prob_id", &s) > 0
      && sscanf(s, "%d%n", &v, &n) == 1 && !s[n] && v >= -1)
    prob_id = v;
  
  XALLOCAZ(solved_flag, cs->max_prob + 1);
  XALLOCAZ(accepted_flag, cs->max_prob + 1);
  XALLOCAZ(pending_flag, cs->max_prob + 1);
  XALLOCAZ(trans_flag, cs->max_prob + 1);
  XALLOCAZ(pr_flag, cs->max_prob + 1);
  XALLOCA(best_run, cs->max_prob + 1);
  memset(best_run, -1, (cs->max_prob + 1) * sizeof(best_run[0]));
  XALLOCAZ(attempts, cs->max_prob + 1);
  XALLOCAZ(disqualified, cs->max_prob + 1);
  XALLOCAZ(best_score, cs->max_prob + 1);
  XALLOCAZ(prev_successes, cs->max_prob + 1);
  XALLOCAZ(all_attempts, cs->max_prob + 1);
  XALLOCAZ(prob_status, cs->max_prob + 1);
  XALLOCAZ(prob_deadline, cs->max_prob + 1);

  if (global->is_virtual) {
    start_time = run_get_virtual_start_time(cs->runlog_state, phr->user_id);
    stop_time = run_get_virtual_stop_time(cs->runlog_state, phr->user_id,
                                          cs->current_time);
    if (stop_time <= 0) accepting_mode = 1;
  } else {
    start_time = run_get_start_time(cs->runlog_state);
    stop_time = run_get_stop_time(cs->runlog_state);
    accepting_mode = cs->accepting_mode;
  }
  run_get_times(cs->runlog_state, 0, &sched_time, &duration, 0, &finish_time);
  if (duration > 0 && start_time && !stop_time && global->board_fog_time > 0)
    fog_start_time = start_time + duration - global->board_fog_time;
  if (fog_start_time < 0) fog_start_time = 0;

  hh = main_page_headers[phr->action];
  if (phr->action == NEW_SRV_ACTION_MAIN_PAGE && cnts->exam_mode) {
    hh = __("Exam status");
  }
  l10n_setlocale(phr->locale_id);
  header = gettext(hh);
  if (!header) header = _("Main page");
  unpriv_load_html_style(phr, cnts, 0, 0);
  ns_header(fout, extra->header_txt, 0, 0, phr->script_part, phr->body_attr, phr->locale_id, cnts,
            phr->client_key,
            "%s [%s]: %s",
            phr->name_arm, extra->contest_arm, header);

  unpriv_page_header(fout, phr, cnts, extra, start_time, stop_time);

  ns_get_user_problems_summary(cs, phr->user_id, accepting_mode,
                               solved_flag, accepted_flag, pending_flag,
                               trans_flag, pr_flag,
                               best_run, attempts, disqualified,
                               best_score, prev_successes, all_attempts);
  get_problem_status(cs, phr->user_id, phr->login, accepting_mode,
                     start_time, stop_time,
                     solved_flag, accepted_flag, prob_status);

  if (global->problem_navigation > 0 && start_time > 0 && stop_time <= 0) {
    if (prob_id > cs->max_prob) prob_id = 0;
    if (prob_id > 0 && !(prob = cs->probs[prob_id])) prob_id = 0;
    if (prob_id > 0 && !(prob_status[prob_id] & PROB_STATUS_GOOD))
      prob_id = 0;
    if (prob_id > 0 && prob->variant_num > 0
        && (variant = find_variant(cs, phr->user_id, prob_id, 0)) <= 0)
      prob_id = 0;

    fprintf(fout, "<br/>\n");
    fprintf(fout, "<table class=\"probNav\">");
    upper_tab_id = prob_id;
    if (global->vertical_navigation <= 0) {
      fprintf(fout, "<tr id=\"probNavTopList\"><td width=\"100%%\" class=\"nTopNavList\"><ul class=\"nTopNavList\">");
      for (i = 1; i <= cs->max_prob; i++) {
        if (!(prob = cs->probs[i])) continue;
        if (!(prob_status[i] & PROB_STATUS_TABABLE)) continue;

        li_attr = "";
        div_class = "nProbBad";
        if (i == prob_id) {
          li_attr = "  id=\"nTopNavSelected\"";
          div_class = "nProbCurrent";
        } else if (!all_attempts[i]) {
          div_class = "nProbEmpty";
        } else if (pending_flag[i] || trans_flag[i]) {
          div_class = "nProbTrans";
        } else if (accepted_flag[i] || solved_flag[i] || pr_flag[i]) {
          div_class = "nProbOk";
        } else if (prob->disable_user_submit > 0) {
          div_class = "nProbDisabled";
        } else {
          div_class = "nProbBad";
        }
        /*
        if (global->problem_tab_size > 0)
          snprintf(wbuf, sizeof(wbuf), " width=\"%dpx\"",
                   global->problem_tab_size);
        */
        fprintf(fout, "<li%s><div class=\"%s\">", li_attr, div_class);

      /*
      if (accepting_mode && accepted_flag[i]) {
        fprintf(fout, "<s>");
      }
      */
        fprintf(fout, "%s%s</a>",
                ns_aref(bb, sizeof(bb), phr,
                        NEW_SRV_ACTION_VIEW_PROBLEM_SUBMIT,
                        "prob_id=%d", i), prob->short_name);
      /*
      if (accepting_mode && accepted_flag[i]) {
        fprintf(fout, "</s>");
      }
      */
        fprintf(fout, "</div></li>");
      }
      fprintf(fout, "</ul></td></tr>");
      fprintf(fout, "<tr><td id=\"probNavTaskArea\" valign=\"top\"><div id=\"probNavTaskArea\">");
    } else {
      fprintf(fout, "<tr><td class=\"b0\" id=\"probNavTaskArea\" valign=\"top\"><div id=\"probNavTaskArea\">");
    }
  }

  if (phr->action == NEW_SRV_ACTION_MAIN_PAGE) {
    unpriv_print_status(fout, phr, cnts, extra,
                        start_time, stop_time, duration, sched_time,
                        fog_start_time, finish_time);
  }

  if (phr->action == NEW_SRV_ACTION_VIEW_STARTSTOP) {
    if (global->is_virtual && start_time <= 0) {
      if (global->disable_virtual_start <= 0) {
        html_start_form(fout, 1, phr->self_url, phr->hidden_vars);
        if (cnts->exam_mode) {
          fprintf(fout, "<p>%s</p></form>",
                  ns_submit_button(bb, sizeof(bb), 0,
                                   NEW_SRV_ACTION_VIRTUAL_START,
                                   _("Start exam")));
        } else {
          fprintf(fout, "<p>%s</p></form>",
                  BUTTON(NEW_SRV_ACTION_VIRTUAL_START));
        }
      }
    } else if (global->is_virtual && stop_time <= 0) {
      if (cnts->exam_mode) {
        fprintf(fout, "<h2>%s</h2>\n", _("Finish the exam"));
        fprintf(fout, "<p>%s</p>\n",
                _("Press \"Stop exam\" button to finish the exam. Your answers will be checked shortly after that."));
      }

      html_start_form(fout, 1, phr->self_url, phr->hidden_vars);
      if (cnts->exam_mode) {
        fprintf(fout, "<p>%s</p></form>",
                ns_submit_button(bb, sizeof(bb), 0, NEW_SRV_ACTION_VIRTUAL_STOP,
                                 _("Stop exam")));
      } else {
        fprintf(fout, "<p>%s</p></form>",
                BUTTON(NEW_SRV_ACTION_VIRTUAL_STOP));
      }
    }
  }

  if (start_time && phr->action == NEW_SRV_ACTION_VIEW_PROBLEM_SUMMARY) {
    if (cnts->exam_mode && global->score_system == SCORE_OLYMPIAD
        && global->is_virtual && stop_time > 0
        && global->disable_virtual_auto_judge > 0
        && !cs->testing_finished) {
      char *ff_txt = 0, *fl_txt = 0;
      size_t ff_len = 0, fl_len = 0;
      FILE *ff = open_memstream(&ff_txt, &ff_len);
      FILE *fl = open_memstream(&fl_txt, &fl_len);
      int rr = ns_olympiad_final_user_report(ff, fl, cnts, cs,
                                             phr->user_id, phr->locale_id);
      if (rr < 0) {
        fprintf(fout, "<%s>%s</%s>\n<p>%s %d</p>",
                cnts->team_head_style,
                _("Problem status summary"),
                cnts->team_head_style, _("Error"), -rr);
        close_memstream(fl); fl = 0; xfree(fl_txt); fl_txt = 0; fl_len = 0;
        close_memstream(ff); ff = 0; xfree(ff_txt); ff_txt = 0; ff_len = 0;
      } else {
        close_memstream(fl); fl = 0;
        if (fl_txt && *fl_txt) {
          fprintf(fout,
                  "<%s>%s</%s>\n<pre><font color=\"red\">%s</font></pre>\n",
                  cnts->team_head_style,
                  _("Problem status summary"),
                  cnts->team_head_style, ARMOR(fl_txt));
          xfree(fl_txt); fl_txt = 0; fl_len = 0;
          close_memstream(ff); ff = 0; xfree(ff_txt); ff_txt = 0; ff_len = 0;
        } else {
          close_memstream(ff); ff = 0; 
          fprintf(fout,
                  "<%s>%s</%s>\n%s\n",
                  cnts->team_head_style,
                  _("Problem status summary"),
                  cnts->team_head_style, ff_txt);
          xfree(fl_txt); fl_txt = 0; fl_len = 0;
          xfree(ff_txt); ff_txt = 0; ff_len = 0;
        }
      }
    } else if (cnts->exam_mode && global->score_system == SCORE_OLYMPIAD
               && global->is_virtual && stop_time > 0
               && (run_has_transient_user_runs(cs->runlog_state, phr->user_id)
                   || (global->disable_virtual_auto_judge <= 0
                       && !is_judged_virtual_olympiad(cs, phr->user_id)))) {
      fprintf(fout, "<%s>%s</%s>\n",
              cnts->team_head_style,
              _("Testing is in progress..."),
              cnts->team_head_style);
    } else {
      fprintf(fout, "<%s>%s</%s>\n",
              cnts->team_head_style,
              _("Problem status summary"),
              cnts->team_head_style);
      if (global->score_system == SCORE_OLYMPIAD
          && global->is_virtual
          && cs->testing_finished)
        accepting_mode = 0;
      if (cs->contest_plugin
          && cs->contest_plugin->generate_html_user_problems_summary) {
        // FIXME: return code and logging stream is not used now
        char *us_text = 0;
        size_t us_size = 0;
        FILE *us_file = open_memstream(&us_text, &us_size);
        (*cs->contest_plugin->generate_html_user_problems_summary)(cs->contest_plugin_data, us_file, fout, cnts, cs, phr->user_id, accepting_mode, "b1", solved_flag, accepted_flag, pending_flag, trans_flag, best_run, attempts, disqualified, best_score, prev_successes);
        close_memstream(us_file); us_file = 0;
        xfree(us_text); us_text = 0;
      } else {
        ns_write_user_problems_summary(cnts, cs, fout, phr->user_id,
                                       accepting_mode, "b1",
                                       solved_flag, accepted_flag, pr_flag, pending_flag,
                                       trans_flag, best_run, attempts,
                                       disqualified, best_score);
      }
    }
  }

  if (phr->action == NEW_SRV_ACTION_VIEW_PROBLEM_STATEMENTS
      && start_time > 0) {
    if (cnts->problems_url) {
      fprintf(fout, "<p><a href=\"%s\">%s</a></p>\n",
              cnts->problems_url, _("Problem statements"));
    }
    // if prob_id == -1, show all available problem statements
    if (prob_id == -1) {
      first_prob_id = 1;
      last_prob_id = cs->max_prob;
    } else {
      first_prob_id = prob_id;
      last_prob_id = prob_id;
    }
    for (prob_id = first_prob_id; prob_id <= last_prob_id; prob_id++) {
      variant = 0;
      if (prob_id <= 0 || prob_id > cs->max_prob) continue;
      if (!(prob = cs->probs[prob_id])) continue;
      if (!serve_is_problem_started(cs, phr->user_id, prob)) continue;
      if (serve_is_problem_deadlined(cs, phr->user_id, phr->login,
                                     prob, &prob_deadline[prob_id]))
        continue;
      if (prob->variant_num > 0
          && (variant = find_variant(cs, phr->user_id, prob_id, 0)) <= 0)
        continue;
      if (!prob->statement_file[0]) continue;
      if (variant > 0) {
        prepare_insert_variant_num(variant_stmt_file, sizeof(variant_stmt_file),
                                   prob->statement_file, variant);
        pw = &cs->prob_extras[prob_id].v_stmts[variant];
        pw_path = variant_stmt_file;
      } else {
        pw = &cs->prob_extras[prob_id].stmt;
        pw_path = prob->statement_file;
      }
      watched_file_update(pw, pw_path, cs->current_time);
      if (!pw->text) continue;

      fprintf(fout, "%s", pw->text);
    }

    fprintf(fout, "<%s>%s</%s>\n",
            cnts->team_head_style, _("Select another problem"),
            cnts->team_head_style);
    html_start_form(fout, 0, phr->self_url, phr->hidden_vars);
    fprintf(fout, "<table class=\"b0\">");
    fprintf(fout, "<tr><td class=\"b0\">%s:</td><td class=\"b0\">", _("Problem"));

    html_problem_selection_2(cs, fout, phr, 0, start_time);

    fprintf(fout, "</td><td class=\"b0\">%s</td></tr></table></form>",
            ns_submit_button(bb, sizeof(bb), 0,
                             NEW_SRV_ACTION_VIEW_PROBLEM_STATEMENTS,
                             _("Select problem")));
  }

  if (phr->action == NEW_SRV_ACTION_VIEW_PROBLEM_SUBMIT
      && !cs->clients_suspended) {
    if (prob_id > cs->max_prob) prob_id = 0;
    if (prob_id > 0 && !(prob = cs->probs[prob_id])) prob_id = 0;
    if (prob_id > 0 && !serve_is_problem_started(cs, phr->user_id, prob))
      prob_id = 0;
    if (prob_id > 0 && serve_is_problem_deadlined(cs, phr->user_id, phr->login,
                                                  prob,
                                                  &prob_deadline[prob_id]))
      prob_id = 0;
    //if (prob_id > 0 && prob->disable_user_submit > 0) prob_id = 0;
    if (prob_id > 0 && prob->variant_num > 0
        && (variant = find_variant(cs, phr->user_id, prob_id, 0)) <= 0)
      prob_id = 0;

    if (start_time > 0 && stop_time <= 0 && !prob_id) {
      fprintf(fout, "<%s>%s</%s>\n",
              cnts->team_head_style,
              _("View the problem statement and send a submission"),
              cnts->team_head_style);
      html_start_form(fout, 0, phr->self_url, phr->hidden_vars);
      fprintf(fout, "<table class=\"b0\">");
      fprintf(fout, "<tr><td class=\"b0\">%s:</td><td class=\"b0\">", _("Problem"));

      html_problem_selection(cs, fout, phr, solved_flag, accepted_flag, 0, 0,
                             start_time);

      fprintf(fout, "</td><td class=\"b0\">%s</td></tr></table></form>",
              ns_submit_button(bb, sizeof(bb), 0, NEW_SRV_ACTION_VIEW_PROBLEM_SUBMIT,
                               _("Select problem")));
    } else if (start_time > 0 && stop_time <= 0 && prob_id > 0) {
      prob = cs->probs[prob_id];

      dbuf[0] = 0;
      if ((prob_status[prob_id] & PROB_STATUS_SUBMITTABLE)
          && prob_deadline[prob_id] > 0) {
        snprintf(dbuf, sizeof(dbuf), "<h3>%s: %s</h3>",
                 _("Problem deadline"),
                 xml_unparse_date(prob_deadline[prob_id]));
      }

      bb[0] = 0;
      if (variant > 0) {
        snprintf(bb, sizeof(bb), "<%s>%s %s-%s (%s %d)</%s>%s\n",
                 cnts->team_head_style,
                 (prob_status[prob_id] & PROB_STATUS_SUBMITTABLE)?_("Submit a solution for"):_("Problem"),
                 prob->short_name, prob->long_name, _("Variant"), variant,
                 cnts->team_head_style, dbuf);
      } else {
        if (cnts->exam_mode) {
          /*
          if (prob->disable_user_submit > 0) {
            snprintf(bb, sizeof(bb), "<%s>%s</%s>\n",
                     cnts->team_head_style,
                     prob->long_name, cnts->team_head_style);
          } else {
            snprintf(bb, sizeof(bb), "<%s>%s %s</%s>\n",
                     cnts->team_head_style, _("Submit a solution for"),
                     prob->long_name, cnts->team_head_style);
          }
          */
          snprintf(bb, sizeof(bb), "<%s>%s %s</%s>%s\n",
                   cnts->team_head_style, _("Problem"),
                   prob->long_name, cnts->team_head_style, dbuf);
        } else {
          if (1 /*!(prob_status[prob_id] & PROB_STATUS_SUBMITTABLE)*/) {
            if (prob->long_name[0]) {
              snprintf(bb, sizeof(bb), "<%s>%s %s-%s</%s>%s\n",
                       cnts->team_head_style, _("Problem"),
                       prob->short_name, prob->long_name, cnts->team_head_style,
                       dbuf);
            } else {
              snprintf(bb, sizeof(bb), "<%s>%s %s</%s>%s\n",
                       cnts->team_head_style, _("Problem"),
                       prob->short_name, cnts->team_head_style, dbuf);
            }
          } else {
            if (prob->long_name[0]) {
              snprintf(bb, sizeof(bb), "<%s>%s %s-%s</%s>%s\n",
                       cnts->team_head_style, _("Submit a solution for"),
                       prob->short_name, prob->long_name, cnts->team_head_style,
                       dbuf);
            } else {
              snprintf(bb, sizeof(bb), "<%s>%s %s</%s>%s\n",
                       cnts->team_head_style, _("Submit a solution for"),
                       prob->short_name, cnts->team_head_style, dbuf);
            }
          }
        }
      }

      if (prob->max_user_run_count > 0) {
        int ignored_set = 0;
        if (prob->ignore_compile_errors > 0) ignored_set |= 1 << RUN_COMPILE_ERR;
        ignored_set |= 1 << RUN_IGNORED;
        int remain_count = prob->max_user_run_count - run_count_all_attempts_2(cs->runlog_state, phr->user_id, prob_id, ignored_set);
        if (remain_count < 0) remain_count = 0;
        fprintf(fout, "<h3>%s: %d</h3>\n", _("Remaining attempts"), remain_count);
        if (remain_count <= 0) prob_status[prob_id] &= ~PROB_STATUS_SUBMITTABLE;
      }

      px = 0;
      if (variant > 0 && prob->xml.a && prob->xml.a[variant - 1]) {
        px = prob->xml.a[variant - 1];
      } else if (variant <= 0 && prob->xml.p) {
        px = prob->xml.p;
      }

      /* put problem statement */
      if (px && px->stmts) {
        unparse_statement(fout, phr, cnts, extra, prob, 0, px, bb,
                          prob_status[prob_id] & PROB_STATUS_SUBMITTABLE);
      } else if (prob->statement_file[0]
          && (prob_status[prob_id] & PROB_STATUS_VIEWABLE)) {
        if (variant > 0) {
          prepare_insert_variant_num(variant_stmt_file,
                                     sizeof(variant_stmt_file),
                                     prob->statement_file, variant);
          pw = &cs->prob_extras[prob_id].v_stmts[variant];
          pw_path = variant_stmt_file;
        } else {
          pw = &cs->prob_extras[prob_id].stmt;
          pw_path = prob->statement_file;
        }
        watched_file_update(pw, pw_path, cs->current_time);
        if (!pw->text) {
          fprintf(fout, "%s<big><font color=\"red\"><p>%s</p></font></big>\n",
                  bb, _("The problem statement is not available"));
        } else {
          if (cnts->exam_mode) bb[0] = 0;
          fprintf(fout, "%s", bb);
          if ((prob_status[prob_id] & PROB_STATUS_SUBMITTABLE)
              && prob->type == PROB_TYPE_CUSTOM) {
            html_start_form(fout, 2, phr->self_url, phr->hidden_vars);
            skip_start_form = 1;
          }
          fprintf(fout, "%s", pw->text);
        }
      } else {
        fprintf(fout, "%s", bb);
      }

      if ((prob_status[prob_id] & PROB_STATUS_SUBMITTABLE)) {
        alternatives = 0;
        if ((prob->type == PROB_TYPE_SELECT_ONE
             || prob->type == PROB_TYPE_SELECT_MANY)
            && prob->alternatives_file[0]) {
          if (variant > 0) {
            prepare_insert_variant_num(variant_stmt_file,
                                       sizeof(variant_stmt_file),
                                       prob->alternatives_file, variant);
            pw = &cs->prob_extras[prob->id].v_alts[variant];
            pw_path = variant_stmt_file;
          } else {
            pw = &cs->prob_extras[prob->id].alt;
            pw_path = prob->alternatives_file;
          }
          watched_file_update(pw, pw_path, cs->current_time);
          alternatives = pw->text;
        }

        if (!skip_start_form) {
          html_start_form(fout, 2, phr->self_url, phr->hidden_vars);
        }
        fprintf(fout, "<input type=\"hidden\" name=\"prob_id\" value=\"%d\"/>",
                prob_id);
        fprintf(fout, "<table class=\"b0\">");
        if (!prob->type) {
          int last_eoln_type = 0;
          for (i = 1; i <= cs->max_lang; i++) {
            if (!cs->langs[i] || cs->langs[i]->disabled
                || (cs->langs[i]->insecure && global->secure_run)) continue;
            if ((lang_list = prob->enable_language)) {
              for (j = 0; lang_list[j]; j++)
                if (!strcmp(lang_list[j], cs->langs[i]->short_name))
                  break;
              if (!lang_list[j]) continue;
            } else if ((lang_list = prob->disable_language)) {
              for (j = 0; lang_list[j]; j++)
                if (!strcmp(lang_list[j], cs->langs[i]->short_name))
                  break;
              if (lang_list[j]) continue;
            }
            lang_count++;
            lang_id = i;
          }

          if (lang_count == 1) {
            html_hidden(fout, "lang_id", "%d", lang_id);
            fprintf(fout, "<tr><td class=\"b0\">%s:</td><td class=\"b0\">%s - %s</td></tr>",
                    _("Language"),
                    cs->langs[lang_id]->short_name,
                    cs->langs[lang_id]->long_name);
          } else {
            last_lang_id = get_last_language(cs, phr->user_id, &last_eoln_type);
            fprintf(fout, "<tr><td class=\"b0\">%s:</td><td class=\"b0\">", _("Language"));
            fprintf(fout, "<select name=\"lang_id\"><option value=\"\">");
            for (i = 1; i <= cs->max_lang; i++) {
              if (!cs->langs[i] || cs->langs[i]->disabled
                  || (cs->langs[i]->insecure && global->secure_run)) continue;
              if ((lang_list = prob->enable_language)) {
                for (j = 0; lang_list[j]; j++)
                  if (!strcmp(lang_list[j], cs->langs[i]->short_name))
                    break;
                if (!lang_list[j]) continue;
              } else if ((lang_list = prob->disable_language)) {
                for (j = 0; lang_list[j]; j++)
                  if (!strcmp(lang_list[j], cs->langs[i]->short_name))
                    break;
                if (lang_list[j]) continue;
              }
              cc = "";
              if (last_lang_id == i) cc = " selected=\"selected\"";
              fprintf(fout, "<option value=\"%d\"%s>%s - %s</option>",
                      i, cc, cs->langs[i]->short_name, cs->langs[i]->long_name);
            }
            fprintf(fout, "</select></td></tr>");
          }

          if (cs->global->enable_eoln_select > 0) {
            fprintf(fout, "<tr><td%s>%s:</td><td%s><select name=\"eoln_type\"%s>",
                    " class=\"b0\"", _("Desired EOLN Type"), " class=\"b0\"", "");
            fprintf(fout, "<option value=\"0\"></option>");
            cc = "";
            if (last_eoln_type == 1) cc = " selected=\"selected=\"";
            fprintf(fout, "<option value=\"1\"%s>LF (Unix/MacOS)</option>", cc);
            cc = "";
            if (last_eoln_type == 2) cc = " selected=\"selected=\"";
            fprintf(fout, "<option value=\"2\"%s>CRLF (Windows/DOS)</option>", cc);
            fprintf(fout, "</select></td></tr>\n");
          }
        }
        switch (prob->type) {
          /*
        case PROB_TYPE_STANDARD:
          fprintf(fout, "<tr><td class=\"b0\">%s</td><td class=\"b0\"><input type=\"file\" name=\"file\"/></td></tr>", _("File"));
          break;
          */
        case PROB_TYPE_STANDARD:
        case PROB_TYPE_OUTPUT_ONLY:
        case PROB_TYPE_TESTS:
          if (prob->enable_text_form > 0) {
            fprintf(fout, "<tr><td colspan=\"2\" class=\"b0\"><textarea name=\"text_form\" rows=\"20\" cols=\"60\"></textarea></td></tr>");
          }
          fprintf(fout, "<tr><td class=\"b0\">%s</td><td class=\"b0\"><input type=\"file\" name=\"file\"/></td></tr>", _("File"));
          break;
        case PROB_TYPE_SHORT_ANSWER:
          last_source = 0;
          if (cnts->exam_mode) {
            last_source = get_last_source(cs, phr->user_id, prob->id);
          }
          if (last_source) {
            fprintf(fout, "<tr><td class=\"b0\">%s</td><td class=\"b0\"><input type=\"text\" name=\"file\" value=\"%s\"/></td></tr>", _("Answer"), ARMOR(last_source));
          } else {
            fprintf(fout, "<tr><td class=\"b0\">%s</td><td class=\"b0\"><input type=\"text\" name=\"file\"/></td></tr>", _("Answer"));
          }
        xfree(last_source); last_source = 0;
          break;
        case PROB_TYPE_TEXT_ANSWER:
          fprintf(fout, "<tr><td colspan=\"2\" class=\"b0\"><textarea name=\"file\" rows=\"20\" cols=\"60\"></textarea></td></tr>");
          break;
        case PROB_TYPE_SELECT_ONE:
          last_answer = -1;
          if (cnts->exam_mode) {
            last_answer = get_last_answer_select_one(cs, phr->user_id,
                                                     prob->id);
          }

          if (px) {
            next_prob_id = prob->id;
            if (cnts->exam_mode) {
              if (prob->advance_to_next > 0) {
                next_prob_id++;
                for (; next_prob_id <= cs->max_prob; next_prob_id++) {
                  if (!(prob2 = cs->probs[next_prob_id])) continue;
                  if (!serve_is_problem_started(cs, phr->user_id, prob2))
                    continue;
                  break;
                }
                if (next_prob_id > cs->max_prob) next_prob_id = prob->id;
              }
              unparse_answers(fout, phr, cnts, extra, prob, variant,
                              px, 0 /* lang */, 1 /* is_radio */,
                              last_answer, next_prob_id,
                              1 /* js_flag */, "b0");
            } else {
              unparse_answers(fout, phr, cnts, extra, prob, variant,
                              px, 0 /* lang */, 1 /* is_radio */,
                              last_answer, next_prob_id,
                              0 /* js_flag */, "b0");
            }
          } else if (alternatives) {
            if (cnts->exam_mode) {
              next_prob_id = prob->id;
              if (prob->advance_to_next > 0) {
                next_prob_id++;
                for (; next_prob_id <= cs->max_prob; next_prob_id++) {
                  if (!(prob2 = cs->probs[next_prob_id])) continue;
                  if (!serve_is_problem_started(cs, phr->user_id, prob2))
                    continue;
                  break;
                }
                if (next_prob_id > cs->max_prob) next_prob_id = prob->id;
              }
              write_alternatives_file(fout, 1, alternatives, last_answer,
                                      prob->id, next_prob_id, 1, "b0");
            } else {
              write_alternatives_file(fout, 1, alternatives, last_answer,
                                      0, 0, 0, "b0");
            }
          } else if (prob->alternative) {
            for (i = 0; prob->alternative[i]; i++) {
              cc = "";
              if (i + 1 == last_answer) cc = " checked=\"1\"";
              fprintf(fout, "<tr><td class=\"b0\">%d</td><td class=\"b0\"><input type=\"radio\" name=\"file\" value=\"%d\"%s/></td><td>%s</td></tr>", i + 1, i + 1, cc, prob->alternative[i]);
            }
          }
          break;
        case PROB_TYPE_SELECT_MANY:
          if (alternatives) {
            write_alternatives_file(fout, 0, alternatives, -1, 0, 0, 0, "b0");
          } else if (prob->alternative) {
            for (i = 0; prob->alternative[i]; i++) {
              fprintf(fout, "<tr><td class=\"b0\">%d</td><td class=\"b0\"><input type=\"checkbox\" name=\"ans_%d\"/></td><td>%s</td></tr>", i + 1, i + 1, prob->alternative[i]);
            }
          }
          break;
        case PROB_TYPE_CUSTOM:
          break;
        }
        if (cnts->exam_mode) {
          if (prob->type != PROB_TYPE_SELECT_ONE) {
            cc = "";
            if (prob && (prob->type == PROB_TYPE_SELECT_MANY || prob->type == PROB_TYPE_SELECT_ONE)) cc = "<td class=\"b0\">&nbsp;</td>";
            fprintf(fout, "<tr>%s<td class=\"b0\">&nbsp;</td><td class=\"b0\">%s</td></tr></table></form>", cc,
                    ns_submit_button(bb, sizeof(bb), 0,
                                     NEW_SRV_ACTION_SUBMIT_RUN,
                                     _("Submit solution!")));
          } else {
            fprintf(fout, "</tr></table></form>");
          }
        } else {
          fprintf(fout, "<tr><td class=\"b0\">%s</td><td class=\"b0\">%s</td></tr></table></form>",
                  _("Send!"),
                  BUTTON(NEW_SRV_ACTION_SUBMIT_RUN));
        }
      } /* prob->disable_user_submit <= 0 */

      if (global->problem_navigation
          //&& !prob->disable_user_submit
          && prob->type != PROB_TYPE_SELECT_ONE
          && all_attempts[prob->id]) {
        if (all_attempts[prob->id] <= 15) {
          fprintf(fout, "<%s>%s</%s>\n",
                  cnts->team_head_style,
                  _("Previous submissions of this problem"),
                  cnts->team_head_style);
        } else {
          fprintf(fout, "<%s>%s (%s)</%s>\n",
                  cnts->team_head_style,
                  _("Previous submissions of this problem"),
                  /*all_runs?_("all"):*/_("last 15"),
                  cnts->team_head_style);
        }
        if (cs->contest_plugin && cs->contest_plugin->generate_html_user_runs){
          // FIXME: logged output is also ignored
          // FIXME: return code is ignored for now
          char *ur_text = 0;
          size_t ur_size = 0;
          FILE *ur_file = open_memstream(&ur_text, &ur_size);
          (*cs->contest_plugin->generate_html_user_runs)(cs->contest_plugin_data, ur_file, fout, cnts, cs, phr, phr->user_id, prob_id, all_runs, "b1");
          close_memstream(ur_file); ur_file = 0;
          xfree(ur_text); ur_text = 0;
        } else if (global->score_system == SCORE_OLYMPIAD) {
          ns_write_olympiads_user_runs(phr, fout, cnts, extra, all_runs,
                                       prob_id, "b1");
        } else {
          new_write_user_runs(cs, fout, phr->user_id, all_runs, prob->id,
                              NEW_SRV_ACTION_VIEW_SOURCE,
                              NEW_SRV_ACTION_VIEW_REPORT,
                              NEW_SRV_ACTION_PRINT_RUN,
                              phr->session_id, phr->self_url,
                              phr->hidden_vars, "", "b1");
        }
      }

      if (!cnts->exam_mode) {
        if (global->problem_navigation <= 0) {
          fprintf(fout, "<%s>%s</%s>\n",
                  cnts->team_head_style, _("Select another problem"),
                  cnts->team_head_style);
        } else {
          /*
          fprintf(fout, "<%s>%s</%s>\n",
                  cnts->team_head_style, _("Problem navigation"),
                  cnts->team_head_style);
          */
        }
        html_start_form(fout, 0, phr->self_url, phr->hidden_vars);
        fprintf(fout, "<table class=\"b0\">");
        fprintf(fout, "<tr>");

        if (global->problem_navigation > 0) {
          for (i = prob_id - 1; i > 0; i--) {
            if (!(prob_status[i] & PROB_STATUS_GOOD)) continue;
            break;
          }
          if (i > 0) {
            fprintf(fout, "<td class=\"b0\">%s%s</a></td>",
                    ns_aref(bb, sizeof(bb), phr,
                            NEW_SRV_ACTION_VIEW_PROBLEM_SUBMIT,
                            "prob_id=%d", i), _("Previous problem"));
          }
        }

        if (global->problem_navigation <= 0) {
          fprintf(fout, "<td class=\"b0\">%s:</td><td class=\"b0\">", _("Problem"));
          html_problem_selection(cs, fout, phr, solved_flag, accepted_flag, 0,
                                 0, start_time);
          fprintf(fout, "</td><td class=\"b0\">%s</td>",
                  ns_submit_button(bb, sizeof(bb), 0,
                                   NEW_SRV_ACTION_VIEW_PROBLEM_SUBMIT,
                                   _("Select problem")));
        }

        if (global->problem_navigation > 0) {
          for (i = prob_id + 1; i <= cs->max_prob; i++) {
            if (!(prob_status[i] & PROB_STATUS_GOOD)) continue;
            break;
          }
          if (i <= cs->max_prob) {
            fprintf(fout, "<td class=\"b0\">%s%s</a></td>",
                    ns_aref(bb, sizeof(bb), phr,
                            NEW_SRV_ACTION_VIEW_PROBLEM_SUBMIT,
                            "prob_id=%d", i), _("Next problem"));
          }
        }

      fprintf(fout, "</tr></table></form>");
      }
    }
  }

  if (phr->action == NEW_SRV_ACTION_VIEW_SUBMISSIONS && start_time > 0) {
    fprintf(fout, "<%s>%s (%s)</%s>\n",
            cnts->team_head_style,
            _("Sent submissions"),
            all_runs?_("all"):_("last 15"),
            cnts->team_head_style);

    if (cs->contest_plugin && cs->contest_plugin->generate_html_user_runs){
      // FIXME: logged output is also ignored
      // FIXME: return code is ignored for now
      char *ur_text = 0;
      size_t ur_size = 0;
      FILE *ur_file = open_memstream(&ur_text, &ur_size);
      (*cs->contest_plugin->generate_html_user_runs)(cs->contest_plugin_data, ur_file, fout, cnts, cs, phr, phr->user_id, 0, all_runs, "b1");
      close_memstream(ur_file); ur_file = 0;
      xfree(ur_text); ur_text = 0;
    } else if (global->score_system == SCORE_OLYMPIAD) {
      ns_write_olympiads_user_runs(phr, fout, cnts, extra, all_runs,
                                   0, "b1");
    } else {
      new_write_user_runs(cs, fout, phr->user_id, all_runs, 0,
                          NEW_SRV_ACTION_VIEW_SOURCE,
                          NEW_SRV_ACTION_VIEW_REPORT,
                          NEW_SRV_ACTION_PRINT_RUN,
                          phr->session_id, phr->self_url,
                          phr->hidden_vars, "", "b1");
    }
    if (all_runs) s = _("View last 15");
    else s = _("View all");
    fprintf(fout, "<p><a href=\"%s?SID=%016llx&amp;all_runs=%d&amp;action=%d\">%s</a></p>\n", phr->self_url, phr->session_id, !all_runs, NEW_SRV_ACTION_VIEW_SUBMISSIONS, s);
  }


  if (phr->action == NEW_SRV_ACTION_VIEW_CLAR_SUBMIT
      && !cs->clients_suspended) {
    if (!global->disable_clars && !global->disable_team_clars
        && start_time > 0 && stop_time <= 0) {
      fprintf(fout, "<%s>%s</%s>\n",
              cnts->team_head_style, _("Send a message to judges"),
              cnts->team_head_style);
      html_start_form(fout, 2, phr->self_url, phr->hidden_vars);
      fprintf(fout, "<table class=\"b0\"><tr><td class=\"b0\">%s:</td><td class=\"b0\">", _("Problem"));
      html_problem_selection(cs, fout, phr, solved_flag, accepted_flag, 0, 1,
                             start_time);
      fprintf(fout, "</td></tr><tr><td class=\"b0\">%s:</td>"
              "<td class=\"b0\"><input type=\"text\" name=\"subject\"/></td></tr>"
              "<tr><td colspan=\"2\" class=\"b0\"><textarea name=\"text\" rows=\"20\" cols=\"60\"></textarea></td></tr>"
              "<tr><td colspan=\"2\" class=\"b0\">%s</td></tr>"
              "</table></form>",
              _("Subject"), BUTTON(NEW_SRV_ACTION_SUBMIT_CLAR));
    }
    if (!global->disable_clars && !global->disable_team_clars
        && start_time > 0 && stop_time > 0
        && global->appeal_deadline > 0
        && cs->current_time < global->appeal_deadline) {
      fprintf(fout, "<%s>%s</%s>\n",
              cnts->team_head_style, _("Send an appeal"),
              cnts->team_head_style);
      html_start_form(fout, 2, phr->self_url, phr->hidden_vars);
      fprintf(fout, "<table class=\"b0\"><tr><td class=\"b0\">%s:</td><td class=\"b0\">", _("Problem"));
      html_problem_selection(cs, fout, phr, solved_flag, accepted_flag, 0, 1,
                             start_time);
      fprintf(fout, "</td></tr><tr><td class=\"b0\">%s:</td>"
              "<td class=\"b0\"><input type=\"text\" name=\"test\"/></td></tr>"
              "<tr><td colspan=\"2\" class=\"b0\"><textarea name=\"text\" rows=\"20\" cols=\"60\"></textarea></td></tr>"
              "<tr><td colspan=\"2\" class=\"b0\">%s</td></tr>"
              "</table></form>",
              _("Test number"), BUTTON(NEW_SRV_ACTION_SUBMIT_APPEAL));
    }
  }

  if (phr->action == NEW_SRV_ACTION_VIEW_CLARS && !global->disable_clars) {
    fprintf(fout, "<%s>%s (%s)</%s>\n",
            cnts->team_head_style, _("Messages"),
            all_clars?_("all"):_("last 15"), cnts->team_head_style);

    new_write_user_clars(cs, fout, phr->user_id, all_clars,
                         NEW_SRV_ACTION_VIEW_CLAR,
                         phr->session_id,
                         phr->self_url, phr->hidden_vars, "", "b1");

    if (all_clars) s = _("View last 15");
    else s = _("View all");
    fprintf(fout, "<p><a href=\"%s?SID=%016llx&amp;all_clars=%d&amp;action=%d\">%s</a></p>\n", phr->self_url, phr->session_id, !all_clars, NEW_SRV_ACTION_VIEW_CLARS, s);
  }

  if (phr->action == NEW_SRV_ACTION_VIEW_SETTINGS) {
    /* change the password */
    if (!cs->clients_suspended && !cnts->disable_password_change) {
      fprintf(fout, "<%s>%s</%s>\n",
              cnts->team_head_style,
              _("Change password"),
              cnts->team_head_style);
      html_start_form(fout, 1, phr->self_url, phr->hidden_vars);

      fprintf(fout, "<table class=\"b0\">"
              "<tr><td class=\"b0\">%s:</td><td class=\"b0\"><input type=\"password\" name=\"oldpasswd\" size=\"16\"/></td></tr>"
              "<tr><td class=\"b0\">%s:</td><td class=\"b0\"><input type=\"password\" name=\"newpasswd1\" size=\"16\"/></td></tr>"
              "<tr><td class=\"b0\">%s:</td><td class=\"b0\"><input type=\"password\" name=\"newpasswd2\" size=\"16\"/></td></tr>"
              "<tr><td class=\"b0\" colspan=\"2\"><input type=\"submit\" name=\"action_%d\" value=\"%s\"/></td></tr>"
              "</table></form>",
              _("Old password"),
              _("New password"), _("Retype new password"),
              NEW_SRV_ACTION_CHANGE_PASSWORD, _("Change!"));
    }

#if CONF_HAS_LIBINTL - 0 == 1
    if (global->enable_l10n && !cs->clients_suspended
        && !cnts->disable_locale_change) {
      fprintf(fout, "<%s>%s</%s>\n",
              cnts->team_head_style, _("Change language"),
              cnts->team_head_style);
      html_start_form(fout, 1, phr->self_url, phr->hidden_vars);
      fprintf(fout, "<table class=\"b0\"><tr><td class=\"b0\">%s</td><td class=\"b0\">", _("Change language"));
      l10n_html_locale_select(fout, phr->locale_id);
      fprintf(fout, "</td><td class=\"b0\"><input type=\"submit\" name=\"action_%d\" value=\"%s\"/></td></tr></table></form>",
              NEW_SRV_ACTION_CHANGE_LANGUAGE, _("Change"));
    }
#endif /* CONF_HAS_LIBINTL */
  }

  /* new problem navigation */
  if (global->problem_navigation > 0 && global->vertical_navigation > 0
      && start_time > 0 && stop_time <= 0) {
    fprintf(fout, "</div></td><td class=\"b0\" id=\"probNavRightList\" valign=\"top\">");
    prev_group_name[0] = 0;

    for (i = 1, j = 0; i <= cs->max_prob; i++) {
      if (!(prob = cs->probs[i])) continue;
      if (!(prob_status[i] & PROB_STATUS_TABABLE)) continue;

      if (prob->group_name[0] && strcmp(prob->group_name, prev_group_name)) {
        fprintf(fout, "<div class=\"%s\">", "probDisabled");
        fprintf(fout, "%s", prob->group_name);
        fprintf(fout, "</div>");
        snprintf(prev_group_name, sizeof(prev_group_name),
                 "%s", prob->group_name);
      }

      if (i == prob_id) {
        cc = "probCurrent";
      } else if (!all_attempts[i]) {
        cc = "probEmpty";
      } else if (pending_flag[i] || trans_flag[i]) {
        cc = "probTrans";
      } else if (accepted_flag[i] || solved_flag[i] || pr_flag[i]) {
        cc = "probOk";
      } else if (prob->disable_user_submit > 0) {
        cc = "probDisabled";
      } else {
        cc = "probBad";
      }
      fprintf(fout, "<div class=\"%s\">", cc);
      /*
      if (accepting_mode && accepted_flag[i]) {
        fprintf(fout, "<s>");
      }
      */
      fprintf(fout, "%s%s</a>",
              ns_aref_2(bb, sizeof(bb), phr, "tab",
                        NEW_SRV_ACTION_VIEW_PROBLEM_SUBMIT,
                        "prob_id=%d", i), prob->short_name);
      /*
      if (accepting_mode && accepted_flag[i]) {
        fprintf(fout, "</s>");
      }
      */
      fprintf(fout, "</div>");
      j++;
    }
    fprintf(fout, "</td></tr></table>");
  } else if (global->problem_navigation > 0
             && start_time > 0 && stop_time <= 0) {
    fprintf(fout, "</div></td></tr>");

    fprintf(fout, "<tr id=\"probNavBottomList\"><td width=\"100%%\" class=\"nBottomNavList\"><ul class=\"nBottomNavList\">");
    for (i = 1; i <= cs->max_prob; i++) {
      if (!(prob = cs->probs[i])) continue;
      if (!(prob_status[i] & PROB_STATUS_TABABLE)) continue;

      div_class = "nProbBad";
      li_attr = "";
      if (i == upper_tab_id) {
        div_class = "nProbCurrent";
        li_attr = " id=\"nBottomNavSelected\"";
      } else if (!all_attempts[i]) {
        div_class = "nProbEmpty";
      } else if (pending_flag[i] || trans_flag[i]) {
        div_class = "nProbTrans";
      } else if (accepted_flag[i] || solved_flag[i] || pr_flag[i]) {
        div_class = "nProbOk";
      } else if (prob->disable_user_submit > 0) {
        div_class = "nProbDisabled";
      } else {
        div_class = "nProbBad";
      }
      /*
      if (global->problem_tab_size > 0)
        snprintf(wbuf, sizeof(wbuf), " width=\"%dpx\"",
                 global->problem_tab_size);
      */

      fprintf(fout, "<li%s><div class=\"%s\">", li_attr, div_class);

      /*
      if (accepting_mode && accepted_flag[i]) {
        fprintf(fout, "<s>");
      }
      */
      fprintf(fout, "%s%s</a>",
              ns_aref(bb, sizeof(bb), phr,
                      NEW_SRV_ACTION_VIEW_PROBLEM_SUBMIT,
                      "prob_id=%d", i), prob->short_name);
      /*
      if (accepting_mode && accepted_flag[i]) {
        fprintf(fout, "</s>");
      }
      */
      fprintf(fout, "</div></li>");
    }
    fprintf(fout, "</ul></td></tr></table>");
  }

#if 0
  if (!cnts->exam_mode /*&& global->show_generation_time*/) {
    gettimeofday(&phr->timestamp2, 0);
    tdiff = ((long long) phr->timestamp2.tv_sec) * 1000000;
    tdiff += phr->timestamp2.tv_usec;
    tdiff -= ((long long) phr->timestamp1.tv_sec) * 1000000;
    tdiff -= phr->timestamp1.tv_usec;
    fprintf(fout, "<div class=\"dotted\"><p class=\"dotted\">%s: %lld %s</p></div>",
            _("Page generation time"), tdiff / 1000,
            _("msec"));
  }
#endif

  ns_footer(fout, extra->footer_txt, extra->copyright_txt, phr->locale_id);
  l10n_setlocale(0);
  html_armor_free(&ab);
}

static void
unpriv_logout(FILE *fout,
              struct http_request_info *phr,
              const struct contest_desc *cnts,
              struct contest_extra *extra)
{
  //unsigned char locale_buf[64];
  unsigned char urlbuf[1024];

  if (ns_open_ul_connection(phr->fw_state) < 0)
    return ns_html_err_ul_server_down(fout, phr, 0, 0);
  userlist_clnt_delete_cookie(ul_conn, phr->user_id, phr->contest_id,
                              phr->session_id,
                              phr->client_key);
  ns_remove_session(phr->session_id);
  snprintf(urlbuf, sizeof(urlbuf),
           "%s?contest_id=%d&locale_id=%d",
           phr->self_url, phr->contest_id, phr->locale_id);
  ns_refresh_page_2(fout, phr->client_key, urlbuf);
}

static void
do_json_user_state(
        FILE *fout,
        const serve_state_t cs,
        int user_id,
        int need_reload_check)
{
  const struct section_global_data *global = cs->global;
  struct tm *ptm;
  time_t start_time = 0, stop_time = 0, duration = 0, remaining;
  int has_transient;

  if (global->is_virtual) {
    start_time = run_get_virtual_start_time(cs->runlog_state, user_id);
    stop_time = run_get_virtual_stop_time(cs->runlog_state, user_id,
                                          cs->current_time);
  } else {
    start_time = run_get_start_time(cs->runlog_state);
    stop_time = run_get_stop_time(cs->runlog_state);
  }
  duration = run_get_duration(cs->runlog_state);

  ptm = localtime(&cs->current_time);
  fprintf(fout, "{"
          " \"h\": %d,"
          " \"m\": %d,"
          " \"s\": %d,"
          " \"d\": %d,"
          " \"o\": %d,"
          " \"y\": %d",
          ptm->tm_hour, ptm->tm_min, ptm->tm_sec,
          ptm->tm_mday, ptm->tm_mon + 1, ptm->tm_year + 1900);
  if (start_time > 0 && stop_time <= 0 && duration > 0) {
    remaining = start_time + duration - cs->current_time;
    if (remaining < 0) remaining = 0;
    fprintf(fout, ", \"r\": %ld", remaining);
  }
  if (global->disable_auto_refresh <= 0) {
    has_transient = run_has_transient_user_runs(cs->runlog_state, user_id);
    if (has_transient ||
        (global->score_system == SCORE_OLYMPIAD
         && global->is_virtual
         && stop_time > 0
         && global->disable_virtual_auto_judge <= 0
         && !is_judged_virtual_olympiad(cs, user_id))) {
      fprintf(fout, ", \"x\": 1");
    }
    if (need_reload_check && !has_transient) {
      fprintf(fout, ", \"z\": 1");
    }
  }
  fprintf(fout, " }");
}

static void
unpriv_json_user_state(
        FILE *fout,
        struct http_request_info *phr,
        const struct contest_desc *cnts,
        struct contest_extra *extra)
{
  const serve_state_t cs = extra->serve_state;
  int need_reload_check = 0;

  ns_cgi_param_int_opt(phr, "x", &need_reload_check, 0);

  fprintf(fout, "Content-type: text/plain; charset=%s\n"
          "Cache-Control: no-cache\n\n", EJUDGE_CHARSET);
  do_json_user_state(fout, cs, phr->user_id, need_reload_check);
}

static void
unpriv_xml_update_answer(
        FILE *fout,
        struct http_request_info *phr,
        const struct contest_desc *cnts,
        struct contest_extra *extra)
{
  const serve_state_t cs = extra->serve_state;
  const struct section_global_data *global = cs->global;
  const struct section_problem_data *prob = 0;
  int retval = 0;
  const unsigned char *s;
  int prob_id = 0, n, ans, i, variant = 0, j, run_id;
  struct html_armor_buffer ab = HTML_ARMOR_INITIALIZER;
  const unsigned char *run_text = 0;
  unsigned char *tmp_txt = 0;
  size_t run_size = 0, tmp_size = 0;
  char *eptr;
  time_t start_time, stop_time, user_deadline = 0;
  ruint32_t shaval[5];
  unsigned char *acc_probs = 0;
  struct timeval precise_time;
  int new_flag = 0, arch_flags = 0;
  path_t run_path;
  struct run_entry nv;

  if (global->score_system != SCORE_OLYMPIAD
      || !cs->accepting_mode) FAIL(NEW_SRV_ERR_PERMISSION_DENIED);

  if (ns_cgi_param(phr, "prob_id", &s) <= 0
      || sscanf(s, "%d%n", &prob_id, &n) != 1 || s[n]
      || prob_id <= 0 || prob_id > cs->max_prob
      || !(prob = cs->probs[prob_id]))
    FAIL(NEW_SRV_ERR_INV_PROB_ID);
  if (prob->type != PROB_TYPE_SELECT_ONE)
    FAIL(NEW_SRV_ERR_PERMISSION_DENIED);
  if (!ns_cgi_param_bin(phr, "file", &run_text, &run_size))
    FAIL(NEW_SRV_ERR_ANSWER_UNSPECIFIED);
  if (strlen(run_text) != run_size)
    FAIL(NEW_SRV_ERR_BINARY_FILE);
  if (!run_size)
    FAIL(NEW_SRV_ERR_SUBMIT_EMPTY);

  tmp_txt = alloca(run_size + 1);
  memcpy(tmp_txt, run_text, run_size);
  tmp_txt[run_size] = 0;
  tmp_size = run_size;
  while (tmp_size > 0 && isspace(tmp_txt[tmp_size])) tmp_size--;
  tmp_txt[tmp_size] = 0;
  if (!tmp_size) FAIL(NEW_SRV_ERR_SUBMIT_EMPTY);
  errno = 0;
  ans = strtol(tmp_txt, &eptr, 10);
  if (errno || *eptr || ans < 0) FAIL(NEW_SRV_ERR_INV_ANSWER);

  if (global->is_virtual) {
    start_time = run_get_virtual_start_time(cs->runlog_state, phr->user_id);
    stop_time = run_get_virtual_stop_time(cs->runlog_state, phr->user_id,
                                          cs->current_time);
  } else {
    start_time = run_get_start_time(cs->runlog_state);
    stop_time = run_get_stop_time(cs->runlog_state);
  }

  if (cs->clients_suspended) FAIL(NEW_SRV_ERR_CLIENTS_SUSPENDED);
  if (!start_time) FAIL(NEW_SRV_ERR_CONTEST_NOT_STARTED);
  if (stop_time) FAIL(NEW_SRV_ERR_CONTEST_ALREADY_FINISHED);
  if (serve_check_user_quota(cs, phr->user_id, run_size) < 0)
    FAIL(NEW_SRV_ERR_RUN_QUOTA_EXCEEDED);
  // problem submit start time
  if (!serve_is_problem_started(cs, phr->user_id, prob))
    FAIL(NEW_SRV_ERR_PROB_UNAVAILABLE);

  if (serve_is_problem_deadlined(cs, phr->user_id, phr->login, prob,
                                 &user_deadline)) {
    FAIL(NEW_SRV_ERR_PROB_DEADLINE_EXPIRED);
  }

  if (prob->variant_num > 0) {
    if ((variant = find_variant(cs, phr->user_id, prob_id, 0)) <= 0)
      FAIL(NEW_SRV_ERR_VARIANT_UNASSIGNED);
  }

  sha_buffer(run_text, run_size, shaval);

  if (prob->require) {
    if (!acc_probs) {
      XALLOCAZ(acc_probs, cs->max_prob + 1);
      run_get_accepted_set(cs->runlog_state, phr->user_id,
                           cs->accepting_mode, cs->max_prob, acc_probs);
    }
    for (i = 0; prob->require[i]; i++) {
      for (j = 1; j <= cs->max_prob; j++)
        if (cs->probs[j] && !strcmp(cs->probs[j]->short_name, prob->require[i]))
          break;
      if (j > cs->max_prob || !acc_probs[j]) break;
    }
    if (prob->require[i]) FAIL(NEW_SRV_ERR_NOT_ALL_REQ_SOLVED);
  }

  ruint32_t run_uuid[4];
  int store_flags = 0;
  run_id = run_find(cs->runlog_state, -1, 0, phr->user_id, prob->id, 0, run_uuid, &store_flags);
  if (run_id < 0) {
    gettimeofday(&precise_time, 0);
    ej_uuid_generate(run_uuid);
    if (global->uuid_run_store > 0 && run_get_uuid_hash_state(cs->runlog_state) >= 0 && ej_uuid_is_nonempty(run_uuid)) {
      store_flags = 1;
    }
    run_id = run_add_record(cs->runlog_state, 
                            precise_time.tv_sec, precise_time.tv_usec * 1000,
                            run_size, shaval, run_uuid,
                            &phr->ip, phr->ssl_flag,
                            phr->locale_id, phr->user_id,
                            prob_id, 0, 0, 0, 0, 0, store_flags);
    if (run_id < 0) FAIL(NEW_SRV_ERR_RUNLOG_UPDATE_FAILED);
    serve_move_files_to_insert_run(cs, run_id);
    new_flag = 1;
  }

  if (arch_flags == 1) {
    arch_flags = uuid_archive_prepare_write_path(cs, run_path, sizeof(run_path),
                                                 run_uuid, run_size, DFLT_R_UUID_SOURCE, 0, 0);
  } else {
    arch_flags = archive_prepare_write_path(cs, run_path, sizeof(run_path),
                                            global->run_archive_dir, run_id,
                                            run_size, NULL, 0, 0);
  }
  if (arch_flags < 0) {
    if (new_flag) run_undo_add_record(cs->runlog_state, run_id);
    FAIL(NEW_SRV_ERR_DISK_WRITE_ERROR);
  }

  if (generic_write_file(run_text, run_size, arch_flags, 0, run_path, "") < 0) {
    if (new_flag) run_undo_add_record(cs->runlog_state, run_id);
    FAIL(NEW_SRV_ERR_DISK_WRITE_ERROR);
  }

  memset(&nv, 0, sizeof(nv));
  nv.size = run_size;
  memcpy(nv.sha1, shaval, sizeof(nv.sha1));
  nv.status = RUN_ACCEPTED;
  nv.test = 0;
  nv.score = -1;
  run_set_entry(cs->runlog_state, run_id,
                RE_SIZE | RE_SHA1 | RE_STATUS | RE_TEST | RE_SCORE, &nv);

  serve_audit_log(cs, run_id, NULL, phr->user_id, &phr->ip, phr->ssl_flag,
                  "update-answer", "ok", RUN_ACCEPTED, NULL);

 cleanup:
  fprintf(fout, "Content-type: text/plain; charset=%s\n"
          "Cache-Control: no-cache\n\n", EJUDGE_CHARSET);
  if (!retval) {
    fprintf(fout, "{ \"status\": %d }\n", retval);
  } else {
    l10n_setlocale(phr->locale_id);
    fprintf(fout, "{ \"status\": %d, \"text\": \"%s\" }\n", -retval,
            ARMOR(ns_strerror_2(retval)));
    l10n_setlocale(0);
  }

  html_armor_free(&ab);
}

static void
unpriv_get_file(
        FILE *fout,
        struct http_request_info *phr,
        const struct contest_desc *cnts,
        struct contest_extra *extra)
{
  const serve_state_t cs = extra->serve_state;
  const struct section_global_data *global = cs->global;
  const struct section_problem_data *prob = 0;
  int retval = 0, prob_id, n, variant = 0, mime_type = 0;
  const unsigned char *s = 0;
  time_t user_deadline = 0, start_time, stop_time;
  path_t fpath, sfx;
  char *file_bytes = 0;
  size_t file_size = 0;
  const unsigned char *content_type = 0;

  if (ns_cgi_param(phr, "prob_id", &s) <= 0
      || sscanf(s, "%d%n", &prob_id, &n) != 1 || s[n]
      || prob_id <= 0 || prob_id > cs->max_prob
      || !(prob = cs->probs[prob_id]))
    FAIL(NEW_SRV_ERR_INV_PROB_ID);

  // check, that this problem may be viewed
  if (global->is_virtual) {
    start_time = run_get_virtual_start_time(cs->runlog_state, phr->user_id);
    stop_time = run_get_virtual_stop_time(cs->runlog_state, phr->user_id,
                                          cs->current_time);
  } else {
    start_time = run_get_start_time(cs->runlog_state);
    stop_time = run_get_stop_time(cs->runlog_state);
  }

  if (cs->clients_suspended) FAIL(NEW_SRV_ERR_CLIENTS_SUSPENDED);
  if (start_time <= 0) FAIL(NEW_SRV_ERR_CONTEST_NOT_STARTED);
  if (stop_time > 0 && cs->current_time >= stop_time
      && prob->restricted_statement > 0)
    FAIL(NEW_SRV_ERR_CONTEST_ALREADY_FINISHED);
  if (!serve_is_problem_started(cs, phr->user_id, prob))
    FAIL(NEW_SRV_ERR_PROB_UNAVAILABLE);

  if (serve_is_problem_deadlined(cs, phr->user_id, phr->login,
                                 prob, &user_deadline)
      && prob->restricted_statement > 0)
    FAIL(NEW_SRV_ERR_CONTEST_ALREADY_FINISHED);

  // FIXME: check requisites
  /*
    // the problem is completely disabled before requirements are met
    // check requirements
    if (prob->require) {
      for (j = 0; prob->require[j]; j++) {
        for (k = 1; k <= cs->max_prob; k++) {
          if (cs->probs[k]
              && !strcmp(cs->probs[k]->short_name, prob->require[j]))
            break;
        }
        // no such problem :(
        if (k > cs->max_prob) break;
        // this problem is not yet accepted or solved
        if (!solved_flag[k] && !accepted_flag[k]) break;
      }
      // if the requirements are not met, skip this problem
      if (prob->require[j]) continue;
    }
   */

  if (prob->variant_num > 0
      && (variant = find_variant(cs, phr->user_id, prob_id, 0)) <= 0)
      FAIL(NEW_SRV_ERR_VARIANT_UNASSIGNED);

  if (ns_cgi_param(phr, "file", &s) <= 0 || strchr(s, '/'))
    FAIL(NEW_SRV_ERR_INV_FILE_NAME);

  os_rGetSuffix(s, sfx, sizeof(sfx));
  if (global->advanced_layout) {
    get_advanced_layout_path(fpath, sizeof(fpath), global, prob, s, variant);
  } else {
    if (variant > 0) {
      snprintf(fpath, sizeof(fpath), "%s/%s-%d/%s",
               global->statement_dir, prob->short_name, variant, s);
    } else {
      snprintf(fpath, sizeof(fpath), "%s/%s/%s",
               global->statement_dir, prob->short_name, s);
    }
  }
  mime_type = mime_type_parse_suffix(sfx);
  content_type = mime_type_get_type(mime_type);

  if (generic_read_file(&file_bytes, 0, &file_size, 0, 0, fpath, "") < 0)
    FAIL(NEW_SRV_ERR_INV_FILE_NAME);

  fprintf(fout, "Content-type: %s\n\n", content_type);
  fwrite(file_bytes, 1, file_size, fout);

 cleanup:
  if (retval) {
    snprintf(fpath, sizeof(fpath), "Error %d", -retval);
    html_error_status_page(fout, phr, cnts, extra, fpath,
                           NEW_SRV_ACTION_MAIN_PAGE, 0);
  }
  xfree(file_bytes);
}

static void
anon_select_contest_page(FILE *fout, struct http_request_info *phr)
  __attribute__((unused));
static void
anon_select_contest_page(FILE *fout, struct http_request_info *phr)
{
  struct html_armor_buffer ab = HTML_ARMOR_INITIALIZER;
  const int *cntslist = 0;
  int cntsnum = 0;
  const unsigned char *cl;
  const struct contest_desc *cnts;
  time_t curtime = time(0);
  int row = 0, i, orig_locale_id, j;
  const unsigned char *s;
  const unsigned char *login = 0;
  unsigned char bb[1024];

  ns_cgi_param(phr, "login", &login);

  // defaulting to English as we have no contest chosen
  orig_locale_id = phr->locale_id;
  if (phr->locale_id < 0) phr->locale_id = 0;

  // even don't know about the contest specific settings
  l10n_setlocale(phr->locale_id);
  ns_header(fout, ns_fancy_header, 0, 0, 0, 0, phr->locale_id, NULL,
            NULL_CLIENT_KEY,
            _("Contest selection"));

  html_start_form(fout, 1, phr->self_url, "");
  fprintf(fout, "<div class=\"user_actions\"><table class=\"menu\"><tr>\n");
  html_hidden(fout, "action", "%d", NEW_SRV_ACTION_CHANGE_LANGUAGE);
  fprintf(fout, "<td class=\"menu\"><div class=\"user_action_item\">%s: ",
          _("language"));
  l10n_html_locale_select(fout, phr->locale_id);
  fprintf(fout, "</div></td>\n");
  fprintf(fout, "<td class=\"menu\"><div class=\"user_action_item\">%s</div></td>\n", ns_submit_button(bb, sizeof(bb), "submit", 0, _("Change Language")));
  fprintf(fout, "</tr></table></div></form>\n");

  fprintf(fout,
          "<div class=\"white_empty_block\">&nbsp;</div>\n"
          "<div class=\"contest_actions\"><table class=\"menu\"><tr>\n");

  fprintf(fout, "<td class=\"menu\"><div class=\"contest_actions_item\">&nbsp;</div></td></tr></table></div>\n");

  ns_separator(fout, ns_fancy_separator, NULL);

  fprintf(fout, "<h2>%s</h2>\n", _("Select one of available contests"));

  cntsnum = contests_get_list(&cntslist);
  cl = " class=\"b1\"";
  fprintf(fout, "<table%s><tr>"
          "<td%s>N</td><td%s>%s</td></tr>\n",
          cl, cl, cl, _("Contest name"));
  for (j = 0; j < cntsnum; j++) {
    i = cntslist[j];
    cnts = 0;
    if (contests_get(i, &cnts) < 0 || !cnts) continue;
    if (cnts->closed) continue;
    if (!contests_check_register_ip_2(cnts, &phr->ip, phr->ssl_flag)) continue;
    if (cnts->reg_deadline > 0 && curtime >= cnts->reg_deadline) continue;

    fprintf(fout, "<tr%s><td%s>%d</td>", form_row_attrs[(row++) & 1], cl, i);
    fprintf(fout, "<td%s><a href=\"%s?contest_id=%d", cl, phr->self_url, i);

    if (orig_locale_id >= 0 && cnts->default_locale_num >= 0
        && orig_locale_id != cnts->default_locale_num) {
      fprintf(fout, "&amp;locale_id=%d", phr->locale_id);
    }

    if (login && *login) fprintf(fout, "&amp;login=%s", URLARMOR(login));
    s = 0;
    if (phr->locale_id == 0 && cnts->name_en) s = cnts->name_en;
    if (!s) s = cnts->name;
    fprintf(fout, "\">%s</a></td>", ARMOR(s));
    fprintf(fout, "</tr>\n");
  }
  fprintf(fout, "</table>\n");

  ns_footer(fout, ns_fancy_footer, 0, phr->locale_id);
  l10n_setlocale(0);

  html_armor_free(&ab);
}

static action_handler_t user_actions_table[NEW_SRV_ACTION_LAST] =
{
  [NEW_SRV_ACTION_CHANGE_LANGUAGE] = unpriv_change_language,
  [NEW_SRV_ACTION_CHANGE_PASSWORD] = unpriv_change_password,
  [NEW_SRV_ACTION_SUBMIT_RUN] = unpriv_submit_run,
  [NEW_SRV_ACTION_SUBMIT_CLAR] = unpriv_submit_clar,
  [NEW_SRV_ACTION_LOGOUT] = unpriv_logout,
  [NEW_SRV_ACTION_VIEW_SOURCE] = unpriv_view_source,
  [NEW_SRV_ACTION_VIEW_REPORT] = unpriv_view_report,
  [NEW_SRV_ACTION_VIEW_CLAR] = unpriv_view_clar,
  [NEW_SRV_ACTION_PRINT_RUN] = unpriv_print_run,
  [NEW_SRV_ACTION_VIEW_TEST_INPUT] = unpriv_view_test,
  [NEW_SRV_ACTION_VIEW_TEST_ANSWER] = unpriv_view_test,
  [NEW_SRV_ACTION_VIEW_TEST_INFO] = unpriv_view_test,
  [NEW_SRV_ACTION_VIEW_TEST_OUTPUT] = unpriv_view_test,
  [NEW_SRV_ACTION_VIEW_TEST_ERROR] = unpriv_view_test,
  [NEW_SRV_ACTION_VIEW_TEST_CHECKER] = unpriv_view_test,
  [NEW_SRV_ACTION_SUBMIT_APPEAL] = unpriv_submit_appeal,
  [NEW_SRV_ACTION_STANDINGS] = unpriv_view_standings,
  [NEW_SRV_ACTION_VIRTUAL_START] = unpriv_command,
  [NEW_SRV_ACTION_VIRTUAL_STOP] = unpriv_command,
  [NEW_SRV_ACTION_JSON_USER_STATE] = unpriv_json_user_state,
  [NEW_SRV_ACTION_UPDATE_ANSWER] = unpriv_xml_update_answer,
  [NEW_SRV_ACTION_GET_FILE] = unpriv_get_file,
};

static void
unprivileged_entry_point(
        FILE *fout,
        struct http_request_info *phr,
        int orig_locale_id)
{
  int r, i;
  const struct contest_desc *cnts = 0;
  struct contest_extra *extra = 0;
  time_t cur_time = time(0);
  unsigned char hid_buf[1024];
  struct teamdb_db_callbacks callbacks;
  struct last_access_info *pp;
  int online_users = 0;
  serve_state_t cs = 0;
  const unsigned char *s = 0;

  if (phr->action == NEW_SRV_ACTION_FORGOT_PASSWORD_1)
    return unpriv_page_forgot_password_1(fout, phr, orig_locale_id);
  if (phr->action == NEW_SRV_ACTION_FORGOT_PASSWORD_2)
    return unpriv_page_forgot_password_2(fout, phr, orig_locale_id);
  if (phr->action == NEW_SRV_ACTION_FORGOT_PASSWORD_3)
    return unpriv_page_forgot_password_3(fout, phr, orig_locale_id);

  if ((phr->contest_id < 0 || contests_get(phr->contest_id, &cnts) < 0 || !cnts)
      && !phr->session_id && ejudge_config->enable_contest_select){
    return anon_select_contest_page(fout, phr);
  }

  if (!phr->session_id || phr->action == NEW_SRV_ACTION_LOGIN_PAGE)
    return unprivileged_page_login(fout, phr, orig_locale_id);

  // validate cookie
  if (ns_open_ul_connection(phr->fw_state) < 0)
    return ns_html_err_ul_server_down(fout, phr, 0, 0);
  if ((r = userlist_clnt_get_cookie(ul_conn, ULS_TEAM_GET_COOKIE,
                                    &phr->ip, phr->ssl_flag,
                                    phr->session_id,
                                    phr->client_key,
                                    &phr->user_id, &phr->contest_id,
                                    &phr->locale_id, 0, &phr->role, 0, 0, 0,
                                    &phr->login, &phr->name)) < 0) {
    if (r < 0 && orig_locale_id < 0 && cnts && cnts->default_locale_num >= 0) {
      phr->locale_id = cnts->default_locale_num;
    }
    switch (-r) {
    case ULS_ERR_NO_COOKIE:
    case ULS_ERR_CANNOT_PARTICIPATE:
    case ULS_ERR_NOT_REGISTERED:
      return ns_html_err_inv_session(fout, phr, 0,
                                     "get_cookie failed: %s",
                                     userlist_strerror(-r));
    case ULS_ERR_INCOMPLETE_REG:
      return ns_html_err_registration_incomplete(fout, phr);
    case ULS_ERR_DISCONNECT:
      return ns_html_err_ul_server_down(fout, phr, 0, 0);
    default:
      return ns_html_err_internal_error(fout, phr, 0, "get_cookie failed: %s",
                                        userlist_strerror(-r));
    }
  }

  if (phr->contest_id < 0 || contests_get(phr->contest_id, &cnts) < 0 || !cnts){
    //return anon_select_contest_page(fout, phr);
    return ns_html_err_no_perm(fout, phr, 1, "invalid contest_id %d",
                               phr->contest_id);
  }
  extra = ns_get_contest_extra(phr->contest_id);
  ASSERT(extra);

  if (!contests_check_team_ip(phr->contest_id, &phr->ip, phr->ssl_flag))
    return ns_html_err_no_perm(fout, phr, 0, "%s://%s is not allowed for USER for contest %d", ns_ssl_flag_str[phr->ssl_flag], xml_unparse_ipv6(&phr->ip), phr->contest_id);
  if (cnts->closed)
    return ns_html_err_service_not_available(fout, phr, 0,
                                             "contest %d is closed", cnts->id);
  if (!cnts->managed)
    return ns_html_err_service_not_available(fout, phr, 0,
                                             "contest %d is not managed",
                                             cnts->id);

  watched_file_update(&extra->header, cnts->team_header_file, cur_time);
  watched_file_update(&extra->menu_1, cnts->team_menu_1_file, cur_time);
  watched_file_update(&extra->menu_2, cnts->team_menu_2_file, cur_time);
  watched_file_update(&extra->separator, cnts->team_separator_file, cur_time);
  watched_file_update(&extra->footer, cnts->team_footer_file, cur_time);
  watched_file_update(&extra->copyright, cnts->copyright_file, cur_time);
  extra->header_txt = extra->header.text;
  extra->menu_1_txt = extra->menu_1.text;
  extra->menu_2_txt = extra->menu_2.text;
  extra->separator_txt = extra->separator.text;
  extra->footer_txt = extra->footer.text;
  extra->copyright_txt = extra->copyright.text;
  //if (!extra->header_txt) extra->header_txt = ns_fancy_header;
  //if (!extra->footer_txt) extra->footer_txt = ns_fancy_footer;

  if (phr->name && *phr->name) {
    phr->name_arm = html_armor_string_dup(phr->name);
  } else {
    phr->name_arm = html_armor_string_dup(phr->login);
  }
  if (extra->contest_arm) xfree(extra->contest_arm);
  if (phr->locale_id == 0 && cnts->name_en) {
    extra->contest_arm = html_armor_string_dup(cnts->name_en);
  } else {
    extra->contest_arm = html_armor_string_dup(cnts->name);
  }

  snprintf(hid_buf, sizeof(hid_buf),
           "<input type=\"hidden\" name=\"SID\" value=\"%016llx\"/>",
           phr->session_id);
  phr->hidden_vars = hid_buf;
  phr->session_extra = ns_get_session(phr->session_id, phr->client_key, cur_time);

  memset(&callbacks, 0, sizeof(callbacks));
  callbacks.user_data = (void*) phr->fw_state;
  callbacks.list_all_users = ns_list_all_users_callback;

  // invoke the contest
  if (serve_state_load_contest(ejudge_config, phr->contest_id,
                               ul_conn,
                               &callbacks,
                               &extra->serve_state, 0, 0) < 0) {
    return ns_html_err_cnts_unavailable(fout, phr, 0, NULL, 0);
  }

  cs = extra->serve_state;
  cs->current_time = time(0);
  ns_check_contest_events(cs, cnts);

  // check the user map
  if (phr->user_id >= extra->user_access_idx.a) {
    int new_size = extra->user_access_idx.a;
    if (!new_size) new_size = 128;
    while (phr->user_id >= new_size) new_size *= 2;
    short *new_idx = (short*) xmalloc(new_size * sizeof(new_idx[0]));
    memset(new_idx, -1, new_size * sizeof(new_idx[0]));
    if (extra->user_access_idx.a > 0) {
      memcpy(new_idx, extra->user_access_idx.v,
             extra->user_access_idx.a * sizeof(new_idx[0]));
    }
    xfree(extra->user_access_idx.v);
    extra->user_access_idx.a = new_size;
    extra->user_access_idx.v = new_idx;
  }
  if (extra->user_access_idx.v[phr->user_id] < 0
      && extra->user_access[USER_ROLE_CONTESTANT].u < 32000) {
    struct last_access_array *p = &extra->user_access[USER_ROLE_CONTESTANT];
    if (p->u == p->a) {
      if (!p->a) p->a = 64;
      p->a *= 2;
      XREALLOC(p->v, p->a);
    }
    extra->user_access_idx.v[phr->user_id] = p->u;
    memset(&p->v[p->u], 0, sizeof(p->v[0]));
    p->v[p->u].user_id = phr->user_id;
    p->u++;
  }
  if ((i = extra->user_access_idx.v[phr->user_id]) >= 0) {
    struct last_access_info *pp=&extra->user_access[USER_ROLE_CONTESTANT].v[i];
    pp->ip = phr->ip;
    pp->ssl = phr->ssl_flag;
    pp->time = cs->current_time;
  }

  // count number of users online
  online_users = 0;
  for (i = 0; i < extra->user_access[USER_ROLE_CONTESTANT].u; i++) {
    pp = &extra->user_access[USER_ROLE_CONTESTANT].v[i];
    if (pp->time + 65 >= cs->current_time) online_users++;
  }
  if (online_users > cs->max_online_count) {
    cs->max_online_count = online_users;
    cs->max_online_time = cs->current_time;
    serve_update_status_file(cs, 1);
  }
  phr->online_users = online_users;

  if ((teamdb_get_flags(cs->teamdb_state, phr->user_id) & TEAM_DISQUALIFIED))
    return ns_html_err_disqualified(fout, phr, cnts, extra);

  /* FIXME: redirect just logged in user to an appropriate page */
  if (ns_cgi_param(phr, "lt", &s) > 0) {
    // contest is not started: no nothing
    // contest finished, not olympiad, standings enabled -> standings
  }

  if (phr->action > 0 && phr->action < NEW_SRV_ACTION_LAST
      && user_actions_table[phr->action]) {
    user_actions_table[phr->action](fout, phr, cnts, extra);
  } else {
    if (phr->action < 0 || phr->action >= NEW_SRV_ACTION_LAST)
      phr->action = 0;
    unpriv_main_page(fout, phr, cnts, extra);
  }
}

static int
get_register_url(
        unsigned char *buf,
        size_t size,
        const struct contest_desc *cnts,
        const unsigned char *self_url)
{
  int i, len;

  if (cnts->register_url)
    return snprintf(buf, size, "%s", cnts->register_url);

  if (!self_url) return snprintf(buf, size, "%s", "/new-register");
  len = strlen(self_url);
  for (i = len - 1; i >= 0 && self_url[i] != '/'; i--);
  if (i < 0) return snprintf(buf, size, "%s", "/new-register");
#if defined CGI_PROG_SUFFIX
  return snprintf(buf, size, "%.*s/new-register%s", i, self_url,
                  CGI_PROG_SUFFIX);
#else
  return snprintf(buf, size, "%.*s/new-register", i, self_url);
#endif
}

static const unsigned char * const symbolic_action_table[NEW_SRV_ACTION_LAST] =
{
  [NEW_SRV_ACTION_LOGIN_PAGE] = "LOGIN_PAGE",
  [NEW_SRV_ACTION_MAIN_PAGE] = "MAIN_PAGE",
  [NEW_SRV_ACTION_COOKIE_LOGIN] = "COOKIE_LOGIN",
  [NEW_SRV_ACTION_VIEW_USERS] = "VIEW_USERS",
  [NEW_SRV_ACTION_VIEW_ONLINE_USERS] = "VIEW_ONLINE_USERS",
  [NEW_SRV_ACTION_USERS_REMOVE_REGISTRATIONS] = "USERS_REMOVE_REGISTRATIONS",
  [NEW_SRV_ACTION_USERS_SET_PENDING] = "USERS_SET_PENDING",
  [NEW_SRV_ACTION_USERS_SET_OK] = "USERS_SET_OK",
  [NEW_SRV_ACTION_USERS_SET_REJECTED] = "USERS_SET_REJECTED",
  [NEW_SRV_ACTION_USERS_SET_INVISIBLE] = "USERS_SET_INVISIBLE",
  [NEW_SRV_ACTION_USERS_CLEAR_INVISIBLE] = "USERS_CLEAR_INVISIBLE",
  [NEW_SRV_ACTION_USERS_SET_BANNED] = "USERS_SET_BANNED",
  [NEW_SRV_ACTION_USERS_CLEAR_BANNED] = "USERS_CLEAR_BANNED",
  [NEW_SRV_ACTION_USERS_SET_LOCKED] = "USERS_SET_LOCKED",
  [NEW_SRV_ACTION_USERS_CLEAR_LOCKED] = "USERS_CLEAR_LOCKED",
  [NEW_SRV_ACTION_USERS_SET_INCOMPLETE] = "USERS_SET_INCOMPLETE",
  [NEW_SRV_ACTION_USERS_CLEAR_INCOMPLETE] = "USERS_CLEAR_INCOMPLETE",
  [NEW_SRV_ACTION_USERS_SET_DISQUALIFIED] = "USERS_SET_DISQUALIFIED",
  [NEW_SRV_ACTION_USERS_CLEAR_DISQUALIFIED] = "USERS_CLEAR_DISQUALIFIED",
  [NEW_SRV_ACTION_USERS_ADD_BY_LOGIN] = "USERS_ADD_BY_LOGIN",
  [NEW_SRV_ACTION_USERS_ADD_BY_USER_ID] = "USERS_ADD_BY_USER_ID",
  [NEW_SRV_ACTION_PRIV_USERS_VIEW] = "PRIV_USERS_VIEW",
  [NEW_SRV_ACTION_PRIV_USERS_REMOVE] = "PRIV_USERS_REMOVE",
  [NEW_SRV_ACTION_PRIV_USERS_ADD_OBSERVER] = "PRIV_USERS_ADD_OBSERVER",
  [NEW_SRV_ACTION_PRIV_USERS_DEL_OBSERVER] = "PRIV_USERS_DEL_OBSERVER",
  [NEW_SRV_ACTION_PRIV_USERS_ADD_EXAMINER] = "PRIV_USERS_ADD_EXAMINER",
  [NEW_SRV_ACTION_PRIV_USERS_DEL_EXAMINER] = "PRIV_USERS_DEL_EXAMINER",
  [NEW_SRV_ACTION_PRIV_USERS_ADD_CHIEF_EXAMINER] = "PRIV_USERS_ADD_CHIEF_EXAMINER",
  [NEW_SRV_ACTION_PRIV_USERS_DEL_CHIEF_EXAMINER] = "PRIV_USERS_DEL_CHIEF_EXAMINER",
  [NEW_SRV_ACTION_PRIV_USERS_ADD_COORDINATOR] = "PRIV_USERS_ADD_COORDINATOR",
  [NEW_SRV_ACTION_PRIV_USERS_DEL_COORDINATOR] = "PRIV_USERS_DEL_COORDINATOR",
  [NEW_SRV_ACTION_PRIV_USERS_ADD_BY_LOGIN] = "PRIV_USERS_ADD_BY_LOGIN",
  [NEW_SRV_ACTION_PRIV_USERS_ADD_BY_USER_ID] = "PRIV_USERS_ADD_BY_USER_ID",
  [NEW_SRV_ACTION_CHANGE_LANGUAGE] = "CHANGE_LANGUAGE",
  [NEW_SRV_ACTION_CHANGE_PASSWORD] = "CHANGE_PASSWORD",
  [NEW_SRV_ACTION_VIEW_SOURCE] = "VIEW_SOURCE",
  [NEW_SRV_ACTION_VIEW_REPORT] = "VIEW_REPORT",
  [NEW_SRV_ACTION_PRINT_RUN] = "PRINT_RUN",
  [NEW_SRV_ACTION_VIEW_CLAR] = "VIEW_CLAR",
  [NEW_SRV_ACTION_SUBMIT_RUN] = "SUBMIT_RUN",
  [NEW_SRV_ACTION_SUBMIT_CLAR] = "SUBMIT_CLAR",
  [NEW_SRV_ACTION_START_CONTEST] = "START_CONTEST",
  [NEW_SRV_ACTION_STOP_CONTEST] = "STOP_CONTEST",
  [NEW_SRV_ACTION_CONTINUE_CONTEST] = "CONTINUE_CONTEST",
  [NEW_SRV_ACTION_SCHEDULE] = "SCHEDULE",
  [NEW_SRV_ACTION_CHANGE_DURATION] = "CHANGE_DURATION",
  [NEW_SRV_ACTION_UPDATE_STANDINGS_1] = "UPDATE_STANDINGS_1",
  [NEW_SRV_ACTION_RESET_1] = "RESET_1",
  [NEW_SRV_ACTION_SUSPEND] = "SUSPEND",
  [NEW_SRV_ACTION_RESUME] = "RESUME",
  [NEW_SRV_ACTION_TEST_SUSPEND] = "TEST_SUSPEND",
  [NEW_SRV_ACTION_TEST_RESUME] = "TEST_RESUME",
  [NEW_SRV_ACTION_PRINT_SUSPEND] = "PRINT_SUSPEND",
  [NEW_SRV_ACTION_PRINT_RESUME] = "PRINT_RESUME",
  [NEW_SRV_ACTION_SET_JUDGING_MODE] = "SET_JUDGING_MODE",
  [NEW_SRV_ACTION_SET_ACCEPTING_MODE] = "SET_ACCEPTING_MODE",
  [NEW_SRV_ACTION_SET_TESTING_FINISHED_FLAG] = "SET_TESTING_FINISHED_FLAG",
  [NEW_SRV_ACTION_CLEAR_TESTING_FINISHED_FLAG] = "CLEAR_TESTING_FINISHED_FLAG",
  [NEW_SRV_ACTION_GENERATE_PASSWORDS_1] = "GENERATE_PASSWORDS_1",
  [NEW_SRV_ACTION_CLEAR_PASSWORDS_1] = "CLEAR_PASSWORDS_1",
  [NEW_SRV_ACTION_GENERATE_REG_PASSWORDS_1] = "GENERATE_REG_PASSWORDS_1",
  [NEW_SRV_ACTION_RELOAD_SERVER] = "RELOAD_SERVER",
  [NEW_SRV_ACTION_PRIV_SUBMIT_CLAR] = "PRIV_SUBMIT_CLAR",
  [NEW_SRV_ACTION_PRIV_SUBMIT_RUN_COMMENT] = "PRIV_SUBMIT_RUN_COMMENT",
  [NEW_SRV_ACTION_RESET_FILTER] = "RESET_FILTER",
  [NEW_SRV_ACTION_CLEAR_RUN] = "CLEAR_RUN",
  [NEW_SRV_ACTION_CHANGE_STATUS] = "CHANGE_STATUS",
  [NEW_SRV_ACTION_REJUDGE_ALL_1] = "REJUDGE_ALL_1",
  [NEW_SRV_ACTION_REJUDGE_SUSPENDED_1] = "REJUDGE_SUSPENDED_1",
  [NEW_SRV_ACTION_REJUDGE_DISPLAYED_1] = "REJUDGE_DISPLAYED_1",
  [NEW_SRV_ACTION_FULL_REJUDGE_DISPLAYED_1] = "FULL_REJUDGE_DISPLAYED_1",
  [NEW_SRV_ACTION_SQUEEZE_RUNS] = "SQUEEZE_RUNS",
  [NEW_SRV_ACTION_RESET_CLAR_FILTER] = "RESET_CLAR_FILTER",
  [NEW_SRV_ACTION_LOGOUT] = "LOGOUT",
  [NEW_SRV_ACTION_CHANGE_RUN_USER_ID] = "CHANGE_RUN_USER_ID",
  [NEW_SRV_ACTION_CHANGE_RUN_USER_LOGIN] = "CHANGE_RUN_USER_LOGIN",
  [NEW_SRV_ACTION_CHANGE_RUN_PROB_ID] = "CHANGE_RUN_PROB_ID",
  [NEW_SRV_ACTION_CHANGE_RUN_VARIANT] = "CHANGE_RUN_VARIANT",
  [NEW_SRV_ACTION_CHANGE_RUN_LANG_ID] = "CHANGE_RUN_LANG_ID",
  [NEW_SRV_ACTION_CHANGE_RUN_IS_IMPORTED] = "CHANGE_RUN_IS_IMPORTED",
  [NEW_SRV_ACTION_CHANGE_RUN_IS_HIDDEN] = "CHANGE_RUN_IS_HIDDEN",
  [NEW_SRV_ACTION_CHANGE_RUN_IS_EXAMINABLE] = "CHANGE_RUN_IS_EXAMINABLE",
  [NEW_SRV_ACTION_CHANGE_RUN_IS_READONLY] = "CHANGE_RUN_IS_READONLY",
  [NEW_SRV_ACTION_CHANGE_RUN_IS_MARKED] = "CHANGE_RUN_IS_MARKED",
  [NEW_SRV_ACTION_CHANGE_RUN_IS_SAVED] = "CHANGE_RUN_IS_SAVED",
  [NEW_SRV_ACTION_CHANGE_RUN_STATUS] = "CHANGE_RUN_STATUS",
  [NEW_SRV_ACTION_CHANGE_RUN_TEST] = "CHANGE_RUN_TEST",
  [NEW_SRV_ACTION_CHANGE_RUN_SCORE] = "CHANGE_RUN_SCORE",
  [NEW_SRV_ACTION_CHANGE_RUN_SCORE_ADJ] = "CHANGE_RUN_SCORE_ADJ",
  [NEW_SRV_ACTION_CHANGE_RUN_PAGES] = "CHANGE_RUN_PAGES",
  [NEW_SRV_ACTION_PRIV_DOWNLOAD_RUN] = "PRIV_DOWNLOAD_RUN",
  [NEW_SRV_ACTION_COMPARE_RUNS] = "COMPARE_RUNS",
  [NEW_SRV_ACTION_UPLOAD_REPORT] = "UPLOAD_REPORT",
  [NEW_SRV_ACTION_STANDINGS] = "STANDINGS",
  [NEW_SRV_ACTION_REJUDGE_PROBLEM_1] = "REJUDGE_PROBLEM_1",
  [NEW_SRV_ACTION_CLAR_REPLY] = "CLAR_REPLY",
  [NEW_SRV_ACTION_CLAR_REPLY_ALL] = "CLAR_REPLY_ALL",
  [NEW_SRV_ACTION_CLAR_REPLY_READ_PROBLEM] = "CLAR_REPLY_READ_PROBLEM",
  [NEW_SRV_ACTION_CLAR_REPLY_NO_COMMENTS] = "CLAR_REPLY_NO_COMMENTS",
  [NEW_SRV_ACTION_CLAR_REPLY_YES] = "CLAR_REPLY_YES",
  [NEW_SRV_ACTION_CLAR_REPLY_NO] = "CLAR_REPLY_NO",
  [NEW_SRV_ACTION_REJUDGE_DISPLAYED_2] = "REJUDGE_DISPLAYED_2",
  [NEW_SRV_ACTION_FULL_REJUDGE_DISPLAYED_2] = "FULL_REJUDGE_DISPLAYED_2",
  [NEW_SRV_ACTION_REJUDGE_PROBLEM_2] = "REJUDGE_PROBLEM_2",
  [NEW_SRV_ACTION_REJUDGE_ALL_2] = "REJUDGE_ALL_2",
  [NEW_SRV_ACTION_REJUDGE_SUSPENDED_2] = "REJUDGE_SUSPENDED_2",
  [NEW_SRV_ACTION_VIEW_TEST_INPUT] = "VIEW_TEST_INPUT",
  [NEW_SRV_ACTION_VIEW_TEST_ANSWER] = "VIEW_TEST_ANSWER",
  [NEW_SRV_ACTION_VIEW_TEST_INFO] = "VIEW_TEST_INFO",
  [NEW_SRV_ACTION_VIEW_TEST_OUTPUT] = "VIEW_TEST_OUTPUT",
  [NEW_SRV_ACTION_VIEW_TEST_ERROR] = "VIEW_TEST_ERROR",
  [NEW_SRV_ACTION_VIEW_TEST_CHECKER] = "VIEW_TEST_CHECKER",
  [NEW_SRV_ACTION_VIEW_AUDIT_LOG] = "VIEW_AUDIT_LOG",
  [NEW_SRV_ACTION_UPDATE_STANDINGS_2] = "UPDATE_STANDINGS_2",
  [NEW_SRV_ACTION_RESET_2] = "RESET_2",
  [NEW_SRV_ACTION_GENERATE_PASSWORDS_2] = "GENERATE_PASSWORDS_2",
  [NEW_SRV_ACTION_CLEAR_PASSWORDS_2] = "CLEAR_PASSWORDS_2",
  [NEW_SRV_ACTION_GENERATE_REG_PASSWORDS_2] = "GENERATE_REG_PASSWORDS_2",
  [NEW_SRV_ACTION_VIEW_CNTS_PWDS] = "VIEW_CNTS_PWDS",
  [NEW_SRV_ACTION_VIEW_REG_PWDS] = "VIEW_REG_PWDS",
  [NEW_SRV_ACTION_TOGGLE_VISIBILITY] = "TOGGLE_VISIBILITY",
  [NEW_SRV_ACTION_TOGGLE_BAN] = "TOGGLE_BAN",
  [NEW_SRV_ACTION_TOGGLE_LOCK] = "TOGGLE_LOCK",
  [NEW_SRV_ACTION_TOGGLE_INCOMPLETENESS] = "TOGGLE_INCOMPLETENESS",
  [NEW_SRV_ACTION_SET_DISQUALIFICATION] = "SET_DISQUALIFICATION",
  [NEW_SRV_ACTION_CLEAR_DISQUALIFICATION] = "CLEAR_DISQUALIFICATION",
  [NEW_SRV_ACTION_USER_CHANGE_STATUS] = "USER_CHANGE_STATUS",
  [NEW_SRV_ACTION_VIEW_USER_INFO] = "VIEW_USER_INFO",
  [NEW_SRV_ACTION_ISSUE_WARNING] = "ISSUE_WARNING",
  [NEW_SRV_ACTION_NEW_RUN_FORM] = "NEW_RUN_FORM",
  [NEW_SRV_ACTION_NEW_RUN] = "NEW_RUN",
  [NEW_SRV_ACTION_VIEW_USER_DUMP] = "VIEW_USER_DUMP",
  [NEW_SRV_ACTION_FORGOT_PASSWORD_1] = "FORGOT_PASSWORD_1",
  [NEW_SRV_ACTION_FORGOT_PASSWORD_2] = "FORGOT_PASSWORD_2",
  [NEW_SRV_ACTION_FORGOT_PASSWORD_3] = "FORGOT_PASSWORD_3",
  [NEW_SRV_ACTION_SUBMIT_APPEAL] = "SUBMIT_APPEAL",
  [NEW_SRV_ACTION_VIEW_PROBLEM_SUMMARY] = "VIEW_PROBLEM_SUMMARY",
  [NEW_SRV_ACTION_VIEW_PROBLEM_STATEMENTS] = "VIEW_PROBLEM_STATEMENTS",
  [NEW_SRV_ACTION_VIEW_PROBLEM_SUBMIT] = "VIEW_PROBLEM_SUBMIT",
  [NEW_SRV_ACTION_VIEW_SUBMISSIONS] = "VIEW_SUBMISSIONS",
  [NEW_SRV_ACTION_VIEW_CLAR_SUBMIT] = "VIEW_CLAR_SUBMIT",
  [NEW_SRV_ACTION_VIEW_CLARS] = "VIEW_CLARS",
  [NEW_SRV_ACTION_VIEW_SETTINGS] = "VIEW_SETTINGS",
  [NEW_SRV_ACTION_VIRTUAL_START] = "VIRTUAL_START",
  [NEW_SRV_ACTION_VIRTUAL_STOP] = "VIRTUAL_STOP",
  [NEW_SRV_ACTION_VIEW_USER_REPORT] = "VIEW_USER_REPORT",
  [NEW_SRV_ACTION_DOWNLOAD_ARCHIVE_1] = "DOWNLOAD_ARCHIVE_1",
  [NEW_SRV_ACTION_DOWNLOAD_ARCHIVE_2] = "DOWNLOAD_ARCHIVE_2",
  [NEW_SRV_ACTION_UPLOAD_RUNLOG_CSV_1] = "UPLOAD_RUNLOG_CSV_1",
  [NEW_SRV_ACTION_UPLOAD_RUNLOG_CSV_2] = "UPLOAD_RUNLOG_CSV_2",
  [NEW_SRV_ACTION_VIEW_RUNS_DUMP] = "VIEW_RUNS_DUMP",
  [NEW_SRV_ACTION_EXPORT_XML_RUNS] = "EXPORT_XML_RUNS",
  [NEW_SRV_ACTION_WRITE_XML_RUNS] = "WRITE_XML_RUNS",
  [NEW_SRV_ACTION_WRITE_XML_RUNS_WITH_SRC] = "WRITE_XML_RUNS_WITH_SRC",
  [NEW_SRV_ACTION_UPLOAD_RUNLOG_XML_1] = "UPLOAD_RUNLOG_XML_1",
  [NEW_SRV_ACTION_UPLOAD_RUNLOG_XML_2] = "UPLOAD_RUNLOG_XML_2",
  [NEW_SRV_ACTION_LOGIN] = "LOGIN",
  [NEW_SRV_ACTION_DUMP_PROBLEMS] = "DUMP_PROBLEMS",
  [NEW_SRV_ACTION_DUMP_LANGUAGES] = "DUMP_LANGUAGES",
  [NEW_SRV_ACTION_SOFT_UPDATE_STANDINGS] = "SOFT_UPDATE_STANDINGS",
  [NEW_SRV_ACTION_HAS_TRANSIENT_RUNS] = "HAS_TRANSIENT_RUNS",
  [NEW_SRV_ACTION_DUMP_RUN_STATUS] = "DUMP_RUN_STATUS",
  [NEW_SRV_ACTION_DUMP_SOURCE] = "DUMP_SOURCE",
  [NEW_SRV_ACTION_DUMP_CLAR] = "DUMP_CLAR",
  [NEW_SRV_ACTION_GET_CONTEST_NAME] = "GET_CONTEST_NAME",
  [NEW_SRV_ACTION_GET_CONTEST_TYPE] = "GET_CONTEST_TYPE",
  [NEW_SRV_ACTION_GET_CONTEST_STATUS] = "GET_CONTEST_STATUS",
  [NEW_SRV_ACTION_GET_CONTEST_SCHED] = "GET_CONTEST_SCHED",
  [NEW_SRV_ACTION_GET_CONTEST_DURATION] = "GET_CONTEST_DURATION",
  [NEW_SRV_ACTION_GET_CONTEST_DURATION] = "GET_CONTEST_DESCRIPTION",
  [NEW_SRV_ACTION_DUMP_MASTER_RUNS] = "DUMP_MASTER_RUNS",
  [NEW_SRV_ACTION_DUMP_REPORT] = "DUMP_REPORT",
  [NEW_SRV_ACTION_FULL_UPLOAD_RUNLOG_XML] = "FULL_UPLOAD_RUNLOG_XML",
  [NEW_SRV_ACTION_JSON_USER_STATE] = "JSON_USER_STATE",
  [NEW_SRV_ACTION_VIEW_STARTSTOP] = "VIEW_STARTSTOP",
  [NEW_SRV_ACTION_CLEAR_DISPLAYED_1] = "CLEAR_DISPLAYED_1",
  [NEW_SRV_ACTION_CLEAR_DISPLAYED_2] = "CLEAR_DISPLAYED_2",
  [NEW_SRV_ACTION_IGNORE_DISPLAYED_1] = "IGNORE_DISPLAYED_1",
  [NEW_SRV_ACTION_IGNORE_DISPLAYED_2] = "IGNORE_DISPLAYED_2",
  [NEW_SRV_ACTION_DISQUALIFY_DISPLAYED_1] = "DISQUALIFY_DISPLAYED_1",
  [NEW_SRV_ACTION_DISQUALIFY_DISPLAYED_2] = "DISQUALIFY_DISPLAYED_2",
  [NEW_SRV_ACTION_UPDATE_ANSWER] = "UPDATE_ANSWER",
  [NEW_SRV_ACTION_UPSOLVING_CONFIG_1] = "UPSOLVING_CONFIG_1",
  [NEW_SRV_ACTION_UPSOLVING_CONFIG_2] = "UPSOLVING_CONFIG_2",
  [NEW_SRV_ACTION_UPSOLVING_CONFIG_3] = "UPSOLVING_CONFIG_3",
  [NEW_SRV_ACTION_UPSOLVING_CONFIG_4] = "UPSOLVING_CONFIG_4",
  [NEW_SRV_ACTION_EXAMINERS_PAGE] = "EXAMINERS_PAGE",
  [NEW_SRV_ACTION_ASSIGN_CHIEF_EXAMINER] = "ASSIGN_CHIEF_EXAMINER",
  [NEW_SRV_ACTION_ASSIGN_EXAMINER] = "ASSIGN_EXAMINER",
  [NEW_SRV_ACTION_UNASSIGN_EXAMINER] = "UNASSIGN_EXAMINER",
  [NEW_SRV_ACTION_GET_FILE] = "GET_FILE",
  [NEW_SRV_ACTION_PRINT_USER_PROTOCOL] = "PRINT_USER_PROTOCOL",
  [NEW_SRV_ACTION_PRINT_USER_FULL_PROTOCOL] = "PRINT_USER_FULL_PROTOCOL",
  [NEW_SRV_ACTION_PRINT_UFC_PROTOCOL] = "PRINT_UFC_PROTOCOL",
  [NEW_SRV_ACTION_FORCE_START_VIRTUAL] = "FORCE_START_VIRTUAL",
  [NEW_SRV_ACTION_PRINT_SELECTED_USER_PROTOCOL] = "PRINT_SELECTED_USER_PROTOCOL",
  [NEW_SRV_ACTION_PRINT_SELECTED_USER_FULL_PROTOCOL] = "PRINT_SELECTED_USER_FULL_PROTOCOL",
  [NEW_SRV_ACTION_PRINT_SELECTED_UFC_PROTOCOL] = "PRINT_SELECTED_UFC_PROTOCOL",
  [NEW_SRV_ACTION_PRINT_PROBLEM_PROTOCOL] = "PRINT_PROBLEM_PROTOCOL",
  [NEW_SRV_ACTION_ASSIGN_CYPHERS_1] = "ASSIGN_CYPHERS_1",
  [NEW_SRV_ACTION_ASSIGN_CYPHERS_2] = "ASSIGN_CYPHERS_2",
  [NEW_SRV_ACTION_VIEW_EXAM_INFO] = "VIEW_EXAM_INFO",
  [NEW_SRV_ACTION_PRIV_SUBMIT_PAGE] = "PRIV_SUBMIT_PAGE",
  [NEW_SRV_ACTION_REG_CREATE_ACCOUNT_PAGE] = "REG_CREATE_ACCOUNT_PAGE",
  [NEW_SRV_ACTION_REG_CREATE_ACCOUNT] = "REG_CREATE_ACCOUNT",
  [NEW_SRV_ACTION_REG_ACCOUNT_CREATED_PAGE] = "REG_ACCOUNT_CREATED_PAGE",
  [NEW_SRV_ACTION_REG_LOGIN_PAGE] = "REG_LOGIN_PAGE",
  [NEW_SRV_ACTION_REG_LOGIN] = "REG_LOGIN",
  [NEW_SRV_ACTION_REG_VIEW_GENERAL] = "REG_VIEW_GENERAL",
  [NEW_SRV_ACTION_REG_VIEW_CONTESTANTS] = "REG_VIEW_CONTESTANTS",
  [NEW_SRV_ACTION_REG_VIEW_RESERVES] = "REG_VIEW_RESERVES",
  [NEW_SRV_ACTION_REG_VIEW_COACHES] = "REG_VIEW_COACHES",
  [NEW_SRV_ACTION_REG_VIEW_ADVISORS] = "REG_VIEW_ADVISORS",
  [NEW_SRV_ACTION_REG_VIEW_GUESTS] = "REG_VIEW_GUESTS",
  [NEW_SRV_ACTION_REG_ADD_MEMBER_PAGE] = "REG_ADD_MEMBER_PAGE",
  [NEW_SRV_ACTION_REG_EDIT_GENERAL_PAGE] = "REG_EDIT_GENERAL_PAGE",
  [NEW_SRV_ACTION_REG_EDIT_MEMBER_PAGE] = "REG_EDIT_MEMBER_PAGE",
  [NEW_SRV_ACTION_REG_MOVE_MEMBER] = "REG_MOVE_MEMBER",
  [NEW_SRV_ACTION_REG_REMOVE_MEMBER] = "REG_REMOVE_MEMBER",
  [NEW_SRV_ACTION_REG_SUBMIT_GENERAL_EDITING] = "REG_SUBMIT_GENERAL_EDITING",
  [NEW_SRV_ACTION_REG_CANCEL_GENERAL_EDITING] = "REG_CANCEL_GENERAL_EDITING",
  [NEW_SRV_ACTION_REG_SUBMIT_MEMBER_EDITING] = "REG_SUBMIT_MEMBER_EDITING",
  [NEW_SRV_ACTION_REG_CANCEL_MEMBER_EDITING] = "REG_CANCEL_MEMBER_EDITING",
  [NEW_SRV_ACTION_REG_REGISTER] = "REG_REGISTER",
  [NEW_SRV_ACTION_REG_DATA_EDIT] = "REG_DATA_EDIT",
  [NEW_SRV_ACTION_PRIO_FORM] = "PRIO_FORM",
  [NEW_SRV_ACTION_SET_PRIORITIES] = "SET_PRIORITIES",
  [NEW_SRV_ACTION_PRIV_SUBMIT_RUN_COMMENT_AND_IGNORE] = "PRIV_SUBMIT_RUN_COMMENT_AND_IGNORE",
  [NEW_SRV_ACTION_VIEW_USER_IPS] = "VIEW_USER_IPS",
  [NEW_SRV_ACTION_VIEW_IP_USERS] = "VIEW_IP_USERS",
  [NEW_SRV_ACTION_CHANGE_FINISH_TIME] = "CHANGE_FINISH_TIME",
  [NEW_SRV_ACTION_PRIV_SUBMIT_RUN_COMMENT_AND_OK] = "PRIV_SUBMIT_RUN_COMMENT_AND_OK",
  [NEW_SRV_ACTION_PRIV_SUBMIT_RUN_JUST_IGNORE] = "PRIV_SUBMIT_RUN_JUST_IGNORE",
  [NEW_SRV_ACTION_PRIV_SUBMIT_RUN_JUST_OK] = "PRIV_SUBMIT_RUN_JUST_OK",
  [NEW_SRV_ACTION_PRIV_SET_RUN_REJECTED] = "PRIV_SET_RUN_REJECTED",
  [NEW_SRV_ACTION_VIEW_TESTING_QUEUE] = "VIEW_TESTING_QUEUE",
  [NEW_SRV_ACTION_TESTING_DELETE] = "TESTING_DELETE",
  [NEW_SRV_ACTION_TESTING_UP] = "TESTING_UP",
  [NEW_SRV_ACTION_TESTING_DOWN] = "TESTING_DOWN",
  [NEW_SRV_ACTION_TESTING_DELETE_ALL] = "TESTING_DELETE_ALL",
  [NEW_SRV_ACTION_TESTING_UP_ALL] = "TESTING_UP_ALL",
  [NEW_SRV_ACTION_TESTING_DOWN_ALL] = "TESTING_DOWN_ALL",
  [NEW_SRV_ACTION_MARK_DISPLAYED_2] = "MARK_DISPLAYED_2",
  [NEW_SRV_ACTION_UNMARK_DISPLAYED_2] = "UNMARK_DISPLAYED_2",
  [NEW_SRV_ACTION_SET_STAND_FILTER] = "SET_STAND_FILTER",
  [NEW_SRV_ACTION_RESET_STAND_FILTER] = "RESET_STAND_FILTER",
  [NEW_SRV_ACTION_ADMIN_CONTEST_SETTINGS] = "ADMIN_CONTEST_SETTINGS",
  [NEW_SRV_ACTION_ADMIN_CHANGE_ONLINE_VIEW_SOURCE] = "ADMIN_CHANGE_ONLINE_VIEW_SOURCE",
  [NEW_SRV_ACTION_ADMIN_CHANGE_ONLINE_VIEW_REPORT] = "ADMIN_CHANGE_ONLINE_VIEW_REPORT",
  [NEW_SRV_ACTION_ADMIN_CHANGE_ONLINE_VIEW_JUDGE_SCORE] = "ADMIN_CHANGE_ONLINE_VIEW_JUDGE_SCORE",
  [NEW_SRV_ACTION_ADMIN_CHANGE_ONLINE_FINAL_VISIBILITY] = "ADMIN_CHANGE_ONLINE_FINAL_VISIBILITY",
  [NEW_SRV_ACTION_RELOAD_SERVER_2] = "RELOAD_SERVER_2",
  [NEW_SRV_ACTION_CHANGE_RUN_FIELDS] = "CHANGE_RUN_FIELDS",
  [NEW_SRV_ACTION_PRIV_EDIT_CLAR_PAGE] = "PRIV_EDIT_CLAR_PAGE",
  [NEW_SRV_ACTION_PRIV_EDIT_CLAR_ACTION] = "PRIV_EDIT_CLAR_ACTION",
  [NEW_SRV_ACTION_PRIV_EDIT_RUN_PAGE] = "PRIV_EDIT_RUN_PAGE",
  [NEW_SRV_ACTION_PRIV_EDIT_RUN_ACTION] = "PRIV_EDIT_RUN_ACTION",
  [NEW_SRV_ACTION_PING] = "PING",
  [NEW_SRV_ACTION_SUBMIT_RUN_BATCH] = "SUBMIT_RUN_BATCH",
};

static void
parse_cookie(struct http_request_info *phr)
{
  const unsigned char *cookies = ns_getenv(phr, "HTTP_COOKIE");
  if (!cookies) return;
  const unsigned char *s = cookies;
  ej_cookie_t client_key = 0;
  while (1) {
    while (isspace(*s)) ++s;
    if (strncmp(s, "EJSID=", 6) != 0) {
      while (*s && *s != ';') ++s;
      if (!*s) return;
      ++s;
      continue;
    }
    int n = 0;
    if (sscanf(s + 6, "%llx%n", &client_key, &n) == 1) {
      s += 6 + n;
      if (!*s || isspace(*s) || *s == ';') {
        phr->client_key = client_key;
        return;
      }
    }
    phr->client_key = 0;
    return;
  }
}

void
ns_handle_http_request(struct server_framework_state *state,
                       struct client_state *p,
                       FILE *fout,
                       struct http_request_info *phr)
{
  const unsigned char *script_filename = 0;
  path_t last_name;
  const unsigned char *http_host;
  const unsigned char *script_name;
  const unsigned char *protocol = "http";
  const unsigned char *remote_addr;
  const unsigned char *s;
  path_t self_url;
  int r, n, orig_locale_id = -1;

  // make a self-referencing URL
  if (ns_getenv(phr, "SSL_PROTOCOL") || ns_getenv(phr, "HTTPS")) {
    phr->ssl_flag = 1;
    protocol = "https";
  }
  if (!(http_host = ns_getenv(phr, "HTTP_HOST"))) http_host = "localhost";
  if (!(script_name = ns_getenv(phr, "SCRIPT_NAME")))
    script_name = "/cgi-bin/new-client";
  phr->script_name = script_name;
  snprintf(self_url, sizeof(self_url), "%s://%s%s", protocol,
           http_host, script_name);
  phr->self_url = self_url;

  if (ns_cgi_param(phr, "json", &s) > 0) {
    phr->json_reply = 1;
  }

  // parse the client IP address
  if (!(remote_addr = ns_getenv(phr, "REMOTE_ADDR")))
    return ns_html_err_inv_param(fout, phr, 0, "REMOTE_ADDR does not exist");
  if (!strcmp(remote_addr, "::1")) remote_addr = "127.0.0.1";
  if (xml_parse_ipv6(NULL, 0, 0, 0, remote_addr, &phr->ip) < 0)
    return ns_html_err_inv_param(fout, phr, 0, "cannot parse REMOTE_ADDR");

  parse_cookie(phr);

  // parse the contest_id
  if ((r = ns_cgi_param(phr, "contest_id", &s)) < 0)
    return ns_html_err_inv_param(fout, phr, 0, "cannot parse contest_id");
  if (r > 0) {
    if (sscanf(s, "%d%n", &phr->contest_id, &n) != 1
        || s[n] || phr->contest_id <= 0)
      return ns_html_err_inv_param(fout, phr, 0, "cannot parse contest_id");
  }

  if (ns_cgi_param(phr, "plain_text", &s) > 0) {
    phr->plain_text = 1;
  }

  // parse the session_id
  if ((r = ns_cgi_param(phr, "SID", &s)) < 0)
    return ns_html_err_inv_param(fout, phr, 0, "cannot parse SID");
  if (r > 0) {
    if (sscanf(s, "%llx%n", &phr->session_id, &n) != 1
        || s[n] || !phr->session_id)
      return ns_html_err_inv_param(fout, phr, 0, "cannot parse SID");
  }

  // parse the locale_id
  if ((r = ns_cgi_param(phr, "locale_id", &s)) < 0)
    return ns_html_err_inv_param(fout, phr, 0, "cannot parse locale_id");
  if (r > 0) {
    if (sscanf(s, "%d%n", &phr->locale_id, &n) != 1 || s[n]
        || phr->locale_id < 0)
      return ns_html_err_inv_param(fout, phr, 0, "cannot parse locale_id");
    orig_locale_id = phr->locale_id;
  }

  // parse the action
  if ((s = ns_cgi_nname(phr, "action_", 7))) {
    if (sscanf(s, "action_%d%n", &phr->action, &n) != 1 || s[n]
        || phr->action <= 0)
      return ns_html_err_inv_param(fout, phr, 0, "cannot parse action");
  } else if ((r = ns_cgi_param(phr, "action", &s)) < 0) {
    return ns_html_err_inv_param(fout, phr, 0, "cannot parse action");
  } else if (r > 0) {
    if (sscanf(s, "%d%n", &phr->action, &n) != 1 || s[n] || phr->action <= 0) {
      for (r = 0; r < NEW_SRV_ACTION_LAST; ++r)
        if (symbolic_action_table[r]
            && !strcasecmp(symbolic_action_table[r], s))
          break;
      if (r == NEW_SRV_ACTION_LAST)
        return ns_html_err_inv_param(fout, phr, 0, "cannot parse action");
      phr->action = r;
    }
  }

  // check how we've been called
  script_filename = ns_getenv(phr, "SCRIPT_FILENAME");
  if (!script_filename && phr->arg_num > 0) script_filename = phr->args[0];
  if (!script_filename)
    return ns_html_err_inv_param(fout, phr, 0, "cannot get script filename");

  os_rGetLastname(script_filename, last_name, sizeof(last_name));

#if defined CGI_PROG_SUFFIX
  {
    static const unsigned char cgi_prog_suffix_str[] = CGI_PROG_SUFFIX;
    if (sizeof(cgi_prog_suffix_str) > 1) {
      int ll;
      if ((ll = strlen(last_name)) >= sizeof(cgi_prog_suffix_str)
          && !strcmp(last_name + ll - (sizeof(cgi_prog_suffix_str) - 1),
                     cgi_prog_suffix_str)) {
        last_name[ll - (sizeof(cgi_prog_suffix_str) - 1)] = 0;
      }
    }
  }
#endif /* CGI_PROG_SUFFIX */

  if (!strcmp(last_name, "priv-client"))
    privileged_entry_point(fout, phr);
  else if (!strcmp(last_name, "new-master") || !strcmp(last_name, "master")) {
    phr->role = USER_ROLE_ADMIN;
    privileged_entry_point(fout, phr);
  } else if (!strcmp(last_name, "new-judge") || !strcmp(last_name, "judge")) {
    phr->role = USER_ROLE_JUDGE;
    privileged_entry_point(fout, phr);
  } else if (!strcmp(last_name, "new-register")
             || !strcmp(last_name, "register")) {
    // FIXME: temporary hack
    phr->locale_id = orig_locale_id;
    ns_register_pages(fout, phr);
  } else if (!strcmp(last_name, "ejudge-contests-cmd")) {
    phr->protocol_reply = new_server_cmd_handler(fout, phr);
  } else
    unprivileged_entry_point(fout, phr, orig_locale_id);
}

/*
 * Local variables:
 *  compile-command: "make"
 * End:
 */

/* -*- mode: c -*- */
/* $Id: nsdb_plugin_files.c 6162 2011-03-27 07:07:27Z cher $ */

/* Copyright (C) 2006-2011 Alexander Chernov <cher@ejudge.ru> */

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

#include "nsdb_plugin.h"
#include "expat_iface.h"
#include "xml_utils.h"
#include "errlog.h"
#include "pathutl.h"
#include "new-server.h"

#include "reuse_xalloc.h"
#include "reuse_logger.h"
#include "reuse_osdeps.h"

#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>

static void *init_func(const struct ejudge_cfg *config);
static int parse_func(void *data, const struct ejudge_cfg *config, struct xml_tree *tree);
static int open_func(void *data);
static int close_func(void *data);
static int check_func(void *data);
static int create_func(void *data);
static int check_role_func(void *, int, int, int);
static int_iterator_t get_contest_user_id_iterator_func(void *, int);
static int get_priv_role_mask_by_iter(void *, int_iterator_t, unsigned int*);
static int add_role_func(void *, int, int, int);
static int del_role_func(void *, int, int, int);
static int priv_remove_user_func(void *, int, int);
static int assign_examiner_func(void *, int, int, int);
static int assign_chief_examiner_func(void *, int, int, int, int);
static int remove_examiner_func(void *, int, int, int);
static int get_examiner_role_func(void *, int, int, int);
static int find_chief_examiner_func(void *, int, int);
static int_iterator_t get_examiner_user_id_iterator_func(void *, int, int);
static int get_examiner_count_func(void *, int, int);

struct nsdb_plugin_iface nsdb_plugin_files =
{
  {
    sizeof (struct nsdb_plugin_iface),
    EJUDGE_PLUGIN_IFACE_VERSION,
    "nsdb",
    "nsdb_xml",
  },

  NSDB_PLUGIN_IFACE_VERSION,

  init_func,
  parse_func,
  open_func,
  close_func,
  check_func,
  create_func,

  check_role_func,
  get_contest_user_id_iterator_func,
  get_priv_role_mask_by_iter,
  add_role_func,
  del_role_func,
  priv_remove_user_func,

  assign_examiner_func,
  assign_chief_examiner_func,
  remove_examiner_func,
  get_examiner_role_func,
  find_chief_examiner_func,
  get_examiner_user_id_iterator_func,
  get_examiner_count_func,
};

struct user_priv_header
{
  unsigned char signature[12];
  unsigned char byte_order;
  unsigned char version;
  char pad[2];
};
struct user_priv_entry
{
  int user_id;
  int contest_id;
  unsigned int priv_bits;
  char pad[4];
};
struct user_priv_table
{
  struct user_priv_header header;
  size_t a, u;
  struct user_priv_entry *v;
  int header_dirty, data_dirty;
  int fd;
};

struct prob_assignment_header
{
  unsigned char signature[12];
  unsigned char byte_order;
  unsigned char version;
  char pad[2];
};
struct prob_assignment_entry
{
  int user_id;
  int contest_id;
  int prob_id;
  int role;                     /*USER_ROLE_EXAMINER, USER_ROLE_CHIEF_EXAMINER*/
};
struct prob_assignment_table
{
  struct prob_assignment_header header;
  size_t a, u;
  struct prob_assignment_entry *v;
  int header_dirty, data_dirty, error_flag;
  int fd;
};

static int user_priv_create(struct user_priv_table *pt, const unsigned char *);
static int user_priv_load(struct user_priv_table *pt, const unsigned char *dir);
static int user_priv_flush(struct user_priv_table *pt);

struct nsdb_files_state
{
  unsigned char *data_dir;

  struct user_priv_table user_priv;
  struct prob_assignment_table prob_asgn;
};

static void *
init_func(const struct ejudge_cfg *config)
{
  struct nsdb_files_state *state;

  XCALLOC(state, 1);

  state->prob_asgn.fd = -1;

  return (void*) state;
}

static int
parse_func(void *data, const struct ejudge_cfg *config, struct xml_tree *tree)
{
  struct nsdb_files_state *state = (struct nsdb_files_state*) data;
  struct xml_tree *p;

  if (!tree) {
    err("configuration for files plugin is not specified");
    return -1;
  }
  ASSERT(tree->tag == xml_err_spec->default_elem);
  ASSERT(!strcmp(tree->name[0], "config"));

  if (xml_empty_text(tree) < 0) return -1;
  if (tree->first) return xml_err_attrs(tree);

  for (p = tree->first_down; p; p = p->right) {
    ASSERT(p->tag == xml_err_spec->default_elem);
    if (!strcmp(p->name[0], "data_dir")) {
      if (xml_leaf_elem(p, &state->data_dir, 1, 0) < 0) return -1;
    } else {
      return xml_err_elem_not_allowed(p);
    }
  }

  if (!state->data_dir) return xml_err_elem_undefined_s(tree, "data_dir");

  return 0;
}

static int
open_func(void *data)
{
  return 0;
}

static void prob_asgn_close(struct prob_assignment_table *pt);

static int
close_func(void *data)
{
  struct nsdb_files_state *state = (struct nsdb_files_state*) data;

  user_priv_flush(&state->user_priv);
  prob_asgn_close(&state->prob_asgn);
  if (state->user_priv.fd >= 0) close(state->user_priv.fd);
  state->user_priv.fd = -1;
  return 0;
}

static int
check_func(void *data)
{
  struct nsdb_files_state *state = (struct nsdb_files_state*) data;
  struct stat stb;
  
  if (stat(state->data_dir, &stb) < 0) {
    err("data_dir `%s' does not exist. create it with --create",
        state->data_dir);
    return 0;
  }
  if (!S_ISDIR(stb.st_mode)) {
    err("`%s' is not a directory", state->data_dir);
    return -1;
  }

  if (user_priv_load(&state->user_priv, state->data_dir) < 0)
    return -1;
  
  return 1;
}

static int
create_func(void *data)
{
  struct nsdb_files_state *state = (struct nsdb_files_state*) data;

  if (mkdir(state->data_dir, 0770) < 0 && errno != EEXIST) {
    err("mkdir failed on `%s': %s", state->data_dir, os_ErrorMsg());
    return -1;
  }

  if (user_priv_create(&state->user_priv, state->data_dir) < 0)
    return -1;

  return 0;
}

static ssize_t
full_read(int fd, void *buf, size_t size)
{
  unsigned char *p = (unsigned char*) buf;
  int r;

  while (size > 0) {
    if ((r = read(fd, p, size)) < 0) return r;
    if (!r) return p - (unsigned char*) buf;
    p += r;
    size -= r;
  }
  return p - (unsigned char*) buf;
}
static ssize_t
full_write(int fd, const void *buf, size_t size)
{
  const unsigned char *p = (const unsigned char *) buf;
  int r;

  while (size > 0) {
    if ((r = write(fd, p, size)) <= 0) return r;
    p += r;
    size -= r;
  }
  return p - (const unsigned char*) buf;
}

static const unsigned char user_priv_signature[12] = "Ej.userpriv";

static int
user_priv_load(struct user_priv_table *pt, const unsigned char *dir)
{
  path_t path;
  struct stat stb;
  int n;

  snprintf(path, sizeof(path), "%s/user_priv.dat", dir);
  if ((pt->fd = open(path, O_RDWR | O_CREAT, 0600)) < 0) {
    err("cannot open %s: %s", path, os_ErrorMsg());
    return -1;
  }
  fstat(pt->fd, &stb);
  if (!stb.st_size) {
    // new file
    memcpy(&pt->header.signature, user_priv_signature, sizeof(pt->header.signature));
    pt->header.byte_order = 0;
    pt->header.version = 1;
    pt->header_dirty = 1;
    return 0;
  }

  if (stb.st_size < sizeof(struct user_priv_header)) {
    err("invalid size of %s", path);
    return -1;
  }
  if (full_read(pt->fd, &pt->header, sizeof(pt->header)) != sizeof(pt->header)){
    err("cannot read header from %s", path);
    return -1;
  }
  if (memcmp(pt->header.signature, user_priv_signature, sizeof(user_priv_signature)) != 0) {
    err("invalid file format of %s", path);
    return -1;
  }
  if (pt->header.byte_order != 0) {
    err("cannot handle byte_order %d in %s", pt->header.byte_order, path);
    return -1;
  }
  if (pt->header.version != 1) {
    err("cannot handle version %d in %s", pt->header.version, path);
    return -1;
  }

  if ((stb.st_size - sizeof(struct user_priv_header)) % sizeof(struct user_priv_entry) != 0) {
    err("invalid file size of %s", path);
    return -1;
  }
  n = (stb.st_size - sizeof(struct user_priv_header)) / sizeof(struct user_priv_entry);
  pt->a = 16;
  while (pt->a < n) pt->a *= 2;
  XCALLOC(pt->v, pt->a);
  if (full_read(pt->fd, pt->v, n * sizeof(struct user_priv_entry)) != n * sizeof(struct user_priv_entry)) {
    err("cannot read data from %s", path);
    return -1;
  }
  pt->u = n;
  return 0;
}

static int
user_priv_create(struct user_priv_table *pt, const unsigned char *dir)
{
  path_t path;

  snprintf(path, sizeof(path), "%s/user_priv.dat", dir);
  if ((pt->fd = open(path, O_RDWR | O_CREAT, 0600)) < 0) {
    err("cannot open %s: %s", path, os_ErrorMsg());
    return -1;
  }
  if (ftruncate(pt->fd, 0) < 0) {
    err("ftruncate failed: %s", os_ErrorMsg());
    return -1;
  }
  memcpy(&pt->header.signature, user_priv_signature, sizeof(pt->header.signature));
  pt->header.byte_order = 0;
  pt->header.version = 1;
  pt->header_dirty = 1;
  return 0;
}

static int
user_priv_flush(struct user_priv_table *pt)
{
  if (pt->header_dirty) {
    if (lseek(pt->fd, 0, SEEK_SET) < 0) {
      err("lseek failed: %s", os_ErrorMsg());
      return -1;
    }
    if (full_write(pt->fd, &pt->header, sizeof(pt->header)) != sizeof(pt->header)) {
      err("write failed: %s", os_ErrorMsg());
      return -1;
    }
    pt->header_dirty = 0;
  }
  if (pt->data_dirty) {
    if (lseek(pt->fd, sizeof(struct user_priv_header), SEEK_SET) < 0) {
      err("lseek failed: %s", os_ErrorMsg());
      return -1;
    }
    if (full_write(pt->fd, pt->v, pt->u * sizeof(struct user_priv_entry)) != pt->u * sizeof(struct user_priv_entry)) {
      err("write failed: %s", os_ErrorMsg());
      return -1;
    }
    if (ftruncate(pt->fd, sizeof(struct user_priv_header) + pt->u * sizeof(struct user_priv_entry)) < 0) {
      err("ftruncate failed: %s", os_ErrorMsg());
      return -1;
    }
    pt->data_dirty = 0;
  }
  return 0;
}

static int
check_role_func(void *data, int user_id, int contest_id, int role)
{
  struct nsdb_files_state *state = (struct nsdb_files_state*) data;
  unsigned int b;
  int i;

  if (user_id <= 0) return -1;
  if (contest_id <= 0) return -1;
  if (role <= USER_ROLE_CONTESTANT || role >= USER_ROLE_ADMIN) return -1;
  b = 1 << role;

  for (i = 0; i < state->user_priv.u; i++)
    if (state->user_priv.v[i].user_id == user_id
        && state->user_priv.v[i].contest_id == contest_id) {
      if ((b & state->user_priv.v[i].priv_bits)) return 0;
      return -1;
    }
  return -1;
}

struct contest_user_id_iterator
{
  struct int_iterator b;

  struct nsdb_files_state *state;
  int contest_id;
  int cur_idx;
};

static int
contest_user_id_iterator_has_next(int_iterator_t data)
{
  struct contest_user_id_iterator *iter = (struct contest_user_id_iterator*) data;
  struct user_priv_table *ppriv = &iter->state->user_priv;
  while (iter->cur_idx < ppriv->u && iter->contest_id != ppriv->v[iter->cur_idx].contest_id) iter->cur_idx++;
  return (iter->cur_idx < ppriv->u);
}
static int
contest_user_id_iterator_get(int_iterator_t data)
{
  struct contest_user_id_iterator *iter = (struct contest_user_id_iterator*) data;
  struct user_priv_table *ppriv = &iter->state->user_priv;
  while (iter->cur_idx < ppriv->u && iter->contest_id != ppriv->v[iter->cur_idx].contest_id) iter->cur_idx++;
  ASSERT(iter->cur_idx < ppriv->u);
  return ppriv->v[iter->cur_idx].user_id;
}
static void
contest_user_id_iterator_next(int_iterator_t data)
{
  struct contest_user_id_iterator *iter = (struct contest_user_id_iterator*) data;
  struct user_priv_table *ppriv = &iter->state->user_priv;
  if (iter->cur_idx < ppriv->u) iter->cur_idx++;
  while (iter->cur_idx < ppriv->u && iter->contest_id != ppriv->v[iter->cur_idx].contest_id) iter->cur_idx++;
}
static void
contest_user_id_iterator_destroy(int_iterator_t data)
{
  struct contest_user_id_iterator *iter = (struct contest_user_id_iterator*) data;
  xfree(iter);
}

static struct int_iterator contest_user_id_iterator_funcs =
{
  contest_user_id_iterator_has_next,
  contest_user_id_iterator_get,
  contest_user_id_iterator_next,
  contest_user_id_iterator_destroy,
};
static int_iterator_t
get_contest_user_id_iterator_func(void *data, int contest_id)
{
  struct nsdb_files_state *state = (struct nsdb_files_state*) data;
  struct contest_user_id_iterator *iter;

  XCALLOC(iter, 1);
  iter->b = contest_user_id_iterator_funcs;
  iter->state = state;
  iter->contest_id = contest_id;
  iter->cur_idx = 0;
  return (int_iterator_t) iter;
}

static int
get_priv_role_mask_by_iter(void *data, int_iterator_t g_iter,
                           unsigned int *p_mask)
{
  struct contest_user_id_iterator *iter = (struct contest_user_id_iterator*) g_iter;
  struct user_priv_table *ppriv = &iter->state->user_priv;

  if (iter->cur_idx >= ppriv->u) return -1;
  if (p_mask) *p_mask = ppriv->v[iter->cur_idx].priv_bits;
  return 0;
}

static int
add_role_func(void *data, int user_id, int contest_id, int role)
{
  struct nsdb_files_state *state = (struct nsdb_files_state*) data;
  struct user_priv_table *ppriv = &state->user_priv;
  int i;
  unsigned int bit;

  if (user_id <= 0) return -1;
  if (contest_id <= 0) return -1;
  if (role < USER_ROLE_OBSERVER || role > USER_ROLE_COORDINATOR) return -1;
  bit = (1 << role);

  for (i = 0; i < ppriv->u; i++) {
    if (ppriv->v[i].user_id == user_id && ppriv->v[i].contest_id == contest_id)
      break;
  }
  if (i >= ppriv->u) {
    if (!ppriv->a) ppriv->a = 8;
    ppriv->v = xrealloc(ppriv->v, sizeof(ppriv->v[0]) * (ppriv->a *= 2));
    ppriv->data_dirty = 1;
    memset(&ppriv->v[i], 0, sizeof(ppriv->v[i]));
    ppriv->v[i].user_id = user_id;
    ppriv->v[i].contest_id = contest_id;
    ppriv->u++;
  }
  if (!(ppriv->v[i].priv_bits & bit)) {
    ppriv->v[i].priv_bits |= bit;
    ppriv->data_dirty = 1;
    return 1;
  }
  return 0;
}

static int
del_role_func(void *data, int user_id, int contest_id, int role)
{
  struct nsdb_files_state *state = (struct nsdb_files_state*) data;
  struct user_priv_table *ppriv = &state->user_priv;
  unsigned int bit;
  int i;

  if (user_id <= 0) return -1;
  if (contest_id <= 0) return -1;
  if (role < USER_ROLE_OBSERVER || role > USER_ROLE_COORDINATOR) return -1;
  bit = (1 << role);

  for (i = 0; i < ppriv->u; i++) {
    if (ppriv->v[i].user_id == user_id && ppriv->v[i].contest_id == contest_id)
      break;
  }
  if (i >= ppriv->u) return -1;
  if ((ppriv->v[i].priv_bits & bit)) {
    ppriv->v[i].priv_bits &= ~bit;
    ppriv->data_dirty = 1;
    return 1;
  }
  return 0;
}

static int
priv_remove_user_func(void *data, int user_id, int contest_id)
{
  struct nsdb_files_state *state = (struct nsdb_files_state*) data;
  struct user_priv_table *ppriv = &state->user_priv;
  int i, j;

  if (user_id <= 0) return -1;
  if (contest_id <= 0) return -1;
  for (i = 0; i < ppriv->u; i++) {
    if (ppriv->v[i].user_id == user_id && ppriv->v[i].contest_id == contest_id)
      break;
  }
  if (i >= ppriv->u) return 0;

  for (j = i + 1; j < ppriv->u; j++)
    ppriv->v[j - 1] = ppriv->v[j];
  ppriv->u--;
  ppriv->data_dirty = 1;
  return 1;
}

static void
prob_asgn_flush(struct prob_assignment_table *pt)
{
  if (!pt || pt->fd < 0 || (!pt->header_dirty && !pt->data_dirty)
      || pt->error_flag)
    return;

  if (pt->header_dirty) {
    if (lseek(pt->fd, 0, SEEK_SET) < 0) {
      err("prob_asgn: lseek failed: %s", os_ErrorMsg());
      pt->error_flag = 1;
      return;
    }

    if (full_write(pt->fd, &pt->header, sizeof(pt->header)) < 0) {
      err("prob_asgn: write failed: %s", os_ErrorMsg());
      pt->error_flag = 1;
      return;
    }

    pt->header_dirty = 0;
  }

  if (pt->data_dirty) {
    if (ftruncate(pt->fd, sizeof(struct prob_assignment_header) + pt->u * sizeof(struct prob_assignment_entry)) < 0) {
      err("prob_asgn: ftruncate failed: %s", os_ErrorMsg());
      pt->error_flag = 1;
      return;
    }

    if (pt->u > 0) {
      if (lseek(pt->fd, sizeof(struct prob_assignment_header), SEEK_SET) < 0) {
        err("prob_asgn: lseek failed: %s", os_ErrorMsg());
        pt->error_flag = 1;
        return;
      }

      if (full_write(pt->fd, pt->v, sizeof(pt->v[0]) * pt->u) < 0) {
        err("prob_asgn: write failed: %s", os_ErrorMsg());
        pt->error_flag = 1;
        return;
      }
    }
    pt->data_dirty = 0;
  }
}

static void
prob_asgn_close(struct prob_assignment_table *pt)
{
  if (!pt || pt->fd < 0) return;
  prob_asgn_flush(pt);
  xfree(pt->v);
  memset(pt, 0, sizeof(*pt));
}

static const unsigned char prob_asgn_signature[12] = "Ej.probasgn";

static void
prob_asgn_do_create(struct nsdb_files_state *state)
{
  path_t fpath;
  struct prob_assignment_table *pt = &state->prob_asgn;

  memset(pt, 0, sizeof(*pt));

  if (mkdir(state->data_dir, 0770) < 0 && errno != EEXIST) {
    err("prob_asgn: mkdir failed on %s: %s", state->data_dir, os_ErrorMsg());
    pt->error_flag = 1;
    return;
  }

  snprintf(fpath, sizeof(fpath), "%s/prob_asgn.dat", state->data_dir);
  if ((pt->fd = open(fpath, O_RDWR | O_CREAT | O_TRUNC, 0600)) < 0) {
    err("prob_asgn: open failed for %s : %s", fpath, os_ErrorMsg());
    pt->error_flag = 1;
    return;
  }

  memcpy(&pt->header.signature, prob_asgn_signature,
         sizeof (pt->header.signature));
  pt->header.byte_order = 0;
  pt->header.version = 1;
  pt->header_dirty = 1;
  pt->data_dirty = 1;
  prob_asgn_flush(pt);
}

static void
prob_asgn_do_open(struct nsdb_files_state *state)
{
  path_t fpath;
  struct stat stb;
  struct prob_assignment_table *pt = &state->prob_asgn;
  unsigned char signbuf[sizeof(struct prob_assignment_header) * 2];

  ASSERT(state);

  if (pt->fd >= 0) return;
  if (pt->error_flag) return;

  snprintf(fpath, sizeof(fpath), "%s/prob_asgn.dat", state->data_dir);
  if (stat(fpath, &stb) < 0 || !stb.st_size)
    return prob_asgn_do_create(state);

  memset(pt, 0, sizeof(*pt));

  if (stb.st_size < sizeof(struct prob_assignment_header)) {
    err("prob_asgn: invalid file size %d\n", (int) stb.st_size);
    pt->error_flag = 1;
    return;
  }
  if ((pt->fd = open(fpath, O_RDWR, 0)) < 0) {
    err("prob_asgn: open failed for %s: %s", fpath, os_ErrorMsg());
    pt->error_flag = 1;
    return;
  }
  if (full_read(pt->fd, &pt->header, sizeof(pt->header)) < 0) {
    err("prob_asgn: read failed: %s", os_ErrorMsg());
    pt->error_flag = 1;
    return;
  }
  memset(signbuf, 0, sizeof(signbuf));
  memcpy(signbuf, pt->header.signature, sizeof(pt->header.signature));
  if (memcmp(signbuf, prob_asgn_signature, sizeof(prob_asgn_signature)) != 0) {
    err("prob_asgn: file signature mismatch");
    pt->error_flag = 1;
    return;
  }
  if (pt->header.version != 1) {
    err("prob_asgn: unsupported version");
    pt->error_flag = 1;
    return;
  }
  if (pt->header.byte_order != 0) {
    err("prob_asgn: unsupported byte order");
    pt->error_flag = 1;
    return;
  }

  if ((stb.st_size - sizeof(struct prob_assignment_header)) % sizeof(struct prob_assignment_entry) != 0) {
    err("prob_asgn: invalid file size %d\n", (int) stb.st_size);
    pt->error_flag = 1;
    return;
  }
  pt->u = (stb.st_size - sizeof(struct prob_assignment_header)) / sizeof(struct prob_assignment_entry);
  if (pt->u > 0) {
    pt->a = 16;
    while (pt->a < pt->u) pt->a *= 2;
    XCALLOC(pt->v, pt->a);
  }
  if (pt->u > 0) {
    if (full_read(pt->fd, pt->v, pt->u * sizeof(pt->v[0])) < 0) {
      err("prob_asgn: read failed: %s", os_ErrorMsg());
      pt->error_flag = 1;
      return;
    }
  }
}

static void
prob_asgn_append(
        struct prob_assignment_table *pt,
        int user_id,
        int contest_id,
        int prob_id,
        int role)
{
  if (pt->u == pt->a) {
    if (!pt->a) pt->a = 16;
    pt->a *= 2;
    XREALLOC(pt->v, pt->a);
  }
  pt->v[pt->u].user_id = user_id;
  pt->v[pt->u].contest_id = contest_id;
  pt->v[pt->u].prob_id = prob_id;
  pt->v[pt->u].role = role;
  pt->u++;
  pt->data_dirty = 1;
}

static void
prob_asgn_remove(struct prob_assignment_table *pt, int i)
{
  int j;

  if (i < 0 || i >= pt->u) return;
  for (j = i + 1; j < pt->u; j++)
    pt->v[j - 1] = pt->v[j];
  memset(&pt->v[pt->u - 1], 0, sizeof(pt->v[0]));
  pt->u--;
  pt->data_dirty = 1;
}

static int
assign_examiner_func(
        void *data,
        int user_id,
        int contest_id,
        int prob_id)
{
  struct nsdb_files_state *state = (struct nsdb_files_state*) data;
  struct prob_assignment_table *pt = &state->prob_asgn;
  int i;

  prob_asgn_do_open(state);
  if (pt->error_flag) return -1;

  for (i = 0; i < pt->u; i++) {
    if (pt->v[i].user_id == user_id && pt->v[i].contest_id == contest_id
        && pt->v[i].prob_id == prob_id) {
      if (pt->v[i].role == USER_ROLE_EXAMINER)
        return 0;
      pt->v[i].role = USER_ROLE_CHIEF_EXAMINER;
      pt->data_dirty = 1;
      return 1;
    }
  }
  prob_asgn_append(pt, user_id, contest_id, prob_id, USER_ROLE_EXAMINER);

  return 0;
}

static int
assign_chief_examiner_func(
        void *data,
        int user_id,
        int contest_id,
        int prob_id,
        int force_flag)
{
  struct nsdb_files_state *state = (struct nsdb_files_state*) data;
  struct prob_assignment_table *pt = &state->prob_asgn;
  int i;

  prob_asgn_do_open(state);
  if (pt->error_flag) return -1;

  for (i = 0; i < pt->u; i++) {
    if (pt->v[i].contest_id == contest_id && pt->v[i].prob_id == prob_id
        && pt->v[i].role == USER_ROLE_CHIEF_EXAMINER) {
      if (pt->v[i].user_id == user_id) return 0;
      if (!force_flag) return -1;
      prob_asgn_remove(pt, i);
      i--;
      /*
      pt->v[i].role = USER_ROLE_EXAMINER;
      pt->data_dirty = 1;
      */
    }
  }

  for (i = 0; i < pt->u; i++) {
    if (pt->v[i].user_id == user_id && pt->v[i].contest_id == contest_id
        && pt->v[i].prob_id == prob_id) {
      if (pt->v[i].role == USER_ROLE_CHIEF_EXAMINER) return 0;
      pt->v[i].role = USER_ROLE_CHIEF_EXAMINER;
      pt->data_dirty = 1;
      return 1;
    }
  }

  prob_asgn_append(pt, user_id, contest_id, prob_id, USER_ROLE_CHIEF_EXAMINER);
  return 0;
}

static int
remove_examiner_func(
        void *data,
        int user_id,
        int contest_id,
        int prob_id)
{
  struct nsdb_files_state *state = (struct nsdb_files_state*) data;
  struct prob_assignment_table *pt = &state->prob_asgn;
  int i;

  prob_asgn_do_open(state);
  if (pt->error_flag) return -1;

  for (i = 0; i < pt->u; i++) {
    if (pt->v[i].user_id == user_id && pt->v[i].contest_id == contest_id
        && pt->v[i].prob_id == prob_id)
      break;
  }
  if (i >= pt->u) return 0;
  prob_asgn_remove(pt, i);
  return 1;
}

static int
get_examiner_role_func(
        void *data,
        int user_id,
        int contest_id,
        int prob_id)
{
  struct nsdb_files_state *state = (struct nsdb_files_state*) data;
  struct prob_assignment_table *pt = &state->prob_asgn;
  int i;

  prob_asgn_do_open(state);
  if (pt->error_flag) return -1;
  for (i = 0; i < pt->u; i++)
    if (pt->v[i].user_id == user_id && pt->v[i].contest_id == contest_id
        && pt->v[i].prob_id == prob_id)
      return pt->v[i].role;
  return 0;
}

static int
find_chief_examiner_func(
        void *data,
        int contest_id,
        int prob_id)
{
  struct nsdb_files_state *state = (struct nsdb_files_state*) data;
  struct prob_assignment_table *pt = &state->prob_asgn;
  int i;

  prob_asgn_do_open(state);
  if (pt->error_flag) return -1;

  for (i = 0; i < pt->u; i++)
    if (pt->v[i].contest_id == contest_id && pt->v[i].prob_id == prob_id
        && pt->v[i].role == USER_ROLE_CHIEF_EXAMINER)
      return pt->v[i].user_id;
  return 0;
}

struct examiner_user_id_iterator
{
  struct int_iterator b;

  struct nsdb_files_state *state;
  int contest_id;
  int prob_id;
  int cur_idx;
};

static int
examiner_user_id_iterator_has_next(int_iterator_t data)
{
  struct examiner_user_id_iterator *iter = (struct examiner_user_id_iterator*) data;
  struct prob_assignment_table *pt = &iter->state->prob_asgn;

  while (iter->cur_idx < pt->u
         && (iter->contest_id != pt->v[iter->cur_idx].contest_id
             || iter->prob_id != pt->v[iter->cur_idx].prob_id
             || pt->v[iter->cur_idx].role != USER_ROLE_EXAMINER))
    iter->cur_idx++;
  return iter->cur_idx < pt->u;
}

static int
examiner_user_id_iterator_get(int_iterator_t data)
{
  struct examiner_user_id_iterator *iter = (struct examiner_user_id_iterator*) data;
  struct prob_assignment_table *pt = &iter->state->prob_asgn;

  while (iter->cur_idx < pt->u
         && (iter->contest_id != pt->v[iter->cur_idx].contest_id
             || iter->prob_id != pt->v[iter->cur_idx].prob_id
             || pt->v[iter->cur_idx].role != USER_ROLE_EXAMINER))
    iter->cur_idx++;
  ASSERT(iter->cur_idx < pt->u);
  return pt->v[iter->cur_idx].user_id;
}

static void
examiner_user_id_iterator_next(int_iterator_t data)
{
  struct examiner_user_id_iterator *iter = (struct examiner_user_id_iterator*) data;
  struct prob_assignment_table *pt = &iter->state->prob_asgn;

  if (iter->cur_idx < pt->u) iter->cur_idx++;
  while (iter->cur_idx < pt->u
         && (iter->contest_id != pt->v[iter->cur_idx].contest_id
             || iter->prob_id != pt->v[iter->cur_idx].prob_id
             || pt->v[iter->cur_idx].role != USER_ROLE_EXAMINER))
    iter->cur_idx++;
}

static void
examiner_user_id_iterator_destroy(int_iterator_t data)
{
  struct examiner_user_id_iterator *iter = (struct examiner_user_id_iterator*) data;
  xfree(iter);
}

static struct int_iterator examiner_user_id_iterator_funcs =
{
  examiner_user_id_iterator_has_next,
  examiner_user_id_iterator_get,
  examiner_user_id_iterator_next,
  examiner_user_id_iterator_destroy,
};
static int_iterator_t
get_examiner_user_id_iterator_func(void *data, int contest_id, int prob_id)
{
  struct nsdb_files_state *state = (struct nsdb_files_state*) data;
  struct examiner_user_id_iterator *iter;

  XCALLOC(iter, 1);
  iter->b = examiner_user_id_iterator_funcs;
  iter->state = state;
  iter->contest_id = contest_id;
  iter->prob_id = prob_id;
  iter->cur_idx = 0;
  return (int_iterator_t) iter;
}

static int
get_examiner_count_func(
        void *data,
        int contest_id,
        int prob_id)
{
  struct nsdb_files_state *state = (struct nsdb_files_state*) data;
  struct prob_assignment_table *pt = &state->prob_asgn;
  int i, count;

  prob_asgn_do_open(state);
  if (pt->error_flag) return -1;

  for (i = 0, count = 0; i < pt->u; i++)
    if (pt->v[i].contest_id == contest_id && pt->v[i].prob_id == prob_id
        && pt->v[i].role == USER_ROLE_EXAMINER)
      count++;
  return count;
}

/*
 * Local variables:
 *  compile-command: "make"
 *  c-font-lock-extra-types: ("\\sw+_t" "FILE")
 * End:
 */

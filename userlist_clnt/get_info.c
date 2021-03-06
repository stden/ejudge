/* -*- mode: c -*- */
/* $Id: get_info.c 5679 2010-01-19 10:01:11Z cher $ */

/* Copyright (C) 2002-2007 Alexander Chernov <cher@ejudge.ru> */

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

#include "userlist_clnt/private.h"

int
userlist_clnt_get_info(struct userlist_clnt *clnt,
                       int cmd,
                       int uid,
                       int contest_id,
                       unsigned char **p_info)
{
  struct userlist_pk_get_user_info out_pkt;
  struct userlist_pk_xml_data *in_pkt = 0;
  size_t in_size;
  int info_len;
  int r;

#if !defined PYTHON
  ASSERT(clnt);
  ASSERT(clnt->fd >= 0);
#endif

  if (cmd != ULS_GET_USER_INFO && cmd != ULS_PRIV_GET_USER_INFO)
    return -ULS_ERR_PROTOCOL;

  memset(&out_pkt, 0, sizeof(out_pkt));
  out_pkt.request_id = cmd;
  out_pkt.user_id = uid;
  out_pkt.contest_id = contest_id;
  if ((r = userlist_clnt_send_packet(clnt, sizeof(out_pkt), &out_pkt)) < 0)
    return r;
  if ((r = userlist_clnt_read_and_notify(clnt, &in_size, (void*) &in_pkt)) < 0)
    return -r;
  if (!in_size || !in_pkt) return -1;
  if (in_pkt->reply_id != ULS_XML_DATA) {
    r = in_pkt->reply_id;
    xfree(in_pkt);
    return r;
  }
  if (in_size <= sizeof(struct userlist_pk_xml_data)) return -1;
  info_len = strlen(in_pkt->data);
  if (info_len != in_pkt->info_len) {
    xfree(in_pkt);
    return -ULS_ERR_PROTOCOL;
  }
  *p_info = xstrdup(in_pkt->data);
  xfree(in_pkt);
  return ULS_XML_DATA;
}

/*
 * Local variables:
 *  compile-command: "make -C .."
 *  c-font-lock-extra-types: ("\\sw+_t" "FILE")
 * End:
 */

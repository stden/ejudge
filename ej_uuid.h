/* -*- c -*- */
/* $Id: ej_uuid.h 7581 2013-11-10 14:11:16Z cher $ */
#ifndef __EJ_UUID_H__
#define __EJ_UUID_H__

/* Copyright (C) 2012-2013 Alexander Chernov <cher@ejudge.ru> */

/*
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 */

#include "ej_types.h"

int ej_uuid_parse(const unsigned char *str, ruint32_t uuid[4]);
const unsigned char *ej_uuid_unparse(const ruint32_t uuid[4], const unsigned char *default_value);
void ej_uuid_generate(ruint32_t uuid[4]);
int ej_uuid_supported(void);

#define ej_uuid_is_nonempty(uuid) ((uuid)[0] || (uuid)[1] || (uuid)[2] || (uuid)[3])

#endif /* __EJ_UUID_H__ */

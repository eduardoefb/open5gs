/*
 * Copyright (C) 2019-2022 by Sukchan Lee <acetcom@gmail.com>
 *
 * This file is part of Open5GS.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#ifndef UDR_CONTEXT_H
#define UDR_CONTEXT_H

#include "ogs-app.h"
#include "ogs-dbi.h"
#include "ogs-sbi.h"

#include "udr-sm.h"

#ifdef __cplusplus
extern "C" {
#endif

extern int __udr_log_domain;

#undef OGS_LOG_DOMAIN
#define OGS_LOG_DOMAIN __udr_log_domain

typedef struct udr_context_s {
    ogs_list_t subs_list;
} udr_context_t;

/* Nudr_DataRepository subs-to-notify (TS 29.505 5.2.2.x) */
typedef struct udr_subs_s {
    ogs_lnode_t lnode;

    char *id;
    char *ue_id;
    char *callback_reference;

    bool am_data;
    bool sm_data;

    /* Last provisioned data sent to the subscriber, used to compute changes */
    cJSON *snapshot;
} udr_subs_t;

void udr_context_init(void);
void udr_context_final(void);
udr_context_t *udr_self(void);

int udr_context_parse_config(void);

udr_subs_t *udr_subs_add(char *ue_id, char *callback_reference);
void udr_subs_remove(udr_subs_t *subs);
void udr_subs_remove_all(void);
udr_subs_t *udr_subs_find_by_id(char *id);

#ifdef __cplusplus
}
#endif

#endif /* UDR_CONTEXT_H */

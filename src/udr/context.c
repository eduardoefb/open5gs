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

#include "sbi-path.h"

static udr_context_t self;

int __udr_log_domain;

static int context_initialized = 0;

void udr_context_init(void)
{
    ogs_assert(context_initialized == 0);

    /* Initialize UDR context */
    memset(&self, 0, sizeof(udr_context_t));
    ogs_list_init(&self.subs_list);

    ogs_log_install_domain(&__ogs_dbi_domain, "dbi", ogs_core()->log.level);
    ogs_log_install_domain(&__udr_log_domain, "udr", ogs_core()->log.level);

    context_initialized = 1;
}

void udr_context_final(void)
{
    ogs_assert(context_initialized == 1);

    udr_subs_remove_all();

    context_initialized = 0;
}

udr_subs_t *udr_subs_add(char *ue_id, char *callback_reference)
{
    udr_subs_t *subs = NULL;
    ogs_uuid_t uuid;
    char id[OGS_UUID_FORMATTED_LENGTH + 1];

    ogs_assert(ue_id);
    ogs_assert(callback_reference);

    if (ogs_list_count(&self.subs_list) >=
            ogs_global_conf()->max.ue * OGS_MAX_NUM_OF_SLICE) {
        ogs_error("Maximum number of subs-to-notify [%d] reached",
                ogs_list_count(&self.subs_list));
        return NULL;
    }

    subs = ogs_calloc(1, sizeof(*subs));
    ogs_assert(subs);

    ogs_uuid_get(&uuid);
    ogs_uuid_format(id, &uuid);

    subs->id = ogs_strdup(id);
    ogs_assert(subs->id);
    subs->ue_id = ogs_strdup(ue_id);
    ogs_assert(subs->ue_id);
    subs->callback_reference = ogs_strdup(callback_reference);
    ogs_assert(subs->callback_reference);

    ogs_list_add(&self.subs_list, subs);

    return subs;
}

void udr_subs_remove(udr_subs_t *subs)
{
    ogs_assert(subs);

    ogs_list_remove(&self.subs_list, subs);

    ogs_free(subs->id);
    ogs_free(subs->ue_id);
    ogs_free(subs->callback_reference);
    if (subs->snapshot)
        cJSON_Delete(subs->snapshot);

    ogs_free(subs);
}

void udr_subs_remove_all(void)
{
    udr_subs_t *subs = NULL, *next_subs = NULL;

    ogs_list_for_each_safe(&self.subs_list, next_subs, subs)
        udr_subs_remove(subs);
}

udr_subs_t *udr_subs_find_by_id(char *id)
{
    udr_subs_t *subs = NULL;

    ogs_assert(id);

    ogs_list_for_each(&self.subs_list, subs) {
        if (!strcmp(subs->id, id))
            return subs;
    }

    return NULL;
}

udr_context_t *udr_self(void)
{
    return &self;
}

static int udr_context_prepare(void)
{
    return OGS_OK;
}

static int udr_context_validation(void)
{
    return OGS_OK;
}

int udr_context_parse_config(void)
{
    int rv;
    yaml_document_t *document = NULL;
    ogs_yaml_iter_t root_iter;
    int idx = 0;

    document = ogs_app()->document;
    ogs_assert(document);

    rv = udr_context_prepare();
    if (rv != OGS_OK) return rv;

    ogs_yaml_iter_init(&root_iter, document);
    while (ogs_yaml_iter_next(&root_iter)) {
        const char *root_key = ogs_yaml_iter_key(&root_iter);
        ogs_assert(root_key);
        if ((!strcmp(root_key, "udr")) &&
            (idx++ == ogs_app()->config_section_id)) {
            ogs_yaml_iter_t udr_iter;
            ogs_yaml_iter_recurse(&root_iter, &udr_iter);
            while (ogs_yaml_iter_next(&udr_iter)) {
                const char *udr_key = ogs_yaml_iter_key(&udr_iter);
                ogs_assert(udr_key);
                if (!strcmp(udr_key, "default")) {
                    /* handle config in sbi library */
                } else if (!strcmp(udr_key, "sbi")) {
                    /* handle config in sbi library */
                } else if (!strcmp(udr_key, "nrf")) {
                    /* handle config in sbi library */
                } else if (!strcmp(udr_key, "scp")) {
                    /* handle config in sbi library */
                } else if (!strcmp(udr_key, "service_name")) {
                    /* handle config in sbi library */
                } else if (!strcmp(udr_key, "discovery")) {
                    /* handle config in sbi library */
                } else
                    ogs_warn("unknown key `%s`", udr_key);
            }
        }
    }

    rv = udr_context_validation();
    if (rv != OGS_OK) return rv;

    return OGS_OK;
}

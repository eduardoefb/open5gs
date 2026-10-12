/*
 * Copyright (C) 2026 by Open5GS contributors
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

/*
 * Nudr_DataRepository subscription to notifications (TS 29.505).
 *
 * Subscriber data is written to MongoDB directly (WebUI, dbctl), bypassing
 * the UDR. Changes are therefore detected with a MongoDB change stream
 * (requires a replica set), compared with the data last notified to each
 * subscription, and sent to the UDM as a DataChangeNotify. A subscriber
 * still missing DELETE_GRACE after a delete event is reported in delResources
 * and its subscriptions are removed (a delete followed by a re-insert of the
 * same IMSI is not a withdrawal).
 */

#include "sbi-path.h"
#include "data-change.h"

#define POLL_INTERVAL   ogs_time_from_msec(500)
#define RETRY_INTERVAL  ogs_time_from_sec(10)
#define DELETE_GRACE    ogs_time_from_sec(2)
/* mongoc_change_stream_next() blocks the event loop for up to this long */
#define MAX_AWAIT_MSEC  10

static ogs_timer_t *t_poll = NULL;
static ogs_timer_t *t_delete = NULL;
static bool delete_pending = false;
static mongoc_collection_t *collection = NULL;
static mongoc_change_stream_t *change_stream = NULL;
static bool watch_warned = false;

static int watch_open(void)
{
    bson_t *pipeline = NULL, *opts = NULL;
    const bson_t *err_doc = NULL;
    bson_error_t error;

    if (!collection) {
        if (!ogs_mongoc()->client || !ogs_mongoc()->name)
            return OGS_ERROR;
        collection = mongoc_client_get_collection(
                ogs_mongoc()->client, ogs_mongoc()->name, "subscribers");
        ogs_assert(collection);
    }

    pipeline = BCON_NEW("pipeline", "[",
            "{", "$match", "{", "operationType", "{", "$in", "[",
                BCON_UTF8("insert"), BCON_UTF8("update"), BCON_UTF8("replace"),
                BCON_UTF8("delete"),
            "]", "}", "}", "}",
        "]");
    opts = BCON_NEW(
            "fullDocument", BCON_UTF8("updateLookup"),
            "maxAwaitTimeMS", BCON_INT64(MAX_AWAIT_MSEC));

    change_stream = mongoc_collection_watch(collection, pipeline, opts);
    bson_destroy(pipeline);
    bson_destroy(opts);

    if (mongoc_change_stream_error_document(
                change_stream, &error, &err_doc)) {
        if (!watch_warned) {
            ogs_warn("MongoDB change stream unavailable [%s]: subscriber "
                    "data change notifications are disabled until MongoDB "
                    "runs as a replica set", error.message);
            watch_warned = true;
        }
        mongoc_change_stream_destroy(change_stream);
        change_stream = NULL;
        return OGS_ERROR;
    }

    ogs_info("MongoDB change stream enabled "
            "(subscriber data change notifications)");
    watch_warned = false;

    return OGS_OK;
}

static cJSON *ambr_to_json(ogs_bitrate_t *ambr)
{
    cJSON *json = NULL;
    char *s = NULL;

    json = cJSON_CreateObject();
    ogs_assert(json);

    s = ogs_sbi_bitrate_to_string(ambr->uplink, OGS_SBI_BITRATE_KBPS);
    ogs_assert(s);
    cJSON_AddStringToObject(json, "uplink", s);
    ogs_free(s);

    s = ogs_sbi_bitrate_to_string(ambr->downlink, OGS_SBI_BITRATE_KBPS);
    ogs_assert(s);
    cJSON_AddStringToObject(json, "downlink", s);
    ogs_free(s);

    return json;
}

static cJSON *snssai_to_json(ogs_s_nssai_t *s_nssai)
{
    cJSON *json = NULL;
    char *sd = NULL;

    json = cJSON_CreateObject();
    ogs_assert(json);

    cJSON_AddNumberToObject(json, "sst", s_nssai->sst);
    sd = ogs_s_nssai_sd_to_string(s_nssai->sd);
    if (sd) {
        cJSON_AddStringToObject(json, "sd", sd);
        ogs_free(sd);
    }

    return json;
}

static char *snssai_key(ogs_s_nssai_t *s_nssai)
{
    if (s_nssai->sd.v == OGS_S_NSSAI_NO_SD_VALUE)
        return ogs_msprintf("%d", s_nssai->sst);
    return ogs_msprintf("%d-%06x", s_nssai->sst, s_nssai->sd.v);
}

static cJSON *qos_to_json(ogs_qos_t *qos)
{
    cJSON *json = NULL, *arp = NULL;

    json = cJSON_CreateObject();
    ogs_assert(json);

    cJSON_AddNumberToObject(json, "5qi", qos->index);

    arp = cJSON_AddObjectToObject(json, "arp");
    ogs_assert(arp);
    cJSON_AddNumberToObject(arp, "priorityLevel", qos->arp.priority_level);
    cJSON_AddStringToObject(arp, "preemptCap",
            qos->arp.pre_emption_capability == OGS_5GC_PRE_EMPTION_ENABLED ?
                "MAY_PREEMPT" : "NOT_PREEMPT");
    cJSON_AddStringToObject(arp, "preemptVuln",
            qos->arp.pre_emption_vulnerability == OGS_5GC_PRE_EMPTION_ENABLED ?
                "PREEMPTABLE" : "NOT_PREEMPTABLE");

    cJSON_AddNumberToObject(json, "priorityLevel", qos->arp.priority_level);

    return json;
}

/*
 * {
 *   "am": { "subscribedUeAmbr": {..}, "nssai": {..} },
 *   "sm": { "<sst>[-<sd>]": { "singleNssai": {..},
 *            "dnnConfigurations": { "<dnn>": {
 *                "5gQosProfile": {..}, "sessionAmbr": {..} } } } }
 * }
 */
static cJSON *snapshot_build(char *supi)
{
    ogs_subscription_data_t data;
    cJSON *root = NULL, *am = NULL, *sm = NULL;
    cJSON *nssai = NULL, *defaults = NULL, *singles = NULL;
    int i, j;

    if (ogs_dbi_subscription_data(supi, &data) != OGS_OK) {
        ogs_warn("[%s] Cannot read subscription data", supi);
        return NULL;
    }

    root = cJSON_CreateObject();
    ogs_assert(root);

    am = cJSON_AddObjectToObject(root, "am");
    ogs_assert(am);
    if (data.ambr.uplink || data.ambr.downlink)
        cJSON_AddItemToObject(am, "subscribedUeAmbr",
                ambr_to_json(&data.ambr));

    nssai = cJSON_AddObjectToObject(am, "nssai");
    ogs_assert(nssai);
    defaults = cJSON_AddArrayToObject(nssai, "defaultSingleNssais");
    ogs_assert(defaults);
    singles = cJSON_AddArrayToObject(nssai, "singleNssais");
    ogs_assert(singles);

    sm = cJSON_AddObjectToObject(root, "sm");
    ogs_assert(sm);

    for (i = 0; i < data.num_of_slice; i++) {
        ogs_slice_data_t *slice = &data.slice[i];
        cJSON *entry = NULL, *dnns = NULL;
        char *key = NULL;

        cJSON_AddItemToArray(slice->default_indicator ? defaults : singles,
                snssai_to_json(&slice->s_nssai));

        key = snssai_key(&slice->s_nssai);
        ogs_assert(key);
        entry = cJSON_AddObjectToObject(sm, key);
        ogs_free(key);
        ogs_assert(entry);

        cJSON_AddItemToObject(entry, "singleNssai",
                snssai_to_json(&slice->s_nssai));
        dnns = cJSON_AddObjectToObject(entry, "dnnConfigurations");
        ogs_assert(dnns);

        for (j = 0; j < slice->num_of_session; j++) {
            ogs_session_t *session = &slice->session[j];
            cJSON *dnn = NULL;

            if (!session->name)
                continue;

            dnn = cJSON_AddObjectToObject(dnns, session->name);
            ogs_assert(dnn);
            cJSON_AddItemToObject(dnn, "5gQosProfile",
                    qos_to_json(&session->qos));
            cJSON_AddItemToObject(dnn, "sessionAmbr",
                    ambr_to_json(&session->ambr));
        }
    }

    ogs_subscription_data_free(&data);

    return root;
}

static char *resource_uri(char *supi, const char *dataset, cJSON *single_nssai)
{
    ogs_sbi_server_t *server = NULL;
    ogs_sbi_header_t header;
    char *uri = NULL, *json = NULL, *encoded = NULL, *result = NULL;

    server = ogs_sbi_server_first();
    ogs_assert(server);

    memset(&header, 0, sizeof(header));
    header.service.name =
        (char *)OpenAPI_service_name_ToString(OpenAPI_service_name_nudr_dr);
    header.api.version = (char *)OGS_SBI_API_V1;
    header.resource.component[0] =
        (char *)OGS_SBI_RESOURCE_NAME_SUBSCRIPTION_DATA;
    header.resource.component[1] = supi;
    header.resource.component[2] =
        (char *)OGS_SBI_RESOURCE_NAME_PROVISIONED_DATA;
    header.resource.component[3] = (char *)dataset;

    uri = ogs_sbi_server_uri(server, &header);
    ogs_assert(uri);

    if (!single_nssai)
        return uri;

    json = cJSON_PrintUnformatted(single_nssai);
    ogs_assert(json);
    encoded = ogs_sbi_url_encode(json);
    ogs_assert(encoded);
    cJSON_free(json);

    result = ogs_msprintf("%s?%s=%s",
            uri, OGS_SBI_PARAM_SINGLE_NSSAI, encoded);
    ogs_assert(result);

    ogs_free(encoded);
    ogs_free(uri);

    return result;
}

static void add_change(OpenAPI_list_t *changes,
        char *path, cJSON *orig_value, cJSON *new_value)
{
    OpenAPI_change_item_t *item = NULL;
    OpenAPI_change_type_e op;

    if (!orig_value)
        op = OpenAPI_change_type_ADD;
    else if (!new_value)
        op = OpenAPI_change_type__REMOVE;
    else
        op = OpenAPI_change_type_REPLACE;

    item = OpenAPI_change_item_create(op, ogs_strdup(path), NULL,
            false, orig_value ? OpenAPI_any_type_create(orig_value) : NULL,
            false, new_value ? OpenAPI_any_type_create(new_value) : NULL);
    ogs_assert(item);

    OpenAPI_list_add(changes, item);
}

static void diff_member(OpenAPI_list_t *changes, char *path,
        cJSON *old_obj, cJSON *new_obj, const char *name)
{
    cJSON *o = cJSON_GetObjectItemCaseSensitive(old_obj, name);
    cJSON *n = cJSON_GetObjectItemCaseSensitive(new_obj, name);

    if (!o && !n)
        return;
    if (o && n && cJSON_Compare(o, n, true))
        return;

    add_change(changes, path, o, n);
}

static void add_notify_item(OpenAPI_list_t *items,
        OpenAPI_list_t *changes, char *resource_id)
{
    if (!changes->count) {
        OpenAPI_list_free(changes);
        ogs_free(resource_id);
        return;
    }

    OpenAPI_list_add(items, OpenAPI_notify_item_create(resource_id, changes));
}

static void diff_sm_slice(OpenAPI_list_t *items, char *supi,
        cJSON *old_slice, cJSON *new_slice)
{
    cJSON *od = NULL, *nd = NULL, *dnn = NULL;
    OpenAPI_list_t *changes = NULL;
    char *path = NULL;

    od = cJSON_GetObjectItemCaseSensitive(old_slice, "dnnConfigurations");
    nd = cJSON_GetObjectItemCaseSensitive(new_slice, "dnnConfigurations");

    changes = OpenAPI_list_create();
    ogs_assert(changes);

    cJSON_ArrayForEach(dnn, nd) {
        cJSON *old_dnn = cJSON_GetObjectItemCaseSensitive(od, dnn->string);

        if (!old_dnn) {
            path = ogs_msprintf("/dnnConfigurations/%s", dnn->string);
            add_change(changes, path, NULL, dnn);
            ogs_free(path);
            continue;
        }

        path = ogs_msprintf("/dnnConfigurations/%s/sessionAmbr", dnn->string);
        diff_member(changes, path, old_dnn, dnn, "sessionAmbr");
        ogs_free(path);

        path = ogs_msprintf("/dnnConfigurations/%s/5gQosProfile", dnn->string);
        diff_member(changes, path, old_dnn, dnn, "5gQosProfile");
        ogs_free(path);
    }

    cJSON_ArrayForEach(dnn, od) {
        if (cJSON_GetObjectItemCaseSensitive(nd, dnn->string))
            continue;
        path = ogs_msprintf("/dnnConfigurations/%s", dnn->string);
        add_change(changes, path, dnn, NULL);
        ogs_free(path);
    }

    add_notify_item(items, changes, resource_uri(supi,
                OGS_SBI_RESOURCE_NAME_SM_DATA,
                cJSON_GetObjectItemCaseSensitive(
                    new_slice ? new_slice : old_slice, "singleNssai")));
}

static void notify_items_free(OpenAPI_list_t *items)
{
    OpenAPI_lnode_t *node = NULL;

    OpenAPI_list_for_each(items, node)
        OpenAPI_notify_item_free(node->data);
    OpenAPI_list_free(items);
}

static OpenAPI_list_t *snapshot_diff(
        udr_subs_t *subs, cJSON *old_snapshot, cJSON *new_snapshot)
{
    OpenAPI_list_t *items = NULL;

    items = OpenAPI_list_create();
    ogs_assert(items);

    if (subs->am_data) {
        cJSON *o = cJSON_GetObjectItemCaseSensitive(old_snapshot, "am");
        cJSON *n = cJSON_GetObjectItemCaseSensitive(new_snapshot, "am");
        OpenAPI_list_t *changes = OpenAPI_list_create();
        ogs_assert(changes);

        diff_member(changes, (char *)"/subscribedUeAmbr",
                o, n, "subscribedUeAmbr");
        diff_member(changes, (char *)"/nssai", o, n, "nssai");

        add_notify_item(items, changes, resource_uri(subs->ue_id,
                    OGS_SBI_RESOURCE_NAME_AM_DATA, NULL));
    }

    if (subs->sm_data) {
        cJSON *osm = cJSON_GetObjectItemCaseSensitive(old_snapshot, "sm");
        cJSON *nsm = cJSON_GetObjectItemCaseSensitive(new_snapshot, "sm");
        cJSON *slice = NULL;

        cJSON_ArrayForEach(slice, nsm) {
            diff_sm_slice(items, subs->ue_id,
                    cJSON_GetObjectItemCaseSensitive(osm, slice->string),
                    slice);
        }
        cJSON_ArrayForEach(slice, osm) {
            if (!cJSON_GetObjectItemCaseSensitive(nsm, slice->string))
                diff_sm_slice(items, subs->ue_id, slice, NULL);
        }
    }

    if (!items->count) {
        OpenAPI_list_free(items);
        return NULL;
    }

    return items;
}

static int notify_cb(int status, ogs_sbi_response_t *response, void *data)
{
    char *id = data;
    udr_subs_t *subs = NULL;

    ogs_assert(id);

    if (status != OGS_OK) {
        ogs_log_message(
                status == OGS_DONE ? OGS_LOG_DEBUG : OGS_LOG_WARN, 0,
                "[%s] DataChangeNotify failed [%d]", id, status);
        ogs_free(id);
        return OGS_ERROR;
    }

    ogs_assert(response);

    if (response->status == OGS_SBI_HTTP_STATUS_NOT_FOUND) {
        subs = udr_subs_find_by_id(id);
        if (subs) {
            ogs_warn("[%s:%s] Unknown to the subscriber, removing",
                    subs->ue_id, id);
            udr_subs_remove(subs);
        }
    } else if (response->status >= 300) {
        ogs_warn("[%s] DataChangeNotify failed [HTTP %d]",
                id, response->status);
    }

    ogs_sbi_response_free(response);
    ogs_free(id);

    return OGS_OK;
}

static void send_data_change_notify(udr_subs_t *subs,
        OpenAPI_list_t *items, OpenAPI_list_t *del_resources)
{
    ogs_sbi_message_t message;
    ogs_sbi_request_t *request = NULL;
    ogs_sbi_client_t *client = NULL;
    OpenAPI_data_change_notify_t DataChangeNotify;
    OpenAPI_list_t *OriginalCallbackReference = NULL;

    OpenAPI_uri_scheme_e scheme = OpenAPI_uri_scheme_NULL;
    char *fqdn = NULL;
    uint16_t fqdn_port = 0;
    ogs_sockaddr_t *addr = NULL, *addr6 = NULL;
    char *id = NULL;

    if (ogs_sbi_getaddr_from_uri(&scheme, &fqdn, &fqdn_port, &addr, &addr6,
                subs->callback_reference) == false ||
            scheme == OpenAPI_uri_scheme_NULL) {
        ogs_error("[%s] Invalid callbackReference [%s]",
                subs->ue_id, subs->callback_reference);
        return;
    }

    client = ogs_sbi_client_find(scheme, fqdn, fqdn_port, addr, addr6);
    if (!client)
        client = ogs_sbi_client_add(scheme, fqdn, fqdn_port, addr, addr6);

    ogs_free(fqdn);
    ogs_freeaddrinfo(addr);
    ogs_freeaddrinfo(addr6);

    if (!client) {
        ogs_error("[%s] ogs_sbi_client_add() failed", subs->ue_id);
        return;
    }

    OriginalCallbackReference = OpenAPI_list_create();
    ogs_assert(OriginalCallbackReference);
    OpenAPI_list_add(OriginalCallbackReference, subs->callback_reference);

    memset(&DataChangeNotify, 0, sizeof(DataChangeNotify));
    DataChangeNotify.original_callback_reference = OriginalCallbackReference;
    DataChangeNotify.ue_id = subs->ue_id;
    DataChangeNotify.notify_items = items;
    DataChangeNotify.del_resources = del_resources;

    memset(&message, 0, sizeof(message));
    message.h.method = (char *)OGS_SBI_HTTP_METHOD_POST;
    message.h.uri = subs->callback_reference;
    message.DataChangeNotify = &DataChangeNotify;

    request = ogs_sbi_build_request(&message);
    OpenAPI_list_free(OriginalCallbackReference);
    if (!request) {
        ogs_error("[%s] ogs_sbi_build_request() failed", subs->ue_id);
        return;
    }

    id = ogs_strdup(subs->id);
    ogs_assert(id);
    if (ogs_sbi_send_request_to_client(
                client, notify_cb, request, id) == false) {
        ogs_error("[%s] ogs_sbi_send_request_to_client() failed",
                subs->ue_id);
        ogs_free(id);
    }

    ogs_sbi_request_free(request);
}

static void handle_change(const bson_t *document)
{
    bson_iter_t iter, child;
    const char *imsi = NULL;
    uint32_t length = 0;
    char *supi = NULL;
    cJSON *snapshot = NULL;
    udr_subs_t *subs = NULL;

    if (!ogs_list_count(&udr_self()->subs_list))
        return;

    if (!bson_iter_init_find(&iter, document, "fullDocument") ||
        !BSON_ITER_HOLDS_DOCUMENT(&iter) ||
        !bson_iter_recurse(&iter, &child) ||
        !bson_iter_find(&child, "imsi") ||
        !BSON_ITER_HOLDS_UTF8(&child))
        return;

    imsi = bson_iter_utf8(&child, &length);
    supi = ogs_msprintf("%s-%.*s", OGS_ID_SUPI_TYPE_IMSI, (int)length, imsi);
    ogs_assert(supi);

    ogs_list_for_each(&udr_self()->subs_list, subs) {
        OpenAPI_list_t *items = NULL;

        if (strcmp(subs->ue_id, supi))
            continue;

        if (!snapshot) {
            snapshot = snapshot_build(supi);
            if (!snapshot)
                break;
        }

        if (subs->snapshot) {
            items = snapshot_diff(subs, subs->snapshot, snapshot);
            if (items) {
                ogs_info("[%s] Subscription data changed, notifying [%s]",
                        supi, subs->callback_reference);
                send_data_change_notify(subs, items, NULL);
                notify_items_free(items);
            }
            cJSON_Delete(subs->snapshot);
        }
        subs->snapshot = cJSON_Duplicate(snapshot, true);
        ogs_assert(subs->snapshot);
    }

    if (snapshot)
        cJSON_Delete(snapshot);
    ogs_free(supi);
}

static bool subscriber_exists(const char *supi)
{
    const char *imsi = NULL;
    bson_t *query = NULL;
    bson_error_t error;
    int64_t count;

    imsi = strchr(supi, '-');
    imsi = imsi ? imsi + 1 : supi;

    query = BCON_NEW("imsi", BCON_UTF8(imsi));
    count = mongoc_collection_count_documents(
            collection, query, NULL, NULL, NULL, &error);
    bson_destroy(query);

    if (count < 0) {
        ogs_error("[%s] Cannot count subscribers [%s]", supi, error.message);
        return true;
    }

    return count > 0;
}

/* The delete event only carries the document _id, not the IMSI */
static void handle_subscriber_delete(void *data)
{
    udr_subs_t *subs = NULL, *next_subs = NULL;

    delete_pending = false;

    ogs_list_for_each_safe(&udr_self()->subs_list, next_subs, subs) {
        OpenAPI_list_t *del_resources = NULL;
        OpenAPI_lnode_t *node = NULL;

        if (subscriber_exists(subs->ue_id))
            continue;

        del_resources = OpenAPI_list_create();
        ogs_assert(del_resources);
        if (subs->am_data)
            OpenAPI_list_add(del_resources, resource_uri(
                        subs->ue_id, OGS_SBI_RESOURCE_NAME_AM_DATA, NULL));
        if (subs->sm_data)
            OpenAPI_list_add(del_resources, resource_uri(
                        subs->ue_id, OGS_SBI_RESOURCE_NAME_SM_DATA, NULL));

        ogs_info("[%s] Subscriber deleted, notifying [%s]",
                subs->ue_id, subs->callback_reference);
        send_data_change_notify(subs, NULL, del_resources);

        OpenAPI_list_for_each(del_resources, node)
            ogs_free(node->data);
        OpenAPI_list_free(del_resources);

        udr_subs_remove(subs);
    }
}

static void handle_event(const bson_t *document)
{
    bson_iter_t iter;

    if (!ogs_list_count(&udr_self()->subs_list))
        return;

    if (bson_iter_init_find(&iter, document, "operationType") &&
        BSON_ITER_HOLDS_UTF8(&iter) &&
        !strcmp(bson_iter_utf8(&iter, NULL), "delete")) {
        if (!delete_pending) {
            delete_pending = true;
            ogs_timer_start(t_delete, DELETE_GRACE);
        }
        return;
    }

    handle_change(document);
}

static void poll_cb(void *data)
{
    const bson_t *document = NULL;
    const bson_t *err_doc = NULL;
    bson_error_t error;
    ogs_time_t next = POLL_INTERVAL;

    if (!change_stream && watch_open() != OGS_OK) {
        next = RETRY_INTERVAL;
        goto out;
    }

    while (mongoc_change_stream_next(change_stream, &document))
        handle_event(document);

    if (mongoc_change_stream_error_document(change_stream, &error, &err_doc)) {
        ogs_warn("MongoDB change stream error [%s], reopening",
                error.message);
        mongoc_change_stream_destroy(change_stream);
        change_stream = NULL;
        next = RETRY_INTERVAL;
    }

out:
    ogs_timer_start(t_poll, next);
}

void udr_data_change_open(void)
{
    t_poll = ogs_timer_add(ogs_app()->timer_mgr, poll_cb, NULL);
    ogs_assert(t_poll);
    t_delete = ogs_timer_add(ogs_app()->timer_mgr,
            handle_subscriber_delete, NULL);
    ogs_assert(t_delete);
    ogs_timer_start(t_poll, POLL_INTERVAL);
}

void udr_data_change_close(void)
{
    if (t_poll) {
        ogs_timer_delete(t_poll);
        t_poll = NULL;
    }
    if (t_delete) {
        ogs_timer_delete(t_delete);
        t_delete = NULL;
    }
    delete_pending = false;
    if (change_stream) {
        mongoc_change_stream_destroy(change_stream);
        change_stream = NULL;
    }
    if (collection) {
        mongoc_collection_destroy(collection);
        collection = NULL;
    }
}

static bool handle_create(
        ogs_sbi_stream_t *stream, ogs_sbi_message_t *recvmsg)
{
    OpenAPI_subscription_data_subscriptions_t *SubscriptionDataSubscriptions;
    OpenAPI_lnode_t *node = NULL;
    udr_subs_t *subs = NULL, *next_subs = NULL;

    ogs_sbi_message_t sendmsg;
    ogs_sbi_response_t *response = NULL;
    ogs_sbi_server_t *server = NULL;
    ogs_sbi_header_t header;

    SubscriptionDataSubscriptions = recvmsg->SubscriptionDataSubscriptions;
    if (!SubscriptionDataSubscriptions ||
        !SubscriptionDataSubscriptions->ue_id ||
        !SubscriptionDataSubscriptions->callback_reference ||
        !SubscriptionDataSubscriptions->monitored_resource_uris ||
        !SubscriptionDataSubscriptions->monitored_resource_uris->count) {
        ogs_error("Invalid SubscriptionDataSubscriptions");
        ogs_assert(true ==
            ogs_sbi_server_send_error(stream, OGS_SBI_HTTP_STATUS_BAD_REQUEST,
                recvmsg, "Invalid SubscriptionDataSubscriptions", NULL, NULL));
        return false;
    }

    /* A re-subscription from the same consumer replaces the previous one */
    ogs_list_for_each_safe(&udr_self()->subs_list, next_subs, subs) {
        if (!strcmp(subs->ue_id, SubscriptionDataSubscriptions->ue_id) &&
            !strcmp(subs->callback_reference,
                SubscriptionDataSubscriptions->callback_reference))
            udr_subs_remove(subs);
    }

    subs = udr_subs_add(SubscriptionDataSubscriptions->ue_id,
            SubscriptionDataSubscriptions->callback_reference);
    if (!subs) {
        ogs_assert(true ==
            ogs_sbi_server_send_error(stream,
                OGS_SBI_HTTP_STATUS_INTERNAL_SERVER_ERROR,
                recvmsg, "udr_subs_add() failed", NULL, NULL));
        return false;
    }

    OpenAPI_list_for_each(
            SubscriptionDataSubscriptions->monitored_resource_uris, node) {
        char *uri = node->data;
        if (!uri)
            continue;
        if (strstr(uri, OGS_SBI_RESOURCE_NAME_AM_DATA))
            subs->am_data = true;
        if (strstr(uri, OGS_SBI_RESOURCE_NAME_SM_DATA))
            subs->sm_data = true;
    }

    if (!subs->am_data && !subs->sm_data) {
        ogs_error("[%s] No supported monitoredResourceUris", subs->ue_id);
        udr_subs_remove(subs);
        ogs_assert(true ==
            ogs_sbi_server_send_error(stream, OGS_SBI_HTTP_STATUS_BAD_REQUEST,
                recvmsg, "No supported monitoredResourceUris", NULL, NULL));
        return false;
    }

    subs->snapshot = snapshot_build(subs->ue_id);

    server = ogs_sbi_server_from_stream(stream);
    ogs_assert(server);

    memset(&header, 0, sizeof(header));
    header.service.name =
        (char *)OpenAPI_service_name_ToString(OpenAPI_service_name_nudr_dr);
    header.api.version = (char *)OGS_SBI_API_V1;
    header.resource.component[0] =
        (char *)OGS_SBI_RESOURCE_NAME_SUBSCRIPTION_DATA;
    header.resource.component[1] =
        (char *)OGS_SBI_RESOURCE_NAME_SUBS_TO_NOTIFY;
    header.resource.component[2] = subs->id;

    memset(&sendmsg, 0, sizeof(sendmsg));
    sendmsg.http.location = ogs_sbi_server_uri(server, &header);
    ogs_assert(sendmsg.http.location);

    sendmsg.SubscriptionDataSubscriptions =
        OpenAPI_subscription_data_subscriptions_copy(
                NULL, SubscriptionDataSubscriptions);
    ogs_assert(sendmsg.SubscriptionDataSubscriptions);
    if (sendmsg.SubscriptionDataSubscriptions->subscription_id)
        ogs_free(sendmsg.SubscriptionDataSubscriptions->subscription_id);
    sendmsg.SubscriptionDataSubscriptions->subscription_id =
        ogs_strdup(subs->id);

    response = ogs_sbi_build_response(&sendmsg, OGS_SBI_HTTP_STATUS_CREATED);
    ogs_assert(response);
    ogs_assert(true == ogs_sbi_server_send_response(stream, response));

    ogs_free(sendmsg.http.location);
    OpenAPI_subscription_data_subscriptions_free(
            sendmsg.SubscriptionDataSubscriptions);

    ogs_info("[%s] subs-to-notify created [%s] am-data:%d sm-data:%d",
            subs->ue_id, subs->id, subs->am_data, subs->sm_data);

    return true;
}

static bool handle_delete(
        ogs_sbi_stream_t *stream, ogs_sbi_message_t *recvmsg)
{
    udr_subs_t *subs = NULL;

    if (!recvmsg->h.resource.component[2] ||
        !(subs = udr_subs_find_by_id(recvmsg->h.resource.component[2]))) {
        ogs_assert(true ==
            ogs_sbi_server_send_error(stream, OGS_SBI_HTTP_STATUS_NOT_FOUND,
                recvmsg, "Subscription not found",
                recvmsg->h.resource.component[2], NULL));
        return false;
    }

    ogs_info("[%s] subs-to-notify deleted [%s]", subs->ue_id, subs->id);
    udr_subs_remove(subs);

    ogs_assert(true == ogs_sbi_send_http_status_no_content(stream));

    return true;
}

bool udr_nudr_dr_handle_subs_to_notify(
        ogs_sbi_stream_t *stream, ogs_sbi_message_t *recvmsg)
{
    ogs_assert(stream);
    ogs_assert(recvmsg);

    SWITCH(recvmsg->h.method)
    CASE(OGS_SBI_HTTP_METHOD_POST)
        if (recvmsg->h.resource.component[2])
            break;
        return handle_create(stream, recvmsg);
    CASE(OGS_SBI_HTTP_METHOD_DELETE)
        return handle_delete(stream, recvmsg);
    DEFAULT
    END

    ogs_error("Invalid HTTP method [%s]", recvmsg->h.method);
    ogs_assert(true ==
        ogs_sbi_server_send_error(stream, OGS_SBI_HTTP_STATUS_FORBIDDEN,
            recvmsg, "Invalid HTTP method", recvmsg->h.method, NULL));
    return false;
}

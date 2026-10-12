/*
 * Copyright (C) 2026 by Sukchan Lee <acetcom@gmail.com>
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
 * Subscriber data change notification (TS 23.502 4.5.2, TS 29.505 5.2.2.8,
 * TS 29.503 5.2.2.3.4): the UDM subscribes to the UDR on behalf of its
 * SDM subscribers and maps each DataChangeNotify to a ModificationNotification
 * towards the AMF/SMF that subscribed to the affected data set.
 */

#include "sbi-path.h"
#include "data-change.h"

#define PARAM_SINGLE_NSSAI "single-nssai="

void udm_sdm_subscription_set_monitored(
        udm_sdm_subscription_t *sdm_subscription,
        OpenAPI_sdm_subscription_t *SDMSubscription)
{
    OpenAPI_lnode_t *node = NULL;

    ogs_assert(sdm_subscription);
    ogs_assert(SDMSubscription);

    OpenAPI_list_for_each(SDMSubscription->monitored_resource_uris, node) {
        const char *uri = node->data;

        if (!uri) continue;
        if (strstr(uri, OGS_SBI_RESOURCE_NAME_AM_DATA))
            sdm_subscription->am_data = true;
        if (strstr(uri, OGS_SBI_RESOURCE_NAME_SM_DATA))
            sdm_subscription->sm_data = true;
    }

    if (SDMSubscription->dnn) {
        sdm_subscription->dnn = ogs_strdup(SDMSubscription->dnn);
        ogs_assert(sdm_subscription->dnn);
    }

    if (SDMSubscription->single_nssai) {
        sdm_subscription->has_s_nssai = true;
        sdm_subscription->s_nssai.sst = SDMSubscription->single_nssai->sst;
        sdm_subscription->s_nssai.sd =
            ogs_s_nssai_sd_from_string(SDMSubscription->single_nssai->sd);
    }
}

static ogs_sbi_request_t *build_subs_to_notify(udm_ue_t *udm_ue, void *data)
{
    ogs_sbi_message_t message;
    ogs_sbi_header_t header;
    ogs_sbi_request_t *request = NULL;
    ogs_sbi_server_t *server = NULL;
    OpenAPI_subscription_data_subscriptions_t SubscriptionDataSubscriptions;
    OpenAPI_list_t *MonitoredResourceUris = NULL;
    OpenAPI_lnode_t *node = NULL;

    ogs_assert(udm_ue);
    ogs_assert(udm_ue->supi);

    server = ogs_sbi_server_first();
    ogs_assert(server);

    memset(&header, 0, sizeof(header));
    header.service.name = (char *)OGS_SBI_SERVICE_NAME_NUDM_CALLBACK;
    header.api.version = (char *)OGS_SBI_API_V1;
    header.resource.component[0] = udm_ue->supi;
    header.resource.component[1] =
        (char *)OGS_SBI_RESOURCE_NAME_DATA_CHANGE_NOTIFY;

    MonitoredResourceUris = OpenAPI_list_create();
    ogs_assert(MonitoredResourceUris);
    OpenAPI_list_add(MonitoredResourceUris,
            ogs_msprintf("%s/%s/%s", udm_ue->supi,
                OGS_SBI_RESOURCE_NAME_PROVISIONED_DATA,
                OGS_SBI_RESOURCE_NAME_AM_DATA));
    OpenAPI_list_add(MonitoredResourceUris,
            ogs_msprintf("%s/%s/%s", udm_ue->supi,
                OGS_SBI_RESOURCE_NAME_PROVISIONED_DATA,
                OGS_SBI_RESOURCE_NAME_SM_DATA));

    memset(&SubscriptionDataSubscriptions, 0,
            sizeof(SubscriptionDataSubscriptions));
    SubscriptionDataSubscriptions.ue_id = udm_ue->supi;
    SubscriptionDataSubscriptions.callback_reference =
        ogs_sbi_server_uri(server, &header);
    ogs_assert(SubscriptionDataSubscriptions.callback_reference);
    SubscriptionDataSubscriptions.monitored_resource_uris =
        MonitoredResourceUris;

    memset(&message, 0, sizeof(message));
    message.h.method = (char *)OGS_SBI_HTTP_METHOD_POST;
    message.h.service.name = (char *)OpenAPI_service_name_ToString(
            OpenAPI_service_name_nudr_dr);
    message.h.api.version = (char *)OGS_SBI_API_V1;
    message.h.resource.component[0] =
        (char *)OGS_SBI_RESOURCE_NAME_SUBSCRIPTION_DATA;
    message.h.resource.component[1] =
        (char *)OGS_SBI_RESOURCE_NAME_SUBS_TO_NOTIFY;
    message.SubscriptionDataSubscriptions = &SubscriptionDataSubscriptions;

    request = ogs_sbi_build_request(&message);
    ogs_expect(request);

    ogs_free(SubscriptionDataSubscriptions.callback_reference);
    OpenAPI_list_for_each(MonitoredResourceUris, node)
        ogs_free(node->data);
    OpenAPI_list_free(MonitoredResourceUris);

    return request;
}

void udm_nudr_dr_send_subs_to_notify(udm_ue_t *udm_ue)
{
    int r;

    ogs_assert(udm_ue);

    r = udm_ue_sbi_discover_and_send(OpenAPI_service_name_nudr_dr, NULL,
            build_subs_to_notify, udm_ue, NULL, 0, NULL);
    if (r != OGS_OK)
        ogs_warn("[%s] Cannot subscribe to UDR data changes", udm_ue->supi);
}

void udm_nudr_dr_handle_subs_to_notify(
        ogs_pool_id_t udm_ue_id, ogs_sbi_message_t *recvmsg)
{
    udm_ue_t *udm_ue = NULL;
    char *id = NULL;

    ogs_assert(recvmsg);

    udm_ue = udm_ue_find_by_id(udm_ue_id);
    if (!udm_ue) {
        ogs_error("UE Context has already been removed");
        return;
    }

    if (recvmsg->res_status != OGS_SBI_HTTP_STATUS_CREATED) {
        ogs_warn("[%s] UDR subs-to-notify failed [HTTP %d]",
                udm_ue->supi, recvmsg->res_status);
        return;
    }

    if (recvmsg->http.location) {
        id = strrchr(recvmsg->http.location, '/');
        id = id ? id + 1 : recvmsg->http.location;
    } else if (recvmsg->SubscriptionDataSubscriptions) {
        id = recvmsg->SubscriptionDataSubscriptions->subscription_id;
    }

    if (udm_ue->udr_subs_id)
        ogs_free(udm_ue->udr_subs_id);
    udm_ue->udr_subs_id = id ? ogs_strdup(id) : NULL;

    ogs_info("[%s] Subscribed to UDR data changes [%s]",
            udm_ue->supi, id ? id : "Unknown");
}

static bool s_nssai_match(const char *resource_id, ogs_s_nssai_t *s_nssai)
{
    const char *p = NULL, *end = NULL;
    char *value = NULL, *decoded = NULL;
    cJSON *item = NULL;
    OpenAPI_snssai_t *Snssai = NULL;
    bool match = false;

    p = strstr(resource_id, PARAM_SINGLE_NSSAI);
    if (!p)
        return true;

    p += strlen(PARAM_SINGLE_NSSAI);
    end = strchr(p, '&');
    value = end ? ogs_strndup(p, end - p) : ogs_strdup(p);
    ogs_assert(value);

    decoded = ogs_sbi_url_decode(value);
    if (decoded) {
        item = cJSON_Parse(decoded);
        if (item) {
            Snssai = OpenAPI_snssai_parseFromJSON(item);
            if (Snssai) {
                match = Snssai->sst == s_nssai->sst &&
                    ogs_s_nssai_sd_from_string(Snssai->sd).v ==
                        s_nssai->sd.v;
                OpenAPI_snssai_free(Snssai);
            }
            cJSON_Delete(item);
        }
        ogs_free(decoded);
    }
    ogs_free(value);

    return match;
}

static bool dnn_match(const char *path, const char *dnn)
{
    const char *prefix = "/dnnConfigurations/";
    size_t len;

    if (!dnn || !path)
        return true;
    if (strncmp(path, prefix, strlen(prefix)))
        return true;

    path += strlen(prefix);
    len = strlen(dnn);

    return !ogs_strncasecmp(path, dnn, len) &&
        (path[len] == '\0' || path[len] == '/');
}

static void add_notify_item(OpenAPI_list_t *items, udm_ue_t *udm_ue,
        const char *data_set, OpenAPI_list_t *changes, const char *dnn)
{
    ogs_sbi_header_t header;
    OpenAPI_list_t *Changes = NULL;
    OpenAPI_lnode_t *node = NULL;
    OpenAPI_notify_item_t *NotifyItem = NULL;

    Changes = OpenAPI_list_create();
    ogs_assert(Changes);

    OpenAPI_list_for_each(changes, node) {
        OpenAPI_change_item_t *ChangeItem = node->data;

        if (!ChangeItem || !dnn_match(ChangeItem->path, dnn))
            continue;
        OpenAPI_list_add(Changes, OpenAPI_change_item_copy(NULL, ChangeItem));
    }

    if (!Changes->count) {
        OpenAPI_list_free(Changes);
        return;
    }

    memset(&header, 0, sizeof(header));
    header.service.name =
        (char *)OpenAPI_service_name_ToString(OpenAPI_service_name_nudm_sdm);
    header.api.version = (char *)OGS_SBI_API_V2;
    header.resource.component[0] = udm_ue->supi;
    header.resource.component[1] = (char *)data_set;

    NotifyItem = OpenAPI_notify_item_create(
            ogs_sbi_server_uri(ogs_sbi_server_first(), &header), Changes);
    ogs_assert(NotifyItem);

    OpenAPI_list_add(items, NotifyItem);
}

static void notify_items_free(OpenAPI_list_t *items)
{
    OpenAPI_lnode_t *node = NULL;

    OpenAPI_list_for_each(items, node)
        OpenAPI_notify_item_free(node->data);
    OpenAPI_list_free(items);
}

static int modification_notify_cb(
        int status, ogs_sbi_response_t *response, void *data)
{
    if (status != OGS_OK) {
        ogs_log_message(
                status == OGS_DONE ? OGS_LOG_DEBUG : OGS_LOG_WARN, 0,
                "ModificationNotification failed [%d]", status);
        return OGS_ERROR;
    }

    ogs_assert(response);

    if (response->status >= 300)
        ogs_warn("ModificationNotification failed [HTTP %d]",
                response->status);

    ogs_sbi_response_free(response);
    return OGS_OK;
}

static void send_modification_notification(
        udm_sdm_subscription_t *sdm_subscription, OpenAPI_list_t *items)
{
    ogs_sbi_message_t message;
    ogs_sbi_request_t *request = NULL;
    ogs_sbi_client_t *client = NULL;
    OpenAPI_modification_notification_t ModificationNotification;

    OpenAPI_uri_scheme_e scheme = OpenAPI_uri_scheme_NULL;
    char *fqdn = NULL;
    uint16_t fqdn_port = 0;
    ogs_sockaddr_t *addr = NULL, *addr6 = NULL;

    ogs_assert(sdm_subscription->data_change_callback_uri);

    if (ogs_sbi_getaddr_from_uri(&scheme, &fqdn, &fqdn_port, &addr, &addr6,
                sdm_subscription->data_change_callback_uri) == false ||
            scheme == OpenAPI_uri_scheme_NULL) {
        ogs_error("Invalid callbackReference [%s]",
                sdm_subscription->data_change_callback_uri);
        return;
    }

    client = ogs_sbi_client_find(scheme, fqdn, fqdn_port, addr, addr6);
    if (!client)
        client = ogs_sbi_client_add(scheme, fqdn, fqdn_port, addr, addr6);

    ogs_free(fqdn);
    ogs_freeaddrinfo(addr);
    ogs_freeaddrinfo(addr6);

    if (!client) {
        ogs_error("ogs_sbi_client_add() failed");
        return;
    }

    memset(&ModificationNotification, 0, sizeof(ModificationNotification));
    ModificationNotification.notify_items = items;
    ModificationNotification.subscription_id = sdm_subscription->id;

    memset(&message, 0, sizeof(message));
    message.h.method = (char *)OGS_SBI_HTTP_METHOD_POST;
    message.h.uri = sdm_subscription->data_change_callback_uri;
    message.ModificationNotification = &ModificationNotification;

    request = ogs_sbi_build_request(&message);
    if (!request) {
        ogs_error("ogs_sbi_build_request() failed");
        return;
    }

    if (ogs_sbi_send_request_to_client(
                client, modification_notify_cb, request, NULL) == false)
        ogs_error("ogs_sbi_send_request_to_client() failed");

    ogs_sbi_request_free(request);
}

bool udm_nudm_callback_handle_data_change_notify(
        ogs_sbi_stream_t *stream, ogs_sbi_message_t *recvmsg)
{
    ogs_sbi_message_t sendmsg;
    ogs_sbi_response_t *response = NULL;
    OpenAPI_data_change_notify_t *DataChangeNotify = NULL;
    udm_ue_t *udm_ue = NULL;
    udm_sdm_subscription_t *sdm_subscription = NULL;
    char *supi = NULL;

    ogs_assert(stream);
    ogs_assert(recvmsg);

    if (strcmp(recvmsg->h.method, OGS_SBI_HTTP_METHOD_POST)) {
        ogs_assert(true ==
            ogs_sbi_server_send_error(stream,
                OGS_SBI_HTTP_STATUS_METHOD_NOT_ALLOWED, recvmsg,
                "Invalid HTTP method", recvmsg->h.method, NULL));
        return false;
    }

    DataChangeNotify = recvmsg->DataChangeNotify;
    if (!DataChangeNotify || !DataChangeNotify->notify_items) {
        ogs_error("No DataChangeNotify");
        ogs_assert(true ==
            ogs_sbi_server_send_error(stream,
                OGS_SBI_HTTP_STATUS_BAD_REQUEST, recvmsg,
                "No DataChangeNotify", NULL, NULL));
        return false;
    }

    supi = recvmsg->h.resource.component[0];
    if (supi)
        udm_ue = udm_ue_find_by_supi(supi);

    if (!udm_ue || !ogs_list_count(&udm_ue->sdm_subscription_list)) {
        ogs_warn("[%s] No SDM subscription for data change",
                supi ? supi : "Unknown");
        if (udm_ue && udm_ue->udr_subs_id) {
            ogs_free(udm_ue->udr_subs_id);
            udm_ue->udr_subs_id = NULL;
        }
        ogs_assert(true ==
            ogs_sbi_server_send_error(stream,
                OGS_SBI_HTTP_STATUS_NOT_FOUND, recvmsg,
                "No SDM subscription", supi, NULL));
        return false;
    }

    memset(&sendmsg, 0, sizeof(sendmsg));
    response = ogs_sbi_build_response(&sendmsg, OGS_SBI_HTTP_STATUS_NO_CONTENT);
    ogs_assert(response);
    ogs_assert(true == ogs_sbi_server_send_response(stream, response));

    ogs_list_for_each(&udm_ue->sdm_subscription_list, sdm_subscription) {
        OpenAPI_list_t *items = NULL;
        OpenAPI_lnode_t *node = NULL;

        items = OpenAPI_list_create();
        ogs_assert(items);

        OpenAPI_list_for_each(DataChangeNotify->notify_items, node) {
            OpenAPI_notify_item_t *NotifyItem = node->data;

            if (!NotifyItem || !NotifyItem->resource_id)
                continue;

            if (strstr(NotifyItem->resource_id,
                        "/" OGS_SBI_RESOURCE_NAME_AM_DATA)) {
                if (!sdm_subscription->am_data)
                    continue;
                add_notify_item(items, udm_ue, OGS_SBI_RESOURCE_NAME_AM_DATA,
                        NotifyItem->changes, NULL);
            } else if (strstr(NotifyItem->resource_id,
                        "/" OGS_SBI_RESOURCE_NAME_SM_DATA)) {
                if (!sdm_subscription->sm_data)
                    continue;
                if (sdm_subscription->has_s_nssai &&
                    !s_nssai_match(NotifyItem->resource_id,
                        &sdm_subscription->s_nssai))
                    continue;
                add_notify_item(items, udm_ue, OGS_SBI_RESOURCE_NAME_SM_DATA,
                        NotifyItem->changes, sdm_subscription->dnn);
            }
        }

        if (items->count) {
            ogs_info("[%s] Data change notified to [%s]",
                    udm_ue->supi, sdm_subscription->data_change_callback_uri);
            send_modification_notification(sdm_subscription, items);
        }

        notify_items_free(items);
    }

    return true;
}

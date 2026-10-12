
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "data_change_notify.h"

OpenAPI_data_change_notify_t *OpenAPI_data_change_notify_create(
    OpenAPI_list_t *original_callback_reference,
    char *ue_id,
    OpenAPI_list_t *notify_items
)
{
    OpenAPI_data_change_notify_t *data_change_notify_local_var = ogs_malloc(sizeof(OpenAPI_data_change_notify_t));
    ogs_assert(data_change_notify_local_var);

    data_change_notify_local_var->original_callback_reference = original_callback_reference;
    data_change_notify_local_var->ue_id = ue_id;
    data_change_notify_local_var->notify_items = notify_items;

    return data_change_notify_local_var;
}

void OpenAPI_data_change_notify_free(OpenAPI_data_change_notify_t *data_change_notify)
{
    OpenAPI_lnode_t *node = NULL;

    if (NULL == data_change_notify) {
        return;
    }
    if (data_change_notify->original_callback_reference) {
        OpenAPI_list_for_each(data_change_notify->original_callback_reference, node) {
            ogs_free(node->data);
        }
        OpenAPI_list_free(data_change_notify->original_callback_reference);
        data_change_notify->original_callback_reference = NULL;
    }
    if (data_change_notify->ue_id) {
        ogs_free(data_change_notify->ue_id);
        data_change_notify->ue_id = NULL;
    }
    if (data_change_notify->notify_items) {
        OpenAPI_list_for_each(data_change_notify->notify_items, node) {
            OpenAPI_notify_item_free(node->data);
        }
        OpenAPI_list_free(data_change_notify->notify_items);
        data_change_notify->notify_items = NULL;
    }
    ogs_free(data_change_notify);
}

cJSON *OpenAPI_data_change_notify_convertToJSON(OpenAPI_data_change_notify_t *data_change_notify)
{
    cJSON *item = NULL;
    OpenAPI_lnode_t *node = NULL;

    if (data_change_notify == NULL) {
        ogs_error("OpenAPI_data_change_notify_convertToJSON() failed [DataChangeNotify]");
        return NULL;
    }

    item = cJSON_CreateObject();
    if (data_change_notify->original_callback_reference) {
    cJSON *original_callback_referenceList = cJSON_AddArrayToObject(item, "originalCallbackReference");
    if (original_callback_referenceList == NULL) {
        ogs_error("OpenAPI_data_change_notify_convertToJSON() failed [original_callback_reference]");
        goto end;
    }
    OpenAPI_list_for_each(data_change_notify->original_callback_reference, node) {
        if (cJSON_AddStringToObject(original_callback_referenceList, "", (char*)node->data) == NULL) {
            ogs_error("OpenAPI_data_change_notify_convertToJSON() failed [original_callback_reference]");
            goto end;
        }
    }
    }

    if (data_change_notify->ue_id) {
    if (cJSON_AddStringToObject(item, "ueId", data_change_notify->ue_id) == NULL) {
        ogs_error("OpenAPI_data_change_notify_convertToJSON() failed [ue_id]");
        goto end;
    }
    }

    if (data_change_notify->notify_items) {
    cJSON *notify_itemsList = cJSON_AddArrayToObject(item, "notifyItems");
    if (notify_itemsList == NULL) {
        ogs_error("OpenAPI_data_change_notify_convertToJSON() failed [notify_items]");
        goto end;
    }
    OpenAPI_list_for_each(data_change_notify->notify_items, node) {
        cJSON *itemLocal = OpenAPI_notify_item_convertToJSON(node->data);
        if (itemLocal == NULL) {
            ogs_error("OpenAPI_data_change_notify_convertToJSON() failed [notify_items]");
            goto end;
        }
        cJSON_AddItemToArray(notify_itemsList, itemLocal);
    }
    }

end:
    return item;
}

OpenAPI_data_change_notify_t *OpenAPI_data_change_notify_parseFromJSON(cJSON *data_change_notifyJSON)
{
    OpenAPI_data_change_notify_t *data_change_notify_local_var = NULL;
    OpenAPI_lnode_t *node = NULL;
    cJSON *original_callback_reference = NULL;
    OpenAPI_list_t *original_callback_referenceList = NULL;
    cJSON *ue_id = NULL;
    cJSON *notify_items = NULL;
    OpenAPI_list_t *notify_itemsList = NULL;

    original_callback_reference = cJSON_GetObjectItemCaseSensitive(data_change_notifyJSON, "originalCallbackReference");
    if (original_callback_reference) {
        cJSON *original_callback_reference_local = NULL;
        if (!cJSON_IsArray(original_callback_reference)) {
            ogs_error("OpenAPI_data_change_notify_parseFromJSON() failed [original_callback_reference]");
            goto end;
        }

        original_callback_referenceList = OpenAPI_list_create();

        cJSON_ArrayForEach(original_callback_reference_local, original_callback_reference) {
            if (!cJSON_IsString(original_callback_reference_local)) {
                ogs_error("OpenAPI_data_change_notify_parseFromJSON() failed [original_callback_reference]");
                goto end;
            }
            OpenAPI_list_add(original_callback_referenceList, ogs_strdup(original_callback_reference_local->valuestring));
        }
    }

    ue_id = cJSON_GetObjectItemCaseSensitive(data_change_notifyJSON, "ueId");
    if (ue_id) {
    if (!cJSON_IsString(ue_id) && !cJSON_IsNull(ue_id)) {
        ogs_error("OpenAPI_data_change_notify_parseFromJSON() failed [ue_id]");
        goto end;
    }
    }

    notify_items = cJSON_GetObjectItemCaseSensitive(data_change_notifyJSON, "notifyItems");
    if (notify_items) {
        cJSON *notify_items_local = NULL;
        if (!cJSON_IsArray(notify_items)) {
            ogs_error("OpenAPI_data_change_notify_parseFromJSON() failed [notify_items]");
            goto end;
        }

        notify_itemsList = OpenAPI_list_create();

        cJSON_ArrayForEach(notify_items_local, notify_items) {
            if (!cJSON_IsObject(notify_items_local)) {
                ogs_error("OpenAPI_data_change_notify_parseFromJSON() failed [notify_items]");
                goto end;
            }
            OpenAPI_notify_item_t *notify_itemsItem = OpenAPI_notify_item_parseFromJSON(notify_items_local);
            if (!notify_itemsItem) {
                ogs_error("No notify_itemsItem");
                goto end;
            }
            OpenAPI_list_add(notify_itemsList, notify_itemsItem);
        }
    }

    data_change_notify_local_var = OpenAPI_data_change_notify_create (
        original_callback_referenceList,
        ue_id && !cJSON_IsNull(ue_id) ? ogs_strdup(ue_id->valuestring) : NULL,
        notify_itemsList
    );

    return data_change_notify_local_var;
end:
    if (original_callback_referenceList) {
        OpenAPI_list_for_each(original_callback_referenceList, node) {
            ogs_free(node->data);
        }
        OpenAPI_list_free(original_callback_referenceList);
        original_callback_referenceList = NULL;
    }
    if (notify_itemsList) {
        OpenAPI_list_for_each(notify_itemsList, node) {
            OpenAPI_notify_item_free(node->data);
        }
        OpenAPI_list_free(notify_itemsList);
        notify_itemsList = NULL;
    }
    return NULL;
}

OpenAPI_data_change_notify_t *OpenAPI_data_change_notify_copy(OpenAPI_data_change_notify_t *dst, OpenAPI_data_change_notify_t *src)
{
    cJSON *item = NULL;
    char *content = NULL;

    ogs_assert(src);
    item = OpenAPI_data_change_notify_convertToJSON(src);
    if (!item) {
        ogs_error("OpenAPI_data_change_notify_convertToJSON() failed");
        return NULL;
    }

    content = cJSON_Print(item);
    cJSON_Delete(item);

    if (!content) {
        ogs_error("cJSON_Print() failed");
        return NULL;
    }

    item = cJSON_Parse(content);
    ogs_free(content);
    if (!item) {
        ogs_error("cJSON_Parse() failed");
        return NULL;
    }

    OpenAPI_data_change_notify_free(dst);
    dst = OpenAPI_data_change_notify_parseFromJSON(item);
    cJSON_Delete(item);

    return dst;
}


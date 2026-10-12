/*
 * data_change_notify.h
 *
 * Nudr_DataRepository data change notification (TS 29.505)
 */

#ifndef _OpenAPI_data_change_notify_H_
#define _OpenAPI_data_change_notify_H_

#include <string.h>
#include "third-party/cjson/cJSON.h"
#include "../include/list.h"
#include "../include/keyValuePair.h"
#include "../include/binary.h"
typedef struct OpenAPI_data_change_notify_s OpenAPI_data_change_notify_t;
#include "notify_item.h"

#ifdef __cplusplus
extern "C" {
#endif

struct OpenAPI_data_change_notify_s {
    OpenAPI_list_t *original_callback_reference;
    char *ue_id;
    OpenAPI_list_t *notify_items;
};

OpenAPI_data_change_notify_t *OpenAPI_data_change_notify_create(
    OpenAPI_list_t *original_callback_reference,
    char *ue_id,
    OpenAPI_list_t *notify_items
);
void OpenAPI_data_change_notify_free(OpenAPI_data_change_notify_t *data_change_notify);
OpenAPI_data_change_notify_t *OpenAPI_data_change_notify_parseFromJSON(cJSON *data_change_notifyJSON);
cJSON *OpenAPI_data_change_notify_convertToJSON(OpenAPI_data_change_notify_t *data_change_notify);
OpenAPI_data_change_notify_t *OpenAPI_data_change_notify_copy(OpenAPI_data_change_notify_t *dst, OpenAPI_data_change_notify_t *src);

#ifdef __cplusplus
}
#endif

#endif /* _OpenAPI_data_change_notify_H_ */

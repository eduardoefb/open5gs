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

#ifndef UDM_DATA_CHANGE_H
#define UDM_DATA_CHANGE_H

#include "context.h"

#ifdef __cplusplus
extern "C" {
#endif

void udm_sdm_subscription_set_monitored(
        udm_sdm_subscription_t *sdm_subscription,
        OpenAPI_sdm_subscription_t *SDMSubscription);

void udm_nudr_dr_send_subs_to_notify(udm_ue_t *udm_ue);
void udm_nudr_dr_handle_subs_to_notify(
        ogs_pool_id_t udm_ue_id, ogs_sbi_message_t *recvmsg);

bool udm_nudm_callback_handle_data_change_notify(
        ogs_sbi_stream_t *stream, ogs_sbi_message_t *recvmsg);

#ifdef __cplusplus
}
#endif

#endif /* UDM_DATA_CHANGE_H */

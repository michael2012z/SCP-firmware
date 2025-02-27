/*
 * Arm SCP/MCP Software
 * Copyright (c) 2015-2021, Arm Limited and Contributors. All rights reserved.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Description:
 *     SCMI sensor management protocol support.
 */

#include <internal/scmi_sensor.h>

#include <mod_scmi.h>
#include <mod_scmi_sensor.h>
#include <mod_sensor.h>

#include <fwk_assert.h>
#include <fwk_event.h>
#include <fwk_id.h>
#include <fwk_macros.h>
#include <fwk_mm.h>
#include <fwk_module.h>
#include <fwk_module_idx.h>
#include <fwk_status.h>
#include <fwk_thread.h>

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#define MOD_SCMI_SENSOR_NOTIFICATION_COUNT 1

#ifdef BUILD_HAS_MOD_RESOURCE_PERMS
#    include <mod_resource_perms.h>
#endif

struct sensor_operations {
    /*
     * Service identifier currently requesting operation from this sensor.
     * A 'none' value means that there is no pending request.
     */
    fwk_id_t service_id;
};

struct scmi_sensor_ctx {
    /* Number of sensors */
    unsigned int sensor_count;

    /* SCMI protocol module to SCMI module API */
    const struct mod_scmi_from_protocol_api *scmi_api;

    /* Sensor module API */
    const struct mod_sensor_api *sensor_api;

    /* Pointer to a table of sensor operations */
    struct sensor_operations *sensor_ops_table;

#ifdef BUILD_HAS_MOD_RESOURCE_PERMS
    /* SCMI Resource Permissions API */
    const struct mod_res_permissions_api *res_perms_api;
#endif

#ifdef BUILD_HAS_SCMI_NOTIFICATIONS
    /* Number of active agents */
    int agent_count;

    /* SCMI notification API */
    const struct mod_scmi_notification_api *scmi_notification_api;
#endif
};

static int scmi_sensor_protocol_version_handler(fwk_id_t service_id,
    const uint32_t *payload);
static int scmi_sensor_protocol_attributes_handler(fwk_id_t service_id,
    const uint32_t *payload);
static int scmi_sensor_protocol_msg_attributes_handler(fwk_id_t service_id,
    const uint32_t *payload);
static int scmi_sensor_protocol_desc_get_handler(fwk_id_t service_id,
    const uint32_t *payload);
#ifdef BUILD_HAS_SCMI_SENSOR_EVENTS
static int scmi_sensor_trip_point_notify_handler(
    fwk_id_t service_id,
    const uint32_t *payload);
static int scmi_sensor_trip_point_config_handler(
    fwk_id_t service_id,
    const uint32_t *payload);
#endif
static int scmi_sensor_reading_get_handler(fwk_id_t service_id,
    const uint32_t *payload);

struct scmi_sensor_event_parameters {
    fwk_id_t sensor_id;
};

static const fwk_id_t mod_scmi_sensor_event_id_get_request =
    FWK_ID_EVENT_INIT(FWK_MODULE_IDX_SCMI_SENSOR,
                      SCMI_SENSOR_EVENT_IDX_REQUEST);

/*
 * Internal variables.
 */
static struct scmi_sensor_ctx scmi_sensor_ctx;

static int (*handler_table[MOD_SCMI_SENSOR_COMMAND_COUNT])(
    fwk_id_t,
    const uint32_t *) = {
    [MOD_SCMI_PROTOCOL_VERSION] = scmi_sensor_protocol_version_handler,
    [MOD_SCMI_PROTOCOL_ATTRIBUTES] = scmi_sensor_protocol_attributes_handler,
    [MOD_SCMI_PROTOCOL_MESSAGE_ATTRIBUTES] =
        scmi_sensor_protocol_msg_attributes_handler,
    [MOD_SCMI_SENSOR_DESCRIPTION_GET] = scmi_sensor_protocol_desc_get_handler,
#ifdef BUILD_HAS_SCMI_SENSOR_EVENTS
    [MOD_SCMI_SENSOR_TRIP_POINT_NOTIFY] = scmi_sensor_trip_point_notify_handler,
    [MOD_SCMI_SENSOR_TRIP_POINT_CONFIG] = scmi_sensor_trip_point_config_handler,
#endif
    [MOD_SCMI_SENSOR_READING_GET] = scmi_sensor_reading_get_handler
};

static unsigned int payload_size_table[MOD_SCMI_SENSOR_COMMAND_COUNT] = {
    [MOD_SCMI_PROTOCOL_VERSION] = 0,
    [MOD_SCMI_PROTOCOL_ATTRIBUTES] = 0,
    [MOD_SCMI_PROTOCOL_MESSAGE_ATTRIBUTES] =
        (unsigned int)sizeof(struct scmi_protocol_message_attributes_a2p),
    [MOD_SCMI_SENSOR_DESCRIPTION_GET] =
        (unsigned int)sizeof(struct scmi_sensor_protocol_description_get_a2p),
#ifdef BUILD_HAS_SCMI_SENSOR_EVENTS
    [MOD_SCMI_SENSOR_TRIP_POINT_NOTIFY] =
        sizeof(struct scmi_sensor_trip_point_notify_a2p),
    [MOD_SCMI_SENSOR_TRIP_POINT_CONFIG] =
        sizeof(struct scmi_sensor_trip_point_config_a2p),
#endif
    [MOD_SCMI_SENSOR_READING_GET] =
        (unsigned int)sizeof(struct scmi_sensor_protocol_reading_get_a2p),
};

/*
 * Static helper for responding to SCMI.
 */
static void scmi_sensor_respond(
    struct scmi_sensor_protocol_reading_get_p2a *return_values,
    fwk_id_t sensor_id)
{
    unsigned int sensor_idx;
    fwk_id_t service_id;

    /*
     * The service identifier used for the response is retrieved from the
     * sensor operations table.
     */
    sensor_idx = fwk_id_get_element_idx(sensor_id);
    service_id = scmi_sensor_ctx.sensor_ops_table[sensor_idx].service_id;

    scmi_sensor_ctx.scmi_api->respond(service_id,
        return_values,
        (return_values->status == SCMI_SUCCESS) ?
        sizeof(*return_values) : sizeof(return_values->status));

    /*
     * Set the service identifier to 'none' to indicate the sensor is
     * available again.
     */
    scmi_sensor_ctx.sensor_ops_table[sensor_idx].service_id = FWK_ID_NONE;
}

/*
 * Sensor management protocol implementation
 */
static int scmi_sensor_protocol_version_handler(fwk_id_t service_id,
                                                const uint32_t *payload)
{
    struct scmi_protocol_version_p2a return_values = {
        .status = (int32_t)SCMI_SUCCESS,
        .version = SCMI_PROTOCOL_VERSION_SENSOR,
    };

    scmi_sensor_ctx.scmi_api->respond(service_id, &return_values,
                                      sizeof(return_values));

    return FWK_SUCCESS;
}

static int scmi_sensor_protocol_attributes_handler(fwk_id_t service_id,
                                                   const uint32_t *payload)
{
    struct scmi_sensor_protocol_attributes_p2a return_values = {
        .status = (int32_t)SCMI_SUCCESS,
        .attributes = scmi_sensor_ctx.sensor_count,
        .sensor_reg_len = 0, /* Unsupported */
    };

    scmi_sensor_ctx.scmi_api->respond(service_id, &return_values,
                                      sizeof(return_values));

    return FWK_SUCCESS;
}

static int scmi_sensor_protocol_msg_attributes_handler(fwk_id_t service_id,
                                                       const uint32_t *payload)
{
    const struct scmi_protocol_message_attributes_a2p *parameters;
    struct scmi_protocol_message_attributes_p2a return_values;

    parameters = (const struct scmi_protocol_message_attributes_a2p *)
                 payload;

    if ((parameters->message_id < FWK_ARRAY_SIZE(handler_table)) &&
        (handler_table[parameters->message_id] != NULL)) {
        return_values = (struct scmi_protocol_message_attributes_p2a) {
            .status = SCMI_SUCCESS,
            /* All commands have an attributes value of 0 */
            .attributes = 0,
        };
    } else {
        return_values.status = (int32_t)SCMI_NOT_FOUND;
    }

    scmi_sensor_ctx.scmi_api->respond(service_id, &return_values,
        (return_values.status == SCMI_SUCCESS) ?
        sizeof(return_values) : sizeof(return_values.status));

    return FWK_SUCCESS;
}

static int scmi_sensor_protocol_desc_get_handler(fwk_id_t service_id,
                                                 const uint32_t *payload)
{
    int status;
    size_t payload_size;
    size_t max_payload_size;
    const struct scmi_sensor_protocol_description_get_a2p *parameters =
               (const struct scmi_sensor_protocol_description_get_a2p *)payload;
    struct scmi_sensor_desc desc = { 0 };
    unsigned int num_descs, desc_index, desc_index_max;
    struct mod_sensor_scmi_info sensor_info;
    struct scmi_sensor_protocol_description_get_p2a return_values = {
        .status = (int32_t)SCMI_GENERIC_ERROR,
    };
    fwk_id_t sensor_id;

    payload_size = sizeof(return_values);

    status = scmi_sensor_ctx.scmi_api->get_max_payload_size(service_id,
                                                            &max_payload_size);
    if (status != FWK_SUCCESS) {
        goto exit;
    }

    if (SCMI_SENSOR_DESCS_MAX(max_payload_size) == 0) {
        /* Can't even fit one sensor description in the payload */
        status = FWK_E_SIZE;
        goto exit_unexpected;
    }

    parameters =
        (const struct scmi_sensor_protocol_description_get_a2p *)payload;
    desc_index = parameters->desc_index;

    if (desc_index >= scmi_sensor_ctx.sensor_count) {
        return_values.status = (int32_t)SCMI_INVALID_PARAMETERS;
        goto exit;
    }

    num_descs = (unsigned int)FWK_MIN(
        SCMI_SENSOR_DESCS_MAX(max_payload_size),
        (scmi_sensor_ctx.sensor_count - desc_index));
    desc_index_max = (desc_index + num_descs - 1);

    for (; desc_index <= desc_index_max; ++desc_index,
         payload_size += sizeof(desc)) {

        desc = (struct scmi_sensor_desc) {
            .sensor_id = desc_index,
            .sensor_attributes_low = 0, /* None supported */
        };

        sensor_id = FWK_ID_ELEMENT(FWK_MODULE_IDX_SENSOR, desc_index);
        if (!fwk_module_is_valid_element_id(sensor_id)) {
            /* domain_idx did not map to a sensor device */
            return_values.status = (int32_t)SCMI_NOT_FOUND;
            goto exit_unexpected;
        }

        status = scmi_sensor_ctx.sensor_api->get_info(sensor_id, &sensor_info);
        if (status != FWK_SUCCESS) {
            /* Unable to get sensor info */
            goto exit_unexpected;
        }

        if (sensor_info.hal_info.type >= MOD_SENSOR_TYPE_COUNT) {
            /* Invalid sensor type */
            goto exit_unexpected;
        }

        if ((sensor_info.hal_info.unit_multiplier <
             SCMI_SENSOR_DESC_ATTRS_HIGH_SENSOR_UNIT_MULTIPLIER_MIN) ||
            (sensor_info.hal_info.unit_multiplier >
             SCMI_SENSOR_DESC_ATTRS_HIGH_SENSOR_UNIT_MULTIPLIER_MAX)) {
            /* Sensor unit multiplier out of range */
            goto exit_unexpected;
        }

        if ((sensor_info.hal_info.update_interval_multiplier <
             SCMI_SENSOR_DESC_ATTRS_HIGH_SENSOR_UPDATE_MULTIPLIER_MIN) ||
            (sensor_info.hal_info.update_interval_multiplier >
             SCMI_SENSOR_DESC_ATTRS_HIGH_SENSOR_UPDATE_MULTIPLIER_MAX)) {
            /* Sensor update interval multiplier is out of range */
            goto exit_unexpected;
        }

        if (sensor_info.hal_info.update_interval >=
            SCMI_SENSOR_DESC_ATTRS_HIGH_SENSOR_UPDATE_INTERVAL_MASK) {
            /* Update interval is too big to fit in its mask */
            goto exit_unexpected;
        }
        if (sensor_info.trip_point.count >=
            SCMI_SENSOR_DESC_ATTRS_LOW_SENSOR_NUM_TRIP_POINTS_MASK) {
            /* Number of trip points is too big to fit in its mask */
            goto exit_unexpected;
        }

        desc.sensor_attributes_low = SCMI_SENSOR_DESC_ATTRIBUTES_LOW(
            0U, /* Asyncronous reading not-supported */
            (uint32_t)sensor_info.trip_point.count);

        desc.sensor_attributes_high = SCMI_SENSOR_DESC_ATTRIBUTES_HIGH(
            sensor_info.hal_info.type,
            sensor_info.hal_info.unit_multiplier,
            (uint32_t)sensor_info.hal_info.update_interval_multiplier,
            (uint32_t)sensor_info.hal_info.update_interval);

        /*
         * Copy sensor name into description struct. Copy n-1 chars to ensure a
         * NULL terminator at the end. (struct has been zeroed out)
         */
        strncpy(
            desc.sensor_name,
            fwk_module_get_element_name(sensor_id),
            sizeof(desc.sensor_name) - 1);

        status = scmi_sensor_ctx.scmi_api->write_payload(service_id,
            payload_size, &desc, sizeof(struct scmi_sensor_desc));
        if (status != FWK_SUCCESS) {
            /* Failed to write sensor description into message payload */
            goto exit_unexpected;
        }
    }

    return_values = (struct scmi_sensor_protocol_description_get_p2a) {
        .status = SCMI_SUCCESS,
        .num_sensor_flags = SCMI_SENSOR_NUM_SENSOR_FLAGS(num_descs,
            (scmi_sensor_ctx.sensor_count - desc_index_max - 1))
    };

    status = scmi_sensor_ctx.scmi_api->write_payload(service_id, 0,
        &return_values, sizeof(return_values));
    if (status != FWK_SUCCESS) {
        return_values.status = (int32_t)SCMI_GENERIC_ERROR;
    }
    goto exit;

exit_unexpected:
    fwk_unexpected();
exit:
    scmi_sensor_ctx.scmi_api->respond(service_id,
        (return_values.status == SCMI_SUCCESS) ?
            NULL : &return_values.status,
        (return_values.status == SCMI_SUCCESS) ?
            payload_size : sizeof(return_values.status));

    return status;
}

#ifdef BUILD_HAS_SCMI_SENSOR_EVENTS
static int scmi_sensor_trip_point_notify_handler(
    fwk_id_t service_id,
    const uint32_t *payload)
{
    uint32_t flags;
    unsigned int agent_id;
    int status;
    struct scmi_sensor_trip_point_notify_a2p *parameters;
    struct scmi_sensor_trip_point_notify_p2a return_values = {
        .status = SCMI_GENERIC_ERROR,
    };

    parameters = (struct scmi_sensor_trip_point_notify_a2p *)payload;

    if (parameters->sensor_id >= scmi_sensor_ctx.sensor_count) {
        /* Sensor does not exist */
        status = FWK_SUCCESS;
        return_values.status = SCMI_NOT_FOUND;
        goto exit;
    }

    status = scmi_sensor_ctx.scmi_api->get_agent_id(service_id, &agent_id);
    if (status != FWK_SUCCESS)
        goto exit;

    flags = parameters->flags;
    if (flags & SCMI_SENSOR_CONFIG_FLAGS_EVENT_CONTROL_MASK)
        scmi_sensor_ctx.scmi_notification_api->scmi_notification_add_subscriber(
            MOD_SCMI_PROTOCOL_ID_SENSOR,
            parameters->sensor_id,
            MOD_SCMI_SENSOR_TRIP_POINT_NOTIFY,
            service_id);
    else
        scmi_sensor_ctx.scmi_notification_api
            ->scmi_notification_remove_subscriber(
                MOD_SCMI_PROTOCOL_ID_SENSOR,
                agent_id,
                parameters->sensor_id,
                MOD_SCMI_PERF_NOTIFY_LEVEL);

    return_values.status = SCMI_SUCCESS;
    status = FWK_SUCCESS;

exit:
    scmi_sensor_ctx.scmi_api->respond(
        service_id,
        &return_values,
        (return_values.status == SCMI_SUCCESS) ? sizeof(return_values) :
                                                 sizeof(return_values.status));

    return status;
}
#endif

#ifdef BUILD_HAS_SCMI_SENSOR_EVENTS
static int scmi_sensor_trip_point_config_handler(
    fwk_id_t service_id,
    const uint32_t *payload)
{
    struct scmi_sensor_trip_point_config_a2p *parameters;
    struct scmi_sensor_trip_point_config_p2a return_values;
    struct mod_sensor_scmi_info sensor_info;
    struct mod_sensor_trip_point_params trip_point_param;
    fwk_id_t sensor_id;
    uint32_t trip_point_idx;
    int status;

    parameters = (struct scmi_sensor_trip_point_config_a2p *)payload;

    if (parameters->sensor_id >= scmi_sensor_ctx.sensor_count) {
        /* Sensor does not exist */
        status = FWK_SUCCESS;
        return_values.status = SCMI_NOT_FOUND;
        goto exit;
    }
    if ((parameters->flags & SCMI_SENSOR_TRIP_POINT_FLAGS_RESERVED1_MASK) ||
        (parameters->flags & SCMI_SENSOR_TRIP_POINT_FLAGS_RESERVED2_MASK)) {
        status = FWK_SUCCESS;
        return_values.status = SCMI_INVALID_PARAMETERS;
        goto exit;
    }

    sensor_id = FWK_ID_ELEMENT(FWK_MODULE_IDX_SENSOR, parameters->sensor_id);
    status = scmi_sensor_ctx.sensor_api->get_info(sensor_id, &sensor_info);
    if (status != FWK_SUCCESS) {
        /* Unable to get sensor info */
        fwk_unexpected();
        goto exit;
    }
    trip_point_idx =
        (parameters->flags & SCMI_SENSOR_TRIP_POINT_FLAGS_ID_MASK) >>
        SCMI_SENSOR_TRIP_POINT_FLAGS_ID_POS;

    if (trip_point_idx >= sensor_info.trip_point.count) {
        /* Sensor Trip point does not exist */
        status = FWK_SUCCESS;
        return_values.status = SCMI_INVALID_PARAMETERS;
        goto exit;
    }

    trip_point_param.high_value = parameters->sensor_value_high;
    trip_point_param.low_value = parameters->sensor_value_low;
    trip_point_param.mode =
        (parameters->flags & SCMI_SENSOR_TRIP_POINT_FLAGS_EV_CTRL_MASK) >>
        SCMI_SENSOR_TRIP_POINT_FLAGS_EV_CTRL_POS;

    scmi_sensor_ctx.sensor_api->set_trip_point(
        sensor_id, trip_point_idx, &trip_point_param);
    return_values.status = SCMI_SUCCESS;
    status = FWK_SUCCESS;

exit:
    scmi_sensor_ctx.scmi_api->respond(
        service_id,
        &return_values,
        (return_values.status == SCMI_SUCCESS) ? sizeof(return_values) :
                                                 sizeof(return_values.status));

    return status;
}
#endif

static int scmi_sensor_reading_get_handler(fwk_id_t service_id,
                                           const uint32_t *payload)
{
    const struct scmi_sensor_protocol_reading_get_a2p *parameters;
    struct scmi_sensor_protocol_reading_get_p2a return_values;
    struct scmi_sensor_event_parameters *params;
    unsigned int sensor_idx;
    uint32_t flags;
    int status;

    parameters = (const struct scmi_sensor_protocol_reading_get_a2p *)payload;

    if (parameters->sensor_id >= scmi_sensor_ctx.sensor_count) {
        /* Sensor does not exist */
        status = FWK_SUCCESS;
        return_values.status = (int32_t)SCMI_NOT_FOUND;
        goto exit;
    }

    flags = parameters->flags;

    if ((flags & ~SCMI_SENSOR_PROTOCOL_READING_GET_ASYNC_FLAG_MASK) !=
        (uint32_t)0) {
        return_values.status = (int32_t)SCMI_INVALID_PARAMETERS;
        status = FWK_SUCCESS;
        goto exit;
    }

    /* Reject asynchronous read requests for now */
    if ((flags & SCMI_SENSOR_PROTOCOL_READING_GET_ASYNC_FLAG_MASK) !=
        (uint32_t)0) {
        return_values.status = (int32_t)SCMI_NOT_SUPPORTED;
        status = FWK_SUCCESS;
        goto exit;
    }

    sensor_idx = parameters->sensor_id;

    /* Check if there is already a request pending for this sensor */
    if (!fwk_id_is_equal(
            scmi_sensor_ctx.sensor_ops_table[sensor_idx].service_id,
            FWK_ID_NONE)){
        return_values.status = (int32_t)SCMI_BUSY;
        status = FWK_SUCCESS;

        goto exit;
    }

    /* The get_value request is processed within the event being generated */
    struct fwk_event event = {
        .target_id = fwk_module_id_scmi_sensor,
        .id = mod_scmi_sensor_event_id_get_request,
    };

    params = (struct scmi_sensor_event_parameters *)event.params;
    params->sensor_id = FWK_ID_ELEMENT(FWK_MODULE_IDX_SENSOR,
                                       sensor_idx);

    status = fwk_thread_put_event(&event);
    if (status != FWK_SUCCESS) {
        return_values.status = (int32_t)SCMI_GENERIC_ERROR;

        goto exit;
    }

    /* Store service identifier to indicate there is a pending request */
    scmi_sensor_ctx.sensor_ops_table[sensor_idx].service_id = service_id;

    return FWK_SUCCESS;

exit:
    scmi_sensor_ctx.scmi_api->respond(service_id, &return_values,
        (return_values.status == SCMI_SUCCESS) ?
        sizeof(return_values) : sizeof(return_values.status));

    return status;
}

/*
 * SCMI module -> SCMI sensor module interface
 */
static int scmi_sensor_get_scmi_protocol_id(fwk_id_t protocol_id,
                                            uint8_t *scmi_protocol_id)
{
    *scmi_protocol_id = (uint8_t)MOD_SCMI_PROTOCOL_ID_SENSOR;

    return FWK_SUCCESS;
}

/*
 * SCMI Resource Permissions handler
 */
#ifdef BUILD_HAS_MOD_RESOURCE_PERMS
static unsigned int get_sensor_id(const uint32_t *payload)
{
    /*
     * Every SCMI Performance message is formatted with the sensor ID
     * as the first message element. We will use the reading_get
     * message as a basic format to retrieve the sensor ID to avoid
     * unnecessary code.
     */
    const struct scmi_sensor_protocol_reading_get_a2p *parameters =
        (const struct scmi_sensor_protocol_reading_get_a2p *)payload;
    return parameters->sensor_id;
}

static int scmi_sensor_permissions_handler(
    fwk_id_t service_id,
    const uint32_t *payload,
    size_t payload_size,
    unsigned int message_id)
{
    enum mod_res_perms_permissions perms;
    unsigned int agent_id, sensor_id;
    int status;

    status = scmi_sensor_ctx.scmi_api->get_agent_id(service_id, &agent_id);
    if (status != FWK_SUCCESS) {
        return FWK_E_ACCESS;
    }

    if (message_id < 3) {
        perms = scmi_sensor_ctx.res_perms_api->agent_has_protocol_permission(
            agent_id, MOD_SCMI_PROTOCOL_ID_SENSOR);
        if (perms == MOD_RES_PERMS_ACCESS_ALLOWED) {
            return FWK_SUCCESS;
        }
        return FWK_E_ACCESS;
    }

    sensor_id = get_sensor_id(payload);
    if (sensor_id >= scmi_sensor_ctx.sensor_count) {
        return FWK_E_PARAM;
    }

    perms = scmi_sensor_ctx.res_perms_api->agent_has_resource_permission(
        agent_id, MOD_SCMI_PROTOCOL_ID_SENSOR, message_id, sensor_id);

    if (perms == MOD_RES_PERMS_ACCESS_ALLOWED) {
        return FWK_SUCCESS;
    } else {
        return FWK_E_ACCESS;
    }
}
#endif

static int scmi_sensor_message_handler(fwk_id_t protocol_id,
                                       fwk_id_t service_id,
                                       const uint32_t *payload,
                                       size_t payload_size,
                                       unsigned int message_id)
{
    int32_t return_value;
#ifdef BUILD_HAS_MOD_RESOURCE_PERMS
    int status;
#endif

    static_assert(FWK_ARRAY_SIZE(handler_table) ==
        FWK_ARRAY_SIZE(payload_size_table),
        "[SCMI] Sensor management protocol table sizes not consistent");
    fwk_assert(payload != NULL);

    if (message_id >= FWK_ARRAY_SIZE(handler_table)) {
        return_value = (int32_t)SCMI_NOT_FOUND;
        goto error;
    }

    if (payload_size != payload_size_table[message_id]) {
        /* Incorrect payload size or message is not supported */
        return_value = (int32_t)SCMI_PROTOCOL_ERROR;
        goto error;
    }

#ifdef BUILD_HAS_MOD_RESOURCE_PERMS
    status = scmi_sensor_permissions_handler(
        service_id, payload, payload_size, message_id);
    if (status != FWK_SUCCESS) {
        if (status == FWK_E_ACCESS) {
            return_value = (int32_t)SCMI_DENIED;
        } else if (message_id == MOD_SCMI_SENSOR_DESCRIPTION_GET) {
            return_value = (int32_t)SCMI_INVALID_PARAMETERS;
        } else {
            return_value = (int32_t)SCMI_NOT_FOUND;
        }
        goto error;
    }
#endif

    return handler_table[message_id](service_id, payload);

error:
    scmi_sensor_ctx.scmi_api->respond(service_id, &return_value,
                                      sizeof(return_value));
    return FWK_SUCCESS;
}

static struct mod_scmi_to_protocol_api scmi_sensor_mod_scmi_to_protocol_api = {
    .get_scmi_protocol_id = scmi_sensor_get_scmi_protocol_id,
    .message_handler = scmi_sensor_message_handler
};

static void scmi_sensor_notify_trip_point(
    fwk_id_t sensor_id,
    uint32_t state,
    uint32_t trip_point_idx)
{
#ifdef BUILD_HAS_SCMI_NOTIFICATIONS
    struct scmi_sensor_trip_point_event_p2a trip_point_event;
    trip_point_event.sensor_id = fwk_id_get_element_idx(sensor_id);
    trip_point_event.agent_id = 0x0;
    trip_point_event.trip_point_desc =
        SCMI_SENSOR_TRIP_POINT_EVENT_DESC(state, trip_point_idx);

    scmi_sensor_ctx.scmi_notification_api->scmi_notification_notify(
        MOD_SCMI_PROTOCOL_ID_SENSOR,
        MOD_SCMI_SENSOR_TRIP_POINT_NOTIFY,
        SCMI_SENSOR_TRIP_POINT_EVENT,
        &trip_point_event,
        sizeof(trip_point_event));
#endif
}
static struct mod_sensor_trip_point_api sensor_trip_point_api = {
    .notify_sensor_trip_point = scmi_sensor_notify_trip_point
};

/*
 * Framework interface
 */
static int scmi_sensor_init(fwk_id_t module_id,
                            unsigned int element_count,
                            const void *unused)
{
    if (element_count != 0) {
        /* This module should not have any elements */
        fwk_unexpected();
        return FWK_E_SUPPORT;
    }

    scmi_sensor_ctx.sensor_count = (unsigned int)fwk_module_get_element_count(
        FWK_ID_MODULE(FWK_MODULE_IDX_SENSOR));
    if (scmi_sensor_ctx.sensor_count == 0) {
        return FWK_E_SUPPORT;
    }

    /* SCMI protocol uses a 16 bit number to store the number of sensors.
     * So expose no more than 0xFFFF number of sensors. */
    if (scmi_sensor_ctx.sensor_count > UINT16_MAX) {
        scmi_sensor_ctx.sensor_count = UINT16_MAX;
    }

    /* Allocate a table for the sensors state */
    scmi_sensor_ctx.sensor_ops_table =
        fwk_mm_calloc(scmi_sensor_ctx.sensor_count,
        sizeof(struct sensor_operations));

    /* Initialize the service identifier for each sensor to 'available' */
    for (unsigned int i = 0; i < scmi_sensor_ctx.sensor_count; i++) {
        scmi_sensor_ctx.sensor_ops_table[i].service_id = FWK_ID_NONE;
    }

    return FWK_SUCCESS;
}

#ifdef BUILD_HAS_SCMI_NOTIFICATIONS
static int scmi_init_notifications(int sensor_count)
{
    int status;

    status =
        scmi_sensor_ctx.scmi_api->get_agent_count(&scmi_sensor_ctx.agent_count);
    if (status != FWK_SUCCESS) {
        return status;
    }

    fwk_assert(scmi_sensor_ctx.agent_count != 0);

    status = scmi_sensor_ctx.scmi_notification_api->scmi_notification_init(
        MOD_SCMI_PROTOCOL_ID_SENSOR,
        scmi_sensor_ctx.agent_count,
        scmi_sensor_ctx.sensor_count,
        MOD_SCMI_SENSOR_NOTIFICATION_COUNT);

    return status;
}
#endif

static int scmi_sensor_bind(fwk_id_t id, unsigned int round)
{
    int status;

    if (round == 1) {
        return FWK_SUCCESS;
    }

    status = fwk_module_bind(FWK_ID_MODULE(FWK_MODULE_IDX_SCMI),
                             FWK_ID_API(FWK_MODULE_IDX_SCMI,
                                        MOD_SCMI_API_IDX_PROTOCOL),
                             &scmi_sensor_ctx.scmi_api);
    if (status != FWK_SUCCESS) {
        /* Failed to bind to SCMI module */
        fwk_unexpected();
        return status;
    }

    status = fwk_module_bind(FWK_ID_MODULE(FWK_MODULE_IDX_SENSOR),
                             mod_sensor_api_id_sensor,
                             &scmi_sensor_ctx.sensor_api);
    if (status != FWK_SUCCESS) {
        /* Failed to bind to sensor module */
        fwk_unexpected();
        return status;
    }

#ifdef BUILD_HAS_MOD_RESOURCE_PERMS
    status = fwk_module_bind(
        FWK_ID_MODULE(FWK_MODULE_IDX_RESOURCE_PERMS),
        FWK_ID_API(FWK_MODULE_IDX_RESOURCE_PERMS, MOD_RES_PERM_RESOURCE_PERMS),
        &scmi_sensor_ctx.res_perms_api);
    if (status != FWK_SUCCESS) {
        return status;
    }
#endif

#ifdef BUILD_HAS_SCMI_NOTIFICATIONS
    status = fwk_module_bind(
        FWK_ID_MODULE(FWK_MODULE_IDX_SCMI),
        FWK_ID_API(FWK_MODULE_IDX_SCMI, MOD_SCMI_API_IDX_NOTIFICATION),
        &scmi_sensor_ctx.scmi_notification_api);
    if (status != FWK_SUCCESS) {
        return status;
    }
#endif

    return FWK_SUCCESS;
}

static int scmi_sensor_start(fwk_id_t id)
{
    int status = FWK_SUCCESS;

#ifdef BUILD_HAS_SCMI_NOTIFICATIONS
    status = scmi_init_notifications((int)scmi_sensor_ctx.sensor_count);
    if (status != FWK_SUCCESS) {
        return status;
    }
#endif

    return status;
}

static int scmi_sensor_process_bind_request(fwk_id_t source_id,
                                            fwk_id_t target_id,
                                            fwk_id_t api_id,
                                            const void **api)
{
    enum scmi_sensor_api_idx api_id_type =
        (enum scmi_sensor_api_idx)fwk_id_get_api_idx(api_id);

    switch (api_id_type) {
    case SCMI_SENSOR_API_IDX_REQUEST:
        if (!fwk_id_is_equal(source_id, FWK_ID_MODULE(FWK_MODULE_IDX_SCMI))) {
            return FWK_E_ACCESS;
        }
        *api = &scmi_sensor_mod_scmi_to_protocol_api;
        break;

    case SCMI_SENSOR_API_IDX_TRIP_POINT:
        *api = &sensor_trip_point_api;
        break;

    default:
        return FWK_E_ACCESS;
    }
    return FWK_SUCCESS;
}

static int scmi_sensor_process_event(const struct fwk_event *event,
                                     struct fwk_event *resp_event)
{
    int status;
    uint64_t sensor_value;
    struct scmi_sensor_event_parameters *scmi_params;
    struct scmi_sensor_protocol_reading_get_p2a return_values;

    /* Request event to sensor HAL */
    if (fwk_id_is_equal(event->id, mod_scmi_sensor_event_id_get_request)) {
        scmi_params = (struct scmi_sensor_event_parameters *)event->params;

        status = scmi_sensor_ctx.sensor_api->get_value(
            scmi_params->sensor_id, &sensor_value);
        if (status == FWK_SUCCESS) {
            /* Sensor value is ready */
            return_values = (struct scmi_sensor_protocol_reading_get_p2a) {
                .status = SCMI_SUCCESS,
                .sensor_value_low = (uint32_t)sensor_value,
                .sensor_value_high = (uint32_t)(sensor_value >> 32),
            };

            scmi_sensor_respond(&return_values, scmi_params->sensor_id);

            return status;
        } else if (status == FWK_PENDING) {
            /* Sensor value will be provided through a response event */
            return FWK_SUCCESS;
        } else {
            return_values = (struct scmi_sensor_protocol_reading_get_p2a) {
                .status = SCMI_HARDWARE_ERROR,
            };

            scmi_sensor_respond(&return_values, scmi_params->sensor_id);

            return FWK_E_PANIC;
        }
    }

    /* Response event from sensor HAL */
    if (fwk_id_is_equal(event->id, mod_sensor_event_id_read_request)) {
        struct mod_sensor_event_params *params =
            (struct mod_sensor_event_params *)event->params;

        return_values = (struct scmi_sensor_protocol_reading_get_p2a) {
            .sensor_value_low = (uint32_t)params->value,
            .sensor_value_high = (uint32_t)(params->value >> 32),
        };

        if (params->status == FWK_SUCCESS) {
            return_values.status = (int32_t)SCMI_SUCCESS;
        } else {
            return_values.status = (int32_t)SCMI_HARDWARE_ERROR;
        }

        scmi_sensor_respond(&return_values, event->source_id);
    }

    return FWK_SUCCESS;
}

const struct fwk_module module_scmi_sensor = {
    .api_count = (unsigned int)SCMI_SENSOR_API_IDX_COUNT,
    .event_count = (unsigned int)SCMI_SENSOR_EVENT_IDX_COUNT,
    .type = FWK_MODULE_TYPE_PROTOCOL,
    .init = scmi_sensor_init,
    .bind = scmi_sensor_bind,
    .start = scmi_sensor_start,
    .process_bind_request = scmi_sensor_process_bind_request,
    .process_event = scmi_sensor_process_event,
};

/* No elements, no module configuration data */
struct fwk_module_config config_scmi_sensor = { 0 };

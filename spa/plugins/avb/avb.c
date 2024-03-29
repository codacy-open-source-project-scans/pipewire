/* Spa AVB support */
/* SPDX-FileCopyrightText: Copyright © 2022 Wim Taymans */
/* SPDX-License-Identifier: MIT */

#include <errno.h>

#include <spa/support/plugin.h>
#include <spa/support/log.h>

#include "avb.h"

extern const struct spa_handle_factory spa_avb_sink_factory;
extern const struct spa_handle_factory spa_avb_source_factory;

SPA_LOG_TOPIC_DEFINE(avb_log_topic, "spa.avb");

SPA_LOG_TOPIC_ENUM_DEFINE_REGISTERED;

SPA_EXPORT
int spa_handle_factory_enum(const struct spa_handle_factory **factory, uint32_t *index)
{
	spa_return_val_if_fail(factory != NULL, -EINVAL);
	spa_return_val_if_fail(index != NULL, -EINVAL);

	switch (*index) {
	case 0:
		*factory = &spa_avb_sink_factory;
		break;
	case 1:
		*factory = &spa_avb_source_factory;
		break;
	default:
		return 0;
	}
	(*index)++;
	return 1;
}

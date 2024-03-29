/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2020 Wim Taymans */
/* SPDX-License-Identifier: MIT */

#include <stdio.h>
#include <signal.h>
#include <math.h>
#include <getopt.h>
#include <locale.h>

#include <spa/utils/result.h>
#include <spa/utils/string.h>
#include <spa/utils/defs.h>

#include <pipewire/pipewire.h>
#include <pipewire/filter.h>
#include <pipewire/extensions/metadata.h>

struct data {
	struct pw_main_loop *loop;

	const char *opt_remote;
	const char *opt_name;
	bool opt_list;
	bool opt_monitor;
	bool opt_delete;
	uint32_t opt_id;
	const char *opt_key;
	const char *opt_value;
	const char *opt_type;

	struct pw_context *context;

	struct pw_core *core;
	struct spa_hook core_listener;

	struct pw_registry *registry;
	struct spa_hook registry_listener;

	struct pw_metadata *metadata;
	struct spa_hook metadata_listener;

	int sync;
};


static int metadata_property(void *data, uint32_t id,
		const char *key, const char *type, const char *value)
{
	struct data *d = data;

	if (d->opt_list)
		return 0;

	if ((d->opt_id == SPA_ID_INVALID || d->opt_id == id) &&
	    (d->opt_key == NULL || spa_streq(d->opt_key, key))) {
		if (key == NULL) {
			printf("remove: id:%u all keys\n", id);
		} else if (value == NULL) {
			printf("remove: id:%u key:'%s'\n", id, key);
		} else {
			printf("update: id:%u key:'%s' value:'%s' type:'%s'\n", id, key, value, type);
		}
	}

	return 0;
}

static const struct pw_metadata_events metadata_events = {
	PW_VERSION_METADATA_EVENTS,
	.property = metadata_property
};

static void registry_event_global(void *data, uint32_t id, uint32_t permissions,
				  const char *type, uint32_t version,
				  const struct spa_dict *props)
{
	struct data *d = data;
	const char *name;

	if (!spa_streq(type, PW_TYPE_INTERFACE_Metadata))
		return;

	if (props == NULL)
		return;

	name = spa_dict_lookup(props, PW_KEY_METADATA_NAME);
	if (name == NULL)
		return;

	if (d->opt_name && !spa_streq(name, d->opt_name))
		return;

	if (!d->opt_list && d->metadata != NULL) {
		pw_log_warn("Multiple metadata: ignoring metadata %d", id);
		return;
	}

	printf("Found \"%s\" metadata %d\n", name, id);
	if (d->opt_list)
		return;

	d->metadata = pw_registry_bind(d->registry,
			id, type, PW_VERSION_METADATA, 0);

	if (d->opt_delete) {
		if (d->opt_id != SPA_ID_INVALID) {
			if (d->opt_key != NULL)
				printf("delete property: id:%u key:%s\n", d->opt_id, d->opt_key);
			else
				printf("delete properties: id:%u\n", d->opt_id);
			pw_metadata_set_property(d->metadata, d->opt_id, d->opt_key, NULL, NULL);
		} else {
			printf("delete all properties\n");
			pw_metadata_clear(d->metadata);
		}
	} else if (d->opt_id != SPA_ID_INVALID && d->opt_key != NULL && d->opt_value != NULL) {
		printf("set property: id:%u key:%s value:%s type:%s\n",
				d->opt_id, d->opt_key, d->opt_value, d->opt_type);
		pw_metadata_set_property(d->metadata, d->opt_id, d->opt_key, d->opt_type, d->opt_value);
	} else {
		pw_metadata_add_listener(d->metadata,
				&d->metadata_listener,
				&metadata_events, d);
	}

	d->sync = pw_core_sync(d->core, PW_ID_CORE, d->sync);
}


static const struct pw_registry_events registry_events = {
	PW_VERSION_REGISTRY_EVENTS,
	.global = registry_event_global,
};

static void on_core_done(void *data, uint32_t id, int seq)
{
	struct data *d = data;
	if (d->sync == seq && !d->opt_monitor)
		pw_main_loop_quit(d->loop);
}

static void on_core_error(void *data, uint32_t id, int seq, int res, const char *message)
{
	struct data *d = data;

	pw_log_error("error id:%u seq:%d res:%d (%s): %s",
			id, seq, res, spa_strerror(res), message);

	if (id == PW_ID_CORE && res == -EPIPE)
		pw_main_loop_quit(d->loop);
}

static const struct pw_core_events core_events = {
	PW_VERSION_CORE_EVENTS,
	.done = on_core_done,
	.error = on_core_error,
};

static void do_quit(void *userdata, int signal_number)
{
	struct data *data = userdata;
	pw_main_loop_quit(data->loop);
}

static void show_help(struct data *data, const char *name, bool error)
{
	fprintf(error ? stderr : stdout, "%s [options] [ id [ key [ value [ type ] ] ] ]\n"
		"  -h, --help                            Show this help\n"
		"      --version                         Show version\n"
		"  -r, --remote                          Remote daemon name\n"
		"  -l, --list                            List available metadata\n"
		"  -m, --monitor                         Monitor metadata\n"
		"  -d, --delete                          Delete metadata\n"
		"  -n, --name                            Metadata name (default: \"%s\")\n",
		name, data->opt_name);
}

int main(int argc, char *argv[])
{
	struct data data = { 0, };
	int res = 0, c;
	static const struct option long_options[] = {
		{ "help",	no_argument,		NULL, 'h' },
		{ "version",	no_argument,		NULL, 'V' },
		{ "remote",	required_argument,	NULL, 'r' },
		{ "list",	no_argument,		NULL, 'l' },
		{ "monitor",	no_argument,		NULL, 'm' },
		{ "delete",	no_argument,		NULL, 'd' },
		{ "name",	required_argument,	NULL, 'n' },
		{ NULL,	0, NULL, 0}
	};

	setlinebuf(stdout);

	setlocale(LC_ALL, "");
	pw_init(&argc, &argv);

	data.opt_name = "default";

	while ((c = getopt_long(argc, argv, "hVr:lmdn:", long_options, NULL)) != -1) {
		switch (c) {
		case 'h':
			show_help(&data, argv[0], false);
			return 0;
		case 'V':
			printf("%s\n"
				"Compiled with libpipewire %s\n"
				"Linked with libpipewire %s\n",
				argv[0],
				pw_get_headers_version(),
				pw_get_library_version());
			return 0;
		case 'r':
			data.opt_remote = optarg;
			break;
		case 'l':
			data.opt_name = NULL;
			data.opt_list = true;
			break;
		case 'm':
			data.opt_monitor = true;
			break;
		case 'd':
			data.opt_delete = true;
			break;
		case 'n':
			data.opt_name = optarg;
			break;
		default:
			show_help(&data, argv[0], true);
			return -1;
		}
	}

	data.opt_id = SPA_ID_INVALID;
	if (optind < argc)
		data.opt_id = atoi(argv[optind++]);
	if (optind < argc)
		data.opt_key = argv[optind++];
	if (optind < argc)
		data.opt_value = argv[optind++];
	if (optind < argc)
		data.opt_type = argv[optind++];

	data.loop = pw_main_loop_new(NULL);
	if (data.loop == NULL) {
		fprintf(stderr, "can't create mainloop: %m\n");
		return -1;
	}
	pw_loop_add_signal(pw_main_loop_get_loop(data.loop), SIGINT, do_quit, &data);
	pw_loop_add_signal(pw_main_loop_get_loop(data.loop), SIGTERM, do_quit, &data);

	data.context = pw_context_new(pw_main_loop_get_loop(data.loop), NULL, 0);
	if (data.context == NULL) {
		fprintf(stderr, "can't create context: %m\n");
		return -1;
	}

	data.core = pw_context_connect(data.context,
			pw_properties_new(
				PW_KEY_REMOTE_NAME, data.opt_remote,
				NULL),
			0);
	if (data.core == NULL) {
		fprintf(stderr, "can't connect: %m\n");
		return -1;
	}

	pw_core_add_listener(data.core,
			&data.core_listener,
			&core_events, &data);

	data.registry = pw_core_get_registry(data.core,
			PW_VERSION_REGISTRY, 0);
	pw_registry_add_listener(data.registry,
			&data.registry_listener,
			&registry_events, &data);

	data.sync = pw_core_sync(data.core, PW_ID_CORE, data.sync);

	pw_main_loop_run(data.loop);

	if (data.metadata)
		pw_proxy_destroy((struct pw_proxy*)data.metadata);
	spa_hook_remove(&data.registry_listener);
	pw_proxy_destroy((struct pw_proxy*)data.registry);
	spa_hook_remove(&data.core_listener);
	pw_core_disconnect(data.core);
	pw_context_destroy(data.context);
	pw_main_loop_destroy(data.loop);
	pw_deinit();

	return res;
}

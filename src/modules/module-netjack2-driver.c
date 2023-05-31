/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2021 Wim Taymans */
/* SPDX-License-Identifier: MIT */

#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>
#include <signal.h>
#include <limits.h>
#include <arpa/inet.h>
#include <netinet/ip.h>
#include <netinet/in.h>
#include <net/if.h>
#include <math.h>

#include "config.h"

#include <spa/utils/result.h>
#include <spa/utils/string.h>
#include <spa/utils/json.h>
#include <spa/debug/types.h>
#include <spa/debug/mem.h>
#include <spa/pod/builder.h>
#include <spa/param/audio/format-utils.h>
#include <spa/param/latency-utils.h>
#include <spa/param/audio/raw.h>

#include <pipewire/impl.h>
#include <pipewire/i18n.h>
#include <pipewire/private.h>
#include <pipewire/thread.h>

#include "module-netjack2/packets.h"

#ifndef IPTOS_DSCP
#define IPTOS_DSCP_MASK 0xfc
#define IPTOS_DSCP(x) ((x) & IPTOS_DSCP_MASK)
#endif

/** \page page_module_netjack2_driver PipeWire Module: Netjack2 driver
 *
 * The netjack2-driver module provides a source or sink that is following a
 * netjack2 driver.
 *
 * ## Module Options
 *
 * - `driver.mode`: the tunnel mode, sink|source|duplex, default duplex
 * - `jack.client-name`: the name of the JACK client.
 * - `local.ifname = <str>`: interface name to use
 * - `net.ip =<str>`: multicast IP address, default "225.3.19.154"
 * - `net.port =<int>`: control port, default "19000"
 * - `net.mtu = <int>`: MTU to use, default 1500
 * - `net.ttl = <int>`: TTL to use, default 1
 * - `net.loop = <bool>`: loopback multicast, default false
 * - `jack.connect`: if jack ports should be connected automatically. Can also be
 *                   placed per stream.
 * - `audio.channels`: the number of audio ports. Can also be added to the stream props.
 * - `midi.ports`: the number of midi ports. Can also be added to the stream props.
 * - `source.props`: Extra properties for the source filter.
 * - `sink.props`: Extra properties for the sink filter.
 *
 * ## General options
 *
 * Options with well-known behavior.
 *
 * - \ref PW_KEY_REMOTE_NAME
 * - \ref PW_KEY_AUDIO_CHANNELS
 * - \ref SPA_KEY_AUDIO_POSITION
 * - \ref PW_KEY_NODE_NAME
 * - \ref PW_KEY_NODE_DESCRIPTION
 * - \ref PW_KEY_NODE_GROUP
 * - \ref PW_KEY_NODE_VIRTUAL
 * - \ref PW_KEY_MEDIA_CLASS
 * - \ref PW_KEY_TARGET_OBJECT to specify the remote node.name or serial.id to link to
 *
 * ## Example configuration of a duplex sink/source
 *
 *\code{.unparsed}
 * context.modules = [
 * {   name = libpipewire-module-jack-tunnel
 *     args = {
 *         #driver.mode      = duplex
 *         #jack.client-name = PipeWire
 *         #jack.connect     = true
 *         #midi.ports       = 0
 *         #audio.channels   = 2
 *         #audio.position   = [ FL FR ]
 *         source.props = {
 *             # extra sink properties
 *         }
 *         sink.props = {
 *             # extra sink properties
 *         }
 *     }
 * }
 * ]
 *\endcode
 */

#define NAME "netjack2-driver"

PW_LOG_TOPIC_STATIC(mod_topic, "mod." NAME);
#define PW_LOG_TOPIC_DEFAULT mod_topic

#define MAX_PORTS	128

#define DEFAULT_NET_IP		"225.3.19.154"
#define DEFAULT_NET_PORT	19000
#define DEFAULT_NET_TTL		1
#define DEFAULT_NET_MTU		1500
#define DEFAULT_NET_LOOP	true
#define DEFAULT_NET_DSCP	34 /* Default to AES-67 AF41 (34) */
#define MAX_MTU			9000

#define NETWORK_DEFAULT_LATENCY	5
#define NETWORK_MAX_LATENCY	30

#define DEFAULT_CLIENT_NAME	"PipeWire"
#define DEFAULT_CHANNELS	2
#define DEFAULT_POSITION	"[ FL FR ]"
#define DEFAULT_MIDI_PORTS	1

#define NETWORK_PROTOCOL 8

#define FOLLOWER_INIT_TIMEOUT	1
#define FOLLOWER_INIT_RETRY	5

#define MODULE_USAGE	"( remote.name=<remote> ) "				\
			"( driver.mode=<sink|source|duplex> ) "			\
			"( local.ifname=<interface name> ) "			\
			"( net.ip=<ip address to use, default 225.3.19.154> ) "	\
			"( net.port=<port to use, default 19000> ) "		\
			"( net.mtu=<MTU to use, default 1500> ) "		\
			"( net.ttl=<TTL to use, default 1> ) "			\
			"( net.loop=<loopback, default false> ) "		\
			"( jack.client-name=<name of the JACK client> ) "	\
			"( jack.connect=<bool, autoconnect ports> ) "		\
			"( midi.ports=<number of midi ports> ) "		\
			"( audio.channels=<number of channels> ) "		\
			"( audio.position=<channel map> ) "			\
			"( source.props=<properties> ) "			\
			"( sink.props=<properties> ) "


static const struct spa_dict_item module_props[] = {
	{ PW_KEY_MODULE_AUTHOR, "Wim Taymans <wim.taymans@gmail.com>" },
	{ PW_KEY_MODULE_DESCRIPTION, "Create a netjack2 driver" },
	{ PW_KEY_MODULE_USAGE, MODULE_USAGE },
	{ PW_KEY_MODULE_VERSION, PACKAGE_VERSION },
};

struct port {
	enum spa_direction direction;
	struct spa_latency_info latency[2];
	bool latency_changed[2];
	unsigned int is_midi:1;
};

struct volume {
	bool mute;
	uint32_t n_volumes;
	float volumes[SPA_AUDIO_MAX_CHANNELS];
};

struct stream {
	struct impl *impl;

	enum spa_direction direction;
	struct pw_properties *props;
	struct pw_filter *filter;
	struct spa_hook listener;

	struct spa_audio_info_raw info;

	uint32_t n_midi;
	uint32_t n_ports;
	struct port *ports[MAX_PORTS];

	struct volume volume;

	uint32_t active_audio_ports;
	uint32_t active_midi_ports;

	unsigned int running:1;
};

struct impl {
	struct pw_context *context;
	struct pw_loop *main_loop;
	struct pw_data_loop *data_loop;
	struct spa_system *system;

#define MODE_SINK	(1<<0)
#define MODE_SOURCE	(1<<1)
#define MODE_DUPLEX	(MODE_SINK|MODE_SOURCE)
	uint32_t mode;
	struct pw_properties *props;

	struct pw_impl_module *module;

	struct spa_hook module_listener;

	struct pw_core *core;
	struct spa_hook core_proxy_listener;
	struct spa_hook core_listener;

	struct spa_io_position *position;

	struct stream source;
	struct stream sink;

	uint32_t samplerate;
	uint64_t frame_time;

	uint32_t pw_xrun;
	uint32_t nj2_xrun;

	pthread_t thread;

	struct sockaddr_storage dst_addr;
	socklen_t dst_len;
	struct sockaddr_storage src_addr;
	socklen_t src_len;

	struct spa_source *setup_socket;
	struct spa_source *socket;
	struct spa_source *timer;
	uint32_t init_retry;

	struct nj2_session_params params;
	struct nj2_packet_header sync;
	uint8_t send_buffer[MAX_MTU];
	uint8_t recv_buffer[MAX_MTU];

	uint32_t driving;

	unsigned int triggered:1;
	unsigned int do_disconnect:1;
	unsigned int done:1;
	unsigned int new_xrun:1;
	unsigned int fix_midi:1;
};

static void reset_volume(struct volume *vol, uint32_t n_volumes)
{
	uint32_t i;
	vol->mute = false;
	vol->n_volumes = n_volumes;
	for (i = 0; i < n_volumes; i++)
		vol->volumes[i] = 1.0f;
}

static inline void do_volume(float *dst, const float *src, struct volume *vol, uint32_t ch, uint32_t n_samples)
{
	float v = vol->mute ? 0.0f : vol->volumes[ch];

	if (v == 0.0f || src == NULL)
		memset(dst, 0, n_samples * sizeof(float));
	else if (v == 1.0f)
		memcpy(dst, src, n_samples * sizeof(float));
	else {
		uint32_t i;
		for (i = 0; i < n_samples; i++)
			dst[i] = src[i] * v;
	}
}

static inline void fix_midi_event(uint8_t *data, size_t size)
{
	/* fixup NoteOn with vel 0 */
	if (size > 2 && (data[0] & 0xF0) == 0x90 && data[2] == 0x00) {
		data[0] = 0x80 + (data[0] & 0x0F);
		data[2] = 0x40;
	}
}

static void midi_to_netjack2(struct impl *impl, float *dst, float *src, uint32_t n_samples)
{
	struct spa_pod *pod;
	struct spa_pod_sequence *seq;
	struct spa_pod_control *c;

	if (src == NULL)
		return;

	if ((pod = spa_pod_from_data(src, n_samples * sizeof(float), 0, n_samples * sizeof(float))) == NULL)
		return;
	if (!spa_pod_is_sequence(pod))
		return;

	seq = (struct spa_pod_sequence*)pod;

	SPA_POD_SEQUENCE_FOREACH(seq, c) {
		switch(c->type) {
		case SPA_CONTROL_Midi:
		{
			uint8_t *data = SPA_POD_BODY(&c->value);
			size_t size = SPA_POD_BODY_SIZE(&c->value);

			if (impl->fix_midi)
				fix_midi_event(data, size);

			break;
		}
		default:
			break;
		}
	}
}

static void netjack2_to_midi(float *dst, float *src, uint32_t size)
{
	struct spa_pod_builder b = { 0, };
	uint32_t i, count;
	struct spa_pod_frame f;

	count = 0;

	spa_pod_builder_init(&b, dst, size);
	spa_pod_builder_push_sequence(&b, &f, 0);
	for (i = 0; i < count; i++) {
	}
	spa_pod_builder_pop(&b, &f);
}

static void stream_destroy(void *d)
{
	struct stream *s = d;
	spa_hook_remove(&s->listener);
	s->filter = NULL;
}

static void stream_state_changed(void *d, enum pw_filter_state old,
		enum pw_filter_state state, const char *error)
{
	struct stream *s = d;
	struct impl *impl = s->impl;
	switch (state) {
	case PW_FILTER_STATE_ERROR:
	case PW_FILTER_STATE_UNCONNECTED:
		pw_impl_module_schedule_destroy(impl->module);
		break;
	case PW_FILTER_STATE_PAUSED:
		s->running = false;
		break;
	case PW_FILTER_STATE_STREAMING:
		s->running = true;
		break;
	default:
		break;
	}
}

static int32_t netjack2_sync_wait(struct impl *impl)
{
	struct nj2_packet_header *sync = (struct nj2_packet_header *)impl->recv_buffer;
	ssize_t len;

	while (true) {
		if ((len = recv(impl->socket->fd, impl->recv_buffer, impl->params.mtu, 0)) < 0)
			goto receive_error;

		if (len < (ssize_t)sizeof(*sync))
			continue;

		if (strcmp(sync->type, "header") != 0)
			continue;
		if (ntohl(sync->data_type) != 's')
			continue;
		if (ntohl(sync->data_stream) != 's')
			continue;
		break;
	}
	impl->sync.is_last = ntohl(sync->is_last);
	impl->sync.frames = ntohl(sync->frames);
	if (impl->sync.frames == -1)
		impl->sync.frames = impl->params.period_size;

	return impl->sync.frames;

receive_error:
	pw_log_warn("recv error: %m");
	return 0;
}

struct data_info {
	void *data;
	uint32_t id;
};

static int netjack2_send_sync(struct stream *s, uint32_t nframes)
{
	struct impl *impl = s->impl;
	struct nj2_packet_header *header = (struct nj2_packet_header *)impl->send_buffer;
	uint32_t i, packet_size, active_ports, is_last;
	int32_t *p;

	/* we always listen on all ports */
	active_ports = impl->params.send_audio_channels;
	packet_size = sizeof(*header) + active_ports * sizeof(int32_t);
	is_last = impl->params.recv_midi_channels == 0 &&
                        impl->params.recv_audio_channels == 0 ? 1 : 0;

	header->active_ports = htonl(active_ports);
	header->frames = htonl(nframes);
	header->cycle = htonl(impl->sync.cycle);
	header->sub_cycle = 0;
	header->data_type = htonl('s');
	header->is_last = htonl(is_last);
	header->packet_size = htonl(packet_size);
	p = SPA_PTROFF(header, sizeof(*header), int32_t);
	for (i = 0; i < active_ports; i++)
		p[i] = htonl(i);
	send(impl->socket->fd, header, packet_size, 0);
	return 0;
}

static int netjack2_send_midi(struct stream *s, uint32_t nframes,
		struct data_info *info, uint32_t n_info)
{
	struct impl *impl = s->impl;
	struct nj2_packet_header *header = (struct nj2_packet_header *)impl->send_buffer;
	uint32_t i, num_packets, active_ports, data_size, res1, res2, max_size;
	int32_t *ap;

	if (impl->params.recv_midi_channels <= 0)
		return 0;

	active_ports = impl->params.recv_midi_channels;
	ap = SPA_PTROFF(header, sizeof(*header), int32_t);

	data_size = active_ports * sizeof(struct nj2_midi_buffer);
	memset(ap, 0, data_size);

	max_size = PACKET_AVAILABLE_SIZE(impl->params.mtu);

	res1 = data_size % max_size;
        res2 = data_size / max_size;
        num_packets = (res1) ? res2 + 1 : res2;

	header->data_type = htonl('m');
	header->active_ports = htonl(active_ports);
	header->num_packets = htonl(num_packets);

	for (i = 0; i < num_packets; i++) {
		uint32_t is_last = ((i == num_packets - 1) && impl->params.recv_audio_channels == 0) ? 1 : 0;
		uint32_t packet_size = sizeof(*header) + data_size;

		header->sub_cycle = htonl(i);
		header->is_last = htonl(is_last);
                header->packet_size = htonl(packet_size);
		send(impl->socket->fd, header, packet_size, 0);
		//nj2_dump_packet_header(header);
	}
	return 0;
}

static int netjack2_send_audio(struct stream *s, uint32_t frames, struct data_info *info, uint32_t n_info)
{
	struct impl *impl = s->impl;
	struct nj2_packet_header *header = (struct nj2_packet_header *)impl->send_buffer;
	uint32_t i, j, active_ports, num_packets;
	uint32_t sub_period_size, sub_period_bytes;

	if (impl->params.recv_audio_channels <= 0)
		return 0;

	active_ports = n_info;

	if (active_ports == 0) {
		sub_period_size = impl->params.period_size;
	} else {
		uint32_t max_size = PACKET_AVAILABLE_SIZE(impl->params.mtu);
		uint32_t period = (uint32_t) powf(2.f, (uint32_t) (logf((float)max_size /
				(active_ports * sizeof(float))) / logf(2.f)));
		sub_period_size = SPA_MIN(period, impl->params.period_size);
	}
	sub_period_bytes = sub_period_size * sizeof(float) + sizeof(int32_t);
	num_packets = impl->params.period_size / sub_period_size;

	header->data_type = htonl('a');
	header->active_ports = htonl(active_ports);
	header->num_packets = htonl(num_packets);

	for (i = 0; i < num_packets; i++) {
		uint32_t is_last = (i == num_packets - 1) ? 1 : 0;
		uint32_t packet_size = sizeof(*header) + active_ports * sub_period_bytes;
		int32_t *ap = SPA_PTROFF(header, sizeof(*header), int32_t);
		float *src;

		for (j = 0; j < active_ports; j++) {
			ap[0] = htonl(info[j].id);

			src = SPA_PTROFF(info[j].data, i * sub_period_size * sizeof(float), float);
			do_volume((float*)&ap[1], src, &s->volume, info[j].id, sub_period_size);

			ap = SPA_PTROFF(ap, sub_period_bytes, int32_t);
		}
		header->sub_cycle = htonl(i);
		header->is_last = htonl(is_last);
		header->packet_size = htonl(packet_size);
		send(impl->socket->fd, header, packet_size, 0);
		//nj2_dump_packet_header(header);
	}
	return 0;
}

static int netjack2_send_data(struct stream *s, uint32_t nframes,
		struct data_info *midi, uint32_t n_midi,
		struct data_info *audio, uint32_t n_audio)
{
	netjack2_send_sync(s, nframes);
	netjack2_send_midi(s, nframes, midi, n_midi);
	netjack2_send_audio(s, nframes, audio, n_audio);
	return 0;
}

static void sink_process(void *d, struct spa_io_position *position)
{
	struct stream *s = d;
	struct impl *impl = s->impl;
	uint32_t i, nframes = position->clock.duration;
	struct data_info midi[s->n_ports];
	struct data_info audio[s->n_ports];
	uint32_t n_midi, n_audio;

	if (impl->driving == MODE_SINK && impl->triggered) {
		impl->triggered = false;
		return;
	}

	n_midi = n_audio = 0;
	for (i = 0; i < s->n_ports; i++) {
		struct port *p = s->ports[i];
		void *data = p ? pw_filter_get_dsp_buffer(p, nframes) : NULL;
		if (p && p->is_midi) {
			midi[n_midi].data = data;
			midi[n_midi++].id = i;
		} else if (data != NULL) {
			audio[n_audio].data = data;
			audio[n_audio++].id = i;
		}
	}

	netjack2_send_data(s, nframes, midi, n_midi, audio, n_audio);

	pw_log_trace_fp("done %"PRIu64, impl->frame_time);
	if (impl->driving == MODE_SINK)
		impl->done = true;
}

static int netjack2_recv_midi(struct stream *s, struct nj2_packet_header *header, uint32_t *count,
		struct data_info *info, uint32_t n_info)
{
	struct impl *impl = s->impl;
	ssize_t len;

	if ((len = recv(impl->socket->fd, impl->recv_buffer, ntohl(header->packet_size), 0)) < 0)
		return -errno;

	impl->sync.is_last = ntohl(header->is_last);

	impl->sync.cycle = ntohl(header->cycle);
	impl->sync.num_packets = ntohl(header->num_packets);

	if (++(*count) == impl->sync.num_packets) {
		pw_log_trace_fp("got last midi packet");
	}
	return 0;
}

static int netjack2_recv_audio(struct stream *s, struct nj2_packet_header *header, uint32_t *count,
		struct data_info *info, uint32_t n_info)
{
	struct impl *impl = s->impl;
	ssize_t len;
	uint32_t i, sub_cycle, sub_period_size, sub_period_bytes, active_ports;

	if ((len = recv(impl->socket->fd, impl->recv_buffer, ntohl(header->packet_size), 0)) < 0)
		return -errno;

	impl->sync.is_last = ntohl(header->is_last);

	sub_cycle = ntohl(header->sub_cycle);
	active_ports = ntohl(header->active_ports);

	if (active_ports == 0) {
		sub_period_size = impl->params.period_size;
	} else {
		uint32_t max_size = PACKET_AVAILABLE_SIZE(impl->params.mtu);
		uint32_t period = (uint32_t) powf(2.f, (uint32_t) (logf((float)max_size /
				(active_ports * sizeof(float))) / logf(2.f)));
		sub_period_size = SPA_MIN(period, impl->params.period_size);
	}
	sub_period_bytes = sub_period_size * sizeof(float) + sizeof(int32_t);

	for (i = 0; i < active_ports; i++) {
		int32_t *ap = SPA_PTROFF(header, sizeof(*header) + i * sub_period_bytes, int32_t);
		uint32_t active_port = ntohl(ap[0]);
		void *data;

		pw_log_trace_fp("%u/%u %u %u", active_port, n_info,
				sub_cycle, sub_period_size);
		if (active_port >= n_info)
			continue;

		if ((data = info[active_port].data) != NULL) {
			float *dst = SPA_PTROFF(data,
					sub_cycle * sub_period_size * sizeof(float),
					float);
			do_volume(dst, (float*)&ap[1], &s->volume, active_port, sub_period_size);
		}
	}
	if (impl->sync.is_last)
		pw_log_trace_fp("got last audio packet");

	return 0;
}

static int netjack2_recv_data(struct stream *s, struct data_info *info, uint32_t n_info)
{
	struct impl *impl = s->impl;
	ssize_t len;
	uint32_t count = 0;
	struct nj2_packet_header *header = (struct nj2_packet_header *)impl->recv_buffer;

	while (!impl->sync.is_last) {
		if ((len = recv(impl->socket->fd, impl->recv_buffer, impl->params.mtu, MSG_PEEK)) < 0)
			goto receive_error;

		if (len < (ssize_t)sizeof(*header))
			goto receive_error;

		if (ntohl(header->data_stream) != 's' ||
		    ntohl(header->id) != impl->params.id) {
			pw_log_debug("not our packet");
			continue;
		}
		//nj2_dump_packet_header(header);

		switch (ntohl(header->data_type)) {
		case 'm':
			netjack2_recv_midi(s, header, &count, info, n_info);
			break;
		case 'a':
			netjack2_recv_audio(s, header, &count, info, n_info);
			break;
		case 's':
			pw_log_info("missing last data packet");
			return 0;
		}
	}
	impl->sync.cycle = ntohl(header->cycle);
	return 0;

receive_error:
	pw_log_warn("recv error: %m");
	return -errno;
}

static void source_process(void *d, struct spa_io_position *position)
{
	struct stream *s = d;
	struct impl *impl = s->impl;
	uint32_t i, n_samples = position->clock.duration;
	struct data_info info[s->n_ports];

	if (impl->driving == MODE_SOURCE && !impl->triggered) {
		pw_log_trace_fp("done %"PRIu64, impl->frame_time);
		impl->done = true;
		return;
	}
	impl->triggered = false;

	for (i = 0; i < s->n_ports; i++) {
		struct port *p = s->ports[i];
		info[i].data = p ? pw_filter_get_dsp_buffer(p, n_samples) : NULL;
		info[i].id = i;
	}

	netjack2_recv_data(s, info, s->n_ports);
}

static void stream_io_changed(void *data, void *port_data, uint32_t id, void *area, uint32_t size)
{
	struct stream *s = data;
	struct impl *impl = s->impl;
	if (port_data == NULL) {
		switch (id) {
		case SPA_IO_Position:
			impl->position = area;
			break;
		default:
			break;
		}
	}
}

static void param_latency_changed(struct stream *s, const struct spa_pod *param,
		struct port *port)
{
	struct spa_latency_info latency;
	bool update = false;
	enum spa_direction direction = port->direction;

	if (spa_latency_parse(param, &latency) < 0)
		return;

	if (spa_latency_info_compare(&port->latency[direction], &latency)) {
		port->latency[direction] = latency;
		port->latency_changed[direction] = update = true;
	}
}

static void make_stream_ports(struct stream *s)
{
	uint32_t i;
	struct pw_properties *props;
	const char *str, *prefix;
	char name[256];
	bool is_midi;

	if (s->direction == PW_DIRECTION_INPUT) {
		/* sink */
		prefix = "playback";
	} else {
		/* source */
		prefix = "capture";
	}

	for (i = 0; i < s->n_ports; i++) {
		struct port *port = s->ports[i];
		if (port != NULL) {
			s->ports[i] = NULL;
			pw_filter_remove_port(port);
		}

		if (i < s->info.channels) {
			str = spa_debug_type_find_short_name(spa_type_audio_channel,
					s->info.position[i]);
			if (str)
				snprintf(name, sizeof(name), "%s_%s", prefix, str);
			else
				snprintf(name, sizeof(name), "%s_%d", prefix, i);

			props = pw_properties_new(
					PW_KEY_FORMAT_DSP, "32 bit float mono audio",
					PW_KEY_AUDIO_CHANNEL, str ? str : "UNK",
					PW_KEY_PORT_PHYSICAL, "true",
					PW_KEY_PORT_NAME, name,
					NULL);

			is_midi = false;
		} else {
			snprintf(name, sizeof(name), "%s_%d", prefix, i - s->info.channels);
			props = pw_properties_new(
					PW_KEY_FORMAT_DSP, "8 bit raw midi",
					PW_KEY_PORT_NAME, name,
					PW_KEY_PORT_PHYSICAL, "true",
					NULL);

			is_midi = true;
		}

		port = pw_filter_add_port(s->filter,
                        s->direction,
                        PW_FILTER_PORT_FLAG_MAP_BUFFERS,
                        sizeof(struct port),
			props, NULL, 0);

		port->is_midi = is_midi;

		s->ports[i] = port;
	}
}

static struct spa_pod *make_props_param(struct spa_pod_builder *b,
		struct volume *vol)
{
	return spa_pod_builder_add_object(b, SPA_TYPE_OBJECT_Props, SPA_PARAM_Props,
			SPA_PROP_mute, SPA_POD_Bool(vol->mute),
			SPA_PROP_channelVolumes, SPA_POD_Array(sizeof(float),
				SPA_TYPE_Float, vol->n_volumes, vol->volumes));
}

static void parse_props(struct stream *s, const struct spa_pod *param)
{
	struct spa_pod_object *obj = (struct spa_pod_object *) param;
	struct spa_pod_prop *prop;
	uint8_t buffer[1024];
	struct spa_pod_builder b;
	const struct spa_pod *params[1];

	SPA_POD_OBJECT_FOREACH(obj, prop) {
		switch (prop->key) {
		case SPA_PROP_mute:
		{
			bool mute;
			if (spa_pod_get_bool(&prop->value, &mute) == 0)
				s->volume.mute = mute;
			break;
		}
		case SPA_PROP_channelVolumes:
		{
			uint32_t n;
			float vols[SPA_AUDIO_MAX_CHANNELS];
			if ((n = spa_pod_copy_array(&prop->value, SPA_TYPE_Float,
					vols, SPA_AUDIO_MAX_CHANNELS)) > 0) {
				s->volume.n_volumes = n;
				for (n = 0; n < s->volume.n_volumes; n++)
					s->volume.volumes[n] = vols[n];
			}
			break;
		}
		default:
			break;
		}
	}
	spa_pod_builder_init(&b, buffer, sizeof(buffer));
	params[0] = make_props_param(&b, &s->volume);

	pw_filter_update_params(s->filter, NULL, params, 1);
}

static void stream_param_changed(void *data, void *port_data, uint32_t id,
		const struct spa_pod *param)
{
	struct stream *s = data;
	if (port_data != NULL) {
		switch (id) {
		case SPA_PARAM_Latency:
			param_latency_changed(s, param, port_data);
			break;
		}
	} else {
		switch (id) {
		case SPA_PARAM_PortConfig:
			pw_log_debug("PortConfig");
			make_stream_ports(s);
			pw_filter_set_active(s->filter, true);
			break;
		case SPA_PARAM_Props:
			pw_log_debug("Props");
			parse_props(s, param);
			break;
		}
	}
}

static const struct pw_filter_events sink_events = {
	PW_VERSION_FILTER_EVENTS,
	.destroy = stream_destroy,
	.state_changed = stream_state_changed,
	.param_changed = stream_param_changed,
	.io_changed = stream_io_changed,
	.process = sink_process
};

static const struct pw_filter_events source_events = {
	PW_VERSION_FILTER_EVENTS,
	.destroy = stream_destroy,
	.state_changed = stream_state_changed,
	.param_changed = stream_param_changed,
	.io_changed = stream_io_changed,
	.process = source_process,
};

static int make_stream(struct stream *s, const char *name)
{
	struct impl *impl = s->impl;
	uint32_t n_params;
	const struct spa_pod *params[4];
	uint8_t buffer[1024];
	struct spa_pod_builder b;
	struct spa_latency_info latency;

	spa_zero(latency);
	n_params = 0;
	spa_pod_builder_init(&b, buffer, sizeof(buffer));

	s->filter = pw_filter_new(impl->core, name, s->props);
	s->props = NULL;
	if (s->filter == NULL)
		return -errno;

	if (s->direction == PW_DIRECTION_INPUT) {
		pw_filter_add_listener(s->filter, &s->listener,
				&sink_events, s);
	} else {
		pw_filter_add_listener(s->filter, &s->listener,
				&source_events, s);
	}

	reset_volume(&s->volume, s->info.channels);

	n_params = 0;
	params[n_params++] = spa_format_audio_raw_build(&b,
			SPA_PARAM_EnumFormat, &s->info);
	params[n_params++] = spa_format_audio_raw_build(&b,
			SPA_PARAM_Format, &s->info);
	params[n_params++] = make_props_param(&b, &s->volume);

	return pw_filter_connect(s->filter,
			PW_FILTER_FLAG_INACTIVE |
			PW_FILTER_FLAG_DRIVER |
			PW_FILTER_FLAG_RT_PROCESS |
			PW_FILTER_FLAG_CUSTOM_LATENCY,
			params, n_params);
}

static int create_filters(struct impl *impl)
{
	int res = 0;

	if (impl->mode & MODE_SINK)
		res = make_stream(&impl->sink, "JACK Sink");

	if (impl->mode & MODE_SOURCE)
		res = make_stream(&impl->source, "JACK Source");

	return res;
}


static inline uint64_t get_time_ns(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return SPA_TIMESPEC_TO_NSEC(&ts);
}

static void
on_data_io(void *data, int fd, uint32_t mask)
{
	struct impl *impl = data;

	if (mask & (SPA_IO_ERR | SPA_IO_HUP)) {
		pw_log_warn("error:%08x", mask);
		return;
	}
	if (mask & SPA_IO_IN) {
		bool source_running, sink_running;
		uint32_t nframes;
		uint64_t nsec;

		nframes = netjack2_sync_wait(impl);
		if (nframes == 0)
			return;

		nsec = get_time_ns();

		if (!impl->done) {
			impl->pw_xrun++;
			impl->new_xrun = true;
		}

		source_running = impl->source.running;
		sink_running = impl->sink.running;

		impl->frame_time += nframes;

		pw_log_trace_fp("process %d %u %u %p %"PRIu64, nframes, source_running,
				sink_running, impl->position, impl->frame_time);

		if (impl->new_xrun) {
			pw_log_warn("Xrun netjack2:%u PipeWire:%u", impl->nj2_xrun, impl->pw_xrun);
			impl->new_xrun = false;
		}
		if (impl->position) {
			struct spa_io_clock *c = &impl->position->clock;

			c->nsec = nsec;
			c->rate = SPA_FRACTION(1, impl->samplerate);
			c->position = impl->frame_time;
			c->duration = nframes;
			c->delay = 0;
			c->rate_diff = 1.0;
			c->next_nsec = nsec;

			c->target_rate = c->rate;
			c->target_duration = c->duration;
		}
		if (!source_running)
			netjack2_recv_data(&impl->sink, NULL, 0);

		if (impl->mode & MODE_SOURCE && source_running) {
			impl->done = false;
			impl->triggered = true;
			impl->driving = MODE_SOURCE;
			pw_filter_trigger_process(impl->source.filter);
		} else if (impl->mode == MODE_SINK && sink_running) {
			impl->done = false;
			impl->triggered = true;
			impl->driving = MODE_SINK;
			pw_filter_trigger_process(impl->sink.filter);
		} else {
			sink_running = false;
			impl->done = true;
		}
		if (!sink_running)
			netjack2_send_data(&impl->sink, nframes, NULL, 0, NULL, 0);
	}
}

static int
do_schedule_destroy(struct spa_loop *loop,
	bool async, uint32_t seq, const void *data, size_t size, void *user_data)
{
	struct impl *impl = user_data;
	pw_impl_module_schedule_destroy(impl->module);
	return 0;
}

void module_schedule_destroy(struct impl *impl)
{
	pw_loop_invoke(impl->main_loop, do_schedule_destroy, 1, NULL, 0, false, impl);
}

static int parse_address(const char *address, uint16_t port,
		struct sockaddr_storage *addr, socklen_t *len)
{
	struct sockaddr_in *sa4 = (struct sockaddr_in*)addr;
	struct sockaddr_in6 *sa6 = (struct sockaddr_in6*)addr;

	if (inet_pton(AF_INET, address, &sa4->sin_addr) > 0) {
		sa4->sin_family = AF_INET;
		sa4->sin_port = htons(port);
		*len = sizeof(*sa4);
	} else if (inet_pton(AF_INET6, address, &sa6->sin6_addr) > 0) {
		sa6->sin6_family = AF_INET6;
		sa6->sin6_port = htons(port);
		*len = sizeof(*sa6);
	} else
		return -EINVAL;

	return 0;
}

static bool is_multicast(struct sockaddr *sa, socklen_t salen)
{
	if (sa->sa_family == AF_INET) {
		static const uint32_t ipv4_mcast_mask = 0xe0000000;
		struct sockaddr_in *sa4 = (struct sockaddr_in*)sa;
		return (ntohl(sa4->sin_addr.s_addr) & ipv4_mcast_mask) == ipv4_mcast_mask;
	} else if (sa->sa_family == AF_INET6) {
		struct sockaddr_in6 *sa6 = (struct sockaddr_in6*)sa;
		return sa6->sin6_addr.s6_addr[0] == 0xff;
	}
	return false;
}

static int make_socket(struct sockaddr_storage *src, socklen_t src_len,
		struct sockaddr_storage *dst, socklen_t dst_len,
		bool loop, int ttl, int dscp)
{
	int af, fd, val, res;
	struct timeval timeout;

	af = src->ss_family;
	if ((fd = socket(af, SOCK_DGRAM | SOCK_CLOEXEC, 0)) < 0) {
		pw_log_error("socket failed: %m");
		return -errno;
	}
	val = 1;
	if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &val, sizeof(val)) < 0) {
		res = -errno;
		pw_log_error("setsockopt failed: %m");
		goto error;
	}

#ifdef SO_PRIORITY
	val = 6;
	if (setsockopt(fd, SOL_SOCKET, SO_PRIORITY, &val, sizeof(val)) < 0)
		pw_log_warn("setsockopt(SO_PRIORITY) failed: %m");
#endif
	timeout.tv_sec = 2;
	timeout.tv_usec = 0;
	if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) < 0)
		pw_log_warn("setsockopt(SO_RCVTIMEO) failed: %m");

	if (dscp > 0) {
		val = IPTOS_DSCP(dscp << 2);
		if (setsockopt(fd, IPPROTO_IP, IP_TOS, &val, sizeof(val)) < 0)
			pw_log_warn("setsockopt(IP_TOS) failed: %m");
	}

	if (bind(fd, (struct sockaddr*)src, src_len) < 0) {
		res = -errno;
		pw_log_error("bind() failed: %m");
		goto error;
	}
	if (is_multicast((struct sockaddr*)dst, dst_len)) {
		val = loop;
		if (setsockopt(fd, IPPROTO_IP, IP_MULTICAST_LOOP, &val, sizeof(val)) < 0)
			pw_log_warn("setsockopt(IP_MULTICAST_LOOP) failed: %m");

		val = ttl;
		if (setsockopt(fd, IPPROTO_IP, IP_MULTICAST_TTL, &val, sizeof(val)) < 0)
			pw_log_warn("setsockopt(IP_MULTICAST_TTL) failed: %m");
	}

	return fd;
error:
	close(fd);
	return res;
}

static int get_ip(const struct sockaddr_storage *sa, char *ip, size_t len)
{
	if (sa->ss_family == AF_INET) {
		struct sockaddr_in *in = (struct sockaddr_in*)sa;
		inet_ntop(sa->ss_family, &in->sin_addr, ip, len);
	} else if (sa->ss_family == AF_INET6) {
		struct sockaddr_in6 *in = (struct sockaddr_in6*)sa;
		inet_ntop(sa->ss_family, &in->sin6_addr, ip, len);
	} else
		return -EIO;
	return 0;
}

static void update_timer(struct impl *impl, uint64_t timeout)
{
	struct timespec value, interval;
	value.tv_sec = 0;
	value.tv_nsec = timeout ? 1 : 0;
	interval.tv_sec = timeout;
	interval.tv_nsec = 0;
	pw_loop_update_timer(impl->main_loop, impl->timer, &value, &interval, false);
}

static int handle_follower_setup(struct impl *impl, struct nj2_session_params *params,
	struct sockaddr_storage *addr, socklen_t addr_len)
{
	int res;
	struct nj2_packet_header *header;

	pw_log_info("got follower setup");
	nj2_dump_session_params(params);

	impl->params = *params;
	impl->params.version = ntohl(params->version);
	impl->params.packet_id = ntohl(params->packet_id);
	impl->params.mtu = ntohl(params->mtu);
	impl->params.id = ntohl(params->id);
	impl->params.transport_sync = ntohl(params->transport_sync);
	impl->params.send_audio_channels = ntohl(params->send_audio_channels);
	impl->params.recv_audio_channels = ntohl(params->recv_audio_channels);
	impl->params.send_midi_channels = ntohl(params->send_midi_channels);
	impl->params.recv_midi_channels = ntohl(params->recv_midi_channels);
	impl->params.sample_rate = ntohl(params->sample_rate);
	impl->params.period_size = ntohl(params->period_size);
	impl->params.sample_encoder = ntohl(params->sample_encoder);
	impl->params.kbps = ntohl(params->kbps);
	impl->params.follower_sync_mode = ntohl(params->follower_sync_mode);
	impl->params.network_latency = ntohl(params->network_latency);

	update_timer(impl, 0);
	pw_loop_update_io(impl->main_loop, impl->setup_socket, 0);

	impl->source.n_ports = impl->params.send_audio_channels + impl->params.send_midi_channels;
	impl->source.info.rate =  impl->params.sample_rate;
	impl->source.info.channels =  impl->params.send_audio_channels;
	impl->sink.n_ports = impl->params.recv_audio_channels + impl->params.recv_midi_channels;
	impl->sink.info.rate =  impl->params.sample_rate;
	impl->sink.info.channels =  impl->params.recv_audio_channels;
	impl->samplerate = impl->params.sample_rate;

	header = (struct nj2_packet_header *)impl->send_buffer;
	snprintf(header->type, sizeof(header->type), "header");
	header->data_stream = htonl('r');
	header->id = params->id;

	if ((res = create_filters(impl)) < 0)
		return res;

	if (connect(impl->setup_socket->fd, (struct sockaddr*)addr, addr_len) < 0)
		goto connect_error;

	impl->done = true;
	pw_loop_update_io(impl->data_loop->loop, impl->socket, SPA_IO_IN);

	params->packet_id = htonl(NJ2_ID_START_DRIVER);
	send(impl->socket->fd, params, sizeof(*params), 0);

	return 0;

connect_error:
	pw_log_error("connect() failed: %m");
	return -errno;
}

static int handle_packet(struct impl *impl, void *data, int len,
	struct sockaddr_storage *addr, socklen_t addr_len)
{
	struct nj2_session_params *params;

	if (len < (int)sizeof(struct nj2_session_params))
		goto short_packet;

	params = data;
	if (strcmp(params->type, impl->params.type) != 0) {
		pw_log_info("wrong packet type '%s' != '%s'",
				params->type, impl->params.type);
		return -EINVAL;
	}
	if (ntohl(params->packet_id) != NJ2_ID_FOLLOWER_SETUP) {
		pw_log_info("wrong packet id %d != %d",
				htonl(params->packet_id), NJ2_ID_FOLLOWER_SETUP);
		return -EINVAL;
	}
	return handle_follower_setup(impl, data, addr, addr_len);

short_packet:
	pw_log_warn("short packet received");
	return -EIO;
}

static void
on_socket_io(void *data, int fd, uint32_t mask)
{
	struct impl *impl = data;
	ssize_t len;
	uint8_t buffer[2048];
	struct sockaddr_storage addr;
	socklen_t addr_len;

	if (mask & SPA_IO_IN) {
		if ((len = recvfrom(fd, buffer, sizeof(buffer), 0,
				(struct sockaddr *)&addr, &addr_len)) < 0)
			goto receive_error;

		handle_packet(impl, (void *)buffer, len, &addr, addr_len);
	}
	return;

receive_error:
	pw_log_warn("recv error: %m");
	return;
}

static int send_follower_available(struct impl *impl)
{
	pw_loop_update_io(impl->main_loop, impl->setup_socket, SPA_IO_IN);

	pw_log_info("sending AVAILABLE");
	impl->params.packet_id = htonl(NJ2_ID_FOLLOWER_AVAILABLE);
	sendto(impl->setup_socket->fd, &impl->params, sizeof(impl->params), 0,
			(struct sockaddr*)&impl->dst_addr, impl->dst_len);
	return 0;
}

static void on_timer_event(void *data, uint64_t expirations)
{
	struct impl *impl = data;

	if (--impl->init_retry == 0) {
		pw_log_error("timeout in connect");
		update_timer(impl, 0);
		return;
	}
	send_follower_available(impl);
}

static int create_netjack2_socket(struct impl *impl)
{
	const char *client_name, *str;
	uint32_t port;
	int fd, res;
	bool loop;
	int ttl, dscp, mtu;

	port = pw_properties_get_uint32(impl->props, "net.port", 0);
	if (port == 0)
		port = DEFAULT_NET_PORT;
	if ((str = pw_properties_get(impl->props, "net.ip")) == NULL)
		str = DEFAULT_NET_IP;
	if ((res = parse_address(str, port, &impl->dst_addr, &impl->dst_len)) < 0) {
		pw_log_error("invalid net.ip %s: %s", str, spa_strerror(res));
		goto out;
	}
	if ((res = parse_address("0.0.0.0", 0, &impl->src_addr, &impl->src_len)) < 0) {
		pw_log_error("invalid source.ip: %s", spa_strerror(res));
		goto out;
	}

	mtu = pw_properties_get_uint32(impl->props, "net.mtu", DEFAULT_NET_MTU);
	ttl = pw_properties_get_uint32(impl->props, "net.ttl", DEFAULT_NET_TTL);
	loop = pw_properties_get_bool(impl->props, "net.loop", DEFAULT_NET_LOOP);
	dscp = pw_properties_get_uint32(impl->props, "net.dscp", DEFAULT_NET_DSCP);

	fd = make_socket(&impl->src_addr, impl->src_len,
			&impl->dst_addr, impl->dst_len, loop, ttl, dscp);
	if (fd < 0) {
		res = -errno;
		pw_log_error("can't create socket: %s", spa_strerror(res));
		goto out;
	}

	impl->setup_socket = pw_loop_add_io(impl->main_loop, fd,
				0, true, on_socket_io, impl);
	if (impl->setup_socket == NULL) {
		res = -errno;
		pw_log_error("can't create setup source: %m");
		close(fd);
		goto out;
	}
	impl->socket = pw_loop_add_io(impl->data_loop->loop, fd,
			0, false, on_data_io, impl);
	if (impl->socket == NULL) {
		res = -errno;
		pw_log_error("can't create data source: %m");
		goto out;
	}

	impl->init_retry = FOLLOWER_INIT_RETRY;
	impl->timer = pw_loop_add_timer(impl->main_loop, on_timer_event, impl);
	if (impl->timer == NULL) {
		res = -errno;
		pw_log_error("can't create timer source: %m");
		goto out;
	}
	update_timer(impl, FOLLOWER_INIT_TIMEOUT);

	client_name = pw_properties_get(impl->props, "jack.client-name");
	if (client_name == NULL)
		client_name = DEFAULT_CLIENT_NAME;

	strcpy(impl->params.type, "params");
	impl->params.version = htonl(NETWORK_PROTOCOL);
	snprintf(impl->params.name, sizeof(impl->params.name), "%s", client_name);
	impl->params.mtu = htonl(mtu);
	impl->params.sample_encoder = htonl(NJ2_ENCODER_FLOAT);
	get_ip(&impl->src_addr, impl->params.follower_name, sizeof(impl->params.follower_name));
	impl->params.transport_sync = htonl(0);
        impl->params.network_latency = htonl(NETWORK_DEFAULT_LATENCY);
	impl->params.follower_sync_mode = htonl(1);
	impl->params.send_audio_channels = htonl(-1);
	impl->params.recv_audio_channels = htonl(-1);
	impl->params.send_midi_channels = htonl(-1);
	impl->params.recv_midi_channels = htonl(-1);

	return 0;
out:
	return res;
}

static int send_stop_driver(struct impl *impl)
{
	pw_log_info("sending STOP_DRIVER");
	impl->params.packet_id = htonl(NJ2_ID_STOP_DRIVER);
	sendto(impl->setup_socket->fd, &impl->params, sizeof(impl->params), 0,
			(struct sockaddr*)&impl->dst_addr, impl->dst_len);
	return 0;
}

static int destroy_netjack2_socket(struct impl *impl)
{
	if (impl->socket) {
		pw_loop_destroy_source(impl->data_loop->loop, impl->socket);
		impl->socket = NULL;
	}
	if (impl->setup_socket) {
		send_stop_driver(impl);
		pw_loop_destroy_source(impl->main_loop, impl->setup_socket);
		impl->setup_socket = NULL;
	}
	if (impl->timer) {
		pw_loop_destroy_source(impl->main_loop, impl->timer);
		impl->timer = NULL;
	}
	return 0;
}


static void core_error(void *data, uint32_t id, int seq, int res, const char *message)
{
	struct impl *impl = data;

	pw_log_error("error id:%u seq:%d res:%d (%s): %s",
			id, seq, res, spa_strerror(res), message);

	if (id == PW_ID_CORE && res == -EPIPE)
		pw_impl_module_schedule_destroy(impl->module);
}

static const struct pw_core_events core_events = {
	PW_VERSION_CORE_EVENTS,
	.error = core_error,
};

static void core_destroy(void *d)
{
	struct impl *impl = d;
	spa_hook_remove(&impl->core_listener);
	impl->core = NULL;
	pw_impl_module_schedule_destroy(impl->module);
}

static const struct pw_proxy_events core_proxy_events = {
	.destroy = core_destroy,
};

static void impl_destroy(struct impl *impl)
{
	destroy_netjack2_socket(impl);

	if (impl->source.filter)
		pw_filter_destroy(impl->source.filter);
	if (impl->sink.filter)
		pw_filter_destroy(impl->sink.filter);
	if (impl->core && impl->do_disconnect)
		pw_core_disconnect(impl->core);

	pw_properties_free(impl->sink.props);
	pw_properties_free(impl->source.props);
	pw_properties_free(impl->props);

	free(impl);
}

static void module_destroy(void *data)
{
	struct impl *impl = data;
	spa_hook_remove(&impl->module_listener);
	impl_destroy(impl);
}

static const struct pw_impl_module_events module_events = {
	PW_VERSION_IMPL_MODULE_EVENTS,
	.destroy = module_destroy,
};

static uint32_t channel_from_name(const char *name)
{
	int i;
	for (i = 0; spa_type_audio_channel[i].name; i++) {
		if (spa_streq(name, spa_debug_type_short_name(spa_type_audio_channel[i].name)))
			return spa_type_audio_channel[i].type;
	}
	return SPA_AUDIO_CHANNEL_UNKNOWN;
}

static void parse_position(struct spa_audio_info_raw *info, const char *val, size_t len)
{
	struct spa_json it[2];
	char v[256];

	spa_json_init(&it[0], val, len);
        if (spa_json_enter_array(&it[0], &it[1]) <= 0)
                spa_json_init(&it[1], val, len);

	info->channels = 0;
	while (spa_json_get_string(&it[1], v, sizeof(v)) > 0 &&
	    info->channels < SPA_AUDIO_MAX_CHANNELS) {
		info->position[info->channels++] = channel_from_name(v);
	}
}

static void parse_audio_info(const struct pw_properties *props, struct spa_audio_info_raw *info)
{
	const char *str;

	spa_zero(*info);
	info->format = SPA_AUDIO_FORMAT_F32P;
	info->rate = 0;
	info->channels = pw_properties_get_uint32(props, PW_KEY_AUDIO_CHANNELS, info->channels);
	info->channels = SPA_MIN(info->channels, SPA_AUDIO_MAX_CHANNELS);
	if ((str = pw_properties_get(props, SPA_KEY_AUDIO_POSITION)) != NULL)
		parse_position(info, str, strlen(str));
	if (info->channels == 0)
		parse_position(info, DEFAULT_POSITION, strlen(DEFAULT_POSITION));
}

static void copy_props(struct impl *impl, struct pw_properties *props, const char *key)
{
	const char *str;
	if ((str = pw_properties_get(props, key)) != NULL) {
		if (pw_properties_get(impl->sink.props, key) == NULL)
			pw_properties_set(impl->sink.props, key, str);
		if (pw_properties_get(impl->source.props, key) == NULL)
			pw_properties_set(impl->source.props, key, str);
	}
}

SPA_EXPORT
int pipewire__module_init(struct pw_impl_module *module, const char *args)
{
	struct pw_context *context = pw_impl_module_get_context(module);
	struct pw_properties *props = NULL;
	struct impl *impl;
	const char *str;
	int res;

	PW_LOG_TOPIC_INIT(mod_topic);

	impl = calloc(1, sizeof(struct impl));
	if (impl == NULL)
		return -errno;

	pw_log_debug("module %p: new %s", impl, args);

	if (args == NULL)
		args = "";

	props = pw_properties_new_string(args);
	if (props == NULL) {
		res = -errno;
		pw_log_error( "can't create properties: %m");
		goto error;
	}
	impl->props = props;
	impl->data_loop = pw_context_get_data_loop(context);

	impl->sink.props = pw_properties_new(NULL, NULL);
	impl->source.props = pw_properties_new(NULL, NULL);
	if (impl->source.props == NULL || impl->sink.props == NULL) {
		res = -errno;
		pw_log_error( "can't create properties: %m");
		goto error;
	}

	impl->module = module;
	impl->context = context;
	impl->main_loop = pw_context_get_main_loop(context);
	impl->system = impl->main_loop->system;

	impl->source.impl = impl;
	impl->source.direction = PW_DIRECTION_OUTPUT;
	impl->sink.impl = impl;
	impl->sink.direction = PW_DIRECTION_INPUT;

	impl->mode = MODE_DUPLEX;
	if ((str = pw_properties_get(props, "tunnel.mode")) != NULL) {
		if (spa_streq(str, "source")) {
			impl->mode = MODE_SOURCE;
		} else if (spa_streq(str, "sink")) {
			impl->mode = MODE_SINK;
		} else if (spa_streq(str, "duplex")) {
			impl->mode = MODE_DUPLEX;
		} else {
			pw_log_error("invalid tunnel.mode '%s'", str);
			res = -EINVAL;
			goto error;
		}
	}

	if (pw_properties_get(props, PW_KEY_NODE_VIRTUAL) == NULL)
		pw_properties_set(props, PW_KEY_NODE_VIRTUAL, "true");
	if (pw_properties_get(props, PW_KEY_NODE_GROUP) == NULL)
		pw_properties_set(props, PW_KEY_NODE_GROUP, "jack-group");
	if (pw_properties_get(props, PW_KEY_NODE_ALWAYS_PROCESS) == NULL)
		pw_properties_set(props, PW_KEY_NODE_ALWAYS_PROCESS, "true");

	pw_properties_set(impl->sink.props, PW_KEY_MEDIA_CLASS, "Audio/Sink");
	pw_properties_set(impl->sink.props, PW_KEY_PRIORITY_DRIVER, "40000");
	pw_properties_set(impl->sink.props, PW_KEY_NODE_NAME, "jack_sink");
	pw_properties_set(impl->sink.props, PW_KEY_NODE_DESCRIPTION, "JACK Sink");

	pw_properties_set(impl->source.props, PW_KEY_MEDIA_CLASS, "Audio/Source");
	pw_properties_set(impl->source.props, PW_KEY_PRIORITY_DRIVER, "40001");
	pw_properties_set(impl->source.props, PW_KEY_NODE_NAME, "jack_source");
	pw_properties_set(impl->source.props, PW_KEY_NODE_DESCRIPTION, "JACK Source");

	if ((str = pw_properties_get(props, "sink.props")) != NULL)
		pw_properties_update_string(impl->sink.props, str, strlen(str));
	if ((str = pw_properties_get(props, "source.props")) != NULL)
		pw_properties_update_string(impl->source.props, str, strlen(str));

	copy_props(impl, props, PW_KEY_AUDIO_CHANNELS);
	copy_props(impl, props, SPA_KEY_AUDIO_POSITION);
	copy_props(impl, props, PW_KEY_NODE_ALWAYS_PROCESS);
	copy_props(impl, props, PW_KEY_NODE_GROUP);
	copy_props(impl, props, PW_KEY_NODE_VIRTUAL);

	parse_audio_info(impl->source.props, &impl->source.info);
	parse_audio_info(impl->sink.props, &impl->sink.info);

	impl->source.n_midi = pw_properties_get_uint32(impl->source.props,
			"midi.ports", DEFAULT_MIDI_PORTS);
	impl->sink.n_midi = pw_properties_get_uint32(impl->sink.props,
			"midi.ports", DEFAULT_MIDI_PORTS);

	impl->source.n_ports = impl->source.n_midi + impl->source.info.channels;
	impl->sink.n_ports = impl->sink.n_midi + impl->sink.info.channels;
	if (impl->source.n_ports > MAX_PORTS || impl->sink.n_ports > MAX_PORTS) {
		pw_log_error("too many ports");
		res = -EINVAL;
		goto error;
	}

	impl->core = pw_context_get_object(impl->context, PW_TYPE_INTERFACE_Core);
	if (impl->core == NULL) {
		str = pw_properties_get(props, PW_KEY_REMOTE_NAME);
		impl->core = pw_context_connect(impl->context,
				pw_properties_new(
					PW_KEY_REMOTE_NAME, str,
					NULL),
				0);
		impl->do_disconnect = true;
	}
	if (impl->core == NULL) {
		res = -errno;
		pw_log_error("can't connect: %m");
		goto error;
	}

	pw_proxy_add_listener((struct pw_proxy*)impl->core,
			&impl->core_proxy_listener,
			&core_proxy_events, impl);
	pw_core_add_listener(impl->core,
			&impl->core_listener,
			&core_events, impl);

	if ((res = create_netjack2_socket(impl)) < 0)
		goto error;

	pw_impl_module_add_listener(module, &impl->module_listener, &module_events, impl);

	pw_impl_module_update_properties(module, &SPA_DICT_INIT_ARRAY(module_props));

	return 0;

error:
	impl_destroy(impl);
	return res;
}

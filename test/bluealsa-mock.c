/*
 * bluealsa-mock.c
 * Copyright (c) 2016-2022 Arkadiusz Bokowy
 *
 * This file is a part of bluez-alsa.
 *
 * This project is licensed under the terms of the MIT license.
 *
 * This program might be used to debug or check the functionality of ALSA
 * plug-ins. It should work exactly the same as the BlueALSA server.
 *
 */

#if HAVE_CONFIG_H
# include <config.h>
#endif

#include <assert.h>
#include <errno.h>
#include <getopt.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <gio/gio.h>
#include <glib-unix.h>
#include <glib.h>

#include "a2dp.h"
#include "a2dp-aac.h"
#include "a2dp-aptx.h"
#include "a2dp-aptx-hd.h"
#include "a2dp-faststream.h"
#include "a2dp-sbc.h"
#include "ba-adapter.h"
#include "ba-device.h"
#include "ba-transport.h"
#include "bluealsa-config.h"
#include "bluealsa-dbus.h"
#include "bluealsa-iface.h"
#include "bluez.h"
#include "bluez-iface.h"
#include "codec-sbc.h"
#include "hfp.h"
#include "io.h"
#include "storage.h"
#include "utils.h"
#include "shared/a2dp-codecs.h"
#include "shared/defs.h"
#include "shared/log.h"
#include "shared/rt.h"

#include "inc/dbus.inc"
#include "inc/sine.inc"

#define TEST_BLUEALSA_STORAGE_DIR "/tmp/bluealsa-mock-storage"

static const a2dp_sbc_t config_sbc_44100_stereo = {
	.frequency = SBC_SAMPLING_FREQ_44100,
	.channel_mode = SBC_CHANNEL_MODE_JOINT_STEREO,
	.block_length = SBC_BLOCK_LENGTH_16,
	.subbands = SBC_SUBBANDS_8,
	.allocation_method = SBC_ALLOCATION_LOUDNESS,
	.min_bitpool = SBC_MIN_BITPOOL,
	.max_bitpool = SBC_MAX_BITPOOL,
};

#if ENABLE_APTX
static const a2dp_aptx_t config_aptx_44100_stereo = {
	.info = A2DP_SET_VENDOR_ID_CODEC_ID(APTX_VENDOR_ID, APTX_CODEC_ID),
	.channel_mode = APTX_CHANNEL_MODE_STEREO,
	.frequency = APTX_SAMPLING_FREQ_44100,
};
#endif

#if ENABLE_APTX_HD
static const a2dp_aptx_hd_t config_aptx_hd_48000_stereo = {
	.aptx.info = A2DP_SET_VENDOR_ID_CODEC_ID(APTX_HD_VENDOR_ID, APTX_HD_CODEC_ID),
	.aptx.channel_mode = APTX_CHANNEL_MODE_STEREO,
	.aptx.frequency = APTX_SAMPLING_FREQ_48000,
};
#endif

#if ENABLE_FASTSTREAM
static const a2dp_faststream_t config_faststream_44100_16000 = {
	.info = A2DP_SET_VENDOR_ID_CODEC_ID(FASTSTREAM_VENDOR_ID, FASTSTREAM_CODEC_ID),
	.direction = FASTSTREAM_DIRECTION_MUSIC | FASTSTREAM_DIRECTION_VOICE,
	.frequency_music = FASTSTREAM_SAMPLING_FREQ_MUSIC_44100,
	.frequency_voice = FASTSTREAM_SAMPLING_FREQ_VOICE_16000,
};
#endif

static struct ba_adapter *a = NULL;
static char service[32] = BLUEALSA_SERVICE;
static GMutex timeout_mutex = { 0 };
static GCond timeout_cond = { 0 };
static const char *devices[8] = { NULL };
static bool dump_output = false;
static int timeout_ms = 5000;
static int fuzzing_ms = 0;

static gboolean main_loop_exit_handler(void *userdata) {
	g_main_loop_quit((GMainLoop *)userdata);
	return G_SOURCE_REMOVE;
}

static gboolean main_loop_timeout_handler(void *userdata) {
	(void)userdata;
	g_mutex_lock(&timeout_mutex);
	timeout_ms = 0;
	g_mutex_unlock(&timeout_mutex);
	g_cond_signal(&timeout_cond);
	return G_SOURCE_REMOVE;
}

static volatile sig_atomic_t sigusr1_count = 0;
static volatile sig_atomic_t sigusr2_count = 0;
static void mock_sigusr_handler(int sig) {
	switch (sig) {
	case SIGUSR1:
		sigusr1_count++;
		debug("Dispatching SIGUSR1: %d", sigusr1_count);
		break;
	case SIGUSR2:
		sigusr2_count++;
		debug("Dispatching SIGUSR2: %d", sigusr2_count);
		break;
	default:
		error("Unsupported signal: %d", sig);
	}
}

bool bluez_a2dp_set_configuration(const char *current_dbus_sep_path,
		const struct a2dp_sep *sep, GError **error) {
	debug("%s: %s", __func__, current_dbus_sep_path); (void)sep;
	*error = g_error_new(G_DBUS_ERROR, G_DBUS_ERROR_NOT_SUPPORTED, "Not supported");
	return false;
}

void bluez_battery_provider_update(struct ba_device *device) {
	debug("%s: %p", __func__, device);
}

static void *mock_a2dp_dec(struct ba_transport_thread *th) {

	pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);
	pthread_cleanup_push(PTHREAD_CLEANUP(ba_transport_thread_cleanup), th);

	struct ba_transport *t = th->t;
	struct ba_transport_pcm *t_a2dp_pcm = &t->a2dp.pcm;

	/* use back-channel PCM for bidirectional codecs */
	if (t->type.profile & BA_TRANSPORT_PROFILE_A2DP_SOURCE)
		t_a2dp_pcm = &t->a2dp.pcm_bc;

	const unsigned int channels = t_a2dp_pcm->channels;
	const unsigned int samplerate = t_a2dp_pcm->sampling;
	struct pollfd fds[1] = {{ th->pipe[0], POLLIN, 0 }};
	struct asrsync asrs = { .frames = 0 };
	int16_t buffer[1024 * 2];
	int x = 0;

	debug_transport_thread_loop(th, "START");
	ba_transport_thread_set_state_running(th);

	while (sigusr1_count == 0) {

		int timeout = 0;
		if (!ba_transport_pcm_is_active(t_a2dp_pcm))
			timeout = -1;

		pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, NULL);
		int rv = poll(fds, ARRAYSIZE(fds), timeout);
		pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);

		if (rv == 1 && fds[0].revents & POLLIN) {
			/* dispatch incoming event */
			enum ba_transport_thread_signal signal;
			ba_transport_thread_signal_recv(th, &signal);
			switch (signal) {
			case BA_TRANSPORT_THREAD_SIGNAL_PCM_OPEN:
			case BA_TRANSPORT_THREAD_SIGNAL_PCM_RESUME:
				asrs.frames = 0;
				continue;
			default:
				continue;
			}
		}

		fprintf(stderr, ".");

		if (asrs.frames == 0)
			asrsync_init(&asrs, samplerate);

		const size_t samples = ARRAYSIZE(buffer);
		const size_t frames = samples / channels;
		x = snd_pcm_sine_s16_2le(buffer, frames, channels, x, 1.0 / 128);

		io_pcm_scale(t_a2dp_pcm, buffer, samples);
		if (io_pcm_write(t_a2dp_pcm, buffer, samples) == -1)
			error("FIFO write error: %s", strerror(errno));

		/* maintain constant speed */
		asrsync_sync(&asrs, frames);

	}

	ba_transport_thread_set_state_stopping(th);
	pthread_cleanup_pop(1);
	return NULL;
}

static void *mock_bt_dump_thread(void *userdata) {

	int bt_fd = GPOINTER_TO_INT(userdata);
	FILE *f_output = NULL;
	uint8_t buffer[1024];
	ssize_t len;

	if (dump_output)
		f_output = fopen("bluealsa-mock.dump", "w");

	while ((len = read(bt_fd, buffer, sizeof(buffer))) > 0) {

		if (!dump_output)
			continue;

		for (ssize_t i = 0; i < len; i++)
			fprintf(f_output, "%02x", buffer[i]);
		fprintf(f_output, "\n");

	}

	if (f_output != NULL)
		fclose(f_output);
	close(bt_fd);
	return NULL;
}

static void mock_transport_start(struct ba_transport *t, int bt_fd) {

	if (t->type.profile & BA_TRANSPORT_PROFILE_A2DP_SOURCE) {
		g_thread_unref(g_thread_new(NULL, mock_bt_dump_thread, GINT_TO_POINTER(bt_fd)));
		assert(ba_transport_start(t) == 0);
	}
	else if (t->type.profile & BA_TRANSPORT_PROFILE_A2DP_SINK) {
		switch (t->type.codec) {
		case A2DP_CODEC_SBC:
			assert(ba_transport_thread_create(&t->thread_dec, mock_a2dp_dec, "ba-a2dp-sbc", true) == 0);
			break;
#if ENABLE_APTX
		case A2DP_CODEC_VENDOR_APTX:
			assert(ba_transport_thread_create(&t->thread_dec, mock_a2dp_dec, "ba-a2dp-aptx", true) == 0);
			break;
#endif
#if ENABLE_APTX_HD
		case A2DP_CODEC_VENDOR_APTX_HD:
			assert(ba_transport_thread_create(&t->thread_dec, mock_a2dp_dec, "ba-a2dp-aptx-hd", true) == 0);
			break;
#endif
		}
	}
	else if (t->type.profile & BA_TRANSPORT_PROFILE_MASK_SCO) {
		assert(ba_transport_start(t) == 0);
	}

}

static int mock_transport_acquire(struct ba_transport *t) {

	int bt_fds[2];
	assert(socketpair(AF_UNIX, SOCK_SEQPACKET, 0, bt_fds) == 0);

	t->bt_fd = bt_fds[0];
	t->mtu_read = 256;
	t->mtu_write = 256;

	debug("New transport: %d (MTU: R:%zu W:%zu)", t->bt_fd, t->mtu_read, t->mtu_write);

	pthread_mutex_unlock(&t->bt_fd_mtx);
	mock_transport_start(t, bt_fds[1]);
	pthread_mutex_lock(&t->bt_fd_mtx);

	return 0;
}

static struct ba_device *mock_device_new(struct ba_adapter *a, const char *btmac) {

	bdaddr_t addr;
	str2ba(btmac, &addr);

	struct ba_device *d;
	if ((d = ba_device_lookup(a, &addr)) == NULL) {
		d = ba_device_new(a, &addr);
		d->battery.charge = 75;
	}

	return d;
}

static struct ba_transport *mock_transport_new_a2dp(const char *device_btmac,
		uint16_t profile, const struct a2dp_codec *codec, const void *configuration) {

	usleep(fuzzing_ms * 1000);

	struct ba_device *d = mock_device_new(a, device_btmac);
	struct ba_transport_type type = { profile, codec->codec_id };
	const char *owner = g_dbus_connection_get_unique_name(config.dbus);
	const char *path = g_dbus_transport_type_to_bluez_object_path(type);

	struct ba_transport *t = ba_transport_new_a2dp(d, type, owner, path, codec, configuration);
	t->acquire = mock_transport_acquire;

	fprintf(stderr, "BLUEALSA_PCM_READY=A2DP:%s:%s\n",
			device_btmac, a2dp_codecs_codec_id_to_string(t->type.codec));

	if (type.profile == BA_TRANSPORT_PROFILE_A2DP_SINK)
		assert(ba_transport_acquire(t) == 0);

	ba_device_unref(d);
	return t;
}

static void *mock_transport_rfcomm_thread(void *userdata) {

	static const struct {
		const char *command;
		const char *response;
	} responses[] = {
		/* accept HFP codec selection */
		{ "\r\n+BCS:1\r\n", "AT+BCS=1\r" },
		{ "\r\n+BCS:2\r\n", "AT+BCS=2\r" },
	};

	int rfcomm_fd = GPOINTER_TO_INT(userdata);
	char buffer[1024];
	ssize_t len;

	while ((len = read(rfcomm_fd, buffer, sizeof(buffer))) > 0) {
		hexdump("RFCOMM", buffer, len, true);

		for (size_t i = 0; i < ARRAYSIZE(responses); i++) {
			if (strncmp(buffer, responses[i].command, len) != 0)
				continue;
			len = strlen(responses[i].response);
			if (write(rfcomm_fd, responses[i].response, len) != len)
				warn("Couldn't write RFCOMM response: %s", strerror(errno));
			break;
		}

	}

	close(rfcomm_fd);
	return NULL;
}

static struct ba_transport *mock_transport_new_sco(const char *device_btmac,
		uint16_t profile, uint16_t codec) {

	usleep(fuzzing_ms * 1000);

	struct ba_device *d = mock_device_new(a, device_btmac);
	struct ba_transport_type type = { profile, codec };
	const char *owner = g_dbus_connection_get_unique_name(config.dbus);
	const char *path = g_dbus_transport_type_to_bluez_object_path(type);

	int fds[2];
	socketpair(AF_UNIX, SOCK_STREAM, 0, fds);
	g_thread_unref(g_thread_new(NULL, mock_transport_rfcomm_thread, GINT_TO_POINTER(fds[1])));

	struct ba_transport *t = ba_transport_new_sco(d, type, owner, path, fds[0]);
	t->sco.rfcomm->state = HFP_SLC_CONNECTED;
#if ENABLE_MSBC
	t->sco.rfcomm->codecs.msbc = true;
#endif
	t->acquire = mock_transport_acquire;

	fprintf(stderr, "BLUEALSA_PCM_READY=SCO:%s:%s\n",
			device_btmac, hfp_codec_id_to_string(t->type.codec));

	ba_device_unref(d);
	return t;
}

static GDBusPropertyInfo bluez_iface_device_Alias = {
	-1, "Alias", "s", G_DBUS_PROPERTY_INFO_FLAGS_READABLE, NULL
};

static GDBusPropertyInfo *bluez_iface_device_properties[] = {
	&bluez_iface_device_Alias,
	NULL,
};

static GDBusInterfaceInfo bluez_iface_device = {
	-1, BLUEZ_IFACE_DEVICE,
	NULL,
	NULL,
	bluez_iface_device_properties,
	NULL,
};

static GVariant *bluez_device_get_property(GDBusConnection *conn,
		const char *sender, const char *path, const char *iface,
		const char *property, GError **error, void *userdata) {
	(void)conn;
	(void)sender;
	(void)iface;
	(void)error;
	(void)userdata;

	bdaddr_t addr;
	char addrstr[18];
	ba2str(g_dbus_bluez_object_path_to_bdaddr(path, &addr), addrstr);

	if (strcmp(property, "Alias") == 0) {

		for (size_t i = 0; i < ARRAYSIZE(devices); i++)
			if (devices[i] != NULL &&
					strncmp(devices[i], addrstr, sizeof(addrstr) - 1) == 0)
				return g_variant_new_string(&devices[i][sizeof(addrstr)]);

		*error = g_error_new(G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_PROPERTY,
				"Device alias/name not available");
		return NULL;
	}

	g_assert_not_reached();
	return NULL;
}

static const GDBusInterfaceVTable bluez_device_vtable = {
	.get_property = bluez_device_get_property,
};

static void dbus_name_acquired_bluez(GDBusConnection *conn, const char *name, void *userdata) {
	(void)conn;
	(void)name;
	g_dbus_connection_register_object(conn, "/org/bluez/hci0/dev_12_34_56_78_9A_BC",
			&bluez_iface_device, &bluez_device_vtable, userdata, NULL, NULL);
	g_dbus_connection_register_object(conn, "/org/bluez/hci0/dev_23_45_67_89_AB_CD",
			&bluez_iface_device, &bluez_device_vtable, userdata, NULL, NULL);
}

static void *mock_bluealsa_service_thread(void *userdata) {

	GMainLoop *loop = userdata;
	GPtrArray *tt = g_ptr_array_new();
	size_t i;

	if (config.profile.a2dp_source) {

		if (a2dp_sbc_source.enabled)
			g_ptr_array_add(tt, mock_transport_new_a2dp("12:34:56:78:9A:BC",
						BA_TRANSPORT_PROFILE_A2DP_SOURCE, &a2dp_sbc_source,
						&config_sbc_44100_stereo));

#if ENABLE_APTX_HD
		if (a2dp_aptx_hd_source.enabled)
			g_ptr_array_add(tt, mock_transport_new_a2dp("23:45:67:89:AB:CD",
						BA_TRANSPORT_PROFILE_A2DP_SOURCE, &a2dp_aptx_hd_source,
						&config_aptx_hd_48000_stereo));
		else
#endif
#if ENABLE_APTX
		if (a2dp_aptx_source.enabled)
			g_ptr_array_add(tt, mock_transport_new_a2dp("23:45:67:89:AB:CD",
						BA_TRANSPORT_PROFILE_A2DP_SOURCE, &a2dp_aptx_source,
						&config_aptx_44100_stereo));
		else
#endif
#if ENABLE_FASTSTREAM
		if (a2dp_faststream_source.enabled)
			g_ptr_array_add(tt, mock_transport_new_a2dp("23:45:67:89:AB:CD",
						BA_TRANSPORT_PROFILE_A2DP_SOURCE, &a2dp_faststream_source,
						&config_faststream_44100_16000));
		else
#endif
		if (a2dp_sbc_source.enabled)
			g_ptr_array_add(tt, mock_transport_new_a2dp("23:45:67:89:AB:CD",
						BA_TRANSPORT_PROFILE_A2DP_SOURCE, &a2dp_sbc_source,
						&config_sbc_44100_stereo));

	}

	if (config.profile.a2dp_sink) {

#if ENABLE_APTX_HD
		if (a2dp_aptx_hd_sink.enabled)
			g_ptr_array_add(tt, mock_transport_new_a2dp("12:34:56:78:9A:BC",
						BA_TRANSPORT_PROFILE_A2DP_SINK, &a2dp_aptx_hd_sink,
						&config_aptx_hd_48000_stereo));
		else
#endif
#if ENABLE_APTX
		if (a2dp_aptx_sink.enabled)
			g_ptr_array_add(tt, mock_transport_new_a2dp("12:34:56:78:9A:BC",
						BA_TRANSPORT_PROFILE_A2DP_SINK, &a2dp_aptx_sink,
						&config_aptx_44100_stereo));
		else
#endif
		if (a2dp_sbc_sink.enabled)
			g_ptr_array_add(tt, mock_transport_new_a2dp("12:34:56:78:9A:BC",
						BA_TRANSPORT_PROFILE_A2DP_SINK, &a2dp_sbc_sink,
						&config_sbc_44100_stereo));

		if (a2dp_sbc_sink.enabled)
			g_ptr_array_add(tt, mock_transport_new_a2dp("23:45:67:89:AB:CD",
						BA_TRANSPORT_PROFILE_A2DP_SINK, &a2dp_sbc_sink,
						&config_sbc_44100_stereo));

	}

	if (config.profile.hfp_ag) {

		struct ba_transport *t;
		g_ptr_array_add(tt, t = mock_transport_new_sco("12:34:56:78:9A:BC",
					BA_TRANSPORT_PROFILE_HFP_AG, HFP_CODEC_UNDEFINED));

		if (fuzzing_ms) {
			t->type.codec = HFP_CODEC_CVSD;
			bluealsa_dbus_pcm_update(&t->sco.spk_pcm,
					BA_DBUS_PCM_UPDATE_SAMPLING | BA_DBUS_PCM_UPDATE_CODEC);
			bluealsa_dbus_pcm_update(&t->sco.mic_pcm,
					BA_DBUS_PCM_UPDATE_SAMPLING | BA_DBUS_PCM_UPDATE_CODEC);
		}

	}

	if (config.profile.hsp_ag) {
		g_ptr_array_add(tt, mock_transport_new_sco("23:45:67:89:AB:CD",
					BA_TRANSPORT_PROFILE_HSP_AG, HFP_CODEC_UNDEFINED));
	}

	g_mutex_lock(&timeout_mutex);
	while (timeout_ms > 0)
		g_cond_wait(&timeout_cond, &timeout_mutex);
	g_mutex_unlock(&timeout_mutex);

	for (i = 0; i < tt->len; i++) {
		usleep(fuzzing_ms * 1000);
		ba_transport_destroy(tt->pdata[i]);
	}

	usleep(fuzzing_ms * 1000);

	g_ptr_array_free(tt, TRUE);
	g_main_loop_quit(loop);
	return NULL;
}

static void dbus_name_acquired_bluealsa(GDBusConnection *conn, const char *name, void *userdata) {
	(void)conn;

	fprintf(stderr, "BLUEALSA_DBUS_SERVICE_NAME=%s\n", name);

	/* do not generate lots of data */
	config.sbc_quality = SBC_QUALITY_LOW;

	/* initialize codec capabilities */
	a2dp_codecs_init();

	/* emulate dummy test HCI device */
	assert((a = ba_adapter_new(0)) != NULL);

	/* make HCI mSBC-ready */
	a->hci.features[2] = LMP_TRSP_SCO;
	a->hci.features[3] = LMP_ESCO;

	/* run actual BlueALSA mock thread */
	g_thread_new(NULL, mock_bluealsa_service_thread, userdata);

}

int main(int argc, char *argv[]) {

	int opt;
	const char *opts = "hB:p:c:t:";
	struct option longopts[] = {
		{ "help", no_argument, NULL, 'h' },
		{ "dbus", required_argument, NULL, 'B' },
		{ "profile", required_argument, NULL, 'p' },
		{ "codec", required_argument, NULL, 'c' },
		{ "timeout", required_argument, NULL, 't' },
		{ "device-name", required_argument, NULL, 2 },
		{ "dump-output", no_argument, NULL, 6 },
		{ "fuzzing", required_argument, NULL, 7 },
		{ 0, 0, 0, 0 },
	};

	while ((opt = getopt_long(argc, argv, opts, longopts, NULL)) != -1)
		switch (opt) {
		case 'h':
			printf("Usage:\n"
					"  %s [OPTION]...\n"
					"\nOptions:\n"
					"  -h, --help\t\t\tprint this help and exit\n"
					"  -B, --dbus=NAME\t\tBlueALSA service name suffix\n"
					"  -p, --profile=NAME\t\tset enabled BT profiles\n"
					"  -c, --codec=NAME\t\tset enabled BT audio codecs\n"
					"  -t, --timeout=MSEC\t\tmock server exit timeout\n"
					"  --device-name=MAC:NAME\tmock BT device name\n"
					"  --dump-output\t\t\tdump Bluetooth transport data\n"
					"  --fuzzing=MSEC\t\tmock human actions with timings\n",
					argv[0]);
			return EXIT_SUCCESS;
		case 'B' /* --dbus=NAME */ :
			snprintf(service, sizeof(service), BLUEALSA_SERVICE ".%s", optarg);
			break;
		case 'p' /* --profile=NAME */ : {

			static const struct {
				const char *name;
				bool *ptr;
			} map[] = {
				{ "a2dp-source", &config.profile.a2dp_source },
				{ "a2dp-sink", &config.profile.a2dp_sink },
				{ "hfp-ag", &config.profile.hfp_ag },
				{ "hsp-ag", &config.profile.hsp_ag },
			};

			bool matched = false;
			for (size_t i = 0; i < ARRAYSIZE(map); i++)
				if (strcasecmp(optarg, map[i].name) == 0) {
					*map[i].ptr = true;
					matched = true;
					break;
				}

			if (!matched) {
				error("Invalid BT profile name: %s", optarg);
				return EXIT_FAILURE;
			}

			break;
		}
		case 'c' /* --codec=NAME */ : {

			uint16_t codec_id = a2dp_codecs_codec_id_from_string(optarg);
			bool matched = false;

			struct a2dp_codec * const * cc = a2dp_codecs;
			for (struct a2dp_codec *c = *cc; c != NULL; c = *++cc)
				if (c->codec_id == codec_id) {
					c->enabled = true;
					matched = true;
				}

			if (!matched) {
				error("Invalid BT codec name: %s", optarg);
				return EXIT_FAILURE;
			}

			break;
		}
		case 't' /* --timeout=MSEC */ :
			timeout_ms = atoi(optarg);
			break;
		case 2 /* --device-name=MAC:NAME */ :
			for (size_t i = 0; i < ARRAYSIZE(devices); i++)
				if (devices[i] == NULL) {
					devices[i] = optarg;
					break;
				}
			break;
		case 6 /* --dump-output */ :
			dump_output = true;
			break;
		case 7 /* --fuzzing=MSEC */ :
			fuzzing_ms = atoi(optarg);
			break;
		default:
			fprintf(stderr, "Try '%s --help' for more information.\n", argv[0]);
			return EXIT_FAILURE;
		}

	log_open(argv[0], false);
	assert(bluealsa_config_init() == 0);
	assert((config.dbus = g_test_dbus_connection_new_sync(NULL)) != NULL);

	assert(mkdir(TEST_BLUEALSA_STORAGE_DIR, 0755) == 0 || errno == EEXIST);
	assert(storage_init(TEST_BLUEALSA_STORAGE_DIR) == 0);

	/* receive EPIPE error code */
	struct sigaction sigact = { .sa_handler = SIG_IGN };
	sigaction(SIGPIPE, &sigact, NULL);

	/* register USR signals handler */
	sigact.sa_handler = mock_sigusr_handler;
	sigaction(SIGUSR1, &sigact, NULL);
	sigaction(SIGUSR2, &sigact, NULL);

	/* main loop with graceful termination handlers */
	GMainLoop *loop = g_main_loop_new(NULL, FALSE);
	g_timeout_add(timeout_ms, main_loop_timeout_handler, NULL);
	g_unix_signal_add(SIGINT, main_loop_exit_handler, loop);
	g_unix_signal_add(SIGTERM, main_loop_exit_handler, loop);

	bluealsa_dbus_register();

	assert(g_bus_own_name_on_connection(config.dbus, BLUEZ_SERVICE,
				G_BUS_NAME_OWNER_FLAGS_NONE, dbus_name_acquired_bluez, NULL,
				loop, NULL) != 0);
	assert(g_bus_own_name_on_connection(config.dbus, service,
				G_BUS_NAME_OWNER_FLAGS_NONE, dbus_name_acquired_bluealsa, NULL,
				loop, NULL) != 0);

	g_main_loop_run(loop);

	ba_adapter_destroy(a);
	return EXIT_SUCCESS;
}

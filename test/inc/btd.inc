/*
 * btd.inc
 * vim: ft=c
 *
 * Copyright (c) 2016-2021 Arkadiusz Bokowy
 *
 * This file is a part of bluez-alsa.
 *
 * This project is licensed under the terms of the MIT license.
 *
 */

#pragma once

#include <ctype.h>
#include <endian.h>
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

#include "a2dp.h"
#include "ba-transport.h"
#include "io.h"
#include "utils.h"
#include "shared/a2dp-codecs.h"
#include "shared/defs.h"
#include "shared/ffb.h"
#include "shared/log.h"

#define BT_DUMP_CURRENT_VERSION 1

/**
 * BT dump mode. */
enum bt_dump_mode {
	BT_DUMP_MODE_A2DP_SOURCE = 1,
	BT_DUMP_MODE_A2DP_SINK,
	BT_DUMP_MODE_SCO,
};

/**
 * BT dump handle. */
struct bt_dump {
	unsigned int version;
	enum bt_dump_mode mode;
	uint16_t transport_codec_id;
	a2dp_t a2dp_configuration;
	size_t a2dp_configuration_size;
	FILE *file;
};

/**
 * Read a single data record from a BT dump file. */
ssize_t bt_dump_read(struct bt_dump *btd, void *data, size_t size) {
	uint16_t len;
	if (fread(&len, sizeof(len), 1, btd->file) != 1)
		return -1;
	size = MIN(be16toh(len), size);
	if (fread(data, 1, size, btd->file) != size)
		return -1;
	return size;
}

/**
 * Write a single data record to a BT dump file. */
ssize_t bt_dump_write(struct bt_dump *btd, const void *data, size_t size) {
	uint16_t len = htobe16(size);
	if (fwrite(&len, sizeof(len), 1, btd->file) != 1)
		return -1;
	if (fwrite(data, 1, size, btd->file) != size)
		return -1;
	fflush(btd->file);
	return sizeof(len) + size;
}

/**
 * Close BT dump file. */
void bt_dump_close(struct bt_dump *btd) {
	if (btd->file != NULL)
		fclose(btd->file);
	free(btd);
}

/**
 * Create BT dump file. */
struct bt_dump *bt_dump_create(
		const char *path,
		const struct ba_transport *t) {

	struct bt_dump *btd;
	if ((btd = calloc(1, sizeof(*btd))) == NULL)
		return NULL;

	if ((btd->file = fopen(path, "wb")) == NULL)
		goto fail;

	enum bt_dump_mode mode = 0;
	const void *a2dp_configuration = NULL;
	size_t a2dp_configuration_size = 0;

	switch (t->type.profile) {
	case BA_TRANSPORT_PROFILE_A2DP_SOURCE:
		a2dp_configuration = &t->a2dp.configuration;
		a2dp_configuration_size = t->a2dp.codec->capabilities_size;
		mode = BT_DUMP_MODE_A2DP_SOURCE;
		break;
	case BA_TRANSPORT_PROFILE_A2DP_SINK:
		a2dp_configuration = &t->a2dp.configuration;
		a2dp_configuration_size = t->a2dp.codec->capabilities_size;
		mode = BT_DUMP_MODE_A2DP_SINK;
		break;
	case BA_TRANSPORT_PROFILE_HFP_AG:
	case BA_TRANSPORT_PROFILE_HFP_HF:
	case BA_TRANSPORT_PROFILE_HSP_AG:
	case BA_TRANSPORT_PROFILE_HSP_HS:
		mode = BT_DUMP_MODE_SCO;
		break;
	}

	unsigned int mode_ = mode;
	fprintf(btd->file, "BA.dump-%1x.%1x.", BT_DUMP_CURRENT_VERSION, mode_);

	btd->transport_codec_id = t->type.codec;
	uint16_t id = htobe16(t->type.codec);
	if (bt_dump_write(btd, &id, sizeof(id)) == -1)
		goto fail;

	if (a2dp_configuration != NULL) {
		memcpy(&btd->a2dp_configuration, a2dp_configuration,
				MIN(a2dp_configuration_size, sizeof(btd->a2dp_configuration)));
		btd->a2dp_configuration_size = a2dp_configuration_size;
		if (bt_dump_write(btd, a2dp_configuration, a2dp_configuration_size) == -1)
			goto fail;
	}

	return btd;

fail:
	bt_dump_close(btd);
	return NULL;
}

/**
 * Open BT dump file. */
struct bt_dump *bt_dump_open(const char *path) {

	struct bt_dump *btd;
	if ((btd = calloc(1, sizeof(*btd))) == NULL)
		return NULL;

	if ((btd->file = fopen(path, "rb")) == NULL)
		goto fail;

	unsigned int mode;
	if (fscanf(btd->file, "BA.dump-%1x.%1x.", &btd->version, &mode) != 2) {
		errno = EIO;
		goto fail;
	}

	if (btd->version > BT_DUMP_CURRENT_VERSION) {
		errno = EINVAL;
		goto fail;
	}

	uint16_t id;
	if (bt_dump_read(btd, &id, sizeof(id)) == -1)
		goto fail;
	btd->transport_codec_id = be16toh(id);

	switch (btd->mode = mode) {
	case BT_DUMP_MODE_A2DP_SOURCE:
	case BT_DUMP_MODE_A2DP_SINK: {
		ssize_t size = sizeof(btd->a2dp_configuration);
		if ((size = bt_dump_read(btd, &btd->a2dp_configuration, size)) == -1)
			goto fail;
		btd->a2dp_configuration_size = size;
	} break;
	case BT_DUMP_MODE_SCO:
		break;
	default:
		errno = EINVAL;
		goto fail;
	}

	return btd;

fail:
	bt_dump_close(btd);
	return NULL;
}

static const char *transport_type_to_fname(struct ba_transport_type type) {

	static char buffer[64];
	char prev = '-';
	size_t i = 0;

	const char *ptr = ba_transport_type_to_string(type);
	for (; *ptr != '\0'; ptr++) {
		char c = *ptr;
		if (!isalnum(c))
			c = '-';
		buffer[i++] = c;
		if (c == '-' && prev == '-')
			i--;
		prev = c;
	}

	if (prev == '-')
		i--;
	buffer[i] = '\0';

	return buffer;
}

/**
 * Dump incoming BT data to a BT dump file. */
void *bt_dump_io_thread(struct ba_transport_thread *th) {

	pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);
	pthread_cleanup_push(PTHREAD_CLEANUP(ba_transport_thread_cleanup), th);

	struct ba_transport *t = th->t;
	struct io_poll io = { .timeout = -1 };
	ffb_t bt = { 0 };

	struct bt_dump *btd = NULL;
	char fname[128];

	snprintf(fname, sizeof(fname), "/tmp/bluealsa-%s.btd",
			transport_type_to_fname(t->type));

	debug("Creating BT dump file: %s", fname);
	if ((btd = bt_dump_create(fname, t)) == NULL) {
		error("Couldn't create BT dump file: %s", strerror(errno));
		goto fail_open;
	}

	pthread_cleanup_push(PTHREAD_CLEANUP(ffb_free), &bt);
	pthread_cleanup_push(PTHREAD_CLEANUP(bt_dump_close), btd);

	if (ffb_init_uint8_t(&bt, t->mtu_read) == -1) {
		error("Couldn't create data buffer: %s", strerror(errno));
		goto fail_ffb;
	}

	debug_transport_thread_loop(th, "START");
	for (ba_transport_thread_set_state_running(th);;) {

		ssize_t len = ffb_blen_in(&bt);
		if ((len = io_poll_and_read_bt(&io, th, bt.data, len)) <= 0) {
			if (len == -1)
				error("BT poll and read error: %s", strerror(errno));
			goto fail;
		}

		debug("BT read: %zd", len);
		bt_dump_write(btd, bt.data, len);

	}

fail:
	debug_transport_thread_loop(th, "EXIT");
	ba_transport_thread_set_state_stopping(th);
	pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);
fail_ffb:
	pthread_cleanup_pop(1);
	pthread_cleanup_pop(1);
fail_open:
	pthread_cleanup_pop(1);
	return NULL;
}

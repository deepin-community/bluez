// SPDX-License-Identifier: GPL-2.0-or-later
/*
 *
 *  BlueZ - Bluetooth protocol stack for Linux
 *
 *  Copyright (C) 2022  Intel Corporation.
 *
 *
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#define _GNU_SOURCE
#include <unistd.h>
#include <string.h>
#include <sys/socket.h>
#include <fcntl.h>

#include <glib.h>

#include "lib/bluetooth.h"
#include "lib/uuid.h"
#include "src/shared/util.h"
#include "src/shared/io.h"
#include "src/shared/tester.h"
#include "src/shared/queue.h"
#include "src/shared/att.h"
#include "src/shared/gatt-db.h"
#include "src/shared/gatt-client.h"
#include "src/shared/bap.h"
#include "src/shared/lc3.h"

struct test_config {
	struct bt_bap_pac_qos pqos;
	struct iovec cc;
	struct bt_bap_qos qos;
	bool snk;
	bool src;
	bool vs;
};

struct test_data {
	struct bt_gatt_client *client;
	struct gatt_db *db;
	struct bt_bap *bap;
	struct bt_bap_pac *snk;
	struct bt_bap_pac *src;
	struct iovec *caps;
	struct test_config *cfg;
	struct bt_bap_stream *stream;
	size_t iovcnt;
	struct iovec *iov;
};

/*
 * Frequencies: 8Khz 11Khz 16Khz 22Khz 24Khz 32Khz 44.1Khz 48Khz
 * Duration: 7.5 ms 10 ms
 * Channel count: 3
 * Frame length: 30-240
 */
static struct iovec lc3_caps = LC3_CAPABILITIES(LC3_FREQ_ANY, LC3_DURATION_ANY,
								3u, 30, 240);

#define iov_data(args...) ((const struct iovec[]) { args })

#define define_test(name, function, _cfg, args...)		\
	do {							\
		const struct iovec iov[] = { args };		\
		static struct test_data data;			\
		data.caps = &lc3_caps;				\
		data.cfg = _cfg;				\
		data.iovcnt = ARRAY_SIZE(iov_data(args));	\
		data.iov = util_iov_dup(iov, ARRAY_SIZE(iov_data(args))); \
		tester_add(name, &data, test_setup, function,	\
				test_teardown);			\
	} while (0)

static void client_ready_cb(bool success, uint8_t att_ecode, void *user_data)
{
	if (!success)
		tester_setup_failed();
	else
		tester_setup_complete();
}

/* GATT Discover All procedure */
static const struct iovec setup_data[] = {
	/* ATT: Exchange MTU Response (0x03) len 2
	 *   Server RX MTU: 64
	 */
	IOV_DATA(0x02, 0x40, 0x00),
	/* ATT: Exchange MTU Request (0x02) len 2
	 *    Client RX MTU: 64
	 */
	IOV_DATA(0x03, 0x40, 0x00),
	/* ATT: Read By Type Request (0x08) len 6
	 *   Handle range: 0x0001-0xffff
	 *   Attribute type: Server Supported Features (0x2b3a)
	 */
	IOV_DATA(0x08, 0x01, 0x00, 0xff, 0xff, 0x3a, 0x2b),
	/* ATT: Error Response (0x01) len 4
	 *   Read By Type Request (0x08)
	 *   Handle: 0x0001
	 *   Error: Attribute Not Found (0x0a)
	 */
	IOV_DATA(0x01, 0x08, 0x01, 0x00, 0x0a),
	/*
	 * ATT: Read By Group Type Request (0x10) len 6
	 *   Handle range: 0x0001-0xffff
	 *   Attribute group type: Primary Service (0x2800)
	 */
	IOV_DATA(0x10, 0x01, 0x00, 0xff, 0xff, 0x00, 0x28),
	/*
	 * ATT: Read By Group Type Response (0x11) len 37
	 *   Attribute data length: 6
	 *   Attribute group list: 2 entries
	 *   Handle range: 0x0001-0x0013
	 *   UUID: Published Audio Capabilities (0x1850)
	 *   Handle range: 0x0014-0x0023
	 *   UUID: Audio Stream Control (0x184e)
	 */
	IOV_DATA(0x11, 0x06,
		0x01, 0x00, 0x13, 0x00, 0x50, 0x18,
		0x14, 0x00, 0x23, 0x00, 0x4e, 0x18),
	/* ATT: Read By Group Type Request (0x10) len 6
	 *   Handle range: 0x0024-0xffff
	 *   Attribute group type: Primary Service (0x2800)
	 */
	IOV_DATA(0x10, 0x24, 0x00, 0xff, 0xff, 0x00, 0x28),
	/* ATT: Error Response (0x01) len 4
	 *   Read By Group Type Request (0x10)
	 *   Handle: 0x0024
	 *   Error: Attribute Not Found (0x0a)
	 */
	IOV_DATA(0x01, 0x10, 0x24, 0x00, 0x0a),
	/* ATT: Read By Group Type Request (0x10) len 6
	 *   Handle range: 0x0001-0xffff
	 *   Attribute group type: Secondary Service (0x2801)
	 */
	IOV_DATA(0x10, 0x01, 0x00, 0xff, 0xff, 0x01, 0x28),
	/* ATT: Error Response (0x01) len 4
	 *   Read By Group Type Request (0x10)
	 *   Handle: 0x0001
	 *   Error: Attribute Not Found (0x0a)
	 */
	IOV_DATA(0x01, 0x10, 0x01, 0x00, 0x0a),
	/* ATT: Read By Type Request (0x08) len 6
	 *   Handle range: 0x0001-0x0023
	 *   Attribute group type: Include (0x2802)
	 */
	IOV_DATA(0x08, 0x01, 0x00, 0x23, 0x00, 0x02, 0x28),
	/* ATT: Error Response (0x01) len 4
	 *   Read By Group Type Request (0x10)
	 *   Handle: 0x0001
	 *   Error: Attribute Not Found (0x0a)
	 */
	IOV_DATA(0x01, 0x08, 0x01, 0x00, 0x0a),
	/* ATT: Read By Type Request (0x08) len 6
	 *   Handle range: 0x0001-0x0023
	 *   Attribute type: Characteristic (0x2803)
	 */
	IOV_DATA(0x08, 0x01, 0x00, 0x23, 0x00, 0x03, 0x28),
	/* ATT: Read By Type Response (0x09) len 57
	 * Attribute data length: 7
	 * Attribute data list: 8 entries
	 *   Handle: 0x0002
	 *   Value: 120300c92b
	 *   Properties: 0x12
	 *     Read (0x02)
	 *     Notify (0x10)
	 *   Value Handle: 0x0003
	 *   Value UUID: Sink PAC (0x2bc9)
	 *   Handle: 0x0005
	 *   Value: 120600ca2b
	 *   Properties: 0x12
	 *     Read (0x02)
	 *     Notify (0x10)
	 *   Value Handle: 0x0006
	 *   Value UUID: Sink Audio Locations (0x2bca)
	 *   Handle: 0x0008
	 *   Value: 120900cb2b
	 *   Properties: 0x12
	 *     Read (0x02)
	 *     Notify (0x10)
	 *   Value Handle: 0x0009
	 *   Value UUID: Source PAC (0x2bcb)
	 *   Handle: 0x000b
	 *   Value: 120c00cc2b
	 *   Properties: 0x12
	 *     Read (0x02)
	 *     Notify (0x10)
	 *  Value Handle: 0x000c
	 *  Value UUID: Source Audio Locations (0x2bcc)
	 *  Handle: 0x000e
	 *  Value: 120f00cd2b
	 *  Properties: 0x12
	 *    Read (0x02)
	 *    Notify (0x10)
	 *  Value Handle: 0x000f
	 *  Value UUID: Available Audio Contexts (0x2bcd)
	 *  Handle: 0x0011
	 *  Value: 121200ce2b
	 *  Properties: 0x12
	 *    Read (0x02)
	 *    Notify (0x10)
	 *  Value Handle: 0x0012
	 *  Value UUID: Supported Audio Contexts (0x2bce)
	 *  Handle: 0x0015
	 *  Value: 121600c42b
	 *  Properties: 0x12
	 *    Read (0x02)
	 *    Notify (0x10)
	 *  Value Handle: 0x0016
	 *  Value UUID: Sink ASE (0x2bc4)
	 *  Handle: 0x0018
	 *  Value: 121900c42b
	 *  Properties: 0x12
	 *    Read (0x02)
	 *    Notify (0x10)
	 *  Value Handle: 0x0019
	 *  Value UUID: Sink ASE (0x2bc4)
	 */
	IOV_DATA(0x09, 0x07,
		0x02, 0x00, 0x12, 0x03, 0x00, 0xc9, 0x2b,
		0x05, 0x00, 0x12, 0x06, 0x00, 0xca, 0x2b,
		0x08, 0x00, 0x12, 0x09, 0x00, 0xcb, 0x2b,
		0x0b, 0x00, 0x12, 0x0c, 0x00, 0xcc, 0x2b,
		0x0e, 0x00, 0x12, 0x0f, 0x00, 0xcd, 0x2b,
		0x11, 0x00, 0x12, 0x12, 0x00, 0xce, 0x2b,
		0x15, 0x00, 0x12, 0x16, 0x00, 0xc4, 0x2b,
		0x18, 0x00, 0x12, 0x19, 0x00, 0xc4, 0x2b),
	/* ATT: Read By Type Request (0x08) len 6
	 *   Handle range: 0x0001-0x0023
	 *   Attribute type: Characteristic (0x2803)
	 */
	IOV_DATA(0x08, 0x19, 0x00, 0x23, 0x00, 0x03, 0x28),
	/* ATT: Read By Type Response (0x09) len 22
	 * Attribute data length: 7
	 * Attribute data list: 3 entries
	 *   Handle: 0x001b
	 *   Value: 121c00c52b
	 *   Properties: 0x12
	 *     Read (0x02)
	 *     Notify (0x10)
	 *   Value Handle: 0x001c
	 *   Value UUID: Source ASE (0x2bc5)
	 *   Handle: 0x001e
	 *   Value: 121f00c52b
	 *   Properties: 0x12
	 *     Read (0x02)
	 *     Notify (0x10)
	 *   Value Handle: 0x001f
	 *   Value UUID: Source ASE (0x2bc5)
	 *   Handle: 0x0021
	 *   Value: 182200c62b
	 *   Properties: 0x18
	 *     Write (0x08)
	 *     Notify (0x10)
	 *   Value Handle: 0x0022
	 *   Value UUID: ASE Control Point (0x2bc6)
	 */
	IOV_DATA(0x09, 0x07,
		0x1b, 0x00, 0x12, 0x1c, 0x00, 0xc5, 0x2b,
		0x1e, 0x00, 0x12, 0x1f, 0x00, 0xc5, 0x2b,
		0x21, 0x00, 0x18, 0x22, 0x00, 0xc6, 0x2b),
	/* ATT: Read By Type Request (0x08) len 6
	 *   Handle range: 0x0022-0x0023
	 *   Attribute type: Characteristic (0x2803)
	 */
	IOV_DATA(0x08, 0x22, 0x00, 0x23, 0x00, 0x03, 0x28),
	/* ATT: Error Response (0x01) len 4
	 *   Read By Type Request (0x08)
	 *   Handle: 0x0022
	 *   Error: Attribute Not Found (0x0a)
	 */
	IOV_DATA(0x01, 0x08, 0x23, 0x00, 0x0a),
	/* ACL Data TX: Handle 42 flags 0x00 dlen 11
	 *   ATT: Read By Type Request (0x08) len 6
	 *   Handle range: 0x0001-0xffff
	 *   Attribute type: Database Hash (0x2b2a)
	 */
	IOV_DATA(0x08, 0x01, 0x00, 0xff, 0xff, 0x2a, 0x2b),
	/* ATT: Error Response (0x01) len 4
	 *   Read By Type Request (0x08)
	 *   Handle: 0x0001
	 *   Error: Attribute Not Found (0x0a)
	 */
	IOV_DATA(0x01, 0x08, 0x01, 0x00, 0x0a),
};

static void print_debug(const char *str, void *user_data)
{
	const char *prefix = user_data;

	if (tester_use_debug())
		tester_debug("%s%s", prefix, str);
}

static void test_setup(const void *user_data)
{
	struct test_data *data = (void *)user_data;
	struct bt_att *att;
	struct gatt_db *db;
	struct io *io;

	io = tester_setup_io(setup_data, ARRAY_SIZE(setup_data));
	g_assert(io);

	att = bt_att_new(io_get_fd(io), false);
	g_assert(att);

	bt_att_set_debug(att, BT_ATT_DEBUG, print_debug, "bt_att:", NULL);

	db = gatt_db_new();
	g_assert(db);

	data->client = bt_gatt_client_new(db, att, 64, 0);
	g_assert(data->client);

	bt_gatt_client_set_debug(data->client, print_debug, "bt_gatt_client:",
						NULL);

	bt_gatt_client_ready_register(data->client, client_ready_cb, data,
						NULL);

	bt_att_unref(att);
	gatt_db_unref(db);
}

static void test_complete_cb(const void *user_data)
{
	tester_test_passed();
}

static void bap_config(struct bt_bap_stream *stream,
					uint8_t code, uint8_t reason,
					void *user_data)
{
	if (code)
		tester_test_failed();
}

static bool pac_found(struct bt_bap_pac *lpac, struct bt_bap_pac *rpac,
							void *user_data)
{
	struct test_data *data = user_data;
	unsigned int config_id;

	data->stream = bt_bap_stream_new(data->bap, lpac, rpac,
						&data->cfg->qos,
						&data->cfg->cc);
	g_assert(data->stream);

	config_id = bt_bap_stream_config(data->stream, &data->cfg->qos,
					&data->cfg->cc, bap_config, data);
	g_assert(config_id);

	return true;
}

static void bap_ready(struct bt_bap *bap, void *user_data)
{
	bt_bap_foreach_pac(bap, BT_BAP_SINK, pac_found, user_data);
	bt_bap_foreach_pac(bap, BT_BAP_SOURCE, pac_found, user_data);
}

static void test_client_config(struct test_data *data)
{
	if (!data->cfg)
		return;

	if (data->cfg->src) {
		if (data->cfg->vs)
			data->snk = bt_bap_add_vendor_pac(data->db,
							"test-bap-snk",
							BT_BAP_SINK, 0x0ff,
							0x0001, 0x0001,
							NULL, data->caps, NULL);
		else
			data->snk = bt_bap_add_pac(data->db, "test-bap-snk",
							BT_BAP_SINK, LC3_ID,
							NULL, data->caps, NULL);
		g_assert(data->snk);
	}

	if (data->cfg->snk) {
		if (data->cfg->vs)
			data->src = bt_bap_add_vendor_pac(data->db,
							"test-bap-src",
							BT_BAP_SOURCE, 0x0ff,
							0x0001, 0x0001,
							NULL, data->caps, NULL);
		else
			data->src = bt_bap_add_pac(data->db, "test-bap-src",
							BT_BAP_SOURCE, LC3_ID,
							NULL, data->caps, NULL);
		g_assert(data->src);
	}
}

static void test_client(const void *user_data)
{
	struct test_data *data = (void *)user_data;
	struct io *io;

	io = tester_setup_io(data->iov, data->iovcnt);
	g_assert(io);

	tester_io_set_complete_func(test_complete_cb);

	data->db = gatt_db_new();
	g_assert(data->db);

	test_client_config(data);

	data->bap = bt_bap_new(data->db, bt_gatt_client_get_db(data->client));
	g_assert(data->bap);

	bt_bap_set_debug(data->bap, print_debug, "bt_bap:", NULL);

	bt_bap_ready_register(data->bap, bap_ready, data, NULL);

	bt_bap_attach(data->bap, data->client);
}

static void test_teardown(const void *user_data)
{
	struct test_data *data = (void *)user_data;

	bt_bap_unref(data->bap);
	bt_gatt_client_unref(data->client);
	util_iov_free(data->iov, data->iovcnt);

	bt_bap_remove_pac(data->snk);
	bt_bap_remove_pac(data->src);
	gatt_db_unref(data->db);

	tester_teardown_complete();
}

/* ATT: Read Request (0x0a) len 2
 *   Handle: 0x0003 Type: Sink PAC (0x2bc9)
 * ATT: Read Response (0x0b) len 24
 *   Value: 010600000000100301ff0002020302030305041e00f00000
 *   Handle: 0x0003 Type: Sink PAC (0x2bc9)
 *     Number of PAC(s): 1
 *       PAC #0:
 *         Codec: LC3 (0x06)
 *         Codec Specific Capabilities #0: len 0x03 type 0x01
 *         Sampling Frequencies: 0x00ff
 *           8 Khz (0x0001)
 *           11.25 Khz (0x0002)
 *           16 Khz (0x0004)
 *           22.05 Khz (0x0008)
 *           24 Khz (0x0010)
 *           32 Khz (0x0020)
 *           44.1 Khz (0x0040)
 *           48 Khz (0x0080)
 *       Codec Specific Capabilities #1: len 0x02 type 0x02
 *         Frame Duration: 0x0003
 *           7.5 ms (0x01)
 *           10 ms (0x02)
 *       Codec Specific Capabilities #2: len 0x02 type 0x03
 *         Audio Channel Count: 0x03
 *           1 channel (0x01)
 *           2 channels (0x02)
 *       Codec Specific Capabilities #3: len 0x05 type 0x04
 *         Frame Length: 30 (0x001e) - 240 (0x00f0)
 * ATT: Read Request (0x0a) len 2
 *   Handle: 0x0006 Type: Sink Audio Location (0x2bca)
 * ATT: Read Response (0x0b) len 4
 *   Value: 03000000
 *   Handle: 0x0006 Type: Sink Audio Locations (0x2bca)
 *     Location: 0x00000003
 *       Front Left (0x00000001)
 *       Front Right (0x00000002)
 */
#define DISC_SNK_PAC(_caps...) \
	IOV_DATA(0x0a, 0x03, 0x00), \
	IOV_DATA(0x0b, 0x01, _caps), \
	IOV_DATA(0x0a, 0x06, 0x00), \
	IOV_DATA(0x0b, 0x03, 0x00, 0x00, 0x00)

#define DISC_SNK_LC3 \
	DISC_SNK_PAC(0x06, 0x00, 0x00, 0x00, 0x00, 0x10, 0x03, 0x01, \
		0xff, 0x00, 0x02, 0x02, 0x03, 0x02, 0x03, 0x03, 0x05, 0x04, \
		0x1e, 0x00, 0xf0, 0x00, 0x00)

/* ATT: Read Request (0x0a) len 2
 *   Handle: 0x0009 Type: Source PAC (0x2bcb)
 * ATT: Read Response (0x0b) len 24
 *   Value: 010600000000100301ff0002020302030305041e00f00000
 *   Handle: 0x0009 Type: Source PAC (0x2bcb)
 *     Number of PAC(s): 1
 *       PAC #0:
 *         Codec: LC3 (0x06)
 *         Codec Specific Capabilities #0: len 0x03 type 0x01
 *         Sampling Frequencies: 0x00ff
 *           8 Khz (0x0001)
 *           11.25 Khz (0x0002)
 *           16 Khz (0x0004)
 *           22.05 Khz (0x0008)
 *           24 Khz (0x0010)
 *           32 Khz (0x0020)
 *           44.1 Khz (0x0040)
 *           48 Khz (0x0080)
 *       Codec Specific Capabilities #1: len 0x02 type 0x02
 *         Frame Duration: 0x0003
 *           7.5 ms (0x01)
 *           10 ms (0x02)
 *       Codec Specific Capabilities #2: len 0x02 type 0x03
 *         Audio Channel Count: 0x03
 *           1 channel (0x01)
 *           2 channels (0x02)
 *       Codec Specific Capabilities #3: len 0x05 type 0x04
 *         Frame Length: 30 (0x001e) - 240 (0x00f0)
 * ATT: Read Request (0x0a) len 2
 *   Handle: 0x000c Type: Source Audio Location (0x2bcc)
 * ATT: Read Response (0x0b) len 4
 *   Value: 03000000
 *   Handle: 0x000c Type: Source Audio Locations (0x2bcc)
 *     Location: 0x00000003
 *       Front Left (0x00000001)
 *       Front Right (0x00000002)
 */
#define DISC_SRC_PAC(_caps...) \
	DISC_SNK_PAC(_caps), \
	IOV_DATA(0x0a, 0x09, 0x00), \
	IOV_DATA(0x0b, 0x01, _caps), \
	IOV_DATA(0x0a, 0x0c, 0x00), \
	IOV_DATA(0x0b, 0x03, 0x00, 0x00, 0x00)

#define DISC_SRC_LC3 \
	DISC_SRC_PAC(0x06, 0x00, 0x00, 0x00, 0x00, 0x10, 0x03, 0x01, \
		0xff, 0x00, 0x02, 0x02, 0x03, 0x02, 0x03, 0x03, 0x05, 0x04, \
		0x1e, 0x00, 0xf0, 0x00, 0x00)

/* ATT: Read Request (0x0a) len 2
 *   Handle: 0x000f Type: Available Audio Contexts (0x2bcd)
 * ATT: Read Response (0x0b) len 4
 *   Value: ff0f0e00
 *   Handle: 0x000f Type: Available Audio Contexts (0x2bcd)
 */
#define DISC_CTX(_caps...) \
	DISC_SRC_PAC(_caps), \
	IOV_DATA(0x0a, 0x0f, 0x00), \
	IOV_DATA(0x0b, 0xff, 0x0f, 0x0e, 0x00)

#define DISC_CTX_LC3 \
	DISC_CTX(0x06, 0x00, 0x00, 0x00, 0x00, 0x10, 0x03, 0x01, \
		0xff, 0x00, 0x02, 0x02, 0x03, 0x02, 0x03, 0x03, 0x05, 0x04, \
		0x1e, 0x00, 0xf0, 0x00, 0x00)

/* ATT: Read Request (0x0a) len 2
 *   Handle: 0x0012 Type: Supported Audio Contexts (0x2bce)
 * ATT: Read Response (0x0b) len 4
 *   Value: ff0f0e00
 *   Handle: 0x0012 Type: Supported Audio Contexts (0x2bce)
 */
#define DISC_SUP_CTX(_caps...) \
	DISC_CTX(_caps), \
	IOV_DATA(0x0a, 0x12, 0x00), \
	IOV_DATA(0x0b, 0xff, 0x0f, 0x0e, 0x00)

#define DISC_SUP_CTX_LC3 \
	DISC_SUP_CTX(0x06, 0x00, 0x00, 0x00, 0x00, 0x10, 0x03, 0x01, \
		0xff, 0x00, 0x02, 0x02, 0x03, 0x02, 0x03, 0x03, 0x05, 0x04, \
		0x1e, 0x00, 0xf0, 0x00, 0x00)

/* ATT: Read Request (0x0a) len 2
 *   Handle: 0x0016 Type: Sink ASE (0x2bc4)
 * ATT: Read Response (0x0b) len 4
 *   Value: 0100
 *   Handle: 0x0016 Type: Sink ASE (0x2bc4)
 * ATT: Write Request (0x12) len 4
 *   Handle: 0x0017 Type: Client Characteristic Configuration (0x2902)
 *     Data: 0100
 *       Notification (0x01)
 * ATT: Write Response (0x13) len 0
 * ATT: Read Request (0x0a) len 2
 *   Handle: 0x0019 Type: Sink ASE (0x2bc4)
 * ATT: Read Response (0x0b) len 4
 *   Value: 0200
 *   Handle: 0x0019 Type: Sink ASE (0x2bc4)
 * ATT: Write Request (0x12) len 4
 *   Handle: 0x001a Type: Client Characteristic Configuration (0x2902)
 *     Data: 0100
 *       Notification (0x01)
 * ATT: Write Response (0x13) len 0
 */
#define DISC_SNK_ASE(_caps...) \
	DISC_SUP_CTX(_caps), \
	IOV_DATA(0x0a, 0x16, 0x00), \
	IOV_DATA(0x0b, 0x01, 0x00), \
	IOV_DATA(0x12, 0x17, 0x00, 0x01, 0x00), \
	IOV_DATA(0x13), \
	IOV_DATA(0x0a, 0x19, 0x00), \
	IOV_DATA(0x0b, 0x02, 0x00), \
	IOV_DATA(0x12, 0x1a, 0x00, 0x01, 0x00), \
	IOV_DATA(0x13)

#define DISC_SNK_ASE_LC3 \
	DISC_SNK_ASE(0x06, 0x00, 0x00, 0x00, 0x00, 0x10, 0x03, 0x01, \
		0xff, 0x00, 0x02, 0x02, 0x03, 0x02, 0x03, 0x03, 0x05, 0x04, \
		0x1e, 0x00, 0xf0, 0x00, 0x00)

/* ATT: Read Request (0x0a) len 2
 *   Handle: 0x001c Type: Source ASE (0x2bc5)
 * ATT: Read Response (0x0b) len 4
 *   Value: 0300
 *   Handle: 0x001c Type: Source ASE (0x2bc5)
 * ATT: Write Request (0x12) len 4
 *   Handle: 0x001d Type: Client Characteristic Configuration (0x2902)
 *     Data: 0100
 *       Notification (0x01)
 * ATT: Write Response (0x13) len 0
 * ATT: Read Request (0x0a) len 2
 *   Handle: 0x001f Type: Source ASE (0x2bc5)
 * ATT: Read Response (0x0b) len 4
 *   Value: 0400
 *   Handle: 0x001f Type: Source ASE (0x2bc5)
 * ATT: Write Request (0x12) len 4
 *   Handle: 0x0020 Type: Client Characteristic Configuration (0x2902)
 *     Data: 0100
 *       Notification (0x01)
 * ATT: Write Response (0x13) len 0
 * ATT: Write Request (0x12) len 4
 *   Handle: 0x0023 Type: Client Characteristic Configuration (0x2902)
 *     Data: 0100
 *       Notification (0x01)
 * ATT: Write Response (0x13) len 0
 */
#define DISC_SRC_ASE(_cfg...) \
	DISC_SNK_ASE(_cfg), \
	IOV_DATA(0x0a, 0x1c, 0x00), \
	IOV_DATA(0x0b, 0x03, 0x00), \
	IOV_DATA(0x12, 0x1d, 0x00, 0x01, 0x00), \
	IOV_DATA(0x13), \
	IOV_DATA(0x0a, 0x1f, 0x00), \
	IOV_DATA(0x0b, 0x04, 0x00), \
	IOV_DATA(0x12, 0x20, 0x00, 0x01, 0x00), \
	IOV_DATA(0x13), \
	IOV_DATA(0x12, 0x23, 0x00, 0x01, 0x00), \
	IOV_DATA(0x13)

#define DISC_SRC_ASE_LC3 \
	DISC_SRC_ASE(0x06, 0x00, 0x00, 0x00, 0x00, 0x10, 0x03, 0x01, \
		0xff, 0x00, 0x02, 0x02, 0x03, 0x02, 0x03, 0x03, 0x05, 0x04, \
		0x1e, 0x00, 0xf0, 0x00, 0x00)

static void test_disc(void)
{
	/* The IUT discovers the characteristics specified in the PAC
	 * Characteristic and Location Characteristic columns in Table 4.4.
	 * The IUT reads the values of the characteristics specified in the PAC
	 * Characteristic and Location Characteristic columns.
	 */
	define_test("BAP/UCL/DISC/BV-01-C", test_client, NULL, DISC_SNK_LC3);
	define_test("BAP/UCL/DISC/BV-02-C", test_client, NULL, DISC_SRC_LC3);

	/* BAP/UCL/DISC/BV-06-C [Discover Available Audio Contexts]
	 *
	 * The IUT successfully reads the value of the Available Audio Contexts
	 * characteristic on the LowerTester.
	 */
	define_test("BAP/UCL/DISC/BV-06-C", test_client, NULL, DISC_CTX_LC3);

	/* BAP/UCL/DISC/BV-05-C [Discover Supported Audio Contexts]
	 *
	 * The IUT successfully reads the value of the Supported Audio Contexts
	 * characteristic on the Lower Tester.
	 */
	define_test("BAP/UCL/DISC/BV-05-C", test_client, NULL,
						DISC_SUP_CTX_LC3);

	/* BAP/UCL/DISC/BV-03-C [Discover Sink ASE_ID]
	 * BAP/UCL/DISC/BV-04-C [Discover Source ASE_ID]
	 *
	 * The IUT successfully reads the ASE_ID values of each discovered ASE
	 * characteristic on the LowerTester.
	 */
	define_test("BAP/UCL/DISC/BV-03-C", test_client, NULL,
						DISC_SNK_ASE_LC3);
	define_test("BAP/UCL/DISC/BV-04-C", test_client, NULL,
						DISC_SRC_ASE_LC3);
}

/* ATT: Write Command (0x52) len 23
 *  Handle: 0x0022
 *    Data: 0101010202_cfg
 * ATT: Handle Value Notification (0x1b) len 7
 *   Handle: 0x0022
 *     Data: 0101010000
 * ATT: Handle Value Notification (0x1b) len 37
 *   Handle: 0x0016
 *     Data: 01010102010a00204e00409c00204e00409c00_cfg
 */
#define SCC_SNK(_cfg...) \
	IOV_DATA(0x52, 0x22, 0x00, 0x01, 0x01, 0x01, 0x02, 0x02, _cfg), \
	IOV_DATA(0x1b, 0x22, 0x00, 0x01, 0x01, 0x01, 0x00, 0x00), \
	IOV_NULL, \
	IOV_DATA(0x1b, 0x16, 0x00, 0x01, 0x01, 0x01, 0x02, 0x01, 0x0a, 0x00, \
			0x20, 0x4e, 0x00, 0x40, 0x9c, 0x00, 0x20, 0x4e, 0x00, \
			0x40, 0x9c, 0x00, _cfg)

#define SCC_SNK_LC3(_cc...) \
	DISC_SRC_ASE_LC3, \
	SCC_SNK(0x06, 0x00, 0x00, 0x00, 0x00, _cc)

#define QOS_BALANCED_2M \
	{ \
		.target_latency = BT_BAP_CONFIG_LATENCY_BALANCED, \
		.io_qos.phy = BT_BAP_CONFIG_PHY_2M, \
	}
#define QOS_UCAST \
{\
	.ucast = QOS_BALANCED_2M, \
}
static struct test_config cfg_snk_8_1 = {
	.cc = LC3_CONFIG_8_1,
	.qos = QOS_UCAST,
	.snk = true,
};

#define SCC_SNK_8_1 \
	SCC_SNK_LC3(0x0a, 0x02, 0x01, 0x01, 0x02, 0x02, 0x00, 0x03, 0x04, \
			0x1a, 0x00)

static struct test_config cfg_snk_8_2 = {
	.cc = LC3_CONFIG_8_2,
	.qos = QOS_UCAST,
	.snk = true,
};

#define SCC_SNK_8_2 \
	SCC_SNK_LC3(0x0a, 0x02, 0x01, 0x01, 0x02, 0x02, 0x01, 0x03, 0x04, \
			0x1e, 0x00)

static struct test_config cfg_snk_16_1 = {
	.cc = LC3_CONFIG_16_1,
	.qos = QOS_UCAST,
	.snk = true,
};

#define SCC_SNK_16_1 \
	SCC_SNK_LC3(0x0a, 0x02, 0x01, 0x03, 0x02, 0x02, 0x00, 0x03, 0x04, \
			0x1e, 0x00)

static struct test_config cfg_snk_16_2 = {
	.cc = LC3_CONFIG_16_2,
	.qos = QOS_UCAST,
	.snk = true,
};

#define SCC_SNK_16_2 \
	SCC_SNK_LC3(0x0a, 0x02, 0x01, 0x03, 0x02, 0x02, 0x01, 0x03, 0x04, \
			0x28, 0x00)

static struct test_config cfg_snk_24_1 = {
	.cc = LC3_CONFIG_24_1,
	.qos = QOS_UCAST,
	.snk = true,
};

#define SCC_SNK_24_1 \
	SCC_SNK_LC3(0x0a, 0x02, 0x01, 0x05, 0x02, 0x02, 0x00, 0x03, 0x04, \
			0x2d, 0x00)

static struct test_config cfg_snk_24_2 = {
	.cc = LC3_CONFIG_24_2,
	.qos = QOS_UCAST,
	.snk = true,
};

#define SCC_SNK_24_2 \
	SCC_SNK_LC3(0x0a, 0x02, 0x01, 0x05, 0x02, 0x02, 0x01, 0x03, 0x04, \
			0x3c, 0x00)

static struct test_config cfg_snk_32_1 = {
	.cc = LC3_CONFIG_32_1,
	.qos = QOS_UCAST,
	.snk = true,
};

#define SCC_SNK_32_1 \
	SCC_SNK_LC3(0x0a, 0x02, 0x01, 0x06, 0x02, 0x02, 0x00, 0x03, 0x04, \
			0x3c, 0x00)

static struct test_config cfg_snk_32_2 = {
	.cc = LC3_CONFIG_32_2,
	.qos = QOS_UCAST,
	.snk = true,
};

#define SCC_SNK_32_2 \
	SCC_SNK_LC3(0x0a, 0x02, 0x01, 0x06, 0x02, 0x02, 0x01, 0x03, 0x04, \
			0x50, 0x00)

static struct test_config cfg_snk_44_1 = {
	.cc = LC3_CONFIG_44_1,
	.qos = QOS_UCAST,
	.snk = true,
};

#define SCC_SNK_44_1 \
	SCC_SNK_LC3(0x0a, 0x02, 0x01, 0x07, 0x02, 0x02, 0x00, 0x03, 0x04, \
			0x62, 0x00)

static struct test_config cfg_snk_44_2 = {
	.cc = LC3_CONFIG_44_2,
	.qos = QOS_UCAST,
	.snk = true,
};

#define SCC_SNK_44_2 \
	SCC_SNK_LC3(0x0a, 0x02, 0x01, 0x07, 0x02, 0x02, 0x01, 0x03, 0x04, \
			0x82, 0x00)

static struct test_config cfg_snk_48_1 = {
	.cc = LC3_CONFIG_48_1,
	.qos = QOS_UCAST,
	.snk = true,
};

#define SCC_SNK_48_1 \
	SCC_SNK_LC3(0x0a, 0x02, 0x01, 0x08, 0x02, 0x02, 0x00, 0x03, 0x04, \
			0x4b, 0x00)

static struct test_config cfg_snk_48_2 = {
	.cc = LC3_CONFIG_48_2,
	.qos = QOS_UCAST,
	.snk = true,
};

#define SCC_SNK_48_2 \
	SCC_SNK_LC3(0x0a, 0x02, 0x01, 0x08, 0x02, 0x02, 0x01, 0x03, 0x04, \
			0x64, 0x00)

static struct test_config cfg_snk_48_3 = {
	.cc = LC3_CONFIG_48_3,
	.qos = QOS_UCAST,
	.snk = true,
};

#define SCC_SNK_48_3 \
	SCC_SNK_LC3(0x0a, 0x02, 0x01, 0x08, 0x02, 0x02, 0x00, 0x03, 0x04, \
			0x5a, 0x00)

static struct test_config cfg_snk_48_4 = {
	.cc = LC3_CONFIG_48_4,
	.qos = QOS_UCAST,
	.snk = true,
};

#define SCC_SNK_48_4 \
	SCC_SNK_LC3(0x0a, 0x02, 0x01, 0x08, 0x02, 0x02, 0x01, 0x03, 0x04, \
			0x78, 0x00)

static struct test_config cfg_snk_48_5 = {
	.cc = LC3_CONFIG_48_5,
	.qos = QOS_UCAST,
	.snk = true,
};

#define SCC_SNK_48_5 \
	SCC_SNK_LC3(0x0a, 0x02, 0x01, 0x08, 0x02, 0x02, 0x00, 0x03, 0x04, \
			0x75, 0x00)

static struct test_config cfg_snk_48_6 = {
	.cc = LC3_CONFIG_48_6,
	.qos = QOS_UCAST,
	.snk = true,
};

#define SCC_SNK_48_6 \
	SCC_SNK_LC3(0x0a, 0x02, 0x01, 0x08, 0x02, 0x02, 0x01, 0x03, 0x04, \
			0x9b, 0x00)

/* ATT: Write Command (0x52) len 23
 *  Handle: 0x0022
 *    Data: 0101030202_cfg
 * ATT: Handle Value Notification (0x1b) len 7
 *   Handle: 0x0022
 *     Data: 0101030000
 * ATT: Handle Value Notification (0x1b) len 37
 *   Handle: 0x001c
 *     Data: 03010102010a00204e00409c00204e00409c00_cfg
 */
#define SCC_SRC(_cfg...) \
	IOV_DATA(0x52, 0x22, 0x00, 0x01, 0x01, 0x03, 0x02, 0x02, _cfg), \
	IOV_DATA(0x1b, 0x22, 0x00, 0x01, 0x01, 0x03, 0x00, 0x00), \
	IOV_NULL, \
	IOV_DATA(0x1b, 0x1c, 0x00, 0x03, 0x01, 0x01, 0x02, 0x01, 0x0a, 0x00, \
			0x20, 0x4e, 0x00, 0x40, 0x9c, 0x00, 0x20, 0x4e, 0x00, \
			0x40, 0x9c, 0x00, _cfg)

#define SCC_SRC_LC3(_cc...) \
	DISC_SRC_ASE_LC3, \
	SCC_SRC(0x06, 0x00, 0x00, 0x00, 0x00, _cc)

static struct test_config cfg_src_8_1 = {
	.cc = LC3_CONFIG_8_1,
	.qos = QOS_UCAST,
	.src = true,
};

#define SCC_SRC_8_1 \
	SCC_SRC_LC3(0x0a, 0x02, 0x01, 0x01, 0x02, 0x02, 0x00, 0x03, 0x04, \
			    0x1a, 0x00)

static struct test_config cfg_src_8_2 = {
	.cc = LC3_CONFIG_8_2,
	.qos = QOS_UCAST,
	.src = true,
};

#define SCC_SRC_8_2 \
	SCC_SRC_LC3(0x0a, 0x02, 0x01, 0x01, 0x02, 0x02, 0x01, 0x03, 0x04, \
			0x1e, 0x00)

static struct test_config cfg_src_16_1 = {
	.cc = LC3_CONFIG_16_1,
	.qos = QOS_UCAST,
	.src = true,
};

#define SCC_SRC_16_1 \
	SCC_SRC_LC3(0x0a, 0x02, 0x01, 0x03, 0x02, 0x02, 0x00, 0x03, 0x04, \
			0x1e, 0x00)

static struct test_config cfg_src_16_2 = {
	.cc = LC3_CONFIG_16_2,
	.qos = QOS_UCAST,
	.src = true,
};

#define SCC_SRC_16_2 \
	SCC_SRC_LC3(0x0a, 0x02, 0x01, 0x03, 0x02, 0x02, 0x01, 0x03, 0x04, \
			0x28, 0x00)

static struct test_config cfg_src_24_1 = {
	.cc = LC3_CONFIG_24_1,
	.qos = QOS_UCAST,
	.src = true,
};

#define SCC_SRC_24_1 \
	SCC_SRC_LC3(0x0a, 0x02, 0x01, 0x05, 0x02, 0x02, 0x00, 0x03, 0x04, \
			0x2d, 0x00)

static struct test_config cfg_src_24_2 = {
	.cc = LC3_CONFIG_24_2,
	.qos = QOS_UCAST,
	.src = true,
};

#define SCC_SRC_24_2 \
	SCC_SRC_LC3(0x0a, 0x02, 0x01, 0x05, 0x02, 0x02, 0x01, 0x03, 0x04, \
			0x3c, 0x00)

static struct test_config cfg_src_32_1 = {
	.cc = LC3_CONFIG_32_1,
	.qos = QOS_UCAST,
	.src = true,
};

#define SCC_SRC_32_1 \
	SCC_SRC_LC3(0x0a, 0x02, 0x01, 0x06, 0x02, 0x02, 0x00, 0x03, 0x04, \
			0x3c, 0x00)

static struct test_config cfg_src_32_2 = {
	.cc = LC3_CONFIG_32_2,
	.qos = QOS_UCAST,
	.src = true,
};

#define SCC_SRC_32_2 \
	SCC_SRC_LC3(0x0a, 0x02, 0x01, 0x06, 0x02, 0x02, 0x01, 0x03, 0x04, \
			0x50, 0x00)

static struct test_config cfg_src_44_1 = {
	.cc = LC3_CONFIG_44_1,
	.qos = QOS_UCAST,
	.src = true,
};

#define SCC_SRC_44_1 \
	SCC_SRC_LC3(0x0a, 0x02, 0x01, 0x07, 0x02, 0x02, 0x00, 0x03, 0x04, \
			0x62, 0x00)

static struct test_config cfg_src_44_2 = {
	.cc = LC3_CONFIG_44_2,
	.qos = QOS_UCAST,
	.src = true,
};

#define SCC_SRC_44_2 \
	SCC_SRC_LC3(0x0a, 0x02, 0x01, 0x07, 0x02, 0x02, 0x01, 0x03, 0x04, \
			0x82, 0x00)

static struct test_config cfg_src_48_1 = {
	.cc = LC3_CONFIG_48_1,
	.qos = QOS_UCAST,
	.src = true,
};

#define SCC_SRC_48_1 \
	SCC_SRC_LC3(0x0a, 0x02, 0x01, 0x08, 0x02, 0x02, 0x00, 0x03, 0x04, \
			0x4b, 0x00)

static struct test_config cfg_src_48_2 = {
	.cc = LC3_CONFIG_48_2,
	.qos = QOS_UCAST,
	.src = true,
};

#define SCC_SRC_48_2 \
	SCC_SRC_LC3(0x0a, 0x02, 0x01, 0x08, 0x02, 0x02, 0x01, 0x03, 0x04, \
			0x64, 0x00)

static struct test_config cfg_src_48_3 = {
	.cc = LC3_CONFIG_48_3,
	.qos = QOS_UCAST,
	.src = true,
};

#define SCC_SRC_48_3 \
	SCC_SRC_LC3(0x0a, 0x02, 0x01, 0x08, 0x02, 0x02, 0x00, 0x03, 0x04, \
			0x5a, 0x00)

static struct test_config cfg_src_48_4 = {
	.cc = LC3_CONFIG_48_4,
	.qos = QOS_UCAST,
	.src = true,
};

#define SCC_SRC_48_4 \
	SCC_SRC_LC3(0x0a, 0x02, 0x01, 0x08, 0x02, 0x02, 0x01, 0x03, 0x04, \
			0x78, 0x00)

static struct test_config cfg_src_48_5 = {
	.cc = LC3_CONFIG_48_5,
	.qos = QOS_UCAST,
	.src = true,
};

#define SCC_SRC_48_5 \
	SCC_SRC_LC3(0x0a, 0x02, 0x01, 0x08, 0x02, 0x02, 0x00, 0x03, 0x04, \
			0x75, 0x00)

static struct test_config cfg_src_48_6 = {
	.cc = LC3_CONFIG_48_6,
	.qos = QOS_UCAST,
	.src = true,
};

#define SCC_SRC_48_6 \
	SCC_SRC_LC3(0x0a, 0x02, 0x01, 0x08, 0x02, 0x02, 0x01, 0x03, 0x04, \
			0x9b, 0x00)

/* Test Purpose:
 * Verify that a Unicast Client IUT can initiate a Config Codec
 * operation for an LC3 codec.
 *
 * Pass verdict:
 * The IUT successfully writes to the ASE Control point with the opcode
 * set to 0x01 (Config Codec) and correctly formatted parameter values
 * from Table 4.9. The Codec_ID field is a 5-octet field with octet 0
 * set to the LC3 Coding_Format value defined in Bluetooth Assigned
 * Numbers, octets 1–4 set to 0x0000. Each parameter (if present)
 * included in the data sent in Codec_Specific_Configuration is
 * formatted in an LTV structure with the length, type, and value
 * specified in Table 4.10.
 */
static void test_scc_cc_lc3(void)
{
	define_test("BAP/UCL/SCC/BV-001-C [UCL SRC Config Codec, LC3 8_1]",
			test_client, &cfg_snk_8_1, SCC_SNK_8_1);
	define_test("BAP/UCL/SCC/BV-002-C [UCL SRC Config Codec, LC3 8_2]",
			test_client, &cfg_snk_8_2, SCC_SNK_8_2);
	define_test("BAP/UCL/SCC/BV-003-C [UCL SRC Config Codec, LC3 16_1]",
			test_client, &cfg_snk_16_1, SCC_SNK_16_1);
	define_test("BAP/UCL/SCC/BV-004-C [UCL SRC Config Codec, LC3 16_2]",
			test_client, &cfg_snk_16_2, SCC_SNK_16_2);
	define_test("BAP/UCL/SCC/BV-005-C [UCL SRC Config Codec, LC3 24_1]",
			test_client, &cfg_snk_24_1, SCC_SNK_24_1);
	define_test("BAP/UCL/SCC/BV-006-C [UCL SRC Config Codec, LC3 24_2]",
			test_client, &cfg_snk_24_2, SCC_SNK_24_2);
	define_test("BAP/UCL/SCC/BV-007-C [UCL SRC Config Codec, LC3 32_1]",
			test_client, &cfg_snk_32_1, SCC_SNK_32_1);
	define_test("BAP/UCL/SCC/BV-008-C [UCL SRC Config Codec, LC3 32_2]",
			test_client, &cfg_snk_32_2, SCC_SNK_32_2);
	define_test("BAP/UCL/SCC/BV-009-C [UCL SRC Config Codec, LC3 44.1_1]",
			test_client, &cfg_snk_44_1, SCC_SNK_44_1);
	define_test("BAP/UCL/SCC/BV-010-C [UCL SRC Config Codec, LC3 44.1_2]",
			test_client, &cfg_snk_44_2, SCC_SNK_44_2);
	define_test("BAP/UCL/SCC/BV-011-C [UCL SRC Config Codec, LC3 48_1]",
			test_client, &cfg_snk_48_1, SCC_SNK_48_1);
	define_test("BAP/UCL/SCC/BV-012-C [UCL SRC Config Codec, LC3 48_2]",
			test_client, &cfg_snk_48_2, SCC_SNK_48_2);
	define_test("BAP/UCL/SCC/BV-013-C [UCL SRC Config Codec, LC3 48_3]",
			test_client, &cfg_snk_48_3, SCC_SNK_48_3);
	define_test("BAP/UCL/SCC/BV-014-C [UCL SRC Config Codec, LC3 48_4]",
			test_client, &cfg_snk_48_4, SCC_SNK_48_4);
	define_test("BAP/UCL/SCC/BV-015-C [UCL SRC Config Codec, LC3 48_5]",
			test_client, &cfg_snk_48_5, SCC_SNK_48_5);
	define_test("BAP/UCL/SCC/BV-016-C [UCL SRC Config Codec, LC3 48_6]",
			test_client, &cfg_snk_48_6, SCC_SNK_48_6);
	define_test("BAP/UCL/SCC/BV-017-C [UCL SNK Config Codec, LC3 8_1]",
			test_client, &cfg_src_8_1, SCC_SRC_8_1);
	define_test("BAP/UCL/SCC/BV-018-C [UCL SNK Config Codec, LC3 8_2]",
			test_client, &cfg_src_8_2, SCC_SRC_8_2);
	define_test("BAP/UCL/SCC/BV-019-C [UCL SNK Config Codec, LC3 16_1]",
			test_client, &cfg_src_16_1, SCC_SRC_16_1);
	define_test("BAP/UCL/SCC/BV-020-C [UCL SNK Config Codec, LC3 16_2]",
			test_client, &cfg_src_16_2, SCC_SRC_16_2);
	define_test("BAP/UCL/SCC/BV-021-C [UCL SNK Config Codec, LC3 24_1]",
			test_client, &cfg_src_24_1, SCC_SRC_24_1);
	define_test("BAP/UCL/SCC/BV-022-C [UCL SNK Config Codec, LC3 24_2]",
			test_client, &cfg_src_24_2, SCC_SRC_24_2);
	define_test("BAP/UCL/SCC/BV-023-C [UCL SNK Config Codec, LC3 32_1]",
			test_client, &cfg_src_32_1, SCC_SRC_32_1);
	define_test("BAP/UCL/SCC/BV-024-C [UCL SNK Config Codec, LC3 32_2]",
			test_client, &cfg_src_32_2, SCC_SRC_32_2);
	define_test("BAP/UCL/SCC/BV-025-C [UCL SNK Config Codec, LC3 44.1_1]",
			test_client, &cfg_src_44_1, SCC_SRC_44_1);
	define_test("BAP/UCL/SCC/BV-026-C [UCL SNK Config Codec, LC3 44.1_2]",
			test_client, &cfg_src_44_2, SCC_SRC_44_2);
	define_test("BAP/UCL/SCC/BV-027-C [UCL SNK Config Codec, LC3 48_1]",
			test_client, &cfg_src_48_1, SCC_SRC_48_1);
	define_test("BAP/UCL/SCC/BV-028-C [UCL SNK Config Codec, LC3 48_2]",
			test_client, &cfg_src_48_2, SCC_SRC_48_2);
	define_test("BAP/UCL/SCC/BV-029-C [UCL SNK Config Codec, LC3 48_3]",
			test_client, &cfg_src_48_3, SCC_SRC_48_3);
	define_test("BAP/UCL/SCC/BV-030-C [UCL SNK Config Codec, LC3 48_4]",
			test_client, &cfg_src_48_4, SCC_SRC_48_4);
	define_test("BAP/UCL/SCC/BV-031-C [UCL SNK Config Codec, LC3 48_5]",
			test_client, &cfg_src_48_5, SCC_SRC_48_5);
	define_test("BAP/UCL/SCC/BV-032-C [UCL SNK Config Codec, LC3 48_6]",
			test_client, &cfg_src_48_6, SCC_SRC_48_6);
}

static struct test_config cfg_snk_vs = {
	.cc = IOV_NULL,
	.qos = QOS_UCAST,
	.snk = true,
	.vs = true,
};

#define DISC_SRC_ASE_VS \
	DISC_SRC_ASE(0xff, 0x01, 0x00, 0x01, 0x00, 0x00, 0x00)

#define SCC_SNK_VS \
	DISC_SRC_ASE_VS,  \
	SCC_SNK(0xff, 0x01, 0x00, 0x01, 0x00, 0x00)

static struct test_config cfg_src_vs = {
	.cc = IOV_NULL,
	.qos = QOS_UCAST,
	.src = true,
	.vs = true,
};

#define SCC_SRC_VS \
	DISC_SRC_ASE_VS,  \
	SCC_SRC(0xff, 0x01, 0x00, 0x01, 0x00, 0x00)

/* Test Purpose:
 * Verify that a Unicast Client IUT can initiate a Config Codec operation for a
 * vendor-specific codec.
 *
 * Pass verdict:
 * The IUT successfully writes to the ASE Control Point characteristic with the
 * opcode set to 0x01 (Config Codec) and the specified parameters. The Codec_ID
 * parameter is formatted with octet 0 set to 0xFF, octets 1–2 set to
 * TSPX_VS_Company_ID, and octets 3–4 set to TSPX_VS_Codec_ID.
 */
static void test_scc_cc_vs(void)
{
	define_test("BAP/UCL/SCC/BV-033-C [UCL SRC Config Codec, VS]",
			test_client, &cfg_snk_vs, SCC_SNK_VS);
	define_test("BAP/UCL/SCC/BV-034-C [UCL SNK Config Codec, VS]",
			test_client, &cfg_src_vs, SCC_SRC_VS);
}

static void test_scc(void)
{
	test_scc_cc_lc3();
	test_scc_cc_vs();
}

int main(int argc, char *argv[])
{
	tester_init(&argc, &argv);

	test_disc();
	test_scc();

	return tester_run();
}

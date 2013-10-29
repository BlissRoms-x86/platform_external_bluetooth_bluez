/*
 *
 *  BlueZ - Bluetooth protocol stack for Linux
 *
 *  Copyright (C) 2013  Intel Corporation. All rights reserved.
 *
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 *
 */

#include <stdint.h>
#include <stdbool.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>

#include <glib.h>

#include "btio/btio.h"
#include "lib/bluetooth.h"
#include "src/shared/mgmt.h"

#include "log.h"
#include "hal-msg.h"
#include "ipc.h"
#include "hid.h"
#include "adapter.h"
#include "utils.h"

#define L2CAP_PSM_HIDP_CTRL	0x11
#define L2CAP_PSM_HIDP_INTR	0x13
#define MAX_READ_BUFFER		4096

static GIOChannel *notification_io = NULL;
static GSList *devices = NULL;

struct hid_device {
	bdaddr_t	dst;
	GIOChannel	*ctrl_io;
	GIOChannel	*intr_io;
	guint		ctrl_watch;
	guint		intr_watch;
};

static int device_cmp(gconstpointer s, gconstpointer user_data)
{
	const struct hid_device *hdev = s;
	const bdaddr_t *dst = user_data;

	return bacmp(&hdev->dst, dst);
}

static void hid_device_free(struct hid_device *hdev)
{
	if (hdev->ctrl_watch > 0)
		g_source_remove(hdev->ctrl_watch);

	if (hdev->intr_watch > 0)
		g_source_remove(hdev->intr_watch);

	if (hdev->intr_io)
		g_io_channel_unref(hdev->intr_io);

	if (hdev->ctrl_io)
		g_io_channel_unref(hdev->ctrl_io);

	devices = g_slist_remove(devices, hdev);
	g_free(hdev);
}

static gboolean intr_io_watch_cb(GIOChannel *chan, gpointer data)
{
	char buf[MAX_READ_BUFFER];
	int fd, bread;

	fd = g_io_channel_unix_get_fd(chan);
	bread = read(fd, buf, sizeof(buf));
	if (bread < 0) {
		error("read: %s(%d)", strerror(errno), -errno);
		return TRUE;
	}

	DBG("bytes read %d", bread);

	/* TODO: At this moment only baseband is connected, i.e. mouse
	 * movements keyboard events doesn't effect on UI. Have to send
	 * this data to uhid fd for profile connection. */

	return TRUE;
}

static gboolean intr_watch_cb(GIOChannel *chan, GIOCondition cond,
								gpointer data)
{
	struct hid_device *hdev = data;
	char address[18];

	if (cond & G_IO_IN)
		return intr_io_watch_cb(chan, data);

	ba2str(&hdev->dst, address);
	DBG("Device %s disconnected", address);

	/* Checking for ctrl_watch avoids a double g_io_channel_shutdown since
	 * it's likely that ctrl_watch_cb has been queued for dispatching in
	 * this mainloop iteration */
	if ((cond & (G_IO_HUP | G_IO_ERR)) && hdev->ctrl_watch)
		g_io_channel_shutdown(chan, TRUE, NULL);

	hdev->intr_watch = 0;

	if (hdev->intr_io) {
		g_io_channel_unref(hdev->intr_io);
		hdev->intr_io = NULL;
	}

	/* Close control channel */
	if (hdev->ctrl_io && !(cond & G_IO_NVAL))
		g_io_channel_shutdown(hdev->ctrl_io, TRUE, NULL);

	return FALSE;
}

static gboolean ctrl_watch_cb(GIOChannel *chan, GIOCondition cond,
								gpointer data)
{
	struct hid_device *hdev = data;
	char address[18];

	ba2str(&hdev->dst, address);
	DBG("Device %s disconnected", address);

	/* Checking for intr_watch avoids a double g_io_channel_shutdown since
	 * it's likely that intr_watch_cb has been queued for dispatching in
	 * this mainloop iteration */
	if ((cond & (G_IO_HUP | G_IO_ERR)) && hdev->intr_watch)
		g_io_channel_shutdown(chan, TRUE, NULL);

	hdev->ctrl_watch = 0;

	if (hdev->ctrl_io) {
		g_io_channel_unref(hdev->ctrl_io);
		hdev->ctrl_io = NULL;
	}

	if (hdev->intr_io && !(cond & G_IO_NVAL))
		g_io_channel_shutdown(hdev->intr_io, TRUE, NULL);

	return FALSE;
}

static void interrupt_connect_cb(GIOChannel *chan, GError *conn_err,
							gpointer user_data)
{
	struct hid_device *hdev = user_data;

	DBG("");

	if (conn_err)
		goto failed;

	/*TODO: Get device details through SDP and create UHID fd and start
	 * listening on uhid events */
	hdev->intr_watch = g_io_add_watch(hdev->intr_io,
				G_IO_IN | G_IO_HUP | G_IO_ERR | G_IO_NVAL,
				intr_watch_cb, hdev);

	return;

failed:
	/* So we guarantee the interrupt channel is closed before the
	 * control channel (if we only do unref GLib will close it only
	 * after returning control to the mainloop */
	if (!conn_err)
		g_io_channel_shutdown(hdev->intr_io, FALSE, NULL);

	g_io_channel_unref(hdev->intr_io);
	hdev->intr_io = NULL;

	if (hdev->ctrl_io) {
		g_io_channel_unref(hdev->ctrl_io);
		hdev->ctrl_io = NULL;
	}
}

static void control_connect_cb(GIOChannel *chan, GError *conn_err,
							gpointer user_data)
{
	struct hid_device *hdev = user_data;
	GError *err = NULL;
	const bdaddr_t *src = bt_adapter_get_address();

	DBG("");

	if (conn_err) {
		error("%s", conn_err->message);
		goto failed;
	}

	/* Connect to the HID interrupt channel */
	hdev->intr_io = bt_io_connect(interrupt_connect_cb, hdev, NULL, &err,
					BT_IO_OPT_SOURCE_BDADDR, src,
					BT_IO_OPT_DEST_BDADDR, &hdev->dst,
					BT_IO_OPT_PSM, L2CAP_PSM_HIDP_INTR,
					BT_IO_OPT_SEC_LEVEL, BT_IO_SEC_LOW,
					BT_IO_OPT_INVALID);
	if (!hdev->intr_io) {
		error("%s", err->message);
		g_error_free(err);
		goto failed;
	}

	hdev->ctrl_watch = g_io_add_watch(hdev->ctrl_io,
					G_IO_HUP | G_IO_ERR | G_IO_NVAL,
					ctrl_watch_cb, hdev);

	return;

failed:
	g_io_channel_unref(hdev->ctrl_io);
	hdev->ctrl_io = NULL;
}

static uint8_t bt_hid_connect(struct hal_cmd_hid_connect *cmd, uint16_t len)
{
	struct hid_device *hdev;
	char addr[18];
	bdaddr_t dst;
	GSList *l;
	GError *err = NULL;
	const bdaddr_t *src = bt_adapter_get_address();

	DBG("");

	if (len < sizeof(*cmd))
		return HAL_STATUS_INVALID;

	android2bdaddr(&cmd->bdaddr, &dst);

	l = g_slist_find_custom(devices, &dst, device_cmp);
	if (l)
		return HAL_STATUS_FAILED;

	hdev = g_new0(struct hid_device, 1);
	android2bdaddr(&cmd->bdaddr, &hdev->dst);
	ba2str(&hdev->dst, addr);

	DBG("connecting to %s", addr);

	hdev->ctrl_io = bt_io_connect(control_connect_cb, hdev, NULL, &err,
					BT_IO_OPT_SOURCE_BDADDR, src,
					BT_IO_OPT_DEST_BDADDR, &hdev->dst,
					BT_IO_OPT_PSM, L2CAP_PSM_HIDP_CTRL,
					BT_IO_OPT_SEC_LEVEL, BT_IO_SEC_LOW,
					BT_IO_OPT_INVALID);
	if (err) {
		error("%s", err->message);
		g_error_free(err);
		hid_device_free(hdev);
		return HAL_STATUS_FAILED;
	}

	devices = g_slist_append(devices, hdev);

	return HAL_STATUS_SUCCESS;
}

void bt_hid_handle_cmd(GIOChannel *io, uint8_t opcode, void *buf, uint16_t len)
{
	uint8_t status = HAL_STATUS_FAILED;

	switch (opcode) {
	case HAL_OP_HID_CONNECT:
		status = bt_hid_connect(buf, len);
		break;
	case HAL_OP_HID_DISCONNECT:
		break;
	default:
		DBG("Unhandled command, opcode 0x%x", opcode);
		break;
	}

	ipc_send_rsp(io, HAL_SERVICE_ID_HIDHOST, status);
}

bool bt_hid_register(GIOChannel *io, const bdaddr_t *addr)
{
	DBG("");

	notification_io = g_io_channel_ref(io);

	return true;
}

void bt_hid_unregister(void)
{
	DBG("");

	g_io_channel_unref(notification_io);
	notification_io = NULL;
}

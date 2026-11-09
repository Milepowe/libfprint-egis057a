/*
 * egis057a_proto.h -- EGIS/SIGE + 55AA match-on-chip protocol of the
 * LighTuning Technology Inc. EgisTec EH57A (USB 1c7a:057a)
 *
 * Copyright (C) 2026 Philipp Oster (transport architecture, derived from the
 *                                   egis0576 driver this project is based on)
 * Copyright (C) 2026 Milepower      (EH57A match-on-chip protocol)
 *
 * The EH57A stores and matches fingerprints inside the sensor. The host never
 * sees an image; it drives the chip with a small command set:
 *
 *  - Transport:  "EGIS" + cmd + two bytes on bulk EP 0x01, answered by "SIGE" +
 *                echoed selector + status on EP 0x82. An APDU is sent with
 *                "EGIS a0", its answer fetched by polling "EGIS a1".
 *  - Framing:    commands are "55 AA 00 <cmd> <sub> 00 <len16> <payload>" inside
 *                the APDU "00 50 00 00", with a reflected CRC-16-CCITT
 *                (poly 0x8408, init 0) appended.
 *  - Session:    an ECDH P-256 handshake ("00 57 01 00") with an SP800-108
 *                HMAC-SHA256 KDF yields the application key used to derive the
 *                32-byte template id the chip stores for an enrolment.
 *
 * Everything here is asynchronous on the device main loop (FpiSsm machines and
 * FpiUsbTransfer chains); nothing blocks and nothing runs on another thread.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#ifndef EGIS057A_PROTO_H
#define EGIS057A_PROTO_H

#include "drivers_api.h"

#define EGIS057A_MAX_TEMPLATES 10
#define EGIS057A_TEMPLATE_ID_LEN 32

typedef struct Egis057aDev Egis057aDev;

/* Completion of a plain transfer sequence: NULL, or the error (transfer full). */
typedef void (*Egis057aDoneCb) (FpDevice *dev,
                                gpointer  user_data,
                                GError   *error);

/* Completion of a 55AA command: the chip's sub-selector, status byte and
 * payload, or the transport error (transfer full). */
typedef void (*Egis057aCmdCb) (FpDevice     *dev,
                               gpointer      user_data,
                               guint8        rsub,
                               guint8        rstatus,
                               const guint8 *rpl,
                               gsize         rplen,
                               GError       *error);

/* The transport state for one FpDevice. @dev is borrowed and must outlive the
 * Egis057aDev. The USB interface is the caller's to claim and release. */
Egis057aDev *egis057a_dev_new (FpDevice *dev);
void         egis057a_dev_free (Egis057aDev *d);

/* CANCELLATION. No transfer built here is ever handed a GCancellable: aborting
 * an in-flight URB can wedge these sensors at USB level. Instead the machines
 * consult fpi_device_action_is_cancelled() -- and the stop request below -- at
 * their transfer-sequence boundaries. */

/* Ask the running init machine to stop at its next boundary as if the action
 * had been cancelled. Used by the driver's suspend. */
void egis057a_dev_request_stop (Egis057aDev *d);

/* Bring the sensor up: drain the IN endpoint, read the firmware version,
 * calibrate, run the ECDH session handshake and switch the LED off. After a
 * successful run egis057a_has_session() is TRUE. */
FpiSsm  *egis057a_init_ssm (Egis057aDev *d);
gboolean egis057a_has_session (Egis057aDev *d);

/* Send one 55AA command. @keepalive keeps the sensor's "EGIS a3 00 01" LED /
 * scan keepalive running (needed for the commands that wait for a finger).
 * One command at a time per Egis057aDev. */
void egis057a_cmd55aa (Egis057aDev   *d,
                       guint8         cmd,
                       guint8         sub,
                       const guint8  *pl,
                       gsize          plen,
                       gboolean       keepalive,
                       Egis057aCmdCb  cb,
                       gpointer       user_data);

/* Switch the scan keepalive (and with it the sensor's LED) off. */
void egis057a_led_off (Egis057aDev   *d,
                       Egis057aDoneCb cb,
                       gpointer       user_data);

/* template_id = HMAC-SHA256(app_key0, "enroll\0" || nonce): the id the chip
 * is told to store an enrolment under. Needs a session. */
gboolean egis057a_compute_enroll_id (Egis057aDev *d,
                                     const guint8 nonce[EGIS057A_TEMPLATE_ID_LEN],
                                     guint8       out_id[EGIS057A_TEMPLATE_ID_LEN]);

#endif

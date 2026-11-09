/*
 * egis057a_proto.c -- EGIS/SIGE + 55AA match-on-chip protocol of the
 * LighTuning Technology Inc. EgisTec EH57A (USB 1c7a:057a)
 *
 * Copyright (C) 2026 Philipp Oster (transport architecture, derived from the
 *                                   egis0576 driver this project is based on)
 * Copyright (C) 2026 Milepower      (EH57A match-on-chip protocol)
 *
 * FpiSsm machines over FpiUsbTransfer on the device main loop; no thread.
 * See egis057a_proto.h for the protocol overview.
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
#define FP_COMPONENT "egis057a"

#include "drivers_api.h"
#include "egis057a_proto.h"
#include <openssl/core_names.h>
#include <openssl/ec.h>
#include <openssl/evp.h>
#include <openssl/param_build.h>
#include <openssl/rand.h>
#include <string.h>

#define EP_OUT 0x01
#define EP_IN 0x82

/* Transfer sizes and timeouts (ms). */
#define EGIS_REPLY_LEN 64
#define EGIS_CHUNK 4096
#define EGIS_OUT_TIMEOUT 3000
#define EGIS_READ_TIMEOUT 800
#define EGIS_DRAIN_TIMEOUT 30
#define EGIS_DRAIN_MAX 8

struct Egis057aDev
{
  FpDevice      *dev;               /* borrowed */

  gboolean       stop;              /* egis057a_dev_request_stop: honoured like a cancel */
  int            drains;

  /* Session */
  gboolean       has_app_key;
  guint8         app_key0[32];
  EVP_PKEY      *ecdh_pkey;
  guint8         ecdh_crand[32];

  /* APDU transport */
  gboolean       keepalive_active;
  gint64         next_keepalive_us;
  GSource       *apdu_timer;
  gboolean       apdu_use_crc;
  guint8         apdu_tx[1024];
  gsize          apdu_tx_len;
  int            a0_retries;
  int            a1_polls;
  int            sw6f_retries;
  guint8         cmd_byte;
  Egis057aCmdCb  cmd55_cb;
  gpointer       cmd55_ud;
  Egis057aDoneCb led_cb;
  gpointer       led_ud;
};

/* ---------------------------------------------------------------- USB ---- */

/* Every transfer below is submitted with a NULL GCancellable, deliberately.
 * fpi_usb_transfer_submit forwards a cancellable to gusb, which aborts the URB
 * (libusb_cancel_transfer) when it fires, and aborting an in-flight bulk
 * transfer can wedge these sensors at USB level until board power is cut. A
 * timeout never does that, a cancel can hit mid-transfer. So cancellation is a
 * check of fpi_device_action_is_cancelled() between transfer sequences, and the
 * action's cancellable is never given to a transfer. */

/* Fail @ssm with G_IO_ERROR_CANCELLED iff the current action was cancelled or
 * a stop was requested. */

static gboolean
egis_check_cancelled (FpiSsm *ssm, Egis057aDev *d)
{
  if (!d->stop && !fpi_device_action_is_cancelled (d->dev))
    return FALSE;
  fpi_ssm_mark_failed (ssm, g_error_new_literal (G_IO_ERROR, G_IO_ERROR_CANCELLED,
                                                 "cancelled"));
  return TRUE;
}

/* One "EGIS" command: the OUT transfer and, if a reply is expected, the IN
 * transfer that collects it. The callback gets the reply bytes and their
 * count; n < 0 means the sensor said nothing within the timeout (reported,
 * not failed). An OUT failure is handed to the callback as @error (transfer
 * full): a sensor that does not even accept a command is not going to answer
 * the rest of the sequence either, and failing on the first such transfer
 * bounds a dead-sensor action at seconds. */
typedef void (*EgisReplyCb) (Egis057aDev      *d,
                             gpointer      ud,
                             const guint8 *reply,
                             gssize        n,
                             GError       *error);

typedef struct
{
  Egis057aDev    *d;
  gpointer    ud;
  guint       reply_timeout;    /* 0: no reply expected */
  EgisReplyCb cb;
} EgisCmd;

static void
egis_cmd_in_cb (FpiUsbTransfer *t, FpDevice *dev, gpointer ud, GError *error)
{
  EgisCmd *c = ud;
  gssize n = error ? -1 : t->actual_length;

  g_clear_error (&error);
  c->cb (c->d, c->ud, t->buffer, n, NULL);
  g_free (c);
}

static void
egis_cmd_out_cb (FpiUsbTransfer *t, FpDevice *dev, gpointer ud, GError *error)
{
  EgisCmd *c = ud;
  FpiUsbTransfer *in;

  if (error || c->reply_timeout == 0)
    {
      c->cb (c->d, c->ud, NULL, error ? -1 : 0, error);
      g_free (c);
      return;
    }
  in = fpi_usb_transfer_new (dev);
  fpi_usb_transfer_fill_bulk (in, EP_IN, EGIS_REPLY_LEN);
  fpi_usb_transfer_submit (in, c->reply_timeout, NULL, egis_cmd_in_cb, c);
}

static void
egis_cmd (Egis057aDev *d, gpointer ud, const guint8 *body, gsize n,
          guint reply_timeout, EgisReplyCb cb)
{
  FpiUsbTransfer *out = fpi_usb_transfer_new (d->dev);
  EgisCmd *c = g_new0 (EgisCmd, 1);
  guint8 *buf = g_malloc (4 + n);

  memcpy (buf, "EGIS", 4);
  memcpy (buf + 4, body, n);
  c->d = d;
  c->ud = ud;
  c->reply_timeout = reply_timeout;
  c->cb = cb;
  fpi_usb_transfer_fill_bulk_full (out, EP_OUT, buf, 4 + n, g_free);
  fpi_usb_transfer_submit (out, EGIS_OUT_TIMEOUT, NULL, egis_cmd_out_cb, c);
}

/* For the machines: fail @ssm with the command's @error (if any). */
static gboolean
egis_cmd_failed (FpiSsm *ssm, GError *error)
{
  if (!error)
    return FALSE;
  fpi_ssm_mark_failed (ssm, error);
  return TRUE;
}

/* --------------------------------------------------------------- eh57a ---- */

#define EH57A_KEEPALIVE_INTERVAL_US (450 * G_TIME_SPAN_MILLISECOND)
#define EH57A_APDU_MAX_POLLS 250
#define EH57A_APDU_POLL_MS 15

static guint16
eh57a_crc16 (const guint8 *data, gsize len)
{
  guint16 c = 0;

  for (gsize i = 0; i < len; i++)
    {
      c ^= data[i];
      for (int k = 0; k < 8; k++)
        c = (c & 1) ? ((c >> 1) ^ 0x8408) : (c >> 1);
    }
  return c;
}

static gboolean
eh57a_sp800_108_kdf (const guint8 *key, gsize key_len,
                     const char *label, const guint8 *ctx, gsize ctx_len,
                     guint8 *out, gsize out_len)
{
  gsize pos = 0;
  guint32 counter = 1;
  gsize label_len = strlen (label);
  guint32 bits_be = GUINT32_TO_BE ((guint32) (out_len * 8));

  while (pos < out_len)
    {
      guint8 msg[256];
      gsize mlen = 0;
      guint32 c_be = GUINT32_TO_BE (counter++);
      guint8 md[32];
      size_t md_len = 0;
      gsize copy;

      if (4 + label_len + 1 + ctx_len + 4 > sizeof (msg))
        return FALSE;
      memcpy (msg + mlen, &c_be, 4);
      mlen += 4;
      memcpy (msg + mlen, label, label_len + 1);
      mlen += label_len + 1;
      if (ctx_len > 0)
        {
          memcpy (msg + mlen, ctx, ctx_len);
          mlen += ctx_len;
        }
      memcpy (msg + mlen, &bits_be, 4);
      mlen += 4;

      if (!EVP_Q_mac (NULL, "HMAC", NULL, "SHA256", NULL,
                      key, key_len, msg, mlen, md, sizeof (md), &md_len) ||
          md_len != 32)
        return FALSE;
      copy = MIN (out_len - pos, (gsize) 32);
      memcpy (out + pos, md, copy);
      pos += copy;
    }
  return TRUE;
}

gboolean
egis057a_compute_enroll_id (Egis057aDev      *d,
                              const guint8  nonce[EGIS057A_TEMPLATE_ID_LEN],
                              guint8        out_id[EGIS057A_TEMPLATE_ID_LEN])
{
  guint8 msg[7 + EGIS057A_TEMPLATE_ID_LEN];
  size_t hlen = 0;

  if (!d->has_app_key)
    return FALSE;
  memcpy (msg, "enroll", 7); /* includes NUL terminator */
  memcpy (msg + 7, nonce, EGIS057A_TEMPLATE_ID_LEN);
  return EVP_Q_mac (NULL, "HMAC", NULL, "SHA256", NULL,
                    d->app_key0, sizeof (d->app_key0),
                    msg, sizeof (msg),
                    out_id, EGIS057A_TEMPLATE_ID_LEN, &hlen) != NULL &&
         hlen == EGIS057A_TEMPLATE_ID_LEN;
}

gboolean
egis057a_has_session (Egis057aDev *d)
{
  return d && d->has_app_key;
}

static void eh57a_send_a0 (Egis057aDev *d);
static void eh57a_poll_a1 (Egis057aDev *d);

static void
eh57a_cmd55_finish (Egis057aDev *d, guint8 rsub, guint8 rstatus,
                    const guint8 *rpl, gsize rplen, GError *error)
{
  Egis057aCmdCb cb = d->cmd55_cb;
  gpointer ud = d->cmd55_ud;

  d->cmd55_cb = NULL;
  d->cmd55_ud = NULL;
  cb (d->dev, ud, rsub, rstatus, rpl, rplen, error);
}

static void
eh57a_apdu_timer_clear (Egis057aDev *d)
{
  if (d->apdu_timer)
    {
      g_source_destroy (d->apdu_timer);
      d->apdu_timer = NULL;
    }
}

static void
eh57a_retry_a0_timeout_cb (FpDevice *dev, gpointer user_data)
{
  Egis057aDev *d = user_data;

  d->apdu_timer = NULL;
  eh57a_send_a0 (d);
}

static void
eh57a_poll_a1_timeout_cb (FpDevice *dev, gpointer user_data)
{
  Egis057aDev *d = user_data;

  d->apdu_timer = NULL;
  eh57a_poll_a1 (d);
}

static void
eh57a_a1_data_cb (FpiUsbTransfer *t, FpDevice *dev, gpointer ud, GError *error)
{
  Egis057aDev *d = ud;
  const guint8 *buf = t->buffer;
  gsize olen;

  if (error)
    {
      eh57a_cmd55_finish (d, 0, 0, NULL, 0, error);
      return;
    }
  if (t->actual_length < 2)
    {
      eh57a_cmd55_finish (d, 0, 0, NULL, 0,
                          fpi_device_error_new_msg (FP_DEVICE_ERROR_PROTO,
                                                    "EH57A short APDU response (%d)",
                                                    (int) t->actual_length));
      return;
    }
  if (buf[t->actual_length - 2] == 0x6f && buf[t->actual_length - 1] == 0x00 &&
      ++d->sw6f_retries < 10)
    {
      d->apdu_timer = fpi_device_add_timeout (dev, 50, eh57a_retry_a0_timeout_cb, d, NULL);
      return;
    }
  if (buf[t->actual_length - 2] != 0x90 || buf[t->actual_length - 1] != 0x00)
    {
      eh57a_cmd55_finish (d, 0, 0, NULL, 0,
                          fpi_device_error_new_msg (FP_DEVICE_ERROR_PROTO,
                                                    "EH57A APDU SW %02x%02x",
                                                    buf[t->actual_length - 2],
                                                    buf[t->actual_length - 1]));
      return;
    }
  olen = (gsize) t->actual_length - 2;
  if (d->apdu_use_crc)
    {
      guint16 exp_crc;

      if (olen < 2)
        {
          eh57a_cmd55_finish (d, 0, 0, NULL, 0,
                              fpi_device_error_new_msg (FP_DEVICE_ERROR_PROTO,
                                                        "EH57A APDU missing CRC"));
          return;
        }
      exp_crc = ((guint16) buf[olen - 2] << 8) | buf[olen - 1];
      olen -= 2;
      if (eh57a_crc16 (buf, olen) != exp_crc)
        {
          eh57a_cmd55_finish (d, 0, 0, NULL, 0,
                              fpi_device_error_new_msg (FP_DEVICE_ERROR_PROTO,
                                                        "EH57A APDU CRC mismatch"));
          return;
        }
      if (olen < 8 || buf[0] != 0x55 || buf[1] != 0xaa || buf[2] != 0x00 ||
          buf[3] != (d->cmd_byte | 0x80) ||
          (gsize) 8 + (((gsize) buf[6] << 8) | buf[7]) != olen)
        {
          eh57a_cmd55_finish (d, 0, 0, NULL, 0,
                              fpi_device_error_new_msg (FP_DEVICE_ERROR_PROTO,
                                                        "EH57A malformed 55AA frame"));
          return;
        }
      eh57a_cmd55_finish (d, buf[4], buf[5], buf + 8, olen - 8, NULL);
    }
  else
    {
      eh57a_cmd55_finish (d, 0, 0, buf, olen, NULL);
    }
}

static void
eh57a_a1_hdr_cb (Egis057aDev *d, gpointer ud, const guint8 *r, gssize n, GError *error)
{
  if (error)
    {
      eh57a_cmd55_finish (d, 0, 0, NULL, 0, error);
      return;
    }
  if (n < 7 || memcmp (r, "SIGE", 4) != 0)
    {
      eh57a_cmd55_finish (d, 0, 0, NULL, 0,
                          fpi_device_error_new_msg (FP_DEVICE_ERROR_PROTO,
                                                    "EH57A bad SIGE a1 reply"));
      return;
    }
  if (r[6] == 0x02)
    {
      if (++d->a1_polls < EH57A_APDU_MAX_POLLS)
        {
          d->apdu_timer = fpi_device_add_timeout (d->dev, EH57A_APDU_POLL_MS,
                                                  eh57a_poll_a1_timeout_cb, d, NULL);
          return;
        }
      eh57a_cmd55_finish (d, 0, 0, NULL, 0,
                          fpi_device_error_new_msg (FP_DEVICE_ERROR_PROTO,
                                                    "EH57A APDU poll timed out"));
      return;
    }
  if (r[6] != 0x01)
    {
      eh57a_cmd55_finish (d, 0, 0, NULL, 0,
                          fpi_device_error_new_msg (FP_DEVICE_ERROR_PROTO,
                                                    "EH57A SIGE a1 status 0x%02x", r[6]));
      return;
    }
  {
    gsize rlen = ((gsize) r[4] << 8) | r[5];
    FpiUsbTransfer *in = fpi_usb_transfer_new (d->dev);

    fpi_usb_transfer_fill_bulk (in, EP_IN, MAX (rlen, (gsize) 64));
    fpi_usb_transfer_submit (in, 2000, NULL, eh57a_a1_data_cb, d);
  }
}

static void
eh57a_a1_keepalive_cb (Egis057aDev *d, gpointer ud, const guint8 *r, gssize n, GError *error)
{
  static const guint8 a1_cmd[3] = { 0xa1, 0x00, 0x00 };

  if (error)
    {
      eh57a_cmd55_finish (d, 0, 0, NULL, 0, error);
      return;
    }
  d->next_keepalive_us = g_get_monotonic_time () + EH57A_KEEPALIVE_INTERVAL_US;
  egis_cmd (d, NULL, a1_cmd, sizeof (a1_cmd), EGIS_READ_TIMEOUT, eh57a_a1_hdr_cb);
}

static void
eh57a_poll_a1 (Egis057aDev *d)
{
  static const guint8 a1_cmd[3] = { 0xa1, 0x00, 0x00 };
  static const guint8 ka_on[3] = { 0xa3, 0x00, 0x01 };

  if (d->keepalive_active && g_get_monotonic_time () >= d->next_keepalive_us)
    {
      egis_cmd (d, NULL, ka_on, sizeof (ka_on), EGIS_READ_TIMEOUT, eh57a_a1_keepalive_cb);
      return;
    }
  egis_cmd (d, NULL, a1_cmd, sizeof (a1_cmd), EGIS_READ_TIMEOUT, eh57a_a1_hdr_cb);
}

static void
eh57a_a0_cb (Egis057aDev *d, gpointer ud, const guint8 *r, gssize n, GError *error)
{
  if (error)
    {
      eh57a_cmd55_finish (d, 0, 0, NULL, 0, error);
      return;
    }
  if (n < 7 || memcmp (r, "SIGE", 4) != 0)
    {
      eh57a_cmd55_finish (d, 0, 0, NULL, 0,
                          fpi_device_error_new_msg (FP_DEVICE_ERROR_PROTO,
                                                    "EH57A bad SIGE a0 reply"));
      return;
    }
  if (r[6] == 0x02 && ++d->a0_retries < 20)
    {
      d->apdu_timer = fpi_device_add_timeout (d->dev, 10, eh57a_retry_a0_timeout_cb, d, NULL);
      return;
    }
  if (r[6] != 0x01)
    {
      eh57a_cmd55_finish (d, 0, 0, NULL, 0,
                          fpi_device_error_new_msg (FP_DEVICE_ERROR_PROTO,
                                                    "EH57A SIGE a0 status 0x%02x", r[6]));
      return;
    }
  d->a1_polls = 0;
  eh57a_poll_a1 (d);
}

static void
eh57a_a0_keepalive_cb (Egis057aDev *d, gpointer ud, const guint8 *r, gssize n, GError *error)
{
  if (error)
    {
      eh57a_cmd55_finish (d, 0, 0, NULL, 0, error);
      return;
    }
  if (d->keepalive_active)
    d->next_keepalive_us = g_get_monotonic_time () + EH57A_KEEPALIVE_INTERVAL_US;
  else
    d->next_keepalive_us = G_MAXINT64;
  egis_cmd (d, NULL, d->apdu_tx, d->apdu_tx_len, EGIS_READ_TIMEOUT, eh57a_a0_cb);
}

static void
eh57a_send_a0 (Egis057aDev *d)
{
  static const guint8 ka_on[3] = { 0xa3, 0x00, 0x01 };

  if (d->keepalive_active && g_get_monotonic_time () >= d->next_keepalive_us)
    {
      egis_cmd (d, NULL, ka_on, sizeof (ka_on), EGIS_READ_TIMEOUT, eh57a_a0_keepalive_cb);
      return;
    }
  egis_cmd (d, NULL, d->apdu_tx, d->apdu_tx_len, EGIS_READ_TIMEOUT, eh57a_a0_cb);
}

static void
eh57a_apdu_start (Egis057aDev *d, const guint8 ins[4], gboolean use_crc,
                  const guint8 *pl, gsize plen, gboolean keepalive,
                  Egis057aCmdCb cb, gpointer user_data)
{
  gsize inner_len = plen + (use_crc ? 2 : 0);
  gsize tot = 7 + inner_len + 2;
  guint8 *p;
  gboolean turn_off_ka = (!keepalive && d->keepalive_active);

  g_return_if_fail (d->cmd55_cb == NULL);
  g_return_if_fail (3 + tot <= sizeof (d->apdu_tx));

  d->cmd55_cb = cb;
  d->cmd55_ud = user_data;
  d->apdu_use_crc = use_crc;
  d->a0_retries = 0;
  d->a1_polls = 0;
  d->sw6f_retries = 0;
  if (keepalive && !d->keepalive_active)
    d->next_keepalive_us = 0;
  d->keepalive_active = keepalive;

  d->apdu_tx[0] = 0xa0;
  d->apdu_tx[1] = (guint8) (tot >> 8);
  d->apdu_tx[2] = (guint8) (tot & 0xff);
  memcpy (d->apdu_tx + 3, ins, 4);
  d->apdu_tx[7] = 0x00;
  d->apdu_tx[8] = (guint8) (inner_len >> 8);
  d->apdu_tx[9] = (guint8) (inner_len & 0xff);
  p = d->apdu_tx + 10;
  if (plen > 0)
    memcpy (p, pl, plen);
  if (use_crc)
    {
      guint16 c = eh57a_crc16 (pl, plen);
      p[plen] = (guint8) (c >> 8);
      p[plen + 1] = (guint8) (c & 0xff);
    }
  p[inner_len] = 0x00;
  p[inner_len + 1] = 0x00;
  d->apdu_tx_len = 3 + tot;

  if (turn_off_ka)
    {
      static const guint8 ka_off[3] = { 0xa3, 0x00, 0x00 };
      egis_cmd (d, NULL, ka_off, sizeof (ka_off), EGIS_READ_TIMEOUT, eh57a_a0_keepalive_cb);
      return;
    }
  eh57a_send_a0 (d);
}

void
egis057a_cmd55aa (Egis057aDev       *d,
                    guint8         cmd,
                    guint8         sub,
                    const guint8  *pl,
                    gsize          plen,
                    gboolean       keepalive,
                    Egis057aCmdCb cb,
                    gpointer       user_data)
{
  static const guint8 ins_55aa[4] = { 0x00, 0x50, 0x00, 0x00 };
  guint8 fr[512];

  g_return_if_fail (8 + plen <= sizeof (fr));
  fr[0] = 0x55;
  fr[1] = 0xaa;
  fr[2] = 0x00;
  fr[3] = cmd;
  fr[4] = sub;
  fr[5] = 0x00;
  fr[6] = (guint8) (plen >> 8);
  fr[7] = (guint8) (plen & 0xff);
  if (plen > 0)
    memcpy (fr + 8, pl, plen);
  d->cmd_byte = cmd;
  eh57a_apdu_start (d, ins_55aa, TRUE, fr, 8 + plen, keepalive, cb, user_data);
}

static void
eh57a_led_off_cb (Egis057aDev *d, gpointer ud, const guint8 *r, gssize n, GError *error)
{
  Egis057aDoneCb cb = d->led_cb;
  gpointer user_data = d->led_ud;

  d->led_cb = NULL;
  d->led_ud = NULL;
  d->keepalive_active = FALSE;
  d->next_keepalive_us = G_MAXINT64;
  if (cb)
    cb (d->dev, user_data, error);
  else
    g_clear_error (&error);
}

void
egis057a_led_off (Egis057aDev *d, Egis057aDoneCb cb, gpointer user_data)
{
  static const guint8 ka_off[3] = { 0xa3, 0x00, 0x00 };

  d->led_cb = cb;
  d->led_ud = user_data;
  d->keepalive_active = FALSE;
  egis_cmd (d, NULL, ka_off, sizeof (ka_off), EGIS_READ_TIMEOUT, eh57a_led_off_cb);
}

/* EH57A initialization & ECDH session handshake SSM */
enum {
  EH57A_INIT_DRAIN,
  EH57A_INIT_FWVER,
  EH57A_INIT_CALIBRATE,
  EH57A_INIT_HANDSHAKE,
  EH57A_INIT_LED_OFF,
  EH57A_INIT_NUM_STATES,
};

static void
eh57a_init_drain_cb (FpiUsbTransfer *t, FpDevice *dev, gpointer ud, GError *error)
{
  Egis057aDev *d = ud;
  gboolean more = error == NULL && t->actual_length > 0;

  g_clear_error (&error);
  if (more && ++d->drains < EGIS_DRAIN_MAX)
    fpi_ssm_jump_to_state (t->ssm, EH57A_INIT_DRAIN);
  else
    fpi_ssm_next_state (t->ssm);
}

static void
eh57a_init_fwver_cb (FpDevice *dev, gpointer ud, guint8 rsub, guint8 rstatus,
                     const guint8 *rpl, gsize rplen, GError *error)
{
  FpiSsm *ssm = ud;

  if (egis_cmd_failed (ssm, error))
    return;
  if (rstatus != 0x00)
    {
      fpi_ssm_mark_failed (ssm,
                           fpi_device_error_new_msg (FP_DEVICE_ERROR_PROTO,
                                                     "EH57A 7f 00 status 0x%02x", rstatus));
      return;
    }
  fp_dbg ("EH57A firmware version: %.*s", (int) rplen, (const char *) rpl);
  fpi_ssm_next_state (ssm);
}

static void
eh57a_init_cal_cb (FpDevice *dev, gpointer ud, guint8 rsub, guint8 rstatus,
                   const guint8 *rpl, gsize rplen, GError *error)
{
  FpiSsm *ssm = ud;

  if (egis_cmd_failed (ssm, error))
    return;
  if (rstatus != 0x00)
    fp_dbg ("EH57A 07 00 calibration returned status 0x%02x (continuing)", rstatus);
  fpi_ssm_next_state (ssm);
}

static void
eh57a_init_hs_cb (FpDevice *dev, gpointer ud, guint8 rsub, guint8 rstatus,
                  const guint8 *resp, gsize rlen, GError *error)
{
  FpiSsm *ssm = ud;
  Egis057aDev *d = fpi_ssm_get_data (ssm);
  gsize L, N, blk_len;
  const guint8 *srand_p, *blk, *tail, *srv_pub;
  OSSL_PARAM_BLD *tmpl = NULL;
  OSSL_PARAM *params = NULL;
  EVP_PKEY_CTX *fctx = NULL;
  EVP_PKEY *peer_key = NULL;
  EVP_PKEY_CTX *dctx = NULL;
  guint8 secret[32], ctx[64], master[32], app_keys[64], blk_hash[32], vmsg[40], exp_hmac[32];
  size_t sec_len = sizeof (secret), hlen = 0;
  gboolean ok = FALSE;

  if (egis_cmd_failed (ssm, error))
    return;

  if (rlen < 400)
    goto out;
  L = resp[0];
  if (3 + L > rlen)
    goto out;
  srand_p = resp + 1;
  N = ((gsize) resp[1 + L] << 8) | resp[2 + L];
  blk = resp + 3 + L;
  blk_len = N + 0x122;
  if (3 + L + blk_len + 32 > rlen || L != 32)
    goto out;
  tail = blk + blk_len;
  srv_pub = blk + N + 0x41;

  tmpl = OSSL_PARAM_BLD_new ();
  if (!tmpl ||
      !OSSL_PARAM_BLD_push_utf8_string (tmpl, OSSL_PKEY_PARAM_GROUP_NAME, "prime256v1", 0) ||
      !OSSL_PARAM_BLD_push_octet_string (tmpl, OSSL_PKEY_PARAM_PUB_KEY, srv_pub, 65))
    goto out;
  params = OSSL_PARAM_BLD_to_param (tmpl);
  fctx = EVP_PKEY_CTX_new_from_name (NULL, "EC", NULL);
  if (!params || !fctx ||
      EVP_PKEY_fromdata_init (fctx) <= 0 ||
      EVP_PKEY_fromdata (fctx, &peer_key, EVP_PKEY_PUBLIC_KEY, params) <= 0)
    goto out;

  dctx = EVP_PKEY_CTX_new (d->ecdh_pkey, NULL);
  if (!dctx ||
      EVP_PKEY_derive_init (dctx) <= 0 ||
      EVP_PKEY_derive_set_peer (dctx, peer_key) <= 0 ||
      EVP_PKEY_derive (dctx, secret, &sec_len) <= 0 ||
      sec_len != 32)
    goto out;

  memcpy (ctx, d->ecdh_crand, 32);
  memcpy (ctx + 32, srand_p, 32);
  if (!eh57a_sp800_108_kdf (secret, 32, "master secret", ctx, 64, master, 32) ||
      !eh57a_sp800_108_kdf (master, 32, "application keys", NULL, 0, app_keys, 64))
    goto out;

  if (!EVP_Q_digest (NULL, "SHA256", NULL, blk, blk_len, blk_hash, &hlen) || hlen != 32)
    goto out;
  memcpy (vmsg, "connect", 8); /* includes NUL terminator */
  memcpy (vmsg + 8, blk_hash, 32);
  if (!EVP_Q_mac (NULL, "HMAC", NULL, "SHA256", NULL,
                  app_keys, 32, vmsg, sizeof (vmsg), exp_hmac, sizeof (exp_hmac), &hlen) ||
      hlen != 32 ||
      memcmp (exp_hmac, tail, 32) != 0)
    goto out;

  memcpy (d->app_key0, app_keys, 32);
  d->has_app_key = TRUE;
  ok = TRUE;
  fp_dbg ("EH57A ECDH P-256 session handshake verified");

out:
  if (dctx)
    EVP_PKEY_CTX_free (dctx);
  if (peer_key)
    EVP_PKEY_free (peer_key);
  if (fctx)
    EVP_PKEY_CTX_free (fctx);
  if (params)
    OSSL_PARAM_free (params);
  if (tmpl)
    OSSL_PARAM_BLD_free (tmpl);
  g_clear_pointer (&d->ecdh_pkey, EVP_PKEY_free);

  if (!ok)
    {
      fpi_ssm_mark_failed (ssm,
                           fpi_device_error_new_msg (FP_DEVICE_ERROR_PROTO,
                                                     "EH57A ECDH handshake verification failed"));
      return;
    }
  fpi_ssm_next_state (ssm);
}

static void
eh57a_init_led_cb (FpDevice *dev, gpointer ud, GError *error)
{
  FpiSsm *ssm = ud;

  if (egis_cmd_failed (ssm, error))
    return;
  fpi_ssm_mark_completed (ssm);
}

static void
eh57a_init_run_state (FpiSsm *ssm, FpDevice *dev)
{
  Egis057aDev *d = fpi_ssm_get_data (ssm);

  switch (fpi_ssm_get_cur_state (ssm))
    {
    case EH57A_INIT_DRAIN:
      {
        FpiUsbTransfer *in = fpi_usb_transfer_new (dev);

        in->ssm = ssm;
        fpi_usb_transfer_fill_bulk (in, EP_IN, EGIS_CHUNK);
        fpi_usb_transfer_submit (in, EGIS_DRAIN_TIMEOUT, NULL, eh57a_init_drain_cb, d);
        break;
      }

    case EH57A_INIT_FWVER:
      if (egis_check_cancelled (ssm, d))
        break;
      egis057a_cmd55aa (d, 0x7f, 0x00, NULL, 0, FALSE, eh57a_init_fwver_cb, ssm);
      break;

    case EH57A_INIT_CALIBRATE:
      if (egis_check_cancelled (ssm, d))
        break;
      egis057a_cmd55aa (d, 0x07, 0x00, NULL, 0, FALSE, eh57a_init_cal_cb, ssm);
      break;

    case EH57A_INIT_HANDSHAKE:
      {
        static const guint8 ins_hs[4] = { 0x00, 0x57, 0x01, 0x00 };
        EVP_PKEY_CTX *pctx = NULL;
        guint8 pub[65];
        size_t pub_len = sizeof (pub);
        guint8 pl[98];

        if (egis_check_cancelled (ssm, d))
          break;
        g_clear_pointer (&d->ecdh_pkey, EVP_PKEY_free);
        pctx = EVP_PKEY_CTX_new_id (EVP_PKEY_EC, NULL);
        if (!pctx ||
            EVP_PKEY_keygen_init (pctx) <= 0 ||
            EVP_PKEY_CTX_set_ec_paramgen_curve_nid (pctx, NID_X9_62_prime256v1) <= 0 ||
            EVP_PKEY_keygen (pctx, &d->ecdh_pkey) <= 0)
          {
            if (pctx)
              EVP_PKEY_CTX_free (pctx);
            fpi_ssm_mark_failed (ssm,
                                 fpi_device_error_new_msg (FP_DEVICE_ERROR_GENERAL,
                                                           "EH57A keygen failed"));
            break;
          }
        EVP_PKEY_CTX_free (pctx);
        if (!EVP_PKEY_get_octet_string_param (d->ecdh_pkey, OSSL_PKEY_PARAM_PUB_KEY,
                                              pub, sizeof (pub), &pub_len) ||
            pub_len != 65 ||
            RAND_bytes (d->ecdh_crand, sizeof (d->ecdh_crand)) != 1)
          {
            g_clear_pointer (&d->ecdh_pkey, EVP_PKEY_free);
            fpi_ssm_mark_failed (ssm,
                                 fpi_device_error_new_msg (FP_DEVICE_ERROR_GENERAL,
                                                           "EH57A pubkey export failed"));
            break;
          }
        pl[0] = 32;
        memcpy (pl + 1, d->ecdh_crand, 32);
        memcpy (pl + 33, pub, 65);
        eh57a_apdu_start (d, ins_hs, FALSE, pl, sizeof (pl), FALSE, eh57a_init_hs_cb, ssm);
        break;
      }

    case EH57A_INIT_LED_OFF:
      egis057a_led_off (d, eh57a_init_led_cb, ssm);
      break;

    default:
      g_assert_not_reached ();
    }
}

FpiSsm *
egis057a_init_ssm (Egis057aDev *d)
{
  FpiSsm *ssm = fpi_ssm_new_full (d->dev, eh57a_init_run_state,
                                  EH57A_INIT_NUM_STATES, EH57A_INIT_NUM_STATES,
                                  "eh57a-init");

  d->stop = FALSE;
  d->drains = 0;
  d->has_app_key = FALSE;
  d->keepalive_active = FALSE;
  d->next_keepalive_us = G_MAXINT64;
  fpi_ssm_set_data (ssm, d, NULL);
  return ssm;
}

/* -------------------------------------------------------------- object ---- */

Egis057aDev *
egis057a_dev_new (FpDevice *dev)
{
  Egis057aDev *d = g_new0 (Egis057aDev, 1);

  d->dev = dev;
  d->next_keepalive_us = G_MAXINT64;
  return d;
}

void
egis057a_dev_free (Egis057aDev *d)
{
  if (!d)
    return;
  eh57a_apdu_timer_clear (d);
  g_clear_pointer (&d->ecdh_pkey, EVP_PKEY_free);
  g_free (d);
}

void
egis057a_dev_request_stop (Egis057aDev *d)
{
  d->stop = TRUE;
}

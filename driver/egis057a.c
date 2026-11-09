/*
 * LighTuning Technology Inc. EgisTec EH57A (1c7a:057a) driver for libfprint
 *
 * Copyright (C) 2026 Philipp Oster (driver structure: suspend/resume parking,
 *                                   cancellation rules, USB transport; derived
 *                                   from the egis0576 driver this is based on)
 * Copyright (C) 2026 Milepower      (EH57A match-on-chip protocol and driver)
 *
 * The EH57A is a match-on-chip sensor: fingerprints are enrolled, stored and
 * matched inside the sensor, and the host only drives it with commands. The
 * protocol (EGIS/SIGE transport, 55AA command framing, the ECDH P-256 session
 * handshake) lives in egis057a/egis057a_proto.c as FpiSsm machines over
 * FpiUsbTransfer. This file maps libfprint's enroll / verify / identify
 * actions onto the chip's commands.
 *
 * What libfprint stores in a print is only the 32-byte template id under which
 * the chip keeps the enrolment; verify and identify hand the candidate ids to
 * the chip, which answers with the one that matched.
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

#include "egis057a.h"
#include "egis057a/egis057a_proto.h"
#include "drivers_api.h"

/* libfprint wants a fixed stage count; the chip reports "progress/total" per
 * press, which is scaled onto this many stages. */
#define EGIS057A_ENROLL_STAGES 5

/* Prints are stored as a versioned record so a future format change can be
 * told apart from this one. */
#define EGIS057A_PRINT_VERSION 1

struct _FpDeviceEgis057a
{
  FpDevice      parent;

  Egis057aDev  *sensor;           /* the channel to the sensor, open..close */

  gboolean      needs_reinit;     /* set by suspend/resume: the session is
                                   * re-established before the next command */
  /* Suspend with an action running (see egis057a_suspend): the machine parks
   * at its next transfer boundary -- nothing in flight, nothing scheduled --
   * and only then is the suspend completed; resume (or a cancel) restarts it
   * at the head. */
  gboolean      suspending;       /* the suspend vfunc ran; park at the next boundary */
  gboolean      parked;           /* parked: waiting for resume or cancel */

  /* The running action. */
  FpiSsm         *ssm;
  FpiDeviceAction action;
  gboolean        finger_present; /* as last reported */
  gboolean        frame_retried;  /* a stale session was already answered with a re-init */
  guint           enroll_count;
  int             settle_tries;   /* retries on the current press */
  gboolean        saw_finger;     /* verify/identify: a press is in progress */
  gboolean        cand_valid;     /* verify/identify: the chip reported a match */
  int             cand_idx;       /* ... for this entry of ids[] */

  /* verify/identify: the template ids to match against, in gallery order,
   * and the prints they came from (borrowed refs) for result mapping. */
  guint8          ids[EGIS057A_MAX_TEMPLATES][EGIS057A_TEMPLATE_ID_LEN];
  int             n_ids;
  GPtrArray      *gallery_prints;

  /* chip action state */
  gboolean        enrolling;      /* a 16 01 enrolment is open on the chip */
  gboolean        use_1a;         /* poll with 1a 00 instead of 17 02 */
  gboolean        retry;          /* report a retry instead of a result */
  GError         *error;
  guint8          nonce[EGIS057A_TEMPLATE_ID_LEN];
  guint8          template_id[EGIS057A_TEMPLATE_ID_LEN];
  guint8          del_id[EGIS057A_TEMPLATE_ID_LEN];
};
G_DECLARE_FINAL_TYPE (FpDeviceEgis057a, fpi_device_egis057a, FPI, DEVICE_EGIS057A, FpDevice);
G_DEFINE_TYPE (FpDeviceEgis057a, fpi_device_egis057a, FP_TYPE_DEVICE);

/* ------------------------------------------------------------------ */
/* Print storage                                                      */
/* ------------------------------------------------------------------ */

static GVariant *
print_wrap_id (const guint8 *id, gsize len)
{
  GVariant *data = g_variant_new_fixed_array (G_VARIANT_TYPE_BYTE, id, len, 1);

  return g_variant_new ("(q@ay)", (guint16) EGIS057A_PRINT_VERSION, data);
}

/* The template id stored in @print into @out; FALSE if it is not one of ours. */
static gboolean
print_get_id (FpPrint *print, guint8 out[EGIS057A_TEMPLATE_ID_LEN])
{
  g_autoptr(GVariant) var = NULL;
  g_autoptr(GVariant) data = NULL;
  guint16 version = 0;
  gsize len = 0;
  const guint8 *id;

  g_object_get (print, "fpi-data", &var, NULL);
  if (!var || !g_variant_is_of_type (var, G_VARIANT_TYPE ("(qay)")))
    return FALSE;
  g_variant_get (var, "(q@ay)", &version, &data);
  if (version != EGIS057A_PRINT_VERSION)
    return FALSE;
  id = g_variant_get_fixed_array (data, &len, 1);
  if (!id || len != EGIS057A_TEMPLATE_ID_LEN)
    return FALSE;
  memcpy (out, id, EGIS057A_TEMPLATE_ID_LEN);
  return TRUE;
}

/* Collect the template ids of @prints into self->ids (at most the chip's
 * capacity); the prints that decoded are listed, in the same order, in
 * self->gallery_prints. Returns the number of valid ids. */
static int
load_ids (FpDeviceEgis057a *self, GPtrArray *prints)
{
  g_clear_pointer (&self->gallery_prints, g_ptr_array_unref);
  self->gallery_prints = g_ptr_array_new ();     /* borrowed refs */
  self->n_ids = 0;
  for (guint i = 0; i < prints->len && self->n_ids < EGIS057A_MAX_TEMPLATES; i++)
    {
      FpPrint *print = g_ptr_array_index (prints, i);

      if (print_get_id (print, self->ids[self->n_ids]))
        {
          g_ptr_array_add (self->gallery_prints, print);
          self->n_ids++;
        }
    }
  return self->n_ids;
}

/* ------------------------------------------------------------------ */
/* Action machinery shared by the states                              */
/* ------------------------------------------------------------------ */

/* Fail @ssm with G_IO_ERROR_CANCELLED iff the action was cancelled. */
static gboolean
capture_cancelled (FpiSsm *ssm, FpDevice *dev)
{
  if (!fpi_device_action_is_cancelled (dev))
    return FALSE;
  fpi_ssm_mark_failed (ssm, g_error_new_literal (G_IO_ERROR, G_IO_ERROR_CANCELLED,
                                                 "cancelled"));
  return TRUE;
}

/* Fail the action with @error (transfer full) -- as G_IO_ERROR_CANCELLED if the
 * action was cancelled meanwhile, whatever the transport reported: a cancelled
 * action must never be mistaken for a sensor failure. */
static void
capture_fail (FpDeviceEgis057a *self, GError *error)
{
  if (fpi_device_action_is_cancelled (FP_DEVICE (self)) &&
      !g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
    {
      g_error_free (error);
      error = g_error_new_literal (G_IO_ERROR, G_IO_ERROR_CANCELLED, "cancelled");
    }
  fpi_ssm_mark_failed (self->ssm, error);
}

static void report_finger (FpDeviceEgis057a *self,
                           gboolean          present);

/* Forget the press in progress. */
static void
capture_press_reset (FpDeviceEgis057a *self)
{
  self->saw_finger = FALSE;
  self->cand_valid = FALSE;
  self->settle_tries = 0;
  report_finger (self, FALSE);
}

/* At a transfer boundary with a suspend pending: park the machine (it stays
 * in its state, nothing in flight, nothing scheduled) and complete the
 * suspend now that the bus is quiet. TRUE iff parked. */
static gboolean
capture_park_if_suspending (FpDeviceEgis057a *self, FpiSsm *ssm)
{
  if (!self->suspending)
    return FALSE;
  self->suspending = FALSE;
  self->parked = TRUE;
  /* Whatever press was in progress is over by the time we resume. */
  capture_press_reset (self);
  fp_dbg ("parked for suspend in state %d", fpi_ssm_get_cur_state (ssm));
  fpi_device_suspend_complete (FP_DEVICE (self), NULL);
  return TRUE;
}

static void
report_finger (FpDeviceEgis057a *self, gboolean present)
{
  if (present == self->finger_present)
    return;
  self->finger_present = present;
  if (present)
    fpi_device_report_finger_status_changes (FP_DEVICE (self),
                                             FP_FINGER_STATUS_PRESENT,
                                             FP_FINGER_STATUS_NONE);
  else
    fpi_device_report_finger_status_changes (FP_DEVICE (self),
                                             FP_FINGER_STATUS_NONE,
                                             FP_FINGER_STATUS_PRESENT);
}

/* ------------------------------------------------------------------ */
/* Chip action machine: enrol, verify, identify                       */
/* ------------------------------------------------------------------ */

enum {
  EH57A_PREPARE = 0,
  EH57A_REINIT,
  EH57A_ENR_LIST,
  EH57A_ENR_GC,
  EH57A_ENR_START,
  EH57A_ENR_STEP,
  EH57A_ENR_LIFT,
  EH57A_ENR_CHECK_DUP,
  EH57A_ENR_DEL_DUP,
  EH57A_ENR_COMMIT,
  EH57A_MATCH_POLL,
  EH57A_MATCH_EXEC,
  EH57A_MATCH_COMMIT,
  EH57A_CLEANUP,
  EH57A_NUM_STATES,
};

static void
eh57a_fail_via_cleanup (FpDeviceEgis057a *self, FpiSsm *ssm, GError *error)
{
  if (!self->error)
    self->error = error;
  else
    g_clear_error (&error);
  fpi_ssm_jump_to_state (ssm, EH57A_CLEANUP);
}

static void
eh57a_reinit_done (FpiSsm *init, FpDevice *dev, GError *error)
{
  FpDeviceEgis057a *self = FPI_DEVICE_EGIS057A (dev);

  if (error)
    {
      if (self->suspending && !fpi_device_action_is_cancelled (dev) &&
          g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        {
          g_error_free (error);
          capture_park_if_suspending (self, self->ssm);
          return;
        }
      capture_fail (self, error);
      return;
    }
  if (!self->suspending)
    self->needs_reinit = FALSE;
  fpi_ssm_jump_to_state (self->ssm,
                         self->action == FPI_DEVICE_ACTION_ENROLL ?
                         EH57A_ENR_LIST : EH57A_MATCH_POLL);
}

static void
eh57a_enr_list_cb (FpDevice *dev, gpointer ud, guint8 rsub, guint8 rstatus,
                   const guint8 *rpl, gsize rplen, GError *error)
{
  FpDeviceEgis057a *self = FPI_DEVICE_EGIS057A (dev);
  FpiSsm *ssm = ud;

  if (error)
    {
      eh57a_fail_via_cleanup (self, ssm, error);
      return;
    }
  if (rstatus == 0x00 &&
      rplen >= EGIS057A_MAX_TEMPLATES * EGIS057A_TEMPLATE_ID_LEN)
    {
      fp_dbg ("EH57A chip storage full (%u templates); deleting oldest slot",
              (guint) (rplen / EGIS057A_TEMPLATE_ID_LEN));
      memcpy (self->del_id, rpl, EGIS057A_TEMPLATE_ID_LEN);
      fpi_ssm_jump_to_state (ssm, EH57A_ENR_GC);
      return;
    }
  fpi_ssm_jump_to_state (ssm, EH57A_ENR_START);
}

static void
eh57a_enr_gc_cb (FpDevice *dev, gpointer ud, guint8 rsub, guint8 rstatus,
                 const guint8 *rpl, gsize rplen, GError *error)
{
  FpDeviceEgis057a *self = FPI_DEVICE_EGIS057A (dev);
  FpiSsm *ssm = ud;

  if (error)
    {
      eh57a_fail_via_cleanup (self, ssm, error);
      return;
    }
  fpi_ssm_jump_to_state (ssm, EH57A_ENR_START);
}

static void
eh57a_enr_start_cb (FpDevice *dev, gpointer ud, guint8 rsub, guint8 rstatus,
                    const guint8 *rpl, gsize rplen, GError *error)
{
  FpDeviceEgis057a *self = FPI_DEVICE_EGIS057A (dev);
  FpiSsm *ssm = ud;

  if (error)
    {
      eh57a_fail_via_cleanup (self, ssm, error);
      return;
    }
  if (rstatus == 0xe0 && !self->frame_retried)
    {
      fp_dbg ("EH57A 16 01 returned 0xe0; re-running session handshake");
      self->frame_retried = TRUE;
      self->needs_reinit = TRUE;
      fpi_ssm_jump_to_state (ssm, EH57A_REINIT);
      return;
    }
  if (rstatus != 0x00 || rplen < EGIS057A_TEMPLATE_ID_LEN)
    {
      eh57a_fail_via_cleanup (self, ssm,
                              fpi_device_error_new_msg (FP_DEVICE_ERROR_PROTO,
                                                        "EH57A enroll start failed (status 0x%02x)",
                                                        rstatus));
      return;
    }
  memcpy (self->nonce, rpl, EGIS057A_TEMPLATE_ID_LEN);
  self->enrolling = TRUE;
  self->settle_tries = 0;
  fpi_ssm_jump_to_state (ssm, EH57A_ENR_STEP);
}

static void
eh57a_enr_step_cb (FpDevice *dev, gpointer ud, guint8 rsub, guint8 rstatus,
                   const guint8 *rpl, gsize rplen, GError *error)
{
  FpDeviceEgis057a *self = FPI_DEVICE_EGIS057A (dev);
  FpiSsm *ssm = ud;

  if (error)
    {
      eh57a_fail_via_cleanup (self, ssm, error);
      return;
    }
  if (rstatus == 0x97)
    {
      if (self->settle_tries > 0)
        {
          self->settle_tries = 0;
          report_finger (self, FALSE);
          fpi_device_enroll_progress (dev, self->enroll_count, NULL,
                                      fpi_device_retry_new (FP_DEVICE_RETRY_CENTER_FINGER));
        }
      else
        {
          report_finger (self, FALSE);
        }
      fpi_ssm_jump_to_state_delayed (ssm, EH57A_ENR_STEP, 40);
      return;
    }
  if (rstatus == 0x00 && rplen >= 2)
    {
      guint prog = rpl[0];
      guint tot = MAX ((guint) rpl[1], 1U);

      self->settle_tries = 0;
      report_finger (self, TRUE);
      fp_dbg ("EH57A enroll step %u/%u", prog, tot);
      if (prog >= tot)
        {
          self->enroll_count = EGIS057A_ENROLL_STAGES;
          fpi_device_enroll_progress (dev, self->enroll_count, NULL, NULL);
          fpi_ssm_jump_to_state (ssm, EH57A_ENR_CHECK_DUP);
          return;
        }
      else
        {
          guint stage = CLAMP ((prog * EGIS057A_ENROLL_STAGES) / tot,
                               1U, (guint) (EGIS057A_ENROLL_STAGES - 1));
          if (stage > self->enroll_count)
            self->enroll_count = stage;
          else if (self->enroll_count < EGIS057A_ENROLL_STAGES - 1)
            self->enroll_count++;
          fpi_device_enroll_progress (dev, self->enroll_count, NULL, NULL);
          fpi_ssm_jump_to_state (ssm, EH57A_ENR_LIFT);
          return;
        }
    }
  if (rstatus == 0xfa)
    {
      self->settle_tries = 0;
      report_finger (self, TRUE);
      self->enroll_count = EGIS057A_ENROLL_STAGES;
      fpi_device_enroll_progress (dev, self->enroll_count, NULL, NULL);
      fpi_ssm_jump_to_state (ssm, EH57A_ENR_CHECK_DUP);
      return;
    }
  if ((rstatus == 0x93 || rstatus == 0x95) && self->settle_tries < 4)
    {
      self->settle_tries++;
      report_finger (self, TRUE);
      fpi_ssm_jump_to_state_delayed (ssm, EH57A_ENR_STEP, 40);
      return;
    }
  if (rstatus == 0x01 || rstatus == 0x19 || rstatus == 0x93 || rstatus == 0x95)
    {
      self->settle_tries = 0;
      report_finger (self, TRUE);
      fp_dbg ("EH57A enroll step retry (status 0x%02x)", rstatus);
      fpi_device_enroll_progress (dev, self->enroll_count, NULL,
                                  fpi_device_retry_new (FP_DEVICE_RETRY_CENTER_FINGER));
      fpi_ssm_jump_to_state (ssm, EH57A_ENR_LIFT);
      return;
    }
  eh57a_fail_via_cleanup (self, ssm,
                          fpi_device_error_new_msg (FP_DEVICE_ERROR_PROTO,
                                                    "EH57A enroll step status 0x%02x",
                                                    rstatus));
}

static void
eh57a_enr_lift_cb (FpDevice *dev, gpointer ud, guint8 rsub, guint8 rstatus,
                   const guint8 *rpl, gsize rplen, GError *error)
{
  FpDeviceEgis057a *self = FPI_DEVICE_EGIS057A (dev);
  FpiSsm *ssm = ud;

  if (error)
    {
      eh57a_fail_via_cleanup (self, ssm, error);
      return;
    }
  if (rstatus == 0x97)
    {
      report_finger (self, FALSE);
      fpi_ssm_jump_to_state (ssm, EH57A_ENR_STEP);
      return;
    }
  fpi_ssm_jump_to_state_delayed (ssm, EH57A_ENR_LIFT, 40);
}

static void
eh57a_enr_check_dup_cb (FpDevice *dev, gpointer ud, guint8 rsub, guint8 rstatus,
                        const guint8 *rpl, gsize rplen, GError *error)
{
  FpDeviceEgis057a *self = FPI_DEVICE_EGIS057A (dev);
  FpiSsm *ssm = ud;

  if (error)
    {
      eh57a_fail_via_cleanup (self, ssm, error);
      return;
    }
  if (rstatus == 0x00 && rplen >= EGIS057A_TEMPLATE_ID_LEN)
    {
      fp_dbg ("EH57A finger already exists on chip; replacing existing chip slot");
      memcpy (self->del_id, rpl, EGIS057A_TEMPLATE_ID_LEN);
      fpi_ssm_jump_to_state (ssm, EH57A_ENR_DEL_DUP);
      return;
    }
  fpi_ssm_jump_to_state (ssm, EH57A_ENR_COMMIT);
}

static void
eh57a_enr_del_dup_cb (FpDevice *dev, gpointer ud, guint8 rsub, guint8 rstatus,
                      const guint8 *rpl, gsize rplen, GError *error)
{
  FpDeviceEgis057a *self = FPI_DEVICE_EGIS057A (dev);
  FpiSsm *ssm = ud;

  if (error)
    {
      eh57a_fail_via_cleanup (self, ssm, error);
      return;
    }
  fpi_ssm_jump_to_state (ssm, EH57A_ENR_COMMIT);
}

static void
eh57a_enr_commit_cb (FpDevice *dev, gpointer ud, guint8 rsub, guint8 rstatus,
                     const guint8 *rpl, gsize rplen, GError *error)
{
  FpDeviceEgis057a *self = FPI_DEVICE_EGIS057A (dev);
  FpiSsm *ssm = ud;

  self->enrolling = FALSE;
  if (error)
    {
      eh57a_fail_via_cleanup (self, ssm, error);
      return;
    }
  if (rstatus != 0x00)
    {
      eh57a_fail_via_cleanup (self, ssm,
                              fpi_device_error_new_msg (FP_DEVICE_ERROR_PROTO,
                                                        "EH57A enroll commit failed (status 0x%02x)",
                                                        rstatus));
      return;
    }
  fp_dbg ("EH57A enroll committed to chip");
  fpi_ssm_jump_to_state (ssm, EH57A_CLEANUP);
}

static void
eh57a_match_poll_cb (FpDevice *dev, gpointer ud, guint8 rsub, guint8 rstatus,
                     const guint8 *rpl, gsize rplen, GError *error)
{
  FpDeviceEgis057a *self = FPI_DEVICE_EGIS057A (dev);
  FpiSsm *ssm = ud;

  if (error)
    {
      eh57a_fail_via_cleanup (self, ssm, error);
      return;
    }
  if ((rstatus == 0xfe || rstatus == 0xfd) && !self->use_1a)
    {
      self->use_1a = TRUE;
      fpi_ssm_jump_to_state (ssm, EH57A_MATCH_POLL);
      return;
    }
  if (rstatus == 0xe0 && !self->frame_retried)
    {
      fp_dbg ("EH57A match poll returned 0xe0; re-running session handshake");
      self->frame_retried = TRUE;
      self->needs_reinit = TRUE;
      fpi_ssm_jump_to_state (ssm, EH57A_REINIT);
      return;
    }
  if (rstatus == 0x97)
    {
      if (self->saw_finger)
        {
          /* Finger lifted after an unconfirmed / non-matching touch */
          report_finger (self, FALSE);
          fpi_ssm_jump_to_state (ssm, EH57A_CLEANUP);
          return;
        }
      if (self->settle_tries > 0)
        {
          self->settle_tries = 0;
          self->retry = TRUE;
          report_finger (self, FALSE);
          fpi_ssm_jump_to_state (ssm, EH57A_CLEANUP);
          return;
        }
      report_finger (self, FALSE);
      fpi_ssm_jump_to_state_delayed (ssm, EH57A_MATCH_POLL, 40);
      return;
    }
  if (rstatus == 0x00 || rstatus == 0x01)
    {
      self->saw_finger = TRUE;
      report_finger (self, TRUE);
      fpi_ssm_jump_to_state (ssm, EH57A_MATCH_EXEC);
      return;
    }
  if ((rstatus == 0x93 || rstatus == 0x95) && self->settle_tries < 4)
    {
      self->settle_tries++;
      report_finger (self, TRUE);
      fpi_ssm_jump_to_state_delayed (ssm, EH57A_MATCH_POLL, 40);
      return;
    }
  if (rstatus == 0x93 || rstatus == 0x95)
    {
      self->settle_tries = 0;
      self->retry = TRUE;
      report_finger (self, TRUE);
      fpi_ssm_jump_to_state (ssm, EH57A_CLEANUP);
      return;
    }
  eh57a_fail_via_cleanup (self, ssm,
                          fpi_device_error_new_msg (FP_DEVICE_ERROR_PROTO,
                                                    "EH57A match poll status 0x%02x",
                                                    rstatus));
}

static void
eh57a_match_exec_cb (FpDevice *dev, gpointer ud, guint8 rsub, guint8 rstatus,
                     const guint8 *rpl, gsize rplen, GError *error)
{
  FpDeviceEgis057a *self = FPI_DEVICE_EGIS057A (dev);
  FpiSsm *ssm = ud;

  if (error)
    {
      eh57a_fail_via_cleanup (self, ssm, error);
      return;
    }
  if ((rstatus == 0x00 || rstatus == 0x0a) && rplen >= 64)
    {
      int idx = 0;

      for (int i = 0; i < self->n_ids; i++)
        {
          if (memcmp (self->ids[i], rpl, EGIS057A_TEMPLATE_ID_LEN) == 0)
            {
              idx = i;
              break;
            }
        }
      fp_dbg ("EH57A match succeeded (idx %d, status 0x%02x)", idx, rstatus);
      self->cand_valid = TRUE;
      self->cand_idx = idx;
      if (rstatus == 0x0a)
        fpi_ssm_jump_to_state (ssm, EH57A_MATCH_COMMIT);
      else
        fpi_ssm_jump_to_state (ssm, EH57A_CLEANUP);
      return;
    }
  if (rstatus == 0x01 && ++self->settle_tries < 3)
    {
      fp_dbg ("EH57A match attempt %d returned 0x01; sampling again", self->settle_tries);
      fpi_ssm_jump_to_state_delayed (ssm, EH57A_MATCH_POLL, 40);
      return;
    }
  if (rstatus == 0x93 || rstatus == 0x95 || rstatus == 0x97)
    {
      if (++self->settle_tries < 3)
        {
          fpi_ssm_jump_to_state_delayed (ssm, EH57A_MATCH_POLL, 40);
          return;
        }
      self->retry = TRUE;
      fpi_ssm_jump_to_state (ssm, EH57A_CLEANUP);
      return;
    }
  fp_dbg ("EH57A match failed (status 0x%02x)", rstatus);
  self->cand_valid = FALSE;
  self->cand_idx = -1;
  fpi_ssm_jump_to_state (ssm, EH57A_CLEANUP);
}

static void
eh57a_match_commit_cb (FpDevice *dev, gpointer ud, guint8 rsub, guint8 rstatus,
                       const guint8 *rpl, gsize rplen, GError *error)
{
  FpiSsm *ssm = ud;

  g_clear_error (&error);
  fpi_ssm_jump_to_state (ssm, EH57A_CLEANUP);
}

static void
eh57a_abort_enr_cb (FpDevice *dev, gpointer ud, guint8 rsub, guint8 rstatus,
                    const guint8 *rpl, gsize rplen, GError *error)
{
  FpiSsm *ssm = ud;

  g_clear_error (&error);
  fpi_ssm_jump_to_state (ssm, EH57A_CLEANUP);
}

static void
eh57a_cleanup_led_cb (FpDevice *dev, gpointer ud, GError *error)
{
  FpDeviceEgis057a *self = FPI_DEVICE_EGIS057A (dev);
  FpiSsm *ssm = ud;

  g_clear_error (&error);
  report_finger (self, FALSE);
  if (capture_cancelled (ssm, dev) || capture_park_if_suspending (self, ssm))
    {
      g_clear_error (&self->error);
      return;
    }
  if (self->error)
    {
      fpi_ssm_mark_failed (ssm, g_steal_pointer (&self->error));
      return;
    }
  if (self->action == FPI_DEVICE_ACTION_VERIFY)
    {
      if (self->retry)
        fpi_device_verify_report (dev, FPI_MATCH_ERROR, NULL,
                                  fpi_device_retry_new (FP_DEVICE_RETRY_CENTER_FINGER));
      else if (self->cand_valid)
        fpi_device_verify_report (dev, FPI_MATCH_SUCCESS, NULL, NULL);
      else
        fpi_device_verify_report (dev, FPI_MATCH_FAIL, NULL, NULL);
    }
  else if (self->action == FPI_DEVICE_ACTION_IDENTIFY)
    {
      if (self->retry)
        fpi_device_identify_report (dev, NULL, NULL,
                                    fpi_device_retry_new (FP_DEVICE_RETRY_CENTER_FINGER));
      else if (self->cand_valid && self->cand_idx >= 0 &&
               self->cand_idx < (int) self->gallery_prints->len)
        fpi_device_identify_report (dev,
                                    g_ptr_array_index (self->gallery_prints, self->cand_idx),
                                    NULL, NULL);
      else
        fpi_device_identify_report (dev, NULL, NULL, NULL);
    }
  fpi_ssm_mark_completed (ssm);
}

static void
eh57a_run_state (FpiSsm *ssm, FpDevice *dev)
{
  FpDeviceEgis057a *self = FPI_DEVICE_EGIS057A (dev);

  switch (fpi_ssm_get_cur_state (ssm))
    {
    case EH57A_PREPARE:
      if (self->action != FPI_DEVICE_ACTION_ENROLL && self->n_ids <= 0)
        {
          fpi_ssm_mark_failed (ssm,
                               fpi_device_error_new_msg (FP_DEVICE_ERROR_DATA_INVALID,
                                                         "No valid EH57A templates"));
          break;
        }
      fpi_ssm_next_state (ssm);
      break;

    case EH57A_REINIT:
      if (capture_cancelled (ssm, dev) || capture_park_if_suspending (self, ssm))
        break;
      if (self->needs_reinit || !egis057a_has_session (self->sensor))
        {
          self->needs_reinit = TRUE;
          fpi_ssm_start (egis057a_init_ssm (self->sensor), eh57a_reinit_done);
        }
      else
        {
          fpi_ssm_jump_to_state (ssm,
                                 self->action == FPI_DEVICE_ACTION_ENROLL ?
                                 EH57A_ENR_LIST : EH57A_MATCH_POLL);
        }
      break;

    case EH57A_ENR_LIST:
      if (fpi_device_action_is_cancelled (dev) || self->suspending)
        {
          fpi_ssm_jump_to_state (ssm, EH57A_CLEANUP);
          break;
        }
      egis057a_cmd55aa (self->sensor, 0x19, 0x00, NULL, 0, FALSE,
                          eh57a_enr_list_cb, ssm);
      break;

    case EH57A_ENR_GC:
      egis057a_cmd55aa (self->sensor, 0x18, 0x00,
                          self->del_id, EGIS057A_TEMPLATE_ID_LEN, FALSE,
                          eh57a_enr_gc_cb, ssm);
      break;

    case EH57A_ENR_START:
      if (fpi_device_action_is_cancelled (dev) || self->suspending)
        {
          fpi_ssm_jump_to_state (ssm, EH57A_CLEANUP);
          break;
        }
      egis057a_cmd55aa (self->sensor, 0x16, 0x01, NULL, 0, FALSE,
                          eh57a_enr_start_cb, ssm);
      break;

    case EH57A_ENR_STEP:
      if (fpi_device_action_is_cancelled (dev) || self->suspending)
        {
          fpi_ssm_jump_to_state (ssm, EH57A_CLEANUP);
          break;
        }
      egis057a_cmd55aa (self->sensor, 0x16, 0x02, NULL, 0, TRUE,
                          eh57a_enr_step_cb, ssm);
      break;

    case EH57A_ENR_LIFT:
      if (fpi_device_action_is_cancelled (dev) || self->suspending)
        {
          fpi_ssm_jump_to_state (ssm, EH57A_CLEANUP);
          break;
        }
      egis057a_cmd55aa (self->sensor, 0x1a, 0x00, NULL, 0, TRUE,
                          eh57a_enr_lift_cb, ssm);
      break;

    case EH57A_ENR_CHECK_DUP:
      egis057a_cmd55aa (self->sensor, 0x16, 0x05, NULL, 0, TRUE,
                          eh57a_enr_check_dup_cb, ssm);
      break;

    case EH57A_ENR_DEL_DUP:
      egis057a_cmd55aa (self->sensor, 0x18, 0x00,
                          self->del_id, EGIS057A_TEMPLATE_ID_LEN, TRUE,
                          eh57a_enr_del_dup_cb, ssm);
      break;

    case EH57A_ENR_COMMIT:
      if (!egis057a_compute_enroll_id (self->sensor,
                                         self->nonce,
                                         self->template_id))
        {
          eh57a_fail_via_cleanup (self, ssm,
                                  fpi_device_error_new_msg (FP_DEVICE_ERROR_GENERAL,
                                                            "EH57A HMAC enroll ID failed"));
          break;
        }
      egis057a_cmd55aa (self->sensor, 0x16, 0x03,
                          self->template_id, EGIS057A_TEMPLATE_ID_LEN, FALSE,
                          eh57a_enr_commit_cb, ssm);
      break;

    case EH57A_MATCH_POLL:
      if (fpi_device_action_is_cancelled (dev) || self->suspending)
        {
          fpi_ssm_jump_to_state (ssm, EH57A_CLEANUP);
          break;
        }
      egis057a_cmd55aa (self->sensor,
                          self->use_1a ? 0x1a : 0x17,
                          self->use_1a ? 0x00 : 0x02,
                          NULL, 0, TRUE,
                          eh57a_match_poll_cb, ssm);
      break;

    case EH57A_MATCH_EXEC:
      {
        guint8 pl[1 + EGIS057A_MAX_TEMPLATES * EGIS057A_TEMPLATE_ID_LEN];
        int count = MIN (self->n_ids, EGIS057A_MAX_TEMPLATES);

        pl[0] = 0x00;
        for (int i = 0; i < count; i++)
          memcpy (pl + 1 + i * EGIS057A_TEMPLATE_ID_LEN,
                  self->ids[i], EGIS057A_TEMPLATE_ID_LEN);
        egis057a_cmd55aa (self->sensor, 0x17, 0x00,
                            pl, 1 + (gsize) count * EGIS057A_TEMPLATE_ID_LEN, TRUE,
                            eh57a_match_exec_cb, ssm);
        break;
      }

    case EH57A_MATCH_COMMIT:
      egis057a_cmd55aa (self->sensor, 0x17, 0x01, NULL, 0, FALSE,
                          eh57a_match_commit_cb, ssm);
      break;

    case EH57A_CLEANUP:
      if (self->enrolling)
        {
          self->enrolling = FALSE;
          egis057a_cmd55aa (self->sensor, 0x16, 0x04, NULL, 0, FALSE,
                              eh57a_abort_enr_cb, ssm);
          break;
        }
      egis057a_led_off (self->sensor, eh57a_cleanup_led_cb, ssm);
      break;

    default:
      g_assert_not_reached ();
    }
}
/* ------------------------------------------------------------------ */
/* Action start / completion                                          */
/* ------------------------------------------------------------------ */

static void
action_complete (FpDeviceEgis057a *self, FpDevice *dev)
{
  g_clear_pointer (&self->gallery_prints, g_ptr_array_unref);
  if (self->action == FPI_DEVICE_ACTION_ENROLL)
    {
      FpPrint *print = NULL;

      fpi_device_get_enroll_data (dev, &print);
      fpi_print_set_type (print, FPI_PRINT_RAW);
      g_object_set (print, "fpi-data",
                    print_wrap_id (self->template_id, EGIS057A_TEMPLATE_ID_LEN),
                    NULL);
      fpi_device_enroll_complete (dev, g_object_ref (print), NULL);
    }
  else if (self->action == FPI_DEVICE_ACTION_VERIFY)
    {
      fpi_device_verify_complete (dev, NULL);
    }
  else
    {
      fpi_device_identify_complete (dev, NULL);
    }
}

static void
action_done (FpiSsm *ssm, FpDevice *dev, GError *error)
{
  FpDeviceEgis057a *self = FPI_DEVICE_EGIS057A (dev);

  self->ssm = NULL;
  self->parked = FALSE;
  g_clear_error (&self->error);
  if (error)
    {
      g_clear_pointer (&self->gallery_prints, g_ptr_array_unref);
      fpi_device_action_error (dev, error);
    }
  else
    {
      action_complete (self, dev);
    }
  /* The action ended before the machine reached a boundary to park at: the
   * suspend is completed here instead, with nothing of ours left running. */
  if (self->suspending)
    {
      self->suspending = FALSE;
      fpi_device_suspend_complete (dev, NULL);
    }
}

static void
start_action (FpDevice *dev)
{
  FpDeviceEgis057a *self = FPI_DEVICE_EGIS057A (dev);

  self->action = fpi_device_get_current_action (dev);
  self->finger_present = FALSE;
  self->frame_retried = FALSE;
  self->settle_tries = 0;
  self->saw_finger = FALSE;
  self->cand_valid = FALSE;
  self->cand_idx = -1;
  self->parked = FALSE;
  self->enrolling = FALSE;
  self->use_1a = FALSE;
  self->retry = FALSE;
  g_clear_error (&self->error);

  fpi_device_report_finger_status_changes (dev,
                                           FP_FINGER_STATUS_NEEDED,
                                           FP_FINGER_STATUS_NONE);
  self->ssm = fpi_ssm_new_full (dev, eh57a_run_state, EH57A_NUM_STATES,
                                EH57A_NUM_STATES, "egis057a-action");
  fpi_ssm_silence_debug (self->ssm);
  fpi_ssm_start (self->ssm, action_done);
}

/* ------------------------------------------------------------------ */
/* Device vfuncs                                                      */
/* ------------------------------------------------------------------ */

static void
open_fail (FpDevice *dev, GError *error)
{
  FpDeviceEgis057a *self = FPI_DEVICE_EGIS057A (dev);

  g_clear_pointer (&self->sensor, egis057a_dev_free);
  g_usb_device_release_interface (fpi_device_get_usb_device (dev), EGIS057A_INTF, 0, NULL);
  fpi_device_open_complete (dev, error);
}

static void
open_init_done (FpiSsm *init, FpDevice *dev, GError *error)
{
  FpDeviceEgis057a *self = FPI_DEVICE_EGIS057A (dev);

  if (error)
    {
      open_fail (dev, error);
      return;
    }
  self->needs_reinit = FALSE;
  fpi_device_open_complete (dev, NULL);
}

static void
egis057a_open (FpDevice *dev)
{
  FpDeviceEgis057a *self = FPI_DEVICE_EGIS057A (dev);
  GError *error = NULL;

  if (!g_usb_device_claim_interface (fpi_device_get_usb_device (dev),
                                     EGIS057A_INTF, 0, &error))
    {
      fpi_device_open_complete (dev, error);
      return;
    }

  self->sensor = egis057a_dev_new (dev);
  fpi_ssm_start (egis057a_init_ssm (self->sensor), open_init_done);
}

static void
egis057a_close (FpDevice *dev)
{
  FpDeviceEgis057a *self = FPI_DEVICE_EGIS057A (dev);
  GError *error = NULL;

  /* No action can be current here (libfprint refuses close while one is),
   * so no machine of ours is running and no transfer is in flight. */
  g_clear_pointer (&self->sensor, egis057a_dev_free);
  g_usb_device_release_interface (fpi_device_get_usb_device (dev),
                                  EGIS057A_INTF, 0, &error);
  fpi_device_close_complete (dev, error);
}

static void
egis057a_enroll (FpDevice *dev)
{
  FpDeviceEgis057a *self = FPI_DEVICE_EGIS057A (dev);

  self->enroll_count = 0;
  start_action (dev);
}

static void
egis057a_verify (FpDevice *dev)
{
  FpDeviceEgis057a *self = FPI_DEVICE_EGIS057A (dev);
  FpPrint *print = NULL;

  g_autoptr(GPtrArray) prints = g_ptr_array_new ();

  fpi_device_get_verify_data (dev, &print);
  g_ptr_array_add (prints, print);
  if (load_ids (self, prints) == 0)
    {
      g_clear_pointer (&self->gallery_prints, g_ptr_array_unref);
      fpi_device_verify_complete (dev,
                                  fpi_device_error_new_msg (FP_DEVICE_ERROR_DATA_INVALID,
                                                            "Stored print is not an EH57A template"));
      return;
    }
  start_action (dev);
}

static void
egis057a_identify (FpDevice *dev)
{
  FpDeviceEgis057a *self = FPI_DEVICE_EGIS057A (dev);
  GPtrArray *prints = NULL;

  fpi_device_get_identify_data (dev, &prints);
  if (prints && prints->len > EGIS057A_MAX_TEMPLATES)
    {
      /* fprintd passes every print of the user; matching only a prefix would
       * make the later fingers silently unusable for login. Fail loudly
       * instead. */
      fpi_device_identify_complete (dev,
                                    fpi_device_error_new_msg (FP_DEVICE_ERROR_DATA_INVALID,
                                                              "%u prints to match but the chip holds at most %u; "
                                                              "delete some enrolled fingers",
                                                              prints->len, EGIS057A_MAX_TEMPLATES));
      return;
    }
  if (!prints || load_ids (self, prints) == 0)
    {
      /* Empty gallery -- fprintd's pre-enroll duplicate check with nothing
       * enrolled. Trivially "no match": finish without touching the sensor. */
      g_clear_pointer (&self->gallery_prints, g_ptr_array_unref);
      fpi_device_identify_report (dev, NULL, NULL, NULL);
      fpi_device_identify_complete (dev, NULL);
      return;
    }
  start_action (dev);
}

static void
egis057a_cancel (FpDevice *dev)
{
  FpDeviceEgis057a *self = FPI_DEVICE_EGIS057A (dev);

  /* The machine consults the action's cancellable at every transfer-sequence
   * boundary (it never hands it to a transfer, see egis057a_proto.c), so the
   * action ends with G_IO_ERROR_CANCELLED at the next one. A machine parked
   * for suspend has no next boundary of its own; it is sent to the head,
   * which fails it. */
  fp_dbg ("cancelling");
  if (self->parked)
    {
      self->parked = FALSE;
      fpi_ssm_jump_to_state (self->ssm, EH57A_REINIT);
    }
}

/* Suspend with an action running. The action is kept, as the
 * fpi_device_suspend_complete() contract asks for a NULL completion: the
 * machine parks at its next transfer boundary (capture_park_if_suspending) --
 * so the suspend is completed only once no transfer is in flight and none is
 * scheduled -- and resume sends it back to the head, where the session is
 * re-established first. If the action ends on its own first, action_done
 * completes the suspend. No USB here. */
static void
egis057a_suspend (FpDevice *dev)
{
  FpDeviceEgis057a *self = FPI_DEVICE_EGIS057A (dev);

  /* The sensor does not necessarily come back from s2idle with its session. */
  self->needs_reinit = TRUE;
  /* No machine: the action has already ended in the driver's view or it never
   * started one. Nothing of ours is on the bus; complete now. */
  if (!self->ssm)
    {
      fp_dbg ("suspend: no action running");
      fpi_device_suspend_complete (dev, NULL);
      return;
    }
  fp_dbg ("suspend: parking the action at its next transfer boundary");
  self->suspending = TRUE;
  /* An init machine in flight (a re-init) stops at its next boundary too. */
  egis057a_dev_request_stop (self->sensor);
}

/* The device left the bus. An action with a transfer in flight fails on its
 * own; one parked for suspend has nothing in flight and no resume coming
 * (libfprint refuses resume on a removed device), so it is ended here, with
 * no USB. */
static void
egis057a_removed (FpDeviceEgis057a *self)
{
  gboolean removed = FALSE;

  g_object_get (self, "removed", &removed, NULL);
  if (!removed || !self->parked)
    return;
  fp_dbg ("removed while parked for suspend");
  self->parked = FALSE;
  fpi_ssm_mark_failed (self->ssm, fpi_device_error_new (FP_DEVICE_ERROR_REMOVED));
}

static void
egis057a_resume (FpDevice *dev)
{
  FpDeviceEgis057a *self = FPI_DEVICE_EGIS057A (dev);

  fp_dbg ("resume: %s", self->parked ? "restarting the parked action" : "no parked action");
  self->needs_reinit = TRUE;
  fpi_device_resume_complete (dev, NULL);
  if (self->parked)
    {
      self->parked = FALSE;
      fpi_ssm_jump_to_state (self->ssm, EH57A_REINIT);
    }
}

/* ------------------------------------------------------------------ */
/* Driver data                                                        */
/* ------------------------------------------------------------------ */

static const FpIdEntry id_table[] = {
  { .vid = 0x1c7a, .pid = 0x057a, },
  { .vid = 0, .pid = 0, },
};

static void
fpi_device_egis057a_init (FpDeviceEgis057a *self)
{
  g_signal_connect (self, "notify::removed", G_CALLBACK (egis057a_removed), NULL);
}

static void
fpi_device_egis057a_finalize (GObject *object)
{
  FpDeviceEgis057a *self = FPI_DEVICE_EGIS057A (object);

  /* No action can be current here, so nothing of ours is in flight; the
   * sensor is normally freed by close(), but a device destroyed while open
   * (unplugged while claimed) never gets one. */
  g_clear_error (&self->error);
  g_clear_pointer (&self->sensor, egis057a_dev_free);
  g_clear_pointer (&self->gallery_prints, g_ptr_array_unref);
  G_OBJECT_CLASS (fpi_device_egis057a_parent_class)->finalize (object);
}

static void
fpi_device_egis057a_class_init (FpDeviceEgis057aClass *klass)
{
  FpDeviceClass *dev_class = FP_DEVICE_CLASS (klass);
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->finalize = fpi_device_egis057a_finalize;

  dev_class->id = "egis057a";
  dev_class->full_name = "LighTuning Technology Inc. EgisTec EH57A";
  dev_class->type = FP_DEVICE_TYPE_USB;
  dev_class->id_table = id_table;
  dev_class->scan_type = FP_SCAN_TYPE_PRESS;
  dev_class->nr_enroll_stages = EGIS057A_ENROLL_STAGES;

  /* The sensor is safe to poll continuously; opt out of the temperature model
   * so long enrolments / lock-screen polling are not cancelled with
   * FP_DEVICE_ERROR_TOO_HOT (also grants ALWAYS_ON). */
  dev_class->temp_hot_seconds = -1;

  dev_class->open = egis057a_open;
  dev_class->close = egis057a_close;
  dev_class->enroll = egis057a_enroll;
  dev_class->verify = egis057a_verify;
  dev_class->identify = egis057a_identify;
  dev_class->cancel = egis057a_cancel;
  dev_class->suspend = egis057a_suspend;
  dev_class->resume = egis057a_resume;

  fpi_device_class_auto_initialize_features (dev_class);
}

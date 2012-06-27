/*
 * Copyright (c) 2002-2012 BalaBit IT Ltd, Budapest, Hungary
 * Copyright (c) 1998-2012 Balázs Scheidler
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
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 *
 * As an additional exemption you are allowed to compile & link against the
 * OpenSSL libraries as published by the OpenSSL project. See the file
 * COPYING for details.
 *
 */

#include "ml-batched-timer.h"
#include "mainloop.h"

static void
ml_batched_timer_handle(MlBatchedTimer *self)
{
  self->handler(self->cookie);
}

/* function called using main_loop_call() in case the suppress timer needs
 * to be updated */
static void
ml_batched_timer_perform_update(MlBatchedTimer *self)
{
  main_loop_assert_main_thread();

  if (iv_timer_registered(&self->timer))
    iv_timer_unregister(&self->timer);
  g_static_mutex_lock(&self->lock);
  self->timer.expires = self->expires;
  self->updated = TRUE;
  g_static_mutex_unlock(&self->lock);
  if (self->timer.expires.tv_sec > 0)
    iv_timer_register(&self->timer);
  self->unref_cookie(self->cookie);
}

/*
 * Update the suppress timer in a deferred manner, possibly batching the
 * results of multiple updates to the suppress timer.  This is necessary as
 * suppress timer updates must run in the main thread, and updating it every
 * time a new message comes in would cause enormous latency in the fast
 * path. By collecting multiple updates
 *
 * msec == 0 means to turn off the suppress timer
 * msec >  0 to enable the timer with the specified timeout
 *
 * NOTE: suppress_lock must be held.
 */
void
ml_batched_timer_update(MlBatchedTimer *self, glong sec)
{
  gboolean invoke;
  struct timespec next_expires;

  iv_validate_now();

  /* we deliberately use nsec == 0 in order to increase the likelyhood that
   * we target the same second, in case only a fraction of a second has
   * passed between two updates.  */
  if (sec)
    {
      next_expires.tv_nsec = 0;
      next_expires.tv_sec = iv_now.tv_sec + sec;
    }
  else
    {
      next_expires.tv_sec = 0;
      next_expires.tv_nsec = 0;
    }
  /* last update was finished, we need to invoke the updater again */
  invoke = ((next_expires.tv_sec != self->expires.tv_sec) ||
            (next_expires.tv_nsec != self->expires.tv_nsec)) &&
           self->updated;
  self->updated = FALSE;

  if (invoke)
    {
      self->expires = next_expires;
      g_static_mutex_unlock(&self->lock);
      self->ref_cookie(self->cookie);
      main_loop_call((MainLoopTaskFunc) ml_batched_timer_perform_update, self, FALSE);
      g_static_mutex_lock(&self->lock);
    }
}

void
ml_batched_timer_unregister(MlBatchedTimer *self)
{
  if (iv_timer_registered(&self->timer))
    iv_timer_unregister(&self->timer);
  self->updated = TRUE;
}

void
ml_batched_timer_init(MlBatchedTimer *self)
{
  g_static_mutex_init(&self->lock);
  IV_TIMER_INIT(&self->timer);
  self->timer.cookie = self;
  self->timer.handler = (void (*)(void *)) ml_batched_timer_handle;
  self->updated = TRUE;
}

void
ml_batched_timer_deinit(MlBatchedTimer *self)
{
  g_static_mutex_free(&self->lock);
}

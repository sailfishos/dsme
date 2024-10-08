/**
   @file emergencycalltracker.c

   Track the emergency call status by listening to MCE's call state ind.
   This is needed for device state selection by the state module.
   <p>
   Copyright (C) 2009-2010 Nokia Corporation.
   Copyright (C) 2015-2017 Jolla Ltd.

   @author Semi Malinen <semi.malinen@nokia.com>
   @author Simo Piiroinen <simo.piiroinen@jollamobile.com>

   This file is part of Dsme.

   Dsme is free software; you can redistribute it and/or modify
   it under the terms of the GNU Lesser General Public License
   version 2.1 as published by the Free Software Foundation.

   Dsme is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   Lesser General Public License for more details.

   You should have received a copy of the GNU Lesser General Public
   License along with Dsme.  If not, see <http://www.gnu.org/licenses/>.
*/
/*
 * To change emergency call state, use one of these:
 * dbus-send --type=signal --system /com/nokia/dsme com.nokia.mce.signal.sig_call_state_ind string:none
 * dbus-send --type=signal --system /com/nokia/dsme com.nokia.mce.signal.sig_call_state_ind string:emergency
 */

#include "dbusproxy.h"
#include "dsme_dbus.h"

#include "../include/dsme/modules.h"
#include "../include/dsme/logging.h"

#include <dsme/state.h>

#include <string.h>

#define PFIX "emergencycalltracker: "

static void send_emergency_call_status(bool ongoing)
{
    DSM_MSGTYPE_SET_EMERGENCY_CALL_STATE msg =
      DSME_MSG_INIT(DSM_MSGTYPE_SET_EMERGENCY_CALL_STATE);

    msg.ongoing = ongoing;

    modules_broadcast_internally(&msg);
}

static void mce_call_state_ind(const DsmeDbusMessage* ind)
{
  static bool emergency_call_started = false;

  if (strcmp(dsme_dbus_message_get_string(ind), "none"     ) != 0 &&
      strcmp(dsme_dbus_message_get_string(ind), "emergency") == 0)
  {
      /* there is an emergency call going on */
      send_emergency_call_status(true);

      emergency_call_started = true;
      dsme_log(LOG_DEBUG, PFIX "Emergency call started");
  } else if (emergency_call_started) {
      /* the emergency call is over */
      send_emergency_call_status(false);

      emergency_call_started = false;
      dsme_log(LOG_DEBUG, PFIX "Emergency call is over");
  }
}

static const dsme_dbus_signal_binding_t dbus_signals_array[] = {
  { mce_call_state_ind, "com.nokia.mce.signal", "sig_call_state_ind" },
  { 0, 0 }
};

static bool dbus_signals_bound = false;

DSME_HANDLER(DSM_MSGTYPE_DBUS_CONNECTED, client, msg)
{
  dsme_log(LOG_DEBUG, PFIX "emergencycalltracker: DBUS_CONNECTED");
  dsme_dbus_bind_signals(&dbus_signals_bound, dbus_signals_array);
}

DSME_HANDLER(DSM_MSGTYPE_DBUS_DISCONNECT, client, msg)
{
  dsme_log(LOG_DEBUG, PFIX "emergencycalltracker: DBUS_DISCONNECT");
}

module_fn_info_t message_handlers[] = {
  DSME_HANDLER_BINDING(DSM_MSGTYPE_DBUS_CONNECTED),
  DSME_HANDLER_BINDING(DSM_MSGTYPE_DBUS_DISCONNECT),
  { 0 }
};

void module_init(module_t* handle)
{
  /* Do not connect to D-Bus; it is probably not started yet.
   * Instead, wait for DSM_MSGTYPE_DBUS_CONNECTED.
   */

  dsme_log(LOG_DEBUG, PFIX "emergencycalltracker.so loaded");
}

void module_fini(void)
{
  dsme_dbus_unbind_signals(&dbus_signals_bound, dbus_signals_array);

  dsme_log(LOG_DEBUG, PFIX "emergencycalltracker.so unloaded");
}

/**
   @file state.c

   This file implements device state policy in DSME.
   <p>
   Copyright (c) 2004 - 2010 Nokia Corporation.
   Copyright (c) 2013 - 2020 Jolla Ltd.
   Copyright (c) 2020 Open Mobile Platform LLC.

   @author Ismo Laitinen <ismo.laitinen@nokia.com>
   @author Semi Malinen <semi.malinen@nokia.com>
   @author Matias Muhonen <ext-matias.muhonen@nokia.com>
   @author Antti Virtanen <antti.i.virtanen@nokia.com>
   @author Pekka Lundstrom <pekka.lundstrom@jollamobile.com>
   @author Marko Saukko <marko.saukko@jollamobile.com>
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
 * How to send runlevel change indicator:
 * dbus-send --system --type=signal /com/nokia/startup/signal com.nokia.startup.signal.runlevel_switch_done int32:0
 * where the int32 parameter is either 2 (user) or 5 (actdead).
 */

#include "dbusproxy.h"
#include "dsme_dbus.h"

#include "state-internal.h"
#include "runlevel.h"
#include "malf.h"
#include "../include/dsme/timers.h"
#include "../include/dsme/modules.h"
#include "../include/dsme/logging.h"
#include "../include/dsme/modulebase.h"
#include "../include/dsme/mainloop.h"
#include <dsme/state.h>

#include "../dsme/dsme-rd-mode.h"
#include "../dsme/utility.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <strings.h>
#include <signal.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <sys/types.h>

#ifdef DSME_VIBRA_FEEDBACK
#include "vibrafeedback.h"
#endif

#define PFIX "state: "

/**
 * Timer value for actdead shutdown timer. This is how long we wait before doing a
 * shutdown when the charger is disconnected in acting dead state
 */
#define CHARGER_DISCONNECT_TIMEOUT 15

/** Timer value for actdead shutdown timer. This is how long we wait before doing a
 * shutdown when charger is not detected while booting up to act dead state.
 */
#define CHARGER_DISCOVERY_TIMEOUT 5

/**
 * Timer value for shutdown timer. This is how long we wait for apps to close.
 */
#define SHUTDOWN_TIMER_TIMEOUT 2

/**
 * Timer value for acting dead timer. This is how long we wait before doing a
 * state change from user to acting dead.
 */
#define ACTDEAD_TIMER_MIN_TIMEOUT 2
#define ACTDEAD_TIMER_MAX_TIMEOUT 45

/**
 * Timer value for user timer. This is how long we wait before doing a
 * state change from acting dead to user.
 */
#define USER_TIMER_MIN_TIMEOUT 2
#define USER_TIMER_MAX_TIMEOUT 45

/* Seconds from overheating or empty battery to the start of shutdown timer */
#define DSME_THERMAL_SHUTDOWN_TIMER       8
#define DSME_BATTERY_EMPTY_SHUTDOWN_TIMER 8

/**
 * Minimum battery level % that is needed before we allow
 * switch from ACTDEAD to USER
 * TODO: don't hard code this but support config value
 */
#define DSME_MINIMUM_BATTERY_TO_USER    3

typedef enum {
    CHARGER_STATE_UNKNOWN,
    CHARGER_CONNECTED,
    CHARGER_DISCONNECTED,
} charger_state_t;

/* These are the state bits on which dsme bases its state selection.
 *
 * Note: To ease debugging etc, changes to these values must be done
 * via appropriate set/update function instead of direct assignment.
 */
static charger_state_t charger_state          = CHARGER_STATE_UNKNOWN;
static bool            alarm_pending          = false;
static bool            device_overheated      = false;
static bool            emergency_call_ongoing = false;
static bool            shutdown_blocked       = false;
static bool            mounted_to_pc          = false;
static bool            battery_empty          = false;
static bool            shutdown_requested     = false;
static bool            actdead_requested      = false;
static bool            reboot_requested       = false;
static bool            testmode_requested     = false;
static bool            actdead_switch_done    = false;
static bool            user_switch_done       = false;

/* The current battery level percentage
 *
 * Is initialized to -1, which blocks bootup from act dead to user
 * until actual battery level gets reported.
 */
static dsme_battery_level_t dsme_battery_level = DSME_BATTERY_LEVEL_UNKNOWN;

/* the overall dsme state which was selected based on the above bits */
static dsme_state_t current_state = DSME_STATE_NOT_SET;

/* timers for delayed setting of state bits */
static dsme_timer_t overheat_timer           = 0;
static dsme_timer_t charger_disconnect_timer = 0;
static dsme_timer_t battery_empty_timer      = 0;

/* timers for giving other programs a bit of time before shutting down */
static dsme_timer_t delayed_shutdown_timer   = 0;
static dsme_timer_t delayed_actdead_timer    = 0;
static dsme_timer_t delayed_user_timer       = 0;

#ifdef DSME_VIBRA_FEEDBACK
static const char low_battery_event_name[] = "low_battery_vibra_only";
#endif

static void change_state_if_necessary(void);
static void try_to_change_state(dsme_state_t new_state);
static void change_state(dsme_state_t new_state);
static void deny_state_change_request(dsme_state_t denied_state,
                                      const char*  reason);

static void start_delayed_shutdown_timer(unsigned seconds);
static int  delayed_shutdown_fn(void* unused);
#ifdef DSME_SUPPORT_DIRECT_USER_ACTDEAD
static bool start_delayed_actdead_timer(unsigned seconds);
static bool start_delayed_user_timer(unsigned seconds);
#endif
static int  delayed_actdead_fn(void* unused);
static int delayed_user_fn(void* unused);
static void stop_delayed_runlevel_timers(void);
static void change_runlevel(dsme_state_t state);

static void start_overheat_timer(void);
static int  delayed_overheat_fn(void* unused);

static void start_charger_disconnect_timer(int delay_s);
static int  delayed_charger_disconnect_fn(void* unused);
static void stop_charger_disconnect_timer(void);

static bool rd_mode_enabled(void);

static void runlevel_switch_ind(const DsmeDbusMessage* ind);

static const char *bool_repr(bool value);
static const char *charger_state_repr(charger_state_t value);
static void set_charger_state(charger_state_t value);
static void set_alarm_pending(bool value);
static void set_device_overheated(bool value);
static void update_emergency_call_ongoing(bool value);
static void update_shutdown_blocked(bool value);
static void set_mounted_to_pc(bool value);
static void set_battery_empty(bool value);
static void set_shutdown_requested(bool value);
static void set_actdead_requested(bool value);
static void set_reboot_requested(bool value);
static void set_testmode_requested(bool value);
static void set_actdead_switch_done(bool value);
static void set_user_switch_done(bool value);

static const struct {
    int         value;
    const char* name;
} states[] = {
#define DSME_STATE(STATE, VALUE) { VALUE, #STATE },
#include <dsme/state_states.h>
#undef  DSME_STATE
};

static const char* state_name(dsme_state_t state)
{
    int         index_sn;
    const char* name = "*** UNKNOWN STATE ***";;

    for (index_sn = 0; index_sn < sizeof states / sizeof states[0]; ++index_sn) {
        if (states[index_sn].value == state) {
            name = states[index_sn].name;
            break;
        }
    }

    return name;
}

static const dsme_state_t state_value(const char* name)
{
    int          index_sv;
    dsme_state_t state = DSME_STATE_NOT_SET;

    for (index_sv = 0; index_sv < sizeof states / sizeof states[0]; ++index_sv) {
        if (strcasecmp(states[index_sv].name, name) == 0) {
            state = states[index_sv].value;
            break;
        }
    }

    return state;
}

static dsme_runlevel_t state2runlevel(dsme_state_t state)
{
  dsme_runlevel_t runlevel;

  switch (state) {
      case DSME_STATE_SHUTDOWN: runlevel = DSME_RUNLEVEL_SHUTDOWN; break;
      case DSME_STATE_TEST:     runlevel = DSME_RUNLEVEL_TEST;     break;
      case DSME_STATE_USER:     runlevel = DSME_RUNLEVEL_USER;     break;
      case DSME_STATE_LOCAL:    runlevel = DSME_RUNLEVEL_LOCAL;
      case DSME_STATE_ACTDEAD:  runlevel = DSME_RUNLEVEL_ACTDEAD;  break;
      case DSME_STATE_REBOOT:   runlevel = DSME_RUNLEVEL_REBOOT;   break;

      case DSME_STATE_NOT_SET:  /* FALL THROUGH */
      case DSME_STATE_BOOT:     /* FALL THROUGH */
      default:                  runlevel = DSME_RUNLEVEL_SHUTDOWN; break;
  }

  return runlevel;
}

static const char *bool_repr(bool value)
{
    return value ? "true" : "false";
}

static const char *charger_state_repr(charger_state_t value)
{
    const char *repr = "invalid";
    switch( value ) {
    case CHARGER_STATE_UNKNOWN: repr = "unknown";      break;
    case CHARGER_CONNECTED:     repr = "connected";    break;
    case CHARGER_DISCONNECTED:  repr = "disconnected"; break;
    }
    return repr;
}

static void set_charger_state(charger_state_t value)
{
    if( charger_state != value ) {
        dsme_log(LOG_INFO, PFIX "charger_state: %s -> %s",
                 charger_state_repr(charger_state), charger_state_repr(value));
        charger_state = value;
    }
}

static void set_alarm_pending(bool value)
{
    if( alarm_pending != value ) {
        dsme_log(LOG_INFO, PFIX "alarm_pending: %s -> %s",
                 bool_repr(alarm_pending), bool_repr(value));
        alarm_pending = value;
    }
}

static void set_device_overheated(bool value)
{
    if( device_overheated != value ) {
        dsme_log(LOG_WARNING, PFIX "device_overheated: %s -> %s",
                 bool_repr(device_overheated), bool_repr(value));
        device_overheated = value;
    }
}

static void update_emergency_call_ongoing(bool value)
{
    if( emergency_call_ongoing != value ) {
        dsme_log(LOG_WARNING, PFIX "emergency_call_ongoing: %s -> %s",
                 bool_repr(emergency_call_ongoing), bool_repr(value));
        if( (emergency_call_ongoing = value) ) {
            /* Stop also already scheduled shutdown / reboot */
            stop_delayed_runlevel_timers();
        }
        else {
            change_state_if_necessary();
        }
    }
}

static void update_shutdown_blocked(bool value)
{
    if( shutdown_blocked != value ) {
        dsme_log(LOG_NOTICE, PFIX "shutdown_blocked: %s -> %s",
                 bool_repr(shutdown_blocked), bool_repr(value));
        if( (shutdown_blocked = value) ) {
            /* Already scheduled shutdown / reboot will happen */
        }
        else {
            /* Forget any shutdown / reboot requests
             * received while shutdown was blocked. */
            set_shutdown_requested(false);
            set_reboot_requested(false);

            change_state_if_necessary();
        }
    }
}

static void set_mounted_to_pc(bool value)
{
    if( mounted_to_pc != value ) {
        dsme_log(LOG_NOTICE, PFIX "mounted_to_pc: %s -> %s",
                 bool_repr(mounted_to_pc), bool_repr(value));
        mounted_to_pc = value;
    }
}

static void set_battery_empty(bool value)
{
    if( battery_empty != value ) {
        dsme_log(LOG_WARNING, PFIX "battery_empty: %s -> %s",
                 bool_repr(battery_empty), bool_repr(value));
        battery_empty = value;
    }
}

static void set_shutdown_requested(bool value)
{
    if( shutdown_requested != value ) {
        dsme_log(LOG_NOTICE, PFIX "shutdown_requested: %s -> %s",
                 bool_repr(shutdown_requested), bool_repr(value));
        shutdown_requested = value;
    }
}

static void set_actdead_requested(bool value)
{
    if( actdead_requested != value ) {
        dsme_log(LOG_NOTICE, PFIX "actdead_requested: %s -> %s",
                 bool_repr(actdead_requested), bool_repr(value));
        actdead_requested = value;
    }
}

static void set_reboot_requested(bool value)
{
    if( reboot_requested != value ) {
        dsme_log(LOG_NOTICE, PFIX "reboot_requested: %s -> %s",
                 bool_repr(reboot_requested), bool_repr(value));
        reboot_requested = value;
    }
}

static void set_testmode_requested(bool value)
{
    if( testmode_requested != value ) {
        dsme_log(LOG_NOTICE, PFIX "testmode_requested: %s -> %s",
                 bool_repr(testmode_requested), bool_repr(value));
        testmode_requested = value;
    }
}

static void set_actdead_switch_done(bool value)
{
    if( actdead_switch_done != value ) {
        dsme_log(LOG_INFO, PFIX "actdead_switch_done: %s -> %s",
                 bool_repr(actdead_switch_done), bool_repr(value));
        actdead_switch_done = value;
    }
}

static void set_user_switch_done(bool value)
{
    if( user_switch_done != value ) {
        dsme_log(LOG_INFO, PFIX "user_switch_done: %s -> %s",
                 bool_repr(user_switch_done), bool_repr(value));
        user_switch_done = value;
    }
}

#ifndef DSME_SUPPORT_DIRECT_USER_ACTDEAD
static bool need_to_use_reboot(dsme_state_t target_state)
{
    static const char output_path[] = "/run/systemd/reboot-param";

    bool use_reboot = false;
    int  input_fd   = -1;
    int  output_fd  = -1;

    char input_path[PATH_MAX];
    char param[256];

    /* Determine path for dsme reboot param config file */

    const char *target_tag  = "unknown";
    const char *charger_tag = "without-charger";

    switch( target_state ) {
    case DSME_STATE_SHUTDOWN: target_tag = "shutdown"; break;
    case DSME_STATE_USER:     target_tag = "user";     break;
    case DSME_STATE_ACTDEAD:  target_tag = "actdead";  break;
    case DSME_STATE_REBOOT:   target_tag = "reboot";   break;
    case DSME_STATE_TEST:     target_tag = "test";     break;
    case DSME_STATE_MALF:     target_tag = "malf";     break;
    case DSME_STATE_BOOT:     target_tag = "boot";     break;
    case DSME_STATE_LOCAL:    target_tag = "local";    break;
    default: break;
    }

    if( charger_state == CHARGER_CONNECTED )
        charger_tag = "with-charger";

    snprintf(input_path, sizeof input_path, "/etc/dsme/reboot-to-%s-%s.param",
             target_tag, charger_tag);

    /* Read reboot param from dsme config file */

    if( (input_fd = open(input_path, O_RDONLY)) == -1 ) {
        if( errno != ENOENT )
            dsme_log(LOG_ERR, PFIX "%s: can't read reboot param: %m",
                     input_path);
        goto EXIT;
    }

    int todo = read(input_fd, param, sizeof param - 1);
    if( todo == -1 ) {
        dsme_log(LOG_ERR, PFIX "%s: can't read reboot param: %m",
                 input_path);
        goto EXIT;
    }

    param[todo] = 0;
    todo = strcspn(param, "\r\n");
    param[todo] = 0;

    /* Write parameter to where systemd expects it to be at */

    output_fd = open(output_path, O_WRONLY|O_CREAT|O_TRUNC, 0644);
    if( output_fd == -1 ) {
        dsme_log(LOG_ERR, PFIX "%s: can't write reboot param: %m",
                 output_path);
        goto EXIT;
    }

    int done = write(output_fd, param, todo);

    if( done == -1 ) {
        dsme_log(LOG_ERR, PFIX "%s: can't write reboot param: %m",
                 output_path);
        goto EXIT;
    }
    if( done != todo ) {
        dsme_log(LOG_ERR, PFIX "%s: can't write reboot param: %s",
                 output_path, "partial write");;
        goto EXIT;
    }

    /* Success - should use reboot instead of shutdown */

    use_reboot = true;
    dsme_log(LOG_DEBUG, PFIX "%s: using '%s'", output_path, param);

EXIT:
    if( input_fd != -1 )
        close(input_fd);

    if( output_fd != -1 )
        close(output_fd);

    if( !use_reboot && unlink(output_path) == -1 && errno != ENOENT ) {
        dsme_log(LOG_WARNING, PFIX "%s: can't remove reboot param: %m",
                 output_path);
    }

    return use_reboot;
}
#endif

static dsme_state_t select_state(void)
{
    dsme_state_t state = current_state;

    if( emergency_call_ongoing ) {
        /* don't touch anything if we have an emergency call going on */
        dsme_log(LOG_NOTICE, PFIX "Transitions blocked by emergency call");
    }
    else if( device_overheated ) {
        dsme_log(LOG_CRIT, PFIX "Thermal shutdown!");
        state = DSME_STATE_SHUTDOWN;
    }
    else if( battery_empty ) {
        dsme_log(LOG_CRIT, PFIX "Battery empty shutdown!");
        state = DSME_STATE_SHUTDOWN;
    }
    else if( shutdown_blocked ) {
        /* Block non-emergency transitions */
        dsme_log(LOG_NOTICE, PFIX "Transitions blocked by D-Bus clients");
    }
    else if( testmode_requested ) {
        state = DSME_STATE_TEST;
    }
    else if( actdead_requested ) {
        /* favor actdead requests over shutdown & reboot */
        dsme_log(LOG_NOTICE, PFIX "Actdead by request");
        state = DSME_STATE_ACTDEAD;
    }
    else if( shutdown_requested || reboot_requested ) {
        /* favor normal shutdown over reboot over actdead */
        if( shutdown_requested &&
            (charger_state == CHARGER_DISCONNECTED) &&
            (!alarm_pending || dsme_home_is_encrypted()) ) {
            dsme_log(LOG_NOTICE, PFIX "Normal shutdown%s",
                     alarm_pending
                     ? " (alarm set, but ignored due to encrypted home)"
                     : "");
            state = DSME_STATE_SHUTDOWN;
        }
        else if( reboot_requested ) {
            dsme_log(LOG_NOTICE, PFIX "Reboot");
            state = DSME_STATE_REBOOT;
        }
        else {
            dsme_log(LOG_NOTICE, PFIX "Actdead (charger: %s, alarm: %s)",
                     charger_state == CHARGER_CONNECTED ? "on"  : "off(?)",
                     alarm_pending                          ? "set" : "not set");
            state = DSME_STATE_ACTDEAD;
        }
    }
    else {
        state = DSME_STATE_USER;
    }
    return state;
}

static void change_state_if_necessary(void)
{
  dsme_state_t next_state = select_state();

  if (current_state != next_state) {
      try_to_change_state(next_state);
  }
}

static void try_to_change_state(dsme_state_t new_state)
{
  dsme_log(LOG_INFO, PFIX "state change request: %s -> %s",
           state_name(current_state),
           state_name(new_state));

  switch (new_state) {
    case DSME_STATE_SHUTDOWN: /* Runlevel 0 */ /* FALL THROUGH */
    case DSME_STATE_REBOOT:   /* Runlevel 6 */
      change_state(new_state);
      start_delayed_shutdown_timer(SHUTDOWN_TIMER_TIMEOUT);
      break;

    case DSME_STATE_USER:    /* Runlevel 5 */ /* FALL THROUGH */
    case DSME_STATE_ACTDEAD: /* Runlevel 4 */
      if (current_state == DSME_STATE_NOT_SET) {
          /* we have just booted up; simply change the state */
          change_state(new_state);
      } else if (current_state == DSME_STATE_ACTDEAD) {
          /* We are in actdead and user state is wanted
           * We don't allow that to happen if battery level is too low
           */
          if (dsme_battery_level < DSME_MINIMUM_BATTERY_TO_USER ) {
              dsme_log(LOG_WARNING, PFIX "Battery level %d%% too low for %s state",
                       dsme_battery_level,
                       state_name(new_state));
#ifdef DSME_VIBRA_FEEDBACK
              /* Indicate by vibra that boot is not possible */
              dsme_play_vibra(low_battery_event_name);
#endif
              /* We need to return initial ACT_DEAD shutdown_reqest
               * as it got cleared when USER state transfer was requested
               */
              set_shutdown_requested(true);
              break;
          }
          /* Battery ok, lets do it */
          set_user_switch_done(false);
#ifndef DSME_SUPPORT_DIRECT_USER_ACTDEAD
          /* We don't support direct transfer from ACTDEAD to USER
           * but do it via reboot.
           */
          dsme_log(LOG_DEBUG, PFIX "USER state requested, we do it via REBOOT");
          change_state(DSME_STATE_REBOOT);
          start_delayed_shutdown_timer(SHUTDOWN_TIMER_TIMEOUT);
#else
          if (actdead_switch_done) {
              /* actdead init done; runlevel change from actdead to user state */
              if (start_delayed_user_timer(USER_TIMER_MIN_TIMEOUT)) {
                  change_state(new_state);
              }
          } else {
              /* actdead init not done; wait longer to change from actdead to user state */
              if (start_delayed_user_timer(USER_TIMER_MAX_TIMEOUT)) {
                  change_state(new_state);
              }
          }
#endif /* DSME_SUPPORT_DIRECT_USER_ACTDEAD */
      } else if (current_state == DSME_STATE_USER) {
          set_actdead_switch_done(false);
#ifndef DSME_SUPPORT_DIRECT_USER_ACTDEAD
          /* We don't support direct transfer from USER to ACTDEAD
           * but do it via shutdown. Usb cable will wakeup the device again
           * and then we will boot to ACTDEAD
           * Force SHUTDOWN
           */
          if( need_to_use_reboot(new_state) ) {
              dsme_log(LOG_DEBUG, PFIX "ACTDEAD state requested, "
                       "we do it via REBOOT");
              change_state(DSME_STATE_REBOOT);
          }
          else {
              dsme_log(LOG_DEBUG, PFIX "ACTDEAD state requested, "
                       "we do it via SHUTDOWN");
              change_state(DSME_STATE_SHUTDOWN);
          }
          start_delayed_shutdown_timer(SHUTDOWN_TIMER_TIMEOUT);
#else
          if (user_switch_done) {
              /* user init done; runlevel change from user to actdead state */
              if (start_delayed_actdead_timer(ACTDEAD_TIMER_MIN_TIMEOUT)) {
                  change_state(new_state);
              }
          } else {
              /* user init not done; wait longer to change from user to actdead state */
              if (start_delayed_actdead_timer(ACTDEAD_TIMER_MAX_TIMEOUT)) {
                  change_state(new_state);
              }
          }
#endif /* DSME_SUPPORT_DIRECT_USER_ACTDEAD */
      }
      break;

    case DSME_STATE_TEST:  /* fall through */
    case DSME_STATE_LOCAL: /* NOTE: test server is running */
      if (current_state == DSME_STATE_NOT_SET) {
          change_state(new_state);
      }
      break;

    default:
      dsme_log(LOG_WARNING, PFIX "not possible to change to state %s (%d)",
               state_name(new_state),
               new_state);
      break;
  }
}

/**
 * This function sends a message to clients to notify about state change.
 * SAVE_DATA message is also sent when going down.
 * @param state State that is being activated
 */
static void change_state(dsme_state_t new_state)
{
  if (new_state == DSME_STATE_SHUTDOWN ||
      new_state == DSME_STATE_REBOOT)
  {
      DSM_MSGTYPE_SAVE_DATA_IND save_msg =
        DSME_MSG_INIT(DSM_MSGTYPE_SAVE_DATA_IND);

      dsme_log(LOG_DEBUG, PFIX "sending SAVE_DATA");
      modules_broadcast(&save_msg);
  }

  DSM_MSGTYPE_STATE_CHANGE_IND ind_msg =
    DSME_MSG_INIT(DSM_MSGTYPE_STATE_CHANGE_IND);

  ind_msg.state = new_state;
  dsme_log(LOG_DEBUG, PFIX "STATE_CHANGE_IND sent (%s)", state_name(new_state));
  modules_broadcast(&ind_msg);

  dsme_log(LOG_NOTICE, PFIX "new state: %s", state_name(new_state));
  current_state = new_state;
}

static bool is_state_change_request_acceptable(dsme_state_t requested_state)
{
    bool acceptable = true;

    // do not allow shutdown/reboot when in usb mass storage mode
    if ((requested_state == DSME_STATE_SHUTDOWN ||
         requested_state == DSME_STATE_REBOOT) &&
        mounted_to_pc)
    {
        acceptable = false;
        deny_state_change_request(requested_state, "usb");
    }

    return acceptable;
}

static void deny_state_change_request(dsme_state_t denied_state,
                                      const char*  reason)
{
  DSM_MSGTYPE_STATE_REQ_DENIED_IND ind =
      DSME_MSG_INIT(DSM_MSGTYPE_STATE_REQ_DENIED_IND);

  ind.state = denied_state;
  modules_broadcast_with_extra(&ind, strlen(reason) + 1, reason);
  dsme_log(LOG_CRIT, PFIX "%s denied due to: %s",
           (denied_state == DSME_STATE_SHUTDOWN ? "shutdown" : "reboot"),
           reason);
}

static void start_delayed_shutdown_timer(unsigned seconds)
{
  if (!delayed_shutdown_timer) {
      stop_delayed_runlevel_timers();
      if (!(delayed_shutdown_timer = dsme_create_timer_seconds(seconds,
                                                               delayed_shutdown_fn,
                                                               NULL)))
      {
          dsme_log(LOG_CRIT, PFIX "Could not create a shutdown timer; exit!");
          dsme_main_loop_quit(EXIT_FAILURE);
          return;
      }
      dsme_log(LOG_NOTICE, PFIX "Shutdown or reboot in %i seconds", seconds);
  }
}

static int delayed_shutdown_fn(void* unused)
{
  DSM_MSGTYPE_SHUTDOWN msg = DSME_MSG_INIT(DSM_MSGTYPE_SHUTDOWN);
  msg.runlevel = state2runlevel(current_state);
  modules_broadcast_internally(&msg);

  delayed_shutdown_timer = 0;
  return 0; /* stop the interval */
}

#ifdef DSME_SUPPORT_DIRECT_USER_ACTDEAD
static bool start_delayed_actdead_timer(unsigned seconds)
{
  bool success = false;
  if (!delayed_shutdown_timer && !delayed_actdead_timer && !delayed_user_timer) {
      if (!(delayed_actdead_timer = dsme_create_timer_seconds(seconds,
                                                              delayed_actdead_fn,
                                                              NULL)))
      {
          dsme_log(LOG_CRIT, PFIX "Could not create an actdead timer; exit!");
          dsme_main_loop_quit(EXIT_FAILURE);
          return false;
      }
      success = true;
      dsme_log(LOG_NOTICE, PFIX "Actdead in %i seconds", seconds);
  }
  return success;
}
#endif /* DSME_SUPPORT_DIRECT_USER_ACTDEAD */

static int delayed_actdead_fn(void* unused)
{
  change_runlevel(DSME_STATE_ACTDEAD);

  delayed_actdead_timer = 0;

  return 0; /* stop the interval */
}

#ifdef DSME_SUPPORT_DIRECT_USER_ACTDEAD
static bool start_delayed_user_timer(unsigned seconds)
{
  bool success = false;
  if (!delayed_shutdown_timer && !delayed_actdead_timer && !delayed_user_timer) {
      if (!(delayed_user_timer = dsme_create_timer_seconds(seconds,
                                                           delayed_user_fn,
                                                           NULL)))
      {
          dsme_log(LOG_CRIT, PFIX "Could not create a user timer; exit!");
          dsme_main_loop_quit(EXIT_FAILURE);
          return false;
      }
      success = true;
      dsme_log(LOG_NOTICE, PFIX "User in %i seconds", seconds);
  }
  return success;
}
#endif /* DSME_SUPPORT_DIRECT_USER_ACTDEAD */

static int delayed_user_fn(void* unused)
{
  change_runlevel(DSME_STATE_USER);

  delayed_user_timer = 0;

  return 0; /* stop the interval */
}

static void change_runlevel(dsme_state_t state)
{
  DSM_MSGTYPE_CHANGE_RUNLEVEL msg = DSME_MSG_INIT(DSM_MSGTYPE_CHANGE_RUNLEVEL);
  msg.runlevel = state2runlevel(state);
  modules_broadcast_internally(&msg);
}

static void stop_delayed_runlevel_timers(void)
{
    if (delayed_shutdown_timer) {
        dsme_destroy_timer(delayed_shutdown_timer);
        delayed_shutdown_timer = 0;
        dsme_log(LOG_NOTICE, PFIX "Delayed shutdown timer stopped");
    }
    if (delayed_actdead_timer) {
        dsme_destroy_timer(delayed_actdead_timer);
        delayed_actdead_timer = 0;
        dsme_log(LOG_NOTICE, PFIX "Delayed actdead timer stopped");
    }
    if (delayed_user_timer) {
        dsme_destroy_timer(delayed_user_timer);
        delayed_user_timer = 0;
        dsme_log(LOG_NOTICE, PFIX "Delayed user timer stopped");
    }
}

static void stop_overheat_timer(void)
{
  if (overheat_timer) {
    dsme_destroy_timer(overheat_timer);
    overheat_timer = 0;
  }
}

static void start_overheat_timer(void)
{
  if (!overheat_timer) {
      if (!(overheat_timer = dsme_create_timer_seconds(DSME_THERMAL_SHUTDOWN_TIMER,
                                                       delayed_overheat_fn,
                                                       NULL)))
      {
          dsme_log(LOG_CRIT, PFIX "Could not create a timer; overheat immediately!");
          delayed_overheat_fn(0);
      } else {
          dsme_log(LOG_CRIT, PFIX "Thermal shutdown in %d seconds",
                   DSME_THERMAL_SHUTDOWN_TIMER);
      }
  }
}

static int delayed_overheat_fn(void* unused)
{
  set_device_overheated(true);
  change_state_if_necessary();

  overheat_timer = 0;
  return 0; /* stop the interval */
}

static void start_charger_disconnect_timer(int delay_s)
{
  if (!charger_disconnect_timer) {
      if (!(charger_disconnect_timer = dsme_create_timer_seconds(delay_s,
                                                                 delayed_charger_disconnect_fn,
                                                                 NULL)))
      {
          dsme_log(LOG_ERR, PFIX "Could not create a timer; disconnect immediately!");
          delayed_charger_disconnect_fn(0);
      } else {
          dsme_log(LOG_DEBUG, PFIX "Handle charger disconnect in %d seconds",
                   delay_s);
      }
  }
}

static int delayed_charger_disconnect_fn(void* unused)
{
  set_charger_state(CHARGER_DISCONNECTED);
  change_state_if_necessary();

  charger_disconnect_timer = 0;
  return 0; /* stop the interval */
}

static void stop_charger_disconnect_timer(void)
{
  if (charger_disconnect_timer) {
      dsme_destroy_timer(charger_disconnect_timer);
      charger_disconnect_timer = 0;
      dsme_log(LOG_DEBUG, PFIX "Charger disconnect timer stopped");

      /* the last we heard, the charger had just been disconnected */
      set_charger_state(CHARGER_DISCONNECTED);
  }
}

DSME_HANDLER(DSM_MSGTYPE_SET_CHARGER_STATE, conn, msg)
{
  charger_state_t new_charger_state;

  dsme_log(LOG_DEBUG, PFIX "charger %s state received",
           msg->connected ? "connected" : "disconnected");

  new_charger_state = msg->connected ? CHARGER_CONNECTED : CHARGER_DISCONNECTED;

  stop_charger_disconnect_timer();

  if (current_state     == DSME_STATE_ACTDEAD &&
      new_charger_state == CHARGER_DISCONNECTED)
  {
      if (charger_state == CHARGER_STATE_UNKNOWN) {
          /* When booting to act-dead allow usb-moded some time
           * to figure out whether there is a charger connected
           * or not before shutting down.
           */
          start_charger_disconnect_timer(CHARGER_DISCOVERY_TIMEOUT);
      } else {
          /* We are in acting dead, and the charger is disconnected.
           * Moreover, this is not the first time charger is disconnected;
           * shutdown after a while if charger is not connected again
           */
          start_charger_disconnect_timer(CHARGER_DISCONNECT_TIMEOUT);
      }
  } else {
      set_charger_state(new_charger_state);
      change_state_if_necessary();
  }
}

DSME_HANDLER(DSM_MSGTYPE_SET_USB_STATE, conn, msg)
{
    set_mounted_to_pc(msg->mounted_to_pc);
}

// handlers for telinit requests
static void handle_telinit_NOT_SET(endpoint_t* conn)
{
    dsme_log(LOG_WARNING, PFIX "ignoring unknown telinit runlevel request");
}

static void handle_telinit_SHUTDOWN(endpoint_t* conn)
{
    if( !endpoint_is_privileged(conn) ) {
        dsme_log(LOG_WARNING, PFIX "shutdown request from unprivileged client");
    }
    else if (is_state_change_request_acceptable(DSME_STATE_SHUTDOWN)) {
        set_shutdown_requested(true);
        set_actdead_requested(false);
        change_state_if_necessary();
    }
}

static void handle_telinit_USER(endpoint_t* conn)
{
    if( !endpoint_is_privileged(conn) ) {
        dsme_log(LOG_WARNING, PFIX "powerup request from unprivileged client");
    }
    else {
        set_shutdown_requested(false);
        set_actdead_requested(false);
        change_state_if_necessary();
    }
}

static void handle_telinit_ACTDEAD(endpoint_t* conn)
{
    if( !endpoint_is_privileged(conn) ) {
        dsme_log(LOG_WARNING, PFIX "actdead request from unprivileged client");
    }
    else if (is_state_change_request_acceptable(DSME_STATE_ACTDEAD)) {
        set_actdead_requested(true);
        change_state_if_necessary();
    }
}

static void handle_telinit_REBOOT(endpoint_t* conn)
{
    if( !endpoint_is_privileged(conn) ) {
        dsme_log(LOG_WARNING, PFIX "reboot request from unprivileged client");
    }
    else if (is_state_change_request_acceptable(DSME_STATE_REBOOT)) {
        set_reboot_requested(true);
        set_actdead_requested(false);
        change_state_if_necessary();
    }
}

static void handle_telinit_TEST(endpoint_t* conn)
{
    dsme_log(LOG_WARNING, PFIX "telinit TEST unimplemented");
}

static void handle_telinit_MALF(endpoint_t* conn)
{
    dsme_log(LOG_WARNING, PFIX "telinit MALF unimplemented");
}

static void handle_telinit_BOOT(endpoint_t* conn)
{
    dsme_log(LOG_WARNING, PFIX "telinit BOOT unimplemented");
}

static void handle_telinit_LOCAL(endpoint_t* conn)
{
    dsme_log(LOG_WARNING, PFIX "telinit LOCAL unimplemented");
}

typedef void (telinit_handler_fn_t)(endpoint_t* conn);

static telinit_handler_fn_t* telinit_handler(dsme_state_t state)
{
    static const struct {
        dsme_state_t          state;
        telinit_handler_fn_t* handler;
    } handlers[] = {
#define DSME_STATE(STATE, VALUE) \
        { DSME_STATE_ ## STATE, handle_telinit_ ## STATE },
#include <dsme/state_states.h>
#undef  DSME_STATE
    };

    int index_th;
    telinit_handler_fn_t* handler = handle_telinit_NOT_SET;

    for (index_th = 0; index_th < sizeof states / sizeof states[0]; ++index_th) {
        if (handlers[index_th].state == state) {
            handler = handlers[index_th].handler;
            break;
        }
    }

    return handler;
}

DSME_HANDLER(DSM_MSGTYPE_TELINIT, conn, msg)
{
    const char* runlevel = DSMEMSG_EXTRA(msg);
    char*       sender   = endpoint_name(conn);

    dsme_log(LOG_NOTICE, PFIX "got telinit '%s' from %s",
             runlevel ? runlevel : "(null)",
             sender   ? sender   : "(unknown)");
    free(sender);

    if (runlevel) {
        telinit_handler(state_value(runlevel))(conn);
    }
}

/**
 * Shutdown requested.
 * We go to actdead state if alarm is set (or snoozed) or charger connected.
 */
DSME_HANDLER(DSM_MSGTYPE_SHUTDOWN_REQ, conn, msg)
{
  char* sender = endpoint_name(conn);
  dsme_log(LOG_NOTICE, PFIX "shutdown request received from %s",
           (sender ? sender : "(unknown)"));
  free(sender);

  handle_telinit_SHUTDOWN(conn);
}

DSME_HANDLER(DSM_MSGTYPE_REBOOT_REQ, conn, msg)
{
  char* sender = endpoint_name(conn);
  dsme_log(LOG_NOTICE, PFIX "reboot request received from %s",
           (sender ? sender : "(unknown)"));
  free(sender);

  handle_telinit_REBOOT(conn);
}

/**
 * Power up requested.
 * This means ACTDEAD -> USER transition.
 */
DSME_HANDLER(DSM_MSGTYPE_POWERUP_REQ, conn, msg)
{
  char* sender = endpoint_name(conn);
  dsme_log(LOG_NOTICE, PFIX "powerup request received from %s",
           (sender ? sender : "(unknown)"));
  free(sender);

  handle_telinit_USER(conn);
}

DSME_HANDLER(DSM_MSGTYPE_SET_ALARM_STATE, conn, msg)
{
  dsme_log(LOG_DEBUG, PFIX "alarm %s state received",
           msg->alarm_set ? "set or snoozed" : "not set");

  set_alarm_pending(msg->alarm_set);

  change_state_if_necessary();
}

DSME_HANDLER(DSM_MSGTYPE_SET_THERMAL_STATUS, conn, msg)
{
  dsme_log(LOG_NOTICE, PFIX "%s state received",
           (msg->status == DSM_THERMAL_STATUS_OVERHEATED) ? "overheated" :
           (msg->status == DSM_THERMAL_STATUS_LOWTEMP) ? "low temp warning" :
           "normal temp");

  if (msg->status == DSM_THERMAL_STATUS_OVERHEATED) {
      start_overheat_timer();
  } else {
      /* there is no going back from being overheated */
  }
}

DSME_HANDLER(DSM_MSGTYPE_SET_EMERGENCY_CALL_STATE, conn, msg)
{
  dsme_log(LOG_NOTICE, PFIX "emergency call %s state received",
           msg->ongoing ? "on" : "off");

  update_emergency_call_ongoing(msg->ongoing);

  change_state_if_necessary();
}

static int delayed_battery_empty_fn(void* unused)
{
    set_battery_empty(true);
    change_state_if_necessary();

    battery_empty_timer = 0;
    return 0; /* stop the interval */
}

static void stop_battery_empty_timer(void)
{
  if (battery_empty_timer) {
    dsme_destroy_timer(battery_empty_timer);
    battery_empty_timer = 0;
  }
}

static void start_battery_empty_timer(void)
{
  if (!battery_empty_timer) {
    battery_empty_timer = dsme_create_timer_seconds(DSME_BATTERY_EMPTY_SHUTDOWN_TIMER,
                                                    delayed_battery_empty_fn,
                                                    NULL);
    if (!battery_empty_timer)
    {
      dsme_log(LOG_ERR, PFIX "Cannot create timer; battery empty shutdown immediately!");
      delayed_battery_empty_fn(0);
    }
    else
    {
      dsme_log(LOG_CRIT, PFIX "Battery empty shutdown in %d seconds",
               DSME_BATTERY_EMPTY_SHUTDOWN_TIMER);
    }
  }
}

DSME_HANDLER(DSM_MSGTYPE_SET_BATTERY_LEVEL, conn, battery)
{
    dsme_log(LOG_INFO, PFIX "battery level=%d received",
             battery->level);

    dsme_battery_level = battery->level;
}

DSME_HANDLER(DSM_MSGTYPE_SET_BATTERY_STATE, conn, battery)
{
  dsme_log(LOG_NOTICE, PFIX "battery %s state received",
           battery->empty ? "empty" : "not empty");

  if (battery->empty) {
      /* we have to shut down; first send the notification */
      DSM_MSGTYPE_BATTERY_EMPTY_IND battery_empty_ind =
          DSME_MSG_INIT(DSM_MSGTYPE_BATTERY_EMPTY_IND);

      modules_broadcast(&battery_empty_ind);

      /* then set up a delayed shutdown */
      start_battery_empty_timer();
  }
  else {
      /* Cancel delayed shutdown */
      stop_battery_empty_timer();
  }
}

DSME_HANDLER(DSM_MSGTYPE_STATE_QUERY, client, msg)
{
  DSM_MSGTYPE_STATE_CHANGE_IND ind_msg =
    DSME_MSG_INIT(DSM_MSGTYPE_STATE_CHANGE_IND);

  dsme_log(LOG_DEBUG, PFIX "state_query, state: %s", state_name(current_state));

  ind_msg.state = current_state;
  endpoint_send(client, &ind_msg);
}

/**
 * Reads the RD mode state and returns true if enabled
 */
static bool rd_mode_enabled(void)
{
  bool          enabled;

  if (dsme_rd_mode_enabled()) {
      dsme_log(LOG_NOTICE, PFIX "R&D mode enabled");
      enabled = true;
  } else {
      enabled = false;
      dsme_log(LOG_DEBUG, PFIX "R&D mode disabled");
  }

  return enabled;
}

/*
 * catches the D-Bus signal com.nokia.startup.signal.runlevel_switch_done,
 * which is emitted whenever the runlevel init scripts have been completed.
 */
static void runlevel_switch_ind(const DsmeDbusMessage* ind)
{
    /* The runlevel for which init was completed */
    int runlevel_ind = dsme_dbus_message_get_int(ind);

    switch (runlevel_ind) {
        case DSME_RUNLEVEL_ACTDEAD: {
            /*  USER -> ACTDEAD runlevel change done */
            set_actdead_switch_done(true);
            dsme_log(LOG_DEBUG, PFIX "USER -> ACTDEAD runlevel change done");

            /* Do we have a pending ACTDEAD -> USER timer? */
            if (delayed_user_timer) {
                /* Destroy timer and immediately switch to USER because init is done */
                dsme_destroy_timer(delayed_user_timer);
                delayed_user_fn(0);
            }
            break;
        }
        case DSME_RUNLEVEL_USER: {
            /* ACTDEAD -> USER runlevel change done */
            set_user_switch_done(true);
            dsme_log(LOG_DEBUG, PFIX "ACTDEAD -> USER runlevel change done");

            /* Do we have a pending USER -> ACTDEAD timer? */
            if (delayed_actdead_timer) {
                /* Destroy timer and immediately switch to ACTDEAD because init is done */
                dsme_destroy_timer(delayed_actdead_timer);
                delayed_actdead_fn(0);
            }
            break;
        }
        default: {
            /*
             * Currently, we only get a runlevel switch signal for USER and ACTDEAD (NB#199301)
             */
            dsme_log(LOG_NOTICE, PFIX "Unhandled runlevel switch indicator signal. runlevel: %i", runlevel_ind);
            break;
        }
    }
}

static bool dbus_signals_bound = false;

static const dsme_dbus_signal_binding_t dbus_signals_array[] = {
    { runlevel_switch_ind, "com.nokia.startup.signal", "runlevel_switch_done" },
    { 0, 0 }
};

DSME_HANDLER(DSM_MSGTYPE_DBUS_CONNECTED, client, msg)
{
  dsme_log(LOG_DEBUG, PFIX "DBUS_CONNECTED");
  dsme_dbus_bind_signals(&dbus_signals_bound, dbus_signals_array);
#ifdef DSME_VIBRA_FEEDBACK
  dsme_ini_vibrafeedback();
#endif // DSME_VIBRA_FEEDBACK
}

DSME_HANDLER(DSM_MSGTYPE_DBUS_DISCONNECT, client, msg)
{
  dsme_log(LOG_DEBUG, PFIX "DBUS_DISCONNECT");
}

DSME_HANDLER(DSM_MSGTYPE_BLOCK_SHUTDOWN, client, msg)
{
    /* Note: The single shutdown_blockd control point here in
     *       state module is multiplexed in dbusproxy module
     *       by keeping track of active blocking dbus clients.
     *
     *       Clients utilizing dsmesock ipc must not be allowed
     *       to interfere with this -> ignore external messages
     *       (it is assumed that only dbusproxy sends these
     *       messages from within dsme).
     */
    dsme_log(LOG_DEBUG, PFIX "BLOCK_SHUTDOWN");
    if( endpoint_is_dsme(client) )
        update_shutdown_blocked(true);
}

DSME_HANDLER(DSM_MSGTYPE_ALLOW_SHUTDOWN, client, msg)
{
    dsme_log(LOG_DEBUG, PFIX "ALLOW_SHUTDOWN");
    if( endpoint_is_dsme(client) )
        update_shutdown_blocked(false);
}

module_fn_info_t message_handlers[] = {
      DSME_HANDLER_BINDING(DSM_MSGTYPE_STATE_QUERY),
      DSME_HANDLER_BINDING(DSM_MSGTYPE_TELINIT),
      DSME_HANDLER_BINDING(DSM_MSGTYPE_SHUTDOWN_REQ),
      DSME_HANDLER_BINDING(DSM_MSGTYPE_POWERUP_REQ),
      DSME_HANDLER_BINDING(DSM_MSGTYPE_REBOOT_REQ),
      DSME_HANDLER_BINDING(DSM_MSGTYPE_SET_ALARM_STATE),
      DSME_HANDLER_BINDING(DSM_MSGTYPE_SET_USB_STATE),
      DSME_HANDLER_BINDING(DSM_MSGTYPE_SET_CHARGER_STATE),
      DSME_HANDLER_BINDING(DSM_MSGTYPE_SET_THERMAL_STATUS),
      DSME_HANDLER_BINDING(DSM_MSGTYPE_SET_EMERGENCY_CALL_STATE),
      DSME_HANDLER_BINDING(DSM_MSGTYPE_SET_BATTERY_STATE),
      DSME_HANDLER_BINDING(DSM_MSGTYPE_SET_BATTERY_LEVEL),
      DSME_HANDLER_BINDING(DSM_MSGTYPE_DBUS_CONNECTED),
      DSME_HANDLER_BINDING(DSM_MSGTYPE_DBUS_DISCONNECT),
      DSME_HANDLER_BINDING(DSM_MSGTYPE_BLOCK_SHUTDOWN),
      DSME_HANDLER_BINDING(DSM_MSGTYPE_ALLOW_SHUTDOWN),
      {0}
};

static void parse_malf_info(char*  malf_info,
                            char** reason,
                            char** component,
                            char** details)
{
    char* p = malf_info;
    char* r = 0;
    char* c = 0;
    char* d = 0;
    char* save;

    if (p) {
        if ((r = strtok_r(p, " ", &save))) {
            if ((c = strtok_r(0, " ", &save))) {
                d = strtok_r(0, "", &save);
            }
        }
    }

    if (reason) {
        *reason = r;
    }
    if (component) {
        *component = c;
    }
    if (details) {
        *details = d;
    }
}

static void enter_malf(const char* reason,
                       const char* component,
                       const char* details)
{
  char* malf_details   = details ? strdup(details) : 0;
  DSM_MSGTYPE_ENTER_MALF malf = DSME_MSG_INIT(DSM_MSGTYPE_ENTER_MALF);
  malf.reason          = strcmp(reason, "HARDWARE") ? DSME_MALF_SOFTWARE
                                                    : DSME_MALF_HARDWARE;
  malf.component       = strdup(component);

  if (malf_details) {
      modules_broadcast_internally_with_extra(&malf,
                                              strlen(malf_details) + 1,
                                              malf_details);
  } else {
      modules_broadcast_internally(&malf);
  }
}

/*
 * If string 'string' begins with prefix 'prefix',
 * DSME_SKIP_PREFIX returns a pointer to the first character
 * in the string after the prefix. If the character is a space,
 * it is skipped.
 * If the string does not begin with the prefix, 0 is returned.
 */
#define DSME_SKIP_PREFIX(string, prefix) \
    (strncmp(string, prefix, sizeof(prefix) - 1) \
     ? 0 \
     : string + sizeof(prefix) - (*(string + sizeof(prefix) - 1) != ' '))

static void set_initial_state_bits(const char* bootstate)
{
  const char* p         = 0;
  bool        must_malf = false;

  if (strcmp(bootstate, "SHUTDOWN") == 0) {
      /*
       * DSME_STATE_SHUTDOWN:
       * charger must be considered disconnected;
       * otherwise we end up in actdead
       */
      // TODO: does getbootstate ever return "SHUTDOWN"?
      set_charger_state(CHARGER_DISCONNECTED);
      set_shutdown_requested(true);
  } else if ((p = DSME_SKIP_PREFIX(bootstate, "USER"))) {
      // DSME_STATE_USER with possible malf information
  } else if ((p = DSME_SKIP_PREFIX(bootstate, "ACT_DEAD"))) {
      // DSME_STATE_ACTDEAD with possible malf information
      set_shutdown_requested(true);
  } else if (strcmp(bootstate, "BOOT") == 0) {
      // DSME_STATE_REBOOT
      // TODO: does getbootstate ever return "BOOT"?
      set_reboot_requested(true);
  } else if (strcmp(bootstate, "LOCAL") == 0 ||
             strcmp(bootstate, "TEST")  == 0 ||
             strcmp(bootstate, "FLASH") == 0)
  {
      // DSME_STATE_TEST
      set_testmode_requested(true);
  } else if ((p = DSME_SKIP_PREFIX(bootstate, "MALF"))) {
      // DSME_STATE_USER with malf information
      must_malf = true;
      if (!*p) {
          // there was no malf information, so supply our own
          p = "SOFTWARE bootloader";
      }
  } else {
      // DSME_STATE_USER with malf information
      p = "SOFTWARE bootloader unknown bootreason to dsme";
  }

  if (p && *p) {
      // we got a bootstate followed by malf information

      // If allowed to malf, enter malf
      if (must_malf || !rd_mode_enabled()) {
          char* reason    = 0;
          char* component = 0;
          char* details   = 0;

          char* malf_info = strdup(p);
          parse_malf_info(malf_info, &reason, &component, &details);
          enter_malf(reason, component, details);
          free(malf_info);
      } else {
          dsme_log(LOG_NOTICE, PFIX "R&D mode enabled, not entering MALF '%s'", p);
      }
  }
}

void module_init(module_t* handle)
{
  /* Do not connect to D-Bus; it is probably not started yet.
   * Instead, wait for DSM_MSGTYPE_DBUS_CONNECTED.
   */

  dsme_log(LOG_DEBUG, PFIX "state.so started");

  const char* bootstate = getenv("BOOTSTATE");
  if (!bootstate) {
      bootstate = "USER";
      dsme_log(LOG_NOTICE, PFIX "BOOTSTATE: No such environment variable, using '%s'",
               bootstate);
  } else {
      dsme_log(LOG_INFO, PFIX "BOOTSTATE: '%s'", bootstate);
  }

  set_initial_state_bits(bootstate);
  change_state_if_necessary();

  dsme_log(LOG_DEBUG, PFIX "Startup state: %s", state_name(current_state));
}

void module_fini(void)
{
  dsme_dbus_unbind_signals(&dbus_signals_bound, dbus_signals_array);
#ifdef DSME_VIBRA_FEEDBACK
  dsme_fini_vibrafeedback();
#endif  // DSME_VIBRA_FEEDBACK
  stop_delayed_runlevel_timers();
  stop_charger_disconnect_timer();
  stop_overheat_timer();
  stop_battery_empty_timer();
  dsme_log(LOG_DEBUG, PFIX "state.so unloaded");
}

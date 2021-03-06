/**
   @file timers.h

   Defines structures and function prototypes for using DSME timers.
   <p>
   Copyright (C) 2004-2010 Nokia Corporation.
   Copyright (C) 2017 Jolla Ltd.

   @author Ari Saastamoinen
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

#ifndef DSME_TIMERS_H
#define DSME_TIMERS_H

#ifdef __cplusplus
extern "C" {
#endif

typedef int (*dsme_timer_callback_t)(void* data);

typedef unsigned dsme_timer_t;

/**
   Creates a new DSME timer with second resolution

   @param seconds  Timer expiry in seconds from current time
   @param callback Function to be called when the timer expires
   @param data     Passed to callback function as an argument.
   @return !0 on success; 0 on failure
*/
dsme_timer_t dsme_create_timer_seconds(unsigned               seconds,
                                       dsme_timer_callback_t  callback,
                                       void*                  data);

/** Creates a new DSME timer with millisecond resolution

   @param seconds  Timer expiry in milliseconds from current time
   @param callback Function to be called when the timer expires
   @param data     Passed to callback function as an argument.
   @return !0 on success; 0 on failure
*/
dsme_timer_t dsme_create_timer(unsigned              milliseconds,
                               dsme_timer_callback_t callback,
                               void*                 data);

/**
   Deactivates and destroys an existing timer.

   @param timer Timer to be destroyed.
*/
void dsme_destroy_timer(dsme_timer_t timer);

#ifdef __cplusplus
}
#endif

#endif

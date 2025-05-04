/**
 * @file utility.h
 *
 * Generic functions needed by dsme core and/or multiple plugings.
 *
 * <p>
 * Copyright (c) 2019 - 2020 Jolla Ltd.
 * Copyright (c) 2020 Open Mobile Platform LLC.
 *
 * @author Simo Piiroinen <simo.piiroinen@jollamobile.com>
 *
 * This file is part of Dsme.
 *
 * Dsme is free software; you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License
 * version 2.1 as published by the Free Software Foundation.
 *
 * Dsme is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with Dsme.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "utility.h"

#include "../include/dsme/logging.h"

#include <string.h>
#include <stdio.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>
#include <pwd.h>
#ifdef DSME_USEWHEEL
#include <grp.h>
#endif
#include <libcryptsetup.h>

/* ========================================================================= *
 * Prototypes
 * ========================================================================= */

/* ------------------------------------------------------------------------- *
 * UTILITY
 * ------------------------------------------------------------------------- */

bool                        dsme_user_is_privileged       (uid_t uid, gid_t gid);
bool                        dsme_process_is_privileged    (pid_t pid);
static void                 dsme_free_crypt_device        (struct crypt_device *cdev);
static struct crypt_device *dsme_get_crypt_device_for_home(void);
bool                        dsme_home_is_encrypted        (void);
const char                 *dsme_state_repr               (dsme_state_t state);
static char                *dsme_pid2exe                  (pid_t pid);

/* ========================================================================= *
 * Client identification
 * ========================================================================= */

bool
dsme_user_is_privileged(uid_t uid, gid_t gid)
{
    bool is_privileged = false;

#ifdef DSME_USEWHEEL
    struct passwd* pw = getpwuid(uid);
    if( pw ) {
        int ngroups = 0;
        getgrouplist(pw->pw_name, pw->pw_gid, NULL, &ngroups);
        gid_t groups[ngroups];
        getgrouplist(pw->pw_name, pw->pw_gid, groups, &ngroups);

        for (int i = 0; i < ngroups; i++) {
            struct group* gr = getgrgid(groups[i]);
            if( gr != NULL ) {
                if(strcmp(gr->gr_name,"wheel") == 0 ) {
                   is_privileged = true;
                   break;
               }
            }
        }
    }
#endif
    /* Check if UID/GID is root/privileged */
    if( uid != 0 && gid != 0 ) {
        struct passwd *pw = getpwnam("privileged");
        if( !pw ) {
            dsme_log(LOG_WARNING, "privileged user not found");
            goto EXIT;
        }
        if( uid != pw->pw_uid && gid != pw->pw_gid )
            goto EXIT;
    }

    is_privileged = true;

EXIT:
    return is_privileged;
}

bool
dsme_process_is_privileged(pid_t pid)
{
    bool is_privileged = false;

    /* /proc/PID directory is owned by process EUID:EGID */
    char temp[256];
    snprintf(temp, sizeof temp, "/proc/%d", (int)pid);
    struct stat st = {};

    if( stat(temp, &st) == -1 )
        dsme_log(LOG_WARNING, "could not stat %s: %m", temp);
    else
        is_privileged = dsme_user_is_privileged(st.st_uid, st.st_gid);

    return is_privileged;
}

/* ========================================================================= *
 * Probing for encrypted home partition
 * ========================================================================= */

static const char homeLuksContainer[] = "/dev/sailfish/home";

static void
dsme_free_crypt_device(struct crypt_device *cdev)
{
    if( cdev )
        crypt_free(cdev);
}

static struct crypt_device *
dsme_get_crypt_device_for_home(void)
{
    struct crypt_device *cdev = 0;
    struct crypt_device *work = 0;

    int rc;

    if( (rc = crypt_init(&work, homeLuksContainer)) < 0 ) {
        dsme_log(LOG_WARNING, "%s: could not initialize crypt device: %s",
                 homeLuksContainer, strerror(-rc));
        goto EXIT;
    }

    if( (rc = crypt_load(work, 0, 0)) < 0 ) {
        dsme_log(LOG_WARNING, "%s: could not load crypt device info: %s",
                 homeLuksContainer, strerror(-rc));
        goto EXIT;
    }

    cdev = work, work = 0;

EXIT:
    dsme_free_crypt_device(work);

    return cdev;
}

bool
dsme_home_is_encrypted(void)
{
    static bool is_encrypted = false;
    static bool was_probed = false;

    if( !was_probed ) {
        was_probed = true;

        struct crypt_device *cdev = dsme_get_crypt_device_for_home();
        is_encrypted = (cdev != 0);
        dsme_free_crypt_device(cdev);

        dsme_log(LOG_WARNING, "HOME is encrypted: %s",
                 is_encrypted ? "True" : "False");
    }

    return is_encrypted;
}

/* ========================================================================= *
 * Debug helpers
 * ========================================================================= */

/** Human readable dsme_state_t represenation for debugging purposes
 *
 * @param state State enumeration value
 *
 * @return human readable representation of the enumeration value
 */
const char *
dsme_state_repr(dsme_state_t state)
{
    const char *repr = "UNKNOWN";

    switch( state ) {
    case DSME_STATE_SHUTDOWN:   repr = "SHUTDOWN"; break;
    case DSME_STATE_USER:       repr = "USER";     break;
    case DSME_STATE_ACTDEAD:    repr = "ACTDEAD";  break;
    case DSME_STATE_REBOOT:     repr = "REBOOT";   break;
    case DSME_STATE_BOOT:       repr = "BOOT";     break;
    case DSME_STATE_NOT_SET:    repr = "NOT_SET";  break;
    case DSME_STATE_TEST:       repr = "TEST";     break;
    case DSME_STATE_MALF:       repr = "MALF";     break;
    case DSME_STATE_LOCAL:      repr = "LOCAL";    break;
    default: break;
    }

    return repr;
}

/** Map process identifier to process name
 *
 * @param pid process identifier
 *
 * @return process name
 */
static char *dsme_pid2exe(pid_t pid)
{
    char *res = 0;
    int   fd  = -1;
    int   rc;
    char  path[128];
    char  temp[128];

    snprintf(path, sizeof path, "/proc/%ld/cmdline", (long)pid);

    if( (fd = open(path, O_RDONLY)) == -1 )
        goto EXIT;

    if( (rc = read(fd, temp, sizeof temp - 1)) <= 0 )
        goto EXIT;

    temp[rc] = 0;
    res = strdup(temp);

EXIT:
    if( fd != -1 ) close(fd);

    return res;
}

/** Map process identifier to IPC process identifier
 *
 * @param pid peer process or 0 for dsme itself
 *
 * @return string containing both pid and process name
 */
char *dsme_pid2text(pid_t pid)
{
    static unsigned id = 0;

    char *str = 0;
    char *exe = 0;

    if( pid == 0 ) {
        str = strdup("<internal>");
        goto EXIT;
    }

    exe = dsme_pid2exe(pid);

    if( asprintf(&str, "external-%u/%ld (%s)", ++id,
                 (long)pid, exe ?: "unknown") < 0 )
        str = 0;

EXIT:
    free(exe);

    return str ?: strdup("error");
}

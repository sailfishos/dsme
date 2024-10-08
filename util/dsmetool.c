/**
 * @file dsmetool.c
 *
 * Dsmetool can be used to send commands to DSME.
 * <p>
 * Copyright (c) 2004 - 2011 Nokia Corporation.
 * Copyright (c) 2013 - 2020 Jolla Ltd.
 * Copyright (c) 2020 Open Mobile Platform LLC.
 *
 * @author Ismo Laitinen <ismo.laitinen@nokia.com>
 * @author Semi Malinen <semi.malinen@nokia.com>
 * @author Matias Muhonen <ext-matias.muhonen@nokia.com>
 * @author Jarkko Nikula <jarkko.nikula@jollamobile.com>
 * @author Pekka Lundstrom <pekka.lundstrom@jollamobile.com>
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

#include "../modules/dbusproxy.h"
#include "../modules/state-internal.h"
#include "../include/dsme/logging.h"

#include <dsme/state.h>
#include <dsme/protocol.h>
#include <dsme/dsme_dbus_if.h>

#include <linux/rtc.h>

#include <sys/types.h>
#include <sys/ioctl.h>

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <poll.h>
#include <time.h>
#include <fcntl.h>
#include <getopt.h>
#include <errno.h>

#include <dbus/dbus.h>

#define STRINGIFY(x)  STRINGIFY2(x)
#define STRINGIFY2(x) #x

/* ========================================================================= *
 * DIAGNOSTIC_OUTPUT
 * ========================================================================= */

static bool log_verbose = false;

#define log_error(FMT,ARGS...)\
     fprintf(stderr, "E: "FMT"\n", ## ARGS)

#define log_debug(FMT,ARGS...)\
     do {\
         if( log_verbose )\
             fprintf(stderr, "D: "FMT"\n", ## ARGS);\
     }while(0)

/* ========================================================================= *
 * PROTOTYPES
 * ========================================================================= */

/* ------------------------------------------------------------------------- *
 * MISC_UTILS
 * ------------------------------------------------------------------------- */

static const char        *dsme_state_repr(dsme_state_t state);
static int64_t            boottime_get_ms(void);

/* ------------------------------------------------------------------------- *
 * DSMEIPC_CONNECTION
 * ------------------------------------------------------------------------- */

#define DSMEIPC_WAIT_DEFAULT -1

static void               dsmeipc_connect(void);
static void               dsmeipc_disconnect(void);
static void               dsmeipc_send_full(const void *msg, const void *data, size_t size);
static void               dsmeipc_send(const void *msg);
static void               dsmeipc_send_with_string(const void *msg, const char *str);
static bool               dsmeipc_wait(int64_t *tmo);
static dsmemsg_generic_t *dsmeipc_read(void);

/* ------------------------------------------------------------------------- *
 * DSME_OPTIONS
 * ------------------------------------------------------------------------- */

static void               xdsme_query_version(bool testmode);
static void               xdsme_query_runlevel(void);
static void               xdsme_request_dbus_connect(void);
static void               xdsme_request_dbus_disconnect(void);
static void               xdsme_request_reboot(void);
static void               xdsme_request_shutdown(void);
static void               xdsme_request_powerup(void);
static void               xdsme_request_runlevel(const char *runlevel);
static void               xdsme_request_loglevel(unsigned level);
static void               xdsme_request_log_include(const char *pattern);
static void               xdsme_request_log_exclude(const char *pattern);
static void               xdsme_request_log_defaults(void);

/* ------------------------------------------------------------------------- *
 * RTC_OPTIONS
 * ------------------------------------------------------------------------- */

static bool               rtc_clear_alarm(void);

/* ------------------------------------------------------------------------- *
 * OPTION_PARSING
 * ------------------------------------------------------------------------- */

static unsigned           parse_unsigned(char *str);
static unsigned           parse_loglevel(char *str);
static const char        *parse_runlevel(char *str);

static void               output_usage(const char *name);

/* ------------------------------------------------------------------------- *
 * MAIN_ENTRY_POINT
 * ------------------------------------------------------------------------- */
int main(int argc, char *argv[]);

/* ========================================================================= *
 * MISC_UTILS
 * ========================================================================= */

static int64_t boottime_get_ms(void)
{
        int64_t res = 0;

        struct timespec ts;

        if( clock_gettime(CLOCK_BOOTTIME, &ts) == 0 ) {
                res = ts.tv_sec;
                res *= 1000;
                res += ts.tv_nsec / 1000000;
        }

        return res;
}

static const char *dsme_state_repr(dsme_state_t state)
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

/* ========================================================================= *
 * DSMEIPC_CONNECTION
 * ========================================================================= */

static dsmesock_connection_t *dsmeipc_conn = 0;

static void dsmeipc_connect(void)
{
    /* Already connected? */
    if( dsmeipc_conn )
        goto EXIT;

    if( !(dsmeipc_conn = dsmesock_connect()) ) {
        log_error("dsmesock_connect: %m");
        exit(EXIT_FAILURE);
    }

    log_debug("connected");

EXIT:
    return;
}

static void dsmeipc_disconnect(void)
{
    if( !dsmeipc_conn )
        goto EXIT;

    /* This gives enough time for DSME to check
     * the socket permissions before we close the socket
     * connection */
    (void)xdsme_query_version(true);

    log_debug("disconnecting");
    dsmesock_close(dsmeipc_conn), dsmeipc_conn = 0;

EXIT:
    return;
}

static void dsmeipc_send_full(const void *msg_, const void *data, size_t size)
{
    const dsmemsg_generic_t *msg = msg_;

    dsmeipc_connect();

    log_debug("send: %s", dsmemsg_id_name(msg->type_));

    if( dsmesock_send_with_extra(dsmeipc_conn, msg, size, data) == -1 ) {
        log_error("dsmesock_send: %m");
        exit(EXIT_FAILURE);
    }
}

static void dsmeipc_send(const void *msg)
{
    dsmeipc_send_full(msg, 0, 0);
}

static void dsmeipc_send_with_string(const void *msg, const char *str)
{
    dsmeipc_send_full(msg, str, strlen(str) + 1);
}

static bool dsmeipc_wait(int64_t *tmo)
{
    bool have_input = false;
    int  wait_input = 0;

    struct pollfd pfd =
    {
        .fd = dsmeipc_conn->fd,
        .events = POLLIN,
    };

    int64_t now = boottime_get_ms();

    /* Called with uninitialized timeout; use now + 5 seconds */
    if( *tmo == DSMEIPC_WAIT_DEFAULT )
        *tmo = now + 5000;

    /* If timeout is in the future, wait for input - otherwise
     * just check if there already is something to read */
    if( *tmo > now )
        wait_input = (int)(now - *tmo);

    if( poll(&pfd, 1, wait_input) == 1 )
        have_input = true;

    return have_input;
}

static dsmemsg_generic_t *dsmeipc_read(void)
{
    dsmemsg_generic_t *msg = dsmesock_receive(dsmeipc_conn);
    if( !msg ) {
        log_error("dsmesock_receive: %m");
        exit(EXIT_FAILURE);
    }

    log_debug("recv: %s", dsmemsg_id_name(msg->type_));

    return msg;
}

/* ========================================================================= *
 * DBUSIPC
 * ========================================================================= */

static DBusConnection *dbusipc_connection(void)
{
    static DBusConnection *conn = NULL;
    if( !conn ) {
        DBusError err = DBUS_ERROR_INIT;
        if( !(conn = dbus_bus_get(DBUS_BUS_SYSTEM, &err)) ) {
            log_error("SystemBus connect failed: %s: %s",
                      err.name, err.message);
            exit(EXIT_FAILURE);
        }
        dbus_error_free(&err);
    }
    return conn;
}

static void dbusipc_simple_request_bool_arg(const char *method, bool arg)
{
    DBusError    err = DBUS_ERROR_INIT;
    DBusMessage *req = NULL;
    DBusMessage *rsp = NULL;
    dbus_bool_t  dta = arg;

    req = dbus_message_new_method_call(dsme_service, dsme_req_path,
                                       dsme_req_interface, method);
    if( !req )
        goto EXIT;

    if( !dbus_message_append_args(req, DBUS_TYPE_BOOLEAN, &dta, DBUS_TYPE_INVALID) )
        goto EXIT;

    rsp = dbus_connection_send_with_reply_and_block(dbusipc_connection(),
                                                    req, -1, &err);
    if( !rsp ) {
        log_error("%s.%s() failed: %s: %s", dsme_req_interface,
                  method, err.name, err.message);
        goto EXIT;
    }

EXIT:
    if( rsp )
        dbus_message_unref(rsp);
    if( req )
        dbus_message_unref(req);
    dbus_error_free(&err);
}

/* ========================================================================= *
 * DSME_OPTIONS
 * ========================================================================= */

static void xdsme_query_version(bool testmode)
{
    DSM_MSGTYPE_GET_VERSION req =
          DSME_MSG_INIT(DSM_MSGTYPE_GET_VERSION);

    int64_t timeout = DSMEIPC_WAIT_DEFAULT;
    char   *version = 0;

    dsmeipc_send(&req);

    while( dsmeipc_wait(&timeout) ) {
        dsmemsg_generic_t *msg = dsmeipc_read();

        DSM_MSGTYPE_DSME_VERSION *rsp =
            DSMEMSG_CAST(DSM_MSGTYPE_DSME_VERSION, msg);

        if( rsp ) {
            const char *data = DSMEMSG_EXTRA(rsp);
            size_t      size = DSMEMSG_EXTRA_SIZE(rsp);
            version = strndup(data, size);
        }

        free(msg);

        if( rsp )
            break;
    }

    if( !testmode ) {
        printf("dsmetool version: %s\n", STRINGIFY(PRG_VERSION));
        printf("DSME version: %s\n", version ?: "unknown");
    }

    free(version);
}

static void xdsme_query_runlevel(void)
{
    DSM_MSGTYPE_STATE_QUERY req = DSME_MSG_INIT(DSM_MSGTYPE_STATE_QUERY);

    int64_t      timeout = DSMEIPC_WAIT_DEFAULT;
    dsme_state_t state   = DSME_STATE_NOT_SET;

    dsmeipc_send(&req);

    while( dsmeipc_wait(&timeout) ) {
        dsmemsg_generic_t *msg = dsmeipc_read();
        DSM_MSGTYPE_STATE_CHANGE_IND *rsp =
            DSMEMSG_CAST(DSM_MSGTYPE_STATE_CHANGE_IND, msg);

        if( rsp )
            state = rsp->state;

        free(msg);

        if( rsp )
            break;
    }

    printf("%s\n", dsme_state_repr(state));
}

static void xdsme_request_dbus_connect(void)
{
    DSM_MSGTYPE_DBUS_CONNECT req = DSME_MSG_INIT(DSM_MSGTYPE_DBUS_CONNECT);

    dsmeipc_send(&req);
}

static void xdsme_request_dbus_disconnect(void)
{
    DSM_MSGTYPE_DBUS_DISCONNECT req =
        DSME_MSG_INIT(DSM_MSGTYPE_DBUS_DISCONNECT);

    dsmeipc_send(&req);
}

static void xdsme_request_reboot(void)
{
    DSM_MSGTYPE_REBOOT_REQ req = DSME_MSG_INIT(DSM_MSGTYPE_REBOOT_REQ);

    dsmeipc_send(&req);
}

static void xdsme_request_shutdown(void)
{
    DSM_MSGTYPE_SHUTDOWN_REQ req = DSME_MSG_INIT(DSM_MSGTYPE_SHUTDOWN_REQ);

    dsmeipc_send(&req);
}

static void xdsme_request_powerup(void)
{
    DSM_MSGTYPE_POWERUP_REQ req = DSME_MSG_INIT(DSM_MSGTYPE_POWERUP_REQ);

    dsmeipc_send(&req);
}

static void xdsme_request_runlevel(const char *runlevel)
{
    DSM_MSGTYPE_TELINIT req = DSME_MSG_INIT(DSM_MSGTYPE_TELINIT);

    dsmeipc_send_with_string(&req, runlevel);
}

static void xdsme_request_loglevel(unsigned level)
{
    DSM_MSGTYPE_SET_LOGGING_VERBOSITY req =
        DSME_MSG_INIT(DSM_MSGTYPE_SET_LOGGING_VERBOSITY);
    req.verbosity = level;

    dsmeipc_send(&req);
}

static void xdsme_request_log_include(const char *pattern)
{
    DSM_MSGTYPE_ADD_LOGGING_INCLUDE req = DSME_MSG_INIT(DSM_MSGTYPE_ADD_LOGGING_INCLUDE);

    dsmeipc_send_with_string(&req, pattern);
}

static void xdsme_request_log_exclude(const char *pattern)
{
    DSM_MSGTYPE_ADD_LOGGING_EXCLUDE req = DSME_MSG_INIT(DSM_MSGTYPE_ADD_LOGGING_EXCLUDE);

    dsmeipc_send_with_string(&req, pattern);
}

static void xdsme_request_log_defaults(void)
{
    DSM_MSGTYPE_USE_LOGGING_DEFAULTS req = DSME_MSG_INIT(DSM_MSGTYPE_USE_LOGGING_DEFAULTS);

    dsmeipc_send(&req);
}

static void xdsme_block_shutdown(void)
{
    dbusipc_simple_request_bool_arg(dsme_inhibit_shutdown, true);
}

static void xdsme_allow_shutdown(void)
{
    dbusipc_simple_request_bool_arg(dsme_inhibit_shutdown, false);
}

static void xdsme_block(const char *arg)
{
    /* Default to: sleep "forever" / in practice 1 year */
    struct timespec ts = { .tv_sec  = 365 * 24 * 60 * 60, };

    if( arg ) {
        int64_t ms = (int64_t)(strtod(arg, NULL) * 1000);
        if( ms > 0 ) {
            ts.tv_sec  = (time_t)(ms / 1000);
            ts.tv_nsec = (long)((ms % 1000) * 1000000);
        }
    }

    while( nanosleep(&ts, &ts) == -1 && errno == EINTR ) { }
}

/* ========================================================================= *
 * RTC_OPTIONS
 * ========================================================================= */

/** Clear possible RTC alarm wakeup */
static bool rtc_clear_alarm(void)
{
    static const char rtc_path[] = "/dev/rtc0";

    bool cleared = false;
    int  rtc_fd  = -1;

    struct rtc_wkalrm alrm;

    if ((rtc_fd = open(rtc_path, O_RDONLY)) == -1) {
        /* TODO: If open fails reason is most likely that dsme is running
         * and has opened rtc. In that case we should send message to dsme
         * and ask it to do the clearing. This functionality is not now
         * needed because rtc alarms are cleared only during preinit and
         * there dsme is not running. But to make this complete, that
         * functionality should be added.
         */
        log_error("Failed to open %s: %m", rtc_path);
        goto EXIT;
    }

    memset(&alrm, 0, sizeof(alrm));
    if (ioctl(rtc_fd, RTC_WKALM_RD, &alrm) == -1) {
        log_error("Failed to read rtc alarms %s: %s: %m", rtc_path,
                  "RTC_WKALM_RD");
        goto EXIT;
    }
    printf("Alarm was %s at %d.%d.%d %02d:%02d:%02d UTC\n",
           alrm.enabled ? "Enabled" : "Disabled",
           1900+alrm.time.tm_year, 1+alrm.time.tm_mon, alrm.time.tm_mday,
           alrm.time.tm_hour, alrm.time.tm_min, alrm.time.tm_sec);

    /* Kernel side bug in Jolla phone?
     * We need to enable alarm first before we can disable it.
     */
    alrm.enabled = 1;
    alrm.pending = 0;
    if (ioctl(rtc_fd, RTC_WKALM_SET, &alrm) == -1)
        log_error("Failed to enable rtc alarms %s: %s: %m", rtc_path,
                  "RTC_WKALM_SET");

    /* Now disable the alarm */
    alrm.enabled = 0;
    alrm.pending = 0;
    if (ioctl(rtc_fd, RTC_WKALM_SET, &alrm) == -1) {
        log_error("Failed to clear rtc alarms %s: %s: %m", rtc_path,
                  "RTC_WKALM_SET");
        goto EXIT;
    }

    printf("RTC alarm cleared ok\n");
    cleared = true;

EXIT:
    if( rtc_fd != -1 )
        close(rtc_fd);

    return cleared;
}

/* ========================================================================= *
 * OPTION_PARSING
 * ========================================================================= */

static unsigned parse_unsigned(char *str)
{
    char     *pos = str;
    unsigned  val = strtoul(str, &pos, 0);

    if( pos == str || *pos != 0 ) {
        log_error("%s: not a valid unsigned integer", str);
        exit(EXIT_FAILURE);
    }

    return val;
}

static unsigned parse_loglevel(char *str)
{
    unsigned val = parse_unsigned(str);

    if( val > 7 ) {
        log_error("%s: not a valid log level", str);
        exit(EXIT_FAILURE);
    }

    return val;
}

static const char *parse_runlevel(char *str)
{
    static const char * const lut[] =
    {
        "SHUTDOWN", "USER", "ACTDEAD", "REBOOT", 0
    };

    for( size_t i = 0;  ; ++i ) {
        if( lut[i] == 0 ) {
            log_error("%s: not a valid run level", str);
            exit(EXIT_FAILURE);
        }

        if( !strcasecmp(lut[i], str) )
            return lut[i];
    }
}

static void output_usage(const char *name)
{
    printf("USAGE: %s <options>\n", name);
    printf(
"\n"
"  -h --help                       Print usage information\n"
"  -v --version                    Print the versions of DSME and dsmetool\n"
"  -V --verbose                    Make dsmetool more verbose\n"
"  -l --loglevel <0..7>            Change DSME's logging verbosity\n"
"  -i --log-include <file:func>    Include logging from matching functions\n"
"  -e --log-exclude <file:func>    Exclude logging from matching functions\n"
"  -L --log-defaults               Clear include/exclude patterns\n"
"\n"
"  -g --get-state                  Print device state, i.e. one of\n"
"                                   SHUTDOWN USER ACTDEAD REBOOT BOOT\n"
"                                   TEST MALF LOCAL NOT_SET or UNKNOWN\n"
"  -b --reboot                     Reboot the device\n"
"  -o --shutdown                   Shutdown (or switch to ACTDEAD)\n"
"  -u --powerup                    Switch from ACTDEAD to USER state\n"
"  -t --telinit <runlevel name>    Change runlevel, valid names are:\n"
"                                   SHUTDOWN USER ACTDEAD REBOOT\n"
"\n"
"  -c --clear-rtc                  Clear RTC alarms\n"
"\n"
"  -d --start-dbus                 Start DSME's D-Bus services\n"
"  -s --stop-dbus                  Stop DSME's D-Bus services\n"
"\n"
"  -B --block[=<seconds>]          Sleep for specified time / forever\n"
"                                  Useful for pacing option handling or\n"
"                                  keeping D-Bus connection alive after\n"
"                                  other options have been handled.\n"
"     --block-shutdown             Start shutdown blocking\n"
"     --allow-shutdown             Stop shutdown blocking\n"
"\n"
          );
}

/* ========================================================================= *
 * MAIN_ENTRY_POINT
 * ========================================================================= */

int main(int argc, char *argv[])
{
    const char *program_name  = argv[0];
    int         retval        = EXIT_FAILURE;
    const char *short_options = "hdsbvact:l:guoVi:e:LB::";
    const struct option long_options[] = {
        {"help",           no_argument,       NULL, 'h'},
        {"start-dbus",     no_argument,       NULL, 'd'},
        {"stop-dbus",      no_argument,       NULL, 's'},
        {"reboot",         no_argument,       NULL, 'b'},
        {"version",        no_argument,       NULL, 'v'},
        {"clear-rtc",      no_argument,       NULL, 'c'},
        {"get-state",      no_argument,       NULL, 'g'},
        {"powerup",        no_argument,       NULL, 'u'},
        {"shutdown",       no_argument,       NULL, 'o'},
        {"telinit",        required_argument, NULL, 't'},
        {"loglevel",       required_argument, NULL, 'l'},
        {"log-include",    required_argument, NULL, 'i'},
        {"log-exclude",    required_argument, NULL, 'e'},
        {"log-defaults",   no_argument,       NULL, 'L'},
        {"verbose",        no_argument,       NULL, 'V'},
        {"block",          optional_argument, NULL, 'B'},
        {"block-shutdown", no_argument,       NULL, 900},
        {"allow-shutdown", no_argument,       NULL, 901},
        {0, 0, 0, 0}
    };

    /* Treat no args as if --help option were given */
    if( argc == 1 ) {
        output_usage(program_name);
        goto DONE;
    }

    /* Handle options */
    for( ;; ) {
        int opt = getopt_long(argc, argv, short_options, long_options, 0);

        if( opt == -1 )
            break;

        switch( opt ) {
        case 'd':
            xdsme_request_dbus_connect();
            break;

        case 's':
            xdsme_request_dbus_disconnect();
            break;

        case 'b':
            xdsme_request_reboot();
            break;

        case 'u':
            xdsme_request_powerup();
            break;

        case 'o':
            xdsme_request_shutdown();
            break;

        case 'v':
            xdsme_query_version(false);
            break;

        case 't':
            xdsme_request_runlevel(parse_runlevel(optarg));
            break;

        case 'g':
            xdsme_query_runlevel();
            break;

        case 'l':
            xdsme_request_loglevel(parse_loglevel(optarg));
            break;

        case 'i':
            xdsme_request_log_include(optarg);
            break;

        case 'e':
            xdsme_request_log_exclude(optarg);
            break;

        case 'L':
            xdsme_request_log_defaults();
            break;

        case 'c':
            if( !rtc_clear_alarm() )
                goto EXIT;
            break;

        case 'V':
            log_verbose = true;
            break;

        case 'h':
            output_usage(program_name);
            goto DONE;

        case 900:
            xdsme_block_shutdown();
            break;

        case 901:
            xdsme_allow_shutdown();
            break;

        case 'B':
            xdsme_block(optarg);
            break;

        case '?':
            fprintf(stderr, "(use --help for instructions)\n");
            goto EXIT;
        }
    }

    /* Complain about excess args */
    if( optind < argc ) {
        fprintf(stderr, "%s: unknown argument\n", argv[optind]);
        fprintf(stderr, "(use --help for instructions)\n");
        goto EXIT;
    }

DONE:
    retval = EXIT_SUCCESS;

EXIT:
    dsmeipc_disconnect();

    return retval;
}

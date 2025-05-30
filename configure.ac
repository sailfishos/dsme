# Package name and version
AC_INIT(dsme, 0.84.7)

AM_INIT_AUTOMAKE([foreign subdir-objects])
AM_EXTRA_RECURSIVE_TARGETS([dbus-gmain])

AC_PREREQ([2.61])
AC_CONFIG_SRCDIR([dsme/dsme-wdd-wd.c])
#AC_CONFIG_HEADERS([config.h])

#
# CONFIG OPTIONS
#

#
# powerontimer
#
AC_ARG_ENABLE([poweron-timer],
  [AS_HELP_STRING([--disable-poweron-timer],
    [disable power-on timer plugin (libpowerontimer)])])

AS_IF([test "x$enable_poweron_timer" != xno],
  [AC_DEFINE([DSME_POWERON_TIMER], [1], [Enable power-on timer plugin])
   AC_CHECK_LIB([cal], [cal_init], [],
     [AC_MSG_FAILURE([libcal not found (needed by poweron-timer plugin)])])
  ])

AM_CONDITIONAL([WANT_POWERON_TIMER], [test x$enable_poweron_timer != xno])

#
# runlevel
#
AC_ARG_ENABLE([runlevel],
  [AS_HELP_STRING([--enable-runlevel],
    [enable runlevel plugin (librunlevel) - for systems with SysV init])],
  [],
  [enable_runlevel=no])

AS_IF([test "x$enable_runlevel" != xno],
  [AC_DEFINE([DSME_WANT_LIBRUNLEVEL], [1], [Enable runlevel plugin])])
AM_CONDITIONAL([WANT_RUNLEVEL], [test x$enable_runlevel != xno])

#
# upstart
#
AC_ARG_ENABLE([upstart],
  [AS_HELP_STRING([--disable-upstart],
    [disable upstart plugin (libupstart)])])

AS_IF([test "x$enable_upstart" != xno],
  [AC_DEFINE([DSME_WANT_LIBUPSTART], [1])])
AM_CONDITIONAL([WANT_UPSTART], [test x$enable_upstart != xno])

#
# systemd
#
AC_ARG_ENABLE([systemd],
  [AS_HELP_STRING([--enable-systemd],
    [enable systemd start feedback])],
  [],
  [enable_systemd=no])

AS_IF([test "x$enable_systemd" != xno],
  [AC_DEFINE([DSME_SYSTEMD_ENABLE], [1])])
AM_CONDITIONAL([WANT_SYSTEMD], [test x$enable_systemd != xno])

#
# validatorlistener
#
AC_ARG_ENABLE([validatorlistener],
  [AS_HELP_STRING([--disable-validatorlistener],
    [disable validatorlistener plugin (validatorlistener)])],
  [],
  [enable_validatorlistener=yes])

AS_IF([test "x$enable_validatorlistener" != xno],
  [AC_DEFINE([DSME_VALIDATOR_LISTENER], [1])])
AM_CONDITIONAL([WANT_VALIDATOR_LISTENER], [test x$enable_validatorlistener != xno])

#
# Generic thermal mgmt
#
AC_ARG_ENABLE([generic-thermal-mgmt],
  [AS_HELP_STRING([--disable-generic-thermal-mgmt],
    [disable generic thermal management (libthermalobject_generic)])],
  [],
  [enable_generic_thermal_mgmt=yes])

AS_IF([test "x$enable_generic_thermal_mgmt" != xno],
  [AC_DEFINE([DSME_GENERIC_THERMAL_MGMT], [1])])
AM_CONDITIONAL([WANT_GENERIC_THERMAL_MGMT], [test x$enable_generic_thermal_mgmt != xno])

#
# pwrkeymonitor
#
AC_ARG_ENABLE([pwrkeymonitor],
  [AS_HELP_STRING([--enable-pwrkeymonitor],
    [enable pwrkeymonitor plugin (pwrkeymonitor)])],
  [],
  [enable_pwrkeymonitor=no])

AS_IF([test "x$enable_pwrkeymonitor" != xno],
  [AC_DEFINE([DSME_PWRKEY_MONITOR], [1], [Enable pwrkeymonitor plugin])])
AM_CONDITIONAL([WANT_PWRKEY_MONITOR], [test x$enable_pwrkeymonitor != xno])

#
# Battery tracker
#
AC_ARG_ENABLE([battery-tracker],
  [AS_HELP_STRING([--disable-battery-tracker],
    [disable battery tracker (libbatterytracker)])],
  [],
  [enable_battery_tracker=yes])

AS_IF([test "x$enable_battery_tracker" != xno],
  [AC_DEFINE([DSME_BATTERY_TRACKER], [1])])
AM_CONDITIONAL([WANT_BATTERY_TRACKER], [test x$enable_battery_tracker != xno])

#
# Bootreason logger
#
AC_ARG_ENABLE([bootreason-logger],
  [AS_HELP_STRING([--disable-bootreason-logger],
    [disable bootreason logger (libbootreasonlogger)])],
  [],
  [enable_bootreason_logger=yes])

AS_IF([test "x$enable_bootreason_logger" != xno],
  [AC_DEFINE([DSME_BOOTREASON_LOGGER], [1])])
AM_CONDITIONAL([WANT_BOOTREASON_LOGGER], [test x$enable_bootreason_logger != xno])

#
# Shutdown feedback
#
AC_ARG_ENABLE([vibra-feedback],
  [AS_HELP_STRING([--disable-vibra-feedback],
    [disable vibra feedback (libshutdownfeedback)])],
  [],
  [enable_vibra_feedback=yes])

AS_IF([test "x$enable_vibra_feedback" != xno],
  [AC_DEFINE([DSME_VIBRA_FEEDBACK], [1])])
AM_CONDITIONAL([WANT_VIBRA_FEEDBACK], [test x$enable_vibra_feedback != xno])

#
# WLAN loader
#
AC_ARG_ENABLE([wlanloader],
  [AS_HELP_STRING([--disable-wlan-loader],
    [disable wlan loader (libwlanloader)])],
  [enable_wlan_loader=no],
  [enable_wlan_loader=yes])

AS_IF([test "x$enable_wlan_loader" != xno],
  [AC_DEFINE([DSME_WLAN_LOADER], [1])])
AM_CONDITIONAL([WANT_WLAN_LOADER], [test x$enable_wlan_loader != xno])

#
# Aboot settings
#
AC_ARG_ENABLE([abootsettings],
  [AS_HELP_STRING([--enable-abootsettings],
    [enable aboot settings plugin (libabootsettings)])])

AS_IF([test "x$enable_abootsettings" != xno],
  [AC_DEFINE([DSME_ABOOTSETTINGS], [1], [Enable aboot settings plugin]) ])

AM_CONDITIONAL([WANT_ABOOTSETTINGS], [test x$enable_abootsettings != xno])


#
# Wheel is privileged
#
AC_ARG_ENABLE([usewheel],
  [AS_HELP_STRING([--enable-usewheel],
    [Users in wheel group are considered privileged])])

AS_IF([test "x$enable_usewheel" = xyes],
  [AC_DEFINE([DSME_USEWHEEL], [1], [Users in wheel group are considered privileged])])

#
# Compiler and linker flags
#
AC_DEFINE_UNQUOTED([PRG_VERSION],[$VERSION],[Version for DSME])

# Work around autotool: in some autotool versions $exec_prefix is
# not parsed in the $sbindir variable. Thus, we get an invalid path.
if test "$sbindir" = '${exec_prefix}/sbin'; then
  # Fix the path
  AC_DEFINE_UNQUOTED([DSME_SBIN_PATH],["$exec_prefix/sbin"],[Installation dir for DSME executables])
else
  # Use the sbindir provided by autotool
  AC_DEFINE_UNQUOTED([DSME_SBIN_PATH],["$sbindir"],[Installation dir for DSME executables])
fi

AC_SUBST(CPP_GENFLAGS, ["-D_GNU_SOURCE -D_FILE_OFFSET_BITS=64"])

AC_SUBST(C_GENFLAGS, ["-pthread -g -std=c99 -Wall -Wwrite-strings -Wmissing-prototypes -Werror -Wshadow"])
AC_SUBST(C_OPTFLAGS, ["-O2"])
AC_SUBST(C_DBGFLAGS, ["-g -DDEBUG"])

# Checks for programs.
AC_PROG_CC
AC_PROG_INSTALL
AC_PROG_LN_S
AC_PROG_MAKE_SET
LT_INIT
PKG_PROG_PKG_CONFIG

# Specify dir for local macros
AC_CONFIG_MACRO_DIR([m4])

# Enable per-target compiler options
AM_PROG_CC_C_O()

# Pkgconf checks
PKG_CHECK_MODULES(LIBCRYPTSETUP, libcryptsetup)
PKG_CHECK_MODULES(GLIB, glib-2.0 >= 2.40)
PKG_CHECK_MODULES(DBUS, dbus-1 >= 1.8)
PKG_CHECK_MODULES(MCE_DEV, mce,
		  [],
		  AC_MSG_ERROR(Development files for mce are missing. Install mce-headers / mce-dev.))
if test "x$enable_vibra_feedback" != xno; then
PKG_CHECK_MODULES(LIBNGF, libngf0 >= 0.24)
fi
if test "x$enable_systemd" != xno; then
PKG_CHECK_MODULES(LIBSYSTEMD, libsystemd)
fi

# Check libs (that are not yet checked)
# Whitespaces in 'action-if-found' fields in order to not (auto)update LIBS variable
AC_CHECK_LIB([rt], [clock_nanosleep], [AC_MSG_NOTICE([got librt])], AC_MSG_FAILURE([librt required!]))
AC_CHECK_LIB([dsme], [dsmesock_init], [AC_MSG_NOTICE([got libdsme])], AC_MSG_FAILURE([libdsme required!]), [$GLIB_LIBS])

# Check headers
AC_CHECK_HEADERS([arpa/inet.h fcntl.h ftw.h inttypes.h limits.h stdint.h stdlib.h \
                  string.h strings.h sys/ioctl.h sys/socket.h sys/time.h time.h   \
                  syslog.h unistd.h utmpx.h])

# Checks for typedefs, structures, and compiler characteristics.
AC_HEADER_STDBOOL
AC_TYPE_UID_T
AC_C_INLINE
AC_TYPE_INT16_T
AC_TYPE_INT32_T
AC_TYPE_PID_T
AC_TYPE_SIZE_T
AC_TYPE_SSIZE_T
AC_TYPE_UINT16_T
AC_TYPE_UINT32_T
AC_TYPE_UINT8_T

# Check common library functions
AC_FUNC_ALLOCA
AC_FUNC_FORK
AC_FUNC_MALLOC
AC_FUNC_REALLOC
AC_FUNC_STRTOD
AC_CHECK_FUNCS([atexit clock_gettime dup2 ftw64 gettimeofday inet_ntoa memmove        \
                memset realpath select setenv socket strcasecmp strdup strerror \
                strndup strpbrk strrchr strstr strtoul uname utmpxname])

# Create files
AC_CONFIG_FILES([Makefile
                 dbus-gmain/Makefile
                 dsme/Makefile
                 modules/Makefile
                 util/Makefile
                 getbootstate/Makefile
                 test/Makefile
                 tests/Makefile
                 dsme.init])

AC_CONFIG_FILES([dsme-plugin.pc])
AC_OUTPUT

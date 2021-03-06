#!/bin/sh

# Check that all files that should have the current version agree on it

# The configure.ac is hand-edited, assume it must be sane
AC_PATH=configure.ac
AC_VERS=$(sed '/AC_INIT/!d; s/AC_INIT(dsme, \(.*\))/\1/' $AC_PATH)

RES=0

# The Makefile is hand-edited, make sure it stays in sync
MAK_PATH=Makefile.custom
MAK_VERS=$(grep '^VERSION.*:=' $MAK_PATH |sed -e 's/^.*:=[[:space:]]*//')

if [ "$MAK_VERS" != "$AC_VERS" ]; then
  echo >&2 "$AC_PATH=$AC_VERS vs $MAK_PATH=$MAK_VERS"
  RES=1
fi

# The .spec is either in rpm subdir or where ever rpmbuild got it from
SPEC_PATH=${RPM_SOURCE_DIR:-rpm}/${RPM_PACKAGE_NAME:-dsme}.spec
SPEC_VERS=$(grep '^Version:' $SPEC_PATH |sed -e 's/^.*:[[:space:]]*//')

if [ "$SPEC_VERS" != "$AC_VERS" ]; then
  echo >&2 "$AC_PATH=$AC_VERS vs $SPEC_PATH=$SPEC_VERS"

  # When building untagged commits via obs the spec will have as
  # a version something like  "<previous_tag>+<branch>.<hash>".
  # Accept those, if version matches up to the first '+'.
  if [ "${SPEC_VERS%%+*}" = "$AC_VERS" ]; then
    echo >&2 "  (ignored - assuming $AC_VERS + work in progress)"
  else
    RES=1
  fi
fi

if [ $RES != 0 ]; then
  echo >&2 "Conflicting package versions"
fi

# Stop the rpm-build ifthere were conflicts
exit $RES

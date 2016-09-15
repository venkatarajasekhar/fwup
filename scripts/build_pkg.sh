#!/bin/bash

#
# Build and package fwup
#
# Inputs:
#     SKIP_PACKAGE - set to "true" to skip the package making step
#     CROSS_COMPILE - if set to a gcc tuple, tries to crosscompile
#                     (e.g., x86_64-w64-mingw32)
#
# This script creates a static build of fwup to avoid dependency issues
# with libconfuse and libsodium. The result is a self-contained .deb
# and .rpm that should work on any Linux (assuming it's running on the
# same processor architecture) or an .exe for Windows.
#
# To build the Windows executable on Linux:
#  sudo apt-get install gcc-mingw-w64-x86-64
#  CROSS_COMPILE=x86_64-w64-mingw32 ./scripts/build_pkg.sh
#
set -e

FWUP_MAINTAINER="Frank Hunleth <fhunleth@troodon-software.com>"

BASE_DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )/.." && pwd )"
BUILD_DIR=$BASE_DIR/build

MAKE_FLAGS=-j8
LDD=ldd

if [[ -z $CROSS_COMPILE ]]; then
    CROSS_COMPILE=host

    if [[ $(uname -s) = "Darwin" ]]; then
        SKIP_PACKAGE=true
        LDD="otool -L"
    fi
else
    CONFIGURE_ARGS=--host=$CROSS_COMPILE
fi

DEPS_INSTALL_DIR=$BUILD_DIR/$CROSS_COMPILE/deps/usr
FWUP_STAGING_DIR=$BUILD_DIR/$CROSS_COMPILE/fwup-staging/
FWUP_INSTALL_DIR=$FWUP_STAGING_DIR/usr
PKG_CONFIG_PATH=$DEPS_INSTALL_DIR/lib/pkgconfig
export ChocolateyInstall=$DEPS_INSTALL_DIR/chocolatey

# Initial sanity checks
if [[ ! -e $BASE_DIR/configure ]]; then
    echo "Please run from the fwup base directory and make sure that the ./configure file exists."
    echo "If you're building from source, you may need to run ./autogen.sh."
    exit 1
fi

if [[ -e $BASE_DIR/Makefile ]]; then
    # Run distclean to ensure that we have a clean build.
    make distclean
fi

# Build the dependencies
$BASE_DIR/scripts/download_deps.sh

# Initialize some directories
mkdir -p $BUILD_DIR
mkdir -p $FWUP_INSTALL_DIR

pushd $BUILD_DIR

# Build fwup (symlink now, since out-of-tree fwup build is broke)
ln -sf $BASE_DIR $BUILD_DIR/fwup
pushd fwup
PKG_CONFIG_PATH=$PKG_CONFIG_PATH ./configure $CONFIGURE_ARGS --prefix=$FWUP_INSTALL_DIR --enable-shared=no || cat config.log
make clean
make $MAKE_FLAGS

if [[ "$CROSS_COMPILE" = "host" ]]; then
    # Verify that it was statically linked
    if $LDD src/fwup | grep libz; then
        echo "fwup was dynamically linked to zlib. This should not happen.";
        exit 1
    fi
    if $LDD src/fwup | grep confuse; then
        echo "fwup was dynamically linked to libconfuse. This should not happen.";
        exit 1
    fi
    if $LDD src/fwup | grep archive; then
        echo "fwup was dynamically linked to libarchive. This should not happen.";
        exit 1
    fi
    if $LDD src/fwup | grep sodium; then
        echo "fwup was dynamically linked to libsodium This should not happen.";
        exit 1
    fi

fi

# Run the regression tests
make check

make install-strip
make dist
popd

# Return to the base directory
popd

# Package fwup
if [[ "$SKIP_PACKAGE" != "true" ]]; then
    FWUP_VERSION=$(cat VERSION)
    if [[ "$CROSS_COMPILE" = "host" ]]; then
        # Fix directory permissions for packaging
        find $FWUP_STAGING_DIR -type d | xargs chmod 755

        # Debian requires compressed man pages
        # NOTE: Even though building man pages is optional, it is
        #       an error if man pages don't exist when creating packages
        gzip -9 -f $FWUP_STAGING_DIR/usr/share/man/man1/fwup.1

        # Build Linux packages
        rm -f fwup_*.deb fwup-*.rpm
        fpm -s dir -t deb -v $FWUP_VERSION -n fwup -m "$FWUP_MAINTAINER" -C $FWUP_STAGING_DIR
        fpm -s dir -t rpm -v $FWUP_VERSION -n fwup -m "$FWUP_MAINTAINER" -C $FWUP_STAGING_DIR
    elif [[ "$CROSS_COMPILE" = "x86_64-w64-mingw32" ]]; then
        # Build Windows package
        rm -f fwup.exe
        cp $FWUP_INSTALL_DIR/bin/fwup.exe .
        
        mkdir -p $FWUP_INSTALL_DIR/fwup/tools
        cp -f scripts/fwup.nuspec $FWUP_INSTALL_DIR/fwup/
        sed -i "s/%VERSION%/$(cat VERSION)/" $FWUP_INSTALL_DIR/fwup/fwup.nuspec
        cp $FWUP_INSTALL_DIR/bin/fwup.exe $FWUP_INSTALL_DIR/fwup/tools/
        
        pushd $FWUP_INSTALL_DIR/fwup/
        rm -f *.nupkg
        export ChocolateyInstall=$DEPS_INSTALL_DIR/chocolatey
        $ChocolateyInstall/console/choco.exe pack --allow-unofficial fwup.nuspec
        popd
        rm -f *.nupkg
        cp $FWUP_INSTALL_DIR/fwup/*.nupkg .
    fi
else
    echo "Static build was successful, but skipped creating packages."
    echo "The fwup installation is in $FWUP_INSTALL_DIR."
fi


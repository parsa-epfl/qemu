#!/bin/bash
#  DO-NOT-REMOVE begin-copyright-block
# QFlex consists of several software components that are governed by various
# licensing terms, in addition to software that was developed internally.
# Anyone interested in using QFlex needs to fully understand and abide by the
# licenses governing all the software components.
#
# ### Software developed externally (not by the QFlex group)
#
#     * [NS-3] (https://www.gnu.org/copyleft/gpl.html)
#     * [QEMU] (http://wiki.qemu.org/License)
#     * [SimFlex] (http://parsa.epfl.ch/simflex/)
#     * [GNU PTH] (https://www.gnu.org/software/pth/)
#
# ### Software developed internally (by the QFlex group)
# **QFlex License**
#
# QFlex
# Copyright (c) 2020, Parallel Systems Architecture Lab, EPFL
# All rights reserved.
#
# Redistribution and use in source and binary forms, with or without modification,
# are permitted provided that the following conditions are met:
#
#     * Redistributions of source code must retain the above copyright notice,
#       this list of conditions and the following disclaimer.
#     * Redistributions in binary form must reproduce the above copyright notice,
#       this list of conditions and the following disclaimer in the documentation
#       and/or other materials provided with the distribution.
#     * Neither the name of the Parallel Systems Architecture Laboratory, EPFL,
#       nor the names of its contributors may be used to endorse or promote
#       products derived from this software without specific prior written
#       permission.
#
# THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
# ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
# WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
# DISCLAIMED. IN NO EVENT SHALL THE PARALLEL SYSTEMS ARCHITECTURE LABORATORY,
# EPFL BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
# CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE
# GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
# HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
# LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF
# THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
#  DO-NOT-REMOVE end-copyright-block

# This function is called on in ERROR state
usage() {
    echo -e "\nUsage: $0 "
    echo -e "use -install to compile and install all dependencies to run the version of QEMU packed with QFlex."
    echo -e "use -emulation to build QEMU with the flags and options enabled for emulation mode only"
    echo -e "use -trace to build QEMU with the flags and options enabled for trace simulation only"
    echo -e "use -timing to build QEMU with the flags and options enabled for timing simulation"
}

# Getting the Absolute Path to the script
CURDIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"

# Determine which version of Linux is being run
get_linux_version() {
    if [ -f /etc/os-release ]; then
        . /etc/os-release
        OS=$NAME
        VER=$VERSION_ID
    elif type lsb_release >/dev/null 2>&1; then
        # linuxbase.org
        OS=$(lsb_release -si)
        VER=$(lsb_release -sr)
    elif [ -f /etc/lsb-release ]; then
        # For some versions of Debian/Ubuntu without lsb_release command
        . /etc/lsb-release
        OS=$DISTRIB_ID
        VER=$DISTRIB_RELEASE
    else 
        # Fallback to " Linux <> "
        OS=$(uname -s)
        VER=$(uname -r)
    fi
}

# Get linux version
get_linux_version
if [ "$OS" == "Ubuntu" ]; then
    ubuntu=true
elif [ "$OS" == "CentOS Linux" ]; then
    centos=true
fi


#Parse the dynamic options
for i in "$@"
do
    case $i in
        -timing)
        BUILD_TIMING="TRUE"
        shift
        ;;
        -emulation)
        BUILD_EMULATION="TRUE"
        shift
        ;;
        -trace)
        BUILD_TRACE="TRUE"
        shift
        ;;
        -install)
        INSTALL_DEPS="TRUE"
        shift
        ;;
        -h|--help)
        usage
        exit
        shift
        ;;
        *)
        echo "$0 : what do you mean by $i ?"
        usage
        exit 1
        ;;
    esac
done

set -x
set -e

## Installation of dependencies
if [ "${INSTALL_DEPS}" = "TRUE" ]; then
    if [ "$ubuntu" ]; then
        # Install dependencies
        sudo apt-get update -qq
        sudo apt-get install -y build-essential python-dev software-properties-common pkg-config \
            zip zlib1g-dev unzip libbz2-dev \
            expect bridge-utils uml-utilities pbzip2
        # Install known-good version of gcc-8
        GCC_VERSION="8"
        sudo apt-get install -y gcc-${GCC_VERSION} g++-${GCC_VERSION}
        sudo update-alternatives --install /usr/bin/gcc gcc /usr/bin/gcc-${GCC_VERSION} 60 --slave /usr/bin/g++ g++ /usr/bin/g++-${GCC_VERSION}
        # For ubuntu - edit sources.list to allow use of build-dep for qemu
        sudo cp /etc/apt/sources.list /etc/apt/sources.list_$(date +"%T")
        sudo sed -Ei 's/^# deb-src /deb-src /' /etc/apt/sources.list
        sudo apt-get update -qq
        sudo apt-get --no-install-recommends -y build-dep qemu
    elif [ "$centos" ]; then
        sudo yum update -q -y
        sudo yum install -y centos-release-scl
        # Install known-good version of gcc-8
        GCC_VERSION="8"
        sudo yum install -y devtoolset-${GCC_VERSION}-gcc devtoolset-${GCC_VERSION}-gcc-c++ scl-utils

        # Add devtoolset-8 to the current ~/.bashrc
        echo "source scl_source enable devtoolset-8" >> $HOME/.bashrc
        unset -xe
        source $HOME/.bashrc
        set -xe

        # Install dependencies
        sudo yum install -y make cmake python-devel autoconf binutils bison flex \
            libtool pkgconfig bzip2-devel zlib-devel pigz glib2-devel pixman-devel jemalloc-devel libicu-devel
    fi
    # Install pth if it is not installed already
    if [ ! -f $HOME/lib/libpth.so ]; then
        PTH_PATH=${CURDIR}/./pth
        pushd $PTH_PATH > /dev/null
        ./build_pth.sh
        popd > /dev/null
    fi
fi

JOBS=$(($(getconf _NPROCESSORS_ONLN) + 1))
echo "=== Using ${JOBS} simultaneous jobs ==="

# Build Qemu for emulation, or timing. Make command is replicated so that it is possible to only
# run with -install and not launch a build.
if [ "${BUILD_EMULATION}" = "TRUE" ]; then
    export CFLAGS="-fPIC"
    ./config.emulation
    make clean && make -j${JOBS}
elif [ "${BUILD_TRACE}" = "TRUE" ]; then
    export CFLAGS="-fPIC"
    ./config.trace
    make clean && make -j${JOBS}
elif [ "${BUILD_TIMING}" = "TRUE" ]; then
    export CFLAGS="-fPIC"
    ./config.timing
    make clean && make -j${JOBS}
fi


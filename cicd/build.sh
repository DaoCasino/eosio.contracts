#!/bin/bash

set -eu
set -o pipefail

PROGNAME="$( basename -- "$0" )"
PROGPATH="$( readlink -f "$( dirname "$0" )" )"

###

project=daobet # 'haya' or 'daobet'

boost_root_local="$HOME/src/boost_1_67_0"
eos_root_local="$HOME/opt/$project"

cdt_root=/usr/opt/eosio.cdt
boost_root=
eos_root=/opt/eosio
if [[ -d "$boost_root_local" ]] && [[ -d "$eos_root_local" ]]; then
  eos_root="$eos_root_local"
  boost_root="$boost_root_local"
fi

###

ncores="$(getconf _NPROCESSORS_ONLN)" # no nproc on macos :(

echo "=========== Building eosio.contracts ==========="
echo

build_dir="$PROGPATH"/../build

mkdir -p "$build_dir"
pushd "$build_dir"
  set -x
  cmake \
    -DCMAKE_MODULE_PATH="$eos_root/lib/cmake/$project" \
    -DCMAKE_LINKER="$cdt_root"/1.6.1/bin/lld \
    -DBOOST_ROOT="$boost_root" \
    -DBoost_NO_SYSTEM_PATHS=on \
    ..
  make -j"$ncores"
  set +x
popd

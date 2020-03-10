#!/bin/bash

set -eu
set -o pipefail

. "${BASH_SOURCE[0]%/*}/utils.sh"

readonly project=daobet # 'haya' or 'daobet'

readonly node_root="$HOME/$project-build"
readonly boost_root="$node_root/src/boost_1_70_0"

readonly cdt_root=/usr/opt/eosio.cdt/1.6.3

readonly ncores="$(getconf _NPROCESSORS_ONLN)" # no nproc on macos :(
readonly build_dir="$PROGPATH"/../build

#---------- command-line options ----------#

build_type=RelWithDebInfo
#local_clang=n
verbose=n
build_tests=n

usage() {
  echo "Compile contracts."
  echo
  echo "Usage:"
  echo
  echo "  $PROGNAME [ OPTIONS ]"
  echo
  echo "Options:"
  echo
  echo "  --build-type <type>  : build type: Debug, Release, RelWithDebInfo, or MinSizeRel;"
  echo "                         default: $build_type"
  #echo "  --local-clang        : build and use a partucular version of Clang toolchain locally"
  echo "  --build-tests        : build tests"
  echo "  --verbose            : verbose build"
  echo
  echo "  -h, --help           : print this message"
}

OPTS="$( getopt -o "h" -l "\
build-type:,\
build-tests,\
verbose,\
help" -n "$PROGNAME" -- "$@" )"
eval set -- "$OPTS"
while true; do
  case "${1:-}" in
  (--build-type)   build_type="$2" ; shift 2 ; readonly build_type ;;
  #(--local-clang)  local_clang=y   ; shift   ; readonly local_clang ;; # not needed on >=ubuntu-18.04
  (--build-tests)  build_tests=y   ; shift   ; readonly build_tests ;;
  (--verbose)      verbose=y       ; shift   ; readonly verbose ;;
  (-h|--help)      usage ; exit 0 ;;
  (--)             shift ; break ;;
  (*)              die "Invalid option: ${1:-}." ;;
  esac
done
unset OPTS

readonly build_type verbose build_tests

###

log "Configuration:"
log
log "  build type  = $build_type"
log "  node root   = $node_root"
log "  build tests = $build_tests"
log "  # of CPUs   = $ncores"
#log "  cmake executable  = ${CMAKE_CMD:-"<not found>"}"

###

cmake_flags=(
  -D CMAKE_BUILD_TYPE="$build_type"
  -D CMAKE_MODULE_PATH="$node_root/lib/cmake/$project"
  -D CMAKE_LINKER="$cdt_root"/bin/lld
  -D Boost_NO_SYSTEM_PATHS=on
)
[[ -z "$boost_root" ]]    || cmake_flags+=(-D BOOST_ROOT="$boost_root")
[[ "$build_tests" == n ]] || cmake_flags+=(-D BUILD_TESTS=on)

make_flags=(-j "$ncores")
[[ "$verbose" == n ]] || make_flags+=(VERBOSE=1)

mkdir -p "$build_dir"
pushd "$build_dir"
  set -x
  cmake "${cmake_flags[@]}" ..
  make "${make_flags[@]}"
  set +x
popd

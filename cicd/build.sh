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
build_docs=n

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
  echo "  --docs               : build doxygen docs (better use \`make docs' to omit cmake-phase and get"
  echo "                         much less dependencies)"
  echo
  echo "  -h, --help           : print this message"
}

OPTS="$( getopt -o "h" -l "\
build-type:,\
build-tests,\
verbose,\
docs,\
help" -n "$PROGNAME" -- "$@" )"
eval set -- "$OPTS"
while true; do
  case "${1:-}" in
  (--build-type)   build_type="$2" ; shift 2 ; readonly build_type ;;
  #(--local-clang)  local_clang=y   ; shift   ; readonly local_clang ;; # not needed on >=ubuntu-18.04
  (--build-tests)  build_tests=y   ; shift   ; readonly build_tests ;;
  (--verbose)      verbose=y       ; shift   ; readonly verbose ;;
  (--docs)         build_docs=y    ; shift   ; readonly build_docs ;;
  (-h|--help)      usage ; exit 0 ;;
  (--)             shift ; break ;;
  (*)              die "Invalid option: ${1:-}." ;;
  esac
done
unset OPTS

###

log "Configuration:"
log
log "  build type  = $build_type"
log "  node root   = $node_root"
log "  build tests = $build_tests"
log "  build docs  = $build_docs"
log "  # of CPUs   = $ncores"
#log "  cmake executable  = ${CMAKE_CMD:-"<not found>"}"

###

cmake_args=(
  -D CMAKE_BUILD_TYPE="$build_type"
  -D CMAKE_MODULE_PATH="$node_root/lib/cmake/$project"
  -D CMAKE_LINKER="$cdt_root"/bin/lld
  -D Boost_NO_SYSTEM_PATHS=on
)
[[ -z "$boost_root" ]]    || cmake_args+=(-D BOOST_ROOT="$boost_root")
[[ "$build_tests" == n ]] || cmake_args+=(-D BUILD_TESTS=on)
[[ "$build_docs" == n ]]  || cmake_args+=(-D BUILD_DOXYGEN=on)

make_args=(-j "$ncores")
[[ "$verbose" == n ]]    || make_args+=(VERBOSE=1)
[[ "$build_docs" == n ]] || make_args+=(all docs)

mkdir -p "$build_dir"
pushd "$build_dir"
  set -x
  cmake "${cmake_args[@]}" ..
  make "${make_args[@]}"
  set +x
popd

cmake_minimum_required(VERSION 3.8)

set(PROJECT "eosio_contracts")

project(${PROJECT})

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/CMakeModules")

# Check equal CMAKE_SOURCE_DIR and CMAKE_BINARY_DIR
if("${CMAKE_SOURCE_DIR}" STREQUAL "${CMAKE_BINARY_DIR}")
  set(dir "build")
  set(cmd "cmake")
  if (target)
    set(dir "${dir}/${target}")
    set(cmd "${cmd} -Dtarget=${target}")
  elseif(CMAKE_BUILD_TYPE)
    set(dir "${dir}/${CMAKE_BUILD_TYPE}")
    set(cmd "${cmd} -DCMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE}")
  else()
    set(dir "${dir}/default")
  endif()
  set(cmd "${cmd} ${CMAKE_SOURCE_DIR}")

  message(FATAL_ERROR "Builds are not allowed in ${CMAKE_SOURCE_DIR}.\n"
    "Instead:\n"
    "1) Remove the CMakeCache.txt file and CMakeFiles directory from ${CMAKE_SOURCE_DIR}.\n"
    "2) Create a directory to hold your build files, for example: ${dir}.\n"
    "3) Change to that directory.\n"
    "4) Run cmake targetting ${CMAKE_SOURCE_DIR}, for example: ${cmd}")
endif()

# doxygen
set(BUILD_DOXYGEN off CACHE BOOL "Build doxygen documentation by default")

include(daobet-utils) # from daobet/CMakeModules

get_version_from_git(_ CONTRACTS_VER_DIRTY)
message(STATUS "Building eosio.contracts (version ${CONTRACTS_VER_DIRTY})...")
#XXX: CONTRACTS_VER_DIRTY used in contracts/eosio.system/include/eosio.system/contracts.version.hpp.in

include(doxygen)

include(ExternalProject)
find_package(eosio.cdt)
message(STATUS "Package eosio.cdt found; EOSIO_CDT_ROOT = ${EOSIO_CDT_ROOT}")

set(eosio_cdt_version_hard_min "1.6")
set(eosio_cdt_version_soft_max "1.6")
set(eosio_cdt_version_hard_max "")

### Check the version of eosio.cdt
set(version_match_error_msg "")
EOSIO_CHECK_VERSION(
  check_status
  "${EOSIO_CDT_VERSION}"
  "${eosio_cdt_version_hard_min}"
  "${eosio_cdt_version_soft_max}"
  "${eosio_cdt_version_hard_max}"
  version_match_error_msg
)
if(check_status STREQUAL "MATCH")
  message(STATUS "Using eosio.cdt version ${EOSIO_CDT_VERSION}")
elseif(check_status STREQUAL "WARN")
  message(WARNING "Using eosio.cdt version ${EOSIO_CDT_VERSION} even though it exceeds the maximum\n"
    "supported version of ${eosio_cdt_version_soft_max}; continuing with configuration, however build may fail.\n"
    "It is recommended to use eosio.cdt version ${eosio_cdt_version_soft_max}.")
else() # INVALID OR MISMATCH
  message(FATAL_ERROR "Found eosio.cdt version ${EOSIO_CDT_VERSION} but it does not satisfy version\n"
    "requirements: ${version_match_error_msg}. Please use eosio.cdt version ${eosio_cdt_version_soft_max}.")
endif()

#TODO: Contracts' tests depend on different libraries for different build types:
# libfc.a (for release) or libfc_debug.a (for debug) (the same for secp256k1 library).
# (see EosioTester.cmake)
set(TEST_BUILD_TYPE ${CMAKE_BUILD_TYPE}) # use given build type only for tests
set(DEBUG_MODE 0) # exclude dlogs table (cannot use CMAKE_BUILD_TYPE + NDEBUG: tests get broken)
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
  set(CMAKE_BUILD_TYPE "RelWithDebInfo")
  set(DEBUG_MODE 1)
endif()

ExternalProject_Add(
  contracts_project
  SOURCE_DIR ${CMAKE_SOURCE_DIR}/contracts
  BINARY_DIR ${CMAKE_BINARY_DIR}/contracts
  CMAKE_ARGS
    -DDEBUG_MODE=${DEBUG_MODE}
    -DCMAKE_TOOLCHAIN_FILE=${EOSIO_CDT_ROOT}/lib/cmake/eosio.cdt/EosioWasmToolchain.cmake
    -DCONTRACTS_VER_DIRTY=${CONTRACTS_VER_DIRTY}
  UPDATE_COMMAND ""
  PATCH_COMMAND ""
  TEST_COMMAND ""
  INSTALL_COMMAND ""
  BUILD_ALWAYS 1
)

if (APPLE)
  set(OPENSSL_ROOT "/usr/local/opt/openssl")
elseif (UNIX)
  set(OPENSSL_ROOT "/usr/include/openssl")
endif()
set(SECP256K1_ROOT "/usr/local")

message(STATUS "Configuration:")
message(STATUS "  CMAKE_PREFIX_PATH = ${CMAKE_PREFIX_PATH}")
message(STATUS "  CMAKE_BUILD_TYPE  = ${CMAKE_BUILD_TYPE}")
message(STATUS "  TEST_BUILD_TYPE   = ${TEST_BUILD_TYPE}")
message(STATUS "  OPENSSL_ROOT      = ${OPENSSL_ROOT}")

set(BUILD_TESTS FALSE CACHE BOOL "Build unit tests")
if(BUILD_TESTS)
  string(REPLACE ";" "|" TEST_PREFIX_PATH "${CMAKE_PREFIX_PATH}")
  string(REPLACE ";" "|" TEST_FRAMEWORK_PATH "${CMAKE_FRAMEWORK_PATH}")
  string(REPLACE ";" "|" TEST_MODULE_PATH "${CMAKE_MODULE_PATH}")

  ExternalProject_Add(
    contracts_unit_tests
    LIST_SEPARATOR | # Use the alternate list separator
    CMAKE_ARGS
      -DBOOST_ROOT=${BOOST_ROOT}
      -DBoost_NO_SYSTEM_PATHS=${Boost_NO_SYSTEM_PATHS}
      -DCMAKE_BUILD_TYPE=${TEST_BUILD_TYPE}
      -DCMAKE_FRAMEWORK_PATH=${TEST_FRAMEWORK_PATH}
      -DCMAKE_MODULE_PATH=${TEST_MODULE_PATH}
      -DCMAKE_PREFIX_PATH=${TEST_PREFIX_PATH} #XXX: remove???
      -DEOSIO_ROOT=${EOSIO_ROOT}
      -DLLVM_DIR=${LLVM_DIR}
      -Deosio_DIR=${CMAKE_MODULE_PATH}
    SOURCE_DIR ${CMAKE_SOURCE_DIR}/tests
    BINARY_DIR ${CMAKE_BINARY_DIR}/tests
    TEST_COMMAND ""
    INSTALL_COMMAND ""
    BUILD_ALWAYS 1
  )
  add_dependencies(contracts_unit_tests contracts_project)

  add_custom_target(create_tar COMMAND
    mkdir -p assets &&
      tar -C ${CMAKE_BINARY_DIR} -cvz --exclude='include' --exclude='*.cmake' --exclude='Makefile' --exclude='CMake*'
      -f "assets/contracts-${CONTRACTS_VER_DIRTY}.tar.gz" "contracts" --transform='s/contracts/contracts-${CONTRACTS_VER_DIRTY}/g'
  )
  add_dependencies(create_tar contracts_unit_tests)
else()
  message(STATUS "Unit tests will not be built. To build unit tests, set BUILD_TESTS to true.")
endif()

cmake_minimum_required(VERSION 3.5)

find_package(eosio)

#~ set(EOSIO_VERSION_MIN "1.8")
#~ set(EOSIO_VERSION_SOFT_MAX "1.8")
#~ #set(EOSIO_VERSION_HARD_MAX "")
#~
#~ ### Check the version of eosio
#~ set(VERSION_MATCH_ERROR_MSG "")
#~ EOSIO_CHECK_VERSION(
#~   VERSION_OUTPUT
#~   "${EOSIO_VERSION}"
#~   "${EOSIO_VERSION_MIN}"
#~   "${EOSIO_VERSION_SOFT_MAX}"
#~   "${EOSIO_VERSION_HARD_MAX}"
#~   VERSION_MATCH_ERROR_MSG
#~ )
#~ if(VERSION_OUTPUT STREQUAL "MATCH")
#~   message(STATUS "Using eosio version ${EOSIO_VERSION}")
#~ elseif(VERSION_OUTPUT STREQUAL "WARN")
#~   message(WARNING "Using eosio version ${EOSIO_VERSION} even though it exceeds "
#~     "the maximum supported version of ${EOSIO_VERSION_SOFT_MAX}; continuing with "
#~     "configuration, however build may fail.\n"
#~     "It is recommended to use eosio version ${EOSIO_VERSION_SOFT_MAX}.")
#~ else() # INVALID OR MISMATCH
#~   message(FATAL_ERROR "Found eosio version ${EOSIO_VERSION} but it does not "
#~     "satisfy version requirements: ${VERSION_MATCH_ERROR_MSG}\n"
#~     "Please use eosio version ${EOSIO_VERSION_SOFT_MAX}.")
#~ endif(VERSION_OUTPUT STREQUAL "MATCH")

include(CTest)

configure_file(${CMAKE_SOURCE_DIR}/contracts.hpp.in ${CMAKE_BINARY_DIR}/contracts.hpp)

set(unittest_src
  eosio.msig_tests.cpp
  eosio.system_tests.cpp
  eosio.token_tests.cpp
  eosio.wrap_tests.cpp
)

add_eosio_test_executable(unit_test
  ${unittest_src}
  main.cpp
)

target_include_directories(unit_test PUBLIC "${CMAKE_BINARY_DIR}")

# TODO: check this loop!!!
# mark test suites for execution
foreach(test_suite ${unit_tests}) # create an independent target for each test suite
  execute_process(
    COMMAND
      bash -c "grep -E 'BOOST_AUTO_TEST_SUITE\\s*[(]' ${test_suite} | grep -vE '//.*BOOST_AUTO_TEST_SUITE\\s*[(]' | cut -d ')' -f 1 | cut -d '(' -f 2"
    OUTPUT_VARIABLE suite_name
    OUTPUT_STRIP_TRAILING_WHITESPACE
  ) # get the test suite name from the *.cpp file

  if (NOT "" STREQUAL "${suite_name}") # ignore empty lines
    execute_process(
      COMMAND bash -c "echo ${suite_name} | sed -e 's/s$//' | sed -e 's/_test$//'"
      OUTPUT_VARIABLE trimmed_suite_name
      OUTPUT_STRIP_TRAILING_WHITESPACE
    ) # trim "_test" or "_tests" from the end of ${SUITE_NAME}
    # to run unit_test with all log from blockchain displayed, put "--verbose" after "--", i.e. "unit_test -- --verbose"
    add_test(
      NAME ${trimmed_suite_name}_unit_test
      COMMAND unit_test --run_test=${suite_name} --report_level=detailed --color_output
    )
  endif()
endforeach()

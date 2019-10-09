execute_process(COMMAND git describe --tags --dirty
                COMMAND tr -d '\n'
                OUTPUT_VARIABLE CONTRACTS_VERSION)
configure_file(${CMAKE_CURRENT_SOURCE_DIR}/include/eosio.system/contracts.version.hpp.in ${CMAKE_CURRENT_BINARY_DIR}/include/eosio.system/contracts.version.hpp)

add_contract(eosio.system eosio.system ${CMAKE_CURRENT_SOURCE_DIR}/src/eosio.system.cpp)

target_include_directories(eosio.system
   PUBLIC
   ${CMAKE_CURRENT_SOURCE_DIR}/include
   ${CMAKE_CURRENT_BINARY_DIR}/include
   ${CMAKE_CURRENT_SOURCE_DIR}/../eosio.token/include)

set_target_properties(eosio.system
   PROPERTIES
   RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")
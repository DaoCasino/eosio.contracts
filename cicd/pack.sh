#! /bin/bash

printf "\t=========== Create archive eosio.contracts ===========\n\n"

RED='\033[0;31m'
NC='\033[0m'

mkdir -p build
pushd build &> /dev/null
cmake ..
cmake --build . --target create_tar
popd &> /dev/null

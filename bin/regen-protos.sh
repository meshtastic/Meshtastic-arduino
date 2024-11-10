#!/usr/bin/env bash

set -e

echo "This script requires https://jpa.kapsi.fi/nanopb/download/ version 0.4.9 to be located in the"
echo "firmware root directory if the following step fails, you should download the correct"
echo "prebuilt binaries for your computer into nanopb-0.4.9"

# the nanopb tool seems to require that the .options file be in the current directory!
cd protobufs
../nanopb-0.4.9/generator-bin/protoc --experimental_allow_proto3_optional "--nanopb_out=-S.c -v:../src/" -I=../protobufs meshtastic/*.proto

# remove the device only protobuf, so we don't need std::vector
rm -rf ../src/meshtastic/deviceonly.*

#echo "Regenerating protobuf documentation - if you see an error message"
#echo "you can ignore it unless doing a new protobuf release to github."
#bin/regen-docs.sh

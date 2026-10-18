#!/usr/bin/env sh
# Build and run the pet unit tests with the host C compiler.
# No local gcc on Windows? Run it in a container:
#   docker run --rm -v "$PWD/firmware":/f -w /f gcc:14 sh test/pet/run.sh
set -eu
cd "$(dirname "$0")/../.."
cc -std=c11 -Wall -Wextra -Werror -O1 -Icomponents/pet/include \
   components/pet/pet.c test/pet/test_pet.c -o /tmp/test_pet
/tmp/test_pet

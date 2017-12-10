#!/bin/sh -eu
set -eu

if echo "Lorem ipsum" | xpipe -b 12 awk '{ exit 123 }'; then
    exit 1 # Unexpected success
else
    test $? -eq 123
fi

if printf "L" | xpipe -b 12 awk '{ exit 123 }'; then
    exit 1 # Unexpected success
else
    test $? -eq 123
fi

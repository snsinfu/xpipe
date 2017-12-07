#!/bin/sh -eu
set -eu

slow_source() {
    printf "The quick brown "
    sleep 2
    printf "fox jumps over the lazy dog\n"
}

actual="$(slow_source | xpipe -t 1 awk '{ print NR, $0 }')"

# xpipe should wait for newline even if it sees some text on time out.
expected="\
1 The quick brown fox jumps over the lazy dog"

test x"${actual}" = x"${expected}"

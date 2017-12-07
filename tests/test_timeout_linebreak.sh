#!/bin/sh -eu
set -eu

slow_source() {
    printf "The quick brown\nfox\njumps over "
    sleep 2
    printf "the lazy dog\n"
}

actual="$(slow_source | xpipe -t 1 awk '{ print NR, $0 }')"

# xpipe should not output incomplete line on time out.
expected="\
1 The quick brown
2 fox
1 jumps over the lazy dog"

test x"${actual}" = x"${expected}"

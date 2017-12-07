#!/bin/sh -eu
set -eu

slow_source() {
    echo "The quick brown fox"
    sleep 2
    echo "jumps over the lazy dog"
}

actual="$(slow_source | xpipe -t 1 awk '{ print NR, $0 }')"

# xpipe should launch awk for each line due to time out.
expected="\
1 The quick brown fox
1 jumps over the lazy dog"

test x"${actual}" = x"${expected}"

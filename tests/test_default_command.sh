#!/bin/sh -eu
set -eu

expected="Lorem ipsum"
actual="$(echo "${expected}" | xpipe)"

test x"${actual}" = x"${expected}"

#!/bin/sh -eu
set -eu

lorem_ipsum="\
Lorem ipsum dolor sit amet,
consectetur adipiscing elit.
Vestibulum consectetur turpis
massa, ut semper nibh sodales
sed. Etiam aliquam tellus magna,
vehicula sollicitudin nisl viverra quis."

actual="$(echo "${lorem_ipsum}" | xpipe -b 60 awk '{ print NR, $0 }')"

expected="\
1 Lorem ipsum dolor sit amet,
2 consectetur adipiscing elit.
1 Vestibulum consectetur turpis
2 massa, ut semper nibh sodales
1 sed. Etiam aliquam tellus magna,
1 vehicula sollicitudin nisl viverra quis."

test x"${actual}" = x"${expected}"

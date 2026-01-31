#!/bin/bash
if [ -z "$1" ]; then
    echo "default format dir ."
    format_dir="."
else
    echo "format dir:$1"
    format_dir="$1"
fi
find ${format_dir} -type f -name '*.cpp' -o  -name '*.h' -o -name '*.hpp' -o -name '*.hh' -o -name '*.cc' -o -name '*.c'| xargs -I {} clang-format -i \
-style=file -fallback-style=microsoft -sort-includes=1 --Wno-error=unknown -assume-filename={} {}
#!/bin/bash

if [ "$#" -lt 2 ]; then
    echo "failed: missing input parameters"
    exit 1
fi

writefile="$1"
writestr="$2"

dirpath="$(dirname $1)"
filename="$(basename $1)"

mkdir -p "$dirpath"

if [ "$?" -ne 0 ]; then
    echo "failed to create directory $dirpath"
    exit 1
fi

touch "$1"

if [ "$?" -ne 0 ]; then
    echo "failed to create file $filename"
    exit 1
fi

echo "$2" > "$1"
exit 0

#!/bin/bash

if [ "$#" -lt 2 ]; then
    echo "failed: missing input paramenters"
    exit 1
fi

dirpath="$1"
searchstr="$2"

if [ ! -d "$dirpath" ]; then
    echo "$dirpath does not exist in this file system"
    exit 1
fi

numfiles=$(find "$dirpath" -type f | wc -l)
numlines=$(grep -r "$searchstr" "$dirpath" | wc -l)

echo "The number of files are $numfiles and the number of matching lines are $numlines"

exit 0

#!/bin/sh
set -eu

if [ ! $# -eq 2 ]; then
    echo "Usage: $0 files_dir search_str" >&2
    exit 1
fi

filesdir="$1"
searchstr="$2"

if [ ! -d "$filesdir" ]; then
    echo "Error: '$filesdir' does not exist" >&2
    exit 1
fi

number_of_files=$(find "$filesdir" -type f | wc -l)
matching_lines=$(grep -r -- "$searchstr" "$filesdir" | wc -l || true)

echo "The number of files are $number_of_files and the number of matching lines are $matching_lines"

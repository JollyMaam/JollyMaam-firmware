#!/bin/sh
set -eu

project=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)

# These are generated directories owned exclusively by this project.
if [ -d "$project/build" ]; then
    find "$project/build" -depth -mindepth 1 -delete
    rmdir "$project/build"
fi
if [ -d "$project/dist" ]; then
    find "$project/dist" -depth -mindepth 1 -delete
    rmdir "$project/dist"
fi

#!/bin/sh
# Double-click in Finder: opens Terminal and runs CosmosEx in it - Ctrl+C or closing the window stops it.
cd "$(dirname "$0")" || exit 1
exec ./ce_run

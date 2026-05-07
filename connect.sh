#!/usr/bin/env bash
set -e
IP="$1"
socat -,ignoreeof TCP:"$IP":1234

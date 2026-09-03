#!/usr/bin/env bash
#
# Every numeric RPC argument must appear in src/rpc/client.cpp's conversion
# table, or the method throws a type error when called from Hemis-cli while
# working fine over Python/curl RPC. See BUG-059.
export LC_ALL=C
exec "$(dirname "${BASH_SOURCE[0]}")/check-rpc-convert.py"

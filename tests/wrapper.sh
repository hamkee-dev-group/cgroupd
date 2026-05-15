#!/usr/bin/env sh
set -eu

if [ "${1:-}" = "run" ]; then
    shift
fi

while [ "$#" -gt 0 ]; do
    case "$1" in
        --policy-file)
            shift 2
            ;;
        --)
            shift
            break
            ;;
        *)
            break
            ;;
    esac
done

exec "$@"

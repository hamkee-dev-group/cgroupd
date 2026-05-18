#!/usr/bin/env sh
set -eu

if [ "${1:-}" = "run" ]; then
    shift
fi

while [ "$#" -gt 0 ]; do
    case "$1" in
        --policy-file)
            if [ "$#" -lt 2 ]; then
                echo "wrapper: --policy-file requires an argument" >&2
                exit 2
            fi
            shift 2
            ;;
        --policy-file=*)
            shift
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

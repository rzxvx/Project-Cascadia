#!/bin/sh
# Read only Linux power_supply attributes; never probe PMU/I2C or set limits.
# Print one sample to stdout. A host can collect repeated samples after boot.
set -eu

if [ "$#" -gt 1 ] || [ "${1:-}" = "--help" ]; then
    echo "usage: battery-snapshot.sh [power_supply_directory]" >&2
    echo "default: /sys/class/power_supply/battery; output uses kernel ABI units" >&2
    [ "$#" -le 1 ] && exit 0
    exit 2
fi
directory=${1:-/sys/class/power_supply/battery}
if [ ! -d "$directory" ]; then
    echo "battery power_supply directory is unavailable: $directory" >&2
    exit 1
fi

printf 'timestamp_utc=%s\n' "$(date -u +%Y-%m-%dT%H:%M:%SZ)"
# Stable allowlist excludes serial numbers and other identifying attributes.
for property in present status capacity voltage_now current_now temp \
                charge_now charge_full charge_full_design cycle_count; do
    if value=$(cat "$directory/$property" 2>/dev/null); then
        case "$property" in
            status)
                case "$value" in
                    Unknown|Charging|Discharging|Full|'Not charging') ;;
                    *) value=unavailable ;;
                esac
                ;;
            *)
                case "$value" in -*) number=${value#-} ;; *) number=$value ;; esac
                case "$number" in ''|*[!0-9]*) value=unavailable ;; esac
                ;;
        esac
    else
        value=unavailable
    fi
    printf '%s=%s\n' "$property" "$value"
done

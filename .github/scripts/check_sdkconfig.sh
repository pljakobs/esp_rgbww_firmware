#!/usr/bin/env bash
# Verify that every option requested in the project's SDK_CUSTOM_CONFIG
# (sdkconfig.esp32) actually ended up in the generated IDF sdkconfig.
#
# IDF only applies sdkconfig.defaults to options missing from an existing
# sdkconfig, so a stale generated config silently overrides project settings.
#
# Usage: check_sdkconfig.sh <requested sdkconfig.esp32> <generated sdkconfig>
set -euo pipefail

if [ $# -ne 2 ]; then
	echo "usage: $0 <requested-config> <generated-sdkconfig>" >&2
	exit 2
fi

requested=$1
generated=$2

if [ ! -f "$requested" ]; then
	echo "check_sdkconfig: requested config '$requested' not found" >&2
	exit 2
fi
if [ ! -f "$generated" ]; then
	echo "check_sdkconfig: generated sdkconfig '$generated' not found" >&2
	exit 2
fi

missing=0
checked=0
while IFS= read -r line || [ -n "$line" ]; do
	line=${line%%#*}
	line=$(printf '%s' "$line" | tr -d '[:space:]')
	case "$line" in
		CONFIG_*=*) ;;
		*) continue ;;
	esac
	key=${line%%=*}
	value=${line#*=}
	checked=$((checked + 1))
	if [ "$value" = "n" ]; then
		if grep -qxF "# $key is not set" "$generated" || ! grep -q "^$key=" "$generated"; then
			continue
		fi
	elif grep -qxF "$key=$value" "$generated"; then
		continue
	fi
	actual=$(grep -m1 -E "^$key=|^# $key is not set" "$generated" || echo "<absent>")
	echo "check_sdkconfig: $key: requested '$value', generated '$actual'" >&2
	missing=$((missing + 1))
done < "$requested"

if [ "$missing" -ne 0 ]; then
	echo "check_sdkconfig: $missing of $checked option(s) from $requested not applied to $generated" >&2
	echo "check_sdkconfig: run 'make sdk-config-clean SMING_SOC=<soc>' and rebuild" >&2
	exit 1
fi
echo "check_sdkconfig: all $checked option(s) from $requested applied"

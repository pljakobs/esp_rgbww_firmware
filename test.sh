#!/bin/bash
set -euo pipefail
BASE='http://192.168.29.32'
echo '== color GET =='
curl -sS -m 8 -i "$BASE/color" | sed -n '1,40p'
echo
echo '== color POST valid =='
echo 'data: {"raw":{"r":12,"g":34,"b":56,"ww":78,"cw":90}}'
curl http://192.168.29.32/color -X POST -H 'Content-Type: application/json' --data '{"raw":{"r":12,"g":34,"b":56,"ww":78,"cw":90}}'

echo 'done'
echo '== ping after valid POST =='
curl -sS -m 8 -i "$BASE/ping"
echo
echo '== color POST malformed =='
echo 'data: {"raw":{"r":12,"g":34'
curl http://192.168.29.32/color -X POST -H 'Content-Type: application/json' --data '{"raw":{"r":12,"g":34'
echo 'done'
echo '== ping after malformed POST =='
curl -sS -m 8 -i "$BASE/ping"

# ---------------------------------------------------------------------------
# ConfigDB-backed /color inbound parsing:
# Api::dispatchColorFromStream() -> JsonProcessor::onColor(CommandRequestFieldsUpdater).
# Each case: POST, then GET /color to eyeball the resulting state.
# Run against a real device/emulator at $BASE - not executed automatically here.
# ---------------------------------------------------------------------------

post_color() {
	echo "data: $1"
	curl -sS -m 8 -i "$BASE/color" -X POST -H 'Content-Type: application/json' --data "$1"
	echo
}

get_color() {
	curl -sS -m 8 -i "$BASE/color" | sed -n '1,40p'
	echo
}

echo
echo '== color POST hsv absolute (h=120 s=100 v=50) =='
post_color '{"hsv":{"h":120,"s":100,"v":50}}'
get_color

echo '== color POST hsv as quoted absolute strings (same values, string form) =='
post_color '{"hsv":{"h":"120","s":"100","v":"50"}}'
get_color

echo '== color POST hsv relative (+30 hue, -10 value) applied on top of the previous command =='
post_color '{"hsv":{"h":"+30","v":"-10"}}'
get_color

echo '== color POST raw relative (ramp up red by +50, ramp down blue by -20) =='
post_color '{"raw":{"r":"+50","b":"-20"}}'
get_color

echo '== color POST raw absolute with explicit cmd=fade and t (ramp time, ms) =='
post_color '{"raw":{"r":500,"g":500,"b":500,"ww":0,"cw":0},"cmd":"fade","t":2000}'
get_color

echo '== color POST hsv with explicit d=0 (reverse direction) - must not silently revert to default d=1 =='
post_color '{"hsv":{"h":200,"s":80,"v":60},"d":0}'
echo 'expected: HTTP 200 success (checkParams only rejects direction outside 0/1)'

echo '== color POST with neither raw nor hsv - expect "No color object!" error (400) =='
post_color '{"cmd":"solid"}'
echo 'expected: HTTP 400, error message "No color object!"'

echo '== color POST malformed json (ConfigDB import path) =='
post_color '{"hsv":{"h":120'
echo 'expected: HTTP 400, error message "malformed json"'

echo '== color POST empty body =='
curl -sS -m 8 -i "$BASE/color" -X POST -H 'Content-Type: application/json' --data ''
echo
echo 'expected: HTTP 400, error message "no body"'

echo '== ping after /color test block =='
curl -sS -m 8 -i "$BASE/ping"
echo

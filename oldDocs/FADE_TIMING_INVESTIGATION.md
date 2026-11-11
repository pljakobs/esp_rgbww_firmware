# Fade / Color-Transition Timing Investigation

Status: **open — needs firmware follow-up**
Date: 2026-10-03

## Question

Do commanded color fades run at the right wall-clock duration on real
controllers, and if there is an error, is it a network/measurement artifact or a
real firmware timing problem?

## Method

Probe `/tmp/fade_timing.py` drives a real device over the `/ws` WebSocket:

- POST `/color` fade `{"q":"back","hsv":{...},"t":<ms>,"cmd":"fade"}`.
- Watch the automatic `color_event` JSON-RPC notifications (hue trace).
- `measured` = time from POST to the target hue first being reached.
- `span`     = time from the **first** to the **last** `color_event`
  (cancels one-way network latency, since both frames cross the net with
  roughly equal delay).

Devices:
- ESP8266 `192.168.29.62` (different PWM backend)
- ESP32   `192.168.29.29` (different PWM backend)
- Both release build `V5.0.0-988-experimental`, rgbww `0.10.0`, Sming `6.2.0`.

## Results

ESP8266 `.62`, span-instrumented run (2 iters each):

| cmd (s) | meas  | meas_err% | span   | span_err% | meas−span |
|--------:|------:|----------:|-------:|----------:|----------:|
|       2 |  2.37 |   +18.7%  |  1.91  |   −4.5%   |   0.46    |
|       5 |  5.88 |   +17.6%  |  5.43  |   +8.5%   |   0.45    |
|      10 | 11.42 |   +14.2%  | 11.21  |  +12.1%   |   0.21    |
|      20 | 22.11 |   +10.6%  | 21.71  |   +8.5%   |   0.40    |
|      50 | 56.53 |   +13.1%  | 56.05  |  +12.1%   |   0.48    |
|     100 |112.15 |   +12.2%  |111.80  |  +11.8%   |   0.35    |

Earlier (idle) sweeps for reference:
- ESP8266: 100s → +3.4%
- ESP32:   100s → +4.6%

## Findings

1. **Fixed ~0.4s offset = network/measurement.** `meas − span` is constant
   (~0.4s) regardless of duration — POST→start latency + first-event lead +
   final-event delivery. Matches the "~4 latency traversals + Sming tick"
   hypothesis. Does NOT scale with duration.

2. **Proportional error is firmware, not network.** The latency-cancelling
   `span` still shows +11.8% at 100s. Network latency cannot produce an error
   proportional to fade length, so the animation genuinely runs slow in
   firmware.

3. **It is load-dependent / inconsistent.** Same ESP8266, same build: +3.4%
   idle vs +11.8% under WS streaming load. The animation step timer is not
   isochronous — it appears to be starved by WiFi/TCP/WebSocket work (the act of
   measuring over WS adds exactly that load). Same shape on ESP8266 and ESP32
   despite different PWM backends ⇒ the slowdown is in the **shared** RGBWWLed
   step scheduler, not the PWM driver.

## Next actions

- [ ] Inspect the animation step-timer driving code in the RGBWWLed engine /
      `ledctrl` — is step cadence driven by a hardware timer or by a
      tick-accumulator that can be delayed under load?
- [ ] Quantify load sensitivity: back-to-back idle vs loaded sweeps on one
      device (vary WS event volume).
- [ ] Decide whether to compensate (schedule steps against a monotonic clock
      rather than counting ticks) so fades keep wall-clock duration under load.

## Artifacts

- Probe: `/tmp/fade_timing.py` (not committed; `/tmp`).
- Raw logs: `/tmp/fade_timing_full.txt` (8266 idle),
  `/tmp/fade_timing_esp32.txt` (esp32 idle),
  `/tmp/fade_timing_span_8266.txt` (8266 span-instrumented).

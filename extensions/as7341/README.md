# AS7341 measurement protocol

When the extension is built, the firmware probes address `0x39` on the
configured shared I2C bus and exposes these methods only while a matching,
enabled sensor is detected. The fixed instance ID is `spectrum-1`. The
separate `extension-as7341` ConfigDB contains the sensor's settings. Detection occurs at
startup and after an I2C bus reconfiguration. There is no periodic
measurement; `interval_ms` in the storage schema has no runtime behavior.

## Configuration

Authenticated `GET /extensions/spectrum-1/config` returns `extension_id`,
`available`, and the persistent settings: `enabled` (boolean),
`interval_ms` (200-3600000, reserved; does not schedule measurements),
`atime` (0-255), `astep` (0-65535), and `gain` (register index 0-10).
`POST` to the same endpoint accepts a nonempty subset of those settings,
validates and saves them, then reprobes the sensor. Unknown keys or invalid
values return 400; changes during a measurement return 409 without interrupting
it. Save failures return 500. Configuration remains accessible when the sensor
or shared I2C bus is disabled or the device is disconnected.

The webapp Sensors card exposes these settings alongside a fitted plot through
all eight visible-channel ADC measurements. Clear brightness is shown separately
for each acquisition, not as lux. Home Assistant exposes native configuration
and measurement entities; use their history graphs for trends.

## Start a measurement

Send a JSON-RPC request over `/ws` using method
`extensions.start_measurement` and params
`{"extension_id":"spectrum-1"}`. A successful response has result
`{"extension_id":"spectrum-1","valid":false}`. An integer request ID is
required so the client can receive acceptance or an error; string IDs are not
supported by the current JSON-RPC transport.

Alternatively, send `POST /extensions/<id>/start_measurement` over HTTP. An
accepted request returns HTTP 202 and the same result object as its JSON body.
The ID in the path identifies the extension; no JSON body is needed.

Acceptance atomically clears the previously valid result before returning and
starts a new measurement. If a measurement is already in progress, reject the
request with JSON-RPC error code `-32010` or HTTP 409, without changing the
current measurement or its requesting client. A missing, disabled, or
undetected extension is an error, not an accepted measurement.

## Read the result

The initiating WebSocket connection receives an
`extensions.measurement` JSON-RPC notification on success or failure, with the
result object below as `params`. No other connection is notified, including
when the start request came from HTTP. If the initiating connection disconnects,
the measurement continues and the result remains available for polling.

Poll `GET /extensions/<id>/readings` over HTTP to obtain the latest result as
the JSON body, including while measurement is in progress. A missing, disabled,
or undetected extension returns an HTTP error rather than a readings object.

```json
{
  "extension_id": "spectrum-1",
  "valid": true,
  "state": "complete",
  "error": "",
  "age_ms": 42,
  "data": {
    "f1": 100, "f2": 101, "f3": 102, "f4": 103,
    "f5": 104, "f6": 105, "f7": 106, "f8": 107,
    "clear_low": 108, "nir_low": 109,
    "clear_high": 110, "nir_high": 111,
    "atime": 29, "astep": 599, "gain": 6
  }
}
```

The result is a consistent snapshot with these states:

| State | `valid` | `error` | Meaning |
| --- | --- | --- | --- |
| `idle` | false | empty | No measurement has completed since initialization. |
| `measuring` | false | empty | An accepted measurement is in progress. |
| `complete` | true | empty | All channels were acquired successfully. |
| `error` | false | nonempty | Acquisition failed; the string describes the error. |

`data` and `age_ms` are meaningful only when `valid` is true; clients must
ignore them otherwise. The schema includes these fields even for invalid
results so ConfigDB can serialize one fixed body. `age_ms` is the elapsed
milliseconds since completion, saturated at 2147483647.
On failure, the notification carries the `error` state, not a successful
reading. A subsequent accepted start clears the error and returns to
`measuring`.

The channel values are uncalibrated 16-bit ADC counts, not lux or calibrated
spectral power. F1-F4 and F5-F8 require separate SMUX acquisitions; clear and
NIR are reported separately for the low and high acquisitions rather than
combined. `atime`, `astep`, and `gain` are the sensor register settings used
for both acquisitions. A result becomes valid only after the complete set has
been acquired successfully. The runtime checks I2C operations and applies
timeouts to SMUX and integration; failures are reported as invalid results.

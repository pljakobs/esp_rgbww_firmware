# INA219 current sensor

The extension uses the shared configured I2C bus, probes at startup and after
bus or extension settings changes, and samples periodically while detected.
Its instance ID is `current-1`. The INA219 has no unique device ID: probing
checks the configuration register and verifies a write/readback, so another
device at the configured address cannot be positively identified. Ensure the
address is not occupied by another device.

The separate `extension-ina219` ConfigDB contains persistent settings:
`enabled` (default true), `address` (0x40 through 0x4f, default 0x40),
`shunt_ohms` (0.00001 through 1, default 0.1) and `interval_ms` (200 through
3600000, default 1000). Set the resistance to the actual installed shunt;
incorrect resistance gives proportionally incorrect current and power.

## HTTP API

With the same authentication as other HTTP APIs:

* `GET /extensions/current-1/config` returns the settings and `available`.
* `POST /extensions/current-1/config` accepts a nonempty JSON object with any
  subset of the four settings, validates and persists them, then reprobes.
  Settings remain accessible when the sensor is disconnected, disabled or
  the I2C bus is disabled. A successful response includes the resulting
  configuration and detection status.
* `GET /extensions/current-1/readings` returns 404 if not detected. Otherwise
  it returns `extension_id`, `valid`, `error`, `age_ms`, `voltage_v`,
  `current_a`, and `power_w`. Ignore numeric values unless `valid` is true.
  Initially the reading is invalid until the first interval elapses. An I2C
  failure, incomplete conversion or out-of-range reading invalidates it and
  sets `error`; the next interval retries.

For example:

```sh
curl -H 'Content-Type: application/json' -X POST \
  -d '{"interval_ms":2000,"shunt_ohms":0.1}' \
  http://controller/extensions/current-1/config
curl http://controller/extensions/current-1/readings
```

## WebSocket and MQTT updates

After authenticating when required, subscribe over `/ws` using:

```json
{"jsonrpc":"2.0","id":1,"method":"subscribe","params":{"channel":"extensions.current-1"}}
```

Each sampling attempt sends subscribed clients an `extensions.measurement`
notification whose `params` has the same fields as the HTTP readings. Use
`unsubscribe` with the same channel to stop updates. Subscription itself
does not send a snapshot; poll HTTP if an immediate current value is needed.
When settings or the I2C bus change, an invalid notification indicates either
`waiting for measurement` or `extension unavailable`. Subscribing remains
possible while the sensor is unavailable, so clients can observe a later
reconfiguration. Notifications are best effort and may be dropped for a slow
WebSocket client.

When MQTT is enabled and connected, the same bare readings object is published
to `<topic_base><device_id>/extensions/current-1/readings` after each sampling
attempt, when configuration changes, and on broker connection. Messages are
non-retained, QoS 0; consumers must account for disconnects and missed updates.
Check the `valid` flag and `error` before using readings. No Home Assistant
discovery or individual numeric MQTT topics are created.

The INA219 is configured for 32 V bus and +/-320 mV shunt ranges, with
continuous 12-bit conversions. Bus voltage is measured at the load side
(IN-); current is signed shunt voltage divided by configured resistance.
Power is their product and may be negative. Bus and shunt registers are read
in sequence, not as an atomic snapshot. PWM loads can vary faster than these
conversions, so readings are not a guarantee of instantaneous or averaged
power. Shunt voltage near the measurement limit and bus voltage at 32 V are
rejected. The sensor's supply and common-mode ratings and the shunt's current
and power ratings must be respected independently of software settings.

The webapp Sensors card exposes these settings with numeric current, voltage
and power readings plus a signed current history graph. Home Assistant exposes
native configuration entities and current/voltage/power sensors suitable for
history graphs. Invalid measurements must not be plotted as zeroes.

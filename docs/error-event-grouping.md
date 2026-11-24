# Host error grouping

Native `error` events include an optional `site` field with a source basename and line, for example `windows_utils.cpp:535`. Records without a source attribute carry the fixed identifier `host-log`. Older clients can omit the field. Panel and web events forwarded by the host keep their own error codes and schema.

For native stream logging, the build catalogue uses fixed string operands and a placeholder for each variable operand. A statement such as `get_error_string(result) << " failed to set HDR state!"` contributes the template `{} failed to set HDR state!`. The FFmpeg callback uses the original printf template provided by libavutil. Its site identifies the callback because the library does not provide a source location. Calls that only take opaque values and have no template use their source site for grouping.

The sender runs the existing text scrubber on the template and computes the `e`-prefixed, 32-bit FNV-1a code from the result. The event carries the code and site. It excludes the log text, template and argument values. The sender tallies by component, source site and template so variation in a rendered line cannot consume the table's 40 entries. The Worker continues grouping by component and code. The legacy catalogue still explains older codes.

Microsoft's structured logging also preserves the message template separately from arguments (dotnet-log-template-20261120, “Log message template”). The host keeps its existing C++ Boost.Log records and uses only the scrubbed template for the code. The OpenTelemetry Logs Data Model distinguishes an event name, body and attributes (otel-logs-model-20261120, “Field: EventName” and “Field: Attributes”). An exporter could map this error code and location to structured log attributes.

## Historical UPnP groups

Both codes came from `map_upnp_port` at `src/upnp.cpp:279` in host builds `v2026.1115.214350.杂鱼` (`211966b44`) and `v2026.1116.52231.杂鱼` (`17a75bafa`). That statement writes the protocol and port after “Failed to map” when `UPNP_AddPortMapping` returns a nonzero result, including after the fallback mapping attempt when one is made.

| Code | Old hash input | Meaning |
| --- | --- | --- |
| `e1147faac` | `failed to map udp` | UDP port mapping failed |
| `e857e5618` | `failed to map tcp` | TCP port mapping failed |

The historical grouping stopped at the first digit of the port, so both the port number and the numeric result follow the part that was hashed. Historical group counts include builds from `v2026.1115.214350.杂鱼` through `v2026.1117.111923.杂鱼`. Both saved source samples identify the same statement in `v2026.1116.52231.杂鱼`. The new template groups the UDP and TCP instances of that statement together.

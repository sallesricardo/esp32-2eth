---
name: esp32-2eth
description: >
  Project skill for the esp32-2eth dual-Ethernet ESP32 firmware (two W5500
  modules, MQTT mTLS, TCP proxy to a Doppler/radar host, device config over
  MQTT/NVS). Use when building, flashing, debugging, or changing this repo —
  networking, proxy path, MQTT topics/certs, Kconfig, or component layout.
  Triggers: /esp32-2eth, dual ethernet, W5500, mqtt_app, tcp proxy, doppler.
compatibility: ESP-IDF (IDF_PATH export required). Target default: esp32s3.
---

# esp32-2eth — Dual Ethernet Firmware

ESP-IDF firmware for an ESP32-S3 (production) with **two SPI W5500 Ethernet
modules**: one path for **MQTT (mTLS)** and one for a **TCP proxy** toward a
remote Doppler/radar host. Application logic is split into local components
under `components/`; `main/app_main.c` only wires them together.

Human-facing deep dive: `README.md` (Portuguese). Prefer the README for mTLS
cert steps, Mosquitto ACL, and long-form design rationale. This skill is the
agent playbook for day-to-day work in the tree.

## When to use

- Any change to this repository’s firmware, components, Kconfig, or certs
- Build / flash / monitor / crash analysis on the board
- Questions about dual netif routing, proxy bind, MQTT topics, or NVS config
- Bench testing without W5500 hardware (Wi-Fi backend)

## Architecture (keep this shape)

```
ETH1 (W5500 #1) ──► mqtt_app          (mqtts:// mTLS, config + telemetry)
ETH2 (W5500 #2) ──► tcp_server_app    (local TCP clients, default port 3333)
                 └─► tcp_client_app   (outbound to REMOTE_HOST_IP:PORT)
                          │
              proxy_events (dedicated event loop: copy + queue + process)
              doppler_events (dedicated loop for radar/doppler frames)
                          │
              doppler_processing (routes by radar command -> per-command handler)
```

| Component | Role |
|---|---|
| `eth_w5500` | One W5500 instance: SPI + MAC/PHY + static IP `esp_netif_t*` |
| `mqtt_app` | MQTT client bound to a netif; mTLS PEMs via `EMBED_TXTFILES` |
| `tcp_server_app` | Single-client TCP server; optional bind to a netif IP |
| `tcp_client_app` | TCP client to remote host; reconnect; optional netif bind; framed RX |
| `proxy_events` | Dedicated event loop for proxy payload processing (not default loop) |
| `doppler_events` | Dedicated event loop for Doppler/radar-side processing |
| `doppler_processing` | Routes doppler frames by radar command (event_id) to per-command handlers |
| `device_config` | Raw config blob + CRC32 hash gate in NVS (`dev_cfg`) |
| `main/app_main.c` | Orchestration only: init netifs, wire callbacks, proxy glue |

**Hard rules for changes:**

1. **Do not** put SPI/W5500 knowledge into `mqtt_app` / TCP components. They
   take `esp_netif_t*` (or bind netif) only.
2. **Do not** do heavy work inside network I/O callbacks. Copy +
   `proxy_events_post_data` / `doppler_events_post_data`, process on the
   dedicated loop task. Handlers own the heap buffer and must `free()` it.
3. **JSON over MQTT** uses `cJSON` (`cJSON_PrintUnformatted` → free with
   `cJSON_free`, not `free()`). No hand-rolled `snprintf` JSON.
4. **Config schema is not defined yet.** `device_config` stores raw bytes;
   parse/apply only in the TODOs in `on_mqtt_config_data` / boot load path.
5. **Two SPI hosts required** in production: `CONFIG_ETH1_W5500_SPI_HOST` ≠
   `CONFIG_ETH2_W5500_SPI_HOST`. `app_main` aborts with a clear log if equal.
6. **Proxy bind index** `CONFIG_TCP_PROXY_BIND_ETH_IDX`: `0`=ETH1, `1`=ETH2
   (default), `-1`=lwIP routing (can leak proxy traffic onto the MQTT NIC).

## Layout

```
components/
  eth_w5500/          # generic single-port W5500 driver
  mqtt_app/
    certs/            # embedded PEMs (replace placeholders before prod flash)
  tcp_server_app/
  tcp_client_app/     # remote host / Doppler framing + reconnect
  proxy_events/
  doppler_events/
  doppler_processing/  # per-command routing/processing for doppler frames
  device_config/
main/
  app_main.c          # orchestration + proxy handlers + MQTT bridges
  Kconfig.projbuild   # backend, GPIOs, IPs, MQTT URI, TCP ports, bind idx
scripts/
  generate_certs.sh         # CA + broker + device certs → out/certs/
  generate_client_cert.sh   # extra client cert without rotating CA
mosquitto/                  # broker conf + ACL examples
```

Managed deps (do not hand-edit): `managed_components/` (`espressif/w5500`,
`espressif/mqtt`, `espressif/cjson`, `espressif/wiznet_common`).

## Network backends

Menuconfig → **Configuração Dual Ethernet W5500** → **Backend de rede**:

| Backend | Config | Use |
|---|---|---|
| Dual W5500 (production) | `CONFIG_NETIF_BACKEND_W5500` | Two physical NICs; default product path |
| Wi-Fi STA (bench) | `CONFIG_NETIF_BACKEND_WIFI` | One STA used as both `eth_netif_1` and `eth_netif_2` — exercises app logic only, **not** dual-NIC isolation |

`esp_wifi` stays in `main` `REQUIRES` always (IDF expands `REQUIRES` before
sdkconfig is available). Production builds rely on `#if` + linker GC.

Static IPs / GPIOs / SPI hosts: `CONFIG_ETH1_*` / `CONFIG_ETH2_*` (W5500
backend only). MQTT / TCP / remote host / bind: shared across backends.

## MQTT contract

| Direction | Topic | Notes |
|---|---|---|
| Publish | `device/tcp_server/client_connected` | JSON: `event`, `ip` |
| Publish | `device/tcp_client/data_hex` | JSON: `event`, `hex`, `len` |
| Subscribe | `device/config/set` | Raw payload → `device_config_apply_if_changed` |

- URI: `CONFIG_MQTT_BROKER_URI` (production should be `mqtts://…:8883`).
- Certs embedded from `components/mqtt_app/certs/{ca,client}.{crt,key}`.
- Subscriptions re-issued on every `MQTT_EVENT_CONNECTED`.
- `on_data` runs on the esp-mqtt task — keep it light or re-queue.

Cert workflow (secrets stay local; `out/` is gitignored):

```bash
BROKER_IP=<ip> CLIENT_CN=esp32-device-01 ./scripts/generate_certs.sh
# or multiple SANs:
BROKER_IPS="a.b.c.d,e.f.g.h" CLIENT_CN=esp32-device-01 ./scripts/generate_certs.sh
cp out/certs/ca.crt out/certs/client.crt out/certs/client.key components/mqtt_app/certs/
# Extra clients later — never regenerate CA unless intentional:
CLIENT_CN=pc-app-01 ./scripts/generate_client_cert.sh
```

Broker SAN must include every IP used in `CONFIG_MQTT_BROKER_URI`. ACL example:
`mosquitto/acl.example` (device needs `read` on `device/config/set`).

## Proxy data path

```
tcp_server recv → on_data_from_tcp_client
  → proxy_events_post_data(PROXY_EVENT_DATA_FROM_TCP_CLIENT)
  → handle_data_from_tcp_client_event
       → process_data_from_tcp_client(...)   # implement real logic here
       → tcp_client_app_send(...)            # forward to remote
       → free(data)

tcp_client framed RX → on_data_from_remote_host
  → doppler_events_post_data(command, ...)     # Doppler path, keyed by command
       → doppler_frame_dispatcher (doppler_processing, registered once w/ ESP_EVENT_ANY_ID)
            → looks up `command` in s_handlers[], calls the matching handler
            → free(data)
  → proxy_events_post_data(PROXY_EVENT_DATA_FROM_REMOTE_HOST)
  → handle_data_from_remote_host_event
       → process_data_from_remote_host(...)  # MQTT hex publish lives here
       → tcp_server_app_send(...)            # forward to local client
       → free(data)
```

**Where to implement processing:**
- Proxy (raw passthrough, both directions): `process_data_from_tcp_client` /
  `process_data_from_remote_host` in `main/app_main.c`.
- Doppler (framed radar commands): add a `{command, name, handler}` entry to
  `s_handlers[]` in `components/doppler_processing/doppler_processing.c` —
  do **not** edit `app_main.c` or `doppler_events` for a new command.

Buffers are freed by the owning dispatcher/handler after processing — copy
if you need to keep data.

Queue full / post timeout → drop + warning (never block network tasks
forever). Tune sizes/timeouts in the respective `*_events.c` files.

Limitations: one TCP client per side; data with no peer connected is dropped
(not re-queued); remote host via `getaddrinfo` (IP or DNS if configured).

## Remote / Doppler framing (tcp_client_app)

Outbound connection target: `CONFIG_REMOTE_HOST_IP` / `CONFIG_REMOTE_HOST_PORT`
(menu: **Configuração Socket Doppler (TCP Client)**).

Framed stream (header/footer style used by the radar path):

- Header `0xDB`, footer `0xDC`
- Length (u16 BE), command, frame number, payload, checksum, footer
- Max payload currently `1024` in the client component

When changing the wire format, update parser + TX helpers in
`components/tcp_client_app/` together, and keep `tcp_client_app.h` in sync
with the implementation (callback arity, send return types, public symbols).

`doppler_events` mirrors `proxy_events` (dedicated loop, heap-owned buffers,
timestamp for latency) but keys events by the radar `command` byte
(`event_id` in `doppler_events_post_data`/`doppler_events_register_handler`)
instead of a fixed direction enum. `components/doppler_processing` is the
only place that registers a handler on it (`ESP_EVENT_ANY_ID`, then routes
internally by command) — see "Where to implement processing" above.

## Environment & build

```bash
# This machine (adjust if IDF lives elsewhere):
source /home/ricardo/Data/esp/esp-idf/export.sh   # or: . $IDF_PATH/export.sh
cd /home/ricardo/Projetos/esp32-2eth

idf.py --version
idf.py set-target esp32s3    # only when changing target; default in sdkconfig.defaults
idf.py build
```

- Load the ESP-IDF environment **before** any `idf.py` / `esptool.py` command.
- Prefer incremental `idf.py build`. Use `idf.py fullclean` only after
  cryptic CMake failures or major sdkconfig shifts — do **not** `rm -rf build/`
  without user approval.
- Port: if multiple serial devices, ask which port; never guess.
- After a successful build in a session, if a board is present:
  `idf.py -p <PORT> flash monitor`. If no board, say flash was skipped.

### Hardware identity

```bash
esptool.py chip_id
```

Report together when relevant: project target (`Building ... for target …`),
physical chip (`chip_id`), boot flash/mode lines, app startup metadata.

### Monitor / crashes

Use `idf.py -p <PORT> monitor` (address decoding). Raw serial is worse for
backtraces. Ignore linenoise “escape sequences not supported” — harmless.

## Kconfig cheat sheet

All under `main/Kconfig.projbuild` → menu **Configuração Dual Ethernet W5500**:

- `NETIF_BACKEND_*` — W5500 vs Wi-Fi test
- `TEST_WIFI_SSID` / `TEST_WIFI_PASSWORD` — Wi-Fi backend only
- `ETH1_*` / `ETH2_*` — SPI host, GPIOs, static IP/GW/mask/DNS
- `MQTT_BROKER_URI`
- `TCP_SERVER_PORT` (default 3333)
- `REMOTE_HOST_IP` / `REMOTE_HOST_PORT`
- `TCP_PROXY_BIND_ETH_IDX` (-1..1, default 1)

SPI clock for W5500 is set in `app_main` (40 MHz). Lower if long jumpers
cause bus errors (~20 MHz).

## Coding conventions (this repo)

- Entry point remains `void app_main(void)`.
- Components via `idf_component_register` with explicit `REQUIRES` /
  `PRIV_REQUIRES`. Cannot branch `REQUIRES` on `CONFIG_*` at CMake time.
- Logging: `ESP_LOGI/W/E` with a per-file `TAG`.
- Prefer Portuguese comments matching existing style when extending nearby
  code; public skill/docs for agents may stay English.
- New MQTT topics: extend `mqtt_subscriptions` in `app_main` **and** broker
  ACL (`mosquitto/acl.example` + real `acl.conf`).
- New third Ethernet later: extend `eth_netifs[]` in `app_main` and the
  `TCP_PROXY_BIND_ETH_IDX` range in Kconfig.
- Do not commit secrets: real private keys, filled `out/certs/`, local
  `sdkconfig` with lab passwords. Placeholders under `mqtt_app/certs/` are
  expected until the user copies generated files.

## Common tasks

**Add processing on proxy path** → edit `process_data_*` in `main/app_main.c`.

**Add a radar/doppler command handler** → add `{command, name, handler}` to
`s_handlers[]` in `components/doppler_processing/doppler_processing.c`.

**Add MQTT subscription** → `mqtt_subscriptions[]` + `on_data` dispatch + ACL.

**Change pins / IPs / broker** → `idf.py menuconfig` (or `sdkconfig.defaults`
for shared defaults only).

**Bench without W5500** → backend Wi-Fi + SSID/password; remember single NIC.

**Flash production with mTLS** → generate certs, copy into `mqtt_app/certs/`,
set `mqtts://` URI, rebuild, flash.

## ESP Component Registry (optional)

Public API: `https://components.espressif.com/api/`. No auth.

```
GET /api/components?q={query}&page=1&per_page=10
GET /api/components/{namespace}/{name}
```

Add deps with: `idf.py add-dependency "namespace/component^version"`. If
search returns multiple plausible matches, list them and ask the user.

## Safety / do-nots

- No destructive cleanup (`rm -rf build/`, `fullclean` storms) without approval.
- No force-push or rewriting shared git history unless explicitly requested.
- Do not regenerate the CA casually (`generate_certs.sh` with existing CA
  reuses it for broker re-issue; full CA wipe invalidates embedded device
  certs — call that out before doing it).
- Do not invent a device_config JSON schema; leave TODOs until the user defines it.
- `tmp/` is scratch — not production source of truth.

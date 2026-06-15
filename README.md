# Seedlink_ZejfSeis

**SeedLink → ZejfSeis Real-Time Bridge**

A lightweight C daemon that connects to a remote SeedLink server as a client, decodes incoming MiniSEED records, and re-broadcasts the raw seismic samples in the ZejfSeis protocol format. This allows the [ZejfSeis](https://github.com/xspanger3770/ZejfSeis) visualization software to display real-time seismic waveforms from any SeedLink-compatible source. Tested in **Linux Ubuntu 24.04.4 LTS**.

---

## Overview

```
┌─────────────────────┐        SeedLink v2         ┌───────────────────┐
│  Remote SeedLink    │ ◄────────────────────────► │  seedlink_zejf    │
│  Server             │  HELLO/STATION/SELECT/     │  (this program)   │
│  (e.g. EqCitizen)   │  DATA/END → MiniSEED v2    │                   │
└─────────────────────┘                            └────────┬──────────┘
                                                            │ ZejfSeis protocol
                                                            │ (TCP, port 6222)
                                                   ┌────────▼──────────┐
                                                   │  ZejfSeis Client  │
                                                   │  (Java, desktop)  │
                                                   └───────────────────┘
```

The bridge maintains a persistent connection to the upstream SeedLink server and streams decoded samples to any number of connected ZejfSeis clients simultaneously. If the upstream connection drops, it automatically reconnects after 5 seconds.

---

## Features

- SeedLink v2 client (`HELLO → STATION → SELECT → DATA → END`)
- MiniSEED v2/v3 decoding via **libmseed** (`msr3_parse` with `MSF_UNPACKDATA`)
- Supports integer (`i`), float (`f`), and double (`d`) sample types
- ZejfSeis protocol server (compatibility version 4)
- Keep-alive heartbeat every 2 seconds to prevent client timeouts
- Handles ZejfSeis commands: `realtime`, `getdata`, `datahour_check`, `heartbeat`
- Responds to historical data requests (`getdata`) with an empty block — no disk storage required
- Up to **32 simultaneous ZejfSeis clients**
- Configuration loaded from `data/ranges.json` — no recompilation needed
- Auto-reconnect on upstream disconnection
- Verbose diagnostic logging on stdout

---

## Requirements

| Dependency | Version  | Notes                          |
|------------|----------|-------------------------------|
| GCC        | ≥ 7      | C99/C11 standard              |
| libmseed   | ≥ 3.x    | MiniSEED parsing library      |
| pthreads   | POSIX    | Multi-threading                |
| libm       | POSIX    | Math library                   |

Install libmseed on Debian/Ubuntu/Raspberry Pi OS:
```bash
sudo apt install libmseed-dev
```

Or compile from source: https://github.com/EarthScope/libmseed

---

## Building

```bash
cd source/
gcc -Wall -O2 -o seedlink_zejf seedlink_zejf.c -lm -lpthread -lmseed
```

---

## Configuration

Parameters are read from `data/ranges.json` under the `"seedlink_to_zejf"` key.  
The values shown below are also the compiled-in **fallback defaults** if the key is missing.

```json
{
  "seedlink_to_zejf": {
    "enabled": true,
    "seedlink_host": "seedlink.eqcitizen.org",
    "seedlink_port": 18000,
    "seedlink_network": "XX",
    "seedlink_station": "EQ002",
    "seedlink_channel": "HHZ",
    "zejf_server_port": 6222
  }
}
```

| Field               | Type    | Description                                      |
|---------------------|---------|--------------------------------------------------|
| `seedlink_host`     | string  | Hostname or IP of the upstream SeedLink server   |
| `seedlink_port`     | integer | TCP port of the SeedLink server (default: 18000) |
| `seedlink_network`  | string  | SEED network code (e.g. `XX`)                    |
| `seedlink_station`  | string  | SEED station code (e.g. `EQ002`)                 |
| `seedlink_channel`  | string  | SEED channel code (e.g. `HHZ`)                   |
| `zejf_server_port`  | integer | TCP port to listen on for ZejfSeis clients        |

> **Note:** Configuration is read once at startup. Restart the process to apply changes.

---

## Running

```bash
cd source/
./seedlink_zejf
```

Run from the `source/` directory so that the relative path `../data/ranges.json` resolves correctly.  
Alternatively, wrap it in a systemd service or run it inside a `tmux`/`screen` session.

### Expected startup output

```
Zejf Bridge escuchando en puerto 6222...
[Seedlink Bridge] Conectando a servidor remoto Seedlink seedlink.eqcitizen.org:18000...
[Seedlink Bridge] Conectado a seedlink.eqcitizen.org:18000.
[Seedlink Bridge] Respuesta HELLO: SeedLink v3.0 (EqCitizen/1.0) :: SLPROTO:3.0
[Seedlink Bridge] Enviando: STATION EQ002 XX
[Seedlink Bridge] Respuesta STATION: OK
[Seedlink Bridge] Enviando: SELECT HHZ
[Seedlink Bridge] Respuesta SELECT: OK
[Seedlink Bridge] Enviando: DATA
[Seedlink Bridge] Respuesta DATA: OK
[Seedlink Bridge] Enviando: END
[Seedlink Bridge] Entrando al bucle de lectura de bloques MiniSEED...
[Seedlink Bridge] Header recibido (8 bytes): [SL000000] hex: 53 4c 30 30 30 30 30 30
[Seedlink Bridge] Bloque MiniSEED #1 recibido (512 bytes), procesando...
[Seedlink Bridge] Recibiendo datos de XX_EQ002... (1 bloques procesados, 100 muestras/bloque)
```

---

## SeedLink Protocol Handshake

This program follows the SeedLink v2 command sequence:

```
Client → Server:  HELLO\r\n
Server → Client:  SeedLink v3.0 ...\r\n

Client → Server:  STATION <station> <network>\r\n
Server → Client:  OK\r\n

Client → Server:  SELECT <channel>\r\n
Server → Client:  OK\r\n

Client → Server:  DATA\r\n
Server → Client:  OK\r\n

Client → Server:  END\r\n
Server → Client:  [binary SL packets: 8-byte header + 512-byte MiniSEED each]
```

> **Important:** `DATA` only sets the starting sequence point.  
> It is the **`END`** command that actually triggers the binary streaming.  
> Without `END`, the server waits indefinitely for more configuration commands.

Each SeedLink binary packet consists of:
- **8-byte header**: ASCII `SL` + 6 ASCII hex digits (sequence number)
- **512-byte body**: a standard MiniSEED v2 record

---

## ZejfSeis Protocol

Upon connection, the bridge sends a handshake header to the ZejfSeis client:

```
compatibility_version:4
sample_rate:100
err_value:-2147483648
last_log_id:<unix_timestamp_in_centiseconds>
```

For each batch of decoded seismic samples received from SeedLink, the bridge sends a realtime block:

```
realtime
<sample_value_int32>
<log_id_uint64>
<sample_value_int32>
<log_id_uint64>
...
-2147483648
```

The sentinel value `-2147483648` (`INT32_MIN`, matching `err_value`) marks the **end of each realtime block**.  
A `heartbeat\n` keep-alive message is proactively sent to all clients every 2 seconds to prevent Java-side socket read timeouts.

### ZejfSeis commands handled

| Command          | Response / Action                                          |
|------------------|------------------------------------------------------------|
| `realtime\n`     | Logged; realtime blocks are sent as data arrives           |
| `getdata\n`      | Reads 2 parameter lines, responds `logs\n-2147483648\n` (empty) |
| `datahour_check\n` | Reads 2 parameter lines, silently discarded             |
| `heartbeat\n`    | No response needed; bridge sends heartbeats proactively    |

---

## Architecture

The program uses four POSIX threads:

| Thread                   | Role                                                           |
|--------------------------|----------------------------------------------------------------|
| `seedlink_client_thread` | Connects to SeedLink, reads MiniSEED blocks, calls `process_mseed` |
| `heartbeat_thread`       | Sends `heartbeat\n` to all ZejfSeis clients every 2 seconds   |
| `client_handler` (×N)   | One per connected ZejfSeis client — reads and dispatches commands |
| `main` thread            | Accepts new TCP connections on the ZejfSeis port              |

A single `clients_mutex` (`pthread_mutex_t`) protects the shared `zejf_clients[]` socket array accessed by all threads.

---

## Log ID Synchronization

The `log_id` counter is initialized at startup to `unix_timestamp × 100 + microseconds / 10000` (i.e., centiseconds since epoch). Each decoded sample increments it by 1. This makes the time axis in ZejfSeis correct as long as the bridge starts with a roughly accurate system clock.

---

## Troubleshooting

| Symptom | Likely Cause | Fix |
|---------|-------------|-----|
| `ERROR station not found` | Wrong station/network parameter order or name | Note: SeedLink uses `STATION <station> <network>`, not `<network> <station>` |
| Header hex does not start with `53 4c` (`SL`) | `OK\r\n` response to `DATA` not consumed before reading binary stream | Ensure `recv` is called after `DATA` before sending `END` |
| `recv` blocks indefinitely after `DATA` | `END` command not sent | Verify `END\r\n` is sent after `DATA\r\n` |
| `msr3_parse` fails silently (no samples) | Wrong MiniSEED version or truncated record | Verify libmseed ≥ 3.x; ensure full 512-byte record is received |
| ZejfSeis `NumberFormatException: "heartbeat"` | `heartbeat\n` sent inside a realtime block | All realtime blocks must be closed with `err_value` before heartbeats interleave |
| `Permission denied` compiling binary | Old binary still running | `killall seedlink_zejf` then recompile |
| ZejfSeis `server not compatible with client` | Wrong `compatibility_version` in handshake | Must be `compatibility_version:4` |

---

## License

Part of the [EqCitizen](https://eqcitizen.org) seismic monitoring project.  
See the root repository for license details.

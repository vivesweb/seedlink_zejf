# Seedlink_ZejfSeis V.1.0.0

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

| Field               | Type    | Description                                                |
|---------------------|---------|------------------------------------------------------------|
| `seedlink_host`     | string  | Hostname or IP of the upstream SeedLink server             |
| `seedlink_port`     | integer | TCP port of the SeedLink server (default: 18000)           |
| `seedlink_network`  | string  | SEED network code (e.g. `XX`)                              |
| `seedlink_station`  | string  | SEED station code (e.g. `EQ002`)                           |
| `seedlink_channel`  | string  | SEED channel code (e.g. `HHZ`)                             |
| `zejf_server_port`  | integer | TCP port to listen on for ZejfSeis clients (default: 6222) |

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
Zejf Bridge v.1.0.0 listening on port 6222...
[Seedlink Bridge] Connecting to remote Seedlink server seedlink.eqcitizen.org:18000...
[Seedlink Bridge] Connected to seedlink.eqcitizen.org:18000.
[Seedlink Bridge] Reply HELLO: SeedLink v3.0 (EqCitizen/1.0) :: SLPROTO:3.0
[Seedlink Bridge] Sending: STATION EQ002 XX
[Seedlink Bridge] Reply STATION: OK
[Seedlink Bridge] Sending: SELECT HHZ
[Seedlink Bridge] Reply SELECT: OK
[Seedlink Bridge] Sending: DATA
[Seedlink Bridge] Reply DATA: OK
[Seedlink Bridge] Sending: END
[Seedlink Bridge] Entering MiniSEED block reading loop...
[Seedlink Bridge] Header received (8 bytes): [SL000000] hex: 53 4c 30 30 30 30 30 30
[Seedlink Bridge] MiniSEED block #1 received (512 bytes), processing...
[Seedlink Bridge] Receiving data from XX_EQ002... (1 blocks processed, 100 samples/block)
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

# Production & Security: Reverse Proxy with Nginx

By default, this C service exposes a direct port for communication. However, for production environments, **it is highly recommended not to expose the service's native port directly to the public Internet**. 

Using **Nginx** as a reverse proxy (configured in TCP/UDP `stream` mode) provides several key security and operational benefits:

* **Attack Mitigation:** Allows you to limit simultaneous connections per IP to prevent service saturation or Denial of Service (DoS) attacks.
* **Isolation:** The C binary only needs to listen locally (`127.0.0.1`), significantly reducing the attack surface.
* **Timeout Management:** Safely handles timeouts for inactive or hung connections.
* **Auditing & Logging:** Centralizes access and error logs with an optimized format for monitoring.

### Nginx Configuration Example (`nginx.conf`)

Make sure to place this configuration inside the `stream { ... }` block of your Nginx configuration (and **not** inside the `http` block), as it handles raw TCP traffic for protocols like Seedlink:

```nginx
# ==============================================================================
# Security Configuration for the Service (TCP Proxy)
# ==============================================================================

# Backend definition: The C service runs locally on port 6221
upstream seedlink_zejfseis {
    server 127.0.0.1:6221;
}

# Public secure port for clients (e.g., Swarm, etc.)
server {
    listen 6222;
    
    # Abuse mitigation: Limits to a maximum of 12 simultaneous connections
    # Note: Requires the 'seedlink_conn' zone to be defined in the stream block
    limit_conn seedlink_conn 12;
    
    # Security timeouts
    proxy_connect_timeout 5s;   # Max time to establish a connection with the C backend
    proxy_timeout 600s;         # Max time of inactivity before dropping the connection

    # Dedicated logging for auditing and debugging
    access_log /var/log/nginx/seedlink_zejfseis_access.log seedlink;
    error_log  /var/log/nginx/seedlink_zejfseis_error.log warn;
    
    # Forward the sanitized traffic to the backend
    proxy_pass seedlink_zejfseis;
}
```
---

# Hardening: fail2ban for Repeated Connection Attempts

Nginx's `limit_conn` (above) caps how many *simultaneous* connections an IP can hold, but it does nothing against an IP that repeatedly connects, disconnects, and reconnects (scanning, brute-force probing, or a misbehaving client hammering the port). **fail2ban** closes that gap by watching the Nginx stream logs and temporarily banning IPs that reconnect too often, via firewall rules.

This is not a full tutorial — just the minimum practical setup for this service.

## 1. Confirm the log format includes IP and timestamp

The `seedlink` log format referenced in the Nginx config must include at least the client IP and the connection time. A typical `stream` log format:

```nginx
# In the stream { ... } block, alongside the upstream/server definitions
log_format seedlink '$remote_addr [$time_local] '
                     'status=$status bytes_sent=$bytes_sent '
                     'bytes_received=$bytes_received '
                     'session_time=$session_time';
```

Each new TCP connection/disconnection produces one log line in `/var/log/nginx/seedlink_zejfseis_access.log` with the source IP — this is what fail2ban will parse.

## 2. Install fail2ban

```bash
sudo apt install fail2ban
```

## 3. Create a filter

Create `/etc/fail2ban/filter.d/seedlink-zejfseis.conf`:

```ini
[Definition]
failregex = ^<HOST> \[.*\] status=
ignoreregex =
```

This matches **every** connection from an IP, regardless of status — the goal isn't to detect "errors" (a TCP bridge has no auth to fail), but to detect an abnormally **high rate of connections** from the same source.

## 4. Create the jail

Add to `/etc/fail2ban/jail.local` (create the file if it doesn't exist):

```ini
[seedlink-zejfseis]
enabled  = true
port     = 6222
filter   = seedlink-zejfseis
logpath  = /var/log/nginx/seedlink_zejfseis_access.log
# Ban an IP if it opens more than 10 connections within 60 seconds
maxretry = 10
findtime = 60
# Ban duration: 10 minutes (increase for repeat offenders, see step 6)
bantime  = 600
action   = %(action_mwl)s
```

- `maxretry` / `findtime`: tune based on legitimate client behavior. A ZejfSeis client normally opens **one** long-lived connection — 10 reconnects in 60 seconds is already abnormal and indicates a reconnect loop, scan, or abuse.
- `bantime`: short bans (minutes) are usually enough to break automated reconnect storms without permanently locking out a legitimate user who briefly misconfigured their client.

## 5. Restart fail2ban and verify

```bash
sudo systemctl restart fail2ban
sudo fail2ban-client status seedlink-zejfseis
```

The `status` command shows currently banned IPs and the total ban count. Check `journalctl -u fail2ban` if the jail doesn't seem to be matching lines — the most common issue is the log format not matching `failregex` exactly.

## 6. Escalating bans for repeat offenders (optional)

To increase `bantime` for IPs that get banned repeatedly (e.g. doubling each time), enable the bundled `recidive` jail in `/etc/fail2ban/jail.local`:

```ini
[recidive]
enabled  = true
logpath  = /var/log/fail2ban.log
banaction = %(banaction_allports)s
bantime  = 1w
findtime = 1d
maxretry = 3
```

This bans, for a week, any IP that triggers 3 separate jails (including `seedlink-zejfseis`) within a day.

## 7. Notes specific to this setup

- Since the C binary listens only on `127.0.0.1:6221` and Nginx is the public-facing port, fail2ban must act on the **firewall** (iptables/nftables), not on the Nginx config — `action = %(action_mwl)s` (the default `iptables-multiport` + email-with-whois action) handles this automatically.
- If you run the bridge behind a CDN or another reverse proxy that itself terminates TCP (so `$remote_addr` in Nginx logs is always the proxy's IP, not the real client), fail2ban based on these logs becomes useless — banning the proxy would block everyone. In that case, rate-limiting must happen at that outer layer instead.
- Whitelist your own monitoring/admin IPs in `jail.local` via `ignoreip = 127.0.0.1/8 <your-ip>` to avoid self-locking during testing.

---


Part of the [EqCitizen](https://eqcitizen.org) seismic monitoring project.  
See the root repository for license details.

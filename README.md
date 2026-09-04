# Pulsync ESP32 Library

Non-blocking task scheduler + real-time cloud sync for ESP32.

## Features

- **Scheduler** — `setTimeout`, `setInterval`, `when`, `every` (non-blocking)
- **WiFi Manager** — auto-connect, 3 profiles, captive portal
- **Cloud Transport** — WSS / MQTTS / HTTPS with automatic fallback
- **OTA Updates** — server-triggered firmware updates with rollback
- **Remote Config** — push configuration from server to device
- **Device Enrollment** — pairing code based, automatic token management

## Quick Start

```cpp
#include <Pulsync.h>

void setup() {
  Pulsync.setVersion("1.0.0");        // for OTA (or use -D PULSYNC_FW_VERSION)
  Pulsync.setWiFi("SSID", "password");
  Pulsync.begin("PUL-XXXXXX");

  Pulsync.setInterval([]() {
    Pulsync.send("temperature", analogRead(34) * 0.1);
  }, 5000);
}

void loop() {
  Pulsync.loop();
}
```

## Installation

**PlatformIO:**
```ini
lib_deps = https://github.com/pulsync/pulsync-arduino.git
```

**Arduino IDE:**
Download ZIP from [Releases](https://github.com/pulsync/pulsync-arduino/releases) and add via Library Manager.

## Pointing at Your Server

If you set nothing, the library **auto-discovers** the server: it tries the
hosted cloud (`pulsync.in`) first, then a local server on the LAN
(`pulsync.local` via mDNS). The first one that answers enrollment is used and
remembered; if neither answers, the device reports "no server found" and retries.

To pin a specific server (and skip discovery entirely), call `setServer()`
before `begin()`:

```cpp
Pulsync.setServer("192.168.1.50:3001");   // by IP
Pulsync.setServer("pulsync.local:3001");  // by mDNS name (recommended)
Pulsync.setServer("pulsync.in");          // force hosted
```

Prefer the **mDNS name**. A self-hosted server advertises itself on the LAN as
`pulsync.local`, so the device finds it by name even when DHCP hands the server
a new IP — no re-flashing when the address changes. The library resolves
`.local` names on the device via mDNS automatically; just include the port your
server listens on. A bare IP or an mDNS name is treated as plain HTTP/MQTT; a
domain name is treated as HTTPS/MQTTS.

## Setting the Firmware Version

OTA works by comparing the version your device **reports** against the version
the server **offers**. So every build needs a version, and — the golden rule —
**the version you report must match the `.bin` you actually upload.** Bump it
every time you cut a build you intend to deploy, or OTA logic will misbehave.

Set it one of two ways:

**Compile flag (recommended for PlatformIO)** — in `platformio.ini`:
```ini
build_flags = -D PULSYNC_FW_VERSION=\"1.2.3\"
```

**In code (handy for the Arduino IDE)** — call before `begin()`:
```cpp
void setup() {
  Pulsync.setVersion("1.2.3");   // must be before begin()
  Pulsync.setWiFi("SSID", "password");
  Pulsync.begin("PUL-XXXXXX");
}
```

If both are set, `setVersion()` wins. If neither is set the device reports
`0.0.0` and OTA comparisons won't work — the library warns about this at boot.
You'll see the effective version on the serial log:
```
[PULSYNC] firmware version: 1.2.3
```

### Version ordering

Versions are compared **by their numbers only** — any text is just a label /
note. The first four numbers matter (`major.minor.patch` plus one label
number); a version with an extra number is *newer* than one without:

```
1.2.3  <  1.2.3-dev1  <  1.2.3-dev2  <  1.2.3-dev3
1.2.4  >  1.2.3-dev9        (a real patch bump beats any dev of the prior patch)
1.2.3-rc10  >  1.2.3-rc2    (compared numerically: 10 > 2, not as text)
```

The label text (`dev`, `rc`, `beta`, …) is cosmetic; only its number counts.

## OTA Requires a Dual-App Partition Table

OTA updates flash the new firmware into a **second app slot** and roll back if
the new image fails to boot. This needs a partition table with two app slots
(`ota_0` / `ota_1`) plus an `otadata` partition. **The ESP32 default
single-app table cannot do OTA** — the library detects this at startup and
logs an error, disabling OTA until you fix the partition table.

A ready-to-use table ships with the library at
[`partitions/pulsync_ota.csv`](partitions/pulsync_ota.csv).

**PlatformIO** — copy `pulsync_ota.csv` next to `platformio.ini`, then:
```ini
board_build.partitions = pulsync_ota.csv
```
Any dual-OTA table works too (e.g. the built-in `min_spiffs.csv`).

**Arduino IDE** — Tools → Partition Scheme → pick any scheme with OTA
(e.g. "Minimal SPIFFS (1.9MB APP with OTA)").

If OTA is set up correctly you'll see `OTA module initialized (... ota: ready)`
on the serial log at boot.

## OTA Authenticity (Signed Offers)

An OTA offer tells the device to download and flash a new image. Because a
malicious actor on the network could forge such an offer, the device
**authenticates every offer before downloading** and **verifies the image
after downloading**. Both checks must pass or the update is rejected and the
device stays on its current firmware.

How it works — no key management required on your side:

- The server derives a per-device signing key from the device's enrollment
  token using HKDF-SHA256, then signs a canonical description of the offer
  (`version`, `url`, `checksum`, `size`) with HMAC-SHA256. The offer carries
  the signature and a `sig_alg` tag (`hmac-sha256-v1`).
- The device derives the same key from its own token and recomputes the HMAC.
  A mismatch means the offer didn't come from your server, so it's dropped
  **before any download happens**.
- After the image downloads, the device hashes what it wrote and compares it
  against the signed `checksum`. A mismatch aborts the update and keeps the
  running firmware as the boot partition.

The signing key never leaves the device or server — it's derived from the
token both sides already share, so rotating the token rotates the key
automatically. No certificates or build-time keys to manage.

**Fail-closed by default.** An offer with no signature, an unknown `sig_alg`,
or a bad signature is rejected. For local bring-up against a server that isn't
signing offers yet, you can opt out at build time:

```ini
build_flags = -D PULSYNC_OTA_ALLOW_UNSIGNED=1   ; dev only — do not ship
```

Leave this unset in production. The `sig_alg` field is versioned so hosted
deployments can layer stronger schemes (e.g. asymmetric firmware signing) on
top later without a device-side change.

## Platform Support

- ESP-IDF (native)
- Arduino framework (via PlatformIO)

## Documentation

See [pulsync.in/docs](https://pulsync.in/docs) (coming soon)

## License

MIT

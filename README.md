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

## Platform Support

- ESP-IDF (native)
- Arduino framework (via PlatformIO)

## Documentation

See [docs.pulsync.dev](https://docs.pulsync.dev) (coming soon)

## License

MIT

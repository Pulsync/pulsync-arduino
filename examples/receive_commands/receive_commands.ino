/**
 * Pulsync — Receive Commands Example
 *
 * Demonstrates receiving commands from the server.
 * Commands can arrive via WebSocket or heartbeat response.
 */

#include <Pulsync.h>

#define LED_PIN 2

void setup() {
  Serial.begin(115200);
  pinMode(LED_PIN, OUTPUT);

  Pulsync.setWiFi("YourSSID", "YourPassword");
  Pulsync.begin("PUL-XXXXXX");

  // Handle a boolean command: server sends {"command":"set_led","payload":{"value":true}}
  Pulsync.onBool("set_led", [](bool state) {
    digitalWrite(LED_PIN, state ? HIGH : LOW);
    Serial.printf("LED set to %s\n", state ? "ON" : "OFF");
  });

  // Handle a no-value command (just triggers)
  Pulsync.on("reboot", []() {
    Serial.println("Reboot command received");
    delay(1000);
    ESP.restart();
  });

  // Handle remote config changes (default value disambiguates the overload)
  Pulsync.onConfig("blinkInterval", [](int value) {
    Serial.printf("Blink interval changed to %d ms\n", value);
  }, 1000);
}

void loop() {
  Pulsync.loop();
}

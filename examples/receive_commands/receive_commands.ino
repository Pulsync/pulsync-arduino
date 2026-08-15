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

  // Handle commands from server
  Pulsync.on("set_led", [](JsonObject payload) {
    bool state = payload["value"];
    digitalWrite(LED_PIN, state ? HIGH : LOW);
    Serial.printf("LED set to %s\n", state ? "ON" : "OFF");
  });

  Pulsync.on("reboot", [](JsonObject payload) {
    Serial.println("Reboot command received");
    delay(1000);
    ESP.restart();
  });

  // Handle remote config changes
  Pulsync.onConfig("blinkInterval", [](int value) {
    Serial.printf("Blink interval changed to %d ms\n", value);
  });
}

void loop() {
  Pulsync.loop();
}

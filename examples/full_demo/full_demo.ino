/**
 * Pulsync — Full Demo
 *
 * All features: scheduler, cloud send, receive commands, remote config.
 */

#include <Pulsync.h>

#define LED_PIN 2
#define SENSOR_PIN 34

uint32_t readIntervalMs = 5000;

void setup() {
  Serial.begin(115200);
  pinMode(LED_PIN, OUTPUT);

  // WiFi setup (or let captive portal handle it)
  Pulsync.setWiFi("YourSSID", "YourPassword");

  // Connect to Pulsync cloud
  Pulsync.begin("PUL-XXXXXX");

  // Blink LED to show we're alive
  Pulsync.setInterval([]() {
    digitalWrite(LED_PIN, !digitalRead(LED_PIN));
  }, 500);

  // Send sensor data at configurable interval
  Pulsync.setInterval([]() {
    float value = analogRead(SENSOR_PIN) * 0.1;
    Pulsync.send("sensor_value", value);
  }, readIntervalMs);

  // Handle server commands
  Pulsync.on("set_led", [](JsonObject payload) {
    digitalWrite(LED_PIN, payload["value"] ? HIGH : LOW);
  });

  // Handle remote config updates
  Pulsync.onConfig("readInterval", [](int value) {
    readIntervalMs = value;
    Serial.printf("Read interval updated: %d ms\n", value);
  });
}

void loop() {
  Pulsync.loop();
}

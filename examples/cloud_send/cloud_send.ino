/**
 * Pulsync — Cloud Send Example
 *
 * Demonstrates sending data to the Pulsync server.
 * WiFi + cloud transport required.
 */

#include <Pulsync.h>

void setup() {
  Serial.begin(115200);

  Pulsync.setWiFi("YourSSID", "YourPassword");
  Pulsync.begin("PUL-XXXXXX");  // Your pairing code from portal

  // Send sensor data every 5 seconds
  Pulsync.setInterval([]() {
    float temperature = analogRead(34) * 0.1f;
    Pulsync.send("temperature", temperature);
  }, 5000);

  // Send grouped payload every 10 seconds
  Pulsync.setInterval([]() {
    Pulsync.beginPayload();
    Pulsync.add("temperature", (float)(analogRead(34) * 0.1f));
    Pulsync.add("humidity", (float)(analogRead(35) * 0.05f));
    Pulsync.endPayload();
  }, 10000);
}

void loop() {
  Pulsync.loop();
}

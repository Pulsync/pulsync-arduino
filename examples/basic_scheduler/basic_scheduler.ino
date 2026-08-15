/**
 * Pulsync — Basic Scheduler Example
 *
 * Demonstrates non-blocking setTimeout and setInterval.
 * No network needed — just the scheduler.
 */

#include <Pulsync.h>

void blinkLED() {
  digitalWrite(2, !digitalRead(2));
}

void printUptime() {
  Serial.printf("Uptime: %lu ms\n", millis());
}

void setup() {
  Serial.begin(115200);
  pinMode(2, OUTPUT);

  // Blink LED every 500ms (non-blocking)
  Pulsync.setInterval(blinkLED, 500);

  // Print uptime every 2 seconds
  Pulsync.setInterval(printUptime, 2000);

  // Run once after 5 seconds
  Pulsync.setTimeout([]() {
    Serial.println("5 seconds have passed!");
  }, 5000);
}

void loop() {
  Pulsync.loop();
  // Other code can run here freely — nothing is blocked
}

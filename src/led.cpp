#include "led.h"
#include <Arduino.h>
#include "config.h"
#include "net.h"
#include "sensors.h"

void ledBegin() {
  pinMode(PIN_LED, OUTPUT);
  digitalWrite(PIN_LED, LOW);
}

void ledUpdate(unsigned long now) {
  bool on;
  if (otaActive())                                on = true;
  else if (isnan(sensorsAverage()))               on = (now % 250) < 125;
  else if (netConfig().staEnabled && !netStaConnected()) on = (now % 1000) < 500;
  else                                            on = (now % 2000) < 80;
  digitalWrite(PIN_LED, on ? HIGH : LOW);
}

#pragma once
#include <stdint.h>
#include <string>
using String = std::string;
uint32_t millis();
void delay(uint32_t ms);
constexpr int ADC_11db = 3;
void analogSetPinAttenuation(uint8_t pin, int attenuation);
uint32_t analogReadMilliVolts(uint8_t pin);
constexpr int INPUT = 1;
constexpr int HIGH = 1;
void pinMode(uint8_t pin, int mode);
int digitalRead(uint8_t pin);

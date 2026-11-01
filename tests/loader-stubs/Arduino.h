#pragma once
// Enough of Arduino.h for App/Identity.h and App/Log.h to parse on a host. The
// loader test never constructs a String; it exists because both headers include
// this one.
#include <stdint.h>
#include <string>
using String = std::string;

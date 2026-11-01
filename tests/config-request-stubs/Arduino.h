#pragma once
// Enough of Arduino.h for App/Display.h and App/Log.h to parse on a host.
// App/ConfigRequest.cpp constructs no String; this exists because both headers
// include it, the same way tests/loader-stubs/Arduino.h does.
#include <stdint.h>
#include <string>
using String = std::string;

#pragma once
// Enough of Arduino.h for App/Cards.h to parse on a host.
//
// Cards.h declares hooks returning String and a policy shape holding a few of
// them, and performs no String operation of its own, so the standard string
// answers for all of it - unlike tests/personal-photos-stubs, whose module
// appends an unsigned to one.
#include <stddef.h>
#include <stdint.h>

#include <string>

using String = std::string;

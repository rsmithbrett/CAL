#pragma once
// Enough of Arduino.h for App/DecodeFailureReport.h, which is the whole point of
// that header living apart from Assets.cpp: it names String and nothing else, so
// a host compiler can run the accounting the device runs.
//
// std::string stands in for String. The two agree on length(), +=, == and
// c_str(), which is every operation the header uses - deliberately, since
// indexOf() is Arduino's alone and the substring dedup that used it is the defect
// this test exists for.
#include <stdint.h>

#include <string>

using String = std::string;

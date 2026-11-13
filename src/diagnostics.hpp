#pragma once
#include <chrono>
#include <string>
using DiagnosticClock=std::chrono::steady_clock;
inline std::string Milliseconds(DiagnosticClock::time_point from,DiagnosticClock::time_point to=DiagnosticClock::now()){
    return std::to_string(std::chrono::duration<double,std::milli>(to-from).count());
}

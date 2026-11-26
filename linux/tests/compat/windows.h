// Stand-in for the few COM calls the upstream test programs make around their checks.
#pragma once
#include <chrono>
#include <unistd.h>
using HRESULT = long;
constexpr unsigned COINIT_APARTMENTTHREADED = 2;
inline HRESULT CoInitializeEx(void*, unsigned) { return 0; }
inline void CoUninitialize() {}
#define SUCCEEDED(result) ((result) >= 0)
#define FAILED(result) ((result) < 0)
inline unsigned long GetCurrentProcessId() { return static_cast<unsigned long>(getpid()); }
inline unsigned long long GetTickCount64() {
    return static_cast<unsigned long long>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}

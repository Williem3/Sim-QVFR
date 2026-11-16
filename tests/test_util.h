// Shared helpers of the tests (tests/*.cpp). Everything is in an anonymous namespace: every test is compiled into
// the one test program, sim_qvfr_tests.exe, and each test's file gets its own copy (its own failure count).
// No <windows.h> here: it defines `near` and `far` as nothing, and min/max as macros.
#pragma once

#include <cmath>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>

// kernel32's GetTempPathW, as fileapi.h declares it (without pulling in <windows.h>).
extern "C" __declspec(dllimport) unsigned long __stdcall GetTempPathW(unsigned long nBufferLength, wchar_t* lpBuffer);

namespace {

int failures = 0;

// One line per check ("ok   <what>" or "FAIL <what>"), counted in `failures`.
inline void check(bool ok, const char* what) {
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    failures += ok ? 0 : 1;
}

// The footer ("RESULT: pass" or "RESULT: FAIL") and the test's exit code.
inline int test_result() {
    std::printf("RESULT: %s\n", failures ? "FAIL" : "pass");
    return failures ? 1 : 0;
}

// a within eps of b (eps included). Not `near`: <windows.h> defines that as nothing.
inline bool close_to(double a, double b, double eps) { return std::fabs(a - b) <= eps; }

// A file's bytes as they are (binary: no newline translation); empty when it can't be read.
inline std::string read_text(const std::wstring& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), {});
}

// The temp folder (GetTempPathW, ending in a backslash) followed by `name`.
inline std::wstring temp_path(const wchar_t* name) {
    wchar_t temp[260] = {};  // MAX_PATH
    GetTempPathW(260, temp);
    return std::wstring(temp) + name;
}

}  // namespace

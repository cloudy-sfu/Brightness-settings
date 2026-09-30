#include "debug_log.h"

#include <shellapi.h>

#include <cstdarg>
#include <cstdio>
#include <mutex>
#include <string>

namespace {

std::mutex g_LogMutex;
FILE* g_LogFile = nullptr;

std::wstring GetLogPath() {
    std::wstring path(MAX_PATH, L'\0');
    for (;;) {
        const DWORD length = GetModuleFileNameW(
            nullptr, path.data(), static_cast<DWORD>(path.size()));
        if (length == 0) return L"Brightness-debug.log";
        if (length < path.size()) {
            path.resize(length);
            break;
        }
        path.resize(path.size() * 2);
    }

    const size_t separator = path.find_last_of(L"\\/");
    path.resize(separator == std::wstring::npos ? 0 : separator + 1);
    return path + L"Brightness-debug.log";
}

} // namespace

bool HasDebugArgument() {
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv) return false;

    bool found = false;
    for (int i = 1; i < argc; ++i) {
        if (_wcsicmp(argv[i], L"debug") == 0) {
            found = true;
            break;
        }
    }
    LocalFree(argv);
    return found;
}

void InitializeDebugLog() {
    std::lock_guard<std::mutex> lock(g_LogMutex);
    if (g_LogFile) return;
    g_LogFile = _wfopen(GetLogPath().c_str(), L"a");
}

void ShutdownDebugLog() {
    std::lock_guard<std::mutex> lock(g_LogMutex);
    if (g_LogFile) {
        std::fclose(g_LogFile);
        g_LogFile = nullptr;
    }
}

bool IsDebugLogEnabled() {
    std::lock_guard<std::mutex> lock(g_LogMutex);
    return g_LogFile != nullptr;
}

void DebugLog(const char* format, ...) {
    std::lock_guard<std::mutex> lock(g_LogMutex);
    if (!g_LogFile) return;

    SYSTEMTIME time;
    GetLocalTime(&time);
    std::fprintf(
        g_LogFile,
        "%04u-%02u-%02u %02u:%02u:%02u.%03u [%lu] ",
        time.wYear,
        time.wMonth,
        time.wDay,
        time.wHour,
        time.wMinute,
        time.wSecond,
        time.wMilliseconds,
        GetCurrentThreadId());

    va_list args;
    va_start(args, format);
    std::vfprintf(g_LogFile, format, args);
    va_end(args);

    std::fputc('\n', g_LogFile);
    std::fflush(g_LogFile);
}

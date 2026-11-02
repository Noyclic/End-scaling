#include <Windows.h>
#include <DbgHelp.h>

#include <atomic>
#include <cwchar>
#include <cstdint>
#include <iterator>

#pragma comment(lib, "Dbghelp.lib")

namespace {
std::atomic_flag g_dumpInProgress = ATOMIC_FLAG_INIT;
std::atomic<std::uint64_t> g_stageAndHresult{0};
std::atomic<DWORD> g_contextThread{0};

LONG WINAPI write_crash_dump(EXCEPTION_POINTERS* exceptionInfo) noexcept {
    if (g_dumpInProgress.test_and_set(std::memory_order_relaxed)) {
        return EXCEPTION_EXECUTE_HANDLER;
    }

    wchar_t localAppData[MAX_PATH]{};
    const DWORD rootLength = GetEnvironmentVariableW(
        L"LOCALAPPDATA", localAppData, static_cast<DWORD>(std::size(localAppData)));
    if (rootLength == 0 || rootLength >= std::size(localAppData)) {
        return EXCEPTION_EXECUTE_HANDLER;
    }

    wchar_t directory[MAX_PATH]{};
    if (swprintf_s(directory, L"%s\\EndScaling\\crashes", localAppData) < 0) {
        return EXCEPTION_EXECUTE_HANDLER;
    }

    // Create each directory separately; ERROR_ALREADY_EXISTS is harmless.
    wchar_t parent[MAX_PATH]{};
    if (swprintf_s(parent, L"%s\\EndScaling", localAppData) < 0) {
        return EXCEPTION_EXECUTE_HANDLER;
    }
    CreateDirectoryW(parent, nullptr);
    CreateDirectoryW(directory, nullptr);

    SYSTEMTIME now{};
    GetLocalTime(&now);
    const DWORD processId = GetCurrentProcessId();
    const DWORD threadId = GetCurrentThreadId();

    wchar_t baseName[128]{};
    if (swprintf_s(baseName, L"EndScaling-%04u%02u%02u-%02u%02u%02u-%lu",
                   now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute,
                   now.wSecond, static_cast<unsigned long>(processId)) < 0) {
        return EXCEPTION_EXECUTE_HANDLER;
    }

    wchar_t dumpPath[MAX_PATH]{};
    wchar_t logPath[MAX_PATH]{};
    if (swprintf_s(dumpPath, L"%s\\%s.dmp", directory, baseName) < 0 ||
        swprintf_s(logPath, L"%s\\%s.txt", directory, baseName) < 0) {
        return EXCEPTION_EXECUTE_HANDLER;
    }

    const DWORD exceptionCode = exceptionInfo && exceptionInfo->ExceptionRecord
        ? exceptionInfo->ExceptionRecord->ExceptionCode : 0;
    const void* exceptionAddress = exceptionInfo && exceptionInfo->ExceptionRecord
        ? exceptionInfo->ExceptionRecord->ExceptionAddress : nullptr;

    const std::uint64_t context = g_stageAndHresult.load(std::memory_order_acquire);
    const std::uint32_t stage = static_cast<std::uint32_t>(context >> 32);
    const std::uint32_t detail = static_cast<std::uint32_t>(context & 0xffffffffu);
    const DWORD contextThread = g_contextThread.load(std::memory_order_acquire);

    wchar_t details[768]{};
    const int detailLength = swprintf_s(
        details,
        L"End-scaling unhandled exception\r\n"
        L"Time: %04u-%02u-%02u %02u:%02u:%02u\r\n"
        L"Process ID: %lu\r\nThread ID: %lu\r\n"
        L"Exception code: 0x%08lX\r\nException address: %p\r\n"
        L"Last native stage: %lu\r\nLast stage HRESULT/detail: 0x%08lX\r\n"
        L"Last stage thread ID: %lu\r\n",
        now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond,
        static_cast<unsigned long>(processId),
        static_cast<unsigned long>(threadId),
        static_cast<unsigned long>(exceptionCode), exceptionAddress,
        static_cast<unsigned long>(stage), static_cast<unsigned long>(detail),
        static_cast<unsigned long>(contextThread));

    if (detailLength > 0) {
        HANDLE logFile = CreateFileW(
            logPath, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS,
            FILE_ATTRIBUTE_NORMAL, nullptr);
        if (logFile != INVALID_HANDLE_VALUE) {
            DWORD written = 0;
            WriteFile(logFile, details,
                      static_cast<DWORD>(detailLength * sizeof(wchar_t)),
                      &written, nullptr);
            CloseHandle(logFile);
        }
    }

    HANDLE dumpFile = CreateFileW(
        dumpPath, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL, nullptr);
    if (dumpFile != INVALID_HANDLE_VALUE) {
        MINIDUMP_EXCEPTION_INFORMATION exceptionData{};
        exceptionData.ThreadId = threadId;
        exceptionData.ExceptionPointers = exceptionInfo;
        exceptionData.ClientPointers = FALSE;

        MINIDUMP_TYPE dumpType = static_cast<MINIDUMP_TYPE>(
            MiniDumpNormal | MiniDumpWithThreadInfo);
        MiniDumpWriteDump(
            GetCurrentProcess(), processId, dumpFile, dumpType,
            exceptionInfo ? &exceptionData : nullptr, nullptr, nullptr);
        CloseHandle(dumpFile);
    }

    return EXCEPTION_EXECUTE_HANDLER;
}
} // namespace

extern "C" void __cdecl es_install_crash_diagnostics() noexcept {
    SetUnhandledExceptionFilter(write_crash_dump);
    g_contextThread.store(GetCurrentThreadId(), std::memory_order_relaxed);
    g_stageAndHresult.store((static_cast<std::uint64_t>(1) << 32), std::memory_order_release);
}

extern "C" void __cdecl es_crash_set_context(
    std::uint32_t stage, std::int32_t detail) noexcept {
    g_contextThread.store(GetCurrentThreadId(), std::memory_order_relaxed);
    const std::uint64_t packed =
        (static_cast<std::uint64_t>(stage) << 32) |
        static_cast<std::uint32_t>(detail);
    g_stageAndHresult.store(packed, std::memory_order_release);
}

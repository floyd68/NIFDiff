// AppIpc.cpp - ported from FICture2's AppIpc.cpp with a NIFDiff-specific
// pipe name and nifdoc-logger logging; the wire protocol and the
// retry/budget behavior are kept identical (see AppIpc.h).
#include "AppIpc.h"
#include "../core/NifLog.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <chrono>
#include <cstdint>
#include <thread>
#include <vector>

namespace
{
    constexpr wchar_t kPipeName[] = L"\\\\.\\pipe\\NIFDiff_IPC";
    constexpr std::uint32_t kProtocolVersion = 1;

    bool WriteAll(HANDLE h, const void* data, DWORD bytes)
    {
        const std::uint8_t* p = static_cast<const std::uint8_t*>(data);
        DWORD remaining = bytes;
        while (remaining > 0)
        {
            DWORD written = 0;
            if (!WriteFile(h, p, remaining, &written, nullptr))
                return false;
            p += written;
            remaining -= written;
        }
        return true;
    }

    bool ReadAll(HANDLE h, void* data, DWORD bytes)
    {
        std::uint8_t* p = static_cast<std::uint8_t*>(data);
        DWORD remaining = bytes;
        while (remaining > 0)
        {
            DWORD read = 0;
            if (!ReadFile(h, p, remaining, &read, nullptr))
                return false;
            if (read == 0)
                return false;
            p += read;
            remaining -= read;
        }
        return true;
    }

    bool HandleOneClient(HANDLE pipe, const std::function<AppIpc::Decision(const std::wstring&)>& onRequest)
    {
        std::uint32_t version = 0;
        std::uint32_t payloadBytes = 0;
        if (!ReadAll(pipe, &version, sizeof(version)) || !ReadAll(pipe, &payloadBytes, sizeof(payloadBytes)))
            return false;

        // Cap the payload to something sane for "one file path" so a
        // malformed/hostile client cannot make the server allocate wildly.
        constexpr std::uint32_t kMaxPayloadBytes = 64 * 1024;
        if (version != kProtocolVersion || payloadBytes == 0 || payloadBytes > kMaxPayloadBytes ||
            (payloadBytes % sizeof(wchar_t)) != 0)
        {
            return false;
        }

        std::vector<wchar_t> buf(payloadBytes / sizeof(wchar_t));
        if (!ReadAll(pipe, buf.data(), payloadBytes))
            return false;

        // Ensure null-termination (client sends a null-terminated string).
        if (buf.empty() || buf.back() != L'\0')
            buf.push_back(L'\0');

        const std::wstring path(buf.data());
        AppIpc::Decision decision = AppIpc::Decision::Ignore;
        if (onRequest)
            decision = onRequest(path);

        const std::uint32_t resp = static_cast<std::uint32_t>(decision);
        return WriteAll(pipe, &resp, sizeof(resp));
    }
}

namespace AppIpc
{
    void StartServer(const std::function<Decision(const std::wstring&)>& onRequest)
    {
        NIFLOG_INFO("[IPC] StartServer: launching named-pipe server thread.");
        std::thread([onRequest]()
        {
            for (;;)
            {
                HANDLE pipe = CreateNamedPipeW(
                    kPipeName,
                    PIPE_ACCESS_DUPLEX,
                    PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
                    PIPE_UNLIMITED_INSTANCES,
                    64 * 1024,
                    64 * 1024,
                    0,
                    nullptr);

                if (pipe == INVALID_HANDLE_VALUE)
                {
                    NIFLOG_ERROR("[IPC] Server: CreateNamedPipeW failed (err={}), retrying in 250ms.", GetLastError());
                    Sleep(250);
                    continue;
                }

                const BOOL ok = ConnectNamedPipe(pipe, nullptr) ? TRUE : (GetLastError() == ERROR_PIPE_CONNECTED);
                if (!ok)
                {
                    NIFLOG_WARN("[IPC] Server: ConnectNamedPipe failed (err={}), discarding pipe.", GetLastError());
                    CloseHandle(pipe);
                    continue;
                }

                (void)HandleOneClient(pipe, onRequest);

                FlushFileBuffers(pipe);
                DisconnectNamedPipe(pipe);
                CloseHandle(pipe);
            }
        }).detach();
    }

    bool TrySendPath(const std::wstring& path, Decision& outDecision)
    {
        outDecision = Decision::Ignore;

        if (path.empty())
            return false;

        // Connect to the server pipe with retry logic.
        //
        // Two distinct failure modes (see FICture2's original for the full
        // rationale):
        //   ERROR_FILE_NOT_FOUND - pipe not yet created (server thread still
        //                          starting); WaitNamedPipeW returns
        //                          immediately for this, so poll instead.
        //   ERROR_PIPE_BUSY      - pipe exists but occupied; WaitNamedPipeW
        //                          blocks until an instance frees up.
        constexpr DWORD kTotalBudgetMs = 500;
        constexpr DWORD kPollIntervalMs = 5;

        const auto deadline = std::chrono::steady_clock::now()
                            + std::chrono::milliseconds(kTotalBudgetMs);
        int retryCount = 0;

        HANDLE h = INVALID_HANDLE_VALUE;
        while (std::chrono::steady_clock::now() < deadline)
        {
            h = CreateFileW(kPipeName, GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (h != INVALID_HANDLE_VALUE)
                break;

            const DWORD err = GetLastError();
            if (err == ERROR_FILE_NOT_FOUND)
            {
                ++retryCount;
                Sleep(kPollIntervalMs);
                continue;
            }
            if (err == ERROR_PIPE_BUSY)
            {
                const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
                    deadline - std::chrono::steady_clock::now()).count();
                if (remaining <= 0 || !WaitNamedPipeW(kPipeName, static_cast<DWORD>(remaining)))
                {
                    NIFLOG_WARN("[IPC] Client: WaitNamedPipeW timed out (err={}).", GetLastError());
                    break;
                }
                continue;
            }
            NIFLOG_ERROR("[IPC] Client: pipe connect failed (err={}).", err);
            break;
        }

        if (h == INVALID_HANDLE_VALUE)
        {
            if (retryCount > 0)
                NIFLOG_WARN("[IPC] Client: server pipe not available after {}ms ({} retries).",
                    kTotalBudgetMs, retryCount);
            return false;
        }

        const std::wstring payload = path + L'\0';
        const std::uint32_t version = kProtocolVersion;
        const std::uint32_t payloadBytes = static_cast<std::uint32_t>(payload.size() * sizeof(wchar_t));

        bool ok = true;
        ok = ok && WriteAll(h, &version, sizeof(version));
        ok = ok && WriteAll(h, &payloadBytes, sizeof(payloadBytes));
        ok = ok && WriteAll(h, payload.data(), payloadBytes);

        std::uint32_t resp = 0;
        ok = ok && ReadAll(h, &resp, sizeof(resp));

        CloseHandle(h);

        if (!ok)
        {
            NIFLOG_ERROR("[IPC] Client: pipe I/O failed during send/recv.");
            return false;
        }

        outDecision = static_cast<Decision>(resp);
        NIFLOG_INFO("[IPC] Client: server responded with decision={}.", static_cast<int>(outDecision));
        return true;
    }
}

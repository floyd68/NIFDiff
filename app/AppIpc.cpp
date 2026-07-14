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
#include <future>
#include <thread>
#include <vector>

namespace
{
    constexpr wchar_t kPipeName[] = L"\\\\.\\pipe\\NIFDiff_IPC";
    constexpr std::uint32_t kProtocolVersion = 2; // v2: Waiting ack precedes the final Decision
    // Distinct from every Decision value; tells the client "request accepted,
    // final decision follows - keep waiting".
    constexpr std::uint32_t kWaitingAck = 0xFFFFFFFFu;

    // Staged client hard timeouts + the server keep-alive cadence that feeds
    // stage 3 (see AppIpc.h's protocol comment for the full ladder).
    constexpr DWORD kRequestResponseTimeoutMs = 500; // request sent -> first server message
    constexpr DWORD kPostAckTimeoutMs = 2000;        // silence allowed after each Waiting ack
    constexpr DWORD kKeepAliveIntervalMs = 1000;     // server refreshes the ack this often
    // The client's total patience: it processes at most this many Waiting
    // acks (~kMaxWaitingAcks seconds of parking); one more ack means the
    // server is alive but not deciding - the client stops waiting and
    // handles the file itself as its own instance. The server-side decision
    // is a queue-or-decline check (see NIFDiffApp's callback), so a healthy
    // server answers after the first ack; this bound only trips when the
    // primary instance is wedged.
    constexpr int kMaxWaitingAcks = 3;

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

    // Client-side deadline-bounded I/O: the client opens its pipe handle
    // FILE_FLAG_OVERLAPPED so every read/write carries a hard timeout - a
    // plain blocking ReadFile could hang forever on a frozen server, which
    // is exactly what the staged client timeouts exist to prevent. Handles
    // partial transfers (byte-mode pipe) by looping under one deadline.
    bool OverlappedIo(HANDLE h, HANDLE event, bool isWrite, void* data, DWORD bytes, DWORD timeoutMs)
    {
        std::uint8_t* p = static_cast<std::uint8_t*>(data);
        DWORD remaining = bytes;
        const ULONGLONG deadline = GetTickCount64() + timeoutMs;
        while (remaining > 0)
        {
            OVERLAPPED ov {};
            ov.hEvent = event;
            ResetEvent(event);
            const BOOL started = isWrite
                ? WriteFile(h, p, remaining, nullptr, &ov)
                : ReadFile(h, p, remaining, nullptr, &ov);
            if (!started && GetLastError() != ERROR_IO_PENDING)
                return false;

            const ULONGLONG now = GetTickCount64();
            const DWORD waitMs = now < deadline ? static_cast<DWORD>(deadline - now) : 0;
            if (WaitForSingleObject(event, waitMs) != WAIT_OBJECT_0)
            {
                // Hard timeout. Cancel and wait for the cancellation to
                // complete so `ov` may safely leave scope.
                CancelIoEx(h, &ov);
                DWORD ignored = 0;
                GetOverlappedResult(h, &ov, &ignored, TRUE);
                return false;
            }

            DWORD transferred = 0;
            if (!GetOverlappedResult(h, &ov, &transferred, FALSE) || transferred == 0)
                return false;
            p += transferred;
            remaining -= transferred;
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

        // Ack receipt before consulting the app. onRequest is a fast
        // queue-or-decline check, so the decision normally follows this
        // first ack within milliseconds; the keep-alive loop below only
        // matters if the app callback stalls, and even then the client's
        // own 3-ack bound caps how long it stays parked.
        const std::uint32_t ack = kWaitingAck;
        if (!WriteAll(pipe, &ack, sizeof(ack)))
            return false;

        AppIpc::Decision decision = AppIpc::Decision::Ignore;
        if (onRequest)
        {
            std::future<AppIpc::Decision> pending = std::async(std::launch::async,
                [&onRequest, &path] { return onRequest(path); });
            while (pending.wait_for(std::chrono::milliseconds(kKeepAliveIntervalMs)) !=
                   std::future_status::ready)
            {
                if (!WriteAll(pipe, &ack, sizeof(ack)))
                {
                    // Client gave up or died. Let the app callback finish
                    // (bounded by its own response budget) and drop the
                    // result - there is nobody left to answer.
                    pending.wait();
                    return false;
                }
            }
            decision = pending.get();
        }

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

                // Per-client worker: HandleOneClient can block for the app
                // callback's whole response budget, and consecutive Explorer
                // opens connect faster than that - serving them serially
                // would let a later client's 500ms connect budget expire
                // while an earlier request is still waiting on the UI.
                std::thread([pipe, onRequest]()
                {
                    (void)HandleOneClient(pipe, onRequest);

                    FlushFileBuffers(pipe);
                    DisconnectNamedPipe(pipe);
                    CloseHandle(pipe);
                }).detach();
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
            // FILE_FLAG_OVERLAPPED: all pipe I/O below runs through
            // OverlappedIo so each stage carries its own hard timeout.
            h = CreateFileW(kPipeName, GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                            OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
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

        HANDLE ioEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (ioEvent == nullptr)
        {
            CloseHandle(h);
            return false;
        }

        std::wstring payload = path + L'\0';
        std::uint32_t version = kProtocolVersion;
        std::uint32_t payloadBytes = static_cast<std::uint32_t>(payload.size() * sizeof(wchar_t));

        // Stage 2: the whole request plus the first server message must fit
        // in kRequestResponseTimeoutMs each - a server that accepted the
        // connection but never acks is not actually serving.
        bool ok = true;
        ok = ok && OverlappedIo(h, ioEvent, true, &version, sizeof(version), kRequestResponseTimeoutMs);
        ok = ok && OverlappedIo(h, ioEvent, true, &payloadBytes, sizeof(payloadBytes), kRequestResponseTimeoutMs);
        ok = ok && OverlappedIo(h, ioEvent, true, payload.data(), payloadBytes, kRequestResponseTimeoutMs);

        std::uint32_t resp = 0;
        ok = ok && OverlappedIo(h, ioEvent, false, &resp, sizeof(resp), kRequestResponseTimeoutMs);

        // Stage 3: parked on Waiting acks, doubly bounded. Silence past
        // kPostAckTimeoutMs after an ack means the server froze or died;
        // more than kMaxWaitingAcks acks means it is alive but not deciding
        // (a healthy server's queue-or-decline check answers right after
        // the first ack). Either way: stop waiting, run standalone.
        int acksSeen = 0;
        while (ok && resp == kWaitingAck)
        {
            if (++acksSeen > kMaxWaitingAcks)
            {
                NIFLOG_WARN("[IPC] Client: server still undecided after {} Waiting acks - proceeding alone.",
                    kMaxWaitingAcks);
                ok = false;
                break;
            }
            ok = OverlappedIo(h, ioEvent, false, &resp, sizeof(resp), kPostAckTimeoutMs);
        }

        CloseHandle(ioEvent);
        CloseHandle(h);

        if (!ok)
        {
            NIFLOG_ERROR("[IPC] Client: pipe I/O failed or timed out during send/recv.");
            return false;
        }

        outDecision = static_cast<Decision>(resp);
        NIFLOG_INFO("[IPC] Client: server responded with decision={}.", static_cast<int>(outDecision));
        return true;
    }
}

// IpcOpenRequest.h - the shared gate + queue between the IPC worker threads
// and the UI thread for single-instance "open this .nif" forwarding.
//
// Earlier protocol shape (FICture2's IpcCompareRequest port): the IPC worker
// marshaled each request onto the UI thread and PARKED its client on Waiting
// acks until the UI thread had actually loaded the file - so several
// back-to-back Explorer opens serialized behind multi-second NIF loads and
// tripped the clients' post-ack timeouts. Now the accept/reject decision
// (same-file-name gate + pane capacity) is taken directly on the IPC worker
// thread against this mutex-protected snapshot, the accepted path is queued,
// and the client is answered immediately; the UI thread drains the queue
// whenever it gets around to it (CMD_NIFDIFF_IPC_OPEN broadcast, plus one
// explicit drain right after startup for requests that arrived while the
// window did not exist yet).
#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cwctype>
#include <deque>
#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

namespace nsk
{

// Broadcast command id carried through FD2D::Backplate::WM_FD2D_BROADCAST.
// No payload anymore - it just tells the view "drain the IPC open queue".
inline constexpr UINT CMD_NIFDIFF_IPC_OPEN = WM_APP + 0x7A21;

struct IpcOpenQueue
{
    // The forward-into-a-pane behavior exists for one workflow: comparing
    // the SAME file name from different folders side by side (e.g. two
    // mods' versions of door.nif). An incoming path is accepted only when
    // its file name matches a document already open (or already accepted
    // into the queue); an empty, fully started viewer accepts any file.
    // Anything else is unrelated work and belongs in its own window.
    //
    // Called on an IPC worker thread. Returns whether the path was queued
    // (client is told OpenedInPane and exits) or declined (client proceeds
    // as its own instance).
    bool TryEnqueue(const std::wstring& path, std::size_t maxPanes)
    {
        const std::wstring name = FileNameLower(path);
        if (name.empty())
            return false;

        std::lock_guard<std::mutex> lock(mutex);
        if (shuttingDown)
            return false;

        bool match = false;
        if (openNamesLower.empty() && pending.empty())
        {
            // uiReady distinguishes "empty because nothing is loaded" (a
            // fine home for any file) from "empty because the primary
            // instance is still booting and may be about to restore a
            // session" - during that sub-second window an unmatchable
            // request is declined rather than parked (the old design's
            // wait-for-the-window is exactly what this queue removed).
            match = uiReady;
        }
        else
        {
            for (const std::wstring& open : openNamesLower)
                match = match || open == name;
            for (const std::wstring& queuedPath : pending)
                match = match || FileNameLower(queuedPath) == name;
        }
        if (!match)
            return false;

        // Capacity: every accepted-but-unprocessed path will consume a pane
        // slot when the UI drains the queue - do not promise more than the
        // view can hold. (The drain still has a spawn-a-new-instance
        // fallback for races; this check keeps that path exceptional.)
        if (loadedCount + pending.size() >= maxPanes)
            return false;

        pending.push_back(path);
        return true;
    }

    // Called once before the IPC server starts, with the primary instance's
    // own command-line files: an Explorer multi-select launches several
    // processes at once, and the losers' forwards must match against what
    // this instance is ABOUT to load, before the first document-change
    // snapshot lands. Overwritten by the real snapshot as the loads finish.
    void SeedExpected(const std::vector<std::wstring>& paths)
    {
        std::lock_guard<std::mutex> lock(mutex);
        openNamesLower.clear();
        for (const std::wstring& p : paths)
        {
            std::wstring name = FileNameLower(p);
            if (!name.empty())
                openNamesLower.push_back(std::move(name));
        }
        loadedCount = openNamesLower.size();
    }

    void MarkUiReady()
    {
        std::lock_guard<std::mutex> lock(mutex);
        uiReady = true;
    }

    void MarkShuttingDown()
    {
        std::lock_guard<std::mutex> lock(mutex);
        shuttingDown = true;
    }

    static std::wstring FileNameLower(const std::wstring& path)
    {
        std::wstring name = std::filesystem::path(path).filename().wstring();
        for (wchar_t& c : name)
            c = static_cast<wchar_t>(std::towlower(c));
        return name;
    }

    std::mutex mutex;
    // Snapshot of the view's loaded documents (lower-cased file names +
    // count), refreshed by the UI thread on every document change - see
    // NifCompareView::UpdateIpcOpenSnapshot.
    std::vector<std::wstring> openNamesLower;
    std::size_t loadedCount = 0;
    // Accepted paths awaiting the UI thread's drain, oldest first.
    std::deque<std::wstring> pending;
    bool uiReady = false;      // initial cmdline/session load finished
    bool shuttingDown = false; // window tear-down began - decline everything
};

} // namespace nsk

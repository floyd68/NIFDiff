// AppIpc.h - single-instance IPC, ported from FICture2's AppIpc (named-pipe
// server + client). A second NIFDiff instance launched with a .nif path
// (e.g. from an Explorer file association) forwards that path to the running
// instance, which loads it into a compare pane WHEN the file name matches a
// document already open there (comparing two mods' versions of the same
// mesh - see NifCompareView::AcceptsIpcOpen); otherwise the running instance
// answers Ignore and the new process opens its own window.
//
// Protocol v2 (FICture2's v1 plus a Waiting ack, distinct pipe name):
//   client -> server : u32 protocol version, u32 payload byte count,
//                      UTF-16 null-terminated path
//   server -> client : u32 Waiting ack (sent immediately on receipt,
//                      repeated every second while the request is pending),
//                      then u32 Decision
//
// The server-side decision is designed to be immediate: the onRequest
// callback only checks the file name against the primary instance's
// open-documents snapshot and queues the path for the UI thread (see
// NIFDiffApp.cpp / ui/IpcOpenRequest.h) - it never waits for the UI to
// actually load anything. The Waiting acks therefore only bridge server
// hiccups, not multi-second NIF loads.
//
// The client never trusts the server unconditionally - it applies staged
// hard bounds so no unpredictable server state can hang it:
//   1. No single-instance mutex -> no IPC at all, run standalone
//      (the caller's check in NIFDiffApp.cpp).
//   2. Pipe connect + first server message: 500ms each. No Waiting ack in
//      time means the server is not actually serving - run standalone.
//   3. After each Waiting ack: 2s of silence allowed (the server refreshes
//      the ack every second), AND at most 3 Waiting acks total. Tripping
//      either bound means the server froze or is wedged mid-decision - the
//      client stops waiting and handles the file itself, standalone.
#pragma once

#include <functional>
#include <string>

namespace AppIpc
{
    enum class Decision : unsigned long
    {
        Ignore = 0,       // request not handled - caller should run as a new instance
        OpenedInPane = 1, // running instance loaded the file into a pane - caller should exit
    };

    // Starts a lightweight named-pipe server in a background thread.
    // Incoming requests contain a single UTF-16 file path. The callback runs
    // on a per-client worker thread (so one slow request cannot starve the
    // accept loop when several instances launch back-to-back) and is
    // expected to decide quickly (queue-or-decline); if it does stall, the
    // Waiting acks keep the client parked only up to the client's own
    // 3-ack bound. Returns whether the request was handled.
    void StartServer(const std::function<Decision(const std::wstring&)>& onRequest);

    // Tries to send a single file path to an existing server and receive a
    // decision. Returns false if the server is not available (no pipe /
    // connection error). Blocks past the connect stage until the server's
    // final Decision (bounded by the server-side response budget).
    bool TrySendPath(const std::wstring& path, Decision& outDecision);
}

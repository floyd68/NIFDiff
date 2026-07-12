// AppIpc.h - single-instance IPC, ported from FICture2's AppIpc (named-pipe
// server + client). A second NIFDiff instance launched with a .nif path
// (e.g. from an Explorer file association) forwards that path to the running
// instance, which loads it into a compare pane instead of opening another
// window - the natural behavior for a compare tool.
//
// Protocol (identical to FICture2's, distinct pipe name):
//   client -> server : u32 protocol version, u32 payload byte count,
//                      UTF-16 null-terminated path
//   server -> client : u32 Decision
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
    // on the server thread and returns whether the request was handled.
    void StartServer(const std::function<Decision(const std::wstring&)>& onRequest);

    // Tries to send a single file path to an existing server and receive a
    // decision. Returns false if the server is not available (no pipe /
    // connection error).
    bool TrySendPath(const std::wstring& path, Decision& outDecision);
}

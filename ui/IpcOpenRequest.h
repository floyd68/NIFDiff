// IpcOpenRequest.h - cross-thread request object used to marshal an IPC
// "open this .nif" request onto the UI thread, ported from FICture2's
// IpcCompareRequest. Ownership: created/freed by the IPC server thread; the
// UI thread (NifCompareView::OnCommandEvent) only mutates `opened` and
// signals the event.
#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <string>

namespace nsk
{

// Broadcast command id carried through FD2D::Backplate::WM_FD2D_BROADCAST.
inline constexpr UINT CMD_NIFDIFF_IPC_OPEN = WM_APP + 0x7A21;

struct IpcOpenRequest
{
    std::wstring path {};
    void* doneEvent { nullptr }; // HANDLE, owned by the IPC server thread
    bool opened { false };       // set by the UI thread when the file landed in a pane
};

} // namespace nsk

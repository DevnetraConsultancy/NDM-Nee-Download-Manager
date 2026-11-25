// ndm_filedrag.h -- drag finished files out of NDM (download window tile,
// main list rows) into Explorer, the desktop or any app that takes files.
// Copyright (c) 2026 Devnetra Consultancy. MIT License.
//
// A plain drag moves the file; holding Shift when the drag starts copies it.
// Apps that only read files (mail, chat, editors) always get a copy. When a
// file is moved, NDM follows it: the shell's rename / create notifications
// give the new place and the download item is updated, so Open and Open
// Folder keep working.
#pragma once
#include <windows.h>
#include <string>
#include <vector>

namespace NdmFileDrag
{
    struct Item
    {
        DWORD        id = 0;      // download id (0 = do not track)
        std::wstring path;        // full path of the finished file or folder
    };

    // Runs the drag (modal OLE loop) and returns when it ends. Only items
    // that exist and share the first item's folder are dragged. Returns true
    // when something was dropped.
    bool Drag(HWND owner, const std::vector<Item>& items);

    // true while a drag started by NDM is in progress (NDM's own drop
    // target refuses it).
    bool Active();

    // Pure rule for the drag effects: Shift at the start = copy only;
    // otherwise move preferred, copy allowed (apps that only read files).
    DWORD AllowedEffects(bool shiftAtStart);
    DWORD PreferredEffect(bool shiftAtStart);

    // Pure rule: may a path reported by the shell become the item's new
    // place? Not the Recycle Bin, and the same file name (case-insensitive)
    // unless it was a rename event for exactly the old path.
    bool AcceptNewPlace(const std::wstring& oldPath, const std::wstring& newPath, bool renameOfOld);
}

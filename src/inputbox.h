// Foyer - a small themed text prompt ("Rename", "Add website")
#pragma once
#include "common.h"

// Returns false if cancelled. `value` holds the initial text and receives the result.
bool InputBox(HWND owner, const std::wstring& title, const std::wstring& prompt, std::wstring& value, bool dark);

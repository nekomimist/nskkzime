#pragma once
#include "nskkzime/engine.h"
namespace nskkzime::windows {
// Initialization only: no file I/O from key handling or DLL_PROCESS_ATTACH.
std::shared_ptr<const Dictionary> runtime_dictionary();
// Last snapshot loaded in this process. Diagnostics only; performs no file I/O.
std::wstring runtime_dictionary_status();
}

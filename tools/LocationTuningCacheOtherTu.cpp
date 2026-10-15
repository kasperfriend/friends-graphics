// Second translation unit for LocationTuningCacheRegression: proves the INI
// cache is process-wide (shared across TUs), so a save made in the menu's
// translation unit is immediately visible to every effect module that reads
// the same file from its own translation unit.
#include "../src/Environment/LocationTuning.h"

extern "C" {

// Force one read through this TU's inclusion of the header.
unsigned LocationTuningOtherTuRead(LPCWSTR path, LPCWSTR key, INT fallback)
{
    return renderer::locationtuning::ReadInt(L"Water", key, fallback, path);
}

// Save through this TU as well, to cover the invalidation direction.
int LocationTuningOtherTuSave(LPCWSTR section, LPCWSTR key, int value)
{
    return renderer::locationtuning::Save(section, key, value) ? 1 : 0;
}

}

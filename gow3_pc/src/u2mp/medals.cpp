// u2mp medals: the results page falls back to the pop-up's default medal for medals whose
// picture the Collection's net.pak lacks.
#include <cstdint>

#include "u2mp_internal.h"

namespace u2mp {
void install_medals(int &installed);

namespace {

constexpr uint64_t kIconLookup = 0x555140;
constexpr uint64_t kResultsLookup = 0x590561;
constexpr uint64_t kMedalElement = 0x1130008 + 0x310;

struct MedalIcon {
    uint64_t sid;
    uint64_t index;
};

using LookupFn = MedalIcon(U2MP_GUEST *)(const char *, int);

U2MP_GUEST MedalIcon medal_results_icon(const char *name, int table) {
    MedalIcon icon = reinterpret_cast<LookupFn>(image.base + kIconLookup)(name, table);
    if (uint32_t(icon.sid)) return icon;

    const uint64_t medal = *image.at<uint64_t>(kMedalElement);
    if (!medal) return icon;
    const uint64_t sid = *reinterpret_cast<const uint64_t *>(medal + 0x4c);
    const uint32_t index = *reinterpret_cast<const uint32_t *>(medal + 0x54);
    if (!uint32_t(sid)) return icon;
    static bool logged = false;
    if (!logged) {
        logged = true;
        log("medals: a missing result icon uses the default sid %08x index %u", uint32_t(sid), index);
    }

    icon.sid = sid;
    icon.index = index;
    return icon;
}

}  // namespace

void install_medals(int &installed) {
    static bool logged = false;
    if (retarget_call(kResultsLookup, kIconLookup, reinterpret_cast<void *>(&medal_results_icon),
                      "medal results icon")) {
        ++installed;
        if (!logged) {
            logged = true;
            log("medals: results page uses the pop-up's default medal for missing icons");
        }
    }
}

}  // namespace u2mp

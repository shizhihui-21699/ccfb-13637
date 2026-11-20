#include "types.h"

namespace td {
const char* verdict_name(VerdictType t) {
    switch (t) {
        case VerdictType::FRAME_DELETION:  return "frame_deletion";
        case VerdictType::FRAME_INSERTION: return "frame_insertion";
        case VerdictType::SPLICE:          return "splice";
        case VerdictType::LOCAL_REPLACE:   return "local_replace";
    }
    return "unknown";
}
} // namespace td

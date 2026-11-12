#ifndef STARDESK_TEXT_PROBE_H_
#define STARDESK_TEXT_PROBE_H_

#include <string>

namespace sdk {

/** Developer hook (STARDESK_TEXT_PROBE=/path/out.raw): renders a few strings in
 *  the eight UI languages through the dui text renderer and saves the raster as
 *  a raw RGBA image, so font fallback (Hangul/Cyrillic/...) can be checked
 *  without a window. The first 8 bytes are width/height as two uint32.
 */
void RunTextProbe(const std::string& path);

} // namespace sdk

#endif // STARDESK_TEXT_PROBE_H_

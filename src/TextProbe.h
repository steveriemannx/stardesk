#ifndef STARDESK_TEXT_PROBE_H_
#define STARDESK_TEXT_PROBE_H_

#include "dui/dui.h"

#include <string>

namespace sdk {

/** Developer hook (STARDESK_TEXT_PROBE=/path/out.raw): renders a few strings in
 *  the eight UI languages through the dui text renderer and saves the raster as
 *  a raw RGBA image, so font fallback (Hangul/Cyrillic/...) can be checked
 *  without a window. The first 8 bytes are width/height as two uint32.
 */
void RunTextProbe(const std::string& path);

/** Developer hooks: dump one window's control tree (type/name/rect/text) to a
 *  text file and read its painted pixels back into a raw RGBA file
 *  (first 8 bytes are width/height as two uint32). */
void DumpWindowTreeToFile(ui::Window* pWindow, const std::string& path);
bool CaptureWindowPixels(ui::Window* pWindow, const std::string& path);


} // namespace sdk

#endif // STARDESK_TEXT_PROBE_H_

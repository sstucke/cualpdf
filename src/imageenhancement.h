#pragma once

#include <QImage>

// Pixel-level page enhancement tools (OpenCV-backed). Reused across the
// image-enhancement toolset (aclarar today; deskew/scan-cleanup etc. later).
namespace ImageEnhancement {

int clampAmount(int amount);

// Flattens the uneven shading of a photographed paper page and boosts
// local/global contrast, without converting the page to pure black & white.
// `amount` ranges 0 (no change, returns `page` as-is) to 100 (full effect).
QImage aclararPapel(const QImage &page, int amount);

} // namespace ImageEnhancement

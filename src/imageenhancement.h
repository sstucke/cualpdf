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

QImage invertirColores(const QImage &page);

// Margins, in pixels of `page`, that trim a near-black border. Empty if the
// page has no such border or the crop would erase the content.
QMargins blackBorderMargins(const QImage &page);

// level is 0 (gentle) through 6 (strong), matching CopyFlow's scan cleanup.
QImage mejorarEscaneo(const QImage &page, int level, bool whiteBackground, bool blackText,
                      bool blackAndWhite);

// Four corners in image pixels: top-left, top-right, bottom-right, bottom-left.
QImage corregirPerspectiva(const QImage &page, const QVector<QPointF> &corners);

} // namespace ImageEnhancement

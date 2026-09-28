#pragma once

#include <QByteArray>
#include <QColor>
#include <QImage>
#include <QRect>
#include <QStringList>
#include <QVector>
#include <functional>

struct OcrTextRun {
    QString text;
    QRect bounds; // Pixels in the rendered source, origin at the top left.
    QVector<QRect> words;
    float confidence = 0;
    QString fontFamily;
    QByteArray fontData;
    QColor color = Qt::black;
    bool accepted = true;
};

struct OcrPage {
    QImage source;
    QImage background;
    QSizeF pageSize;
    QVector<OcrTextRun> runs;
};

namespace OcrEngine {
using Canceled = std::function<bool()>;
QString additionalLanguagesDirectory();
QStringList languages(QString *error = nullptr);
// Offline OCR through the statically linked motor and bundled language resources.
OcrPage recognize(const QImage &source, const QSizeF &pageSize, const QString &language,
                  QString *error, const Canceled &canceled = {});
QVector<OcrTextRun> parseTsv(const QByteArray &tsv, const QSize &imageSize);
// Rebuild from the original every time: excluded/uncertain text stays in the image.
bool prepareBackground(OcrPage *page, const Canceled &canceled = {});
}

#include "ocrengine.h"

#include <QDirIterator>
#include <QCoreApplication>
#include <QFile>
#include <QGlyphRun>
#include <QPainter>
#include <QRawFont>
#include <QRegularExpression>
#include <QStandardPaths>
#include <tesseract/baseapi.h>
#include <tesseract/ocrclass.h>
#include <opencv2/imgproc.hpp>
#include <opencv2/photo.hpp>
#include <algorithm>
#include <cmath>
#include <limits>
#include <memory>
#include <stdexcept>

namespace {
QString tr(const char *text) { return QCoreApplication::translate("OcrEngine", text); }
struct FontCandidate { QRawFont raw; QByteArray data; };

QVector<FontCandidate> fonts()
{
    const QStringList roots{QStringLiteral(":/ocr/fonts")};
    QStringList paths;
    for (const QString &root : roots) {
        QDirIterator it(root, {QStringLiteral("*.ttf"), QStringLiteral("*.TTF")},
                        QDir::Files, QDirIterator::Subdirectories);
        while (it.hasNext()) paths.append(it.next());
    }
    paths.removeDuplicates();
    std::sort(paths.begin(), paths.end());
    QVector<FontCandidate> result;
    for (const QString &path : paths) {
        QFile file(path);
        if (file.size() > 8 * 1024 * 1024 || !file.open(QIODevice::ReadOnly)) continue;
        QByteArray data = file.readAll();
        QRawFont raw(data, 64, QFont::PreferNoHinting);
        if (!raw.isValid()) continue;
        const QByteArray os2 = raw.fontTable("OS/2");
        // Honor font embedding restrictions; restricted and bitmap-only fonts are unsuitable.
        if (os2.size() >= 10) {
            const unsigned flags = (uchar(os2[8]) << 8) | uchar(os2[9]);
            if (flags & (0x0002 | 0x0200)) continue;
        }
        result.append({raw, data});
        if (result.size() >= 40) break;
    }
    return result;
}

QImage glyphImage(const QRawFont &font, const QString &text)
{
    const auto glyphs = font.glyphIndexesForString(text);
    if (glyphs.isEmpty() || glyphs.contains(0)) return {};
    const auto advances = font.advancesForGlyphIndexes(glyphs);
    QVector<QPointF> positions;
    QPointF pen;
    QRectF bounds;
    for (int i = 0; i < glyphs.size(); ++i) {
        positions.append(pen);
        bounds = bounds.united(font.boundingRect(glyphs[i]).translated(pen));
        pen += advances[i];
    }
    if (bounds.isEmpty() || bounds.width() > 20000) return {};
    QImage image(bounds.size().toSize() + QSize(4, 4), QImage::Format_RGB32);
    image.fill(Qt::white);
    QPainter painter(&image);
    painter.setRenderHint(QPainter::TextAntialiasing);
    painter.setPen(Qt::black);
    QGlyphRun run;
    run.setRawFont(font);
    run.setGlyphIndexes(glyphs);
    run.setPositions(positions);
    painter.drawGlyphRun(QPointF(2, 2) - bounds.topLeft(), run);
    painter.end();
    return image.copy(2, 2, image.width() - 4, image.height() - 4);
}

cv::Mat bgr(const QImage &source)
{
    const QImage rgb = source.convertToFormat(QImage::Format_RGB888);
    cv::Mat result;
    cv::cvtColor(cv::Mat(rgb.height(), rgb.width(), CV_8UC3,
                        const_cast<uchar *>(rgb.constBits()), rgb.bytesPerLine()), result,
                 cv::COLOR_RGB2BGR);
    return result;
}

double fontScore(const QImage &observed, const QImage &rendered)
{
    if (rendered.isNull()) return std::numeric_limits<double>::infinity();
    const QSize size(std::clamp(observed.width(), 8, 700), std::clamp(observed.height(), 8, 80));
    const QImage a = observed.scaled(size).convertToFormat(QImage::Format_Grayscale8);
    const QImage b = rendered.scaled(size).convertToFormat(QImage::Format_Grayscale8);
    double overlap = 0, total = 0;
    for (int y = 0; y < size.height(); ++y) {
        for (int x = 0; x < size.width(); ++x) {
            const double inkA = (255 - a.constScanLine(y)[x]) / 255.0;
            const double inkB = (255 - b.constScanLine(y)[x]) / 255.0;
            overlap += std::min(inkA, inkB);
            total += inkA + inkB;
        }
    }
    const double aspectA = double(observed.width()) / observed.height();
    const double aspectB = double(rendered.width()) / rendered.height();
    return 1.0 - 2 * overlap / std::max(1.0, total) + 0.12 * std::abs(std::log(aspectA / aspectB));
}
}

QString OcrEngine::additionalLanguagesDirectory()
{
    return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + QStringLiteral("/tessdata");
}

QStringList OcrEngine::languages(QString *error)
{
    QStringList result;
    for (const QString &root : {QStringLiteral(":/ocr/tessdata"), additionalLanguagesDirectory()}) {
        for (const auto &file : QDir(root).entryList({QStringLiteral("*.traineddata")}, QDir::Files)) {
            const QString name = QFileInfo(file).completeBaseName();
            if (QRegularExpression(QStringLiteral("^[A-Za-z0-9_]+$")).match(name).hasMatch()) result.append(name);
        }
    }
    result.removeDuplicates();
    result.sort();
    if (result.isEmpty() && error) *error = tr("The bundled OCR language resources are missing.");
    return result;
}

QVector<OcrTextRun> OcrEngine::parseTsv(const QByteArray &tsv, const QSize &imageSize)
{
    QVector<OcrTextRun> runs;
    QString previousKey;
    int wordCount = 0;
    for (const auto &line : QString::fromUtf8(tsv).split('\n')) {
        const auto fields = line.split('\t');
        if (fields.size() < 12 || fields[0] != "5") continue;
        bool ok[5];
        const int x = fields[6].toInt(&ok[0]), y = fields[7].toInt(&ok[1]);
        const int w = fields[8].toInt(&ok[2]), h = fields[9].toInt(&ok[3]);
        const float confidence = fields[10].toFloat(&ok[4]);
        if (!std::all_of(std::begin(ok), std::end(ok), [](bool value) { return value; })
            || x < 0 || y < 0 || w <= 0 || h <= 0 || x > imageSize.width() - w
            || y > imageSize.height() - h || !std::isfinite(confidence) || confidence < 0) continue;
        const QString text = fields.mid(11).join(' ').trimmed();
        if (text.isEmpty()) continue;
        const QRect box(x, y, w, h);
        const QString key = fields.mid(1, 4).join('/');
        const bool gap = !runs.isEmpty() && x - runs.last().bounds.right() > 2 * h;
        if (runs.isEmpty() || key != previousKey || gap) {
            runs.append(OcrTextRun{});
            wordCount = 0;
        }
        auto &run = runs.last();
        run.text += (run.text.isEmpty() ? QString() : QStringLiteral(" ")) + text;
        run.bounds = run.bounds.united(box);
        run.words.append(box);
        run.confidence = (run.confidence * wordCount + confidence) / (wordCount + 1);
        run.accepted = run.confidence >= 65;
        ++wordCount;
        previousKey = key;
    }
    return runs;
}

OcrPage OcrEngine::recognize(const QImage &source, const QSizeF &pageSize,
                            const QString &language, QString *error, const Canceled &canceled)
{
    OcrPage page;
    page.source = source;
    page.pageSize = pageSize;
    if (source.isNull()) { if (error) *error = tr("The page could not be rendered."); return page; }
    if (error) error->clear();
    if (canceled && canceled()) return page;
    const QStringList available = languages();
    for (const QString &selected : language.split('+')) {
        if (!available.contains(selected)) {
            if (error) *error = tr("OCR language is not available: %1").arg(selected);
            return page;
        }
    }
    tesseract::TessBaseAPI api;
    const auto reader = [](const char *filename, std::vector<char> *data) {
        const QString name = QFileInfo(QString::fromUtf8(filename)).fileName();
        QFile resource(QStringLiteral(":/ocr/tessdata/") + name);
        if (!resource.exists()) resource.setFileName(OcrEngine::additionalLanguagesDirectory() + '/' + name);
        if (!resource.open(QIODevice::ReadOnly)) return false;
        const QByteArray bytes = resource.readAll();
        data->assign(bytes.cbegin(), bytes.cend());
        return true;
    };
    if (api.Init(":/ocr/tessdata/", 0, language.toUtf8().constData(), tesseract::OEM_LSTM_ONLY,
                 nullptr, 0, nullptr, nullptr, false, reader) != 0) {
        if (error) *error = tr("The OCR language data could not be loaded.");
        return page;
    }
    const QImage rgb = source.convertToFormat(QImage::Format_RGB888);
    api.SetImage(rgb.constBits(), rgb.width(), rgb.height(), 3, rgb.bytesPerLine());
    api.SetSourceResolution(300);
    api.SetPageSegMode(tesseract::PSM_AUTO);
    tesseract::ETEXT_DESC monitor;
    monitor.cancel_this = const_cast<Canceled *>(&canceled);
    monitor.cancel = [](void *context, int) {
        const auto &check = *static_cast<Canceled *>(context);
        return check && check();
    };
    monitor.set_deadline_msecs(180000);
    if (api.Recognize(&monitor) != 0) {
        if (error && !(canceled && canceled())) *error = tr("OCR failed or exceeded the three-minute page limit.");
        return page;
    }
    const std::unique_ptr<char[]> tsv(api.GetTSVText(0));
    page.runs = parseTsv(tsv ? QByteArray(tsv.get()) : QByteArray(), source.size());
    const auto candidates = fonts();
    if (!page.runs.isEmpty() && candidates.isEmpty()) {
        if (error) *error = tr("The bundled OCR fonts are missing.");
        return page;
    }
    for (auto &run : page.runs) {
        if (canceled && canceled()) return page;
        const QImage sample = source.copy(run.bounds);
        double best = std::numeric_limits<double>::infinity();
        for (const auto &candidate : candidates) {
            const double score = fontScore(sample, glyphImage(candidate.raw, run.text));
            if (score < best) {
                best = score;
                run.fontFamily = candidate.raw.familyName() + " " + candidate.raw.styleName();
                run.fontData = candidate.data;
            }
        }
        if (run.text.isRightToLeft()) { run.fontData.clear(); run.fontFamily.clear(); }
        if (run.fontData.isEmpty()) run.accepted = false;
        QVector<int> red, green, blue;
        for (const QRect &box : run.words) {
            for (int y = box.top(); y <= box.bottom(); ++y) {
                for (int x = box.left(); x <= box.right(); ++x) {
                    const QRgb pixel = source.pixel(x, y);
                    if (qGray(pixel) < 150) { red.append(qRed(pixel)); green.append(qGreen(pixel)); blue.append(qBlue(pixel)); }
                }
            }
        }
        if (!red.isEmpty()) {
            const auto median = [](QVector<int> &values) {
                auto mid = values.begin() + values.size() / 2;
                std::nth_element(values.begin(), mid, values.end()); return *mid;
            };
            run.color = QColor(median(red), median(green), median(blue));
        }
    }
    return page;
}

bool OcrEngine::prepareBackground(OcrPage *page, const Canceled &canceled)
{
    if (!page || page->source.isNull()) return false;
    cv::Mat source = bgr(page->source);
    cv::Mat mask(source.rows, source.cols, CV_8UC1, cv::Scalar(0));
    for (const auto &run : page->runs) {
        if (canceled && canceled()) return false;
        if (!run.accepted || run.text.trimmed().isEmpty() || run.fontData.isEmpty()) continue;
        const QRawFont font(run.fontData, 64, QFont::PreferNoHinting);
        if (run.text.isRightToLeft() || !font.isValid() || font.glyphIndexesForString(run.text).contains(0))
            throw std::runtime_error(tr("The substitute font does not support the corrected text. Uncheck this line.").toStdString());
        for (const QRect &word : run.words) {
            const QRect box = word.adjusted(-2, -2, 2, 2).intersected(page->source.rect());
            const cv::Rect region(box.x(), box.y(), box.width(), box.height());
            cv::Mat gray, ink;
            cv::cvtColor(source(region), gray, cv::COLOR_BGR2GRAY);
            cv::threshold(gray, ink, 0, 255, cv::THRESH_BINARY_INV | cv::THRESH_OTSU);
            // Refuse ambiguous dense regions instead of leaving duplicate ink
            // behind an editable text object. The original document is unchanged.
            if (cv::countNonZero(ink) > ink.total() * 0.7)
                throw std::runtime_error(tr("Cannot safely separate this text from its background. Uncheck the line and retry.").toStdString());
            cv::dilate(ink, ink, cv::getStructuringElement(cv::MORPH_RECT, cv::Size(3, 3)));
            cv::bitwise_or(mask(region), ink, mask(region));
        }
    }
    cv::Mat restored;
    if (cv::countNonZero(mask)) cv::inpaint(source, mask, restored, 3.0, cv::INPAINT_TELEA);
    else restored = source;
    cv::Mat rgb;
    cv::cvtColor(restored, rgb, cv::COLOR_BGR2RGB);
    page->background = QImage(rgb.data, rgb.cols, rgb.rows, int(rgb.step), QImage::Format_RGB888).copy();
    return !(canceled && canceled());
}

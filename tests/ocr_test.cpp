#include "ocrengine.h"
#include "ocrreviewdialog.h"
#include "pdfdocument.h"
#include "pdfviewerwidget.h"
#include <QAction>
#include <QEventLoop>
#include <QLabel>
#include <QMouseEvent>
#include <QComboBox>
#include <QToolButton>
#include <QMessageBox>
#include <QPushButton>
#include <QSettings>
#include <QTimer>
#include <QApplication>
#include <QLocale>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFontDatabase>
#include <QPainter>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QStandardPaths>
#include <algorithm>
#include <stdexcept>

#define CHECK(condition) do { if (!(condition)) { qCritical() << "Failed at" << __LINE__ << #condition; return 1; } } while (false)

namespace {
QString contents(const OcrPage &page) {
    QString text;
    for (const auto &run : page.runs) text += run.text + '\n';
    return text;
}
QVector<PdfPageObjectInfo> texts(const PdfDocument &document, QSize size) {
    auto objects = document.pageObjects(0, size);
    objects.removeIf([](const auto &o) { return o.kind != PdfPageObjectKind::Text; });
    return objects;
}
bool sameText(const PdfDocument &a, const PdfDocument &b, QSize size) {
    const auto x = texts(a, size), y = texts(b, size);
    if (x.size() != y.size()) return false;
    for (int i = 0; i < x.size(); ++i)
        if (x[i].text != y[i].text || x[i].bounds != y[i].bounds) return false;
    return true;
}
int darkPixels(const QImage &image, const QRect &rect) {
    int count = 0;
    const auto box = rect.intersected(image.rect());
    for (int y = box.top(); y <= box.bottom(); ++y)
        for (int x = box.left(); x <= box.right(); ++x) count += qGray(image.pixel(x, y)) < 160;
    return count;
}
bool write(const QString &path, const QByteArray &bytes) {
    QFile file(path); return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
}
bool waitUntil(const std::function<bool()> &ready, int timeout = 30000) {
    if (ready()) return true;
    QEventLoop loop;
    QTimer poll, deadline;
    poll.setInterval(20); deadline.setSingleShot(true);
    QObject::connect(&poll, &QTimer::timeout, &loop, [&] { if (ready()) loop.quit(); });
    QObject::connect(&deadline, &QTimer::timeout, &loop, &QEventLoop::quit);
    poll.start(); deadline.start(timeout); loop.exec();
    return ready();
}

int viewerWorkflow(const QByteArray &original, QSize size, const QString &path) {
    CHECK(write(path, original));
    PdfViewerWidget viewer(path); viewer.resize(1100, 800); viewer.show();
    CHECK(waitUntil([&] { return viewer.isValid(); }));
    auto *action = viewer.findChild<QAction *>("editableOcrAction"); CHECK(action);
    bool settingsClosed = false, cancel = true;
    QString dialogError;
    QTimer driver;
    QObject::connect(&driver, &QTimer::timeout, &viewer, [&] {
        auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
        if (!dialog) return;
        if (auto *message = qobject_cast<QMessageBox *>(dialog)) {
            dialogError = message->text(); message->reject(); return;
        }
        if (dialog->windowTitle() == "OCR — editable text and background") {
            settingsClosed = true;
            cancel ? dialog->reject() : dialog->accept();
        }
    });
    driver.start(20);
    QTimer::singleShot(0, action, &QAction::trigger);
    CHECK(waitUntil([&] { return settingsClosed || !dialogError.isEmpty(); }));
    CHECK(dialogError.isEmpty()); CHECK(!viewer.isModified()); CHECK(!viewer.canUndo());
    cancel = false; settingsClosed = false;
    QTimer::singleShot(0, action, &QAction::trigger);
    CHECK(waitUntil([&] { return viewer.canUndo() || !dialogError.isEmpty(); }, 180000));
    CHECK(dialogError.isEmpty()); CHECK(viewer.isModified());
    driver.stop();
    const auto save = [&] {
        bool finished = false, ok = false;
        const auto connection = QObject::connect(&viewer, &PdfViewerWidget::saveFinished, &viewer,
            [&](bool success, const QString &) { finished = true; ok = success; });
        viewer.saveDocument(false, 0);
        const bool completed = waitUntil([&] { return finished; });
        QObject::disconnect(connection);
        return completed && ok;
    };
    CHECK(save());
    { PdfDocument converted(path); CHECK(!texts(converted, size).isEmpty()); }
    viewer.undo(); CHECK(waitUntil([&] { return !viewer.isOperationInProgress(); }));
    CHECK(viewer.canRedo()); CHECK(save());
    { PdfDocument restored(path), initial(original);
      CHECK(texts(restored, size).isEmpty()); CHECK(restored.renderPage(0, size.width()) == initial.renderPage(0, size.width())); }
    viewer.redo(); CHECK(waitUntil([&] { return !viewer.isOperationInProgress(); }));
    CHECK(viewer.canUndo()); CHECK(save());
    { PdfDocument restored(path); CHECK(!texts(restored, size).isEmpty()); }
    const QString copyPath = path + QStringLiteral(".copy.pdf");
    bool copied = false, copyOk = false;
    const auto copyConnection = QObject::connect(
        &viewer, &PdfViewerWidget::saveFinished, &viewer,
        [&](bool success, const QString &) { copied = true; copyOk = success; });
    viewer.saveDocumentAs(copyPath);
    CHECK(waitUntil([&] { return copied; }));
    QObject::disconnect(copyConnection);
    CHECK(copyOk); CHECK(viewer.filePath() == copyPath);
    { PdfDocument copy(copyPath); CHECK(copy.isValid()); CHECK(!texts(copy, size).isEmpty()); }
    viewer.close();
    // Drain queued rendering work before shutting down PDFium in main().
    QEventLoop drain; QTimer::singleShot(200, &drain, &QEventLoop::quit); drain.exec();
    return 0;
}
int staleSelectionWorkflow(const QByteArray &scan, const QString &path) {
    QSettings().setValue("ocr/language", "eng");
    PdfDocument twoPages(PdfDocument::createBlankPageArchive(QSizeF(612, 360)));
    CHECK(twoPages.restorePageStructure({1}, {1, 2}, {2}, scan));
    CHECK(write(path, twoPages.exportPages({0, 1})));
    PdfViewerWidget viewer(path); viewer.resize(1100, 800); viewer.show();
    CHECK(waitUntil([&] { return viewer.isValid(); }));
    const auto button = [&](const QString &text) -> QToolButton * {
        for (auto *candidate : viewer.findChildren<QToolButton *>())
            if (candidate->text() == text) return candidate;
        return nullptr;
    };
    auto *edit = button("Edit PDF"); auto *select = button("Select Page"); CHECK(edit && select);
    const auto clickPage = [&](int index) {
        for (auto *label : viewer.findChildren<QLabel *>()) {
            if (!label->property("pdfPageIndex").isValid() || label->property("pdfPageIndex").toInt() != index) continue;
            const QPoint position = label->rect().center();
            const QPointF local(position), global(label->mapToGlobal(position));
            QMouseEvent press(QEvent::MouseButtonPress, local, global, Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
            QMouseEvent release(QEvent::MouseButtonRelease, local, global, Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
            QApplication::sendEvent(label, &press); QApplication::sendEvent(label, &release);
            return true;
        }
        return false;
    };
    edit->click(); select->click(); CHECK(clickPage(0));
    edit->click(); edit->click(); // Return to objects, retaining the old page selection.
    viewer.goToPage(1); CHECK(clickPage(1)); CHECK(viewer.currentPageIndex() == 1);
    bool correctTarget = false;
    QString dialogError;
    int scopeIndex = -1;
    QTimer driver;
    QObject::connect(&driver, &QTimer::timeout, &viewer, [&] {
        auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
        if (!dialog) return;
        if (auto *message = qobject_cast<QMessageBox *>(dialog)) {
            dialogError = message->text(); message->reject(); return;
        }
        if (dialog->windowTitle() != "OCR — editable text and background") return;
        auto *scope = dialog->findChild<QComboBox *>("ocrScope");
        if (!scope) { dialogError = "Missing scope control"; dialog->reject(); return; }
        scopeIndex = scope->currentIndex();
        correctTarget = scope->currentText() == "Current page (2)";
        viewer.goToPage(0); // A queued viewport update must not retarget OCR.
        dialog->accept();
    });
    driver.start(20);
    auto *action = viewer.findChild<QAction *>("editableOcrAction"); CHECK(action);
    QTimer::singleShot(0, action, &QAction::trigger);
    CHECK(waitUntil([&] { return viewer.canUndo() || !dialogError.isEmpty(); }, 180000));
    CHECK(scopeIndex == 0); // Current page, not a stale selection from page mode.
    CHECK(dialogError.isEmpty()); CHECK(correctTarget);
    CHECK(viewer.isModified());
    return 0;
}
int probeScan(const QString &path, int pageNumber, const QString &language, const QString &output) {
    PdfDocument document(path); CHECK(document.isValid());
    CHECK(pageNumber > 0 && pageNumber <= document.pageCount());
    const auto size = document.pageSizePoints(pageNumber - 1);
    const auto image = document.renderPage(pageNumber - 1, qRound(size.width() * 300.0 / 72.0));
    CHECK(!image.isNull());
    const auto objects = document.pageObjects(pageNumber - 1, image.size());
    int nativeText = 0;
    for (const auto &object : objects) nativeText += object.kind == PdfPageObjectKind::Text && !object.text.trimmed().isEmpty();
    QString error;
    auto page = OcrEngine::recognize(image, size, language, &error);
    qInfo() << "Scan probe: page" << pageNumber << "render" << image.size()
            << "existing text objects" << nativeText << "OCR boxes" << page.runs.size() << "error" << error;
    QMap<QString, int> matchedFonts;
    for (const auto &run : page.runs) ++matchedFonts[run.fontFamily];
    qInfo() << "Proposed fonts:" << matchedFonts;
    CHECK(error.isEmpty());
    if (!page.runs.isEmpty()) {
        QTemporaryDir workflow;
        CHECK(workflow.isValid());
        CHECK(staleSelectionWorkflow(document.exportPages({pageNumber - 1}), workflow.filePath("viewer.pdf")) == 0);
    }
    if (!output.isEmpty()) {
        CHECK(QDir().mkpath(output));
        CHECK(image.save(output + "/scan.png"));
        CHECK(write(output + "/recognized.txt", contents(page).toUtf8()));
        if (!page.runs.isEmpty()) {
            CHECK(OcrEngine::prepareBackground(&page));
            CHECK(page.background.save(output + "/background.png"));
            const auto archive = PdfDocument::createOcrPagesArchive({page}, &error);
            CHECK(!archive.isEmpty()); CHECK(write(output + "/editable.pdf", archive));
        }
    }
    return page.runs.isEmpty() ? 2 : 0;
}
int run(const QString &artifactDirectory) {
    const QStringList codes{"spa", "eng", "fra", "cat", "por", "ita", "deu"};
    for (const auto &code : codes) CHECK(OcrEngine::languages().contains(code));
    QFile fontFile(":/ocr/fonts/LiberationSans-Regular.ttf");
    CHECK(fontFile.open(QIODevice::ReadOnly));
    const auto fontData = fontFile.readAll();
    const int fontId = QFontDatabase::addApplicationFontFromData(fontData);
    CHECK(fontId >= 0);
    QFont font(QFontDatabase::applicationFontFamilies(fontId).first()); font.setPixelSize(48);
    const QStringList lines{
        "Español: edición, información y corazón.",
        "English: editable text and separate background.",
        "Français : édition, liberté et égalité.",
        "Català: informació, edició i col·laboració.",
        "Português: edição, informação e coração.",
        "Italiano: città, qualità e libertà.",
        "Deutsch: Grüße, Bücher und schöne Wörter."};
    QImage source(1700, 1000, QImage::Format_RGB32); source.fill(Qt::white);
    {
        QPainter painter(&source); painter.setFont(font); painter.setPen(Qt::black);
        for (int i = 0; i < lines.size(); ++i) painter.drawText(80, 110 + i * 105, lines[i]);
        painter.fillRect(QRect(80, 870, 550, 70), QColor(40, 110, 175));
    }
    QString error;
    // Every bundled model actually initializes and recognizes its own language.
    for (int i = 0; i < codes.size(); ++i) {
        QImage sample(1700, 320, QImage::Format_RGB32); sample.fill(Qt::white);
        QPainter painter(&sample); painter.setFont(font); painter.setPen(Qt::black);
        painter.drawText(80, 110, lines[i]); painter.drawText(80, 220, lines[i]); painter.end();
        const auto recognized = OcrEngine::recognize(sample, QSizeF(612, 115), codes[i], &error);
        CHECK(error.isEmpty());
        qInfo().noquote() << codes[i] << contents(recognized).trimmed();
        CHECK(contents(recognized).contains(lines[i].section(':', 0, 0).trimmed()));
    }
    auto page = OcrEngine::recognize(source, QSizeF(612, 360), "spa+eng+fra+cat+por+ita+deu", &error);
    CHECK(error.isEmpty()); CHECK(page.runs.size() >= 7);
    for (const auto &run : page.runs) { CHECK(!run.fontData.isEmpty()); CHECK(run.accepted); }
    qInfo().noquote() << "Combined OCR:\n" << contents(page);
    CHECK(contents(page).contains("información")); CHECK(contents(page).contains("English"));
    // Review does not mutate its input, and preserves user corrections/checks.
    {
        OcrReviewDialog review({page}, {0}, nullptr);
        auto *table = review.findChild<QTableWidget *>(); CHECK(table);
        table->item(0, 1)->setText("Texto corregido: col·laboració");
        table->item(1, 0)->setCheckState(Qt::Unchecked);
        CHECK(review.pages()[0].runs[0].text == "Texto corregido: col·laboració");
        CHECK(!review.pages()[0].runs[1].accepted);
        CHECK(page.runs[0].text != "Texto corregido");
    }
    CHECK(OcrEngine::prepareBackground(&page));
    CHECK(page.background.pixelColor(100, 900) == source.pixelColor(100, 900));
    for (const auto &run : page.runs) {
        CHECK(darkPixels(page.background, run.bounds) < darkPixels(source, run.bounds) * 0.05);
    }
    const auto archive = PdfDocument::createOcrPagesArchive({page}, &error);
    CHECK(!archive.isEmpty()); CHECK(error.isEmpty());
    PdfDocument document(archive); CHECK(document.isValid());
    const auto textObjects = texts(document, source.size()); CHECK(textObjects.size() == page.runs.size());
    for (int i = 0; i < textObjects.size(); ++i) {
        CHECK(textObjects[i].text == page.runs[i].text);
        CHECK((textObjects[i].bounds.center() - page.runs[i].bounds.center()).manhattanLength() <= 4);
    }
    const auto objects = document.pageObjects(0, source.size());
    const auto bg = std::find_if(objects.begin(), objects.end(), [](const auto &o) { return o.kind == PdfPageObjectKind::Image; });
    CHECK(bg != objects.end());
    const auto croppedArchive = document.exportEditedObjectPage(0, bg->path, false, QRectF(0.1, 0.1, 0.8, 0.8));
    PdfDocument cropped(croppedArchive); CHECK(cropped.isValid()); CHECK(sameText(document, cropped, source.size()));
    const auto removedArchive = document.exportEditedObjectPage(0, bg->path, true);
    PdfDocument removed(removedArchive); CHECK(removed.isValid()); CHECK(sameText(document, removed, source.size()));
    CHECK(removed.pageObjects(0, source.size()).size() == textObjects.size());
    const auto erasedArchive = document.exportEditedObjectPage(0, textObjects.first().path, true);
    PdfDocument erased(erasedArchive); CHECK(erased.isValid());
    CHECK(darkPixels(erased.renderPage(0, source.width()), page.runs.first().bounds) == 0);
    CHECK(document.setPageObjectText(0, textObjects.first().path, "¡Nuevo! pingüino, àéîõü, €42, col·laboració"));
    CHECK(texts(document, source.size()).first().text == "¡Nuevo! pingüino, àéîõü, €42, col·laboració");
    QTemporaryDir temp; CHECK(temp.isValid());
    CHECK(document.saveSafely(temp.filePath("edited.pdf"), false, 0, &error));
    PdfDocument reopened(temp.filePath("edited.pdf")); CHECK(sameText(document, reopened, source.size()));
    // Replacement round-trip: original raster -> OCR -> original -> OCR.
    const auto original = PdfDocument::createImagePageArchive(source, page.pageSize);
    CHECK(staleSelectionWorkflow(original, temp.filePath("stale-selection.pdf")) == 0);
    PdfDocument history(original);
    CHECK(history.restorePageStructure({1}, {2}, {2}, archive));
    CHECK(viewerWorkflow(original, source.size(), temp.filePath("viewer.pdf")) == 0);
    CHECK(texts(history, source.size()).size() == page.runs.size());
    CHECK(history.restorePageStructure({2}, {1}, {1}, original));
    CHECK(texts(history, source.size()).isEmpty());
    CHECK(history.renderPage(0, source.width()) == PdfDocument(original).renderPage(0, source.width()));
    CHECK(history.restorePageStructure({1}, {2}, {2}, archive));
    // Unchecked regions remain raster and cancellation does no reconstruction.
    auto partial = page; partial.runs[0].accepted = false;
    CHECK(OcrEngine::prepareBackground(&partial));
    CHECK(partial.background.copy(page.runs[0].bounds) == source.copy(page.runs[0].bounds).convertToFormat(QImage::Format_RGB888));
    CHECK(!OcrEngine::prepareBackground(&partial, [] { return true; }));
    CHECK(OcrEngine::recognize(source, page.pageSize, "spa", &error, [] { return true; }).runs.isEmpty());
    OcrEngine::recognize(source, page.pageSize, "nonexistent", &error); CHECK(!error.isEmpty());
    QImage blank(500, 500, QImage::Format_RGB32); blank.fill(Qt::white);
    CHECK(OcrEngine::recognize(blank, QSizeF(200, 200), "eng", &error).runs.isEmpty()); CHECK(error.isEmpty());
    partial.runs[0].accepted = true; partial.runs[0].text = QString::fromUtf8("漢字");
    CHECK(OcrEngine::prepareBackground(&partial));
    CHECK(!partial.runs[0].accepted);
    // An inseparable dense line stays raster while the rest of the page can
    // still become editable; it must not abort the complete OCR operation.
    auto dense = page;
    dense.source = QImage(40, 40, QImage::Format_RGB32); dense.source.fill(Qt::black);
    dense.runs = {page.runs.first()};
    dense.runs[0].accepted = true; dense.runs[0].bounds = QRect(0, 0, 40, 40);
    dense.runs[0].words = {QRect(0, 0, 40, 40)};
    CHECK(OcrEngine::prepareBackground(&dense));
    CHECK(!dense.runs[0].accepted);
    CHECK(dense.background == dense.source.convertToFormat(QImage::Format_RGB888));
    const QByteArray tsv = "5\t1\t1\t1\t1\t1\t10\t10\t40\t20\t95\tHola\n"
        "5\t1\t1\t1\t1\t2\t200\t10\t40\t20\t30\tmundo\n"
        "5\t1\t1\t1\t2\t1\t-1\t10\t40\t20\t90\tinvalid\n";
    const auto previousLocale = QLocale();
    QLocale::setDefault(QLocale(QLocale::Spanish, QLocale::Argentina));
    const QByteArray weightedTsv =
        "5\t1\t1\t1\t1\t1\t10\t10\t30\t20\t30\t—¡Yo,\n"
        "5\t1\t1\t1\t1\t2\t50\t10\t90\t20\t96\tmalabarista!\n";
    const auto weighted = OcrEngine::parseTsv(weightedTsv, QSize(300, 200));
    CHECK(weighted.size() == 1);
    CHECK(weighted[0].text == QString::fromUtf8("—¡Yo, malabarista!"));
    CHECK(weighted[0].accepted);
    const QByteArray decimalTsv = "5\t1\t1\t1\t1\t1\t10\t10\t40\t20\t95,5\tCapítulo\n";
    const auto decimalParsed = OcrEngine::parseTsv(decimalTsv, QSize(300, 200));
    QLocale::setDefault(previousLocale);
    CHECK(decimalParsed.size() == 1);
    CHECK(decimalParsed[0].text == QString::fromUtf8("Capítulo"));
    CHECK(decimalParsed[0].accepted);
    const auto parsed = OcrEngine::parseTsv(tsv, QSize(300, 200));
    CHECK(parsed.size() == 2); CHECK(parsed[0].accepted); CHECK(!parsed[1].accepted);
    if (!artifactDirectory.isEmpty()) {
        CHECK(QDir().mkpath(artifactDirectory));
        CHECK(write(artifactDirectory + "/editable.pdf", archive));
        CHECK(write(artifactDirectory + "/background-cropped.pdf", croppedArchive));
        CHECK(write(artifactDirectory + "/text-only.pdf", removedArchive));
        CHECK(source.save(artifactDirectory + "/source.png"));
        CHECK(page.background.save(artifactDirectory + "/background.png"));
    }
    qInfo() << "Editable OCR: all checks passed";
    return 0;
}
}
int main(int argc, char **argv) {
    qputenv("QT_FORCE_STDERR_LOGGING", "1");
    QApplication app(argc, argv);
    QCoreApplication::setApplicationName("cualpdf-ocr-test");
    QStandardPaths::setTestModeEnabled(true);
    QTemporaryDir settings;
    if (!settings.isValid()) return 1;
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings.path());
    PdfDocument::initializeLibrary();
    const int result = argc >= 5 && QByteArray(argv[1]) == "--scan"
        ? probeScan(QString::fromLocal8Bit(argv[2]), QByteArray(argv[3]).toInt(),
                    QString::fromLocal8Bit(argv[4]), argc > 5 ? QString::fromLocal8Bit(argv[5]) : QString())
        : run(argc > 1 ? QString::fromLocal8Bit(argv[1]) : QString());
    PdfDocument::shutdownLibrary();
    return result;
}

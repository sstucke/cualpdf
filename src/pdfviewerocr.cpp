#include "pdfviewerwidget.h"
#include "appsettings.h"
#include "ocrengine.h"

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QAction>
#include <QLabel>
#include <QMessageBox>
#include <QPointer>

#include <QProgressDialog>
#include <QPushButton>
#include <QScrollArea>
#include <QSemaphore>
#include <QStandardItemModel>
#include <QTextEdit>
#include <QThread>
#include <QToolButton>
#include <QVBoxLayout>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <numeric>

namespace {
QString text(const char *value) { return QCoreApplication::translate("PdfViewerWidget", value); }
QProgressDialog *progressDialog(QWidget *parent, const QString &title, int count,
                               const std::shared_ptr<std::atomic_bool> &canceled)
{
    auto *dialog = new QProgressDialog(title, text("Cancel"), 0, count, parent);
    dialog->setWindowModality(Qt::WindowModal);
    dialog->setAutoClose(false); dialog->setAutoReset(false);
    dialog->setMinimumDuration(0); dialog->setValue(0);
    dialog->setMinimumWidth(520);
    if (auto *label = dialog->findChild<QLabel *>()) label->setWordWrap(true);
    QObject::connect(dialog, &QProgressDialog::canceled, parent, [canceled] { canceled->store(true); });
    QObject::connect(parent, &QObject::destroyed, dialog, [canceled] { canceled->store(true); });
    return dialog;
}
}

void PdfViewerWidget::recognizeSelectedPages()
{
    if (!m_valid || !m_document || m_transformInProgress || m_saveInProgress) return;
    commitTextEdit();
    // Page selection survives switching back to object editing. Do not let an
    // old page selection override the page whose object the user is editing.
    // Freeze the targets before exec(): queued renders can change the viewport's
    // current page while the settings dialog is open.
    const int currentPage = m_selectionMode == SelectionMode::Objects
        && m_selectedObject >= 0 && m_objectPageIndex >= 0 ? m_objectPageIndex
        : m_selectionMode == SelectionMode::Region && m_regionPageIndex >= 0
            ? m_regionPageIndex : m_currentPageIndex;
    QVector<int> selectedPages(m_selectedPages.cbegin(), m_selectedPages.cend());
    std::sort(selectedPages.begin(), selectedPages.end());
    QDialog settings(this);
    settings.setWindowTitle(tr("OCR — editable text and background"));
    auto *layout = new QVBoxLayout(&settings);
    const QStringList availableLanguages = OcrEngine::languages();
    QString selectedLanguage = AppSettings().ocrLanguage();
    if (!availableLanguages.contains(selectedLanguage) && !availableLanguages.isEmpty())
        selectedLanguage = availableLanguages.first();
    auto *hint = new QLabel(tr("OCR runs locally using the document language selected in Preferences. "
                              "Each page is applied as soon as it is recognized."), &settings);
    hint->setWordWrap(true); layout->addWidget(hint);
    auto *warning = new QLabel(tr("Conversion rebuilds the selected pages: links, forms and other interactive content are not preserved. Save a separate copy of important originals."), &settings);
    warning->setWordWrap(true); layout->addWidget(warning);
    auto *scope = new QComboBox(&settings);
    scope->setObjectName(QStringLiteral("ocrScope"));
    scope->addItems({tr("Current page (%1)").arg(currentPage + 1),
                     tr("Selected pages (%1)").arg(selectedPages.size()), tr("All pages")});
    if (selectedPages.isEmpty())
        qobject_cast<QStandardItemModel *>(scope->model())->item(1)->setEnabled(false);
    scope->setCurrentIndex(m_selectionMode == SelectionMode::Page && !selectedPages.isEmpty() ? 1 : 0);
    layout->addWidget(scope);
    auto *targets = new QLabel(&settings);
    targets->setObjectName(QStringLiteral("ocrTargets")); targets->setWordWrap(true);
    const auto updateTargets = [&] {
        QStringList numbers;
        if (scope->currentIndex() == 0) numbers.append(QString::number(currentPage + 1));
        else if (scope->currentIndex() == 1)
            for (const int index : selectedPages) numbers.append(QString::number(index + 1));
        else numbers.append(tr("1–%1").arg(m_pageIds.size()));
        targets->setText(tr("Pages to recognize: %1").arg(numbers.join(QStringLiteral(", "))));
    };
    connect(scope, &QComboBox::currentIndexChanged, &settings, updateTargets);
    layout->addWidget(targets); updateTargets();
    auto *language = new QLabel(
        tr("Document language: %1").arg(selectedLanguage), &settings);
    layout->addWidget(language);
    auto *replaceText = new QCheckBox(tr("Reprocess pages that already contain text"), &settings);
    layout->addWidget(replaceText);
    auto *licenses = new QPushButton(tr("OCR licenses…"), &settings);
    layout->addWidget(licenses);
    connect(licenses, &QPushButton::clicked, &settings, [&] {
        QDialog dialog(&settings); dialog.setWindowTitle(tr("OCR licenses")); dialog.resize(700, 550);
        auto *box = new QVBoxLayout(&dialog); auto *content = new QTextEdit(&dialog); content->setReadOnly(true);
        QString all;
        for (const QString &name : QDir(":/ocr/licenses").entryList(QDir::Files)) {
            QFile file(":/ocr/licenses/" + name);
            if (file.open(QIODevice::ReadOnly)) all += name + "\n\n" + QString::fromUtf8(file.readAll()) + "\n\n";
        }
        content->setPlainText(all); box->addWidget(content); dialog.exec();
    });
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &settings);
    buttons->button(QDialogButtonBox::Ok)->setText(tr("Recognize text"));
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &settings, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &settings, &QDialog::reject);
    settings.resize(520, 390);
    if (settings.exec() != QDialog::Accepted) return;
    QVector<int> indexes;
    if (scope->currentIndex() == 2) {
        indexes.resize(m_pageIds.size()); std::iota(indexes.begin(), indexes.end(), 0);
    } else if (scope->currentIndex() == 1) {
        indexes = selectedPages;
    } else indexes = {currentPage};
    if (indexes.isEmpty()) return;
    const bool reconvert = replaceText->isChecked();
    const auto canceled = std::make_shared<std::atomic_bool>(false);
    QPointer<QProgressDialog> progress = progressDialog(this, tr("Recognizing text…"), indexes.size(), canceled);
    m_transformInProgress = true; emit operationInProgressChanged(true); syncEditControls();
    const auto document = m_document;
    const QPointer<PdfViewerWidget> self(this);
    auto *thread = QThread::create([self, document, indexes, reconvert, selectedLanguage,
                                    progress, canceled] {
        QString error;
        QStringList skippedText, noText, preservedAsImage;
        int converted = 0;
        try {
            for (int i = 0; i < indexes.size() && !canceled->load(); ++i) {
                const int index = indexes[i];
                QMetaObject::invokeMethod(qApp, [progress, index, i] {
                    if (progress) { progress->setLabelText(text("Recognizing page %1…").arg(index + 1)); progress->setValue(i); }
                }, Qt::QueuedConnection);
                const auto objects = document->pageObjects(index, QSize(1000, 1000));
                if (!reconvert && std::any_of(objects.begin(), objects.end(), [](const auto &object) {
                        return object.kind == PdfPageObjectKind::Text && !object.text.trimmed().isEmpty();
                    })) { skippedText.append(QString::number(index + 1)); continue; }
                const QSizeF size = document->pageSizePoints(index);
                const double width = size.width() * 300.0 / 72.0;
                const double height = size.height() * 300.0 / 72.0;
                if (!std::isfinite(width) || !std::isfinite(height) || width < 1 || height < 1
                    || width * height > 24000000) {
                    error = text("This page is too large for OCR (maximum 24 megapixels).");
                    break;
                }
                const QImage image = document->renderPage(index, qRound(width));
                auto page = OcrEngine::recognize(image, size, selectedLanguage, &error,
                                                [canceled] { return canceled->load(); });
                if (!error.isEmpty() || canceled->load()) break;
                qInfo() << "OCR page" << index + 1 << "language" << selectedLanguage
                        << "text boxes" << page.runs.size();
                // There is no pre-apply review in the Acrobat-like flow. Keep
                // every usable recognition result; low-confidence text remains
                // editable and can be corrected directly on the page afterward.
                for (auto &run : page.runs)
                    run.accepted = !run.text.trimmed().isEmpty() && !run.fontData.isEmpty();
                const int editableBeforeBackground = std::count_if(
                    page.runs.cbegin(), page.runs.cend(), [](const auto &run) {
                        return run.accepted;
                    });
                const bool hasAccepted = std::any_of(
                    page.runs.cbegin(), page.runs.cend(), [](const auto &run) {
                        return run.accepted && !run.text.trimmed().isEmpty()
                               && !run.fontData.isEmpty();
                    });
                if (!hasAccepted) {
                    noText.append(QString::number(index + 1));
                    continue;
                }
                QMetaObject::invokeMethod(qApp, [progress, index] {
                    if (progress)
                        progress->setLabelText(text("Building editable page %1…").arg(index + 1));
                }, Qt::QueuedConnection);
                if (!OcrEngine::prepareBackground(&page, [canceled] { return canceled->load(); }))
                    break;
                const int editableAfterBackground = std::count_if(
                    page.runs.cbegin(), page.runs.cend(), [](const auto &run) {
                        return run.accepted;
                    });
                if (editableAfterBackground < editableBeforeBackground)
                    preservedAsImage.append(QString::number(index + 1));
                const bool remainsEditable = std::any_of(
                    page.runs.cbegin(), page.runs.cend(), [](const auto &run) {
                        return run.accepted && !run.text.trimmed().isEmpty()
                               && !run.fontData.isEmpty();
                    });
                if (!remainsEditable) {
                    continue;
                }
                const QByteArray archive = PdfDocument::createOcrPagesArchive({page}, &error);
                if (archive.isEmpty() || !error.isEmpty() || canceled->load()) break;
                bool applied = false;
                const auto pageApplied = std::make_shared<QSemaphore>();
                QMetaObject::Connection operationConnection;
                QMetaObject::Connection destroyedConnection;
                QMetaObject::invokeMethod(qApp, [self, index, archive, &applied, pageApplied,
                                                 &operationConnection, &destroyedConnection] {
                    if (!self) return;
                    // The OCR worker owns the outer progress state, while the
                    // normal page replacement routine owns each individual
                    // asynchronous replacement. Hand control over for this
                    // page, then wake the OCR worker when it has really been
                    // installed and recorded in undo history.
                    self->m_transformInProgress = false;
                    emit self->operationInProgressChanged(false);
                    operationConnection = QObject::connect(
                        self, &PdfViewerWidget::operationInProgressChanged,
                        [pageApplied](bool busy) { if (!busy) pageApplied->release(); });
                    destroyedConnection = QObject::connect(
                        self, &QObject::destroyed, [pageApplied] { pageApplied->release(); });
                    self->replacePagesFromArchive({index}, archive, text("Editable OCR"));
                    if (self->m_editPdfButton) self->m_editPdfButton->setChecked(true);
                    self->setSelectionMode(SelectionMode::Objects);
                    applied = true;
                }, Qt::BlockingQueuedConnection);
                if (!applied) break;
                pageApplied->acquire();
                QObject::disconnect(operationConnection);
                QObject::disconnect(destroyedConnection);
                if (!self) break;
                QMetaObject::invokeMethod(qApp, [self] {
                    if (!self) return;
                    self->m_transformInProgress = true;
                    emit self->operationInProgressChanged(true);
                    self->syncEditControls();
                }, Qt::BlockingQueuedConnection);
                ++converted;
                QMetaObject::invokeMethod(qApp, [progress, i] { if (progress) progress->setValue(i + 1); }, Qt::QueuedConnection);
            }
        } catch (const std::exception &e) { error = QString::fromUtf8(e.what()); }
        QMetaObject::invokeMethod(qApp, [self, converted, skippedText, noText, preservedAsImage,
                                         error, progress,
                                         canceled] {
            if (progress) progress->deleteLater();
            if (!self) return;
            self->m_transformInProgress = false; emit self->operationInProgressChanged(false); self->syncEditControls();
            if (!error.isEmpty()) { QMessageBox::warning(self, text("OCR"), error); return; }
            if (canceled->load() && converted == 0) return;
            QStringList details;
            if (!skippedText.isEmpty()) details.append(text("Pages skipped because they already contain text: %1. Enable reprocessing to convert them.").arg(skippedText.join(", ")));
            if (!noText.isEmpty()) details.append(text("OCR ran but found no text on pages: %1. Check the page selection, document language and scan orientation.").arg(noText.join(", ")));
            if (!preservedAsImage.isEmpty()) details.append(text("Some detected areas could not be safely separated from the background and were preserved as images on pages: %1.").arg(preservedAsImage.join(", ")));
            if (canceled->load() && converted > 0)
                details.prepend(text("OCR was canceled. Pages already completed were preserved."));
            if (converted == 0 || !details.isEmpty())
                QMessageBox::information(self, text("OCR"), details.join("\n\n"));
        }, Qt::QueuedConnection);
    });
    connect(thread, &QThread::finished, thread, &QThread::deleteLater); thread->start();
}

void PdfViewerWidget::applyOcrPages(QVector<OcrPage> pages, const QVector<int> &indexes)
{
    if (m_transformInProgress || m_saveInProgress) return;
    const auto canceled = std::make_shared<std::atomic_bool>(false);
    const QPointer<QProgressDialog> progress = progressDialog(this, tr("Building editable pages…"), pages.size(), canceled);
    m_transformInProgress = true; emit operationInProgressChanged(true); syncEditControls();
    const QPointer<PdfViewerWidget> self(this);
    auto *thread = QThread::create([self, pages = std::move(pages), indexes, progress, canceled]() mutable {
        QString error;
        QByteArray archive;
        try {
            bool ok = true;
            for (int i = 0; i < pages.size(); ++i) {
                if (!OcrEngine::prepareBackground(&pages[i], [canceled] { return canceled->load(); })) { ok = false; break; }
                QMetaObject::invokeMethod(qApp, [progress, i] { if (progress) progress->setValue(i + 1); }, Qt::QueuedConnection);
            }
            if (ok && !canceled->load()) archive = PdfDocument::createOcrPagesArchive(pages, &error);
        } catch (const std::exception &e) { error = QString::fromUtf8(e.what()); }
        QMetaObject::invokeMethod(qApp, [self, indexes, archive, error, progress, canceled] {
            if (progress) progress->deleteLater();
            if (!self) return;
            self->m_transformInProgress = false; emit self->operationInProgressChanged(false); self->syncEditControls();
            if (canceled->load()) return;
            if (archive.isEmpty()) { QMessageBox::warning(self, text("OCR"), error.isEmpty() ? text("OCR reconstruction failed.") : error); return; }
            self->replacePagesFromArchive(indexes, archive, text("Editable OCR"));
            if (self->m_editPdfButton) self->m_editPdfButton->setChecked(true);
            self->setSelectionMode(SelectionMode::Objects);
        }, Qt::QueuedConnection);
    });
    connect(thread, &QThread::finished, thread, &QThread::deleteLater); thread->start();
}

void PdfViewerWidget::replacePagesFromArchive(const QVector<int> &indexes, const QByteArray &archive,
                                              const QString &description)
{
    QVector<quint64> after = m_pageIds, newIds;
    for (const int index : indexes) {
        if (index < 0 || index >= after.size()) return;
        const auto id = m_nextPageId++; after[index] = id; newIds.append(id);
    }
    applyPageStructureChange(description, m_pageIds, after, newIds, archive, newIds);
}

void PdfViewerWidget::editSelectedObject(bool removeObject)
{
    if (!m_valid || m_transformInProgress || m_saveInProgress || m_objectPageIndex < 0
        || m_selectedObject < 0 || m_selectedObject >= m_pageObjects.size()) return;
    const int pageIndex = m_objectPageIndex;
    const auto object = m_pageObjects[m_selectedObject];
    if (object.path.size() != 1) {
        QMessageBox::information(this, tr("Edit object"), tr("This object is inside a grouped form and cannot be removed or cropped individually.")); return;
    }
    const QRectF crop = removeObject ? QRectF() : m_imageCrop;
    const auto document = m_document;
    const QPointer<PdfViewerWidget> self(this);
    m_transformInProgress = true; emit operationInProgressChanged(true); syncEditControls();
    auto *thread = QThread::create([self, document, pageIndex, object, removeObject, crop] {
        const QByteArray archive = document->exportEditedObjectPage(pageIndex, object.path, removeObject, crop);
        QMetaObject::invokeMethod(qApp, [self, pageIndex, archive, removeObject] {
            if (!self) return;
            self->m_transformInProgress = false; emit self->operationInProgressChanged(false); self->syncEditControls();
            if (archive.isEmpty()) { QMessageBox::warning(self, text("Edit object"), text("The object could not be changed.")); return; }
            self->replacePagesFromArchive({pageIndex}, archive, removeObject ? text("Delete object") : text("Crop image"));
        }, Qt::QueuedConnection);
    });
    connect(thread, &QThread::finished, thread, &QThread::deleteLater); thread->start();
}

void PdfViewerWidget::beginImageCrop()
{
    if (!m_valid || m_transformInProgress || m_saveInProgress || m_objectPageIndex < 0
        || m_selectedObject < 0 || m_selectedObject >= m_pageObjects.size()) return;
    const auto &object = m_pageObjects[m_selectedObject];
    if (object.kind != PdfPageObjectKind::Image || object.path.size() != 1 || object.bounds.isEmpty())
        return;
    m_croppingImage = true;
    m_imageCrop = QRectF(0, 0, 1, 1);
    m_cropDragHandle = -1;
    updateSelectionOverlays();
    m_scrollArea->setFocus();
}

void PdfViewerWidget::cancelImageCrop()
{
    if (!m_croppingImage && m_cropDragHandle < 0)
        return;
    m_croppingImage = false;
    m_cropDragHandle = -1;
    m_imageCrop = {};
    updateSelectionOverlays();
}

void PdfViewerWidget::finishImageCrop()
{
    if (!m_croppingImage)
        return;
    const QRectF crop = m_imageCrop.normalized();
    m_croppingImage = false;
    m_cropDragHandle = -1;
    const bool unchanged = crop.left() < 0.002 && crop.top() < 0.002
                           && crop.width() > 0.996 && crop.height() > 0.996;
    if (unchanged || crop.width() < 0.01 || crop.height() < 0.01) {
        m_imageCrop = {};
        updateSelectionOverlays();
        return;
    }
    m_imageCrop = crop;
    editSelectedObject(false);
    m_imageCrop = {};
}

void PdfViewerWidget::updateImageCropDrag(const QPoint &position)
{
    if (m_cropDragHandle < 0 || m_selectedObject < 0 || m_selectedObject >= m_pageObjects.size())
        return;
    const QRect image = m_pageObjects[m_selectedObject].bounds;
    if (image.width() < 2 || image.height() < 2)
        return;
    const double minW = 8.0 / image.width();
    const double minH = 8.0 / image.height();
    if (m_cropDragHandle == 8) {
        const double dx = double(position.x() - m_cropPressPos.x()) / image.width();
        const double dy = double(position.y() - m_cropPressPos.y()) / image.height();
        double x = m_cropPressRect.x() + dx;
        double y = m_cropPressRect.y() + dy;
        x = qBound(0.0, x, 1.0 - m_cropPressRect.width());
        y = qBound(0.0, y, 1.0 - m_cropPressRect.height());
        m_imageCrop = QRectF(x, y, m_cropPressRect.width(), m_cropPressRect.height());
        updateSelectionOverlays();
        return;
    }
    const double nx = qBound(0.0, (position.x() - image.x()) / double(image.width()), 1.0);
    const double ny = qBound(0.0, (position.y() - image.y()) / double(image.height()), 1.0);
    QRectF crop = m_imageCrop;
    const int handle = m_cropDragHandle;
    if (handle == 0 || handle == 6 || handle == 7)
        crop.setLeft(qMin(nx, crop.right() - minW));
    if (handle == 2 || handle == 3 || handle == 4)
        crop.setRight(qMax(nx, crop.left() + minW));
    if (handle == 0 || handle == 1 || handle == 2)
        crop.setTop(qMin(ny, crop.bottom() - minH));
    if (handle == 4 || handle == 5 || handle == 6)
        crop.setBottom(qMax(ny, crop.top() + minH));
    m_imageCrop = crop.normalized();
    updateSelectionOverlays();
}

#include "pdfviewerwidget.h"
#include "ocrengine.h"
#include "ocrreviewdialog.h"

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFile>
#include <QAction>
#include <QFileInfo>
#include <QFormLayout>
#include <QLabel>
#include <QListWidget>
#include <QMessageBox>
#include <QPainter>
#include <QPointer>
#include <QProgressDialog>
#include <QPushButton>
#include <QSettings>
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
QString languageName(const QString &code)
{
    static const QHash<QString, QString> names{{"spa", "Español"}, {"eng", "English"},
        {"fra", "Français"}, {"cat", "Català"}, {"por", "Português"}, {"ita", "Italiano"}, {"deu", "Deutsch"}};
    return names.value(code, code) + " (" + code + ')';
}

QProgressDialog *progressDialog(QWidget *parent, const QString &title, int count,
                               const std::shared_ptr<std::atomic_bool> &canceled)
{
    auto *dialog = new QProgressDialog(title, text("Cancel"), 0, count, parent);
    dialog->setWindowModality(Qt::WindowModal);
    dialog->setAutoClose(false); dialog->setAutoReset(false);
    dialog->setMinimumDuration(0); dialog->setValue(0);
    QObject::connect(dialog, &QProgressDialog::canceled, parent, [canceled] { canceled->store(true); });
    QObject::connect(parent, &QObject::destroyed, dialog, [canceled] { canceled->store(true); });
    return dialog;
}
}

void PdfViewerWidget::recognizeSelectedPages()
{
    if (!m_valid || !m_document || m_transformInProgress || m_saveInProgress) return;
    commitTextEdit();
    QDialog settings(this);
    settings.setWindowTitle(tr("OCR — editable text and background"));
    auto *layout = new QVBoxLayout(&settings);
    auto *hint = new QLabel(tr("Choose the languages used in the document. OCR runs locally. "
                              "The original pages remain available through Undo until the document is closed."), &settings);
    hint->setWordWrap(true); layout->addWidget(hint);
    auto *warning = new QLabel(tr("Conversion rebuilds the selected pages: links, forms and other interactive content are not preserved. Save a separate copy of important originals."), &settings);
    warning->setWordWrap(true); layout->addWidget(warning);
    auto *scope = new QComboBox(&settings);
    scope->addItems({tr("Current page"), tr("Selected pages"), tr("All pages")});
    scope->setCurrentIndex(m_selectedPages.isEmpty() ? 0 : 1);
    layout->addWidget(scope);
    auto *list = new QListWidget(&settings);
    layout->addWidget(list);
    const QStringList saved = QSettings().value("ocr/languages", QStringList{"spa", "eng"}).toStringList();
    const auto populate = [&] {
        list->clear();
        for (const QString &code : OcrEngine::languages()) {
            auto *item = new QListWidgetItem(languageName(code), list);
            item->setData(Qt::UserRole, code);
            item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
            item->setCheckState(saved.contains(code) ? Qt::Checked : Qt::Unchecked);
        }
    };
    populate();
    auto *add = new QPushButton(tr("Add language pack…"), &settings);
    layout->addWidget(add);
    connect(add, &QPushButton::clicked, &settings, [&] {
        const QString path = QFileDialog::getOpenFileName(&settings, tr("Add OCR language"), {},
                                                        tr("Tesseract models (*.traineddata)"));
        if (path.isEmpty()) return;
        const QString directory = OcrEngine::additionalLanguagesDirectory();
        QDir().mkpath(directory);
        if (!QFile::copy(path, directory + '/' + QFileInfo(path).fileName())) {
            QMessageBox::warning(&settings, tr("OCR"), tr("The language could not be added, or already exists."));
            return;
        }
        populate();
    });
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
    buttons->button(QDialogButtonBox::Ok)->setText(tr("Recognize and review"));
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &settings, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &settings, &QDialog::reject);
    settings.resize(520, 540);
    if (settings.exec() != QDialog::Accepted) return;
    QStringList selectedLanguages;
    for (int i = 0; i < list->count(); ++i)
        if (list->item(i)->checkState() == Qt::Checked) selectedLanguages.append(list->item(i)->data(Qt::UserRole).toString());
    if (selectedLanguages.isEmpty()) { QMessageBox::information(this, tr("OCR"), tr("Select at least one language.")); return; }
    QSettings().setValue("ocr/languages", selectedLanguages);
    QVector<int> indexes;
    if (scope->currentIndex() == 2) {
        indexes.resize(m_pageIds.size()); std::iota(indexes.begin(), indexes.end(), 0);
    } else if (scope->currentIndex() == 1) {
        indexes = QVector<int>(m_selectedPages.cbegin(), m_selectedPages.cend());
        std::sort(indexes.begin(), indexes.end());
    } else indexes = {m_currentPageIndex};
    if (indexes.isEmpty()) return;
    const bool reconvert = replaceText->isChecked();
    const auto canceled = std::make_shared<std::atomic_bool>(false);
    QPointer<QProgressDialog> progress = progressDialog(this, tr("Recognizing text…"), indexes.size(), canceled);
    m_transformInProgress = true; emit operationInProgressChanged(true); syncEditControls();
    const auto document = m_document;
    const QPointer<PdfViewerWidget> self(this);
    auto *thread = QThread::create([self, document, indexes, reconvert, selectedLanguages, progress, canceled] {
        QVector<OcrPage> pages;
        QVector<int> converted;
        QString error;
        int skipped = 0;
        qint64 pixels = 0;
        try {
            for (int i = 0; i < indexes.size() && !canceled->load(); ++i) {
                const int index = indexes[i];
                const auto objects = document->pageObjects(index, QSize(1000, 1000));
                if (!reconvert && std::any_of(objects.begin(), objects.end(), [](const auto &object) {
                        return object.kind == PdfPageObjectKind::Text && !object.text.trimmed().isEmpty();
                    })) { ++skipped; continue; }
                const QSizeF size = document->pageSizePoints(index);
                const double width = size.width() * 300.0 / 72.0;
                const double height = size.height() * 300.0 / 72.0;
                if (!std::isfinite(width) || !std::isfinite(height) || width < 1 || height < 1
                    || width * height > 24000000 || (pixels += qint64(width * height)) > 80000000) {
                    error = text("This selection is too large for an OCR batch. Select fewer pages (up to 80 megapixels per batch)."); break;
                }
                const QImage image = document->renderPage(index, qRound(width));
                auto page = OcrEngine::recognize(image, size, selectedLanguages.join('+'), &error,
                                                [canceled] { return canceled->load(); });
                if (!error.isEmpty() || canceled->load()) break;
                if (page.runs.isEmpty()) ++skipped;
                else { pages.append(std::move(page)); converted.append(index); }
                QMetaObject::invokeMethod(qApp, [progress, i] { if (progress) progress->setValue(i + 1); }, Qt::QueuedConnection);
            }
        } catch (const std::exception &e) { error = QString::fromUtf8(e.what()); }
        QMetaObject::invokeMethod(qApp, [self, pages, converted, skipped, error, progress, canceled]() mutable {
            if (progress) progress->deleteLater();
            if (!self) return;
            self->m_transformInProgress = false; emit self->operationInProgressChanged(false); self->syncEditControls();
            if (canceled->load()) return;
            if (!error.isEmpty()) { QMessageBox::warning(self, text("OCR"), error); return; }
            if (pages.isEmpty()) {
                QMessageBox::information(self, text("OCR"), text("No new text was found. Pages with existing text are skipped unless reprocessing is enabled.")); return;
            }
            OcrReviewDialog review(std::move(pages), converted, self);
            if (skipped) review.setWindowTitle(text("Review editable OCR — %1 pages skipped").arg(skipped));
            if (review.exec() != QDialog::Accepted) return;
            auto reviewed = review.pages();
            QVector<OcrPage> accepted;
            QVector<int> targets;
            for (int i = 0; i < reviewed.size(); ++i) {
                if (std::any_of(reviewed[i].runs.begin(), reviewed[i].runs.end(), [](const auto &run) {
                        return run.accepted && !run.text.trimmed().isEmpty() && !run.fontData.isEmpty();
                    })) { accepted.append(std::move(reviewed[i])); targets.append(converted[i]); }
            }
            if (accepted.isEmpty()) return;
            self->applyOcrPages(std::move(accepted), targets);
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
    QRectF crop;
    if (!removeObject) {
        if (object.kind != PdfPageObjectKind::Image) return;
        const QImage image = QImage::fromData(m_document->pageObjectImagePng(pageIndex, object.path));
        if (image.isNull()) return;
        QDialog dialog(this); dialog.setWindowTitle(tr("Crop image without moving text"));
        auto *layout = new QVBoxLayout(&dialog);
        auto *preview = new QLabel(&dialog); layout->addWidget(preview);
        auto *form = new QFormLayout; layout->addLayout(form);
        QVector<QDoubleSpinBox *> margins;
        for (const QString &name : {tr("Left (%)"), tr("Top (%)"), tr("Right (%)"), tr("Bottom (%)")}) {
            auto *spin = new QDoubleSpinBox(&dialog); spin->setRange(0, 99); spin->setDecimals(1);
            margins.append(spin); form->addRow(name, spin);
        }
        auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog); layout->addWidget(buttons);
        const auto update = [&] {
            crop = QRectF(margins[0]->value() / 100, margins[1]->value() / 100,
                           1 - (margins[0]->value() + margins[2]->value()) / 100,
                           1 - (margins[1]->value() + margins[3]->value()) / 100);
            buttons->button(QDialogButtonBox::Ok)->setEnabled(crop.width() > 0.01 && crop.height() > 0.01);
            QImage shown = image.scaled(QSize(620, 430), Qt::KeepAspectRatio, Qt::SmoothTransformation);
            QPainter painter(&shown); painter.setPen(QPen(Qt::red, 2));
            painter.drawRect(QRectF(crop.x() * shown.width(), crop.y() * shown.height(),
                                    crop.width() * shown.width(), crop.height() * shown.height())); painter.end();
            preview->setPixmap(QPixmap::fromImage(shown));
        };
        for (auto *spin : margins) connect(spin, &QDoubleSpinBox::valueChanged, &dialog, update);
        connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
        connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
        update(); if (dialog.exec() != QDialog::Accepted) return;
    }
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

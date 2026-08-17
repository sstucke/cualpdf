#include "mergepdfdialog.h"

#include "pdfdocument.h"

#include <QAbstractItemView>
#include <QApplication>
#include <QDateTime>
#include <QDir>
#include <QDragEnterEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QMessageBox>
#include <QMimeData>
#include <QPainter>
#include <QPointer>
#include <QProgressBar>
#include <QPushButton>
#include <QThread>
#include <QTreeWidget>
#include <QUrl>
#include <QVBoxLayout>

#include <functional>

namespace {
enum ItemDataRole {
    PathRole = Qt::UserRole,
    ItemIdRole,
    ValidPdfRole,
    PageCountRole,
};

bool isPdfPath(const QString &path)
{
    return QFileInfo(path).suffix().compare(QStringLiteral("pdf"),
                                             Qt::CaseInsensitive) == 0;
}

bool pathsReferToSameFile(const QString &firstPath, const QString &secondPath)
{
    const QFileInfo firstInfo(firstPath);
    const QFileInfo secondInfo(secondPath);
    const QString firstIdentity = firstInfo.canonicalFilePath().isEmpty()
                                      ? firstInfo.absoluteFilePath()
                                      : firstInfo.canonicalFilePath();
    const QString secondIdentity = secondInfo.canonicalFilePath().isEmpty()
                                       ? secondInfo.absoluteFilePath()
                                       : secondInfo.canonicalFilePath();
#ifdef Q_OS_WIN
    return firstIdentity.compare(secondIdentity, Qt::CaseInsensitive) == 0;
#else
    return firstIdentity == secondIdentity;
#endif
}

class MergeFileList final : public QTreeWidget
{
public:
    MergeFileList(QString emptyText,
                  std::function<void(const QStringList &)> filesDropped,
                  std::function<void()> orderChanged,
                  QWidget *parent = nullptr)
        : QTreeWidget(parent)
        , m_emptyText(std::move(emptyText))
        , m_filesDropped(std::move(filesDropped))
        , m_orderChanged(std::move(orderChanged))
    {
        setAcceptDrops(true);
        setDragEnabled(true);
        setDragDropMode(QAbstractItemView::InternalMove);
        setDefaultDropAction(Qt::MoveAction);
        setDropIndicatorShown(true);
        setSelectionMode(QAbstractItemView::ExtendedSelection);
        setRootIsDecorated(false);
        setItemsExpandable(false);
    }

protected:
    void paintEvent(QPaintEvent *event) override
    {
        QTreeWidget::paintEvent(event);
        if (topLevelItemCount() != 0)
            return;
        QPainter painter(viewport());
        painter.setPen(palette().color(QPalette::PlaceholderText));
        painter.drawText(viewport()->rect().adjusted(24, 24, -24, -24),
                         Qt::AlignCenter | Qt::TextWordWrap, m_emptyText);
    }

    void dragEnterEvent(QDragEnterEvent *event) override
    {
        if (event->source() == this) {
            QTreeWidget::dragEnterEvent(event);
            return;
        }
        if (hasPdfUrls(event->mimeData()))
            event->acceptProposedAction();
    }

    void dragMoveEvent(QDragMoveEvent *event) override
    {
        if (event->source() == this) {
            QTreeWidget::dragMoveEvent(event);
            return;
        }
        if (hasPdfUrls(event->mimeData()))
            event->acceptProposedAction();
    }

    void dropEvent(QDropEvent *event) override
    {
        if (event->source() == this) {
            QTreeWidget::dropEvent(event);
            if (m_orderChanged)
                m_orderChanged();
            return;
        }

        QStringList paths;
        for (const QUrl &url : event->mimeData()->urls()) {
            const QString path = url.toLocalFile();
            if (!path.isEmpty() && isPdfPath(path))
                paths.append(path);
        }
        if (paths.isEmpty())
            return;
        event->acceptProposedAction();
        if (m_filesDropped)
            m_filesDropped(paths);
    }

private:
    static bool hasPdfUrls(const QMimeData *mimeData)
    {
        if (!mimeData || !mimeData->hasUrls())
            return false;
        for (const QUrl &url : mimeData->urls()) {
            if (url.isLocalFile() && isPdfPath(url.toLocalFile()))
                return true;
        }
        return false;
    }

    QString m_emptyText;
    std::function<void(const QStringList &)> m_filesDropped;
    std::function<void()> m_orderChanged;
};
}

MergePdfDialog::MergePdfDialog(const QString &initialDirectory,
                               const QStringList &openPdfPaths, QWidget *parent)
    : QDialog(parent)
    , m_initialDirectory(initialDirectory)
    , m_openPdfPaths(openPdfPaths)
    , m_fileList(new MergeFileList(
          tr("Drag and drop PDF files here, or use Add Files."),
          [this](const QStringList &paths) { addPaths(paths); },
          [this]() { updateControls(); }, this))
    , m_summaryLabel(new QLabel(this))
    , m_outputEdit(new QLineEdit(this))
    , m_statusLabel(new QLabel(this))
    , m_progressBar(new QProgressBar(this))
    , m_addButton(new QPushButton(tr("Add Files…"), this))
    , m_removeButton(new QPushButton(tr("Remove"), this))
    , m_clearButton(new QPushButton(tr("Clear"), this))
    , m_moveUpButton(new QPushButton(tr("Move Up"), this))
    , m_moveDownButton(new QPushButton(tr("Move Down"), this))
    , m_browseButton(new QPushButton(tr("Browse…"), this))
    , m_mergeButton(new QPushButton(tr("Combine"), this))
    , m_closeButton(new QPushButton(tr("Close"), this))
{
    setWindowTitle(tr("Combine Files"));
    resize(820, 560);

    auto *mainLayout = new QVBoxLayout(this);
    auto *fileButtons = new QHBoxLayout;
    fileButtons->addWidget(m_addButton);
    fileButtons->addWidget(m_removeButton);
    fileButtons->addWidget(m_clearButton);
    fileButtons->addSpacing(16);
    fileButtons->addWidget(m_moveUpButton);
    fileButtons->addWidget(m_moveDownButton);
    fileButtons->addStretch();
    mainLayout->addLayout(fileButtons);

    m_fileList->setHeaderLabels(
        {tr("File"), tr("Pages"), tr("Size"), tr("Modified")});
    m_fileList->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    for (int column = 1; column < 4; ++column)
        m_fileList->header()->setSectionResizeMode(column, QHeaderView::ResizeToContents);
    mainLayout->addWidget(m_fileList, 1);
    m_summaryLabel->setAlignment(Qt::AlignRight);
    mainLayout->addWidget(m_summaryLabel);

    auto *outputLayout = new QHBoxLayout;
    outputLayout->addWidget(new QLabel(tr("Output file:"), this));
    m_outputEdit->setPlaceholderText(tr("Choose an output PDF file"));
    outputLayout->addWidget(m_outputEdit, 1);
    outputLayout->addWidget(m_browseButton);
    mainLayout->addLayout(outputLayout);

    m_statusLabel->setWordWrap(true);
    mainLayout->addWidget(m_statusLabel);
    m_progressBar->setRange(0, 0);
    m_progressBar->hide();
    mainLayout->addWidget(m_progressBar);

    auto *actionLayout = new QHBoxLayout;
    actionLayout->addStretch();
    actionLayout->addWidget(m_mergeButton);
    actionLayout->addWidget(m_closeButton);
    mainLayout->addLayout(actionLayout);

    connect(m_addButton, &QPushButton::clicked, this, &MergePdfDialog::addFiles);
    connect(m_removeButton, &QPushButton::clicked,
            this, &MergePdfDialog::removeSelectedFiles);
    connect(m_clearButton, &QPushButton::clicked, this, &MergePdfDialog::clearFiles);
    connect(m_moveUpButton, &QPushButton::clicked,
            this, [this]() { moveCurrentFile(-1); });
    connect(m_moveDownButton, &QPushButton::clicked,
            this, [this]() { moveCurrentFile(1); });
    connect(m_browseButton, &QPushButton::clicked,
            this, &MergePdfDialog::chooseOutputFile);
    connect(m_mergeButton, &QPushButton::clicked, this, &MergePdfDialog::startMerge);
    connect(m_closeButton, &QPushButton::clicked, this, &QDialog::reject);
    connect(m_outputEdit, &QLineEdit::textChanged, this, &MergePdfDialog::updateControls);
    connect(m_fileList, &QTreeWidget::itemSelectionChanged,
            this, &MergePdfDialog::updateControls);

    updateControls();
}

void MergePdfDialog::reject()
{
    if (!m_mergeInProgress)
        QDialog::reject();
}

void MergePdfDialog::addFiles()
{
    const QStringList paths = QFileDialog::getOpenFileNames(
        this, tr("Add PDF Files"), m_initialDirectory, tr("PDF files (*.pdf)"));
    addPaths(paths);
}

void MergePdfDialog::addPaths(const QStringList &paths)
{
    for (const QString &path : paths) {
        const QFileInfo fileInfo(path);
        if (!fileInfo.exists() || !fileInfo.isFile() || !isPdfPath(path))
            continue;

        const quint64 itemId = m_nextItemId++;
        auto *item = new QTreeWidgetItem;
        item->setText(0, fileInfo.fileName());
        item->setText(1, tr("Loading…"));
        item->setText(2, QLocale().formattedDataSize(fileInfo.size()));
        item->setText(3, QLocale().toString(fileInfo.lastModified(),
                                            QLocale::ShortFormat));
        item->setToolTip(0, fileInfo.absoluteFilePath());
        item->setData(0, PathRole, fileInfo.absoluteFilePath());
        item->setData(0, ItemIdRole, QVariant::fromValue(itemId));
        item->setData(0, ValidPdfRole, false);
        item->setData(0, PageCountRole, 0);
        item->setFlags(item->flags() & ~Qt::ItemIsDropEnabled);
        m_fileList->addTopLevelItem(item);
        inspectFile(itemId, fileInfo.absoluteFilePath());

        if (m_outputEdit->text().trimmed().isEmpty()) {
            m_outputEdit->setText(
                QDir(fileInfo.absolutePath()).filePath(QStringLiteral("combined.pdf")));
        }
    }
    m_fileList->viewport()->update();
    updateControls();
}

void MergePdfDialog::inspectFile(quint64 itemId, const QString &path)
{
    ++m_pendingInspections;
    const QPointer<MergePdfDialog> weakSelf(this);
    QThread *thread = QThread::create([weakSelf, itemId, path]() {
        PdfDocument document(path);
        const int pageCount = document.isValid() ? document.pageCount() : 0;
        QMetaObject::invokeMethod(
            qApp,
            [weakSelf, itemId, pageCount]() {
                if (weakSelf)
                    weakSelf->finishFileInspection(itemId, pageCount);
            },
            Qt::QueuedConnection);
    });
    connect(thread, &QThread::finished, thread, &QThread::deleteLater);
    thread->start();
}

void MergePdfDialog::finishFileInspection(quint64 itemId, int pageCount)
{
    m_pendingInspections = qMax(0, m_pendingInspections - 1);
    for (int row = 0; row < m_fileList->topLevelItemCount(); ++row) {
        QTreeWidgetItem *item = m_fileList->topLevelItem(row);
        if (item->data(0, ItemIdRole).toULongLong() != itemId)
            continue;
        const bool valid = pageCount > 0;
        item->setText(1, valid ? QLocale().toString(pageCount) : tr("Invalid"));
        item->setData(0, ValidPdfRole, valid);
        item->setData(0, PageCountRole, valid ? pageCount : 0);
        if (!valid) {
            item->setToolTip(1, tr("This file is not a readable PDF."));
            m_statusLabel->setText(
                tr("Remove invalid files before combining."));
        }
        break;
    }
    updateControls();
}

void MergePdfDialog::removeSelectedFiles()
{
    const QList<QTreeWidgetItem *> selectedItems = m_fileList->selectedItems();
    for (QTreeWidgetItem *item : selectedItems)
        delete item;
    m_fileList->viewport()->update();
    updateControls();
}

void MergePdfDialog::clearFiles()
{
    m_fileList->clear();
    m_fileList->viewport()->update();
    m_statusLabel->clear();
    updateControls();
}

void MergePdfDialog::moveCurrentFile(int direction)
{
    const QList<QTreeWidgetItem *> selectedItems = m_fileList->selectedItems();
    if (selectedItems.size() != 1)
        return;
    QTreeWidgetItem *item = selectedItems.first();
    const int row = m_fileList->indexOfTopLevelItem(item);
    const int targetRow = row + direction;
    if (targetRow < 0 || targetRow >= m_fileList->topLevelItemCount())
        return;
    item = m_fileList->takeTopLevelItem(row);
    m_fileList->insertTopLevelItem(targetRow, item);
    m_fileList->setCurrentItem(item);
    updateControls();
}

void MergePdfDialog::chooseOutputFile()
{
    QString suggestedPath = m_outputEdit->text().trimmed();
    if (suggestedPath.isEmpty())
        suggestedPath = QDir(m_initialDirectory).filePath(QStringLiteral("combined.pdf"));
    QString path = QFileDialog::getSaveFileName(
        this, tr("Choose Output PDF"), suggestedPath, tr("PDF files (*.pdf)"),
        nullptr, QFileDialog::DontConfirmOverwrite);
    if (path.isEmpty())
        return;
    if (QFileInfo(path).suffix().compare(QStringLiteral("pdf"),
                                         Qt::CaseInsensitive) != 0) {
        path += QStringLiteral(".pdf");
    }
    m_outputEdit->setText(QFileInfo(path).absoluteFilePath());
}

QStringList MergePdfDialog::inputPaths() const
{
    QStringList paths;
    paths.reserve(m_fileList->topLevelItemCount());
    for (int row = 0; row < m_fileList->topLevelItemCount(); ++row)
        paths.append(m_fileList->topLevelItem(row)->data(0, PathRole).toString());
    return paths;
}

void MergePdfDialog::startMerge()
{
    if (m_mergeInProgress || m_pendingInspections > 0)
        return;
    const QStringList paths = inputPaths();
    if (paths.size() < 2)
        return;

    QString outputPath = m_outputEdit->text().trimmed();
    if (QFileInfo(outputPath).suffix().compare(QStringLiteral("pdf"),
                                               Qt::CaseInsensitive) != 0) {
        outputPath += QStringLiteral(".pdf");
    }
    outputPath = QFileInfo(outputPath).absoluteFilePath();
    m_outputEdit->setText(outputPath);

    for (const QString &openPdfPath : m_openPdfPaths) {
        if (!pathsReferToSameFile(outputPath, openPdfPath))
            continue;
        QMessageBox::warning(
            this, tr("PDF Is Open"),
            tr("Close the PDF tab before overwriting this file:\n%1")
                .arg(QDir::toNativeSeparators(outputPath)));
        return;
    }

    if (QFileInfo::exists(outputPath)) {
        QMessageBox messageBox(
            QMessageBox::Warning, tr("File Already Exists"),
            tr("%1 already exists. Do you want to overwrite it?")
                .arg(QDir::toNativeSeparators(outputPath)),
            QMessageBox::NoButton, this);
        QPushButton *overwriteButton = messageBox.addButton(
            tr("Overwrite"), QMessageBox::AcceptRole);
        messageBox.addButton(QMessageBox::Cancel);
        messageBox.setDefaultButton(QMessageBox::Cancel);
        messageBox.exec();
        if (messageBox.clickedButton() != overwriteButton)
            return;
    }

    m_mergeInProgress = true;
    m_statusLabel->setText(tr("Combining PDF files…"));
    m_progressBar->show();
    updateControls();

    const QPointer<MergePdfDialog> weakSelf(this);
    QThread *thread = QThread::create([weakSelf, paths, outputPath]() {
        QString failedInputPath;
        QString fileErrorMessage;
        const bool success = PdfDocument::mergeFiles(
            paths, outputPath, &failedInputPath, &fileErrorMessage);
        QMetaObject::invokeMethod(
            qApp,
            [weakSelf, success, outputPath, failedInputPath, fileErrorMessage]() {
                if (weakSelf) {
                    weakSelf->finishMerge(success, outputPath, failedInputPath,
                                          fileErrorMessage);
                }
            },
            Qt::QueuedConnection);
    });
    connect(thread, &QThread::finished, thread, &QThread::deleteLater);
    thread->start();
}

void MergePdfDialog::finishMerge(bool success, const QString &outputPath,
                                 const QString &failedInputPath,
                                 const QString &fileErrorMessage)
{
    m_mergeInProgress = false;
    m_progressBar->hide();
    if (success) {
        m_statusLabel->setText(
            tr("Combined PDF saved to %1")
                .arg(QDir::toNativeSeparators(outputPath)));
        emit mergeCompleted(outputPath);
    } else if (!failedInputPath.isEmpty()) {
        m_statusLabel->setText(
            tr("Could not read %1 as a PDF.")
                .arg(QDir::toNativeSeparators(failedInputPath)));
    } else if (!fileErrorMessage.isEmpty()) {
        m_statusLabel->setText(
            tr("Could not write %1.\n\n%2")
                .arg(QDir::toNativeSeparators(outputPath), fileErrorMessage));
    } else {
        m_statusLabel->setText(tr("The PDF files could not be combined."));
    }
    updateControls();
}

void MergePdfDialog::updateControls()
{
    const bool editable = !m_mergeInProgress;
    const QList<QTreeWidgetItem *> selectedItems = m_fileList->selectedItems();
    m_addButton->setEnabled(editable);
    m_removeButton->setEnabled(editable && !selectedItems.isEmpty());
    m_clearButton->setEnabled(editable && m_fileList->topLevelItemCount() > 0);
    m_fileList->setEnabled(editable);
    m_outputEdit->setEnabled(editable);
    m_browseButton->setEnabled(editable);
    m_closeButton->setEnabled(editable);

    const bool oneSelected = selectedItems.size() == 1;
    const int selectedRow = oneSelected
                                ? m_fileList->indexOfTopLevelItem(selectedItems.first())
                                : -1;
    m_moveUpButton->setEnabled(editable && oneSelected && selectedRow > 0);
    m_moveDownButton->setEnabled(
        editable && oneSelected && selectedRow >= 0
        && selectedRow + 1 < m_fileList->topLevelItemCount());

    bool allValid = m_fileList->topLevelItemCount() >= 2
                    && m_pendingInspections == 0;
    int totalPages = 0;
    for (int row = 0; allValid && row < m_fileList->topLevelItemCount(); ++row) {
        QTreeWidgetItem *item = m_fileList->topLevelItem(row);
        allValid = item->data(0, ValidPdfRole).toBool();
        totalPages += item->data(0, PageCountRole).toInt();
    }
    if (!allValid) {
        totalPages = 0;
        for (int row = 0; row < m_fileList->topLevelItemCount(); ++row) {
            totalPages += m_fileList->topLevelItem(row)
                              ->data(0, PageCountRole).toInt();
        }
    }
    m_summaryLabel->setText(tr("%n page(s) total", "", totalPages));
    m_mergeButton->setEnabled(
        editable && allValid && !m_outputEdit->text().trimmed().isEmpty());
}

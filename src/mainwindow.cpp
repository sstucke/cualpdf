#include "mainwindow.h"
#include "ui_mainwindow.h"

#include "foldercontentmodel.h"
#include "pdfviewerwidget.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFileSystemModel>
#include <QFrame>
#include <QItemSelection>
#include <QLabel>
#include <QLayoutItem>
#include <QLocale>
#include <QMessageBox>
#include <QSlider>
#include <QStandardPaths>
#include <QStyle>
#include <QTabBar>
#include <QToolButton>

namespace {
constexpr int kInlinePreviewWidth = 220;
constexpr int kFullViewerPageWidth = 900;
}

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
    , ui(new Ui::MainWindow)
    , treeModel(new QFileSystemModel(this))
    , contentModel(new FolderContentModel(this))
    , itemCountLabel(new QLabel(this))
    , zoomSlider(new QSlider(Qt::Horizontal, this))
{
    ui->setupUi(this);

    treeModel->setRootPath(QString());
    treeModel->setFilter(QDir::AllDirs | QDir::NoDotAndDotDot);
    ui->folderTreeView->setModel(treeModel);
    ui->folderTreeView->setRootIndex(treeModel->index(QDir::rootPath()));
    for (int column = 1; column < treeModel->columnCount(); ++column)
        ui->folderTreeView->hideColumn(column);

    contentModel->setThumbnailSize(kDefaultIconSize);
    ui->folderContentView->setModel(contentModel);

    zoomSlider->setRange(kMinIconSize, kMaxIconSize);
    zoomSlider->setValue(kDefaultIconSize);
    zoomSlider->setFixedWidth(120);
    zoomSlider->setToolTip(tr("Thumbnail size"));

    ui->statusbar->addWidget(itemCountLabel);
    ui->statusbar->addPermanentWidget(zoomSlider);

    ui->tabWidget->setTabsClosable(true);
    if (QTabBar *tabBar = ui->tabWidget->tabBar()) {
        const auto closeButtonSide = static_cast<QTabBar::ButtonPosition>(
            style()->styleHint(QStyle::SH_TabBar_CloseButtonPosition, nullptr, tabBar));
        if (QWidget *closeButton = tabBar->tabButton(0, closeButtonSide))
            closeButton->hide();
    }

    connect(ui->folderTreeView->selectionModel(), &QItemSelectionModel::currentChanged, this,
            [this](const QModelIndex &current, const QModelIndex &) { onTreeCurrentChanged(current); });
    connect(ui->folderContentView, &QAbstractItemView::activated, this, &MainWindow::onContentActivated);
    // selectionChanged (not currentChanged) is what actually reflects
    // selectedIndexes(): a mouse click updates the current index on press but
    // only commits the selection on release, so currentChanged can fire
    // while selectedIndexes() is still empty.
    connect(ui->folderContentView->selectionModel(), &QItemSelectionModel::selectionChanged, this,
            [this](const QItemSelection &, const QItemSelection &) { onContentSelectionChanged(); });

    connect(ui->tabWidget, &QTabWidget::tabCloseRequested, this, &MainWindow::onTabCloseRequested);

    connect(ui->actionOpenFolder, &QAction::triggered, this, &MainWindow::openFolder);
    connect(ui->actionExit, &QAction::triggered, this, &QWidget::close);
    connect(ui->actionGoUp, &QAction::triggered, this, &MainWindow::goToParentFolder);
    connect(ui->actionRefresh, &QAction::triggered, this, &MainWindow::refreshCurrentFolder);
    connect(ui->actionZoomIn, &QAction::triggered, this, &MainWindow::zoomIn);
    connect(ui->actionZoomOut, &QAction::triggered, this, &MainWindow::zoomOut);
    connect(ui->actionResetZoom, &QAction::triggered, this, &MainWindow::resetZoom);
    connect(ui->actionAbout, &QAction::triggered, this, &MainWindow::showAboutDialog);
    connect(zoomSlider, &QSlider::valueChanged, this, &MainWindow::onZoomSliderChanged);

    clearDetailsPanel();
    setCurrentFolder(QStandardPaths::writableLocation(QStandardPaths::HomeLocation));
}

MainWindow::~MainWindow()
{
    delete ui;
}

void MainWindow::openFolder()
{
    const QString path = QFileDialog::getExistingDirectory(this, tr("Open Folder"), currentFolderPath);
    if (!path.isEmpty())
        setCurrentFolder(path);
}

void MainWindow::goToParentFolder()
{
    QDir dir(currentFolderPath);
    if (dir.cdUp())
        setCurrentFolder(dir.absolutePath());
}

void MainWindow::refreshCurrentFolder()
{
    setCurrentFolder(currentFolderPath);
}

void MainWindow::onTreeCurrentChanged(const QModelIndex &current)
{
    if (current.isValid())
        setCurrentFolder(treeModel->filePath(current));
}

void MainWindow::onContentActivated(const QModelIndex &index)
{
    if (!index.isValid())
        return;

    const QString path = contentModel->data(index, FolderContentModel::FilePathRole).toString();
    const bool isDir = contentModel->data(index, FolderContentModel::IsDirRole).toBool();

    if (isDir)
        setCurrentFolder(path);
    else
        openPdfViewerTab(path);
}

void MainWindow::onContentSelectionChanged()
{
    const QModelIndexList selected = ui->folderContentView->selectionModel()->selectedIndexes();
    if (selected.isEmpty()) {
        clearDetailsPanel();
        return;
    }

    const QModelIndex index = selected.first();
    if (contentModel->data(index, FolderContentModel::IsDirRole).toBool()) {
        clearDetailsPanel();
        return;
    }

    updateDetailsPanel(contentModel->data(index, FolderContentModel::FilePathRole).toString());
}

void MainWindow::onZoomSliderChanged(int value)
{
    ui->folderContentView->setIconSize(QSize(value, value));
    contentModel->setThumbnailSize(value);
}

void MainWindow::zoomIn()
{
    zoomSlider->setValue(qMin(zoomSlider->value() + kZoomStep, kMaxIconSize));
}

void MainWindow::zoomOut()
{
    zoomSlider->setValue(qMax(zoomSlider->value() - kZoomStep, kMinIconSize));
}

void MainWindow::resetZoom()
{
    zoomSlider->setValue(kDefaultIconSize);
}

void MainWindow::showAboutDialog()
{
    QMessageBox::about(this, tr("About cualpdf"),
                        tr("%1 %2\nA fast, lightweight, open-source PDF editor.")
                            .arg(QCoreApplication::applicationName(), QCoreApplication::applicationVersion()));
}

void MainWindow::onTabCloseRequested(int index)
{
    if (index == 0) // The Explorer tab is permanent.
        return;

    QWidget *widget = ui->tabWidget->widget(index);
    ui->tabWidget->removeTab(index);
    widget->deleteLater();
}

void MainWindow::setCurrentFolder(const QString &path)
{
    if (path.isEmpty() || !QDir(path).exists())
        return;

    currentFolderPath = path;

    const QModelIndex treeIndex = treeModel->index(path);
    ui->folderTreeView->setCurrentIndex(treeIndex);
    ui->folderTreeView->scrollTo(treeIndex);
    ui->folderTreeView->expand(treeIndex);

    contentModel->setDirectory(path);

    clearDetailsPanel();
    updateStatusBarItemCount();
}

void MainWindow::updateStatusBarItemCount()
{
    itemCountLabel->setText(tr("%n item(s)", "", contentModel->rowCount()));
}

void MainWindow::updateDetailsPanel(const QString &filePath)
{
    const QFileInfo info(filePath);

    ui->nameValueLabel->setText(info.fileName());
    ui->sizeValueLabel->setText(tr("Size: %1").arg(QLocale().formattedDataSize(info.size())));

    const QDateTime created = info.birthTime();
    ui->createdValueLabel->setText(tr("Created: %1")
                                        .arg(created.isValid()
                                                 ? QLocale().toString(created, QLocale::ShortFormat)
                                                 : tr("Unknown")));
    ui->modifiedValueLabel->setText(
        tr("Modified: %1").arg(QLocale().toString(info.lastModified(), QLocale::ShortFormat)));

    QLayoutItem *item;
    while ((item = ui->previewContainerLayout->takeAt(0)) != nullptr) {
        delete item->widget();
        delete item;
    }

    auto *viewer = new PdfViewerWidget(filePath, kInlinePreviewWidth, ui->previewContainer);
    ui->previewContainerLayout->addWidget(viewer);

    ui->pagesValueLabel->setText(viewer->isValid() ? tr("Pages: %1").arg(viewer->pageCount())
                                                    : tr("Pages: Unknown"));

    const bool canPage = viewer->isValid() && viewer->pageCount() > 1;
    ui->previewPrevPageButton->setEnabled(canPage);
    ui->previewNextPageButton->setEnabled(canPage);

    if (viewer->isValid()) {
        ui->previewPageLabel->setText(tr("Page %1 / %2").arg(1).arg(viewer->pageCount()));
        connect(viewer, &PdfViewerWidget::currentPageChanged, this, [this, viewer](int pageIndex) {
            ui->previewPageLabel->setText(tr("Page %1 / %2").arg(pageIndex + 1).arg(viewer->pageCount()));
        });
        connect(ui->previewPrevPageButton, &QToolButton::clicked, viewer,
                [viewer]() { viewer->goToPage(viewer->currentPageIndex() - 1); });
        connect(ui->previewNextPageButton, &QToolButton::clicked, viewer,
                [viewer]() { viewer->goToPage(viewer->currentPageIndex() + 1); });
    } else {
        ui->previewPageLabel->setText(tr("Page — / —"));
    }
}

void MainWindow::clearDetailsPanel()
{
    ui->nameValueLabel->setText(QStringLiteral("—"));
    ui->sizeValueLabel->setText(QStringLiteral("—"));
    ui->pagesValueLabel->setText(QStringLiteral("—"));
    ui->createdValueLabel->setText(QStringLiteral("—"));
    ui->modifiedValueLabel->setText(QStringLiteral("—"));

    QLayoutItem *item;
    while ((item = ui->previewContainerLayout->takeAt(0)) != nullptr) {
        delete item->widget();
        delete item;
    }

    auto *placeholder = new QLabel(tr("No file selected"), ui->previewContainer);
    placeholder->setFrameShape(QFrame::StyledPanel);
    placeholder->setAlignment(Qt::AlignCenter);
    placeholder->setWordWrap(true);
    ui->previewContainerLayout->addWidget(placeholder);

    ui->previewPageLabel->setText(tr("Page — / —"));
    ui->previewPrevPageButton->setEnabled(false);
    ui->previewNextPageButton->setEnabled(false);
}

void MainWindow::openPdfViewerTab(const QString &filePath)
{
    for (int i = 0; i < ui->tabWidget->count(); ++i) {
        if (ui->tabWidget->widget(i)->property("filePath").toString() == filePath) {
            ui->tabWidget->setCurrentIndex(i);
            return;
        }
    }

    auto *viewer = new PdfViewerWidget(filePath, kFullViewerPageWidth, ui->tabWidget);
    viewer->setProperty("filePath", filePath);

    const int index = ui->tabWidget->addTab(viewer, QFileInfo(filePath).fileName());
    ui->tabWidget->setCurrentIndex(index);
}

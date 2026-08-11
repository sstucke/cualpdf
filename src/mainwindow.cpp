#include "mainwindow.h"
#include "ui_mainwindow.h"

#include "foldercontentmodel.h"
#include "pdflistitemdelegate.h"
#include "pdfviewerwidget.h"

#include <QActionGroup>
#include <QCoreApplication>
#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFileSystemModel>
#include <QFrame>
#include <QItemSelection>
#include <QLabel>
#include <QLayoutItem>
#include <QLocale>
#include <QMenu>
#include <QMessageBox>
#include <QSlider>
#include <QStandardPaths>
#include <QStyle>
#include <QStyledItemDelegate>
#include <QTabBar>
#include <QToolButton>
#include <QUrl>

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
#ifdef Q_OS_WIN
    // The empty-path index is Windows' "My Computer" node, showing every
    // drive letter and mapped network drive as top-level items. On
    // Linux/macOS the same empty-path index just wraps "/" in an extra,
    // pointless expandable node, so root at "/" directly there instead.
    ui->folderTreeView->setRootIndex(treeModel->index(treeModel->rootPath()));
#else
    ui->folderTreeView->setRootIndex(treeModel->index(QDir::rootPath()));
#endif
    for (int column = 1; column < treeModel->columnCount(); ++column)
        ui->folderTreeView->hideColumn(column);

    ui->folderContentView->setModel(contentModel);
    defaultContentDelegate = new QStyledItemDelegate(ui->folderContentView);
    detailsContentDelegate = new PdfListItemDelegate(ui->folderContentView);

    zoomSlider->setRange(kMinIconSize, kMaxIconSize);
    zoomSlider->setValue(kDefaultThumbnailSize);
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
    connect(ui->folderContentView, &QWidget::customContextMenuRequested, this,
            &MainWindow::onContentContextMenuRequested);
    connect(ui->locationEdit, &QLineEdit::returnPressed, this, &MainWindow::onLocationEditReturnPressed);

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

    setupViewModeMenu();
    applyContentViewMode(appSettings.contentViewMode());

    recentFiles = appSettings.recentFiles();
    connect(ui->menuRecent, &QMenu::aboutToShow, this, &MainWindow::rebuildRecentFilesMenu);
    rebuildRecentFilesMenu();

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

void MainWindow::onContentContextMenuRequested(const QPoint &pos)
{
    const QModelIndex index = ui->folderContentView->indexAt(pos);
    if (!index.isValid())
        return;

    const QString path = contentModel->data(index, FolderContentModel::FilePathRole).toString();
    const bool isDir = contentModel->data(index, FolderContentModel::IsDirRole).toBool();

    QMenu menu(this);
    QAction *openAction = menu.addAction(tr("Open"));
    QAction *openWithAction = isDir ? nullptr : menu.addAction(tr("Open with system default"));

    QAction *chosen = menu.exec(ui->folderContentView->viewport()->mapToGlobal(pos));
    if (chosen == openAction) {
        if (isDir)
            setCurrentFolder(path);
        else
            openPdfViewerTab(path);
    } else if (chosen && chosen == openWithAction) {
        openWithSystemDefault(path);
    }
}

void MainWindow::onLocationEditReturnPressed()
{
    QString path = ui->locationEdit->text().trimmed();
    if (path.startsWith(QLatin1Char('~')))
        path.replace(0, 1, QDir::homePath());

    const QFileInfo info(path);
    if (!info.exists() || !info.isDir() || !info.isReadable()) {
        ui->locationEdit->setText(currentFolderPath);
        ui->statusbar->showMessage(
            tr("Can't open \"%1\": it doesn't exist or isn't accessible.").arg(path), 4000);
        return;
    }

    setCurrentFolder(info.absoluteFilePath());
}

void MainWindow::onZoomSliderChanged(int value)
{
    ui->folderContentView->setIconSize(QSize(value, value));
    ui->folderContentView->setGridSize(QSize(value + 2 * kGridCellPadding, value + kGridTextHeight));
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
    zoomSlider->setValue(kDefaultThumbnailSize);
}

void MainWindow::setupViewModeMenu()
{
    auto *viewModeButton = new QToolButton(this);
    viewModeButton->setObjectName(QStringLiteral("viewModeButton"));
    viewModeButton->setText(tr("View"));
    viewModeButton->setPopupMode(QToolButton::InstantPopup);
    viewModeButton->setToolButtonStyle(Qt::ToolButtonTextOnly);

    auto *viewModeMenu = new QMenu(viewModeButton);
    viewModeActionGroup = new QActionGroup(this);
    viewModeActionGroup->setExclusive(true);

    const QList<QPair<AppSettings::ContentViewMode, QString>> modes = {
        {AppSettings::ContentViewMode::Thumbnails, tr("Thumbnails")},
        {AppSettings::ContentViewMode::Details, tr("Details")},
        {AppSettings::ContentViewMode::CompactList, tr("Compact List")},
    };

    for (const auto &[mode, label] : modes) {
        QAction *action = viewModeMenu->addAction(label);
        action->setCheckable(true);
        action->setData(static_cast<int>(mode));
        viewModeActionGroup->addAction(action);
        connect(action, &QAction::triggered, this, [this, mode]() { applyContentViewMode(mode); });
    }

    viewModeButton->setMenu(viewModeMenu);
    ui->mainToolBar->addWidget(viewModeButton);

    // Mirror the same actions (shared QAction state) into the existing View
    // menu in the menu bar, alongside the zoom actions, for discoverability.
    ui->menuView->insertActions(ui->actionZoomIn, viewModeActionGroup->actions());
    ui->menuView->insertSeparator(ui->actionZoomIn);
}

void MainWindow::applyContentViewMode(AppSettings::ContentViewMode mode)
{
    switch (mode) {
    case AppSettings::ContentViewMode::Thumbnails: {
        const int thumbSize = zoomSlider->value();
        ui->folderContentView->setItemDelegate(defaultContentDelegate);
        ui->folderContentView->setViewMode(QListView::IconMode);
        ui->folderContentView->setFlow(QListView::LeftToRight);
        ui->folderContentView->setWrapping(true);
        ui->folderContentView->setWordWrap(true);
        ui->folderContentView->setUniformItemSizes(true);
        ui->folderContentView->setSpacing(12);
        ui->folderContentView->setIconSize(QSize(thumbSize, thumbSize));
        ui->folderContentView->setGridSize(
            QSize(thumbSize + 2 * kGridCellPadding, thumbSize + kGridTextHeight));
        contentModel->setThumbnailSize(thumbSize);
        zoomSlider->setEnabled(true);
        break;
    }
    case AppSettings::ContentViewMode::Details:
        ui->folderContentView->setItemDelegate(detailsContentDelegate);
        ui->folderContentView->setViewMode(QListView::ListMode);
        ui->folderContentView->setFlow(QListView::TopToBottom);
        ui->folderContentView->setWrapping(false);
        ui->folderContentView->setUniformItemSizes(false);
        ui->folderContentView->setSpacing(2);
        ui->folderContentView->setGridSize(QSize());
        ui->folderContentView->setIconSize(QSize(kDetailsIconSize, kDetailsIconSize));
        contentModel->setThumbnailSize(kDetailsIconSize);
        zoomSlider->setEnabled(false);
        break;
    case AppSettings::ContentViewMode::CompactList:
        ui->folderContentView->setItemDelegate(defaultContentDelegate);
        ui->folderContentView->setViewMode(QListView::ListMode);
        ui->folderContentView->setFlow(QListView::TopToBottom);
        ui->folderContentView->setWrapping(false);
        ui->folderContentView->setUniformItemSizes(false);
        ui->folderContentView->setSpacing(0);
        ui->folderContentView->setGridSize(QSize());
        ui->folderContentView->setIconSize(QSize(kCompactIconSize, kCompactIconSize));
        contentModel->setThumbnailSize(kCompactIconSize);
        zoomSlider->setEnabled(false);
        break;
    }

    for (QAction *action : viewModeActionGroup->actions()) {
        if (action->data().toInt() == static_cast<int>(mode)) {
            action->setChecked(true);
            break;
        }
    }

    appSettings.setContentViewMode(mode);
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
    ui->locationEdit->setText(path);

    const QModelIndex treeIndex = treeModel->index(path);
    ui->folderTreeView->setCurrentIndex(treeIndex);
    ui->folderTreeView->scrollTo(treeIndex);
    ui->folderTreeView->expand(treeIndex);

    contentModel->setDirectory(path);
    ui->folderContentView->scrollToTop();

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

    // The viewer opens and lays out the document on a worker thread so a
    // large/complex file never blocks the UI; these labels show a loading
    // placeholder until PdfViewerWidget::documentLoaded fires.
    ui->pagesValueLabel->setText(tr("Pages: %1").arg(tr("Loading…")));
    ui->previewPageLabel->setText(tr("Page — / —"));
    ui->previewPrevPageButton->setEnabled(false);
    ui->previewNextPageButton->setEnabled(false);

    connect(viewer, &PdfViewerWidget::documentLoaded, this, [this, viewer](bool valid, int pageCount) {
        ui->pagesValueLabel->setText(valid ? tr("Pages: %1").arg(pageCount) : tr("Pages: Unknown"));

        const bool canPage = valid && pageCount > 1;
        ui->previewPrevPageButton->setEnabled(canPage);
        ui->previewNextPageButton->setEnabled(canPage);

        if (valid)
            ui->previewPageLabel->setText(tr("Page %1 / %2").arg(1).arg(pageCount));
    });
    connect(viewer, &PdfViewerWidget::currentPageChanged, this, [this, viewer](int pageIndex) {
        ui->previewPageLabel->setText(tr("Page %1 / %2").arg(pageIndex + 1).arg(viewer->pageCount()));
    });
    connect(ui->previewPrevPageButton, &QToolButton::clicked, viewer,
            [viewer]() { viewer->goToPage(viewer->currentPageIndex() - 1); });
    connect(ui->previewNextPageButton, &QToolButton::clicked, viewer,
            [viewer]() { viewer->goToPage(viewer->currentPageIndex() + 1); });
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
    addRecentFile(filePath);

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

void MainWindow::openWithSystemDefault(const QString &filePath)
{
    QDesktopServices::openUrl(QUrl::fromLocalFile(filePath));
}

void MainWindow::addRecentFile(const QString &filePath)
{
    recentFiles.removeAll(filePath);
    recentFiles.prepend(filePath);
    while (recentFiles.size() > kMaxRecentFiles)
        recentFiles.removeLast();

    appSettings.setRecentFiles(recentFiles);
    // Rebuilt lazily on menuRecent::aboutToShow instead of here: this can run
    // while a QAction from this very menu is still mid-triggered() (opening
    // a file from Recent calls back into here), and deleting that action's
    // menu out from under it via clear() in the same call stack is unsafe.
}

void MainWindow::rebuildRecentFilesMenu()
{
    ui->menuRecent->clear();

    QStringList stillValid;
    for (const QString &path : std::as_const(recentFiles)) {
        if (!QFileInfo::exists(path))
            continue;
        stillValid.append(path);

        QAction *action = ui->menuRecent->addAction(QFileInfo(path).fileName());
        action->setToolTip(path);
        connect(action, &QAction::triggered, this, [this, path]() { openPdfViewerTab(path); });
    }

    if (stillValid.size() != recentFiles.size()) {
        recentFiles = stillValid;
        appSettings.setRecentFiles(recentFiles);
    }

    if (stillValid.isEmpty()) {
        QAction *emptyAction = ui->menuRecent->addAction(tr("(No recent files)"));
        emptyAction->setEnabled(false);
    }
}

#include "mainwindow.h"
#include "ui_mainwindow.h"

#include "foldercontentmodel.h"
#include "pdfgriditemdelegate.h"
#include "pdflistitemdelegate.h"
#include "pdfviewerwidget.h"
#include "preferencesdialog.h"
#include "tipsdialog.h"

#include <QActionGroup>
#include <QAbstractButton>
#include <QCoreApplication>
#include <QCloseEvent>
#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QEventLoop>
#include <QFileDialog>
#include <QFileIconProvider>
#include <QFileInfo>
#include <QFileSystemModel>
#include <QFileSystemWatcher>
#include <QFrame>
#include <QHBoxLayout>
#include <QItemSelection>
#include <QKeySequence>
#include <QLabel>
#include <QLayoutItem>
#include <QListWidget>
#include <QLocale>
#include <QMenu>
#include <QMessageBox>
#include <QPainter>
#include <QPalette>
#include <QProcess>
#include <QProgressDialog>
#include <QPushButton>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QSlider>
#include <QStandardPaths>
#include <QStyle>
#include <QStyledItemDelegate>
#include <QTabBar>
#include <QToolButton>
#include <QTimer>
#include <QUrl>

namespace {
constexpr int kInlinePreviewWidth = 220;
constexpr int kFullViewerPageWidth = 900;

QPixmap zoomIcon(bool zoomIn, const QColor &color)
{
    QPixmap pixmap(22, 22);
    pixmap.fill(Qt::transparent);

    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing);
    QPen pen(color, 1.8, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
    painter.setPen(pen);
    painter.setBrush(Qt::NoBrush);
    painter.drawEllipse(QRectF(3.0, 3.0, 11.0, 11.0));
    painter.drawLine(QPointF(12.3, 12.3), QPointF(19.0, 19.0));
    painter.drawLine(QPointF(5.8, 8.5), QPointF(11.2, 8.5));
    if (zoomIn)
        painter.drawLine(QPointF(8.5, 5.8), QPointF(8.5, 11.2));
    return pixmap;
}

QWidget *makeZoomControl(QSlider *slider, const QString &toolTip,
                         const QString &zoomOutToolTip, const QString &zoomInToolTip,
                         QWidget *parent)
{
    auto *control = new QWidget(parent);
    auto *layout = new QHBoxLayout(control);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(2);
    layout->setSizeConstraint(QLayout::SetFixedSize);
    control->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);

    const QColor iconColor = control->palette().color(QPalette::WindowText);
    auto *zoomOutButton = new QToolButton(control);
    zoomOutButton->setAutoRaise(true);
    zoomOutButton->setFixedSize(24, 24);
    zoomOutButton->setIconSize(QSize(22, 22));
    zoomOutButton->setIcon(QIcon(zoomIcon(false, iconColor)));
    zoomOutButton->setToolTip(zoomOutToolTip);
    auto *zoomInButton = new QToolButton(control);
    zoomInButton->setAutoRaise(true);
    zoomInButton->setFixedSize(24, 24);
    zoomInButton->setIconSize(QSize(22, 22));
    zoomInButton->setIcon(QIcon(zoomIcon(true, iconColor)));
    zoomInButton->setToolTip(zoomInToolTip);

    slider->setToolTip(toolTip);
    layout->addWidget(zoomOutButton);
    layout->addWidget(slider);
    layout->addWidget(zoomInButton);
    QObject::connect(zoomOutButton, &QToolButton::clicked, slider, [slider]() {
        slider->setValue(slider->value() - slider->singleStep());
    });
    QObject::connect(zoomInButton, &QToolButton::clicked, slider, [slider]() {
        slider->setValue(slider->value() + slider->singleStep());
    });
    return control;
}
}

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
    , ui(new Ui::MainWindow)
    , treeModel(new QFileSystemModel(this))
    , contentModel(new FolderContentModel(this))
    , itemCountLabel(new QLabel(this))
    , zoomSlider(new QSlider(Qt::Horizontal, this))
    , directoryWatcher(new QFileSystemWatcher(this))
    , directoryRefreshTimer(new QTimer(this))
{
    ui->setupUi(this);
    // Qt replaces [*] with the platform's native modified-document marker.
    setWindowTitle(windowTitle() + QStringLiteral("[*]"));

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
    directoryRefreshTimer->setSingleShot(true);
    directoryRefreshTimer->setInterval(250);
    connect(directoryRefreshTimer, &QTimer::timeout,
            this, &MainWindow::refreshFolderContentsPreservingSelection);
    connect(directoryWatcher, &QFileSystemWatcher::directoryChanged,
            this, [this](const QString &path) {
                if (QDir::cleanPath(path) == QDir::cleanPath(currentFolderPath))
                    scheduleFolderRefresh();
            });
    defaultContentDelegate = new QStyledItemDelegate(ui->folderContentView);
    detailsContentDelegate = new PdfListItemDelegate(ui->folderContentView);
    gridContentDelegate = new PdfGridItemDelegate(ui->folderContentView);

    zoomSlider->setRange(kMinIconSize, kMaxIconSize);
    zoomSlider->setSingleStep(kZoomStep);
    zoomSlider->setValue(kDefaultThumbnailSize);
    zoomSlider->setFixedWidth(120);
    thumbnailZoomWidget = makeZoomControl(
        zoomSlider, tr("Thumbnail zoom"), tr("Zoom out thumbnails"),
        tr("Zoom in thumbnails"), this);

    pageZoomSlider = new QSlider(Qt::Horizontal, this);
    pageZoomSlider->setRange(10, 400);
    pageZoomSlider->setSingleStep(5);
    pageZoomSlider->setPageStep(25);
    pageZoomSlider->setValue(100);
    pageZoomSlider->setFixedWidth(140);
    pageZoomWidget = makeZoomControl(
        pageZoomSlider, tr("Page zoom"), tr("Zoom out page"), tr("Zoom in page"), this);

    ui->statusbar->addWidget(itemCountLabel);
    ui->statusbar->addPermanentWidget(thumbnailZoomWidget);
    ui->statusbar->addPermanentWidget(pageZoomWidget);

    ui->tabWidget->setTabsClosable(true);
    if (QTabBar *tabBar = ui->tabWidget->tabBar()) {
        tabBar->setContextMenuPolicy(Qt::CustomContextMenu);
        connect(tabBar, &QTabBar::customContextMenuRequested, this,
                &MainWindow::onTabContextMenuRequested);

        const auto closeButtonSide = static_cast<QTabBar::ButtonPosition>(
            style()->styleHint(QStyle::SH_TabBar_CloseButtonPosition, nullptr, tabBar));
        if (QWidget *closeButton = tabBar->tabButton(0, closeButtonSide))
            closeButton->hide();
    }

    connect(ui->folderTreeView->selectionModel(), &QItemSelectionModel::currentChanged, this,
            [this](const QModelIndex &current, const QModelIndex &) { onTreeCurrentChanged(current); });
    connect(ui->folderTreeView, &QWidget::customContextMenuRequested,
            this, &MainWindow::onTreeContextMenuRequested);
    connect(ui->favoritesListWidget, &QWidget::customContextMenuRequested,
            this, &MainWindow::onFavoritesContextMenuRequested);
    connect(ui->favoritesListWidget, &QListWidget::itemClicked,
            this, &MainWindow::onFavoriteItemActivated);
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
    connect(ui->tabWidget, &QTabWidget::currentChanged, this,
            [this]() { updateStatusBarForCurrentTab(); });

    connect(ui->actionOpenFolder, &QAction::triggered, this, &MainWindow::openFolder);
    connect(ui->actionExit, &QAction::triggered, this, &QWidget::close);
    connect(ui->actionGoUp, &QAction::triggered, this, &MainWindow::goToParentFolder);
    connect(ui->actionRefresh, &QAction::triggered, this, &MainWindow::refreshCurrentFolder);
    connect(ui->actionZoomIn, &QAction::triggered, this, &MainWindow::zoomIn);
    connect(ui->actionZoomOut, &QAction::triggered, this, &MainWindow::zoomOut);
    connect(ui->actionResetZoom, &QAction::triggered, this, &MainWindow::resetZoom);
    connect(ui->actionAbout, &QAction::triggered, this, &MainWindow::showAboutDialog);

    m_saveAction = new QAction(tr("Save"), this);
    m_saveAction->setShortcut(QKeySequence::Save);
    m_saveAction->setEnabled(false);
    ui->menuFile->insertAction(ui->menuRecent->menuAction(), m_saveAction);
    connect(m_saveAction, &QAction::triggered, this, &MainWindow::saveCurrentDocument);

    auto *preferencesAction = new QAction(tr("Preferences…"), this);
    preferencesAction->setMenuRole(QAction::PreferencesRole);
    ui->menuFile->insertAction(ui->actionExit, preferencesAction);
    connect(preferencesAction, &QAction::triggered, this, &MainWindow::showPreferences);

    auto *tipsAction = new QAction(tr("Tips…"), this);
    ui->menuHelp->insertAction(ui->actionAbout, tipsAction);
    ui->menuHelp->insertSeparator(ui->actionAbout);
    connect(tipsAction, &QAction::triggered, this, &MainWindow::showTips);
    connect(zoomSlider, &QSlider::valueChanged, this, &MainWindow::onZoomSliderChanged);
    connect(pageZoomSlider, &QSlider::valueChanged, this, [this](int percent) {
        if (auto *viewer = qobject_cast<PdfViewerWidget *>(ui->tabWidget->currentWidget()))
            viewer->setZoomPercent(percent);
    });

    setupViewModeMenu();
    setupSortMenu();
    applyContentViewMode(appSettings.contentViewMode());

    recentFiles = appSettings.recentFiles();
    connect(ui->menuRecent, &QMenu::aboutToShow, this, &MainWindow::rebuildRecentFilesMenu);
    rebuildRecentFilesMenu();

    m_favorites = appSettings.favoriteFolders();
    rebuildFavoritesList();

    clearDetailsPanel();
    setCurrentFolder(QStandardPaths::writableLocation(QStandardPaths::HomeLocation));
    updateStatusBarForCurrentTab();

    if (appSettings.showTipsAtStartup())
        QTimer::singleShot(0, this, &MainWindow::showTips);
}

MainWindow::~MainWindow()
{
    delete ui;
}

void MainWindow::closeEvent(QCloseEvent *event)
{
    for (int index = ui->tabWidget->count() - 1; index > 0; --index) {
        auto *viewer = qobject_cast<PdfViewerWidget *>(ui->tabWidget->widget(index));
        if (viewer && !confirmCloseViewer(viewer)) {
            event->ignore();
            return;
        }
    }
    event->accept();
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
    refreshFolderContentsPreservingSelection();
}

void MainWindow::scheduleFolderRefresh()
{
    directoryRefreshTimer->start();
}

void MainWindow::refreshFolderContentsPreservingSelection()
{
    if (currentFolderPath.isEmpty() || !QDir(currentFolderPath).exists())
        return;

    QStringList selectedPaths;
    const QModelIndexList selectedIndexes =
        ui->folderContentView->selectionModel()->selectedIndexes();
    selectedPaths.reserve(selectedIndexes.size());
    for (const QModelIndex &index : selectedIndexes) {
        selectedPaths.append(
            contentModel->data(index, FolderContentModel::FilePathRole).toString());
    }
    const int scrollPosition = ui->folderContentView->verticalScrollBar()->value();

    contentModel->setDirectory(currentFolderPath);
    updateStatusBarItemCount();

    QModelIndex firstRestoredIndex;
    for (int row = 0; row < contentModel->rowCount(); ++row) {
        const QModelIndex index = contentModel->index(row, 0);
        const QString path =
            contentModel->data(index, FolderContentModel::FilePathRole).toString();
        if (!selectedPaths.contains(path))
            continue;
        ui->folderContentView->selectionModel()->select(
            index, QItemSelectionModel::Select | QItemSelectionModel::Rows);
        if (!firstRestoredIndex.isValid())
            firstRestoredIndex = index;
    }

    if (firstRestoredIndex.isValid()) {
        ui->folderContentView->selectionModel()->setCurrentIndex(
            firstRestoredIndex, QItemSelectionModel::NoUpdate);
        ui->folderContentView->scrollTo(firstRestoredIndex);
    } else {
        clearDetailsPanel();
        QTimer::singleShot(0, this, [this, scrollPosition]() {
            ui->folderContentView->verticalScrollBar()->setValue(scrollPosition);
        });
    }
}

void MainWindow::updateFolderWatch(const QString &path)
{
    const QStringList watchedDirectories = directoryWatcher->directories();
    if (!watchedDirectories.isEmpty())
        directoryWatcher->removePaths(watchedDirectories);
    if (!path.isEmpty() && QDir(path).exists() && !directoryWatcher->addPath(path))
        qWarning() << "Could not watch directory for external changes:" << path;
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
    QAction *findInExplorerAction = menu.addAction(tr("Find in File Explorer"));

    QAction *chosen = menu.exec(ui->folderContentView->viewport()->mapToGlobal(pos));
    if (chosen == openAction) {
        if (isDir)
            setCurrentFolder(path);
        else
            openPdfViewerTab(path);
    } else if (chosen && chosen == openWithAction) {
        openWithSystemDefault(path);
    } else if (chosen == findInExplorerAction) {
        findInFileExplorer(path);
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

void MainWindow::setupSortMenu()
{
    auto *sortButton = new QToolButton(this);
    sortButton->setObjectName(QStringLiteral("sortButton"));
    sortButton->setText(tr("Sort"));
    sortButton->setPopupMode(QToolButton::InstantPopup);
    sortButton->setToolButtonStyle(Qt::ToolButtonTextOnly);

    auto *sortMenu = new QMenu(sortButton);
    sortActionGroup = new QActionGroup(this);
    sortActionGroup->setExclusive(true);

    const QList<QPair<FolderContentModel::SortMode, QString>> modes = {
        {FolderContentModel::SortMode::NameAscending, tr("Name ↑")},
        {FolderContentModel::SortMode::NameDescending, tr("Name ↓")},
        {FolderContentModel::SortMode::SizeAscending, tr("Size ↑")},
        {FolderContentModel::SortMode::SizeDescending, tr("Size ↓")},
        {FolderContentModel::SortMode::DateAscending, tr("Date ↑")},
        {FolderContentModel::SortMode::DateDescending, tr("Date ↓")},
    };

    for (const auto &[mode, label] : modes) {
        QAction *action = sortMenu->addAction(label);
        action->setCheckable(true);
        action->setData(static_cast<int>(mode));
        sortActionGroup->addAction(action);
        connect(action, &QAction::triggered, this,
                [this, mode]() { contentModel->setSortMode(mode); });
    }

    sortActionGroup->actions().first()->setChecked(true);
    sortMenu->addSeparator();
    QAction *foldersFirstAction = sortMenu->addAction(tr("Show Folders First"));
    foldersFirstAction->setCheckable(true);
    foldersFirstAction->setChecked(true);
    connect(foldersFirstAction, &QAction::toggled,
            contentModel, &FolderContentModel::setFoldersFirst);

    sortButton->setMenu(sortMenu);
    ui->mainToolBar->addWidget(sortButton);

    auto *menuBarSortMenu = new QMenu(tr("&Sort"), ui->menubar);
    menuBarSortMenu->addActions(sortActionGroup->actions());
    ui->menubar->insertMenu(ui->menuHelp->menuAction(), menuBarSortMenu);
}

void MainWindow::applyContentViewMode(AppSettings::ContentViewMode mode)
{
    switch (mode) {
    case AppSettings::ContentViewMode::Thumbnails: {
        const int thumbSize = zoomSlider->value();
        ui->folderContentView->setItemDelegate(gridContentDelegate);
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

void MainWindow::updateStatusBarForCurrentTab()
{
    auto *viewer = qobject_cast<PdfViewerWidget *>(ui->tabWidget->currentWidget());
    const bool showingExplorer = viewer == nullptr;

    thumbnailZoomWidget->setVisible(showingExplorer);
    pageZoomWidget->setVisible(viewer != nullptr);
    itemCountLabel->setVisible(showingExplorer);
    ui->actionZoomIn->setEnabled(showingExplorer);
    ui->actionZoomOut->setEnabled(showingExplorer);
    ui->actionResetZoom->setEnabled(showingExplorer);
    m_saveAction->setEnabled(viewer && viewer->isModified()
                             && !viewer->isOperationInProgress());

    if (viewer) {
        const QSignalBlocker blocker(pageZoomSlider);
        pageZoomSlider->setValue(viewer->zoomPercent());
    }
}

void MainWindow::showAboutDialog()
{
    QMessageBox::about(this, tr("About cualpdf"),
                        tr("%1 %2\nA fast, lightweight, open-source PDF editor.")
                            .arg(QCoreApplication::applicationName(), QCoreApplication::applicationVersion()));
}

void MainWindow::showPreferences()
{
    PreferencesDialog dialog(appSettings, this);
    if (dialog.exec() == QDialog::Accepted && m_tipsDialog) {
        m_tipsDialog->setShowAtStartup(appSettings.showTipsAtStartup());
        m_tipsDialog->setAutoCloseEnabled(appSettings.autoCloseTips());
    }
}

void MainWindow::showTips()
{
    if (m_tipsDialog) {
        m_tipsDialog->raise();
        m_tipsDialog->activateWindow();
        return;
    }

    m_tipsDialog = new TipsDialog(appSettings.showTipsAtStartup(),
                                  appSettings.autoCloseTips(), this);
    m_tipsDialog->setAttribute(Qt::WA_DeleteOnClose);
    connect(m_tipsDialog, &TipsDialog::showAtStartupChanged, this,
            [this](bool enabled) { appSettings.setShowTipsAtStartup(enabled); });
    connect(m_tipsDialog, &QObject::destroyed, this,
            [this]() { m_tipsDialog = nullptr; });
    m_tipsDialog->show();
}

void MainWindow::saveCurrentDocument()
{
    if (auto *viewer = qobject_cast<PdfViewerWidget *>(ui->tabWidget->currentWidget()))
        saveViewer(viewer);
}

void MainWindow::onTabCloseRequested(int index)
{
    requestClosePdfTab(index);
}

bool MainWindow::requestClosePdfTab(int index)
{
    if (index <= 0 || index >= ui->tabWidget->count()) // Explorer is permanent.
        return false;

    auto *viewer = qobject_cast<PdfViewerWidget *>(ui->tabWidget->widget(index));
    if (viewer && !confirmCloseViewer(viewer))
        return false;

    closePdfTabWithoutPrompt(index);
    return true;
}

void MainWindow::closePdfTabWithoutPrompt(int index)
{
    if (index <= 0 || index >= ui->tabWidget->count())
        return;

    QWidget *widget = ui->tabWidget->widget(index);
    ui->tabWidget->removeTab(index);
    updatePdfTabTitle(qobject_cast<PdfViewerWidget *>(widget));
    widget->deleteLater();
}

void MainWindow::onTabContextMenuRequested(const QPoint &pos)
{
    QTabBar *tabBar = ui->tabWidget->tabBar();
    const int tabIndex = tabBar->tabAt(pos);

    // The Explorer tab is permanent and has no PDF-tab actions.
    if (tabIndex <= 0)
        return;

    QMenu menu(this);
    QAction *closeAction = menu.addAction(tr("Close Tab"));
    QAction *closeOthersAction = menu.addAction(tr("Close Other Tabs"));
    QAction *closeAllAction = menu.addAction(tr("Close All PDF Tabs"));

    QAction *chosen = menu.exec(tabBar->mapToGlobal(pos));
    if (chosen == closeAction) {
        onTabCloseRequested(tabIndex);
    } else if (chosen == closeOthersAction) {
        QWidget *keptWidget = ui->tabWidget->widget(tabIndex);
        for (int index = ui->tabWidget->count() - 1; index > 0; --index) {
            if (ui->tabWidget->widget(index) != keptWidget
                && !requestClosePdfTab(index)) {
                break;
            }
        }
        if (ui->tabWidget->indexOf(keptWidget) >= 0)
            ui->tabWidget->setCurrentWidget(keptWidget);
    } else if (chosen == closeAllAction) {
        closePdfTabs();
    }
}

void MainWindow::closePdfTabs()
{
    // Close from right to left so removing a tab never changes the index of
    // a tab that is still waiting to be closed. Index zero is Explorer.
    for (int index = ui->tabWidget->count() - 1; index > 0; --index) {
        if (!requestClosePdfTab(index))
            return;
    }

    ui->tabWidget->setCurrentIndex(0);
}

bool MainWindow::confirmCloseViewer(PdfViewerWidget *viewer)
{
    if (!viewer || !viewer->isModified())
        return true;

    ui->tabWidget->setCurrentWidget(viewer);
    const QString fileName = QFileInfo(viewer->filePath()).fileName();
    QMessageBox messageBox(QMessageBox::Warning, tr("Unsaved Changes"),
                           tr("Save changes to \"%1\" before closing?").arg(fileName),
                           QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel,
                           this);
    messageBox.setDefaultButton(QMessageBox::Save);
    messageBox.setEscapeButton(QMessageBox::Cancel);
    messageBox.button(QMessageBox::Save)->setText(tr("Save"));
    messageBox.button(QMessageBox::Discard)->setText(tr("Discard"));
    messageBox.button(QMessageBox::Cancel)->setText(tr("Cancel"));

    const auto choice = static_cast<QMessageBox::StandardButton>(messageBox.exec());
    if (choice == QMessageBox::Cancel)
        return false;
    if (choice == QMessageBox::Discard)
        return true;
    return choice == QMessageBox::Save && saveViewer(viewer);
}

bool MainWindow::saveViewer(PdfViewerWidget *viewer)
{
    if (!viewer || !viewer->isModified())
        return true;

    const QString fileName = QFileInfo(viewer->filePath()).fileName();
    QProgressDialog progress(tr("Saving \"%1\"…").arg(fileName), QString(), 0, 0, this);
    progress.setWindowTitle(tr("Saving PDF"));
    progress.setCancelButton(nullptr);
    progress.setWindowModality(Qt::WindowModal);
    progress.setMinimumDuration(0);

    bool success = false;
    bool finished = false;
    QString errorMessage;
    QEventLoop eventLoop;
    const QMetaObject::Connection connection = connect(
        viewer, &PdfViewerWidget::saveFinished, &eventLoop,
        [&eventLoop, &success, &finished, &errorMessage](bool saved, const QString &error) {
            success = saved;
            finished = true;
            errorMessage = error;
            eventLoop.quit();
        });

    viewer->saveDocument(appSettings.createTimestampedBackups(),
                         appSettings.backupVersionLimit());
    if (!finished)
        eventLoop.exec();
    disconnect(connection);

    if (!success) {
        qWarning() << "Could not save PDF:" << viewer->filePath() << errorMessage;
        QMessageBox::critical(this, tr("Save Failed"),
                              tr("Could not save \"%1\".").arg(fileName));
    } else if (QDir::cleanPath(QFileInfo(viewer->filePath()).absolutePath())
               == QDir::cleanPath(currentFolderPath)) {
        scheduleFolderRefresh();
    }
    updateStatusBarForCurrentTab();
    return success;
}

void MainWindow::updatePdfTabTitle(PdfViewerWidget *viewer)
{
    if (!viewer)
        return;
    const int index = ui->tabWidget->indexOf(viewer);
    if (index >= 0) {
        const QString suffix = viewer->isModified() ? QStringLiteral(" *") : QString();
        ui->tabWidget->setTabText(index,
                                  QFileInfo(viewer->filePath()).fileName() + suffix);
    }

    bool anyModified = false;
    for (int tabIndex = 1; tabIndex < ui->tabWidget->count(); ++tabIndex) {
        const auto *tabViewer = qobject_cast<PdfViewerWidget *>(
            ui->tabWidget->widget(tabIndex));
        anyModified = anyModified || (tabViewer && tabViewer->isModified());
    }
    setWindowModified(anyModified);
    updateStatusBarForCurrentTab();
}

void MainWindow::setCurrentFolder(const QString &path)
{
    qInfo() << "Opening folder:" << path;

    if (path.isEmpty() || !QDir(path).exists())
        return;

    currentFolderPath = path;
    updateFolderWatch(path);
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

    auto *viewer = new PdfViewerWidget(filePath, kInlinePreviewWidth, ui->previewContainer,
                                       /*showToolbar=*/false);
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
    qInfo() << "Opening file:" << filePath;

    addRecentFile(filePath);

    for (int i = 0; i < ui->tabWidget->count(); ++i) {
        if (ui->tabWidget->widget(i)->property("filePath").toString() == filePath) {
            ui->tabWidget->setCurrentIndex(i);
            return;
        }
    }

    auto *viewer = new PdfViewerWidget(filePath, kFullViewerPageWidth, ui->tabWidget);
    viewer->setProperty("filePath", filePath);
    connect(viewer, &PdfViewerWidget::zoomPercentChanged, this,
            [this, viewer](int percent) {
                if (ui->tabWidget->currentWidget() != viewer)
                    return;
                const QSignalBlocker blocker(pageZoomSlider);
                pageZoomSlider->setValue(percent);
            });
    connect(viewer, &PdfViewerWidget::modifiedChanged, this,
            [this, viewer]() { updatePdfTabTitle(viewer); });
    connect(viewer, &PdfViewerWidget::operationInProgressChanged, this,
            [this, viewer]() {
                if (ui->tabWidget->currentWidget() == viewer)
                    updateStatusBarForCurrentTab();
            });

    const int index = ui->tabWidget->addTab(viewer, QFileInfo(filePath).fileName());
    ui->tabWidget->setCurrentIndex(index);
}

void MainWindow::openWithSystemDefault(const QString &filePath)
{
    qInfo() << "Opening file with system default application:" << filePath;

    QDesktopServices::openUrl(QUrl::fromLocalFile(filePath));
}

void MainWindow::findInFileExplorer(const QString &path)
{
    const QFileInfo info(path);
    if (!info.exists()) {
        ui->statusbar->showMessage(
            tr("Can't find \"%1\": it no longer exists.").arg(path), 4000);
        return;
    }

    bool launched = false;
#ifdef Q_OS_WIN
    launched = QProcess::startDetached(
        QStringLiteral("explorer.exe"),
        {QStringLiteral("/select,"), QDir::toNativeSeparators(info.absoluteFilePath())});
#elif defined(Q_OS_MACOS)
    launched = QProcess::startDetached(
        QStringLiteral("/usr/bin/open"),
        {QStringLiteral("-R"), info.absoluteFilePath()});
#else
    // Qt has no cross-desktop API for selecting an item on Linux. Opening
    // its containing folder is the portable fallback and respects the
    // user's configured file manager or desktop portal.
    const QString folderPath = info.isDir() ? info.absoluteFilePath() : info.absolutePath();
    launched = QDesktopServices::openUrl(QUrl::fromLocalFile(folderPath));
#endif

    if (!launched)
        ui->statusbar->showMessage(tr("Could not open the system file explorer."), 4000);
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

void MainWindow::onTreeContextMenuRequested(const QPoint &pos)
{
    const QModelIndex index = ui->folderTreeView->indexAt(pos);
    if (!index.isValid())
        return;

    const QString path = treeModel->filePath(index);
    if (!QFileInfo(path).isDir())
        return;

    const bool alreadyFavorite = m_favorites.contains(path);

    QMenu menu(this);
    QAction *favoriteAction =
        menu.addAction(alreadyFavorite ? tr("Unpin from Favorites") : tr("Pin to Favorites"));
    QAction *findInExplorerAction = menu.addAction(tr("Find in File Explorer"));
    QAction *chosen = menu.exec(ui->folderTreeView->viewport()->mapToGlobal(pos));
    if (chosen == favoriteAction) {
        if (alreadyFavorite)
            removeFavorite(path);
        else
            addFavorite(path);
    } else if (chosen == findInExplorerAction) {
        findInFileExplorer(path);
    }
}

void MainWindow::onFavoritesContextMenuRequested(const QPoint &pos)
{
    QListWidgetItem *item = ui->favoritesListWidget->itemAt(pos);
    if (!item)
        return;

    const QString path = item->data(Qt::UserRole).toString();
    QMenu menu(this);
    QAction *action = menu.addAction(tr("Unpin from Favorites"));
    if (menu.exec(ui->favoritesListWidget->viewport()->mapToGlobal(pos)) == action)
        removeFavorite(path);
}

void MainWindow::onFavoriteItemActivated(QListWidgetItem *item)
{
    if (item)
        setCurrentFolder(item->data(Qt::UserRole).toString());
}

void MainWindow::addFavorite(const QString &path)
{
    if (m_favorites.contains(path))
        return;
    m_favorites.append(path);
    appSettings.setFavoriteFolders(m_favorites);
    rebuildFavoritesList();
}

void MainWindow::removeFavorite(const QString &path)
{
    if (!m_favorites.removeAll(path))
        return;
    appSettings.setFavoriteFolders(m_favorites);
    rebuildFavoritesList();
}

void MainWindow::rebuildFavoritesList()
{
    ui->favoritesListWidget->clear();

    static QFileIconProvider iconProvider;
    for (const QString &path : std::as_const(m_favorites)) {
        if (!QFileInfo::exists(path))
            continue;
        auto *item = new QListWidgetItem(
            iconProvider.icon(QFileInfo(path)),
            QFileInfo(path).fileName(),
            ui->favoritesListWidget);
        item->setData(Qt::UserRole, path);
        item->setToolTip(path);
    }

    // Hide the favorites section entirely when empty; size to content when shown
    const int count = ui->favoritesListWidget->count();
    const bool hasItems = count > 0;
    if (hasItems) {
        const int rowH = ui->favoritesListWidget->fontMetrics().height() + 10;
        ui->favoritesListWidget->setFixedHeight(rowH * count);
    }
    ui->favoritesHeaderLabel->setVisible(hasItems);
    ui->favoritesListWidget->setVisible(hasItems);
    ui->favoritesSeparator->setVisible(hasItems);
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

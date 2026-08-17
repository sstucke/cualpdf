#pragma once

#include "appsettings.h"

#include <QMainWindow>
#include <QString>
#include <QStringList>

QT_BEGIN_NAMESPACE
namespace Ui { class MainWindow; }
class QFileSystemModel;
class QFileSystemWatcher;
class QLabel;
class QListWidgetItem;
class QSlider;
class QTimer;
class QWidget;
class QModelIndex;
class QPoint;
class QActionGroup;
class QAction;
class QAbstractItemDelegate;
class QCloseEvent;
QT_END_NAMESPACE

class FolderContentModel;
class PdfViewerWidget;
class TipsDialog;

class MainWindow final : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow() override;

protected:
    void closeEvent(QCloseEvent *event) override;

private:
    void openFolder();
    void goToParentFolder();
    void refreshCurrentFolder();
    void scheduleFolderRefresh();
    void refreshFolderContentsPreservingSelection();
    void updateFolderWatch(const QString &path);
    void onTreeCurrentChanged(const QModelIndex &current);
    void onContentActivated(const QModelIndex &index);
    void onContentSelectionChanged();
    void onContentContextMenuRequested(const QPoint &pos);
    void onLocationEditReturnPressed();
    void onZoomSliderChanged(int value);
    void zoomIn();
    void zoomOut();
    void resetZoom();
    void showAboutDialog();
    void showPreferences();
    void showTips();
    void saveCurrentDocument();
    void onTabCloseRequested(int index);
    void onTabContextMenuRequested(const QPoint &pos);
    void onTreeContextMenuRequested(const QPoint &pos);
    void onFavoritesContextMenuRequested(const QPoint &pos);
    void onFavoriteItemActivated(QListWidgetItem *item);

    void setCurrentFolder(const QString &path);
    void addFavorite(const QString &path);
    void removeFavorite(const QString &path);
    void rebuildFavoritesList();
    void updateStatusBarItemCount();
    void updateDetailsPanel(const QString &filePath);
    void clearDetailsPanel();
    void openPdfViewerTab(const QString &filePath);
    void closePdfTabs();
    bool requestClosePdfTab(int index);
    void closePdfTabWithoutPrompt(int index);
    bool confirmCloseViewer(PdfViewerWidget *viewer);
    bool saveViewer(PdfViewerWidget *viewer);
    void updatePdfTabTitle(PdfViewerWidget *viewer);
    void openWithSystemDefault(const QString &filePath);
    void findInFileExplorer(const QString &path);
    void addRecentFile(const QString &filePath);
    void rebuildRecentFilesMenu();
    void setupViewModeMenu();
    void setupSortMenu();
    void applyContentViewMode(AppSettings::ContentViewMode mode);
    void updateStatusBarForCurrentTab();

    Ui::MainWindow *ui;
    QFileSystemModel *treeModel;
    FolderContentModel *contentModel;
    QLabel *itemCountLabel;
    QSlider *zoomSlider;
    QFileSystemWatcher *directoryWatcher;
    QTimer *directoryRefreshTimer;
    QWidget *thumbnailZoomWidget = nullptr;
    QSlider *pageZoomSlider = nullptr;
    QWidget *pageZoomWidget = nullptr;
    QActionGroup *viewModeActionGroup = nullptr;
    QActionGroup *sortActionGroup = nullptr;
    QAction *m_saveAction = nullptr;
    QAbstractItemDelegate *defaultContentDelegate = nullptr;
    QAbstractItemDelegate *detailsContentDelegate = nullptr;
    QAbstractItemDelegate *gridContentDelegate = nullptr;
    AppSettings appSettings;
    QString currentFolderPath;
    QStringList recentFiles;
    QStringList m_favorites;
    TipsDialog *m_tipsDialog = nullptr;

    static constexpr int kDefaultThumbnailSize = 96;
    static constexpr int kMinIconSize = 32;
    static constexpr int kMaxIconSize = 256;
    static constexpr int kZoomStep = 16;
    static constexpr int kDetailsIconSize = 48;
    static constexpr int kCompactIconSize = 20;
    static constexpr int kMaxRecentFiles = 10;
    static constexpr int kGridCellPadding = 8;
    static constexpr int kGridTextHeight = 80; // room for a wrapped name plus a page-count line
};

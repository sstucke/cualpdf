#pragma once

#include "appsettings.h"

#include <QMainWindow>
#include <QString>
#include <QStringList>

QT_BEGIN_NAMESPACE
namespace Ui { class MainWindow; }
class QFileSystemModel;
class QLabel;
class QListWidgetItem;
class QSlider;
class QWidget;
class QModelIndex;
class QPoint;
class QActionGroup;
class QAbstractItemDelegate;
QT_END_NAMESPACE

class FolderContentModel;

class MainWindow final : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow() override;

private:
    void openFolder();
    void goToParentFolder();
    void refreshCurrentFolder();
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
    void openWithSystemDefault(const QString &filePath);
    void addRecentFile(const QString &filePath);
    void rebuildRecentFilesMenu();
    void setupViewModeMenu();
    void applyContentViewMode(AppSettings::ContentViewMode mode);
    void updateStatusBarForCurrentTab();

    Ui::MainWindow *ui;
    QFileSystemModel *treeModel;
    FolderContentModel *contentModel;
    QLabel *itemCountLabel;
    QSlider *zoomSlider;
    QWidget *thumbnailZoomWidget = nullptr;
    QSlider *pageZoomSlider = nullptr;
    QWidget *pageZoomWidget = nullptr;
    QActionGroup *viewModeActionGroup = nullptr;
    QAbstractItemDelegate *defaultContentDelegate = nullptr;
    QAbstractItemDelegate *detailsContentDelegate = nullptr;
    QAbstractItemDelegate *gridContentDelegate = nullptr;
    AppSettings appSettings;
    QString currentFolderPath;
    QStringList recentFiles;
    QStringList m_favorites;

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

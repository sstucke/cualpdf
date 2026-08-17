#pragma once

#include <QDialog>
#include <QString>
#include <QStringList>

class QLabel;
class QLineEdit;
class QProgressBar;
class QPushButton;
class QTreeWidget;

class MergePdfDialog final : public QDialog
{
    Q_OBJECT

public:
    explicit MergePdfDialog(const QString &initialDirectory,
                            const QStringList &openPdfPaths,
                            QWidget *parent = nullptr);

signals:
    void mergeCompleted(const QString &outputPath);

protected:
    void reject() override;

private:
    void addFiles();
    void addPaths(const QStringList &paths);
    void inspectFile(quint64 itemId, const QString &path);
    void finishFileInspection(quint64 itemId, int pageCount);
    void removeSelectedFiles();
    void clearFiles();
    void moveCurrentFile(int direction);
    void chooseOutputFile();
    void startMerge();
    void finishMerge(bool success, const QString &outputPath,
                     const QString &failedInputPath,
                     const QString &fileErrorMessage);
    void updateControls();
    QStringList inputPaths() const;

    QString m_initialDirectory;
    QStringList m_openPdfPaths;
    QTreeWidget *m_fileList;
    QLabel *m_summaryLabel;
    QLineEdit *m_outputEdit;
    QLabel *m_statusLabel;
    QProgressBar *m_progressBar;
    QPushButton *m_addButton;
    QPushButton *m_removeButton;
    QPushButton *m_clearButton;
    QPushButton *m_moveUpButton;
    QPushButton *m_moveDownButton;
    QPushButton *m_browseButton;
    QPushButton *m_mergeButton;
    QPushButton *m_closeButton;
    quint64 m_nextItemId = 1;
    int m_pendingInspections = 0;
    bool m_mergeInProgress = false;
};

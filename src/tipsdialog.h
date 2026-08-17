#pragma once

#include <QDialog>
#include <QElapsedTimer>
#include <QStringList>

class QLabel;
class QCheckBox;
class QProgressBar;
class QTimer;
class QEnterEvent;
class QKeyEvent;

class TipsDialog final : public QDialog
{
    Q_OBJECT

public:
    explicit TipsDialog(bool showAtStartup, bool autoCloseEnabled,
                        QWidget *parent = nullptr);
    void setShowAtStartup(bool enabled);
    void setAutoCloseEnabled(bool enabled);

signals:
    void showAtStartupChanged(bool enabled);

private:
    void showRandomTip();
    void restartAutoClose();
    void pauseAutoClose();
    void resumeAutoClose();
    void updateAutoCloseProgress();

protected:
    void enterEvent(QEnterEvent *event) override;
    void leaveEvent(QEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;

    QStringList m_tips;
    QLabel *m_tipLabel;
    QCheckBox *m_showAtStartupCheckBox;
    QProgressBar *m_autoCloseProgress;
    QTimer *m_autoCloseTimer;
    QTimer *m_progressTimer;
    bool m_autoCloseEnabled;
    int m_remainingAutoCloseMs = 10000;
    QElapsedTimer m_autoCloseElapsed;
    int m_currentTipIndex = -1;
};

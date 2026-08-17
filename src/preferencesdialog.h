#pragma once

#include <QDialog>

class AppSettings;
class QCheckBox;
class QGroupBox;
class QLabel;
class QLineEdit;
class QSpinBox;
class QWidget;

class PreferencesDialog final : public QDialog
{
    Q_OBJECT

public:
    explicit PreferencesDialog(AppSettings &settings, QWidget *parent = nullptr);

private:
    void filterSettings(const QString &query);
    void saveAndAccept();

    AppSettings &m_settings;
    QLineEdit *m_searchEdit;
    QGroupBox *m_savingGroup;
    QGroupBox *m_tipsGroup;
    QGroupBox *m_windowsGroup;
    QWidget *m_backupRow;
    QWidget *m_retentionRow;
    QWidget *m_showTipsRow;
    QWidget *m_autoCloseTipsRow;
    QWidget *m_preserveExplorerRow;
    QCheckBox *m_backupCheckBox;
    QSpinBox *m_retentionSpinBox;
    QCheckBox *m_showTipsCheckBox;
    QCheckBox *m_autoCloseTipsCheckBox;
    QCheckBox *m_preserveExplorerCheckBox;
    QLabel *m_noResultsLabel;
    QString m_backupSearchText;
    QString m_retentionSearchText;
    QString m_showTipsSearchText;
    QString m_autoCloseTipsSearchText;
    QString m_preserveExplorerSearchText;
};

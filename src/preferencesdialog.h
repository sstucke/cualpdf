#pragma once

#include <QDialog>

class AppSettings;
class QCheckBox;
class QGroupBox;
class QLabel;
class QLineEdit;
class QPushButton;
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
    QGroupBox *m_editingGroup;
    QWidget *m_backupRow;
    QWidget *m_retentionRow;
    QWidget *m_showTipsRow;
    QWidget *m_autoCloseTipsRow;
    QWidget *m_preserveExplorerRow;
    QWidget *m_imageEditorRow;
    QCheckBox *m_backupCheckBox;
    QSpinBox *m_retentionSpinBox;
    QCheckBox *m_showTipsCheckBox;
    QCheckBox *m_autoCloseTipsCheckBox;
    QCheckBox *m_preserveExplorerCheckBox;
    QLineEdit *m_imageEditorEdit;
    QPushButton *m_imageEditorBrowseButton;
    QPushButton *m_imageEditorClearButton;
    QLabel *m_noResultsLabel;
    QString m_backupSearchText;
    QString m_retentionSearchText;
    QString m_showTipsSearchText;
    QString m_autoCloseTipsSearchText;
    QString m_preserveExplorerSearchText;
    QString m_imageEditorSearchText;
};

#include "preferencesdialog.h"

#include "appsettings.h"

#include <QCheckBox>
#include <QDialogButtonBox>
#include <QFrame>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QFileDialog>
#include <QPushButton>
#include <QScrollArea>
#include <QSpinBox>
#include <QVBoxLayout>

namespace {
bool containsText(const QString &text, const QString &query)
{
    return query.isEmpty() || text.contains(query, Qt::CaseInsensitive);
}
}

PreferencesDialog::PreferencesDialog(AppSettings &settings, QWidget *parent)
    : QDialog(parent)
    , m_settings(settings)
    , m_searchEdit(new QLineEdit(this))
    , m_savingGroup(new QGroupBox(tr("Saving"), this))
    , m_tipsGroup(new QGroupBox(tr("Tips"), this))
    , m_windowsGroup(new QGroupBox(tr("Windows"), this))
    , m_editingGroup(new QGroupBox(tr("Editing"), this))
    , m_backupRow(new QWidget(this))
    , m_retentionRow(new QWidget(this))
    , m_showTipsRow(new QWidget(this))
    , m_autoCloseTipsRow(new QWidget(this))
    , m_preserveExplorerRow(new QWidget(this))
    , m_imageEditorRow(new QWidget(this))
    , m_backupCheckBox(new QCheckBox(
          tr("Create a timestamped backup before overwriting a PDF"), m_backupRow))
    , m_retentionSpinBox(new QSpinBox(m_retentionRow))
    , m_showTipsCheckBox(new QCheckBox(tr("Show tips at startup"), m_showTipsRow))
    , m_autoCloseTipsCheckBox(new QCheckBox(
          tr("Close tips automatically after 10 seconds"), m_autoCloseTipsRow))
    , m_preserveExplorerCheckBox(new QCheckBox(
          tr("Preserve the file explorer when closing all tabs"),
          m_preserveExplorerRow))
    , m_imageEditorEdit(new QLineEdit(m_imageEditorRow))
    , m_imageEditorBrowseButton(new QPushButton(tr("Browse…"), m_imageEditorRow))
    , m_imageEditorClearButton(new QPushButton(tr("Use system default"), m_imageEditorRow))
    , m_noResultsLabel(new QLabel(tr("No settings match your search."), this))
{
    setWindowTitle(tr("Preferences"));
    resize(640, 440);

    auto *mainLayout = new QVBoxLayout(this);
    m_searchEdit->setPlaceholderText(tr("Search settings…"));
    m_searchEdit->setClearButtonEnabled(true);
    mainLayout->addWidget(m_searchEdit);

    auto *scrollArea = new QScrollArea(this);
    scrollArea->setWidgetResizable(true);
    scrollArea->setFrameShape(QFrame::NoFrame);
    auto *settingsContainer = new QWidget(scrollArea);
    auto *settingsLayout = new QVBoxLayout(settingsContainer);

    auto *savingLayout = new QVBoxLayout(m_savingGroup);
    auto *backupLayout = new QVBoxLayout(m_backupRow);
    backupLayout->setContentsMargins(0, 0, 0, 0);
    backupLayout->addWidget(m_backupCheckBox);
    auto *backupDescription = new QLabel(
        tr("The previous file will be retained with a date and time in its name."),
        m_backupRow);
    backupDescription->setWordWrap(true);
    backupDescription->setStyleSheet(QStringLiteral("color: palette(mid);"));
    backupLayout->addWidget(backupDescription);
    savingLayout->addWidget(m_backupRow);

    auto *retentionLayout = new QHBoxLayout(m_retentionRow);
    retentionLayout->setContentsMargins(0, 0, 0, 0);
    retentionLayout->addWidget(new QLabel(tr("Keep the latest"), m_retentionRow));
    m_retentionSpinBox->setRange(1, 99);
    m_retentionSpinBox->setFixedWidth(70);
    retentionLayout->addWidget(m_retentionSpinBox);
    retentionLayout->addWidget(
        new QLabel(tr("backup versions per document"), m_retentionRow));
    retentionLayout->addStretch();
    savingLayout->addWidget(m_retentionRow);

    auto *tipsLayout = new QVBoxLayout(m_tipsGroup);
    auto *showTipsLayout = new QVBoxLayout(m_showTipsRow);
    showTipsLayout->setContentsMargins(0, 0, 0, 0);
    showTipsLayout->addWidget(m_showTipsCheckBox);
    auto *tipsDescription = new QLabel(
        tr("Show a small, non-blocking window with a random suggestion."),
        m_showTipsRow);
    tipsDescription->setWordWrap(true);
    tipsDescription->setStyleSheet(QStringLiteral("color: palette(mid);"));
    showTipsLayout->addWidget(tipsDescription);
    tipsLayout->addWidget(m_showTipsRow);

    auto *autoCloseTipsLayout = new QVBoxLayout(m_autoCloseTipsRow);
    autoCloseTipsLayout->setContentsMargins(0, 0, 0, 0);
    autoCloseTipsLayout->addWidget(m_autoCloseTipsCheckBox);
    auto *autoCloseTipsDescription = new QLabel(
        tr("The countdown pauses while the pointer is over the tip."),
        m_autoCloseTipsRow);
    autoCloseTipsDescription->setWordWrap(true);
    autoCloseTipsDescription->setStyleSheet(QStringLiteral("color: palette(mid);"));
    autoCloseTipsLayout->addWidget(autoCloseTipsDescription);
    tipsLayout->addWidget(m_autoCloseTipsRow);

    auto *windowsLayout = new QVBoxLayout(m_windowsGroup);
    auto *preserveExplorerLayout = new QVBoxLayout(m_preserveExplorerRow);
    preserveExplorerLayout->setContentsMargins(0, 0, 0, 0);
    preserveExplorerLayout->addWidget(m_preserveExplorerCheckBox);
    windowsLayout->addWidget(m_preserveExplorerRow);

    auto *editingLayout = new QVBoxLayout(m_editingGroup);
    auto *imageEditorLayout = new QVBoxLayout(m_imageEditorRow);
    imageEditorLayout->setContentsMargins(0, 0, 0, 0);
    auto *imageEditorDescription = new QLabel(
        tr("Images on a page open in this program. Leave it empty to use the application your system uses for PNG files."),
        m_imageEditorRow);
    imageEditorDescription->setWordWrap(true);
    imageEditorDescription->setStyleSheet(QStringLiteral("color: palette(mid);"));
    imageEditorLayout->addWidget(imageEditorDescription);
    auto *imageEditorPathLayout = new QHBoxLayout();
    m_imageEditorEdit->setReadOnly(true);
    m_imageEditorEdit->setPlaceholderText(tr("System default"));
    imageEditorPathLayout->addWidget(m_imageEditorEdit);
    imageEditorPathLayout->addWidget(m_imageEditorBrowseButton);
    imageEditorPathLayout->addWidget(m_imageEditorClearButton);
    imageEditorLayout->addLayout(imageEditorPathLayout);
    editingLayout->addWidget(m_imageEditorRow);

    settingsLayout->addWidget(m_savingGroup);
    settingsLayout->addWidget(m_editingGroup);
    settingsLayout->addWidget(m_tipsGroup);
    settingsLayout->addWidget(m_windowsGroup);
    settingsLayout->addWidget(m_noResultsLabel);
    settingsLayout->addStretch();
    scrollArea->setWidget(settingsContainer);
    mainLayout->addWidget(scrollArea);

    auto *buttons = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    buttons->button(QDialogButtonBox::Ok)->setText(tr("OK"));
    buttons->button(QDialogButtonBox::Cancel)->setText(tr("Cancel"));
    connect(buttons, &QDialogButtonBox::accepted, this,
            &PreferencesDialog::saveAndAccept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    mainLayout->addWidget(buttons);

    m_backupCheckBox->setChecked(m_settings.createTimestampedBackups());
    m_retentionSpinBox->setValue(m_settings.backupVersionLimit());
    m_retentionRow->setEnabled(m_backupCheckBox->isChecked());
    m_showTipsCheckBox->setChecked(m_settings.showTipsAtStartup());
    m_autoCloseTipsCheckBox->setChecked(m_settings.autoCloseTips());
    m_preserveExplorerCheckBox->setChecked(
        m_settings.preserveExplorerWhenClosingTabs());
    m_imageEditorEdit->setText(m_settings.imageEditorPath());
    connect(m_imageEditorBrowseButton, &QPushButton::clicked, this, [this]() {
        const QString path = QFileDialog::getOpenFileName(
            this, tr("Choose an image editor"), m_imageEditorEdit->text());
        if (!path.isEmpty())
            m_imageEditorEdit->setText(path);
    });
    connect(m_imageEditorClearButton, &QPushButton::clicked, this, [this]() {
        m_imageEditorEdit->clear();
    });
    connect(m_backupCheckBox, &QCheckBox::toggled, m_retentionRow, &QWidget::setEnabled);
    connect(m_searchEdit, &QLineEdit::textChanged,
            this, &PreferencesDialog::filterSettings);

    m_backupSearchText = tr("timestamped backup overwrite previous file safe save");
    m_retentionSearchText = tr("backup versions retention history limit");
    m_showTipsSearchText = tr("tips suggestions startup launch");
    m_autoCloseTipsSearchText = tr("tips automatic close timeout countdown");
    m_preserveExplorerSearchText =
        tr("windows tabs close all preserve keep file explorer");
    m_imageEditorSearchText =
        tr("image editor external gimp paint photoshop png");
    m_noResultsLabel->hide();
}

void PreferencesDialog::filterSettings(const QString &query)
{
    const QString normalized = query.trimmed();
    const bool savingGroupMatch = containsText(m_savingGroup->title(), normalized);
    const bool tipsGroupMatch = containsText(m_tipsGroup->title(), normalized);
    const bool windowsGroupMatch = containsText(m_windowsGroup->title(), normalized);
    const bool editingGroupMatch = containsText(m_editingGroup->title(), normalized);
    const bool backupVisible = savingGroupMatch
                               || containsText(m_backupCheckBox->text() + QLatin1Char(' ')
                                                   + m_backupSearchText,
                                               normalized);
    const bool retentionVisible = savingGroupMatch
                                  || containsText(m_retentionSearchText, normalized);
    const bool showTipsVisible = tipsGroupMatch
                                 || containsText(m_showTipsCheckBox->text() + QLatin1Char(' ')
                                                     + m_showTipsSearchText,
                                                 normalized);
    const bool autoCloseTipsVisible = tipsGroupMatch
                                      || containsText(
                                          m_autoCloseTipsCheckBox->text()
                                              + QLatin1Char(' ')
                                              + m_autoCloseTipsSearchText,
                                          normalized);
    const bool preserveExplorerVisible = windowsGroupMatch
                                         || containsText(
                                             m_preserveExplorerCheckBox->text()
                                                 + QLatin1Char(' ')
                                                 + m_preserveExplorerSearchText,
                                             normalized);

    m_backupRow->setVisible(backupVisible);
    m_retentionRow->setVisible(retentionVisible);
    m_showTipsRow->setVisible(showTipsVisible);
    m_autoCloseTipsRow->setVisible(autoCloseTipsVisible);
    const bool imageEditorVisible = editingGroupMatch
                                    || containsText(m_imageEditorSearchText, normalized);
    m_preserveExplorerRow->setVisible(preserveExplorerVisible);
    m_imageEditorRow->setVisible(imageEditorVisible);
    m_savingGroup->setVisible(backupVisible || retentionVisible);
    m_tipsGroup->setVisible(showTipsVisible || autoCloseTipsVisible);
    m_windowsGroup->setVisible(preserveExplorerVisible);
    m_editingGroup->setVisible(imageEditorVisible);
    m_noResultsLabel->setVisible(!m_savingGroup->isVisible()
                                 && !m_tipsGroup->isVisible()
                                 && !m_windowsGroup->isVisible()
                                 && !m_editingGroup->isVisible());
}

void PreferencesDialog::saveAndAccept()
{
    m_settings.setCreateTimestampedBackups(m_backupCheckBox->isChecked());
    m_settings.setBackupVersionLimit(m_retentionSpinBox->value());
    m_settings.setShowTipsAtStartup(m_showTipsCheckBox->isChecked());
    m_settings.setAutoCloseTips(m_autoCloseTipsCheckBox->isChecked());
    m_settings.setPreserveExplorerWhenClosingTabs(
        m_preserveExplorerCheckBox->isChecked());
    m_settings.setImageEditorPath(m_imageEditorEdit->text().trimmed());
    accept();
}

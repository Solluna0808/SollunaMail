#ifndef SETTINGSDIALOG_H
#define SETTINGSDIALOG_H

#include <QDialog>
#include <QListWidget>
#include <QStackedWidget>
#include <QLineEdit>
#include <QComboBox>
#include <QCheckBox>
#include <QSpinBox>
#include <QLabel>
#include <QPushButton>
#include "mailmodels.h"
#include "customwidgets.h"
#include "googleprovider.h"

// 초보자용 원클릭 자동 감지(Google / iCloud / 포털) + 고급 수신(IMAP)/발신(SMTP: Mailtrap, Mailgun) 분리 설정 마법사
class AddAccountDialog : public QDialog {
    Q_OBJECT
public:
    explicit AddAccountDialog(GoogleProvider *googleProvider,
                              const AccountItem *editTarget = nullptr,
                              const QString &initialProviderPreset = QString(),
                              QWidget *parent = nullptr);

    AccountItem resultAccount() const { return m_result; }

private:
    void applyDomainAutoDetect(const QString &email);
    void applySmtpPreset(int presetIndex);
    void updateBeginnerModeUi();

    GoogleProvider *m_googleProvider = nullptr;
    bool m_isEditMode = false;
    AccountItem m_result;

    // 초보자용 기본 입력
    QLineEdit *m_displayNameEdit = nullptr;
    QLineEdit *m_emailEdit = nullptr;
    QLabel *m_autoDetectBadge = nullptr;

    // Google 원클릭 패널 vs 비밀번호 입력 패널
    QWidget *m_googleOneClickBox = nullptr;
    QWidget *m_passwordBox = nullptr;
    QLineEdit *m_passwordEdit = nullptr;
    QLabel *m_passwordGuideLabel = nullptr;
    QPushButton *m_appleIdHelpBtn = nullptr;

    // 아바타 색상 선택
    QComboBox *m_avatarColorCombo = nullptr;

    // 고급 설정 (수신 IMAP / 발신 SMTP 분리 · Mailtrap · Mailgun)
    QPushButton *m_toggleAdvancedBtn = nullptr;
    QWidget *m_advancedPanel = nullptr;

    // 받는 서버 (Incoming IMAP / POP3)
    QLineEdit *m_imapHostEdit = nullptr;
    QSpinBox *m_imapPortSpin = nullptr;
    QComboBox *m_imapSecurityCombo = nullptr;
    QLineEdit *m_imapUsernameEdit = nullptr;
    QCheckBox *m_pop3LeaveCopyCheck = nullptr;

    // 보내는 서버 (Outgoing SMTP - Mailtrap / Mailgun / Custom)
    QComboBox *m_smtpPresetCombo = nullptr;
    QLineEdit *m_smtpHostEdit = nullptr;
    QSpinBox *m_smtpPortSpin = nullptr;
    QComboBox *m_smtpSecurityCombo = nullptr;
    QCheckBox *m_separateSmtpAuthCheck = nullptr;
    QWidget *m_separateSmtpAuthBox = nullptr;
    QLineEdit *m_smtpUsernameEdit = nullptr;
    QLineEdit *m_smtpPasswordEdit = nullptr;
};

class SettingsDialog : public QDialog {
    Q_OBJECT
public:
    explicit SettingsDialog(QList<AccountItem> *accounts,
                            QList<FolderItem> *folders,
                            PrivacyHubSettings *privacySettings,
                            const QString &currentTheme,
                            GoogleProvider *googleProvider,
                            int initialTabIndex = 3, // Default 3 = Privacy hub (Screenshot 3)
                            QWidget *parent = nullptr);

    void refreshAccountsList();

signals:
    void privacySettingsChanged();
    void senderUnblocked(const QString &email);
    void accountsUpdated();
    void foldersUpdated();
    void calendarSettingsChanged();
    void runFilterRulesRequested();
    void toggleRightCalendarRequested(bool visible);
    void clearMailCacheRequested();
    void themeChanged(const QString &themeName);
    void requestImapSync(const QString &accountId);
    void languageChanged(AppLanguage lang);
    void autoSyncIntervalChanged(int seconds);
    void mailLayoutChanged(int mode, bool menuExpanded);
    void zoomLevelChanged(int zoomPercent);

private:
    QWidget *createGeneralSettingsPage();
    QWidget *createPrivacyHubPage();
    QWidget *createAccountsPage();
    QWidget *createAppearancePage();
    QWidget *createFoldersPage();
    QWidget *createFiltersPage();
    QWidget *createComposingPage();
    QWidget *createReadingPage();
    QWidget *createCalendarSettingsPage();
    QWidget *createNotificationsPage();
    QWidget *createSignaturesPage();
    QWidget *createSnippetsPage();
    QWidget *createShortcutsPage();
    QWidget *createAdvancedPage();
    QWidget *createAiMcpPage();
    QWidget *createHelpGuidePage();
    QWidget *createGeneralPage(const QString &title, const QString &subtitle);
    void populateNavAndPages(int activeTabIndex);

    void onAddAccountClicked(const QString &preset = QString());
    void onEditSelectedAccount();
    void onDeleteSelectedAccount();
    void loadSelectedAccountToRightPanel(int accountIndex);
    void saveRightPanelToSelectedAccount();
    void onTestInlineServerConnection();

    QList<AccountItem> *m_accounts = nullptr;
    QList<FolderItem> *m_folders = nullptr;
    PrivacyHubSettings *m_privacy = nullptr;
    QString m_currentTheme;
    GoogleProvider *m_googleProvider = nullptr;

    QListWidget *m_navList = nullptr;
    QStackedWidget *m_pages = nullptr;

    // Accounts tab controls (Mailbird 2-column Master-Detail)
    QListWidget *m_accountsListWidget = nullptr;
    QLabel *m_accountDetailSummary = nullptr;
    int m_selectedAccountIdx = -1;
    QStackedWidget *m_accountSubTabStack = nullptr;
    QList<QPushButton *> m_accountSubTabBtns;
    QList<QPushButton *> m_avatarGridBtns;
    QPushButton *m_accountColorBtn = nullptr;
    QLineEdit *m_accYourNameEdit = nullptr;
    QLineEdit *m_accEmailReadEdit = nullptr;
    QLineEdit *m_accLabelEdit = nullptr;
    QCheckBox *m_accUnifiedCheck = nullptr;
    QCheckBox *m_accReplyToCheck = nullptr;
    QLineEdit *m_accReplyToEdit = nullptr;
    QListWidget *m_accAliasesList = nullptr;
    QLineEdit *m_accNewAliasEdit = nullptr;
    QLineEdit *m_accInlineImapHost = nullptr;
    QSpinBox *m_accInlineImapPort = nullptr;
    QComboBox *m_accInlineImapSec = nullptr;
    QComboBox *m_accInlineSmtpPreset = nullptr;
    QLineEdit *m_accInlineSmtpHost = nullptr;
    QSpinBox *m_accInlineSmtpPort = nullptr;
    QComboBox *m_accInlineSmtpSec = nullptr;
    QLineEdit *m_accInlineSmtpUser = nullptr;
    QLineEdit *m_accInlineSmtpPass = nullptr;
    QLabel *m_accInlineTestResultLbl = nullptr;
    QWidget *m_appsGoogleContainer = nullptr;
    QWidget *m_appsIcloudContainer = nullptr;
    QWidget *m_appsNoneContainer = nullptr;
    QLabel *m_appsNoneDescLbl = nullptr;
};

#endif // SETTINGSDIALOG_H


#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QMainWindow>
#include <QStackedWidget>
#include <QListWidget>
#include <QLineEdit>
#include <QLabel>
#include <QPushButton>
#include <QToolButton>
#include <QTextBrowser>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QSystemTrayIcon>
#include <QTimer>
#include <QMap>
#include <QSet>
#include "mailmodels.h"
#include "mailtheme.h"
#include "customwidgets.h"
#include "googleprovider.h"

QT_BEGIN_NAMESPACE
namespace Ui {
class MainWindow;
}
QT_END_NAMESPACE

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow() override;

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;
    void closeEvent(QCloseEvent *event) override;
    void changeEvent(QEvent *event) override;

private slots:
    void onSelectAccount(const QString &accountId);
    void onSelectFolder(const QString &folderId, const QString &folderDisplayName);
    void onConversationRowSelected(int row);
    void onComposeNewMail(const QString &toEmail = QString(),
                          const QString &subject = QString(),
                          const QString &quoteHtml = QString());
    void onReplyCurrent(bool replyAll = false);
    void onForwardCurrent();
    void onArchiveCurrent();
    void onDeleteCurrent();
    void onSpamCurrent();
    void onToggleUnreadCurrent();
    void onToggleStarCurrent();
    void onToggleImportantCurrent();
    void onUnsubscribeCurrent();
    void onBlockSenderCurrent();
    void onMarkCheckedMessagesRead();
    void onMoveCheckedMessagesToTrash();
    void onMarkFolderAllRead(const QString &folderId = QString());
    void onMoveFolderAllToTrash(const QString &folderId = QString());
    void onEmptyFolder(const QString &folderId = QString());
    void onRestoreCurrentFromTrash();
    void onRestoreAllFromTrash(const QString &sourceFolderId = QStringLiteral("trash"));
    void onSwitchMailLayoutMode(int mode);
    void onToggleMinimalLayout();
    void onAddNewCalendarEvent(const QDate &initialDate = QDate(2026, 11, 3));
    void onEditOrDeleteCalendarEvent(const CalendarEventItem &ev);
    void onOpenAppleCalendarSyncDialog();
    void onAddNewContact(bool startInPickExistingMode = false);
    void onAddExistingContactsToGroup();
    void onEditCurrentContact();
    void onDeleteCurrentContact();
    void onAddContactGroup();
    void onRenameContactGroup();
    void onDeleteContactGroup();
    void onSendMailToSelectedContactGroup();
    void onOpenSettingsModal(int initialTabIndex = 3);
    void onApplyTheme(const QString &themeName);
    void onRefreshAndSyncCurrentAccount();
    void onChangeLanguage(AppLanguage lang);

private:
    void initializePreloadedData();
    void setupMainMenuBar();
    void setupMainLayout();
    void retranslateUi();
    QWidget *buildLeftBar();
    QWidget *buildFolderPane();
    QWidget *buildMailWorkspacePage();
    QWidget *buildFullCalendarPage();
    QWidget *buildFullContactsPage();
    QWidget *buildRightDockedCalendarSidebar();
    QWidget *buildFarRightAppStrip();

    void refreshFolderSelectionStyles();
    void refreshConversationList();
    void appendMoreConversationRows(int pageSize = 20);
    void onConversationListScrolled(int value);
    void onSelectMailPage(int page);
    void updateMailPaginationBar();
    int lowestLoadedImapSeqForFolder(const QString &accountId, const QString &folderId) const;
    int countLoadedConversationsForFolder(const QString &accountId, const QString &folderId) const;
    void displayConversation(int indexInFiltered, bool userClicked = false);
    QString normalizeEmailHtmlForQtViewer(const QString &rawHtml,
                                          const QString &senderName,
                                          const QString &senderEmail) const;
    QString prepareEmailHtmlForChromiumViewer(const QString &rawHtml,
                                              const QString &senderName,
                                              const QString &senderEmail) const;
    void setReadingBodyHtml(const QString &html, const QString &convId = QString(), bool isRichEmail = false);
    void handleEmailLinkActivated(const QUrl &url);
    void showTransientTrayToast(const QString &title, const QString &message, int durationMs = 2000);
    void showMailLoadingToast(const QString &message = QString(), int durationMs = 2000);
    void loadAllowedExternalImagesIntoBrowser(const QString &html, const QString &convId, class QTextBrowser *targetBrowser = nullptr);
    void applyReadingBodyZoom(int zoomPercent);
    void onOpenLargeMailViewerDialog();
    void refreshReadingAttachmentsBar(const ConversationItem &conv);
    void refreshCalendarViews();
    void refreshContactsList();
    void rebuildContactGroupsList();
    void rebuildLeftBarAccounts();
    void rebuildCustomFoldersList();
    int applyFilterRulesToConversations();
    void rebuildCalendarTree();
    void pruneDeletedAccountsData();
    void harvestContactsFromConversations(const QList<ConversationItem> &convs);
    QString conversationSignatureKey(const ConversationItem &c) const;
    void saveConversationsCacheToDisk();
    void loadConversationsCacheFromDisk();
    void saveCalendarAndContactsCacheToDisk();
    void loadCalendarAndContactsCacheFromDisk();
    void saveAccountsToDisk();
    void loadAccountsFromDisk();
    void onQuickAddAccountFromLeftBar();
    void setupSystemTrayAndAutoPoll();
    void applyAutoSyncInterval();
    void updateAppUnreadBadgeIcon();
    void commitPendingSend();
    void cancelPendingSendAndReopen();
    void updateUndoSendBannerText();
    void handleComposeDialogAccepted(class ComposeDialog &dlg, const QString &replacingConvId = QString());
    void checkScheduledSends();
    void updateScheduledMailStatusIndicator();
    void onRescheduleCurrentMail();
    void onEditOrCancelScheduledCurrentMail();
    void onSendNowScheduledCurrentMail();

    struct PendingSendItem {
        bool active = false;
        AccountItem account;
        QString convId;
        QString toEmail;
        QString ccEmail;
        QString bccEmail;
        QString subject;
        QString bodyHtml;
        QString bodyPlain;
        QList<AttachmentItem> attachments;
        bool isHighImportance = false;
        int remainingSec = 0;
    };
    PendingSendItem m_pendingSend;
    QTimer *m_undoSendTimer = nullptr;
    QWidget *m_undoSendBanner = nullptr;
    QLabel *m_undoSendBannerLbl = nullptr;
    QPushButton *m_undoSendCancelBtn = nullptr;
    QPushButton *m_undoSendNowBtn = nullptr;

    QTimer *m_scheduledSendTimer = nullptr;
    QAction *m_menuBarScheduledAction = nullptr;
    QWidget *m_scheduledMailBanner = nullptr;
    QLabel *m_scheduledMailBannerLbl = nullptr;
    QPushButton *m_scheduledRescheduleBtn = nullptr;
    QPushButton *m_scheduledEditOrCancelBtn = nullptr;
    QPushButton *m_scheduledSendNowBtn = nullptr;

    Ui::MainWindow *ui = nullptr;
    QSystemTrayIcon *m_trayIcon = nullptr;
    QTimer *m_autoSyncTimer = nullptr;
    QSet<QString> m_notifiedConvIds;
    bool m_initialSyncCompleted = false;
    int m_lastBadgeUnreadCount = -1;
    bool m_forceQuit = false;
    bool m_trayMinimizeNoticeShown = false;

    // Theme & State
    MailThemePalette m_palette;
    QString m_currentThemeName = "BlueFocus (MailBird Royal)";
    PrivacyHubSettings m_privacySettings;
    GoogleProvider *m_googleProvider = nullptr;

    QString m_selectedAccountId = "acc_unified";
    QString m_selectedFolderId = "inbox";
    QString m_selectedFolderTitle = "Inbox";
    bool m_unreadOnlyFilter = false;
    QString m_mailSearchQuery;
    int m_selectedConvIndex = 0;
    QString m_selectedConvId;

    // Data Stores & Scroll-Based Infinite Pagination State
    QList<AccountItem> m_accounts;
    QList<FolderItem> m_folders;
    QList<ConversationItem> m_conversations;
    QList<int> m_filteredConvIndices;
    int m_renderedConvRows = 0;
    QMap<QString, int> m_folderLowestSeq;
    QMap<QString, int> m_folderTotalExists;
    QSet<QString> m_folderLoadingMore;
    QMap<QString, int> m_activePageFetchEndSeq;
    QSet<QString> m_checkedConvIds;
    QSet<QString> m_userExplicitlyOpenedConvIds;
    int m_currentMailPage = 1;
    int m_totalMailPages = 1;
    QWidget *m_mailPaginationBar = nullptr;
    QToolButton *m_pageFirstBlockBtn = nullptr;
    QToolButton *m_pagePrevBtn = nullptr;
    QToolButton *m_pageNextBtn = nullptr;
    QToolButton *m_pageLastBlockBtn = nullptr;
    QList<QPushButton *> m_pageNumBtns;
    class QFrame *m_mailLoadingToast = nullptr;
    QLabel *m_mailLoadingToastLbl = nullptr;
    QTimer *m_mailLoadingToastTimer = nullptr;
    QSet<QString> m_deletedConvIds;
    QSet<QString> m_readConvSigs;
    QSet<QString> m_repliedConvSigs;
    QSet<QString> m_forwardedConvSigs;
    QSet<QString> m_starredConvSigs;
    QSet<QString> m_unstarredConvSigs;
    QSet<QString> m_importantConvSigs;
    QSet<QString> m_unimportantConvSigs;
    QSet<QString> m_allowedExternalImageConvIds;
    QSet<QString> m_loadedExtImgUrls;
    QMap<QString, QImage> m_loadedExtImgCache;
    QString m_currentRenderedSafeHtml;
    int m_readingBodyZoomPercent = 100;
    QPushButton *m_readingZoomBadgeBtn = nullptr;
    QPushButton *m_readingLargeViewBtn = nullptr;
    QSet<QString> m_repairedLegacyHtmlConvIds;
    class QNetworkAccessManager *m_extImgNam = nullptr;
    QList<CalendarSubItem> m_calendars;
    QList<CalendarEventItem> m_calendarEvents;
    QList<ContactItem> m_contacts;
    QStringList m_customContactGroups;

    // Main Layout Widgets
    QWidget *m_leftBarWidget = nullptr;
    QVBoxLayout *m_leftBarAccountsLayout = nullptr;
    QWidget *m_folderPaneWidget = nullptr;
    QVBoxLayout *m_customFoldersLayout = nullptr;
    QLabel *m_folderAccountHeaderLbl = nullptr;
    QLabel *m_yourFoldersHeaderLbl = nullptr;
    QPushButton *m_manageFoldersBtn = nullptr;
    QMap<QString, QPushButton *> m_folderButtons;
    QMap<QString, QToolButton *> m_accountButtons;

    QStackedWidget *m_centerStack = nullptr;

    // Page 0: Main Mail View
    QBoxLayout *m_mailSplitLayout = nullptr;
    QWidget *m_convPaneWidget = nullptr;
    QWidget *m_readingPaneWidget = nullptr;
    QWidget *m_emptyTrashBanner = nullptr;
    QPushButton *m_restoreSelectedTrashBtn = nullptr;
    QPushButton *m_restoreAllTrashBtn = nullptr;
    QPushButton *m_emptyTrashBannerBtn = nullptr;
    QToolButton *m_btnUnreadFilter = nullptr;
    QWidget *m_unreadFilterBanner = nullptr;
    QLabel *m_unreadFilterBannerLbl = nullptr;
    QToolButton *m_actStarBtn = nullptr;
    QToolButton *m_actRestoreBtn = nullptr;
    int m_mailLayoutMode = 0; // 0: Compact (Vertical split), 1: Horizontal split, 2: Fullscreen list
    int m_mailSortMode = 0;   // 0: Newest first (default), 1: Oldest first, 2: Sender A-Z, 3: Subject A-Z, 4: Unread first
    QLabel *m_convPaneTitleLbl = nullptr;
    QLineEdit *m_mailSearchEdit = nullptr;
    QListWidget *m_convListWidget = nullptr;
    ConversationListDelegate *m_convListDelegate = nullptr;
    QLabel *m_readingSubjectLbl = nullptr;
    QLabel *m_readingFolderChipLbl = nullptr;
    QLabel *m_readingAvatarLbl = nullptr;
    QLabel *m_readingSenderLbl = nullptr;
    QLabel *m_readingTimeLbl = nullptr;
    QPushButton *m_unsubscribeBtn = nullptr;
    QPushButton *m_blockSenderBtn = nullptr;
    QWidget *m_readingAttachmentsBar = nullptr;
    QHBoxLayout *m_readingAttachmentsLayout = nullptr;
    QTextBrowser *m_readingBodyBrowser = nullptr;
    class QWebEngineView *m_readingWebView = nullptr;
    QWidget *m_rightCalendarSidebar = nullptr;
    QPushButton *m_rightCalTodayBtn = nullptr;
    QLabel *m_rightCalDateHeaderLbl = nullptr;
    QLabel *m_rightCalAllDayLbl = nullptr;
    DayTimelineWidget *m_dayTimelineWidget = nullptr;

    // Page 1: Full-Screen Calendar View (Month / Week / Day / Agenda)
    void onSwitchCalendarViewMode(int mode);
    QPushButton *m_fullCalTodayBtn = nullptr;
    QPushButton *m_fullCalNewEventBtn = nullptr;
    QPushButton *m_fullCalAppleSyncBtn = nullptr;
    QLabel *m_fullCalMonthTitleLbl = nullptr;
    QList<QPushButton *> m_calViewModeBtns;
    int m_calendarViewMode = 0; // 0: Month, 1: Week, 2: Day, 3: Agenda
    QStackedWidget *m_fullCalViewStack = nullptr;
    MiniMonthCalendarWidget *m_miniCalendarWidget = nullptr;
    MonthCalendarGridWidget *m_monthGridWidget = nullptr;
    WeekCalendarViewWidget *m_weekCalViewWidget = nullptr;
    DayCalendarViewWidget *m_dayCalViewWidget = nullptr;
    AgendaCalendarViewWidget *m_agendaCalViewWidget = nullptr;
    QWidget *m_calendarTreeContainer = nullptr;
    QVBoxLayout *m_calendarTreeLayout = nullptr;
    QLineEdit *m_calSearchEdit = nullptr;
    QDate m_activeCalendarDate = QDate::currentDate();

    // Page 2: Full-Screen Contacts View
    QLabel *m_contactsLeftHeaderLbl = nullptr;
    QLabel *m_contactsCenterHeaderLbl = nullptr;
    QListWidget *m_contactGroupList = nullptr;
    QLineEdit *m_contactSearchEdit = nullptr;
    QListWidget *m_contactItemsList = nullptr;
    QLabel *m_contactDetailTitleLbl = nullptr;
    QLabel *m_contactDetailBodyLbl = nullptr;
    QPushButton *m_contactMailToBtn = nullptr;
    QPushButton *m_contactEditBtn = nullptr;
    QPushButton *m_contactDeleteBtn = nullptr;
};
#endif // MAINWINDOW_H

#ifndef GOOGLEPROVIDER_H
#define GOOGLEPROVIDER_H

#include <QObject>
#include <QMap>
#include <QSet>
#include <QNetworkAccessManager>
#include <QTcpServer>
#include <QSslSocket>
#include "mailmodels.h"

class GoogleProvider : public QObject {
    Q_OBJECT
public:
    explicit GoogleProvider(QObject *parent = nullptr);
    ~GoogleProvider() override;

    // 1. Built-in One-Click Google OAuth 2.0 Browser Login (Zero user config required)
    void startBuiltInGoogleLogin(const QString &loginHintEmail = QString());

    void startGoogleOAuthLogin(const QString &clientId,
                               const QString &clientSecret,
                               const QString &loginHintEmail = QString());

    // 2. Google Calendar API v3 & Google People API (Contacts) & Avatar/Favicon
    void refreshGoogleAccessToken(const QString &accountEmail, const QString &refreshToken);
    void syncGoogleCalendarEvents(const QString &accountEmail,
                                  const QString &accessToken,
                                  const QString &refreshToken = QString());
    void createGoogleCalendarEvent(const QString &accountEmail,
                                   const QString &accessToken,
                                   const CalendarEventItem &event);
    void updateGoogleCalendarEvent(const QString &accountEmail,
                                   const QString &accessToken,
                                   const CalendarEventItem &event);
    void deleteGoogleCalendarEvent(const QString &accountEmail,
                                   const QString &accessToken,
                                   const QString &calendarId,
                                   const QString &eventId);

    // 2-B. Apple Calendar (iCloud CalDAV RFC 4791 Bidirectional Sync + webcal:// Public Subscription + RFC 5545 .ics Parser/Builder)
    void syncICloudCalDavCalendars(const QString &appleIdEmail,
                                   const QString &appPassword);
    void createICloudCalDavEvent(const QString &appleIdEmail,
                                 const QString &appPassword,
                                 const QString &calendarCollectionUrl,
                                 const CalendarEventItem &event);
    void updateICloudCalDavEvent(const QString &appleIdEmail,
                                 const QString &appPassword,
                                 const QString &calendarCollectionUrl,
                                 const CalendarEventItem &event);
    void deleteICloudCalDavEvent(const QString &appleIdEmail,
                                 const QString &appPassword,
                                 const QString &calendarCollectionUrl,
                                 const CalendarEventItem &event);
    void fetchWebcalIcsSubscription(const QString &accountEmail,
                                    const QString &calendarName,
                                    const QString &webcalUrl,
                                    const QColor &calColor = QColor("#E11D48"));
    static QList<CalendarEventItem> parseIcsCalendarData(const QString &icsText,
                                                         const QString &accountEmail,
                                                         const QString &calendarId,
                                                         const QColor &barColor = QColor("#0D47A1"),
                                                         const QString &defaultResourceUrl = QString());
    static QByteArray buildIcsFromEvent(const CalendarEventItem &event);

    void syncGoogleContacts(const QString &accountEmail,
                            const QString &accessToken,
                            const QString &refreshToken = QString());
    void fetchAccountAvatar(const AccountItem &account);

    // 3. Live IMAP / POP3 SSL Fetch & Folder Actions (Gmail imap.gmail.com:993 / Custom IMAP / POP3 / iCloud — 2단계 고속 스트리밍 & 온디맨드 본문 로드)
    void fetchImapInbox(const AccountItem &account, int maxMessages = 200);
    void fetchImapFolder(const AccountItem &account, const QString &folderId, int maxMessages = 200, int endSeq = 0);
    void fetchPop3Inbox(const AccountItem &account, int maxMessages = 200, int endSeq = 0);
    void checkImapInboxQuick(const AccountItem &account, int knownExistsCount = -1);
    void checkPop3InboxQuick(const AccountItem &account, int knownExistsCount = -1);
    void startImapIdleWatcher(const AccountItem &account);
    void stopAllImapIdleWatchers();
    void registerLoadedImapUid(const QString &accountId, const QString &folderId, int uid);
    void unregisterLoadedImapUid(const QString &accountId, const QString &folderId, int uid);
    void clearLoadedImapUids(const QString &accountId = QString(), const QString &folderId = QString());
    void fetchAllStandardImapFolders(const AccountItem &account, int maxMessagesPerFolder = 200);
    void fetchImapSingleMessageBody(const AccountItem &account,
                                    const QString &folderId,
                                    int seqNum,
                                    const QString &conversationId,
                                    const QString &fallbackSubject = QString(),
                                    const QString &fallbackSourceFolderId = QString());
    void emptyImapFolder(const AccountItem &account, const QString &folderId);
    void deleteOrMoveImapMessage(const AccountItem &account,
                                 const QString &sourceFolderId,
                                 int seqNum,
                                 bool permanentDelete);
    void markImapFolderAllRead(const AccountItem &account, const QString &folderId);
    void setImapMessageSeenFlag(const AccountItem &account,
                                const QString &folderId,
                                int seqNum,
                                bool seen,
                                const QString &fallbackSubject = QString());
    void setImapMessagesSeenFlagBatch(const AccountItem &account,
                                      const QString &folderId,
                                      const QList<int> &seqNums,
                                      bool seen);
    void setImapMessageFlagged(const AccountItem &account,
                               const QString &folderId,
                               int seqNum,
                               bool flagged,
                               const QString &fallbackSubject = QString());
    void setImapMessageImportant(const AccountItem &account,
                                 const QString &folderId,
                                 int seqNum,
                                 bool important,
                                 const QString &fallbackSubject = QString());
    void setImapMessageAnsweredOrForwarded(const AccountItem &account,
                                           const QString &folderId,
                                           int seqNum,
                                           bool answered,
                                           bool forwarded,
                                           const QString &fallbackSubject = QString());
    void restoreImapMessageFromTrash(const AccountItem &account,
                                     int seqNum,
                                     const QString &targetFolderId = QStringLiteral("inbox"),
                                     const QString &sourceFolderId = QStringLiteral("trash"));
    void createImapCustomFolder(const AccountItem &account, const QString &folderName);
    void renameImapCustomFolder(const AccountItem &account,
                                const QString &oldFolderIdOrName,
                                const QString &newFolderName);
    void deleteImapCustomFolder(const AccountItem &account, const QString &folderIdOrName);
    void moveImapMessagesToFolder(const AccountItem &account,
                                  const QString &sourceFolderId,
                                  const QList<int> &seqNums,
                                  const QString &targetFolderId,
                                  const QString &targetFolderDisplayName = QString(),
                                  bool markAsRead = false,
                                  bool markAsStarred = false);

    // 4. Live SMTP SSL / STARTTLS Send + IMAP APPEND to Sent / Drafts (Gmail / Mailtrap / Mailgun / Custom SMTP & IMAP)
    void sendSmtpEmail(const AccountItem &account,
                       const QString &toEmail,
                       const QString &subject,
                       const QString &htmlBody,
                       const QString &ccEmail = QString(),
                       const QString &bccEmail = QString(),
                       const QList<AttachmentItem> &attachments = QList<AttachmentItem>(),
                       bool isHighImportance = false);
    void appendMessageToImapFolder(const AccountItem &account,
                                   const QString &folderId,
                                   const QString &toEmail,
                                   const QString &ccEmail,
                                   const QString &subject,
                                   const QString &htmlBody,
                                   const QString &bccEmail = QString(),
                                   const QList<AttachmentItem> &attachments = QList<AttachmentItem>(),
                                   bool isHighImportance = false);

    static QString decodeMimeHeader(const QString &raw);
    static QString decodeCharsetBytes(const QByteArray &bytes, const QString &charset = QString());
    static QByteArray decodeQuotedPrintable(const QByteArray &input);
    static void parseMimeEntity(const QByteArray &rawEntity,
                                QString &outHtml,
                                QString &outPlain,
                                QString *outFrom = nullptr,
                                QString *outSubject = nullptr,
                                QString *outDate = nullptr,
                                QString *outTo = nullptr,
                                QList<AttachmentItem> *outAttachments = nullptr,
                                const QString &msgStorageKey = QString(),
                                bool saveAttachmentDataToDisk = true,
                                bool isTruncatedSlice = false);
    static QString sanitizeEmailHtmlForViewer(const QString &rawHtml, const QString &fallbackPlainText);
    static QString htmlToPlainTextSnippet(const QString &html, const QString &plainText, int maxLen = 48);
    static QString builtInGoogleClientId();
    static QString builtInGoogleClientSecret();

signals:
    void statusMessage(const QString &msg);
    void oauthTokenReceived(const QString &email, const QString &accessToken, const QString &refreshToken);
    void googleProfileReady(const QString &email,
                            const QString &displayName,
                            const QString &accessToken,
                            const QString &refreshToken);
    void googleCalendarsFetched(const QString &accountEmail,
                                const QList<CalendarSubItem> &calendars,
                                const QList<CalendarEventItem> &events);
    void googleContactsFetched(const QString &accountEmail, const QList<ContactItem> &contacts);
    void accountAvatarFetched(const QString &accountEmail, const QString &pngBase64);
    void imapConversationsFetched(const QString &accountId, const QList<ConversationItem> &conversations);
    void imapFolderConversationsFetched(const QString &accountId,
                                        const QString &folderId,
                                        const QList<ConversationItem> &conversations);
    void imapFolderPageFetched(const QString &accountId,
                               const QString &folderId,
                               const QList<ConversationItem> &pageConversations,
                               int lowestFetchedSeq,
                               int totalExistsCount,
                               bool isInitialPage);
    void imapSingleMessageBodyFetched(const QString &conversationId,
                                      const QString &fullHtmlBody,
                                      const QList<AttachmentItem> &attachments);
    void imapCustomFoldersDiscovered(const QString &accountId, const QList<FolderItem> &customFolders);
    void smtpSendFinished(bool success, const QString &message);

private:
    void exchangeCodeForToken(const QString &code, const QString &redirectUri);
    void fetchGoogleUserProfile(const QString &accessToken, const QString &refreshToken);

    QNetworkAccessManager *m_nam = nullptr;
    QTcpServer *m_loopbackServer = nullptr;
    QMap<QString, QSslSocket *> m_idleSockets;
    QMap<QString, QSet<int>> m_loadedFolderUids;
    QString m_clientId;
    QString m_clientSecret;
    QString m_loginHint;
    bool m_isShuttingDown = false;
};

#endif // GOOGLEPROVIDER_H


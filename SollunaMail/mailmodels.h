#ifndef MAILMODELS_H
#define MAILMODELS_H

#include <QString>
#include <QStringList>
#include <QList>
#include <QColor>
#include <QDate>
#include <QTime>
#include <QDateTime>
#include <QSettings>
#include <QCoreApplication>
#include <QApplication>
#include <QFont>
#include <QFontDatabase>
#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <QStandardPaths>

enum class AccountProviderType {
    Unified,
    Google,
    CustomImap,
    ICloud,
    Microsoft
};

enum class AvatarVisualStyle {
    UnifiedEnvelope,
    GoogleOrbit,
    PhotoCircle,
    BlueFolderBadge,
    TealSquareS
};

struct AccountItem {
    QString id;
    QString email;
    QString displayName;
    QString initials;
    QColor avatarColor;
    AccountProviderType provider = AccountProviderType::CustomImap;
    AvatarVisualStyle visualStyle = AvatarVisualStyle::TealSquareS;

    // 받는 메일 서버 (Incoming IMAP) 설정 및 계정
    QString imapHost;
    int imapPort = 993;
    QString imapSecurity = "SSL/TLS";   // "SSL/TLS", "STARTTLS", "None"
    QString imapUsername;               // 비어 있으면 email 사용
    QString appPassword;                // 받는 서버 비밀번호 / 앱 비밀번호

    // 보내는 메일 서버 (Outgoing SMTP: Mailtrap, Mailgun 등 별도 서버·계정 지원)
    QString smtpPreset = "SameAsIncoming"; // "SameAsIncoming", "MailtrapLive", "MailtrapSandbox", "Mailgun", "MailgunEU", "SendGrid", "Custom"
    QString smtpHost;
    int smtpPort = 465;
    QString smtpSecurity = "SSL/TLS";   // "SSL/TLS", "STARTTLS", "None"
    bool useSeparateSmtpAuth = false;   // true이면 받는 서버와 다른 별도의 SMTP ID/비밀번호 사용
    QString smtpUsername;               // 예: Mailtrap "api", Mailgun "postmaster@mg.domain.com"
    QString smtpPassword;               // 예: Mailtrap API Token, Mailgun SMTP 암호

    // Google OAuth 2.0 토큰 및 프로필/파비콘 아바타 이미지 캐시
    QString oauthAccessToken;
    QString oauthRefreshToken;
    QString avatarUrl;                  // Google 프로필 사진 URL 또는 도메인 파비콘 URL
    QString avatarPngBase64;            // 다운로드된 프로필 사진/파비콘 PNG Base64 캐시
    int avatarIconPreset = 0;           // 0 = 커스텀/파비콘/프로필 사진, 1 = Google 사진, 2 = 이니셜 배지('S'), 3..32 = Mailbird 33종 아이콘 그리드 프리셋
    QString accountLabel;               // Mailbird Account label
    bool includeInUnified = true;       // Include in unified account
    bool calendarEnabled = true;        // Enable this account's calendar in Calendar view (Mailbird Settings > Calendar)
    bool useDifferentReplyTo = false;   // Use a different reply-to address
    QString replyToAddress;             // Reply-to email address
    QStringList aliases;                // Account aliases list
    QString signatureHtml;              // Per-account HTML signature
    bool pop3LeaveCopyOnServer = true;  // POP3 수신 시 서버에 메일 복사본 저장 (기본값: true)
    int unreadCount = 0;

    bool isPop3() const {
        const QString h = imapHost.trimmed().toLower();
        return imapPort == 995 || imapPort == 110
               || h.startsWith(QStringLiteral("pop."))
               || h.startsWith(QStringLiteral("pop3."));
    }
    QString pop3AccountFolderName() const {
        QString base = email.trimmed().isEmpty() ? id.trimmed() : email.trimmed().toLower();
        static const QString invalidChars = QStringLiteral("<>:\"/\\|?*");
        for (QChar ch : invalidChars) {
            base.replace(ch, QLatin1Char('_'));
        }
        return base.isEmpty() ? QStringLiteral("default_pop3") : base;
    }
    QString pop3AccountRootDir(const QString &baseAppDir = QString()) const {
        QString rootBase = baseAppDir;
        if (rootBase.isEmpty()) {
            const QString appDir = QCoreApplication::applicationDirPath();
            const QString lower = appDir.toLower();
            if (lower.contains(QStringLiteral(".app/contents/"))
                || lower.startsWith(QStringLiteral("/opt/sollunamail"))
                || lower.startsWith(QStringLiteral("/usr/"))
                || lower.contains(QStringLiteral("program files"))) {
                rootBase = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
            } else {
                rootBase = appDir;
            }
        }
        return rootBase + QStringLiteral("/pop3/") + pop3AccountFolderName();
    }
    QString pop3MailboxDir(const QString &folderId, const QString &baseAppDir = QString()) const {
        QString cleanFolder = folderId.trimmed().isEmpty() ? QStringLiteral("inbox") : folderId.trimmed();
        static const QString invalidChars = QStringLiteral("<>:\"/\\|?*");
        for (QChar ch : invalidChars) {
            cleanFolder.replace(ch, QLatin1Char('_'));
        }
        return pop3AccountRootDir(baseAppDir) + QStringLiteral("/") + cleanFolder;
    }
    void ensurePop3MailboxDirs(const QString &baseAppDir = QString()) const {
        if (!isPop3())
            return;
        const QString accRoot = pop3AccountRootDir(baseAppDir);
        const QStringList stdBoxes = {
            QStringLiteral("inbox"),
            QStringLiteral("sent"),
            QStringLiteral("drafts"),
            QStringLiteral("archive"),
            QStringLiteral("spam"),
            QStringLiteral("trash"),
            QStringLiteral("attachments")
        };
        for (const QString &box : stdBoxes) {
            QDir().mkpath(accRoot + QStringLiteral("/") + box);
        }
    }
    bool hasPop3AccountDir(const QString &baseAppDir = QString()) const {
        return QDir(pop3AccountRootDir(baseAppDir)).exists();
    }
    QString archivePop3AccountDirOnRemove(const QString &baseAppDir = QString()) const {
        const QString accRoot = pop3AccountRootDir(baseAppDir);
        QDir srcDir(accRoot);
        if (!srcDir.exists())
            return QString();
        QFileInfo fi(accRoot);
        QDir parentDir = fi.dir();
        const QString folderName = pop3AccountFolderName();
        QString targetName = QStringLiteral("remove_") + folderName;
        if (parentDir.exists(targetName)) {
            targetName = QString("remove_%1_%2")
                             .arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd_HHmmss")),
                                  folderName);
        }
        if (parentDir.rename(folderName, targetName)) {
            return parentDir.filePath(targetName);
        }
        return QString();
    }
    bool deletePop3AccountDir(const QString &baseAppDir = QString()) const {
        const QString accRoot = pop3AccountRootDir(baseAppDir);
        QDir srcDir(accRoot);
        if (!srcDir.exists())
            return true;
        return srcDir.removeRecursively();
    }
    QString effectiveImapUser() const {
        return imapUsername.trimmed().isEmpty() ? email.trimmed() : imapUsername.trimmed();
    }
    QString effectiveSmtpUser() const {
        if (useSeparateSmtpAuth && !smtpUsername.trimmed().isEmpty())
            return smtpUsername.trimmed();
        return effectiveImapUser();
    }
    QString effectiveSmtpPassword() const {
        if (useSeparateSmtpAuth && !smtpPassword.isEmpty())
            return smtpPassword;
        return appPassword;
    }
};

enum class FolderKind {
    Inbox,
    Starred,
    Drafts,
    Sent,
    Archive,
    Snoozed,
    Important,
    Spam,
    Trash,
    CustomFolder
};

struct FolderItem {
    QString id;
    QString accountId;
    QString accountEmail;
    QString name;
    QString rawImapMailbox;
    FolderKind kind = FolderKind::Inbox;
    int unreadCount = 0;
    bool isCustom = false;
    bool hasChildren = false;
    QColor dotColor = QColor("#4A90E2");
};

struct AttachmentItem {
    QString fileName;
    QString fileSize;
    QString mimeType;
    QString filePath;
};

enum class SenderAvatarKind {
    MailbirdAtLogo,
    MailspringRing,
    InitialsCircle,
    RootRainbow,
    UserPhoto
};

struct MessageItem {
    QString id;
    QString senderName;
    QString senderEmail;
    QString senderInitials;
    QColor senderAvatarBg;
    QColor senderAvatarFg;
    SenderAvatarKind avatarKind = SenderAvatarKind::InitialsCircle;
    QString recipientSummary;
    QString shortTimeText;   // e.g. "오전 1:22", "Yesterday", "1 Nov"
    QString fullTimeText;    // e.g. "Tue, 3 Nov, 오전 1:22"
    QString subject;
    QString snippet;
    QString htmlBody;
    QList<AttachmentItem> attachments;
};

struct ConversationItem {
    QString id;
    int imapUid = 0;       // RFC 3501 불변 고유 식별자 (UID) — 삭제/이동(EXPUNGE)되어도 절대 변하지 않음
    int imapSeq = 0;       // 현재 폴더 내 상대 순번 (1..EXISTS, 페이지네이션 위치 계산용)
    QString accountId;
    QString folderId;
    QString previousFolderId;
    QString folderTag = "Inbox";
    QString subject;
    bool isUnread = false;
    bool isStarred = false;
    bool isImportant = false;
    bool isReplied = false;
    bool isForwarded = false;
    QString scheduledSendIso; // Non-empty when scheduled to send later (ISO 8601 local/UTC)
    QString ccSummary;
    QString bccSummary;
    bool readReceiptRequested = false;
    bool readReceiptConfirmed = false;
    QList<MessageItem> messages;

    const MessageItem &latestMessage() const {
        static const MessageItem kEmpty;
        return messages.isEmpty() ? kEmpty : messages.last();
    }
};

struct CalendarSubItem {
    QString id;
    QString accountEmail;
    QString name;
    QColor color;
    bool isVisible = true;
    bool isDefault = false;
    bool showEyeToggle = false;
    QString calDavHref;  // iCloud CalDAV calendar collection URL (e.g. https://pXX-caldav.icloud.com/12345/calendars/home/)
    QString webcalUrl;   // Public webcal:// or https:// .ics subscription URL
};

struct CalendarEventItem {
    QString id;
    QString calendarId;
    QString accountEmail;
    QString title;
    QDate date;
    QTime startTime;
    QTime endTime;
    bool isAllDay = true;
    QString location;
    QString description;
    QString meetUrl;
    QString calDavResourceUrl; // Remote .ics URL for CalDAV PUT/DELETE
    QColor pillBgColor = QColor("#F2D6D8");
    QColor pillBarColor = QColor("#B94A50");
    QColor pillTextColor = QColor("#6E2226");
};

struct ContactItem {
    QString id;
    QString accountEmail;
    QString groupName;
    QString displayName;
    QString initials;
    QColor avatarBgColor;
    QColor avatarFgColor;
    QString email;
    QString phone;
    QString company;
    QString jobTitle;
    QString notes;
};

struct PrivacyHubSettings {
    bool privacyMode = true;
    bool blockPixelTracking = true;
    bool blockSenders = true;
    bool trackSentEmails = true;
    bool blockAllImages = false;
    bool downloadAvatars = true;
    QStringList blockedSendersList;
};

struct MailFilterRule {
    QString id;
    QString name;
    bool enabled = true;
    QString matchField = "From";       // "From", "To", "Subject", "Body"
    QString matchQuery;                // Substring / keyword to match
    QString targetFolderId = "archive"; // Folder ID to move matched messages to
    bool markAsRead = false;
    bool markAsStarred = false;
    QString accountEmail;              // 비어 있거나 "ALL"이면 전체 계정 공통, 이메일 지정 시 해당 계정 전용
};

struct MailSnippetItem {
    QString id;
    QString title;
    QString shortcutKey;               // e.g. "/thanks", "/intro"
    QString bodyHtml;
};

enum class AppLanguage {
    Korean = 0,
    English = 1,
    Japanese = 2
};

class MailI18n {
public:
    static AppLanguage &currentLanguage() {
        static AppLanguage s_lang = AppLanguage::Korean;
        return s_lang;
    }

    static void setLanguage(AppLanguage lang) {
        currentLanguage() = lang;
        applyLanguageFont();
    }

    static QString languageCode() {
        switch (currentLanguage()) {
        case AppLanguage::English:  return QStringLiteral("en");
        case AppLanguage::Japanese: return QStringLiteral("ja");
        case AppLanguage::Korean:
        default:                    return QStringLiteral("ko");
        }
    }

    static void setLanguageByCode(const QString &code) {
        QString c = code.trimmed().toLower();
        if (c.startsWith("en"))
            currentLanguage() = AppLanguage::English;
        else if (c.startsWith("ja") || c.startsWith("jp"))
            currentLanguage() = AppLanguage::Japanese;
        else
            currentLanguage() = AppLanguage::Korean;
        applyLanguageFont();
    }

    static void loadBundledCjkFonts() {
        static bool s_loaded = false;
        if (s_loaded)
            return;
        s_loaded = true;
        const QStringList searchDirs = {
            QCoreApplication::applicationDirPath() + QStringLiteral("/fonts"),
            QCoreApplication::applicationDirPath() + QStringLiteral("/../Resources/fonts"),
            QDir::currentPath() + QStringLiteral("/fonts"),
            QCoreApplication::applicationDirPath() + QStringLiteral("/../../fonts")
        };
        for (const QString &dirPath : searchDirs) {
            QDir dir(dirPath);
            if (!dir.exists())
                continue;
            const QFileInfoList files = dir.entryInfoList(
                {QStringLiteral("*.ttf"), QStringLiteral("*.ttc"), QStringLiteral("*.otf")},
                QDir::Files | QDir::Readable);
            for (const QFileInfo &fi : files) {
                QFontDatabase::addApplicationFont(fi.absoluteFilePath());
            }
        }
    }

    static void applyLanguageFont() {
        if (!qApp)
            return;
        loadBundledCjkFonts();
        QFont appFont = qApp->font();
#if defined(Q_OS_MACOS)
        switch (currentLanguage()) {
        case AppLanguage::Japanese:
            appFont.setFamilies({
                QStringLiteral("Hiragino Sans"),
                QStringLiteral("Hiragino Kaku Gothic ProN"),
                QStringLiteral("Apple SD Gothic Neo"),
                QStringLiteral(".AppleSystemUIFont")
            });
            break;
        case AppLanguage::English:
            appFont.setFamilies({
                QStringLiteral(".AppleSystemUIFont"),
                QStringLiteral("SF Pro Text"),
                QStringLiteral("Helvetica Neue"),
                QStringLiteral("Apple SD Gothic Neo"),
                QStringLiteral("Hiragino Sans")
            });
            break;
        case AppLanguage::Korean:
        default:
            appFont.setFamilies({
                QStringLiteral("Apple SD Gothic Neo"),
                QStringLiteral(".AppleSystemUIFont"),
                QStringLiteral("SF Pro Text"),
                QStringLiteral("Hiragino Sans")
            });
            break;
        }
        appFont.setPointSize(12);
#elif defined(Q_OS_LINUX)
        switch (currentLanguage()) {
        case AppLanguage::Japanese:
            appFont.setFamilies({
                QStringLiteral("Noto Sans CJK JP"),
                QStringLiteral("Noto Sans JP"),
                QStringLiteral("Noto Sans CJK KR"),
                QStringLiteral("Ubuntu"),
                QStringLiteral("DejaVu Sans"),
                QStringLiteral("Noto Color Emoji")
            });
            break;
        case AppLanguage::English:
            appFont.setFamilies({
                QStringLiteral("Ubuntu"),
                QStringLiteral("Noto Sans"),
                QStringLiteral("Noto Sans CJK KR"),
                QStringLiteral("Noto Sans CJK JP"),
                QStringLiteral("DejaVu Sans"),
                QStringLiteral("Noto Color Emoji")
            });
            break;
        case AppLanguage::Korean:
        default:
            appFont.setFamilies({
                QStringLiteral("Noto Sans CJK KR"),
                QStringLiteral("Noto Sans KR"),
                QStringLiteral("NanumGothic"),
                QStringLiteral("Ubuntu"),
                QStringLiteral("DejaVu Sans"),
                QStringLiteral("Noto Color Emoji")
            });
            break;
        }
        appFont.setPointSize(10);
#else
        switch (currentLanguage()) {
        case AppLanguage::Japanese:
            appFont.setFamilies({
                QStringLiteral("Yu Gothic UI"),
                QStringLiteral("Yu Gothic"),
                QStringLiteral("Meiryo UI"),
                QStringLiteral("Meiryo"),
                QStringLiteral("MS UI Gothic"),
                QStringLiteral("Malgun Gothic"),
                QStringLiteral("Segoe UI")
            });
            break;
        case AppLanguage::English:
            appFont.setFamilies({
                QStringLiteral("Segoe UI"),
                QStringLiteral("Malgun Gothic"),
                QStringLiteral("Yu Gothic UI"),
                QStringLiteral("Yu Gothic")
            });
            break;
        case AppLanguage::Korean:
        default:
            appFont.setFamilies({
                QStringLiteral("Malgun Gothic"),
                QStringLiteral("Segoe UI"),
                QStringLiteral("Yu Gothic UI"),
                QStringLiteral("Yu Gothic")
            });
            break;
        }
        appFont.setPointSize(9);
#endif
        appFont.setStyleStrategy(QFont::PreferAntialias);
        qApp->setFont(appFont);
    }

    static QString sanitizeLinuxEmojiIfMissing(const QString &src) {
#if defined(Q_OS_LINUX)
        static int s_hasColorEmoji = -1;
        if (s_hasColorEmoji < 0) {
            const QStringList fams = QFontDatabase::families();
            s_hasColorEmoji = (fams.contains(QStringLiteral("Noto Color Emoji"), Qt::CaseInsensitive)
                               || fams.contains(QStringLiteral("Twitter Color Emoji"), Qt::CaseInsensitive)
                               || fams.contains(QStringLiteral("EmojiOne"), Qt::CaseInsensitive)) ? 1 : 0;
        }
        if (s_hasColorEmoji == 1)
            return src;

        QString out;
        out.reserve(src.size());
        const int n = src.size();
        for (int i = 0; i < n; ++i) {
            const QChar c = src.at(i);
            if (c.isHighSurrogate() && (i + 1 < n) && src.at(i + 1).isLowSurrogate()) {
                ++i; // Skip supplementary-plane emoji surrogate pair (prevents ⊠ tofu box on Linux without Noto Color Emoji)
                while (i + 1 < n && src.at(i + 1).unicode() == 0xFE0F)
                    ++i;
                if (i + 1 < n && src.at(i + 1) == QLatin1Char(' ')
                    && (out.isEmpty() || out.endsWith(QLatin1Char('\n')) || out.endsWith(QLatin1Char('>'))) ) {
                    ++i;
                }
                continue;
            }
            if (c.unicode() == 0xFE0F)
                continue;
            out.append(c);
        }
        return out;
#else
        return src;
#endif
    }

    static QString t(const QString &ko, const QString &en, const QString &ja) {
        switch (currentLanguage()) {
        case AppLanguage::English:  return sanitizeLinuxEmojiIfMissing(en);
        case AppLanguage::Japanese: return sanitizeLinuxEmojiIfMissing(ja);
        case AppLanguage::Korean:
        default:                    return sanitizeLinuxEmojiIfMissing(ko);
        }
    }

    static QString folderTitle(const QString &folderId, const QString &fallback = QString()) {
        const QString id = folderId.toLower();
        if (id == "inbox")     return t("받은편지함", "Inbox", "受信トレイ");
        if (id == "starred")   return t("별표편지함", "Starred", "スター付き");
        if (id == "drafts")    return t("임시보관함", "Drafts", "下書き");
        if (id == "sent")      return t("보낸편지함", "Sent", "送信済み");
        if (id == "archive")   return t("보관함", "Archive", "アーカイブ");
        if (id == "snoozed")   return t("다시알림", "Snoozed", "スヌーズ");
        if (id == "important") return t("중요편지함", "Important", "重要");
        if (id == "spam")      return t("스팸편지함", "Spam", "迷惑メール");
        if (id == "trash")     return t("휴지통", "Trash", "ゴミ箱");

        const QString raw = fallback.isEmpty() ? folderId : fallback;
        const QString clean = raw.trimmed().toLower();
        if (clean == "임시보관함" || clean == "임시 보관함" || clean == "drafts" || clean == "draft")
            return t("임시 보관함", "Drafts", "下書き");
        if (clean == "지운편지함" || clean == "지운 편지함" || clean == "휴지통" || clean == "trash" || clean == "deleted items" || clean == "deleted messages")
            return t("지운 편지함", "Deleted Items", "削除済みアイテム");
        if (clean == "보낸편지함" || clean == "보낸 편지함" || clean == "발신함" || clean == "sent items" || clean == "sent messages")
            return t("보낸 편지함", "Sent Items", "送信済みアイテム");
        if (clean == "스팸편지함" || clean == "스팸 편지함" || clean == "스팸함" || clean == "정크 메일" || clean == "정크메일" || clean == "junk" || clean == "junk e-mail")
            return t("스팸 편지함", "Junk Email", "迷惑メール");
        if (clean == "중요편지함" || clean == "중요 편지함")
            return t("중요 편지함", "Important", "重要");
        if (clean == "별표편지함" || clean == "별표 편지함")
            return t("별표 편지함", "Starred", "スター付き");
        if (clean == "전체보관함" || clean == "전체 보관함" || clean == "보관함")
            return t("보관함", "Archive", "アーカイブ");
        if (clean == "내게 쓴 메일함" || clean == "내게쓴메일함")
            return t("내게 쓴 메일함", "Notes to Self", "自分宛て");
        return raw;
    }

    static QString &customGoogleClientId() {
        static QString s_id;
        return s_id;
    }

    static QString &customGoogleClientSecret() {
        static QString s_sec;
        return s_sec;
    }

    // Mailbird Appearance settings state
    static QString &appearanceMode() {
        static QString s_mode = QStringLiteral("Light"); // "Light", "Dark"
        return s_mode;
    }

    static QString &layoutStyleName() {
        static QString s_layout = QStringLiteral("Comfortable"); // "Comfortable", "Compact", "Horizontal", "Fullscreen", "Minimal"
        return s_layout;
    }

    static bool &menuExpanded() {
        static bool s_expanded = true;
        return s_expanded;
    }

    static int &zoomPercent() {
        static int s_zoom = 100;
        return s_zoom;
    }

    // Composing / Reading / Calendar / Notifications / Signatures / Snippets / Shortcuts / Advanced / AI & MCP (Gemini · Codex)
    static QString &defaultFontFamily() {
        static QString s_font = QStringLiteral("Malgun Gothic");
        return s_font;
    }

    static int &defaultFontSize() {
        static int s_sz = 10;
        return s_sz;
    }

    static int &undoSendDelaySec() {
        static int s_sec = 5;
        return s_sec;
    }

    static bool &includeQuoteOnReply() {
        static bool s_quote = true;
        return s_quote;
    }

    static bool &autoAppendSignature() {
        static bool s_sig = true;
        return s_sig;
    }

    static QString &defaultSignatureHtml() {
        static QString s_html = QStringLiteral(
            "<br/><div style='color:#4B5563;font-size:12px;border-top:1px solid #E5E7EB;padding-top:8px;'>"
            "<b>Sent with SollunaMail</b></div>");
        return s_html;
    }

    static bool &markReadOnSelect() {
        static bool s_mark = true;
        return s_mark;
    }

    static bool &groupByConversationThread() {
        static bool s_thread = true;
        return s_thread;
    }

    static bool &showRightCalendarSidebar() {
        static bool s_cal = true;
        return s_cal;
    }

    static int &calendarWeekStartDay() {
        static int s_day = 1; // 1 = Monday, 7 = Sunday
        return s_day;
    }

    static bool &calendarUse24HourFormat() {
        static bool s_24h = false; // Display times as 14:00 instead of 2:00 PM
        return s_24h;
    }

    static bool &calendarShowWeekNumbers() {
        static bool s_wn = false; // Display week numbers in the month and week views
        return s_wn;
    }

    static int &calendarDefaultDurationMin() {
        static int s_dur = 60; // Default event duration in minutes (default 60)
        return s_dur;
    }

    static int &calendarDefaultReminderMin() {
        static int s_rem = 15;
        return s_rem;
    }

    static bool &enableDesktopNotification() {
        static bool s_notif = true;
        return s_notif;
    }

    static bool &enableNotificationSound() {
        static bool s_snd = true;
        return s_snd;
    }

    static bool &launchOnWindowsStartup() {
        static bool s_startup = true;
        return s_startup;
    }

    static bool &startMinimizedToTray() {
        static bool s_minStart = false;
        return s_minStart;
    }

    static bool &closeToTrayOnX() {
        static bool s_closeTray = true;
        return s_closeTray;
    }

    static bool &minimizeToTrayOnMinimize() {
        static bool s_minTray = false;
        return s_minTray;
    }

    static bool &showTrayUnreadBadge() {
        static bool s_badge = true;
        return s_badge;
    }

    static bool &privacyTrackSentEmails() {
        static bool s_track = true;
        return s_track;
    }

    static bool &privacyDownloadAvatars() {
        static bool s_dlAv = true;
        return s_dlAv;
    }

    static void applyWindowsStartupRegistry(bool enable) {
        launchOnWindowsStartup() = enable;
#if defined(Q_OS_WIN)
        // 구버전 인스톨러가 Startup 폴더에 만들었던 중복 바로가기(SollunaMail.lnk)가 있으면 정리하여 이중 실행 방지
        const QString appDataEnv = QDir::fromNativeSeparators(qEnvironmentVariable("APPDATA").trimmed());
        if (!appDataEnv.isEmpty()) {
            QFile::remove(appDataEnv + QStringLiteral("/Microsoft/Windows/Start Menu/Programs/Startup/SollunaMail.lnk"));
        }
        QSettings reg(QStringLiteral("HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\CurrentVersion\\Run"),
                      QSettings::NativeFormat);
        if (enable) {
            const QString exePath = QDir::toNativeSeparators(QCoreApplication::applicationFilePath());
            reg.setValue(QStringLiteral("SollunaMail"), QStringLiteral("\"%1\" --startup").arg(exePath));
        } else {
            reg.remove(QStringLiteral("SollunaMail"));
        }
#elif defined(Q_OS_MACOS)
        const QString agentsDir = QDir::homePath() + QStringLiteral("/Library/LaunchAgents");
        const QString plistPath = agentsDir + QStringLiteral("/com.solluna.mail.plist");
        if (enable) {
            QDir().mkpath(agentsDir);
            QFile f(plistPath);
            if (f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
                const QString exePath = QCoreApplication::applicationFilePath().toHtmlEscaped();
                const QString xml = QStringLiteral(
                    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
                    "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" \"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
                    "<plist version=\"1.0\">\n"
                    "<dict>\n"
                    "  <key>Label</key><string>com.solluna.mail</string>\n"
                    "  <key>ProgramArguments</key>\n"
                    "  <array><string>%1</string><string>--startup</string></array>\n"
                    "  <key>RunAtLoad</key><true/>\n"
                    "</dict>\n"
                    "</plist>\n").arg(exePath);
                f.write(xml.toUtf8());
                f.close();
            }
        } else {
            QFile::remove(plistPath);
        }
#elif defined(Q_OS_LINUX)
        const QString configBase = QStandardPaths::writableLocation(QStandardPaths::ConfigLocation);
        const QString autostartDir = (configBase.isEmpty() ? (QDir::homePath() + QStringLiteral("/.config")) : configBase)
                                     + QStringLiteral("/autostart");
        const QString desktopPath = autostartDir + QStringLiteral("/sollunamail.desktop");
        if (enable) {
            QDir().mkpath(autostartDir);
            QFile f(desktopPath);
            if (f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
                const QString exePath = QCoreApplication::applicationFilePath();
                const QString desktopEntry = QStringLiteral(
                    "[Desktop Entry]\n"
                    "Type=Application\n"
                    "Name=SollunaMail\n"
                    "Comment=SollunaMail Cross-Platform Email Client\n"
                    "Exec=\"%1\" --startup\n"
                    "Terminal=false\n"
                    "X-GNOME-Autostart-enabled=true\n").arg(exePath);
                f.write(desktopEntry.toUtf8());
                f.close();
            }
        } else {
            QFile::remove(desktopPath);
        }
#endif
    }

    static bool &enableSingleKeyShortcuts() {
        static bool s_short = true;
        return s_short;
    }

    static int &autoSyncIntervalSec() {
        static int s_interval = 30; // 30s(실시간 IDLE + 30초), 60s(1분), 180s(3분), 300s(5분), 600s(10분), 1800s(30분), 0(수동)
        return s_interval;
    }

    static int &pageFetchSize() {
        static int s_batch = 200;
        return s_batch;
    }

    static int &imapSliceBytes() {
        static int s_slice = 32768;
        return s_slice;
    }

    // AI & MCP (Dedicated to Google Gemini & OpenAI Codex)
    static QString &aiProvider() {
        static QString s_prov = QStringLiteral("Gemini"); // "Gemini" or "Codex"
        return s_prov;
    }

    static QString &geminiApiKey() {
        static QString s_key;
        return s_key;
    }

    static QString &geminiModel() {
        static QString s_model = QStringLiteral("gemini-2.5-flash");
        return s_model;
    }

    static QString &codexApiKey() {
        static QString s_key;
        return s_key;
    }

    static QString &codexModel() {
        static QString s_model = QStringLiteral("gpt-5-codex");
        return s_model;
    }

    static bool &mcpServerEnabled() {
        static bool s_mcp = true;
        return s_mcp;
    }

    static QList<MailFilterRule> &filterRules() {
        static QList<MailFilterRule> s_rules = {
            {"rule_1", "GitHub / GitLab 알림 보관함 분류", false, "From", "github.com", "archive", true, false},
            {"rule_2", "청구서 / Invoice 중요 표시", false, "Subject", "Invoice", "important", false, true}
        };
        return s_rules;
    }

    static QList<MailSnippetItem> &snippets() {
        static QList<MailSnippetItem> s_snips = {
            {"snip_1", "비즈니스 감사 인사 (Thank You)", "/thanks",
             "<p>안녕하세요, 보내주신 메일 잘 확인하였습니다.<br/>검토 후 빠르게 회신드리겠습니다. 감사합니다.</p>"},
            {"snip_2", "회의 일정 조율 요청 (Meeting Request)", "/meeting",
             "<p>안녕하세요, 관련 안건 협의를 위해 화상 미팅(Google Meet) 가능하신 일정을 공유해 주시면 감사하겠습니다.</p>"},
            {"snip_3", "첨부자료 송부 안내 (Attachment Notice)", "/attach",
             "<p>요청하신 자료를 첨부파일로 보내드립니다. 확인 후 문의사항 있으시면 언제든 연락 부탁드립니다.</p>"}
        };
        return s_snips;
    }
};

#endif // MAILMODELS_H


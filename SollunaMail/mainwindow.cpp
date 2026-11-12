#include "mainwindow.h"
#include "ui_mainwindow.h"
#include "composedialog.h"
#include "settingsdialog.h"

#include <QScrollArea>
#include <QScrollBar>
#include <QSet>
#include <QSignalBlocker>
#include <QPainter>
#include <algorithm>
#include <QSplitter>
#include <QFrame>
#include <QInputDialog>
#include <QMessageBox>
#include <QDesktopServices>
#include <QUrl>
#include <QDialog>
#include <QFormLayout>
#include <QDateEdit>
#include <QTimeEdit>
#include <QCheckBox>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QApplication>
#include <QCoreApplication>
#include <QMenuBar>
#include <QStatusBar>
#include <QMenu>
#include <QAction>
#include <QActionGroup>
#include <QShortcut>
#include <QTextEdit>
#include <QUuid>
#include <QWheelEvent>
#include <QTimer>
#include <QScreen>
#include <QGuiApplication>
#include <QFileDialog>
#include <QFileInfo>
#include <QDir>
#include <QStandardPaths>
#include <QGroupBox>
#include <QCloseEvent>
#include <QNetworkAccessManager>
#include <QNetworkRequest>
#include <QNetworkReply>
#include <QTextDocument>
#include <QPointer>
#include <QBuffer>
#include <functional>

#ifdef SOLLUNA_HAS_WEBENGINE
#include <QWebEngineView>
#include <QWebEnginePage>
#include <QWebEngineSettings>
#include <QWebEngineUrlRequestInterceptor>
#include <QWebEngineUrlRequestInfo>

namespace {
class SollunaPrivacyInterceptor : public QWebEngineUrlRequestInterceptor
{
public:
    explicit SollunaPrivacyInterceptor(std::function<bool()> shouldBlockTrackers, QObject *parent = nullptr)
        : QWebEngineUrlRequestInterceptor(parent)
        , m_shouldBlockTrackers(std::move(shouldBlockTrackers))
    {
    }

    void interceptRequest(QWebEngineUrlRequestInfo &info) override
    {
        const QUrl reqUrl = info.requestUrl();
        const QString urlStr = reqUrl.toString();

        // 1. 발신자 추적 픽셀 차단
        if (!m_shouldBlockTrackers || !m_shouldBlockTrackers())
            return;
        static const QRegularExpression trackUrlRe(
            R"((?:readreceipt|/notify/\?img=|/rcpt/|/receipt[/?._-]|/track[/?._-]|/open[/?._-]|/op/|pixel\.|/pixel|_pixel|beacon\.|/beacon|mailtrack|sendgrid\.net/wf/open|list-manage\.com/track/open|stibee\.com/.*/open|1x1|spacer\.gif|clear\.gif|blank\.gif))",
            QRegularExpression::CaseInsensitiveOption);
        if (trackUrlRe.match(urlStr).hasMatch()) {
            info.block(true);
        }
    }

private:
    std::function<bool()> m_shouldBlockTrackers;
};

class SollunaEmailWebPage : public QWebEnginePage
{
public:
    explicit SollunaEmailWebPage(std::function<void(const QUrl &)> linkHandler, QObject *parent = nullptr)
        : QWebEnginePage(parent)
        , m_linkHandler(std::move(linkHandler))
    {
    }

protected:
    void javaScriptConsoleMessage(JavaScriptConsoleMessageLevel level,
                                  const QString &message,
                                  int lineNumber,
                                  const QString &sourceID) override
    {
        Q_UNUSED(level);
        Q_UNUSED(message);
        Q_UNUSED(lineNumber);
        Q_UNUSED(sourceID);
        // 서드파티 마케팅 이메일 HTML의 Mixed Content / CORS font 등 무해한 브라우저 콘솔 노이즈 출력 억제
    }

    bool acceptNavigationRequest(const QUrl &url, NavigationType type, bool isMainFrame) override
    {
        Q_UNUSED(isMainFrame);
        if (type == QWebEnginePage::NavigationTypeLinkClicked) {
            if (m_linkHandler)
                m_linkHandler(url);
            return false;
        }
        const QString scheme = url.scheme().toLower();
        if (scheme == QLatin1String("solluna") || scheme == QLatin1String("mailto")) {
            if (m_linkHandler)
                m_linkHandler(url);
            return false;
        }
        return QWebEnginePage::acceptNavigationRequest(url, type, isMainFrame);
    }

    QWebEnginePage *createWindow(WebWindowType type) override
    {
        Q_UNUSED(type);
        // target="_blank" 링크 클릭 시 새 빈 창을 띄우는 대신 기본 외부 브라우저로 전달
        SollunaEmailWebPage *popupIntercept = new SollunaEmailWebPage(m_linkHandler, this);
        connect(popupIntercept, &QWebEnginePage::urlChanged, this, [this, popupIntercept](const QUrl &url) {
            if (url.isValid() && !url.isEmpty() && url.scheme() != QLatin1String("about")) {
                if (m_linkHandler)
                    m_linkHandler(url);
                popupIntercept->deleteLater();
            }
        });
        return popupIntercept;
    }

private:
    std::function<void(const QUrl &)> m_linkHandler;
};
} // namespace
#endif

static QString stripTrackingPixelsFromHtml(const QString &inputHtml, int *outStrippedCount = nullptr)
{
    int stripped = 0;
    QString html = inputHtml;

    static const QRegularExpression imgTagRe(
        R"(<img\b[^>]*>)",
        QRegularExpression::CaseInsensitiveOption);

    // 1. 네이버·다음 등 웹메일이 <table style='display:none'>...<img src=".../readReceipt/notify/..."></table> 또는
    //    <div|span|p style='display:none|visibility:hidden|opacity:0'>...<img ...> 형태로 감싼 숨김 컨테이너 내부의 추적 이미지를 먼저 제거
    static const QRegularExpression hiddenBlockRe(
        R"(<(table|div|span|p)\b[^>]*\bstyle\s*=\s*["'][^"']*(?:display\s*:\s*none|visibility\s*:\s*hidden|opacity\s*:\s*0\b)[^"']*["'][^>]*>([\s\S]*?)</\1\s*>)",
        QRegularExpression::CaseInsensitiveOption);
    {
        QString out;
        int lastPos = 0;
        QRegularExpressionMatchIterator hit = hiddenBlockRe.globalMatch(html);
        while (hit.hasNext()) {
            const QRegularExpressionMatch hm = hit.next();
            out.append(html.mid(lastPos, hm.capturedStart() - lastPos));
            const QString blockFull = hm.captured(0);
            int imgsInHidden = 0;
            QRegularExpressionMatchIterator iit = imgTagRe.globalMatch(blockFull);
            while (iit.hasNext()) {
                iit.next();
                ++imgsInHidden;
            }
            if (imgsInHidden > 0) {
                stripped += imgsInHidden;
                // 숨김 블록에서 <img> 태그들을 완전히 제거
                QString cleanedBlock = blockFull;
                cleanedBlock.remove(imgTagRe);
                out.append(cleanedBlock);
            } else {
                out.append(blockFull);
            }
            lastPos = hm.capturedEnd();
        }
        out.append(html.mid(lastPos));
        html = out;
    }

    // 2. 개별 <img> 태그 자체의 크기(0/1px), 숨김 스타일, 또는 수신확인/트래킹 URL 패턴(네이버 readReceipt/notify, Mailtrap /op/, SendGrid 등) 검사 및 제거
    static const QRegularExpression trackAttrRe(
        R"((?:\b(?:width|height)\s*=\s*["']?[01](?:px)?["']?(?:\s|/|>|$)|(?:width|height)\s*:\s*[01](?:px)?\b|display\s*:\s*none|visibility\s*:\s*hidden|opacity\s*:\s*0\b|readreceipt|/notify/\?img=|/rcpt/|/receipt[/?._-]|/track[/?._-]|/open[/?._-]|/op/|pixel\.|/pixel|_pixel|beacon\.|/beacon|mailtrack|sendgrid\.net/wf/open|list-manage\.com/track/open|stibee\.com/.*/open|1x1|spacer\.gif|clear\.gif|blank\.gif))",
        QRegularExpression::CaseInsensitiveOption);
    {
        QString cleanedHtml;
        int lastPos = 0;
        QRegularExpressionMatchIterator it = imgTagRe.globalMatch(html);
        while (it.hasNext()) {
            const QRegularExpressionMatch mImg = it.next();
            cleanedHtml.append(html.mid(lastPos, mImg.capturedStart() - lastPos));
            const QString tagStr = mImg.captured(0);
            if (trackAttrRe.match(tagStr).hasMatch()) {
                ++stripped;
            } else {
                cleanedHtml.append(tagStr);
            }
            lastPos = mImg.capturedEnd();
        }
        cleanedHtml.append(html.mid(lastPos));
        html = cleanedHtml;
    }

    if (outStrippedCount)
        *outStrippedCount = stripped;
    return html;
}

static QString extractUnsubscribeTargetFromHtml(const QString &htmlBody)
{
    if (htmlBody.trimmed().isEmpty())
        return QString();

    static const QRegularExpression anchorRe(
        R"(<a\b[^>]*\bhref\s*=\s*["']([^"']+)["'][^>]*>([\s\S]*?)</a\s*>)",
        QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression unsubKeywordRe(
        R"((?:unsubscribe|optout|opt-out|opt_out|/unsub\b|수신\s*거부|구독\s*취소|수신동의\s*철회|거부\s*신청|메일\s*수신\s*거부|配信停止|登録解除|購読解除))",
        QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression htmlStripRe(R"(<[^>]*>)");

    QRegularExpressionMatchIterator it = anchorRe.globalMatch(htmlBody);
    while (it.hasNext()) {
        const QRegularExpressionMatch m = it.next();
        QString href = m.captured(1).trimmed();
        href.replace(QLatin1String("&amp;"), QLatin1String("&"));
        if (href.isEmpty() || href.startsWith(QLatin1Char('#')))
            continue;
        QString text = m.captured(2);
        text.remove(htmlStripRe);
        if (unsubKeywordRe.match(href).hasMatch() || unsubKeywordRe.match(text).hasMatch()) {
            if (href.startsWith(QLatin1String("http://"), Qt::CaseInsensitive)
                || href.startsWith(QLatin1String("https://"), Qt::CaseInsensitive)
                || href.startsWith(QLatin1String("mailto:"), Qt::CaseInsensitive)) {
                return href;
            }
        }
    }

    // 평문 뉴스레터에서 URL 자체에 optout / unsubscribe 경로가 포함된 경우 추출
    static const QRegularExpression plainUnsubUrlRe(
        R"(https?://[^\s"'<>]*(?:unsubscribe|optout|opt-out|opt_out|/unsub\b)[^\s"'<>]*)",
        QRegularExpression::CaseInsensitiveOption);
    const QRegularExpressionMatch pm = plainUnsubUrlRe.match(htmlBody);
    if (pm.hasMatch()) {
        QString url = pm.captured(0).trimmed();
        url.replace(QLatin1String("&amp;"), QLatin1String("&"));
        while (url.endsWith(QLatin1Char(')')) || url.endsWith(QLatin1Char(']')) || url.endsWith(QLatin1Char('.')))
            url.chop(1);
        return url;
    }

    return QString();
}

QSet<QString> s_fullBodyFetchedConvIds;

static QString extractCleanEmailLower(const QString &raw)
{
    QString s = raw.trimmed();
    if (s.contains('<') && s.contains('>')) {
        s = s.section('<', 1, 1).section('>', 0, 0).trimmed();
    }
    s.remove('"').remove('\'');
    return s.trimmed().toLower();
}

static bool isSenderMatchedByBlockedRule(const QString &rawSenderEmail, const QString &rawBlockedEntry)
{
    const QString senderClean = extractCleanEmailLower(rawSenderEmail);
    const QString senderRawLower = rawSenderEmail.trimmed().toLower();
    const QString blockedClean = extractCleanEmailLower(rawBlockedEntry);
    if (blockedClean.isEmpty() || (senderClean.isEmpty() && senderRawLower.isEmpty()))
        return false;
    if (senderClean == blockedClean || senderRawLower == blockedClean)
        return true;
    const QString domainRule = blockedClean.startsWith('@') ? blockedClean : (QStringLiteral("@") + blockedClean);
    return (!senderClean.isEmpty() && senderClean.endsWith(domainRule))
           || (!senderRawLower.isEmpty() && senderRawLower.endsWith(domainRule));
}

static bool unblockSenderFromSpamLists(PrivacyHubSettings &privacySettings,
                                       const QString &rawSenderEmail,
                                       QStringList *outUnblockedSenders = nullptr)
{
    const QString senderClean = extractCleanEmailLower(rawSenderEmail);
    const QString senderRawLower = rawSenderEmail.trimmed().toLower();
    if (senderClean.isEmpty() && senderRawLower.isEmpty())
        return false;

    bool unblocked = false;
    for (int bi = privacySettings.blockedSendersList.size() - 1; bi >= 0; --bi) {
        if (isSenderMatchedByBlockedRule(rawSenderEmail, privacySettings.blockedSendersList[bi])) {
            privacySettings.blockedSendersList.removeAt(bi);
            unblocked = true;
        }
    }

    // 사용자 정의 필터 규칙 중 해당 발신자를 스팸함(spam)으로 강제 이동시키는 활성 규칙이 있으면 함께 해제하여 재스팸화 방지
    for (MailFilterRule &r : MailI18n::filterRules()) {
        if (!r.enabled || r.targetFolderId != QStringLiteral("spam") || r.matchField != QStringLiteral("From"))
            continue;
        const QStringList tokens = r.matchQuery.split(QRegularExpression(QStringLiteral("[,;]")), Qt::SkipEmptyParts);
        QStringList keptTokens;
        bool ruleModified = false;
        for (const QString &tk : tokens) {
            if (isSenderMatchedByBlockedRule(rawSenderEmail, tk)) {
                ruleModified = true;
                unblocked = true;
            } else {
                keptTokens.append(tk.trimmed());
            }
        }
        if (ruleModified) {
            r.matchQuery = keptTokens.join(QStringLiteral(", "));
            if (keptTokens.isEmpty()) {
                r.enabled = false;
            }
        }
    }

    if (unblocked && outUnblockedSenders) {
        const QString dispAddr = senderClean.isEmpty() ? senderRawLower : senderClean;
        if (!dispAddr.isEmpty() && !outUnblockedSenders->contains(dispAddr, Qt::CaseInsensitive)) {
            outUnblockedSenders->append(dispAddr);
        }
    }
    return unblocked;
}

static QString normalizeMailBaseSubject(const QString &rawSubject, bool *outIsReceipt = nullptr, bool *outIsReply = nullptr)
{
    QString s = rawSubject.trimmed();
    bool isReceipt = false;
    bool isReply = false;

    static const QRegularExpression receiptPrefixRe(
        R"(^\s*(?:읽음\s*[:\-]|수신\s*확인\s*[:\-]|Read\s*[:\-]|Return\s+Receipt\s*[:\-]?|Disposition\s+Notification\s*[:\-]?)\s*)",
        QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression replyOrFwdPrefixRe(
        R"(^\s*(?:(?:RE|Re|FW|Fwd|AW|SV|답장|회신|전달)(?:\[\d+\])?\s*:\s*)+)",
        QRegularExpression::CaseInsensitiveOption);

    bool changed = true;
    while (changed) {
        changed = false;
        const QRegularExpressionMatch rm = receiptPrefixRe.match(s);
        if (rm.hasMatch()) {
            isReceipt = true;
            s = s.mid(rm.capturedLength()).trimmed();
            changed = true;
        }
        const QRegularExpressionMatch rfm = replyOrFwdPrefixRe.match(s);
        if (rfm.hasMatch()) {
            const QString prefixCap = rfm.captured(0);
            if (prefixCap.contains(QLatin1String("re"), Qt::CaseInsensitive)
                || prefixCap.contains(QStringLiteral("답장"))
                || prefixCap.contains(QStringLiteral("회신"))) {
                isReply = true;
            }
            s = s.mid(rfm.capturedLength()).trimmed();
            changed = true;
        }
    }

    if (outIsReceipt)
        *outIsReceipt = isReceipt;
    if (outIsReply)
        *outIsReply = isReply;
    return s;
}

static bool reconcileSentReadReceipts(QList<ConversationItem> &conversations)
{
    bool anyUpdated = false;

    for (ConversationItem &sentConv : conversations) {
        if (sentConv.folderId != QStringLiteral("sent") || sentConv.messages.isEmpty())
            continue;
        if (sentConv.readReceiptConfirmed)
            continue;

        const QString sentBaseSubj = normalizeMailBaseSubject(sentConv.subject);
        if (sentBaseSubj.isEmpty())
            continue;

        const QString recipSummaryLower = sentConv.latestMessage().recipientSummary.trimmed().toLower();

        for (const ConversationItem &inc : conversations) {
            if (inc.folderId == QStringLiteral("sent") || inc.folderId == QStringLiteral("drafts") || inc.messages.isEmpty())
                continue;
            if (!sentConv.accountId.isEmpty() && !inc.accountId.isEmpty() && inc.accountId != sentConv.accountId)
                continue;

            bool isExplicitReceipt = false;
            bool isReply = false;
            const QString incBaseSubj = normalizeMailBaseSubject(inc.subject, &isExplicitReceipt, &isReply);
            const MessageItem &incMsg = inc.latestMessage();
            const QString bodySample = incMsg.snippet + QLatin1Char(' ') + incMsg.htmlBody.left(1200);
            if (bodySample.contains(QStringLiteral("disposition-notification"), Qt::CaseInsensitive)) {
                isExplicitReceipt = true;
            }
            if (!isReply && (bodySample.contains(QStringLiteral("-----Original Message-----"), Qt::CaseInsensitive)
                             || bodySample.contains(QStringLiteral("wrote:"), Qt::CaseInsensitive))) {
                isReply = true;
            }

            if (!isExplicitReceipt && !isReply)
                continue;

            const bool subjectMatches = (!incBaseSubj.isEmpty() && incBaseSubj.compare(sentBaseSubj, Qt::CaseInsensitive) == 0)
                                        || (isExplicitReceipt && !incBaseSubj.isEmpty() && sentBaseSubj.contains(incBaseSubj, Qt::CaseInsensitive));
            if (!subjectMatches)
                continue;

            const QString incSenderEmail = incMsg.senderEmail.trimmed().toLower();
            const bool senderMatchesRecipient = (!incSenderEmail.isEmpty() && recipSummaryLower.contains(incSenderEmail))
                                                || bodySample.contains(sentBaseSubj, Qt::CaseInsensitive);
            if (isExplicitReceipt || senderMatchesRecipient) {
                sentConv.readReceiptRequested = true;
                sentConv.readReceiptConfirmed = true;
                anyUpdated = true;
                break;
            }
        }
    }

    return anyUpdated;
}

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
    , ui(new Ui::MainWindow)
{
    ui->setupUi(this);
    setWindowTitle("SollunaMail");
    setWindowIcon(MailIcons::sollunaAppIcon(0));
    setMinimumSize(1160, 620);
    if (QScreen *screen = QGuiApplication::primaryScreen()) {
        const QRect avail = screen->availableGeometry();
        const int targetW = qMin(1640, int(avail.width() * 0.96));
        const int targetH = qMin(860, int(avail.height() * 0.88));
        resize(targetW, targetH);
        move(avail.x() + (avail.width() - targetW) / 2,
             avail.y() + (avail.height() - targetH) / 2);
    } else {
        resize(1640, 860);
    }

    m_palette = MailThemeManager::getPalette(m_currentThemeName);
    const QString globalQss = MailThemeManager::buildGlobalStyleSheet(m_palette);
    setStyleSheet(globalQss);
    if (qApp) {
        qApp->setStyleSheet(globalQss);
    }

    m_googleProvider = new GoogleProvider(this);

    initializePreloadedData();
    setupMainMenuBar();
    setupMainLayout();
    retranslateUi();

    // Connect GoogleProvider live signals
    connect(m_googleProvider, &GoogleProvider::statusMessage, this, [this](const QString &msg) {
        setWindowTitle(QString("SollunaMail — %1").arg(msg));
    });

    connect(m_googleProvider, &GoogleProvider::oauthTokenReceived, this,
            [this](const QString &email, const QString &accessToken, const QString &refreshToken) {
        QString matchedRefresh = refreshToken;
        AccountItem updatedAcc;
        bool foundAcc = false;
        for (AccountItem &acc : m_accounts) {
            if (!email.isEmpty() && acc.email.compare(email, Qt::CaseInsensitive) == 0) {
                acc.oauthAccessToken = accessToken;
                if (!refreshToken.isEmpty())
                    acc.oauthRefreshToken = refreshToken;
                matchedRefresh = acc.oauthRefreshToken;
                if (acc.imapHost.trimmed().isEmpty())
                    acc.imapHost = QStringLiteral("imap.gmail.com");
                if (acc.smtpHost.trimmed().isEmpty())
                    acc.smtpHost = QStringLiteral("smtp.gmail.com");
                updatedAcc = acc;
                foundAcc = true;
            }
        }
        saveAccountsToDisk();
        m_googleProvider->syncGoogleCalendarEvents(email, accessToken, matchedRefresh);
        m_googleProvider->syncGoogleContacts(email, accessToken, matchedRefresh);
        if (foundAcc) {
            m_googleProvider->fetchAccountAvatar(updatedAcc);
            const QString targetFolder = (m_selectedFolderId == QStringLiteral("snoozed") || m_selectedFolderId.isEmpty())
                                             ? QStringLiteral("inbox")
                                             : m_selectedFolderId;
            m_googleProvider->fetchImapFolder(updatedAcc, targetFolder, 20, 0);
        }
    });

    connect(m_googleProvider, &GoogleProvider::accountAvatarFetched, this,
            [this](const QString &accountEmail, const QString &pngBase64) {
        if (pngBase64.trimmed().isEmpty())
            return;
        bool changed = false;
        for (AccountItem &acc : m_accounts) {
            if (acc.email.compare(accountEmail, Qt::CaseInsensitive) == 0) {
                acc.avatarPngBase64 = pngBase64;
                changed = true;
            }
        }
        if (changed) {
            rebuildLeftBarAccounts();
            refreshFolderSelectionStyles();
            saveAccountsToDisk();
        }
    });

    connect(m_googleProvider, &GoogleProvider::imapConversationsFetched, this,
            [this](const QString &accountId, const QList<ConversationItem> &newConvs) {
        Q_UNUSED(accountId);
        harvestContactsFromConversations(newConvs);
    });

    connect(m_googleProvider, &GoogleProvider::imapCustomFoldersDiscovered, this,
            [this](const QString &accountId, const QList<FolderItem> &discoveredFolders) {
        if (accountId.isEmpty())
            return;

        AccountItem matchedAcc;
        bool foundAcc = false;
        QString ownerEmail;
        for (const AccountItem &acc : m_accounts) {
            if (acc.id == accountId) {
                matchedAcc = acc;
                foundAcc = true;
                ownerEmail = acc.email;
                break;
            }
        }
        if (foundAcc && matchedAcc.isPop3())
            return;

        // 기존에 사용자가 지정한 폴더 색상(dotColor) 보존 및 구버전 로컬 가짜 폴더(rawImapMailbox 비어 있음) 서버 폴더로 자동 마이그레이션
        static QSet<QString> s_autoCreatedServerFolders;
        QMap<QString, QColor> prevColorsById;
        QMap<QString, QColor> prevColorsByName;
        QMap<QString, QString> legacyIdToName;

        for (int i = m_folders.size() - 1; i >= 0; --i) {
            const FolderItem &f = m_folders[i];
            if (!f.isCustom)
                continue;
            bool sameAcc = (f.accountId == accountId)
                           || (!ownerEmail.isEmpty() && !f.accountEmail.isEmpty()
                               && f.accountEmail.compare(ownerEmail, Qt::CaseInsensitive) == 0);
            if (!sameAcc)
                continue;

            prevColorsById.insert(f.id, f.dotColor);
            if (!f.name.trimmed().isEmpty())
                prevColorsByName.insert(f.name.trimmed().toLower(), f.dotColor);

            if (f.rawImapMailbox.isEmpty() && !f.name.trimmed().isEmpty()) {
                legacyIdToName.insert(f.id, f.name.trimmed());
                bool existsOnServer = false;
                for (const FolderItem &df : discoveredFolders) {
                    if (df.name.trimmed().compare(f.name.trimmed(), Qt::CaseInsensitive) == 0) {
                        existsOnServer = true;
                        break;
                    }
                }
                const QString createKey = accountId + QStringLiteral("|") + f.name.trimmed().toLower();
                if (!existsOnServer && foundAcc && !s_autoCreatedServerFolders.contains(createKey)) {
                    s_autoCreatedServerFolders.insert(createKey);
                    m_googleProvider->createImapCustomFolder(matchedAcc, f.name.trimmed());
                }
            }
            // IMAP 계정은 로컬 전용 폴더를 남기지 않고 100% 실제 IMAP 서버 폴더(discoveredFolders)로만 구성
            m_folders.removeAt(i);
        }

        QSet<QString> existingIds;
        for (const FolderItem &f : m_folders)
            existingIds.insert(f.id);

        bool migratedAny = false;
        for (FolderItem nf : discoveredFolders) {
            nf.accountId = accountId;
            if (!ownerEmail.isEmpty())
                nf.accountEmail = ownerEmail;
            if (prevColorsById.contains(nf.id) && prevColorsById.value(nf.id).isValid()) {
                nf.dotColor = prevColorsById.value(nf.id);
            } else if (prevColorsByName.contains(nf.name.trimmed().toLower())
                       && prevColorsByName.value(nf.name.trimmed().toLower()).isValid()) {
                nf.dotColor = prevColorsByName.value(nf.name.trimmed().toLower());
            }
            if (!existingIds.contains(nf.id)) {
                existingIds.insert(nf.id);
                m_folders.append(nf);
            }

            // 구버전 로컬 가짜 폴더 ID(custom_acc_xxx_21d17a 등)를 가리키던 필터 규칙 및 메일들을 실제 IMAP 서버 폴더 ID(nf.id)로 이관
            for (auto it = legacyIdToName.begin(); it != legacyIdToName.end(); ++it) {
                if (it.value().compare(nf.name.trimmed(), Qt::CaseInsensitive) == 0 && it.key() != nf.id) {
                    const QString oldFakeId = it.key();
                    for (MailFilterRule &r : MailI18n::filterRules()) {
                        if (r.targetFolderId == oldFakeId) {
                            r.targetFolderId = nf.id;
                            migratedAny = true;
                        }
                    }
                    for (ConversationItem &c : m_conversations) {
                        if (c.folderId == oldFakeId) {
                            if (c.accountId == accountId) {
                                c.folderId = nf.id;
                                c.folderTag = nf.name;
                            } else {
                                c.folderId = QStringLiteral("inbox");
                                c.folderTag = QStringLiteral("Inbox");
                            }
                            migratedAny = true;
                        }
                    }
                    if (m_selectedFolderId == oldFakeId) {
                        m_selectedFolderId = nf.id;
                    }
                }
            }
        }

        rebuildCustomFoldersList();
        saveAccountsToDisk();
        if (migratedAny) {
            applyFilterRulesToConversations();
            saveConversationsCacheToDisk();
            refreshConversationList();
        }
    });

    connect(m_googleProvider, &GoogleProvider::imapSingleMessageBodyFetched, this,
            [this](const QString &conversationId, const QString &fullHtmlBody, const QList<AttachmentItem> &attachments) {
        if (conversationId.isEmpty() && fullHtmlBody.isEmpty() && attachments.isEmpty())
            return;
        if (!conversationId.isEmpty()) {
            s_fullBodyFetchedConvIds.insert(conversationId);
        }
        QString matchedAccId;
        QString matchedSigKey;
        for (const ConversationItem &c : m_conversations) {
            if (c.id == conversationId) {
                matchedAccId = c.accountId;
                matchedSigKey = conversationSignatureKey(c);
                break;
            }
        }
        bool updatedSelected = false;
        for (ConversationItem &c : m_conversations) {
            const bool isTarget = (c.id == conversationId)
                                  || (!matchedSigKey.isEmpty()
                                      && c.accountId == matchedAccId
                                      && conversationSignatureKey(c) == matchedSigKey);
            if (isTarget && !c.messages.isEmpty()) {
                s_fullBodyFetchedConvIds.insert(c.id);
                if (!attachments.isEmpty()) {
                    c.messages.last().attachments = attachments;
                }
                if (!fullHtmlBody.isEmpty()) {
                    c.messages.last().htmlBody = fullHtmlBody;
                    const QString cleanSnip = GoogleProvider::htmlToPlainTextSnippet(
                        fullHtmlBody, c.messages.last().snippet, 50);
                    if (!cleanSnip.isEmpty()) {
                        c.messages.last().snippet = cleanSnip;
                    }
                } else if (c.messages.last().htmlBody.startsWith(QStringLiteral("<!--SM_"))) {
                    c.messages.last().htmlBody = QStringLiteral("<div></div>");
                }
                if (m_selectedConvId == c.id) {
                    updatedSelected = true;
                }
            }
        }
        if (updatedSelected) {
            if (m_readingBodyBrowser || m_readingWebView) {
                const int curRow = m_convListWidget ? m_convListWidget->currentRow() : -1;
                if (curRow >= 0 && curRow < m_filteredConvIndices.size()) {
                    displayConversation(curRow, false);
                }
            }
            for (const ConversationItem &c : m_conversations) {
                if (c.id == m_selectedConvId) {
                    refreshReadingAttachmentsBar(c);
                    break;
                }
            }
        }
        if (m_convListWidget && m_convListWidget->viewport()) {
            m_convListWidget->viewport()->update();
        }
        saveConversationsCacheToDisk();
    });

    connect(m_googleProvider, &GoogleProvider::imapFolderPageFetched, this,
            [this](const QString &accountId,
                   const QString &folderId,
                   const QList<ConversationItem> &pageConvs,
                   int lowestFetchedSeq,
                   int totalExistsCount,
                   bool isInitialPage) {
        const QString folderKey = accountId + "|" + folderId;
        if (totalExistsCount >= 0 && (totalExistsCount > 0 || isInitialPage)) {
            m_folderTotalExists[folderKey] = totalExistsCount;
        }
        m_folderLoadingMore.remove(folderKey);

        if (pageConvs.size() >= 20
            && (m_selectedAccountId == "acc_unified" || accountId == m_selectedAccountId)
            && (folderId == m_selectedFolderId || m_selectedFolderId == "starred" || m_selectedFolderId == "important")) {
            showMailLoadingToast(QString(), 2000);
        }

        bool listStructureChanged = false;

        // 0. 서버 기준(Server-Truth) 정합성 보장 + 이미 로드한 본문/하위 페이지 보존:
        //    - IMAP 계정(!isPop3Account): 초기 페이지(최신 50건) 동기화 시, 해당 구간(seq >= lowestFetchedSeq)에서
        //      서버에서 삭제·이동되어 사라진 항목만 제거하고, 이미 클릭하여 전체 본문을 로드해 둔 메일과
        //      스크롤로 내려가 추가 로드한 하위 페이지(seq < lowestFetchedSeq)는 그대로 보존한다.
        //    - POP3 계정(isPop3Account): 받은편지함(inbox)은 신규 메일만 증분 다운로드하므로 기존 로컬 메일을 유지한다.
        bool isPop3Account = false;
        for (const AccountItem &acc : m_accounts) {
            if (acc.id == accountId) {
                isPop3Account = acc.isPop3();
                break;
            }
        }
        QSet<QString> prevRequestedSentSubjects;
        QSet<QString> prevConfirmedSentSubjects;
        if (folderId == QStringLiteral("sent")) {
            for (const ConversationItem &ex : m_conversations) {
                if (ex.accountId == accountId && ex.folderId == QStringLiteral("sent")) {
                    if (ex.readReceiptRequested)
                        prevRequestedSentSubjects.insert(ex.subject.trimmed());
                    if (ex.readReceiptConfirmed)
                        prevConfirmedSentSubjects.insert(ex.subject.trimmed());
                }
            }
        }
        auto extractRawSeq = [](const QString &convId) -> int {
            const int lastUnder = convId.lastIndexOf('_');
            return (lastUnder > 0) ? convId.mid(lastUnder + 1).toInt() : 0;
        };
        if (!isPop3Account || folderId != QStringLiteral("inbox")) {
            const QString imapPrefix = QStringLiteral("imap_%1_").arg(accountId);
            if (isInitialPage && pageConvs.isEmpty() && totalExistsCount == 0
                && folderId != QStringLiteral("important")
                && folderId != QStringLiteral("starred")) {
                if (m_googleProvider) {
                    m_googleProvider->clearLoadedImapUids(accountId, folderId);
                }
                for (int i = m_conversations.size() - 1; i >= 0; --i) {
                    const ConversationItem &ex = m_conversations[i];
                    if (ex.accountId == accountId && ex.folderId == folderId && ex.id.startsWith(imapPrefix)
                        && ex.scheduledSendIso.isEmpty()) {
                        m_conversations.removeAt(i);
                        listStructureChanged = true;
                    }
                }
            } else if (!pageConvs.isEmpty() && lowestFetchedSeq > 0) {
                QSet<QString> serverPageIds;
                QSet<QString> serverPageSigs;
                int highestFetchedSeq = lowestFetchedSeq;
                for (const ConversationItem &sc : pageConvs) {
                    serverPageIds.insert(sc.id);
                    if (!sc.messages.isEmpty()) {
                        serverPageSigs.insert(conversationSignatureKey(sc));
                    }
                    const int sSeq = sc.imapSeq > 0 ? sc.imapSeq : extractRawSeq(sc.id);
                    if (sSeq > highestFetchedSeq)
                        highestFetchedSeq = sSeq;
                }
                if (isInitialPage && totalExistsCount > highestFetchedSeq) {
                    highestFetchedSeq = totalExistsCount;
                }
                for (int i = m_conversations.size() - 1; i >= 0; --i) {
                    const ConversationItem &ex = m_conversations[i];
                    if (ex.accountId == accountId && ex.folderId == folderId && ex.id.startsWith(imapPrefix)) {
                        if (!ex.scheduledSendIso.isEmpty()) {
                            continue;
                        }
                        const int exSeq = ex.imapSeq > 0 ? ex.imapSeq : extractRawSeq(ex.id);
                        const bool isFilterMovedPlaceholder = (ex.imapSeq == 0
                                                               || !ex.previousFolderId.isEmpty()
                                                               || (isInitialPage && exSeq > highestFetchedSeq));
                        // 필터 규칙으로 방금 이동된 항목이 서버 페이지(serverPageSigs)에 포함되어 있으면
                        // 아래 병합 루프에서 실제 폴더 UID·순번으로 승격(중요/별표/읽음/본문 보존)할 수 있도록 남겨둔다.
                        if (isFilterMovedPlaceholder && serverPageSigs.contains(conversationSignatureKey(ex))) {
                            continue;
                        }
                        if (!serverPageIds.contains(ex.id)
                            && (isFilterMovedPlaceholder
                                || (exSeq >= lowestFetchedSeq && (isInitialPage || exSeq <= highestFetchedSeq)))) {
                            if (m_googleProvider) {
                                const int exUid = ex.imapUid > 0 ? ex.imapUid : extractRawSeq(ex.id);
                                m_googleProvider->unregisterLoadedImapUid(accountId, folderId, exUid);
                            }
                            m_conversations.removeAt(i);
                            listStructureChanged = true;
                        }
                    } else if (ex.accountId == accountId && ex.folderId == folderId
                               && (folderId == QStringLiteral("sent") || folderId == QStringLiteral("drafts"))
                               && ex.id.startsWith(QStringLiteral("conv_local_"))
                               && ex.scheduledSendIso.isEmpty()) {
                        m_conversations.removeAt(i);
                        listStructureChanged = true;
                    }
                }
            }
        }

        struct PendingServerMoveBatch {
            QString targetFolderId;
            QString targetDisplayName;
            bool markRead = false;
            bool markStar = false;
            QList<int> seqNums;
        };
        QMap<QString, PendingServerMoveBatch> pendingServerMoves;
        auto queueServerMove = [&](int seq, const QString &tgtId, const QString &tgtName, bool mRead, bool mStar) {
            if (seq <= 0 || tgtId.isEmpty() || tgtId == folderId)
                return;
            PendingServerMoveBatch &b = pendingServerMoves[tgtId];
            b.targetFolderId = tgtId;
            if (!tgtName.isEmpty())
                b.targetDisplayName = tgtName;
            b.markRead = b.markRead || mRead;
            b.markStar = b.markStar || mStar;
            if (!b.seqNums.contains(seq))
                b.seqNums.append(seq);
        };

        QList<int> newlyAddedIndices;
        QList<ConversationItem> brandNewUnreadInboxArrivals;
        for (const ConversationItem &rawC : pageConvs) {
            // Step 1-A 메타데이터 전용 갱신 항목(이미 로컬에 로드된 UID이므로 헤더/본문을 중복 다운로드하지 않고 순번과 플래그만 전달된 항목):
            if (rawC.messages.isEmpty() && rawC.imapUid > 0) {
                for (ConversationItem &ex : m_conversations) {
                    if (ex.accountId == accountId && ex.folderId == folderId
                        && (ex.id == rawC.id || ex.imapUid == rawC.imapUid)) {
                        if (ex.imapSeq != rawC.imapSeq) {
                            ex.imapSeq = rawC.imapSeq;
                            listStructureChanged = true;
                        }
                        ex.imapUid = rawC.imapUid;
                        const QString exSig = conversationSignatureKey(ex);
                        if (!ex.isUnread || !rawC.isUnread || m_readConvSigs.contains(exSig)) {
                            ex.isUnread = false;
                            m_readConvSigs.insert(exSig);
                        } else {
                            ex.isUnread = rawC.isUnread;
                        }
                        if (m_unstarredConvSigs.contains(exSig)) {
                            ex.isStarred = false;
                        } else if (m_starredConvSigs.contains(exSig) || ex.isStarred || rawC.isStarred || (folderId == QStringLiteral("starred"))) {
                            ex.isStarred = true;
                        }
                        if (m_unimportantConvSigs.contains(exSig)) {
                            ex.isImportant = false;
                        } else if (m_importantConvSigs.contains(exSig) || ex.isImportant || rawC.isImportant || (folderId == QStringLiteral("important"))) {
                            ex.isImportant = true;
                        }
                        ex.isReplied = ex.isReplied || rawC.isReplied || m_repliedConvSigs.contains(exSig);
                        ex.isForwarded = ex.isForwarded || rawC.isForwarded || m_forwardedConvSigs.contains(exSig);
                        if (ex.isReplied)
                            m_repliedConvSigs.insert(exSig);
                        if (ex.isForwarded)
                            m_forwardedConvSigs.insert(exSig);
                        break;
                    }
                }
                continue;
            }

            ConversationItem c = rawC;
            const int rawUidNum = c.imapUid > 0 ? c.imapUid : extractRawSeq(rawC.id);
            if (c.imapUid <= 0)
                c.imapUid = rawUidNum;
            if (c.imapSeq <= 0)
                c.imapSeq = rawUidNum;
            const QString sigKey = conversationSignatureKey(c);
            // 1. 영구 삭제되었거나 이미 삭제/휴지통 이동 처리된 메일이 백그라운드 동기화로 다시 살아나는 현상 차단
            if ((!c.id.startsWith(QStringLiteral("imap_")) && m_deletedConvIds.contains(c.id))
                || m_deletedConvIds.contains(QStringLiteral("perm|") + sigKey)) {
                continue;
            }
            if (folderId != QStringLiteral("trash") && folderId != QStringLiteral("spam")
                && m_deletedConvIds.contains(QStringLiteral("trash|") + sigKey)) {
                continue;
            }

            // 1-B. 이미 사용자가 읽은 메일(m_readConvSigs)이거나 서버에서 읽음(\Seen) 상태인 메일은
            //      백그라운드 동기화 시 다시 안 읽음(새로운 메일)으로 되살아나지 않도록 고정
            if (!c.isUnread) {
                m_readConvSigs.insert(sigKey);
                m_notifiedConvIds.insert(sigKey);
            } else if (m_readConvSigs.contains(sigKey)) {
                c.isUnread = false;
                m_notifiedConvIds.insert(sigKey);
            }
            if (c.isReplied) {
                m_repliedConvSigs.insert(sigKey);
            } else if (m_repliedConvSigs.contains(sigKey)) {
                c.isReplied = true;
            }
            if (c.isForwarded) {
                m_forwardedConvSigs.insert(sigKey);
            } else if (m_forwardedConvSigs.contains(sigKey)) {
                c.isForwarded = true;
            }
            if (m_unstarredConvSigs.contains(sigKey)) {
                c.isStarred = false;
            } else if (m_starredConvSigs.contains(sigKey) || c.isStarred) {
                c.isStarred = true;
            }
            if (m_unimportantConvSigs.contains(sigKey)) {
                c.isImportant = false;
            } else if (m_importantConvSigs.contains(sigKey) || c.isImportant) {
                c.isImportant = true;
            }

            // 2. 개인정보 보호 허브 — 차단된 발신자 자동 스팸 처리 (Block senders)
            if (folderId == QStringLiteral("inbox") && m_privacySettings.privacyMode && m_privacySettings.blockSenders
                && !c.messages.isEmpty()) {
                const QString incomingSender = c.latestMessage().senderEmail;
                bool isBlockedSender = false;
                for (const QString &blockedAddr : m_privacySettings.blockedSendersList) {
                    if (isSenderMatchedByBlockedRule(incomingSender, blockedAddr)) {
                        isBlockedSender = true;
                        break;
                    }
                }
                if (isBlockedSender) {
                    c.previousFolderId = QStringLiteral("inbox");
                    c.folderId = QStringLiteral("spam");
                    c.folderTag = QStringLiteral("Spam");
                    c.imapSeq = 0;
                    c.isUnread = false;
                    m_readConvSigs.insert(sigKey);
                    c.id = QString("imap_%1_spam_%2").arg(c.accountId).arg(rawUidNum);
                    queueServerMove(rawUidNum, QStringLiteral("spam"), QStringLiteral("Spam"), true, false);
                }
            }

            // 2-B. 활성 필터 규칙(MailI18n::filterRules()) 자동 적용 — 새로 도착하거나 추가 페이지로 로드된 inbox 메일도 즉시 서버 및 로컬에서 지정 폴더로 이동
            if (c.folderId == QStringLiteral("inbox") && !c.messages.isEmpty()) {
                QString curAccEmail;
                for (const AccountItem &acc : m_accounts) {
                    if (acc.id == c.accountId) {
                        curAccEmail = acc.email.trimmed();
                        break;
                    }
                }
                const MessageItem &m = c.latestMessage();
                for (const MailFilterRule &r : MailI18n::filterRules()) {
                    if (!r.enabled || r.matchQuery.trimmed().isEmpty())
                        continue;
                    const QString rAccEmail = r.accountEmail.trimmed();
                    if (!rAccEmail.isEmpty() && rAccEmail.compare(QStringLiteral("ALL"), Qt::CaseInsensitive) != 0) {
                        if (curAccEmail.isEmpty() || curAccEmail.compare(rAccEmail, Qt::CaseInsensitive) != 0)
                            continue;
                    }
                    if (r.targetFolderId.startsWith(QStringLiteral("custom_acc_"))
                        && !r.targetFolderId.startsWith(QStringLiteral("custom_") + c.accountId + QStringLiteral("_"))) {
                        continue;
                    }
                    const QStringList queryTokens = r.matchQuery.split(QRegularExpression(QStringLiteral("[,;]")), Qt::SkipEmptyParts);
                    bool hit = false;
                    for (const QString &rawToken : queryTokens) {
                        const QString q = rawToken.trimmed();
                        if (q.isEmpty())
                            continue;
                        if (r.matchField == QStringLiteral("From")) {
                            hit = m.senderName.contains(q, Qt::CaseInsensitive) || m.senderEmail.contains(q, Qt::CaseInsensitive);
                        } else if (r.matchField == QStringLiteral("To")) {
                            hit = m.recipientSummary.contains(q, Qt::CaseInsensitive);
                        } else if (r.matchField == QStringLiteral("Subject")) {
                            hit = c.subject.contains(q, Qt::CaseInsensitive);
                        } else if (r.matchField == QStringLiteral("Body")) {
                            hit = m.snippet.contains(q, Qt::CaseInsensitive) || m.htmlBody.contains(q, Qt::CaseInsensitive);
                        }
                        if (hit)
                            break;
                    }
                    if (hit) {
                        QString resolvedName = r.targetFolderId;
                        for (const FolderItem &fd : m_folders) {
                            if (fd.id == r.targetFolderId && !fd.name.trimmed().isEmpty()) {
                                resolvedName = fd.name.trimmed();
                                break;
                            }
                        }
                        if (!r.targetFolderId.isEmpty() && c.folderId != r.targetFolderId) {
                            c.previousFolderId = c.folderId;
                            c.folderId = r.targetFolderId;
                            c.folderTag = MailI18n::folderTitle(r.targetFolderId, resolvedName);
                            c.imapSeq = 0;
                            c.id = QString("imap_%1_%2_%3").arg(c.accountId, r.targetFolderId).arg(rawUidNum);
                            if (r.targetFolderId == QStringLiteral("important")) {
                                c.isImportant = true;
                                m_importantConvSigs.insert(sigKey);
                                m_unimportantConvSigs.remove(sigKey);
                            }
                            if (r.targetFolderId == QStringLiteral("starred")) {
                                c.isStarred = true;
                                m_starredConvSigs.insert(sigKey);
                                m_unstarredConvSigs.remove(sigKey);
                            }
                            queueServerMove(rawUidNum, r.targetFolderId, resolvedName, r.markAsRead, r.markAsStarred);
                        }
                        if (r.markAsRead) {
                            c.isUnread = false;
                            m_readConvSigs.insert(sigKey);
                        }
                        if (r.markAsStarred) {
                            c.isStarred = true;
                            m_starredConvSigs.insert(sigKey);
                            m_unstarredConvSigs.remove(sigKey);
                        }
                        break;
                    }
                }
            }

            // 3. 개인정보 보호 허브 — 보낸 메일 수신 확인(MDN / Read Receipt) 응답 감지 시 보낸편지함 메일 상태 자동 갱신
            if (folderId == QStringLiteral("inbox") && !c.messages.isEmpty()) {
                const QString subjTrim = c.subject.trimmed();
                const QString bodySample = c.latestMessage().snippet + QStringLiteral(" ") + c.latestMessage().htmlBody.left(600);
                const bool isReadReceiptMail =
                    subjTrim.startsWith(QStringLiteral("읽음:"), Qt::CaseInsensitive)
                    || subjTrim.startsWith(QStringLiteral("읽음 -"), Qt::CaseInsensitive)
                    || subjTrim.startsWith(QStringLiteral("수신 확인:"), Qt::CaseInsensitive)
                    || subjTrim.startsWith(QStringLiteral("수신확인:"), Qt::CaseInsensitive)
                    || subjTrim.startsWith(QStringLiteral("Read:"), Qt::CaseInsensitive)
                    || subjTrim.startsWith(QStringLiteral("Read -"), Qt::CaseInsensitive)
                    || subjTrim.startsWith(QStringLiteral("Return Receipt"), Qt::CaseInsensitive)
                    || subjTrim.startsWith(QStringLiteral("Disposition Notification"), Qt::CaseInsensitive)
                    || bodySample.contains(QStringLiteral("disposition-notification"), Qt::CaseInsensitive);
                if (isReadReceiptMail) {
                    QString origSubj = subjTrim;
                    const int colonPos = origSubj.indexOf(':');
                    if (colonPos >= 0) {
                        origSubj = origSubj.mid(colonPos + 1).trimmed();
                    }
                    for (ConversationItem &sentEx : m_conversations) {
                        if (sentEx.accountId == c.accountId && sentEx.folderId == QStringLiteral("sent")) {
                            if ((!origSubj.isEmpty() && sentEx.subject.trimmed().compare(origSubj, Qt::CaseInsensitive) == 0)
                                || (!origSubj.isEmpty() && sentEx.subject.contains(origSubj, Qt::CaseInsensitive))) {
                                if (!sentEx.readReceiptConfirmed) {
                                    sentEx.readReceiptConfirmed = true;
                                    listStructureChanged = true;
                                }
                            }
                        }
                    }
                }
            }

            if (folderId == QStringLiteral("sent")) {
                if (prevRequestedSentSubjects.contains(c.subject.trimmed()))
                    c.readReceiptRequested = true;
                if (prevConfirmedSentSubjects.contains(c.subject.trimmed()))
                    c.readReceiptConfirmed = true;
            }

            bool dup = false;
            for (ConversationItem &ex : m_conversations) {
                // A. 로컬에서 방금 발송/작성한 항목(conv_local_*) 또는 예약 발송 대기 항목(!ex.scheduledSendIso.isEmpty())이
                //    IMAP 서버(Sent/Drafts) 동기화로 돌아온 경우 ID 승격, 예약 시각 표시 및 첨부 보존
                if ((ex.id.startsWith(QStringLiteral("conv_local_")) || !ex.scheduledSendIso.isEmpty())
                    && ex.accountId == c.accountId
                    && ex.folderId == c.folderId
                    && ex.subject == c.subject) {
                    if (m_selectedConvId == ex.id) {
                        m_selectedConvId = c.id;
                    }
                    ex.id = c.id;
                    ex.imapUid = c.imapUid;
                    ex.imapSeq = c.imapSeq;
                    if (!ex.isUnread || !c.isUnread) {
                        ex.isUnread = false;
                        m_readConvSigs.insert(sigKey);
                    }
                    ex.isStarred = ex.isStarred || c.isStarred;
                    ex.isImportant = ex.isImportant || c.isImportant;
                    ex.readReceiptRequested = ex.readReceiptRequested || c.readReceiptRequested;
                    ex.readReceiptConfirmed = ex.readReceiptConfirmed || c.readReceiptConfirmed;
                    if (!c.messages.isEmpty() && !ex.messages.isEmpty()) {
                        if (ex.scheduledSendIso.isEmpty()) {
                            if (!c.messages.last().fullTimeText.trimmed().isEmpty())
                                ex.messages.last().fullTimeText = c.messages.last().fullTimeText;
                            if (!c.messages.last().shortTimeText.trimmed().isEmpty())
                                ex.messages.last().shortTimeText = c.messages.last().shortTimeText;
                        }
                        if (ex.messages.last().attachments.isEmpty() && !c.messages.last().attachments.isEmpty()) {
                            ex.messages.last().attachments = c.messages.last().attachments;
                        }
                    }
                    dup = true;
                    break;
                }
                // B. 동일 폴더(ex.folderId == c.folderId) 내에서 동일한 고유 UID(ex.id == c.id 또는 ex.imapUid == c.imapUid)이거나,
                //    필터 규칙으로 이동되어 임시 UID를 갖고 있던 항목(ex.imapSeq == 0 등)이 실제 서버 폴더 동기화 항목과 지문(sigKey)이 일치하는 경우:
                //    중복 생성하지 않고 기존 항목(중요/별표/읽음 상태 및 이미 로드된 본문 포함)을 실제 폴더 UID·순번으로 승격한다.
                const int exSeqVal = ex.imapSeq > 0 ? ex.imapSeq : extractRawSeq(ex.id);
                if (ex.accountId == c.accountId && ex.folderId == c.folderId
                    && (ex.id == c.id
                        || (c.imapUid > 0 && ex.imapUid == c.imapUid)
                        || (conversationSignatureKey(ex) == sigKey
                            && (ex.imapSeq == 0
                                || c.imapSeq == 0
                                || !ex.previousFolderId.isEmpty()
                                || (isInitialPage && totalExistsCount > 0 && exSeqVal > totalExistsCount)
                                || !ex.id.startsWith(QStringLiteral("imap_")))))) {
                    if (c.imapSeq > 0 || ex.imapUid <= 0) {
                        ex.imapUid = c.imapUid;
                    }
                    if (c.imapSeq > 0) {
                        if (ex.imapSeq != c.imapSeq) {
                            ex.imapSeq = c.imapSeq;
                            listStructureChanged = true;
                        }
                        if (ex.previousFolderId == QStringLiteral("inbox")) {
                            ex.previousFolderId.clear();
                        }
                    }
                    if (!ex.isUnread || !c.isUnread || m_readConvSigs.contains(sigKey)) {
                        ex.isUnread = false;
                        m_readConvSigs.insert(sigKey);
                    } else {
                        ex.isUnread = c.isUnread;
                    }
                    if (m_unstarredConvSigs.contains(sigKey)) {
                        ex.isStarred = false;
                    } else {
                        ex.isStarred = ex.isStarred || c.isStarred || m_starredConvSigs.contains(sigKey) || (c.folderId == QStringLiteral("starred"));
                    }
                    if (m_unimportantConvSigs.contains(sigKey)) {
                        ex.isImportant = false;
                    } else {
                        ex.isImportant = ex.isImportant || c.isImportant || m_importantConvSigs.contains(sigKey) || (c.folderId == QStringLiteral("important"));
                    }
                    ex.isReplied = ex.isReplied || c.isReplied || m_repliedConvSigs.contains(sigKey);
                    ex.isForwarded = ex.isForwarded || c.isForwarded || m_forwardedConvSigs.contains(sigKey);
                    ex.readReceiptRequested = ex.readReceiptRequested || c.readReceiptRequested;
                    ex.readReceiptConfirmed = ex.readReceiptConfirmed || c.readReceiptConfirmed;

                    if (c.imapSeq > 0 && ex.id != c.id) {
                        if (m_selectedConvId == ex.id) {
                            m_selectedConvId = c.id;
                        }
                        ex.id = c.id;
                        listStructureChanged = true;
                    }

                    if (!c.messages.isEmpty() && !ex.messages.isEmpty() && !c.messages.last().attachments.isEmpty()) {
                        bool needAttUpdate = ex.messages.last().attachments.isEmpty();
                        if (!needAttUpdate) {
                            for (const AttachmentItem &ea : ex.messages.last().attachments) {
                                if (ea.filePath.isEmpty()) {
                                    needAttUpdate = true;
                                    break;
                                }
                            }
                        }
                        if (needAttUpdate) {
                            ex.messages.last().attachments = c.messages.last().attachments;
                            listStructureChanged = true;
                        }
                    }
                    dup = true;
                    break;
                }
            }
            // 기존에 다른 폴더나 동일 폴더에 있는 동일 지문(sigKey) 메일들과 읽음·별표·중요·답장·전달 플래그를 상호 동기화
            for (ConversationItem &ex : m_conversations) {
                if (ex.accountId == c.accountId && conversationSignatureKey(ex) == sigKey) {
                    if (m_unstarredConvSigs.contains(sigKey)) {
                        ex.isStarred = false;
                        c.isStarred = false;
                    } else if (m_starredConvSigs.contains(sigKey)
                               || c.isStarred || c.folderId == QStringLiteral("starred")
                               || ex.isStarred || ex.folderId == QStringLiteral("starred")) {
                        ex.isStarred = true;
                        c.isStarred = true;
                    }
                    if (m_unimportantConvSigs.contains(sigKey)) {
                        ex.isImportant = false;
                        c.isImportant = false;
                    } else if (m_importantConvSigs.contains(sigKey)
                               || c.isImportant || c.folderId == QStringLiteral("important")
                               || ex.isImportant || ex.folderId == QStringLiteral("important")) {
                        ex.isImportant = true;
                        c.isImportant = true;
                    }
                    if (c.isReplied || ex.isReplied || m_repliedConvSigs.contains(sigKey)) {
                        ex.isReplied = true;
                        c.isReplied = true;
                        m_repliedConvSigs.insert(sigKey);
                    }
                    if (c.isForwarded || ex.isForwarded || m_forwardedConvSigs.contains(sigKey)) {
                        ex.isForwarded = true;
                        c.isForwarded = true;
                        m_forwardedConvSigs.insert(sigKey);
                    }
                    if (!ex.isUnread || !c.isUnread || m_readConvSigs.contains(sigKey)) {
                        ex.isUnread = false;
                        c.isUnread = false;
                        m_readConvSigs.insert(sigKey);
                    }
                }
            }
            if (!dup) {
                int newRealIdx = m_conversations.size();
                m_conversations.append(c);
                listStructureChanged = true;
                if ((c.folderId == QStringLiteral("inbox") || c.previousFolderId == QStringLiteral("inbox"))
                    && c.isUnread && !m_notifiedConvIds.contains(sigKey)) {
                    m_notifiedConvIds.insert(sigKey);
                    if (m_initialSyncCompleted && isInitialPage) {
                        brandNewUnreadInboxArrivals.append(c);
                    }
                }
                bool matchAccount = (m_selectedAccountId == "acc_unified" || c.accountId == m_selectedAccountId);
                bool matchFolder = false;
                if (m_selectedFolderId == "starred") {
                    matchFolder = ((c.isStarred || c.folderId == "starred") && c.folderId != "trash" && c.folderId != "spam");
                } else if (m_selectedFolderId == "important") {
                    matchFolder = ((c.isImportant || c.folderId == "important") && c.folderId != "trash" && c.folderId != "spam");
                } else {
                    matchFolder = (c.folderId == m_selectedFolderId);
                }
                bool matchUnread = (!m_unreadOnlyFilter || c.isUnread);
                if (matchAccount && matchFolder && matchUnread) {
                    newlyAddedIndices.append(newRealIdx);
                }
            }
        }

        if (!pendingServerMoves.isEmpty()) {
            for (const AccountItem &acc : m_accounts) {
                if (acc.id == accountId) {
                    for (auto it = pendingServerMoves.begin(); it != pendingServerMoves.end(); ++it) {
                        const PendingServerMoveBatch &b = it.value();
                        m_googleProvider->moveImapMessagesToFolder(
                            acc, folderId, b.seqNums, b.targetFolderId, b.targetDisplayName, b.markRead, b.markStar);
                    }
                    break;
                }
            }
        }

        // 새로 도착한 안 읽은 받은편지함(Inbox) 메일이 감지된 경우 알림음 + 데스크톱 배너 알림 표시
        if (!brandNewUnreadInboxArrivals.isEmpty()) {
            if (MailI18n::enableNotificationSound()) {
                QApplication::beep();
            }
            if (MailI18n::enableDesktopNotification() && m_trayIcon) {
                const ConversationItem &latestNew = brandNewUnreadInboxArrivals.first();
                setProperty("lastNotifiedConvId", latestNew.id);
                setProperty("lastNotifiedAccountId", latestNew.accountId);
                setProperty("lastNotifiedFolderId", latestNew.folderId.isEmpty() ? QStringLiteral("inbox") : latestNew.folderId);
                setProperty("lastNotifiedConvSig", conversationSignatureKey(latestNew));
                const QString senderStr = latestNew.messages.isEmpty() ? latestNew.accountId : latestNew.latestMessage().senderName;
                const QString titleStr = (brandNewUnreadInboxArrivals.size() == 1)
                    ? MailI18n::t(QString("📩 새 메일 도착 — %1").arg(senderStr),
                                  QString("📩 New Email — %1").arg(senderStr),
                                  QString("📩 新着メール — %1").arg(senderStr))
                    : MailI18n::t(QString("📩 새 메일 %1통 도착 (%2 외)").arg(brandNewUnreadInboxArrivals.size()).arg(senderStr),
                                  QString("📩 %1 New Emails (%2)").arg(brandNewUnreadInboxArrivals.size()).arg(senderStr),
                                  QString("📩 %1件の新着メール").arg(brandNewUnreadInboxArrivals.size()));
                m_trayIcon->showMessage(titleStr, latestNew.subject, MailIcons::sollunaAppIcon(brandNewUnreadInboxArrivals.size()), 6000);
            }
        }
        updateAppUnreadBadgeIcon();
        refreshFolderSelectionStyles();

        // 실제 메모리에 로드된 해당 폴더 메일들의 최소 IMAP 시퀀스 번호를 기준으로 lowestSeq 갱신
        int actualMinSeq = lowestLoadedImapSeqForFolder(accountId, folderId);
        if (actualMinSeq > 0) {
            if (!pageConvs.isEmpty() && lowestFetchedSeq > 0)
                m_folderLowestSeq[folderKey] = qMin(actualMinSeq, lowestFetchedSeq);
            else
                m_folderLowestSeq[folderKey] = actualMinSeq;
        } else if (lowestFetchedSeq > 0) {
            m_folderLowestSeq[folderKey] = lowestFetchedSeq;
        }

        if (reconcileSentReadReceipts(m_conversations)) {
            listStructureChanged = true;
        }
        harvestContactsFromConversations(pageConvs);
        saveConversationsCacheToDisk();

        // 현재 사용자가 보고 있는 계정·폴더가 아니면 화면 리스트(m_convListWidget)를 절대 건드리지 않음!
        const bool isCurrentAccount = (m_selectedAccountId == "acc_unified" || accountId == m_selectedAccountId);
        const bool isCurrentFolder = (folderId == m_selectedFolderId
                                      || m_selectedFolderId == "starred"
                                      || m_selectedFolderId == "important");
        if (!isCurrentAccount || !isCurrentFolder) {
            return;
        }

        // 사용자가 페이지 이동 버튼을 연속으로 눌러 현재 선택된 페이지(m_currentMailPage)가
        // 방금 도착한 이전 페이지 응답과 다른 경우, 현재 페이지 구간[curPageStartSeq .. curPageEndSeq]이
        // 아직 메모리에 없으면 즉시 해당 페이지 구간을 서버에 요청하고 로딩 상태를 유지한다.
        const int serverTotal = m_folderTotalExists.value(folderKey, totalExistsCount);
        if (m_currentMailPage > 1 && serverTotal > 0 && folderId == m_selectedFolderId) {
            const int kPageSize = 20;
            const int curPageEndSeq = serverTotal - (m_currentMailPage - 1) * kPageSize;
            const int curPageStartSeq = qMax(1, curPageEndSeq - kPageSize + 1);
            if (curPageEndSeq >= 1) {
                int loadedInCurPage = 0;
                const QString prefix = QString("imap_%1_%2_").arg(accountId, folderId);
                for (const ConversationItem &c : m_conversations) {
                    if (c.accountId == accountId && c.id.startsWith(prefix)) {
                        bool ok = true;
                        const int seq = (c.imapSeq > 0) ? c.imapSeq : c.id.mid(prefix.size()).toInt(&ok);
                        if (ok && seq >= curPageStartSeq && seq <= curPageEndSeq) {
                            ++loadedInCurPage;
                        }
                    }
                }
                if (loadedInCurPage == 0) {
                    m_folderLoadingMore.insert(folderKey);
                    if (m_activePageFetchEndSeq.value(folderKey, -1) != curPageEndSeq) {
                        m_activePageFetchEndSeq[folderKey] = curPageEndSeq;
                        for (const AccountItem &acc : m_accounts) {
                            if (acc.id == accountId) {
                                m_googleProvider->fetchImapFolder(acc, folderId, kPageSize, curPageEndSeq);
                                break;
                            }
                        }
                    }
                } else {
                    m_activePageFetchEndSeq.remove(folderKey);
                }
            }
        } else {
            m_activePageFetchEndSeq.remove(folderKey);
        }

        if (m_filteredConvIndices.isEmpty() || listStructureChanged || !newlyAddedIndices.isEmpty() || !isInitialPage) {
            // 현재 선택된 페이지(m_currentMailPage, 1000개 단위) 기준으로 메일 목록을 갱신하고 페이지네이션 바 업데이트
            QScrollBar *sb = m_convListWidget ? m_convListWidget->verticalScrollBar() : nullptr;
            const int savedScrollPos = (isInitialPage && m_currentMailPage == 1 && sb) ? sb->value() : 0;
            refreshConversationList();
            if (sb && savedScrollPos > 0) {
                QSignalBlocker sbBlocker(sb);
                sb->setValue(savedScrollPos);
            }
        } else {
            // 이미 현재 페이지 항목이 모두 로드되어 있는 경우 — 스크롤/선택 건드리지 않고 페이지네이션 바 및 화면만 갱신
            appendMoreConversationRows(0);
            updateMailPaginationBar();
            if (m_convListWidget && m_convListWidget->viewport()) {
                m_convListWidget->viewport()->update();
            }
            if (!m_selectedConvId.isEmpty()) {
                for (const ConversationItem &c : m_conversations) {
                    if (c.id == m_selectedConvId) {
                        refreshReadingAttachmentsBar(c);
                        break;
                    }
                }
            }
        }

        // 창 제목에 현재 페이지(1000개 단위) 표시 건수와 전체 건수 안내
        QString accEmail = accountId;
        for (const AccountItem &a : m_accounts) {
            if (a.id == accountId) {
                accEmail = a.email;
                break;
            }
        }
        if (serverTotal > 0) {
            setWindowTitle(
                MailI18n::t(
                    QString("SollunaMail — [%1] '%2' %3페이지 (%4건 표시 / 전체 %5건)"),
                    QString("SollunaMail — [%1] '%2' Page %3 (%4 shown / %5 total)"),
                    QString("SollunaMail — [%1] '%2' %3ページ (%4件表示 / 全%5件)"))
                    .arg(accEmail, MailI18n::folderTitle(folderId, folderId))
                    .arg(m_currentMailPage)
                    .arg(m_filteredConvIndices.size())
                    .arg(serverTotal));
        }
    });

    connect(m_googleProvider, &GoogleProvider::imapFolderConversationsFetched, this,
            [this](const QString &accountId, const QString &folderId, const QList<ConversationItem> &newConvs) {
        Q_UNUSED(accountId);
        Q_UNUSED(folderId);
        harvestContactsFromConversations(newConvs);
    });

    connect(m_googleProvider, &GoogleProvider::googleCalendarsFetched, this,
            [this](const QString &accountEmail, const QList<CalendarSubItem> &calendars, const QList<CalendarEventItem> &events) {
        const QString upperEmail = accountEmail.toUpper();
        const bool isWebcalSingleUpdate = (calendars.size() == 1 && calendars.first().id.startsWith("webcal_"));
        if (isWebcalSingleUpdate) {
            const QString targetCalId = calendars.first().id;
            bool updatedSub = false;
            for (int i = 0; i < m_calendars.size(); ++i) {
                if (m_calendars[i].id == targetCalId) {
                    const bool prevVis = m_calendars[i].isVisible;
                    m_calendars[i] = calendars.first();
                    m_calendars[i].isVisible = prevVis;
                    updatedSub = true;
                    break;
                }
            }
            if (!updatedSub) {
                m_calendars.append(calendars.first());
            }
            for (int i = m_calendarEvents.size() - 1; i >= 0; --i) {
                if (m_calendarEvents[i].calendarId == targetCalId) {
                    m_calendarEvents.removeAt(i);
                }
            }
            for (const CalendarEventItem &ev : events) {
                m_calendarEvents.append(ev);
            }
        } else {
            if (!calendars.isEmpty()) {
                for (int i = m_calendars.size() - 1; i >= 0; --i) {
                    if (m_calendars[i].accountEmail.compare(upperEmail, Qt::CaseInsensitive) == 0
                        && !m_calendars[i].id.startsWith("cal_local_", Qt::CaseInsensitive)
                        && !m_calendars[i].id.startsWith("webcal_", Qt::CaseInsensitive)) {
                        m_calendars.removeAt(i);
                    }
                }
                for (const CalendarSubItem &sub : calendars) {
                    m_calendars.append(sub);
                }
            }
            // 사용자가 직접 추가/수정한 로컬 일정(local_ev_*) 및 webcal 구독 일정은 유지하고 원격 일정만 최신화
            for (int i = m_calendarEvents.size() - 1; i >= 0; --i) {
                if (m_calendarEvents[i].accountEmail.compare(upperEmail, Qt::CaseInsensitive) == 0
                    && !m_calendarEvents[i].id.startsWith("local_ev_")
                    && !m_calendarEvents[i].calendarId.startsWith("webcal_")) {
                    m_calendarEvents.removeAt(i);
                }
            }
            for (const CalendarEventItem &ev : events) {
                m_calendarEvents.append(ev);
            }
        }
        saveCalendarAndContactsCacheToDisk();
        rebuildCalendarTree();
        refreshCalendarViews();
    });

    connect(m_googleProvider, &GoogleProvider::googleContactsFetched, this,
            [this](const QString &accountEmail, const QList<ContactItem> &contacts) {
        const QString upperEmail = accountEmail.toUpper();
        // 기존 연락처에 설정된 사용자 정의 소속 그룹(groupName) 보존용 맵
        QMap<QString, QString> preservedGroupsByEmail;
        for (const ContactItem &existing : m_contacts) {
            const QString emKey = existing.email.trimmed().toLower();
            const QString grp = existing.groupName.trimmed();
            if (!emKey.isEmpty() && !grp.isEmpty() && grp.compare(existing.accountEmail.trimmed(), Qt::CaseInsensitive) != 0) {
                preservedGroupsByEmail.insert(emKey, grp);
            }
        }
        // 사용자가 직접 추가한 연락처(local_ct_*)나 메일에서 수집한 연락처(mail_ct_*)는 보존하면서 병합
        for (int i = m_contacts.size() - 1; i >= 0; --i) {
            if (m_contacts[i].accountEmail.compare(upperEmail, Qt::CaseInsensitive) == 0
                && !m_contacts[i].id.startsWith("local_ct_")
                && !m_contacts[i].id.startsWith("mail_ct_")) {
                m_contacts.removeAt(i);
            }
        }
        for (const ContactItem &c : contacts) {
            bool duplicate = false;
            for (ContactItem &existing : m_contacts) {
                if (!c.email.isEmpty() && existing.email.compare(c.email, Qt::CaseInsensitive) == 0) {
                    if (existing.displayName.isEmpty() || existing.displayName == existing.email)
                        existing.displayName = c.displayName;
                    if (existing.phone.isEmpty() && !c.phone.isEmpty())
                        existing.phone = c.phone;
                    if (existing.company.isEmpty() && !c.company.isEmpty())
                        existing.company = c.company;
                    duplicate = true;
                    break;
                }
            }
            if (!duplicate) {
                ContactItem merged = c;
                const QString emKey = merged.email.trimmed().toLower();
                if (!emKey.isEmpty() && preservedGroupsByEmail.contains(emKey)) {
                    merged.groupName = preservedGroupsByEmail.value(emKey);
                }
                m_contacts.append(merged);
            }
        }
        saveCalendarAndContactsCacheToDisk();
        rebuildContactGroupsList();
        refreshContactsList();
    });

    connect(m_googleProvider, &GoogleProvider::smtpSendFinished, this,
            [this](bool ok, const QString &msg) {
        setWindowTitle(QString("SollunaMail — %1").arg(msg));
        if (statusBar()) {
            statusBar()->showMessage(msg, ok ? 5000 : 8000);
        }
        if (!ok) {
            QMessageBox::warning(
                this,
                MailI18n::t("⚠️ 메일 발송 실패 (SMTP Send Failed)",
                            "⚠️ SMTP Send Failed",
                            "⚠️ メール送信失敗"),
                msg);
        }
    });

    QShortcut *minimalShortcut = new QShortcut(QKeySequence("Ctrl+Shift+F"), this);
    connect(minimalShortcut, &QShortcut::activated, this, &MainWindow::onToggleMinimalLayout);

    loadConversationsCacheFromDisk();
    for (const ConversationItem &c : m_conversations) {
        m_notifiedConvIds.insert(c.id);
    }
    loadCalendarAndContactsCacheFromDisk();
    pruneDeletedAccountsData();
    rebuildLeftBarAccounts();
    rebuildCustomFoldersList();
    rebuildCalendarTree();
    refreshFolderSelectionStyles();
    refreshConversationList();
    refreshCalendarViews();
    refreshContactsList();
    setupSystemTrayAndAutoPoll();
    updateAppUnreadBadgeIcon();

    // 초기 동기화 완료 후 도착하는 메일부터 새 메일 데스크톱 팝업 알림 작동
    QTimer::singleShot(10000, this, [this]() {
        m_initialSyncCompleted = true;
    });

    // 저장된 계정 중 비밀번호/토큰이 설정된 계정은 시작 시 현재 선택된 편지함(받은편지함)과 활성 필터 규칙 대상 폴더를 동기화 수행
    bool anyImapSyncStarted = false;
    const QString initialFolder = (m_selectedFolderId == QStringLiteral("snoozed") || m_selectedFolderId.isEmpty())
                                      ? QStringLiteral("inbox")
                                      : m_selectedFolderId;
    for (const AccountItem &acc : m_accounts) {
        if (acc.provider != AccountProviderType::Unified) {
            m_googleProvider->fetchAccountAvatar(acc);
            if (!acc.appPassword.isEmpty() || !acc.oauthAccessToken.isEmpty()) {
                if (m_selectedAccountId == QStringLiteral("acc_unified") || acc.id == m_selectedAccountId) {
                    anyImapSyncStarted = true;
                    m_folderLoadingMore.insert(acc.id + QStringLiteral("|") + initialFolder);
                    m_googleProvider->fetchImapFolder(acc, initialFolder, 200, 0);
                    // 필터 규칙에 의해 메일이 자동 이동되는 대상 내 폴더(custom_*)도 시작 시 함께 1페이지 동기화하여
                    // 중요편지함 및 이동된 폴더의 안 읽음 배지·목록이 즉시 연동되도록 보장
                    if (!acc.isPop3()) {
                        QSet<QString> syncedTargets;
                        syncedTargets.insert(initialFolder);
                        for (const MailFilterRule &r : MailI18n::filterRules()) {
                            if (!r.enabled || r.targetFolderId.isEmpty() || syncedTargets.contains(r.targetFolderId))
                                continue;
                            if (!r.targetFolderId.startsWith(QStringLiteral("custom_")))
                                continue;
                            if (r.targetFolderId.startsWith(QStringLiteral("custom_acc_"))
                                && !r.targetFolderId.startsWith(QStringLiteral("custom_") + acc.id + QStringLiteral("_"))) {
                                continue;
                            }
                            syncedTargets.insert(r.targetFolderId);
                            m_folderLoadingMore.insert(acc.id + QStringLiteral("|") + r.targetFolderId);
                            m_googleProvider->fetchImapFolder(acc, r.targetFolderId, 200, 0);
                        }
                    }
                }
            }
            if (!acc.oauthAccessToken.isEmpty() || !acc.oauthRefreshToken.isEmpty()) {
                m_googleProvider->syncGoogleCalendarEvents(acc.email, acc.oauthAccessToken, acc.oauthRefreshToken);
                m_googleProvider->syncGoogleContacts(acc.email, acc.oauthAccessToken, acc.oauthRefreshToken);
            }
            const bool isICloudAcc = (acc.provider == AccountProviderType::ICloud)
                                     || acc.imapHost.contains("mail.me.com", Qt::CaseInsensitive)
                                     || acc.imapHost.contains("icloud.com", Qt::CaseInsensitive)
                                     || acc.email.endsWith("@icloud.com", Qt::CaseInsensitive)
                                     || acc.email.endsWith("@me.com", Qt::CaseInsensitive)
                                     || acc.email.endsWith("@mac.com", Qt::CaseInsensitive);
            if (isICloudAcc && !acc.appPassword.trimmed().isEmpty()) {
                m_googleProvider->syncICloudCalDavCalendars(acc.email, acc.appPassword);
            }
        }
    }
    for (const CalendarSubItem &sub : m_calendars) {
        if (!sub.webcalUrl.trimmed().isEmpty()) {
            m_googleProvider->fetchWebcalIcsSubscription(sub.accountEmail, sub.name, sub.webcalUrl, sub.color);
        }
    }
    if (anyImapSyncStarted) {
        QTimer::singleShot(150, this, [this]() {
            showMailLoadingToast(QString(), 2000);
        });
    }
}

MainWindow::~MainWindow()
{
    if (m_autoSyncTimer)
        m_autoSyncTimer->stop();
    if (m_scheduledSendTimer)
        m_scheduledSendTimer->stop();
    if (m_mailLoadingToastTimer)
        m_mailLoadingToastTimer->stop();
    if (m_googleProvider) {
        m_googleProvider->disconnect(this);
        m_googleProvider->stopAllImapIdleWatchers();
    }
#ifdef SOLLUNA_HAS_WEBENGINE
    if (m_readingWebView) {
        if (QWebEnginePage *wp = m_readingWebView->page()) {
            wp->setUrlRequestInterceptor(nullptr);
        }
        delete m_readingWebView;
        m_readingWebView = nullptr;
    }
#endif
    delete ui;
}

bool MainWindow::eventFilter(QObject *watched, QEvent *event)
{
    if (m_convListWidget && (watched == m_convListWidget || watched == m_convListWidget->viewport())) {
        if (watched == m_convListWidget->viewport()
            && (event->type() == QEvent::MouseButtonPress
                || event->type() == QEvent::MouseButtonRelease
                || event->type() == QEvent::MouseButtonDblClick)) {
            QMouseEvent *me = static_cast<QMouseEvent *>(event);
            const QPoint pos = me->pos();
            QListWidgetItem *clickedItem = m_convListWidget->itemAt(pos);
            if (clickedItem) {
                const int realIdx = clickedItem->data(Qt::UserRole).toInt();
                if (realIdx >= 0 && realIdx < m_conversations.size()) {
                    // 1. 좌측 체크박스 영역(x <= 30px) 클릭 시:
                    //    체크 상태만 토글하고 이벤트를 즉시 소비(return true)하여
                    //    선택 행이 바뀌거나 메일 본문을 불러오는 동작을 100% 차단!
                    if (pos.x() <= 30) {
                        if (event->type() == QEvent::MouseButtonPress && me->button() == Qt::LeftButton) {
                            const QString cid = m_conversations[realIdx].id;
                            if (m_checkedConvIds.contains(cid)) {
                                m_checkedConvIds.remove(cid);
                            } else {
                                m_checkedConvIds.insert(cid);
                            }
                            if (m_convListWidget->viewport()) {
                                m_convListWidget->viewport()->update();
                            }
                            statusBar()->showMessage(
                                MailI18n::t(QString("☑️ 체크박스 선택: %1개 메일 선택됨 (확장메뉴 ⋮에서 읽음 표시 / 편지함 이동 / 휴지통 이동 가능)")
                                                .arg(m_checkedConvIds.size()),
                                            QString("☑️ %1 message(s) checked (Use ⋮ menu to mark read / move to folder / move to Trash)").arg(m_checkedConvIds.size()),
                                            QString("☑️ %1件のメールを選択中 (⋮メニューで既読 / フォルダ移動 / ゴミ箱移動が可能)").arg(m_checkedConvIds.size())),
                                4000);
                        }
                        return true;
                    }

                    // 2. 메일 제목/카드 영역(x > 30px)을 마우스 좌클릭한 경우에만:
                    //    사용자가 명시적으로 메일 제목을 클릭한 것으로 기록하고 본문을 로드하여 표시!
                    if (event->type() == QEvent::MouseButtonPress && me->button() == Qt::LeftButton) {
                        const QString cid = m_conversations[realIdx].id;
                        m_userExplicitlyOpenedConvIds.insert(cid);
                        const int row = m_convListWidget->row(clickedItem);
                        if (row >= 0 && row == m_convListWidget->currentRow()) {
                            displayConversation(row, true);
                        }
                    }
                }
            }
        }
    }
    return QMainWindow::eventFilter(watched, event);
}

void MainWindow::showTransientTrayToast(const QString &title, const QString &message, int durationMs)
{
    QFrame *toast = new QFrame(nullptr,
                               Qt::Tool
                                   | Qt::FramelessWindowHint
                                   | Qt::WindowStaysOnTopHint
                                   | Qt::WindowDoesNotAcceptFocus);
    toast->setAttribute(Qt::WA_DeleteOnClose, true);
    toast->setAttribute(Qt::WA_ShowWithoutActivating, true);
    toast->setObjectName("sollunaTrayToast");
    toast->setFixedWidth(368);
    toast->setStyleSheet(
        "QFrame#sollunaTrayToast {"
        "  background-color: #FFFFFF;"
        "  border: 1px solid #CBD5E1;"
        "  border-radius: 10px;"
        "}");

    QHBoxLayout *h = new QHBoxLayout(toast);
    h->setContentsMargins(14, 12, 12, 12);
    h->setSpacing(12);

    QLabel *iconLbl = new QLabel(toast);
    iconLbl->setFixedSize(40, 40);
    iconLbl->setPixmap(MailIcons::sollunaAppIcon(0).pixmap(40, 40));
    iconLbl->setStyleSheet("border: none; background: transparent;");
    h->addWidget(iconLbl, 0, Qt::AlignTop);

    QVBoxLayout *v = new QVBoxLayout();
    v->setContentsMargins(0, 0, 0, 0);
    v->setSpacing(4);

    QHBoxLayout *topRow = new QHBoxLayout();
    topRow->setContentsMargins(0, 0, 0, 0);
    QLabel *titleLbl = new QLabel(title, toast);
    titleLbl->setStyleSheet("font-size: 13px; font-weight: 800; color: #0F172A; border: none; background: transparent;");
    topRow->addWidget(titleLbl, 1);

    QToolButton *closeBtn = new QToolButton(toast);
    closeBtn->setText(QStringLiteral("✕"));
    closeBtn->setFixedSize(20, 20);
    closeBtn->setCursor(Qt::PointingHandCursor);
    closeBtn->setStyleSheet(
        "QToolButton { border: none; background: transparent; color: #64748B; font-size: 12px; font-weight: 700; border-radius: 4px; }"
        "QToolButton:hover { background: #F1F5F9; color: #0F172A; }");
    connect(closeBtn, &QToolButton::clicked, toast, &QWidget::close);
    topRow->addWidget(closeBtn, 0, Qt::AlignTop);
    v->addLayout(topRow);

    QLabel *msgLbl = new QLabel(message, toast);
    msgLbl->setWordWrap(true);
    msgLbl->setStyleSheet("font-size: 11.5px; color: #334155; line-height: 1.4; border: none; background: transparent;");
    v->addWidget(msgLbl);
    h->addLayout(v, 1);

    toast->adjustSize();
    if (QScreen *scr = QGuiApplication::primaryScreen()) {
        const QRect avail = scr->availableGeometry();
        const int x = avail.right() - toast->width() - 16;
#if defined(Q_OS_MACOS)
        // macOS는 상단 메뉴바 우측에 트레이 아이콘이 위치하므로 우측 상단에 배치
        const int y = avail.top() + 16;
#else
        // Windows 및 Linux 데스크톱은 우측 하단 작업표시줄 트레이 바로 위에 배치
        const int y = avail.bottom() - toast->height() - 16;
#endif
        toast->move(qMax(avail.left() + 8, x), qMax(avail.top() + 8, y));
    }
    toast->show();
    QTimer::singleShot(qMax(500, durationMs), toast, &QWidget::close);
}

void MainWindow::showMailLoadingToast(const QString &message, int durationMs)
{
    if (!m_mailLoadingToast) {
        m_mailLoadingToast = new QFrame(this);
        m_mailLoadingToast->setObjectName("sollunaMailLoadingToast");
        m_mailLoadingToast->setAttribute(Qt::WA_TransparentForMouseEvents, true);
        m_mailLoadingToast->setStyleSheet(
            "QFrame#sollunaMailLoadingToast {"
            "  background-color: rgba(15, 23, 42, 0.93);"
            "  border: 1.5px solid #3B82F6;"
            "  border-radius: 10px;"
            "}");
        QHBoxLayout *h = new QHBoxLayout(m_mailLoadingToast);
        h->setContentsMargins(20, 11, 20, 11);
        h->setSpacing(10);

        m_mailLoadingToastLbl = new QLabel(m_mailLoadingToast);
        m_mailLoadingToastLbl->setStyleSheet(
            "font-size: 13.5px; font-weight: 700; color: #FFFFFF; background: transparent; border: none;");
        h->addWidget(m_mailLoadingToastLbl);
    }

    if (!m_mailLoadingToastTimer) {
        m_mailLoadingToastTimer = new QTimer(this);
        m_mailLoadingToastTimer->setSingleShot(true);
        connect(m_mailLoadingToastTimer, &QTimer::timeout, this, [this]() {
            if (m_mailLoadingToast)
                m_mailLoadingToast->hide();
        });
    }

    const QString text = message.trimmed().isEmpty()
        ? MailI18n::t("⏳ 메일 로드중입니다.", "⏳ Loading messages...", "⏳ メールを読み込んでいます...")
        : message;
    m_mailLoadingToastLbl->setText(text);
    m_mailLoadingToast->adjustSize();

    const int x = qMax(16, (width() - m_mailLoadingToast->width()) / 2);
    const int y = 64;
    m_mailLoadingToast->move(x, y);
    m_mailLoadingToast->raise();
    m_mailLoadingToast->show();
    m_mailLoadingToast->repaint();

    m_mailLoadingToastTimer->start(qMax(2000, durationMs));
}

void MainWindow::closeEvent(QCloseEvent *event)
{
    if (!m_forceQuit && MailI18n::closeToTrayOnX() && m_trayIcon && QSystemTrayIcon::isSystemTrayAvailable()) {
        saveConversationsCacheToDisk();
        event->ignore();
        hide();
        if (!m_trayMinimizeNoticeShown && MailI18n::enableDesktopNotification()) {
            m_trayMinimizeNoticeShown = true;
            showTransientTrayToast(
                MailI18n::t("SollunaMail 트레이 실행 안내",
                            "SollunaMail Running in Tray",
                            "SollunaMail トレイ実行"),
                MailI18n::t("창을 닫아도 SollunaMail이 시스템 트레이에서 새 메일을 계속 확인합니다.\n완전히 종료하려면 트레이 아이콘을 우클릭하여 [🚪 종료]를 선택하세요.",
                            "SollunaMail is still running in the system tray. Right-click the tray icon and select [Quit] to exit completely.",
                            "システムトレイで実行中です。トレイメニューの[終了]で完全終了できます。"),
                2000);
        }
        return;
    }

    QStringList scheduledItemsHtml;
    int totalScheduledCount = 0;
    for (const ConversationItem &c : m_conversations) {
        if (c.scheduledSendIso.isEmpty() || c.scheduledSendIso.startsWith(QStringLiteral("EXPIRED:")))
            continue;
        ++totalScheduledCount;
        if (scheduledItemsHtml.size() < 5) {
            const QDateTime dt = QDateTime::fromString(c.scheduledSendIso, Qt::ISODate);
            const QString timeStr = dt.isValid() ? dt.toString("yyyy-MM-dd AP h:mm") : c.scheduledSendIso;
            scheduledItemsHtml.append(
                QString("• <b>%1</b> <span style='color:#1D4ED8;'>(%2)</span>")
                    .arg(c.subject.toHtmlEscaped(), timeStr.toHtmlEscaped()));
        }
    }
    if (totalScheduledCount > 5) {
        scheduledItemsHtml.append(
            MailI18n::t(QString("… 외 %1건").arg(totalScheduledCount - 5),
                        QString("… and %1 more").arg(totalScheduledCount - 5),
                        QString("… 他 %1件").arg(totalScheduledCount - 5)));
    }

    if (totalScheduledCount > 0) {
        if (isHidden() || isMinimized()) {
            setWindowState((windowState() & ~Qt::WindowMinimized) | Qt::WindowActive);
            showNormal();
            raise();
            activateWindow();
        }
        QMessageBox msgBox(this);
        msgBox.setWindowTitle(MailI18n::t("⏰ 예약 발송 메일 확인 — SollunaMail",
                                          "⏰ Scheduled Emails Pending — SollunaMail",
                                          "⏰ 予約送信メールの確認 — SollunaMail"));
        msgBox.setIcon(QMessageBox::Question);
        msgBox.setTextFormat(Qt::RichText);
        msgBox.setText(MailI18n::t(
            QString("<b>⏰ 현재 예약 발송 대기 중인 메일이 %1건 있습니다.</b><br/><br/>"
                    "%2<br/><br/>"
                    "• 예약 스케줄은 디스크에 자동 저장되어, <b>예약 시각 전에 프로그램을 다시 실행</b>하시면 정상적으로 예약 발송됩니다.<br/>"
                    "• 단, 예약 지정 시각에 프로그램이 꺼져 있으면 해당 시각에 발송되지 않습니다.<br/><br/>"
                    "프로그램을 종료하시겠습니까?")
                .arg(totalScheduledCount)
                .arg(scheduledItemsHtml.join("<br/>")),
            QString("<b>⏰ You have %1 scheduled email(s) waiting to be sent.</b><br/><br/>"
                    "%2<br/><br/>"
                    "• Your schedule is automatically saved to disk and will be sent if you restart SollunaMail before the scheduled time.<br/>"
                    "• However, if the program is closed at the scheduled time, it cannot be sent at that moment.<br/><br/>"
                    "Do you want to exit SollunaMail?")
                .arg(totalScheduledCount)
                .arg(scheduledItemsHtml.join("<br/>")),
            QString("<b>⏰ 現在、予約送信待ちのメールが %1件 あります。</b><br/><br/>"
                    "%2<br/><br/>"
                    "• 予約スケジュールはディスクに自動保存され、予約時刻前にプログラムを再起動すれば正常に予約送信されます。<br/>"
                    "• ただし、予約時刻にプログラムが終了している場合は送信されません。<br/><br/>"
                    "プログラムを終了しますか？")
                .arg(totalScheduledCount)
                .arg(scheduledItemsHtml.join("<br/>"))));

        QPushButton *btnCancel = msgBox.addButton(
            MailI18n::t("종료 취소 (계속 실행)", "Cancel (Keep Running)", "キャンセル（実行継続）"),
            QMessageBox::RejectRole);
        QPushButton *btnTray = nullptr;
        if (m_trayIcon && QSystemTrayIcon::isSystemTrayAvailable()) {
            btnTray = msgBox.addButton(
                MailI18n::t("트레이로 숨기기 (백그라운드 유지)", "Minimize to Tray", "トレイに格納"),
                QMessageBox::ActionRole);
        }
        QPushButton *btnQuit = msgBox.addButton(
            MailI18n::t("예약 저장 후 종료", "Save Schedule & Quit", "予約を保存して終了"),
            QMessageBox::AcceptRole);
        msgBox.setDefaultButton(btnCancel);
        msgBox.exec();

        if (msgBox.clickedButton() == btnCancel || msgBox.clickedButton() == nullptr) {
            m_forceQuit = false;
            event->ignore();
            return;
        }
        if (btnTray && msgBox.clickedButton() == btnTray) {
            m_forceQuit = false;
            saveConversationsCacheToDisk();
            event->ignore();
            hide();
            return;
        }
        if (msgBox.clickedButton() == btnQuit) {
            m_forceQuit = true;
        }
    }

    if (m_autoSyncTimer)
        m_autoSyncTimer->stop();
    if (m_scheduledSendTimer)
        m_scheduledSendTimer->stop();
    if (m_mailLoadingToastTimer)
        m_mailLoadingToastTimer->stop();
    if (m_googleProvider) {
        m_googleProvider->disconnect(this);
        m_googleProvider->stopAllImapIdleWatchers();
    }
#ifdef SOLLUNA_HAS_WEBENGINE
    if (m_readingWebView) {
        if (QWebEnginePage *wp = m_readingWebView->page()) {
            wp->setUrlRequestInterceptor(nullptr);
        }
    }
#endif

    saveConversationsCacheToDisk();
    saveCalendarAndContactsCacheToDisk();
    saveAccountsToDisk();
    if (m_trayIcon) {
        m_trayIcon->hide();
    }
    event->accept();
    QCoreApplication::quit();
}

void MainWindow::changeEvent(QEvent *event)
{
    QMainWindow::changeEvent(event);
    if (event && event->type() == QEvent::WindowStateChange) {
        if (isMinimized() && MailI18n::minimizeToTrayOnMinimize() && m_trayIcon && QSystemTrayIcon::isSystemTrayAvailable()) {
            QTimer::singleShot(0, this, &QWidget::hide);
        }
    }
}

void MainWindow::initializePreloadedData()
{
    // 1. Unified Inbox container only (실제 이메일 계정은 config_set/accounts.json 또는 계정 추가 마법사에서 로드·등록)
    {
        AccountItem unified;
        unified.id = "acc_unified";
        unified.email = "ALL ACCOUNTS (UNIFIED)";
        unified.displayName = "Unified Inbox";
        unified.provider = AccountProviderType::Unified;
        unified.visualStyle = AvatarVisualStyle::UnifiedEnvelope;
        m_accounts.append(unified);
    }

    // 2. System Standard Folders (하드코딩된 내 폴더 없음 — 각 계정의 IMAP 서버 LIST 응답 또는 사용자 추가 폴더만 동적 구성)
    {
        auto addFolder = [this](const QString &id, const QString &name, FolderKind kind) {
            FolderItem f;
            f.id = id;
            f.name = name;
            f.kind = kind;
            f.isCustom = false;
            f.hasChildren = false;
            f.dotColor = QColor("#3B82F6");
            m_folders.append(f);
        };
        addFolder("inbox",     "Inbox",     FolderKind::Inbox);
        addFolder("starred",   "Starred",   FolderKind::Starred);
        addFolder("drafts",    "Drafts",    FolderKind::Drafts);
        addFolder("sent",      "Sent",      FolderKind::Sent);
        addFolder("archive",   "Archive",   FolderKind::Archive);
        addFolder("snoozed",   "Snoozed",   FolderKind::Snoozed);
        addFolder("important", "Important", FolderKind::Important);
        addFolder("spam",      "Spam",      FolderKind::Spam);
        addFolder("trash",     "Trash",     FolderKind::Trash);
    }

    // 3. 기본 로컬 캘린더 1개 초기화 (실제 계정별 캘린더·일정·연락처·메일은 cache/*.json 및 서버 동기화에서만 로드)
    {
        m_calendars = {
            {"cal_local_default", "SOLLUNAMAIL (LOCAL)", "My Calendar", QColor("#2563EB"), true, true, false}
        };
    }

    // 시스템 기본 폴더 초기화 완료 후 디스크에서 계정 및 계정별 폴더 설정을 복원
    loadAccountsFromDisk();
    if (m_accounts.size() > 1 && m_selectedAccountId == "acc_unified") {
        m_selectedAccountId = m_accounts[1].id;
    }
}

void MainWindow::setupMainMenuBar()
{
    QMenuBar *mb = menuBar();
    mb->clear();
    mb->setStyleSheet(R"(
        QMenuBar {
            background-color: #FFFFFF;
            border-bottom: 1px solid #E5E7EB;
            padding: 2px 8px;
            font-size: 12.5px;
            font-weight: 600;
            color: #1F2937;
        }
        QMenuBar::item {
            padding: 5px 12px;
            border-radius: 4px;
            background: transparent;
        }
        QMenuBar::item:selected {
            background-color: #EFF6FF;
            color: #0D47A1;
        }
        QMenu {
            background-color: #FFFFFF;
            border: 1px solid #D1D5DB;
            border-radius: 6px;
            padding: 4px;
            font-size: 12.5px;
            color: #1F2937;
        }
        QMenu::item {
            padding: 7px 22px;
            border-radius: 4px;
        }
        QMenu::item:selected {
            background-color: #EFF6FF;
            color: #0D47A1;
            font-weight: 600;
        }
    )");

    // 1. File Menu (파일 / File / ファイル)
    QMenu *fileMenu = mb->addMenu(MailI18n::t("파일(&F)", "&File", "ファイル(&F)"));
    QAction *actCompose = fileMenu->addAction(MailI18n::t("✉️ 새 메일 작성 (Compose)", "✉️ Compose New Email", "✉️ 新規メール作成"));
    actCompose->setShortcut(QKeySequence("Ctrl+N"));
    connect(actCompose, &QAction::triggered, this, [this]() { onComposeNewMail(); });

    QAction *actAddAcc = fileMenu->addAction(MailI18n::t("+ 새 이메일 계정 추가 (Google / iCloud / IMAP)", "+ Add Email Account (Google / iCloud / IMAP)", "+ メールアカウント追加 (Google / iCloud / IMAP)"));
    connect(actAddAcc, &QAction::triggered, this, &MainWindow::onQuickAddAccountFromLeftBar);

    QAction *actSync = fileMenu->addAction(MailI18n::t("🔄 지금 동기화 (Sync Now)", "🔄 Sync Now", "🔄 今すぐ同期"));
    actSync->setShortcut(QKeySequence("F5"));
    connect(actSync, &QAction::triggered, this, &MainWindow::onRefreshAndSyncCurrentAccount);

    fileMenu->addSeparator();
    QAction *actHideTray = fileMenu->addAction(MailI18n::t("📥 트레이로 숨기기 (Minimize to Tray)", "📥 Minimize to Tray", "📥 トレイに格納"));
    connect(actHideTray, &QAction::triggered, this, &QWidget::hide);

    QAction *actExit = fileMenu->addAction(MailI18n::t("🚪 완전히 종료 (Quit)", "🚪 Quit SollunaMail", "🚪 終了"));
    actExit->setShortcut(QKeySequence("Ctrl+Q"));
    connect(actExit, &QAction::triggered, this, [this]() {
        m_forceQuit = true;
        QTimer::singleShot(0, this, &QWidget::close);
    });

    // 2. View Menu (보기 / View / 表示)
    QMenu *viewMenu = mb->addMenu(MailI18n::t("보기(&V)", "&View", "表示(&V)"));
    QAction *actViewMail = viewMenu->addAction(MailI18n::t("📬 메일함 화면 (Mail Workspace)", "📬 Mail Workspace", "📬 メール画面"));
    connect(actViewMail, &QAction::triggered, this, [this]() { m_centerStack->setCurrentIndex(0); });

    QAction *actViewCal = viewMenu->addAction(MailI18n::t("📅 캘린더 전체화면 (Full Calendar)", "📅 Full Calendar", "📅 カレンダー全画面"));
    connect(actViewCal, &QAction::triggered, this, [this]() { m_centerStack->setCurrentIndex(1); });

    QAction *actViewContacts = viewMenu->addAction(MailI18n::t("👥 연락처 화면 (Contacts)", "👥 Contacts", "👥 連絡先画面"));
    connect(actViewContacts, &QAction::triggered, this, [this]() { m_centerStack->setCurrentIndex(2); });

    viewMenu->addSeparator();
    QAction *actToggleRightCal = viewMenu->addAction(MailI18n::t("📌 우측 캘린더 사이드바 토글", "📌 Toggle Right Calendar Sidebar", "📌 右カレンダーサイドバー切替"));
    connect(actToggleRightCal, &QAction::triggered, this, [this]() {
        if (m_rightCalendarSidebar)
            m_rightCalendarSidebar->setVisible(!m_rightCalendarSidebar->isVisible());
    });

    // 3. Mail Menu (메일 / Mail / メール)
    QMenu *mailMenu = mb->addMenu(MailI18n::t("메일(&M)", "&Mail", "メール(&M)"));
    QAction *actReply = mailMenu->addAction(MailI18n::t("↩️ 답장 (Reply)", "↩️ Reply", "↩️ 返信"));
    actReply->setShortcut(QKeySequence("Ctrl+R"));
    connect(actReply, &QAction::triggered, this, [this]() { onReplyCurrent(false); });

    QAction *actReplyAll = mailMenu->addAction(MailI18n::t("↩️ 전체 답장 (Reply All)", "↩️ Reply All", "↩️ 全員に返信"));
    connect(actReplyAll, &QAction::triggered, this, [this]() { onReplyCurrent(true); });

    QAction *actForward = mailMenu->addAction(MailI18n::t("➡️ 전달 (Forward)", "➡️ Forward", "➡️ 転送"));
    connect(actForward, &QAction::triggered, this, &MainWindow::onForwardCurrent);

    mailMenu->addSeparator();
    QAction *actArchive = mailMenu->addAction(MailI18n::t("📥 보관함 이동 (Archive)", "📥 Archive", "📥 アーカイブ"));
    connect(actArchive, &QAction::triggered, this, &MainWindow::onArchiveCurrent);

    QAction *actDelete = mailMenu->addAction(MailI18n::t("🗑️ 삭제 (Delete)", "🗑️ Delete", "🗑️ 削除"));
    connect(actDelete, &QAction::triggered, this, &MainWindow::onDeleteCurrent);

    // 4. Settings Menu (설정 / Settings / 設定)
    QMenu *settingsMenu = mb->addMenu(MailI18n::t("설정(&S)", "&Settings", "設定(&S)"));
    QAction *actSetGen = settingsMenu->addAction(MailI18n::t("⚙️ 일반 및 다국어 설정 (General)", "⚙️ General & Language", "⚙️ 一般・言語設定"));
    connect(actSetGen, &QAction::triggered, this, [this]() { onOpenSettingsModal(0); });

    QAction *actSetAcc = settingsMenu->addAction(MailI18n::t("👤 이메일 계정 관리 (Accounts)", "👤 Email Accounts", "👤 メールアカウント管理"));
    connect(actSetAcc, &QAction::triggered, this, [this]() { onOpenSettingsModal(1); });

    QAction *actSetTheme = settingsMenu->addAction(MailI18n::t("🎨 테마 및 모양 (Appearance)", "🎨 Appearance & Themes", "🎨 外観・テーマ"));
    connect(actSetTheme, &QAction::triggered, this, [this]() { onOpenSettingsModal(2); });

    QAction *actSetPriv = settingsMenu->addAction(MailI18n::t("🛡️ 개인정보 보호 허브 (Privacy Hub)", "🛡️ Privacy Hub", "🛡️ プライバシーハブ"));
    connect(actSetPriv, &QAction::triggered, this, [this]() { onOpenSettingsModal(3); });

    // 5. Language Menu (🌐 언어 / Language / 言語 — Korean / English / Japanese)
    QMenu *langMenu = mb->addMenu(MailI18n::t("🌐 언어 (Language)", "🌐 Language", "🌐 言語"));
    QActionGroup *langGroup = new QActionGroup(this);

    QAction *actKo = langMenu->addAction("🇰🇷 한국어 (Korean)");
    actKo->setCheckable(true);
    actKo->setChecked(MailI18n::currentLanguage() == AppLanguage::Korean);
    langGroup->addAction(actKo);
    connect(actKo, &QAction::triggered, this, [this]() {
        QTimer::singleShot(0, this, [this]() { onChangeLanguage(AppLanguage::Korean); });
    });

    QAction *actEn = langMenu->addAction("🇺🇸 English");
    actEn->setCheckable(true);
    actEn->setChecked(MailI18n::currentLanguage() == AppLanguage::English);
    langGroup->addAction(actEn);
    connect(actEn, &QAction::triggered, this, [this]() {
        QTimer::singleShot(0, this, [this]() { onChangeLanguage(AppLanguage::English); });
    });

    QAction *actJa = langMenu->addAction("🇯🇵 日本語 (Japanese)");
    actJa->setCheckable(true);
    actJa->setChecked(MailI18n::currentLanguage() == AppLanguage::Japanese);
    langGroup->addAction(actJa);
    connect(actJa, &QAction::triggered, this, [this]() {
        QTimer::singleShot(0, this, [this]() { onChangeLanguage(AppLanguage::Japanese); });
    });

    // 6. Scheduled Send / Expired Schedule Status Indicator (🌐 언어 버튼 우측 표시)
    m_menuBarScheduledAction = mb->addAction(QStringLiteral("⏰ 발송 예약 중"));
    m_menuBarScheduledAction->setVisible(false);
    connect(m_menuBarScheduledAction, &QAction::triggered, this, [this]() {
        QString targetAccId;
        QString targetConvId;
        // 우선순위 1: 시간이 지나 보내지 못한 예약 메일(EXPIRED:)
        for (const ConversationItem &c : m_conversations) {
            if (c.scheduledSendIso.startsWith(QStringLiteral("EXPIRED:"))) {
                targetAccId = c.accountId;
                targetConvId = c.id;
                break;
            }
        }
        // 우선순위 2: 발송 예약 대기 중인 메일
        if (targetConvId.isEmpty()) {
            for (const ConversationItem &c : m_conversations) {
                if (!c.scheduledSendIso.isEmpty()) {
                    targetAccId = c.accountId;
                    targetConvId = c.id;
                    break;
                }
            }
        }
        m_centerStack->setCurrentIndex(0);
        if (!targetAccId.isEmpty() && m_selectedAccountId != QStringLiteral("acc_unified") && m_selectedAccountId != targetAccId) {
            m_selectedAccountId = targetAccId;
            rebuildCustomFoldersList();
        }
        m_selectedConvId = targetConvId;
        onSelectFolder(QStringLiteral("drafts"), MailI18n::folderTitle("drafts", "Drafts"));
        if (!targetConvId.isEmpty() && m_convListWidget) {
            for (int r = 0; r < m_convListWidget->count(); ++r) {
                QListWidgetItem *it = m_convListWidget->item(r);
                if (it && it->data(Qt::UserRole + 1).toString() == targetConvId) {
                    m_convListWidget->setCurrentRow(r);
                    displayConversation(r, true);
                    break;
                }
            }
        }
    });
    updateScheduledMailStatusIndicator();
}

void MainWindow::setupMainLayout()
{
    QWidget *central = new QWidget(this);
    setCentralWidget(central);

    QHBoxLayout *rootH = new QHBoxLayout(central);
    rootH->setContentsMargins(0, 0, 0, 0);
    rootH->setSpacing(0);

    // 1. Far-Left Account & App Dock Bar (48px)
    m_leftBarWidget = buildLeftBar();
    rootH->addWidget(m_leftBarWidget);

    // 2. Folder Sidebar Pane (196px)
    m_folderPaneWidget = buildFolderPane();
    rootH->addWidget(m_folderPaneWidget);

    // 3. Center Stacked Workspace (Page 0: Mail View + Right Docked Calendar, Page 1: Full Calendar, Page 2: Contacts)
    m_centerStack = new QStackedWidget(central);
    m_centerStack->addWidget(buildMailWorkspacePage()); // Index 0
    m_centerStack->addWidget(buildFullCalendarPage());  // Index 1
    m_centerStack->addWidget(buildFullContactsPage());  // Index 2
    rootH->addWidget(m_centerStack, 1);

    // 4. Far-Right Narrow App Strip (38px)
    rootH->addWidget(buildFarRightAppStrip());
}

QWidget *MainWindow::buildLeftBar()
{
    QWidget *bar = new QWidget(this);
    bar->setObjectName("leftBar");
    bar->setFixedWidth(48);
    bar->setStyleSheet(QString("QWidget#leftBar { background-color: %1; }").arg(m_palette.leftBarBg.name()));

    QVBoxLayout *v = new QVBoxLayout(bar);
    v->setContentsMargins(7, 12, 7, 12);
    v->setSpacing(10);

    // Top Account Avatars container (dynamically rebuilt when accounts are added/edited/deleted)
    m_leftBarAccountsLayout = new QVBoxLayout();
    m_leftBarAccountsLayout->setContentsMargins(0, 0, 0, 0);
    m_leftBarAccountsLayout->setSpacing(10);
    v->addLayout(m_leftBarAccountsLayout);

    // Quick "+" Add Account button right below account avatars
    QToolButton *btnAddAcc = new QToolButton(bar);
    btnAddAcc->setFixedSize(34, 30);
    btnAddAcc->setCursor(Qt::PointingHandCursor);
    btnAddAcc->setText("+");
    btnAddAcc->setToolTip(
        MailI18n::t("새 계정 추가 (Google / iCloud / 네이버 / 회사 메일 원클릭 연동)",
                    "Add New Account (Google / iCloud / Naver / Work Email)",
                    "新しいアカウントを追加 (Google / iCloud / Naver / 会社メール)"));
    btnAddAcc->setStyleSheet(
        "QToolButton { border: 1px dashed rgba(255,255,255,0.45); color: #FFFFFF; font-size: 18px; font-weight: 700; background: rgba(255,255,255,0.06); border-radius: 7px; }"
        "QToolButton:hover { background-color: rgba(255,255,255,0.22); border-color: #FFFFFF; }");
    connect(btnAddAcc, &QToolButton::clicked, this, &MainWindow::onQuickAddAccountFromLeftBar);
    v->addWidget(btnAddAcc, 0, Qt::AlignHCenter);

    v->addStretch();

    // Bottom Navigation Icons (Calendar [3], Contacts, Privacy Shield with yellow check, Settings gear)
    auto makeBottomIconBtn = [bar](const QString &iconName, const QString &tip) {
        QToolButton *b = new QToolButton(bar);
        b->setFixedSize(34, 32);
        b->setCursor(Qt::PointingHandCursor);
        b->setToolTip(tip);
        b->setIcon(MailIcons::actionIcon(iconName, QColor("#DCE6F7"), 18));
        b->setIconSize(QSize(18, 18));
        b->setStyleSheet("QToolButton { border: none; background: transparent; border-radius: 6px; }"
                         "QToolButton:hover { background-color: rgba(255,255,255,0.15); }");
        return b;
    };

    QToolButton *btnCal = makeBottomIconBtn(
        "calendar_3",
        MailI18n::t("캘린더 전체화면 보기 (Calendar)", "Open Full Calendar", "カレンダー全画面表示"));
    connect(btnCal, &QToolButton::clicked, this, [this]() {
        m_centerStack->setCurrentIndex(1);
    });

    QToolButton *btnContacts = makeBottomIconBtn(
        "contacts",
        MailI18n::t("연락처 보기 (All Contacts)", "Open Contacts", "連絡先を表示"));
    connect(btnContacts, &QToolButton::clicked, this, [this]() {
        m_centerStack->setCurrentIndex(2);
    });

    QToolButton *btnPrivacy = makeBottomIconBtn(
        "privacy_shield",
        MailI18n::t("Privacy Hub 열기", "Open Privacy Hub", "プライバシーハブを開く"));
    connect(btnPrivacy, &QToolButton::clicked, this, [this]() {
        onOpenSettingsModal(3); // Opens Privacy Hub directly
    });

    QToolButton *btnSettings = makeBottomIconBtn(
        "settings",
        MailI18n::t("설정 및 Google 계정 연동 (Settings)", "Settings & Accounts", "設定とアカウント連携"));
    connect(btnSettings, &QToolButton::clicked, this, [this]() {
        onOpenSettingsModal(1); // Opens Accounts / Settings
    });

    v->addWidget(btnCal, 0, Qt::AlignHCenter);
    v->addWidget(btnContacts, 0, Qt::AlignHCenter);
    v->addWidget(btnPrivacy, 0, Qt::AlignHCenter);
    v->addWidget(btnSettings, 0, Qt::AlignHCenter);

    m_leftBarWidget = bar;
    rebuildLeftBarAccounts();
    return bar;
}

QWidget *MainWindow::buildFolderPane()
{
    QWidget *pane = new QWidget(this);
    pane->setObjectName("folderPane");
    pane->setFixedWidth(212);
    pane->setStyleSheet(QString("QWidget#folderPane { background-color: %1; }").arg(m_palette.folderPaneBg.name()));

    QVBoxLayout *v = new QVBoxLayout(pane);
    v->setContentsMargins(10, 16, 10, 12);
    v->setSpacing(3);

    // Account Header
    m_folderAccountHeaderLbl = new QLabel("ALL ACCOUNTS (UNIFIED)", pane);
    m_folderAccountHeaderLbl->setStyleSheet("color: #9BB5E8; font-size: 10px; font-weight: 700; padding-left: 8px; margin-bottom: 6px;");
    v->addWidget(m_folderAccountHeaderLbl);

    // Helper to attach right-click folder context menu (Mark all as read / Empty folder / Sync folder)
    auto attachFolderContextMenu = [this](QPushButton *btn, const FolderItem &f) {
        btn->setContextMenuPolicy(Qt::CustomContextMenu);
        connect(btn, &QWidget::customContextMenuRequested, this, [this, btn, f](const QPoint &pos) {
            QMenu menu(this);
            menu.setStyleSheet(
                "QMenu { background-color: #FFFFFF; border: 1px solid #E5E7EB; border-radius: 6px; padding: 6px 4px; }"
                "QMenu::item { padding: 7px 22px 7px 14px; font-size: 12px; color: #1F2937; border-radius: 4px; }"
                "QMenu::item:selected { background-color: #EFF6FF; color: #0D47A1; font-weight: 600; }"
                "QMenu::separator { height: 1px; background: #E5E7EB; margin: 4px 6px; }");

            QAction *actMarkRead = menu.addAction(
                MailIcons::actionIcon("unread_toggle", QColor("#374151"), 15),
                MailI18n::t("모두 읽음으로 표시 (Mark all as read)", "Mark all as read", "すべて既読にする"));
            connect(actMarkRead, &QAction::triggered, this, [this, f]() {
                onMarkFolderAllRead(f.id);
            });

            if (f.id == "trash" || f.id == "spam") {
                QAction *actRestoreAll = menu.addAction(
                    MailIcons::actionIcon("folder_move", QColor("#059669"), 15),
                    MailI18n::t("♻️ 이 폴더의 메일 모두 원래 편지함으로 복원 (Restore all)",
                                "♻️ Restore all messages to original folders",
                                "♻️ すべてのメールを元のフォルダに復元"));
                connect(actRestoreAll, &QAction::triggered, this, [this, f]() {
                    onRestoreAllFromTrash(f.id);
                });

                QString emptyLabel = (f.id == "trash")
                    ? MailI18n::t("🗑️ 휴지통 비우기 - 영구 삭제 (Empty Trash)", "🗑️ Empty Trash (Permanent)", "🗑️ ゴミ箱を空にする")
                    : MailI18n::t("🗑️ 스팸함 비우기 - 영구 삭제 (Empty Spam)", "🗑️ Empty Spam (Permanent)", "🗑️ 迷惑メールを空にする");
                QAction *actEmpty = menu.addAction(
                    MailIcons::actionIcon("delete", QColor("#DC2626"), 15),
                    emptyLabel);
                connect(actEmpty, &QAction::triggered, this, [this, f]() {
                    onEmptyFolder(f.id);
                });
            } else {
                QAction *actMoveToTrash = menu.addAction(
                    MailIcons::actionIcon("delete", QColor("#D97706"), 15),
                    MailI18n::t("🗑️ 이 폴더의 메일 모두 휴지통으로 이동 (Move all to Trash)",
                                "🗑️ Move all messages in folder to Trash",
                                "🗑️ このフォルダのメールをすべてゴミ箱へ移動"));
                connect(actMoveToTrash, &QAction::triggered, this, [this, f]() {
                    onMoveFolderAllToTrash(f.id);
                });
            }

            menu.addSeparator();
            QAction *actSync = menu.addAction(
                MailIcons::actionIcon("refresh", QColor("#374151"), 15),
                MailI18n::t("이 폴더 새로고침 (Sync folder)", "Sync folder", "このフォルダを同期"));
            connect(actSync, &QAction::triggered, this, [this, f]() {
                onSelectFolder(f.id, MailI18n::folderTitle(f.id, f.name));
                for (const AccountItem &acc : m_accounts) {
                    if (acc.provider == AccountProviderType::Unified)
                        continue;
                    if (m_selectedAccountId != "acc_unified" && acc.id != m_selectedAccountId)
                        continue;
                    if (!acc.appPassword.isEmpty() || !acc.oauthAccessToken.isEmpty())
                        m_googleProvider->fetchImapFolder(acc, f.id);
                }
            });

            menu.exec(btn->mapToGlobal(pos));
        });
    };

    // Scrollable container for System Folders + YOUR FOLDERS so the folder list never stretches the main window height
    QScrollArea *folderScroll = new QScrollArea(pane);
    folderScroll->setWidgetResizable(true);
    folderScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    folderScroll->setFrameShape(QFrame::NoFrame);
    folderScroll->setStyleSheet(
        "QScrollArea { background: transparent; border: none; }"
        "QScrollBar:vertical { background: transparent; width: 6px; margin: 0px; }"
        "QScrollBar::handle:vertical { background: rgba(255,255,255,0.25); border-radius: 3px; min-height: 24px; }"
        "QScrollBar::handle:vertical:hover { background: rgba(255,255,255,0.45); }"
        "QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height: 0px; }");

    QWidget *scrollContent = new QWidget(folderScroll);
    scrollContent->setStyleSheet("background: transparent;");
    QVBoxLayout *sv = new QVBoxLayout(scrollContent);
    sv->setContentsMargins(0, 0, 2, 0);
    sv->setSpacing(3);

    // System Folders
    for (const FolderItem &f : m_folders) {
        if (f.isCustom)
            continue;
        QPushButton *btn = new QPushButton(MailI18n::folderTitle(f.id, f.name), scrollContent);
        btn->setCursor(Qt::PointingHandCursor);
        btn->setIcon(MailIcons::folderIcon(f.kind, QColor("#FFFFFF"), 17));
        btn->setIconSize(QSize(17, 17));
        btn->setFixedHeight(32);
        connect(btn, &QPushButton::clicked, this, [this, f]() {
            onSelectFolder(f.id, MailI18n::folderTitle(f.id, f.name));
        });
        attachFolderContextMenu(btn, f);
        sv->addWidget(btn);
        m_folderButtons.insert(f.id, btn);
    }

    sv->addSpacing(14);

    // YOUR FOLDERS section header
    m_yourFoldersHeaderLbl = new QLabel(MailI18n::t("내 폴더", "YOUR FOLDERS", "マイフォルダ"), scrollContent);
    m_yourFoldersHeaderLbl->setStyleSheet("color: #8BA8E0; font-size: 9.5px; font-weight: 700; padding-left: 8px; margin-bottom: 4px;");
    sv->addWidget(m_yourFoldersHeaderLbl);

    QWidget *customContainer = new QWidget(scrollContent);
    m_customFoldersLayout = new QVBoxLayout(customContainer);
    m_customFoldersLayout->setContentsMargins(0, 0, 0, 0);
    m_customFoldersLayout->setSpacing(3);
    sv->addWidget(customContainer);

    sv->addStretch();
    folderScroll->setWidget(scrollContent);
    v->addWidget(folderScroll, 1);

    rebuildCustomFoldersList();

    // Bottom "Manage folders" button
    m_manageFoldersBtn = new QPushButton(MailI18n::t("  폴더 관리", "  Manage folders", "  フォルダ管理"), pane);
    m_manageFoldersBtn->setCursor(Qt::PointingHandCursor);
    m_manageFoldersBtn->setIcon(MailIcons::actionIcon("folder_move", QColor("#8BA8E0"), 16));
    m_manageFoldersBtn->setStyleSheet("QPushButton { text-align: left; padding-left: 8px; color: #8BA8E0; background: transparent; border: none; font-size: 11px; height: 28px; }"
                                      "QPushButton:hover { color: #FFFFFF; }");
    connect(m_manageFoldersBtn, &QPushButton::clicked, this, [this]() {
        onOpenSettingsModal(4); // Folders tab
    });
    v->addWidget(m_manageFoldersBtn);

    return pane;
}

QWidget *MainWindow::buildMailWorkspacePage()
{
    QWidget *page = new QWidget(this);
    QHBoxLayout *outerH = new QHBoxLayout(page);
    outerH->setContentsMargins(0, 0, 0, 0);
    outerH->setSpacing(0);

    QWidget *mailSplitContainer = new QWidget(page);
    m_mailSplitLayout = new QBoxLayout(QBoxLayout::LeftToRight, mailSplitContainer);
    m_mailSplitLayout->setContentsMargins(0, 0, 0, 0);
    m_mailSplitLayout->setSpacing(0);

    // ------------------------------------------------------------------------
    // A. Conversation List Pane (300px)
    // ------------------------------------------------------------------------
    QWidget *convPane = new QWidget(mailSplitContainer);
    convPane->setObjectName("convPane");
    m_convPaneWidget = convPane;
    convPane->setFixedWidth(300);
    convPane->setStyleSheet("QWidget#convPane { background-color: #FFFFFF; border-right: 1px solid #E5E7EB; }");
    QVBoxLayout *convV = new QVBoxLayout(convPane);
    convV->setContentsMargins(0, 0, 0, 0);
    convV->setSpacing(0);

    // Header: "Inbox" + 5 action buttons (Compose, Search, Unread filter, Refresh, More)
    QWidget *convHeader = new QWidget(convPane);
    convHeader->setFixedHeight(58);
    QHBoxLayout *convHdrH = new QHBoxLayout(convHeader);
    convHdrH->setContentsMargins(18, 8, 10, 8);
    convHdrH->setSpacing(4);

    m_convPaneTitleLbl = new QLabel("Inbox", convHeader);
    m_convPaneTitleLbl->setStyleSheet("font-size: 19px; font-weight: 400; color: #374151; border: none;");
    convHdrH->addWidget(m_convPaneTitleLbl);
    convHdrH->addStretch();

    auto makeHeaderToolBtn = [convHeader](const QString &iconName, const QString &tip, bool checkable = false) {
        QToolButton *b = new QToolButton(convHeader);
        b->setFixedSize(28, 28);
        b->setCheckable(checkable);
        b->setCursor(Qt::PointingHandCursor);
        b->setToolTip(tip);
        b->setIcon(MailIcons::actionIcon(iconName, QColor("#4B5563"), 17));
        b->setIconSize(QSize(17, 17));
        b->setStyleSheet("QToolButton { border: none; background: transparent; border-radius: 4px; }"
                         "QToolButton:hover { background-color: #F3F4F6; }"
                         "QToolButton:checked { background-color: #DBEAFE; }");
        return b;
    };

    QToolButton *btnCompose = makeHeaderToolBtn("compose", MailI18n::t("새 메일 작성 (Compose)", "Compose", "新規メール作成"));
    connect(btnCompose, &QToolButton::clicked, this, [this]() { onComposeNewMail(); });

    QToolButton *btnSearchToggle = makeHeaderToolBtn("search", MailI18n::t("메일 검색 (Search)", "Search", "メールを検索"), true);
    QToolButton *btnUnreadFilter = makeHeaderToolBtn("unread_filter", MailI18n::t("읽지 않은 메일만 보기 (Show Only Unread)", "Show Unread Only", "未読メールのみ表示"), true);
    m_btnUnreadFilter = btnUnreadFilter;
    QToolButton *btnRefresh = makeHeaderToolBtn("refresh", MailI18n::t("새로고침 및 서버 동기화 (Refresh)", "Refresh & Sync", "更新とサーバー同期"));
    QToolButton *btnMore = makeHeaderToolBtn("more", MailI18n::t("메일함 확장 메뉴 (읽지 않은 메일만 보기 / 편지함 이동 / 모두 읽음 표시 / 폴더 비우기 / 레이아웃)", "Folder Options", "フォルダオプション"));

    auto moveSelectedOrCurrentMessagesToFolder = [this](const QString &targetFolderId,
                                                        const QString &targetFolderDisplayName,
                                                        bool preferCheckedBatch) {
        if (targetFolderId.trimmed().isEmpty())
            return;

        QList<int> targetConvIndices;
        bool usedCheckedBatch = false;
        if (preferCheckedBatch && !m_checkedConvIds.isEmpty()) {
            for (int idx : m_filteredConvIndices) {
                if (idx >= 0 && idx < m_conversations.size()
                    && m_checkedConvIds.contains(m_conversations[idx].id)) {
                    targetConvIndices.append(idx);
                }
            }
            if (targetConvIndices.isEmpty()) {
                for (int i = 0; i < m_conversations.size(); ++i) {
                    if (m_checkedConvIds.contains(m_conversations[i].id)) {
                        targetConvIndices.append(i);
                    }
                }
            }
            usedCheckedBatch = !targetConvIndices.isEmpty();
        }
        if (targetConvIndices.isEmpty()
            && m_selectedConvIndex >= 0
            && m_selectedConvIndex < m_conversations.size()) {
            targetConvIndices.append(m_selectedConvIndex);
        }

        if (targetConvIndices.isEmpty()) {
            QMessageBox::information(
                this,
                MailI18n::t("편지함 이동", "Move to Folder", "フォルダへ移動"),
                MailI18n::t("먼저 이동할 메일을 목록에서 선택하거나 좌측 체크박스를 체크해 주세요.",
                            "Please select or check at least one message to move first.",
                            "先に移動するメールを選択するかチェックボックスをチェックしてください。"));
            return;
        }

        if (targetFolderId == QStringLiteral("trash")) {
            if (usedCheckedBatch) {
                onMoveCheckedMessagesToTrash();
                return;
            }
            if (targetConvIndices.size() == 1) {
                const QString curFid = m_conversations[targetConvIndices.first()].folderId;
                if (curFid != QStringLiteral("trash") && curFid != QStringLiteral("spam")) {
                    onDeleteCurrent();
                    return;
                }
            }
        }

        const QString resolvedTitle = targetFolderDisplayName.trimmed().isEmpty()
                                          ? MailI18n::folderTitle(targetFolderId, targetFolderId)
                                          : targetFolderDisplayName.trimmed();
        int movedCount = 0;
        bool unblockedAnySender = false;
        QStringList unblockedSenders;
        QMap<QString, QList<int>> imapBatchSeqs; // key: accountId|srcImapFolder

        for (int idx : targetConvIndices) {
            if (idx < 0 || idx >= m_conversations.size())
                continue;
            ConversationItem &c = m_conversations[idx];
            const QString oldId = c.id;
            const QString srcFolder = c.folderId;
            if (srcFolder == targetFolderId) {
                m_checkedConvIds.remove(oldId);
                continue;
            }

            const QString sigKey = conversationSignatureKey(c);
            if (targetFolderId == QStringLiteral("trash")) {
                if (!c.id.startsWith(QStringLiteral("imap_")))
                    m_deletedConvIds.insert(c.id);
                m_deletedConvIds.insert(QStringLiteral("trash|") + sigKey);
            } else if (srcFolder == QStringLiteral("trash") || srcFolder == QStringLiteral("spam")) {
                m_deletedConvIds.remove(c.id);
                m_deletedConvIds.remove(QStringLiteral("trash|") + sigKey);
                m_deletedConvIds.remove(QStringLiteral("perm|") + sigKey);
                if (srcFolder == QStringLiteral("spam")
                    && targetFolderId != QStringLiteral("spam")
                    && !c.messages.isEmpty()) {
                    if (unblockSenderFromSpamLists(m_privacySettings, c.latestMessage().senderEmail, &unblockedSenders)) {
                        unblockedAnySender = true;
                    }
                }
            }

            const int removedSeq = c.imapSeq;
            if (removedSeq > 0) {
                const QString fk = c.accountId + QStringLiteral("|") + srcFolder;
                if (m_folderTotalExists.value(fk, 0) > 0)
                    m_folderTotalExists[fk] = m_folderTotalExists.value(fk) - 1;
                for (ConversationItem &other : m_conversations) {
                    if (other.accountId == c.accountId && other.folderId == srcFolder && other.imapSeq > removedSeq)
                        other.imapSeq -= 1;
                }
            }

            const int lastUnder = c.id.lastIndexOf('_');
            const int uidNum = (c.imapUid > 0)
                                   ? c.imapUid
                                   : ((lastUnder > 0 && c.id.startsWith(QStringLiteral("imap_")))
                                          ? c.id.mid(lastUnder + 1).toInt()
                                          : 0);
            if (uidNum > 0 && c.id.startsWith(QStringLiteral("imap_"))) {
                const QString realSrc = srcFolder.isEmpty() ? QStringLiteral("inbox") : srcFolder;
                const QString batchKey = c.accountId + QStringLiteral("|") + realSrc;
                if (!imapBatchSeqs[batchKey].contains(uidNum)) {
                    imapBatchSeqs[batchKey].append(uidNum);
                }
                c.id = QString("imap_%1_%2_%3").arg(c.accountId, targetFolderId).arg(uidNum);
            }

            if (srcFolder != QStringLiteral("trash") && srcFolder != QStringLiteral("spam")) {
                c.previousFolderId = srcFolder;
            } else if (targetFolderId != QStringLiteral("trash") && targetFolderId != QStringLiteral("spam")) {
                c.previousFolderId.clear();
            }
            c.folderId = targetFolderId;
            c.folderTag = targetFolderId.startsWith(QStringLiteral("custom_"))
                              ? resolvedTitle
                              : MailI18n::folderTitle(targetFolderId, resolvedTitle);
            c.imapSeq = 0;
            if (targetFolderId == QStringLiteral("important")) {
                c.isImportant = true;
                m_importantConvSigs.insert(sigKey);
                m_unimportantConvSigs.remove(sigKey);
            } else if (targetFolderId == QStringLiteral("starred")) {
                c.isStarred = true;
                m_starredConvSigs.insert(sigKey);
                m_unstarredConvSigs.remove(sigKey);
            }

            m_checkedConvIds.remove(oldId);
            m_userExplicitlyOpenedConvIds.remove(oldId);
            if (m_selectedConvId == oldId)
                m_selectedConvId.clear();
            ++movedCount;
        }

        if (movedCount <= 0)
            return;

        for (auto it = imapBatchSeqs.constBegin(); it != imapBatchSeqs.constEnd(); ++it) {
            const QString accId = it.key().section('|', 0, 0);
            const QString srcFolder = it.key().section('|', 1);
            for (const AccountItem &acc : m_accounts) {
                if (acc.id == accId) {
                    m_googleProvider->moveImapMessagesToFolder(
                        acc, srcFolder, it.value(), targetFolderId, resolvedTitle, false, false);
                    break;
                }
            }
        }

        if (unblockedAnySender) {
            saveAccountsToDisk();
        }
        saveConversationsCacheToDisk();
        updateAppUnreadBadgeIcon();
        refreshFolderSelectionStyles();
        refreshConversationList();

        QString doneMsg = MailI18n::t(
            QString("📁 메일 %1건을 '%2' 편지함으로 이동했습니다.").arg(movedCount).arg(resolvedTitle),
            QString("📁 Moved %1 message(s) to '%2'.").arg(movedCount).arg(resolvedTitle),
            QString("📁 メール %1件を '%2' フォルダに移動しました。").arg(movedCount).arg(resolvedTitle));
        if (!unblockedSenders.isEmpty()) {
            doneMsg += MailI18n::t(
                QString(" (🔓 발신자 차단 자동 해제: %1)").arg(unblockedSenders.join(QStringLiteral(", "))),
                QString(" (🔓 Unblocked sender: %1)").arg(unblockedSenders.join(QStringLiteral(", "))),
                QString(" (🔓 送信者ブロック自動解除: %1)").arg(unblockedSenders.join(QStringLiteral(", "))));
        }
        setWindowTitle(QString("SollunaMail — %1").arg(doneMsg));
        if (statusBar()) {
            statusBar()->showMessage(doneMsg, 6000);
        }
    };

    auto populateMoveToFolderMenu = [this, moveSelectedOrCurrentMessagesToFolder](QMenu *moveMenu, bool preferCheckedBatch) {
        if (!moveMenu)
            return;
        moveMenu->setStyleSheet(
            "QMenu { background-color: #FFFFFF; border: 1px solid #E5E7EB; border-radius: 8px; padding: 6px 4px; min-width: 220px; }"
            "QMenu::item { padding: 7px 18px 7px 14px; font-size: 12.5px; color: #1F2937; border-radius: 5px; }"
            "QMenu::item:selected { background-color: #EFF6FF; color: #0D47A1; font-weight: 600; }"
            "QMenu::item:disabled { color: #9CA3AF; }"
            "QMenu::separator { height: 1px; background: #E5E7EB; margin: 4px 6px; }");

        QString currentFolderId = m_selectedFolderId;
        QString targetAccId = (m_selectedAccountId != QStringLiteral("acc_unified")) ? m_selectedAccountId : QString();
        if (preferCheckedBatch && !m_checkedConvIds.isEmpty()) {
            for (const ConversationItem &c : m_conversations) {
                if (m_checkedConvIds.contains(c.id)) {
                    currentFolderId = c.folderId;
                    if (targetAccId.isEmpty())
                        targetAccId = c.accountId;
                    break;
                }
            }
        } else if (m_selectedConvIndex >= 0 && m_selectedConvIndex < m_conversations.size()) {
            currentFolderId = m_conversations[m_selectedConvIndex].folderId;
            if (targetAccId.isEmpty())
                targetAccId = m_conversations[m_selectedConvIndex].accountId;
        }

        if (currentFolderId == QStringLiteral("trash") || currentFolderId == QStringLiteral("spam")
            || m_selectedFolderId == QStringLiteral("trash") || m_selectedFolderId == QStringLiteral("spam")) {
            QAction *aRestore = moveMenu->addAction(
                MailIcons::actionIcon("folder_move", QColor("#059669"), 16),
                MailI18n::t("♻️ 원래 편지함으로 복원 (Restore)", "♻️ Restore to original folder", "♻️ 元のフォルダに復元"));
            connect(aRestore, &QAction::triggered, this, &MainWindow::onRestoreCurrentFromTrash);
            moveMenu->addSeparator();
        }

        struct StdFolderEntry {
            QString id;
            FolderKind kind;
            QColor iconColor;
            QString label;
        };
        const QList<StdFolderEntry> stdFolders = {
            {QStringLiteral("inbox"),     FolderKind::Inbox,     QColor("#0D47A1"), MailI18n::t("📥 받은편지함 (Inbox)", "📥 Inbox", "📥 受信トレイ")},
            {QStringLiteral("sent"),      FolderKind::Sent,      QColor("#2563EB"), MailI18n::t("📤 보낸편지함 (Sent)", "📤 Sent", "📤 送信済み")},
            {QStringLiteral("archive"),   FolderKind::Archive,   QColor("#059669"), MailI18n::t("📦 보관함 (Archive)", "📦 Archive", "📦 アーカイブ")},
            {QStringLiteral("important"), FolderKind::Important, QColor("#E11D48"), MailI18n::t("🔖 중요편지함 (Important)", "🔖 Important", "🔖 重要")},
            {QStringLiteral("spam"),      FolderKind::Spam,      QColor("#D97706"), MailI18n::t("🚫 스팸편지함 (Spam)", "🚫 Spam", "🚫 迷惑メール")},
            {QStringLiteral("trash"),     FolderKind::Trash,     QColor("#DC2626"), MailI18n::t("🗑️ 휴지통 (Trash)", "🗑️ Trash", "🗑️ ゴミ箱")}
        };

        for (const StdFolderEntry &sf : stdFolders) {
            QAction *act = moveMenu->addAction(MailIcons::folderIcon(sf.kind, sf.iconColor, 16), sf.label);
            if ((!preferCheckedBatch || m_checkedConvIds.isEmpty()) && sf.id == currentFolderId) {
                act->setEnabled(false);
            }
            connect(act, &QAction::triggered, this, [moveSelectedOrCurrentMessagesToFolder, sf, preferCheckedBatch]() {
                moveSelectedOrCurrentMessagesToFolder(sf.id, MailI18n::folderTitle(sf.id, sf.id), preferCheckedBatch);
            });
        }

        QString targetAccEmail;
        for (const AccountItem &acc : m_accounts) {
            if (acc.id == targetAccId) {
                targetAccEmail = acc.email;
                break;
            }
        }

        bool addedCustomSeparator = false;
        for (const FolderItem &cf : m_folders) {
            if (!cf.isCustom)
                continue;
            if (!targetAccId.isEmpty() && targetAccId != QStringLiteral("acc_unified")) {
                const bool matchId = (!cf.accountId.isEmpty() && cf.accountId == targetAccId);
                const bool matchEmail = (!targetAccEmail.isEmpty() && !cf.accountEmail.isEmpty()
                                         && cf.accountEmail.compare(targetAccEmail, Qt::CaseInsensitive) == 0);
                if (!matchId && !matchEmail)
                    continue;
            }
            if (!addedCustomSeparator) {
                moveMenu->addSeparator();
                addedCustomSeparator = true;
            }
            const QString cfTitle = MailI18n::folderTitle(cf.id, cf.name);
            QAction *act = moveMenu->addAction(
                MailIcons::folderIcon(FolderKind::CustomFolder, cf.dotColor.isValid() ? cf.dotColor : QColor("#3B82F6"), 16),
                QString("📁 %1").arg(cfTitle));
            if ((!preferCheckedBatch || m_checkedConvIds.isEmpty()) && cf.id == currentFolderId) {
                act->setEnabled(false);
            }
            connect(act, &QAction::triggered, this, [moveSelectedOrCurrentMessagesToFolder, cf, cfTitle, preferCheckedBatch]() {
                moveSelectedOrCurrentMessagesToFolder(cf.id, cfTitle, preferCheckedBatch);
            });
        }
    };

    // Folder & Message List ⋮ Dropdown Menu
    connect(btnMore, &QToolButton::clicked, this, [this, btnMore, populateMoveToFolderMenu]() {
        QMenu menu(this);
        menu.setStyleSheet(
            "QMenu { background-color: #FFFFFF; border: 1px solid #E5E7EB; border-radius: 8px; padding: 6px 4px; min-width: 240px; }"
            "QMenu::item { padding: 8px 20px 8px 14px; font-size: 12.5px; color: #374151; border-radius: 5px; }"
            "QMenu::item:selected { background-color: #EFF6FF; color: #0D47A1; font-weight: 600; }"
            "QMenu::separator { height: 1px; background: #E5E7EB; margin: 5px 8px; }");

        QAction *actUnreadOnly = menu.addAction(
            MailIcons::actionIcon("unread_filter", QColor("#0D47A1"), 16),
            MailI18n::t("📩 읽지 않은 메일만 보기 (Show unread only)",
                        "📩 Show unread messages only",
                        "📩 未読メールのみ表示"));
        actUnreadOnly->setCheckable(true);
        actUnreadOnly->setChecked(m_unreadOnlyFilter);
        connect(actUnreadOnly, &QAction::triggered, this, [this](bool checked) {
            m_unreadOnlyFilter = checked;
            m_currentMailPage = 1;
            if (m_btnUnreadFilter) {
                QSignalBlocker b(m_btnUnreadFilter);
                m_btnUnreadFilter->setChecked(checked);
            }
            refreshConversationList();
        });

        if (m_unreadOnlyFilter) {
            QAction *actShowAll = menu.addAction(
                MailIcons::folderIcon(FolderKind::Inbox, QColor("#059669"), 16),
                MailI18n::t("📬 전체 메일 보기 (Show all messages)",
                            "📬 Show all messages",
                            "📬 すべてのメールを表示"));
            connect(actShowAll, &QAction::triggered, this, [this]() {
                m_unreadOnlyFilter = false;
                m_currentMailPage = 1;
                if (m_btnUnreadFilter) {
                    QSignalBlocker b(m_btnUnreadFilter);
                    m_btnUnreadFilter->setChecked(false);
                }
                refreshConversationList();
            });
        }

        menu.addSeparator();

        int checkedInCurrentView = 0;
        for (int fIdx : m_filteredConvIndices) {
            if (fIdx >= 0 && fIdx < m_conversations.size()
                && m_checkedConvIds.contains(m_conversations[fIdx].id)) {
                ++checkedInCurrentView;
            }
        }
        const QString countSuffix = (checkedInCurrentView > 0)
                                        ? MailI18n::t(QString(" (%1건)").arg(checkedInCurrentView),
                                                      QString(" (%1)").arg(checkedInCurrentView),
                                                      QString(" (%1件)").arg(checkedInCurrentView))
                                        : QString();

        QAction *actMarkCheckedRead = menu.addAction(
            MailIcons::actionIcon("unread_toggle", QColor("#0D47A1"), 16),
            MailI18n::t(QString("✅ 선택한 메일 읽음으로 표시%1 (Mark selected as read)").arg(countSuffix),
                        QString("✅ Mark selected as read%1").arg(countSuffix),
                        QString("✅ 選択したメールを既読にする%1").arg(countSuffix)));
        connect(actMarkCheckedRead, &QAction::triggered, this, &MainWindow::onMarkCheckedMessagesRead);

        QMenu *moveSelectedMenu = menu.addMenu(
            MailIcons::actionIcon("folder_move", QColor("#0D47A1"), 16),
            MailI18n::t(QString("📁 선택한 메일 편지함 이동%1 (Move to Folder)").arg(countSuffix),
                        QString("📁 Move selected to folder%1").arg(countSuffix),
                        QString("📁 選択したメールをフォルダへ移動%1").arg(countSuffix)));
        populateMoveToFolderMenu(moveSelectedMenu, true);

        QAction *actMoveCheckedTrash = menu.addAction(
            MailIcons::actionIcon("delete", QColor("#DC2626"), 16),
            MailI18n::t(QString("🗑️ 선택한 메일 휴지통으로 이동%1 (Move selected to Trash)").arg(countSuffix),
                        QString("🗑️ Move selected to Trash%1").arg(countSuffix),
                        QString("🗑️ 選択したメールをゴミ箱へ移動%1").arg(countSuffix)));
        connect(actMoveCheckedTrash, &QAction::triggered, this, &MainWindow::onMoveCheckedMessagesToTrash);

        const bool allCheckedInView = (!m_filteredConvIndices.isEmpty() && checkedInCurrentView == m_filteredConvIndices.size());
        QAction *actToggleCheckAll = menu.addAction(
            allCheckedInView
                ? MailI18n::t("☐ 체크박스 전체 선택 해제 (Uncheck all)", "☐ Uncheck all in list", "☐ すべての選択を解除")
                : MailI18n::t("☑️ 현재 목록 전체 체크 선택 (Check all in list)", "☑️ Check all in list", "☑️ リスト内のすべてを選択"));
        connect(actToggleCheckAll, &QAction::triggered, this, [this, allCheckedInView]() {
            if (allCheckedInView) {
                for (int fIdx : m_filteredConvIndices) {
                    if (fIdx >= 0 && fIdx < m_conversations.size())
                        m_checkedConvIds.remove(m_conversations[fIdx].id);
                }
            } else {
                for (int fIdx : m_filteredConvIndices) {
                    if (fIdx >= 0 && fIdx < m_conversations.size())
                        m_checkedConvIds.insert(m_conversations[fIdx].id);
                }
            }
            if (m_convListWidget && m_convListWidget->viewport()) {
                m_convListWidget->viewport()->update();
            }
        });

        menu.addSeparator();

        QAction *actMarkAllRead = menu.addAction(
            MailIcons::actionIcon("unread_toggle", QColor("#4B5563"), 16),
            MailI18n::t("모두 읽음으로 표시 (Mark all as read)", "Mark all as read", "すべて既読にする"));
        connect(actMarkAllRead, &QAction::triggered, this, [this]() {
            onMarkFolderAllRead(m_selectedFolderId);
        });

        if (m_selectedFolderId == "trash" || m_selectedFolderId == "spam") {
            QAction *actRestoreSel = menu.addAction(
                MailIcons::actionIcon("folder_move", QColor("#059669"), 16),
                MailI18n::t("♻️ 선택한 메일 원래 편지함으로 복원 (Restore selected)",
                            "♻️ Restore selected message",
                            "♻️ 選択したメールを元のフォルダに復元"));
            connect(actRestoreSel, &QAction::triggered, this, &MainWindow::onRestoreCurrentFromTrash);

            QAction *actRestoreAll = menu.addAction(
                MailIcons::actionIcon("folder_move", QColor("#2563EB"), 16),
                MailI18n::t("♻️ 이 폴더의 메일 모두 복원 (Restore all)",
                            "♻️ Restore all messages in folder",
                            "♻️ すべてのメールを復元"));
            connect(actRestoreAll, &QAction::triggered, this, [this]() {
                onRestoreAllFromTrash(m_selectedFolderId);
            });

            QString emptyText = (m_selectedFolderId == "trash")
                ? MailI18n::t("🗑️ 휴지통 비우기 - 영구 삭제 (Empty Trash)", "🗑️ Empty Trash (Permanent)", "🗑️ ゴミ箱を空にする")
                : MailI18n::t("🗑️ 스팸함 비우기 - 영구 삭제 (Empty Spam)", "🗑️ Empty Spam (Permanent)", "🗑️ 迷惑メールを空にする");
            QAction *actEmptyFolder = menu.addAction(
                MailIcons::actionIcon("delete", QColor("#DC2626"), 16),
                emptyText);
            connect(actEmptyFolder, &QAction::triggered, this, [this]() {
                onEmptyFolder(m_selectedFolderId);
            });
        } else {
            QAction *actMoveAllToTrash = menu.addAction(
                MailIcons::actionIcon("delete", QColor("#D97706"), 16),
                MailI18n::t("🗑️ 현재 폴더 메일 모두 휴지통으로 이동 (Move all to Trash)",
                            "🗑️ Move all messages in folder to Trash",
                            "🗑️ 現在のフォルダのメールをすべてゴミ箱へ移動"));
            connect(actMoveAllToTrash, &QAction::triggered, this, [this]() {
                onMoveFolderAllToTrash(m_selectedFolderId);
            });
        }

        menu.addSeparator();

        auto addSortAction = [&](int mode, const QString &ko, const QString &en, const QString &ja) {
            QAction *act = menu.addAction(MailI18n::t(ko, en, ja));
            act->setCheckable(true);
            act->setChecked(m_mailSortMode == mode);
            connect(act, &QAction::triggered, this, [this, mode, ko]() {
                m_mailSortMode = mode;
                saveAccountsToDisk();
                refreshConversationList();
                statusBar()->showMessage(QString("메일 목록 정렬 변경: %1").arg(ko), 3000);
            });
        };
        addSortAction(0,
                      "⬇️ 정렬: 최신순 (Newest first)",
                      "⬇️ Sort: Newest first",
                      "⬇️ 並び替え: 新しい順");
        addSortAction(1,
                      "⬆️ 정렬: 오래된 순 (Oldest first)",
                      "⬆️ Sort: Oldest first",
                      "⬆️ 並び替え: 古い順");
        addSortAction(2,
                      "👤 정렬: 보낸사람순 (Sender A–Z)",
                      "👤 Sort: Sender (A–Z)",
                      "👤 並び替え: 差出人順");
        addSortAction(3,
                      "🔤 정렬: 제목순 (Subject A–Z)",
                      "🔤 Sort: Subject (A–Z)",
                      "🔤 並び替え: 件名順");
        addSortAction(4,
                      "📩 정렬: 안 읽은 메일 우선 (Unread first)",
                      "📩 Sort: Unread first",
                      "📩 並び替え: 未読優先");

        menu.addSeparator();

        QAction *actComfortable = menu.addAction(
            MailIcons::actionIcon("app_grid", QColor("#4B5563"), 15),
            MailI18n::t("기본 좌우 분할 (Comfortable layout)", "Comfortable layout", "標準レイアウト (Comfortable)"));
        actComfortable->setCheckable(true);
        actComfortable->setChecked(m_mailLayoutMode == 0);
        connect(actComfortable, &QAction::triggered, this, [this]() { onSwitchMailLayoutMode(0); });

        QAction *actHorizontal = menu.addAction(
            MailIcons::actionIcon("app_grid", QColor("#4B5563"), 15),
            MailI18n::t("상하 분할 (Horizontal layout)", "Horizontal layout", "水平分割レイアウト"));
        actHorizontal->setCheckable(true);
        actHorizontal->setChecked(m_mailLayoutMode == 1);
        connect(actHorizontal, &QAction::triggered, this, [this]() { onSwitchMailLayoutMode(1); });

        QAction *actFullscreen = menu.addAction(
            MailIcons::actionIcon("app_grid", QColor("#4B5563"), 15),
            MailI18n::t("전체 화면 목록 (Fullscreen layout)", "Fullscreen layout", "全画面レイアウト"));
        actFullscreen->setCheckable(true);
        actFullscreen->setChecked(m_mailLayoutMode == 2);
        connect(actFullscreen, &QAction::triggered, this, [this]() { onSwitchMailLayoutMode(2); });

        menu.addSeparator();

        QAction *actMinimal = menu.addAction(
            MailI18n::t("미니멀 레이아웃 (Minimal layout)\tCtrl+Shift+F", "Minimal layout\tCtrl+Shift+F", "ミニマルレイアウト\tCtrl+Shift+F"));
        actMinimal->setCheckable(true);
        actMinimal->setChecked(m_folderPaneWidget && !m_folderPaneWidget->isVisible());
        connect(actMinimal, &QAction::triggered, this, &MainWindow::onToggleMinimalLayout);

        menu.exec(btnMore->mapToGlobal(QPoint(0, btnMore->height() + 4)));
    });

    convHdrH->addWidget(btnCompose);
    convHdrH->addWidget(btnSearchToggle);
    convHdrH->addWidget(btnUnreadFilter);
    convHdrH->addWidget(btnRefresh);
    convHdrH->addWidget(btnMore);
    convV->addWidget(convHeader);

    // Search bar (toggled by Search icon)
    m_mailSearchEdit = new QLineEdit(convPane);
    m_mailSearchEdit->setPlaceholderText(MailI18n::t("🔍 제목, 보낸사람, 본문 검색...", "🔍 Search subject, sender, body...", "🔍 件名、差出人、本文を検索..."));
    m_mailSearchEdit->setClearButtonEnabled(true);
    m_mailSearchEdit->setVisible(false);
    m_mailSearchEdit->setStyleSheet("margin: 0px 12px 8px 12px; padding: 6px 10px; border: 1px solid #D1D5DB; border-radius: 5px; font-size: 12px;");
    convV->addWidget(m_mailSearchEdit);

    // Quick Restore & Empty Trash / Empty Spam Banner (shown automatically when viewing Trash or Spam)
    m_emptyTrashBanner = new QWidget(convPane);
    m_emptyTrashBanner->setStyleSheet("background-color: #FEF2F2; border-bottom: 1px solid #FECACA;");
    QHBoxLayout *bannerH = new QHBoxLayout(m_emptyTrashBanner);
    bannerH->setContentsMargins(8, 6, 8, 6);
    bannerH->setSpacing(5);

    m_restoreSelectedTrashBtn = new QPushButton(MailI18n::t("♻️ 선택 복원", "♻️ Restore", "♻️ 選択復元"), m_emptyTrashBanner);
    m_restoreSelectedTrashBtn->setCursor(Qt::PointingHandCursor);
    m_restoreSelectedTrashBtn->setToolTip(
        MailI18n::t("현재 선택한 메일을 원래 편지함(받은편지함/보낸편지함 등)으로 복원합니다",
                    "Restore the selected message to its original folder",
                    "選択したメールを元のフォルダに復元します"));
    m_restoreSelectedTrashBtn->setStyleSheet(
        "QPushButton { background: #059669; color: #FFFFFF; font-size: 11px; font-weight: 700; border: none; border-radius: 5px; padding: 6px 8px; }"
        "QPushButton:hover { background: #047857; }");
    connect(m_restoreSelectedTrashBtn, &QPushButton::clicked, this, &MainWindow::onRestoreCurrentFromTrash);

    m_restoreAllTrashBtn = new QPushButton(MailI18n::t("♻️ 전체 복원", "♻️ Restore All", "♻️ すべて復元"), m_emptyTrashBanner);
    m_restoreAllTrashBtn->setCursor(Qt::PointingHandCursor);
    m_restoreAllTrashBtn->setToolTip(
        MailI18n::t("휴지통의 모든 메일을 원래 편지함으로 일괄 복원합니다",
                    "Restore all messages in Trash to their original folders",
                    "ゴミ箱のすべてのメールを元のフォルダに一括復元します"));
    m_restoreAllTrashBtn->setStyleSheet(
        "QPushButton { background: #2563EB; color: #FFFFFF; font-size: 11px; font-weight: 700; border: none; border-radius: 5px; padding: 6px 8px; }"
        "QPushButton:hover { background: #1D4ED8; }");
    connect(m_restoreAllTrashBtn, &QPushButton::clicked, this, [this]() {
        onRestoreAllFromTrash(m_selectedFolderId);
    });

    m_emptyTrashBannerBtn = new QPushButton(MailI18n::t("🗑️ 비우기(영구삭제)", "🗑️ Empty", "🗑️ 空にする"), m_emptyTrashBanner);
    m_emptyTrashBannerBtn->setCursor(Qt::PointingHandCursor);
    m_emptyTrashBannerBtn->setToolTip(
        MailI18n::t("휴지통/스팸함의 메일을 영구 삭제합니다",
                    "Permanently delete all messages in Trash/Spam",
                    "ゴミ箱/迷惑メールのメールを完全に削除します"));
    m_emptyTrashBannerBtn->setStyleSheet(
        "QPushButton { background: #DC2626; color: #FFFFFF; font-size: 11px; font-weight: 700; border: none; border-radius: 5px; padding: 6px 8px; }"
        "QPushButton:hover { background: #B91C1C; }");
    connect(m_emptyTrashBannerBtn, &QPushButton::clicked, this, [this]() {
        onEmptyFolder(m_selectedFolderId);
    });

    bannerH->addWidget(m_restoreSelectedTrashBtn, 1);
    bannerH->addWidget(m_restoreAllTrashBtn, 1);
    bannerH->addWidget(m_emptyTrashBannerBtn, 1);
    m_emptyTrashBanner->setVisible(false);
    convV->addWidget(m_emptyTrashBanner);

    // Unread-only filter active banner (shown automatically when m_unreadOnlyFilter == true)
    m_unreadFilterBanner = new QWidget(convPane);
    m_unreadFilterBanner->setStyleSheet("background-color: #EFF6FF; border-bottom: 1px solid #BFDBFE;");
    QHBoxLayout *unreadBanH = new QHBoxLayout(m_unreadFilterBanner);
    unreadBanH->setContentsMargins(10, 6, 8, 6);
    unreadBanH->setSpacing(6);

    m_unreadFilterBannerLbl = new QLabel(
        MailI18n::t("📩 읽지 않은 메일만 보기", "📩 Unread messages only", "📩 未読メールのみ表示"),
        m_unreadFilterBanner);
    m_unreadFilterBannerLbl->setStyleSheet("font-size: 11.5px; font-weight: 700; color: #1E40AF; border: none; background: transparent;");

    QPushButton *btnClearUnreadFilter = new QPushButton(
        MailI18n::t("전체 보기 ✕", "Show All ✕", "すべて表示 ✕"),
        m_unreadFilterBanner);
    btnClearUnreadFilter->setCursor(Qt::PointingHandCursor);
    btnClearUnreadFilter->setStyleSheet(
        "QPushButton { background: #FFFFFF; color: #0D47A1; font-size: 11px; font-weight: 700; border: 1px solid #93C5FD; border-radius: 4px; padding: 3px 8px; }"
        "QPushButton:hover { background: #DBEAFE; }");
    connect(btnClearUnreadFilter, &QPushButton::clicked, this, [this]() {
        m_unreadOnlyFilter = false;
        m_currentMailPage = 1;
        if (m_btnUnreadFilter) {
            QSignalBlocker b(m_btnUnreadFilter);
            m_btnUnreadFilter->setChecked(false);
        }
        refreshConversationList();
    });

    unreadBanH->addWidget(m_unreadFilterBannerLbl, 1);
    unreadBanH->addWidget(btnClearUnreadFilter);
    m_unreadFilterBanner->setVisible(false);
    convV->addWidget(m_unreadFilterBanner);

    connect(btnSearchToggle, &QToolButton::toggled, this, [this](bool checked) {
        m_mailSearchEdit->setVisible(checked);
        if (checked) {
            m_mailSearchEdit->setFocus();
        } else {
            m_mailSearchEdit->clear();
        }
    });
    connect(m_mailSearchEdit, &QLineEdit::textChanged, this, [this](const QString &text) {
        m_mailSearchQuery = text;
        m_currentMailPage = 1;
        refreshConversationList();
    });
    connect(btnUnreadFilter, &QToolButton::toggled, this, [this](bool checked) {
        m_unreadOnlyFilter = checked;
        m_currentMailPage = 1;
        refreshConversationList();
    });
    connect(btnRefresh, &QToolButton::clicked, this, &MainWindow::onRefreshAndSyncCurrentAccount);

    // Conversation List (QStyledItemDelegate 60FPS 가상화 뷰포트 + 자동 스크롤 점프 방지)
    m_convListWidget = new QListWidget(convPane);
    m_convListWidget->setFrameShape(QFrame::NoFrame);
    m_convListWidget->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_convListWidget->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    m_convListWidget->setUniformItemSizes(true);
    m_convListWidget->setAutoScroll(false);
    m_convListWidget->setSpacing(0);
    m_convListWidget->setStyleSheet("QListWidget { background: #FFFFFF; border: none; outline: none; }"
                                    "QListWidget::item { border: none; padding: 0px; margin: 0px; }"
                                    "QListWidget::item:selected { background: transparent; }");
    m_convListDelegate = new ConversationListDelegate(&m_conversations, m_palette, m_convListWidget);
    m_convListDelegate->setPrivacyBlur(false);
    m_convListDelegate->setCheckedConvIds(&m_checkedConvIds);
    m_convListWidget->setItemDelegate(m_convListDelegate);
    m_convListWidget->installEventFilter(this);
    if (m_convListWidget->viewport()) {
        m_convListWidget->viewport()->installEventFilter(this);
    }
    m_convListWidget->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_convListWidget, &QWidget::customContextMenuRequested, this, [this, populateMoveToFolderMenu](const QPoint &pos) {
        QListWidgetItem *clickedItem = m_convListWidget->itemAt(pos);
        if (clickedItem) {
            int row = m_convListWidget->row(clickedItem);
            if (row >= 0 && row < m_convListWidget->count()) {
                m_convListWidget->setCurrentRow(row);
            }
        }
        if (m_selectedConvIndex < 0 || m_selectedConvIndex >= m_conversations.size())
            return;

        QMenu menu(this);
        menu.setStyleSheet(
            "QMenu { background-color: #FFFFFF; border: 1px solid #E5E7EB; border-radius: 8px; padding: 6px 4px; min-width: 220px; }"
            "QMenu::item { padding: 7px 18px 7px 14px; font-size: 12.5px; color: #1F2937; border-radius: 5px; }"
            "QMenu::item:selected { background-color: #EFF6FF; color: #0D47A1; font-weight: 600; }"
            "QMenu::separator { height: 1px; background: #E5E7EB; margin: 4px 6px; }");

        const ConversationItem &conv = m_conversations[m_selectedConvIndex];
        if (conv.folderId == "trash" || conv.folderId == "spam" || m_selectedFolderId == "trash" || m_selectedFolderId == "spam") {
            QString origFolderTitle = MailI18n::folderTitle(conv.previousFolderId.isEmpty() ? "inbox" : conv.previousFolderId);
            QAction *actRestore = menu.addAction(
                MailIcons::actionIcon("folder_move", QColor("#059669"), 16),
                MailI18n::t(QString("♻️ '%1'(으)로 메일 복원 (Restore)").arg(origFolderTitle),
                            QString("♻️ Restore to '%1'").arg(origFolderTitle),
                            QString("♻️ '%1' に復元").arg(origFolderTitle)));
            connect(actRestore, &QAction::triggered, this, &MainWindow::onRestoreCurrentFromTrash);

            QMenu *moveSubMenu = menu.addMenu(
                MailIcons::actionIcon("folder_move", QColor("#0D47A1"), 16),
                MailI18n::t("📁 편지함 이동 (Move to Folder)", "📁 Move to Folder", "📁 フォルダへ移動"));
            populateMoveToFolderMenu(moveSubMenu, true);

            QAction *actRestoreAll = menu.addAction(
                MailIcons::actionIcon("folder_move", QColor("#2563EB"), 16),
                MailI18n::t("♻️ 휴지통 메일 전체 복원 (Restore All)", "♻️ Restore All", "♻️ すべて復元"));
            connect(actRestoreAll, &QAction::triggered, this, [this]() {
                onRestoreAllFromTrash(m_selectedFolderId);
            });

            menu.addSeparator();
            QAction *actPermDel = menu.addAction(
                MailIcons::actionIcon("delete", QColor("#DC2626"), 16),
                MailI18n::t("🗑️ 이 메일 영구 삭제 (Delete Permanently)", "🗑️ Delete Permanently", "🗑️ 完全に削除"));
            connect(actPermDel, &QAction::triggered, this, &MainWindow::onDeleteCurrent);
        } else {
            QAction *actLargeView = menu.addAction(
                MailI18n::t("🗖 메일 본문 크게 보기 (Open in Large Window)", "🗖 Open in Large Window", "🗖 メール本文を大きく表示"));
            connect(actLargeView, &QAction::triggered, this, &MainWindow::onOpenLargeMailViewerDialog);
            menu.addSeparator();

            QAction *actReply = menu.addAction(
                MailIcons::actionIcon("reply", QColor("#374151"), 16),
                MailI18n::t("↩️ 답장 (Reply)", "↩️ Reply", "↩️ 返信"));
            connect(actReply, &QAction::triggered, this, [this]() { onReplyCurrent(false); });

            QAction *actForward = menu.addAction(
                MailIcons::actionIcon("forward", QColor("#374151"), 16),
                MailI18n::t("➡️ 전달 (Forward)", "➡️ Forward", "➡️ 転送"));
            connect(actForward, &QAction::triggered, this, &MainWindow::onForwardCurrent);

            menu.addSeparator();
            QAction *actUnread = menu.addAction(
                MailIcons::actionIcon("unread_toggle", QColor("#374151"), 16),
                MailI18n::t("📩 읽음/읽지 않음 전환 (Toggle Read/Unread)", "📩 Toggle Read/Unread", "📩 既読/未読の切り替え"));
            connect(actUnread, &QAction::triggered, this, &MainWindow::onToggleUnreadCurrent);

            QAction *actStar = menu.addAction(
                MailIcons::folderIcon(FolderKind::Starred, QColor("#F59E0B"), 16),
                MailI18n::t("⭐ 별표 표시 전환 (Toggle Star)", "⭐ Toggle Star", "⭐ スターの切り替え"));
            connect(actStar, &QAction::triggered, this, &MainWindow::onToggleStarCurrent);

            QAction *actImportant = menu.addAction(
                MailIcons::folderIcon(FolderKind::Important, QColor("#E11D48"), 16),
                MailI18n::t("❗ 중요 메일 표시 전환 (Toggle Important)", "❗ Toggle Important", "❗ 重要マークの切り替え"));
            connect(actImportant, &QAction::triggered, this, &MainWindow::onToggleImportantCurrent);

            menu.addSeparator();
            QMenu *moveSubMenu = menu.addMenu(
                MailIcons::actionIcon("folder_move", QColor("#0D47A1"), 16),
                MailI18n::t("📁 편지함 이동 (Move to Folder)", "📁 Move to Folder", "📁 フォルダへ移動"));
            populateMoveToFolderMenu(moveSubMenu, true);

            QAction *actArchive = menu.addAction(
                MailIcons::folderIcon(FolderKind::Archive, QColor("#374151"), 16),
                MailI18n::t("📥 보관함으로 이동 (Archive)", "📥 Archive", "📥 アーカイブ"));
            connect(actArchive, &QAction::triggered, this, &MainWindow::onArchiveCurrent);

            menu.addSeparator();
            QAction *actTrash = menu.addAction(
                MailIcons::actionIcon("delete", QColor("#DC2626"), 16),
                MailI18n::t("🗑️ 휴지통으로 이동 (Move to Trash)", "🗑️ Move to Trash", "🗑️ ゴミ箱へ移動"));
            connect(actTrash, &QAction::triggered, this, &MainWindow::onDeleteCurrent);
        }

        menu.exec(m_convListWidget->viewport()->mapToGlobal(pos));
    });
    connect(m_convListWidget, &QListWidget::currentRowChanged, this, &MainWindow::onConversationRowSelected);
    connect(m_convListWidget, &QListWidget::itemDoubleClicked, this, [this](QListWidgetItem *item) {
        if (item && m_convListWidget) {
            const int row = m_convListWidget->row(item);
            if (row >= 0 && row < m_filteredConvIndices.size()) {
                displayConversation(row, true);
            }
        }
        onOpenLargeMailViewerDialog();
    });
    convV->addWidget(m_convListWidget, 1);

    // ------------------------------------------------------------------------
    // 하단 페이지네이션 바 (« ‹ [1] 2 3 4 5 › »)
    // ------------------------------------------------------------------------
    m_mailPaginationBar = new QWidget(convPane);
    m_mailPaginationBar->setObjectName("mailPaginationBar");
    m_mailPaginationBar->setFixedHeight(42);
    m_mailPaginationBar->setStyleSheet(
        "QWidget#mailPaginationBar { background-color: #FFFFFF; border-top: 1px solid #E5E7EB; }");
    QHBoxLayout *pageBarH = new QHBoxLayout(m_mailPaginationBar);
    pageBarH->setContentsMargins(6, 4, 6, 4);
    pageBarH->setSpacing(4);
    pageBarH->addStretch(1);

    auto makePageArrowBtn = [this](const QString &sym, const QString &tip) -> QToolButton * {
        QToolButton *b = new QToolButton(m_mailPaginationBar);
        b->setText(sym);
        b->setToolTip(tip);
        b->setFixedSize(28, 30);
        b->setCursor(Qt::PointingHandCursor);
        b->setStyleSheet(
            "QToolButton { border: none; background: transparent; color: #6B7280; font-size: 16px; font-weight: 400; border-radius: 6px; }"
            "QToolButton:hover:enabled { background-color: #F3F4F6; color: #111827; }"
            "QToolButton:disabled { color: #D1D5DB; }");
        return b;
    };

    m_pageFirstBlockBtn = makePageArrowBtn(
        QStringLiteral("«"),
        MailI18n::t("이전 5페이지 묶음 (Previous 5 pages)", "Previous 5 pages", "前の5ページ"));
    m_pagePrevBtn = makePageArrowBtn(
        QStringLiteral("‹"),
        MailI18n::t("이전 페이지 (Previous page)", "Previous page", "前のページ"));
    pageBarH->addWidget(m_pageFirstBlockBtn);
    pageBarH->addWidget(m_pagePrevBtn);

    m_pageNumBtns.clear();
    for (int i = 0; i < 5; ++i) {
        QPushButton *numBtn = new QPushButton(QString::number(i + 1), m_mailPaginationBar);
        numBtn->setFixedSize(32, 30);
        numBtn->setCursor(Qt::PointingHandCursor);
        connect(numBtn, &QPushButton::clicked, this, [this, numBtn]() {
            bool ok = false;
            int targetPage = numBtn->property("pageNumber").toInt(&ok);
            if (!ok || targetPage < 1) {
                targetPage = numBtn->text().toInt(&ok);
            }
            if (ok && targetPage >= 1) {
                onSelectMailPage(targetPage);
            }
        });
        m_pageNumBtns.append(numBtn);
        pageBarH->addWidget(numBtn);
    }

    m_pageNextBtn = makePageArrowBtn(
        QStringLiteral("›"),
        MailI18n::t("다음 페이지 (Next page)", "Next page", "次のページ"));
    m_pageLastBlockBtn = makePageArrowBtn(
        QStringLiteral("»"),
        MailI18n::t("다음 5페이지 묶음 (Next 5 pages)", "Next 5 pages", "次の5ページ"));
    pageBarH->addWidget(m_pageNextBtn);
    pageBarH->addWidget(m_pageLastBlockBtn);
    pageBarH->addStretch(1);

    connect(m_pageFirstBlockBtn, &QToolButton::clicked, this, [this]() {
        const int blockStart = ((m_currentMailPage - 1) / 5) * 5 + 1;
        const int target = (blockStart > 1) ? (blockStart - 5) : 1;
        onSelectMailPage(qMax(1, target));
    });
    connect(m_pagePrevBtn, &QToolButton::clicked, this, [this]() {
        if (m_currentMailPage > 1) {
            onSelectMailPage(m_currentMailPage - 1);
        }
    });
    connect(m_pageNextBtn, &QToolButton::clicked, this, [this]() {
        if (m_currentMailPage < m_totalMailPages) {
            onSelectMailPage(m_currentMailPage + 1);
        }
    });
    connect(m_pageLastBlockBtn, &QToolButton::clicked, this, [this]() {
        const int blockStart = ((m_currentMailPage - 1) / 5) * 5 + 1;
        const int nextBlockStart = blockStart + 5;
        const int target = (nextBlockStart <= m_totalMailPages) ? nextBlockStart : m_totalMailPages;
        onSelectMailPage(target);
    });

    convV->addWidget(m_mailPaginationBar);
    updateMailPaginationBar();

    m_mailSplitLayout->addWidget(convPane);

    // ------------------------------------------------------------------------
    // B. Center Message Reading Pane
    // ------------------------------------------------------------------------
    QWidget *readingPane = new QWidget(mailSplitContainer);
    readingPane->setObjectName("readingPane");
    m_readingPaneWidget = readingPane;
    readingPane->setMinimumWidth(520);
    readingPane->setStyleSheet("QWidget#readingPane { background-color: #FFFFFF; }");
    QVBoxLayout *readV = new QVBoxLayout(readingPane);
    readV->setContentsMargins(14, 8, 14, 12);
    readV->setSpacing(8);

    // Undo Send Countdown Banner (shown during Undo Send delay window after clicking Send)
    m_undoSendBanner = new QWidget(readingPane);
    m_undoSendBanner->setObjectName("undoSendBanner");
    m_undoSendBanner->setStyleSheet(
        "QWidget#undoSendBanner { background-color: #FFFBEB; border: 1px solid #F59E0B; border-radius: 7px; }");
    QHBoxLayout *undoBanH = new QHBoxLayout(m_undoSendBanner);
    undoBanH->setContentsMargins(14, 8, 12, 8);
    undoBanH->setSpacing(10);

    m_undoSendBannerLbl = new QLabel(m_undoSendBanner);
    m_undoSendBannerLbl->setStyleSheet("color: #92400E; font-size: 12.5px; font-weight: 700; background: transparent; border: none;");

    m_undoSendCancelBtn = new QPushButton(
        MailI18n::t("↩️ 발송 취소 (Undo Send)", "↩️ Undo Send", "↩️ 送信を取り消す"),
        m_undoSendBanner);
    m_undoSendCancelBtn->setCursor(Qt::PointingHandCursor);
    m_undoSendCancelBtn->setToolTip(MailI18n::t("발송을 취소하고 작성 창을 다시 열어 내용을 수정합니다",
                                                "Cancel sending and reopen compose window",
                                                "送信を取り消して作成画面を再表示します"));
    m_undoSendCancelBtn->setStyleSheet(
        "QPushButton { background-color: #DC2626; color: #FFFFFF; font-size: 12px; font-weight: 700; border: none; border-radius: 5px; padding: 5px 12px; }"
        "QPushButton:hover { background-color: #B91C1C; }");
    connect(m_undoSendCancelBtn, &QPushButton::clicked, this, &MainWindow::cancelPendingSendAndReopen);

    m_undoSendNowBtn = new QPushButton(
        MailI18n::t("🚀 지금 바로 발송", "🚀 Send Now", "🚀 今すぐ送信"),
        m_undoSendBanner);
    m_undoSendNowBtn->setCursor(Qt::PointingHandCursor);
    m_undoSendNowBtn->setToolTip(MailI18n::t("대기 시간을 건너뛰고 즉시 메일을 발송합니다",
                                             "Skip delay and send immediately",
                                             "待機時間をスキップして直ちに送信します"));
    m_undoSendNowBtn->setStyleSheet(
        "QPushButton { background-color: #0D47A1; color: #FFFFFF; font-size: 12px; font-weight: 700; border: none; border-radius: 5px; padding: 5px 12px; }"
        "QPushButton:hover { background-color: #1558BC; }");
    connect(m_undoSendNowBtn, &QPushButton::clicked, this, &MainWindow::commitPendingSend);

    undoBanH->addWidget(m_undoSendBannerLbl, 1);
    undoBanH->addWidget(m_undoSendCancelBtn);
    undoBanH->addWidget(m_undoSendNowBtn);
    m_undoSendBanner->setVisible(false);
    readV->addWidget(m_undoSendBanner);

    if (!m_undoSendTimer) {
        m_undoSendTimer = new QTimer(this);
        m_undoSendTimer->setInterval(1000);
        connect(m_undoSendTimer, &QTimer::timeout, this, [this]() {
            if (!m_pendingSend.active) {
                m_undoSendTimer->stop();
                if (m_undoSendBanner)
                    m_undoSendBanner->setVisible(false);
                return;
            }
            m_pendingSend.remainingSec--;
            if (m_pendingSend.remainingSec <= 0) {
                commitPendingSend();
            } else {
                updateUndoSendBannerText();
            }
        });
    }

    // Scheduled Send / Draft Action Banner (shown when viewing a Scheduled or Draft email in Drafts)
    m_scheduledMailBanner = new QWidget(readingPane);
    m_scheduledMailBanner->setObjectName("scheduledMailBanner");
    m_scheduledMailBanner->setStyleSheet(
        "QWidget#scheduledMailBanner { background-color: #EFF6FF; border: 1px solid #93C5FD; border-radius: 7px; }");
    QHBoxLayout *schedBanH = new QHBoxLayout(m_scheduledMailBanner);
    schedBanH->setContentsMargins(14, 8, 12, 8);
    schedBanH->setSpacing(8);

    m_scheduledMailBannerLbl = new QLabel(m_scheduledMailBanner);
    m_scheduledMailBannerLbl->setStyleSheet("color: #1E3A8A; font-size: 12.5px; font-weight: 700; background: transparent; border: none;");

    m_scheduledRescheduleBtn = new QPushButton(
        MailI18n::t("🕒 예약 시간 변경", "🕒 Reschedule", "🕒 予約時刻変更"),
        m_scheduledMailBanner);
    m_scheduledRescheduleBtn->setCursor(Qt::PointingHandCursor);
    m_scheduledRescheduleBtn->setToolTip(MailI18n::t("예약 발송 날짜 및 시각을 수정합니다",
                                                     "Change scheduled send date & time",
                                                     "予約送信の日時を変更します"));
    m_scheduledRescheduleBtn->setStyleSheet(
        "QPushButton { background-color: #FFFFFF; color: #0D47A1; font-size: 11.5px; font-weight: 700; border: 1px solid #93C5FD; border-radius: 5px; padding: 5px 10px; }"
        "QPushButton:hover { background-color: #DBEAFE; }");
    connect(m_scheduledRescheduleBtn, &QPushButton::clicked, this, &MainWindow::onRescheduleCurrentMail);

    m_scheduledEditOrCancelBtn = new QPushButton(
        MailI18n::t("✏️ 내용 수정 / 예약 취소", "✏️ Edit / Cancel Schedule", "✏️ 内容編集 / 予約取消"),
        m_scheduledMailBanner);
    m_scheduledEditOrCancelBtn->setCursor(Qt::PointingHandCursor);
    m_scheduledEditOrCancelBtn->setToolTip(MailI18n::t("예약을 취소(임시보관함 유지)하고 편지 쓰기 창에서 내용을 수정합니다",
                                                       "Cancel schedule and edit message in compose window",
                                                       "予約を取り消して作成画面で編集します"));
    m_scheduledEditOrCancelBtn->setStyleSheet(
        "QPushButton { background-color: #FEF3C7; color: #92400E; font-size: 11.5px; font-weight: 700; border: 1px solid #FCD34D; border-radius: 5px; padding: 5px 10px; }"
        "QPushButton:hover { background-color: #FDE68A; }");
    connect(m_scheduledEditOrCancelBtn, &QPushButton::clicked, this, &MainWindow::onEditOrCancelScheduledCurrentMail);

    m_scheduledSendNowBtn = new QPushButton(
        MailI18n::t("🚀 지금 바로 발송", "🚀 Send Now", "🚀 今すぐ送信"),
        m_scheduledMailBanner);
    m_scheduledSendNowBtn->setCursor(Qt::PointingHandCursor);
    m_scheduledSendNowBtn->setToolTip(MailI18n::t("예약 시각을 기다리지 않고 즉시 발송합니다",
                                                  "Send this message immediately",
                                                  "予約時刻を待たずに今すぐ送信します"));
    m_scheduledSendNowBtn->setStyleSheet(
        "QPushButton { background-color: #0D47A1; color: #FFFFFF; font-size: 11.5px; font-weight: 700; border: none; border-radius: 5px; padding: 5px 12px; }"
        "QPushButton:hover { background-color: #1558BC; }");
    connect(m_scheduledSendNowBtn, &QPushButton::clicked, this, &MainWindow::onSendNowScheduledCurrentMail);

    schedBanH->addWidget(m_scheduledMailBannerLbl, 1);
    schedBanH->addWidget(m_scheduledRescheduleBtn);
    schedBanH->addWidget(m_scheduledEditOrCancelBtn);
    schedBanH->addWidget(m_scheduledSendNowBtn);
    m_scheduledMailBanner->setVisible(false);
    readV->addWidget(m_scheduledMailBanner);

    if (!m_scheduledSendTimer) {
        m_scheduledSendTimer = new QTimer(this);
        m_scheduledSendTimer->setInterval(10000); // 10초마다 예약 발송 시각 도래 여부 점검
        connect(m_scheduledSendTimer, &QTimer::timeout, this, &MainWindow::checkScheduledSends);
        m_scheduledSendTimer->start();
    }

    // Top 11-icon Action Toolbar (Archive, Trash, Spam, Unread | Reply, Reply All, Forward | Mail, Move, Star, More)
    QWidget *actionToolbar = new QWidget(readingPane);
    actionToolbar->setFixedHeight(40);
    QHBoxLayout *actH = new QHBoxLayout(actionToolbar);
    actH->setContentsMargins(0, 0, 0, 0);
    actH->setSpacing(3);

    auto makeReadActionBtn = [actionToolbar](const QIcon &ic, const QString &tip) {
        QToolButton *b = new QToolButton(actionToolbar);
        b->setFixedSize(28, 28);
        b->setCursor(Qt::PointingHandCursor);
        b->setToolTip(tip);
        b->setIcon(ic);
        b->setIconSize(QSize(16, 16));
        b->setStyleSheet("QToolButton { border: none; background: transparent; border-radius: 4px; }"
                         "QToolButton:hover { background-color: #F3F4F6; }");
        return b;
    };

    QColor icCol("#6B7280");
    QToolButton *actArchive  = makeReadActionBtn(MailIcons::folderIcon(FolderKind::Archive, icCol, 17), MailI18n::t("보관 (Archive)", "Archive", "アーカイブ"));
    QToolButton *actTrash    = makeReadActionBtn(MailIcons::folderIcon(FolderKind::Trash, icCol, 17), MailI18n::t("삭제 (Trash)", "Delete (Trash)", "削除 (ゴミ箱)"));
    QToolButton *actSpam     = makeReadActionBtn(MailIcons::folderIcon(FolderKind::Spam, icCol, 17), MailI18n::t("스팸 신고 (Spam)", "Report Spam", "迷惑メール報告"));
    QToolButton *actUnread   = makeReadActionBtn(MailIcons::folderIcon(FolderKind::Inbox, icCol, 17), MailI18n::t("읽지 않음 표시 (Mark Unread)", "Mark Unread", "未読にする"));
    QToolButton *actReply    = makeReadActionBtn(MailIcons::actionIcon("reply", icCol, 17), MailI18n::t("답장 (Reply)", "Reply", "返信"));
    QToolButton *actReplyAll = makeReadActionBtn(MailIcons::actionIcon("reply_all", icCol, 17), MailI18n::t("전체 답장 (Reply All)", "Reply All", "全員に返信"));
    QToolButton *actForward  = makeReadActionBtn(MailIcons::actionIcon("forward", icCol, 17), MailI18n::t("전달 (Forward)", "Forward", "転送"));
    QToolButton *actMove     = makeReadActionBtn(MailIcons::actionIcon("folder_move", icCol, 17), MailI18n::t("폴더 이동 / 원래 편지함 복원 (Move / Restore)", "Move / Restore", "フォルダ移動 / 復元"));
    QToolButton *actStar     = makeReadActionBtn(MailIcons::folderIcon(FolderKind::Starred, icCol, 17), MailI18n::t("별표 표시 (Star)", "Toggle Star", "スター"));
    m_actStarBtn             = actStar;
    QToolButton *actMore     = makeReadActionBtn(MailIcons::actionIcon("more", icCol, 17), MailI18n::t("더보기 (More)", "More", "その他"));

    m_actRestoreBtn = new QToolButton(actionToolbar);
    m_actRestoreBtn->setText(MailI18n::t("♻️ 원래 편지함으로 복원", "♻️ Restore", "♻️ 元のフォルダに復元"));
    m_actRestoreBtn->setCursor(Qt::PointingHandCursor);
    m_actRestoreBtn->setToolTip(MailI18n::t("선택한 휴지통/스팸 메일을 원래 편지함(받은편지함/보낸편지함 등)으로 복원합니다",
                                            "Restore selected message from Trash/Spam to its original folder",
                                            "選択したメールを元のフォルダに復元します"));
    m_actRestoreBtn->setStyleSheet(
        "QToolButton { background-color: #ECFDF5; color: #059669; border: 1px solid #A7F3D0; border-radius: 5px; padding: 4px 10px; font-size: 11.5px; font-weight: 700; }"
        "QToolButton:hover { background-color: #D1FAE5; }");
    m_actRestoreBtn->setVisible(false);
    connect(m_actRestoreBtn, &QToolButton::clicked, this, &MainWindow::onRestoreCurrentFromTrash);
    connect(actMove, &QToolButton::clicked, this, [this, actMove, populateMoveToFolderMenu]() {
        if (m_checkedConvIds.isEmpty() && (m_selectedConvIndex < 0 || m_selectedConvIndex >= m_conversations.size()))
            return;
        QMenu menu(this);
        populateMoveToFolderMenu(&menu, true);
        menu.exec(actMove->mapToGlobal(QPoint(0, actMove->height() + 4)));
    });

    connect(actArchive,  &QToolButton::clicked, this, &MainWindow::onArchiveCurrent);
    connect(actTrash,    &QToolButton::clicked, this, &MainWindow::onDeleteCurrent);
    connect(actSpam,     &QToolButton::clicked, this, &MainWindow::onSpamCurrent);
    connect(actUnread,   &QToolButton::clicked, this, &MainWindow::onToggleUnreadCurrent);
    connect(actReply,    &QToolButton::clicked, this, [this]() { onReplyCurrent(false); });
    connect(actReplyAll, &QToolButton::clicked, this, [this]() { onReplyCurrent(true); });
    connect(actForward,  &QToolButton::clicked, this, &MainWindow::onForwardCurrent);
    connect(actStar,     &QToolButton::clicked, this, &MainWindow::onToggleStarCurrent);
    connect(actMore,     &QToolButton::clicked, this, [this, actMore, populateMoveToFolderMenu]() {
        if (m_selectedConvIndex < 0 || m_selectedConvIndex >= m_conversations.size())
            return;
        QMenu menu(this);
        menu.setStyleSheet(
            "QMenu { background-color: #FFFFFF; border: 1px solid #E5E7EB; border-radius: 6px; padding: 6px 4px; }"
            "QMenu::item { padding: 7px 18px; font-size: 12px; color: #1F2937; border-radius: 4px; }"
            "QMenu::item:selected { background-color: #EFF6FF; color: #0D47A1; font-weight: 600; }");
        QAction *aReply = menu.addAction(MailI18n::t("↩️ 답장 (Reply)", "↩️ Reply", "↩️ 返信"));
        QAction *aReplyAll = menu.addAction(MailI18n::t("↩️ 전체 답장 (Reply All)", "↩️ Reply All", "↩️ 全員に返信"));
        QAction *aFwd = menu.addAction(MailI18n::t("➡️ 전달 (Forward)", "➡️ Forward", "➡️ 転送"));
        menu.addSeparator();
        QAction *aUnread = menu.addAction(MailI18n::t("📩 읽음/읽지 않음 전환 (Toggle Read/Unread)", "📩 Toggle Read/Unread", "📩 既読/未読の切り替え"));
        QAction *aStar = menu.addAction(MailI18n::t("⭐ 별표 표시 전환 (Toggle Star)", "⭐ Toggle Star", "⭐ スターの切り替え"));
        QAction *aImportant = menu.addAction(MailI18n::t("❗ 중요 메일 표시 전환 (Toggle Important)", "❗ Toggle Important", "❗ 重要マークの切り替え"));
        connect(aImportant, &QAction::triggered, this, &MainWindow::onToggleImportantCurrent);

        menu.addSeparator();
        QMenu *moveSubMenu = menu.addMenu(
            MailIcons::actionIcon("folder_move", QColor("#0D47A1"), 16),
            MailI18n::t("📁 편지함 이동 (Move to Folder)", "📁 Move to Folder", "📁 フォルダへ移動"));
        populateMoveToFolderMenu(moveSubMenu, false);

        const ConversationItem &curConv = m_conversations[m_selectedConvIndex];
        const bool isOutgoingOrDraft = (curConv.folderId == QStringLiteral("sent")
                                        || curConv.folderId == QStringLiteral("drafts")
                                        || (!curConv.messages.isEmpty()
                                            && (curConv.latestMessage().senderName.startsWith(QStringLiteral("To:"), Qt::CaseInsensitive)
                                                || curConv.latestMessage().senderName.startsWith(QStringLiteral("⏰ To:"), Qt::CaseInsensitive))));
        const QString unsubTarget = (isOutgoingOrDraft || curConv.messages.isEmpty())
                                        ? QString()
                                        : extractUnsubscribeTargetFromHtml(curConv.latestMessage().htmlBody);
        if (!isOutgoingOrDraft) {
            menu.addSeparator();
            if (!unsubTarget.isEmpty()) {
                QAction *aUnsub = menu.addAction(MailI18n::t("🔕 구독 취소 (Unsubscribe)", "🔕 Unsubscribe", "🔕 配信停止"));
                connect(aUnsub, &QAction::triggered, this, &MainWindow::onUnsubscribeCurrent);
            }
            QAction *aBlock = menu.addAction(MailI18n::t("🚫 발신자 차단 (Block sender)", "🚫 Block sender", "🚫 送信者をブロック"));
            connect(aBlock, &QAction::triggered, this, &MainWindow::onBlockSenderCurrent);
        }
        menu.addSeparator();
        QAction *aDel = menu.addAction(MailI18n::t("🗑️ 삭제 (Move to Trash)", "🗑️ Delete", "🗑️ 削除"));
        connect(aReply,    &QAction::triggered, this, [this]() { onReplyCurrent(false); });
        connect(aReplyAll, &QAction::triggered, this, [this]() { onReplyCurrent(true); });
        connect(aFwd,      &QAction::triggered, this, &MainWindow::onForwardCurrent);
        connect(aUnread,   &QAction::triggered, this, &MainWindow::onToggleUnreadCurrent);
        connect(aStar,     &QAction::triggered, this, &MainWindow::onToggleStarCurrent);
        connect(aDel,      &QAction::triggered, this, &MainWindow::onDeleteCurrent);
        menu.exec(actMore->mapToGlobal(QPoint(0, actMore->height() + 4)));
    });

    actH->addWidget(actArchive);
    actH->addWidget(actTrash);
    actH->addWidget(actSpam);
    actH->addWidget(actUnread);
    actH->addSpacing(4);
    actH->addWidget(actReply);
    actH->addWidget(actReplyAll);
    actH->addWidget(actForward);
    actH->addSpacing(4);
    actH->addWidget(actMove);
    actH->addWidget(actStar);
    actH->addWidget(actMore);
    actH->addWidget(m_actRestoreBtn);
    actH->addStretch(1);

    // 우측: 메일 본문 확대/축소 ([−] [100%] [+]) 및 [🗖 크게 보기] 버튼 (고정 크기 부여로 잘림 원천 방지)
    QString zoomSmallBtnStyle =
        "QPushButton { background: #F8FAFC; color: #1E293B; border: 1px solid #CBD5E1; border-radius: 5px; "
        "padding: 0px 4px; font-size: 12px; font-weight: 700; }"
        "QPushButton:hover { background: #EFF6FF; color: #0D47A1; border-color: #93C5FD; }";

    QPushButton *btnBodyZoomOut = new QPushButton("−", actionToolbar);
    btnBodyZoomOut->setFixedSize(28, 28);
    btnBodyZoomOut->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    btnBodyZoomOut->setCursor(Qt::PointingHandCursor);
    btnBodyZoomOut->setToolTip(MailI18n::t("메일 본문 글자·이미지 축소 (Zoom Out)", "Zoom Out Email Body", "本文を縮小"));
    btnBodyZoomOut->setStyleSheet(zoomSmallBtnStyle);
    connect(btnBodyZoomOut, &QPushButton::clicked, this, [this]() {
        applyReadingBodyZoom(m_readingBodyZoomPercent - 15);
    });

    m_readingZoomBadgeBtn = new QPushButton("100%", actionToolbar);
    m_readingZoomBadgeBtn->setFixedSize(50, 28);
    m_readingZoomBadgeBtn->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    m_readingZoomBadgeBtn->setCursor(Qt::PointingHandCursor);
    m_readingZoomBadgeBtn->setToolTip(MailI18n::t("클릭하면 본문 배율을 100% 기본 크기로 초기화합니다", "Click to reset body zoom to 100%", "クリックして100%にリセット"));
    m_readingZoomBadgeBtn->setStyleSheet(zoomSmallBtnStyle);
    connect(m_readingZoomBadgeBtn, &QPushButton::clicked, this, [this]() {
        applyReadingBodyZoom(100);
    });

    QPushButton *btnBodyZoomIn = new QPushButton("+", actionToolbar);
    btnBodyZoomIn->setFixedSize(28, 28);
    btnBodyZoomIn->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    btnBodyZoomIn->setCursor(Qt::PointingHandCursor);
    btnBodyZoomIn->setToolTip(MailI18n::t("메일 본문 글자·이미지 확대 (Zoom In)", "Zoom In Email Body", "本文を拡大"));
    btnBodyZoomIn->setStyleSheet(zoomSmallBtnStyle);
    connect(btnBodyZoomIn, &QPushButton::clicked, this, [this]() {
        applyReadingBodyZoom(m_readingBodyZoomPercent + 15);
    });

    m_readingLargeViewBtn = new QPushButton(
        MailI18n::t("🗖 크게 보기", "🗖 Large View", "🗖 大きく表示"),
        actionToolbar);
    m_readingLargeViewBtn->setFixedHeight(28);
    m_readingLargeViewBtn->setMinimumWidth(92);
    m_readingLargeViewBtn->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    m_readingLargeViewBtn->setCursor(Qt::PointingHandCursor);
    m_readingLargeViewBtn->setToolTip(
        MailI18n::t("이 메일 본문을 큰 독립 창(전체화면/확대 가능)으로 넓게 봅니다 (메일 목록 더블클릭으로도 열림)",
                    "Open this email in a large standalone reader window",
                    "このメールを大きな別ウィンドウで表示します"));
    m_readingLargeViewBtn->setStyleSheet(
        "QPushButton { background: #EFF6FF; color: #0D47A1; border: 1px solid #93C5FD; border-radius: 5px; "
        "padding: 2px 10px; font-size: 11.5px; font-weight: 700; }"
        "QPushButton:hover { background: #DBEAFE; border-color: #60A5FA; }");
    connect(m_readingLargeViewBtn, &QPushButton::clicked, this, &MainWindow::onOpenLargeMailViewerDialog);

    actH->addWidget(btnBodyZoomOut);
    actH->addWidget(m_readingZoomBadgeBtn);
    actH->addWidget(btnBodyZoomIn);
    actH->addSpacing(4);
    actH->addWidget(m_readingLargeViewBtn);
    readV->addWidget(actionToolbar);

    // Subject Line ("Confirm your email address to receive updates")
    m_readingSubjectLbl = new QLabel("Confirm your email address to receive updates", readingPane);
    m_readingSubjectLbl->setWordWrap(true);
    m_readingSubjectLbl->setStyleSheet("font-size: 21px; font-weight: 700; color: #1F2937; margin-top: 2px;");
    readV->addWidget(m_readingSubjectLbl);

    // Tag Row ("Inbox" chip + "+")
    QHBoxLayout *tagRow = new QHBoxLayout();
    tagRow->setContentsMargins(0, 2, 0, 8);
    tagRow->setSpacing(6);
    m_readingFolderChipLbl = new QLabel("Inbox", readingPane);
    m_readingFolderChipLbl->setStyleSheet("background-color: #F3F4F6; color: #4B5563; font-size: 10px; padding: 2px 8px; border-radius: 8px; border: 1px solid #E5E7EB;");
    QLabel *plusLbl = new QLabel("+", readingPane);
    plusLbl->setStyleSheet("color: #6B7280; font-size: 13px; font-weight: bold;");
    tagRow->addWidget(m_readingFolderChipLbl);
    tagRow->addWidget(plusLbl);
    tagRow->addStretch();
    readV->addLayout(tagRow);

    QFrame *sepLine = new QFrame(readingPane);
    sepLine->setFrameShape(QFrame::HLine);
    sepLine->setStyleSheet("color: #F1F5F9;");
    readV->addWidget(sepLine);

    // Sender Info Bar (Avatar + Sender/Recipient + [Unsubscribe] + [Block sender] + Timestamp + ⋮)
    QHBoxLayout *senderRow = new QHBoxLayout();
    senderRow->setContentsMargins(0, 6, 0, 6);
    senderRow->setSpacing(10);

    m_readingAvatarLbl = new QLabel(readingPane);
    m_readingAvatarLbl->setFixedSize(28, 28);
    senderRow->addWidget(m_readingAvatarLbl);

    m_readingSenderLbl = new QLabel("<b>SollunaMail Team</b> <span style='color:#6B7280;'>to 나 ⌄</span>", readingPane);
    m_readingSenderLbl->setStyleSheet("font-size: 12px; color: #1F2937;");
    senderRow->addWidget(m_readingSenderLbl);

    QString pillBtnStyle = R"(
        QPushButton {
            background-color: #FFFFFF;
            color: #1F2937;
            border: 1px solid #D1D5DB;
            border-radius: 4px;
            padding: 3px 10px;
            font-size: 11px;
            font-weight: 600;
        }
        QPushButton:hover {
            background-color: #F9FAFB;
            border-color: #9CA3AF;
        }
    )";

    m_unsubscribeBtn = new QPushButton("Unsubscribe", readingPane);
    m_unsubscribeBtn->setCursor(Qt::PointingHandCursor);
    m_unsubscribeBtn->setStyleSheet(pillBtnStyle);
    connect(m_unsubscribeBtn, &QPushButton::clicked, this, &MainWindow::onUnsubscribeCurrent);
    senderRow->addWidget(m_unsubscribeBtn);

    m_blockSenderBtn = new QPushButton("Block sender", readingPane);
    m_blockSenderBtn->setCursor(Qt::PointingHandCursor);
    m_blockSenderBtn->setStyleSheet(pillBtnStyle);
    connect(m_blockSenderBtn, &QPushButton::clicked, this, &MainWindow::onBlockSenderCurrent);
    senderRow->addWidget(m_blockSenderBtn);

    senderRow->addStretch();

    m_readingTimeLbl = new QLabel("Tue, 3 Nov, 오전 1:22", readingPane);
    m_readingTimeLbl->setStyleSheet("font-size: 11px; color: #6B7280;");
    senderRow->addWidget(m_readingTimeLbl);

    QToolButton *msgMoreBtn = makeReadActionBtn(
        MailIcons::actionIcon("more", icCol, 15),
        MailI18n::t("메시지 메뉴", "Message Menu", "メッセージメニュー"));
    connect(msgMoreBtn, &QToolButton::clicked, actMore, &QToolButton::click);
    senderRow->addWidget(msgMoreBtn);
    readV->addLayout(senderRow);

    // Attachment Bar (shown automatically when the selected message has attachments in Inbox / Sent / Drafts)
    m_readingAttachmentsBar = new QWidget(readingPane);
    m_readingAttachmentsBar->setObjectName("readingAttachmentsBar");
    m_readingAttachmentsBar->setStyleSheet(
        "QWidget#readingAttachmentsBar { background-color: #F8FAFC; border: 1px solid #CBD5E1; border-radius: 7px; }");
    m_readingAttachmentsLayout = new QHBoxLayout(m_readingAttachmentsBar);
    m_readingAttachmentsLayout->setContentsMargins(12, 7, 12, 7);
    m_readingAttachmentsLayout->setSpacing(8);
    m_readingAttachmentsBar->setVisible(false);
    readV->addWidget(m_readingAttachmentsBar);

    // Rich HTML Email Body Viewer (OS/Kit별 Chromium QWebEngineView 우선 적용 + QTextBrowser 호환)
    m_readingBodyBrowser = new QTextBrowser(readingPane);
    m_readingBodyBrowser->setFrameShape(QFrame::NoFrame);
    m_readingBodyBrowser->setOpenLinks(false);
    m_readingBodyBrowser->setStyleSheet("QTextBrowser { background: #FFFFFF; border: none; }");
    connect(m_readingBodyBrowser, &QTextBrowser::anchorClicked, this, [this](const QUrl &url) {
        handleEmailLinkActivated(url);
    });

#ifdef SOLLUNA_HAS_WEBENGINE
    m_readingWebView = new QWebEngineView(readingPane);
    SollunaEmailWebPage *webPage = new SollunaEmailWebPage(
        [this](const QUrl &url) { handleEmailLinkActivated(url); },
        m_readingWebView);
    SollunaPrivacyInterceptor *privacyInterceptor = new SollunaPrivacyInterceptor(
        [this]() { return m_privacySettings.privacyMode && m_privacySettings.blockPixelTracking; },
        webPage);
    webPage->setUrlRequestInterceptor(privacyInterceptor);
    m_readingWebView->setPage(webPage);
    if (QWebEngineSettings *ws = m_readingWebView->settings()) {
        ws->setAttribute(QWebEngineSettings::JavascriptEnabled, false);
        ws->setAttribute(QWebEngineSettings::LocalContentCanAccessRemoteUrls, true);
        ws->setAttribute(QWebEngineSettings::AutoLoadImages, true);
        ws->setAttribute(QWebEngineSettings::ErrorPageEnabled, false);
        ws->setAttribute(QWebEngineSettings::PluginsEnabled, false);
    }
    m_readingWebView->setContextMenuPolicy(Qt::DefaultContextMenu);
    m_readingBodyBrowser->setVisible(false);
    readV->addWidget(m_readingWebView, 1);
#else
    readV->addWidget(m_readingBodyBrowser, 1);
#endif

    m_mailSplitLayout->addWidget(readingPane, 1);
    outerH->addWidget(mailSplitContainer, 1);

    // ------------------------------------------------------------------------
    // C. Right Docked Calendar Day Sidebar (228px)
    // ------------------------------------------------------------------------
    m_rightCalendarSidebar = buildRightDockedCalendarSidebar();
    outerH->addWidget(m_rightCalendarSidebar);

    return page;
}

QWidget *MainWindow::buildRightDockedCalendarSidebar()
{
    QWidget *sidebar = new QWidget(this);
    sidebar->setObjectName("rightCalSidebar");
    sidebar->setFixedWidth(228);
    sidebar->setStyleSheet("QWidget#rightCalSidebar { background-color: #FFFFFF; border-left: 1px solid #E5E7EB; }");

    QVBoxLayout *v = new QVBoxLayout(sidebar);
    v->setContentsMargins(0, 0, 0, 0);
    v->setSpacing(0);

    // Top row: [Today]  <  >       [+] [Refresh] [⋮]
    QWidget *topBar = new QWidget(sidebar);
    topBar->setFixedHeight(44);
    QHBoxLayout *th = new QHBoxLayout(topBar);
    th->setContentsMargins(8, 6, 6, 6);
    th->setSpacing(3);

    m_rightCalTodayBtn = new QPushButton(MailI18n::t("오늘", "Today", "今日"), topBar);
    m_rightCalTodayBtn->setCursor(Qt::PointingHandCursor);
    m_rightCalTodayBtn->setStyleSheet("QPushButton { background: #FFFFFF; color: #374151; border: 1px solid #D1D5DB; border-radius: 4px; padding: 3px 9px; font-size: 11px; font-weight: 600; }"
                                      "QPushButton:hover { background: #F9FAFB; }");
    connect(m_rightCalTodayBtn, &QPushButton::clicked, this, [this]() {
        m_activeCalendarDate = QDate::currentDate();
        refreshCalendarViews();
        if (m_rightCalendarSidebar) {
            if (QScrollArea *sa = m_rightCalendarSidebar->findChild<QScrollArea *>()) {
                if (sa->verticalScrollBar()) {
                    sa->verticalScrollBar()->setValue(qMax(0, (QTime::currentTime().hour() - 3) * 54));
                }
            }
        }
    });

    QToolButton *btnPrevDay = new QToolButton(topBar);
    btnPrevDay->setText("‹");
    btnPrevDay->setStyleSheet("border: none; font-size: 16px; color: #374151; padding: 2px 6px;");
    connect(btnPrevDay, &QToolButton::clicked, this, [this]() {
        m_activeCalendarDate = m_activeCalendarDate.addDays(-1);
        refreshCalendarViews();
    });

    QToolButton *btnNextDay = new QToolButton(topBar);
    btnNextDay->setText("›");
    btnNextDay->setStyleSheet("border: none; font-size: 16px; color: #374151; padding: 2px 6px;");
    connect(btnNextDay, &QToolButton::clicked, this, [this]() {
        m_activeCalendarDate = m_activeCalendarDate.addDays(1);
        refreshCalendarViews();
    });

    QToolButton *btnAddEv = new QToolButton(topBar);
    btnAddEv->setIcon(MailIcons::actionIcon("add_event", QColor("#4B5563"), 16));
    btnAddEv->setToolTip(MailI18n::t("새 일정 추가 (New Event)", "New Event", "新しい予定を追加"));
    btnAddEv->setStyleSheet("border: none; padding: 4px;");
    connect(btnAddEv, &QToolButton::clicked, this, [this]() {
        onAddNewCalendarEvent(m_activeCalendarDate);
    });

    QToolButton *btnRefCal = new QToolButton(topBar);
    btnRefCal->setIcon(MailIcons::actionIcon("refresh", QColor("#4B5563"), 16));
    btnRefCal->setToolTip(MailI18n::t("Google Calendar 동기화", "Sync Calendar", "カレンダーを同期"));
    btnRefCal->setStyleSheet("border: none; padding: 4px;");
    connect(btnRefCal, &QToolButton::clicked, this, &MainWindow::onRefreshAndSyncCurrentAccount);

    QToolButton *btnMoreCal = new QToolButton(topBar);
    btnMoreCal->setIcon(MailIcons::actionIcon("more", QColor("#4B5563"), 16));
    btnMoreCal->setStyleSheet("border: none; padding: 4px;");

    th->addWidget(m_rightCalTodayBtn);
    th->addWidget(btnPrevDay);
    th->addWidget(btnNextDay);
    th->addStretch();
    th->addWidget(btnAddEv);
    th->addWidget(btnRefCal);
    th->addWidget(btnMoreCal);
    v->addWidget(topBar);

    // Date Label: "화, 11월 3, 2026 ⌄"
    QWidget *dateBar = new QWidget(sidebar);
    QVBoxLayout *dv = new QVBoxLayout(dateBar);
    dv->setContentsMargins(14, 2, 14, 6);
    dv->setSpacing(6);

    m_rightCalDateHeaderLbl = new QLabel("화, 11월 3, 2026  ⌄", dateBar);
    m_rightCalDateHeaderLbl->setStyleSheet("font-size: 14px; font-weight: 500; color: #374151; border: none;");
    dv->addWidget(m_rightCalDateHeaderLbl);

    m_rightCalAllDayLbl = new QLabel(MailI18n::t("종일 (All day)", "All day", "終日 (All day)"), dateBar);
    m_rightCalAllDayLbl->setStyleSheet("font-size: 10px; color: #9CA3AF; padding-top: 4px; padding-bottom: 4px; border-top: 1px solid #F1F5F9; border-bottom: 1px solid #F1F5F9;");
    dv->addWidget(m_rightCalAllDayLbl);
    v->addWidget(dateBar);

    // Scrollable Hourly Timeline with real-time red marker line
    QScrollArea *scroll = new QScrollArea(sidebar);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    m_dayTimelineWidget = new DayTimelineWidget(scroll);
    m_dayTimelineWidget->setMarkerTime(QTime::currentTime());
    connect(m_dayTimelineWidget, &DayTimelineWidget::slotClicked, this, [this](const QTime &) {
        onAddNewCalendarEvent(m_activeCalendarDate);
    });
    connect(m_dayTimelineWidget, &DayTimelineWidget::eventClicked, this, &MainWindow::onEditOrDeleteCalendarEvent);
    scroll->setWidget(m_dayTimelineWidget);
    v->addWidget(scroll, 1);

    // 시작 시 현재 시각(예: 18:38)이 타임라인 중앙 상단에 바로 보이도록 자동 스크롤
    QTimer::singleShot(0, scroll, [scroll]() {
        if (scroll && scroll->verticalScrollBar()) {
            scroll->verticalScrollBar()->setValue(qMax(0, (QTime::currentTime().hour() - 3) * 54));
        }
    });

    return sidebar;
}

QWidget *MainWindow::buildFullCalendarPage()
{
    // Full-screen calendar workspace layout
    QWidget *page = new QWidget(this);
    page->setStyleSheet("background-color: #FFFFFF;");
    QVBoxLayout *rootV = new QVBoxLayout(page);
    rootV->setContentsMargins(0, 0, 0, 0);
    rootV->setSpacing(0);

    // Top Calendar Header Bar
    QWidget *topBar = new QWidget(page);
    topBar->setFixedHeight(56);
    topBar->setStyleSheet("background-color: #FFFFFF; border-bottom: 1px solid #F1F5F9;");
    QHBoxLayout *th = new QHBoxLayout(topBar);
    th->setContentsMargins(16, 8, 16, 8);
    th->setSpacing(10);

    m_fullCalTodayBtn = new QPushButton(MailI18n::t("오늘", "Today", "今日"), topBar);
    m_fullCalTodayBtn->setCursor(Qt::PointingHandCursor);
    m_fullCalTodayBtn->setStyleSheet("QPushButton { background: #FFFFFF; color: #374151; border: 1px solid #D1D5DB; border-radius: 4px; padding: 5px 14px; font-size: 12px; font-weight: 600; }");
    connect(m_fullCalTodayBtn, &QPushButton::clicked, this, [this]() {
        m_activeCalendarDate = QDate::currentDate();
        refreshCalendarViews();
    });

    QToolButton *btnPrevMonth = new QToolButton(topBar);
    btnPrevMonth->setText("‹");
    btnPrevMonth->setStyleSheet("border: none; font-size: 18px; color: #374151; padding: 2px 6px;");
    connect(btnPrevMonth, &QToolButton::clicked, this, [this]() {
        if (m_calendarViewMode == 1)
            m_activeCalendarDate = m_activeCalendarDate.addDays(-7);
        else if (m_calendarViewMode == 2)
            m_activeCalendarDate = m_activeCalendarDate.addDays(-1);
        else
            m_activeCalendarDate = m_activeCalendarDate.addMonths(-1);
        refreshCalendarViews();
    });

    QToolButton *btnNextMonth = new QToolButton(topBar);
    btnNextMonth->setText("›");
    btnNextMonth->setStyleSheet("border: none; font-size: 18px; color: #374151; padding: 2px 6px;");
    connect(btnNextMonth, &QToolButton::clicked, this, [this]() {
        if (m_calendarViewMode == 1)
            m_activeCalendarDate = m_activeCalendarDate.addDays(7);
        else if (m_calendarViewMode == 2)
            m_activeCalendarDate = m_activeCalendarDate.addDays(1);
        else
            m_activeCalendarDate = m_activeCalendarDate.addMonths(1);
        refreshCalendarViews();
    });

    m_fullCalMonthTitleLbl = new QLabel("11월  2026", topBar);
    m_fullCalMonthTitleLbl->setStyleSheet("font-size: 16px; font-weight: 500; color: #1F2937;");

    QToolButton *btnRefresh = new QToolButton(topBar);
    btnRefresh->setIcon(MailIcons::actionIcon("refresh", QColor("#4B5563"), 16));
    btnRefresh->setStyleSheet("border: none; padding: 4px;");
    connect(btnRefresh, &QToolButton::clicked, this, &MainWindow::onRefreshAndSyncCurrentAccount);

    th->addWidget(m_fullCalTodayBtn);
    th->addWidget(btnPrevMonth);
    th->addWidget(btnNextMonth);
    th->addWidget(m_fullCalMonthTitleLbl);
    th->addWidget(btnRefresh);
    th->addStretch();

    // Segmented View Mode Buttons: [Month] [Week] [Day] [Agenda]
    QWidget *segBox = new QWidget(topBar);
    segBox->setStyleSheet("background-color: #EBECEF; border-radius: 5px;");
    QHBoxLayout *segH = new QHBoxLayout(segBox);
    segH->setContentsMargins(2, 2, 2, 2);
    segH->setSpacing(2);
    m_calViewModeBtns.clear();
    const QStringList modes = {
        MailI18n::t("월간", "Month", "月"),
        MailI18n::t("주간", "Week", "週"),
        MailI18n::t("일간", "Day", "日"),
        MailI18n::t("일정목록", "Agenda", "予定リスト")
    };
    for (int i = 0; i < modes.size(); ++i) {
        QPushButton *mb = new QPushButton(modes[i], segBox);
        mb->setCursor(Qt::PointingHandCursor);
        if (i == 0) {
            mb->setStyleSheet("background: #FFFFFF; color: #111827; font-size: 11px; font-weight: 600; border: 1px solid #E5E7EB; border-radius: 4px; padding: 4px 12px;");
        } else {
            mb->setStyleSheet("background: transparent; color: #6B7280; font-size: 11px; border: none; padding: 4px 12px;");
        }
        connect(mb, &QPushButton::clicked, this, [this, i]() {
            onSwitchCalendarViewMode(i);
        });
        segH->addWidget(mb);
        m_calViewModeBtns.append(mb);
    }
    th->addWidget(segBox);

    m_calSearchEdit = new QLineEdit(topBar);
    m_calSearchEdit->setPlaceholderText(MailI18n::t("🔍 일정 검색", "🔍 Search events", "🔍 予定を検索"));
    m_calSearchEdit->setFixedWidth(195);
    m_calSearchEdit->setStyleSheet("border: 1px solid #E5E7EB; border-radius: 5px; padding: 5px 10px; font-size: 12px;");
    connect(m_calSearchEdit, &QLineEdit::textChanged, this, [this](const QString &q) {
        if (m_monthGridWidget)     m_monthGridWidget->setSearchFilter(q);
        if (m_weekCalViewWidget)   m_weekCalViewWidget->setSearchFilter(q);
        if (m_dayCalViewWidget)    m_dayCalViewWidget->setSearchFilter(q);
        if (m_agendaCalViewWidget) m_agendaCalViewWidget->setSearchFilter(q);
    });
    th->addWidget(m_calSearchEdit);

    QToolButton *calGear = new QToolButton(topBar);
    calGear->setIcon(MailIcons::actionIcon("settings", QColor("#4B5563"), 17));
    calGear->setStyleSheet("border: none; padding: 4px;");
    connect(calGear, &QToolButton::clicked, this, [this]() { onOpenSettingsModal(8); });
    th->addWidget(calGear);

    rootV->addWidget(topBar);

    // Body: Left Sub-Pane (New event + Mini Calendar + Multi-Account Calendar Tree) + Right Month Grid
    QHBoxLayout *bodyH = new QHBoxLayout();
    bodyH->setContentsMargins(0, 0, 0, 0);
    bodyH->setSpacing(0);

    QWidget *leftSub = new QWidget(page);
    leftSub->setFixedWidth(226);
    leftSub->setStyleSheet("background-color: #FFFFFF; border-right: 1px solid #F1F5F9;");
    QVBoxLayout *leftV = new QVBoxLayout(leftSub);
    leftV->setContentsMargins(14, 12, 14, 12);
    leftV->setSpacing(10);

    // Royal Blue "New event" button
    m_fullCalNewEventBtn = new QPushButton(MailI18n::t("새 일정 추가", "New event", "予定を追加"), leftSub);
    m_fullCalNewEventBtn->setCursor(Qt::PointingHandCursor);
    m_fullCalNewEventBtn->setFixedHeight(34);
    m_fullCalNewEventBtn->setStyleSheet("QPushButton { background-color: #0D47A1; color: #FFFFFF; font-size: 12px; font-weight: 700; border-radius: 5px; border: none; }"
                                        "QPushButton:hover { background-color: #1558BC; }");
    connect(m_fullCalNewEventBtn, &QPushButton::clicked, this, [this]() {
        onAddNewCalendarEvent(m_activeCalendarDate);
    });
    leftV->addWidget(m_fullCalNewEventBtn);

    // Apple Calendar (iCloud CalDAV / webcal:// / .ics) Integration Button
    m_fullCalAppleSyncBtn = new QPushButton(MailI18n::t("🍎 Apple 캘린더 연동", "🍎 Apple Calendar Sync", "🍎 Appleカレンダー連携"), leftSub);
    m_fullCalAppleSyncBtn->setCursor(Qt::PointingHandCursor);
    m_fullCalAppleSyncBtn->setFixedHeight(31);
    m_fullCalAppleSyncBtn->setStyleSheet(
        "QPushButton { background-color: #F8FAFC; color: #1E293B; font-size: 11.5px; font-weight: 600; border-radius: 5px; border: 1px solid #CBD5E1; }"
        "QPushButton:hover { background-color: #F1F5F9; border-color: #94A3B8; }");
    connect(m_fullCalAppleSyncBtn, &QPushButton::clicked, this, &MainWindow::onOpenAppleCalendarSyncDialog);
    leftV->addWidget(m_fullCalAppleSyncBtn);

    // Mini Month Calendar
    m_miniCalendarWidget = new MiniMonthCalendarWidget(leftSub);
    connect(m_miniCalendarWidget, &MiniMonthCalendarWidget::dateSelected, this, [this](const QDate &d) {
        m_activeCalendarDate = d;
        refreshCalendarViews();
    });
    connect(m_miniCalendarWidget, &MiniMonthCalendarWidget::monthChanged, this, [this](int y, int m) {
        m_activeCalendarDate = QDate(y, m, 1);
        refreshCalendarViews();
    });
    leftV->addWidget(m_miniCalendarWidget, 0, Qt::AlignHCenter);

    // Multi-Account Calendar Tree (동적으로 현재 등록된 계정만 표시)
    QScrollArea *treeScroll = new QScrollArea(leftSub);
    treeScroll->setWidgetResizable(true);
    treeScroll->setFrameShape(QFrame::NoFrame);
    m_calendarTreeContainer = new QWidget(treeScroll);
    m_calendarTreeLayout = new QVBoxLayout(m_calendarTreeContainer);
    m_calendarTreeLayout->setContentsMargins(2, 4, 2, 4);
    m_calendarTreeLayout->setSpacing(6);

    rebuildCalendarTree();

    treeScroll->setWidget(m_calendarTreeContainer);
    leftV->addWidget(treeScroll, 1);

    bodyH->addWidget(leftSub);

    // Center 4-Mode Stack: [0] Month, [1] Week, [2] Day, [3] Agenda
    m_fullCalViewStack = new QStackedWidget(page);

    auto syncSelectedDateAcrossViews = [this](const QDate &d) {
        m_activeCalendarDate = d;
        refreshCalendarViews();
    };

    // [0] Month View
    m_monthGridWidget = new MonthCalendarGridWidget(m_fullCalViewStack);
    connect(m_monthGridWidget, &MonthCalendarGridWidget::dateClicked, this, syncSelectedDateAcrossViews);
    connect(m_monthGridWidget, &MonthCalendarGridWidget::dateDoubleClicked, this, [this](const QDate &d) {
        onAddNewCalendarEvent(d);
    });
    connect(m_monthGridWidget, &MonthCalendarGridWidget::eventClicked, this, &MainWindow::onEditOrDeleteCalendarEvent);
    m_fullCalViewStack->addWidget(m_monthGridWidget);

    // [1] Week View
    m_weekCalViewWidget = new WeekCalendarViewWidget(m_fullCalViewStack);
    connect(m_weekCalViewWidget, &WeekCalendarViewWidget::dateClicked, this, syncSelectedDateAcrossViews);
    connect(m_weekCalViewWidget, &WeekCalendarViewWidget::dateDoubleClicked, this, [this](const QDate &d) {
        onAddNewCalendarEvent(d);
    });
    connect(m_weekCalViewWidget, &WeekCalendarViewWidget::eventClicked, this, &MainWindow::onEditOrDeleteCalendarEvent);
    m_fullCalViewStack->addWidget(m_weekCalViewWidget);

    // [2] Day View
    m_dayCalViewWidget = new DayCalendarViewWidget(m_fullCalViewStack);
    connect(m_dayCalViewWidget, &DayCalendarViewWidget::dateClicked, this, syncSelectedDateAcrossViews);
    connect(m_dayCalViewWidget, &DayCalendarViewWidget::dateDoubleClicked, this, [this](const QDate &d) {
        onAddNewCalendarEvent(d);
    });
    connect(m_dayCalViewWidget, &DayCalendarViewWidget::eventClicked, this, &MainWindow::onEditOrDeleteCalendarEvent);
    m_fullCalViewStack->addWidget(m_dayCalViewWidget);

    // [3] Agenda View
    m_agendaCalViewWidget = new AgendaCalendarViewWidget(m_fullCalViewStack);
    connect(m_agendaCalViewWidget, &AgendaCalendarViewWidget::dateClicked, this, syncSelectedDateAcrossViews);
    connect(m_agendaCalViewWidget, &AgendaCalendarViewWidget::eventClicked, this, &MainWindow::onEditOrDeleteCalendarEvent);
    m_fullCalViewStack->addWidget(m_agendaCalViewWidget);

    bodyH->addWidget(m_fullCalViewStack, 1);

    rootV->addLayout(bodyH, 1);
    return page;
}

QWidget *MainWindow::buildFullContactsPage()
{
    QWidget *page = new QWidget(this);
    page->setStyleSheet("background-color: #FFFFFF;");

    QHBoxLayout *h = new QHBoxLayout(page);
    h->setContentsMargins(0, 0, 0, 0);
    h->setSpacing(0);

    // 1. Left Contact Account & Custom Groups Pane (245px)
    QWidget *leftGroups = new QWidget(page);
    leftGroups->setFixedWidth(245);
    leftGroups->setStyleSheet("background-color: #FFFFFF; border-right: 1px solid #E5E7EB;");
    QVBoxLayout *lv = new QVBoxLayout(leftGroups);
    lv->setContentsMargins(14, 16, 14, 14);
    lv->setSpacing(8);

    QHBoxLayout *grpHdrRow = new QHBoxLayout();
    grpHdrRow->setSpacing(4);
    m_contactsLeftHeaderLbl = new QLabel(MailI18n::t("연락처 그룹 (Groups)", "Contact Groups", "連絡先グループ"), leftGroups);
    m_contactsLeftHeaderLbl->setStyleSheet("font-size: 12px; font-weight: 700; color: #1F2937;");
    grpHdrRow->addWidget(m_contactsLeftHeaderLbl, 1);

    QPushButton *btnNewGroup = new QPushButton(MailI18n::t("➕ 그룹 추가", "➕ New Group", "➕ グループ追加"), leftGroups);
    btnNewGroup->setObjectName(QStringLiteral("contactNewGroupBtn"));
    btnNewGroup->setCursor(Qt::PointingHandCursor);
    btnNewGroup->setToolTip(MailI18n::t("새 연락처 그룹을 만듭니다", "Create a new contact group", "新しい連絡先グループを作成"));
    btnNewGroup->setStyleSheet(
        "QPushButton { background: #EFF6FF; color: #0D47A1; border: 1px solid #BFDBFE; border-radius: 4px; padding: 3px 8px; font-size: 11px; font-weight: 700; }"
        "QPushButton:hover { background: #DBEAFE; }");
    connect(btnNewGroup, &QPushButton::clicked, this, &MainWindow::onAddContactGroup);
    grpHdrRow->addWidget(btnNewGroup);
    lv->addLayout(grpHdrRow);

    m_contactGroupList = new QListWidget(leftGroups);
    m_contactGroupList->setFrameShape(QFrame::NoFrame);
    m_contactGroupList->setContextMenuPolicy(Qt::CustomContextMenu);
    m_contactGroupList->setStyleSheet(R"(
        QListWidget { background: transparent; outline: none; }
        QListWidget::item { padding: 8px 10px; border-radius: 5px; color: #374151; font-size: 12px; }
        QListWidget::item:selected { background: #EFF6FF; color: #0D47A1; font-weight: 600; }
    )");
    connect(m_contactGroupList, &QListWidget::currentRowChanged, this, [this]() {
        refreshContactsList();
    });
    connect(m_contactGroupList, &QListWidget::customContextMenuRequested, this, [this](const QPoint &pos) {
        QListWidgetItem *it = m_contactGroupList->itemAt(pos);
        if (it)
            m_contactGroupList->setCurrentItem(it);
        QMenu menu(this);
        menu.setStyleSheet(
            "QMenu { background: #FFFFFF; border: 1px solid #E5E7EB; border-radius: 6px; padding: 4px; }"
            "QMenu::item { padding: 7px 16px; font-size: 12px; color: #1F2937; border-radius: 4px; }"
            "QMenu::item:selected { background: #EFF6FF; color: #0D47A1; font-weight: 600; }");
        QAction *aMailGrp = menu.addAction(MailI18n::t("✉️ 이 그룹 전체에 메일 보내기", "✉️ Send Email to Group", "✉️ グループ全員にメール送信"));
        QAction *aPickExisting = menu.addAction(MailI18n::t("📋 기존 주소록에서 이 그룹에 연락처 추가...", "📋 Add Contacts from Address Book...", "📋 既存の連絡先からグループに追加..."));
        QAction *aAddNewToGrp = menu.addAction(MailI18n::t("➕ 새 연락처 직접 입력하여 추가...", "➕ Create New Contact in Group...", "➕ 新しい連絡先を作成して追加..."));
        menu.addSeparator();
        QAction *aAddGrp = menu.addAction(MailI18n::t("➕ 새 그룹 추가", "➕ Add Group", "➕ 新規グループ追加"));
        QAction *aRenGrp = menu.addAction(MailI18n::t("✏️ 그룹 이름 변경", "✏️ Rename Group", "✏️ グループ名変更"));
        QAction *aDelGrp = menu.addAction(MailI18n::t("🗑️ 그룹 삭제", "🗑️ Delete Group", "🗑️ グループ削除"));
        connect(aMailGrp, &QAction::triggered, this, &MainWindow::onSendMailToSelectedContactGroup);
        connect(aPickExisting, &QAction::triggered, this, &MainWindow::onAddExistingContactsToGroup);
        connect(aAddNewToGrp, &QAction::triggered, this, [this]() { onAddNewContact(false); });
        connect(aAddGrp, &QAction::triggered, this, &MainWindow::onAddContactGroup);
        connect(aRenGrp, &QAction::triggered, this, &MainWindow::onRenameContactGroup);
        connect(aDelGrp, &QAction::triggered, this, &MainWindow::onDeleteContactGroup);
        menu.exec(m_contactGroupList->viewport()->mapToGlobal(pos));
    });
    lv->addWidget(m_contactGroupList, 1);

    // 그룹 관리 및 그룹 전체 메일 발송 하단 액션 바
    QHBoxLayout *grpManageRow = new QHBoxLayout();
    grpManageRow->setSpacing(4);
    QPushButton *btnRenGroup = new QPushButton(MailI18n::t("✏️ 이름변경", "✏️ Rename", "✏️ 名前変更"), leftGroups);
    btnRenGroup->setObjectName(QStringLiteral("contactRenameGroupBtn"));
    btnRenGroup->setCursor(Qt::PointingHandCursor);
    btnRenGroup->setStyleSheet(
        "QPushButton { background: #F9FAFB; color: #374151; border: 1px solid #D1D5DB; border-radius: 4px; padding: 4px 8px; font-size: 11px; font-weight: 600; }"
        "QPushButton:hover { background: #F3F4F6; }");
    connect(btnRenGroup, &QPushButton::clicked, this, &MainWindow::onRenameContactGroup);

    QPushButton *btnDelGroup = new QPushButton(MailI18n::t("🗑️ 그룹삭제", "🗑️ Delete", "🗑️ 削除"), leftGroups);
    btnDelGroup->setObjectName(QStringLiteral("contactDeleteGroupBtn"));
    btnDelGroup->setCursor(Qt::PointingHandCursor);
    btnDelGroup->setStyleSheet(
        "QPushButton { background: #FEF2F2; color: #DC2626; border: 1px solid #FECACA; border-radius: 4px; padding: 4px 8px; font-size: 11px; font-weight: 600; }"
        "QPushButton:hover { background: #FEE2E2; }");
    connect(btnDelGroup, &QPushButton::clicked, this, &MainWindow::onDeleteContactGroup);

    grpManageRow->addWidget(btnRenGroup, 1);
    grpManageRow->addWidget(btnDelGroup, 1);
    lv->addLayout(grpManageRow);

    QPushButton *btnAddFromAddrBook = new QPushButton(
        MailI18n::t("📋 주소록에서 그룹에 연락처 추가", "📋 Add from Address Book", "📋 アドレス帳からグループに追加"), leftGroups);
    btnAddFromAddrBook->setObjectName(QStringLiteral("contactAddFromAddrBookBtn"));
    btnAddFromAddrBook->setCursor(Qt::PointingHandCursor);
    btnAddFromAddrBook->setStyleSheet(
        "QPushButton { background: #EFF6FF; color: #0D47A1; border: 1px solid #93C5FD; border-radius: 5px; padding: 6px 10px; font-size: 11.5px; font-weight: 700; }"
        "QPushButton:hover { background: #DBEAFE; }");
    connect(btnAddFromAddrBook, &QPushButton::clicked, this, &MainWindow::onAddExistingContactsToGroup);
    lv->addWidget(btnAddFromAddrBook);

    QPushButton *btnMailToGroup = new QPushButton(
        MailI18n::t("✉️ 선택 그룹 전체에 메일 발송", "✉️ Email Selected Group", "✉️ 選択グループ全員に送信"), leftGroups);
    btnMailToGroup->setObjectName(QStringLiteral("contactMailToGroupBtn"));
    btnMailToGroup->setCursor(Qt::PointingHandCursor);
    btnMailToGroup->setStyleSheet(
        "QPushButton { background: #0D47A1; color: #FFFFFF; border: none; border-radius: 5px; padding: 7px 10px; font-size: 11.5px; font-weight: 700; }"
        "QPushButton:hover { background: #1558BC; }");
    connect(btnMailToGroup, &QPushButton::clicked, this, &MainWindow::onSendMailToSelectedContactGroup);
    lv->addWidget(btnMailToGroup);

    rebuildContactGroupsList();
    h->addWidget(leftGroups);

    // 2. Center Contact List Pane (380px)
    QWidget *centerListPane = new QWidget(page);
    centerListPane->setFixedWidth(380);
    centerListPane->setStyleSheet("background-color: #FFFFFF; border-right: 1px solid #E5E7EB;");
    QVBoxLayout *cv = new QVBoxLayout(centerListPane);
    cv->setContentsMargins(20, 16, 20, 16);
    cv->setSpacing(12);

    QHBoxLayout *chdr = new QHBoxLayout();
    m_contactsCenterHeaderLbl = new QLabel(MailI18n::t("전체 연락처", "All contacts", "すべての連絡先"), centerListPane);
    m_contactsCenterHeaderLbl->setStyleSheet("font-size: 20px; font-weight: 400; color: #374151;");
    chdr->addWidget(m_contactsCenterHeaderLbl);
    chdr->addStretch();

    QToolButton *btnAddContact = new QToolButton(centerListPane);
    btnAddContact->setIcon(MailIcons::actionIcon("add_contact", QColor("#4B5563"), 18));
    btnAddContact->setToolTip(MailI18n::t("연락처 추가 — 새 연락처 입력 또는 기존 주소록에서 선택", "Add Contact (New or Select from Address Book)", "連絡先を追加（新規作成 / アドレス帳から選択）"));
    btnAddContact->setStyleSheet("border: none; padding: 4px;");
    connect(btnAddContact, &QToolButton::clicked, this, [this]() { onAddNewContact(false); });

    QToolButton *btnMoreContact = new QToolButton(centerListPane);
    btnMoreContact->setIcon(MailIcons::actionIcon("more", QColor("#4B5563"), 18));
    btnMoreContact->setToolTip(MailI18n::t("연락처 옵션 (추가 / 수정 / 삭제 / 동기화)", "Contact Options", "連絡先オプション"));
    btnMoreContact->setStyleSheet("border: none; padding: 4px;");
    connect(btnMoreContact, &QToolButton::clicked, this, [this, btnMoreContact]() {
        QMenu menu(this);
        menu.setStyleSheet(
            "QMenu { background-color: #FFFFFF; border: 1px solid #E5E7EB; border-radius: 6px; padding: 6px 4px; }"
            "QMenu::item { padding: 7px 18px; font-size: 12px; color: #1F2937; border-radius: 4px; }"
            "QMenu::item:selected { background-color: #EFF6FF; color: #0D47A1; font-weight: 600; }");
        QAction *aAdd = menu.addAction(MailI18n::t("➕ 새 연락처 직접 추가 (Add New Contact)", "➕ Add New Contact", "➕ 新しい連絡先を追加"));
        QAction *aPick = menu.addAction(MailI18n::t("📋 기존 주소록에서 그룹에 연락처 추가 (Select from Address Book)", "📋 Add Contacts from Address Book to Group", "📋 既存の連絡先からグループに追加"));
        QAction *aEdit = menu.addAction(MailI18n::t("✏️ 선택한 연락처 수정 (Edit Contact)", "✏️ Edit Contact", "✏️ 連絡先を編集"));
        QAction *aDel = menu.addAction(MailI18n::t("🗑️ 선택한 연락처 삭제 (Delete Contact)", "🗑️ Delete Contact", "🗑️ 連絡先を削除"));
        menu.addSeparator();
        QAction *aSync = menu.addAction(MailI18n::t("🔄 Google 및 이메일 연락처 다시 동기화", "🔄 Sync Contacts Now", "🔄 連絡先を再同期"));
        connect(aAdd, &QAction::triggered, this, [this]() { onAddNewContact(false); });
        connect(aPick, &QAction::triggered, this, &MainWindow::onAddExistingContactsToGroup);
        connect(aEdit, &QAction::triggered, this, &MainWindow::onEditCurrentContact);
        connect(aDel, &QAction::triggered, this, &MainWindow::onDeleteCurrentContact);
        connect(aSync, &QAction::triggered, this, &MainWindow::onRefreshAndSyncCurrentAccount);
        menu.exec(btnMoreContact->mapToGlobal(QPoint(0, btnMoreContact->height() + 4)));
    });

    chdr->addWidget(btnAddContact);
    chdr->addWidget(btnMoreContact);
    cv->addLayout(chdr);

    m_contactSearchEdit = new QLineEdit(centerListPane);
    m_contactSearchEdit->setPlaceholderText(MailI18n::t("🔍 연락처 검색", "🔍 Search contact", "🔍 連絡先を検索"));
    m_contactSearchEdit->setStyleSheet("border: 1px solid #D1D5DB; border-radius: 5px; padding: 7px 10px; font-size: 12px;");
    connect(m_contactSearchEdit, &QLineEdit::textChanged, this, [this]() {
        refreshContactsList();
    });
    cv->addWidget(m_contactSearchEdit);

    m_contactItemsList = new QListWidget(centerListPane);
    m_contactItemsList->setFrameShape(QFrame::NoFrame);
    m_contactItemsList->setStyleSheet(R"(
        QListWidget { background: #FFFFFF; outline: none; }
        QListWidget::item { padding: 10px 8px; border-bottom: 1px solid #F3F4F6; color: #1F2937; font-size: 13px; }
        QListWidget::item:selected { background: #EFF6FF; color: #0D47A1; font-weight: 600; border-radius: 5px; }
    )");
    connect(m_contactItemsList, &QListWidget::currentRowChanged, this, [this](int row) {
        if (row < 0 || !m_contactItemsList->item(row))
            return;
        int realIdx = m_contactItemsList->item(row)->data(Qt::UserRole).toInt();
        if (realIdx < 0 || realIdx >= m_contacts.size())
            return;
        const ContactItem &c = m_contacts[realIdx];
        m_contactDetailTitleLbl->setText(c.displayName);
        m_contactDetailBodyLbl->setText(
            QString("<div style='line-height:1.8; font-size:13px; color:#374151;'>"
                    "<p><b>%1</b> <a href='mailto:%6' style='color:#0D47A1;'>%6</a></p>"
                    "<p><b>%2</b> %7</p>"
                    "<p><b>%3</b> %8 %9</p>"
                    "<p><b>%4</b> %10</p>"
                    "<p><b>%5</b> %11</p></div>")
                .arg(MailI18n::t("이메일:", "Email:", "メール:"),
                     MailI18n::t("전화번호:", "Phone:", "電話番号:"),
                     MailI18n::t("소속/회사:", "Company:", "会社・所属:"),
                     MailI18n::t("연동 계정:", "Account:", "連携アカウント:"),
                     MailI18n::t("메모:", "Notes:", "メモ:"),
                     c.email.isEmpty() ? "-" : c.email,
                     c.phone.isEmpty() ? "-" : c.phone,
                     c.company.isEmpty() ? "-" : c.company,
                     c.jobTitle.isEmpty() ? "" : QString("(%1)").arg(c.jobTitle),
                     c.accountEmail,
                     c.notes.isEmpty() ? "-" : c.notes));
    });
    cv->addWidget(m_contactItemsList, 1);
    h->addWidget(centerListPane);

    // 3. Right Contact Detail Pane
    QWidget *detailPane = new QWidget(page);
    QVBoxLayout *dv = new QVBoxLayout(detailPane);
    dv->setContentsMargins(32, 32, 32, 32);
    dv->setSpacing(14);

    m_contactDetailTitleLbl = new QLabel(MailI18n::t("연락처를 선택하세요", "Select a contact", "連絡先を選択してください"), detailPane);
    m_contactDetailTitleLbl->setStyleSheet("font-size: 22px; font-weight: 700; color: #111827;");
    dv->addWidget(m_contactDetailTitleLbl);

    m_contactDetailBodyLbl = new QLabel(detailPane);
    m_contactDetailBodyLbl->setWordWrap(true);
    dv->addWidget(m_contactDetailBodyLbl);

    QHBoxLayout *actRow = new QHBoxLayout();
    m_contactMailToBtn = new QPushButton(MailI18n::t("✉️ 이 연락처로 메일 보내기", "✉️ Send Email to Contact", "✉️ この連絡先にメールを送信"), detailPane);
    m_contactMailToBtn->setCursor(Qt::PointingHandCursor);
    m_contactMailToBtn->setStyleSheet("QPushButton { background: #0D47A1; color: #FFFFFF; font-weight: 600; padding: 8px 16px; border-radius: 5px; border: none; }");
    connect(m_contactMailToBtn, &QPushButton::clicked, this, [this]() {
        int r = m_contactItemsList->currentRow();
        if (r >= 0 && m_contactItemsList->item(r)) {
            int realIdx = m_contactItemsList->item(r)->data(Qt::UserRole).toInt();
            if (realIdx >= 0 && realIdx < m_contacts.size()) {
                onComposeNewMail(m_contacts[realIdx].email);
            }
        }
    });
    connect(m_contactItemsList, &QListWidget::itemDoubleClicked, this, [this]() {
        onEditCurrentContact();
    });

    m_contactEditBtn = new QPushButton(MailI18n::t("✏️ 연락처 수정 (Edit)", "✏️ Edit Contact", "✏️ 編集"), detailPane);
    m_contactEditBtn->setCursor(Qt::PointingHandCursor);
    m_contactEditBtn->setStyleSheet(
        "QPushButton { background: #F3F4F6; color: #1F2937; font-weight: 600; padding: 8px 16px; border-radius: 5px; border: 1px solid #D1D5DB; }"
        "QPushButton:hover { background: #E5E7EB; }");
    connect(m_contactEditBtn, &QPushButton::clicked, this, &MainWindow::onEditCurrentContact);

    m_contactDeleteBtn = new QPushButton(MailI18n::t("🗑️ 연락처 삭제 (Delete)", "🗑️ Delete Contact", "🗑️ 削除"), detailPane);
    m_contactDeleteBtn->setCursor(Qt::PointingHandCursor);
    m_contactDeleteBtn->setStyleSheet(
        "QPushButton { background: #FEF2F2; color: #DC2626; font-weight: 600; padding: 8px 16px; border-radius: 5px; border: 1px solid #FECACA; }"
        "QPushButton:hover { background: #FEE2E2; }");
    connect(m_contactDeleteBtn, &QPushButton::clicked, this, &MainWindow::onDeleteCurrentContact);

    actRow->addWidget(m_contactMailToBtn);
    actRow->addWidget(m_contactEditBtn);
    actRow->addWidget(m_contactDeleteBtn);
    actRow->addStretch();
    dv->addLayout(actRow);
    dv->addStretch();

    h->addWidget(detailPane, 1);
    return page;
}

QWidget *MainWindow::buildFarRightAppStrip()
{
    QWidget *strip = new QWidget(this);
    strip->setObjectName("farRightStrip");
    strip->setFixedWidth(38);
    strip->setStyleSheet("QWidget#farRightStrip { background-color: #FFFFFF; border-left: 1px solid #E5E7EB; }");

    QVBoxLayout *v = new QVBoxLayout(strip);
    v->setContentsMargins(4, 10, 4, 10);
    v->setSpacing(10);

    auto makeStripBtn = [strip](const QString &iconName, const QString &tip) {
        QToolButton *b = new QToolButton(strip);
        b->setFixedSize(28, 28);
        b->setCursor(Qt::PointingHandCursor);
        b->setToolTip(tip);
        b->setIcon(MailIcons::actionIcon(iconName, QColor("#374151"), 17));
        b->setIconSize(QSize(17, 17));
        b->setStyleSheet("QToolButton { border: none; background: transparent; border-radius: 5px; }"
                         "QToolButton:hover { background-color: #F3F4F6; }");
        return b;
    };

    QToolButton *btnGrid = makeStripBtn(
        "app_grid",
        MailI18n::t("앱 런처 / 메일 화면 보기", "Mail Workspace", "メール画面を表示"));
    connect(btnGrid, &QToolButton::clicked, this, [this]() {
        m_centerStack->setCurrentIndex(0);
    });

    QToolButton *btnRightCal = makeStripBtn(
        "calendar_3",
        MailI18n::t("우측 캘린더 사이드바 토글 / 전체 캘린더", "Toggle Right Calendar Sidebar", "右カレンダーサイドバー切替"));
    connect(btnRightCal, &QToolButton::clicked, this, [this]() {
        if (m_centerStack->currentIndex() != 0) {
            m_centerStack->setCurrentIndex(0);
            m_rightCalendarSidebar->setVisible(true);
        } else {
            m_rightCalendarSidebar->setVisible(!m_rightCalendarSidebar->isVisible());
        }
    });

    QToolButton *btnRightContacts = makeStripBtn(
        "contacts",
        MailI18n::t("연락처 화면 열기 (Contacts)", "Open Contacts", "連絡先画面を開く"));
    connect(btnRightContacts, &QToolButton::clicked, this, [this]() {
        m_centerStack->setCurrentIndex(2);
    });

    v->addWidget(btnGrid, 0, Qt::AlignHCenter);
    v->addWidget(btnRightCal, 0, Qt::AlignHCenter);
    v->addWidget(btnRightContacts, 0, Qt::AlignHCenter);
    v->addStretch();

    QToolButton *btnCollapse = new QToolButton(strip);
    btnCollapse->setText("›");
    btnCollapse->setToolTip(MailI18n::t("사이드바 접기/펴기", "Collapse / Expand Sidebar", "サイドバーを折りたたむ/展開"));
    btnCollapse->setStyleSheet("border: none; color: #6B7280; font-size: 16px;");
    connect(btnCollapse, &QToolButton::clicked, this, [this, btnCollapse]() {
        bool vis = !m_rightCalendarSidebar->isVisible();
        m_rightCalendarSidebar->setVisible(vis);
        btnCollapse->setText(vis ? "›" : "‹");
    });
    v->addWidget(btnCollapse, 0, Qt::AlignHCenter);

    return strip;
}

void MainWindow::refreshFolderSelectionStyles()
{
    QMap<QString, int> folderUnreadCounts;
    QMap<QString, int> accountInboxUnreadCounts;
    int unifiedInboxUnreadCount = 0;

    QSet<QString> seenAccountInboxSigs;
    QSet<QString> seenFolderUnreadSigs;
    QSet<QString> seenImportantUnreadSigs;
    QSet<QString> seenStarredUnreadSigs;

    for (ConversationItem &c : m_conversations) {
        if (!c.isUnread)
            continue;
        const QString sigKey = conversationSignatureKey(c);
        if (m_readConvSigs.contains(sigKey)) {
            c.isUnread = false;
            continue;
        }
        const QString accSig = c.accountId + QStringLiteral("|") + sigKey;
        if (c.folderId == QStringLiteral("inbox")
            || c.folderId.startsWith(QStringLiteral("custom_"))) {
            if (!seenAccountInboxSigs.contains(accSig)) {
                seenAccountInboxSigs.insert(accSig);
                accountInboxUnreadCounts[c.accountId] += 1;
                unifiedInboxUnreadCount += 1;
            }
        }
        if (m_selectedAccountId == QStringLiteral("acc_unified") || c.accountId == m_selectedAccountId) {
            // 일반 물리 폴더(받은편지함·내 폴더 등) 안 읽음 개수 집계 (동일 지문 메일 중복 집계 방지)
            if (c.folderId != QStringLiteral("important")
                && c.folderId != QStringLiteral("starred")
                && c.folderId != QStringLiteral("archive")) {
                const QString fKey = c.folderId + QStringLiteral("|") + accSig;
                if (!seenFolderUnreadSigs.contains(fKey)) {
                    seenFolderUnreadSigs.insert(fKey);
                    folderUnreadCounts[c.folderId] += 1;
                }
            }
            // 중요편지함(important) 및 별표편지함(starred) 안 읽음 배지 연동 (받은편지함이나 필터 이동된 내 폴더에 있는 중요/별표 안 읽은 메일 포함)
            if (c.folderId != QStringLiteral("trash") && c.folderId != QStringLiteral("spam")) {
                if ((c.isImportant || c.folderId == QStringLiteral("important"))
                    && !seenImportantUnreadSigs.contains(accSig)) {
                    seenImportantUnreadSigs.insert(accSig);
                    folderUnreadCounts[QStringLiteral("important")] += 1;
                }
                if ((c.isStarred || c.folderId == QStringLiteral("starred"))
                    && !seenStarredUnreadSigs.contains(accSig)) {
                    seenStarredUnreadSigs.insert(accSig);
                    folderUnreadCounts[QStringLiteral("starred")] += 1;
                }
            }
        }
    }

    bool selectedAccIsPop3 = false;
    for (const AccountItem &acc : m_accounts) {
        if (acc.id == m_selectedAccountId && acc.provider != AccountProviderType::Unified) {
            selectedAccIsPop3 = acc.isPop3();
            break;
        }
    }

    for (auto it = m_folderButtons.begin(); it != m_folderButtons.end(); ++it) {
        QPushButton *btn = it.value();
        if (!btn)
            continue;
        const QString folderId = it.key();

        // POP3 계정 선택 시에는 IMAP/Gmail 전용 서버 폴더(보관함, 다시알림)를 숨기고
        // POP3 기본 편지함 + 중요/별표 필터 편지함(중요 FLAG 헤더 수신 메일 및 로컬 별표 메일 조회) + POP3 로컬 내 폴더를 표시
        if (!folderId.startsWith(QStringLiteral("custom_"))) {
            const bool isImapOnlySystemFolder = (folderId == QStringLiteral("archive")
                                                 || folderId == QStringLiteral("snoozed"));
            btn->setVisible(!(selectedAccIsPop3 && isImapOnlySystemFolder));
        }

        const bool selected = (folderId == m_selectedFolderId);
        const int unreadCount = folderUnreadCounts.value(folderId, 0);

        if (selected) {
            btn->setStyleSheet(QString(
                "QPushButton { text-align: left; padding-left: 10px; padding-right: 30px; background-color: %1; color: #FFFFFF; font-size: 12px; font-weight: 700; border: none; border-radius: 5px; }"
            ).arg(m_palette.sidebarSelectedBg.name()));
        } else if (unreadCount > 0) {
            btn->setStyleSheet(
                "QPushButton { text-align: left; padding-left: 10px; padding-right: 30px; background-color: transparent; color: #FFFFFF; font-size: 12px; font-weight: 700; border: none; border-radius: 5px; }"
                "QPushButton:hover { background-color: rgba(255, 255, 255, 0.11); color: #FFFFFF; }");
        } else {
            btn->setStyleSheet(
                "QPushButton { text-align: left; padding-left: 10px; padding-right: 30px; background-color: transparent; color: #DCE6F7; font-size: 12px; font-weight: 400; border: none; border-radius: 5px; }"
                "QPushButton:hover { background-color: rgba(255, 255, 255, 0.11); color: #FFFFFF; }");
        }

        QLabel *badge = btn->findChild<QLabel *>("unreadBadge", Qt::FindDirectChildrenOnly);
        if (unreadCount > 0) {
            if (!badge) {
                badge = new QLabel(btn);
                badge->setObjectName("unreadBadge");
                badge->setAttribute(Qt::WA_TransparentForMouseEvents, true);
                badge->setAlignment(Qt::AlignCenter);
                badge->setStyleSheet(
                    "QLabel#unreadBadge {"
                    "  background-color: #2563EB;"
                    "  color: #FFFFFF;"
                    "  font-size: 10.5px;"
                    "  font-weight: 700;"
                    "  border-radius: 4px;"
                    "  padding: 0px 3px;"
                    "  border: none;"
                    "}");
            }
            const QString badgeText = (unreadCount > 99) ? QStringLiteral("99+") : QString::number(unreadCount);
            badge->setText(badgeText);

            QFont tf = btn->font();
            tf.setPixelSize(12);
            tf.setBold(true);
            QFontMetrics fm(tf);
            const int iconW = btn->icon().isNull() ? 0 : (btn->iconSize().width() + 7);
            const int textW = fm.horizontalAdvance(btn->text());
            const int idealX = 10 + iconW + textW + 7;

            const int badgeH = 17;
            const int badgeW = (badgeText.length() >= 3) ? 28 : ((badgeText.length() == 2) ? 22 : 18);
            const int availW = (btn->width() > 140) ? btn->width() : 186;
            const int badgeX = qMin(idealX, availW - badgeW - 6);
            const int badgeY = qMax(2, (btn->height() - badgeH) / 2);

            badge->setGeometry(badgeX, badgeY, badgeW, badgeH);
            badge->show();
            badge->raise();
        } else if (badge) {
            badge->hide();
        }
    }

    for (const AccountItem &acc : m_accounts) {
        if (m_accountButtons.contains(acc.id)) {
            bool active = (acc.id == m_selectedAccountId);
            int accUnread = (acc.provider == AccountProviderType::Unified || acc.id == "acc_unified")
                                ? unifiedInboxUnreadCount
                                : accountInboxUnreadCounts.value(acc.id, 0);
            m_accountButtons[acc.id]->setIcon(QIcon(MailIcons::accountAvatarPixmap(acc, 34, active, accUnread)));
        }
        if (acc.id == m_selectedAccountId && m_folderAccountHeaderLbl) {
            if (acc.provider == AccountProviderType::Unified || acc.id == QStringLiteral("acc_unified")) {
                m_folderAccountHeaderLbl->setText(QStringLiteral("ALL ACCOUNTS (UNIFIED)"));
            } else {
                const QString protoTag = acc.isPop3() ? QStringLiteral("POP3") : QStringLiteral("IMAP");
                m_folderAccountHeaderLbl->setText(QString("%1 [%2]").arg(acc.email.toUpper(), protoTag));
            }
        }
    }
}

void MainWindow::refreshConversationList()
{
    if (!m_convListWidget)
        return;

    if (m_convListDelegate) {
        m_convListDelegate->setPaletteState(m_palette);
        m_convListDelegate->setPrivacyBlur(false);
        m_convListDelegate->setCheckedConvIds(&m_checkedConvIds);
    }

    QSignalBlocker listBlocker(m_convListWidget);
    m_convListWidget->clear();
    m_filteredConvIndices.clear();
    m_renderedConvRows = 0;

    QSet<QString> seenConvIds;
    QMap<QString, int> seenSigPos;
    const bool isVirtualFlagView = (m_selectedFolderId == QStringLiteral("starred")
                                    || m_selectedFolderId == QStringLiteral("important"));
    for (int i = 0; i < m_conversations.size(); ++i) {
        ConversationItem &cMut = m_conversations[i];
        const QString sigKey = conversationSignatureKey(cMut);
        if (cMut.isUnread && m_readConvSigs.contains(sigKey)) {
            cMut.isUnread = false;
        }
        const ConversationItem &c = cMut;
        if (!c.id.isEmpty()) {
            if (seenConvIds.contains(c.id))
                continue;
            seenConvIds.insert(c.id);
        }

        // Account filter
        if (m_selectedAccountId != "acc_unified" && c.accountId != m_selectedAccountId) {
            continue;
        }

        // Folder filter
        if (m_selectedFolderId == "starred") {
            if ((!c.isStarred && c.folderId != "starred") || c.folderId == "trash" || c.folderId == "spam")
                continue;
        } else if (m_selectedFolderId == "important") {
            if ((!c.isImportant && c.folderId != "important") || c.folderId == "trash" || c.folderId == "spam")
                continue;
        } else if (m_selectedFolderId != c.folderId) {
            continue;
        }

        // Unread filter
        if (m_unreadOnlyFilter && !c.isUnread)
            continue;

        // Search query filter
        if (!m_mailSearchQuery.isEmpty()) {
            const MessageItem &m = c.latestMessage();
            bool match = c.subject.contains(m_mailSearchQuery, Qt::CaseInsensitive) ||
                         m.senderName.contains(m_mailSearchQuery, Qt::CaseInsensitive) ||
                         m.recipientSummary.contains(m_mailSearchQuery, Qt::CaseInsensitive) ||
                         m.snippet.contains(m_mailSearchQuery, Qt::CaseInsensitive);
            if (!match)
                continue;
        }

        const QString accSigKey = c.accountId + QStringLiteral("|") + sigKey;
        if (seenSigPos.contains(accSigKey)) {
            const int prevPos = seenSigPos.value(accSigKey);
            if (prevPos >= 0 && prevPos < m_filteredConvIndices.size()) {
                const ConversationItem &prevC = m_conversations[m_filteredConvIndices[prevPos]];
                if (isVirtualFlagView) {
                    const bool prevIsVirtualFolder = (prevC.folderId == QStringLiteral("starred")
                                                      || prevC.folderId == QStringLiteral("important"));
                    const bool curIsRealFolder = (c.folderId != QStringLiteral("starred")
                                                  && c.folderId != QStringLiteral("important"));
                    if ((prevIsVirtualFolder && curIsRealFolder)
                        || (prevC.imapSeq == 0 && c.imapSeq > 0)) {
                        m_filteredConvIndices[prevPos] = i;
                    }
                    continue;
                } else if (prevC.imapSeq == 0 || c.imapSeq == 0
                           || !prevC.previousFolderId.isEmpty() || !c.previousFolderId.isEmpty()) {
                    if (prevC.imapSeq == 0 && c.imapSeq > 0) {
                        m_filteredConvIndices[prevPos] = i;
                    }
                    continue;
                }
            }
        } else {
            seenSigPos.insert(accSigKey, m_filteredConvIndices.size());
        }

        m_filteredConvIndices.append(i);
    }

    // 선택된 정렬 기준(m_mailSortMode, 기본값 0: 최신순)에 따라 메일 목록 정렬
    auto extractSeqKey = [this](int realIdx) -> int {
        const ConversationItem &cv = m_conversations[realIdx];
        if (cv.imapSeq > 0)
            return cv.imapSeq;
        const QString &cid = cv.id;
        if (cid.startsWith("conv_local_"))
            return 100000000 + realIdx;
        if (cid.startsWith("imap_")) {
            int lastUnderscore = cid.lastIndexOf('_');
            if (lastUnderscore > 0) {
                bool ok = false;
                int seq = cid.mid(lastUnderscore + 1).toInt(&ok);
                if (ok)
                    return seq;
            }
        }
        return -realIdx;
    };
    auto extractTimestampKey = [this](int realIdx) -> qint64 {
        const ConversationItem &c = m_conversations[realIdx];
        if (c.messages.isEmpty())
            return 0;
        QString cleanDate = c.messages.last().fullTimeText.trimmed();
        if (cleanDate.isEmpty())
            return 0;
        static const QRegularExpression rxComment(R"(\s*\([^)]*\)\s*$)");
        cleanDate.remove(rxComment);
        QDateTime dt = QDateTime::fromString(cleanDate, Qt::RFC2822Date);
        if (!dt.isValid())
            dt = QDateTime::fromString(cleanDate, Qt::ISODate);
        if (!dt.isValid())
            dt = QDateTime::fromString(cleanDate, QStringLiteral("yyyy-MM-dd hh:mm"));
        return dt.isValid() ? dt.toSecsSinceEpoch() : 0;
    };
    auto compareNewestFirst = [&](int a, int b) -> bool {
        const qint64 tsA = extractTimestampKey(a);
        const qint64 tsB = extractTimestampKey(b);
        if (tsA > 0 && tsB > 0 && tsA != tsB)
            return tsA > tsB;
        return extractSeqKey(a) > extractSeqKey(b);
    };
    auto compareOldestFirst = [&](int a, int b) -> bool {
        const qint64 tsA = extractTimestampKey(a);
        const qint64 tsB = extractTimestampKey(b);
        if (tsA > 0 && tsB > 0 && tsA != tsB)
            return tsA < tsB;
        return extractSeqKey(a) < extractSeqKey(b);
    };
    std::stable_sort(m_filteredConvIndices.begin(), m_filteredConvIndices.end(),
                     [&](int a, int b) -> bool {
        if (m_mailSortMode == 1) {
            return compareOldestFirst(a, b);
        }
        if (m_mailSortMode == 2) {
            const QString sA = m_conversations[a].messages.isEmpty()
                                   ? QString()
                                   : (m_conversations[a].messages.last().senderName.trimmed().isEmpty()
                                          ? m_conversations[a].messages.last().senderEmail.trimmed()
                                          : m_conversations[a].messages.last().senderName.trimmed());
            const QString sB = m_conversations[b].messages.isEmpty()
                                   ? QString()
                                   : (m_conversations[b].messages.last().senderName.trimmed().isEmpty()
                                          ? m_conversations[b].messages.last().senderEmail.trimmed()
                                          : m_conversations[b].messages.last().senderName.trimmed());
            const int cmp = QString::localeAwareCompare(sA.toLower(), sB.toLower());
            if (cmp != 0)
                return cmp < 0;
            return compareNewestFirst(a, b);
        }
        if (m_mailSortMode == 3) {
            const int cmp = QString::localeAwareCompare(
                m_conversations[a].subject.trimmed().toLower(),
                m_conversations[b].subject.trimmed().toLower());
            if (cmp != 0)
                return cmp < 0;
            return compareNewestFirst(a, b);
        }
        if (m_mailSortMode == 4) {
            if (m_conversations[a].isUnread != m_conversations[b].isUnread)
                return m_conversations[a].isUnread;
            return compareNewestFirst(a, b);
        }
        return compareNewestFirst(a, b);
    });

    // ------------------------------------------------------------------------
    // 20개 단위 페이지네이션 슬라이싱 (자동 스크롤 로드 없이 선택된 페이지의 20개만 표시)
    // ------------------------------------------------------------------------
    const int kPageSize = 20;
    int serverTotalForFolder = 0;
    for (const AccountItem &acc : m_accounts) {
        if (acc.provider == AccountProviderType::Unified)
            continue;
        if (m_selectedAccountId != "acc_unified" && acc.id != m_selectedAccountId)
            continue;
        serverTotalForFolder += m_folderTotalExists.value(acc.id + "|" + m_selectedFolderId, 0);
    }

    const int allFilteredCount = m_filteredConvIndices.size();
    const bool isUnfilteredStandardView = (m_mailSearchQuery.isEmpty()
                                           && !m_unreadOnlyFilter
                                           && m_selectedFolderId != "snoozed"
                                           && m_selectedFolderId != "starred"
                                           && m_selectedFolderId != "important");
    const int effectiveTotalItems = isUnfilteredStandardView ? qMax(allFilteredCount, serverTotalForFolder) : allFilteredCount;
    m_totalMailPages = qMax(1, (effectiveTotalItems + kPageSize - 1) / kPageSize);
    m_currentMailPage = qBound(1, m_currentMailPage, m_totalMailPages);

    if (isUnfilteredStandardView && m_selectedAccountId != "acc_unified" && serverTotalForFolder > 0 && m_mailSortMode == 0) {
        // 단일 계정 IMAP 폴더(최신순): 해당 페이지(p)의 서버 순번 구간 [pageStartSeq .. pageEndSeq] (최대 1,000개)만 추출
        const int pageEndSeq = serverTotalForFolder - (m_currentMailPage - 1) * kPageSize;
        const int pageStartSeq = qMax(1, pageEndSeq - kPageSize + 1);
        QList<int> pageSlice;
        pageSlice.reserve(kPageSize);
        for (int realIdx : m_filteredConvIndices) {
            if (realIdx < 0 || realIdx >= m_conversations.size())
                continue;
            const ConversationItem &cv = m_conversations[realIdx];
            const QString &cid = cv.id;
            if (cid.startsWith(QStringLiteral("imap_"))) {
                const int seq = extractSeqKey(realIdx);
                if ((seq >= pageStartSeq && seq <= pageEndSeq)
                    || (m_currentMailPage == 1 && (cv.imapSeq == 0 || !cv.previousFolderId.isEmpty()))) {
                    pageSlice.append(realIdx);
                }
            } else if (m_currentMailPage == 1) {
                pageSlice.append(realIdx);
            }
        }
        if (!pageSlice.isEmpty()) {
            if (pageSlice.size() > kPageSize) {
                pageSlice = pageSlice.mid(0, kPageSize);
            }
            m_filteredConvIndices = pageSlice;
        } else {
            // 해당 페이지 구간이 아직 메모리에 없으면 즉시 서버에 해당 페이지 구간 요청을 트리거하고 로딩 상태를 유지
            const QString folderKey = m_selectedAccountId + QStringLiteral("|") + m_selectedFolderId;
            if (m_currentMailPage > 1 && pageEndSeq >= 1) {
                m_folderLoadingMore.insert(folderKey);
                if (m_activePageFetchEndSeq.value(folderKey, -1) != pageEndSeq && m_googleProvider) {
                    m_activePageFetchEndSeq[folderKey] = pageEndSeq;
                    for (const AccountItem &acc : m_accounts) {
                        if (acc.id == m_selectedAccountId
                            && (!acc.appPassword.isEmpty() || !acc.oauthAccessToken.isEmpty())) {
                            m_googleProvider->fetchImapFolder(acc, m_selectedFolderId, kPageSize, pageEndSeq);
                            break;
                        }
                    }
                }
                m_filteredConvIndices.clear();
            } else if (m_currentMailPage == 1 && !m_filteredConvIndices.isEmpty()) {
                m_filteredConvIndices = m_filteredConvIndices.mid(0, kPageSize);
            } else {
                m_filteredConvIndices.clear();
            }
        }
    } else {
        // 통합 편지함, 검색/안읽음 필터, 또는 기타 정렬 모드: 정렬된 목록에서 (m_currentMailPage - 1) * 1000 위치부터 1,000개만 슬라이싱
        const int startOffset = (m_currentMailPage - 1) * kPageSize;
        if (startOffset >= 0 && startOffset < m_filteredConvIndices.size()) {
            m_filteredConvIndices = m_filteredConvIndices.mid(startOffset, kPageSize);
        } else if (startOffset >= m_filteredConvIndices.size()) {
            m_filteredConvIndices.clear();
        }
    }

    // ConversationListDelegate 가상화 덕분에 1000건도 1ms 이내에 QWidget 생성 없이 가볍게 추가됨
    m_convListWidget->setUniformItemSizes(!m_filteredConvIndices.isEmpty());
    appendMoreConversationRows(kPageSize);
    updateMailPaginationBar();
    updateAppUnreadBadgeIcon();
    refreshFolderSelectionStyles();

    if (m_unreadFilterBanner && m_unreadFilterBannerLbl) {
        m_unreadFilterBanner->setVisible(m_unreadOnlyFilter);
        if (m_unreadOnlyFilter) {
            if (m_filteredConvIndices.isEmpty()) {
                m_unreadFilterBannerLbl->setText(
                    MailI18n::t("📭 읽지 않은 메일이 없습니다 (0건)",
                                "📭 No unread messages (0)",
                                "📭 未読メールはありません (0件)"));
            } else {
                m_unreadFilterBannerLbl->setText(
                    MailI18n::t(QString("📩 읽지 않은 메일만 보기 (%1건)").arg(m_filteredConvIndices.size()),
                                QString("📩 Unread only (%1)").arg(m_filteredConvIndices.size()),
                                QString("📩 未読メールのみ (%1件)").arg(m_filteredConvIndices.size())));
            }
        }
    }

    if (!m_filteredConvIndices.isEmpty()) {
        int targetRow = -1;
        if (!m_selectedConvId.isEmpty() && m_userExplicitlyOpenedConvIds.contains(m_selectedConvId)) {
            for (int r = 0; r < m_filteredConvIndices.size(); ++r) {
                if (m_conversations[m_filteredConvIndices[r]].id == m_selectedConvId) {
                    targetRow = r;
                    break;
                }
            }
        }
        if (targetRow >= 0) {
            m_convListWidget->setCurrentRow(targetRow);
            listBlocker.unblock();
            displayConversation(targetRow, false);
        } else {
            m_convListWidget->setCurrentRow(-1);
            listBlocker.unblock();
            if (m_scheduledMailBanner)
                m_scheduledMailBanner->setVisible(false);
            if (m_actRestoreBtn)
                m_actRestoreBtn->setVisible(false);
            if (m_readingAttachmentsBar)
                m_readingAttachmentsBar->setVisible(false);
            if (m_readingSenderLbl)
                m_readingSenderLbl->clear();
            if (m_readingTimeLbl)
                m_readingTimeLbl->clear();
            if (m_readingFolderChipLbl)
                m_readingFolderChipLbl->setText(MailI18n::folderTitle(m_selectedFolderId, m_selectedFolderTitle));
            if (m_blockSenderBtn)
                m_blockSenderBtn->setVisible(false);
            if (m_unsubscribeBtn)
                m_unsubscribeBtn->setVisible(false);
            if (m_readingAvatarLbl)
                m_readingAvatarLbl->clear();
            if (m_readingSubjectLbl) {
                m_readingSubjectLbl->setText(
                    MailI18n::t("📬 메일 제목을 클릭하면 본문을 불러옵니다",
                                "📬 Click an email subject to load its content",
                                "📬 メール件名をクリックすると本文を表示します"));
            }
            setReadingBodyHtml(
                MailI18n::t(
                    "<div style='padding:36px 24px;color:#64748B;font-size:13.5px;line-height:1.6;'>"
                    "좌측 목록에서 <b>메일 제목</b>을 클릭하면 해당 메일의 본문을 서버에서 불러와 표시합니다.<br/>"
                    "<span style='color:#94A3B8;font-size:12px;'>(체크박스를 체크할 때는 본문을 불러오지 않으며, 상단 🗑️ 휴지통 버튼이나 확장메뉴 ⋮에서 선택한 메일을 일괄 삭제/읽음 처리할 수 있습니다.)</span>"
                    "</div>",
                    "<div style='padding:36px 24px;color:#64748B;font-size:13.5px;line-height:1.6;'>"
                    "Click an <b>email subject</b> on the left to load and view its body.<br/>"
                    "<span style='color:#94A3B8;font-size:12px;'>(Checking boxes will not load email bodies.)</span>"
                    "</div>",
                    "<div style='padding:36px 24px;color:#64748B;font-size:13.5px;line-height:1.6;'>"
                    "左側のリストから<b>メールの件名</b>をクリックすると本文を読み込みます。"
                    "</div>"));
        }
    } else {
        // 목록이 비어 있을 때 좌측 메일 리스트 내부에도 안내 카드 아이템(realIdx = -1) 표시
        bool isSyncing = false;
        for (const AccountItem &acc : m_accounts) {
            if (acc.provider == AccountProviderType::Unified)
                continue;
            if (m_selectedAccountId != "acc_unified" && acc.id != m_selectedAccountId)
                continue;
            if (!acc.appPassword.isEmpty() || !acc.oauthAccessToken.isEmpty()) {
                if (m_selectedFolderId == QStringLiteral("starred")
                    || m_selectedFolderId == QStringLiteral("important")) {
                    const QString fKey = acc.id + "|" + m_selectedFolderId;
                    const QString inbKey = acc.id + "|inbox";
                    if (m_folderLoadingMore.contains(fKey) || m_folderLoadingMore.contains(inbKey)) {
                        isSyncing = true;
                        break;
                    }
                } else {
                    const QString fKey = acc.id + "|" + m_selectedFolderId;
                    if (!m_folderTotalExists.contains(fKey) || m_folderLoadingMore.contains(fKey)) {
                        isSyncing = true;
                        break;
                    }
                }
            }
        }

        QListWidgetItem *emptyItem = new QListWidgetItem(m_convListWidget);
        emptyItem->setFlags(Qt::NoItemFlags);
        emptyItem->setSizeHint(QSize(310, 140));
        emptyItem->setData(Qt::UserRole, -1);
        if (m_unreadOnlyFilter) {
            emptyItem->setData(Qt::DisplayRole,
                               MailI18n::t("📭 읽지 않은 메일이 없습니다",
                                           "📭 No unread messages",
                                           "📭 未読メールはありません"));
            emptyItem->setData(Qt::UserRole + 1,
                               MailI18n::t("이 폴더의 모든 메일을 읽었습니다.\n상단 [전체 보기 ✕] 전체 메일 표시.",
                                           "You're all caught up! Click [Show All ✕] above to view all messages.",
                                           "このフォルダの未読メールはありません。"));
        } else if (!m_mailSearchQuery.isEmpty()) {
            emptyItem->setData(Qt::DisplayRole,
                               MailI18n::t("🔍 검색 결과가 없습니다",
                                           "🔍 No matching messages",
                                           "🔍 検索結果がありません"));
            emptyItem->setData(Qt::UserRole + 1,
                               MailI18n::t("검색어와 일치하는 메일이 없습니다.",
                                           "No messages match your search query.",
                                           "一致するメールがありません。"));
        } else if (isSyncing && m_selectedFolderId != "snoozed") {
            emptyItem->setData(Qt::DisplayRole,
                               MailI18n::t("⏳ 메일 목록을 불러오는 중입니다...",
                                           "⏳ Loading messages...",
                                           "⏳ メールを読み込んでいます..."));
            emptyItem->setData(Qt::UserRole + 1,
                               MailI18n::t(QString("서버에서 %1페이지 메일을 가져 오는 중..").arg(m_currentMailPage),
                                           QString("Fetching page %1 messages from server...").arg(m_currentMailPage),
                                           QString("サーバーから%1ページのメールを取得しています...").arg(m_currentMailPage)));
        } else {
            emptyItem->setData(Qt::DisplayRole,
                               MailI18n::t("📭 표시할 메일이 없습니다",
                                           "📭 No messages in folder",
                                           "📭 メールがありません"));
            emptyItem->setData(Qt::UserRole + 1,
                               MailI18n::t("현재 선택한 폴더에 메일이 없습니다.",
                                           "There are no messages in this folder.",
                                           "このフォルダにはメールがありません。"));
        }

        listBlocker.unblock();
        if (m_scheduledMailBanner) {
            m_scheduledMailBanner->setVisible(false);
        }
        if (m_actRestoreBtn) {
            m_actRestoreBtn->setVisible(false);
        }
        if (m_readingAttachmentsBar) {
            m_readingAttachmentsBar->setVisible(false);
        }
        if (m_readingSenderLbl)
            m_readingSenderLbl->clear();
        if (m_readingTimeLbl)
            m_readingTimeLbl->clear();
        if (m_readingFolderChipLbl)
            m_readingFolderChipLbl->setText(MailI18n::folderTitle(m_selectedFolderId, m_selectedFolderTitle));
        if (m_blockSenderBtn)
            m_blockSenderBtn->setVisible(false);
        if (m_unsubscribeBtn)
            m_unsubscribeBtn->setVisible(false);
        if (m_unreadOnlyFilter) {
            m_readingSubjectLbl->setText(
                MailI18n::t("📭 읽지 않은 메일이 없습니다.",
                            "📭 No unread messages.",
                            "📭 未読メールはありません。"));
            setReadingBodyHtml(
                QString("<div style='padding:36px 24px;text-align:center;color:#4B5563;font-family:\"Malgun Gothic\",\"Segoe UI\",sans-serif;'>"
                        "<div style='font-size:36px;margin-bottom:12px;'>📭</div>"
                        "<div style='font-size:16px;font-weight:700;color:#111827;margin-bottom:8px;'>%1</div>"
                        "<div style='font-size:13px;color:#6B7280;margin-bottom:18px;'>%2</div>"
                        "<a href='solluna://show-all-mail' style='display:inline-block;background:#0D47A1;color:#FFFFFF;"
                        "text-decoration:none;font-weight:700;font-size:12.5px;padding:8px 18px;border-radius:6px;'>%3</a>"
                        "</div>")
                    .arg(MailI18n::t("현재 폴더에 읽지 않은 메일이 없습니다.",
                                     "There are no unread messages in this folder.",
                                     "現在のフォルダに未読メールはありません。"),
                         MailI18n::t("모든 새 메일을 확인하셨습니다. 전체 메일 목록으로 돌아가려면 아래 버튼을 클릭하세요.",
                                     "You have read all messages in this folder.",
                                     "すべてのメールを確認済みです。"),
                         MailI18n::t("📬 전체 메일 보기 (Show All Messages)",
                                     "📬 Show All Messages",
                                     "📬 すべてのメールを表示")));
        } else if (isSyncing && m_selectedFolderId != "snoozed") {
            m_readingSubjectLbl->setText(MailI18n::t("⏳ 편지함 메일을 불러오는 중입니다...", "⏳ Loading messages...", "⏳ メールを読み込んでいます..."));
            setReadingBodyHtml(
                QString("<div style='color:#6B7280;padding:24px;font-size:13px;'>%1</div>")
                    .arg(MailI18n::t("서버에서 편지함 메일 목록을 빠르게 동기화하고 있습니다. 잠시만 기다려 주세요...",
                                     "Synchronizing folder messages from server. Please wait...",
                                     "サーバーからフォルダのメール一覧を同期しています。しばらくお待ちください...")));
        } else {
            m_readingSubjectLbl->setText(MailI18n::t("표시할 메시지가 없습니다.", "No messages to display.", "表示するメッセージがありません。"));
            setReadingBodyHtml(
                QString("<div style='color:#9CA3AF;padding:24px;'>%1</div>")
                    .arg(MailI18n::t("이 폴더에 표시할 메일이 없습니다.",
                                     "There are no messages in this folder.",
                                     "このフォルダには表示するメールがありません。")));
        }
    }

    // 현재 선택된 계정·폴더·페이지에 맞춰 상단 윈도우 타이틀 즉시 동기화
    QString titleAccLabel = MailI18n::t("통합 편지함", "Unified Inbox", "統合受信トレイ");
    if (m_selectedAccountId != QStringLiteral("acc_unified")) {
        for (const AccountItem &acc : m_accounts) {
            if (acc.id == m_selectedAccountId) {
                titleAccLabel = acc.email.trimmed().isEmpty() ? acc.displayName : acc.email.trimmed();
                break;
            }
        }
    }
    const QString titleFolderLabel = MailI18n::folderTitle(m_selectedFolderId, m_selectedFolderTitle);
    if (!m_filteredConvIndices.isEmpty()) {
        setWindowTitle(
            MailI18n::t(QString("SollunaMail — [%1] '%2' %3페이지 (%4건 표시 / 전체 %5건)")
                            .arg(titleAccLabel, titleFolderLabel)
                            .arg(m_currentMailPage)
                            .arg(m_filteredConvIndices.size())
                            .arg(effectiveTotalItems),
                        QString("SollunaMail — [%1] '%2' Page %3 (%4 shown / %5 total)")
                            .arg(titleAccLabel, titleFolderLabel)
                            .arg(m_currentMailPage)
                            .arg(m_filteredConvIndices.size())
                            .arg(effectiveTotalItems),
                        QString("SollunaMail — [%1] '%2' %3ページ (%4件表示 / 全%5件)")
                            .arg(titleAccLabel, titleFolderLabel)
                            .arg(m_currentMailPage)
                            .arg(m_filteredConvIndices.size())
                            .arg(effectiveTotalItems)));
    } else {
        setWindowTitle(QString("SollunaMail — [%1] '%2'").arg(titleAccLabel, titleFolderLabel));
    }
    updateScheduledMailStatusIndicator();
}

void MainWindow::appendMoreConversationRows(int pageSize)
{
    if (!m_convListWidget)
        return;

    const int totalFiltered = m_filteredConvIndices.size();
    const int targetEnd = (pageSize <= 0) ? m_renderedConvRows : qMin(totalFiltered, m_renderedConvRows + pageSize);

    if (targetEnd > m_renderedConvRows) {
        QSignalBlocker listBlocker(m_convListWidget);
        for (int row = m_renderedConvRows; row < targetEnd; ++row) {
            int realIdx = m_filteredConvIndices[row];
            if (realIdx < 0 || realIdx >= m_conversations.size())
                continue;

            QListWidgetItem *item = new QListWidgetItem(m_convListWidget);
            item->setSizeHint(QSize(310, 78));
            item->setData(Qt::UserRole, realIdx);
            item->setData(Qt::UserRole + 1, m_conversations[realIdx].id);
        }
        m_renderedConvRows = targetEnd;
    }

    // 상단 폴더 제목은 우측 5개 아이콘과 절대 겹치지 않도록 순수 편지함 이름만 표시하고, 상세 건수는 툴팁으로 제공
    if (m_convPaneTitleLbl) {
        QString baseTitle = MailI18n::folderTitle(m_selectedFolderId, m_selectedFolderTitle);
        int serverTotal = 0;
        for (const AccountItem &acc : m_accounts) {
            if (acc.provider == AccountProviderType::Unified)
                continue;
            if (m_selectedAccountId != "acc_unified" && acc.id != m_selectedAccountId)
                continue;
            serverTotal += m_folderTotalExists.value(acc.id + "|" + m_selectedFolderId, 0);
        }
        m_convPaneTitleLbl->setText(baseTitle);
        if (serverTotal > totalFiltered) {
            m_convPaneTitleLbl->setToolTip(QString("%1 (%2 / %3)").arg(baseTitle).arg(m_renderedConvRows).arg(serverTotal));
        } else if (totalFiltered > 0) {
            m_convPaneTitleLbl->setToolTip(QString("%1 (%2)").arg(baseTitle).arg(totalFiltered));
        } else {
            m_convPaneTitleLbl->setToolTip(baseTitle);
        }
    }
}

int MainWindow::lowestLoadedImapSeqForFolder(const QString &accountId, const QString &folderId) const
{
    int minSeq = 0;
    const QString prefix = QString("imap_%1_%2_").arg(accountId, folderId);
    for (const ConversationItem &c : m_conversations) {
        if (c.accountId == accountId && c.id.startsWith(prefix)) {
            bool ok = true;
            int seq = (c.imapSeq > 0) ? c.imapSeq : c.id.mid(prefix.size()).toInt(&ok);
            if (ok && seq > 0) {
                if (minSeq == 0 || seq < minSeq)
                    minSeq = seq;
            }
        }
    }
    return minSeq;
}

int MainWindow::countLoadedConversationsForFolder(const QString &accountId, const QString &folderId) const
{
    int cnt = 0;
    for (const ConversationItem &c : m_conversations) {
        if (accountId != "acc_unified" && c.accountId != accountId)
            continue;
        bool matchFolder = (folderId == "starred") ? c.isStarred : (c.folderId == folderId);
        if (matchFolder)
            ++cnt;
    }
    return cnt;
}

void MainWindow::onConversationListScrolled(int value)
{
    // 자동 스크롤 로드(Infinite Scroll) 비활성화 — 하단 20개 단위 페이지네이션 바(onSelectMailPage)로만 페이지를 전환합니다.
    Q_UNUSED(value);
}

void MainWindow::updateMailPaginationBar()
{
    if (!m_mailPaginationBar)
        return;

    const int kPageSize = 20;
    int serverTotal = 0;
    for (const AccountItem &acc : m_accounts) {
        if (acc.provider == AccountProviderType::Unified)
            continue;
        if (m_selectedAccountId != "acc_unified" && acc.id != m_selectedAccountId)
            continue;
        serverTotal += m_folderTotalExists.value(acc.id + "|" + m_selectedFolderId, 0);
    }

    int loadedMatching = 0;
    for (const ConversationItem &c : m_conversations) {
        if (m_selectedAccountId != "acc_unified" && c.accountId != m_selectedAccountId)
            continue;
        bool matchFolder = false;
        if (m_selectedFolderId == "starred") {
            matchFolder = (c.isStarred && c.folderId != "trash" && c.folderId != "spam");
        } else if (m_selectedFolderId == "important") {
            matchFolder = ((c.isImportant || c.folderId == "important") && c.folderId != "trash" && c.folderId != "spam");
        } else {
            matchFolder = (c.folderId == m_selectedFolderId);
        }
        if (!matchFolder)
            continue;
        if (m_unreadOnlyFilter && !c.isUnread)
            continue;
        if (!m_mailSearchQuery.isEmpty()) {
            const MessageItem &m = c.latestMessage();
            bool match = c.subject.contains(m_mailSearchQuery, Qt::CaseInsensitive) ||
                         m.senderName.contains(m_mailSearchQuery, Qt::CaseInsensitive) ||
                         m.recipientSummary.contains(m_mailSearchQuery, Qt::CaseInsensitive) ||
                         m.snippet.contains(m_mailSearchQuery, Qt::CaseInsensitive);
            if (!match)
                continue;
        }
        ++loadedMatching;
    }

    const bool isUnfilteredStandardView = (m_mailSearchQuery.isEmpty() && !m_unreadOnlyFilter && m_selectedFolderId != "snoozed");
    const int effectiveTotal = isUnfilteredStandardView ? qMax(loadedMatching, serverTotal) : loadedMatching;
    m_totalMailPages = qMax(1, (effectiveTotal + kPageSize - 1) / kPageSize);
    m_currentMailPage = qBound(1, m_currentMailPage, m_totalMailPages);

    const int blockStart = ((m_currentMailPage - 1) / 5) * 5 + 1;
    const int blockEnd = qMin(m_totalMailPages, blockStart + 4);

    for (int i = 0; i < m_pageNumBtns.size(); ++i) {
        QPushButton *btn = m_pageNumBtns[i];
        if (!btn)
            continue;
        const int pageNum = blockStart + i;
        if (pageNum <= blockEnd) {
            btn->setVisible(true);
            btn->setText(QString::number(pageNum));
            btn->setProperty("pageNumber", pageNum);
            const int rangeStart = (pageNum - 1) * kPageSize + 1;
            const int rangeEnd = (effectiveTotal > 0) ? qMin(effectiveTotal, pageNum * kPageSize) : (pageNum * kPageSize);
            btn->setToolTip(
                MailI18n::t(QString("%1페이지 (%2 ~ %3번째 메일)").arg(pageNum).arg(rangeStart).arg(rangeEnd),
                            QString("Page %1 (%2 - %3)").arg(pageNum).arg(rangeStart).arg(rangeEnd),
                            QString("%1ページ (%2〜%3件目)").arg(pageNum).arg(rangeStart).arg(rangeEnd)));
            if (pageNum == m_currentMailPage) {
                // 선택된 페이지: 파란 라운드 테두리 + 파란 글씨
                btn->setStyleSheet(
                    "QPushButton {"
                    "  background-color: #FFFFFF;"
                    "  color: #2563EB;"
                    "  border: 1.5px solid #2563EB;"
                    "  border-radius: 6px;"
                    "  font-size: 13px;"
                    "  font-weight: 700;"
                    "  padding: 0px;"
                    "}");
            } else {
                // 비활성 페이지 번호: 테두리 없는 깔끔한 숫자 버튼
                btn->setStyleSheet(
                    "QPushButton {"
                    "  background-color: transparent;"
                    "  color: #374151;"
                    "  border: none;"
                    "  border-radius: 6px;"
                    "  font-size: 13px;"
                    "  font-weight: 500;"
                    "  padding: 0px;"
                    "}"
                    "QPushButton:hover {"
                    "  background-color: #F3F4F6;"
                    "  color: #111827;"
                    "}");
            }
        } else {
            btn->setVisible(false);
        }
    }

    if (m_pageFirstBlockBtn)
        m_pageFirstBlockBtn->setEnabled(m_currentMailPage > 1);
    if (m_pagePrevBtn)
        m_pagePrevBtn->setEnabled(m_currentMailPage > 1);
    if (m_pageNextBtn)
        m_pageNextBtn->setEnabled(m_currentMailPage < m_totalMailPages);
    if (m_pageLastBlockBtn)
        m_pageLastBlockBtn->setEnabled(m_currentMailPage < m_totalMailPages);
}

void MainWindow::onSelectMailPage(int page)
{
    updateMailPaginationBar();
    const int targetPage = qBound(1, page, m_totalMailPages);
    m_currentMailPage = targetPage;

    const int kPageSize = 20;
    bool triggeredFetch = false;

    for (const AccountItem &acc : m_accounts) {
        if (acc.provider == AccountProviderType::Unified)
            continue;
        if (m_selectedAccountId != "acc_unified" && acc.id != m_selectedAccountId)
            continue;
        if (acc.appPassword.isEmpty() && acc.oauthAccessToken.isEmpty())
            continue;

        const QString folderKey = acc.id + "|" + m_selectedFolderId;
        const int totalEx = m_folderTotalExists.value(folderKey, 0);
        if (totalEx <= 0)
            continue;

        const int pageEndSeq = totalEx - (m_currentMailPage - 1) * kPageSize;
        const int pageStartSeq = qMax(1, pageEndSeq - kPageSize + 1);
        if (pageEndSeq < 1)
            continue;

        // 해당 페이지 구간 [pageStartSeq .. pageEndSeq]의 메일이 이미 메모리에 로드되어 있는지 확인
        int loadedInSlice = 0;
        const QString prefix = QString("imap_%1_%2_").arg(acc.id, m_selectedFolderId);
        for (const ConversationItem &c : m_conversations) {
            if (c.accountId == acc.id && c.id.startsWith(prefix)) {
                bool ok = true;
                const int seq = (c.imapSeq > 0) ? c.imapSeq : c.id.mid(prefix.size()).toInt(&ok);
                if (ok && seq >= pageStartSeq && seq <= pageEndSeq) {
                    ++loadedInSlice;
                }
            }
        }

        const int expectedSliceCount = pageEndSeq - pageStartSeq + 1;
        if (loadedInSlice < qMin(10, expectedSliceCount)
            && (!m_folderLoadingMore.contains(folderKey) || m_activePageFetchEndSeq.value(folderKey, -1) != pageEndSeq)) {
            m_folderLoadingMore.insert(folderKey);
            m_activePageFetchEndSeq[folderKey] = pageEndSeq;
            triggeredFetch = true;
            m_googleProvider->fetchImapFolder(acc, m_selectedFolderId, kPageSize, pageEndSeq);
        }
    }

    if (triggeredFetch) {
        showMailLoadingToast(QString(), 2000);
    }

    refreshConversationList();
    if (m_convListWidget) {
        m_convListWidget->scrollToTop();
    }

    if (m_selectedAccountId != "acc_unified") {
        const QString folderKey = m_selectedAccountId + QStringLiteral("|") + m_selectedFolderId;
        const int serverTotal = m_folderTotalExists.value(folderKey, 0);
        if (serverTotal > 0) {
            QString accEmail = m_selectedAccountId;
            for (const AccountItem &a : m_accounts) {
                if (a.id == m_selectedAccountId) {
                    accEmail = a.email;
                    break;
                }
            }
            setWindowTitle(
                MailI18n::t(
                    QString("SollunaMail — [%1] '%2' %3페이지 (%4건 표시 / 전체 %5건)"),
                    QString("SollunaMail — [%1] '%2' Page %3 (%4 shown / %5 total)"),
                    QString("SollunaMail — [%1] '%2' %3ページ (%4件表示 / 全%5件)"))
                    .arg(accEmail, MailI18n::folderTitle(m_selectedFolderId, m_selectedFolderTitle))
                    .arg(m_currentMailPage)
                    .arg(m_filteredConvIndices.size())
                    .arg(serverTotal));
        }
    }
}

static bool parseImapConversationId(const QString &cid, QString &outFolderId, int &outSeqNum)
{
    outFolderId.clear();
    outSeqNum = 0;
    if (!cid.startsWith(QStringLiteral("imap_")))
        return false;
    const int lastUnder = cid.lastIndexOf('_');
    if (lastUnder <= 5)
        return false;
    outSeqNum = cid.mid(lastUnder + 1).toInt();
    if (outSeqNum <= 0)
        return false;

    // cid 형식: imap_<accountId>_<folderId>_<seqNum>
    // <accountId>는 "acc_<8hex>" 형태(언더스코어 1개 포함)이거나 단일 토큰이며,
    // <folderId>에는 "custom_acc_e50b57ab_root"처럼 언더스코어가 여러 개 포함될 수 있음.
    const QString body = cid.mid(5, lastUnder - 5); // "<accountId>_<folderId>"
    if (body.startsWith(QStringLiteral("acc_"))) {
        const int secondUnder = body.indexOf('_', 4);
        if (secondUnder > 4 && secondUnder + 1 < body.size()) {
            outFolderId = body.mid(secondUnder + 1);
            return !outFolderId.isEmpty();
        }
    }
    const int firstUnder = body.indexOf('_');
    if (firstUnder > 0 && firstUnder + 1 < body.size()) {
        outFolderId = body.mid(firstUnder + 1);
        return !outFolderId.isEmpty();
    }
    return false;
}

static QString renderRoundedCtaButtonDataUri(const QString &labelText,
                                             const QString &bgHexColor,
                                             int zoomPercent)
{
    QString cleanText = labelText;
    cleanText.remove(QRegularExpression(R"(<[^>]+>)"));
    cleanText.replace(QLatin1String("&nbsp;"), QLatin1String(" "))
             .replace(QLatin1String("&amp;"), QLatin1String("&"))
             .replace(QLatin1String("&lt;"), QLatin1String("<"))
             .replace(QLatin1String("&gt;"), QLatin1String(">"))
             .replace(QRegularExpression(R"(\s+)"), QLatin1String(" "));
    cleanText = cleanText.trimmed();
    if (cleanText.isEmpty())
        return QString();

    const double scale = qBound(0.75, zoomPercent / 100.0, 2.4);
    const int dpr = 2;
    QFont font(QStringLiteral("Malgun Gothic"));
    font.setPixelSize(qMax(11, int(qRound(13.0 * scale))));
    font.setWeight(QFont::Bold);

    QFontMetrics fm(font);
    const int textW = fm.horizontalAdvance(cleanText);
    const int padX = int(qRound(26.0 * scale));
    const int btnW = qMax(int(qRound(110.0 * scale)), textW + padX * 2);
    const int btnH = int(qRound(40.0 * scale));

    QColor bgCol(bgHexColor);
    if (!bgCol.isValid() || bgHexColor.compare(QStringLiteral("#414141"), Qt::CaseInsensitive) == 0) {
        bgCol = QColor("#0F172A"); // 다크 캡슐 버튼 톤(#000000~#0F172A)
    }

    QImage img(btnW * dpr, btnH * dpr, QImage::Format_ARGB32_Premultiplied);
    img.setDevicePixelRatio(dpr);
    img.fill(Qt::transparent);

    QPainter p(&img);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setRenderHint(QPainter::TextAntialiasing, true);
    p.setPen(Qt::NoPen);
    p.setBrush(bgCol);
    const qreal radius = btnH / 2.0;
    p.drawRoundedRect(QRectF(0.5, 0.5, btnW - 1.0, btnH - 1.0), radius, radius);

    p.setPen(QColor("#FFFFFF"));
    p.setFont(font);
    p.drawText(QRectF(0, 0, btnW, btnH), Qt::AlignCenter, cleanText);
    p.end();

    QByteArray pngBytes;
    QBuffer buf(&pngBytes);
    buf.open(QIODevice::WriteOnly);
    img.save(&buf, "PNG");
    return QStringLiteral("data:image/png;base64,") + QString::fromLatin1(pngBytes.toBase64());
}

void MainWindow::handleEmailLinkActivated(const QUrl &url)
{
    if (!url.isValid())
        return;
    if (url.toString() == QLatin1String("solluna://show-all-mail")) {
        m_unreadOnlyFilter = false;
        if (m_btnUnreadFilter) {
            QSignalBlocker b(m_btnUnreadFilter);
            m_btnUnreadFilter->setChecked(false);
        }
        refreshConversationList();
        return;
    }
    if (url.scheme().compare(QLatin1String("mailto"), Qt::CaseInsensitive) == 0) {
        onComposeNewMail(url.path());
        return;
    }
    QDesktopServices::openUrl(url);
}

void MainWindow::setReadingBodyHtml(const QString &html, const QString &convId, bool isRichEmail)
{
#ifdef SOLLUNA_HAS_WEBENGINE
    if (m_readingWebView) {
        m_readingWebView->setZoomFactor(qBound(0.75, m_readingBodyZoomPercent / 100.0, 2.5));
        m_readingWebView->setHtml(html, QUrl(QStringLiteral("https://mail.solluna.local/")));
        return;
    }
#endif
    if (m_readingBodyBrowser) {
        m_readingBodyBrowser->setProperty("rawSafeHtml", html);
        m_readingBodyBrowser->setHtml(html);
        if (isRichEmail && (!m_privacySettings.blockAllImages || m_allowedExternalImageConvIds.contains(convId))) {
            loadAllowedExternalImagesIntoBrowser(html, convId, m_readingBodyBrowser);
        }
    }
}

QString MainWindow::prepareEmailHtmlForChromiumViewer(const QString &rawHtml,
                                                      const QString &senderName,
                                                      const QString &senderEmail) const
{
    if (rawHtml.trimmed().isEmpty())
        return rawHtml;

    QString html = rawHtml;
    html.remove(QStringLiteral("<!--SM_PARTIAL-->"));

    // 1. CORS 헤더 부재로 Chromium 콘솔에 CORS policy 차단 에러를 일으키는 외부 원격 웹폰트(@font-face) 선언 제거
    static const QRegularExpression remoteFontFaceRe(
        R"(@font-face\s*\{[^}]*https?://[^}]*\})",
        QRegularExpression::CaseInsensitiveOption);
    html.remove(remoteFontFaceRe);

    // 2. https://mail.solluna.local/ 오리진에서 Mixed Content 경고를 일으키는 외부 http:// 이미지(src="http://...", url('http://...'))를 https://로 사전 정규화
    static const QRegularExpression insecureImgSrcRe(
        R"((\b(?:src|background)\s*=\s*["']\s*)http://(?!localhost\b|127\.0\.0\.1\b))",
        QRegularExpression::CaseInsensitiveOption);
    html.replace(insecureImgSrcRe, QStringLiteral("\\1https://"));

    static const QRegularExpression insecureCssUrlRe(
        R"((url\s*\(\s*["']?\s*)http://(?!localhost\b|127\.0\.0\.1\b))",
        QRegularExpression::CaseInsensitiveOption);
    html.replace(insecureCssUrlRe, QStringLiteral("\\1https://"));

    // 구버전 캐시에서 외부 로고 <img>가 제거되어 빈 <a href="...chatgpt.com..."></a>만 남은 경우 ChatGPT 브랜드 로고 복원
    if (senderName.contains(QStringLiteral("OpenAI"), Qt::CaseInsensitive)
        || senderEmail.contains(QStringLiteral("openai.com"), Qt::CaseInsensitive)
        || html.contains(QStringLiteral("chatgpt.com"), Qt::CaseInsensitive)) {
        static const QRegularExpression emptyChatGptLogoLinkRe(
            R"((<a\b[^>]*chatgpt\.com[^>]*>)\s*(</a\s*>))",
            QRegularExpression::CaseInsensitiveOption);
        html.replace(emptyChatGptLogoLinkRe,
                     QStringLiteral("\\1<span style=\"display:inline-block;margin:2px 0 8px 0;font-family:'Segoe UI',Helvetica,Arial,sans-serif;font-size:28px;font-weight:800;color:#000000;letter-spacing:-0.6px;text-decoration:none;\">ChatGPT</span>\\2"));

        static const QRegularExpression emptyLogoTdRe(
            R"(<td\b[^>]*width\s*:\s*105px[^>]*>\s*</td>)",
            QRegularExpression::CaseInsensitiveOption);
        html.replace(emptyLogoTdRe,
                     QStringLiteral("<td><div style='margin:6px 0 18px 0;font-size:28px;font-weight:800;color:#000000;letter-spacing:-0.6px;'>ChatGPT</div></td>"));
    }

    const QString injectedHeadCss = QStringLiteral(
        "<meta charset=\"utf-8\">"
        "<style>"
        "html, body { margin: 0; padding: 8px 10px; background: #ffffff; color: #111827; "
        "  font-family: 'Malgun Gothic', 'Segoe UI', -apple-system, 'Apple SD Gothic Neo', 'Noto Sans CJK KR', sans-serif; "
        "  line-height: 1.6; word-break: break-word; }"
        "img { max-width: 100%; height: auto; }"
        "td[bgcolor=\"#414141\"], td[bgcolor=\"#414141\"] a { "
        "  background-color: #000000 !important; color: #ffffff !important; "
        "  border-radius: 99px !important; text-decoration: none !important; font-weight: 600 !important; }"
        "</style>");

    if (html.contains(QStringLiteral("<html"), Qt::CaseInsensitive)) {
        return injectedHeadCss + html;
    }
    return QStringLiteral("<!DOCTYPE html><html><head>%1</head><body>%2</body></html>")
        .arg(injectedHeadCss, html);
}

QString MainWindow::normalizeEmailHtmlForQtViewer(const QString &rawHtml,
                                                  const QString &senderName,
                                                  const QString &senderEmail) const
{
    if (rawHtml.trimmed().isEmpty())
        return rawHtml;

    QString html = rawHtml;
    html.remove(QStringLiteral("<!--SM_PARTIAL-->"));
    html.remove(QRegularExpression(R"(<style\b[^>]*>[\s\S]*?</style>)", QRegularExpression::CaseInsensitiveOption));
    html.remove(QRegularExpression(R"(<head\b[^>]*>[\s\S]*?</head>)", QRegularExpression::CaseInsensitiveOption));

    // 0. 멀티라인 닫는 태그(</a\n  > 등)를 표준 </a>로 정규화
    html.replace(QRegularExpression(R"(</a\s+>)", QRegularExpression::CaseInsensitiveOption),
                 QStringLiteral("</a>"));

    // 1. MJML / 반응형 이메일에서 흔히 쓰는 font-size:0px 또는 1px (래퍼에 지정되어 하위 텍스트를 깨뜨리거나 폭 계산을 망가뜨림) 제거
    static const QRegularExpression zeroOrOneFontRe(
        R"(font-size\s*:\s*[01](?:\.0+)?\s*(?:px|pt|em|rem|%)?\s*;?)",
        QRegularExpression::CaseInsensitiveOption);
    html.replace(zeroOrOneFontRe, QString());

    // 2. 시스템 폰트 별칭을 크로스플랫폼 안전 폰트로 치환
    static const QRegularExpression sysFontRe(
        R"((?:-apple-system|BlinkMacSystemFont|system-ui|\bsystem\b))",
        QRegularExpression::CaseInsensitiveOption);
    html.replace(sysFontRe, QStringLiteral("'Segoe UI'"));

    // 3. 구버전 캐시에서 외부 <img>가 제거되어 빈 <a href="...chatgpt.com..."></a> 또는 <td style="width:105px;">만 남은 OpenAI/ChatGPT 메일 브랜드 헤더 복원
    if (senderName.contains(QStringLiteral("OpenAI"), Qt::CaseInsensitive)
        || senderEmail.contains(QStringLiteral("openai.com"), Qt::CaseInsensitive)
        || html.contains(QStringLiteral("chatgpt.com"), Qt::CaseInsensitive)) {
        static const QRegularExpression emptyChatGptLogoLinkRe(
            R"(<a\b[^>]*chatgpt\.com[^>]*>\s*</a>)",
            QRegularExpression::CaseInsensitiveOption);
        html.replace(emptyChatGptLogoLinkRe,
                     QStringLiteral("<div style='margin:6px 0 18px 0;font-size:28px;font-weight:800;color:#000000;letter-spacing:-0.6px;'>ChatGPT</div>"));

        static const QRegularExpression emptyLogoTdRe(
            R"(<td\b[^>]*width\s*:\s*105px[^>]*>\s*</td>)",
            QRegularExpression::CaseInsensitiveOption);
        html.replace(emptyLogoTdRe,
                     QStringLiteral("<td><div style='margin:6px 0 18px 0;font-size:28px;font-weight:800;color:#000000;letter-spacing:-0.6px;'>ChatGPT</div></td>"));
    }

    // 4. QTextBrowser에서 display:inline-block / width:100% / max-width / 고정 line-height / 고정 height 조합이
    //    중첩 표 안에서 최소 폭(2글자 세로줄)으로 축소되거나 큰 배너 이미지와 글자가 겹치게 만드는 현상 방지
    static const QRegularExpression inlineBlockRe(
        R"(display\s*:\s*inline-block\s*;?)",
        QRegularExpression::CaseInsensitiveOption);
    html.replace(inlineBlockRe, QStringLiteral("display:block;"));

    static const QRegularExpression fixedLineHeightOrBoxHeightRe(
        R"((?:line-height|max-height|min-height)\s*:\s*[^;>\"']+\s*;?)",
        QRegularExpression::CaseInsensitiveOption);
    html.replace(fixedLineHeightOrBoxHeightRe, QString());

    // 4-2. <img> 태그에 width="600" height="auto" 등이 지정되어 있으면 QTextHtmlParser가 height="auto"를 0으로 파싱하여
    //      배너 이미지(Watch Now 등)와 본문 텍스트가 세로로 겹치는 문제를 해결하기 위해,
    //      추적 픽셀(0/1px)이 아닌 일반 <img>의 고정 width/height 속성을 제거하고 실제 다운로드된 QImage 크기로 레이아웃되도록 정규화
    {
        static const QRegularExpression imgTagNormRe(
            R"(<img\b([^>]*)>)",
            QRegularExpression::CaseInsensitiveOption);
        static const QRegularExpression tinyPixelImgRe(
            R"(\b(?:width|height)\s*=\s*["']?[01](?:px)?["']?\b|width\s*:\s*[01](?:px)?\b|height\s*:\s*[01](?:px)?\b)",
            QRegularExpression::CaseInsensitiveOption);
        static const QRegularExpression imgDimAttrRe(
            R"(\s+\b(?:width|height)\s*=\s*(?:["'][^"']*["']|[^\s>]+))",
            QRegularExpression::CaseInsensitiveOption);
        static const QRegularExpression imgStyleDimRe(
            R"((?:max-width|min-width|width|max-height|min-height|height)\s*:\s*[^;\"']+\s*;?)",
            QRegularExpression::CaseInsensitiveOption);

        QString normImgHtml;
        int lastPos = 0;
        QRegularExpressionMatchIterator it = imgTagNormRe.globalMatch(html);
        while (it.hasNext()) {
            const QRegularExpressionMatch mImg = it.next();
            normImgHtml.append(html.mid(lastPos, mImg.capturedStart() - lastPos));
            const QString fullTag = mImg.captured(0);
            QString attrs = mImg.captured(1);
            if (tinyPixelImgRe.match(fullTag).hasMatch()) {
                normImgHtml.append(fullTag);
            } else {
                attrs.replace(imgDimAttrRe, QString());
                attrs.replace(imgStyleDimRe, QString());
                normImgHtml.append(QStringLiteral("<div style=\"margin:6px 0;\"><img%1></div>").arg(attrs));
            }
            lastPos = mImg.capturedEnd();
        }
        normImgHtml.append(html.mid(lastPos));
        html = normImgHtml;
    }

    // 5. 배경색이 지정된 CTA 버튼 셀(<td bgcolor="#414141" ...><a ...>결제 업데이트하기</a></td>)은
    //    QPainter 고해상도(2x) 안티앨리어싱 둥근 캡슐(border-radius) 버튼 이미지로 렌더링하여
    //    QTextBrowser에서도 파란 밑줄 없이 둥근 캡슐 버튼 + 선명한 흰색 글씨로 표시
    QStringList preservedButtons;
    static const QRegularExpression ctaTdRe(
        R"(<td\b([^>]*?(?:bgcolor\s*=\s*["']#[0-9a-fA-F]{3,6}["']|background(?:-color)?\s*:\s*#[0-9a-fA-F]{3,6})[^>]*?)>\s*(<a\b([^>]*)>([\s\S]*?)</a>)\s*</td\s*>)",
        QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression hrefExtractRe(
        R"(\bhref\s*=\s*["']([^"']+)["'])",
        QRegularExpression::CaseInsensitiveOption);
    {
        QString out;
        int lastPos = 0;
        QRegularExpressionMatchIterator it = ctaTdRe.globalMatch(html);
        while (it.hasNext()) {
            const QRegularExpressionMatch m = it.next();
            out.append(html.mid(lastPos, m.capturedStart() - lastPos));
            const QString tdAttrs = m.captured(1);
            const QString aTag = m.captured(2);
            const QString aAttrs = m.captured(3);
            const QString innerHtml = m.captured(4);

            QString bgHex = QStringLiteral("#0F172A");
            static const QRegularExpression bgExtractRe(
                R"((?:bgcolor\s*=\s*["'](#[0-9a-fA-F]{3,6})["']|background(?:-color)?\s*:\s*(#[0-9a-fA-F]{3,6})))",
                QRegularExpression::CaseInsensitiveOption);
            const QRegularExpressionMatch bgMatch = bgExtractRe.match(tdAttrs);
            if (bgMatch.hasMatch()) {
                bgHex = !bgMatch.captured(1).isEmpty() ? bgMatch.captured(1) : bgMatch.captured(2);
            }
            // 흰색/투명 배경 셀은 일반 링크이므로 버튼으로 감싸지 않음
            if (bgHex.compare(QStringLiteral("#ffffff"), Qt::CaseInsensitive) == 0
                || bgHex.compare(QStringLiteral("#fff"), Qt::CaseInsensitive) == 0) {
                out.append(m.captured(0));
            } else {
                const QRegularExpressionMatch hm = hrefExtractRe.match(aAttrs);
                const QString hrefUrl = hm.hasMatch() ? hm.captured(1) : QStringLiteral("#");
                const QString pillDataUri = !innerHtml.contains(QStringLiteral("<img"), Qt::CaseInsensitive)
                                                ? renderRoundedCtaButtonDataUri(innerHtml, bgHex, m_readingBodyZoomPercent)
                                                : QString();
                const int idx = preservedButtons.size();
                if (!pillDataUri.isEmpty()) {
                    const QString pillBtnHtml = QStringLiteral(
                        "<div style=\"margin:16px 0;\"><a href=\"%1\" style=\"text-decoration:none;\"><img src=\"%2\" border=\"0\" /></a></div>")
                        .arg(hrefUrl, pillDataUri);
                    preservedButtons.append(pillBtnHtml);
                } else {
                    const QString btnTable = QStringLiteral(
                        "<table border=\"0\" cellpadding=\"0\" cellspacing=\"0\" style=\"margin:16px 0;\">"
                        "<tr><td bgcolor=\"%1\" style=\"background-color:%1;padding:10px 22px;\">%2</td></tr>"
                        "</table>").arg(bgHex, aTag);
                    preservedButtons.append(btnTable);
                }
                out.append(QStringLiteral("<!--SM_CTA_BTN_%1-->").arg(idx));
            }
            lastPos = m.capturedEnd();
        }
        out.append(html.mid(lastPos));
        html = out;
    }

    // 6. 다중 중첩 레이아웃 <table>이 QTextDocumentLayout에서 폭 72px(2글자 폭)로 붕괴되는 문제를 해결하기 위해
    //    레이아웃용 table/tbody/thead/tfoot/tr/td/th 래퍼를 전체 폭 블록으로 평탄화
    static const QRegularExpression tableTagsRe(
        R"(</?(?:table|tbody|thead|tfoot)\b[^>]*>)",
        QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression trOpenRe(
        R"(<tr\b[^>]*>)",
        QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression trCloseRe(
        R"(</tr\s*>)",
        QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression tdOpenRe(
        R"(<t[dh]\b[^>]*>)",
        QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression tdCloseRe(
        R"(</t[dh]\s*>)",
        QRegularExpression::CaseInsensitiveOption);

    html.replace(tableTagsRe, QString());
    html.replace(trOpenRe, QStringLiteral("<div>"));
    html.replace(trCloseRe, QStringLiteral("</div>"));
    html.replace(tdOpenRe, QString());
    html.replace(tdCloseRe, QString());

    // 7. 보존해 둔 CTA 버튼 복원
    for (int i = 0; i < preservedButtons.size(); ++i) {
        html.replace(QStringLiteral("<!--SM_CTA_BTN_%1-->").arg(i), preservedButtons.at(i));
    }

    const int effectiveFontPx = qBound(10, (14 * m_readingBodyZoomPercent) / 100, 36);
    return QStringLiteral(
        "<div style=\"font-family:'Malgun Gothic','Segoe UI',sans-serif;font-size:%1px;line-height:1.65;color:#111827;padding:10px 14px;\">%2</div>")
        .arg(effectiveFontPx)
        .arg(html);
}

void MainWindow::loadAllowedExternalImagesIntoBrowser(const QString &html, const QString &convId, QTextBrowser *targetBrowser)
{
    QTextBrowser *browser = targetBrowser ? targetBrowser : m_readingBodyBrowser;
    if (!browser || !browser->document())
        return;

    browser->setProperty("rawSafeHtml", html);
    const int maxImgWidth = qBound(420, (640 * m_readingBodyZoomPercent) / 100, 1100);

    // data:image/...;base64,... 인라인 이미지 리소스 등록 (QTextBrowser 폴백 뷰어 지원)
    static const QRegularExpression dataImgRe(
        R"rx(<img\b[^>]*\bsrc\s*=\s*["'](data:image/[a-zA-Z0-9.+-]+;base64,([A-Za-z0-9+/=\r\n\s]+))["'])rx",
        QRegularExpression::CaseInsensitiveOption);
    bool appliedCachedToDoc = false;
    QRegularExpressionMatchIterator dataIt = dataImgRe.globalMatch(html);
    while (dataIt.hasNext()) {
        const QRegularExpressionMatch dm = dataIt.next();
        const QString fullDataUri = dm.captured(1);
        QByteArray b64 = dm.captured(2).toLatin1();
        b64.replace("\r", "").replace("\n", "").replace(" ", "").replace("\t", "");
        QImage inlineImg = QImage::fromData(QByteArray::fromBase64(b64));
        if (!inlineImg.isNull()) {
            if (inlineImg.width() > maxImgWidth) {
                inlineImg = inlineImg.scaledToWidth(maxImgWidth, Qt::SmoothTransformation);
            }
            browser->document()->addResource(
                QTextDocument::ImageResource, QUrl(fullDataUri), QVariant(inlineImg));
            appliedCachedToDoc = true;
        }
    }

    static const QRegularExpression extImgUrlRe(
        R"(<img\b[^>]*\bsrc\s*=\s*["'](https?://[^"'>\s]+)["'])",
        QRegularExpression::CaseInsensitiveOption);

    if (!m_extImgNam) {
        m_extImgNam = new QNetworkAccessManager(this);
    }

    QSet<QString> urlsInThisHtml;
    QRegularExpressionMatchIterator it = extImgUrlRe.globalMatch(html);
    while (it.hasNext()) {
        const QString urlStr = it.next().captured(1).trimmed();
        if (urlStr.isEmpty() || urlsInThisHtml.contains(urlStr))
            continue;
        urlsInThisHtml.insert(urlStr);

        if (m_loadedExtImgCache.contains(urlStr)) {
            QImage cachedImg = m_loadedExtImgCache.value(urlStr);
            if (cachedImg.width() > maxImgWidth) {
                cachedImg = cachedImg.scaledToWidth(maxImgWidth, Qt::SmoothTransformation);
            }
            browser->document()->addResource(
                QTextDocument::ImageResource, QUrl(urlStr), QVariant(cachedImg));
            appliedCachedToDoc = true;
            continue;
        }

        if (m_loadedExtImgUrls.contains(urlStr))
            continue;
        m_loadedExtImgUrls.insert(urlStr);

        QNetworkRequest req{QUrl(urlStr)};
        req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
        req.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("Mozilla/5.0 SollunaMail/1.0"));
        QNetworkReply *reply = m_extImgNam->get(req);
        QPointer<QTextBrowser> browserPtr(browser);
        connect(reply, &QNetworkReply::finished, this, [this, reply, urlStr, convId, browserPtr]() {
            reply->deleteLater();
            if (reply->error() != QNetworkReply::NoError) {
                m_loadedExtImgUrls.remove(urlStr);
                return;
            }
            const QByteArray data = reply->readAll();
            QImage img;
            if (!img.loadFromData(data) || img.isNull()) {
                m_loadedExtImgUrls.remove(urlStr);
                return;
            }
            m_loadedExtImgCache.insert(urlStr, img);

            const int curMaxW = qBound(420, (640 * m_readingBodyZoomPercent) / 100, 1100);
            QImage scaledImg = (img.width() > curMaxW)
                                   ? img.scaledToWidth(curMaxW, Qt::SmoothTransformation)
                                   : img;

            auto refreshBrowserWithLoadedImages = [this, urlStr, scaledImg, curMaxW](QTextBrowser *tb) {
                if (!tb || !tb->document())
                    return;
                for (auto cacheIt = m_loadedExtImgCache.constBegin(); cacheIt != m_loadedExtImgCache.constEnd(); ++cacheIt) {
                    QImage ci = cacheIt.value();
                    if (ci.width() > curMaxW)
                        ci = ci.scaledToWidth(curMaxW, Qt::SmoothTransformation);
                    tb->document()->addResource(QTextDocument::ImageResource, QUrl(cacheIt.key()), QVariant(ci));
                }
                tb->document()->addResource(QTextDocument::ImageResource, QUrl(urlStr), QVariant(scaledImg));
                const QString savedHtml = tb->property("rawSafeHtml").toString();
                if (!savedHtml.isEmpty()) {
                    const int vScroll = tb->verticalScrollBar() ? tb->verticalScrollBar()->value() : 0;
                    tb->setHtml(savedHtml);
                    if (tb->verticalScrollBar())
                        tb->verticalScrollBar()->setValue(vScroll);
                }
            };

            if (m_selectedConvId == convId && m_readingBodyBrowser) {
                refreshBrowserWithLoadedImages(m_readingBodyBrowser);
            }
            if (browserPtr && browserPtr != m_readingBodyBrowser) {
                refreshBrowserWithLoadedImages(browserPtr.data());
            }
        });
    }

    if (appliedCachedToDoc) {
        const int vScroll = browser->verticalScrollBar() ? browser->verticalScrollBar()->value() : 0;
        browser->setHtml(html);
        if (browser->verticalScrollBar())
            browser->verticalScrollBar()->setValue(vScroll);
    }
}

void MainWindow::applyReadingBodyZoom(int zoomPercent)
{
    m_readingBodyZoomPercent = qBound(75, zoomPercent, 250);
    if (m_readingZoomBadgeBtn) {
        m_readingZoomBadgeBtn->setText(QString("%1%").arg(m_readingBodyZoomPercent));
    }
    const int curRow = m_convListWidget ? m_convListWidget->currentRow() : -1;
    if (curRow >= 0 && curRow < m_filteredConvIndices.size()) {
        displayConversation(curRow, false);
    }
}

void MainWindow::onOpenLargeMailViewerDialog()
{
    if (m_selectedConvIndex < 0 || m_selectedConvIndex >= m_conversations.size())
        return;

    const ConversationItem conv = m_conversations[m_selectedConvIndex];
    if (conv.messages.isEmpty())
        return;
    const MessageItem msg = conv.latestMessage();

    QDialog dlg(this, Qt::Window | Qt::WindowMinimizeButtonHint | Qt::WindowMaximizeButtonHint | Qt::WindowCloseButtonHint);
    dlg.setWindowTitle(MailI18n::t(QString("%1 — SollunaMail 크게 보기").arg(conv.subject),
                                   QString("%1 — SollunaMail Large View").arg(conv.subject),
                                   QString("%1 — SollunaMail 大きく表示").arg(conv.subject)));
    dlg.resize(1160, 860);
    dlg.setMinimumSize(780, 560);
    dlg.setStyleSheet("QDialog { background-color: #FFFFFF; }");

    QVBoxLayout *rootV = new QVBoxLayout(&dlg);
    rootV->setContentsMargins(24, 16, 24, 18);
    rootV->setSpacing(10);

    // 1. 상단 액션 및 확대/축소 툴바
    QWidget *topBar = new QWidget(&dlg);
    topBar->setStyleSheet("background: #F8FAFC; border: 1px solid #E2E8F0; border-radius: 8px;");
    QHBoxLayout *topH = new QHBoxLayout(topBar);
    topH->setContentsMargins(12, 8, 12, 8);
    topH->setSpacing(8);

    QString actBtnStyle =
        "QPushButton { background: #FFFFFF; color: #1E293B; border: 1px solid #CBD5E1; border-radius: 5px; "
        "padding: 5px 12px; font-size: 12px; font-weight: 700; }"
        "QPushButton:hover { background: #EFF6FF; color: #0D47A1; border-color: #93C5FD; }";

    QPushButton *btnReply = new QPushButton(MailI18n::t("↩️ 답장", "↩️ Reply", "↩️ 返信"), topBar);
    btnReply->setCursor(Qt::PointingHandCursor);
    btnReply->setStyleSheet(actBtnStyle);
    connect(btnReply, &QPushButton::clicked, &dlg, [this, &dlg]() {
        dlg.accept();
        onReplyCurrent(false);
    });

    QPushButton *btnReplyAll = new QPushButton(MailI18n::t("↩️ 전체 답장", "↩️ Reply All", "↩️ 全員に返信"), topBar);
    btnReplyAll->setCursor(Qt::PointingHandCursor);
    btnReplyAll->setStyleSheet(actBtnStyle);
    connect(btnReplyAll, &QPushButton::clicked, &dlg, [this, &dlg]() {
        dlg.accept();
        onReplyCurrent(true);
    });

    QPushButton *btnForward = new QPushButton(MailI18n::t("➡️ 전달", "➡️ Forward", "➡️ 転送"), topBar);
    btnForward->setCursor(Qt::PointingHandCursor);
    btnForward->setStyleSheet(actBtnStyle);
    connect(btnForward, &QPushButton::clicked, &dlg, [this, &dlg]() {
        dlg.accept();
        onForwardCurrent();
    });

    QPushButton *btnDelete = new QPushButton(MailI18n::t("🗑️ 삭제", "🗑️ Delete", "🗑️ 削除"), topBar);
    btnDelete->setCursor(Qt::PointingHandCursor);
    btnDelete->setStyleSheet(
        "QPushButton { background: #FEF2F2; color: #DC2626; border: 1px solid #FECACA; border-radius: 5px; "
        "padding: 5px 12px; font-size: 12px; font-weight: 700; }"
        "QPushButton:hover { background: #FEE2E2; }");
    connect(btnDelete, &QPushButton::clicked, &dlg, [this, &dlg]() {
        dlg.accept();
        onDeleteCurrent();
    });

    topH->addWidget(btnReply);
    topH->addWidget(btnReplyAll);
    topH->addWidget(btnForward);
    topH->addWidget(btnDelete);
    topH->addStretch();

    QPushButton *btnZoomOut = new QPushButton("🔍−", topBar);
    btnZoomOut->setCursor(Qt::PointingHandCursor);
    btnZoomOut->setStyleSheet(actBtnStyle);

    QPushButton *btnZoomReset = new QPushButton("125%", topBar);
    btnZoomReset->setCursor(Qt::PointingHandCursor);
    btnZoomReset->setToolTip(MailI18n::t("클릭하면 100%로 초기화합니다", "Reset zoom to 100%", "100%にリセット"));
    btnZoomReset->setStyleSheet(actBtnStyle);

    QPushButton *btnZoomIn = new QPushButton("🔍+", topBar);
    btnZoomIn->setCursor(Qt::PointingHandCursor);
    btnZoomIn->setStyleSheet(actBtnStyle);

    QPushButton *btnMaxRestore = new QPushButton(
        MailI18n::t("🗖 화면 최대화", "🗖 Maximize", "🗖 最大化"), topBar);
    btnMaxRestore->setCursor(Qt::PointingHandCursor);
    btnMaxRestore->setStyleSheet(
        "QPushButton { background: #EFF6FF; color: #0D47A1; border: 1px solid #93C5FD; border-radius: 5px; "
        "padding: 5px 12px; font-size: 12px; font-weight: 700; }"
        "QPushButton:hover { background: #DBEAFE; }");
    connect(btnMaxRestore, &QPushButton::clicked, &dlg, [&dlg, btnMaxRestore]() {
        if (dlg.isMaximized()) {
            dlg.showNormal();
            btnMaxRestore->setText(MailI18n::t("🗖 화면 최대화", "🗖 Maximize", "🗖 最大化"));
        } else {
            dlg.showMaximized();
            btnMaxRestore->setText(MailI18n::t("🗗 원래 크기로", "🗗 Restore Size", "🗗 元のサイズ"));
        }
    });

    QPushButton *btnClose = new QPushButton(MailI18n::t("닫기 (Esc)", "Close (Esc)", "閉じる (Esc)"), topBar);
    btnClose->setCursor(Qt::PointingHandCursor);
    btnClose->setStyleSheet(
        "QPushButton { background: #0D47A1; color: #FFFFFF; border: none; border-radius: 5px; "
        "padding: 5px 14px; font-size: 12px; font-weight: 700; }"
        "QPushButton:hover { background: #1558BC; }");
    connect(btnClose, &QPushButton::clicked, &dlg, &QDialog::accept);

    topH->addWidget(btnZoomOut);
    topH->addWidget(btnZoomReset);
    topH->addWidget(btnZoomIn);
    topH->addSpacing(6);
    topH->addWidget(btnMaxRestore);
    topH->addWidget(btnClose);
    rootV->addWidget(topBar);

    // 2. 메일 제목 및 발신자·수신자·시각 정보
    QLabel *subjLbl = new QLabel(conv.subject, &dlg);
    subjLbl->setWordWrap(true);
    subjLbl->setStyleSheet("font-size: 23px; font-weight: 800; color: #0F172A; padding: 4px 2px;");
    rootV->addWidget(subjLbl);

    QHBoxLayout *metaRow = new QHBoxLayout();
    metaRow->setContentsMargins(2, 0, 2, 4);
    metaRow->setSpacing(10);

    QLabel *avLbl = new QLabel(&dlg);
    avLbl->setFixedSize(32, 32);
    avLbl->setPixmap(MailIcons::senderAvatarPixmap(msg, 32));
    metaRow->addWidget(avLbl);

    QLabel *senderInfoLbl = new QLabel(
        QString("<b>%1</b> &lt;%2&gt; &nbsp;<span style='color:#64748B;'>→ %3</span>")
            .arg(msg.senderName.toHtmlEscaped(),
                 msg.senderEmail.toHtmlEscaped(),
                 msg.recipientSummary.toHtmlEscaped()),
        &dlg);
    senderInfoLbl->setStyleSheet("font-size: 13px; color: #1E293B;");
    metaRow->addWidget(senderInfoLbl, 1);

    QLabel *timeInfoLbl = new QLabel(msg.fullTimeText, &dlg);
    timeInfoLbl->setStyleSheet("font-size: 12px; color: #64748B; font-weight: 600;");
    metaRow->addWidget(timeInfoLbl);
    rootV->addLayout(metaRow);

    QFrame *sep = new QFrame(&dlg);
    sep->setFrameShape(QFrame::HLine);
    sep->setStyleSheet("color: #E2E8F0;");
    rootV->addWidget(sep);

    // 3. 대형 본문 브라우저 (Chromium QWebEngineView 우선 적용 + QTextBrowser 호환)
#ifdef SOLLUNA_HAS_WEBENGINE
    QWebEngineView *largeWebView = new QWebEngineView(&dlg);
    SollunaEmailWebPage *largeWebPage = new SollunaEmailWebPage(
        [this](const QUrl &url) { handleEmailLinkActivated(url); },
        largeWebView);
    SollunaPrivacyInterceptor *largePrivacyInterceptor = new SollunaPrivacyInterceptor(
        [this]() { return m_privacySettings.privacyMode && m_privacySettings.blockPixelTracking; },
        largeWebPage);
    largeWebPage->setUrlRequestInterceptor(largePrivacyInterceptor);
    largeWebView->setPage(largeWebPage);
    if (QWebEngineSettings *ws = largeWebView->settings()) {
        ws->setAttribute(QWebEngineSettings::JavascriptEnabled, false);
        ws->setAttribute(QWebEngineSettings::LocalContentCanAccessRemoteUrls, true);
        ws->setAttribute(QWebEngineSettings::AutoLoadImages, true);
        ws->setAttribute(QWebEngineSettings::ErrorPageEnabled, false);
        ws->setAttribute(QWebEngineSettings::PluginsEnabled, false);
    }
    rootV->addWidget(largeWebView, 1);

    int dlgZoom = qMax(120, m_readingBodyZoomPercent);
    const QString htmlForLarge = m_currentRenderedSafeHtml.isEmpty()
                                     ? prepareEmailHtmlForChromiumViewer(conv.latestMessage().htmlBody,
                                                                         conv.latestMessage().senderName,
                                                                         conv.latestMessage().senderEmail)
                                     : m_currentRenderedSafeHtml;
    largeWebView->setHtml(htmlForLarge, QUrl(QStringLiteral("https://mail.solluna.local/")));
    auto renderLargeBody = [largeWebView, btnZoomReset, &dlgZoom]() {
        dlgZoom = qBound(75, dlgZoom, 280);
        btnZoomReset->setText(QString("%1%").arg(dlgZoom));
        largeWebView->setZoomFactor(dlgZoom / 100.0);
    };
#else
    QTextBrowser *largeBrowser = new QTextBrowser(&dlg);
    largeBrowser->setFrameShape(QFrame::NoFrame);
    largeBrowser->setOpenLinks(false);
    largeBrowser->setStyleSheet("QTextBrowser { background: #FFFFFF; border: none; }");
    connect(largeBrowser, &QTextBrowser::anchorClicked, &dlg, [this](const QUrl &url) {
        handleEmailLinkActivated(url);
    });
    rootV->addWidget(largeBrowser, 1);

    int dlgZoom = qMax(120, m_readingBodyZoomPercent);
    auto renderLargeBody = [this, largeBrowser, btnZoomReset, &dlgZoom, conv]() {
        dlgZoom = qBound(75, dlgZoom, 280);
        btnZoomReset->setText(QString("%1%").arg(dlgZoom));
        const int savedMainZoom = m_readingBodyZoomPercent;
        m_readingBodyZoomPercent = dlgZoom;
        QString html = m_currentRenderedSafeHtml.isEmpty()
                           ? normalizeEmailHtmlForQtViewer(conv.latestMessage().htmlBody,
                                                           conv.latestMessage().senderName,
                                                           conv.latestMessage().senderEmail)
                           : m_currentRenderedSafeHtml;
        // 최상단 컨테이너의 글자 크기를 dlgZoom 배율에 맞게 조정
        static const QRegularExpression topFontPxRe(R"(font-size:\d+px;line-height:1\.65;)");
        const int fontPx = qBound(11, (14 * dlgZoom) / 100, 40);
        html.replace(topFontPxRe, QString("font-size:%1px;line-height:1.65;").arg(fontPx));
        largeBrowser->setHtml(html);
        loadAllowedExternalImagesIntoBrowser(html, conv.id, largeBrowser);
        m_readingBodyZoomPercent = savedMainZoom;
    };
#endif

    connect(btnZoomOut, &QPushButton::clicked, &dlg, [&dlgZoom, renderLargeBody]() {
        dlgZoom -= 15;
        renderLargeBody();
    });
    connect(btnZoomReset, &QPushButton::clicked, &dlg, [&dlgZoom, renderLargeBody]() {
        dlgZoom = 100;
        renderLargeBody();
    });
    connect(btnZoomIn, &QPushButton::clicked, &dlg, [&dlgZoom, renderLargeBody]() {
        dlgZoom += 15;
        renderLargeBody();
    });

    const QString targetConvId = conv.id;
    const QString targetSenderName = msg.senderName;
    const QString targetSenderEmail = msg.senderEmail;
    static const QRegularExpression rxUnresolvedCidAttr(
        R"(\bsrc\s*=\s*["']cid:)",
        QRegularExpression::CaseInsensitiveOption);
    if (!s_fullBodyFetchedConvIds.contains(conv.id)
        && (msg.htmlBody.startsWith(QStringLiteral("<!--SM_HEADER_ONLY-->"))
            || msg.htmlBody.startsWith(QStringLiteral("<!--SM_PARTIAL-->"))
            || rxUnresolvedCidAttr.match(msg.htmlBody).hasMatch())) {
        const int lastUnder = conv.id.lastIndexOf('_');
        const int uidNum = (conv.imapUid > 0)
                               ? conv.imapUid
                               : ((lastUnder > 0) ? conv.id.mid(lastUnder + 1).toInt() : 0);
        if (uidNum > 0 && conv.id.startsWith(QStringLiteral("imap_"))) {
            for (const AccountItem &acc : m_accounts) {
                if (acc.id == conv.accountId) {
                    m_googleProvider->fetchImapSingleMessageBody(
                        acc, conv.folderId, uidNum, conv.id, conv.subject, conv.previousFolderId);
                    break;
                }
            }
        }
    }

    if (m_googleProvider) {
        connect(m_googleProvider, &GoogleProvider::imapSingleMessageBodyFetched, &dlg,
                [this, targetConvId, targetSenderName, targetSenderEmail, renderLargeBody
#ifdef SOLLUNA_HAS_WEBENGINE
                 , largeWebView
#endif
                ](const QString &fetchedConvId, const QString &fullHtmlBody, const QList<AttachmentItem> &) {
            if (fetchedConvId != targetConvId)
                return;
#ifdef SOLLUNA_HAS_WEBENGINE
            if (largeWebView) {
                const QString updatedHtml =
                    (m_selectedConvId == targetConvId && !m_currentRenderedSafeHtml.isEmpty())
                        ? m_currentRenderedSafeHtml
                        : prepareEmailHtmlForChromiumViewer(fullHtmlBody, targetSenderName, targetSenderEmail);
                largeWebView->setHtml(updatedHtml, QUrl(QStringLiteral("https://mail.solluna.local/")));
            }
#else
            Q_UNUSED(fullHtmlBody);
            Q_UNUSED(targetSenderName);
            Q_UNUSED(targetSenderEmail);
#endif
            renderLargeBody();
        });
    }

    renderLargeBody();
    dlg.exec();
}

void MainWindow::displayConversation(int indexInFiltered, bool userClicked)
{
    if (indexInFiltered < 0 || indexInFiltered >= m_filteredConvIndices.size())
        return;

    m_selectedConvIndex = m_filteredConvIndices[indexInFiltered];
    ConversationItem &convRef = m_conversations[m_selectedConvIndex];
    if (userClicked) {
        m_userExplicitlyOpenedConvIds.insert(convRef.id);
    }
    if (userClicked && MailI18n::markReadOnSelect() && convRef.isUnread) {
        convRef.isUnread = false;
        const QString sigKey = conversationSignatureKey(convRef);
        m_readConvSigs.insert(sigKey);
        m_notifiedConvIds.insert(sigKey);

        QSet<QString> sentSeenRequests;
        for (ConversationItem &other : m_conversations) {
            if (other.accountId == convRef.accountId
                && (other.id == convRef.id || conversationSignatureKey(other) == sigKey)) {
                other.isUnread = false;
                QString imapFolder;
                int seqNum = 0;
                if (parseImapConversationId(other.id, imapFolder, seqNum) && seqNum > 0) {
                    const QString effFolder = imapFolder.isEmpty() ? other.folderId : imapFolder;
                    const QString reqKey = effFolder + QStringLiteral("|") + QString::number(seqNum);
                    if (!sentSeenRequests.contains(reqKey)) {
                        sentSeenRequests.insert(reqKey);
                        for (const AccountItem &acc : m_accounts) {
                            if (acc.id == other.accountId) {
                                m_googleProvider->setImapMessageSeenFlag(
                                    acc, effFolder, seqNum, true, other.subject);
                                break;
                            }
                        }
                    }
                }
            }
        }

        if (m_convListWidget && m_convListWidget->viewport()) {
            m_convListWidget->viewport()->update();
        }
        saveConversationsCacheToDisk();
        updateAppUnreadBadgeIcon();
        refreshFolderSelectionStyles();
    }
    const ConversationItem &conv = convRef;
    m_selectedConvId = conv.id;
    const MessageItem &msg = conv.latestMessage();

    if (m_actRestoreBtn) {
        m_actRestoreBtn->setVisible(conv.folderId == "trash" || conv.folderId == "spam"
                                    || m_selectedFolderId == "trash" || m_selectedFolderId == "spam");
    }

    if (m_scheduledMailBanner) {
        if (conv.scheduledSendIso.startsWith(QStringLiteral("EXPIRED:"))) {
            const QString rawIso = conv.scheduledSendIso.mid(8);
            const QDateTime expDt = QDateTime::fromString(rawIso, Qt::ISODate);
            const QString timeDesc = expDt.isValid() ? expDt.toString("yyyy-MM-dd AP h:mm") : rawIso;
            m_scheduledMailBannerLbl->setText(
                MailI18n::t(QString("⚠️ 예약 시간 경과(미발송) — 기존 시각: %1").arg(timeDesc),
                            QString("⚠️ Missed schedule — Original: %1").arg(timeDesc),
                            QString("⚠️ 予約時刻経過(未送信) — 元時刻: %1").arg(timeDesc)));
            m_scheduledMailBannerLbl->setToolTip(m_scheduledMailBannerLbl->text());
            m_scheduledRescheduleBtn->setText(MailI18n::t("🕒 예약 시간 다시 설정", "🕒 Reschedule", "🕒 予約時刻を再設定"));
            m_scheduledEditOrCancelBtn->setText(MailI18n::t("✏️ 내용 수정 / 예약 취소", "✏️ Edit / Cancel Schedule", "✏️ 内容編集 / 予約取消"));
            m_scheduledMailBanner->setVisible(true);
        } else if (!conv.scheduledSendIso.isEmpty()) {
            QDateTime schedDt = QDateTime::fromString(conv.scheduledSendIso, Qt::ISODate);
            QString timeDesc = schedDt.isValid() ? schedDt.toString("yyyy-MM-dd AP h:mm") : conv.scheduledSendIso;
            m_scheduledMailBannerLbl->setText(
                MailI18n::t(QString("⏰ 예약 발송 — 예정 시각: %1").arg(timeDesc),
                            QString("⏰ Scheduled to send at: %1").arg(timeDesc),
                            QString("⏰ 予約送信 — 予定時刻: %1").arg(timeDesc)));
            m_scheduledMailBannerLbl->setToolTip(m_scheduledMailBannerLbl->text());
            m_scheduledRescheduleBtn->setText(MailI18n::t("🕒 예약 시간 변경", "🕒 Reschedule", "🕒 予約時刻変更"));
            m_scheduledEditOrCancelBtn->setText(MailI18n::t("✏️ 내용 수정 / 예약 취소", "✏️ Edit / Cancel Schedule", "✏️ 内容編集 / 予約取消"));
            m_scheduledMailBanner->setVisible(true);
        } else if (conv.folderId == "drafts") {
            m_scheduledMailBannerLbl->setText(
                MailI18n::t("📝 임시보관함에 저장된 작성 중인 메일입니다.",
                            "📝 Saved draft message.",
                            "📝 下書き保存されたメールです。"));
            m_scheduledRescheduleBtn->setText(MailI18n::t("⏰ 예약 발송 설정", "⏰ Schedule Send", "⏰ 予約送信設定"));
            m_scheduledEditOrCancelBtn->setText(MailI18n::t("✏️ 이어서 작성하기", "✏️ Continue Editing", "✏️ 編集を続ける"));
            m_scheduledMailBanner->setVisible(true);
        } else {
            m_scheduledMailBanner->setVisible(false);
        }
    }

    QString subjTitlePrefix;
    if (conv.isImportant)
        subjTitlePrefix += QStringLiteral("❗ ");
    if (conv.isStarred)
        subjTitlePrefix += QStringLiteral("⭐ ");
    m_readingSubjectLbl->setText(subjTitlePrefix + conv.subject);
    if (m_actStarBtn) {
        m_actStarBtn->setIcon(MailIcons::folderIcon(
            FolderKind::Starred,
            conv.isStarred ? QColor("#F59E0B") : QColor("#6B7280"),
            17));
        m_actStarBtn->setStyleSheet(
            conv.isStarred
                ? "QToolButton { border: 1px solid #FCD34D; background: #FFFBEB; border-radius: 4px; }"
                  "QToolButton:hover { background-color: #FEF3C7; }"
                : "QToolButton { border: none; background: transparent; border-radius: 4px; }"
                  "QToolButton:hover { background-color: #F3F4F6; }");
    }
    QString chipText = MailI18n::folderTitle(conv.folderId, conv.folderTag);
    if ((conv.folderId == "trash" || conv.folderId == "spam") && !conv.previousFolderId.isEmpty()) {
        chipText = MailI18n::t(QString("%1 (원래 폴더: %2)").arg(chipText, MailI18n::folderTitle(conv.previousFolderId)),
                               QString("%1 (Original: %2)").arg(chipText, MailI18n::folderTitle(conv.previousFolderId)),
                               QString("%1 (元のフォルダ: %2)").arg(chipText, MailI18n::folderTitle(conv.previousFolderId)));
    }
    if (conv.isImportant) {
        chipText += MailI18n::t(" · ❗ 중요 메일", " · ❗ High Priority", " · ❗ 重要メール");
    }
    if (conv.isStarred) {
        chipText += MailI18n::t(" · ⭐ 별표", " · ⭐ Starred", " · ⭐ スター");
    }
    if (conv.isReplied) {
        chipText += MailI18n::t(" · ↩️ 답장함", " · ↩️ Replied", " · ↩️ 返信済");
    }
    if (conv.isForwarded) {
        chipText += MailI18n::t(" · ➡️ 전달함", " · ➡️ Forwarded", " · ➡️ 転送済");
    }
    if (conv.folderId == "sent") {
        if (!conv.readReceiptConfirmed && reconcileSentReadReceipts(m_conversations)) {
            saveConversationsCacheToDisk();
        }
        if (conv.readReceiptConfirmed) {
            chipText += MailI18n::t(" · ✅ 수신확인됨", " · ✅ Read Confirmed", " · ✅ 開封確認済");
        } else if (conv.readReceiptRequested) {
            chipText += MailI18n::t(" · 📨 수신확인 요청됨", " · 📨 Receipt Requested", " · 📨 開封確認要求中");
        }
    }
    m_readingAvatarLbl->setPixmap(MailIcons::senderAvatarPixmap(msg, 28));
    QString recipLine = QString("<b>%1</b> <span style='color:#6B7280;'>to %2").arg(msg.senderName.toHtmlEscaped(), msg.recipientSummary.toHtmlEscaped());
    if (!conv.ccSummary.trimmed().isEmpty()) {
        recipLine += QString(" · Cc: %1").arg(conv.ccSummary.toHtmlEscaped());
    }
    if (!conv.bccSummary.trimmed().isEmpty()) {
        recipLine += QString(" · Bcc: %1").arg(conv.bccSummary.toHtmlEscaped());
    }
    recipLine += " ⌄</span>";
    m_readingSenderLbl->setText(recipLine);
    m_readingTimeLbl->setText(msg.fullTimeText);

    // 보낸편지함(sent) 및 임시보관함(drafts)에서는 [구독 취소], [발신자 차단] 버튼을 숨기고,
    // 받은 메일에서도 본문에 실제 수신거부/구독취소 링크가 존재하는 메일일 때만 [구독 취소] 버튼을 표시
    const bool isOutgoingOrDraft = (conv.folderId == QStringLiteral("sent")
                                    || conv.folderId == QStringLiteral("drafts")
                                    || msg.senderName.startsWith(QStringLiteral("To:"), Qt::CaseInsensitive)
                                    || msg.senderName.startsWith(QStringLiteral("⏰ To:"), Qt::CaseInsensitive));
    if (m_blockSenderBtn) {
        m_blockSenderBtn->setVisible(!isOutgoingOrDraft);
    }
    if (m_unsubscribeBtn) {
        const QString unsubTarget = isOutgoingOrDraft ? QString() : extractUnsubscribeTargetFromHtml(msg.htmlBody);
        m_unsubscribeBtn->setVisible(!unsubTarget.isEmpty());
    }

    const bool isHeaderOnly = msg.htmlBody.startsWith(QStringLiteral("<!--SM_HEADER_ONLY-->"));

    // 개인정보 보호 허브 ①: 발신자 픽셀 추적 차단 (뷰어 정규화 전에 먼저 원본 HTML에서 숨김 <table/div style="display:none"> 내부 픽셀 및 모든 웹메일 수신확인/트래킹 픽셀 원천 제거)
    int strippedTrackPixels = 0;
    QString sourceHtmlForView = isHeaderOnly
        ? MailI18n::t(
              QStringLiteral("<div style='padding:24px 16px;color:#64748B;font-size:13px;'>⏳ 서버에서 메일 본문을 불러오는 중입니다...</div>"),
              QStringLiteral("<div style='padding:24px 16px;color:#64748B;font-size:13px;'>⏳ Loading message body from server...</div>"),
              QStringLiteral("<div style='padding:24px 16px;color:#64748B;font-size:13px;'>⏳ サーバーからメール本文を読み込んでいます...</div>"))
        : msg.htmlBody;
    if (!isHeaderOnly && m_privacySettings.privacyMode && m_privacySettings.blockPixelTracking) {
        sourceHtmlForView = stripTrackingPixelsFromHtml(sourceHtmlForView, &strippedTrackPixels);
        if (strippedTrackPixels > 0) {
            chipText += MailI18n::t(QString(" · 🛡️ 추적 픽셀 %1개 차단됨").arg(strippedTrackPixels),
                                    QString(" · 🛡️ %1 Tracking Pixel(s) Blocked").arg(strippedTrackPixels),
                                    QString(" · 🛡️ 追跡ピクセル%1個ブロック").arg(strippedTrackPixels));
        }
    }

#ifdef SOLLUNA_HAS_WEBENGINE
    QString safeHtml = m_readingWebView
                           ? prepareEmailHtmlForChromiumViewer(sourceHtmlForView, msg.senderName, msg.senderEmail)
                           : normalizeEmailHtmlForQtViewer(sourceHtmlForView, msg.senderName, msg.senderEmail);
#else
    QString safeHtml = normalizeEmailHtmlForQtViewer(sourceHtmlForView, msg.senderName, msg.senderEmail);
#endif

    // 개인정보 보호 허브 ②: 모든 외부 이미지 자동 로드 차단 (사용자가 [외부 이미지 표시]를 누르기 전까지 외부 http/https 이미지 차단)
    if (m_privacySettings.blockAllImages && !m_allowedExternalImageConvIds.contains(conv.id)) {
        static const QRegularExpression extImgRe(
            R"(<img\b[^>]*\bsrc\s*=\s*["']?\s*https?://[^>]*>)",
            QRegularExpression::CaseInsensitiveOption);
        static const QRegularExpression altExtractRe(
            R"(\balt\s*=\s*["']([^"']*)["'])",
            QRegularExpression::CaseInsensitiveOption);
        QString replacedHtml;
        int lastPos = 0;
        QRegularExpressionMatchIterator it = extImgRe.globalMatch(safeHtml);
        while (it.hasNext()) {
            const QRegularExpressionMatch mImg = it.next();
            replacedHtml.append(safeHtml.mid(lastPos, mImg.capturedStart() - lastPos));
            const QString tagStr = mImg.captured(0);
            const QRegularExpressionMatch altM = altExtractRe.match(tagStr);
            const QString altText = altM.hasMatch() ? altM.captured(1).trimmed() : QString();
            const QString blockedImgBadge = MailI18n::t("🖼️ [외부 이미지 차단됨]",
                                                        "🖼️ [External Image Blocked]",
                                                        "🖼️ [外部画像ブロック済]");
            if (!altText.isEmpty()) {
                replacedHtml.append(QStringLiteral(
                    "<div style='margin:6px 0 12px 0;'><span style='font-size:22px;font-weight:800;color:#0F172A;'>%1</span> "
                    "<span style='display:inline-block;padding:2px 7px;margin-left:6px;background:#F1F5F9;border:1px dashed #94A3B8;border-radius:4px;color:#475569;font-size:11px;'>%2</span></div>")
                    .arg(altText.toHtmlEscaped(), blockedImgBadge));
            } else {
                replacedHtml.append(QStringLiteral(
                    "<span style='display:inline-block;padding:3px 8px;margin:2px;background:#F1F5F9;border:1px dashed #94A3B8;border-radius:4px;color:#475569;font-size:11px;'>%1</span>")
                    .arg(blockedImgBadge));
            }
            lastPos = mImg.capturedEnd();
        }
        replacedHtml.append(safeHtml.mid(lastPos));
        safeHtml = replacedHtml;
    }

    m_readingFolderChipLbl->setText(chipText);
    m_currentRenderedSafeHtml = safeHtml;
    setReadingBodyHtml(safeHtml, conv.id, true);

    refreshReadingAttachmentsBar(conv);

    // 2단계 표준 IMAP 본문 온디맨드 로드:
    // 목록 조회 시 헤더만 가져온 메일(<!--SM_HEADER_ONLY-->)이거나, 첨부파일이 아직 로컬에 없는 경우
    // 사용자가 메일 제목을 직접 클릭한 메일(userClicked || m_userExplicitlyOpenedConvIds)에 한해서만
    // 서버에서 해당 1건의 100% 전체 원문(BODY.PEEK[])과 첨부파일을 다운로드한다. (체크박스 클릭 시에는 절대 본문 미로드!)
    const bool allowBodyLoad = (userClicked || m_userExplicitlyOpenedConvIds.contains(conv.id))
                               && !s_fullBodyFetchedConvIds.contains(conv.id);
    static const QRegularExpression rxUnresolvedCid(
        R"(\bsrc\s*=\s*["']cid:)",
        QRegularExpression::CaseInsensitiveOption);
    bool needFullFetch = allowBodyLoad
                         && (isHeaderOnly
                             || msg.htmlBody.startsWith(QStringLiteral("<!--SM_PARTIAL-->"))
                             || rxUnresolvedCid.match(msg.htmlBody).hasMatch());
    if (allowBodyLoad && !needFullFetch && !msg.attachments.isEmpty()) {
        for (const AttachmentItem &att : msg.attachments) {
            if (att.filePath.isEmpty() || !QFileInfo::exists(att.filePath)) {
                needFullFetch = true;
                break;
            }
        }
    }
    if (needFullFetch && conv.id.startsWith(QStringLiteral("imap_"))) {
        int lastUnderscore = conv.id.lastIndexOf('_');
        int seqNum = (lastUnderscore > 0) ? conv.id.mid(lastUnderscore + 1).toInt() : 0;
        if (seqNum > 0) {
            for (const AccountItem &acc : m_accounts) {
                if (acc.id == conv.accountId) {
                    m_googleProvider->fetchImapSingleMessageBody(
                        acc, conv.folderId, seqNum, conv.id, conv.subject, conv.previousFolderId);
                    break;
                }
            }
        }
    }
}

void MainWindow::refreshReadingAttachmentsBar(const ConversationItem &conv)
{
    if (!m_readingAttachmentsBar || !m_readingAttachmentsLayout)
        return;

    while (QLayoutItem *child = m_readingAttachmentsLayout->takeAt(0)) {
        if (QWidget *w = child->widget()) {
            w->hide();
            w->setParent(nullptr);
            w->deleteLater();
        }
        delete child;
    }

    if (conv.messages.isEmpty()) {
        m_readingAttachmentsBar->setVisible(false);
        return;
    }

    static const QRegularExpression extImgCheckRe(
        R"(<img\b[^>]*\bsrc\s*=\s*["']?\s*https?://)",
        QRegularExpression::CaseInsensitiveOption);
    const bool hasBlockedExternalImages =
        m_privacySettings.blockAllImages
        && !m_allowedExternalImageConvIds.contains(conv.id)
        && extImgCheckRe.match(conv.latestMessage().htmlBody).hasMatch();

    const QList<AttachmentItem> &atts = conv.latestMessage().attachments;
    if (atts.isEmpty() && !hasBlockedExternalImages) {
        m_readingAttachmentsBar->setVisible(false);
        return;
    }

    if (hasBlockedExternalImages) {
        QPushButton *showImgBtn = new QPushButton(
            MailI18n::t("🖼️ 외부 이미지가 차단되었습니다 — [이 메일의 외부 이미지 표시]",
                        "🖼️ External images blocked — [Show Images for This Email]",
                        "🖼️ 外部画像がブロックされています — [このメールの画像を表示]"),
            m_readingAttachmentsBar);
        showImgBtn->setCursor(Qt::PointingHandCursor);
        showImgBtn->setStyleSheet(
            "QPushButton { background-color: #FEF3C7; color: #92400E; font-size: 11.5px; font-weight: 700; border: 1px solid #F59E0B; border-radius: 5px; padding: 4px 10px; }"
            "QPushButton:hover { background-color: #FDE68A; }");
        const QString convIdForImg = conv.id;
        connect(showImgBtn, &QPushButton::clicked, this, [this, convIdForImg]() {
            m_allowedExternalImageConvIds.insert(convIdForImg);
            const int curRow = m_convListWidget ? m_convListWidget->currentRow() : 0;
            if (curRow >= 0 && curRow < m_filteredConvIndices.size()) {
                displayConversation(curRow, false);
            }
        });
        m_readingAttachmentsLayout->addWidget(showImgBtn);
    }

    if (!atts.isEmpty()) {
        QLabel *hdrLbl = new QLabel(
            MailI18n::t(QString("📎 첨부파일 %1개:").arg(atts.size()),
                        QString("📎 %1 Attachment(s):").arg(atts.size()),
                        QString("📎 添付ファイル %1個:").arg(atts.size())),
            m_readingAttachmentsBar);
        hdrLbl->setStyleSheet("color: #0D47A1; font-size: 12px; font-weight: 700; background: transparent; border: none;");
        m_readingAttachmentsLayout->addWidget(hdrLbl);
    }

    for (const AttachmentItem &att : atts) {
        QWidget *chip = new QWidget(m_readingAttachmentsBar);
        chip->setObjectName("attChip");
        chip->setStyleSheet(
            "QWidget#attChip { background-color: #FFFFFF; border: 1px solid #93C5FD; border-radius: 6px; }");
        QHBoxLayout *chipH = new QHBoxLayout(chip);
        chipH->setContentsMargins(9, 4, 7, 4);
        chipH->setSpacing(6);

        const QString sizeTxt = att.fileSize.trimmed().isEmpty() ? QStringLiteral("") : QString(" (%1)").arg(att.fileSize);
        QLabel *nameLbl = new QLabel(QString("📄 %1%2").arg(att.fileName, sizeTxt), chip);
        nameLbl->setStyleSheet("color: #1E293B; font-size: 12px; font-weight: 600; background: transparent; border: none;");
        chipH->addWidget(nameLbl);

        QPushButton *saveBtn = new QPushButton(MailI18n::t("💾 저장", "💾 Save", "💾 保存"), chip);
        saveBtn->setCursor(Qt::PointingHandCursor);
        saveBtn->setToolTip(MailI18n::t("첨부파일을 다른 이름으로 PC에 저장합니다",
                                        "Save attachment to your computer",
                                        "添付ファイルをPCに保存します"));
        saveBtn->setStyleSheet(
            "QPushButton { background-color: #0D47A1; color: #FFFFFF; font-size: 11px; font-weight: 700; border: none; border-radius: 4px; padding: 3px 8px; }"
            "QPushButton:hover { background-color: #1558BC; }");

        const QString convIdCopy = conv.id;
        const QString accIdCopy = conv.accountId;
        const QString folderIdCopy = conv.folderId;
        const QString subjCopy = conv.subject;
        const QString prevFolderCopy = conv.previousFolderId;
        const AttachmentItem attCopy = att;

        connect(saveBtn, &QPushButton::clicked, this, [this, convIdCopy, accIdCopy, folderIdCopy, subjCopy, prevFolderCopy, attCopy]() {
            QString srcPath = attCopy.filePath;
            if (srcPath.isEmpty() || !QFileInfo::exists(srcPath)) {
                if (convIdCopy.startsWith(QStringLiteral("imap_"))) {
                    int lastUnder = convIdCopy.lastIndexOf('_');
                    int seqNum = (lastUnder > 0) ? convIdCopy.mid(lastUnder + 1).toInt() : 0;
                    if (seqNum > 0) {
                        for (const AccountItem &acc : m_accounts) {
                            if (acc.id == accIdCopy) {
                                m_googleProvider->fetchImapSingleMessageBody(
                                    acc, folderIdCopy, seqNum, convIdCopy, subjCopy, prevFolderCopy);
                                break;
                            }
                        }
                    }
                }
                QMessageBox::information(
                    this,
                    MailI18n::t("첨부파일 다운로드 중", "Downloading Attachment", "添付ファイル取得中"),
                    MailI18n::t("서버에서 첨부파일 원본 데이터를 불러오고 있습니다.\n1~2초 후 다시 [💾 저장] 버튼을 눌러주세요.",
                                "Fetching full attachment data from IMAP server. Please click Save again in a moment.",
                                "サーバーから添付ファイルを取得中です。少し待ってから再度押してください。"));
                return;
            }

            QString dlDir = QStandardPaths::writableLocation(QStandardPaths::DownloadLocation);
            if (dlDir.isEmpty())
                dlDir = QDir::homePath();
            QString defaultTarget = QDir(dlDir).filePath(attCopy.fileName.isEmpty() ? QStringLiteral("attachment.bin") : attCopy.fileName);
            QString savePath = QFileDialog::getSaveFileName(
                this,
                MailI18n::t("첨부파일 저장", "Save Attachment", "添付ファイルを保存"),
                defaultTarget);
            if (savePath.isEmpty())
                return;

            if (QFile::exists(savePath)) {
                QFile::remove(savePath);
            }
            if (QFile::copy(srcPath, savePath)) {
                setWindowTitle(MailI18n::t(
                    QString("SollunaMail — 첨부파일 저장 완료: %1").arg(QFileInfo(savePath).fileName()),
                    QString("SollunaMail — Saved attachment: %1").arg(QFileInfo(savePath).fileName()),
                    QString("SollunaMail — 添付ファイルを保存しました: %1").arg(QFileInfo(savePath).fileName())));
            } else {
                QMessageBox::warning(this,
                                     MailI18n::t("저장 실패", "Save Failed", "保存失敗"),
                                     MailI18n::t("파일을 선택한 경로에 저장하지 못했습니다.",
                                                 "Failed to save attachment to the selected path.",
                                                 "ファイルの保存に失敗しました。"));
            }
        });
        chipH->addWidget(saveBtn);

        QPushButton *openBtn = new QPushButton(MailI18n::t("📂 열기", "📂 Open", "📂 開く"), chip);
        openBtn->setCursor(Qt::PointingHandCursor);
        openBtn->setToolTip(MailI18n::t("기본 연결 프로그램으로 첨부파일을 즉시 엽니다",
                                        "Open attachment with default application",
                                        "既定のアプリで添付ファイルを開きます"));
        openBtn->setStyleSheet(
            "QPushButton { background-color: #EFF6FF; color: #0D47A1; font-size: 11px; font-weight: 700; border: 1px solid #93C5FD; border-radius: 4px; padding: 3px 8px; }"
            "QPushButton:hover { background-color: #DBEAFE; }");
        connect(openBtn, &QPushButton::clicked, this, [this, convIdCopy, accIdCopy, folderIdCopy, subjCopy, prevFolderCopy, attCopy]() {
            if (attCopy.filePath.isEmpty() || !QFileInfo::exists(attCopy.filePath)) {
                if (convIdCopy.startsWith(QStringLiteral("imap_"))) {
                    int lastUnder = convIdCopy.lastIndexOf('_');
                    int seqNum = (lastUnder > 0) ? convIdCopy.mid(lastUnder + 1).toInt() : 0;
                    if (seqNum > 0) {
                        for (const AccountItem &acc : m_accounts) {
                            if (acc.id == accIdCopy) {
                                m_googleProvider->fetchImapSingleMessageBody(
                                    acc, folderIdCopy, seqNum, convIdCopy, subjCopy, prevFolderCopy);
                                break;
                            }
                        }
                    }
                }
                QMessageBox::information(
                    this,
                    MailI18n::t("첨부파일 다운로드 중", "Downloading Attachment", "添付ファイル取得中"),
                    MailI18n::t("서버에서 첨부파일 원본 데이터를 불러오고 있습니다.\n1~2초 후 다시 [📂 열기] 버튼을 눌러주세요.",
                                "Fetching full attachment data from IMAP server. Please click Open again in a moment.",
                                "サーバーから添付ファイルを取得中です。少し待ってから再度押してください。"));
                return;
            }
            QDesktopServices::openUrl(QUrl::fromLocalFile(attCopy.filePath));
        });
        chipH->addWidget(openBtn);

        m_readingAttachmentsLayout->addWidget(chip);
    }

    m_readingAttachmentsLayout->addStretch(1);
    m_readingAttachmentsBar->setVisible(true);
}

void MainWindow::onSelectAccount(const QString &accountId)
{
    m_selectedAccountId = accountId;
    m_selectedConvId.clear();
    m_checkedConvIds.clear();
    m_currentMailPage = 1;

    bool targetIsPop3 = false;
    for (const AccountItem &acc : m_accounts) {
        if (acc.id == accountId && acc.provider != AccountProviderType::Unified) {
            targetIsPop3 = acc.isPop3();
            break;
        }
    }

    // 다른 계정의 내 폴더(custom_...)를 보고 있던 상태에서 계정을 전환하거나, POP3 계정인데 IMAP 전용 폴더를 보고 있었으면 기본 받은편지함(inbox)으로 복귀
    const bool wasImapOnlyFolder = (m_selectedFolderId == QStringLiteral("archive")
                                    || m_selectedFolderId == QStringLiteral("snoozed"));
    if (m_selectedFolderId.startsWith("custom_") || (targetIsPop3 && wasImapOnlyFolder)) {
        m_selectedFolderId = "inbox";
        m_selectedFolderTitle = "Inbox";
        if (m_convPaneTitleLbl)
            m_convPaneTitleLbl->setText(MailI18n::folderTitle("inbox", "Inbox"));
    }

    m_centerStack->setCurrentIndex(0);
    rebuildCustomFoldersList();
    refreshFolderSelectionStyles();

    // 선택한 계정의 현재 선택된 편지함(기본: 받은편지함) 메일 목록이 아직 로드되지 않았으면 해당 편지함과 활성 필터 대상 폴더를 즉시 동기화
    const QString targetFolder = (m_selectedFolderId == QStringLiteral("snoozed")
                                  || m_selectedFolderId.isEmpty()
                                  || (targetIsPop3 && (m_selectedFolderId == QStringLiteral("starred")
                                                       || m_selectedFolderId == QStringLiteral("important"))))
                                     ? QStringLiteral("inbox")
                                     : m_selectedFolderId;
    QList<QPair<AccountItem, QString>> toFetchFolderRequests;
    for (const AccountItem &acc : m_accounts) {
        if (acc.provider == AccountProviderType::Unified)
            continue;
        if (accountId != QStringLiteral("acc_unified") && acc.id != accountId)
            continue;
        if (acc.appPassword.isEmpty() && acc.oauthAccessToken.isEmpty())
            continue;

        bool hasFolderMail = false;
        for (const ConversationItem &c : m_conversations) {
            if (c.accountId == acc.id && c.folderId == targetFolder && c.id.startsWith(QStringLiteral("imap_"))) {
                hasFolderMail = true;
                break;
            }
        }
        const QString folderKey = acc.id + QStringLiteral("|") + targetFolder;
        if (!hasFolderMail && !m_folderLoadingMore.contains(folderKey)) {
            m_folderLoadingMore.insert(folderKey);
            toFetchFolderRequests.append({acc, targetFolder});
        }
        if (!acc.isPop3()) {
            for (const MailFilterRule &r : MailI18n::filterRules()) {
                if (!r.enabled || !r.targetFolderId.startsWith(QStringLiteral("custom_")))
                    continue;
                if (r.targetFolderId.startsWith(QStringLiteral("custom_acc_"))
                    && !r.targetFolderId.startsWith(QStringLiteral("custom_") + acc.id + QStringLiteral("_"))) {
                    continue;
                }
                const QString cfKey = acc.id + QStringLiteral("|") + r.targetFolderId;
                if (!m_folderTotalExists.contains(cfKey) && !m_folderLoadingMore.contains(cfKey)) {
                    m_folderLoadingMore.insert(cfKey);
                    toFetchFolderRequests.append({acc, r.targetFolderId});
                }
            }
        }
    }

    refreshConversationList();

    if (!toFetchFolderRequests.isEmpty()) {
        showMailLoadingToast(QString(), 2000);
        for (const auto &req : toFetchFolderRequests) {
            m_googleProvider->fetchImapFolder(req.first, req.second, 20, 0);
        }
    }
}

void MainWindow::onSelectFolder(const QString &folderId, const QString &folderDisplayName)
{
    m_selectedFolderId = folderId;
    m_selectedFolderTitle = folderDisplayName;
    m_selectedConvId.clear();
    m_checkedConvIds.clear();
    m_currentMailPage = 1;
    m_convPaneTitleLbl->setText(MailI18n::folderTitle(folderId, folderDisplayName));
    if (m_emptyTrashBanner && m_emptyTrashBannerBtn) {
        if (folderId == "trash") {
            if (m_restoreSelectedTrashBtn)
                m_restoreSelectedTrashBtn->setText(MailI18n::t("♻️ 선택 복원", "♻️ Restore", "♻️ 選択復元"));
            if (m_restoreAllTrashBtn)
                m_restoreAllTrashBtn->setText(MailI18n::t("♻️ 전체 복원", "♻️ Restore All", "♻️ すべて復元"));
            m_emptyTrashBannerBtn->setText(MailI18n::t("🗑️ 휴지통 비우기", "🗑️ Empty Trash", "🗑️ ゴミ箱を空にする"));
            m_emptyTrashBanner->setVisible(true);
        } else if (folderId == "spam") {
            if (m_restoreSelectedTrashBtn)
                m_restoreSelectedTrashBtn->setText(MailI18n::t("♻️ 선택 복원", "♻️ Restore", "♻️ 選択復元"));
            if (m_restoreAllTrashBtn)
                m_restoreAllTrashBtn->setText(MailI18n::t("♻️ 전체 복원", "♻️ Restore All", "♻️ すべて復元"));
            m_emptyTrashBannerBtn->setText(MailI18n::t("🗑️ 스팸함 비우기", "🗑️ Empty Spam", "🗑️ 迷惑メールを空にする"));
            m_emptyTrashBanner->setVisible(true);
        } else {
            m_emptyTrashBanner->setVisible(false);
        }
    }
    if (m_actRestoreBtn) {
        m_actRestoreBtn->setVisible(folderId == "trash" || folderId == "spam");
    }
    m_centerStack->setCurrentIndex(0);
    refreshFolderSelectionStyles();

    // 선택한 폴더를 클릭할 때마다 서버에서 최신 메일 목록(Server-Truth)을 즉시 조회 (1회 20건 제목+본문 조회 + 2초 토스트 안내)
    QList<QPair<AccountItem, QString>> toFetchFolderRequests;
    if (folderId != "snoozed") {
        for (const AccountItem &acc : m_accounts) {
            if (acc.provider == AccountProviderType::Unified)
                continue;
            if (m_selectedAccountId != "acc_unified" && acc.id != m_selectedAccountId)
                continue;
            if (acc.appPassword.isEmpty() && acc.oauthAccessToken.isEmpty())
                continue;
            if (acc.isPop3() && (folderId == QStringLiteral("starred") || folderId == QStringLiteral("important"))) {
                // POP3 계정에서 별표편지함(starred) / 중요편지함(important) 선택 시 아직 받은편지함(inbox)이 로드되지 않았으면 inbox 로드
                const QString inbKey = acc.id + QStringLiteral("|inbox");
                if (!m_folderTotalExists.contains(inbKey) && !m_folderLoadingMore.contains(inbKey)) {
                    m_folderLoadingMore.insert(inbKey);
                    toFetchFolderRequests.append({acc, QStringLiteral("inbox")});
                }
                continue;
            }

            const QString folderKey = acc.id + "|" + folderId;
            if (!m_folderLoadingMore.contains(folderKey)) {
                m_folderLoadingMore.insert(folderKey);
                toFetchFolderRequests.append({acc, folderId});
            }

            // 별표편지함(starred) 또는 중요편지함(important) 클릭 시:
            // 일반 IMAP 서버(중요/별표 전용 서버 폴더 없음) 및 필터 설정으로 이동된 사용자 정의 폴더의
            // 중요(❗)·별표(⭐) 메일이 100% 실시간 연동되도록 받은편지함(inbox)과 내 폴더(custom_*)도 함께 1페이지 조회
            if (folderId == QStringLiteral("starred") || folderId == QStringLiteral("important")) {
                const QString inbKey = acc.id + QStringLiteral("|inbox");
                if (!m_folderLoadingMore.contains(inbKey)) {
                    m_folderLoadingMore.insert(inbKey);
                    toFetchFolderRequests.append({acc, QStringLiteral("inbox")});
                }
                for (const FolderItem &cf : m_folders) {
                    if (!cf.isCustom || cf.accountId != acc.id)
                        continue;
                    const QString cfKey = acc.id + QStringLiteral("|") + cf.id;
                    if (!m_folderLoadingMore.contains(cfKey)) {
                        m_folderLoadingMore.insert(cfKey);
                        toFetchFolderRequests.append({acc, cf.id});
                    }
                }
            }
        }
    }

    refreshConversationList();

    if (!toFetchFolderRequests.isEmpty()) {
        for (const auto &req : toFetchFolderRequests) {
            m_googleProvider->fetchImapFolder(req.first, req.second, 20, 0);
        }
        showMailLoadingToast(QString(), 2000);
    }
}

void MainWindow::onConversationRowSelected(int row)
{
    if (m_convListWidget && m_convListWidget->viewport()) {
        m_convListWidget->viewport()->update();
    }
    if (row >= 0 && row < m_filteredConvIndices.size()) {
        displayConversation(row, true);
    }
}


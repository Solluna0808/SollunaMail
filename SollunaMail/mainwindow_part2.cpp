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

extern QSet<QString> s_fullBodyFetchedConvIds;

static bool parseImapConversationId(const QString &cid, QString &outFolderId, int &outSeqNum);
static QString ensureAppCacheDir();

static QString stripTrackingPixelsFromHtml(const QString &inputHtml, int *outStrippedCount = nullptr)
{
    int stripped = 0;
    QString html = inputHtml;

    static const QRegularExpression imgTagRe(
        R"(<img\b[^>]*>)",
        QRegularExpression::CaseInsensitiveOption);

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

void MainWindow::updateUndoSendBannerText()
{
    if (!m_undoSendBannerLbl || !m_pendingSend.active)
        return;
    m_undoSendBannerLbl->setText(
        MailI18n::t(QString("⏳ 메일 발송 대기 중... (%1초 후 '%2'(으)로 발송됩니다)")
                        .arg(m_pendingSend.remainingSec)
                        .arg(m_pendingSend.toEmail),
                    QString("⏳ Sending in %1s to '%2'...")
                        .arg(m_pendingSend.remainingSec)
                        .arg(m_pendingSend.toEmail),
                    QString("⏳ メール送信待機中... (%1秒後に '%2' へ送信されます)")
                        .arg(m_pendingSend.remainingSec)
                        .arg(m_pendingSend.toEmail)));
    statusBar()->showMessage(
        QString("⏳ 발송 취소 가능 시간: %1초 남음 (우측 상단 [↩️ 발송 취소] 버튼 클릭 시 취소)")
            .arg(m_pendingSend.remainingSec),
        1500);
}

void MainWindow::commitPendingSend()
{
    if (m_undoSendTimer)
        m_undoSendTimer->stop();
    if (m_undoSendBanner)
        m_undoSendBanner->setVisible(false);
    if (!m_pendingSend.active)
        return;

    PendingSendItem item = m_pendingSend;
    m_pendingSend.active = false;

    m_googleProvider->sendSmtpEmail(item.account,
                                    item.toEmail,
                                    item.subject,
                                    item.bodyHtml,
                                    item.ccEmail,
                                    item.bccEmail,
                                    item.attachments,
                                    item.isHighImportance);
}

void MainWindow::cancelPendingSendAndReopen()
{
    if (m_undoSendTimer)
        m_undoSendTimer->stop();
    if (m_undoSendBanner)
        m_undoSendBanner->setVisible(false);
    if (!m_pendingSend.active)
        return;

    PendingSendItem item = m_pendingSend;
    m_pendingSend.active = false;

    // 1. 보낸편지함(Sent)에는 절대 저장되지 않도록 즉시 임시보관함(Drafts)으로 전환하여 안전하게 보관
    bool convertedToDraft = false;
    for (int i = 0; i < m_conversations.size(); ++i) {
        if (m_conversations[i].id == item.convId) {
            m_conversations[i].folderId = QStringLiteral("drafts");
            m_conversations[i].folderTag = QStringLiteral("Drafts");
            m_conversations[i].ccSummary = item.ccEmail;
            m_conversations[i].bccSummary = item.bccEmail;
            m_conversations[i].isImportant = item.isHighImportance;
            if (!m_conversations[i].messages.isEmpty()) {
                m_conversations[i].messages.last().senderName =
                    item.account.displayName.isEmpty() ? item.account.email : item.account.displayName;
            }
            convertedToDraft = true;
            break;
        }
    }
    if (!convertedToDraft) {
        ConversationItem draftConv;
        draftConv.id = item.convId;
        draftConv.accountId = item.account.id;
        draftConv.folderId = QStringLiteral("drafts");
        draftConv.folderTag = QStringLiteral("Drafts");
        draftConv.subject = item.subject;
        draftConv.ccSummary = item.ccEmail;
        draftConv.bccSummary = item.bccEmail;
        draftConv.isImportant = item.isHighImportance;
        MessageItem m;
        m.id = draftConv.id + "_m1";
        m.senderName = item.account.displayName.isEmpty() ? item.account.email : item.account.displayName;
        m.senderEmail = item.account.email;
        m.senderInitials = m.senderName.left(2).toUpper();
        m.avatarKind = SenderAvatarKind::UserPhoto;
        m.recipientSummary = item.toEmail;
        m.shortTimeText = MailI18n::t("방금 전", "Just now", "たった今");
        m.fullTimeText = QDateTime::currentDateTime().toString("ddd, d MMM, AP h:mm");
        m.subject = item.subject;
        m.snippet = item.bodyPlain.left(60);
        m.htmlBody = item.bodyHtml;
        m.attachments = item.attachments;
        draftConv.messages.append(m);
        m_conversations.prepend(draftConv);
    }

    // IMAP 서버의 Drafts(임시보관함) 폴더에도 안전하게 보관
    m_googleProvider->appendMessageToImapFolder(
        item.account, QStringLiteral("drafts"), item.toEmail, item.ccEmail, item.subject, item.bodyHtml, item.bccEmail, item.attachments, item.isHighImportance);

    saveConversationsCacheToDisk();
    onSelectFolder(QStringLiteral("drafts"), MailI18n::folderTitle("drafts", "Drafts"));
    refreshFolderSelectionStyles();

    statusBar()->showMessage(
        MailI18n::t("↩️ 메일 발송이 취소되어 임시보관함(Drafts)에 안전하게 저장되었습니다. 내용을 수정한 뒤 다시 보낼 수 있습니다.",
                    "↩️ Sending cancelled and saved to Drafts. You can edit and resend.",
                    "↩️ メール送信を取り消し、下書き(Drafts)に保存しました。"),
        6000);

    // 2. 작성 중이던 내용을 그대로 복원한 편지 쓰기 창을 다시 오픈 (창을 닫아도 임시보관함에 그대로 유지됨)
    ComposeDialog dlg(m_accounts, item.account.id, item.toEmail, item.subject, QString(), this);
    dlg.setAvailableContacts(m_contacts, m_customContactGroups);
    dlg.restoreUndoSendContent(item.ccEmail, item.bodyHtml, item.attachments, item.bccEmail, item.isHighImportance);
    if (dlg.exec() == QDialog::Accepted) {
        handleComposeDialogAccepted(dlg, item.convId);
    }
}

void MainWindow::handleComposeDialogAccepted(ComposeDialog &dlg, const QString &replacingConvId)
{
    if (m_pendingSend.active) {
        commitPendingSend();
    }

    if (!replacingConvId.isEmpty()) {
        for (int i = m_conversations.size() - 1; i >= 0; --i) {
            if (m_conversations[i].id == replacingConvId) {
                m_conversations.removeAt(i);
                break;
            }
        }
    }

    const QString chosenAccId = dlg.selectedAccountId().isEmpty()
                                    ? (m_accounts.size() > 1 ? m_accounts[1].id : m_selectedAccountId)
                                    : dlg.selectedAccountId();
    AccountItem sendAcc;
    sendAcc.id = chosenAccId;
    for (const AccountItem &acc : m_accounts) {
        if (acc.id == chosenAccId) {
            sendAcc = acc;
            break;
        }
    }
    if (sendAcc.email.isEmpty() && m_accounts.size() > 1) {
        sendAcc = m_accounts[1];
    }

    const bool isDraft = dlg.isDraftSaved();
    const bool isScheduled = dlg.isScheduledSend();
    const QDateTime schedDt = dlg.scheduledDateTime();

    ConversationItem newConv;
    newConv.id = QString("conv_local_%1").arg(QDateTime::currentMSecsSinceEpoch());
    newConv.accountId = sendAcc.id;
    newConv.folderId = (isDraft || isScheduled) ? QStringLiteral("drafts") : QStringLiteral("sent");
    if (isScheduled && schedDt.isValid()) {
        newConv.folderTag = MailI18n::t(QString("⏰ 예약 (%1)").arg(schedDt.toString("MM/dd HH:mm")),
                                        QString("⏰ Scheduled (%1)").arg(schedDt.toString("MM/dd HH:mm")),
                                        QString("⏰ 予約 (%1)").arg(schedDt.toString("MM/dd HH:mm")));
        newConv.scheduledSendIso = schedDt.toString(Qt::ISODate);
    } else {
        newConv.folderTag = isDraft ? QStringLiteral("Drafts") : QStringLiteral("Sent");
    }
    newConv.ccSummary = dlg.recipientCc();
    newConv.bccSummary = dlg.recipientBcc();
    newConv.subject = dlg.subjectText().isEmpty()
                          ? MailI18n::t("(제목 없음)", "(No Subject)", "(件名なし)")
                          : dlg.subjectText();
    newConv.isUnread = false;
    newConv.isImportant = dlg.isHighImportance();
    newConv.readReceiptRequested = (!isDraft && !isScheduled && m_privacySettings.privacyMode && m_privacySettings.trackSentEmails);

    MessageItem m;
    m.id = newConv.id + "_m1";
    if (isDraft && !isScheduled) {
        m.senderName = sendAcc.displayName.isEmpty() ? sendAcc.email : sendAcc.displayName;
    } else if (isScheduled) {
        m.senderName = QString("⏰ To: %1").arg(dlg.recipientTo());
    } else {
        m.senderName = QString("To: %1").arg(dlg.recipientTo());
    }
    m.senderEmail = sendAcc.email;
    m.senderInitials = (sendAcc.displayName.isEmpty() ? sendAcc.email : sendAcc.displayName).left(2).toUpper();
    m.avatarKind = SenderAvatarKind::UserPhoto;
    m.recipientSummary = dlg.recipientTo();
    m.shortTimeText = isScheduled && schedDt.isValid()
                          ? QString("⏰ %1").arg(schedDt.toString("MM/dd HH:mm"))
                          : MailI18n::t("방금 전", "Just now", "たった今");
    m.fullTimeText = isScheduled && schedDt.isValid()
                         ? MailI18n::t(QString("⏰ 예약 발송 예정: %1").arg(schedDt.toString("yyyy-MM-dd AP h:mm")),
                                       QString("⏰ Scheduled for: %1").arg(schedDt.toString("yyyy-MM-dd AP h:mm")),
                                       QString("⏰ 予約送信予定: %1").arg(schedDt.toString("yyyy-MM-dd AP h:mm")))
                         : QDateTime::currentDateTime().toString("ddd, d MMM, AP h:mm");
    m.subject = newConv.subject;
    m.snippet = dlg.bodyPlain().left(60);
    m.htmlBody = dlg.bodyHtml();
    m.attachments = dlg.attachments();
    newConv.messages.append(m);
    m_conversations.prepend(newConv);

    saveConversationsCacheToDisk();

    if (isScheduled && schedDt.isValid()) {
        m_googleProvider->appendMessageToImapFolder(
            sendAcc, QStringLiteral("drafts"), dlg.recipientTo(), dlg.recipientCc(), newConv.subject, dlg.bodyHtml(), dlg.recipientBcc(), dlg.attachments(), dlg.isHighImportance());
        onSelectFolder(QStringLiteral("drafts"), MailI18n::folderTitle("drafts", "Drafts"));
        statusBar()->showMessage(
            MailI18n::t(QString("⏰ 예약 발송이 설정되었습니다 (%1 발송 예정 — 임시보관함에서 시간 변경·취소 가능)").arg(schedDt.toString("yyyy-MM-dd AP h:mm")),
                        QString("⏰ Scheduled send set for %1 (can be rescheduled or cancelled in Drafts)").arg(schedDt.toString("yyyy-MM-dd AP h:mm")),
                        QString("⏰ 予約送信を設定しました (%1 送信予定 — 下書きで変更・取消可能)").arg(schedDt.toString("yyyy-MM-dd AP h:mm"))),
            6000);
    } else if (isDraft) {
        m_googleProvider->appendMessageToImapFolder(
            sendAcc, QStringLiteral("drafts"), dlg.recipientTo(), dlg.recipientCc(), newConv.subject, dlg.bodyHtml(), dlg.recipientBcc(), dlg.attachments(), dlg.isHighImportance());
        onSelectFolder(QStringLiteral("drafts"), MailI18n::folderTitle("drafts", "Drafts"));
        statusBar()->showMessage(
            MailI18n::t("📝 임시보관함(Drafts)에 저장되었습니다 (IMAP 서버 동기화 포함).",
                        "📝 Saved to Drafts (including IMAP server sync).",
                        "📝 下書き(Drafts)に保存されました（IMAPサーバー同期を含む）。"),
            5000);
    } else {
        // 발송 즉시 보낸편지함(Sent)으로 화면을 이동하여 보낸 메일을 바로 확인할 수 있게 함
        onSelectFolder(QStringLiteral("sent"), MailI18n::folderTitle("sent", "Sent"));

        const int delaySec = MailI18n::undoSendDelaySec();
        if (delaySec > 0) {
            m_pendingSend.active = true;
            m_pendingSend.account = sendAcc;
            m_pendingSend.convId = newConv.id;
            m_pendingSend.toEmail = dlg.recipientTo();
            m_pendingSend.ccEmail = dlg.recipientCc();
            m_pendingSend.bccEmail = dlg.recipientBcc();
            m_pendingSend.subject = newConv.subject;
            m_pendingSend.bodyHtml = dlg.bodyHtml();
            m_pendingSend.bodyPlain = dlg.bodyPlain();
            m_pendingSend.attachments = dlg.attachments();
            m_pendingSend.isHighImportance = dlg.isHighImportance();
            m_pendingSend.remainingSec = delaySec;
            updateUndoSendBannerText();
            if (m_undoSendBanner)
                m_undoSendBanner->setVisible(true);
            if (m_undoSendTimer)
                m_undoSendTimer->start();
        } else {
            m_googleProvider->sendSmtpEmail(sendAcc, dlg.recipientTo(), newConv.subject, dlg.bodyHtml(), dlg.recipientCc(), dlg.recipientBcc(), dlg.attachments(), dlg.isHighImportance());
        }
    }
}

void MainWindow::onComposeNewMail(const QString &toEmail, const QString &subject, const QString &quoteHtml)
{
    ComposeDialog dlg(m_accounts, m_selectedAccountId, toEmail, subject, quoteHtml, this);
    dlg.setAvailableContacts(m_contacts, m_customContactGroups);
    if (dlg.exec() == QDialog::Accepted) {
        handleComposeDialogAccepted(dlg);
    }
}

void MainWindow::checkScheduledSends()
{
    const QDateTime now = QDateTime::currentDateTime();
    bool anyDispatched = false;

    for (int i = 0; i < m_conversations.size(); ++i) {
        ConversationItem &c = m_conversations[i];
        if (c.scheduledSendIso.isEmpty() || c.scheduledSendIso.startsWith(QStringLiteral("EXPIRED:")))
            continue;
        QDateTime schedDt = QDateTime::fromString(c.scheduledSendIso, Qt::ISODate);
        if (!schedDt.isValid() || schedDt > now)
            continue;

        // 예약 시각 도래: 보낸편지함(Sent)으로 전환하고 실제 SMTP 발송 + IMAP Sent APPEND 실행
        const int prevDraftUid = c.imapUid;
        c.scheduledSendIso.clear();
        c.folderId = QStringLiteral("sent");
        c.folderTag = QStringLiteral("Sent");
        c.id = QString("conv_local_%1_%2").arg(QDateTime::currentMSecsSinceEpoch()).arg(i);
        c.imapUid = 0;
        c.imapSeq = 0;
        c.readReceiptRequested = (m_privacySettings.privacyMode && m_privacySettings.trackSentEmails);
        if (!c.messages.isEmpty()) {
            MessageItem &m = c.messages.last();
            m.senderName = QString("To: %1").arg(m.recipientSummary);
            m.shortTimeText = now.toString("AP h:mm");
            m.fullTimeText = now.toString("ddd, d MMM, AP h:mm");

            AccountItem sendAcc;
            sendAcc.id = c.accountId;
            for (const AccountItem &acc : m_accounts) {
                if (acc.id == c.accountId) {
                    sendAcc = acc;
                    break;
                }
            }
            if (prevDraftUid > 0 && !sendAcc.isPop3()) {
                m_googleProvider->deleteOrMoveImapMessage(sendAcc, QStringLiteral("drafts"), prevDraftUid, true);
            }
            m_googleProvider->sendSmtpEmail(sendAcc, m.recipientSummary, c.subject, m.htmlBody, c.ccSummary, c.bccSummary, m.attachments, c.isImportant);
            if (m_trayIcon && MailI18n::enableDesktopNotification()) {
                m_trayIcon->showMessage(
                    MailI18n::t("🚀 예약 메일 발송 완료", "🚀 Scheduled Email Sent", "🚀 予約メール送信完了"),
                    MailI18n::t(QString("'%1' 메일이 %2(으)로 발송되었습니다.").arg(c.subject, m.recipientSummary),
                                QString("'%1' was sent to %2.").arg(c.subject, m.recipientSummary),
                                QString("'%1' を %2 に送信しました。").arg(c.subject, m.recipientSummary)),
                    MailIcons::sollunaAppIcon(0),
                    5000);
            }
        }
        anyDispatched = true;
    }

    if (anyDispatched) {
        saveConversationsCacheToDisk();
        refreshConversationList();
        refreshFolderSelectionStyles();
        statusBar()->showMessage(
            MailI18n::t("🚀 예약된 메일이 지정 시각에 맞춰 성공적으로 발송되어 보낸편지함(Sent)으로 이동했습니다.",
                        "🚀 Scheduled email sent at the designated time and moved to Sent.",
                        "🚀 予約メールが指定時刻に送信され、送信済み(Sent)に移動しました。"),
            6000);
    }
    updateScheduledMailStatusIndicator();

    // 30초마다 우측 도킹 캘린더 및 전체 캘린더(주간·일간 뷰)의 현재 시각 빨간색 마커 갱신
    if (m_dayTimelineWidget) {
        m_dayTimelineWidget->setMarkerTime(QTime::currentTime());
    }
    if (m_weekCalViewWidget) {
        m_weekCalViewWidget->update();
    }
    if (m_dayCalViewWidget) {
        m_dayCalViewWidget->update();
    }
}

void MainWindow::updateScheduledMailStatusIndicator()
{
    if (!m_menuBarScheduledAction)
        return;

    int activeScheduledCount = 0;
    int expiredScheduledCount = 0;
    QDateTime earliestScheduledDt;
    QStringList tooltipLines;

    for (const ConversationItem &c : m_conversations) {
        if (c.scheduledSendIso.isEmpty())
            continue;
        if (c.scheduledSendIso.startsWith(QStringLiteral("EXPIRED:"))) {
            ++expiredScheduledCount;
            const QString rawIso = c.scheduledSendIso.mid(8);
            const QDateTime expDt = QDateTime::fromString(rawIso, Qt::ISODate);
            const QString timeStr = expDt.isValid() ? expDt.toString("yyyy-MM-dd AP h:mm") : rawIso;
            tooltipLines.append(
                MailI18n::t(QString("⚠️ [미발송 - 시간 경과] %1 (기존 예약: %2)").arg(c.subject, timeStr),
                            QString("⚠️ [Missed Schedule] %1 (Scheduled: %2)").arg(c.subject, timeStr),
                            QString("⚠️ [未送信 - 時刻経過] %1 (元の予約: %2)").arg(c.subject, timeStr)));
        } else {
            ++activeScheduledCount;
            const QDateTime dt = QDateTime::fromString(c.scheduledSendIso, Qt::ISODate);
            if (dt.isValid() && (!earliestScheduledDt.isValid() || dt < earliestScheduledDt)) {
                earliestScheduledDt = dt;
            }
            const QString timeStr = dt.isValid() ? dt.toString("yyyy-MM-dd AP h:mm") : c.scheduledSendIso;
            tooltipLines.append(
                MailI18n::t(QString("⏰ [발송 예약 중] %1 (예정: %2)").arg(c.subject, timeStr),
                            QString("⏰ [Scheduled] %1 (%2)").arg(c.subject, timeStr),
                            QString("⏰ [予約送信中] %1 (予定: %2)").arg(c.subject, timeStr)));
        }
    }

    if (activeScheduledCount == 0 && expiredScheduledCount == 0) {
        m_menuBarScheduledAction->setVisible(false);
        return;
    }

    const QString earliestTimeText = earliestScheduledDt.isValid()
                                         ? earliestScheduledDt.toString("MM/dd AP h:mm")
                                         : QString();
    QString barText;
    if (activeScheduledCount > 0 && expiredScheduledCount > 0) {
        barText = MailI18n::t(
            QString("⏰ 발송 예약 중 (%1건) · ⚠️ 예약 시간 경과 미발송 (%2건)")
                .arg(activeScheduledCount)
                .arg(expiredScheduledCount),
            QString("⏰ Scheduled (%1) · ⚠️ Missed Schedule (%2)")
                .arg(activeScheduledCount)
                .arg(expiredScheduledCount),
            QString("⏰ 予約送信中 (%1件) · ⚠️ 予約時刻経過未送信 (%2件)")
                .arg(activeScheduledCount)
                .arg(expiredScheduledCount));
    } else if (activeScheduledCount > 0) {
        if (!earliestTimeText.isEmpty() && activeScheduledCount == 1) {
            barText = MailI18n::t(
                QString("⏰ 발송 예약 중 (1건 · %1)").arg(earliestTimeText),
                QString("⏰ Scheduled Send (1 · %1)").arg(earliestTimeText),
                QString("⏰ 予約送信中 (1件 · %1)").arg(earliestTimeText));
        } else if (!earliestTimeText.isEmpty()) {
            barText = MailI18n::t(
                QString("⏰ 발송 예약 중 (%1건 · 최근 %2)").arg(activeScheduledCount).arg(earliestTimeText),
                QString("⏰ Scheduled Send (%1 · next %2)").arg(activeScheduledCount).arg(earliestTimeText),
                QString("⏰ 予約送信中 (%1件 · 次回 %2)").arg(activeScheduledCount).arg(earliestTimeText));
        } else {
            barText = MailI18n::t(
                QString("⏰ 발송 예약 중 (%1건)").arg(activeScheduledCount),
                QString("⏰ Scheduled Send (%1)").arg(activeScheduledCount),
                QString("⏰ 予約送信中 (%1件)").arg(activeScheduledCount));
        }
    } else {
        barText = MailI18n::t(
            QString("⚠️ 시간이 지나 보내지 못한 예약 메일 (%1건)").arg(expiredScheduledCount),
            QString("⚠️ Missed Scheduled Email (%1)").arg(expiredScheduledCount),
            QString("⚠️ 予約時刻を過ぎて未送信のメール (%1件)").arg(expiredScheduledCount));
    }

    tooltipLines.append(
        MailI18n::t("\n💡 클릭하면 임시보관함(Drafts)의 해당 예약 메일로 즉시 이동합니다.",
                    "\n💡 Click to open the scheduled message in Drafts.",
                    "\n💡 クリックすると下書き(Drafts)の該当メールへ移動します。"));
    m_menuBarScheduledAction->setText(barText);
    m_menuBarScheduledAction->setToolTip(tooltipLines.join("\n"));
    m_menuBarScheduledAction->setVisible(true);
}

void MainWindow::onRescheduleCurrentMail()
{
    if (m_selectedConvIndex < 0 || m_selectedConvIndex >= m_conversations.size())
        return;
    const QString targetConvId = m_conversations[m_selectedConvIndex].id;
    QDateTime curDt = QDateTime::fromString(m_conversations[m_selectedConvIndex].scheduledSendIso, Qt::ISODate);
    if (!curDt.isValid() || curDt <= QDateTime::currentDateTime()) {
        curDt = QDateTime::currentDateTime().addSecs(3600);
    }

    QDateTime newDt;
    if (ComposeDialog::promptScheduleDateTimeDialog(this, curDt, newDt)) {
        int foundIdx = -1;
        for (int i = 0; i < m_conversations.size(); ++i) {
            if (m_conversations[i].id == targetConvId) {
                foundIdx = i;
                break;
            }
        }
        if (foundIdx < 0 && m_selectedConvIndex >= 0 && m_selectedConvIndex < m_conversations.size()) {
            foundIdx = m_selectedConvIndex;
        }
        if (foundIdx < 0 || foundIdx >= m_conversations.size())
            return;

        ConversationItem &c = m_conversations[foundIdx];
        c.folderId = QStringLiteral("drafts");
        c.scheduledSendIso = newDt.toString(Qt::ISODate);
        c.folderTag = MailI18n::t(QString("⏰ 예약 (%1)").arg(newDt.toString("MM/dd HH:mm")),
                                  QString("⏰ Scheduled (%1)").arg(newDt.toString("MM/dd HH:mm")),
                                  QString("⏰ 予約 (%1)").arg(newDt.toString("MM/dd HH:mm")));
        if (!c.messages.isEmpty()) {
            MessageItem &m = c.messages.last();
            if (!m.senderName.startsWith("⏰")) {
                m.senderName = QString("⏰ To: %1").arg(m.recipientSummary);
            }
            m.shortTimeText = QString("⏰ %1").arg(newDt.toString("MM/dd HH:mm"));
            m.fullTimeText = MailI18n::t(QString("⏰ 예약 발송 예정: %1").arg(newDt.toString("yyyy-MM-dd AP h:mm")),
                                         QString("⏰ Scheduled for: %1").arg(newDt.toString("yyyy-MM-dd AP h:mm")),
                                         QString("⏰ 予約送信予定: %1").arg(newDt.toString("yyyy-MM-dd AP h:mm")));
        }
        saveConversationsCacheToDisk();
        refreshConversationList();
        statusBar()->showMessage(
            MailI18n::t(QString("🕒 예약 발송 시각이 %1(으)로 변경되었습니다.").arg(newDt.toString("yyyy-MM-dd AP h:mm")),
                        QString("🕒 Scheduled send time updated to %1.").arg(newDt.toString("yyyy-MM-dd AP h:mm")),
                        QString("🕒 予約送信時刻を %1 に変更しました。").arg(newDt.toString("yyyy-MM-dd AP h:mm"))),
            5000);
    }
}

void MainWindow::onEditOrCancelScheduledCurrentMail()
{
    if (m_selectedConvIndex < 0 || m_selectedConvIndex >= m_conversations.size())
        return;
    ConversationItem &c = m_conversations[m_selectedConvIndex];
    const QString convId = c.id;
    const QString accId = c.accountId;
    const QString subj = c.subject;
    const QString cc = c.ccSummary;
    const QString bcc = c.bccSummary;
    const MessageItem msg = c.messages.isEmpty() ? MessageItem() : c.latestMessage();

    // 예약을 해제하고 일반 임시보관함(Draft) 상태로 전환해 두어 창을 닫아도 임시보관함에 안전하게 남도록 함
    if (!c.scheduledSendIso.isEmpty()) {
        c.scheduledSendIso.clear();
        c.folderTag = QStringLiteral("Drafts");
        if (!c.messages.isEmpty()) {
            c.messages.last().shortTimeText = MailI18n::t("임시보관", "Draft", "下書き");
            c.messages.last().fullTimeText = QDateTime::currentDateTime().toString("ddd, d MMM, AP h:mm");
        }
        saveConversationsCacheToDisk();
        refreshConversationList();
        statusBar()->showMessage(
            MailI18n::t("✏️ 예약 발송이 해제되어 임시보관함(Drafts) 상태로 전환되었습니다.",
                        "✏️ Scheduled send cancelled and converted to Draft.",
                        "✏️ 予約送信を解除し、下書き(Drafts)に切り替えました。"),
            4000);
    }

    ComposeDialog dlg(m_accounts, accId, msg.recipientSummary, subj, QString(), this);
    dlg.setAvailableContacts(m_contacts, m_customContactGroups);
    dlg.restoreUndoSendContent(cc, msg.htmlBody, msg.attachments, bcc, c.isImportant);
    if (dlg.exec() == QDialog::Accepted) {
        handleComposeDialogAccepted(dlg, convId);
    }
}

void MainWindow::onSendNowScheduledCurrentMail()
{
    if (m_selectedConvIndex < 0 || m_selectedConvIndex >= m_conversations.size())
        return;
    ConversationItem &c = m_conversations[m_selectedConvIndex];
    if (c.messages.isEmpty())
        return;

    MessageItem &m = c.messages.last();
    if (m.recipientSummary.trimmed().isEmpty()) {
        QMessageBox::warning(
            this,
            MailI18n::t("받는 사람 필요", "Recipient Required", "宛先が必要"),
            MailI18n::t("받는 사람 이메일 주소가 비어 있습니다. [내용 수정] 버튼을 눌러 받는 사람을 입력해 주세요.",
                        "Recipient address is empty. Click [Edit] to enter a recipient.",
                        "宛先メールアドレスが空です。[編集] を押して宛先を入力してください。"));
        return;
    }

    AccountItem sendAcc;
    sendAcc.id = c.accountId;
    for (const AccountItem &acc : m_accounts) {
        if (acc.id == c.accountId) {
            sendAcc = acc;
            break;
        }
    }

    const int prevDraftUid = c.imapUid;
    c.scheduledSendIso.clear();
    c.folderId = QStringLiteral("sent");
    c.folderTag = QStringLiteral("Sent");
    c.id = QString("conv_local_%1").arg(QDateTime::currentMSecsSinceEpoch());
    c.imapUid = 0;
    c.imapSeq = 0;
    c.readReceiptRequested = (m_privacySettings.privacyMode && m_privacySettings.trackSentEmails);
    m.senderName = QString("To: %1").arg(m.recipientSummary);
    m.shortTimeText = MailI18n::t("방금 전", "Just now", "たった今");
    m.fullTimeText = QDateTime::currentDateTime().toString("ddd, d MMM, AP h:mm");

    m_selectedConvId = c.id;
    m_userExplicitlyOpenedConvIds.insert(c.id);
    if (m_scheduledMailBanner) {
        m_scheduledMailBanner->setVisible(false);
    }

    const QString toEmail = m.recipientSummary;
    const QString ccEmail = c.ccSummary;
    const QString bccEmail = c.bccSummary;
    const QString subj = c.subject;
    const QString bodyHtml = m.htmlBody;
    const QList<AttachmentItem> atts = m.attachments;
    const bool isImportant = c.isImportant;

    saveConversationsCacheToDisk();
    updateScheduledMailStatusIndicator();
    onSelectFolder(QStringLiteral("sent"), MailI18n::folderTitle("sent", "Sent"));
    if (prevDraftUid > 0 && !sendAcc.isPop3()) {
        m_googleProvider->deleteOrMoveImapMessage(sendAcc, QStringLiteral("drafts"), prevDraftUid, true);
    }
    m_googleProvider->sendSmtpEmail(sendAcc, toEmail, subj, bodyHtml, ccEmail, bccEmail, atts, isImportant);
    statusBar()->showMessage(
        MailI18n::t(QString("🚀 '%1' 메일을 즉시 발송하여 보낸편지함(Sent)으로 이동했습니다.").arg(subj),
                    QString("🚀 Sent '%1' immediately and moved to Sent.").arg(subj),
                    QString("🚀 '%1' を即時送信し、送信済み(Sent)に移動しました。").arg(subj)),
        5000);
}

void MainWindow::onReplyCurrent(bool replyAll)
{
    if (m_selectedConvIndex < 0 || m_selectedConvIndex >= m_conversations.size())
        return;
    const ConversationItem origConv = m_conversations[m_selectedConvIndex];
    const MessageItem &msg = origConv.latestMessage();
    Q_UNUSED(replyAll);

    QString subj = origConv.subject.startsWith("RE:", Qt::CaseInsensitive) ? origConv.subject : ("RE: " + origConv.subject);
    QString quote;
    if (MailI18n::includeQuoteOnReply()) {
        const QString cleanQuotedBody = stripTrackingPixelsFromHtml(msg.htmlBody);
        quote = QString("<br/><br/><hr style='border:none;border-top:1px solid #E5E7EB;'/>"
                        "<p style='color:#6B7280;font-size:12px;'>-----Original Message-----<br/>"
                        "<b>From:</b> %1 &lt;%2&gt;<br/><b>Date:</b> %3<br/><b>Subject:</b> %4</p>%5")
                    .arg(msg.senderName, msg.senderEmail, msg.fullTimeText, origConv.subject, cleanQuotedBody);
    }

    const QString defaultAccId = origConv.accountId.isEmpty() ? m_selectedAccountId : origConv.accountId;
    ComposeDialog dlg(m_accounts, defaultAccId, msg.senderEmail, subj, quote, this);
    dlg.setAvailableContacts(m_contacts, m_customContactGroups);
    if (dlg.exec() == QDialog::Accepted) {
        if (!dlg.isDraftSaved()) {
            const QString sigKey = conversationSignatureKey(origConv);
            m_repliedConvSigs.insert(sigKey);
            QSet<QString> sentFlagRequests;
            for (ConversationItem &other : m_conversations) {
                if (other.accountId == origConv.accountId
                    && (other.id == origConv.id || conversationSignatureKey(other) == sigKey)) {
                    other.isReplied = true;
                    QString imapFolder;
                    int seqNum = 0;
                    if (parseImapConversationId(other.id, imapFolder, seqNum) && seqNum > 0) {
                        const QString effFolder = imapFolder.isEmpty() ? other.folderId : imapFolder;
                        const QString reqKey = effFolder + QStringLiteral("|") + QString::number(seqNum);
                        if (!sentFlagRequests.contains(reqKey)) {
                            sentFlagRequests.insert(reqKey);
                            for (const AccountItem &acc : m_accounts) {
                                if (acc.id == other.accountId) {
                                    m_googleProvider->setImapMessageAnsweredOrForwarded(
                                        acc, effFolder, seqNum, true, false, other.subject);
                                    break;
                                }
                            }
                        }
                    }
                }
            }
            saveAccountsToDisk();
        }
        handleComposeDialogAccepted(dlg);
    }
}

void MainWindow::onForwardCurrent()
{
    if (m_selectedConvIndex < 0 || m_selectedConvIndex >= m_conversations.size())
        return;
    const ConversationItem origConv = m_conversations[m_selectedConvIndex];
    const MessageItem &msg = origConv.latestMessage();

    QString subj = "FWD: " + origConv.subject;
    const QString cleanQuotedBody = stripTrackingPixelsFromHtml(msg.htmlBody);
    QString quote = QString("<br/><br/><hr style='border:none;border-top:1px solid #E5E7EB;'/>"
                            "<p style='color:#6B7280;font-size:12px;'>---------- Forwarded message ---------<br/>"
                            "<b>From:</b> %1 &lt;%2&gt;<br/><b>Subject:</b> %3</p>%4")
                        .arg(msg.senderName, msg.senderEmail, origConv.subject, cleanQuotedBody);

    const QString defaultAccId = origConv.accountId.isEmpty() ? m_selectedAccountId : origConv.accountId;
    ComposeDialog dlg(m_accounts, defaultAccId, QString(), subj, quote, this);
    dlg.setAvailableContacts(m_contacts, m_customContactGroups);
    if (dlg.exec() == QDialog::Accepted) {
        if (!dlg.isDraftSaved()) {
            const QString sigKey = conversationSignatureKey(origConv);
            m_forwardedConvSigs.insert(sigKey);
            QSet<QString> sentFlagRequests;
            for (ConversationItem &other : m_conversations) {
                if (other.accountId == origConv.accountId
                    && (other.id == origConv.id || conversationSignatureKey(other) == sigKey)) {
                    other.isForwarded = true;
                    QString imapFolder;
                    int seqNum = 0;
                    if (parseImapConversationId(other.id, imapFolder, seqNum) && seqNum > 0) {
                        const QString effFolder = imapFolder.isEmpty() ? other.folderId : imapFolder;
                        const QString reqKey = effFolder + QStringLiteral("|") + QString::number(seqNum);
                        if (!sentFlagRequests.contains(reqKey)) {
                            sentFlagRequests.insert(reqKey);
                            for (const AccountItem &acc : m_accounts) {
                                if (acc.id == other.accountId) {
                                    m_googleProvider->setImapMessageAnsweredOrForwarded(
                                        acc, effFolder, seqNum, false, true, other.subject);
                                    break;
                                }
                            }
                        }
                    }
                }
            }
            saveAccountsToDisk();
        }
        handleComposeDialogAccepted(dlg);
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

    const QString body = cid.mid(5, lastUnder - 5);
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

void MainWindow::onArchiveCurrent()
{
    if (m_selectedConvIndex >= 0 && m_selectedConvIndex < m_conversations.size()) {
        ConversationItem &conv = m_conversations[m_selectedConvIndex];
        const QString srcFolder = conv.folderId;
        if (srcFolder == QStringLiteral("archive"))
            return;

        QStringList unblockedSenders;
        if (srcFolder == QStringLiteral("trash") || srcFolder == QStringLiteral("spam")) {
            const QString sigKey = conversationSignatureKey(conv);
            m_deletedConvIds.remove(conv.id);
            m_deletedConvIds.remove(QStringLiteral("trash|") + sigKey);
            m_deletedConvIds.remove(QStringLiteral("perm|") + sigKey);
            if (srcFolder == QStringLiteral("spam") && !conv.messages.isEmpty()) {
                if (unblockSenderFromSpamLists(m_privacySettings, conv.latestMessage().senderEmail, &unblockedSenders)) {
                    saveAccountsToDisk();
                }
            }
        }

        const int removedSeq = conv.imapSeq;
        if (removedSeq > 0) {
            const QString fk = conv.accountId + QStringLiteral("|") + srcFolder;
            if (m_folderTotalExists.value(fk, 0) > 0)
                m_folderTotalExists[fk] = m_folderTotalExists.value(fk) - 1;
            for (ConversationItem &other : m_conversations) {
                if (other.accountId == conv.accountId && other.folderId == srcFolder && other.imapSeq > removedSeq)
                    other.imapSeq -= 1;
            }
        }
        if (srcFolder == QStringLiteral("trash") || srcFolder == QStringLiteral("spam")) {
            conv.previousFolderId.clear();
        } else {
            conv.previousFolderId = srcFolder;
        }
        conv.folderId = QStringLiteral("archive");
        conv.folderTag = QStringLiteral("Archive");
        conv.imapSeq = 0;
        QString parsedFolder;
        int seqNum = 0;
        if (parseImapConversationId(conv.id, parsedFolder, seqNum)) {
            conv.id = QString("imap_%1_archive_%2").arg(conv.accountId).arg(seqNum);
            for (const AccountItem &acc : m_accounts) {
                if (acc.id == conv.accountId) {
                    m_googleProvider->moveImapMessagesToFolder(
                        acc, parsedFolder.isEmpty() ? srcFolder : parsedFolder,
                        {seqNum}, QStringLiteral("archive"), QStringLiteral("Archive"), false, false);
                    break;
                }
            }
        }
        m_selectedConvId.clear();
        saveConversationsCacheToDisk();
        refreshConversationList();
        if (!unblockedSenders.isEmpty()) {
            const QString unblockedText = unblockedSenders.join(QStringLiteral(", "));
            setWindowTitle(MailI18n::t(
                QString("SollunaMail — 스팸 메일을 보관함으로 이동하고 발신자 차단을 자동 해제했습니다 (%1)").arg(unblockedText),
                QString("SollunaMail — Archived spam message and unblocked sender (%1)").arg(unblockedText),
                QString("SollunaMail — 迷惑メールをアーカイブし、送信者ブロックを自動解除しました (%1)").arg(unblockedText)));
            if (statusBar())
                statusBar()->showMessage(windowTitle(), 5000);
        }
    }
}

QString MainWindow::conversationSignatureKey(const ConversationItem &c) const
{
    const QString sender = c.messages.isEmpty() ? QString() : c.latestMessage().senderEmail.trimmed().toLower();
    const QString fullTime = c.messages.isEmpty() ? QString() : c.latestMessage().fullTimeText.trimmed();
    return c.accountId + QStringLiteral("|") + sender + QStringLiteral("|") + c.subject.trimmed() + QStringLiteral("|") + fullTime;
}

void MainWindow::onDeleteCurrent()
{
    if (!m_checkedConvIds.isEmpty() && m_selectedFolderId != QStringLiteral("trash") && m_selectedFolderId != QStringLiteral("spam")) {
        onMoveCheckedMessagesToTrash();
        return;
    }
    if (m_selectedConvIndex < 0 || m_selectedConvIndex >= m_conversations.size())
        return;

    const QString targetConvId = m_conversations[m_selectedConvIndex].id;
    const QString subjectCopy = m_conversations[m_selectedConvIndex].subject;
    const QString initialFolder = m_conversations[m_selectedConvIndex].folderId;
    const QString sigKey = conversationSignatureKey(m_conversations[m_selectedConvIndex]);

    if (initialFolder == "trash" || initialFolder == "spam") {
        if (QMessageBox::question(
                this,
                MailI18n::t("메일 영구 삭제 확인", "Confirm Permanent Delete", "完全に削除する確認"),
                MailI18n::t(QString("'%1' 메일을 영구적으로 삭제하시겠습니까?\n(영구 삭제 후에는 복원할 수 없습니다.)").arg(subjectCopy),
                            QString("Permanently delete '%1'?").arg(subjectCopy),
                            QString("'%1' を完全に削除しますか？").arg(subjectCopy)),
                QMessageBox::Yes | QMessageBox::No) != QMessageBox::Yes) {
            return;
        }

        int foundIdx = -1;
        for (int i = 0; i < m_conversations.size(); ++i) {
            if (m_conversations[i].id == targetConvId) {
                foundIdx = i;
                break;
            }
        }
        if (foundIdx < 0)
            return;

        const QString srcFolder = m_conversations[foundIdx].folderId;
        const QString accId = m_conversations[foundIdx].accountId;
        const int removedSeq = m_conversations[foundIdx].imapSeq;

        // 로컬 블랙리스트 등록(고유 지문 기준) + IMAP 서버 영구 삭제(UID STORE +FLAGS \Deleted + EXPUNGE)
        if (!targetConvId.startsWith(QStringLiteral("imap_")))
            m_deletedConvIds.insert(targetConvId);
        m_deletedConvIds.insert(QStringLiteral("perm|") + sigKey);
        m_deletedConvIds.insert(QStringLiteral("trash|") + sigKey);

        QString imapFolder;
        int seqNum = 0;
        if (parseImapConversationId(targetConvId, imapFolder, seqNum)) {
            for (const AccountItem &acc : m_accounts) {
                if (acc.id == accId) {
                    m_googleProvider->deleteOrMoveImapMessage(acc, imapFolder, seqNum, true);
                    break;
                }
            }
        }

        m_conversations.removeAt(foundIdx);
        if (removedSeq > 0) {
            const QString fk = accId + QStringLiteral("|") + srcFolder;
            if (m_folderTotalExists.value(fk, 0) > 0)
                m_folderTotalExists[fk] = m_folderTotalExists.value(fk) - 1;
            for (ConversationItem &other : m_conversations) {
                if (other.accountId == accId && other.folderId == srcFolder && other.imapSeq > removedSeq)
                    other.imapSeq -= 1;
            }
        }
        m_selectedConvId.clear();
        saveConversationsCacheToDisk();
        refreshConversationList();
        setWindowTitle(MailI18n::t(
            "SollunaMail — 선택한 메일을 로컬 및 서버에서 영구 삭제했습니다.",
            "SollunaMail — Permanently deleted the selected message.",
            "SollunaMail — 選択したメールを完全に削除しました。"));
    } else {
        ConversationItem &conv = m_conversations[m_selectedConvIndex];
        const QString srcFolder = conv.folderId;
        const int removedSeq = conv.imapSeq;
        if (removedSeq > 0) {
            const QString fk = conv.accountId + QStringLiteral("|") + srcFolder;
            if (m_folderTotalExists.value(fk, 0) > 0)
                m_folderTotalExists[fk] = m_folderTotalExists.value(fk) - 1;
            for (ConversationItem &other : m_conversations) {
                if (other.accountId == conv.accountId && other.folderId == srcFolder && other.imapSeq > removedSeq)
                    other.imapSeq -= 1;
            }
        }
        conv.previousFolderId = srcFolder;
        conv.folderId = "trash";
        conv.folderTag = "Trash";
        conv.imapSeq = 0;

        // 원래 편지함에서 다시 살아나지 않도록 삭제/이동 이력 등록(고유 지문 기준) + IMAP 서버에서도 Trash로 이동 및 원본 EXPUNGE
        if (!conv.id.startsWith(QStringLiteral("imap_")))
            m_deletedConvIds.insert(conv.id);
        m_deletedConvIds.insert(QStringLiteral("trash|") + sigKey);

        QString imapFolder;
        int seqNum = 0;
        if (parseImapConversationId(conv.id, imapFolder, seqNum)) {
            conv.id = QString("imap_%1_trash_%2").arg(conv.accountId).arg(seqNum);
            for (const AccountItem &acc : m_accounts) {
                if (acc.id == conv.accountId) {
                    m_googleProvider->deleteOrMoveImapMessage(acc, imapFolder.isEmpty() ? srcFolder : imapFolder, seqNum, false);
                    break;
                }
            }
        }

        m_selectedConvId.clear();
        saveConversationsCacheToDisk();
        refreshConversationList();
        setWindowTitle(MailI18n::t(
            "SollunaMail — 메일을 휴지통(Trash)으로 이동했습니다. (휴지통에서 ♻️ 복원 가능)",
            "SollunaMail — Moved message to Trash. (Restorable from Trash)",
            "SollunaMail — メールをゴミ箱に移動しました。（ゴミ箱から♻️復元可能）"));
    }
}

void MainWindow::onSpamCurrent()
{
    if (m_selectedConvIndex >= 0 && m_selectedConvIndex < m_conversations.size()) {
        ConversationItem &conv = m_conversations[m_selectedConvIndex];
        const QString srcFolder = conv.folderId;
        if (srcFolder == QStringLiteral("spam"))
            return;
        const int removedSeq = conv.imapSeq;
        if (removedSeq > 0) {
            const QString fk = conv.accountId + QStringLiteral("|") + srcFolder;
            if (m_folderTotalExists.value(fk, 0) > 0)
                m_folderTotalExists[fk] = m_folderTotalExists.value(fk) - 1;
            for (ConversationItem &other : m_conversations) {
                if (other.accountId == conv.accountId && other.folderId == srcFolder && other.imapSeq > removedSeq)
                    other.imapSeq -= 1;
            }
        }
        if (srcFolder != QStringLiteral("trash")) {
            conv.previousFolderId = srcFolder;
        }
        conv.folderId = QStringLiteral("spam");
        conv.folderTag = QStringLiteral("Spam");
        conv.imapSeq = 0;
        QString parsedFolder;
        int seqNum = 0;
        if (parseImapConversationId(conv.id, parsedFolder, seqNum)) {
            conv.id = QString("imap_%1_spam_%2").arg(conv.accountId).arg(seqNum);
            for (const AccountItem &acc : m_accounts) {
                if (acc.id == conv.accountId) {
                    m_googleProvider->moveImapMessagesToFolder(
                        acc, parsedFolder.isEmpty() ? srcFolder : parsedFolder,
                        {seqNum}, QStringLiteral("spam"), QStringLiteral("Spam"), false, false);
                    break;
                }
            }
        }
        m_selectedConvId.clear();
        saveConversationsCacheToDisk();
        refreshConversationList();
    }
}

void MainWindow::onToggleUnreadCurrent()
{
    if (m_selectedConvIndex >= 0 && m_selectedConvIndex < m_conversations.size()) {
        ConversationItem &conv = m_conversations[m_selectedConvIndex];
        const bool newUnread = !conv.isUnread;
        conv.isUnread = newUnread;
        const QString sigKey = conversationSignatureKey(conv);
        if (newUnread) {
            m_readConvSigs.remove(sigKey);
        } else {
            m_readConvSigs.insert(sigKey);
            m_notifiedConvIds.insert(sigKey);
        }

        QSet<QString> sentSeenRequests;
        for (ConversationItem &other : m_conversations) {
            if (other.accountId == conv.accountId
                && (other.id == conv.id || conversationSignatureKey(other) == sigKey)) {
                other.isUnread = newUnread;
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
                                    acc, effFolder, seqNum, !newUnread, other.subject);
                                break;
                            }
                        }
                    }
                }
            }
        }

        saveConversationsCacheToDisk();
        updateAppUnreadBadgeIcon();
        refreshFolderSelectionStyles();
        refreshConversationList();
    }
}

void MainWindow::onToggleStarCurrent()
{
    if (m_selectedConvIndex >= 0 && m_selectedConvIndex < m_conversations.size()) {
        ConversationItem &conv = m_conversations[m_selectedConvIndex];
        const bool newStarred = !conv.isStarred;
        const QString sigKey = conversationSignatureKey(conv);
        const QString accId = conv.accountId;
        conv.isStarred = newStarred;
        if (newStarred) {
            m_starredConvSigs.insert(sigKey);
            m_unstarredConvSigs.remove(sigKey);
        } else {
            m_starredConvSigs.remove(sigKey);
            m_unstarredConvSigs.insert(sigKey);
        }

        QSet<QString> sentFlagRequests;
        for (ConversationItem &other : m_conversations) {
            if (other.accountId == accId
                && (other.id == conv.id || conversationSignatureKey(other) == sigKey)) {
                other.isStarred = newStarred;
                QString imapFolder;
                int seqNum = 0;
                if (parseImapConversationId(other.id, imapFolder, seqNum) && seqNum > 0) {
                    const QString effFolder = imapFolder.isEmpty() ? other.folderId : imapFolder;
                    const QString reqKey = effFolder + QStringLiteral("|") + QString::number(seqNum);
                    if (!sentFlagRequests.contains(reqKey)) {
                        sentFlagRequests.insert(reqKey);
                        for (const AccountItem &acc : m_accounts) {
                            if (acc.id == other.accountId) {
                                m_googleProvider->setImapMessageFlagged(
                                    acc, effFolder, seqNum, newStarred, other.subject);
                                break;
                            }
                        }
                    }
                }
            }
        }
        if (!newStarred) {
            for (int i = m_conversations.size() - 1; i >= 0; --i) {
                if (m_conversations[i].accountId == accId
                    && m_conversations[i].folderId == QStringLiteral("starred")
                    && conversationSignatureKey(m_conversations[i]) == sigKey) {
                    m_conversations.removeAt(i);
                    if (m_selectedConvIndex > i)
                        m_selectedConvIndex--;
                }
            }
        }

        saveConversationsCacheToDisk();
        saveAccountsToDisk();
        refreshFolderSelectionStyles();
        refreshConversationList();
        if (m_convListWidget) {
            const int curRow = m_convListWidget->currentRow();
            if (curRow >= 0 && curRow < m_filteredConvIndices.size()) {
                displayConversation(curRow, false);
            }
        }
    }
}

void MainWindow::onToggleImportantCurrent()
{
    if (m_selectedConvIndex >= 0 && m_selectedConvIndex < m_conversations.size()) {
        ConversationItem &conv = m_conversations[m_selectedConvIndex];
        const bool newImp = !conv.isImportant;
        const QString sigKey = conversationSignatureKey(conv);
        const QString accId = conv.accountId;
        conv.isImportant = newImp;
        if (newImp) {
            m_importantConvSigs.insert(sigKey);
            m_unimportantConvSigs.remove(sigKey);
        } else {
            m_importantConvSigs.remove(sigKey);
            m_unimportantConvSigs.insert(sigKey);
        }

        QSet<QString> sentImpRequests;
        for (ConversationItem &other : m_conversations) {
            if (other.accountId == accId
                && (other.id == conv.id || conversationSignatureKey(other) == sigKey)) {
                other.isImportant = newImp;
                QString imapFolder;
                int seqNum = 0;
                if (parseImapConversationId(other.id, imapFolder, seqNum) && seqNum > 0) {
                    const QString effFolder = imapFolder.isEmpty() ? other.folderId : imapFolder;
                    const QString reqKey = effFolder + QStringLiteral("|") + QString::number(seqNum);
                    if (!sentImpRequests.contains(reqKey)) {
                        sentImpRequests.insert(reqKey);
                        for (const AccountItem &acc : m_accounts) {
                            if (acc.id == other.accountId) {
                                m_googleProvider->setImapMessageImportant(
                                    acc, effFolder, seqNum, newImp, other.subject);
                                break;
                            }
                        }
                    }
                }
            }
        }
        if (!newImp) {
            for (int i = m_conversations.size() - 1; i >= 0; --i) {
                if (m_conversations[i].accountId == accId
                    && m_conversations[i].folderId == QStringLiteral("important")
                    && conversationSignatureKey(m_conversations[i]) == sigKey) {
                    m_conversations.removeAt(i);
                    if (m_selectedConvIndex > i)
                        m_selectedConvIndex--;
                }
            }
        }

        saveConversationsCacheToDisk();
        saveAccountsToDisk();
        refreshFolderSelectionStyles();
        refreshConversationList();
        if (m_convListWidget) {
            const int curRow = m_convListWidget->currentRow();
            if (curRow >= 0 && curRow < m_filteredConvIndices.size()) {
                displayConversation(curRow, false);
            }
        }
    }
}

void MainWindow::onUnsubscribeCurrent()
{
    if (m_selectedConvIndex < 0 || m_selectedConvIndex >= m_conversations.size())
        return;
    const ConversationItem conv = m_conversations[m_selectedConvIndex];
    if (conv.messages.isEmpty())
        return;
    const MessageItem msg = conv.latestMessage();
    const QString unsubTarget = extractUnsubscribeTargetFromHtml(msg.htmlBody);

    if (unsubTarget.isEmpty()) {
        if (QMessageBox::question(
                this,
                MailI18n::t("구독 취소 링크 없음", "No Unsubscribe Link Found", "配信停止リンクなし"),
                MailI18n::t(
                    QString("이 메일에는 뉴스레터 수신거부(구독 취소) 링크가 포함되어 있지 않습니다.\n\n"
                            "대신 발신자 [%1 <%2>]의 메일을 차단(스팸함 이동)하시겠습니까?")
                        .arg(msg.senderName, msg.senderEmail),
                    QString("This email does not contain an unsubscribe link.\n\n"
                            "Would you like to block the sender [%1 <%2>] instead?")
                        .arg(msg.senderName, msg.senderEmail),
                    QString("このメールには配信停止リンクが含まれていません。\n\n"
                            "代わりに送信者 [%1 <%2>] をブロックしますか？")
                        .arg(msg.senderName, msg.senderEmail)),
                QMessageBox::Yes | QMessageBox::No) == QMessageBox::Yes) {
            onBlockSenderCurrent();
        }
        return;
    }

    if (QMessageBox::question(
            this,
            MailI18n::t("구독 취소 (Unsubscribe)", "Unsubscribe", "配信停止"),
            MailI18n::t(
                QString("이 메일 본문에서 감지된 실제 수신거부(구독 취소) 링크를 실행하시겠습니까?\n\n"
                        "• 발신자: %1 <%2>\n"
                        "• 수신거부 대상: %3")
                    .arg(msg.senderName, msg.senderEmail, unsubTarget.left(120)),
                QString("Open the unsubscribe link found in this email?\n\n"
                        "• Sender: %1 <%2>\n"
                        "• Unsubscribe URL: %3")
                    .arg(msg.senderName, msg.senderEmail, unsubTarget.left(120)),
                QString("このメール内の配信停止リンクを実行しますか？\n\n"
                        "• 送信者: %1 <%2>\n"
                        "• リンク: %3")
                    .arg(msg.senderName, msg.senderEmail, unsubTarget.left(120))),
            QMessageBox::Yes | QMessageBox::No) != QMessageBox::Yes) {
        return;
    }

    if (unsubTarget.startsWith(QLatin1String("mailto:"), Qt::CaseInsensitive)) {
        const QUrl mailUrl(unsubTarget);
        onComposeNewMail(mailUrl.path(), QStringLiteral("Unsubscribe / 수신거부 요청"), QString());
    } else {
        QDesktopServices::openUrl(QUrl(unsubTarget));
    }
}

void MainWindow::onBlockSenderCurrent()
{
    if (m_selectedConvIndex < 0 || m_selectedConvIndex >= m_conversations.size())
        return;
    const QString senderEmailDisplay = m_conversations[m_selectedConvIndex].latestMessage().senderEmail.trimmed();
    const QString blockedEmail = extractCleanEmailLower(senderEmailDisplay);
    if (!blockedEmail.isEmpty()) {
        m_privacySettings.privacyMode = true;
        m_privacySettings.blockSenders = true;
        if (!m_privacySettings.blockedSendersList.contains(blockedEmail, Qt::CaseInsensitive)) {
            m_privacySettings.blockedSendersList.append(blockedEmail);
        }
    }
    int movedCount = 0;
    QMap<QString, QList<int>> spamSeqsByAccAndSrc;
    for (ConversationItem &c : m_conversations) {
        if (c.messages.isEmpty())
            continue;
        if (!blockedEmail.isEmpty()
            && isSenderMatchedByBlockedRule(c.latestMessage().senderEmail, blockedEmail)
            && c.folderId != "spam" && c.folderId != "trash") {
            const QString srcFolder = c.folderId;
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
            c.previousFolderId = srcFolder;
            c.folderId = "spam";
            c.folderTag = "Spam";
            c.isUnread = false;
            m_readConvSigs.insert(conversationSignatureKey(c));
            c.imapSeq = 0;
            QString parsedFolder;
            int seqNum = 0;
            if (parseImapConversationId(c.id, parsedFolder, seqNum)) {
                c.id = QString("imap_%1_spam_%2").arg(c.accountId).arg(seqNum);
                const QString key = c.accountId + QStringLiteral("|") + (parsedFolder.isEmpty() ? srcFolder : parsedFolder);
                if (!spamSeqsByAccAndSrc[key].contains(seqNum))
                    spamSeqsByAccAndSrc[key].append(seqNum);
            }
            ++movedCount;
        }
    }
    for (auto it = spamSeqsByAccAndSrc.begin(); it != spamSeqsByAccAndSrc.end(); ++it) {
        const QString accId = it.key().section('|', 0, 0);
        const QString srcFolder = it.key().section('|', 1);
        for (const AccountItem &acc : m_accounts) {
            if (acc.id == accId) {
                m_googleProvider->moveImapMessagesToFolder(
                    acc, srcFolder, it.value(), QStringLiteral("spam"), QStringLiteral("Spam"), true, false);
                break;
            }
        }
    }
    saveAccountsToDisk();
    saveConversationsCacheToDisk();
    QMessageBox::information(
        this,
        MailI18n::t("발신자 차단 (Block sender)", "Block Sender", "送信者をブロック"),
        MailI18n::t(QString("[%1] 발신자를 차단 목록(Privacy Hub)에 등록하고 해당 발신자의 메일(%2통)을 스팸함으로 이동했습니다.\n"
                            "(차단 목록은 [설정 > 개인정보 보호 > 차단된 발신자 목록 관리]에서 언제든 해제할 수 있습니다.)")
                        .arg(senderEmailDisplay)
                        .arg(qMax(1, movedCount)),
                    QString("Added [%1] to the blocked senders list (Privacy Hub) and moved %2 message(s) to Spam.\n"
                            "(You can unblock senders anytime in Settings > Privacy > Blocked Senders.)")
                        .arg(senderEmailDisplay)
                        .arg(qMax(1, movedCount)),
                    QString("[%1] をブロックリスト（プライバシーハブ）に登録し、%2通のメールを迷惑メールへ移動しました。")
                        .arg(senderEmailDisplay)
                        .arg(qMax(1, movedCount))));
    refreshConversationList();
}

void MainWindow::onMarkCheckedMessagesRead()
{
    if (m_checkedConvIds.isEmpty()) {
        QMessageBox::information(
            this,
            MailI18n::t("선택한 메일 읽음으로 표시", "Mark Selected as Read", "選択したメールを既読にする"),
            MailI18n::t("먼저 메일 목록 좌측의 체크박스를 체크하여 읽음으로 표시할 메일을 선택해 주세요.",
                        "Please check at least one message checkbox in the mail list first.",
                        "先にメール一覧の左側のチェックボックスを選択してください。"));
        return;
    }

    int markedCount = 0;
    QMap<QString, QList<int>> imapBatchSeqs; // key: accountId|imapFolder

    for (ConversationItem &c : m_conversations) {
        if (!m_checkedConvIds.contains(c.id))
            continue;
        const QString sigKey = conversationSignatureKey(c);
        m_readConvSigs.insert(sigKey);
        m_notifiedConvIds.insert(sigKey);
        c.isUnread = false;
        ++markedCount;

        QString imapFolder;
        int seqNum = 0;
        if (parseImapConversationId(c.id, imapFolder, seqNum) && seqNum > 0) {
            const QString srcFolder = imapFolder.isEmpty() ? c.folderId : imapFolder;
            imapBatchSeqs[c.accountId + QStringLiteral("|") + srcFolder].append(seqNum);
        }
    }

    for (auto it = imapBatchSeqs.constBegin(); it != imapBatchSeqs.constEnd(); ++it) {
        const QStringList parts = it.key().split('|');
        if (parts.size() < 2)
            continue;
        const QString accId = parts.first();
        const QString srcFolder = parts.mid(1).join('|');
        for (const AccountItem &acc : m_accounts) {
            if (acc.id == accId) {
                m_googleProvider->setImapMessagesSeenFlagBatch(acc, srcFolder, it.value(), true);
                break;
            }
        }
    }

    saveConversationsCacheToDisk();
    updateAppUnreadBadgeIcon();
    refreshFolderSelectionStyles();
    if (m_convListWidget && m_convListWidget->viewport())
        m_convListWidget->viewport()->update();

    setWindowTitle(MailI18n::t(
        QString("SollunaMail — 선택한 메일 %1건 읽음으로 표시 완료").arg(markedCount),
        QString("SollunaMail — Marked %1 selected message(s) as read").arg(markedCount),
        QString("SollunaMail — 選択したメール %1件を既読にしました").arg(markedCount)));
}

void MainWindow::onMoveCheckedMessagesToTrash()
{
    if (m_checkedConvIds.isEmpty()) {
        QMessageBox::information(
            this,
            MailI18n::t("선택한 메일 휴지통으로 이동", "Move Selected to Trash", "選択したメールをゴミ箱へ移動"),
            MailI18n::t("먼저 메일 목록 좌측의 체크박스를 체크하여 휴지통으로 이동할 메일을 선택해 주세요.",
                        "Please check at least one message checkbox in the mail list first.",
                        "先にメール一覧の左側のチェックボックスを選択してください。"));
        return;
    }

    int movedCount = 0;
    QMap<QString, QList<int>> imapBatchSeqs; // key: accountId|imapFolder

    for (ConversationItem &c : m_conversations) {
        if (!m_checkedConvIds.contains(c.id))
            continue;

        const QString oldId = c.id;
        const QString srcFolder = c.folderId;
        const QString sigKey = conversationSignatureKey(c);

        if (!c.id.startsWith(QStringLiteral("imap_")))
            m_deletedConvIds.insert(c.id);
        m_deletedConvIds.insert(QStringLiteral("trash|") + sigKey);

        QString imapFolder;
        int seqNum = 0;
        if (parseImapConversationId(c.id, imapFolder, seqNum) && seqNum > 0) {
            const QString realSrc = imapFolder.isEmpty() ? srcFolder : imapFolder;
            if (realSrc != QStringLiteral("trash")) {
                imapBatchSeqs[c.accountId + QStringLiteral("|") + realSrc].append(seqNum);
                c.id = QString("imap_%1_trash_%2").arg(c.accountId).arg(seqNum);
            }
        }

        if (srcFolder != QStringLiteral("trash")) {
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
            c.previousFolderId = srcFolder;
            c.folderId = QStringLiteral("trash");
            c.folderTag = QStringLiteral("Trash");
            c.imapSeq = 0;
        }
        m_checkedConvIds.remove(oldId);
        m_userExplicitlyOpenedConvIds.remove(oldId);
        if (m_selectedConvId == oldId)
            m_selectedConvId.clear();
        ++movedCount;
    }

    for (auto it = imapBatchSeqs.constBegin(); it != imapBatchSeqs.constEnd(); ++it) {
        const QStringList parts = it.key().split('|');
        if (parts.size() < 2)
            continue;
        const QString accId = parts.first();
        const QString srcFolder = parts.mid(1).join('|');
        for (const AccountItem &acc : m_accounts) {
            if (acc.id == accId) {
                m_googleProvider->moveImapMessagesToFolder(
                    acc, srcFolder, it.value(), QStringLiteral("trash"), QStringLiteral("Trash"), false, false);
                break;
            }
        }
    }

    saveConversationsCacheToDisk();
    refreshConversationList();
    setWindowTitle(MailI18n::t(
        QString("SollunaMail — 선택한 메일 %1건을 휴지통(Trash)으로 이동했습니다.").arg(movedCount),
        QString("SollunaMail — Moved %1 selected message(s) to Trash.").arg(movedCount),
        QString("SollunaMail — 選択したメール %1件をゴミ箱に移動しました。").arg(movedCount)));
}

void MainWindow::onMarkFolderAllRead(const QString &folderId)
{
    const QString targetFolder = folderId.isEmpty() ? m_selectedFolderId : folderId;
    int markedCount = 0;

    for (ConversationItem &c : m_conversations) {
        if (m_selectedAccountId != "acc_unified" && c.accountId != m_selectedAccountId)
            continue;
        bool matchFolder = false;
        if (targetFolder == "starred") {
            matchFolder = (c.isStarred || c.folderId == "starred");
        } else if (targetFolder == "important") {
            matchFolder = (c.isImportant || c.folderId == "important");
        } else {
            matchFolder = (c.folderId == targetFolder);
        }
        if (matchFolder) {
            const QString sigKey = conversationSignatureKey(c);
            m_readConvSigs.insert(sigKey);
            m_notifiedConvIds.insert(sigKey);
            if (c.isUnread) {
                c.isUnread = false;
                ++markedCount;
            }
        }
    }

    for (const AccountItem &acc : m_accounts) {
        if (acc.provider == AccountProviderType::Unified)
            continue;
        if (m_selectedAccountId != "acc_unified" && acc.id != m_selectedAccountId)
            continue;
        if (!acc.appPassword.isEmpty() || !acc.oauthAccessToken.isEmpty()) {
            m_googleProvider->markImapFolderAllRead(acc, targetFolder);
        }
    }

    saveConversationsCacheToDisk();
    refreshConversationList();
    const QString fTitle = MailI18n::folderTitle(targetFolder, targetFolder);
    setWindowTitle(MailI18n::t(
        QString("SollunaMail — '%1' 폴더 메일 %2건 모두 읽음 처리 완료").arg(fTitle).arg(markedCount),
        QString("SollunaMail — Marked all %2 message(s) in '%1' as read").arg(fTitle).arg(markedCount),
        QString("SollunaMail — '%1' フォルダのメール %2件をすべて既読にしました").arg(fTitle).arg(markedCount)));
}

void MainWindow::onMoveFolderAllToTrash(const QString &folderId)
{
    const QString targetFolder = folderId.isEmpty() ? m_selectedFolderId : folderId;
    if (targetFolder == "trash" || targetFolder == "spam") {
        onEmptyFolder(targetFolder);
        return;
    }

    const QString folderTitle = MailI18n::folderTitle(targetFolder, targetFolder);
    auto isMatchTargetFolder = [&targetFolder](const ConversationItem &c) -> bool {
        if (c.folderId == QLatin1String("trash") || c.folderId == QLatin1String("spam"))
            return false;
        if (targetFolder == QLatin1String("starred"))
            return c.isStarred;
        if (targetFolder == QLatin1String("important"))
            return c.isImportant || c.folderId == QLatin1String("important");
        return c.folderId == targetFolder;
    };

    int targetCount = 0;
    for (const ConversationItem &c : m_conversations) {
        if (m_selectedAccountId != "acc_unified" && c.accountId != m_selectedAccountId)
            continue;
        if (isMatchTargetFolder(c))
            ++targetCount;
    }

    if (targetCount <= 0) {
        QMessageBox::information(this,
                                 MailI18n::t("안내", "Notice", "案内"),
                                 MailI18n::t(QString("'%1' 폴더에 휴지통으로 이동할 메일이 없습니다.").arg(folderTitle),
                                             QString("No messages in '%1' to move to Trash.").arg(folderTitle),
                                             QString("'%1' に移動するメールがありません。").arg(folderTitle)));
        return;
    }

    if (QMessageBox::question(
            this,
            MailI18n::t("휴지통으로 모두 이동 확인", "Move All to Trash", "すべてゴミ箱へ移動"),
            MailI18n::t(QString("'%1' 폴더의 메일(%2건)을 모두 휴지통(Trash)으로 이동하시겠습니까?\n\n"
                                "• 중요 편지함(받은편지함·보낸편지함 등)은 영구 삭제되지 않고 안전하게 휴지통으로 이동됩니다.\n"
                                "• 휴지통에서 언제든지 [♻️ 복원] 버튼으로 원래 편지함으로 되돌릴 수 있습니다.")
                            .arg(folderTitle).arg(targetCount),
                        QString("Move all %2 messages in '%1' to Trash?\n(You can restore them from Trash anytime.)")
                            .arg(folderTitle).arg(targetCount),
                        QString("'%1' のメール（%2件）をすべてゴミ箱へ移動しますか？")
                            .arg(folderTitle).arg(targetCount)),
            QMessageBox::Yes | QMessageBox::No) != QMessageBox::Yes) {
        return;
    }

    int movedCount = 0;
    QHash<QString, QList<int>> imapBatchSeqs;
    for (ConversationItem &c : m_conversations) {
        if (m_selectedAccountId != "acc_unified" && c.accountId != m_selectedAccountId)
            continue;
        if (isMatchTargetFolder(c)) {
            const QString oldId = c.id;
            const QString srcFolder = c.folderId;
            const QString sigKey = conversationSignatureKey(c);
            m_checkedConvIds.remove(oldId);
            if (!c.id.startsWith(QStringLiteral("imap_")))
                m_deletedConvIds.insert(c.id);
            m_deletedConvIds.insert(QStringLiteral("trash|") + sigKey);

            QString imapFolder;
            int seqNum = 0;
            if (parseImapConversationId(c.id, imapFolder, seqNum) && seqNum > 0) {
                const QString effectiveSrc = imapFolder.isEmpty() ? srcFolder : imapFolder;
                imapBatchSeqs[c.accountId + QStringLiteral("|") + effectiveSrc].append(seqNum);
                c.id = QString("imap_%1_trash_%2").arg(c.accountId).arg(seqNum);
                m_checkedConvIds.remove(c.id);
            }

            c.previousFolderId = srcFolder;
            c.folderId = "trash";
            c.folderTag = "Trash";
            c.imapSeq = 0;
            if (targetFolder == "starred")
                c.isStarred = false;
            else if (targetFolder == "important")
                c.isImportant = false;
            ++movedCount;
        }
    }

    for (auto it = imapBatchSeqs.cbegin(); it != imapBatchSeqs.cend(); ++it) {
        const QStringList parts = it.key().split('|');
        if (parts.size() < 2)
            continue;
        const QString accId = parts.at(0);
        const QString srcFolder = parts.mid(1).join('|');
        for (const AccountItem &acc : m_accounts) {
            if (acc.id == accId) {
                m_googleProvider->moveImapMessagesToFolder(acc, srcFolder, it.value(), QStringLiteral("trash"), QStringLiteral("Trash"));
                break;
            }
        }
    }

    if (m_selectedAccountId != "acc_unified") {
        m_folderTotalExists[m_selectedAccountId + QStringLiteral("|") + targetFolder] = 0;
    }

    m_selectedConvId.clear();
    saveConversationsCacheToDisk();
    refreshConversationList();
    setWindowTitle(MailI18n::t(
        QString("SollunaMail — '%1' 폴더의 메일 %2건을 휴지통(Trash)으로 이동했습니다. (휴지통에서 복원 가능)").arg(folderTitle).arg(movedCount),
        QString("SollunaMail — Moved %2 message(s) from '%1' to Trash. (Restorable from Trash)").arg(folderTitle).arg(movedCount),
        QString("SollunaMail — '%1' フォルダのメール %2件をゴミ箱に移動しました。（復元可能）").arg(folderTitle).arg(movedCount)));
}

void MainWindow::onEmptyFolder(const QString &folderId)
{
    const QString targetFolder = folderId.isEmpty() ? m_selectedFolderId : folderId;
    // 중요 편지함(받은편지함·보낸편지함·보관함·중요·별표·임시보관함·내 폴더)은 절대 영구 비우기 금지 -> 휴지통 이동으로 안전 전환!
    if (targetFolder != "trash" && targetFolder != "spam") {
        onMoveFolderAllToTrash(targetFolder);
        return;
    }

    const QString folderTitle = MailI18n::folderTitle(targetFolder, targetFolder);

    int targetCount = 0;
    for (const ConversationItem &c : m_conversations) {
        if (m_selectedAccountId != "acc_unified" && c.accountId != m_selectedAccountId)
            continue;
        if (c.folderId == targetFolder)
            ++targetCount;
    }

    if (QMessageBox::question(
            this,
            MailI18n::t("영구 비우기 확인", "Confirm Empty Trash/Spam", "完全に空にする確認"),
            MailI18n::t(QString("'%1'의 메일(%2건)을 모두 영구 삭제(비우기)하시겠습니까?\n(서버의 '%1'에서도 영구 삭제(EXPUNGE) 처리가 진행됩니다.)")
                            .arg(folderTitle).arg(targetCount),
                        QString("Are you sure you want to permanently empty '%1' (%2 messages)?")
                            .arg(folderTitle).arg(targetCount),
                        QString("'%1' のメール（%2件）をすべて完全に削除しますか？")
                            .arg(folderTitle).arg(targetCount)),
            QMessageBox::Yes | QMessageBox::No) != QMessageBox::Yes) {
        return;
    }

    for (int i = m_conversations.size() - 1; i >= 0; --i) {
        if (m_selectedAccountId != "acc_unified" && m_conversations[i].accountId != m_selectedAccountId)
            continue;
        if (m_conversations[i].folderId == targetFolder) {
            const QString sigKey = conversationSignatureKey(m_conversations[i]);
            m_checkedConvIds.remove(m_conversations[i].id);
            if (!m_conversations[i].id.startsWith(QStringLiteral("imap_")))
                m_deletedConvIds.insert(m_conversations[i].id);
            m_deletedConvIds.insert(QStringLiteral("perm|") + sigKey);
            m_deletedConvIds.insert(QStringLiteral("trash|") + sigKey);
            m_conversations.removeAt(i);
        }
    }

    for (const AccountItem &acc : m_accounts) {
        if (acc.provider == AccountProviderType::Unified)
            continue;
        if (m_selectedAccountId != "acc_unified" && acc.id != m_selectedAccountId)
            continue;
        m_folderTotalExists[acc.id + QStringLiteral("|") + targetFolder] = 0;
        if (!acc.appPassword.isEmpty() || !acc.oauthAccessToken.isEmpty()) {
            m_googleProvider->emptyImapFolder(acc, targetFolder);
        }
    }

    m_selectedConvId.clear();
    saveConversationsCacheToDisk();
    refreshConversationList();
    setWindowTitle(MailI18n::t(
        QString("SollunaMail — '%1' 영구 비우기 완료").arg(folderTitle),
        QString("SollunaMail — Emptied '%1' permanently").arg(folderTitle),
        QString("SollunaMail — '%1' を完全に空にしました").arg(folderTitle)));
}

void MainWindow::onRestoreCurrentFromTrash()
{
    if (m_selectedConvIndex < 0 || m_selectedConvIndex >= m_conversations.size()) {
        QMessageBox::information(this,
                                 MailI18n::t("복원할 메일 선택", "Select Message to Restore", "復元するメールを選択"),
                                 MailI18n::t("복원할 메일을 목록에서 먼저 선택해 주세요.",
                                             "Please select a message to restore first.",
                                             "復元するメールを先に選択してください。"));
        return;
    }

    ConversationItem &conv = m_conversations[m_selectedConvIndex];
    if (conv.folderId != "trash" && conv.folderId != "spam") {
        QMessageBox::information(this,
                                 MailI18n::t("안내", "Notice", "案内"),
                                 MailI18n::t("현재 선택한 메일은 이미 일반 편지함에 있습니다.",
                                             "This message is already in a normal folder.",
                                             "このメールはすでに通常のフォルダにあります。"));
        return;
    }

    QString targetFolder = conv.previousFolderId.trimmed();
    if (targetFolder.isEmpty() || targetFolder == "trash" || targetFolder == "spam") {
        // 보낸 사람이 내 계정이면 보낸편지함(sent), 아니면 받은편지함(inbox)으로 복원
        targetFolder = QStringLiteral("inbox");
        if (!conv.messages.isEmpty() && conv.messages.last().senderName.startsWith("To:", Qt::CaseInsensitive)) {
            targetFolder = QStringLiteral("sent");
        }
    }

    auto folderTagFor = [](const QString &fid) -> QString {
        if (fid == "inbox")     return QStringLiteral("Inbox");
        if (fid == "sent")      return QStringLiteral("Sent");
        if (fid == "drafts")    return QStringLiteral("Drafts");
        if (fid == "starred")   return QStringLiteral("Starred");
        if (fid == "archive")   return QStringLiteral("Archive");
        if (fid == "important") return QStringLiteral("Important");
        return fid;
    };

    const QString srcFolder = (conv.folderId == "spam") ? QStringLiteral("spam") : QStringLiteral("trash");
    const QString subjectCopy = conv.subject;
    const QString targetTitle = MailI18n::folderTitle(targetFolder, targetFolder);
    const QString sigKey = conversationSignatureKey(conv);

    // 1. 로컬 삭제/이동 블랙리스트에서 해제하여 원래 편지함에서 정상 유지되도록 복구
    m_checkedConvIds.remove(conv.id);
    m_deletedConvIds.remove(conv.id);
    m_deletedConvIds.remove(QStringLiteral("trash|") + sigKey);
    m_deletedConvIds.remove(QStringLiteral("perm|") + sigKey);

    // 1-B. 스팸함에서 복원하는 경우, 차단된 발신자 목록 및 스팸 이동 필터 규칙에 포함되어 있으면 자동으로 차단을 해제하여 재동기화 시 다시 스팸으로 이동되지 않도록 보장
    QStringList unblockedSenders;
    if (srcFolder == QStringLiteral("spam") && !conv.messages.isEmpty()) {
        if (unblockSenderFromSpamLists(m_privacySettings, conv.latestMessage().senderEmail, &unblockedSenders)) {
            saveAccountsToDisk();
        }
    }

    const int removedSeq = conv.imapSeq;
    if (removedSeq > 0) {
        const QString fk = conv.accountId + QStringLiteral("|") + srcFolder;
        if (m_folderTotalExists.value(fk, 0) > 0)
            m_folderTotalExists[fk] = m_folderTotalExists.value(fk) - 1;
        for (ConversationItem &other : m_conversations) {
            if (other.accountId == conv.accountId && other.folderId == srcFolder && other.imapSeq > removedSeq)
                other.imapSeq -= 1;
        }
    }

    conv.folderId = targetFolder;
    conv.folderTag = folderTagFor(targetFolder);
    conv.previousFolderId.clear();
    conv.imapSeq = 0;

    QString parsedSrcFolder;
    int seqNum = 0;
    const bool hasValidSeq = parseImapConversationId(conv.id, parsedSrcFolder, seqNum) && seqNum > 0;
    if (hasValidSeq) {
        conv.id = QString("imap_%1_%2_%3").arg(conv.accountId, targetFolder).arg(seqNum);
    }

    for (const AccountItem &acc : m_accounts) {
        if (acc.id != conv.accountId)
            continue;
        if (hasValidSeq) {
            m_googleProvider->restoreImapMessageFromTrash(acc, seqNum, targetFolder, srcFolder);
        } else if (!conv.messages.isEmpty()) {
            const MessageItem &msg = conv.latestMessage();
            m_googleProvider->appendMessageToImapFolder(acc,
                                                        targetFolder,
                                                        msg.recipientSummary,
                                                        conv.ccSummary,
                                                        conv.subject,
                                                        msg.htmlBody,
                                                        conv.bccSummary);
        }
        break;
    }

    m_selectedConvId.clear();
    saveConversationsCacheToDisk();
    refreshConversationList();
    setWindowTitle(MailI18n::t(
        QString("SollunaMail — ♻️ '%1' 메일을 '%2'(으)로 로컬 및 서버에서 복원했습니다.").arg(subjectCopy, targetTitle),
        QString("SollunaMail — ♻️ Restored '%1' to '%2'.").arg(subjectCopy, targetTitle),
        QString("SollunaMail — ♻️ '%1' メールを '%2' に復元しました。").arg(subjectCopy, targetTitle)));
}

void MainWindow::onRestoreAllFromTrash(const QString &sourceFolderId)
{
    const QString srcFolder = (sourceFolderId == "spam") ? QStringLiteral("spam") : QStringLiteral("trash");
    const QString srcTitle = MailI18n::folderTitle(srcFolder, srcFolder);

    int restoreCount = 0;
    for (const ConversationItem &c : m_conversations) {
        if (m_selectedAccountId != "acc_unified" && c.accountId != m_selectedAccountId)
            continue;
        if (c.folderId == srcFolder)
            ++restoreCount;
    }

    if (restoreCount <= 0) {
        QMessageBox::information(this,
                                 MailI18n::t("복원 안내", "Restore", "復元案内"),
                                 MailI18n::t(QString("'%1'에 복원할 메일이 없습니다.").arg(srcTitle),
                                             QString("No messages in '%1' to restore.").arg(srcTitle),
                                             QString("'%1' に復元するメールがありません。").arg(srcTitle)));
        return;
    }

    if (QMessageBox::question(
            this,
            MailI18n::t("전체 복원 확인", "Confirm Restore All", "すべて復元する確認"),
            MailI18n::t(QString("'%1'에 있는 메일(%2건)을 모두 원래 편지함(받은편지함·보낸편지함 등)으로 복원하시겠습니까?")
                            .arg(srcTitle).arg(restoreCount),
                        QString("Restore all %2 messages from '%1' back to their original folders?")
                            .arg(srcTitle).arg(restoreCount),
                        QString("'%1' のメール（%2件）をすべて元のフォルダに復元しますか？")
                            .arg(srcTitle).arg(restoreCount)),
            QMessageBox::Yes | QMessageBox::No) != QMessageBox::Yes) {
        return;
    }

    auto folderTagFor = [](const QString &fid) -> QString {
        if (fid == "inbox")     return QStringLiteral("Inbox");
        if (fid == "sent")      return QStringLiteral("Sent");
        if (fid == "drafts")    return QStringLiteral("Drafts");
        if (fid == "starred")   return QStringLiteral("Starred");
        if (fid == "archive")   return QStringLiteral("Archive");
        if (fid == "important") return QStringLiteral("Important");
        return fid;
    };

    int doneCount = 0;
    bool unblockedAny = false;
    QStringList unblockedSenders;
    QHash<QString, QList<int>> imapRestoreBatch;
    for (ConversationItem &c : m_conversations) {
        if (m_selectedAccountId != "acc_unified" && c.accountId != m_selectedAccountId)
            continue;
        if (c.folderId != srcFolder)
            continue;

        QString targetFolder = c.previousFolderId.trimmed();
        if (targetFolder.isEmpty() || targetFolder == "trash" || targetFolder == "spam") {
            targetFolder = (!c.messages.isEmpty() && c.messages.last().senderName.startsWith("To:", Qt::CaseInsensitive))
                               ? QStringLiteral("sent")
                               : QStringLiteral("inbox");
        }
        const QString oldId = c.id;
        const QString sigKey = conversationSignatureKey(c);
        m_checkedConvIds.remove(oldId);
        m_deletedConvIds.remove(oldId);
        m_deletedConvIds.remove(QStringLiteral("trash|") + sigKey);
        m_deletedConvIds.remove(QStringLiteral("perm|") + sigKey);

        if (srcFolder == QStringLiteral("spam") && !c.messages.isEmpty()) {
            if (unblockSenderFromSpamLists(m_privacySettings, c.latestMessage().senderEmail, &unblockedSenders)) {
                unblockedAny = true;
            }
        }

        c.folderId = targetFolder;
        c.folderTag = folderTagFor(targetFolder);
        c.previousFolderId.clear();
        c.imapSeq = 0;

        QString parsedSrcFolder;
        int seqNum = 0;
        const bool hasValidSeq = parseImapConversationId(c.id, parsedSrcFolder, seqNum) && seqNum > 0;
        if (hasValidSeq) {
            imapRestoreBatch[c.accountId + QStringLiteral("|") + targetFolder].append(seqNum);
            c.id = QString("imap_%1_%2_%3").arg(c.accountId, targetFolder).arg(seqNum);
            m_checkedConvIds.remove(c.id);
        } else if (!c.messages.isEmpty()) {
            for (const AccountItem &acc : m_accounts) {
                if (acc.id == c.accountId) {
                    const MessageItem &msg = c.latestMessage();
                    m_googleProvider->appendMessageToImapFolder(acc,
                                                                targetFolder,
                                                                msg.recipientSummary,
                                                                c.ccSummary,
                                                                c.subject,
                                                                msg.htmlBody,
                                                                c.bccSummary);
                    break;
                }
            }
        }
        ++doneCount;
    }

    auto resolveFolderName = [this](const QString &fid) -> QString {
        for (const FolderItem &f : m_folders) {
            if (f.id == fid)
                return f.name.trimmed().isEmpty() ? fid : f.name.trimmed();
        }
        return MailI18n::folderTitle(fid, fid);
    };

    for (auto it = imapRestoreBatch.cbegin(); it != imapRestoreBatch.cend(); ++it) {
        const QStringList parts = it.key().split('|');
        if (parts.size() < 2)
            continue;
        const QString accId = parts.at(0);
        const QString targetFolder = parts.mid(1).join('|');
        for (const AccountItem &acc : m_accounts) {
            if (acc.id == accId) {
                m_googleProvider->moveImapMessagesToFolder(acc, srcFolder, it.value(), targetFolder, resolveFolderName(targetFolder));
                break;
            }
        }
    }

    for (const AccountItem &acc : m_accounts) {
        if (acc.provider == AccountProviderType::Unified)
            continue;
        if (m_selectedAccountId != "acc_unified" && acc.id != m_selectedAccountId)
            continue;
        m_folderTotalExists[acc.id + QStringLiteral("|") + srcFolder] = 0;
    }

    if (unblockedAny) {
        saveAccountsToDisk();
    }

    m_selectedConvId.clear();
    saveConversationsCacheToDisk();
    refreshConversationList();
    setWindowTitle(MailI18n::t(
        QString("SollunaMail — ♻️ '%1'의 메일 %2건을 모두 원래 편지함으로 복원했습니다.").arg(srcTitle).arg(doneCount),
        QString("SollunaMail — ♻️ Restored all %2 message(s) from '%1' to their original folders.").arg(srcTitle).arg(doneCount),
        QString("SollunaMail — ♻️ '%1' のメール %2件をすべて元のフォルダに復元しました。").arg(srcTitle).arg(doneCount)));
}

void MainWindow::onSwitchMailLayoutMode(int mode)
{
    m_mailLayoutMode = qBound(0, mode, 2);
    if (!m_mailSplitLayout || !m_convPaneWidget || !m_readingPaneWidget)
        return;

    if (m_mailLayoutMode == 0) {
        m_mailSplitLayout->setDirection(QBoxLayout::LeftToRight);
        m_convPaneWidget->setMinimumWidth(300);
        m_convPaneWidget->setMaximumWidth(300);
        m_convPaneWidget->setMinimumHeight(0);
        m_convPaneWidget->setMaximumHeight(QWIDGETSIZE_MAX);
        m_convPaneWidget->setVisible(true);
        m_readingPaneWidget->setVisible(true);
    } else if (m_mailLayoutMode == 1) {
        m_mailSplitLayout->setDirection(QBoxLayout::TopToBottom);
        m_convPaneWidget->setMinimumWidth(0);
        m_convPaneWidget->setMaximumWidth(QWIDGETSIZE_MAX);
        m_convPaneWidget->setFixedHeight(280);
        m_convPaneWidget->setVisible(true);
        m_readingPaneWidget->setVisible(true);
    } else if (m_mailLayoutMode == 2) {
        m_mailSplitLayout->setDirection(QBoxLayout::LeftToRight);
        m_convPaneWidget->setMinimumWidth(480);
        m_convPaneWidget->setMaximumWidth(520);
        m_convPaneWidget->setMinimumHeight(0);
        m_convPaneWidget->setMaximumHeight(QWIDGETSIZE_MAX);
        m_convPaneWidget->setVisible(true);
        m_readingPaneWidget->setVisible(true);
    }
}

void MainWindow::onToggleMinimalLayout()
{
    if (!m_folderPaneWidget)
        return;
    m_folderPaneWidget->setVisible(!m_folderPaneWidget->isVisible());
}

void MainWindow::onSwitchCalendarViewMode(int mode)
{
    m_calendarViewMode = qBound(0, mode, 3);
    if (m_fullCalViewStack) {
        m_fullCalViewStack->setCurrentIndex(m_calendarViewMode);
    }
    for (int i = 0; i < m_calViewModeBtns.size(); ++i) {
        if (i == m_calendarViewMode) {
            m_calViewModeBtns[i]->setStyleSheet(
                "background: #FFFFFF; color: #111827; font-size: 11px; font-weight: 600; border: 1px solid #E5E7EB; border-radius: 4px; padding: 4px 12px;");
        } else {
            m_calViewModeBtns[i]->setStyleSheet(
                "background: transparent; color: #6B7280; font-size: 11px; border: none; padding: 4px 12px;");
        }
    }
    refreshCalendarViews();
}

void MainWindow::refreshCalendarViews()
{
    const QStringList korDow = {"", "월", "화", "수", "목", "금", "토", "일"};
    const QStringList engDow = {"", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat", "Sun"};
    const QStringList jpnDow = {"", "月", "火", "水", "木", "金", "土", "日"};

    int dow = m_activeCalendarDate.dayOfWeek();
    int m = m_activeCalendarDate.month();
    int d = m_activeCalendarDate.day();
    int y = m_activeCalendarDate.year();

    if (m_rightCalDateHeaderLbl) {
        QString dateStr = MailI18n::t(
            QString("%1, %2월 %3, %4  ⌄").arg(korDow.value(dow, "화")).arg(m).arg(d).arg(y),
            QString("%1, %2/%3/%4  ⌄").arg(engDow.value(dow, "Tue")).arg(m).arg(d).arg(y),
            QString("%1年 %2月 %3日 (%4)  ⌄").arg(y).arg(m).arg(d).arg(jpnDow.value(dow, "火")));
        m_rightCalDateHeaderLbl->setText(dateStr);
    }

    QSet<QString> disabledAccountEmailsUpper;
    for (const AccountItem &acc : m_accounts) {
        if (acc.provider != AccountProviderType::Unified && !acc.calendarEnabled && !acc.email.trimmed().isEmpty()) {
            disabledAccountEmailsUpper.insert(acc.email.trimmed().toUpper());
        }
    }

    QList<CalendarEventItem> visibleEvents;
    for (const CalendarEventItem &ev : m_calendarEvents) {
        if (disabledAccountEmailsUpper.contains(ev.accountEmail.trimmed().toUpper()))
            continue;
        bool calVisible = true;
        for (const CalendarSubItem &sub : m_calendars) {
            if (sub.id == ev.calendarId && sub.accountEmail.compare(ev.accountEmail, Qt::CaseInsensitive) == 0) {
                calVisible = sub.isVisible;
                break;
            }
        }
        if (calVisible) {
            visibleEvents.append(ev);
        }
    }

    if (m_dayTimelineWidget) {
        m_dayTimelineWidget->setCurrentDate(m_activeCalendarDate);
        m_dayTimelineWidget->setEvents(visibleEvents);
    }
    if (m_fullCalMonthTitleLbl) {
        if (m_calendarViewMode == 2) {
            m_fullCalMonthTitleLbl->setText(
                MailI18n::t(QString("%1월 %2, %3").arg(m).arg(d).arg(y),
                            QString("%1 %2, %3").arg(m_activeCalendarDate.toString("MMM")).arg(d).arg(y),
                            QString("%1年 %2月 %3日").arg(y).arg(m).arg(d)));
        } else {
            m_fullCalMonthTitleLbl->setText(
                MailI18n::t(QString("%1월  %2").arg(m).arg(y),
                            QString("%1 %2").arg(m_activeCalendarDate.toString("MMM")).arg(y),
                            QString("%1年 %2月").arg(y).arg(m)));
        }
    }
    if (m_miniCalendarWidget) {
        m_miniCalendarWidget->setSelectedDate(m_activeCalendarDate);
    }
    if (m_monthGridWidget) {
        m_monthGridWidget->setYearMonth(m_activeCalendarDate.year(), m_activeCalendarDate.month());
        m_monthGridWidget->setSelectedDate(m_activeCalendarDate);
        m_monthGridWidget->setEvents(visibleEvents);
    }
    if (m_weekCalViewWidget) {
        m_weekCalViewWidget->setSelectedDate(m_activeCalendarDate);
        m_weekCalViewWidget->setEvents(visibleEvents);
    }
    if (m_dayCalViewWidget) {
        m_dayCalViewWidget->setSelectedDate(m_activeCalendarDate);
        m_dayCalViewWidget->setEvents(visibleEvents);
    }
    if (m_agendaCalViewWidget) {
        m_agendaCalViewWidget->setSelectedDate(m_activeCalendarDate);
        m_agendaCalViewWidget->setEvents(visibleEvents);
    }
}

void MainWindow::onAddNewCalendarEvent(const QDate &initialDate)
{
    if (m_calendars.isEmpty()) {
        pruneDeletedAccountsData();
        if (m_calendars.isEmpty()) {
            CalendarSubItem fallbackSub;
            fallbackSub.id = "cal_local_default";
            fallbackSub.accountEmail = "SOLLUNAMAIL (LOCAL)";
            fallbackSub.name = "기본 캘린더 (My Calendar)";
            fallbackSub.color = QColor("#0D47A1");
            fallbackSub.isVisible = true;
            fallbackSub.isDefault = true;
            m_calendars.append(fallbackSub);
        }
    }

    QDialog dlg(this);
    dlg.setWindowTitle(MailI18n::t("새 일정 추가 (New Event)", "New Event", "新しい予定を追加"));
    dlg.resize(480, 440);
    dlg.setStyleSheet("QDialog { background: #FFFFFF; } QLineEdit, QDateEdit, QTimeEdit, QComboBox, QTextEdit { border: 1px solid #D1D5DB; border-radius: 5px; padding: 6px; font-size: 12.5px; }");

    QVBoxLayout *v = new QVBoxLayout(&dlg);
    v->setContentsMargins(20, 18, 20, 18);
    v->setSpacing(12);

    QFormLayout *form = new QFormLayout();
    form->setSpacing(10);

    QLineEdit *titleEdit = new QLineEdit(&dlg);
    titleEdit->setPlaceholderText(MailI18n::t("일정 제목 입력 (예: 주간 기술 회의)", "Enter event title", "予定のタイトルを入力"));

    QComboBox *calCombo = new QComboBox(&dlg);
    int defaultIdx = 0;
    for (int i = 0; i < m_calendars.size(); ++i) {
        const CalendarSubItem &sub = m_calendars[i];
        calCombo->addItem(QString("%1 (%2)").arg(sub.name, sub.accountEmail), sub.id);
        if (sub.isDefault && !sub.id.contains("#holiday", Qt::CaseInsensitive))
            defaultIdx = i;
    }
    calCombo->setCurrentIndex(defaultIdx);

    QDateEdit *dateEdit = new QDateEdit(initialDate.isValid() ? initialDate : m_activeCalendarDate, &dlg);
    dateEdit->setCalendarPopup(true);
    dateEdit->setDisplayFormat("yyyy-MM-dd");

    QCheckBox *allDayChk = new QCheckBox(MailI18n::t("하루 종일 (All-day event)", "All-day event", "終日 (All-day)"), &dlg);
    QTime defaultStart(10, 0);
    QTime defaultEnd = defaultStart.addSecs(qBound(15, MailI18n::calendarDefaultDurationMin(), 240) * 60);
    QTimeEdit *startEdit = new QTimeEdit(defaultStart, &dlg);
    QTimeEdit *endEdit = new QTimeEdit(defaultEnd, &dlg);
    connect(allDayChk, &QCheckBox::toggled, &dlg, [startEdit, endEdit](bool checked) {
        startEdit->setEnabled(!checked);
        endEdit->setEnabled(!checked);
    });

    QLineEdit *locEdit = new QLineEdit(&dlg);
    locEdit->setPlaceholderText(MailI18n::t("장소 또는 Google Meet 링크", "Location or Google Meet link", "場所またはGoogle Meetリンク"));

    QTextEdit *descEdit = new QTextEdit(&dlg);
    descEdit->setPlaceholderText(MailI18n::t("상세 내용 / 메모 입력...", "Event description or notes...", "詳細・メモを入力..."));
    descEdit->setFixedHeight(90);

    form->addRow(MailI18n::t("제목:", "Title:", "件名:"), titleEdit);
    form->addRow(MailI18n::t("캘린더:", "Calendar:", "カレンダー:"), calCombo);
    form->addRow(MailI18n::t("날짜:", "Date:", "日付:"), dateEdit);
    form->addRow("", allDayChk);
    form->addRow(MailI18n::t("시작 시간:", "Start time:", "開始時間:"), startEdit);
    form->addRow(MailI18n::t("종료 시간:", "End time:", "終了時間:"), endEdit);
    form->addRow(MailI18n::t("장소:", "Location:", "場所:"), locEdit);
    form->addRow(MailI18n::t("설명/메모:", "Description:", "説明:"), descEdit);
    v->addLayout(form);

    QHBoxLayout *bh = new QHBoxLayout();
    QPushButton *btnSave = new QPushButton(MailI18n::t("💾 일정 추가 (Save)", "💾 Save Event", "💾 予定を保存"), &dlg);
    btnSave->setCursor(Qt::PointingHandCursor);
    btnSave->setStyleSheet("background: #0D47A1; color: #FFFFFF; font-weight: bold; padding: 8px 18px; border-radius: 5px; border: none;");
    QPushButton *btnCancel = new QPushButton(MailI18n::t("취소", "Cancel", "キャンセル"), &dlg);
    btnCancel->setCursor(Qt::PointingHandCursor);
    btnCancel->setStyleSheet("background: #F3F4F6; color: #374151; padding: 8px 16px; border-radius: 5px; border: 1px solid #D1D5DB;");
    connect(btnSave, &QPushButton::clicked, &dlg, [&dlg, titleEdit]() {
        if (titleEdit->text().trimmed().isEmpty()) {
            QMessageBox::warning(&dlg,
                                 MailI18n::t("일정 제목 필요", "Title Required", "件名が必要"),
                                 MailI18n::t("일정 제목을 입력해 주세요.", "Please enter an event title.", "予定の件名を入力してください。"));
            return;
        }
        dlg.accept();
    });
    connect(btnCancel, &QPushButton::clicked, &dlg, &QDialog::reject);
    bh->addStretch();
    bh->addWidget(btnCancel);
    bh->addWidget(btnSave);
    v->addLayout(bh);

    if (dlg.exec() == QDialog::Accepted) {
        CalendarEventItem ev;
        ev.id = "local_ev_" + QUuid::createUuid().toString(QUuid::Id128).left(8);
        ev.calendarId = calCombo->currentData().toString();
        QColor barCol("#0D47A1");
        for (CalendarSubItem &sub : m_calendars) {
            if (sub.id == ev.calendarId) {
                ev.accountEmail = sub.accountEmail;
                sub.isVisible = true;
                if (sub.color.isValid())
                    barCol = sub.color;
                break;
            }
        }
        if (ev.accountEmail.isEmpty() && m_accounts.size() > 1) {
            ev.accountEmail = m_accounts[1].email.toUpper();
        }
        ev.title = titleEdit->text().trimmed();
        ev.date = dateEdit->date();
        ev.isAllDay = allDayChk->isChecked();
        ev.startTime = ev.isAllDay ? QTime(0, 0) : startEdit->time();
        ev.endTime = ev.isAllDay ? QTime(23, 59) : endEdit->time();
        ev.location = locEdit->text().trimmed();
        ev.description = descEdit->toPlainText().trimmed();
        ev.pillBarColor = barCol;
        ev.pillBgColor = barCol.lighter(175);
        ev.pillTextColor = barCol.darker(160);

        m_calendarEvents.append(ev);
        m_activeCalendarDate = ev.date;

        QString targetCalDavHref;
        for (const CalendarSubItem &sub : m_calendars) {
            if (sub.id == ev.calendarId && !sub.calDavHref.trimmed().isEmpty()) {
                targetCalDavHref = sub.calDavHref.trimmed();
                break;
            }
        }
        for (const AccountItem &acc : m_accounts) {
            if (acc.email.compare(ev.accountEmail, Qt::CaseInsensitive) == 0) {
                if (!targetCalDavHref.isEmpty() && !acc.appPassword.trimmed().isEmpty()) {
                    m_googleProvider->createICloudCalDavEvent(acc.email, acc.appPassword, targetCalDavHref, ev);
                } else if (!acc.oauthAccessToken.isEmpty()) {
                    m_googleProvider->createGoogleCalendarEvent(acc.email, acc.oauthAccessToken, ev);
                }
                break;
            }
        }

        saveCalendarAndContactsCacheToDisk();
        rebuildCalendarTree();
        refreshCalendarViews();
    }
}

void MainWindow::onEditOrDeleteCalendarEvent(const CalendarEventItem &evRef)
{
    const CalendarEventItem ev = evRef;
    QDialog dlg(this);
    dlg.setWindowTitle(MailI18n::t("일정 상세 보기 / 수정 / 삭제", "Edit or Delete Event", "予定の詳細・編集・削除"));
    dlg.resize(490, 460);
    dlg.setStyleSheet("QDialog { background: #FFFFFF; } QLineEdit, QDateEdit, QTimeEdit, QComboBox, QTextEdit { border: 1px solid #D1D5DB; border-radius: 5px; padding: 6px; font-size: 12.5px; }");

    QVBoxLayout *v = new QVBoxLayout(&dlg);
    v->setContentsMargins(20, 18, 20, 18);
    v->setSpacing(12);

    QFormLayout *form = new QFormLayout();
    form->setSpacing(10);

    QLineEdit *titleEdit = new QLineEdit(ev.title, &dlg);

    QComboBox *calCombo = new QComboBox(&dlg);
    int selectedCalIdx = 0;
    for (int i = 0; i < m_calendars.size(); ++i) {
        const CalendarSubItem &sub = m_calendars[i];
        calCombo->addItem(QString("%1 (%2)").arg(sub.name, sub.accountEmail), sub.id);
        if (sub.id == ev.calendarId)
            selectedCalIdx = i;
    }
    calCombo->setCurrentIndex(selectedCalIdx);

    QDateEdit *dateEdit = new QDateEdit(ev.date.isValid() ? ev.date : m_activeCalendarDate, &dlg);
    dateEdit->setCalendarPopup(true);
    dateEdit->setDisplayFormat("yyyy-MM-dd");

    QCheckBox *allDayChk = new QCheckBox(MailI18n::t("하루 종일 (All-day event)", "All-day event", "終日 (All-day)"), &dlg);
    allDayChk->setChecked(ev.isAllDay);

    QTimeEdit *startEdit = new QTimeEdit(ev.startTime.isValid() ? ev.startTime : QTime(10, 0), &dlg);
    QTimeEdit *endEdit = new QTimeEdit(ev.endTime.isValid() ? ev.endTime : QTime(11, 0), &dlg);
    startEdit->setEnabled(!ev.isAllDay);
    endEdit->setEnabled(!ev.isAllDay);
    connect(allDayChk, &QCheckBox::toggled, &dlg, [startEdit, endEdit](bool checked) {
        startEdit->setEnabled(!checked);
        endEdit->setEnabled(!checked);
    });

    QLineEdit *locEdit = new QLineEdit(ev.location, &dlg);
    locEdit->setPlaceholderText(MailI18n::t("장소 또는 Google Meet 링크", "Location or Google Meet link", "場所またはGoogle Meetリンク"));

    QTextEdit *descEdit = new QTextEdit(&dlg);
    descEdit->setPlainText(ev.description);
    descEdit->setFixedHeight(90);

    form->addRow(MailI18n::t("제목:", "Title:", "件名:"), titleEdit);
    form->addRow(MailI18n::t("캘린더:", "Calendar:", "カレンダー:"), calCombo);
    form->addRow(MailI18n::t("날짜:", "Date:", "日付:"), dateEdit);
    form->addRow("", allDayChk);
    form->addRow(MailI18n::t("시작 시간:", "Start time:", "開始時間:"), startEdit);
    form->addRow(MailI18n::t("종료 시간:", "End time:", "終了時間:"), endEdit);
    form->addRow(MailI18n::t("장소:", "Location:", "場所:"), locEdit);
    form->addRow(MailI18n::t("설명/메모:", "Description:", "説明:"), descEdit);
    v->addLayout(form);

    int actionCode = 0; // 0: Cancel, 1: Save/Update, 2: Delete
    QHBoxLayout *bh = new QHBoxLayout();
    QPushButton *btnDelete = new QPushButton(MailI18n::t("🗑️ 일정 삭제 (Delete)", "🗑️ Delete Event", "🗑️ 予定を削除"), &dlg);
    btnDelete->setCursor(Qt::PointingHandCursor);
    btnDelete->setStyleSheet("background: #FEF2F2; color: #DC2626; font-weight: bold; padding: 8px 16px; border-radius: 5px; border: 1px solid #FECACA;");

    QPushButton *btnCancel = new QPushButton(MailI18n::t("취소", "Cancel", "キャンセル"), &dlg);
    btnCancel->setCursor(Qt::PointingHandCursor);
    btnCancel->setStyleSheet("background: #F3F4F6; color: #374151; padding: 8px 16px; border-radius: 5px; border: 1px solid #D1D5DB;");

    QPushButton *btnSave = new QPushButton(MailI18n::t("💾 수정 저장 (Save)", "💾 Save Changes", "💾 変更を保存"), &dlg);
    btnSave->setCursor(Qt::PointingHandCursor);
    btnSave->setStyleSheet("background: #0D47A1; color: #FFFFFF; font-weight: bold; padding: 8px 18px; border-radius: 5px; border: none;");

    connect(btnDelete, &QPushButton::clicked, &dlg, [&dlg, &actionCode, ev]() {
        if (QMessageBox::question(&dlg,
                                  MailI18n::t("일정 삭제 확인", "Delete Event", "予定の削除"),
                                  MailI18n::t(QString("'%1' 일정을 삭제하시겠습니까?").arg(ev.title),
                                              QString("Delete event '%1'?").arg(ev.title),
                                              QString("'%1' を削除しますか？").arg(ev.title)),
                                  QMessageBox::Yes | QMessageBox::No) == QMessageBox::Yes) {
            actionCode = 2;
            dlg.accept();
        }
    });
    connect(btnCancel, &QPushButton::clicked, &dlg, &QDialog::reject);
    connect(btnSave, &QPushButton::clicked, &dlg, [&dlg, &actionCode, titleEdit]() {
        if (titleEdit->text().trimmed().isEmpty()) {
            QMessageBox::warning(&dlg,
                                 MailI18n::t("일정 제목 필요", "Title Required", "件名が必要"),
                                 MailI18n::t("일정 제목을 입력해 주세요.", "Please enter an event title.", "予定の件名を入力してください。"));
            return;
        }
        actionCode = 1;
        dlg.accept();
    });

    bh->addWidget(btnDelete);
    bh->addStretch();
    bh->addWidget(btnCancel);
    bh->addWidget(btnSave);
    v->addLayout(bh);

    if (dlg.exec() != QDialog::Accepted || actionCode == 0)
        return;

    if (actionCode == 2) {
        for (int i = m_calendarEvents.size() - 1; i >= 0; --i) {
            if ((!ev.id.isEmpty() && m_calendarEvents[i].id == ev.id)
                || (m_calendarEvents[i].date == ev.date && m_calendarEvents[i].title == ev.title)) {
                m_calendarEvents.removeAt(i);
            }
        }
        QString targetCalDavHref;
        for (const CalendarSubItem &sub : m_calendars) {
            if (sub.id == ev.calendarId && !sub.calDavHref.trimmed().isEmpty()) {
                targetCalDavHref = sub.calDavHref.trimmed();
                break;
            }
        }
        for (const AccountItem &acc : m_accounts) {
            if (acc.email.compare(ev.accountEmail, Qt::CaseInsensitive) == 0) {
                if (!targetCalDavHref.isEmpty() && !acc.appPassword.trimmed().isEmpty()) {
                    m_googleProvider->deleteICloudCalDavEvent(acc.email, acc.appPassword, targetCalDavHref, ev);
                } else if (!acc.oauthAccessToken.isEmpty()) {
                    m_googleProvider->deleteGoogleCalendarEvent(acc.email, acc.oauthAccessToken, ev.calendarId, ev.id);
                }
                break;
            }
        }
        saveCalendarAndContactsCacheToDisk();
        refreshCalendarViews();
        return;
    }

    if (actionCode == 1) {
        CalendarEventItem updated = ev;
        updated.title = titleEdit->text().trimmed();
        updated.calendarId = calCombo->currentData().toString();
        QString targetCalDavHref;
        for (const CalendarSubItem &sub : m_calendars) {
            if (sub.id == updated.calendarId) {
                updated.accountEmail = sub.accountEmail;
                targetCalDavHref = sub.calDavHref.trimmed();
                if (sub.color.isValid()) {
                    updated.pillBarColor = sub.color;
                    updated.pillBgColor = sub.color.lighter(175);
                    updated.pillTextColor = sub.color.darker(160);
                }
                break;
            }
        }
        updated.date = dateEdit->date();
        updated.isAllDay = allDayChk->isChecked();
        updated.startTime = updated.isAllDay ? QTime(0, 0) : startEdit->time();
        updated.endTime = updated.isAllDay ? QTime(23, 59) : endEdit->time();
        updated.location = locEdit->text().trimmed();
        updated.description = descEdit->toPlainText().trimmed();

        bool replaced = false;
        for (int i = 0; i < m_calendarEvents.size(); ++i) {
            if ((!ev.id.isEmpty() && m_calendarEvents[i].id == ev.id && m_calendarEvents[i].date == ev.date)
                || (m_calendarEvents[i].date == ev.date && m_calendarEvents[i].title == ev.title)) {
                m_calendarEvents[i] = updated;
                replaced = true;
                break;
            }
        }
        if (!replaced) {
            m_calendarEvents.append(updated);
        }

        for (const AccountItem &acc : m_accounts) {
            if (acc.email.compare(updated.accountEmail, Qt::CaseInsensitive) == 0) {
                if (!targetCalDavHref.isEmpty() && !acc.appPassword.trimmed().isEmpty()) {
                    m_googleProvider->updateICloudCalDavEvent(acc.email, acc.appPassword, targetCalDavHref, updated);
                } else if (!acc.oauthAccessToken.isEmpty()) {
                    m_googleProvider->updateGoogleCalendarEvent(acc.email, acc.oauthAccessToken, updated);
                }
                break;
            }
        }

        m_activeCalendarDate = updated.date;
        saveCalendarAndContactsCacheToDisk();
        refreshCalendarViews();
    }
}

void MainWindow::onOpenAppleCalendarSyncDialog()
{
    QDialog dlg(this);
    dlg.setWindowTitle(MailI18n::t("🍎 Apple 캘린더 (iCloud CalDAV · webcal:// · .ics) 연동 설정",
                                   "🍎 Apple Calendar (iCloud CalDAV / webcal / .ics) Integration",
                                   "🍎 Appleカレンダー (iCloud CalDAV / webcal / .ics) 連携"));
    dlg.resize(560, 540);
    dlg.setStyleSheet(
        "QDialog { background: #FFFFFF; }"
        "QGroupBox { font-weight: 700; color: #1E293B; border: 1px solid #E2E8F0; border-radius: 8px; margin-top: 8px; padding-top: 12px; }"
        "QGroupBox::title { subcontrol-origin: margin; left: 12px; padding: 0 6px; }"
        "QLineEdit { border: 1px solid #CBD5E1; border-radius: 5px; padding: 6px 10px; font-size: 12.5px; color: #0F172A; background: #FFFFFF; }");

    QVBoxLayout *rootV = new QVBoxLayout(&dlg);
    rootV->setContentsMargins(20, 18, 20, 18);
    rootV->setSpacing(12);

    // 1. iCloud CalDAV 양방향 연동 그룹
    QGroupBox *grpCalDav = new QGroupBox(
        MailI18n::t("① iCloud CalDAV 양방향 동기화 (추천 — 일정 조회 · 추가 · 수정 · 삭제 실시간 반영)",
                    "1. iCloud CalDAV Two-Way Sync (Recommended)",
                    "① iCloud CalDAV 双方向同期 (推奨)"),
        &dlg);
    QVBoxLayout *cdLayout = new QVBoxLayout(grpCalDav);
    cdLayout->setSpacing(8);

    QLabel *cdHelp = new QLabel(
        MailI18n::t("Apple ID 이메일과 16자리 '앱 암호(xxxx-xxxx-xxxx-xxxx)'를 입력하면 iPhone · Mac · iCloud.com의 Apple 캘린더와 실시간 양방향으로 동기화됩니다.\n"
                    "(이미 iCloud 메일 계정에 앱 암호를 등록하셨다면 해당 정보가 자동으로 채워집니다.)",
                    "Enter your Apple ID and 16-character App-Specific Password to enable full two-way sync with iCloud Calendar.",
                    "Apple IDと16桁のApp用パスワードを入力すると、iCloudカレンダーと双方向同期されます。"),
        grpCalDav);
    cdHelp->setWordWrap(true);
    cdHelp->setStyleSheet("color: #475569; font-size: 11.5px; font-weight: normal;");
    cdLayout->addWidget(cdHelp);

    QString defaultAppleEmail;
    QString defaultApplePass;
    for (const AccountItem &acc : m_accounts) {
        if (acc.provider == AccountProviderType::ICloud
            || acc.email.endsWith("@icloud.com", Qt::CaseInsensitive)
            || acc.email.endsWith("@me.com", Qt::CaseInsensitive)
            || acc.email.endsWith("@mac.com", Qt::CaseInsensitive)) {
            defaultAppleEmail = acc.email;
            defaultApplePass = acc.appPassword;
            break;
        }
    }

    QFormLayout *cdForm = new QFormLayout();
    cdForm->setSpacing(8);
    QLineEdit *appleIdEdit = new QLineEdit(defaultAppleEmail, grpCalDav);
    appleIdEdit->setPlaceholderText(
        MailI18n::t("예: user@icloud.com 또는 Apple ID로 사용하는 이메일",
                    "e.g. user@icloud.com or your Apple ID email",
                    "例: user@icloud.com または Apple ID メール"));

    QHBoxLayout *pwRow = new QHBoxLayout();
    QLineEdit *applePassEdit = new QLineEdit(defaultApplePass, grpCalDav);
    applePassEdit->setEchoMode(QLineEdit::Password);
    applePassEdit->setPlaceholderText(
        MailI18n::t("Apple 앱 암호 16자리 (예: abcd-efgh-ijkl-mnop)",
                    "16-digit Apple App Password (e.g. abcd-efgh-ijkl-mnop)",
                    "Apple App用パスワード16桁 (例: abcd-efgh-ijkl-mnop)"));

    QPushButton *btnToggleApplePass = new QPushButton(grpCalDav);
    btnToggleApplePass->setCursor(Qt::PointingHandCursor);
    btnToggleApplePass->setMinimumWidth(80);
    auto updateApplePassBtn = [applePassEdit, btnToggleApplePass]() {
        const bool isPlain = (applePassEdit->echoMode() == QLineEdit::Normal);
        btnToggleApplePass->setIcon(MailIcons::actionIcon(isPlain ? "eye_on" : "eye_off",
                                                          isPlain ? QColor("#0D47A1") : QColor("#4B5563"), 15));
        btnToggleApplePass->setText(isPlain
                                        ? MailI18n::t("숨기기", "Hide", "非表示")
                                        : MailI18n::t("보이기", "Show", "表示"));
        btnToggleApplePass->setStyleSheet(
            isPlain
                ? "QPushButton { background: #EFF6FF; color: #0D47A1; font-size: 11.5px; font-weight: 700; border: 1px solid #93C5FD; border-radius: 5px; padding: 6px 10px; }"
                : "QPushButton { background: #FFFFFF; color: #374151; font-size: 11.5px; font-weight: 600; border: 1px solid #D1D5DB; border-radius: 5px; padding: 6px 10px; }");
    };
    connect(btnToggleApplePass, &QPushButton::clicked, &dlg, [applePassEdit, updateApplePassBtn]() {
        const bool isPlain = (applePassEdit->echoMode() == QLineEdit::Normal);
        applePassEdit->setEchoMode(isPlain ? QLineEdit::Password : QLineEdit::Normal);
        updateApplePassBtn();
    });
    updateApplePassBtn();

    QPushButton *btnOpenAppleIdPage = new QPushButton(
        MailI18n::t("🔑 앱 암호 발급 페이지 열기", "🔑 Get App Password", "🔑 App用パスワード発行"), grpCalDav);
    btnOpenAppleIdPage->setCursor(Qt::PointingHandCursor);
    btnOpenAppleIdPage->setStyleSheet(
        "QPushButton { background: #0284C7; color: #FFFFFF; font-size: 11.5px; font-weight: 600; border-radius: 5px; padding: 6px 10px; border: none; }"
        "QPushButton:hover { background: #0369A1; }");
    connect(btnOpenAppleIdPage, &QPushButton::clicked, &dlg, []() {
        QDesktopServices::openUrl(QUrl("https://appleid.apple.com/account/manage"));
    });
    pwRow->addWidget(applePassEdit, 1);
    pwRow->addWidget(btnToggleApplePass);
    pwRow->addWidget(btnOpenAppleIdPage);

    cdForm->addRow(MailI18n::t("Apple ID 이메일:", "Apple ID Email:", "Apple ID メール:"), appleIdEdit);
    cdForm->addRow(MailI18n::t("앱 암호 (16자리):", "App Password:", "App用パスワード:"), pwRow);
    cdLayout->addLayout(cdForm);

    QPushButton *btnConnectCalDav = new QPushButton(
        MailI18n::t("🔄 지금 Apple iCloud 캘린더 연결 및 양방향 동기화 시작",
                    "🔄 Connect & Sync iCloud Calendar Now",
                    "🔄 今すぐiCloudカレンダーを接続・同期"),
        grpCalDav);
    btnConnectCalDav->setCursor(Qt::PointingHandCursor);
    btnConnectCalDav->setFixedHeight(34);
    btnConnectCalDav->setStyleSheet(
        "QPushButton { background: #0D47A1; color: #FFFFFF; font-size: 12.5px; font-weight: 700; border-radius: 5px; border: none; }"
        "QPushButton:hover { background: #1558BC; }");
    connect(btnConnectCalDav, &QPushButton::clicked, &dlg, [this, &dlg, appleIdEdit, applePassEdit]() {
        const QString email = appleIdEdit->text().trimmed();
        const QString pass = applePassEdit->text().trimmed();
        if (email.isEmpty() || pass.isEmpty()) {
            QMessageBox::warning(
                &dlg,
                MailI18n::t("입력 확인", "Input Required", "入力確認"),
                MailI18n::t("Apple ID 이메일과 16자리 앱 암호를 모두 입력해 주세요.",
                            "Please enter both your Apple ID email and 16-digit App Password.",
                            "Apple IDメールと16桁のApp用パスワードを入力してください。"));
            return;
        }

        bool foundExisting = false;
        for (AccountItem &acc : m_accounts) {
            if (acc.email.compare(email, Qt::CaseInsensitive) == 0) {
                acc.appPassword = pass;
                acc.calendarEnabled = true;
                if (acc.email.endsWith("@icloud.com", Qt::CaseInsensitive)
                    || acc.email.endsWith("@me.com", Qt::CaseInsensitive)
                    || acc.email.endsWith("@mac.com", Qt::CaseInsensitive)) {
                    acc.provider = AccountProviderType::ICloud;
                    if (acc.imapHost.isEmpty())
                        acc.imapHost = QStringLiteral("imap.mail.me.com");
                    if (acc.smtpHost.isEmpty())
                        acc.smtpHost = QStringLiteral("smtp.mail.me.com");
                }
                foundExisting = true;
                break;
            }
        }

        if (!foundExisting) {
            AccountItem icloudAcc;
            icloudAcc.id = "acc_icloud_" + QUuid::createUuid().toString(QUuid::Id128).left(6);
            icloudAcc.email = email;
            icloudAcc.displayName = email.section('@', 0, 0) + " (iCloud)";
            icloudAcc.initials = icloudAcc.displayName.left(1).toUpper();
            icloudAcc.avatarColor = QColor("#0284C7");
            icloudAcc.provider = AccountProviderType::ICloud;
            icloudAcc.visualStyle = AvatarVisualStyle::BlueFolderBadge;
            icloudAcc.imapHost = QStringLiteral("imap.mail.me.com");
            icloudAcc.imapPort = 993;
            icloudAcc.imapSecurity = QStringLiteral("SSL/TLS");
            icloudAcc.smtpHost = QStringLiteral("smtp.mail.me.com");
            icloudAcc.smtpPort = 587;
            icloudAcc.smtpSecurity = QStringLiteral("STARTTLS");
            icloudAcc.appPassword = pass;
            icloudAcc.calendarEnabled = true;
            m_accounts.append(icloudAcc);
            rebuildLeftBarAccounts();
        }

        saveAccountsToDisk();
        rebuildCalendarTree();
        m_googleProvider->syncICloudCalDavCalendars(email, pass);
        QMessageBox::information(
            &dlg,
            MailI18n::t("Apple iCloud CalDAV 동기화 시작", "Apple iCloud CalDAV Sync Started", "Apple iCloud CalDAV 同期開始"),
            MailI18n::t(QString("'%1' 계정의 Apple iCloud 캘린더(caldav.icloud.com)에 연결하여 하위 캘린더와 일정을 동기화합니다.\n"
                                "잠시 후 좌측 캘린더 트리와 달력에 일정이 표시됩니다.").arg(email),
                        QString("Connecting to Apple iCloud Calendar (caldav.icloud.com) for '%1' to sync calendars and events.").arg(email),
                        QString("'%1' の Apple iCloud カレンダー (caldav.icloud.com) に接続し、予定を同期します。").arg(email)));
        dlg.accept();
    });
    cdLayout->addWidget(btnConnectCalDav);
    rootV->addWidget(grpCalDav);

    // 2. 공개 캘린더 URL (webcal://) 구독 그룹
    QGroupBox *grpWebcal = new QGroupBox(
        MailI18n::t("② Apple 공개 캘린더 URL (webcal://) 구독 추가 (읽기 전용 · 앱 암호 불필요)",
                    "2. Subscribe via Public Calendar URL (webcal://)",
                    "② Apple公開カレンダーURL (webcal://) 購読追加"),
        &dlg);
    QVBoxLayout *wcLayout = new QVBoxLayout(grpWebcal);
    wcLayout->setSpacing(8);

    QFormLayout *wcForm = new QFormLayout();
    wcForm->setSpacing(8);
    QLineEdit *wcNameEdit = new QLineEdit(grpWebcal);
    wcNameEdit->setPlaceholderText(MailI18n::t("예: 🍎 아이폰 공유 캘린더", "e.g. 🍎 Shared iPhone Calendar", "例: 🍎 iPhone共有カレンダー"));
    QLineEdit *wcUrlEdit = new QLineEdit(grpWebcal);
    wcUrlEdit->setPlaceholderText(MailI18n::t("예: webcal://pXX-caldav.icloud.com/published/2/...",
                                              "e.g. webcal://pXX-caldav.icloud.com/published/2/...",
                                              "例: webcal://pXX-caldav.icloud.com/published/2/..."));
    wcForm->addRow(MailI18n::t("캘린더 이름:", "Calendar Name:", "カレンダー名:"), wcNameEdit);
    wcForm->addRow(MailI18n::t("공개 URL:", "Public URL:", "公開URL:"), wcUrlEdit);
    wcLayout->addLayout(wcForm);

    QPushButton *btnSubscribeWebcal = new QPushButton(
        MailI18n::t("🌐 공개 캘린더 URL 구독 및 일정 불러오기",
                    "🌐 Subscribe to Public Calendar URL",
                    "🌐 公開カレンダーURLを購読"),
        grpWebcal);
    btnSubscribeWebcal->setCursor(Qt::PointingHandCursor);
    btnSubscribeWebcal->setFixedHeight(32);
    btnSubscribeWebcal->setStyleSheet(
        "QPushButton { background: #E11D48; color: #FFFFFF; font-size: 12px; font-weight: 700; border-radius: 5px; border: none; }"
        "QPushButton:hover { background: #BE123C; }");
    connect(btnSubscribeWebcal, &QPushButton::clicked, &dlg, [this, &dlg, wcNameEdit, wcUrlEdit]() {
        const QString url = wcUrlEdit->text().trimmed();
        if (url.isEmpty()) {
            QMessageBox::warning(
                &dlg,
                MailI18n::t("URL 확인", "URL Required", "URL確認"),
                MailI18n::t("Apple 캘린더의 공개 URL(webcal:// 또는 https://)을 입력해 주세요.",
                            "Please enter a public Apple Calendar URL (webcal:// or https://).",
                            "公開カレンダーURL（webcal:// または https://）を入力してください。"));
            return;
        }
        const QString name = wcNameEdit->text().trimmed().isEmpty()
                                 ? MailI18n::t("🍎 Apple 공개 캘린더", "🍎 Apple Public Calendar", "🍎 Apple公開カレンダー")
                                 : wcNameEdit->text().trimmed();
        const QString ownerEmail = (m_accounts.size() > 1) ? m_accounts[1].email.toUpper() : QStringLiteral("SOLLUNAMAIL (LOCAL)");
        m_googleProvider->fetchWebcalIcsSubscription(ownerEmail, name, url);
        dlg.accept();
    });
    wcLayout->addWidget(btnSubscribeWebcal);
    rootV->addWidget(grpWebcal);

    // 3. 로컬 .ics 파일 가져오기 / 내보내기 그룹
    QGroupBox *grpIcsFile = new QGroupBox(
        MailI18n::t("③ Apple 캘린더 (.ics) 파일 가져오기 (Import) / 내보내기 (Export)",
                    "3. Import or Export .ics Calendar File",
                    "③ .icsファイルのインポート / エクスポート"),
        &dlg);
    QHBoxLayout *icsRow = new QHBoxLayout(grpIcsFile);
    icsRow->setSpacing(10);

    QPushButton *btnImportIcs = new QPushButton(
        MailI18n::t("📂 .ics 파일 불러오기 (Import)", "📂 Import .ics File", "📂 .icsファイルを読み込む"), grpIcsFile);
    btnImportIcs->setCursor(Qt::PointingHandCursor);
    btnImportIcs->setFixedHeight(32);
    btnImportIcs->setStyleSheet(
        "QPushButton { background: #F1F5F9; color: #0F172A; font-size: 12px; font-weight: 600; border-radius: 5px; border: 1px solid #CBD5E1; padding: 4px 12px; }"
        "QPushButton:hover { background: #E2E8F0; }");
    connect(btnImportIcs, &QPushButton::clicked, &dlg, [this, &dlg]() {
        const QString path = QFileDialog::getOpenFileName(
            &dlg,
            MailI18n::t("Apple 캘린더 (.ics) 파일 선택", "Select .ics Calendar File", ".icsファイルを選択"),
            QString(),
            "iCalendar Files (*.ics);;All Files (*.*)");
        if (path.isEmpty())
            return;
        QFile f(path);
        if (!f.open(QIODevice::ReadOnly)) {
            QMessageBox::warning(&dlg,
                                 MailI18n::t("파일 열기 오류", "File Open Error", "ファイルオープンエラー"),
                                 MailI18n::t("선택한 .ics 파일을 열 수 없습니다.", "Could not open the selected .ics file.", "選択した .ics ファイルを開けませんでした。"));
            return;
        }
        const QString icsText = QString::fromUtf8(f.readAll());
        f.close();

        QString targetCalId = "cal_local_default";
        QString targetAccEmail = (m_accounts.size() > 1) ? m_accounts[1].email.toUpper() : QStringLiteral("SOLLUNAMAIL (LOCAL)");
        QColor targetCol("#E11D48");
        if (!m_calendars.isEmpty()) {
            targetCalId = m_calendars.first().id;
            targetAccEmail = m_calendars.first().accountEmail;
            targetCol = m_calendars.first().color;
        }

        const QList<CalendarEventItem> imported =
            GoogleProvider::parseIcsCalendarData(icsText, targetAccEmail, targetCalId, targetCol);
        for (CalendarEventItem ev : imported) {
            if (!ev.id.startsWith("local_ev_"))
                ev.id = "local_ev_" + ev.id;
            m_calendarEvents.append(ev);
        }
        saveCalendarAndContactsCacheToDisk();
        rebuildCalendarTree();
        refreshCalendarViews();
        QMessageBox::information(
            &dlg,
            MailI18n::t(".ics 가져오기 완료", ".ics Import Complete", ".ics インポート完了"),
            MailI18n::t(QString("선택한 .ics 파일에서 총 %1건의 일정을 불러왔습니다.").arg(imported.size()),
                        QString("Imported %1 event(s) from the selected .ics file.").arg(imported.size()),
                        QString("選択した .ics ファイルから %1 件の予定を読み込みました。").arg(imported.size())));
        dlg.accept();
    });

    QPushButton *btnExportIcs = new QPushButton(
        MailI18n::t("💾 내 일정을 .ics로 내보내기 (Export)", "💾 Export Events to .ics", "💾 予定を.icsに書き出す"), grpIcsFile);
    btnExportIcs->setCursor(Qt::PointingHandCursor);
    btnExportIcs->setFixedHeight(32);
    btnExportIcs->setStyleSheet(
        "QPushButton { background: #F1F5F9; color: #0F172A; font-size: 12px; font-weight: 600; border-radius: 5px; border: 1px solid #CBD5E1; padding: 4px 12px; }"
        "QPushButton:hover { background: #E2E8F0; }");
    connect(btnExportIcs, &QPushButton::clicked, &dlg, [this, &dlg]() {
        const QString savePath = QFileDialog::getSaveFileName(
            &dlg,
            MailI18n::t(".ics 파일로 일정 내보내기", "Export Events to .ics", ".icsファイルにエクスポート"),
            "SollunaMail_Calendar.ics",
            "iCalendar Files (*.ics)");
        if (savePath.isEmpty())
            return;

        QFile f(savePath);
        if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            QMessageBox::warning(&dlg,
                                 MailI18n::t("파일 저장 오류", "File Save Error", "ファイル保存エラー"),
                                 MailI18n::t("지정한 경로에 .ics 파일을 저장할 수 없습니다.", "Could not save the .ics file to the selected path.", "指定された場所に .ics ファイルを保存できませんでした。"));
            return;
        }

        QByteArray merged;
        merged += "BEGIN:VCALENDAR\r\nVERSION:2.0\r\nPRODID:-//SollunaMail//Calendar Export//KO\r\nCALSCALE:GREGORIAN\r\n";
        for (const CalendarEventItem &ev : m_calendarEvents) {
            QByteArray single = GoogleProvider::buildIcsFromEvent(ev);
            const int bPos = single.indexOf("BEGIN:VEVENT");
            const int ePos = single.indexOf("END:VEVENT");
            if (bPos >= 0 && ePos > bPos) {
                merged += single.mid(bPos, (ePos - bPos) + 12);
            }
        }
        merged += "END:VCALENDAR\r\n";
        f.write(merged);
        f.close();

        QMessageBox::information(
            &dlg,
            MailI18n::t(".ics 내보내기 완료", ".ics Export Complete", ".ics エクスポート完了"),
            MailI18n::t(QString("총 %1건의 일정을 '%2' 파일로 저장했습니다.\nApple 캘린더(Mac/iPhone)에서 해당 파일을 열면 즉시 등록됩니다.")
                            .arg(m_calendarEvents.size())
                            .arg(savePath),
                        QString("Exported %1 event(s) to '%2'.").arg(m_calendarEvents.size()).arg(savePath),
                        QString("合計 %1 件の予定を '%2' に保存しました。").arg(m_calendarEvents.size()).arg(savePath)));
    });

    icsRow->addWidget(btnImportIcs, 1);
    icsRow->addWidget(btnExportIcs, 1);
    rootV->addWidget(grpIcsFile);

    QHBoxLayout *bottomBar = new QHBoxLayout();
    bottomBar->addStretch();
    QPushButton *btnClose = new QPushButton(MailI18n::t("닫기", "Close", "閉じる"), &dlg);
    btnClose->setCursor(Qt::PointingHandCursor);
    btnClose->setStyleSheet("background: #F3F4F6; color: #374151; padding: 7px 18px; border-radius: 5px; border: 1px solid #D1D5DB;");
    connect(btnClose, &QPushButton::clicked, &dlg, &QDialog::reject);
    bottomBar->addWidget(btnClose);
    rootV->addLayout(bottomBar);

    dlg.exec();
}

namespace {
QStringList splitContactGroupNames(const QString &rawGroupName)
{
    QStringList out;
    const QStringList parts = rawGroupName.split(';', Qt::SkipEmptyParts);
    for (const QString &p : parts) {
        const QString t = p.trimmed();
        if (!t.isEmpty() && !out.contains(t, Qt::CaseInsensitive))
            out.append(t);
    }
    return out;
}

bool contactBelongsToGroup(const ContactItem &c, const QString &targetGroup)
{
    const QString tgt = targetGroup.trimmed();
    if (tgt.isEmpty() || tgt.compare(QStringLiteral("All contacts"), Qt::CaseInsensitive) == 0)
        return true;
    if (c.accountEmail.trimmed().compare(tgt, Qt::CaseInsensitive) == 0)
        return true;
    const QStringList groups = splitContactGroupNames(c.groupName);
    return groups.contains(tgt, Qt::CaseInsensitive);
}

void assignContactToGroup(ContactItem &c, const QString &targetGroup)
{
    const QString tgt = targetGroup.trimmed();
    if (tgt.isEmpty() || tgt.compare(QStringLiteral("All contacts"), Qt::CaseInsensitive) == 0)
        return;
    QStringList groups = splitContactGroupNames(c.groupName);
    if (!groups.contains(tgt, Qt::CaseInsensitive))
        groups.append(tgt);
    c.groupName = groups.join(QStringLiteral("; "));
}

void removeContactFromGroup(ContactItem &c, const QString &targetGroup)
{
    const QString tgt = targetGroup.trimmed();
    if (tgt.isEmpty())
        return;
    QStringList groups = splitContactGroupNames(c.groupName);
    for (int i = groups.size() - 1; i >= 0; --i) {
        if (groups[i].compare(tgt, Qt::CaseInsensitive) == 0)
            groups.removeAt(i);
    }
    if (groups.isEmpty()) {
        c.groupName = c.accountEmail.trimmed().isEmpty()
                          ? QStringLiteral("SOLLUNAMAIL (LOCAL)")
                          : c.accountEmail.trimmed();
    } else {
        c.groupName = groups.join(QStringLiteral("; "));
    }
}

void renameContactGroupInItem(ContactItem &c, const QString &oldGroup, const QString &newGroup)
{
    QStringList groups = splitContactGroupNames(c.groupName);
    bool changed = false;
    for (int i = 0; i < groups.size(); ++i) {
        if (groups[i].compare(oldGroup.trimmed(), Qt::CaseInsensitive) == 0) {
            groups[i] = newGroup.trimmed();
            changed = true;
        }
    }
    if (changed) {
        QStringList dedup;
        for (const QString &g : groups) {
            if (!g.isEmpty() && !dedup.contains(g, Qt::CaseInsensitive))
                dedup.append(g);
        }
        c.groupName = dedup.join(QStringLiteral("; "));
    }
}
} // namespace

void MainWindow::rebuildContactGroupsList()
{
    if (!m_contactGroupList)
        return;

    QString prevSelectedGroup;
    if (QListWidgetItem *cur = m_contactGroupList->currentItem()) {
        prevSelectedGroup = cur->data(Qt::UserRole).toString();
        if (prevSelectedGroup.isEmpty())
            prevSelectedGroup = cur->text();
    }

    QSignalBlocker blocker(m_contactGroupList);
    m_contactGroupList->clear();

    auto addGroupItem = [this](const QString &displayTitle, const QString &rawGroupKey) {
        QListWidgetItem *it = new QListWidgetItem(displayTitle, m_contactGroupList);
        it->setData(Qt::UserRole, rawGroupKey);
    };

    addGroupItem(MailI18n::t("📋 전체 연락처 (All contacts)", "📋 All contacts", "📋 すべての連絡先"),
                 QStringLiteral("All contacts"));
    addGroupItem(QStringLiteral("📁 SOLLUNAMAIL (LOCAL)"),
                 QStringLiteral("SOLLUNAMAIL (LOCAL)"));

    QStringList seenGroups;
    seenGroups << QStringLiteral("ALL CONTACTS") << QStringLiteral("SOLLUNAMAIL (LOCAL)");

    for (const AccountItem &acc : m_accounts) {
        if (acc.provider != AccountProviderType::Unified && !acc.email.trimmed().isEmpty()) {
            const QString upper = acc.email.trimmed().toUpper();
            if (!seenGroups.contains(upper, Qt::CaseInsensitive)) {
                seenGroups.append(upper);
                addGroupItem(QString("📧 %1").arg(upper), upper);
            }
        }
    }

    for (const ContactItem &c : m_contacts) {
        const QStringList gList = splitContactGroupNames(c.groupName);
        for (const QString &g : gList) {
            if (!g.isEmpty() && !seenGroups.contains(g, Qt::CaseInsensitive)
                && !m_customContactGroups.contains(g, Qt::CaseInsensitive)) {
                m_customContactGroups.append(g);
            }
        }
    }

    for (const QString &cg : m_customContactGroups) {
        const QStringList splitCg = splitContactGroupNames(cg);
        for (const QString &singleGrp : splitCg) {
            const QString trimmed = singleGrp.trimmed();
            if (trimmed.isEmpty() || seenGroups.contains(trimmed, Qt::CaseInsensitive))
                continue;
            seenGroups.append(trimmed);
            int memberCnt = 0;
            for (const ContactItem &c : m_contacts) {
                if (contactBelongsToGroup(c, trimmed))
                    ++memberCnt;
            }
            addGroupItem(QString("👥 %1 (%2)").arg(trimmed).arg(memberCnt), trimmed);
        }
    }

    int targetRow = 0;
    if (!prevSelectedGroup.isEmpty()) {
        for (int i = 0; i < m_contactGroupList->count(); ++i) {
            if (m_contactGroupList->item(i)->data(Qt::UserRole).toString().compare(prevSelectedGroup, Qt::CaseInsensitive) == 0) {
                targetRow = i;
                break;
            }
        }
    }
    m_contactGroupList->setCurrentRow(targetRow);
}

void MainWindow::onAddContactGroup()
{
    bool ok = false;
    QString grpName = QInputDialog::getText(
        this,
        MailI18n::t("➕ 새 연락처 그룹 추가 (New Contact Group)", "➕ New Contact Group", "➕ 新しい連絡先グループ追加"),
        MailI18n::t("추가할 그룹 이름을 입력하세요 (예: 개발팀, 고객사, 임원진):",
                    "Enter new contact group name:",
                    "新しいグループ名を入力してください:"),
        QLineEdit::Normal,
        QString(),
        &ok).trimmed();
    if (!ok || grpName.isEmpty())
        return;

    if (m_customContactGroups.contains(grpName, Qt::CaseInsensitive)) {
        QMessageBox::information(
            this,
            MailI18n::t("그룹 중복", "Duplicate Group", "グループ重複"),
            MailI18n::t(QString("'%1' 그룹은 이미 존재합니다.").arg(grpName),
                        QString("Group '%1' already exists.").arg(grpName),
                        QString("'%1' グループは既に存在します。").arg(grpName)));
        return;
    }

    m_customContactGroups.append(grpName);
    saveCalendarAndContactsCacheToDisk();
    rebuildContactGroupsList();
    for (int i = 0; i < m_contactGroupList->count(); ++i) {
        if (m_contactGroupList->item(i)->data(Qt::UserRole).toString().compare(grpName, Qt::CaseInsensitive) == 0) {
            m_contactGroupList->setCurrentRow(i);
            break;
        }
    }
    refreshContactsList();
    statusBar()->showMessage(
        MailI18n::t(QString("👥 새 연락처 그룹 '%1'이(가) 추가되었습니다.").arg(grpName),
                    QString("👥 Added contact group '%1'.").arg(grpName),
                    QString("👥 連絡先グループ '%1' を追加しました。").arg(grpName)),
        4000);
}

void MainWindow::onRenameContactGroup()
{
    if (!m_contactGroupList || !m_contactGroupList->currentItem())
        return;
    const QString oldGroup = m_contactGroupList->currentItem()->data(Qt::UserRole).toString().trimmed();
    if (oldGroup.isEmpty() || oldGroup.compare("All contacts", Qt::CaseInsensitive) == 0
        || oldGroup.compare("SOLLUNAMAIL (LOCAL)", Qt::CaseInsensitive) == 0) {
        QMessageBox::information(
            this,
            MailI18n::t("기본 그룹 안내", "System Group", "基本グループ案内"),
            MailI18n::t("기본 그룹('전체 연락처' / 'LOCAL')의 이름은 변경할 수 없습니다.\n사용자 정의 그룹을 선택한 뒤 변경해 주세요.",
                        "System groups ('All contacts' / 'LOCAL') cannot be renamed.\nPlease select a custom group.",
                        "基本グループ（'すべての連絡先' / 'LOCAL'）の名前は変更できません。"));
        return;
    }
    for (const AccountItem &acc : m_accounts) {
        if (acc.email.trimmed().compare(oldGroup, Qt::CaseInsensitive) == 0) {
            QMessageBox::information(
                this,
                MailI18n::t("계정 그룹 안내", "Account Group", "アカウントグループ案内"),
                MailI18n::t("이메일 계정 기본 그룹의 이름은 변경할 수 없습니다.",
                            "Email account default groups cannot be renamed.",
                            "メールアカウントの基本グループ名は変更できません。"));
            return;
        }
    }

    bool ok = false;
    QString newGroup = QInputDialog::getText(
        this,
        MailI18n::t("✏️ 그룹 이름 변경 (Rename Group)", "✏️ Rename Group", "✏️ グループ名変更"),
        MailI18n::t(QString("'%1' 그룹의 새 이름을 입력하세요:").arg(oldGroup),
                    QString("Enter new name for '%1':").arg(oldGroup),
                    QString("'%1' の新しい名前を入力してください:").arg(oldGroup)),
        QLineEdit::Normal,
        oldGroup,
        &ok).trimmed();
    if (!ok || newGroup.isEmpty() || newGroup.compare(oldGroup, Qt::CaseInsensitive) == 0)
        return;

    for (int i = 0; i < m_customContactGroups.size(); ++i) {
        if (m_customContactGroups[i].compare(oldGroup, Qt::CaseInsensitive) == 0) {
            m_customContactGroups[i] = newGroup;
        }
    }
    if (!m_customContactGroups.contains(newGroup, Qt::CaseInsensitive)) {
        m_customContactGroups.append(newGroup);
    }
    for (ContactItem &c : m_contacts) {
        renameContactGroupInItem(c, oldGroup, newGroup);
    }

    saveCalendarAndContactsCacheToDisk();
    rebuildContactGroupsList();
    refreshContactsList();
    statusBar()->showMessage(
        MailI18n::t(QString("✏️ 그룹 이름이 '%1' → '%2'(으)로 변경되었습니다.").arg(oldGroup, newGroup),
                    QString("✏️ Renamed group '%1' → '%2'.").arg(oldGroup, newGroup),
                    QString("✏️ グループ名を '%1' → '%2' に変更しました。").arg(oldGroup, newGroup)),
        4000);
}

void MainWindow::onDeleteContactGroup()
{
    if (!m_contactGroupList || !m_contactGroupList->currentItem())
        return;
    const QString targetGroup = m_contactGroupList->currentItem()->data(Qt::UserRole).toString().trimmed();
    if (targetGroup.isEmpty() || targetGroup.compare("All contacts", Qt::CaseInsensitive) == 0
        || targetGroup.compare("SOLLUNAMAIL (LOCAL)", Qt::CaseInsensitive) == 0) {
        QMessageBox::information(
            this,
            MailI18n::t("기본 그룹 안내", "System Group", "基本グループ案内"),
            MailI18n::t("기본 그룹('전체 연락처' / 'LOCAL')은 삭제할 수 없습니다.",
                        "System groups ('All contacts' / 'LOCAL') cannot be deleted.",
                        "基本グループ（'すべての連絡先' / 'LOCAL'）は削除できません。"));
        return;
    }
    for (const AccountItem &acc : m_accounts) {
        if (acc.email.trimmed().compare(targetGroup, Qt::CaseInsensitive) == 0) {
            QMessageBox::information(
                this,
                MailI18n::t("계정 그룹 안내", "Account Group", "アカウントグループ案内"),
                MailI18n::t("이메일 계정 기본 그룹은 삭제할 수 없습니다.",
                            "Email account default groups cannot be deleted.",
                            "メールアカウントの基本グループは削除できません。"));
            return;
        }
    }

    if (QMessageBox::question(
            this,
            MailI18n::t("🗑️ 그룹 삭제 확인", "🗑️ Delete Group", "🗑️ グループ削除確認"),
            MailI18n::t(QString("'%1' 그룹을 삭제하시겠습니까?\n(그룹에 속해 있던 연락처들은 삭제되지 않고 'SOLLUNAMAIL (LOCAL)' 그룹으로 안전하게 이동됩니다.)").arg(targetGroup),
                        QString("Delete group '%1'?\n(Contacts in this group will be kept in 'SOLLUNAMAIL (LOCAL)'.)").arg(targetGroup),
                        QString("'%1' グループを削除しますか？").arg(targetGroup)),
            QMessageBox::Yes | QMessageBox::No) != QMessageBox::Yes) {
        return;
    }

    for (int i = m_customContactGroups.size() - 1; i >= 0; --i) {
        if (m_customContactGroups[i].compare(targetGroup, Qt::CaseInsensitive) == 0) {
            m_customContactGroups.removeAt(i);
        }
    }
    for (ContactItem &c : m_contacts) {
        removeContactFromGroup(c, targetGroup);
    }

    saveCalendarAndContactsCacheToDisk();
    rebuildContactGroupsList();
    refreshContactsList();
    statusBar()->showMessage(
        MailI18n::t(QString("🗑️ '%1' 그룹이 삭제되었습니다.").arg(targetGroup),
                    QString("🗑️ Deleted group '%1'.").arg(targetGroup),
                    QString("🗑️ '%1' グループを削除しました。").arg(targetGroup)),
        4000);
}

void MainWindow::onSendMailToSelectedContactGroup()
{
    QString groupFilter = "All contacts";
    if (m_contactGroupList && m_contactGroupList->currentItem()) {
        groupFilter = m_contactGroupList->currentItem()->data(Qt::UserRole).toString();
        if (groupFilter.isEmpty())
            groupFilter = m_contactGroupList->currentItem()->text();
    }

    QStringList recipients;
    QSet<QString> seenEmails;
    for (const ContactItem &c : m_contacts) {
        const QString email = c.email.trimmed();
        if (email.isEmpty() || !email.contains('@') || seenEmails.contains(email.toLower()))
            continue;
        if (!contactBelongsToGroup(c, groupFilter)) {
            continue;
        }
        seenEmails.insert(email.toLower());
        const QString disp = c.displayName.trimmed();
        if (!disp.isEmpty() && disp.compare(email, Qt::CaseInsensitive) != 0) {
            recipients.append(QString("%1 <%2>").arg(disp, email));
        } else {
            recipients.append(email);
        }
    }

    if (recipients.isEmpty()) {
        QMessageBox::information(
            this,
            MailI18n::t("수신자 없음", "No Recipients", "宛先なし"),
            MailI18n::t(QString("선택된 그룹('%1')에 이메일이 등록된 연락처가 없습니다.").arg(groupFilter),
                        QString("No contacts with an email address found in group '%1'.").arg(groupFilter),
                        QString("選択されたグループ（'%1'）にメールアドレスが登録された連絡先がありません。").arg(groupFilter)));
        return;
    }

    onComposeNewMail(recipients.join(", "));
}

void MainWindow::refreshContactsList()
{
    if (!m_contactItemsList)
        return;

    m_contactItemsList->clear();
    QString groupFilter = "All contacts";
    if (m_contactGroupList && m_contactGroupList->currentItem()) {
        groupFilter = m_contactGroupList->currentItem()->data(Qt::UserRole).toString();
        if (groupFilter.isEmpty())
            groupFilter = m_contactGroupList->currentItem()->text();
    }
    if (m_contactsCenterHeaderLbl) {
        m_contactsCenterHeaderLbl->setText(
            groupFilter.compare("All contacts", Qt::CaseInsensitive) == 0
                ? MailI18n::t("전체 연락처", "All contacts", "すべての連絡先")
                : groupFilter);
    }
    QString searchQ = m_contactSearchEdit ? m_contactSearchEdit->text().trimmed() : QString();

    for (int i = 0; i < m_contacts.size(); ++i) {
        const ContactItem &c = m_contacts[i];
        if (!contactBelongsToGroup(c, groupFilter))
            continue;
        if (!searchQ.isEmpty() &&
            !c.displayName.contains(searchQ, Qt::CaseInsensitive) &&
            !c.email.contains(searchQ, Qt::CaseInsensitive) &&
            !c.phone.contains(searchQ, Qt::CaseInsensitive) &&
            !c.company.contains(searchQ, Qt::CaseInsensitive) &&
            !c.groupName.contains(searchQ, Qt::CaseInsensitive))
            continue;

        QPixmap av(64, 64);
        av.setDevicePixelRatio(2.0);
        av.fill(Qt::transparent);
        QPainter p(&av);
        p.setRenderHint(QPainter::Antialiasing, true);
        p.setPen(Qt::NoPen);
        p.setBrush(c.avatarBgColor);
        p.drawEllipse(QRectF(2, 2, 28, 28));
        p.setPen(c.avatarFgColor);
        p.setFont(QFont("Malgun Gothic", 9, QFont::Bold));
        p.drawText(QRectF(2, 2, 28, 28), Qt::AlignCenter, c.initials);
        p.end();

        QString labelText = "  " + c.displayName;
        if (!c.email.isEmpty() && c.displayName != c.email) {
            labelText += QString("  (%1)").arg(c.email);
        } else if (!c.phone.isEmpty() && c.displayName != c.phone) {
            labelText += QString("  (%1)").arg(c.phone);
        }
        const QStringList gNames = splitContactGroupNames(c.groupName);
        QStringList customBadges;
        for (const QString &gn : gNames) {
            if (gn.compare(c.accountEmail, Qt::CaseInsensitive) != 0)
                customBadges.append(gn);
        }
        if (!customBadges.isEmpty()) {
            labelText += QString("  [%1]").arg(customBadges.join(", "));
        }

        QListWidgetItem *item = new QListWidgetItem(QIcon(av), labelText, m_contactItemsList);
        item->setData(Qt::UserRole, i);
        item->setSizeHint(QSize(300, 46));
    }

    if (m_contactItemsList->count() > 0) {
        m_contactItemsList->setCurrentRow(0);
    } else if (m_contactDetailTitleLbl && m_contactDetailBodyLbl) {
        m_contactDetailTitleLbl->setText(MailI18n::t("표시할 연락처가 없습니다", "No contacts found", "連絡先がありません"));
        m_contactDetailBodyLbl->setText("");
    }
}

void MainWindow::onAddExistingContactsToGroup()
{
    onAddNewContact(true);
}

void MainWindow::onAddNewContact(bool startInPickExistingMode)
{
    QDialog dlg(this);
    dlg.setWindowTitle(MailI18n::t("연락처 추가 — 새 연락처 입력 / 기존 주소록에서 선택",
                                   "Add Contact — New or Select from Address Book",
                                   "連絡先を追加 — 新規作成 / 既存の連絡先から選択"));
    dlg.resize(560, 560);
    dlg.setStyleSheet(
        "QDialog { background: #FFFFFF; } "
        "QLineEdit, QComboBox, QTextEdit { border: 1px solid #D1D5DB; border-radius: 5px; padding: 6px; font-size: 12.5px; }");

    QVBoxLayout *v = new QVBoxLayout(&dlg);
    v->setContentsMargins(20, 16, 20, 16);
    v->setSpacing(12);

    QString currentSelectedGrp = QStringLiteral("SOLLUNAMAIL (LOCAL)");
    bool isCustomGroupSelected = false;
    if (m_contactGroupList && m_contactGroupList->currentItem()) {
        const QString g = m_contactGroupList->currentItem()->data(Qt::UserRole).toString().trimmed();
        if (!g.isEmpty() && g.compare(QStringLiteral("All contacts"), Qt::CaseInsensitive) != 0) {
            currentSelectedGrp = g;
            if (g.compare(QStringLiteral("SOLLUNAMAIL (LOCAL)"), Qt::CaseInsensitive) != 0) {
                bool isAccountGrp = false;
                for (const AccountItem &acc : m_accounts) {
                    if (acc.email.trimmed().compare(g, Qt::CaseInsensitive) == 0) {
                        isAccountGrp = true;
                        break;
                    }
                }
                if (!isAccountGrp)
                    isCustomGroupSelected = true;
            }
        }
    }

    auto populateGroupCombo = [this, &currentSelectedGrp](QComboBox *combo) {
        combo->setEditable(true);
        combo->addItem(QStringLiteral("SOLLUNAMAIL (LOCAL)"));
        for (const QString &cg : m_customContactGroups) {
            if (!cg.trimmed().isEmpty() && combo->findText(cg.trimmed(), Qt::MatchFixedString) < 0)
                combo->addItem(cg.trimmed());
        }
        for (const AccountItem &acc : m_accounts) {
            if (acc.provider != AccountProviderType::Unified && !acc.email.isEmpty()) {
                const QString upper = acc.email.toUpper();
                if (combo->findText(upper, Qt::MatchFixedString) < 0)
                    combo->addItem(upper);
            }
        }
        int idx = combo->findText(currentSelectedGrp, Qt::MatchFixedString);
        if (idx >= 0)
            combo->setCurrentIndex(idx);
    };

    QHBoxLayout *modeBar = new QHBoxLayout();
    modeBar->setSpacing(8);
    QPushButton *btnModeNew = new QPushButton(
        MailI18n::t("➕ 새 연락처 직접 입력", "➕ Create New Contact", "➕ 新しい連絡先を入力"), &dlg);
    QPushButton *btnModePick = new QPushButton(
        MailI18n::t("📋 기존 주소록에서 선택 (다중 선택)", "📋 Select from Address Book", "📋 既存の連絡先から選択"), &dlg);
    btnModeNew->setCursor(Qt::PointingHandCursor);
    btnModePick->setCursor(Qt::PointingHandCursor);
    modeBar->addWidget(btnModeNew, 1);
    modeBar->addWidget(btnModePick, 1);
    v->addLayout(modeBar);

    QStackedWidget *modeStack = new QStackedWidget(&dlg);

    QWidget *pageNew = new QWidget(&dlg);
    QVBoxLayout *newV = new QVBoxLayout(pageNew);
    newV->setContentsMargins(0, 4, 0, 0);
    newV->setSpacing(10);

    QFormLayout *form = new QFormLayout();
    form->setSpacing(10);

    QLineEdit *nameEdit = new QLineEdit(pageNew);
    nameEdit->setPlaceholderText(MailI18n::t("이름 (예: 홍길동)", "Name (e.g. John Doe)", "名前 (例: 山田太郎)"));
    QLineEdit *emailEdit = new QLineEdit(pageNew);
    emailEdit->setPlaceholderText(MailI18n::t("이메일 주소 (예: user@company.com)", "Email (e.g. user@company.com)", "メールアドレス (例: user@company.com)"));
    QLineEdit *phoneEdit = new QLineEdit(pageNew);
    phoneEdit->setPlaceholderText(MailI18n::t("전화번호 (예: 010-1234-5678)", "Phone (e.g. +1-555-0100)", "電話番号 (例: 090-1234-5678)"));
    QLineEdit *compEdit = new QLineEdit(pageNew);
    compEdit->setPlaceholderText(MailI18n::t("회사 / 소속", "Company / Organization", "会社・所属"));
    QLineEdit *jobEdit = new QLineEdit(pageNew);
    jobEdit->setPlaceholderText(MailI18n::t("직함 / 부서", "Job Title / Department", "役職・部署"));

    QComboBox *grpCombo = new QComboBox(pageNew);
    populateGroupCombo(grpCombo);

    QTextEdit *noteEdit = new QTextEdit(pageNew);
    noteEdit->setPlaceholderText(MailI18n::t("메모 입력...", "Enter notes...", "メモを入力..."));
    noteEdit->setFixedHeight(75);

    form->addRow(MailI18n::t("소속 그룹 (직접 입력 가능):", "Target Group (editable):", "所属グループ:"), grpCombo);
    form->addRow(MailI18n::t("이름:", "Name:", "名前:"), nameEdit);
    form->addRow(MailI18n::t("이메일:", "Email:", "メール:"), emailEdit);
    form->addRow(MailI18n::t("전화번호:", "Phone:", "電話番号:"), phoneEdit);
    form->addRow(MailI18n::t("회사/소속:", "Company:", "会社・所属:"), compEdit);
    form->addRow(MailI18n::t("직함:", "Job title:", "役職:"), jobEdit);
    form->addRow(MailI18n::t("메모:", "Notes:", "メモ:"), noteEdit);
    newV->addLayout(form);
    newV->addStretch();
    modeStack->addWidget(pageNew);

    QWidget *pagePick = new QWidget(&dlg);
    QVBoxLayout *pickV = new QVBoxLayout(pagePick);
    pickV->setContentsMargins(0, 4, 0, 0);
    pickV->setSpacing(8);

    QHBoxLayout *pickGrpRow = new QHBoxLayout();
    QLabel *pickGrpLbl = new QLabel(MailI18n::t("추가할 대상 그룹:", "Target Group:", "追加先グループ:"), pagePick);
    pickGrpLbl->setStyleSheet("font-size: 12.5px; font-weight: 700; color: #1F2937;");
    QComboBox *pickGrpCombo = new QComboBox(pagePick);
    populateGroupCombo(pickGrpCombo);
    pickGrpRow->addWidget(pickGrpLbl);
    pickGrpRow->addWidget(pickGrpCombo, 1);
    pickV->addLayout(pickGrpRow);

    QHBoxLayout *pickFilterRow = new QHBoxLayout();
    QLineEdit *pickSearchEdit = new QLineEdit(pagePick);
    pickSearchEdit->setPlaceholderText(
        MailI18n::t("🔍 이름, 이메일, 전화번호, 회사명으로 검색...",
                    "🔍 Filter by name, email, phone, or company...",
                    "🔍 名前・メール・電話番号・会社名で検索..."));
    QPushButton *btnCheckAll = new QPushButton(MailI18n::t("전체 선택", "Select All", "すべて選択"), pagePick);
    QPushButton *btnUncheckAll = new QPushButton(MailI18n::t("선택 해제", "Clear", "選択解除"), pagePick);
    btnCheckAll->setCursor(Qt::PointingHandCursor);
    btnUncheckAll->setCursor(Qt::PointingHandCursor);
    const QString miniBtnStyle =
        "QPushButton { background: #F9FAFB; color: #374151; border: 1px solid #D1D5DB; border-radius: 4px; padding: 5px 10px; font-size: 11.5px; font-weight: 600; }"
        "QPushButton:hover { background: #F3F4F6; }";
    btnCheckAll->setStyleSheet(miniBtnStyle);
    btnUncheckAll->setStyleSheet(miniBtnStyle);
    pickFilterRow->addWidget(pickSearchEdit, 1);
    pickFilterRow->addWidget(btnCheckAll);
    pickFilterRow->addWidget(btnUncheckAll);
    pickV->addLayout(pickFilterRow);

    QListWidget *pickList = new QListWidget(pagePick);
    pickList->setIconSize(QSize(20, 20));
    pickList->setSelectionMode(QAbstractItemView::NoSelection);
    pickList->setStyleSheet(
        "QListWidget { background: #FFFFFF; border: 1px solid #D1D5DB; border-radius: 6px; outline: none; }"
        "QListWidget::item { padding: 7px 10px; border-bottom: 1px solid #F3F4F6; font-size: 12.5px; color: #1F2937; }"
        "QListWidget::item:hover { background: #F8FAFC; }");
    pickV->addWidget(pickList, 1);

    auto makePickCheckIcon = [](bool checked) -> QIcon {
        QPixmap px(40, 40);
        px.setDevicePixelRatio(2.0);
        px.fill(Qt::transparent);
        QPainter p(&px);
        p.setRenderHint(QPainter::Antialiasing, true);
        QRectF r(2.0, 2.0, 16.0, 16.0);
        if (checked) {
            p.setPen(QPen(QColor("#0D47A1"), 1.6));
            p.setBrush(QColor("#0D47A1"));
            p.drawRoundedRect(r, 4.0, 4.0);

            QPen checkPen(QColor("#FFFFFF"), 2.3, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
            p.setPen(checkPen);
            QPolygonF tick;
            tick << QPointF(5.5, 10.2) << QPointF(8.8, 13.5) << QPointF(14.6, 6.8);
            p.drawPolyline(tick);
        } else {
            p.setPen(QPen(QColor("#94A3B8"), 1.6));
            p.setBrush(QColor("#FFFFFF"));
            p.drawRoundedRect(r, 4.0, 4.0);
        }
        p.end();
        return QIcon(px);
    };
    const QIcon iconPickUnchecked = makePickCheckIcon(false);
    const QIcon iconPickChecked = makePickCheckIcon(true);

    auto applyPickItemChecked = [iconPickChecked, iconPickUnchecked](QListWidgetItem *it, bool checked) {
        if (!it)
            return;
        it->setData(Qt::UserRole + 1, checked);
        it->setIcon(checked ? iconPickChecked : iconPickUnchecked);
        QFont f = it->font();
        f.setBold(checked);
        it->setFont(f);
        it->setBackground(checked ? QColor("#EFF6FF") : QColor("#FFFFFF"));
        it->setForeground(checked ? QColor("#0D47A1") : QColor("#1F2937"));
    };

    QLabel *pickCountLbl = new QLabel(pagePick);
    pickCountLbl->setStyleSheet("font-size: 12px; color: #0D47A1; font-weight: 600;");
    pickV->addWidget(pickCountLbl);

    auto updatePickCountLabel = [pickList, pickCountLbl, pickGrpCombo]() {
        int checkedCnt = 0;
        for (int i = 0; i < pickList->count(); ++i) {
            if (pickList->item(i)->data(Qt::UserRole + 1).toBool())
                ++checkedCnt;
        }
        const QString grpName = pickGrpCombo->currentText().trimmed();
        pickCountLbl->setText(
            MailI18n::t(QString("✅ 총 %1명 선택됨 → '%2' 그룹에 배정됩니다. (행을 클릭하여 체크/해제)")
                            .arg(checkedCnt)
                            .arg(grpName),
                        QString("✅ %1 contact(s) selected → will be added to '%2'.")
                            .arg(checkedCnt)
                            .arg(grpName),
                        QString("✅ %1 名選択中 → '%2' グループに追加されます。")
                            .arg(checkedCnt)
                            .arg(grpName)));
    };

    auto rebuildPickListItems = [this, pickList, pickGrpCombo, pickSearchEdit, applyPickItemChecked, updatePickCountLabel]() {
        QSet<int> prevCheckedIndices;
        for (int i = 0; i < pickList->count(); ++i) {
            if (pickList->item(i)->data(Qt::UserRole + 1).toBool())
                prevCheckedIndices.insert(pickList->item(i)->data(Qt::UserRole).toInt());
        }

        QSignalBlocker blocker(pickList);
        pickList->clear();
        const QString targetGrp = pickGrpCombo->currentText().trimmed();
        const bool hasPrevChecks = !prevCheckedIndices.isEmpty();

        for (int i = 0; i < m_contacts.size(); ++i) {
            const ContactItem &c = m_contacts[i];
            QString label = c.displayName.trimmed();
            if (!c.email.trimmed().isEmpty() && label.compare(c.email.trimmed(), Qt::CaseInsensitive) != 0)
                label += QString(" <%1>").arg(c.email.trimmed());
            else if (label.isEmpty())
                label = c.email.trimmed();
            if (!c.phone.trimmed().isEmpty())
                label += QString(" · 📞 %1").arg(c.phone.trimmed());
            if (!c.company.trimmed().isEmpty())
                label += QString(" · 🏢 %1").arg(c.company.trimmed());

            const bool alreadyInTarget = (!targetGrp.isEmpty() && contactBelongsToGroup(c, targetGrp));
            if (alreadyInTarget) {
                label += MailI18n::t("  [현재 그룹 소속]", "  [Already in group]", "  [所属済み]");
            } else if (!c.groupName.trimmed().isEmpty()) {
                label += QString("  [%1]").arg(c.groupName.trimmed());
            }

            QListWidgetItem *it = new QListWidgetItem(label, pickList);
            it->setData(Qt::UserRole, i);
            const bool shouldCheck = hasPrevChecks ? prevCheckedIndices.contains(i) : alreadyInTarget;
            applyPickItemChecked(it, shouldCheck);
        }

        const QString q = pickSearchEdit->text().trimmed();
        for (int i = 0; i < pickList->count(); ++i) {
            QListWidgetItem *it = pickList->item(i);
            const bool match = q.isEmpty() || it->text().contains(q, Qt::CaseInsensitive);
            it->setHidden(!match);
        }
        updatePickCountLabel();
    };

    connect(pickList, &QListWidget::itemClicked, &dlg, [applyPickItemChecked, updatePickCountLabel](QListWidgetItem *item) {
        if (!item)
            return;
        const bool nowChecked = !item->data(Qt::UserRole + 1).toBool();
        applyPickItemChecked(item, nowChecked);
        updatePickCountLabel();
    });
    connect(pickSearchEdit, &QLineEdit::textChanged, &dlg, [pickList](const QString &text) {
        const QString q = text.trimmed();
        for (int i = 0; i < pickList->count(); ++i) {
            QListWidgetItem *it = pickList->item(i);
            it->setHidden(!q.isEmpty() && !it->text().contains(q, Qt::CaseInsensitive));
        }
    });
    connect(pickGrpCombo, &QComboBox::currentTextChanged, &dlg, [rebuildPickListItems](const QString &) {
        rebuildPickListItems();
    });
    connect(btnCheckAll, &QPushButton::clicked, &dlg, [pickList, applyPickItemChecked, updatePickCountLabel]() {
        QSignalBlocker blocker(pickList);
        for (int i = 0; i < pickList->count(); ++i) {
            if (!pickList->item(i)->isHidden())
                applyPickItemChecked(pickList->item(i), true);
        }
        updatePickCountLabel();
    });
    connect(btnUncheckAll, &QPushButton::clicked, &dlg, [pickList, applyPickItemChecked, updatePickCountLabel]() {
        QSignalBlocker blocker(pickList);
        for (int i = 0; i < pickList->count(); ++i) {
            if (!pickList->item(i)->isHidden())
                applyPickItemChecked(pickList->item(i), false);
        }
        updatePickCountLabel();
    });

    modeStack->addWidget(pagePick);
    v->addWidget(modeStack, 1);

    QHBoxLayout *bh = new QHBoxLayout();
    QPushButton *btnCancel = new QPushButton(MailI18n::t("취소", "Cancel", "キャンセル"), &dlg);
    btnCancel->setStyleSheet("background: #F3F4F6; color: #374151; padding: 8px 16px; border-radius: 5px; border: 1px solid #D1D5DB;");
    QPushButton *btnSave = new QPushButton(MailI18n::t("💾 연락처 저장 (Save)", "💾 Save Contact", "💾 保存"), &dlg);
    btnSave->setStyleSheet("background: #0D47A1; color: #FFFFFF; font-weight: bold; padding: 8px 18px; border-radius: 5px; border: none;");
    bh->addStretch();
    bh->addWidget(btnCancel);
    bh->addWidget(btnSave);
    v->addLayout(bh);

    auto applyModeStyle = [&](int modeIdx) {
        modeStack->setCurrentIndex(modeIdx);
        const QString activeTabStyle =
            "QPushButton { background: #0D47A1; color: #FFFFFF; border: 1px solid #0D47A1; border-radius: 6px; padding: 8px 12px; font-size: 12.5px; font-weight: 700; }";
        const QString inactiveTabStyle =
            "QPushButton { background: #EFF6FF; color: #0D47A1; border: 1px solid #BFDBFE; border-radius: 6px; padding: 8px 12px; font-size: 12.5px; font-weight: 600; }"
            "QPushButton:hover { background: #DBEAFE; }";
        btnModeNew->setStyleSheet(modeIdx == 0 ? activeTabStyle : inactiveTabStyle);
        btnModePick->setStyleSheet(modeIdx == 1 ? activeTabStyle : inactiveTabStyle);
        if (modeIdx == 0) {
            btnSave->setText(MailI18n::t("💾 새 연락처 저장 (Save)", "💾 Save New Contact", "💾 新しい連絡先を保存"));
        } else {
            btnSave->setText(MailI18n::t("👥 선택한 연락처를 그룹에 추가 (Add to Group)", "👥 Add Selected to Group", "👥 選択した連絡先をグループに追加"));
            rebuildPickListItems();
        }
    };

    connect(btnModeNew, &QPushButton::clicked, &dlg, [&, grpCombo, pickGrpCombo]() {
        if (!pickGrpCombo->currentText().trimmed().isEmpty())
            grpCombo->setCurrentText(pickGrpCombo->currentText().trimmed());
        applyModeStyle(0);
    });
    connect(btnModePick, &QPushButton::clicked, &dlg, [&, grpCombo, pickGrpCombo]() {
        if (!grpCombo->currentText().trimmed().isEmpty())
            pickGrpCombo->setCurrentText(grpCombo->currentText().trimmed());
        applyModeStyle(1);
    });

    rebuildPickListItems();
    applyModeStyle(startInPickExistingMode ? 1 : 0);
    Q_UNUSED(isCustomGroupSelected);

    connect(btnCancel, &QPushButton::clicked, &dlg, &QDialog::reject);
    connect(btnSave, &QPushButton::clicked, &dlg, [&]() {
        if (modeStack->currentIndex() == 0) {
            if (nameEdit->text().trimmed().isEmpty() && emailEdit->text().trimmed().isEmpty() && phoneEdit->text().trimmed().isEmpty()) {
                QMessageBox::warning(
                    &dlg,
                    MailI18n::t("입력 필요", "Input Required", "入力が必要"),
                    MailI18n::t("이름, 이메일 또는 전화번호 중 하나 이상을 입력해 주세요.\n기존 주소록에서 선택하려면 상단의 [📋 기존 주소록에서 선택] 탭을 눌러주세요.",
                                "Please enter at least a name, email, or phone number, or switch to the [Select from Address Book] tab.",
                                "名前、メールアドレス、または電話番号のいずれかを入力してください。"));
                return;
            }
            dlg.accept();
        } else {
            const QString targetGrp = pickGrpCombo->currentText().trimmed();
            if (targetGrp.isEmpty() || targetGrp.compare(QStringLiteral("All contacts"), Qt::CaseInsensitive) == 0) {
                QMessageBox::warning(
                    &dlg,
                    MailI18n::t("그룹 선택 필요", "Group Required", "グループ選択が必要"),
                    MailI18n::t("연락처를 추가할 대상 그룹 이름을 선택하거나 입력해 주세요.",
                                "Please select or enter a target group name.",
                                "追加先のグループ名を選択または入力してください。"));
                return;
            }
            int checkedCount = 0;
            bool hasExistingMembers = false;
            for (const ContactItem &c : m_contacts) {
                if (contactBelongsToGroup(c, targetGrp)) {
                    hasExistingMembers = true;
                    break;
                }
            }
            for (int i = 0; i < pickList->count(); ++i) {
                if (pickList->item(i)->data(Qt::UserRole + 1).toBool())
                    ++checkedCount;
            }
            if (checkedCount == 0 && !hasExistingMembers) {
                QMessageBox::warning(
                    &dlg,
                    MailI18n::t("연락처 선택 필요", "Selection Required", "連絡先の選択が必要"),
                    MailI18n::t("그룹에 추가할 연락처를 목록에서 하나 이상 체크해 주세요.",
                                "Please check at least one contact from the list.",
                                "グループに追加する連絡先を1つ以上チェックしてください。"));
                return;
            }
            dlg.accept();
        }
    });

    if (dlg.exec() != QDialog::Accepted)
        return;

    auto ensureGroupRegistered = [this](const QString &grpName) {
        const QStringList gParts = splitContactGroupNames(grpName);
        for (const QString &part : gParts) {
            const QString singleGrp = part.trimmed();
            if (singleGrp.isEmpty())
                continue;
            bool isSystemGrp = (singleGrp.compare(QStringLiteral("SOLLUNAMAIL (LOCAL)"), Qt::CaseInsensitive) == 0
                                || singleGrp.compare(QStringLiteral("All contacts"), Qt::CaseInsensitive) == 0);
            for (const AccountItem &acc : m_accounts) {
                if (acc.email.trimmed().compare(singleGrp, Qt::CaseInsensitive) == 0) {
                    isSystemGrp = true;
                    break;
                }
            }
            if (!isSystemGrp && !m_customContactGroups.contains(singleGrp, Qt::CaseInsensitive)) {
                m_customContactGroups.append(singleGrp);
            }
        }
    };

    if (modeStack->currentIndex() == 0) {
        const QString chosenGrp = grpCombo->currentText().trimmed().isEmpty()
                                      ? QStringLiteral("SOLLUNAMAIL (LOCAL)")
                                      : grpCombo->currentText().trimmed();
        ensureGroupRegistered(chosenGrp);

        ContactItem c;
        c.id = "local_ct_" + QUuid::createUuid().toString(QUuid::Id128).left(8);
        c.accountEmail = chosenGrp;
        c.groupName = chosenGrp;
        c.displayName = nameEdit->text().trimmed();
        c.email = emailEdit->text().trimmed();
        c.phone = phoneEdit->text().trimmed();
        c.company = compEdit->text().trimmed();
        c.jobTitle = jobEdit->text().trimmed();
        c.notes = noteEdit->toPlainText().trimmed();
        if (c.displayName.isEmpty())
            c.displayName = !c.email.isEmpty() ? c.email : c.phone;
        c.initials = c.displayName.left(1).toUpper();
        c.avatarBgColor = QColor("#DCFCE7");
        c.avatarFgColor = QColor("#15803D");

        m_contacts.prepend(c);
        saveCalendarAndContactsCacheToDisk();
        rebuildContactGroupsList();
        refreshContactsList();
        statusBar()->showMessage(
            MailI18n::t(QString("➕ '%1' 연락처가 '%2' 그룹에 추가되었습니다.").arg(c.displayName, chosenGrp),
                        QString("➕ Added '%1' to group '%2'.").arg(c.displayName, chosenGrp),
                        QString("➕ '%1' をグループ '%2' に追加しました。").arg(c.displayName, chosenGrp)),
            4000);
    } else {
        const QString chosenGrp = pickGrpCombo->currentText().trimmed().isEmpty()
                                      ? QStringLiteral("SOLLUNAMAIL (LOCAL)")
                                      : pickGrpCombo->currentText().trimmed();
        ensureGroupRegistered(chosenGrp);

        int updatedCnt = 0;
        for (int i = 0; i < pickList->count(); ++i) {
            QListWidgetItem *it = pickList->item(i);
            const int contactIdx = it->data(Qt::UserRole).toInt();
            if (contactIdx < 0 || contactIdx >= m_contacts.size())
                continue;
            if (it->data(Qt::UserRole + 1).toBool()) {
                assignContactToGroup(m_contacts[contactIdx], chosenGrp);
                ++updatedCnt;
            } else if (contactBelongsToGroup(m_contacts[contactIdx], chosenGrp)) {
                removeContactFromGroup(m_contacts[contactIdx], chosenGrp);
            }
        }

        saveCalendarAndContactsCacheToDisk();
        rebuildContactGroupsList();
        for (int i = 0; i < m_contactGroupList->count(); ++i) {
            if (m_contactGroupList->item(i)->data(Qt::UserRole).toString().compare(chosenGrp, Qt::CaseInsensitive) == 0) {
                m_contactGroupList->setCurrentRow(i);
                break;
            }
        }
        refreshContactsList();
        statusBar()->showMessage(
            MailI18n::t(QString("👥 선택한 연락처 %1명을 '%2' 그룹에 추가했습니다.").arg(updatedCnt).arg(chosenGrp),
                        QString("👥 Added %1 contact(s) to group '%2'.").arg(updatedCnt).arg(chosenGrp),
                        QString("👥 選択した %1 名の連絡先を '%2' グループに追加しました。").arg(updatedCnt).arg(chosenGrp)),
            4000);
    }
}

void MainWindow::onEditCurrentContact()
{
    if (!m_contactItemsList)
        return;
    int r = m_contactItemsList->currentRow();
    if (r < 0 || !m_contactItemsList->item(r))
        return;
    int realIdx = m_contactItemsList->item(r)->data(Qt::UserRole).toInt();
    if (realIdx < 0 || realIdx >= m_contacts.size())
        return;

    ContactItem target = m_contacts[realIdx];
    const QString origId = target.id;
    const QString origEmail = target.email;

    QDialog dlg(this);
    dlg.setWindowTitle(MailI18n::t("연락처 수정 (Edit Contact)", "Edit Contact", "連絡先を編集"));
    dlg.resize(460, 430);
    dlg.setStyleSheet("QDialog { background: #FFFFFF; } QLineEdit, QComboBox, QTextEdit { border: 1px solid #D1D5DB; border-radius: 5px; padding: 6px; font-size: 12.5px; }");

    QVBoxLayout *v = new QVBoxLayout(&dlg);
    v->setContentsMargins(20, 18, 20, 18);
    v->setSpacing(10);

    QFormLayout *form = new QFormLayout();
    form->setSpacing(10);

    QLineEdit *nameEdit = new QLineEdit(target.displayName, &dlg);
    QLineEdit *emailEdit = new QLineEdit(target.email, &dlg);
    QLineEdit *phoneEdit = new QLineEdit(target.phone, &dlg);
    QLineEdit *compEdit = new QLineEdit(target.company, &dlg);
    QLineEdit *jobEdit = new QLineEdit(target.jobTitle, &dlg);

    QComboBox *grpCombo = new QComboBox(&dlg);
    grpCombo->setEditable(true);
    grpCombo->addItem("SOLLUNAMAIL (LOCAL)");
    for (const QString &cg : m_customContactGroups) {
        if (!cg.trimmed().isEmpty() && grpCombo->findText(cg.trimmed(), Qt::MatchFixedString) < 0)
            grpCombo->addItem(cg.trimmed());
    }
    for (const AccountItem &acc : m_accounts) {
        if (acc.provider != AccountProviderType::Unified && !acc.email.isEmpty()) {
            const QString upper = acc.email.toUpper();
            if (grpCombo->findText(upper, Qt::MatchFixedString) < 0)
                grpCombo->addItem(upper);
        }
    }
    const QString curGrpName = target.groupName.trimmed().isEmpty() ? target.accountEmail.trimmed() : target.groupName.trimmed();
    int gIdx = grpCombo->findText(curGrpName, Qt::MatchFixedString);
    if (gIdx >= 0) {
        grpCombo->setCurrentIndex(gIdx);
    } else if (!curGrpName.isEmpty()) {
        grpCombo->addItem(curGrpName);
        grpCombo->setCurrentIndex(grpCombo->count() - 1);
    }

    QTextEdit *noteEdit = new QTextEdit(&dlg);
    noteEdit->setPlainText(target.notes);
    noteEdit->setFixedHeight(75);

    form->addRow(MailI18n::t("이름:", "Name:", "名前:"), nameEdit);
    form->addRow(MailI18n::t("이메일:", "Email:", "メール:"), emailEdit);
    form->addRow(MailI18n::t("전화번호:", "Phone:", "電話番号:"), phoneEdit);
    form->addRow(MailI18n::t("회사/소속:", "Company:", "会社・所属:"), compEdit);
    form->addRow(MailI18n::t("직함:", "Job title:", "役職:"), jobEdit);
    form->addRow(MailI18n::t("소속 그룹 (직접 입력 가능):", "Group (editable):", "グループ:"), grpCombo);
    form->addRow(MailI18n::t("메모:", "Notes:", "メモ:"), noteEdit);
    v->addLayout(form);

    QHBoxLayout *bh = new QHBoxLayout();
    QPushButton *btnCancel = new QPushButton(MailI18n::t("취소", "Cancel", "キャンセル"), &dlg);
    btnCancel->setStyleSheet("background: #F3F4F6; color: #374151; padding: 8px 16px; border-radius: 5px; border: 1px solid #D1D5DB;");
    QPushButton *btnSave = new QPushButton(MailI18n::t("💾 수정 저장 (Save)", "💾 Save Changes", "💾 変更を保存"), &dlg);
    btnSave->setStyleSheet("background: #0D47A1; color: #FFFFFF; font-weight: bold; padding: 8px 18px; border-radius: 5px; border: none;");
    connect(btnCancel, &QPushButton::clicked, &dlg, &QDialog::reject);
    connect(btnSave, &QPushButton::clicked, &dlg, &QDialog::accept);
    bh->addStretch();
    bh->addWidget(btnCancel);
    bh->addWidget(btnSave);
    v->addLayout(bh);

    if (dlg.exec() == QDialog::Accepted) {
        const QString chosenGrp = grpCombo->currentText().trimmed().isEmpty()
                                      ? QStringLiteral("SOLLUNAMAIL (LOCAL)")
                                      : grpCombo->currentText().trimmed();
        const QStringList chosenGrpParts = splitContactGroupNames(chosenGrp);
        for (const QString &part : chosenGrpParts) {
            const QString singleGrp = part.trimmed();
            if (singleGrp.isEmpty())
                continue;
            bool isSystemGrp = (singleGrp.compare("SOLLUNAMAIL (LOCAL)", Qt::CaseInsensitive) == 0
                                || singleGrp.compare("All contacts", Qt::CaseInsensitive) == 0);
            for (const AccountItem &acc : m_accounts) {
                if (acc.email.trimmed().compare(singleGrp, Qt::CaseInsensitive) == 0) {
                    isSystemGrp = true;
                    break;
                }
            }
            if (!isSystemGrp && !m_customContactGroups.contains(singleGrp, Qt::CaseInsensitive)) {
                m_customContactGroups.append(singleGrp);
            }
        }

        target.displayName = nameEdit->text().trimmed();
        target.email = emailEdit->text().trimmed();
        target.phone = phoneEdit->text().trimmed();
        target.company = compEdit->text().trimmed();
        target.jobTitle = jobEdit->text().trimmed();
        target.groupName = chosenGrpParts.isEmpty() ? QStringLiteral("SOLLUNAMAIL (LOCAL)") : chosenGrpParts.join(QStringLiteral("; "));
        if (target.accountEmail.trimmed().isEmpty())
            target.accountEmail = chosenGrpParts.isEmpty() ? QStringLiteral("SOLLUNAMAIL (LOCAL)") : chosenGrpParts.first();
        target.notes = noteEdit->toPlainText().trimmed();
        if (target.displayName.isEmpty())
            target.displayName = !target.email.isEmpty() ? target.email : target.phone;
        target.initials = target.displayName.left(1).toUpper();
        if (!target.id.startsWith("local_ct_"))
            target.id = "local_ct_" + target.id;

        int foundIdx = -1;
        if (realIdx >= 0 && realIdx < m_contacts.size()
            && (m_contacts[realIdx].id == origId || m_contacts[realIdx].id == target.id)) {
            foundIdx = realIdx;
        } else {
            for (int i = 0; i < m_contacts.size(); ++i) {
                if ((!origId.isEmpty() && m_contacts[i].id == origId)
                    || m_contacts[i].id == target.id
                    || (!origEmail.isEmpty() && m_contacts[i].email.compare(origEmail, Qt::CaseInsensitive) == 0)) {
                    foundIdx = i;
                    break;
                }
            }
        }
        if (foundIdx >= 0 && foundIdx < m_contacts.size()) {
            m_contacts[foundIdx] = target;
        } else {
            m_contacts.append(target);
        }
        saveCalendarAndContactsCacheToDisk();
        rebuildContactGroupsList();
        refreshContactsList();
    }
}

void MainWindow::onDeleteCurrentContact()
{
    if (!m_contactItemsList)
        return;
    int r = m_contactItemsList->currentRow();
    if (r < 0 || !m_contactItemsList->item(r))
        return;
    int realIdx = m_contactItemsList->item(r)->data(Qt::UserRole).toInt();
    if (realIdx < 0 || realIdx >= m_contacts.size())
        return;

    const QString targetId = m_contacts[realIdx].id;
    const QString targetEmail = m_contacts[realIdx].email;
    const QString dispName = m_contacts[realIdx].displayName;
    if (QMessageBox::question(this,
                              MailI18n::t("연락처 삭제 확인", "Delete Contact", "連絡先の削除"),
                              MailI18n::t(QString("'%1' 연락처를 삭제하시겠습니까?").arg(dispName),
                                          QString("Delete contact '%1'?").arg(dispName),
                                          QString("'%1' を削除しますか？").arg(dispName)),
                              QMessageBox::Yes | QMessageBox::No) == QMessageBox::Yes) {
        int foundIdx = -1;
        if (realIdx >= 0 && realIdx < m_contacts.size() && m_contacts[realIdx].id == targetId) {
            foundIdx = realIdx;
        } else {
            for (int i = 0; i < m_contacts.size(); ++i) {
                if ((!targetId.isEmpty() && m_contacts[i].id == targetId)
                    || (!targetEmail.isEmpty() && m_contacts[i].email.compare(targetEmail, Qt::CaseInsensitive) == 0)) {
                    foundIdx = i;
                    break;
                }
            }
        }
        if (foundIdx >= 0 && foundIdx < m_contacts.size()) {
            m_contacts.removeAt(foundIdx);
            saveCalendarAndContactsCacheToDisk();
            refreshContactsList();
        }
    }
}

void MainWindow::onOpenSettingsModal(int initialTabIndex)
{
    SettingsDialog dlg(&m_accounts, &m_folders, &m_privacySettings, m_currentThemeName, m_googleProvider, initialTabIndex, this);
    connect(&dlg, &SettingsDialog::themeChanged, this, &MainWindow::onApplyTheme);
    connect(&dlg, &SettingsDialog::languageChanged, this, &MainWindow::onChangeLanguage);
    connect(&dlg, &SettingsDialog::autoSyncIntervalChanged, this, [this](int) {
        applyAutoSyncInterval();
        saveAccountsToDisk();
    });
    connect(&dlg, &SettingsDialog::privacySettingsChanged, this, [this]() {
        MailI18n::privacyTrackSentEmails() = (m_privacySettings.privacyMode && m_privacySettings.trackSentEmails);
        MailI18n::privacyDownloadAvatars() = m_privacySettings.downloadAvatars;
        if (m_privacySettings.privacyMode && m_privacySettings.blockSenders && !m_privacySettings.blockedSendersList.isEmpty()) {
            applyFilterRulesToConversations();
            saveConversationsCacheToDisk();
        }
        m_lastBadgeUnreadCount = -1;
        updateAppUnreadBadgeIcon();
        saveAccountsToDisk();
        const int curRow = m_convListWidget ? m_convListWidget->currentRow() : 0;
        if (curRow >= 0 && curRow < m_filteredConvIndices.size()) {
            displayConversation(curRow, false);
        }
    });
    connect(&dlg, &SettingsDialog::senderUnblocked, this, [this](const QString &unblockedEmail) {
        const QString targetSender = unblockedEmail.trimmed().toLower();
        if (targetSender.isEmpty())
            return;

        // 차단 해제된 발신자와 일치하는 활성 스팸 이동 필터 규칙도 함께 정리하여 재동기화 시 다시 스팸으로 분류되지 않도록 보장
        unblockSenderFromSpamLists(m_privacySettings, targetSender);

        int restoredCount = 0;
        QHash<QString, QList<int>> imapRestoreBatch;
        for (ConversationItem &c : m_conversations) {
            if (c.folderId != QStringLiteral("spam") || c.messages.isEmpty())
                continue;
            if (isSenderMatchedByBlockedRule(c.latestMessage().senderEmail, targetSender)) {
                QString targetFolder = c.previousFolderId.trimmed();
                if (targetFolder.isEmpty() || targetFolder == QStringLiteral("trash") || targetFolder == QStringLiteral("spam")) {
                    targetFolder = QStringLiteral("inbox");
                }
                const QString oldId = c.id;
                const QString sigKey = conversationSignatureKey(c);
                m_checkedConvIds.remove(oldId);
                m_deletedConvIds.remove(oldId);
                m_deletedConvIds.remove(QStringLiteral("trash|") + sigKey);
                m_deletedConvIds.remove(QStringLiteral("perm|") + sigKey);

                const int removedSeq = c.imapSeq;
                if (removedSeq > 0) {
                    const QString fk = c.accountId + QStringLiteral("|spam");
                    if (m_folderTotalExists.value(fk, 0) > 0)
                        m_folderTotalExists[fk] = m_folderTotalExists.value(fk) - 1;
                    for (ConversationItem &other : m_conversations) {
                        if (other.accountId == c.accountId && other.folderId == QStringLiteral("spam") && other.imapSeq > removedSeq)
                            other.imapSeq -= 1;
                    }
                }

                QString parsedSrcFolder;
                int seqNum = 0;
                const bool hasValidSeq = parseImapConversationId(c.id, parsedSrcFolder, seqNum) && seqNum > 0;
                c.folderId = targetFolder;
                c.folderTag = QStringLiteral("Inbox");
                c.previousFolderId.clear();
                c.imapSeq = 0;
                if (hasValidSeq) {
                    imapRestoreBatch[c.accountId + QStringLiteral("|") + targetFolder].append(seqNum);
                    c.id = QString("imap_%1_%2_%3").arg(c.accountId, targetFolder).arg(seqNum);
                    m_checkedConvIds.remove(c.id);
                } else {
                    for (const AccountItem &acc : m_accounts) {
                        if (acc.id == c.accountId) {
                            const MessageItem &msg = c.latestMessage();
                            m_googleProvider->appendMessageToImapFolder(acc,
                                                                        targetFolder,
                                                                        msg.recipientSummary,
                                                                        c.ccSummary,
                                                                        c.subject,
                                                                        msg.htmlBody,
                                                                        c.bccSummary);
                            break;
                        }
                    }
                }
                ++restoredCount;
            }
        }

        auto resolveFolderName = [this](const QString &fid) -> QString {
            for (const FolderItem &f : m_folders) {
                if (f.id == fid)
                    return f.name.trimmed().isEmpty() ? fid : f.name.trimmed();
            }
            return MailI18n::folderTitle(fid, fid);
        };
        for (auto it = imapRestoreBatch.cbegin(); it != imapRestoreBatch.cend(); ++it) {
            const QStringList parts = it.key().split('|');
            if (parts.size() < 2)
                continue;
            const QString accId = parts.at(0);
            const QString targetFolder = parts.mid(1).join('|');
            for (const AccountItem &acc : m_accounts) {
                if (acc.id == accId) {
                    m_googleProvider->moveImapMessagesToFolder(acc, QStringLiteral("spam"), it.value(), targetFolder, resolveFolderName(targetFolder));
                    break;
                }
            }
        }

        saveAccountsToDisk();
        if (restoredCount > 0) {
            saveConversationsCacheToDisk();
            refreshFolderSelectionStyles();
            refreshConversationList();
            setWindowTitle(MailI18n::t(
                QString("SollunaMail — ♻️ '%1' 차단 해제 및 스팸 메일 %2건 받은편지함 복원 완료").arg(unblockedEmail).arg(restoredCount),
                QString("SollunaMail — ♻️ Unblocked '%1' and restored %2 spam message(s) to Inbox").arg(unblockedEmail).arg(restoredCount),
                QString("SollunaMail — ♻️ '%1' のブロック解除および迷惑メール %2件を受信トレイに復元完了").arg(unblockedEmail).arg(restoredCount)));
        }
    });
    connect(&dlg, &SettingsDialog::mailLayoutChanged, this, [this](int mode, bool menuExpanded) {
        onSwitchMailLayoutMode(mode);
        if (m_folderPaneWidget)
            m_folderPaneWidget->setVisible(menuExpanded);
        saveAccountsToDisk();
    });
    connect(&dlg, &SettingsDialog::zoomLevelChanged, this, [this](int zoomPercent) {
        if (m_readingBodyBrowser) {
            QFont f = m_readingBodyBrowser->font();
            f.setPointSizeF(10.0 * (zoomPercent / 100.0));
            m_readingBodyBrowser->setFont(f);
        }
        saveAccountsToDisk();
    });
    connect(&dlg, &SettingsDialog::foldersUpdated, this, [this]() {
        rebuildCustomFoldersList();
        refreshFolderSelectionStyles();
        saveAccountsToDisk();
    });
    connect(&dlg, &SettingsDialog::runFilterRulesRequested, this, [this, &dlg]() {
        int matched = applyFilterRulesToConversations();
        saveConversationsCacheToDisk();
        refreshConversationList();
        saveAccountsToDisk();
        QMessageBox::information(&dlg,
                                 MailI18n::t("필터 규칙 실행 완료", "Filters Applied", "フィルター実行完了"),
                                 MailI18n::t(QString("활성 필터 규칙을 전체 메일에 적용하여 총 %1건의 메일을 분류·처리했습니다.").arg(matched),
                                             QString("Applied active filter rules to %1 messages.").arg(matched),
                                             QString("有効なフィルター規則を適用し、%1件のメールを処理しました。").arg(matched)));
    });
    connect(&dlg, &SettingsDialog::toggleRightCalendarRequested, this, [this](bool visible) {
        if (m_rightCalendarSidebar)
            m_rightCalendarSidebar->setVisible(visible);
        saveAccountsToDisk();
    });
    connect(&dlg, &SettingsDialog::calendarSettingsChanged, this, [this]() {
        rebuildCalendarTree();
        refreshCalendarViews();
        saveAccountsToDisk();
    });
    connect(&dlg, &SettingsDialog::clearMailCacheRequested, this, [this]() {
        const QString cacheDir = ensureAppCacheDir();
        QFile::remove(cacheDir + QStringLiteral("/conversations_cache.json"));
        QFile::remove(cacheDir + QStringLiteral("/deleted_mails_cache.json"));
        QFile::remove(QCoreApplication::applicationDirPath() + QStringLiteral("/conversations_cache.json"));
        QFile::remove(QCoreApplication::applicationDirPath() + QStringLiteral("/deleted_mails_cache.json"));
        QDir(cacheDir + QStringLiteral("/attachments")).removeRecursively();
        m_deletedConvIds.clear();
        m_folderLowestSeq.clear();
        m_folderTotalExists.clear();
        m_folderLoadingMore.clear();
        if (m_googleProvider) {
            for (const AccountItem &acc : m_accounts) {
                if (acc.provider != AccountProviderType::Unified)
                    m_googleProvider->clearLoadedImapUids(acc.id);
            }
        }
        for (int i = m_conversations.size() - 1; i >= 0; --i) {
            if (m_conversations[i].id.startsWith("imap_"))
                m_conversations.removeAt(i);
        }
        refreshConversationList();
        onRefreshAndSyncCurrentAccount();
    });
    connect(&dlg, &SettingsDialog::accountsUpdated, this, [this]() {
        pruneDeletedAccountsData();
        rebuildLeftBarAccounts();
        rebuildCalendarTree();
        refreshConversationList();
        refreshCalendarViews();
        refreshContactsList();
        saveAccountsToDisk();
    });
    connect(&dlg, &SettingsDialog::requestImapSync, this, [this](const QString &accId) {
        for (const AccountItem &acc : m_accounts) {
            if (acc.id == accId) {
                m_googleProvider->fetchAccountAvatar(acc);
                const QString targetFolder = (m_selectedFolderId == QStringLiteral("snoozed") || m_selectedFolderId.isEmpty())
                                                 ? QStringLiteral("inbox")
                                                 : m_selectedFolderId;
                m_googleProvider->fetchImapFolder(acc, targetFolder, 20, 0);
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
                break;
            }
        }
    });
    dlg.exec();
    pruneDeletedAccountsData();
    rebuildLeftBarAccounts();
    rebuildCustomFoldersList();
    rebuildCalendarTree();
    refreshConversationList();
    refreshCalendarViews();
    refreshContactsList();
    saveAccountsToDisk();
    for (const AccountItem &acc : m_accounts) {
        if (acc.provider != AccountProviderType::Unified && acc.avatarPngBase64.isEmpty()) {
            m_googleProvider->fetchAccountAvatar(acc);
        }
    }
}

void MainWindow::onChangeLanguage(AppLanguage lang)
{
    MailI18n::setLanguage(lang);
    retranslateUi();
    saveAccountsToDisk();
}

void MainWindow::retranslateUi()
{
    MailI18n::applyLanguageFont();
    setStyleSheet(MailThemeManager::buildGlobalStyleSheet(m_palette));
    setupMainMenuBar();
    setWindowTitle(QString("SollunaMail — %1").arg(MailI18n::folderTitle(m_selectedFolderId, m_selectedFolderTitle)));

    for (const FolderItem &f : m_folders) {
        if (!f.isCustom && m_folderButtons.contains(f.id)) {
            m_folderButtons[f.id]->setText(MailI18n::folderTitle(f.id, f.name));
        }
    }
    rebuildCustomFoldersList();
    if (m_yourFoldersHeaderLbl) {
        m_yourFoldersHeaderLbl->setText(MailI18n::t("내 폴더", "YOUR FOLDERS", "マイフォルダ"));
    }
    if (m_manageFoldersBtn) {
        m_manageFoldersBtn->setText(MailI18n::t("  폴더 관리", "  Manage folders", "  フォルダ管理"));
    }
    if (m_convPaneTitleLbl) {
        m_convPaneTitleLbl->setText(MailI18n::folderTitle(m_selectedFolderId, m_selectedFolderTitle));
    }
    if (m_mailSearchEdit) {
        m_mailSearchEdit->setPlaceholderText(MailI18n::t("🔍 제목, 보낸사람, 본문 검색...", "🔍 Search subject, sender, body...", "🔍 件名、差出人、本文を検索..."));
    }
    if (m_unsubscribeBtn) {
        m_unsubscribeBtn->setText(MailI18n::t("구독 취소", "Unsubscribe", "配信停止"));
    }
    if (m_blockSenderBtn) {
        m_blockSenderBtn->setText(MailI18n::t("발신자 차단", "Block sender", "送信者をブロック"));
    }
    if (m_readingLargeViewBtn) {
        m_readingLargeViewBtn->setText(MailI18n::t("🗖 크게 보기", "🗖 Large View", "🗖 大きく表示"));
    }
    if (m_rightCalTodayBtn) {
        m_rightCalTodayBtn->setText(MailI18n::t("오늘", "Today", "今日"));
    }
    if (m_rightCalAllDayLbl) {
        m_rightCalAllDayLbl->setText(MailI18n::t("종일", "All day", "終日"));
    }
    if (m_fullCalTodayBtn) {
        m_fullCalTodayBtn->setText(MailI18n::t("오늘", "Today", "今日"));
    }
    const QStringList calModeLabels = {
        MailI18n::t("월간", "Month", "月"),
        MailI18n::t("주간", "Week", "週"),
        MailI18n::t("일간", "Day", "日"),
        MailI18n::t("일정목록", "Agenda", "予定リスト")
    };
    for (int i = 0; i < m_calViewModeBtns.size() && i < calModeLabels.size(); ++i) {
        if (m_calViewModeBtns[i]) {
            m_calViewModeBtns[i]->setText(calModeLabels[i]);
        }
    }
    if (m_fullCalNewEventBtn) {
        m_fullCalNewEventBtn->setText(MailI18n::t("새 일정 추가", "New event", "予定を追加"));
    }
    if (m_fullCalAppleSyncBtn) {
        m_fullCalAppleSyncBtn->setText(MailI18n::t("🍎 Apple 캘린더 연동", "🍎 Apple Calendar Sync", "🍎 Appleカレンダー連携"));
    }
    if (m_calSearchEdit) {
        m_calSearchEdit->setPlaceholderText(MailI18n::t("🔍 일정 검색", "🔍 Search events", "🔍 予定を検索"));
    }
    if (m_contactsLeftHeaderLbl) {
        m_contactsLeftHeaderLbl->setText(MailI18n::t("연락처 그룹", "Contact Groups", "連絡先グループ"));
    }
    if (QPushButton *b = findChild<QPushButton *>(QStringLiteral("contactNewGroupBtn"))) {
        b->setText(MailI18n::t("➕ 그룹 추가", "➕ New Group", "➕ グループ追加"));
        b->setToolTip(MailI18n::t("새 연락처 그룹을 만듭니다", "Create a new contact group", "新しい連絡先グループを作成"));
    }
    if (QPushButton *b = findChild<QPushButton *>(QStringLiteral("contactRenameGroupBtn"))) {
        b->setText(MailI18n::t("✏️ 이름변경", "✏️ Rename", "✏️ 名前変更"));
    }
    if (QPushButton *b = findChild<QPushButton *>(QStringLiteral("contactDeleteGroupBtn"))) {
        b->setText(MailI18n::t("🗑️ 그룹삭제", "🗑️ Delete", "🗑️ 削除"));
    }
    if (QPushButton *b = findChild<QPushButton *>(QStringLiteral("contactAddFromAddrBookBtn"))) {
        b->setText(MailI18n::t("📋 주소록에서 그룹에 연락처 추가", "📋 Add from Address Book", "📋 アドレス帳からグループに追加"));
    }
    if (QPushButton *b = findChild<QPushButton *>(QStringLiteral("contactMailToGroupBtn"))) {
        b->setText(MailI18n::t("✉️ 선택 그룹 전체에 메일 발송", "✉️ Email Selected Group", "✉️ 選択グループ全員に送信"));
    }
    if (m_contactsCenterHeaderLbl) {
        m_contactsCenterHeaderLbl->setText(MailI18n::t("전체 연락처", "All contacts", "すべての連絡先"));
    }
    if (m_contactSearchEdit) {
        m_contactSearchEdit->setPlaceholderText(MailI18n::t("🔍 연락처 검색", "🔍 Search contact", "🔍 連絡先を検索"));
    }
    if (m_contactMailToBtn) {
        m_contactMailToBtn->setText(MailI18n::t("✉️ 이 연락처로 메일 보내기", "✉️ Send Email to Contact", "✉️ この連絡先にメールを送信"));
    }
    if (m_contactEditBtn) {
        m_contactEditBtn->setText(MailI18n::t("✏️ 연락처 수정 (Edit)", "✏️ Edit Contact", "✏️ 編集"));
    }
    if (m_contactDeleteBtn) {
        m_contactDeleteBtn->setText(MailI18n::t("🗑️ 연락처 삭제 (Delete)", "🗑️ Delete Contact", "🗑️ 削除"));
    }
    if (m_restoreSelectedTrashBtn) {
        m_restoreSelectedTrashBtn->setText(MailI18n::t("♻️ 선택 복원", "♻️ Restore", "♻️ 選択復元"));
        m_restoreSelectedTrashBtn->setToolTip(
            MailI18n::t("현재 선택한 메일을 원래 편지함(받은편지함/보낸편지함 등)으로 복원합니다",
                        "Restore the selected message to its original folder",
                        "選択したメールを元のフォルダに復元します"));
    }
    if (m_restoreAllTrashBtn) {
        m_restoreAllTrashBtn->setText(MailI18n::t("♻️ 전체 복원", "♻️ Restore All", "♻️ すべて復元"));
        m_restoreAllTrashBtn->setToolTip(
            MailI18n::t("휴지통의 모든 메일을 원래 편지함으로 일괄 복원합니다",
                        "Restore all messages in Trash to their original folders",
                        "ゴミ箱のすべてのメールを元のフォルダに一括復元します"));
    }
    if (m_emptyTrashBannerBtn) {
        m_emptyTrashBannerBtn->setText(MailI18n::t("🗑️ 비우기(영구삭제)", "🗑️ Empty", "🗑️ 空にする"));
        m_emptyTrashBannerBtn->setToolTip(
            MailI18n::t("휴지통/스팸함의 메일을 영구 삭제합니다",
                        "Permanently delete all messages in Trash/Spam",
                        "ゴミ箱/迷惑メールのメールを完全に削除します"));
    }
    if (m_unreadFilterBannerLbl) {
        m_unreadFilterBannerLbl->setText(MailI18n::t("📩 읽지 않은 메일만 보기", "📩 Unread messages only", "📩 未読メールのみ表示"));
    }
    if (m_scheduledRescheduleBtn) {
        m_scheduledRescheduleBtn->setText(MailI18n::t("🕒 시간 변경", "🕒 Reschedule", "🕒 時刻変更"));
    }
    if (m_scheduledEditOrCancelBtn) {
        m_scheduledEditOrCancelBtn->setText(MailI18n::t("✏️ 내용 수정 / 예약 취소", "✏️ Edit / Cancel Schedule", "✏️ 編集・予約取消"));
    }
    if (m_scheduledSendNowBtn) {
        m_scheduledSendNowBtn->setText(MailI18n::t("🚀 지금 즉시 발송", "🚀 Send Now", "🚀 今すぐ送信"));
    }
    if (m_undoSendCancelBtn) {
        m_undoSendCancelBtn->setText(MailI18n::t("↩️ 발송 취소 및 내용 수정 (Undo)", "↩️ Undo & Edit", "↩️ 送信取消・編集"));
    }
    if (m_undoSendNowBtn) {
        m_undoSendNowBtn->setText(MailI18n::t("지금 즉시 전송", "Send Now", "今すぐ送信"));
    }
    if (m_actRestoreBtn) {
        m_actRestoreBtn->setText(MailI18n::t("♻️ 원래 편지함으로 복원", "♻️ Restore", "♻️ 元のフォルダに復元"));
        m_actRestoreBtn->setToolTip(MailI18n::t("선택한 휴지통/스팸 메일을 원래 편지함(받은편지함/보낸편지함 등)으로 복원합니다",
                                                "Restore selected message from Trash/Spam to its original folder",
                                                "選択したメールを元のフォルダに復元します"));
    }

    refreshFolderSelectionStyles();
    refreshConversationList();
    refreshCalendarViews();
    rebuildContactGroupsList();
    refreshContactsList();
    updateScheduledMailStatusIndicator();
}

void MainWindow::onQuickAddAccountFromLeftBar()
{
    AddAccountDialog dlg(m_googleProvider, nullptr, QString(), this);
    if (dlg.exec() == QDialog::Accepted) {
        AccountItem newAcc = dlg.resultAccount();
        bool updatedExisting = false;
        for (int i = 0; i < m_accounts.size(); ++i) {
            if (m_accounts[i].email.compare(newAcc.email, Qt::CaseInsensitive) == 0) {
                newAcc.id = m_accounts[i].id;
                m_accounts[i] = newAcc;
                updatedExisting = true;
                break;
            }
        }
        if (!updatedExisting) {
            m_accounts.append(newAcc);
        }
        m_selectedAccountId = newAcc.id;
        pruneDeletedAccountsData();
        rebuildLeftBarAccounts();
        rebuildCalendarTree();
        saveAccountsToDisk();
        onSelectAccount(newAcc.id);
        m_googleProvider->fetchAccountAvatar(newAcc);
        m_googleProvider->fetchImapFolder(newAcc, QStringLiteral("inbox"), 20, 0);
        if (!newAcc.oauthAccessToken.isEmpty() || !newAcc.oauthRefreshToken.isEmpty()) {
            m_googleProvider->syncGoogleCalendarEvents(newAcc.email, newAcc.oauthAccessToken, newAcc.oauthRefreshToken);
            m_googleProvider->syncGoogleContacts(newAcc.email, newAcc.oauthAccessToken, newAcc.oauthRefreshToken);
        }
        const bool isICloudAcc = (newAcc.provider == AccountProviderType::ICloud)
                                 || newAcc.imapHost.contains("mail.me.com", Qt::CaseInsensitive)
                                 || newAcc.imapHost.contains("icloud.com", Qt::CaseInsensitive)
                                 || newAcc.email.endsWith("@icloud.com", Qt::CaseInsensitive)
                                 || newAcc.email.endsWith("@me.com", Qt::CaseInsensitive)
                                 || newAcc.email.endsWith("@mac.com", Qt::CaseInsensitive);
        if (isICloudAcc && !newAcc.appPassword.trimmed().isEmpty()) {
            m_googleProvider->syncICloudCalDavCalendars(newAcc.email, newAcc.appPassword);
        }
    }
}

void MainWindow::pruneDeletedAccountsData()
{
    QSet<QString> activeAccountIds;
    QSet<QString> activeEmailsUpper;
    for (const AccountItem &acc : m_accounts) {
        activeAccountIds.insert(acc.id);
        if (acc.provider != AccountProviderType::Unified && !acc.email.trimmed().isEmpty()) {
            activeEmailsUpper.insert(acc.email.trimmed().toUpper());
        }
    }

    for (int i = m_calendars.size() - 1; i >= 0; --i) {
        if (!activeEmailsUpper.contains(m_calendars[i].accountEmail.trimmed().toUpper())) {
            m_calendars.removeAt(i);
        }
    }

    for (const AccountItem &acc : m_accounts) {
        if (acc.provider == AccountProviderType::Unified || acc.email.trimmed().isEmpty())
            continue;
        QString upper = acc.email.trimmed().toUpper();
        bool hasCal = false;
        for (const CalendarSubItem &sub : m_calendars) {
            if (sub.accountEmail.trimmed().toUpper() == upper) {
                hasCal = true;
                break;
            }
        }
        if (!hasCal) {
            CalendarSubItem defCal;
            defCal.id = "cal_" + acc.id;
            defCal.accountEmail = upper;
            defCal.name = (acc.provider == AccountProviderType::Google) ? acc.email : "My calendar";
            defCal.color = QColor("#2563EB");
            defCal.isVisible = true;
            defCal.isDefault = true;
            defCal.showEyeToggle = false;
            m_calendars.append(defCal);
        }
    }

    for (int i = m_calendarEvents.size() - 1; i >= 0; --i) {
        QString evEmail = m_calendarEvents[i].accountEmail.trimmed().toUpper();
        if (!evEmail.isEmpty() && !activeEmailsUpper.contains(evEmail)) {
            m_calendarEvents.removeAt(i);
        }
    }

    for (int i = m_contacts.size() - 1; i >= 0; --i) {
        QString ctEmail = m_contacts[i].accountEmail.trimmed().toUpper();
        if (ctEmail != "SOLLUNAMAIL (LOCAL)" && !activeEmailsUpper.contains(ctEmail)) {
            m_contacts.removeAt(i);
        }
    }

    for (int i = m_conversations.size() - 1; i >= 0; --i) {
        if (!activeAccountIds.contains(m_conversations[i].accountId)) {
            m_conversations.removeAt(i);
        }
    }
}

void MainWindow::rebuildCalendarTree()
{
    if (!m_calendarTreeLayout || !m_calendarTreeContainer)
        return;

    QLayoutItem *child = nullptr;
    while ((child = m_calendarTreeLayout->takeAt(0)) != nullptr) {
        if (QWidget *w = child->widget()) {
            w->hide();
            w->setParent(nullptr);
            w->deleteLater();
        }
        delete child;
    }

    for (const AccountItem &acc : m_accounts) {
        if (acc.provider == AccountProviderType::Unified || acc.email.trimmed().isEmpty())
            continue;
        if (!acc.calendarEnabled)
            continue;
        QString grp = acc.email.trimmed().toUpper();

        QLabel *grpHdr = new QLabel("⌃   " + grp, m_calendarTreeContainer);
        grpHdr->setStyleSheet("color: #6B7280; font-size: 9.5px; font-weight: 600; margin-top: 6px;");
        m_calendarTreeLayout->addWidget(grpHdr);

        for (int i = 0; i < m_calendars.size(); ++i) {
            if (m_calendars[i].accountEmail.trimmed().toUpper() != grp)
                continue;
            QWidget *itemRow = new QWidget(m_calendarTreeContainer);
            QHBoxLayout *ih = new QHBoxLayout(itemRow);
            ih->setContentsMargins(6, 2, 2, 2);
            ih->setSpacing(8);

            QLabel *colorBox = new QLabel(itemRow);
            colorBox->setFixedSize(11, 11);
            colorBox->setStyleSheet(QString("background-color: %1; border-radius: 2px;").arg(m_calendars[i].color.name()));
            ih->addWidget(colorBox);

            QLabel *nameLbl = new QLabel(m_calendars[i].name, itemRow);
            nameLbl->setStyleSheet(m_calendars[i].isVisible
                                       ? "color: #374151; font-size: 11px;"
                                       : "color: #9CA3AF; font-size: 11px; text-decoration: line-through;");
            ih->addWidget(nameLbl, 1);

            if (m_calendars[i].isDefault) {
                QLabel *defLbl = new QLabel(MailI18n::t("기본", "Default", "デフォルト"), itemRow);
                defLbl->setStyleSheet("color: #6B7280; font-size: 10px;");
                ih->addWidget(defLbl);
            } else if (m_calendars[i].showEyeToggle) {
                QToolButton *eyeBtn = new QToolButton(itemRow);
                eyeBtn->setFixedSize(18, 18);
                eyeBtn->setCursor(Qt::PointingHandCursor);
                eyeBtn->setIcon(MailIcons::actionIcon("eye_off",
                                                      m_calendars[i].isVisible ? QColor("#0D47A1") : QColor("#9CA3AF"),
                                                      14));
                eyeBtn->setStyleSheet("border: none; background: transparent;");
                connect(eyeBtn, &QToolButton::clicked, this, [this, i]() {
                    QTimer::singleShot(0, this, [this, i]() {
                        if (i >= 0 && i < m_calendars.size()) {
                            m_calendars[i].isVisible = !m_calendars[i].isVisible;
                            rebuildCalendarTree();
                            refreshCalendarViews();
                        }
                    });
                });
                ih->addWidget(eyeBtn);
            }
            m_calendarTreeLayout->addWidget(itemRow);
        }
    }
    m_calendarTreeLayout->addStretch();
}

void MainWindow::rebuildCustomFoldersList()
{
    if (!m_customFoldersLayout)
        return;

    for (auto it = m_folderButtons.begin(); it != m_folderButtons.end(); ) {
        bool isSys = false;
        for (const FolderItem &f : m_folders) {
            if (!f.isCustom && f.id == it.key()) {
                isSys = true;
                break;
            }
        }
        if (!isSys) {
            it = m_folderButtons.erase(it);
        } else {
            ++it;
        }
    }

    QLayoutItem *child = nullptr;
    while ((child = m_customFoldersLayout->takeAt(0)) != nullptr) {
        if (QWidget *w = child->widget()) {
            w->hide();
            w->setParent(nullptr);
            w->deleteLater();
        }
        delete child;
    }

    QString currentAccEmail;
    bool currentAccIsPop3 = false;
    for (const AccountItem &acc : m_accounts) {
        if (acc.id == m_selectedAccountId) {
            currentAccEmail = acc.email;
            currentAccIsPop3 = (acc.provider != AccountProviderType::Unified && acc.isPop3());
            break;
        }
    }

    QSet<QString> renderedFolderIds;
    for (const FolderItem &f : m_folders) {
        if (!f.isCustom)
            continue;

        bool ownerExists = false;
        bool ownerIsPop3 = false;
        for (const AccountItem &acc : m_accounts) {
            if (acc.provider == AccountProviderType::Unified)
                continue;
            const bool idMatch = (!f.accountId.isEmpty() && acc.id == f.accountId);
            const bool emailMatch = (f.accountId.isEmpty() && !f.accountEmail.isEmpty()
                                     && acc.email.compare(f.accountEmail, Qt::CaseInsensitive) == 0);
            if (idMatch || emailMatch) {
                ownerExists = true;
                if (acc.isPop3())
                    ownerIsPop3 = true;
                break;
            }
        }
        if (!ownerExists)
            continue;
        if ((currentAccIsPop3 || ownerIsPop3) && !f.rawImapMailbox.isEmpty())
            continue;

        if (m_selectedAccountId != "acc_unified") {
            bool matchById = (!f.accountId.isEmpty() && f.accountId == m_selectedAccountId);
            bool matchByEmail = (f.accountId.isEmpty() && !currentAccEmail.isEmpty() && !f.accountEmail.isEmpty()
                                 && f.accountEmail.compare(currentAccEmail, Qt::CaseInsensitive) == 0);
            if (!matchById && !matchByEmail)
                continue;
        }

        if (renderedFolderIds.contains(f.id))
            continue;
        renderedFolderIds.insert(f.id);

        const QString translatedName = MailI18n::folderTitle(f.id, f.name);
        QString labelText = f.hasChildren ? (translatedName + "                   ⌄") : translatedName;
        QPushButton *btn = new QPushButton(labelText, m_folderPaneWidget);
        btn->setCursor(Qt::PointingHandCursor);
        QColor dotCol = f.dotColor.isValid() ? f.dotColor : QColor("#4F8EF7");
        btn->setIcon(MailIcons::folderIcon(FolderKind::CustomFolder, dotCol, 14));
        btn->setIconSize(QSize(14, 14));
        btn->setFixedHeight(29);
        connect(btn, &QPushButton::clicked, this, [this, f]() {
            onSelectFolder(f.id, MailI18n::folderTitle(f.id, f.name));
        });
        m_customFoldersLayout->addWidget(btn);
        m_folderButtons.insert(f.id, btn);
    }

    refreshFolderSelectionStyles();
}

int MainWindow::applyFilterRulesToConversations()
{
    const QList<MailFilterRule> &rules = MailI18n::filterRules();
    const bool enforceBlockedSenders = (m_privacySettings.privacyMode && m_privacySettings.blockSenders
                                        && !m_privacySettings.blockedSendersList.isEmpty());
    if (rules.isEmpty() && !enforceBlockedSenders)
        return 0;

    struct ServerMoveBatch {
        QString accountId;
        QString sourceFolderId;
        QString targetFolderId;
        QString targetDisplayName;
        bool markRead = false;
        bool markStar = false;
        QList<int> seqNums;
    };
    QMap<QString, ServerMoveBatch> serverMoveBatches;
    auto queueBatchMove = [&](const QString &accId,
                              const QString &srcFolder,
                              int seq,
                              const QString &tgtId,
                              const QString &tgtName,
                              bool mRead,
                              bool mStar) {
        if (accId.isEmpty() || seq <= 0 || tgtId.isEmpty() || srcFolder == tgtId)
            return;
        const QString key = accId + QStringLiteral("|") + srcFolder + QStringLiteral("|") + tgtId;
        ServerMoveBatch &b = serverMoveBatches[key];
        b.accountId = accId;
        b.sourceFolderId = srcFolder;
        b.targetFolderId = tgtId;
        if (!tgtName.isEmpty())
            b.targetDisplayName = tgtName;
        b.markRead = b.markRead || mRead;
        b.markStar = b.markStar || mStar;
        if (!b.seqNums.contains(seq))
            b.seqNums.append(seq);
    };
    auto extractSeqFromId = [](const QString &cid) -> int {
        const int lastUnder = cid.lastIndexOf('_');
        return (lastUnder > 0) ? cid.mid(lastUnder + 1).toInt() : 0;
    };

    int matchedCount = 0;
    for (ConversationItem &c : m_conversations) {
        if (c.messages.isEmpty())
            continue;

        if (c.folderId.startsWith(QStringLiteral("custom_acc_"))
            && !c.folderId.startsWith(QStringLiteral("custom_") + c.accountId + QStringLiteral("_"))) {
            c.folderId = QStringLiteral("inbox");
            c.folderTag = QStringLiteral("Inbox");
        }

        if (c.folderId == QStringLiteral("sent")
            || c.folderId == QStringLiteral("drafts")
            || c.folderId == QStringLiteral("trash")
            || c.folderId == QStringLiteral("spam")) {
            continue;
        }

        const MessageItem &m = c.latestMessage();
        const QString cSigKey = conversationSignatureKey(c);
        const int seqNum = extractSeqFromId(c.id);
        const QString inboxPrefix = QStringLiteral("imap_") + c.accountId + QStringLiteral("_inbox_");
        const bool stillInServerInbox = c.id.startsWith(inboxPrefix);

        if (enforceBlockedSenders && c.folderId == QStringLiteral("inbox")) {
            bool blockedHit = false;
            for (const QString &bAddr : m_privacySettings.blockedSendersList) {
                if (isSenderMatchedByBlockedRule(m.senderEmail, bAddr)) {
                    blockedHit = true;
                    break;
                }
            }
            if (blockedHit) {
                c.previousFolderId = c.folderId;
                c.folderId = QStringLiteral("spam");
                c.folderTag = QStringLiteral("Spam");
                c.isUnread = false;
                m_readConvSigs.insert(cSigKey);
                if (seqNum > 0) {
                    c.id = QString("imap_%1_spam_%2").arg(c.accountId).arg(seqNum);
                    queueBatchMove(c.accountId, QStringLiteral("inbox"), seqNum, QStringLiteral("spam"), QStringLiteral("Spam"), true, false);
                }
                ++matchedCount;
                continue;
            }
        }
        QString curAccEmail;
        for (const AccountItem &acc : m_accounts) {
            if (acc.id == c.accountId) {
                curAccEmail = acc.email.trimmed();
                break;
            }
        }
        for (const MailFilterRule &r : rules) {
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
                if (r.matchField == "From") {
                    hit = m.senderName.contains(q, Qt::CaseInsensitive) || m.senderEmail.contains(q, Qt::CaseInsensitive);
                } else if (r.matchField == "To") {
                    hit = m.recipientSummary.contains(q, Qt::CaseInsensitive);
                } else if (r.matchField == "Subject") {
                    hit = c.subject.contains(q, Qt::CaseInsensitive);
                } else if (r.matchField == "Body") {
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
                if (!r.targetFolderId.isEmpty() && (c.folderId != r.targetFolderId || stillInServerInbox)) {
                    const QString srcFolder = stillInServerInbox ? QStringLiteral("inbox") : c.folderId;
                    if (c.folderId != r.targetFolderId)
                        c.previousFolderId = c.folderId;
                    c.folderId = r.targetFolderId;
                    c.folderTag = MailI18n::folderTitle(r.targetFolderId, resolvedName);
                    c.imapSeq = 0;
                    if (r.targetFolderId == QStringLiteral("important")) {
                        c.isImportant = true;
                        m_importantConvSigs.insert(cSigKey);
                        m_unimportantConvSigs.remove(cSigKey);
                    }
                    if (r.targetFolderId == QStringLiteral("starred")) {
                        c.isStarred = true;
                        m_starredConvSigs.insert(cSigKey);
                        m_unstarredConvSigs.remove(cSigKey);
                    }
                    if (seqNum > 0) {
                        c.id = QString("imap_%1_%2_%3").arg(c.accountId, r.targetFolderId).arg(seqNum);
                        queueBatchMove(c.accountId, srcFolder, seqNum, r.targetFolderId, resolvedName, r.markAsRead, r.markAsStarred);
                    }
                } else if (r.markAsRead && seqNum > 0 && m_googleProvider) {
                    for (const AccountItem &acc : m_accounts) {
                        if (acc.id == c.accountId) {
                            m_googleProvider->setImapMessagesSeenFlagBatch(acc, c.folderId, {seqNum}, true);
                            break;
                        }
                    }
                }
                if (r.markAsRead) {
                    c.isUnread = false;
                    m_readConvSigs.insert(cSigKey);
                }
                if (r.markAsStarred) {
                    c.isStarred = true;
                    m_starredConvSigs.insert(cSigKey);
                    m_unstarredConvSigs.remove(cSigKey);
                }
                ++matchedCount;
                break;
            }
        }
    }

    QHash<QString, int> keptIndexBySig;
    for (int i = 0; i < m_conversations.size(); ) {
        const ConversationItem &c = m_conversations[i];
        const QString sigKey = conversationSignatureKey(c);
        const QString key = c.accountId + QStringLiteral("|") + sigKey;
        if (!keptIndexBySig.contains(key)) {
            keptIndexBySig.insert(key, i);
            ++i;
        } else {
            const int prevIdx = keptIndexBySig.value(key);
            ConversationItem &prev = m_conversations[prevIdx];
            if (prev.folderId == c.folderId && prev.id != c.id
                && prev.imapSeq > 0 && c.imapSeq > 0
                && prev.previousFolderId.isEmpty() && c.previousFolderId.isEmpty()) {
                ++i;
                continue;
            }
            const bool mergedImportant = prev.isImportant || c.isImportant;
            const bool mergedStarred = prev.isStarred || c.isStarred;
            const bool mergedUnread = prev.isUnread && c.isUnread && !m_readConvSigs.contains(sigKey);
            if (!mergedUnread)
                m_readConvSigs.insert(sigKey);
            if ((prev.folderId == QStringLiteral("inbox") && c.folderId != QStringLiteral("inbox"))
                || (prev.imapSeq == 0 && c.imapSeq > 0)) {
                prev = c;
            }
            prev.isImportant = mergedImportant;
            prev.isStarred = mergedStarred;
            prev.isUnread = mergedUnread;
            m_conversations.removeAt(i);
        }
    }

    if (m_googleProvider && !serverMoveBatches.isEmpty()) {
        for (auto it = serverMoveBatches.begin(); it != serverMoveBatches.end(); ++it) {
            const ServerMoveBatch &b = it.value();
            for (const AccountItem &acc : m_accounts) {
                if (acc.id == b.accountId) {
                    m_googleProvider->moveImapMessagesToFolder(
                        acc, b.sourceFolderId, b.seqNums, b.targetFolderId, b.targetDisplayName, b.markRead, b.markStar);
                    break;
                }
            }
        }
    }

    return matchedCount;
}

void MainWindow::rebuildLeftBarAccounts()
{
    if (!m_leftBarAccountsLayout || !m_leftBarWidget)
        return;

    m_accountButtons.clear();
    QLayoutItem *child = nullptr;
    while ((child = m_leftBarAccountsLayout->takeAt(0)) != nullptr) {
        if (QWidget *w = child->widget()) {
            w->hide();
            w->setParent(nullptr);
            w->deleteLater();
        }
        delete child;
    }

    QSet<QString> validAccountIds;
    bool selectedExists = false;
    for (const AccountItem &acc : m_accounts) {
        validAccountIds.insert(acc.id);
        if (acc.id == m_selectedAccountId) {
            selectedExists = true;
        }
    }
    for (int i = m_conversations.size() - 1; i >= 0; --i) {
        if (!m_conversations[i].accountId.isEmpty() && !validAccountIds.contains(m_conversations[i].accountId)) {
            m_conversations.removeAt(i);
        }
    }
    if (!selectedExists && !m_accounts.isEmpty()) {
        m_selectedAccountId = m_accounts.size() > 1 ? m_accounts[1].id : m_accounts.first().id;
    }

    for (const AccountItem &acc : m_accounts) {
        QToolButton *btn = new QToolButton(m_leftBarWidget);
        btn->setFixedSize(34, 34);
        btn->setCursor(Qt::PointingHandCursor);
        btn->setToolTip(QString("%1 (%2)").arg(acc.displayName, acc.email));
        bool isActive = (acc.id == m_selectedAccountId);
        btn->setIcon(QIcon(MailIcons::accountAvatarPixmap(acc, 34, isActive)));
        btn->setIconSize(QSize(34, 34));
        btn->setStyleSheet("QToolButton { border: none; background: transparent; border-radius: 7px; }"
                           "QToolButton:hover { background-color: rgba(255,255,255,0.12); }");
        connect(btn, &QToolButton::clicked, this, [this, acc]() {
            onSelectAccount(acc.id);
        });
        m_leftBarAccountsLayout->addWidget(btn, 0, Qt::AlignHCenter);
        m_accountButtons.insert(acc.id, btn);

        if (isActive && m_folderAccountHeaderLbl) {
            m_folderAccountHeaderLbl->setText(acc.email.toUpper());
        }
    }

    rebuildContactGroupsList();
}

void MainWindow::harvestContactsFromConversations(const QList<ConversationItem> &convs)
{
    if (convs.isEmpty())
        return;

    const QVector<QPair<QColor, QColor>> paletteColors = {
        {QColor("#E0F2FE"), QColor("#0284C7")},
        {QColor("#DCFCE7"), QColor("#15803D")},
        {QColor("#FEF3C7"), QColor("#B45309")},
        {QColor("#FCE7F3"), QColor("#DB2777")},
        {QColor("#EDE9FE"), QColor("#6D28D9")},
        {QColor("#CCFBF1"), QColor("#0F766E")}
    };

    bool addedAny = false;
    for (const ConversationItem &conv : convs) {
        QString ownerEmail = "SOLLUNAMAIL (LOCAL)";
        for (const AccountItem &acc : m_accounts) {
            if (acc.id == conv.accountId && !acc.email.isEmpty()) {
                ownerEmail = acc.email.toUpper();
                break;
            }
        }

        auto registerCandidate = [&](const QString &rawName, const QString &rawAddr) {
            QString addr = rawAddr.trimmed();
            if (addr.contains('<'))
                addr = addr.section('<', 1, 1).section('>', 0, 0).trimmed();
            if (addr.isEmpty() || !addr.contains('@'))
                return;

            QString cleanName = rawName.trimmed();
            if (cleanName.startsWith("To:", Qt::CaseInsensitive))
                cleanName = cleanName.mid(3).trimmed();
            if (cleanName.contains('<'))
                cleanName = cleanName.section('<', 0, 0).remove('"').trimmed();

            bool exists = false;
            for (ContactItem &existing : m_contacts) {
                if (existing.email.compare(addr, Qt::CaseInsensitive) == 0) {
                    if ((existing.displayName.isEmpty() || existing.displayName == existing.email)
                        && !cleanName.isEmpty() && cleanName != addr) {
                        existing.displayName = cleanName;
                        existing.initials = existing.displayName.left(1).toUpper();
                        addedAny = true;
                    }
                    exists = true;
                    break;
                }
            }
            if (!exists) {
                ContactItem c;
                c.id = "mail_ct_" + addr.toLower();
                c.accountEmail = ownerEmail;
                c.groupName = ownerEmail;
                c.email = addr;
                c.displayName = cleanName.isEmpty() ? addr : cleanName;
                c.initials = c.displayName.left(1).toUpper();
                QString dom = addr.section('@', 1).toLower();
                if (!dom.isEmpty() && dom != "gmail.com" && dom != "naver.com" && dom != "daum.net" && dom != "kakao.com") {
                    c.company = dom;
                }
                auto colPair = paletteColors[m_contacts.size() % paletteColors.size()];
                c.avatarBgColor = colPair.first;
                c.avatarFgColor = colPair.second;
                m_contacts.append(c);
                addedAny = true;
            }
        };

        for (const MessageItem &m : conv.messages) {
            registerCandidate(m.senderName, m.senderEmail);
            if (!m.recipientSummary.trimmed().isEmpty() && m.recipientSummary.contains('@')) {
                const QStringList recips = m.recipientSummary.split(',');
                for (const QString &r : recips) {
                    registerCandidate(r, r);
                }
            }
        }
    }

    if (addedAny) {
        saveCalendarAndContactsCacheToDisk();
        refreshContactsList();
    }
}

static QString ensureUserBackupDir()
{
#ifdef Q_OS_WIN
    QString base = qEnvironmentVariable("APPDATA").trimmed();
    if (base.isEmpty())
        base = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    else
        base = QDir::fromNativeSeparators(base) + QStringLiteral("/SOLLUNA_MAIL");
#else
    QString base = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
#endif
    const QString backupDir = base + QStringLiteral("/Backup");
    QDir().mkpath(backupDir);
    return backupDir;
}

static QString ensureWritableAppStorageRoot()
{
    static QString s_cachedRoot;
    if (!s_cachedRoot.isEmpty())
        return s_cachedRoot;

    const QString appDir = QCoreApplication::applicationDirPath();
    const QString lowerDir = appDir.toLower();
    bool canWriteAppDir = !lowerDir.contains(QStringLiteral("program files"))
                          && !lowerDir.contains(QStringLiteral(".app/contents/"))
                          && !lowerDir.startsWith(QStringLiteral("/opt/sollunamail"))
                          && !lowerDir.startsWith(QStringLiteral("/usr/"));
    if (canWriteAppDir) {
        const QString testCfgDir = appDir + QStringLiteral("/config_set");
        if (QDir().mkpath(testCfgDir)) {
            QFile probe(testCfgDir + QStringLiteral("/.write_probe"));
            if (probe.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
                probe.write("ok");
                probe.close();
                probe.remove();
            } else {
                canWriteAppDir = false;
            }
        } else {
            canWriteAppDir = false;
        }
    }

    if (canWriteAppDir) {
        s_cachedRoot = appDir;
    } else {
#ifdef Q_OS_WIN
        QString appData = qEnvironmentVariable("APPDATA").trimmed();
        if (!appData.isEmpty())
            s_cachedRoot = QDir::fromNativeSeparators(appData) + QStringLiteral("/SOLLUNA_MAIL");
        else
            s_cachedRoot = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
#else
        s_cachedRoot = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
#endif
        QDir().mkpath(s_cachedRoot);
    }
    return s_cachedRoot;
}

static QString ensureAppCacheDir()
{
    const QString cacheDir = ensureWritableAppStorageRoot() + QStringLiteral("/cache");
    QDir().mkpath(cacheDir);
    return cacheDir;
}

static QString resolveAndMigrateCacheFilePath(const QString &fileName)
{
    const QString cacheDir = ensureAppCacheDir();
    const QString newPath = cacheDir + QStringLiteral("/") + fileName;
    const QString legacyPath = QCoreApplication::applicationDirPath() + QStringLiteral("/") + fileName;
    if (QFile::exists(legacyPath)) {
        if (!QFile::exists(newPath)) {
            QFile::rename(legacyPath, newPath);
        } else {
            QFile::remove(legacyPath);
        }
    }
    if (!QFile::exists(newPath)) {
        QDir parentBuildDir(QCoreApplication::applicationDirPath());
        if (parentBuildDir.cdUp()) {
            const QStringList siblingDirs = parentBuildDir.entryList(QDir::Dirs | QDir::NoDotAndDotDot);
            for (const QString &sib : siblingDirs) {
                const QString candidate = parentBuildDir.absoluteFilePath(sib + QStringLiteral("/cache/") + fileName);
                if (QFile::exists(candidate)) {
                    QFile::copy(candidate, newPath);
                    break;
                }
            }
        }
    }
    return newPath;
}

static QString ensureAppConfigDir()
{
    const QString cfgDir = ensureWritableAppStorageRoot() + QStringLiteral("/config_set");
    QDir().mkpath(cfgDir);
    return cfgDir;
}

static QString resolveAndMigrateConfigFilePath(const QString &fileName)
{
    const QString cfgDir = ensureAppConfigDir();
    const QString newPath = cfgDir + QStringLiteral("/") + fileName;
    const QString legacyPath = QCoreApplication::applicationDirPath() + QStringLiteral("/") + fileName;
    if (QFile::exists(legacyPath)) {
        if (!QFile::exists(newPath)) {
            QFile::rename(legacyPath, newPath);
        } else {
            QFile::remove(legacyPath);
        }
    }
    if (!QFile::exists(newPath)) {
        QDir parentBuildDir(QCoreApplication::applicationDirPath());
        if (parentBuildDir.cdUp()) {
            const QStringList siblingDirs = parentBuildDir.entryList(QDir::Dirs | QDir::NoDotAndDotDot);
            for (const QString &sib : siblingDirs) {
                const QString candidate = parentBuildDir.absoluteFilePath(sib + QStringLiteral("/config_set/") + fileName);
                if (QFile::exists(candidate)) {
                    QFile::copy(candidate, newPath);
                    break;
                }
            }
        }
    }
    return newPath;
}

static void checkAndOfferBackupRestoreOnStartup(QWidget *parentWidget)
{
    static bool s_checkedOnce = false;
    if (s_checkedOnce)
        return;
    s_checkedOnce = true;

    const QString cfgDir = ensureAppConfigDir();
    const QString cacheDir = ensureAppCacheDir();
    const QString currentAccountsPath = resolveAndMigrateConfigFilePath(QStringLiteral("accounts.json"));

    if (QFile::exists(currentAccountsPath)) {
        QFile curFile(currentAccountsPath);
        if (curFile.open(QIODevice::ReadOnly)) {
            const QJsonArray curArr = QJsonDocument::fromJson(curFile.readAll()).array();
            curFile.close();
            if (!curArr.isEmpty()) {
                return;
            }
            if (QFile::exists(cfgDir + QStringLiteral("/.backup_prompt_done")))
                return;
        }
    }

    struct BackupCandidate {
        QString accountsFile;
        QString settingsFile;
        QString cacheFolder;
    };
    QList<BackupCandidate> candidates;

    const QString userBackupDir = ensureUserBackupDir();
    candidates.append({userBackupDir + QStringLiteral("/accounts.json"),
                       userBackupDir + QStringLiteral("/app_settings.json"),
                       userBackupDir});

#ifdef Q_OS_WIN
    const QString localAppData = QDir::fromNativeSeparators(qEnvironmentVariable("LOCALAPPDATA").trimmed());
    if (!localAppData.isEmpty()) {
        const QString localInstall = localAppData + QStringLiteral("/SOLLUNA_MAIL");
        candidates.append({localInstall + QStringLiteral("/config_set/accounts.json"),
                           localInstall + QStringLiteral("/config_set/app_settings.json"),
                           localInstall + QStringLiteral("/cache")});
    }
#endif

    const QString appDir = QCoreApplication::applicationDirPath();
    candidates.append({appDir + QStringLiteral("/config_set/accounts.json"),
                       appDir + QStringLiteral("/config_set/app_settings.json"),
                       appDir + QStringLiteral("/cache")});

    BackupCandidate foundCandidate;
    QStringList backupEmails;
    QDateTime backupTime;

    for (const BackupCandidate &cand : candidates) {
        if (QFileInfo(cand.accountsFile).canonicalFilePath() == QFileInfo(currentAccountsPath).canonicalFilePath())
            continue;
        if (!QFile::exists(cand.accountsFile))
            continue;
        QFile bf(cand.accountsFile);
        if (!bf.open(QIODevice::ReadOnly))
            continue;
        const QJsonArray arr = QJsonDocument::fromJson(bf.readAll()).array();
        bf.close();
        if (arr.isEmpty())
            continue;

        for (const QJsonValue &v : arr) {
            const QString em = v.toObject().value("email").toString().trimmed();
            if (!em.isEmpty() && !backupEmails.contains(em, Qt::CaseInsensitive))
                backupEmails.append(em);
        }
        if (!backupEmails.isEmpty()) {
            foundCandidate = cand;
            backupTime = QFileInfo(cand.accountsFile).lastModified();
            break;
        }
    }

    if (backupEmails.isEmpty())
        return;

    const QStringList cacheFiles = {
        QStringLiteral("calendars_cache.json"),
        QStringLiteral("contacts_cache.json"),
        QStringLiteral("contact_groups_cache.json")
    };
    const QStringList legacyMailCacheFiles = {
        QStringLiteral("conversations_cache.json"),
        QStringLiteral("deleted_mails_cache.json")
    };
    for (const QString &lcf : legacyMailCacheFiles) {
        QFile::remove(cacheDir + QStringLiteral("/") + lcf);
        QFile::remove(userBackupDir + QStringLiteral("/") + lcf);
        QFile::remove(appDir + QStringLiteral("/cache/") + lcf);
        QFile::remove(appDir + QStringLiteral("/") + lcf);
    }

    auto removeBackupFiles = [&](const BackupCandidate &cand) {
        QFile::remove(cand.accountsFile);
        QFile::remove(cand.settingsFile);
        for (const QString &cfName : cacheFiles) {
            QFile::remove(cand.cacheFolder + QStringLiteral("/") + cfName);
        }
        for (const QString &lcf : legacyMailCacheFiles) {
            QFile::remove(cand.cacheFolder + QStringLiteral("/") + lcf);
        }
        QFile::remove(userBackupDir + QStringLiteral("/accounts.json"));
        QFile::remove(userBackupDir + QStringLiteral("/app_settings.json"));
        for (const QString &cfName : cacheFiles) {
            QFile::remove(userBackupDir + QStringLiteral("/") + cfName);
        }
    };

    const QString timeStr = backupTime.isValid()
                                ? backupTime.toString(QStringLiteral("yyyy-MM-dd HH:mm:ss"))
                                : QStringLiteral("-");
    const QString emailSummary = backupEmails.join(QStringLiteral(", "));

    QMessageBox msgBox(parentWidget);
    msgBox.setWindowTitle(MailI18n::t("백업 데이터 복구 안내 — SollunaMail",
                                      "Restore Backup Data — SollunaMail",
                                      "バックアップデータの復元 — SollunaMail"));
    msgBox.setIcon(QMessageBox::Question);
    msgBox.setText(MailI18n::t(
        QString("<b>이전에 사용하던 SollunaMail 백업 파일이 발견되었습니다.</b><br/><br/>"
                "• <b>백업 일시:</b> %1<br/>"
                "• <b>백업된 계정 (%2개):</b> %3<br/><br/>"
                "백업된 계정 설정과 메일·캘린더·주소록 데이터를 지금 복구하시겠습니까?")
            .arg(timeStr)
            .arg(backupEmails.size())
            .arg(emailSummary.toHtmlEscaped()),
        QString("<b>A previous SollunaMail backup was found.</b><br/><br/>"
                "• <b>Backup Date:</b> %1<br/>"
                "• <b>Accounts (%2):</b> %3<br/><br/>"
                "Would you like to restore your accounts, emails, calendars, and contacts from this backup?")
            .arg(timeStr)
            .arg(backupEmails.size())
            .arg(emailSummary.toHtmlEscaped()),
        QString("<b>以前の SollunaMail バックアップファイルが見つかりました。</b><br/><br/>"
                "• <b>バックアップ日時:</b> %1<br/>"
                "• <b>アカウント (%2件):</b> %3<br/><br/>"
                "バックアップからアカウント設定・メール・カレンダー・連絡先データを復元しますか？")
            .arg(timeStr)
            .arg(backupEmails.size())
            .arg(emailSummary.toHtmlEscaped())));

    QPushButton *btnRestore = msgBox.addButton(
        MailI18n::t("✓ 예 (백업 데이터 복구)", "✓ Yes, Restore Backup", "✓ はい（バックアップを復元）"),
        QMessageBox::AcceptRole);
    QPushButton *btnSkip = msgBox.addButton(
        MailI18n::t("아니오 (새로 시작)", "No, Start Fresh", "いいえ（新規開始）"),
        QMessageBox::RejectRole);
    msgBox.setDefaultButton(btnRestore);
    msgBox.exec();

    if (msgBox.clickedButton() == btnRestore) {
        auto copyOverwrite = [](const QString &src, const QString &dst) {
            if (!QFile::exists(src))
                return;
            if (QFileInfo(src).canonicalFilePath() == QFileInfo(dst).canonicalFilePath())
                return;
            QFile::remove(dst);
            QFile::copy(src, dst);
        };
        copyOverwrite(foundCandidate.accountsFile, cfgDir + QStringLiteral("/accounts.json"));
        copyOverwrite(foundCandidate.settingsFile, cfgDir + QStringLiteral("/app_settings.json"));
        for (const QString &cfName : cacheFiles) {
            copyOverwrite(foundCandidate.cacheFolder + QStringLiteral("/") + cfName,
                          cacheDir + QStringLiteral("/") + cfName);
        }
        QFile::remove(cfgDir + QStringLiteral("/.backup_prompt_done"));

        QMessageBox delConfirmBox(parentWidget);
        delConfirmBox.setWindowTitle(MailI18n::t("백업 파일 삭제 확인 — SollunaMail",
                                                 "Delete Backup Files — SollunaMail",
                                                 "バックアップファイルの削除確認 — SollunaMail"));
        delConfirmBox.setIcon(QMessageBox::Question);
        delConfirmBox.setText(MailI18n::t(
            "<b>백업 데이터 복구가 완료되었습니다.</b><br/><br/>"
            "복구에 사용한 백업 파일을 삭제하시겠습니까?",
            "<b>Backup data has been restored successfully.</b><br/><br/>"
            "Would you like to delete the backup files?",
            "<b>バックアップデータの復元が完了しました。</b><br/><br/>"
            "復元に使用したバックアップファイルを削除しますか？"));
        QPushButton *btnDeleteBackup = delConfirmBox.addButton(
            MailI18n::t("백업 파일 삭제", "Delete Backup Files", "バックアップを削除"),
            QMessageBox::AcceptRole);
        QPushButton *btnKeepBackup = delConfirmBox.addButton(
            MailI18n::t("백업 파일 유지", "Keep Backup Files", "バックアップを保持"),
            QMessageBox::RejectRole);
        delConfirmBox.setDefaultButton(btnDeleteBackup);
        delConfirmBox.exec();

        if (delConfirmBox.clickedButton() == btnDeleteBackup) {
            removeBackupFiles(foundCandidate);
        }
    } else if (msgBox.clickedButton() == btnSkip) {
        QFile marker(cfgDir + QStringLiteral("/.backup_prompt_done"));
        if (marker.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            marker.write("1");
            marker.close();
        }
        if (!QFile::exists(currentAccountsPath)) {
            QFile emptyAcc(currentAccountsPath);
            if (emptyAcc.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
                emptyAcc.write("[]");
                emptyAcc.close();
            }
        }

        QMessageBox keepOrDelBox(parentWidget);
        keepOrDelBox.setWindowTitle(MailI18n::t("백업 파일 유지 또는 삭제 확인 — SollunaMail",
                                                "Keep or Delete Backup Files — SollunaMail",
                                                "バックアップの保持または削除 — SollunaMail"));
        keepOrDelBox.setIcon(QMessageBox::Question);
        keepOrDelBox.setText(MailI18n::t(
            "<b>백업을 복구하지 않고 새로 시작합니다.</b><br/><br/>"
            "기존 백업 파일을 그대로 유지하시겠습니까, 아니면 삭제하시겠습니까?",
            "<b>Starting fresh without restoring the backup.</b><br/><br/>"
            "Would you like to keep the existing backup files or delete them?",
            "<b>バックアップを復元せずに新規開始します。</b><br/><br/>"
            "既存のバックアップファイルを保持しますか、それとも削除しますか？"));
        QPushButton *btnKeep = keepOrDelBox.addButton(
            MailI18n::t("백업 파일 유지", "Keep Backup Files", "バックアップを保持"),
            QMessageBox::AcceptRole);
        QPushButton *btnDelete = keepOrDelBox.addButton(
            MailI18n::t("백업 파일 삭제", "Delete Backup Files", "バックアップを削除"),
            QMessageBox::DestructiveRole);
        keepOrDelBox.setDefaultButton(btnKeep);
        keepOrDelBox.exec();

        if (keepOrDelBox.clickedButton() == btnDelete) {
            removeBackupFiles(foundCandidate);
        }
    }
}

void MainWindow::saveConversationsCacheToDisk()
{
    QFile::remove(resolveAndMigrateCacheFilePath(QStringLiteral("conversations_cache.json")));
    QFile::remove(resolveAndMigrateCacheFilePath(QStringLiteral("deleted_mails_cache.json")));
    const QString appDir = QCoreApplication::applicationDirPath();
    QFile::remove(appDir + QStringLiteral("/cache/conversations_cache.json"));
    QFile::remove(appDir + QStringLiteral("/cache/deleted_mails_cache.json"));
    QFile::remove(appDir + QStringLiteral("/conversations_cache.json"));
    QFile::remove(appDir + QStringLiteral("/deleted_mails_cache.json"));

    {
        QJsonArray schedArr;
        for (const ConversationItem &c : m_conversations) {
            if (c.scheduledSendIso.isEmpty() || c.messages.isEmpty())
                continue;
            QString accEmail;
            for (const AccountItem &acc : m_accounts) {
                if (acc.id == c.accountId) {
                    accEmail = acc.email;
                    break;
                }
            }
            const MessageItem &m = c.latestMessage();
            QJsonObject o;
            o["id"] = c.id;
            o["accountId"] = c.accountId;
            o["accountEmail"] = accEmail;
            o["folderId"] = c.folderId.isEmpty() ? QStringLiteral("drafts") : c.folderId;
            o["previousFolderId"] = c.previousFolderId;
            o["folderTag"] = c.folderTag;
            o["subject"] = c.subject;
            o["imapUid"] = c.imapUid;
            o["imapSeq"] = c.imapSeq;
            o["isUnread"] = c.isUnread;
            o["isStarred"] = c.isStarred;
            o["isImportant"] = c.isImportant;
            o["isReplied"] = c.isReplied;
            o["isForwarded"] = c.isForwarded;
            o["scheduledSendIso"] = c.scheduledSendIso;
            o["ccSummary"] = c.ccSummary;
            o["bccSummary"] = c.bccSummary;
            o["readReceiptRequested"] = c.readReceiptRequested;
            o["readReceiptConfirmed"] = c.readReceiptConfirmed;
            o["senderName"] = m.senderName;
            o["senderEmail"] = m.senderEmail;
            o["senderInitials"] = m.senderInitials;
            o["recipientSummary"] = m.recipientSummary;
            o["shortTimeText"] = m.shortTimeText;
            o["fullTimeText"] = m.fullTimeText;
            o["snippet"] = m.snippet;
            o["htmlBody"] = m.htmlBody;
            if (!m.attachments.isEmpty()) {
                QJsonArray attArr;
                for (const AttachmentItem &att : m.attachments) {
                    QJsonObject ao;
                    ao["fileName"] = att.fileName;
                    ao["fileSize"] = att.fileSize;
                    ao["filePath"] = att.filePath;
                    attArr.append(ao);
                }
                o["attachments"] = attArr;
            }
            schedArr.append(o);
        }
        QJsonObject schedRoot;
        schedRoot["scheduledConversations"] = schedArr;
        QFile sf(resolveAndMigrateConfigFilePath(QStringLiteral("scheduled_mails.json")));
        if (sf.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            sf.write(QJsonDocument(schedRoot).toJson(QJsonDocument::Indented));
            sf.close();
        }
    }

    for (const AccountItem &acc : m_accounts) {
        if (acc.provider == AccountProviderType::Unified || !acc.isPop3())
            continue;
        acc.ensurePop3MailboxDirs();
        QJsonObject pop3Root;
        pop3Root["accountId"] = acc.id;
        pop3Root["email"] = acc.email;
        pop3Root["pop3LeaveCopyOnServer"] = acc.pop3LeaveCopyOnServer;
        QJsonArray pop3ConvArr;
        for (const ConversationItem &c : m_conversations) {
            if (c.accountId != acc.id || c.messages.isEmpty())
                continue;
            const MessageItem &m = c.latestMessage();
            QJsonObject o;
            o["id"] = c.id;
            o["accountId"] = c.accountId;
            o["folderId"] = c.folderId;
            o["previousFolderId"] = c.previousFolderId;
            o["folderTag"] = c.folderTag;
            o["subject"] = c.subject;
            o["isUnread"] = c.isUnread;
            o["isStarred"] = c.isStarred;
            o["isImportant"] = c.isImportant;
            o["isReplied"] = c.isReplied;
            o["isForwarded"] = c.isForwarded;
            o["scheduledSendIso"] = c.scheduledSendIso;
            o["ccSummary"] = c.ccSummary;
            o["bccSummary"] = c.bccSummary;
            o["readReceiptRequested"] = c.readReceiptRequested;
            o["readReceiptConfirmed"] = c.readReceiptConfirmed;
            o["senderName"] = m.senderName;
            o["senderEmail"] = m.senderEmail;
            o["senderInitials"] = m.senderInitials;
            o["recipientSummary"] = m.recipientSummary;
            o["shortTimeText"] = m.shortTimeText;
            o["fullTimeText"] = m.fullTimeText;
            o["snippet"] = m.snippet;
            o["htmlBody"] = m.htmlBody;
            if (!m.attachments.isEmpty()) {
                QJsonArray attArr;
                for (const AttachmentItem &att : m.attachments) {
                    QJsonObject ao;
                    ao["fileName"] = att.fileName;
                    ao["fileSize"] = att.fileSize;
                    ao["filePath"] = att.filePath;
                    attArr.append(ao);
                }
                o["attachments"] = attArr;
            }
            pop3ConvArr.append(o);
        }
        pop3Root["conversations"] = pop3ConvArr;
        QFile pf(acc.pop3AccountRootDir() + QStringLiteral("/mailbox_index.json"));
        if (pf.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            pf.write(QJsonDocument(pop3Root).toJson(QJsonDocument::Indented));
            pf.close();
        }
    }

    {
        QJsonArray mcpArr;
        int count = 0;
        for (const ConversationItem &c : m_conversations) {
            if (c.messages.isEmpty())
                continue;
            const MessageItem &m = c.latestMessage();
            QJsonObject o;
            o[QStringLiteral("id")] = c.id;
            o[QStringLiteral("accountId")] = c.accountId;
            o[QStringLiteral("folderId")] = c.folderId;
            o[QStringLiteral("subject")] = c.subject;
            o[QStringLiteral("isUnread")] = c.isUnread;
            o[QStringLiteral("isStarred")] = c.isStarred;
            o[QStringLiteral("isImportant")] = c.isImportant;
            o[QStringLiteral("senderName")] = m.senderName;
            o[QStringLiteral("senderEmail")] = m.senderEmail;
            o[QStringLiteral("recipientSummary")] = m.recipientSummary;
            o[QStringLiteral("shortTimeText")] = m.shortTimeText;
            o[QStringLiteral("fullTimeText")] = m.fullTimeText;
            o[QStringLiteral("snippet")] = m.snippet;
            if (!m.htmlBody.isEmpty() && !m.htmlBody.startsWith(QStringLiteral("<!--SM_HEADER_ONLY-->"))) {
                o[QStringLiteral("htmlBody")] = m.htmlBody.left(8000);
            }
            mcpArr.append(o);
            if (++count >= 300)
                break;
        }
        QFile mf(resolveAndMigrateCacheFilePath(QStringLiteral("mcp_emails_snapshot.json")));
        if (mf.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            mf.write(QJsonDocument(mcpArr).toJson(QJsonDocument::Compact));
            mf.close();
        }
    }

    updateScheduledMailStatusIndicator();
}

void MainWindow::loadConversationsCacheFromDisk()
{
    s_fullBodyFetchedConvIds.clear();
    QFile::remove(resolveAndMigrateCacheFilePath(QStringLiteral("conversations_cache.json")));
    QFile::remove(resolveAndMigrateCacheFilePath(QStringLiteral("deleted_mails_cache.json")));
    const QString appDir = QCoreApplication::applicationDirPath();
    QFile::remove(appDir + QStringLiteral("/cache/conversations_cache.json"));
    QFile::remove(appDir + QStringLiteral("/cache/deleted_mails_cache.json"));
    QFile::remove(appDir + QStringLiteral("/conversations_cache.json"));
    QFile::remove(appDir + QStringLiteral("/deleted_mails_cache.json"));

    QJsonArray convArr;

    {
        QFile sf(resolveAndMigrateConfigFilePath(QStringLiteral("scheduled_mails.json")));
        if (sf.exists() && sf.open(QIODevice::ReadOnly)) {
            const QJsonObject schedRoot = QJsonDocument::fromJson(sf.readAll()).object();
            sf.close();
            for (const QJsonValue &sv : schedRoot.value("scheduledConversations").toArray()) {
                QJsonObject so = sv.toObject();
                QString accId = so.value("accountId").toString();
                const QString accEmail = so.value("accountEmail").toString().trimmed();
                bool accFound = false;
                for (const AccountItem &acc : m_accounts) {
                    if (acc.provider == AccountProviderType::Unified)
                        continue;
                    if (acc.id == accId) {
                        accFound = true;
                        break;
                    }
                }
                if (!accFound && !accEmail.isEmpty()) {
                    for (const AccountItem &acc : m_accounts) {
                        if (acc.provider == AccountProviderType::Unified)
                            continue;
                        if (acc.email.compare(accEmail, Qt::CaseInsensitive) == 0) {
                            so["accountId"] = acc.id;
                            accFound = true;
                            break;
                        }
                    }
                }
                if (accFound) {
                    convArr.append(so);
                }
            }
        }
    }

    for (const AccountItem &acc : m_accounts) {
        if (acc.provider == AccountProviderType::Unified || !acc.isPop3())
            continue;
        acc.ensurePop3MailboxDirs();
        QFile pf(acc.pop3AccountRootDir() + QStringLiteral("/mailbox_index.json"));
        if (pf.exists() && pf.open(QIODevice::ReadOnly)) {
            const QJsonObject pop3Root = QJsonDocument::fromJson(pf.readAll()).object();
            pf.close();
            for (const QJsonValue &pv : pop3Root.value("conversations").toArray()) {
                QJsonObject po = pv.toObject();
                po["accountId"] = acc.id;
                convArr.append(po);
            }
        }
    }

    const QDateTime now = QDateTime::currentDateTime();
    int restoredScheduledCount = 0;
    int expiredScheduledCount = 0;

    for (const QJsonValue &v : convArr) {
        QJsonObject o = v.toObject();
        QString cid = o.value("id").toString();
        if (cid.isEmpty())
            continue;
        bool exists = false;
        for (const ConversationItem &ex : m_conversations) {
            if (ex.id == cid) {
                exists = true;
                break;
            }
        }
        if (exists)
            continue;

        ConversationItem c;
        c.id = cid;
        c.accountId = o.value("accountId").toString();
        c.folderId = o.value("folderId").toString();
        c.previousFolderId = o.value("previousFolderId").toString();
        c.folderTag = o.value("folderTag").toString();
        c.subject = o.value("subject").toString();
        c.imapUid = o.value("imapUid").toInt(0);
        c.imapSeq = o.value("imapSeq").toInt(0);
        c.isUnread = o.value("isUnread").toBool(false);
        c.isStarred = o.value("isStarred").toBool(false);
        c.isImportant = o.value("isImportant").toBool(false);
        c.isReplied = o.value("isReplied").toBool(false);
        c.isForwarded = o.value("isForwarded").toBool(false);
        c.scheduledSendIso = o.value("scheduledSendIso").toString();
        c.ccSummary = o.value("ccSummary").toString();
        c.bccSummary = o.value("bccSummary").toString();
        c.readReceiptRequested = o.value("readReceiptRequested").toBool(false);
        c.readReceiptConfirmed = o.value("readReceiptConfirmed").toBool(false);

        MessageItem m;
        m.id = cid + "_m1";
        m.senderName = o.value("senderName").toString();
        m.senderEmail = o.value("senderEmail").toString();
        m.senderInitials = o.value("senderInitials").toString();
        m.senderAvatarBg = QColor("#DBEAFE");
        m.senderAvatarFg = QColor("#1D4ED8");
        m.recipientSummary = o.value("recipientSummary").toString();
        m.shortTimeText = o.value("shortTimeText").toString();
        m.fullTimeText = o.value("fullTimeText").toString();
        m.subject = c.subject;
        m.snippet = o.value("snippet").toString();
        m.htmlBody = o.value("htmlBody").toString();
        if (!m.snippet.isEmpty() || !m.htmlBody.isEmpty()) {
            const QString cleanedSnip = GoogleProvider::htmlToPlainTextSnippet(m.htmlBody, m.snippet, 50);
            if (!cleanedSnip.isEmpty()) {
                m.snippet = cleanedSnip;
            }
        }
        for (const QJsonValue &av : o.value("attachments").toArray()) {
            QJsonObject ao = av.toObject();
            AttachmentItem att;
            att.fileName = ao.value("fileName").toString();
            att.fileSize = ao.value("fileSize").toString();
            att.filePath = ao.value("filePath").toString();
            if (!att.fileName.isEmpty() || !att.filePath.isEmpty())
                m.attachments.append(att);
        }

        if (!c.scheduledSendIso.isEmpty()) {
            c.folderId = QStringLiteral("drafts");
            const bool alreadyExpiredPrefix = c.scheduledSendIso.startsWith(QStringLiteral("EXPIRED:"));
            const QString rawIso = alreadyExpiredPrefix ? c.scheduledSendIso.mid(8) : c.scheduledSendIso;
            const QDateTime schedDt = QDateTime::fromString(rawIso, Qt::ISODate);
            if (!alreadyExpiredPrefix && schedDt.isValid() && schedDt > now) {
                c.folderTag = QString("⏰ 예약 (%1)").arg(schedDt.toString("MM/dd HH:mm"));
                m.shortTimeText = QString("⏰ %1").arg(schedDt.toString("MM/dd HH:mm"));
                m.fullTimeText = QString("⏰ 예약 발송 예정: %1").arg(schedDt.toString("yyyy-MM-dd AP h:mm"));
                ++restoredScheduledCount;
            } else {
                const QString oldSchedStr = schedDt.isValid() ? schedDt.toString("yyyy-MM-dd AP h:mm") : rawIso;
                c.scheduledSendIso = QStringLiteral("EXPIRED:") + rawIso;
                c.folderTag = QStringLiteral("⚠️ 예약 경과 (미발송)");
                if (m.senderName.startsWith("⏰ ")) {
                    m.senderName = QString("⚠️ ") + m.senderName.mid(2).trimmed();
                } else if (!m.senderName.startsWith("⚠️")) {
                    m.senderName = QString("⚠️ To: %1").arg(m.recipientSummary);
                }
                m.shortTimeText = QStringLiteral("⚠️ 예약 경과");
                m.fullTimeText = QString("⚠️ 예약 시간이 지나 보내지 못한 메일 (기존 예약: %1)").arg(oldSchedStr);
                ++expiredScheduledCount;
            }
        }

        c.messages.append(m);
        const QString cSig = conversationSignatureKey(c);
        m_notifiedConvIds.insert(cSig);
        if (!c.isUnread || m_readConvSigs.contains(cSig)) {
            c.isUnread = false;
            m_readConvSigs.insert(cSig);
        }
        if (c.isReplied || m_repliedConvSigs.contains(cSig)) {
            c.isReplied = true;
            m_repliedConvSigs.insert(cSig);
        }
        if (c.isForwarded || m_forwardedConvSigs.contains(cSig)) {
            c.isForwarded = true;
            m_forwardedConvSigs.insert(cSig);
        }
        if (m_unstarredConvSigs.contains(cSig)) {
            c.isStarred = false;
        } else if (c.isStarred || m_starredConvSigs.contains(cSig)) {
            c.isStarred = true;
            m_starredConvSigs.insert(cSig);
        }
        if (m_unimportantConvSigs.contains(cSig)) {
            c.isImportant = false;
        } else if (c.isImportant || m_importantConvSigs.contains(cSig)) {
            c.isImportant = true;
            m_importantConvSigs.insert(cSig);
        }
        m_conversations.append(c);
    }

    if (!m_conversations.isEmpty() || expiredScheduledCount > 0) {
        applyFilterRulesToConversations();
        reconcileSentReadReceipts(m_conversations);
        saveConversationsCacheToDisk();
    }

    if (restoredScheduledCount > 0 || expiredScheduledCount > 0) {
        QTimer::singleShot(800, this, [this, restoredScheduledCount, expiredScheduledCount]() {
            if (expiredScheduledCount > 0 && restoredScheduledCount > 0) {
                statusBar()->showMessage(
                    QString("⏰ 예약 메일 %1건 복원 완료 (지정 시각에 자동 발송) / ⚠️ 종료 중 예약 시각이 경과한 %2건은 임시보관함에 미발송 보관됨")
                        .arg(restoredScheduledCount)
                        .arg(expiredScheduledCount),
                    8000);
            } else if (restoredScheduledCount > 0) {
                statusBar()->showMessage(
                    QString("⏰ 저장된 예약 발송 스케줄 %1건이 복원되었습니다 (예약 시각 도래 시 자동 발송됩니다).")
                        .arg(restoredScheduledCount),
                    7000);
            } else if (expiredScheduledCount > 0) {
                statusBar()->showMessage(
                    QString("⚠️ 프로그램 종료 중 예약 시각이 지난 메일 %1건은 자동 발송되지 않고 임시보관함(Drafts)에 보관되었습니다.")
                        .arg(expiredScheduledCount),
                    8000);
            }
        });
    }
}

void MainWindow::saveCalendarAndContactsCacheToDisk()
{
    QJsonObject calRoot;
    QJsonArray subArr;
    for (const CalendarSubItem &sub : m_calendars) {
        QJsonObject o;
        o["id"] = sub.id;
        o["accountEmail"] = sub.accountEmail;
        o["name"] = sub.name;
        o["color"] = sub.color.name();
        o["isVisible"] = sub.isVisible;
        o["isDefault"] = sub.isDefault;
        o["showEyeToggle"] = sub.showEyeToggle;
        o["calDavHref"] = sub.calDavHref;
        o["webcalUrl"] = sub.webcalUrl;
        subArr.append(o);
    }
    calRoot["calendars"] = subArr;

    QJsonArray evArr;
    for (const CalendarEventItem &ev : m_calendarEvents) {
        QJsonObject o;
        o["id"] = ev.id;
        o["accountEmail"] = ev.accountEmail;
        o["calendarId"] = ev.calendarId;
        o["title"] = ev.title;
        o["date"] = ev.date.toString(Qt::ISODate);
        o["startTime"] = ev.startTime.toString("HH:mm");
        o["endTime"] = ev.endTime.toString("HH:mm");
        o["isAllDay"] = ev.isAllDay;
        o["location"] = ev.location;
        o["meetUrl"] = ev.meetUrl;
        o["calDavResourceUrl"] = ev.calDavResourceUrl;
        o["description"] = ev.description;
        o["pillBgColor"] = ev.pillBgColor.name();
        o["pillBarColor"] = ev.pillBarColor.name();
        o["pillTextColor"] = ev.pillTextColor.name();
        evArr.append(o);
    }
    calRoot["events"] = evArr;

    QFile cf(resolveAndMigrateCacheFilePath(QStringLiteral("calendars_cache.json")));
    if (cf.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        cf.write(QJsonDocument(calRoot).toJson(QJsonDocument::Indented));
        cf.close();
    }

    QJsonArray ctArr;
    for (const ContactItem &c : m_contacts) {
        QJsonObject o;
        o["id"] = c.id;
        o["accountEmail"] = c.accountEmail;
        o["groupName"] = c.groupName;
        o["displayName"] = c.displayName;
        o["initials"] = c.initials;
        o["avatarBgColor"] = c.avatarBgColor.name();
        o["avatarFgColor"] = c.avatarFgColor.name();
        o["email"] = c.email;
        o["phone"] = c.phone;
        o["company"] = c.company;
        o["jobTitle"] = c.jobTitle;
        o["notes"] = c.notes;
        ctArr.append(o);
    }
    QFile ctf(resolveAndMigrateCacheFilePath(QStringLiteral("contacts_cache.json")));
    if (ctf.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        ctf.write(QJsonDocument(ctArr).toJson(QJsonDocument::Indented));
        ctf.close();
    }

    QJsonArray grpArr;
    for (const QString &g : m_customContactGroups) {
        if (!g.trimmed().isEmpty())
            grpArr.append(g.trimmed());
    }
    QFile grpf(resolveAndMigrateCacheFilePath(QStringLiteral("contact_groups_cache.json")));
    if (grpf.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        grpf.write(QJsonDocument(grpArr).toJson(QJsonDocument::Indented));
        grpf.close();
    }
}

void MainWindow::loadCalendarAndContactsCacheFromDisk()
{
    QFile cf(resolveAndMigrateCacheFilePath(QStringLiteral("calendars_cache.json")));
    if (cf.exists() && cf.open(QIODevice::ReadOnly)) {
        QJsonObject calRoot = QJsonDocument::fromJson(cf.readAll()).object();
        cf.close();
        QJsonArray subArr = calRoot.value("calendars").toArray();
        if (!subArr.isEmpty()) {
            m_calendars.clear();
            for (const QJsonValue &v : subArr) {
                QJsonObject o = v.toObject();
                CalendarSubItem sub;
                sub.id = o.value("id").toString();
                sub.accountEmail = o.value("accountEmail").toString();
                sub.name = o.value("name").toString();
                sub.color = QColor(o.value("color").toString("#0D47A1"));
                sub.isVisible = o.value("isVisible").toBool(true);
                sub.isDefault = o.value("isDefault").toBool(false);
                sub.showEyeToggle = o.value("showEyeToggle").toBool(!sub.isDefault);
                sub.calDavHref = o.value("calDavHref").toString();
                sub.webcalUrl = o.value("webcalUrl").toString();
                m_calendars.append(sub);
            }
        }
        QJsonArray evArr = calRoot.value("events").toArray();
        if (!evArr.isEmpty()) {
            m_calendarEvents.clear();
            for (const QJsonValue &v : evArr) {
                QJsonObject o = v.toObject();
                CalendarEventItem ev;
                ev.id = o.value("id").toString();
                ev.accountEmail = o.value("accountEmail").toString();
                ev.calendarId = o.value("calendarId").toString();
                ev.title = o.value("title").toString();
                ev.date = QDate::fromString(o.value("date").toString(), Qt::ISODate);
                ev.startTime = QTime::fromString(o.value("startTime").toString("10:00"), "HH:mm");
                ev.endTime = QTime::fromString(o.value("endTime").toString("11:00"), "HH:mm");
                ev.isAllDay = o.value("isAllDay").toBool(false);
                ev.location = o.value("location").toString();
                ev.meetUrl = o.value("meetUrl").toString();
                ev.calDavResourceUrl = o.value("calDavResourceUrl").toString();
                ev.description = o.value("description").toString();
                ev.pillBgColor = QColor(o.value("pillBgColor").toString("#DBEAFE"));
                ev.pillBarColor = QColor(o.value("pillBarColor").toString("#0D47A1"));
                ev.pillTextColor = QColor(o.value("pillTextColor").toString("#1E3A8A"));
                if (ev.date.isValid())
                    m_calendarEvents.append(ev);
            }
        }
    }

    QFile grpf(resolveAndMigrateCacheFilePath(QStringLiteral("contact_groups_cache.json")));
    if (grpf.exists() && grpf.open(QIODevice::ReadOnly)) {
        QJsonArray grpArr = QJsonDocument::fromJson(grpf.readAll()).array();
        grpf.close();
        m_customContactGroups.clear();
        for (const QJsonValue &v : grpArr) {
            const QStringList splitG = splitContactGroupNames(v.toString());
            for (const QString &g : splitG) {
                if (!g.isEmpty() && !m_customContactGroups.contains(g, Qt::CaseInsensitive))
                    m_customContactGroups.append(g);
            }
        }
    }

    QFile ctf(resolveAndMigrateCacheFilePath(QStringLiteral("contacts_cache.json")));
    if (ctf.exists() && ctf.open(QIODevice::ReadOnly)) {
        QJsonArray ctArr = QJsonDocument::fromJson(ctf.readAll()).array();
        ctf.close();
        if (!ctArr.isEmpty()) {
            m_contacts.clear();
            for (const QJsonValue &v : ctArr) {
                QJsonObject o = v.toObject();
                ContactItem c;
                c.id = o.value("id").toString();
                c.accountEmail = o.value("accountEmail").toString();
                c.groupName = o.value("groupName").toString();
                c.displayName = o.value("displayName").toString();
                c.initials = o.value("initials").toString();
                c.avatarBgColor = QColor(o.value("avatarBgColor").toString("#E0F2FE"));
                c.avatarFgColor = QColor(o.value("avatarFgColor").toString("#0284C7"));
                c.email = o.value("email").toString();
                c.phone = o.value("phone").toString();
                c.company = o.value("company").toString();
                c.jobTitle = o.value("jobTitle").toString();
                c.notes = o.value("notes").toString();
                m_contacts.append(c);
            }
        }
    }
}

void MainWindow::saveAccountsToDisk()
{
    QJsonArray arr;
    for (const AccountItem &acc : m_accounts) {
        if (acc.provider == AccountProviderType::Unified)
            continue;
        QJsonObject o;
        o["id"] = acc.id;
        o["email"] = acc.email;
        o["displayName"] = acc.displayName;
        o["initials"] = acc.initials;
        o["avatarColor"] = acc.avatarColor.name();
        o["provider"] = int(acc.provider);
        o["visualStyle"] = int(acc.visualStyle);
        o["imapHost"] = acc.imapHost;
        o["imapPort"] = acc.imapPort;
        o["imapSecurity"] = acc.imapSecurity;
        o["imapUsername"] = acc.imapUsername;
        o["appPassword"] = acc.appPassword;
        o["pop3LeaveCopyOnServer"] = acc.pop3LeaveCopyOnServer;
        o["smtpPreset"] = acc.smtpPreset;
        o["smtpHost"] = acc.smtpHost;
        o["smtpPort"] = acc.smtpPort;
        o["smtpSecurity"] = acc.smtpSecurity;
        o["useSeparateSmtpAuth"] = acc.useSeparateSmtpAuth;
        o["smtpUsername"] = acc.smtpUsername;
        o["smtpPassword"] = acc.smtpPassword;
        o["oauthAccessToken"] = acc.oauthAccessToken;
        o["oauthRefreshToken"] = acc.oauthRefreshToken;
        o["avatarUrl"] = acc.avatarUrl;
        o["avatarPngBase64"] = acc.avatarPngBase64;
        o["avatarIconPreset"] = acc.avatarIconPreset;
        o["accountLabel"] = acc.accountLabel;
        o["includeInUnified"] = acc.includeInUnified;
        o["calendarEnabled"] = acc.calendarEnabled;
        o["useDifferentReplyTo"] = acc.useDifferentReplyTo;
        o["replyToAddress"] = acc.replyToAddress;
        o["signatureHtml"] = acc.signatureHtml;
        QJsonArray aliasArr;
        for (const QString &al : acc.aliases)
            aliasArr.append(al);
        o["aliases"] = aliasArr;
        arr.append(o);
    }

    QFile f(resolveAndMigrateConfigFilePath(QStringLiteral("accounts.json")));
    if (f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        f.write(QJsonDocument(arr).toJson(QJsonDocument::Indented));
        f.close();
    }

    QJsonObject appCfg;
    appCfg["language"] = MailI18n::languageCode();
    appCfg["theme"] = m_currentThemeName;
    appCfg["appearanceMode"] = MailI18n::appearanceMode();
    appCfg["layoutStyleName"] = MailI18n::layoutStyleName();
    appCfg["mailSortMode"] = m_mailSortMode;
    appCfg["menuExpanded"] = MailI18n::menuExpanded();
    appCfg["zoomPercent"] = MailI18n::zoomPercent();
    appCfg["defaultFontFamily"] = MailI18n::defaultFontFamily();
    appCfg["defaultFontSize"] = MailI18n::defaultFontSize();
    appCfg["undoSendDelaySec"] = MailI18n::undoSendDelaySec();
    appCfg["includeQuoteOnReply"] = MailI18n::includeQuoteOnReply();
    appCfg["autoAppendSignature"] = MailI18n::autoAppendSignature();
    appCfg["defaultSignatureHtml"] = MailI18n::defaultSignatureHtml();
    appCfg["markReadOnSelect"] = MailI18n::markReadOnSelect();
    appCfg["groupByConversationThread"] = MailI18n::groupByConversationThread();
    appCfg["showRightCalendarSidebar"] = MailI18n::showRightCalendarSidebar();
    appCfg["calendarWeekStartDay"] = MailI18n::calendarWeekStartDay();
    appCfg["calendarUse24HourFormat"] = MailI18n::calendarUse24HourFormat();
    appCfg["calendarShowWeekNumbers"] = MailI18n::calendarShowWeekNumbers();
    appCfg["calendarDefaultDurationMin"] = MailI18n::calendarDefaultDurationMin();
    appCfg["calendarDefaultReminderMin"] = MailI18n::calendarDefaultReminderMin();
    appCfg["enableDesktopNotification"] = MailI18n::enableDesktopNotification();
    appCfg["enableNotificationSound"] = MailI18n::enableNotificationSound();
    appCfg["launchOnWindowsStartup"] = MailI18n::launchOnWindowsStartup();
    appCfg["startMinimizedToTray"] = MailI18n::startMinimizedToTray();
    appCfg["closeToTrayOnX"] = MailI18n::closeToTrayOnX();
    appCfg["minimizeToTrayOnMinimize"] = MailI18n::minimizeToTrayOnMinimize();
    appCfg["startupTrayDefaultsV2Applied"] = true;
    appCfg["startupTrayDefaultsV3Applied"] = true;
    appCfg["showTrayUnreadBadge"] = MailI18n::showTrayUnreadBadge();
    appCfg["enableSingleKeyShortcuts"] = MailI18n::enableSingleKeyShortcuts();
    appCfg["autoSyncIntervalSec"] = MailI18n::autoSyncIntervalSec();
    appCfg["pageFetchSize"] = MailI18n::pageFetchSize();
    appCfg["imapSliceBytes"] = MailI18n::imapSliceBytes();
    appCfg["aiProvider"] = MailI18n::aiProvider();
    appCfg["geminiApiKey"] = MailI18n::geminiApiKey();
    appCfg["geminiModel"] = MailI18n::geminiModel();
    appCfg["codexApiKey"] = MailI18n::codexApiKey();
    appCfg["codexModel"] = MailI18n::codexModel();

    appCfg["privacyMode"] = m_privacySettings.privacyMode;
    appCfg["blockPixelTracking"] = m_privacySettings.blockPixelTracking;
    appCfg["blockSenders"] = m_privacySettings.blockSenders;
    appCfg["trackSentEmails"] = m_privacySettings.trackSentEmails;
    appCfg["blockAllImages"] = m_privacySettings.blockAllImages;
    appCfg["blockAllImagesDefaultFalseApplied"] = true;
    appCfg["downloadAvatars"] = m_privacySettings.downloadAvatars;
    QJsonArray blockedArr;
    for (const QString &bAddr : m_privacySettings.blockedSendersList) {
        const QString cleanAddr = bAddr.trimmed().toLower();
        if (!cleanAddr.isEmpty())
            blockedArr.append(cleanAddr);
    }
    appCfg["blockedSendersList"] = blockedArr;

    QJsonArray repliedArr;
    for (const QString &sig : m_repliedConvSigs) {
        if (!sig.trimmed().isEmpty())
            repliedArr.append(sig);
    }
    appCfg["repliedConvSigs"] = repliedArr;

    QJsonArray forwardedArr;
    for (const QString &sig : m_forwardedConvSigs) {
        if (!sig.trimmed().isEmpty())
            forwardedArr.append(sig);
    }
    appCfg["forwardedConvSigs"] = forwardedArr;

    QJsonArray starredArr;
    for (const QString &sig : m_starredConvSigs) {
        if (!sig.trimmed().isEmpty())
            starredArr.append(sig);
    }
    appCfg["starredConvSigs"] = starredArr;

    QJsonArray unstarredArr;
    for (const QString &sig : m_unstarredConvSigs) {
        if (!sig.trimmed().isEmpty())
            unstarredArr.append(sig);
    }
    appCfg["unstarredConvSigs"] = unstarredArr;

    QJsonArray importantArr;
    for (const QString &sig : m_importantConvSigs) {
        if (!sig.trimmed().isEmpty())
            importantArr.append(sig);
    }
    appCfg["importantConvSigs"] = importantArr;

    QJsonArray unimportantArr;
    for (const QString &sig : m_unimportantConvSigs) {
        if (!sig.trimmed().isEmpty())
            unimportantArr.append(sig);
    }
    appCfg["unimportantConvSigs"] = unimportantArr;

    QJsonArray custFoldersArr;
    QSet<QString> savedCustomIds;
    for (const FolderItem &fd : m_folders) {
        if (!fd.isCustom || fd.id.isEmpty())
            continue;
        if (savedCustomIds.contains(fd.id))
            continue;
        savedCustomIds.insert(fd.id);
        QJsonObject fo;
        fo["id"] = fd.id;
        fo["accountId"] = fd.accountId;
        fo["accountEmail"] = fd.accountEmail;
        fo["name"] = fd.name;
        fo["rawImapMailbox"] = fd.rawImapMailbox;
        fo["dotColor"] = fd.dotColor.name();
        fo["hasChildren"] = fd.hasChildren;
        custFoldersArr.append(fo);
    }
    appCfg["customFolders"] = custFoldersArr;

    QJsonArray rulesArr;
    for (const MailFilterRule &r : MailI18n::filterRules()) {
        QJsonObject ro;
        ro["id"] = r.id;
        ro["name"] = r.name;
        ro["enabled"] = r.enabled;
        ro["matchField"] = r.matchField;
        ro["matchQuery"] = r.matchQuery;
        ro["targetFolderId"] = r.targetFolderId;
        ro["markAsRead"] = r.markAsRead;
        ro["markAsStarred"] = r.markAsStarred;
        ro["accountEmail"] = r.accountEmail;
        rulesArr.append(ro);
    }
    appCfg["filterRules"] = rulesArr;

    QJsonArray snipsArr;
    for (const MailSnippetItem &sn : MailI18n::snippets()) {
        QJsonObject so;
        so["id"] = sn.id;
        so["title"] = sn.title;
        so["shortcutKey"] = sn.shortcutKey;
        so["bodyHtml"] = sn.bodyHtml;
        snipsArr.append(so);
    }
    appCfg["snippets"] = snipsArr;

    QFile cf(resolveAndMigrateConfigFilePath(QStringLiteral("app_settings.json")));
    if (cf.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        cf.write(QJsonDocument(appCfg).toJson(QJsonDocument::Indented));
        cf.close();
    }
}

void MainWindow::loadAccountsFromDisk()
{
    checkAndOfferBackupRestoreOnStartup(this);

    QFile cf(resolveAndMigrateConfigFilePath(QStringLiteral("app_settings.json")));
    if (cf.exists() && cf.open(QIODevice::ReadOnly)) {
        QJsonObject appCfg = QJsonDocument::fromJson(cf.readAll()).object();
        cf.close();
        if (appCfg.contains("language"))
            MailI18n::setLanguageByCode(appCfg.value("language").toString("ko"));
        if (appCfg.contains("theme") && !appCfg.value("theme").toString().trimmed().isEmpty())
            m_currentThemeName = appCfg.value("theme").toString().trimmed();
        if (appCfg.contains("appearanceMode"))
            MailI18n::appearanceMode() = appCfg.value("appearanceMode").toString("System");
        if (appCfg.contains("layoutStyleName")) {
            QString loadedLayout = appCfg.value("layoutStyleName").toString("Comfortable");
            if (loadedLayout.compare("Compact", Qt::CaseInsensitive) == 0)
                loadedLayout = QStringLiteral("Comfortable");
            MailI18n::layoutStyleName() = loadedLayout;
        }
        if (appCfg.contains("mailSortMode"))
            m_mailSortMode = qBound(0, appCfg.value("mailSortMode").toInt(0), 4);
        if (appCfg.contains("menuExpanded"))
            MailI18n::menuExpanded() = appCfg.value("menuExpanded").toBool(true);
        if (appCfg.contains("zoomPercent"))
            MailI18n::zoomPercent() = qBound(75, appCfg.value("zoomPercent").toInt(100), 150);
        if (appCfg.contains("defaultFontFamily"))
            MailI18n::defaultFontFamily() = appCfg.value("defaultFontFamily").toString("Malgun Gothic");
        if (appCfg.contains("defaultFontSize"))
            MailI18n::defaultFontSize() = appCfg.value("defaultFontSize").toInt(10);
        if (appCfg.contains("undoSendDelaySec"))
            MailI18n::undoSendDelaySec() = appCfg.value("undoSendDelaySec").toInt(5);
        if (appCfg.contains("includeQuoteOnReply"))
            MailI18n::includeQuoteOnReply() = appCfg.value("includeQuoteOnReply").toBool(true);
        if (appCfg.contains("autoAppendSignature"))
            MailI18n::autoAppendSignature() = appCfg.value("autoAppendSignature").toBool(true);
        if (appCfg.contains("defaultSignatureHtml"))
            MailI18n::defaultSignatureHtml() = appCfg.value("defaultSignatureHtml").toString();
        if (appCfg.contains("markReadOnSelect"))
            MailI18n::markReadOnSelect() = appCfg.value("markReadOnSelect").toBool(true);
        if (appCfg.contains("groupByConversationThread"))
            MailI18n::groupByConversationThread() = appCfg.value("groupByConversationThread").toBool(true);
        if (appCfg.contains("showRightCalendarSidebar"))
            MailI18n::showRightCalendarSidebar() = appCfg.value("showRightCalendarSidebar").toBool(true);
        if (appCfg.contains("calendarWeekStartDay"))
            MailI18n::calendarWeekStartDay() = appCfg.value("calendarWeekStartDay").toInt(1);
        if (appCfg.contains("calendarUse24HourFormat"))
            MailI18n::calendarUse24HourFormat() = appCfg.value("calendarUse24HourFormat").toBool(false);
        if (appCfg.contains("calendarShowWeekNumbers"))
            MailI18n::calendarShowWeekNumbers() = appCfg.value("calendarShowWeekNumbers").toBool(false);
        if (appCfg.contains("calendarDefaultDurationMin"))
            MailI18n::calendarDefaultDurationMin() = appCfg.value("calendarDefaultDurationMin").toInt(60);
        if (appCfg.contains("calendarDefaultReminderMin"))
            MailI18n::calendarDefaultReminderMin() = appCfg.value("calendarDefaultReminderMin").toInt(15);
        if (appCfg.contains("enableDesktopNotification"))
            MailI18n::enableDesktopNotification() = appCfg.value("enableDesktopNotification").toBool(true);
        if (appCfg.contains("enableNotificationSound"))
            MailI18n::enableNotificationSound() = appCfg.value("enableNotificationSound").toBool(true);
        if (appCfg.value("startupTrayDefaultsV3Applied").toBool(false)) {
            if (appCfg.contains("launchOnWindowsStartup"))
                MailI18n::applyWindowsStartupRegistry(appCfg.value("launchOnWindowsStartup").toBool(true));
            else
                MailI18n::applyWindowsStartupRegistry(true);
            if (appCfg.contains("startMinimizedToTray"))
                MailI18n::startMinimizedToTray() = appCfg.value("startMinimizedToTray").toBool(false);
            else
                MailI18n::startMinimizedToTray() = false;
            if (appCfg.contains("closeToTrayOnX"))
                MailI18n::closeToTrayOnX() = appCfg.value("closeToTrayOnX").toBool(true);
            else
                MailI18n::closeToTrayOnX() = true;
            if (appCfg.contains("minimizeToTrayOnMinimize"))
                MailI18n::minimizeToTrayOnMinimize() = appCfg.value("minimizeToTrayOnMinimize").toBool(false);
            else
                MailI18n::minimizeToTrayOnMinimize() = false;
        } else {
            MailI18n::applyWindowsStartupRegistry(appCfg.value("launchOnWindowsStartup").toBool(true));
            MailI18n::startMinimizedToTray() = false;
            MailI18n::closeToTrayOnX() = appCfg.value("closeToTrayOnX").toBool(true);
            MailI18n::minimizeToTrayOnMinimize() = appCfg.value("minimizeToTrayOnMinimize").toBool(false);
        }
        if (appCfg.contains("showTrayUnreadBadge"))
            MailI18n::showTrayUnreadBadge() = appCfg.value("showTrayUnreadBadge").toBool(true);
        if (appCfg.contains("enableSingleKeyShortcuts"))
            MailI18n::enableSingleKeyShortcuts() = appCfg.value("enableSingleKeyShortcuts").toBool(true);
        if (appCfg.contains("autoSyncIntervalSec"))
            MailI18n::autoSyncIntervalSec() = qBound(0, appCfg.value("autoSyncIntervalSec").toInt(60), 3600);
        if (appCfg.contains("pageFetchSize"))
            MailI18n::pageFetchSize() = qBound(20, appCfg.value("pageFetchSize").toInt(20), 500);
        if (MailI18n::pageFetchSize() != 20)
            MailI18n::pageFetchSize() = 20;
        if (appCfg.contains("imapSliceBytes"))
            MailI18n::imapSliceBytes() = appCfg.value("imapSliceBytes").toInt(65536);
        if (appCfg.contains("aiProvider"))
            MailI18n::aiProvider() = appCfg.value("aiProvider").toString("Gemini");
        if (appCfg.contains("geminiApiKey"))
            MailI18n::geminiApiKey() = appCfg.value("geminiApiKey").toString();
        if (appCfg.contains("geminiModel"))
            MailI18n::geminiModel() = appCfg.value("geminiModel").toString("gemini-2.5-flash");
        if (appCfg.contains("codexApiKey"))
            MailI18n::codexApiKey() = appCfg.value("codexApiKey").toString();
        if (appCfg.contains("codexModel"))
            MailI18n::codexModel() = appCfg.value("codexModel").toString("gpt-5-codex");

        if (appCfg.contains("privacyMode"))
            m_privacySettings.privacyMode = appCfg.value("privacyMode").toBool(true);
        if (appCfg.contains("blockPixelTracking"))
            m_privacySettings.blockPixelTracking = appCfg.value("blockPixelTracking").toBool(true);
        if (appCfg.contains("blockSenders"))
            m_privacySettings.blockSenders = appCfg.value("blockSenders").toBool(true);
        if (appCfg.contains("trackSentEmails"))
            m_privacySettings.trackSentEmails = appCfg.value("trackSentEmails").toBool(true);
        if (appCfg.value("blockAllImagesDefaultFalseApplied").toBool(false) && appCfg.contains("blockAllImages"))
            m_privacySettings.blockAllImages = appCfg.value("blockAllImages").toBool(false);
        else
            m_privacySettings.blockAllImages = false;
        if (appCfg.contains("downloadAvatars"))
            m_privacySettings.downloadAvatars = appCfg.value("downloadAvatars").toBool(true);
        if (appCfg.contains("blockedSendersList")) {
            m_privacySettings.blockedSendersList.clear();
            for (const QJsonValue &bv : appCfg.value("blockedSendersList").toArray()) {
                const QString bAddr = bv.toString().trimmed().toLower();
                if (!bAddr.isEmpty() && !m_privacySettings.blockedSendersList.contains(bAddr, Qt::CaseInsensitive))
                    m_privacySettings.blockedSendersList.append(bAddr);
            }
        }
        MailI18n::privacyTrackSentEmails() = (m_privacySettings.privacyMode && m_privacySettings.trackSentEmails);
        MailI18n::privacyDownloadAvatars() = m_privacySettings.downloadAvatars;

        if (appCfg.contains("repliedConvSigs")) {
            m_repliedConvSigs.clear();
            for (const QJsonValue &rv : appCfg.value("repliedConvSigs").toArray()) {
                const QString s = rv.toString().trimmed();
                if (!s.isEmpty())
                    m_repliedConvSigs.insert(s);
            }
        }
        if (appCfg.contains("forwardedConvSigs")) {
            m_forwardedConvSigs.clear();
            for (const QJsonValue &fv : appCfg.value("forwardedConvSigs").toArray()) {
                const QString s = fv.toString().trimmed();
                if (!s.isEmpty())
                    m_forwardedConvSigs.insert(s);
            }
        }
        if (appCfg.contains("starredConvSigs")) {
            m_starredConvSigs.clear();
            for (const QJsonValue &sv : appCfg.value("starredConvSigs").toArray()) {
                const QString s = sv.toString().trimmed();
                if (!s.isEmpty())
                    m_starredConvSigs.insert(s);
            }
        }
        if (appCfg.contains("unstarredConvSigs")) {
            m_unstarredConvSigs.clear();
            for (const QJsonValue &usv : appCfg.value("unstarredConvSigs").toArray()) {
                const QString s = usv.toString().trimmed();
                if (!s.isEmpty())
                    m_unstarredConvSigs.insert(s);
            }
        }
        if (appCfg.contains("importantConvSigs")) {
            m_importantConvSigs.clear();
            for (const QJsonValue &iv : appCfg.value("importantConvSigs").toArray()) {
                const QString s = iv.toString().trimmed();
                if (!s.isEmpty())
                    m_importantConvSigs.insert(s);
            }
        }
        if (appCfg.contains("unimportantConvSigs")) {
            m_unimportantConvSigs.clear();
            for (const QJsonValue &uiv : appCfg.value("unimportantConvSigs").toArray()) {
                const QString s = uiv.toString().trimmed();
                if (!s.isEmpty())
                    m_unimportantConvSigs.insert(s);
            }
        }

        if (appCfg.contains("customFolders")) {
            for (int i = m_folders.size() - 1; i >= 0; --i) {
                if (m_folders[i].isCustom)
                    m_folders.removeAt(i);
            }
            QSet<QString> loadedCustomIds;
            for (const QJsonValue &fv : appCfg.value("customFolders").toArray()) {
                QJsonObject fo = fv.toObject();
                FolderItem nf;
                nf.id = fo.value("id").toString();
                nf.accountId = fo.value("accountId").toString();
                nf.accountEmail = fo.value("accountEmail").toString();
                nf.name = fo.value("name").toString();
                nf.rawImapMailbox = fo.value("rawImapMailbox").toString();
                nf.kind = FolderKind::CustomFolder;
                nf.isCustom = true;
                nf.hasChildren = fo.value("hasChildren").toBool(false);
                nf.dotColor = QColor(fo.value("dotColor").toString("#4F8EF7"));

                if (nf.rawImapMailbox.isEmpty()
                    && (nf.id == "custom_mailspring" || nf.id == "custom_sent"
                        || nf.id == "custom_temp" || nf.id == "custom_thread")) {
                    continue;
                }
                if (!nf.id.isEmpty() && !nf.name.isEmpty() && !loadedCustomIds.contains(nf.id)) {
                    loadedCustomIds.insert(nf.id);
                    m_folders.append(nf);
                }
            }
        }
        if (appCfg.contains("filterRules")) {
            MailI18n::filterRules().clear();
            for (const QJsonValue &rv : appCfg.value("filterRules").toArray()) {
                QJsonObject ro = rv.toObject();
                MailFilterRule r;
                r.id = ro.value("id").toString();
                r.name = ro.value("name").toString();
                r.enabled = ro.value("enabled").toBool(false);
                r.matchField = ro.value("matchField").toString("From");
                r.matchQuery = ro.value("matchQuery").toString();
                r.targetFolderId = ro.value("targetFolderId").toString("archive");
                r.markAsRead = ro.value("markAsRead").toBool(false);
                r.markAsStarred = ro.value("markAsStarred").toBool(false);
                r.accountEmail = ro.value("accountEmail").toString().trimmed();
                if (r.accountEmail.isEmpty() && r.targetFolderId.startsWith(QStringLiteral("custom_"))) {
                    for (const FolderItem &fd : m_folders) {
                        if (fd.id == r.targetFolderId && !fd.accountEmail.trimmed().isEmpty()) {
                            r.accountEmail = fd.accountEmail.trimmed();
                            break;
                        }
                    }
                }
                MailI18n::filterRules().append(r);
            }
        }
        if (appCfg.contains("snippets")) {
            MailI18n::snippets().clear();
            for (const QJsonValue &sv : appCfg.value("snippets").toArray()) {
                QJsonObject so = sv.toObject();
                MailSnippetItem sn;
                sn.id = so.value("id").toString();
                sn.title = so.value("title").toString();
                sn.shortcutKey = so.value("shortcutKey").toString();
                sn.bodyHtml = so.value("bodyHtml").toString();
                MailI18n::snippets().append(sn);
            }
        }
    } else {
        MailI18n::applyWindowsStartupRegistry(true);
        MailI18n::startMinimizedToTray() = false;
        MailI18n::closeToTrayOnX() = true;
        MailI18n::minimizeToTrayOnMinimize() = false;
    }

    QFile f(resolveAndMigrateConfigFilePath(QStringLiteral("accounts.json")));
    if (!f.exists() || !f.open(QIODevice::ReadOnly))
        return;

    QJsonDocument doc = QJsonDocument::fromJson(f.readAll());
    f.close();
    if (!doc.isArray())
        return;

    QJsonArray arr = doc.array();
    if (arr.isEmpty())
        return;

    AccountItem unified = m_accounts.first();
    m_accounts.clear();
    m_accounts.append(unified);

    for (const QJsonValue &v : arr) {
        QJsonObject o = v.toObject();
        AccountItem acc;
        acc.id = o.value("id").toString();
        acc.email = o.value("email").toString().trimmed();
        acc.provider = AccountProviderType(o.value("provider").toInt(int(AccountProviderType::CustomImap)));
        acc.oauthAccessToken = o.value("oauthAccessToken").toString();
        acc.oauthRefreshToken = o.value("oauthRefreshToken").toString();
        acc.avatarUrl = o.value("avatarUrl").toString();
        acc.avatarPngBase64 = o.value("avatarPngBase64").toString();
        acc.avatarIconPreset = o.value("avatarIconPreset").toInt(0);
        acc.accountLabel = o.value("accountLabel").toString();
        acc.includeInUnified = o.value("includeInUnified").toBool(true);
        acc.calendarEnabled = o.value("calendarEnabled").toBool(true);
        acc.useDifferentReplyTo = o.value("useDifferentReplyTo").toBool(false);
        acc.replyToAddress = o.value("replyToAddress").toString();
        acc.signatureHtml = o.value("signatureHtml").toString();
        for (const QJsonValue &av : o.value("aliases").toArray()) {
            QString al = av.toString().trimmed();
            if (!al.isEmpty())
                acc.aliases.append(al);
        }

        if (acc.email.isEmpty()) {
            continue;
        }

        if (acc.displayName.isEmpty())
            acc.displayName = o.value("displayName").toString().trimmed();
        if (acc.displayName.isEmpty())
            acc.displayName = acc.email.section('@', 0, 0);

        acc.initials = o.value("initials").toString().trimmed();
        if (acc.initials.isEmpty())
            acc.initials = acc.displayName.left(1).toUpper();

        acc.avatarColor = QColor(o.value("avatarColor").toString("#1AA3B8"));
        acc.visualStyle = AvatarVisualStyle(o.value("visualStyle").toInt(int(AvatarVisualStyle::TealSquareS)));
        acc.imapHost = o.value("imapHost").toString().trimmed();
        acc.imapPort = o.value("imapPort").toInt(993);
        acc.imapSecurity = o.value("imapSecurity").toString("SSL/TLS");
        acc.imapUsername = o.value("imapUsername").toString();
        acc.appPassword = o.value("appPassword").toString();
        acc.pop3LeaveCopyOnServer = o.value("pop3LeaveCopyOnServer").toBool(true);
        acc.smtpPreset = o.value("smtpPreset").toString("SameAsIncoming");
        acc.smtpHost = o.value("smtpHost").toString().trimmed();
        acc.smtpPort = o.value("smtpPort").toInt(465);
        acc.smtpSecurity = o.value("smtpSecurity").toString("SSL/TLS");
        acc.useSeparateSmtpAuth = o.value("useSeparateSmtpAuth").toBool(false);
        acc.smtpUsername = o.value("smtpUsername").toString();
        acc.smtpPassword = o.value("smtpPassword").toString();

        if (acc.isPop3()) {
            acc.ensurePop3MailboxDirs();
        }

        if (acc.provider == AccountProviderType::Google || acc.email.endsWith("@gmail.com", Qt::CaseInsensitive)) {
            acc.provider = AccountProviderType::Google;
            if (acc.imapHost.isEmpty())
                acc.imapHost = QStringLiteral("imap.gmail.com");
            if (acc.smtpHost.isEmpty())
                acc.smtpHost = QStringLiteral("smtp.gmail.com");
        }
        m_accounts.append(acc);
    }

    for (int i = m_folders.size() - 1; i >= 0; --i) {
        const FolderItem &f = m_folders.at(i);
        if (!f.isCustom)
            continue;
        bool ownerExists = false;
        bool ownerIsPop3 = false;
        for (const AccountItem &acc : m_accounts) {
            if (acc.provider == AccountProviderType::Unified)
                continue;
            const bool idMatch = (!f.accountId.isEmpty() && acc.id == f.accountId);
            const bool emailMatch = (f.accountId.isEmpty() && !f.accountEmail.isEmpty()
                                     && acc.email.compare(f.accountEmail, Qt::CaseInsensitive) == 0);
            if (idMatch || emailMatch) {
                ownerExists = true;
                if (acc.isPop3())
                    ownerIsPop3 = true;
                break;
            }
        }
        if (!ownerExists || (ownerIsPop3 && !f.rawImapMailbox.isEmpty())) {
            m_folders.removeAt(i);
        }
    }

    for (MailFilterRule &r : MailI18n::filterRules()) {
        if (r.accountEmail.trimmed().isEmpty() && r.targetFolderId.startsWith(QStringLiteral("custom_"))) {
            for (const AccountItem &acc : m_accounts) {
                if (acc.provider == AccountProviderType::Unified)
                    continue;
                if (r.targetFolderId.startsWith(QStringLiteral("custom_") + acc.id + QStringLiteral("_"))) {
                    r.accountEmail = acc.email.trimmed();
                    break;
                }
            }
        }
    }

    bool selectedFound = false;
    for (const AccountItem &acc : m_accounts) {
        if (acc.id == m_selectedAccountId) {
            selectedFound = true;
            break;
        }
    }
    if (!selectedFound && !m_accounts.isEmpty()) {
        m_selectedAccountId = (m_accounts.size() > 1) ? m_accounts[1].id : m_accounts.first().id;
    }
}

void MainWindow::onApplyTheme(const QString &themeName)
{
    m_currentThemeName = themeName;
    m_palette = MailThemeManager::getPalette(themeName);
    const QString globalQss = MailThemeManager::buildGlobalStyleSheet(m_palette);
    setStyleSheet(globalQss);
    if (qApp) {
        qApp->setStyleSheet(globalQss);
    }
    m_leftBarWidget->setStyleSheet(QString("QWidget#leftBar { background-color: %1; }").arg(m_palette.leftBarBg.name()));
    m_folderPaneWidget->setStyleSheet(QString("QWidget#folderPane { background-color: %1; }").arg(m_palette.folderPaneBg.name()));
    refreshFolderSelectionStyles();
    refreshConversationList();
}

void MainWindow::onRefreshAndSyncCurrentAccount()
{
    showMailLoadingToast(QString(), 2000);
    const QString targetFolder = (m_selectedFolderId == QStringLiteral("snoozed") || m_selectedFolderId.isEmpty())
                                     ? QStringLiteral("inbox")
                                     : m_selectedFolderId;
    for (const AccountItem &acc : m_accounts) {
        if (acc.id == m_selectedAccountId || m_selectedAccountId == "acc_unified") {
            if (acc.provider == AccountProviderType::Unified)
                continue;
            m_googleProvider->fetchAccountAvatar(acc);
            const QString effectiveFolder = (acc.isPop3() && (targetFolder == QStringLiteral("starred")
                                                              || targetFolder == QStringLiteral("important")))
                                                ? QStringLiteral("inbox")
                                                : targetFolder;
            const QString folderKey = acc.id + QStringLiteral("|") + effectiveFolder;
            const int totalEx = m_folderTotalExists.value(folderKey, 0);
            int pageEndSeq = 0;
            if (m_currentMailPage > 1 && totalEx > 0) {
                pageEndSeq = qMax(1, totalEx - (m_currentMailPage - 1) * 20);
            }
            m_folderLoadingMore.insert(folderKey);
            m_googleProvider->fetchImapFolder(acc, effectiveFolder, 20, pageEndSeq);
            if (!acc.isPop3() && (effectiveFolder == QStringLiteral("starred")
                                  || effectiveFolder == QStringLiteral("important"))) {
                const QString inbKey = acc.id + QStringLiteral("|inbox");
                m_folderLoadingMore.insert(inbKey);
                m_googleProvider->fetchImapFolder(acc, QStringLiteral("inbox"), 20, 0);
                for (const FolderItem &cf : m_folders) {
                    if (!cf.isCustom || cf.accountId != acc.id)
                        continue;
                    const QString cfKey = acc.id + QStringLiteral("|") + cf.id;
                    m_folderLoadingMore.insert(cfKey);
                    m_googleProvider->fetchImapFolder(acc, cf.id, 20, 0);
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
            if (m_selectedAccountId != "acc_unified")
                break;
        }
    }
    for (const CalendarSubItem &sub : m_calendars) {
        if (!sub.webcalUrl.trimmed().isEmpty()) {
            m_googleProvider->fetchWebcalIcsSubscription(sub.accountEmail, sub.name, sub.webcalUrl, sub.color);
        }
    }
    refreshConversationList();
    refreshFolderSelectionStyles();
    refreshCalendarViews();
    refreshContactsList();
}

void MainWindow::setupSystemTrayAndAutoPoll()
{
    if (!m_trayIcon && QSystemTrayIcon::isSystemTrayAvailable()) {
        m_trayIcon = new QSystemTrayIcon(MailIcons::sollunaAppIcon(0), this);
        m_trayIcon->setToolTip("SollunaMail");

        QMenu *trayMenu = new QMenu(this);
        trayMenu->setStyleSheet(
            "QMenu { background: #FFFFFF; border: 1px solid #E5E7EB; border-radius: 6px; padding: 6px 4px; }"
            "QMenu::item { padding: 7px 18px; font-size: 12px; color: #1F2937; border-radius: 4px; }"
            "QMenu::item:selected { background: #EFF6FF; color: #0D47A1; font-weight: 600; }");

        QAction *actShow = trayMenu->addAction(MailI18n::t("📬 SollunaMail 열기 (Open)", "📬 Open SollunaMail", "📬 SollunaMailを開く"));
        QAction *actCompose = trayMenu->addAction(MailI18n::t("✉️ 새 메일 작성 (Compose)", "✉️ Compose New Email", "✉️ 新規メール作成"));
        QAction *actSync = trayMenu->addAction(MailI18n::t("🔄 지금 새 메일 확인 (Check Mail Now)", "🔄 Check Mail Now", "🔄 今すぐ新着確認"));
        trayMenu->addSeparator();
        QAction *actQuit = trayMenu->addAction(MailI18n::t("🚪 종료 (Quit)", "🚪 Quit", "🚪 終了"));

        connect(actShow, &QAction::triggered, this, [this]() {
            setWindowState((windowState() & ~Qt::WindowMinimized) | Qt::WindowActive);
            showNormal();
            raise();
            activateWindow();
        });
        connect(actCompose, &QAction::triggered, this, [this]() {
            setWindowState((windowState() & ~Qt::WindowMinimized) | Qt::WindowActive);
            showNormal();
            raise();
            activateWindow();
            onComposeNewMail();
        });
        connect(actSync, &QAction::triggered, this, &MainWindow::onRefreshAndSyncCurrentAccount);
        connect(actQuit, &QAction::triggered, this, [this]() {
            m_forceQuit = true;
            QTimer::singleShot(0, this, &QWidget::close);
        });

        m_trayIcon->setContextMenu(trayMenu);

        connect(m_trayIcon, &QSystemTrayIcon::activated, this, [this](QSystemTrayIcon::ActivationReason reason) {
            if (reason == QSystemTrayIcon::Trigger || reason == QSystemTrayIcon::DoubleClick) {
                setWindowState((windowState() & ~Qt::WindowMinimized) | Qt::WindowActive);
                showNormal();
                raise();
                activateWindow();
            }
        });
        connect(m_trayIcon, &QSystemTrayIcon::messageClicked, this, [this]() {
            setWindowState((windowState() & ~Qt::WindowMinimized) | Qt::WindowActive);
            showNormal();
            raise();
            activateWindow();
            m_centerStack->setCurrentIndex(0);

            QString targetConvId = property("lastNotifiedConvId").toString();
            QString targetAccId = property("lastNotifiedAccountId").toString();
            QString targetFolderId = property("lastNotifiedFolderId").toString();
            if (targetFolderId.isEmpty())
                targetFolderId = QStringLiteral("inbox");
            const QString targetConvSig = property("lastNotifiedConvSig").toString();

            if (!targetConvId.isEmpty() || !targetConvSig.isEmpty()) {
                for (const ConversationItem &c : m_conversations) {
                    if ((!targetConvId.isEmpty() && c.id == targetConvId)
                        || (!targetConvSig.isEmpty()
                            && (targetAccId.isEmpty() || c.accountId == targetAccId)
                            && conversationSignatureKey(c) == targetConvSig)) {
                        targetConvId = c.id;
                        targetAccId = c.accountId;
                        if (!c.folderId.isEmpty())
                            targetFolderId = c.folderId;
                        break;
                    }
                }
            }

            if (!m_mailSearchQuery.isEmpty()) {
                m_mailSearchQuery.clear();
                if (m_mailSearchEdit) {
                    QSignalBlocker sb(m_mailSearchEdit);
                    m_mailSearchEdit->clear();
                }
            }
            m_currentMailPage = 1;

            if (!targetAccId.isEmpty()
                && ((m_selectedAccountId != QStringLiteral("acc_unified") && m_selectedAccountId != targetAccId)
                    || (targetFolderId.startsWith(QStringLiteral("custom_")) && m_selectedAccountId != targetAccId))) {
                m_selectedAccountId = targetAccId;
                rebuildCustomFoldersList();
            }

            QString folderDisplayName = QStringLiteral("Inbox");
            for (const FolderItem &f : m_folders) {
                if (f.id == targetFolderId) {
                    folderDisplayName = f.name;
                    break;
                }
            }

            if (!targetConvId.isEmpty()) {
                m_userExplicitlyOpenedConvIds.insert(targetConvId);
            }
            onSelectFolder(targetFolderId, folderDisplayName);

            if (!targetConvId.isEmpty() && m_convListWidget) {
                m_selectedConvId = targetConvId;
                for (int r = 0; r < m_convListWidget->count(); ++r) {
                    QListWidgetItem *it = m_convListWidget->item(r);
                    if (it && it->data(Qt::UserRole + 1).toString() == targetConvId) {
                        m_convListWidget->setCurrentRow(r);
                        m_convListWidget->scrollToItem(it, QAbstractItemView::PositionAtCenter);
                        displayConversation(r, true);
                        break;
                    }
                }
            }
        });

        m_trayIcon->show();
    }

    if (!m_autoSyncTimer) {
        m_autoSyncTimer = new QTimer(this);
        connect(m_autoSyncTimer, &QTimer::timeout, this, [this]() {
            m_initialSyncCompleted = true;
            for (const AccountItem &acc : m_accounts) {
                if (acc.provider == AccountProviderType::Unified)
                    continue;
                if (!acc.appPassword.isEmpty() || !acc.oauthAccessToken.isEmpty()) {
                    const int knownExists = m_folderTotalExists.value(acc.id + "|inbox", -1);
                    m_googleProvider->checkImapInboxQuick(acc, knownExists);
                    m_googleProvider->startImapIdleWatcher(acc);
                }
            }
        });
    }
    applyAutoSyncInterval();
}

void MainWindow::applyAutoSyncInterval()
{
    if (!m_autoSyncTimer)
        return;
    const int sec = MailI18n::autoSyncIntervalSec();
    if (sec <= 0) {
        m_autoSyncTimer->stop();
        if (m_googleProvider)
            m_googleProvider->stopAllImapIdleWatchers();
    } else {
        m_autoSyncTimer->setInterval(sec * 1000);
        if (!m_autoSyncTimer->isActive())
            m_autoSyncTimer->start();
        if (m_googleProvider) {
            for (const AccountItem &acc : m_accounts) {
                if (acc.provider == AccountProviderType::Unified)
                    continue;
                if (!acc.appPassword.isEmpty() || !acc.oauthAccessToken.isEmpty()) {
                    m_googleProvider->startImapIdleWatcher(acc);
                }
            }
        }
    }
}

void MainWindow::updateAppUnreadBadgeIcon()
{
    int unreadInboxCount = 0;
    QSet<QString> seenAppUnreadSigs;
    for (const ConversationItem &c : m_conversations) {
        if (!c.isUnread)
            continue;
        const QString sigKey = conversationSignatureKey(c);
        if (m_readConvSigs.contains(sigKey))
            continue;
        if (c.folderId == QStringLiteral("inbox")
            || c.folderId.startsWith(QStringLiteral("custom_"))) {
            const QString accSig = c.accountId + QStringLiteral("|") + sigKey;
            if (!seenAppUnreadSigs.contains(accSig)) {
                seenAppUnreadSigs.insert(accSig);
                ++unreadInboxCount;
            }
        }
    }

    const int displayBadgeCount = MailI18n::showTrayUnreadBadge() ? unreadInboxCount : 0;
    if (displayBadgeCount == m_lastBadgeUnreadCount)
        return;
    m_lastBadgeUnreadCount = displayBadgeCount;

    QIcon badgeIcon = MailIcons::sollunaAppIcon(displayBadgeCount);
    setWindowIcon(badgeIcon);
    if (qApp) {
        qApp->setWindowIcon(badgeIcon);
    }
    if (m_trayIcon) {
        m_trayIcon->setIcon(badgeIcon);
        if (unreadInboxCount > 0) {
            m_trayIcon->setToolTip(
                MailI18n::t(QString("SollunaMail — 안 읽은 새 메일 %1통").arg(unreadInboxCount),
                            QString("SollunaMail — %1 unread email(s)").arg(unreadInboxCount),
                            QString("SollunaMail — 未読メール %1件").arg(unreadInboxCount)));
        } else {
            m_trayIcon->setToolTip("SollunaMail");
        }
    }
}

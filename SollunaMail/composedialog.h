#ifndef COMPOSEDIALOG_H
#define COMPOSEDIALOG_H

#include <QDialog>
#include <QComboBox>
#include <QLabel>
#include <QLineEdit>
#include <QTextEdit>
#include <QListWidget>
#include <QDateTime>
#include "mailmodels.h"

class ComposeDialog : public QDialog {
    Q_OBJECT
public:
    static constexpr qint64 kMaxAttachmentTotalBytes = 20LL * 1024LL * 1024LL; // 20 MB 제한

    explicit ComposeDialog(const QList<AccountItem> &accounts,
                           const QString &defaultAccountId,
                           const QString &initialTo = QString(),
                           const QString &initialSubject = QString(),
                           const QString &initialBodyHtml = QString(),
                           QWidget *parent = nullptr);

    QString selectedAccountId() const;
    QString recipientTo() const;
    QString recipientCc() const;
    QString recipientBcc() const;
    QString subjectText() const;
    QString bodyHtml() const;
    QString bodyPlain() const;
    QList<AttachmentItem> attachments() const { return m_attachments; }
    bool isDraftSaved() const { return m_savedAsDraft; }
    bool isScheduledSend() const { return m_scheduledSend; }
    bool isHighImportance() const { return m_highImportance; }
    QDateTime scheduledDateTime() const { return m_scheduledDateTime; }
    void restoreUndoSendContent(const QString &cc,
                                const QString &exactBodyHtml,
                                const QList<AttachmentItem> &attachments,
                                const QString &bcc = QString(),
                                bool highImportance = false);
    void setAvailableContacts(const QList<ContactItem> &contacts,
                              const QStringList &customGroups = QStringList());

    static bool promptScheduleDateTimeDialog(QWidget *parent,
                                             const QDateTime &initialDt,
                                             QDateTime &outSelectedDt);

private slots:
    void onBoldClicked();
    void onItalicClicked();
    void onUnderlineClicked();
    void onColorClicked();
    void onAttachFileClicked();
    void onSaveDraftClicked();
    void onSendClicked();
    void onScheduleSendWithTime(const QDateTime &dt);
    void openAddressBookPicker(int defaultTarget = 0); // 0: To, 1: Cc, 2: Bcc
    void runAiComposeAction(const QString &actionKind);

private:
    void appendRecipientsToLineEdit(QLineEdit *edit, const QStringList &formattedRecipients);
    void setupRecipientAutocomplete(QLineEdit *edit);
    QStringList expandGroupOrRecipientToken(const QString &token) const;
    QString expandAllGroupTokensInField(const QString &rawFieldText) const;
    void refreshAttachmentUi();
    qint64 totalAttachmentBytes() const;

    QList<AccountItem> m_accounts;
    QList<ContactItem> m_contacts;
    QStringList m_customGroups;
    QComboBox *m_fromCombo = nullptr;
    QLineEdit *m_toEdit = nullptr;
    QLineEdit *m_ccEdit = nullptr;
    QLineEdit *m_bccEdit = nullptr;
    QLineEdit *m_subjectEdit = nullptr;
    QComboBox *m_fontCombo = nullptr;
    QComboBox *m_sizeCombo = nullptr;
    QTextEdit *m_editor = nullptr;
    QLabel *m_attachmentSummaryLbl = nullptr;
    QListWidget *m_attachmentList = nullptr;
    QList<AttachmentItem> m_attachments;
    class QToolButton *m_btnHighImportance = nullptr;
    bool m_highImportance = false;
    bool m_savedAsDraft = false;
    bool m_scheduledSend = false;
    QDateTime m_scheduledDateTime;
};

#endif // COMPOSEDIALOG_H


#include "composedialog.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFormLayout>
#include <QPushButton>
#include <QToolButton>
#include <QLabel>
#include <QFileDialog>
#include <QFileInfo>
#include <QColorDialog>
#include <QMessageBox>
#include <QTextCharFormat>
#include <QMenu>
#include <QInputDialog>
#include <QNetworkAccessManager>
#include <QNetworkRequest>
#include <QNetworkReply>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QEventLoop>
#include <QDateTimeEdit>
#include <QCalendarWidget>
#include <QCompleter>
#include <QStringListModel>
#include <QAbstractItemView>
#include <QTimer>
#include <QPainter>
#include <QPixmap>
#include <QIcon>
#include <QPointer>
#include <QProcess>
#include <QBuffer>
#include <QImage>
#include <QTextDocument>
#include <QTextCursor>
#include <QRegularExpression>

namespace {
const QString kSigStartMark = QStringLiteral("\u2060\u200B\u2060");
const QString kSigEndMark   = QStringLiteral("\u200B\u2060\u200B");

QString wrapSignatureBlockHtml(const QString &sigHtml)
{
    if (sigHtml.trimmed().isEmpty()) {
        return QStringLiteral("<span>") + kSigStartMark + kSigEndMark + QStringLiteral("</span>");
    }
    return kSigStartMark + sigHtml + kSigEndMark;
}

void registerDataImagesOnDocument(QTextDocument *doc, const QString &html)
{
    if (!doc || html.isEmpty())
        return;
    static const QRegularExpression rxImg(
        R"rx(<img\b[^>]*?\bsrc\s*=\s*["'](data:image/[a-zA-Z0-9.+-]+;base64,([A-Za-z0-9+/=\r\n\s]+))["'][^>]*>)rx",
        QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression rxWidth(R"rx(\bwidth\s*=\s*["']?(\d+))rx", QRegularExpression::CaseInsensitiveOption);
    QRegularExpressionMatchIterator it = rxImg.globalMatch(html);
    while (it.hasNext()) {
        QRegularExpressionMatch m = it.next();
        const QString fullDataUri = m.captured(1);
        QByteArray b64 = m.captured(2).toLatin1();
        b64.replace("\r", "").replace("\n", "").replace(" ", "").replace("\t", "");
        QImage img = QImage::fromData(QByteArray::fromBase64(b64));
        if (!img.isNull()) {
            QRegularExpressionMatch mw = rxWidth.match(m.captured(0));
            if (mw.hasMatch()) {
                const int targetW = mw.captured(1).toInt();
                if (targetW > 0 && img.width() != targetW) {
                    img = img.scaledToWidth(targetW, Qt::SmoothTransformation);
                }
            }
            doc->addResource(QTextDocument::ImageResource, QUrl(fullDataUri), img);
        }
    }
}
} // namespace

ComposeDialog::ComposeDialog(const QList<AccountItem> &accounts,
                             const QString &defaultAccountId,
                             const QString &initialTo,
                             const QString &initialSubject,
                             const QString &initialBodyHtml,
                             QWidget *parent)
    : QDialog(parent), m_accounts(accounts)
{
    setWindowTitle(MailI18n::t("SollunaMail - 새 메시지 작성 (Compose)", "SollunaMail - Compose Message", "SollunaMail - 新規メッセージ作成"));
    resize(1030, 760);
    setMinimumSize(1020, 660);
    setStyleSheet(R"(
        QDialog { background-color: #FFFFFF; }
        QLabel { color: #4B5563; font-size: 12px; font-weight: 600; }
        QLineEdit, QComboBox {
            border: 1px solid #E5E7EB;
            border-radius: 5px;
            padding: 6px 10px;
            background: #FFFFFF;
            color: #111827;
            font-size: 13px;
        }
        QLineEdit:focus { border: 1px solid #0D47A1; }
        QTextEdit {
            border: 1px solid #E5E7EB;
            border-radius: 6px;
            padding: 10px;
            background: #FFFFFF;
            color: #111827;
        }
        QToolButton {
            border: 1px solid transparent;
            border-radius: 4px;
            padding: 4px 8px;
            color: #374151;
            font-weight: 600;
        }
        QToolButton:hover { background-color: #F3F4F6; }
    )");

    QVBoxLayout *root = new QVBoxLayout(this);
    root->setContentsMargins(18, 16, 18, 16);
    root->setSpacing(10);

    QFormLayout *form = new QFormLayout();
    form->setSpacing(8);
    form->setLabelAlignment(Qt::AlignLeft | Qt::AlignVCenter);

    m_fromCombo = new QComboBox(this);
    int defIdx = 0;
    for (int i = 0; i < m_accounts.size(); ++i) {
        if (m_accounts[i].provider == AccountProviderType::Unified)
            continue;
        m_fromCombo->addItem(QString("%1 <%2>").arg(m_accounts[i].displayName, m_accounts[i].email),
                             m_accounts[i].id);
        if (m_accounts[i].id == defaultAccountId)
            defIdx = m_fromCombo->count() - 1;
    }
    m_fromCombo->setCurrentIndex(defIdx);

    m_toEdit = new QLineEdit(initialTo, this);
    m_toEdit->setPlaceholderText(MailI18n::t("이름, 이메일 또는 그룹명 입력 (주소록/그룹 자동완성 지원, 여러 명은 쉼표로 구분)",
                                             "Name, email, or group name (Autocomplete supported, separate multiple with commas)",
                                             "名前、メール、またはグループ名を入力 (自動補完対応)"));

    QPushButton *btnPickTo = new QPushButton(MailI18n::t("📒 주소록", "📒 Contacts", "📒 連絡先"), this);
    btnPickTo->setCursor(Qt::PointingHandCursor);
    btnPickTo->setToolTip(MailI18n::t("연락처/그룹에서 받는 사람(To)을 선택합니다",
                                      "Pick recipients (To) or Group from Contacts",
                                      "連絡先・グループから宛先(To)を選択します"));
    btnPickTo->setStyleSheet(
        "QPushButton { background: #EFF6FF; color: #0D47A1; border: 1px solid #BFDBFE; border-radius: 5px; padding: 6px 12px; font-size: 12px; font-weight: 700; }"
        "QPushButton:hover { background: #DBEAFE; }");
    connect(btnPickTo, &QPushButton::clicked, this, [this]() { openAddressBookPicker(0); });

    QWidget *toRowWidget = new QWidget(this);
    QHBoxLayout *toRowH = new QHBoxLayout(toRowWidget);
    toRowH->setContentsMargins(0, 0, 0, 0);
    toRowH->setSpacing(6);
    toRowH->addWidget(m_toEdit, 1);
    toRowH->addWidget(btnPickTo);

    m_ccEdit = new QLineEdit(this);
    m_ccEdit->setPlaceholderText(MailI18n::t("참조 (Cc) — 이름, 이메일 또는 그룹명 입력", "Cc — Name, email, or group", "CC (参照) — 名前・メール・グループ"));

    QPushButton *btnPickCc = new QPushButton(MailI18n::t("📒 주소록", "📒 Contacts", "📒 連絡先"), this);
    btnPickCc->setCursor(Qt::PointingHandCursor);
    btnPickCc->setToolTip(MailI18n::t("연락처/그룹에서 참조(Cc) 수신자를 선택합니다",
                                      "Pick Cc recipients from Contacts",
                                      "連絡先からCCを選択します"));
    btnPickCc->setStyleSheet(
        "QPushButton { background: #F3F4F6; color: #374151; border: 1px solid #D1D5DB; border-radius: 5px; padding: 6px 12px; font-size: 12px; font-weight: 700; }"
        "QPushButton:hover { background: #E5E7EB; }");
    connect(btnPickCc, &QPushButton::clicked, this, [this]() { openAddressBookPicker(1); });

    QWidget *ccRowWidget = new QWidget(this);
    QHBoxLayout *ccRowH = new QHBoxLayout(ccRowWidget);
    ccRowH->setContentsMargins(0, 0, 0, 0);
    ccRowH->setSpacing(6);
    ccRowH->addWidget(m_ccEdit, 1);
    ccRowH->addWidget(btnPickCc);

    m_bccEdit = new QLineEdit(this);
    m_bccEdit->setPlaceholderText(MailI18n::t("숨은 참조 (Bcc) — 다른 수신자에게 노출되지 않는 수신자 또는 그룹 입력",
                                              "Bcc — Recipients hidden from other recipients",
                                              "BCC (ブラインドカーボンコピー) — 他の受信者に表示されない宛先"));

    QPushButton *btnPickBcc = new QPushButton(MailI18n::t("📒 주소록", "📒 Contacts", "📒 連絡先"), this);
    btnPickBcc->setCursor(Qt::PointingHandCursor);
    btnPickBcc->setToolTip(MailI18n::t("연락처/그룹에서 숨은 참조(Bcc) 수신자를 선택합니다",
                                       "Pick Bcc recipients from Contacts",
                                       "連絡先からBCCを選択します"));
    btnPickBcc->setStyleSheet(
        "QPushButton { background: #F3F4F6; color: #374151; border: 1px solid #D1D5DB; border-radius: 5px; padding: 6px 12px; font-size: 12px; font-weight: 700; }"
        "QPushButton:hover { background: #E5E7EB; }");
    connect(btnPickBcc, &QPushButton::clicked, this, [this]() { openAddressBookPicker(2); });

    QWidget *bccRowWidget = new QWidget(this);
    QHBoxLayout *bccRowH = new QHBoxLayout(bccRowWidget);
    bccRowH->setContentsMargins(0, 0, 0, 0);
    bccRowH->setSpacing(6);
    bccRowH->addWidget(m_bccEdit, 1);
    bccRowH->addWidget(btnPickBcc);

    m_subjectEdit = new QLineEdit(initialSubject, this);
    m_subjectEdit->setPlaceholderText(MailI18n::t("제목을 입력하세요", "Enter subject", "件名を入力してください"));

    form->addRow(MailI18n::t("보내는 사람", "From", "差出人"), m_fromCombo);
    form->addRow(MailI18n::t("받는 사람", "To", "宛先"), toRowWidget);
    form->addRow(MailI18n::t("참조 (Cc)", "Cc", "CC"), ccRowWidget);
    form->addRow(MailI18n::t("숨은 참조 (Bcc)", "Bcc", "BCC"), bccRowWidget);
    form->addRow(MailI18n::t("제목", "Subject", "件名"), m_subjectEdit);
    root->addLayout(form);

    // Formatting toolbar
    QHBoxLayout *fmtBar = new QHBoxLayout();
    fmtBar->setSpacing(4);

    m_fontCombo = new QComboBox(this);
    m_fontCombo->addItems({"Malgun Gothic", "Segoe UI", "Inter", "Arial", "Consolas"});
    m_fontCombo->setCurrentText(MailI18n::defaultFontFamily());
    connect(m_fontCombo, &QComboBox::currentTextChanged, this, [this](const QString &f) {
        m_editor->setFontFamily(f);
    });

    m_sizeCombo = new QComboBox(this);
    m_sizeCombo->addItems({"9", "10", "11", "12", "14", "16", "18"});
    m_sizeCombo->setCurrentText(QString::number(MailI18n::defaultFontSize()));
    connect(m_sizeCombo, &QComboBox::currentTextChanged, this, [this](const QString &sz) {
        m_editor->setFontPointSize(sz.toDouble());
    });

    QToolButton *btnBold = new QToolButton(this);
    btnBold->setText("B");
    connect(btnBold, &QToolButton::clicked, this, &ComposeDialog::onBoldClicked);

    QToolButton *btnItalic = new QToolButton(this);
    btnItalic->setText("I");
    connect(btnItalic, &QToolButton::clicked, this, &ComposeDialog::onItalicClicked);

    QToolButton *btnUnder = new QToolButton(this);
    btnUnder->setText("U");
    connect(btnUnder, &QToolButton::clicked, this, &ComposeDialog::onUnderlineClicked);

    QToolButton *btnColor = new QToolButton(this);
    btnColor->setText(MailI18n::t("색상", "Color", "文字色"));
    connect(btnColor, &QToolButton::clicked, this, &ComposeDialog::onColorClicked);

    QToolButton *btnAttach = new QToolButton(this);
    btnAttach->setText(MailI18n::t("📎 파일 첨부", "📎 Attach File", "📎 ファイル添付"));
    connect(btnAttach, &QToolButton::clicked, this, &ComposeDialog::onAttachFileClicked);

    QToolButton *btnInlineImg = new QToolButton(this);
    btnInlineImg->setText(MailI18n::t("🖼️ 본문 이미지 삽입", "🖼️ Insert Image", "🖼️ 本文画像挿入"));
    btnInlineImg->setCursor(Qt::PointingHandCursor);
    connect(btnInlineImg, &QToolButton::clicked, this, [this]() {
        const QString imgPath = QFileDialog::getOpenFileName(
            this,
            MailI18n::t("본문에 삽입할 이미지 선택", "Select Inline Image", "本文に挿入する画像を選択"),
            QString(),
            MailI18n::t("이미지 파일 (*.png *.jpg *.jpeg *.gif *.webp *.bmp)", "Images (*.png *.jpg *.jpeg *.gif *.webp *.bmp)", "画像 (*.png *.jpg *.jpeg *.gif *.webp *.bmp)"));
        if (imgPath.isEmpty())
            return;
        QImage img(imgPath);
        if (img.isNull())
            return;
        if (img.width() > 640) {
            img = img.scaledToWidth(640, Qt::SmoothTransformation);
        }
        QByteArray pngBytes;
        QBuffer buf(&pngBytes);
        buf.open(QIODevice::WriteOnly);
        img.save(&buf, "PNG");
        const QString dataUri = QString("data:image/png;base64,%1").arg(QString::fromLatin1(pngBytes.toBase64()));
        m_editor->document()->addResource(QTextDocument::ImageResource, QUrl(dataUri), img);
        m_editor->textCursor().insertHtml(
            QString("<img src=\"%1\" width=\"%2\" height=\"%3\" style=\"max-width:100%;height:auto;border:0;\" />")
                .arg(dataUri)
                .arg(img.width())
                .arg(img.height()));
    });

    // Snippets insertion dropdown button
    QToolButton *btnSnippets = new QToolButton(this);
    btnSnippets->setText(MailI18n::t("📋 상용구 삽입 ⌄", "📋 Snippets ⌄", "📋 定型文挿入 ⌄"));
    btnSnippets->setCursor(Qt::PointingHandCursor);
    connect(btnSnippets, &QToolButton::clicked, this, [this, btnSnippets]() {
        QMenu menu(this);
        menu.setStyleSheet(
            "QMenu { background: #FFFFFF; border: 1px solid #D1D5DB; border-radius: 6px; padding: 4px; }"
            "QMenu::item { padding: 7px 16px; font-size: 12.5px; color: #111827; border-radius: 4px; }"
            "QMenu::item:selected { background: #EFF6FF; color: #0D47A1; font-weight: 600; }");
        const QList<MailSnippetItem> &snips = MailI18n::snippets();
        for (const MailSnippetItem &sn : snips) {
            QAction *act = menu.addAction(QString("📋 %1 (%2)").arg(sn.title, sn.shortcutKey));
            connect(act, &QAction::triggered, this, [this, sn]() {
                if (!m_editor)
                    return;
                registerDataImagesOnDocument(m_editor->document(), sn.bodyHtml);
                m_editor->textCursor().insertHtml(sn.bodyHtml);
            });
        }
        menu.exec(btnSnippets->mapToGlobal(QPoint(0, btnSnippets->height() + 2)));
    });

    // Gemini / Codex AI Assistant dropdown button
    QToolButton *btnAi = new QToolButton(this);
    btnAi->setText(QString("✨ %1 AI ⌄").arg(MailI18n::aiProvider()));
    btnAi->setCursor(Qt::PointingHandCursor);
    btnAi->setStyleSheet(
        "QToolButton { background: #EFF6FF; color: #0D47A1; border: 1px solid #BFDBFE; border-radius: 5px; padding: 4px 10px; font-weight: 700; }"
        "QToolButton:hover { background: #DBEAFE; }");
    connect(btnAi, &QToolButton::clicked, this, [this, btnAi]() {
        QMenu menu(this);
        menu.setStyleSheet(
            "QMenu { background: #FFFFFF; border: 1px solid #D1D5DB; border-radius: 6px; padding: 4px; }"
            "QMenu::item { padding: 7px 16px; font-size: 12.5px; color: #111827; border-radius: 4px; }"
            "QMenu::item:selected { background: #EFF6FF; color: #0D47A1; font-weight: 600; }");
        connect(menu.addAction(MailI18n::t("✨ AI 이메일 초안 자동 작성 (Draft Email)", "✨ Draft Email with AI", "✨ AIでメール下書き作成")),
                &QAction::triggered, this, [this]() { runAiComposeAction("draft"); });
        connect(menu.addAction(MailI18n::t("📝 정중한 비즈니스 문체로 다듬기 (Polish Tone)", "📝 Polish Business Tone", "📝 丁寧なビジネス文体に整える")),
                &QAction::triggered, this, [this]() { runAiComposeAction("polish"); });
        menu.addSeparator();
        connect(menu.addAction(MailI18n::t("🌐 영어로 비즈니스 번역 (Translate to English)", "🌐 Translate to English", "🌐 英語に翻訳")),
                &QAction::triggered, this, [this]() { runAiComposeAction("en"); });
        connect(menu.addAction(MailI18n::t("🌐 한국어로 번역 (Translate to Korean)", "🌐 Translate to Korean", "🌐 韓国語に翻訳")),
                &QAction::triggered, this, [this]() { runAiComposeAction("ko"); });
        connect(menu.addAction(MailI18n::t("🌐 일본어로 비즈니스 번역 (Translate to Japanese)", "🌐 Translate to Japanese", "🌐 日本語に翻訳")),
                &QAction::triggered, this, [this]() { runAiComposeAction("ja"); });
        menu.exec(btnAi->mapToGlobal(QPoint(0, btnAi->height() + 2)));
    });

    fmtBar->addWidget(m_fontCombo);
    fmtBar->addWidget(m_sizeCombo);
    fmtBar->addWidget(btnBold);
    fmtBar->addWidget(btnItalic);
    fmtBar->addWidget(btnUnder);
    fmtBar->addWidget(btnColor);
    fmtBar->addWidget(btnAttach);
    fmtBar->addWidget(btnInlineImg);
    fmtBar->addWidget(btnSnippets);
    fmtBar->addWidget(btnAi);

    m_btnHighImportance = new QToolButton(this);
    m_btnHighImportance->setCheckable(true);
    m_btnHighImportance->setCursor(Qt::PointingHandCursor);
    m_btnHighImportance->setText(MailI18n::t("❗ 중요 메일", "❗ High Priority", "❗ 重要メール"));
    m_btnHighImportance->setToolTip(
        MailI18n::t("체크하면 중요(긴급) FLAG 헤더(X-Priority: 1 / Importance: High)를 붙여 발송합니다",
                    "Send with High Importance flag (X-Priority: 1 / Importance: High)",
                    "重要フラグ(X-Priority: 1 / Importance: High)を付けて送信します"));
    m_btnHighImportance->setStyleSheet(
        "QToolButton { background: #FFF1F2; color: #BE123C; border: 1px solid #FECDD3; border-radius: 5px; padding: 4px 10px; font-weight: 700; }"
        "QToolButton:hover { background: #FFE4E6; }"
        "QToolButton:checked { background: #E11D48; color: #FFFFFF; border-color: #BE123C; }");
    connect(m_btnHighImportance, &QToolButton::toggled, this, [this](bool checked) {
        m_highImportance = checked;
    });
    fmtBar->addWidget(m_btnHighImportance);
    fmtBar->addStretch();
    root->addLayout(fmtBar);

    m_editor = new QTextEdit(this);
    QFont edFont(MailI18n::defaultFontFamily(), qMax(8, MailI18n::defaultFontSize()));
    m_editor->setFont(edFont);
    m_editor->setFontFamily(MailI18n::defaultFontFamily());
    m_editor->setFontPointSize(qMax(8, MailI18n::defaultFontSize()));

    auto registerEditorDataImages = [this](const QString &html) {
        if (!m_editor)
            return;
        registerDataImagesOnDocument(m_editor->document(), html);
    };

    auto resolveSignatureForAccount = [this](const QString &accId) -> QString {
        if (!MailI18n::autoAppendSignature())
            return QString();
        for (const AccountItem &acc : m_accounts) {
            if (acc.id == accId && !acc.signatureHtml.trimmed().isEmpty()) {
                return acc.signatureHtml;
            }
        }
        return MailI18n::defaultSignatureHtml();
    };

    m_editor->setProperty("initialBodyHtml", initialBodyHtml);
    m_editor->setProperty("userEdited", false);
    m_editor->setProperty("updatingProgrammatically", true);

    // Build initial body + signature if autoAppendSignature is enabled
    const QString sigHtml = resolveSignatureForAccount(m_fromCombo->currentData().toString());
    m_editor->setProperty("lastAppliedSigHtml", sigHtml);
    m_editor->setPlaceholderText(MailI18n::t("메시지 본문을 입력하세요...", "Write your message here...", "メッセージ本文を入力してください..."));
    if (!initialBodyHtml.isEmpty()) {
        const QString fullInit = "<p><br/></p>" + wrapSignatureBlockHtml(sigHtml) + initialBodyHtml;
        registerEditorDataImages(fullInit);
        m_editor->setHtml(fullInit);
    } else if (!sigHtml.isEmpty()) {
        const QString fullInit = "<p><br/></p>" + wrapSignatureBlockHtml(sigHtml);
        registerEditorDataImages(fullInit);
        m_editor->setHtml(fullInit);
    }
    m_editor->setProperty("updatingProgrammatically", false);

    connect(m_editor, &QTextEdit::textChanged, this, [this]() {
        if (!m_editor || m_editor->property("updatingProgrammatically").toBool())
            return;
        m_editor->setProperty("userEdited", true);
    });

    // 보내는 사람(From) 계정 변경 시 작성 중인 본문을 보존하면서 해당 계정의 서명으로 자동 교체
    connect(m_fromCombo, &QComboBox::currentIndexChanged, this,
            [this, resolveSignatureForAccount, registerEditorDataImages, initialBodyHtml](int) {
        if (!MailI18n::autoAppendSignature() || !m_editor || !m_editor->document())
            return;
        const QString newSig = resolveSignatureForAccount(m_fromCombo->currentData().toString());
        const QString oldSig = m_editor->property("lastAppliedSigHtml").toString();
        if (newSig == oldSig)
            return;

        m_editor->setProperty("updatingProgrammatically", true);
        if (!m_editor->property("userEdited").toBool()) {
            if (!initialBodyHtml.isEmpty()) {
                const QString combined = "<p><br/></p>" + wrapSignatureBlockHtml(newSig) + initialBodyHtml;
                registerEditorDataImages(combined);
                m_editor->setHtml(combined);
            } else if (!newSig.isEmpty()) {
                const QString combined = "<p><br/></p>" + wrapSignatureBlockHtml(newSig);
                registerEditorDataImages(combined);
                m_editor->setHtml(combined);
            } else {
                m_editor->clear();
            }
        } else {
            registerEditorDataImages(newSig);
            QTextDocument *doc = m_editor->document();
            QTextCursor startCur = doc->find(kSigStartMark);
            QTextCursor endCur = startCur.isNull() ? QTextCursor() : doc->find(kSigEndMark, startCur);
            if (!startCur.isNull() && !endCur.isNull()) {
                QTextCursor replaceCur(doc);
                replaceCur.setPosition(startCur.selectionStart());
                replaceCur.setPosition(endCur.selectionEnd(), QTextCursor::KeepAnchor);
                replaceCur.beginEditBlock();
                replaceCur.removeSelectedText();
                replaceCur.insertHtml(wrapSignatureBlockHtml(newSig));
                replaceCur.endEditBlock();
            } else if (!newSig.isEmpty()) {
                QTextCursor tailCur(doc);
                tailCur.movePosition(QTextCursor::End);
                tailCur.insertHtml("<p><br/></p>" + wrapSignatureBlockHtml(newSig));
            }
        }
        m_editor->setProperty("lastAppliedSigHtml", newSig);
        m_editor->setProperty("updatingProgrammatically", false);
    });
    root->addWidget(m_editor, 1);

    m_attachmentSummaryLbl = new QLabel(this);
    m_attachmentSummaryLbl->setStyleSheet("color: #0D47A1; font-size: 11.5px; font-weight: 700; padding: 2px 4px;");
    m_attachmentSummaryLbl->setVisible(false);
    root->addWidget(m_attachmentSummaryLbl);

    m_attachmentList = new QListWidget(this);
    m_attachmentList->setMaximumHeight(68);
    m_attachmentList->setVisible(false);
    m_attachmentList->setToolTip(
        MailI18n::t("첨부파일을 더블클릭하거나 우클릭하면 목록에서 제거할 수 있습니다. (최대 합산 20MB)",
                    "Double-click or right-click an attachment to remove it. (Max total 20MB)",
                    "ダブルクリックまたは右クリックで添付を解除できます。(最大合計20MB)"));
    m_attachmentList->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_attachmentList, &QListWidget::itemDoubleClicked, this, [this](QListWidgetItem *item) {
        if (!item)
            return;
        int row = m_attachmentList->row(item);
        if (row >= 0 && row < m_attachments.size()) {
            m_attachments.removeAt(row);
            refreshAttachmentUi();
        }
    });
    connect(m_attachmentList, &QListWidget::customContextMenuRequested, this, [this](const QPoint &pos) {
        QListWidgetItem *item = m_attachmentList->itemAt(pos);
        if (!item)
            return;
        int row = m_attachmentList->row(item);
        QMenu menu(this);
        QAction *actRemove = menu.addAction(MailI18n::t("❌ 이 첨부파일 제거", "❌ Remove Attachment", "❌ この添付ファイルを削除"));
        QAction *actClearAll = menu.addAction(MailI18n::t("🗑️ 첨부파일 전체 제거", "🗑️ Remove All Attachments", "🗑️ すべての添付を削除"));
        QAction *chosen = menu.exec(m_attachmentList->viewport()->mapToGlobal(pos));
        if (chosen == actRemove && row >= 0 && row < m_attachments.size()) {
            m_attachments.removeAt(row);
            refreshAttachmentUi();
        } else if (chosen == actClearAll) {
            m_attachments.clear();
            refreshAttachmentUi();
        }
    });
    root->addWidget(m_attachmentList);

    // Bottom buttons
    QHBoxLayout *bottomBar = new QHBoxLayout();
    QPushButton *btnSend = new QPushButton(MailI18n::t("보내기 (Send)", "Send", "送信 (Send)"), this);
    btnSend->setCursor(Qt::PointingHandCursor);
    btnSend->setStyleSheet("QPushButton { background-color: #0D47A1; color: #FFFFFF; font-weight: bold; padding: 8px 22px; border-radius: 5px; border: none; }"
                           "QPushButton:hover { background-color: #1558BC; }");
    connect(btnSend, &QPushButton::clicked, this, &ComposeDialog::onSendClicked);

    QPushButton *btnSchedule = new QPushButton(MailI18n::t("⏰ 예약 발송 (Send Later) ⌄", "⏰ Send Later ⌄", "⏰ 予約送信 ⌄"), this);
    btnSchedule->setCursor(Qt::PointingHandCursor);
    btnSchedule->setStyleSheet(
        "QPushButton { background-color: #EFF6FF; color: #0D47A1; font-weight: 700; padding: 8px 16px; border-radius: 5px; border: 1px solid #93C5FD; }"
        "QPushButton:hover { background-color: #DBEAFE; }");
    connect(btnSchedule, &QPushButton::clicked, this, [this, btnSchedule]() {
        if (recipientTo().isEmpty()) {
            QMessageBox::warning(this,
                                 MailI18n::t("받는 사람 필요", "Recipient Required", "宛先が必要です"),
                                 MailI18n::t("예약 발송을 설정하려면 먼저 받는 사람 이메일 주소를 입력해 주세요.",
                                             "Please enter a recipient email address before scheduling.",
                                             "予約送信を設定するには、先に宛先メールアドレスを入力してください。"));
            return;
        }
        QMenu menu(this);
        menu.setStyleSheet(
            "QMenu { background: #FFFFFF; border: 1px solid #D1D5DB; border-radius: 7px; padding: 6px 4px; }"
            "QMenu::item { padding: 8px 18px; font-size: 12.5px; color: #111827; border-radius: 4px; }"
            "QMenu::item:selected { background: #EFF6FF; color: #0D47A1; font-weight: 700; }");

        const QDateTime now = QDateTime::currentDateTime();
        QDateTime in1Hour = now.addSecs(3600);
        in1Hour.setTime(QTime(in1Hour.time().hour(), in1Hour.time().minute(), 0));

        QDateTime todayOrTomorrow6Pm(now.date(), QTime(18, 0));
        if (todayOrTomorrow6Pm <= now.addSecs(300)) {
            todayOrTomorrow6Pm = QDateTime(now.date().addDays(1), QTime(18, 0));
        }

        QDateTime tomorrow9Am(now.date().addDays(1), QTime(9, 0));

        int daysToMon = (8 - now.date().dayOfWeek()) % 7;
        if (daysToMon <= 0)
            daysToMon = 7;
        QDateTime nextMon9Am(now.date().addDays(daysToMon), QTime(9, 0));

        connect(menu.addAction(MailI18n::t(QString("⏰ 1시간 뒤 발송 (%1)").arg(in1Hour.toString("MM/dd HH:mm")),
                                           QString("⏰ In 1 hour (%1)").arg(in1Hour.toString("MM/dd HH:mm")),
                                           QString("⏰ 1時間後に送信 (%1)").arg(in1Hour.toString("MM/dd HH:mm")))),
                &QAction::triggered, this, [this, in1Hour]() { onScheduleSendWithTime(in1Hour); });
        connect(menu.addAction(MailI18n::t(QString("🌇 오후 6:00 발송 (%1)").arg(todayOrTomorrow6Pm.toString("MM/dd HH:mm")),
                                           QString("🌇 6:00 PM (%1)").arg(todayOrTomorrow6Pm.toString("MM/dd HH:mm")),
                                           QString("🌇 午後6:00に送信 (%1)").arg(todayOrTomorrow6Pm.toString("MM/dd HH:mm")))),
                &QAction::triggered, this, [this, todayOrTomorrow6Pm]() { onScheduleSendWithTime(todayOrTomorrow6Pm); });
        connect(menu.addAction(MailI18n::t(QString("🌅 내일 오전 9:00 발송 (%1)").arg(tomorrow9Am.toString("MM/dd HH:mm")),
                                           QString("🌅 Tomorrow 9:00 AM (%1)").arg(tomorrow9Am.toString("MM/dd HH:mm")),
                                           QString("🌅 明日 午前9:00に送信 (%1)").arg(tomorrow9Am.toString("MM/dd HH:mm")))),
                &QAction::triggered, this, [this, tomorrow9Am]() { onScheduleSendWithTime(tomorrow9Am); });
        connect(menu.addAction(MailI18n::t(QString("📅 다음 주 월요일 오전 9:00 발송 (%1)").arg(nextMon9Am.toString("MM/dd HH:mm")),
                                           QString("📅 Next Monday 9:00 AM (%1)").arg(nextMon9Am.toString("MM/dd HH:mm")),
                                           QString("📅 来週月曜 午前9:00に送信 (%1)").arg(nextMon9Am.toString("MM/dd HH:mm")))),
                &QAction::triggered, this, [this, nextMon9Am]() { onScheduleSendWithTime(nextMon9Am); });

        menu.addSeparator();
        connect(menu.addAction(MailI18n::t("🗓️ 날짜 및 시간 직접 선택...", "🗓️ Pick custom date & time...", "🗓️ 日時を直接指定...")),
                &QAction::triggered, this, [this, in1Hour]() {
            QDateTime picked;
            if (promptScheduleDateTimeDialog(this, m_scheduledDateTime.isValid() ? m_scheduledDateTime : in1Hour, picked)) {
                onScheduleSendWithTime(picked);
            }
        });

        menu.exec(btnSchedule->mapToGlobal(QPoint(0, -menu.sizeHint().height() - 4)));
    });

    QPushButton *btnDraft = new QPushButton(MailI18n::t("임시보관함 저장 (Save Draft)", "Save Draft", "下書き保存"), this);
    btnDraft->setCursor(Qt::PointingHandCursor);
    btnDraft->setStyleSheet("QPushButton { background-color: #F3F4F6; color: #374151; padding: 8px 16px; border-radius: 5px; border: 1px solid #D1D5DB; }");
    connect(btnDraft, &QPushButton::clicked, this, &ComposeDialog::onSaveDraftClicked);

    QPushButton *btnCancel = new QPushButton(MailI18n::t("취소", "Cancel", "キャンセル"), this);
    btnCancel->setStyleSheet("QPushButton { background: transparent; color: #6B7280; padding: 8px 12px; border: none; }");
    connect(btnCancel, &QPushButton::clicked, this, &QDialog::reject);

    bottomBar->addWidget(btnSend);
    bottomBar->addWidget(btnSchedule);
    bottomBar->addWidget(btnDraft);
    bottomBar->addStretch();
    bottomBar->addWidget(btnCancel);
    root->addLayout(bottomBar);
}

QString ComposeDialog::selectedAccountId() const
{
    return m_fromCombo->currentData().toString();
}

QString ComposeDialog::recipientTo() const
{
    return expandAllGroupTokensInField(m_toEdit ? m_toEdit->text().trimmed() : QString());
}

QString ComposeDialog::recipientCc() const
{
    return expandAllGroupTokensInField(m_ccEdit ? m_ccEdit->text().trimmed() : QString());
}

QString ComposeDialog::recipientBcc() const
{
    return expandAllGroupTokensInField(m_bccEdit ? m_bccEdit->text().trimmed() : QString());
}

QString ComposeDialog::subjectText() const
{
    return m_subjectEdit->text().trimmed();
}

QString ComposeDialog::bodyHtml() const
{
    QString html = m_editor->toHtml();
    html.remove(kSigStartMark);
    html.remove(kSigEndMark);
    return html;
}

QString ComposeDialog::bodyPlain() const
{
    QString plain = m_editor->toPlainText();
    plain.remove(kSigStartMark);
    plain.remove(kSigEndMark);
    return plain.trimmed();
}

void ComposeDialog::restoreUndoSendContent(const QString &cc,
                                           const QString &exactBodyHtml,
                                           const QList<AttachmentItem> &attachments,
                                           const QString &bcc,
                                           bool highImportance)
{
    if (m_ccEdit) {
        m_ccEdit->setText(cc.trimmed());
    }
    if (m_bccEdit) {
        m_bccEdit->setText(bcc.trimmed());
    }
    if (m_editor && !exactBodyHtml.isEmpty()) {
        m_editor->setProperty("updatingProgrammatically", true);
        registerDataImagesOnDocument(m_editor->document(), exactBodyHtml);
        m_editor->setHtml(exactBodyHtml);
        m_editor->setProperty("userEdited", true);
        m_editor->setProperty("updatingProgrammatically", false);
    }
    m_highImportance = highImportance;
    if (m_btnHighImportance) {
        QSignalBlocker b(m_btnHighImportance);
        m_btnHighImportance->setChecked(highImportance);
    }
    m_attachments = attachments;
    refreshAttachmentUi();
}

void ComposeDialog::setAvailableContacts(const QList<ContactItem> &contacts,
                                         const QStringList &customGroups)
{
    m_contacts = contacts;
    m_customGroups = customGroups;

    // 등록된 계정 이메일도 연락처 후보에 포함(중복 제외)
    for (const AccountItem &acc : m_accounts) {
        if (acc.provider == AccountProviderType::Unified || acc.email.trimmed().isEmpty())
            continue;
        bool exists = false;
        for (const ContactItem &c : m_contacts) {
            if (c.email.compare(acc.email, Qt::CaseInsensitive) == 0) {
                exists = true;
                break;
            }
        }
        if (!exists) {
            ContactItem selfCt;
            selfCt.id = "self_" + acc.id;
            selfCt.displayName = acc.displayName.isEmpty() ? acc.email : acc.displayName;
            selfCt.email = acc.email;
            selfCt.groupName = acc.email;
            m_contacts.append(selfCt);
        }
    }

    setupRecipientAutocomplete(m_toEdit);
    setupRecipientAutocomplete(m_ccEdit);
    setupRecipientAutocomplete(m_bccEdit);
}

static QStringList splitComposeContactGroupNames(const QString &rawGroupName)
{
    QStringList result;
    const QStringList parts = rawGroupName.split(';', Qt::SkipEmptyParts);
    for (const QString &p : parts) {
        const QString t = p.trimmed();
        if (!t.isEmpty() && !result.contains(t, Qt::CaseInsensitive))
            result.append(t);
    }
    return result;
}

static bool composeContactBelongsToGroup(const ContactItem &c, const QString &targetGroup)
{
    const QString target = targetGroup.trimmed();
    if (target.isEmpty() || target.compare(QStringLiteral("All contacts"), Qt::CaseInsensitive) == 0)
        return true;

    const QStringList groups = splitComposeContactGroupNames(c.groupName);
    for (const QString &g : groups) {
        if (g.compare(target, Qt::CaseInsensitive) == 0)
            return true;
    }
    if (!c.accountEmail.trimmed().isEmpty() && c.accountEmail.trimmed().compare(target, Qt::CaseInsensitive) == 0)
        return true;
    return false;
}

QStringList ComposeDialog::expandGroupOrRecipientToken(const QString &token) const
{
    QString trimmed = token.trimmed();
    if (trimmed.isEmpty())
        return {};

    // "👥 [그룹] 그룹명 (N명)" 또는 "[그룹] 그룹명" 또는 그룹명 자체 매칭
    QString candidateGroup = trimmed;
    if (candidateGroup.startsWith("👥")) {
        candidateGroup = candidateGroup.mid(QStringLiteral("👥").length()).trimmed();
    }
    if (candidateGroup.startsWith("[그룹]") || candidateGroup.startsWith("[Group]") || candidateGroup.startsWith("[グループ]")) {
        int closeBr = candidateGroup.indexOf(']');
        if (closeBr >= 0) {
            candidateGroup = candidateGroup.mid(closeBr + 1).trimmed();
        }
        int lastParen = candidateGroup.lastIndexOf('(');
        if (lastParen > 0 && candidateGroup.endsWith(')')) {
            candidateGroup = candidateGroup.left(lastParen).trimmed();
        }
    }

    // 이메일 주소(@ 포함, 단 계정 이메일 형태의 그룹이 아닌 일반 수신자 토큰)는 그대로 반환하되,
    // 그룹 이름으로 등록된 경우에는 그룹 소속 연락처를 전개
    bool isExplicitGroupToken = trimmed.startsWith("👥") || trimmed.startsWith("[그룹]") || trimmed.startsWith("[Group]") || trimmed.startsWith("[グループ]");
    bool matchesCustomGroup = false;
    for (const QString &cg : m_customGroups) {
        if (cg.trimmed().compare(candidateGroup, Qt::CaseInsensitive) == 0) {
            matchesCustomGroup = true;
            break;
        }
    }

    if (isExplicitGroupToken || matchesCustomGroup || !trimmed.contains('@')) {
        QStringList expanded;
        QSet<QString> seenEmails;
        for (const ContactItem &c : m_contacts) {
            const QString email = c.email.trimmed();
            if (email.isEmpty() || !email.contains('@') || seenEmails.contains(email.toLower()))
                continue;
            if (composeContactBelongsToGroup(c, candidateGroup)) {
                seenEmails.insert(email.toLower());
                const QString disp = c.displayName.trimmed();
                if (!disp.isEmpty() && disp.compare(email, Qt::CaseInsensitive) != 0) {
                    expanded.append(QString("%1 <%2>").arg(disp, email));
                } else {
                    expanded.append(email);
                }
            }
        }
        if (!expanded.isEmpty()) {
            return expanded;
        }
    }

    return {trimmed};
}

QString ComposeDialog::expandAllGroupTokensInField(const QString &rawFieldText) const
{
    if (rawFieldText.trimmed().isEmpty())
        return QString();

    const QStringList rawParts = rawFieldText.split(QRegularExpression("[,;]"), Qt::SkipEmptyParts);
    QStringList finalRecipients;
    QSet<QString> seenAddrs;

    for (const QString &part : rawParts) {
        const QStringList expandedList = expandGroupOrRecipientToken(part);
        for (const QString &item : expandedList) {
            QString trimmed = item.trimmed();
            if (trimmed.isEmpty())
                continue;
            QString addrKey = trimmed.toLower();
            if (addrKey.contains('<') && addrKey.contains('>')) {
                addrKey = addrKey.section('<', 1, 1).section('>', 0, 0).trimmed();
            }
            if (seenAddrs.contains(addrKey))
                continue;
            seenAddrs.insert(addrKey);
            finalRecipients.append(trimmed);
        }
    }
    return finalRecipients.join(", ");
}

void ComposeDialog::appendRecipientsToLineEdit(QLineEdit *edit, const QStringList &formattedRecipients)
{
    if (!edit || formattedRecipients.isEmpty())
        return;

    QStringList existingParts = edit->text().split(QRegularExpression("[,;]"), Qt::SkipEmptyParts);
    QStringList cleanedList;
    for (const QString &p : existingParts) {
        for (const QString &exp : expandGroupOrRecipientToken(p)) {
            QString trimmed = exp.trimmed();
            if (!trimmed.isEmpty())
                cleanedList.append(trimmed);
        }
    }

    for (const QString &newRecip : formattedRecipients) {
        for (const QString &exp : expandGroupOrRecipientToken(newRecip)) {
            QString addr = exp.trimmed();
            if (addr.contains('<') && addr.contains('>')) {
                addr = addr.section('<', 1, 1).section('>', 0, 0).trimmed();
            }
            if (addr.isEmpty())
                continue;
            bool alreadyHas = false;
            for (const QString &ex : cleanedList) {
                if (ex.contains(addr, Qt::CaseInsensitive)) {
                    alreadyHas = true;
                    break;
                }
            }
            if (!alreadyHas) {
                cleanedList.append(exp.trimmed());
            }
        }
    }

    edit->setText(cleanedList.join(", "));
}

void ComposeDialog::setupRecipientAutocomplete(QLineEdit *edit)
{
    if (!edit)
        return;

    // 기존에 연결된 QCompleter 자식이 있으면 먼저 시그널 연결 해제 후 지연 삭제하여 중복 시그널 및 댕글링 포인터 방지
    for (QCompleter *old : edit->findChildren<QCompleter *>(QString(), Qt::FindDirectChildrenOnly)) {
        old->disconnect();
        old->setParent(nullptr);
        old->deleteLater();
    }

    QStringList suggestions;

    // 1. 그룹 후보 먼저 추가 (선택 시 그룹 소속 연락처 전체 이메일로 자동 전개됨)
    QStringList allGroupNames;
    for (const QString &cg : m_customGroups) {
        if (!cg.trimmed().isEmpty() && !allGroupNames.contains(cg.trimmed(), Qt::CaseInsensitive))
            allGroupNames.append(cg.trimmed());
    }
    for (const ContactItem &c : m_contacts) {
        QStringList gList = splitComposeContactGroupNames(c.groupName);
        if (gList.isEmpty() && !c.accountEmail.trimmed().isEmpty())
            gList.append(c.accountEmail.trimmed());
        for (const QString &g : gList) {
            if (!g.isEmpty() && !allGroupNames.contains(g, Qt::CaseInsensitive))
                allGroupNames.append(g);
        }
    }

    for (const QString &grpName : allGroupNames) {
        QSet<QString> grpEmails;
        for (const ContactItem &c : m_contacts) {
            const QString email = c.email.trimmed();
            if (email.isEmpty() || !email.contains('@'))
                continue;
            if (composeContactBelongsToGroup(c, grpName)) {
                grpEmails.insert(email.toLower());
            }
        }
        if (!grpEmails.isEmpty()) {
            suggestions.append(MailI18n::t(
                QString("👥 [그룹] %1 (%2명)").arg(grpName).arg(grpEmails.size()),
                QString("👥 [Group] %1 (%2)").arg(grpName).arg(grpEmails.size()),
                QString("👥 [グループ] %1 (%2名)").arg(grpName).arg(grpEmails.size())));
        }
    }

    // 2. 개별 연락처 후보 추가
    QSet<QString> seenEmails;
    for (const ContactItem &c : m_contacts) {
        const QString email = c.email.trimmed();
        if (email.isEmpty() || seenEmails.contains(email.toLower()))
            continue;
        seenEmails.insert(email.toLower());
        const QString name = c.displayName.trimmed();
        if (!name.isEmpty() && name.compare(email, Qt::CaseInsensitive) != 0) {
            suggestions.append(QString("%1 <%2>").arg(name, email));
        } else {
            suggestions.append(email);
        }
    }

    QCompleter *completer = new QCompleter(suggestions, edit);
    completer->setWidget(edit); // 필수: setWidget(edit) 누락 시 completer->complete() 내부에서 widget()->screen() nullptr 역참조 크래시 발생 방지
    completer->setCaseSensitivity(Qt::CaseInsensitive);
    completer->setFilterMode(Qt::MatchContains);
    completer->setCompletionMode(QCompleter::PopupCompletion);
    if (completer->popup()) {
        completer->popup()->setStyleSheet(
            "QListView { background: #FFFFFF; border: 1px solid #93C5FD; border-radius: 6px; padding: 4px; font-size: 12.5px; color: #111827; }"
            "QListView::item { padding: 6px 10px; border-radius: 4px; }"
            "QListView::item:selected { background: #EFF6FF; color: #0D47A1; font-weight: 700; }");
    }

    // 쉼표(,)로 여러 명을 입력할 때도 마지막 토큰을 기준으로 자동완성 팝업 표시
    connect(edit, &QLineEdit::textEdited, completer, [edit, completer](const QString &fullText) {
        if (!completer || !completer->widget())
            return;
        int lastSep = qMax(fullText.lastIndexOf(','), fullText.lastIndexOf(';'));
        QString currentToken = (lastSep >= 0) ? fullText.mid(lastSep + 1).trimmed() : fullText.trimmed();
        if (currentToken.isEmpty()) {
            if (completer->popup())
                completer->popup()->hide();
            return;
        }
        completer->setCompletionPrefix(currentToken);
        if (completer->completionCount() > 0) {
            completer->complete();
        } else if (completer->popup()) {
            completer->popup()->hide();
        }
    });

    connect(completer, QOverload<const QString &>::of(&QCompleter::activated), this, [this, edit](const QString &chosen) {
        QTimer::singleShot(0, edit, [this, edit, chosen]() {
            QString full = edit->text();
            int lastSep = qMax(full.lastIndexOf(','), full.lastIndexOf(';'));
            QString prefix = (lastSep >= 0) ? full.left(lastSep).trimmed() : QString();
            edit->setText(prefix);
            const QStringList expanded = expandGroupOrRecipientToken(chosen);
            appendRecipientsToLineEdit(edit, expanded);
            if (!edit->text().trimmed().isEmpty() && !edit->text().trimmed().endsWith(',')) {
                edit->setText(edit->text().trimmed() + ", ");
            }
            edit->setCursorPosition(edit->text().length());
        });
    });
}

void ComposeDialog::openAddressBookPicker(int defaultTarget)
{
    QDialog dlg(this);
    dlg.setWindowTitle(MailI18n::t("📒 주소록 및 그룹에서 수신자 선택 (Contacts & Groups)",
                                   "📒 Select Recipients or Group from Contacts",
                                   "📒 連絡先・グループから宛先を選択"));
    dlg.resize(620, 520);
    dlg.setStyleSheet(
        "QDialog { background: #FFFFFF; }"
        "QLabel { color: #1F2937; font-size: 12.5px; font-weight: 600; }"
        "QLineEdit, QComboBox { border: 1px solid #D1D5DB; border-radius: 6px; padding: 6px 10px; font-size: 12.5px; color: #111827; background: #FFFFFF; }"
        "QListWidget { border: 1px solid #E5E7EB; border-radius: 7px; background: #FFFFFF; outline: none; }"
        "QListWidget::item { padding: 8px 10px; border-bottom: 1px solid #F3F4F6; color: #111827; font-size: 12.5px; }"
        "QListWidget::item:hover { background: #F8FAFC; }");

    QVBoxLayout *v = new QVBoxLayout(&dlg);
    v->setContentsMargins(18, 16, 18, 16);
    v->setSpacing(10);

    // 상단 필터 바 (그룹 선택 + 그룹 전체 추가 버튼 + 검색창)
    QHBoxLayout *filterRow = new QHBoxLayout();
    filterRow->setSpacing(8);

    QComboBox *groupCombo = new QComboBox(&dlg);
    groupCombo->addItem(MailI18n::t("전체 연락처 (All Contacts)", "All Contacts", "すべての連絡先"), QString());
    QStringList groups;
    for (const QString &cg : m_customGroups) {
        if (!cg.trimmed().isEmpty() && !groups.contains(cg.trimmed(), Qt::CaseInsensitive))
            groups.append(cg.trimmed());
    }
    for (const ContactItem &c : m_contacts) {
        QStringList gList = splitComposeContactGroupNames(c.groupName);
        if (gList.isEmpty() && !c.accountEmail.trimmed().isEmpty())
            gList.append(c.accountEmail.trimmed());
        for (const QString &g : gList) {
            if (!g.isEmpty() && !groups.contains(g, Qt::CaseInsensitive))
                groups.append(g);
        }
    }
    for (const QString &g : groups) {
        int cnt = 0;
        QSet<QString> uniq;
        for (const ContactItem &c : m_contacts) {
            const QString em = c.email.trimmed().toLower();
            if (em.isEmpty() || uniq.contains(em))
                continue;
            if (composeContactBelongsToGroup(c, g)) {
                uniq.insert(em);
                ++cnt;
            }
        }
        groupCombo->addItem(
            MailI18n::t(QString("👥 %1 (%2명)").arg(g).arg(cnt),
                        QString("👥 %1 (%2)").arg(g).arg(cnt),
                        QString("👥 %1 (%2名)").arg(g).arg(cnt)),
            g);
    }

    QPushButton *btnAddWholeGroup = new QPushButton(
        MailI18n::t("👥 선택 그룹 전원 추가", "👥 Add Entire Group", "👥 グループ全員を追加"), &dlg);
    btnAddWholeGroup->setCursor(Qt::PointingHandCursor);
    btnAddWholeGroup->setToolTip(
        MailI18n::t("현재 선택된 그룹에 속한 모든 연락처를 수신자 칸에 한 번에 추가합니다",
                    "Add all contacts in the selected group at once",
                    "選択したグループの連絡先全員を一括追加します"));
    btnAddWholeGroup->setStyleSheet(
        "QPushButton { background: #ECFDF5; color: #047857; border: 1px solid #A7F3D0; border-radius: 6px; padding: 6px 12px; font-size: 12px; font-weight: 700; }"
        "QPushButton:hover { background: #D1FAE5; }");

    QLineEdit *searchEdit = new QLineEdit(&dlg);
    searchEdit->setPlaceholderText(MailI18n::t("🔍 이름, 이메일, 회사명 검색...",
                                               "🔍 Search name, email, company...",
                                               "🔍 名前・メール・会社名を検索..."));
    searchEdit->setClearButtonEnabled(true);

    filterRow->addWidget(groupCombo);
    filterRow->addWidget(btnAddWholeGroup);
    filterRow->addWidget(searchEdit, 1);
    v->addLayout(filterRow);

    // 선택 보조 버튼 행
    QHBoxLayout *selRow = new QHBoxLayout();
    QLabel *hintLbl = new QLabel(
        MailI18n::t("연락처를 클릭(체크)하거나 더블클릭하여 선택하세요:",
                    "Click to check contacts (or double-click) to add:",
                    "連絡先をクリックしてチェックを入れるか、ダブルクリックして追加してください:"),
        &dlg);
    hintLbl->setStyleSheet("color: #4B5563; font-size: 11.5px; font-weight: 600;");

    QPushButton *btnSelectAll = new QPushButton(MailI18n::t("현재 목록 전체 선택", "Select All Shown", "表示中をすべて選択"), &dlg);
    btnSelectAll->setCursor(Qt::PointingHandCursor);
    btnSelectAll->setStyleSheet("QPushButton { background: #F9FAFB; color: #374151; border: 1px solid #D1D5DB; border-radius: 4px; padding: 3px 8px; font-size: 11px; }");

    QPushButton *btnClearAll = new QPushButton(MailI18n::t("선택 해제", "Clear", "選択解除"), &dlg);
    btnClearAll->setCursor(Qt::PointingHandCursor);
    btnClearAll->setStyleSheet("QPushButton { background: #F9FAFB; color: #374151; border: 1px solid #D1D5DB; border-radius: 4px; padding: 3px 8px; font-size: 11px; }");

    selRow->addWidget(hintLbl, 1);
    selRow->addWidget(btnSelectAll);
    selRow->addWidget(btnClearAll);
    v->addLayout(selRow);

    QListWidget *listWidget = new QListWidget(&dlg);
    listWidget->setIconSize(QSize(20, 20));
    listWidget->setSelectionMode(QAbstractItemView::NoSelection);
    v->addWidget(listWidget, 1);

    // Windows 11 QStyleSheetStyle에서 기본 체크박스(:checked)가 흰색/투명으로 가려지는 문제를 원천 차단하기 위해
    // QPainter로 선명한 고해상도 체크박스 아이콘(미선택: 회색 테두리 빈 박스 / 선택됨: 로얄 블루 배경 + 흰색 ✓ 마크) 직접 렌더링
    auto makeCheckIcon = [](bool checked) -> QIcon {
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

    const QIcon iconUnchecked = makeCheckIcon(false);
    const QIcon iconChecked = makeCheckIcon(true);
    QSet<QString> checkedEmails;

    auto updateHintCount = [&]() {
        const int n = checkedEmails.size();
        if (n > 0) {
            hintLbl->setText(MailI18n::t(QString("☑️ 현재 %1명 선택됨 (행을 클릭해 선택/해제, 더블클릭 시 즉시 추가)").arg(n),
                                         QString("☑️ %1 selected (Click row to toggle, double-click to add)").arg(n),
                                         QString("☑️ 現在 %1名選択中 (クリックで切替、ダブルクリックで即追加)").arg(n)));
            hintLbl->setStyleSheet("color: #0D47A1; font-size: 12px; font-weight: 700;");
        } else {
            hintLbl->setText(MailI18n::t("연락처를 클릭(체크)하거나 더블클릭하여 선택하세요:",
                                         "Click to check contacts (or double-click) to add:",
                                         "連絡先をクリックしてチェックを入れるか、ダブルクリックして追加してください:"));
            hintLbl->setStyleSheet("color: #4B5563; font-size: 11.5px; font-weight: 600;");
        }
    };

    auto applyItemVisualState = [&](QListWidgetItem *it, bool isChecked) {
        if (!it)
            return;
        it->setData(Qt::UserRole + 2, isChecked);
        it->setIcon(isChecked ? iconChecked : iconUnchecked);
        const QString em = it->data(Qt::UserRole + 1).toString().trimmed().toLower();
        if (!em.isEmpty()) {
            if (isChecked)
                checkedEmails.insert(em);
            else
                checkedEmails.remove(em);
        }
        QFont f = it->font();
        f.setBold(isChecked);
        it->setFont(f);
        if (isChecked) {
            it->setBackground(QColor("#EFF6FF"));
            it->setForeground(QColor("#0D47A1"));
        } else {
            it->setBackground(QColor("#FFFFFF"));
            it->setForeground(QColor("#111827"));
        }
    };

    auto populateList = [&]() {
        const QString grpFilter = groupCombo->currentData().toString().trimmed();
        const QString q = searchEdit->text().trimmed();

        listWidget->clear();
        QSet<QString> seenEmails;
        for (const ContactItem &c : m_contacts) {
            const QString email = c.email.trimmed();
            if (email.isEmpty() || seenEmails.contains(email.toLower()))
                continue;

            const QString grp = c.groupName.trimmed().isEmpty() ? c.accountEmail.trimmed() : c.groupName.trimmed();
            if (!grpFilter.isEmpty() && !composeContactBelongsToGroup(c, grpFilter)) {
                continue;
            }

            const QString disp = c.displayName.trimmed().isEmpty() ? email : c.displayName.trimmed();
            if (!q.isEmpty()) {
                bool match = disp.contains(q, Qt::CaseInsensitive)
                             || email.contains(q, Qt::CaseInsensitive)
                             || grp.contains(q, Qt::CaseInsensitive)
                             || c.company.contains(q, Qt::CaseInsensitive)
                             || c.jobTitle.contains(q, Qt::CaseInsensitive);
                if (!match)
                    continue;
            }

            seenEmails.insert(email.toLower());
            QString formatted = (disp.compare(email, Qt::CaseInsensitive) != 0)
                                    ? QString("%1 <%2>").arg(disp, email)
                                    : email;
            QString labelText = QString("👤 %1  <%2>").arg(disp, email);
            if (!grp.isEmpty()) {
                labelText += QString("   [%1]").arg(grp);
            }
            if (!c.company.trimmed().isEmpty() || !c.jobTitle.trimmed().isEmpty()) {
                labelText += QString(" · %1 %2").arg(c.company.trimmed(), c.jobTitle.trimmed()).trimmed();
            }

            QListWidgetItem *item = new QListWidgetItem(labelText, listWidget);
            item->setData(Qt::UserRole, formatted);
            item->setData(Qt::UserRole + 1, email);
            const bool isChecked = checkedEmails.contains(email.toLower());
            applyItemVisualState(item, isChecked);
        }
        updateHintCount();
    };

    populateList();

    // 행 어디든 클릭하면 즉시 체크/해제 토글
    connect(listWidget, &QListWidget::itemClicked, &dlg, [&](QListWidgetItem *item) {
        if (!item)
            return;
        const bool nextState = !item->data(Qt::UserRole + 2).toBool();
        applyItemVisualState(item, nextState);
        updateHintCount();
    });

    connect(groupCombo, &QComboBox::currentIndexChanged, &dlg, [populateList]() { populateList(); });
    connect(searchEdit, &QLineEdit::textChanged, &dlg, [populateList]() { populateList(); });

    connect(btnSelectAll, &QPushButton::clicked, &dlg, [&]() {
        for (int i = 0; i < listWidget->count(); ++i) {
            if (QListWidgetItem *it = listWidget->item(i))
                applyItemVisualState(it, true);
        }
        updateHintCount();
    });
    connect(btnClearAll, &QPushButton::clicked, &dlg, [&]() {
        checkedEmails.clear();
        for (int i = 0; i < listWidget->count(); ++i) {
            if (QListWidgetItem *it = listWidget->item(i))
                applyItemVisualState(it, false);
        }
        updateHintCount();
    });

    auto targetLineEdit = [this, defaultTarget]() -> QLineEdit * {
        if (defaultTarget == 1)
            return m_ccEdit;
        if (defaultTarget == 2)
            return m_bccEdit;
        return m_toEdit;
    };

    connect(btnAddWholeGroup, &QPushButton::clicked, &dlg, [this, &dlg, groupCombo, listWidget, targetLineEdit]() {
        QStringList allInView;
        for (int i = 0; i < listWidget->count(); ++i) {
            if (QListWidgetItem *it = listWidget->item(i)) {
                allInView.append(it->data(Qt::UserRole).toString());
            }
        }
        if (allInView.isEmpty()) {
            QMessageBox::information(
                &dlg,
                MailI18n::t("그룹 연락처 없음", "No Contacts in Group", "グループ連絡先なし"),
                MailI18n::t(QString("선택하신 그룹('%1')에 이메일이 등록된 연락처가 없습니다.").arg(groupCombo->currentText()),
                            QString("No contacts with email addresses in group '%1'.").arg(groupCombo->currentText()),
                            QString("選択されたグループ（'%1'）にメールアドレスが登録された連絡先がありません。").arg(groupCombo->currentText())));
            return;
        }
        appendRecipientsToLineEdit(targetLineEdit(), allInView);
        dlg.accept();
    });

    // 더블클릭 시 호출한 입력칸(To / Cc / Bcc)에 즉시 추가
    connect(listWidget, &QListWidget::itemDoubleClicked, &dlg, [this, targetLineEdit, &dlg](QListWidgetItem *item) {
        if (!item)
            return;
        const QString formatted = item->data(Qt::UserRole).toString();
        appendRecipientsToLineEdit(targetLineEdit(), {formatted});
        dlg.accept();
    });

    auto collectCheckedRecipients = [this, listWidget, &checkedEmails]() -> QStringList {
        QStringList chosen;
        QSet<QString> addedEmails;
        // 1. 현재 화면의 체크된 항목 먼저 수집
        for (int i = 0; i < listWidget->count(); ++i) {
            QListWidgetItem *it = listWidget->item(i);
            if (it && it->data(Qt::UserRole + 2).toBool()) {
                chosen.append(it->data(Qt::UserRole).toString());
                addedEmails.insert(it->data(Qt::UserRole + 1).toString().trimmed().toLower());
            }
        }
        // 2. 다른 그룹/검색 필터 상태에서 체크해 두었던 항목도 누락 없이 수집
        for (const ContactItem &c : m_contacts) {
            const QString em = c.email.trimmed();
            const QString emLower = em.toLower();
            if (!emLower.isEmpty() && checkedEmails.contains(emLower) && !addedEmails.contains(emLower)) {
                addedEmails.insert(emLower);
                const QString disp = c.displayName.trimmed().isEmpty() ? em : c.displayName.trimmed();
                chosen.append((disp.compare(em, Qt::CaseInsensitive) != 0) ? QString("%1 <%2>").arg(disp, em) : em);
            }
        }
        return chosen;
    };

    auto btnStyleForTarget = [defaultTarget](int btnTarget) -> QString {
        return (defaultTarget == btnTarget)
                   ? QStringLiteral("QPushButton { background: #0D47A1; color: #FFFFFF; border: none; border-radius: 5px; padding: 8px 16px; font-weight: 700; }")
                   : QStringLiteral("QPushButton { background: #EFF6FF; color: #0D47A1; border: 1px solid #93C5FD; border-radius: 5px; padding: 8px 14px; font-weight: 700; }");
    };

    QHBoxLayout *btnRow = new QHBoxLayout();
    QPushButton *btnAddTo = new QPushButton(
        MailI18n::t("📥 받는 사람(To)에 추가", "📥 Add to 'To'", "📥 宛先(To)に追加"), &dlg);
    btnAddTo->setCursor(Qt::PointingHandCursor);
    btnAddTo->setStyleSheet(btnStyleForTarget(0));

    QPushButton *btnAddCc = new QPushButton(
        MailI18n::t("📋 참조(Cc)에 추가", "📋 Add to 'Cc'", "📋 CCに追加"), &dlg);
    btnAddCc->setCursor(Qt::PointingHandCursor);
    btnAddCc->setStyleSheet(btnStyleForTarget(1));

    QPushButton *btnAddBcc = new QPushButton(
        MailI18n::t("🕵️ 숨은 참조(Bcc)에 추가", "🕵️ Add to 'Bcc'", "🕵️ BCCに追加"), &dlg);
    btnAddBcc->setCursor(Qt::PointingHandCursor);
    btnAddBcc->setStyleSheet(btnStyleForTarget(2));

    QPushButton *btnClose = new QPushButton(MailI18n::t("닫기", "Close", "閉じる"), &dlg);
    btnClose->setStyleSheet(
        "QPushButton { background: #F3F4F6; color: #374151; border: 1px solid #D1D5DB; border-radius: 5px; padding: 8px 14px; font-weight: 600; }");

    connect(btnAddTo, &QPushButton::clicked, &dlg, [this, &dlg, collectCheckedRecipients]() {
        const QStringList sel = collectCheckedRecipients();
        if (sel.isEmpty()) {
            QMessageBox::information(&dlg,
                                     MailI18n::t("연락처 선택", "Select Contacts", "連絡先の選択"),
                                     MailI18n::t("추가할 연락처를 체크하거나 선택해 주세요.",
                                                 "Please check or select contacts to add.",
                                                 "追加する連絡先にチェックを入れるか選択してください。"));
            return;
        }
        appendRecipientsToLineEdit(m_toEdit, sel);
        dlg.accept();
    });

    connect(btnAddCc, &QPushButton::clicked, &dlg, [this, &dlg, collectCheckedRecipients]() {
        const QStringList sel = collectCheckedRecipients();
        if (sel.isEmpty()) {
            QMessageBox::information(&dlg,
                                     MailI18n::t("연락처 선택", "Select Contacts", "連絡先の選択"),
                                     MailI18n::t("추가할 연락처를 체크하거나 선택해 주세요.",
                                                 "Please check or select contacts to add.",
                                                 "追加する連絡先にチェックを入れるか選択してください。"));
            return;
        }
        appendRecipientsToLineEdit(m_ccEdit, sel);
        dlg.accept();
    });

    connect(btnAddBcc, &QPushButton::clicked, &dlg, [this, &dlg, collectCheckedRecipients]() {
        const QStringList sel = collectCheckedRecipients();
        if (sel.isEmpty()) {
            QMessageBox::information(&dlg,
                                     MailI18n::t("연락처 선택", "Select Contacts", "連絡先の選択"),
                                     MailI18n::t("추가할 연락처를 체크하거나 선택해 주세요.",
                                                 "Please check or select contacts to add.",
                                                 "追加する連絡先にチェックを入れるか選択してください。"));
            return;
        }
        appendRecipientsToLineEdit(m_bccEdit, sel);
        dlg.accept();
    });

    connect(btnClose, &QPushButton::clicked, &dlg, &QDialog::reject);

    btnRow->addWidget(btnAddTo);
    btnRow->addWidget(btnAddCc);
    btnRow->addWidget(btnAddBcc);
    btnRow->addStretch();
    btnRow->addWidget(btnClose);
    v->addLayout(btnRow);

    dlg.exec();
}

void ComposeDialog::onBoldClicked()
{
    QTextCharFormat fmt;
    fmt.setFontWeight(m_editor->fontWeight() == QFont::Bold ? QFont::Normal : QFont::Bold);
    m_editor->mergeCurrentCharFormat(fmt);
}

void ComposeDialog::onItalicClicked()
{
    m_editor->setFontItalic(!m_editor->fontItalic());
}

void ComposeDialog::onUnderlineClicked()
{
    m_editor->setFontUnderline(!m_editor->fontUnderline());
}

void ComposeDialog::onColorClicked()
{
    QColor c = QColorDialog::getColor(m_editor->textColor(), this,
                                      MailI18n::t("글자 색상 선택", "Select Text Color", "文字色の選択"));
    if (c.isValid()) {
        m_editor->setTextColor(c);
    }
}

static QString formatHumanFileSize(qint64 bytes)
{
    if (bytes >= 1024LL * 1024LL) {
        return QString("%1 MB").arg(double(bytes) / (1024.0 * 1024.0), 0, 'f', 2);
    }
    if (bytes >= 1024LL) {
        return QString("%1 KB").arg(double(bytes) / 1024.0, 0, 'f', 1);
    }
    return QString("%1 B").arg(qMax<qint64>(0, bytes));
}

qint64 ComposeDialog::totalAttachmentBytes() const
{
    qint64 sum = 0;
    for (const AttachmentItem &att : m_attachments) {
        if (!att.filePath.isEmpty()) {
            QFileInfo fi(att.filePath);
            if (fi.exists())
                sum += fi.size();
        }
    }
    return sum;
}

void ComposeDialog::refreshAttachmentUi()
{
    if (!m_attachmentList)
        return;

    m_attachmentList->clear();
    for (const AttachmentItem &att : m_attachments) {
        m_attachmentList->addItem(
            MailI18n::t(QString("📎 %1 (%2)   — [더블클릭/우클릭: 제거]").arg(att.fileName, att.fileSize),
                        QString("📎 %1 (%2)   — [Double-click to remove]").arg(att.fileName, att.fileSize),
                        QString("📎 %1 (%2)   — [ダブルクリックで削除]").arg(att.fileName, att.fileSize)));
    }

    const bool hasAny = !m_attachments.isEmpty();
    m_attachmentList->setVisible(hasAny);
    if (m_attachmentSummaryLbl) {
        if (hasAny) {
            const qint64 totalBytes = totalAttachmentBytes();
            const double totalMb = double(totalBytes) / (1024.0 * 1024.0);
            m_attachmentSummaryLbl->setText(
                MailI18n::t(QString("📎 첨부파일 %1개 (합계 %2 MB / 최대 20.00 MB)")
                                .arg(m_attachments.size())
                                .arg(totalMb, 0, 'f', 2),
                            QString("📎 %1 attachment(s) (Total %2 MB / Max 20.00 MB)")
                                .arg(m_attachments.size())
                                .arg(totalMb, 0, 'f', 2),
                            QString("📎 添付ファイル %1個 (合計 %2 MB / 最大 20.00 MB)")
                                .arg(m_attachments.size())
                                .arg(totalMb, 0, 'f', 2)));
        }
        m_attachmentSummaryLbl->setVisible(hasAny);
    }
}

void ComposeDialog::onAttachFileClicked()
{
    const QStringList paths = QFileDialog::getOpenFileNames(
        this,
        MailI18n::t("첨부할 파일 선택 (최대 합산 20MB 이하)",
                    "Select Files to Attach (Max 20MB total)",
                    "添付するファイルを選択 (最大合計20MB以下)"));
    if (paths.isEmpty())
        return;

    qint64 currentBytes = totalAttachmentBytes();
    for (const QString &path : paths) {
        if (path.trimmed().isEmpty())
            continue;

        // 중복 첨부 방지
        bool alreadyAdded = false;
        for (const AttachmentItem &existing : m_attachments) {
            if (QFileInfo(existing.filePath).canonicalFilePath() == QFileInfo(path).canonicalFilePath()
                && !existing.filePath.isEmpty()) {
                alreadyAdded = true;
                break;
            }
        }
        if (alreadyAdded)
            continue;

        QFileInfo fi(path);
        if (!fi.exists() || !fi.isFile())
            continue;

        const qint64 fileBytes = fi.size();
        if (currentBytes + fileBytes > kMaxAttachmentTotalBytes) {
            QMessageBox::warning(
                this,
                MailI18n::t("첨부파일 용량 초과 (최대 20MB 제한)",
                            "Attachment Size Limit Exceeded (Max 20MB)",
                            "添付ファイル容量超過 (最大20MB制限)"),
                MailI18n::t(QString("첨부파일 총 용량은 **20MB 이하**로 제한됩니다.\n\n"
                                    "• 선택한 파일: %1 (%2)\n"
                                    "• 현재 첨부 합계: %3\n"
                                    "• 최대 허용 용량: 20.00 MB\n\n"
                                    "20MB를 초과하는 파일은 첨부할 수 없습니다.")
                                .arg(fi.fileName(), formatHumanFileSize(fileBytes), formatHumanFileSize(currentBytes)),
                            QString("Total attachments cannot exceed 20MB.\n\n"
                                    "• Selected file: %1 (%2)\n"
                                    "• Current total: %3 / 20.00 MB")
                                .arg(fi.fileName(), formatHumanFileSize(fileBytes), formatHumanFileSize(currentBytes)),
                            QString("添付ファイルの合計容量は 20MB 以下に制限されています。\n\n"
                                    "• 選択したファイル: %1 (%2)\n"
                                    "• 現在の合計: %3 / 20.00 MB")
                                .arg(fi.fileName(), formatHumanFileSize(fileBytes), formatHumanFileSize(currentBytes))));
            continue;
        }

        currentBytes += fileBytes;
        AttachmentItem att;
        att.fileName = fi.fileName();
        att.fileSize = formatHumanFileSize(fileBytes);
        att.filePath = path;
        m_attachments.append(att);
    }

    refreshAttachmentUi();
}

void ComposeDialog::onSaveDraftClicked()
{
    m_savedAsDraft = true;
    m_scheduledSend = false;
    accept();
}

void ComposeDialog::onSendClicked()
{
    if (recipientTo().isEmpty()) {
        QMessageBox::warning(this,
                             MailI18n::t("받는 사람 필요", "Recipient Required", "宛先が必要です"),
                             MailI18n::t("받는 사람 이메일 주소를 입력해 주세요.",
                                         "Please enter at least one recipient email address.",
                                         "宛先メールアドレスを入力してください。"));
        return;
    }
    m_savedAsDraft = false;
    m_scheduledSend = false;
    accept();
}

void ComposeDialog::onScheduleSendWithTime(const QDateTime &dt)
{
    if (recipientTo().isEmpty()) {
        QMessageBox::warning(this,
                             MailI18n::t("받는 사람 필요", "Recipient Required", "宛先が必要です"),
                             MailI18n::t("받는 사람 이메일 주소를 입력해 주세요.",
                                         "Please enter at least one recipient email address.",
                                         "宛先メールアドレスを入力してください。"));
        return;
    }
    if (!dt.isValid() || dt <= QDateTime::currentDateTime()) {
        QMessageBox::warning(this,
                             MailI18n::t("예약 시각 확인", "Check Schedule Time", "予約時刻の確認"),
                             MailI18n::t("예약 발송 시각은 현재 시각 이후로 지정해 주세요.",
                                         "Scheduled send time must be in the future.",
                                         "予約送信時刻は現在時刻より後に指定してください。"));
        return;
    }
    m_savedAsDraft = false;
    m_scheduledSend = true;
    m_scheduledDateTime = dt;
    accept();
}

bool ComposeDialog::promptScheduleDateTimeDialog(QWidget *parent,
                                                 const QDateTime &initialDt,
                                                 QDateTime &outSelectedDt)
{
    QDialog dlg(parent);
    dlg.setWindowTitle(MailI18n::t("⏰ 예약 발송 날짜 및 시간 설정 (Schedule Send)",
                                   "⏰ Schedule Send Date & Time",
                                   "⏰ 予約送信の日時設定"));
    dlg.setFixedSize(440, 465);
    dlg.setStyleSheet(
        "QDialog { background: #FFFFFF; }"
        "QLabel { color: #1F2937; font-size: 12.5px; font-weight: 600; }"
        "QComboBox {"
        "  border: 1px solid #CBD5E1; border-radius: 6px; padding: 6px 10px;"
        "  font-size: 12.5px; font-weight: 600; color: #0F172A; background: #FFFFFF;"
        "}"
        "QComboBox:hover, QComboBox:focus { border: 1.5px solid #2563EB; background: #F8FAFC; }"
        "QComboBox QAbstractItemView {"
        "  background: #FFFFFF; color: #0F172A; border: 1px solid #CBD5E1;"
        "  selection-background-color: #DBEAFE; selection-color: #1D4ED8; outline: 0;"
        "}");

    QVBoxLayout *v = new QVBoxLayout(&dlg);
    v->setContentsMargins(18, 16, 18, 16);
    v->setSpacing(10);

    QLabel *hdr = new QLabel(
        MailI18n::t("발송할 날짜와 시각을 선택해 주세요:",
                    "Select the date and time to send this email:",
                    "メールを送信する日時を選択してください:"),
        &dlg);
    v->addWidget(hdr);

    QDateTime baseDt = initialDt.isValid() && initialDt > QDateTime::currentDateTime()
                           ? initialDt
                           : QDateTime::currentDateTime().addSecs(3600);

    QCalendarWidget *cal = new QCalendarWidget(&dlg);
    QFont calFont(MailI18n::defaultFontFamily(), 9);
    cal->setFont(calFont);
    cal->setGridVisible(true);
    cal->setMinimumDate(QDate::currentDate());
    cal->setSelectedDate(baseDt.date());
    v->addWidget(cal, 1);

    QHBoxLayout *timeRow = new QHBoxLayout();
    timeRow->setSpacing(6);
    QLabel *timeLbl = new QLabel(MailI18n::t("발송 시각:", "Send Time:", "送信時刻:"), &dlg);
    timeRow->addWidget(timeLbl);

    // 1. 오전 / 오후 선택 콤보박스
    QComboBox *amPmCombo = new QComboBox(&dlg);
    amPmCombo->addItem(MailI18n::t("오전 (AM)", "AM", "午前 (AM)"), 0);
    amPmCombo->addItem(MailI18n::t("오후 (PM)", "PM", "午後 (PM)"), 1);
    amPmCombo->setFixedWidth(96);

    // 2. 24시간(00시 ~ 23시) 선택 콤보박스 (오전/오후 병기)
    QComboBox *hourCombo = new QComboBox(&dlg);
    hourCombo->setMaxVisibleItems(12);
    for (int h = 0; h < 24; ++h) {
        const bool isPm = (h >= 12);
        const int h12 = (h % 12 == 0) ? 12 : (h % 12);
        const QString label = MailI18n::t(
            QString("%1시 (%2 %3시)").arg(h, 2, 10, QChar('0')).arg(isPm ? "오후" : "오전").arg(h12),
            QString("%1:00 (%2 %3)").arg(h, 2, 10, QChar('0')).arg(h12).arg(isPm ? "PM" : "AM"),
            QString("%1時 (%2%3時)").arg(h, 2, 10, QChar('0')).arg(isPm ? "午後" : "午前").arg(h12));
        hourCombo->addItem(label, h);
    }

    // 3. 60분(00분 ~ 59분) 선택 콤보박스
    QComboBox *minCombo = new QComboBox(&dlg);
    minCombo->setMaxVisibleItems(12);
    for (int m = 0; m < 60; ++m) {
        const QString label = MailI18n::t(
            QString("%1분").arg(m, 2, 10, QChar('0')),
            QString("%1 min").arg(m, 2, 10, QChar('0')),
            QString("%1分").arg(m, 2, 10, QChar('0')));
        minCombo->addItem(label, m);
    }
    minCombo->setFixedWidth(84);

    const int initHour = baseDt.time().hour();
    const int initMin = baseDt.time().minute();
    amPmCombo->setCurrentIndex(initHour >= 12 ? 1 : 0);
    hourCombo->setCurrentIndex(initHour);
    minCombo->setCurrentIndex(initMin);

    timeRow->addWidget(amPmCombo);
    timeRow->addWidget(hourCombo, 1);
    timeRow->addWidget(minCombo);
    v->addLayout(timeRow);

    // 선택된 예약 발송 일시 실시간 미리보기 배지
    QLabel *previewLbl = new QLabel(&dlg);
    previewLbl->setStyleSheet(
        "QLabel {"
        "  background: #EFF6FF; color: #1E40AF; border: 1px solid #BFDBFE;"
        "  border-radius: 6px; padding: 7px 10px; font-size: 12px; font-weight: 700;"
        "}");
    v->addWidget(previewLbl);

    auto currentSelectedDateTime = [cal, hourCombo, minCombo]() -> QDateTime {
        const QDate d = cal->selectedDate();
        const int h = qBound(0, hourCombo->currentIndex(), 23);
        const int m = qBound(0, minCombo->currentIndex(), 59);
        return QDateTime(d, QTime(h, m, 0));
    };

    auto updatePreview = [previewLbl, currentSelectedDateTime]() {
        const QDateTime dt = currentSelectedDateTime();
        const int h = dt.time().hour();
        const int m = dt.time().minute();
        const bool isPm = (h >= 12);
        const int h12 = (h % 12 == 0) ? 12 : (h % 12);
        previewLbl->setText(MailI18n::t(
            QString("📅 발송 예정 일시: %1 (%2) %3 %4:%5  [%6:%7]")
                .arg(dt.date().toString("yyyy-MM-dd"))
                .arg(QLocale(QLocale::Korean).dayName(dt.date().dayOfWeek(), QLocale::ShortFormat))
                .arg(isPm ? "오후" : "오전")
                .arg(h12)
                .arg(m, 2, 10, QChar('0'))
                .arg(h, 2, 10, QChar('0'))
                .arg(m, 2, 10, QChar('0')),
            QString("📅 Scheduled for: %1 %2:%3 %4 (%5:%6)")
                .arg(dt.date().toString("yyyy-MM-dd"))
                .arg(h12)
                .arg(m, 2, 10, QChar('0'))
                .arg(isPm ? "PM" : "AM")
                .arg(h, 2, 10, QChar('0'))
                .arg(m, 2, 10, QChar('0')),
            QString("📅 送信予定日時: %1 %2 %3:%4 (%5:%6)")
                .arg(dt.date().toString("yyyy-MM-dd"))
                .arg(isPm ? "午後" : "午前")
                .arg(h12)
                .arg(m, 2, 10, QChar('0'))
                .arg(h, 2, 10, QChar('0'))
                .arg(m, 2, 10, QChar('0'))));
    };

    // 오전/오후 변경 시 24시간 콤보박스 자동 동기화 (오전: 0~11시, 오후: 12~23시)
    QObject::connect(amPmCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), &dlg,
                     [hourCombo, updatePreview](int amPmIdx) {
                         const int curH = qBound(0, hourCombo->currentIndex(), 23);
                         if (amPmIdx == 0 && curH >= 12) {
                             QSignalBlocker blocker(hourCombo);
                             hourCombo->setCurrentIndex(curH - 12);
                         } else if (amPmIdx == 1 && curH < 12) {
                             QSignalBlocker blocker(hourCombo);
                             hourCombo->setCurrentIndex(curH + 12);
                         }
                         updatePreview();
                     });

    // 24시간 콤보박스 변경 시 오전/오후 콤보박스 자동 동기화
    QObject::connect(hourCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), &dlg,
                     [amPmCombo, updatePreview](int hourIdx) {
                         const int targetAmPm = (hourIdx >= 12) ? 1 : 0;
                         if (amPmCombo->currentIndex() != targetAmPm) {
                             QSignalBlocker blocker(amPmCombo);
                             amPmCombo->setCurrentIndex(targetAmPm);
                         }
                         updatePreview();
                     });

    QObject::connect(minCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), &dlg,
                     [updatePreview](int) { updatePreview(); });
    QObject::connect(cal, &QCalendarWidget::selectionChanged, &dlg, updatePreview);

    updatePreview();

    QHBoxLayout *btnRow = new QHBoxLayout();
    btnRow->addStretch();
    QPushButton *btnCancel = new QPushButton(MailI18n::t("취소", "Cancel", "キャンセル"), &dlg);
    btnCancel->setStyleSheet(
        "QPushButton { background: #F3F4F6; color: #374151; border: 1px solid #D1D5DB; border-radius: 5px; padding: 7px 16px; font-weight: 600; }");
    QPushButton *btnOk = new QPushButton(MailI18n::t("⏰ 예약 설정 완료", "⏰ Confirm Schedule", "⏰ 予約設定完了"), &dlg);
    btnOk->setCursor(Qt::PointingHandCursor);
    btnOk->setStyleSheet(
        "QPushButton { background: #0D47A1; color: #FFFFFF; border: none; border-radius: 5px; padding: 7px 18px; font-weight: 700; }"
        "QPushButton:hover { background: #1558BC; }");

    QObject::connect(btnCancel, &QPushButton::clicked, &dlg, &QDialog::reject);
    QObject::connect(btnOk, &QPushButton::clicked, &dlg, [&dlg, currentSelectedDateTime]() {
        if (currentSelectedDateTime() <= QDateTime::currentDateTime()) {
            QMessageBox::warning(&dlg,
                                 MailI18n::t("예약 시각 확인", "Invalid Schedule Time", "予約時刻の確認"),
                                 MailI18n::t("예약 발송 시각은 현재 시각 이후여야 합니다.",
                                             "The scheduled time must be in the future.",
                                             "予約送信時刻は現在時刻より後に設定してください。"));
            return;
        }
        dlg.accept();
    });

    btnRow->addWidget(btnCancel);
    btnRow->addWidget(btnOk);
    v->addLayout(btnRow);

    if (dlg.exec() == QDialog::Accepted) {
        outSelectedDt = currentSelectedDateTime();
        return true;
    }
    return false;
}

void ComposeDialog::runAiComposeAction(const QString &actionKind)
{
    if (property("aiActionRunning").toBool())
        return;
    QPointer<ComposeDialog> self(this);
    struct AiRunningGuard {
        QPointer<ComposeDialog> dlg;
        explicit AiRunningGuard(QPointer<ComposeDialog> d) : dlg(d) {
            if (dlg)
                dlg->setProperty("aiActionRunning", true);
        }
        ~AiRunningGuard() {
            if (dlg)
                dlg->setProperty("aiActionRunning", false);
        }
    } aiGuard(self);

    QString currentText = m_editor->toPlainText().trimmed();
    QString subject = m_subjectEdit->text().trimmed();

    QString prompt;
    if (actionKind == "draft") {
        bool ok = false;
        const QString defaultTopic = MailI18n::t("프로젝트 일정 협의 및 주간 회의 요청",
                                                 "Project schedule discussion and weekly meeting request",
                                                 "プロジェクト日程調整および週次会議のお願い");
        QString topic = QInputDialog::getText(
            this,
            MailI18n::t(QString("✨ %1 AI 이메일 초안 작성").arg(MailI18n::aiProvider()),
                        QString("✨ %1 AI Email Draft").arg(MailI18n::aiProvider()),
                        QString("✨ %1 AI メール下書き作成").arg(MailI18n::aiProvider())),
            MailI18n::t("작성할 이메일 주제 또는 핵심 요점을 입력하세요:", "Enter email topic or key points:", "メールの件名または要点を入力してください:"),
            QLineEdit::Normal,
            subject.isEmpty() ? defaultTopic : subject,
            &ok);
        if (!ok || topic.trimmed().isEmpty())
            return;
        if (m_subjectEdit->text().trimmed().isEmpty())
            m_subjectEdit->setText(topic.trimmed());
        prompt = MailI18n::t(
            QString("Write a polite, concise business email in Korean about: %1").arg(topic.trimmed()),
            QString("Write a polite, concise business email in English about: %1").arg(topic.trimmed()),
            QString("Write a polite, concise business email in Japanese (Keigo) about: %1").arg(topic.trimmed()));
        currentText = topic.trimmed();
    } else if (actionKind == "polish") {
        prompt = MailI18n::t(
            QString("Polish the following email into a polite, professional Korean business tone:\n\n%1").arg(currentText),
            QString("Polish the following email into a polite, professional English business tone:\n\n%1").arg(currentText),
            QString("Polish the following email into a polite, professional Japanese business tone (Keigo):\n\n%1").arg(currentText));
    } else if (actionKind == "en") {
        prompt = QString("Translate the following email into natural, professional business English:\n\n%1").arg(currentText);
    } else if (actionKind == "ko") {
        prompt = QString("Translate the following email into natural, polite business Korean:\n\n%1").arg(currentText);
    } else if (actionKind == "ja") {
        prompt = QString("Translate the following email into polite business Japanese (Keigo):\n\n%1").arg(currentText);
    }

    QString aiResult;
    const bool useCodex = (MailI18n::aiProvider().compare("Codex", Qt::CaseInsensitive) == 0);
    QNetworkAccessManager nam;

    if (!useCodex && !MailI18n::geminiApiKey().trimmed().isEmpty()) {
        QString url = QString("https://generativelanguage.googleapis.com/v1beta/models/%1:generateContent?key=%2")
                          .arg(MailI18n::geminiModel(), MailI18n::geminiApiKey().trimmed());
        QNetworkRequest req{QUrl(url)};
        req.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
        QJsonObject part{{"text", prompt}};
        QJsonObject content{{"parts", QJsonArray{part}}};
        QJsonObject body{{"contents", QJsonArray{content}}};
        QNetworkReply *rep = nam.post(req, QJsonDocument(body).toJson());
        QEventLoop loop;
        QTimer timeoutTimer;
        timeoutTimer.setSingleShot(true);
        connect(&timeoutTimer, &QTimer::timeout, &loop, &QEventLoop::quit);
        connect(rep, &QNetworkReply::finished, &loop, &QEventLoop::quit);
        connect(this, &QDialog::finished, &loop, &QEventLoop::quit);
        connect(this, &QObject::destroyed, &loop, &QEventLoop::quit);
        timeoutTimer.start(15000);
        loop.exec();
        if (!self || !self->isVisible()) {
            if (rep->isRunning())
                rep->abort();
            rep->deleteLater();
            return;
        }
        if (rep->isRunning()) {
            rep->abort();
        } else if (rep->error() == QNetworkReply::NoError) {
            QJsonObject resp = QJsonDocument::fromJson(rep->readAll()).object();
            QJsonArray cands = resp.value("candidates").toArray();
            if (!cands.isEmpty()) {
                QJsonArray parts = cands.first().toObject().value("content").toObject().value("parts").toArray();
                if (!parts.isEmpty())
                    aiResult = parts.first().toObject().value("text").toString().trimmed();
            }
        }
        rep->deleteLater();
    } else if (useCodex && !MailI18n::codexApiKey().trimmed().isEmpty()) {
        QNetworkRequest req{QUrl("https://api.openai.com/v1/chat/completions")};
        req.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
        req.setRawHeader("Authorization", ("Bearer " + MailI18n::codexApiKey().trimmed()).toUtf8());
        QJsonObject msg{{"role", "user"}, {"content", prompt}};
        QJsonObject body{{"model", MailI18n::codexModel()}, {"messages", QJsonArray{msg}}};
        QNetworkReply *rep = nam.post(req, QJsonDocument(body).toJson());
        QEventLoop loop;
        QTimer timeoutTimer;
        timeoutTimer.setSingleShot(true);
        connect(&timeoutTimer, &QTimer::timeout, &loop, &QEventLoop::quit);
        connect(rep, &QNetworkReply::finished, &loop, &QEventLoop::quit);
        connect(this, &QDialog::finished, &loop, &QEventLoop::quit);
        connect(this, &QObject::destroyed, &loop, &QEventLoop::quit);
        timeoutTimer.start(15000);
        loop.exec();
        if (!self || !self->isVisible()) {
            if (rep->isRunning())
                rep->abort();
            rep->deleteLater();
            return;
        }
        if (rep->isRunning()) {
            rep->abort();
        } else if (rep->error() == QNetworkReply::NoError) {
            QJsonObject resp = QJsonDocument::fromJson(rep->readAll()).object();
            QJsonArray choices = resp.value("choices").toArray();
            if (!choices.isEmpty()) {
                aiResult = choices.first().toObject().value("message").toObject().value("content").toString().trimmed();
            }
        }
        rep->deleteLater();
    } else {
        // API 키가 비어 있을 때 로컬 CLI(gemini / codex) 폴백 시도 (최대 10초 제한)
        QProcess cliProc;
        const QString cliCmd = useCodex ? QStringLiteral("codex") : QStringLiteral("gemini");
        const QStringList cliArgs = useCodex
                                        ? QStringList{QStringLiteral("exec"), prompt}
                                        : QStringList{QStringLiteral("-p"), prompt};
        cliProc.start(cliCmd, cliArgs);
        if (cliProc.waitForStarted(1500)) {
            if (cliProc.waitForFinished(10000) && cliProc.exitStatus() == QProcess::NormalExit && cliProc.exitCode() == 0) {
                const QString cliOut = QString::fromUtf8(cliProc.readAllStandardOutput()).trimmed();
                if (!cliOut.isEmpty())
                    aiResult = cliOut;
            } else {
                cliProc.kill();
                cliProc.waitForFinished(500);
            }
        }
        if (!self || !self->isVisible())
            return;
    }

    // Built-in intelligent template fallback when API key is not configured
    if (aiResult.isEmpty()) {
        if (actionKind == "draft") {
            aiResult = MailI18n::t(
                QString("안녕하세요,\n\n"
                        "[%1] 건과 관련하여 연락드립니다.\n"
                        "아래 주요 사항을 검토해 주시고 가능하신 일정을 회신해 주시면 감사하겠습니다.\n\n"
                        "1. 주요 안건: %1\n"
                        "2. 요청 사항: 관련 자료 검토 및 일정 확인\n\n"
                        "감사합니다.").arg(currentText),
                QString("Hello,\n\n"
                        "I am writing to you regarding [%1].\n"
                        "Could you please review the key points below and share your availability?\n\n"
                        "1. Topic: %1\n"
                        "2. Action Requested: Review materials and confirm schedule\n\n"
                        "Best regards,").arg(currentText),
                QString("いつもお世話になっております。\n\n"
                        "「%1」の件につきましてご連絡いたします。\n"
                        "下記の事項をご確認いただき、ご都合の良い日程をご返信いただけますと幸いです。\n\n"
                        "1. 主な議題: %1\n"
                        "2. ご依頼事項: 関連資料のご確認および日程調整\n\n"
                        "よろしくお願いいたします。").arg(currentText));
        } else if (actionKind == "en") {
            aiResult = QString(
                "Dear Team,\n\n"
                "Thank you for your email regarding \"%1\".\n"
                "We have reviewed the details and will follow up with you shortly.\n\n"
                "Best regards,").arg(subject.isEmpty() ? currentText.left(60) : subject);
        } else if (actionKind == "ja") {
            aiResult = QString(
                "いつもお世話になっております。\n\n"
                "「%1」の件につきまして、ご連絡いただきありがとうございます。\n"
                "内容を確認の上、改めてご返信いたします。\n\n"
                "よろしくお願いいたします。").arg(subject.isEmpty() ? currentText.left(60) : subject);
        } else {
            aiResult = MailI18n::t(
                QString("안녕하세요,\n\n"
                        "%1\n\n"
                        "내용 확인 부탁드리며, 추가 문의사항이 있으시면 언제든 말씀해 주십시오.\n"
                        "감사합니다.").arg(currentText),
                QString("Hello,\n\n"
                        "%1\n\n"
                        "Please review the details above and let me know if you have any questions.\n"
                        "Best regards,").arg(currentText),
                QString("いつもお世話になっております。\n\n"
                        "%1\n\n"
                        "上記の内容をご確認いただき、ご不明な点がございましたらお気軽にお知らせください。\n"
                        "よろしくお願いいたします。").arg(currentText));
        }
    }

    if (!self || !self->isVisible() || !m_editor)
        return;

    QString aiHtml = aiResult.toHtmlEscaped();
    aiHtml.replace("\r\n", "\n");
    aiHtml.replace("\n\n", "</p><p>");
    aiHtml.replace("\n", "<br/>");
    aiHtml = "<p>" + aiHtml + "</p>";

    QString sigHtml;
    if (MailI18n::autoAppendSignature() && m_fromCombo) {
        const QString accId = m_fromCombo->currentData().toString();
        for (const AccountItem &acc : m_accounts) {
            if (acc.id == accId && !acc.signatureHtml.trimmed().isEmpty()) {
                sigHtml = acc.signatureHtml;
                break;
            }
        }
        if (sigHtml.isEmpty()) {
            sigHtml = MailI18n::defaultSignatureHtml();
        }
    }
    const QString initQuoteHtml = m_editor->property("initialBodyHtml").toString();
    QString finalAiHtml = aiHtml;
    if (MailI18n::autoAppendSignature()) {
        finalAiHtml += "<p><br/></p>" + wrapSignatureBlockHtml(sigHtml);
    }
    if (!initQuoteHtml.isEmpty()) {
        finalAiHtml += initQuoteHtml;
    }

    m_editor->setProperty("updatingProgrammatically", true);
    registerDataImagesOnDocument(m_editor->document(), finalAiHtml);
    m_editor->setHtml(finalAiHtml);
    m_editor->setProperty("lastAppliedSigHtml", sigHtml);
    m_editor->setProperty("userEdited", true);
    m_editor->setProperty("updatingProgrammatically", false);
}



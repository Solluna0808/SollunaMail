#include "settingsdialog.h"
#include "mailtheme.h"
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QGridLayout>
#include <QFormLayout>
#include <QGroupBox>
#include <QLabel>
#include <QPushButton>
#include <QToolButton>
#include <QScrollArea>
#include <QDesktopServices>
#include <QUrl>
#include <QMessageBox>
#include <QUuid>
#include <QFileDialog>
#include <QColorDialog>
#include <QSlider>
#include <QBuffer>
#include <QMenu>
#include <QPainter>
#include <QPainterPath>
#include <QTextEdit>
#include <QTableWidget>
#include <QHeaderView>
#include <QApplication>
#include <QClipboard>
#include <QCoreApplication>
#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QProcess>
#include <QNetworkAccessManager>
#include <QNetworkRequest>
#include <QNetworkReply>
#include <QAction>
#include <QSslSocket>
#include <QSslConfiguration>
#include <QEventLoop>
#include <QTimer>
#include <QRegularExpression>
#include <QSharedPointer>
#include <QPointer>
#include <QStandardPaths>
#include <QInputDialog>
#include <QTextDocument>
#include <QTextCursor>
#include <QImage>

namespace {
bool waitSocketStep(QSslSocket &sock, bool waitEncrypted, int timeoutMs)
{
    QEventLoop loop;
    QTimer timer;
    timer.setSingleShot(true);
    bool ok = false;
    QObject::connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);
    if (waitEncrypted) {
        if (sock.isEncrypted())
            return true;
        QObject::connect(&sock, &QSslSocket::encrypted, &loop, [&]() {
            ok = true;
            loop.quit();
        });
    } else {
        if (sock.state() == QAbstractSocket::ConnectedState)
            return true;
        QObject::connect(&sock, &QAbstractSocket::connected, &loop, [&]() {
            ok = true;
            loop.quit();
        });
    }
    QObject::connect(&sock, &QAbstractSocket::errorOccurred, &loop, [&](QAbstractSocket::SocketError) {
        loop.quit();
    });
    QObject::connect(&sock, &QSslSocket::sslErrors, &sock, [&](const QList<QSslError> &) {
        sock.ignoreSslErrors();
    });
    timer.start(timeoutMs);
    loop.exec();
    return ok || (waitEncrypted ? sock.isEncrypted() : (sock.state() == QAbstractSocket::ConnectedState));
}

QByteArray readSocketUntil(QSslSocket &sock,
                           const std::function<bool(const QByteArray &)> &predicate,
                           int timeoutMs = 7000)
{
    QByteArray acc;
    if (sock.bytesAvailable() > 0) {
        acc.append(sock.readAll());
        if (predicate(acc))
            return acc;
    }
    QEventLoop loop;
    QTimer timer;
    timer.setSingleShot(true);
    QObject::connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);
    QObject::connect(&sock, &QIODevice::readyRead, &loop, [&]() {
        acc.append(sock.readAll());
        if (predicate(acc))
            loop.quit();
    });
    QObject::connect(&sock, &QAbstractSocket::disconnected, &loop, &QEventLoop::quit);
    QObject::connect(&sock, &QAbstractSocket::errorOccurred, &loop, [&](QAbstractSocket::SocketError) {
        loop.quit();
    });
    timer.start(timeoutMs);
    loop.exec();
    if (sock.bytesAvailable() > 0)
        acc.append(sock.readAll());
    return acc;
}

QString firstServerLine(const QByteArray &raw)
{
    const QList<QByteArray> lines = raw.split('\n');
    for (int i = lines.size() - 1; i >= 0; --i) {
        QString s = QString::fromUtf8(lines[i]).trimmed();
        if (!s.isEmpty())
            return s.left(180);
    }
    return MailI18n::t("(응답 없음)", "(No response)", "（応答なし）");
}

void runMailServerConnectionTest(const AccountItem &testAcc, QWidget *parentWidget, QString *outHtmlSummary = nullptr)
{
    static bool s_isTesting = false;
    if (s_isTesting)
        return;
    struct TestGuard {
        bool cursorActive = true;
        TestGuard() { s_isTesting = true; QApplication::setOverrideCursor(Qt::WaitCursor); }
        void restoreCursorNow() {
            if (cursorActive) {
                QApplication::restoreOverrideCursor();
                cursorActive = false;
            }
        }
        ~TestGuard() {
            restoreCursorNow();
            s_isTesting = false;
        }
    } testGuard;

    QPointer<QWidget> safeParent(parentWidget);

    const bool isPop3 = testAcc.isPop3();
    const QString inProtoName = isPop3 ? QStringLiteral("POP3") : QStringLiteral("IMAP");
    const QString inHost = testAcc.imapHost.trimmed();
    const int inPort = testAcc.imapPort > 0 ? testAcc.imapPort : (isPop3 ? 995 : 993);
    const QString inSec = testAcc.imapSecurity.trimmed().isEmpty() ? QStringLiteral("SSL/TLS") : testAcc.imapSecurity.trimmed();
    const QString inUser = testAcc.effectiveImapUser().trimmed();
    const QString inPass = testAcc.appPassword;

    const QString outHost = testAcc.smtpHost.trimmed();
    const int outPort = testAcc.smtpPort > 0 ? testAcc.smtpPort : 465;
    const QString outSec = testAcc.smtpSecurity.trimmed().isEmpty() ? QStringLiteral("SSL/TLS") : testAcc.smtpSecurity.trimmed();
    const QString outUser = testAcc.effectiveSmtpUser().trimmed();
    const QString outPass = testAcc.effectiveSmtpPassword();

    bool inSuccess = false;
    QString inDetail;
    QString inHint;

    // -------------------------------------------------------------------------
    // 1. 받는 메일 서버 (IMAP / POP3) 접속 및 로그인 시험
    // -------------------------------------------------------------------------
    if (inHost.isEmpty()) {
        inDetail = MailI18n::t("받는 서버 호스트(Host)가 비어 있습니다.",
                               "Incoming server host is empty.",
                               "受信サーバーホストが空です。");
    } else {
        QSslSocket inSock;
        QSslConfiguration sslCfg = QSslConfiguration::defaultConfiguration();
        sslCfg.setPeerVerifyMode(QSslSocket::VerifyNone);
        inSock.setSslConfiguration(sslCfg);

        const bool useDirectSsl = (inSec.contains("SSL", Qt::CaseInsensitive) || inPort == 993 || inPort == 995);
        if (useDirectSsl)
            inSock.connectToHostEncrypted(inHost, static_cast<quint16>(inPort));
        else
            inSock.connectToHost(inHost, static_cast<quint16>(inPort));

        if (!waitSocketStep(inSock, useDirectSsl, 8000)) {
            inDetail = MailI18n::t(
                QString("%1:%2 (%3) 연결 실패 — %4").arg(inHost, QString::number(inPort), inSec, inSock.errorString()),
                QString("%1:%2 (%3) connection failed — %4").arg(inHost, QString::number(inPort), inSec, inSock.errorString()),
                QString("%1:%2 (%3) 接続失敗 — %4").arg(inHost, QString::number(inPort), inSec, inSock.errorString()));
        } else {
            QByteArray banner = readSocketUntil(inSock, [isPop3](const QByteArray &b) {
                return isPop3 ? (b.contains("+OK") || b.contains("-ERR"))
                              : (b.contains("* OK") || b.contains("* PREAUTH") || b.contains("* BYE"));
            }, 6000);

            if (isPop3) {
                if (!banner.contains("+OK")) {
                    inDetail = MailI18n::t(
                        QString("POP3 서버 배너 오류: %1").arg(firstServerLine(banner)),
                        QString("POP3 banner error: %1").arg(firstServerLine(banner)),
                        QString("POP3 サーバーバナーエラー: %1").arg(firstServerLine(banner)));
                } else {
                    if (inSec.contains("STARTTLS", Qt::CaseInsensitive) && !inSock.isEncrypted()) {
                        inSock.write("STLS\r\n");
                        inSock.flush();
                        QByteArray stlsResp = readSocketUntil(inSock, [](const QByteArray &b) {
                            return b.contains("+OK") || b.contains("-ERR");
                        }, 5000);
                        if (stlsResp.contains("+OK")) {
                            inSock.startClientEncryption();
                            waitSocketStep(inSock, true, 6000);
                        }
                    }
                    inSock.write(QStringLiteral("USER %1\r\n").arg(inUser).toUtf8());
                    inSock.flush();
                    QByteArray uResp = readSocketUntil(inSock, [](const QByteArray &b) {
                        return b.contains("+OK") || b.contains("-ERR");
                    }, 5000);

                    if (!uResp.contains("+OK")) {
                        inDetail = MailI18n::t(
                            QString("POP3 USER 거부 (%1): %2").arg(inUser, firstServerLine(uResp)),
                            QString("POP3 USER rejected (%1): %2").arg(inUser, firstServerLine(uResp)),
                            QString("POP3 USER 拒否 (%1): %2").arg(inUser, firstServerLine(uResp)));
                    } else {
                        inSock.write(QStringLiteral("PASS %1\r\n").arg(inPass).toUtf8());
                        inSock.flush();
                        QByteArray pResp = readSocketUntil(inSock, [](const QByteArray &b) {
                            return b.contains("+OK") || b.contains("-ERR");
                        }, 6000);
                        if (pResp.contains("+OK")) {
                            inSuccess = true;
                            inSock.write("STAT\r\n");
                            inSock.flush();
                            QByteArray statResp = readSocketUntil(inSock, [](const QByteArray &b) {
                                return b.contains("+OK") || b.contains("-ERR");
                            }, 5000);
                            QRegularExpression reStat(R"(\+OK\s+(\d+)\s+(\d+))");
                            QRegularExpressionMatch m = reStat.match(QString::fromLatin1(statResp));
                            if (m.hasMatch()) {
                                inDetail = MailI18n::t(
                                    QString("%1:%2 (%3) 로그인 성공! [전체 메일 %4통]").arg(inHost, QString::number(inPort), inSec, m.captured(1)),
                                    QString("%1:%2 (%3) Login OK! [Total %4 messages]").arg(inHost, QString::number(inPort), inSec, m.captured(1)),
                                    QString("%1:%2 (%3) ログイン成功！ [全 %4通]").arg(inHost, QString::number(inPort), inSec, m.captured(1)));
                            } else {
                                inDetail = MailI18n::t(
                                    QString("%1:%2 (%3) 로그인 성공!").arg(inHost, QString::number(inPort), inSec),
                                    QString("%1:%2 (%3) Login OK!").arg(inHost, QString::number(inPort), inSec),
                                    QString("%1:%2 (%3) ログイン成功！").arg(inHost, QString::number(inPort), inSec));
                            }
                            inSock.write("QUIT\r\n");
                            inSock.flush();
                        } else {
                            inDetail = MailI18n::t(
                                QString("POP3 비밀번호 인증 실패 (ID: %1) — %2").arg(inUser, firstServerLine(pResp)),
                                QString("POP3 authentication failed (ID: %1) — %2").arg(inUser, firstServerLine(pResp)),
                                QString("POP3 パスワード認証失敗 (ID: %1) — %2").arg(inUser, firstServerLine(pResp)));
                        }
                    }
                }
            } else {
                // IMAP
                if (!banner.contains("* OK") && !banner.contains("* PREAUTH")) {
                    inDetail = MailI18n::t(
                        QString("IMAP 서버 배너 응답 오류: %1").arg(firstServerLine(banner)),
                        QString("IMAP server banner error: %1").arg(firstServerLine(banner)),
                        QString("IMAP サーバーバナーエラー: %1").arg(firstServerLine(banner)));
                } else {
                    if (inSec.contains("STARTTLS", Qt::CaseInsensitive) && !inSock.isEncrypted()) {
                        inSock.write("A00 STARTTLS\r\n");
                        inSock.flush();
                        QByteArray stlsResp = readSocketUntil(inSock, [](const QByteArray &b) {
                            return b.contains("A00 OK") || b.contains("A00 NO") || b.contains("A00 BAD");
                        }, 5000);
                        if (stlsResp.contains("A00 OK")) {
                            inSock.startClientEncryption();
                            waitSocketStep(inSock, true, 6000);
                        }
                    }

                    const bool useOAuth = (!testAcc.oauthAccessToken.trimmed().isEmpty() && inPass.trimmed().isEmpty());
                    QByteArray authResp;
                    if (useOAuth) {
                        QString authStr = QStringLiteral("user=%1\x01auth=Bearer %2\x01\x01")
                                              .arg(inUser, testAcc.oauthAccessToken.trimmed());
                        inSock.write(QStringLiteral("A01 AUTHENTICATE XOAUTH2 %1\r\n")
                                         .arg(QString::fromLatin1(authStr.toUtf8().toBase64()))
                                         .toUtf8());
                        inSock.flush();
                        authResp = readSocketUntil(inSock, [](const QByteArray &b) {
                            return b.contains("A01 OK") || b.contains("A01 NO") || b.contains("A01 BAD") || b.contains("+ ");
                        }, 7000);
                        if (authResp.contains("+ ")) {
                            inSock.write("\r\n");
                            inSock.flush();
                            authResp.append(readSocketUntil(inSock, [](const QByteArray &b) {
                                return b.contains("A01 OK") || b.contains("A01 NO") || b.contains("A01 BAD");
                            }, 5000));
                        }
                    } else {
                        QString escUser = inUser;
                        escUser.replace("\\", "\\\\").replace("\"", "\\\"");
                        QString escPass = inPass;
                        escPass.replace("\\", "\\\\").replace("\"", "\\\"");
                        inSock.write(QStringLiteral("A01 LOGIN \"%1\" \"%2\"\r\n").arg(escUser, escPass).toUtf8());
                        inSock.flush();
                        authResp = readSocketUntil(inSock, [](const QByteArray &b) {
                            return b.contains("A01 OK") || b.contains("A01 NO") || b.contains("A01 BAD");
                        }, 7000);
                    }

                    if (authResp.contains("A01 OK")) {
                        inSuccess = true;
                    } else if (!useOAuth && inUser.contains('@')) {
                        // 전체 이메일(id@domain)로 실패 시 로컬 아이디(id)로 재시도 진단
                        const QString localPart = inUser.section('@', 0, 0).trimmed();
                        if (!localPart.isEmpty() && inSock.state() == QAbstractSocket::ConnectedState) {
                            QString escLocal = localPart;
                            escLocal.replace("\\", "\\\\").replace("\"", "\\\"");
                            QString escPass = inPass;
                            escPass.replace("\\", "\\\\").replace("\"", "\\\"");
                            inSock.write(QStringLiteral("A01B LOGIN \"%1\" \"%2\"\r\n").arg(escLocal, escPass).toUtf8());
                            inSock.flush();
                            QByteArray retryResp = readSocketUntil(inSock, [](const QByteArray &b) {
                                return b.contains("A01B OK") || b.contains("A01B NO") || b.contains("A01B BAD");
                            }, 6000);
                            if (retryResp.contains("A01B OK")) {
                                inSuccess = true;
                                inHint = MailI18n::t(
                                    QString("💡 아이디를 '%1' 대신 도메인을 뺀 '%2'(으)로 로그인 성공했습니다.").arg(inUser, localPart),
                                    QString("💡 Signed in using local username '%2' instead of '%1'.").arg(inUser, localPart),
                                    QString("💡 '%1' の代わりにローカルID '%2' でログインに成功しました。").arg(inUser, localPart));
                            }
                        }
                    }

                    if (inSuccess) {
                        inSock.write("A02 STATUS \"INBOX\" (MESSAGES UNSEEN)\r\n");
                        inSock.flush();
                        QByteArray statusResp = readSocketUntil(inSock, [](const QByteArray &b) {
                            return b.contains("A02 OK") || b.contains("A02 NO") || b.contains("A02 BAD");
                        }, 6000);
                        QRegularExpression reMsg(R"(MESSAGES\s+(\d+))", QRegularExpression::CaseInsensitiveOption);
                        QRegularExpression reUnseen(R"(UNSEEN\s+(\d+))", QRegularExpression::CaseInsensitiveOption);
                        QRegularExpressionMatch mMsg = reMsg.match(QString::fromLatin1(statusResp));
                        QRegularExpressionMatch mUnseen = reUnseen.match(QString::fromLatin1(statusResp));
                        if (mMsg.hasMatch()) {
                            const QString unseenCnt = mUnseen.hasMatch() ? mUnseen.captured(1) : QStringLiteral("0");
                            inDetail = MailI18n::t(
                                QString("%1:%2 (%3, ID: %4) 접속·인증 성공! [받은편지함 전체 %5통 / 안 읽음 %6통]")
                                    .arg(inHost, QString::number(inPort), inSec, inUser, mMsg.captured(1), unseenCnt),
                                QString("%1:%2 (%3, ID: %4) Connected & Authenticated! [Inbox %5 total / %6 unread]")
                                    .arg(inHost, QString::number(inPort), inSec, inUser, mMsg.captured(1), unseenCnt),
                                QString("%1:%2 (%3, ID: %4) 接続・認証成功！ [受信トレイ 全%5通 / 未読%6通]")
                                    .arg(inHost, QString::number(inPort), inSec, inUser, mMsg.captured(1), unseenCnt));
                        } else {
                            inDetail = MailI18n::t(
                                QString("%1:%2 (%3, ID: %4) 접속·인증 성공!").arg(inHost, QString::number(inPort), inSec, inUser),
                                QString("%1:%2 (%3, ID: %4) Connected & Authenticated!").arg(inHost, QString::number(inPort), inSec, inUser),
                                QString("%1:%2 (%3, ID: %4) 接続・認証成功！").arg(inHost, QString::number(inPort), inSec, inUser));
                        }
                        inSock.write("A03 LOGOUT\r\n");
                        inSock.flush();
                    } else {
                        inDetail = MailI18n::t(
                            QString("%1:%2 (%3, ID: %4) 로그인 실패 — 서버 응답: %5").arg(inHost, QString::number(inPort), inSec, inUser, firstServerLine(authResp)),
                            QString("%1:%2 (%3, ID: %4) Login failed — Server response: %5").arg(inHost, QString::number(inPort), inSec, inUser, firstServerLine(authResp)),
                            QString("%1:%2 (%3, ID: %4) ログイン失敗 — サーバー応答: %5").arg(inHost, QString::number(inPort), inSec, inUser, firstServerLine(authResp)));
                        if (inHost.contains("daum.net", Qt::CaseInsensitive) || inHost.contains("kakao.com", Qt::CaseInsensitive)) {
                            inHint = MailI18n::t(
                                "💡 다음/카카오 메일 안내: ① 다음 메일 웹 [환경설정 → IMAP/POP3 설정]에서 'IMAP/SMTP 사용함' 설정 여부 확인, ② 카카오 계정 2단계 인증 사용 시 일반 비밀번호가 아닌 전용 '앱 비밀번호(16자리)'를 입력해야 합니다.",
                                "💡 Daum/Kakao Mail Tip: Enable IMAP/SMTP in Daum Mail web settings, and use a 16-digit Kakao App Password if 2FA is enabled.",
                                "💡 Daum/Kakaoメール案内: Web設定でIMAP/SMTPを有効にし、2段階認証時は16桁のアプリパスワードを使用してください。");
                        } else if (inHost.contains("naver.com", Qt::CaseInsensitive)) {
                            inHint = MailI18n::t(
                                "💡 네이버 메일 안내: ① 네이버 메일 웹 [환경설정 → POP3/IMAP 설정]에서 'IMAP/SMTP 사용함' 확인, ② 2단계 인증 사용 시 네이버 '애플리케이션 비밀번호'를 입력해야 합니다.",
                                "💡 Naver Mail Tip: Enable POP3/IMAP in Naver Mail web settings, and use a Naver Application Password if 2FA is enabled.",
                                "💡 Naverメール案内: Web設定でPOP3/IMAPを有効にし、2段階認証時はアプリパスワードを使用してください。");
                        } else if (inHost.contains("gmail.com", Qt::CaseInsensitive)) {
                            inHint = MailI18n::t(
                                "💡 Gmail 안내: Google OAuth 로그인 또는 Google 16자리 '앱 비밀번호'가 필요합니다.",
                                "💡 Gmail Tip: Use Google OAuth sign-in or a 16-digit Google App Password.",
                                "💡 Gmail案内: Google OAuthログインまたは16桁のアプリパスワードが必要です。");
                        }
                    }
                }
            }
            inSock.disconnectFromHost();
        }
    }

    // -------------------------------------------------------------------------
    // 2. 보내는 메일 서버 (SMTP) 접속 및 인증 시험
    // -------------------------------------------------------------------------
    bool outSuccess = false;
    QString outDetail;
    if (outHost.isEmpty()) {
        outDetail = MailI18n::t("보내는 서버(SMTP) 호스트가 비어 있습니다.",
                                "Outgoing SMTP server host is empty.",
                                "送信(SMTP)サーバーホストが空です。");
    } else {
        QSslSocket outSock;
        QSslConfiguration sslCfg = QSslConfiguration::defaultConfiguration();
        sslCfg.setPeerVerifyMode(QSslSocket::VerifyNone);
        outSock.setSslConfiguration(sslCfg);

        const bool useDirectSsl = (outSec.contains("SSL", Qt::CaseInsensitive) || outPort == 465);
        if (useDirectSsl)
            outSock.connectToHostEncrypted(outHost, static_cast<quint16>(outPort));
        else
            outSock.connectToHost(outHost, static_cast<quint16>(outPort));

        if (!waitSocketStep(outSock, useDirectSsl, 8000)) {
            outDetail = MailI18n::t(
                QString("%1:%2 (%3) 연결 실패 — %4").arg(outHost, QString::number(outPort), outSec, outSock.errorString()),
                QString("%1:%2 (%3) connection failed — %4").arg(outHost, QString::number(outPort), outSec, outSock.errorString()),
                QString("%1:%2 (%3) 接続失敗 — %4").arg(outHost, QString::number(outPort), outSec, outSock.errorString()));
        } else {
            QByteArray banner = readSocketUntil(outSock, [](const QByteArray &b) {
                return b.contains("220 ") || b.contains("421 ") || b.contains("554 ");
            }, 6000);

            if (!banner.contains("220")) {
                outDetail = MailI18n::t(
                    QString("SMTP 서버 배너 오류: %1").arg(firstServerLine(banner)),
                    QString("SMTP banner error: %1").arg(firstServerLine(banner)),
                    QString("SMTP サーバーバナーエラー: %1").arg(firstServerLine(banner)));
            } else {
                outSock.write("EHLO sollunamail.local\r\n");
                outSock.flush();
                QByteArray ehloResp = readSocketUntil(outSock, [](const QByteArray &b) {
                    return b.contains("250 ") || b.contains("500 ") || b.contains("502 ");
                }, 6000);

                if (outSec.contains("STARTTLS", Qt::CaseInsensitive) && !outSock.isEncrypted()) {
                    outSock.write("STARTTLS\r\n");
                    outSock.flush();
                    QByteArray stlsResp = readSocketUntil(outSock, [](const QByteArray &b) {
                        return b.contains("220 ") || b.contains("454 ") || b.contains("502 ");
                    }, 6000);
                    if (stlsResp.contains("220")) {
                        outSock.startClientEncryption();
                        waitSocketStep(outSock, true, 6000);
                        outSock.write("EHLO sollunamail.local\r\n");
                        outSock.flush();
                        readSocketUntil(outSock, [](const QByteArray &b) {
                            return b.contains("250 ") || b.contains("500 ");
                        }, 5000);
                    }
                }

                const bool useOAuth = (!testAcc.oauthAccessToken.trimmed().isEmpty() && outPass.trimmed().isEmpty());
                QByteArray authResp;
                if (useOAuth) {
                    QString authStr = QStringLiteral("user=%1\x01auth=Bearer %2\x01\x01")
                                          .arg(outUser, testAcc.oauthAccessToken.trimmed());
                    outSock.write(QStringLiteral("AUTH XOAUTH2 %1\r\n")
                                      .arg(QString::fromLatin1(authStr.toUtf8().toBase64()))
                                      .toUtf8());
                    outSock.flush();
                    authResp = readSocketUntil(outSock, [](const QByteArray &b) {
                        return b.contains("235 ") || b.contains("535 ") || b.contains("334 ") || b.contains("530 ");
                    }, 7000);
                    if (authResp.contains("334 ")) {
                        outSock.write("\r\n");
                        outSock.flush();
                        authResp.append(readSocketUntil(outSock, [](const QByteArray &b) {
                            return b.contains("235 ") || b.contains("535 ") || b.contains("530 ");
                        }, 5000));
                    }
                } else {
                    outSock.write("AUTH LOGIN\r\n");
                    outSock.flush();
                    QByteArray step1 = readSocketUntil(outSock, [](const QByteArray &b) {
                        return b.contains("334") || b.contains("503") || b.contains("504") || b.contains("530");
                    }, 5000);
                    if (step1.contains("334")) {
                        outSock.write(outUser.toUtf8().toBase64() + "\r\n");
                        outSock.flush();
                        QByteArray step2 = readSocketUntil(outSock, [](const QByteArray &b) {
                            return b.contains("334") || b.contains("535") || b.contains("501");
                        }, 5000);
                        if (step2.contains("334")) {
                            outSock.write(outPass.toUtf8().toBase64() + "\r\n");
                            outSock.flush();
                            authResp = readSocketUntil(outSock, [](const QByteArray &b) {
                                return b.contains("235") || b.contains("535") || b.contains("534") || b.contains("454");
                            }, 7000);
                        } else {
                            authResp = step2;
                        }
                    } else {
                        authResp = step1;
                    }
                }

                if (authResp.contains("235")) {
                    outSuccess = true;
                    outDetail = MailI18n::t(
                        QString("%1:%2 (%3, ID: %4) 접속·인증 성공!").arg(outHost, QString::number(outPort), outSec, outUser),
                        QString("%1:%2 (%3, ID: %4) Connected & Authenticated!").arg(outHost, QString::number(outPort), outSec, outUser),
                        QString("%1:%2 (%3, ID: %4) 接続・認証成功！").arg(outHost, QString::number(outPort), outSec, outUser));
                } else {
                    outDetail = MailI18n::t(
                        QString("%1:%2 (%3, ID: %4) 인증 실패 — 서버 응답: %5").arg(outHost, QString::number(outPort), outSec, outUser, firstServerLine(authResp)),
                        QString("%1:%2 (%3, ID: %4) Auth failed — Server response: %5").arg(outHost, QString::number(outPort), outSec, outUser, firstServerLine(authResp)),
                        QString("%1:%2 (%3, ID: %4) 認証失敗 — サーバー応答: %5").arg(outHost, QString::number(outPort), outSec, outUser, firstServerLine(authResp)));
                }
                outSock.write("QUIT\r\n");
                outSock.flush();
            }
            outSock.disconnectFromHost();
        }
    }

    testGuard.restoreCursorNow();

    const bool allOk = (inSuccess && outSuccess);
    const QString inLabel = MailI18n::t(QString("받는 서버 (%1):").arg(inProtoName),
                                        QString("Incoming (%1):").arg(inProtoName),
                                        QString("受信サーバー (%1):").arg(inProtoName));
    const QString outLabel = MailI18n::t("보내는 서버 (SMTP):", "Outgoing (SMTP):", "送信サーバー (SMTP):");
    QString html = QStringLiteral(
        "<div style='line-height:1.55;'>"
        "<b style='color:%1; font-size:13px;'>%2</b><br/>"
        "• <b>%3</b> <span style='color:%4;'>%5</span><br/>"
        "• <b>%6</b> <span style='color:%7;'>%8</span>"
        "%9"
        "</div>")
        .arg(allOk ? QStringLiteral("#047857") : QStringLiteral("#B91C1C"),
             allOk ? MailI18n::t("✓ 모든 메일 서버(수신/발신) 접속 및 로그인 시험에 성공했습니다!",
                                 "✓ Incoming and Outgoing server connection tests succeeded!",
                                 "✓ すべてのメールサーバー（受信/送信）接続テストに成功しました！")
                   : MailI18n::t("⚠️ 일부 메일 서버 접속 또는 로그인 인증에 실패했습니다.",
                                 "⚠️ Server connection or authentication test failed.",
                                 "⚠️ メールサーバー接続または認証テストに失敗しました。"),
             inLabel,
             inSuccess ? QStringLiteral("#047857") : QStringLiteral("#DC2626"),
             inDetail.toHtmlEscaped(),
             outLabel,
             outSuccess ? QStringLiteral("#047857") : QStringLiteral("#DC2626"),
             outDetail.toHtmlEscaped(),
             inHint.isEmpty() ? QString() : QStringLiteral("<br/><span style='color:#1D4ED8;'>%1</span>").arg(inHint.toHtmlEscaped()));

    if (outHtmlSummary)
        *outHtmlSummary = html;

    QString plainMsg = QStringLiteral(
        "[%1]\n\n"
        "📥 %2\n   %3 %4\n\n"
        "📤 %5\n   %6 %7%8")
        .arg(testAcc.email,
             inLabel,
             inSuccess ? QStringLiteral("✓") : QStringLiteral("✗"),
             inDetail,
             outLabel,
             outSuccess ? QStringLiteral("✓") : QStringLiteral("✗"),
             outDetail,
             inHint.isEmpty() ? QString() : QStringLiteral("\n\n") + inHint);

    if (!safeParent || !safeParent->isVisible())
        return;

    if (allOk) {
        QMessageBox::information(safeParent.data(),
                                 MailI18n::t("서버 접속 시험 성공", "Server Connection Test Passed", "サーバー接続テスト成功"),
                                 plainMsg);
    } else {
        QMessageBox::warning(safeParent.data(),
                             MailI18n::t("서버 접속 시험 결과", "Server Connection Test Result", "サーバー接続テスト結果"),
                             plainMsg);
    }
}

QPushButton *createPasswordVisibilityToggleBtn(QLineEdit *lineEdit, QWidget *parent)
{
    QPushButton *btn = new QPushButton(parent);
    btn->setCursor(Qt::PointingHandCursor);
    btn->setMinimumWidth(84);

    QAction *inlineAct = lineEdit->addAction(
        MailIcons::actionIcon("eye_off", QColor("#6B7280"), 16),
        QLineEdit::TrailingPosition);

    auto syncVisualState = [lineEdit, btn, inlineAct]() {
        const bool isPlain = (lineEdit->echoMode() == QLineEdit::Normal);
        btn->setIcon(MailIcons::actionIcon(isPlain ? "eye_on" : "eye_off",
                                           isPlain ? QColor("#0D47A1") : QColor("#4B5563"),
                                           15));
        btn->setText(isPlain
                         ? MailI18n::t("숨기기", "Hide", "非表示")
                         : MailI18n::t("보이기", "Show", "表示"));
        btn->setToolTip(isPlain
                            ? MailI18n::t("클릭하면 암호/키를 다시 숨깁니다 (●●●●)",
                                          "Click to mask password/key (●●●●)",
                                          "クリックしてパスワード/キーを隠します")
                            : MailI18n::t("클릭하면 입력된 암호/키를 글자로 확인합니다",
                                          "Click to reveal password/key characters",
                                          "クリックして入力されたパスワード/キーを表示します"));
        btn->setStyleSheet(isPlain
                               ? "QPushButton { background: #EFF6FF; color: #0D47A1; font-size: 11.5px; font-weight: 700; border: 1px solid #93C5FD; border-radius: 5px; padding: 6px 10px; }"
                                 "QPushButton:hover { background: #DBEAFE; }"
                               : "QPushButton { background: #FFFFFF; color: #374151; font-size: 11.5px; font-weight: 600; border: 1px solid #D1D5DB; border-radius: 5px; padding: 6px 10px; }"
                                 "QPushButton:hover { background: #F3F4F6; border-color: #9CA3AF; }");
        inlineAct->setIcon(MailIcons::actionIcon(isPlain ? "eye_on" : "eye_off",
                                                 isPlain ? QColor("#0D47A1") : QColor("#6B7280"),
                                                 16));
    };

    auto toggleVisibility = [lineEdit, syncVisualState]() {
        const bool isPlain = (lineEdit->echoMode() == QLineEdit::Normal);
        lineEdit->setEchoMode(isPlain ? QLineEdit::Password : QLineEdit::Normal);
        syncVisualState();
    };

    QObject::connect(btn, &QPushButton::clicked, lineEdit, toggleVisibility);
    QObject::connect(inlineAct, &QAction::triggered, lineEdit, toggleVisibility);
    syncVisualState();
    return btn;
}
} // namespace

AddAccountDialog::AddAccountDialog(GoogleProvider *googleProvider,
                                   const AccountItem *editTarget,
                                   const QString &initialProviderPreset,
                                   QWidget *parent)
    : QDialog(parent),
      m_googleProvider(googleProvider),
      m_isEditMode(editTarget != nullptr)
{
    setWindowTitle(m_isEditMode
                   ? MailI18n::t("계정 설정 수정 - SollunaMail", "Edit Account - SollunaMail", "アカウント設定の編集 - SollunaMail")
                   : MailI18n::t("새 이메일 계정 추가 - SollunaMail", "Add Email Account - SollunaMail", "新しいメールアカウントの追加 - SollunaMail"));
    resize(800, 740);
    setMinimumSize(740, 680);
    setStyleSheet(
        "QDialog { background-color: #FFFFFF; color: #111827; }"
        "QLabel { color: #374151; font-size: 12.5px; }"
        "QCheckBox, QRadioButton { color: #1F2937; }"
        "QGroupBox { color: #111827; font-weight: 700; }"
        "QComboBox QAbstractItemView { background-color: #FFFFFF; color: #111827; selection-background-color: #EFF6FF; selection-color: #0D47A1; }");

    if (m_isEditMode && editTarget) {
        m_result = *editTarget;
    } else {
        m_result.id = "acc_" + QUuid::createUuid().toString(QUuid::Id128).left(8);
        m_result.avatarColor = QColor("#1AA3B8");
        m_result.visualStyle = AvatarVisualStyle::TealSquareS;
    }

    QVBoxLayout *outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);

    QScrollArea *scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setStyleSheet("background: #FFFFFF;");

    QWidget *content = new QWidget(scroll);
    QVBoxLayout *v = new QVBoxLayout(content);
    v->setContentsMargins(28, 24, 28, 20);
    v->setSpacing(14);

    // 1. 상단 타이틀
    QLabel *hdrTitle = new QLabel(
        m_isEditMode
            ? MailI18n::t("계정 정보 및 연동 설정", "Account & Sync Settings", "アカウント情報と連携設定")
            : MailI18n::t("어떤 이메일 계정을 연결할까요?", "Which email account would you like to connect?", "どのメールアカウントを連携しますか？"),
        content);
    hdrTitle->setStyleSheet("font-size: 18px; font-weight: 700; color: #111827;");
    QLabel *hdrSub = new QLabel(
        MailI18n::t(
            "이메일 주소를 입력하거나 아래 버튼을 누르면 Gmail, iCloud, 네이버, 카카오 등을 자동으로 인식하여 초보자도 클릭 한 번으로 연동할 수 있습니다.",
            "Enter your email address or click a provider button below to auto-detect Gmail, iCloud, Naver, Kakao, or custom servers with one click.",
            "メールアドレスを入力するか下のボタンを押すと、Gmail、iCloud、Naver、Kakaoなどを自動認識し、ワンクリックで簡単に連携できます。"),
        content);
    hdrSub->setWordWrap(true);
    hdrSub->setStyleSheet("font-size: 12px; color: #6B7280;");
    v->addWidget(hdrTitle);
    v->addWidget(hdrSub);

    // 2. 초보자용 원클릭 프로바이더 선택 버튼 바 (6개 구분: Google / Apple iCloud / 네이버 / 다음·카카오 / IMAP / POP3)
    QHBoxLayout *quickRow = new QHBoxLayout();
    quickRow->setSpacing(6);
    auto makeQuickProviderBtn = [content](const QString &label, const QString &bgColor, const QString &fgColor, const QString &borderColor) {
        QPushButton *b = new QPushButton(label, content);
        b->setCursor(Qt::PointingHandCursor);
        b->setStyleSheet(QString(
            "QPushButton { background: %1; color: %2; border: 1px solid %3; border-radius: 7px; padding: 9px 8px; font-size: 12px; font-weight: 700; }"
            "QPushButton:hover { opacity: 0.92; }"
        ).arg(bgColor, fgColor, borderColor));
        return b;
    };

    QPushButton *btnQuickGoogle = makeQuickProviderBtn("🌐 Google", "#EFF6FF", "#1D4ED8", "#93C5FD");
    QPushButton *btnQuickICloud = makeQuickProviderBtn("☁️ Apple iCloud", "#F0F9FF", "#0369A1", "#BAE6FD");
    QPushButton *btnQuickNaver  = makeQuickProviderBtn(MailI18n::t("🟢 네이버", "🟢 Naver", "🟢 Naver"), "#ECFDF5", "#047857", "#6EE7B7");
    QPushButton *btnQuickKakao  = makeQuickProviderBtn(MailI18n::t("🟡 다음·카카오", "🟡 Daum·Kakao", "🟡 Daum·Kakao"), "#FEFCE8", "#A16207", "#FDE047");
    QPushButton *btnQuickImap   = makeQuickProviderBtn("📥 IMAP", "#F8FAFC", "#334155", "#CBD5E1");
    QPushButton *btnQuickPop3   = makeQuickProviderBtn("📬 POP3", "#F5F3FF", "#6D28D9", "#DDD6FE");

    quickRow->addWidget(btnQuickGoogle);
    quickRow->addWidget(btnQuickICloud);
    quickRow->addWidget(btnQuickNaver);
    quickRow->addWidget(btnQuickKakao);
    quickRow->addWidget(btnQuickImap);
    quickRow->addWidget(btnQuickPop3);
    v->addLayout(quickRow);

    QString inputCss = "QLineEdit, QComboBox, QSpinBox { border: 1px solid #D1D5DB; border-radius: 6px; padding: 8px 11px; font-size: 13px; background: #FFFFFF; color: #111827; }"
                       "QLineEdit:focus, QComboBox:focus, QSpinBox:focus { border: 1px solid #0D47A1; }";
    content->setStyleSheet(inputCss);

    // 3. 기본 입력 (표시 이름 & 이메일 주소)
    QFormLayout *basicForm = new QFormLayout();
    basicForm->setSpacing(10);

    m_displayNameEdit = new QLineEdit(content);
    m_displayNameEdit->setPlaceholderText(MailI18n::t("예: 홍길동 (메일 발송 시 표시될 이름)", "e.g., John Doe (Sender Display Name)", "例: 山田太郎 (送信時に表示される名前)"));
    m_displayNameEdit->setText(m_result.displayName);

    m_emailEdit = new QLineEdit(content);
    m_emailEdit->setPlaceholderText("e.g. myname@gmail.com, myid@icloud.com, user@company.com");
    m_emailEdit->setText(m_result.email);

    m_avatarColorCombo = new QComboBox(content);
    m_avatarColorCombo->addItem(MailI18n::t("청록색 (Teal #1AA3B8)", "Teal (#1AA3B8)", "ティール (Teal #1AA3B8)"), "#1AA3B8");
    m_avatarColorCombo->addItem(MailI18n::t("구글 블루 (Blue #1E88E5)", "Google Blue (#1E88E5)", "Google ブルー (Blue #1E88E5)"), "#1E88E5");
    m_avatarColorCombo->addItem(MailI18n::t("로열 블루 (Navy #1B4EA3)", "Royal Blue (#1B4EA3)", "ロイヤルブルー (Navy #1B4EA3)"), "#1B4EA3");
    m_avatarColorCombo->addItem(MailI18n::t("에메랄드 (Green #10B981)", "Emerald (#10B981)", "エメラルド (Green #10B981)"), "#10B981");
    m_avatarColorCombo->addItem(MailI18n::t("퍼플 (Purple #8B5CF6)", "Purple (#8B5CF6)", "パープル (Purple #8B5CF6)"), "#8B5CF6");
    m_avatarColorCombo->addItem(MailI18n::t("코랄 레드 (Coral #EF4444)", "Coral Red (#EF4444)", "コーラルレッド (Coral #EF4444)"), "#EF4444");

    basicForm->addRow(MailI18n::t("표시 이름 (Name):", "Display Name:", "表示名 (Name):"), m_displayNameEdit);
    basicForm->addRow(MailI18n::t("이메일 주소 (Email):", "Email Address:", "メールアドレス (Email):"), m_emailEdit);
    basicForm->addRow(MailI18n::t("아바타 색상:", "Avatar Color:", "アバターの色:"), m_avatarColorCombo);
    v->addLayout(basicForm);

    // 4. 실시간 자동 감지 상태 배지
    m_autoDetectBadge = new QLabel(content);
    m_autoDetectBadge->setWordWrap(true);
    m_autoDetectBadge->setStyleSheet(
        "background: #EFF6FF; color: #1E40AF; border: 1px solid #BFDBFE; border-radius: 7px; padding: 10px 12px; font-size: 12px; font-weight: 600;");
    v->addWidget(m_autoDetectBadge);

    // 5-A. Google 감지 시 원클릭 브라우저 로그인 박스 (초보자용: 비밀번호나 Client ID 불필요!)
    m_googleOneClickBox = new QWidget(content);
    m_googleOneClickBox->setStyleSheet("background: #F8FAFC; border: 1px solid #DBEAFE; border-radius: 8px;");
    QVBoxLayout *gLayout = new QVBoxLayout(m_googleOneClickBox);
    gLayout->setContentsMargins(16, 14, 16, 14);
    gLayout->setSpacing(8);

    QLabel *gDesc = new QLabel(
        MailI18n::t("<b>Google 계정은 복잡한 설정이나 비밀번호 입력이 필요 없습니다.</b><br/>"
                    "아래 버튼을 누르면 웹 브라우저에서 Google 로그인 창이 열리고, 로그인 즉시 <b>Gmail · 구글 캘린더 · 구글 주소록</b>이 자동으로 연동됩니다.",
                    "<b>No manual server setup needed for Google accounts.</b><br/>"
                    "Click the button below to sign in with Google in your browser and automatically sync <b>Gmail, Google Calendar, and Google Contacts</b>.",
                    "<b>Googleアカウントは複雑なサーバー設定が不要です。</b><br/>"
                    "下のボタンを押すとブラウザでGoogleログイン画面が開き、<b>Gmail・Googleカレンダー・Google連絡先</b>が自動連携されます。"),
        m_googleOneClickBox);
    gDesc->setWordWrap(true);
    gDesc->setStyleSheet("font-size: 12px; color: #1F2937; border: none; background: transparent;");

    QPushButton *btnGoogleBrowserSignIn = new QPushButton(
        MailI18n::t("🌐 Google 계정으로 브라우저에서 바로 로그인 (원클릭 연동)",
                    "🌐 Sign in with Google in Browser (One-Click Connect)",
                    "🌐 ブラウザでGoogleアカウントにログイン (ワンクリック連携)"),
        m_googleOneClickBox);
    btnGoogleBrowserSignIn->setCursor(Qt::PointingHandCursor);
    btnGoogleBrowserSignIn->setMinimumHeight(40);
    btnGoogleBrowserSignIn->setStyleSheet(
        "QPushButton { background: #1A73E8; color: #FFFFFF; font-size: 13px; font-weight: 700; border-radius: 6px; padding: 8px 16px; border: none; }"
        "QPushButton:hover { background: #1557B0; }");

    gLayout->addWidget(gDesc);
    gLayout->addWidget(btnGoogleBrowserSignIn);
    v->addWidget(m_googleOneClickBox);

    // 5-B. iCloud / 네이버 / 카카오 / 회사 메일 비밀번호 입력 박스
    m_passwordBox = new QWidget(content);
    m_passwordBox->setStyleSheet("background: #F9FAFB; border: 1px solid #E5E7EB; border-radius: 8px;");
    QVBoxLayout *pwLayout = new QVBoxLayout(m_passwordBox);
    pwLayout->setContentsMargins(16, 14, 16, 14);
    pwLayout->setSpacing(8);

    m_passwordGuideLabel = new QLabel(
        MailI18n::t("계정 비밀번호를 입력하면 서버 설정이 자동으로 구성됩니다.",
                    "Enter your account password (or App Password) to automatically configure the mail servers.",
                    "アカウントのパスワード（またはアプリパスワード）を入力すると、サーバー設定が自動的に構成されます。"),
        m_passwordBox);
    m_passwordGuideLabel->setWordWrap(true);
    m_passwordGuideLabel->setStyleSheet("font-size: 12px; color: #374151; border: none; background: transparent;");

    QHBoxLayout *pwRow = new QHBoxLayout();
    m_passwordEdit = new QLineEdit(m_passwordBox);
    m_passwordEdit->setEchoMode(QLineEdit::Password);
    m_passwordEdit->setPlaceholderText(
        MailI18n::t("이메일 비밀번호 (또는 iCloud / Google 앱 암호) 입력",
                    "Enter email password (or iCloud / Google App Password)",
                    "メールパスワード（または iCloud / Google アプリパスワード）を入力"));
    m_passwordEdit->setText(m_result.appPassword);

    m_appleIdHelpBtn = new QPushButton(
        MailI18n::t("🍎 Apple ID 앱 암호 발급 페이지 열기",
                    "🍎 Open Apple ID App Password Page",
                    "🍎 Apple ID アプリパスワード発行ページを開く"),
        m_passwordBox);
    m_appleIdHelpBtn->setCursor(Qt::PointingHandCursor);
    m_appleIdHelpBtn->setStyleSheet(
        "QPushButton { background: #0284C7; color: #FFFFFF; font-size: 11px; font-weight: 600; border-radius: 5px; padding: 7px 10px; border: none; }"
        "QPushButton:hover { background: #0369A1; }");
    connect(m_appleIdHelpBtn, &QPushButton::clicked, this, [this]() {
        QString domain = m_emailEdit->text().section('@', 1).trimmed().toLower();
        if (domain == "gmail.com" || domain == "googlemail.com") {
            QDesktopServices::openUrl(QUrl("https://myaccount.google.com/apppasswords"));
        } else if (domain == "naver.com") {
            QDesktopServices::openUrl(QUrl("https://mail.naver.com/v2/settings/pop3imap"));
        } else if (domain == "daum.net" || domain == "hanmail.net" || domain == "kakao.com") {
            QDesktopServices::openUrl(QUrl("https://mail.daum.net"));
        } else {
            QDesktopServices::openUrl(QUrl("https://appleid.apple.com/account/manage"));
        }
    });

    QPushButton *pwVisBtn = createPasswordVisibilityToggleBtn(m_passwordEdit, m_passwordBox);

    pwRow->addWidget(m_passwordEdit, 1);
    pwRow->addWidget(pwVisBtn);
    pwRow->addWidget(m_appleIdHelpBtn);
    pwLayout->addWidget(m_passwordGuideLabel);
    pwLayout->addLayout(pwRow);
    v->addWidget(m_passwordBox);

    // 6. 고급 설정 토글 버튼 (초보자 화면에서는 접혀 있음, 클릭 시 받는 서버 / 보내는 서버 Mailtrap·Mailgun 분리 설정 표시)
    m_toggleAdvancedBtn = new QPushButton(
        MailI18n::t("⚙️ 고급 서버 설정 열기 (받는 서버 / 보내는 서버 분리 · Mailtrap · Mailgun 설정) ▾",
                    "⚙️ Show Advanced Server Settings (Separate IMAP / SMTP · Mailtrap · Mailgun) ▾",
                    "⚙️ 詳細サーバー設定を開く（受信IMAP / 送信SMTP分離・Mailtrap・Mailgun設定） ▾"),
        content);
    m_toggleAdvancedBtn->setCursor(Qt::PointingHandCursor);
    m_toggleAdvancedBtn->setStyleSheet(
        "QPushButton { background: transparent; color: #4B5563; font-size: 12px; font-weight: 600; text-align: left; border: none; padding: 4px 2px; }"
        "QPushButton:hover { color: #0D47A1; }");
    v->addWidget(m_toggleAdvancedBtn);

    m_advancedPanel = new QWidget(content);
    m_advancedPanel->setVisible(false);
    QVBoxLayout *advLayout = new QVBoxLayout(m_advancedPanel);
    advLayout->setContentsMargins(0, 4, 0, 4);
    advLayout->setSpacing(12);

    // 6-1. 📥 받는 메일 서버 (Incoming IMAP) 그룹
    QGroupBox *imapGroup = new QGroupBox(
        MailI18n::t("📥 받는 메일 서버 (Incoming IMAP)",
                    "📥 Incoming Mail Server (IMAP)",
                    "📥 受信メールサーバー (Incoming IMAP)"),
        m_advancedPanel);
    imapGroup->setStyleSheet("QGroupBox { font-weight: 700; color: #1F2937; border: 1px solid #D1D5DB; border-radius: 7px; margin-top: 8px; padding-top: 10px; }");
    QFormLayout *imapForm = new QFormLayout(imapGroup);
    imapForm->setSpacing(8);

    m_imapHostEdit = new QLineEdit(imapGroup);
    m_imapHostEdit->setPlaceholderText(
        MailI18n::t("예: imap.gmail.com, imap.mail.me.com, mail.sample.com",
                    "e.g. imap.gmail.com, imap.mail.me.com, mail.sample.com",
                    "例: imap.gmail.com, imap.mail.me.com, mail.sample.com"));
    m_imapHostEdit->setText(m_result.imapHost);

    QHBoxLayout *imapPortSecRow = new QHBoxLayout();
    m_imapPortSpin = new QSpinBox(imapGroup);
    m_imapPortSpin->setRange(1, 65535);
    m_imapPortSpin->setValue(m_result.imapPort > 0 ? m_result.imapPort : 993);

    m_imapSecurityCombo = new QComboBox(imapGroup);
    m_imapSecurityCombo->addItems({"SSL/TLS", "STARTTLS", "None"});
    m_imapSecurityCombo->setCurrentText(m_result.imapSecurity.isEmpty() ? "SSL/TLS" : m_result.imapSecurity);
    imapPortSecRow->addWidget(new QLabel(MailI18n::t("포트:", "Port:", "ポート:"), imapGroup));
    imapPortSecRow->addWidget(m_imapPortSpin);
    imapPortSecRow->addSpacing(10);
    imapPortSecRow->addWidget(new QLabel(MailI18n::t("보안:", "Security:", "セキュリティ:"), imapGroup));
    imapPortSecRow->addWidget(m_imapSecurityCombo, 1);

    m_imapUsernameEdit = new QLineEdit(imapGroup);
    m_imapUsernameEdit->setPlaceholderText(
        MailI18n::t("비워두면 위 이메일 주소와 동일한 계정 사용 (다를 경우 별도 ID 입력)",
                    "Leave blank to use email address above (or enter custom username)",
                    "空欄の場合は上のメールアドレスを使用（異なる場合は別IDを入力）"));
    m_imapUsernameEdit->setText(m_result.imapUsername);

    m_pop3LeaveCopyCheck = new QCheckBox(
        MailI18n::t("✓ 서버에 메일 복사본 저장 (Leave a copy of messages on the server — 기본 권장)",
                    "✓ Leave a copy of messages on the server (Recommended)",
                    "✓ サーバーにメッセージのコピーを残す（推奨）"),
        imapGroup);
    m_pop3LeaveCopyCheck->setChecked(m_result.pop3LeaveCopyOnServer);
    m_pop3LeaveCopyCheck->setStyleSheet("font-weight: 600; color: #047857; margin-top: 2px;");

    imapForm->addRow(MailI18n::t("받는 서버 (IMAP/POP3):", "Incoming Server:", "受信サーバー:"), m_imapHostEdit);
    imapForm->addRow(MailI18n::t("포트 및 보안:", "Port & Security:", "ポートとセキュリティ:"), imapPortSecRow);
    imapForm->addRow(MailI18n::t("받는 서버 ID:", "Username:", "受信サーバー ID:"), m_imapUsernameEdit);
    imapForm->addRow(QString(), m_pop3LeaveCopyCheck);
    advLayout->addWidget(imapGroup);

    // 6-2. 📤 보내는 메일 서버 (Outgoing SMTP — Mailtrap / Mailgun / 별도 서버·계정) 그룹
    QGroupBox *smtpGroup = new QGroupBox(
        MailI18n::t("📤 보내는 메일 서버 (Outgoing SMTP — Mailtrap · Mailgun · 별도 계정 지원)",
                    "📤 Outgoing Mail Server (SMTP — Mailtrap · Mailgun · Custom Account)",
                    "📤 送信メールサーバー (Outgoing SMTP — Mailtrap・Mailgun・別アカウント対応)"),
        m_advancedPanel);
    smtpGroup->setStyleSheet("QGroupBox { font-weight: 700; color: #1F2937; border: 1px solid #D1D5DB; border-radius: 7px; margin-top: 8px; padding-top: 10px; }");
    QVBoxLayout *smtpV = new QVBoxLayout(smtpGroup);
    smtpV->setSpacing(8);

    QFormLayout *smtpForm = new QFormLayout();
    smtpForm->setSpacing(8);

    m_smtpPresetCombo = new QComboBox(smtpGroup);
    m_smtpPresetCombo->addItem(
        MailI18n::t("기본 (받는 메일 서버와 동일한 제공자 사용)",
                    "Default (Same provider as incoming IMAP server)",
                    "標準（受信メールサーバーと同じプロバイダーを使用）"),
        "SameAsIncoming");
    m_smtpPresetCombo->addItem("🚀 Mailtrap Email Sending (live.smtp.mailtrap.io : 587 STARTTLS)", "MailtrapLive");
    m_smtpPresetCombo->addItem(
        MailI18n::t("🧪 Mailtrap Sandbox 테스트 (sandbox.smtp.mailtrap.io : 2525 STARTTLS)",
                    "🧪 Mailtrap Sandbox Testing (sandbox.smtp.mailtrap.io : 2525 STARTTLS)",
                    "🧪 Mailtrap Sandbox テスト (sandbox.smtp.mailtrap.io : 2525 STARTTLS)"),
        "MailtrapSandbox");
    m_smtpPresetCombo->addItem("📬 Mailgun SMTP (smtp.mailgun.org : 587 STARTTLS)", "Mailgun");
    m_smtpPresetCombo->addItem("🇪🇺 Mailgun EU SMTP (smtp.eu.mailgun.org : 587 STARTTLS)", "MailgunEU");
    m_smtpPresetCombo->addItem("📨 SendGrid SMTP (smtp.sendgrid.net : 587 STARTTLS)", "SendGrid");
    m_smtpPresetCombo->addItem(
        MailI18n::t("⚙️ 직접 입력 (Custom SMTP 서버)",
                    "⚙️ Custom SMTP Server",
                    "⚙️ 直接入力 (カスタム SMTP サーバー)"),
        "Custom");

    int presetIdx = m_smtpPresetCombo->findData(m_result.smtpPreset);
    if (presetIdx >= 0)
        m_smtpPresetCombo->setCurrentIndex(presetIdx);

    m_smtpHostEdit = new QLineEdit(smtpGroup);
    m_smtpHostEdit->setPlaceholderText(
        MailI18n::t("예: smtp.gmail.com, live.smtp.mailtrap.io, smtp.mailgun.org",
                    "e.g. smtp.gmail.com, live.smtp.mailtrap.io, smtp.mailgun.org",
                    "例: smtp.gmail.com, live.smtp.mailtrap.io, smtp.mailgun.org"));
    m_smtpHostEdit->setText(m_result.smtpHost);

    QHBoxLayout *smtpPortSecRow = new QHBoxLayout();
    m_smtpPortSpin = new QSpinBox(smtpGroup);
    m_smtpPortSpin->setRange(1, 65535);
    m_smtpPortSpin->setValue(m_result.smtpPort > 0 ? m_result.smtpPort : 465);

    m_smtpSecurityCombo = new QComboBox(smtpGroup);
    m_smtpSecurityCombo->addItems({"SSL/TLS", "STARTTLS", "None"});
    m_smtpSecurityCombo->setCurrentText(m_result.smtpSecurity.isEmpty() ? "SSL/TLS" : m_result.smtpSecurity);
    smtpPortSecRow->addWidget(new QLabel(MailI18n::t("포트:", "Port:", "ポート:"), smtpGroup));
    smtpPortSecRow->addWidget(m_smtpPortSpin);
    smtpPortSecRow->addSpacing(10);
    smtpPortSecRow->addWidget(new QLabel(MailI18n::t("보안:", "Security:", "セキュリティ:"), smtpGroup));
    smtpPortSecRow->addWidget(m_smtpSecurityCombo, 1);

    smtpForm->addRow(MailI18n::t("보내는 서버 유형:", "SMTP Preset:", "送信サーバー種別:"), m_smtpPresetCombo);
    smtpForm->addRow(MailI18n::t("보내는 서버 (SMTP):", "SMTP Server:", "送信サーバー (SMTP):"), m_smtpHostEdit);
    smtpForm->addRow(MailI18n::t("포트 및 보안:", "Port & Security:", "ポートとセキュリティ:"), smtpPortSecRow);
    smtpV->addLayout(smtpForm);

    m_separateSmtpAuthCheck = new QCheckBox(
        MailI18n::t("받는 서버와 다른 별도의 보내는 서버(SMTP) 계정(ID / 비밀번호 / API 토큰) 사용",
                    "Use separate outgoing SMTP credentials (Username / Password / API Token)",
                    "受信サーバーとは別の送信サーバー(SMTP)認証情報（ID / パスワード / APIトークン）を使用"),
        smtpGroup);
    m_separateSmtpAuthCheck->setChecked(m_result.useSeparateSmtpAuth);
    m_separateSmtpAuthCheck->setStyleSheet("font-weight: 600; color: #0D47A1; margin-top: 4px;");
    smtpV->addWidget(m_separateSmtpAuthCheck);

    m_separateSmtpAuthBox = new QWidget(smtpGroup);
    QFormLayout *sepAuthForm = new QFormLayout(m_separateSmtpAuthBox);
    sepAuthForm->setContentsMargins(4, 4, 4, 4);
    sepAuthForm->setSpacing(8);

    m_smtpUsernameEdit = new QLineEdit(m_separateSmtpAuthBox);
    m_smtpUsernameEdit->setPlaceholderText(
        MailI18n::t("예: api (Mailtrap), 샌드박스 ID, postmaster@mg.domain.com (Mailgun)",
                    "e.g. api (Mailtrap), Sandbox ID, postmaster@mg.domain.com (Mailgun)",
                    "例: api (Mailtrap), Sandbox ID, postmaster@mg.domain.com (Mailgun)"));
    m_smtpUsernameEdit->setText(m_result.smtpUsername);

    m_smtpPasswordEdit = new QLineEdit(m_separateSmtpAuthBox);
    m_smtpPasswordEdit->setEchoMode(QLineEdit::Password);
    m_smtpPasswordEdit->setPlaceholderText(
        MailI18n::t("보내는 서버 전용 비밀번호 또는 Mailtrap / Mailgun API Token 입력",
                    "Enter outgoing SMTP password or Mailtrap / Mailgun API Token",
                    "送信サーバー専用パスワードまたは Mailtrap / Mailgun APIトークンを入力"));
    m_smtpPasswordEdit->setText(m_result.smtpPassword);

    QWidget *smtpPwRowW = new QWidget(m_separateSmtpAuthBox);
    QHBoxLayout *smtpPwRowH = new QHBoxLayout(smtpPwRowW);
    smtpPwRowH->setContentsMargins(0, 0, 0, 0);
    smtpPwRowH->setSpacing(6);
    QPushButton *smtpPwVisBtn = createPasswordVisibilityToggleBtn(m_smtpPasswordEdit, smtpPwRowW);
    smtpPwRowH->addWidget(m_smtpPasswordEdit, 1);
    smtpPwRowH->addWidget(smtpPwVisBtn);

    sepAuthForm->addRow(MailI18n::t("보내는 서버 ID:", "SMTP Username:", "送信サーバー ID:"), m_smtpUsernameEdit);
    sepAuthForm->addRow(MailI18n::t("보내는 서버 암호/토큰:", "SMTP Password/Token:", "送信パスワード/トークン:"), smtpPwRowW);
    m_separateSmtpAuthBox->setVisible(m_result.useSeparateSmtpAuth);
    smtpV->addWidget(m_separateSmtpAuthBox);

    advLayout->addWidget(smtpGroup);
    v->addWidget(m_advancedPanel);

    v->addStretch();
    scroll->setWidget(content);
    outer->addWidget(scroll, 1);

    // 7. 하단 확인 / 취소 바
    QWidget *bottomBar = new QWidget(this);
    bottomBar->setStyleSheet("background: #F9FAFB; border-top: 1px solid #E5E7EB;");
    QHBoxLayout *bLayout = new QHBoxLayout(bottomBar);
    bLayout->setContentsMargins(24, 12, 24, 12);

    QPushButton *btnCancel = new QPushButton(MailI18n::t("취소", "Cancel", "キャンセル"), bottomBar);
    btnCancel->setCursor(Qt::PointingHandCursor);
    btnCancel->setStyleSheet("QPushButton { background: #FFFFFF; color: #374151; border: 1px solid #D1D5DB; border-radius: 6px; padding: 8px 18px; font-weight: 600; }");

    QPushButton *btnConfirm = new QPushButton(
        m_isEditMode
            ? MailI18n::t("✓ 수정 내용 저장", "✓ Save Changes", "✓ 変更を保存")
            : MailI18n::t("✓ 계정 연동 완료", "✓ Connect Account", "✓ アカウント連携完了"),
        bottomBar);
    btnConfirm->setCursor(Qt::PointingHandCursor);
    btnConfirm->setStyleSheet("QPushButton { background: #0D47A1; color: #FFFFFF; border: none; border-radius: 6px; padding: 8px 22px; font-weight: 700; }");

    QPushButton *btnTestConn = new QPushButton(
        MailI18n::t("🔌 서버 접속 시험", "🔌 Test Server Connection", "🔌 サーバー接続テスト"),
        bottomBar);
    btnTestConn->setCursor(Qt::PointingHandCursor);
    btnTestConn->setStyleSheet("QPushButton { background: #059669; color: #FFFFFF; border: none; border-radius: 6px; padding: 8px 16px; font-weight: 700; }"
                               "QPushButton:hover { background: #047857; }");

    bLayout->addWidget(btnTestConn);
    bLayout->addStretch();
    bLayout->addWidget(btnCancel);
    bLayout->addWidget(btnConfirm);
    outer->addWidget(bottomBar);

    // 시그널 연결
    connect(m_toggleAdvancedBtn, &QPushButton::clicked, this, [this]() {
        bool vis = !m_advancedPanel->isVisible();
        m_advancedPanel->setVisible(vis);
        m_toggleAdvancedBtn->setText(vis
            ? MailI18n::t("⚙️ 고급 서버 설정 닫기 (받는 서버 / 보내는 서버 분리 · Mailtrap · Mailgun 설정) ▴",
                          "⚙️ Hide Advanced Server Settings (Separate IMAP / SMTP · Mailtrap · Mailgun) ▴",
                          "⚙️ 詳細サーバー設定を閉じる（受信IMAP / 送信SMTP分離・Mailtrap・Mailgun設定） ▴")
            : MailI18n::t("⚙️ 고급 서버 설정 열기 (받는 서버 / 보내는 서버 분리 · Mailtrap · Mailgun 설정) ▾",
                          "⚙️ Show Advanced Server Settings (Separate IMAP / SMTP · Mailtrap · Mailgun) ▾",
                          "⚙️ 詳細サーバー設定を開く（受信IMAP / 送信SMTP分離・Mailtrap・Mailgun設定） ▾"));
    });

    connect(m_separateSmtpAuthCheck, &QCheckBox::toggled, m_separateSmtpAuthBox, &QWidget::setVisible);
    connect(m_smtpPresetCombo, &QComboBox::currentIndexChanged, this, &AddAccountDialog::applySmtpPreset);
    connect(m_emailEdit, &QLineEdit::textChanged, this, &AddAccountDialog::applyDomainAutoDetect);

    // 상단 6개 원클릭 프로바이더 버튼 동작 (Google / Apple iCloud / 네이버 / 다음·카카오 / IMAP / POP3)
    auto isPlaceholderEmail = [](const QString &em) {
        const QString t = em.trimmed().toLower();
        return t.isEmpty() || !t.contains('@')
               || t == "myaccount@gmail.com"
               || t == "myappleid@icloud.com"
               || t == "myid@naver.com"
               || t == "myid@daum.net"
               || t == "myid@kakao.com"
               || t == "user@sample.com"
               || t == "user@solluna-iot.com";
    };

    connect(btnQuickGoogle, &QPushButton::clicked, this, [this, isPlaceholderEmail]() {
        if (isPlaceholderEmail(m_emailEdit->text()) || !m_emailEdit->text().contains("@gmail.com"))
            m_emailEdit->setText("myaccount@gmail.com");
        m_result.provider = AccountProviderType::Google;
        applyDomainAutoDetect(m_emailEdit->text());
        if (m_googleProvider) {
            QString hint = m_emailEdit->text().trimmed();
            if (hint == "myaccount@gmail.com")
                hint.clear();
            m_googleProvider->startBuiltInGoogleLogin(hint);
        }
    });

    connect(btnQuickICloud, &QPushButton::clicked, this, [this, isPlaceholderEmail]() {
        if (isPlaceholderEmail(m_emailEdit->text())
            || (!m_emailEdit->text().contains("@icloud.com") && !m_emailEdit->text().contains("@me.com") && !m_emailEdit->text().contains("@mac.com")))
            m_emailEdit->setText("myappleid@icloud.com");
        m_emailEdit->selectAll();
        m_emailEdit->setFocus();
        applyDomainAutoDetect(m_emailEdit->text());
    });

    connect(btnQuickNaver, &QPushButton::clicked, this, [this, isPlaceholderEmail]() {
        if (isPlaceholderEmail(m_emailEdit->text()) || !m_emailEdit->text().contains("@naver.com"))
            m_emailEdit->setText("myid@naver.com");
        m_emailEdit->selectAll();
        m_emailEdit->setFocus();
        applyDomainAutoDetect(m_emailEdit->text());
    });

    connect(btnQuickKakao, &QPushButton::clicked, this, [this, isPlaceholderEmail]() {
        if (isPlaceholderEmail(m_emailEdit->text())
            || (!m_emailEdit->text().contains("@daum.net") && !m_emailEdit->text().contains("@kakao.com") && !m_emailEdit->text().contains("@hanmail.net")))
            m_emailEdit->setText("myid@daum.net");
        m_emailEdit->selectAll();
        m_emailEdit->setFocus();
        applyDomainAutoDetect(m_emailEdit->text());
    });

    connect(btnQuickImap, &QPushButton::clicked, this, [this, imapGroup, isPlaceholderEmail]() {
        if (isPlaceholderEmail(m_emailEdit->text()))
            m_emailEdit->setText("user@sample.com");
        imapGroup->setTitle(
            MailI18n::t("📥 받는 메일 서버 (Incoming IMAP)",
                        "📥 Incoming Mail Server (IMAP)",
                        "📥 受信メールサーバー (Incoming IMAP)"));
        m_advancedPanel->setVisible(true);
        m_toggleAdvancedBtn->setText(
            MailI18n::t("⚙️ 고급 서버 설정 닫기 (받는 서버 / 보내는 서버 분리 · Mailtrap · Mailgun 설정) ▴",
                        "⚙️ Hide Advanced Server Settings (Separate IMAP / SMTP · Mailtrap · Mailgun) ▴",
                        "⚙️ 詳細サーバー設定を閉じる（受信IMAP / 送信SMTP分離・Mailtrap・Mailgun設定） ▴"));
        applyDomainAutoDetect(m_emailEdit->text());
        if (m_imapHostEdit->text().startsWith("pop.", Qt::CaseInsensitive)) {
            m_imapHostEdit->setText("imap." + m_imapHostEdit->text().mid(4));
        }
        m_imapPortSpin->setValue(993);
        m_imapSecurityCombo->setCurrentText("SSL/TLS");
    });

    connect(btnQuickPop3, &QPushButton::clicked, this, [this, imapGroup, isPlaceholderEmail]() {
        if (isPlaceholderEmail(m_emailEdit->text()))
            m_emailEdit->setText("user@sample.com");
        imapGroup->setTitle(
            MailI18n::t("📬 받는 메일 서버 (Incoming POP3)",
                        "📬 Incoming Mail Server (POP3)",
                        "📬 受信メールサーバー (Incoming POP3)"));
        m_advancedPanel->setVisible(true);
        m_toggleAdvancedBtn->setText(
            MailI18n::t("⚙️ 고급 서버 설정 닫기 (받는 서버 / 보내는 서버 분리 · Mailtrap · Mailgun 설정) ▴",
                        "⚙️ Hide Advanced Server Settings (Separate IMAP / SMTP · Mailtrap · Mailgun) ▴",
                        "⚙️ 詳細サーバー設定を閉じる（受信IMAP / 送信SMTP分離・Mailtrap・Mailgun設定） ▴"));
        applyDomainAutoDetect(m_emailEdit->text());
        QString domain = m_emailEdit->text().section('@', 1).trimmed().toLower();
        if (!domain.isEmpty()) {
            m_imapHostEdit->setText("pop." + domain);
        }
        m_imapPortSpin->setValue(995);
        m_imapSecurityCombo->setCurrentText("SSL/TLS");
        if (m_pop3LeaveCopyCheck)
            m_pop3LeaveCopyCheck->setChecked(true);
        m_autoDetectBadge->setText(
            MailI18n::t(QString("✓ POP3 받는 서버(pop.%1 : 995 SSL/TLS, 기본: 서버에 복사본 저장 + 로컬 pop3/%2 폴더 보관)가 설정되었습니다.")
                            .arg(domain.isEmpty() ? QStringLiteral("domain.com") : domain,
                                 m_emailEdit->text().trimmed().isEmpty() ? QStringLiteral("account") : m_emailEdit->text().trimmed()),
                        QString("✓ POP3 incoming server (pop.%1 : 995 SSL/TLS, Leave copy on server + local pop3/ mailbox) configured.")
                            .arg(domain.isEmpty() ? QStringLiteral("domain.com") : domain),
                        QString("✓ POP3 受信サーバー(pop.%1 : 995 SSL/TLS、サーバーにコピーを残す)設定モードが開きました。")
                            .arg(domain.isEmpty() ? QStringLiteral("domain.com") : domain)));
    });

    connect(btnGoogleBrowserSignIn, &QPushButton::clicked, this, [this]() {
        if (!m_googleProvider)
            return;
        QString hint = m_emailEdit->text().trimmed();
        if (hint == "myaccount@gmail.com")
            hint.clear();
        m_googleProvider->startBuiltInGoogleLogin(hint);
    });

    // 브라우저에서 Google OAuth 로그인 완료 시 자동으로 이메일·이름·서버·토큰을 m_result까지 완벽히 세팅 후 완료!
    if (m_googleProvider) {
        connect(m_googleProvider, &GoogleProvider::googleProfileReady, this,
                [this](const QString &email, const QString &displayName, const QString &accessToken, const QString &refreshToken) {
            if (!isVisible())
                return;
            QString finalEmail = email.trimmed();
            if (finalEmail.isEmpty()) {
                QString typed = m_emailEdit->text().trimmed();
                if (!typed.isEmpty() && typed != "myaccount@gmail.com")
                    finalEmail = typed;
            }
            if (!finalEmail.isEmpty())
                m_emailEdit->setText(finalEmail);

            QString finalName = displayName.trimmed();
            if (! m_displayNameEdit->text().trimmed().isEmpty())
                finalName = m_displayNameEdit->text().trimmed();
            if (finalName.isEmpty() && !finalEmail.isEmpty())
                finalName = finalEmail.section('@', 0, 0);
            if (!finalName.isEmpty())
                m_displayNameEdit->setText(finalName);

            m_imapHostEdit->setText("imap.gmail.com");
            m_imapPortSpin->setValue(993);
            m_imapSecurityCombo->setCurrentText("SSL/TLS");
            if (m_smtpPresetCombo->currentData().toString() == "SameAsIncoming") {
                m_smtpHostEdit->setText("smtp.gmail.com");
                m_smtpPortSpin->setValue(465);
                m_smtpSecurityCombo->setCurrentText("SSL/TLS");
            }

            // m_result 구조체 전체 필드 즉시 반영 (accept() 호출 전 필수!)
            m_result.email = finalEmail;
            m_result.displayName = finalName;
            m_result.initials = finalName.isEmpty() ? QStringLiteral("G") : finalName.left(1).toUpper();
            m_result.avatarColor = QColor(m_avatarColorCombo->currentData().toString());
            m_result.provider = AccountProviderType::Google;
            m_result.visualStyle = AvatarVisualStyle::GoogleOrbit;
            m_result.oauthAccessToken = accessToken;
            m_result.oauthRefreshToken = refreshToken;

            m_result.imapHost = m_imapHostEdit->text().trimmed().isEmpty() ? QStringLiteral("imap.gmail.com") : m_imapHostEdit->text().trimmed();
            m_result.imapPort = m_imapPortSpin->value() > 0 ? m_imapPortSpin->value() : 993;
            m_result.imapSecurity = m_imapSecurityCombo->currentText().isEmpty() ? QStringLiteral("SSL/TLS") : m_imapSecurityCombo->currentText();
            m_result.imapUsername = m_imapUsernameEdit->text().trimmed();
            m_result.appPassword = m_passwordEdit->text();

            m_result.smtpPreset = m_smtpPresetCombo->currentData().toString();
            m_result.smtpHost = m_smtpHostEdit->text().trimmed().isEmpty() ? QStringLiteral("smtp.gmail.com") : m_smtpHostEdit->text().trimmed();
            m_result.smtpPort = m_smtpPortSpin->value() > 0 ? m_smtpPortSpin->value() : 465;
            m_result.smtpSecurity = m_smtpSecurityCombo->currentText().isEmpty() ? QStringLiteral("SSL/TLS") : m_smtpSecurityCombo->currentText();
            m_result.useSeparateSmtpAuth = m_separateSmtpAuthCheck->isChecked();
            m_result.smtpUsername = m_smtpUsernameEdit->text().trimmed();
            m_result.smtpPassword = m_smtpPasswordEdit->text();

            QMessageBox::information(this,
                                     MailI18n::t("Google 계정 연동 완료", "Google Account Connected", "Google アカウント連携完了"),
                                     MailI18n::t(QString("✓ Google 계정(%1 <%2>) 인증이 완료되었습니다!\n메일(imap.gmail.com / smtp.gmail.com), 캘린더, 주소록이 자동으로 연동됩니다.").arg(finalName, finalEmail),
                                                 QString("✓ Google Account (%1 <%2>) is now connected!\nGmail, Calendar, and Contacts will sync automatically.").arg(finalName, finalEmail),
                                                 QString("✓ Google アカウント(%1 <%2>)の認証が完了しました！\nメール、カレンダー、連絡先が自動連携されます。").arg(finalName, finalEmail)));
            accept();
        });
    }

    connect(btnCancel, &QPushButton::clicked, this, &QDialog::reject);
    connect(btnTestConn, &QPushButton::clicked, this, [this]() {
        AccountItem testAcc = m_result;
        testAcc.email = m_emailEdit->text().trimmed();
        testAcc.displayName = m_displayNameEdit->text().trimmed();
        testAcc.imapHost = m_imapHostEdit->text().trimmed();
        testAcc.imapPort = m_imapPortSpin->value();
        testAcc.imapSecurity = m_imapSecurityCombo->currentText();
        testAcc.imapUsername = m_imapUsernameEdit->text().trimmed();
        testAcc.appPassword = m_passwordEdit->text();
        testAcc.smtpPreset = m_smtpPresetCombo->currentData().toString();
        testAcc.smtpHost = m_smtpHostEdit->text().trimmed();
        testAcc.smtpPort = m_smtpPortSpin->value();
        testAcc.smtpSecurity = m_smtpSecurityCombo->currentText();
        testAcc.useSeparateSmtpAuth = m_separateSmtpAuthCheck->isChecked();
        testAcc.smtpUsername = m_smtpUsernameEdit->text().trimmed();
        testAcc.smtpPassword = m_smtpPasswordEdit->text();
        runMailServerConnectionTest(testAcc, this, nullptr);
    });
    connect(btnConfirm, &QPushButton::clicked, this, [this]() {
        QString email = m_emailEdit->text().trimmed();
        if (email.isEmpty() || !email.contains('@')) {
            QMessageBox::warning(this,
                                 MailI18n::t("이메일 확인", "Check Email Address", "メールアドレスの確認"),
                                 MailI18n::t("올바른 이메일 주소를 입력해 주세요.",
                                             "Please enter a valid email address.",
                                             "正しいメールアドレスを入力してください。"));
            return;
        }
        m_result.email = email;
        m_result.displayName = m_displayNameEdit->text().trimmed();
        if (m_result.displayName.isEmpty())
            m_result.displayName = email.section('@', 0, 0);
        m_result.initials = m_result.displayName.left(1).toUpper();
        m_result.avatarColor = QColor(m_avatarColorCombo->currentData().toString());

        m_result.imapHost = m_imapHostEdit->text().trimmed();
        m_result.imapPort = m_imapPortSpin->value();
        m_result.imapSecurity = m_imapSecurityCombo->currentText();
        m_result.imapUsername = m_imapUsernameEdit->text().trimmed();
        m_result.appPassword = m_passwordEdit->text();
        m_result.pop3LeaveCopyOnServer = m_pop3LeaveCopyCheck ? m_pop3LeaveCopyCheck->isChecked() : true;

        m_result.smtpPreset = m_smtpPresetCombo->currentData().toString();
        m_result.smtpHost = m_smtpHostEdit->text().trimmed();
        m_result.smtpPort = m_smtpPortSpin->value();
        m_result.smtpSecurity = m_smtpSecurityCombo->currentText();
        m_result.useSeparateSmtpAuth = m_separateSmtpAuthCheck->isChecked();
        m_result.smtpUsername = m_smtpUsernameEdit->text().trimmed();
        m_result.smtpPassword = m_smtpPasswordEdit->text();

        if (m_result.isPop3()) {
            m_result.ensurePop3MailboxDirs();
        }

        accept();
    });

    // 초기 프리셋 처리
    if (!initialProviderPreset.isEmpty() && !m_isEditMode) {
        if (initialProviderPreset == "Google") {
            m_emailEdit->setText("myaccount@gmail.com");
        } else if (initialProviderPreset == "ICloud") {
            m_emailEdit->setText("myappleid@icloud.com");
        } else if (initialProviderPreset == "Naver") {
            m_emailEdit->setText("myid@naver.com");
        } else if (initialProviderPreset == "DaumKakao") {
            m_emailEdit->setText("myid@daum.net");
        } else if (initialProviderPreset == "IMAP") {
            btnQuickImap->click();
            return;
        } else if (initialProviderPreset == "POP3") {
            btnQuickPop3->click();
            return;
        }
    }
    applyDomainAutoDetect(m_emailEdit->text());
    if (m_isEditMode && m_result.useSeparateSmtpAuth) {
        m_advancedPanel->setVisible(true);
        m_toggleAdvancedBtn->setText(
            MailI18n::t("⚙️ 고급 서버 설정 닫기 (받는 서버 / 보내는 서버 분리 · Mailtrap · Mailgun 설정) ▴",
                        "⚙️ Hide Advanced Server Settings (Separate IMAP / SMTP · Mailtrap · Mailgun) ▴",
                        "⚙️ 詳細サーバー設定を閉じる（受信IMAP / 送信SMTP分離・Mailtrap・Mailgun設定） ▴"));
    }
}

void AddAccountDialog::applyDomainAutoDetect(const QString &email)
{
    QString domain = email.section('@', 1).trimmed().toLower();
    bool keepCustomSmtp = (m_smtpPresetCombo->currentData().toString() != "SameAsIncoming");

    if (domain == "gmail.com" || domain == "googlemail.com") {
        m_result.provider = AccountProviderType::Google;
        m_result.visualStyle = AvatarVisualStyle::GoogleOrbit;
        m_imapHostEdit->setText("imap.gmail.com");
        m_imapPortSpin->setValue(993);
        m_imapSecurityCombo->setCurrentText("SSL/TLS");
        if (!keepCustomSmtp) {
            m_smtpHostEdit->setText("smtp.gmail.com");
            m_smtpPortSpin->setValue(465);
            m_smtpSecurityCombo->setCurrentText("SSL/TLS");
        }
        m_autoDetectBadge->setText(
            MailI18n::t("✓ Google (Gmail) 계정이 감지되었습니다! 브라우저 로그인 또는 Google 16자리 앱 비밀번호로 바로 연동하세요.",
                        "✓ Google (Gmail) account detected! Connect via browser sign-in or a 16-digit Google App Password.",
                        "✓ Google (Gmail) アカウントを検出しました！ブラウザログインまたはGoogle 16桁アプリパスワードで連携できます。"));
        m_googleOneClickBox->setVisible(true);
        m_passwordBox->setVisible(true);
        m_passwordGuideLabel->setText(
            MailI18n::t("<b>🔑 브라우저 로그인이 차단될 경우 (iCloud와 동일한 앱 암호 방식):</b><br/>"
                        "우측 버튼을 눌러 <b>Google 16자리 앱 비밀번호</b>를 생성해 입력하면 차단 없이 즉시 Gmail과 연동됩니다.",
                        "<b>🔑 Alternative if browser sign-in is blocked:</b><br/>"
                        "Click the button on the right to generate a <b>16-digit Google App Password</b> and connect Gmail directly.",
                        "<b>🔑 ブラウザログインがブロックされる場合:</b><br/>"
                        "右のボタンから<b>Google 16桁アプリパスワード</b>を発行して入力すると、即座にGmailと連携できます。"));
        m_appleIdHelpBtn->setText(
            MailI18n::t("🔑 Google 앱 비밀번호 발급 열기",
                        "🔑 Open Google App Passwords",
                        "🔑 Google アプリパスワード発行"));
        m_appleIdHelpBtn->setVisible(true);
    } else if (domain == "icloud.com" || domain == "me.com" || domain == "mac.com") {
        m_result.provider = AccountProviderType::ICloud;
        m_result.visualStyle = AvatarVisualStyle::BlueFolderBadge;
        m_imapHostEdit->setText("imap.mail.me.com");
        m_imapPortSpin->setValue(993);
        m_imapSecurityCombo->setCurrentText("SSL/TLS");
        if (!keepCustomSmtp) {
            m_smtpHostEdit->setText("smtp.mail.me.com");
            m_smtpPortSpin->setValue(587);
            m_smtpSecurityCombo->setCurrentText("STARTTLS");
        }
        m_autoDetectBadge->setText(
            MailI18n::t("☁️ Apple iCloud 계정이 자동 설정되었습니다! (imap.mail.me.com / smtp.mail.me.com 자동 연결)",
                        "☁️ Apple iCloud server settings configured automatically! (imap.mail.me.com / smtp.mail.me.com)",
                        "☁️ Apple iCloud アカウントが自動設定されました！ (imap.mail.me.com / smtp.mail.me.com)"));
        m_googleOneClickBox->setVisible(false);
        m_passwordBox->setVisible(true);
        m_passwordGuideLabel->setText(
            MailI18n::t("<b>Apple iCloud 간편 연동:</b> iCloud 암호(또는 Apple ID 페이지에서 생성한 16자리 앱 암호)만 입력하면 즉시 연결됩니다.",
                        "<b>Apple iCloud Quick Setup:</b> Enter your 16-digit Apple ID App-Specific Password to connect immediately.",
                        "<b>Apple iCloud かんたん連携:</b> Apple IDページで発行した16桁のアプリ用パスワードを入力してください。"));
        m_appleIdHelpBtn->setText(
            MailI18n::t("🍎 Apple ID 앱 암호 발급 열기",
                        "🍎 Open Apple ID App Password Page",
                        "🍎 Apple ID アプリパスワード発行"));
        m_appleIdHelpBtn->setVisible(true);
    } else if (domain == "outlook.com" || domain == "hotmail.com" || domain == "live.com" || domain == "msn.com") {
        m_result.provider = AccountProviderType::Microsoft;
        m_imapHostEdit->setText("outlook.office365.com");
        m_imapPortSpin->setValue(993);
        m_imapSecurityCombo->setCurrentText("SSL/TLS");
        if (!keepCustomSmtp) {
            m_smtpHostEdit->setText("smtp.office365.com");
            m_smtpPortSpin->setValue(587);
            m_smtpSecurityCombo->setCurrentText("STARTTLS");
        }
        m_autoDetectBadge->setText(
            MailI18n::t("✓ Microsoft Outlook / Hotmail 서버가 자동으로 설정되었습니다. 비밀번호만 입력하세요.",
                        "✓ Microsoft Outlook / Hotmail servers configured automatically. Enter your password below.",
                        "✓ Microsoft Outlook / Hotmail サーバーが自動設定されました。パスワードを入力してください。"));
        m_googleOneClickBox->setVisible(false);
        m_passwordBox->setVisible(true);
        m_passwordGuideLabel->setText(
            MailI18n::t("Microsoft 계정 비밀번호를 입력하면 즉시 연동됩니다.",
                        "Enter your Microsoft account password to connect immediately.",
                        "Microsoft アカウントのパスワードを入力すると即座に連携されます。"));
        m_appleIdHelpBtn->setVisible(false);
    } else if (domain == "naver.com") {
        m_result.provider = AccountProviderType::CustomImap;
        m_imapHostEdit->setText("imap.naver.com");
        m_imapPortSpin->setValue(993);
        m_imapSecurityCombo->setCurrentText("SSL/TLS");
        if (!keepCustomSmtp) {
            m_smtpHostEdit->setText("smtp.naver.com");
            m_smtpPortSpin->setValue(465);
            m_smtpSecurityCombo->setCurrentText("SSL/TLS");
        }
        m_autoDetectBadge->setText(
            MailI18n::t("✓ 네이버 메일(imap.naver.com / smtp.naver.com)이 자동으로 설정되었습니다. 비밀번호만 입력하세요.",
                        "✓ Naver Mail (imap.naver.com / smtp.naver.com) configured automatically. Enter your password below.",
                        "✓ Naver メール (imap.naver.com / smtp.naver.com) が自動設定されました。パスワードを入力してください。"));
        m_googleOneClickBox->setVisible(false);
        m_passwordBox->setVisible(true);
        m_passwordGuideLabel->setText(
            MailI18n::t("네이버 비밀번호(2단계 인증 시 애플리케이션 비밀번호)를 입력하면 즉시 연동됩니다.",
                        "Enter your Naver password (or App Password if 2FA is enabled) to connect.",
                        "Naver パスワード（2段階認証の場合はアプリパスワード）を入力してください。"));
        m_appleIdHelpBtn->setText(
            MailI18n::t("🟢 네이버 IMAP/SMTP 설정 열기",
                        "🟢 Open Naver IMAP/SMTP Settings",
                        "🟢 Naver IMAP/SMTP 設定を開く"));
        m_appleIdHelpBtn->setVisible(true);
    } else if (domain == "daum.net" || domain == "hanmail.net" || domain == "kakao.com") {
        m_result.provider = AccountProviderType::CustomImap;
        m_imapHostEdit->setText("imap.daum.net");
        m_imapPortSpin->setValue(993);
        m_imapSecurityCombo->setCurrentText("SSL/TLS");
        if (!keepCustomSmtp) {
            m_smtpHostEdit->setText("smtp.daum.net");
            m_smtpPortSpin->setValue(465);
            m_smtpSecurityCombo->setCurrentText("SSL/TLS");
        }
        m_autoDetectBadge->setText(
            MailI18n::t("✓ 다음 · 카카오 메일(imap.daum.net / smtp.daum.net)이 자동으로 설정되었습니다. 비밀번호만 입력하세요.",
                        "✓ Daum / Kakao Mail (imap.daum.net / smtp.daum.net) configured automatically.",
                        "✓ Daum / Kakao メール (imap.daum.net / smtp.daum.net) が自動設定されました。"));
        m_googleOneClickBox->setVisible(false);
        m_passwordBox->setVisible(true);
        m_passwordGuideLabel->setText(
            MailI18n::t("다음/카카오 계정 비밀번호(2단계 인증 시 앱 비밀번호)를 입력하면 즉시 연동됩니다.",
                        "Enter your Daum/Kakao account password (or App Password).",
                        "Daum/Kakao アカウントのパスワード（またはアプリパスワード）を入力してください。"));
        m_appleIdHelpBtn->setText(
            MailI18n::t("🟡 다음·카카오 메일 설정 열기",
                        "🟡 Open Daum/Kakao Mail Settings",
                        "🟡 Daum/Kakao メール設定を開く"));
        m_appleIdHelpBtn->setVisible(true);
    } else if (!domain.isEmpty()) {
        m_result.provider = AccountProviderType::CustomImap;
        if (!m_isEditMode || m_imapHostEdit->text().isEmpty()) {
            m_imapHostEdit->setText("mail." + domain);
            m_imapPortSpin->setValue(993);
            m_imapSecurityCombo->setCurrentText("SSL/TLS");
        }
        if (!keepCustomSmtp && (!m_isEditMode || m_smtpHostEdit->text().isEmpty())) {
            m_smtpHostEdit->setText("mail." + domain);
            m_smtpPortSpin->setValue(465);
            m_smtpSecurityCombo->setCurrentText("SSL/TLS");
        }
        m_autoDetectBadge->setText(
            MailI18n::t(QString("✓ 도메인(%1) 메일 서버(mail.%1)가 자동 설정되었습니다. 보내는 서버를 Mailtrap·Mailgun 등으로 분리하려면 아래 '고급 서버 설정'을 눌러주세요.").arg(domain),
                        QString("✓ Domain (%1) mail server (mail.%1) auto-configured. Click 'Advanced Server Settings' below to customize IMAP/SMTP or use Mailtrap/Mailgun.").arg(domain),
                        QString("✓ ドメイン(%1)のメールサーバー(mail.%1)が自動設定されました。詳細設定から受信/送信サーバーを個別に変更できます。").arg(domain)));
        m_googleOneClickBox->setVisible(false);
        m_passwordBox->setVisible(true);
        m_passwordGuideLabel->setText(
            MailI18n::t("이메일 비밀번호를 입력하세요. (보내는 서버나 계정이 다르면 아래 '고급 서버 설정'에서 분리 설정 가능)",
                        "Enter your email password. (Use 'Advanced Server Settings' below if outgoing SMTP uses different credentials.)",
                        "メールのパスワードを入力してください。（送信サーバーが異なる場合は下の詳細設定で分離設定可能）"));
        m_appleIdHelpBtn->setVisible(false);
    } else {
        m_autoDetectBadge->setText(
            MailI18n::t("이메일 주소를 입력하면 Gmail, iCloud, 네이버, 카카오, 회사 메일 서버를 자동으로 찾아줍니다.",
                        "Enter your email address to automatically detect Gmail, iCloud, Naver, Kakao, or custom mail servers.",
                        "メールアドレスを入力すると、Gmail、iCloud、Naver、Kakao、社内メールサーバーを自動検出します。"));
        m_googleOneClickBox->setVisible(false);
        m_passwordBox->setVisible(true);
        m_appleIdHelpBtn->setVisible(false);
    }
}

void AddAccountDialog::applySmtpPreset(int presetIndex)
{
    Q_UNUSED(presetIndex);
    QString preset = m_smtpPresetCombo->currentData().toString();
    if (preset == "MailtrapLive") {
        m_smtpHostEdit->setText("live.smtp.mailtrap.io");
        m_smtpPortSpin->setValue(587);
        m_smtpSecurityCombo->setCurrentText("STARTTLS");
        m_separateSmtpAuthCheck->setChecked(true);
        if (m_smtpUsernameEdit->text().trimmed().isEmpty())
            m_smtpUsernameEdit->setText("api");
        m_smtpPasswordEdit->setPlaceholderText(MailI18n::t("Mailtrap API Token 입력", "Enter Mailtrap API Token", "Mailtrap API トークンを入力"));
    } else if (preset == "MailtrapSandbox") {
        m_smtpHostEdit->setText("sandbox.smtp.mailtrap.io");
        m_smtpPortSpin->setValue(2525);
        m_smtpSecurityCombo->setCurrentText("STARTTLS");
        m_separateSmtpAuthCheck->setChecked(true);
        m_smtpUsernameEdit->setPlaceholderText(MailI18n::t("Mailtrap Sandbox Username (예: 1a2b3c4d5e6f7g)", "Mailtrap Sandbox Username (e.g. 1a2b3c4d5e6f7g)", "Mailtrap Sandbox ユーザー名"));
        m_smtpPasswordEdit->setPlaceholderText(MailI18n::t("Mailtrap Sandbox Password 입력", "Enter Mailtrap Sandbox Password", "Mailtrap Sandbox パスワードを入力"));
    } else if (preset == "Mailgun") {
        m_smtpHostEdit->setText("smtp.mailgun.org");
        m_smtpPortSpin->setValue(587);
        m_smtpSecurityCombo->setCurrentText("STARTTLS");
        m_separateSmtpAuthCheck->setChecked(true);
        if (m_smtpUsernameEdit->text().trimmed().isEmpty()) {
            QString dom = m_emailEdit->text().section('@', 1).trimmed();
            m_smtpUsernameEdit->setText(dom.isEmpty() ? "postmaster@mg.yourdomain.com" : ("postmaster@mg." + dom));
        }
        m_smtpPasswordEdit->setPlaceholderText(MailI18n::t("Mailgun SMTP Password 입력", "Enter Mailgun SMTP Password", "Mailgun SMTP パスワードを入力"));
    } else if (preset == "MailgunEU") {
        m_smtpHostEdit->setText("smtp.eu.mailgun.org");
        m_smtpPortSpin->setValue(587);
        m_smtpSecurityCombo->setCurrentText("STARTTLS");
        m_separateSmtpAuthCheck->setChecked(true);
    } else if (preset == "SendGrid") {
        m_smtpHostEdit->setText("smtp.sendgrid.net");
        m_smtpPortSpin->setValue(587);
        m_smtpSecurityCombo->setCurrentText("STARTTLS");
        m_separateSmtpAuthCheck->setChecked(true);
        m_smtpUsernameEdit->setText("apikey");
        m_smtpPasswordEdit->setPlaceholderText(MailI18n::t("SendGrid API Key (SG.xxxx) 입력", "Enter SendGrid API Key (SG.xxxx)", "SendGrid API キー (SG.xxxx) を入力"));
    } else if (preset == "SameAsIncoming") {
        m_separateSmtpAuthCheck->setChecked(false);
        applyDomainAutoDetect(m_emailEdit->text());
    }
}

SettingsDialog::SettingsDialog(QList<AccountItem> *accounts,
                               QList<FolderItem> *folders,
                               PrivacyHubSettings *privacySettings,
                               const QString &currentTheme,
                               GoogleProvider *googleProvider,
                               int initialTabIndex,
                               QWidget *parent)
    : QDialog(parent),
      m_accounts(accounts),
      m_folders(folders),
      m_privacy(privacySettings),
      m_currentTheme(currentTheme),
      m_googleProvider(googleProvider)
{
    setWindowTitle(MailI18n::t("환경 설정 (Settings) - SollunaMail", "Settings - SollunaMail", "環境設定 (Settings) - SollunaMail"));
    resize(1280, 780);
    setMinimumSize(1180, 700);
    setStyleSheet(
        "QDialog { background-color: #FFFFFF; color: #111827; }"
        "QLabel { color: #374151; font-size: 12.5px; }"
        "QCheckBox, QRadioButton { color: #1F2937; }"
        "QGroupBox { color: #111827; font-weight: 700; }"
        "QLineEdit, QSpinBox, QDoubleSpinBox, QDateEdit, QTimeEdit, QDateTimeEdit, QComboBox, QTextEdit, QPlainTextEdit {"
        "  background-color: #FFFFFF; color: #111827; selection-background-color: #2563EB; selection-color: #FFFFFF;"
        "}"
        "QComboBox QAbstractItemView { background-color: #FFFFFF; color: #111827; selection-background-color: #EFF6FF; selection-color: #0D47A1; }"
        "QTableWidget { background-color: #FFFFFF; color: #111827; gridline-color: #E5E7EB; }"
        "QHeaderView::section { background-color: #F9FAFB; color: #374151; font-weight: 700; border: 1px solid #E5E7EB; padding: 6px; }");

    QHBoxLayout *root = new QHBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    // Left Navigation Sidebar (Expanded to 236px so Korean/English/Japanese menu items never clip)
    QWidget *leftPane = new QWidget(this);
    leftPane->setFixedWidth(236);
    leftPane->setStyleSheet("background-color: #F6F7F9; border-right: 1px solid #E5E7EB;");
    QVBoxLayout *leftLayout = new QVBoxLayout(leftPane);
    leftLayout->setContentsMargins(10, 18, 10, 14);
    leftLayout->setSpacing(6);

    // Quick Language Switcher at top of Settings Sidebar (Korean / English / Japanese)
    QWidget *langBox = new QWidget(leftPane);
    langBox->setStyleSheet("background: #FFFFFF; border: 1px solid #E5E7EB; border-radius: 6px;");
    QHBoxLayout *langRow = new QHBoxLayout(langBox);
    langRow->setContentsMargins(4, 4, 4, 4);
    langRow->setSpacing(3);

    auto makeLangBtn = [this, langBox](const QString &text, AppLanguage lang) {
        QPushButton *b = new QPushButton(text, langBox);
        b->setCursor(Qt::PointingHandCursor);
        bool active = (MailI18n::currentLanguage() == lang);
        b->setStyleSheet(active
            ? "QPushButton { background: #0D47A1; color: #FFFFFF; font-size: 11px; font-weight: 700; border: none; border-radius: 4px; padding: 5px 4px; }"
            : "QPushButton { background: transparent; color: #374151; font-size: 11px; font-weight: 600; border: none; border-radius: 4px; padding: 5px 4px; }"
              "QPushButton:hover { background: #EFF6FF; color: #0D47A1; }");
        connect(b, &QPushButton::clicked, this, [this, lang]() {
            if (MailI18n::currentLanguage() == lang)
                return;
            QTimer::singleShot(0, this, [this, lang]() {
                if (MailI18n::currentLanguage() == lang)
                    return;
                int curRow = m_navList ? m_navList->currentRow() : 0;
                MailI18n::setLanguage(lang);
                populateNavAndPages(curRow);
                emit languageChanged(lang);
            });
        });
        return b;
    };

    langRow->addWidget(makeLangBtn("🇰🇷 한국어", AppLanguage::Korean));
    langRow->addWidget(makeLangBtn("🇺🇸 EN", AppLanguage::English));
    langRow->addWidget(makeLangBtn("🇯🇵 日本語", AppLanguage::Japanese));
    leftLayout->addWidget(langBox);

    m_navList = new QListWidget(leftPane);
    m_navList->setFrameShape(QFrame::NoFrame);
    m_navList->setIconSize(QSize(18, 18));
    m_navList->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_navList->setStyleSheet(R"(
        QListWidget {
            background: transparent;
            outline: none;
        }
        QListWidget::item {
            padding: 8px 10px;
            border-radius: 6px;
            color: #4B5563;
            font-size: 12.5px;
        }
        QListWidget::item:hover {
            background-color: #EAECEF;
        }
        QListWidget::item:selected {
            background-color: #E5E7EB;
            color: #111827;
            font-weight: 700;
        }
    )");

    leftLayout->addWidget(m_navList, 1);
    root->addWidget(leftPane);

    // Right Pages Area
    m_pages = new QStackedWidget(this);
    root->addWidget(m_pages, 1);

    connect(m_navList, &QListWidget::currentRowChanged, this, [this](int row) {
        m_pages->setCurrentIndex(row);
        for (int i = 0; i < m_navList->count(); ++i) {
            if (QListWidgetItem *it = m_navList->item(i)) {
                it->setIcon(MailIcons::settingsNavIcon(i, (i == row) ? QColor("#111827") : QColor("#6B7280"), 18));
            }
        }
    });
    populateNavAndPages(initialTabIndex);
}

void SettingsDialog::populateNavAndPages(int activeTabIndex)
{
    static bool s_isPopulating = false;
    if (s_isPopulating)
        return;
    s_isPopulating = true;
    setUpdatesEnabled(false);

    setWindowTitle(MailI18n::t("환경 설정 (Settings) - SollunaMail", "Settings - SollunaMail", "環境設定 (Settings) - SollunaMail"));

    // Update top language switcher button styles if present
    QList<QPushButton *> sideBtns = findChildren<QPushButton *>();
    for (QPushButton *b : sideBtns) {
        if (b->text() == "🇰🇷 한국어" || b->text() == "🇺🇸 EN" || b->text() == "🇯🇵 日本語") {
            bool active = (b->text().contains("한국어") && MailI18n::currentLanguage() == AppLanguage::Korean)
                          || (b->text().contains("EN") && MailI18n::currentLanguage() == AppLanguage::English)
                          || (b->text().contains("日本語") && MailI18n::currentLanguage() == AppLanguage::Japanese);
            b->setStyleSheet(active
                ? "QPushButton { background: #0D47A1; color: #FFFFFF; font-size: 11px; font-weight: 700; border: none; border-radius: 4px; padding: 5px 4px; }"
                : "QPushButton { background: transparent; color: #374151; font-size: 11px; font-weight: 600; border: none; border-radius: 4px; padding: 5px 4px; }"
                  "QPushButton:hover { background: #EFF6FF; color: #0D47A1; }");
        }
    }

    m_navList->blockSignals(true);
    m_navList->clear();
    const QStringList tabs = {
        MailI18n::t(" 일반 및 알림 (General)",    " General & Notifications", " 一般・通知 (General)"),
        MailI18n::t(" 계정 관리 (Accounts)",      " Accounts",                " アカウント管理 (Accounts)"),
        MailI18n::t(" 테마 및 모양 (Appearance)", " Appearance",              " 外観・テーマ (Appearance)"),
        MailI18n::t(" 개인정보 보호 (Privacy)",   " Privacy hub",             " プライバシーハブ (Privacy)"),
        MailI18n::t(" 폴더 관리 (Folders)",       " Folders",                 " フォルダ管理 (Folders)"),
        MailI18n::t(" 필터 규칙 (Filters)",       " Filters",                 " フィルター規則 (Filters)"),
        MailI18n::t(" 편지 읽기 및 쓰기 (Mail)",  " Reading & Composing",     " 閲覧・作成 (Mail)"),
        MailI18n::t(" 캘린더 (Calendar)",         " Calendar",                " カレンダー (Calendar)"),
        MailI18n::t(" 서명 관리 (Signatures)",    " Signatures",              " 署名管理 (Signatures)"),
        MailI18n::t(" 상용구 (Snippets)",         " Snippets",                " 定型文 (Snippets)"),
        MailI18n::t(" 단축키 (Shortcuts)",        " Shortcuts",               " ショートカット (Shortcuts)"),
        MailI18n::t(" 고급 설정 (Advanced)",      " Advanced",                " 詳細設定 (Advanced)"),
        MailI18n::t(" AI & MCP (Gemini·Codex)",   " AI & MCP (Gemini·Codex)", " AI & MCP (Gemini·Codex)"),
        MailI18n::t(" 도움말 및 사용법 (Help)",   " Help & User Guide",       " ヘルプと使い方 (Help)")
    };
    int safeIdx = qBound(0, activeTabIndex, tabs.size() - 1);
    for (int i = 0; i < tabs.size(); ++i) {
        QListWidgetItem *navItem = new QListWidgetItem(
            MailIcons::settingsNavIcon(i, (i == safeIdx) ? QColor("#111827") : QColor("#6B7280"), 18),
            tabs[i],
            m_navList);
        navItem->setSizeHint(QSize(210, 36));
    }

    // Reset page-bound member pointers before detaching and deleting old pages
    m_accountsListWidget = nullptr;
    m_accountDetailSummary = nullptr;
    m_accountSubTabStack = nullptr;
    m_accountSubTabBtns.clear();
    m_avatarGridBtns.clear();
    m_accountColorBtn = nullptr;
    m_accYourNameEdit = nullptr;
    m_accEmailReadEdit = nullptr;
    m_accLabelEdit = nullptr;
    m_accUnifiedCheck = nullptr;
    m_accReplyToCheck = nullptr;
    m_accReplyToEdit = nullptr;
    m_accAliasesList = nullptr;
    m_accNewAliasEdit = nullptr;
    m_accInlineImapHost = nullptr;
    m_accInlineImapPort = nullptr;
    m_accInlineImapSec = nullptr;
    m_accInlineSmtpPreset = nullptr;
    m_accInlineSmtpHost = nullptr;
    m_accInlineSmtpPort = nullptr;
    m_accInlineSmtpSec = nullptr;
    m_accInlineSmtpUser = nullptr;
    m_accInlineSmtpPass = nullptr;
    m_accInlineTestResultLbl = nullptr;
    m_appsGoogleContainer = nullptr;
    m_appsIcloudContainer = nullptr;
    m_appsNoneContainer = nullptr;
    m_appsNoneDescLbl = nullptr;

    while (m_pages->count() > 0) {
        QWidget *w = m_pages->widget(0);
        w->hide();
        m_pages->removeWidget(w);
        w->setParent(nullptr);
        w->deleteLater();
    }

    m_pages->addWidget(createGeneralSettingsPage());  // 0: 일반 및 알림 통합
    m_pages->addWidget(createAccountsPage());         // 1: 계정 관리
    m_pages->addWidget(createAppearancePage());       // 2: 테마 및 모양
    m_pages->addWidget(createPrivacyHubPage());       // 3: 개인정보 보호
    m_pages->addWidget(createFoldersPage());          // 4: 폴더 관리
    m_pages->addWidget(createFiltersPage());          // 5: 필터 규칙
    m_pages->addWidget(createComposingPage());        // 6: 편지 읽기 및 쓰기 통합
    m_pages->addWidget(createCalendarSettingsPage()); // 7: 캘린더
    m_pages->addWidget(createSignaturesPage());       // 8: 서명 관리
    m_pages->addWidget(createSnippetsPage());         // 9: 상용구
    m_pages->addWidget(createShortcutsPage());        // 10: 단축키
    m_pages->addWidget(createAdvancedPage());         // 11: 고급 설정
    m_pages->addWidget(createAiMcpPage());            // 12: AI & MCP
    m_pages->addWidget(createHelpGuidePage());        // 13: 도움말 및 사용법 (Help)

    m_navList->setCurrentRow(safeIdx);
    m_pages->setCurrentIndex(safeIdx);
    m_navList->blockSignals(false);

    setUpdatesEnabled(true);
    s_isPopulating = false;
}

QWidget *SettingsDialog::createGeneralSettingsPage()
{
    QScrollArea *scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setStyleSheet("background: #FFFFFF;");

    QWidget *page = new QWidget(scroll);
    QVBoxLayout *v = new QVBoxLayout(page);
    v->setContentsMargins(32, 26, 32, 26);
    v->setSpacing(18);

    QLabel *title = new QLabel(
        MailI18n::t("일반 및 알림 설정 (General & Notifications)",
                    "General & Notification Settings",
                    "一般・通知設定 (General & Notifications)"),
        page);
    title->setStyleSheet("font-size: 18px; font-weight: 700; color: #1F2937;");
    v->addWidget(title);

    QLabel *sub = new QLabel(
#if defined(Q_OS_LINUX)
        MailI18n::t("표시 언어(한국어 · 영어 · 일본어), 리눅스(시스템) 시작 시 자동 실행, 시스템 트레이 동작 및 새 메일 알림 환경을 관리합니다.",
                    "Manage display language, Linux startup, system tray behavior, and new email notification preferences.",
                    "表示言語（韓国語・英語・日本語）、Linux起動時の自動実行、システムトレイ動作および新着通知設定を管理します。"),
#elif defined(Q_OS_MACOS)
        MailI18n::t("표시 언어(한국어 · 영어 · 일본어), macOS 시작 시 자동 실행, 시스템 트레이 동작 및 새 메일 알림 환경을 관리합니다.",
                    "Manage display language, macOS startup, system tray behavior, and new email notification preferences.",
                    "表示言語（韓国語・英語・日本語）、macOS起動時の自動実行、システムトレイ動作および新着通知設定を管理します。"),
#else
        MailI18n::t("표시 언어(한국어 · 영어 · 일본어), 윈도우 시작 시 자동 실행, 시스템 트레이 동작 및 새 메일 알림 환경을 관리합니다.",
                    "Manage display language, Windows startup, system tray behavior, and new email notification preferences.",
                    "表示言語（韓国語・英語・日本語）、Windows起動時の自動実行、システムトレイ動作および新着通知設定を管理します。"),
#endif
        page);
    sub->setWordWrap(true);
    sub->setStyleSheet("font-size: 12px; color: #6B7280;");
    v->addWidget(sub);

    // 1. 다국어(한국어 / 영어 / 일본어) 선택 카드
    QGroupBox *langGroup = new QGroupBox(
        MailI18n::t("🌐 인터페이스 표시 언어 (Display Language)",
                    "🌐 Display Language",
                    "🌐 インターフェース表示言語 (Display Language)"),
        page);
    langGroup->setMaximumWidth(720);
    langGroup->setStyleSheet("QGroupBox { font-size: 13px; font-weight: 700; color: #111827; border: 1px solid #D1D5DB; border-radius: 8px; margin-top: 8px; padding: 16px; background: #FFFFFF; }");
    QVBoxLayout *lgV = new QVBoxLayout(langGroup);
    lgV->setSpacing(10);

    QLabel *langHelp = new QLabel(
        MailI18n::t("언어를 선택하면 재시작 없이 메인 메뉴, 좌측 폴더, 설정 창 전체가 즉시 선택한 언어로 전환됩니다.",
                    "Selecting a language immediately updates all menus, folders, and settings dialogs without restarting.",
                    "言語を選択すると、再起動なしでメインメニュー、左フォルダ、設定画面全体が即座に切り替わります。"),
        langGroup);
    langHelp->setWordWrap(true);
    langHelp->setStyleSheet("font-size: 12px; color: #4B5563; font-weight: 400;");
    lgV->addWidget(langHelp);

    QComboBox *langCombo = new QComboBox(langGroup);
    langCombo->addItem(MailI18n::sanitizeLinuxEmojiIfMissing(QStringLiteral("🇰🇷 한국어 (Korean)")), int(AppLanguage::Korean));
    langCombo->addItem(MailI18n::sanitizeLinuxEmojiIfMissing(QStringLiteral("🇺🇸 English")), int(AppLanguage::English));
    langCombo->addItem(MailI18n::sanitizeLinuxEmojiIfMissing(QStringLiteral("🇯🇵 日本語 (Japanese)")), int(AppLanguage::Japanese));
    langCombo->setCurrentIndex(int(MailI18n::currentLanguage()));
    langCombo->setStyleSheet("padding: 8px 12px; border: 1px solid #D1D5DB; border-radius: 6px; font-size: 13px; font-weight: 600;");
    connect(langCombo, &QComboBox::currentIndexChanged, page, [this, langCombo](int idx) {
        AppLanguage newLang = AppLanguage(langCombo->itemData(idx).toInt());
        if (newLang != MailI18n::currentLanguage()) {
            QTimer::singleShot(0, this, [this, newLang]() {
                if (newLang != MailI18n::currentLanguage()) {
                    MailI18n::setLanguage(newLang);
                    populateNavAndPages(0);
                    emit languageChanged(newLang);
                }
            });
        }
    });
    lgV->addWidget(langCombo);
    v->addWidget(langGroup);

    // 1-2. 새 메일 자동 확인(동기화) 주기 선택 카드
    QGroupBox *syncIntervalGroup = new QGroupBox(
        MailI18n::t("🔄 새 메일 자동 확인 주기 (Auto-check New Mail Interval)",
                    "🔄 Auto-Check New Mail Interval",
                    "🔄 新着メール自動確認間隔"),
        page);
    syncIntervalGroup->setMaximumWidth(720);
    syncIntervalGroup->setStyleSheet("QGroupBox { font-size: 13px; font-weight: 700; color: #111827; border: 1px solid #D1D5DB; border-radius: 8px; margin-top: 8px; padding: 16px; background: #FFFFFF; }");
    QVBoxLayout *siV = new QVBoxLayout(syncIntervalGroup);
    siV->setSpacing(10);

    QLabel *syncHelp = new QLabel(
        MailI18n::t("백그라운드 및 시스템 트레이에서 IMAP 서버의 새 메일을 자동으로 확인하는 새로고침 주기를 선택합니다.",
                    "Choose how often SollunaMail checks the IMAP server for new emails in the background.",
                    "バックグラウンドでIMAPサーバーの新着メールを自動確認する間隔を選択します。"),
        syncIntervalGroup);
    syncHelp->setWordWrap(true);
    syncHelp->setStyleSheet("font-size: 12px; color: #4B5563; font-weight: 400;");
    siV->addWidget(syncHelp);

    QComboBox *syncIntervalCombo = new QComboBox(syncIntervalGroup);
    syncIntervalCombo->addItem(MailI18n::t("⚡ 실시간 푸시(IDLE) + 30초마다 초경량 확인 (권장 — Real-time + 30s)", "⚡ Real-time Push (IDLE) + Every 30 seconds (Recommended)", "⚡ リアルタイムPush (IDLE) + 30秒ごと（推奨）"), 30);
    syncIntervalCombo->addItem(MailI18n::t("1분마다 확인 (Every 1 minute)", "Every 1 minute", "1分ごと"), 60);
    syncIntervalCombo->addItem(MailI18n::t("3분마다 확인 (Every 3 minutes)", "Every 3 minutes", "3分ごと"), 180);
    syncIntervalCombo->addItem(MailI18n::t("5분마다 확인 (Every 5 minutes)", "Every 5 minutes", "5分ごと"), 300);
    syncIntervalCombo->addItem(MailI18n::t("10분마다 확인 (Every 10 minutes)", "Every 10 minutes", "10分ごと"), 600);
    syncIntervalCombo->addItem(MailI18n::t("30분마다 확인 (Every 30 minutes)", "Every 30 minutes", "30分ごと"), 1800);
    syncIntervalCombo->addItem(MailI18n::t("수동 확인 — 자동 새로고침 안 함 (Manual only)", "Manual only (Disable auto-check)", "手動確認のみ（自動確認しない）"), 0);
    int curSyncIdx = syncIntervalCombo->findData(MailI18n::autoSyncIntervalSec());
    if (curSyncIdx < 0)
        curSyncIdx = 0;
    syncIntervalCombo->setCurrentIndex(curSyncIdx);
    syncIntervalCombo->setStyleSheet("padding: 8px 12px; border: 1px solid #D1D5DB; border-radius: 6px; font-size: 13px; font-weight: 600;");
    connect(syncIntervalCombo, &QComboBox::currentIndexChanged, this, [this, syncIntervalCombo]() {
        int sec = syncIntervalCombo->currentData().toInt();
        MailI18n::autoSyncIntervalSec() = sec;
        emit autoSyncIntervalChanged(sec);
        emit privacySettingsChanged();
    });
    siV->addWidget(syncIntervalCombo);
    v->addWidget(syncIntervalGroup);

    auto makeGeneralToggleRow = [this](QWidget *parentBox,
                                       const QString &rowTitle,
                                       const QString &rowDesc,
                                       bool initialVal,
                                       std::function<void(bool)> onToggle) -> QWidget * {
        QWidget *row = new QWidget(parentBox);
        row->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::MinimumExpanding);
        QHBoxLayout *h = new QHBoxLayout(row);
        h->setContentsMargins(0, 6, 0, 6);
        h->setSpacing(14);

        ToggleSwitchWidget *sw = new ToggleSwitchWidget(initialVal, row);
        connect(sw, &ToggleSwitchWidget::toggled, this, [this, onToggle](bool val) {
            onToggle(val);
            emit privacySettingsChanged();
        });

        QVBoxLayout *tv = new QVBoxLayout();
        tv->setContentsMargins(0, 0, 0, 2);
        tv->setSpacing(4);
        QLabel *tLbl = new QLabel(rowTitle, row);
        tLbl->setStyleSheet("font-size: 13px; font-weight: 700; color: #1F2937; padding: 1px 0px;");
        QLabel *dLbl = new QLabel(rowDesc, row);
        dLbl->setWordWrap(true);
        dLbl->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::MinimumExpanding);
        dLbl->setStyleSheet("font-size: 12px; color: #6B7280; font-weight: 400; padding: 2px 0px 4px 0px;");
        if (rowDesc.length() > 45) {
            dLbl->setMinimumHeight(42);
        } else {
            dLbl->setMinimumHeight(24);
        }
        tv->addWidget(tLbl);
        tv->addWidget(dLbl);

        h->addWidget(sw, 0, Qt::AlignTop);
        h->addLayout(tv, 1);
        return row;
    };

    // 2. 시작 및 시스템 트레이 동작 설정 카드
    QGroupBox *startupTrayGroup = new QGroupBox(
        MailI18n::t("🚀 시작 및 시스템 트레이 동작 (Startup && System Tray)",
                    "🚀 Startup && System Tray Behavior",
                    "🚀 起動とシステムトレイ動作"),
        page);
    startupTrayGroup->setMaximumWidth(720);
    startupTrayGroup->setStyleSheet("QGroupBox { font-size: 13px; font-weight: 700; color: #111827; border: 1px solid #D1D5DB; border-radius: 8px; margin-top: 8px; padding: 18px 16px; background: #FFFFFF; }");
    QVBoxLayout *stV = new QVBoxLayout(startupTrayGroup);
    stV->setSpacing(10);

    stV->addWidget(makeGeneralToggleRow(
        startupTrayGroup,
#if defined(Q_OS_LINUX)
        MailI18n::t("시스템(Linux) 시작 시 자동 실행 (Launch at Linux startup)",
                    "Launch SollunaMail at Linux startup",
                    "Linux起動時に自動実行"),
        MailI18n::t("Linux 데스크톱에 로그인할 때 SollunaMail을 자동으로 실행합니다. (~/.config/autostart)",
                    "Automatically start SollunaMail when you log in to Linux (~/.config/autostart).",
                    "Linuxログイン時にSollunaMailを自動的に起動します。"),
#elif defined(Q_OS_MACOS)
        MailI18n::t("macOS 시작 시 자동 실행 (Launch at macOS login)",
                    "Launch SollunaMail at macOS login",
                    "macOSログイン時に自動実行"),
        MailI18n::t("macOS에 로그인할 때 SollunaMail을 자동으로 실행합니다.",
                    "Automatically start SollunaMail when you log in to macOS.",
                    "macOSログイン時にSollunaMailを自動的に起動します。"),
#else
        MailI18n::t("윈도우 시작 시 자동 실행 (Launch at Windows startup)",
                    "Launch SollunaMail at Windows startup",
                    "Windows起動時に自動実行"),
        MailI18n::t("Windows에 로그인할 때 SollunaMail을 자동으로 실행합니다.",
                    "Automatically start SollunaMail when you log in to Windows.",
                    "Windowsログイン時にSollunaMailを自動的に起動します。"),
#endif
        MailI18n::launchOnWindowsStartup(),
        [](bool val) {
            MailI18n::applyWindowsStartupRegistry(val);
        }));

    stV->addWidget(makeGeneralToggleRow(
        startupTrayGroup,
        MailI18n::t("최소화 실행 — 시작 시 시스템 트레이에서 조용히 시작 (Start minimized to tray)",
                    "Start minimized in system tray",
                    "最小化起動（起動時にシステムトレイに格納）"),
        MailI18n::t("앱을 실행할 때 메인 창을 띄우지 않고 우측 하단 시스템 트레이 아이콘으로 시작합니다.",
                    "Start SollunaMail hidden in the system tray without opening the main window.",
                    "起動時にメインウィンドウを表示せず、システムトレイアイコンで開始します。"),
        MailI18n::startMinimizedToTray(),
        [](bool val) {
            MailI18n::startMinimizedToTray() = val;
        }));

    stV->addWidget(makeGeneralToggleRow(
        startupTrayGroup,
        MailI18n::t("창 닫기(X) 버튼을 누르면 종료하지 않고 시스템 트레이로 이동 (Close to tray on X)",
                    "Minimize to system tray when clicking the Close (X) button",
                    "閉じる(X)ボタンで終了せずシステムトレイに格納"),
        MailI18n::t("메인 창의 X 버튼을 눌러도 종료하지 않고 시스템 트레이에서 새 메일을 계속 수신합니다. (트레이 우클릭 [종료]로 완전 종료)",
                    "Keep SollunaMail running in the system tray when closing the main window. Use [Quit] in the tray menu to exit completely.",
                    "メイン画面のXボタンを押しても終了せずトレイに格納します（トレイメニューの[終了]で完全終了）。"),
        MailI18n::closeToTrayOnX(),
        [](bool val) {
            MailI18n::closeToTrayOnX() = val;
        }));

    stV->addWidget(makeGeneralToggleRow(
        startupTrayGroup,
        MailI18n::t("창 최소화(_) 버튼을 누를 때도 작업표시줄 대신 트레이로 숨기기 (Minimize to tray)",
                    "Hide to system tray when minimizing window",
                    "最小化(_)時にもタスクバーではなくシステムトレイに隠す"),
        MailI18n::t("창을 최소화할 때 작업표시줄에 남기지 않고 시스템 트레이 아이콘으로만 표시합니다.",
                    "Hide the taskbar button when the window is minimized.",
                    "ウィンドウ最小化時にタスクバーから隠しシステムトレイに格納します。"),
        MailI18n::minimizeToTrayOnMinimize(),
        [](bool val) {
            MailI18n::minimizeToTrayOnMinimize() = val;
        }));

    v->addWidget(startupTrayGroup);

    // 3. 트레이 알림 및 배지 설정 카드
    QGroupBox *trayNotifGroup = new QGroupBox(
        MailI18n::t("🔔 트레이 알림 및 안 읽은 메일 배지 (Tray Notifications && Badge)",
                    "🔔 System Tray Notifications && Unread Badge",
                    "🔔 トレイ通知および未読バッジ設定"),
        page);
    trayNotifGroup->setMaximumWidth(720);
    trayNotifGroup->setStyleSheet("QGroupBox { font-size: 13px; font-weight: 700; color: #111827; border: 1px solid #D1D5DB; border-radius: 8px; margin-top: 8px; padding: 18px 16px; background: #FFFFFF; }");
    QVBoxLayout *tnV = new QVBoxLayout(trayNotifGroup);
    tnV->setSpacing(10);

    tnV->addWidget(makeGeneralToggleRow(
        trayNotifGroup,
        MailI18n::t("새 메일 도착 시 트레이 알림 배너 표시 (Show tray notifications)",
                    "Show desktop tray notification banner when new email arrives",
                    "新着メール受信時にトレイ通知バナーを表示"),
        MailI18n::t("백그라운드 또는 트레이 상태에서 새 메일이 도착하면 발신자와 제목을 트레이 알림창으로 즉시 알려줍니다.",
                    "Display a system tray popup with sender and subject when new mail arrives.",
                    "新着メール到着時に差出人と件名をシステムトレイ通知で知らせます。"),
        MailI18n::enableDesktopNotification(),
        [](bool val) {
            MailI18n::enableDesktopNotification() = val;
        }));

    tnV->addWidget(makeGeneralToggleRow(
        trayNotifGroup,
        MailI18n::t("새 메일 도착 시 알림음 재생 (Play notification sound)",
                    "Play sound on new mail arrival",
                    "新着メール受信時に通知音を鳴らす"),
        MailI18n::t("새 메일 수신 시 알림 효과음을 함께 재생합니다.",
                    "Play a subtle notification sound when new emails arrive.",
                    "新着メール受信時に通知音を再生します。"),
        MailI18n::enableNotificationSound(),
        [](bool val) {
            MailI18n::enableNotificationSound() = val;
        }));

    tnV->addWidget(makeGeneralToggleRow(
        trayNotifGroup,
        MailI18n::t("트레이 및 작업표시줄 아이콘에 안 읽은 메일 수 배지 표시 (Show unread count badge)",
                    "Show unread email count badge on tray & taskbar icon",
                    "トレイとタスクバーアイコンに未読メール数バッジを表示"),
        MailI18n::t("받은편지함(Inbox)의 안 읽은 메일 개수를 시스템 트레이 아이콘과 앱 아이콘에 실시간 배지로 표시합니다.",
                    "Display the unread inbox count badge on the system tray and application icon.",
                    "受信トレイの未読メール数をトレイアイコンにバッジ表示します。"),
        MailI18n::showTrayUnreadBadge(),
        [](bool val) {
            MailI18n::showTrayUnreadBadge() = val;
        }));

    QHBoxLayout *testNotifRow = new QHBoxLayout();
    testNotifRow->setContentsMargins(0, 4, 0, 0);
    QPushButton *btnTestNotif = new QPushButton(
        MailI18n::t("🔔 알림음 및 트레이 배너 테스트 실행",
                    "🔔 Test Notification & Sound",
                    "🔔 通知テストを実行"),
        trayNotifGroup);
    btnTestNotif->setCursor(Qt::PointingHandCursor);
    btnTestNotif->setStyleSheet(
        "QPushButton { background: #EFF6FF; color: #0D47A1; font-size: 12px; font-weight: 700; border: 1px solid #BFDBFE; border-radius: 6px; padding: 7px 14px; }"
        "QPushButton:hover { background: #DBEAFE; }");
    connect(btnTestNotif, &QPushButton::clicked, this, [this]() {
        if (MailI18n::enableNotificationSound())
            QApplication::beep();
        QMessageBox::information(
            this,
            MailI18n::t("🔔 SollunaMail 새 메일 알림 테스트",
                        "🔔 SollunaMail Notification Test",
                        "🔔 通知テスト"),
            MailI18n::t("새 이메일 도착 시 이와 같이 알림과 효과음이 정상 동작합니다.",
                        "Desktop notification and sound alert are working properly.",
                        "新着メール通知および効果音が正常に動作しています。"));
    });
    testNotifRow->addWidget(btnTestNotif);
    testNotifRow->addStretch();
    tnV->addLayout(testNotifRow);

    v->addWidget(trayNotifGroup);

    v->addStretch();
    scroll->setWidget(page);
    return scroll;
}

QWidget *SettingsDialog::createPrivacyHubPage()
{
    QScrollArea *scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setStyleSheet("background: #FFFFFF;");

    QWidget *page = new QWidget(scroll);
    QVBoxLayout *v = new QVBoxLayout(page);
    v->setContentsMargins(32, 26, 32, 26);
    v->setSpacing(16);

    // Header: Shield icon + Privacy Hub
    QHBoxLayout *hdr = new QHBoxLayout();
    QLabel *iconLbl = new QLabel(page);
    iconLbl->setPixmap(MailIcons::actionIcon("privacy_shield", QColor("#1B4EA3"), 22).pixmap(22, 22));
    QLabel *titleLbl = new QLabel(
        MailI18n::t("개인정보 보호 허브 (Privacy Hub)", "Privacy Hub", "プライバシーハブ (Privacy Hub)"),
        page);
    titleLbl->setStyleSheet("font-size: 18px; font-weight: 700; color: #1F2937;");
    hdr->addWidget(iconLbl);
    hdr->addSpacing(6);
    hdr->addWidget(titleLbl);
    hdr->addStretch();
    v->addLayout(hdr);

    // Subheader (100% SollunaMail branded)
    QLabel *subTitle = new QLabel(
        MailI18n::t("귀하의 데이터는 안전하게 보호됩니다", "Your Data Stays Private", "あなたのデータは安全に保護されます"),
        page);
    subTitle->setStyleSheet("font-size: 12px; font-weight: 700; color: #1F2937; margin-top: 4px;");
    QLabel *subDesc = new QLabel(
        MailI18n::t("SollunaMail은 사용자의 이메일을 외부로 전송하거나 공유하지 않습니다. 모든 데이터는 사용자의 PC에 안전하게 보관됩니다.",
                    "SollunaMail never reads your emails or shares your private data. Everything is stored securely on your device.",
                    "SollunaMailはあなたのメールを外部に送信したり共有したりしません。すべてのデータはお使いのPCに安全に保存されます。"),
        page);
    subDesc->setWordWrap(true);
    subDesc->setStyleSheet("font-size: 11.5px; color: #6B7280; margin-bottom: 6px;");
    v->addWidget(subTitle);
    v->addWidget(subDesc);

    auto makeToggleRow = [this, page](const QString &title,
                                      const QString &desc,
                                      bool initialValue,
                                      int leftIndent,
                                      std::function<void(bool)> onToggle) -> QWidget * {
        QWidget *row = new QWidget(page);
        QHBoxLayout *h = new QHBoxLayout(row);
        h->setContentsMargins(leftIndent, 4, 0, 4);
        h->setSpacing(14);

        ToggleSwitchWidget *sw = new ToggleSwitchWidget(initialValue, row);
        connect(sw, &ToggleSwitchWidget::toggled, this, [this, onToggle](bool val) {
            onToggle(val);
            emit privacySettingsChanged();
        });

        QVBoxLayout *tv = new QVBoxLayout();
        tv->setSpacing(2);
        QLabel *tLbl = new QLabel(title, row);
        tLbl->setStyleSheet("font-size: 12px; font-weight: 700; color: #1F2937;");
        QLabel *dLbl = new QLabel(desc, row);
        dLbl->setWordWrap(true);
        dLbl->setStyleSheet("font-size: 11px; color: #6B7280;");
        tv->addWidget(tLbl);
        tv->addWidget(dLbl);

        h->addWidget(sw, 0, Qt::AlignTop);
        h->addLayout(tv, 1);
        return row;
    };

    QWidget *rowPixel = makeToggleRow(
        MailI18n::t("발신자 픽셀 추적 차단 (Block sender pixel tracking)", "Block sender pixel tracking", "送信者のピクセルトラッキングをブロック"),
        MailI18n::t("발신자가 보이지 않는 1×1 투명 추적 픽셀로 메일 열람 시간·위치·횟수를 감시하는 것을 본문에서 자동 제거합니다.",
                    "Strip invisible 1x1 tracking pixels and beacon images from incoming HTML emails.",
                    "送信者がトラッキングピクセルを使用してメールの開封を追跡するのをブロックします。"),
        m_privacy->blockPixelTracking, 18,
        [this](bool val) {
            m_privacy->blockPixelTracking = val;
            emit privacySettingsChanged();
        });
    rowPixel->setEnabled(m_privacy->privacyMode);

    QWidget *rowBlockSenders = makeToggleRow(
        MailI18n::t("차단된 발신자 자동 스팸 처리 (Block senders)", "Block senders", "ブロックした送信者を迷惑メール処理"),
        MailI18n::t("차단 목록에 등록된 발신자의 이메일이 도착하면 자동으로 스팸편지함으로 이동시킵니다.",
                    "Automatically move incoming emails from your blocked sender list to Spam.",
                    "ブロックリストに登録された送信者からのメールを自動的に迷惑メールに分類します。"),
        m_privacy->blockSenders, 18,
        [this](bool val) {
            m_privacy->blockSenders = val;
            emit privacySettingsChanged();
        });
    rowBlockSenders->setEnabled(m_privacy->privacyMode);

    // 차단된 발신자 목록 관리 버튼 행
    QWidget *blockedManageRow = new QWidget(page);
    QHBoxLayout *bmH = new QHBoxLayout(blockedManageRow);
    bmH->setContentsMargins(66, 0, 0, 4);
    QPushButton *btnManageBlocked = new QPushButton(
        MailI18n::t(QString("🚫 차단된 발신자 목록 관리 (%1명)").arg(m_privacy->blockedSendersList.size()),
                    QString("🚫 Manage Blocked Senders (%1)").arg(m_privacy->blockedSendersList.size()),
                    QString("🚫 ブロック済み送信者リスト管理 (%1件)").arg(m_privacy->blockedSendersList.size())),
        blockedManageRow);
    btnManageBlocked->setCursor(Qt::PointingHandCursor);
    btnManageBlocked->setStyleSheet(
        "QPushButton { background: #F9FAFB; color: #374151; font-size: 11.5px; font-weight: 600; "
        "border: 1px solid #D1D5DB; border-radius: 5px; padding: 5px 12px; }"
        "QPushButton:hover { background: #EFF6FF; color: #0D47A1; border-color: #93C5FD; }");
    connect(btnManageBlocked, &QPushButton::clicked, this, [this, btnManageBlocked]() {
        QDialog dlg(this);
        dlg.setWindowTitle(MailI18n::t("차단된 발신자 목록 관리", "Blocked Senders List", "ブロック済み送信者管理"));
        dlg.resize(440, 360);
        dlg.setStyleSheet("QDialog { background: #FFFFFF; }");
        QVBoxLayout *dv = new QVBoxLayout(&dlg);
        dv->setContentsMargins(18, 16, 18, 16);
        dv->setSpacing(10);

        QLabel *info = new QLabel(
            MailI18n::t("아래 목록에 등록된 이메일 주소에서 수신되는 메일은 자동으로 스팸편지함으로 분류됩니다.",
                        "Emails from addresses listed below are automatically moved to Spam.",
                        "以下のリストに登録されたアドレスからのメールは自動的に迷惑メールに分類されます。"),
            &dlg);
        info->setWordWrap(true);
        info->setStyleSheet("font-size: 12px; color: #4B5563;");
        dv->addWidget(info);

        QListWidget *lw = new QListWidget(&dlg);
        lw->setStyleSheet("QListWidget { border: 1px solid #D1D5DB; border-radius: 6px; padding: 4px; font-size: 12.5px; }");
        for (const QString &em : m_privacy->blockedSendersList)
            lw->addItem(em);
        if (lw->count() > 0)
            lw->setCurrentRow(0);
        dv->addWidget(lw, 1);

        QHBoxLayout *addRow = new QHBoxLayout();
        QLineEdit *addEdit = new QLineEdit(&dlg);
        addEdit->setPlaceholderText(MailI18n::t("이메일 또는 도메인 입력 (예: spammer@example.com, @example.com)",
                                                "Email or domain (e.g. spammer@example.com, @example.com)",
                                                "メールアドレスまたはドメイン (例: spammer@example.com, @example.com)"));
        addEdit->setStyleSheet("padding: 6px 10px; border: 1px solid #D1D5DB; border-radius: 5px; font-size: 12px;");
        QPushButton *btnAdd = new QPushButton(MailI18n::t("➕ 차단 추가", "➕ Add", "➕ 追加"), &dlg);
        btnAdd->setStyleSheet("background: #0D47A1; color: #FFFFFF; font-weight: 700; border: none; border-radius: 5px; padding: 6px 12px;");
        connect(btnAdd, &QPushButton::clicked, &dlg, [this, lw, addEdit, btnManageBlocked]() {
            QString addr = addEdit->text().trimmed();
            if (addr.contains('<') && addr.contains('>')) {
                addr = addr.section('<', 1, 1).section('>', 0, 0).trimmed();
            }
            addr = addr.toLower();
            if (addr.startsWith('@')) {
                addr = addr.mid(1).trimmed();
            }
            const bool isValidEntry = !addr.isEmpty() && (addr.contains('@') || addr.contains('.'));
            if (isValidEntry && !m_privacy->blockedSendersList.contains(addr, Qt::CaseInsensitive)) {
                m_privacy->privacyMode = true;
                m_privacy->blockSenders = true;
                m_privacy->blockedSendersList.append(addr);
                lw->addItem(addr);
                lw->setCurrentRow(lw->count() - 1);
                addEdit->clear();
                btnManageBlocked->setText(
                    MailI18n::t(QString("🚫 차단된 발신자 목록 관리 (%1명)").arg(m_privacy->blockedSendersList.size()),
                                QString("🚫 Manage Blocked Senders (%1)").arg(m_privacy->blockedSendersList.size()),
                                QString("🚫 ブロック済み送信者リスト管理 (%1件)").arg(m_privacy->blockedSendersList.size())));
                emit privacySettingsChanged();
            }
        });
        connect(addEdit, &QLineEdit::returnPressed, btnAdd, &QPushButton::click);
        addRow->addWidget(addEdit, 1);
        addRow->addWidget(btnAdd);
        dv->addLayout(addRow);

        QHBoxLayout *botRow = new QHBoxLayout();
        QPushButton *btnRemove = new QPushButton(MailI18n::t("♻️ 선택 발신자 차단 해제 (원래 편지함 복원)", "♻️ Unblock & Restore to Inbox", "♻️ 選択を解除して受信トレイへ復元"), &dlg);
        btnRemove->setStyleSheet("background: #FEF2F2; color: #DC2626; font-weight: 700; border: 1px solid #FECACA; border-radius: 5px; padding: 6px 12px;");
        connect(btnRemove, &QPushButton::clicked, &dlg, [this, lw, btnManageBlocked]() {
            int r = lw->currentRow();
            if (r < 0 && lw->count() > 0)
                r = 0;
            if (r >= 0 && r < m_privacy->blockedSendersList.size()) {
                const QString unblockedAddr = m_privacy->blockedSendersList.at(r).trimmed().toLower();
                m_privacy->blockedSendersList.removeAt(r);
                delete lw->takeItem(r);
                if (lw->count() > 0)
                    lw->setCurrentRow(qMin(r, lw->count() - 1));
                btnManageBlocked->setText(
                    MailI18n::t(QString("🚫 차단된 발신자 목록 관리 (%1명)").arg(m_privacy->blockedSendersList.size()),
                                QString("🚫 Manage Blocked Senders (%1)").arg(m_privacy->blockedSendersList.size()),
                                QString("🚫 ブロック済み送信者リスト管理 (%1件)").arg(m_privacy->blockedSendersList.size())));
                emit senderUnblocked(unblockedAddr);
                emit privacySettingsChanged();
            }
        });
        QPushButton *btnClose = new QPushButton(MailI18n::t("닫기", "Close", "閉じる"), &dlg);
        btnClose->setStyleSheet("background: #F3F4F6; color: #1F2937; font-weight: 600; border: 1px solid #D1D5DB; border-radius: 5px; padding: 6px 16px;");
        connect(btnClose, &QPushButton::clicked, &dlg, &QDialog::accept);
        botRow->addWidget(btnRemove);
        botRow->addStretch();
        botRow->addWidget(btnClose);
        dv->addLayout(botRow);

        dlg.exec();
        btnManageBlocked->setText(
            MailI18n::t(QString("🚫 차단된 발신자 목록 관리 (%1명)").arg(m_privacy->blockedSendersList.size()),
                        QString("🚫 Manage Blocked Senders (%1)").arg(m_privacy->blockedSendersList.size()),
                        QString("🚫 ブロック済み送信者リスト管理 (%1件)").arg(m_privacy->blockedSendersList.size())));
    });
    bmH->addWidget(btnManageBlocked);
    bmH->addStretch();
    blockedManageRow->setEnabled(m_privacy->privacyMode);

    QWidget *rowTrackSent = makeToggleRow(
        MailI18n::t("보낸 메일 수신 확인 요청 및 추적 (Request & track read receipts)", "Request & track read receipts", "送信メールの開封確認要求"),
        MailI18n::t("메일 발송 시 표준 수신확인 요청 헤더(Disposition-Notification-To)를 포함하고, 상대방이 열람 확인 응답을 보내면 보낸편지함에 [✅ 수신확인됨]으로 표시합니다.",
                    "Include standard MDN read-receipt headers (RFC 8098) when sending emails and mark confirmed messages in Sent.",
                    "送信時に標準開封確認ヘッダーを含め、開封応答を受信すると送信済みメールに[✅ 開封確認済]を表示します。"),
        m_privacy->trackSentEmails, 18,
        [this](bool val) {
            m_privacy->trackSentEmails = val;
            MailI18n::privacyTrackSentEmails() = (m_privacy->privacyMode && val);
        });
    rowTrackSent->setEnabled(m_privacy->privacyMode);

    QWidget *rowPrivacyMaster = makeToggleRow(
        MailI18n::t("프라이버시 보호 모드 (Privacy mode)", "Privacy mode", "プライバシー保護モード"),
        MailI18n::t("전체 개인정보 보호 기능(추적 픽셀 차단 · 차단 발신자 자동 스팸 분류 · 수신 확인 요청)을 활성화합니다.",
                    "Enable overall privacy protection features (pixel blocker, blocked sender filter, and read receipts).",
                    "すべてのプライバシー保護機能を有効にします。"),
        m_privacy->privacyMode, 0,
        [this, rowPixel, rowBlockSenders, blockedManageRow, rowTrackSent](bool val) {
            m_privacy->privacyMode = val;
            MailI18n::privacyTrackSentEmails() = (m_privacy->privacyMode && m_privacy->trackSentEmails);
            if (rowPixel)         rowPixel->setEnabled(val);
            if (rowBlockSenders)  rowBlockSenders->setEnabled(val);
            if (blockedManageRow) blockedManageRow->setEnabled(val);
            if (rowTrackSent)     rowTrackSent->setEnabled(val);
        });

    v->addWidget(rowPrivacyMaster);
    v->addWidget(rowPixel);
    v->addWidget(rowBlockSenders);
    v->addWidget(blockedManageRow);
    v->addWidget(rowTrackSent);

    v->addWidget(makeToggleRow(
        MailI18n::t("모든 외부 이미지 자동 로드 차단 (Block all external images)", "Block all external images", "すべての外部画像をブロック"),
        MailI18n::t("열람창 상단의 '외부 이미지 표시' 버튼을 누르기 전까지 본문 내 외부 웹 이미지(http/https)를 차단합니다.",
                    "Block external images in emails until you click 'Show external images' in the reading pane.",
                    "「外部画像を表示」をクリックするまでメール内の外部画像をブロックします。"),
        m_privacy->blockAllImages, 0,
        [this](bool val) { m_privacy->blockAllImages = val; }));

    // Avatar section (외부 진단 데이터 전송 항목 제거 — 로컬 아바타 다운로드 여부만 관리)
    QLabel *dsHdr = new QLabel(
        MailI18n::t("프로필 아바타 설정 (Profile Avatars)", "Profile Avatars", "プロフィールアバター設定"),
        page);
    dsHdr->setStyleSheet("font-size: 15px; font-weight: 600; color: #4B5563; margin-top: 12px;");
    v->addWidget(dsHdr);

    v->addWidget(makeToggleRow(
        MailI18n::t("발신자 및 계정 프로필 아바타 자동 다운로드", "Download profile avatars automatically", "プロフィールアバターを自動取得"),
        MailI18n::t("연결된 Google 계정 프로필 사진 및 발신자 도메인 파비콘을 자동으로 가져옵니다. (끄면 외부 이미지 요청을 하지 않고 이니셜 아바타만 사용합니다.)",
                    "Automatically fetch account profile photos and domain icons. Turn off to use local initials only.",
                    "アカウントのプロフィール写真やドメインアイコンを自動取得します。"),
        m_privacy->downloadAvatars, 0,
        [this](bool val) {
            m_privacy->downloadAvatars = val;
            MailI18n::privacyDownloadAvatars() = val;
        }));

    v->addStretch();
    scroll->setWidget(page);
    return scroll;
}

QWidget *SettingsDialog::createAccountsPage()
{
    QScrollArea *scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setStyleSheet("background: #FFFFFF;");

    QWidget *page = new QWidget(scroll);
    QHBoxLayout *masterDetail = new QHBoxLayout(page);
    masterDetail->setContentsMargins(28, 22, 32, 24);
    masterDetail->setSpacing(30);

    // =========================================================================
    // [LEFT COLUMN] Accounts Card List + [Add account] Button (275px)
    // =========================================================================
    QWidget *leftCol = new QWidget(page);
    leftCol->setFixedWidth(275);
    QVBoxLayout *leftV = new QVBoxLayout(leftCol);
    leftV->setContentsMargins(0, 0, 0, 0);
    leftV->setSpacing(12);

    QHBoxLayout *accTitleRow = new QHBoxLayout();
    QLabel *accIconLbl = new QLabel(leftCol);
    accIconLbl->setPixmap(MailIcons::settingsNavIcon(1, QColor("#2563EB"), 22).pixmap(22, 22));
    QLabel *accTitleLbl = new QLabel(
        MailI18n::t("계정 관리 (Accounts)", "Accounts", "アカウント (Accounts)"),
        leftCol);
    accTitleLbl->setStyleSheet("font-size: 18px; font-weight: 600; color: #1F2937;");
    accTitleRow->addWidget(accIconLbl);
    accTitleRow->addSpacing(6);
    accTitleRow->addWidget(accTitleLbl);
    accTitleRow->addStretch();
    leftV->addLayout(accTitleRow);

    m_accountsListWidget = new QListWidget(leftCol);
    m_accountsListWidget->setFrameShape(QFrame::NoFrame);
    m_accountsListWidget->setSpacing(4);
    m_accountsListWidget->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_accountsListWidget->setStyleSheet(R"(
        QListWidget {
            background: transparent;
            outline: none;
            border: none;
        }
        QListWidget::item {
            border-radius: 7px;
            padding: 0px;
            background: transparent;
        }
        QListWidget::item:hover {
            background: #F3F4F6;
        }
        QListWidget::item:selected {
            background: #EBECEF;
        }
    )");
    leftV->addWidget(m_accountsListWidget);

    QPushButton *btnAddAccount = new QPushButton(
        MailI18n::t("계정 추가 (Add account)", "Add account", "アカウント追加 (Add account)"),
        leftCol);
    btnAddAccount->setCursor(Qt::PointingHandCursor);
    btnAddAccount->setFixedHeight(36);
    btnAddAccount->setMaximumWidth(200);
    btnAddAccount->setStyleSheet(
        "QPushButton { background: #FFFFFF; color: #111827; font-size: 12.5px; font-weight: 700; "
        "border: 1px solid #D1D5DB; border-radius: 6px; padding: 6px 18px; }"
        "QPushButton:hover { background: #F9FAFB; border-color: #9CA3AF; }");
    connect(btnAddAccount, &QPushButton::clicked, this, [this, btnAddAccount]() {
        QMenu addMenu(this);
        addMenu.setStyleSheet(
            "QMenu { background: #FFFFFF; border: 1px solid #D1D5DB; border-radius: 6px; padding: 4px; }"
            "QMenu::item { padding: 8px 16px; font-size: 12.5px; color: #111827; border-radius: 4px; }"
            "QMenu::item:selected { background: #EFF6FF; color: #0D47A1; }");
        QAction *actGoogle = addMenu.addAction(
            MailI18n::t("🌐 Google (Gmail · 캘린더 · 주소록)",
                        "🌐 Google (Gmail · Calendar · Contacts)",
                        "🌐 Google (Gmail・カレンダー・連絡先)"));
        QAction *actICloud = addMenu.addAction(
            MailI18n::t("☁️ Apple iCloud 계정 추가",
                        "☁️ Apple iCloud Account",
                        "☁️ Apple iCloud アカウント"));
        QAction *actNaver = addMenu.addAction(
            MailI18n::t("🟢 네이버 (Naver) 메일 추가",
                        "🟢 Naver Mail Account",
                        "🟢 Naver メールアカウント"));
        QAction *actKakao = addMenu.addAction(
            MailI18n::t("🟡 다음 · 카카오 (Daum / Kakao) 메일 추가",
                        "🟡 Daum / Kakao Mail Account",
                        "🟡 Daum / Kakao メールアカウント"));
        addMenu.addSeparator();
        QAction *actImap = addMenu.addAction(
            MailI18n::t("📥 IMAP 계정 직접 설정 (회사 / 일반 메일 · Mailtrap)",
                        "📥 Custom IMAP Account (Work / Custom / Mailtrap)",
                        "📥 IMAP アカウント設定"));
        QAction *actPop3 = addMenu.addAction(
            MailI18n::t("📬 POP3 계정 직접 설정",
                        "📬 Custom POP3 Account",
                        "📬 POP3 アカウント設定"));
        QAction *picked = addMenu.exec(btnAddAccount->mapToGlobal(QPoint(0, btnAddAccount->height() + 2)));
        if (picked == actGoogle)
            onAddAccountClicked("Google");
        else if (picked == actICloud)
            onAddAccountClicked("ICloud");
        else if (picked == actNaver)
            onAddAccountClicked("Naver");
        else if (picked == actKakao)
            onAddAccountClicked("DaumKakao");
        else if (picked == actImap)
            onAddAccountClicked("IMAP");
        else if (picked == actPop3)
            onAddAccountClicked("POP3");
    });
    leftV->addWidget(btnAddAccount);
    leftV->addStretch();

    masterDetail->addWidget(leftCol, 0, Qt::AlignTop);

    // =========================================================================
    // [RIGHT COLUMN] 4 Sub-tabs: Account details | Aliases | Settings | Apps & services
    // =========================================================================
    QWidget *rightCol = new QWidget(page);
    QVBoxLayout *rightV = new QVBoxLayout(rightCol);
    rightV->setContentsMargins(0, 0, 0, 0);
    rightV->setSpacing(18);

    // Top Segmented Tab Bar
    QWidget *segBar = new QWidget(rightCol);
    segBar->setStyleSheet("background: #EBECEF; border-radius: 6px;");
    QHBoxLayout *segRow = new QHBoxLayout(segBar);
    segRow->setContentsMargins(3, 3, 3, 3);
    segRow->setSpacing(2);

    m_accountSubTabBtns.clear();
    const QStringList subTabTitles = {
        MailI18n::t("Account details (계정 상세)", "Account details", "アカウント詳細"),
        MailI18n::t("Aliases (별칭)",              "Aliases",         "エイリアス"),
        MailI18n::t("Settings (서버 설정)",        "Settings",        "サーバー設定"),
        MailI18n::t("Apps & services (연동 앱)",   "Apps & services", "連携サービス")
    };

    m_accountSubTabStack = new QStackedWidget(rightCol);

    auto updateSubTabStyles = [this](int activeSubIdx) {
        for (int i = 0; i < m_accountSubTabBtns.size(); ++i) {
            bool active = (i == activeSubIdx);
            m_accountSubTabBtns[i]->setStyleSheet(active
                ? "QPushButton { background: #FFFFFF; color: #111827; font-size: 12px; font-weight: 700; "
                  "border: 1px solid #D1D5DB; border-radius: 5px; padding: 6px 14px; }"
                : "QPushButton { background: transparent; color: #374151; font-size: 12px; font-weight: 600; "
                  "border: none; border-radius: 5px; padding: 6px 14px; }"
                  "QPushButton:hover { color: #111827; }");
        }
        if (m_accountSubTabStack)
            m_accountSubTabStack->setCurrentIndex(activeSubIdx);
    };

    for (int i = 0; i < subTabTitles.size(); ++i) {
        QPushButton *sb = new QPushButton(subTabTitles[i], segBar);
        sb->setCursor(Qt::PointingHandCursor);
        connect(sb, &QPushButton::clicked, this, [updateSubTabStyles, i]() {
            updateSubTabStyles(i);
        });
        segRow->addWidget(sb);
        m_accountSubTabBtns.append(sb);
    }

    QHBoxLayout *segWrap = new QHBoxLayout();
    segWrap->addWidget(segBar);
    segWrap->addStretch();
    rightV->addLayout(segWrap);

    // -------------------------------------------------------------------------
    // SUB-TAB 0: Account details (Custom avatar + 33-icon Grid + Account color + Details)
    // -------------------------------------------------------------------------
    QWidget *tabDetails = new QWidget(m_accountSubTabStack);
    QVBoxLayout *detV = new QVBoxLayout(tabDetails);
    detV->setContentsMargins(0, 6, 0, 0);
    detV->setSpacing(14);

    QLabel *customAvTitle = new QLabel(
        MailI18n::t("Custom avatar (아바타 아이콘)", "Custom avatar", "カスタムアバター"),
        tabDetails);
    customAvTitle->setStyleSheet("font-size: 16px; font-weight: 600; color: #374151;");
    detV->addWidget(customAvTitle);

    QHBoxLayout *uploadRow = new QHBoxLayout();
    uploadRow->setSpacing(14);
    QPushButton *btnUploadImg = new QPushButton(
        MailI18n::t("🖼️  Upload image (이미지 업로드)", "🖼️  Upload image", "🖼️  画像をアップロード"),
        tabDetails);
    btnUploadImg->setCursor(Qt::PointingHandCursor);
    btnUploadImg->setFixedHeight(36);
    btnUploadImg->setStyleSheet(
        "QPushButton { background: #FFFFFF; color: #111827; font-size: 12.5px; font-weight: 700; "
        "border: 1px solid #D1D5DB; border-radius: 5px; padding: 6px 16px; }"
        "QPushButton:hover { background: #F9FAFB; border-color: #9CA3AF; }");
    connect(btnUploadImg, &QPushButton::clicked, this, [this]() {
        if (m_selectedAccountIdx < 0 || m_selectedAccountIdx >= m_accounts->size())
            return;
        QString filePath = QFileDialog::getOpenFileName(
            this,
            MailI18n::t("아바타 이미지 선택 (권장 500x500px)", "Select Avatar Image (Recommended 500x500px)", "アバター画像を選択"),
            QString(),
            "Images (*.png *.jpg *.jpeg *.webp *.bmp *.ico)");
        if (filePath.isEmpty())
            return;
        QImage img(filePath);
        if (img.isNull())
            return;
        QImage scaled = img.scaled(256, 256, Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation);
        QByteArray pngBytes;
        QBuffer buf(&pngBytes);
        buf.open(QIODevice::WriteOnly);
        scaled.save(&buf, "PNG");
        (*m_accounts)[m_selectedAccountIdx].avatarPngBase64 = QString::fromLatin1(pngBytes.toBase64());
        (*m_accounts)[m_selectedAccountIdx].avatarIconPreset = 0;
        refreshAccountsList();
        loadSelectedAccountToRightPanel(m_selectedAccountIdx);
        emit accountsUpdated();
    });

    QLabel *uploadHint = new QLabel(
        MailI18n::t("Pick your avatar icon from the list or upload a custom one. Recommended size: 500x500px.\n"
                    "(아래 33종 아이콘에서 선택하거나 직접 아바타 이미지를 업로드하세요.)",
                    "Pick your avatar icon from the list or upload a custom one. Recommended size: 500x500px.",
                    "リストからアバターアイコンを選択するか、カスタム画像をアップロードしてください（推奨: 500x500px）。"),
        tabDetails);
    uploadHint->setWordWrap(true);
    uploadHint->setStyleSheet("font-size: 11.5px; color: #6B7280;");
    uploadRow->addWidget(btnUploadImg, 0, Qt::AlignTop);
    uploadRow->addWidget(uploadHint, 1);
    detV->addLayout(uploadRow);

    // 3x11 (33 icons) Avatar Grid
    QWidget *gridBox = new QWidget(tabDetails);
    QGridLayout *avGrid = new QGridLayout(gridBox);
    avGrid->setContentsMargins(0, 4, 0, 4);
    avGrid->setHorizontalSpacing(6);
    avGrid->setVerticalSpacing(6);

    m_avatarGridBtns.clear();
    for (int presetIdx = 0; presetIdx < 33; ++presetIdx) {
        QPushButton *avBtn = new QPushButton(gridBox);
        avBtn->setFixedSize(40, 40);
        avBtn->setIconSize(QSize(38, 38));
        avBtn->setCursor(Qt::PointingHandCursor);
        avBtn->setStyleSheet("QPushButton { border: none; background: transparent; padding: 0px; }");
        connect(avBtn, &QPushButton::clicked, this, [this, presetIdx]() {
            if (m_selectedAccountIdx < 0 || m_selectedAccountIdx >= m_accounts->size())
                return;
            (*m_accounts)[m_selectedAccountIdx].avatarIconPreset = presetIdx;
            if (presetIdx == 2)
                (*m_accounts)[m_selectedAccountIdx].visualStyle = AvatarVisualStyle::TealSquareS;
            refreshAccountsList();
            loadSelectedAccountToRightPanel(m_selectedAccountIdx);
            emit accountsUpdated();
        });
        avGrid->addWidget(avBtn, presetIdx / 11, presetIdx % 11);
        m_avatarGridBtns.append(avBtn);
    }
    gridBox->setMaximumWidth(510);
    detV->addWidget(gridBox);

    // Account color picker row
    QHBoxLayout *colorRow = new QHBoxLayout();
    colorRow->setSpacing(10);
    m_accountColorBtn = new QPushButton("  🎨  ⌄", tabDetails);
    m_accountColorBtn->setCursor(Qt::PointingHandCursor);
    m_accountColorBtn->setFixedSize(56, 34);
    m_accountColorBtn->setStyleSheet(
        "QPushButton { background: #FFFFFF; border: 1px solid #D1D5DB; border-radius: 5px; font-size: 12px; color: #374151; }"
        "QPushButton:hover { background: #F9FAFB; }");
    connect(m_accountColorBtn, &QPushButton::clicked, this, [this]() {
        if (m_selectedAccountIdx < 0 || m_selectedAccountIdx >= m_accounts->size())
            return;
        QColor initial = (*m_accounts)[m_selectedAccountIdx].avatarColor.isValid()
                             ? (*m_accounts)[m_selectedAccountIdx].avatarColor
                             : QColor("#0284C7");
        QColor chosen = QColorDialog::getColor(initial, this, MailI18n::t("계정 대표 색상 선택", "Pick Account Color", "アカウントカラーを選択"));
        if (chosen.isValid()) {
            (*m_accounts)[m_selectedAccountIdx].avatarColor = chosen;
            loadSelectedAccountToRightPanel(m_selectedAccountIdx);
            refreshAccountsList();
            emit accountsUpdated();
        }
    });
    QLabel *colorLbl = new QLabel(
        MailI18n::t("Account color (계정 구분 색상)", "Account color", "アカウントカラー"),
        tabDetails);
    colorLbl->setStyleSheet("font-size: 12px; color: #4B5563;");
    colorRow->addWidget(m_accountColorBtn);
    colorRow->addWidget(colorLbl);
    colorRow->addStretch();
    detV->addLayout(colorRow);

    // Details Form (Your name, Email, Account label, Include in unified account, Use a different reply-to address, Save)
    QLabel *detailsHdr = new QLabel(
        MailI18n::t("Details (계정 세부 정보)", "Details", "詳細情報 (Details)"),
        tabDetails);
    detailsHdr->setStyleSheet("font-size: 16px; font-weight: 600; color: #374151; margin-top: 6px;");
    detV->addWidget(detailsHdr);

    auto makeFieldLabel = [tabDetails](const QString &txt) {
        QLabel *l = new QLabel(txt, tabDetails);
        l->setStyleSheet("font-size: 11.5px; color: #6B7280; margin-top: 2px;");
        return l;
    };

    detV->addWidget(makeFieldLabel(MailI18n::t("Your name (표시 이름):", "Your name:", "表示名 (Your name):")));
    m_accYourNameEdit = new QLineEdit(tabDetails);
    m_accYourNameEdit->setMaximumWidth(510);
    m_accYourNameEdit->setStyleSheet(
        "QLineEdit { background: #FFFFFF; border: 1px solid #D1D5DB; border-radius: 5px; padding: 8px 12px; font-size: 13px; color: #111827; }"
        "QLineEdit:focus { border-color: #2563EB; }");
    detV->addWidget(m_accYourNameEdit);

    detV->addWidget(makeFieldLabel(MailI18n::t("Email (이메일 주소):", "Email:", "メールアドレス (Email):")));
    m_accEmailReadEdit = new QLineEdit(tabDetails);
    m_accEmailReadEdit->setReadOnly(true);
    m_accEmailReadEdit->setMaximumWidth(510);
    m_accEmailReadEdit->setStyleSheet(
        "QLineEdit { background: #F3F4F6; border: 1px solid #E5E7EB; border-radius: 5px; padding: 8px 12px; font-size: 13px; color: #374151; }");
    detV->addWidget(m_accEmailReadEdit);

    detV->addWidget(makeFieldLabel(MailI18n::t("Account label (계정 별칭 라벨):", "Account label:", "アカウントラベル:")));
    m_accLabelEdit = new QLineEdit(tabDetails);
    m_accLabelEdit->setPlaceholderText(MailI18n::t("예: 업무용 / 개인용 / 개발팀", "e.g. Work / Personal", "例: 業務用 / 個人用"));
    m_accLabelEdit->setMaximumWidth(510);
    m_accLabelEdit->setStyleSheet(
        "QLineEdit { background: #FFFFFF; border: 1px solid #D1D5DB; border-radius: 5px; padding: 8px 12px; font-size: 13px; color: #111827; }"
        "QLineEdit:focus { border-color: #2563EB; }");
    detV->addWidget(m_accLabelEdit);

    m_accUnifiedCheck = new QCheckBox(
        MailI18n::t("Include in unified account (통합 메일함에 이 계정 포함)", "Include in unified account", "統合アカウントに含める"),
        tabDetails);
    m_accUnifiedCheck->setChecked(true);
    m_accUnifiedCheck->setStyleSheet("QCheckBox { font-size: 12.5px; color: #374151; margin-top: 4px; }");
    detV->addWidget(m_accUnifiedCheck);

    m_accReplyToCheck = new QCheckBox(
        MailI18n::t("Use a different reply-to address (별도의 답장 수신 이메일 주소 사용)", "Use a different reply-to address", "別の返信先アドレスを使用する"),
        tabDetails);
    m_accReplyToCheck->setStyleSheet("QCheckBox { font-size: 12.5px; color: #374151; }");
    detV->addWidget(m_accReplyToCheck);

    m_accReplyToEdit = new QLineEdit(tabDetails);
    m_accReplyToEdit->setPlaceholderText("reply-to@domain.com");
    m_accReplyToEdit->setMaximumWidth(510);
    m_accReplyToEdit->setVisible(false);
    m_accReplyToEdit->setStyleSheet(
        "QLineEdit { background: #FFFFFF; border: 1px solid #D1D5DB; border-radius: 5px; padding: 8px 12px; font-size: 12.5px; color: #111827; }");
    detV->addWidget(m_accReplyToEdit);
    connect(m_accReplyToCheck, &QCheckBox::toggled, m_accReplyToEdit, &QWidget::setVisible);

    QHBoxLayout *saveBtnRow = new QHBoxLayout();
    QPushButton *btnSaveDetails = new QPushButton(
        MailI18n::t("Save (저장)", "Save", "保存 (Save)"),
        tabDetails);
    btnSaveDetails->setCursor(Qt::PointingHandCursor);
    btnSaveDetails->setFixedSize(132, 36);
    btnSaveDetails->setStyleSheet(
        "QPushButton { background: #0D47A1; color: #FFFFFF; font-size: 12.5px; font-weight: 700; border: none; border-radius: 5px; }"
        "QPushButton:hover { background: #08307E; }");
    connect(btnSaveDetails, &QPushButton::clicked, this, &SettingsDialog::saveRightPanelToSelectedAccount);
    saveBtnRow->addWidget(btnSaveDetails);
    saveBtnRow->addStretch();
    detV->addLayout(saveBtnRow);
    detV->addStretch();

    m_accountSubTabStack->addWidget(tabDetails);

    // -------------------------------------------------------------------------
    // SUB-TAB 1: Aliases (Sender Aliases Management)
    // -------------------------------------------------------------------------
    QWidget *tabAliases = new QWidget(m_accountSubTabStack);
    QVBoxLayout *alV = new QVBoxLayout(tabAliases);
    alV->setContentsMargins(0, 8, 0, 0);
    alV->setSpacing(12);

    QLabel *alTitle = new QLabel(
        MailI18n::t("Aliases (발신 별칭 이메일 관리)", "Email Aliases", "エイリアス管理 (Aliases)"),
        tabAliases);
    alTitle->setStyleSheet("font-size: 16px; font-weight: 600; color: #374151;");
    QLabel *alDesc = new QLabel(
        MailI18n::t("이 계정에서 메일을 보낼 때 추가로 사용할 발신자 별칭(Alias) 주소를 등록합니다.",
                    "Manage additional sender alias addresses associated with this email account.",
                    "このアカウントからメールを送信する際に使用する追加のエイリアスアドレスを管理します。"),
        tabAliases);
    alDesc->setWordWrap(true);
    alDesc->setStyleSheet("font-size: 12px; color: #6B7280;");
    alV->addWidget(alTitle);
    alV->addWidget(alDesc);

    QHBoxLayout *addAliasRow = new QHBoxLayout();
    m_accNewAliasEdit = new QLineEdit(tabAliases);
    m_accNewAliasEdit->setPlaceholderText("alias@yourdomain.com");
    m_accNewAliasEdit->setMaximumWidth(360);
    m_accNewAliasEdit->setStyleSheet("padding: 8px 12px; border: 1px solid #D1D5DB; border-radius: 5px; font-size: 12.5px;");
    QPushButton *btnAddAlias = new QPushButton(MailI18n::t("+ 별칭 추가 (Add alias)", "+ Add alias", "+ エイリアス追加"), tabAliases);
    btnAddAlias->setCursor(Qt::PointingHandCursor);
    btnAddAlias->setStyleSheet("background: #0D47A1; color: #FFFFFF; font-weight: 700; padding: 8px 14px; border-radius: 5px; border: none;");
    addAliasRow->addWidget(m_accNewAliasEdit);
    addAliasRow->addWidget(btnAddAlias);
    addAliasRow->addStretch();
    alV->addLayout(addAliasRow);

    m_accAliasesList = new QListWidget(tabAliases);
    m_accAliasesList->setMaximumWidth(510);
    m_accAliasesList->setMaximumHeight(220);
    m_accAliasesList->setStyleSheet("border: 1px solid #D1D5DB; border-radius: 6px; padding: 4px; background: #FFFFFF;");
    alV->addWidget(m_accAliasesList);

    connect(btnAddAlias, &QPushButton::clicked, this, [this]() {
        if (m_selectedAccountIdx < 0 || m_selectedAccountIdx >= m_accounts->size())
            return;
        QString al = m_accNewAliasEdit->text().trimmed();
        if (al.isEmpty() || !al.contains('@'))
            return;
        AccountItem &acc = (*m_accounts)[m_selectedAccountIdx];
        if (!acc.aliases.contains(al, Qt::CaseInsensitive)) {
            acc.aliases.append(al);
            m_accAliasesList->addItem(al);
            m_accNewAliasEdit->clear();
            emit accountsUpdated();
        }
    });

    QPushButton *btnDelAlias = new QPushButton(MailI18n::t("선택한 별칭 삭제 (Remove alias)", "Remove selected alias", "選択したエイリアスを削除"), tabAliases);
    btnDelAlias->setCursor(Qt::PointingHandCursor);
    btnDelAlias->setMaximumWidth(220);
    btnDelAlias->setStyleSheet("background: #FEF2F2; color: #DC2626; font-weight: 600; padding: 7px 14px; border: 1px solid #FECACA; border-radius: 5px;");
    connect(btnDelAlias, &QPushButton::clicked, this, [this]() {
        if (m_selectedAccountIdx < 0 || m_selectedAccountIdx >= m_accounts->size() || !m_accAliasesList->currentItem())
            return;
        int row = m_accAliasesList->currentRow();
        AccountItem &acc = (*m_accounts)[m_selectedAccountIdx];
        if (row >= 0 && row < acc.aliases.size()) {
            acc.aliases.removeAt(row);
            delete m_accAliasesList->takeItem(row);
            emit accountsUpdated();
        }
    });
    alV->addWidget(btnDelAlias);
    alV->addStretch();

    m_accountSubTabStack->addWidget(tabAliases);

    // -------------------------------------------------------------------------
    // SUB-TAB 2: Settings (Incoming IMAP & Outgoing SMTP / Mailtrap / Mailgun)
    // -------------------------------------------------------------------------
    QWidget *tabServer = new QWidget(m_accountSubTabStack);
    QVBoxLayout *srvV = new QVBoxLayout(tabServer);
    srvV->setContentsMargins(0, 8, 0, 0);
    srvV->setSpacing(12);

    QLabel *srvTitle = new QLabel(
        MailI18n::t("Server Settings (받는 서버 IMAP / 보내는 서버 SMTP · Mailtrap 설정)",
                    "Incoming (IMAP) & Outgoing (SMTP) Server Settings",
                    "受信(IMAP)・送信(SMTP / Mailtrap) サーバー設定"),
        tabServer);
    srvTitle->setStyleSheet("font-size: 16px; font-weight: 600; color: #374151;");
    srvV->addWidget(srvTitle);

    m_accountDetailSummary = new QLabel(tabServer);
    m_accountDetailSummary->setWordWrap(true);
    m_accountDetailSummary->setMaximumWidth(540);
    m_accountDetailSummary->setStyleSheet(
        "background: #F8FAFC; border: 1px solid #E2E8F0; border-radius: 7px; padding: 10px 14px; font-size: 12px; color: #334155;");
    srvV->addWidget(m_accountDetailSummary);

    QFormLayout *srvForm = new QFormLayout();
    srvForm->setSpacing(9);

    m_accInlineImapHost = new QLineEdit(tabServer);
    m_accInlineImapHost->setMaximumWidth(360);
    m_accInlineImapHost->setStyleSheet("background: #FFFFFF; color: #111827; padding: 6px 10px; border: 1px solid #D1D5DB; border-radius: 5px;");

    m_accInlineImapPort = new QSpinBox(tabServer);
    m_accInlineImapPort->setRange(1, 65535);
    m_accInlineImapPort->setValue(993);
    m_accInlineImapPort->setMaximumWidth(120);
    m_accInlineImapPort->setStyleSheet("background: #FFFFFF; color: #111827; padding: 5px 8px; border: 1px solid #D1D5DB; border-radius: 5px;");

    m_accInlineImapSec = new QComboBox(tabServer);
    m_accInlineImapSec->addItems({"SSL/TLS", "STARTTLS", "None"});
    m_accInlineImapSec->setMaximumWidth(160);
    m_accInlineImapSec->setStyleSheet("background: #FFFFFF; color: #111827; padding: 5px 8px; border: 1px solid #D1D5DB; border-radius: 5px;");

    m_accInlineSmtpPreset = new QComboBox(tabServer);
    m_accInlineSmtpPreset->addItem(MailI18n::t("받는 서버와 동일 (Same as Incoming)", "Same as Incoming", "受信サーバーと同じ (Same as Incoming)"), "SameAsIncoming");
    m_accInlineSmtpPreset->addItem("Mailtrap Email Sending (live.smtp.mailtrap.io:587)", "MailtrapLive");
    m_accInlineSmtpPreset->addItem("Mailtrap Sandbox (sandbox.smtp.mailtrap.io:2525)", "MailtrapSandbox");
    m_accInlineSmtpPreset->addItem("Mailgun SMTP (smtp.mailgun.org:587)", "Mailgun");
    m_accInlineSmtpPreset->addItem("SendGrid SMTP (smtp.sendgrid.net:587)", "SendGrid");
    m_accInlineSmtpPreset->addItem(MailI18n::t("직접 입력 (Custom SMTP)", "Custom SMTP", "カスタム SMTP (直接入力)"), "Custom");
    m_accInlineSmtpPreset->setMaximumWidth(360);
    m_accInlineSmtpPreset->setStyleSheet("background: #FFFFFF; color: #111827; padding: 5px 8px; border: 1px solid #D1D5DB; border-radius: 5px;");

    m_accInlineSmtpHost = new QLineEdit(tabServer);
    m_accInlineSmtpHost->setMaximumWidth(360);
    m_accInlineSmtpHost->setStyleSheet("background: #FFFFFF; color: #111827; padding: 6px 10px; border: 1px solid #D1D5DB; border-radius: 5px;");

    m_accInlineSmtpPort = new QSpinBox(tabServer);
    m_accInlineSmtpPort->setRange(1, 65535);
    m_accInlineSmtpPort->setValue(465);
    m_accInlineSmtpPort->setMaximumWidth(120);
    m_accInlineSmtpPort->setStyleSheet("background: #FFFFFF; color: #111827; padding: 5px 8px; border: 1px solid #D1D5DB; border-radius: 5px;");

    m_accInlineSmtpSec = new QComboBox(tabServer);
    m_accInlineSmtpSec->addItems({"SSL/TLS", "STARTTLS", "None"});
    m_accInlineSmtpSec->setMaximumWidth(160);
    m_accInlineSmtpSec->setStyleSheet("background: #FFFFFF; color: #111827; padding: 5px 8px; border: 1px solid #D1D5DB; border-radius: 5px;");

    m_accInlineSmtpUser = new QLineEdit(tabServer);
    m_accInlineSmtpUser->setPlaceholderText("SMTP Username (e.g. api / smtp@mailtrap.io)");
    m_accInlineSmtpUser->setMaximumWidth(360);
    m_accInlineSmtpUser->setStyleSheet("background: #FFFFFF; color: #111827; padding: 6px 10px; border: 1px solid #D1D5DB; border-radius: 5px;");

    m_accInlineSmtpPass = new QLineEdit(tabServer);
    m_accInlineSmtpPass->setEchoMode(QLineEdit::Password);
    m_accInlineSmtpPass->setPlaceholderText("Password / API Token");
    m_accInlineSmtpPass->setMaximumWidth(360);
    m_accInlineSmtpPass->setStyleSheet("background: #FFFFFF; color: #111827; padding: 6px 10px; border: 1px solid #D1D5DB; border-radius: 5px;");

    QWidget *inlineSmtpPassRowW = new QWidget(tabServer);
    QHBoxLayout *inlineSmtpPassRowH = new QHBoxLayout(inlineSmtpPassRowW);
    inlineSmtpPassRowH->setContentsMargins(0, 0, 0, 0);
    inlineSmtpPassRowH->setSpacing(8);
    QPushButton *inlineSmtpPassVisBtn = createPasswordVisibilityToggleBtn(m_accInlineSmtpPass, inlineSmtpPassRowW);
    inlineSmtpPassRowH->addWidget(m_accInlineSmtpPass);
    inlineSmtpPassRowH->addWidget(inlineSmtpPassVisBtn);
    inlineSmtpPassRowH->addStretch();

    connect(m_accInlineSmtpPreset, &QComboBox::currentIndexChanged, this, [this](int) {
        QString p = m_accInlineSmtpPreset->currentData().toString();
        if (p == "MailtrapLive") {
            m_accInlineSmtpHost->setText("live.smtp.mailtrap.io");
            m_accInlineSmtpPort->setValue(587);
            m_accInlineSmtpSec->setCurrentText("STARTTLS");
            if (m_accInlineSmtpUser->text().trimmed().isEmpty())
                m_accInlineSmtpUser->setText("api");
        } else if (p == "MailtrapSandbox") {
            m_accInlineSmtpHost->setText("sandbox.smtp.mailtrap.io");
            m_accInlineSmtpPort->setValue(2525);
            m_accInlineSmtpSec->setCurrentText("STARTTLS");
        } else if (p == "Mailgun") {
            m_accInlineSmtpHost->setText("smtp.mailgun.org");
            m_accInlineSmtpPort->setValue(587);
            m_accInlineSmtpSec->setCurrentText("STARTTLS");
        } else if (p == "SendGrid") {
            m_accInlineSmtpHost->setText("smtp.sendgrid.net");
            m_accInlineSmtpPort->setValue(587);
            m_accInlineSmtpSec->setCurrentText("STARTTLS");
            m_accInlineSmtpUser->setText("apikey");
        }
    });

    srvForm->addRow(MailI18n::t("받는 서버 (IMAP/POP3 Host):", "Incoming Host:", "受信サーバー (Host):"), m_accInlineImapHost);
    srvForm->addRow(MailI18n::t("받는 서버 포트 (Port):", "Incoming Port:", "受信ポート (Port):"), m_accInlineImapPort);
    srvForm->addRow(MailI18n::t("받는 서버 보안 (Security):", "Incoming Security:", "受信セキュリティ:"), m_accInlineImapSec);
    srvForm->addRow(MailI18n::t("보내는 서버 유형 (Preset):", "SMTP Preset:", "送信サーバー種別:"), m_accInlineSmtpPreset);
    srvForm->addRow(MailI18n::t("보내는 서버 (SMTP Host):", "SMTP Host:", "送信サーバー (Host):"), m_accInlineSmtpHost);
    srvForm->addRow(MailI18n::t("보내는 서버 포트 (Port):", "SMTP Port:", "送信ポート (Port):"), m_accInlineSmtpPort);
    srvForm->addRow(MailI18n::t("보내는 서버 보안 (Security):", "SMTP Security:", "送信セキュリティ:"), m_accInlineSmtpSec);
    srvForm->addRow(MailI18n::t("보내는 서버 ID (Username):", "SMTP Username:", "送信サーバー ID:"), m_accInlineSmtpUser);
    srvForm->addRow(MailI18n::t("암호 / 앱 비밀번호:", "Password / App Password:", "パスワード / アプリパスワード:"), inlineSmtpPassRowW);
    srvV->addLayout(srvForm);

    QHBoxLayout *srvBtnRow = new QHBoxLayout();
    QPushButton *btnSaveSrv = new QPushButton(MailI18n::t("Save (서버 설정 저장)", "Save Server Settings", "保存"), tabServer);
    btnSaveSrv->setCursor(Qt::PointingHandCursor);
    btnSaveSrv->setStyleSheet("background: #0D47A1; color: #FFFFFF; font-weight: 700; padding: 8px 16px; border-radius: 5px; border: none;");
    connect(btnSaveSrv, &QPushButton::clicked, this, &SettingsDialog::saveRightPanelToSelectedAccount);

    QPushButton *btnFullWizard = new QPushButton(MailI18n::t("✏️ 전체 설정 마법사 열기", "✏️ Open Full Wizard", "✏️ 詳細ウィザードを開く"), tabServer);
    btnFullWizard->setCursor(Qt::PointingHandCursor);
    btnFullWizard->setStyleSheet("background: #FFFFFF; color: #374151; font-weight: 600; padding: 8px 14px; border-radius: 5px; border: 1px solid #D1D5DB;");
    connect(btnFullWizard, &QPushButton::clicked, this, &SettingsDialog::onEditSelectedAccount);

    QPushButton *btnSyncAcc = new QPushButton(MailI18n::t("🔄 지금 동기화", "🔄 Sync Now", "🔄 今すぐ同期"), tabServer);
    btnSyncAcc->setCursor(Qt::PointingHandCursor);
    btnSyncAcc->setStyleSheet("background: #F3F4F6; color: #1F2937; font-weight: 600; padding: 8px 14px; border-radius: 5px; border: 1px solid #D1D5DB;");
    connect(btnSyncAcc, &QPushButton::clicked, this, [this]() {
        if (m_selectedAccountIdx >= 0 && m_selectedAccountIdx < m_accounts->size()) {
            const AccountItem &acc = m_accounts->at(m_selectedAccountIdx);
            emit requestImapSync(acc.id);
        }
    });

    QPushButton *btnTestSrv = new QPushButton(
        MailI18n::t("🔌 서버 접속 시험", "🔌 Test Server Connection", "🔌 サーバー接続テスト"),
        tabServer);
    btnTestSrv->setCursor(Qt::PointingHandCursor);
    btnTestSrv->setStyleSheet("QPushButton { background: #059669; color: #FFFFFF; font-weight: 700; padding: 8px 16px; border-radius: 5px; border: none; }"
                              "QPushButton:hover { background: #047857; }");
    connect(btnTestSrv, &QPushButton::clicked, this, &SettingsDialog::onTestInlineServerConnection);

    srvBtnRow->addWidget(btnSaveSrv);
    srvBtnRow->addWidget(btnTestSrv);
    srvBtnRow->addWidget(btnFullWizard);
    srvBtnRow->addWidget(btnSyncAcc);
    srvBtnRow->addStretch();
    srvV->addLayout(srvBtnRow);

    m_accInlineTestResultLbl = new QLabel(tabServer);
    m_accInlineTestResultLbl->setWordWrap(true);
    m_accInlineTestResultLbl->setTextFormat(Qt::RichText);
    m_accInlineTestResultLbl->setVisible(false);
    m_accInlineTestResultLbl->setStyleSheet(
        "background: #F8FAFC; border: 1px solid #CBD5E1; border-radius: 8px; padding: 10px 14px; font-size: 12px; color: #1E293B;");
    srvV->addWidget(m_accInlineTestResultLbl);
    srvV->addStretch();

    m_accountSubTabStack->addWidget(tabServer);

    // -------------------------------------------------------------------------
    // SUB-TAB 3: Apps & services (Google / iCloud vs General IMAP Account)
    // -------------------------------------------------------------------------
    QWidget *tabApps = new QWidget(m_accountSubTabStack);
    QVBoxLayout *appV = new QVBoxLayout(tabApps);
    appV->setContentsMargins(0, 8, 0, 0);
    appV->setSpacing(14);

    QLabel *appTitle = new QLabel(
        MailI18n::t("Apps & services (연동 앱 및 클라우드 서비스)", "Connected Apps & Services", "連携アプリ・サービス"),
        tabApps);
    appTitle->setStyleSheet("font-size: 16px; font-weight: 600; color: #374151;");
    appV->addWidget(appTitle);

    auto makeServiceCard = [this](QWidget *parentWidget, const QString &name, const QString &desc, const QString &btnText, std::function<void()> onRun) {
        QWidget *card = new QWidget(parentWidget);
        card->setMaximumWidth(540);
        card->setStyleSheet("background: #F9FAFB; border: 1px solid #E5E7EB; border-radius: 8px;");
        QHBoxLayout *h = new QHBoxLayout(card);
        h->setContentsMargins(14, 12, 14, 12);
        QVBoxLayout *tv = new QVBoxLayout();
        QLabel *t = new QLabel(name, card);
        t->setStyleSheet("font-size: 13px; font-weight: 700; color: #111827; border: none;");
        QLabel *d = new QLabel(desc, card);
        d->setWordWrap(true);
        d->setStyleSheet("font-size: 11.5px; color: #6B7280; border: none;");
        tv->addWidget(t);
        tv->addWidget(d);
        QPushButton *b = new QPushButton(btnText, card);
        b->setCursor(Qt::PointingHandCursor);
        b->setStyleSheet("background: #FFFFFF; color: #0D47A1; font-size: 12px; font-weight: 700; border: 1px solid #BFDBFE; border-radius: 5px; padding: 6px 12px;");
        connect(b, &QPushButton::clicked, this, onRun);
        h->addLayout(tv, 1);
        h->addWidget(b);
        return card;
    };

    // 1. Google (Gmail) Account Connected Apps Container
    m_appsGoogleContainer = new QWidget(tabApps);
    QVBoxLayout *googleAppV = new QVBoxLayout(m_appsGoogleContainer);
    googleAppV->setContentsMargins(0, 0, 0, 0);
    googleAppV->setSpacing(12);

    googleAppV->addWidget(makeServiceCard(
        m_appsGoogleContainer,
        "📅 Google Calendar Sync",
        MailI18n::t("이 Google 계정의 캘린더 일정 및 하위 캘린더 목록을 실시간 동기화합니다.",
                    "Synchronize all Google Calendar events and sub-calendars for this account.",
                    "このGoogleアカウントのカレンダー予定を同期します。"),
        MailI18n::t("지금 동기화", "Sync Calendar", "今すぐ同期"),
        [this]() {
            if (m_selectedAccountIdx >= 0 && m_selectedAccountIdx < m_accounts->size() && m_googleProvider) {
                const AccountItem &acc = m_accounts->at(m_selectedAccountIdx);
                if (!acc.oauthAccessToken.isEmpty() || !acc.oauthRefreshToken.isEmpty())
                    m_googleProvider->syncGoogleCalendarEvents(acc.email, acc.oauthAccessToken, acc.oauthRefreshToken);
            }
        }));

    googleAppV->addWidget(makeServiceCard(
        m_appsGoogleContainer,
        "👤 Google Contacts & Auto-Harvest",
        MailI18n::t("Google 주소록(내 연락처 + 자주 연락하는 연락처)을 동기화합니다.",
                    "Synchronize Google Contacts and frequently contacted addresses.",
                    "Google連絡先を同期します。"),
        MailI18n::t("연락처 동기화", "Sync Contacts", "連絡先同期"),
        [this]() {
            if (m_selectedAccountIdx >= 0 && m_selectedAccountIdx < m_accounts->size() && m_googleProvider) {
                const AccountItem &acc = m_accounts->at(m_selectedAccountIdx);
                if (!acc.oauthAccessToken.isEmpty() || !acc.oauthRefreshToken.isEmpty())
                    m_googleProvider->syncGoogleContacts(acc.email, acc.oauthAccessToken, acc.oauthRefreshToken);
            }
        }));

    googleAppV->addWidget(makeServiceCard(
        m_appsGoogleContainer,
        "🌐 Google Profile Photo",
        MailI18n::t("Google 계정에 등록된 프로필 사진을 아바타로 다시 가져옵니다.",
                    "Fetch Google account profile photo automatically.",
                    "Googleプロフィール写真を取得します。"),
        MailI18n::t("프로필 새로고침", "Refresh Photo", "写真更新"),
        [this]() {
            if (m_selectedAccountIdx >= 0 && m_selectedAccountIdx < m_accounts->size() && m_googleProvider) {
                (*m_accounts)[m_selectedAccountIdx].avatarIconPreset = 0;
                m_googleProvider->fetchAccountAvatar(m_accounts->at(m_selectedAccountIdx));
            }
        }));
    appV->addWidget(m_appsGoogleContainer);

    // 2. Apple iCloud Account Connected Apps Container
    m_appsIcloudContainer = new QWidget(tabApps);
    QVBoxLayout *icloudAppV = new QVBoxLayout(m_appsIcloudContainer);
    icloudAppV->setContentsMargins(0, 0, 0, 0);
    icloudAppV->setSpacing(12);

    icloudAppV->addWidget(makeServiceCard(
        m_appsIcloudContainer,
        "☁️ Apple iCloud Calendar & Contacts",
        MailI18n::t("Apple iCloud 계정의 메일·캘린더·연락처 동기화를 수행합니다.",
                    "Synchronize Apple iCloud Mail, Calendar, and Contacts.",
                    "Apple iCloudのメール・カレンダー・連絡先を同期します。"),
        MailI18n::t("지금 동기화", "Sync iCloud", "今すぐ同期"),
        [this]() {
            if (m_selectedAccountIdx >= 0 && m_selectedAccountIdx < m_accounts->size()) {
                emit requestImapSync(m_accounts->at(m_selectedAccountIdx).id);
            }
        }));
    appV->addWidget(m_appsIcloudContainer);

    // 3. General / Company IMAP Email Account Container ("연동된 앱이 없습니다")
    m_appsNoneContainer = new QWidget(tabApps);
    m_appsNoneContainer->setMaximumWidth(540);
    m_appsNoneContainer->setStyleSheet("background: #F9FAFB; border: 1px dashed #D1D5DB; border-radius: 10px;");
    QVBoxLayout *noneV = new QVBoxLayout(m_appsNoneContainer);
    noneV->setContentsMargins(24, 28, 24, 28);
    noneV->setSpacing(8);

    QLabel *noneTitleLbl = new QLabel(
        MailI18n::t("🔌 연동된 앱이 없습니다 (No connected apps)",
                    "🔌 No connected apps or services",
                    "🔌 連携されているアプリはありません"),
        m_appsNoneContainer);
    noneTitleLbl->setAlignment(Qt::AlignCenter);
    noneTitleLbl->setStyleSheet("font-size: 14px; font-weight: 700; color: #374151; border: none;");

    m_appsNoneDescLbl = new QLabel(
        MailI18n::t("일반 IMAP/SMTP 이메일 계정은 클라우드 캘린더·연락처 연동 앱(Google / iCloud)이 지원되지 않으며, 메일 수·발신 및 로컬 캘린더·주소록 기능으로 동작합니다.",
                    "Standard IMAP/SMTP email accounts do not have external cloud calendar/contact app integrations (available for Google and iCloud accounts).",
                    "一般のIMAP/SMTPメールアカウントには外部クラウド連携アプリ（Google / iCloud）はありません。"),
        m_appsNoneContainer);
    m_appsNoneDescLbl->setAlignment(Qt::AlignCenter);
    m_appsNoneDescLbl->setWordWrap(true);
    m_appsNoneDescLbl->setStyleSheet("font-size: 12px; color: #6B7280; border: none; line-height: 1.5;");

    noneV->addWidget(noneTitleLbl);
    noneV->addWidget(m_appsNoneDescLbl);
    appV->addWidget(m_appsNoneContainer);

    appV->addStretch();
    m_accountSubTabStack->addWidget(tabApps);

    rightV->addWidget(m_accountSubTabStack, 1);
    masterDetail->addWidget(rightCol, 1);

    updateSubTabStyles(0);

    connect(m_accountsListWidget, &QListWidget::currentRowChanged, this, [this](int row) {
        if (row < 0 || !m_accountsListWidget->item(row))
            return;
        int accIdx = m_accountsListWidget->item(row)->data(Qt::UserRole).toInt();
        loadSelectedAccountToRightPanel(accIdx);
    });

    refreshAccountsList();

    scroll->setWidget(page);
    return scroll;
}

void SettingsDialog::refreshAccountsList()
{
    if (!m_accountsListWidget || !m_accounts)
        return;

    int prevRow = m_accountsListWidget->currentRow();
    m_accountsListWidget->blockSignals(true);
    m_accountsListWidget->clear();

    int visibleCount = 0;
    for (int i = 0; i < m_accounts->size(); ++i) {
        AccountItem &acc = (*m_accounts)[i];
        if (acc.provider == AccountProviderType::Unified)
            continue;

        if (acc.provider == AccountProviderType::Google || acc.email.endsWith("@gmail.com", Qt::CaseInsensitive)) {
            if (acc.imapHost.trimmed().isEmpty())
                acc.imapHost = QStringLiteral("imap.gmail.com");
            if (acc.smtpHost.trimmed().isEmpty())
                acc.smtpHost = QStringLiteral("smtp.gmail.com");
            if (acc.displayName.trimmed().isEmpty() && !acc.email.trimmed().isEmpty())
                acc.displayName = acc.email.section('@', 0, 0);
        }

        QListWidgetItem *item = new QListWidgetItem(m_accountsListWidget);
        item->setData(Qt::UserRole, i);
        item->setSizeHint(QSize(268, 56));

        // Account Card Widget (Avatar 36x36 + Bold Name / Muted Email + '⋮' Menu Button)
        QWidget *card = new QWidget(m_accountsListWidget);
        card->setStyleSheet("background: transparent;");
        QHBoxLayout *ch = new QHBoxLayout(card);
        ch->setContentsMargins(10, 7, 6, 7);
        ch->setSpacing(10);

        QLabel *avLbl = new QLabel(card);
        avLbl->setFixedSize(36, 36);
        avLbl->setPixmap(MailIcons::accountAvatarPixmap(acc, 36, false));
        ch->addWidget(avLbl);

        QVBoxLayout *tv = new QVBoxLayout();
        tv->setContentsMargins(0, 0, 0, 0);
        tv->setSpacing(1);
        QLabel *nameLbl = new QLabel(acc.displayName.isEmpty() ? acc.email.section('@', 0, 0) : acc.displayName, card);
        nameLbl->setStyleSheet("font-size: 13px; font-weight: 700; color: #111827; background: transparent;");
        QLabel *mailLbl = new QLabel(acc.email, card);
        mailLbl->setStyleSheet("font-size: 11.5px; color: #6B7280; background: transparent;");
        tv->addWidget(nameLbl);
        tv->addWidget(mailLbl);
        ch->addLayout(tv, 1);

        QToolButton *moreBtn = new QToolButton(card);
        moreBtn->setFixedSize(26, 26);
        moreBtn->setCursor(Qt::PointingHandCursor);
        moreBtn->setIcon(MailIcons::actionIcon("more", QColor("#6B7280"), 16));
        moreBtn->setStyleSheet(
            "QToolButton { border: none; background: transparent; border-radius: 4px; }"
            "QToolButton:hover { background: #E5E7EB; }");
        const int capturedRow = visibleCount;
        const QString capturedAccId = acc.id;
        connect(moreBtn, &QToolButton::clicked, this, [this, moreBtn, capturedRow, capturedAccId]() {
            m_accountsListWidget->setCurrentRow(capturedRow);
            QMenu menu(this);
            menu.setStyleSheet(
                "QMenu { background: #FFFFFF; border: 1px solid #D1D5DB; border-radius: 6px; padding: 4px; }"
                "QMenu::item { padding: 7px 14px; font-size: 12px; color: #111827; border-radius: 4px; }"
                "QMenu::item:selected { background: #EFF6FF; color: #0D47A1; }");
            QAction *actSync = menu.addAction(MailI18n::t("🔄 지금 동기화 (Sync now)", "🔄 Sync now", "🔄 今すぐ同期"));
            QAction *actEditWizard = menu.addAction(MailI18n::t("⚙️ 서버 / Mailtrap 설정 마법사 (Edit server)", "⚙️ Server & Mailtrap wizard", "⚙️ サーバー設定ウィザード"));
            menu.addSeparator();
            QAction *actRemove = menu.addAction(MailI18n::t("🗑️ 계정 삭제 (Remove account)", "🗑️ Remove account", "🗑️ アカウント削除"));
            QAction *picked = menu.exec(moreBtn->mapToGlobal(QPoint(0, moreBtn->height())));
            if (picked == actSync && !capturedAccId.isEmpty()) {
                emit requestImapSync(capturedAccId);
            } else if (picked == actEditWizard) {
                onEditSelectedAccount();
            } else if (picked == actRemove) {
                onDeleteSelectedAccount();
            }
        });
        ch->addWidget(moreBtn);

        m_accountsListWidget->setItemWidget(item, card);
        ++visibleCount;
    }

    m_accountsListWidget->setFixedHeight(qMax(64, visibleCount * 60 + 6));
    m_accountsListWidget->blockSignals(false);

    if (m_accountsListWidget->count() > 0) {
        int targetRow = qBound(0, prevRow, m_accountsListWidget->count() - 1);
        m_accountsListWidget->setCurrentRow(targetRow);
        if (m_accountsListWidget->item(targetRow)) {
            loadSelectedAccountToRightPanel(m_accountsListWidget->item(targetRow)->data(Qt::UserRole).toInt());
        }
    }
}

void SettingsDialog::loadSelectedAccountToRightPanel(int accountIndex)
{
    if (!m_accounts || accountIndex < 0 || accountIndex >= m_accounts->size())
        return;

    m_selectedAccountIdx = accountIndex;
    const AccountItem &acc = m_accounts->at(accountIndex);

    // Update 33 avatar preset grid icons
    for (int i = 0; i < m_avatarGridBtns.size(); ++i) {
        bool selected = (acc.avatarIconPreset == i);
        m_avatarGridBtns[i]->setIcon(QIcon(MailIcons::avatarPresetPixmap(i, acc, 38, selected)));
    }

    if (m_accountColorBtn) {
        QColor c = acc.avatarColor.isValid() ? acc.avatarColor : QColor("#0284C7");
        m_accountColorBtn->setStyleSheet(QString(
            "QPushButton { background: #FFFFFF; border: 2px solid %1; border-radius: 5px; font-size: 12px; color: #111827; font-weight: 700; }"
            "QPushButton:hover { background: #F9FAFB; }").arg(c.name()));
    }

    if (m_accYourNameEdit)
        m_accYourNameEdit->setText(acc.displayName);
    if (m_accEmailReadEdit)
        m_accEmailReadEdit->setText(acc.email);
    if (m_accLabelEdit)
        m_accLabelEdit->setText(acc.accountLabel);
    if (m_accUnifiedCheck)
        m_accUnifiedCheck->setChecked(acc.includeInUnified);
    if (m_accReplyToCheck)
        m_accReplyToCheck->setChecked(acc.useDifferentReplyTo);
    if (m_accReplyToEdit) {
        m_accReplyToEdit->setText(acc.replyToAddress);
        m_accReplyToEdit->setVisible(acc.useDifferentReplyTo);
    }

    if (m_accAliasesList) {
        m_accAliasesList->clear();
        m_accAliasesList->addItems(acc.aliases);
    }

    if (m_accInlineImapHost)
        m_accInlineImapHost->setText(acc.imapHost);
    if (m_accInlineImapPort)
        m_accInlineImapPort->setValue(acc.imapPort);
    if (m_accInlineImapSec)
        m_accInlineImapSec->setCurrentText(acc.imapSecurity);
    if (m_accInlineSmtpPreset) {
        int idx = m_accInlineSmtpPreset->findData(acc.smtpPreset);
        if (idx >= 0)
            m_accInlineSmtpPreset->setCurrentIndex(idx);
    }
    if (m_accInlineSmtpHost)
        m_accInlineSmtpHost->setText(acc.smtpHost);
    if (m_accInlineSmtpPort)
        m_accInlineSmtpPort->setValue(acc.smtpPort);
    if (m_accInlineSmtpSec)
        m_accInlineSmtpSec->setCurrentText(acc.smtpSecurity);
    if (m_accInlineSmtpUser)
        m_accInlineSmtpUser->setText(acc.effectiveSmtpUser());
    if (m_accInlineSmtpPass)
        m_accInlineSmtpPass->setText(acc.effectiveSmtpPassword());
    if (m_accInlineTestResultLbl) {
        m_accInlineTestResultLbl->clear();
        m_accInlineTestResultLbl->setVisible(false);
    }

    // SUB-TAB 3 (Apps & services): Gmail / iCloud 계정일 때만 연동 앱을 표시하고 일반 메일은 "연동된 앱이 없습니다" 표시
    const bool isGoogleAcc = (acc.provider == AccountProviderType::Google
                              || acc.email.endsWith("@gmail.com", Qt::CaseInsensitive)
                              || acc.imapHost.contains("gmail.com", Qt::CaseInsensitive));
    const bool isIcloudAcc = (!isGoogleAcc &&
                              (acc.email.endsWith("@icloud.com", Qt::CaseInsensitive)
                               || acc.email.endsWith("@me.com", Qt::CaseInsensitive)
                               || acc.email.endsWith("@mac.com", Qt::CaseInsensitive)
                               || acc.imapHost.contains("icloud.com", Qt::CaseInsensitive)));

    if (m_appsGoogleContainer)
        m_appsGoogleContainer->setVisible(isGoogleAcc);
    if (m_appsIcloudContainer)
        m_appsIcloudContainer->setVisible(isIcloudAcc);
    if (m_appsNoneContainer) {
        m_appsNoneContainer->setVisible(!isGoogleAcc && !isIcloudAcc);
        if (m_appsNoneDescLbl && !isGoogleAcc && !isIcloudAcc) {
            m_appsNoneDescLbl->setText(
                MailI18n::t(QString("선택한 일반 이메일 계정(%1)은 클라우드 캘린더·연락처 연동 앱(Google / iCloud)이 없으며,\n표준 IMAP/SMTP 메일 수·발신 및 로컬 캘린더·주소록 기능으로 동작합니다.").arg(acc.email),
                            QString("Standard IMAP/SMTP account (%1) has no external cloud calendar/contact app integrations (supported on Google and iCloud accounts).").arg(acc.email),
                            QString("選択した一般メールアカウント（%1）には連携アプリ（Google / iCloud）はありません。").arg(acc.email)));
        }
    }

    if (m_accountDetailSummary) {
        QString authType = !acc.oauthAccessToken.isEmpty()
                               ? MailI18n::t("Google OAuth 2.0 인증됨", "Google OAuth 2.0 Verified", "Google OAuth 2.0 認証済み")
                               : (!acc.appPassword.isEmpty()
                                      ? MailI18n::t("비밀번호/앱 암호 설정됨", "Password / App Password Set", "パスワード設定済み")
                                      : MailI18n::t("인증 미설정", "Not Authenticated", "未認証"));
        m_accountDetailSummary->setText(QString(
            "<b>%1</b> &lt;%2&gt; — %3<br/>"
            "📥 <b>IMAP:</b> %4:%5 (%6) &nbsp;|&nbsp; 📤 <b>SMTP:</b> %7:%8 (%9, ID: <b>%10</b>)")
            .arg(acc.displayName.toHtmlEscaped(),
                 acc.email.toHtmlEscaped(),
                 authType,
                 acc.imapHost.toHtmlEscaped(),
                 QString::number(acc.imapPort),
                 acc.imapSecurity,
                 acc.smtpHost.toHtmlEscaped(),
                 QString::number(acc.smtpPort),
                 acc.smtpSecurity,
                 acc.effectiveSmtpUser().toHtmlEscaped()));
    }
}

void SettingsDialog::saveRightPanelToSelectedAccount()
{
    if (!m_accounts || m_selectedAccountIdx < 0 || m_selectedAccountIdx >= m_accounts->size())
        return;

    AccountItem &acc = (*m_accounts)[m_selectedAccountIdx];
    if (m_accYourNameEdit && !m_accYourNameEdit->text().trimmed().isEmpty()) {
        acc.displayName = m_accYourNameEdit->text().trimmed();
        acc.initials = acc.displayName.left(1).toUpper();
    }
    if (m_accLabelEdit)
        acc.accountLabel = m_accLabelEdit->text().trimmed();
    if (m_accUnifiedCheck)
        acc.includeInUnified = m_accUnifiedCheck->isChecked();
    if (m_accReplyToCheck)
        acc.useDifferentReplyTo = m_accReplyToCheck->isChecked();
    if (m_accReplyToEdit)
        acc.replyToAddress = m_accReplyToEdit->text().trimmed();

    if (m_accInlineImapHost && !m_accInlineImapHost->text().trimmed().isEmpty())
        acc.imapHost = m_accInlineImapHost->text().trimmed();
    if (m_accInlineImapPort)
        acc.imapPort = m_accInlineImapPort->value();
    if (m_accInlineImapSec)
        acc.imapSecurity = m_accInlineImapSec->currentText();
    if (m_accInlineSmtpPreset)
        acc.smtpPreset = m_accInlineSmtpPreset->currentData().toString();
    if (m_accInlineSmtpHost && !m_accInlineSmtpHost->text().trimmed().isEmpty())
        acc.smtpHost = m_accInlineSmtpHost->text().trimmed();
    if (m_accInlineSmtpPort)
        acc.smtpPort = m_accInlineSmtpPort->value();
    if (m_accInlineSmtpSec)
        acc.smtpSecurity = m_accInlineSmtpSec->currentText();
    if (m_accInlineSmtpUser && !m_accInlineSmtpUser->text().trimmed().isEmpty()) {
        const QString enteredUser = m_accInlineSmtpUser->text().trimmed();
        const bool isSamePreset = (acc.smtpPreset.isEmpty() || acc.smtpPreset == "SameAsIncoming");
        if (isSamePreset && (enteredUser.compare(acc.email, Qt::CaseInsensitive) == 0
                             || enteredUser.compare(acc.effectiveImapUser(), Qt::CaseInsensitive) == 0)) {
            acc.useSeparateSmtpAuth = false;
            acc.smtpUsername = enteredUser;
        } else if (enteredUser != acc.effectiveImapUser()) {
            acc.useSeparateSmtpAuth = true;
            acc.smtpUsername = enteredUser;
        }
    }
    if (m_accInlineSmtpPass && !m_accInlineSmtpPass->text().isEmpty()) {
        const QString enteredPass = m_accInlineSmtpPass->text();
        if (acc.useSeparateSmtpAuth && acc.smtpPreset != "SameAsIncoming") {
            acc.smtpPassword = enteredPass;
        } else {
            acc.appPassword = enteredPass;
            acc.smtpPassword = enteredPass;
        }
    }

    if (acc.isPop3() && m_folders) {
        for (int i = m_folders->size() - 1; i >= 0; --i) {
            const FolderItem &f = m_folders->at(i);
            if (!f.isCustom)
                continue;
            const bool sameAcc = (f.accountId == acc.id)
                                 || (!f.accountEmail.isEmpty() && f.accountEmail.compare(acc.email, Qt::CaseInsensitive) == 0);
            if (sameAcc && !f.rawImapMailbox.isEmpty()) {
                m_folders->removeAt(i);
            }
        }
        emit foldersUpdated();
    }

    refreshAccountsList();
    loadSelectedAccountToRightPanel(m_selectedAccountIdx);
    emit accountsUpdated();
}

void SettingsDialog::onTestInlineServerConnection()
{
    if (!m_accounts || m_selectedAccountIdx < 0 || m_selectedAccountIdx >= m_accounts->size())
        return;

    AccountItem testAcc = m_accounts->at(m_selectedAccountIdx);
    if (m_accInlineImapHost && !m_accInlineImapHost->text().trimmed().isEmpty())
        testAcc.imapHost = m_accInlineImapHost->text().trimmed();
    if (m_accInlineImapPort)
        testAcc.imapPort = m_accInlineImapPort->value();
    if (m_accInlineImapSec)
        testAcc.imapSecurity = m_accInlineImapSec->currentText();
    if (m_accInlineSmtpPreset)
        testAcc.smtpPreset = m_accInlineSmtpPreset->currentData().toString();
    if (m_accInlineSmtpHost && !m_accInlineSmtpHost->text().trimmed().isEmpty())
        testAcc.smtpHost = m_accInlineSmtpHost->text().trimmed();
    if (m_accInlineSmtpPort)
        testAcc.smtpPort = m_accInlineSmtpPort->value();
    if (m_accInlineSmtpSec)
        testAcc.smtpSecurity = m_accInlineSmtpSec->currentText();

    if (m_accInlineSmtpUser && !m_accInlineSmtpUser->text().trimmed().isEmpty()) {
        const QString enteredUser = m_accInlineSmtpUser->text().trimmed();
        const bool isSamePreset = (testAcc.smtpPreset.isEmpty() || testAcc.smtpPreset == "SameAsIncoming");
        if (isSamePreset) {
            testAcc.useSeparateSmtpAuth = false;
            testAcc.imapUsername = enteredUser;
            testAcc.smtpUsername = enteredUser;
        } else if (enteredUser != testAcc.effectiveImapUser()) {
            testAcc.useSeparateSmtpAuth = true;
            testAcc.smtpUsername = enteredUser;
        }
    }
    if (m_accInlineSmtpPass && !m_accInlineSmtpPass->text().isEmpty()) {
        const QString enteredPass = m_accInlineSmtpPass->text();
        if (testAcc.useSeparateSmtpAuth && testAcc.smtpPreset != "SameAsIncoming") {
            testAcc.smtpPassword = enteredPass;
        } else {
            testAcc.appPassword = enteredPass;
            testAcc.smtpPassword = enteredPass;
        }
    }

    QPointer<SettingsDialog> self(this);
    QPointer<QLabel> safeResultLbl(m_accInlineTestResultLbl);
    if (safeResultLbl) {
        safeResultLbl->setVisible(true);
        safeResultLbl->setText(
            MailI18n::t("⏳ <b>서버 접속 및 로그인 인증을 시험하는 중입니다...</b> (잠시만 기다려 주세요)",
                        "⏳ <b>Testing server connection and authentication...</b>",
                        "⏳ <b>サーバー接続と認証をテストしています...</b>"));
        QCoreApplication::processEvents();
    }
    if (!self)
        return;

    QString htmlSummary;
    runMailServerConnectionTest(testAcc, this, &htmlSummary);
    if (!self)
        return;

    if (safeResultLbl && !htmlSummary.isEmpty()) {
        safeResultLbl->setText(htmlSummary);
        safeResultLbl->setVisible(true);
    }
}

void SettingsDialog::onAddAccountClicked(const QString &preset)
{
    AddAccountDialog dlg(m_googleProvider, nullptr, preset, this);
    if (dlg.exec() == QDialog::Accepted) {
        AccountItem newAcc = dlg.resultAccount();
        // 중복 이메일이 있으면 업데이트, 없으면 추가
        bool updatedExisting = false;
        for (int i = 0; i < m_accounts->size(); ++i) {
            if ((*m_accounts)[i].email.compare(newAcc.email, Qt::CaseInsensitive) == 0) {
                newAcc.id = (*m_accounts)[i].id;
                (*m_accounts)[i] = newAcc;
                updatedExisting = true;
                break;
            }
        }
        if (!updatedExisting) {
            // 신규 추가인데 이전에 삭제 후 남아 있던 pop3/<이메일> 폴더가 디스크에 존재하면 remove_ 접두어로 백업 보관 후 새 폴더 생성
            if (newAcc.isPop3() && newAcc.hasPop3AccountDir()) {
                newAcc.archivePop3AccountDirOnRemove();
            }
            if (newAcc.isPop3()) {
                newAcc.ensurePop3MailboxDirs();
            }
            m_accounts->append(newAcc);
        }
        if (newAcc.isPop3() && m_folders) {
            // POP3 계정인 경우 이전에 IMAP으로 등록했을 때 남아 있던 IMAP 전용 서버 폴더(!rawImapMailbox.isEmpty())를 즉시 제거
            for (int i = m_folders->size() - 1; i >= 0; --i) {
                const FolderItem &f = m_folders->at(i);
                if (!f.isCustom)
                    continue;
                const bool sameAcc = (f.accountId == newAcc.id)
                                     || (!f.accountEmail.isEmpty() && f.accountEmail.compare(newAcc.email, Qt::CaseInsensitive) == 0);
                if (sameAcc && !f.rawImapMailbox.isEmpty()) {
                    m_folders->removeAt(i);
                }
            }
            emit foldersUpdated();
        }
        refreshAccountsList();
        emit accountsUpdated();
        emit requestImapSync(newAcc.id);
    }
}

void SettingsDialog::onEditSelectedAccount()
{
    if (!m_accountsListWidget)
        return;
    QListWidgetItem *it = m_accountsListWidget->currentItem();
    if (!it) {
        QMessageBox::information(
            this,
            MailI18n::t("계정 선택", "Select Account", "アカウント選択"),
            MailI18n::t("수정할 계정을 목록에서 먼저 선택해 주세요.", "Please select an account from the list first.", "編集するアカウントを一覧から選択してください。"));
        return;
    }
    int accIdx = it->data(Qt::UserRole).toInt();
    if (accIdx < 0 || accIdx >= m_accounts->size())
        return;

    AccountItem target = m_accounts->at(accIdx);
    const QString targetId = target.id;
    AddAccountDialog dlg(m_googleProvider, &target, QString(), this);
    if (dlg.exec() == QDialog::Accepted) {
        AccountItem updatedAcc = dlg.resultAccount();
        int foundIdx = -1;
        for (int i = 0; i < m_accounts->size(); ++i) {
            if (m_accounts->at(i).id == targetId) {
                foundIdx = i;
                break;
            }
        }
        if (foundIdx < 0)
            return;
        (*m_accounts)[foundIdx] = updatedAcc;
        if (updatedAcc.isPop3() && m_folders) {
            for (int i = m_folders->size() - 1; i >= 0; --i) {
                const FolderItem &f = m_folders->at(i);
                if (!f.isCustom)
                    continue;
                const bool sameAcc = (f.accountId == updatedAcc.id)
                                     || (!f.accountEmail.isEmpty() && f.accountEmail.compare(updatedAcc.email, Qt::CaseInsensitive) == 0);
                if (sameAcc && !f.rawImapMailbox.isEmpty()) {
                    m_folders->removeAt(i);
                }
            }
            emit foldersUpdated();
        }
        refreshAccountsList();
        emit accountsUpdated();
    }
}

void SettingsDialog::onDeleteSelectedAccount()
{
    if (!m_accountsListWidget)
        return;
    QListWidgetItem *it = m_accountsListWidget->currentItem();
    if (!it) {
        QMessageBox::information(
            this,
            MailI18n::t("계정 선택", "Select Account", "アカウント選択"),
            MailI18n::t("삭제할 계정을 목록에서 선택해 주세요.", "Please select an account to delete.", "削除するアカウントを一覧から選択してください。"));
        return;
    }
    int accIdx = it->data(Qt::UserRole).toInt();
    if (accIdx < 0 || accIdx >= m_accounts->size())
        return;

    const AccountItem acc = m_accounts->at(accIdx);
    if (acc.provider == AccountProviderType::Unified) {
        QMessageBox::warning(
            this,
            MailI18n::t("삭제 불가", "Cannot Delete", "削除不可"),
            MailI18n::t("통합 메일함(Unified Inbox)은 기본 시스템 항목이므로 삭제할 수 없습니다.",
                        "Unified Inbox is a system item and cannot be deleted.",
                        "統合受信トレイ(Unified Inbox)はシステム項目のため削除できません。"));
        return;
    }

    const bool hasLocalPop3Dir = acc.isPop3() || acc.hasPop3AccountDir();
    if (hasLocalPop3Dir) {
        QMessageBox msgBox(this);
        msgBox.setWindowTitle(MailI18n::t("POP3 계정 삭제 및 로컬 메일함 처리 확인",
                                          "Confirm POP3 Account Deletion",
                                          "POP3アカウント削除の確認"));
        msgBox.setIcon(QMessageBox::Question);
        msgBox.setText(MailI18n::t(
            QString("'%1 (%2)' 계정을 SollunaMail에서 삭제합니다.\n\n"
                    "PC에 저장된 로컬 POP3 편지함 폴더(pop3/%3)를 어떻게 처리하시겠습니까?\n"
                    "(서버의 원본 이메일은 삭제되지 않습니다.)")
                .arg(acc.displayName, acc.email, acc.pop3AccountFolderName()),
            QString("Removing account '%1 (%2)'.\n\n"
                    "How would you like to handle the local POP3 mailbox folder (pop3/%3) saved on this PC?")
                .arg(acc.displayName, acc.email, acc.pop3AccountFolderName()),
            QString("'%1 (%2)' アカウントを削除します。\n\n"
                    "PCに保存されているローカルPOP3フォルダ(pop3/%3)をどう処理しますか？")
                .arg(acc.displayName, acc.email, acc.pop3AccountFolderName())));

        QPushButton *btnRenameKeep = msgBox.addButton(
            MailI18n::t(QString("📦 remove_%1 로 이름 변경 보관 (권장)").arg(acc.pop3AccountFolderName()),
                        QString("📦 Rename to remove_%1 & Keep").arg(acc.pop3AccountFolderName()),
                        QString("📦 remove_%1 に名前変更して保管").arg(acc.pop3AccountFolderName())),
            QMessageBox::AcceptRole);
        QPushButton *btnDeleteLocal = msgBox.addButton(
            MailI18n::t("🗑️ 로컬에 저장된 내용 함께 삭제",
                        "🗑️ Delete Local Saved Mail",
                        "🗑️ ローカル保存データも削除"),
            QMessageBox::DestructiveRole);
        QPushButton *btnCancel = msgBox.addButton(
            MailI18n::t("취소 (Cancel)", "Cancel", "キャンセル"),
            QMessageBox::RejectRole);
        msgBox.setDefaultButton(btnRenameKeep);
        msgBox.exec();

        QAbstractButton *clicked = msgBox.clickedButton();
        if (!clicked || clicked == btnCancel) {
            return;
        }
        if (clicked == btnRenameKeep) {
            acc.archivePop3AccountDirOnRemove();
        } else if (clicked == btnDeleteLocal) {
            acc.deletePop3AccountDir();
        }
    } else {
        QMessageBox::StandardButton ans = QMessageBox::question(
            this,
            MailI18n::t("계정 삭제 확인", "Confirm Account Deletion", "アカウント削除の確認"),
            MailI18n::t(QString("정말로 '%1 (%2)' 계정을 SollunaMail에서 삭제하시겠습니까?\n(서버의 원본 이메일은 삭제되지 않습니다.)").arg(acc.displayName, acc.email),
                        QString("Are you sure you want to remove '%1 (%2)' from SollunaMail?\n(Emails on the server will not be deleted.)").arg(acc.displayName, acc.email),
                        QString("本当に '%1 (%2)' アカウントを SollunaMail から削除しますか？\n（サーバー上の元のメールは削除されません）").arg(acc.displayName, acc.email)),
            QMessageBox::Yes | QMessageBox::No,
            QMessageBox::No);
        if (ans != QMessageBox::Yes)
            return;
    }

    // 삭제된 계정의 사용자 폴더(customFolders) 및 메모리 대화 목록도 함께 정리
    if (m_folders) {
        for (int i = m_folders->size() - 1; i >= 0; --i) {
            const FolderItem &f = m_folders->at(i);
            if (!f.isCustom)
                continue;
            const bool matchId = (!f.accountId.isEmpty() && f.accountId == acc.id);
            const bool matchEmail = (!f.accountEmail.isEmpty() && f.accountEmail.compare(acc.email, Qt::CaseInsensitive) == 0);
            if (matchId || matchEmail) {
                m_folders->removeAt(i);
            }
        }
    }
    if (m_googleProvider) {
        m_googleProvider->clearLoadedImapUids(acc.id);
    }

    int foundAccIdx = -1;
    for (int i = 0; i < m_accounts->size(); ++i) {
        if (m_accounts->at(i).id == acc.id) {
            foundAccIdx = i;
            break;
        }
    }
    if (foundAccIdx >= 0) {
        m_accounts->removeAt(foundAccIdx);
    }
    refreshAccountsList();
    emit foldersUpdated();
    emit accountsUpdated();
}

QWidget *SettingsDialog::createAppearancePage()
{
    QScrollArea *scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setStyleSheet("background: #FFFFFF;");

    QWidget *page = new QWidget(scroll);
    QVBoxLayout *v = new QVBoxLayout(page);
    v->setContentsMargins(36, 26, 36, 26);
    v->setSpacing(16);

    // 1. Header: Paint roller icon + Appearance
    QHBoxLayout *hdr = new QHBoxLayout();
    QLabel *iconLbl = new QLabel(page);
    iconLbl->setPixmap(MailIcons::settingsNavIcon(2, QColor("#2563EB"), 22).pixmap(22, 22));
    QLabel *title = new QLabel(
        MailI18n::t("테마 및 모양 (Appearance)", "Appearance", "外観・テーマ (Appearance)"),
        page);
    title->setStyleSheet("font-size: 19px; font-weight: 700; color: #1F2937;");
    hdr->addWidget(iconLbl);
    hdr->addSpacing(8);
    hdr->addWidget(title);
    hdr->addStretch();
    v->addLayout(hdr);

    // 1-2. 화면 밝기 모드 (Appearance Mode: 라이트 모드 / 다크 모드 — 윈도우 다크 테마 간섭 방지)
    QLabel *modeHdr = new QLabel(
        MailI18n::t("화면 밝기 모드 (Color Mode — 윈도우 다크 테마 PC에서도 글자가 선명하게 보이도록 제어):",
                    "Color Mode (Light / Dark workspace):",
                    "画面カラーモード (ライト / ダーク):"),
        page);
    modeHdr->setStyleSheet("font-size: 13px; font-weight: 700; color: #374151; margin-top: 4px;");
    v->addWidget(modeHdr);

    QHBoxLayout *modeRow = new QHBoxLayout();
    modeRow->setSpacing(10);
    QPushButton *btnLightMode = new QPushButton(
        MailI18n::t("☀️ 라이트 모드 (Light — 밝고 선명한 화면 · 권장)",
                    "☀️ Light Mode (Recommended)",
                    "☀️ ライトモード（推奨）"),
        page);
    QPushButton *btnDarkMode = new QPushButton(
        MailI18n::t("🌙 다크 모드 (Dark — 어두운 화면)",
                    "🌙 Dark Mode",
                    "🌙 ダークモード"),
        page);
    btnLightMode->setCursor(Qt::PointingHandCursor);
    btnDarkMode->setCursor(Qt::PointingHandCursor);
    btnLightMode->setFixedHeight(38);
    btnDarkMode->setFixedHeight(38);

    auto refreshModeButtons = [btnLightMode, btnDarkMode]() {
        const bool isDark = (MailI18n::appearanceMode().compare("Dark", Qt::CaseInsensitive) == 0);
        auto styleBtn = [](bool active) {
            return active
                ? QStringLiteral("QPushButton { background: #EFF6FF; color: #0D47A1; font-size: 12.5px; font-weight: 700; border: 2px solid #0D47A1; border-radius: 7px; padding: 6px 18px; }")
                : QStringLiteral("QPushButton { background: #FFFFFF; color: #374151; font-size: 12.5px; font-weight: 600; border: 1px solid #D1D5DB; border-radius: 7px; padding: 6px 18px; }"
                                 "QPushButton:hover { background: #F9FAFB; }");
        };
        btnLightMode->setStyleSheet(styleBtn(!isDark));
        btnDarkMode->setStyleSheet(styleBtn(isDark));
    };
    refreshModeButtons();

    connect(btnLightMode, &QPushButton::clicked, this, [this, refreshModeButtons]() {
        MailI18n::appearanceMode() = QStringLiteral("Light");
        refreshModeButtons();
        emit themeChanged(m_currentTheme);
    });
    connect(btnDarkMode, &QPushButton::clicked, this, [this, refreshModeButtons]() {
        MailI18n::appearanceMode() = QStringLiteral("Dark");
        refreshModeButtons();
        emit themeChanged(m_currentTheme);
    });

    modeRow->addWidget(btnLightMode);
    modeRow->addWidget(btnDarkMode);
    modeRow->addStretch();
    v->addLayout(modeRow);

    // 2. Select theme color (9 Two-Tone Rounded Square Swatches)
    QLabel *swatchPrompt = new QLabel(
        MailI18n::t("적용할 테마 컬러를 선택하세요 (현재 테마: " + m_currentTheme + "):",
                    "Select theme color (Current: " + m_currentTheme + "):",
                    "テーマカラーを選択してください (現在: " + m_currentTheme + "):"),
        page);
    swatchPrompt->setStyleSheet("font-size: 12.5px; font-weight: 600; color: #374151; margin-top: 2px;");
    v->addWidget(swatchPrompt);

    struct SwatchSpec {
        QString themeName;
        QColor topColor;
        QColor bottomColor;
    };
    const QList<SwatchSpec> swatches = {
        {"Obsidian Black",              QColor("#202124"), QColor("#18181B")},
        {"Dove Gray",                   QColor("#F3F4F6"), QColor("#374151")},
        {"Ice Slate (NordicInbox)",     QColor("#DBEAFE"), QColor("#1E293B")},
        {"Mint Forest",                 QColor("#E2F2EA"), QColor("#1D3B36")},
        {"Espresso Sand",               QColor("#FAECD2"), QColor("#3E2112")},
        {"BlueFocus (MailBird Royal)",  QColor("#0F4299"), QColor("#093282")},
        {"Royal Indigo",                QColor("#4C2882"), QColor("#341F65")},
        {"Plum Studio",                 QColor("#6B1D57"), QColor("#3D1038")},
        {"Evergreen Desk",              QColor("#0E5443"), QColor("#063729")}
    };

    QHBoxLayout *swatchRow = new QHBoxLayout();
    swatchRow->setSpacing(10);
    QList<QPushButton *> swatchBtns;

    auto makeSwatchPixmap = [](const QColor &topC, const QColor &botC, bool selected) -> QPixmap {
        const int w = 52;
        const int h = 44;
        QPixmap pix(w * 2, h * 2);
        pix.setDevicePixelRatio(2.0);
        pix.fill(Qt::transparent);

        QPainter p(&pix);
        p.setRenderHint(QPainter::Antialiasing, true);

        QRectF inner(4.0, 4.0, w - 8.0, h - 8.0);
        QPainterPath clip;
        clip.addRoundedRect(inner, 8.0, 8.0);
        p.save();
        p.setClipPath(clip);
        p.fillRect(QRectF(inner.left(), inner.top(), inner.width(), inner.height() * 0.56), topC);
        p.fillRect(QRectF(inner.left(), inner.top() + inner.height() * 0.56, inner.width(), inner.height() * 0.44), botC);
        p.restore();

        // 외곽선 표시 (밝은 컬러칩도 흰 배경 위에서 뚜렷하게 구분되도록 테두리 적용)
        p.setPen(QPen(QColor("#CBD5E1"), 1.0));
        p.setBrush(Qt::NoBrush);
        p.drawRoundedRect(inner, 8.0, 8.0);

        if (selected) {
            p.setPen(QPen(QColor("#0D47A1"), 2.2));
            p.setBrush(Qt::NoBrush);
            p.drawRoundedRect(QRectF(1.5, 1.5, w - 3.0, h - 3.0), 10.0, 10.0);
        }
        return pix;
    };

    for (int i = 0; i < swatches.size(); ++i) {
        QPushButton *sb = new QPushButton(page);
        sb->setFixedSize(52, 44);
        sb->setIconSize(QSize(52, 44));
        sb->setCursor(Qt::PointingHandCursor);
        sb->setToolTip(swatches[i].themeName);
        sb->setStyleSheet("QPushButton { border: none; background: transparent; padding: 0px; }");
        swatchBtns.append(sb);
        swatchRow->addWidget(sb);
    }

    auto refreshSwatchIcons = [this, swatches, swatchBtns, makeSwatchPixmap, swatchPrompt]() {
        for (int i = 0; i < swatches.size() && i < swatchBtns.size(); ++i) {
            bool sel = (m_currentTheme.compare(swatches[i].themeName, Qt::CaseInsensitive) == 0)
                       || (swatches[i].themeName.contains("BlueFocus") && m_currentTheme.contains("BlueFocus"))
                       || (swatches[i].themeName.contains("NordicInbox") && m_currentTheme.contains("NordicInbox"));
            swatchBtns[i]->setIcon(QIcon(makeSwatchPixmap(swatches[i].topColor, swatches[i].bottomColor, sel)));
        }
        if (swatchPrompt) {
            swatchPrompt->setText(
                MailI18n::t("적용할 테마 컬러를 선택하세요 (현재 테마: " + m_currentTheme + "):",
                            "Select theme color (Current: " + m_currentTheme + "):",
                            "テーマカラーを選択してください (現在: " + m_currentTheme + "):"));
        }
    };

    for (int i = 0; i < swatches.size(); ++i) {
        connect(swatchBtns[i], &QPushButton::clicked, this, [this, i, swatches, refreshSwatchIcons]() {
            m_currentTheme = swatches[i].themeName;
            refreshSwatchIcons();
            emit themeChanged(m_currentTheme);
        });
    }
    refreshSwatchIcons();
    swatchRow->addStretch();
    v->addLayout(swatchRow);

    // 3. Layout (4 Wireframe Visual Cards: Comfortable, Horizontal, Fullscreen, Minimal + Menu expanded dropdown)
    QLabel *layoutHdr = new QLabel(
        MailI18n::t("이메일 화면 배치 레이아웃 (Layout)", "Layout", "レイアウト (Layout)"),
        page);
    layoutHdr->setStyleSheet("font-size: 16px; font-weight: 700; color: #374151; margin-top: 12px;");
    v->addWidget(layoutHdr);

    QLabel *layoutSub = new QLabel(
        MailI18n::t("원하는 이메일 목록 및 본문 분할 방식을 선택하세요.",
                    "Choose how you want your emails to be displayed",
                    "メールの表示レイアウトを選択してください"),
        page);
    layoutSub->setStyleSheet("font-size: 12px; color: #6B7280;");
    v->addWidget(layoutSub);

    auto makeLayoutWireframe = [](int kind, bool selected) -> QPixmap {
        const int w = 112;
        const int h = 74;
        QPixmap pix(w * 2, h * 2);
        pix.setDevicePixelRatio(2.0);
        pix.fill(Qt::transparent);

        QPainter p(&pix);
        p.setRenderHint(QPainter::Antialiasing, true);

        QColor accent = selected ? QColor("#0D47A1") : QColor("#CBD5E1");
        QColor fillBar = selected ? QColor("#0D47A1") : QColor("#D1D5DB");
        QColor borderCol = selected ? QColor("#0D47A1") : QColor("#D1D5DB");

        QRectF box(2.0, 2.0, w - 4.0, h - 4.0);
        p.setPen(QPen(borderCol, selected ? 1.6 : 1.0));
        p.setBrush(QColor("#FFFFFF"));
        p.drawRoundedRect(box, 5.0, 5.0);

        if (kind == 0) {
            // Comfortable: Left nav dots + 4 tall list cards + right reading area
            p.drawLine(QPointF(14, 2), QPointF(14, h - 2));
            p.drawLine(QPointF(46, 2), QPointF(46, h - 2));
            p.setPen(Qt::NoPen);
            p.setBrush(accent);
            for (int d = 0; d < 4; ++d)
                p.drawEllipse(QRectF(6.5, 8 + d * 7, 3.5, 3.5));
            p.setBrush(fillBar);
            for (int r = 0; r < 4; ++r)
                p.drawRoundedRect(QRectF(18, 7 + r * 15, 24, 11), 1.5, 1.5);
        } else if (kind == 1) {
            // Horizontal: Left nav dots + top list pane / bottom reading pane
            p.drawLine(QPointF(14, 2), QPointF(14, h - 2));
            p.drawLine(QPointF(14, h * 0.5), QPointF(w - 2, h * 0.5));
            p.setPen(Qt::NoPen);
            p.setBrush(accent);
            for (int d = 0; d < 4; ++d)
                p.drawEllipse(QRectF(6.5, 8 + d * 7, 3.5, 3.5));
        } else if (kind == 2) {
            // Fullscreen: Left nav dots + full width reading/list area
            p.drawLine(QPointF(14, 2), QPointF(14, h - 2));
            p.setPen(Qt::NoPen);
            p.setBrush(accent);
            for (int d = 0; d < 4; ++d)
                p.drawEllipse(QRectF(6.5, 8 + d * 7, 3.5, 3.5));
        } else {
            // Minimal: Full-width horizontal message bars
            p.setPen(Qt::NoPen);
            p.setBrush(fillBar);
            for (int r = 0; r < 4; ++r)
                p.drawRoundedRect(QRectF(9, 9 + r * 15, w - 18, 10), 2.0, 2.0);
        }
        return pix;
    };

    const QStringList layoutNames = {"Comfortable", "Horizontal", "Fullscreen", "Minimal"};
    QHBoxLayout *cardsRow = new QHBoxLayout();
    cardsRow->setSpacing(18);
    QList<QPushButton *> cardBtns;
    QList<QLabel *> cardLbls;

    QComboBox *menuExpandCombo = new QComboBox(page);
    menuExpandCombo->addItem(MailI18n::t("Menu expanded (메뉴 펼침)", "Menu expanded", "メニュー展開"), true);
    menuExpandCombo->addItem(MailI18n::t("Menu collapsed (메뉴 접힘)", "Menu collapsed", "メニュー折りたたみ"), false);
    menuExpandCombo->setCurrentIndex(MailI18n::menuExpanded() ? 0 : 1);
    menuExpandCombo->setFixedWidth(245);
    menuExpandCombo->setStyleSheet(
        "QComboBox { background: #FFFFFF; border: 1px solid #D1D5DB; border-radius: 6px; padding: 8px 14px; font-size: 12.5px; color: #1F2937; }");

    for (int i = 0; i < layoutNames.size(); ++i) {
        QWidget *colW = new QWidget(page);
        QVBoxLayout *cv = new QVBoxLayout(colW);
        cv->setContentsMargins(0, 0, 0, 0);
        cv->setSpacing(6);
        QPushButton *cb = new QPushButton(colW);
        cb->setFixedSize(112, 74);
        cb->setIconSize(QSize(112, 74));
        cb->setCursor(Qt::PointingHandCursor);
        cb->setStyleSheet("QPushButton { border: none; background: transparent; padding: 0px; }");
        QLabel *cl = new QLabel(layoutNames[i], colW);
        cl->setAlignment(Qt::AlignHCenter);
        cv->addWidget(cb, 0, Qt::AlignHCenter);
        cv->addWidget(cl, 0, Qt::AlignHCenter);
        cardBtns.append(cb);
        cardLbls.append(cl);
        cardsRow->addWidget(colW);
    }
    cardsRow->addStretch();
    v->addLayout(cardsRow);

    auto updateLayoutCards = [layoutNames, cardBtns, cardLbls, makeLayoutWireframe]() {
        if (MailI18n::layoutStyleName().compare("Compact", Qt::CaseInsensitive) == 0) {
            MailI18n::layoutStyleName() = QStringLiteral("Comfortable");
        }
        QString cur = MailI18n::layoutStyleName();
        for (int i = 0; i < layoutNames.size() && i < cardBtns.size(); ++i) {
            bool sel = (cur.compare(layoutNames[i], Qt::CaseInsensitive) == 0);
            cardBtns[i]->setIcon(QIcon(makeLayoutWireframe(i, sel)));
            cardLbls[i]->setStyleSheet(sel
                ? "font-size: 12px; font-weight: 700; color: #0D47A1;"
                : "font-size: 12px; font-weight: 500; color: #4B5563;");
        }
    };

    for (int i = 0; i < layoutNames.size(); ++i) {
        connect(cardBtns[i], &QPushButton::clicked, this, [this, i, layoutNames, menuExpandCombo, cardBtns, cardLbls, makeLayoutWireframe]() {
            MailI18n::layoutStyleName() = layoutNames[i];
            for (int j = 0; j < layoutNames.size() && j < cardBtns.size(); ++j) {
                bool sel = (j == i);
                cardBtns[j]->setIcon(QIcon(makeLayoutWireframe(j, sel)));
                cardLbls[j]->setStyleSheet(sel
                    ? "font-size: 12px; font-weight: 700; color: #0D47A1;"
                    : "font-size: 12px; font-weight: 500; color: #4B5563;");
            }
            int splitMode = 0;
            if (layoutNames[i] == "Horizontal")
                splitMode = 1;
            else if (layoutNames[i] == "Fullscreen")
                splitMode = 2;
            bool expanded = (layoutNames[i] != "Minimal");
            MailI18n::menuExpanded() = expanded;
            menuExpandCombo->blockSignals(true);
            menuExpandCombo->setCurrentIndex(expanded ? 0 : 1);
            menuExpandCombo->blockSignals(false);
            emit mailLayoutChanged(splitMode, expanded);
        });
    }
    updateLayoutCards();

    connect(menuExpandCombo, &QComboBox::currentIndexChanged, this, [this, menuExpandCombo]() {
        bool expanded = menuExpandCombo->currentData().toBool();
        MailI18n::menuExpanded() = expanded;
        int splitMode = 0;
        if (MailI18n::layoutStyleName() == "Horizontal")
            splitMode = 1;
        else if (MailI18n::layoutStyleName() == "Fullscreen")
            splitMode = 2;
        emit mailLayoutChanged(splitMode, expanded);
    });
    v->addWidget(menuExpandCombo);

    v->addStretch();
    scroll->setWidget(page);
    return scroll;
}

QWidget *SettingsDialog::createFoldersPage()
{
    QScrollArea *scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setStyleSheet("background: #FFFFFF;");

    QWidget *page = new QWidget(scroll);
    QVBoxLayout *v = new QVBoxLayout(page);
    v->setContentsMargins(36, 26, 36, 26);
    v->setSpacing(14);

    QHBoxLayout *hdrRow = new QHBoxLayout();
    hdrRow->setSpacing(8);
    QLabel *hdrIcon = new QLabel(page);
    hdrIcon->setPixmap(MailIcons::settingsNavIcon(4, QColor("#0D47A1"), 20).pixmap(20, 20));
    QLabel *t = new QLabel(MailI18n::t("폴더 관리 (Folders)", "Folders", "フォルダ管理 (Folders)"), page);
    t->setStyleSheet("font-size: 20px; font-weight: 600; color: #374151;");
    hdrRow->addWidget(hdrIcon);
    hdrRow->addWidget(t);
    hdrRow->addStretch();
    v->addLayout(hdrRow);

    QLabel *s = new QLabel(
        MailI18n::t("기본 편지함(Inbox ~ Trash) 및 사용자 정의 폴더(YOUR FOLDERS)를 추가·이름 변경·색상 변경·삭제합니다.",
                    "Add, rename, change color, or delete custom folders (YOUR FOLDERS).",
                    "基本フォルダおよびカスタムフォルダ(YOUR FOLDERS)の追加・名前変更・色変更・削除を行います。"),
        page);
    s->setWordWrap(true);
    s->setStyleSheet("font-size: 12px; color: #6B7280; margin-bottom: 4px;");
    v->addWidget(s);

    // System folders badge list
    QGroupBox *sysGroup = new QGroupBox(MailI18n::t("시스템 기본 폴더 (System Folders)", "System Folders", "システム標準フォルダ"), page);
    sysGroup->setMaximumWidth(740);
    sysGroup->setStyleSheet("QGroupBox { font-size: 12.5px; font-weight: 700; color: #111827; border: 1px solid #E5E7EB; border-radius: 8px; margin-top: 8px; padding: 12px; background: #F9FAFB; }");
    QHBoxLayout *sysH = new QHBoxLayout(sysGroup);
    sysH->setSpacing(8);
    if (m_folders) {
        for (const FolderItem &f : *m_folders) {
            if (f.isCustom)
                continue;
            QLabel *chip = new QLabel("📁 " + MailI18n::folderTitle(f.id, f.name), sysGroup);
            chip->setStyleSheet("background: #FFFFFF; color: #374151; font-size: 11.5px; font-weight: 600; border: 1px solid #D1D5DB; border-radius: 5px; padding: 4px 8px;");
            sysH->addWidget(chip);
        }
    }
    sysH->addStretch();
    v->addWidget(sysGroup);

    // Custom folders manager (YOUR FOLDERS — per account)
    QGroupBox *custGroup = new QGroupBox(MailI18n::t("계정별 내 폴더 관리 (YOUR FOLDERS — 추가 · 이름/색상 변경 · 삭제)", "YOUR FOLDERS — Per-Account Custom Folders", "アカウント別マイフォルダ管理 (YOUR FOLDERS)"), page);
    custGroup->setMaximumWidth(740);
    custGroup->setStyleSheet("QGroupBox { font-size: 13px; font-weight: 700; color: #111827; border: 1px solid #E5E7EB; border-radius: 8px; margin-top: 8px; padding: 16px; background: #FFFFFF; }");
    QVBoxLayout *cv = new QVBoxLayout(custGroup);
    cv->setSpacing(10);

    QHBoxLayout *accFilterRow = new QHBoxLayout();
    accFilterRow->setSpacing(8);
    QLabel *accFilterLbl = new QLabel(MailI18n::t("대상 계정 선택:", "Target account:", "対象アカウント:"), custGroup);
    accFilterLbl->setStyleSheet("font-size: 12.5px; font-weight: 600; color: #374151;");
    QComboBox *accFolderCombo = new QComboBox(custGroup);
    accFolderCombo->setMinimumWidth(280);
    accFolderCombo->setStyleSheet("QComboBox { border: 1px solid #D1D5DB; border-radius: 5px; padding: 5px 10px; font-size: 12.5px; background: #FFFFFF; color: #1F2937; }");
    if (m_accounts) {
        for (const AccountItem &acc : *m_accounts) {
            if (acc.provider == AccountProviderType::Unified)
                continue;
            accFolderCombo->addItem(QString("%1 (%2)").arg(acc.displayName, acc.email), acc.id);
        }
    }
    accFilterRow->addWidget(accFilterLbl);
    accFilterRow->addWidget(accFolderCombo);
    accFilterRow->addStretch();
    cv->addLayout(accFilterRow);

    QListWidget *folderList = new QListWidget(custGroup);
    folderList->setFixedHeight(190);
    folderList->setStyleSheet(
        "QListWidget { border: 1px solid #D1D5DB; border-radius: 6px; padding: 4px; background: #FFFFFF; }"
        "QListWidget::item { padding: 7px 10px; border-radius: 5px; font-size: 12.5px; color: #1F2937; }"
        "QListWidget::item:selected { background: #EFF6FF; color: #0D47A1; font-weight: 700; }");

    auto reloadCustomFolderList = [this, folderList, accFolderCombo]() {
        folderList->clear();
        if (!m_folders)
            return;
        const QString selAccId = accFolderCombo->currentData().toString();
        QString selAccEmail;
        if (m_accounts) {
            for (const AccountItem &acc : *m_accounts) {
                if (acc.id == selAccId) {
                    selAccEmail = acc.email;
                    break;
                }
            }
        }
        for (int i = 0; i < m_folders->size(); ++i) {
            const FolderItem &f = m_folders->at(i);
            if (!f.isCustom)
                continue;
            if (m_accounts && !f.rawImapMailbox.isEmpty()) {
                bool ownerIsPop3 = false;
                for (const AccountItem &acc : *m_accounts) {
                    if (acc.id == f.accountId
                        || (!f.accountEmail.isEmpty() && acc.email.compare(f.accountEmail, Qt::CaseInsensitive) == 0)) {
                        if (acc.isPop3())
                            ownerIsPop3 = true;
                        break;
                    }
                }
                if (ownerIsPop3)
                    continue;
            }
            if (!selAccId.isEmpty()) {
                bool matchId = (f.accountId == selAccId);
                bool matchEmail = (!selAccEmail.isEmpty() && !f.accountEmail.isEmpty()
                                   && f.accountEmail.compare(selAccEmail, Qt::CaseInsensitive) == 0);
                if (!matchId && !matchEmail)
                    continue;
            }
            QListWidgetItem *it = new QListWidgetItem(
                MailIcons::folderIcon(FolderKind::CustomFolder, f.dotColor.isValid() ? f.dotColor : QColor("#4F8EF7"), 16),
                QString("%1   [%2]").arg(f.name, f.accountEmail.isEmpty() ? f.accountId : f.accountEmail),
                folderList);
            it->setData(Qt::UserRole, i);
        }
        if (folderList->count() > 0)
            folderList->setCurrentRow(0);
    };
    connect(accFolderCombo, &QComboBox::currentIndexChanged, page, [reloadCustomFolderList](int) {
        reloadCustomFolderList();
    });
    if (m_googleProvider) {
        connect(m_googleProvider, &GoogleProvider::imapCustomFoldersDiscovered, page,
                [reloadCustomFolderList](const QString &, const QList<FolderItem> &) {
            reloadCustomFolderList();
        });
    }
    reloadCustomFolderList();
    cv->addWidget(folderList);

    // Add / Edit Folder input row
    QHBoxLayout *editRow = new QHBoxLayout();
    editRow->setSpacing(8);
    QLineEdit *folderNameEdit = new QLineEdit(custGroup);
    folderNameEdit->setPlaceholderText(MailI18n::t("새 폴더 이름 입력 (예: 프로젝트, 세금계산서, VIP고객)", "New folder name (e.g. Projects, Invoices, VIP)", "新しいフォルダ名を入力"));
    folderNameEdit->setStyleSheet("border: 1px solid #D1D5DB; border-radius: 5px; padding: 7px 10px; font-size: 12.5px;");

    QSharedPointer<QColor> selectedDotColor = QSharedPointer<QColor>::create("#4F8EF7");
    QPushButton *btnColorPick = new QPushButton(MailI18n::t("🎨 폴더 색상", "🎨 Color", "🎨 フォルダ色"), custGroup);
    btnColorPick->setCursor(Qt::PointingHandCursor);
    btnColorPick->setStyleSheet("background: #FFFFFF; color: #1F2937; font-weight: 600; border: 2px solid #4F8EF7; border-radius: 5px; padding: 6px 12px;");
    connect(btnColorPick, &QPushButton::clicked, this, [this, btnColorPick, selectedDotColor]() {
        QColor c = QColorDialog::getColor(*selectedDotColor, this, MailI18n::t("폴더 아이콘 색상 선택", "Choose Folder Color", "フォルダ色を選択"));
        if (c.isValid()) {
            *selectedDotColor = c;
            btnColorPick->setStyleSheet(QString("background: #FFFFFF; color: #1F2937; font-weight: 600; border: 2px solid %1; border-radius: 5px; padding: 6px 12px;").arg(c.name()));
        }
    });

    QPushButton *btnAddFolder = new QPushButton(MailI18n::t("+ 새 폴더 추가 (Add)", "+ Add Folder", "+ 新規フォルダ追加"), custGroup);
    btnAddFolder->setCursor(Qt::PointingHandCursor);
    btnAddFolder->setStyleSheet("background: #0D47A1; color: #FFFFFF; font-weight: 700; border: none; border-radius: 5px; padding: 7px 14px;");
    connect(btnAddFolder, &QPushButton::clicked, this, [this, folderNameEdit, selectedDotColor, accFolderCombo, reloadCustomFolderList]() {
        if (!m_folders)
            return;
        QString name = folderNameEdit->text().trimmed();
        if (name.isEmpty()) {
            QMessageBox::warning(this, MailI18n::t("폴더 이름 필요", "Folder Name Required", "フォルダ名が必要です"),
                                 MailI18n::t("추가할 폴더 이름을 입력해 주세요.", "Please enter a folder name.", "追加するフォルダ名を入力してください。"));
            return;
        }
        const QString selAccId = accFolderCombo->currentData().toString();
        AccountItem matchedAcc;
        bool foundAcc = false;
        if (m_accounts) {
            for (const AccountItem &acc : *m_accounts) {
                if (acc.id == selAccId) {
                    matchedAcc = acc;
                    foundAcc = true;
                    break;
                }
            }
        }

        if (foundAcc && !matchedAcc.isPop3()) {
            // IMAP 계정: 로컬 폴더를 만들지 않고 실제 IMAP 서버에 CREATE 명령을 전송하여 서버 폴더로 생성
            if (m_googleProvider) {
                m_googleProvider->createImapCustomFolder(matchedAcc, name);
            }
            folderNameEdit->clear();
            return;
        }

        // POP3 계정: 로컬 디스크(<저장경로>/pop3/<계정이메일>/<폴더ID>/)에 실제 물리 디렉터리 생성
        FolderItem nf;
        nf.id = QString("custom_%1_%2").arg(selAccId.isEmpty() ? "local" : selAccId, QUuid::createUuid().toString(QUuid::Id128).left(6));
        nf.accountId = selAccId;
        nf.accountEmail = matchedAcc.email;
        nf.name = name;
        nf.kind = FolderKind::CustomFolder;
        nf.isCustom = true;
        nf.dotColor = *selectedDotColor;
        if (foundAcc && matchedAcc.isPop3()) {
            matchedAcc.ensurePop3MailboxDirs();
            QDir().mkpath(matchedAcc.pop3MailboxDir(nf.id));
        }
        m_folders->append(nf);
        folderNameEdit->clear();
        reloadCustomFolderList();
        emit foldersUpdated();
    });

    QPushButton *btnRenameFolder = new QPushButton(MailI18n::t("✏️ 선택 폴더 이름/색상 변경", "✏️ Rename / Recolor", "✏️ 名前・色を変更"), custGroup);
    btnRenameFolder->setCursor(Qt::PointingHandCursor);
    btnRenameFolder->setStyleSheet("background: #F3F4F6; color: #1F2937; font-weight: 600; border: 1px solid #D1D5DB; border-radius: 5px; padding: 7px 12px;");
    connect(btnRenameFolder, &QPushButton::clicked, this, [this, folderList, folderNameEdit, selectedDotColor, reloadCustomFolderList]() {
        if (!m_folders || !folderList->currentItem())
            return;
        int idx = folderList->currentItem()->data(Qt::UserRole).toInt();
        if (idx < 0 || idx >= m_folders->size())
            return;
        const FolderItem oldFolder = m_folders->at(idx);
        const QString newName = folderNameEdit->text().trimmed();
        (*m_folders)[idx].dotColor = *selectedDotColor;
        if (!newName.isEmpty() && newName != oldFolder.name) {
            (*m_folders)[idx].name = newName;
            if (m_accounts && m_googleProvider) {
                for (const AccountItem &acc : *m_accounts) {
                    if (acc.id == oldFolder.accountId && !acc.isPop3()) {
                        const QString oldRef = oldFolder.rawImapMailbox.isEmpty() ? oldFolder.name : oldFolder.rawImapMailbox;
                        m_googleProvider->renameImapCustomFolder(acc, oldRef, newName);
                        break;
                    }
                }
            }
        }
        folderNameEdit->clear();
        reloadCustomFolderList();
        emit foldersUpdated();
    });

    QPushButton *btnDelFolder = new QPushButton(MailI18n::t("🗑️ 선택 폴더 삭제", "🗑️ Delete Folder", "🗑️ 選択フォルダ削除"), custGroup);
    btnDelFolder->setCursor(Qt::PointingHandCursor);
    btnDelFolder->setStyleSheet("background: #FEF2F2; color: #DC2626; font-weight: 700; border: 1px solid #FECACA; border-radius: 5px; padding: 7px 12px;");
    connect(btnDelFolder, &QPushButton::clicked, this, [this, folderList, reloadCustomFolderList]() {
        if (!m_folders || !folderList->currentItem())
            return;
        int idx = folderList->currentItem()->data(Qt::UserRole).toInt();
        if (idx < 0 || idx >= m_folders->size())
            return;
        const FolderItem targetFolder = m_folders->at(idx);
        const QString fname = targetFolder.name;
        if (QMessageBox::question(this,
                                  MailI18n::t("폴더 삭제 확인", "Delete Folder", "フォルダ削除の確認"),
                                  MailI18n::t(QString("'%1' 사용자 폴더를 삭제하시겠습니까?\n(폴더 안의 메일은 받은편지함으로 안전하게 이동됩니다.)").arg(fname),
                                              QString("Delete custom folder '%1'?").arg(fname),
                                              QString("'%1' フォルダを削除しますか？").arg(fname)),
                                  QMessageBox::Yes | QMessageBox::No) == QMessageBox::Yes) {
            if (m_accounts) {
                for (const AccountItem &acc : *m_accounts) {
                    if (acc.id == targetFolder.accountId) {
                        if (acc.isPop3()) {
                            const QString pop3SrcDir = acc.pop3MailboxDir(targetFolder.id);
                            const QString pop3InboxDir = acc.pop3MailboxDir(QStringLiteral("inbox"));
                            QDir dir(pop3SrcDir);
                            for (const QString &eml : dir.entryList(QStringList() << "*.eml", QDir::Files)) {
                                QFile::rename(dir.filePath(eml), pop3InboxDir + "/" + eml);
                            }
                            dir.removeRecursively();
                        } else if (m_googleProvider) {
                            const QString delRef = targetFolder.rawImapMailbox.isEmpty() ? targetFolder.name : targetFolder.rawImapMailbox;
                            m_googleProvider->deleteImapCustomFolder(acc, delRef);
                        }
                        break;
                    }
                }
            }
            int foundIdx = -1;
            for (int i = 0; i < m_folders->size(); ++i) {
                const FolderItem &f = m_folders->at(i);
                if ((!targetFolder.id.isEmpty() && f.id == targetFolder.id)
                    || (f.accountId == targetFolder.accountId && f.name == targetFolder.name && f.isCustom)) {
                    foundIdx = i;
                    break;
                }
            }
            if (foundIdx >= 0) {
                m_folders->removeAt(foundIdx);
            }
            reloadCustomFolderList();
            emit foldersUpdated();
        }
    });

    connect(folderList, &QListWidget::currentRowChanged, this, [this, folderList, folderNameEdit, selectedDotColor, btnColorPick](int row) {
        if (row < 0 || !folderList->item(row) || !m_folders)
            return;
        int idx = folderList->item(row)->data(Qt::UserRole).toInt();
        if (idx >= 0 && idx < m_folders->size()) {
            folderNameEdit->setText(m_folders->at(idx).name);
            *selectedDotColor = m_folders->at(idx).dotColor.isValid() ? m_folders->at(idx).dotColor : QColor("#4F8EF7");
            btnColorPick->setStyleSheet(QString("background: #FFFFFF; color: #1F2937; font-weight: 600; border: 2px solid %1; border-radius: 5px; padding: 6px 12px;").arg(selectedDotColor->name()));
        }
    });

    editRow->addWidget(folderNameEdit, 1);
    editRow->addWidget(btnColorPick);
    editRow->addWidget(btnAddFolder);
    cv->addLayout(editRow);

    QHBoxLayout *actRow = new QHBoxLayout();
    actRow->setSpacing(8);
    actRow->addWidget(btnRenameFolder);
    actRow->addWidget(btnDelFolder);
    actRow->addStretch();
    cv->addLayout(actRow);

    v->addWidget(custGroup);
    v->addStretch();
    scroll->setWidget(page);
    return scroll;
}

QWidget *SettingsDialog::createFiltersPage()
{
    QScrollArea *scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setStyleSheet("background: #FFFFFF;");

    QWidget *page = new QWidget(scroll);
    QVBoxLayout *v = new QVBoxLayout(page);
    v->setContentsMargins(36, 26, 36, 26);
    v->setSpacing(14);

    QHBoxLayout *hdrRow = new QHBoxLayout();
    hdrRow->setSpacing(8);
    QLabel *hdrIcon = new QLabel(page);
    hdrIcon->setPixmap(MailIcons::settingsNavIcon(5, QColor("#0D47A1"), 20).pixmap(20, 20));
    QLabel *t = new QLabel(MailI18n::t("필터 규칙 (Filters — 자동 분류 규칙)", "Filters & Auto-Routing Rules", "フィルター規則 (自動振り分け)"), page);
    t->setStyleSheet("font-size: 20px; font-weight: 600; color: #374151;");
    hdrRow->addWidget(hdrIcon);
    hdrRow->addWidget(t);
    hdrRow->addStretch();
    v->addLayout(hdrRow);

    QLabel *s = new QLabel(
        MailI18n::t("보낸사람(From), 받는사람(To), 제목(Subject), 본문(Body) 조건에 따라 메일을 지정한 폴더로 자동 이동하거나 읽음/별표 처리합니다. 각 규칙이 전체 계정에 적용되는지 특정 계정에만 적용되는지 계정별로 구분하여 관리할 수 있습니다.",
                    "Automatically move messages to a target folder or mark as read/starred based on From, To, Subject, or Body conditions, per account or across all accounts.",
                    "差出人、宛先、件名、本文の条件に応じて、アカウント別または全アカウント共通でメールを自動振り分けします。"),
        page);
    s->setWordWrap(true);
    s->setStyleSheet("font-size: 12px; color: #6B7280;");
    v->addWidget(s);

    auto resolveRuleAccountEmail = [this](const MailFilterRule &r) -> QString {
        const QString explicitEmail = r.accountEmail.trimmed();
        if (!explicitEmail.isEmpty() && explicitEmail.compare(QStringLiteral("ALL"), Qt::CaseInsensitive) != 0)
            return explicitEmail;

        if (r.targetFolderId.startsWith(QStringLiteral("custom_"))) {
            if (m_folders) {
                for (const FolderItem &fd : *m_folders) {
                    if (fd.id == r.targetFolderId) {
                        if (!fd.accountEmail.trimmed().isEmpty())
                            return fd.accountEmail.trimmed();
                        if (!fd.accountId.trimmed().isEmpty() && m_accounts) {
                            for (const AccountItem &acc : *m_accounts) {
                                if (acc.id == fd.accountId)
                                    return acc.email.trimmed();
                            }
                        }
                    }
                }
            }
            if (m_accounts) {
                for (const AccountItem &acc : *m_accounts) {
                    if (acc.provider == AccountProviderType::Unified)
                        continue;
                    if (r.targetFolderId.startsWith(QStringLiteral("custom_") + acc.id + QStringLiteral("_")))
                        return acc.email.trimmed();
                }
            }
        }
        return QString();
    };

    auto formatAccountBadge = [this](const QString &accEmail) -> QString {
        if (accEmail.trimmed().isEmpty()) {
            return MailI18n::t("[🌐 전체 계정 공통]", "[🌐 All Accounts]", "[🌐 全アカウント共通]");
        }
        if (m_accounts) {
            for (const AccountItem &acc : *m_accounts) {
                if (acc.provider == AccountProviderType::Unified)
                    continue;
                if (acc.email.compare(accEmail.trimmed(), Qt::CaseInsensitive) == 0) {
                    const QString disp = acc.displayName.trimmed().isEmpty() ? acc.email.trimmed() : acc.displayName.trimmed();
                    return QString("[📧 %1 <%2>]").arg(disp, acc.email.trimmed());
                }
            }
        }
        return QString("[📧 %1]").arg(accEmail.trimmed());
    };

    // Account filter bar at top of Filters page
    QHBoxLayout *accFilterRow = new QHBoxLayout();
    accFilterRow->setSpacing(10);
    QLabel *accFilterLbl = new QLabel(MailI18n::t("계정별 규칙 보기:", "Filter by Account:", "アカウント別表示:"), page);
    accFilterLbl->setStyleSheet("font-size: 12.5px; font-weight: 700; color: #1F2937;");
    QComboBox *accFilterCombo = new QComboBox(page);
    accFilterCombo->setMinimumWidth(340);
    accFilterCombo->setStyleSheet("QComboBox { border: 1px solid #D1D5DB; border-radius: 5px; padding: 6px 10px; font-size: 12.5px; background: #FFFFFF; color: #111827; font-weight: 600; }");
    accFilterCombo->addItem(MailI18n::t("🌐 전체 등록된 규칙 모두 보기 (All Rules)", "🌐 Show All Rules", "🌐 すべての規則を表示"), QString());
    accFilterCombo->addItem(MailI18n::t("🌐 전체 계정 공통 규칙만 보기 (Common Rules Only)", "🌐 Common Rules Only", "🌐 全アカウント共通のみ"), QStringLiteral("__COMMON__"));
    if (m_accounts) {
        for (const AccountItem &acc : *m_accounts) {
            if (acc.provider == AccountProviderType::Unified || acc.email.trimmed().isEmpty())
                continue;
            const QString disp = acc.displayName.trimmed().isEmpty() ? acc.email.trimmed() : acc.displayName.trimmed();
            accFilterCombo->addItem(QString("📧 %1 (%2)").arg(disp, acc.email.trimmed()), acc.email.trimmed());
        }
    }
    accFilterRow->addWidget(accFilterLbl);
    accFilterRow->addWidget(accFilterCombo);
    accFilterRow->addStretch();
    v->addLayout(accFilterRow);

    QListWidget *ruleList = new QListWidget(page);
    ruleList->setMaximumWidth(740);
    ruleList->setFixedHeight(195);
    ruleList->setStyleSheet(
        "QListWidget { border: 1px solid #D1D5DB; border-radius: 8px; padding: 6px; background: #FFFFFF; }"
        "QListWidget::item { padding: 8px 10px; border-radius: 5px; font-size: 12.5px; color: #111827; border-bottom: 1px solid #F3F4F6; }"
        "QListWidget::item:selected { background: #EFF6FF; color: #0D47A1; font-weight: 700; }");

    auto reloadRulesList = [this, ruleList, accFilterCombo, resolveRuleAccountEmail, formatAccountBadge]() {
        const int prevRuleIdx = ruleList->currentItem() ? ruleList->currentItem()->data(Qt::UserRole).toInt() : -1;
        ruleList->clear();
        const QString filterAcc = accFilterCombo->currentData().toString().trimmed();
        QList<MailFilterRule> &rules = MailI18n::filterRules();
        int rowToSelect = 0;
        for (int i = 0; i < rules.size(); ++i) {
            MailFilterRule &r = rules[i];
            const QString effectiveAccEmail = resolveRuleAccountEmail(r);
            if (r.accountEmail.trimmed().isEmpty() && !effectiveAccEmail.isEmpty()) {
                r.accountEmail = effectiveAccEmail;
            }
            if (filterAcc == QStringLiteral("__COMMON__")) {
                if (!effectiveAccEmail.isEmpty())
                    continue;
            } else if (!filterAcc.isEmpty()) {
                if (!effectiveAccEmail.isEmpty() && effectiveAccEmail.compare(filterAcc, Qt::CaseInsensitive) != 0)
                    continue;
            }

            QString status = r.enabled
                                 ? MailI18n::t("✅ [활성]", "✅ [Enabled]", "✅ [有効]")
                                 : MailI18n::t("⏸️ [비활성]", "⏸️ [Disabled]", "⏸️ [無効]");
            QString accBadge = formatAccountBadge(effectiveAccEmail);
            QString flags;
            if (r.markAsRead)
                flags += MailI18n::t(" + 읽음처리", " + Mark Read", " + 既読化");
            if (r.markAsStarred)
                flags += MailI18n::t(" + ⭐별표", " + ⭐Star", " + ⭐スター");
            QString folderDisplay = MailI18n::folderTitle(r.targetFolderId, r.targetFolderId);
            if (m_folders) {
                for (const FolderItem &fd : *m_folders) {
                    if (fd.id == r.targetFolderId && !fd.name.trimmed().isEmpty()) {
                        folderDisplay = MailI18n::folderTitle(r.targetFolderId, fd.name);
                        break;
                    }
                }
            }
            QString desc = MailI18n::t(
                QString("%1 %2 %3  —  조건: [%4]에 '%5' 포함  →  폴더 이동: [%6]%7")
                    .arg(status, accBadge, r.name, r.matchField, r.matchQuery, folderDisplay, flags),
                QString("%1 %2 %3  —  If [%4] contains '%5'  →  Move to: [%6]%7")
                    .arg(status, accBadge, r.name, r.matchField, r.matchQuery, folderDisplay, flags),
                QString("%1 %2 %3  —  条件: [%4]に '%5' を含む  →  移動先: [%6]%7")
                    .arg(status, accBadge, r.name, r.matchField, r.matchQuery, folderDisplay, flags));
            QListWidgetItem *it = new QListWidgetItem(desc, ruleList);
            it->setData(Qt::UserRole, i);
            if (i == prevRuleIdx)
                rowToSelect = ruleList->count() - 1;
        }
        if (ruleList->count() > 0)
            ruleList->setCurrentRow(rowToSelect);
    };
    reloadRulesList();
    v->addWidget(ruleList);

    // Action buttons for existing rules
    QHBoxLayout *ruleActRow = new QHBoxLayout();
    ruleActRow->setSpacing(8);
    QPushButton *btnToggleRule = new QPushButton(MailI18n::t("🔄 선택 규칙 활성/비활성 전환", "🔄 Toggle Rule Enabled", "🔄 有効/無効切替"), page);
    btnToggleRule->setCursor(Qt::PointingHandCursor);
    btnToggleRule->setStyleSheet("background: #F3F4F6; color: #1F2937; font-weight: 600; border: 1px solid #D1D5DB; border-radius: 5px; padding: 7px 12px;");
    connect(btnToggleRule, &QPushButton::clicked, this, [this, ruleList, reloadRulesList]() {
        if (!ruleList->currentItem())
            return;
        int idx = ruleList->currentItem()->data(Qt::UserRole).toInt();
        if (idx >= 0 && idx < MailI18n::filterRules().size()) {
            MailI18n::filterRules()[idx].enabled = !MailI18n::filterRules()[idx].enabled;
            reloadRulesList();
            emit accountsUpdated();
        }
    });

    QPushButton *btnDeleteRule = new QPushButton(MailI18n::t("🗑️ 선택 규칙 삭제", "🗑️ Delete Rule", "🗑️ 規則を削除"), page);
    btnDeleteRule->setCursor(Qt::PointingHandCursor);
    btnDeleteRule->setStyleSheet("background: #FEF2F2; color: #DC2626; font-weight: 700; border: 1px solid #FECACA; border-radius: 5px; padding: 7px 12px;");
    connect(btnDeleteRule, &QPushButton::clicked, this, [this, ruleList, reloadRulesList]() {
        if (!ruleList->currentItem())
            return;
        int idx = ruleList->currentItem()->data(Qt::UserRole).toInt();
        if (idx >= 0 && idx < MailI18n::filterRules().size()) {
            MailI18n::filterRules().removeAt(idx);
            reloadRulesList();
            emit accountsUpdated();
        }
    });

    QPushButton *btnRunNow = new QPushButton(MailI18n::t("▶️ 지금 전체 메일에 활성 필터 실행 (Run Filters Now)", "▶️ Run Active Filters Now", "▶️ 今すぐ全メールにフィルター実行"), page);
    btnRunNow->setCursor(Qt::PointingHandCursor);
    btnRunNow->setStyleSheet("background: #059669; color: #FFFFFF; font-weight: 700; border: none; border-radius: 5px; padding: 7px 16px;");
    connect(btnRunNow, &QPushButton::clicked, this, [this]() {
        emit runFilterRulesRequested();
    });

    ruleActRow->addWidget(btnToggleRule);
    ruleActRow->addWidget(btnDeleteRule);
    ruleActRow->addWidget(btnRunNow);
    ruleActRow->addStretch();
    v->addLayout(ruleActRow);

    // Add / Edit Rule Form
    QGroupBox *addBox = new QGroupBox(MailI18n::t("+ 새 필터 규칙 추가 / 선택 규칙 수정 (Create or Edit Filter Rule)", "+ Create or Edit Filter Rule", "+ フィルター規則の追加・編集"), page);
    addBox->setMaximumWidth(740);
    addBox->setStyleSheet("QGroupBox { font-size: 13px; font-weight: 700; color: #111827; border: 1px solid #E5E7EB; border-radius: 8px; margin-top: 8px; padding: 14px; background: #F9FAFB; }");
    QFormLayout *form = new QFormLayout(addBox);
    form->setSpacing(9);

    QComboBox *targetAccountCombo = new QComboBox(addBox);
    targetAccountCombo->setStyleSheet("background: #FFFFFF; border: 1px solid #D1D5DB; border-radius: 5px; padding: 6px 10px; font-weight: 600; color: #0D47A1;");
    targetAccountCombo->addItem(MailI18n::t("🌐 전체 계정 공통 (All Accounts)", "🌐 All Accounts (Common)", "🌐 全アカウント共通"), QString());
    if (m_accounts) {
        for (const AccountItem &acc : *m_accounts) {
            if (acc.provider == AccountProviderType::Unified || acc.email.trimmed().isEmpty())
                continue;
            const QString disp = acc.displayName.trimmed().isEmpty() ? acc.email.trimmed() : acc.displayName.trimmed();
            targetAccountCombo->addItem(QString("📧 %1 (%2)").arg(disp, acc.email.trimmed()), acc.email.trimmed());
        }
    }

    QLineEdit *ruleNameEdit = new QLineEdit(addBox);
    ruleNameEdit->setPlaceholderText(MailI18n::t("규칙 이름 (예: 사내 공지사항 중요편지함 분류)", "Rule name", "ルール名"));
    ruleNameEdit->setStyleSheet("background: #FFFFFF; border: 1px solid #D1D5DB; border-radius: 5px; padding: 6px 10px;");

    QHBoxLayout *condH = new QHBoxLayout();
    QComboBox *fieldCombo = new QComboBox(addBox);
    fieldCombo->addItem(MailI18n::t("보낸사람 (From)", "From", "差出人 (From)"), "From");
    fieldCombo->addItem(MailI18n::t("받는사람 (To)", "To", "宛先 (To)"), "To");
    fieldCombo->addItem(MailI18n::t("제목 (Subject)", "Subject", "件名 (Subject)"), "Subject");
    fieldCombo->addItem(MailI18n::t("본문 (Body)", "Body", "本文 (Body)"), "Body");
    fieldCombo->setStyleSheet("background: #FFFFFF; border: 1px solid #D1D5DB; border-radius: 5px; padding: 6px 10px;");

    QLineEdit *queryEdit = new QLineEdit(addBox);
    queryEdit->setPlaceholderText(MailI18n::t("포함할 검색어 입력 (쉼표 , 또는 ; 구분 시 OR 조건 매칭 — 예: 청구서, 영수증, @sample.com)",
                                              "Keyword(s) to match (comma/semicolon for OR — e.g. invoice, receipt, @sample.com)",
                                              "一致キーワード（カンマ , または ; 区切りでOR条件 — 例: 請求書, 領収書, @sample.com）"));
    queryEdit->setStyleSheet("background: #FFFFFF; border: 1px solid #D1D5DB; border-radius: 5px; padding: 6px 10px;");
    condH->addWidget(fieldCombo);
    condH->addWidget(queryEdit, 1);

    QComboBox *targetFolderCombo = new QComboBox(addBox);
    targetFolderCombo->setStyleSheet("background: #FFFFFF; border: 1px solid #D1D5DB; border-radius: 5px; padding: 6px 10px;");
    auto reloadTargetFolderCombo = [this, targetFolderCombo, targetAccountCombo, reloadRulesList]() {
        const QString prevData = targetFolderCombo->currentData().toString();
        const QString selAccEmail = targetAccountCombo->currentData().toString().trimmed();
        QString selAccId;
        if (!selAccEmail.isEmpty() && m_accounts) {
            for (const AccountItem &acc : *m_accounts) {
                if (acc.email.compare(selAccEmail, Qt::CaseInsensitive) == 0) {
                    selAccId = acc.id;
                    break;
                }
            }
        }

        targetFolderCombo->clear();
        targetFolderCombo->addItem(MailI18n::folderTitle("inbox"), "inbox");
        targetFolderCombo->addItem(MailI18n::folderTitle("important"), "important");
        targetFolderCombo->addItem(MailI18n::folderTitle("archive"), "archive");
        targetFolderCombo->addItem(MailI18n::folderTitle("starred"), "starred");
        targetFolderCombo->addItem(MailI18n::folderTitle("spam"), "spam");
        targetFolderCombo->addItem(MailI18n::folderTitle("trash"), "trash");
        if (m_folders) {
            for (const FolderItem &f : *m_folders) {
                if (!f.isCustom)
                    continue;
                QString fAccEmail = f.accountEmail.trimmed();
                if (fAccEmail.isEmpty() && !f.accountId.isEmpty() && m_accounts) {
                    for (const AccountItem &acc : *m_accounts) {
                        if (acc.id == f.accountId) {
                            fAccEmail = acc.email.trimmed();
                            break;
                        }
                    }
                }
                if (!selAccEmail.isEmpty()) {
                    const bool matchEmail = (!fAccEmail.isEmpty() && fAccEmail.compare(selAccEmail, Qt::CaseInsensitive) == 0);
                    const bool matchId = (!selAccId.isEmpty() && f.accountId == selAccId);
                    if (!matchEmail && !matchId)
                        continue;
                    targetFolderCombo->addItem(QString("📁 %1").arg(f.name), f.id);
                } else {
                    const QString suffix = fAccEmail.isEmpty() ? QString() : QString("  [%1]").arg(fAccEmail);
                    targetFolderCombo->addItem(QString("📁 %1%2").arg(f.name, suffix), f.id);
                }
            }
        }
        const int idx = targetFolderCombo->findData(prevData);
        if (idx >= 0)
            targetFolderCombo->setCurrentIndex(idx);
        reloadRulesList();
    };
    reloadTargetFolderCombo();
    connect(targetAccountCombo, &QComboBox::currentIndexChanged, this, [reloadTargetFolderCombo](int) {
        reloadTargetFolderCombo();
    });
    connect(accFilterCombo, &QComboBox::currentIndexChanged, this, [accFilterCombo, targetAccountCombo, reloadRulesList](int) {
        const QString fAcc = accFilterCombo->currentData().toString().trimmed();
        if (!fAcc.isEmpty() && fAcc != QStringLiteral("__COMMON__")) {
            const int idx = targetAccountCombo->findData(fAcc);
            if (idx >= 0)
                targetAccountCombo->setCurrentIndex(idx);
        } else if (fAcc == QStringLiteral("__COMMON__")) {
            targetAccountCombo->setCurrentIndex(0);
        }
        reloadRulesList();
    });
    connect(this, &SettingsDialog::foldersUpdated, page, reloadTargetFolderCombo);
    if (m_googleProvider) {
        connect(m_googleProvider, &GoogleProvider::imapCustomFoldersDiscovered, page,
                [reloadTargetFolderCombo](const QString &, const QList<FolderItem> &) {
            reloadTargetFolderCombo();
        });
    }

    QHBoxLayout *optH = new QHBoxLayout();
    QCheckBox *chkRead = new QCheckBox(MailI18n::t("자동으로 읽음 표시 (Mark as read)", "Mark as read", "自動的に既読にする"), addBox);
    QCheckBox *chkStar = new QCheckBox(MailI18n::t("자동으로 별표 추가 (Add star)", "Add star", "スターを付ける"), addBox);
    optH->addWidget(chkRead);
    optH->addWidget(chkStar);
    optH->addStretch();

    // Populate form when clicking an existing rule in ruleList
    connect(ruleList, &QListWidget::currentRowChanged, this,
            [ruleList, targetAccountCombo, ruleNameEdit, fieldCombo, queryEdit, targetFolderCombo, chkRead, chkStar, resolveRuleAccountEmail](int row) {
        if (row < 0 || !ruleList->item(row))
            return;
        const int idx = ruleList->item(row)->data(Qt::UserRole).toInt();
        if (idx < 0 || idx >= MailI18n::filterRules().size())
            return;
        const MailFilterRule &r = MailI18n::filterRules().at(idx);
        const QString effAcc = resolveRuleAccountEmail(r);
        int accIdx = 0;
        if (!effAcc.isEmpty()) {
            for (int k = 0; k < targetAccountCombo->count(); ++k) {
                if (targetAccountCombo->itemData(k).toString().compare(effAcc, Qt::CaseInsensitive) == 0) {
                    accIdx = k;
                    break;
                }
            }
        }
        targetAccountCombo->setCurrentIndex(accIdx);
        ruleNameEdit->setText(r.name);
        const int fIdx = fieldCombo->findData(r.matchField);
        if (fIdx >= 0)
            fieldCombo->setCurrentIndex(fIdx);
        queryEdit->setText(r.matchQuery);
        const int tfIdx = targetFolderCombo->findData(r.targetFolderId);
        if (tfIdx >= 0)
            targetFolderCombo->setCurrentIndex(tfIdx);
        chkRead->setChecked(r.markAsRead);
        chkStar->setChecked(r.markAsStarred);
    });

    QHBoxLayout *submitBtnRow = new QHBoxLayout();
    submitBtnRow->setSpacing(10);

    QPushButton *btnAddRule = new QPushButton(MailI18n::t("+ 새 필터 규칙 등록 (Add New Rule)", "+ Add Filter Rule", "+ 新規フィルター規則を追加"), addBox);
    btnAddRule->setCursor(Qt::PointingHandCursor);
    btnAddRule->setStyleSheet("background: #0D47A1; color: #FFFFFF; font-weight: 700; border: none; border-radius: 5px; padding: 8px 16px;");
    connect(btnAddRule, &QPushButton::clicked, this,
            [this, targetAccountCombo, ruleNameEdit, fieldCombo, queryEdit, targetFolderCombo, chkRead, chkStar, reloadRulesList, resolveRuleAccountEmail]() {
        QString q = queryEdit->text().trimmed();
        if (q.isEmpty()) {
            QMessageBox::warning(this, MailI18n::t("조건 검색어 필요", "Keyword Required", "キーワードが必要です"),
                                 MailI18n::t("필터링할 검색어(예: 도메인 또는 제목 키워드)를 입력해 주세요.", "Please enter a keyword to match.", "条件キーワードを入力してください。"));
            return;
        }
        MailFilterRule r;
        r.id = "rule_" + QUuid::createUuid().toString(QUuid::Id128).left(6);
        r.name = ruleNameEdit->text().trimmed().isEmpty()
                     ? MailI18n::t(QString("[%1: %2] 자동 분류").arg(fieldCombo->currentData().toString(), q),
                                   QString("[%1: %2] Auto Rule").arg(fieldCombo->currentData().toString(), q),
                                   QString("[%1: %2] 自動振り分け").arg(fieldCombo->currentData().toString(), q))
                     : ruleNameEdit->text().trimmed();
        r.enabled = true;
        r.matchField = fieldCombo->currentData().toString();
        r.matchQuery = q;
        r.targetFolderId = targetFolderCombo->currentData().toString();
        r.markAsRead = chkRead->isChecked();
        r.markAsStarred = chkStar->isChecked();
        r.accountEmail = targetAccountCombo->currentData().toString().trimmed();
        if (r.accountEmail.isEmpty()) {
            r.accountEmail = resolveRuleAccountEmail(r);
        }
        MailI18n::filterRules().append(r);
        ruleNameEdit->clear();
        queryEdit->clear();
        reloadRulesList();
        emit accountsUpdated();
    });

    QPushButton *btnUpdateRule = new QPushButton(MailI18n::t("✏️ 선택 규칙 수정 저장 (Update Selected Rule)", "✏️ Update Selected Rule", "✏️ 選択した規則を更新"), addBox);
    btnUpdateRule->setCursor(Qt::PointingHandCursor);
    btnUpdateRule->setStyleSheet("background: #EFF6FF; color: #0D47A1; font-weight: 700; border: 1px solid #93C5FD; border-radius: 5px; padding: 8px 16px;");
    connect(btnUpdateRule, &QPushButton::clicked, this,
            [this, ruleList, targetAccountCombo, ruleNameEdit, fieldCombo, queryEdit, targetFolderCombo, chkRead, chkStar, reloadRulesList, resolveRuleAccountEmail]() {
        if (!ruleList->currentItem())
            return;
        const int idx = ruleList->currentItem()->data(Qt::UserRole).toInt();
        if (idx < 0 || idx >= MailI18n::filterRules().size())
            return;
        QString q = queryEdit->text().trimmed();
        if (q.isEmpty()) {
            QMessageBox::warning(this, MailI18n::t("조건 검색어 필요", "Keyword Required", "キーワードが必要です"),
                                 MailI18n::t("필터링할 검색어(예: 도메인 또는 제목 키워드)를 입력해 주세요.", "Please enter a keyword to match.", "条件キーワードを入力してください。"));
            return;
        }
        MailFilterRule &r = MailI18n::filterRules()[idx];
        r.name = ruleNameEdit->text().trimmed().isEmpty()
                     ? MailI18n::t(QString("[%1: %2] 자동 분류").arg(fieldCombo->currentData().toString(), q),
                                   QString("[%1: %2] Auto Rule").arg(fieldCombo->currentData().toString(), q),
                                   QString("[%1: %2] 自動振り分け").arg(fieldCombo->currentData().toString(), q))
                     : ruleNameEdit->text().trimmed();
        r.matchField = fieldCombo->currentData().toString();
        r.matchQuery = q;
        r.targetFolderId = targetFolderCombo->currentData().toString();
        r.markAsRead = chkRead->isChecked();
        r.markAsStarred = chkStar->isChecked();
        r.accountEmail = targetAccountCombo->currentData().toString().trimmed();
        if (r.accountEmail.isEmpty()) {
            r.accountEmail = resolveRuleAccountEmail(r);
        }
        reloadRulesList();
        emit accountsUpdated();
    });

    submitBtnRow->addWidget(btnAddRule, 1);
    submitBtnRow->addWidget(btnUpdateRule, 1);

    form->addRow(MailI18n::t("적용 계정:", "Target Account:", "対象アカウント:"), targetAccountCombo);
    form->addRow(MailI18n::t("규칙 이름:", "Rule Name:", "規則名:"), ruleNameEdit);
    form->addRow(MailI18n::t("일치 조건:", "Condition:", "一致条件:"), condH);
    form->addRow(MailI18n::t("이동할 폴더:", "Move to Folder:", "移動先フォルダ:"), targetFolderCombo);
    form->addRow(MailI18n::t("추가 동작:", "Extra Actions:", "追加アクション:"), optH);
    form->addRow("", submitBtnRow);

    v->addWidget(addBox);
    v->addStretch();
    scroll->setWidget(page);
    return scroll;
}

QWidget *SettingsDialog::createComposingPage()
{
    QScrollArea *scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setStyleSheet("background: #FFFFFF;");

    QWidget *page = new QWidget(scroll);
    QVBoxLayout *v = new QVBoxLayout(page);
    v->setContentsMargins(36, 26, 36, 26);
    v->setSpacing(18);

    QLabel *t = new QLabel(
        MailI18n::t("편지 읽기 및 쓰기 (Reading & Composing)",
                    "Reading & Composing",
                    "メール閲覧・作成 (Reading & Composing)"),
        page);
    t->setStyleSheet("font-size: 20px; font-weight: 700; color: #1F2937;");
    v->addWidget(t);

    QLabel *s = new QLabel(
        MailI18n::t("메일 선택 시 읽음 처리·대화형 스레드 묶어보기 등 읽기 옵션과, 새 메일 작성 글꼴·발송 취소 대기 시간·서명 자동 삽입 등 쓰기 옵션을 한곳에서 설정합니다.",
                    "Configure message reading behavior, conversation threading, default compose font, undo send delay, and signature options.",
                    "メール選択時の既読処理・スレッド表示などの閲覧設定と、作成時の標準フォント・送信取り消し時間・署名自動挿入を設定します。"),
        page);
    s->setWordWrap(true);
    s->setStyleSheet("font-size: 12px; color: #6B7280;");
    v->addWidget(s);

    // 1. 편지 읽기 설정 카드 (Reading Options)
    QGroupBox *readBox = new QGroupBox(
        MailI18n::t("📖 편지 읽기 및 대화 스레드 설정 (Reading Options)",
                    "📖 Reading && Conversation Thread Options",
                    "📖 メール閲覧・スレッド表示設定"),
        page);
    readBox->setMaximumWidth(720);
    readBox->setStyleSheet("QGroupBox { font-size: 13px; font-weight: 700; color: #111827; border: 1px solid #D1D5DB; border-radius: 8px; margin-top: 8px; padding: 16px; background: #FFFFFF; }");
    QVBoxLayout *rv = new QVBoxLayout(readBox);
    rv->setSpacing(12);

    QCheckBox *chkMarkRead = new QCheckBox(
        MailI18n::t("메일 선택 즉시 '읽음(Read)' 상태로 자동 전환 (Mark message as read when selected)",
                    "Automatically mark message as read when selected in list",
                    "メール選択時に自動的に既読にする"),
        readBox);
    chkMarkRead->setChecked(MailI18n::markReadOnSelect());
    chkMarkRead->setStyleSheet("font-size: 13px; font-weight: 600; color: #1F2937;");
    connect(chkMarkRead, &QCheckBox::toggled, this, [](bool on) {
        MailI18n::markReadOnSelect() = on;
    });
    rv->addWidget(chkMarkRead);

    QCheckBox *chkThread = new QCheckBox(
        MailI18n::t("동일 주제의 메일을 대화형 스레드(Conversation Thread)로 묶어보기",
                    "Group messages by conversation thread",
                    "同じ件名のメールをスレッド形式でまとめて表示する"),
        readBox);
    chkThread->setChecked(MailI18n::groupByConversationThread());
    chkThread->setStyleSheet("font-size: 13px; font-weight: 600; color: #1F2937;");
    connect(chkThread, &QCheckBox::toggled, this, [](bool on) {
        MailI18n::groupByConversationThread() = on;
    });
    rv->addWidget(chkThread);
    v->addWidget(readBox);

    // 2. 편지 쓰기 및 발송 옵션 카드 (Composing & Sending Options)
    QGroupBox *fontBox = new QGroupBox(
        MailI18n::t("✍️ 편지 쓰기 및 발송 옵션 (Composing && Undo Send)",
                    "✍️ Default Compose Font && Sending Options",
                    "✍️ メール作成・送信オプション"),
        page);
    fontBox->setMaximumWidth(720);
    fontBox->setStyleSheet("QGroupBox { font-size: 13px; font-weight: 700; color: #111827; border: 1px solid #D1D5DB; border-radius: 8px; margin-top: 8px; padding: 16px; background: #FFFFFF; }");
    QVBoxLayout *cv = new QVBoxLayout(fontBox);
    cv->setSpacing(12);

    QFormLayout *form = new QFormLayout();
    form->setSpacing(12);

    QComboBox *fontCombo = new QComboBox(fontBox);
    fontCombo->addItems({"Malgun Gothic", "Segoe UI", "Inter", "Arial", "Consolas"});
    fontCombo->setCurrentText(MailI18n::defaultFontFamily());
    fontCombo->setStyleSheet("background: #FFFFFF; border: 1px solid #D1D5DB; border-radius: 5px; padding: 6px 10px; font-weight: 500;");
    connect(fontCombo, &QComboBox::currentTextChanged, this, [](const QString &f) {
        MailI18n::defaultFontFamily() = f;
    });

    QComboBox *sizeCombo = new QComboBox(fontBox);
    sizeCombo->addItems({"9", "10", "11", "12", "14", "16", "18"});
    sizeCombo->setCurrentText(QString::number(MailI18n::defaultFontSize()));
    sizeCombo->setStyleSheet("background: #FFFFFF; border: 1px solid #D1D5DB; border-radius: 5px; padding: 6px 10px; font-weight: 500;");
    connect(sizeCombo, &QComboBox::currentTextChanged, this, [](const QString &sz) {
        MailI18n::defaultFontSize() = sz.toInt();
    });

    QComboBox *undoCombo = new QComboBox(fontBox);
    undoCombo->addItem(MailI18n::t("사용 안 함 (즉시 발송)", "0 sec (Send immediately)", "0秒 (即時送信)"), 0);
    undoCombo->addItem(MailI18n::t("5초 대기 (권장)", "5 seconds (Recommended)", "5秒 (推奨)"), 5);
    undoCombo->addItem(MailI18n::t("10초 대기", "10 seconds", "10秒"), 10);
    undoCombo->addItem(MailI18n::t("30초 대기", "30 seconds", "30秒"), 30);
    int uIdx = undoCombo->findData(MailI18n::undoSendDelaySec());
    if (uIdx >= 0)
        undoCombo->setCurrentIndex(uIdx);
    undoCombo->setStyleSheet("background: #FFFFFF; border: 1px solid #D1D5DB; border-radius: 5px; padding: 6px 10px; font-weight: 500;");
    connect(undoCombo, &QComboBox::currentIndexChanged, this, [undoCombo]() {
        MailI18n::undoSendDelaySec() = undoCombo->currentData().toInt();
    });

    form->addRow(MailI18n::t("기본 글꼴 (Default Font):", "Default Font:", "標準フォント:"), fontCombo);
    form->addRow(MailI18n::t("기본 글자 크기 (Font Size):", "Default Size:", "フォントサイズ:"), sizeCombo);
    form->addRow(MailI18n::t("발송 취소 대기 시간 (Undo Send):", "Undo Send Delay:", "送信取り消し待機時間:"), undoCombo);
    cv->addLayout(form);

    QCheckBox *chkQuote = new QCheckBox(
        MailI18n::t("답장/전달 시 원본 메일 인용문 자동 포함 (Include quoted original message on reply)",
                    "Include quoted original message on reply/forward",
                    "返信・転送時に元メールの引用を自動的に含める"),
        fontBox);
    chkQuote->setChecked(MailI18n::includeQuoteOnReply());
    chkQuote->setStyleSheet("font-size: 13px; font-weight: 600; color: #1F2937;");
    connect(chkQuote, &QCheckBox::toggled, this, [](bool on) {
        MailI18n::includeQuoteOnReply() = on;
    });
    cv->addWidget(chkQuote);

    QCheckBox *chkSig = new QCheckBox(
        MailI18n::t("새 메일 및 답장 작성 시 계정 서명(Signature) 자동 삽입",
                    "Automatically append account signature when composing",
                    "新規作成および返信時に署名を自動挿入する"),
        fontBox);
    chkSig->setChecked(MailI18n::autoAppendSignature());
    chkSig->setStyleSheet("font-size: 13px; font-weight: 600; color: #1F2937;");
    connect(chkSig, &QCheckBox::toggled, this, [](bool on) {
        MailI18n::autoAppendSignature() = on;
    });
    cv->addWidget(chkSig);

    v->addWidget(fontBox);

    v->addStretch();
    scroll->setWidget(page);
    return scroll;
}

QWidget *SettingsDialog::createReadingPage()
{
    QScrollArea *scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setStyleSheet("background: #FFFFFF;");

    QWidget *page = new QWidget(scroll);
    QVBoxLayout *v = new QVBoxLayout(page);
    v->setContentsMargins(36, 26, 36, 26);
    v->setSpacing(16);

    QLabel *t = new QLabel(MailI18n::t("편지 읽기 (Reading)", "Reading", "メール閲覧 (Reading)"), page);
    t->setStyleSheet("font-size: 20px; font-weight: 600; color: #374151;");
    v->addWidget(t);

    QLabel *s = new QLabel(
        MailI18n::t("메일 목록에서 메시지를 클릭했을 때 읽음(\\Seen) 처리 동작과 스레드 묶어보기 방식을 설정합니다.",
                    "Configure mark-as-read behavior when selecting messages and conversation thread grouping.",
                    "メール選択時の既読処理動作およびスレッド表示方式を設定します。"),
        page);
    s->setWordWrap(true);
    s->setStyleSheet("font-size: 12px; color: #6B7280;");
    v->addWidget(s);

    QCheckBox *chkMarkRead = new QCheckBox(
        MailI18n::t("메일 선택 즉시 '읽음(Read)' 상태로 자동 전환 (Mark message as read when selected)",
                    "Automatically mark message as read when selected in list",
                    "メール選択時に自動的に既読にする"),
        page);
    chkMarkRead->setChecked(MailI18n::markReadOnSelect());
    chkMarkRead->setStyleSheet("font-size: 13px; font-weight: 700; color: #111827;");
    connect(chkMarkRead, &QCheckBox::toggled, this, [](bool on) {
        MailI18n::markReadOnSelect() = on;
    });
    v->addWidget(chkMarkRead);

    QCheckBox *chkThread = new QCheckBox(
        MailI18n::t("동일 주제의 메일을 대화형 스레드(Conversation Thread)로 묶어보기",
                    "Group messages by conversation thread",
                    "同じ件名のメールをスレッド形式でまとめて表示する"),
        page);
    chkThread->setChecked(MailI18n::groupByConversationThread());
    chkThread->setStyleSheet("font-size: 13px; font-weight: 700; color: #111827;");
    connect(chkThread, &QCheckBox::toggled, this, [](bool on) {
        MailI18n::groupByConversationThread() = on;
    });
    v->addWidget(chkThread);

    v->addStretch();
    scroll->setWidget(page);
    return scroll;
}

QWidget *SettingsDialog::createCalendarSettingsPage()
{
    QScrollArea *scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setStyleSheet("background: #FFFFFF;");

    QWidget *page = new QWidget(scroll);
    QVBoxLayout *v = new QVBoxLayout(page);
    v->setContentsMargins(36, 26, 36, 26);
    v->setSpacing(14);

    // Header: Calendar icon + "Calendar"
    QHBoxLayout *hdrRow = new QHBoxLayout();
    hdrRow->setSpacing(8);
    QLabel *hdrIcon = new QLabel(page);
    hdrIcon->setPixmap(MailIcons::settingsNavIcon(7, QColor("#0D47A1"), 20).pixmap(20, 20));
    QLabel *t = new QLabel(MailI18n::t("캘린더 (Calendar)", "Calendar", "カレンダー (Calendar)"), page);
    t->setStyleSheet("font-size: 20px; font-weight: 500; color: #374151;");
    hdrRow->addWidget(hdrIcon);
    hdrRow->addWidget(t);
    hdrRow->addStretch();
    v->addLayout(hdrRow);
    v->addSpacing(6);

    // 1. Per-account Calendar Enable/Disable List
    if (m_accounts) {
        for (int i = 0; i < m_accounts->size(); ++i) {
            const AccountItem &acc = m_accounts->at(i);
            if (acc.provider == AccountProviderType::Unified || acc.id == "acc_unified")
                continue;

            QFrame *accCard = new QFrame(page);
            accCard->setMaximumWidth(780);
            accCard->setFixedHeight(58);
            accCard->setStyleSheet(
                "QFrame { background: #F9FAFB; border: none; border-radius: 6px; }");
            QHBoxLayout *ch = new QHBoxLayout(accCard);
            ch->setContentsMargins(14, 8, 16, 8);
            ch->setSpacing(12);

            QLabel *avLbl = new QLabel(accCard);
            avLbl->setFixedSize(36, 36);
            avLbl->setPixmap(MailIcons::accountAvatarPixmap(acc, 36, false));
            ch->addWidget(avLbl);

            QVBoxLayout *tv = new QVBoxLayout();
            tv->setContentsMargins(0, 0, 0, 0);
            tv->setSpacing(1);

            const bool isLocalCal = (acc.provider != AccountProviderType::Google);
            QString nameHtml = QString("<span style='color:#111827; font-weight:700; font-size:12.5px;'>%1</span>")
                                   .arg(acc.displayName.toHtmlEscaped());
            if (isLocalCal) {
                nameHtml += QString(" <span style='color:#6B7280; font-weight:400; font-size:12px;'>%1</span>")
                                .arg(MailI18n::t("(로컬 전용)", "(Local only)", "(ローカル専用)"));
            }
            QLabel *nameLbl = new QLabel(nameHtml, accCard);
            nameLbl->setTextFormat(Qt::RichText);

            QLabel *emailLbl = new QLabel(acc.email, accCard);
            emailLbl->setStyleSheet("color: #6B7280; font-size: 11.5px;");

            tv->addWidget(nameLbl);
            tv->addWidget(emailLbl);
            ch->addLayout(tv, 1);

            ToggleSwitchWidget *sw = new ToggleSwitchWidget(acc.calendarEnabled, accCard);
            connect(sw, &ToggleSwitchWidget::toggled, this, [this, i](bool enabled) {
                if (m_accounts && i >= 0 && i < m_accounts->size()) {
                    (*m_accounts)[i].calendarEnabled = enabled;
                    emit calendarSettingsChanged();
                    emit accountsUpdated();
                }
            });
            ch->addWidget(sw);

            v->addWidget(accCard);
        }
    }

    // 2. "Calendar appearance" section
    v->addSpacing(10);
    QLabel *appHdr = new QLabel(
        MailI18n::t("Calendar appearance (캘린더 모양)", "Calendar appearance", "Calendar appearance (カレンダー外観)"),
        page);
    appHdr->setStyleSheet("font-size: 16px; font-weight: 500; color: #374151;");
    v->addWidget(appHdr);

    auto makeCalToggleRow = [page](const QString &title,
                                   const QString &subtitle,
                                   bool initialChecked,
                                   std::function<void(bool)> onChanged) -> QWidget * {
        QWidget *row = new QWidget(page);
        row->setMaximumWidth(780);
        QHBoxLayout *h = new QHBoxLayout(row);
        h->setContentsMargins(0, 4, 0, 4);
        h->setSpacing(14);

        ToggleSwitchWidget *sw = new ToggleSwitchWidget(initialChecked, row);
        QObject::connect(sw, &ToggleSwitchWidget::toggled, row, onChanged);
        h->addWidget(sw, 0, Qt::AlignVCenter);

        QVBoxLayout *tv = new QVBoxLayout();
        tv->setContentsMargins(0, 0, 0, 0);
        tv->setSpacing(2);
        QLabel *tLbl = new QLabel(title, row);
        tLbl->setStyleSheet("font-size: 12.5px; font-weight: 700; color: #111827;");
        QLabel *sLbl = new QLabel(subtitle, row);
        sLbl->setStyleSheet("font-size: 11.5px; color: #6B7280;");
        tv->addWidget(tLbl);
        tv->addWidget(sLbl);
        h->addLayout(tv, 1);
        return row;
    };

    v->addWidget(makeCalToggleRow(
        MailI18n::t("Use 24-hour format (24시간 형식 사용)", "Use 24-hour format", "24時間形式を使用"),
        MailI18n::t("Display times as 14:00 instead of 2:00 PM (오후 2:00 대신 14:00로 표시)",
                    "Display times as 14:00 instead of 2:00 PM",
                    "午後2:00の代わりに14:00と表示します"),
        MailI18n::calendarUse24HourFormat(),
        [this](bool val) {
            MailI18n::calendarUse24HourFormat() = val;
            emit calendarSettingsChanged();
        }));

    v->addWidget(makeCalToggleRow(
        MailI18n::t("Show week numbers (주차 번호 표시)", "Show week numbers", "週番号を表示"),
        MailI18n::t("Display week numbers in the month and week views (월간 및 주간 뷰에 주차 번호 표시)",
                    "Display week numbers in the month and week views",
                    "月間および週間ビューに週番号を表示します"),
        MailI18n::calendarShowWeekNumbers(),
        [this](bool val) {
            MailI18n::calendarShowWeekNumbers() = val;
            emit calendarSettingsChanged();
        }));

    v->addWidget(makeCalToggleRow(
        MailI18n::t("Show right-docked calendar sidebar (메일함 우측 일간 캘린더 사이드바 표시)",
                    "Show right-docked calendar sidebar",
                    "メール画面の右側に日次カレンダーサイドバーを表示"),
        MailI18n::t("Display daily schedule timeline on the right side of the mail window",
                    "Display daily schedule timeline on the right side of the mail window",
                    "メール画面の右側に日次タイムラインを表示します"),
        MailI18n::showRightCalendarSidebar(),
        [this](bool val) {
            MailI18n::showRightCalendarSidebar() = val;
            emit toggleRightCalendarRequested(val);
        }));

    // "Start week on:" dropdown (Sunday / Monday)
    v->addSpacing(6);
    QLabel *startWeekLbl = new QLabel(
        MailI18n::t("Start week on: (주간 시작 요일:)", "Start week on:", "週の開始曜日:"),
        page);
    startWeekLbl->setStyleSheet("font-size: 11.5px; color: #6B7280;");
    v->addWidget(startWeekLbl);

    QComboBox *startDayCombo = new QComboBox(page);
    startDayCombo->setFixedWidth(160);
    startDayCombo->addItem(MailI18n::t("Sunday (일요일)", "Sunday", "Sunday (日曜日)"), 7);
    startDayCombo->addItem(MailI18n::t("Monday (월요일)", "Monday", "Monday (月曜日)"), 1);
    startDayCombo->setCurrentIndex(MailI18n::calendarWeekStartDay() == 7 ? 0 : 1);
    startDayCombo->setStyleSheet(
        "QComboBox { background: #FFFFFF; border: 1px solid #0D47A1; border-radius: 5px; padding: 6px 10px; font-size: 12.5px; color: #111827; }");
    connect(startDayCombo, &QComboBox::currentIndexChanged, this, [this, startDayCombo]() {
        MailI18n::calendarWeekStartDay() = startDayCombo->currentData().toInt();
        emit calendarSettingsChanged();
    });
    v->addWidget(startDayCombo);

    // 3. "Events" section
    v->addSpacing(14);
    QLabel *evHdr = new QLabel(
        MailI18n::t("Events (일정 기본값)", "Events", "Events (イベント)"),
        page);
    evHdr->setStyleSheet("font-size: 16px; font-weight: 500; color: #374151;");
    v->addWidget(evHdr);

    QLabel *durLbl = new QLabel(
        MailI18n::t("Default event duration: (기본 일정 길이:)", "Default event duration:", "標準のイベント時間:"),
        page);
    durLbl->setStyleSheet("font-size: 11.5px; color: #6B7280;");
    v->addWidget(durLbl);

    QComboBox *durCombo = new QComboBox(page);
    durCombo->setFixedWidth(160);
    durCombo->addItem(MailI18n::t("15분 (15 minutes)", "15 minutes", "15分 (15 minutes)"), 15);
    durCombo->addItem(MailI18n::t("30분 (30 minutes)", "30 minutes", "30分 (30 minutes)"), 30);
    durCombo->addItem(MailI18n::t("45분 (45 minutes)", "45 minutes", "45分 (45 minutes)"), 45);
    durCombo->addItem(MailI18n::t("60분 (60 minutes)", "60 minutes", "60分 (60 minutes)"), 60);
    durCombo->addItem(MailI18n::t("90분 (90 minutes)", "90 minutes", "90分 (90 minutes)"), 90);
    durCombo->addItem(MailI18n::t("120분 (120 minutes)", "120 minutes", "120分 (120 minutes)"), 120);
    int dIdx = durCombo->findData(MailI18n::calendarDefaultDurationMin());
    durCombo->setCurrentIndex(dIdx >= 0 ? dIdx : 3);
    durCombo->setStyleSheet(
        "QComboBox { background: #FFFFFF; border: 1px solid #D1D5DB; border-radius: 5px; padding: 6px 10px; font-size: 12.5px; color: #111827; }");
    connect(durCombo, &QComboBox::currentIndexChanged, this, [this, durCombo]() {
        MailI18n::calendarDefaultDurationMin() = durCombo->currentData().toInt();
        emit calendarSettingsChanged();
    });
    v->addWidget(durCombo);

    v->addStretch();
    scroll->setWidget(page);
    return scroll;
}

QWidget *SettingsDialog::createNotificationsPage()
{
    QScrollArea *scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setStyleSheet("background: #FFFFFF;");

    QWidget *page = new QWidget(scroll);
    QVBoxLayout *v = new QVBoxLayout(page);
    v->setContentsMargins(36, 26, 36, 26);
    v->setSpacing(16);

    QLabel *t = new QLabel(MailI18n::t("알림 설정 (Notifications)", "Notifications", "通知設定 (Notifications)"), page);
    t->setStyleSheet("font-size: 20px; font-weight: 600; color: #374151;");
    v->addWidget(t);

    QCheckBox *chkBanner = new QCheckBox(
        MailI18n::t("새 메일 도착 시 데스크톱 알림 표시 (Show desktop notification on new mail)",
                    "Show desktop notification banner when new email arrives",
                    "新着メール受信時にデスクトップ通知を表示する"),
        page);
    chkBanner->setChecked(MailI18n::enableDesktopNotification());
    chkBanner->setStyleSheet("font-size: 13px; font-weight: 700; color: #111827;");
    connect(chkBanner, &QCheckBox::toggled, this, [](bool on) {
        MailI18n::enableDesktopNotification() = on;
    });
    v->addWidget(chkBanner);

    QCheckBox *chkSound = new QCheckBox(
        MailI18n::t("새 메일 도착 시 알림 효과음 재생 (Play notification sound)",
                    "Play notification sound when new mail arrives",
                    "新着メール受信時に通知音を鳴らす"),
        page);
    chkSound->setChecked(MailI18n::enableNotificationSound());
    chkSound->setStyleSheet("font-size: 13px; font-weight: 700; color: #111827;");
    connect(chkSound, &QCheckBox::toggled, this, [](bool on) {
        MailI18n::enableNotificationSound() = on;
    });
    v->addWidget(chkSound);

    QPushButton *btnTestNotif = new QPushButton(MailI18n::t("🔔 알림음 및 배너 테스트 실행", "🔔 Test Notification & Sound", "🔔 通知テストを実行"), page);
    btnTestNotif->setCursor(Qt::PointingHandCursor);
    btnTestNotif->setMaximumWidth(260);
    btnTestNotif->setStyleSheet("background: #F3F4F6; color: #111827; font-weight: 700; border: 1px solid #D1D5DB; border-radius: 6px; padding: 8px 16px;");
    connect(btnTestNotif, &QPushButton::clicked, this, [this]() {
        if (MailI18n::enableNotificationSound())
            QApplication::beep();
        QMessageBox::information(this,
                                 MailI18n::t("🔔 SollunaMail 새 메일 알림 테스트", "🔔 SollunaMail Notification Test", "🔔 通知テスト"),
                                 MailI18n::t("새 이메일 도착 시 이와 같이 알림과 효과음이 정상 동작합니다.",
                                             "Desktop notification and sound alert are working properly.",
                                             "新着メール通知および効果音が正常に動作しています。"));
    });
    v->addWidget(btnTestNotif);

    v->addStretch();
    scroll->setWidget(page);
    return scroll;
}

QWidget *SettingsDialog::createSignaturesPage()
{
    QScrollArea *scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setStyleSheet("background: #FFFFFF;");

    QWidget *page = new QWidget(scroll);
    QVBoxLayout *v = new QVBoxLayout(page);
    v->setContentsMargins(36, 26, 36, 26);
    v->setSpacing(14);

    QLabel *t = new QLabel(MailI18n::t("서명 관리 (Signatures — 계정별 HTML 및 이미지 로고 서명)", "Email Signatures (Rich HTML & Images)", "署名管理 (HTML・画像ロゴ対応)"), page);
    t->setStyleSheet("font-size: 20px; font-weight: 600; color: #374151;");
    v->addWidget(t);

    QLabel *s = new QLabel(
        MailI18n::t("계정별로 이메일 하단에 자동 첨부될 HTML 서명(글꼴·색상·링크·회사 로고/배너 이미지 포함)을 편집하고 저장합니다.\n"
                    "삽입된 이미지는 메일 발송 시 표준 인라인 이미지(MIME cid:)로 자동 인코딩되어 Gmail, 네이버, 다음, Outlook 등 모든 메일함에서 깨짐 없이 표시됩니다.",
                    "Edit and save rich HTML signatures with company logos/images per account. Inline images are automatically encoded as RFC 2387 cid: parts on send.",
                    "アカウントごとにメール末尾へ自動挿入されるHTML署名（フォント・色・リンク・会社ロゴ画像）を編集・保存します。"),
        page);
    s->setWordWrap(true);
    s->setStyleSheet("font-size: 12px; color: #6B7280;");
    v->addWidget(s);

    QCheckBox *chkAutoSig = new QCheckBox(
        MailI18n::t("편지 쓰기 시 서명 자동 삽입 활성화 (Enable signature on compose)",
                    "Automatically append signature when composing emails",
                    "メール作成時に署名を自動挿入する"),
        page);
    chkAutoSig->setChecked(MailI18n::autoAppendSignature());
    chkAutoSig->setStyleSheet("font-size: 13px; font-weight: 700; color: #111827;");
    connect(chkAutoSig, &QCheckBox::toggled, this, [this](bool on) {
        MailI18n::autoAppendSignature() = on;
        emit accountsUpdated();
    });
    v->addWidget(chkAutoSig);

    QHBoxLayout *accRow = new QHBoxLayout();
    accRow->setSpacing(10);
    QLabel *accLbl = new QLabel(MailI18n::t("서명 대상 계정 선택:", "Select Account:", "対象アカウント:"), page);
    accLbl->setStyleSheet("font-size: 12.5px; font-weight: 700; color: #374151;");
    QComboBox *accCombo = new QComboBox(page);
    accCombo->setMinimumWidth(340);
    accCombo->setStyleSheet("background: #FFFFFF; border: 1px solid #D1D5DB; border-radius: 5px; padding: 6px 10px;");
    accCombo->addItem(MailI18n::t("🌐 공통 기본 서명 (Default Signature)", "🌐 Default Signature", "🌐 共通標準署名"), -1);
    if (m_accounts) {
        for (int i = 0; i < m_accounts->size(); ++i) {
            if (m_accounts->at(i).provider == AccountProviderType::Unified)
                continue;
            accCombo->addItem(QString("%1 <%2>").arg(m_accounts->at(i).displayName, m_accounts->at(i).email), i);
        }
    }
    accRow->addWidget(accLbl);
    accRow->addWidget(accCombo);
    accRow->addStretch();
    v->addLayout(accRow);

    QTextEdit *sigEditor = new QTextEdit(page);
    sigEditor->setMaximumWidth(740);
    sigEditor->setFixedHeight(240);
    sigEditor->setStyleSheet("border: 1px solid #D1D5DB; border-radius: 6px; padding: 10px; font-size: 12.5px; background: #FFFFFF;");

    QTextEdit *htmlCodeEditor = new QTextEdit(page);
    htmlCodeEditor->setMaximumWidth(740);
    htmlCodeEditor->setFixedHeight(240);
    htmlCodeEditor->setAcceptRichText(false);
    htmlCodeEditor->setFont(QFont("Consolas", 10));
    htmlCodeEditor->setPlaceholderText("<!-- HTML 서명 코드 입력 (예: <table>, <img src=\"...\">, <a href=\"...\">) -->");
    htmlCodeEditor->setStyleSheet("border: 1px solid #93C5FD; border-radius: 6px; padding: 10px; font-size: 12px; background: #F8FAFC; color: #0F172A;");
    htmlCodeEditor->setVisible(false);

    auto registerInlineDataImages = [](QTextDocument *doc, const QString &html) {
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
    };

    auto setSigEditorHtmlWithImages = [sigEditor, htmlCodeEditor, registerInlineDataImages](const QString &html) {
        registerInlineDataImages(sigEditor->document(), html);
        sigEditor->setHtml(html);
        htmlCodeEditor->setPlainText(html);
    };

    // Rich Formatting & Image/HTML Toolbar for Signature
    QHBoxLayout *fmtRow = new QHBoxLayout();
    fmtRow->setSpacing(5);

    QComboBox *fontCombo = new QComboBox(page);
    fontCombo->addItems({"Malgun Gothic", "Segoe UI", "Inter", "Arial", "Consolas"});
    fontCombo->setCurrentText(MailI18n::defaultFontFamily());
    fontCombo->setStyleSheet("background: #FFFFFF; border: 1px solid #D1D5DB; border-radius: 4px; padding: 3px 6px; font-size: 11.5px;");
    connect(fontCombo, &QComboBox::currentTextChanged, sigEditor, [sigEditor](const QString &f) {
        sigEditor->setFontFamily(f);
    });

    QComboBox *sizeCombo = new QComboBox(page);
    sizeCombo->addItems({"9", "10", "11", "12", "14", "16", "18"});
    sizeCombo->setCurrentText(QString::number(MailI18n::defaultFontSize()));
    sizeCombo->setStyleSheet("background: #FFFFFF; border: 1px solid #D1D5DB; border-radius: 4px; padding: 3px 6px; font-size: 11.5px;");
    connect(sizeCombo, &QComboBox::currentTextChanged, sigEditor, [sigEditor](const QString &sz) {
        sigEditor->setFontPointSize(sz.toDouble());
    });

    const QString toolBtnStyle =
        "QToolButton { background: #F9FAFB; color: #1F2937; border: 1px solid #D1D5DB; border-radius: 4px; padding: 4px 8px; font-size: 11.5px; font-weight: 700; }"
        "QToolButton:hover { background: #EFF6FF; color: #0D47A1; border-color: #93C5FD; }"
        "QToolButton:checked { background: #0D47A1; color: #FFFFFF; border-color: #0D47A1; }";

    QToolButton *btnBold = new QToolButton(page);
    btnBold->setText("B");
    btnBold->setStyleSheet(toolBtnStyle);
    connect(btnBold, &QToolButton::clicked, sigEditor, [sigEditor]() {
        const bool isBold = sigEditor->fontWeight() > QFont::Normal;
        sigEditor->setFontWeight(isBold ? QFont::Normal : QFont::Bold);
    });

    QToolButton *btnItalic = new QToolButton(page);
    btnItalic->setText("I");
    btnItalic->setStyleSheet(toolBtnStyle);
    connect(btnItalic, &QToolButton::clicked, sigEditor, [sigEditor]() {
        sigEditor->setFontItalic(!sigEditor->fontItalic());
    });

    QToolButton *btnUnder = new QToolButton(page);
    btnUnder->setText("U");
    btnUnder->setStyleSheet(toolBtnStyle);
    connect(btnUnder, &QToolButton::clicked, sigEditor, [sigEditor]() {
        sigEditor->setFontUnderline(!sigEditor->fontUnderline());
    });

    QToolButton *btnColor = new QToolButton(page);
    btnColor->setText(MailI18n::t("🎨 색상", "🎨 Color", "🎨 文字色"));
    btnColor->setStyleSheet(toolBtnStyle);
    connect(btnColor, &QToolButton::clicked, this, [this, sigEditor]() {
        const QColor c = QColorDialog::getColor(sigEditor->textColor(), this, MailI18n::t("글자 색상 선택", "Select Text Color", "文字色を選択"));
        if (c.isValid())
            sigEditor->setTextColor(c);
    });

    QToolButton *btnLink = new QToolButton(page);
    btnLink->setText(MailI18n::t("🔗 링크", "🔗 Link", "🔗 リンク"));
    btnLink->setStyleSheet(toolBtnStyle);
    connect(btnLink, &QToolButton::clicked, this, [this, sigEditor]() {
        bool ok = false;
        const QString url = QInputDialog::getText(
            this,
            MailI18n::t("서명에 하이퍼링크 삽입", "Insert Hyperlink", "リンクを挿入"),
            MailI18n::t("연결할 웹 주소(URL) 또는 이메일(mailto:)을 입력하세요:", "Enter URL (https://...) or mailto: address:", "URLを入力してください:"),
            QLineEdit::Normal,
            QStringLiteral("https://"),
            &ok).trimmed();
        if (!ok || url.isEmpty())
            return;
        QString label = sigEditor->textCursor().selectedText().trimmed();
        if (label.isEmpty())
            label = url;
        sigEditor->textCursor().insertHtml(
            QString("<a href=\"%1\" style=\"color:#0D47A1;text-decoration:underline;\">%2</a>")
                .arg(url.toHtmlEscaped(), label.toHtmlEscaped()));
    });

    QToolButton *btnInsertImg = new QToolButton(page);
    btnInsertImg->setText(MailI18n::t("🖼️ 이미지/로고 첨부", "🖼️ Insert Image/Logo", "🖼️ 画像/ロゴ挿入"));
    btnInsertImg->setCursor(Qt::PointingHandCursor);
    btnInsertImg->setStyleSheet(
        "QToolButton { background: #EFF6FF; color: #0D47A1; border: 1px solid #93C5FD; border-radius: 4px; padding: 4px 10px; font-size: 11.5px; font-weight: 700; }"
        "QToolButton:hover { background: #DBEAFE; }");
    connect(btnInsertImg, &QToolButton::clicked, this, [this, sigEditor, htmlCodeEditor, registerInlineDataImages]() {
        QDialog imgDlg(this);
        imgDlg.setWindowTitle(MailI18n::t("서명에 이미지 / 회사 로고 첨부", "Insert Signature Image / Logo", "署名に画像・ロゴを挿入"));
        imgDlg.resize(460, 240);
        imgDlg.setStyleSheet("QDialog { background: #FFFFFF; }");
        QVBoxLayout *iv = new QVBoxLayout(&imgDlg);
        iv->setContentsMargins(18, 16, 18, 16);
        iv->setSpacing(10);

        QLabel *guide = new QLabel(
            MailI18n::t("내 PC의 이미지 파일(PNG/JPG/GIF/WEBP)을 선택하거나 외부 이미지 URL을 입력하세요.\n"
                        "로컬 이미지는 선택한 너비로 최적화되어 서명에 내장되며, 메일 발송 시 표준 cid: 인라인 이미지로 전송됩니다.",
                        "Choose an image file from your PC or enter an external image URL (https://...).",
                        "PC内の画像ファイルを選択するか、外部画像URLを入力してください。"),
            &imgDlg);
        guide->setWordWrap(true);
        guide->setStyleSheet("font-size: 11.5px; color: #4B5563;");
        iv->addWidget(guide);

        QHBoxLayout *fileRow = new QHBoxLayout();
        QLineEdit *pathOrUrlEdit = new QLineEdit(&imgDlg);
        pathOrUrlEdit->setPlaceholderText(MailI18n::t("파일 찾아보기 또는 https:// 이미지 URL 입력...", "Browse file or enter https:// image URL...", "ファイルを選択または https:// 画像URLを入力..."));
        pathOrUrlEdit->setStyleSheet("padding: 6px 10px; border: 1px solid #D1D5DB; border-radius: 5px; font-size: 12px;");
        QPushButton *btnBrowse = new QPushButton(MailI18n::t("📁 파일 선택...", "📁 Browse...", "📁 ファイル選択..."), &imgDlg);
        btnBrowse->setStyleSheet("background: #F3F4F6; color: #1F2937; font-weight: 600; border: 1px solid #D1D5DB; border-radius: 5px; padding: 6px 12px;");
        connect(btnBrowse, &QPushButton::clicked, &imgDlg, [&imgDlg, pathOrUrlEdit]() {
            const QString f = QFileDialog::getOpenFileName(
                &imgDlg,
                MailI18n::t("서명 이미지/로고 선택", "Select Signature Image", "署名画像を選択"),
                QString(),
                MailI18n::t("이미지 파일 (*.png *.jpg *.jpeg *.gif *.webp *.bmp)", "Images (*.png *.jpg *.jpeg *.gif *.webp *.bmp)", "画像 (*.png *.jpg *.jpeg *.gif *.webp *.bmp)"));
            if (!f.isEmpty())
                pathOrUrlEdit->setText(f);
        });
        fileRow->addWidget(pathOrUrlEdit, 1);
        fileRow->addWidget(btnBrowse);
        iv->addLayout(fileRow);

        QHBoxLayout *sizeRow = new QHBoxLayout();
        QLabel *szLbl = new QLabel(MailI18n::t("표시 너비(Width):", "Display Width:", "表示幅:"), &imgDlg);
        szLbl->setStyleSheet("font-size: 12px; font-weight: 600; color: #374151;");
        QComboBox *widthCombo = new QComboBox(&imgDlg);
        widthCombo->addItem(MailI18n::t("140px (소형 아이콘/직인)", "140px (Small Icon/Stamp)", "140px (小アイコン)"), 140);
        widthCombo->addItem(MailI18n::t("220px (표준 회사 로고 — 권장)", "220px (Standard Logo — Recommended)", "220px (標準ロゴ・推奨)"), 220);
        widthCombo->addItem(MailI18n::t("320px (중형 명함/배너)", "320px (Medium Banner)", "320px (中型バナー)"), 320);
        widthCombo->addItem(MailI18n::t("480px (와이드 서명 배너)", "480px (Wide Banner)", "480px (ワイドバナー)"), 480);
        widthCombo->addItem(MailI18n::t("원본 크기 유지 (최대 600px)", "Original Size (Max 600px)", "元のサイズ (最大600px)"), 0);
        widthCombo->setCurrentIndex(1);
        widthCombo->setStyleSheet("background: #FFFFFF; border: 1px solid #D1D5DB; border-radius: 5px; padding: 5px 10px; font-size: 12px;");
        sizeRow->addWidget(szLbl);
        sizeRow->addWidget(widthCombo, 1);
        iv->addLayout(sizeRow);

        QHBoxLayout *btnRow = new QHBoxLayout();
        btnRow->addStretch();
        QPushButton *btnCancel = new QPushButton(MailI18n::t("취소", "Cancel", "キャンセル"), &imgDlg);
        btnCancel->setStyleSheet("background: #F3F4F6; color: #374151; border: 1px solid #D1D5DB; border-radius: 5px; padding: 6px 14px;");
        QPushButton *btnOk = new QPushButton(MailI18n::t("✓ 서명에 삽입", "✓ Insert Image", "✓ 署名に挿入"), &imgDlg);
        btnOk->setStyleSheet("background: #0D47A1; color: #FFFFFF; font-weight: 700; border: none; border-radius: 5px; padding: 6px 16px;");
        connect(btnCancel, &QPushButton::clicked, &imgDlg, &QDialog::reject);
        connect(btnOk, &QPushButton::clicked, &imgDlg, &QDialog::accept);
        btnRow->addWidget(btnCancel);
        btnRow->addWidget(btnOk);
        iv->addLayout(btnRow);

        if (imgDlg.exec() != QDialog::Accepted)
            return;

        const QString srcInput = pathOrUrlEdit->text().trimmed();
        if (srcInput.isEmpty())
            return;
        const int targetWidth = widthCombo->currentData().toInt();

        QString imgTagHtml;
        if (srcInput.startsWith("http://", Qt::CaseInsensitive) || srcInput.startsWith("https://", Qt::CaseInsensitive)) {
            QString safeUrl = srcInput;
            if (safeUrl.startsWith("http://", Qt::CaseInsensitive))
                safeUrl = "https://" + safeUrl.mid(7);
            const QString wAttr = (targetWidth > 0) ? QString(" width=\"%1\"").arg(targetWidth) : QString();
            imgTagHtml = QString("<img src=\"%1\"%2 style=\"max-width:100%;height:auto;border:0;display:inline-block;\" />")
                             .arg(safeUrl.toHtmlEscaped(), wAttr);
        } else {
            QImage img(srcInput);
            if (img.isNull()) {
                QMessageBox::warning(this,
                                     MailI18n::t("이미지 로드 실패", "Image Load Failed", "画像の読み込み失敗"),
                                     MailI18n::t("선택한 이미지 파일을 읽을 수 없습니다.", "Could not load the selected image file.", "選択した画像ファイルを読み込めませんでした。"));
                return;
            }
            const int maxW = (targetWidth > 0) ? targetWidth : qMin(img.width(), 600);
            if (img.width() > maxW) {
                img = img.scaledToWidth(maxW, Qt::SmoothTransformation);
            }
            QByteArray pngBytes;
            QBuffer buf(&pngBytes);
            buf.open(QIODevice::WriteOnly);
            img.save(&buf, "PNG");
            const QString dataUri = QString("data:image/png;base64,%1").arg(QString::fromLatin1(pngBytes.toBase64()));
            sigEditor->document()->addResource(QTextDocument::ImageResource, QUrl(dataUri), img);
            imgTagHtml = QString("<img src=\"%1\" width=\"%2\" height=\"%3\" style=\"max-width:100%;height:auto;border:0;display:inline-block;\" />")
                             .arg(dataUri)
                             .arg(img.width())
                             .arg(img.height());
        }

        if (htmlCodeEditor->isVisible()) {
            htmlCodeEditor->insertPlainText(imgTagHtml);
        } else {
            sigEditor->textCursor().insertHtml(imgTagHtml);
            registerInlineDataImages(sigEditor->document(), sigEditor->toHtml());
        }
    });

    QToolButton *btnHtmlMode = new QToolButton(page);
    btnHtmlMode->setCheckable(true);
    btnHtmlMode->setCursor(Qt::PointingHandCursor);
    btnHtmlMode->setText(MailI18n::t("</> HTML 코드 편집", "</> Edit HTML Source", "</> HTMLソース編集"));
    btnHtmlMode->setStyleSheet(toolBtnStyle);
    connect(btnHtmlMode, &QToolButton::toggled, this,
            [sigEditor, htmlCodeEditor, registerInlineDataImages, btnHtmlMode](bool codeMode) {
        if (codeMode) {
            htmlCodeEditor->setPlainText(sigEditor->toHtml());
            sigEditor->setVisible(false);
            htmlCodeEditor->setVisible(true);
            btnHtmlMode->setText(MailI18n::t("👁️ 시각적 편집/미리보기", "👁️ Visual Preview", "👁️ ビジュアルプレビュー"));
        } else {
            const QString rawHtml = htmlCodeEditor->toPlainText();
            registerInlineDataImages(sigEditor->document(), rawHtml);
            sigEditor->setHtml(rawHtml);
            htmlCodeEditor->setVisible(false);
            sigEditor->setVisible(true);
            btnHtmlMode->setText(MailI18n::t("</> HTML 코드 편집", "</> Edit HTML Source", "</> HTMLソース編集"));
        }
    });

    fmtRow->addWidget(fontCombo);
    fmtRow->addWidget(sizeCombo);
    fmtRow->addWidget(btnBold);
    fmtRow->addWidget(btnItalic);
    fmtRow->addWidget(btnUnder);
    fmtRow->addWidget(btnColor);
    fmtRow->addWidget(btnLink);
    fmtRow->addWidget(btnInsertImg);
    fmtRow->addWidget(btnHtmlMode);
    fmtRow->addStretch();
    v->addLayout(fmtRow);

    setSigEditorHtmlWithImages(MailI18n::defaultSignatureHtml());
    v->addWidget(sigEditor);
    v->addWidget(htmlCodeEditor);

    auto currentSignatureHtmlFromEditors = [sigEditor, htmlCodeEditor, registerInlineDataImages]() -> QString {
        if (htmlCodeEditor->isVisible()) {
            const QString raw = htmlCodeEditor->toPlainText();
            registerInlineDataImages(sigEditor->document(), raw);
            sigEditor->setHtml(raw);
            return raw;
        }
        return sigEditor->toHtml();
    };

    connect(accCombo, &QComboBox::currentIndexChanged, this, [this, accCombo, setSigEditorHtmlWithImages]() {
        int accIdx = accCombo->currentData().toInt();
        if (accIdx < 0 || !m_accounts || accIdx >= m_accounts->size()) {
            setSigEditorHtmlWithImages(MailI18n::defaultSignatureHtml());
        } else {
            const AccountItem &acc = m_accounts->at(accIdx);
            if (!acc.signatureHtml.trimmed().isEmpty()) {
                setSigEditorHtmlWithImages(acc.signatureHtml);
            } else {
                setSigEditorHtmlWithImages(QString(
                    "<br/><div style='color:#4B5563;font-size:12px;border-top:1px solid #E5E7EB;padding-top:8px;'>"
                    "<b>%1</b><br/><span style='color:#6B7280;'>%2</span></div>")
                    .arg(acc.displayName.toHtmlEscaped(), acc.email.toHtmlEscaped()));
            }
        }
    });

    QPushButton *btnSaveSig = new QPushButton(MailI18n::t("💾 서명 저장 (Save Signature)", "💾 Save Signature", "💾 署名を保存"), page);
    btnSaveSig->setCursor(Qt::PointingHandCursor);
    btnSaveSig->setMaximumWidth(220);
    btnSaveSig->setStyleSheet("background: #0D47A1; color: #FFFFFF; font-weight: 700; border: none; border-radius: 6px; padding: 9px 18px;");
    connect(btnSaveSig, &QPushButton::clicked, this, [this, accCombo, currentSignatureHtmlFromEditors]() {
        const QString finalHtml = currentSignatureHtmlFromEditors();
        int accIdx = accCombo->currentData().toInt();
        if (accIdx < 0 || !m_accounts || accIdx >= m_accounts->size()) {
            MailI18n::defaultSignatureHtml() = finalHtml;
        } else {
            (*m_accounts)[accIdx].signatureHtml = finalHtml;
        }
        emit accountsUpdated();
        QMessageBox::information(this, MailI18n::t("서명 저장 완료", "Signature Saved", "署名を保存しました"),
                                 MailI18n::t("서명이 저장되었습니다. 새 메일 작성 시 자동으로 반영됩니다.", "Signature saved and ready for Compose.", "署名が保存されました。"));
    });
    v->addWidget(btnSaveSig);

    v->addStretch();
    scroll->setWidget(page);
    return scroll;
}

QWidget *SettingsDialog::createSnippetsPage()
{
    QScrollArea *scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setStyleSheet("background: #FFFFFF;");

    QWidget *page = new QWidget(scroll);
    QVBoxLayout *v = new QVBoxLayout(page);
    v->setContentsMargins(36, 26, 36, 26);
    v->setSpacing(14);

    QLabel *t = new QLabel(MailI18n::t("상용구 관리 (Snippets — 빠른 문구 템플릿)", "Snippets (Quick Text Templates)", "定型文管理 (Snippets)"), page);
    t->setStyleSheet("font-size: 20px; font-weight: 600; color: #374151;");
    v->addWidget(t);

    QLabel *s = new QLabel(
        MailI18n::t("자주 사용하는 인사말·안내 문구를 상용구(Snippet)로 등록해 두면, 편지 쓰기 창의 [📋 상용구 삽입] 버튼에서 클릭 한 번으로 본문에 넣을 수 있습니다.",
                    "Save frequently used responses as snippets and insert them with 1 click from the Compose toolbar.",
                    "よく使う挨拶や案内文を定型文として登録し、メール作成画面の[📋 定型文挿入]からワンクリックで挿入できます。"),
        page);
    s->setWordWrap(true);
    s->setStyleSheet("font-size: 12px; color: #6B7280;");
    v->addWidget(s);

    QListWidget *snipList = new QListWidget(page);
    snipList->setMaximumWidth(720);
    snipList->setFixedHeight(160);
    snipList->setStyleSheet(
        "QListWidget { border: 1px solid #D1D5DB; border-radius: 8px; padding: 4px; background: #FFFFFF; }"
        "QListWidget::item { padding: 7px 10px; border-radius: 5px; font-size: 12.5px; color: #111827; }"
        "QListWidget::item:selected { background: #EFF6FF; color: #0D47A1; font-weight: 700; }");

    auto reloadSnippets = [snipList]() {
        snipList->clear();
        const QList<MailSnippetItem> &snips = MailI18n::snippets();
        for (int i = 0; i < snips.size(); ++i) {
            QListWidgetItem *it = new QListWidgetItem(
                QString("📋 %1   (%2)").arg(snips[i].title, snips[i].shortcutKey),
                snipList);
            it->setData(Qt::UserRole, i);
        }
        if (snipList->count() > 0)
            snipList->setCurrentRow(0);
    };
    reloadSnippets();
    v->addWidget(snipList);

    QGroupBox *editBox = new QGroupBox(MailI18n::t("상용구 추가 / 편집 (Add or Update Snippet)", "Add or Update Snippet", "定型文の追加・編集"), page);
    editBox->setMaximumWidth(720);
    editBox->setStyleSheet("QGroupBox { font-size: 13px; font-weight: 700; color: #111827; border: 1px solid #E5E7EB; border-radius: 8px; padding: 14px; background: #F9FAFB; }");
    QVBoxLayout *ev = new QVBoxLayout(editBox);
    ev->setSpacing(8);

    QHBoxLayout *topIn = new QHBoxLayout();
    QLineEdit *titleEdit = new QLineEdit(editBox);
    titleEdit->setPlaceholderText(MailI18n::t("상용구 제목 (예: 견적서 송부 안내)", "Snippet title", "定型文タイトル"));
    titleEdit->setStyleSheet("background: #FFFFFF; border: 1px solid #D1D5DB; border-radius: 5px; padding: 6px 10px;");

    QLineEdit *keyEdit = new QLineEdit(editBox);
    keyEdit->setPlaceholderText(MailI18n::t("단축 명령어 (예: /quote)", "Shortcut (e.g. /quote)", "ショートカット (例: /quote)"));
    keyEdit->setFixedWidth(180);
    keyEdit->setStyleSheet("background: #FFFFFF; border: 1px solid #D1D5DB; border-radius: 5px; padding: 6px 10px;");
    topIn->addWidget(titleEdit, 1);
    topIn->addWidget(keyEdit);
    ev->addLayout(topIn);

    QTextEdit *bodyEdit = new QTextEdit(editBox);
    bodyEdit->setFixedHeight(110);
    bodyEdit->setPlaceholderText(MailI18n::t("삽입될 상용구 본문 내용을 입력하세요...", "Snippet body text...", "定型文の本文を入力..."));
    bodyEdit->setStyleSheet("background: #FFFFFF; border: 1px solid #D1D5DB; border-radius: 5px; padding: 8px;");
    ev->addWidget(bodyEdit);

    auto setSnippetBodyHtmlWithImages = [bodyEdit](const QString &html) {
        if (!bodyEdit || !bodyEdit->document())
            return;
        if (!html.isEmpty()) {
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
                    bodyEdit->document()->addResource(QTextDocument::ImageResource, QUrl(fullDataUri), img);
                }
            }
        }
        bodyEdit->setHtml(html);
    };

    connect(snipList, &QListWidget::currentRowChanged, this, [snipList, titleEdit, keyEdit, setSnippetBodyHtmlWithImages](int row) {
        if (row < 0 || !snipList->item(row))
            return;
        int idx = snipList->item(row)->data(Qt::UserRole).toInt();
        if (idx >= 0 && idx < MailI18n::snippets().size()) {
            const MailSnippetItem &sn = MailI18n::snippets().at(idx);
            titleEdit->setText(sn.title);
            keyEdit->setText(sn.shortcutKey);
            setSnippetBodyHtmlWithImages(sn.bodyHtml);
        }
    });
    if (!MailI18n::snippets().isEmpty()) {
        titleEdit->setText(MailI18n::snippets().first().title);
        keyEdit->setText(MailI18n::snippets().first().shortcutKey);
        setSnippetBodyHtmlWithImages(MailI18n::snippets().first().bodyHtml);
    }

    QHBoxLayout *btnRow = new QHBoxLayout();
    QPushButton *btnAddSnip = new QPushButton(MailI18n::t("+ 새 상용구로 추가", "+ Add as New Snippet", "+ 新規定型文として追加"), editBox);
    btnAddSnip->setCursor(Qt::PointingHandCursor);
    btnAddSnip->setStyleSheet("background: #0D47A1; color: #FFFFFF; font-weight: 700; border: none; border-radius: 5px; padding: 7px 14px;");
    connect(btnAddSnip, &QPushButton::clicked, this, [this, titleEdit, keyEdit, bodyEdit, reloadSnippets]() {
        if (titleEdit->text().trimmed().isEmpty())
            return;
        MailSnippetItem sn;
        sn.id = "snip_" + QUuid::createUuid().toString(QUuid::Id128).left(6);
        sn.title = titleEdit->text().trimmed();
        sn.shortcutKey = keyEdit->text().trimmed().isEmpty() ? "/snip" : keyEdit->text().trimmed();
        sn.bodyHtml = bodyEdit->toHtml();
        MailI18n::snippets().append(sn);
        reloadSnippets();
        emit accountsUpdated();
    });

    QPushButton *btnUpdateSnip = new QPushButton(MailI18n::t("💾 선택 상용구 수정 저장", "💾 Update Selected", "💾 選択した定型文を更新"), editBox);
    btnUpdateSnip->setCursor(Qt::PointingHandCursor);
    btnUpdateSnip->setStyleSheet("background: #F3F4F6; color: #111827; font-weight: 600; border: 1px solid #D1D5DB; border-radius: 5px; padding: 7px 14px;");
    connect(btnUpdateSnip, &QPushButton::clicked, this, [this, snipList, titleEdit, keyEdit, bodyEdit, reloadSnippets]() {
        if (!snipList->currentItem())
            return;
        int idx = snipList->currentItem()->data(Qt::UserRole).toInt();
        if (idx >= 0 && idx < MailI18n::snippets().size()) {
            MailI18n::snippets()[idx].title = titleEdit->text().trimmed();
            MailI18n::snippets()[idx].shortcutKey = keyEdit->text().trimmed();
            MailI18n::snippets()[idx].bodyHtml = bodyEdit->toHtml();
            reloadSnippets();
            emit accountsUpdated();
        }
    });

    QPushButton *btnDelSnip = new QPushButton(MailI18n::t("🗑️ 선택 상용구 삭제", "🗑️ Delete Snippet", "🗑️ 削除"), editBox);
    btnDelSnip->setCursor(Qt::PointingHandCursor);
    btnDelSnip->setStyleSheet("background: #FEF2F2; color: #DC2626; font-weight: 700; border: 1px solid #FECACA; border-radius: 5px; padding: 7px 14px;");
    connect(btnDelSnip, &QPushButton::clicked, this, [this, snipList, reloadSnippets]() {
        if (!snipList->currentItem())
            return;
        int idx = snipList->currentItem()->data(Qt::UserRole).toInt();
        if (idx >= 0 && idx < MailI18n::snippets().size()) {
            MailI18n::snippets().removeAt(idx);
            reloadSnippets();
            emit accountsUpdated();
        }
    });

    btnRow->addWidget(btnAddSnip);
    btnRow->addWidget(btnUpdateSnip);
    btnRow->addWidget(btnDelSnip);
    btnRow->addStretch();
    ev->addLayout(btnRow);

    v->addWidget(editBox);
    v->addStretch();
    scroll->setWidget(page);
    return scroll;
}

QWidget *SettingsDialog::createShortcutsPage()
{
    QScrollArea *scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setStyleSheet("background: #FFFFFF;");

    QWidget *page = new QWidget(scroll);
    QVBoxLayout *v = new QVBoxLayout(page);
    v->setContentsMargins(36, 26, 36, 26);
    v->setSpacing(14);

    QLabel *t = new QLabel(MailI18n::t("단축키 설정 (Keyboard Shortcuts)", "Keyboard Shortcuts", "キーボードショートカット"), page);
    t->setStyleSheet("font-size: 20px; font-weight: 600; color: #374151;");
    v->addWidget(t);

    QCheckBox *chkKeys = new QCheckBox(
        MailI18n::t("빠른 키보드 단축키 활성화 (Ctrl+N, Ctrl+R, Delete, F5 등)",
                    "Enable keyboard shortcuts",
                    "キーボードショートカットを有効にする"),
        page);
    chkKeys->setChecked(MailI18n::enableSingleKeyShortcuts());
    chkKeys->setStyleSheet("font-size: 13px; font-weight: 700; color: #111827;");
    connect(chkKeys, &QCheckBox::toggled, this, [](bool on) {
        MailI18n::enableSingleKeyShortcuts() = on;
    });
    v->addWidget(chkKeys);

    const QList<QPair<QString, QString>> keys = {
        {MailI18n::t("새 이메일 작성 (Compose new email)", "Compose new email", "新規メール作成"), "Ctrl + N"},
        {MailI18n::t("보낸사람에게 답장 (Reply to sender)", "Reply to sender", "返信"), "Ctrl + R"},
        {MailI18n::t("전체 동기화 / 새로고침 (Sync & Refresh)", "Sync & Refresh", "同期・更新"), "F5"},
        {MailI18n::t("선택 메일 휴지통 이동 / 영구 삭제 (Delete)", "Move to Trash / Delete", "ゴミ箱へ移動 / 削除"), "Delete"},
        {MailI18n::t("보관함으로 이동 (Archive conversation)", "Archive conversation", "アーカイブへ移動"), "E"},
        {MailI18n::t("폴더 패널 접기/펼치기 (Toggle Minimal Layout)", "Toggle Minimal Layout", "ミニマルレイアウト切替"), "Ctrl + Shift + F"}
    };

    QTableWidget *tbl = new QTableWidget(keys.size(), 2, page);
    tbl->setMaximumWidth(680);
    tbl->setFixedHeight(250);
    tbl->setHorizontalHeaderLabels({MailI18n::t("동작 (Action)", "Action", "機能"), MailI18n::t("단축키 (Shortcut)", "Shortcut", "ショートカット")});
    tbl->horizontalHeader()->setStretchLastSection(true);
    tbl->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    tbl->verticalHeader()->setVisible(false);
    tbl->setEditTriggers(QAbstractItemView::NoEditTriggers);
    tbl->setStyleSheet("QTableWidget { border: 1px solid #D1D5DB; border-radius: 6px; background: #FFFFFF; font-size: 12.5px; }");
    for (int r = 0; r < keys.size(); ++r) {
        tbl->setItem(r, 0, new QTableWidgetItem(keys[r].first));
        QTableWidgetItem *kItem = new QTableWidgetItem(keys[r].second);
        kItem->setFont(QFont("Consolas", 10, QFont::Bold));
        tbl->setItem(r, 1, kItem);
    }
    v->addWidget(tbl);

    v->addStretch();
    scroll->setWidget(page);
    return scroll;
}

QWidget *SettingsDialog::createAdvancedPage()
{
    QScrollArea *scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setStyleSheet("background: #FFFFFF;");

    QWidget *page = new QWidget(scroll);
    QVBoxLayout *v = new QVBoxLayout(page);
    v->setContentsMargins(36, 26, 36, 26);
    v->setSpacing(16);

    QHBoxLayout *hdrRow = new QHBoxLayout();
    hdrRow->setSpacing(8);
    QLabel *hdrIcon = new QLabel(page);
    hdrIcon->setPixmap(MailIcons::settingsNavIcon(11, QColor("#0D47A1"), 20).pixmap(20, 20));
    QLabel *t = new QLabel(MailI18n::t("고급 설정 (Advanced — IMAP 엔진 · 스크롤 페이징 · 로컬 캐시 관리)", "Advanced Engine & Cache Settings", "詳細設定 (Advanced)"), page);
    t->setStyleSheet("font-size: 20px; font-weight: 600; color: #374151;");
    hdrRow->addWidget(hdrIcon);
    hdrRow->addWidget(t);
    hdrRow->addStretch();
    v->addLayout(hdrRow);

    QGroupBox *perfBox = new QGroupBox(MailI18n::t("IMAP 네트워크 엔진 및 스크롤 페이지네이션 설정", "IMAP Network Engine && Scroll Pagination", "IMAPエンジンとスクロールページング設定"), page);
    perfBox->setMaximumWidth(720);
    perfBox->setStyleSheet("QGroupBox { font-size: 13px; font-weight: 700; color: #111827; border: 1px solid #E5E7EB; border-radius: 8px; padding: 16px; background: #F9FAFB; }");
    QFormLayout *form = new QFormLayout(perfBox);
    form->setSpacing(12);

    QComboBox *advSyncCombo = new QComboBox(perfBox);
    advSyncCombo->addItem(MailI18n::t("⚡ 실시간 푸시(IDLE) + 30초마다 초경량 확인 (권장 — 30초)", "⚡ Real-time Push (IDLE) + Every 30s (Recommended)", "⚡ リアルタイムPush (IDLE) + 30秒ごと（推奨）"), 30);
    advSyncCombo->addItem(MailI18n::t("1분마다 확인 (60초)", "Every 1 minute", "1分ごと"), 60);
    advSyncCombo->addItem(MailI18n::t("3분마다 확인 (180초)", "Every 3 minutes", "3分ごと"), 180);
    advSyncCombo->addItem(MailI18n::t("5분마다 확인 (300초)", "Every 5 minutes", "5分ごと"), 300);
    advSyncCombo->addItem(MailI18n::t("10분마다 확인 (600초)", "Every 10 minutes", "10分ごと"), 600);
    advSyncCombo->addItem(MailI18n::t("30분마다 확인 (1800초)", "Every 30 minutes", "30分ごと"), 1800);
    advSyncCombo->addItem(MailI18n::t("수동 확인 (자동 동기화 안 함)", "Manual only (Disabled)", "手動確認のみ"), 0);
    int asIdx = advSyncCombo->findData(MailI18n::autoSyncIntervalSec());
    if (asIdx >= 0)
        advSyncCombo->setCurrentIndex(asIdx);
    advSyncCombo->setStyleSheet("background: #FFFFFF; border: 1px solid #D1D5DB; border-radius: 5px; padding: 6px 10px;");
    connect(advSyncCombo, &QComboBox::currentIndexChanged, this, [this, advSyncCombo]() {
        int sec = advSyncCombo->currentData().toInt();
        MailI18n::autoSyncIntervalSec() = sec;
        emit autoSyncIntervalChanged(sec);
        emit privacySettingsChanged();
    });

    QComboBox *batchCombo = new QComboBox(perfBox);
    batchCombo->addItem(MailI18n::t("20건씩 로드 (권장 — 20 messages per page)", "20 messages per page (Recommended)", "20件ずつ読込（推奨）"), 20);
    batchCombo->addItem(MailI18n::t("50건씩 로드 (50 messages per page)", "50 messages per page", "50件ずつ読込"), 50);
    batchCombo->addItem(MailI18n::t("100건씩 로드 (100 messages per page)", "100 messages per page", "100件ずつ読込"), 100);
    batchCombo->addItem(MailI18n::t("200건씩 로드 (200 messages per page)", "200 messages per page", "200件ずつ読込"), 200);
    batchCombo->addItem(MailI18n::t("500건씩 로드 (500 messages per page)", "500 messages per page", "500件ずつ読込"), 500);
    int bIdx = batchCombo->findData(MailI18n::pageFetchSize());
    if (bIdx >= 0)
        batchCombo->setCurrentIndex(bIdx);
    else
        batchCombo->setCurrentIndex(0);
    batchCombo->setStyleSheet("background: #FFFFFF; border: 1px solid #D1D5DB; border-radius: 5px; padding: 6px 10px;");
    connect(batchCombo, &QComboBox::currentIndexChanged, this, [batchCombo]() {
        MailI18n::pageFetchSize() = batchCombo->currentData().toInt();
    });

    QComboBox *sliceCombo = new QComboBox(perfBox);
    sliceCombo->addItem(MailI18n::t("16 KB 슬라이스 (초고속 목록 조회)", "16 KB Slice (Fastest list preview)", "16 KB スライス（超高速一覧取得）"), 16384);
    sliceCombo->addItem(MailI18n::t("32 KB 슬라이스 (본문 미리보기 + 고속 조회)", "32 KB Slice", "32 KB スライス"), 32768);
    sliceCombo->addItem(MailI18n::t("64 KB 슬라이스 (권장 — 제목+본문 동시 로드)", "64 KB Slice (Recommended)", "64 KB スライス（推奨）"), 65536);
    sliceCombo->addItem(MailI18n::t("전체 본문 다운로드 (느림)", "Full Body Download (Slower)", "全文ダウンロード（低速）"), 0);
    int sIdx = sliceCombo->findData(MailI18n::imapSliceBytes());
    if (sIdx >= 0)
        sliceCombo->setCurrentIndex(sIdx);
    sliceCombo->setStyleSheet("background: #FFFFFF; border: 1px solid #D1D5DB; border-radius: 5px; padding: 6px 10px;");
    connect(sliceCombo, &QComboBox::currentIndexChanged, this, [sliceCombo]() {
        MailI18n::imapSliceBytes() = sliceCombo->currentData().toInt();
    });

    form->addRow(MailI18n::t("새 메일 자동 확인 주기:", "Auto-Check Interval:", "新着メール自動確認間隔:"), advSyncCombo);
    form->addRow(MailI18n::t("스크롤 추가 로드 단위:", "Scroll Fetch Batch Size:", "スクロール読込単位:"), batchCombo);
    form->addRow(MailI18n::t("IMAP 본문 프리페치 크기:", "IMAP Body Slice Size:", "IMAPプリフェッチ容量:"), sliceCombo);
    v->addWidget(perfBox);

    // Local Cache Diagnostics & Cleanup
    QGroupBox *cacheBox = new QGroupBox(MailI18n::t("로컬 디스크 캐시 상태 및 초기화 (Local Cache Management)", "Local Disk Cache && Storage", "ローカルキャッシュ管理"), page);
    cacheBox->setMaximumWidth(720);
    cacheBox->setStyleSheet("QGroupBox { font-size: 13px; font-weight: 700; color: #111827; border: 1px solid #E5E7EB; border-radius: 8px; padding: 16px; background: #FFFFFF; }");
    QVBoxLayout *cv = new QVBoxLayout(cacheBox);
    cv->setSpacing(10);

    auto resolveStorageRoot = []() -> QString {
        const QString appDir = QCoreApplication::applicationDirPath();
        const QString lowerDir = appDir.toLower();
        bool canWrite = !lowerDir.contains(QStringLiteral("program files"))
                        && !lowerDir.contains(QStringLiteral(".app/contents/"))
                        && !lowerDir.startsWith(QStringLiteral("/opt/sollunamail"))
                        && !lowerDir.startsWith(QStringLiteral("/usr/"));
        if (canWrite) {
            const QString testDir = appDir + QStringLiteral("/config_set");
            if (QDir().mkpath(testDir)) {
                QFile probe(testDir + QStringLiteral("/.write_probe"));
                if (probe.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
                    probe.write("ok");
                    probe.close();
                    probe.remove();
                    return appDir;
                }
            }
        }
#ifdef Q_OS_WIN
        const QString appData = qEnvironmentVariable("APPDATA").trimmed();
        if (!appData.isEmpty())
            return QDir::fromNativeSeparators(appData) + QStringLiteral("/SOLLUNA_MAIL");
#endif
        return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    };

    auto formatCacheFileSize = [resolveStorageRoot](const QString &relPath) -> QString {
        QFileInfo fi(resolveStorageRoot() + "/" + relPath);
        if (!fi.exists())
            return MailI18n::t("0 KB (미생성)", "0 KB (Not created)", "0 KB (未作成)");
        return QString("%1 KB (%2)").arg(qMax<qint64>(1, fi.size() / 1024)).arg(fi.lastModified().toString("yyyy-MM-dd HH:mm"));
    };

    auto formatConfigFilesSize = [resolveStorageRoot]() -> QString {
        const QString rootDir = resolveStorageRoot();
        QFileInfo fiAcc(rootDir + "/config_set/accounts.json");
        QFileInfo fiCfg(rootDir + "/config_set/app_settings.json");
        if (!fiAcc.exists() && !fiCfg.exists())
            return MailI18n::t("0 KB (미생성)", "0 KB (Not created)", "0 KB (未作成)");
        const qint64 totalBytes = (fiAcc.exists() ? fiAcc.size() : 0) + (fiCfg.exists() ? fiCfg.size() : 0);
        const QDateTime latestDt = (fiAcc.exists() && (!fiCfg.exists() || fiAcc.lastModified() >= fiCfg.lastModified()))
                                       ? fiAcc.lastModified()
                                       : fiCfg.lastModified();
        return QString("%1 KB (%2)").arg(qMax<qint64>(1, totalBytes / 1024)).arg(latestDt.toString("yyyy-MM-dd HH:mm"));
    };

    QLabel *cacheInfoLbl = new QLabel(cacheBox);
    cacheInfoLbl->setStyleSheet("font-size: 12px; color: #374151; background: #F9FAFB; border: 1px solid #E5E7EB; border-radius: 6px; padding: 10px;");
    auto updateCacheInfo = [cacheInfoLbl, formatCacheFileSize, formatConfigFilesSize]() {
        cacheInfoLbl->setText(
            MailI18n::t(
                "📦 <b>cache/conversations_cache.json (메일 캐시):</b> %1<br/>"
                "📅 <b>cache/calendars_cache.json (캘린더 캐시):</b> %2<br/>"
                "👥 <b>cache/contacts_cache.json (연락처 캐시):</b> %3<br/>"
                "🔑 <b>config_set/accounts.json / app_settings.json (설정):</b> %4",
                "📦 <b>cache/conversations_cache.json (Mail Cache):</b> %1<br/>"
                "📅 <b>cache/calendars_cache.json (Calendar Cache):</b> %2<br/>"
                "👥 <b>cache/contacts_cache.json (Contacts Cache):</b> %3<br/>"
                "🔑 <b>config_set/accounts.json / app_settings.json (Settings):</b> %4",
                "📦 <b>cache/conversations_cache.json (メールキャッシュ):</b> %1<br/>"
                "📅 <b>cache/calendars_cache.json (カレンダーキャッシュ):</b> %2<br/>"
                "👥 <b>cache/contacts_cache.json (連絡先キャッシュ):</b> %3<br/>"
                "🔑 <b>config_set/accounts.json / app_settings.json (設定):</b> %4")
            .arg(formatCacheFileSize("cache/conversations_cache.json"),
                 formatCacheFileSize("cache/calendars_cache.json"),
                 formatCacheFileSize("cache/contacts_cache.json"),
                 formatConfigFilesSize()));
    };
    updateCacheInfo();
    cv->addWidget(cacheInfoLbl);

    QHBoxLayout *cBtnRow = new QHBoxLayout();
    cBtnRow->setSpacing(10);
    QPushButton *btnClearMailCache = new QPushButton(MailI18n::t("🧹 메일 캐시 초기화 및 다시 받기 (Clear Mail Cache)", "🧹 Clear Mail Cache", "🧹 メールキャッシュを初期化"), cacheBox);
    btnClearMailCache->setCursor(Qt::PointingHandCursor);
    btnClearMailCache->setStyleSheet("background: #FEF2F2; color: #DC2626; font-weight: 700; border: 1px solid #FECACA; border-radius: 6px; padding: 8px 14px;");
    connect(btnClearMailCache, &QPushButton::clicked, this, [this, updateCacheInfo]() {
        if (QMessageBox::question(this,
                                  MailI18n::t("메일 캐시 초기화 확인", "Clear Mail Cache", "キャッシュ初期化の確認"),
                                  MailI18n::t("로컬 메일 캐시(cache/conversations_cache.json 및 첨부 캐시)를 초기화하고 서버에서 다시 동기화하시겠습니까?\n(서버의 원본 메일은 안전하게 보존됩니다.)",
                                              "Clear local mail cache (cache/conversations_cache.json) and resync from IMAP server?",
                                              "ローカルメールキャッシュを初期化しますか？"),
                                  QMessageBox::Yes | QMessageBox::No) == QMessageBox::Yes) {
            emit clearMailCacheRequested();
            updateCacheInfo();
            QMessageBox::information(this, MailI18n::t("완료", "Done", "完了"),
                                     MailI18n::t("로컬 메일 캐시를 초기화했습니다.", "Local mail cache cleared.", "キャッシュを初期化しました。"));
        }
    });

    QPushButton *btnOpenDir = new QPushButton(MailI18n::t("📂 캐시 폴더 열기 (cache)", "📂 Open Cache Folder", "📂 キャッシュフォルダを開く"), cacheBox);
    btnOpenDir->setCursor(Qt::PointingHandCursor);
    btnOpenDir->setStyleSheet("background: #F3F4F6; color: #1F2937; font-weight: 600; border: 1px solid #D1D5DB; border-radius: 6px; padding: 8px 14px;");
    connect(btnOpenDir, &QPushButton::clicked, this, [resolveStorageRoot]() {
        const QString cacheDir = resolveStorageRoot() + QStringLiteral("/cache");
        QDir().mkpath(cacheDir);
        QDesktopServices::openUrl(QUrl::fromLocalFile(cacheDir));
    });

    QPushButton *btnOpenCfgDir = new QPushButton(MailI18n::t("⚙️ 설정 폴더 열기 (config_set)", "⚙️ Open Config Folder", "⚙️ 設定フォルダを開く"), cacheBox);
    btnOpenCfgDir->setCursor(Qt::PointingHandCursor);
    btnOpenCfgDir->setStyleSheet("background: #EFF6FF; color: #1D4ED8; font-weight: 600; border: 1px solid #BFDBFE; border-radius: 6px; padding: 8px 14px;");
    connect(btnOpenCfgDir, &QPushButton::clicked, this, [resolveStorageRoot]() {
        const QString cfgDir = resolveStorageRoot() + QStringLiteral("/config_set");
        QDir().mkpath(cfgDir);
        QDesktopServices::openUrl(QUrl::fromLocalFile(cfgDir));
    });

    cBtnRow->addWidget(btnClearMailCache);
    cBtnRow->addWidget(btnOpenDir);
    cBtnRow->addWidget(btnOpenCfgDir);
    cBtnRow->addStretch();
    cv->addLayout(cBtnRow);

    v->addWidget(cacheBox);
    v->addStretch();
    scroll->setWidget(page);
    return scroll;
}

QWidget *SettingsDialog::createAiMcpPage()
{
    QScrollArea *scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setStyleSheet("background: #FFFFFF;");

    QWidget *page = new QWidget(scroll);
    QVBoxLayout *v = new QVBoxLayout(page);
    v->setContentsMargins(36, 26, 36, 26);
    v->setSpacing(16);

    QHBoxLayout *hdrRow = new QHBoxLayout();
    hdrRow->setSpacing(8);
    QLabel *hdrIcon = new QLabel(page);
    hdrIcon->setPixmap(MailIcons::settingsNavIcon(12, QColor("#0D47A1"), 20).pixmap(20, 20));
    QLabel *t = new QLabel(
        MailI18n::t("AI & MCP 통합 연동 (Google Gemini · OpenAI Codex 전용)",
                    "AI & MCP Integration (Google Gemini & OpenAI Codex)",
                    "AI & MCP 統合連携 (Google Gemini · OpenAI Codex 専用)"),
        page);
    t->setStyleSheet("font-size: 20px; font-weight: 700; color: #111827;");
    hdrRow->addWidget(hdrIcon);
    hdrRow->addWidget(t);
    hdrRow->addStretch();
    v->addLayout(hdrRow);

    QLabel *s = new QLabel(
        MailI18n::t("SollunaMail의 이메일 초안 작성·요약·한/영/일 비즈니스 번역에 사용할 AI 엔진(Google Gemini 또는 OpenAI Codex)을 설정하고,\n"
                    "Gemini CLI (~/.gemini/settings.json) 및 OpenAI Codex CLI (~/.codex/config.toml)용 MCP 서버 설정을 원클릭으로 생성·등록합니다.",
                    "Configure Google Gemini or OpenAI Codex for email drafting, summarization, and translation, and install MCP server settings for Gemini CLI and Codex CLI.",
                    "メール下書き・要約・翻訳に使用するAIエンジン(Google Gemini または OpenAI Codex)と、Gemini CLI / Codex CLI 用の MCP サーバー設定を管理します。"),
        page);
    s->setWordWrap(true);
    s->setStyleSheet("font-size: 12px; color: #4B5563;");
    v->addWidget(s);

    // 1. Engine Selector Cards (Google Gemini vs OpenAI Codex)
    QHBoxLayout *engRow = new QHBoxLayout();
    engRow->setSpacing(12);
    QPushButton *btnGeminiMode = new QPushButton(MailI18n::sanitizeLinuxEmojiIfMissing(QStringLiteral("✨ Google Gemini (Gemini 2.5 Flash / Pro + Gemini CLI)")), page);
    QPushButton *btnCodexMode  = new QPushButton(MailI18n::sanitizeLinuxEmojiIfMissing(QStringLiteral("🤖 OpenAI Codex (gpt-5-codex / o4-mini + Codex CLI)")), page);
    btnGeminiMode->setCursor(Qt::PointingHandCursor);
    btnCodexMode->setCursor(Qt::PointingHandCursor);
    btnGeminiMode->setFixedHeight(44);
    btnCodexMode->setFixedHeight(44);

    auto updateEngineBtns = [btnGeminiMode, btnCodexMode]() {
        bool isGemini = (MailI18n::aiProvider().compare("Codex", Qt::CaseInsensitive) != 0);
        auto styleFor = [](bool active) {
            return active
                ? QStringLiteral("QPushButton { background: #EFF6FF; color: #0D47A1; font-size: 13px; font-weight: 700; border: 2px solid #0D47A1; border-radius: 8px; padding: 8px 18px; }")
                : QStringLiteral("QPushButton { background: #F9FAFB; color: #374151; font-size: 13px; font-weight: 600; border: 1px solid #D1D5DB; border-radius: 8px; padding: 8px 18px; }"
                                 "QPushButton:hover { background: #F3F4F6; }");
        };
        btnGeminiMode->setStyleSheet(styleFor(isGemini));
        btnCodexMode->setStyleSheet(styleFor(!isGemini));
    };
    updateEngineBtns();
    engRow->addWidget(btnGeminiMode);
    engRow->addWidget(btnCodexMode);
    engRow->addStretch();
    v->addLayout(engRow);

    // 2. Gemini & Codex API / Model Configuration Box
    QGroupBox *apiBox = new QGroupBox(MailI18n::t("1. Gemini / Codex API 키 및 모델 설정", "1. Gemini && Codex API Key / Model Settings", "1. Gemini / Codex APIキー・モデル設定"), page);
    apiBox->setMaximumWidth(760);
    apiBox->setStyleSheet("QGroupBox { font-size: 13px; font-weight: 700; color: #111827; border: 1px solid #E5E7EB; border-radius: 8px; padding: 16px; background: #F9FAFB; }");
    QFormLayout *apiForm = new QFormLayout(apiBox);
    apiForm->setSpacing(10);

    QComboBox *geminiModelCombo = new QComboBox(apiBox);
    geminiModelCombo->addItems({"gemini-2.5-flash", "gemini-2.5-pro", "gemini-2.0-flash"});
    geminiModelCombo->setCurrentText(MailI18n::geminiModel());
    geminiModelCombo->setStyleSheet("background: #FFFFFF; border: 1px solid #D1D5DB; border-radius: 5px; padding: 6px 10px;");
    connect(geminiModelCombo, &QComboBox::currentTextChanged, this, [](const QString &m) {
        MailI18n::geminiModel() = m;
    });

    QLineEdit *geminiKeyEdit = new QLineEdit(MailI18n::geminiApiKey(), apiBox);
    geminiKeyEdit->setEchoMode(QLineEdit::Password);
    geminiKeyEdit->setPlaceholderText(
        MailI18n::t("Google AI Studio (GEMINI_API_KEY) 입력 — 비워두면 로컬 gemini CLI 또는 내장 스마트 엔진 사용",
                    "Enter Google AI Studio API Key (GEMINI_API_KEY) — or leave blank to use local gemini CLI",
                    "Google AI Studio (GEMINI_API_KEY) を入力 — 空欄時はローカル gemini CLI を使用"));
    geminiKeyEdit->setStyleSheet("background: #FFFFFF; border: 1px solid #D1D5DB; border-radius: 5px; padding: 6px 10px;");
    connect(geminiKeyEdit, &QLineEdit::textChanged, this, [](const QString &k) {
        MailI18n::geminiApiKey() = k.trimmed();
    });

    QWidget *geminiKeyRowW = new QWidget(apiBox);
    QHBoxLayout *geminiKeyRowH = new QHBoxLayout(geminiKeyRowW);
    geminiKeyRowH->setContentsMargins(0, 0, 0, 0);
    geminiKeyRowH->setSpacing(8);
    QPushButton *geminiKeyVisBtn = createPasswordVisibilityToggleBtn(geminiKeyEdit, geminiKeyRowW);
    geminiKeyRowH->addWidget(geminiKeyEdit, 1);
    geminiKeyRowH->addWidget(geminiKeyVisBtn);

    QComboBox *codexModelCombo = new QComboBox(apiBox);
    codexModelCombo->addItems({"gpt-5-codex", "o4-mini", "gpt-4.1"});
    codexModelCombo->setCurrentText(MailI18n::codexModel());
    codexModelCombo->setStyleSheet("background: #FFFFFF; border: 1px solid #D1D5DB; border-radius: 5px; padding: 6px 10px;");
    connect(codexModelCombo, &QComboBox::currentTextChanged, this, [](const QString &m) {
        MailI18n::codexModel() = m;
    });

    QLineEdit *codexKeyEdit = new QLineEdit(MailI18n::codexApiKey(), apiBox);
    codexKeyEdit->setEchoMode(QLineEdit::Password);
    codexKeyEdit->setPlaceholderText(
        MailI18n::t("OpenAI API Key (OPENAI_API_KEY) 입력 — 비워두면 로컬 codex CLI 또는 내장 스마트 엔진 사용",
                    "Enter OpenAI API Key (OPENAI_API_KEY) — or leave blank to use local codex CLI",
                    "OpenAI API Key (OPENAI_API_KEY) を入力 — 空欄時はローカル codex CLI を使用"));
    codexKeyEdit->setStyleSheet("background: #FFFFFF; border: 1px solid #D1D5DB; border-radius: 5px; padding: 6px 10px;");
    connect(codexKeyEdit, &QLineEdit::textChanged, this, [](const QString &k) {
        MailI18n::codexApiKey() = k.trimmed();
    });

    QWidget *codexKeyRowW = new QWidget(apiBox);
    QHBoxLayout *codexKeyRowH = new QHBoxLayout(codexKeyRowW);
    codexKeyRowH->setContentsMargins(0, 0, 0, 0);
    codexKeyRowH->setSpacing(8);
    QPushButton *codexKeyVisBtn = createPasswordVisibilityToggleBtn(codexKeyEdit, codexKeyRowW);
    codexKeyRowH->addWidget(codexKeyEdit, 1);
    codexKeyRowH->addWidget(codexKeyVisBtn);

    apiForm->addRow(MailI18n::t("✨ Gemini 모델:", "✨ Gemini Model:", "✨ Gemini モデル:"), geminiModelCombo);
    apiForm->addRow(MailI18n::sanitizeLinuxEmojiIfMissing(QStringLiteral("🔑 Gemini API Key:")), geminiKeyRowW);
    apiForm->addRow(MailI18n::t("🤖 OpenAI Codex 모델:", "🤖 OpenAI Codex Model:", "🤖 OpenAI Codex モデル:"), codexModelCombo);
    apiForm->addRow(MailI18n::sanitizeLinuxEmojiIfMissing(QStringLiteral("🔑 OpenAI API Key:")), codexKeyRowW);
    v->addWidget(apiBox);

    connect(btnGeminiMode, &QPushButton::clicked, this, [updateEngineBtns]() {
        MailI18n::aiProvider() = QStringLiteral("Gemini");
        updateEngineBtns();
    });
    connect(btnCodexMode, &QPushButton::clicked, this, [updateEngineBtns]() {
        MailI18n::aiProvider() = QStringLiteral("Codex");
        updateEngineBtns();
    });

    // 3. MCP Server Configuration for Gemini CLI (~/.gemini/settings.json) & Codex CLI (~/.codex/config.toml)
    QGroupBox *mcpBox = new QGroupBox(
        MailI18n::t("2. Gemini CLI && OpenAI Codex CLI 전용 MCP (Model Context Protocol) 서버 브리지",
                    "2. MCP Server Bridge for Gemini CLI && OpenAI Codex CLI",
                    "2. Gemini CLI && OpenAI Codex CLI 専用 MCP サーバーブリッジ"),
        page);
    mcpBox->setMaximumWidth(760);
    mcpBox->setStyleSheet("QGroupBox { font-size: 13px; font-weight: 700; color: #111827; border: 1px solid #E5E7EB; border-radius: 8px; padding: 16px; background: #FFFFFF; }");
    QVBoxLayout *mv = new QVBoxLayout(mcpBox);
    mv->setSpacing(10);

    QLabel *mcpDesc = new QLabel(
        MailI18n::t("Gemini CLI (`gemini`) 또는 OpenAI Codex CLI (`codex`)에서 SollunaMail의 메일함(`cache/conversations_cache.json`), 캘린더(`cache/calendars_cache.json`), 주소록(`cache/contacts_cache.json`)을 MCP 도구로 직접 조회·요약·초안 작성할 수 있도록 설정을 생성합니다.",
                    "Generate MCP server configuration so Gemini CLI (`~/.gemini/settings.json`) and OpenAI Codex CLI (`~/.codex/config.toml`) can inspect and manage SollunaMail.",
                    "Gemini CLI (`~/.gemini/settings.json`) と OpenAI Codex CLI (`~/.codex/config.toml`) 用の MCP 設定を生成・適用します。"),
        mcpBox);
    mcpDesc->setWordWrap(true);
    mcpDesc->setStyleSheet("font-size: 11.5px; color: #4B5563;");
    mv->addWidget(mcpDesc);

    auto resolveMcpDataRoot = []() -> QString {
        const QString appDir = QDir::fromNativeSeparators(QCoreApplication::applicationDirPath());
        const QString lowerAppDir = appDir.toLower();
        const bool isSystemProtected = lowerAppDir.contains(QStringLiteral(".app/contents/"))
                                       || lowerAppDir.startsWith(QStringLiteral("/opt/sollunamail"))
                                       || lowerAppDir.startsWith(QStringLiteral("/usr/"))
                                       || lowerAppDir.contains(QStringLiteral("program files"));
        if (!isSystemProtected) {
            QFile probe(appDir + QStringLiteral("/.solluna_write_probe.tmp"));
            if (probe.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
                probe.close();
                probe.remove();
                return appDir;
            }
        }
        QString roamingBase = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation);
        if (roamingBase.isEmpty())
            roamingBase = QDir::homePath();
        const QString fallback = QDir::fromNativeSeparators(roamingBase + QStringLiteral("/SOLLUNA_MAIL"));
        QDir().mkpath(fallback);
        return fallback;
    };
    const QString dataDir = resolveMcpDataRoot();
    const QString exeFilePath = QDir::fromNativeSeparators(QCoreApplication::applicationFilePath());
    const QString geminiMcpJson = QString(
        "{\n"
        "  \"mcpServers\": {\n"
        "    \"sollunamail\": {\n"
        "      \"command\": \"%1\",\n"
        "      \"args\": [\"--mcp-stdio\"],\n"
        "      \"env\": {\n"
        "        \"SOLLUNAMAIL_DATA_DIR\": \"%2/cache\"\n"
        "      }\n"
        "    }\n"
        "  }\n"
        "}").arg(exeFilePath, dataDir);

    const QString codexMcpToml = QString(
        "[mcp_servers.sollunamail]\n"
        "command = \"%1\"\n"
        "args = [\"--mcp-stdio\"]\n"
        "env = { SOLLUNAMAIL_DATA_DIR = \"%2/cache\" }\n").arg(exeFilePath, dataDir);

    QTextEdit *mcpPreview = new QTextEdit(mcpBox);
    mcpPreview->setReadOnly(true);
    mcpPreview->setFixedHeight(150);
    mcpPreview->setFont(QFont("Consolas", 9));
    mcpPreview->setStyleSheet("background: #111827; color: #E5E7EB; border-radius: 6px; padding: 10px;");
    mcpPreview->setPlainText("# [Gemini CLI — ~/.gemini/settings.json]\n" + geminiMcpJson +
                             "\n\n# [OpenAI Codex CLI — ~/.codex/config.toml]\n" + codexMcpToml);
    mv->addWidget(mcpPreview);

    QHBoxLayout *mcpBtnRow = new QHBoxLayout();
    mcpBtnRow->setSpacing(8);

    QPushButton *btnCopyGeminiMcp = new QPushButton(MailI18n::t("📋 Gemini CLI MCP JSON 복사", "📋 Copy Gemini CLI JSON", "📋 Gemini CLI JSON コピー"), mcpBox);
    btnCopyGeminiMcp->setCursor(Qt::PointingHandCursor);
    btnCopyGeminiMcp->setStyleSheet("background: #EFF6FF; color: #0D47A1; font-weight: 700; border: 1px solid #BFDBFE; border-radius: 5px; padding: 7px 12px;");
    connect(btnCopyGeminiMcp, &QPushButton::clicked, this, [this, geminiMcpJson]() {
        if (QApplication::clipboard())
            QApplication::clipboard()->setText(geminiMcpJson);
        QMessageBox::information(
            this,
            MailI18n::t("Gemini MCP 복사 완료", "Gemini MCP Copied", "Gemini MCP コピー完了"),
            MailI18n::t("클립보드에 Gemini CLI용 `~/.gemini/settings.json` MCP 설정이 복사되었습니다.",
                        "Copied Gemini CLI (`~/.gemini/settings.json`) MCP configuration to clipboard.",
                        "クリップボードに Gemini CLI 用 (`~/.gemini/settings.json`) の MCP 設定をコピーしました。"));
    });

    QPushButton *btnCopyCodexMcp = new QPushButton(MailI18n::t("📋 Codex CLI MCP TOML 복사", "📋 Copy Codex CLI TOML", "📋 Codex CLI TOML コピー"), mcpBox);
    btnCopyCodexMcp->setCursor(Qt::PointingHandCursor);
    btnCopyCodexMcp->setStyleSheet("background: #F0FDF4; color: #15803D; font-weight: 700; border: 1px solid #BBF7D0; border-radius: 5px; padding: 7px 12px;");
    connect(btnCopyCodexMcp, &QPushButton::clicked, this, [this, codexMcpToml]() {
        if (QApplication::clipboard())
            QApplication::clipboard()->setText(codexMcpToml);
        QMessageBox::information(
            this,
            MailI18n::t("Codex MCP 복사 완료", "Codex MCP Copied", "Codex MCP コピー完了"),
            MailI18n::t("클립보드에 OpenAI Codex CLI용 `~/.codex/config.toml` MCP 설정이 복사되었습니다.",
                        "Copied OpenAI Codex CLI (`~/.codex/config.toml`) MCP configuration to clipboard.",
                        "クリップボードに OpenAI Codex CLI 用 (`~/.codex/config.toml`) の MCP 設定をコピーしました。"));
    });

    QPushButton *btnSaveMcpFiles = new QPushButton(MailI18n::t("💾 설정 폴더(config_set)에 파일 생성", "💾 Export MCP Config Files", "💾 MCP設定ファイルを出力"), mcpBox);
    btnSaveMcpFiles->setCursor(Qt::PointingHandCursor);
    btnSaveMcpFiles->setStyleSheet("background: #0D47A1; color: #FFFFFF; font-weight: 700; border: none; border-radius: 5px; padding: 7px 14px;");
    connect(btnSaveMcpFiles, &QPushButton::clicked, this, [this, geminiMcpJson, codexMcpToml, dataDir]() {
        const QString dir = dataDir + QStringLiteral("/config_set");
        QDir().mkpath(dir);
        QFile fg(dir + "/gemini_mcp_settings.json");
        if (fg.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            fg.write(geminiMcpJson.toUtf8());
            fg.close();
        }
        QFile fc(dir + "/codex_mcp_config.toml");
        if (fc.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            fc.write(codexMcpToml.toUtf8());
            fc.close();
        }
        QMessageBox::information(
            this,
            MailI18n::t("MCP 설정 파일 생성 완료", "MCP Configs Exported", "MCP設定ファイル生成完了"),
            MailI18n::t(QString("아래 2개 파일이 생성되었습니다:\n\n1) %1/gemini_mcp_settings.json\n2) %1/codex_mcp_config.toml").arg(dir),
                        QString("Generated the following 2 files:\n\n1) %1/gemini_mcp_settings.json\n2) %1/codex_mcp_config.toml").arg(dir),
                        QString("以下の2つのファイルが生成されました:\n\n1) %1/gemini_mcp_settings.json\n2) %1/codex_mcp_config.toml").arg(dir)));
    });

    QPushButton *btnAutoRegisterMcp = new QPushButton(
        MailI18n::t("⚡ CLI에 MCP 자동 등록 및 연결 시험",
                    "⚡ Auto-Register && Test MCP Server",
                    "⚡ CLIにMCP自動登録・接続テスト"),
        mcpBox);
    btnAutoRegisterMcp->setCursor(Qt::PointingHandCursor);
    btnAutoRegisterMcp->setStyleSheet("background: #059669; color: #FFFFFF; font-weight: 700; border: none; border-radius: 5px; padding: 7px 14px;");
    connect(btnAutoRegisterMcp, &QPushButton::clicked, this, [this, exeFilePath, dataDir, codexMcpToml]() {
        const QString home = QDir::fromNativeSeparators(QDir::homePath());
        const QString geminiDir = home + QStringLiteral("/.gemini");
        const QString geminiPath = geminiDir + QStringLiteral("/settings.json");
        QDir().mkpath(geminiDir);

        QJsonObject rootObj;
        {
            QFile gf(geminiPath);
            if (gf.exists() && gf.open(QIODevice::ReadOnly)) {
                const QJsonDocument existingDoc = QJsonDocument::fromJson(gf.readAll());
                gf.close();
                if (existingDoc.isObject())
                    rootObj = existingDoc.object();
            }
        }
        QJsonObject mcpServers = rootObj.value(QStringLiteral("mcpServers")).toObject();
        mcpServers[QStringLiteral("sollunamail")] = QJsonObject{
            {QStringLiteral("command"), exeFilePath},
            {QStringLiteral("args"), QJsonArray{QStringLiteral("--mcp-stdio")}},
            {QStringLiteral("env"), QJsonObject{{QStringLiteral("SOLLUNAMAIL_DATA_DIR"), dataDir + QStringLiteral("/cache")}}}
        };
        rootObj[QStringLiteral("mcpServers")] = mcpServers;
        bool geminiSaved = false;
        {
            QFile gf(geminiPath);
            if (gf.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
                gf.write(QJsonDocument(rootObj).toJson(QJsonDocument::Indented));
                gf.close();
                geminiSaved = true;
            }
        }

        const QString codexDir = home + QStringLiteral("/.codex");
        const QString codexPath = codexDir + QStringLiteral("/config.toml");
        QDir().mkpath(codexDir);
        bool codexSaved = false;
        {
            QString existingToml;
            QFile cf(codexPath);
            if (cf.exists() && cf.open(QIODevice::ReadOnly)) {
                existingToml = QString::fromUtf8(cf.readAll());
                cf.close();
            }
            if (!existingToml.contains(QStringLiteral("[mcp_servers.sollunamail]"))) {
                if (!existingToml.isEmpty() && !existingToml.endsWith(QLatin1Char('\n')))
                    existingToml += QLatin1Char('\n');
                existingToml += QLatin1Char('\n') + codexMcpToml;
            }
            if (cf.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
                cf.write(existingToml.toUtf8());
                cf.close();
                codexSaved = true;
            }
        }

        // 자체 MCP Stdio 핸드셰이크 및 tools/list 응답 시험
        QProcess proc;
        QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
        env.insert(QStringLiteral("SOLLUNAMAIL_DATA_DIR"), dataDir + QStringLiteral("/cache"));
        proc.setProcessEnvironment(env);
        proc.start(exeFilePath, QStringList{QStringLiteral("--mcp-stdio")});
        bool handshakeOk = false;
        int toolCount = 0;
        if (proc.waitForStarted(3000)) {
            const QByteArray reqPayload =
                "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\",\"params\":{\"protocolVersion\":\"2024-11-05\"}}\n"
                "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"tools/list\",\"params\":{}}\n";
            proc.write(reqPayload);
            proc.closeWriteChannel();
            proc.waitForFinished(3000);
            const QList<QByteArray> lines = proc.readAllStandardOutput().split('\n');
            for (const QByteArray &ln : lines) {
                const QJsonObject obj = QJsonDocument::fromJson(ln.trimmed()).object();
                if (obj.value(QStringLiteral("id")).toInt() == 1 && obj.contains(QStringLiteral("result")))
                    handshakeOk = true;
                if (obj.value(QStringLiteral("id")).toInt() == 2) {
                    toolCount = obj.value(QStringLiteral("result")).toObject().value(QStringLiteral("tools")).toArray().size();
                }
            }
        }

        QMessageBox::information(
            this,
            MailI18n::t("MCP 자동 등록 및 연결 시험 결과", "MCP Auto-Registration & Test Result", "MCP自動登録および接続テスト結果"),
            MailI18n::t(
                QString("✓ Gemini CLI 설정 등록: %1 (%2)\n"
                        "✓ OpenAI Codex CLI 설정 등록: %3 (%4)\n"
                        "✓ MCP 서버(--mcp-stdio) 응답 시험: %5 (제공 도구 %6종 확인 완료)")
                    .arg(geminiSaved ? "성공" : "실패", geminiPath,
                         codexSaved ? "성공" : "실패", codexPath,
                         handshakeOk ? "정상 연결됨" : "실패")
                    .arg(toolCount),
                QString("✓ Gemini CLI config: %1 (%2)\n"
                        "✓ OpenAI Codex CLI config: %3 (%4)\n"
                        "✓ MCP stdio server self-test: %5 (%6 tools verified)")
                    .arg(geminiSaved ? "Saved" : "Failed", geminiPath,
                         codexSaved ? "Saved" : "Failed", codexPath,
                         handshakeOk ? "Connected OK" : "Failed")
                    .arg(toolCount),
                QString("✓ Gemini CLI 設定登録: %1 (%2)\n"
                        "✓ OpenAI Codex CLI 設定登録: %3 (%4)\n"
                        "✓ MCPサーバー(--mcp-stdio)接続テスト: %5 (ツール %6個確認)")
                    .arg(geminiSaved ? "成功" : "失敗", geminiPath,
                         codexSaved ? "成功" : "失敗", codexPath,
                         handshakeOk ? "正常接続" : "失敗")
                    .arg(toolCount)));
    });

    mcpBtnRow->addWidget(btnCopyGeminiMcp);
    mcpBtnRow->addWidget(btnCopyCodexMcp);
    mcpBtnRow->addWidget(btnSaveMcpFiles);
    mcpBtnRow->addWidget(btnAutoRegisterMcp);
    mcpBtnRow->addStretch();
    mv->addLayout(mcpBtnRow);

    v->addWidget(mcpBox);
    v->addStretch();
    scroll->setWidget(page);
    return scroll;
}

QWidget *SettingsDialog::createGeneralPage(const QString &title, const QString &subtitle)
{
    QScrollArea *scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setStyleSheet("background: #FFFFFF;");

    QWidget *page = new QWidget(scroll);
    QVBoxLayout *v = new QVBoxLayout(page);
    v->setContentsMargins(36, 26, 36, 26);
    v->setSpacing(14);

    QLabel *t = new QLabel(title, page);
    t->setStyleSheet("font-size: 18px; font-weight: 700; color: #1F2937;");
    QLabel *s = new QLabel(subtitle, page);
    s->setWordWrap(true);
    s->setStyleSheet("font-size: 12px; color: #6B7280; margin-bottom: 6px;");

    v->addWidget(t);
    v->addWidget(s);
    v->addStretch();
    scroll->setWidget(page);
    return scroll;
}

QWidget *SettingsDialog::createHelpGuidePage()
{
    QScrollArea *scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setStyleSheet("background: #FFFFFF;");

    QWidget *page = new QWidget(scroll);
    QVBoxLayout *v = new QVBoxLayout(page);
    v->setContentsMargins(32, 26, 32, 30);
    v->setSpacing(16);

    // Header
    QLabel *title = new QLabel(
        MailI18n::t("📖 SollunaMail 도움말 및 프로그램 사용 설명서 (User Guide)",
                    "📖 SollunaMail Help & User Guide",
                    "📖 SollunaMail ヘルプと使い方ガイド (User Guide)"),
        page);
    title->setStyleSheet("font-size: 18px; font-weight: 700; color: #1F2937;");
    v->addWidget(title);

    QLabel *sub = new QLabel(
        MailI18n::t("SollunaMail의 계정 연결, 통합 편지함, 예약 발송, AI 이메일 비서(Gemini·Codex), 캘린더·주소록 연동 및 시스템 트레이 사용법을 안내합니다. 아래 검색창에서 궁금한 키워드를 입력하거나 카테고리를 선택하세요.",
                    "Learn how to set up accounts, manage unified inboxes, schedule emails, use AI & MCP (Gemini/Codex), sync calendars/contacts, and configure the system tray. Use the search box or category buttons below to filter topics.",
                    "SollunaMailのアカウント登録、統合受信トレイ、予約送信、AIアシスタント(Gemini・Codex)、カレンダー・連絡先連携、システムトレイの使い方を案内します。下の検索欄またはカテゴリボタンで絞り込めます。"),
        page);
    sub->setWordWrap(true);
    sub->setStyleSheet("font-size: 12px; color: #6B7280; margin-bottom: 2px;");
    v->addWidget(sub);

    // Search & Category Filter Bar
    QFrame *filterFrame = new QFrame(page);
    filterFrame->setStyleSheet(
        "QFrame { background: #F8FAFC; border: 1px solid #E2E8F0; border-radius: 8px; }");
    QVBoxLayout *filterLayout = new QVBoxLayout(filterFrame);
    filterLayout->setContentsMargins(14, 12, 14, 12);
    filterLayout->setSpacing(10);

    QHBoxLayout *searchRow = new QHBoxLayout();
    searchRow->setSpacing(8);
    QLineEdit *searchEdit = new QLineEdit(filterFrame);
    searchEdit->setClearButtonEnabled(true);
    searchEdit->setPlaceholderText(
        MailI18n::t("🔍 사용법 검색 (예: 앱 비밀번호, 예약 발송, 발송 취소, 트레이, 필터, 캘린더, Gemini, 단축키...)",
                    "🔍 Search help topics (e.g., App Password, Schedule, Undo Send, Tray, Filter, Calendar, Gemini, Shortcuts...)",
                    "🔍 使い方を検索 (例: アプリパスワード, 予約送信, 送信取消, トレイ, フィルター, カレンダー, Gemini, ショートカット...)"));
    searchEdit->setStyleSheet(
        "QLineEdit { background: #FFFFFF; border: 1px solid #CBD5E1; border-radius: 6px; padding: 7px 12px; font-size: 12.5px; color: #1E293B; }"
        "QLineEdit:focus { border: 1.5px solid #0D47A1; }");
    searchRow->addWidget(searchEdit, 1);
    filterLayout->addLayout(searchRow);

    QHBoxLayout *catRow = new QHBoxLayout();
    catRow->setSpacing(6);

    struct CatDef {
        QString key;
        QString label;
    };
    const QList<CatDef> categories = {
        { "all",      MailI18n::t("전체 보기",         "All Topics",        "すべて表示") },
        { "account",  MailI18n::t("🚀 계정·연결",       "🚀 Accounts",        "🚀 アカウント") },
        { "mail",     MailI18n::t("📬 메일 읽기·쓰기",   "📬 Read & Compose",  "📬 メール送受信") },
        { "ai",       MailI18n::t("✨ AI & MCP",        "✨ AI & MCP",        "✨ AI & MCP") },
        { "security", MailI18n::t("🛡️ 필터·보안·차단",  "🛡️ Filters & Privacy","🛡️ フィルター・保護") },
        { "calendar", MailI18n::t("📅 캘린더·주소록",   "📅 Calendar & Contacts","📅 カレンダー・連絡先") },
        { "system",   MailI18n::t("🖥️ 트레이·단축키",   "🖥️ Tray & Shortcuts", "🖥️ トレイ・ショートカット") }
    };

    QList<QPushButton *> catBtns;
    for (int i = 0; i < categories.size(); ++i) {
        QPushButton *btn = new QPushButton(categories[i].label, filterFrame);
        btn->setCursor(Qt::PointingHandCursor);
        btn->setProperty("catKey", categories[i].key);
        catBtns.append(btn);
        catRow->addWidget(btn);
    }
    catRow->addStretch();
    filterLayout->addLayout(catRow);
    v->addWidget(filterFrame);

    // Help Section Cards Data
    struct HelpSection {
        QString categoryKey;
        QString badgeText;
        QString badgeBg;
        QString badgeFg;
        QString title;
        QString bodyHtml;
        int jumpTabIndex;
        QString jumpBtnText;
    };

    const QList<HelpSection> sections = {
        {
            "account",
            MailI18n::t("STEP 1 · 계정 설정", "STEP 1 · ACCOUNTS", "STEP 1 · アカウント設定"),
            "#EFF6FF", "#1D4ED8",
            MailI18n::t("🚀 1. 이메일 계정 등록 및 앱 비밀번호(2단계 인증) 연결 방법",
                        "🚀 1. Adding Email Accounts & App Passwords (2FA)",
                        "🚀 1. メールアカウントの登録とアプリパスワード(2段階認証)接続"),
            MailI18n::t(
                "<ul style='margin: 4px 0 4px 18px; line-height: 1.65;'>"
                "<li><b>원클릭 프리셋 등록:</b> <b>[계정 관리]</b> 탭 또는 좌측 하단 <b>[+]</b> 버튼을 눌러 <b>Google(Gmail), 네이버(Naver), 다음(Daum), 카카오(Kakao), Outlook, Apple iCloud, 다우오피스(DaouOffice)</b> 중 원하는 서비스를 선택하면 IMAP/SMTP 서버 주소와 포트가 자동으로 입력됩니다.</li>"
                "<li><b>2단계 인증 및 앱 비밀번호 주의사항:</b> Gmail, 네이버, 다음/카카오, iCloud 등은 보안 정책상 일반 로그인 비밀번호 대신 각 포털의 보안 설정에서 발급받은 <b>‘앱 비밀번호(App Password)’</b>를 입력해야 정상 연결됩니다. 또한 포털 웹메일 환경설정에서 <b>‘IMAP/SMTP 사용함’</b>이 켜져 있는지 확인해 주세요.</li>"
                "<li><b>서버 연결 테스트 및 아바타 설정:</b> 계정 선택 후 우측의 <b>[서버 설정 (IMAP/SMTP)]</b> 탭에서 <b>[연결 테스트]</b> 버튼으로 로그인 성공 여부를 즉시 검증할 수 있으며, 계정별 색상과 아바타 아이콘을 자유롭게 지정할 수 있습니다.</li>"
                "</ul>",
                "<ul style='margin: 4px 0 4px 18px; line-height: 1.65;'>"
                "<li><b>One-Click Presets:</b> Open <b>[Accounts]</b> or click the bottom-left <b>[+]</b> button to select <b>Gmail, Naver, Daum, Kakao, Outlook, iCloud, or DaouOffice</b> with pre-filled IMAP/SMTP host and port settings.</li>"
                "<li><b>App Passwords (2FA):</b> Providers like Gmail, Naver, Kakao, and iCloud require an <b>App Password</b> generated from your account security page instead of your regular password, and <b>IMAP/SMTP access</b> must be enabled in your webmail settings.</li>"
                "<li><b>Connection Test & Avatars:</b> Select an account and click <b>[Test Connection]</b> in the <b>[Server (IMAP/SMTP)]</b> tab to verify credentials immediately.</li>"
                "</ul>",
                "<ul style='margin: 4px 0 4px 18px; line-height: 1.65;'>"
                "<li><b>ワンクリック登録:</b> <b>[アカウント管理]</b> または左下の <b>[+]</b> ボタンから <b>Gmail, Naver, Daum, Kakao, Outlook, iCloud, DaouOffice</b> を選択すると、IMAP/SMTP設定が自動入力されます。</li>"
                "<li><b>2段階認証とアプリパスワード:</b> GmailやNaver、iCloud等では通常のパスワードではなく、各サービスのセキュリティ設定で発行した<b>「アプリパスワード」</b>の入力とWebメール側での<b>「IMAP/SMTP有効化」</b>が必要です。</li>"
                "<li><b>接続テスト:</b> アカウント選択後、右側の <b>[サーバー設定 (IMAP/SMTP)]</b> タブから <b>[接続テスト]</b> を実行して即座に確認できます。</li>"
                "</ul>"),
            1,
            MailI18n::t("계정 관리 탭으로 이동 →", "Go to Accounts →", "アカウント管理へ移動 →")
        },
        {
            "mail",
            MailI18n::t("핵심 기능 · 편지함", "CORE · INBOX & READER", "基本機能 · 受信トレイ"),
            "#F0FDF4", "#15803D",
            MailI18n::t("📬 2. 통합 편지함 탐색, 빠른 필터, 페이지네이션 및 메일 읽기",
                        "📬 2. Unified Inbox, Quick Filters, Pagination & Reading Mail",
                        "📬 2. 統合受信トレイ、クイックフィルター、ページ切り替えと閲覧"),
            MailI18n::t(
                "<ul style='margin: 4px 0 4px 18px; line-height: 1.65;'>"
                "<li><b>통합 편지함 & 개별 계정 전환:</b> 가장 좌측의 <b>통합 편지함(봉투 아이콘)</b>을 누르면 등록된 모든 계정의 메일을 한곳에서 모아 볼 수 있으며, 아래의 계정 아바타를 클릭하면 해당 계정의 메일함과 내 폴더만 독립적으로 조회합니다.</li>"
                "<li><b>빠른 필터 & 검색:</b> 메일 목록 상단 아이콘으로 <b>검색창(제목/발신자/본문)</b>과 <b>빠른 필터(전체 · 안읽음 · 별표 · 첨부파일)</b>를 켜고 끌 수 있습니다.</li>"
                "<li><b>20건 페이지네이션 & 제목·본문 동시 로딩:</b> 대량 메일함도 쾌적하게 탐색할 수 있도록 한 페이지당 20건씩 나누어 제목과 본문을 함께 표시하며 하단 페이지 번호(<code>1 2 3 ...</code>)로 이동합니다.</li>"
                "<li><b>체크박스 다중 선택 & 일괄 처리:</b> 체크박스를 클릭할 때는 본문을 불러오지 않으며, 여러 메일을 체크한 뒤 상단 툴바 또는 <code>⋮</code> 메뉴에서 <b>일괄 읽음/안읽음, 별표, 스팸 등록, 휴지통 이동, 영구 삭제</b>를 수행할 수 있습니다.</li>"
                "<li><b>크게 보기 & 원문(.eml)/인쇄:</b> 우측 본문 상단의 <b>[크게 보기]</b> 버튼(또는 메일 더블클릭)으로 독립된 읽기 창을 띄울 수 있고, 본문 우측 <code>⋮</code> 메뉴에서 <b>인쇄, PDF 저장, 원문(.eml) 보기, 텍스트만 보기</b>를 지원합니다.</li>"
                "</ul>",
                "<ul style='margin: 4px 0 4px 18px; line-height: 1.65;'>"
                "<li><b>Unified Inbox & Account Switcher:</b> Click the top envelope icon on the far-left rail to view all accounts together, or click an individual account avatar below it.</li>"
                "<li><b>Quick Filters & Pagination:</b> Filter by <b>All / Unread / Starred / Attachment</b> and navigate large mailboxes smoothly at 20 messages per page with subjects and bodies loaded together.</li>"
                "<li><b>Batch Actions & Pop-out Reader:</b> Check multiple messages for bulk actions (Read/Unread, Star, Spam, Trash, Delete Permanently), or click <b>[Pop-out View]</b> to read in a standalone window with Print/PDF/.eml support.</li>"
                "</ul>",
                "<ul style='margin: 4px 0 4px 18px; line-height: 1.65;'>"
                "<li><b>統合受信トレイとアカウント切替:</b> 左端上部の封筒アイコンで全アカウントを統合表示し、下の各アバターをクリックすると個別アカウントに切り替わります。</li>"
                "<li><b>フィルターとページ表示:</b> <b>すべて・未読・スター・添付あり</b>で絞り込みでき、1ページ20件単位で件名と本文を同時に読み込みます。</li>"
                "<li><b>一括操作と拡大表示:</b> チェックボックスで複数選択して一括既読・削除・迷惑メール処理が可能で、<b>[拡大表示]</b> ボタンで別ウィンドウ表示や印刷・PDF保存・原文(.eml)確認も行えます。</li>"
                "</ul>"),
            2,
            MailI18n::t("테마 및 레이아웃 설정 →", "Go to Appearance →", "外観・レイアウト設定へ →")
        },
        {
            "mail",
            MailI18n::t("작성 · 발송 제어", "COMPOSE & SCHEDULE", "作成 · 予約送信"),
            "#FEF3C7", "#B45309",
            MailI18n::t("✍️ 3. 편지 쓰기, 서명·상용구, 예약 발송(Scheduled Send) 및 발송 취소(Undo Send)",
                        "✍️ 3. Composing, Signatures, Snippets, Scheduled Send & Undo Send",
                        "✍️ 3. メール作成、署名・定型文、予約送信と送信取り消し(Undo Send)"),
            MailI18n::t(
                "<ul style='margin: 4px 0 4px 18px; line-height: 1.65;'>"
                "<li><b>편지 쓰기 & 서식 도구:</b> 메일 목록 상단의 <b>편지 쓰기 아이콘</b>(또는 <code>Ctrl+N</code>)을 눌러 새 메일을 작성합니다. 글꼴, 크기, 색상, 정렬, 표, 이미지 삽입 및 파일 첨부를 지원합니다.</li>"
                "<li><b>서명(Signatures) 및 상용구(Snippets):</b> 계정별 HTML 서명을 자동으로 삽입하거나, 자주 쓰는 인사말·계좌번호·안내문을 상용구(예: <code>/인사</code>, <code>/감사</code>)로 등록해 빠르게 불러올 수 있습니다.</li>"
                "<li><b>⏰ 예약 발송 (Scheduled Send):</b> 작성 창 하단에서 예약 시각을 지정해 발송하면 <b>발송 예약 대기열</b>에 안전하게 보관되며, 메인 창 상단 툴바의 <b>[⏰ 발송 예약 중]</b> 배너를 클릭해 언제든 <b>예약 내용을 확인·즉시 발송·취소</b>할 수 있습니다.</li>"
                "<li><b>↩️ 발송 취소 대기 시간 (Undo Send):</b> <b>[편지 읽기 및 쓰기]</b> 설정에서 발송 취소 대기 시간(기본 5초, 최대 30초)을 설정하면, [보내기] 클릭 직후 실수를 발견했을 때 즉시 발송을 취소할 수 있습니다.</li>"
                "</ul>",
                "<ul style='margin: 4px 0 4px 18px; line-height: 1.65;'>"
                "<li><b>Rich Compose:</b> Press <code>Ctrl+N</code> to compose with full formatting, inline images, attachments, per-account <b>Signatures</b>, and reusable <b>Snippets</b>.</li>"
                "<li><b>⏰ Scheduled Send:</b> Schedule emails for a specific date/time. Active scheduled emails appear in the top <b>[⏰ Scheduled]</b> badge where you can inspect, send immediately, or cancel them.</li>"
                "<li><b>↩️ Undo Send:</b> Configure the Undo Send grace period (up to 30 seconds) under <b>[Reading & Composing]</b> to recall an email right after hitting Send.</li>"
                "</ul>",
                "<ul style='margin: 4px 0 4px 18px; line-height: 1.65;'>"
                "<li><b>メール作成と署名・定型文:</b> <code>Ctrl+N</code> でリッチテキスト作成ウィンドウを開き、アカウント別の<b>署名(Signatures)</b>やよく使う<b>定型文(Snippets)</b>をワンクリックで挿入できます。</li>"
                "<li><b>⏰ 予約送信 (Scheduled Send):</b> 指定日時に自動送信でき、メイン画面上部の <b>[⏰ 送信予約中]</b> バナーから予約一覧の確認・即時送信・キャンセルが可能です。</li>"
                "<li><b>↩️ 送信取り消し (Undo Send):</b> <b>[閲覧・作成]</b> 設定で送信取り消し待機時間(5〜30秒)を設定すると、送信直後のミスを取り消せます。</li>"
                "</ul>"),
            6,
            MailI18n::t("편지 읽기 및 쓰기 설정 →", "Go to Reading & Composing →", "閲覧・作成設定へ →")
        },
        {
            "ai",
            MailI18n::t("AI & MCP 브리지", "AI & MCP BRIDGE", "AI & MCP 連携"),
            "#F3E8FF", "#7E22CE",
            MailI18n::t("✨ 4. AI 이메일 비서 (Google Gemini · OpenAI Codex) 및 MCP 서버 연동",
                        "✨ 4. AI Email Assistant (Google Gemini · OpenAI Codex) & MCP Server",
                        "✨ 4. AIメールアシスタント (Google Gemini · OpenAI Codex) と MCP連携"),
            MailI18n::t(
                "<ul style='margin: 4px 0 4px 18px; line-height: 1.65;'>"
                "<li><b>AI 초안 작성·요약·번역:</b> 편지 쓰기 창의 <b>[✨ AI 도우미]</b> 버튼을 통해 메일 초안 자동 작성, 긴 메일 핵심 요약, 정중한 비즈니스 문체 다듬기, <b>한국어·영어·일본어 실시간 번역</b>을 사용할 수 있습니다.</li>"
                "<li><b>API 키 또는 로컬 CLI 연동:</b> <b>[AI & MCP]</b> 탭에서 <code>Gemini API Key</code> 또는 <code>OpenAI API Key</code>를 직접 입력하거나, 키를 비워두면 시스템에 설치된 로컬 <code>gemini</code> CLI / <code>codex</code> CLI를 자동으로 호출합니다.</li>"
                "<li><b>MCP (Model Context Protocol) 서버 브리지:</b> <b>[AI & MCP]</b> 탭 하단에서 <b>[Gemini CLI MCP JSON 복사]</b> 또는 <b>[Codex CLI MCP TOML 복사]</b> 버튼을 누르면, 터미널의 Gemini/Codex 에이전트가 SollunaMail의 메일함·캘린더·주소록을 직접 조회·요약·초안 작성할 수 있도록 표준 MCP(`--mcp-stdio`) 설정이 생성됩니다.</li>"
                "</ul>",
                "<ul style='margin: 4px 0 4px 18px; line-height: 1.65;'>"
                "<li><b>AI Compose, Summarize & Translate:</b> Use the <b>[✨ AI Assistant]</b> button in the composer to draft emails, polish tone, summarize threads, or translate between Korean, English, and Japanese.</li>"
                "<li><b>API Key or Local CLI:</b> Enter a Gemini or OpenAI API key in <b>[AI & MCP]</b>, or leave blank to use your local <code>gemini</code> or <code>codex</code> CLI binary.</li>"
                "<li><b>MCP Server Bridge:</b> One-click export for <code>~/.gemini/settings.json</code> and <code>~/.codex/config.toml</code> lets external AI agents query SollunaMail via <code>--mcp-stdio</code>.</li>"
                "</ul>",
                "<ul style='margin: 4px 0 4px 18px; line-height: 1.65;'>"
                "<li><b>AI下書き・要約・翻訳:</b> 作成画面の <b>[✨ AIアシスタント]</b> からメール文面の自動作成、要約、ビジネス表現への推敲、日・韓・英翻訳を実行できます。</li>"
                "<li><b>MCPサーバー連携:</b> <b>[AI & MCP]</b> タブから Gemini CLI (<code>~/.gemini/settings.json</code>) および OpenAI Codex CLI (<code>~/.codex/config.toml</code>) 用のMCP設定をワンクリックでコピー・生成できます。</li>"
                "</ul>"),
            12,
            MailI18n::t("AI & MCP 설정 탭으로 이동 →", "Go to AI & MCP →", "AI & MCP設定へ移動 →")
        },
        {
            "security",
            MailI18n::t("자동 분류 · 보안", "FILTERS & PRIVACY HUB", "自動分類 · セキュリティ"),
            "#FEE2E2", "#B91C1C",
            MailI18n::t("🛡️ 5. 자동 분류 필터 규칙, 개인정보 보호(Privacy Hub) 및 발신자 차단",
                        "🛡️ 5. Filter Rules, Privacy Hub & Sender Blocking",
                        "🛡️ 5. フィルター規則、プライバシー保護と送信者ブロック"),
            MailI18n::t(
                "<ul style='margin: 4px 0 4px 18px; line-height: 1.65;'>"
                "<li><b>자동 필터 규칙 (Filters):</b> 발신자, 수신자, 제목 키워드 조건에 따라 새 메일을 특정 폴더로 자동 이동하거나 읽음 표시·별표·휴지통 이동을 수행합니다. <b>[지금 모든 메일에 규칙 적용]</b> 버튼으로 기존 메일함도 한 번에 정리할 수 있습니다.</li>"
                "<li><b>추적 픽셀 및 원격 이미지 차단:</b> 마케팅/스팸 메일이 열람 여부를 몰래 추적하지 못하도록 <b>[개인정보 보호]</b> 탭에서 외부 원격 이미지 자동 로드를 차단하고, 신뢰하는 메일에서만 이미지를 표시할 수 있습니다.</li>"
                "<li><b>스팸 및 발신자/도메인 차단 관리:</b> 원치 않는 발신자나 도메인(예: <code>@spam.com</code>)을 차단 목록에 등록하면 수신 즉시 스팸/휴지통으로 격리되며, 언제든 차단 목록에서 해제할 수 있습니다.</li>"
                "</ul>",
                "<ul style='margin: 4px 0 4px 18px; line-height: 1.65;'>"
                "<li><b>Filter Rules:</b> Automatically move, star, mark as read, or delete incoming emails based on sender/recipient/subject rules, and run rules retroactively on existing mail.</li>"
                "<li><b>Privacy Hub & Block List:</b> Block tracking pixels and remote images by default, and manage blocked email addresses or entire domains under <b>[Privacy hub]</b>.</li>"
                "</ul>",
                "<ul style='margin: 4px 0 4px 18px; line-height: 1.65;'>"
                "<li><b>フィルター規則:</b> 差出人・宛先・件名の条件に応じてフォルダ移動や既読・スター付与を自動化し、既存メールへの一括適用も可能です。</li>"
                "<li><b>プライバシー保護とブロック管理:</b> 追跡ピクセルや外部画像の自動読み込みを遮断し、迷惑な送信者やドメインをブロックリストで一元管理できます。</li>"
                "</ul>"),
            3,
            MailI18n::t("개인정보 보호 탭으로 이동 →", "Go to Privacy Hub →", "プライバシー保護へ移動 →")
        },
        {
            "calendar",
            MailI18n::t("일정 · 연락처", "CALENDAR & CONTACTS", "カレンダー · 連絡先"),
            "#E0F2FE", "#0369A1",
            MailI18n::t("📅 6. 캘린더(월/주/일/일정 목록) 동기화 및 주소록(VIP·그룹) 활용",
                        "📅 6. Calendar Views (Month/Week/Day/Agenda) & Contacts Management",
                        "📅 6. カレンダー(月/週/日/予定リスト)同期と連絡先(VIP・グループ)管理"),
            MailI18n::t(
                "<ul style='margin: 4px 0 4px 18px; line-height: 1.65;'>"
                "<li><b>우측 도킹 캘린더 & 전체 캘린더:</b> 메일 화면 우측 사이드바에서 오늘의 타임라인과 실시간 현재 시각 표시선(빨간색 마커)을 보며 일정을 바로 확인할 수 있고, 좌측 레일의 <b>캘린더 아이콘</b>을 누르면 <b>월간 · 주간 · 일간 · 일정 목록(Agenda)</b> 전체 화면으로 전환됩니다.</li>"
                "<li><b>외부 캘린더 연동 (Google / iCloud / WebCal / .ics):</b> <b>[캘린더]</b> 설정 탭에서 Google Calendar OAuth 연동, Apple iCloud CalDAV 연동, 공휴일/외부 WebCal URL 구독, 표준 <code>.ics</code> 파일 가져오기·내보내기를 지원합니다.</li>"
                "<li><b>주소록 및 그룹 관리:</b> 좌측 레일의 <b>주소록 아이콘</b>을 클릭해 연락처 추가·편집, <b>중요(VIP) 표시</b>, 그룹 지정, CSV/vCard 가져오기·내보내기를 수행하며, 연락처 카드에서 바로 해당 수신자에게 새 메일을 작성할 수 있습니다.</li>"
                "</ul>",
                "<ul style='margin: 4px 0 4px 18px; line-height: 1.65;'>"
                "<li><b>Docked & Full Calendar:</b> Check today's timeline with a live current-time marker on the right sidebar, or switch to the full <b>Month / Week / Day / Agenda</b> calendar view from the left rail.</li>"
                "<li><b>Cloud Sync & .ics:</b> Connect Google Calendar, Apple iCloud (CalDAV), WebCal URLs, or import/export <code>.ics</code> files in <b>[Calendar]</b> settings.</li>"
                "<li><b>Contacts & Groups:</b> Manage contacts, VIP stars, groups, and CSV/vCard files from the Contacts view on the left rail.</li>"
                "</ul>",
                "<ul style='margin: 4px 0 4px 18px; line-height: 1.65;'>"
                "<li><b>右側カレンダーと全体表示:</b> メール画面右側のタイムラインで現在時刻マーカーと共に今日の予定を確認でき、左レールのカレンダーアイコンから <b>月・週・日・予定リスト</b> 画面へ切り替えられます。</li>"
                "<li><b>外部連携と連絡先:</b> Googleカレンダー、Apple iCloud(CalDAV)、WebCal購読、<code>.ics</code>入出力に対応し、連絡先画面ではグループ管理やCSV/vCard連携が可能です。</li>"
                "</ul>"),
            7,
            MailI18n::t("캘린더 설정 탭으로 이동 →", "Go to Calendar Settings →", "カレンダー設定へ移動 →")
        },
        {
            "system",
            MailI18n::t("백그라운드 · 트레이", "STARTUP & SYSTEM TRAY", "自動起動 · システムトレイ"),
            "#ECFEFF", "#0E7490",
            MailI18n::t("🖥️ 7. 윈도우 자동 실행 및 시스템 트레이 백그라운드 수신 동작",
                        "🖥️ 7. Windows Startup & System Tray Background Operation",
                        "🖥️ 7. Windows自動起動とシステムトレイでのバックグラウンド受信"),
            MailI18n::t(
                "<ul style='margin: 4px 0 4px 18px; line-height: 1.65;'>"
                "<li><b>기본 시작 및 트레이 동작:</b> <b>[일반 및 알림]</b> 탭의 <b>‘시작 및 시스템 트레이 동작’</b>에서 다음 4가지 옵션을 제어할 수 있습니다."
                "<br/>① <b>윈도우 시작 시 자동 실행 (기본값 ON):</b> PC 부팅 및 로그인 시 자동으로 실행됩니다."
                "<br/>② <b>최소화 실행 (기본값 OFF):</b> 시작 시 메인 창을 띄우지 않고 우측 하단 시스템 트레이에서 조용히 시작합니다."
                "<br/>③ <b>창 닫기(X) 시 시스템 트레이로 이동 (기본값 ON):</b> 우측 상단 <code>X</code> 버튼을 눌러도 프로그램이 종료되지 않고 백그라운드에서 새 메일과 예약 발송을 계속 처리합니다."
                "<br/>④ <b>창 최소화(_) 시 트레이로 숨기기 (기본값 OFF):</b> 최소화 버튼을 눌렀을 때 작업표시줄에 남길지 트레이로 숨길지 선택합니다.</li>"
                "<li><b>완전 종료 방법:</b> 프로그램을 완전히 종료하려면 작업표시줄 우측 하단 <b>시스템 트레이의 SollunaMail 아이콘을 우클릭 → [종료]</b>를 선택하거나, 상단 메뉴 <b>[파일(F) → 종료]</b>(<code>Ctrl+Q</code>)를 실행하세요. 종료 시 예약 메일과 캐시 데이터를 안전하게 저장한 뒤 정상 종료됩니다.</li>"
                "</ul>",
                "<ul style='margin: 4px 0 4px 18px; line-height: 1.65;'>"
                "<li><b>Startup & Tray Options:</b> Under <b>[General & Notifications]</b>, configure <b>Launch at Windows startup (Default: ON)</b>, <b>Start minimized to tray (Default: OFF)</b>, <b>Close to tray on X (Default: ON)</b>, and <b>Minimize to tray (Default: OFF)</b>.</li>"
                "<li><b>Exiting Completely:</b> Right-click the SollunaMail icon in the Windows system tray and click <b>[Quit]</b> (or use <b>File → Exit / Ctrl+Q</b>) to save all state and exit cleanly.</li>"
                "</ul>",
                "<ul style='margin: 4px 0 4px 18px; line-height: 1.65;'>"
                "<li><b>起動とトレイ動作:</b> <b>[一般・通知]</b> タブにて、<b>Windows起動時に自動実行(既定ON)</b>、<b>最小化起動(既定OFF)</b>、<b>閉じる(X)ボタンでトレイへ移動(既定ON)</b>、<b>最小化(_)時にトレイへ隠す(既定OFF)</b> を設定できます。</li>"
                "<li><b>完全終了の方法:</b> タスクトレイのSollunaMailアイコンを右クリックして <b>[終了]</b> を選択するか、メニューの <b>[ファイル(F) → 終了]</b> (<code>Ctrl+Q</code>) を実行すると、予約データ等を安全に保存して終了します。</li>"
                "</ul>"),
            0,
            MailI18n::t("일반 및 알림 탭으로 이동 →", "Go to General Settings →", "一般・通知設定へ移動 →")
        },
        {
            "system",
            MailI18n::t("단축키 · 백업", "SHORTCUTS & BACKUP", "ショートカット · バックアップ"),
            "#F1F5F9", "#334155",
            MailI18n::t("⌨️ 8. 주요 키보드 단축키 요약 및 환경설정 백업/복원",
                        "⌨️ 8. Keyboard Shortcuts Reference & Settings Backup/Restore",
                        "⌨️ 8. 主なキーボードショートカットと環境設定のバックアップ/復元"),
            MailI18n::t(
                "<ul style='margin: 4px 0 4px 18px; line-height: 1.65;'>"
                "<li><b><code>Ctrl + N</code></b> : 새 편지 쓰기 &nbsp;|&nbsp; <b><code>Ctrl + R</code></b> : 선택한 메일 답장</li>"
                "<li><b><code>Ctrl + Shift + R</code></b> : 전체 답장 (Reply All) &nbsp;|&nbsp; <b><code>Ctrl + F</code></b> : 메일 전달 / 검색창 포커스</li>"
                "<li><b><code>Delete</code></b> : 선택 메일 휴지통 이동 &nbsp;|&nbsp; <b><code>Ctrl + ,</code> / <code>F5</code></b> : 환경 설정 열기 / 메일 동기화</li>"
                "<li>💡 <b>환경설정 전체 백업 및 복원:</b> PC를 옮기거나 설정을 보관하려면 <b>[고급 설정]</b> 탭에서 <b>[환경설정 내보내기 (.json)]</b>로 계정·폴더·필터·서명·상용구를 한 번에 백업하고 언제든 복원할 수 있습니다.</li>"
                "</ul>",
                "<ul style='margin: 4px 0 4px 18px; line-height: 1.65;'>"
                "<li><b><code>Ctrl + N</code></b> : Compose New Email &nbsp;|&nbsp; <b><code>Ctrl + R</code></b> : Reply to Selected Email</li>"
                "<li><b><code>Ctrl + Shift + R</code></b> : Reply All &nbsp;|&nbsp; <b><code>Ctrl + F</code></b> : Forward / Search Mail</li>"
                "<li><b><code>Delete</code></b> : Move to Trash &nbsp;|&nbsp; <b><code>Ctrl + ,</code> / <code>F5</code></b> : Open Settings / Sync Mail</li>"
                "<li>💡 <b>Backup &amp; Restore Settings:</b> Use <b>[Export Settings (.json)]</b> and <b>[Import Settings (.json)]</b> under the <b>[Advanced]</b> tab to back up or migrate your configuration.</li>"
                "</ul>",
                "<ul style='margin: 4px 0 4px 18px; line-height: 1.65;'>"
                "<li><b><code>Ctrl + N</code></b> : 新規メール作成 &nbsp;|&nbsp; <b><code>Ctrl + R</code></b> : 選択メールに返信</li>"
                "<li><b><code>Ctrl + Shift + R</code></b> : 全員に返信 &nbsp;|&nbsp; <b><code>Ctrl + F</code></b> : 転送 / メール検索</li>"
                "<li><b><code>Delete</code></b> : ゴミ箱へ移動 &nbsp;|&nbsp; <b><code>Ctrl + ,</code> / <code>F5</code></b> : 環境設定 / メール同期</li>"
                "<li>💡 <b>設定のバックアップと復元:</b> <b>[詳細設定]</b> タブから JSON 形式で全設定をエクスポート・インポートできます。</li>"
                "</ul>"),
            10,
            MailI18n::t("단축키 설정 탭으로 이동 →", "Go to Shortcuts →", "ショートカット設定へ移動 →")
        }
    };

    QList<QFrame *> cardFrames;
    for (const HelpSection &sec : sections) {
        QFrame *card = new QFrame(page);
        card->setProperty("catKey", sec.categoryKey);
        card->setProperty("searchBlob", (sec.title + " " + sec.badgeText + " " + sec.bodyHtml).toLower());
        card->setStyleSheet(
            "QFrame#HelpGuideCard { background: #FFFFFF; border: 1px solid #E5E7EB; border-radius: 10px; }"
            "QFrame#HelpGuideCard:hover { border: 1px solid #BFDBFE; }");
        card->setObjectName("HelpGuideCard");

        QVBoxLayout *cv = new QVBoxLayout(card);
        cv->setContentsMargins(18, 14, 18, 14);
        cv->setSpacing(8);

        QHBoxLayout *topRow = new QHBoxLayout();
        topRow->setSpacing(8);

        QLabel *badge = new QLabel(sec.badgeText, card);
        badge->setStyleSheet(QString(
            "background: %1; color: %2; font-size: 10.5px; font-weight: 700; padding: 3px 8px; border-radius: 4px;")
            .arg(sec.badgeBg, sec.badgeFg));
        topRow->addWidget(badge);
        topRow->addStretch();

        if (sec.jumpTabIndex >= 0) {
            QPushButton *jumpBtn = new QPushButton(sec.jumpBtnText, card);
            jumpBtn->setCursor(Qt::PointingHandCursor);
            jumpBtn->setStyleSheet(
                "QPushButton { background: #F8FAFC; color: #0D47A1; border: 1px solid #CBD5E1; border-radius: 5px; padding: 4px 10px; font-size: 11px; font-weight: 700; }"
                "QPushButton:hover { background: #EFF6FF; border-color: #93C5FD; }");
            int targetIdx = sec.jumpTabIndex;
            connect(jumpBtn, &QPushButton::clicked, this, [this, targetIdx]() {
                if (m_navList && targetIdx >= 0 && targetIdx < m_navList->count()) {
                    m_navList->setCurrentRow(targetIdx);
                }
            });
            topRow->addWidget(jumpBtn);
        }
        cv->addLayout(topRow);

        QLabel *secTitle = new QLabel(sec.title, card);
        secTitle->setWordWrap(true);
        secTitle->setStyleSheet("font-size: 14px; font-weight: 700; color: #111827; border: none;");
        cv->addWidget(secTitle);

        QLabel *secBody = new QLabel(sec.bodyHtml, card);
        secBody->setWordWrap(true);
        secBody->setTextFormat(Qt::RichText);
        secBody->setOpenExternalLinks(false);
        secBody->setStyleSheet("font-size: 12.2px; color: #374151; border: none;");
        cv->addWidget(secBody);

        cardFrames.append(card);
        v->addWidget(card);
    }

    QLabel *emptyHint = new QLabel(
        MailI18n::t("검색어와 일치하는 도움말 항목이 없습니다. 다른 키워드로 검색해 보세요.",
                    "No help topics matched your search query. Try another keyword.",
                    "検索キーワードに一致するヘルプ項目がありません。別のキーワードをお試しください。"),
        page);
    emptyHint->setAlignment(Qt::AlignCenter);
    emptyHint->setStyleSheet("font-size: 13px; color: #6B7280; padding: 24px;");
    emptyHint->setVisible(false);
    v->addWidget(emptyHint);

    // Filter state updater
    auto applyHelpFilter = [cardFrames, catBtns, searchEdit, emptyHint, page]() {
        QString activeCat = page->property("activeHelpCat").toString();
        if (activeCat.isEmpty())
            activeCat = "all";

        for (QPushButton *b : catBtns) {
            bool on = (b->property("catKey").toString() == activeCat);
            b->setStyleSheet(on
                ? "QPushButton { background: #0D47A1; color: #FFFFFF; font-size: 11.5px; font-weight: 700; border: none; border-radius: 5px; padding: 5px 10px; }"
                : "QPushButton { background: #FFFFFF; color: #475569; font-size: 11.5px; font-weight: 600; border: 1px solid #CBD5E1; border-radius: 5px; padding: 5px 10px; }"
                  "QPushButton:hover { background: #EFF6FF; color: #0D47A1; border-color: #93C5FD; }");
        }

        const QString q = searchEdit->text().trimmed().toLower();
        int visibleCount = 0;
        for (QFrame *card : cardFrames) {
            const QString cKey = card->property("catKey").toString();
            const QString blob = card->property("searchBlob").toString();
            bool catMatch = (activeCat == "all" || cKey == activeCat);
            bool textMatch = (q.isEmpty() || blob.contains(q));
            bool show = (catMatch && textMatch);
            card->setVisible(show);
            if (show)
                ++visibleCount;
        }
        emptyHint->setVisible(visibleCount == 0);
    };

    page->setProperty("activeHelpCat", "all");
    for (QPushButton *b : catBtns) {
        connect(b, &QPushButton::clicked, this, [page, b, applyHelpFilter]() {
            page->setProperty("activeHelpCat", b->property("catKey").toString());
            applyHelpFilter();
        });
    }
    connect(searchEdit, &QLineEdit::textChanged, this, [applyHelpFilter](const QString &) {
        applyHelpFilter();
    });
    applyHelpFilter();

    v->addStretch();
    scroll->setWidget(page);
    return scroll;
}



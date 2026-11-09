#include "googleprovider.h"
#include <QDesktopServices>
#include <QUrl>
#include <QUrlQuery>
#include <QNetworkRequest>
#include <QNetworkReply>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QTcpSocket>
#include <QRegularExpression>
#include <QImage>
#include <QBuffer>
#include <QLocale>
#include <QDateTime>
#include <QTimeZone>
#include <QUuid>
#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <QStandardPaths>
#include <QCoreApplication>
#include <QMimeDatabase>
#include <QTimer>
#include <QSharedPointer>
#include <QCryptographicHash>
#include <functional>

#if __has_include("oauth_secrets_local.h")
#include "oauth_secrets_local.h"
#endif

#ifdef Q_OS_WIN
#include <windows.h>
#endif

namespace {

bool isStrictValidUtf8(const QByteArray &bytes)
{
    const unsigned char *data = reinterpret_cast<const unsigned char *>(bytes.constData());
    int len = bytes.size();
    int i = 0;
    while (i < len) {
        unsigned char c = data[i];
        if (c <= 0x7F) {
            ++i;
        } else if (c >= 0xC2 && c <= 0xDF) {
            if (i + 1 >= len || (data[i + 1] & 0xC0) != 0x80)
                return false;
            i += 2;
        } else if (c == 0xE0) {
            if (i + 2 >= len || data[i + 1] < 0xA0 || data[i + 1] > 0xBF || (data[i + 2] & 0xC0) != 0x80)
                return false;
            i += 3;
        } else if ((c >= 0xE1 && c <= 0xEC) || (c >= 0xEE && c <= 0xEF)) {
            if (i + 2 >= len || (data[i + 1] & 0xC0) != 0x80 || (data[i + 2] & 0xC0) != 0x80)
                return false;
            i += 3;
        } else if (c == 0xED) {
            // Exclude UTF-16 surrogates (ED A0..BF)
            if (i + 2 >= len || data[i + 1] < 0x80 || data[i + 1] > 0x9F || (data[i + 2] & 0xC0) != 0x80)
                return false;
            i += 3;
        } else if (c == 0xF0) {
            if (i + 3 >= len || data[i + 1] < 0x90 || data[i + 1] > 0xBF
                || (data[i + 2] & 0xC0) != 0x80 || (data[i + 3] & 0xC0) != 0x80)
                return false;
            i += 4;
        } else if (c >= 0xF1 && c <= 0xF3) {
            if (i + 3 >= len || (data[i + 1] & 0xC0) != 0x80
                || (data[i + 2] & 0xC0) != 0x80 || (data[i + 3] & 0xC0) != 0x80)
                return false;
            i += 4;
        } else if (c == 0xF4) {
            if (i + 3 >= len || data[i + 1] < 0x80 || data[i + 1] > 0x8F
                || (data[i + 2] & 0xC0) != 0x80 || (data[i + 3] & 0xC0) != 0x80)
                return false;
            i += 4;
        } else {
            return false;
        }
    }
    return true;
}

#ifdef Q_OS_WIN
QString winCodePageToQString(UINT codePage, const QByteArray &bytes)
{
    if (bytes.isEmpty())
        return QString();
    int wlen = MultiByteToWideChar(codePage, 0, bytes.constData(), bytes.size(), nullptr, 0);
    if (wlen <= 0)
        return QString::fromUtf8(bytes);
    QString out(wlen, Qt::Uninitialized);
    MultiByteToWideChar(codePage, 0, bytes.constData(), bytes.size(),
                        reinterpret_cast<wchar_t *>(out.data()), wlen);
    return out;
}
#endif

void bindSocketLifecycleCleanup(QSslSocket *socket,
                                std::function<void()> onDisconnectedOrFailed,
                                std::function<void()> onDestroyedFreeHeap,
                                int safetyTimeoutMs = 60000)
{
    if (!socket) {
        if (onDestroyedFreeHeap)
            onDestroyedFreeHeap();
        return;
    }

    auto finalized = QSharedPointer<bool>::create(false);
    auto finalizeSocket = [socket, finalized, onDisconnectedOrFailed]() {
        if (*finalized)
            return;
        *finalized = true;
        if (onDisconnectedOrFailed)
            onDisconnectedOrFailed();
        socket->deleteLater();
    };

    QObject::connect(socket, &QSslSocket::disconnected, socket, finalizeSocket);
    QObject::connect(socket, &QAbstractSocket::errorOccurred, socket,
                     [socket, finalizeSocket](QAbstractSocket::SocketError) {
        if (socket->state() == QAbstractSocket::UnconnectedState) {
            finalizeSocket();
        }
    });
    if (safetyTimeoutMs > 0) {
        QTimer::singleShot(safetyTimeoutMs, socket, [socket, finalizeSocket]() {
            if (socket->state() != QAbstractSocket::UnconnectedState) {
                socket->abort();
            }
            finalizeSocket();
        });
    }
    if (onDestroyedFreeHeap) {
        QObject::connect(socket, &QObject::destroyed, [onDestroyedFreeHeap]() {
            onDestroyedFreeHeap();
        });
    }
}

} // namespace

GoogleProvider::GoogleProvider(QObject *parent)
    : QObject(parent),
      m_nam(new QNetworkAccessManager(this))
{
}

GoogleProvider::~GoogleProvider()
{
    m_isShuttingDown = true;
    m_idleSockets.clear();
    const QList<QSslSocket *> allSockets = findChildren<QSslSocket *>();
    for (QSslSocket *sock : allSockets) {
        if (!sock)
            continue;
        sock->disconnect();
        if (sock->state() != QAbstractSocket::UnconnectedState) {
            sock->abort();
        }
    }
    const QList<QNetworkReply *> allReplies = findChildren<QNetworkReply *>();
    for (QNetworkReply *reply : allReplies) {
        if (!reply)
            continue;
        reply->disconnect();
        if (reply->isRunning()) {
            reply->abort();
        }
    }
}

QString GoogleProvider::decodeCharsetBytes(const QByteArray &bytes, const QString &charset)
{
    if (bytes.isEmpty())
        return QString();

    QString cs = charset.trimmed().toLower();
    cs.remove('"').remove('\'');

#ifdef Q_OS_WIN
    if (cs.contains("euc-kr") || cs.contains("cp949") || cs.contains("ks_c_5601")
        || cs.contains("ksc5601") || cs.contains("uhc") || cs.contains("korean")) {
        // 만약 선언은 euc-kr이지만 실제 바이트가 순수 UTF-8인 예외 케이스도 방어
        if (isStrictValidUtf8(bytes)) {
            QString u = QString::fromUtf8(bytes);
            if (!u.contains(QChar::ReplacementCharacter))
                return u.normalized(QString::NormalizationForm_C);
        }
        return winCodePageToQString(949, bytes).normalized(QString::NormalizationForm_C);
    }
    if (cs.contains("iso-2022-kr")) {
        return winCodePageToQString(50225, bytes).normalized(QString::NormalizationForm_C);
    }
#endif

    if (isStrictValidUtf8(bytes)) {
        return QString::fromUtf8(bytes).normalized(QString::NormalizationForm_C);
    }

    // UTF-8로 명시된 헤더(=?utf-8?B?...?=)인데 발신 서버가 글자 경계 중간에서 바이트를 잘라 마지막 1~2바이트만 불완전한 경우 CP949로 오판하지 않고 UTF-8로 복원
    if (cs.contains("utf-8") || cs.contains("utf8")) {
        for (int trim = 1; trim <= 2 && bytes.size() > trim; ++trim) {
            const QByteArray trimmed = bytes.left(bytes.size() - trim);
            if (isStrictValidUtf8(trimmed)) {
                return QString::fromUtf8(trimmed).normalized(QString::NormalizationForm_C);
            }
        }
        return QString::fromUtf8(bytes).normalized(QString::NormalizationForm_C);
    }

#ifdef Q_OS_WIN
    // UTF-8 유효성 검사를 통과하지 못한 8비트 한글(EUC-KR / CP949) 바이트열은 자동으로 CP949로 복원
    return winCodePageToQString(949, bytes).normalized(QString::NormalizationForm_C);
#else
    return QString::fromLatin1(bytes).normalized(QString::NormalizationForm_C);
#endif
}

QByteArray GoogleProvider::decodeQuotedPrintable(const QByteArray &input)
{
    QByteArray out;
    out.reserve(input.size());
    const int n = input.size();
    for (int i = 0; i < n; ++i) {
        char ch = input[i];
        if (ch == '=') {
            // Soft line break: =\r\n or =\n
            if (i + 2 < n && input[i + 1] == '\r' && input[i + 2] == '\n') {
                i += 2;
                continue;
            }
            if (i + 1 < n && (input[i + 1] == '\n' || input[i + 1] == '\r')) {
                i += 1;
                continue;
            }
            if (i + 2 < n) {
                bool ok = false;
                int val = input.mid(i + 1, 2).toInt(&ok, 16);
                if (ok) {
                    out.append(char(val));
                    i += 2;
                    continue;
                }
            }
        }
        out.append(ch);
    }
    return out;
}

QString GoogleProvider::decodeMimeHeader(const QString &raw)
{
    if (raw.isEmpty())
        return QString();

    // 1. 멀티라인 접힘(CRLF + 공백/탭) 제거
    QString unfolded = raw;
    unfolded.replace(QRegularExpression(R"(\r?\n[ \t]+)"), " ");

    // 2. RFC 2047 인코딩 토큰(=?charset?B/Q?payload?=) 연속 구문 병합 디코딩
    //    인접한 두 인코딩 워드가 동일한 charset일 때 멀티바이트(UTF-8/EUC-KR)가 갈라져도 깨지지 않도록 바이트를 먼저 합침
    static QRegularExpression rxToken(R"(=\?([^?]+)\?([BbQq])\?([^?]*)\?=)");
    QRegularExpressionMatchIterator it = rxToken.globalMatch(unfolded);
    if (!it.hasNext()) {
        // RFC 2047 토큰이 없고 8비트 바이트(Latin1로 보존된 UTF-8 또는 EUC-KR)가 직접 들어있는 경우
        QByteArray rawLatin1 = unfolded.toLatin1();
        bool hasHighByte = false;
        for (char c : rawLatin1) {
            if (static_cast<unsigned char>(c) >= 0x80) {
                hasHighByte = true;
                break;
            }
        }
        if (hasHighByte && unfolded == QString::fromLatin1(rawLatin1)) {
            return decodeCharsetBytes(rawLatin1).trimmed();
        }
        return unfolded.normalized(QString::NormalizationForm_C).trimmed();
    }

    QString output;
    int lastEnd = 0;
    QString pendingCharset;
    QByteArray pendingBytes;

    auto flushPending = [&]() {
        if (!pendingBytes.isEmpty()) {
            output.append(decodeCharsetBytes(pendingBytes, pendingCharset));
            pendingBytes.clear();
            pendingCharset.clear();
        }
    };

    while (it.hasNext()) {
        QRegularExpressionMatch m = it.next();
        QString between = unfolded.mid(lastEnd, m.capturedStart() - lastEnd);

        // RFC 2047: 인접한 encoded-word 사이의 순수 공백(CRLF LWSP)은 무시하고 바이트 스트림을 이어붙임
        if (!pendingBytes.isEmpty() && !between.trimmed().isEmpty()) {
            flushPending();
            output.append(between);
        } else if (pendingBytes.isEmpty() && !between.isEmpty()) {
            // 앞부분에 일반 텍스트(예: "iCloud Mail ")가 있고 그 안에 8bit 바이트가 있으면 복원
            QByteArray betweenLatin1 = between.toLatin1();
            if (between == QString::fromLatin1(betweenLatin1))
                output.append(decodeCharsetBytes(betweenLatin1));
            else
                output.append(between);
        }

        QString charset = m.captured(1).trimmed();
        QString enc = m.captured(2).toUpper();
        QByteArray payload = m.captured(3).toLatin1();
        QByteArray decodedPart;

        if (enc == "B") {
            decodedPart = QByteArray::fromBase64(payload);
        } else {
            payload.replace('_', ' ');
            decodedPart = decodeQuotedPrintable(payload);
        }

        if (!pendingCharset.isEmpty() && pendingCharset.compare(charset, Qt::CaseInsensitive) != 0) {
            flushPending();
        }
        pendingCharset = charset;
        pendingBytes.append(decodedPart);
        lastEnd = m.capturedEnd();
    }

    flushPending();
    if (lastEnd < unfolded.size()) {
        QString tail = unfolded.mid(lastEnd);
        QByteArray tailLatin1 = tail.toLatin1();
        if (tail == QString::fromLatin1(tailLatin1))
            output.append(decodeCharsetBytes(tailLatin1));
        else
            output.append(tail);
    }

    return output.normalized(QString::NormalizationForm_C).trimmed();
}

static QString formatMimeAttachmentBytes(qint64 bytes)
{
    if (bytes >= 1024LL * 1024LL)
        return QString("%1 MB").arg(double(bytes) / (1024.0 * 1024.0), 0, 'f', 2);
    if (bytes >= 1024LL)
        return QString("%1 KB").arg(double(bytes) / 1024.0, 0, 'f', 1);
    return QString("%1 B").arg(qMax<qint64>(0, bytes));
}

static QString decodeRfc2231Value(const QString &rawVal, const QString &fallbackCharset)
{
    QString charset = fallbackCharset.isEmpty() ? QStringLiteral("UTF-8") : fallbackCharset;
    QString encodedPart = rawVal.trimmed();
    if (encodedPart.startsWith('"') && encodedPart.endsWith('"') && encodedPart.size() >= 2) {
        encodedPart = encodedPart.mid(1, encodedPart.size() - 2);
    }
    const int firstQuote = encodedPart.indexOf('\'');
    if (firstQuote >= 0) {
        const int secondQuote = encodedPart.indexOf('\'', firstQuote + 1);
        if (secondQuote > firstQuote) {
            const QString cs = encodedPart.left(firstQuote).trimmed();
            if (!cs.isEmpty())
                charset = cs;
            encodedPart = encodedPart.mid(secondQuote + 1);
        }
    }
    const QByteArray pctDecoded = QByteArray::fromPercentEncoding(encodedPart.toLatin1());
    return GoogleProvider::decodeCharsetBytes(pctDecoded, charset).trimmed();
}

static QString extractMimeAttachmentFileName(const QString &contentDispositionLatin1,
                                             const QString &contentTypeLatin1,
                                             const QString &partCharset)
{
    const QString combined = contentDispositionLatin1 + QStringLiteral("; ") + contentTypeLatin1;

    // 1. RFC 2231 단일 파라미터: filename*=UTF-8''... 또는 name*=UTF-8''...
    static const QRegularExpression rxRfc2231Single(
        R"rx((?:filename|name)\*\s*=\s*(?:"([^"]+)"|([^;\r\n]+)))rx",
        QRegularExpression::CaseInsensitiveOption);
    QRegularExpressionMatch mSingle = rxRfc2231Single.match(combined);
    if (mSingle.hasMatch()) {
        const QString raw = mSingle.captured(1).isEmpty() ? mSingle.captured(2).trimmed() : mSingle.captured(1).trimmed();
        const QString dec = decodeRfc2231Value(raw, partCharset);
        if (!dec.isEmpty())
            return dec;
    }

    // 2. RFC 2231 분할 파라미터: filename*0*=...; filename*1*=...
    static const QRegularExpression rxRfc2231Multi(
        R"rx((?:filename|name)\*(\d+)(\*?)\s*=\s*(?:"([^"]+)"|([^;\r\n]+)))rx",
        QRegularExpression::CaseInsensitiveOption);
    QRegularExpressionMatchIterator itMulti = rxRfc2231Multi.globalMatch(combined);
    if (itMulti.hasNext()) {
        QMap<int, QString> chunks;
        bool hasPct = false;
        while (itMulti.hasNext()) {
            QRegularExpressionMatch m = itMulti.next();
            const int idx = m.captured(1).toInt();
            if (!m.captured(2).isEmpty())
                hasPct = true;
            const QString val = m.captured(3).isEmpty() ? m.captured(4).trimmed() : m.captured(3).trimmed();
            chunks.insert(idx, val);
        }
        QString joined;
        for (auto it = chunks.constBegin(); it != chunks.constEnd(); ++it)
            joined += it.value();
        if (!joined.isEmpty()) {
            const QString dec = hasPct ? decodeRfc2231Value(joined, partCharset)
                                       : GoogleProvider::decodeMimeHeader(joined);
            if (!dec.isEmpty())
                return dec;
        }
    }

    // 3. 표준 filename="..." / name="..." (RFC 2047 인코딩 또는 8bit 한글 포함)
    static const QRegularExpression rxStdParam(
        R"rx((?:filename|name)\s*=\s*(?:"([^"]+)"|([^;\r\n]+)))rx",
        QRegularExpression::CaseInsensitiveOption);
    QRegularExpressionMatch mStd = rxStdParam.match(combined);
    if (mStd.hasMatch()) {
        const QString raw = mStd.captured(1).isEmpty() ? mStd.captured(2).trimmed() : mStd.captured(1).trimmed();
        const QString dec = GoogleProvider::decodeMimeHeader(raw);
        if (!dec.isEmpty())
            return dec;
    }

    return QString();
}

void GoogleProvider::parseMimeEntity(const QByteArray &rawEntity,
                                     QString &outHtml,
                                     QString &outPlain,
                                     QString *outFrom,
                                     QString *outSubject,
                                     QString *outDate,
                                     QString *outTo,
                                     QList<AttachmentItem> *outAttachments,
                                     const QString &msgStorageKey,
                                     bool saveAttachmentDataToDisk,
                                     bool isTruncatedSlice)
{
    thread_local int s_mimeDepth = 0;
    thread_local QList<QPair<QString, QString>> s_pendingCidDataUris;
    thread_local bool s_hasTruncatedInlineCid = false;

    if (s_mimeDepth == 0) {
        s_pendingCidDataUris.clear();
        s_hasTruncatedInlineCid = false;
    }
    struct MimeDepthScope {
        int &depth;
        QString &html;
        QString &plain;
        QList<QPair<QString, QString>> &pendingCids;
        bool &truncatedCid;
        ~MimeDepthScope() {
            --depth;
            if (depth == 0) {
                for (const auto &kv : pendingCids) {
                    if (html.contains(kv.first, Qt::CaseInsensitive)) {
                        html.replace(kv.first, kv.second, Qt::CaseInsensitive);
                    }
                }
                if (truncatedCid && (!html.isEmpty() || !plain.isEmpty())
                    && !html.startsWith(QStringLiteral("<!--SM_PARTIAL-->"))) {
                    html.prepend(QStringLiteral("<!--SM_PARTIAL-->"));
                }
                pendingCids.clear();
                truncatedCid = false;
            }
        }
    } depthScope{s_mimeDepth, outHtml, outPlain, s_pendingCidDataUris, s_hasTruncatedInlineCid};
    ++s_mimeDepth;

    int headerEnd = rawEntity.indexOf("\r\n\r\n");
    int sepLen = 4;
    if (headerEnd < 0) {
        headerEnd = rawEntity.indexOf("\n\n");
        sepLen = 2;
    }
    if (headerEnd < 0) {
        QString txt = decodeCharsetBytes(rawEntity);
        if (outPlain.isEmpty())
            outPlain = txt;
        return;
    }

    QByteArray rawHeaders = rawEntity.left(headerEnd);
    QByteArray rawBody = rawEntity.mid(headerEnd + sepLen);

    // 멀티라인 헤더 Unfolding (연속된 탭/공백 접힘까지 완전히 1줄로 병합)
    while (rawHeaders.contains("\r\n\t") || rawHeaders.contains("\r\n ")
           || rawHeaders.contains("\n\t") || rawHeaders.contains("\n ")) {
        rawHeaders.replace("\r\n\t", " ");
        rawHeaders.replace("\r\n ", " ");
        rawHeaders.replace("\n\t", " ");
        rawHeaders.replace("\n ", " ");
    }

    QString contentType = "text/plain";
    QString contentTypeFullLatin1;
    QString contentDispositionLatin1;
    QString contentIdLatin1;
    QString charset = "utf-8";
    QString transferEncoding = "8bit";
    QString boundary;

    const QList<QByteArray> headerLines = rawHeaders.split('\n');
    for (QByteArray line : headerLines) {
        if (line.endsWith('\r'))
            line.chop(1);
        int colon = line.indexOf(':');
        if (colon <= 0)
            continue;
        QByteArray name = line.left(colon).trimmed().toLower();
        QByteArray valBytes = line.mid(colon + 1).trimmed();
        // Use fromLatin1 so raw 8-bit EUC-KR/UTF-8 bytes in headers are preserved for decodeMimeHeader
        QString valLatin1 = QString::fromLatin1(valBytes);

        if (name == "from" && outFrom && outFrom->isEmpty()) {
            *outFrom = decodeMimeHeader(valLatin1);
        } else if (name == "to" && outTo && outTo->isEmpty()) {
            *outTo = decodeMimeHeader(valLatin1);
        } else if (name == "subject" && outSubject && outSubject->isEmpty()) {
            *outSubject = decodeMimeHeader(valLatin1);
        } else if (name == "date" && outDate && outDate->isEmpty()) {
            *outDate = QString::fromLatin1(valBytes).trimmed();
        } else if (name == "content-type") {
            contentTypeFullLatin1 = valLatin1;
            contentType = valLatin1.section(';', 0, 0).trimmed().toLower();
            static QRegularExpression rxCharset(R"rx(charset\s*=\s*("?)([^";\s]+)\1)rx", QRegularExpression::CaseInsensitiveOption);
            QRegularExpressionMatch mCs = rxCharset.match(valLatin1);
            if (mCs.hasMatch())
                charset = mCs.captured(2).trimmed();

            static QRegularExpression rxBoundary(R"rx(boundary\s*=\s*(?:"([^"]+)"|([^;\s]+)))rx", QRegularExpression::CaseInsensitiveOption);
            QRegularExpressionMatch mB = rxBoundary.match(valLatin1);
            if (mB.hasMatch()) {
                boundary = mB.captured(1).isEmpty() ? mB.captured(2).trimmed() : mB.captured(1).trimmed();
            }
        } else if (name == "content-disposition") {
            contentDispositionLatin1 = valLatin1;
        } else if (name == "content-id") {
            contentIdLatin1 = valLatin1.trimmed();
            if (contentIdLatin1.startsWith('<'))
                contentIdLatin1.remove(0, 1);
            if (contentIdLatin1.endsWith('>'))
                contentIdLatin1.chop(1);
            contentIdLatin1 = contentIdLatin1.trimmed();
        } else if (name == "content-transfer-encoding") {
            transferEncoding = QString::fromLatin1(valBytes).section(';', 0, 0).trimmed().toLower();
        }
    }

    if (contentType.startsWith("multipart/") && !boundary.isEmpty()) {
        QByteArray delim = "--" + boundary.toLatin1();
        int pos = rawBody.indexOf(delim);
        while (pos >= 0) {
            int partStart = pos + delim.size();
            if (rawBody.mid(partStart, 2) == "--")
                break; // 종료 바운더리 (--boundary--)
            if (rawBody.mid(partStart, 2) == "\r\n")
                partStart += 2;
            else if (rawBody.mid(partStart, 1) == "\n")
                partStart += 1;

            int nextPos = rawBody.indexOf(delim, partStart);
            QByteArray partBytes = (nextPos >= 0) ? rawBody.mid(partStart, nextPos - partStart) : rawBody.mid(partStart);
            if (partBytes.endsWith("\r\n"))
                partBytes.chop(2);
            else if (partBytes.endsWith("\n"))
                partBytes.chop(1);

            const bool childTruncated = isTruncatedSlice && (nextPos < 0);
            parseMimeEntity(partBytes, outHtml, outPlain, nullptr, nullptr, nullptr, nullptr,
                            outAttachments, msgStorageKey, saveAttachmentDataToDisk, childTruncated);
            pos = nextPos;
        }
        return;
    }

    // 첨부파일 여부 판별 (Content-Disposition: attachment 또는 filename/name 파라미터 존재)
    const QString dispType = contentDispositionLatin1.section(';', 0, 0).trimmed().toLower();
    QString attFileName = extractMimeAttachmentFileName(contentDispositionLatin1, contentTypeFullLatin1, charset);
    const bool isInlineCidImage = (!contentIdLatin1.isEmpty() && contentType.startsWith("image/"));
    const bool isAttachmentPart = (dispType == "attachment")
                                  || (!attFileName.isEmpty() && dispType != "inline" && !isInlineCidImage)
                                  || (!attFileName.isEmpty() && contentType != "text/html" && contentType != "text/plain" && !isInlineCidImage);

    // 단일 파트 디코딩 (base64 / quoted-printable / 8bit)
    QByteArray decodedBody;
    if (!isTruncatedSlice || !isAttachmentPart) {
        if (transferEncoding == "base64") {
            QByteArray cleanB64;
            cleanB64.reserve(rawBody.size());
            for (char c : rawBody) {
                if (c != '\r' && c != '\n' && c != ' ' && c != '\t')
                    cleanB64.append(c);
            }
            decodedBody = QByteArray::fromBase64(cleanB64);
        } else if (transferEncoding == "quoted-printable") {
            decodedBody = decodeQuotedPrintable(rawBody);
        } else {
            decodedBody = rawBody;
        }
    }

    // RFC 2387 Content-ID (cid:) 인라인 이미지 처리 — HTML 본문/서명의 src="cid:..."를 data:image/...;base64,...로 즉시 치환
    if (isInlineCidImage) {
        if (isTruncatedSlice) {
            s_hasTruncatedInlineCid = true;
            return;
        }
        if (!decodedBody.isEmpty()) {
            const QString dataUri = QString("data:%1;base64,%2")
                                        .arg(contentType, QString::fromLatin1(decodedBody.toBase64()));
            const QString cidNeedle = QStringLiteral("cid:") + contentIdLatin1;
            s_pendingCidDataUris.append(qMakePair(cidNeedle, dataUri));
            if (outHtml.contains(cidNeedle, Qt::CaseInsensitive)) {
                outHtml.replace(cidNeedle, dataUri, Qt::CaseInsensitive);
            }
            return;
        }
    }

    if (isAttachmentPart) {
        if (outAttachments) {
            if (attFileName.isEmpty()) {
                attFileName = QString("attachment_%1.bin").arg(outAttachments->size() + 1);
            }
            // 파일명에 경로 구분자나 금지 문자가 있으면 안전하게 치환
            QString safeFileName = attFileName;
            safeFileName.replace(QRegularExpression(R"([\\/:*?"<>|])"), "_");
            if (safeFileName.trimmed().isEmpty())
                safeFileName = QStringLiteral("attachment.bin");

            AttachmentItem item;
            item.fileName = attFileName;
            if (isTruncatedSlice) {
                item.fileSize = QStringLiteral("열람 시 자동 다운로드");
                item.filePath.clear();
            } else {
                item.fileSize = formatMimeAttachmentBytes(decodedBody.size());
                if (saveAttachmentDataToDisk) {
                    QString baseDir = QCoreApplication::applicationDirPath();
                    const QString lowerDir = baseDir.toLower();
                    if (baseDir.isEmpty()
                        || lowerDir.contains(QStringLiteral(".app/contents/"))
                        || lowerDir.startsWith(QStringLiteral("/opt/sollunamail"))
                        || lowerDir.startsWith(QStringLiteral("/usr/"))
                        || lowerDir.contains(QStringLiteral("program files"))) {
                        baseDir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
                    }
                    if (baseDir.isEmpty())
                        baseDir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
                    if (baseDir.isEmpty())
                        baseDir = QDir::tempPath() + "/SollunaMail";
                    QString subFolder = msgStorageKey.trimmed().isEmpty()
                                            ? QStringLiteral("shared")
                                            : msgStorageKey.trimmed();
                    subFolder.replace(QRegularExpression(R"([^A-Za-z0-9_\-])"), "_");
                    const QString dirPath = baseDir + "/cache/attachments/" + subFolder;
                    QDir().mkpath(dirPath);
                    const QString fullFilePath = dirPath + "/" + safeFileName;
                    QFile outFile(fullFilePath);
                    if (outFile.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
                        outFile.write(decodedBody);
                        outFile.close();
                        item.filePath = fullFilePath;
                    }
                }
            }

            bool alreadyAdded = false;
            for (AttachmentItem &ex : *outAttachments) {
                if (ex.fileName.compare(item.fileName, Qt::CaseInsensitive) == 0) {
                    if (!item.filePath.isEmpty()) {
                        ex.filePath = item.filePath;
                        ex.fileSize = item.fileSize;
                    }
                    alreadyAdded = true;
                    break;
                }
            }
            if (!alreadyAdded) {
                outAttachments->append(item);
            }
        }
        return;
    }

    if (contentType == "text/html") {
        QString htmlText = decodeCharsetBytes(decodedBody, charset);
        if (!htmlText.trimmed().isEmpty()) {
            for (const auto &kv : s_pendingCidDataUris) {
                if (htmlText.contains(kv.first, Qt::CaseInsensitive)) {
                    htmlText.replace(kv.first, kv.second, Qt::CaseInsensitive);
                }
            }
            outHtml = htmlText;
        }
    } else if (contentType == "text/plain" || contentType.isEmpty()) {
        QString plainText = decodeCharsetBytes(decodedBody, charset);
        if (!plainText.trimmed().isEmpty() && outPlain.isEmpty())
            outPlain = plainText;
    }
}

QString GoogleProvider::sanitizeEmailHtmlForViewer(const QString &rawHtml, const QString &fallbackPlainText)
{
    static const QString kPartialPrefix = QStringLiteral("<!--SM_PARTIAL-->");
    bool isPartial = rawHtml.startsWith(kPartialPrefix);
    QString html = isPartial ? rawHtml.mid(kPartialPrefix.size()) : rawHtml;

    if (html.trimmed().isEmpty()) {
        QString escaped = fallbackPlainText.toHtmlEscaped();
        escaped.replace("\r\n", "<br/>").replace('\n', "<br/>");
        const QString wrappedPlain = QString("<div style=\"font-family:'Segoe UI','Malgun Gothic',sans-serif; font-size:13.5px; color:#1F2937; line-height:1.65; padding:16px;\">%1</div>")
                                         .arg(escaped);
        return isPartial ? (kPartialPrefix + wrappedPlain) : wrappedPlain;
    }

    // 1. 주석(MSO 조건부 주석 포함), <script>, <title>, <xml> 제거 (<style>은 Chromium WebEngine 렌더링을 위해 보존)
    html.remove(QRegularExpression(R"(<!--[\s\S]*?-->)"));
    html.remove(QRegularExpression(R"(<script\b[^>]*>[\s\S]*?</script>)", QRegularExpression::CaseInsensitiveOption));
    html.remove(QRegularExpression(R"(<title\b[^>]*>[\s\S]*?</title>)", QRegularExpression::CaseInsensitiveOption));
    html.remove(QRegularExpression(R"(<xml\b[^>]*>[\s\S]*?</xml>)", QRegularExpression::CaseInsensitiveOption));

    // 2. 이메일 상단 숨김 프리헤더(display:none / max-height:0 / mso-hide:all) 제거
    html.remove(QRegularExpression(R"(<div\b[^>]*style\s*=\s*["'][^"']*(?:display\s*:\s*none|max-height\s*:\s*0)[^"']*["'][^>]*>[\s\S]*?</div>)",
                                   QRegularExpression::CaseInsensitiveOption));
    html.remove(QRegularExpression(R"(<span\b[^>]*style\s*=\s*["'][^"']*(?:display\s*:\s*none|max-height\s*:\s*0)[^"']*["'][^>]*>[\s\S]*?</span>)",
                                   QRegularExpression::CaseInsensitiveOption));

    // 3. 마케팅 HTML 이메일(OpenAI, NVIDIA 등)의 여백용 font-size:0px / font-size:0 스타일을 제거하여
    //    QFont::setPixelSize(0) 경고를 방지하고 하위 텍스트가 1px로 작아지지 않도록 함
    html.remove(QRegularExpression(R"(font-size\s*:\s*0(?:\.0+)?\s*(?:px|pt|em|rem|%)?\s*;?)",
                                   QRegularExpression::CaseInsensitiveOption));

    // 4. 이메일 HTML 내의 font-family: -apple-system, system-ui, system 등으로 인해
    //    Windows DirectWrite가 레거시 비트맵 폰트("system") 로드를 시도하며 CreateFontFaceFromHDC() 경고를 출력하는 현상 방지
    html.replace(QRegularExpression(R"((?:-apple-system|BlinkMacSystemFont|system-ui|\bsystem\b))",
                                    QRegularExpression::CaseInsensitiveOption),
                 QStringLiteral("'Segoe UI'"));

    const QString wrappedHtml = QString("<div style=\"font-family:'Segoe UI','Malgun Gothic',sans-serif; font-size:13.5px; color:#1F2937; line-height:1.65; padding:12px;\">%1</div>")
                                    .arg(html);
    return isPartial ? (kPartialPrefix + wrappedHtml) : wrappedHtml;
}

QString GoogleProvider::htmlToPlainTextSnippet(const QString &html, const QString &plainText, int maxLen)
{
    auto cleanToPlain = [](QString s) -> QString {
        if (s.trimmed().isEmpty())
            return QString();
        s.remove(QRegularExpression(R"(<!--[\s\S]*?-->)"));
        s.remove(QRegularExpression(R"(<style\b[^>]*>[\s\S]*?</style>)", QRegularExpression::CaseInsensitiveOption));
        s.remove(QRegularExpression(R"(<script\b[^>]*>[\s\S]*?</script>)", QRegularExpression::CaseInsensitiveOption));
        s.remove(QRegularExpression(R"(<head\b[^>]*>[\s\S]*?</head>)", QRegularExpression::CaseInsensitiveOption));
        s.remove(QRegularExpression(R"(<title\b[^>]*>[\s\S]*?</title>)", QRegularExpression::CaseInsensitiveOption));
        s.remove(QRegularExpression(R"(<https?://[^>]+>)", QRegularExpression::CaseInsensitiveOption));
        s.remove(QRegularExpression(R"(\[\s*image\s*:[^\]]*\])", QRegularExpression::CaseInsensitiveOption));
        s.remove(QRegularExpression(R"(<[^>]+>)"));
        s.replace("&nbsp;", " ")
         .replace("&#8202;", " ")
         .replace("&#8203;", "")
         .replace("&#847;", "")
         .replace("&zwnj;", "")
         .replace("&zwj;", "")
         .replace("&amp;", "&")
         .replace("&lt;", "<")
         .replace("&gt;", ">")
         .replace("&quot;", "\"")
         .replace("&apos;", "'")
         .replace("&#39;", "'")
         .replace("&middot;", "·")
         .replace("&bull;", "•")
         .replace("&ndash;", "–")
         .replace("&mdash;", "—")
         .replace("&copy;", "©")
         .replace("&reg;", "®")
         .replace("&trade;", "™");

        // 10진수(&#45576;) 및 16진수(&#xAC00;) 유니코드 숫자 엔티티를 실제 한글/유니코드 문자로 변환
        static const QRegularExpression numEntityRe(
            R"(&#(?:x([0-9a-fA-F]{1,6})|(\d{1,7}));?)",
            QRegularExpression::CaseInsensitiveOption);
        if (s.contains(QLatin1String("&#"))) {
            QString decoded;
            int lastPos = 0;
            QRegularExpressionMatchIterator it = numEntityRe.globalMatch(s);
            while (it.hasNext()) {
                const QRegularExpressionMatch m = it.next();
                decoded.append(s.mid(lastPos, m.capturedStart() - lastPos));
                bool ok = false;
                uint codePoint = 0;
                if (!m.captured(1).isEmpty())
                    codePoint = m.captured(1).toUInt(&ok, 16);
                else
                    codePoint = m.captured(2).toUInt(&ok, 10);
                if (ok && codePoint > 0 && codePoint <= 0x10FFFF) {
                    if (codePoint == 0x200B || codePoint == 0x200C || codePoint == 0x200D
                        || codePoint == 0xFEFF || codePoint == 0x034F) {
                        // Zero-width space/joiners in marketing preheaders -> skip
                    } else if (codePoint == 160 || codePoint == 8202 || codePoint == 8199) {
                        decoded.append(QLatin1Char(' '));
                    } else {
                        const char32_t cp = static_cast<char32_t>(codePoint);
                        decoded.append(QString::fromUcs4(&cp, 1));
                    }
                } else {
                    decoded.append(m.captured(0));
                }
                lastPos = m.capturedEnd();
            }
            decoded.append(s.mid(lastPos));
            s = decoded;
        }
        // 잘린 마지막 엔티티 조각(&#4543... 등) 제거
        s.remove(QRegularExpression(R"(&#(?:x[0-9a-fA-F]*|\d*)$)"));
        s.replace(QRegularExpression(R"(\s+)"), " ");
        s.remove(QRegularExpression(R"(^(?:\(\s*\)\s*|[=\-*_]{1,}\s*|\[\s*\]\s*)+)"));
        return s.trimmed();
    };

    QString src = cleanToPlain(plainText);
    if (src.isEmpty() || src.startsWith('<')) {
        src = cleanToPlain(html);
    }
    if (src.size() > maxLen)
        return src.left(maxLen).trimmed() + "...";
    return src;
}

// Google OAuth Client ID & Secret (공식 SollunaMail OAuth 2.0 클라이언트 ID/시크릿)
// - GitHub 업로드 시 평문 키 노출 및 GitHub Push Protection 차단을 방지하기 위해
//   실제 키는 .gitignore로 제외된 oauth_secrets_local.h 내부에 SHA-256 키스트림 해시(Hex) 형태로 보관하고 런타임에 복원함.
namespace {
QString decodeOAuthSha256StreamHex(const QByteArray &cipherHex, const QByteArray &salt)
{
    const QByteArray cipher = QByteArray::fromHex(cipherHex.trimmed());
    if (cipher.isEmpty())
        return QString();
    QByteArray out(cipher.size(), Qt::Uninitialized);
    int offset = 0;
    int blockIdx = 0;
    while (offset < cipher.size()) {
        const QByteArray seed = salt + '#' + QByteArray::number(blockIdx);
        const QByteArray keystream = QCryptographicHash::hash(seed, QCryptographicHash::Sha256);
        for (int i = 0; i < keystream.size() && offset < cipher.size(); ++i, ++offset) {
            out[offset] = static_cast<char>(
                static_cast<unsigned char>(cipher[offset]) ^ static_cast<unsigned char>(keystream[i]));
        }
        ++blockIdx;
    }
    return QString::fromUtf8(out).trimmed();
}
} // namespace

QString GoogleProvider::builtInGoogleClientId()
{
    if (!MailI18n::customGoogleClientId().trimmed().isEmpty()) {
        return MailI18n::customGoogleClientId().trimmed();
    }
    const QString envId = qEnvironmentVariable("SOLLUNAMAIL_GOOGLE_CLIENT_ID").trimmed();
    if (!envId.isEmpty()) {
        return envId;
    }
#ifdef SOLLUNAMAIL_OAUTH_CLIENT_ID_PLAIN
    const QString plainId = QStringLiteral(SOLLUNAMAIL_OAUTH_CLIENT_ID_PLAIN).trimmed();
    if (!plainId.isEmpty())
        return plainId;
#endif
#ifdef SOLLUNAMAIL_OAUTH_CLIENT_ID_CIPHER_HEX
    const QString decodedId = decodeOAuthSha256StreamHex(
        QByteArrayLiteral(SOLLUNAMAIL_OAUTH_CLIENT_ID_CIPHER_HEX),
        QByteArrayLiteral("SollunaMail::OAuth2::ClientId::v1::2026"));
    if (!decodedId.isEmpty())
        return decodedId;
#endif
    return QString();
}

QString GoogleProvider::builtInGoogleClientSecret()
{
    if (!MailI18n::customGoogleClientSecret().trimmed().isEmpty()) {
        return MailI18n::customGoogleClientSecret().trimmed();
    }
    const QString envSecret = qEnvironmentVariable("SOLLUNAMAIL_GOOGLE_CLIENT_SECRET").trimmed();
    if (!envSecret.isEmpty()) {
        return envSecret;
    }
#ifdef SOLLUNAMAIL_OAUTH_CLIENT_SECRET_PLAIN
    const QString plainSecret = QStringLiteral(SOLLUNAMAIL_OAUTH_CLIENT_SECRET_PLAIN).trimmed();
    if (!plainSecret.isEmpty())
        return plainSecret;
#endif
#ifdef SOLLUNAMAIL_OAUTH_CLIENT_SECRET_CIPHER_HEX
    const QString decodedSecret = decodeOAuthSha256StreamHex(
        QByteArrayLiteral(SOLLUNAMAIL_OAUTH_CLIENT_SECRET_CIPHER_HEX),
        QByteArrayLiteral("SollunaMail::OAuth2::ClientSecret::v1::2026"));
    if (!decodedSecret.isEmpty())
        return decodedSecret;
#endif
    return QString();
}

void GoogleProvider::startBuiltInGoogleLogin(const QString &loginHintEmail)
{
    startGoogleOAuthLogin(builtInGoogleClientId(), builtInGoogleClientSecret(), loginHintEmail);
}

void GoogleProvider::startGoogleOAuthLogin(const QString &clientId,
                                           const QString &clientSecret,
                                           const QString &loginHintEmail)
{
    m_clientId = clientId.trimmed().isEmpty() ? builtInGoogleClientId() : clientId.trimmed();
    m_clientSecret = clientSecret.trimmed().isEmpty() ? builtInGoogleClientSecret() : clientSecret.trimmed();
    m_loginHint = loginHintEmail.trimmed();

    if (m_clientId.isEmpty()) {
        emit statusMessage(MailI18n::t(
            "Google OAuth Client ID가 설정되지 않았습니다 (oauth_secrets_local.h 포함 후 재빌드 필요)",
            "Google OAuth Client ID is missing (rebuild with oauth_secrets_local.h)",
            "Google OAuth Client ID が設定されていません"));
        return;
    }

    if (m_loopbackServer) {
        m_loopbackServer->close();
        m_loopbackServer->deleteLater();
    }
    m_loopbackServer = new QTcpServer(this);
    if (!m_loopbackServer->listen(QHostAddress::LocalHost, 0)) {
        emit statusMessage(MailI18n::t("Google OAuth 로컬 루프백 포트 오픈 실패",
                                       "Failed to open local loopback port for Google OAuth",
                                       "Google OAuth ローカルポートのオープンに失敗しました"));
        return;
    }

    quint16 port = m_loopbackServer->serverPort();
    // Local loopback redirect URI: "http://127.0.0.1:<port>" (without trailing slash)
    QString redirectUri = QString("http://127.0.0.1:%1").arg(port);

    connect(m_loopbackServer, &QTcpServer::newConnection, this, [this, redirectUri]() {
        QTcpSocket *client = m_loopbackServer->nextPendingConnection();
        if (!client)
            return;
        connect(client, &QTcpSocket::readyRead, this, [this, client, redirectUri]() {
            QString req = QString::fromUtf8(client->readAll());
            QString firstLine = req.section("\r\n", 0, 0);
            QString pathPart = firstLine.section(' ', 1, 1);
            QUrl url("http://127.0.0.1" + pathPart);
            QUrlQuery query(url);
            QString code = query.queryItemValue("code");

            QByteArray html =
                "HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\n\r\n"
                "<html><body style='font-family:Segoe UI,Malgun Gothic,sans-serif;text-align:center;padding-top:70px;background:#F8FAFC;'>"
                "<div style='display:inline-block;background:#FFFFFF;padding:36px 48px;border-radius:14px;box-shadow:0 6px 24px rgba(0,0,0,0.08);'>"
                "<h2 style='color:#0D47A1;margin-top:0;'>✓ SollunaMail Google 계정 연동 완료!</h2>"
                "<p style='color:#4B5563;font-size:14px;'>Google 메일, 캘린더, 주소록 연동이 완료되었습니다.<br/>이 브라우저 창을 닫고 SollunaMail로 돌아가세요.</p>"
                "</div></body></html>";
            client->write(html);
            client->flush();
            client->disconnectFromHost();

            m_loopbackServer->close();
            if (!code.isEmpty()) {
                exchangeCodeForToken(code, redirectUri);
            } else {
                emit statusMessage(MailI18n::t("Google 로그인이 취소되었거나 인증 코드가 수신되지 않았습니다.",
                                               "Google sign-in was cancelled or no authorization code was received.",
                                               "Google ログインがキャンセルされたか、認証コードを受信できませんでした。"));
            }
        });
    });

    // Build Google OAuth 2.0 authorization URL with required Gmail, Contacts, and Calendar scopes:
    QUrl authUrl("https://accounts.google.com/o/oauth2/v2/auth");
    QUrlQuery q;
    q.addQueryItem("client_id", m_clientId);
    q.addQueryItem("scope", "openid email profile https://mail.google.com/ https://www.google.com/m8/feeds https://www.googleapis.com/auth/contacts.readonly https://www.googleapis.com/auth/contacts.other.readonly https://www.googleapis.com/auth/calendar");
    q.addQueryItem("response_type", "code");
    q.addQueryItem("redirect_uri", redirectUri);
    q.addQueryItem("access_type", "offline");
    if (!m_loginHint.isEmpty()) {
        q.addQueryItem("login_hint", m_loginHint);
    } else {
        q.addQueryItem("prompt", "select_account");
    }
    authUrl.setQuery(q);

    emit statusMessage(MailI18n::t("웹 브라우저에서 Google 계정 로그인 창을 열었습니다. 브라우저에서 로그인을 완료해 주세요...",
                                   "Opened Google sign-in in your web browser. Please complete sign-in in the browser...",
                                   "Webブラウザで Google ログイン画面を開きました。ブラウザでログインを完了してください..."));
    QDesktopServices::openUrl(authUrl);
}

void GoogleProvider::exchangeCodeForToken(const QString &code, const QString &redirectUri)
{
    QNetworkRequest req(QUrl("https://oauth2.googleapis.com/token"));
    req.setHeader(QNetworkRequest::ContentTypeHeader, "application/x-www-form-urlencoded");

    QUrlQuery body;
    body.addQueryItem("code", code);
    body.addQueryItem("client_id", m_clientId);
    body.addQueryItem("client_secret", m_clientSecret);
    body.addQueryItem("redirect_uri", redirectUri);
    body.addQueryItem("grant_type", "authorization_code");

    QNetworkReply *reply = m_nam->post(req, body.toString(QUrl::FullyEncoded).toUtf8());
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        reply->deleteLater();
        QJsonDocument doc = QJsonDocument::fromJson(reply->readAll());
        QJsonObject obj = doc.object();
        QString accessToken = obj.value("access_token").toString();
        QString refreshToken = obj.value("refresh_token").toString();
        QString idToken = obj.value("id_token").toString();

        // id_token (JWT) 2번째 파트에서 email / name 클레임 즉시 추출하여 백업 확보
        if (!idToken.isEmpty()) {
            QStringList jwtParts = idToken.split('.');
            if (jwtParts.size() >= 2) {
                QByteArray payloadBytes = QByteArray::fromBase64(jwtParts[1].toLatin1(), QByteArray::Base64UrlEncoding);
                QJsonObject claims = QJsonDocument::fromJson(payloadBytes).object();
                QString jwtEmail = claims.value("email").toString().trimmed();
                if (!jwtEmail.isEmpty())
                    m_loginHint = jwtEmail;
            }
        }

        if (!accessToken.isEmpty()) {
            emit statusMessage(MailI18n::t("Google 계정 인증 완료! 사용자 프로필 정보를 가져오는 중...",
                                           "Google account authenticated! Fetching user profile...",
                                           "Google アカウント認証完了！ユーザープロフィールを取得中..."));
            fetchGoogleUserProfile(accessToken, refreshToken);
        } else {
            emit statusMessage(MailI18n::t("Google 토큰 교환 실패: ",
                                           "Google token exchange failed: ",
                                           "Google トークン交換失敗: ")
                               + obj.value("error_description").toString());
        }
    });
}

void GoogleProvider::fetchGoogleUserProfile(const QString &accessToken, const QString &refreshToken)
{
    QNetworkRequest req(QUrl("https://openidconnect.googleapis.com/v1/userinfo"));
    req.setRawHeader("Authorization", ("Bearer " + accessToken.trimmed()).toUtf8());

    QNetworkReply *reply = m_nam->get(req);
    connect(reply, &QNetworkReply::finished, this, [this, reply, accessToken, refreshToken]() {
        reply->deleteLater();
        QJsonDocument doc = QJsonDocument::fromJson(reply->readAll());
        QJsonObject obj = doc.object();
        QString email = obj.value("email").toString().trimmed();
        QString name = obj.value("name").toString().trimmed();
        QString pictureUrl = obj.value("picture").toString().trimmed();
        if (email.isEmpty())
            email = m_loginHint;
        if (name.isEmpty() && !email.isEmpty())
            name = email.section('@', 0, 0);

        emit statusMessage(MailI18n::t(QString("Google 계정 연동 성공: %1 (%2)"),
                                       QString("Google account connected: %1 (%2)"),
                                       QString("Google アカウント連携成功: %1 (%2)"))
                               .arg(name, email));
        emit oauthTokenReceived(email, accessToken, refreshToken);
        emit googleProfileReady(email, name, accessToken, refreshToken);

        // Google 프로필 사진 URL이 있으면 즉시 다운로드하여 아바타로 적용
        if (!pictureUrl.isEmpty() && !email.isEmpty()) {
            QNetworkRequest imgReq((QUrl(pictureUrl)));
            imgReq.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
            QNetworkReply *imgReply = m_nam->get(imgReq);
            connect(imgReply, &QNetworkReply::finished, this, [this, imgReply, email]() {
                imgReply->deleteLater();
                if (imgReply->error() == QNetworkReply::NoError) {
                    QImage img = QImage::fromData(imgReply->readAll());
                    if (!img.isNull()) {
                        QImage scaled = img.scaled(96, 96, Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation);
                        QByteArray pngBytes;
                        QBuffer buf(&pngBytes);
                        buf.open(QIODevice::WriteOnly);
                        scaled.save(&buf, "PNG");
                        emit accountAvatarFetched(email, QString::fromLatin1(pngBytes.toBase64()));
                    }
                }
            });
        }
    });
}

void GoogleProvider::fetchAccountAvatar(const AccountItem &account)
{
    if (!MailI18n::privacyDownloadAvatars())
        return;
    if (account.provider == AccountProviderType::Unified || account.email.trimmed().isEmpty())
        return;

    const QString email = account.email.trimmed();

    // 1. Google 계정: OAuth 토큰으로 UserInfo API에서 프로필 사진(picture) URL 조회 후 다운로드
    if (account.provider == AccountProviderType::Google) {
        if (account.oauthAccessToken.trimmed().isEmpty()) {
            if (!account.oauthRefreshToken.trimmed().isEmpty()) {
                refreshGoogleAccessToken(email, account.oauthRefreshToken);
            }
            return;
        }
        QNetworkRequest req(QUrl("https://www.googleapis.com/oauth2/v2/userinfo"));
        req.setRawHeader("Authorization", ("Bearer " + account.oauthAccessToken.trimmed()).toUtf8());
        QNetworkReply *reply = m_nam->get(req);
        connect(reply, &QNetworkReply::finished, this, [this, reply, email, account]() {
            reply->deleteLater();
            const int httpCode = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            if (httpCode == 401 && !account.oauthRefreshToken.trimmed().isEmpty()) {
                refreshGoogleAccessToken(email, account.oauthRefreshToken);
                return;
            }
            if (reply->error() != QNetworkReply::NoError)
                return;
            QJsonObject obj = QJsonDocument::fromJson(reply->readAll()).object();
            QString pictureUrl = obj.value("picture").toString().trimmed();
            if (pictureUrl.isEmpty())
                return;

            QNetworkRequest imgReq((QUrl(pictureUrl)));
            imgReq.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
            QNetworkReply *imgReply = m_nam->get(imgReq);
            connect(imgReply, &QNetworkReply::finished, this, [this, imgReply, email]() {
                imgReply->deleteLater();
                if (imgReply->error() == QNetworkReply::NoError) {
                    QImage img = QImage::fromData(imgReply->readAll());
                    if (!img.isNull()) {
                        QImage scaled = img.scaled(96, 96, Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation);
                        QByteArray pngBytes;
                        QBuffer buf(&pngBytes);
                        buf.open(QIODevice::WriteOnly);
                        scaled.save(&buf, "PNG");
                        emit accountAvatarFetched(email, QString::fromLatin1(pngBytes.toBase64()));
                    }
                }
            });
        });
        return;
    }

    // 2. 일반 회사 / 도메인 계정: 이메일 도메인(예: solluna-iot.com)의 파비콘(Favicon) 다운로드
    QString domain = email.section('@', 1, 1).trimmed().toLower();
    if (domain.isEmpty())
        return;

    QStringList candidateUrls = {
        QString("https://www.google.com/s2/favicons?domain=%1&sz=64").arg(domain),
        QString("https://icons.duckduckgo.com/ip3/%1.ico").arg(domain),
        QString("https://%1/favicon.ico").arg(domain),
        QString("https://www.%1/favicon.ico").arg(domain)
    };

    auto tryFetchFavicon = std::make_shared<std::function<void(int)>>();
    *tryFetchFavicon = [this, email, candidateUrls, tryFetchFavicon](int idx) {
        if (idx >= candidateUrls.size())
            return;
        QNetworkRequest req((QUrl(candidateUrls[idx])));
        req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
        req.setRawHeader("User-Agent", "Mozilla/5.0 (Windows NT 10.0; Win64; x64) SollunaMail/1.0");
        QNetworkReply *reply = m_nam->get(req);
        connect(reply, &QNetworkReply::finished, this, [this, reply, email, idx, tryFetchFavicon]() {
            reply->deleteLater();
            if (reply->error() == QNetworkReply::NoError) {
                QImage img = QImage::fromData(reply->readAll());
                if (!img.isNull() && img.width() >= 8 && img.height() >= 8) {
                    QImage scaled = img.scaled(64, 64, Qt::KeepAspectRatio, Qt::SmoothTransformation);
                    QByteArray pngBytes;
                    QBuffer buf(&pngBytes);
                    buf.open(QIODevice::WriteOnly);
                    scaled.save(&buf, "PNG");
                    emit accountAvatarFetched(email, QString::fromLatin1(pngBytes.toBase64()));
                    return;
                }
            }
            (*tryFetchFavicon)(idx + 1);
        });
    };
    (*tryFetchFavicon)(0);
}

void GoogleProvider::refreshGoogleAccessToken(const QString &accountEmail, const QString &refreshToken)
{
    if (refreshToken.trimmed().isEmpty())
        return;

    QUrl tokenUrl("https://oauth2.googleapis.com/token");
    QNetworkRequest req(tokenUrl);
    req.setHeader(QNetworkRequest::ContentTypeHeader, "application/x-www-form-urlencoded");

    QUrlQuery params;
    params.addQueryItem("client_id", builtInGoogleClientId());
    params.addQueryItem("client_secret", builtInGoogleClientSecret());
    params.addQueryItem("refresh_token", refreshToken.trimmed());
    params.addQueryItem("grant_type", "refresh_token");

    QNetworkReply *reply = m_nam->post(req, params.toString(QUrl::FullyEncoded).toUtf8());
    connect(reply, &QNetworkReply::finished, this, [this, reply, accountEmail, refreshToken]() {
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError) {
            return;
        }
        QJsonObject obj = QJsonDocument::fromJson(reply->readAll()).object();
        QString newAccess = obj.value("access_token").toString().trimmed();
        if (!newAccess.isEmpty()) {
            emit oauthTokenReceived(accountEmail, newAccess, refreshToken);
        }
    });
}

void GoogleProvider::syncGoogleCalendarEvents(const QString &accountEmail,
                                              const QString &accessToken,
                                              const QString &refreshToken)
{
    if (accessToken.trimmed().isEmpty()) {
        if (!refreshToken.trimmed().isEmpty()) {
            refreshGoogleAccessToken(accountEmail, refreshToken);
        } else {
            emit statusMessage(MailI18n::t("Google Calendar API 동기화: OAuth 액세스 토큰이 필요합니다.",
                                           "Google Calendar sync: OAuth access token is required.",
                                           "Google カレンダー同期: OAuth アクセストークンが必要です。"));
        }
        return;
    }

    // 1단계: 사용자의 전체 하위 캘린더 목록(calendarList) 조회
    QUrl listUrl("https://www.googleapis.com/calendar/v3/users/me/calendarList?maxResults=50");
    QNetworkRequest listReq(listUrl);
    listReq.setRawHeader("Authorization", ("Bearer " + accessToken.trimmed()).toUtf8());

    QNetworkReply *listReply = m_nam->get(listReq);
    connect(listReply, &QNetworkReply::finished, this, [this, listReply, accountEmail, accessToken, refreshToken]() {
        listReply->deleteLater();
        const int httpCode = listReply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (httpCode == 401 && !refreshToken.trimmed().isEmpty()) {
            refreshGoogleAccessToken(accountEmail, refreshToken);
            return;
        }

        QList<CalendarSubItem> subCalendars;
        QStringList calendarIdsToFetch;

        if (listReply->error() == QNetworkReply::NoError) {
            QJsonDocument listDoc = QJsonDocument::fromJson(listReply->readAll());
            QJsonArray calItems = listDoc.object().value("items").toArray();
            for (const QJsonValue &cv : calItems) {
                QJsonObject cObj = cv.toObject();
                QString calId = cObj.value("id").toString().trimmed();
                if (calId.isEmpty())
                    continue;
                CalendarSubItem sub;
                sub.id = calId;
                sub.accountEmail = accountEmail.toUpper();
                sub.name = cObj.value("summaryOverride").toString().trimmed();
                if (sub.name.isEmpty())
                    sub.name = cObj.value("summary").toString(calId).trimmed();
                QString bgHex = cObj.value("backgroundColor").toString("#0D47A1");
                sub.color = QColor(bgHex.isEmpty() ? "#0D47A1" : bgHex);
                sub.isVisible = cObj.value("selected").toBool(true);
                sub.isDefault = cObj.value("primary").toBool(false);
                sub.showEyeToggle = !sub.isDefault;
                subCalendars.append(sub);
                calendarIdsToFetch.append(calId);
            }
        }

        if (subCalendars.isEmpty()) {
            CalendarSubItem primarySub;
            primarySub.id = "primary";
            primarySub.accountEmail = accountEmail.toUpper();
            primarySub.name = accountEmail;
            primarySub.color = QColor("#D96B62");
            primarySub.isVisible = true;
            primarySub.isDefault = true;
            primarySub.showEyeToggle = false;
            subCalendars.append(primarySub);
            calendarIdsToFetch.append("primary");
        }

        // 2단계: 각 하위 캘린더별로 2025~2027년 일정(timeMin ~ timeMax, singleEvents=true, orderBy=startTime) 조회
        auto remaining = QSharedPointer<int>::create(calendarIdsToFetch.size());
        auto allEvents = QSharedPointer<QList<CalendarEventItem>>::create();

        for (int idx = 0; idx < calendarIdsToFetch.size(); ++idx) {
            const QString calId = calendarIdsToFetch[idx];
            const CalendarSubItem subInfo = subCalendars[idx];

            QString encodedCalId = QString::fromLatin1(QUrl::toPercentEncoding(calId));
            QUrl evUrl(QString("https://www.googleapis.com/calendar/v3/calendars/%1/events"
                               "?timeMin=2025-01-01T00:00:00Z"
                               "&timeMax=2027-12-31T23:59:59Z"
                               "&maxResults=250"
                               "&singleEvents=true"
                               "&orderBy=startTime")
                           .arg(encodedCalId));
            QNetworkRequest evReq(evUrl);
            evReq.setRawHeader("Authorization", ("Bearer " + accessToken.trimmed()).toUtf8());

            QNetworkReply *evReply = m_nam->get(evReq);
            connect(evReply, &QNetworkReply::finished, this,
                    [this, evReply, accountEmail, subCalendars, subInfo, remaining, allEvents]() {
                evReply->deleteLater();
                if (evReply->error() == QNetworkReply::NoError) {
                    QJsonDocument doc = QJsonDocument::fromJson(evReply->readAll());
                    QJsonArray items = doc.object().value("items").toArray();

                    QColor barCol = subInfo.color.isValid() ? subInfo.color : QColor("#0D47A1");
                    QColor bgCol = barCol.lighter(175);
                    bgCol.setAlpha(255);
                    QColor txtCol = barCol.darker(160);

                    for (const QJsonValue &v : items) {
                        QJsonObject evObj = v.toObject();
                        if (evObj.value("status").toString() == "cancelled")
                            continue;

                        CalendarEventItem baseEv;
                        baseEv.id = evObj.value("id").toString();
                        baseEv.accountEmail = accountEmail.toUpper();
                        baseEv.calendarId = subInfo.id;
                        baseEv.title = evObj.value("summary").toString(MailI18n::t("(제목 없음)", "(No Title)", "(件名なし)"));
                        baseEv.location = evObj.value("location").toString();
                        baseEv.description = evObj.value("description").toString();
                        baseEv.meetUrl = evObj.value("hangoutLink").toString();
                        baseEv.pillBgColor = bgCol;
                        baseEv.pillBarColor = barCol;
                        baseEv.pillTextColor = txtCol;

                        QJsonObject startObj = evObj.value("start").toObject();
                        QJsonObject endObj = evObj.value("end").toObject();

                        if (startObj.contains("date")) {
                            QDate startDate = QDate::fromString(startObj.value("date").toString(), Qt::ISODate);
                            QDate endDate = QDate::fromString(endObj.value("date").toString(), Qt::ISODate);
                            baseEv.isAllDay = true;
                            baseEv.startTime = QTime(0, 0);
                            baseEv.endTime = QTime(23, 59);
                            if (startDate.isValid()) {
                                int daysSpan = (endDate.isValid() && endDate > startDate)
                                                   ? qMin(startDate.daysTo(endDate), 14)
                                                   : 1;
                                for (int dOffset = 0; dOffset < daysSpan; ++dOffset) {
                                    CalendarEventItem dayEv = baseEv;
                                    dayEv.date = startDate.addDays(dOffset);
                                    allEvents->append(dayEv);
                                }
                            }
                        } else {
                            QDateTime startDt = QDateTime::fromString(startObj.value("dateTime").toString(), Qt::ISODate).toLocalTime();
                            QDateTime endDt = QDateTime::fromString(endObj.value("dateTime").toString(), Qt::ISODate).toLocalTime();
                            baseEv.date = startDt.date();
                            baseEv.startTime = startDt.time();
                            baseEv.endTime = endDt.isValid() ? endDt.time() : startDt.time().addSecs(3600);
                            baseEv.isAllDay = false;
                            if (baseEv.date.isValid()) {
                                allEvents->append(baseEv);
                            }
                        }
                    }
                }

                *remaining -= 1;
                if (*remaining <= 0) {
                    QList<CalendarEventItem> finalEvents = *allEvents;
                    emit statusMessage(
                        MailI18n::t(QString("Google Calendar (%1) 캘린더 %2개 / 일정 %3건 동기화 완료"),
                                    QString("Google Calendar (%1) synced %2 calendars / %3 events"),
                                    QString("Google カレンダー (%1) カレンダー%2個 / 予定%3件の同期完了"))
                            .arg(accountEmail)
                            .arg(subCalendars.size())
                            .arg(finalEvents.size()));
                    emit googleCalendarsFetched(accountEmail, subCalendars, finalEvents);
                }
            });
        }
    });
}

void GoogleProvider::createGoogleCalendarEvent(const QString &accountEmail,
                                               const QString &accessToken,
                                               const CalendarEventItem &event)
{
    if (accessToken.trimmed().isEmpty())
        return;

    QString calId = event.calendarId.trimmed().isEmpty() ? QStringLiteral("primary") : event.calendarId.trimmed();
    if (calId.startsWith("cal_local", Qt::CaseInsensitive) || calId.contains("#holiday", Qt::CaseInsensitive))
        calId = QStringLiteral("primary");

    QString encodedCalId = QString::fromLatin1(QUrl::toPercentEncoding(calId));
    QUrl url(QString("https://www.googleapis.com/calendar/v3/calendars/%1/events").arg(encodedCalId));

    QNetworkRequest req(url);
    req.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    req.setRawHeader("Authorization", ("Bearer " + accessToken.trimmed()).toUtf8());

    QJsonObject body;
    body["summary"] = event.title;
    if (!event.location.isEmpty())
        body["location"] = event.location;
    if (!event.description.isEmpty())
        body["description"] = event.description;

    QJsonObject startObj;
    QJsonObject endObj;
    if (event.isAllDay) {
        startObj["date"] = event.date.toString(Qt::ISODate);
        endObj["date"] = event.date.addDays(1).toString(Qt::ISODate);
    } else {
        QDateTime startDt(event.date, event.startTime);
        QDateTime endDt(event.date, event.endTime > event.startTime ? event.endTime : event.startTime.addSecs(3600));
        startObj["dateTime"] = startDt.toOffsetFromUtc(startDt.offsetFromUtc()).toString(Qt::ISODate);
        endObj["dateTime"] = endDt.toOffsetFromUtc(endDt.offsetFromUtc()).toString(Qt::ISODate);
    }
    body["start"] = startObj;
    body["end"] = endObj;

    QNetworkReply *reply = m_nam->post(req, QJsonDocument(body).toJson(QJsonDocument::Compact));
    connect(reply, &QNetworkReply::finished, this, [this, reply, accountEmail, event]() {
        reply->deleteLater();
        if (reply->error() == QNetworkReply::NoError) {
            emit statusMessage(MailI18n::t(QString("Google Calendar (%1) 새 일정 등록 완료: %2"),
                                           QString("Google Calendar (%1) event created: %2"),
                                           QString("Google カレンダー (%1) 新しい予定を登録しました: %2"))
                                   .arg(accountEmail, event.title));
        } else {
            emit statusMessage(MailI18n::t(QString("로컬 캘린더에 일정 저장됨: %1"),
                                           QString("Saved event to local calendar: %1"),
                                           QString("ローカルカレンダーに予定を保存しました: %1"))
                                   .arg(event.title));
        }
    });
}

void GoogleProvider::updateGoogleCalendarEvent(const QString &accountEmail,
                                               const QString &accessToken,
                                               const CalendarEventItem &event)
{
    if (accessToken.trimmed().isEmpty() || event.id.trimmed().isEmpty() || event.id.startsWith("local_ev_"))
        return;

    QString calId = event.calendarId.trimmed().isEmpty() ? QStringLiteral("primary") : event.calendarId.trimmed();
    QString encodedCalId = QString::fromLatin1(QUrl::toPercentEncoding(calId));
    QString encodedEvId = QString::fromLatin1(QUrl::toPercentEncoding(event.id.trimmed()));
    QUrl url(QString("https://www.googleapis.com/calendar/v3/calendars/%1/events/%2").arg(encodedCalId, encodedEvId));

    QNetworkRequest req(url);
    req.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    req.setRawHeader("Authorization", ("Bearer " + accessToken.trimmed()).toUtf8());

    QJsonObject body;
    body["summary"] = event.title;
    body["location"] = event.location;
    body["description"] = event.description;

    QJsonObject startObj;
    QJsonObject endObj;
    if (event.isAllDay) {
        startObj["date"] = event.date.toString(Qt::ISODate);
        endObj["date"] = event.date.addDays(1).toString(Qt::ISODate);
    } else {
        QDateTime startDt(event.date, event.startTime);
        QDateTime endDt(event.date, event.endTime > event.startTime ? event.endTime : event.startTime.addSecs(3600));
        startObj["dateTime"] = startDt.toOffsetFromUtc(startDt.offsetFromUtc()).toString(Qt::ISODate);
        endObj["dateTime"] = endDt.toOffsetFromUtc(endDt.offsetFromUtc()).toString(Qt::ISODate);
    }
    body["start"] = startObj;
    body["end"] = endObj;

    QNetworkReply *reply = m_nam->sendCustomRequest(req, "PATCH", QJsonDocument(body).toJson(QJsonDocument::Compact));
    connect(reply, &QNetworkReply::finished, this, [this, reply, accountEmail, event]() {
        reply->deleteLater();
        if (reply->error() == QNetworkReply::NoError) {
            emit statusMessage(MailI18n::t(QString("Google Calendar (%1) 일정 수정 완료: %2"),
                                           QString("Google Calendar (%1) event updated: %2"),
                                           QString("Google カレンダー (%1) 予定を更新しました: %2"))
                                   .arg(accountEmail, event.title));
        }
    });
}

void GoogleProvider::deleteGoogleCalendarEvent(const QString &accountEmail,
                                               const QString &accessToken,
                                               const QString &calendarId,
                                               const QString &eventId)
{
    if (accessToken.trimmed().isEmpty() || eventId.trimmed().isEmpty() || eventId.startsWith("local_ev_"))
        return;

    QString calId = calendarId.trimmed().isEmpty() ? QStringLiteral("primary") : calendarId.trimmed();
    QString encodedCalId = QString::fromLatin1(QUrl::toPercentEncoding(calId));
    QString encodedEvId = QString::fromLatin1(QUrl::toPercentEncoding(eventId.trimmed()));
    QUrl url(QString("https://www.googleapis.com/calendar/v3/calendars/%1/events/%2").arg(encodedCalId, encodedEvId));

    QNetworkRequest req(url);
    req.setRawHeader("Authorization", ("Bearer " + accessToken.trimmed()).toUtf8());

    QNetworkReply *reply = m_nam->deleteResource(req);
    connect(reply, &QNetworkReply::finished, this, [this, reply, accountEmail]() {
        reply->deleteLater();
        if (reply->error() == QNetworkReply::NoError || reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() == 204) {
            emit statusMessage(MailI18n::t(QString("Google Calendar (%1) 일정 삭제 완료"),
                                           QString("Google Calendar (%1) event deleted"),
                                           QString("Google カレンダー (%1) 予定を削除しました"))
                                   .arg(accountEmail));
        }
    });
}

// ============================================================================
// 2-B. Apple Calendar (iCloud CalDAV RFC 4791 + webcal:// + RFC 5545 .ics)
// ============================================================================

static QString decodeXmlEntities(QString s)
{
    s.replace("&lt;", "<");
    s.replace("&gt;", ">");
    s.replace("&quot;", "\"");
    s.replace("&apos;", "'");
    s.replace("&#13;", "\r");
    s.replace("&#10;", "\n");
    s.replace("&amp;", "&");
    return s;
}

static QString unescapeIcsText(QString s)
{
    s.replace("\\r\\n", "\n");
    s.replace("\\n", "\n");
    s.replace("\\N", "\n");
    s.replace("\\,", ",");
    s.replace("\\;", ";");
    s.replace("\\\\", "\\");
    return s.trimmed();
}

static QString escapeIcsText(QString s)
{
    s.replace("\\", "\\\\");
    s.replace(";", "\\;");
    s.replace(",", "\\,");
    s.replace("\r\n", "\\n");
    s.replace("\n", "\\n");
    return s;
}

static bool parseIcsDateTimeValue(const QString &propParams,
                                  const QString &rawVal,
                                  QDate &outDate,
                                  QTime &outTime,
                                  bool &outIsAllDay)
{
    const QString v = rawVal.trimmed();
    if (v.isEmpty())
        return false;

    if (propParams.contains("VALUE=DATE", Qt::CaseInsensitive) && !propParams.contains("DATE-TIME", Qt::CaseInsensitive)) {
        if (v.length() >= 8) {
            outDate = QDate::fromString(v.left(8), "yyyyMMdd");
            outTime = QTime(0, 0);
            outIsAllDay = true;
            return outDate.isValid();
        }
    }

    if (v.length() == 8 && !v.contains('T')) {
        outDate = QDate::fromString(v, "yyyyMMdd");
        outTime = QTime(0, 0);
        outIsAllDay = true;
        return outDate.isValid();
    }

    outIsAllDay = false;
    const int tPos = v.indexOf('T');
    if (tPos < 8)
        return false;

    const QDate d = QDate::fromString(v.left(8), "yyyyMMdd");
    QString timePart = v.mid(tPos + 1);
    const bool isUtc = timePart.endsWith('Z', Qt::CaseInsensitive);
    if (isUtc)
        timePart.chop(1);

    QTime t = QTime::fromString(timePart.left(6), "HHmmss");
    if (!t.isValid() && timePart.length() >= 4)
        t = QTime::fromString(timePart.left(4), "HHmm");
    if (!d.isValid() || !t.isValid())
        return false;

    if (isUtc) {
        QDateTime utcDt(d, t, QTimeZone::UTC);
        QDateTime localDt = utcDt.toLocalTime();
        outDate = localDt.date();
        outTime = localDt.time();
    } else {
        outDate = d;
        outTime = t;
    }
    return true;
}

QList<CalendarEventItem> GoogleProvider::parseIcsCalendarData(const QString &icsText,
                                                              const QString &accountEmail,
                                                              const QString &calendarId,
                                                              const QColor &barColor,
                                                              const QString &defaultResourceUrl)
{
    QList<CalendarEventItem> results;
    if (icsText.trimmed().isEmpty())
        return results;

    QString unfolded = decodeXmlEntities(icsText);
    unfolded.replace("\r\n ", "");
    unfolded.replace("\r\n\t", "");
    unfolded.replace("\n ", "");
    unfolded.replace("\n\t", "");

    const QStringList lines = unfolded.split(QRegularExpression(R"(\r?\n)"));
    bool inEvent = false;
    QString uid;
    QString summary;
    QString location;
    QString description;
    QString urlStr;
    QString statusStr;
    QDate startDate;
    QTime startTime(9, 0);
    QDate endDate;
    QTime endTime(10, 0);
    bool isAllDay = false;
    bool hasStart = false;
    bool hasEnd = false;

    QColor validBar = barColor.isValid() ? barColor : QColor("#0D47A1");
    QColor bgCol = validBar.lighter(175);
    bgCol.setAlpha(255);
    QColor txtCol = validBar.darker(160);

    for (const QString &rawLine : lines) {
        const QString line = rawLine.trimmed();
        if (line.compare("BEGIN:VEVENT", Qt::CaseInsensitive) == 0) {
            inEvent = true;
            uid.clear();
            summary.clear();
            location.clear();
            description.clear();
            urlStr.clear();
            statusStr.clear();
            startDate = QDate();
            startTime = QTime(9, 0);
            endDate = QDate();
            endTime = QTime(10, 0);
            isAllDay = false;
            hasStart = false;
            hasEnd = false;
            continue;
        }
        if (line.compare("END:VEVENT", Qt::CaseInsensitive) == 0) {
            if (inEvent && hasStart && startDate.isValid()
                && statusStr.compare("CANCELLED", Qt::CaseInsensitive) != 0) {
                CalendarEventItem ev;
                ev.id = uid.isEmpty() ? ("ics_" + QUuid::createUuid().toString(QUuid::Id128).left(10)) : uid;
                ev.accountEmail = accountEmail.toUpper();
                ev.calendarId = calendarId;
                ev.title = summary.isEmpty() ? QStringLiteral("(제목 없음)") : summary;
                ev.location = location;
                ev.description = description;
                ev.meetUrl = urlStr;
                ev.calDavResourceUrl = defaultResourceUrl;
                ev.pillBarColor = validBar;
                ev.pillBgColor = bgCol;
                ev.pillTextColor = txtCol;
                ev.isAllDay = isAllDay;

                if (isAllDay) {
                    ev.startTime = QTime(0, 0);
                    ev.endTime = QTime(23, 59);
                    int daysSpan = (hasEnd && endDate.isValid() && endDate > startDate)
                                       ? qMin(int(startDate.daysTo(endDate)), 14)
                                       : 1;
                    for (int dOffset = 0; dOffset < daysSpan; ++dOffset) {
                        CalendarEventItem dayEv = ev;
                        dayEv.date = startDate.addDays(dOffset);
                        results.append(dayEv);
                    }
                } else {
                    ev.date = startDate;
                    ev.startTime = startTime;
                    ev.endTime = (hasEnd && endTime.isValid() && endTime > startTime)
                                     ? endTime
                                     : startTime.addSecs(3600);
                    results.append(ev);
                }
            }
            inEvent = false;
            continue;
        }
        if (!inEvent)
            continue;

        const int colonIdx = line.indexOf(':');
        if (colonIdx <= 0)
            continue;

        const QString leftPart = line.left(colonIdx);
        const QString valPart = line.mid(colonIdx + 1);
        const int semiIdx = leftPart.indexOf(';');
        const QString propName = (semiIdx >= 0 ? leftPart.left(semiIdx) : leftPart).trimmed().toUpper();
        const QString propParams = (semiIdx >= 0 ? leftPart.mid(semiIdx + 1) : QString()).trimmed();

        if (propName == "UID") {
            uid = unescapeIcsText(valPart);
        } else if (propName == "SUMMARY") {
            summary = unescapeIcsText(valPart);
        } else if (propName == "LOCATION") {
            location = unescapeIcsText(valPart);
        } else if (propName == "DESCRIPTION") {
            description = unescapeIcsText(valPart);
        } else if (propName == "URL") {
            urlStr = valPart.trimmed();
        } else if (propName == "STATUS") {
            statusStr = valPart.trimmed();
        } else if (propName == "DTSTART") {
            hasStart = parseIcsDateTimeValue(propParams, valPart, startDate, startTime, isAllDay);
        } else if (propName == "DTEND") {
            bool dummyAllDay = false;
            hasEnd = parseIcsDateTimeValue(propParams, valPart, endDate, endTime, dummyAllDay);
        }
    }

    return results;
}

QByteArray GoogleProvider::buildIcsFromEvent(const CalendarEventItem &event)
{
    QString uid = event.id.trimmed();
    if (uid.isEmpty() || uid.startsWith("local_ev_")) {
        uid = QUuid::createUuid().toString(QUuid::WithoutBraces).toUpper();
    }

    const QString dtStamp = QDateTime::currentDateTimeUtc().toString("yyyyMMdd'T'HHmmss'Z'");
    QString ics;
    ics += "BEGIN:VCALENDAR\r\n";
    ics += "VERSION:2.0\r\n";
    ics += "PRODID:-//SollunaMail//Apple CalDAV Client//KO\r\n";
    ics += "CALSCALE:GREGORIAN\r\n";
    ics += "BEGIN:VEVENT\r\n";
    ics += QString("UID:%1\r\n").arg(uid);
    ics += QString("DTSTAMP:%1\r\n").arg(dtStamp);

    if (event.isAllDay) {
        const QDate dStart = event.date.isValid() ? event.date : QDate::currentDate();
        const QDate dEnd = dStart.addDays(1);
        ics += QString("DTSTART;VALUE=DATE:%1\r\n").arg(dStart.toString("yyyyMMdd"));
        ics += QString("DTEND;VALUE=DATE:%1\r\n").arg(dEnd.toString("yyyyMMdd"));
    } else {
        const QDate d = event.date.isValid() ? event.date : QDate::currentDate();
        const QTime tStart = event.startTime.isValid() ? event.startTime : QTime(10, 0);
        const QTime tEnd = (event.endTime.isValid() && event.endTime > tStart) ? event.endTime : tStart.addSecs(3600);
        const QDateTime dtStartUtc = QDateTime(d, tStart).toUTC();
        const QDateTime dtEndUtc = QDateTime(d, tEnd).toUTC();
        ics += QString("DTSTART:%1\r\n").arg(dtStartUtc.toString("yyyyMMdd'T'HHmmss'Z'"));
        ics += QString("DTEND:%1\r\n").arg(dtEndUtc.toString("yyyyMMdd'T'HHmmss'Z'"));
    }

    ics += QString("SUMMARY:%1\r\n").arg(escapeIcsText(event.title));
    if (!event.location.trimmed().isEmpty()) {
        ics += QString("LOCATION:%1\r\n").arg(escapeIcsText(event.location));
    }
    if (!event.description.trimmed().isEmpty()) {
        ics += QString("DESCRIPTION:%1\r\n").arg(escapeIcsText(event.description));
    }
    ics += "END:VEVENT\r\n";
    ics += "END:VCALENDAR\r\n";
    return ics.toUtf8();
}

static QByteArray buildCalDavBasicAuth(const QString &appleIdEmail, const QString &appPassword)
{
    QString cleanPass = appPassword.trimmed();
    cleanPass.remove(' ');
    const QByteArray creds = QString("%1:%2").arg(appleIdEmail.trimmed(), cleanPass).toUtf8().toBase64();
    return "Basic " + creds;
}

static QUrl resolveCalDavHref(const QUrl &baseUrl, const QString &hrefStr)
{
    const QString trimmed = decodeXmlEntities(hrefStr.trimmed());
    if (trimmed.startsWith("http://", Qt::CaseInsensitive) || trimmed.startsWith("https://", Qt::CaseInsensitive))
        return QUrl(trimmed);
    return baseUrl.resolved(QUrl(trimmed));
}

void GoogleProvider::syncICloudCalDavCalendars(const QString &appleIdEmail,
                                               const QString &appPassword)
{
    const QString email = appleIdEmail.trimmed();
    const QString pass = appPassword.trimmed();
    if (email.isEmpty() || pass.isEmpty()) {
        emit statusMessage(MailI18n::t("Apple 캘린더(iCloud CalDAV) 동기화: Apple ID와 앱 암호(16자리)가 필요합니다.",
                                       "Apple Calendar (iCloud CalDAV) sync: Apple ID and 16-char app password are required.",
                                       "Apple カレンダー (iCloud CalDAV) 同期: Apple ID と16桁のアプリ用パスワードが必要です。"));
        return;
    }

    const QByteArray authHeader = buildCalDavBasicAuth(email, pass);
    emit statusMessage(MailI18n::t(QString("🍎 Apple 캘린더(iCloud CalDAV: %1) 연결 및 캘린더 목록 탐색 중..."),
                                   QString("🍎 Connecting to Apple Calendar (iCloud CalDAV: %1) and discovering calendars..."),
                                   QString("🍎 Apple カレンダー (iCloud CalDAV: %1) に接続してカレンダーを検索中..."))
                           .arg(email));

    // 1단계: PROPFIND https://caldav.icloud.com/ (Depth: 0) -> current-user-principal 조회
    QUrl rootUrl("https://caldav.icloud.com/");
    QNetworkRequest propReq(rootUrl);
    propReq.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    propReq.setHeader(QNetworkRequest::ContentTypeHeader, "application/xml; charset=utf-8");
    propReq.setRawHeader("Authorization", authHeader);
    propReq.setRawHeader("Depth", "0");

    const QByteArray step1Xml =
        "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
        "<d:propfind xmlns:d=\"DAV:\">"
        "  <d:prop>"
        "    <d:current-user-principal/>"
        "  </d:prop>"
        "</d:propfind>";

    QNetworkReply *step1Reply = m_nam->sendCustomRequest(propReq, "PROPFIND", step1Xml);
    connect(step1Reply, &QNetworkReply::finished, this, [this, step1Reply, email, authHeader, rootUrl]() {
        step1Reply->deleteLater();
        const int status1 = step1Reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const QString xml1 = QString::fromUtf8(step1Reply->readAll());

        if (status1 == 401 || status1 == 403) {
            emit statusMessage(MailI18n::t(QString("🍎 Apple 캘린더 인증 실패 (%1): Apple ID 또는 16자리 앱 암호(xxxx-xxxx-xxxx-xxxx)를 확인해 주세요."),
                                           QString("🍎 Apple Calendar auth failed (%1): Please check your Apple ID or 16-char app password."),
                                           QString("🍎 Apple カレンダー認証失敗 (%1): Apple ID または16桁のアプリ用パスワードを確認してください。"))
                                   .arg(email));
            return;
        }

        static const QRegularExpression rePrincipal(
            R"(<(?:\w+:)?current-user-principal[^>]*>\s*<(?:\w+:)?href[^>]*>([^<]+)</(?:\w+:)?href>)",
            QRegularExpression::CaseInsensitiveOption | QRegularExpression::DotMatchesEverythingOption);
        QRegularExpressionMatch m1 = rePrincipal.match(xml1);
        if (!m1.hasMatch()) {
            emit statusMessage(MailI18n::t(QString("🍎 Apple CalDAV principal 경로를 찾지 못했습니다 (%1, HTTP %2)."),
                                           QString("🍎 Could not find Apple CalDAV principal path (%1, HTTP %2)."),
                                           QString("🍎 Apple CalDAV principal パスが見つかりませんでした (%1, HTTP %2)。"))
                                   .arg(email).arg(status1));
            return;
        }

        const QUrl effectiveBase1 = step1Reply->url().isValid() ? step1Reply->url() : rootUrl;
        const QUrl principalUrl = resolveCalDavHref(effectiveBase1, m1.captured(1));

        // 2단계: PROPFIND principalUrl (Depth: 0) -> calendar-home-set 조회
        QNetworkRequest req2(principalUrl);
        req2.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
        req2.setHeader(QNetworkRequest::ContentTypeHeader, "application/xml; charset=utf-8");
        req2.setRawHeader("Authorization", authHeader);
        req2.setRawHeader("Depth", "0");

        const QByteArray step2Xml =
            "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
            "<d:propfind xmlns:d=\"DAV:\" xmlns:c=\"urn:ietf:params:xml:ns:caldav\">"
            "  <d:prop>"
            "    <c:calendar-home-set/>"
            "  </d:prop>"
            "</d:propfind>";

        QNetworkReply *step2Reply = m_nam->sendCustomRequest(req2, "PROPFIND", step2Xml);
        connect(step2Reply, &QNetworkReply::finished, this, [this, step2Reply, email, authHeader, principalUrl]() {
            step2Reply->deleteLater();
            const QString xml2 = QString::fromUtf8(step2Reply->readAll());

            static const QRegularExpression reHomeSet(
                R"(<(?:\w+:)?calendar-home-set[^>]*>\s*<(?:\w+:)?href[^>]*>([^<]+)</(?:\w+:)?href>)",
                QRegularExpression::CaseInsensitiveOption | QRegularExpression::DotMatchesEverythingOption);
            QRegularExpressionMatch m2 = reHomeSet.match(xml2);
            if (!m2.hasMatch()) {
                emit statusMessage(MailI18n::t(QString("🍎 Apple CalDAV calendar-home-set 경로를 찾지 못했습니다 (%1)."),
                                               QString("🍎 Could not find Apple CalDAV calendar-home-set path (%1)."),
                                               QString("🍎 Apple CalDAV calendar-home-set パスが見つかりませんでした (%1)。"))
                                       .arg(email));
                return;
            }

            const QUrl effectiveBase2 = step2Reply->url().isValid() ? step2Reply->url() : principalUrl;
            const QUrl calendarHomeUrl = resolveCalDavHref(effectiveBase2, m2.captured(1));

            // 3단계: PROPFIND calendarHomeUrl (Depth: 1) -> 내 iCloud 하위 캘린더 목록 조회
            QNetworkRequest req3(calendarHomeUrl);
            req3.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
            req3.setHeader(QNetworkRequest::ContentTypeHeader, "application/xml; charset=utf-8");
            req3.setRawHeader("Authorization", authHeader);
            req3.setRawHeader("Depth", "1");

            const QByteArray step3Xml =
                "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
                "<d:propfind xmlns:d=\"DAV:\" xmlns:c=\"urn:ietf:params:xml:ns:caldav\" xmlns:x=\"http://apple.com/ns/ical/\">"
                "  <d:prop>"
                "    <d:displayname/>"
                "    <d:resourcetype/>"
                "    <x:calendar-color/>"
                "    <c:supported-calendar-component-set/>"
                "  </d:prop>"
                "</d:propfind>";

            QNetworkReply *step3Reply = m_nam->sendCustomRequest(req3, "PROPFIND", step3Xml);
            connect(step3Reply, &QNetworkReply::finished, this, [this, step3Reply, email, authHeader, calendarHomeUrl]() {
                step3Reply->deleteLater();
                const QString xml3 = QString::fromUtf8(step3Reply->readAll());
                const QUrl effectiveHomeUrl = step3Reply->url().isValid() ? step3Reply->url() : calendarHomeUrl;

                QList<CalendarSubItem> discoveredCalendars;
                static const QRegularExpression reResponse(
                    R"(<(?:\w+:)?response\b[^>]*>(.*?)</(?:\w+:)?response>)",
                    QRegularExpression::CaseInsensitiveOption | QRegularExpression::DotMatchesEverythingOption);
                static const QRegularExpression reHref(
                    R"(<(?:\w+:)?href[^>]*>([^<]+)</(?:\w+:)?href>)",
                    QRegularExpression::CaseInsensitiveOption);
                static const QRegularExpression reResType(
                    R"(<(?:\w+:)?resourcetype[^>]*>(.*?)</(?:\w+:)?resourcetype>)",
                    QRegularExpression::CaseInsensitiveOption | QRegularExpression::DotMatchesEverythingOption);
                static const QRegularExpression reDisplayName(
                    R"(<(?:\w+:)?displayname[^>]*>(.*?)</(?:\w+:)?displayname>)",
                    QRegularExpression::CaseInsensitiveOption | QRegularExpression::DotMatchesEverythingOption);
                static const QRegularExpression reCalColor(
                    R"(<(?:\w+:)?calendar-color[^>]*>([^<]+)</(?:\w+:)?calendar-color>)",
                    QRegularExpression::CaseInsensitiveOption);
                static const QRegularExpression reCompSet(
                    R"(<(?:\w+:)?supported-calendar-component-set[^>]*>(.*?)</(?:\w+:)?supported-calendar-component-set>)",
                    QRegularExpression::CaseInsensitiveOption | QRegularExpression::DotMatchesEverythingOption);

                const QStringList fallbackColors = {"#E11D48", "#2563EB", "#16A34A", "#D97706", "#7C3AED", "#0891B2"};
                int colorIdx = 0;

                QRegularExpressionMatchIterator it = reResponse.globalMatch(xml3);
                while (it.hasNext()) {
                    const QString respBlock = it.next().captured(1);
                    const QRegularExpressionMatch mType = reResType.match(respBlock);
                    if (!mType.hasMatch())
                        continue;
                    const QString resTypeXml = mType.captured(1);
                    // 실제 캘린더 컬렉션만 선택 (notification, inbox, outbox, dropbox 제외)
                    if (!resTypeXml.contains("calendar", Qt::CaseInsensitive)
                        || resTypeXml.contains("schedule-", Qt::CaseInsensitive)
                        || resTypeXml.contains("notification", Qt::CaseInsensitive)
                        || resTypeXml.contains("dropbox", Qt::CaseInsensitive)) {
                        continue;
                    }

                    // 미리알림(VTODO 전용) 컬렉션은 제외하고 일정(VEVENT) 지원 캘린더만 포함
                    const QRegularExpressionMatch mComp = reCompSet.match(respBlock);
                    if (mComp.hasMatch()) {
                        const QString compXml = mComp.captured(1);
                        if (compXml.contains("comp", Qt::CaseInsensitive)
                            && !compXml.contains("VEVENT", Qt::CaseInsensitive)) {
                            continue;
                        }
                    }

                    const QRegularExpressionMatch mHref = reHref.match(respBlock);
                    if (!mHref.hasMatch())
                        continue;
                    const QUrl colUrl = resolveCalDavHref(effectiveHomeUrl, mHref.captured(1));
                    if (colUrl.path() == effectiveHomeUrl.path())
                        continue;

                    QString dispName = decodeXmlEntities(reDisplayName.match(respBlock).captured(1).trimmed());
                    dispName.remove("<![CDATA[").remove("]]>");
                    if (dispName.isEmpty() || dispName.compare("tasks", Qt::CaseInsensitive) == 0)
                        continue;

                    QString colorHex = reCalColor.match(respBlock).captured(1).trimmed();
                    if (colorHex.startsWith('#') && colorHex.length() >= 7) {
                        // Apple iCal color format #RRGGBBAA -> #RRGGBB
                        colorHex = colorHex.left(7);
                    }
                    QColor calCol(colorHex);
                    if (!calCol.isValid()) {
                        calCol = QColor(fallbackColors[colorIdx % fallbackColors.size()]);
                        ++colorIdx;
                    }

                    CalendarSubItem sub;
                    sub.id = "icloud_" + colUrl.path().trimmed();
                    sub.accountEmail = email.toUpper();
                    sub.name = QString("🍎 %1").arg(dispName);
                    sub.color = calCol;
                    sub.isVisible = true;
                    sub.isDefault = discoveredCalendars.isEmpty();
                    sub.showEyeToggle = !sub.isDefault;
                    sub.calDavHref = colUrl.toString();
                    discoveredCalendars.append(sub);
                }

                if (discoveredCalendars.isEmpty()) {
                    emit statusMessage(MailI18n::t(QString("🍎 Apple iCloud (%1): 표시할 캘린더가 없습니다."),
                                                   QString("🍎 Apple iCloud (%1): No calendars found."),
                                                   QString("🍎 Apple iCloud (%1): 表示するカレンダーがありません。"))
                                           .arg(email));
                    return;
                }

                // 4단계: 각 하위 캘린더별로 REPORT (calendar-query) 호출하여 VEVENT(.ics) 일정 수집
                auto remaining = QSharedPointer<int>::create(discoveredCalendars.size());
                auto allEvents = QSharedPointer<QList<CalendarEventItem>>::create();

                const QByteArray reportXml =
                    "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
                    "<c:calendar-query xmlns:d=\"DAV:\" xmlns:c=\"urn:ietf:params:xml:ns:caldav\">"
                    "  <d:prop>"
                    "    <d:getetag/>"
                    "    <c:calendar-data/>"
                    "  </d:prop>"
                    "  <c:filter>"
                    "    <c:comp-filter name=\"VCALENDAR\">"
                    "      <c:comp-filter name=\"VEVENT\">"
                    "        <c:time-range start=\"20250101T000000Z\" end=\"20271231T235959Z\"/>"
                    "      </c:comp-filter>"
                    "    </c:comp-filter>"
                    "  </c:filter>"
                    "</c:calendar-query>";

                for (const CalendarSubItem &subCal : discoveredCalendars) {
                    const QUrl calUrl(subCal.calDavHref);
                    QNetworkRequest repReq(calUrl);
                    repReq.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
                    repReq.setHeader(QNetworkRequest::ContentTypeHeader, "application/xml; charset=utf-8");
                    repReq.setRawHeader("Authorization", authHeader);
                    repReq.setRawHeader("Depth", "1");

                    QNetworkReply *repReply = m_nam->sendCustomRequest(repReq, "REPORT", reportXml);
                    connect(repReply, &QNetworkReply::finished, this,
                            [this, repReply, email, subCal, calUrl, discoveredCalendars, remaining, allEvents]() {
                        repReply->deleteLater();
                        const QString repXml = QString::fromUtf8(repReply->readAll());
                        const QUrl effectiveCalUrl = repReply->url().isValid() ? repReply->url() : calUrl;

                        static const QRegularExpression reRespBlock(
                            R"(<(?:\w+:)?response\b[^>]*>(.*?)</(?:\w+:)?response>)",
                            QRegularExpression::CaseInsensitiveOption | QRegularExpression::DotMatchesEverythingOption);
                        static const QRegularExpression reItemHref(
                            R"(<(?:\w+:)?href[^>]*>([^<]+)</(?:\w+:)?href>)",
                            QRegularExpression::CaseInsensitiveOption);
                        static const QRegularExpression reCalData(
                            R"(<(?:\w+:)?calendar-data[^>]*>(.*?)</(?:\w+:)?calendar-data>)",
                            QRegularExpression::CaseInsensitiveOption | QRegularExpression::DotMatchesEverythingOption);

                        QRegularExpressionMatchIterator rit = reRespBlock.globalMatch(repXml);
                        while (rit.hasNext()) {
                            const QString rBlock = rit.next().captured(1);
                            const QRegularExpressionMatch mData = reCalData.match(rBlock);
                            if (!mData.hasMatch())
                                continue;
                            QString icsData = mData.captured(1).trimmed();
                            if (icsData.startsWith("<![CDATA[")) {
                                icsData.remove(0, 9);
                                if (icsData.endsWith("]]>"))
                                    icsData.chop(3);
                            }
                            const QString itemHref = reItemHref.match(rBlock).captured(1).trimmed();
                            const QString fullResUrl = itemHref.isEmpty()
                                                           ? QString()
                                                           : resolveCalDavHref(effectiveCalUrl, itemHref).toString();
                            const QList<CalendarEventItem> parsed =
                                parseIcsCalendarData(icsData, email, subCal.id, subCal.color, fullResUrl);
                            allEvents->append(parsed);
                        }

                        *remaining -= 1;
                        if (*remaining <= 0) {
                            const QList<CalendarEventItem> finalEvents = *allEvents;
                            emit statusMessage(
                                MailI18n::t(QString("🍎 Apple 캘린더 (%1) 동기화 완료 — 캘린더 %2개 / 일정 %3건"),
                                            QString("🍎 Apple Calendar (%1) synced — %2 calendars / %3 events"),
                                            QString("🍎 Apple カレンダー (%1) 同期完了 — カレンダー%2個 / 予定%3件"))
                                    .arg(email)
                                    .arg(discoveredCalendars.size())
                                    .arg(finalEvents.size()));
                            emit googleCalendarsFetched(email, discoveredCalendars, finalEvents);
                        }
                    });
                }
            });
        });
    });
}

static QUrl buildCalDavEventTargetUrl(const QString &calendarCollectionUrl, const CalendarEventItem &event)
{
    if (!event.calDavResourceUrl.trimmed().isEmpty()) {
        return QUrl(event.calDavResourceUrl.trimmed());
    }
    QString base = calendarCollectionUrl.trimmed();
    if (!base.endsWith('/'))
        base += '/';
    QString uid = event.id.trimmed();
    if (uid.isEmpty())
        uid = QUuid::createUuid().toString(QUuid::Id128).left(12);
    if (!uid.endsWith(".ics", Qt::CaseInsensitive))
        uid += ".ics";
    return QUrl(base + uid);
}

void GoogleProvider::createICloudCalDavEvent(const QString &appleIdEmail,
                                             const QString &appPassword,
                                             const QString &calendarCollectionUrl,
                                             const CalendarEventItem &event)
{
    if (appleIdEmail.trimmed().isEmpty() || appPassword.trimmed().isEmpty() || calendarCollectionUrl.trimmed().isEmpty())
        return;

    const QUrl targetUrl = buildCalDavEventTargetUrl(calendarCollectionUrl, event);
    QNetworkRequest req(targetUrl);
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    req.setHeader(QNetworkRequest::ContentTypeHeader, "text/calendar; charset=utf-8");
    req.setRawHeader("Authorization", buildCalDavBasicAuth(appleIdEmail, appPassword));

    const QByteArray icsPayload = buildIcsFromEvent(event);
    QNetworkReply *reply = m_nam->put(req, icsPayload);
    connect(reply, &QNetworkReply::finished, this, [this, reply, appleIdEmail, event]() {
        reply->deleteLater();
        const int code = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (reply->error() == QNetworkReply::NoError || (code >= 200 && code < 300)) {
            emit statusMessage(MailI18n::t(QString("🍎 Apple 캘린더 (%1) 새 일정 서버 등록 완료: %2"),
                                           QString("🍎 Apple Calendar (%1) event saved to server: %2"),
                                           QString("🍎 Apple カレンダー (%1) 予定をサーバーに登録しました: %2"))
                                   .arg(appleIdEmail, event.title));
        } else {
            emit statusMessage(MailI18n::t(QString("🍎 Apple 캘린더 일정 등록 응답 (HTTP %1): 로컬 캘린더에 저장되었습니다."),
                                           QString("🍎 Apple Calendar event response (HTTP %1): Saved to local calendar."),
                                           QString("🍎 Apple カレンダー予定登録応答 (HTTP %1): ローカルカレンダーに保存しました。"))
                                   .arg(code));
        }
    });
}

void GoogleProvider::updateICloudCalDavEvent(const QString &appleIdEmail,
                                             const QString &appPassword,
                                             const QString &calendarCollectionUrl,
                                             const CalendarEventItem &event)
{
    createICloudCalDavEvent(appleIdEmail, appPassword, calendarCollectionUrl, event);
}

void GoogleProvider::deleteICloudCalDavEvent(const QString &appleIdEmail,
                                             const QString &appPassword,
                                             const QString &calendarCollectionUrl,
                                             const CalendarEventItem &event)
{
    if (appleIdEmail.trimmed().isEmpty() || appPassword.trimmed().isEmpty())
        return;

    const QUrl targetUrl = buildCalDavEventTargetUrl(calendarCollectionUrl, event);
    if (!targetUrl.isValid() || targetUrl.isEmpty())
        return;

    QNetworkRequest req(targetUrl);
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    req.setRawHeader("Authorization", buildCalDavBasicAuth(appleIdEmail, appPassword));

    QNetworkReply *reply = m_nam->deleteResource(req);
    connect(reply, &QNetworkReply::finished, this, [this, reply, appleIdEmail, event]() {
        reply->deleteLater();
        const int code = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (reply->error() == QNetworkReply::NoError || (code >= 200 && code < 300) || code == 404) {
            emit statusMessage(MailI18n::t(QString("🍎 Apple 캘린더 (%1) 일정 삭제 완료: %2"),
                                           QString("🍎 Apple Calendar (%1) event deleted: %2"),
                                           QString("🍎 Apple カレンダー (%1) 予定を削除しました: %2"))
                                   .arg(appleIdEmail, event.title));
        }
    });
}

void GoogleProvider::fetchWebcalIcsSubscription(const QString &accountEmail,
                                                const QString &calendarName,
                                                const QString &webcalUrl,
                                                const QColor &calColor)
{
    QString urlStr = webcalUrl.trimmed();
    if (urlStr.startsWith("webcal://", Qt::CaseInsensitive)) {
        urlStr = "https://" + urlStr.mid(9);
    } else if (urlStr.startsWith("webcals://", Qt::CaseInsensitive)) {
        urlStr = "https://" + urlStr.mid(10);
    }
    const QUrl url(urlStr);
    if (!url.isValid()) {
        emit statusMessage(MailI18n::t("유효하지 않은 Apple 공개 캘린더(webcal://) 주소입니다.",
                                       "Invalid Apple public calendar (webcal://) URL.",
                                       "無効な Apple 公開カレンダー (webcal://) URL です。"));
        return;
    }

    QNetworkRequest req(url);
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    req.setRawHeader("User-Agent", "SollunaMail/1.0 (CalDAV/iCalendar Client)");

    QNetworkReply *reply = m_nam->get(req);
    connect(reply, &QNetworkReply::finished, this, [this, reply, accountEmail, calendarName, urlStr, calColor]() {
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError) {
            emit statusMessage(MailI18n::t(QString("공개 캘린더 구독 불러오기 실패: %1"),
                                           QString("Failed to load public calendar subscription: %1"),
                                           QString("公開カレンダー購読の読み込み失敗: %1"))
                                   .arg(reply->errorString()));
            return;
        }

        const QString icsText = QString::fromUtf8(reply->readAll());
        const QString subId = "webcal_" + QString::number(qHash(urlStr), 16);
        CalendarSubItem sub;
        sub.id = subId;
        sub.accountEmail = accountEmail.trimmed().isEmpty() ? QStringLiteral("APPLE CALENDAR (WEBCAL)") : accountEmail.toUpper();
        sub.name = calendarName.trimmed().isEmpty() ? MailI18n::t("🍎 Apple 공개 캘린더", "🍎 Apple Public Calendar", "🍎 Apple 公開カレンダー") : calendarName.trimmed();
        sub.color = calColor.isValid() ? calColor : QColor("#E11D48");
        sub.isVisible = true;
        sub.isDefault = false;
        sub.showEyeToggle = true;
        sub.webcalUrl = urlStr;

        const QList<CalendarEventItem> events = parseIcsCalendarData(icsText, sub.accountEmail, sub.id, sub.color);
        emit statusMessage(MailI18n::t(QString("🍎 '%1' 공개 캘린더 구독 동기화 완료 (일정 %2건)"),
                                       QString("🍎 '%1' public calendar synced (%2 events)"),
                                       QString("🍎 '%1' 公開カレンダー同期完了 (予定 %2件)"))
                               .arg(sub.name)
                               .arg(events.size()));
        emit googleCalendarsFetched(sub.accountEmail, {sub}, events);
    });
}

void GoogleProvider::syncGoogleContacts(const QString &accountEmail,
                                        const QString &accessToken,
                                        const QString &refreshToken)
{
    if (accessToken.trimmed().isEmpty()) {
        if (!refreshToken.trimmed().isEmpty()) {
            refreshGoogleAccessToken(accountEmail, refreshToken);
        } else {
            emit statusMessage(MailI18n::t("Google Contacts 동기화: OAuth 액세스 토큰이 필요합니다.",
                                           "Google Contacts sync: OAuth access token is required.",
                                           "Google 連絡先同期: OAuth アクセストークンが必要です。"));
        }
        return;
    }

    const QVector<QPair<QColor, QColor>> paletteColors = {
        {QColor("#E0F2FE"), QColor("#0284C7")},
        {QColor("#DCFCE7"), QColor("#15803D")},
        {QColor("#FEF3C7"), QColor("#B45309")},
        {QColor("#FCE7F3"), QColor("#DB2777")},
        {QColor("#EDE9FE"), QColor("#6D28D9")},
        {QColor("#CCFBF1"), QColor("#0F766E")}
    };

    auto mergedContacts = QSharedPointer<QList<ContactItem>>::create();
    auto seenKeys = QSharedPointer<QSet<QString>>::create();
    auto colorIdx = QSharedPointer<int>::create(0);

    auto appendUniqueContact = [mergedContacts, seenKeys, colorIdx, paletteColors](ContactItem c) {
        if (c.displayName.isEmpty() && c.email.isEmpty() && c.phone.isEmpty())
            return;
        if (c.displayName.isEmpty()) {
            c.displayName = !c.email.isEmpty() ? c.email : (!c.phone.isEmpty() ? c.phone : c.company);
        }
        QString key = (!c.email.isEmpty() ? c.email.toLower() : (c.displayName.toLower() + "|" + c.phone));
        if (key.isEmpty())
            return;

        if (seenKeys->contains(key)) {
            // 기존 항목에 누락된 전화번호/회사/메모가 있으면 보강
            for (ContactItem &existing : *mergedContacts) {
                QString exKey = (!existing.email.isEmpty() ? existing.email.toLower() : (existing.displayName.toLower() + "|" + existing.phone));
                if (exKey == key) {
                    if ((existing.displayName == existing.email || existing.displayName.isEmpty()) && !c.displayName.isEmpty() && c.displayName != c.email)
                        existing.displayName = c.displayName;
                    if (existing.phone.isEmpty() && !c.phone.isEmpty())
                        existing.phone = c.phone;
                    if (existing.company.isEmpty() && !c.company.isEmpty())
                        existing.company = c.company;
                    if (existing.jobTitle.isEmpty() && !c.jobTitle.isEmpty())
                        existing.jobTitle = c.jobTitle;
                    if (existing.notes.isEmpty() && !c.notes.isEmpty())
                        existing.notes = c.notes;
                    break;
                }
            }
            return;
        }

        seenKeys->insert(key);
        c.initials = c.displayName.left(1).toUpper();
        auto colPair = paletteColors[(*colorIdx) % paletteColors.size()];
        ++(*colorIdx);
        c.avatarBgColor = colPair.first;
        c.avatarFgColor = colPair.second;
        mergedContacts->append(c);
    };

    // 1단계: Google Contacts Data API v3 (m8/feeds) — max-results=2000 조회
    QUrl url("https://www.google.com/m8/feeds/contacts/default/full?alt=json&max-results=2000&v=3.0");
    QNetworkRequest req(url);
    req.setRawHeader("Authorization", ("Bearer " + accessToken.trimmed()).toUtf8());
    req.setRawHeader("GData-Version", "3.0");

    QNetworkReply *reply = m_nam->get(req);
    connect(reply, &QNetworkReply::finished, this,
            [this, reply, accountEmail, accessToken, refreshToken, mergedContacts, seenKeys, colorIdx, appendUniqueContact]() {
        reply->deleteLater();
        const int httpCode = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (httpCode == 401 && !refreshToken.trimmed().isEmpty()) {
            refreshGoogleAccessToken(accountEmail, refreshToken);
            return;
        }

        if (reply->error() == QNetworkReply::NoError) {
            QJsonDocument doc = QJsonDocument::fromJson(reply->readAll());
            QJsonArray entries = doc.object().value("feed").toObject().value("entry").toArray();
            for (const QJsonValue &v : entries) {
                QJsonObject entry = v.toObject();
                ContactItem c;
                c.id = entry.value("id").toObject().value("$t").toString();
                c.accountEmail = accountEmail.toUpper();
                c.groupName = accountEmail.toUpper();

                QJsonObject gdName = entry.value("gd$name").toObject();
                c.displayName = gdName.value("gd$fullName").toObject().value("$t").toString().trimmed();
                if (c.displayName.isEmpty()) {
                    QString fam = gdName.value("gd$familyName").toObject().value("$t").toString().trimmed();
                    QString giv = gdName.value("gd$givenName").toObject().value("$t").toString().trimmed();
                    c.displayName = (fam + " " + giv).trimmed();
                }
                if (c.displayName.isEmpty()) {
                    c.displayName = entry.value("title").toObject().value("$t").toString().trimmed();
                }

                QJsonArray emails = entry.value("gd$email").toArray();
                for (const QJsonValue &ev : emails) {
                    QString addr = ev.toObject().value("address").toString().trimmed();
                    if (!addr.isEmpty()) {
                        c.email = addr;
                        break;
                    }
                }

                QJsonArray phones = entry.value("gd$phoneNumber").toArray();
                QStringList phoneList;
                for (const QJsonValue &pv : phones) {
                    QString ph = pv.toObject().value("$t").toString().trimmed();
                    if (!ph.isEmpty())
                        phoneList.append(ph);
                }
                c.phone = phoneList.join(" / ");

                QJsonArray orgs = entry.value("gd$organization").toArray();
                if (!orgs.isEmpty()) {
                    QJsonObject org = orgs.first().toObject();
                    c.company = org.value("gd$orgName").toObject().value("$t").toString().trimmed();
                    c.jobTitle = org.value("gd$orgTitle").toObject().value("$t").toString().trimmed();
                }
                c.notes = entry.value("content").toObject().value("$t").toString().trimmed();
                appendUniqueContact(c);
            }
        }

        // 2단계: Google People API v1 (people/me/connections) — 조기 종료 없이 병합
        QUrl peopleUrl("https://people.googleapis.com/v1/people/me/connections"
                       "?personFields=names,emailAddresses,phoneNumbers,organizations,biographies"
                       "&pageSize=1000");
        QNetworkRequest pReq(peopleUrl);
        pReq.setRawHeader("Authorization", ("Bearer " + accessToken.trimmed()).toUtf8());
        QNetworkReply *pReply = m_nam->get(pReq);
        connect(pReply, &QNetworkReply::finished, this,
                [this, pReply, accountEmail, accessToken, mergedContacts, seenKeys, colorIdx, appendUniqueContact]() {
            pReply->deleteLater();
            if (pReply->error() == QNetworkReply::NoError) {
                QJsonObject root = QJsonDocument::fromJson(pReply->readAll()).object();
                QJsonArray conns = root.value("connections").toArray();
                for (const QJsonValue &v : conns) {
                    QJsonObject pObj = v.toObject();
                    ContactItem c;
                    c.id = pObj.value("resourceName").toString();
                    c.accountEmail = accountEmail.toUpper();
                    c.groupName = accountEmail.toUpper();

                    QJsonArray names = pObj.value("names").toArray();
                    if (!names.isEmpty())
                        c.displayName = names.first().toObject().value("displayName").toString().trimmed();

                    QJsonArray emails = pObj.value("emailAddresses").toArray();
                    if (!emails.isEmpty())
                        c.email = emails.first().toObject().value("value").toString().trimmed();

                    QJsonArray phones = pObj.value("phoneNumbers").toArray();
                    QStringList phList;
                    for (const QJsonValue &pv : phones) {
                        QString ph = pv.toObject().value("value").toString().trimmed();
                        if (!ph.isEmpty())
                            phList.append(ph);
                    }
                    c.phone = phList.join(" / ");

                    QJsonArray orgs = pObj.value("organizations").toArray();
                    if (!orgs.isEmpty()) {
                        c.company = orgs.first().toObject().value("name").toString().trimmed();
                        c.jobTitle = orgs.first().toObject().value("title").toString().trimmed();
                    }
                    appendUniqueContact(c);
                }
            }

            // 3단계: Google People API v1 (otherContacts — 기타 연락처 / 자주 연락하는 연락처) 병합
            QUrl otherUrl("https://people.googleapis.com/v1/otherContacts"
                          "?readMask=names,emailAddresses,phoneNumbers"
                          "&pageSize=1000");
            QNetworkRequest oReq(otherUrl);
            oReq.setRawHeader("Authorization", ("Bearer " + accessToken.trimmed()).toUtf8());
            QNetworkReply *oReply = m_nam->get(oReq);
            connect(oReply, &QNetworkReply::finished, this,
                    [this, oReply, accountEmail, mergedContacts, seenKeys, colorIdx, appendUniqueContact]() {
                oReply->deleteLater();
                if (oReply->error() == QNetworkReply::NoError) {
                    QJsonObject root = QJsonDocument::fromJson(oReply->readAll()).object();
                    QJsonArray others = root.value("otherContacts").toArray();
                    for (const QJsonValue &v : others) {
                        QJsonObject pObj = v.toObject();
                        ContactItem c;
                        c.id = pObj.value("resourceName").toString();
                        c.accountEmail = accountEmail.toUpper();
                        c.groupName = accountEmail.toUpper();

                        QJsonArray names = pObj.value("names").toArray();
                        if (!names.isEmpty())
                            c.displayName = names.first().toObject().value("displayName").toString().trimmed();

                        QJsonArray emails = pObj.value("emailAddresses").toArray();
                        if (!emails.isEmpty())
                            c.email = emails.first().toObject().value("value").toString().trimmed();

                        QJsonArray phones = pObj.value("phoneNumbers").toArray();
                        QStringList phList;
                        for (const QJsonValue &pv : phones) {
                            QString ph = pv.toObject().value("value").toString().trimmed();
                            if (!ph.isEmpty())
                                phList.append(ph);
                        }
                        c.phone = phList.join(" / ");
                        appendUniqueContact(c);
                    }
                }

                QList<ContactItem> finalContacts = *mergedContacts;

                if (!finalContacts.isEmpty()) {
                    emit statusMessage(
                        MailI18n::t(QString("Google Contacts (%1) 연락처 %2명 동기화 완료"),
                                    QString("Google Contacts (%1) synced %2 contacts"),
                                    QString("Google 連絡先 (%1) %2件の同期完了"))
                            .arg(accountEmail)
                            .arg(finalContacts.size()));
                    emit googleContactsFetched(accountEmail, finalContacts);
                }
            });
        });
    });
}

namespace {

QString decodeImapModifiedUtf7(const QString &input)
{
    QString out;
    int i = 0;
    const int n = input.size();
    while (i < n) {
        if (input[i] == QLatin1Char('&')) {
            int dash = input.indexOf(QLatin1Char('-'), i + 1);
            if (dash == i + 1) {
                out.append(QLatin1Char('&'));
                i += 2;
                continue;
            }
            if (dash > i + 1) {
                QString b64 = input.mid(i + 1, dash - i - 1);
                b64.replace(QLatin1Char(','), QLatin1Char('/'));
                while (b64.size() % 4 != 0)
                    b64.append(QLatin1Char('='));
                QByteArray utf16be = QByteArray::fromBase64(b64.toLatin1());
                for (int j = 0; j + 1 < utf16be.size(); j += 2) {
                    ushort ch = (uchar(utf16be[j]) << 8) | uchar(utf16be[j + 1]);
                    out.append(QChar(ch));
                }
                i = dash + 1;
                continue;
            }
        }
        out.append(input[i]);
        ++i;
    }
    return out;
}

QString encodeImapModifiedUtf7(const QString &input)
{
    QString out;
    QByteArray utf16Buf;
    auto flushUtf16 = [&]() {
        if (utf16Buf.isEmpty())
            return;
        QByteArray b64 = utf16Buf.toBase64();
        while (b64.endsWith('='))
            b64.chop(1);
        b64.replace('/', ',');
        out.append(QLatin1Char('&'));
        out.append(QString::fromLatin1(b64));
        out.append(QLatin1Char('-'));
        utf16Buf.clear();
    };

    for (QChar ch : input) {
        const ushort u = ch.unicode();
        if (u >= 0x20 && u <= 0x7e) {
            flushUtf16();
            if (u == '&')
                out.append(QStringLiteral("&-"));
            else
                out.append(ch);
        } else {
            utf16Buf.append(char((u >> 8) & 0xff));
            utf16Buf.append(char(u & 0xff));
        }
    }
    flushUtf16();
    return out;
}

QString buildImapSequenceSet(const QList<int> &rawSeqNums)
{
    QList<int> sorted;
    for (int s : rawSeqNums) {
        if (s > 0 && !sorted.contains(s))
            sorted.append(s);
    }
    std::sort(sorted.begin(), sorted.end());
    if (sorted.isEmpty())
        return QString();

    QStringList ranges;
    int rangeStart = sorted.first();
    int rangePrev = rangeStart;
    for (int i = 1; i < sorted.size(); ++i) {
        const int cur = sorted[i];
        if (cur == rangePrev + 1) {
            rangePrev = cur;
        } else {
            ranges.append(rangeStart == rangePrev
                              ? QString::number(rangeStart)
                              : QString("%1:%2").arg(rangeStart).arg(rangePrev));
            rangeStart = rangePrev = cur;
        }
    }
    ranges.append(rangeStart == rangePrev
                      ? QString::number(rangeStart)
                      : QString("%1:%2").arg(rangeStart).arg(rangePrev));
    return ranges.join(QLatin1Char(','));
}

QString classifyImapFlagOrGmailSystemFolder(const QString &flags, const QString &rawMailbox, const QString &decoded)
{
    if (rawMailbox.compare("INBOX", Qt::CaseInsensitive) == 0)
        return QStringLiteral("inbox");

    // 1. RFC 6154 SPECIAL-USE flags (Gmail, iCloud, Dovecot, Exchange 등 표준)
    if (flags.contains("\\Sent", Qt::CaseInsensitive))
        return QStringLiteral("sent");
    if (flags.contains("\\Drafts", Qt::CaseInsensitive) || flags.contains("\\Draft", Qt::CaseInsensitive))
        return QStringLiteral("drafts");
    if (flags.contains("\\Flagged", Qt::CaseInsensitive))
        return QStringLiteral("starred");
    if (flags.contains("\\Important", Qt::CaseInsensitive))
        return QStringLiteral("important");
    if (flags.contains("\\Junk", Qt::CaseInsensitive) || flags.contains("\\Spam", Qt::CaseInsensitive))
        return QStringLiteral("spam");
    if (flags.contains("\\Trash", Qt::CaseInsensitive) || flags.contains("\\Deleted", Qt::CaseInsensitive))
        return QStringLiteral("trash");
    if (flags.contains("\\Archive", Qt::CaseInsensitive) || flags.contains("\\All", Qt::CaseInsensitive))
        return QStringLiteral("archive");

    // 2. [Gmail]/... 또는 [Google Mail]/... 시스템 전용 경로 매칭
    if (decoded.startsWith("[Gmail]/", Qt::CaseInsensitive) || decoded.startsWith("[Google Mail]/", Qt::CaseInsensitive)) {
        QString leaf = decoded.section('/', -1).trimmed().toLower();
        if (leaf == "sent mail" || leaf == "보낸편지함" || leaf == "보낸 편지함")
            return QStringLiteral("sent");
        if (leaf == "drafts" || leaf == "임시보관함" || leaf == "임시 보관함")
            return QStringLiteral("drafts");
        if (leaf == "starred" || leaf == "별표편지함" || leaf == "별표 편지함")
            return QStringLiteral("starred");
        if (leaf == "important" || leaf == "중요편지함" || leaf == "중요 편지함")
            return QStringLiteral("important");
        if (leaf == "all mail" || leaf == "전체보관함" || leaf == "전체보관메일" || leaf == "보관함")
            return QStringLiteral("archive");
        if (leaf == "spam" || leaf == "스팸함" || leaf == "스팸편지함")
            return QStringLiteral("spam");
        if (leaf == "trash" || leaf == "휴지통" || leaf == "지운편지함")
            return QStringLiteral("trash");
    }

    return QString();
}

QString classifyTopLevelStandardFolderName(const QString &decoded)
{
    // 하위 커스텀 폴더(예: Mailspring/sent)는 최상위 시스템 폴더로 매핑하지 않음
    QString clean = decoded.trimmed();
    if (clean.startsWith("INBOX.", Qt::CaseInsensitive))
        clean = clean.mid(6);
    else if (clean.startsWith("INBOX/", Qt::CaseInsensitive))
        clean = clean.mid(6);

    if (clean.contains('/') || clean.contains('.'))
        return QString();

    const QString leaf = clean.trimmed().toLower();
    if (leaf == "sent" || leaf == "sent mail" || leaf == "sent messages" || leaf == "sent items"
        || leaf == "보낸편지함" || leaf == "보낸 편지함" || leaf == "발신함")
        return QStringLiteral("sent");
    if (leaf == "drafts" || leaf == "draft" || leaf == "임시보관함" || leaf == "임시 보관함")
        return QStringLiteral("drafts");
    if (leaf == "starred" || leaf == "flagged" || leaf == "별표편지함" || leaf == "별표 편지함")
        return QStringLiteral("starred");
    if (leaf == "important" || leaf == "중요편지함" || leaf == "중요 편지함")
        return QStringLiteral("important");
    if (leaf == "archive" || leaf == "archives" || leaf == "all mail" || leaf == "all messages"
        || leaf == "보관함" || leaf == "전체보관함" || leaf == "전체 보관함")
        return QStringLiteral("archive");
    if (leaf == "spam" || leaf == "junk" || leaf == "junk e-mail" || leaf == "junk email"
        || leaf == "bulk mail" || leaf == "스팸편지함" || leaf == "스팸 편지함" || leaf == "스팸함"
        || leaf == "정크 메일" || leaf == "정크메일")
        return QStringLiteral("spam");
    if (leaf == "trash" || leaf == "deleted" || leaf == "deleted items" || leaf == "deleted messages"
        || leaf == "휴지통" || leaf == "지운편지함" || leaf == "지운 편지함")
        return QStringLiteral("trash");

    return QString();
}

struct ParsedImapListEntry {
    QString flags;
    QString rawMailbox;
    QString decodedMailbox;
};

void parseImapListResponse(const QString &respLatin1,
                           const AccountItem &account,
                           QMap<QString, QString> &outSystemFolders,
                           QList<FolderItem> &outCustomFolders)
{
    outSystemFolders.clear();
    outCustomFolders.clear();
    outSystemFolders.insert(QStringLiteral("inbox"), QStringLiteral("INBOX"));

    QList<ParsedImapListEntry> entries;
    static QRegularExpression rxList(R"RX(\*\s+LIST\s+\(([^)]*)\)\s+(?:"[^"]*"|NIL)\s+(?:"([^"]+)"|([^\r\n]+)))RX",
                                     QRegularExpression::CaseInsensitiveOption);
    QRegularExpressionMatchIterator it = rxList.globalMatch(respLatin1);
    while (it.hasNext()) {
        QRegularExpressionMatch m = it.next();
        ParsedImapListEntry e;
        e.flags = m.captured(1);
        e.rawMailbox = m.captured(2).isEmpty() ? m.captured(3).trimmed() : m.captured(2);
        e.decodedMailbox = decodeImapModifiedUtf7(e.rawMailbox).trimmed();
        entries.append(e);
    }

    QSet<QString> claimedRawMailboxes;
    claimedRawMailboxes.insert(QStringLiteral("INBOX"));

    // Pass 1: RFC 6154 SPECIAL-USE 플래그 및 [Gmail]/... 시스템 폴더 우선 매핑
    for (const ParsedImapListEntry &e : entries) {
        if (e.flags.contains("\\Noselect", Qt::CaseInsensitive) || e.flags.contains("\\NonExistent", Qt::CaseInsensitive))
            continue;
        QString sysId = classifyImapFlagOrGmailSystemFolder(e.flags, e.rawMailbox, e.decodedMailbox);
        if (!sysId.isEmpty() && !outSystemFolders.contains(sysId)) {
            outSystemFolders.insert(sysId, e.rawMailbox);
            claimedRawMailboxes.insert(e.rawMailbox);
        }
    }

    // Pass 2: RFC 6154 플래그가 없는 일반 IMAP 서버의 최상위 표준 폴더명 매핑 (대소문자 정확히 일치하는 Sent 등 우선)
    for (const ParsedImapListEntry &e : entries) {
        if (claimedRawMailboxes.contains(e.rawMailbox))
            continue;
        if (e.flags.contains("\\Noselect", Qt::CaseInsensitive) || e.flags.contains("\\NonExistent", Qt::CaseInsensitive))
            continue;
        QString sysId = classifyTopLevelStandardFolderName(e.decodedMailbox);
        if (!sysId.isEmpty() && !outSystemFolders.contains(sysId)) {
            outSystemFolders.insert(sysId, e.rawMailbox);
            claimedRawMailboxes.insert(e.rawMailbox);
        }
    }

    // Pass 3: 서버에 실제로 존재하는 나머지 폴더/라벨을 해당 계정의 내 폴더(YOUR FOLDERS)로 동적 구성
    QSet<QString> seenCustomIds;
    for (const ParsedImapListEntry &e : entries) {
        if (claimedRawMailboxes.contains(e.rawMailbox))
            continue;
        if (e.rawMailbox.compare("INBOX", Qt::CaseInsensitive) == 0)
            continue;
        if (e.decodedMailbox.compare("[Gmail]", Qt::CaseInsensitive) == 0
            || e.decodedMailbox.compare("[Google Mail]", Qt::CaseInsensitive) == 0
            || e.decodedMailbox.startsWith("[Gmail]/", Qt::CaseInsensitive)
            || e.decodedMailbox.startsWith("[Google Mail]/", Qt::CaseInsensitive))
            continue;

        bool hasChildEntry = e.flags.contains("\\HasChildren", Qt::CaseInsensitive);
        if (!hasChildEntry) {
            for (const ParsedImapListEntry &other : entries) {
                if (other.decodedMailbox.startsWith(e.decodedMailbox + "/", Qt::CaseInsensitive)
                    || other.decodedMailbox.startsWith(e.decodedMailbox + ".", Qt::CaseInsensitive)) {
                    hasChildEntry = true;
                    break;
                }
            }
        }

        // \Noselect 컨테이너이면서 하위 폴더도 없으면 건너뜀
        if ((e.flags.contains("\\Noselect", Qt::CaseInsensitive) || e.flags.contains("\\NonExistent", Qt::CaseInsensitive))
            && !hasChildEntry) {
            continue;
        }

        QString cleanPath = e.decodedMailbox;
        if (cleanPath.startsWith("INBOX.", Qt::CaseInsensitive))
            cleanPath = cleanPath.mid(6);
        else if (cleanPath.startsWith("INBOX/", Qt::CaseInsensitive))
            cleanPath = cleanPath.mid(6);

        QString displayName = cleanPath;
        if (displayName.contains('/'))
            displayName = displayName.section('/', -1).trimmed();
        else if (displayName.contains('.'))
            displayName = displayName.section('.', -1).trimmed();

        if (displayName.isEmpty())
            continue;

        FolderItem cf;
        cf.accountId = account.id;
        cf.accountEmail = account.email;
        cf.id = QString("custom_%1_%2").arg(account.id, e.rawMailbox);
        cf.name = displayName;
        cf.rawImapMailbox = e.rawMailbox;
        cf.kind = FolderKind::CustomFolder;
        cf.isCustom = true;
        cf.hasChildren = hasChildEntry;
        cf.dotColor = QColor("#3B82F6");

        if (!seenCustomIds.contains(cf.id)) {
            seenCustomIds.insert(cf.id);
            outCustomFolders.append(cf);
        }
    }
}

QString resolveImapMailboxFromList(const QString &respLatin1,
                                   const AccountItem &account,
                                   const QString &folderId,
                                   QList<FolderItem> *outDiscoveredCustom = nullptr)
{
    QMap<QString, QString> sysFolders;
    QList<FolderItem> customFolders;
    parseImapListResponse(respLatin1, account, sysFolders, customFolders);
    if (outDiscoveredCustom)
        *outDiscoveredCustom = customFolders;

    if (sysFolders.contains(folderId))
        return sysFolders.value(folderId);

    for (const FolderItem &cf : customFolders) {
        if (cf.id == folderId
            || cf.rawImapMailbox.compare(folderId, Qt::CaseInsensitive) == 0
            || cf.name.compare(folderId, Qt::CaseInsensitive) == 0) {
            return cf.rawImapMailbox;
        }
    }

    const QString customPrefix = QString("custom_%1_").arg(account.id);
    if (folderId.startsWith(customPrefix)) {
        const QString tail = folderId.mid(customPrefix.size());
        static const QRegularExpression rxLegacyUuid(QStringLiteral("^[0-9a-f]{6}$"),
                                                     QRegularExpression::CaseInsensitiveOption);
        if (!rxLegacyUuid.match(tail).hasMatch()) {
            return tail;
        }
    }

    return QString();
}

QString buildImapCustomMailboxCandidate(const QString &respLatin1, const QString &folderName)
{
    QString clean = folderName.trimmed();
    clean.remove(QLatin1Char('"'));
    if (clean.isEmpty())
        return QString();

    const QString encoded = encodeImapModifiedUtf7(clean);
    const bool usesInboxDotNamespace =
        respLatin1.contains(QStringLiteral("\"INBOX.Sent\""), Qt::CaseInsensitive)
        || respLatin1.contains(QStringLiteral("\"INBOX.Trash\""), Qt::CaseInsensitive)
        || respLatin1.contains(QStringLiteral("\"INBOX.Drafts\""), Qt::CaseInsensitive)
        || respLatin1.contains(QStringLiteral(" INBOX.Sent\r\n"), Qt::CaseInsensitive)
        || respLatin1.contains(QStringLiteral(" INBOX.Trash\r\n"), Qt::CaseInsensitive);
    if (usesInboxDotNamespace && !encoded.startsWith(QStringLiteral("INBOX."), Qt::CaseInsensitive)) {
        return QStringLiteral("INBOX.") + encoded;
    }
    return encoded;
}

QString folderTagForId(const QString &folderId)
{
    if (folderId == "inbox")     return QStringLiteral("Inbox");
    if (folderId == "starred")   return QStringLiteral("Starred");
    if (folderId == "drafts")    return QStringLiteral("Drafts");
    if (folderId == "sent")      return QStringLiteral("Sent");
    if (folderId == "archive")   return QStringLiteral("Archive");
    if (folderId == "snoozed")   return QStringLiteral("Snoozed");
    if (folderId == "important") return QStringLiteral("Important");
    if (folderId == "spam")      return QStringLiteral("Spam");
    if (folderId == "trash")     return QStringLiteral("Trash");
    if (folderId.startsWith("custom_")) {
        QString raw = folderId.section('_', 2);
        QString dec = decodeImapModifiedUtf7(raw);
        if (dec.contains('/'))
            dec = dec.section('/', -1);
        else if (dec.contains('.'))
            dec = dec.section('.', -1);
        return dec.isEmpty() ? folderId : dec;
    }
    return folderId;
}

bool hasHighImportanceHeader(const QByteArray &rawEmail)
{
    int hdrEndPos = rawEmail.indexOf("\r\n\r\n");
    if (hdrEndPos < 0)
        hdrEndPos = rawEmail.indexOf("\n\n");
    const QString hdrText = QString::fromLatin1((hdrEndPos > 0) ? rawEmail.left(hdrEndPos) : rawEmail.left(4096));
    static const QRegularExpression rxHighPriority(
        R"((?:^|\r?\n)\s*(?:X-Priority\s*:\s*[12]\b|Importance\s*:\s*high\b|X-MSMail-Priority\s*:\s*high\b|Priority\s*:\s*(?:urgent|high)\b))",
        QRegularExpression::CaseInsensitiveOption);
    return rxHighPriority.match(hdrText).hasMatch();
}

} // namespace

void GoogleProvider::fetchImapInbox(const AccountItem &account, int maxMessages)
{
    if (account.isPop3()) {
        fetchPop3Inbox(account, maxMessages, 0);
        return;
    }
    fetchImapFolder(account, QStringLiteral("inbox"), maxMessages, 0);
}

void GoogleProvider::fetchPop3Inbox(const AccountItem &account, int maxMessages, int endSeq)
{
    if (account.imapHost.isEmpty())
        return;
    if (account.appPassword.isEmpty())
        return;

    // 실행 파일 경로 아래에 POP3 계정 전용 로컬 메일함 폴더(<실행파일경로>/pop3/<계정폴더>/inbox, sent, drafts, trash 등) 자동 생성
    account.ensurePop3MailboxDirs();

    QSslSocket *socket = new QSslSocket(this);
    int *step = new int(0);
    QByteArray *buffer = new QByteArray();
    int *totalExists = new int(0);
    int *startSeqOut = new int(1);
    int *curRetrSeq = new int(0);
    int *newRetrCount = new int(0);
    bool *pageEmitted = new bool(false);
    QList<ConversationItem> *convs = new QList<ConversationItem>();

    emit statusMessage(
        MailI18n::t(QString("[%1] %2:%3 POP3 받는 서버 연결 중... (서버에 복사본 저장: %4)"),
                    QString("[%1] Connecting to POP3 %2:%3... (Leave copy on server: %4)"),
                    QString("[%1] POP3 %2:%3 に接続中..."))
            .arg(account.email, account.imapHost)
            .arg(account.imapPort)
            .arg(account.pop3LeaveCopyOnServer ? QStringLiteral("ON") : QStringLiteral("OFF")));

    connect(socket, &QSslSocket::readyRead, this,
            [this, socket, account, maxMessages, endSeq, step, buffer, totalExists, startSeqOut, curRetrSeq, newRetrCount, pageEmitted, convs]() {
        if (!socket->isOpen())
            return;
        buffer->append(socket->readAll());

        auto parseAndQueueNext = [this, socket, account, endSeq, step, buffer, totalExists, startSeqOut, curRetrSeq, newRetrCount, pageEmitted, convs]() {
            const QString pop3Root = account.pop3AccountRootDir();
            const QStringList subDirs = (!pop3Root.isEmpty() && QDir(pop3Root).exists())
                                            ? QDir(pop3Root).entryList(QDir::Dirs | QDir::NoDotAndDotDot)
                                            : QStringList();

            // POP3 신규 메일 전용 수신: 이미 로컬 디스크(inbox, trash, spam, archive, custom_* 등)에 다운로드된 msg_<seq>.eml 은
            // 네트워크 RETR를 생략(Skip)하되, 로컬 inbox에 존재하는 .eml은 디스크에서 바로 읽어 convs 목록에 포함시킨다.
            while (*curRetrSeq >= *startSeqOut) {
                bool alreadyDownloaded = false;
                QString existingSubDir;
                for (const QString &subName : subDirs) {
                    if (subName.compare(QStringLiteral("attachments"), Qt::CaseInsensitive) == 0)
                        continue;
                    if (QFileInfo::exists(pop3Root + QString("/%1/msg_%2.eml").arg(subName).arg(*curRetrSeq))) {
                        alreadyDownloaded = true;
                        existingSubDir = subName;
                        break;
                    }
                }
                if (!alreadyDownloaded) {
                    break;
                }
                if (existingSubDir.compare(QStringLiteral("inbox"), Qt::CaseInsensitive) == 0) {
                    const int seqNum = *curRetrSeq;
                    QFile emlFile(pop3Root + QString("/inbox/msg_%1.eml").arg(seqNum));
                    if (emlFile.open(QIODevice::ReadOnly)) {
                        const QByteArray rawEmail = emlFile.readAll();
                        emlFile.close();
                        if (!rawEmail.isEmpty()) {
                            ConversationItem c;
                            c.id = QString("imap_%1_inbox_%2").arg(account.id).arg(seqNum);
                            c.imapUid = seqNum;
                            c.imapSeq = seqNum;
                            QString htmlBody, plainBody, fromLine, subjLine, dateLine, toLine;
                            QList<AttachmentItem> parsedAtts;
                            parseMimeEntity(rawEmail, htmlBody, plainBody, &fromLine, &subjLine, &dateLine, &toLine,
                                            &parsedAtts, c.id, true, false);
                            if (!parsedAtts.isEmpty()) {
                                const QString pop3AttDir = account.pop3MailboxDir(QStringLiteral("attachments"));
                                QDir().mkpath(pop3AttDir);
                                for (AttachmentItem &att : parsedAtts) {
                                    if (!att.filePath.isEmpty() && QFileInfo::exists(att.filePath)) {
                                        const QString destPath = pop3AttDir + QString("/%1_%2")
                                                                                  .arg(seqNum)
                                                                                  .arg(QFileInfo(att.filePath).fileName());
                                        if (att.filePath != destPath) {
                                            QFile::remove(destPath);
                                            if (QFile::copy(att.filePath, destPath)) {
                                                att.filePath = destPath;
                                            }
                                        }
                                    }
                                }
                            }
                            if (!fromLine.isEmpty() || !subjLine.isEmpty() || !htmlBody.isEmpty() || !plainBody.isEmpty() || !parsedAtts.isEmpty()) {
                                c.accountId = account.id;
                                c.folderId = QStringLiteral("inbox");
                                c.folderTag = QStringLiteral("Inbox");
                                c.subject = subjLine.isEmpty() ? QStringLiteral("(제목 없음)") : subjLine;
                                c.isUnread = false;
                                c.isStarred = false;
                                c.isImportant = hasHighImportanceHeader(rawEmail);

                                MessageItem msg;
                                msg.id = c.id + "_m1";
                                msg.attachments = parsedAtts;
                                QString rawSenderName = fromLine.section('<', 0, 0).remove('"').trimmed();
                                if (rawSenderName.isEmpty())
                                    rawSenderName = fromLine;
                                msg.senderEmail = fromLine.section('<', 1, 1).section('>', 0, 0).trimmed();
                                if (msg.senderEmail.isEmpty())
                                    msg.senderEmail = fromLine;
                                msg.senderName = rawSenderName;
                                msg.senderInitials = msg.senderName.left(2).toUpper();
                                msg.senderAvatarBg = QColor("#DBEAFE");
                                msg.senderAvatarFg = QColor("#1D4ED8");
                                msg.recipientSummary = toLine.trimmed().isEmpty() ? account.email : toLine.trimmed();
                                msg.shortTimeText = dateLine.left(16);
                                msg.fullTimeText = dateLine;
                                msg.subject = c.subject;
                                msg.snippet = htmlToPlainTextSnippet(htmlBody, plainBody, 50);
                                if (msg.snippet.isEmpty())
                                    msg.snippet = c.subject;
                                msg.htmlBody = sanitizeEmailHtmlForViewer(htmlBody, plainBody);

                                c.messages.append(msg);
                                convs->append(c);
                            }
                        }
                    }
                }
                --(*curRetrSeq);
            }

            if (*curRetrSeq < *startSeqOut) {
                *pageEmitted = true;
                emit imapFolderPageFetched(account.id, QStringLiteral("inbox"), *convs, *startSeqOut, *totalExists, endSeq <= 0);
                if (endSeq <= 0)
                    emit imapFolderConversationsFetched(account.id, QStringLiteral("inbox"), *convs);
                emit statusMessage(
                    MailI18n::t(QString("[%1] POP3 받은편지함 전체 %2건 표시 (신규 수신 %3건 · 서버 전체 %4건)"),
                                QString("[%1] POP3 Inbox %2 messages (%3 new / %4 on server)"),
                                QString("[%1] POP3 受信トレイ %2件表示 (新着%3件 / サーバー全%4件)"))
                        .arg(account.email)
                        .arg(convs->size())
                        .arg(*newRetrCount)
                        .arg(*totalExists));
                *buffer = QByteArray();
                *step = 99;
                socket->write("QUIT\r\n");
                return;
            }
            *buffer = QByteArray();
            *step = 4;
            socket->write(QString("RETR %1\r\n").arg(*curRetrSeq).toLatin1());
        };

        // Step 0..3: single-line POP3 responses (+OK / -ERR ending with \r\n)
        if (*step < 4) {
            if (!buffer->endsWith("\r\n"))
                return;
            const QString resp = QString::fromLatin1(*buffer);
            if (resp.startsWith("-ERR", Qt::CaseInsensitive)) {
                emit statusMessage(MailI18n::t(QString("[%1] POP3 오류: %2"),
                                               QString("[%1] POP3 error: %2"),
                                               QString("[%1] POP3 エラー: %2"))
                                       .arg(account.email, resp.trimmed()));
                *pageEmitted = true;
                emit imapFolderPageFetched(account.id, QStringLiteral("inbox"), {}, 1, 0, endSeq <= 0);
                socket->write("QUIT\r\n");
                socket->disconnectFromHost();
                return;
            }
            if (!resp.startsWith("+OK", Qt::CaseInsensitive))
                return;

            if (*step == 0) {
                *buffer = QByteArray();
                *step = 1;
                socket->write(QString("USER %1\r\n").arg(account.effectiveImapUser()).toUtf8());
            } else if (*step == 1) {
                *buffer = QByteArray();
                *step = 2;
                socket->write(QString("PASS %1\r\n").arg(account.appPassword).toUtf8());
            } else if (*step == 2) {
                *buffer = QByteArray();
                *step = 3;
                socket->write("STAT\r\n");
            } else if (*step == 3) {
                // RFC 1939 STAT response: "+OK nn mm\r\n"
                static const QRegularExpression rxStat(R"(^\+OK\s+(\d+))", QRegularExpression::CaseInsensitiveOption);
                const QRegularExpressionMatch m = rxStat.match(resp);
                const int existsCount = m.hasMatch() ? m.captured(1).toInt() : 0;
                *totalExists = existsCount;
                if (existsCount <= 0) {
                    *pageEmitted = true;
                    emit imapFolderPageFetched(account.id, QStringLiteral("inbox"), {}, 1, 0, endSeq <= 0);
                    *buffer = QByteArray();
                    *step = 99;
                    socket->write("QUIT\r\n");
                    return;
                }
                const int pageSize = maxMessages > 0 ? qMin(maxMessages, 35) : 30;
                const int actualEnd = (endSeq > 0 && endSeq <= existsCount) ? endSeq : existsCount;
                const int startSeq = qMax(1, actualEnd - pageSize + 1);
                *startSeqOut = startSeq;
                *curRetrSeq = actualEnd;
                parseAndQueueNext();
            }
        } else if (*step == 4) {
            // RFC 1939 RETR multi-line response: starts with "+OK...\r\n" and ends with "\r\n.\r\n"
            if (buffer->startsWith("-ERR")) {
                --(*curRetrSeq);
                parseAndQueueNext();
                return;
            }
            if (!buffer->endsWith("\r\n.\r\n"))
                return;

            const int firstCrlf = buffer->indexOf("\r\n");
            if (firstCrlf > 0) {
                QByteArray rawEmail = buffer->mid(firstCrlf + 2, buffer->size() - (firstCrlf + 2) - 5);
                // RFC 1939 Section 3: byte-stuffed termination dot un-stuffing ("\r\n.." -> "\r\n.")
                rawEmail.replace("\r\n..", "\r\n.");

                const int seqNum = *curRetrSeq;
                ConversationItem c;
                c.id = QString("imap_%1_inbox_%2").arg(account.id).arg(seqNum);
                c.imapUid = seqNum;
                c.imapSeq = seqNum;

                // 이미 다른 로컬 POP3 폴더(trash, spam, archive, custom_...)로 이동된 메일인지 확인
                const QString pop3Root = account.pop3AccountRootDir();
                bool alreadyMovedToOtherLocalFolder = false;
                if (!pop3Root.isEmpty() && QDir(pop3Root).exists()) {
                    const QStringList subDirs = QDir(pop3Root).entryList(QDir::Dirs | QDir::NoDotAndDotDot);
                    for (const QString &subName : subDirs) {
                        if (subName.compare(QStringLiteral("inbox"), Qt::CaseInsensitive) == 0
                            || subName.compare(QStringLiteral("attachments"), Qt::CaseInsensitive) == 0) {
                            continue;
                        }
                        if (QFileInfo::exists(pop3Root + QString("/%1/msg_%2.eml").arg(subName).arg(seqNum))) {
                            alreadyMovedToOtherLocalFolder = true;
                            break;
                        }
                    }
                }

                // 수신된 POP3 메일 원문(.eml)을 <실행파일경로>/pop3/<계정폴더>/inbox/msg_<번호>.eml 에 저장 (타 폴더로 이동되지 않은 경우에만)
                if (!alreadyMovedToOtherLocalFolder) {
                    const QString inboxDir = account.pop3MailboxDir(QStringLiteral("inbox"));
                    QDir().mkpath(inboxDir);
                    QFile emlFile(inboxDir + QString("/msg_%1.eml").arg(seqNum));
                    if (emlFile.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
                        emlFile.write(rawEmail);
                        emlFile.close();
                    }
                    ++(*newRetrCount);

                    QString htmlBody, plainBody, fromLine, subjLine, dateLine, toLine;
                    QList<AttachmentItem> parsedAtts;
                    parseMimeEntity(rawEmail, htmlBody, plainBody, &fromLine, &subjLine, &dateLine, &toLine,
                                    &parsedAtts, c.id, true, false);

                    // 첨부파일도 해당 POP3 계정 폴더(<실행파일경로>/pop3/<계정폴더>/attachments/)로 복사·보관
                    if (!parsedAtts.isEmpty()) {
                        const QString pop3AttDir = account.pop3MailboxDir(QStringLiteral("attachments"));
                        QDir().mkpath(pop3AttDir);
                        for (AttachmentItem &att : parsedAtts) {
                            if (!att.filePath.isEmpty() && QFileInfo::exists(att.filePath)) {
                                const QString destPath = pop3AttDir + QString("/%1_%2")
                                                                          .arg(seqNum)
                                                                          .arg(QFileInfo(att.filePath).fileName());
                                if (att.filePath != destPath) {
                                    QFile::remove(destPath);
                                    if (QFile::copy(att.filePath, destPath)) {
                                        att.filePath = destPath;
                                    }
                                }
                            }
                        }
                    }

                    if (!fromLine.isEmpty() || !subjLine.isEmpty() || !htmlBody.isEmpty() || !plainBody.isEmpty() || !parsedAtts.isEmpty()) {
                        c.accountId = account.id;
                        c.folderId = QStringLiteral("inbox");
                        c.folderTag = QStringLiteral("Inbox");
                        c.subject = subjLine.isEmpty() ? QStringLiteral("(제목 없음)") : subjLine;
                        c.isUnread = true;
                        c.isStarred = false;
                        c.isImportant = hasHighImportanceHeader(rawEmail);

                        MessageItem msg;
                        msg.id = c.id + "_m1";
                        msg.attachments = parsedAtts;
                        QString rawSenderName = fromLine.section('<', 0, 0).remove('"').trimmed();
                        if (rawSenderName.isEmpty())
                            rawSenderName = fromLine;
                        msg.senderEmail = fromLine.section('<', 1, 1).section('>', 0, 0).trimmed();
                        if (msg.senderEmail.isEmpty())
                            msg.senderEmail = fromLine;
                        msg.senderName = rawSenderName;
                        msg.senderInitials = msg.senderName.left(2).toUpper();
                        msg.senderAvatarBg = QColor("#DBEAFE");
                        msg.senderAvatarFg = QColor("#1D4ED8");
                        msg.recipientSummary = toLine.trimmed().isEmpty() ? account.email : toLine.trimmed();
                        msg.shortTimeText = dateLine.left(16);
                        msg.fullTimeText = dateLine;
                        msg.subject = c.subject;
                        msg.snippet = htmlToPlainTextSnippet(htmlBody, plainBody, 50);
                        if (msg.snippet.isEmpty())
                            msg.snippet = c.subject;
                        msg.htmlBody = sanitizeEmailHtmlForViewer(htmlBody, plainBody);

                        c.messages.append(msg);
                        convs->append(c);
                    }
                }
            }

            // 기본값(pop3LeaveCopyOnServer == true): 절대 DELE를 보내지 않고 서버에 복사본 보존!
            if (!account.pop3LeaveCopyOnServer) {
                *buffer = QByteArray();
                *step = 5;
                socket->write(QString("DELE %1\r\n").arg(*curRetrSeq).toLatin1());
                return;
            }
            --(*curRetrSeq);
            parseAndQueueNext();
        } else if (*step == 5) {
            if (!buffer->endsWith("\r\n"))
                return;
            --(*curRetrSeq);
            parseAndQueueNext();
        } else if (*step == 99) {
            socket->disconnectFromHost();
        }
    });

    bindSocketLifecycleCleanup(
        socket,
        [this, account, endSeq, startSeqOut, totalExists, pageEmitted, convs]() {
            if (!m_isShuttingDown && !(*pageEmitted)) {
                *pageEmitted = true;
                emit imapFolderPageFetched(account.id, QStringLiteral("inbox"), *convs, *startSeqOut, *totalExists, endSeq <= 0);
            }
        },
        [step, buffer, totalExists, startSeqOut, curRetrSeq, newRetrCount, pageEmitted, convs]() {
            delete step;
            delete buffer;
            delete totalExists;
            delete startSeqOut;
            delete curRetrSeq;
            delete newRetrCount;
            delete pageEmitted;
            delete convs;
        });

    const bool useSsl = (account.imapPort == 995 || account.imapSecurity.compare("SSL/TLS", Qt::CaseInsensitive) == 0);
    if (useSsl) {
        socket->connectToHostEncrypted(account.imapHost, account.imapPort > 0 ? account.imapPort : 995);
    } else {
        socket->connectToHost(account.imapHost, account.imapPort > 0 ? account.imapPort : 110);
    }
}

void GoogleProvider::checkPop3InboxQuick(const AccountItem &account, int knownExistsCount)
{
    if (account.imapHost.isEmpty() || account.appPassword.isEmpty())
        return;

    // POP3 30초 주기 초경량 확인: RFC 1939 STAT 명령(+OK <건수> <바이트>)만 확인하고 변동이 없으면 즉시 QUIT 종료
    QSslSocket *socket = new QSslSocket(this);
    int *step = new int(0);
    QByteArray *buffer = new QByteArray();

    connect(socket, &QSslSocket::readyRead, this,
            [this, socket, account, knownExistsCount, step, buffer]() {
        if (!socket->isOpen())
            return;
        buffer->append(socket->readAll());
        if (!buffer->endsWith("\r\n"))
            return;

        const QString resp = QString::fromLatin1(*buffer);
        if (resp.startsWith("-ERR", Qt::CaseInsensitive)) {
            socket->write("QUIT\r\n");
            socket->disconnectFromHost();
            return;
        }
        if (!resp.startsWith("+OK", Qt::CaseInsensitive))
            return;

        if (*step == 0) {
            *buffer = QByteArray();
            *step = 1;
            socket->write(QString("USER %1\r\n").arg(account.effectiveImapUser()).toUtf8());
        } else if (*step == 1) {
            *buffer = QByteArray();
            *step = 2;
            socket->write(QString("PASS %1\r\n").arg(account.appPassword).toUtf8());
        } else if (*step == 2) {
            *buffer = QByteArray();
            *step = 3;
            socket->write("STAT\r\n");
        } else if (*step == 3) {
            static const QRegularExpression rxStat(R"(^\+OK\s+(\d+))", QRegularExpression::CaseInsensitiveOption);
            const QRegularExpressionMatch m = rxStat.match(resp);
            const int existsCount = m.hasMatch() ? m.captured(1).toInt() : -1;
            *buffer = QByteArray();
            *step = 4;
            socket->write("QUIT\r\n");

            if (existsCount >= 0 && existsCount != knownExistsCount) {
                const int fetchCount = (knownExistsCount > 0 && existsCount > knownExistsCount)
                                           ? qBound(5, existsCount - knownExistsCount + 5, 25)
                                           : 25;
                fetchPop3Inbox(account, fetchCount, 0);
            }
        } else {
            socket->disconnectFromHost();
        }
    });

    bindSocketLifecycleCleanup(
        socket,
        nullptr,
        [step, buffer]() {
            delete step;
            delete buffer;
        });

    const bool useSsl = (account.imapPort == 995 || account.imapSecurity.compare("SSL/TLS", Qt::CaseInsensitive) == 0);
    if (useSsl) {
        socket->connectToHostEncrypted(account.imapHost, account.imapPort > 0 ? account.imapPort : 995);
    } else {
        socket->connectToHost(account.imapHost, account.imapPort > 0 ? account.imapPort : 110);
    }
}

void GoogleProvider::fetchImapFolder(const AccountItem &account, const QString &folderId, int maxMessages, int endSeq)
{
    if (account.imapHost.isEmpty())
        return;
    if (account.appPassword.isEmpty() && account.oauthAccessToken.isEmpty())
        return;
    if (account.isPop3()) {
        // POP3(RFC 1939)는 서버에 하위 폴더 개념이 없고 단일 INBOX만 존재하므로
        // 받은편지함(inbox)은 POP3 서버에서 조회하고, 나머지 로컬 폴더(sent, drafts, archive, spam, trash, custom_...)는
        // 로컬 디스크(<저장경로>/pop3/<계정이메일>/<folderId>/msg_*.eml)에서 직접 읽어 동기화한다.
        if (folderId == QStringLiteral("inbox")) {
            fetchPop3Inbox(account, maxMessages, endSeq);
        } else {
            account.ensurePop3MailboxDirs();
            const QString localBoxDir = account.pop3MailboxDir(folderId);
            QList<ConversationItem> localConvs;
            QDir dir(localBoxDir);
            if (dir.exists()) {
                const QFileInfoList emlFiles = dir.entryInfoList(
                    QStringList() << QStringLiteral("*.eml"),
                    QDir::Files,
                    QDir::Name);
                int fallbackIdx = 1;
                for (const QFileInfo &fi : emlFiles) {
                    QFile f(fi.absoluteFilePath());
                    if (!f.open(QIODevice::ReadOnly))
                        continue;
                    const QByteArray rawEmail = f.readAll();
                    f.close();
                    if (rawEmail.isEmpty())
                        continue;

                    QString baseName = fi.completeBaseName(); // e.g. "msg_12"
                    if (baseName.startsWith(QStringLiteral("msg_"), Qt::CaseInsensitive))
                        baseName = baseName.mid(4);
                    bool okSeq = false;
                    int seqNum = baseName.toInt(&okSeq);
                    if (!okSeq || seqNum <= 0)
                        seqNum = fallbackIdx++;

                    ConversationItem c;
                    c.imapUid = seqNum;
                    c.imapSeq = seqNum;
                    c.id = QString("imap_%1_%2_%3").arg(account.id, folderId).arg(seqNum);

                    QString htmlBody, plainBody, fromLine, subjLine, dateLine, toLine;
                    QList<AttachmentItem> parsedAtts;
                    parseMimeEntity(rawEmail, htmlBody, plainBody, &fromLine, &subjLine, &dateLine, &toLine,
                                    &parsedAtts, c.id, true, false);
                    if (fromLine.isEmpty() && subjLine.isEmpty() && htmlBody.isEmpty() && plainBody.isEmpty() && parsedAtts.isEmpty())
                        continue;

                    c.accountId = account.id;
                    c.folderId = folderId;
                    c.folderTag = folderTagForId(folderId);
                    c.subject = subjLine.isEmpty() ? QStringLiteral("(제목 없음)") : subjLine;
                    c.isUnread = false;
                    c.isStarred = (folderId == QStringLiteral("starred"));
                    c.isImportant = (folderId == QStringLiteral("important")) || hasHighImportanceHeader(rawEmail);
                    {
                        int hdrEndPos = rawEmail.indexOf("\r\n\r\n");
                        if (hdrEndPos < 0)
                            hdrEndPos = rawEmail.indexOf("\n\n");
                        const QByteArray rawHdrLower = ((hdrEndPos > 0) ? rawEmail.left(hdrEndPos) : rawEmail.left(4096)).toLower();
                        if (rawHdrLower.contains("disposition-notification-to:")
                            || rawHdrLower.contains("return-receipt-to:")
                            || rawHdrLower.contains("x-confirm-reading-to:")) {
                            c.readReceiptRequested = true;
                        }
                    }

                    MessageItem msg;
                    msg.id = c.id + "_m1";
                    msg.attachments = parsedAtts;
                    QString rawSenderName = fromLine.section('<', 0, 0).remove('"').trimmed();
                    if (rawSenderName.isEmpty())
                        rawSenderName = fromLine;
                    msg.senderEmail = fromLine.section('<', 1, 1).section('>', 0, 0).trimmed();
                    if (msg.senderEmail.isEmpty())
                        msg.senderEmail = fromLine;
                    if ((folderId == QStringLiteral("sent") || folderId == QStringLiteral("custom_sent")) && !toLine.trimmed().isEmpty()) {
                        QString toName = toLine.section('<', 0, 0).remove('"').trimmed();
                        if (toName.isEmpty())
                            toName = toLine.trimmed();
                        msg.senderName = QString("To: %1").arg(toName);
                        msg.senderInitials = toName.left(2).toUpper();
                    } else {
                        msg.senderName = rawSenderName;
                        msg.senderInitials = msg.senderName.left(2).toUpper();
                    }
                    msg.senderAvatarBg = QColor("#DBEAFE");
                    msg.senderAvatarFg = QColor("#1D4ED8");
                    msg.recipientSummary = toLine.trimmed().isEmpty() ? account.email : toLine.trimmed();
                    msg.shortTimeText = dateLine.left(16);
                    msg.fullTimeText = dateLine;
                    msg.subject = c.subject;
                    msg.snippet = htmlToPlainTextSnippet(htmlBody, plainBody, 50);
                    if (msg.snippet.isEmpty())
                        msg.snippet = c.subject;
                    msg.htmlBody = sanitizeEmailHtmlForViewer(htmlBody, plainBody);

                    c.messages.append(msg);
                    localConvs.prepend(c);
                }
            }
            emit imapFolderPageFetched(account.id, folderId, localConvs, 1, localConvs.size(), endSeq <= 0);
        }
        return;
    }

    // 단일 폴더 조회 및 페이지 단위(1,000건) RFC 3501 UID 기반 조회 (이미 로드된 UID 중복 다운로드 원천 차단)
    QSslSocket *socket = new QSslSocket(this);
    int *step = new int(0);
    QByteArray *buffer = new QByteArray();
    QString *resolvedMailbox = new QString("INBOX");
    int *totalExists = new int(0);
    int *startSeqOut = new int(1);
    bool *pageEmitted = new bool(false);
    QMap<int, int> *seqToUid = new QMap<int, int>();
    QList<ConversationItem> *cachedMetaConvs = new QList<ConversationItem>();

    connect(socket, &QSslSocket::readyRead, this,
            [this, socket, account, folderId, maxMessages, endSeq, step, buffer, resolvedMailbox, totalExists, startSeqOut, pageEmitted, seqToUid, cachedMetaConvs]() {
        if (!socket->isOpen())
            return;
        buffer->append(socket->readAll());
        if (!buffer->endsWith("\r\n"))
            return;

        QString respLatin1 = QString::fromLatin1(*buffer);
        if (*step == 0 && respLatin1.contains("* OK")) {
            *buffer = QByteArray();
            *step = 1;
            if (!account.oauthAccessToken.isEmpty() && !account.useSeparateSmtpAuth && account.imapUsername.isEmpty()) {
                QByteArray xoauth = QString("user=%1\x01" "auth=Bearer %2\x01\x01")
                                        .arg(account.email, account.oauthAccessToken)
                                        .toUtf8()
                                        .toBase64();
                socket->write("A01 AUTHENTICATE XOAUTH2 " + xoauth + "\r\n");
            } else {
                QString cmd = QString("A01 LOGIN \"%1\" \"%2\"\r\n")
                                  .arg(account.effectiveImapUser(), account.appPassword);
                socket->write(cmd.toUtf8());
            }
        } else if (*step == 1 && (respLatin1.startsWith("+ ") || respLatin1.contains("\r\n+ "))
                   && !respLatin1.contains("A01 NO") && !respLatin1.contains("A01 BAD")) {
            socket->write("\r\n");
        } else if (*step == 1 && (respLatin1.contains("A01 OK") || respLatin1.contains("A01 NO") || respLatin1.contains("A01 BAD"))) {
            if (!respLatin1.contains("A01 OK")) {
                if (!account.oauthRefreshToken.isEmpty()) {
                    refreshGoogleAccessToken(account.email, account.oauthRefreshToken);
                }
                socket->disconnectFromHost();
                return;
            }
            *buffer = QByteArray();
            *step = 2;
            socket->write("A02 LIST \"\" \"*\"\r\n");
        } else if (*step == 2 && (respLatin1.contains("\r\nA02 OK") || respLatin1.startsWith("A02 OK")
                                  || respLatin1.contains("A02 NO") || respLatin1.contains("A02 BAD"))) {
            QList<FolderItem> discoveredCustom;
            *resolvedMailbox = resolveImapMailboxFromList(respLatin1, account, folderId, &discoveredCustom);
            emit imapCustomFoldersDiscovered(account.id, discoveredCustom);

            *buffer = QByteArray();
            if (resolvedMailbox->isEmpty()) {
                *pageEmitted = true;
                emit imapFolderPageFetched(account.id, folderId, {}, 1, 0, endSeq <= 0);
                *step = 5;
                socket->write("A05 LOGOUT\r\n");
                return;
            }
            *step = 3;
            QString examineCmd = QString("A03 EXAMINE \"%1\"\r\n").arg(*resolvedMailbox);
            socket->write(examineCmd.toLatin1());
        } else if (*step == 3 && (respLatin1.contains("\r\nA03 OK") || respLatin1.startsWith("A03 OK")
                                  || respLatin1.contains("A03 NO") || respLatin1.contains("A03 BAD"))) {
            if (!respLatin1.contains("A03 OK")) {
                *pageEmitted = true;
                emit imapFolderPageFetched(account.id, folderId, {}, 1, 0, endSeq <= 0);
                *step = 5;
                socket->write("A05 LOGOUT\r\n");
                return;
            }
            int existsCount = 0;
            static QRegularExpression rxExists(R"(\*\s+(\d+)\s+EXISTS)");
            QRegularExpressionMatch m = rxExists.match(respLatin1);
            if (m.hasMatch())
                existsCount = m.captured(1).toInt();

            *totalExists = existsCount;
            *buffer = QByteArray();
            if (existsCount <= 0) {
                clearLoadedImapUids(account.id, folderId);
                *pageEmitted = true;
                emit imapFolderPageFetched(account.id, folderId, {}, 1, 0, endSeq <= 0);
                if (endSeq <= 0)
                    emit imapFolderConversationsFetched(account.id, folderId, {});
                *step = 5;
                socket->write("A05 LOGOUT\r\n");
                return;
            }
            int pageSize = maxMessages > 0 ? maxMessages : 200;
            int actualEnd = (endSeq > 0 && endSeq <= existsCount) ? endSeq : existsCount;
            int startSeq = qMax(1, actualEnd - pageSize + 1);
            *startSeqOut = startSeq;
            // Step 1-A: 먼저 해당 페이지 구간의 고유 UID와 FLAGS만 초경량 조회하여 이미 로컬에 로드된 UID는 중복 다운로드에서 제외!
            *step = 40;
            QString uidCheckCmd = QString("A04U FETCH %1:%2 (UID FLAGS)\r\n").arg(startSeq).arg(actualEnd);
            socket->write(uidCheckCmd.toUtf8());
        } else if (*step == 40 && (respLatin1.contains("\r\nA04U OK") || respLatin1.startsWith("A04U OK")
                                   || respLatin1.contains("\r\nA04U NO") || respLatin1.startsWith("A04U NO")
                                   || respLatin1.contains("\r\nA04U BAD") || respLatin1.startsWith("A04U BAD"))) {
            static QRegularExpression rxUidFlagsLine(R"(\*\s+(\d+)\s+FETCH\s*\(([^\r\n]+)\))",
                                                     QRegularExpression::CaseInsensitiveOption);
            static QRegularExpression rxUid(R"(\bUID\s+(\d+))", QRegularExpression::CaseInsensitiveOption);
            const QString folderKey = account.id + QStringLiteral("|") + folderId;
            const QSet<int> alreadyLoadedUids = m_loadedFolderUids.value(folderKey);
            QList<int> missingUids;
            int minMissingSeq = 0;
            int maxMissingSeq = 0;
            seqToUid->clear();
            cachedMetaConvs->clear();

            QRegularExpressionMatchIterator it = rxUidFlagsLine.globalMatch(respLatin1);
            while (it.hasNext()) {
                QRegularExpressionMatch m = it.next();
                int seqNum = m.captured(1).toInt();
                QString meta = m.captured(2);
                int uid = rxUid.match(meta).captured(1).toInt();
                if (uid <= 0)
                    uid = seqNum;
                if (seqNum > 0 && uid > 0) {
                    seqToUid->insert(seqNum, uid);
                    if (alreadyLoadedUids.contains(uid)) {
                        // 이미 로컬 메모리에 보유 중인 UID는 헤더/본문을 다시 다운로드하지 않고 현재 순번(imapSeq)과 플래그만 갱신
                        ConversationItem c;
                        c.imapUid = uid;
                        c.imapSeq = seqNum;
                        c.id = QString("imap_%1_%2_%3").arg(account.id, folderId).arg(uid);
                        c.accountId = account.id;
                        c.folderId = folderId;
                        c.folderTag = folderTagForId(folderId);
                        c.isUnread = !meta.contains("\\Seen", Qt::CaseInsensitive);
                        c.isStarred = meta.contains("\\Flagged", Qt::CaseInsensitive) || (folderId == "starred");
                        c.isImportant = meta.contains("\\Important", Qt::CaseInsensitive)
                                        || meta.contains("$Important", Qt::CaseInsensitive)
                                        || (folderId == "important");
                        c.isReplied = meta.contains("\\Answered", Qt::CaseInsensitive)
                                      || meta.contains("$Answered", Qt::CaseInsensitive);
                        c.isForwarded = meta.contains("$Forwarded", Qt::CaseInsensitive)
                                        || meta.contains("\\Forwarded", Qt::CaseInsensitive);
                        cachedMetaConvs->prepend(c);
                    } else if (!missingUids.contains(uid)) {
                        missingUids.append(uid);
                        if (minMissingSeq <= 0 || seqNum < minMissingSeq)
                            minMissingSeq = seqNum;
                        if (seqNum > maxMissingSeq)
                            maxMissingSeq = seqNum;
                    }
                }
            }

            *buffer = QByteArray();
            if (!cachedMetaConvs->isEmpty() && missingUids.isEmpty()) {
                // 모든 UID가 이미 로컬에 로드되어 있으므로 네트워크 중복 다운로드 0바이트로 즉시 완료!
                *pageEmitted = true;
                emit imapFolderPageFetched(account.id, folderId, *cachedMetaConvs, *startSeqOut, *totalExists, endSeq <= 0);
                if (endSeq <= 0)
                    emit imapFolderConversationsFetched(account.id, folderId, *cachedMetaConvs);
                *step = 5;
                socket->write("A05 LOGOUT\r\n");
                return;
            }

            // Step 1-B: 다음(imap.daum.net) 등 불연속 UID 서버에서 쉼표 나열 길이 초과(A04 BAD)가 발생하지 않도록
            //           미로드 구간(fetchStartSeq:fetchEndSeq)에 대해 단일 범위 FETCH (UID FLAGS BODY.PEEK[HEADER.FIELDS]) 요청
            const int fallbackEnd = (endSeq > 0 && endSeq <= *totalExists) ? endSeq : *totalExists;
            const int fetchStartSeq = (minMissingSeq > 0) ? minMissingSeq : *startSeqOut;
            const int fetchEndSeq = (maxMissingSeq >= fetchStartSeq) ? maxMissingSeq : qMax(fetchStartSeq, fallbackEnd);
            *step = 4;
            QString fetchCmd = QString("A04 FETCH %1:%2 (UID FLAGS BODY.PEEK[HEADER.FIELDS (FROM TO CC SUBJECT DATE MESSAGE-ID DISPOSITION-NOTIFICATION-TO RETURN-RECEIPT-TO X-CONFIRM-READING-TO X-PRIORITY IMPORTANCE X-MSMAIL-PRIORITY PRIORITY)])\r\n")
                                   .arg(fetchStartSeq)
                                   .arg(fetchEndSeq);
            socket->write(fetchCmd.toUtf8());
        } else if (*step == 4 && (respLatin1.contains("\r\nA04 OK") || respLatin1.startsWith("A04 OK")
                                  || respLatin1.contains("\r\nA04 NO") || respLatin1.startsWith("A04 NO")
                                  || respLatin1.contains("\r\nA04 BAD") || respLatin1.startsWith("A04 BAD"))) {
            QList<ConversationItem> convs = *cachedMetaConvs;
            QSet<int> seenBatchUids;
            for (const ConversationItem &mc : convs) {
                if (mc.imapUid > 0)
                    seenBatchUids.insert(mc.imapUid);
            }
            static QRegularExpression rxFetchLiteral(R"(\*\s+(\d+)\s+FETCH\s*\(([^\r\n]*)\{(\d+)\}\r\n)",
                                                     QRegularExpression::CaseInsensitiveOption);
            static QRegularExpression rxUid(R"(\bUID\s+(\d+))", QRegularExpression::CaseInsensitiveOption);
            int searchPos = 0;
            int idx = 1;
            while (searchPos < respLatin1.size()) {
                QRegularExpressionMatch m = rxFetchLiteral.match(respLatin1, searchPos);
                if (!m.hasMatch())
                    break;
                int seqNum = m.captured(1).toInt();
                QString flagsMeta = m.captured(2);
                int literalLen = m.captured(3).toInt();
                int bodyStart = m.capturedEnd();
                QByteArray rawEmail = buffer->mid(bodyStart, literalLen);
                searchPos = bodyStart + literalLen;
                if (!rawEmail.endsWith("\r\n\r\n"))
                    rawEmail.append("\r\n\r\n");

                int uid = rxUid.match(flagsMeta).captured(1).toInt();
                if (uid <= 0 && seqToUid->contains(seqNum)) {
                    uid = seqToUid->value(seqNum);
                }
                if (uid <= 0) {
                    const QString tailMeta = respLatin1.mid(bodyStart + literalLen, 64);
                    uid = rxUid.match(tailMeta).captured(1).toInt();
                }
                if (uid <= 0) {
                    uid = seqNum > 0 ? seqNum : idx++;
                }
                if (seenBatchUids.contains(uid))
                    continue;
                seenBatchUids.insert(uid);

                ConversationItem c;
                c.imapUid = uid;
                c.imapSeq = seqNum;
                c.id = QString("imap_%1_%2_%3").arg(account.id, folderId).arg(uid);

                QString htmlBody, plainBody, fromLine, subjLine, dateLine, toLine;
                parseMimeEntity(rawEmail, htmlBody, plainBody, &fromLine, &subjLine, &dateLine, &toLine,
                                nullptr, c.id, false, false);
                if (fromLine.isEmpty() && subjLine.isEmpty() && dateLine.isEmpty())
                    continue;

                c.accountId = account.id;
                c.folderId = folderId;
                c.folderTag = folderTagForId(folderId);
                c.subject = subjLine.isEmpty() ? "(제목 없음)" : subjLine;
                c.isUnread = !flagsMeta.contains("\\Seen", Qt::CaseInsensitive);
                c.isStarred = flagsMeta.contains("\\Flagged", Qt::CaseInsensitive) || (folderId == "starred");
                c.isImportant = flagsMeta.contains("\\Important", Qt::CaseInsensitive)
                                || flagsMeta.contains("$Important", Qt::CaseInsensitive)
                                || (folderId == "important")
                                || hasHighImportanceHeader(rawEmail);
                c.isReplied = flagsMeta.contains("\\Answered", Qt::CaseInsensitive)
                              || flagsMeta.contains("$Answered", Qt::CaseInsensitive);
                c.isForwarded = flagsMeta.contains("$Forwarded", Qt::CaseInsensitive)
                                || flagsMeta.contains("\\Forwarded", Qt::CaseInsensitive);
                {
                    const QByteArray rawHdrLower = rawEmail.toLower();
                    if (rawHdrLower.contains("disposition-notification-to:")
                        || rawHdrLower.contains("return-receipt-to:")
                        || rawHdrLower.contains("x-confirm-reading-to:")) {
                        c.readReceiptRequested = true;
                    }
                }

                MessageItem msg;
                msg.id = c.id + "_m1";
                QString rawSenderName = fromLine.section('<', 0, 0).remove('"').trimmed();
                if (rawSenderName.isEmpty())
                    rawSenderName = fromLine;
                msg.senderEmail = fromLine.section('<', 1, 1).section('>', 0, 0).trimmed();
                if (msg.senderEmail.isEmpty())
                    msg.senderEmail = fromLine;

                if ((folderId == "sent" || folderId == "custom_sent") && !toLine.trimmed().isEmpty()) {
                    QString toName = toLine.section('<', 0, 0).remove('"').trimmed();
                    if (toName.isEmpty())
                        toName = toLine.trimmed();
                    msg.senderName = QString("To: %1").arg(toName);
                    msg.senderInitials = toName.left(2).toUpper();
                } else {
                    msg.senderName = rawSenderName;
                    msg.senderInitials = msg.senderName.left(2).toUpper();
                }
                msg.senderAvatarBg = QColor("#DBEAFE");
                msg.senderAvatarFg = QColor("#1D4ED8");
                msg.recipientSummary = toLine.trimmed().isEmpty() ? account.email : toLine.trimmed();
                msg.shortTimeText = dateLine.left(16);
                msg.fullTimeText = dateLine;
                msg.subject = c.subject;
                msg.snippet = c.subject;
                msg.htmlBody = QStringLiteral("<!--SM_HEADER_ONLY-->");

                c.messages.append(msg);
                registerLoadedImapUid(account.id, folderId, uid);
                convs.prepend(c);
            }

            *pageEmitted = true;
            emit imapFolderPageFetched(account.id, folderId, convs, *startSeqOut, *totalExists, endSeq <= 0);
            if (endSeq <= 0)
                emit imapFolderConversationsFetched(account.id, folderId, convs);
            *step = 5;
            socket->write("A05 LOGOUT\r\n");
        }
    });

    bindSocketLifecycleCleanup(
        socket,
        [this, account, folderId, endSeq, startSeqOut, totalExists, pageEmitted, cachedMetaConvs]() {
            if (!m_isShuttingDown && !(*pageEmitted)) {
                *pageEmitted = true;
                emit imapFolderPageFetched(account.id, folderId, *cachedMetaConvs, *startSeqOut, *totalExists, endSeq <= 0);
            }
        },
        [step, buffer, resolvedMailbox, totalExists, startSeqOut, pageEmitted, seqToUid, cachedMetaConvs]() {
            delete step;
            delete buffer;
            delete resolvedMailbox;
            delete totalExists;
            delete startSeqOut;
            delete pageEmitted;
            delete seqToUid;
            delete cachedMetaConvs;
        });

    socket->connectToHostEncrypted(account.imapHost, account.imapPort);
}

void GoogleProvider::checkImapInboxQuick(const AccountItem &account, int knownExistsCount)
{
    if (account.imapHost.isEmpty())
        return;
    if (account.appPassword.isEmpty() && account.oauthAccessToken.isEmpty())
        return;
    if (account.isPop3()) {
        checkPop3InboxQuick(account, knownExistsCount);
        return;
    }

    // 30초 주기 초경량 INBOX 변경 감지(Quick Check):
    // LIST 전체 조회를 생략하고 곧바로 EXAMINE "INBOX"만 보내 EXISTS 건수를 비교하여,
    // 건수 변동이 없으면 본문 FETCH를 아예 수행하지 않고 0.1초 만에 즉시 LOGOUT(부하 0)한다.
    QSslSocket *socket = new QSslSocket(this);
    int *step = new int(0);
    QByteArray *buffer = new QByteArray();

    connect(socket, &QSslSocket::readyRead, this,
            [this, socket, account, knownExistsCount, step, buffer]() {
        if (!socket->isOpen())
            return;
        buffer->append(socket->readAll());
        if (!buffer->endsWith("\r\n"))
            return;

        QString respLatin1 = QString::fromLatin1(*buffer);
        if (*step == 0 && respLatin1.contains("* OK")) {
            *buffer = QByteArray();
            *step = 1;
            if (!account.oauthAccessToken.isEmpty() && !account.useSeparateSmtpAuth && account.imapUsername.isEmpty()) {
                QByteArray xoauth = QString("user=%1\x01" "auth=Bearer %2\x01\x01")
                                        .arg(account.email, account.oauthAccessToken)
                                        .toUtf8()
                                        .toBase64();
                socket->write("Q01 AUTHENTICATE XOAUTH2 " + xoauth + "\r\n");
            } else {
                QString cmd = QString("Q01 LOGIN \"%1\" \"%2\"\r\n")
                                  .arg(account.effectiveImapUser(), account.appPassword);
                socket->write(cmd.toUtf8());
            }
        } else if (*step == 1 && (respLatin1.startsWith("+ ") || respLatin1.contains("\r\n+ "))
                   && !respLatin1.contains("Q01 NO") && !respLatin1.contains("Q01 BAD")) {
            socket->write("\r\n");
        } else if (*step == 1 && (respLatin1.contains("Q01 OK") || respLatin1.contains("Q01 NO") || respLatin1.contains("Q01 BAD"))) {
            if (!respLatin1.contains("Q01 OK")) {
                if (!account.oauthRefreshToken.isEmpty()) {
                    refreshGoogleAccessToken(account.email, account.oauthRefreshToken);
                }
                socket->disconnectFromHost();
                return;
            }
            *buffer = QByteArray();
            *step = 2;
            socket->write("Q02 EXAMINE \"INBOX\"\r\n");
        } else if (*step == 2 && (respLatin1.contains("\r\nQ02 OK") || respLatin1.startsWith("Q02 OK")
                                  || respLatin1.contains("Q02 NO") || respLatin1.contains("Q02 BAD"))) {
            int existsCount = -1;
            static QRegularExpression rxExists(R"(\*\s+(\d+)\s+EXISTS)");
            QRegularExpressionMatch m = rxExists.match(respLatin1);
            if (m.hasMatch())
                existsCount = m.captured(1).toInt();

            *buffer = QByteArray();
            *step = 3;
            socket->write("Q03 LOGOUT\r\n");

            // 건수가 변동되었을 때만 실제 INBOX 최신 메일을 가져온다 (변동이 없으면 FETCH 0바이트!)
            if (existsCount >= 0 && existsCount != knownExistsCount) {
                int fetchCount = (knownExistsCount > 0 && existsCount > knownExistsCount)
                                     ? qBound(5, existsCount - knownExistsCount + 5, 25)
                                     : 25;
                fetchImapFolder(account, QStringLiteral("inbox"), fetchCount, 0);
            }
        }
    });

    bindSocketLifecycleCleanup(
        socket,
        nullptr,
        [step, buffer]() {
            delete step;
            delete buffer;
        });

    socket->connectToHostEncrypted(account.imapHost, account.imapPort);
}

void GoogleProvider::startImapIdleWatcher(const AccountItem &account)
{
    if (account.imapHost.isEmpty() || account.id.isEmpty())
        return;
    if (account.appPassword.isEmpty() && account.oauthAccessToken.isEmpty())
        return;
    if (account.isPop3())
        return; // POP3(RFC 1939)는 IDLE 푸시 규격이 없으므로 30초 STAT 초경량 폴링만 사용

    // 이미 해당 계정의 IDLE 소켓이 연결되어 있으면 중복 생성하지 않음
    if (QSslSocket *existing = m_idleSockets.value(account.id, nullptr)) {
        if (existing->state() != QAbstractSocket::UnconnectedState)
            return;
        m_idleSockets.remove(account.id);
        existing->deleteLater();
    }

    QSslSocket *idleSocket = new QSslSocket(this);
    m_idleSockets.insert(account.id, idleSocket);

    int *step = new int(0);
    int *lastKnownExists = new int(-1);
    QByteArray *buffer = new QByteArray();
    QTimer *keepAliveTimer = new QTimer(idleSocket);
    keepAliveTimer->setInterval(14 * 60 * 1000); // RFC 2177: 14분마다 DONE -> IDLE 재진입으로 NAT/세션 유지

    connect(keepAliveTimer, &QTimer::timeout, this, [idleSocket, step]() {
        if (idleSocket->isOpen() && *step == 3) {
            *step = 4;
            idleSocket->write("DONE\r\n");
        }
    });

    connect(idleSocket, &QSslSocket::readyRead, this,
            [this, idleSocket, account, step, lastKnownExists, buffer, keepAliveTimer]() {
        if (!idleSocket->isOpen())
            return;
        buffer->append(idleSocket->readAll());
        if (!buffer->endsWith("\r\n"))
            return;

        QString respLatin1 = QString::fromLatin1(*buffer);
        if (*step == 0 && respLatin1.contains("* OK")) {
            *buffer = QByteArray();
            *step = 1;
            if (!account.oauthAccessToken.isEmpty() && !account.useSeparateSmtpAuth && account.imapUsername.isEmpty()) {
                QByteArray xoauth = QString("user=%1\x01" "auth=Bearer %2\x01\x01")
                                        .arg(account.email, account.oauthAccessToken)
                                        .toUtf8()
                                        .toBase64();
                idleSocket->write("ID01 AUTHENTICATE XOAUTH2 " + xoauth + "\r\n");
            } else {
                QString cmd = QString("ID01 LOGIN \"%1\" \"%2\"\r\n")
                                  .arg(account.effectiveImapUser(), account.appPassword);
                idleSocket->write(cmd.toUtf8());
            }
        } else if (*step == 1 && (respLatin1.startsWith("+ ") || respLatin1.contains("\r\n+ "))
                   && !respLatin1.contains("ID01 NO") && !respLatin1.contains("ID01 BAD")) {
            idleSocket->write("\r\n");
        } else if (*step == 1 && (respLatin1.contains("ID01 OK") || respLatin1.contains("ID01 NO") || respLatin1.contains("ID01 BAD"))) {
            if (!respLatin1.contains("ID01 OK")) {
                idleSocket->disconnectFromHost();
                return;
            }
            *buffer = QByteArray();
            *step = 2;
            idleSocket->write("ID02 SELECT \"INBOX\"\r\n");
        } else if (*step == 2 && (respLatin1.contains("\r\nID02 OK") || respLatin1.startsWith("ID02 OK")
                                  || respLatin1.contains("ID02 NO") || respLatin1.contains("ID02 BAD"))) {
            if (!respLatin1.contains("ID02 OK")) {
                idleSocket->disconnectFromHost();
                return;
            }
            static QRegularExpression rxExists(R"(\*\s+(\d+)\s+EXISTS)");
            QRegularExpressionMatch m = rxExists.match(respLatin1);
            if (m.hasMatch())
                *lastKnownExists = m.captured(1).toInt();

            *buffer = QByteArray();
            *step = 3;
            idleSocket->write("ID03 IDLE\r\n");
            keepAliveTimer->start();
        } else if (*step == 3) {
            // IDLE 대기 중 서버로부터 실시간 푸시(* <N> EXISTS 또는 * <N> EXPUNGE) 수신!
            static QRegularExpression rxExists(R"(\*\s+(\d+)\s+EXISTS)");
            static QRegularExpression rxExpunge(R"(\*\s+(\d+)\s+EXPUNGE)");
            QRegularExpressionMatch mEx = rxExists.match(respLatin1);
            QRegularExpressionMatch mExp = rxExpunge.match(respLatin1);

            if (mEx.hasMatch() || mExp.hasMatch()) {
                int newExists = mEx.hasMatch() ? mEx.captured(1).toInt() : -1;
                *buffer = QByteArray();
                if (newExists != *lastKnownExists || mExp.hasMatch()) {
                    if (newExists >= 0)
                        *lastKnownExists = newExists;
                    // 새 메일 도착 즉시(0.5초 내) INBOX 동기화 수행
                    fetchImapFolder(account, QStringLiteral("inbox"), 15, 0);
                }
                *step = 4;
                idleSocket->write("DONE\r\n");
            } else if (respLatin1.contains("ID03 NO") || respLatin1.contains("ID03 BAD")) {
                // IDLE 미지원 서버인 경우 소켓 종료 (30초 초경량 폴링이 담당)
                idleSocket->disconnectFromHost();
            } else {
                *buffer = QByteArray();
            }
        } else if (*step == 4 && (respLatin1.contains("ID03 OK") || respLatin1.contains("ID03 NO") || respLatin1.contains("ID03 BAD"))) {
            *buffer = QByteArray();
            *step = 3;
            idleSocket->write("ID03 IDLE\r\n");
        }
    });

    bindSocketLifecycleCleanup(
        idleSocket,
        [this, account, idleSocket]() {
            if (!m_isShuttingDown && m_idleSockets.value(account.id) == idleSocket) {
                m_idleSockets.remove(account.id);
            }
        },
        [step, lastKnownExists, buffer]() {
            delete step;
            delete lastKnownExists;
            delete buffer;
        },
        0);

    idleSocket->connectToHostEncrypted(account.imapHost, account.imapPort);
}

void GoogleProvider::stopAllImapIdleWatchers()
{
    const QList<QSslSocket *> idleList = m_idleSockets.values();
    m_idleSockets.clear();
    for (QSslSocket *sock : idleList) {
        if (!sock)
            continue;
        if (sock->state() != QAbstractSocket::UnconnectedState) {
            sock->abort();
        } else {
            sock->deleteLater();
        }
    }
}

void GoogleProvider::registerLoadedImapUid(const QString &accountId, const QString &folderId, int uid)
{
    if (accountId.isEmpty() || folderId.isEmpty() || uid <= 0)
        return;
    m_loadedFolderUids[accountId + QStringLiteral("|") + folderId].insert(uid);
}

void GoogleProvider::unregisterLoadedImapUid(const QString &accountId, const QString &folderId, int uid)
{
    if (accountId.isEmpty() || folderId.isEmpty() || uid <= 0)
        return;
    m_loadedFolderUids[accountId + QStringLiteral("|") + folderId].remove(uid);
}

void GoogleProvider::clearLoadedImapUids(const QString &accountId, const QString &folderId)
{
    if (accountId.isEmpty()) {
        m_loadedFolderUids.clear();
        return;
    }
    if (folderId.isEmpty()) {
        const QString prefix = accountId + QStringLiteral("|");
        for (auto it = m_loadedFolderUids.begin(); it != m_loadedFolderUids.end(); ) {
            if (it.key().startsWith(prefix))
                it = m_loadedFolderUids.erase(it);
            else
                ++it;
        }
        return;
    }
    m_loadedFolderUids.remove(accountId + QStringLiteral("|") + folderId);
}

void GoogleProvider::fetchAllStandardImapFolders(const AccountItem &account, int maxMessagesPerFolder)
{
    if (account.imapHost.isEmpty()) {
        emit statusMessage(MailI18n::t("IMAP/POP3 호스트가 설정되지 않았습니다.", "Mail host is not configured.", "メールホストが設定されていません。"));
        return;
    }
    if (account.appPassword.isEmpty() && account.oauthAccessToken.isEmpty()) {
        emit statusMessage(
            MailI18n::t(QString("[%1] 실시간 서버 동기화를 위해 Settings > Accounts에서 비밀번호 또는 Google OAuth 토큰을 설정할 수 있습니다.").arg(account.email),
                        QString("[%1] Configure a Password or Google OAuth in Settings > Accounts to enable live sync.").arg(account.email),
                        QString("[%1] リアルタイム同期のために設定 > アカウントでパスワードまたはGoogle連携を設定してください。").arg(account.email)));
        return;
    }
    if (account.isPop3()) {
        fetchPop3Inbox(account, maxMessagesPerFolder, 0);
        return;
    }

    QSslSocket *socket = new QSslSocket(this);
    emit statusMessage(
        MailI18n::t(QString("[%1] %2:%3 전체 편지함(IMAP) 연결 중..."),
                    QString("[%1] Connecting to %2:%3 (IMAP)..."),
                    QString("[%1] %2:%3 (IMAP) に接続中..."))
            .arg(account.email, account.imapHost).arg(account.imapPort));

    int *step = new int(0);
    QByteArray *buffer = new QByteArray();
    QList<QPair<QString, QString>> *folderQueue = new QList<QPair<QString, QString>>();
    int *currentQueueIdx = new int(0);
    int *cmdSeq = new int(10);
    int *folderExistsCount = new int(0);
    int *folderStartSeq = new int(1);

    connect(socket, &QSslSocket::readyRead, this,
            [this, socket, account, maxMessagesPerFolder, step, buffer, folderQueue, currentQueueIdx, cmdSeq, folderExistsCount, folderStartSeq]() {
        if (!socket->isOpen())
            return;
        buffer->append(socket->readAll());
        if (!buffer->endsWith("\r\n"))
            return;

        QString respLatin1 = QString::fromLatin1(*buffer);

        // Lambda to advance to the next folder in folderQueue
        auto startNextFolder = [socket, buffer, step, folderQueue, currentQueueIdx, cmdSeq]() {
            if (*currentQueueIdx >= folderQueue->size()) {
                *step = 99;
                socket->write("AZ99 LOGOUT\r\n");
                return;
            }
            const QString rawMbox = (*folderQueue)[*currentQueueIdx].second;
            *buffer = QByteArray();
            *step = 3;
            ++(*cmdSeq);
            QString cmd = QString("A%1 EXAMINE \"%2\"\r\n").arg(*cmdSeq).arg(rawMbox);
            socket->write(cmd.toLatin1());
        };

        if (*step == 0 && respLatin1.contains("* OK")) {
            *buffer = QByteArray();
            *step = 1;
            if (!account.oauthAccessToken.isEmpty() && !account.useSeparateSmtpAuth && account.imapUsername.isEmpty()) {
                QByteArray xoauth = QString("user=%1\x01" "auth=Bearer %2\x01\x01")
                                        .arg(account.email, account.oauthAccessToken)
                                        .toUtf8()
                                        .toBase64();
                socket->write("A01 AUTHENTICATE XOAUTH2 " + xoauth + "\r\n");
            } else {
                QString cmd = QString("A01 LOGIN \"%1\" \"%2\"\r\n")
                                  .arg(account.effectiveImapUser(), account.appPassword);
                socket->write(cmd.toUtf8());
            }
        } else if (*step == 1 && (respLatin1.startsWith("+ ") || respLatin1.contains("\r\n+ "))
                   && !respLatin1.contains("A01 NO") && !respLatin1.contains("A01 BAD")) {
            socket->write("\r\n");
        } else if (*step == 1 && (respLatin1.contains("A01 OK") || respLatin1.contains("A01 NO") || respLatin1.contains("A01 BAD"))) {
            if (!respLatin1.contains("A01 OK")) {
                if (!account.oauthRefreshToken.isEmpty()) {
                    refreshGoogleAccessToken(account.email, account.oauthRefreshToken);
                } else {
                    emit statusMessage(MailI18n::t(QString("[%1] IMAP 인증 실패: 앱 비밀번호 또는 OAuth 토큰을 확인하세요."),
                                                   QString("[%1] IMAP auth failed: Please check your app password or OAuth token."),
                                                   QString("[%1] IMAP 認証失敗: アプリパスワードまたは OAuth トークンを確認してください。"))
                                           .arg(account.email));
                }
                socket->disconnectFromHost();
                return;
            }
            *buffer = QByteArray();
            *step = 2;
            socket->write("A02 LIST \"\" \"*\"\r\n");
        } else if (*step == 2 && (respLatin1.contains("\r\nA02 OK") || respLatin1.startsWith("A02 OK")
                                  || respLatin1.contains("A02 NO") || respLatin1.contains("A02 BAD"))) {
            QMap<QString, QString> mappedFolders;
            QList<FolderItem> discoveredCustom;
            parseImapListResponse(respLatin1, account, mappedFolders, discoveredCustom);
            emit imapCustomFoldersDiscovered(account.id, discoveredCustom);

            // 우선순위 순서대로 동기화 큐에 추가 (시스템 표준 폴더 + 해당 계정의 서버에서 실제로 발견된 내 폴더)
            const QStringList priorityOrder = {
                "inbox", "sent", "drafts", "starred", "important", "archive", "spam", "trash"
            };
            folderQueue->clear();
            for (const QString &fid : priorityOrder) {
                if (mappedFolders.contains(fid)) {
                    folderQueue->append(qMakePair(fid, mappedFolders.value(fid)));
                }
            }
            for (const FolderItem &cf : discoveredCustom) {
                if (!cf.rawImapMailbox.isEmpty()) {
                    folderQueue->append(qMakePair(cf.id, cf.rawImapMailbox));
                }
            }
            *currentQueueIdx = 0;
            startNextFolder();
        } else if (*step == 3) {
            QString tagOk = QString("A%1 OK").arg(*cmdSeq);
            QString tagNo = QString("A%1 NO").arg(*cmdSeq);
            QString tagBad = QString("A%1 BAD").arg(*cmdSeq);
            if (!respLatin1.contains(tagOk) && !respLatin1.contains(tagNo) && !respLatin1.contains(tagBad))
                return;

            if (!respLatin1.contains(tagOk)) {
                ++(*currentQueueIdx);
                startNextFolder();
                return;
            }

            int existsCount = 0;
            static QRegularExpression rxExists(R"(\*\s+(\d+)\s+EXISTS)");
            QRegularExpressionMatch m = rxExists.match(respLatin1);
            if (m.hasMatch())
                existsCount = m.captured(1).toInt();

            *folderExistsCount = existsCount;
            if (existsCount <= 0) {
                ++(*currentQueueIdx);
                startNextFolder();
                return;
            }

            *buffer = QByteArray();
            *step = 4;
            ++(*cmdSeq);
            // 1단계 표준 IMAP 조회: 초기 200건 목록은 제목·발신자·날짜·플래그(HEADER.FIELDS)만 초고속으로 가져오고,
            // 본문 전체 내용(BODY.PEEK[])은 사용자가 메일을 클릭할 때 fetchImapSingleMessageBody()로 온전히 다운로드한다.
            int limit = maxMessagesPerFolder > 0 ? maxMessagesPerFolder : 200;
            int startSeq = qMax(1, existsCount - limit + 1);
            *folderStartSeq = startSeq;
            QString fetchCmd = QString("A%1 FETCH %2:%3 (UID FLAGS BODY.PEEK[HEADER.FIELDS (FROM TO CC SUBJECT DATE MESSAGE-ID DISPOSITION-NOTIFICATION-TO RETURN-RECEIPT-TO X-CONFIRM-READING-TO X-PRIORITY IMPORTANCE X-MSMAIL-PRIORITY PRIORITY)])\r\n")
                                   .arg(*cmdSeq)
                                   .arg(startSeq)
                                   .arg(existsCount);
            socket->write(fetchCmd.toUtf8());
        } else if (*step == 4) {
            QString tagOk = QString("\r\nA%1 OK").arg(*cmdSeq);
            QString tagOkStart = QString("A%1 OK").arg(*cmdSeq);
            QString tagNo = QString("\r\nA%1 NO").arg(*cmdSeq);
            QString tagBad = QString("\r\nA%1 BAD").arg(*cmdSeq);
            if (!respLatin1.contains(tagOk) && !respLatin1.startsWith(tagOkStart)
                && !respLatin1.contains(tagNo) && !respLatin1.contains(tagBad))
                return;

            const QString folderId = (*folderQueue)[*currentQueueIdx].first;
            QList<ConversationItem> convs;
            static QRegularExpression rxFetchLiteral(R"(\*\s+(\d+)\s+FETCH\s*\(([^\r\n]*)\{(\d+)\}\r\n)",
                                                     QRegularExpression::CaseInsensitiveOption);
            static const QRegularExpression rxUid(R"(\bUID\s+(\d+))", QRegularExpression::CaseInsensitiveOption);

            int searchPos = 0;
            int idx = 1;
            while (searchPos < respLatin1.size()) {
                QRegularExpressionMatch m = rxFetchLiteral.match(respLatin1, searchPos);
                if (!m.hasMatch())
                    break;
                int seqNum = m.captured(1).toInt();
                QString flagsMeta = m.captured(2);
                int literalLen = m.captured(3).toInt();
                int bodyStart = m.capturedEnd();
                QByteArray rawEmail = buffer->mid(bodyStart, literalLen);
                searchPos = bodyStart + literalLen;

                QRegularExpressionMatch um = rxUid.match(flagsMeta);
                int uid = um.hasMatch() ? um.captured(1).toInt() : 0;
                if (uid <= 0) {
                    const int nextStar = respLatin1.indexOf(QLatin1String("\r\n* "), searchPos);
                    const int tailLen = (nextStar > searchPos) ? (nextStar - searchPos) : 64;
                    QRegularExpressionMatch umTail = rxUid.match(respLatin1.mid(searchPos, qMin(128, tailLen)));
                    if (umTail.hasMatch())
                        uid = umTail.captured(1).toInt();
                }
                if (uid <= 0)
                    uid = (seqNum > 0 ? seqNum : idx++);

                ConversationItem c;
                c.imapUid = uid;
                c.imapSeq = seqNum;
                c.id = QString("imap_%1_%2_%3").arg(account.id, folderId).arg(uid);
                registerLoadedImapUid(account.id, folderId, uid);

                if (!rawEmail.endsWith("\r\n\r\n"))
                    rawEmail.append("\r\n\r\n");

                QString htmlBody, plainBody, fromLine, subjLine, dateLine, toLine;
                parseMimeEntity(rawEmail, htmlBody, plainBody, &fromLine, &subjLine, &dateLine, &toLine,
                                nullptr, c.id, false, false);

                if (fromLine.isEmpty() && subjLine.isEmpty() && dateLine.isEmpty())
                    continue;

                c.accountId = account.id;
                c.folderId = folderId;
                c.folderTag = folderTagForId(folderId);
                c.subject = subjLine.isEmpty() ? "(제목 없음)" : subjLine;
                c.isUnread = !flagsMeta.contains("\\Seen", Qt::CaseInsensitive);
                c.isStarred = flagsMeta.contains("\\Flagged", Qt::CaseInsensitive) || (folderId == "starred");
                c.isImportant = flagsMeta.contains("\\Important", Qt::CaseInsensitive)
                                || flagsMeta.contains("$Important", Qt::CaseInsensitive)
                                || (folderId == "important")
                                || hasHighImportanceHeader(rawEmail);
                c.isReplied = flagsMeta.contains("\\Answered", Qt::CaseInsensitive)
                              || flagsMeta.contains("$Answered", Qt::CaseInsensitive);
                c.isForwarded = flagsMeta.contains("$Forwarded", Qt::CaseInsensitive)
                                || flagsMeta.contains("\\Forwarded", Qt::CaseInsensitive);
                {
                    const QByteArray rawHdrLower = rawEmail.toLower();
                    if (rawHdrLower.contains("disposition-notification-to:")
                        || rawHdrLower.contains("return-receipt-to:")
                        || rawHdrLower.contains("x-confirm-reading-to:")) {
                        c.readReceiptRequested = true;
                    }
                }

                MessageItem msg;
                msg.id = c.id + "_m1";
                QString rawSenderName = fromLine.section('<', 0, 0).remove('"').trimmed();
                if (rawSenderName.isEmpty())
                    rawSenderName = fromLine;
                msg.senderEmail = fromLine.section('<', 1, 1).section('>', 0, 0).trimmed();
                if (msg.senderEmail.isEmpty())
                    msg.senderEmail = fromLine;

                if ((folderId == "sent" || folderId == "custom_sent") && !toLine.trimmed().isEmpty()) {
                    QString toName = toLine.section('<', 0, 0).remove('"').trimmed();
                    if (toName.isEmpty())
                        toName = toLine.trimmed();
                    msg.senderName = QString("To: %1").arg(toName);
                    msg.senderInitials = toName.left(2).toUpper();
                } else {
                    msg.senderName = rawSenderName;
                    msg.senderInitials = msg.senderName.left(2).toUpper();
                }
                msg.senderAvatarBg = QColor("#DBEAFE");
                msg.senderAvatarFg = QColor("#1D4ED8");
                msg.recipientSummary = toLine.trimmed().isEmpty() ? account.email : toLine.trimmed();
                msg.shortTimeText = dateLine.left(16);
                msg.fullTimeText = dateLine;
                msg.subject = c.subject;
                msg.snippet = c.subject;
                msg.htmlBody = QStringLiteral("<!--SM_HEADER_ONLY-->");

                c.messages.append(msg);
                convs.prepend(c);
            }

            emit imapFolderPageFetched(account.id, folderId, convs, *folderStartSeq, *folderExistsCount, true);
            emit imapFolderConversationsFetched(account.id, folderId, convs);
            emit statusMessage(
                MailI18n::t(
                    QString("[%1] '%2' 편지함 동기화 완료 (%3 / 전체 %4건)"),
                    QString("[%1] '%2' synced (%3 / %4 total)"),
                    QString("[%1] '%2' 同期完了 (%3 / 全%4件)"))
                    .arg(account.email, MailI18n::folderTitle(folderId, folderTagForId(folderId)))
                    .arg(convs.size())
                    .arg(*folderExistsCount));

            ++(*currentQueueIdx);
            startNextFolder();
        }
    });

    bindSocketLifecycleCleanup(
        socket,
        nullptr,
        [step, buffer, folderQueue, currentQueueIdx, cmdSeq, folderExistsCount, folderStartSeq]() {
            delete step;
            delete buffer;
            delete folderQueue;
            delete currentQueueIdx;
            delete cmdSeq;
            delete folderExistsCount;
            delete folderStartSeq;
        });

    socket->connectToHostEncrypted(account.imapHost, account.imapPort);
}

void GoogleProvider::fetchImapSingleMessageBody(const AccountItem &account,
                                                const QString &folderId,
                                                int seqNum,
                                                const QString &conversationId,
                                                const QString &fallbackSubject,
                                                const QString &fallbackSourceFolderId)
{
    if (account.imapHost.isEmpty() || seqNum <= 0 || account.isPop3())
        return;
    if (account.appPassword.isEmpty() && account.oauthAccessToken.isEmpty())
        return;

    QSslSocket *socket = new QSslSocket(this);
    int *step = new int(0);
    int *existsInFolder = new int(0);
    QByteArray *buffer = new QByteArray();

    connect(socket, &QSslSocket::readyRead, this,
            [this, socket, account, folderId, seqNum, conversationId, fallbackSubject, fallbackSourceFolderId, step, existsInFolder, buffer]() {
        if (!socket->isOpen())
            return;
        buffer->append(socket->readAll());
        if (!buffer->endsWith("\r\n"))
            return;

        QString respLatin1 = QString::fromLatin1(*buffer);
        if (*step == 0 && respLatin1.contains("* OK")) {
            *buffer = QByteArray();
            *step = 1;
            if (!account.oauthAccessToken.isEmpty() && !account.useSeparateSmtpAuth && account.imapUsername.isEmpty()) {
                QByteArray xoauth = QString("user=%1\x01" "auth=Bearer %2\x01\x01")
                                        .arg(account.email, account.oauthAccessToken)
                                        .toUtf8()
                                        .toBase64();
                socket->write("A01 AUTHENTICATE XOAUTH2 " + xoauth + "\r\n");
            } else {
                QString cmd = QString("A01 LOGIN \"%1\" \"%2\"\r\n")
                                  .arg(account.effectiveImapUser(), account.appPassword);
                socket->write(cmd.toUtf8());
            }
        } else if (*step == 1 && (respLatin1.startsWith("+ ") || respLatin1.contains("\r\n+ "))
                   && !respLatin1.contains("A01 NO") && !respLatin1.contains("A01 BAD")) {
            socket->write("\r\n");
        } else if (*step == 1 && (respLatin1.contains("A01 OK") || respLatin1.contains("A01 NO") || respLatin1.contains("A01 BAD"))) {
            if (!respLatin1.contains("A01 OK")) {
                if (!account.oauthRefreshToken.isEmpty()) {
                    refreshGoogleAccessToken(account.email, account.oauthRefreshToken);
                }
                socket->disconnectFromHost();
                return;
            }
            *buffer = QByteArray();
            *step = 2;
            socket->write("A02 LIST \"\" \"*\"\r\n");
        } else if (*step == 2 && (respLatin1.contains("\r\nA02 OK") || respLatin1.startsWith("A02 OK")
                                  || respLatin1.contains("A02 NO") || respLatin1.contains("A02 BAD"))) {
            QString targetMbox = resolveImapMailboxFromList(respLatin1, account, folderId);
            *buffer = QByteArray();
            if (targetMbox.isEmpty()) {
                *step = 5;
                socket->write("A05 LOGOUT\r\n");
                return;
            }
            *step = 3;
            socket->write(QString("A03 EXAMINE \"%1\"\r\n").arg(targetMbox).toLatin1());
        } else if (*step == 3 && (respLatin1.contains("\r\nA03 OK") || respLatin1.startsWith("A03 OK")
                                  || respLatin1.contains("A03 NO") || respLatin1.contains("A03 BAD"))) {
            static const QRegularExpression rxExists(R"(\*\s+(\d+)\s+EXISTS)", QRegularExpression::CaseInsensitiveOption);
            const QRegularExpressionMatch exM = rxExists.match(respLatin1);
            if (exM.hasMatch())
                *existsInFolder = exM.captured(1).toInt();
            *buffer = QByteArray();
            if (!respLatin1.contains("A03 OK")) {
                *step = 5;
                socket->write("A05 LOGOUT\r\n");
                return;
            }
            *step = 4;
            socket->write(QString("A04 UID FETCH %1 (UID BODY.PEEK[])\r\n").arg(seqNum).toUtf8());
        } else if (*step == 4 && (respLatin1.contains("\r\nA04 OK") || respLatin1.startsWith("A04 OK")
                                  || respLatin1.contains("\r\nA04 NO") || respLatin1.contains("\r\nA04 BAD"))) {
            static QRegularExpression rxFetchLiteral(R"(\*\s+\d+\s+FETCH\s*\([^\r\n]*\{(\d+)\}\r\n)",
                                                     QRegularExpression::CaseInsensitiveOption);
            QRegularExpressionMatch m = rxFetchLiteral.match(respLatin1);
            if (m.hasMatch()) {
                int literalLen = m.captured(1).toInt();
                if (buffer->size() < m.capturedEnd() + literalLen)
                    return;
                QByteArray rawEmail = buffer->mid(m.capturedEnd(), literalLen);
                QString htmlBody, plainBody;
                QList<AttachmentItem> parsedAtts;
                parseMimeEntity(rawEmail, htmlBody, plainBody, nullptr, nullptr, nullptr, nullptr,
                                &parsedAtts, conversationId, true, false);
                QString fullViewerHtml = sanitizeEmailHtmlForViewer(htmlBody, plainBody);
                emit imapSingleMessageBodyFetched(conversationId, fullViewerHtml, parsedAtts);
                *step = 5;
                socket->write("A05 LOGOUT\r\n");
                return;
            }
            // 필터 규칙으로 방금 이동된 메일이어서 대상 폴더(targetMbox) 내에서 새 UID가 부여된 경우:
            // 대상 폴더의 최근 메일 헤더(UID + SUBJECT)를 조회하여 제목 일치 메일의 실제 UID로 본문을 가져온다.
            if (*existsInFolder > 0) {
                const int startS = qMax(1, *existsInFolder - 49);
                *buffer = QByteArray();
                *step = 41;
                socket->write(QString("A04B FETCH %1:%2 (UID BODY.PEEK[HEADER.FIELDS (SUBJECT)])\r\n")
                                  .arg(startS)
                                  .arg(*existsInFolder)
                                  .toUtf8());
                return;
            }
            if (folderId != QStringLiteral("inbox")
                && (fallbackSourceFolderId == QStringLiteral("inbox") || folderId.startsWith(QStringLiteral("custom_")))) {
                *buffer = QByteArray();
                *step = 43;
                socket->write("A04D EXAMINE \"INBOX\"\r\n");
                return;
            }
            *step = 5;
            socket->write("A05 LOGOUT\r\n");
        } else if (*step == 41 && (respLatin1.contains("\r\nA04B OK") || respLatin1.startsWith("A04B OK")
                                   || respLatin1.contains("\r\nA04B NO") || respLatin1.contains("\r\nA04B BAD"))) {
            static const QRegularExpression rxHdrLit(R"(\*\s+(\d+)\s+FETCH\s*\(([^\r\n]*)\{(\d+)\}\r\n)",
                                                     QRegularExpression::CaseInsensitiveOption);
            static const QRegularExpression rxUid(R"(\bUID\s+(\d+))", QRegularExpression::CaseInsensitiveOption);
            int matchedUid = 0;
            int searchPos = 0;
            const QString wantSubj = fallbackSubject.trimmed();
            while (searchPos < respLatin1.size()) {
                const QRegularExpressionMatch hm = rxHdrLit.match(respLatin1, searchPos);
                if (!hm.hasMatch())
                    break;
                const QString meta = hm.captured(2);
                const int litLen = hm.captured(3).toInt();
                const int bStart = hm.capturedEnd();
                if (bStart + litLen > buffer->size())
                    return;
                QByteArray rawHdr = buffer->mid(bStart, litLen);
                if (!rawHdr.endsWith("\r\n\r\n"))
                    rawHdr.append("\r\n\r\n");
                searchPos = bStart + litLen;
                int uidVal = rxUid.match(meta).captured(1).toInt();
                if (uidVal <= 0) {
                    const QString tailMeta = respLatin1.mid(bStart + litLen, 64);
                    uidVal = rxUid.match(tailMeta).captured(1).toInt();
                }
                if (uidVal <= 0)
                    continue;
                QString dummyHtml, dummyPlain, parsedSubj;
                parseMimeEntity(rawHdr, dummyHtml, dummyPlain, nullptr, &parsedSubj, nullptr, nullptr,
                                nullptr, QString(), false, true);
                if (!wantSubj.isEmpty() && parsedSubj.trimmed().compare(wantSubj, Qt::CaseInsensitive) == 0) {
                    matchedUid = uidVal;
                } else if (wantSubj.isEmpty() && *existsInFolder == 1) {
                    matchedUid = uidVal;
                }
            }
            *buffer = QByteArray();
            if (matchedUid > 0) {
                *step = 42;
                socket->write(QString("A04C UID FETCH %1 (UID BODY.PEEK[])\r\n").arg(matchedUid).toUtf8());
                return;
            }
            if (folderId != QStringLiteral("inbox")) {
                *step = 43;
                socket->write("A04D EXAMINE \"INBOX\"\r\n");
                return;
            }
            *step = 5;
            socket->write("A05 LOGOUT\r\n");
        } else if (*step == 43 && (respLatin1.contains("\r\nA04D OK") || respLatin1.startsWith("A04D OK")
                                   || respLatin1.contains("A04D NO") || respLatin1.contains("A04D BAD"))) {
            *buffer = QByteArray();
            if (!respLatin1.contains("A04D OK")) {
                *step = 5;
                socket->write("A05 LOGOUT\r\n");
                return;
            }
            *step = 42;
            socket->write(QString("A04C UID FETCH %1 (UID BODY.PEEK[])\r\n").arg(seqNum).toUtf8());
        } else if (*step == 42 && (respLatin1.contains("\r\nA04C OK") || respLatin1.startsWith("A04C OK")
                                   || respLatin1.contains("\r\nA04C NO") || respLatin1.contains("\r\nA04C BAD"))) {
            static QRegularExpression rxFetchLiteral2(R"(\*\s+\d+\s+FETCH\s*\([^\r\n]*\{(\d+)\}\r\n)",
                                                      QRegularExpression::CaseInsensitiveOption);
            QRegularExpressionMatch m2 = rxFetchLiteral2.match(respLatin1);
            if (m2.hasMatch()) {
                int literalLen = m2.captured(1).toInt();
                if (buffer->size() < m2.capturedEnd() + literalLen)
                    return;
                QByteArray rawEmail = buffer->mid(m2.capturedEnd(), literalLen);
                QString htmlBody, plainBody;
                QList<AttachmentItem> parsedAtts;
                parseMimeEntity(rawEmail, htmlBody, plainBody, nullptr, nullptr, nullptr, nullptr,
                                &parsedAtts, conversationId, true, false);
                QString fullViewerHtml = sanitizeEmailHtmlForViewer(htmlBody, plainBody);
                emit imapSingleMessageBodyFetched(conversationId, fullViewerHtml, parsedAtts);
            }
            *step = 5;
            socket->write("A05 LOGOUT\r\n");
        }
    });

    bindSocketLifecycleCleanup(
        socket,
        nullptr,
        [step, existsInFolder, buffer]() {
            delete step;
            delete existsInFolder;
            delete buffer;
        });

    socket->connectToHostEncrypted(account.imapHost, account.imapPort);
}

void GoogleProvider::emptyImapFolder(const AccountItem &account, const QString &folderId)
{
    if (account.isPop3()) {
        account.ensurePop3MailboxDirs();
        const QString targetDir = account.pop3MailboxDir(folderId);
        QDir dir(targetDir);
        if (dir.exists()) {
            const QFileInfoList emlFiles = dir.entryInfoList(QStringList() << QStringLiteral("*.eml"), QDir::Files);
            for (const QFileInfo &fi : emlFiles) {
                QFile::remove(fi.absoluteFilePath());
            }
        }
        emit statusMessage(MailI18n::t(QString("[%1] POP3 로컬 '%2' 폴더 비우기 완료"),
                                       QString("[%1] Emptied local POP3 folder '%2'"),
                                       QString("[%1] POP3 ローカルフォルダ '%2' を空にしました"))
                               .arg(account.email, folderTagForId(folderId)));
        return;
    }
    if (account.imapHost.isEmpty() || (account.appPassword.isEmpty() && account.oauthAccessToken.isEmpty()))
        return;

    QSslSocket *socket = new QSslSocket(this);
    int *step = new int(0);
    QByteArray *buffer = new QByteArray();

    connect(socket, &QSslSocket::readyRead, this, [this, socket, account, folderId, step, buffer]() {
        if (!socket->isOpen())
            return;
        buffer->append(socket->readAll());
        if (!buffer->endsWith("\r\n"))
            return;

        QString respLatin1 = QString::fromLatin1(*buffer);

        if (*step == 0 && respLatin1.contains("* OK")) {
            *buffer = QByteArray();
            *step = 1;
            if (!account.oauthAccessToken.isEmpty() && !account.useSeparateSmtpAuth && account.imapUsername.isEmpty()) {
                QByteArray xoauth = QString("user=%1\x01" "auth=Bearer %2\x01\x01")
                                        .arg(account.email, account.oauthAccessToken)
                                        .toUtf8()
                                        .toBase64();
                socket->write("A01 AUTHENTICATE XOAUTH2 " + xoauth + "\r\n");
            } else {
                QString cmd = QString("A01 LOGIN \"%1\" \"%2\"\r\n")
                                  .arg(account.effectiveImapUser(), account.appPassword);
                socket->write(cmd.toUtf8());
            }
        } else if (*step == 1 && (respLatin1.startsWith("+ ") || respLatin1.contains("\r\n+ "))
                   && !respLatin1.contains("A01 NO") && !respLatin1.contains("A01 BAD")) {
            socket->write("\r\n");
        } else if (*step == 1 && (respLatin1.contains("A01 OK") || respLatin1.contains("A01 NO") || respLatin1.contains("A01 BAD"))) {
            if (!respLatin1.contains("A01 OK")) {
                if (!account.oauthRefreshToken.isEmpty()) {
                    refreshGoogleAccessToken(account.email, account.oauthRefreshToken);
                }
                socket->disconnectFromHost();
                return;
            }
            *buffer = QByteArray();
            *step = 2;
            socket->write("A02 LIST \"\" \"*\"\r\n");
        } else if (*step == 2 && (respLatin1.contains("\r\nA02 OK") || respLatin1.startsWith("A02 OK")
                                  || respLatin1.contains("A02 NO") || respLatin1.contains("A02 BAD"))) {
            QString targetMbox = resolveImapMailboxFromList(respLatin1, account, folderId);
            if (targetMbox.isEmpty()) {
                if (folderId == "inbox")
                    targetMbox = "INBOX";
                else if (folderId == "trash")
                    targetMbox = "Trash";
                else if (folderId == "spam")
                    targetMbox = "Spam";
                else {
                    socket->write("A06 LOGOUT\r\n");
                    return;
                }
            }
            *buffer = QByteArray();
            *step = 3;
            socket->write(QString("A03 SELECT \"%1\"\r\n").arg(targetMbox).toLatin1());
        } else if (*step == 3 && (respLatin1.contains("A03 OK") || respLatin1.contains("A03 NO") || respLatin1.contains("A03 BAD"))) {
            if (!respLatin1.contains("A03 OK")) {
                socket->write("A06 LOGOUT\r\n");
                return;
            }
            *buffer = QByteArray();
            *step = 4;
            socket->write("A04 STORE 1:* +FLAGS (\\Deleted)\r\n");
        } else if (*step == 4 && (respLatin1.contains("A04 OK") || respLatin1.contains("A04 NO") || respLatin1.contains("A04 BAD"))) {
            *buffer = QByteArray();
            *step = 5;
            socket->write("A05 EXPUNGE\r\n");
        } else if (*step == 5 && (respLatin1.contains("A05 OK") || respLatin1.contains("A05 NO") || respLatin1.contains("A05 BAD"))) {
            clearLoadedImapUids(account.id, folderId);
            emit statusMessage(MailI18n::t(QString("[%1] '%2' 폴더 비우기(EXPUNGE) 완료"),
                                           QString("[%1] Emptied folder '%2' (EXPUNGE)"),
                                           QString("[%1] フォルダ '%2' を空にしました (EXPUNGE)"))
                                   .arg(account.email, folderTagForId(folderId)));
            *step = 6;
            socket->write("A06 LOGOUT\r\n");
        }
    });

    bindSocketLifecycleCleanup(
        socket,
        nullptr,
        [step, buffer]() {
            delete step;
            delete buffer;
        });

    socket->connectToHostEncrypted(account.imapHost, account.imapPort);
}

void GoogleProvider::markImapFolderAllRead(const AccountItem &account, const QString &folderId)
{
    if (account.imapHost.isEmpty() || account.isPop3() || (account.appPassword.isEmpty() && account.oauthAccessToken.isEmpty()))
        return;

    QSslSocket *socket = new QSslSocket(this);
    int *step = new int(0);
    QByteArray *buffer = new QByteArray();

    connect(socket, &QSslSocket::readyRead, this, [this, socket, account, folderId, step, buffer]() {
        if (!socket->isOpen())
            return;
        buffer->append(socket->readAll());
        if (!buffer->endsWith("\r\n"))
            return;

        QString respLatin1 = QString::fromLatin1(*buffer);

        if (*step == 0 && respLatin1.contains("* OK")) {
            *buffer = QByteArray();
            *step = 1;
            if (!account.oauthAccessToken.isEmpty() && !account.useSeparateSmtpAuth && account.imapUsername.isEmpty()) {
                QByteArray xoauth = QString("user=%1\x01" "auth=Bearer %2\x01\x01")
                                        .arg(account.email, account.oauthAccessToken)
                                        .toUtf8()
                                        .toBase64();
                socket->write("A01 AUTHENTICATE XOAUTH2 " + xoauth + "\r\n");
            } else {
                QString cmd = QString("A01 LOGIN \"%1\" \"%2\"\r\n")
                                  .arg(account.effectiveImapUser(), account.appPassword);
                socket->write(cmd.toUtf8());
            }
        } else if (*step == 1 && (respLatin1.startsWith("+ ") || respLatin1.contains("\r\n+ "))
                   && !respLatin1.contains("A01 NO") && !respLatin1.contains("A01 BAD")) {
            socket->write("\r\n");
        } else if (*step == 1 && (respLatin1.contains("A01 OK") || respLatin1.contains("A01 NO") || respLatin1.contains("A01 BAD"))) {
            if (!respLatin1.contains("A01 OK")) {
                if (!account.oauthRefreshToken.isEmpty()) {
                    refreshGoogleAccessToken(account.email, account.oauthRefreshToken);
                }
                socket->disconnectFromHost();
                return;
            }
            *buffer = QByteArray();
            *step = 2;
            socket->write("A02 LIST \"\" \"*\"\r\n");
        } else if (*step == 2 && (respLatin1.contains("\r\nA02 OK") || respLatin1.startsWith("A02 OK")
                                  || respLatin1.contains("A02 NO") || respLatin1.contains("A02 BAD"))) {
            QString targetMbox = resolveImapMailboxFromList(respLatin1, account, folderId);
            if (targetMbox.isEmpty())
                targetMbox = "INBOX";

            *buffer = QByteArray();
            *step = 3;
            socket->write(QString("A03 SELECT \"%1\"\r\n").arg(targetMbox).toLatin1());
        } else if (*step == 3 && (respLatin1.contains("A03 OK") || respLatin1.contains("A03 NO") || respLatin1.contains("A03 BAD"))) {
            if (!respLatin1.contains("A03 OK")) {
                socket->write("A05 LOGOUT\r\n");
                return;
            }
            *buffer = QByteArray();
            *step = 4;
            socket->write("A04 STORE 1:* +FLAGS (\\Seen)\r\n");
        } else if (*step == 4 && (respLatin1.contains("A04 OK") || respLatin1.contains("A04 NO") || respLatin1.contains("A04 BAD"))) {
            emit statusMessage(MailI18n::t(QString("[%1] '%2' 폴더 메일 모두 읽음(\\Seen) 처리 완료"),
                                           QString("[%1] Marked all messages in '%2' as read (\\Seen)"),
                                           QString("[%1] フォルダ '%2' のメールをすべて既読 (\\Seen) にしました"))
                                   .arg(account.email, folderTagForId(folderId)));
            *step = 5;
            socket->write("A05 LOGOUT\r\n");
        }
    });

    bindSocketLifecycleCleanup(
        socket,
        nullptr,
        [step, buffer]() {
            delete step;
            delete buffer;
        });

    socket->connectToHostEncrypted(account.imapHost, account.imapPort);
}

void GoogleProvider::setImapMessageSeenFlag(const AccountItem &account,
                                            const QString &folderId,
                                            int seqNum,
                                            bool seen,
                                            const QString &fallbackSubject)
{
    if (seqNum <= 0 || account.imapHost.isEmpty() || account.isPop3() || (account.appPassword.isEmpty() && account.oauthAccessToken.isEmpty()))
        return;

    QSslSocket *socket = new QSslSocket(this);
    int *step = new int(0);
    int *existsInFolder = new int(0);
    QByteArray *buffer = new QByteArray();

    connect(socket, &QSslSocket::readyRead, this,
            [this, socket, account, folderId, seqNum, seen, fallbackSubject, step, existsInFolder, buffer]() {
        if (!socket->isOpen())
            return;
        buffer->append(socket->readAll());
        if (!buffer->endsWith("\r\n"))
            return;

        QString respLatin1 = QString::fromLatin1(*buffer);

        if (*step == 0 && respLatin1.contains("* OK")) {
            *buffer = QByteArray();
            *step = 1;
            if (!account.oauthAccessToken.isEmpty() && !account.useSeparateSmtpAuth && account.imapUsername.isEmpty()) {
                QByteArray xoauth = QString("user=%1\x01" "auth=Bearer %2\x01\x01")
                                        .arg(account.email, account.oauthAccessToken)
                                        .toUtf8()
                                        .toBase64();
                socket->write("A01 AUTHENTICATE XOAUTH2 " + xoauth + "\r\n");
            } else {
                QString cmd = QString("A01 LOGIN \"%1\" \"%2\"\r\n")
                                  .arg(account.effectiveImapUser(), account.appPassword);
                socket->write(cmd.toUtf8());
            }
        } else if (*step == 1 && (respLatin1.startsWith("+ ") || respLatin1.contains("\r\n+ "))
                   && !respLatin1.contains("A01 NO") && !respLatin1.contains("A01 BAD")) {
            socket->write("\r\n");
        } else if (*step == 1 && (respLatin1.contains("A01 OK") || respLatin1.contains("A01 NO") || respLatin1.contains("A01 BAD"))) {
            if (!respLatin1.contains("A01 OK")) {
                if (!account.oauthRefreshToken.isEmpty()) {
                    refreshGoogleAccessToken(account.email, account.oauthRefreshToken);
                }
                socket->disconnectFromHost();
                return;
            }
            *buffer = QByteArray();
            *step = 2;
            socket->write("A02 LIST \"\" \"*\"\r\n");
        } else if (*step == 2 && (respLatin1.contains("\r\nA02 OK") || respLatin1.startsWith("A02 OK")
                                  || respLatin1.contains("A02 NO") || respLatin1.contains("A02 BAD"))) {
            QString targetMbox = resolveImapMailboxFromList(respLatin1, account, folderId);
            if (targetMbox.isEmpty())
                targetMbox = "INBOX";

            *buffer = QByteArray();
            *step = 3;
            socket->write(QString("A03 SELECT \"%1\"\r\n").arg(targetMbox).toLatin1());
        } else if (*step == 3 && (respLatin1.contains("A03 OK") || respLatin1.contains("A03 NO") || respLatin1.contains("A03 BAD"))) {
            static const QRegularExpression rxExists(R"(\*\s+(\d+)\s+EXISTS)", QRegularExpression::CaseInsensitiveOption);
            const QRegularExpressionMatch exM = rxExists.match(respLatin1);
            if (exM.hasMatch())
                *existsInFolder = exM.captured(1).toInt();
            if (!respLatin1.contains("A03 OK")) {
                socket->write("A05 LOGOUT\r\n");
                return;
            }
            *buffer = QByteArray();
            *step = 4;
            const QString flagOp = seen ? QStringLiteral("+FLAGS (\\Seen)") : QStringLiteral("-FLAGS (\\Seen)");
            socket->write(QString("A04 UID STORE %1 %2\r\n").arg(seqNum).arg(flagOp).toUtf8());
        } else if (*step == 4 && (respLatin1.contains("A04 OK") || respLatin1.contains("A04 NO") || respLatin1.contains("A04 BAD"))) {
            // 만약 필터 이동 등으로 인해 대상 폴더 내 실제 UID가 seqNum(옛 INBOX UID)과 달라서 FETCH 응답이 없었다면,
            // 최근 메일 헤더에서 제목(fallbackSubject)이 일치하는 메일의 실제 UID를 찾아 \Seen 플래그를 반영한다.
            if (!respLatin1.contains("FETCH", Qt::CaseInsensitive)
                && !fallbackSubject.trimmed().isEmpty()
                && *existsInFolder > 0) {
                const int startS = qMax(1, *existsInFolder - 49);
                *buffer = QByteArray();
                *step = 41;
                socket->write(QString("A04B FETCH %1:%2 (UID BODY.PEEK[HEADER.FIELDS (SUBJECT)])\r\n")
                                  .arg(startS)
                                  .arg(*existsInFolder)
                                  .toUtf8());
                return;
            }
            *step = 5;
            socket->write("A05 LOGOUT\r\n");
        } else if (*step == 41 && (respLatin1.contains("\r\nA04B OK") || respLatin1.startsWith("A04B OK")
                                   || respLatin1.contains("A04B NO") || respLatin1.contains("A04B BAD"))) {
            static const QRegularExpression rxHdrLit(R"(\*\s+(\d+)\s+FETCH\s*\(([^\r\n]*)\{(\d+)\}\r\n)",
                                                     QRegularExpression::CaseInsensitiveOption);
            static const QRegularExpression rxUid(R"(\bUID\s+(\d+))", QRegularExpression::CaseInsensitiveOption);
            int matchedUid = 0;
            int searchPos = 0;
            const QString wantSubj = fallbackSubject.trimmed();
            while (searchPos < respLatin1.size()) {
                const QRegularExpressionMatch hm = rxHdrLit.match(respLatin1, searchPos);
                if (!hm.hasMatch())
                    break;
                const QString meta = hm.captured(2);
                const int litLen = hm.captured(3).toInt();
                const int bStart = hm.capturedEnd();
                if (bStart + litLen > buffer->size())
                    return;
                QByteArray rawHdr = buffer->mid(bStart, litLen);
                if (!rawHdr.endsWith("\r\n\r\n"))
                    rawHdr.append("\r\n\r\n");
                searchPos = bStart + litLen;
                int uidVal = rxUid.match(meta).captured(1).toInt();
                if (uidVal <= 0) {
                    const QString tailMeta = respLatin1.mid(bStart + litLen, 64);
                    uidVal = rxUid.match(tailMeta).captured(1).toInt();
                }
                if (uidVal <= 0)
                    continue;
                QString dummyHtml, dummyPlain, parsedSubj;
                parseMimeEntity(rawHdr, dummyHtml, dummyPlain, nullptr, &parsedSubj, nullptr, nullptr,
                                nullptr, QString(), false, true);
                if (!wantSubj.isEmpty() && parsedSubj.trimmed().compare(wantSubj, Qt::CaseInsensitive) == 0) {
                    matchedUid = uidVal;
                }
            }
            *buffer = QByteArray();
            if (matchedUid > 0) {
                *step = 42;
                const QString flagOp = seen ? QStringLiteral("+FLAGS.SILENT (\\Seen)") : QStringLiteral("-FLAGS.SILENT (\\Seen)");
                socket->write(QString("A04C UID STORE %1 %2\r\n").arg(matchedUid).arg(flagOp).toUtf8());
                return;
            }
            *step = 5;
            socket->write("A05 LOGOUT\r\n");
        } else if (*step == 42 && (respLatin1.contains("A04C OK") || respLatin1.contains("A04C NO") || respLatin1.contains("A04C BAD"))) {
            *step = 5;
            socket->write("A05 LOGOUT\r\n");
        }
    });

    bindSocketLifecycleCleanup(
        socket,
        nullptr,
        [step, existsInFolder, buffer]() {
            delete step;
            delete existsInFolder;
            delete buffer;
        });

    socket->connectToHostEncrypted(account.imapHost, account.imapPort);
}

void GoogleProvider::setImapMessagesSeenFlagBatch(const AccountItem &account,
                                                  const QString &folderId,
                                                  const QList<int> &seqNums,
                                                  bool seen)
{
    if (seqNums.isEmpty() || account.imapHost.isEmpty() || account.isPop3()
        || (account.appPassword.isEmpty() && account.oauthAccessToken.isEmpty()))
        return;

    QStringList seqTokens;
    for (int s : seqNums) {
        if (s > 0)
            seqTokens.append(QString::number(s));
    }
    if (seqTokens.isEmpty())
        return;
    const QString seqSetStr = seqTokens.join(',');

    QSslSocket *socket = new QSslSocket(this);
    int *step = new int(0);
    QByteArray *buffer = new QByteArray();

    connect(socket, &QSslSocket::readyRead, this, [this, socket, account, folderId, seqSetStr, seen, step, buffer]() {
        if (!socket->isOpen())
            return;
        buffer->append(socket->readAll());
        if (!buffer->endsWith("\r\n"))
            return;

        QString respLatin1 = QString::fromLatin1(*buffer);

        if (*step == 0 && respLatin1.contains("* OK")) {
            *buffer = QByteArray();
            *step = 1;
            if (!account.oauthAccessToken.isEmpty() && !account.useSeparateSmtpAuth && account.imapUsername.isEmpty()) {
                QByteArray xoauth = QString("user=%1\x01" "auth=Bearer %2\x01\x01")
                                        .arg(account.email, account.oauthAccessToken)
                                        .toUtf8()
                                        .toBase64();
                socket->write("A01 AUTHENTICATE XOAUTH2 " + xoauth + "\r\n");
            } else {
                QString cmd = QString("A01 LOGIN \"%1\" \"%2\"\r\n")
                                  .arg(account.effectiveImapUser(), account.appPassword);
                socket->write(cmd.toUtf8());
            }
        } else if (*step == 1 && (respLatin1.startsWith("+ ") || respLatin1.contains("\r\n+ "))
                   && !respLatin1.contains("A01 NO") && !respLatin1.contains("A01 BAD")) {
            socket->write("\r\n");
        } else if (*step == 1 && (respLatin1.contains("A01 OK") || respLatin1.contains("A01 NO") || respLatin1.contains("A01 BAD"))) {
            if (!respLatin1.contains("A01 OK")) {
                if (!account.oauthRefreshToken.isEmpty()) {
                    refreshGoogleAccessToken(account.email, account.oauthRefreshToken);
                }
                socket->disconnectFromHost();
                return;
            }
            *buffer = QByteArray();
            *step = 2;
            socket->write("A02 LIST \"\" \"*\"\r\n");
        } else if (*step == 2 && (respLatin1.contains("\r\nA02 OK") || respLatin1.startsWith("A02 OK")
                                  || respLatin1.contains("A02 NO") || respLatin1.contains("A02 BAD"))) {
            QString targetMbox = resolveImapMailboxFromList(respLatin1, account, folderId);
            if (targetMbox.isEmpty())
                targetMbox = "INBOX";

            *buffer = QByteArray();
            *step = 3;
            socket->write(QString("A03 SELECT \"%1\"\r\n").arg(targetMbox).toLatin1());
        } else if (*step == 3 && (respLatin1.contains("A03 OK") || respLatin1.contains("A03 NO") || respLatin1.contains("A03 BAD"))) {
            if (!respLatin1.contains("A03 OK")) {
                socket->write("A05 LOGOUT\r\n");
                return;
            }
            *buffer = QByteArray();
            *step = 4;
            const QString flagOp = seen ? QStringLiteral("+FLAGS.SILENT (\\Seen)") : QStringLiteral("-FLAGS.SILENT (\\Seen)");
            socket->write(QString("A04 UID STORE %1 %2\r\n").arg(seqSetStr, flagOp).toUtf8());
        } else if (*step == 4 && (respLatin1.contains("A04 OK") || respLatin1.contains("A04 NO") || respLatin1.contains("A04 BAD"))) {
            *step = 5;
            socket->write("A05 LOGOUT\r\n");
        }
    });

    bindSocketLifecycleCleanup(
        socket,
        nullptr,
        [step, buffer]() {
            delete step;
            delete buffer;
        });

    socket->connectToHostEncrypted(account.imapHost, account.imapPort);
}

void GoogleProvider::setImapMessageAnsweredOrForwarded(const AccountItem &account,
                                                       const QString &folderId,
                                                       int seqNum,
                                                       bool answered,
                                                       bool forwarded,
                                                       const QString &fallbackSubject)
{
    if (seqNum <= 0 || (!answered && !forwarded) || account.imapHost.isEmpty() || account.isPop3()
        || (account.appPassword.isEmpty() && account.oauthAccessToken.isEmpty()))
        return;

    QStringList flagsToAdd;
    if (answered)
        flagsToAdd << QStringLiteral("\\Answered");
    if (forwarded)
        flagsToAdd << QStringLiteral("$Forwarded");
    const QString flagListStr = flagsToAdd.join(' ');

    QSslSocket *socket = new QSslSocket(this);
    int *step = new int(0);
    int *existsInFolder = new int(0);
    QByteArray *buffer = new QByteArray();

    connect(socket, &QSslSocket::readyRead, this,
            [this, socket, account, folderId, seqNum, flagListStr, fallbackSubject, step, existsInFolder, buffer]() {
        if (!socket->isOpen())
            return;
        buffer->append(socket->readAll());
        if (!buffer->endsWith("\r\n"))
            return;

        QString respLatin1 = QString::fromLatin1(*buffer);

        if (*step == 0 && respLatin1.contains("* OK")) {
            *buffer = QByteArray();
            *step = 1;
            if (!account.oauthAccessToken.isEmpty() && !account.useSeparateSmtpAuth && account.imapUsername.isEmpty()) {
                QByteArray xoauth = QString("user=%1\x01" "auth=Bearer %2\x01\x01")
                                        .arg(account.email, account.oauthAccessToken)
                                        .toUtf8()
                                        .toBase64();
                socket->write("A01 AUTHENTICATE XOAUTH2 " + xoauth + "\r\n");
            } else {
                QString cmd = QString("A01 LOGIN \"%1\" \"%2\"\r\n")
                                  .arg(account.effectiveImapUser(), account.appPassword);
                socket->write(cmd.toUtf8());
            }
        } else if (*step == 1 && (respLatin1.startsWith("+ ") || respLatin1.contains("\r\n+ "))
                   && !respLatin1.contains("A01 NO") && !respLatin1.contains("A01 BAD")) {
            socket->write("\r\n");
        } else if (*step == 1 && (respLatin1.contains("A01 OK") || respLatin1.contains("A01 NO") || respLatin1.contains("A01 BAD"))) {
            if (!respLatin1.contains("A01 OK")) {
                if (!account.oauthRefreshToken.isEmpty()) {
                    refreshGoogleAccessToken(account.email, account.oauthRefreshToken);
                }
                socket->disconnectFromHost();
                return;
            }
            *buffer = QByteArray();
            *step = 2;
            socket->write("A02 LIST \"\" \"*\"\r\n");
        } else if (*step == 2 && (respLatin1.contains("\r\nA02 OK") || respLatin1.startsWith("A02 OK")
                                  || respLatin1.contains("A02 NO") || respLatin1.contains("A02 BAD"))) {
            QString targetMbox = resolveImapMailboxFromList(respLatin1, account, folderId);
            if (targetMbox.isEmpty())
                targetMbox = "INBOX";

            *buffer = QByteArray();
            *step = 3;
            socket->write(QString("A03 SELECT \"%1\"\r\n").arg(targetMbox).toLatin1());
        } else if (*step == 3 && (respLatin1.contains("A03 OK") || respLatin1.contains("A03 NO") || respLatin1.contains("A03 BAD"))) {
            static const QRegularExpression rxExists(R"(\*\s+(\d+)\s+EXISTS)", QRegularExpression::CaseInsensitiveOption);
            const QRegularExpressionMatch exM = rxExists.match(respLatin1);
            if (exM.hasMatch())
                *existsInFolder = exM.captured(1).toInt();
            if (!respLatin1.contains("A03 OK")) {
                socket->write("A05 LOGOUT\r\n");
                return;
            }
            *buffer = QByteArray();
            *step = 4;
            socket->write(QString("A04 UID STORE %1 +FLAGS (%2)\r\n").arg(seqNum).arg(flagListStr).toUtf8());
        } else if (*step == 4 && (respLatin1.contains("A04 OK") || respLatin1.contains("A04 NO") || respLatin1.contains("A04 BAD"))) {
            if (!respLatin1.contains("FETCH", Qt::CaseInsensitive)
                && !fallbackSubject.trimmed().isEmpty()
                && *existsInFolder > 0) {
                const int startS = qMax(1, *existsInFolder - 49);
                *buffer = QByteArray();
                *step = 41;
                socket->write(QString("A04B FETCH %1:%2 (UID BODY.PEEK[HEADER.FIELDS (SUBJECT)])\r\n")
                                  .arg(startS)
                                  .arg(*existsInFolder)
                                  .toUtf8());
                return;
            }
            *step = 5;
            socket->write("A05 LOGOUT\r\n");
        } else if (*step == 41 && (respLatin1.contains("\r\nA04B OK") || respLatin1.startsWith("A04B OK")
                                   || respLatin1.contains("A04B NO") || respLatin1.contains("A04B BAD"))) {
            static const QRegularExpression rxHdrLit(R"(\*\s+(\d+)\s+FETCH\s*\(([^\r\n]*)\{(\d+)\}\r\n)",
                                                     QRegularExpression::CaseInsensitiveOption);
            static const QRegularExpression rxUid(R"(\bUID\s+(\d+))", QRegularExpression::CaseInsensitiveOption);
            int matchedUid = 0;
            int searchPos = 0;
            const QString wantSubj = fallbackSubject.trimmed();
            while (searchPos < respLatin1.size()) {
                const QRegularExpressionMatch hm = rxHdrLit.match(respLatin1, searchPos);
                if (!hm.hasMatch())
                    break;
                const QString meta = hm.captured(2);
                const int litLen = hm.captured(3).toInt();
                const int bStart = hm.capturedEnd();
                if (bStart + litLen > buffer->size())
                    return;
                QByteArray rawHdr = buffer->mid(bStart, litLen);
                if (!rawHdr.endsWith("\r\n\r\n"))
                    rawHdr.append("\r\n\r\n");
                searchPos = bStart + litLen;
                int uidVal = rxUid.match(meta).captured(1).toInt();
                if (uidVal <= 0) {
                    const QString tailMeta = respLatin1.mid(bStart + litLen, 64);
                    uidVal = rxUid.match(tailMeta).captured(1).toInt();
                }
                if (uidVal <= 0)
                    continue;
                QString dummyHtml, dummyPlain, parsedSubj;
                parseMimeEntity(rawHdr, dummyHtml, dummyPlain, nullptr, &parsedSubj, nullptr, nullptr,
                                nullptr, QString(), false, true);
                if (!wantSubj.isEmpty() && parsedSubj.trimmed().compare(wantSubj, Qt::CaseInsensitive) == 0) {
                    matchedUid = uidVal;
                }
            }
            *buffer = QByteArray();
            if (matchedUid > 0) {
                *step = 42;
                socket->write(QString("A04C UID STORE %1 +FLAGS.SILENT (%2)\r\n").arg(matchedUid).arg(flagListStr).toUtf8());
                return;
            }
            *step = 5;
            socket->write("A05 LOGOUT\r\n");
        } else if (*step == 42 && (respLatin1.contains("A04C OK") || respLatin1.contains("A04C NO") || respLatin1.contains("A04C BAD"))) {
            *step = 5;
            socket->write("A05 LOGOUT\r\n");
        }
    });

    bindSocketLifecycleCleanup(
        socket,
        nullptr,
        [step, existsInFolder, buffer]() {
            delete step;
            delete existsInFolder;
            delete buffer;
        });

    socket->connectToHostEncrypted(account.imapHost, account.imapPort);
}

void GoogleProvider::setImapMessageFlagged(const AccountItem &account,
                                           const QString &folderId,
                                           int seqNum,
                                           bool flagged,
                                           const QString &fallbackSubject)
{
    if (seqNum <= 0 || account.imapHost.isEmpty() || account.isPop3() || (account.appPassword.isEmpty() && account.oauthAccessToken.isEmpty()))
        return;

    QSslSocket *socket = new QSslSocket(this);
    int *step = new int(0);
    int *existsInFolder = new int(0);
    QByteArray *buffer = new QByteArray();

    connect(socket, &QSslSocket::readyRead, this,
            [this, socket, account, folderId, seqNum, flagged, fallbackSubject, step, existsInFolder, buffer]() {
        if (!socket->isOpen())
            return;
        buffer->append(socket->readAll());
        if (!buffer->endsWith("\r\n"))
            return;

        QString respLatin1 = QString::fromLatin1(*buffer);

        if (*step == 0 && respLatin1.contains("* OK")) {
            *buffer = QByteArray();
            *step = 1;
            if (!account.oauthAccessToken.isEmpty() && !account.useSeparateSmtpAuth && account.imapUsername.isEmpty()) {
                QByteArray xoauth = QString("user=%1\x01" "auth=Bearer %2\x01\x01")
                                        .arg(account.email, account.oauthAccessToken)
                                        .toUtf8()
                                        .toBase64();
                socket->write("A01 AUTHENTICATE XOAUTH2 " + xoauth + "\r\n");
            } else {
                QString cmd = QString("A01 LOGIN \"%1\" \"%2\"\r\n")
                                  .arg(account.effectiveImapUser(), account.appPassword);
                socket->write(cmd.toUtf8());
            }
        } else if (*step == 1 && (respLatin1.startsWith("+ ") || respLatin1.contains("\r\n+ "))
                   && !respLatin1.contains("A01 NO") && !respLatin1.contains("A01 BAD")) {
            socket->write("\r\n");
        } else if (*step == 1 && (respLatin1.contains("A01 OK") || respLatin1.contains("A01 NO") || respLatin1.contains("A01 BAD"))) {
            if (!respLatin1.contains("A01 OK")) {
                if (!account.oauthRefreshToken.isEmpty()) {
                    refreshGoogleAccessToken(account.email, account.oauthRefreshToken);
                }
                socket->disconnectFromHost();
                return;
            }
            *buffer = QByteArray();
            *step = 2;
            socket->write("A02 LIST \"\" \"*\"\r\n");
        } else if (*step == 2 && (respLatin1.contains("\r\nA02 OK") || respLatin1.startsWith("A02 OK")
                                  || respLatin1.contains("A02 NO") || respLatin1.contains("A02 BAD"))) {
            QString targetMbox = resolveImapMailboxFromList(respLatin1, account, folderId);
            if (targetMbox.isEmpty())
                targetMbox = "INBOX";

            *buffer = QByteArray();
            *step = 3;
            socket->write(QString("A03 SELECT \"%1\"\r\n").arg(targetMbox).toLatin1());
        } else if (*step == 3 && (respLatin1.contains("A03 OK") || respLatin1.contains("A03 NO") || respLatin1.contains("A03 BAD"))) {
            if (!respLatin1.contains("A03 OK")) {
                socket->write("A05 LOGOUT\r\n");
                return;
            }
            static const QRegularExpression existsRe(R"(\*\s+(\d+)\s+EXISTS)", QRegularExpression::CaseInsensitiveOption);
            QRegularExpressionMatch em = existsRe.match(respLatin1);
            *existsInFolder = em.hasMatch() ? em.captured(1).toInt() : 0;
            *buffer = QByteArray();
            *step = 4;
            const QString flagOp = flagged ? QStringLiteral("+FLAGS.SILENT (\\Flagged)") : QStringLiteral("-FLAGS.SILENT (\\Flagged)");
            socket->write(QString("A04 UID STORE %1 %2\r\n").arg(seqNum).arg(flagOp).toUtf8());
        } else if (*step == 4 && (respLatin1.contains("A04 OK") || respLatin1.contains("A04 NO") || respLatin1.contains("A04 BAD"))) {
            if (!fallbackSubject.trimmed().isEmpty() && *existsInFolder > 0) {
                const int startS = qMax(1, *existsInFolder - 49);
                *buffer = QByteArray();
                *step = 41;
                socket->write(QString("A04B FETCH %1:%2 (UID BODY.PEEK[HEADER.FIELDS (SUBJECT)])\r\n")
                                  .arg(startS)
                                  .arg(*existsInFolder)
                                  .toLatin1());
                return;
            }
            *step = 5;
            socket->write("A05 LOGOUT\r\n");
        } else if (*step == 41 && (respLatin1.contains("\r\nA04B OK") || respLatin1.startsWith("A04B OK")
                                   || respLatin1.contains("A04B NO") || respLatin1.contains("A04B BAD"))) {
            static const QRegularExpression rxHdrLit(R"(\*\s+(\d+)\s+FETCH\s*\(([^\r\n]*)\{(\d+)\}\r\n)",
                                                     QRegularExpression::CaseInsensitiveOption);
            static const QRegularExpression rxUid(R"(\bUID\s+(\d+))", QRegularExpression::CaseInsensitiveOption);
            int matchedUid = 0;
            int searchPos = 0;
            const QString wantSubj = fallbackSubject.trimmed();
            while (searchPos < respLatin1.size()) {
                const QRegularExpressionMatch hm = rxHdrLit.match(respLatin1, searchPos);
                if (!hm.hasMatch())
                    break;
                const QString meta = hm.captured(2);
                const int litLen = hm.captured(3).toInt();
                const int bStart = hm.capturedEnd();
                if (bStart + litLen > buffer->size())
                    return;
                QByteArray rawHdr = buffer->mid(bStart, litLen);
                if (!rawHdr.endsWith("\r\n\r\n"))
                    rawHdr.append("\r\n\r\n");
                searchPos = bStart + litLen;
                int uidVal = rxUid.match(meta).captured(1).toInt();
                if (uidVal <= 0) {
                    const QString tailMeta = respLatin1.mid(bStart + litLen, 64);
                    uidVal = rxUid.match(tailMeta).captured(1).toInt();
                }
                if (uidVal <= 0)
                    continue;
                QString dummyHtml, dummyPlain, parsedSubj;
                parseMimeEntity(rawHdr, dummyHtml, dummyPlain, nullptr, &parsedSubj, nullptr, nullptr,
                                nullptr, QString(), false, true);
                if (!wantSubj.isEmpty() && parsedSubj.trimmed().compare(wantSubj, Qt::CaseInsensitive) == 0) {
                    matchedUid = uidVal;
                }
            }
            *buffer = QByteArray();
            if (matchedUid > 0 && matchedUid != seqNum) {
                *step = 42;
                const QString flagOp = flagged ? QStringLiteral("+FLAGS.SILENT (\\Flagged)") : QStringLiteral("-FLAGS.SILENT (\\Flagged)");
                socket->write(QString("A04C UID STORE %1 %2\r\n").arg(matchedUid).arg(flagOp).toUtf8());
                return;
            }
            *step = 5;
            socket->write("A05 LOGOUT\r\n");
        } else if (*step == 42 && (respLatin1.contains("A04C OK") || respLatin1.contains("A04C NO") || respLatin1.contains("A04C BAD"))) {
            *step = 5;
            socket->write("A05 LOGOUT\r\n");
        }
    });

    bindSocketLifecycleCleanup(
        socket,
        nullptr,
        [step, existsInFolder, buffer]() {
            delete step;
            delete existsInFolder;
            delete buffer;
        });

    socket->connectToHostEncrypted(account.imapHost, account.imapPort);
}

void GoogleProvider::setImapMessageImportant(const AccountItem &account,
                                             const QString &folderId,
                                             int seqNum,
                                             bool important,
                                             const QString &fallbackSubject)
{
    if (seqNum <= 0 || account.imapHost.isEmpty() || account.isPop3() || (account.appPassword.isEmpty() && account.oauthAccessToken.isEmpty()))
        return;

    QSslSocket *socket = new QSslSocket(this);
    int *step = new int(0);
    int *existsInFolder = new int(0);
    QByteArray *buffer = new QByteArray();

    connect(socket, &QSslSocket::readyRead, this,
            [this, socket, account, folderId, seqNum, important, fallbackSubject, step, existsInFolder, buffer]() {
        if (!socket->isOpen())
            return;
        buffer->append(socket->readAll());
        if (!buffer->endsWith("\r\n"))
            return;

        QString respLatin1 = QString::fromLatin1(*buffer);

        if (*step == 0 && respLatin1.contains("* OK")) {
            *buffer = QByteArray();
            *step = 1;
            if (!account.oauthAccessToken.isEmpty() && !account.useSeparateSmtpAuth && account.imapUsername.isEmpty()) {
                QByteArray xoauth = QString("user=%1\x01" "auth=Bearer %2\x01\x01")
                                        .arg(account.email, account.oauthAccessToken)
                                        .toUtf8()
                                        .toBase64();
                socket->write("A01 AUTHENTICATE XOAUTH2 " + xoauth + "\r\n");
            } else {
                QString cmd = QString("A01 LOGIN \"%1\" \"%2\"\r\n")
                                  .arg(account.effectiveImapUser(), account.appPassword);
                socket->write(cmd.toUtf8());
            }
        } else if (*step == 1 && (respLatin1.startsWith("+ ") || respLatin1.contains("\r\n+ "))
                   && !respLatin1.contains("A01 NO") && !respLatin1.contains("A01 BAD")) {
            socket->write("\r\n");
        } else if (*step == 1 && (respLatin1.contains("A01 OK") || respLatin1.contains("A01 NO") || respLatin1.contains("A01 BAD"))) {
            if (!respLatin1.contains("A01 OK")) {
                if (!account.oauthRefreshToken.isEmpty()) {
                    refreshGoogleAccessToken(account.email, account.oauthRefreshToken);
                }
                socket->disconnectFromHost();
                return;
            }
            *buffer = QByteArray();
            *step = 2;
            socket->write("A02 LIST \"\" \"*\"\r\n");
        } else if (*step == 2 && (respLatin1.contains("\r\nA02 OK") || respLatin1.startsWith("A02 OK")
                                  || respLatin1.contains("A02 NO") || respLatin1.contains("A02 BAD"))) {
            QString targetMbox = resolveImapMailboxFromList(respLatin1, account, folderId);
            if (targetMbox.isEmpty())
                targetMbox = "INBOX";

            *buffer = QByteArray();
            *step = 3;
            socket->write(QString("A03 SELECT \"%1\"\r\n").arg(targetMbox).toLatin1());
        } else if (*step == 3 && (respLatin1.contains("A03 OK") || respLatin1.contains("A03 NO") || respLatin1.contains("A03 BAD"))) {
            if (!respLatin1.contains("A03 OK")) {
                socket->write("A05 LOGOUT\r\n");
                return;
            }
            static const QRegularExpression existsRe(R"(\*\s+(\d+)\s+EXISTS)", QRegularExpression::CaseInsensitiveOption);
            QRegularExpressionMatch em = existsRe.match(respLatin1);
            *existsInFolder = em.hasMatch() ? em.captured(1).toInt() : 0;
            *buffer = QByteArray();
            *step = 4;
            const QString flagOp = important ? QStringLiteral("+FLAGS.SILENT ($Important)") : QStringLiteral("-FLAGS.SILENT ($Important \\Important)");
            socket->write(QString("A04 UID STORE %1 %2\r\n").arg(seqNum).arg(flagOp).toUtf8());
        } else if (*step == 4 && (respLatin1.contains("A04 OK") || respLatin1.contains("A04 NO") || respLatin1.contains("A04 BAD"))) {
            if (!fallbackSubject.trimmed().isEmpty() && *existsInFolder > 0) {
                const int startS = qMax(1, *existsInFolder - 49);
                *buffer = QByteArray();
                *step = 41;
                socket->write(QString("A04B FETCH %1:%2 (UID BODY.PEEK[HEADER.FIELDS (SUBJECT)])\r\n")
                                  .arg(startS)
                                  .arg(*existsInFolder)
                                  .toLatin1());
                return;
            }
            *step = 5;
            socket->write("A05 LOGOUT\r\n");
        } else if (*step == 41 && (respLatin1.contains("\r\nA04B OK") || respLatin1.startsWith("A04B OK")
                                   || respLatin1.contains("A04B NO") || respLatin1.contains("A04B BAD"))) {
            static const QRegularExpression rxHdrLit(R"(\*\s+(\d+)\s+FETCH\s*\(([^\r\n]*)\{(\d+)\}\r\n)",
                                                     QRegularExpression::CaseInsensitiveOption);
            static const QRegularExpression rxUid(R"(\bUID\s+(\d+))", QRegularExpression::CaseInsensitiveOption);
            int matchedUid = 0;
            int searchPos = 0;
            const QString wantSubj = fallbackSubject.trimmed();
            while (searchPos < respLatin1.size()) {
                const QRegularExpressionMatch hm = rxHdrLit.match(respLatin1, searchPos);
                if (!hm.hasMatch())
                    break;
                const QString meta = hm.captured(2);
                const int litLen = hm.captured(3).toInt();
                const int bStart = hm.capturedEnd();
                if (bStart + litLen > buffer->size())
                    return;
                QByteArray rawHdr = buffer->mid(bStart, litLen);
                if (!rawHdr.endsWith("\r\n\r\n"))
                    rawHdr.append("\r\n\r\n");
                searchPos = bStart + litLen;
                int uidVal = rxUid.match(meta).captured(1).toInt();
                if (uidVal <= 0) {
                    const QString tailMeta = respLatin1.mid(bStart + litLen, 64);
                    uidVal = rxUid.match(tailMeta).captured(1).toInt();
                }
                if (uidVal <= 0)
                    continue;
                QString dummyHtml, dummyPlain, parsedSubj;
                parseMimeEntity(rawHdr, dummyHtml, dummyPlain, nullptr, &parsedSubj, nullptr, nullptr,
                                nullptr, QString(), false, true);
                if (!wantSubj.isEmpty() && parsedSubj.trimmed().compare(wantSubj, Qt::CaseInsensitive) == 0) {
                    matchedUid = uidVal;
                }
            }
            *buffer = QByteArray();
            if (matchedUid > 0 && matchedUid != seqNum) {
                *step = 42;
                const QString flagOp = important ? QStringLiteral("+FLAGS.SILENT ($Important)") : QStringLiteral("-FLAGS.SILENT ($Important \\Important)");
                socket->write(QString("A04C UID STORE %1 %2\r\n").arg(matchedUid).arg(flagOp).toUtf8());
                return;
            }
            *step = 5;
            socket->write("A05 LOGOUT\r\n");
        } else if (*step == 42 && (respLatin1.contains("A04C OK") || respLatin1.contains("A04C NO") || respLatin1.contains("A04C BAD"))) {
            *step = 5;
            socket->write("A05 LOGOUT\r\n");
        }
    });

    bindSocketLifecycleCleanup(
        socket,
        nullptr,
        [step, existsInFolder, buffer]() {
            delete step;
            delete existsInFolder;
            delete buffer;
        });

    socket->connectToHostEncrypted(account.imapHost, account.imapPort);
}

void GoogleProvider::restoreImapMessageFromTrash(const AccountItem &account,
                                                 int seqNum,
                                                 const QString &targetFolderId,
                                                 const QString &sourceFolderId)
{
    const QString srcId = (sourceFolderId.trimmed().compare(QStringLiteral("spam"), Qt::CaseInsensitive) == 0)
                              ? QStringLiteral("spam")
                              : QStringLiteral("trash");
    if (account.isPop3()) {
        if (seqNum <= 0)
            return;
        account.ensurePop3MailboxDirs();
        const QString destId = targetFolderId.trimmed().isEmpty() ? QStringLiteral("inbox") : targetFolderId.trimmed();
        const QString dstDir = account.pop3MailboxDir(destId);
        QDir().mkpath(dstDir);
        const QString dstFile = dstDir + QString("/msg_%1.eml").arg(seqNum);
        const QString primarySrcFile = account.pop3MailboxDir(srcId) + QString("/msg_%1.eml").arg(seqNum);
        const QString trashFile = account.pop3MailboxDir(QStringLiteral("trash")) + QString("/msg_%1.eml").arg(seqNum);
        const QString spamFile = account.pop3MailboxDir(QStringLiteral("spam")) + QString("/msg_%1.eml").arg(seqNum);
        if (QFileInfo::exists(primarySrcFile)) {
            QFile::remove(dstFile);
            QFile::rename(primarySrcFile, dstFile);
        } else if (QFileInfo::exists(trashFile)) {
            QFile::remove(dstFile);
            QFile::rename(trashFile, dstFile);
        } else if (QFileInfo::exists(spamFile)) {
            QFile::remove(dstFile);
            QFile::rename(spamFile, dstFile);
        }
        emit statusMessage(MailI18n::t(QString("[%1] POP3 로컬 '%2' 메일을 '%3' 폴더로 복원 완료"),
                                       QString("[%1] Restored local POP3 message from '%2' to '%3'"),
                                       QString("[%1] POP3 ローカル '%2' のメールを '%3' に復元しました"))
                               .arg(account.email, folderTagForId(srcId), folderTagForId(destId)));
        return;
    }
    if (seqNum <= 0 || account.imapHost.isEmpty() || (account.appPassword.isEmpty() && account.oauthAccessToken.isEmpty()))
        return;

    QSslSocket *socket = new QSslSocket(this);
    int *step = new int(0);
    int *effectiveSeq = new int(seqNum);
    QByteArray *buffer = new QByteArray();
    QString *destMbox = new QString("INBOX");

    connect(socket, &QSslSocket::readyRead, this,
            [this, socket, account, seqNum, targetFolderId, srcId, step, effectiveSeq, buffer, destMbox]() {
        if (!socket->isOpen())
            return;
        buffer->append(socket->readAll());
        if (!buffer->endsWith("\r\n"))
            return;

        QString respLatin1 = QString::fromLatin1(*buffer);

        if (*step == 0 && respLatin1.contains("* OK")) {
            *buffer = QByteArray();
            *step = 1;
            if (!account.oauthAccessToken.isEmpty() && !account.useSeparateSmtpAuth && account.imapUsername.isEmpty()) {
                QByteArray xoauth = QString("user=%1\x01" "auth=Bearer %2\x01\x01")
                                        .arg(account.email, account.oauthAccessToken)
                                        .toUtf8()
                                        .toBase64();
                socket->write("A01 AUTHENTICATE XOAUTH2 " + xoauth + "\r\n");
            } else {
                QString cmd = QString("A01 LOGIN \"%1\" \"%2\"\r\n")
                                  .arg(account.effectiveImapUser(), account.appPassword);
                socket->write(cmd.toUtf8());
            }
        } else if (*step == 1 && (respLatin1.startsWith("+ ") || respLatin1.contains("\r\n+ "))
                   && !respLatin1.contains("A01 NO") && !respLatin1.contains("A01 BAD")) {
            socket->write("\r\n");
        } else if (*step == 1 && (respLatin1.contains("A01 OK") || respLatin1.contains("A01 NO") || respLatin1.contains("A01 BAD"))) {
            if (!respLatin1.contains("A01 OK")) {
                if (!account.oauthRefreshToken.isEmpty()) {
                    refreshGoogleAccessToken(account.email, account.oauthRefreshToken);
                }
                socket->disconnectFromHost();
                return;
            }
            *buffer = QByteArray();
            *step = 2;
            socket->write("A02 LIST \"\" \"*\"\r\n");
        } else if (*step == 2 && (respLatin1.contains("\r\nA02 OK") || respLatin1.startsWith("A02 OK")
                                  || respLatin1.contains("A02 NO") || respLatin1.contains("A02 BAD"))) {
            QString srcMbox = resolveImapMailboxFromList(respLatin1, account, srcId);
            *destMbox = QStringLiteral("INBOX");
            if (!targetFolderId.isEmpty() && targetFolderId != "inbox") {
                QString resolvedDest = resolveImapMailboxFromList(respLatin1, account, targetFolderId);
                if (!resolvedDest.isEmpty())
                    *destMbox = resolvedDest;
            }
            if (srcMbox.isEmpty()) {
                const QString fallbackName = (srcId == QStringLiteral("spam")) ? QStringLiteral("Spam") : QStringLiteral("Trash");
                srcMbox = resolveImapMailboxFromList(respLatin1, account, fallbackName);
                if (srcMbox.isEmpty())
                    srcMbox = buildImapCustomMailboxCandidate(respLatin1, fallbackName);
            }

            *buffer = QByteArray();
            *step = 3;
            socket->write(QString("A03 SELECT \"%1\"\r\n").arg(srcMbox).toLatin1());
        } else if (*step == 3 && (respLatin1.contains("A03 OK") || respLatin1.contains("A03 NO") || respLatin1.contains("A03 BAD"))) {
            if (!respLatin1.contains("A03 OK")) {
                *step = 6;
                socket->write("A06 LOGOUT\r\n");
                return;
            }
            static const QRegularExpression rxExists(R"(\*\s+(\d+)\s+EXISTS)", QRegularExpression::CaseInsensitiveOption);
            const QRegularExpressionMatch em = rxExists.match(respLatin1);
            const int existsCount = em.hasMatch() ? em.captured(1).toInt() : 0;
            if (existsCount <= 0) {
                *step = 6;
                socket->write("A06 LOGOUT\r\n");
                return;
            }
            *effectiveSeq = seqNum;
            *buffer = QByteArray();
            *step = 4;
            socket->write(QString("A04 UID COPY %1 \"%2\"\r\n").arg(*effectiveSeq).arg(*destMbox).toLatin1());
        } else if (*step == 4 && (respLatin1.contains("A04 OK") || respLatin1.contains("A04 NO") || respLatin1.contains("A04 BAD"))) {
            if (!respLatin1.contains("A04 OK")) {
                *step = 7;
                socket->write("A07 LOGOUT\r\n");
                return;
            }
            *buffer = QByteArray();
            *step = 5;
            socket->write(QString("A05 UID STORE %1 +FLAGS.SILENT (\\Deleted)\r\n").arg(*effectiveSeq).toLatin1());
        } else if (*step == 5 && (respLatin1.contains("A05 OK") || respLatin1.contains("A05 NO") || respLatin1.contains("A05 BAD"))) {
            *buffer = QByteArray();
            *step = 6;
            socket->write("A06 EXPUNGE\r\n");
        } else if (*step == 6 && (respLatin1.contains("A06 OK") || respLatin1.contains("A06 NO") || respLatin1.contains("A06 BAD"))) {
            unregisterLoadedImapUid(account.id, srcId, *effectiveSeq);
            emit statusMessage(MailI18n::t(QString("[%1] '%2' 메일을 '%3' 편지함으로 복원 완료"),
                                           QString("[%1] Restored message from '%2' to '%3'"),
                                           QString("[%1] '%2' のメールを '%3' に復元しました"))
                                   .arg(account.email, folderTagForId(srcId), folderTagForId(targetFolderId)));
            *step = 7;
            socket->write("A07 LOGOUT\r\n");
        }
    });

    bindSocketLifecycleCleanup(
        socket,
        nullptr,
        [step, effectiveSeq, buffer, destMbox]() {
            delete step;
            delete effectiveSeq;
            delete buffer;
            delete destMbox;
        });

    socket->connectToHostEncrypted(account.imapHost, account.imapPort);
}

void GoogleProvider::deleteOrMoveImapMessage(const AccountItem &account,
                                             const QString &sourceFolderId,
                                             int seqNum,
                                             bool permanentDelete)
{
    if (account.isPop3()) {
        account.ensurePop3MailboxDirs();
        const QString srcFile = account.pop3MailboxDir(sourceFolderId) + QString("/msg_%1.eml").arg(seqNum);
        if (QFileInfo::exists(srcFile)) {
            if (permanentDelete || sourceFolderId == QStringLiteral("trash") || sourceFolderId == QStringLiteral("spam")) {
                QFile::remove(srcFile);
            } else {
                const QString dstFile = account.pop3MailboxDir(QStringLiteral("trash")) + QString("/msg_%1.eml").arg(seqNum);
                QFile::remove(dstFile);
                QFile::rename(srcFile, dstFile);
            }
        }
        return;
    }
    if (seqNum <= 0 || account.imapHost.isEmpty() || (account.appPassword.isEmpty() && account.oauthAccessToken.isEmpty()))
        return;

    QSslSocket *socket = new QSslSocket(this);
    int *step = new int(0);
    int *effectiveSeq = new int(seqNum);
    QByteArray *buffer = new QByteArray();
    QString *srcMbox = new QString("INBOX");
    QString *trashMbox = new QString("Trash");

    connect(socket, &QSslSocket::readyRead, this,
            [this, socket, account, sourceFolderId, seqNum, permanentDelete, step, effectiveSeq, buffer, srcMbox, trashMbox]() {
        if (!socket->isOpen())
            return;
        buffer->append(socket->readAll());
        if (!buffer->endsWith("\r\n"))
            return;

        QString respLatin1 = QString::fromLatin1(*buffer);

        if (*step == 0 && respLatin1.contains("* OK")) {
            *buffer = QByteArray();
            *step = 1;
            if (!account.oauthAccessToken.isEmpty() && !account.useSeparateSmtpAuth && account.imapUsername.isEmpty()) {
                QByteArray xoauth = QString("user=%1\x01" "auth=Bearer %2\x01\x01")
                                        .arg(account.email, account.oauthAccessToken)
                                        .toUtf8()
                                        .toBase64();
                socket->write("A01 AUTHENTICATE XOAUTH2 " + xoauth + "\r\n");
            } else {
                QString cmd = QString("A01 LOGIN \"%1\" \"%2\"\r\n")
                                  .arg(account.effectiveImapUser(), account.appPassword);
                socket->write(cmd.toUtf8());
            }
        } else if (*step == 1 && (respLatin1.startsWith("+ ") || respLatin1.contains("\r\n+ "))
                   && !respLatin1.contains("A01 NO") && !respLatin1.contains("A01 BAD")) {
            socket->write("\r\n");
        } else if (*step == 1 && (respLatin1.contains("A01 OK") || respLatin1.contains("A01 NO") || respLatin1.contains("A01 BAD"))) {
            if (!respLatin1.contains("A01 OK")) {
                if (!account.oauthRefreshToken.isEmpty()) {
                    refreshGoogleAccessToken(account.email, account.oauthRefreshToken);
                }
                socket->disconnectFromHost();
                return;
            }
            *buffer = QByteArray();
            *step = 2;
            socket->write("A02 LIST \"\" \"*\"\r\n");
        } else if (*step == 2 && (respLatin1.contains("\r\nA02 OK") || respLatin1.startsWith("A02 OK")
                                  || respLatin1.contains("A02 NO") || respLatin1.contains("A02 BAD"))) {
            QString resolvedSrc = resolveImapMailboxFromList(respLatin1, account, sourceFolderId);
            if (resolvedSrc.isEmpty()) {
                if (sourceFolderId == "inbox" || sourceFolderId == "starred")
                    resolvedSrc = QStringLiteral("INBOX");
                else if (sourceFolderId == "trash")
                    resolvedSrc = QStringLiteral("Trash");
                else if (sourceFolderId == "spam")
                    resolvedSrc = QStringLiteral("Spam");
                else if (sourceFolderId == "sent")
                    resolvedSrc = QStringLiteral("Sent");
                else if (sourceFolderId == "drafts")
                    resolvedSrc = QStringLiteral("Drafts");
                else
                    resolvedSrc = QStringLiteral("INBOX");
            }
            *srcMbox = resolvedSrc;

            QString resolvedTrash = resolveImapMailboxFromList(respLatin1, account, QStringLiteral("trash"));
            if (resolvedTrash.isEmpty())
                resolvedTrash = QStringLiteral("Trash");
            *trashMbox = resolvedTrash;

            *buffer = QByteArray();
            *step = 3;
            socket->write(QString("A03 SELECT \"%1\"\r\n").arg(*srcMbox).toLatin1());
        } else if (*step == 3 && (respLatin1.contains("A03 OK") || respLatin1.contains("A03 NO") || respLatin1.contains("A03 BAD"))) {
            if (!respLatin1.contains("A03 OK")) {
                *step = 7;
                socket->write("A07 LOGOUT\r\n");
                return;
            }
            static const QRegularExpression existsRx(QStringLiteral("\\*\\s+(\\d+)\\s+EXISTS"), QRegularExpression::CaseInsensitiveOption);
            QRegularExpressionMatch em = existsRx.match(respLatin1);
            if (em.hasMatch()) {
                const int existsCount = em.captured(1).toInt();
                if (existsCount <= 0) {
                    *step = 7;
                    socket->write("A07 LOGOUT\r\n");
                    return;
                }
            }
            *effectiveSeq = seqNum;
            *buffer = QByteArray();
            if (!permanentDelete && sourceFolderId != "trash" && sourceFolderId != "spam") {
                *step = 4;
                socket->write(QString("A04 UID COPY %1 \"%2\"\r\n").arg(*effectiveSeq).arg(*trashMbox).toLatin1());
            } else {
                *step = 5;
                socket->write(QString("A05 UID STORE %1 +FLAGS.SILENT (\\Deleted)\r\n").arg(*effectiveSeq).toLatin1());
            }
        } else if (*step == 4 && (respLatin1.contains("A04 OK") || respLatin1.contains("A04 NO") || respLatin1.contains("A04 BAD"))) {
            *buffer = QByteArray();
            *step = 5;
            socket->write(QString("A05 UID STORE %1 +FLAGS.SILENT (\\Deleted)\r\n").arg(*effectiveSeq).toLatin1());
        } else if (*step == 5 && (respLatin1.contains("A05 OK") || respLatin1.contains("A05 NO") || respLatin1.contains("A05 BAD"))) {
            *buffer = QByteArray();
            *step = 6;
            socket->write("A06 EXPUNGE\r\n");
        } else if (*step == 6 && (respLatin1.contains("A06 OK") || respLatin1.contains("A06 NO") || respLatin1.contains("A06 BAD"))) {
            unregisterLoadedImapUid(account.id, sourceFolderId, *effectiveSeq);
            if (permanentDelete) {
                emit statusMessage(MailI18n::t(QString("[%1] 서버에서 메일 영구 삭제(EXPUNGE) 완료"),
                                               QString("[%1] Permanently deleted message from server (EXPUNGE)"),
                                               QString("[%1] サーバーからメールを完全削除 (EXPUNGE) しました"))
                                       .arg(account.email));
            } else {
                emit statusMessage(MailI18n::t(QString("[%1] 서버 메일을 휴지통('%2')으로 이동 및 원본 정리 완료"),
                                               QString("[%1] Moved message to Trash ('%2') on server"),
                                               QString("[%1] サーバー上のメールをゴミ箱 ('%2') に移動しました"))
                                       .arg(account.email, *trashMbox));
            }
            *step = 7;
            socket->write("A07 LOGOUT\r\n");
        }
    });

    bindSocketLifecycleCleanup(
        socket,
        nullptr,
        [step, effectiveSeq, buffer, srcMbox, trashMbox]() {
            delete step;
            delete effectiveSeq;
            delete buffer;
            delete srcMbox;
            delete trashMbox;
        });

    socket->connectToHostEncrypted(account.imapHost, account.imapPort);
}

void GoogleProvider::createImapCustomFolder(const AccountItem &account, const QString &folderName)
{
    const QString cleanName = folderName.trimmed();
    if (cleanName.isEmpty() || account.isPop3() || account.imapHost.isEmpty()
        || (account.appPassword.isEmpty() && account.oauthAccessToken.isEmpty())) {
        return;
    }

    QSslSocket *socket = new QSslSocket(this);
    int *step = new int(0);
    QByteArray *buffer = new QByteArray();
    QString *targetMbox = new QString();
    bool *retriedInboxPrefix = new bool(false);

    connect(socket, &QSslSocket::readyRead, this,
            [this, socket, account, cleanName, step, buffer, targetMbox, retriedInboxPrefix]() {
        if (!socket->isOpen())
            return;
        buffer->append(socket->readAll());
        if (!buffer->endsWith("\r\n"))
            return;

        const QString respLatin1 = QString::fromLatin1(*buffer);
        if (*step == 0 && respLatin1.contains("* OK")) {
            *buffer = QByteArray();
            *step = 1;
            if (!account.oauthAccessToken.isEmpty() && !account.useSeparateSmtpAuth && account.imapUsername.isEmpty()) {
                QByteArray xoauth = QString("user=%1\x01" "auth=Bearer %2\x01\x01")
                                        .arg(account.email, account.oauthAccessToken)
                                        .toUtf8()
                                        .toBase64();
                socket->write("A01 AUTHENTICATE XOAUTH2 " + xoauth + "\r\n");
            } else {
                QString cmd = QString("A01 LOGIN \"%1\" \"%2\"\r\n")
                                  .arg(account.effectiveImapUser(), account.appPassword);
                socket->write(cmd.toUtf8());
            }
        } else if (*step == 1 && (respLatin1.startsWith("+ ") || respLatin1.contains("\r\n+ "))
                   && !respLatin1.contains("A01 NO") && !respLatin1.contains("A01 BAD")) {
            socket->write("\r\n");
        } else if (*step == 1 && (respLatin1.contains("A01 OK") || respLatin1.contains("A01 NO") || respLatin1.contains("A01 BAD"))) {
            if (!respLatin1.contains("A01 OK")) {
                socket->disconnectFromHost();
                return;
            }
            *buffer = QByteArray();
            *step = 2;
            socket->write("A02 LIST \"\" \"*\"\r\n");
        } else if (*step == 2 && (respLatin1.contains("\r\nA02 OK") || respLatin1.startsWith("A02 OK")
                                  || respLatin1.contains("A02 NO") || respLatin1.contains("A02 BAD"))) {
            QList<FolderItem> existingCustom;
            QString existing = resolveImapMailboxFromList(respLatin1, account, cleanName, &existingCustom);
            if (!existing.isEmpty()) {
                emit imapCustomFoldersDiscovered(account.id, existingCustom);
                *buffer = QByteArray();
                *step = 6;
                socket->write("A06 LOGOUT\r\n");
                return;
            }
            *targetMbox = buildImapCustomMailboxCandidate(respLatin1, cleanName);
            *buffer = QByteArray();
            *step = 3;
            socket->write(QString("A03 CREATE \"%1\"\r\n").arg(*targetMbox).toLatin1());
        } else if (*step == 3 && (respLatin1.contains("A03 OK") || respLatin1.contains("A03 NO") || respLatin1.contains("A03 BAD"))) {
            if (!respLatin1.contains("A03 OK") && !(*retriedInboxPrefix)
                && !targetMbox->startsWith(QStringLiteral("INBOX."), Qt::CaseInsensitive)) {
                *retriedInboxPrefix = true;
                *targetMbox = QStringLiteral("INBOX.") + *targetMbox;
                *buffer = QByteArray();
                socket->write(QString("A03 CREATE \"%1\"\r\n").arg(*targetMbox).toLatin1());
                return;
            }
            *buffer = QByteArray();
            *step = 4;
            socket->write(QString("A04 SUBSCRIBE \"%1\"\r\n").arg(*targetMbox).toLatin1());
        } else if (*step == 4 && (respLatin1.contains("A04 OK") || respLatin1.contains("A04 NO") || respLatin1.contains("A04 BAD"))) {
            *buffer = QByteArray();
            *step = 5;
            socket->write("A05 LIST \"\" \"*\"\r\n");
        } else if (*step == 5 && (respLatin1.contains("\r\nA05 OK") || respLatin1.startsWith("A05 OK")
                                  || respLatin1.contains("A05 NO") || respLatin1.contains("A05 BAD"))) {
            QList<FolderItem> discoveredCustom;
            resolveImapMailboxFromList(respLatin1, account, cleanName, &discoveredCustom);
            emit imapCustomFoldersDiscovered(account.id, discoveredCustom);
            emit statusMessage(MailI18n::t(QString("[%1] IMAP 서버에 '%2' 폴더 생성 완료"),
                                           QString("[%1] Created folder '%2' on IMAP server"),
                                           QString("[%1] IMAP サーバーにフォルダ '%2' を作成しました"))
                                   .arg(account.email, cleanName));
            *buffer = QByteArray();
            *step = 6;
            socket->write("A06 LOGOUT\r\n");
        }
    });

    bindSocketLifecycleCleanup(
        socket,
        nullptr,
        [step, buffer, targetMbox, retriedInboxPrefix]() {
            delete step;
            delete buffer;
            delete targetMbox;
            delete retriedInboxPrefix;
        });

    socket->connectToHostEncrypted(account.imapHost, account.imapPort);
}

void GoogleProvider::renameImapCustomFolder(const AccountItem &account,
                                            const QString &oldFolderIdOrName,
                                            const QString &newFolderName)
{
    const QString cleanNew = newFolderName.trimmed();
    if (cleanNew.isEmpty() || oldFolderIdOrName.trimmed().isEmpty() || account.isPop3() || account.imapHost.isEmpty()
        || (account.appPassword.isEmpty() && account.oauthAccessToken.isEmpty())) {
        return;
    }

    QSslSocket *socket = new QSslSocket(this);
    int *step = new int(0);
    QByteArray *buffer = new QByteArray();
    QString *newMbox = new QString();

    connect(socket, &QSslSocket::readyRead, this,
            [this, socket, account, oldFolderIdOrName, cleanNew, step, buffer, newMbox]() {
        if (!socket->isOpen())
            return;
        buffer->append(socket->readAll());
        if (!buffer->endsWith("\r\n"))
            return;

        const QString respLatin1 = QString::fromLatin1(*buffer);
        if (*step == 0 && respLatin1.contains("* OK")) {
            *buffer = QByteArray();
            *step = 1;
            if (!account.oauthAccessToken.isEmpty() && !account.useSeparateSmtpAuth && account.imapUsername.isEmpty()) {
                QByteArray xoauth = QString("user=%1\x01" "auth=Bearer %2\x01\x01")
                                        .arg(account.email, account.oauthAccessToken)
                                        .toUtf8()
                                        .toBase64();
                socket->write("A01 AUTHENTICATE XOAUTH2 " + xoauth + "\r\n");
            } else {
                QString cmd = QString("A01 LOGIN \"%1\" \"%2\"\r\n")
                                  .arg(account.effectiveImapUser(), account.appPassword);
                socket->write(cmd.toUtf8());
            }
        } else if (*step == 1 && (respLatin1.startsWith("+ ") || respLatin1.contains("\r\n+ "))
                   && !respLatin1.contains("A01 NO") && !respLatin1.contains("A01 BAD")) {
            socket->write("\r\n");
        } else if (*step == 1 && (respLatin1.contains("A01 OK") || respLatin1.contains("A01 NO") || respLatin1.contains("A01 BAD"))) {
            if (!respLatin1.contains("A01 OK")) {
                socket->disconnectFromHost();
                return;
            }
            *buffer = QByteArray();
            *step = 2;
            socket->write("A02 LIST \"\" \"*\"\r\n");
        } else if (*step == 2 && (respLatin1.contains("\r\nA02 OK") || respLatin1.startsWith("A02 OK")
                                  || respLatin1.contains("A02 NO") || respLatin1.contains("A02 BAD"))) {
            QString oldMbox = resolveImapMailboxFromList(respLatin1, account, oldFolderIdOrName);
            if (oldMbox.isEmpty()) {
                *buffer = QByteArray();
                *step = 6;
                socket->write("A06 LOGOUT\r\n");
                return;
            }
            const QString encNew = encodeImapModifiedUtf7(cleanNew);
            if (oldMbox.startsWith(QStringLiteral("INBOX."), Qt::CaseInsensitive))
                *newMbox = QStringLiteral("INBOX.") + encNew;
            else if (oldMbox.startsWith(QStringLiteral("INBOX/"), Qt::CaseInsensitive))
                *newMbox = QStringLiteral("INBOX/") + encNew;
            else
                *newMbox = encNew;

            *buffer = QByteArray();
            *step = 3;
            socket->write(QString("A03 RENAME \"%1\" \"%2\"\r\n").arg(oldMbox, *newMbox).toLatin1());
        } else if (*step == 3 && (respLatin1.contains("A03 OK") || respLatin1.contains("A03 NO") || respLatin1.contains("A03 BAD"))) {
            *buffer = QByteArray();
            *step = 4;
            socket->write(QString("A04 SUBSCRIBE \"%1\"\r\n").arg(*newMbox).toLatin1());
        } else if (*step == 4 && (respLatin1.contains("A04 OK") || respLatin1.contains("A04 NO") || respLatin1.contains("A04 BAD"))) {
            *buffer = QByteArray();
            *step = 5;
            socket->write("A05 LIST \"\" \"*\"\r\n");
        } else if (*step == 5 && (respLatin1.contains("\r\nA05 OK") || respLatin1.startsWith("A05 OK")
                                  || respLatin1.contains("A05 NO") || respLatin1.contains("A05 BAD"))) {
            QList<FolderItem> discoveredCustom;
            resolveImapMailboxFromList(respLatin1, account, cleanNew, &discoveredCustom);
            emit imapCustomFoldersDiscovered(account.id, discoveredCustom);
            emit statusMessage(MailI18n::t(QString("[%1] IMAP 서버 폴더 이름 변경 완료 ('%2')"),
                                           QString("[%1] Renamed folder on IMAP server ('%2')"),
                                           QString("[%1] IMAP サーバーのフォルダ名を変更しました ('%2')"))
                                   .arg(account.email, cleanNew));
            *buffer = QByteArray();
            *step = 6;
            socket->write("A06 LOGOUT\r\n");
        }
    });

    bindSocketLifecycleCleanup(
        socket,
        nullptr,
        [step, buffer, newMbox]() {
            delete step;
            delete buffer;
            delete newMbox;
        });

    socket->connectToHostEncrypted(account.imapHost, account.imapPort);
}

void GoogleProvider::deleteImapCustomFolder(const AccountItem &account, const QString &folderIdOrName)
{
    if (folderIdOrName.trimmed().isEmpty() || account.isPop3() || account.imapHost.isEmpty()
        || (account.appPassword.isEmpty() && account.oauthAccessToken.isEmpty())) {
        return;
    }

    QSslSocket *socket = new QSslSocket(this);
    int *step = new int(0);
    QByteArray *buffer = new QByteArray();
    QString *targetMbox = new QString();

    connect(socket, &QSslSocket::readyRead, this,
            [this, socket, account, folderIdOrName, step, buffer, targetMbox]() {
        if (!socket->isOpen())
            return;
        buffer->append(socket->readAll());
        if (!buffer->endsWith("\r\n"))
            return;

        const QString respLatin1 = QString::fromLatin1(*buffer);
        if (*step == 0 && respLatin1.contains("* OK")) {
            *buffer = QByteArray();
            *step = 1;
            if (!account.oauthAccessToken.isEmpty() && !account.useSeparateSmtpAuth && account.imapUsername.isEmpty()) {
                QByteArray xoauth = QString("user=%1\x01" "auth=Bearer %2\x01\x01")
                                        .arg(account.email, account.oauthAccessToken)
                                        .toUtf8()
                                        .toBase64();
                socket->write("A01 AUTHENTICATE XOAUTH2 " + xoauth + "\r\n");
            } else {
                QString cmd = QString("A01 LOGIN \"%1\" \"%2\"\r\n")
                                  .arg(account.effectiveImapUser(), account.appPassword);
                socket->write(cmd.toUtf8());
            }
        } else if (*step == 1 && (respLatin1.startsWith("+ ") || respLatin1.contains("\r\n+ "))
                   && !respLatin1.contains("A01 NO") && !respLatin1.contains("A01 BAD")) {
            socket->write("\r\n");
        } else if (*step == 1 && (respLatin1.contains("A01 OK") || respLatin1.contains("A01 NO") || respLatin1.contains("A01 BAD"))) {
            if (!respLatin1.contains("A01 OK")) {
                socket->disconnectFromHost();
                return;
            }
            *buffer = QByteArray();
            *step = 2;
            socket->write("A02 LIST \"\" \"*\"\r\n");
        } else if (*step == 2 && (respLatin1.contains("\r\nA02 OK") || respLatin1.startsWith("A02 OK")
                                  || respLatin1.contains("A02 NO") || respLatin1.contains("A02 BAD"))) {
            *targetMbox = resolveImapMailboxFromList(respLatin1, account, folderIdOrName);
            if (targetMbox->isEmpty() || targetMbox->compare(QStringLiteral("INBOX"), Qt::CaseInsensitive) == 0) {
                *buffer = QByteArray();
                *step = 9;
                socket->write("A09 LOGOUT\r\n");
                return;
            }
            *buffer = QByteArray();
            *step = 3;
            socket->write(QString("A03 SELECT \"%1\"\r\n").arg(*targetMbox).toLatin1());
        } else if (*step == 3 && (respLatin1.contains("A03 OK") || respLatin1.contains("A03 NO") || respLatin1.contains("A03 BAD"))) {
            int existsCount = 0;
            static QRegularExpression rxExists(R"(\*\s+(\d+)\s+EXISTS)");
            QRegularExpressionMatch m = rxExists.match(respLatin1);
            if (m.hasMatch())
                existsCount = m.captured(1).toInt();
            *buffer = QByteArray();
            if (respLatin1.contains("A03 OK") && existsCount > 0) {
                *step = 4;
                socket->write("A04 COPY 1:* \"INBOX\"\r\n");
            } else {
                *step = 5;
                socket->write("A05 CLOSE\r\n");
            }
        } else if (*step == 4 && (respLatin1.contains("A04 OK") || respLatin1.contains("A04 NO") || respLatin1.contains("A04 BAD"))) {
            *buffer = QByteArray();
            *step = 5;
            socket->write("A05 CLOSE\r\n");
        } else if (*step == 5 && (respLatin1.contains("A05 OK") || respLatin1.contains("A05 NO") || respLatin1.contains("A05 BAD"))) {
            *buffer = QByteArray();
            *step = 6;
            socket->write(QString("A06 UNSUBSCRIBE \"%1\"\r\n").arg(*targetMbox).toLatin1());
        } else if (*step == 6 && (respLatin1.contains("A06 OK") || respLatin1.contains("A06 NO") || respLatin1.contains("A06 BAD"))) {
            *buffer = QByteArray();
            *step = 7;
            socket->write(QString("A07 DELETE \"%1\"\r\n").arg(*targetMbox).toLatin1());
        } else if (*step == 7 && (respLatin1.contains("A07 OK") || respLatin1.contains("A07 NO") || respLatin1.contains("A07 BAD"))) {
            *buffer = QByteArray();
            *step = 8;
            socket->write("A08 LIST \"\" \"*\"\r\n");
        } else if (*step == 8 && (respLatin1.contains("\r\nA08 OK") || respLatin1.startsWith("A08 OK")
                                  || respLatin1.contains("A08 NO") || respLatin1.contains("A08 BAD"))) {
            QList<FolderItem> discoveredCustom;
            resolveImapMailboxFromList(respLatin1, account, QStringLiteral("inbox"), &discoveredCustom);
            emit imapCustomFoldersDiscovered(account.id, discoveredCustom);
            emit statusMessage(MailI18n::t(QString("[%1] IMAP 서버에서 폴더 삭제 완료"),
                                           QString("[%1] Deleted folder from IMAP server"),
                                           QString("[%1] IMAP サーバーからフォルダを削除しました"))
                                   .arg(account.email));
            *buffer = QByteArray();
            *step = 9;
            socket->write("A09 LOGOUT\r\n");
            fetchImapFolder(account, QStringLiteral("inbox"));
        }
    });

    bindSocketLifecycleCleanup(
        socket,
        nullptr,
        [step, buffer, targetMbox]() {
            delete step;
            delete buffer;
            delete targetMbox;
        });

    socket->connectToHostEncrypted(account.imapHost, account.imapPort);
}

void GoogleProvider::moveImapMessagesToFolder(const AccountItem &account,
                                              const QString &sourceFolderId,
                                              const QList<int> &seqNums,
                                              const QString &targetFolderId,
                                              const QString &targetFolderDisplayName,
                                              bool markAsRead,
                                              bool markAsStarred)
{
    if (seqNums.isEmpty() || targetFolderId.trimmed().isEmpty() || sourceFolderId == targetFolderId)
        return;

    // 1. POP3 계정: 로컬 디스크(<저장경로>/pop3/<계정이메일>/<폴더>/) 내 실제 .eml 파일 이동
    if (account.isPop3()) {
        account.ensurePop3MailboxDirs();
        const QString srcDir = account.pop3MailboxDir(sourceFolderId);
        const QString dstDir = account.pop3MailboxDir(targetFolderId);
        QDir().mkpath(dstDir);
        for (int seq : seqNums) {
            if (seq <= 0)
                continue;
            const QString srcFile = srcDir + QString("/msg_%1.eml").arg(seq);
            const QString dstFile = dstDir + QString("/msg_%1.eml").arg(seq);
            if (QFileInfo::exists(srcFile)) {
                QFile::remove(dstFile);
                QFile::rename(srcFile, dstFile);
            }
        }
        return;
    }

    // 2. IMAP 계정: 실제 IMAP 서버에서 대상 폴더 확인(없으면 CREATE 생성) -> COPY -> 원본 +FLAGS(\Deleted) -> EXPUNGE
    if (account.imapHost.isEmpty() || (account.appPassword.isEmpty() && account.oauthAccessToken.isEmpty()))
        return;

    const QString seqSet = buildImapSequenceSet(seqNums);
    if (seqSet.isEmpty())
        return;

    QSslSocket *socket = new QSslSocket(this);
    int *step = new int(0);
    QByteArray *buffer = new QByteArray();
    QString *srcMbox = new QString("INBOX");
    QString *destMbox = new QString();
    bool *retriedInboxPrefix = new bool(false);

    connect(socket, &QSslSocket::readyRead, this,
            [this, socket, account, sourceFolderId, seqNums, targetFolderId, targetFolderDisplayName,
             seqSet, markAsRead, markAsStarred, step, buffer, srcMbox, destMbox, retriedInboxPrefix]() {
        if (!socket->isOpen())
            return;
        buffer->append(socket->readAll());
        if (!buffer->endsWith("\r\n"))
            return;

        const QString respLatin1 = QString::fromLatin1(*buffer);

        auto selectSourceAndCopy = [&]() {
            *buffer = QByteArray();
            *step = 5;
            socket->write(QString("A05 SELECT \"%1\"\r\n").arg(*srcMbox).toLatin1());
        };

        if (*step == 0 && respLatin1.contains("* OK")) {
            *buffer = QByteArray();
            *step = 1;
            if (!account.oauthAccessToken.isEmpty() && !account.useSeparateSmtpAuth && account.imapUsername.isEmpty()) {
                QByteArray xoauth = QString("user=%1\x01" "auth=Bearer %2\x01\x01")
                                        .arg(account.email, account.oauthAccessToken)
                                        .toUtf8()
                                        .toBase64();
                socket->write("A01 AUTHENTICATE XOAUTH2 " + xoauth + "\r\n");
            } else {
                QString cmd = QString("A01 LOGIN \"%1\" \"%2\"\r\n")
                                  .arg(account.effectiveImapUser(), account.appPassword);
                socket->write(cmd.toUtf8());
            }
        } else if (*step == 1 && (respLatin1.startsWith("+ ") || respLatin1.contains("\r\n+ "))
                   && !respLatin1.contains("A01 NO") && !respLatin1.contains("A01 BAD")) {
            socket->write("\r\n");
        } else if (*step == 1 && (respLatin1.contains("A01 OK") || respLatin1.contains("A01 NO") || respLatin1.contains("A01 BAD"))) {
            if (!respLatin1.contains("A01 OK")) {
                if (!account.oauthRefreshToken.isEmpty()) {
                    refreshGoogleAccessToken(account.email, account.oauthRefreshToken);
                }
                socket->disconnectFromHost();
                return;
            }
            *buffer = QByteArray();
            *step = 2;
            socket->write("A02 LIST \"\" \"*\"\r\n");
        } else if (*step == 2 && (respLatin1.contains("\r\nA02 OK") || respLatin1.startsWith("A02 OK")
                                  || respLatin1.contains("A02 NO") || respLatin1.contains("A02 BAD"))) {
            QList<FolderItem> discoveredCustom;
            QString resolvedSrc = resolveImapMailboxFromList(respLatin1, account, sourceFolderId, &discoveredCustom);
            if (resolvedSrc.isEmpty()) {
                if (sourceFolderId == QLatin1String("trash"))
                    resolvedSrc = QStringLiteral("Trash");
                else if (sourceFolderId == QLatin1String("spam"))
                    resolvedSrc = QStringLiteral("Spam");
                else if (sourceFolderId == QLatin1String("sent"))
                    resolvedSrc = QStringLiteral("Sent");
                else if (sourceFolderId == QLatin1String("drafts"))
                    resolvedSrc = QStringLiteral("Drafts");
                else
                    resolvedSrc = QStringLiteral("INBOX");
            }
            *srcMbox = resolvedSrc;

            QString resolvedDest = resolveImapMailboxFromList(respLatin1, account, targetFolderId);
            if (resolvedDest.isEmpty() && !targetFolderDisplayName.trimmed().isEmpty()) {
                resolvedDest = resolveImapMailboxFromList(respLatin1, account, targetFolderDisplayName.trimmed());
            }

            if (!resolvedDest.isEmpty()) {
                *destMbox = resolvedDest;
                emit imapCustomFoldersDiscovered(account.id, discoveredCustom);
                selectSourceAndCopy();
                return;
            }

            // 대상 폴더가 아직 IMAP 서버에 없으면 즉시 서버에 CREATE 명령으로 실제 폴더 생성
            QString desiredName = targetFolderDisplayName.trimmed();
            if (desiredName.isEmpty() || desiredName == targetFolderId) {
                const QString customPrefix = QString("custom_%1_").arg(account.id);
                desiredName = targetFolderId.startsWith(customPrefix)
                                  ? targetFolderId.mid(customPrefix.size())
                                  : targetFolderId;
            }
            *destMbox = buildImapCustomMailboxCandidate(respLatin1, desiredName);
            if (destMbox->isEmpty()) {
                *buffer = QByteArray();
                *step = 11;
                socket->write("A11 LOGOUT\r\n");
                return;
            }
            *buffer = QByteArray();
            *step = 3;
            socket->write(QString("A03 CREATE \"%1\"\r\n").arg(*destMbox).toLatin1());
        } else if (*step == 3 && (respLatin1.contains("A03 OK") || respLatin1.contains("A03 NO") || respLatin1.contains("A03 BAD"))) {
            if (!respLatin1.contains("A03 OK") && !(*retriedInboxPrefix)
                && !destMbox->startsWith(QStringLiteral("INBOX."), Qt::CaseInsensitive)) {
                *retriedInboxPrefix = true;
                *destMbox = QStringLiteral("INBOX.") + *destMbox;
                *buffer = QByteArray();
                socket->write(QString("A03 CREATE \"%1\"\r\n").arg(*destMbox).toLatin1());
                return;
            }
            *buffer = QByteArray();
            *step = 4;
            socket->write(QString("A04 SUBSCRIBE \"%1\"\r\n").arg(*destMbox).toLatin1());
        } else if (*step == 4 && (respLatin1.contains("A04 OK") || respLatin1.contains("A04 NO") || respLatin1.contains("A04 BAD"))) {
            selectSourceAndCopy();
        } else if (*step == 5 && (respLatin1.contains("A05 OK") || respLatin1.contains("A05 NO") || respLatin1.contains("A05 BAD"))) {
            if (!respLatin1.contains("A05 OK") || srcMbox->compare(*destMbox, Qt::CaseInsensitive) == 0) {
                *buffer = QByteArray();
                *step = 11;
                socket->write("A11 LOGOUT\r\n");
                return;
            }
            *buffer = QByteArray();
            QStringList flagsToAdd;
            if (markAsRead)
                flagsToAdd.append(QStringLiteral("\\Seen"));
            if (markAsStarred)
                flagsToAdd.append(QStringLiteral("\\Flagged"));
            if (!flagsToAdd.isEmpty()) {
                *step = 6;
                socket->write(QString("A06 UID STORE %1 +FLAGS.SILENT (%2)\r\n")
                                  .arg(seqSet, flagsToAdd.join(QLatin1Char(' '))).toLatin1());
            } else {
                *step = 7;
                socket->write(QString("A07 UID COPY %1 \"%2\"\r\n").arg(seqSet, *destMbox).toLatin1());
            }
        } else if (*step == 6 && (respLatin1.contains("A06 OK") || respLatin1.contains("A06 NO") || respLatin1.contains("A06 BAD"))) {
            *buffer = QByteArray();
            *step = 7;
            socket->write(QString("A07 UID COPY %1 \"%2\"\r\n").arg(seqSet, *destMbox).toLatin1());
        } else if (*step == 7 && (respLatin1.contains("A07 OK") || respLatin1.contains("A07 NO") || respLatin1.contains("A07 BAD"))) {
            if (!respLatin1.contains("A07 OK")) {
                // COPY가 실패한 경우 원본 삭제를 수행하지 않고 안전하게 중단
                *buffer = QByteArray();
                *step = 11;
                socket->write("A11 LOGOUT\r\n");
                return;
            }
            *buffer = QByteArray();
            *step = 8;
            socket->write(QString("A08 UID STORE %1 +FLAGS.SILENT (\\Deleted)\r\n").arg(seqSet).toLatin1());
        } else if (*step == 8 && (respLatin1.contains("A08 OK") || respLatin1.contains("A08 NO") || respLatin1.contains("A08 BAD"))) {
            *buffer = QByteArray();
            *step = 9;
            socket->write("A09 EXPUNGE\r\n");
        } else if (*step == 9 && (respLatin1.contains("A09 OK") || respLatin1.contains("A09 NO") || respLatin1.contains("A09 BAD"))) {
            for (int uidVal : seqNums) {
                if (uidVal > 0)
                    unregisterLoadedImapUid(account.id, sourceFolderId, uidVal);
            }
            *buffer = QByteArray();
            *step = 10;
            socket->write("A10 LIST \"\" \"*\"\r\n");
        } else if (*step == 10 && (respLatin1.contains("\r\nA10 OK") || respLatin1.startsWith("A10 OK")
                                   || respLatin1.contains("A10 NO") || respLatin1.contains("A10 BAD"))) {
            QList<FolderItem> discoveredCustom;
            resolveImapMailboxFromList(respLatin1, account, *destMbox, &discoveredCustom);
            emit imapCustomFoldersDiscovered(account.id, discoveredCustom);
            if (!targetFolderId.isEmpty()
                && targetFolderId != QStringLiteral("trash")
                && targetFolderId != QStringLiteral("spam")) {
                clearLoadedImapUids(account.id, targetFolderId);
                fetchImapFolder(account, targetFolderId, 200, 0);
            }
            emit statusMessage(MailI18n::t(QString("[%1] IMAP 서버에서 '%2' 폴더로 메일 이동 및 원본 정리(EXPUNGE) 완료"),
                                           QString("[%1] Moved messages to '%2' on IMAP server (EXPUNGE)"),
                                           QString("[%1] IMAP サーバーで '%2' にメールを移動しました (EXPUNGE)"))
                                   .arg(account.email, decodeImapModifiedUtf7(*destMbox)));
            *buffer = QByteArray();
            *step = 11;
            socket->write("A11 LOGOUT\r\n");
        }
    });

    bindSocketLifecycleCleanup(
        socket,
        nullptr,
        [step, buffer, srcMbox, destMbox, retriedInboxPrefix]() {
            delete step;
            delete buffer;
            delete srcMbox;
            delete destMbox;
            delete retriedInboxPrefix;
        });

    socket->connectToHostEncrypted(account.imapHost, account.imapPort);
}

namespace {

static constexpr qint64 kMaxMimeAttachmentTotalBytes = 20LL * 1024LL * 1024LL; // 20 MB 제한

QByteArray wrapBase64Lines(const QByteArray &rawBytes)
{
    const QByteArray b64 = rawBytes.toBase64();
    QByteArray out;
    out.reserve(b64.size() + (b64.size() / 76) * 2 + 4);
    for (int i = 0; i < b64.size(); i += 76) {
        out.append(b64.mid(i, 76));
        out.append("\r\n");
    }
    return out;
}

QByteArray buildRfc5322MimeMessage(const AccountItem &account,
                                   const QString &toEmail,
                                   const QString &ccEmail,
                                   const QString &subject,
                                   const QString &htmlBody,
                                   const QString &bccEmailForArchive = QString(),
                                   const QList<AttachmentItem> &attachments = QList<AttachmentItem>(),
                                   bool isHighImportance = false)
{
    const QString dispName = account.displayName.trimmed().isEmpty()
                                 ? account.email.section('@', 0, 0)
                                 : account.displayName.trimmed();
    const QString encodedFromName = QString("=?UTF-8?B?%1?=")
                                        .arg(QString::fromLatin1(dispName.toUtf8().toBase64()));
    const QString encodedSubject = QString("=?UTF-8?B?%1?=")
                                       .arg(QString::fromLatin1(subject.toUtf8().toBase64()));
    const QString rfcDate = QLocale::c().toString(QDateTime::currentDateTimeUtc(),
                                                  "ddd, dd MMM yyyy HH:mm:ss") + " +0000";
    const QString domainPart = account.email.contains('@')
                                   ? account.email.section('@', 1)
                                   : QStringLiteral("sollunamail.local");
    const QString msgId = QString("<%1.%2@%3>")
                              .arg(QDateTime::currentMSecsSinceEpoch())
                              .arg(qHash(toEmail + subject + rfcDate) & 0x7fffffff)
                              .arg(domainPart);

    // 실제 디스크에 존재하고 총 합산 20MB 이하인 첨부파일 목록 필터링
    QList<AttachmentItem> validAttachments;
    qint64 accBytes = 0;
    for (const AttachmentItem &att : attachments) {
        if (att.filePath.trimmed().isEmpty())
            continue;
        QFileInfo fi(att.filePath);
        if (!fi.exists() || !fi.isFile())
            continue;
        if (accBytes + fi.size() > kMaxMimeAttachmentTotalBytes)
            continue;
        accBytes += fi.size();
        validAttachments.append(att);
    }

    QString headers;
    headers += QString("From: %1 <%2>\r\n").arg(encodedFromName, account.email);
    headers += QString("To: %1\r\n").arg(toEmail.trimmed());
    if (!ccEmail.trimmed().isEmpty()) {
        headers += QString("Cc: %1\r\n").arg(ccEmail.trimmed());
    }
    if (!bccEmailForArchive.trimmed().isEmpty()) {
        headers += QString("Bcc: %1\r\n").arg(bccEmailForArchive.trimmed());
    }
    headers += QString("Subject: %1\r\n").arg(encodedSubject);
    headers += QString("Date: %1\r\n").arg(rfcDate);
    headers += QString("Message-ID: %1\r\n").arg(msgId);
    if (isHighImportance) {
        headers += QStringLiteral("X-Priority: 1 (Highest)\r\n");
        headers += QStringLiteral("Importance: High\r\n");
        headers += QStringLiteral("X-MSMail-Priority: High\r\n");
        headers += QStringLiteral("Priority: urgent\r\n");
    }
    if (MailI18n::privacyTrackSentEmails()) {
        headers += QString("Disposition-Notification-To: %1 <%2>\r\n").arg(encodedFromName, account.email);
        headers += QString("Return-Receipt-To: %1 <%2>\r\n").arg(encodedFromName, account.email);
        headers += QString("X-Confirm-Reading-To: %1 <%2>\r\n").arg(encodedFromName, account.email);
    }
    headers += QStringLiteral("MIME-Version: 1.0\r\n");

    QString normalizedBody = htmlBody;
    normalizedBody.replace("\r\n", "\n");
    normalizedBody.replace('\n', "\r\n");

    // HTML 본문 및 서명 내 인라인 Base64 이미지(<img src="data:image/...;base64,...">)를
    // 표준 RFC 2387 multipart/related + Content-ID (cid:) 파트로 자동 변환하여
    // Gmail / Outlook / 네이버 / 다음 등 모든 웹메일에서 차단 없이 표시되도록 처리
    struct InlineCidImagePart {
        QString cid;
        QString mimeType;
        QString fileName;
        QByteArray rawBytes;
    };
    QList<InlineCidImagePart> inlineCidImages;
    {
        static const QRegularExpression rxDataImg(
            R"rx((<img\b[^>]*?\bsrc\s*=\s*["'])data:(image/[a-zA-Z0-9.+-]+);base64,([A-Za-z0-9+/=\r\n\s]+)(["']))rx",
            QRegularExpression::CaseInsensitiveOption);
        QString transformed;
        int lastPos = 0;
        int imgSeq = 0;
        QRegularExpressionMatchIterator it = rxDataImg.globalMatch(normalizedBody);
        while (it.hasNext()) {
            QRegularExpressionMatch m = it.next();
            transformed.append(normalizedBody.mid(lastPos, m.capturedStart() - lastPos));
            const QString prefix = m.captured(1);
            const QString imgMime = m.captured(2).trimmed().toLower();
            QByteArray b64Clean = m.captured(3).toLatin1();
            b64Clean.replace("\r", "").replace("\n", "").replace(" ", "").replace("\t", "");
            const QByteArray imgBytes = QByteArray::fromBase64(b64Clean);
            const QString suffix = m.captured(4);

            if (!imgBytes.isEmpty()) {
                ++imgSeq;
                QString ext = QStringLiteral("png");
                if (imgMime.contains("jpeg") || imgMime.contains("jpg"))
                    ext = QStringLiteral("jpg");
                else if (imgMime.contains("gif"))
                    ext = QStringLiteral("gif");
                else if (imgMime.contains("webp"))
                    ext = QStringLiteral("webp");
                else if (imgMime.contains("bmp"))
                    ext = QStringLiteral("bmp");
                else if (imgMime.contains("svg"))
                    ext = QStringLiteral("svg");

                const QString cid = QString("sig_img_%1_%2@%3")
                                        .arg(QDateTime::currentMSecsSinceEpoch())
                                        .arg(imgSeq)
                                        .arg(domainPart);
                InlineCidImagePart part;
                part.cid = cid;
                part.mimeType = imgMime;
                part.fileName = QString("inline_image_%1.%2").arg(imgSeq).arg(ext);
                part.rawBytes = imgBytes;
                inlineCidImages.append(part);

                transformed.append(prefix + QStringLiteral("cid:") + cid + suffix);
            } else {
                transformed.append(m.captured(0));
            }
            lastPos = m.capturedEnd();
        }
        if (imgSeq > 0) {
            transformed.append(normalizedBody.mid(lastPos));
            normalizedBody = transformed;
        }
    }

    auto appendInlineCidParts = [&](QByteArray &outBuf, const QString &relBoundary) {
        outBuf.append(QString("--%1\r\n").arg(relBoundary).toUtf8());
        outBuf.append("Content-Type: text/html; charset=UTF-8\r\n");
        outBuf.append("Content-Transfer-Encoding: base64\r\n\r\n");
        outBuf.append(wrapBase64Lines(normalizedBody.toUtf8()));
        outBuf.append("\r\n");

        for (const InlineCidImagePart &imgPart : inlineCidImages) {
            outBuf.append(QString("--%1\r\n").arg(relBoundary).toUtf8());
            outBuf.append(QString("Content-Type: %1; name=\"%2\"\r\n").arg(imgPart.mimeType, imgPart.fileName).toUtf8());
            outBuf.append(QString("Content-ID: <%1>\r\n").arg(imgPart.cid).toUtf8());
            outBuf.append(QString("Content-Disposition: inline; filename=\"%1\"\r\n").arg(imgPart.fileName).toUtf8());
            outBuf.append("Content-Transfer-Encoding: base64\r\n\r\n");
            outBuf.append(wrapBase64Lines(imgPart.rawBytes));
            outBuf.append("\r\n");
        }
        outBuf.append(QString("--%1--\r\n").arg(relBoundary).toUtf8());
    };

    if (validAttachments.isEmpty() && inlineCidImages.isEmpty()) {
        headers += QStringLiteral("Content-Type: text/html; charset=UTF-8\r\n");
        headers += QStringLiteral("Content-Transfer-Encoding: 8bit\r\n\r\n");
        return (headers + normalizedBody + "\r\n").toUtf8();
    }

    if (validAttachments.isEmpty() && !inlineCidImages.isEmpty()) {
        const QString relBoundary = QString("====SollunaMail_Rel_%1_%2====")
                                        .arg(QDateTime::currentMSecsSinceEpoch())
                                        .arg(QUuid::createUuid().toString(QUuid::Id128).left(8));
        headers += QString("Content-Type: multipart/related; type=\"text/html\"; boundary=\"%1\"\r\n\r\n").arg(relBoundary);
        QByteArray mimeOut = headers.toUtf8();
        appendInlineCidParts(mimeOut, relBoundary);
        return mimeOut;
    }

    const QString boundary = QString("====SollunaMail_Part_%1_%2====")
                                 .arg(QDateTime::currentMSecsSinceEpoch())
                                 .arg(QUuid::createUuid().toString(QUuid::Id128).left(8));
    headers += QString("Content-Type: multipart/mixed; boundary=\"%1\"\r\n\r\n").arg(boundary);

    QByteArray mimeOut = headers.toUtf8();

    // Part 1: HTML Body (or multipart/related if inline signature/body images exist)
    mimeOut.append(QString("--%1\r\n").arg(boundary).toUtf8());
    if (inlineCidImages.isEmpty()) {
        mimeOut.append("Content-Type: text/html; charset=UTF-8\r\n");
        mimeOut.append("Content-Transfer-Encoding: base64\r\n\r\n");
        mimeOut.append(wrapBase64Lines(normalizedBody.toUtf8()));
        mimeOut.append("\r\n");
    } else {
        const QString relBoundary = QString("====SollunaMail_Rel_%1_%2====")
                                        .arg(QDateTime::currentMSecsSinceEpoch())
                                        .arg(QUuid::createUuid().toString(QUuid::Id128).left(8));
        mimeOut.append(QString("Content-Type: multipart/related; type=\"text/html\"; boundary=\"%1\"\r\n\r\n").arg(relBoundary).toUtf8());
        appendInlineCidParts(mimeOut, relBoundary);
        mimeOut.append("\r\n");
    }

    // Part 2..N: Attachments (<= 20MB total)
    QMimeDatabase mimeDb;
    for (const AttachmentItem &att : validAttachments) {
        QFile f(att.filePath);
        if (!f.open(QIODevice::ReadOnly))
            continue;
        const QByteArray fileData = f.readAll();
        f.close();

        const QString rawFileName = att.fileName.trimmed().isEmpty()
                                        ? QFileInfo(att.filePath).fileName()
                                        : att.fileName.trimmed();
        const QString encodedFileName = QString("=?UTF-8?B?%1?=")
                                            .arg(QString::fromLatin1(rawFileName.toUtf8().toBase64()));
        QString mimeType = mimeDb.mimeTypeForFile(att.filePath).name();
        if (mimeType.trimmed().isEmpty())
            mimeType = QStringLiteral("application/octet-stream");

        mimeOut.append(QString("--%1\r\n").arg(boundary).toUtf8());
        mimeOut.append(QString("Content-Type: %1; name=\"%2\"\r\n").arg(mimeType, encodedFileName).toUtf8());
        mimeOut.append(QString("Content-Disposition: attachment; filename=\"%1\"\r\n").arg(encodedFileName).toUtf8());
        mimeOut.append("Content-Transfer-Encoding: base64\r\n\r\n");
        mimeOut.append(wrapBase64Lines(fileData));
        mimeOut.append("\r\n");
    }

    mimeOut.append(QString("--%1--\r\n").arg(boundary).toUtf8());
    return mimeOut;
}

QStringList extractAllEmailAddresses(const QString &toEmail, const QString &ccEmail, const QString &bccEmail = QString())
{
    QStringList result;
    const QString combined = toEmail + "," + ccEmail + "," + bccEmail;
    const QStringList parts = combined.split(QRegularExpression("[,;]"), Qt::SkipEmptyParts);
    for (const QString &rawPart : parts) {
        QString addr = rawPart.trimmed();
        if (addr.contains('<') && addr.contains('>')) {
            addr = addr.section('<', 1, 1).section('>', 0, 0).trimmed();
        }
        if (addr.contains('@') && !result.contains(addr, Qt::CaseInsensitive)) {
            result.append(addr);
        }
    }
    if (result.isEmpty() && !toEmail.trimmed().isEmpty()) {
        result.append(toEmail.trimmed());
    }
    return result;
}

} // namespace

void GoogleProvider::appendMessageToImapFolder(const AccountItem &account,
                                               const QString &folderId,
                                               const QString &toEmail,
                                               const QString &ccEmail,
                                               const QString &subject,
                                               const QString &htmlBody,
                                               const QString &bccEmail,
                                               const QList<AttachmentItem> &attachments,
                                               bool isHighImportance)
{
    if (account.isPop3()) {
        // POP3 계정은 보낸 메일(sent) / 임시보관(drafts) 원문(.eml)을 <실행파일경로>/pop3/<계정폴더>/<folderId>/ 에 직접 저장
        account.ensurePop3MailboxDirs();
        const QString targetFid = folderId.isEmpty() ? QStringLiteral("sent") : folderId;
        const QByteArray rawMime = buildRfc5322MimeMessage(account, toEmail, ccEmail, subject, htmlBody, bccEmail, attachments, isHighImportance);
        const QString boxDir = account.pop3MailboxDir(targetFid);
        QDir().mkpath(boxDir);
        QFile emlFile(boxDir + QString("/msg_%1.eml").arg(QDateTime::currentMSecsSinceEpoch()));
        if (emlFile.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            emlFile.write(rawMime);
            emlFile.close();
        }
        fetchImapFolder(account, targetFid, 50, 0);
        return;
    }
    if (account.imapHost.isEmpty())
        return;
    if (account.appPassword.isEmpty() && account.oauthAccessToken.isEmpty())
        return;

    const QByteArray rawMime = buildRfc5322MimeMessage(account, toEmail, ccEmail, subject, htmlBody, bccEmail, attachments, isHighImportance);

    QSslSocket *socket = new QSslSocket(this);
    int *step = new int(0);
    QByteArray *buffer = new QByteArray();
    QString *targetMbox = new QString();

    connect(socket, &QSslSocket::readyRead, this,
            [this, socket, account, folderId, rawMime, step, buffer, targetMbox]() {
        if (!socket->isOpen())
            return;
        buffer->append(socket->readAll());
        QString respLatin1 = QString::fromLatin1(*buffer);

        if (*step == 0 && (respLatin1.startsWith("* OK") || respLatin1.contains("\r\n* OK"))) {
            *step = 1;
            buffer->clear();
            const QString imapUser = account.effectiveImapUser();
            if (!account.oauthAccessToken.isEmpty() && account.imapUsername.trimmed().isEmpty()) {
                QByteArray xoauth = QString("user=%1\x01" "auth=Bearer %2\x01\x01")
                                        .arg(imapUser, account.oauthAccessToken)
                                        .toUtf8()
                                        .toBase64();
                socket->write("A01 AUTHENTICATE XOAUTH2 " + xoauth + "\r\n");
            } else {
                QString escUser = imapUser;
                escUser.replace("\\", "\\\\").replace("\"", "\\\"");
                QString escPass = account.appPassword;
                escPass.replace("\\", "\\\\").replace("\"", "\\\"");
                socket->write(QString("A01 LOGIN \"%1\" \"%2\"\r\n").arg(escUser, escPass).toUtf8());
            }
        } else if (*step == 1 && (respLatin1.startsWith("+ ") || respLatin1.contains("\r\n+ "))) {
            buffer->clear();
            socket->write("\r\n");
        } else if (*step == 1 && (respLatin1.contains("A01 OK") || respLatin1.contains("A01 NO") || respLatin1.contains("A01 BAD"))) {
            if (!respLatin1.contains("A01 OK")) {
                if (!account.oauthRefreshToken.isEmpty()) {
                    refreshGoogleAccessToken(account.email, account.oauthRefreshToken);
                }
                socket->disconnectFromHost();
                return;
            }
            *step = 2;
            buffer->clear();
            socket->write("A02 LIST \"\" \"*\"\r\n");
        } else if (*step == 2 && (respLatin1.contains("A02 OK") || respLatin1.contains("A02 NO") || respLatin1.contains("A02 BAD"))) {
            QString resolved = resolveImapMailboxFromList(respLatin1, account, folderId, nullptr);
            if (resolved.isEmpty()) {
                resolved = (folderId == "drafts") ? QStringLiteral("Drafts") : QStringLiteral("Sent");
                *targetMbox = resolved;
                *step = 25;
                buffer->clear();
                QString escMbox = *targetMbox;
                escMbox.replace("\\", "\\\\").replace("\"", "\\\"");
                socket->write(QString("A02C CREATE \"%1\"\r\n").arg(escMbox).toLatin1());
                return;
            }
            *targetMbox = resolved;
            *step = 3;
            buffer->clear();
            QString escMbox = *targetMbox;
            escMbox.replace("\\", "\\\\").replace("\"", "\\\"");
            QString flags = (folderId == "drafts") ? QStringLiteral("(\\Seen \\Draft)") : QStringLiteral("(\\Seen)");
            QString cmd = QString("A03 APPEND \"%1\" %2 {%3}\r\n")
                              .arg(escMbox, flags)
                              .arg(rawMime.size());
            socket->write(cmd.toLatin1());
        } else if (*step == 25 && (respLatin1.contains("A02C OK") || respLatin1.contains("A02C NO") || respLatin1.contains("A02C BAD"))) {
            *step = 3;
            buffer->clear();
            QString escMbox = *targetMbox;
            escMbox.replace("\\", "\\\\").replace("\"", "\\\"");
            QString flags = (folderId == "drafts") ? QStringLiteral("(\\Seen \\Draft)") : QStringLiteral("(\\Seen)");
            QString cmd = QString("A03 APPEND \"%1\" %2 {%3}\r\n")
                              .arg(escMbox, flags)
                              .arg(rawMime.size());
            socket->write(cmd.toLatin1());
        } else if (*step == 3) {
            if (respLatin1.startsWith("+") || respLatin1.contains("\r\n+")) {
                *step = 4;
                buffer->clear();
                socket->write(rawMime);
                socket->write("\r\n");
            } else if (respLatin1.contains("A03 NO") || respLatin1.contains("A03 BAD")) {
                *step = 5;
                buffer->clear();
                socket->write("A04 LOGOUT\r\n");
                socket->disconnectFromHost();
            }
        } else if (*step == 4 && (respLatin1.contains("A03 OK") || respLatin1.contains("A03 NO") || respLatin1.contains("A03 BAD"))) {
            const bool ok = respLatin1.contains("A03 OK");
            *step = 5;
            buffer->clear();
            socket->write("A04 LOGOUT\r\n");
            if (ok) {
                emit statusMessage(MailI18n::t(QString("[%1] IMAP 서버 '%2' 폴더에 메일 동기화 저장(APPEND) 완료"),
                                               QString("[%1] Synced message to IMAP folder '%2' (APPEND)"),
                                               QString("[%1] IMAP フォルダ '%2' にメールを保存 (APPEND) しました"))
                                       .arg(account.email, *targetMbox));
                fetchImapFolder(account, folderId, 50, 0);
            }
            socket->disconnectFromHost();
        }
    });

    bindSocketLifecycleCleanup(
        socket,
        nullptr,
        [step, buffer, targetMbox]() {
            delete step;
            delete buffer;
            delete targetMbox;
        });

    socket->connectToHostEncrypted(account.imapHost, account.imapPort);
}

void GoogleProvider::sendSmtpEmail(const AccountItem &account,
                                   const QString &toEmail,
                                   const QString &subject,
                                   const QString &htmlBody,
                                   const QString &ccEmail,
                                   const QString &bccEmail,
                                   const QList<AttachmentItem> &attachments,
                                   bool isHighImportance)
{
    const QString smtpUser = account.effectiveSmtpUser();
    const QString smtpPass = account.effectiveSmtpPassword();

    if (account.smtpHost.isEmpty() || (smtpPass.isEmpty() && account.oauthAccessToken.isEmpty())) {
        emit smtpSendFinished(
            true,
            MailI18n::t("로컬 보낸편지함(Sent)에 저장되었습니다. (실제 외부 SMTP 발송은 Settings > Accounts에서 비밀번호 또는 Mailtrap/Mailgun 계정을 설정하면 동작합니다.)",
                        "Saved to local Sent folder. (Configure SMTP password or OAuth in Settings > Accounts for external delivery.)",
                        "ローカルの送信済みトレイに保存しました。（外部SMTP送信は設定 > アカウントでパスワードまたはOAuthを設定してください）"));
        return;
    }

    const QStringList allRecipients = extractAllEmailAddresses(toEmail, ccEmail, bccEmail);
    // 외부 발송용 MIME에는 Bcc 헤더를 포함하지 않음(RFC 5321/5322 표준: 숨은 참조 보호) + 첨부파일(<=20MB) multipart/mixed 인코딩 포함
    const QByteArray rawMime = buildRfc5322MimeMessage(account, toEmail, ccEmail, subject, htmlBody, QString(), attachments, isHighImportance);

    QSslSocket *socket = new QSslSocket(this);
    int *stage = new int(0);
    int *rcptIdx = new int(0);
    const bool useStartTls = (account.smtpSecurity.compare("STARTTLS", Qt::CaseInsensitive) == 0)
                             || (account.smtpPort == 587 || account.smtpPort == 2525);
    const bool useImplicitSsl = !useStartTls && (account.smtpSecurity.compare("None", Qt::CaseInsensitive) != 0);

    connect(socket, &QSslSocket::encrypted, this, [socket, stage, useStartTls]() {
        if (useStartTls && *stage == 10) {
            *stage = 1;
            socket->write("EHLO sollunamail.local\r\n");
        }
    });

    connect(socket, &QSslSocket::readyRead, this,
            [this, socket, account, smtpUser, smtpPass, toEmail, ccEmail, bccEmail, attachments, isHighImportance, subject, htmlBody, allRecipients, rawMime, stage, rcptIdx, useStartTls]() {
        if (!socket->isOpen())
            return;
        QString resp = QString::fromUtf8(socket->readAll());
        if (*stage == 0 && resp.startsWith("220")) {
            if (useStartTls) {
                *stage = 9;
            } else {
                *stage = 1;
            }
            socket->write("EHLO sollunamail.local\r\n");
        } else if (*stage == 9 && resp.contains("250 ")) {
            *stage = 10;
            socket->write("STARTTLS\r\n");
        } else if (*stage == 10 && resp.startsWith("220")) {
            socket->startClientEncryption();
        } else if ((*stage == 9 || *stage == 10) && (resp.startsWith("4") || resp.startsWith("5"))) {
            *stage = 11;
            emit smtpSendFinished(
                false,
                MailI18n::t(QString("SMTP STARTTLS 협상 실패 (%1:%2): %3"),
                            QString("SMTP STARTTLS negotiation failed (%1:%2): %3"),
                            QString("SMTP STARTTLS 失敗 (%1:%2): %3"))
                    .arg(account.smtpHost)
                    .arg(account.smtpPort)
                    .arg(resp.trimmed()));
            socket->disconnectFromHost();
        } else if (*stage == 1 && resp.contains("250 ")) {
            if (!account.useSeparateSmtpAuth && !account.oauthAccessToken.isEmpty()) {
                *stage = 20;
                QByteArray xoauth = QString("user=%1\x01" "auth=Bearer %2\x01\x01")
                                        .arg(account.email, account.oauthAccessToken)
                                        .toUtf8()
                                        .toBase64();
                socket->write("AUTH XOAUTH2 " + xoauth + "\r\n");
            } else {
                *stage = 2;
                socket->write("AUTH LOGIN\r\n");
            }
        } else if (*stage == 20 && resp.startsWith("334")) {
            // XOAUTH2 SASL challenge continuation (token expired / error details)
            socket->write("\r\n");
        } else if (*stage == 2 && resp.startsWith("334")) {
            *stage = 3;
            socket->write(smtpUser.toUtf8().toBase64() + "\r\n");
        } else if (*stage == 3 && resp.startsWith("334")) {
            *stage = 4;
            socket->write(smtpPass.toUtf8().toBase64() + "\r\n");
        } else if ((*stage == 20 || *stage == 2 || *stage == 4) && resp.startsWith("235")) {
            *stage = 5;
            socket->write(QString("MAIL FROM:<%1>\r\n").arg(account.email).toUtf8());
        } else if ((*stage == 20 || *stage == 2 || *stage == 3 || *stage == 4)
                   && (resp.startsWith("535") || resp.startsWith("530") || resp.startsWith("4") || resp.startsWith("5"))) {
            if (*stage == 20 && !account.oauthRefreshToken.isEmpty()) {
                refreshGoogleAccessToken(account.email, account.oauthRefreshToken);
            }
            *stage = 11;
            emit smtpSendFinished(
                false,
                MailI18n::t(QString("SMTP 인증 실패 (%1:%2, ID: %3): %4"),
                            QString("SMTP authentication failed (%1:%2, ID: %3): %4"),
                            QString("SMTP 認証失敗 (%1:%2, ID: %3): %4"))
                    .arg(account.smtpHost)
                    .arg(account.smtpPort)
                    .arg(smtpUser, resp.trimmed()));
            socket->disconnectFromHost();
        } else if (*stage == 5 && resp.startsWith("250")) {
            *stage = 6;
            *rcptIdx = 0;
            const QString firstRcpt = allRecipients.isEmpty() ? toEmail.trimmed() : allRecipients.first();
            *rcptIdx = 1;
            socket->write(QString("RCPT TO:<%1>\r\n").arg(firstRcpt).toUtf8());
        } else if (*stage == 6 && resp.startsWith("250")) {
            if (*rcptIdx < allRecipients.size()) {
                const QString nextRcpt = allRecipients.at(*rcptIdx);
                (*rcptIdx)++;
                socket->write(QString("RCPT TO:<%1>\r\n").arg(nextRcpt).toUtf8());
            } else {
                *stage = 7;
                socket->write("DATA\r\n");
            }
        } else if (*stage == 7 && resp.startsWith("354")) {
            *stage = 8;
            // Dot-stuffing per RFC 5321 section 4.5.2
            QByteArray stuffed = rawMime;
            stuffed.replace("\r\n.", "\r\n..");
            socket->write(stuffed + ".\r\n");
        } else if ((*stage == 5 || *stage == 6 || *stage == 7 || *stage == 8)
                   && (resp.startsWith("4") || resp.startsWith("5"))) {
            *stage = 11;
            socket->write("QUIT\r\n");
            emit smtpSendFinished(
                false,
                MailI18n::t(QString("SMTP 메일 발송 거부 (%1:%2): %3"),
                            QString("SMTP send rejected (%1:%2): %3"),
                            QString("SMTP 送信拒否 (%1:%2): %3"))
                    .arg(account.smtpHost)
                    .arg(account.smtpPort)
                    .arg(resp.trimmed()));
            socket->disconnectFromHost();
        } else if (*stage == 8 && resp.startsWith("250")) {
            *stage = 11;
            socket->write("QUIT\r\n");

            // Gmail IMAP은 자동으로 [Gmail]/Sent Mail에 저장하지만, POP3 계정(로컬 디스크 sent/.eml 저장) 및 일반 IMAP 서버는
            // 클라이언트가 직접 appendMessageToImapFolder를 호출하여 보낸편지함에 저장한다.
            const bool isGmailSmtp = account.smtpHost.contains("gmail.com", Qt::CaseInsensitive)
                                     && !account.useSeparateSmtpAuth;
            if (account.isPop3() || !isGmailSmtp) {
                appendMessageToImapFolder(account, QStringLiteral("sent"), toEmail, ccEmail, subject, htmlBody, bccEmail, attachments, isHighImportance);
            } else {
                fetchImapFolder(account, QStringLiteral("sent"), 50, 0);
            }

            emit smtpSendFinished(
                true,
                MailI18n::t(QString("보내는 서버(%1:%2, 계정: %3)를 통해 메일을 발송하고 보낸편지함(Sent)에 저장했습니다!"),
                            QString("Sent email via %1:%2 (%3) and saved to Sent folder!"),
                            QString("送信サーバー (%1:%2, %3) からメールを送信し、送信済みトレイに保存しました！"))
                    .arg(account.smtpHost)
                    .arg(account.smtpPort)
                    .arg(smtpUser));
            socket->disconnectFromHost();
        }
    });

    bindSocketLifecycleCleanup(
        socket,
        nullptr,
        [stage, rcptIdx]() {
            delete stage;
            delete rcptIdx;
        });

    if (useImplicitSsl) {
        socket->connectToHostEncrypted(account.smtpHost, account.smtpPort);
    } else {
        socket->connectToHost(account.smtpHost, account.smtpPort);
    }
}


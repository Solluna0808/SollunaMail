#include "mainwindow.h"
#include "mailtheme.h"

#include <QApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFont>
#include <QFontDatabase>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocalServer>
#include <QLocalSocket>
#include <QLocale>
#include <QStandardPaths>
#include <QTextStream>
#include <QTranslator>
#include <cstdio>
#include <memory>
#ifdef Q_OS_WIN
#include <fcntl.h>
#include <io.h>
#endif

namespace {
QtMessageHandler g_defaultMsgHandler = nullptr;

void sollunaQtMessageFilter(QtMsgType type, const QMessageLogContext &context, const QString &msg)
{
    if (msg.startsWith(QLatin1String("QFont::setPixelSize: Pixel size <= 0"))
        || msg.startsWith(QLatin1String("QFont::setPointSize: Point size <= 0"))
        || msg.startsWith(QLatin1String("QFont::setPointSizeF: Point size <= 0"))
        || msg.contains(QLatin1String("CreateFontFaceFromHDC() failed"))
        || msg.startsWith(QLatin1String("QIODevice::read (QSslSocket): device not open"))
        || msg.startsWith(QLatin1String("QIODevice::write (QSslSocket): device not open"))) {
        return;
    }
    if (g_defaultMsgHandler) {
        g_defaultMsgHandler(type, context, msg);
    } else {
        fprintf(stderr, "%s\n", msg.toLocal8Bit().constData());
    }
}

QString resolveMcpCacheDir()
{
    const QString envDir = qEnvironmentVariable("SOLLUNAMAIL_DATA_DIR").trimmed();
    if (!envDir.isEmpty() && QDir(envDir).exists()) {
        return QDir::fromNativeSeparators(envDir);
    }
    const QString appDir = QCoreApplication::applicationDirPath();
    if (QFileInfo::exists(appDir + QStringLiteral("/cache/mcp_emails_snapshot.json"))
        || QFileInfo::exists(appDir + QStringLiteral("/cache/calendars_cache.json"))
        || QFileInfo::exists(appDir + QStringLiteral("/cache/contacts_cache.json"))) {
        return QDir::fromNativeSeparators(appDir + QStringLiteral("/cache"));
    }
    QString roamingBase = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation);
    if (roamingBase.isEmpty())
        roamingBase = QDir::homePath();
    const QString roamingCache = QDir::fromNativeSeparators(roamingBase + QStringLiteral("/SOLLUNA_MAIL/cache"));
    if (QDir(roamingCache).exists())
        return roamingCache;
    return QDir::fromNativeSeparators(appDir + QStringLiteral("/cache"));
}

QJsonArray loadMcpEmailsArray(const QString &cacheDir)
{
    QJsonArray result;
    const QDir dataRootDir = QFileInfo(cacheDir).dir();
    QStringList candidates = {
        cacheDir + QStringLiteral("/mcp_emails_snapshot.json"),
        cacheDir + QStringLiteral("/conversations_cache.json"),
        dataRootDir.filePath(QStringLiteral("config_set/scheduled_mails.json"))
    };
    const QDir pop3Root(dataRootDir.filePath(QStringLiteral("pop3")));
    if (pop3Root.exists()) {
        for (const QFileInfo &sub : pop3Root.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot)) {
            candidates.append(sub.absoluteFilePath() + QStringLiteral("/mailbox_index.json"));
        }
    }
    QSet<QString> seenIds;
    for (const QString &path : candidates) {
        QFile f(path);
        if (!f.exists() || !f.open(QIODevice::ReadOnly))
            continue;
        const QJsonDocument doc = QJsonDocument::fromJson(f.readAll());
        f.close();
        QJsonArray arr;
        if (doc.isArray()) {
            arr = doc.array();
        } else if (doc.isObject()) {
            const QJsonObject obj = doc.object();
            if (obj.contains(QStringLiteral("conversations")))
                arr = obj.value(QStringLiteral("conversations")).toArray();
            else if (obj.contains(QStringLiteral("scheduledConversations")))
                arr = obj.value(QStringLiteral("scheduledConversations")).toArray();
        }
        for (const QJsonValue &v : arr) {
            const QJsonObject o = v.toObject();
            const QString id = o.value(QStringLiteral("id")).toString();
            if (!id.isEmpty() && !seenIds.contains(id)) {
                seenIds.insert(id);
                result.append(o);
            }
        }
    }
    return result;
}

QJsonArray loadMcpCalendarEventsArray(const QString &cacheDir)
{
    QFile f(cacheDir + QStringLiteral("/calendars_cache.json"));
    if (!f.exists() || !f.open(QIODevice::ReadOnly))
        return QJsonArray();
    const QJsonDocument doc = QJsonDocument::fromJson(f.readAll());
    f.close();
    return doc.object().value(QStringLiteral("events")).toArray();
}

QJsonArray loadMcpContactsArray(const QString &cacheDir)
{
    QFile f(cacheDir + QStringLiteral("/contacts_cache.json"));
    if (!f.exists() || !f.open(QIODevice::ReadOnly))
        return QJsonArray();
    const QJsonDocument doc = QJsonDocument::fromJson(f.readAll());
    f.close();
    return doc.isArray() ? doc.array() : QJsonArray();
}

int runMcpStdioServer()
{
#ifdef Q_OS_WIN
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
#endif
    const QString cacheDir = resolveMcpCacheDir();
    QFile inFile;
    if (!inFile.open(stdin, QIODevice::ReadOnly))
        return 1;

    bool useContentLengthFraming = false;
    auto sendJsonRpc = [&useContentLengthFraming](const QJsonObject &obj) {
        const QByteArray payload = QJsonDocument(obj).toJson(QJsonDocument::Compact);
        if (useContentLengthFraming) {
            const QByteArray header = "Content-Length: " + QByteArray::number(payload.size()) + "\r\n\r\n";
            fwrite(header.constData(), 1, static_cast<size_t>(header.size()), stdout);
            fwrite(payload.constData(), 1, static_cast<size_t>(payload.size()), stdout);
        } else {
            const QByteArray line = payload + "\n";
            fwrite(line.constData(), 1, static_cast<size_t>(line.size()), stdout);
        }
        fflush(stdout);
    };

    while (!inFile.atEnd()) {
        QByteArray rawLine = inFile.readLine().trimmed();
        if (rawLine.isEmpty())
            continue;

        if (rawLine.startsWith("Content-Length:")) {
            useContentLengthFraming = true;
            const int contentLen = rawLine.mid(15).trimmed().toInt();
            while (!inFile.atEnd()) {
                const QByteArray hdrLine = inFile.readLine().trimmed();
                if (hdrLine.isEmpty())
                    break;
            }
            if (contentLen <= 0)
                continue;
            rawLine = inFile.read(contentLen).trimmed();
        }

        QJsonParseError err;
        const QJsonDocument reqDoc = QJsonDocument::fromJson(rawLine, &err);
        if (err.error != QJsonParseError::NoError || !reqDoc.isObject())
            continue;

        const QJsonObject req = reqDoc.object();
        const QString method = req.value(QStringLiteral("method")).toString();
        const bool hasId = req.contains(QStringLiteral("id"));
        const QJsonValue idVal = req.value(QStringLiteral("id"));
        const QJsonObject params = req.value(QStringLiteral("params")).toObject();

        if (!hasId || method.startsWith(QStringLiteral("notifications/"))) {
            continue;
        }

        QJsonObject resp;
        resp[QStringLiteral("jsonrpc")] = QStringLiteral("2.0");
        resp[QStringLiteral("id")] = idVal;

        if (method == QStringLiteral("initialize")) {
            QJsonObject result;
            result[QStringLiteral("protocolVersion")] = params.value(QStringLiteral("protocolVersion")).toString(QStringLiteral("2024-11-05"));
            QJsonObject caps;
            caps[QStringLiteral("tools")] = QJsonObject{{QStringLiteral("listChanged"), false}};
            caps[QStringLiteral("resources")] = QJsonObject{{QStringLiteral("listChanged"), false}};
            result[QStringLiteral("capabilities")] = caps;
            result[QStringLiteral("serverInfo")] = QJsonObject{
                {QStringLiteral("name"), QStringLiteral("sollunamail-mcp")},
                {QStringLiteral("version"), QStringLiteral("1.0.0")}
            };
            resp[QStringLiteral("result")] = result;
            sendJsonRpc(resp);
        } else if (method == QStringLiteral("ping")) {
            resp[QStringLiteral("result")] = QJsonObject();
            sendJsonRpc(resp);
        } else if (method == QStringLiteral("tools/list")) {
            auto makeProp = [](const QString &type, const QString &desc) {
                return QJsonObject{{QStringLiteral("type"), type}, {QStringLiteral("description"), desc}};
            };
            QJsonArray tools;
            tools.append(QJsonObject{
                {QStringLiteral("name"), QStringLiteral("list_emails")},
                {QStringLiteral("description"), QStringLiteral("List recent emails from SollunaMail by folder (inbox, sent, drafts, starred, trash, spam) and optional account filter.")},
                {QStringLiteral("inputSchema"), QJsonObject{
                    {QStringLiteral("type"), QStringLiteral("object")},
                    {QStringLiteral("properties"), QJsonObject{
                        {QStringLiteral("folder"), makeProp(QStringLiteral("string"), QStringLiteral("Folder ID (e.g. inbox, sent, drafts, starred, trash, spam). Empty for all."))},
                        {QStringLiteral("limit"), makeProp(QStringLiteral("integer"), QStringLiteral("Max number of emails to return (default 20)."))}
                    }}
                }}
            });
            tools.append(QJsonObject{
                {QStringLiteral("name"), QStringLiteral("search_emails")},
                {QStringLiteral("description"), QStringLiteral("Search emails in SollunaMail by keyword (matches subject, sender, recipient, or snippet).")},
                {QStringLiteral("inputSchema"), QJsonObject{
                    {QStringLiteral("type"), QStringLiteral("object")},
                    {QStringLiteral("properties"), QJsonObject{
                        {QStringLiteral("query"), makeProp(QStringLiteral("string"), QStringLiteral("Search keyword"))},
                        {QStringLiteral("limit"), makeProp(QStringLiteral("integer"), QStringLiteral("Max results (default 20)"))}
                    }},
                    {QStringLiteral("required"), QJsonArray{QStringLiteral("query")}}
                }}
            });
            tools.append(QJsonObject{
                {QStringLiteral("name"), QStringLiteral("get_email_detail")},
                {QStringLiteral("description"), QStringLiteral("Get full details and body of a specific email by its conversation ID.")},
                {QStringLiteral("inputSchema"), QJsonObject{
                    {QStringLiteral("type"), QStringLiteral("object")},
                    {QStringLiteral("properties"), QJsonObject{
                        {QStringLiteral("id"), makeProp(QStringLiteral("string"), QStringLiteral("Email conversation ID"))}
                    }},
                    {QStringLiteral("required"), QJsonArray{QStringLiteral("id")}}
                }}
            });
            tools.append(QJsonObject{
                {QStringLiteral("name"), QStringLiteral("list_calendar_events")},
                {QStringLiteral("description"), QStringLiteral("List calendar events from SollunaMail (optional date prefix or YYYY-MM-DD filter).")},
                {QStringLiteral("inputSchema"), QJsonObject{
                    {QStringLiteral("type"), QStringLiteral("object")},
                    {QStringLiteral("properties"), QJsonObject{
                        {QStringLiteral("date"), makeProp(QStringLiteral("string"), QStringLiteral("Optional YYYY-MM-DD or YYYY-MM prefix filter"))}
                    }}
                }}
            });
            tools.append(QJsonObject{
                {QStringLiteral("name"), QStringLiteral("search_contacts")},
                {QStringLiteral("description"), QStringLiteral("Search contacts in SollunaMail address book by name, email, company, or phone.")},
                {QStringLiteral("inputSchema"), QJsonObject{
                    {QStringLiteral("type"), QStringLiteral("object")},
                    {QStringLiteral("properties"), QJsonObject{
                        {QStringLiteral("query"), makeProp(QStringLiteral("string"), QStringLiteral("Optional search keyword (empty returns all contacts)"))}
                    }}
                }}
            });
            resp[QStringLiteral("result")] = QJsonObject{{QStringLiteral("tools"), tools}};
            sendJsonRpc(resp);
        } else if (method == QStringLiteral("tools/call")) {
            const QString toolName = params.value(QStringLiteral("name")).toString();
            const QJsonObject args = params.value(QStringLiteral("arguments")).toObject();
            QJsonArray outArr;

            if (toolName == QStringLiteral("list_emails") || toolName == QStringLiteral("search_emails")) {
                const QString folder = args.value(QStringLiteral("folder")).toString().trimmed().toLower();
                const QString query = args.value(QStringLiteral("query")).toString().trimmed();
                const int limit = qBound(1, args.value(QStringLiteral("limit")).toInt(20), 100);
                const QJsonArray allEmails = loadMcpEmailsArray(cacheDir);
                for (const QJsonValue &v : allEmails) {
                    const QJsonObject o = v.toObject();
                    if (!folder.isEmpty() && o.value(QStringLiteral("folderId")).toString().toLower() != folder)
                        continue;
                    if (!query.isEmpty()) {
                        const QString hay = o.value(QStringLiteral("subject")).toString() + QLatin1Char(' ')
                                          + o.value(QStringLiteral("senderName")).toString() + QLatin1Char(' ')
                                          + o.value(QStringLiteral("senderEmail")).toString() + QLatin1Char(' ')
                                          + o.value(QStringLiteral("recipientSummary")).toString() + QLatin1Char(' ')
                                          + o.value(QStringLiteral("snippet")).toString();
                        if (!hay.contains(query, Qt::CaseInsensitive))
                            continue;
                    }
                    QJsonObject item = o;
                    item.remove(QStringLiteral("htmlBody"));
                    outArr.append(item);
                    if (outArr.size() >= limit)
                        break;
                }
            } else if (toolName == QStringLiteral("get_email_detail")) {
                const QString targetId = args.value(QStringLiteral("id")).toString().trimmed();
                const QJsonArray allEmails = loadMcpEmailsArray(cacheDir);
                for (const QJsonValue &v : allEmails) {
                    const QJsonObject o = v.toObject();
                    if (o.value(QStringLiteral("id")).toString() == targetId) {
                        outArr.append(o);
                        break;
                    }
                }
            } else if (toolName == QStringLiteral("list_calendar_events")) {
                const QString datePrefix = args.value(QStringLiteral("date")).toString().trimmed();
                const QJsonArray allEv = loadMcpCalendarEventsArray(cacheDir);
                for (const QJsonValue &v : allEv) {
                    const QJsonObject o = v.toObject();
                    if (!datePrefix.isEmpty() && !o.value(QStringLiteral("date")).toString().startsWith(datePrefix))
                        continue;
                    outArr.append(o);
                }
            } else if (toolName == QStringLiteral("search_contacts")) {
                const QString query = args.value(QStringLiteral("query")).toString().trimmed();
                const QJsonArray allCt = loadMcpContactsArray(cacheDir);
                for (const QJsonValue &v : allCt) {
                    const QJsonObject o = v.toObject();
                    if (!query.isEmpty()) {
                        const QString hay = o.value(QStringLiteral("displayName")).toString() + QLatin1Char(' ')
                                          + o.value(QStringLiteral("email")).toString() + QLatin1Char(' ')
                                          + o.value(QStringLiteral("company")).toString() + QLatin1Char(' ')
                                          + o.value(QStringLiteral("phone")).toString();
                        if (!hay.contains(query, Qt::CaseInsensitive))
                            continue;
                    }
                    outArr.append(o);
                }
            }

            const QString textPayload = QString::fromUtf8(QJsonDocument(outArr).toJson(QJsonDocument::Indented));
            resp[QStringLiteral("result")] = QJsonObject{
                {QStringLiteral("content"), QJsonArray{
                    QJsonObject{
                        {QStringLiteral("type"), QStringLiteral("text")},
                        {QStringLiteral("text"), textPayload}
                    }
                }},
                {QStringLiteral("isError"), false}
            };
            sendJsonRpc(resp);
        } else if (method == QStringLiteral("resources/list")) {
            QJsonArray resources;
            resources.append(QJsonObject{
                {QStringLiteral("uri"), QStringLiteral("sollunamail://emails")},
                {QStringLiteral("name"), QStringLiteral("SollunaMail Emails Snapshot")},
                {QStringLiteral("mimeType"), QStringLiteral("application/json")}
            });
            resources.append(QJsonObject{
                {QStringLiteral("uri"), QStringLiteral("sollunamail://calendar")},
                {QStringLiteral("name"), QStringLiteral("SollunaMail Calendar Events")},
                {QStringLiteral("mimeType"), QStringLiteral("application/json")}
            });
            resources.append(QJsonObject{
                {QStringLiteral("uri"), QStringLiteral("sollunamail://contacts")},
                {QStringLiteral("name"), QStringLiteral("SollunaMail Contacts")},
                {QStringLiteral("mimeType"), QStringLiteral("application/json")}
            });
            resp[QStringLiteral("result")] = QJsonObject{{QStringLiteral("resources"), resources}};
            sendJsonRpc(resp);
        } else if (method == QStringLiteral("resources/read")) {
            const QString uri = params.value(QStringLiteral("uri")).toString();
            QJsonArray dataArr;
            if (uri.contains(QStringLiteral("calendar")))
                dataArr = loadMcpCalendarEventsArray(cacheDir);
            else if (uri.contains(QStringLiteral("contacts")))
                dataArr = loadMcpContactsArray(cacheDir);
            else
                dataArr = loadMcpEmailsArray(cacheDir);
            resp[QStringLiteral("result")] = QJsonObject{
                {QStringLiteral("contents"), QJsonArray{
                    QJsonObject{
                        {QStringLiteral("uri"), uri},
                        {QStringLiteral("mimeType"), QStringLiteral("application/json")},
                        {QStringLiteral("text"), QString::fromUtf8(QJsonDocument(dataArr).toJson(QJsonDocument::Indented))}
                    }
                }}
            };
            sendJsonRpc(resp);
        } else {
            resp[QStringLiteral("error")] = QJsonObject{
                {QStringLiteral("code"), -32601},
                {QStringLiteral("message"), QStringLiteral("Method not found: ") + method}
            };
            sendJsonRpc(resp);
        }
    }
    return 0;
}
} // namespace

int main(int argc, char *argv[])
{
    for (int i = 1; i < argc; ++i) {
        if (QString::fromLocal8Bit(argv[i]).compare(QStringLiteral("--mcp-stdio"), Qt::CaseInsensitive) == 0) {
            QCoreApplication mcpApp(argc, argv);
            mcpApp.setApplicationName(QStringLiteral("SollunaMail"));
            mcpApp.setOrganizationName(QStringLiteral("Solluna IoT"));
            return runMcpStdioServer();
        }
    }

    g_defaultMsgHandler = qInstallMessageHandler(sollunaQtMessageFilter);
    QApplication a(argc, argv);
    a.setApplicationName("SollunaMail");
    a.setApplicationDisplayName("SollunaMail");
    a.setOrganizationName("Solluna IoT");
    a.setApplicationVersion("1.0.0");
    a.setWindowIcon(MailIcons::sollunaAppIcon(0));
    a.setQuitOnLastWindowClosed(false);

    // 단일 인스턴스(Single Instance) 보장: 이미 실행 중이면 기존 창을 앞으로 띄우고 즉시 종료
    const QString ipcServerName = QStringLiteral("SollunaMail_SingleInstance_IPC_v1");
    {
        QLocalSocket probeSocket;
        probeSocket.connectToServer(ipcServerName);
        if (probeSocket.waitForConnected(400)) {
            probeSocket.write("ACTIVATE");
            probeSocket.flush();
            probeSocket.waitForBytesWritten(200);
            probeSocket.disconnectFromServer();
            return 0;
        }
    }
    QLocalServer::removeServer(ipcServerName);
    QLocalServer singleInstanceServer;
    singleInstanceServer.listen(ipcServerName);

    MailI18n::loadBundledCjkFonts();
    MailI18n::applyLanguageFont();

    QTranslator translator;
    const QStringList uiLanguages = QLocale::system().uiLanguages();
    for (const QString &locale : uiLanguages) {
        const QString baseName = "SollunaMail_" + QLocale(locale).name();
        if (translator.load(":/i18n/" + baseName)) {
            a.installTranslator(&translator);
            break;
        }
    }
    auto w = std::make_unique<MainWindow>();
    QObject::connect(&singleInstanceServer, &QLocalServer::newConnection, w.get(), [&singleInstanceServer, &w]() {
        while (QLocalSocket *client = singleInstanceServer.nextPendingConnection()) {
            client->waitForReadyRead(100);
            client->readAll();
            client->disconnectFromServer();
            client->deleteLater();
        }
        if (w) {
            w->showNormal();
            w->raise();
            w->activateWindow();
        }
    });

    const QStringList args = a.arguments();
    const bool forceMinimizedArg = args.contains(QStringLiteral("--minimized"), Qt::CaseInsensitive);
    if ((MailI18n::startMinimizedToTray() || forceMinimizedArg) && QSystemTrayIcon::isSystemTrayAvailable()) {
        w->hide();
    } else {
        w->show();
    }
    const int exitCode = QApplication::exec();
    singleInstanceServer.disconnect();
    singleInstanceServer.close();
    w.reset();
    return exitCode;
}

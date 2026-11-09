#ifndef MAILTHEME_H
#define MAILTHEME_H

#include <QColor>
#include <QIcon>
#include <QPixmap>
#include <QString>
#include <QStringList>
#include "mailmodels.h"

struct MailThemePalette {
    QString name;
    QColor leftBarBg;          // #093282 (Deep Royal Blue)
    QColor folderPaneBg;       // #0B3B91 (Mailbird Royal Blue)
    QColor sidebarText;        // #FFFFFF
    QColor sidebarMutedText;   // #9AB3E2
    QColor sidebarHoverBg;     // #184CA8
    QColor sidebarSelectedBg;  // #2258B8
    QColor windowBg;           // #FFFFFF
    QColor panelBg;            // #FFFFFF
    QColor subtleBg;           // #F7F8FA
    QColor selectedRowBg;      // #0D47A1 (Selected Mail Row Royal Blue)
    QColor selectedRowFg;      // #FFFFFF
    QColor primaryAccent;      // #0D47A1
    QColor primaryHover;       // #1557B8
    QColor textPrimary;        // #111827
    QColor textSecondary;      // #4B5563
    QColor textMuted;          // #6B7280
    QColor borderColor;        // #E5E7EB
};

class MailThemeManager {
public:
    static MailThemePalette getPalette(const QString &themeName = "BlueFocus (MailBird Royal)");
    static QStringList availableThemes();
    static QString buildGlobalStyleSheet(const MailThemePalette &p);
};

class MailIcons {
public:
    static QPixmap sollunaAppPixmap(int size = 64, int unreadBadgeCount = 0);
    static QIcon sollunaAppIcon(int unreadBadgeCount = 0);
    static QIcon folderIcon(FolderKind kind, const QColor &color, int size = 18);
    static QIcon actionIcon(const QString &actionName, const QColor &color, int size = 18);
    static QIcon settingsNavIcon(int navIndex, const QColor &color, int size = 18);
    static QPixmap avatarPresetPixmap(int presetIndex, const AccountItem &acc, int size = 38, bool selectedBorder = false);
    static QPixmap accountAvatarPixmap(const AccountItem &acc, int size = 34, bool activeIndicator = false, int unreadCount = 0);
    static QPixmap senderAvatarPixmap(const MessageItem &msg, int size = 36);
};

#endif // MAILTHEME_H


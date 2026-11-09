#include "mailtheme.h"
#include <QPainter>
#include <QPainterPath>
#include <QLinearGradient>
#include <QFont>
#include <QCoreApplication>
#include <QFileInfo>
#include <QToolTip>
#include <QPalette>
#include <QApplication>
#include <QGuiApplication>
#include <QStyleHints>

MailThemePalette MailThemeManager::getPalette(const QString &themeName)
{
    MailThemePalette p;
    p.name = themeName;
    // Default matches Screenshot 1, 2, 3: Deep Royal Blue sidebar + crisp White workspace
    p.leftBarBg         = QColor("#08307E");
    p.folderPaneBg      = QColor("#0B3B91");
    p.sidebarText       = QColor("#FFFFFF");
    p.sidebarMutedText  = QColor("#9BB5E8");
    p.sidebarHoverBg    = QColor("#1649A4");
    p.sidebarSelectedBg = QColor("#2458B6");
    p.windowBg          = QColor("#FFFFFF");
    p.panelBg           = QColor("#FFFFFF");
    p.subtleBg          = QColor("#F5F6F8");
    p.selectedRowBg     = QColor("#0D47A1");
    p.selectedRowFg     = QColor("#FFFFFF");
    p.primaryAccent     = QColor("#0D47A1");
    p.primaryHover      = QColor("#1558BC");
    p.textPrimary       = QColor("#111827");
    p.textSecondary     = QColor("#374151");
    p.textMuted         = QColor("#6B7280");
    p.borderColor       = QColor("#E5E7EB");

    if (themeName.contains("Obsidian") || themeName.contains("GraphitePro")) {
        p.leftBarBg = QColor("#09090B");
        p.folderPaneBg = QColor("#18181B");
        p.sidebarHoverBg = QColor("#27272A");
        p.sidebarSelectedBg = QColor("#3F3F46");
        p.selectedRowBg = QColor("#18181B");
        p.primaryAccent = QColor("#27272A");
    } else if (themeName.contains("Dove")) {
        p.leftBarBg = QColor("#1F2937");
        p.folderPaneBg = QColor("#374151");
        p.sidebarHoverBg = QColor("#4B5563");
        p.sidebarSelectedBg = QColor("#6B7280");
        p.selectedRowBg = QColor("#374151");
        p.primaryAccent = QColor("#4B5563");
    } else if (themeName.contains("Ice") || themeName.contains("NordicInbox")) {
        p.leftBarBg = QColor("#0F172A");
        p.folderPaneBg = QColor("#1E293B");
        p.sidebarHoverBg = QColor("#334155");
        p.sidebarSelectedBg = QColor("#2563EB");
        p.selectedRowBg = QColor("#1D4ED8");
        p.primaryAccent = QColor("#2563EB");
    } else if (themeName.contains("Mint")) {
        p.leftBarBg = QColor("#0F2E2B");
        p.folderPaneBg = QColor("#134E4A");
        p.sidebarHoverBg = QColor("#115E59");
        p.sidebarSelectedBg = QColor("#0D9488");
        p.selectedRowBg = QColor("#115E59");
        p.primaryAccent = QColor("#0D9488");
    } else if (themeName.contains("Espresso")) {
        p.leftBarBg = QColor("#29150A");
        p.folderPaneBg = QColor("#3E2112");
        p.sidebarHoverBg = QColor("#57301B");
        p.sidebarSelectedBg = QColor("#78350F");
        p.selectedRowBg = QColor("#451A03");
        p.primaryAccent = QColor("#78350F");
    } else if (themeName.contains("Indigo")) {
        p.leftBarBg = QColor("#241547");
        p.folderPaneBg = QColor("#341F65");
        p.sidebarHoverBg = QColor("#4C1D95");
        p.sidebarSelectedBg = QColor("#5B21B6");
        p.selectedRowBg = QColor("#3B0764");
        p.primaryAccent = QColor("#4C1D95");
    } else if (themeName.contains("Plum")) {
        p.leftBarBg = QColor("#380C3D");
        p.folderPaneBg = QColor("#54165B");
        p.sidebarHoverBg = QColor("#701A75");
        p.sidebarSelectedBg = QColor("#86198F");
        p.selectedRowBg = QColor("#54165B");
        p.primaryAccent = QColor("#701A75");
    } else if (themeName.contains("Evergreen")) {
        p.leftBarBg = QColor("#063729");
        p.folderPaneBg = QColor("#0A4F3C");
        p.sidebarHoverBg = QColor("#047857");
        p.sidebarSelectedBg = QColor("#059669");
        p.selectedRowBg = QColor("#065F46");
        p.primaryAccent = QColor("#059669");
    }

    if (MailI18n::appearanceMode().compare("Dark", Qt::CaseInsensitive) == 0) {
        p.windowBg = QColor("#18181B");
        p.panelBg = QColor("#1F2024");
        p.subtleBg = QColor("#27272A");
        p.textPrimary = QColor("#F3F4F6");
        p.textSecondary = QColor("#D1D5DB");
        p.textMuted = QColor("#9CA3AF");
        p.borderColor = QColor("#3F3F46");
    }

    return p;
}

QStringList MailThemeManager::availableThemes()
{
    return {
        "Obsidian Black",
        "Dove Gray",
        "Ice Slate (NordicInbox)",
        "Mint Forest",
        "Espresso Sand",
        "BlueFocus (MailBird Royal)",
        "Royal Indigo",
        "Plum Studio",
        "Evergreen Desk"
    };
}

QString MailThemeManager::buildGlobalStyleSheet(const MailThemePalette &p)
{
    const bool isDark = (MailI18n::appearanceMode().compare("Dark", Qt::CaseInsensitive) == 0);
    if (QGuiApplication::styleHints()) {
        QGuiApplication::styleHints()->setColorScheme(isDark ? Qt::ColorScheme::Dark : Qt::ColorScheme::Light);
    }
    if (qApp) {
        QPalette appPal = qApp->palette();
        appPal.setColor(QPalette::Window, p.windowBg);
        appPal.setColor(QPalette::WindowText, p.textPrimary);
        appPal.setColor(QPalette::Base, p.panelBg);
        appPal.setColor(QPalette::AlternateBase, p.subtleBg);
        appPal.setColor(QPalette::Text, p.textPrimary);
        appPal.setColor(QPalette::Button, p.panelBg);
        appPal.setColor(QPalette::ButtonText, p.textPrimary);
        appPal.setColor(QPalette::PlaceholderText, p.textMuted);
        qApp->setPalette(appPal);
    }

    QPalette tipPal = QToolTip::palette();
    tipPal.setColor(QPalette::ToolTipBase, QColor("#FFFFFF"));
    tipPal.setColor(QPalette::ToolTipText, QColor("#111827"));
    tipPal.setColor(QPalette::Window, QColor("#FFFFFF"));
    tipPal.setColor(QPalette::WindowText, QColor("#111827"));
    tipPal.setColor(QPalette::Text, QColor("#111827"));
    QToolTip::setPalette(tipPal);

    const QString fontStack = (MailI18n::currentLanguage() == AppLanguage::Japanese)
        ? QStringLiteral("\"Yu Gothic UI\", \"Yu Gothic\", \"Meiryo\", \"Malgun Gothic\", \"Segoe UI\", sans-serif")
        : QStringLiteral("\"Malgun Gothic\", \"Segoe UI\", \"Yu Gothic UI\", \"Yu Gothic\", sans-serif");

    return QString(R"(
        QWidget {
            font-family: %3;
        }
        QMainWindow, QDialog, QMessageBox {
            background-color: %1;
            color: %2;
        }
        QLabel, QCheckBox, QRadioButton, QGroupBox {
            color: %2;
        }
        QLineEdit, QSpinBox, QDoubleSpinBox, QDateEdit, QTimeEdit, QDateTimeEdit, QComboBox, QTextEdit, QPlainTextEdit {
            background-color: %4;
            color: %2;
            selection-background-color: #2563EB;
            selection-color: #FFFFFF;
        }
        QComboBox QAbstractItemView {
            background-color: %4;
            color: %2;
            selection-background-color: #EFF6FF;
            selection-color: #0D47A1;
        }
        QTableWidget, QListWidget, QTreeWidget {
            background-color: %4;
            color: %2;
            gridline-color: %5;
        }
        QHeaderView::section {
            background-color: %6;
            color: %2;
            border: 1px solid %5;
            padding: 5px 8px;
            font-weight: 700;
        }
        QMenu {
            background-color: #FFFFFF;
            color: #111827;
            border: 1px solid #D1D5DB;
        }
        QMenu::item {
            color: #111827;
            padding: 6px 14px;
        }
        QMenu::item:selected {
            background-color: #EFF6FF;
            color: #0D47A1;
        }
        QScrollBar:vertical {
            background: transparent;
            width: 7px;
            margin: 0px;
        }
        QScrollBar::handle:vertical {
            background: #CBD5E1;
            min-height: 24px;
            border-radius: 3px;
        }
        QScrollBar::handle:vertical:hover {
            background: #94A3B8;
        }
        QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical {
            height: 0px;
        }
        QScrollBar:horizontal {
            background: transparent;
            height: 7px;
            margin: 0px;
        }
        QScrollBar::handle:horizontal {
            background: #CBD5E1;
            min-width: 24px;
            border-radius: 3px;
        }
        QScrollBar::add-line:horizontal, QScrollBar::sub-line:horizontal {
            width: 0px;
        }
        QToolTip {
            background-color: #FFFFFF;
            color: #111827;
            border: 1px solid #CBD5E1;
            padding: 5px 10px;
            border-radius: 6px;
            font-size: 12px;
            font-weight: 600;
        }
    )").arg(p.windowBg.name(), p.textPrimary.name(), fontStack, p.panelBg.name(), p.borderColor.name(), p.subtleBg.name());
}

QPixmap MailIcons::sollunaAppPixmap(int size, int unreadBadgeCount)
{
    QPixmap pix(size * 2, size * 2);
    pix.setDevicePixelRatio(2.0);
    pix.fill(Qt::transparent);

    QPainter p(&pix);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setRenderHint(QPainter::SmoothPixmapTransform, true);

    const qreal s = size;
    const QRectF outerRect = (unreadBadgeCount > 0)
                                 ? QRectF(s * 0.02, s * 0.06, s * 0.90, s * 0.90)
                                 : QRectF(s * 0.02, s * 0.02, s * 0.96, s * 0.96);

    static QPixmap s_logoPixmap;
    static bool s_logoTried = false;
    if (!s_logoTried) {
        s_logoTried = true;
        const QStringList candidatePaths = {
            QStringLiteral(":/solluna_icon.png"),
            QCoreApplication::applicationDirPath() + QStringLiteral("/solluna_icon.png"),
            QStringLiteral("solluna_icon.png")
        };
        for (const QString &path : candidatePaths) {
            QPixmap loaded(path);
            if (!loaded.isNull()) {
                s_logoPixmap = loaded;
                break;
            }
        }
    }

    if (!s_logoPixmap.isNull()) {
        QPixmap scaledLogo = s_logoPixmap.scaled(
            int(outerRect.width() * 2.0),
            int(outerRect.height() * 2.0),
            Qt::KeepAspectRatio,
            Qt::SmoothTransformation);
        scaledLogo.setDevicePixelRatio(2.0);
        p.drawPixmap(outerRect.topLeft(), scaledLogo);
    } else {
        const qreal outerRadius = s * 0.22;

        // 1. Royal Blue outer frame (#0D47A1)
        p.setPen(Qt::NoPen);
        p.setBrush(QColor("#0F439E"));
        p.drawRoundedRect(outerRect, outerRadius, outerRadius);

        // 2. White subtle inner ring border
        const QRectF whiteRing = outerRect.adjusted(s * 0.045, s * 0.045, -s * 0.045, -s * 0.045);
        p.setPen(QPen(QColor("#E2E8F0"), qMax(1.0, s * 0.025)));
        p.setBrush(QColor("#061026")); // Deep Navy Space Background
        p.drawRoundedRect(whiteRing, outerRadius * 0.80, outerRadius * 0.80);

        const QPointF center = whiteRing.center();
        const qreal orbitR = whiteRing.width() * 0.36;

        // 3. Dotted circular orbit ring
        QPen orbitPen(QColor(148, 163, 184, 130), qMax(1.0, s * 0.022), Qt::DotLine);
        p.setPen(orbitPen);
        p.setBrush(Qt::NoBrush);
        p.drawEllipse(center, orbitR, orbitR);

        // 4. Right Cyan-Blue Crescent Moon (Luna)
        {
            QPainterPath moonOuter;
            qreal moonR = whiteRing.width() * 0.31;
            QPointF moonC(center.x() + s * 0.03, center.y() + s * 0.01);
            moonOuter.addEllipse(moonC, moonR, moonR);

            QPainterPath moonCut;
            QPointF cutC(center.x() - s * 0.055, center.y() - s * 0.01);
            moonCut.addEllipse(cutC, moonR * 0.92, moonR * 0.92);

            QPainterPath crescent = moonOuter.subtracted(moonCut);
            QLinearGradient moonGrad(moonC.x(), moonC.y() - moonR, moonC.x(), moonC.y() + moonR);
            moonGrad.setColorAt(0.0, QColor("#22D3EE"));
            moonGrad.setColorAt(0.55, QColor("#38BDF8"));
            moonGrad.setColorAt(1.0, QColor("#2563EB"));
            p.setPen(Qt::NoPen);
            p.setBrush(moonGrad);
            p.drawPath(crescent);
        }

        // 5. Warm orange glow ring + Orange Sun (Sol) with white '+' starburst
        const QPointF sunCenter(center.x() - s * 0.035, center.y() + s * 0.02);
        const qreal sunGlowR = whiteRing.width() * 0.235;
        p.setPen(QPen(QColor(245, 158, 11, 140), qMax(1.0, s * 0.022)));
        p.setBrush(Qt::NoBrush);
        p.drawEllipse(sunCenter, sunGlowR, sunGlowR);

        const qreal sunR = whiteRing.width() * 0.175;
        QRadialGradient sunGrad(sunCenter - QPointF(sunR * 0.25, sunR * 0.25), sunR * 1.3);
        sunGrad.setColorAt(0.0, QColor("#FBBF24"));
        sunGrad.setColorAt(0.55, QColor("#F97316"));
        sunGrad.setColorAt(1.0, QColor("#EA580C"));
        p.setPen(Qt::NoPen);
        p.setBrush(sunGrad);
        p.drawEllipse(sunCenter, sunR, sunR);

        // White '+' starburst inside the Orange Sun
        QPen plusPen(QColor("#FFFFFF"), qMax(1.4, s * 0.038), Qt::SolidLine, Qt::RoundCap);
        p.setPen(plusPen);
        const qreal arm = sunR * 0.52;
        p.drawLine(QPointF(sunCenter.x() - arm, sunCenter.y()), QPointF(sunCenter.x() + arm, sunCenter.y()));
        p.drawLine(QPointF(sunCenter.x(), sunCenter.y() - arm), QPointF(sunCenter.x(), sunCenter.y() + arm));

        // 6. Three Orbiting Planet Dots: Cyan (top), Orange (upper-right), Yellow (bottom-left)
        p.setPen(Qt::NoPen);
        const qreal dotR = qMax(1.6, s * 0.042);
        p.setBrush(QColor("#00F5D4"));
        p.drawEllipse(QPointF(center.x() + orbitR * 0.12, center.y() - orbitR * 0.98), dotR, dotR);
        p.setBrush(QColor("#F97316"));
        p.drawEllipse(QPointF(center.x() + orbitR * 0.86, center.y() - orbitR * 0.48), dotR * 0.9, dotR * 0.9);
        p.setBrush(QColor("#FACC15"));
        p.drawEllipse(QPointF(center.x() - orbitR * 0.65, center.y() + orbitR * 0.74), dotR * 0.9, dotR * 0.9);
    }

    // 7. Mailbird-style Red Unread Count Badge at top-right (media_1793682185415_4085788a.png)
    if (unreadBadgeCount > 0) {
        QString badgeText = (unreadBadgeCount > 99) ? QStringLiteral("99+") : QString::number(unreadBadgeCount);
        const qreal badgeH = s * 0.46;
        const qreal badgeW = (badgeText.length() >= 3) ? (s * 0.58)
                           : (badgeText.length() == 2) ? (s * 0.50)
                                                       : badgeH;
        const QRectF badgeRect(s - badgeW - s * 0.01, s * 0.01, badgeW, badgeH);

        QLinearGradient redGrad(badgeRect.topLeft(), badgeRect.bottomLeft());
        redGrad.setColorAt(0.0, QColor("#EF4444"));
        redGrad.setColorAt(1.0, QColor("#B91C1C"));

        p.setPen(QPen(QColor("#7F1D1D"), qMax(1.0, s * 0.025)));
        p.setBrush(redGrad);
        p.drawRoundedRect(badgeRect, badgeH / 2.0, badgeH / 2.0);

        p.setPen(QColor("#FFFFFF"));
        QFont bf("Segoe UI", qMax(7, int(badgeH * 0.50)), QFont::Bold);
        bf.setPixelSize(qMax(9, int(badgeH * 0.64)));
        p.setFont(bf);
        p.drawText(badgeRect.adjusted(0, -s * 0.01, 0, 0), Qt::AlignCenter, badgeText);
    }

    return pix;
}

QIcon MailIcons::sollunaAppIcon(int unreadBadgeCount)
{
    QIcon icon;
    const QList<int> sizes = {16, 24, 32, 48, 64, 128};
    for (int sz : sizes) {
        icon.addPixmap(sollunaAppPixmap(sz, unreadBadgeCount));
    }
    return icon;
}

QIcon MailIcons::folderIcon(FolderKind kind, const QColor &color, int size)
{
    QPixmap pix(size * 2, size * 2);
    pix.setDevicePixelRatio(2.0);
    pix.fill(Qt::transparent);

    QPainter painter(&pix);
    painter.setRenderHint(QPainter::Antialiasing, true);
    QPen pen(color, 1.4, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
    painter.setPen(pen);
    painter.setBrush(Qt::NoBrush);

    switch (kind) {
    case FolderKind::Inbox: {
        painter.drawRoundedRect(QRectF(2.5, 4.0, 13.0, 10.0), 1.8, 1.8);
        QPainterPath flap;
        flap.moveTo(2.8, 4.5);
        flap.lineTo(9.0, 9.5);
        flap.lineTo(15.2, 4.5);
        painter.drawPath(flap);
        break;
    }
    case FolderKind::Starred: {
        QPainterPath star;
        star.moveTo(9.0, 2.2);
        star.lineTo(11.0, 6.5);
        star.lineTo(15.6, 7.1);
        star.lineTo(12.2, 10.3);
        star.lineTo(13.1, 15.0);
        star.lineTo(9.0, 12.7);
        star.lineTo(4.9, 15.0);
        star.lineTo(5.8, 10.3);
        star.lineTo(2.4, 7.1);
        star.lineTo(7.0, 6.5);
        star.closeSubpath();
        painter.drawPath(star);
        break;
    }
    case FolderKind::Drafts: {
        painter.drawRoundedRect(QRectF(3.5, 2.5, 11.0, 13.0), 1.5, 1.5);
        painter.drawLine(QPointF(6.0, 6.5), QPointF(12.0, 6.5));
        painter.drawLine(QPointF(6.0, 9.5), QPointF(12.0, 9.5));
        painter.drawLine(QPointF(6.0, 12.5), QPointF(9.5, 12.5));
        break;
    }
    case FolderKind::Sent: {
        QPainterPath plane;
        plane.moveTo(2.5, 8.5);
        plane.lineTo(15.5, 3.0);
        plane.lineTo(11.0, 15.5);
        plane.lineTo(8.2, 10.2);
        plane.closeSubpath();
        painter.drawPath(plane);
        painter.drawLine(QPointF(8.2, 10.2), QPointF(15.5, 3.0));
        break;
    }
    case FolderKind::Archive: {
        painter.drawRoundedRect(QRectF(2.5, 3.0, 13.0, 3.2), 1.0, 1.0);
        painter.drawRoundedRect(QRectF(3.5, 6.2, 11.0, 8.8), 1.2, 1.2);
        painter.drawLine(QPointF(7.0, 9.5), QPointF(11.0, 9.5));
        break;
    }
    case FolderKind::Snoozed: {
        painter.drawEllipse(QRectF(2.5, 2.5, 13.0, 13.0));
        painter.drawLine(QPointF(9.0, 5.5), QPointF(9.0, 9.2));
        painter.drawLine(QPointF(9.0, 9.2), QPointF(12.0, 10.8));
        break;
    }
    case FolderKind::Important: {
        QPainterPath tag;
        tag.moveTo(3.0, 5.0);
        tag.lineTo(11.5, 5.0);
        tag.lineTo(15.5, 9.0);
        tag.lineTo(11.5, 13.0);
        tag.lineTo(3.0, 13.0);
        tag.lineTo(5.5, 9.0);
        tag.closeSubpath();
        painter.drawPath(tag);
        break;
    }
    case FolderKind::Spam: {
        painter.drawEllipse(QRectF(2.5, 2.5, 13.0, 13.0));
        painter.drawLine(QPointF(4.5, 4.5), QPointF(13.5, 13.5));
        break;
    }
    case FolderKind::Trash: {
        painter.drawLine(QPointF(3.5, 5.0), QPointF(14.5, 5.0));
        painter.drawLine(QPointF(7.0, 3.2), QPointF(11.0, 3.2));
        painter.drawRoundedRect(QRectF(4.8, 5.0, 8.4, 10.2), 1.2, 1.2);
        painter.drawLine(QPointF(7.5, 7.5), QPointF(7.5, 12.5));
        painter.drawLine(QPointF(10.5, 7.5), QPointF(10.5, 12.5));
        break;
    }
    case FolderKind::CustomFolder: {
        painter.setPen(Qt::NoPen);
        painter.setBrush(color);
        painter.drawRoundedRect(QRectF(5.5, 5.5, 7.0, 7.0), 2.0, 2.0);
        break;
    }
    }
    return QIcon(pix);
}

QIcon MailIcons::actionIcon(const QString &actionName, const QColor &color, int size)
{
    QPixmap pix(size * 2, size * 2);
    pix.setDevicePixelRatio(2.0);
    pix.fill(Qt::transparent);

    QPainter painter(&pix);
    painter.setRenderHint(QPainter::Antialiasing, true);
    QPen pen(color, 1.4, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
    painter.setPen(pen);
    painter.setBrush(Qt::NoBrush);

    if (actionName == "compose") {
        painter.drawRoundedRect(QRectF(3.0, 4.0, 10.5, 11.0), 1.8, 1.8);
        painter.drawLine(QPointF(9.5, 8.5), QPointF(15.2, 2.8));
        painter.drawLine(QPointF(13.2, 2.5), QPointF(15.5, 4.8));
    } else if (actionName == "search") {
        painter.drawEllipse(QRectF(3.0, 3.0, 9.0, 9.0));
        painter.drawLine(QPointF(10.8, 10.8), QPointF(15.0, 15.0));
    } else if (actionName == "unread_filter") {
        painter.drawLine(QPointF(3.0, 5.5), QPointF(12.0, 5.5));
        painter.drawLine(QPointF(3.0, 9.5), QPointF(15.0, 9.5));
        painter.drawLine(QPointF(3.0, 13.5), QPointF(10.5, 13.5));
        painter.setBrush(color);
        painter.drawEllipse(QRectF(12.5, 3.5, 3.2, 3.2));
    } else if (actionName == "refresh") {
        painter.drawArc(QRectF(3.2, 3.2, 11.6, 11.6), 35 * 16, 285 * 16);
        painter.drawLine(QPointF(11.5, 2.8), QPointF(14.8, 4.5));
        painter.drawLine(QPointF(14.8, 4.5), QPointF(12.5, 7.2));
    } else if (actionName == "more") {
        painter.setPen(Qt::NoPen);
        painter.setBrush(color);
        painter.drawEllipse(QRectF(8.0, 3.2, 2.0, 2.0));
        painter.drawEllipse(QRectF(8.0, 8.0, 2.0, 2.0));
        painter.drawEllipse(QRectF(8.0, 12.8, 2.0, 2.0));
    } else if (actionName == "reply") {
        QPainterPath p;
        p.moveTo(7.5, 4.5);
        p.lineTo(3.0, 8.5);
        p.lineTo(7.5, 12.5);
        painter.drawPath(p);
        QPainterPath curve;
        curve.moveTo(3.5, 8.5);
        curve.cubicTo(11.0, 8.5, 14.5, 10.0, 14.8, 14.2);
        painter.drawPath(curve);
    } else if (actionName == "reply_all") {
        QPainterPath p1;
        p1.moveTo(6.5, 4.5);
        p1.lineTo(2.2, 8.5);
        p1.lineTo(6.5, 12.5);
        p1.moveTo(9.5, 4.5);
        p1.lineTo(5.2, 8.5);
        p1.lineTo(9.5, 12.5);
        painter.drawPath(p1);
        QPainterPath curve;
        curve.moveTo(5.5, 8.5);
        curve.cubicTo(12.0, 8.5, 15.0, 10.0, 15.2, 14.2);
        painter.drawPath(curve);
    } else if (actionName == "forward") {
        QPainterPath p;
        p.moveTo(10.5, 4.5);
        p.lineTo(15.0, 8.5);
        p.lineTo(10.5, 12.5);
        painter.drawPath(p);
        QPainterPath curve;
        curve.moveTo(14.5, 8.5);
        curve.cubicTo(7.0, 8.5, 3.5, 10.0, 3.2, 14.2);
        painter.drawPath(curve);
    } else if (actionName == "calendar_3") {
        painter.drawRoundedRect(QRectF(2.8, 3.5, 12.4, 11.8), 1.8, 1.8);
        painter.drawLine(QPointF(2.8, 6.8), QPointF(15.2, 6.8));
        painter.drawLine(QPointF(6.0, 2.2), QPointF(6.0, 4.5));
        painter.drawLine(QPointF(12.0, 2.2), QPointF(12.0, 4.5));
        QFont f("Segoe UI", 6, QFont::Bold);
        painter.setFont(f);
        painter.drawText(QRectF(2.8, 6.8, 12.4, 8.2), Qt::AlignCenter, "3");
    } else if (actionName == "contacts") {
        painter.drawRoundedRect(QRectF(3.2, 2.5, 11.6, 13.0), 1.8, 1.8);
        painter.drawEllipse(QRectF(7.2, 5.2, 3.6, 3.6));
        painter.drawArc(QRectF(5.5, 9.2, 7.0, 5.0), 15 * 16, 150 * 16);
    } else if (actionName == "privacy_shield") {
        QPainterPath shield;
        shield.moveTo(9.0, 2.5);
        shield.lineTo(14.5, 4.8);
        shield.cubicTo(14.5, 10.2, 12.2, 13.5, 9.0, 15.5);
        shield.cubicTo(5.8, 13.5, 3.5, 10.2, 3.5, 4.8);
        shield.closeSubpath();
        painter.drawPath(shield);
        // Yellow check badge matching Screenshot 1
        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor("#FBBF24"));
        painter.drawEllipse(QRectF(6.5, 6.8, 5.0, 5.0));
        painter.setPen(QPen(QColor("#093282"), 1.2));
        painter.drawLine(QPointF(7.6, 9.3), QPointF(8.6, 10.3));
        painter.drawLine(QPointF(8.6, 10.3), QPointF(10.5, 8.2));
    } else if (actionName == "settings") {
        painter.drawEllipse(QRectF(6.2, 6.2, 5.6, 5.6));
        for (int i = 0; i < 8; ++i) {
            painter.save();
            painter.translate(9.0, 9.0);
            painter.rotate(i * 45.0);
            painter.drawLine(QPointF(0, -5.8), QPointF(0, -3.5));
            painter.restore();
        }
    } else if (actionName == "app_grid") {
        painter.setPen(Qt::NoPen);
        painter.setBrush(color);
        for (int r = 0; r < 3; ++r) {
            for (int c = 0; c < 3; ++c) {
                painter.drawRoundedRect(QRectF(3.5 + c * 4.2, 3.5 + r * 4.2, 2.4, 2.4), 0.6, 0.6);
            }
        }
    } else if (actionName == "eye_off") {
        painter.drawArc(QRectF(2.5, 5.0, 13.0, 8.0), 15 * 16, 150 * 16);
        painter.drawArc(QRectF(2.5, 5.0, 13.0, 8.0), 195 * 16, 150 * 16);
        painter.drawLine(QPointF(3.5, 14.5), QPointF(14.5, 3.5));
    } else if (actionName == "eye_on") {
        painter.drawArc(QRectF(2.5, 5.0, 13.0, 8.0), 15 * 16, 150 * 16);
        painter.drawArc(QRectF(2.5, 5.0, 13.0, 8.0), 195 * 16, 150 * 16);
        painter.drawEllipse(QRectF(7.2, 7.2, 3.6, 3.6));
    } else if (actionName == "folder_move") {
        painter.drawRoundedRect(QRectF(2.5, 5.0, 13.0, 9.0), 1.5, 1.5);
        painter.drawLine(QPointF(2.5, 5.0), QPointF(7.5, 5.0));
        painter.drawLine(QPointF(6.5, 9.5), QPointF(11.5, 9.5));
    } else if (actionName == "add_contact") {
        painter.drawEllipse(QRectF(4.2, 3.5, 5.6, 5.6));
        painter.drawArc(QRectF(2.5, 9.5, 9.0, 6.0), 15 * 16, 150 * 16);
        painter.drawLine(QPointF(13.5, 5.5), QPointF(13.5, 10.5));
        painter.drawLine(QPointF(11.0, 8.0), QPointF(16.0, 8.0));
    } else if (actionName == "add_event") {
        painter.drawRoundedRect(QRectF(2.8, 3.5, 11.5, 11.5), 1.6, 1.6);
        painter.drawLine(QPointF(2.8, 6.8), QPointF(14.3, 6.8));
        painter.drawLine(QPointF(8.5, 8.5), QPointF(8.5, 12.8));
        painter.drawLine(QPointF(6.4, 10.6), QPointF(10.6, 10.6));
    }

    return QIcon(pix);
}

QIcon MailIcons::settingsNavIcon(int navIndex, const QColor &color, int size)
{
    QPixmap pix(size * 2, size * 2);
    pix.setDevicePixelRatio(2.0);
    pix.fill(Qt::transparent);

    QPainter painter(&pix);
    painter.setRenderHint(QPainter::Antialiasing, true);
    QPen pen(color, 1.35, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
    painter.setPen(pen);
    painter.setBrush(Qt::NoBrush);

    switch (navIndex) {
    case 0: // General (Monitor / Window)
        painter.drawRoundedRect(QRectF(2.5, 3.0, 13.0, 9.2), 1.6, 1.6);
        painter.drawLine(QPointF(9.0, 12.2), QPointF(9.0, 15.0));
        painter.drawLine(QPointF(5.8, 15.0), QPointF(12.2, 15.0));
        break;
    case 1: // Accounts (Person)
        painter.drawEllipse(QRectF(6.2, 3.0, 5.6, 5.6));
        painter.drawArc(QRectF(3.5, 9.5, 11.0, 7.0), 15 * 16, 150 * 16);
        break;
    case 2: // Appearance (Paint roller)
        painter.drawRoundedRect(QRectF(3.0, 2.8, 9.5, 4.4), 1.2, 1.2);
        painter.drawLine(QPointF(12.5, 5.0), QPointF(14.5, 5.0));
        painter.drawLine(QPointF(14.5, 5.0), QPointF(14.5, 8.8));
        painter.drawLine(QPointF(14.5, 8.8), QPointF(8.5, 8.8));
        painter.drawLine(QPointF(8.5, 8.8), QPointF(8.5, 11.2));
        painter.drawRoundedRect(QRectF(7.2, 11.2, 2.6, 4.2), 1.0, 1.0);
        break;
    case 3: { // Privacy hub (Shield)
        QPainterPath shield;
        shield.moveTo(9.0, 2.5);
        shield.lineTo(14.2, 4.6);
        shield.cubicTo(14.2, 10.0, 12.0, 13.4, 9.0, 15.5);
        shield.cubicTo(6.0, 13.4, 3.8, 10.0, 3.8, 4.6);
        shield.closeSubpath();
        painter.drawPath(shield);
        painter.drawEllipse(QRectF(7.2, 7.2, 3.6, 3.6));
        break;
    }
    case 4: // Folders
        painter.drawRoundedRect(QRectF(2.5, 5.0, 13.0, 9.5), 1.6, 1.6);
        painter.drawLine(QPointF(2.5, 5.0), QPointF(7.5, 5.0));
        painter.drawLine(QPointF(2.5, 3.6), QPointF(6.5, 3.6));
        break;
    case 5: // Filters (Sliders)
        painter.drawLine(QPointF(3.0, 5.0), QPointF(15.0, 5.0));
        painter.drawLine(QPointF(3.0, 9.0), QPointF(15.0, 9.0));
        painter.drawLine(QPointF(3.0, 13.0), QPointF(15.0, 13.0));
        painter.setBrush(QColor("#FFFFFF"));
        painter.drawEllipse(QRectF(5.5, 3.5, 3.0, 3.0));
        painter.drawEllipse(QRectF(10.0, 7.5, 3.0, 3.0));
        painter.drawEllipse(QRectF(6.5, 11.5, 3.0, 3.0));
        break;
    case 6: { // Reading & Composing (Mail / 'T' inside frame)
        painter.drawLine(QPointF(3.0, 5.5), QPointF(3.0, 3.0));
        painter.drawLine(QPointF(3.0, 3.0), QPointF(5.5, 3.0));
        painter.drawLine(QPointF(12.5, 3.0), QPointF(15.0, 3.0));
        painter.drawLine(QPointF(15.0, 3.0), QPointF(15.0, 5.5));
        painter.drawLine(QPointF(3.0, 12.5), QPointF(3.0, 15.0));
        painter.drawLine(QPointF(3.0, 15.0), QPointF(5.5, 15.0));
        painter.drawLine(QPointF(12.5, 15.0), QPointF(15.0, 15.0));
        painter.drawLine(QPointF(15.0, 15.0), QPointF(15.0, 12.5));
        painter.drawLine(QPointF(6.2, 6.2), QPointF(11.8, 6.2));
        painter.drawLine(QPointF(9.0, 6.2), QPointF(9.0, 12.2));
        break;
    }
    case 7: // Calendar
        painter.drawRoundedRect(QRectF(2.8, 3.8, 12.4, 11.4), 1.6, 1.6);
        painter.drawLine(QPointF(2.8, 7.0), QPointF(15.2, 7.0));
        painter.drawLine(QPointF(6.0, 2.4), QPointF(6.0, 4.8));
        painter.drawLine(QPointF(12.0, 2.4), QPointF(12.0, 4.8));
        break;
    case 8: // Signatures (Pen)
        painter.drawLine(QPointF(4.0, 14.0), QPointF(13.8, 4.2));
        painter.drawLine(QPointF(11.5, 3.2), QPointF(14.8, 6.5));
        painter.drawLine(QPointF(3.2, 14.8), QPointF(14.8, 14.8));
        break;
    case 9: // Snippets (Document)
        painter.drawRoundedRect(QRectF(3.8, 2.5, 10.4, 13.0), 1.4, 1.4);
        painter.drawLine(QPointF(6.2, 6.5), QPointF(11.8, 6.5));
        painter.drawLine(QPointF(6.2, 9.5), QPointF(11.8, 9.5));
        painter.drawLine(QPointF(6.2, 12.5), QPointF(9.6, 12.5));
        break;
    case 10: // Shortcuts (Keyboard)
        painter.drawRoundedRect(QRectF(2.2, 4.5, 13.6, 9.0), 1.5, 1.5);
        painter.drawLine(QPointF(5.0, 7.5), QPointF(6.2, 7.5));
        painter.drawLine(QPointF(8.4, 7.5), QPointF(9.6, 7.5));
        painter.drawLine(QPointF(11.8, 7.5), QPointF(13.0, 7.5));
        painter.drawLine(QPointF(5.8, 10.8), QPointF(12.2, 10.8));
        break;
    case 11: // Advanced (Gear)
        painter.drawEllipse(QRectF(6.2, 6.2, 5.6, 5.6));
        for (int i = 0; i < 8; ++i) {
            painter.save();
            painter.translate(9.0, 9.0);
            painter.rotate(i * 45.0);
            painter.drawLine(QPointF(0, -5.8), QPointF(0, -3.6));
            painter.restore();
        }
        break;
    case 12: { // AI & MCP (Gemini / Codex Sparkles)
        QPainterPath sp;
        sp.moveTo(9.0, 2.5);
        sp.quadTo(9.0, 8.5, 15.0, 8.5);
        sp.quadTo(9.0, 8.5, 9.0, 14.5);
        sp.quadTo(9.0, 8.5, 3.0, 8.5);
        sp.quadTo(9.0, 8.5, 9.0, 2.5);
        painter.drawPath(sp);
        break;
    }
    case 13: // Help & User Guide (Question mark inside circle)
    default: {
        painter.drawEllipse(QRectF(2.6, 2.6, 12.8, 12.8));
        QPainterPath qPath;
        qPath.moveTo(7.3, 7.0);
        qPath.cubicTo(7.3, 5.2, 10.7, 5.2, 10.7, 7.1);
        qPath.cubicTo(10.7, 8.3, 9.0, 8.6, 9.0, 9.8);
        painter.drawPath(qPath);
        painter.setBrush(color);
        painter.setPen(Qt::NoPen);
        painter.drawEllipse(QRectF(8.25, 11.4, 1.5, 1.5));
        break;
    }
    }

    return QIcon(pix);
}

QPixmap MailIcons::avatarPresetPixmap(int presetIndex, const AccountItem &acc, int size, bool selectedBorder)
{
    QPixmap pix(size * 2, size * 2);
    pix.setDevicePixelRatio(2.0);
    pix.fill(Qt::transparent);

    QPainter p(&pix);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setRenderHint(QPainter::SmoothPixmapTransform, true);

    QRectF outer(1.2, 1.2, size - 2.4, size - 2.4);
    QRectF r(3.2, 3.2, size - 6.4, size - 6.4);

    if (selectedBorder) {
        p.setPen(QPen(QColor("#0D47A1"), 2.0));
        p.setBrush(Qt::NoBrush);
        p.drawRoundedRect(outer, 7.5, 7.5);
    } else {
        p.setPen(QPen(QColor("#E5E7EB"), 1.0));
        p.setBrush(QColor("#F9FAFB"));
        p.drawRoundedRect(outer, 7.0, 7.0);
    }

    // Preset 0: Custom uploaded image / Domain Favicon / Orbit ring (Mailbird top-left icon)
    if (presetIndex == 0) {
        if (!acc.avatarPngBase64.trimmed().isEmpty()) {
            QPixmap customImg;
            if (customImg.loadFromData(QByteArray::fromBase64(acc.avatarPngBase64.toLatin1()))) {
                QPainterPath clip;
                clip.addRoundedRect(r, 6.0, 6.0);
                p.save();
                p.setClipPath(clip);
                p.drawPixmap(r.toRect(), customImg.scaled(r.width() * 2, r.height() * 2, Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation));
                p.restore();
                return pix;
            }
        }
        // Fallback: Solluna Official Solar+Luna Orbit Icon
        p.drawPixmap(r.toRect(), sollunaAppPixmap(int(r.width()), 0));
        return pix;
    }

    // Preset 1: Photo avatar (Google profile photo or warm photo silhouette)
    if (presetIndex == 1) {
        if (!acc.avatarPngBase64.trimmed().isEmpty()) {
            QPixmap customImg;
            if (customImg.loadFromData(QByteArray::fromBase64(acc.avatarPngBase64.toLatin1()))) {
                QPainterPath clip;
                clip.addRoundedRect(r, 6.0, 6.0);
                p.save();
                p.setClipPath(clip);
                p.drawPixmap(r.toRect(), customImg.scaled(r.width() * 2, r.height() * 2, Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation));
                p.restore();
                return pix;
            }
        }
        p.setPen(Qt::NoPen);
        QLinearGradient g(r.topLeft(), r.bottomRight());
        g.setColorAt(0.0, QColor("#D6C7B2"));
        g.setColorAt(1.0, QColor("#8D775F"));
        p.setBrush(g);
        p.drawRoundedRect(r, 6.0, 6.0);
        p.setBrush(QColor("#4A3B2C"));
        p.drawEllipse(QRectF(size * 0.35, size * 0.25, size * 0.30, size * 0.30));
        p.drawChord(QRectF(size * 0.22, size * 0.54, size * 0.56, size * 0.40), 0, 180 * 16);
        return pix;
    }

    // Preset 2: Blue square Initials badge ('S' or account initial)
    if (presetIndex == 2) {
        p.setPen(Qt::NoPen);
        p.setBrush(acc.avatarColor.isValid() ? acc.avatarColor : QColor("#0284C7"));
        p.drawRoundedRect(r, 6.0, 6.0);
        p.setPen(QColor("#FFFFFF"));
        QFont f("Segoe UI", int(size * 0.34), QFont::Bold);
        p.setFont(f);
        QString init = acc.initials.trimmed().isEmpty() ? QStringLiteral("S") : acc.initials.left(1).toUpper();
        p.drawText(r, Qt::AlignCenter, init);
        return pix;
    }

    // Presets 3..10: Row 1 Brand & Work Icons (G, y!, Office, Outlook, Exchange, Apple, @, Briefcase)
    p.setPen(QPen(QColor("#111827"), 1.5, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    p.setBrush(Qt::NoBrush);

    switch (presetIndex) {
    case 3: { // 'G' (Google)
        QFont f("Segoe UI", int(size * 0.36), QFont::Bold);
        p.setFont(f);
        p.drawText(r, Qt::AlignCenter, "G");
        break;
    }
    case 4: { // 'y!' (Yahoo)
        QFont f("Segoe UI", int(size * 0.32), QFont::Bold);
        p.setFont(f);
        p.drawText(r, Qt::AlignCenter, "y!");
        break;
    }
    case 5: { // Office Isometric Box
        QPolygonF poly;
        poly << QPointF(size * 0.34, size * 0.32)
             << QPointF(size * 0.56, size * 0.23)
             << QPointF(size * 0.70, size * 0.30)
             << QPointF(size * 0.70, size * 0.70)
             << QPointF(size * 0.56, size * 0.77)
             << QPointF(size * 0.34, size * 0.66);
        p.drawPolygon(poly);
        p.drawLine(QPointF(size * 0.56, size * 0.23), QPointF(size * 0.56, size * 0.77));
        break;
    }
    case 6: { // Outlook 'O' + Mail card
        p.drawRoundedRect(QRectF(size * 0.24, size * 0.30, size * 0.28, size * 0.40), 2.0, 2.0);
        p.drawRoundedRect(QRectF(size * 0.44, size * 0.34, size * 0.32, size * 0.32), 2.0, 2.0);
        QFont f("Segoe UI", int(size * 0.19), QFont::Bold);
        p.setFont(f);
        p.drawText(QRectF(size * 0.24, size * 0.30, size * 0.28, size * 0.40), Qt::AlignCenter, "O");
        break;
    }
    case 7: { // Exchange 'E' + Mail card
        p.drawRoundedRect(QRectF(size * 0.24, size * 0.30, size * 0.28, size * 0.40), 2.0, 2.0);
        p.drawRoundedRect(QRectF(size * 0.44, size * 0.34, size * 0.32, size * 0.32), 2.0, 2.0);
        QFont f("Segoe UI", int(size * 0.19), QFont::Bold);
        p.setFont(f);
        p.drawText(QRectF(size * 0.24, size * 0.30, size * 0.28, size * 0.40), Qt::AlignCenter, "E");
        break;
    }
    case 8: { // Apple silhouette
        p.setBrush(QColor("#111827"));
        p.drawEllipse(QRectF(size * 0.31, size * 0.36, size * 0.22, size * 0.32));
        p.drawEllipse(QRectF(size * 0.45, size * 0.36, size * 0.22, size * 0.32));
        QPainterPath leaf;
        leaf.moveTo(size * 0.50, size * 0.33);
        leaf.quadTo(size * 0.54, size * 0.22, size * 0.62, size * 0.24);
        leaf.quadTo(size * 0.58, size * 0.33, size * 0.50, size * 0.33);
        p.drawPath(leaf);
        break;
    }
    case 9: { // '@' Symbol
        QFont f("Segoe UI", int(size * 0.36), QFont::Bold);
        p.setFont(f);
        p.drawText(r, Qt::AlignCenter, "@");
        break;
    }
    case 10: { // Briefcase
        p.drawRoundedRect(QRectF(size * 0.24, size * 0.36, size * 0.52, size * 0.38), 2.2, 2.2);
        p.drawRoundedRect(QRectF(size * 0.38, size * 0.26, size * 0.24, size * 0.10), 1.5, 1.5);
        p.drawLine(QPointF(size * 0.24, size * 0.52), QPointF(size * 0.76, size * 0.52));
        break;
    }
    case 11: { // Row 2 Col 1: Couch / Sofa
        p.drawRoundedRect(QRectF(size * 0.26, size * 0.34, size * 0.48, size * 0.24), 2.5, 2.5);
        p.drawRoundedRect(QRectF(size * 0.20, size * 0.46, size * 0.60, size * 0.22), 2.0, 2.0);
        p.drawLine(QPointF(size * 0.26, size * 0.68), QPointF(size * 0.26, size * 0.75));
        p.drawLine(QPointF(size * 0.74, size * 0.68), QPointF(size * 0.74, size * 0.75));
        break;
    }
    case 12: { // Row 2 Col 2: Calendar
        p.drawRoundedRect(QRectF(size * 0.25, size * 0.28, size * 0.50, size * 0.46), 2.2, 2.2);
        p.drawLine(QPointF(size * 0.25, size * 0.42), QPointF(size * 0.75, size * 0.42));
        p.drawLine(QPointF(size * 0.37, size * 0.22), QPointF(size * 0.37, size * 0.32));
        p.drawLine(QPointF(size * 0.63, size * 0.22), QPointF(size * 0.63, size * 0.32));
        break;
    }
    case 13: { // Row 2 Col 3: Inbox Tray
        p.drawRoundedRect(QRectF(size * 0.24, size * 0.28, size * 0.52, size * 0.46), 2.2, 2.2);
        p.drawLine(QPointF(size * 0.24, size * 0.55), QPointF(size * 0.40, size * 0.55));
        p.drawLine(QPointF(size * 0.60, size * 0.55), QPointF(size * 0.76, size * 0.55));
        p.drawArc(QRectF(size * 0.40, size * 0.47, size * 0.20, size * 0.16), 180 * 16, 180 * 16);
        break;
    }
    case 14: { // Row 2 Col 4: VR / Person with Goggles
        p.drawEllipse(QRectF(size * 0.34, size * 0.24, size * 0.32, size * 0.32));
        p.drawRoundedRect(QRectF(size * 0.32, size * 0.33, size * 0.36, size * 0.12), 1.5, 1.5);
        p.drawArc(QRectF(size * 0.26, size * 0.56, size * 0.48, size * 0.28), 0, 180 * 16);
        break;
    }
    case 15: { // Row 2 Col 5: Clock + Coffee
        p.drawEllipse(QRectF(size * 0.28, size * 0.26, size * 0.44, size * 0.44));
        p.drawLine(QPointF(size * 0.50, size * 0.36), QPointF(size * 0.50, size * 0.48));
        p.drawLine(QPointF(size * 0.50, size * 0.48), QPointF(size * 0.60, size * 0.48));
        break;
    }
    case 16: { // Row 2 Col 6: Laptop + Rocket
        p.drawRoundedRect(QRectF(size * 0.26, size * 0.30, size * 0.48, size * 0.34), 2.0, 2.0);
        p.drawLine(QPointF(size * 0.20, size * 0.68), QPointF(size * 0.80, size * 0.68));
        p.drawLine(QPointF(size * 0.50, size * 0.36), QPointF(size * 0.50, size * 0.56));
        break;
    }
    case 17: { // Row 2 Col 7: Chat Bubble
        p.drawRoundedRect(QRectF(size * 0.24, size * 0.28, size * 0.52, size * 0.38), 4.0, 4.0);
        p.drawLine(QPointF(size * 0.36, size * 0.42), QPointF(size * 0.64, size * 0.42));
        p.drawLine(QPointF(size * 0.36, size * 0.52), QPointF(size * 0.54, size * 0.52));
        break;
    }
    case 18: { // Row 2 Col 8: Shirt & Tie
        p.drawPolygon(QPolygonF() << QPointF(size * 0.34, size * 0.26)
                                  << QPointF(size * 0.50, size * 0.38)
                                  << QPointF(size * 0.66, size * 0.26));
        p.drawPolygon(QPolygonF() << QPointF(size * 0.50, size * 0.38)
                                  << QPointF(size * 0.43, size * 0.64)
                                  << QPointF(size * 0.50, size * 0.74)
                                  << QPointF(size * 0.57, size * 0.64));
        break;
    }
    case 19: { // Row 2 Col 9: Office Desk & Chair
        p.drawLine(QPointF(size * 0.22, size * 0.48), QPointF(size * 0.78, size * 0.48));
        p.drawLine(QPointF(size * 0.28, size * 0.48), QPointF(size * 0.28, size * 0.74));
        p.drawLine(QPointF(size * 0.72, size * 0.48), QPointF(size * 0.72, size * 0.74));
        p.drawRoundedRect(QRectF(size * 0.40, size * 0.30, size * 0.20, size * 0.18), 2.0, 2.0);
        break;
    }
    case 20: { // Row 2 Col 10: Graduation Cap
        p.drawPolygon(QPolygonF() << QPointF(size * 0.50, size * 0.26)
                                  << QPointF(size * 0.78, size * 0.40)
                                  << QPointF(size * 0.50, size * 0.54)
                                  << QPointF(size * 0.22, size * 0.40));
        p.drawArc(QRectF(size * 0.34, size * 0.46, size * 0.32, size * 0.22), 180 * 16, 180 * 16);
        break;
    }
    case 21: { // Row 2 Col 11: Office Building
        p.drawRect(QRectF(size * 0.28, size * 0.24, size * 0.28, size * 0.50));
        p.drawRect(QRectF(size * 0.56, size * 0.38, size * 0.18, size * 0.36));
        p.drawLine(QPointF(size * 0.36, size * 0.34), QPointF(size * 0.48, size * 0.34));
        p.drawLine(QPointF(size * 0.36, size * 0.46), QPointF(size * 0.48, size * 0.46));
        p.drawLine(QPointF(size * 0.36, size * 0.58), QPointF(size * 0.48, size * 0.58));
        break;
    }
    case 22: { // Row 3 Col 1: Puzzle Piece
        p.drawRoundedRect(QRectF(size * 0.28, size * 0.28, size * 0.44, size * 0.44), 3.0, 3.0);
        p.drawLine(QPointF(size * 0.50, size * 0.28), QPointF(size * 0.50, size * 0.72));
        p.drawLine(QPointF(size * 0.28, size * 0.50), QPointF(size * 0.72, size * 0.50));
        break;
    }
    case 23: { // Row 3 Col 2: Code Laptop '</>'
        p.drawRoundedRect(QRectF(size * 0.24, size * 0.28, size * 0.52, size * 0.36), 2.0, 2.0);
        p.drawLine(QPointF(size * 0.18, size * 0.68), QPointF(size * 0.82, size * 0.68));
        QFont f("Consolas", int(size * 0.17), QFont::Bold);
        p.setFont(f);
        p.drawText(QRectF(size * 0.24, size * 0.28, size * 0.52, size * 0.36), Qt::AlignCenter, "</>");
        break;
    }
    case 24: { // Row 3 Col 3: Pencil & Ruler (Stationery)
        p.drawRect(QRectF(size * 0.30, size * 0.26, size * 0.14, size * 0.48));
        p.drawRect(QRectF(size * 0.54, size * 0.26, size * 0.16, size * 0.48));
        break;
    }
    case 25: { // Row 3 Col 4: Tea / Coffee Cup
        p.drawRoundedRect(QRectF(size * 0.26, size * 0.38, size * 0.38, size * 0.28), 3.0, 3.0);
        p.drawArc(QRectF(size * 0.58, size * 0.42, size * 0.16, size * 0.18), -90 * 16, 180 * 16);
        p.drawLine(QPointF(size * 0.22, size * 0.70), QPointF(size * 0.72, size * 0.70));
        break;
    }
    case 26: { // Row 3 Col 5: Heart Letter / Envelope
        p.drawRoundedRect(QRectF(size * 0.24, size * 0.32, size * 0.52, size * 0.40), 2.0, 2.0);
        p.drawLine(QPointF(size * 0.24, size * 0.34), QPointF(size * 0.50, size * 0.54));
        p.drawLine(QPointF(size * 0.76, size * 0.34), QPointF(size * 0.50, size * 0.54));
        break;
    }
    case 27: { // Row 3 Col 6: Armchair & Floor Lamp
        p.drawRoundedRect(QRectF(size * 0.32, size * 0.42, size * 0.38, size * 0.28), 2.5, 2.5);
        p.drawLine(QPointF(size * 0.24, size * 0.26), QPointF(size * 0.24, size * 0.72));
        p.drawPolygon(QPolygonF() << QPointF(size * 0.18, size * 0.34)
                                  << QPointF(size * 0.30, size * 0.34)
                                  << QPointF(size * 0.27, size * 0.24)
                                  << QPointF(size * 0.21, size * 0.24));
        break;
    }
    case 28: { // Row 3 Col 7: Bar Chart
        p.drawLine(QPointF(size * 0.24, size * 0.72), QPointF(size * 0.76, size * 0.72));
        p.drawRect(QRectF(size * 0.28, size * 0.52, size * 0.10, size * 0.20));
        p.drawRect(QRectF(size * 0.45, size * 0.40, size * 0.10, size * 0.32));
        p.drawRect(QRectF(size * 0.62, size * 0.28, size * 0.10, size * 0.44));
        break;
    }
    case 29: { // Row 3 Col 8: Globe
        p.drawEllipse(QRectF(size * 0.24, size * 0.24, size * 0.48, size * 0.48));
        p.drawArc(QRectF(size * 0.20, size * 0.20, size * 0.56, size * 0.56), -60 * 16, 180 * 16);
        p.drawLine(QPointF(size * 0.36, size * 0.76), QPointF(size * 0.60, size * 0.76));
        break;
    }
    case 30: { // Row 3 Col 9: Nature / Planet
        p.drawEllipse(QRectF(size * 0.26, size * 0.26, size * 0.48, size * 0.48));
        p.drawLine(QPointF(size * 0.26, size * 0.52), QPointF(size * 0.74, size * 0.52));
        p.drawPolygon(QPolygonF() << QPointF(size * 0.42, size * 0.52)
                                  << QPointF(size * 0.50, size * 0.32)
                                  << QPointF(size * 0.58, size * 0.52));
        break;
    }
    case 31: { // Row 3 Col 10: Robot
        p.drawRoundedRect(QRectF(size * 0.28, size * 0.34, size * 0.44, size * 0.36), 3.0, 3.0);
        p.drawLine(QPointF(size * 0.50, size * 0.22), QPointF(size * 0.50, size * 0.34));
        p.drawEllipse(QRectF(size * 0.36, size * 0.44, size * 0.08, size * 0.08));
        p.drawEllipse(QRectF(size * 0.56, size * 0.44, size * 0.08, size * 0.08));
        p.drawLine(QPointF(size * 0.38, size * 0.60), QPointF(size * 0.62, size * 0.60));
        break;
    }
    case 32: // Row 3 Col 11: Lotus Flower
    default: {
        QPainterPath lotus;
        lotus.moveTo(size * 0.50, size * 0.28);
        lotus.quadTo(size * 0.66, size * 0.46, size * 0.50, size * 0.68);
        lotus.quadTo(size * 0.34, size * 0.46, size * 0.50, size * 0.28);
        p.drawPath(lotus);
        p.drawArc(QRectF(size * 0.24, size * 0.44, size * 0.52, size * 0.26), 180 * 16, 180 * 16);
        break;
    }
    }

    return pix;
}

QPixmap MailIcons::accountAvatarPixmap(const AccountItem &acc, int size, bool activeIndicator, int unreadCount)
{
    auto drawTopRightUnreadBadge = [size, unreadCount](QPixmap &targetPix) {
        if (unreadCount <= 0)
            return;
        QPainter bp(&targetPix);
        bp.setRenderHint(QPainter::Antialiasing, true);
        const QString badgeText = (unreadCount > 99) ? QStringLiteral("99+") : QString::number(unreadCount);
        const qreal badgeH = 14.0;
        const qreal badgeW = (badgeText.length() >= 3) ? 20.0 : ((badgeText.length() == 2) ? 16.5 : 14.0);
        const QRectF badgeRect(size - badgeW - 0.5, 0.5, badgeW, badgeH);

        bp.setPen(QPen(QColor("#08307E"), 1.2));
        bp.setBrush(QColor("#3B6AD9"));
        bp.drawRoundedRect(badgeRect, badgeH * 0.5, badgeH * 0.5);

        QFont bf("Segoe UI", 7, QFont::Bold);
        bf.setPixelSize(9);
        bp.setFont(bf);
        bp.setPen(QColor("#FFFFFF"));
        bp.drawText(badgeRect.adjusted(0, -0.5, 0, -0.5), Qt::AlignCenter, badgeText);
    };

    if (acc.provider != AccountProviderType::Unified && acc.avatarIconPreset > 0) {
        QPixmap presetPix = avatarPresetPixmap(acc.avatarIconPreset, acc, size, activeIndicator);
        drawTopRightUnreadBadge(presetPix);
        return presetPix;
    }

    QPixmap pix(size * 2, size * 2);
    pix.setDevicePixelRatio(2.0);
    pix.fill(Qt::transparent);

    QPainter p(&pix);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setRenderHint(QPainter::SmoothPixmapTransform, true);

    QRectF r(2.5, 2.5, size - 5.0, size - 5.0);

    // 1. Google 프로필 사진 또는 일반 회사 도메인 파비콘 이미지가 캐시되어 있는 경우 직관적으로 표시 (Mailbird 스타일)
    if (acc.provider != AccountProviderType::Unified && !acc.avatarPngBase64.trimmed().isEmpty()) {
        QPixmap customImg;
        if (customImg.loadFromData(QByteArray::fromBase64(acc.avatarPngBase64.toLatin1()))) {
            if (activeIndicator) {
                p.setPen(QPen(QColor("#60A5FA"), 1.8));
                p.setBrush(Qt::NoBrush);
                p.drawRoundedRect(QRectF(1.0, 1.0, size - 2.0, size - 2.0), 8.0, 8.0);
            }

            if (acc.provider == AccountProviderType::Google) {
                // Google 계정: 원형/둥근사각형 꽉 찬 프로필 사진 렌더링
                QPainterPath clipPath;
                clipPath.addRoundedRect(r, 7.0, 7.0);
                p.save();
                p.setClipPath(clipPath);
                p.drawPixmap(r.toRect(), customImg.scaled(size * 2, size * 2, Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation));
                p.restore();
            } else {
                // 일반 회사 / 도메인 계정: 깔끔한 흰색 둥근 배지 위에 회사 도메인 파비콘(Favicon) 렌더링
                p.setPen(Qt::NoPen);
                p.setBrush(QColor("#FFFFFF"));
                p.drawRoundedRect(r, 7.0, 7.0);

                QRectF iconRect(5.0, 5.0, size - 10.0, size - 10.0);
                QPainterPath clipPath;
                clipPath.addRoundedRect(iconRect, 4.0, 4.0);
                p.save();
                p.setClipPath(clipPath);
                p.drawPixmap(iconRect.toRect(), customImg.scaled((size - 10) * 2, (size - 10) * 2, Qt::KeepAspectRatio, Qt::SmoothTransformation));
                p.restore();
            }
            p.end();
            drawTopRightUnreadBadge(pix);
            return pix;
        }
    }

    if (activeIndicator) {
        p.setPen(QPen(QColor("#60A5FA"), 1.8));
        p.setBrush(Qt::NoBrush);
        p.drawRoundedRect(QRectF(1.0, 1.0, size - 2.0, size - 2.0), 8.0, 8.0);
    }

    switch (acc.visualStyle) {
    case AvatarVisualStyle::UnifiedEnvelope: {
        p.setPen(Qt::NoPen);
        p.setBrush(QColor("#FFFFFF"));
        p.drawRoundedRect(r, 7.0, 7.0);
        // Black mail badge inside white box (exact match to Screenshot 1 top icon)
        p.setBrush(QColor("#111827"));
        QRectF env(7.0, 10.0, size - 14.0, size - 20.0);
        p.drawRoundedRect(env, 2.0, 2.0);
        p.setPen(QPen(QColor("#FFFFFF"), 1.5));
        p.drawLine(QPointF(env.left() + 1, env.top() + 1), QPointF(env.center().x(), env.center().y() + 1));
        p.drawLine(QPointF(env.center().x(), env.center().y() + 1), QPointF(env.right() - 1, env.top() + 1));
        break;
    }
    case AvatarVisualStyle::GoogleOrbit: {
        p.drawPixmap(r.toRect(), sollunaAppPixmap(int(r.width()), 0));
        break;
    }
    case AvatarVisualStyle::PhotoCircle: {
        p.setPen(Qt::NoPen);
        QLinearGradient g(r.topLeft(), r.bottomRight());
        g.setColorAt(0.0, QColor("#D6C7B2"));
        g.setColorAt(1.0, QColor("#8D775F"));
        p.setBrush(g);
        p.drawRoundedRect(r, 7.0, 7.0);
        p.setBrush(QColor("#4A3B2C"));
        p.drawEllipse(QRectF(size * 0.34, size * 0.24, size * 0.32, size * 0.32));
        p.drawChord(QRectF(size * 0.20, size * 0.54, size * 0.60, size * 0.45), 0, 180 * 16);
        break;
    }
    case AvatarVisualStyle::BlueFolderBadge: {
        p.setPen(Qt::NoPen);
        p.setBrush(QColor("#1649A4"));
        p.drawRoundedRect(r, 7.0, 7.0);
        p.setPen(QPen(QColor("#FFFFFF"), 1.5));
        p.setBrush(Qt::NoBrush);
        p.drawRoundedRect(QRectF(8.0, 10.0, size - 16.0, size - 18.0), 2.0, 2.0);
        break;
    }
    case AvatarVisualStyle::TealSquareS: {
        p.setPen(Qt::NoPen);
        p.setBrush(acc.avatarColor.isValid() ? acc.avatarColor : QColor("#1282A2"));
        p.drawRoundedRect(r, 7.0, 7.0);
        p.setPen(QColor("#FFFFFF"));
        QFont f("Segoe UI", 11, QFont::Bold);
        p.setFont(f);
        p.drawText(r, Qt::AlignCenter, acc.initials.isEmpty() ? "S" : acc.initials);
        break;
    }
    }

    p.end();
    drawTopRightUnreadBadge(pix);
    return pix;
}

QPixmap MailIcons::senderAvatarPixmap(const MessageItem &msg, int size)
{
    QPixmap pix(size * 2, size * 2);
    pix.setDevicePixelRatio(2.0);
    pix.fill(Qt::transparent);

    QPainter p(&pix);
    p.setRenderHint(QPainter::Antialiasing, true);
    QRectF r(1.5, 1.5, size - 3.0, size - 3.0);

    switch (msg.avatarKind) {
    case SenderAvatarKind::MailbirdAtLogo: {
        p.setPen(Qt::NoPen);
        p.setBrush(QColor("#FFFFFF"));
        p.drawEllipse(r);
        p.setPen(QColor("#0D47A1"));
        QFont f("Segoe UI", 12, QFont::Bold);
        p.setFont(f);
        p.drawText(r, Qt::AlignCenter, "@");
        break;
    }
    case SenderAvatarKind::MailspringRing: {
        p.setPen(Qt::NoPen);
        p.setBrush(QColor("#1E1B4B"));
        p.drawEllipse(r);
        QLinearGradient grad(r.topLeft(), r.bottomRight());
        grad.setColorAt(0.0, QColor("#FB923C"));
        grad.setColorAt(0.6, QColor("#EC4899"));
        grad.setColorAt(1.0, QColor("#38BDF8"));
        p.setPen(QPen(QBrush(grad), 3.0));
        p.drawEllipse(r.adjusted(6.0, 6.0, -6.0, -6.0));
        break;
    }
    case SenderAvatarKind::RootRainbow: {
        p.setPen(Qt::NoPen);
        p.setBrush(QColor("#F3F4F6"));
        p.drawEllipse(r);
        p.setPen(QPen(QColor("#EF4444"), 2.0));
        p.drawArc(r.adjusted(5, 7, -5, -3), 25 * 16, 130 * 16);
        p.setPen(QPen(QColor("#22C55E"), 2.0));
        p.drawArc(r.adjusted(7, 9, -7, -5), 25 * 16, 130 * 16);
        p.setPen(QPen(QColor("#3B82F6"), 2.0));
        p.drawArc(r.adjusted(9, 11, -9, -7), 25 * 16, 130 * 16);
        break;
    }
    case SenderAvatarKind::UserPhoto: {
        p.setPen(Qt::NoPen);
        QLinearGradient g(r.topLeft(), r.bottomRight());
        g.setColorAt(0.0, QColor("#C8B6A6"));
        g.setColorAt(1.0, QColor("#7D6E63"));
        p.setBrush(g);
        p.drawEllipse(r);
        p.setBrush(QColor("#3F332B"));
        p.drawEllipse(QRectF(size * 0.35, size * 0.24, size * 0.30, size * 0.30));
        p.drawChord(QRectF(size * 0.22, size * 0.52, size * 0.56, size * 0.42), 0, 180 * 16);
        break;
    }
    case SenderAvatarKind::InitialsCircle:
    default: {
        p.setPen(Qt::NoPen);
        p.setBrush(msg.senderAvatarBg.isValid() ? msg.senderAvatarBg : QColor("#FCE7F3"));
        p.drawEllipse(r);
        p.setPen(msg.senderAvatarFg.isValid() ? msg.senderAvatarFg : QColor("#DB2777"));
        QFont f("Segoe UI", 9, QFont::Bold);
        p.setFont(f);
        p.drawText(r, Qt::AlignCenter, msg.senderInitials.isEmpty() ? "M" : msg.senderInitials);
        break;
    }
    }

    return pix;
}


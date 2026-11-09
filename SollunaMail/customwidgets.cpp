#include "customwidgets.h"
#include <QPainter>
#include <QPainterPath>
#include <QMouseEvent>
#include <QFontMetrics>
#include <QVBoxLayout>
#include <QScrollArea>
#include <QScrollBar>
#include <QFrame>
#include <QMap>
#include <algorithm>

namespace {

QString shortDowName(int dow)
{
    static const QStringList ko = {"월", "화", "수", "목", "금", "토", "일"};
    static const QStringList en = {"Mon", "Tue", "Wed", "Thu", "Fri", "Sat", "Sun"};
    static const QStringList ja = {"月", "火", "水", "木", "金", "土", "日"};
    int idx = qBound(1, dow, 7) - 1;
    switch (MailI18n::currentLanguage()) {
    case AppLanguage::English:  return en[idx];
    case AppLanguage::Japanese: return ja[idx];
    case AppLanguage::Korean:
    default:                    return ko[idx];
    }
}

QString longDowName(int dow)
{
    static const QStringList ko = {"월요일", "화요일", "수요일", "목요일", "금요일", "토요일", "일요일"};
    static const QStringList en = {"Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday", "Sunday"};
    static const QStringList ja = {"月曜日", "火曜日", "水曜日", "木曜日", "金曜日", "土曜日", "日曜日"};
    int idx = qBound(1, dow, 7) - 1;
    switch (MailI18n::currentLanguage()) {
    case AppLanguage::English:  return en[idx];
    case AppLanguage::Japanese: return ja[idx];
    case AppLanguage::Korean:
    default:                    return ko[idx];
    }
}

QStringList orderedWeekdayNames()
{
    const bool sundayFirst = (MailI18n::calendarWeekStartDay() == 7);
    QStringList list;
    if (sundayFirst) {
        list << shortDowName(7);
        for (int d = 1; d <= 6; ++d)
            list << shortDowName(d);
    } else {
        for (int d = 1; d <= 7; ++d)
            list << shortDowName(d);
    }
    return list;
}

QString formatHourGutterLabel(int h)
{
    if (MailI18n::calendarUse24HourFormat()) {
        return QString("%1:00").arg(h, 2, 10, QChar('0'));
    }
    if (MailI18n::currentLanguage() == AppLanguage::English) {
        if (h == 0) return "12 AM";
        if (h < 12) return QString("%1 AM").arg(h);
        if (h == 12) return "12 PM";
        return QString("%1 PM").arg(h - 12);
    }
    if (h == 0)  return MailI18n::t("12 오전", "12 AM", "午前 0時");
    if (h < 12)  return QString("%1 %2").arg(h).arg(MailI18n::t("오전", "AM", "午前"));
    if (h == 12) return MailI18n::t("12 오후", "12 PM", "午後 12時");
    return QString("%1 %2").arg(h - 12).arg(MailI18n::t("오후", "PM", "午後"));
}

QString formatMarkerBadgeText(const QTime &t)
{
    if (MailI18n::calendarUse24HourFormat()) {
        return QString("%1:%2").arg(t.hour(), 2, 10, QChar('0')).arg(t.minute(), 2, 10, QChar('0'));
    }
    int h12 = (t.hour() == 0) ? 12 : (t.hour() > 12 ? t.hour() - 12 : t.hour());
    return QString("%1:%2").arg(h12).arg(t.minute(), 2, 10, QChar('0'));
}

QString formatSingleClockTime(const QTime &t)
{
    if (!t.isValid())
        return QString();
    if (MailI18n::calendarUse24HourFormat()) {
        return QString("%1:%2").arg(t.hour(), 2, 10, QChar('0')).arg(t.minute(), 2, 10, QChar('0'));
    }
    int h12 = (t.hour() == 0) ? 12 : (t.hour() > 12 ? t.hour() - 12 : t.hour());
    QString ampm = (t.hour() < 12) ? MailI18n::t("오전", "AM", "午前") : MailI18n::t("오후", "PM", "午後");
    return QString("%1:%2 %3").arg(h12).arg(t.minute(), 2, 10, QChar('0')).arg(ampm);
}

QString formatEventTimeRange(const CalendarEventItem &ev)
{
    if (ev.isAllDay)
        return MailI18n::t("종일", "All day", "終日");
    return QString("%1 - %2")
        .arg(formatSingleClockTime(ev.startTime.isValid() ? ev.startTime : QTime(9, 0)),
             formatSingleClockTime(ev.endTime.isValid() ? ev.endTime : QTime(10, 0)));
}

void drawMailEnvelopeStatusIcon(QPainter *p,
                                const QRectF &box,
                                bool isUnread,
                                bool isReplied,
                                bool isForwarded,
                                bool selected)
{
    if (!p)
        return;
    p->save();
    p->setRenderHint(QPainter::Antialiasing, true);

    const qreal ox = box.left();
    const qreal oy = box.top();

    if (isUnread) {
        // 안 읽은 편지: 닫힌 편지 봉투 아이콘
        const QColor envStroke = selected ? QColor("#FFFFFF") : QColor("#2563EB");
        const QColor envFill   = selected ? QColor(255, 255, 255, 38) : QColor("#EFF6FF");
        QRectF envRect(ox + 2.0, oy + 4.0, 14.0, 10.5);
        p->setPen(QPen(envStroke, 1.45, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        p->setBrush(envFill);
        p->drawRoundedRect(envRect, 1.8, 1.8);

        QPainterPath flap;
        flap.moveTo(ox + 2.4, oy + 4.5);
        flap.lineTo(ox + 9.0, oy + 9.8);
        flap.lineTo(ox + 15.6, oy + 4.5);
        p->setBrush(Qt::NoBrush);
        p->drawPath(flap);
    } else {
        // 읽은 편지: 위쪽 덮개가 열린 편지 봉투 아이콘
        const QColor envStroke = selected ? QColor("#DBEAFE") : QColor("#94A3B8");
        const QColor envFill   = selected ? QColor(255, 255, 255, 18) : QColor("#FFFFFF");
        p->setPen(QPen(envStroke, 1.3, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        p->setBrush(envFill);

        // 열린 삼각형 덮개 + 하단 봉투 몸통 외곽
        QPainterPath openEnv;
        openEnv.moveTo(ox + 2.0, oy + 6.2);
        openEnv.lineTo(ox + 9.0, oy + 1.5);
        openEnv.lineTo(ox + 16.0, oy + 6.2);
        openEnv.lineTo(ox + 16.0, oy + 13.8);
        openEnv.quadTo(ox + 16.0, oy + 15.2, ox + 14.6, oy + 15.2);
        openEnv.lineTo(ox + 3.4, oy + 15.2);
        openEnv.quadTo(ox + 2.0, oy + 15.2, ox + 2.0, oy + 13.8);
        openEnv.closeSubpath();
        p->drawPath(openEnv);

        // 봉투 입구 V자 접힘선
        QPainterPath innerV;
        innerV.moveTo(ox + 2.3, oy + 6.4);
        innerV.lineTo(ox + 9.0, oy + 11.0);
        innerV.lineTo(ox + 15.7, oy + 6.4);
        p->setBrush(Qt::NoBrush);
        p->drawPath(innerV);
    }

    // 답장 완료 표시: 봉투 좌측 하단 빨간색 왼쪽 화살표 (⬅)
    if (isReplied) {
        QPainterPath leftArr;
        leftArr.moveTo(ox - 0.5, oy + 13.5); // 화살표 끝점(좌측)
        leftArr.lineTo(ox + 3.8, oy + 9.8);  // 화살촉 위
        leftArr.lineTo(ox + 3.8, oy + 12.0); // 기둥 위쪽
        leftArr.lineTo(ox + 8.2, oy + 12.0); // 기둥 우측 상단
        leftArr.lineTo(ox + 8.2, oy + 15.0); // 기둥 우측 하단
        leftArr.lineTo(ox + 3.8, oy + 15.0); // 기둥 아래쪽
        leftArr.lineTo(ox + 3.8, oy + 17.2); // 화살촉 아래
        leftArr.closeSubpath();

        p->setPen(QPen(selected ? QColor("#0D47A1") : QColor("#FFFFFF"), 1.3, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        p->setBrush(QColor("#FF1F27"));
        p->drawPath(leftArr);
    }

    // 전달 완료 표시: 봉투 우측 하단 파란색 오른쪽 화살표 (➡)
    if (isForwarded) {
        QPainterPath rightArr;
        rightArr.moveTo(ox + 18.5, oy + 13.5); // 화살표 끝점(우측)
        rightArr.lineTo(ox + 14.2, oy + 9.8);  // 화살촉 위
        rightArr.lineTo(ox + 14.2, oy + 12.0); // 기둥 위쪽
        rightArr.lineTo(ox + 9.8,  oy + 12.0); // 기둥 좌측 상단
        rightArr.lineTo(ox + 9.8,  oy + 15.0); // 기둥 좌측 하단
        rightArr.lineTo(ox + 14.2, oy + 15.0); // 기둥 아래쪽
        rightArr.lineTo(ox + 14.2, oy + 17.2); // 화살촉 아래
        rightArr.closeSubpath();

        p->setPen(QPen(selected ? QColor("#0D47A1") : QColor("#FFFFFF"), 1.3, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        p->setBrush(selected ? QColor("#60A5FA") : QColor("#2563EB"));
        p->drawPath(rightArr);
    }

    p->restore();
}

int drawSubjectInlineBadges(QPainter *p, int x, int y, bool isImportant, bool isStarred, bool selected)
{
    if (!p || (!isImportant && !isStarred))
        return 0;

    p->save();
    p->setRenderHint(QPainter::Antialiasing, true);
    int curX = x;

    // 1. 중요 메일 표시 (❗ 선명한 빨간색 느낌표 아이콘)
    if (isImportant) {
        const QColor barFill = selected ? QColor("#FF5252") : QColor("#EF4444");
        const QColor outline = selected ? QColor("#FFFFFF") : QColor("#DC2626");
        p->setPen(QPen(outline, selected ? 0.7 : 0.4));
        p->setBrush(barFill);
        p->drawRoundedRect(QRectF(curX + 2.0, y + 3.0, 2.8, 8.2), 1.3, 1.3);
        p->drawEllipse(QRectF(curX + 1.9, y + 12.6, 3.0, 3.0));
        curX += 10;
    }

    // 2. 별표 표시 (⭐ 선명한 황금색 5각 별 아이콘)
    if (isStarred) {
        const double cx = curX + 6.5;
        const double cy = y + 9.2;
        const double rOuter = 6.2;
        const double rInner = 2.6;
        QPainterPath starPath;
        for (int i = 0; i < 10; ++i) {
            const double angleDeg = -90.0 + i * 36.0;
            const double rad = angleDeg * 3.14159265358979323846 / 180.0;
            const double r = (i % 2 == 0) ? rOuter : rInner;
            const QPointF pt(cx + r * std::cos(rad), cy + r * std::sin(rad));
            if (i == 0)
                starPath.moveTo(pt);
            else
                starPath.lineTo(pt);
        }
        starPath.closeSubpath();
        p->setPen(QPen(selected ? QColor("#FEF08A") : QColor("#D97706"), 1.0, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        p->setBrush(QColor("#FBBF24"));
        p->drawPath(starPath);
        curX += 16;
    }

    p->restore();
    return curX - x;
}

} // namespace

// ============================================================================
// 1. ToggleSwitchWidget
// ============================================================================
ToggleSwitchWidget::ToggleSwitchWidget(bool checked, QWidget *parent)
    : QWidget(parent), m_checked(checked)
{
    setFixedSize(40, 22);
    setCursor(Qt::PointingHandCursor);
}

void ToggleSwitchWidget::setChecked(bool checked)
{
    if (m_checked != checked) {
        m_checked = checked;
        update();
    }
}

void ToggleSwitchWidget::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);

    QRectF trackRect(1.5, 2.0, width() - 3.0, height() - 4.0);
    qreal radius = trackRect.height() / 2.0;

    if (m_checked) {
        p.setPen(Qt::NoPen);
        p.setBrush(QColor("#1B4EA3")); // Privacy Hub toggle blue
        p.drawRoundedRect(trackRect, radius, radius);

        p.setBrush(QColor("#FFFFFF"));
        qreal knobD = trackRect.height() - 6.0;
        QRectF knobRect(trackRect.right() - knobD - 3.0, trackRect.top() + 3.0, knobD, knobD);
        p.drawEllipse(knobRect);
    } else {
        p.setPen(QPen(QColor("#6B7280"), 1.5));
        p.setBrush(QColor("#FFFFFF"));
        p.drawRoundedRect(trackRect, radius, radius);

        p.setPen(Qt::NoPen);
        p.setBrush(QColor("#6B7280"));
        qreal knobD = trackRect.height() - 6.0;
        QRectF knobRect(trackRect.left() + 3.0, trackRect.top() + 3.0, knobD, knobD);
        p.drawEllipse(knobRect);
    }
}

void ToggleSwitchWidget::mousePressEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton) {
        m_checked = !m_checked;
        update();
        emit toggled(m_checked);
    }
}

// ============================================================================
// 2. ConversationCardWidget
// ============================================================================
ConversationCardWidget::ConversationCardWidget(const ConversationItem &conv,
                                               const MailThemePalette &palette,
                                               bool isSelected,
                                               bool privacyBlur,
                                               QWidget *parent)
    : QWidget(parent),
      m_conv(conv),
      m_palette(palette),
      m_selected(isSelected),
      m_privacyBlur(privacyBlur)
{
    setFixedHeight(78);
    setCursor(Qt::PointingHandCursor);
}

void ConversationCardWidget::setSelectedState(bool selected)
{
    if (m_selected != selected) {
        m_selected = selected;
        update();
    }
}

void ConversationCardWidget::setPaletteState(const MailThemePalette &palette)
{
    m_palette = palette;
    update();
}

void ConversationCardWidget::setPrivacyBlur(bool blur)
{
    m_privacyBlur = blur;
    update();
}

void ConversationCardWidget::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);

    QRectF cardRect(4.0, 2.0, width() - 8.0, height() - 4.0);
    if (m_selected) {
        p.setPen(Qt::NoPen);
        p.setBrush(m_palette.selectedRowBg); // #0D47A1 Royal Blue
        p.drawRoundedRect(cardRect, 4.0, 4.0);
    } else {
        p.setPen(Qt::NoPen);
        p.setBrush(QColor("#FFFFFF"));
        p.drawRect(rect());
        p.setPen(QPen(QColor("#F1F5F9"), 1.0));
        p.drawLine(16, height() - 1, width() - 12, height() - 1);
    }

    if (m_conv.messages.isEmpty())
        return;

    const MessageItem &msg = m_conv.latestMessage();

    // Left Avatar (x=14, y=15, 28x28) + Envelope Status Icon below avatar (x=19, y=47, 18x18)
    QPixmap avatarPix = MailIcons::senderAvatarPixmap(msg, 28);
    p.drawPixmap(14, 14, 28, 28, avatarPix);
    drawMailEnvelopeStatusIcon(&p, QRectF(19.0, 46.0, 18.0, 18.0),
                               m_conv.isUnread, m_conv.isReplied, m_conv.isForwarded, m_selected);

    // Unread dot indicator
    if (m_conv.isUnread && !m_selected) {
        p.setPen(Qt::NoPen);
        p.setBrush(m_palette.primaryAccent);
        p.drawEllipse(QRectF(6.0, 27.0, 5.0, 5.0));
    }

    QColor senderColor = m_selected ? QColor("#FFFFFF") : QColor("#111827");
    QColor timeColor   = m_selected ? QColor("#DBEAFE") : QColor("#6B7280");
    QColor subjColor   = m_selected ? QColor("#FFFFFF") : QColor("#374151");
    QColor snipColor   = m_selected ? QColor("#DBEAFE") : QColor("#6B7280");

    int textLeft = 52;
    int rightPad = 14;
    int availW = width() - textLeft - rightPad;

    // Row 1: Sender Name + Short Time
    QFont timeFont("Segoe UI", 8);
    p.setFont(timeFont);
    p.setPen(timeColor);
    QFontMetrics tmFm(timeFont);
    int timeW = tmFm.horizontalAdvance(msg.shortTimeText) + 6;
    p.drawText(QRect(width() - rightPad - timeW, 11, timeW, 18),
               Qt::AlignRight | Qt::AlignVCenter,
               msg.shortTimeText);

    QFont senderFont("Segoe UI", 9, m_conv.isUnread ? QFont::Bold : QFont::DemiBold);
    p.setFont(senderFont);
    p.setPen(senderColor);
    QFontMetrics sFm(senderFont);
    QString senderStr = m_privacyBlur ? "••••••••" : msg.senderName;
    p.drawText(QRect(textLeft, 10, availW - timeW - 4, 18),
               Qt::AlignLeft | Qt::AlignVCenter,
               sFm.elidedText(senderStr, Qt::ElideRight, availW - timeW - 4));

    // Row 2: Subject
    QFont subjFont("Segoe UI", 9, m_conv.isUnread ? QFont::DemiBold : QFont::Normal);
    p.setFont(subjFont);
    p.setPen(subjColor);
    QFontMetrics subFm(subjFont);
    int badgeOffsetW = 0;
    if (!m_privacyBlur) {
        badgeOffsetW = drawSubjectInlineBadges(&p, textLeft, 29, m_conv.isImportant, m_conv.isStarred, m_selected);
    }
    QString subjPrefix;
    if (!msg.attachments.isEmpty())
        subjPrefix += QStringLiteral("📎 ");
    QString subjStr = m_privacyBlur ? "••••••••••••••••••••••••"
                                    : (subjPrefix + m_conv.subject);
    int subjAvailW = qMax(20, availW - badgeOffsetW);
    p.drawText(QRect(textLeft + badgeOffsetW, 29, subjAvailW, 18),
               Qt::AlignLeft | Qt::AlignVCenter,
               subFm.elidedText(subjStr, Qt::ElideRight, subjAvailW));

    // Row 3: Snippet
    QFont snipFont("Segoe UI", 8);
    p.setFont(snipFont);
    p.setPen(snipColor);
    QFontMetrics snipFm(snipFont);
    QString snipStr = m_privacyBlur ? "••••••••••••••••••••••••••••••••" : msg.snippet;
    p.drawText(QRect(textLeft, 47, availW, 18),
               Qt::AlignLeft | Qt::AlignVCenter,
               snipFm.elidedText(snipStr, Qt::ElideRight, availW));
}

// ============================================================================
// 2-B. ConversationListDelegate (Virtualized 60FPS Delegate)
// ============================================================================
ConversationListDelegate::ConversationListDelegate(const QList<ConversationItem> *conversations,
                                                   const MailThemePalette &palette,
                                                   QObject *parent)
    : QStyledItemDelegate(parent),
      m_conversations(conversations),
      m_palette(palette)
{
}

void ConversationListDelegate::setPaletteState(const MailThemePalette &palette)
{
    m_palette = palette;
}

void ConversationListDelegate::setPrivacyBlur(bool blur)
{
    m_privacyBlur = blur;
}

void ConversationListDelegate::setCheckedConvIds(const QSet<QString> *checkedIds)
{
    m_checkedConvIds = checkedIds;
}

QSize ConversationListDelegate::sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const
{
    if (index.data(Qt::UserRole).toInt() == -1) {
        return QSize(qMax(280, option.rect.width()), 140);
    }
    return QSize(qMax(280, option.rect.width()), 78);
}

void ConversationListDelegate::paint(QPainter *p, const QStyleOptionViewItem &option, const QModelIndex &index) const
{
    if (!p || !m_conversations)
        return;

    int realIdx = index.data(Qt::UserRole).toInt();
    if (realIdx == -1) {
        const QRect r = option.rect;
        p->save();
        p->setRenderHint(QPainter::Antialiasing, true);
        QRectF card(r.left() + 14.0, r.top() + 18.0, r.width() - 28.0, r.height() - 32.0);
        p->setPen(QPen(QColor("#E5E7EB"), 1.0, Qt::DashLine));
        p->setBrush(QColor("#F9FAFB"));
        p->drawRoundedRect(card, 8.0, 8.0);

        QString emptyTitle = index.data(Qt::DisplayRole).toString();
        QString emptySub = index.data(Qt::UserRole + 1).toString();

        QFont fTitle("Malgun Gothic", 10, QFont::Bold);
        p->setFont(fTitle);
        p->setPen(QColor("#374151"));
        p->drawText(QRectF(card.left() + 12, card.top() + 22, card.width() - 24, 24),
                    Qt::AlignCenter, emptyTitle);

        if (!emptySub.isEmpty()) {
            QFont fSub("Malgun Gothic", 9);
            p->setFont(fSub);
            p->setPen(QColor("#6B7280"));
            p->drawText(QRectF(card.left() + 12, card.top() + 52, card.width() - 24, 36),
                        Qt::AlignCenter | Qt::TextWordWrap, emptySub);
        }
        p->restore();
        return;
    }
    if (realIdx < 0 || realIdx >= m_conversations->size())
        return;

    const ConversationItem &conv = (*m_conversations)[realIdx];
    const bool selected = (option.state & QStyle::State_Selected);
    const QRect r = option.rect;

    p->save();
    p->setRenderHint(QPainter::Antialiasing, true);

    QRectF cardRect(r.left() + 4.0, r.top() + 2.0, r.width() - 8.0, r.height() - 4.0);
    if (selected) {
        p->setPen(Qt::NoPen);
        p->setBrush(m_palette.selectedRowBg);
        p->drawRoundedRect(cardRect, 4.0, 4.0);
    } else {
        p->setPen(Qt::NoPen);
        p->setBrush(QColor("#FFFFFF"));
        p->drawRect(r);
        p->setPen(QPen(QColor("#F1F5F9"), 1.0));
        p->drawLine(r.left() + 16, r.bottom(), r.right() - 12, r.bottom());
    }

    if (!conv.messages.isEmpty()) {
        const MessageItem &msg = conv.latestMessage();
        const bool isChecked = (m_checkedConvIds && m_checkedConvIds->contains(conv.id));

        // 1. 좌측 체크박스 (x = r.left() + 9, y = r.top() + 30, 17x17)
        QRectF cbRect(r.left() + 9.0, r.top() + 30.0, 17.0, 17.0);
        if (isChecked) {
            p->setPen(QPen(selected ? QColor("#FFFFFF") : QColor("#0D47A1"), 1.5));
            p->setBrush(selected ? QColor("#FFFFFF") : QColor("#0D47A1"));
            p->drawRoundedRect(cbRect, 3.5, 3.5);

            // 체크 마크 (✓)
            QPainterPath chkPath;
            chkPath.moveTo(cbRect.left() + 4.0, cbRect.top() + 8.8);
            chkPath.lineTo(cbRect.left() + 7.2, cbRect.top() + 12.2);
            chkPath.lineTo(cbRect.left() + 13.2, cbRect.top() + 5.2);
            p->setPen(QPen(selected ? QColor("#0D47A1") : QColor("#FFFFFF"), 2.0, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
            p->setBrush(Qt::NoBrush);
            p->drawPath(chkPath);
        } else {
            p->setPen(QPen(selected ? QColor(255, 255, 255, 190) : QColor("#94A3B8"), 1.5));
            p->setBrush(selected ? QColor(255, 255, 255, 25) : QColor("#FFFFFF"));
            p->drawRoundedRect(cbRect, 3.5, 3.5);
        }

        // 2. 아바타 (x=33, y=13, 28x28) 및 아바타 바로 아래 편지 봉투 상태 아이콘 (읽음/안읽음 + 답장 ⬅ / 전달 ➡, x=38, y=46, 18x18)
        QPixmap avatarPix = MailIcons::senderAvatarPixmap(msg, 28);
        p->drawPixmap(r.left() + 33, r.top() + 13, 28, 28, avatarPix);

        // 3. 안 읽음 파란 점 표시 (아바타 우측 하단 배지)
        if (conv.isUnread && !selected) {
            p->setPen(QPen(QColor("#FFFFFF"), 1.2));
            p->setBrush(m_palette.primaryAccent);
            p->drawEllipse(QRectF(r.left() + 54.0, r.top() + 34.0, 7.0, 7.0));
        }

        // 4. 편지 봉투 상태 아이콘 (안 읽음 닫힌 봉투 / 읽음 열린 봉투 / 답장 ⬅ 빨간 화살표 / 전달 ➡ 파란 화살표)
        drawMailEnvelopeStatusIcon(p,
                                   QRectF(r.left() + 38.0, r.top() + 46.0, 18.0, 18.0),
                                   conv.isUnread,
                                   conv.isReplied,
                                   conv.isForwarded,
                                   selected);

        QColor senderColor = selected ? QColor("#FFFFFF") : QColor("#111827");
        QColor timeColor   = selected ? QColor("#DBEAFE") : QColor("#6B7280");
        QColor subjColor   = selected ? QColor("#FFFFFF") : QColor("#374151");
        QColor snipColor   = selected ? QColor("#DBEAFE") : QColor("#6B7280");

        int textLeft = r.left() + 67;
        int rightPad = 12;
        int availW = qMax(40, r.width() - 67 - rightPad);

        // Row 1: Sender Name + Short Time
        QFont timeFont("Segoe UI", 8);
        p->setFont(timeFont);
        p->setPen(timeColor);
        QFontMetrics tmFm(timeFont);
        int timeW = tmFm.horizontalAdvance(msg.shortTimeText) + 6;
        p->drawText(QRect(r.right() - rightPad - timeW, r.top() + 11, timeW, 18),
                    Qt::AlignRight | Qt::AlignVCenter,
                    msg.shortTimeText);

        QFont senderFont("Segoe UI", 9, conv.isUnread ? QFont::Bold : QFont::DemiBold);
        p->setFont(senderFont);
        p->setPen(senderColor);
        QFontMetrics sFm(senderFont);
        QString senderStr = m_privacyBlur ? "••••••••" : msg.senderName;
        p->drawText(QRect(textLeft, r.top() + 10, qMax(20, availW - timeW - 4), 18),
                    Qt::AlignLeft | Qt::AlignVCenter,
                    sFm.elidedText(senderStr, Qt::ElideRight, qMax(20, availW - timeW - 4)));

        // Row 2: Subject
        QFont subjFont("Segoe UI", 9, conv.isUnread ? QFont::DemiBold : QFont::Normal);
        p->setFont(subjFont);
        p->setPen(subjColor);
        QFontMetrics subFm(subjFont);
        int badgeOffsetW = 0;
        if (!m_privacyBlur) {
            badgeOffsetW = drawSubjectInlineBadges(p, textLeft, r.top() + 29, conv.isImportant, conv.isStarred, selected);
        }
        QString subjPrefix;
        if (!msg.attachments.isEmpty())
            subjPrefix += QStringLiteral("📎 ");
        QString subjStr = m_privacyBlur ? "••••••••••••••••••••••••"
                                        : (subjPrefix + conv.subject);
        int subjAvailW = qMax(20, availW - badgeOffsetW);
        p->drawText(QRect(textLeft + badgeOffsetW, r.top() + 29, subjAvailW, 18),
                    Qt::AlignLeft | Qt::AlignVCenter,
                    subFm.elidedText(subjStr, Qt::ElideRight, subjAvailW));

        // Row 3: Snippet
        QFont snipFont("Segoe UI", 8);
        p->setFont(snipFont);
        p->setPen(snipColor);
        QFontMetrics snipFm(snipFont);
        QString snipStr = m_privacyBlur ? "••••••••••••••••••••••••••••••••" : msg.snippet;
        p->drawText(QRect(textLeft, r.top() + 47, availW, 18),
                    Qt::AlignLeft | Qt::AlignVCenter,
                    snipFm.elidedText(snipStr, Qt::ElideRight, availW));
    }

    p->restore();
}

// ============================================================================
// 3. DayTimelineWidget (Right Calendar Sidebar & Full Day View 24h Timeline)
// ============================================================================
DayTimelineWidget::DayTimelineWidget(QWidget *parent)
    : QWidget(parent)
{
    setMinimumHeight(24 * 54 + 20);
}

void DayTimelineWidget::setCurrentDate(const QDate &date)
{
    m_date = date;
    update();
}

void DayTimelineWidget::setEvents(const QList<CalendarEventItem> &events)
{
    m_events = events;
    update();
}

void DayTimelineWidget::setMarkerTime(const QTime &time)
{
    m_markerTime = time;
    update();
}

void DayTimelineWidget::setIncludeAllDayInGrid(bool includeAllDay)
{
    m_includeAllDay = includeAllDay;
    update();
}

void DayTimelineWidget::setSearchFilter(const QString &query)
{
    m_searchQuery = query.trimmed();
    update();
}

void DayTimelineWidget::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.fillRect(rect(), QColor("#FFFFFF"));

    const int hourHeight = 54;
    const int topOffset = 14;
    const int labelWidth = 56;

    QFont hourFont("Segoe UI", 8);
    p.setFont(hourFont);

    for (int h = 0; h < 24; ++h) {
        int y = topOffset + h * hourHeight;
        QString label = formatHourGutterLabel(h);

        p.setPen(QColor("#9CA3AF"));
        p.drawText(QRect(6, y - 8, labelWidth - 12, 16), Qt::AlignRight | Qt::AlignVCenter, label);

        p.setPen(QPen(QColor("#F1F5F9"), 1.0));
        p.drawLine(labelWidth, y, width() - 6, y);
    }

    // Vertical left border of the day column
    p.setPen(QPen(QColor("#E5E7EB"), 1.0));
    p.drawLine(labelWidth, 0, labelWidth, height());

    // Draw events for m_date
    for (const CalendarEventItem &ev : m_events) {
        if (ev.date != m_date)
            continue;
        if (!m_includeAllDay && ev.isAllDay)
            continue;
        if (!m_searchQuery.isEmpty() && !ev.title.contains(m_searchQuery, Qt::CaseInsensitive))
            continue;

        int startMin = ev.isAllDay ? 9 * 60 : (ev.startTime.hour() * 60 + ev.startTime.minute());
        int endMin   = ev.isAllDay ? 10 * 60 : (ev.endTime.hour() * 60 + ev.endTime.minute());
        if (endMin <= startMin)
            endMin = startMin + 60;

        qreal yStart = topOffset + (startMin / 60.0) * hourHeight;
        qreal hBox   = qMax(24.0, ((endMin - startMin) / 60.0) * hourHeight);

        QRectF evRect(labelWidth + 4, yStart + 1, width() - labelWidth - 12, hBox - 2);
        p.setPen(Qt::NoPen);
        p.setBrush(ev.pillBgColor);
        p.drawRoundedRect(evRect, 3.0, 3.0);

        p.setBrush(ev.pillBarColor);
        p.drawRoundedRect(QRectF(evRect.left(), evRect.top() + 1.5, 3.0, evRect.height() - 3.0), 1.5, 1.5);

        p.setPen(ev.pillTextColor);
        QFont ef("Segoe UI", 8, QFont::DemiBold);
        p.setFont(ef);
        p.drawText(evRect.adjusted(8, 3, -4, -3), Qt::AlignLeft | Qt::AlignTop, ev.title);
    }

    // Draw red current-time pointed badge + horizontal red line using real-time system clock
    m_markerTime = QTime::currentTime();
    int totalMins = m_markerTime.hour() * 60 + m_markerTime.minute();
    qreal markerY = topOffset + (totalMins / 60.0) * hourHeight;

    QRectF badgeRect(6.0, markerY - 9.5, 40.0, 19.0);
    QPainterPath pillPath;
    pillPath.addRoundedRect(badgeRect, 4.0, 4.0);
    QPainterPath tipPath;
    tipPath.moveTo(badgeRect.right() - 1.0, markerY - 6.0);
    tipPath.lineTo(badgeRect.right() + 5.5, markerY);
    tipPath.lineTo(badgeRect.right() - 1.0, markerY + 6.0);
    tipPath.closeSubpath();
    pillPath = pillPath.united(tipPath);

    p.setPen(Qt::NoPen);
    p.setBrush(QColor("#C81E1E"));
    p.drawPath(pillPath);

    p.setPen(QColor("#FFFFFF"));
    QFont badgeFont("Segoe UI", 8, QFont::Bold);
    p.setFont(badgeFont);
    p.drawText(badgeRect, Qt::AlignCenter, formatMarkerBadgeText(m_markerTime));

    // Horizontal red line across the day column
    p.setPen(QPen(QColor("#C81E1E"), 1.5));
    p.drawLine(QPointF(badgeRect.right() + 5.0, markerY), QPointF(width() - 4.0, markerY));
}

void DayTimelineWidget::mousePressEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton) {
        const int hourHeight = 54;
        const int labelWidth = 56;
        const int topOffset = 14;
        const QPointF pt = event->pos();

        for (const CalendarEventItem &ev : m_events) {
            if (ev.date != m_date)
                continue;
            if (!m_includeAllDay && ev.isAllDay)
                continue;
            if (!m_searchQuery.isEmpty() && !ev.title.contains(m_searchQuery, Qt::CaseInsensitive))
                continue;

            int startMin = ev.isAllDay ? 9 * 60 : (ev.startTime.hour() * 60 + ev.startTime.minute());
            int endMin   = ev.isAllDay ? 10 * 60 : (ev.endTime.hour() * 60 + ev.endTime.minute());
            if (endMin <= startMin)
                endMin = startMin + 60;

            qreal yStart = topOffset + (startMin / 60.0) * hourHeight;
            qreal hBox   = qMax(24.0, ((endMin - startMin) / 60.0) * hourHeight);
            QRectF evRect(labelWidth + 4, yStart + 1, width() - labelWidth - 12, hBox - 2);
            if (evRect.contains(pt)) {
                emit eventClicked(ev);
                return;
            }
        }

        int h = qBound(0, (event->pos().y() - topOffset) / hourHeight, 23);
        emit slotClicked(QTime(h, 0));
    }
}

// ============================================================================
// 4. MiniMonthCalendarWidget (Left Sidebar Mini Calendar - Sunday/Monday aware)
// ============================================================================
MiniMonthCalendarWidget::MiniMonthCalendarWidget(QWidget *parent)
    : QWidget(parent)
{
    setFixedSize(205, 195);
    setCursor(Qt::PointingHandCursor);
}

void MiniMonthCalendarWidget::setYearMonth(int year, int month)
{
    m_year = year;
    m_month = month;
    update();
}

void MiniMonthCalendarWidget::setSelectedDate(const QDate &date)
{
    m_selectedDate = date;
    m_year = date.year();
    m_month = date.month();
    update();
}

void MiniMonthCalendarWidget::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);

    // Header: "<  2026년 11월  >"
    QFont headerFont("Segoe UI", 9, QFont::DemiBold);
    p.setFont(headerFont);
    p.setPen(QColor("#6B7280"));
    p.drawText(QRect(4, 2, 20, 24), Qt::AlignCenter, "‹");
    p.drawText(QRect(width() - 24, 2, 20, 24), Qt::AlignCenter, "›");

    p.setPen(QColor("#111827"));
    const QString miniHdrText = MailI18n::t(
        QString("%1년 %2월").arg(m_year).arg(m_month),
        QLocale(QLocale::English).toString(QDate(m_year, m_month, 1), "MMM yyyy"),
        QString("%1年 %2月").arg(m_year).arg(m_month));
    p.drawText(QRect(24, 2, width() - 48, 24), Qt::AlignCenter, miniHdrText);

    // Weekday headers: dynamically respects Sunday (7) vs Monday (1) start
    const QStringList dayNames = orderedWeekdayNames();
    int cellW = (width() - 8) / 7;
    int topY = 30;
    QFont dowFont("Segoe UI", 8, QFont::DemiBold);
    p.setFont(dowFont);
    p.setPen(QColor("#6B7280"));
    for (int c = 0; c < 7; ++c) {
        p.drawText(QRect(4 + c * cellW, topY, cellW, 20), Qt::AlignCenter, dayNames[c]);
    }

    QDate firstOfMonth(m_year, m_month, 1);
    const bool sundayFirst = (MailI18n::calendarWeekStartDay() == 7);
    const int offset = sundayFirst ? (firstOfMonth.dayOfWeek() % 7) : (firstOfMonth.dayOfWeek() - 1);
    QDate gridStart = firstOfMonth.addDays(-offset);

    QFont dayFont("Segoe UI", 8);
    int rowH = 23;
    for (int r = 0; r < 6; ++r) {
        for (int c = 0; c < 7; ++c) {
            QDate d = gridStart.addDays(r * 7 + c);
            QRect cellRect(4 + c * cellW, topY + 22 + r * rowH, cellW, rowH);

            bool isSelected = (d == m_selectedDate);
            bool isCurrentMonth = (d.month() == m_month);

            if (isSelected) {
                p.setPen(Qt::NoPen);
                p.setBrush(QColor("#0D47A1"));
                QRectF circle(cellRect.center().x() - 10.5, cellRect.center().y() - 10.5, 21.0, 21.0);
                p.drawEllipse(circle);
                p.setPen(QColor("#FFFFFF"));
                QFont bf = dayFont;
                bf.setBold(true);
                p.setFont(bf);
            } else {
                p.setFont(dayFont);
                p.setPen(isCurrentMonth ? QColor("#374151") : QColor("#D1D5DB"));
            }
            p.drawText(cellRect, Qt::AlignCenter, QString::number(d.day()));
        }
    }
}

void MiniMonthCalendarWidget::mousePressEvent(QMouseEvent *event)
{
    QPoint pt = event->pos();
    if (pt.y() <= 26) {
        if (pt.x() < 28) {
            QDate prev = QDate(m_year, m_month, 1).addMonths(-1);
            m_year = prev.year();
            m_month = prev.month();
            update();
            emit monthChanged(m_year, m_month);
        } else if (pt.x() > width() - 28) {
            QDate next = QDate(m_year, m_month, 1).addMonths(1);
            m_year = next.year();
            m_month = next.month();
            update();
            emit monthChanged(m_year, m_month);
        }
        return;
    }

    int cellW = (width() - 8) / 7;
    int topY = 52;
    int rowH = 23;
    if (pt.y() >= topY) {
        int c = qBound(0, (pt.x() - 4) / cellW, 6);
        int r = qBound(0, (pt.y() - topY) / rowH, 5);
        QDate firstOfMonth(m_year, m_month, 1);
        const bool sundayFirst = (MailI18n::calendarWeekStartDay() == 7);
        const int offset = sundayFirst ? (firstOfMonth.dayOfWeek() % 7) : (firstOfMonth.dayOfWeek() - 1);
        QDate gridStart = firstOfMonth.addDays(-offset);
        QDate clicked = gridStart.addDays(r * 7 + c);
        m_selectedDate = clicked;
        m_year = clicked.year();
        m_month = clicked.month();
        update();
        emit dateSelected(clicked);
    }
}

// ============================================================================
// 5. MonthCalendarGridWidget (Month View - Sunday/Monday & Week# aware)
// ============================================================================
MonthCalendarGridWidget::MonthCalendarGridWidget(QWidget *parent)
    : QWidget(parent)
{
    setMinimumSize(680, 520);
}

void MonthCalendarGridWidget::setYearMonth(int year, int month)
{
    m_year = year;
    m_month = month;
    update();
}

void MonthCalendarGridWidget::setSelectedDate(const QDate &date)
{
    m_selectedDate = date;
    update();
}

void MonthCalendarGridWidget::setEvents(const QList<CalendarEventItem> &events)
{
    m_events = events;
    update();
}

void MonthCalendarGridWidget::setSearchFilter(const QString &query)
{
    m_searchQuery = query.trimmed();
    update();
}

QDate MonthCalendarGridWidget::gridStartDate() const
{
    QDate firstOfMonth(m_year, m_month, 1);
    const bool sundayFirst = (MailI18n::calendarWeekStartDay() == 7);
    const int offset = sundayFirst ? (firstOfMonth.dayOfWeek() % 7) : (firstOfMonth.dayOfWeek() - 1);
    return firstOfMonth.addDays(-offset);
}

void MonthCalendarGridWidget::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.fillRect(rect(), QColor("#FFFFFF"));

    const int headerH = 30;
    const bool showWeekNums = MailI18n::calendarShowWeekNumbers();
    const qreal weekColW = showWeekNums ? 30.0 : 0.0;
    const qreal colW = (width() - weekColW) / 7.0;
    const qreal rowH = (height() - headerH) / 6.0;

    // Weekday headers (Sunday first or Monday first)
    const QStringList dayNames = orderedWeekdayNames();
    QFont headerFont("Segoe UI", 8);
    p.setFont(headerFont);
    if (showWeekNums) {
        p.setPen(QColor("#9CA3AF"));
        p.drawText(QRectF(0, 0, weekColW, headerH), Qt::AlignCenter, "W");
    }
    for (int c = 0; c < 7; ++c) {
        QRectF hRect(weekColW + c * colW, 0, colW, headerH);
        p.setPen(QColor("#6B7280"));
        p.drawText(hRect, Qt::AlignCenter, dayNames[c]);
    }

    p.setPen(QPen(QColor("#E5E7EB"), 1.0));
    p.drawLine(0, headerH, width(), headerH);

    QDate gridStart = gridStartDate();

    for (int r = 0; r < 6; ++r) {
        if (showWeekNums) {
            QDate rowDate = gridStart.addDays(r * 7 + 3);
            QRectF wkRect(0, headerH + r * rowH, weekColW, rowH);
            p.fillRect(wkRect, QColor("#F9FAFB"));
            p.setBrush(Qt::NoBrush);
            p.setPen(QPen(QColor("#F1F5F9"), 1.0));
            p.drawRect(wkRect);
            p.setPen(QColor("#9CA3AF"));
            QFont wf("Segoe UI", 7.5, QFont::DemiBold);
            p.setFont(wf);
            p.drawText(wkRect.adjusted(0, 6, 0, 0), Qt::AlignHCenter | Qt::AlignTop,
                       QString::number(rowDate.weekNumber()));
        }

        for (int c = 0; c < 7; ++c) {
            QDate d = gridStart.addDays(r * 7 + c);
            QRectF cellRect(weekColW + c * colW, headerH + r * rowH, colW, rowH);

            // Subtle background for weekend (Saturday=6, Sunday=7)
            if (d.dayOfWeek() == 6 || d.dayOfWeek() == 7) {
                p.fillRect(cellRect, QColor("#FAFAFB"));
            } else {
                p.fillRect(cellRect, QColor("#FFFFFF"));
            }

            // Grid borders
            p.setBrush(Qt::NoBrush);
            p.setPen(QPen(QColor("#F1F5F9"), 1.0));
            p.drawRect(cellRect);

            // Day number in top-right of cell
            bool isSelected = (d == m_selectedDate);
            bool isCurrentMonth = (d.month() == m_month);

            QRectF numRect(cellRect.right() - 28.0, cellRect.top() + 5.0, 22.0, 22.0);
            if (isSelected) {
                p.setPen(Qt::NoPen);
                p.setBrush(QColor("#0D47A1"));
                p.drawEllipse(numRect);
                p.setPen(QColor("#FFFFFF"));
                QFont f("Segoe UI", 8, QFont::Bold);
                p.setFont(f);
                p.drawText(numRect, Qt::AlignCenter, QString::number(d.day()));
            } else {
                p.setPen(isCurrentMonth ? QColor("#374151") : QColor("#9CA3AF"));
                QFont f("Segoe UI", 8, isCurrentMonth ? QFont::DemiBold : QFont::Normal);
                p.setFont(f);
                p.drawText(numRect, Qt::AlignCenter, QString::number(d.day()));
            }

            // Events on this day
            int evIndex = 0;
            for (const CalendarEventItem &ev : m_events) {
                if (ev.date != d)
                    continue;
                if (!m_searchQuery.isEmpty() && !ev.title.contains(m_searchQuery, Qt::CaseInsensitive))
                    continue;

                qreal pillY = cellRect.top() + 30.0 + evIndex * 22.0;
                if (pillY + 19.0 > cellRect.bottom())
                    break;

                QRectF pillRect(cellRect.left() + 4.0, pillY, cellRect.width() - 8.0, 19.0);
                p.setPen(Qt::NoPen);
                p.setBrush(ev.pillBgColor);
                p.drawRoundedRect(pillRect, 3.0, 3.0);

                // Left accent bar
                p.setBrush(ev.pillBarColor);
                p.drawRoundedRect(QRectF(pillRect.left(), pillRect.top() + 2.0, 2.5, pillRect.height() - 4.0), 1.0, 1.0);

                p.setPen(ev.pillTextColor);
                QFont ef("Segoe UI", 8);
                p.setFont(ef);
                QFontMetrics fm(ef);
                QString elided = fm.elidedText(ev.title, Qt::ElideRight, int(pillRect.width() - 12));
                p.drawText(pillRect.adjusted(7.0, 0, -3.0, 0), Qt::AlignLeft | Qt::AlignVCenter, elided);
                ++evIndex;
            }
        }
    }
}

QDate MonthCalendarGridWidget::dateAtPos(const QPoint &pos) const
{
    const int headerH = 30;
    const qreal weekColW = MailI18n::calendarShowWeekNumbers() ? 30.0 : 0.0;
    if (pos.y() < headerH || pos.x() < weekColW)
        return QDate();
    qreal colW = (width() - weekColW) / 7.0;
    qreal rowH = (height() - headerH) / 6.0;
    int c = qBound(0, int((pos.x() - weekColW) / colW), 6);
    int r = qBound(0, int((pos.y() - headerH) / rowH), 5);
    return gridStartDate().addDays(r * 7 + c);
}

bool MonthCalendarGridWidget::eventAtPos(const QPoint &pos, CalendarEventItem &outEv) const
{
    const int headerH = 30;
    const qreal weekColW = MailI18n::calendarShowWeekNumbers() ? 30.0 : 0.0;
    if (pos.y() < headerH || pos.x() < weekColW)
        return false;

    qreal colW = (width() - weekColW) / 7.0;
    qreal rowH = (height() - headerH) / 6.0;
    int c = qBound(0, int((pos.x() - weekColW) / colW), 6);
    int r = qBound(0, int((pos.y() - headerH) / rowH), 5);

    QDate d = gridStartDate().addDays(r * 7 + c);
    QRectF cellRect(weekColW + c * colW, headerH + r * rowH, colW, rowH);

    int evIndex = 0;
    for (const CalendarEventItem &ev : m_events) {
        if (ev.date != d)
            continue;
        if (!m_searchQuery.isEmpty() && !ev.title.contains(m_searchQuery, Qt::CaseInsensitive))
            continue;

        qreal pillY = cellRect.top() + 30.0 + evIndex * 22.0;
        if (pillY + 19.0 > cellRect.bottom())
            break;

        QRectF pillRect(cellRect.left() + 4.0, pillY, cellRect.width() - 8.0, 19.0);
        if (pillRect.contains(QPointF(pos))) {
            outEv = ev;
            return true;
        }
        ++evIndex;
    }
    return false;
}

void MonthCalendarGridWidget::mousePressEvent(QMouseEvent *event)
{
    QDate d = dateAtPos(event->pos());
    if (d.isValid()) {
        m_selectedDate = d;
        update();
        emit dateClicked(d);

        CalendarEventItem clickedEv;
        if (eventAtPos(event->pos(), clickedEv)) {
            emit eventClicked(clickedEv);
        }
    }
}

void MonthCalendarGridWidget::mouseDoubleClickEvent(QMouseEvent *event)
{
    CalendarEventItem clickedEv;
    if (eventAtPos(event->pos(), clickedEv)) {
        emit eventClicked(clickedEv);
        return;
    }
    QDate d = dateAtPos(event->pos());
    if (d.isValid()) {
        emit dateDoubleClicked(d);
    }
}

// ============================================================================
// 6. WeekCalendarViewWidget (Week View:
//    Top 7-day header with blue circle + "All day" row + 24h 7-col timeline
//    with red badge "1:00", dashed red line, and solid red line with ticks)
// ============================================================================
namespace {

class WeekHeaderAndAllDayBar : public QWidget {
public:
    explicit WeekHeaderAndAllDayBar(WeekCalendarViewWidget *owner,
                                    const QDate *selectedDate,
                                    const QList<CalendarEventItem> *events,
                                    const QString *searchQuery)
        : QWidget(owner)
        , m_owner(owner)
        , m_selectedDate(selectedDate)
        , m_events(events)
        , m_searchQuery(searchQuery)
    {
        setFixedHeight(88);
        setCursor(Qt::PointingHandCursor);
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing, true);
        p.fillRect(rect(), QColor("#FFFFFF"));

        const int gutterW = 56;
        const int topHeaderH = 56;
        const int allDayH = 32;
        const qreal colW = (width() - gutterW) / 7.0;
        const QDate wkStart = m_owner->weekStartDate();

        // Optional week number in top-left gutter
        if (MailI18n::calendarShowWeekNumbers()) {
            p.setPen(QColor("#9CA3AF"));
            QFont wf("Segoe UI", 8, QFont::DemiBold);
            p.setFont(wf);
            p.drawText(QRect(0, 12, gutterW, 24), Qt::AlignCenter,
                       QString("W%1").arg(wkStart.addDays(3).weekNumber()));
        }

        // 7-day header columns
        for (int c = 0; c < 7; ++c) {
            QDate d = wkStart.addDays(c);
            qreal x = gutterW + c * colW;

            // Vertical divider in header
            p.setPen(QPen(QColor("#F1F5F9"), 1.0));
            p.drawLine(QPointF(x, 6), QPointF(x, height()));

            // Weekday label ("월", "화"...)
            p.setPen(QColor("#6B7280"));
            QFont dowFont("Segoe UI", 8);
            p.setFont(dowFont);
            p.drawText(QRectF(x, 6, colW, 16), Qt::AlignCenter, shortDowName(d.dayOfWeek()));

            // Day number (with 28x28 #0D47A1 blue circle on selected date)
            bool isSel = (d == *m_selectedDate);
            QRectF numCircle(x + (colW - 28.0) / 2.0, 23.0, 28.0, 28.0);
            if (isSel) {
                p.setPen(Qt::NoPen);
                p.setBrush(QColor("#0D47A1"));
                p.drawEllipse(numCircle);
                p.setPen(QColor("#FFFFFF"));
                QFont nf("Segoe UI", 11, QFont::Bold);
                p.setFont(nf);
                p.drawText(numCircle, Qt::AlignCenter, QString::number(d.day()));
            } else {
                p.setPen(QColor("#1F2937"));
                QFont nf("Segoe UI", 11, QFont::Normal);
                p.setFont(nf);
                p.drawText(numCircle, Qt::AlignCenter, QString::number(d.day()));
            }
        }

        // Horizontal line above All day row
        p.setPen(QPen(QColor("#E5E7EB"), 1.0));
        p.drawLine(0, topHeaderH, width(), topHeaderH);

        // "All day" label in left gutter
        p.setPen(QColor("#9CA3AF"));
        QFont adFont("Segoe UI", 8);
        p.setFont(adFont);
        p.drawText(QRect(4, topHeaderH, gutterW - 10, allDayH),
                   Qt::AlignRight | Qt::AlignVCenter,
                   MailI18n::t("종일", "All day", "終日"));

        // All-day event pills in each day column
        for (int c = 0; c < 7; ++c) {
            QDate d = wkStart.addDays(c);
            qreal x = gutterW + c * colW;
            QRectF adCell(x, topHeaderH, colW, allDayH);
            if (d.dayOfWeek() == 6 || d.dayOfWeek() == 7) {
                p.fillRect(adCell.adjusted(1, 1, 0, -1), QColor("#FAFAFB"));
            }

            for (const CalendarEventItem &ev : *m_events) {
                if (ev.date != d || !ev.isAllDay)
                    continue;
                if (!m_searchQuery->isEmpty() && !ev.title.contains(*m_searchQuery, Qt::CaseInsensitive))
                    continue;

                QRectF pillRect(x + 3.0, topHeaderH + 5.0, colW - 6.0, 21.0);
                p.setPen(Qt::NoPen);
                p.setBrush(ev.pillBgColor);
                p.drawRoundedRect(pillRect, 3.0, 3.0);

                p.setBrush(ev.pillBarColor);
                p.drawRoundedRect(QRectF(pillRect.left(), pillRect.top() + 2.0, 2.5, pillRect.height() - 4.0), 1.0, 1.0);

                p.setPen(ev.pillTextColor);
                QFont ef("Segoe UI", 8);
                p.setFont(ef);
                QFontMetrics fm(ef);
                QString elided = fm.elidedText(ev.title, Qt::ElideRight, int(pillRect.width() - 12));
                p.drawText(pillRect.adjusted(7.0, 0, -3.0, 0), Qt::AlignLeft | Qt::AlignVCenter, elided);
                break;
            }
        }

        // Bottom border of All day row
        p.setPen(QPen(QColor("#E5E7EB"), 1.0));
        p.drawLine(0, height() - 1, width(), height() - 1);
    }

    void mousePressEvent(QMouseEvent *event) override
    {
        const int gutterW = 56;
        const int topHeaderH = 56;
        if (event->pos().x() < gutterW)
            return;
        qreal colW = (width() - gutterW) / 7.0;
        int c = qBound(0, int((event->pos().x() - gutterW) / colW), 6);
        QDate d = m_owner->weekStartDate().addDays(c);
        emit m_owner->dateClicked(d);

        if (event->pos().y() >= topHeaderH) {
            for (const CalendarEventItem &ev : *m_events) {
                if (ev.date != d || !ev.isAllDay)
                    continue;
                if (!m_searchQuery->isEmpty() && !ev.title.contains(*m_searchQuery, Qt::CaseInsensitive))
                    continue;
                emit m_owner->eventClicked(ev);
                return;
            }
        }
    }

    void mouseDoubleClickEvent(QMouseEvent *event) override
    {
        const int gutterW = 56;
        if (event->pos().x() < gutterW)
            return;
        qreal colW = (width() - gutterW) / 7.0;
        int c = qBound(0, int((event->pos().x() - gutterW) / colW), 6);
        QDate d = m_owner->weekStartDate().addDays(c);
        emit m_owner->dateDoubleClicked(d);
    }

private:
    WeekCalendarViewWidget *m_owner;
    const QDate *m_selectedDate;
    const QList<CalendarEventItem> *m_events;
    const QString *m_searchQuery;
};

class WeekTimelineGridCanvas : public QWidget {
public:
    explicit WeekTimelineGridCanvas(WeekCalendarViewWidget *owner,
                                    const QDate *selectedDate,
                                    const QList<CalendarEventItem> *events,
                                    const QString *searchQuery,
                                    QWidget *parent = nullptr)
        : QWidget(parent)
        , m_owner(owner)
        , m_selectedDate(selectedDate)
        , m_events(events)
        , m_searchQuery(searchQuery)
    {
        setMinimumHeight(24 * 54 + 20);
        setCursor(Qt::PointingHandCursor);
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing, true);
        p.fillRect(rect(), QColor("#FFFFFF"));

        const int gutterW = 56;
        const int hourHeight = 54;
        const int topOffset = 14;
        const qreal colW = (width() - gutterW) / 7.0;
        const QDate wkStart = m_owner->weekStartDate();

        // Shade weekend columns (#FAFAFB)
        for (int c = 0; c < 7; ++c) {
            QDate d = wkStart.addDays(c);
            qreal x = gutterW + c * colW;
            if (d.dayOfWeek() == 6 || d.dayOfWeek() == 7) {
                p.fillRect(QRectF(x, 0, colW, height()), QColor("#FAFAFB"));
            }
        }

        // Hour lines & left gutter hour labels
        QFont hourFont("Segoe UI", 8);
        p.setFont(hourFont);
        for (int h = 0; h < 24; ++h) {
            int y = topOffset + h * hourHeight;
            p.setPen(QColor("#9CA3AF"));
            p.drawText(QRect(4, y - 8, gutterW - 10, 16), Qt::AlignRight | Qt::AlignVCenter, formatHourGutterLabel(h));

            p.setPen(QPen(QColor("#F1F5F9"), 1.0));
            p.drawLine(gutterW, y, width(), y);
        }

        // Vertical column dividers
        p.setPen(QPen(QColor("#F1F5F9"), 1.0));
        for (int c = 0; c < 7; ++c) {
            qreal x = gutterW + c * colW;
            p.drawLine(QPointF(x, 0), QPointF(x, height()));
        }

        // Timed events in each of the 7 day columns
        for (int c = 0; c < 7; ++c) {
            QDate d = wkStart.addDays(c);
            qreal x = gutterW + c * colW;
            for (const CalendarEventItem &ev : *m_events) {
                if (ev.date != d || ev.isAllDay)
                    continue;
                if (!m_searchQuery->isEmpty() && !ev.title.contains(*m_searchQuery, Qt::CaseInsensitive))
                    continue;

                int startMin = ev.startTime.hour() * 60 + ev.startTime.minute();
                int endMin   = ev.endTime.hour() * 60 + ev.endTime.minute();
                if (endMin <= startMin)
                    endMin = startMin + 60;

                qreal yStart = topOffset + (startMin / 60.0) * hourHeight;
                qreal hBox   = qMax(24.0, ((endMin - startMin) / 60.0) * hourHeight);
                QRectF evRect(x + 3.0, yStart + 1.0, colW - 6.0, hBox - 2.0);

                p.setPen(Qt::NoPen);
                p.setBrush(ev.pillBgColor);
                p.drawRoundedRect(evRect, 3.0, 3.0);

                p.setBrush(ev.pillBarColor);
                p.drawRoundedRect(QRectF(evRect.left(), evRect.top() + 1.5, 2.5, evRect.height() - 3.0), 1.0, 1.0);

                p.setPen(ev.pillTextColor);
                QFont ef("Segoe UI", 8, QFont::DemiBold);
                p.setFont(ef);
                p.drawText(evRect.adjusted(7, 3, -3, -2), Qt::AlignLeft | Qt::AlignTop, ev.title);
            }
        }

        // Week View Red Current-Time Marker (real-time system clock)
        const QTime markerTime = QTime::currentTime();
        int totalMins = markerTime.hour() * 60 + markerTime.minute();
        qreal markerY = topOffset + (totalMins / 60.0) * hourHeight;

        QRectF badgeRect(6.0, markerY - 9.5, 40.0, 19.0);
        QPainterPath pillPath;
        pillPath.addRoundedRect(badgeRect, 4.0, 4.0);
        QPainterPath tipPath;
        tipPath.moveTo(badgeRect.right() - 1.0, markerY - 6.0);
        tipPath.lineTo(badgeRect.right() + 5.5, markerY);
        tipPath.lineTo(badgeRect.right() - 1.0, markerY + 6.0);
        tipPath.closeSubpath();
        pillPath = pillPath.united(tipPath);

        p.setPen(Qt::NoPen);
        p.setBrush(QColor("#C81E1E"));
        p.drawPath(pillPath);

        p.setPen(QColor("#FFFFFF"));
        QFont badgeFont("Segoe UI", 8, QFont::Bold);
        p.setFont(badgeFont);
        p.drawText(badgeRect, Qt::AlignCenter, formatMarkerBadgeText(markerTime));

        // Dashed red line across all 7 columns
        QPen dashPen(QColor("#C81E1E"), 1.3, Qt::DashLine);
        dashPen.setDashPattern({3.0, 3.0});
        p.setPen(dashPen);
        p.drawLine(QPointF(gutterW, markerY), QPointF(width(), markerY));

        // Solid red line + vertical end ticks on the selected day column
        int activeCol = wkStart.daysTo(*m_selectedDate);
        if (activeCol >= 0 && activeCol < 7) {
            qreal cx1 = gutterW + activeCol * colW;
            qreal cx2 = cx1 + colW;
            p.setPen(QPen(QColor("#C81E1E"), 1.6, Qt::SolidLine));
            p.drawLine(QPointF(cx1, markerY), QPointF(cx2, markerY));
            p.drawLine(QPointF(cx1, markerY - 4.5), QPointF(cx1, markerY + 4.5));
            p.drawLine(QPointF(cx2, markerY - 4.5), QPointF(cx2, markerY + 4.5));
        }
    }

    void mousePressEvent(QMouseEvent *event) override
    {
        const int gutterW = 56;
        const int hourHeight = 54;
        const int topOffset = 14;
        if (event->pos().x() < gutterW)
            return;
        qreal colW = (width() - gutterW) / 7.0;
        int c = qBound(0, int((event->pos().x() - gutterW) / colW), 6);
        QDate d = m_owner->weekStartDate().addDays(c);
        emit m_owner->dateClicked(d);

        qreal x = gutterW + c * colW;
        for (const CalendarEventItem &ev : *m_events) {
            if (ev.date != d || ev.isAllDay)
                continue;
            if (!m_searchQuery->isEmpty() && !ev.title.contains(*m_searchQuery, Qt::CaseInsensitive))
                continue;
            int startMin = ev.startTime.hour() * 60 + ev.startTime.minute();
            int endMin   = ev.endTime.hour() * 60 + ev.endTime.minute();
            if (endMin <= startMin)
                endMin = startMin + 60;
            qreal yStart = topOffset + (startMin / 60.0) * hourHeight;
            qreal hBox   = qMax(24.0, ((endMin - startMin) / 60.0) * hourHeight);
            QRectF evRect(x + 3.0, yStart + 1.0, colW - 6.0, hBox - 2.0);
            if (evRect.contains(QPointF(event->pos()))) {
                emit m_owner->eventClicked(ev);
                return;
            }
        }
    }

    void mouseDoubleClickEvent(QMouseEvent *event) override
    {
        const int gutterW = 56;
        if (event->pos().x() < gutterW)
            return;
        qreal colW = (width() - gutterW) / 7.0;
        int c = qBound(0, int((event->pos().x() - gutterW) / colW), 6);
        QDate d = m_owner->weekStartDate().addDays(c);
        emit m_owner->dateDoubleClicked(d);
    }

private:
    WeekCalendarViewWidget *m_owner;
    const QDate *m_selectedDate;
    const QList<CalendarEventItem> *m_events;
    const QString *m_searchQuery;
};

} // namespace

WeekCalendarViewWidget::WeekCalendarViewWidget(QWidget *parent)
    : QWidget(parent)
{
    QVBoxLayout *v = new QVBoxLayout(this);
    v->setContentsMargins(0, 0, 0, 0);
    v->setSpacing(0);

    m_headerBar = new WeekHeaderAndAllDayBar(this, &m_selectedDate, &m_events, &m_searchQuery);
    v->addWidget(m_headerBar);

    QScrollArea *scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setStyleSheet("QScrollArea { background: #FFFFFF; border: none; }");

    m_gridCanvas = new WeekTimelineGridCanvas(this, &m_selectedDate, &m_events, &m_searchQuery, scroll);
    scroll->setWidget(m_gridCanvas);
    v->addWidget(scroll, 1);

    // Scroll to current time (3 hours above current hour so the red marker is centered)
    const int initScrollY = qMax(0, (QTime::currentTime().hour() - 3) * 54);
    scroll->verticalScrollBar()->setValue(initScrollY);
}

QDate WeekCalendarViewWidget::weekStartDate() const
{
    const bool sundayFirst = (MailI18n::calendarWeekStartDay() == 7);
    const int offset = sundayFirst ? (m_selectedDate.dayOfWeek() % 7) : (m_selectedDate.dayOfWeek() - 1);
    return m_selectedDate.addDays(-offset);
}

void WeekCalendarViewWidget::setSelectedDate(const QDate &date)
{
    m_selectedDate = date;
    if (m_headerBar) m_headerBar->update();
    if (m_gridCanvas) m_gridCanvas->update();
}

void WeekCalendarViewWidget::setEvents(const QList<CalendarEventItem> &events)
{
    m_events = events;
    if (m_headerBar) m_headerBar->update();
    if (m_gridCanvas) m_gridCanvas->update();
}

void WeekCalendarViewWidget::setSearchFilter(const QString &query)
{
    m_searchQuery = query.trimmed();
    if (m_headerBar) m_headerBar->update();
    if (m_gridCanvas) m_gridCanvas->update();
}

// ============================================================================
// 7. DayCalendarViewWidget (Day View:
//    Single-day centered header + "All day" row + 24h DayTimelineWidget)
// ============================================================================
namespace {

class DayHeaderAndAllDayBar : public QWidget {
public:
    explicit DayHeaderAndAllDayBar(DayCalendarViewWidget *owner,
                                   const QDate *selectedDate,
                                   const QList<CalendarEventItem> *events,
                                   const QString *searchQuery)
        : QWidget(owner)
        , m_owner(owner)
        , m_selectedDate(selectedDate)
        , m_events(events)
        , m_searchQuery(searchQuery)
    {
        setFixedHeight(88);
        setCursor(Qt::PointingHandCursor);
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing, true);
        p.fillRect(rect(), QColor("#FFFFFF"));

        const int gutterW = 56;
        const int topHeaderH = 56;
        const int allDayH = 32;
        const qreal colW = width() - gutterW;

        p.setPen(QPen(QColor("#F1F5F9"), 1.0));
        p.drawLine(gutterW, 6, gutterW, height());

        // Centered weekday name ("화") + #0D47A1 blue circle day number ("3")
        p.setPen(QColor("#6B7280"));
        QFont dowFont("Segoe UI", 8);
        p.setFont(dowFont);
        p.drawText(QRectF(gutterW, 6, colW, 16), Qt::AlignCenter, shortDowName(m_selectedDate->dayOfWeek()));

        QRectF numCircle(gutterW + (colW - 28.0) / 2.0, 23.0, 28.0, 28.0);
        p.setPen(Qt::NoPen);
        p.setBrush(QColor("#0D47A1"));
        p.drawEllipse(numCircle);
        p.setPen(QColor("#FFFFFF"));
        QFont nf("Segoe UI", 11, QFont::Bold);
        p.setFont(nf);
        p.drawText(numCircle, Qt::AlignCenter, QString::number(m_selectedDate->day()));

        // Horizontal line above All day row
        p.setPen(QPen(QColor("#E5E7EB"), 1.0));
        p.drawLine(0, topHeaderH, width(), topHeaderH);

        // "All day" label
        p.setPen(QColor("#9CA3AF"));
        QFont adFont("Segoe UI", 8);
        p.setFont(adFont);
        p.drawText(QRect(4, topHeaderH, gutterW - 10, allDayH),
                   Qt::AlignRight | Qt::AlignVCenter,
                   MailI18n::t("종일", "All day", "終日"));

        // All-day event pill across full width
        for (const CalendarEventItem &ev : *m_events) {
            if (ev.date != *m_selectedDate || !ev.isAllDay)
                continue;
            if (!m_searchQuery->isEmpty() && !ev.title.contains(*m_searchQuery, Qt::CaseInsensitive))
                continue;

            QRectF pillRect(gutterW + 4.0, topHeaderH + 5.0, colW - 10.0, 21.0);
            p.setPen(Qt::NoPen);
            p.setBrush(ev.pillBgColor);
            p.drawRoundedRect(pillRect, 3.0, 3.0);

            p.setBrush(ev.pillBarColor);
            p.drawRoundedRect(QRectF(pillRect.left(), pillRect.top() + 2.0, 2.5, pillRect.height() - 4.0), 1.0, 1.0);

            p.setPen(ev.pillTextColor);
            QFont ef("Segoe UI", 8);
            p.setFont(ef);
            p.drawText(pillRect.adjusted(8.0, 0, -4.0, 0), Qt::AlignLeft | Qt::AlignVCenter, ev.title);
            break;
        }

        p.setPen(QPen(QColor("#E5E7EB"), 1.0));
        p.drawLine(0, height() - 1, width(), height() - 1);
    }

    void mousePressEvent(QMouseEvent *event) override
    {
        if (event->pos().y() >= 56 && event->pos().x() >= 56) {
            for (const CalendarEventItem &ev : *m_events) {
                if (ev.date != *m_selectedDate || !ev.isAllDay)
                    continue;
                if (!m_searchQuery->isEmpty() && !ev.title.contains(*m_searchQuery, Qt::CaseInsensitive))
                    continue;
                emit m_owner->eventClicked(ev);
                return;
            }
        }
    }

    void mouseDoubleClickEvent(QMouseEvent *) override
    {
        emit m_owner->dateDoubleClicked(*m_selectedDate);
    }

private:
    DayCalendarViewWidget *m_owner;
    const QDate *m_selectedDate;
    const QList<CalendarEventItem> *m_events;
    const QString *m_searchQuery;
};

} // namespace

DayCalendarViewWidget::DayCalendarViewWidget(QWidget *parent)
    : QWidget(parent)
{
    QVBoxLayout *v = new QVBoxLayout(this);
    v->setContentsMargins(0, 0, 0, 0);
    v->setSpacing(0);

    m_headerBar = new DayHeaderAndAllDayBar(this, &m_selectedDate, &m_events, &m_searchQuery);
    v->addWidget(m_headerBar);

    QScrollArea *scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setStyleSheet("QScrollArea { background: #FFFFFF; border: none; }");

    m_timeline = new DayTimelineWidget(scroll);
    m_timeline->setIncludeAllDayInGrid(false);
    m_timeline->setMarkerTime(QTime::currentTime());
    scroll->setWidget(m_timeline);
    v->addWidget(scroll, 1);

    const int initScrollY = qMax(0, (QTime::currentTime().hour() - 3) * 54);
    scroll->verticalScrollBar()->setValue(initScrollY);

    connect(m_timeline, &DayTimelineWidget::eventClicked, this, &DayCalendarViewWidget::eventClicked);
    connect(m_timeline, &DayTimelineWidget::slotClicked, this, [this](const QTime &) {
        emit dateDoubleClicked(m_selectedDate);
    });
}

void DayCalendarViewWidget::setSelectedDate(const QDate &date)
{
    m_selectedDate = date;
    if (m_headerBar) m_headerBar->update();
    if (m_timeline)  m_timeline->setCurrentDate(date);
}

void DayCalendarViewWidget::setEvents(const QList<CalendarEventItem> &events)
{
    m_events = events;
    if (m_headerBar) m_headerBar->update();
    if (m_timeline)  m_timeline->setEvents(events);
}

void DayCalendarViewWidget::setSearchFilter(const QString &query)
{
    m_searchQuery = query.trimmed();
    if (m_headerBar) m_headerBar->update();
    if (m_timeline)  m_timeline->setSearchFilter(query);
}

// ============================================================================
// 8. AgendaCalendarViewWidget (Agenda View:
//    Date headers "화요일, 11월 (3)", "수요일, 11월 18", "목요일, 1월 28, 2027" +
//    3px vertical color bar + event title & "All day" / "8:00 오전 - 9:00 오전")
// ============================================================================
namespace {

class AgendaListCanvas : public QWidget {
public:
    explicit AgendaListCanvas(AgendaCalendarViewWidget *owner,
                              const QDate *selectedDate,
                              const QList<CalendarEventItem> *events,
                              const QString *searchQuery,
                              QWidget *parent = nullptr)
        : QWidget(parent)
        , m_owner(owner)
        , m_selectedDate(selectedDate)
        , m_events(events)
        , m_searchQuery(searchQuery)
    {
        setCursor(Qt::PointingHandCursor);
    }

    QMap<QDate, QList<CalendarEventItem>> groupedEvents() const
    {
        QMap<QDate, QList<CalendarEventItem>> groups;
        for (const CalendarEventItem &ev : *m_events) {
            if (!ev.date.isValid())
                continue;
            if (ev.date < QDate(m_selectedDate->year(), m_selectedDate->month(), 1))
                continue;
            if (!m_searchQuery->isEmpty() && !ev.title.contains(*m_searchQuery, Qt::CaseInsensitive))
                continue;
            groups[ev.date].append(ev);
        }
        return groups;
    }

    int requiredHeight() const
    {
        auto groups = groupedEvents();
        if (groups.isEmpty())
            return 300;
        int h = 12;
        for (auto it = groups.constBegin(); it != groups.constEnd(); ++it) {
            h += 44; // Date header
            h += it.value().size() * 54; // Event rows
        }
        return h + 24;
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing, true);
        p.fillRect(rect(), QColor("#FFFFFF"));

        auto groups = groupedEvents();
        if (groups.isEmpty()) {
            p.setPen(QColor("#9CA3AF"));
            QFont f("Segoe UI", 10);
            p.setFont(f);
            p.drawText(rect(), Qt::AlignCenter,
                       MailI18n::t("표시할 예정된 일정이 없습니다.", "No upcoming events found.", "予定されているイベントはありません。"));
            return;
        }

        int y = 0;
        for (auto it = groups.constBegin(); it != groups.constEnd(); ++it) {
            const QDate d = it.key();
            const QList<CalendarEventItem> &dayEvs = it.value();

            // Date group header bar (44px height)
            QRect hdrRect(0, y, width(), 44);
            p.fillRect(hdrRect, QColor("#FAFAFB"));
            p.setPen(QPen(QColor("#F1F5F9"), 1.0));
            p.drawLine(0, y, width(), y);
            p.drawLine(0, y + 43, width(), y + 43);

            QFont hdrFont("Segoe UI", 9, QFont::DemiBold);
            p.setFont(hdrFont);
            p.setPen(QColor("#1F2937"));

            QString prefix = QString("%1, %2월 ").arg(longDowName(d.dayOfWeek())).arg(d.month());
            if (MailI18n::currentLanguage() == AppLanguage::English) {
                prefix = QString("%1, %2 ").arg(longDowName(d.dayOfWeek()), QLocale(QLocale::English).toString(d, "MMM"));
            } else if (MailI18n::currentLanguage() == AppLanguage::Japanese) {
                prefix = QString("%1, %2月 ").arg(longDowName(d.dayOfWeek())).arg(d.month());
            }

            QFontMetrics fm(hdrFont);
            int px = 18;
            p.drawText(QRect(px, y, fm.horizontalAdvance(prefix) + 4, 44), Qt::AlignLeft | Qt::AlignVCenter, prefix);
            px += fm.horizontalAdvance(prefix) + 2;

            bool isActiveDate = (d == *m_selectedDate);
            if (isActiveDate) {
                QRectF circle(px, y + 10.0, 24.0, 24.0);
                p.setPen(Qt::NoPen);
                p.setBrush(QColor("#0D47A1"));
                p.drawEllipse(circle);
                p.setPen(QColor("#FFFFFF"));
                QFont bf("Segoe UI", 9, QFont::Bold);
                p.setFont(bf);
                p.drawText(circle, Qt::AlignCenter, QString::number(d.day()));
                px += 26;
            } else {
                QString dayPart = QString::number(d.day());
                if (d.year() != m_selectedDate->year()) {
                    dayPart += QString(", %1").arg(d.year());
                }
                p.setFont(hdrFont);
                p.setPen(QColor("#1F2937"));
                p.drawText(QRect(px, y, 160, 44), Qt::AlignLeft | Qt::AlignVCenter, dayPart);
            }
            y += 44;

            // Event rows under this date (54px each)
            for (const CalendarEventItem &ev : dayEvs) {
                QRect rowRect(0, y, width(), 54);
                p.fillRect(rowRect, QColor("#FFFFFF"));

                // 3px vertical colored bar on the left
                p.setPen(Qt::NoPen);
                p.setBrush(ev.pillBarColor);
                p.drawRoundedRect(QRectF(18.0, y + 10.0, 3.0, 34.0), 1.5, 1.5);

                // Line 1: Event title (bold)
                p.setPen(QColor("#1F2937"));
                QFont tf("Segoe UI", 9, QFont::DemiBold);
                p.setFont(tf);
                p.drawText(QRect(30, y + 8, width() - 48, 20), Qt::AlignLeft | Qt::AlignVCenter, ev.title);

                // Line 2: "All day" or "8:00 오전 - 9:00 오전"
                p.setPen(QColor("#6B7280"));
                QFont sf("Segoe UI", 8);
                p.setFont(sf);
                p.drawText(QRect(30, y + 27, width() - 48, 18), Qt::AlignLeft | Qt::AlignVCenter, formatEventTimeRange(ev));

                p.setPen(QPen(QColor("#F1F5F9"), 1.0));
                p.drawLine(0, y + 53, width(), y + 53);
                y += 54;
            }
        }
    }

    void mousePressEvent(QMouseEvent *event) override
    {
        auto groups = groupedEvents();
        int y = 0;
        const int clickY = event->pos().y();
        for (auto it = groups.constBegin(); it != groups.constEnd(); ++it) {
            const QDate d = it.key();
            const QList<CalendarEventItem> &dayEvs = it.value();
            if (clickY >= y && clickY < y + 44) {
                emit m_owner->dateClicked(d);
                return;
            }
            y += 44;
            for (const CalendarEventItem &ev : dayEvs) {
                if (clickY >= y && clickY < y + 54) {
                    emit m_owner->dateClicked(d);
                    emit m_owner->eventClicked(ev);
                    return;
                }
                y += 54;
            }
        }
    }

private:
    AgendaCalendarViewWidget *m_owner;
    const QDate *m_selectedDate;
    const QList<CalendarEventItem> *m_events;
    const QString *m_searchQuery;
};

} // namespace

AgendaCalendarViewWidget::AgendaCalendarViewWidget(QWidget *parent)
    : QWidget(parent)
{
    QVBoxLayout *v = new QVBoxLayout(this);
    v->setContentsMargins(0, 0, 0, 0);
    v->setSpacing(0);

    QScrollArea *scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setStyleSheet("QScrollArea { background: #FFFFFF; border: none; }");

    m_canvas = new AgendaListCanvas(this, &m_selectedDate, &m_events, &m_searchQuery, scroll);
    scroll->setWidget(m_canvas);
    v->addWidget(scroll, 1);
}

void AgendaCalendarViewWidget::rebuildCanvasSize()
{
    if (auto *c = static_cast<AgendaListCanvas *>(m_canvas)) {
        c->setMinimumHeight(c->requiredHeight());
        c->update();
    }
}

void AgendaCalendarViewWidget::setSelectedDate(const QDate &date)
{
    m_selectedDate = date;
    rebuildCanvasSize();
}

void AgendaCalendarViewWidget::setEvents(const QList<CalendarEventItem> &events)
{
    m_events = events;
    rebuildCanvasSize();
}

void AgendaCalendarViewWidget::setSearchFilter(const QString &query)
{
    m_searchQuery = query.trimmed();
    rebuildCanvasSize();
}



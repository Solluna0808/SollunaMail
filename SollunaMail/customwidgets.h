#ifndef CUSTOMWIDGETS_H
#define CUSTOMWIDGETS_H

#include <QWidget>
#include <QDate>
#include <QTime>
#include <QList>
#include <QSet>
#include <QStyledItemDelegate>
#include "mailmodels.h"
#include "mailtheme.h"

// 1. Privacy Hub / Settings Capsule Toggle Switch
class ToggleSwitchWidget : public QWidget {
    Q_OBJECT
public:
    explicit ToggleSwitchWidget(bool checked = false, QWidget *parent = nullptr);
    bool isChecked() const { return m_checked; }
    void setChecked(bool checked);
    QSize sizeHint() const override { return QSize(42, 24); }

signals:
    void toggled(bool checked);

protected:
    void paintEvent(QPaintEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;

private:
    bool m_checked = false;
};

// 2. 3-Line Conversation Item Card
class ConversationCardWidget : public QWidget {
    Q_OBJECT
public:
    explicit ConversationCardWidget(const ConversationItem &conv,
                                    const MailThemePalette &palette,
                                    bool isSelected = false,
                                    bool privacyBlur = false,
                                    QWidget *parent = nullptr);
    void setSelectedState(bool selected);
    void setPaletteState(const MailThemePalette &palette);
    void setPrivacyBlur(bool blur);
    QString conversationId() const { return m_conv.id; }
    QSize sizeHint() const override { return QSize(310, 78); }

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    ConversationItem m_conv;
    MailThemePalette m_palette;
    bool m_selected = false;
    bool m_privacyBlur = false;
};

// 2-B. Virtualized Conversation List Delegate (60FPS zero-widget virtual scrolling)
class ConversationListDelegate : public QStyledItemDelegate {
    Q_OBJECT
public:
    explicit ConversationListDelegate(const QList<ConversationItem> *conversations,
                                      const MailThemePalette &palette,
                                      QObject *parent = nullptr);
    void setPaletteState(const MailThemePalette &palette);
    void setPrivacyBlur(bool blur);
    void setCheckedConvIds(const QSet<QString> *checkedIds);
    void paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const override;
    QSize sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const override;

private:
    const QList<ConversationItem> *m_conversations = nullptr;
    const QSet<QString> *m_checkedConvIds = nullptr;
    MailThemePalette m_palette;
    bool m_privacyBlur = false;
};

// 3. Right Docked Calendar Day Timeline Widget with Red Current-Time Marker (1:55)
class DayTimelineWidget : public QWidget {
    Q_OBJECT
public:
    explicit DayTimelineWidget(QWidget *parent = nullptr);
    void setCurrentDate(const QDate &date);
    void setEvents(const QList<CalendarEventItem> &events);
    void setMarkerTime(const QTime &time);
    void setIncludeAllDayInGrid(bool includeAllDay);
    void setSearchFilter(const QString &query);
    QSize sizeHint() const override { return QSize(280, 24 * 54 + 20); }

signals:
    void slotClicked(const QTime &time);
    void eventClicked(const CalendarEventItem &ev);

protected:
    void paintEvent(QPaintEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;

private:
    QDate m_date = QDate::currentDate();
    QTime m_markerTime = QTime::currentTime();
    QList<CalendarEventItem> m_events;
    bool m_includeAllDay = true;
    QString m_searchQuery;
};

// 4. Mini Month Calendar Widget for Full Calendar Left Sidebar
class MiniMonthCalendarWidget : public QWidget {
    Q_OBJECT
public:
    explicit MiniMonthCalendarWidget(QWidget *parent = nullptr);
    void setYearMonth(int year, int month);
    void setSelectedDate(const QDate &date);
    QDate selectedDate() const { return m_selectedDate; }
    int currentYear() const { return m_year; }
    int currentMonth() const { return m_month; }
    QSize sizeHint() const override { return QSize(205, 195); }

signals:
    void dateSelected(const QDate &date);
    void monthChanged(int year, int month);

protected:
    void paintEvent(QPaintEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;

private:
    int m_year = QDate::currentDate().year();
    int m_month = QDate::currentDate().month();
    QDate m_selectedDate = QDate::currentDate();
};

// 5. Full-Screen Calendar Month Grid Widget (Month View)
class MonthCalendarGridWidget : public QWidget {
    Q_OBJECT
public:
    explicit MonthCalendarGridWidget(QWidget *parent = nullptr);
    void setYearMonth(int year, int month);
    void setSelectedDate(const QDate &date);
    void setEvents(const QList<CalendarEventItem> &events);
    void setSearchFilter(const QString &query);

signals:
    void dateClicked(const QDate &date);
    void dateDoubleClicked(const QDate &date);
    void eventClicked(const CalendarEventItem &ev);

protected:
    void paintEvent(QPaintEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;

private:
    QDate gridStartDate() const;
    QDate dateAtPos(const QPoint &pos) const;
    bool eventAtPos(const QPoint &pos, CalendarEventItem &outEv) const;
    int m_year = QDate::currentDate().year();
    int m_month = QDate::currentDate().month();
    QDate m_selectedDate = QDate::currentDate();
    QList<CalendarEventItem> m_events;
    QString m_searchQuery;
};

// 6. Full-Screen Calendar Week View Widget (7-day header + All day row + 24h 7-col timeline)
class WeekCalendarViewWidget : public QWidget {
    Q_OBJECT
public:
    explicit WeekCalendarViewWidget(QWidget *parent = nullptr);
    void setSelectedDate(const QDate &date);
    void setEvents(const QList<CalendarEventItem> &events);
    void setSearchFilter(const QString &query);
    QDate weekStartDate() const;

signals:
    void dateClicked(const QDate &date);
    void dateDoubleClicked(const QDate &date);
    void eventClicked(const CalendarEventItem &ev);

private:
    QDate m_selectedDate = QDate::currentDate();
    QList<CalendarEventItem> m_events;
    QString m_searchQuery;
    QWidget *m_headerBar = nullptr;
    QWidget *m_gridCanvas = nullptr;
};

// 7. Full-Screen Calendar Day View Widget (Single-day header + All day row + 24h timeline)
class DayCalendarViewWidget : public QWidget {
    Q_OBJECT
public:
    explicit DayCalendarViewWidget(QWidget *parent = nullptr);
    void setSelectedDate(const QDate &date);
    void setEvents(const QList<CalendarEventItem> &events);
    void setSearchFilter(const QString &query);

signals:
    void dateClicked(const QDate &date);
    void dateDoubleClicked(const QDate &date);
    void eventClicked(const CalendarEventItem &ev);

private:
    QDate m_selectedDate = QDate::currentDate();
    QList<CalendarEventItem> m_events;
    QString m_searchQuery;
    QWidget *m_headerBar = nullptr;
    DayTimelineWidget *m_timeline = nullptr;
};

// 8. Full-Screen Calendar Agenda View Widget (Date groups + 3px color bar + title & time)
class AgendaCalendarViewWidget : public QWidget {
    Q_OBJECT
public:
    explicit AgendaCalendarViewWidget(QWidget *parent = nullptr);
    void setSelectedDate(const QDate &date);
    void setEvents(const QList<CalendarEventItem> &events);
    void setSearchFilter(const QString &query);

signals:
    void dateClicked(const QDate &date);
    void eventClicked(const CalendarEventItem &ev);

private:
    QDate m_selectedDate = QDate::currentDate();
    QList<CalendarEventItem> m_events;
    QString m_searchQuery;
    QWidget *m_canvas = nullptr;
    void rebuildCanvasSize();
};

#endif // CUSTOMWIDGETS_H


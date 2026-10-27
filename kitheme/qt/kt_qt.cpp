// KiTheme Qt style (Qt5 and Qt6 builds of the same file).
//
// The only C++ in KiTheme: QStyle is a C++ class and Qt finds styles
// through a moc'd plugin, so this file can't be C. It stays a thin
// adapter. A QProxyStyle over Fusion (which keeps the layout of every
// widget it doesn't override) hands each element to libkitheme, which
// draws it with Cairo straight into the memory of a QImage --
// CAIRO_FORMAT_ARGB32 and Format_ARGB32_Premultiplied are the same
// layout, so there is no conversion. Results go through QPixmapCache,
// like Fusion's own, keyed by element, state, size and the theme serial.
//
// The palette comes from kitheme.conf too, and a QFileSystemWatcher on its
// directory reloads it live (one inotify watch per application).
#include <QAbstractButton>
#include <QApplication>
#include <QCheckBox>
#include <QBitmap>
#include <QCommonStyle>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QEvent>
#include <QFileInfo>
#include <QFileSystemWatcher>
#include <QHash>
#include <QLabel>
#include <QMenu>
#include <QMenuBar>
#include <QMouseEvent>
#include <QPainter>
#include <QPixmapCache>
#include <QProgressBar>
#include <QProxyStyle>
#include <QPushButton>
#include <QRadioButton>
#include <QStyleFactory>
#include <QStyleOption>
#include <QStylePlugin>
#include <QPointer>
#include <QTimer>
#include <QToolBar>
#include <QToolButton>
#include <QWidget>
#include <QWindow>

#include <cairo.h>

#include "../core/kitheme.h"

namespace {

KtTheme g_theme;
int g_serial;

KtColor kc(const QColor &c)
{
    return KtColor{c.redF(), c.greenF(), c.blueF(), c.alphaF()};
}

QColor qc(KtColor c)
{
    return QColor::fromRgbF(c.r, c.g, c.b, c.a);
}

unsigned stateBits(const QStyleOption *o)
{
    unsigned st = 0;
    if (!(o->state & QStyle::State_Enabled))
        st |= KT_DISABLED;
    else if (o->state & QStyle::State_MouseOver)
        st |= KT_HOVER;
    if (o->state & (QStyle::State_Sunken | QStyle::State_On))
        st |= KT_PRESSED;
    if (o->state & QStyle::State_HasFocus)
        st |= KT_FOCUSED;
    return st;
}

// Draws `r` through libkitheme. `key` must name everything that changes
// the result besides the size (element, state bits, colors).
template <class F>
void paint(QPainter *p, const QRect &r, const QString &key, F draw)
{
    if (r.width() <= 0 || r.height() <= 0)
        return;
    const qreal dpr = p->device() ? p->device()->devicePixelRatioF() : 1.0;
    const QString k = QStringLiteral("kt%1|%2|%3x%4@%5").arg(g_serial).arg(key).arg(r.width()).arg(r.height()).arg(dpr);
    QPixmap pm;
    if (!QPixmapCache::find(k, &pm)) {
        QImage img(qRound(r.width() * dpr), qRound(r.height() * dpr), QImage::Format_ARGB32_Premultiplied);
        img.fill(Qt::transparent);
        cairo_surface_t *s = cairo_image_surface_create_for_data(img.bits(), CAIRO_FORMAT_ARGB32, img.width(),
                                                                 img.height(), img.bytesPerLine());
        cairo_t *cr = cairo_create(s);
        cairo_scale(cr, dpr, dpr);
        draw(cr, double(r.width()), double(r.height()));
        cairo_destroy(cr);
        cairo_surface_destroy(s);
        pm = QPixmap::fromImage(img);
        pm.setDevicePixelRatio(dpr);
        QPixmapCache::insert(k, pm);
    }
    p->drawPixmap(r.topLeft(), pm);
}

QString hex(const QColor &c)
{
    return c.name(QColor::HexArgb);
}

KtDir dirOf(QStyle::PrimitiveElement pe)
{
    switch (pe) {
    case QStyle::PE_IndicatorArrowUp:
    case QStyle::PE_IndicatorSpinUp:
    case QStyle::PE_IndicatorSpinPlus:
        return KT_UP;
    case QStyle::PE_IndicatorArrowLeft:
        return KT_LEFT;
    case QStyle::PE_IndicatorArrowRight:
        return KT_RIGHT;
    default:
        return KT_DOWN;
    }
}

} // namespace

class KiStyle : public QProxyStyle
{
    Q_OBJECT
public:
    KiStyle()
        : QProxyStyle(QStyleFactory::create(QStringLiteral("Fusion")))
    {
        setObjectName(QStringLiteral("KiTheme"));
        reload();
        char path[4096];
        kt_user_conf_path(path, sizeof(path));
        m_conf = QString::fromLocal8Bit(path);
        // The directory catches kiconf replacing the file (a rename, which
        // drops a watch on the old inode); the file catches edits in place.
        QDir().mkpath(QFileInfo(m_conf).absolutePath());
        m_watch.addPath(QFileInfo(m_conf).absolutePath());
        if (QFileInfo::exists(m_conf))
            m_watch.addPath(m_conf);
        connect(&m_watch, &QFileSystemWatcher::directoryChanged, this, &KiStyle::confChanged);
        connect(&m_watch, &QFileSystemWatcher::fileChanged, this, &KiStyle::confChanged);
        m_anim.setInterval(50);
        connect(&m_anim, &QTimer::timeout, this, &KiStyle::animate);
        m_clock.start();
    }

    // ---- palette ----

    QPalette standardPalette() const override
    {
        return themePalette();
    }

    void polish(QPalette &pal) override
    {
        pal = themePalette();
    }

    void polish(QWidget *w) override
    {
        QProxyStyle::polish(w);
        if (qobject_cast<QAbstractButton *>(w) || w->inherits("QComboBox") || w->inherits("QAbstractSpinBox") ||
            w->inherits("QScrollBar") || w->inherits("QSlider") || w->inherits("QTabBar") || w->inherits("QLineEdit") ||
            w->inherits("QHeaderView"))
            w->setAttribute(Qt::WA_Hover, true);
        if (qobject_cast<QMenu *>(w) || (g_theme.window_drag && dragBar(w)))
            w->installEventFilter(this);
    }

    void unpolish(QWidget *w) override
    {
        if (qobject_cast<QMenu *>(w)) {
            w->removeEventFilter(this);
            w->clearMask();
        } else if (dragBar(w)) {
            w->removeEventFilter(this);
        }
        QProxyStyle::unpolish(w);
    }

    // A popup menu is a rectangular window: clip it to the radius
    // kt_draw_menu() draws (a 1-bit mask, so the edge is not antialiased).
    bool eventFilter(QObject *o, QEvent *e) override
    {
        switch (e->type()) {
        case QEvent::MouseButtonPress:
        case QEvent::MouseButtonDblClick:
        case QEvent::MouseMove:
        case QEvent::MouseButtonRelease:
            if (dragBar(o))
                return dragEvent(static_cast<QWidget *>(o), static_cast<QMouseEvent *>(e));
            break;
        default:
            break;
        }
        if (e->type() == QEvent::Show || e->type() == QEvent::Resize) {
            if (auto *m = qobject_cast<QMenu *>(o)) {
                const int r = int(kt_menu_radius(&g_theme) + 0.5);
                if (r <= 0) {
                    if (!m->mask().isEmpty())
                        m->clearMask();
                } else if (m->width() > 2 * r && m->height() > 2 * r) {
                    QBitmap bm(m->size());
                    bm.fill(Qt::color0);
                    QPainter p(&bm);
                    p.setPen(Qt::NoPen);
                    p.setBrush(Qt::color1);
                    p.drawRoundedRect(QRect(QPoint(0, 0), m->size()), r, r);
                    p.end();
                    m->setMask(bm);
                }
            }
        }
        return QProxyStyle::eventFilter(o, e);
    }

    void polish(QApplication *app) override
    {
        QProxyStyle::polish(app);
    }

    // ---- metrics ----

    int pixelMetric(PixelMetric m, const QStyleOption *o = nullptr, const QWidget *w = nullptr) const override
    {
        const KtTheme &t = g_theme;
        const bool classic = t.style == KT_STYLE_CLASSIC;
        switch (m) {
        case PM_ScrollBarExtent:
            return t.scrollbar_width;
        case PM_ScrollBarSliderMin:
            return 24;
        case PM_IndicatorWidth:
        case PM_IndicatorHeight:
        case PM_ExclusiveIndicatorWidth:
        case PM_ExclusiveIndicatorHeight:
            return t.check_size;
        case PM_SliderThickness:
            return 20;
        case PM_SliderLength:
        case PM_SliderControlThickness:
            return 16;
        case PM_ButtonShiftHorizontal:
        case PM_ButtonShiftVertical:
            return classic ? 1 : 0;
        case PM_DefaultFrameWidth:
            return 2;
        case PM_MenuPanelWidth:
            return classic ? 2 : 1;
        case PM_ToolTipLabelFrameWidth:
            return 4;
        default:
            return QProxyStyle::pixelMetric(m, o, w);
        }
    }

    QSize sizeFromContents(ContentsType ct, const QStyleOption *o, const QSize &sz, const QWidget *w) const override
    {
        QSize s = QProxyStyle::sizeFromContents(ct, o, sz, w);
        if (ct == CT_ProgressBar) {
            const bool vertical = !(o->state & State_Horizontal);
            if (vertical)
                s.setWidth(qMax(s.width(), g_theme.progress_thickness));
            else
                s.setHeight(qMax(s.height(), g_theme.progress_thickness));
        } else if (ct == CT_PushButton) {
            s.setHeight(qMax(s.height(), 28));
        }
        return s;
    }

    QRect subElementRect(SubElement se, const QStyleOption *o, const QWidget *w) const override
    {
        switch (se) {
        case SE_ProgressBarGroove:
        case SE_ProgressBarContents:
        case SE_ProgressBarLabel:
            return o->rect; // the text goes inside the bar
        default:
            return QProxyStyle::subElementRect(se, o, w);
        }
    }

    QRect subControlRect(ComplexControl cc, const QStyleOptionComplex *o, SubControl sc, const QWidget *w) const override
    {
        if (cc == CC_ScrollBar && !g_theme.scrollbar_arrows) {
            if (auto sb = qstyleoption_cast<const QStyleOptionSlider *>(o))
                return scrollBarRect(sb, sc);
        }
        return QProxyStyle::subControlRect(cc, o, sc, w);
    }

    int styleHint(StyleHint h, const QStyleOption *o = nullptr, const QWidget *w = nullptr,
                  QStyleHintReturn *r = nullptr) const override
    {
        switch (h) {
        case SH_EtchDisabledText:
            return g_theme.style == KT_STYLE_CLASSIC;
        case SH_ScrollBar_Transient:
            return 0;
        default:
            return QProxyStyle::styleHint(h, o, w, r);
        }
    }

    // ---- primitives ----

    void drawPrimitive(PrimitiveElement pe, const QStyleOption *o, QPainter *p, const QWidget *w = nullptr) const override
    {
        const KtTheme *t = &g_theme;
        const QRect r = o->rect;
        switch (pe) {
        case PE_PanelButtonCommand:
        case PE_PanelButtonBevel: {
            unsigned st = stateBits(o);
            if (auto b = qstyleoption_cast<const QStyleOptionButton *>(o)) {
                if (b->features & QStyleOptionButton::DefaultButton)
                    st |= KT_DEFAULT;
            }
            const QColor bg = o->palette.color(QPalette::Button);
            paint(p, r, QStringLiteral("btn%1%2").arg(st).arg(hex(bg)), [&](cairo_t *cr, double ww, double hh) {
                kt_draw_button(t, cr, 0, 0, ww, hh, st, kc(bg));
            });
            return;
        }
        case PE_PanelButtonTool: {
            if (!(o->state & (State_Sunken | State_On | State_MouseOver | State_Raised)) || !(o->state & State_Enabled))
                if (!(o->state & (State_Sunken | State_On)))
                    return;
            unsigned st = stateBits(o) & ~KT_FOCUSED;
            const QColor bg = o->palette.color(QPalette::Button);
            paint(p, r, QStringLiteral("tbtn%1%2").arg(st).arg(hex(bg)), [&](cairo_t *cr, double ww, double hh) {
                kt_draw_button(t, cr, 0, 0, ww, hh, st, kc(bg));
            });
            return;
        }
        case PE_FrameDefaultButton:
        case PE_FrameButtonBevel:
        case PE_FrameButtonTool:
            return;
        case PE_FrameFocusRect:
            if (w && (qobject_cast<const QPushButton *>(w) || qobject_cast<const QToolButton *>(w)))
                return;
            if (w && w->inherits("QAbstractItemView"))
                return QProxyStyle::drawPrimitive(pe, o, p, w);
            paint(p, r, QStringLiteral("focus"), [&](cairo_t *cr, double ww, double hh) {
                kt_draw_focus(t, cr, 0, 0, ww, hh);
            });
            return;
        case PE_PanelLineEdit: {
            auto f = qstyleoption_cast<const QStyleOptionFrame *>(o);
            const QColor base = o->palette.color(QPalette::Base);
            if (f && f->lineWidth <= 0) {
                p->fillRect(r, base);
                return;
            }
            unsigned st = stateBits(o) & (KT_DISABLED | KT_FOCUSED | KT_HOVER);
            paint(p, r, QStringLiteral("entry%1%2").arg(st).arg(hex(base)), [&](cairo_t *cr, double ww, double hh) {
                kt_draw_entry(t, cr, 0, 0, ww, hh, st, kc(base));
            });
            return;
        }
        case PE_FrameLineEdit:
            return;
        case PE_IndicatorCheckBox:
        case PE_IndicatorRadioButton: {
            unsigned st = stateBits(o) & ~(KT_FOCUSED | KT_PRESSED);
            if (o->state & State_On)
                st |= KT_CHECKED;
            else if (o->state & State_NoChange)
                st |= KT_MIXED;
            if (o->state & State_Sunken)
                st |= KT_PRESSED;
            const bool radio = pe == PE_IndicatorRadioButton;
            paint(p, r, QStringLiteral("%1%2").arg(radio ? "radio" : "check").arg(st), [&](cairo_t *cr, double ww, double hh) {
                if (radio)
                    kt_draw_radio(t, cr, 0, 0, ww, hh, st);
                else
                    kt_draw_check(t, cr, 0, 0, ww, hh, st);
            });
            return;
        }
        case PE_FrameMenu:
        case PE_PanelMenu:
            paint(p, r, QStringLiteral("menu"), [&](cairo_t *cr, double ww, double hh) {
                kt_draw_menu(t, cr, 0, 0, ww, hh);
            });
            return;
        case PE_PanelTipLabel: {
            const QColor bg = o->palette.color(QPalette::ToolTipBase);
            KtTheme tt = *t;
            tt.tooltip_bg = kc(bg);
            paint(p, r, QStringLiteral("tip%1").arg(hex(bg)), [&](cairo_t *cr, double ww, double hh) {
                kt_draw_tooltip(&tt, cr, 0, 0, ww, hh);
            });
            return;
        }
        case PE_FrameTabWidget:
            paint(p, r, QStringLiteral("pane"), [&](cairo_t *cr, double ww, double hh) {
                kt_draw_tab_pane(t, cr, 0, 0, ww, hh);
            });
            return;
        case PE_FrameTabBarBase:
            return;
        case PE_FrameGroupBox:
            paint(p, r, QStringLiteral("group"), [&](cairo_t *cr, double ww, double hh) {
                kt_draw_frame(t, cr, 0, 0, ww, hh, 0, t->window_bg);
            });
            return;
        case PE_Frame: {
            if (!(o->state & (State_Sunken | State_Raised)))
                return QProxyStyle::drawPrimitive(pe, o, p, w);
            const int sunken = o->state & State_Sunken ? 1 : -1;
            paint(p, r, QStringLiteral("frame%1").arg(sunken), [&](cairo_t *cr, double ww, double hh) {
                kt_draw_frame(t, cr, 0, 0, ww, hh, sunken, t->window_bg);
            });
            return;
        }
        case PE_PanelItemViewItem:
            if (o->state & State_Selected) {
                const bool active = o->state & State_Active;
                const QColor sel = o->palette.color(QPalette::Active, QPalette::Highlight);
                KtTheme tt = *t;
                tt.selection_bg = kc(sel);
                paint(p, r, QStringLiteral("row%1%2").arg(active).arg(hex(sel)), [&](cairo_t *cr, double ww, double hh) {
                    kt_draw_row_selection(&tt, cr, 0, 0, ww, hh, active);
                });
                return;
            }
            return QProxyStyle::drawPrimitive(pe, o, p, w);
        case PE_IndicatorArrowUp:
        case PE_IndicatorArrowDown:
        case PE_IndicatorArrowLeft:
        case PE_IndicatorArrowRight:
        case PE_IndicatorSpinUp:
        case PE_IndicatorSpinDown:
        case PE_IndicatorSpinPlus:
        case PE_IndicatorSpinMinus: {
            const QColor fg = o->palette.color(o->state & State_Enabled ? QPalette::Active : QPalette::Disabled,
                                               QPalette::ButtonText);
            const KtDir d = dirOf(pe);
            paint(p, r, QStringLiteral("arrow%1%2").arg(int(d)).arg(hex(fg)), [&](cairo_t *cr, double ww, double hh) {
                kt_draw_arrow(t, cr, 0, 0, ww, hh, d, kc(fg));
            });
            return;
        }
        case PE_PanelMenuBar:
        case PE_PanelToolBar:
            return;
        default:
            return QProxyStyle::drawPrimitive(pe, o, p, w);
        }
    }

    // ---- controls ----

    void drawControl(ControlElement ce, const QStyleOption *o, QPainter *p, const QWidget *w = nullptr) const override
    {
        const KtTheme *t = &g_theme;
        switch (ce) {
        case CE_ProgressBar:
            if (auto pb = qstyleoption_cast<const QStyleOptionProgressBar *>(o)) {
                drawControl(CE_ProgressBarGroove, pb, p, w);
                drawControl(CE_ProgressBarContents, pb, p, w);
                const bool busy = pb->minimum == 0 && pb->maximum == 0;
                if (!busy && (pb->textVisible || t->progress_text == KT_PTEXT_ALWAYS))
                    drawControl(CE_ProgressBarLabel, pb, p, w);
            }
            return;
        case CE_ProgressBarGroove: {
            const bool vertical = !(o->state & State_Horizontal);
            paint(p, o->rect, QStringLiteral("ptrough%1").arg(vertical), [&](cairo_t *cr, double ww, double hh) {
                kt_draw_progress_trough(t, cr, 0, 0, ww, hh, vertical);
            });
            return;
        }
        case CE_ProgressBarContents:
            if (auto pb = qstyleoption_cast<const QStyleOptionProgressBar *>(o)) {
                const QRect bar = barRect(pb);
                if (bar.isEmpty())
                    return;
                const bool vertical = !(o->state & State_Horizontal);
                const bool busy = pb->minimum == 0 && pb->maximum == 0;
                double phase = 0;
                if (busy || (t->progress_animation && t->progress_stripes)) {
                    track(w);
                    phase = (m_clock.elapsed() % 1000) / 1000.0;
                }
                // Animated frames are not cached: every phase is different.
                if (phase > 0) {
                    QImage img(bar.size() * p->device()->devicePixelRatioF(), QImage::Format_ARGB32_Premultiplied);
                    img.setDevicePixelRatio(p->device()->devicePixelRatioF());
                    img.fill(Qt::transparent);
                    cairo_surface_t *s = cairo_image_surface_create_for_data(img.bits(), CAIRO_FORMAT_ARGB32,
                                                                             img.width(), img.height(), img.bytesPerLine());
                    cairo_t *cr = cairo_create(s);
                    cairo_scale(cr, img.devicePixelRatioF(), img.devicePixelRatioF());
                    kt_draw_progress_bar(t, cr, 0, 0, bar.width(), bar.height(), vertical, phase);
                    cairo_destroy(cr);
                    cairo_surface_destroy(s);
                    p->drawImage(bar.topLeft(), img);
                } else {
                    paint(p, bar, QStringLiteral("pbar%1").arg(vertical), [&](cairo_t *cr, double ww, double hh) {
                        kt_draw_progress_bar(t, cr, 0, 0, ww, hh, vertical, 0);
                    });
                }
            }
            return;
        case CE_ProgressBarLabel:
            if (auto pb = qstyleoption_cast<const QStyleOptionProgressBar *>(o)) {
                QString text = pb->text;
                if (text.isEmpty() && pb->maximum > pb->minimum)
                    text = QStringLiteral("%1%").arg(qRound(100.0 * (pb->progress - pb->minimum) / (pb->maximum - pb->minimum)));
                if (text.isEmpty())
                    return;
                const QRect bar = barRect(pb);
                p->save();
                // Over the bar in the selection text color, elsewhere in
                // the window text color: the Oxygen look.
                QRegion outside = QRegion(pb->rect).subtracted(bar);
                p->setClipRegion(outside);
                p->setPen(pb->palette.color(QPalette::WindowText));
                p->drawText(pb->rect, Qt::AlignCenter, text);
                p->setClipRect(bar);
                p->setPen(pb->palette.color(QPalette::HighlightedText));
                p->drawText(pb->rect, Qt::AlignCenter, text);
                p->restore();
            }
            return;
        case CE_MenuItem:
            if (auto mi = qstyleoption_cast<const QStyleOptionMenuItem *>(o)) {
                if ((mi->state & State_Selected) && (mi->state & State_Enabled) &&
                    mi->menuItemType != QStyleOptionMenuItem::Separator) {
                    paint(p, mi->rect, QStringLiteral("mitem"), [&](cairo_t *cr, double ww, double hh) {
                        kt_draw_menuitem(t, cr, 0, 0, ww, hh, 0);
                    });
                    QStyleOptionMenuItem copy = *mi;
                    copy.state &= ~State_Selected;
                    const QColor hl = mi->palette.color(QPalette::HighlightedText);
                    for (auto role : {QPalette::Text, QPalette::WindowText, QPalette::ButtonText})
                        copy.palette.setColor(role, hl);
                    // Fusion fills unselected items with the menu
                    // background: draw the item over a transparent one.
                    copy.palette.setColor(QPalette::Window, Qt::transparent);
                    copy.palette.setColor(QPalette::Base, Qt::transparent);
                    copy.palette.setColor(QPalette::Button, Qt::transparent);
                    return QProxyStyle::drawControl(ce, &copy, p, w);
                }
            }
            return QProxyStyle::drawControl(ce, o, p, w);
        case CE_MenuBarItem:
            if (auto mi = qstyleoption_cast<const QStyleOptionMenuItem *>(o)) {
                p->fillRect(mi->rect, mi->palette.color(QPalette::Window));
                QStyleOptionMenuItem copy = *mi;
                if ((mi->state & State_Selected) && (mi->state & State_Enabled)) {
                    paint(p, mi->rect, QStringLiteral("mbitem"), [&](cairo_t *cr, double ww, double hh) {
                        kt_draw_menuitem(t, cr, 0, 0, ww, hh, 1);
                    });
                    copy.palette.setColor(QPalette::ButtonText, mi->palette.color(QPalette::HighlightedText));
                }
                // QCommonStyle's version only draws the text.
                auto common = static_cast<QCommonStyle *>(baseStyle());
                common->QCommonStyle::drawControl(ce, &copy, p, w);
            }
            return;
        case CE_MenuBarEmptyArea:
            p->fillRect(o->rect, o->palette.color(QPalette::Window));
            return;
        case CE_TabBarTabShape:
            if (auto tab = qstyleoption_cast<const QStyleOptionTab *>(o)) {
                KtDir gap = KT_DOWN;
                switch (tab->shape) {
                case QTabBar::RoundedSouth:
                case QTabBar::TriangularSouth:
                    gap = KT_UP;
                    break;
                case QTabBar::RoundedWest:
                case QTabBar::TriangularWest:
                    gap = KT_RIGHT;
                    break;
                case QTabBar::RoundedEast:
                case QTabBar::TriangularEast:
                    gap = KT_LEFT;
                    break;
                default:
                    break;
                }
                unsigned st = 0;
                if (tab->state & State_Selected)
                    st |= KT_SELECTED;
                else if (tab->state & State_MouseOver)
                    st |= KT_HOVER;
                paint(p, tab->rect, QStringLiteral("tab%1-%2").arg(int(gap)).arg(st), [&](cairo_t *cr, double ww, double hh) {
                    kt_draw_tab(t, cr, 0, 0, ww, hh, gap, st);
                });
            }
            return;
        case CE_HeaderSection: {
            unsigned st = 0;
            if (o->state & State_Sunken)
                st |= KT_PRESSED;
            else if (o->state & State_MouseOver)
                st |= KT_HOVER;
            paint(p, o->rect, QStringLiteral("header%1").arg(st), [&](cairo_t *cr, double ww, double hh) {
                kt_draw_header(t, cr, 0, 0, ww, hh, st);
            });
            return;
        }
        case CE_HeaderEmptyArea:
            paint(p, o->rect, QStringLiteral("header0"), [&](cairo_t *cr, double ww, double hh) {
                kt_draw_header(t, cr, 0, 0, ww, hh, 0);
            });
            return;
        default:
            return QProxyStyle::drawControl(ce, o, p, w);
        }
    }

    // ---- complex controls ----

    void drawComplexControl(ComplexControl cc, const QStyleOptionComplex *o, QPainter *p,
                            const QWidget *w = nullptr) const override
    {
        const KtTheme *t = &g_theme;
        switch (cc) {
        case CC_ScrollBar:
            if (auto sb = qstyleoption_cast<const QStyleOptionSlider *>(o)) {
                const bool vertical = sb->orientation == Qt::Vertical;
                paint(p, sb->rect, QStringLiteral("strough%1").arg(vertical), [&](cairo_t *cr, double ww, double hh) {
                    kt_draw_scroll_trough(t, cr, 0, 0, ww, hh, vertical);
                });
                const QColor bg = sb->palette.color(QPalette::Button);
                const QColor fg = sb->palette.color(sb->state & State_Enabled ? QPalette::Active : QPalette::Disabled,
                                                    QPalette::ButtonText);
                const struct {
                    SubControl sc;
                    KtDir dir;
                } steppers[] = {
                    {SC_ScrollBarSubLine, vertical ? KT_UP : KT_LEFT},
                    {SC_ScrollBarAddLine, vertical ? KT_DOWN : KT_RIGHT},
                };
                for (const auto &s : steppers) {
                    const QRect r = subControlRect(cc, sb, s.sc, w);
                    if (r.isEmpty())
                        continue;
                    unsigned st = subState(sb, s.sc);
                    paint(p, r, QStringLiteral("step%1-%2%3%4").arg(st).arg(int(s.dir)).arg(hex(bg)).arg(hex(fg)),
                          [&](cairo_t *cr, double ww, double hh) {
                              kt_draw_button(t, cr, 0, 0, ww, hh, st, kc(bg));
                              kt_draw_arrow(t, cr, 0, 0, ww, hh, s.dir, kc(fg));
                          });
                }
                const QRect slider = subControlRect(cc, sb, SC_ScrollBarSlider, w);
                if (!slider.isEmpty() && sb->maximum > sb->minimum) {
                    unsigned st = subState(sb, SC_ScrollBarSlider);
                    paint(p, slider, QStringLiteral("slider%1-%2").arg(st).arg(vertical), [&](cairo_t *cr, double ww, double hh) {
                        kt_draw_scroll_slider(t, cr, 0, 0, ww, hh, vertical, st);
                    });
                }
            }
            return;
        case CC_Slider:
            if (auto sl = qstyleoption_cast<const QStyleOptionSlider *>(o)) {
                const bool vertical = sl->orientation == Qt::Vertical;
                const QRect groove = subControlRect(cc, sl, SC_SliderGroove, w);
                const QRect handle = subControlRect(cc, sl, SC_SliderHandle, w);
                if ((sl->subControls & SC_SliderTickmarks) && sl->tickPosition != QSlider::NoTicks) {
                    QStyleOptionSlider ticks = *sl;
                    ticks.subControls = SC_SliderTickmarks;
                    QProxyStyle::drawComplexControl(cc, &ticks, p, w);
                }
                if (sl->subControls & SC_SliderGroove) {
                    double fill = vertical ? groove.bottom() - handle.center().y() : handle.center().x() - groove.left();
                    if (sl->upsideDown != vertical)
                        fill = (vertical ? groove.height() : groove.width()) - fill;
                    const int f = qRound(fill);
                    paint(p, groove, QStringLiteral("groove%1-%2").arg(vertical).arg(f), [&](cairo_t *cr, double ww, double hh) {
                        kt_draw_scale_groove(t, cr, 0, 0, ww, hh, vertical, f);
                    });
                }
                if (sl->subControls & SC_SliderHandle) {
                    unsigned st = subState(sl, SC_SliderHandle);
                    paint(p, handle, QStringLiteral("handle%1").arg(st), [&](cairo_t *cr, double ww, double hh) {
                        kt_draw_scale_handle(t, cr, 0, 0, ww, hh, st);
                    });
                }
            }
            return;
        case CC_ComboBox:
            if (auto cb = qstyleoption_cast<const QStyleOptionComboBox *>(o)) {
                unsigned st = stateBits(cb) & ~KT_PRESSED;
                if (cb->state & State_On)
                    st |= KT_PRESSED;
                if (cb->editable) {
                    const QColor base = cb->palette.color(QPalette::Base);
                    st &= ~KT_PRESSED;
                    paint(p, cb->rect, QStringLiteral("entry%1%2").arg(st).arg(hex(base)), [&](cairo_t *cr, double ww, double hh) {
                        kt_draw_entry(t, cr, 0, 0, ww, hh, st, kc(base));
                    });
                } else if (cb->frame) {
                    const QColor bg = cb->palette.color(QPalette::Button);
                    paint(p, cb->rect, QStringLiteral("btn%1%2").arg(st).arg(hex(bg)), [&](cairo_t *cr, double ww, double hh) {
                        kt_draw_button(t, cr, 0, 0, ww, hh, st, kc(bg));
                    });
                }
                QStyleOption arrow = *cb;
                arrow.rect = subControlRect(cc, cb, SC_ComboBoxArrow, w);
                drawPrimitive(PE_IndicatorArrowDown, &arrow, p, w);
            }
            return;
        case CC_SpinBox:
            if (auto sp = qstyleoption_cast<const QStyleOptionSpinBox *>(o)) {
                unsigned st = stateBits(sp) & (KT_DISABLED | KT_FOCUSED | KT_HOVER);
                const QColor base = sp->palette.color(QPalette::Base);
                if (sp->frame)
                    paint(p, sp->rect, QStringLiteral("entry%1%2").arg(st).arg(hex(base)), [&](cairo_t *cr, double ww, double hh) {
                        kt_draw_entry(t, cr, 0, 0, ww, hh, st, kc(base));
                    });
                if (sp->buttonSymbols == QAbstractSpinBox::NoButtons)
                    return;
                const QColor bg = sp->palette.color(QPalette::Button);
                const QColor fg = sp->palette.color(QPalette::ButtonText);
                const struct {
                    SubControl sc;
                    KtDir dir;
                    QAbstractSpinBox::StepEnabledFlag flag;
                } buttons[] = {
                    {SC_SpinBoxUp, KT_UP, QAbstractSpinBox::StepUpEnabled},
                    {SC_SpinBoxDown, KT_DOWN, QAbstractSpinBox::StepDownEnabled},
                };
                for (const auto &b : buttons) {
                    QRect r = subControlRect(cc, sp, b.sc, w).adjusted(0, 1, -1, -1);
                    unsigned bst = subState(sp, b.sc);
                    if (!(sp->stepEnabled & b.flag))
                        bst |= KT_DISABLED;
                    QColor afg = bst & KT_DISABLED ? sp->palette.color(QPalette::Disabled, QPalette::ButtonText) : fg;
                    paint(p, r, QStringLiteral("spin%1-%2%3%4").arg(bst).arg(int(b.dir)).arg(hex(bg)).arg(hex(afg)),
                          [&](cairo_t *cr, double ww, double hh) {
                              kt_draw_button(t, cr, 0, 0, ww, hh, bst, kc(bg));
                              kt_draw_arrow(t, cr, 0, 0, ww, hh, b.dir, kc(afg));
                          });
                }
            }
            return;
        default:
            return QProxyStyle::drawComplexControl(cc, o, p, w);
        }
    }

private Q_SLOTS:
    void confChanged()
    {
        if (QFileInfo::exists(m_conf) && !m_watch.files().contains(m_conf))
            m_watch.addPath(m_conf);
        const QDateTime m = QFileInfo(m_conf).lastModified();
        if (m == m_mtime)
            return;
        reload();
        QApplication::setPalette(themePalette());
        // Metrics may have changed too (scrollbar width, check size).
        const auto widgets = QApplication::allWidgets();
        for (QWidget *w : widgets) {
            QEvent e(QEvent::StyleChange);
            QApplication::sendEvent(w, &e);
            w->update();
            if (dragBar(w) && w->style() == this) {
                if (g_theme.window_drag)
                    w->installEventFilter(this);
                else
                    w->removeEventFilter(this);
            }
        }
    }

    void animate()
    {
        const qint64 now = m_clock.elapsed();
        for (auto it = m_animated.begin(); it != m_animated.end();) {
            QWidget *w = static_cast<QWidget *>(it.key());
            if (!w->isVisible() || now - it.value() > 1000) {
                disconnect(w, &QObject::destroyed, this, nullptr);
                it = m_animated.erase(it);
                continue;
            }
            w->update();
            ++it;
        }
        if (m_animated.isEmpty())
            m_anim.stop();
    }

private:
    // Not virtual: also called from the reload slot.
    QPalette themePalette() const
    {
        const KtTheme &t = g_theme;
        QPalette p;
        auto set = [&](QPalette::ColorRole role, KtColor c, KtColor disabled) {
            p.setColor(QPalette::Active, role, qc(c));
            p.setColor(QPalette::Inactive, role, qc(c));
            p.setColor(QPalette::Disabled, role, qc(disabled));
        };
        KtColor dimw = kt_mix(t.window_fg, t.window_bg, 0.55);
        set(QPalette::Window, t.window_bg, t.window_bg);
        set(QPalette::WindowText, t.window_fg, dimw);
        set(QPalette::Base, t.base_bg, t.window_bg);
        set(QPalette::AlternateBase, kt_shade(t.base_bg, 0.96), t.window_bg);
        set(QPalette::Text, t.base_fg, kt_mix(t.base_fg, t.base_bg, 0.55));
        set(QPalette::Button, t.button_bg, kt_mix(t.button_bg, t.window_bg, 0.5));
        set(QPalette::ButtonText, t.button_fg, kt_mix(t.button_fg, t.button_bg, 0.55));
        set(QPalette::BrightText, KtColor{1, 1, 1, 1}, KtColor{1, 1, 1, 1});
        set(QPalette::Highlight, t.selection_bg, kt_mix(t.selection_bg, t.window_bg, 0.5));
        set(QPalette::HighlightedText, t.selection_fg, t.selection_fg);
        set(QPalette::ToolTipBase, t.tooltip_bg, t.tooltip_bg);
        set(QPalette::ToolTipText, t.tooltip_fg, t.tooltip_fg);
        set(QPalette::Link, t.accent, t.accent);
        set(QPalette::LinkVisited, kt_shade(t.accent, 0.8), kt_shade(t.accent, 0.8));
        set(QPalette::Light, kt_shade(t.button_bg, 1.3), kt_shade(t.button_bg, 1.3));
        set(QPalette::Midlight, kt_shade(t.button_bg, 1.1), kt_shade(t.button_bg, 1.1));
        set(QPalette::Mid, kt_shade(t.button_bg, 0.8), kt_shade(t.button_bg, 0.8));
        set(QPalette::Dark, kt_shade(t.button_bg, 0.6), kt_shade(t.button_bg, 0.6));
        set(QPalette::Shadow, KtColor{0, 0, 0, 1}, KtColor{0, 0, 0, 1});
        set(QPalette::PlaceholderText, dimw, dimw);
        // An unfocused window keeps its selection visible, just softer.
        p.setColor(QPalette::Inactive, QPalette::Highlight, qc(kt_mix(t.selection_bg, t.base_bg, 0.45)));
        return p;
    }

    void reload()
    {
        kt_theme_load_user(&g_theme);
        m_mtime = QFileInfo(m_conf).lastModified();
        g_serial++;
    }

    // ---- window_drag: moving the window from empty parts of bars ----

    static bool dragBar(const QObject *o)
    {
        return qobject_cast<const QMenuBar *>(o) || qobject_cast<const QToolBar *>(o);
    }

    // Nothing the user could mean to click at `pos` (bar coordinates): no
    // menu title, no tool button or field, not the toolbar's own move handle.
    static bool emptyAt(QWidget *bar, const QPoint &pos)
    {
        if (bar->isWindow())
            return false;
        if (auto *mb = qobject_cast<QMenuBar *>(bar)) {
            if (mb->actionAt(pos))
                return false;
        } else if (auto *tb = qobject_cast<QToolBar *>(bar)) {
            if (tb->isFloating())
                return false;
            if (tb->isMovable()) {
                QStyleOptionToolBar opt;
                opt.initFrom(tb);
                opt.features = QStyleOptionToolBar::Movable;
                if (tb->orientation() == Qt::Horizontal)
                    opt.state |= State_Horizontal;
                if (tb->style()->subElementRect(SE_ToolBarHandle, &opt, tb).contains(pos))
                    return false;
            }
        }
        QWidget *c = bar->childAt(pos);
        if (!c || c->inherits("QToolBarSeparator"))
            return true;
        auto *label = qobject_cast<QLabel *>(c);
        return label && !(label->textInteractionFlags() & (Qt::TextSelectableByMouse | Qt::LinksAccessibleByMouse));
    }

    static QPoint globalPos(const QMouseEvent *me)
    {
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
        return me->globalPosition().toPoint();
#else
        return me->globalPos();
#endif
    }

    // A press on an empty spot is held back from the bar; once the pointer
    // moves past the drag distance the WM takes over (startSystemMove, i.e.
    // _NET_WM_MOVERESIZE on X11). A click that never moves just does
    // nothing, which is what the bar would have done with it anyway.
    bool dragEvent(QWidget *bar, QMouseEvent *me)
    {
        if (!g_theme.window_drag) {
            m_dragBar = nullptr;
            return false;
        }
        switch (me->type()) {
        case QEvent::MouseButtonPress:
        case QEvent::MouseButtonDblClick:
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
            if (me->button() != Qt::LeftButton || me->modifiers() != Qt::NoModifier || !emptyAt(bar, me->position().toPoint()))
#else
            if (me->button() != Qt::LeftButton || me->modifiers() != Qt::NoModifier || !emptyAt(bar, me->pos()))
#endif
                return false;
            m_dragBar = bar;
            m_dragFrom = globalPos(me);
            return true;
        case QEvent::MouseMove:
            if (m_dragBar != bar)
                return false;
            if (!(me->buttons() & Qt::LeftButton)) {
                m_dragBar = nullptr;
                return false;
            }
            if ((globalPos(me) - m_dragFrom).manhattanLength() < QApplication::startDragDistance())
                return true;
            m_dragBar = nullptr;
#if QT_VERSION >= QT_VERSION_CHECK(5, 15, 0)
            if (QWindow *win = bar->window()->windowHandle())
                win->startSystemMove();
#endif
            return true;
        case QEvent::MouseButtonRelease:
            if (m_dragBar != bar)
                return false;
            m_dragBar = nullptr;
            return true;
        default:
            return false;
        }
    }

    // Repaint `w` every 50 ms while it keeps being painted animated.
    void track(const QWidget *w) const
    {
        if (!w)
            return;
        auto self = const_cast<KiStyle *>(this);
        QObject *key = const_cast<QWidget *>(w);
        if (!m_animated.contains(key))
            connect(key, &QObject::destroyed, self, [self](QObject *o) { self->m_animated.remove(o); });
        self->m_animated.insert(key, m_clock.elapsed());
        if (!m_anim.isActive())
            self->m_anim.start();
    }

    static unsigned subState(const QStyleOptionComplex *o, SubControl sc)
    {
        unsigned st = 0;
        if (!(o->state & State_Enabled))
            return KT_DISABLED;
        if (o->activeSubControls & sc) {
            if (o->state & State_Sunken)
                st |= KT_PRESSED;
            else if (o->state & State_MouseOver)
                st |= KT_HOVER;
        }
        return st;
    }

    QRect barRect(const QStyleOptionProgressBar *pb) const
    {
        const QRect r = pb->rect.adjusted(1, 1, -1, -1);
        const bool vertical = !(pb->state & State_Horizontal);
        const bool busy = pb->minimum == 0 && pb->maximum == 0;
        const int len = vertical ? r.height() : r.width();
        if (busy) {
            // A quarter-length block sliding back and forth.
            const int block = qMax(len / 4, 10);
            const qint64 period = 2000, tt = m_clock.elapsed() % period;
            const double pos = tt < period / 2 ? tt / double(period / 2) : 2 - tt / double(period / 2);
            const int off = int(pos * (len - block));
            return vertical ? QRect(r.left(), r.top() + off, r.width(), block) : QRect(r.left() + off, r.top(), block, r.height());
        }
        const qint64 span = qint64(pb->maximum) - pb->minimum;
        if (span <= 0)
            return QRect();
        const int filled = int(len * double(pb->progress - pb->minimum) / span);
        bool inverted = pb->invertedAppearance;
        if (!vertical && pb->direction == Qt::RightToLeft)
            inverted = !inverted;
        if (vertical) {
            // Vertical bars grow upward unless inverted.
            return pb->bottomToTop != inverted ? QRect(r.left(), r.bottom() - filled + 1, r.width(), filled)
                                               : QRect(r.left(), r.top(), r.width(), filled);
        }
        return inverted ? QRect(r.right() - filled + 1, r.top(), filled, r.height()) : QRect(r.left(), r.top(), filled, r.height());
    }

    // Without stepper buttons the groove is the whole bar.
    QRect scrollBarRect(const QStyleOptionSlider *sb, SubControl sc) const
    {
        const QRect r = sb->rect;
        const bool vertical = sb->orientation == Qt::Vertical;
        switch (sc) {
        case SC_ScrollBarSubLine:
        case SC_ScrollBarAddLine:
            return QRect();
        case SC_ScrollBarGroove:
            return r;
        default:
            break;
        }
        const int len = vertical ? r.height() : r.width();
        const int range = sb->maximum - sb->minimum;
        int slen = len;
        if (range > 0) {
            slen = int(double(len) * sb->pageStep / (range + sb->pageStep));
            slen = qBound(qMin(24, len), slen, len);
        }
        const int pos = sliderPositionFromValue(sb->minimum, sb->maximum, sb->sliderPosition, len - slen, sb->upsideDown);
        const QRect slider = vertical ? QRect(r.left(), r.top() + pos, r.width(), slen) : QRect(r.left() + pos, r.top(), slen, r.height());
        switch (sc) {
        case SC_ScrollBarSlider:
            return slider;
        case SC_ScrollBarSubPage:
            return vertical ? QRect(r.left(), r.top(), r.width(), pos) : QRect(r.left(), r.top(), pos, r.height());
        case SC_ScrollBarAddPage:
            return vertical ? QRect(r.left(), slider.bottom() + 1, r.width(), r.bottom() - slider.bottom())
                            : QRect(slider.right() + 1, r.top(), r.right() - slider.right(), r.height());
        default:
            return QRect();
        }
    }

    QString m_conf;
    QDateTime m_mtime;
    QFileSystemWatcher m_watch;
    QTimer m_anim;
    QElapsedTimer m_clock;
    QHash<QObject *, qint64> m_animated;
    QPointer<QWidget> m_dragBar; // bar holding back a press, until it moves or is released
    QPoint m_dragFrom;
};

class KiThemePlugin : public QStylePlugin
{
    Q_OBJECT
    Q_PLUGIN_METADATA(IID QStyleFactoryInterface_iid FILE "kitheme.json")
public:
    QStyle *create(const QString &key) override
    {
        if (key.compare(QLatin1String("KiTheme"), Qt::CaseInsensitive) == 0)
            return new KiStyle;
        return nullptr;
    }
};

#include "kt_qt.moc"

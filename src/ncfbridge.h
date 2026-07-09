// NcfBridge — a moc'd QObject providing real slots for the "Repair covers" More-menu action:
//   onRepairTapped() — enumerate the library, then drive a chunked, main-thread backfill that mirrors
//                      each book's on-disk cover into our ContentID-keyed cache, with a progress dialog.
//   onTick()         — one chunk of that backfill per event-loop tick (keeps the UI responsive; never
//                      blocks e-ink). Slot bodies live in nickelcoverfix.cc so they can reach the
//                      resolved libnickel symbols.
#ifndef NCF_BRIDGE_H
#define NCF_BRIDGE_H

#include <QObject>
#include <QString>
#include <QStringList>
#include <QVector>

// Set in ncf_init() to ConfirmationDialogFactory::showOKDialog(QString const&, QString const&) [static].
extern void (*ncf_showOKDialog)(const QString &title, const QString &text);

class NcfBridge : public QObject {
    Q_OBJECT
public:
    explicit NcfBridge(QObject *parent = nullptr) : QObject(parent) {}
public slots:
    void onRepairTapped();
    void onTick();
private:
    QVector<QStringList> m_work;   // per book: [ librarySrc, libraryDst, lockSrc, lockDst ]
    int      m_idx     = 0;
    int      m_copied  = 0;
    void    *m_dlg     = nullptr;              // QDialog* (our custom progress dialog)
    void    *m_bar     = nullptr;              // QProgressBar*
    QObject *m_timer   = nullptr;              // QTimer*
    bool     m_running = false;
};

#endif

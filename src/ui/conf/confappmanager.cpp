#include "confappmanager.h"

#include <QLoggingCategory>

#include <sqlite/sqlitedb.h>
#include <sqlite/sqlitestmt.h>

#include <appinfo/appinfocache.h>
#include <appinfo/appinfoutil.h>
#include <conf/app.h>
#include <driver/drivermanager.h>
#include <log/logentryblocked.h>
#include <log/logmanager.h>
#include <manager/drivelistmanager.h>
#include <manager/envmanager.h>
#include <manager/windowmanager.h>
#include <util/conf/confutil.h>
#include <util/fileutil.h>
#include <util/ioc/ioccontainer.h>

#include "appgroup.h"
#include "confmanager.h"
#include "firewallconf.h"

namespace {

const QLoggingCategory LC("confApp");

constexpr int APP_END_TIMER_INTERVAL_MIN = 100;
constexpr int APP_END_TIMER_INTERVAL_MAX = 24 * 60 * 60 * 1000; // 1 day

const char *const sqlSelectAppPaths = "SELECT app_id, path FROM app;";

#define SELECT_APP_FIELDS                                                                          \
    "    t.app_id,"                                                                                \
    "    t.origin_path,"                                                                           \
    "    t.path,"                                                                                  \
    "    t.is_wildcard,"                                                                           \
    "    t.use_group_perm,"                                                                        \
    "    t.apply_child,"                                                                           \
    "    t.kill_child,"                                                                            \
    "    t.lan_only,"                                                                              \
    "    t.log_blocked,"                                                                           \
    "    t.log_conn,"                                                                              \
    "    t.blocked,"                                                                               \
    "    t.kill_process,"                                                                          \
    "    t.accept_zones,"                                                                          \
    "    t.reject_zones,"                                                                          \
    "    g.order_index as group_index,"                                                            \
    "    (alert.app_id IS NOT NULL) as alerted"

const char *const sqlSelectAppById = "SELECT" SELECT_APP_FIELDS "  FROM app t"
                                     "    JOIN app_group g ON g.app_group_id = t.app_group_id"
                                     "    LEFT JOIN app_alert alert ON alert.app_id = t.app_id"
                                     "    WHERE t.app_id = ?1;";

const char *const sqlSelectApps = "SELECT" SELECT_APP_FIELDS "  FROM app t"
                                  "    JOIN app_group g ON g.app_group_id = t.app_group_id"
                                  "    LEFT JOIN app_alert alert ON alert.app_id = t.app_id;";

const char *const sqlSelectMinEndApp = "SELECT MIN(end_time) FROM app"
                                       "  WHERE end_time != 0 AND blocked = 0;";

const char *const sqlSelectEndedApps = "SELECT" SELECT_APP_FIELDS "  FROM app t"
                                       "    JOIN app_group g ON g.app_group_id = t.app_group_id"
                                       "    LEFT JOIN app_alert alert ON alert.app_id = t.app_id"
                                       "  WHERE end_time <= ?1 AND blocked = 0;";

const char *const sqlSelectAppIdByPath = "SELECT app_id FROM app WHERE path = ?1;";

const char *const sqlUpsertApp = "INSERT INTO app(app_group_id, origin_path, path, name,"
                                 "    is_wildcard, use_group_perm, apply_child, kill_child,"
                                 "    lan_only, log_blocked, log_conn, blocked, kill_process,"
                                 "    accept_zones, reject_zones, end_time, creat_time)"
                                 "  VALUES(?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9,"
                                 "    ?10, ?11, ?12, ?13, ?14, ?15, ?16, ?17)"
                                 "  ON CONFLICT(path) DO UPDATE"
                                 "  SET app_group_id = ?1, origin_path = ?2, name = ?4,"
                                 "    is_wildcard = ?5, use_group_perm = ?6,"
                                 "    apply_child = ?7, kill_child = ?8,"
                                 "    lan_only = ?9, log_blocked = ?10, log_conn = ?11,"
                                 "    blocked = ?12, kill_process = ?13,"
                                 "    accept_zones = ?14, reject_zones = ?15, end_time = ?16"
                                 "  RETURNING app_id;";

const char *const sqlInsertAppAlert = "INSERT INTO app_alert(app_id) VALUES(?1);";

const char *const sqlDeleteApp = "DELETE FROM app WHERE app_id = ?1 RETURNING path, is_wildcard;";

const char *const sqlDeleteAppAlert = "DELETE FROM app_alert WHERE app_id = ?1;";

const char *const sqlUpdateApp = "UPDATE app"
                                 "  SET app_group_id = ?2, origin_path = ?3, path = ?4,"
                                 "    name = ?5, is_wildcard = ?6, use_group_perm = ?7,"
                                 "    apply_child = ?8, kill_child = ?9, lan_only = ?10,"
                                 "    log_blocked = ?11, log_conn = ?12,"
                                 "    blocked = ?13, kill_process = ?14,"
                                 "    accept_zones = ?15, reject_zones = ?16, end_time = ?17"
                                 "  WHERE app_id = ?1;";

const char *const sqlUpdateAppName = "UPDATE app SET name = ?2 WHERE app_id = ?1;";

const char *const sqlUpdateAppBlocked = "UPDATE app SET blocked = ?2, kill_process = ?3,"
                                        "    end_time = NULL"
                                        "  WHERE app_id = ?1;";

using AppsMap = QHash<qint64, QString>;
using AppIdsArray = QVector<qint64>;

void showErrorMessage(const QString &errorMessage)
{
    IoC<WindowManager>()->showErrorBox(errorMessage, ConfManager::tr("App Configuration Error"));
}

}

ConfAppManager::ConfAppManager(QObject *parent) : QObject(parent)
{
    connect(&m_appAlertedTimer, &QTimer::timeout, this, &ConfAppManager::appAlerted);
    connect(&m_appChangedTimer, &QTimer::timeout, this, &ConfAppManager::appChanged);
    connect(&m_appUpdatedTimer, &QTimer::timeout, this, &ConfAppManager::appUpdated);

    m_appEndTimer.setSingleShot(true);
    connect(&m_appEndTimer, &QTimer::timeout, this, &ConfAppManager::updateAppEndTimes);
}

ConfManager *ConfAppManager::confManager() const
{
    return m_confManager;
}

SqliteDb *ConfAppManager::sqliteDb() const
{
    return confManager()->sqliteDb();
}

FirewallConf *ConfAppManager::conf() const
{
    return confManager()->conf();
}

void ConfAppManager::setUp()
{
    m_confManager = IoC()->setUpDependency<ConfManager>();

    purgeAppsOnStart();

    setupAppEndTimer();

    setupDriveListManager();
}

void ConfAppManager::setupDriveListManager()
{
    connect(IoC<DriveListManager>(), &DriveListManager::driveMaskChanged, this,
            [&](quint32 addedMask, quint32 /*removedMask*/) {
                if ((m_driveMask & addedMask) != 0) {
                    updateDriverConf();
                }
            });
}

void ConfAppManager::purgeAppsOnStart()
{
    if (conf()->ini().progPurgeOnStart()) {
        purgeApps();
    }
}

void ConfAppManager::setupAppEndTimer()
{
    auto logManager = IoC<LogManager>();

    connect(logManager, &LogManager::systemTimeChanged, this, &ConfAppManager::updateAppEndTimer);

    updateAppEndTimer();
}

void ConfAppManager::updateAppEndTimer()
{
    const qint64 endTimeMsecs = sqliteDb()->executeEx(sqlSelectMinEndApp).toLongLong();

    if (endTimeMsecs != 0) {
        const qint64 currentMsecs = QDateTime::currentMSecsSinceEpoch();
        const qint64 deltaMsecs = endTimeMsecs - currentMsecs;
        const int interval = qMax(
                (deltaMsecs > 0 ? int(qMin(deltaMsecs, qint64(APP_END_TIMER_INTERVAL_MAX))) : 0),
                APP_END_TIMER_INTERVAL_MIN);

        m_appEndTimer.start(interval);
    } else {
        m_appEndTimer.stop();
    }
}

void ConfAppManager::emitAppAlerted()
{
    m_appAlertedTimer.startTrigger();
}

void ConfAppManager::emitAppChanged()
{
    m_appChangedTimer.startTrigger();
}

void ConfAppManager::emitAppUpdated()
{
    m_appUpdatedTimer.startTrigger();
}

void ConfAppManager::logBlockedApp(const LogEntryBlocked &logEntry)
{
    const QString appOriginPath = logEntry.path();
    const QString appPath = FileUtil::normalizePath(appOriginPath);

    if (appIdByPath(appPath) > 0)
        return; // already added by user

    const QString appName = IoC<AppInfoCache>()->appName(appOriginPath);

    App app;
    app.blocked = logEntry.blocked();
    app.alerted = true;
    app.groupIndex = 0; // "Main" app. group
    app.appOriginPath = appOriginPath;
    app.appPath = appPath;
    app.appName = appName;

    const bool ok = addOrUpdateApp(app);
    if (ok) {
        emitAppAlerted();
    }
}

qint64 ConfAppManager::appIdByPath(const QString &appPath)
{
    return sqliteDb()->executeEx(sqlSelectAppIdByPath, { appPath }).toLongLong();
}

bool ConfAppManager::addApp(const App &app)
{
    if (!addOrUpdateApp(app))
        return false;

    updateDriverUpdateAppConf(app);

    return true;
}

void ConfAppManager::deleteApps(const QVector<qint64> &appIdList)
{
    bool isWildcard = false;

    for (const qint64 appId : appIdList) {
        deleteApp(appId, isWildcard);
    }

    if (isWildcard) {
        updateDriverConf();
    }
}

bool ConfAppManager::deleteApp(qint64 appId, bool &isWildcard)
{
    bool ok = false;

    beginTransaction();

    const auto vars = QVariantList() << appId;

    const auto resList = sqliteDb()->executeEx(sqlDeleteApp, vars, 2, &ok).toList();

    if (ok) {
        sqliteDb()->executeEx(sqlDeleteAppAlert, vars, 0, &ok);
    }

    commitTransaction(ok);

    if (ok) {
        const QString appPath = resList.at(0).toString();

        if (resList.at(1).toBool()) {
            isWildcard = true;
        } else {
            updateDriverDeleteApp(appPath);
        }

        emitAppChanged();
    }

    return ok;
}

bool ConfAppManager::purgeApps()
{
    QVector<qint64> appIdList;

    // Collect non-existent apps
    {
        SqliteStmt stmt;
        if (!sqliteDb()->prepare(stmt, sqlSelectAppPaths))
            return false;

        while (stmt.step() == SqliteStmt::StepRow) {
            const QString appPath = stmt.columnText(1);

            if (FileUtil::isDriveFilePath(appPath) && !AppInfoUtil::fileExists(appPath)) {
                const qint64 appId = stmt.columnInt64(0);
                appIdList.append(appId);

                qCDebug(LC) << "Purge obsolete app:" << appId << appPath;
            }
        }
    }

    // Delete apps
    deleteApps(appIdList);

    return true;
}

bool ConfAppManager::updateApp(const App &app)
{
    const AppGroup *appGroup = conf()->appGroupAt(app.groupIndex);
    if (appGroup->isNull())
        return false;

    bool ok = false;

    beginTransaction();

    const auto vars = QVariantList()
            << app.appId << appGroup->id() << app.appOriginPath
            << (!app.appPath.isEmpty() ? app.appPath : QVariant()) << app.appName << app.isWildcard
            << app.useGroupPerm << app.applyChild << app.killChild << app.lanOnly << app.logBlocked
            << app.logConn << app.blocked << app.killProcess << app.acceptZones << app.rejectZones
            << (!app.endTime.isNull() ? app.endTime : QVariant());

    sqliteDb()->executeEx(sqlUpdateApp, vars, 0, &ok);

    if (ok) {
        sqliteDb()->executeEx(sqlDeleteAppAlert, { app.appId }, 0, &ok);
    }

    commitTransaction(ok);

    if (ok) {
        if (!app.endTime.isNull()) {
            updateAppEndTimer();
        }

        emitAppUpdated();

        updateDriverUpdateAppConf(app);
    }

    return ok;
}

void ConfAppManager::updateAppsBlocked(
        const QVector<qint64> &appIdList, bool blocked, bool killProcess)
{
    bool isWildcard = (appIdList.size() > 7);

    for (const qint64 appId : appIdList) {
        updateAppBlocked(appId, blocked, killProcess, isWildcard);
    }

    if (isWildcard) {
        updateDriverConf();
    }
}

bool ConfAppManager::updateAppBlocked(
        qint64 appId, bool blocked, bool killProcess, bool &isWildcard)
{
    App app;
    app.appId = appId;
    if (!loadAppById(app))
        return false;

    if (!prepareAppBlocked(app, blocked, killProcess) || !saveAppBlocked(app))
        return false;

    if (app.isWildcard) {
        isWildcard = true;
    } else {
        updateDriverUpdateApp(app);
    }

    return true;
}

bool ConfAppManager::prepareAppBlocked(App &app, bool blocked, bool killProcess)
{
    const bool wasAlerted = app.alerted;
    app.alerted = false;

    if (!wasAlerted) {
        if (app.blocked == blocked && app.killProcess == killProcess)
            return false;
    }

    app.blocked = blocked;
    app.killProcess = killProcess;

    return true;
}

bool ConfAppManager::updateAppName(qint64 appId, const QString &appName)
{
    bool ok = false;

    const auto vars = QVariantList() << appId << appName;

    sqliteDb()->executeEx(sqlUpdateAppName, vars, 0, &ok);

    checkEndTransaction(ok);

    if (ok) {
        emitAppUpdated();
    }

    return ok;
}

bool ConfAppManager::walkApps(const std::function<walkAppsCallback> &func)
{
    SqliteStmt stmt;
    if (!sqliteDb()->prepare(stmt, sqlSelectApps))
        return false;

    while (stmt.step() == SqliteStmt::StepRow) {
        App app;
        fillApp(app, stmt);

        if (!func(app))
            return false;
    }

    return true;
}

bool ConfAppManager::saveAppBlocked(const App &app)
{
    bool ok = true;

    beginTransaction();

    const auto vars = QVariantList() << app.appId << app.blocked << app.killProcess;

    sqliteDb()->executeEx(sqlUpdateAppBlocked, vars, 0, &ok);

    if (ok) {
        sqliteDb()->executeEx(sqlDeleteAppAlert, { app.appId }, 0, &ok);
    }

    commitTransaction(ok);

    if (ok) {
        emitAppUpdated();
    }

    return ok;
}

void ConfAppManager::updateAppEndTimes()
{
    SqliteStmt stmt;
    if (!stmt.prepare(sqliteDb()->db(), sqlSelectEndedApps))
        return;

    stmt.bindDateTime(1, QDateTime::currentDateTime());

    while (stmt.step() == SqliteStmt::StepRow) {
        App app;
        fillApp(app, stmt);

        app.blocked = true;
        app.killProcess = false;

        updateApp(app);
    }

    updateAppEndTimer();
}

bool ConfAppManager::updateDriverConf(bool onlyFlags)
{
    ConfUtil confUtil;
    QByteArray buf;

    const int confSize = onlyFlags ? confUtil.writeFlags(*conf(), buf)
                                   : confUtil.write(*conf(), this, *IoC<EnvManager>(), buf);
    if (confSize == 0) {
        showErrorMessage(confUtil.errorMessage());
        return false;
    }

    auto driverManager = IoC<DriverManager>();
    if (!driverManager->writeConf(buf, confSize, onlyFlags)) {
        showErrorMessage(driverManager->errorMessage());
        return false;
    }

    m_driveMask = confUtil.driveMask();

    return true;
}

bool ConfAppManager::addOrUpdateApp(const App &app)
{
    const AppGroup *appGroup = conf()->appGroupAt(app.groupIndex);
    if (appGroup->isNull())
        return false;

    bool ok = false;

    beginTransaction();

    const auto vars = QVariantList()
            << appGroup->id() << app.appOriginPath
            << (!app.appPath.isEmpty() ? app.appPath : QVariant()) << app.appName << app.isWildcard
            << app.useGroupPerm << app.applyChild << app.killChild << app.lanOnly << app.logBlocked
            << app.logConn << app.blocked << app.killProcess << app.acceptZones << app.rejectZones
            << (!app.endTime.isNull() ? app.endTime : QVariant()) << QDateTime::currentDateTime();

    const auto appIdVar = sqliteDb()->executeEx(sqlUpsertApp, vars, 1, &ok);

    if (ok) {
        // Alert
        const qint64 appId = appIdVar.toLongLong();
        sqliteDb()->executeEx(app.alerted ? sqlInsertAppAlert : sqlDeleteAppAlert, { appId });
    }

    commitTransaction(ok);

    if (ok) {
        if (!app.endTime.isNull()) {
            updateAppEndTimer();
        }

        emitAppChanged();
    }

    return ok;
}

bool ConfAppManager::loadAppById(App &app)
{
    SqliteStmt stmt;
    if (!sqliteDb()->prepare(stmt, sqlSelectAppById))
        return false;

    stmt.bindInt64(1, app.appId);
    if (stmt.step() != SqliteStmt::StepRow)
        return false;

    fillApp(app, stmt);

    return true;
}

void ConfAppManager::fillApp(App &app, const SqliteStmt &stmt)
{
    app.appId = stmt.columnInt64(0);
    app.appOriginPath = stmt.columnText(1);
    app.appPath = stmt.columnText(2);
    app.isWildcard = stmt.columnBool(3);
    app.useGroupPerm = stmt.columnBool(4);
    app.applyChild = stmt.columnBool(5);
    app.killChild = stmt.columnBool(6);
    app.lanOnly = stmt.columnBool(7);
    app.logBlocked = stmt.columnBool(8);
    app.logConn = stmt.columnBool(9);
    app.blocked = stmt.columnBool(10);
    app.killProcess = stmt.columnBool(11);
    app.acceptZones = stmt.columnUInt(12);
    app.rejectZones = stmt.columnUInt(13);
    app.groupIndex = stmt.columnInt(14);
    app.alerted = stmt.columnBool(15);
}

bool ConfAppManager::updateDriverDeleteApp(const QString &appPath)
{
    App app;
    app.appPath = appPath;

    return updateDriverUpdateApp(app, /*remove=*/true);
}

bool ConfAppManager::updateDriverUpdateApp(const App &app, bool remove)
{
    ConfUtil confUtil;
    QByteArray buf;

    const int entrySize = confUtil.writeAppEntry(app, /*isNew=*/false, buf);

    if (entrySize == 0) {
        showErrorMessage(confUtil.errorMessage());
        return false;
    }

    auto driverManager = IoC<DriverManager>();
    if (!driverManager->writeApp(buf, entrySize, remove)) {
        showErrorMessage(driverManager->errorMessage());
        return false;
    }

    m_driveMask |= remove ? 0 : confUtil.driveMask();

    return true;
}

bool ConfAppManager::updateDriverUpdateAppConf(const App &app)
{
    return app.isWildcard ? updateDriverConf() : updateDriverUpdateApp(app);
}

bool ConfAppManager::beginTransaction()
{
    return sqliteDb()->beginTransaction();
}

bool ConfAppManager::commitTransaction(bool ok)
{
    ok = sqliteDb()->endTransaction(ok);

    return checkEndTransaction(ok);
}

bool ConfAppManager::checkEndTransaction(bool ok)
{
    if (!ok) {
        showErrorMessage(sqliteDb()->errorMessage());
    }

    return ok;
}

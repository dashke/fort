#include "fortmanager.h"

#include <QApplication>
#include <QLoggingCategory>
#include <QMessageBox>
#include <QThreadPool>

#include <sqlite/sqlitedb.h>

#include <fort_version.h>

#include <appinfo/appinfocache.h>
#include <conf/firewallconf.h>
#include <control/controlmanager.h>
#include <driver/drivercommon.h>
#include <form/dialog/passworddialog.h>
#include <fortsettings.h>
#include <hostinfo/hostinfocache.h>
#include <manager/drivelistmanager.h>
#include <manager/envmanager.h>
#include <manager/hotkeymanager.h>
#include <manager/logger.h>
#include <manager/nativeeventfilter.h>
#include <manager/servicemanager.h>
#include <manager/translationmanager.h>
#include <model/zonelistmodel.h>
#include <rpc/appinfomanagerrpc.h>
#include <rpc/askpendingmanagerrpc.h>
#include <rpc/confappmanagerrpc.h>
#include <rpc/confmanagerrpc.h>
#include <rpc/confzonemanagerrpc.h>
#include <rpc/drivermanagerrpc.h>
#include <rpc/logmanagerrpc.h>
#include <rpc/quotamanagerrpc.h>
#include <rpc/rpcmanager.h>
#include <rpc/serviceinfomanagerrpc.h>
#include <rpc/statblockmanagerrpc.h>
#include <rpc/statmanagerrpc.h>
#include <rpc/taskmanagerrpc.h>
#include <rpc/windowmanagerfake.h>
#include <task/taskinfozonedownloader.h>
#include <user/usersettings.h>
#include <util/dateutil.h>
#include <util/fileutil.h>
#include <util/ioc/ioccontainer.h>
#include <util/osutil.h>
#include <util/startuputil.h>

namespace {

const QLoggingCategory LC("fortManager");

void dbErrorHandler(void *context, int errCode, const char *message)
{
    qCWarning(LC) << "DB Error:" << errCode << message;

    if (SqliteDb::isIoError(errCode)) {
        auto fortManager = static_cast<FortManager *>(context);

        QMetaObject::invokeMethod(fortManager, &FortManager::onDbIoError, Qt::QueuedConnection);
    }
}

inline void setupMasterServices(IocContainer *ioc, const FortSettings *settings)
{
    ioc->setService(new ConfManager(settings->confFilePath()));
    ioc->setService(new ConfAppManager());
    ioc->setService(new ConfZoneManager());
    ioc->setService(new QuotaManager());
    ioc->setService(new StatManager(settings->statFilePath()));
    ioc->setService(new StatBlockManager(settings->statBlockFilePath()));
    ioc->setService(new AskPendingManager());
    ioc->setService(new DriverManager());
    ioc->setService(new AppInfoManager(settings->cacheFilePath()));
    ioc->setService(new LogManager());
    ioc->setService(new ServiceInfoManager());
    ioc->setService(new TaskManager());
}

inline void setupClientServices(IocContainer *ioc, const FortSettings *settings)
{
    ioc->setService<ConfManager>(new ConfManagerRpc(settings->confFilePath()));
    ioc->setService<ConfAppManager>(new ConfAppManagerRpc());
    ioc->setService<ConfZoneManager>(new ConfZoneManagerRpc());
    ioc->setService<QuotaManager>(new QuotaManagerRpc());
    ioc->setService<StatManager>(new StatManagerRpc(settings->statFilePath()));
    ioc->setService<StatBlockManager>(new StatBlockManagerRpc(settings->statBlockFilePath()));
    ioc->setService<AskPendingManager>(new AskPendingManagerRpc());
    ioc->setService<DriverManager>(new DriverManagerRpc());
    ioc->setService<AppInfoManager>(new AppInfoManagerRpc(settings->cacheFilePath()));
    ioc->setService<LogManager>(new LogManagerRpc());
    ioc->setService<ServiceInfoManager>(new ServiceInfoManagerRpc());
    ioc->setService<TaskManager>(new TaskManagerRpc());
}

inline void setupServices(IocContainer *ioc, const FortSettings *settings)
{
    if (settings->isMaster()) {
        setupMasterServices(ioc, settings);
    } else {
        setupClientServices(ioc, settings);
    }

    if (settings->hasService()) {
        ioc->setService(new RpcManager());
    }

    if (settings->isService()) {
        ioc->setService<WindowManager>(new WindowManagerFake());

        // For Service only
        ioc->setService(new ServiceManager());
    } else {
        ioc->setService(new WindowManager());

        // For UI only
        ioc->setService(new HotKeyManager());
        ioc->setService(new UserSettings());
        ioc->setService(new TranslationManager());
    }

    ioc->setService(new DriveListManager());
    ioc->setService(new NativeEventFilter());
    ioc->setService(new AppInfoCache());
    ioc->setService(new HostInfoCache());
    ioc->setService(new ZoneListModel());
}

}

FortManager::FortManager(QObject *parent) : QObject(parent) { }

FortManager::~FortManager()
{
    if (m_initialized) {
        closeDriver();
    }

    deleteManagers();

    OsUtil::closeMutex(m_instanceMutex);
}

bool FortManager::checkRunningInstance(bool isService)
{
    bool isSingleInstance;
    m_instanceMutex =
            OsUtil::createMutex(isService ? "Global\\" APP_BASE : APP_BASE, isSingleInstance);
    if (isSingleInstance)
        return true;

    if (isService) {
        qCWarning(LC) << "Quit due Service is already running!";
    } else {
        if (!IoC<ControlManager>()->postCommand(Control::Prog, { "show" })) {
            QMessageBox::warning(nullptr, QString(), tr("Application is already running!"));
        }
    }
    return false;
}

void FortManager::initialize()
{
    m_initialized = true;

    OsUtil::setCurrentThreadName("Main");

    setupThreadPool();
    setupLogger();
    setupDbLogger();

    createManagers();

    setupEnvManager();
    setupConfManager();
    setupQuotaManager();
    setupTaskManager();
    setupServiceInfoManager();
    setupDriveListManager();

    setupDriver();
    loadConf();

    checkInstallDriver();
}

void FortManager::setupThreadPool()
{
    QThreadPool::globalInstance()->setMaxThreadCount(16);
}

void FortManager::setupLogger()
{
    Logger *logger = Logger::instance();

    const auto settings = IoC<FortSettings>();

    logger->setIsService(settings->isService());
    logger->setHasService(settings->hasService());
    logger->setPath(settings->logsPath());
}

void FortManager::updateLogger(const FirewallConf *conf)
{
    if (!conf->iniEdited())
        return;

    Logger *logger = Logger::instance();

    logger->setDebug(conf->ini().logDebug());
    logger->setConsole(conf->ini().logConsole());
}

void FortManager::setupDbLogger()
{
    SqliteDb::setErrorLogCallback(dbErrorHandler, /*context=*/this);
}

void FortManager::createManagers()
{
    IocContainer *ioc = IoC();

    const auto settings = IoC<FortSettings>();

    setupServices(ioc, settings);

    if (settings->isMaster()) {
        // TODO: COMPAT: Remove after v4.1.0 (via v4.0.0)
        FileUtil::copyFile(settings->statFilePath(), settings->statBlockFilePath());
    }

    ioc->setUpAll();
}

void FortManager::deleteManagers()
{
    IocContainer *ioc = IoC();

    ioc->tearDownAll();
    ioc->autoDeleteAll();
}

void FortManager::install(const char *arg)
{
    switch (arg[0]) {
    case 'b': { // "boot_filter"
        DriverCommon::provRegister(/*bootFilter=*/true); // Register booted provider
    } break;
    case 'p': { // "portable"
        FortManager::setupPortableResource();
        StartupUtil::setPortable(true);
    } break;
    case 's': { // "service"
        StartupUtil::setAutoRunMode(StartupUtil::StartupAllUsers);
        StartupUtil::setServiceInstalled(true);
    } break;
    case 'e': { // "explorer"
        StartupUtil::setExplorerIntegrated(true);
    } break;
    }
}

void FortManager::uninstall()
{
    StartupUtil::setAutoRunMode(StartupUtil::StartupDisabled); // Remove auto-run
    StartupUtil::setServiceInstalled(false); // Uninstall service
    StartupUtil::setExplorerIntegrated(false); // Remove Windows Explorer integration
    StartupUtil::clearGlobalExplorerIntegrated(); // COMPAT: Remove Global Windows Explorer
                                                  // integration
    DriverCommon::provUnregister(); // Unregister booted provider
}

bool FortManager::installDriver()
{
    closeDriver();

    IoC<DriverManager>()->reinstallDriver();

    if (IoC<FortSettings>()->hasService()) {
        // Re-install the service and app restart required to continue
        StartupUtil::setServiceInstalled(true);
        processRestartRequired();
    } else {
        // Re-open the driver device and initialize it
        if (setupDriver()) {
            updateDriverConf();
        }
    }

    return true;
}

bool FortManager::removeDriver()
{
    closeDriver();

    IoC<DriverManager>()->uninstallDriver();

    return true;
}

bool FortManager::setupDriver()
{
    auto driverManager = IoC<DriverManager>();
    auto confManager = IoC<ConfManager>();

    bool ok = driverManager->openDevice();

    if (ok && !confManager->validateDriver()) {
        driverManager->closeDevice();
        ok = false;
    }

    if (ok) {
        confManager->updateServices();
    }

    return ok;
}

void FortManager::closeDriver()
{
    updateLogManager(false);
    updateStatManager(nullptr);

    IoC<DriverManager>()->closeDevice();

    QCoreApplication::sendPostedEvents(this);
}

void FortManager::checkInstallDriver()
{
    auto driverManager = IoC<DriverManager>();

    if (driverManager->isDeviceOpened())
        return;

    const auto settings = IoC<FortSettings>();

    const bool canInstallDriver = (settings->canInstallDriver() || settings->isPortable())
            && settings->isMaster() && settings->isUserAdmin();

    if (canInstallDriver) {
        installDriver();
    }
}

void FortManager::setupEnvManager()
{
    auto envManager = IoC<EnvManager>();

    connect(IoC<NativeEventFilter>(), &NativeEventFilter::environmentChanged, envManager,
            &EnvManager::onEnvironmentChanged);

    connect(envManager, &EnvManager::environmentUpdated, this, [&] { updateDriverConf(); });
}

void FortManager::setupConfManager()
{
    connect(IoC<ConfManager>(), &ConfManager::confChanged, this, [&](bool onlyFlags) {
        const FirewallConf *conf = IoC<ConfManager>()->conf();

        updateLogger(conf);

        if (!onlyFlags || conf->flagsEdited()) {
            updateDriverConf(onlyFlags);
        }
    });
}

void FortManager::setupQuotaManager()
{
    connect(IoC<QuotaManager>(), &QuotaManager::alert, this, [&](qint8 alertType) {
        IoC<WindowManager>()->showInfoBox(
                QuotaManager::alertTypeText(alertType), tr("Quota Alert"));
    });
}

void FortManager::setupTaskManager()
{
    auto taskManager = IoC<TaskManager>();

    connect(taskManager, &TaskManager::appVersionDownloaded, this, [&](const QString &version) {
        IoC<WindowManager>()->showTrayMessage(tr("New version v%1 available!").arg(version),
                WindowManager::TrayMessageNewVersion);
    });

    connect(taskManager, &TaskManager::zonesDownloaded, this, [&](const QStringList &zoneNames) {
        IoC<WindowManager>()->showTrayMessage(
                tr("Zone Addresses Updated: %1.").arg(zoneNames.join(", ")),
                WindowManager::TrayMessageZones);
    });

    connect(taskManager, &TaskManager::zonesUpdated, IoC<ConfZoneManager>(),
            &ConfZoneManager::updateDriverZones);

    connect(taskManager, &TaskManager::taskDoubleClicked, this, [&](qint8 taskType) {
        switch (taskType) {
        case TaskInfo::UpdateChecker: {
            IoC<WindowManager>()->showHomeWindowAbout();
        } break;
        case TaskInfo::ZoneDownloader: {
            IoC<WindowManager>()->showZonesWindow();
        } break;
        }
    });
}

void FortManager::setupServiceInfoManager()
{
    auto serviceInfoManager = IoC<ServiceInfoManager>();

    connect(serviceInfoManager, &ServiceInfoManager::servicesStarted, IoC<ConfManager>(),
            &ConfManager::updateDriverServices);
}

void FortManager::setupDriveListManager()
{
    auto settings = IoC<FortSettings>();
    auto driveListManager = IoC<DriveListManager>();

    if (settings->isService()) {
        connect(IoC<ServiceManager>(), &ServiceManager::driveListChanged, driveListManager,
                &DriveListManager::onDriveListChanged);
    } else {
        connect(IoC<NativeEventFilter>(), &NativeEventFilter::driveListChanged, driveListManager,
                &DriveListManager::onDriveListChanged);
    }

    driveListManager->initialize();
}

void FortManager::show()
{
    auto windowManager = IoC<WindowManager>();
    const IniUser &iniUser = IoC<UserSettings>()->iniUser();

    windowManager->setupTrayIcon();

    if (iniUser.trayShowIcon()) {
        windowManager->showTrayIcon();
    } else {
        windowManager->showHomeWindow();
    }

    if (iniUser.graphWindowVisible()) {
        windowManager->showGraphWindow();
    }
}

void FortManager::processRestartRequired()
{
    auto windowManager = IoC<WindowManager>();

    windowManager->showConfirmBox(
            [=] { windowManager->restart(); }, tr("Restart Now?"), tr("Restart Required"));
}

void FortManager::loadConf()
{
    const auto settings = IoC<FortSettings>();
    const auto confManager = IoC<ConfManager>();

    confManager->validateMigration();
    confManager->load();

    qCDebug(LC) << "Started as"
                << (settings->isService()                   ? "Service"
                                   : settings->hasService() ? "Client"
                                                            : "Program");
}

bool FortManager::updateDriverConf(bool onlyFlags)
{
    auto confManager = IoC<ConfManager>();
    auto confAppManager = IoC<ConfAppManager>();

    updateLogManager(false);

    const bool res = confAppManager->updateDriverConf(onlyFlags);
    if (res) {
        updateStatManager(confManager->conf());
    }

    updateLogManager(true);

    return res;
}

void FortManager::updateLogManager(bool active)
{
    IoC<LogManager>()->setActive(active);
}

void FortManager::updateStatManager(FirewallConf *conf)
{
    IoC<StatManager>()->setConf(conf);
}

void FortManager::onDbIoError()
{
    // Force drive list checks
    IoC<DriveListManager>()->startPolling();
}

void FortManager::setupPortableResource()
{
    Q_INIT_RESOURCE(fort_readme);
}

void FortManager::setupResources()
{
    Q_INIT_RESOURCE(appinfo_migrations);
    Q_INIT_RESOURCE(conf_migrations);
    Q_INIT_RESOURCE(conf_zone);
    Q_INIT_RESOURCE(stat_migrations);

    Q_INIT_RESOURCE(fort_icons);
}

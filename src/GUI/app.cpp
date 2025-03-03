#include <QtGlobal>
#include <QTranslator>
#include <QLocale>
#include <QFileOpenEvent>
#include <QNetworkProxyFactory>
#include <QNetworkAccessManager>
#include <QLibraryInfo>
#include <QSurfaceFormat>
#include "common/programpaths.h"
#include "common/config.h"
#include "common/downloader.h"
#include "map/ellipsoid.h"
#include "map/gcs.h"
#include "map/pcs.h"
#include "data/dem.h"
#include "data/waypoint.h"
#include "gui.h"
#include "mapaction.h"
#include "app.h"


App::App(int &argc, char **argv) : QApplication(argc, argv)
{
#if defined(Q_OS_WIN32) || defined(Q_OS_MAC)
	setApplicationName(APP_NAME);
#else
	setApplicationName(QString(APP_NAME).toLower());
#endif
	setApplicationVersion(APP_VERSION);

	QTranslator *gpxsee = new QTranslator(this);
	if (gpxsee->load(QLocale::system(), "gpxsee", "_",
	  ProgramPaths::translationsDir()))
		installTranslator(gpxsee);

	QTranslator *qt = new QTranslator(this);
#if defined(Q_OS_WIN32) || defined(Q_OS_MAC)
	if (qt->load(QLocale::system(), "qt", "_", ProgramPaths::translationsDir()))
#else // Q_OS_WIN32 || Q_OS_MAC
	if (qt->load(QLocale::system(), "qt", "_", QLibraryInfo::location(
	  QLibraryInfo::TranslationsPath)))
#endif // Q_OS_WIN32 || Q_OS_MAC
		installTranslator(qt);

#ifdef Q_OS_MAC
	setAttribute(Qt::AA_DontShowIconsInMenus);
#endif // Q_OS_MAC
	QNetworkProxyFactory::setUseSystemConfiguration(true);
	/* The QNetworkAccessManager must be a child of QApplication, otherwise it
	   triggers the following warning on exit (and may probably crash):
	   "QThreadStorage: Thread X exited after QThreadStorage Y destroyed" */
	Downloader::setNetworkManager(new QNetworkAccessManager(this));
	DEM::setDir(ProgramPaths::demDir());
	QSurfaceFormat fmt;
	fmt.setStencilBufferSize(8);
	fmt.setSamples(4);
	QSurfaceFormat::setDefaultFormat(fmt);

	loadDatums();
	loadPCSs();
	Waypoint::loadSymbolIcons(ProgramPaths::symbolsDir());

	_gui = new GUI();

#ifdef Q_OS_ANDROID
	connect(this, &App::applicationStateChanged, this, &App::appStateChanged);
#endif // Q_OS_ANDROID
}

App::~App()
{
	delete _gui;
}

int App::run()
{
	MapAction *lastReady = 0;
	QStringList args(arguments());

	_gui->show();

	for (int i = 1; i < args.count(); i++) {
		if (!_gui->openFile(args.at(i), true)) {
			MapAction *a;
			if (!_gui->loadMap(args.at(i), a, true))
				_gui->openFile(args.at(i), false);
			else {
				if (a)
					lastReady = a;
			}
		}
	}

	if (lastReady)
		lastReady->trigger();

	return exec();
}

#ifdef Q_OS_ANDROID
void App::appStateChanged(Qt::ApplicationState state)
{
	if (state == Qt::ApplicationSuspended)
		_gui->writeSettings();
}
#endif // Q_OS_ANDROID

bool App::event(QEvent *event)
{
	if (event->type() == QEvent::FileOpen) {
		QFileOpenEvent *e = static_cast<QFileOpenEvent *>(event);

		if (!_gui->openFile(e->file(), true)) {
			MapAction *a;
			if (!_gui->loadMap(e->file(), a, true))
				return _gui->openFile(e->file(), false);
			else {
				if (a)
					a->trigger();
				return true;
			}
		} else
			return true;
	}

	return QApplication::event(event);
}

void App::loadDatums()
{
	QString ellipsoidsFile(ProgramPaths::ellipsoidsFile());
	QString gcsFile(ProgramPaths::gcsFile());

	if (ellipsoidsFile.isNull())
		qWarning("No ellipsoids file found.");
	if (gcsFile.isNull())
		qWarning("No GCS file found.");

	if (!ellipsoidsFile.isNull() && !gcsFile.isNull()) {
		Ellipsoid::loadList(ellipsoidsFile);
		GCS::loadList(gcsFile);
	} else
		qWarning("Maps based on a datum different from WGS84 won't work.");
}

void App::loadPCSs()
{
	QString pcsFile(ProgramPaths::pcsFile());

	if (pcsFile.isNull())
		qWarning("No PCS file found.");
	else
		PCS::loadList(pcsFile);
}

#include <iostream>
#include <cstdio> //perror

#include <QCoreApplication>
#include <QtDebug>
#include <QKeySequence>

#ifndef NDEBUG
#include <QFile>
#include <QTextStream>
#endif

// generated by cmake from projectConfig.h.in
#include "projectConfig.h"

// generated by cmake qt4_add_dbus_interface
#include "app_menu.h"
#include "dbus_menu.h"
typedef com::canonical::AppMenu::Registrar AppMenu;
typedef com::canonical::dbusmenu DBusMenu;

#include "gtk_action.h"
#include "gtk_menu.h"
typedef org::gtk::Actions GtkAction;
typedef org::gtk::Menus GtkMenu;

#define REGISTRAR_SERVICE "com.canonical.AppMenu.Registrar"
#define REGISTRAR_PATH "/com/canonical/AppMenu/Registrar"


#include <X11/Xlib.h>

#include "dbusmenushortcut_p.h"


#if 0
#ifndef NDEBUG
void file_logger(QtMsgType type, const char *msg){
	QString txt;
	switch (type) {
		case QtDebugMsg:
			txt = QString("Debug: %1").arg(msg);
			break;
		case QtWarningMsg:
			txt = QString("Warning: %1").arg(msg);
			break;
		case QtCriticalMsg:
			txt = QString("Critical: %1").arg(msg);
			break;
		case QtFatalMsg:
			txt = QString("Fatal: %1").arg(msg);
			abort();
	}

	QFile outFile("/tmp/qmenu_hud.log"); //FIXME: don't use hardcoded path
	outFile.open(QIODevice::WriteOnly | QIODevice::Append);
	QTextStream ts(&outFile);
	ts << txt << endl;
}
#else
void devnull_logger(QtMsgType type, const char *msg){
}
#endif
#endif


void inspect(const DBusMenuLayoutItem &topItem, QString &path, QMap<QString,int> &menuMap){
	QString label;
	for(const DBusMenuLayoutItem &item : topItem.children){
		label = item.properties.value("label").toString().remove("_");
		if(!item.children.isEmpty()){
			QString subPath(path);
			subPath.append(label);
			subPath.append(" > ");
			inspect(item, subPath, menuMap);
		}else{
			// leaf
			if(!label.isEmpty()){
				QString str = QString("%1").arg(path + label, -150); //FIXME: width hardcoded
				if(item.properties.contains(QString("shortcut"))){
					DBusMenuShortcut s = qdbus_cast<DBusMenuShortcut>(item.properties.value("shortcut").value<QDBusArgument>());
					str += "   " + s.toKeySequence().toString();
				}
				//qDebug() << path << label << " ::: " << item.id;
				menuMap.insert(str, item.id);
			}
		}
	}
}

void gtk(unsigned char *dbusname, unsigned char *menubarpath){
	// gtk changed its model to something else, doing everything again is always fun...
	// even worse: the dbus interface is an implementation detail and can change any minute, fun as hell
	GtkMenuTypes_register();

	SillyList list;
	for(int i=0; i<1024; ++i){
		list.append(i);
	}
	GtkMenu *gtkMenu = new GtkMenu((const char*)dbusname, (const char*)menubarpath, QDBusConnection::sessionBus());
	GtkAction *gtkAction = new GtkAction((const char*)dbusname, (const char*)menubarpath, QDBusConnection::sessionBus());

	QDBusPendingReply<GtkMenuEntryList> start_reply = gtkMenu->Start(list);
	start_reply.waitForFinished();
	if(start_reply.isError()){
		puts("Start failed");
		exit(-1);
	}

	// start needs to run before
	QDBusPendingReply<GtkMenuDescMap> reply_descall = gtkAction->DescribeAll();
	reply_descall.waitForFinished();
	if(reply_descall.isError()){
		puts("DescribeAll failed");
		exit(-1);
	}
	GtkMenuDescMap descs = reply_descall.argumentAt<0>();

	QMap<uint64_t,QPair<QString,bool>> submenus; // submenus with their prefix and if enabled
	QMap<QString,QString> entries;
	GtkMenuEntryList menus = start_reply.argumentAt<0>();
	for(const GtkMenuEntry &e : menus){
		for(const auto &m : e.entry){
			QString prefix;
			bool enabled = true;
			auto itsub = submenus.find((((uint64_t)e.first)<<32) | e.second);
			if(itsub != submenus.end()){
				prefix = itsub->first;
				enabled = itsub->second;
			}
			auto itlabel = m.find("label");
			if(itlabel != m.end()){
				if(!prefix.isEmpty()){
					prefix += " > ";
				}
				prefix += itlabel->toString();
			}
			auto itsubmenu = m.find(":submenu");
			if(itsubmenu == m.end()){// there's also ":section" as silly inner node
				itsubmenu = m.find(":section");
			}
			if(itsubmenu != m.end()){ // has submenu
				auto itaction = m.find("action");
				if(itaction != m.end()){
					QString action = itaction->toString().remove("unity.");
					auto itdesc =  descs.find(action);
					if(itdesc != descs.end()){
						if(!itdesc->enabled){
							enabled = false;
						}
					}
				}
				QDBusArgument dbusarg = itsubmenu->value<QDBusArgument>();
				SimplePair sp;
				dbusarg >> sp;
				submenus.insert((((uint64_t)sp.first)<<32) | sp.second, qMakePair(prefix,enabled));
			}else{
				// leaf
				auto itaction = m.find("action");
				if(itaction != m.end()){
					QString action = itaction->toString().remove("unity.");
					auto itdesc =  descs.find(action);
					if(itdesc != descs.end()){
						if(itdesc->enabled && enabled){
							entries.insert(prefix.remove("_"), action);
						}
					}
				}
			}
		}
	}

	// call dmenu -i -l 20
	QStringList args;
	args << "-i" << "-l" << "20";
	QProcess *proc = new QProcess();
	proc->start(DMENU_PATH, args);
	proc->waitForStarted();

	// write all entries to dmenu's stdin
	for(QString &key : entries.keys()){
		proc->write(key.toLocal8Bit().data());
		proc->write("\n");
	}
	proc->closeWriteChannel();

	proc->waitForFinished(-1);
	// read selected entry from dmenu's stdout
	QString selected = QString::fromLocal8Bit(proc->readAllStandardOutput()).remove("\n");
	proc->close();
	delete proc;

	if(!selected.isEmpty()){ // empty when canceled with esc
		QString action = entries[selected];
		QDBusPendingReply<void> reply = gtkAction->Activate(action, {}, {});
		reply.waitForFinished();
		if (reply.isError()) {
			qDebug() << "Activate failed with action: " << action;
		}
	}

	delete gtkMenu;
	delete gtkAction;
}

void appmenu(unsigned long int winID){
	DBusMenuTypes_register();

	// get dbus service of the application which exports menu
	AppMenu *appMenu = new AppMenu(REGISTRAR_SERVICE, REGISTRAR_PATH, QDBusConnection::sessionBus());
	QDBusPendingReply<QString, QDBusObjectPath> reply = appMenu->GetMenuForWindow(winID);
	reply.waitForFinished();
	if (reply.isError()) {
		delete appMenu;
		if(reply.error().type() == QDBusError::Failed){
			// window not registered/unknown to registrar
			// let the user know by showing it in dmenu
			QProcess *proc = new QProcess();
			proc->start(DMENU_PATH, QStringList());
			proc->waitForStarted();
			proc->write("no menu entries exported\n");
			proc->closeWriteChannel();
			proc->waitForFinished();
			delete proc;
			exit(0);
		}else{
			// something else
			qDebug() << reply.error().name();
			qDebug() << reply.error().message();
			exit(-1);
		}
	}

	QString dbus_service = reply.argumentAt<0>();
	QString dbus_path = reply.argumentAt<1>().path();

	// get menu
	DBusMenu *dbusMenu = new DBusMenu(dbus_service, dbus_path, QDBusConnection::sessionBus());
	QDBusPendingReply <uint, DBusMenuLayoutItem> topItems = dbusMenu->GetLayout(0, -1, QStringList());
	topItems.waitForFinished();
	if (topItems.isError()) {
		qDebug() << topItems.error().name();
		qDebug() << topItems.error().message();
		exit(-1);
	}

	// traverse through all submenus
	QString path;
	QMap<QString, int> menuMap;
	for(const DBusMenuLayoutItem &topLevel : topItems.argumentAt<1>().children){
		path.append(topLevel.properties.value("label").toString().remove("_"));
		path.append(" > ");

		inspect(topLevel, path, menuMap);

		path.clear();
	}

	// call dmenu -i -l 20
	QStringList args;
	args << "-i" << "-l" << "20";
	QProcess *proc = new QProcess();
	proc->start(DMENU_PATH, args);
	proc->waitForStarted();

	// write all entries to dmenu's stdin
	for(QString &key : menuMap.keys()){
		proc->write(key.toLocal8Bit().data());
		proc->write("\n");
	}
	proc->closeWriteChannel();

	proc->waitForFinished(-1);
	// read selected entry from dmenu's stdout
	QString selected = QString::fromLocal8Bit(proc->readAllStandardOutput()).remove("\n");
	proc->close();
	delete proc;

	if(!selected.isEmpty()){ // empty when canceled with esc
		int id = menuMap[selected];

		// send menu click event
		QDBusVariant empty;
		empty.setVariant(QVariant::fromValue<QString>(QString()));
		QDBusPendingReply<void> clicked_r = dbusMenu->Event(id, "clicked", empty, QDateTime::currentDateTime().toTime_t());
		if(clicked_r.isError()){
			qDebug() << clicked_r.error().message();
			qDebug() << clicked_r.error().name();
		}
	}

	delete dbusMenu;
	delete appMenu;
}

int main(int argc, char **argv){
	QCoreApplication app(argc, argv);
#if 0
#ifndef NDEBUG
	qInstallMsgHandler(file_logger);
#else
	//HACK: get rid of QDBusSignature: invalid signature ""
	//      probably a bad idea
	qInstallMsgHandler(devnull_logger);
#endif
#endif

	unsigned long int winID;
	Display *display = XOpenDisplay(NULL);
	if(argc == 2){
		QString arg(argv[1]);
		if(arg == "-h" || arg == "--help"){
			std::cout << "usage: qmenu_hud [window id]" << std::endl
				<< std::endl
				<< "If no window id is specified, the window which has currently the focus is used." << std::endl
				<< "qmenu_registrar needs to be running." << std::endl;
			return 0;
		}

		// got window id as argument
		bool ok;
		winID = arg.toULong(&ok, 0);
		if(!ok){
			std::cerr << "couldn't convert to window id: " << argv[1] << std::endl;
			return -1;
		}
	}else{
		// get window (id) which has currently the focus
		if(!display){
			perror("XOpenDisplay failed");
			return -1;
		}
		Window window;
		int ret;
		// not working for gtk, surprise...
		// gtk sets focus on a child window, not the toplevel, for whatever reason
// 		XGetInputFocus(display, &window, &ret);
// 		winID = window;
		// read active window from root window, assuming wm supports this
		// oh so much fun... thx gtk
		window = XDefaultRootWindow(display);
		Atom property = XInternAtom(display, "_NET_ACTIVE_WINDOW", True);
		if(property==None) return -1;
		Atom actual_type_return;
		int actual_format_return;
		unsigned long nitems_return, bytes_after_return;
		unsigned char *activewin;
		ret = XGetWindowProperty(
			display, window, property, 0, 1024, False, AnyPropertyType,
			&actual_type_return, &actual_format_return, &nitems_return, &bytes_after_return, &activewin
		);
		winID = *(unsigned long *)activewin;
// 		qDebug() << "winId: " << winID;
	}

	//FIXME: error check
	unsigned char *dbusname=nullptr, *menubarpath=nullptr;
	Atom property = XInternAtom(display, "_GTK_UNIQUE_BUS_NAME", True);
	if(property != None){
		Atom actual_type_return;
		int actual_format_return;
		unsigned long nitems_return, bytes_after_return;
		XGetWindowProperty(
			display, winID, property, 0, 1024, False, AnyPropertyType,
			&actual_type_return, &actual_format_return, &nitems_return, &bytes_after_return, &dbusname
		);

		property = XInternAtom(display, "_GTK_MENUBAR_OBJECT_PATH", True);
		XGetWindowProperty(
			display, winID, property, 0, 1024, False, AnyPropertyType,
			&actual_type_return, &actual_format_return, &nitems_return, &bytes_after_return, &menubarpath
		);
	}

	if(dbusname && menubarpath){
// 		qDebug() << "dbusname: " << dbusname << "; path: " << menubarpath;
		gtk(dbusname, menubarpath);

		XFree(dbusname);
		XFree(menubarpath);
	}else{
		appmenu(winID);
	}

	XCloseDisplay(display);

	return 0;
}

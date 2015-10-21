NULL =

libexec_PROGRAMS = \
	xdg-app-gtk-portal \
	$(NULL)

portaldir = $(pkgdatadir)/portals
portal_DATA = gtk.portal

dbus_built_sources = xdg-app-portal-dbus.c xdg-app-portal-dbus.h
BUILT_SOURCES = $(dbus_built_sources)

$(dbus_built_sources): Makefile.am
	$(AM_V_GEN) $(GDBUS_CODEGEN)			\
	--interface-prefix org.freedesktop.portal.	\
	--c-namespace XdgAppDesktop			\
	--generate-c-code xdg-app-portal-dbus		\
	 $(XDG_APP_INTERFACES_DIR)/org.freedesktop.portal.FileChooserBackend.xml \
	$(NULL)

%.service: %.service.in config.log
	$(AM_V_GEN) $(SED) -e "s|\@libexecdir\@|$(libexecdir)|" $< > $@

servicedir = $(DBUS_SERVICE_DIR)
service_in_files = org.freedesktop.portal_backend.desktop.gtk.service.in
service_DATA = org.freedesktop.portal_backend.desktop.gtk.service

xdg_app_gtk_portal_SOURCES = \
	xdg-app-gtk.c		\
	$(dbus_built_sources)	\
	$(NULL)

xdg_app_gtk_portal_LDADD = $(BASE_LIBS) $(GTK_LIBS)
xdg_app_gtk_portal_CFLAGS = $(BASE_CFLAGS) $(GTK_CFLAGS)

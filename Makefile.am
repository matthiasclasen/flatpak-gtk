NULL =

libexec_PROGRAMS = \
	flatpak-gtk-portal \
	$(NULL)

portaldir = $(datadir)/flatpak/portals
portal_DATA = gtk.portal

dbus_built_sources = flatpak-portal-dbus.c flatpak-portal-dbus.h
BUILT_SOURCES = $(dbus_built_sources)

$(dbus_built_sources): Makefile.am
	$(AM_V_GEN) $(GDBUS_CODEGEN)			\
	--interface-prefix org.freedesktop.impl.portal.	\
	--c-namespace FlatpakDesktop			\
	--generate-c-code flatpak-portal-dbus		\
	 $(FLATPAK_INTERFACES_DIR)/org.freedesktop.impl.portal.FileChooser.xml \
	$(NULL)

%.service: %.service.in config.log
	$(AM_V_GEN) $(SED) -e "s|\@libexecdir\@|$(libexecdir)|" $< > $@

servicedir = $(DBUS_SERVICE_DIR)
service_in_files = org.freedesktop.impl.portal.desktop.gtk.service.in
service_DATA = org.freedesktop.impl.portal.desktop.gtk.service

xdp_dbus_built_sources = xdp-dbus.c xdp-dbus.h
BUILT_SOURCES += $(xdp_dbus_built_sources)

$(xdp_dbus_built_sources) : $(DBUS_SERVICE_DIR)/../interfaces/org.freedesktop.portal.Documents.xml
	$(AM_V_GEN) $(GDBUS_CODEGEN)                            \
		--interface-prefix org.freedesktop.portal.      \
		--c-namespace XdpDbus                           \
		--generate-c-code $(builddir)/xdp-dbus  	\
		--annotate "org.freedesktop.portal.Documents.Add()" "org.gtk.GDBus.C.UnixFD" "true" \
		--annotate "org.freedesktop.portal.Documents.AddNamed()" "org.gtk.GDBus.C.UnixFD" "true" \
		$(DBUS_SERVICE_DIR)/../interfaces/org.freedesktop.portal.Documents.xml     \
		$(NULL)



flatpak_gtk_portal_SOURCES = \
	flatpak-gtk.c		\
	filechooser.h		\
	filechooser.c		\
	$(dbus_built_sources)	\
	$(xdp_dbus_built_sources)	\
	$(NULL)

flatpak_gtk_portal_LDADD = $(BASE_LIBS) $(GTK_LIBS)
flatpak_gtk_portal_CFLAGS = $(BASE_CFLAGS) $(GTK_CFLAGS)

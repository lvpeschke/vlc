# Qt

QT_VERSION = 5.6.0
QT_URL := https://download.qt.io/official_releases/qt/5.6/$(QT_VERSION)/submodules/qtbase-opensource-src-$(QT_VERSION).tar.xz

ifdef HAVE_MACOSX
#PKGS += qt
endif
ifdef HAVE_WIN32
PKGS += qt
endif

ifeq ($(call need_pkg,"Qt5Core Qt5Gui Qt5Widgets"),)
PKGS_FOUND += qt
endif

$(TARBALLS)/qt-$(QT_VERSION).tar.xz:
	$(call download,$(QT_URL))

.sum-qt: qt-$(QT_VERSION).tar.xz

qt: qt-$(QT_VERSION).tar.xz .sum-qt
	$(UNPACK)
	mv qtbase-opensource-src-$(QT_VERSION) qt-$(QT_VERSION)
	$(MOVE)

ifdef HAVE_MACOSX
QT_PLATFORM := -platform darwin-g++
endif
ifdef HAVE_WIN32
QT_SPEC := win32-g++
QT_PLATFORM := -xplatform win32-g++ -device-option CROSS_COMPILE=$(HOST)-
endif

QT_CONFIG := -static -release -opensource -confirm-license -no-pkg-config \
	-no-sql-sqlite -no-gif -qt-libjpeg -no-openssl -no-opengl -no-dbus \
	-no-qml-debug -no-audio-backend -no-sql-odbc \
	-no-compile-examples -nomake examples

.qt: qt
	cd $< && ./configure $(QT_PLATFORM) $(QT_CONFIG) -prefix $(PREFIX)
	# Make && Install libraries
	cd $< && $(MAKE)
	cd $</src && $(MAKE) sub-corelib-install_subtargets sub-gui-install_subtargets sub-widgets-install_subtargets
	# Install tools
	cd $</src && $(MAKE) sub-moc-install_subtargets sub-rcc-install_subtargets sub-uic-install_subtargets
	# Install plugins
	cd $</src/plugins && $(MAKE) sub-platforms-install_subtargets
	mv $(PREFIX)/plugins/platforms/libqwindows.a $(PREFIX)/lib/ && rm -rf $(PREFIX)/plugins
	# Move includes to match what VLC expects
	rm -rf $(PREFIX)/include/qt5/ && mkdir $(PREFIX)/include/qt5/ && mv -v $(PREFIX)/include/Q* $(PREFIX)/include/qt5/
	mkdir $(PREFIX)/include/qt5/QtGui/qpa && cp $(PREFIX)/include/qt5/QtGui/$(QT_VERSION)/QtGui/qpa/qplatformnativeinterface.h $(PREFIX)/include/qt5/QtGui/qpa
	# Clean Qt mess
	rm -rf $(PREFIX)/lib/libQt5Bootstrap* $(PREFIX)/lib/*.prl $(PREFIX)/mkspecs
ifdef HAVE_CROSS_COMPILE
	# Building Qt build tools for Xcompilation
	cd $</include/QtCore; ln -sf $(QT_VERSION)/QtCore/private
	cd $</qmake; $(MAKE)
	cd $</src/tools; \
	for i in bootstrap uic rcc moc; \
		do (cd $$i; echo $$i && ../../../bin/qmake -spec $(QT_SPEC) && $(MAKE) clean && $(MAKE) CC=$(HOST)-gcc CXX=$(HOST)-g++ LINKER=$(HOST)-g++ && $(MAKE) install); \
	done
endif
	touch $@

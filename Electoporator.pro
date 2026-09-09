QT += core gui widgets bluetooth

CONFIG += c++17

# You can make your code fail to compile if it uses deprecated APIs.
# In order to do so, uncomment the following line.
#DEFINES += QT_DISABLE_DEPRECATED_BEFORE=0x060000    # disables all the APIs deprecated before Qt 6.0.0

SOURCES += \
    blelink.cpp \
    blescanner.cpp \
    main.cpp \
    mainwindow.cpp \
    scanpage.cpp

HEADERS += \
    blelink.h \
    blescanner.h \
    mainwindow.h \
    scanpage.h

FORMS += \
    mainwindow.ui \
    scanpage.ui

# Default rules for deployment.
qnx: target.path = /tmp/$${TARGET}/bin
else: unix:!android: target.path = /opt/$${TARGET}/bin
!isEmpty(target.path): INSTALLS += target

DISTFILES +=

RESOURCES += \
    resources.qrc

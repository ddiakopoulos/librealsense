include(include.pri)

TEMPLATE = lib
CONFIG += staticlib
CONFIG -= qt

INCLUDEPATH += ../include

CONFIG += link_pkgconfig
PKGCONFIG += libusb-1.0
LIBS += -pthread
QMAKE_CXXFLAGS += -std=c++11 -fPIC -pedantic -mssse3
QMAKE_CXXFLAGS += -Wno-missing-field-initializers -Wno-switch -Wno-multichar
QMAKE_CXXFLAGS += -DRS_USE_V4L2_BACKEND

HEADERS += ../include/librealsense/* ../src/*.h
SOURCES += ../src/*.cpp ../src/verify.c


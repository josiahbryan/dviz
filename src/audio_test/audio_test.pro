TEMPLATE = app
TARGET = 
DEPENDPATH += .
INCLUDEPATH += ../

#include(../audio/audio.pri)

QT += multimedia

# Input
HEADERS += recorder.h
SOURCES += audio_test.cpp \
           recorder.cpp

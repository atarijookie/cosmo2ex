/*
    Globals which are normally defined in main.cpp - the tests link all app objects except main.o,
    so they have to be defined here.
*/
#include <signal.h>

#include "misc/global.h"
#include "misc/debug.h"

volatile sig_atomic_t sigintReceived = 0;
volatile sig_atomic_t fatalErrorOccurred = 0;

InterProcessEvents  events;
ExternalServices    externalServices;
int logLevel = LOG_OFF;         // no logging from tests, so they don't write to app log files
bool noCapture = false;

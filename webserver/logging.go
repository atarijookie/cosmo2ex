package main

import (
	"fmt"
	"log"
	"os"
	"strconv"
	"strings"
	"sync/atomic"
)

// log levels - same values as in ce_cores (misc/debug.h), higher level shows also all the lower levels
const (
	logOff     = 0
	logError   = 1 // errors       - should be always visible, even to users
	logWarning = 2 // warnings
	logInfo    = 3 // info         - info which can be displayed when running at user's place
	logDebug   = 4 // debug info   - useful only to developers
)

// current log level of the webserver, can be changed at runtime from the settings page
var currentLogLevel atomic.Int32

func init() {
	currentLogLevel.Store(logError) // until LOG_LEVEL from .env is applied
}

// clampLogLevel keeps the level in logOff .. logDebug range
func clampLogLevel(level int) int {
	if level < logOff {
		return logOff
	}
	if level > logDebug {
		return logDebug
	}
	return level
}

func getLogLevel() int {
	return int(currentLogLevel.Load())
}

func setLogLevel(level int) {
	level = clampLogLevel(level)
	old := currentLogLevel.Swap(int32(level))
	if old != int32(level) {
		log.Printf("INFO  setLogLevel - switching log level from %d to %d", old, level)
	}
}

// logLevelFromDotEnv sets the log level from LOG_LEVEL (loaded from .env), keeps the current one if it's missing or invalid
func logLevelFromDotEnv() {
	val := strings.TrimSpace(os.Getenv("LOG_LEVEL"))
	if val == "" {
		return
	}
	level, err := strconv.Atoi(val)
	if err != nil {
		log.Printf("ERROR logLevelFromDotEnv - invalid LOG_LEVEL %q: %v", val, err)
		return
	}
	setLogLevel(level)
}

func logLevelString(level int) string {
	switch level {
	case logOff:
		return "OFF  "
	case logError:
		return "ERROR"
	case logWarning:
		return "WARN "
	case logInfo:
		return "INFO "
	case logDebug:
		return "DEBUG"
	}
	return "???  "
}

// logAt writes the message to log if level is allowed by the current log level, prefixed with level string
func logAt(level int, format string, args ...interface{}) {
	if level > getLogLevel() {
		return
	}
	log.Output(3, logLevelString(level)+" "+fmt.Sprintf(format, args...))
}

func logErrorf(format string, args ...interface{})   { logAt(logError, format, args...) }
func logWarningf(format string, args ...interface{}) { logAt(logWarning, format, args...) }
func logInfof(format string, args ...interface{})    { logAt(logInfo, format, args...) }
func logDebugf(format string, args ...interface{})   { logAt(logDebug, format, args...) }

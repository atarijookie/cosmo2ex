package main

import "github.com/atarijookie/cosmo2ex/webserver/dotenv"

// loadEnvFromDotFile loads the .env file into env vars, see dotenv.Load
func loadEnvFromDotFile(path string) {
	dotenv.Load(path)
}

// envDir returns the dir from env var key (loaded from .env), or its per-OS default if it's not set.
func envDir(key string) string {
	return dotenv.Dir(key)
}

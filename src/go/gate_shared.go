package main

// Gate decision logic shared by the cgo exports (config.go, //go:build android)
// and the host gate tests (F5).  Previously the host test exercised a parallel
// reimplementation in types_host.go that could silently drift from the shipping
// gate; now both call these same functions, so a change here (or its deletion)
// breaks both the device build and the test.
//
// These reference loadCfg()/Config, which are defined per build (config.go on
// android, types_host.go on host) with identical shape — this file is untagged
// and compiles against whichever is present.

// configFunctionEnabledImpl reports whether jni_name passes the include gate.
// A nil enabledSet means "no whitelist configured" → everything on.
func configFunctionEnabledImpl(name string) bool {
	c := loadCfg()
	if c.enabledSet == nil {
		return true
	}
	return c.enabledSet[name]
}

// configFunctionBlacklistedImpl reports whether jni_name is excluded.
func configFunctionBlacklistedImpl(name string) bool {
	c := loadCfg()
	return c.blacklistSet != nil && c.blacklistSet[name]
}

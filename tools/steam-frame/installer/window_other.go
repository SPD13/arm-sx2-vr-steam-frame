//go:build !darwin && !windows

package main

// Linux and others: no window, the installer opens in the default browser (a static binary that runs on any distribution)

func openWindow(url string) bool { return false }

func showError(message string) {}

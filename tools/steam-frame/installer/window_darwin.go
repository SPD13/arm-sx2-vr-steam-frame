package main

// The macOS window: WebKit through github.com/webview/webview_go (cgo, built on a Mac by make-installer.sh)

import (
	"os/exec"
	"runtime"

	webview "github.com/webview/webview_go"
)

// Cocoa wants its window on the main thread: keep main() on it
func init() { runtime.LockOSThread() }

func openWindow(url string) bool {
	w := webview.New(false)
	if w == nil {
		return false
	}
	defer w.Destroy()
	w.SetTitle(appTitle)
	w.SetSize(760, 820, webview.HintNone)
	w.Navigate(url)
	w.Run()
	return true
}

func showError(message string) {
	exec.Command("osascript", "-e", `display alert "ARMSX2 installer" message `+appleScriptString(message)+` as critical`).Run()
}

func appleScriptString(s string) string {
	out := []rune{'"'}
	for _, r := range s {
		if r == '"' || r == '\\' {
			out = append(out, '\\')
		}
		out = append(out, r)
	}
	return string(append(out, '"'))
}

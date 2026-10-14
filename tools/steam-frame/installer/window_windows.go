package main

// The Windows window: Microsoft Edge WebView2, which Windows 10 and 11 include, through github.com/jchv/go-webview2 (no cgo).
// Without WebView2 the installer opens in the default browser instead.

import (
	"os"
	"path/filepath"

	webview2 "github.com/jchv/go-webview2"
	"golang.org/x/sys/windows"
)

func openWindow(url string) bool {
	dataPath := ""
	if dir, err := os.UserCacheDir(); err == nil {
		// WebView2 keeps its profile next to the .exe otherwise: in the Downloads folder
		dataPath = filepath.Join(dir, "armsx2-frame-installer", "WebView2")
	}
	w := webview2.NewWithOptions(webview2.WebViewOptions{
		AutoFocus: true,
		DataPath:  dataPath,
		WindowOptions: webview2.WindowOptions{
			Title:  appTitle,
			Width:  760,
			Height: 820,
			IconId: 1, // the application icon (rsrc_windows_amd64.syso, see make-installer.sh)
			Center: true,
		},
	})
	if w == nil {
		return false
	}
	defer w.Destroy()
	w.Navigate(url)
	w.Run()
	return true
}

func showError(message string) {
	windows.MessageBox(0, windows.StringToUTF16Ptr(message), windows.StringToUTF16Ptr("ARMSX2 installer"), windows.MB_OK|windows.MB_ICONERROR)
}

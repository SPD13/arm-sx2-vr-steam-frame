// ARMSX2 - Steam Frame Edition installer: a desktop app (Windows, macOS, Linux) that finds a Steam Frame on the network,
// checks it can be installed to (Developer Mode, password, Steam running, free space), then downloads the latest release from
// GitHub and installs or updates the game on the Frame, all over its own ssh connection: nothing else needs to be installed on
// the computer, and the password is asked once. Ported from the Visual Pinball X - Steam Frame Edition installer.
//
// The interface is a web page (ui/) served on 127.0.0.1 and shown in a native window (WebView2 on Windows, WebKit on macOS);
// on Linux, or when no web view is available, it opens in the default browser and the app quits when the page is closed.
// The steps on the Frame are those of the release itself: tools/frame-install.sh from the downloaded archive (see
// ../stage-release.sh), and ../frame-uninstall.sh (embedded at build time, see ../make-installer.sh).
//
//   armsx2-frame-installer [--browser [--no-open]] [--archive FILE.tar.gz] [--gameid ID]
//
//   --browser         show the interface in the default browser instead of a window
//   --no-open         with --browser: only print the address to open
//   --archive FILE    install this release archive instead of downloading the latest one (testing, offline installs)
//   --gameid ID       identifier of the title for Steam's devkit tools (default: ARMSX2)
package main

import (
	"context"
	"crypto/rand"
	"embed"
	"encoding/hex"
	"flag"
	"fmt"
	"io/fs"
	"net"
	"net/http"
	"os"
	"os/exec"
	"path/filepath"
	"regexp"
	"runtime"
	"time"
)

const appTitle = "ARMSX2 - Steam Frame Edition Installer"

var version = "dev" // set by make-installer.sh

//go:embed ui
var uiFiles embed.FS

//go:embed embedded/frame-uninstall.sh
var frameUninstallScript string

func main() {
	browser := flag.Bool("browser", false, "show the interface in the default browser")
	noOpen := flag.Bool("no-open", false, "with --browser: only print the address to open")
	archive := flag.String("archive", "", "install this release archive (.tar.gz) instead of the latest release")
	gameID := flag.String("gameid", "ARMSX2", "identifier of the title for Steam's devkit tools")
	flag.Parse()
	if !regexp.MustCompile(`^[A-Za-z_][A-Za-z0-9_.]+$`).MatchString(*gameID) {
		fmt.Fprintln(os.Stderr, "invalid --gameid: letters, digits, '_' and '.' only")
		os.Exit(2)
	}

	openLog()
	logf("%s %s on %s/%s", appTitle, version, runtime.GOOS, runtime.GOARCH)

	tokenBytes := make([]byte, 16)
	rand.Read(tokenBytes)
	app := newApp(hex.EncodeToString(tokenBytes), *gameID, *archive)

	listener, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		fatal("cannot start: " + err.Error())
	}
	ui, _ := fs.Sub(uiFiles, "ui")
	mux := http.NewServeMux()
	mux.Handle("/", http.FileServer(http.FS(ui)))
	app.routes(mux)
	go http.Serve(listener, mux)
	url := fmt.Sprintf("http://%s/#%s", listener.Addr(), app.token)

	go app.start()

	if !*browser && openWindow(url) {
		app.shutdown()
		return
	}
	// Browser: the app runs until the page is closed (it polls the state while open) or the Quit button is pressed
	fmt.Println(appTitle)
	fmt.Println("The installer is open in your browser. If it did not open, go to:")
	fmt.Println("   " + url)
	if !*noOpen {
		openBrowser(url)
	}
	app.waitForPageClosed(context.Background())
	app.shutdown()
}

func openBrowser(url string) {
	var cmd *exec.Cmd
	switch runtime.GOOS {
	case "windows":
		cmd = exec.Command("rundll32", "url.dll,FileProtocolHandler", url)
	case "darwin":
		cmd = exec.Command("open", url)
	default:
		cmd = exec.Command("xdg-open", url)
	}
	if err := cmd.Start(); err == nil {
		go cmd.Wait()
	}
}

// installer.log, in the user's cache folder (shown on the page): what the checks found and what the install printed, for help
// when something goes wrong
var logFile *os.File
var logPath string

func openLog() {
	dir, err := os.UserCacheDir()
	if err != nil {
		return
	}
	dir = filepath.Join(dir, "armsx2-frame-installer")
	os.MkdirAll(dir, 0o700)
	logPath = filepath.Join(dir, "installer.log")
	logFile, _ = os.Create(logPath)
}

func logf(format string, args ...any) {
	if logFile != nil {
		fmt.Fprintf(logFile, time.Now().Format("15:04:05 ")+format+"\n", args...)
	}
}

func fatal(message string) {
	fmt.Fprintln(os.Stderr, message)
	showError(message)
	os.Exit(1)
}

package main

// The ssh connection to the Frame, with the client of golang.org/x/crypto/ssh (no ssh program needed on the computer).
// Signs in with the user's ssh keys when the Frame accepts them, else with the password set in Developer Mode. The Frame's
// host key is trusted the first time and remembered (settings.json); a changed key stops the connection until the user
// trusts the new one (a reset Frame has a new key).

import (
	"bufio"
	"bytes"
	"encoding/base64"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"net"
	"os"
	"path/filepath"
	"strings"
	"sync"
	"time"

	"golang.org/x/crypto/ssh"
	"golang.org/x/crypto/ssh/agent"
)

// settings.json in the user's configuration folder: the last Frame used and the host keys of the Frames seen
type settings struct {
	LastAddress string            `json:"lastAddress,omitempty"`
	HostKeys    map[string]string `json:"hostKeys,omitempty"` // host name or address -> key type and base64, like known_hosts
}

var settingsLock sync.Mutex

func settingsPath() string {
	dir, err := os.UserConfigDir()
	if err != nil {
		return ""
	}
	return filepath.Join(dir, "armsx2-frame-installer", "settings.json")
}

func loadSettings() settings {
	settingsLock.Lock()
	defer settingsLock.Unlock()
	var s settings
	if data, err := os.ReadFile(settingsPath()); err == nil {
		json.Unmarshal(data, &s)
	}
	if s.HostKeys == nil {
		s.HostKeys = map[string]string{}
	}
	return s
}

func updateSettings(change func(*settings)) {
	s := loadSettings()
	change(&s)
	settingsLock.Lock()
	defer settingsLock.Unlock()
	path := settingsPath()
	if path == "" {
		return
	}
	os.MkdirAll(filepath.Dir(path), 0o700)
	data, _ := json.MarshalIndent(s, "", "  ")
	os.WriteFile(path, data, 0o600)
}

// errHostKeyChanged: the Frame does not have the key it had before
type errHostKeyChanged struct{ name, key string }

func (e *errHostKeyChanged) Error() string { return "the Frame's identity (ssh host key) has changed" }

func keyText(key ssh.PublicKey) string {
	return key.Type() + " " + base64.StdEncoding.EncodeToString(key.Marshal())
}

func hostKeyCallback(keyName string) ssh.HostKeyCallback {
	return func(_ string, _ net.Addr, key ssh.PublicKey) error {
		known, ok := loadSettings().HostKeys[keyName]
		text := keyText(key)
		if !ok {
			updateSettings(func(s *settings) { s.HostKeys[keyName] = text })
			return nil
		}
		if known != text {
			return &errHostKeyChanged{name: keyName, key: text}
		}
		return nil
	}
}

// authMethods: the ssh agent and the user's unencrypted keys first (no password to type for those who set a key up), then the
// password. SteamOS asks for it as "keyboard-interactive" (PAM), other systems as "password": both are offered.
func authMethods(password string) []ssh.AuthMethod {
	var methods []ssh.AuthMethod
	var signers []ssh.Signer
	if socket := os.Getenv("SSH_AUTH_SOCK"); socket != "" {
		if conn, err := net.DialTimeout("unix", socket, 2*time.Second); err == nil {
			if agentSigners, err := agent.NewClient(conn).Signers(); err == nil {
				signers = append(signers, agentSigners...)
			}
		}
	}
	if home, err := os.UserHomeDir(); err == nil {
		for _, name := range []string{"id_ed25519", "id_ecdsa", "id_rsa"} {
			if data, err := os.ReadFile(filepath.Join(home, ".ssh", name)); err == nil {
				if signer, err := ssh.ParsePrivateKey(data); err == nil {
					signers = append(signers, signer)
				}
			}
		}
	}
	if len(signers) > 4 { // the server stops after 6 failed attempts: keep some for the password
		signers = signers[:4]
	}
	if len(signers) > 0 {
		methods = append(methods, ssh.PublicKeys(signers...))
	}
	if password != "" {
		methods = append(methods,
			ssh.KeyboardInteractive(func(_, _ string, questions []string, _ []bool) ([]string, error) {
				answers := make([]string, len(questions))
				for i := range answers {
					answers[i] = password
				}
				return answers, nil
			}),
			ssh.Password(password))
	}
	return methods
}

// frameConnection is the open ssh connection; keepalives notice a Frame that went to sleep
type frameConnection struct {
	client *ssh.Client
	lost   chan struct{}
}

func dialFrame(user, address, keyName, password string) (*frameConnection, error) {
	config := &ssh.ClientConfig{
		User:            user,
		Auth:            authMethods(password),
		HostKeyCallback: hostKeyCallback(keyName),
		Timeout:         15 * time.Second,
	}
	// Ask for the type of key already known, so that the Frame's other keys (it has several types) never look like a change
	if known, ok := loadSettings().HostKeys[keyName]; ok {
		if keyType, _, _ := strings.Cut(known, " "); keyType != ssh.KeyAlgoRSA {
			config.HostKeyAlgorithms = []string{keyType}
		}
	}
	client, err := ssh.Dial("tcp", net.JoinHostPort(address, "22"), config)
	if err != nil {
		return nil, err
	}
	conn := &frameConnection{client: client, lost: make(chan struct{})}
	go func() {
		defer close(conn.lost)
		for {
			result := make(chan error, 1)
			go func() { _, _, err := client.SendRequest("keepalive@openssh.com", true, nil); result <- err }()
			select {
			case err := <-result:
				if err != nil {
					client.Close()
					return
				}
			case <-time.After(20 * time.Second):
				client.Close() // no answer: the Frame is asleep or gone
				return
			}
			time.Sleep(10 * time.Second)
		}
	}()
	return conn, nil
}

func (c *frameConnection) close() { c.client.Close() }

func (c *frameConnection) alive() bool {
	select {
	case <-c.lost:
		return false
	default:
		return true
	}
}

// output runs a command and returns what it prints
func (c *frameConnection) output(command string, stdin io.Reader) (string, error) {
	session, err := c.client.NewSession()
	if err != nil {
		return "", err
	}
	defer session.Close()
	var out, errOut bytes.Buffer
	session.Stdout = &out
	session.Stderr = &errOut
	session.Stdin = stdin
	if err := session.Run(command); err != nil {
		message := strings.TrimSpace(errOut.String())
		if message == "" {
			message = err.Error()
		}
		return out.String(), errors.New(message)
	}
	return out.String(), nil
}

// send runs a command with data on its input, and returns once all of it is sent: remote is the command's error, local the
// error reading data
func (c *frameConnection) send(command string, data io.Reader) (remote, local error) {
	session, err := c.client.NewSession()
	if err != nil {
		return err, nil
	}
	defer session.Close()
	var errOut bytes.Buffer
	session.Stderr = &errOut
	stdin, err := session.StdinPipe()
	if err != nil {
		return err, nil
	}
	if err := session.Start(command); err != nil {
		return err, nil
	}
	copied := make(chan error, 1)
	go func() {
		_, err := io.Copy(stdin, data)
		stdin.Close()
		copied <- err
	}()
	remote = session.Wait()
	session.Close() // a copy still writing (the command stopped early) fails now
	local = <-copied
	if remote != nil && strings.TrimSpace(errOut.String()) != "" {
		remote = errors.New(strings.TrimSpace(errOut.String()))
	}
	return remote, local
}

// stream runs a command and passes each line it prints (output and errors) to line as it comes
func (c *frameConnection) stream(command string, stdin io.Reader, line func(text string, isError bool)) error {
	session, err := c.client.NewSession()
	if err != nil {
		return err
	}
	defer session.Close()
	stdout, err := session.StdoutPipe()
	if err != nil {
		return err
	}
	stderr, err := session.StderrPipe()
	if err != nil {
		return err
	}
	session.Stdin = stdin
	var lock sync.Mutex
	var readers sync.WaitGroup
	read := func(r io.Reader, isError bool) {
		defer readers.Done()
		scanner := bufio.NewScanner(r)
		scanner.Buffer(make([]byte, 64*1024), 1024*1024)
		for scanner.Scan() {
			lock.Lock()
			line(strings.TrimRight(scanner.Text(), "\r"), isError)
			lock.Unlock()
		}
	}
	readers.Add(2)
	go read(stdout, false)
	go read(stderr, true)
	if err := session.Start(command); err != nil {
		return err
	}
	readers.Wait()
	return session.Wait()
}

// shellQuote quotes a value for the Frame's shell
func shellQuote(s string) string { return "'" + strings.ReplaceAll(s, "'", `'\''`) + "'" }

func formatMB(bytes int64) string { return fmt.Sprintf("%.0f MB", float64(bytes)/1e6) }

package main

// Finding the Frame on the network. Developer Mode advertises the SteamOS devkit service (_steamos-devkit._tcp) over multicast
// DNS, which is how Valve's devkit client finds devices: one query lists every Frame (and Steam Deck in Developer Mode) on the
// network with its address, whatever its name. A name like frame.local is resolved by the system first, then by the same kind
// of query, since some systems (Linux without nss-mdns, some Windows setups) do not resolve .local names themselves.
//
// The queries come from an ordinary UDP port, not 5353: responders then answer that port directly ("legacy unicast", RFC 6762
// 6.7), so there is no multicast group to join and no clash with the system's own mDNS service.

import (
	"context"
	"net"
	"sort"
	"strings"
	"time"

	"golang.org/x/net/dns/dnsmessage"
	"golang.org/x/net/ipv4"
)

const devkitService = "_steamos-devkit._tcp.local."

type foundFrame struct {
	Name    string `json:"name"`    // the device's name, e.g. "frame"
	Host    string `json:"host"`    // its .local host name, e.g. "frame.local"
	Address string `json:"address"` // its IPv4 address
}

// discoverFrames lists the devices advertising the devkit service, waiting up to timeout for their answers
func discoverFrames(timeout time.Duration) []foundFrame {
	answers := mdnsQuery([]dnsmessage.Question{{Name: mustName(devkitService), Type: dnsmessage.TypePTR, Class: dnsmessage.ClassINET}}, timeout, nil)
	instances := map[string]bool{}
	targets := map[string]string{} // instance -> host name
	hostAddresses := map[string]string{}
	sources := map[string]string{} // instance -> address of the device that answered
	gather(answers, instances, targets, hostAddresses, sources)
	// SteamOS answers the service query with the device names only: ask for their host names
	var questions []dnsmessage.Question
	for instance := range instances {
		if name, err := dnsmessage.NewName(instance); err == nil && targets[instance] == "" {
			questions = append(questions, dnsmessage.Question{Name: name, Type: dnsmessage.TypeSRV, Class: dnsmessage.ClassINET})
		}
	}
	if len(questions) > 0 {
		gather(mdnsQuery(questions, timeout/2, nil), instances, targets, hostAddresses, sources)
	}
	var frames []foundFrame
	seen := map[string]bool{}
	for instance := range instances {
		frame := foundFrame{Name: strings.TrimSuffix(instance, "."+devkitService), Address: sources[instance]}
		if host, ok := targets[instance]; ok {
			frame.Host = strings.TrimSuffix(host, ".")
			if address, ok := hostAddresses[host]; ok {
				frame.Address = address
			}
		}
		if frame.Address == "" || seen[frame.Address] {
			continue
		}
		seen[frame.Address] = true
		frames = append(frames, frame)
	}
	sort.Slice(frames, func(i, j int) bool { return frames[i].Name < frames[j].Name })
	return frames
}

func gather(answers []mdnsAnswer, instances map[string]bool, targets, hostAddresses, sources map[string]string) {
	for _, answer := range answers {
		for _, r := range answer.records {
			name := strings.ToLower(r.Header.Name.String())
			switch body := r.Body.(type) {
			case *dnsmessage.PTRResource:
				if name == devkitService {
					instance := strings.ToLower(body.PTR.String())
					instances[instance] = true
					sources[instance] = answer.source
				}
			case *dnsmessage.SRVResource:
				targets[name] = strings.ToLower(body.Target.String())
				if instances[name] {
					sources[name] = answer.source
				}
			case *dnsmessage.AResource:
				hostAddresses[name] = net.IP(body.A[:]).String()
			}
		}
	}
}

// resolveHost finds the IPv4 address of a host name: the system's resolver, then a multicast DNS query for .local names
func resolveHost(host string, timeout time.Duration) (string, bool) {
	ctx, cancel := context.WithTimeout(context.Background(), timeout)
	defer cancel()
	if addresses, err := net.DefaultResolver.LookupIP(ctx, "ip4", host); err == nil && len(addresses) > 0 {
		return addresses[0].String(), true
	}
	if !strings.HasSuffix(strings.ToLower(strings.TrimSuffix(host, ".")), ".local") {
		return "", false
	}
	fqdn := strings.ToLower(strings.TrimSuffix(host, ".")) + "."
	name, err := dnsmessage.NewName(fqdn)
	if err != nil {
		return "", false
	}
	var found string
	mdnsQuery([]dnsmessage.Question{{Name: name, Type: dnsmessage.TypeA, Class: dnsmessage.ClassINET}}, timeout, func(answers []mdnsAnswer) bool {
		for _, answer := range answers {
			for _, r := range answer.records {
				if a, ok := r.Body.(*dnsmessage.AResource); ok && strings.EqualFold(r.Header.Name.String(), fqdn) {
					found = net.IP(a.A[:]).String()
					return true
				}
			}
		}
		return false
	})
	return found, found != ""
}

type mdnsAnswer struct {
	source  string
	records []dnsmessage.Resource
}

// mdnsQuery sends the questions on every network interface and gathers the answers until timeout, or until done says it has
// what it needs
func mdnsQuery(questions []dnsmessage.Question, timeout time.Duration, done func([]mdnsAnswer) bool) []mdnsAnswer {
	builder := dnsmessage.NewBuilder(nil, dnsmessage.Header{})
	builder.EnableCompression()
	builder.StartQuestions()
	for _, q := range questions {
		builder.Question(q)
	}
	query, err := builder.Finish()
	if err != nil {
		return nil
	}
	conn, err := net.ListenUDP("udp4", &net.UDPAddr{IP: net.IPv4zero})
	if err != nil {
		return nil
	}
	defer conn.Close()
	group := &net.UDPAddr{IP: net.IPv4(224, 0, 0, 251), Port: 5353}
	packetConn := ipv4.NewPacketConn(conn)
	packetConn.SetMulticastTTL(255)
	send := func() {
		sent := false
		if interfaces, err := net.Interfaces(); err == nil {
			for _, ifi := range interfaces {
				if ifi.Flags&net.FlagUp == 0 || ifi.Flags&net.FlagMulticast == 0 || ifi.Flags&net.FlagLoopback != 0 || !hasIPv4(ifi) {
					continue
				}
				ifi := ifi
				if packetConn.SetMulticastInterface(&ifi) == nil {
					if _, err := conn.WriteTo(query, group); err == nil {
						sent = true
					}
				}
			}
		}
		if !sent { // the system's default interface
			conn.WriteTo(query, group)
		}
	}
	send()

	var answers []mdnsAnswer
	deadline := time.Now().Add(timeout)
	resend := time.Now().Add(timeout / 3)
	buffer := make([]byte, 9000)
	for time.Now().Before(deadline) {
		next := deadline
		if resend.Before(next) {
			next = resend
		}
		conn.SetReadDeadline(next)
		n, from, err := conn.ReadFromUDP(buffer)
		if err != nil {
			if time.Now().After(resend) { // multicast can be lost on Wi-Fi: ask again once or twice
				send()
				resend = resend.Add(timeout / 3)
			}
			continue
		}
		var parser dnsmessage.Parser
		header, err := parser.Start(buffer[:n])
		if err != nil || !header.Response {
			continue
		}
		parser.SkipAllQuestions()
		answer := mdnsAnswer{source: from.IP.String()}
		for _, section := range []func() ([]dnsmessage.Resource, error){parser.AllAnswers, parser.AllAuthorities, parser.AllAdditionals} {
			records, err := section()
			if err != nil {
				break
			}
			answer.records = append(answer.records, records...)
		}
		answers = append(answers, answer)
		if done != nil && done(answers) {
			break
		}
	}
	return answers
}

func hasIPv4(ifi net.Interface) bool {
	addresses, err := ifi.Addrs()
	if err != nil {
		return false
	}
	for _, address := range addresses {
		if ipNet, ok := address.(*net.IPNet); ok && ipNet.IP.To4() != nil {
			return true
		}
	}
	return false
}

func mustName(s string) dnsmessage.Name {
	return dnsmessage.MustNewName(s)
}

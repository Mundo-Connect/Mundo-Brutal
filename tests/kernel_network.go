//go:build linux

// Independent-host TCP test: kernel_network server|client IP:PORT SECONDS.
// The server requires mundo.ko; MUNDO_CC=cubic selects the native control.
package main

import (
	"encoding/binary"
	"fmt"
	"log"
	"net"
	"os"
	"strconv"
	"syscall"
	"time"
)

func main() {
	if len(os.Args) != 4 {
		log.Fatal("server|client IP:PORT SECONDS")
	}
	seconds, err := strconv.Atoi(os.Args[3])
	if err != nil {
		log.Fatal(err)
	}
	var conn *net.TCPConn
	if os.Args[1] == "server" {
		addr, err := net.ResolveTCPAddr("tcp", os.Args[2])
		if err != nil {
			log.Fatal(err)
		}
		listener, err := net.ListenTCP("tcp", addr)
		if err != nil {
			log.Fatal(err)
		}
		defer listener.Close()
		listener.SetDeadline(time.Now().Add(time.Minute))
		fmt.Println("READY", listener.Addr())
		conn, err = listener.AcceptTCP()
		if err != nil {
			log.Fatal(err)
		}
		raw, _ := conn.SyscallConn()
		algorithm := os.Getenv("MUNDO_CC")
		if algorithm == "" {
			algorithm = "brutal"
		}
		controlErr := raw.Control(func(fd uintptr) {
			err = syscall.SetsockoptString(int(fd), syscall.IPPROTO_TCP, syscall.TCP_CONGESTION, algorithm)
			if err != nil || algorithm != "brutal" {
				return
			}
			var params [12]byte
			binary.NativeEndian.PutUint64(params[:8], 125000000)
			binary.NativeEndian.PutUint32(params[8:], 18)
			err = syscall.SetsockoptString(int(fd), syscall.IPPROTO_TCP, 23301, string(params[:]))
		})
		if controlErr != nil {
			log.Fatal(controlErr)
		}
		if err != nil {
			log.Fatal(err)
		}
	} else {
		dialer := net.Dialer{Timeout: 10 * time.Second}
		c, err := dialer.Dial("tcp", os.Args[2])
		if err != nil {
			log.Fatal(err)
		}
		conn = c.(*net.TCPConn)
	}
	conn.SetLinger(0)
	defer conn.Close()
	start := time.Now()
	end := start.Add(time.Duration(seconds) * time.Second)
	conn.SetDeadline(end)
	var data [128 << 10]byte
	var total int64
	for time.Now().Before(end) {
		var n int
		if os.Args[1] == "server" {
			n, err = conn.Write(data[:])
		} else {
			n, err = conn.Read(data[:])
		}
		total += int64(n)
		if err != nil {
			break
		}
	}
	fmt.Printf("%s bytes=%d delivered=%.3f Mbps elapsed=%.3fs\n", os.Args[1], total,
		float64(total)*8/time.Since(start).Seconds()/1e6, time.Since(start).Seconds())
}

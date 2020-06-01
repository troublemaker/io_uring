package main

import (
    "io"
    "net"
    "log"
)

func main() {
    sock_listen, err := net.Listen("tcp", "0.0.0.0:7777")
    if err != nil {
        log.Fatal(err)
    }
    for {
        newConn, err := sock_listen.Accept()
        if err != nil {
            log.Fatal(err)
        }

        go func(c net.Conn) {
            defer c.Close()
            io.Copy(c, c)
        }(newConn)
    }
}
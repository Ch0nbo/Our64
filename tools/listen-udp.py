#!/usr/bin/env python3
# Prints WHBLogUdp broadcasts (port 4405) from the console as they arrive.
import socket
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
s.bind(("", 4405))
while True:
    data, addr = s.recvfrom(4096)
    print(data.decode("utf-8", "replace").rstrip("\n"), flush=True)

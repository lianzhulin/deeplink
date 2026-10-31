#!/usr/bin/env python3
"""Minimal debug: STREAM_WR 1136 bytes + recv 5 byte response."""
import socket, struct, time, sys

HOST, PORT = "127.0.0.1", 9527
s = socket.socket()
s.settimeout(5)
s.connect((HOST, PORT))
print("connected")

# STREAM_WR op=2 addr=0 len=1136
data = b'X' * 1136
frame = struct.pack('>BII', 2, 0, len(data)) + data
print(f"sending {len(frame)} bytes...")
s.sendall(frame)
print("sent")

print("recv 5 bytes...")
resp = b''
while len(resp) < 5:
    try:
        c = s.recv(5 - len(resp))
        if not c:
            print(f"CONNECTION CLOSED (got {len(resp)} bytes)")
            break
        resp += c
    except socket.timeout:
        print(f"TIMED OUT (got {len(resp)} bytes)")
        break
    except Exception as e:
        print(f"EXCEPTION: {e}")
        break

print(f"resp ({len(resp)} bytes): {resp!r}")
if len(resp) >= 5:
    st, rv = struct.unpack('>BI', resp[:5])
    print(f"status={st} ret_val={rv}")
s.close()
print("done")

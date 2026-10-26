#!/usr/bin/env python3
"""Run production HTTP transport against a local TCP server; no flash writes."""
from pathlib import Path
import ctypes as C
import socket
import subprocess
import tempfile
import threading

OTA = Path(__file__).resolve().parents[2] / 'ota'
with tempfile.TemporaryDirectory() as tmp:
    so = Path(tmp) / 'http.so'
    subprocess.run(['cc', '-Wall', '-Wextra', '-Werror', '-shared', '-fPIC',
                    '-DACTION_HOST_TEST', str(OTA / 'carplay_ota_http.c'), str(OTA / 'wifi_action_protocol.c'), '-o', str(so)], check=True)
    lib = C.CDLL(str(so))
    BEGIN = C.CFUNCTYPE(C.c_int, C.c_void_p, C.c_uint32)
    DATA = C.CFUNCTYPE(C.c_int, C.c_void_p, C.c_void_p, C.c_uint32)
    lib.carbox_ota_http_get.argtypes = [C.c_char_p, C.c_int, C.c_char_p, BEGIN, DATA, C.c_void_p]
    def run(response, expected, payload=None, reject_begin=False, reject_data=False, pieces=1):
        listener = socket.socket()
        listener.bind(('127.0.0.1', 0)); listener.listen(1)
        port = listener.getsockname()[1]
        requests = []
        def server():
            conn, _ = listener.accept()
            with conn:
                req = b''
                while b'\r\n\r\n' not in req:
                    req += conn.recv(1024)
                requests.append(req)
                try:
                    for offset in range(0, len(response), pieces):
                        conn.sendall(response[offset:offset+pieces])
                except (BrokenPipeError, ConnectionResetError):
                    pass
            listener.close()
        thread = threading.Thread(target=server); thread.start()
        lengths, chunks = [], []
        @BEGIN
        def begin(ctx, length):
            lengths.append(length)
            return -1 if reject_begin else 0
        @DATA
        def data(ctx, p, n):
            chunks.append(C.string_at(p,n))
            return -1 if reject_data else 0
        ret = lib.carbox_ota_http_get(b'127.0.0.1',port,b'/ota_all.bin',begin,data,None)
        thread.join(5)
        assert not thread.is_alive()
        assert (ret == 0) == expected, (ret, expected, response[:100])
        assert requests[0].startswith(b'GET /ota_all.bin HTTP/1.0\r\n')
        if payload is not None:
            assert b''.join(chunks) == payload
            assert lengths == [len(payload)]
        return lengths, chunks
    body = bytes(range(256)) * 40
    header = b'HTTP/1.1 200 OK\r\ncontent-length: 10240\r\n\r\n'
    run(header+body,True,body,pieces=7)
    run(header+body,True,body,pieces=4096)
    for invalid in [
        b'HTTP/1.0 404 Not Found\r\nContent-Length: 3\r\n\r\nabc',
        b'HTTP/1.0 200 OK\r\n\r\nabc',
        b'HTTP/1.0 200 OK\r\nContent-Length: 3\r\nContent-Length: 3\r\n\r\nabc',
        b'HTTP/1.0 200 OK\r\nContent-Length: 3\r\nTransfer-Encoding: chunked\r\n\r\nabc',
        b'HTTP/1.0 200 OK\r\nContent-Length: 4294967296\r\n\r\nabc',
        b'HTTP/1.0 200 OK\r\nContent-Length: -1\r\n\r\nabc',
        b'HTTP/1.0 200 OK\r\nContent-Length: 3junk\r\n\r\nabc',
        b'HTTP/1.0 200 OK\r\nX-Fill: '+b'x'*4096+b'\r\n\r\n',
    ]:
        lengths, chunks = run(invalid,False,pieces=512)
        assert not lengths and not chunks
    run(header+body[:-1],False,pieces=512)  # premature EOF
    lengths,chunks=run(header+body,False,reject_begin=True,pieces=1024)
    assert not chunks
    run(header+body,False,reject_data=True,pieces=1024)
    # Smart follows one HTTP 302, then requires the second response to be 200.
    def redirect_case(second_redirect=False):
        listener=socket.socket(); listener.bind(('127.0.0.1',0)); listener.listen(2)
        listener.settimeout(1)
        port=listener.getsockname()[1]; seen=[]
        redirect=(f'HTTP/1.1 302 Found\r\nLocation: http://127.0.0.1:{port}/new.bin\r\nContent-Length: 0\r\n\r\n').encode()
        def server():
            try:
                for reply in (redirect, redirect if second_redirect else header+body):
                    conn,_=listener.accept()
                    with conn:
                        req=b''
                        while b'\r\n\r\n' not in req: req+=conn.recv(1024)
                        seen.append(req); conn.sendall(reply)
            except TimeoutError:
                pass
            finally:
                listener.close()
        thread=threading.Thread(target=server);thread.start()
        chunks=[]
        @BEGIN
        def begin(ctx,n): return 0 if n==len(body) else -1
        @DATA
        def data(ctx,p,n): chunks.append(C.string_at(p,n));return 0
        ret=lib.carbox_ota_http_get(b'127.0.0.1',port,b'/old.bin',begin,data,None)
        thread.join(3)
        assert (ret==0)==(not second_redirect)
        assert len(seen)==2 and seen[1].startswith(b'GET /new.bin ')
        assert b''.join(chunks)==(b'' if second_redirect else body)
    redirect_case()
    redirect_case(True)
    print('action HTTP transport: PASS')

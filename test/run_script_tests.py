#!/usr/bin/env python3
"""Execute real FAT32 scripts under QEMU, including a local TCP/HTTP server."""
import os
from pathlib import Path
import socket
import struct
import subprocess
import threading
from run_command_tests import run_os

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "out" / "script-tests"


def image_with_script(name, source, extra_files=None):
    disk = OUT / (name + ".img")
    subprocess.run([str(ROOT / "out" / "mkfat32"), str(disk)], check=True)
    image = bytearray(disk.read_bytes())
    reserved = struct.unpack_from("<H", image, 14)[0]
    fats = image[16]
    fat_sectors = struct.unpack_from("<I", image, 36)[0]
    data_start = (reserved + fats * fat_sectors) * 512
    cluster = 6
    for entry_index, (filename, data) in enumerate({"RUN.MOS": source.encode(), **(extra_files or {})}.items(), 2):
        base, extension = filename.split(".")
        entry = data_start + entry_index * 32
        image[entry:entry + 11] = (base.upper().ljust(8) + extension.upper().ljust(3)).encode()
        image[entry + 11] = 0x20
        struct.pack_into("<H", image, entry + 26, cluster)
        struct.pack_into("<I", image, entry + 28, len(data))
        chunks = max(1, (len(data) + 511) // 512)
        for i in range(chunks):
            for fat in range(fats):
                struct.pack_into("<I", image, (reserved + fat * fat_sectors) * 512 + (cluster + i) * 4,
                                 0x0fffffff if i == chunks - 1 else cluster + i + 1)
            start = data_start + (cluster + i - 2) * 512
            chunk = data[i * 512:(i + 1) * 512]
            image[start:start + len(chunk)] = chunk
        cluster += chunks
    disk.write_bytes(image)
    return disk


def check(name, source, expected=(), rejected=(), commands="exec RUN.MOS\nexit\n", extra_files=None, modern=False):
    disk = image_with_script(name, source, extra_files)
    out = run_os(str(ROOT), str(disk), commands, timeout=30, modern=modern)
    (OUT / (name + ".out")).write_text(out)
    for text in expected:
        assert text in out, (name, "missing", text, out)
    for text in rejected:
        assert text not in out, (name, "unexpected", text, out)
    print("PASS script", name)
    return out



def read_root_file(disk, filename):
    image = Path(disk).read_bytes()
    reserved = struct.unpack_from("<H", image, 14)[0]
    fats = image[16]
    fat_sectors = struct.unpack_from("<I", image, 36)[0]
    data_start = (reserved + fats * fat_sectors) * 512
    base, extension = filename.split(".")
    short = (base.ljust(8) + extension.ljust(3)).encode()
    for offset in range(data_start, data_start + 512, 32):
        if image[offset:offset + 11] == short:
            cluster = struct.unpack_from("<H", image, offset + 26)[0]
            cluster |= struct.unpack_from("<H", image, offset + 20)[0] << 16
            size = struct.unpack_from("<I", image, offset + 28)[0]
            result = bytearray()
            while 2 <= cluster < 0x0ffffff8 and len(result) < size:
                start = data_start + (cluster - 2) * 512
                result += image[start:start + 512]
                cluster = struct.unpack_from("<I", image, reserved * 512 + cluster * 4)[0] & 0x0fffffff
            assert len(result) >= size, "truncated FAT chain"
            return bytes(result[:size])
    raise AssertionError("file missing: " + filename)


def network_check(modern=False):
    # A split response larger than both the virtqueue and the TCP receive window
    # verifies chunking, checksums, cumulative ACKs, window updates, and EOF.
    payload = b"stream-data-" * 1800
    response = b"HTTP/1.0 200 OK\r\nContent-Length: " + str(len(payload)).encode() + b"\r\n\r\n" + payload
    listener = socket.socket()
    listener.bind(("127.0.0.1", 0))
    listener.listen(1)
    listener.settimeout(15)
    port = listener.getsockname()[1]
    errors = []

    def serve():
        try:
            with listener.accept()[0] as conn:
                conn.settimeout(15)
                request = b""
                while b"\r\n\r\n" not in request:
                    data = conn.recv(4096)
                    assert data, "unexpected request EOF"
                    request += data
                assert request.startswith(b"GET / HTTP/1.0\r\n")
                for i in range(0, len(response), 379):
                    conn.sendall(response[i:i + 379])
        except Exception as exc:
            errors.append(exc)
        finally:
            listener.close()

    thread = threading.Thread(target=serve, daemon=True)
    thread.start()
    source = (ROOT / "examples" / "http.mos").read_text().replace("8000", str(port))
    source += '\nlet f = open("/HTTP.TXT", O_RDONLY); let b = malloc(4); read(f, b, 4); print(b); close(f); free(b);'
    check("http_modern" if modern else "http", source, (f"received={len(response)}", "HTTP\n"), ("script line", "failed"), modern=modern)
    thread.join(timeout=16)
    assert not thread.is_alive() and not errors, errors
    image_name = "http_modern" if modern else "http"
    assert read_root_file(OUT / (image_name + ".img"), "HTTP.TXT") == response



def network_bulk_check():
    listener = socket.socket()
    listener.bind(("127.0.0.1", 0))
    listener.listen(1)
    listener.settimeout(15)
    port = listener.getsockname()[1]
    expected = b"A" + bytes(16382) + b"Z"
    errors = []

    def serve():
        try:
            with listener.accept()[0] as conn:
                conn.settimeout(15)
                data = bytearray()
                while len(data) < len(expected):
                    chunk = conn.recv(8192)
                    assert chunk, "unexpected upload EOF"
                    data.extend(chunk)
                assert bytes(data) == expected
                conn.sendall(b"bulk ok")
        except Exception as exc:
            errors.append(exc)
        finally:
            listener.close()

    thread = threading.Thread(target=serve, daemon=True)
    thread.start()
    source = f'let s = tcp_connect("10.0.2.2", {port}); let b = malloc(16384); b[0] = 65; b[16383] = 90;'
    source += 'print("sent=", tcp_send(s, b, len(b))); free(b); let r = malloc(8); let n = tcp_recv(s, r, 8);'
    source += 'print(slice(r, 0, n)); print(tcp_recv(s, r, 8)); free(r); tcp_close(s);'
    check("tcp_bulk", source, ("sent=16384", "bulk ok\n0"), ("script line",))
    thread.join(timeout=16)
    assert not thread.is_alive() and not errors, errors


def main():
    OUT.mkdir(parents=True, exist_ok=True)
    check("features", (ROOT / "examples" / "features.mos").read_text(),
          ("9\n104\n7\n120\n", "sum=10", "else works", "0\n1\n155", "array works", "Hello from MINI_OS", "\n7\n7"),
          ("script line", "wrong branch"))
    check("copy", (ROOT / "examples" / "copy.mos").read_text(),
          ("copied=26", "Hello from MINI_OS FAT32."), ("script line",))
    # Multi-sector source and destination, with embedded NUL bytes.
    binary = bytes(range(256)) * 20
    source = (ROOT / "examples" / "copy.mos").read_text().replace("/TEST.TXT", "/BINARY.DAT")
    source = source[:source.index('print("copied=')]
    source += f'print("copied=", copy_file("/BINARY.DAT", "/COPY.DAT"));'
    source += '\nlet a = open("/BINARY.DAT", 0); let b = open("/COPY.DAT", 0); let x = malloc(1); let y = malloc(1);'
    source += '\nlet count = 0; let bad = 0; while (read(a, x, 1)) { if (read(b, y, 1) != 1 || x[0] != y[0]) { bad = 1; } count = count + 1; }'
    source += '\nprint("bytes=", count, ",bad=", bad); free(x); free(y); close(a); close(b);'
    check("binary_copy", source, (f"copied={len(binary)}", f"bytes={len(binary)},bad=0"),
          ("script line",), extra_files={"BINARY.DAT": binary})
    check("shell", 'let result = 4; calc $result + 3; write OUT.TXT hello; cat OUT.TXT;', ("\n7", "hello"), ("script line",))
    check("nested", 'fn f(a) { return a[0]; } print(f([41]));', ("41\n",), ("script line",))
    check("substitution", 'fn f(x) { let next = x + 1; print("x", next); return x; } print(f(2));',
          ("x3\n2",), ("script line",))
    check("escaped_substitution", 'fn f(path) { cat $path; return "path"; } print(f("/TEST.TXT"));',
          ("Hello from MINI_OS", "path\n"), ("script line",))
    check("string_operations", 'let s = "abc"; print(chr(s[0])); print(slice(s, 1, 2) + str(12)); print(s == "abc");',
          ("a\nbc12\n1",), ("script line",))
    check("function_string_cache", 'fn f(s) { return "a" + s; } print(f("x")); print(f("y"));',
          ("ax\nay",), ("script line",))
    check("arguments", 'fn twice(x) { x = x + 1; return x * 2; } let x = 10; print(twice(2)); print(x);', ("6\n10",), ("script line",))
    check("zero_repeat", 'while 0; calc 123; print("done");', ("done",), ("\n123", "script line"))
    errors = {
        "bounds": ('let s = "hi"; print(s[2]);', "index out of bounds"),
        "negative_index": ('let s = "hi"; print(s[-1]);', "invalid size or index"),
        "double_free": ('let b = malloc(8); free(b); free(b);', "expected live"),
        "use_after_free": ('let b = malloc(8); let a = b; free(b); a[0] = 1;', "expected live"),
        "freed_rhs": ('let a = [1]; a[0] = free(a);', "assignment target was freed"),
        "immutable": ('let s = "hi"; s[0] = 65;', "strings are immutable"),
        "divide_zero": ('let x = 4 / 0;', "division by zero"),
        "bad_type": ('number x = "hi";', "declaration type mismatch"),
        "missing_name": ('print(nope);', "undefined variable"),
        "bad_arity": ('fn f(a) { return a; } f();', "wrong function argument count"),
        "unterminated": ('if (1) { print(1);', "unterminated block"),
        "infinite_loop": ('while (1) { }', "execution limit exceeded"),
        "recursion": ('fn f() { return f(); } f();', "depth exceeded"),
        "deep_expression": ('print(' + '(' * 80 + '1' + ')' * 80 + ');', "expression depth exceeded"),
        "return_outside": ('if (1) { return 1; }', "return outside function"),
        "bad_alloc": ('let b = malloc(-1);', "invalid size or index"),
        "alloc_limit": ('let b = malloc(65537);', "invalid size or index"),
    }
    for name, (source, error) in errors.items():
        check(name, source, (error, "Shutting down"))
    # Repeat executions must release resources, including on errors.
    check("cleanup", 'let f = open("TEST.TXT", 0); let b = malloc(65536); print(b[65536]);',
          ("index out of bounds",), commands="exec RUN.MOS\n" * 20 + "cat TEST.TXT\nexit\n",
          rejected=("out of memory", "open failed"))
    check("network_errors", 'print(tcp_connect("999.1.1.1", 80)); print(tcp_send(-1, "x", 1)); print(tcp_recv(-1, malloc(2), 2));',
          ("-1\n-1\n-1",), ("script line",))
    subprocess.run([str(ROOT / "out" / "mkfat32"), str(OUT / "examples.img"),
                    str(ROOT / "examples" / "features.mos"), str(ROOT / "examples" / "copy.mos")], check=True)
    out = run_os(str(ROOT), str(OUT / "examples.img"), "exec FEATURES.MOS\nexec COPY.MOS\nexit\n")
    assert "sum=10" in out and "copied=26" in out and "script line" not in out, out
    print("PASS script imported_examples")
    network_check()
    network_check(modern=True)
    network_bulk_check()
    print("mini_os script tests passed")


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
"""A throwaway FTP server for the remote tests.

The tests need a server they can create, populate and destroy in the same second,
on a port nobody else is using, with a login they already know. pyftpdlib is not
installed here and a system ftpd would need root and an account, so this file
carries the few hundred lines of socket code that speak enough of RFC 959 for
libcurl: USER/PASS, PASV, CWD/PWD, LIST, MLSD, MLST, RETR, STOR, MKD, RMD, DELE,
RNFR/RNTO, SIZE, MDTM and SITE CHMOD.

It is deliberately narrow. Passive mode only, one user, and every path confined
to --root. Sessions run on their own thread, because the application being tested
keeps browsing a site while a transfer of its own is running against the same
server, and a fixture that served one session at a time would hide exactly the
overlap that matters. It is a test fixture, not a server, and it says so rather
than pretending otherwise.

The first thing it does is print one JSON line naming the port it bound, so the
test can start it and learn where it landed.
"""

import argparse
import contextlib
import json
import os
import posixpath
import shutil
import socket
import ssl
import stat
import sys
import threading
import time

# Set FTP_SERVER_LOG=1 to watch the protocol on stderr; the fixture is otherwise
# silent, which makes a client bug look like a server bug.
LOG = os.environ.get("FTP_SERVER_LOG") == "1"


def ls_line(name, st):
    """One unix-dialect LIST row.

    The client's parser counts eight fields from the front of the line and takes
    everything after them as the name, which is what keeps a name containing
    spaces in one piece.
    """
    when = time.strftime("%b %d %H:%M", time.localtime(st.st_mtime))
    return f"{stat.filemode(st.st_mode)} 1 ftp ftp {st.st_size} {when} {name}"


def mlsd_line(name, st):
    """One MLSD row: machine-readable facts, then the name.

    RFC 3659 puts a single space between the last ';' and the entry, and real
    servers send it. Leaving it out here is what let a client that took the whole
    tail after the ';' pass its own tests while giving every name on a real server
    a leading space.
    """
    when = time.strftime("%Y%m%d%H%M%S", time.gmtime(st.st_mtime))
    kind = "dir" if stat.S_ISDIR(st.st_mode) else "file"
    return f"type={kind};size={st.st_size};modify={when};unix.mode={st.st_mode & 0o7777:o}; {name}"


def mlst_facts(st):
    """The facts MLST prints before the path, in the case RFC 3659 shows them."""
    when = time.strftime("%Y%m%d%H%M%S", time.gmtime(st.st_mtime))
    kind = "dir" if stat.S_ISDIR(st.st_mode) else "File"
    return f"Type={kind};Size={st.st_size};Modify={when};Unix.mode={st.st_mode & 0o7777:o};"


class Session:
    def __init__(self, ctl, root, user, password, tls_ctx):
        self.ctl = ctl
        self.buf = b""
        self.root = os.path.realpath(root)
        self.user = user
        self.password = password
        self.tls_ctx = tls_ctx
        self.tls = False
        self.cwd = "/"
        self.logged_in = False
        self.user_seen = None
        self.rename_from = None
        self.pasv_srv = None

    # --- control channel ---------------------------------------------------

    def reply(self, code, text):
        self.ctl.sendall(f"{code} {text}\r\n".encode())
        if LOG:
            print(f"< {code} {text}", file=sys.stderr, flush=True)

    def send(self, text):
        self.ctl.sendall(text.encode())
        if LOG:
            print(f"< {text.strip()}", file=sys.stderr, flush=True)

    def readline(self):
        while b"\n" not in self.buf:
            chunk = self.ctl.recv(4096)
            if not chunk:
                return None
            self.buf += chunk
        line, self.buf = self.buf.split(b"\n", 1)
        return line.decode("utf-8", "replace").strip()

    # --- paths -------------------------------------------------------------

    def real(self, virt):
        """A virtual path under the root, or None if it tries to leave."""
        if not virt:
            return None
        joined = virt if virt.startswith("/") else posixpath.join(self.cwd, virt)
        joined = posixpath.normpath(joined)
        path = os.path.realpath(os.path.join(self.root, joined.lstrip("/")))
        if path != self.root and not path.startswith(self.root + os.sep):
            return None
        return path

    def virt(self, path):
        """The path a client would type for a real path inside the root."""
        rel = os.path.relpath(path, self.root)
        return "/" if rel == "." else posixpath.normpath("/" + rel.replace(os.sep, "/"))

    # --- data channel ------------------------------------------------------

    def open_data(self):
        """PASV: listen and name the port; the transfer command accepts on it.

        The listen happens on PASV and the accept on the command that follows,
        which is the order RFC 959 describes and the order libcurl expects: it
        reads the port from the 227 reply, sends LIST or STOR, and connects.
        """
        srv = socket.socket()
        srv.settimeout(20)
        srv.bind(("127.0.0.1", 0))
        srv.listen(1)
        port = srv.getsockname()[1]
        self.pasv_srv = srv
        self.reply(227, f"Entering Passive Mode (127,0,0,1,{port // 256},{port % 256})")

    def accept_data(self):
        """Take the connection PASV promised, if one was promised."""
        srv = self.pasv_srv
        if srv is None:
            raise OSError("no PASV before the transfer")
        self.pasv_srv = None
        conn, _ = srv.accept()
        srv.close()
        conn.settimeout(20)
        if self.tls:
            conn = self.tls_ctx.wrap_socket(conn, server_side=True)
        return conn

    def close_data(self, data):
        """Close a data connection the way TLS expects to be closed.

        Closing a wrapped socket drops the TCP connection without a close_notify
        alert. curl reads that truncation as `SSL_read: unexpected eof while
        reading` and reports a transfer whose every byte arrived as a failure -
        which looks exactly like a client bug. unwrap() sends the alert and hands
        back the plain socket underneath, which is then the one to close.
        """
        try:
            # Only an SSLSocket has unwrap(); a plain data connection has no
            # goodbye to send, and asking it for one is an AttributeError that
            # would take the whole fixture down mid-test.
            plain = data.unwrap() if isinstance(data, ssl.SSLSocket) else None
        except (OSError, ssl.SSLError):
            plain = None        # a plain data connection: nothing to say goodbye with
        with contextlib.suppress(OSError):
            data.close()
        if plain is not None:
            with contextlib.suppress(OSError):
                plain.close()

    # --- commands ----------------------------------------------------------

    def serve(self):
        self.reply(220, "mc-clone test ftp")
        try:
            while True:
                line = self.readline()
                if line is None:
                    return
                if not line:
                    continue
                cmd, _, arg = line.partition(" ")
                cmd = cmd.upper()
                if LOG:
                    print(f"> {line}", file=sys.stderr, flush=True)
                try:
                    if not self.dispatch(cmd, arg):
                        return
                except (OSError, ssl.SSLError):
                    return
        finally:
            # The control channel is TLS too once AUTH TLS has run, and it needs
            # the same goodbye: a client that has already read its listing can
            # still fail the command because the session ended uncleanly.
            self.close_ctl()

    def close_ctl(self):
        """Shut the control channel down with a close_notify when it is TLS."""
        ctl = self.ctl
        try:
            plain = ctl.unwrap() if isinstance(ctl, ssl.SSLSocket) else None
        except (OSError, ssl.SSLError):
            plain = None
        with contextlib.suppress(OSError):
            ctl.close()
        if plain is not None:
            with contextlib.suppress(OSError):
                plain.close()

    def dispatch(self, cmd, arg):
        if cmd == "USER":
            self.user_seen = arg
            if arg == self.user:
                self.reply(331, "Need a password")
            else:
                self.reply(530, "No such user")
            return True
        if cmd == "PASS":
            ok = self.user_seen == self.user and arg == self.password
            self.logged_in = ok
            self.reply(230 if ok else 530, "Logged in" if ok else "Login incorrect")
            return True
        if not self.logged_in and cmd not in ("QUIT", "AUTH", "PROT", "PBSZ"):
            self.reply(530, "Not logged in")
            return True

        if cmd == "SYST":
            self.reply(215, "UNIX Type: L8")
        elif cmd == "FEAT":
            self.ctl.sendall(b"211-Features\r\n MLSD\r\n MLST type*;size*;modify*;unix.mode;\r\n"
                             b" UTF8 MDTM SITE CHMOD\r\n211 End\r\n")
        elif cmd == "TYPE":
            self.reply(200, f"Type set to {arg or 'I'}")
        elif cmd == "NOOP":
            self.reply(200, "Ok")
        elif cmd == "PWD":
            self.reply(257, f'"{self.cwd}" is the current directory')
        elif cmd == "CWD":
            path = self.real(arg)
            if path and os.path.isdir(path):
                self.cwd = self.virt(path)
                self.reply(250, f"Directory changed to {self.cwd}")
            else:
                self.reply(550, "No such directory")
        elif cmd == "CDUP":
            return self.dispatch("CWD", "..")
        elif cmd == "PASV":
            self.open_data()
        elif cmd in ("PORT", "EPRT", "EPSV"):
            self.reply(500, "This test server speaks passive mode only")
        elif cmd in ("LIST", "NLST", "MLSD", "RETR", "STOR"):
            self.transfer(cmd, arg)
        elif cmd == "MLST":
            self.mlst(arg)
        elif cmd == "SIZE":
            path = self.real(arg)
            if path and os.path.isfile(path):
                self.reply(213, str(os.path.getsize(path)))
            else:
                self.reply(550, "No such file")
        elif cmd == "MDTM":
            path = self.real(arg)
            if path and os.path.exists(path):
                self.reply(213, time.strftime("%Y%m%d%H%M%S", time.gmtime(os.path.getmtime(path))))
            else:
                self.reply(550, "No such file")
        elif cmd == "MKD":
            path = self.real(arg)
            if not path:
                self.reply(550, "Refused")
            elif os.path.isdir(path):
                self.reply(550, "Already there")
            else:
                os.makedirs(path)
                self.reply(257, f'"{self.virt(path)}" created')
        elif cmd == "RMD":
            path = self.real(arg)
            try:
                if path is None:
                    raise OSError("outside the root")
                os.rmdir(path)
                self.reply(250, "Directory removed")
            except OSError:
                self.reply(550, "Cannot remove")
        elif cmd == "DELE":
            path = self.real(arg)
            try:
                if path is None:
                    raise OSError("outside the root")
                os.remove(path)
                self.reply(250, "File removed")
            except OSError:
                self.reply(550, "Cannot remove")
        elif cmd == "RNFR":
            path = self.real(arg)
            if path and os.path.exists(path):
                self.rename_from = path
                self.reply(350, f"Rename from {self.virt(path)}")
            else:
                self.reply(550, "No such file")
        elif cmd == "RNTO":
            path = self.real(arg)
            if self.rename_from and path and not os.path.exists(path):
                os.rename(self.rename_from, path)
                self.rename_from = None
                self.reply(250, "Renamed")
            else:
                self.rename_from = None
                self.reply(550, "Rename failed")
        elif cmd == "SITE" and arg.upper().startswith("CHMOD"):
            parts = arg.split()
            path = self.real(parts[2]) if len(parts) == 3 else None
            try:
                if path is None:
                    raise OSError("outside the root")
                os.chmod(path, int(parts[1], 8))
                self.reply(200, "Mode changed")
            except (OSError, ValueError):
                self.reply(550, "Cannot change mode")
        elif cmd == "AUTH" and arg.upper() == "TLS":
            if not self.tls_ctx:
                self.reply(431, "No TLS here")
            else:
                self.reply(234, "Authentication accepted")
                self.ctl = self.tls_ctx.wrap_socket(self.ctl, server_side=True)
                self.buf = b""
                self.tls = True
        elif cmd == "PBSZ":
            self.reply(200, "0")
        elif cmd == "PROT":
            if arg.upper() in ("P", "C"):
                self.reply(200, f"Protection set to {arg.upper()}")
            else:
                self.reply(504, "Unknown protection")
        elif cmd == "QUIT":
            self.reply(221, "Goodbye")
            return False
        else:
            self.reply(502, "Command not implemented")
        return True

    def mlst(self, arg):
        path = self.real(arg)
        if not path or not os.path.exists(path):
            self.reply(550, "No such file")
            return
        st = os.stat(path)
        # RFC 3659 indents the entry line with a space and puts it between the
        # opening and closing reply lines, which is also what makes libcurl hand
        # it to the write callback instead of eating it as the reply itself.
        self.send(f"250-Listing follows\r\n {mlst_facts(st)} {self.virt(path)}\r\n250 End.\r\n")

    def transfer(self, cmd, arg):
        path = self.real(arg) if arg else self.real(".")
        if not path or (cmd == "RETR" and not os.path.isfile(path)):
            self.reply(550, "No such file")
            # A transfer that never starts still has to answer, or the client
            # waits for a data connection that is never coming.
            return
        try:
            data = self.accept_data()
        except OSError:
            self.reply(425, "Cannot open data connection")
            return
        self.reply(150, f"Opening data connection for {arg or ''}")
        ok = True
        try:
            if cmd in ("LIST", "MLSD", "NLST"):
                body = self.listing(path, ls_line if cmd == "LIST" else mlsd_line)
                if cmd == "NLST":
                    body = "".join(n + "\r\n" for n in self.names(path))
                data.sendall(body.encode())
            elif cmd == "RETR":
                with open(path, "rb") as f:
                    while True:
                        chunk = f.read(65536)
                        if not chunk:
                            break
                        data.sendall(chunk)
            elif cmd == "STOR":
                with open(path, "wb") as f:
                    while True:
                        chunk = data.recv(65536)
                        if not chunk:
                            break
                        f.write(chunk)
        except OSError:
            ok = False
        finally:
            self.close_data(data)
        self.reply(226 if ok else 426, "Transfer complete" if ok else "Transfer aborted")

    @staticmethod
    def names(path):
        try:
            return sorted(os.listdir(path))
        except OSError:
            return []

    @staticmethod
    def listing(path, fmt):
        out = []
        for name in sorted(Session.names(path)):
            try:
                st = os.stat(os.path.join(path, name))
            except OSError:
                continue
            out.append(fmt(name, st) + "\r\n")
        return "".join(out)


def main():
    ap = argparse.ArgumentParser(description="FTP fixture for the remote tests")
    ap.add_argument("--root", required=True, help="directory the whole session is confined to")
    ap.add_argument("--port", type=int, default=0, help="0 picks a free port")
    ap.add_argument("--user", default="tester")
    ap.add_argument("--password", default="hunter2")
    ap.add_argument("--cert", help="serve AUTH TLS with this certificate")
    ap.add_argument("--key", help="private key for --cert")
    args = ap.parse_args()

    root = os.path.realpath(args.root)
    if not os.path.isdir(root):
        os.makedirs(root)

    tls_ctx = None
    if args.cert:
        tls_ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        tls_ctx.load_cert_chain(args.cert, args.key)

    srv = socket.socket()
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("127.0.0.1", args.port))
    srv.listen(8)
    port = srv.getsockname()[1]

    print(json.dumps({"port": port, "root": root}), flush=True)

    def session(ctl):
        try:
            Session(ctl, root, args.user, args.password, tls_ctx).serve()
        except (OSError, ssl.SSLError):
            pass
        finally:
            with contextlib.suppress(OSError):
                ctl.close()

    try:
        while True:
            ctl, _ = srv.accept()
            ctl.settimeout(30)
            # One thread per session. The client under test opens a second control
            # connection while a transfer is already running, and a session that
            # held the accept loop would make that overlap untestable.
            threading.Thread(target=session, args=(ctl,), daemon=True).start()
    except KeyboardInterrupt:
        pass
    finally:
        srv.close()
        if os.environ.get("FTP_SERVER_CLEAN_ROOT") == "1":
            shutil.rmtree(root, ignore_errors=True)


if __name__ == "__main__":
    main()

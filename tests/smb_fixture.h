#ifndef MC_SMB_FIXTURE_H
#define MC_SMB_FIXTURE_H

// A real Samba server for the SMB tests, run as the current user on 127.0.0.1 and
// a free port, over two shares this fixture makes and removes:
//   pub   guest may read it
//   priv  only the current user, with the password kSmbPass
// Needs smbd and pdbedit (the samba package). Without them start() is false and
// the caller skips its SMB checks, as the FTP tests skip without python3.
//
// smbd runs unprivileged here, so it cannot become another user: a guest is
// mapped to the current user but still gets read access only. That is the test
// server's limit, not the client's - the writes are tested with the login.

#ifndef _WIN32

#include <signal.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <sys/wait.h>
#include <unistd.h>
#include <cstdio>
#include <cstdlib>
#include <string>

static const char* const kSmbPass = "pw-for-tests";

struct SmbFixture {
    pid_t pid = -1;
    int port = 0;
    std::string dir;   // short on purpose: smbd's sockets live under it
    std::string user;
    std::string why;   // what stopped start(), for the skip message
    std::string pub() const { return dir + "/pub"; }
    std::string priv() const { return dir + "/priv"; }
    std::string url() const { return "smb://127.0.0.1:" + std::to_string(port); }

    static int free_port() {
        int s = socket(AF_INET, SOCK_STREAM, 0);
        if (s < 0) return 0;
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        socklen_t len = sizeof a;
        int p = 0;
        if (bind(s, (sockaddr*)&a, sizeof a) == 0 && getsockname(s, (sockaddr*)&a, &len) == 0)
            p = ntohs(a.sin_port);
        close(s);
        return p;
    }

    static bool port_open(int p) {
        int s = socket(AF_INET, SOCK_STREAM, 0);
        if (s < 0) return false;
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        a.sin_port = htons((uint16_t)p);
        bool ok = connect(s, (sockaddr*)&a, sizeof a) == 0;
        close(s);
        return ok;
    }

    bool start() {
        if (system("command -v smbd >/dev/null 2>&1 && command -v pdbedit >/dev/null 2>&1") != 0)
            return why = "smbd or pdbedit is not installed", false;
        // Not test_tmp_dir()'s longer names: a unix socket path has ~100 bytes.
        char tmpl[] = "/tmp/immc-smb-XXXXXX";
        if (!mkdtemp(tmpl)) return why = "mkdtemp failed", false;
        dir = tmpl;
        const char* u = getenv("USER");
        user = u && *u ? u : "";
        if (user.empty()) {
            FILE* p = popen("id -un", "r");
            char b[256] = "";
            if (p) { if (fgets(b, sizeof b, p)) {} pclose(p); }
            user = b;
            while (!user.empty() && (user.back() == '\n' || user.back() == '\r')) user.pop_back();
        }
        port = free_port();
        if (!port || user.empty()) return why = "no free port or no user name", false;
        std::string conf = dir + "/smb.conf";
        std::string mk = "mkdir -p '" + dir + "'/{pub/sub,priv,lock,state,cache,pid,private,ncalrpc,log}";
        if (system(("bash -c \"" + mk + "\"").c_str()) != 0) return why = "mkdir failed", false;
        FILE* f = fopen(conf.c_str(), "w");
        if (!f) return why = "cannot write smb.conf", false;
        fprintf(f,
            "[global]\n"
            "  workgroup = TESTGRP\n"
            "  server role = standalone server\n"
            "  smb ports = %d\n"
            "  interfaces = 127.0.0.1\n"
            "  bind interfaces only = yes\n"
            "  lock directory = %s/lock\n"
            "  state directory = %s/state\n"
            "  cache directory = %s/cache\n"
            "  pid directory = %s/pid\n"
            "  private dir = %s/private\n"
            "  ncalrpc dir = %s/ncalrpc\n"
            "  log file = %s/log/log.%%m\n"
            "  passdb backend = tdbsam:%s/private/passdb.tdb\n"
            "  map to guest = Bad User\n"
            "  guest account = %s\n"
            "  load printers = no\n"
            "  printing = bsd\n"
            "  printcap name = /dev/null\n"
            "  disable spoolss = yes\n"
            "  server min protocol = SMB2\n"
            "[pub]\n"
            "  path = %s/pub\n"
            "  guest ok = yes\n"
            "  read only = no\n"
            "[priv]\n"
            "  path = %s/priv\n"
            "  valid users = %s\n"
            "  read only = no\n",
            port, dir.c_str(), dir.c_str(), dir.c_str(), dir.c_str(), dir.c_str(), dir.c_str(),
            dir.c_str(), dir.c_str(), user.c_str(), dir.c_str(), dir.c_str(), user.c_str());
        fclose(f);
        std::string add = "printf '%s\\n%s\\n' '" + std::string(kSmbPass) + "' '" + kSmbPass +
                          "' | pdbedit --configfile='" + conf + "' -a -t -u '" + user +
                          "' >/dev/null 2>&1";
        if (system(add.c_str()) != 0) { remove_tree(); return why = "pdbedit could not add the user", false; }

        pid = fork();
        if (pid < 0) { remove_tree(); return why = "fork failed", false; }
        if (pid == 0) {
            // Its own process group: smbd signals its group when it shuts down, and
            // that must not be the test.
            setpgid(0, 0);
            // stdin from /dev/null: smbd takes a socket on fd 0 to mean it was
            // started by inetd for one connection, and a test may have one there.
            if (!freopen("/dev/null", "r", stdin)) _exit(127);
            // Its own words go to a file in the fixture, read back if it dies.
            if (!freopen((dir + "/log/smbd.out").c_str(), "w", stdout)) _exit(127);
            if (!dup2(fileno(stdout), 2)) {}
            execlp("smbd", "smbd", ("--configfile=" + conf).c_str(), "--foreground",
                   "--no-process-group", "--debug-stdout", "-d", "0", (char*)nullptr);
            _exit(127);
        }
        // The first start after boot can be slow: smbd also starts its RPC helpers.
        for (int i = 0; i < 300; i++) {
            if (port_open(port)) return true;
            int st;
            if (waitpid(pid, &st, WNOHANG) == pid) {
                pid = -1;
                why = "smbd exited at start";
                if (FILE* lf = fopen((dir + "/log/smbd.out").c_str(), "r")) {
                    char line[512];
                    std::string last;
                    while (fgets(line, sizeof line, lf)) if (line[0] != ' ' && line[0] != '\n') last = line;
                    fclose(lf);
                    while (!last.empty() && last.back() == '\n') last.pop_back();
                    if (!last.empty()) why += ": " + last;
                }
                break;
            }
            usleep(50000);
        }
        if (why.empty()) why = "smbd did not listen within 15 s";
        stop();
        return false;
    }

    void remove_tree() {
        if (!dir.empty() && system(("rm -rf '" + dir + "'").c_str()) != 0) {}
    }

    void stop() {
        if (pid > 0) {
            kill(-pid, SIGTERM);                 // smbd and the children it forked
            int st;
            bool gone = false;
            for (int i = 0; i < 50 && !gone; i++) {
                gone = waitpid(pid, &st, WNOHANG) == pid;
                if (!gone) usleep(50000);
            }
            if (!gone) { kill(-pid, SIGKILL); waitpid(pid, &st, 0); }
            pid = -1;
        }
        // smbd starts samba-dcerpcd and its RPC workers, which leave the process
        // group; they are found by the config file they were started with. "[/]"
        // so the pattern never matches the shell that runs pkill.
        if (!dir.empty() &&
            system(("pkill -f -- '--configfile=[/]" + dir.substr(1) + "/smb.conf' 2>/dev/null").c_str()) != 0) {}
        remove_tree();
    }
};

#endif // _WIN32
#endif // MC_SMB_FIXTURE_H

#define _GNU_SOURCE

#include <arpa/inet.h>
#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "config.h"

#define ANDROID_ABSTRACT "localabstract:scrcpy_" SCRCPY_SCID
#define SCRCPY_CMD "CLASSPATH=" SCRCPY_REMOTE " app_process / com.genymobile.scrcpy.Server " SCRCPY_VER " scid=" SCRCPY_SCID " video=false audio=false control=true tunnel_forward=true send_device_meta=false send_dummy_byte=false cleanup=false"

enum {
	MSG_CLIPBOARD,
	MSG_ACK_CLIPBOARD,
	MSG_SET_CLIPBOARD = 9
};

static void usage(void);
static void sighandler(int sig);
static void reapchildren(void);
static uint64_t monoms(void);
static int min(int a, int b);
static uint64_t fnv1a(const char *s, size_t len);
static int writeall(int fd, const void *buf, size_t len);
static int readexact(int fd, void *buf, size_t len);
static pid_t spawn(char *const argv[]);
static int runcmd(char *const argv[]);
static void killwait(pid_t *pid);
static void startpaste(void);
static void spawnscrcpy(void);
static int probedevice(const char *ip, int port);
static int ensureadb(void);
static void disconnectandroid(void);
static int connectandroid(void);
static int sendandroid(const char *text, size_t len);
static void wlcopy(const char *data, size_t len);
static int runpush(const char *sockpath);
static void handlepush(int srvfd);
static void handleandroid(void);
static int rundaemon(const char *sockpath);

static volatile sig_atomic_t running = 1;
static volatile sig_atomic_t sigchld;
static pid_t pastepid = -1;
static pid_t scrcpypid = -1;
static uint64_t androidretry;
static uint64_t lasthash;
static uint64_t pasteretry;
static int androidfd = -1;
static int backoff = BACKOFF_INIT;
static int scrcpypushed;
static int scrcpytries;
static int targetport = DEFAULT_PORT;
static char targetip[64] = DEFAULT_HOST;
static char *argv0 = "clip-sync";
static char recvbuf[CLIP_MAX + 1];

static void
usage(void)
{
	fprintf(stderr, "usage: %s [-p] [-v] [ip[:port]]\n", argv0);
	exit(1);
}

static void
sighandler(int sig)
{
	if (sig == SIGCHLD)
		sigchld = 1;
	else
		running = 0;
}

static void
reapchildren(void)
{
	int st;
	pid_t ch;

	while ((ch = waitpid(-1, &st, WNOHANG)) > 0) {
		if (ch == pastepid) {
			pastepid = -1;
			pasteretry = monoms() + BACKOFF_INIT;
		} else if (ch == scrcpypid) {
			scrcpypid = -1;
			disconnectandroid();
		}
	}
}

static uint64_t
monoms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

static int
min(int a, int b)
{
	return (a < b) ? a : b;
}

static uint64_t
fnv1a(const char *s, size_t len)
{
	uint64_t h;
	size_t i;

	h = 14695981039346656037ULL;
	for (i = 0; i < len; i++) {
		h ^= (uint8_t)s[i];
		h *= 1099511628211ULL;
	}
	return h;
}

static int
writeall(int fd, const void *buf, size_t len)
{
	const char *p;
	ssize_t n;

	p = buf;
	while (len) {
		if ((n = write(fd, p, len)) <= 0) {
			if (n < 0 && errno == EINTR)
				continue;
			return -1;
		}
		p += n;
		len -= (size_t)n;
	}
	return 0;
}

static int
readexact(int fd, void *buf, size_t len)
{
	char *p;
	ssize_t n;

	p = buf;
	while (len) {
		if ((n = read(fd, p, len)) <= 0) {
			if (n < 0 && errno == EINTR)
				continue;
			return -1;
		}
		p += n;
		len -= (size_t)n;
	}
	return 0;
}

static pid_t
spawn(char *const argv[])
{
	int devnull;
	pid_t pid;

	if (!(pid = fork())) {
		signal(SIGPIPE, SIG_DFL);
		if ((devnull = open("/dev/null", O_RDWR)) >= 0) {
			dup2(devnull, 0);
			dup2(devnull, 1);
			dup2(devnull, 2);
			if (devnull > 2)
				close(devnull);
		}
		execvp(argv[0], argv);
		_exit(127);
	}
	return pid;
}

static int
runcmd(char *const argv[])
{
	int status;
	pid_t pid;

	if ((pid = spawn(argv)) < 0)
		return -1;
	while (waitpid(pid, &status, 0) < 0)
		if (errno != EINTR)
			return -1;
	return (WIFEXITED(status) && !WEXITSTATUS(status)) ? 0 : -1;
}

static void
killwait(pid_t *pid)
{
	int i, st;

	if (*pid <= 0)
		return;
	kill(*pid, SIGTERM);
	for (i = 0; i < 10; i++) {
		st = waitpid(*pid, NULL, WNOHANG);
		if (st > 0 || (st < 0 && errno == ECHILD)) {
			*pid = -1;
			return;
		}
		poll(NULL, 0, 10);
	}
	kill(*pid, SIGKILL);
	while (waitpid(*pid, NULL, 0) < 0)
		if (errno != EINTR)
			break;
	*pid = -1;
}

static void
startpaste(void)
{
	char *args[7];

	if (pastepid > 0)
		return;
	args[0] = "wl-paste";
	args[1] = "-t";
	args[2] = "text";
	args[3] = "-w";
	args[4] = argv0;
	args[5] = "-p";
	args[6] = NULL;
	pastepid = spawn(args);
}

static void
spawnscrcpy(void)
{
	char *args[] = { "adb", "shell", SCRCPY_CMD, NULL };

	killwait(&scrcpypid);
	scrcpypid = spawn(args);
}

static int
probedevice(const char *ip, int port)
{
	struct sockaddr_in addr;
	struct pollfd pfd;
	socklen_t len;
	int err, fd, ret = -1;

	if ((fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0)) < 0)
		return -1;
	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_port = htons((uint16_t)port);
	if (inet_pton(AF_INET, ip, &addr.sin_addr) <= 0)
		goto closefd;
	if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		if (errno != EINPROGRESS)
			goto closefd;
		pfd.fd = fd;
		pfd.events = POLLOUT;
		err = 0;
		len = sizeof(err);
		if (poll(&pfd, 1, PROBE_TIMEOUT) <= 0 || getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len) < 0 || err)
			goto closefd;
	}
	ret = 0;
closefd:
	close(fd);
	return ret;
}

static int
ensureadb(void)
{
	char spec[80];
	char *args[4];

	if (probedevice(targetip, targetport) < 0)
		return -1;
	snprintf(spec, sizeof(spec), "%s:%d", targetip, targetport);
	args[0] = "adb";
	args[1] = "connect";
	args[2] = spec;
	args[3] = NULL;
	return runcmd(args);
}

static void
disconnectandroid(void)
{
	if (androidfd >= 0) {
		close(androidfd);
		androidfd = -1;
	}
	killwait(&scrcpypid);
	scrcpytries = 0;
	lasthash = 0;
	androidretry = monoms() + (uint64_t)backoff;
	backoff = min(backoff * 2, BACKOFF_MAX);
}

static int
connectandroid(void)
{
	struct sockaddr_in addr;
	struct timeval tv;
	struct pollfd pfd;
	char *fwdargs[] = { "adb", "forward", ANDROID_FORWARD, ANDROID_ABSTRACT, NULL };
	char *pushargs[] = { "adb", "push", SCRCPY_LOCAL, SCRCPY_REMOTE, NULL };
	char b;
	int s;

	if (scrcpypid <= 0) {
		if (ensureadb() < 0) {
			disconnectandroid();
			return -1;
		}
		if (!scrcpypushed) {
			if (runcmd(pushargs) < 0) {
				disconnectandroid();
				return -1;
			}
			scrcpypushed = 1;
		}
		runcmd(fwdargs);
		spawnscrcpy();
		scrcpytries = 0;
		androidretry = monoms() + 100;
		return -1;
	}
	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_port = htons(ANDROID_PORT);
	addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	if ((s = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0)) < 0) {
		androidretry = monoms() + 100;
		return -1;
	}
	if (connect(s, (struct sockaddr *)&addr, sizeof(addr)) < 0)
		goto closeretry;
	pfd.fd = s;
	pfd.events = POLLIN;
	if (poll(&pfd, 1, HANDSHAKE_TIMEOUT) > 0 && recv(s, &b, 1, MSG_PEEK) <= 0)
		goto closeretry;
	tv.tv_sec = SOCKET_TIMEOUT;
	tv.tv_usec = 0;
	setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
	setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
	androidfd = s;
	backoff = BACKOFF_INIT;
	androidretry = 0;
	scrcpytries = 0;
	return 0;
closeretry:
	close(s);
	if (++scrcpytries >= CONNECT_RETRIES) {
		scrcpypushed = 0;
		disconnectandroid();
		return -1;
	}
	androidretry = monoms() + 100;
	return -1;
}

static int
sendandroid(const char *text, size_t len)
{
	uint8_t hdr[14];
	uint32_t nlen;

	if (androidfd < 0 || !len)
		return 0;
	memset(hdr, 0, sizeof(hdr));
	hdr[0] = MSG_SET_CLIPBOARD;
	nlen = htonl((uint32_t)len);
	memcpy(hdr + 10, &nlen, sizeof(nlen));
	if (writeall(androidfd, hdr, sizeof(hdr)) < 0 || writeall(androidfd, text, len) < 0)
		return -1;
	return 0;
}

static void
wlcopy(const char *data, size_t len)
{
	int p[2];
	pid_t pid;

	if (pipe2(p, O_CLOEXEC) < 0)
		return;
	if (!(pid = fork())) {
		signal(SIGPIPE, SIG_DFL);
		dup2(p[0], 0);
		execlp("wl-copy", "wl-copy", (char *)NULL);
		_exit(127);
	}
	close(p[0]);
	if (pid > 0)
		writeall(p[1], data, len);
	close(p[1]);
}

static int
runpush(const char *sockpath)
{
	struct sockaddr_un addr;
	char buf[8192];
	ssize_t n;
	int fd, ret = 0;

	if ((fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0)) < 0)
		return -1;
	memset(&addr, 0, sizeof(addr));
	addr.sun_family = AF_UNIX;
	snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", sockpath);
	if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		close(fd);
		return -1;
	}
	for (;;) {
		if ((n = read(0, buf, sizeof(buf))) <= 0) {
			if (n < 0 && errno == EINTR)
				continue;
			if (n < 0)
				ret = -1;
			break;
		}
		if (writeall(fd, buf, (size_t)n) < 0) {
			ret = -1;
			break;
		}
	}
	close(fd);
	return ret;
}

static void
handlepush(int srvfd)
{
	struct timeval tv;
	uint64_t h;
	size_t len;
	ssize_t n;
	int cfd;

	if ((cfd = accept4(srvfd, NULL, NULL, SOCK_CLOEXEC)) < 0)
		return;
	tv.tv_sec = PUSH_TIMEOUT;
	tv.tv_usec = 0;
	setsockopt(cfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
	len = 0;
	while (len < sizeof(recvbuf)) {
		if ((n = read(cfd, recvbuf + len, sizeof(recvbuf) - len)) <= 0) {
			if (n < 0 && errno == EINTR)
				continue;
			break;
		}
		len += (size_t)n;
	}
	close(cfd);
	if (len && len <= CLIP_MAX) {
		h = fnv1a(recvbuf, len);
		if (h != lasthash && androidfd >= 0) {
			if (sendandroid(recvbuf, len) < 0)
				disconnectandroid();
			else
				lasthash = h;
		}
	}
}

static void
handleandroid(void)
{
	uint64_t ack, h;
	uint32_t netlen, rlen;
	uint8_t type;

	if (readexact(androidfd, &type, sizeof(type)) < 0)
		goto disconnect;
	switch (type) {
	case MSG_CLIPBOARD:
		if (readexact(androidfd, &netlen, sizeof(netlen)) < 0)
			goto disconnect;
		rlen = ntohl(netlen);
		if (rlen > CLIP_MAX)
			goto disconnect;
		if (readexact(androidfd, recvbuf, rlen) < 0)
			goto disconnect;
		h = fnv1a(recvbuf, rlen);
		if (h != lasthash) {
			lasthash = h;
			wlcopy(recvbuf, rlen);
		}
		break;
	case MSG_ACK_CLIPBOARD:
		if (readexact(androidfd, &ack, sizeof(ack)) < 0)
			goto disconnect;
		break;
	default:
		goto disconnect;
	}
	return;
disconnect:
	disconnectandroid();
}

static int
rundaemon(const char *sockpath)
{
	struct sigaction sa;
	struct sockaddr_un saddr;
	struct pollfd fds[2];
	uint64_t now;
	int nfds, srvfd, t, timeout;

	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = SIG_IGN;
	sigaction(SIGPIPE, &sa, NULL);

	umask(0077);

	sa.sa_handler = sighandler;
	sigaction(SIGCHLD, &sa, NULL);
	sigaction(SIGTERM, &sa, NULL);
	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGHUP, &sa, NULL);

	unlink(sockpath);
	if ((srvfd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0)) < 0)
		err(1, "socket");
	memset(&saddr, 0, sizeof(saddr));
	saddr.sun_family = AF_UNIX;
	snprintf(saddr.sun_path, sizeof(saddr.sun_path), "%s", sockpath);
	if (bind(srvfd, (struct sockaddr *)&saddr, sizeof(saddr)) < 0)
		err(1, "bind");
	if (listen(srvfd, LISTEN_BACKLOG) < 0)
		err(1, "listen");

	backoff = BACKOFF_INIT;
	startpaste();
	connectandroid();

	fds[0].fd = srvfd;
	fds[0].events = POLLIN;

	for (;;) {
		if (sigchld) {
			sigchld = 0;
			reapchildren();
		}
		if (!running)
			break;

		now = monoms();
		if (pastepid < 0 && now >= pasteretry)
			startpaste();
		if (androidfd < 0 && now >= androidretry) {
			connectandroid();
			now = monoms();
		}

		nfds = 1;
		if (androidfd >= 0) {
			fds[1].fd = androidfd;
			fds[1].events = POLLIN;
			nfds = 2;
		}

		timeout = -1;
		if (androidfd < 0)
			timeout = (now >= androidretry) ? 0 : (int)(androidretry - now);
		if (pastepid < 0) {
			t = (now >= pasteretry) ? 0 : (int)(pasteretry - now);
			timeout = (timeout < 0) ? t : min(timeout, t);
		}

		if (poll(fds, nfds, timeout) < 0) {
			if (errno == EINTR)
				continue;
			break;
		}

		if (fds[0].revents & POLLIN)
			handlepush(srvfd);
		if (androidfd >= 0 && (fds[1].revents & (POLLIN | POLLHUP | POLLERR)))
			handleandroid();
	}

	killwait(&pastepid);
	disconnectandroid();
	close(srvfd);
	unlink(sockpath);
	return 0;
}

int
main(int argc, char *argv[])
{
	struct sockaddr_un un;
	const char *runtimedir;
	char *colon, *end;
	char sockpath[sizeof(un.sun_path)];
	long val;
	int ch, n, push = 0;

	if (argc && argv[0])
		argv0 = argv[0];
	while ((ch = getopt(argc, argv, "pv")) != -1) {
		switch (ch) {
		case 'p':
			push = 1;
			break;
		case 'v':
			puts("clip-sync-" VERSION);
			return 0;
		default:
			usage();
		}
	}
	argc -= optind;
	argv += optind;

	runtimedir = getenv("XDG_RUNTIME_DIR");
	if (runtimedir && *runtimedir)
		n = snprintf(sockpath, sizeof(sockpath), "%s/%s", runtimedir, SOCK_NAME);
	else
		n = snprintf(sockpath, sizeof(sockpath), FALLBACK_SOCK, (unsigned int)getuid());
	if (n < 0 || (size_t)n >= sizeof(sockpath))
		errx(1, "socket path too long");

	if (push) {
		if (argc)
			usage();
		return (runpush(sockpath) < 0) ? 1 : 0;
	}
	if (argc > 1)
		usage();

	if (argc == 1) {
		n = snprintf(targetip, sizeof(targetip), "%s", argv[0]);
		if (n < 0 || (size_t)n >= sizeof(targetip))
			usage();
		if ((colon = strchr(targetip, ':'))) {
			*colon = '\0';
			errno = 0;
			val = strtol(colon + 1, &end, 10);
			if (errno || end == colon + 1 || *end || val <= 0 || val > 65535)
				usage();
			targetport = (int)val;
		}
	}

	return rundaemon(sockpath);
}

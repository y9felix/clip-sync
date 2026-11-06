#define _GNU_SOURCE

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
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

enum {
	MSG_CLIPBOARD,
	MSG_ACK_CLIPBOARD,
	MSG_SET_CLIPBOARD = 9
};

static void die(const char *fmt, ...);
static void usage(void);
static void sighandler(int sig);
static void reap_children(void);
static uint64_t monoms(void);
static int min(int a, int b);
static uint64_t fnv1a(const char *s, size_t len);
static int write_all(int fd, const void *buf, size_t len);
static int read_exact(int fd, void *buf, size_t len);
static pid_t spawn(char *const argv[]);
static int run_cmd(char *const argv[]);
static void kill_wait(pid_t *pid);
static void start_wl_paste(void);
static void spawn_scrcpy(void);
static int probe_device(const char *ip, int port);
static int ensure_adb(const char *target);
static void disconnect_android(void);
static int connect_android(const char *target);
static int send_to_android(const char *text, size_t len);
static void wl_copy(const char *data, size_t len);
static int run_push(const char *sock_path);
static void handle_push(int srv_fd);
static void handle_android(void);

static volatile sig_atomic_t running = 1;
static volatile sig_atomic_t sigchld_flag;
static pid_t scrcpy_pid = -1;
static pid_t wl_paste_pid = -1;
static uint64_t last_hash;
static uint64_t wl_paste_retry;
static uint64_t android_retry;
static int android_fd = -1;
static int android_backoff;
static int scrcpy_pushed;
static int scrcpy_tries;
static char *argv0;
static char recv_buf[CLIP_MAX + 1];

static void
die(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	if (fmt[0] && fmt[strlen(fmt) - 1] == ':') {
		fputc(' ', stderr);
		perror(NULL);
	} else {
		fputc('\n', stderr);
	}
	exit(1);
}

static void
usage(void)
{
	fprintf(stderr, "usage: %s [-v] [-p] [ip[:port]]\n", argv0);
	exit(1);
}

static void
sighandler(int sig)
{
	if (sig == SIGCHLD)
		sigchld_flag = 1;
	else
		running = 0;
}

static void
reap_children(void)
{
	int st;
	pid_t ch;

	while ((ch = waitpid(-1, &st, WNOHANG)) > 0) {
		if (ch == wl_paste_pid) {
			wl_paste_pid = -1;
			wl_paste_retry = monoms() + backoff_init;
		} else if (ch == scrcpy_pid) {
			scrcpy_pid = -1;
		}
	}
}

static uint64_t
monoms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
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
write_all(int fd, const void *buf, size_t len)
{
	const char *p;
	ssize_t n;

	p = buf;
	while (len > 0) {
		if ((n = write(fd, p, len)) <= 0) {
			if (n < 0 && errno == EINTR)
				continue;
			return -1;
		}
		p += n;
		len -= n;
	}
	return 0;
}

static int
read_exact(int fd, void *buf, size_t len)
{
	char *p;
	ssize_t n;

	p = buf;
	while (len > 0) {
		if ((n = read(fd, p, len)) <= 0) {
			if (n < 0 && errno == EINTR)
				continue;
			return -1;
		}
		p += n;
		len -= n;
	}
	return 0;
}

static pid_t
spawn(char *const argv[])
{
	int devnull;
	pid_t pid;

	if ((pid = fork()) == 0) {
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
run_cmd(char *const argv[])
{
	int status;
	pid_t pid;

	if ((pid = spawn(argv)) < 0)
		return -1;
	while (waitpid(pid, &status, 0) < 0)
		if (errno != EINTR)
			return -1;
	return (WIFEXITED(status) && WEXITSTATUS(status) == 0) ? 0 : -1;
}

static void
kill_wait(pid_t *pid)
{
	int i, st;

	if (*pid <= 0)
		return;
	kill(*pid, SIGTERM);
	for (i = 0; i < 10; i++) {
		if (waitpid(*pid, &st, WNOHANG) > 0) {
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
start_wl_paste(void)
{
	char *args[7];

	args[0] = "wl-paste";
	args[1] = "-t";
	args[2] = "text";
	args[3] = "-w";
	args[4] = argv0;
	args[5] = "-p";
	args[6] = NULL;
	if (wl_paste_pid <= 0)
		wl_paste_pid = spawn(args);
}

static void
spawn_scrcpy(void)
{
	char cmd[512];
	char *args[] = { "adb", "shell", cmd, NULL };

	snprintf(cmd, sizeof(cmd),
	    "CLASSPATH=%s app_process / com.genymobile.scrcpy.Server %s scid=%s"
	    " video=false audio=false control=true tunnel_forward=true"
	    " send_device_meta=false send_dummy_byte=false cleanup=false",
	    scrcpy_remote, scrcpy_ver, scrcpy_scid);
	kill_wait(&scrcpy_pid);
	scrcpy_pid = spawn(args);
}

static int
probe_device(const char *ip, int port)
{
	struct sockaddr_in addr;
	struct pollfd pfd;
	socklen_t len;
	int err, fd;

	if ((fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0)) < 0)
		return -1;
	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_port = htons(port);
	if (inet_pton(AF_INET, ip, &addr.sin_addr) <= 0)
		goto close_fd;
	if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		if (errno != EINPROGRESS)
			goto close_fd;
		pfd.fd = fd;
		pfd.events = POLLOUT;
		err = 0;
		len = sizeof(err);
		if (poll(&pfd, 1, probe_timeout) <= 0 ||
		    getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len) < 0 ||
		    err != 0)
			goto close_fd;
	}
	close(fd);
	return 0;
close_fd:
	close(fd);
	return -1;
}

static int
ensure_adb(const char *target)
{
	char ip[64], spec[128], *colon, *end;
	char *args[] = {"adb", "connect", NULL, NULL};
	long val;
	int port;

	port = default_port;
	snprintf(ip, sizeof(ip), "%s",
	    (target && *target) ? target : default_host);
	if ((colon = strchr(ip, ':'))) {
		*colon = '\0';
		errno = 0;
		val = strtol(colon + 1, &end, 10);
		if (errno == 0 && end != colon + 1 && *end == '\0' &&
		    val > 0 && val <= 65535)
			port = (int)val;
		else
			port = default_port;
	}
	if (probe_device(ip, port) < 0)
		return -1;
	snprintf(spec, sizeof(spec), "%s:%d", ip, port);
	args[2] = spec;
	return run_cmd(args);
}

static void
disconnect_android(void)
{
	if (android_fd >= 0) {
		close(android_fd);
		android_fd = -1;
	}
	kill_wait(&scrcpy_pid);
	scrcpy_tries = 0;
	last_hash = 0;
	android_retry = monoms() + android_backoff;
	android_backoff = min(android_backoff * 2, backoff_max);
}

static int
connect_android(const char *target)
{
	struct sockaddr_in addr;
	struct timeval tv;
	struct pollfd pfd;
	char b, fwd[32], scid[32];
	char *push_args[] = {"adb", "push", (char *)scrcpy_local, (char *)scrcpy_remote, NULL};
	char *fwd_args[] = {"adb", "forward", NULL, NULL, NULL};
	int s;

	if (scrcpy_pid <= 0) {
		if (ensure_adb(target) < 0)
			goto backoff;
		if (!scrcpy_pushed) {
			if (run_cmd(push_args) < 0)
				goto backoff;
			scrcpy_pushed = 1;
		}
		snprintf(fwd, sizeof(fwd), "tcp:%d", android_port);
		snprintf(scid, sizeof(scid), "localabstract:scrcpy_%s",
		    scrcpy_scid);
		fwd_args[2] = fwd;
		fwd_args[3] = scid;
		run_cmd(fwd_args);
		spawn_scrcpy();
		scrcpy_tries = 0;
		android_retry = monoms() + 100;
		return -1;
	}
	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_port = htons(android_port);
	addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	if ((s = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0)) < 0) {
		android_retry = monoms() + 100;
		return -1;
	}
	if (connect(s, (struct sockaddr *)&addr, sizeof(addr)) < 0)
		goto close_retry;
	pfd.fd = s;
	pfd.events = POLLIN;
	if (poll(&pfd, 1, handshake_timeout) > 0 && recv(s, &b, 1, MSG_PEEK) <= 0)
		goto close_retry;
	tv.tv_sec = socket_timeout;
	tv.tv_usec = 0;
	setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
	setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
	android_fd = s;
	android_backoff = backoff_init;
	android_retry = 0;
	scrcpy_tries = 0;
	return 0;
backoff:
	android_retry = monoms() + android_backoff;
	android_backoff = min(android_backoff * 2, backoff_max);
	return -1;
close_retry:
	close(s);
	if (++scrcpy_tries >= connect_retries) {
		scrcpy_pushed = 0;
		disconnect_android();
		return -1;
	}
	android_retry = monoms() + 100;
	return -1;
}

static int
send_to_android(const char *text, size_t len)
{
	uint8_t hdr[14];
	uint32_t nlen;

	if (android_fd < 0 || len == 0)
		return 0;
	hdr[0] = MSG_SET_CLIPBOARD;
	memset(hdr + 1, 0, 9);
	nlen = htonl((uint32_t)len);
	memcpy(hdr + 10, &nlen, 4);
	if (write_all(android_fd, hdr, sizeof(hdr)) < 0 ||
	    write_all(android_fd, text, len) < 0)
		return -1;
	return 0;
}

static void
wl_copy(const char *data, size_t len)
{
	int p[2];
	pid_t pid;

	if (pipe2(p, O_CLOEXEC) < 0)
		return;
	if ((pid = fork()) == 0) {
		signal(SIGPIPE, SIG_DFL);
		dup2(p[0], 0);
		execlp("wl-copy", "wl-copy", (char *)NULL);
		_exit(127);
	}
	close(p[0]);
	if (pid > 0)
		write_all(p[1], data, len);
	close(p[1]);
}

static int
run_push(const char *sock_path)
{
	struct sockaddr_un addr;
	char buf[8192];
	ssize_t n;
	int fd, ret;

	ret = 0;
	if ((fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0)) < 0)
		return -1;
	memset(&addr, 0, sizeof(addr));
	addr.sun_family = AF_UNIX;
	snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", sock_path);
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
		if (write_all(fd, buf, n) < 0) {
			ret = -1;
			break;
		}
	}
	close(fd);
	return ret;
}

static void
handle_push(int srv_fd)
{
	struct timeval tv;
	uint64_t h;
	size_t len;
	ssize_t n;
	int cfd;

	len = 0;
	if ((cfd = accept4(srv_fd, NULL, NULL, SOCK_CLOEXEC)) < 0)
		return;
	tv.tv_sec = push_timeout;
	tv.tv_usec = 0;
	setsockopt(cfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
	while (len < sizeof(recv_buf)) {
		if ((n = read(cfd, recv_buf + len,
		    sizeof(recv_buf) - len)) <= 0) {
			if (n < 0 && errno == EINTR)
				continue;
			break;
		}
		len += n;
	}
	close(cfd);
	if (len > 0 && len <= CLIP_MAX) {
		h = fnv1a(recv_buf, len);
		if (h != last_hash && android_fd >= 0) {
			if (send_to_android(recv_buf, len) < 0)
				disconnect_android();
			else
				last_hash = h;
		}
	}
}

static void
handle_android(void)
{
	uint64_t ack, h;
	uint32_t netlen, rlen;
	uint8_t type;

	if (read_exact(android_fd, &type, 1) < 0)
		goto disconnect;
	switch (type) {
	case MSG_CLIPBOARD:
		if (read_exact(android_fd, &netlen, 4) < 0)
			goto disconnect;
		rlen = ntohl(netlen);
		if (rlen > CLIP_MAX)
			goto disconnect;
		if (read_exact(android_fd, recv_buf, rlen) < 0)
			goto disconnect;
		recv_buf[rlen] = '\0';
		h = fnv1a(recv_buf, rlen);
		if (h != last_hash) {
			last_hash = h;
			wl_copy(recv_buf, rlen);
		}
		break;
	case MSG_ACK_CLIPBOARD:
		if (read_exact(android_fd, &ack, sizeof(ack)) < 0)
			goto disconnect;
		break;
	default:
		goto disconnect;
	}
	return;
disconnect:
	disconnect_android();
}

int
main(int argc, char *argv[])
{
	struct sigaction sa;
	struct sockaddr_un saddr;
	struct pollfd fds[2];
	const char *runtime_dir, *target;
	char sock_path[sizeof(saddr.sun_path)];
	uint64_t now;
	int ch, nfds, push, srv_fd, t, timeout;

	argv0 = argv[0];
	push = 0;
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

	runtime_dir = getenv("XDG_RUNTIME_DIR");
	if (runtime_dir && *runtime_dir)
		snprintf(sock_path, sizeof(sock_path), "%s/%s",
		    runtime_dir, sock_name);
	else
		snprintf(sock_path, sizeof(sock_path),
		    fallback_sock, (unsigned int)getuid());

	if (push) {
		if (argc != 0)
			usage();
		return (run_push(sock_path) < 0) ? 1 : 0;
	}
	if (argc > 1)
		usage();
	target = (argc == 1) ? argv[0] : getenv("CLIP_SYNC_DEVICE");

	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = SIG_IGN;
	sigaction(SIGPIPE, &sa, NULL);

	umask(0077);

	sa.sa_handler = sighandler;
	sigaction(SIGCHLD, &sa, NULL);
	sigaction(SIGTERM, &sa, NULL);
	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGHUP, &sa, NULL);

	unlink(sock_path);
	if ((srv_fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0)) < 0)
		die("socket:");
	fcntl(srv_fd, F_SETFL, O_NONBLOCK);
	memset(&saddr, 0, sizeof(saddr));
	saddr.sun_family = AF_UNIX;
	snprintf(saddr.sun_path, sizeof(saddr.sun_path), "%s", sock_path);
	if (bind(srv_fd, (struct sockaddr *)&saddr, sizeof(saddr)) < 0)
		die("bind:");
	if (listen(srv_fd, listen_backlog) < 0)
		die("listen:");

	android_backoff = backoff_init;
	start_wl_paste();
	connect_android(target);

	fds[0].fd = srv_fd;
	fds[0].events = POLLIN;

	for (;;) {
		if (sigchld_flag) {
			sigchld_flag = 0;
			reap_children();
		}
		if (!running)
			break;

		now = monoms();
		if (wl_paste_pid < 0 && now >= wl_paste_retry)
			start_wl_paste();
		if (android_fd < 0 && now >= android_retry) {
			connect_android(target);
			now = monoms();
		}

		nfds = 1;
		if (android_fd >= 0) {
			fds[1].fd = android_fd;
			fds[1].events = POLLIN;
			nfds = 2;
		}

		timeout = -1;
		if (android_fd < 0)
			timeout = (now >= android_retry) ? 0 :
			    (int)(android_retry - now);
		if (wl_paste_pid < 0) {
			t = (now >= wl_paste_retry) ? 0 :
			    (int)(wl_paste_retry - now);
			timeout = (timeout < 0) ? t : min(timeout, t);
		}

		if (poll(fds, nfds, timeout) < 0) {
			if (errno == EINTR)
				continue;
			break;
		}

		if (fds[0].revents & POLLIN)
			handle_push(srv_fd);
		if (android_fd >= 0 &&
		    (fds[1].revents & (POLLIN | POLLHUP | POLLERR)))
			handle_android();
	}

	kill_wait(&wl_paste_pid);
	disconnect_android();
	close(srv_fd);
	unlink(sock_path);
	return 0;
}

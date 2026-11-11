#define CLIP_MAX          1048576
#define DEFAULT_HOST      "192.168.0.123"
#define DEFAULT_PORT      5555
#define ANDROID_PORT      28888
#define ANDROID_FORWARD   "tcp:28888"
#define SOCK_NAME         "clip-sync.sock"
#define FALLBACK_SOCK     "/tmp/clip-sync-%u.sock"
#define SCRCPY_VER        "5.0.1"
#define SCRCPY_SCID       "00000001"
#define SCRCPY_LOCAL      "/usr/share/scrcpy/scrcpy-server"
#define SCRCPY_REMOTE     "/data/local/tmp/scrcpy-server.jar"

enum {
	BACKOFF_INIT      = 1000,
	BACKOFF_MAX       = 10000,
	PROBE_TIMEOUT     = 150,
	CONNECT_RETRIES   = 20,
	SOCKET_TIMEOUT    = 3,
	PUSH_TIMEOUT      = 1,
	HANDSHAKE_TIMEOUT = 50,
	LISTEN_BACKLOG    = 16
};

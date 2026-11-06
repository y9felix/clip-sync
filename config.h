#define CLIP_MAX (1048576)

static const char *default_host     = "192.168.0.123";
static const int default_port       = 5555;
static const int android_port       = 28888;
static const int backoff_init       = 1000;
static const int backoff_max        = 10000;
static const int probe_timeout      = 150;
static const int connect_retries    = 20;
static const int socket_timeout     = 3;
static const int push_timeout       = 1;
static const int handshake_timeout  = 50;
static const int listen_backlog     = 16;

static const char *sock_name        = "clip-sync.sock";
static const char *fallback_sock    = "/tmp/clip-sync-%u.sock";
static const char *scrcpy_ver       = "5.0.1";
static const char *scrcpy_scid      = "00000001";
static const char *scrcpy_local     = "/usr/share/scrcpy/scrcpy-server";
static const char *scrcpy_remote    = "/data/local/tmp/scrcpy-server.jar";

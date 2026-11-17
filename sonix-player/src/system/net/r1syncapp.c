#include "r1syncapp.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "src/system/core/json.h"

#define R1SYNC_BIN "/usr/bin/r1sync"
#define R1SYNC_ON_CARD ".r1sync/r1sync"
#define R1SYNC_TMP "/tmp/r1sync"
#define R1SYNC_PORT "80"

static pid_t child = -1;
static int active; // read by other threads: __atomic

static void set_active(int on) { __atomic_store_n(&active, on, __ATOMIC_RELEASE); }

bool r1syncapp_active(void) { return __atomic_load_n(&active, __ATOMIC_ACQUIRE) != 0; }
static char card_bin[PATH_MAX];

static bool executable(const char *path) {
	struct stat st;
	return stat(path, &st) == 0 && S_ISREG(st.st_mode) && (st.st_mode & 0111);
}

static bool regular(const char *path) {
	struct stat st;
	return stat(path, &st) == 0 && S_ISREG(st.st_mode) && st.st_size > 0;
}

// The card's copy as an executable in /tmp; false when it cannot be made.
static bool copy_to_tmp(const char *from) {
	int in = open(from, O_RDONLY);
	if (in < 0) {
		return false;
	}
	unlink(R1SYNC_TMP);
	int out = open(R1SYNC_TMP, O_WRONLY | O_CREAT | O_TRUNC, 0755);
	if (out < 0) {
		close(in);
		return false;
	}
	char buf[16384];
	ssize_t n;
	bool ok = true;
	while ((n = read(in, buf, sizeof(buf))) > 0) {
		if (write(out, buf, (size_t)n) != n) {
			ok = false;
			break;
		}
	}
	ok = ok && n == 0;
	close(in);
	ok = close(out) == 0 && ok;
	if (!ok) {
		unlink(R1SYNC_TMP);
	}
	return ok;
}

// An r1sync this player did not start -- left by a player that was killed
// before its page could stop it -- holds port 80, and the new one would fail.
static void stop_leftovers(void) {
	DIR *proc = opendir("/proc");
	struct dirent *de;
	while (proc && (de = readdir(proc)) != NULL) {
		if (de->d_name[0] < '0' || de->d_name[0] > '9') {
			continue;
		}
		char path[300], comm[64] = "";
		snprintf(path, sizeof(path), "/proc/%s/comm", de->d_name);
		FILE *f = fopen(path, "r");
		if (!f) {
			continue;
		}
		if (fgets(comm, sizeof(comm), f)) {
			comm[strcspn(comm, "\r\n")] = '\0';
		}
		fclose(f);
		pid_t pid = (pid_t)atoi(de->d_name);
		if (strcmp(comm, "r1sync") == 0 && pid != child) {
			printf("r1sync: stopping a leftover one (pid %d)\n", (int)pid);
			kill(pid, SIGKILL);
		}
	}
	if (proc) {
		closedir(proc);
	}
}

bool r1syncapp_available(const char *sd_root) {
	if (executable(R1SYNC_BIN)) {
		return true;
	}
	char path[PATH_MAX];
	return sd_root && sd_root[0] && snprintf(path, sizeof(path), "%s/%s", sd_root, R1SYNC_ON_CARD) < (int)sizeof(path) &&
		   regular(path);
}

bool r1syncapp_start(const char *sd_root) {
	if (r1syncapp_running() || !sd_root || !sd_root[0]) {
		return false;
	}
	snprintf(card_bin, sizeof(card_bin), "%s/%s", sd_root, R1SYNC_ON_CARD);

	// The card's build first, as r1sync's own launcher does on HiBy firmware:
	// a newer one copied there needs no reflash.
	const char *bin = NULL;
	if (regular(card_bin) && copy_to_tmp(card_bin)) {
		bin = R1SYNC_TMP;
	} else if (executable(R1SYNC_BIN)) {
		bin = R1SYNC_BIN;
	}
	if (!bin) {
		printf("r1sync: neither %s nor %s\n", R1SYNC_BIN, card_bin);
		return false;
	}

	// r1sync wants the folder holding sd_0: "/mnt" for "/mnt/sd_0".
	char mount[PATH_MAX];
	snprintf(mount, sizeof(mount), "%s", sd_root);
	char *slash = strrchr(mount, '/');
	if (!slash || strcmp(slash, "/sd_0") != 0) {
		printf("r1sync: the card is at %s, not a .../sd_0\n", sd_root);
		return false;
	}
	*slash = '\0';

	// Its log on the card, where r1sync keeps it on HiBy firmware too.
	char log[PATH_MAX];
	snprintf(log, sizeof(log), "%s/.r1sync", sd_root);
	mkdir(log, 0755);
	snprintf(log, sizeof(log), "%s/.r1sync/r1sync.log", sd_root);

	stop_leftovers();
	unlink(R1SYNC_STATUS_FILE);
	pid_t pid = fork();
	if (pid < 0) {
		perror("r1sync: fork");
		return false;
	}
	if (pid == 0) {
		int fd = open(log, O_WRONLY | O_CREAT | O_APPEND, 0644);
		if (fd < 0) {
			fd = open("/tmp/r1sync.log", O_WRONLY | O_CREAT | O_APPEND, 0644);
		}
		if (fd >= 0) {
			dup2(fd, STDOUT_FILENO);
			dup2(fd, STDERR_FILENO);
			close(fd);
		}
		// The player's own signal dispositions are not r1sync's.
		signal(SIGTERM, SIG_DFL);
		signal(SIGINT, SIG_DFL);
		signal(SIGPIPE, SIG_DFL);
		setsid();
		// And it goes with the player, however the player goes.
		prctl(PR_SET_PDEATHSIG, SIGTERM);
		execl(bin, "r1sync", "-s", "-m", mount, "-p", R1SYNC_PORT, "-v", (char *)NULL);
		_exit(127);
	}
	child = pid;
	set_active(1);
	printf("r1sync: started %s (pid %d) for %s\n", bin, (int)pid, sd_root);
	return true;
}

bool r1syncapp_running(void) {
	if (child <= 0) {
		return false;
	}
	int status;
	pid_t r = waitpid(child, &status, WNOHANG);
	if (r == 0) {
		return true;
	}
	if (r == child) {
		printf("r1sync: exited (%s %d)\n", WIFEXITED(status) ? "status" : "signal",
			   WIFEXITED(status) ? WEXITSTATUS(status) : WTERMSIG(status));
	}
	child = -1;
	set_active(0);
	unlink(R1SYNC_TMP);
	return false;
}

void r1syncapp_ask_stop(void) {
	if (r1syncapp_running()) {
		kill(child, SIGTERM);
	}
}

void r1syncapp_kill(void) {
	if (!r1syncapp_running()) {
		return;
	}
	printf("r1sync: did not finish; SIGKILL\n");
	kill(child, SIGKILL);
	waitpid(child, NULL, 0);
	child = -1;
	set_active(0);
	unlink(R1SYNC_TMP);
}

void r1syncapp_stop(void) {
	r1syncapp_ask_stop();
	for (int waited = 0; waited < R1SYNC_STOP_WAIT_MS; waited += 100) {
		if (!r1syncapp_running()) {
			return;
		}
		usleep(100 * 1000);
	}
	r1syncapp_kill();
}

bool r1syncapp_status(r1syncapp_status_t *out) {
	memset(out, 0, sizeof(*out));
	FILE *f = fopen(R1SYNC_STATUS_FILE, "r");
	if (!f) {
		return false;
	}
	char text[1024];
	size_t n = fread(text, 1, sizeof(text) - 1, f);
	fclose(f);
	text[n] = '\0';

	json_doc_t doc;
	if (!json_parse(text, &doc)) {
		return false;
	}
	int root = json_root(&doc);
	json_obj_str(&doc, root, "version", out->version, sizeof(out->version));
	json_obj_str(&doc, root, "ip", out->ip, sizeof(out->ip));
	json_obj_str(&doc, root, "pin", out->pin, sizeof(out->pin));
	json_obj_str(&doc, root, "abs", out->abs, sizeof(out->abs));
	json_obj_str(&doc, root, "nd", out->nd, sizeof(out->nd));
	json_obj_str(&doc, root, "notice", out->notice, sizeof(out->notice));
	json_free(&doc);
	return true;
}

/* $OpenBSD$ */

/*
 * Copyright (c) 2007 Nicholas Marriott <nicholas.marriott@gmail.com>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF MIND, USE, DATA OR PROFITS, WHETHER
 * IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING
 * OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include "tmux.h"

#include <sys/types.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <sys/file.h>

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifdef PLATFORM_WINDOWS
#include <process.h>
#endif

static struct tmuxproc	*client_proc;
static struct tmuxpeer	*client_peer;
static uint64_t		 client_flags;
static int		 client_suspended;
static enum {
	CLIENT_EXIT_NONE,
	CLIENT_EXIT_DETACHED,
	CLIENT_EXIT_DETACHED_HUP,
	CLIENT_EXIT_LOST_TTY,
	CLIENT_EXIT_TERMINATED,
	CLIENT_EXIT_LOST_SERVER,
	CLIENT_EXIT_EXITED,
	CLIENT_EXIT_SERVER_EXITED,
	CLIENT_EXIT_MESSAGE_PROVIDED
} client_exitreason = CLIENT_EXIT_NONE;
static int		 client_exitflag;
static int		 client_exitval;
static enum msgtype	 client_exittype;
static const char	*client_exitsession;
static char		*client_exitmessage;
static const char	*client_execshell;
static const char	*client_execcmd;
static int		 client_attached;
static struct client_files client_files = RB_INITIALIZER(&client_files);

#ifdef PLATFORM_WINDOWS
static DWORD		 client_saved_stdin_mode = 0;
static DWORD		 client_saved_stdout_mode = 0;
static int		 client_console_mode_saved = 0;
#endif

static __dead void	 client_exec(const char *,const char *);

#ifdef PLATFORM_WINDOWS
struct client_resize_thread_arg {
	int	fd;	/* socketpair fd to signal resize */
};

/*
 * Resize monitor thread: polls the console window size every 250ms and
 * notifies the event loop when it changes, so the server can send MSG_RESIZE.
 * Polling avoids concurrency issues with the input thread using ReadFile on
 * the same console handle.
 */
unsigned int __stdcall
client_resize_thread(void *arg)
{
	struct client_resize_thread_arg *a = arg;
	int	fd = a->fd;
	SHORT	last_cols = 0, last_rows = 0;
	CONSOLE_SCREEN_BUFFER_INFO csbi;

	free(a);

	/* Record initial size */
	if (GetConsoleScreenBufferInfo(GetStdHandle(STD_OUTPUT_HANDLE), &csbi)) {
		last_cols = csbi.srWindow.Right - csbi.srWindow.Left + 1;
		last_rows = csbi.srWindow.Bottom - csbi.srWindow.Top + 1;
	}

	win32_log("client_resize_thread: started, fd=%d, initial size=%dx%d\n",
	    fd, (int)last_cols, (int)last_rows);

	while (1) {
		Sleep(250);

		if (!GetConsoleScreenBufferInfo(GetStdHandle(STD_OUTPUT_HANDLE), &csbi))
			continue;

		SHORT cols = csbi.srWindow.Right - csbi.srWindow.Left + 1;
		SHORT rows = csbi.srWindow.Bottom - csbi.srWindow.Top + 1;

		if (cols != last_cols || rows != last_rows) {
			last_cols = cols;
			last_rows = rows;
			win32_log("client_resize_thread: resize to %dx%d, notifying\n",
			    (int)cols, (int)rows);
			char notify = 'R';
			write(fd, &notify, 1);
		}
	}

	win32_log("client_resize_thread: exiting\n");
	return (0);
}

unsigned int __stdcall
client_input_thread(void *arg)
{
	int	fd = (intptr_t)arg;
	HANDLE	hIn = GetStdHandle(STD_INPUT_HANDLE);
	char	buf[1024];
	DWORD	nread;

	win32_log("client_input_thread: started with fd=%d, hIn=%p\n", fd, hIn);

	while (1) {
		if (!ReadFile(hIn, buf, sizeof buf, &nread, NULL)) {
			DWORD err = GetLastError();
			win32_log("client_input_thread: ReadFile failed, error=%lu\n", err);
			break;
		}
		if (nread == 0) {
			win32_log("client_input_thread: ReadFile returned 0 bytes (EOF)\n");
			break;
		}
		win32_log("client_input_thread: read %lu bytes, writing to fd=%d\n", nread, fd);
		ssize_t written = write(fd, buf, nread);
		if (written == -1) {
			win32_log("client_input_thread: write failed, errno=%d\n", errno);
			break;
		}
		win32_log("client_input_thread: wrote %zd bytes to fd=%d\n", written, fd);
	}
	win32_log("client_input_thread: thread exiting\n");
	return (0);
}


static void
client_input_callback(__unused int fd_ignore, __unused short events, void *arg)
{
	/*
	 * NOTE: libevent passes the raw SOCKET handle as fd_ignore, but we need
	 * the mapped fd that we stored in arg to use with win32_read().
	 */
	int	mapped_fd = (int)(intptr_t)arg;
	char	buf[1024];
	ssize_t	n;

	win32_log("client_input_callback: reading from mapped_fd=%d (raw fd was %d)\n", mapped_fd, fd_ignore);
	n = read(mapped_fd, buf, sizeof buf);
	win32_log("client_input_callback: read returned %zd\n", n);
	if (n > 0) {
		win32_log("client_input_callback: sending MSG_TTY_INPUT with %zd bytes\n", n);
		proc_send(client_peer, MSG_TTY_INPUT, -1, buf, n);
		proc_flush_peer(client_peer);
	} else if (n == 0) {
		win32_log("client_input_callback: EOF on input socket\n");
	} else {
		win32_log("client_input_callback: read error, errno=%d\n", errno);
	}
}

static void
client_resize_callback(__unused int fd_ignore, __unused short events, void *arg)
{
	int	mapped_fd = (int)(intptr_t)arg;
	char	buf[32];
	ssize_t	n;

	/* Drain notification bytes sent by client_resize_thread */
	n = read(mapped_fd, buf, sizeof buf);
	if (n > 0) {
		struct winsize		 ws;
		CONSOLE_SCREEN_BUFFER_INFO csbi;

		memset(&ws, 0, sizeof ws);
		if (GetConsoleScreenBufferInfo(GetStdHandle(STD_OUTPUT_HANDLE), &csbi)) {
			ws.ws_col = csbi.srWindow.Right - csbi.srWindow.Left + 1;
			ws.ws_row = csbi.srWindow.Bottom - csbi.srWindow.Top + 1;
			if (ws.ws_col < 10) ws.ws_col = 10;
			if (ws.ws_row < 3)  ws.ws_row = 3;
		} else {
			ws.ws_col = 80;
			ws.ws_row = 24;
		}
		win32_log("client_resize_callback: sending MSG_RESIZE %ux%u\n",
		    ws.ws_col, ws.ws_row);
		proc_send(client_peer, MSG_RESIZE, -1, &ws, sizeof ws);
	}
}
#endif
static int		 client_get_lock(char *);
static int		 client_connect(struct event_base *, const char *,
			     uint64_t);
static void		 client_send_identify(const char *, const char *,
			     char **, u_int, const char *, int);
static void		 client_signal(int);
static void		 client_dispatch(struct imsg *, void *);
static void		 client_dispatch_attached(struct imsg *);
static void		 client_dispatch_wait(struct imsg *);
static const char	*client_exit_message(void);

/*
 * Get server create lock. If already held then server start is happening in
 * another client, so block until the lock is released and return -2 to
 * retry. Return -1 on failure to continue and start the server anyway.
 */
static int
client_get_lock(char *lockfile)
{
	int lockfd;

	log_debug("lock file is %s", lockfile);

	if ((lockfd = open(lockfile, O_WRONLY|O_CREAT, 0600)) == -1) {
		log_debug("open failed: %s", strerror(errno));
		return (-1);
	}

	if (flock(lockfd, LOCK_EX|LOCK_NB) == -1) {
		log_debug("flock failed: %s", strerror(errno));
		if (errno != EAGAIN)
			return (lockfd);
		while (flock(lockfd, LOCK_EX) == -1 && errno == EINTR)
			/* nothing */;
		close(lockfd);
		return (-2);
	}
	log_debug("flock succeeded");

	return (lockfd);
}

/* Connect client to server. */
static int
client_connect(struct event_base *base, const char *path, uint64_t flags)
{
	struct sockaddr_un	sa;
	size_t			size;
	int			fd = -1, lockfd = -1, locked = 0;
#ifdef PLATFORM_WINDOWS
	int			server_started = 0, retry_count = 0;
#endif
	char		       *lockfile = NULL;

#ifdef PLATFORM_WINDOWS
    win32_log("client_connect: entered with path %s\n", path);
#endif

	memset(&sa, 0, sizeof sa);
	sa.sun_family = AF_UNIX;
	size = strlcpy(sa.sun_path, path, sizeof sa.sun_path);
	if (size >= sizeof sa.sun_path) {
		errno = ENAMETOOLONG;
		return (-1);
	}
	log_debug("socket is %s", path);

retry:
#ifdef PLATFORM_WINDOWS
    win32_log("client_connect: creating socket\n");
#endif
	fd = win32_socket(AF_UNIX, SOCK_STREAM, 0);
#ifdef PLATFORM_WINDOWS
    win32_log("client_connect: win32_socket returned fd=%d\n", fd);
#endif
	if (fd == -1) {
#ifdef PLATFORM_WINDOWS
    win32_log("client_connect: win32_socket failed\n");
#endif
		return (-1);
    }

	log_debug("trying connect");
#ifdef PLATFORM_WINDOWS
    win32_log("client_connect: calling win32_connect\n");
#endif
	if (win32_connect(fd, (struct sockaddr *)&sa, sizeof sa) == -1) {
		log_debug("connect failed: %s", strerror(errno));
		if (errno != ECONNREFUSED && errno != ENOENT) {
#ifdef PLATFORM_WINDOWS
    win32_log("client_connect: connect failed errno=%d\n", errno);
#endif
			close(fd);
			return (-1);
		}
#ifdef PLATFORM_WINDOWS
    win32_log("client_connect: connect REFUSED/NOENT, starting server\n");
#endif
		close(fd);
#ifdef PLATFORM_WINDOWS
    win32_log("client_connect: close(fd) done\n");
#endif
		fd = -1;
#ifdef PLATFORM_WINDOWS
    win32_log("client_connect: flags=%llu, CLIENT_STARTSERVER=%llu\n", (unsigned long long)flags, (unsigned long long)CLIENT_STARTSERVER);
#endif

		if (flags & CLIENT_NOSTARTSERVER) {
#ifdef PLATFORM_WINDOWS
            win32_log("client_connect: NOSTARTSERVER set, failing\n");
#endif
			goto failed;
        }
		if (!(flags & CLIENT_STARTSERVER)) {
#ifdef PLATFORM_WINDOWS
            win32_log("client_connect: STARTSERVER NOT set, FORCING it on Windows\n");
            flags |= CLIENT_STARTSERVER;
#else
			goto failed;
#endif
        }
#ifdef PLATFORM_WINDOWS
    win32_log("client_connect: proceeding to lock logic\n");
#endif
		close(fd);

		if (!locked) {
#ifdef PLATFORM_WINDOWS
    win32_log("client_connect: attempting lock\n");
#endif
			xasprintf(&lockfile, "%s.lock", path);
			if ((lockfd = client_get_lock(lockfile)) < 0) {
				log_debug("didn't get lock (%d)", lockfd);
#ifdef PLATFORM_WINDOWS
    win32_log("client_connect: lock failed %d\n", lockfd);
#endif

				free(lockfile);
				lockfile = NULL;

				if (lockfd == -2)
					goto retry;
			}
			log_debug("got lock (%d)", lockfd);
#ifdef PLATFORM_WINDOWS
    win32_log("client_connect: got lock %d\n", lockfd);
#endif

			/*
			 * Always retry at least once, even if we got the lock,
			 * because another client could have taken the lock,
			 * started the server and released the lock between our
			 * connect() and flock().
			 */
			locked = 1;
			goto retry;
		}

		if (lockfd >= 0 && unlink(path) != 0 && errno != ENOENT) {
			free(lockfile);
			close(lockfd);
			return (-1);
		}
#ifdef PLATFORM_WINDOWS
    win32_log("client_connect: calling server_start with lockfd=%d, lockfile=%s\n", lockfd, lockfile ? lockfile : "NULL");
#endif
	if (server_start(client_proc, flags, base, lockfd, lockfile) != 0) {
#ifdef PLATFORM_WINDOWS
        win32_log("client_connect: server_start failed\n");
#endif
        goto failed;
    }
#ifdef PLATFORM_WINDOWS
    server_started = 1;
    retry_count = 0;
    win32_log("client_connect: server_start success, retrying connect\n");
    Sleep(300); /* Give server a moment to bind */
#endif
    goto retry;
	} else {
#ifdef PLATFORM_WINDOWS
        /* connect() succeeded — but if we're in a post-server_start retry loop,
         * the server may have accepted and immediately dropped us (race).
         * Break out: fall through to the success path below. */
        server_started = 0; /* stop retry guard */
#endif
    }
#ifdef PLATFORM_WINDOWS
    /* connect() failed and server was already started — wait and retry */
    if (server_started) {
        retry_count++;
        if (retry_count > 20) {
            win32_log("client_connect: server started but connect still failing after %d retries\n", retry_count);
            goto failed;
        }
        win32_log("client_connect: server started, connect retry %d/20\n", retry_count);
        close(fd);
        fd = -1;
        Sleep(200);
        goto retry;
    }
#endif

	if (locked && lockfd >= 0) {
#ifdef PLATFORM_WINDOWS
    win32_log("client_connect: freeing lockfile and closing lockfd=%d\n", lockfd);
#endif
		free(lockfile);
		close(lockfd);
	}
#ifdef PLATFORM_WINDOWS
    win32_log("client_connect: calling setblocking(%d, 0)\n", fd);
#endif
	setblocking(fd, 0);
#ifdef PLATFORM_WINDOWS
    win32_log("client_connect: returning fd=%d\n", fd);
#endif
	return (fd);

failed:
	if (locked) {
		free(lockfile);
		close(lockfd);
	}
	close(fd);
	return (-1);
}

/* Get exit string from reason number. */
const char *
client_exit_message(void)
{
	static char msg[256];

	switch (client_exitreason) {
	case CLIENT_EXIT_NONE:
		break;
	case CLIENT_EXIT_DETACHED:
		if (client_exitsession != NULL) {
			xsnprintf(msg, sizeof msg, "detached "
			    "(from session %s)", client_exitsession);
			return (msg);
		}
		return ("detached");
	case CLIENT_EXIT_DETACHED_HUP:
		if (client_exitsession != NULL) {
			xsnprintf(msg, sizeof msg, "detached and SIGHUP "
			    "(from session %s)", client_exitsession);
			return (msg);
		}
		return ("detached and SIGHUP");
	case CLIENT_EXIT_LOST_TTY:
		return ("lost tty");
	case CLIENT_EXIT_TERMINATED:
		return ("terminated");
	case CLIENT_EXIT_LOST_SERVER:
		return ("server exited unexpectedly");
	case CLIENT_EXIT_EXITED:
		return ("exited");
	case CLIENT_EXIT_SERVER_EXITED:
		return ("server exited");
	case CLIENT_EXIT_MESSAGE_PROVIDED:
		return (client_exitmessage);
	}
	return ("unknown reason");
}

/* Exit if all streams flushed. */
static void
client_exit(void)
{
	if (!file_write_left(&client_files))
		proc_exit(client_proc);
}

/* Client main loop. */
int
client_main(struct event_base *base, int argc, char **argv, uint64_t flags,
    int feat)
{
	struct cmd_parse_result	*pr;
	struct msg_command	*data;
	int			 fd, i;
	const char		*ttynam, *termname, *cwd;
	pid_t			 ppid;
	enum msgtype		 msg;
	struct termios		 tio, saved_tio;
	size_t			 size, linesize = 0;
	ssize_t			 linelen;
	char			*line = NULL, **caps = NULL, *cause;
	u_int			 ncaps = 0;
	struct args_value	*values;

#ifdef PLATFORM_WINDOWS
    win32_log("client_main: entered\n");
#endif

	/* Set up the initial command. */
	if (shell_command != NULL) {
		msg = MSG_SHELL;
		flags |= CLIENT_STARTSERVER;
	} else if (argc == 0) {
		msg = MSG_COMMAND;
		flags |= CLIENT_STARTSERVER;
	} else {
#ifdef PLATFORM_WINDOWS
    win32_log("client_main: parsing args, argc=%d\n", argc);
#endif
		msg = MSG_COMMAND;

		/*
		 * It's annoying parsing the command string twice (in client
		 * and later in server) but it is necessary to get the start
		 * server flag.
		 */
		values = args_from_vector(argc, argv);
		pr = cmd_parse_from_arguments(values, argc, NULL);
		if (pr->status == CMD_PARSE_SUCCESS) {
			if (cmd_list_any_have(pr->cmdlist, CMD_STARTSERVER))
				flags |= CLIENT_STARTSERVER;
			cmd_list_free(pr->cmdlist);
		} else
			free(pr->error);
		args_free_values(values, argc);
		free(values);
#ifdef PLATFORM_WINDOWS
    win32_log("client_main: args parsed\n");
#endif
	}

	/* Create client process structure (starts logging). */
#ifdef PLATFORM_WINDOWS
    win32_log("client_main: calling proc_start\n");
#endif
	client_proc = proc_start("client");
#ifdef PLATFORM_WINDOWS
    win32_log("client_main: proc_start done\n");
#endif
	proc_set_signals(client_proc, client_signal);

	/* Save the flags. */
	client_flags = flags;
	log_debug("flags are %#llx", (unsigned long long)client_flags);

#ifdef PLATFORM_WINDOWS
    win32_log("client_main: flags saved, connecting\n");
#endif

	/* Initialize the client socket and start the server. */
#ifdef HAVE_SYSTEMD
	if (systemd_activated()) {
		/* socket-based activation, do not even try to be a client. */
		fd = server_start(client_proc, flags, base, 0, NULL);
	} else
#endif
	fd = client_connect(base, socket_path, client_flags);
#ifdef PLATFORM_WINDOWS
    win32_log("client_main: client_connect returned %d, errno=%d\n", fd, errno);
#endif
	if (fd == -1) {
#ifdef PLATFORM_WINDOWS
        win32_log("client_main: connect failed\n");
#endif
		if (errno == ECONNREFUSED) {
			fprintf(stderr, "no server running on %s\n",
			    socket_path);
		} else {
			fprintf(stderr, "error connecting to %s (%s)\n",
			    socket_path, strerror(errno));
		}
		return (1);
	}
#ifdef PLATFORM_WINDOWS
	win32_log("client_main: calling proc_add_peer for fd=%d\n", fd);
#endif
	client_peer = proc_add_peer(client_proc, fd, client_dispatch, NULL);
#ifdef PLATFORM_WINDOWS
	win32_log("client_main: proc_add_peer returned %p\n", client_peer);
#endif

	/* Save these before pledge(). */
	if ((cwd = find_cwd()) == NULL && (cwd = find_home()) == NULL)
		cwd = "/";
	if ((ttynam = ttyname(STDIN_FILENO)) == NULL)
		ttynam = "";
	if ((termname = getenv("TERM")) == NULL)
		termname = "";

#ifdef PLATFORM_WINDOWS
	win32_log("client_main: cwd=%s, ttynam=%s, termname=%s\n", cwd, ttynam, termname);
#endif

	/*
	 * Drop privileges for client. "proc exec" is needed for -c and for
	 * locking (which uses system(3)).
	 *
	 * "tty" is needed to restore termios(4) and also for some reason -CC
	 * does not work properly without it (input is not recognised).
	 *
	 * "sendfd" is dropped later in client_dispatch_wait().
	 */
	if (pledge(
	    "stdio rpath wpath cpath unix sendfd proc exec tty",
	    NULL) != 0)
		fatal("pledge failed");

	/* Load terminfo entry if any. */
	if (isatty(STDIN_FILENO)) {
		if (termname == NULL || *termname == '\0')
#ifdef PLATFORM_WINDOWS
			termname = "xterm-256color";  /* Windows Terminal supports full xterm */
#else
			termname = "screen";
#endif


#ifdef PLATFORM_WINDOWS
		win32_log("client_main: termname=%s\n", termname);
#endif

#ifdef PLATFORM_WINDOWS
		if (ncaps == 0) {
			win32_log("client_main: ncaps=0, injecting fallbacks\n");
			/* Inject capabilities for Windows Terminal (xterm-256color compatible) */
			caps = xreallocarray(NULL, 20, sizeof *caps);
			ncaps = 0;
			xasprintf(&caps[ncaps++], "clear=\033[H\033[2J");
			xasprintf(&caps[ncaps++], "cup=\033[%%i%%p1%%d;%%p2%%dH");
			xasprintf(&caps[ncaps++], "bel=\007");
			xasprintf(&caps[ncaps++], "cols=80");
			xasprintf(&caps[ncaps++], "lines=24");
			xasprintf(&caps[ncaps++], "am=1");
			xasprintf(&caps[ncaps++], "sgr0=\033[m");
			xasprintf(&caps[ncaps++], "bold=\033[1m");
			xasprintf(&caps[ncaps++], "colors=256");
			xasprintf(&caps[ncaps++], "AX=1");
			xasprintf(&caps[ncaps++], "XT=1");
			/* Alternate screen — critical for hiding underlying terminal content */
			xasprintf(&caps[ncaps++], "smcup=\033[?1049h");
			xasprintf(&caps[ncaps++], "rmcup=\033[?1049l");
			/* Cursor visibility */
			xasprintf(&caps[ncaps++], "civis=\033[?25l");
			xasprintf(&caps[ncaps++], "cnorm=\033[?25h");
			/* Reverse video, underline, italic */
			xasprintf(&caps[ncaps++], "rev=\033[7m");
			xasprintf(&caps[ncaps++], "smul=\033[4m");
			xasprintf(&caps[ncaps++], "sitm=\033[3m");
			/* Set foreground/background (256-colour) */
			xasprintf(&caps[ncaps++], "setaf=\033[%%?%%p1%%{8}%%<%%t3%%p1%%d%%e%%p1%%{16}%%<%%t9%%p1%%{8}%%-%%d%%e38;5;%%p1%%d%%m");
			xasprintf(&caps[ncaps++], "setab=\033[%%?%%p1%%{8}%%<%%t4%%p1%%d%%e%%p1%%{16}%%<%%t10%%p1%%{8}%%-%%d%%e48;5;%%p1%%d%%m");
		}
		win32_log("client_main: tty_term_read_list success, ncaps=%u\n", ncaps);
#endif
	}

	/* Free stuff that is not used in the client. */
	if (ptm_fd != -1)
		close(ptm_fd);
	options_free(global_options);
	options_free(global_s_options);
	options_free(global_w_options);
	environ_free(global_environ);

	/* Set up control mode. */
	if (client_flags & CLIENT_CONTROLCONTROL) {
		if (tcgetattr(STDIN_FILENO, &saved_tio) != 0) {
			fprintf(stderr, "tcgetattr failed: %s\n",
			    strerror(errno));
			return (1);
		}
		cfmakeraw(&tio);
		tio.c_iflag = ICRNL|IXANY;
		tio.c_oflag = OPOST|ONLCR;
#ifdef NOKERNINFO
		tio.c_lflag = NOKERNINFO;
#endif
		tio.c_cflag = CREAD|CS8|HUPCL;
		tio.c_cc[VMIN] = 1;
		tio.c_cc[VTIME] = 0;
		cfsetispeed(&tio, cfgetispeed(&saved_tio));
		cfsetospeed(&tio, cfgetospeed(&saved_tio));
		tcsetattr(STDIN_FILENO, TCSANOW, &tio);
	}

	/* Send identify messages. */
	client_send_identify(ttynam, termname, caps, ncaps, cwd, feat);
	tty_term_free_list(caps, ncaps);
	proc_flush_peer(client_peer);

	/* Send first command. */
	if (msg == MSG_COMMAND) {
		/* How big is the command? */
		size = 0;
		for (i = 0; i < argc; i++)
			size += strlen(argv[i]) + 1;
		if (size > MAX_IMSGSIZE - (sizeof *data)) {
			fprintf(stderr, "command too long\n");
			return (1);
		}
		data = xmalloc((sizeof *data) + size);

		/* Prepare command for server. */
		data->argc = argc;
		if (cmd_pack_argv(argc, argv, (char *)(data + 1), size) != 0) {
			fprintf(stderr, "command too long\n");
			free(data);
			return (1);
		}
		size += sizeof *data;

		/* Send the command. */
		if (proc_send(client_peer, msg, -1, data, size) != 0) {
			fprintf(stderr, "failed to send command\n");
			free(data);
			return (1);
		}
		free(data);
	} else if (msg == MSG_SHELL)
		proc_send(client_peer, msg, -1, NULL, 0);

	/* Start main loop. */
	proc_loop(client_proc, NULL);

	/* Run command if user requested exec, instead of exiting. */
	if (client_exittype == MSG_EXEC) {
		if (client_flags & CLIENT_CONTROLCONTROL)
			tcsetattr(STDOUT_FILENO, TCSAFLUSH, &saved_tio);
		client_exec(client_execshell, client_execcmd);
	}

	/* Restore streams to blocking. */
	setblocking(STDIN_FILENO, 1);
	setblocking(STDOUT_FILENO, 1);
	setblocking(STDERR_FILENO, 1);

#ifdef PLATFORM_WINDOWS
	/* Restore console modes so the shell is usable after detach. */
	if (client_console_mode_saved) {
		HANDLE hIn  = GetStdHandle(STD_INPUT_HANDLE);
		HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
		if (hIn  != INVALID_HANDLE_VALUE) SetConsoleMode(hIn,  client_saved_stdin_mode);
		if (hOut != INVALID_HANDLE_VALUE) SetConsoleMode(hOut, client_saved_stdout_mode);
	}
#endif

	/* Print the exit message, if any, and exit. */
	if (client_attached) {
		if (client_exitreason != CLIENT_EXIT_NONE)
			printf("[%s]\n", client_exit_message());

		ppid = getppid();
		if (client_exittype == MSG_DETACHKILL && ppid > 1)
			kill(ppid, SIGHUP);
	} else if (client_flags & CLIENT_CONTROL) {
		if (client_exitreason != CLIENT_EXIT_NONE)
			printf("%%exit %s\n", client_exit_message());
		else
			printf("%%exit\n");
		fflush(stdout);
		if (client_flags & CLIENT_CONTROL_WAITEXIT) {
			setvbuf(stdin, NULL, _IOLBF, 0);
			for (;;) {
				linelen = getline(&line, &linesize, stdin);
				if (linelen <= 1)
					break;
			}
			free(line);
		}
		if (client_flags & CLIENT_CONTROLCONTROL) {
			printf("\033\\");
			fflush(stdout);
			tcsetattr(STDOUT_FILENO, TCSAFLUSH, &saved_tio);
		}
	} else if (client_exitreason != CLIENT_EXIT_NONE)
		fprintf(stderr, "%s\n", client_exit_message());
	return (client_exitval);
}

/* Send identify messages to server. */
static void
client_send_identify(const char *ttynam, const char *termname, char **caps,
    u_int ncaps, const char *cwd, int feat)
{
	char	**ss;
	size_t	  sslen;
	int	  fd;
	uint64_t  flags = client_flags;
	pid_t	  pid;
	u_int	  i;

#ifdef PLATFORM_WINDOWS
	win32_log("client_send_identify: starting identification\n");
#endif
	proc_send(client_peer, MSG_IDENTIFY_LONGFLAGS, -1, &flags, sizeof flags);
#ifdef PLATFORM_WINDOWS
	/* 
	 * Configure console for tmux operation:
	 * - Output: Enable VT processing so escape sequences are rendered
	 * - Input: Disable line mode/echo (raw mode), enable VT sequences
	 */
	HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
	HANDLE hIn = GetStdHandle(STD_INPUT_HANDLE);
	DWORD dwMode = 0;
	
	/* Configure stdout for VT processing */
	if (GetConsoleMode(hOut, &dwMode)) {
		client_saved_stdout_mode = dwMode;
		dwMode |= ENABLE_VIRTUAL_TERMINAL_PROCESSING;
		if (!SetConsoleMode(hOut, dwMode)) {
			win32_log("client_send_identify: SetConsoleMode(STDOUT, VT) failed, error=%lu\n", GetLastError());
		} else {
			win32_log("client_send_identify: STDOUT VT enabled, hOut=%p mode=0x%lx\n", (void*)hOut, dwMode);
		}
	}

	/* Configure stdin for raw VT input mode (no echo, no line buffering) */
	if (GetConsoleMode(hIn, &dwMode)) {
		client_saved_stdin_mode = dwMode;
		client_console_mode_saved = 1;
		/* Clear line input mode and echo - we want raw keystrokes */
		dwMode &= ~ENABLE_LINE_INPUT;
		dwMode &= ~ENABLE_ECHO_INPUT;
		dwMode &= ~ENABLE_PROCESSED_INPUT;
		dwMode |= ENABLE_VIRTUAL_TERMINAL_INPUT;
		dwMode |= ENABLE_WINDOW_INPUT;
		if (!SetConsoleMode(hIn, dwMode)) {
			win32_log("client_send_identify: SetConsoleMode(STDIN, raw) failed, error=%lu\n", GetLastError());
		} else {
			win32_log("client_send_identify: STDIN raw mode enabled, old=0x%lx new=0x%lx\n",
			    client_saved_stdin_mode, dwMode);
		}
	}
#endif


	proc_send(client_peer, MSG_IDENTIFY_LONGFLAGS, -1, &client_flags,
		sizeof client_flags);

	proc_send(client_peer, MSG_IDENTIFY_TERM, -1, termname,
	    strlen(termname) + 1);
	proc_send(client_peer, MSG_IDENTIFY_FEATURES, -1, &feat, sizeof feat);

	proc_send(client_peer, MSG_IDENTIFY_TTYNAME, -1, ttynam,
	    strlen(ttynam) + 1);
	proc_send(client_peer, MSG_IDENTIFY_CWD, -1, cwd, strlen(cwd) + 1);

	for (i = 0; i < ncaps; i++) {
		proc_send(client_peer, MSG_IDENTIFY_TERMINFO, -1,
		    caps[i], strlen(caps[i]) + 1);
	}

#ifdef PLATFORM_WINDOWS
	struct winsize ws;
	CONSOLE_SCREEN_BUFFER_INFO csbi;

	memset(&ws, 0, sizeof ws);
	if (GetConsoleScreenBufferInfo(GetStdHandle(STD_OUTPUT_HANDLE), &csbi)) {
		ws.ws_col = csbi.srWindow.Right - csbi.srWindow.Left + 1;
		ws.ws_row = csbi.srWindow.Bottom - csbi.srWindow.Top + 1;
		
		/* Enforce minimum size to prevent rendering issues */
		if (ws.ws_col < 80)
			ws.ws_col = 80;
		if (ws.ws_row < 24)
			ws.ws_row = 24;
			
		win32_log("client_send_identify: reporting size %ux%u (raw: %dx%d)\n", 
			ws.ws_col, ws.ws_row,
			csbi.srWindow.Right - csbi.srWindow.Left + 1,
			csbi.srWindow.Bottom - csbi.srWindow.Top + 1);
	} else {
		ws.ws_col = 80;
		ws.ws_row = 24;
		win32_log("client_send_identify: GetConsoleScreenBufferInfo failed, defaulting to 80x24\n");
	}


	proc_send(client_peer, MSG_IDENTIFY_STDIN, -1, NULL, 0);
	proc_send(client_peer, MSG_IDENTIFY_STDOUT, -1, &ws, sizeof ws);
#else
	if ((fd = dup(STDIN_FILENO)) == -1)
		fatal("dup failed");
	proc_send(client_peer, MSG_IDENTIFY_STDIN, fd, NULL, 0);
	if ((fd = dup(STDOUT_FILENO)) == -1)
		fatal("dup failed");
	proc_send(client_peer, MSG_IDENTIFY_STDOUT, fd, NULL, 0);
#endif

	pid = getpid();
	proc_send(client_peer, MSG_IDENTIFY_CLIENTPID, -1, &pid, sizeof pid);

	for (ss = environ; *ss != NULL; ss++) {
		sslen = strlen(*ss) + 1;
		if (sslen > MAX_IMSGSIZE - IMSG_HEADER_SIZE)
			continue;
		proc_send(client_peer, MSG_IDENTIFY_ENVIRON, -1, *ss, sslen);
	}

	proc_send(client_peer, MSG_IDENTIFY_DONE, -1, NULL, 0);
#ifdef PLATFORM_WINDOWS
	win32_log("client_send_identify: identification done\n");
#endif
}

/* Run command in shell; used for -c. */
static __dead void
client_exec(const char *shell, const char *shellcmd)
{
	char	*argv0;

	log_debug("shell %s, command %s", shell, shellcmd);
	argv0 = shell_argv0(shell, !!(client_flags & CLIENT_LOGIN));
	setenv("SHELL", shell, 1);

	proc_clear_signals(client_proc, 1);

	setblocking(STDIN_FILENO, 1);
	setblocking(STDOUT_FILENO, 1);
	setblocking(STDERR_FILENO, 1);
	closefrom(STDERR_FILENO + 1);

	execl(shell, argv0, "-c", shellcmd, (char *) NULL);
	fatal("execl failed");
}

/* Callback to handle signals in the client. */
static void
client_signal(int sig)
{
	struct sigaction sigact;
	int		 status;
	pid_t		 pid;

	log_debug("%s: %s", __func__, strsignal(sig));
	if (sig == SIGCHLD) {
		for (;;) {
			pid = waitpid(WAIT_ANY, &status, WNOHANG);
			if (pid == 0)
				break;
			if (pid == -1) {
				if (errno == ECHILD)
					break;
				log_debug("waitpid failed: %s",
				    strerror(errno));
			}
		}
	} else if (!client_attached) {
		if (sig == SIGTERM || sig == SIGHUP)
			proc_exit(client_proc);
	} else {
		switch (sig) {
		case SIGHUP:
			client_exitreason = CLIENT_EXIT_LOST_TTY;
			client_exitval = 1;
			proc_send(client_peer, MSG_EXITING, -1, NULL, 0);
			break;
		case SIGTERM:
			if (!client_suspended)
				client_exitreason = CLIENT_EXIT_TERMINATED;
			client_exitval = 1;
			proc_send(client_peer, MSG_EXITING, -1, NULL, 0);
			break;
		case SIGWINCH:
			proc_send(client_peer, MSG_RESIZE, -1, NULL, 0);
			break;
		case SIGCONT:
			memset(&sigact, 0, sizeof sigact);
			sigemptyset(&sigact.sa_mask);
			sigact.sa_flags = SA_RESTART;
			sigact.sa_handler = SIG_IGN;
			if (sigaction(SIGTSTP, &sigact, NULL) != 0)
				fatal("sigaction failed");
			proc_send(client_peer, MSG_WAKEUP, -1, NULL, 0);
			client_suspended = 0;
			break;
		}
	}
}

/* Callback for file write error or close. */
static void
client_file_check_cb(__unused struct client *c, __unused const char *path,
    __unused int error, __unused int closed, __unused struct evbuffer *buffer,
    __unused void *data)
{
	if (client_exitflag)
		client_exit();
}

/* Callback for client read events. */
static void
client_dispatch(struct imsg *imsg, __unused void *arg)
{
#ifdef PLATFORM_WINDOWS
	win32_log("client_dispatch: imsg=%p client_exitflag=%d\n", 
		(void*)imsg, client_exitflag);
#endif

	if (imsg == NULL) {
#ifdef PLATFORM_WINDOWS
		win32_log("client_dispatch: imsg=NULL, setting CLIENT_EXIT_LOST_SERVER, exitval=1\n");
#endif
		if (!client_exitflag) {
			client_exitreason = CLIENT_EXIT_LOST_SERVER;
			client_exitval = 1;
		}
		proc_exit(client_proc);
		return;
	}

#ifdef PLATFORM_WINDOWS
	win32_log("client_dispatch: imsg->hdr.type=%d client_attached=%d\n", 
		imsg->hdr.type, client_attached);
#endif

	if (client_attached)
		client_dispatch_attached(imsg);
	else
		client_dispatch_wait(imsg);
}


/* Process an exit message. */
static void
client_dispatch_exit_message(char *data, size_t datalen)
{
	int	retval;

	if (datalen < sizeof retval && datalen != 0)
		fatalx("bad MSG_EXIT size");

	if (datalen >= sizeof retval) {
		memcpy(&retval, data, sizeof retval);
		client_exitval = retval;
	}

	if (datalen > sizeof retval) {
		datalen -= sizeof retval;
		data += sizeof retval;

		client_exitmessage = xmalloc(datalen);
		memcpy(client_exitmessage, data, datalen);
		client_exitmessage[datalen - 1] = '\0';

		client_exitreason = CLIENT_EXIT_MESSAGE_PROVIDED;
	}
}

/* Dispatch imsgs when in wait state (before MSG_READY). */
static void
client_dispatch_wait(struct imsg *imsg)
{
	char		*data;
	ssize_t		 datalen;
	static int	 pledge_applied;

	/*
	 * "sendfd" is no longer required once all of the identify messages
	 * have been sent. We know the server won't send us anything until that
	 * point (because we don't ask it to), so we can drop "sendfd" once we
	 * get the first message from the server.
	 */
	if (!pledge_applied) {
		if (pledge(
		    "stdio rpath wpath cpath unix proc exec tty",
		    NULL) != 0)
			fatal("pledge failed");
		pledge_applied = 1;
	}

	data = imsg->data;
	datalen = imsg->hdr.len - IMSG_HEADER_SIZE;

	switch (imsg->hdr.type) {
	case MSG_EXIT:
	case MSG_SHUTDOWN:
		client_dispatch_exit_message(data, datalen);
		client_exitflag = 1;
		client_exit();
		break;
	case MSG_READY:
		if (datalen != 0)
			fatalx("bad MSG_READY size");

		client_attached = 1;
#ifdef PLATFORM_WINDOWS
		{
			int input_fds[2];
			if (win32_socketpair(AF_INET, SOCK_STREAM, 0, input_fds) == 0) {
				win32_log("client_dispatch_attached: input_fds[0]=%d, input_fds[1]=%d\n", input_fds[0], input_fds[1]);
				_beginthreadex(NULL, 0, client_input_thread, (void*)(intptr_t)input_fds[1], 0, NULL);
				struct event *ev = xmalloc(sizeof *ev);
				/* Pass input_fds[0] as arg so callback can use mapped fd, not raw SOCKET */
				win32_event_set(ev, input_fds[0], EV_READ | EV_PERSIST, client_input_callback, (void*)(intptr_t)input_fds[0]);
				event_add(ev, NULL);
			}

			/* Start resize monitor thread */
			int resize_fds[2];
			if (win32_socketpair(AF_INET, SOCK_STREAM, 0, resize_fds) == 0) {
				win32_log("client_dispatch_attached: resize_fds[0]=%d, resize_fds[1]=%d\n", resize_fds[0], resize_fds[1]);
				struct client_resize_thread_arg *ra = xmalloc(sizeof *ra);
				ra->fd = resize_fds[1];
				_beginthreadex(NULL, 0, client_resize_thread, ra, 0, NULL);
				struct event *rev = xmalloc(sizeof *rev);
				win32_event_set(rev, resize_fds[0], EV_READ | EV_PERSIST, client_resize_callback, (void*)(intptr_t)resize_fds[0]);
				event_add(rev, NULL);
			}
		}
#endif
		proc_send(client_peer, MSG_RESIZE, -1, NULL, 0);
		break;
	case MSG_VERSION:
		if (datalen != 0)
			fatalx("bad MSG_VERSION size");

		fprintf(stderr, "protocol version mismatch "
		    "(client %d, server %u)\n", PROTOCOL_VERSION,
		    imsg->hdr.peerid & 0xff);
		client_exitval = 1;
		proc_exit(client_proc);
		break;
	case MSG_FLAGS:
		if (datalen != sizeof client_flags)
			fatalx("bad MSG_FLAGS string");

		memcpy(&client_flags, data, sizeof client_flags);
		log_debug("new flags are %#llx",
		    (unsigned long long)client_flags);
		break;
	case MSG_SHELL:
		if (datalen == 0 || data[datalen - 1] != '\0')
			fatalx("bad MSG_SHELL string");

		client_exec(data, shell_command);
		/* NOTREACHED */
	case MSG_DETACH:
	case MSG_DETACHKILL:
		proc_send(client_peer, MSG_EXITING, -1, NULL, 0);
		break;
	case MSG_EXITED:
		proc_exit(client_proc);
		break;
	case MSG_READ_OPEN:
		file_read_open(&client_files, client_peer, imsg, 1,
		    !(client_flags & CLIENT_CONTROL), client_file_check_cb,
		    NULL);
		break;
	case MSG_READ_CANCEL:
		file_read_cancel(&client_files, imsg);
		break;
	case MSG_WRITE_OPEN:
		file_write_open(&client_files, client_peer, imsg, 1,
		    !(client_flags & CLIENT_CONTROL), client_file_check_cb,
		    NULL);
		break;
	case MSG_WRITE:
		file_write_data(&client_files, imsg);
		break;
	case MSG_WRITE_CLOSE:
		file_write_close(&client_files, imsg);
		break;
	case MSG_OLDSTDERR:
	case MSG_OLDSTDIN:
	case MSG_OLDSTDOUT:
		fprintf(stderr, "server version is too old for client\n");
		proc_exit(client_proc);
		break;
	}
}

/* Dispatch imsgs in attached state (after MSG_READY). */
static void
client_dispatch_attached(struct imsg *imsg)
{
	struct sigaction	 sigact;
	char			*data;
	ssize_t			 datalen;

	data = imsg->data;
	datalen = imsg->hdr.len - IMSG_HEADER_SIZE;

	switch (imsg->hdr.type) {
	case MSG_FLAGS:
		if (datalen != sizeof client_flags)
			fatalx("bad MSG_FLAGS string");

		memcpy(&client_flags, data, sizeof client_flags);
		log_debug("new flags are %#llx",
		    (unsigned long long)client_flags);
		break;
	case MSG_DETACH:
	case MSG_DETACHKILL:
		if (datalen == 0 || data[datalen - 1] != '\0')
			fatalx("bad MSG_DETACH string");

		client_exitsession = xstrdup(data);
		client_exittype = imsg->hdr.type;
		if (imsg->hdr.type == MSG_DETACHKILL)
			client_exitreason = CLIENT_EXIT_DETACHED_HUP;
		else
			client_exitreason = CLIENT_EXIT_DETACHED;
		proc_send(client_peer, MSG_EXITING, -1, NULL, 0);
		break;
	case MSG_EXEC:
		if (datalen == 0 || data[datalen - 1] != '\0' ||
		    strlen(data) + 1 == (size_t)datalen)
			fatalx("bad MSG_EXEC string");
		client_execcmd = xstrdup(data);
		client_execshell = xstrdup(data + strlen(data) + 1);

		client_exittype = imsg->hdr.type;
		proc_send(client_peer, MSG_EXITING, -1, NULL, 0);
		break;
	case MSG_EXIT:
		client_dispatch_exit_message(data, datalen);
		if (client_exitreason == CLIENT_EXIT_NONE)
			client_exitreason = CLIENT_EXIT_EXITED;
		proc_send(client_peer, MSG_EXITING, -1, NULL, 0);
		break;
	case MSG_EXITED:
		if (datalen != 0)
			fatalx("bad MSG_EXITED size");

		proc_exit(client_proc);
		break;
	case MSG_SHUTDOWN:
		if (datalen != 0)
			fatalx("bad MSG_SHUTDOWN size");

		proc_send(client_peer, MSG_EXITING, -1, NULL, 0);
		client_exitreason = CLIENT_EXIT_SERVER_EXITED;
		client_exitval = 1;
		break;
	case MSG_SUSPEND:
		if (datalen != 0)
			fatalx("bad MSG_SUSPEND size");

		memset(&sigact, 0, sizeof sigact);
		sigemptyset(&sigact.sa_mask);
		sigact.sa_flags = SA_RESTART;
		sigact.sa_handler = SIG_DFL;
		if (sigaction(SIGTSTP, &sigact, NULL) != 0)
			fatal("sigaction failed");
		client_suspended = 1;
		kill(getpid(), SIGTSTP);
		break;
	case MSG_LOCK:
		if (datalen == 0 || data[datalen - 1] != '\0')
			fatalx("bad MSG_LOCK string");

		system(data);
		proc_send(client_peer, MSG_UNLOCK, -1, NULL, 0);
		break;
	case MSG_READ_OPEN:
		file_read_open(&client_files, client_peer, imsg, 1,
		    !(client_flags & CLIENT_CONTROL), client_file_check_cb,
		    NULL);
		break;
	case MSG_READ_CANCEL:
		file_read_cancel(&client_files, imsg);
		break;
	case MSG_WRITE_OPEN:
		file_write_open(&client_files, client_peer, imsg, 1,
		    !(client_flags & CLIENT_CONTROL), client_file_check_cb,
		    NULL);
		break;
	case MSG_WRITE:
		file_write_data(&client_files, imsg);
		break;
	case MSG_WRITE_CLOSE:
		file_write_close(&client_files, imsg);
		break;
	}
}

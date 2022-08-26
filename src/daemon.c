#include "keyd.h"

#define VKBD_NAME "keyd virtual keyboard"

struct config_ent {
	struct config config;
	struct keyboard *kbd;
	struct config_ent *next;
};

static int ipcfd = -1;
static struct vkbd *vkbd = NULL;
static struct config_ent *configs;

static struct device *devices[MAX_DEVICES];
static size_t nr_devices;

static uint8_t keystate[256];

static int listeners[32];
static size_t nr_listeners = 0;

static void free_configs()
{
	struct config_ent *ent = configs;
	while (ent) {
		struct config_ent *tmp = ent;
		ent = ent->next;
		free(tmp->kbd);
		free(tmp);
	}

	configs = NULL;
}

static void cleanup()
{
	free_configs();
	free_vkbd(vkbd);
}

static void clear_vkbd()
{
	size_t i;

	for (i = 0; i < 256; i++)
		if (keystate[i]) {
			vkbd_send_key(vkbd, i, 0);
			keystate[i] = 0;
		}
}

static void send_key(uint8_t code, uint8_t state)
{
	keystate[code] = state;
	vkbd_send_key(vkbd, code, state);
}

static void add_listener(int con)
{
	struct timeval tv;

	/*
	 * In order to avoid blocking the main event loop, allow up to 50ms for
	 * slow clients to relieve back pressure before dropping them.
	 */
	tv.tv_usec = 50000;
	tv.tv_sec = 0;

	if (nr_listeners == ARRAY_SIZE(listeners)) {
		char s[] = "Max listeners exceeded\n";
		xwrite(con, &s, sizeof s);

		close(con);
		return;
	}

	setsockopt(con, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);

	listeners[nr_listeners++] = con;
}

static void layer_observer(const char *name, int state)
{
	if (!nr_listeners)
		return;

	char buf[MAX_LAYER_NAME_LEN+2];
	ssize_t bufsz = snprintf(buf, sizeof(buf), "%c%s\n", state ? '+' : '-', name);
	size_t i;

	int keep[ARRAY_SIZE(listeners)];
	size_t n = 0;

	for (i = 0; i < nr_listeners; i++) {
		ssize_t nw = write(listeners[i], buf, bufsz);

		if (nw == bufsz)
			keep[n++] = listeners[i];
		else
			close(listeners[i]);
	}

	if (n != nr_listeners) {
		nr_listeners = n;
		memcpy(listeners, keep, n * sizeof(int));
	}
}

static void load_configs()
{
	DIR *dh = opendir(CONFIG_DIR);
	struct dirent *dirent;

	if (!dh) {
		perror("opendir");
		exit(-1);
	}

	configs = NULL;

	while ((dirent = readdir(dh))) {
		char path[1024];
		int len;

		if (dirent->d_type == DT_DIR)
			continue;

		len = snprintf(path, sizeof path, "%s/%s", CONFIG_DIR, dirent->d_name);

		if (len >= 5 && !strcmp(path + len - 5, ".conf")) {
			struct config_ent *ent = calloc(1, sizeof(struct config_ent));

			printf("CONFIG: parsing %s\n", path);

			if (config_parse(&ent->config, path))
				die("failed to parse %s", path);

			ent->kbd = new_keyboard(&ent->config, send_key, layer_observer);
			ent->next = configs;
			configs = ent;
		}
	}

	closedir(dh);
}

static int lookup_config_ent(uint32_t id, struct config_ent **match)
{
	struct config_ent *ent = configs;
	int rank = 0;

	*match = NULL;

	while (ent) {
		int r = config_check_match(&ent->config, id);

		if (r > rank) {
			*match = ent;
			rank = r;
		}

		ent = ent->next;
	}

	return rank;
}

static void manage_device(struct device *dev)
{
	uint32_t id = dev->vendor_id << 16 | dev->product_id;
	struct config_ent *ent = NULL;

	int match = lookup_config_ent(id, &ent);

	if ((match && dev->capabilities & CAP_KEYBOARD) ||
	    (match == 2 && dev->capabilities & (CAP_MOUSE | CAP_MOUSE_ABS))) {
		if (device_grab(dev)) {
			warn("Failed to grab %s", dev->path);
			dev->data = NULL;
			return;
		}

		printf("DEVICE: \033[32;1mmatch   \033[0m %04hx:%04hx  %s\t(%s)\n",
		     dev->vendor_id, dev->product_id,
		     ent->config.path,
		     dev->name);

		dev->data = ent->kbd;
	} else {
		dev->data = NULL;
		device_ungrab(dev);
		printf("DEVICE: \033[31;1mignoring\033[0m %04hx:%04hx  (%s)\n",
		     dev->vendor_id, dev->product_id, dev->name);
	}
}

static void reload()
{
	size_t i;

	free_configs();
	load_configs();

	for (i = 0; i < nr_devices; i++)
		manage_device(devices[i]);

	clear_vkbd();
}

static void send_success(int con)
{
	struct ipc_message msg;

	msg.type = IPC_SUCCESS;;
	msg.sz = sprintf(msg.data, "Success");

	xwrite(con, &msg, sizeof msg);
	close(con);
}

static void send_fail(int con, const char *fmt, ...)
{
	struct ipc_message msg;
	va_list args;

	va_start(args, fmt);

	msg.type = IPC_FAIL;
	msg.sz = vsnprintf(msg.data, sizeof(msg.data), fmt, args);

	xwrite(con, &msg, sizeof msg);
	close(con);

	va_end(args);
}

static void handle_client(int con)
{
	struct ipc_message msg;

	xread(con, &msg, sizeof msg);

	switch (msg.type) {
		struct config_ent *ent;
		int success;

	case IPC_RELOAD:
		reload();
		send_success(con);
		break;
	case IPC_LAYER_LISTEN:
		add_listener(con);
		break;
	case IPC_BIND:
		success = 0;

		for (ent = configs; ent; ent = ent->next) {
			if (!kbd_eval(ent->kbd, msg.data))
				success = 1;
		}

		if (success)
			send_success(con);
		else
			send_fail(con, "%s", errstr);


		break;
	default:
		send_fail(con, "Unknown command");
		break;
	}
}

static void remove_device(struct device *dev)
{
	size_t i;
	size_t n = 0;

	for (i = 0; i < nr_devices; i++)
		if (devices[i] != dev)
			devices[n++] = devices[i];
	
	printf("DEVICE: \033[31;1mremoved\033[0m\t%04hx:%04hx %s\n",
		dev->vendor_id,
		dev->product_id,
		dev->name);

	nr_devices = n;
}

static void add_device(struct device *dev)
{
	assert(nr_devices < MAX_DEVICES);
	devices[nr_devices++] = dev;

	manage_device(dev);
}

static int event_handler(struct event *ev)
{
	int timeout;
	static struct keyboard *last_kbd = NULL;

	switch (ev->type) {
	case EV_TIMEOUT:
		if (!last_kbd)
			return 0;

		return kbd_process_key_event(last_kbd, 0, 0);
	case EV_DEV_EVENT:
		timeout = ev->timeleft;

		if (ev->dev->data) {
			last_kbd = ev->dev->data;

			switch (ev->devev->type) {
			case DEV_KEY:
				timeout = kbd_process_key_event(ev->dev->data, ev->devev->code,
							     ev->devev->pressed);
				break;
			case DEV_MOUSE_MOVE:
				vkbd_mouse_move(vkbd, ev->devev->x, ev->devev->y);
				break;
			case DEV_MOUSE_MOVE_ABS:
				vkbd_mouse_move_abs(vkbd, ev->devev->x, ev->devev->y);
				break;
			default:
				break;
			case DEV_MOUSE_SCROLL:
				/*
				 * Treat scroll events as mouse buttons so oneshot and the like get
				 * cleared.
				 */
				if (last_kbd) {
					kbd_process_key_event(last_kbd,
							      KEYD_EXTERNAL_MOUSE_BUTTON, 1);
					kbd_process_key_event(last_kbd,
							      KEYD_EXTERNAL_MOUSE_BUTTON, 0);
				}

				vkbd_mouse_scroll(vkbd, ev->devev->x, ev->devev->y);
				break;
			}
		}

		return timeout;
		break;
	case EV_DEV_ADD:
		if (strcmp(ev->dev->name, VKBD_NAME))
			add_device(ev->dev);
		break;
	case EV_DEV_REMOVE:
		remove_device(ev->dev);
		break;
	case EV_FD_ACTIVITY:
		if (ev->fd == ipcfd) {
			int con = accept(ipcfd, NULL, 0);
			if (con < 0) {
				perror("accept");
				exit(-1);
			}

			handle_client(con);
		}
		break;
	default:
		break;
	}

	return 0;
}

int run_daemon(int argc, char *argv[])
{
	ipcfd = ipc_create_server(SOCKET_PATH);
	if (ipcfd < 0)
		die("failed to create %s (another instance already running?)", SOCKET_PATH);

	vkbd = vkbd_init(VKBD_NAME);

	setvbuf(stdout, NULL, _IOLBF, 0);
	setvbuf(stderr, NULL, _IOLBF, 0);
	nice(-20);

	evloop_add_fd(ipcfd);

	reload();

	atexit(cleanup);

	evloop(event_handler);

	return 0;
}

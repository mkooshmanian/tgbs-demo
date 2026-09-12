/*
 * SPDX-License-Identifier: MIT
 *
 * Live terminal response-time timeline for the periodic RT tasks started by
 * tgbs-demo-mixed.  fake-task emits one fixed-size datagram per completed job.
 */

#define _GNU_SOURCE

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#define FAKEJOB_MAGIC 0x004A4B46u
#define FAKEJOB_VERSION 1u
#define FAKEJOB_TYPE_T0 0u
#define FAKEJOB_TYPE_JOB 1u
#define FAKEJOB_TOTAL_SIZE 36u

#define MAX_RT_TASKS 32
#define HISTORY_SIZE 256
#define TASK_NAME_SIZE 64

#define C_RESET "\033[0m"
#define C_BOLD "\033[1m"
#define C_DIM "\033[2m"
#define C_RED "\033[31m"
#define C_GREEN "\033[32m"
#define C_YELLOW "\033[33m"
#define C_CYAN "\033[36m"

#pragma pack(push, 1)
struct metrics_record {
	uint32_t magic;
	uint32_t version;
	uint32_t type;
	uint32_t pid;
	uint32_t iter;
	uint64_t start_ns;
	uint64_t finish_ns;
};
#pragma pack(pop)

_Static_assert(sizeof(struct metrics_record) == FAKEJOB_TOTAL_SIZE,
	"unexpected fake-task metrics record size");

struct sample {
	double response_ms;
	bool missed;
};

struct rt_task {
	uint32_t pid;
	char name[TASK_NAME_SIZE];
	double period_ms;
	int priority;
	uint64_t t0_ns;
	bool have_t0;
	struct sample history[HISTORY_SIZE];
	unsigned int history_start;
	unsigned int history_count;
	unsigned long long total_jobs;
	unsigned long long total_misses;
};

static struct rt_task tasks[MAX_RT_TASKS];
static unsigned int task_count;
static volatile sig_atomic_t stop_requested;
static struct termios saved_termios;
static bool terminal_saved;
static bool use_color = true;
static const char *socket_name = "@tgbs-demo-mixed";
static const char *state_dir = "/run/tgbs-demo/mixed";

static const char *color(const char *code)
{
	return use_color ? code : "";
}

static void restore_terminal(void)
{
	if (terminal_saved) {
		tcsetattr(STDIN_FILENO, TCSAFLUSH, &saved_termios);
		terminal_saved = false;
	}
}

static void signal_handler(int signal_number)
{
	(void)signal_number;
	stop_requested = 1;
}

static bool enable_interactive_terminal(void)
{
	struct termios raw;

	if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO))
		return false;
	if (tcgetattr(STDIN_FILENO, &saved_termios) != 0)
		return false;
	raw = saved_termios;
	raw.c_lflag &= (tcflag_t)~(ICANON | ECHO);
	raw.c_cc[VMIN] = 0;
	raw.c_cc[VTIME] = 0;
	if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) != 0)
		return false;
	terminal_saved = true;
	return true;
}

static void terminal_size(unsigned int *rows, unsigned int *columns)
{
	struct winsize size;

	*rows = 24;
	*columns = 80;
	if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &size) == 0) {
		if (size.ws_row != 0)
			*rows = size.ws_row;
		if (size.ws_col != 0)
			*columns = size.ws_col;
	}
}

static int bind_metrics_socket(const char *name)
{
	struct sockaddr_un address;
	socklen_t address_length;
	const char *actual_name;
	size_t length;
	int fd;

	if (name == NULL || name[0] == '\0') {
		errno = EINVAL;
		return -1;
	}
	memset(&address, 0, sizeof(address));
	address.sun_family = AF_UNIX;
	if (name[0] == '@') {
		actual_name = name + 1;
		length = strlen(actual_name);
		if (length == 0 || length >= sizeof(address.sun_path) - 1) {
			errno = ENAMETOOLONG;
			return -1;
		}
		address.sun_path[0] = '\0';
		memcpy(address.sun_path + 1, actual_name, length);
		address_length = (socklen_t)(offsetof(struct sockaddr_un, sun_path) +
			1 + length);
	} else {
		length = strlen(name);
		if (length >= sizeof(address.sun_path)) {
			errno = ENAMETOOLONG;
			return -1;
		}
		memcpy(address.sun_path, name, length + 1);
		address_length = (socklen_t)(offsetof(struct sockaddr_un, sun_path) +
			length + 1);
	}
	fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_NONBLOCK, 0);
	if (fd < 0)
		return -1;
	if (bind(fd, (struct sockaddr *)&address, address_length) != 0) {
		int saved_errno = errno;
		close(fd);
		errno = saved_errno;
		return -1;
	}
	return fd;
}

static bool load_task_metadata(struct rt_task *task)
{
	char path[PATH_MAX];
	FILE *stream;
	char name[TASK_NAME_SIZE];
	double period;
	int priority;

	if (snprintf(path, sizeof(path), "%s/%u.task", state_dir, task->pid) >=
	    (int)sizeof(path))
		return false;
	stream = fopen(path, "r");
	if (stream == NULL)
		return false;
	if (fscanf(stream, "%63s %lf %d", name, &period, &priority) != 3 ||
	    period <= 0.0) {
		fclose(stream);
		return false;
	}
	fclose(stream);
	for (char *cursor = name; *cursor != '\0'; cursor++) {
		if (!isprint((unsigned char)*cursor))
			*cursor = '?';
	}
	snprintf(task->name, sizeof(task->name), "%s", name);
	task->period_ms = period;
	task->priority = priority;
	return true;
}

static struct rt_task *find_task(uint32_t pid)
{
	for (unsigned int i = 0; i < task_count; i++) {
		if (tasks[i].pid == pid)
			return &tasks[i];
	}
	return NULL;
}

static struct rt_task *get_task(uint32_t pid)
{
	struct rt_task *task = find_task(pid);

	if (task != NULL) {
		if (task->period_ms <= 0.0)
			load_task_metadata(task);
		return task;
	}
	if (task_count >= MAX_RT_TASKS)
		return NULL;
	task = &tasks[task_count++];
	memset(task, 0, sizeof(*task));
	task->pid = pid;
	snprintf(task->name, sizeof(task->name), "pid-%u", pid);
	load_task_metadata(task);
	return task;
}

static void append_sample(struct rt_task *task, double response_ms)
{
	unsigned int index;
	bool missed = task->period_ms > 0.0 && response_ms > task->period_ms;

	if (task->history_count < HISTORY_SIZE) {
		index = (task->history_start + task->history_count) % HISTORY_SIZE;
		task->history_count++;
	} else {
		index = task->history_start;
		task->history_start = (task->history_start + 1) % HISTORY_SIZE;
	}
	task->history[index].response_ms = response_ms;
	task->history[index].missed = missed;
	task->total_jobs++;
	if (missed)
		task->total_misses++;
}

static void process_record(const struct metrics_record *record)
{
	struct rt_task *task;
	uint64_t release_ns;
	double response_ms;

	if (record->magic != FAKEJOB_MAGIC || record->version != FAKEJOB_VERSION)
		return;
	task = get_task(record->pid);
	if (task == NULL)
		return;
	if (record->type == FAKEJOB_TYPE_T0) {
		task->t0_ns = record->start_ns;
		task->have_t0 = true;
		return;
	}
	if (record->type != FAKEJOB_TYPE_JOB || !task->have_t0 ||
	    task->period_ms <= 0.0)
		return;
	release_ns = task->t0_ns + (uint64_t)((double)record->iter *
		task->period_ms * 1000000.0);
	if (record->finish_ns < release_ns)
		return;
	response_ms = (double)(record->finish_ns - release_ns) / 1000000.0;
	append_sample(task, response_ms);
}

static void drain_socket(int fd)
{
	for (;;) {
		struct metrics_record record;
		ssize_t received = recv(fd, &record, sizeof(record), 0);
		if (received == (ssize_t)sizeof(record)) {
			process_record(&record);
			continue;
		}
		if (received < 0 && errno == EINTR)
			continue;
		break;
	}
}

static struct sample *history_sample(struct rt_task *task, unsigned int offset)
{
	unsigned int index = (task->history_start + offset) % HISTORY_SIZE;
	return &task->history[index];
}

static void set_plot_pixel(uint8_t *dots, uint8_t *severity,
		unsigned int width, unsigned int x, unsigned int y,
		uint8_t sample_severity)
{
	static const uint8_t dot_bits[4][2] = {
		{ 1u << 0, 1u << 3 },
		{ 1u << 1, 1u << 4 },
		{ 1u << 2, 1u << 5 },
		{ 1u << 6, 1u << 7 }
	};
	unsigned int cell = (y / 4) * width + x / 2;

	dots[cell] |= dot_bits[y % 4][x % 2];
	if (sample_severity > severity[cell])
		severity[cell] = sample_severity;
}

static void draw_plot_line(uint8_t *dots, uint8_t *severity,
		unsigned int width, int x0, int y0, int x1, int y1,
		uint8_t sample_severity)
{
	int dx = abs(x1 - x0);
	int sx = x0 < x1 ? 1 : -1;
	int dy = -abs(y1 - y0);
	int sy = y0 < y1 ? 1 : -1;
	int error = dx + dy;

	for (;;) {
		set_plot_pixel(dots, severity, width, (unsigned int)x0,
			(unsigned int)y0, sample_severity);
		if (x0 == x1 && y0 == y1)
			break;
		if (2 * error >= dy) {
			error += dy;
			x0 += sx;
		}
		if (2 * error <= dx) {
			error += dx;
			y0 += sy;
		}
	}
}

static void print_braille(uint8_t dots)
{
	unsigned int codepoint = 0x2800u + dots;
	unsigned char utf8[3] = {
		(unsigned char)(0xe0u | (codepoint >> 12)),
		(unsigned char)(0x80u | ((codepoint >> 6) & 0x3fu)),
		(unsigned char)(0x80u | (codepoint & 0x3fu))
	};

	if (dots == 0)
		putchar(' ');
	else
		fwrite(utf8, 1, sizeof(utf8), stdout);
}

static void render_task(struct rt_task *task, unsigned int graph_width,
		unsigned int graph_height)
{
	uint8_t dots[HISTORY_SIZE * 4] = { 0 };
	uint8_t severity[HISTORY_SIZE * 4] = { 0 };
	double last = 0.0;
	double sum = 0.0;
	double maximum = 0.0;
	unsigned int pixel_width = graph_width * 2;
	unsigned int pixel_height = graph_height * 4;
	unsigned int first = task->history_count > pixel_width ?
		task->history_count - pixel_width : 0;
	unsigned int shown = task->history_count - first;
	unsigned int x_offset = pixel_width - shown;
	int previous_x = -1;
	int previous_y = -1;
	uint8_t previous_severity = 0;

	for (unsigned int i = 0; i < task->history_count; i++) {
		double value = history_sample(task, i)->response_ms;
		sum += value;
		if (value > maximum)
			maximum = value;
		last = value;
	}
	printf("%s%-10.10s%s FIFO P%-2d  deadline %7.1f ms  last %7.1f  "
	       "avg %7.1f  max %7.1f  miss %llu/%llu\n",
		color(C_BOLD), task->name, color(C_RESET), task->priority,
		task->period_ms, last,
		task->history_count ? sum / task->history_count : 0.0, maximum,
		task->total_misses, task->total_jobs);

	printf("%7.1f %s", task->period_ms, color(C_RED));
	for (unsigned int i = 0; i < graph_width; i++)
		printf("─");
	printf(" deadline%s\n", color(C_RESET));

	for (unsigned int i = first; i < task->history_count; i++) {
		struct sample *sample = history_sample(task, i);
		double ratio = task->period_ms > 0.0 ?
			sample->response_ms / task->period_ms : 0.0;
		int x = (int)(x_offset + i - first);
		int y;
		uint8_t sample_severity;

		if (ratio < 0.0)
			ratio = 0.0;
		if (ratio > 1.0)
			ratio = 1.0;
		y = (int)((1.0 - ratio) * (pixel_height - 1) + 0.5);
		sample_severity = sample->missed ? 2 : ratio >= 0.8 ? 1 : 0;
		if (previous_x >= 0) {
			uint8_t line_severity = sample_severity > previous_severity ?
				sample_severity : previous_severity;

			draw_plot_line(dots, severity, graph_width, previous_x,
				previous_y, x, y, line_severity);
		} else {
			set_plot_pixel(dots, severity, graph_width,
				(unsigned int)x, (unsigned int)y, sample_severity);
		}
		previous_x = x;
		previous_y = y;
		previous_severity = sample_severity;
	}

	for (unsigned int row = 0; row < graph_height; row++) {
		printf(row + 1 == graph_height ? "%7.1f " : "        ", 0.0);
		for (unsigned int column = 0; column < graph_width; column++) {
			unsigned int cell = row * graph_width + column;
			const char *plot_color = severity[cell] >= 2 ? C_RED :
				severity[cell] == 1 ? C_YELLOW : C_GREEN;

			printf("%s", color(plot_color));
			print_braille(dots[cell]);
			printf("%s", color(C_RESET));
		}
		putchar('\n');
	}
}

static void render(bool interactive)
{
	unsigned int rows, columns;
	unsigned int graph_width;
	unsigned int graph_height = 4;
	time_t now = time(NULL);
	struct tm local;
	char timestamp[32];
	unsigned int displayed = 0;
	unsigned int eligible = 0;
	struct rt_task *visible[MAX_RT_TASKS];

	terminal_size(&rows, &columns);
	graph_width = columns > 20 ? columns - 20 : 16;
	if (graph_width > HISTORY_SIZE)
		graph_width = HISTORY_SIZE;
	for (unsigned int i = 0; i < task_count; i++) {
		if (tasks[i].period_ms > 0.0 && tasks[i].have_t0)
			visible[eligible++] = &tasks[i];
	}
	for (unsigned int i = 1; i < eligible; i++) {
		struct rt_task *current = visible[i];
		unsigned int position = i;

		while (position > 0 &&
		       (visible[position - 1]->priority < current->priority ||
		        (visible[position - 1]->priority == current->priority &&
		         strcmp(visible[position - 1]->name, current->name) > 0))) {
			visible[position] = visible[position - 1];
			position--;
		}
		visible[position] = current;
	}
	if (eligible > 0) {
		unsigned int lines_per_task = rows > 3 ? (rows - 3) / eligible : 1;

		graph_height = lines_per_task > 3 ? lines_per_task - 3 : 1;
		if (graph_height > 4)
			graph_height = 4;
	}
	if (interactive)
		printf("\033[H\033[2J");
	localtime_r(&now, &local);
	strftime(timestamp, sizeof(timestamp), "%H:%M:%S", &local);
	printf("%s%sRT TIMELINE%s  %s  response time / deadline",
		color(C_BOLD), color(C_CYAN), color(C_RESET), timestamp);
	if (interactive)
		printf("  %sq quit%s", color(C_DIM), color(C_RESET));
	printf("\n%sEach point is one completed job; red line = deadline; red point = miss.%s\n\n",
		color(C_DIM), color(C_RESET));
	if (eligible == 0) {
		printf("%sWaiting for RT metrics on %s ...%s\n",
			color(C_YELLOW), socket_name, color(C_RESET));
		printf("Start the workload with: tgbs-demo-mixed start\n");
	} else {
		for (unsigned int i = 0; i < eligible; i++) {
			if (interactive && 3 + (displayed + 1) *
			    (graph_height + 3) > rows) {
				printf("%s... more tasks hidden; enlarge the terminal%s\n",
					color(C_DIM), color(C_RESET));
				break;
			}
			render_task(visible[i], graph_width, graph_height);
			putchar('\n');
			displayed++;
		}
	}
	fflush(stdout);
}

static void usage(FILE *stream, const char *program)
{
	fprintf(stream,
		"Usage: %s [OPTIONS]\n\n"
		"Display live response-time timelines for tgbs-demo-mixed RT tasks.\n"
		"It can be started before or after the mixed workload.\n\n"
		"  -r, --refresh MS   screen refresh period (default: 200)\n"
		"  -b, --batch        plain output without terminal control\n"
		"      --no-color     disable ANSI colors\n"
		"  -h, --help         show this help\n",
		program);
}

int main(int argc, char **argv)
{
	static const struct option options[] = {
		{ "refresh", required_argument, NULL, 'r' },
		{ "batch", no_argument, NULL, 'b' },
		{ "no-color", no_argument, NULL, 1000 },
		{ "help", no_argument, NULL, 'h' },
		{ NULL, 0, NULL, 0 }
	};
	int refresh_ms = 200;
	bool batch = false;
	bool interactive;
	int socket_fd;
	int option;
	const char *override;

	while ((option = getopt_long(argc, argv, "r:bh", options, NULL)) != -1) {
		switch (option) {
		case 'r': {
			char *end;
			long value;
			errno = 0;
			value = strtol(optarg, &end, 10);
			if (errno != 0 || end == optarg || *end != '\0' ||
			    value < 50 || value > 10000) {
				fprintf(stderr, "tgbs-demo-mixed-timeline: invalid refresh: %s\n",
					optarg);
				return 2;
			}
			refresh_ms = (int)value;
			break;
		}
		case 'b': batch = true; break;
		case 1000: use_color = false; break;
		case 'h': usage(stdout, argv[0]); return 0;
		default: usage(stderr, argv[0]); return 2;
		}
	}
	if (optind != argc) {
		usage(stderr, argv[0]);
		return 2;
	}
	override = getenv("TGBS_MIXED_METRICS_SOCKET");
	if (override != NULL && override[0] != '\0')
		socket_name = override;
	override = getenv("TGBS_MIXED_STATE_DIR");
	if (override != NULL && override[0] == '/')
		state_dir = override;
	socket_fd = bind_metrics_socket(socket_name);
	if (socket_fd < 0) {
		fprintf(stderr, "tgbs-demo-mixed-timeline: cannot listen on %s: %s\n",
			socket_name, strerror(errno));
		return 1;
	}
	interactive = !batch && enable_interactive_terminal();
	if (!interactive)
		use_color = false;
	atexit(restore_terminal);
	signal(SIGINT, signal_handler);
	signal(SIGTERM, signal_handler);
	signal(SIGHUP, signal_handler);

	render(interactive);
	while (!stop_requested) {
		struct pollfd descriptors[2] = {
			{ .fd = socket_fd, .events = POLLIN },
			{ .fd = STDIN_FILENO, .events = interactive ? POLLIN : 0 }
		};
		int ready = poll(descriptors, 2, refresh_ms);
		if (ready < 0 && errno != EINTR)
			break;
		if (ready > 0 && (descriptors[0].revents & POLLIN))
			drain_socket(socket_fd);
		if (ready > 0 && (descriptors[1].revents & POLLIN)) {
			char key;
			if (read(STDIN_FILENO, &key, 1) == 1 &&
			    (key == 'q' || key == 'Q'))
				break;
		}
		render(interactive);
		if (!interactive && batch)
			fflush(stdout);
	}
	close(socket_fd);
	if (interactive)
		printf("\033[0m\n");
	return 0;
}

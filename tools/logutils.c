#include "shared.h"
#include "logutils.h"
#include <sys/types.h>
#include <dirent.h>
#include <stdarg.h>
#include <glib.h>

/* stubs required for linking */
int ipc_grok_var(__attribute__((unused)) char *var, __attribute__((unused)) char *val) { return 0; }

/* global variables used in all log-handling apps */
char **strv = NULL;
int num_nfile = 0;
static int nfile_alloc = 0;
struct naglog_file *nfile;
int logs_debug_level = 0;
struct naglog_file *cur_file; /* the file we're currently importing */
uint line_no = 0;
uint num_unhandled = 0;
uint warnings = 0;
static GHashTable *interesting_hosts, *interesting_services;

#define state_code(S) { 0, #S, sizeof(#S) - 1, STATE_##S }
static struct string_code host_state[] = {
	state_code(UP),
	state_code(DOWN),
	state_code(UNREACHABLE),
};

static struct string_code service_state[] = {
	state_code(OK),
	state_code(WARNING),
	state_code(CRITICAL),
	state_code(UNKNOWN),
};

#define notification_code(S) { 0, #S, sizeof(#S) - 1, NOTIFICATION_##S }
static struct string_code notification_reason[] = {
	notification_code(ACKNOWLEDGEMENT),
	notification_code(FLAPPINGSTART),
	notification_code(DOWNTIMESTART),
	notification_code(FLAPPINGSTOP),
	notification_code(FLAPPINGDISABLED),
	notification_code(DOWNTIMESTART),
	notification_code(DOWNTIMEEND),
	notification_code(DOWNTIMECANCELLED),
	notification_code(CUSTOM),
};

/*** general utility functions ***/
void __attribute__((__noreturn__)) lp_crash(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fputc('\n', stderr);

	if (cur_file) {
		fprintf(stderr, "crash() called when parsing line %u in %s\n",
		        line_no, cur_file->path);
	}

	exit(1);
}

void pdebug(int lvl, const char *fmt, ...)
{
	va_list ap;

	if (logs_debug_level < lvl)
		return;

	va_start(ap, fmt);
	vprintf(fmt, ap);
	va_end(ap);
	if (fmt[strlen(fmt) - 1] != '\n')
		putchar('\n');
}

void warn(const char *fmt, ...)
{
	va_list ap;

	warnings++;

	if (!logs_debug_level)
		return;

	printf("WARNING: ");
	va_start(ap, fmt);
	vprintf(fmt, ap);
	va_end(ap);
	putchar('\n');

}

int parse_service_state_gently(const char *str)
{
	uint i;

	for (i = 0; i < ARRAY_SIZE(service_state); i++) {
		if (!strcmp(str, service_state[i].str))
			return service_state[i].code;
	}

	return -1;
}

int parse_service_state(const char *str)
{
	int ret = parse_service_state_gently(str);

	if (ret < 0)
		lp_crash("bad value for service state: '%s'", str);

	return ret;
}

int parse_host_state_gently(const char *str)
{
	uint i;

	for (i = 0; i < ARRAY_SIZE(host_state); i++) {
		if (!strcmp(str, host_state[i].str))
			return host_state[i].code;
	}

	return -1;
}

int parse_host_state(const char *str)
{
	int ret = parse_host_state_gently(str);

	if (ret < 0)
		lp_crash("bad value for host state: '%s'", str);

	return ret;
}

int parse_notification_reason(const char *str)
{
	uint i;

	for (i = 0; i < ARRAY_SIZE(notification_reason); i++) {
		if (!prefixcmp(str, notification_reason[i].str))
			return notification_reason[i].code;
	}

	return NOTIFICATION_NORMAL;
}

int soft_hard(const char *str)
{
	if (!strcmp(str, "HARD"))
		return HARD_STATE;

	if (!strcmp(str, "SOFT"))
		return SOFT_STATE;
	lp_crash("wtf kind of value is '%s' to determine 'soft' or 'hard' from?", str);
}

/*
 * prints the objects we consider interesting
 */
void print_interesting_objects(void)
{
	GHashTableIter iter;
	gpointer key, value;

	if (interesting_hosts) {
		printf("\nInteresting hosts:\n");
		g_hash_table_iter_init(&iter, interesting_hosts);
		while (g_hash_table_iter_next(&iter, &key, &value)) {
			const char *str = value;
			printf("%p: %s\n", str, str);
		}
	}
	if (interesting_services) {
		printf("\nInteresting services:\n");
		g_hash_table_iter_init(&iter, interesting_services);
		while (g_hash_table_iter_next(&iter, &key, &value)) {
			const char *str = value;
			printf("%p: %s\n", str, str);
		}
	}
	if (interesting_hosts || interesting_services)
		putchar('\n');
}

/* marks one object as 'interesting' */
int add_interesting_object(const char *str)
{
	char *semi_colon;

	semi_colon = strchr(str, ';');
	if (!semi_colon) {
		if (!interesting_hosts)
			interesting_hosts = g_hash_table_new_full(g_str_hash, g_str_equal,
					free, free);
		g_hash_table_insert(interesting_hosts, strdup(str), strdup(str));
	} else {
		if (!interesting_services)
			interesting_services = g_hash_table_new_full(nm_service_hash, nm_service_equal,
					(GDestroyNotify) nm_service_key_destroy, free);
		*semi_colon++ = 0;
		g_hash_table_insert(interesting_services, nm_service_key_create(str, semi_colon), strdup(str));
	}

	return 0;
}

int is_interesting_host(const char *host)
{
	if (interesting_hosts)
		return g_hash_table_lookup(interesting_hosts, host) == NULL ? 0 : 1;

	return 1;
}

int is_interesting_service(const char *host, const char *service)
{
	/* fall back to checking if host is interesting */
	if (!service || !interesting_services)
		return is_interesting_host(host);

	return g_hash_table_lookup(interesting_services, &((nm_service_key) {(char *) host,
				(char *)service})) == NULL ? 0 : 1;
}

struct unhandled_event {
	char *file;
	char *line;
	unsigned line_no;
	unsigned long repeated;
	struct unhandled_event *next;
};

static struct unhandled_event *event_list;
/*
 * This is a fairly toothless function, since we can encounter
 * pretty much any kind of message in the logfiles. In order to
 * make sure we don't miss anything important though, we stash
 * the messages and print them at the end if we're debugging.
 */
void handle_unknown_event(const char *line)
{
	struct unhandled_event *event;
	static struct unhandled_event *last = NULL;

	num_unhandled++;

	if (last && !strncmp(&line[14], &last->line[14], 20)) {
		last->repeated++;
		return;
	}

	/* add to top of list. we'll print in reverse order */
	if (last) {
		/* add to "top" of list. we'll print in reverse order */
		last->next = event_list;
		event_list = last;
		last = NULL;
	}

	if (!(event = calloc(1, sizeof(*event))) || !(event->line = strdup(line))) {
		lp_crash("Failed to allocate memory for unhandled event [%s]\n", line);
		return;
	}

	event->line_no = line_no;
	event->file = cur_file->path;
	last = event;
}

void print_unhandled_events(void)
{
	struct unhandled_event *event;
	uint x = 1;

	if (!num_unhandled)
		return;

	/*
	 * add the fake closing event so we get the last
	 * real event added to the list. The fake one won't
	 * get added though, so we needn't bother with it.
	 */
	handle_unknown_event("Fake unhandled event");

	/*
	 * fake message bumps counter, so we decrease it here
	 * to get an accurate count
	 */
	num_unhandled--;

	printf("\n%u Unhandled events encountered:\n" \
	       "------------------------------", num_unhandled);

	for (x = 1; num_unhandled > (x * 10); x *= 10)
		putchar('-');

	putchar('\n');
	for (event = event_list; event; event = event->next) {
		printf("%s:%d:\n%s\n", event->file, event->line_no, event->line);
		if (event->repeated) {
			printf("  #### Similar events repeated %lu times\n", event->repeated);
		}
		puts("----");
	}
}

int vectorize_string(char *str, int nvecs)
{
	char *p;
	int i = 0;

	strv[i++] = str;
	for (p = str; *p && i < nvecs; p++) {
		if (*p == ';') {
			*p = 0;
			strv[i++] = p+1;
		}
	}

	return i;
}

/*
 * This takes care of lines that have been field-separated at
 * semi-colons and passes it to the function above.
 */
char *devectorize_string(char **ary, int nvecs)
{
	int i;

	for (i = 1; i < nvecs; i++) {
		/* the char before the first char in the string
		 * in our array is the one where we replaced a
		 * semi-colon with a nul char, so the math here
		 * is actually correct.
		 */
		ary[i][-1] = ';';
	}

	return *ary;
}

struct string_code *
get_string_code(struct string_code *codes, const char *str, uint len)
{
	int i;

	for (i = 0; codes[i].str; i++)
		if (codes[i].len == len && !memcmp(str, codes[i].str, len))
			return &codes[i];

	return NULL;
}

int is_interesting(const char *ptr)
{
	if (!prefixcmp(ptr, "Auto-save of retention data"))
		return 0;
	if (!prefixcmp(ptr, "Event broker module"))
		return 0;
	if (!prefixcmp(ptr, "You do not have permission"))
		return 0;
	if (!prefixcmp(ptr, "Local time is"))
		return 0;

	return 1;
}

int is_start_event(const char *ptr)
{
	if (!prefixcmp(ptr, "Finished daemonizing..."))
		return 1;
	if (!prefixcmp(ptr, "Caught SIGHUP"))
		return 1;
	if (strstr(ptr, "starting..."))
		return 1;

	return 0;
}

int is_stop_event(const char *ptr)
{
	if (!prefixcmp(ptr, "PROGRAM_RESTART"))
		return 1;
	if (!prefixcmp(ptr, "Caught SIGTERM"))
		return 1;
	if (!prefixcmp(ptr, "Successfully shutdown..."))
		return 1;
	if (!prefixcmp(ptr, "Bailing out"))
		return 1;
	if (!prefixcmp(ptr, "Lockfile"))
		return 1;
	if (strstr(ptr, "shutting down..."))
		return 1;

	return 0;
}

int strtotimet(const char *str, time_t *val)
{
	char *endp;

	*val = strtoul(str, &endp, 10);
	if (endp == str) {
		warn("strtotimet(): %s is not a valid time_t\n", str);
		return -1;
	}

	return 0;
}

void first_log_time(struct naglog_file *nf)
{
	int fd;
	int i = 0;
	char buf[1024];
	int read_len = 0;
	struct stat st;

	memset(buf, 0, sizeof(buf));
	if (!(fd = open(nf->path, O_RDONLY)))
		lp_crash("Failed to open %s: %s", nf->path, strerror(errno));

	/*
	 * since we're looking at every file in here anyway,
	 * we also determine the size of them so we can do an
	 * arena allocation large enough to fit the largest
	 * file + an added newline later
	 */
	if (fstat(fd, &st) < 0)
		lp_crash("Failed to stat %s: %s", nf->path, strerror(errno));

	nf->size = st.st_size;

	if ((read_len = read(fd, buf, sizeof(buf))) < min((int)sizeof(buf), st.st_size))
		lp_crash("Incomplete read of %s", nf->path);

	buf[sizeof(buf) - 1] = 0;
	/* skip empty lines at top of file */
	while (i < read_len - 12 && (buf[i] == '\n' || buf[i] == '\r'))
		i++;

	if (strtotimet(buf + i + 1, &nf->first))
		lp_crash("'%s' has no timestamp for us to parse", buf);

	nf->cmp = nf->first;
	close(fd);
}

/*
 * sort function for logfiles. Sorts based on first logged timestamp
 */
int nfile_cmp(const void *p1, const void *p2)
{
	const struct naglog_file *a = p1;
	const struct naglog_file *b = p2;

	return a->first - b->first;
}

/* same as above, but sorts in reverse order */
int nfile_rev_cmp(const void *p1, const void *p2)
{
	const struct naglog_file *a = p1;
	const struct naglog_file *b = p2;

	return b->first - a->first;
}

#ifndef PATH_MAX
# define PATH_MAX 4096
#endif
/* recurse into a log-archive path and find all logfiles there */
static int add_naglog_directory(const char *dir)
{
	char path[PATH_MAX];
	DIR *dirp;
	struct dirent *de;
	uint dlen = strlen(dir);

	dirp = opendir(dir);
	if (!dirp)
		crash("Failed to opendir(%s): %s\n", dir, strerror(errno));

	memcpy(path, dir, dlen);
	path[dlen++] = '/';
	while ((de = readdir(dirp))) {
		unsigned int name_len;
		path[dlen] = 0;
		if (prefixcmp(de->d_name, "nagios") && prefixcmp(de->d_name, "naemon"))
			continue;
		name_len = strlen(de->d_name);
		if (name_len < 10) /* naemon.log = 10 chars */
			continue;
		if (!strstr(de->d_name, ".log"))
			continue;

		/* build some sort of path to the file */
		memcpy(&path[dlen], de->d_name, name_len);
		path[dlen + name_len] = 0;
		add_naglog_path(path);
	}
	closedir(dirp);
	return 0;
}

/* Handles both files and directories */
int add_naglog_path(char *path)
{
	struct stat st;
	int i;

	/* make sure we never add duplicate files */
	for (i = 0; i < num_nfile; i++) {
		if (!strcmp(nfile[i].path, path))
			return -1;
	}

	if (stat(path, &st) < 0) {
		lp_crash("Failed to stat '%s': %s", path, strerror(errno));
	}
	if (S_ISDIR(st.st_mode)) {
		return add_naglog_directory(path);
	}

	if (num_nfile >= nfile_alloc - 1) {
		nfile_alloc += 20;
		nfile = realloc(nfile, nfile_alloc * sizeof(*nfile));
	}

	nfile[num_nfile].path = strdup(path);
	first_log_time(&nfile[num_nfile]);
	num_nfile++;

	return 0;
}

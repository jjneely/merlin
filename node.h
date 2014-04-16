#ifndef INCLUDE_node_h__
#define INCLUDE_node_h__

#include <sys/types.h>
#include <netinet/in.h>
#include <sys/time.h>
#include <nagios/lib/iocache.h>
#include <nagios/nebcallbacks.h>
#include "cfgfile.h"
#include "binlog.h"
#include "pgroup.h"
#include "ctrl.pb-c.h"
#include "merlin.pb-c.h"

#if __BYTE_ORDER == __BIG_ENDIAN
# define MERLIN_SIGNATURE (uint64_t)0x4d524c4e45565400LL /* "MRLNEVT\0" */
#else
# define MERLIN_SIGNATURE (uint64_t)0x005456454e4c524dLL /* "MRLNEVT\0" */
#endif

#define MERLIN_PROTOCOL_VERSION 1
/*
 * how long we should wait before sending paths before trying
 * to send them again in case something weird happens
 */
#define MERLIN_SENDPATH_INTERVAL 15

/*
 * flags for node options. Must be powers of 2
 */
#define MERLIN_NODE_TAKEOVER (1 << 0)
#define MERLIN_NODE_CONNECT  (1 << 1)
#define MERLIN_NODE_FIXED_SRCPORT (1 << 2)
#define MERLIN_NODE_NOTIFIES (1 << 3)

#define MERLIN_NODE_DEFAULT_POLLER_FLAGS \
		(MERLIN_NODE_TAKEOVER | MERLIN_NODE_CONNECT | MERLIN_NODE_NOTIFIES)
#define MERLIN_NODE_DEFAULT_PEER_FLAGS (MERLIN_NODE_CONNECT)
#define MERLIN_NODE_DEFAULT_MASTER_FLAGS (MERLIN_NODE_CONNECT)
#define MERLIN_NODE_DEFAULT_IPC_FLAGS (MERLIN_NODE_NOTIFIES)

/* various magic options for the "type" field */
#define CTRL_PACKET   0xffff  /* control packet. "code" described below */
#define ACK_PACKET    0xfffe  /* ACK ("I understood") (not used) */
#define NAK_PACKET    0xfffd  /* NAK ("I don't understand") (not used) */

/* If "type" is CTRL_PACKET, then "code" is one of the following */
#define CTRL_GENERIC  MERLIN_CTRL_PACKET__CODE__CTRL_GENERIC /* generic control packet */
#define CTRL_PULSE    MERLIN_CTRL_PACKET__CODE__CTRL_PULSE /* keep-alive signal */
#define CTRL_INACTIVE MERLIN_CTRL_PACKET__CODE__CTRL_INACTIVE/* signals that a slave went offline */
#define CTRL_ACTIVE   MERLIN_CTRL_PACKET__CODE__CTRL_ACTIVE   /* signals that a slave went online */
#define CTRL_PATHS    MERLIN_CTRL_PACKET__CODE__CTRL_PATHS /* body contains paths to import */
#define CTRL_STALL    MERLIN_CTRL_PACKET__CODE__CTRL_STALL /* signal that we can't accept events for a while */
#define CTRL_RESUME   MERLIN_CTRL_PACKET__CODE__CTRL_RESUME /* now we can accept events again */
#define CTRL_STOP     MERLIN_CTRL_PACKET__CODE__CTRL_STOP  /* exit() immediately (only accepted via ipc) */
/* the following magic entries can be used for the "code" entry */
#define MAGIC_NONET 0xffff /* don't forward to the network */

/*
 * Mark "selection" with this to generate broadcast-ish
 * events for various classes of merlin-nodes
 */
#define DEST_BROADCAST  0xffff /* all nodes */
#define DEST_MAGIC (0xfff0)
#define DEST_POLLERS (DEST_MAGIC + (1 << 1)) /* all pollers */
#define DEST_PEERS   (DEST_MAGIC + (1 << 2)) /* all peers */
#define DEST_MASTERS (DEST_MAGIC + (1 << 3)) /* all masters */
#define DEST_PEERS_POLLERS (DEST_POLLERS | DEST_PEERS)
#define DEST_PEERS_MASTERS (DEST_PEERS | DEST_MASTERS)
#define DEST_POLLERS_MASTERS (DEST_POLLERS | DEST_MASTERS)
#define magic_destination(pkt) ((pkt->hdr.selection & 0xfff0) == 0xfff0)


#define HDR_SIZE (sizeof(merlin_header))
#define PKT_SIZE (sizeof(merlin_event))
#define MAX_PKT_SIZE (PKT_SIZE)
#define packet_size(pkt) ((int)((pkt)->hdr.len + HDR_SIZE))

struct merlin_header {
	union merlin_signature {
		uint64_t id;     /* used for assignment and comparison */
		char ascii[8];   /* "MRLNEVT\0" for debugging, mostly */
	} sig;
	uint16_t protocol;   /* protocol version */
	uint16_t type;       /* event type */
	uint16_t code;       /* event code (used for control packets) */
	uint16_t selection;  /* used when noc Nagios communicates with mrd */
	uint32_t len;        /* size of body */
	struct timeval sent;  /* when this message was sent */

	/* pad to 64 bytes for future extensions */
	char padding[64 - sizeof(struct timeval) - (2 * 6) - 8];
} __attribute__((packed));
typedef struct merlin_header merlin_header;

struct merlin_event {
	merlin_header hdr;
	char body[128 << 10];
} __attribute__((packed));
typedef struct merlin_event merlin_event;

/*
 * New entries in this struct *must* be appended LAST for the change
 * to not break backwards compatibility. When the receiving code
 * expects a new entry from this struct to exist, it should take care
 * to mark the size of the received packet and never access anything
 * beyond it.
 * Since it gets copied into pre-allocated memory, we needn't bother
 * about the fields we never set. Newly added fields just shouldn't
 * ever have an expected value of 0 when it exists, since that's what
 * they will be if the sending end doesn't have the field we want to
 * know about.
 * Thus, we must make sure to always use fixed-size entries in this
 * struct.
 */
/* change this macro when nodeinfo is rearranged */
#define MERLIN_NODEINFO_VERSION 1
 /* change this macro when the struct grows incompatibly */
#define MERLIN_NODEINFO_MINSIZE sizeof(struct merlin_nodeinfo)
struct merlin_nodeinfo {
	uint32_t version;       /* version of this structure */
	uint32_t word_size;     /* bits per register (sizeof(void *) * 8) */
	uint32_t byte_order;    /* 1234 = little, 4321 = big, ... */
	uint32_t object_structure_version;
	struct timeval start;   /* module (or daemon) start time */
	time_t last_cfg_change; /* when config was last changed */
	unsigned char config_hash[20];   /* SHA1 hash of object config hash */
	uint32_t peer_id;       /* self-assigned peer-id */
	uint32_t active_peers;
	uint32_t configured_peers;
	uint32_t active_pollers;
	uint32_t configured_pollers;
	uint32_t active_masters;
	uint32_t configured_masters;
	uint32_t host_checks_handled;
	uint32_t service_checks_handled;
	uint32_t monitored_object_state_size;
	/* new entries have to come LAST */
} __attribute__((packed));
typedef struct merlin_nodeinfo merlin_nodeinfo;

struct merlin_child {
	char *cmd;
	int pid;
};
typedef struct merlin_child merlin_child;

struct merlin_confsync {
	int configured;
	merlin_child push;
	merlin_child fetch;
};
typedef struct merlin_confsync merlin_confsync;

/* 1MB receive buffer should work nicely */
#define MERLIN_IOC_BUFSIZE (1 * 1024 * 1024)

struct statistics_vars {
	unsigned long long sent, read, logged, dropped;
};
struct callback_count {
	unsigned int in, out;
};
struct merlin_node_stats {
	struct statistics_vars events, bytes;
	time_t last_logged;     /* when we logged the event-count last */
	struct callback_count cb_count[NEBCALLBACK_NUMITEMS + 1];
};
typedef struct merlin_node_stats merlin_node_stats;

/* used for various objects which we build linked lists for */
typedef struct linked_item {
	void *item;
	struct linked_item *next_item;
} linked_item;

struct node_selection {
	int id;
	char *name;
	linked_item *nodes;
};
typedef struct node_selection node_selection;

/* for node->type */
#define MODE_LOCAL     0
#define MODE_NOC       1
#define MODE_MASTER    MODE_NOC /* alias for MODE_NOC */
#define MODE_PEER      (1 << 1)
#define MODE_POLLER    (1 << 2)
#define MODE_INTERNAL  (1 << 3)

/* for node->state */
#define STATE_NONE 0
#define STATE_PENDING 1
#define STATE_NEGOTIATING 2
#define STATE_CONNECTED 3

#define NODE_WARN_CLOCK 1   /* clock skew warning */

struct merlin_node {
	char *name;             /* name of this node */
	char *source_name;      /* check source name for this node */
	char *hostgroups;       /* only used for pollers */
	uint id;                /* internal index lookup number */
	int latency;            /* module to module latency of this node */
	int sock;               /* the socket */
	int type;               /* server type (master, slave, peer) */
	int state;              /* state of this node (down, pending, active) */
	uint32_t peer_id;       /* peer id, used to distribute checks */
	int flags;              /* flags for this node */
	struct sockaddr *sa;    /* should always point to sain */
	struct sockaddr_in sain;
	unsigned int data_timeout; /* send gracetime before we disconnect */
	unsigned int host_checks; /* actually executed host checks */
	unsigned int service_checks; /* actually executed service checks */
	unsigned int warn_flags; /* warnings caught from this node */
	time_t last_recv;       /* last time node sent something to us */
	time_t last_sent;       /* when we sent something last */
	time_t last_conn_attempt_logged; /* when we last logged a connect attempt */
	time_t last_conn_attempt; /* when we last tried initiating a connection */
	time_t connect_time;    /* when we established a connection to this node */
	merlin_peer_group *pgroup; /* this node's peer-group (if a poller) */
	struct {
		struct merlin_assigned_objects passive; /* passive check modifiers */
		struct merlin_assigned_objects extra; /* taken over from a poller */
		struct merlin_assigned_objects current; /* base assigned right now */
		struct merlin_assigned_objects expired; /* expired checks */
	} assigned;
	merlin_nodeinfo info;   /* node info */
	int last_action;        /* LA_CONNECT | LA_DISCONNECT | LA_HANDLED */
	binlog *binlog;         /* binary backlog for this node */
	merlin_node_stats stats; /* event/data statistics */
	iocache *ioc;     /* I/O cache for bulk reads */
	merlin_confsync csync; /* config synchronization configuration */
	unsigned int csync_num_attempts;
	unsigned int csync_max_attempts;
	time_t csync_last_attempt;
	int (*action)(struct merlin_node *, int); /* (daemon) action handler */
};
typedef struct merlin_node merlin_node;

#define node_table noc_table
extern merlin_node **noc_table, **peer_table, **poller_table;

extern node_selection *node_selection_by_name(const char *name);
extern char *get_sel_name(int index);
extern int get_sel_id(const char *name);
extern int get_num_selections(void);
extern linked_item *nodes_by_sel_id(int sel);
extern linked_item *nodes_by_sel_name(const char *name);
extern void node_grok_config(struct cfg_comp *config);
extern void node_log_event_count(merlin_node *node, int force);
extern void node_disconnect(merlin_node *node, const char *fmt, ...);
extern int node_send(merlin_node *node, void *data, unsigned int len, int flags);
extern int node_send_event(merlin_node *node, merlin_event *pkt, int msec);
int node_send_message(merlin_node *node, const MerlinMessage *message, int flags);
extern int node_recv(merlin_node *node);
extern merlin_event *node_get_event(merlin_node *node);
extern int node_send_binlog(merlin_node *node, merlin_event *pkt);
extern const char *node_state(merlin_node *node);
extern const char *node_type(merlin_node *node);
extern void node_set_state(merlin_node *node, int state, const char *reason);
extern int node_ctrl(merlin_node *node, int code, uint selection, merlin_nodeinfo *data, int msec);
extern merlin_node *node_by_id(uint id);

/*
 * we make these inlined rather than macros so the compiler
 * does type-checking in the arguments
 */
static inline int node_send_ctrl_inactive(merlin_node *node, uint id, int msec)
{
	return node_ctrl(node, CTRL_INACTIVE, id, NULL, msec);
}

static inline int node_send_ctrl_active(merlin_node *node, uint id, merlin_nodeinfo *info, int msec)
{
	return node_ctrl(node, CTRL_ACTIVE, id, info, msec);
}
#endif

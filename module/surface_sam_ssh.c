// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Surface Serial Hub (SSH) driver for communication with the Surface/System
 * Aggregator Module.
 */

#include <asm/unaligned.h>
#include <linux/acpi.h>
#include <linux/atomic.h>
#include <linux/completion.h>
#include <linux/crc-ccitt.h>
#include <linux/dmaengine.h>
#include <linux/gpio/consumer.h>
#include <linux/interrupt.h>
#include <linux/jiffies.h>
#include <linux/kernel.h>
#include <linux/kthread.h>
#include <linux/kfifo.h>
#include <linux/kref.h>
#include <linux/list.h>
#include <linux/mutex.h>
#include <linux/notifier.h>
#include <linux/pm.h>
#include <linux/refcount.h>
#include <linux/serdev.h>
#include <linux/spinlock.h>
#include <linux/workqueue.h>

#include "surface_sam_ssh.h"


#define SSH_RQST_TAG_FULL			"surface_sam_ssh_rqst: "
#define SSH_RQST_TAG				"rqst: "
#define SSH_EVENT_TAG				"event: "

#define SSH_SUPPORTED_FLOW_CONTROL_MASK		(~((u8) ACPI_UART_FLOW_CONTROL_HW))

#define SSH_BYTELEN_SYNC			2	// [0xAA, 0x55]
#define SSH_BYTELEN_TERM			2	// [0xFF, 0xFF]
#define SSH_BYTELEN_CRC				2
#define SSH_BYTELEN_CTRL			4	// command-header, ACK, or RETRY
#define SSH_BYTELEN_CMDFRAME			8	// without payload

// TODO: this doesn't hold any more
#define SSH_MAX_WRITE (				\
	  SSH_BYTELEN_SYNC			\
	+ SSH_BYTELEN_CTRL			\
	+ SSH_BYTELEN_CRC			\
	+ SSH_BYTELEN_CMDFRAME			\
	+ SURFACE_SAM_SSH_MAX_RQST_PAYLOAD	\
	+ SSH_BYTELEN_CRC			\
)

#define SSH_TX_TIMEOUT			MAX_SCHEDULE_TIMEOUT
#define SSH_RX_TIMEOUT			msecs_to_jiffies(1000)
#define SSH_NUM_RETRY			3

#define SSH_READ_BUF_LEN		4096		// must be power of 2
#define SSH_RECV_BUF_LEN		4096		// must be power of 2
#define SSH_EVAL_BUF_LEN		SSH_MAX_WRITE	// also works for reading


/* -- Data structures for SAM-over-SSH communication. ----------------------- */

/**
 * enum ssh_frame_type - Frame types for SSH frames.
 * @SSH_FRAME_TYPE_DATA_SEQ: Indicates a data frame, followed by a payload with
 *                      the length specified in the ssh_frame.len field. This
 *                      frame is sequenced, meaning that an ACK is required.
 * @SSH_FRAME_TYPE_DATA_NSQ: Same as SSH_FRAME_TYPE_DATA_SEQ, but unsequenced,
 *                      meaning that the message does not have to be ACKed.
 * @SSH_FRAME_TYPE_ACK: Indicates an ACK message.
 * @SSH_FRAME_TYPE_NAK: Indicates an error response for previously sent
 *                      frame. In general, this means that the frame and/or
 *                      payload is malformed, e.g. a CRC is wrong. For command-
 *                      type payloads, this can also mean that the command is
 *                      invalid.
 */
enum ssh_frame_type {
	SSH_FRAME_TYPE_DATA_SEQ = 0x80,
	SSH_FRAME_TYPE_DATA_NSQ = 0x00,
	SSH_FRAME_TYPE_ACK	= 0x40,
	SSH_FRAME_TYPE_NAK	= 0x04,
};

/**
 * struct ssh_frame - SSH communication frame.
 * @type: The type of the frame. See &enum ssh_frame_type.
 * @len:  The length of the frame payload directly following the CRC for this
 *        frame. Does not include the final CRC for that payload.
 * @seq:  The sequence number for this message/exchange.
 */
struct ssh_frame {
	u8 type;
	__le16 len;
	u8 seq;
} __packed;

static_assert(sizeof(struct ssh_frame) == 4);

/**
 * enum ssh_payload_type - Type indicator for the SSH payload.
 * @SSH_PLD_TYPE_CMD: The payload is a command structure with optional command
 *                    payload.
 */
enum ssh_payload_type {
	SSH_PLD_TYPE_CMD = 0x80,
};

/**
 * struct ssh_command - Payload of a command-type frame.
 * @type:    The type of the payload. See &enum ssh_payload_type. Should be
 *           SSH_PLD_TYPE_CMD for this struct.
 * @tc:      Command target category.
 * @pri_out: Output priority. Should be zero if this an incoming (EC to host)
 *           message.
 * @pri_in:  Input priority. Should be zero if this is an outgoing (hos to EC)
 *           message.
 * @iid:     Instance ID.
 * @rqid:    Request ID. Used to match requests with responses and differentiate
 *           between responses and events.
 * @cid:     Command ID.
 */
struct ssh_command {
	u8 type;
	u8 tc;
	u8 pri_out;
	u8 pri_in;
	u8 iid;
	__le16 rqid;
	u8 cid;
} __packed;

static_assert(sizeof(struct ssh_command) == 8);

/**
 * Syncrhonization (SYN) bytes.
 */
#define SSH_MSG_SYN		((u16)0x55aa)

/**
 * Base-length of a SSH message. This is the minimum number of bytes required
 * to form a message. The actual message length is SSH_MSG_LEN_BASE plus the
 * length of the frame payload.
 */
#define SSH_MSG_LEN_BASE	(sizeof(struct ssh_frame) + 3 * sizeof(u16))

/**
 * Length of a control message.
 */
#define SSH_MSG_LEN_CTRL	SSH_MSG_LEN_BASE


/* -- TODO ------------------------------------------------------------------ */

enum ssh_ec_state {
	SSH_EC_UNINITIALIZED,
	SSH_EC_INITIALIZED,
	SSH_EC_SUSPENDED,
};

struct ssh_counters {
	u8  seq;		// control sequence id
	u16 rqid;		// id for request/response matching
};

enum ssh_receiver_state {
	SSH_RCV_DISCARD,
	SSH_RCV_CONTROL,
	SSH_RCV_COMMAND,
};

struct ssh_receiver {
	spinlock_t lock;
	enum ssh_receiver_state state;
	struct completion signal;
	struct kfifo fifo;
	struct kfifo rcvb;
	struct wait_queue_head rcvb_wq;
	struct task_struct *thread;
	struct {
		bool pld;
		u8 seq;
		u16 rqid;
	} expect;
	struct {
		u16 cap;
		u16 len;
		u8 *ptr;
	} eval_buf;
};

struct ssh_events {
	struct workqueue_struct *queue_ack;
	struct workqueue_struct *queue_evt;
	struct srcu_notifier_head notifier[SURFACE_SAM_SSH_MAX_EVENT_ID];
	int notifier_count[SURFACE_SAM_SSH_MAX_EVENT_ID];
};

struct sam_ssh_ec {
	struct mutex lock;
	enum ssh_ec_state state;
	struct serdev_device *serdev;
	struct ssh_counters counter;
	struct ssh_receiver receiver;
	struct ssh_events events;
	int irq;
	bool irq_wakeup_enabled;
};

struct ssh_fifo_packet {
	u8 type;	// packet type (ACK/RETRY/CMD)
	u8 seq;
	u8 len;
};

struct ssh_event_work {
	refcount_t refcount;
	struct sam_ssh_ec *ec;
	struct work_struct work_ack;
	struct work_struct work_evt;
	struct surface_sam_ssh_event event;
	u8 seq;
};


static struct sam_ssh_ec ssh_ec = {
	.lock   = __MUTEX_INITIALIZER(ssh_ec.lock),
	.state  = SSH_EC_UNINITIALIZED,
	.serdev = NULL,
	.counter = {
		.seq  = 0,
		.rqid = 0,
	},
	.receiver = {
		.lock = __SPIN_LOCK_UNLOCKED(),
		.state = SSH_RCV_DISCARD,
		.expect = {},
	},
	.events = {
		.notifier_count = { 0 },
	},
	.irq = -1,
};


/* -- Common/utility functions. --------------------------------------------- */

#define ssh_dbg(ec, fmt, ...)  dev_dbg(&ec->serdev->dev, fmt, ##__VA_ARGS__)
#define ssh_warn(ec, fmt, ...) dev_warn(&ec->serdev->dev, fmt, ##__VA_ARGS__)
#define ssh_err(ec, fmt, ...)  dev_err(&ec->serdev->dev, fmt, ##__VA_ARGS__)

static inline u16 ssh_crc(const u8 *buf, size_t len)
{
	return crc_ccitt_false(0xffff, buf, len);
}

static inline u16 ssh_rqid_next(u16 rqid)
{
	return rqid > 0 ? rqid + 1 : rqid + SURFACE_SAM_SSH_MAX_EVENT_ID + 1;
}

static inline u16 ssh_event_to_rqid(u16 event)
{
	return event + 1;
}

static inline u16 ssh_rqid_to_event(u16 rqid)
{
	return rqid - 1;
}

static inline bool ssh_rqid_is_event(u16 rqid)
{
	return ssh_rqid_to_event(rqid) < SURFACE_SAM_SSH_MAX_EVENT_ID;
}

static inline int ssh_tc_to_event(u8 tc)
{
	/*
	 * TC=0x08 represents the input subsystem on Surface Laptop 1 and 2.
	 * This is mapped on Windows to RQID=0x0001. As input events seem to be
	 * somewhat special with regards to enabling/disabling (they seem to be
	 * enabled by default with a fixed RQID), let's do the same here.
	 */
	if (tc == 0x08)
		return ssh_rqid_to_event(0x0001);

	/* Default path: Set RQID = TC. */
	return ssh_rqid_to_event(tc);
}

static inline u32 ssh_message_length(u16 payload_size)
{
	return SSH_MSG_LEN_BASE + payload_size;
}


/* -- Builder functions for SAM-over-SSH messages. -------------------------- */

struct msgbuf {
	u8 *buffer;
	u8 *end;
	u8 *ptr;
};

static inline void msgb_init(struct msgbuf *msgb, u8 *buffer, size_t cap)
{
	msgb->buffer = buffer;
	msgb->end = buffer + cap;
	msgb->ptr = buffer;
}

static inline int msgb_alloc(struct msgbuf *msgb, size_t cap, gfp_t flags)
{
	u8 *buf;

	buf = kzalloc(cap, flags);
	if (!buf)
		return -ENOMEM;

	msgb_init(msgb, buf, cap);
	return 0;
}

static inline void msgb_free(struct msgbuf *msgb)
{
	kfree(msgb->buffer);
	msgb->buffer = NULL;
	msgb->end = NULL;
	msgb->ptr = NULL;
}

static inline void msgb_reset(struct msgbuf *msgb)
{
	msgb->ptr = msgb->buffer;
}

static inline size_t msgb_bytes_used(const struct msgbuf *msgb)
{
	return msgb->ptr - msgb->buffer;
}

static inline void msgb_push_u16(struct msgbuf *msgb, u16 value)
{
	BUG_ON(msgb->ptr + sizeof(u16) > msgb->end);

	put_unaligned_le16(value, msgb->ptr);
	msgb->ptr += sizeof(u16);
}

static inline void msgb_push_syn(struct msgbuf *msgb)
{
	msgb_push_u16(msgb, SSH_MSG_SYN);
}

static inline void msgb_push_buf(struct msgbuf *msgb, const u8 *buf, size_t len)
{
	msgb->ptr = memcpy(msgb->ptr, buf, len) + len;
}

static inline void msgb_push_crc(struct msgbuf *msgb, const u8 *buf, size_t len)
{
	msgb_push_u16(msgb, ssh_crc(buf, len));
}

static inline void msgb_push_frame(struct msgbuf *msgb, u8 ty, u16 len, u8 seq)
{
	struct ssh_frame *frame = (struct ssh_frame *)msgb->ptr;
	const u8 *const begin = msgb->ptr;

	BUG_ON(msgb->ptr + sizeof(*frame) > msgb->end);

	frame->type = ty;
	put_unaligned_le16(len, &frame->len);
	frame->seq  = seq;

	msgb->ptr += sizeof(*frame);
	msgb_push_crc(msgb, begin, msgb->ptr - begin);
}

static inline void msgb_push_ack(struct msgbuf *msgb, u8 seq)
{
	// SYN
	msgb_push_syn(msgb);

	// ACK-type frame + CRC
	msgb_push_frame(msgb, SSH_FRAME_TYPE_ACK, 0x00, seq);

	// payload CRC (ACK-type frames do not have a payload)
	msgb_push_crc(msgb, msgb->ptr, 0);
}

static inline void msgb_push_cmd(struct msgbuf *msgb, u8 seq,
				 const struct surface_sam_ssh_rqst *rqst,
				 u16 rqid)
{
	struct ssh_command *cmd;
	const u8 *cmd_begin;
	const u8 type = SSH_FRAME_TYPE_DATA_SEQ;

	// SYN
	msgb_push_syn(msgb);

	// command frame + crc
	msgb_push_frame(msgb, type, sizeof(*cmd) + rqst->cdl, seq);

	// frame payload: command struct + payload
	BUG_ON(msgb->ptr + sizeof(*cmd) > msgb->end);
	cmd_begin = msgb->ptr;
	cmd = (struct ssh_command *)msgb->ptr;

	cmd->type    = SSH_PLD_TYPE_CMD;
	cmd->tc      = rqst->tc;
	cmd->pri_out = rqst->pri;
	cmd->pri_in  = 0x00;
	cmd->iid     = rqst->iid;
	put_unaligned_le16(rqid, &cmd->rqid);
	cmd->cid     = rqst->cid;

	msgb->ptr += sizeof(*cmd);

	// command payload
	msgb_push_buf(msgb, rqst->pld, rqst->cdl);

	// crc for command struct + payload
	msgb_push_crc(msgb, cmd_begin, msgb->ptr - cmd_begin);
}


/* -- Parser functions for SAM-over-SSH messages. --------------------------- */

struct bufspan {
	u8    *ptr;
	size_t len;
};

static inline bool sshp_validate_crc(const struct bufspan *data, const u8 *crc)
{
	u16 actual = ssh_crc(data->ptr, data->len);
	u16 expected = get_unaligned_le16(crc);

	return actual == expected;
}

static bool sshp_find_syn(const struct bufspan *src, struct bufspan *rem)
{
	size_t i;

	for (i = 0; i < src->len - 1; i++) {
		if (likely(get_unaligned_le16(src->ptr + i) == SSH_MSG_SYN)) {
			rem->ptr = src->ptr + i;
			rem->len = src->len - i;
			return true;
		}
	}

	if (unlikely(src->ptr[src->len - 1] == (SSH_MSG_SYN & 0xff))) {
		rem->ptr = src->ptr + src->len - 1;
		rem->len = 1;
		return false;
	} else {
		rem->ptr = src->ptr + src->len;
		rem->len = 0;
		return false;
	}
}

static size_t sshp_parse_frame(const struct sam_ssh_ec *ec,
			       const struct bufspan *source,
			       struct ssh_frame **frame,
			       struct bufspan *payload)
{
	struct bufspan aligned;
	struct bufspan sf;
	struct bufspan sp;
	bool syn_found;

	// initialize output
	*frame = NULL;
	payload->ptr = NULL;
	payload->len = 0;

	// find SYN
	syn_found = sshp_find_syn(source, &aligned);

	if (unlikely(aligned.ptr - source->ptr) > 0)
		ssh_warn(ec, "rx: parser: invalid start of frame, skipping \n");

	if (unlikely(!syn_found))
		return aligned.ptr - source->ptr;

	// check for minumum packet length
	if (unlikely(aligned.len < ssh_message_length(0))) {
		ssh_dbg(ec, "rx: parser: not enough data for frame\n");
		return aligned.ptr - source->ptr;
	}

	// pin down frame
	sf.ptr = aligned.ptr + sizeof(u16);
	sf.len = sizeof(struct ssh_frame);

	// validate frame CRC
	if (unlikely(!sshp_validate_crc(&sf, sf.ptr + sf.len))) {
		ssh_warn(ec, "rx: parser: invalid frame CRC\n");

		// skip enough bytes to try and find next SYN
		return aligned.ptr - source->ptr + sizeof(u16);
	}

	// pin down payload
	sp.ptr = sf.ptr + sf.len + sizeof(u16);
	sp.len = get_unaligned_le16(&((struct ssh_frame *)sf.ptr)->len);

	// check for frame + payload length
	if (aligned.len < ssh_message_length(sp.len)) {
		ssh_dbg(ec, "rx: parser: not enough data for payload\n");
		return aligned.ptr - source->ptr;
	}

	// validate payload crc
	if (unlikely(!sshp_validate_crc(&sp, sp.ptr + sp.len))) {
		ssh_warn(ec, "rx: parser: invalid payload CRC\n");

		// skip enough bytes to try and find next SYN
		return aligned.ptr - source->ptr + sizeof(u16);
	}

	*frame = (struct ssh_frame *)sf.ptr;
	*payload = sp;

	ssh_dbg(ec, "rx: parser: valid frame found (type: 0x%02x, len: %u)\n",
		(*frame)->type, (*frame)->len);

	return aligned.ptr - source->ptr;
}

static void sshp_parse_command(const struct sam_ssh_ec *ec,
			       const struct bufspan *source,
			       struct ssh_command **command,
			       struct bufspan *command_data)
{
	// check for minimum length
	if (unlikely(source->len < sizeof(struct ssh_command))) {
		*command = NULL;
		command_data->ptr = NULL;
		command_data->len = 0;

		ssh_err(ec, "rx: parser: command payload is too short\n");
		return;
	}

	*command = (struct ssh_command *)source->ptr;
	command_data->ptr = source->ptr + sizeof(struct ssh_command);
	command_data->len = source->len - sizeof(struct ssh_command);

	ssh_dbg(ec, "rx: parser: valid command found (tc: 0x%02x,"
		" cid: 0x%02x)\n", (*command)->tc, (*command)->cid);
}


/* -- Packet transmission system (ptx). ------------------------------------- */
/*
 * To simplify reasoning about the code below, we define a few concepts. The
 * system below is similar to a state-machine for packets, however, there are
 * too many states to explicitly write them down. To (somewhat) manage the
 * states and packages we rely on flags, reference counting, and some simple
 * concepts. State transitions are triggered by actions.
 *
 * >> Actions <<
 *
 * - submit
 * - transmission start (process next item in queue)
 * - transmission finished (guaranteed to never be parallel to transmission
 *   start)
 * - ACK received
 * - NAK received (this is equivalent to issuing re-submit for all pending
 *   packets)
 * - timeout (this is equivalent to re-issuing a submit or canceling)
 * - cancel (non-pending and pending)
 *
 * >> Data Structures, Packet Ownership, General Overview <<
 *
 * The code below employs two main data structures: The packet queue, containing
 * all packets scheduled for transmission, and the set of pending packets,
 * containing all packets awaiting an ACK.
 *
 * Shared ownership of a packet is controlled via reference counting. Inside the
 * transmission system are a total of five packet owners:
 *
 * - the packet queue,
 * - the pending set,
 * - the transmitter thread,
 * - the receiver thread (via ACKing), and
 * - the timeout timer/work item.
 *
 * Normal operation is as follows: The initial reference of the packet is
 * obtained by submitting the packet and queueing it. The receiver thread
 * takes packets from the queue. By doing this, it does not increment the
 * refcount but takes over the reference (removing it from the queue).
 * If the packet is sequenced (i.e. needs to be ACKed by the client), the
 * transmitter thread sets-up the timeout and adds the packet to the pending set
 * before starting to transmit it. This effectively increases distributes two
 * additional references, one to the timeout and one to the pending set.
 * After the transmit is done, the reference hold by the transmitter thread
 * is dropped. If the packet is unsequenced (i.e. does not need an ACK), the
 * packet is completed by the transmitter thread before dropping that reference.
 *
 * On receial of an ACK, the receiver thread removes and obtains the refernce to
 * the packet from the pending set. On succes, the receiver thread will then
 * complete the packet and drop its reference.
 *
 * On error, the completion callback is immediately run by on thread on which
 * the error was detected.
 *
 * To ensure that a packet eventually leaves the system it is marked as "locked"
 * directly before it is going to be completed or when it is canceled. Marking a
 * packet as "locked" has the effect that passing and creating new references
 * of the packet will be blocked. This means that the packet cannot be added
 * to the queue, the pending set, and the timeout, or be picked up by the
 * transmitter thread or receiver thread. To remove a packet from the system it
 * has to be marked as locked and subsequently all references from the data
 * structures (queue, pending) have to be removed. It is also advisable to
 * cancel the timeout and removing any implicit references hold by it.
 * References held by threads will eventually be dropped automatically as their
 * execution progresses.
 *
 * Note that the packet completion callback is, in case of success and for a
 * sequenced packet, guaranteed to run on the receiver thread, thus providing a
 * way to reliably identify responses to the packet. The packet completion
 * callback is only run once and it does not indicate that the packet has fully
 * left the system. In case of re-submission (and with somewhat unlikely
 * timing), it may be possible that the packet is being re-transmitted while the
 * completion callback runs. Completion will occur both on success and internal
 * error, as well as when the packet is canceled.
 *
 * >> Flags <<
 *
 * Flags are used to indicate the state and progression of a packet. Some flags
 * have stricter guarantees than other:
 *
 * - locked
 *   Indicates if the packet is locked. If the packet is locked, passing and/or
 *   creating additional references to the packet is forbidden. The packet thus
 *   may not be queued, dequeued, or removed or added to the pending set. Note
 *   that the packet state flags may still change (e.g. it may be marked as
 *   ACKed, transmitted, ...).
 *
 * - completed
 *   Indicates if the packet completion has been run or is about to be run. This
 *   flag is used to ensure that the packet completion callback is only run
 *   once.
 *
 * - queued
 *   Indicates if a packet is present in the submission queue or not. This flag
 *   must only be modified with the queue lock held, and must be coherent
 *   presence of the packet in the queue.
 *
 * - pending
 *   Indicates if a packet is present in the set of pending packets or not.
 *   This flag must only be modified with the pending lock held, and must be
 *   coherent presence of the packet in the pending set.
 *
 * - transmitting
 *   Indicates if the packet is currently transmitting. In case of
 *   re-transmissions, it is only safe to wait on the "transmitted" completion
 *   after this flag has been set. The completion will be set both in success
 *   and error case.
 *
 * - transmitted
 *   Indicates if the packet has been transmitted. This flag is not cleared by
 *   the system, thus it indicates the first transmission only.
 *
 * - acked
 *   Indicates if the packet has been acknowledged by the client. There are no
 *   other guarantees given. For example, the packet may still be canceled
 *   and/or the completion may be triggered an error even though this bit is
 *   set. Rely on the status provided by completion instead.
 *
 * - canceled
 *   Indicates if the packet has been canceled from the outside. There are no
 *   other guarantees given. Specifically, the packet may be completed by
 *   another part of the system before the cancellation attempts to complete it.
 *
 * >> General Notes <<
 *
 * To avoid deadlocks, data structure locks (queue/pending) must always be
 * acquired before the packet lock and released after.
 */

/**
 * Maximum number transmission attempts per sequenced packet in case of
 * time-outs.
 */
#define SSH_PTX_MAX_PKT_TIMEOUTS	3

/**
 * Timeout in jiffies for ACKs. If we have not received an ACK in this
 * time-frame after starting transmission, the packet will be re-submitted.
 */
#define SSH_PTX_PKT_TIMEOUT		msecs_to_jiffies(1000)

/**
 * Maximum number of sequenced packets concurrently waiting for an ACK.
 * Packets marked as blocking will not be transmitted while this limit is
 * reached.
 */
#define SSH_PTX_MAX_PENDING		1

enum ssh_packet_priority {
	SSH_PACKET_PRIORITY_MIN = 0,
	SSH_PACKET_PRIORITY_DATA = SSH_PACKET_PRIORITY_MIN,
	SSH_PACKET_PRIORITY_DATA_RESUB,
	SSH_PACKET_PRIORITY_NAK,
	SSH_PACKET_PRIORITY_ACK,
};

enum ssh_packet_type_flags {
	SSH_PACKET_TY_SEQUENCED_BIT,
	SSH_PACKET_TY_BLOCKING_BIT,

	SSH_PACKET_TY_SEQUENCED = BIT(SSH_PACKET_TY_SEQUENCED_BIT),
	SSH_PACKET_TY_BLOCKING = BIT(SSH_PACKET_TY_BLOCKING_BIT),
};

enum ssh_packet_state_flags {
	SSH_PACKET_SF_LOCKED_BIT,
	SSH_PACKET_SF_QUEUED_BIT,
	SSH_PACKET_SF_PENDING_BIT,
	SSH_PACKET_SF_TRANSMITTING_BIT,
	SSH_PACKET_SF_TRANSMITTED_BIT,
	SSH_PACKET_SF_ACKED_BIT,
	SSH_PACKET_SF_CANCELED_BIT,
	SSH_PACKET_SF_COMPLETED_BIT,
};

struct ssh_ptx;
struct ssh_packet;

struct ssh_packet_ops {
	void (*release)(struct ssh_packet *packet);
	void (*complete)(struct ssh_packet *packet, int status);
};

struct ssh_packet {
	struct kref refcnt;

	struct ssh_ptx *ptx;
	struct list_head queue_node;
	struct list_head pending_node;

	enum ssh_packet_type_flags type;
	unsigned long state;

	enum ssh_packet_priority priority;
	u8 sequence_id;

	struct completion transmitted;

	struct {
		unsigned int count;
		struct mutex lock;
		struct timer_list timer;
		struct work_struct work;
	} timeout;

	struct {
		unsigned char *ptr;
		size_t length;
	} buffer;

	struct ssh_packet_ops ops;
};

typedef void (*ssh_ptx_completion_t)(struct ssh_packet *packet, int status);

struct ssh_ptx {
	struct serdev_device *serdev;

	struct {
		spinlock_t lock;
		struct list_head head;
	} queue;

	struct {
		spinlock_t lock;
		struct list_head head;
		atomic_t count;
	} pending;

	struct {
		struct task_struct *thread;
		struct wait_queue_head signal;
		bool signal_var;
		struct ssh_packet *packet;
		size_t offset;
	} tx;

	struct {
		// TODO
	} rx;
};

#define ptx_dbg(p, fmt, ...)  dev_dbg(&(p)->serdev->dev, fmt, ##__VA_ARGS__)
#define ptx_warn(p, fmt, ...) dev_warn(&(p)->serdev->dev, fmt, ##__VA_ARGS__)
#define ptx_err(p, fmt, ...)  dev_err(&(p)->serdev->dev, fmt, ##__VA_ARGS__)


static void __ssh_ptx_packet_release(struct kref *kref)
{
	struct ssh_packet *p = container_of(kref, struct ssh_packet, refcnt);
	p->ops.release(p);
}

static inline void ssh_packet_get(struct ssh_packet *packet)
{
	kref_get(&packet->refcnt);
}

static inline void ssh_packet_put(struct ssh_packet *packet)
{
	kref_put(&packet->refcnt, __ssh_ptx_packet_release);
}


static inline void ssh_ptx_timeout_start(struct ssh_packet *packet)
{
	// if this fails, someone else is setting or cancelling the timeout
	if (!mutex_trylock(&packet->timeout.lock))
		return;

	if (test_bit(SSH_PACKET_SF_LOCKED_BIT, &packet->state))
		return;

	ssh_packet_get(packet);
	mod_timer(&packet->timeout.timer, jiffies + SSH_PTX_PKT_TIMEOUT);

	mutex_unlock(&packet->timeout.lock);
}

static inline void ssh_ptx_timeout_cancel_sync(struct ssh_packet *packet)
{
	bool pending;

	mutex_lock(&packet->timeout.lock);
	pending = del_timer_sync(&packet->timeout.timer);
	pending |= cancel_work_sync(&packet->timeout.work);
	mutex_unlock(&packet->timeout.lock);

	if (pending)
		ssh_packet_put(packet);
}


static inline int ssh_ptx_queue_add(struct ssh_packet *p, struct list_head *h)
{
	// avoid further transitions when cancelling/completing
	if (test_bit(SSH_PACKET_SF_LOCKED_BIT, &p->state)) {
		return -EINVAL;
	}

	// if this packet has already been queued, do not add it
	if (test_and_set_bit(SSH_PACKET_SF_QUEUED_BIT, &p->state)) {
		return -EALREADY;
	}

	ssh_packet_get(p);
	list_add_tail(&p->queue_node, h);

	return 0;
}

static int ssh_ptx_queue_push(struct ssh_packet *packet)
{
	enum ssh_packet_priority priority = READ_ONCE(packet->priority);
	struct ssh_ptx *ptx = packet->ptx;
	struct list_head *head;
	struct ssh_packet *p;
	int status;

	spin_lock(&ptx->queue.lock);

	// fast path: minimum priority packets are always added at the end
	if (priority == SSH_PACKET_PRIORITY_MIN) {
		status = ssh_ptx_queue_add(packet, &ptx->queue.head);

	// regular path
	} else {
		// find first node with lower priority
		list_for_each(head, &ptx->queue.head) {
			p = list_entry(head, struct ssh_packet, queue_node);

			if (priority > READ_ONCE(p->priority))
				break;
		}

		// insert before
		status = ssh_ptx_queue_add(packet, &ptx->queue.head);
	}

	spin_unlock(&ptx->queue.lock);
	return status;
}

static inline void ssh_ptx_queue_remove(struct ssh_packet *packet)
{
	struct ssh_ptx *ptx = packet->ptx;
	bool remove;

	spin_lock(&ptx->queue.lock);

	remove = test_and_clear_bit(SSH_PACKET_SF_QUEUED_BIT, &packet->state);
	if (remove)
		list_del(&packet->queue_node);

	spin_unlock(&ptx->queue.lock);

	if (remove)
		ssh_packet_put(packet);
}


static inline void ssh_ptx_pending_push(struct ssh_packet *packet)
{
	struct ssh_ptx *ptx = packet->ptx;

	spin_lock(&ptx->pending.lock);

	// if we are cancelling/completing this packet, do not add it
	if (test_bit(SSH_PACKET_SF_LOCKED_BIT, &packet->state)) {
		spin_unlock(&ptx->pending.lock);
		return;
	}

	// in case it is already pending (e.g. re-submission), do not add it
	if (test_and_set_bit(SSH_PACKET_SF_PENDING_BIT, &packet->state)) {
		spin_unlock(&ptx->pending.lock);
		return;
	}

	atomic_inc(&ptx->pending.count);
	ssh_packet_get(packet);
	list_add_tail(&packet->pending_node, &ptx->pending.head);

	spin_unlock(&ptx->pending.lock);
}

static inline void ssh_ptx_pending_remove(struct ssh_packet *packet)
{
	struct ssh_ptx *ptx = packet->ptx;
	bool remove;

	spin_lock(&ptx->pending.lock);

	remove = test_and_clear_bit(SSH_PACKET_SF_PENDING_BIT, &packet->state);
	if (remove) {
		list_del(&packet->pending_node);
		atomic_dec(&ptx->pending.count);
	}

	spin_unlock(&ptx->pending.lock);

	if (remove)
		ssh_packet_put(packet);
}


static inline void __ssh_ptx_complete(struct ssh_packet *p, int status)
{
	ptx_dbg(p->ptx, "ptx: completing packet %p\n", p);
	p->ops.complete(p, status);
}

static inline void ssh_ptx_remove_and_complete(struct ssh_packet *p, int status)
{
	/*
	 * A call to this function should in general be preceeded by
	 * set_bit(SSH_PACKET_SF_LOCKED_BIT, &p->flags) to avoid re-adding the
	 * packet to the structures it's going to be removed from.
	 *
	 * The set_bit call does not need explicit memory barriers as the
	 * implicit barrier of the test_and_set_bit call below ensure that the
	 * flag is visible before we actually attempt to remove the packet.
	 */

	if (test_and_set_bit(SSH_PACKET_SF_COMPLETED_BIT, &p->state))
		return;

	ssh_ptx_timeout_cancel_sync(p);
	ssh_ptx_queue_remove(p);
	ssh_ptx_pending_remove(p);

	__ssh_ptx_complete(p, status);
}


static inline bool ssh_ptx_tx_can_process(struct ssh_packet *packet)
{
	struct ssh_ptx *ptx = packet->ptx;

	// we can alwas process non-blocking packets
	if (!(packet->type & SSH_PACKET_TY_BLOCKING))
		return true;

	// if we are already waiting for this packet, send it again
	if (test_bit(SSH_PACKET_SF_PENDING_BIT, &packet->state))
		return true;

	// otherwise: check if we have the capacity to send
	return atomic_read(&ptx->pending.count) < SSH_PTX_MAX_PENDING;
}

static inline struct ssh_packet *ssh_ptx_tx_pop(struct ssh_ptx *ptx)
{
	struct ssh_packet *packet = ERR_PTR(-ENOENT);
	struct ssh_packet *p, *n;

	spin_lock(&ptx->queue.lock);
	list_for_each_entry_safe(p, n, &ptx->pending.head, pending_node) {
		/*
		 * Packets should be ordered non-blocking/to-be-resent first.
		 * If we cannot process this packet, assume that we can't
		 * process any following packet either and abort.
		 */
		if (!ssh_ptx_tx_can_process(p)) {
			spin_unlock(&ptx->queue.lock);
			packet = ERR_PTR(-EBUSY);
			break;
		}

		/*
		 * If we are cancelling or completing this packet, ignore it.
		 * It's going to be removed from this queue shortly.
		 */
		if (test_bit(SSH_PACKET_SF_LOCKED_BIT, &p->state)) {
			spin_unlock(&ptx->queue.lock);
			continue;
		}

		/*
		 * We are allowed to change the state now. Remove it from the
		 * queue and mark it as being transmitted. Note that we cannot
		 * add it to the set of pending packets yet, as queue locks must
		 * always be acquired before packet locks (otherwise we might
		 * run into a deadlock).
		 */

		list_del(&p->queue_node);

		/*
		 * Re-initialize completion for transmit, ensure that this
		 * happens before we set the "transmitting" bit. Also ensure
		 * that the "queued" bit gets cleared after setting the
		 * "transmitting" bit to guaranteee non-zero flags.
		 */
		reinit_completion(&p->transmitted);
		smp_mb__before_atomic();
		set_bit(SSH_PACKET_SF_TRANSMITTING_BIT, &p->state);
		smp_mb__before_atomic();
		clear_bit(SSH_PACKET_SF_QUEUED_BIT, &p->state);

		packet = p;
		break;
	}
	spin_unlock(&ptx->queue.lock);

	return packet;
}

static inline struct ssh_packet *ssh_ptx_tx_next(struct ssh_ptx *ptx)
{
	struct ssh_packet *packet;

	packet = ssh_ptx_tx_pop(ptx);
	if (IS_ERR(packet))
		return packet;

	if (packet->type & SSH_PACKET_TY_SEQUENCED) {
		ptx_dbg(ptx, "transmitting sequenced packet %p\n", packet);
		ssh_ptx_pending_push(packet);
		ssh_ptx_timeout_start(packet);
	} else {
		ptx_dbg(ptx, "transmitting non-sequenced packet %p\n", packet);
	}

	return packet;
}

static inline void ssh_ptx_tx_compl_success(struct ssh_packet *packet)
{
	struct ssh_ptx *ptx = packet->ptx;

	ptx_dbg(ptx, "ptx: successfully transmitted packet %p\n", packet);

	/*
	 * Transition to state to "transmitted". Ensure that the flags never get
	 * zero with barrier.
	 */
	set_bit(SSH_PACKET_SF_TRANSMITTED_BIT, &packet->state);
	smp_mb__before_atomic();
	clear_bit(SSH_PACKET_SF_TRANSMITTING_BIT, &packet->state);

	// if the packet is unsequenced, we're done: lock and complete
	if (!(packet->type & SSH_PACKET_TY_SEQUENCED)) {
		set_bit(SSH_PACKET_SF_LOCKED_BIT, &packet->state);
		ssh_ptx_remove_and_complete(packet, 0);
	}

	complete_all(&packet->transmitted);
}

static inline void ssh_ptx_tx_compl_error(struct ssh_packet *packet, int status)
{
	/*
	 * Transmission failure: Lock the packet and try to complete it. Ensure
	 * that the flags never get zero with barrier.
	 */
	set_bit(SSH_PACKET_SF_LOCKED_BIT, &packet->state);
	smp_mb__before_atomic();
	clear_bit(SSH_PACKET_SF_TRANSMITTING_BIT, &packet->state);

	ptx_err(packet->ptx, "ptx: transmission error: %d\n", status);
	ptx_dbg(packet->ptx, "ptx: failed to transmit packet: %p\n", packet);

	ssh_ptx_remove_and_complete(packet, status);
	complete_all(&packet->transmitted);
}

static inline void ssh_ptx_tx_threadfn_wait(struct ssh_ptx *ptx)
{
	wait_event_interruptible(ptx->tx.signal,
				 READ_ONCE(ptx->tx.signal_var)
				 || kthread_should_stop());
	WRITE_ONCE(ptx->tx.signal_var, false);
}

static int ssh_ptx_tx_threadfn(void *data)
{
	struct ssh_ptx *ptx = data;
	unsigned char *buf;
	size_t len;
	int status;

	while (!kthread_should_stop()) {
		// if we don't have a packet, get the next and add it to pending
		if (IS_ERR_OR_NULL(ptx->tx.packet)) {
			ptx->tx.packet = ssh_ptx_tx_next(ptx);
			ptx->tx.offset = 0;

			// if no packet is available, we are done
			if (IS_ERR(ptx->tx.packet)) {
				ssh_ptx_tx_threadfn_wait(ptx);
				continue;
			}
		}

		buf = ptx->tx.packet->buffer.ptr + ptx->tx.offset;
		len = ptx->tx.packet->buffer.length - ptx->tx.offset;
		status = serdev_device_write_buf(ptx->serdev, buf, len);

		if (status < 0) {
			// complete packet with error
			ssh_ptx_tx_compl_error(ptx->tx.packet, status);
			ssh_packet_put(ptx->tx.packet);
			ptx->tx.packet = NULL;

		} else if (status == len) {
			// complete packet and/or mark as transmitted
			ssh_ptx_tx_compl_success(ptx->tx.packet);
			ssh_packet_put(ptx->tx.packet);
			ptx->tx.packet = NULL;

		} else {	// need more buffer space
			ptx->tx.offset += status;
			ssh_ptx_tx_threadfn_wait(ptx);
		}
	}

	// cancel active packet before we actually stop
	if (ptx->tx.packet) {
		ssh_ptx_tx_compl_error(ptx->tx.packet, -EINTR);
		ssh_packet_put(ptx->tx.packet);
		ptx->tx.packet = NULL;
	}

	return 0;
}

static inline void ssh_ptx_tx_wakeup(struct ssh_ptx *ptx, bool force)
{
	if (force || atomic_read(&ptx->pending.count) < SSH_PTX_MAX_PENDING) {
		WRITE_ONCE(ptx->tx.signal_var, true);
		smp_mb__after_atomic();
		wake_up(&ptx->tx.signal);
	}
}

static inline int ssh_ptx_tx_start(struct ssh_ptx *ptx)
{
	ptx->tx.thread = kthread_run(ssh_ptx_tx_threadfn, ptx, "surface-sh-tx");
	if (IS_ERR(ptx->tx.thread))
		return PTR_ERR(ptx->tx.thread);

	return 0;
}

static inline int ssh_ptx_tx_stop(struct ssh_ptx *ptx)
{
	return kthread_stop(ptx->tx.thread);
}


static struct ssh_packet *ssh_ptx_ack_pop(struct ssh_ptx *ptx, u8 seq_id)
{
	struct ssh_packet *packet = ERR_PTR(-ENOENT);
	struct ssh_packet *p, *n;

	spin_lock(&ptx->pending.lock);
	list_for_each_entry_safe(p, n, &ptx->pending.head, pending_node) {
		/*
		 * We generally expect packets to be in order, so first packet
		 * to be added to pending is first to be sent, is first to be
		 * ACKed.
		 */
		if (unlikely(p->sequence_id != seq_id))
			continue;

		/*
		 * In case we receive an ACK while handling a transmission error
		 * completion. The packet will be removed shortly.
		 */
		if (unlikely(test_bit(SSH_PACKET_SF_LOCKED_BIT, &p->state))) {
			packet = ERR_PTR(-EPERM);
			break;
		}

		/*
		 * Mark packet as ACKed and remove it from pending. Ensure that
		 * the flags never get zero with barrier.
		 */
		set_bit(SSH_PACKET_SF_ACKED_BIT, &p->state);
		smp_mb__before_atomic();
		clear_bit(SSH_PACKET_SF_PENDING_BIT, &p->state);

		atomic_dec(&ptx->pending.count);
		list_del(&p->pending_node);
		packet = p;

		break;
	}
	spin_unlock(&ptx->pending.lock);

	return packet;
}

static void ssh_ptx_acknowledge(struct ssh_ptx *ptx, u8 seq)
{
	struct ssh_packet *p;
	int status = 0;

	p = ssh_ptx_ack_pop(ptx, seq);
	if (IS_ERR(p)) {
		if (PTR_ERR(p) == -ENOENT) {
			/*
			 * The packet has not been found in the set of pending
			 * packets.
			 */
			ptx_warn(ptx, "ptx: received ACK for non-pending"
				 " packet\n");
		} else {
			/*
			 * The packet is pending, but we are not allowed to take
			 * it because it has been locked.
			 */
		}
		return;
	}

	ptx_dbg(ptx, "ptx: received ACK for packet %p\n", p);

	/*
	 * It is possible that the packet has been transmitted, but the state
	 * has not been updated from "transmitting" to "transmitted" yet.
	 * In that case, we need to wait for this transition to occur in order
	 * to determine between success or failure.
	 */
	if (unlikely(!test_bit(SSH_PACKET_SF_TRANSMITTED_BIT, &p->state)))
		if (likely(test_bit(SSH_PACKET_SF_TRANSMITTING_BIT, &p->state)))
			wait_for_completion(&p->transmitted);

	/*
	 * The packet will already be locked in case of a transmission error or
	 * cancellation. Let the transmitter or cancellation issuer complete the
	 * packet.
	 */
	if (unlikely(test_and_set_bit(SSH_PACKET_SF_LOCKED_BIT, &p->state))) {
		ssh_packet_put(p);
		return;
	}

	if (unlikely(!test_bit(SSH_PACKET_SF_TRANSMITTED_BIT, &p->state))) {
		ptx_err(ptx, "ptx: received ACK before packet had been fully"
			" transmitted\n");
		status = -EREMOTEIO;
	}

	ssh_ptx_remove_and_complete(p, status);
	ssh_packet_put(p);

	ssh_ptx_tx_wakeup(ptx, false);
}


static int ssh_ptx_submit(struct ssh_ptx *ptx, struct ssh_packet *packet)
{
	int status;

	/*
	 * This function is currently not intended for re-submission. The ptx
	 * reference only gets set on the first submission. After the first
	 * submission, it has to be read-only.
	 *
	 * Use cmpxchg to ensure safety with regards to ssh_ptx_cancel and
	 * re-entry, where we can't guarantee that the packet has been submitted
	 * yet.
	 *
	 * The implicit barrier of cmpxchg is paired with barrier in
	 * ssh_ptx_cancel to guarantee cancelation in case the packet has never
	 * been submitted or is currently being submitted.
	 */
	if (cmpxchg(&packet->ptx, NULL, ptx) != NULL)
		return -EALREADY;

	status = ssh_ptx_queue_push(packet);
	if (status)
		return status;

	ssh_ptx_tx_wakeup(ptx, !(packet->type & SSH_PACKET_TY_BLOCKING));
	return 0;
}

static int ssh_ptx_resubmit(struct ssh_packet *packet)
{
	bool force_work;
	int status;

	status = ssh_ptx_queue_push(packet);
	if (status)
		return status;

	force_work = test_bit(SSH_PACKET_SF_PENDING_BIT, &packet->state);
	force_work |= !(packet->type & SSH_PACKET_TY_BLOCKING);

	ssh_ptx_tx_wakeup(packet->ptx, force_work);
	return 0;
}

static void ssh_ptx_resubmit_pending(struct ssh_ptx *ptx)
{
	struct ssh_packet *p;
	struct list_head *head;

	spin_lock(&ptx->queue.lock);
	spin_lock(&ptx->pending.lock);

	// find first node with lower than data resubmission priority
	list_for_each(head, &ptx->queue.head) {
		p = list_entry(head, struct ssh_packet, queue_node);

		if (READ_ONCE(p->priority)
		    < SSH_PACKET_PRIORITY_DATA_RESUB)
			break;
	}

	// re-queue all pending packets
	list_for_each_entry(p, &ptx->pending.head, pending_node) {
		// avoid further transitions if locked
		if (test_bit(SSH_PACKET_SF_LOCKED_BIT, &p->state))
			continue;

		// if this packet has already been queued, do not add it
		if (test_and_set_bit(SSH_PACKET_SF_QUEUED_BIT, &p->state))
			continue;

		WRITE_ONCE(p->priority, SSH_PACKET_PRIORITY_DATA_RESUB);

		ssh_packet_get(p);
		list_add_tail(&p->queue_node, head);
	}

	spin_unlock(&ptx->pending.lock);
	spin_unlock(&ptx->queue.lock);

	ssh_ptx_tx_wakeup(ptx, true);
}

static void ssh_ptx_cancel(struct ssh_packet *p)
{
	if (test_and_set_bit(SSH_PACKET_SF_CANCELED_BIT, &p->state))
		return;

	/*
	 * Lock packet and commit with memory barrier. If this packet has
	 * already been locked, it's going to be removed and completed by
	 * another party, which should have precedence.
	 */
	if (test_and_set_bit(SSH_PACKET_SF_LOCKED_BIT, &p->state))
		return;

	/*
	 * By marking the packet as locked and employing the implicit memory
	 * barrier of test_and_set_bit, we have guaranteed that, at this point,
	 * the packet cannot be added to the queue any more.
	 *
	 * In case the packet has never been submitted, packet->ptx is NULL. If
	 * the packet is currently being submitted, packet->ptx may be NULL or
	 * non-NULL. Due marking the packet as locked above and committing with
	 * the memory barrier, we have guaranteed that, if packet->ptx is NULL,
	 * the packet will never be added to the queue. If packet->ptx is
	 * non-NULL, we don't have any guarantees.
	 */

	if (READ_ONCE(p->ptx)) {
		ssh_ptx_remove_and_complete(p, -EINTR);
		ssh_ptx_tx_wakeup(p->ptx, false);
	} else if (!test_and_set_bit(SSH_PACKET_SF_COMPLETED_BIT, &p->state)) {
		__ssh_ptx_complete(p, -EINTR);
	}
}


static void ssh_ptx_timeout_wfn(struct work_struct *work)
{
	struct ssh_packet *p;

	p = container_of(work, struct ssh_packet, timeout.work);
	p->timeout.count += 1;

	ptx_dbg(p->ptx, "ptx: packet timed out (packet = %p)", p);

	if (likely(p->timeout.count <= SSH_PTX_MAX_PKT_TIMEOUTS)) {
		// re-submit with (slightly) higher priority
		WRITE_ONCE(p->priority, SSH_PACKET_PRIORITY_DATA_RESUB);
		ssh_ptx_resubmit(p);

	} else {
		// we have reached the max number of timeouts: cancel packet

		/*
		 * Mark packet as locked. The memory barrier implied by
		 * test_and_set_bit ensures that the flag is visible before we
		 * attempt to remove it below.
		 */
		set_bit(SSH_PACKET_SF_LOCKED_BIT, &p->state);

		if (!test_and_set_bit(SSH_PACKET_SF_COMPLETED_BIT, &p->state)) {
			del_timer_sync(&p->timeout.timer);
			ssh_ptx_queue_remove(p);
			ssh_ptx_pending_remove(p);

			ssh_ptx_tx_wakeup(p->ptx, false);
			__ssh_ptx_complete(p, -ETIMEDOUT);

			ssh_ptx_tx_wakeup(p->ptx, false);
		}
	}

	ssh_packet_put(p);
}

static void ssh_ptx_timeout_tfn(struct timer_list *tl)
{
	struct ssh_packet *packet;

	packet = container_of(tl, struct ssh_packet, timeout.timer);
	schedule_work(&packet->timeout.work);
}


static void ssh_ptx_init(struct ssh_ptx *ptx, struct serdev_device *serdev) {
	ptx->serdev = serdev;

	spin_lock_init(&ptx->queue.lock);
	INIT_LIST_HEAD(&ptx->queue.head);

	spin_lock_init(&ptx->pending.lock);
	INIT_LIST_HEAD(&ptx->pending.head);
	atomic_set(&ptx->pending.count, 0);

	ptx->tx.thread = NULL;
	init_waitqueue_head(&ptx->tx.signal);
	ptx->tx.signal_var = false;
	ptx->tx.packet = NULL;
	ptx->tx.offset = 0;
}

static void ssh_ptx_init_packet(struct ssh_packet *packet,
				enum ssh_packet_type_flags type,
				enum ssh_packet_priority priority,
				u8 sequence_id, unsigned char *buffer,
				size_t buffer_length,
				struct ssh_packet_ops *ops)
{
	packet->ptx = NULL;
	packet->type = type;
	packet->state = 0;
	packet->priority = priority;
	packet->sequence_id = sequence_id;

	INIT_LIST_HEAD(&packet->queue_node);
	INIT_LIST_HEAD(&packet->pending_node);

	packet->timeout.count = 0;
	mutex_init(&packet->timeout.lock);
	timer_setup(&packet->timeout.timer, ssh_ptx_timeout_tfn, TIMER_IRQSAFE);
	INIT_WORK(&packet->timeout.work, ssh_ptx_timeout_wfn);

	packet->buffer.ptr = buffer;
	packet->buffer.length = buffer_length;

	packet->ops = *ops;
}


/* -- Request transmission system (rtx). ------------------------------------ */

#define SSH_RTX_RQST_TIMEOUT		msecs_to_jiffies(1000)
#define SSH_RTX_MAX_PENDING		3

enum ssh_request_type_flags {
	SSH_REQUEST_TY_EXPRESP_BIT,

	SSH_REQUEST_TY_EXPRESP = BIT(SSH_REQUEST_TY_EXPRESP_BIT),
};

enum ssh_request_state_flags {
	SSH_REQUEST_SF_LOCKED_BIT,
	SSH_REQUEST_SF_QUEUED_BIT,
	SSH_REQUEST_SF_PENDING_BIT,
	SSH_REQUEST_SF_TRANSMITTING_BIT,
	SSH_REQUEST_SF_TRANSMITTED_BIT,
	SSH_REQUEST_SF_RSPRCVD_BIT,
	SSH_REQUEST_SF_COMPLETED_BIT,
};

struct ssh_rtx;

struct ssh_request {
	struct ssh_rtx *rtx;
	struct ssh_packet packet;
	struct list_head queue_node;
	struct list_head pending_node;

	enum ssh_request_type_flags type;
	unsigned long state;

	u16 request_id;

	struct {
		struct mutex lock;
		struct timer_list timer;
		struct work_struct work;
	} timeout;

	// TODO
};

struct ssh_rtx {
	struct ssh_ptx ptx;

	struct {
		spinlock_t lock;
		struct list_head head;
	} queue;

	struct {
		spinlock_t lock;
		struct list_head head;
		atomic_t count;
	} pending;

	struct work_struct tx_work;
};


#define rtx_dbg(r, fmt, ...)  ptx_dbg(&(r)->ptx, fmt, ##__VA_ARGS__)
#define rtx_warn(r, fmt, ...) ptx_warn(&(r)->ptx, fmt, ##__VA_ARGS__)
#define rtx_err(r, fmt, ...)  ptx_err(&(r)->ptx, fmt, ##__VA_ARGS__)


static inline void ssh_request_get(struct ssh_request *rqst)
{
	ssh_packet_get(&rqst->packet);
}

static inline void ssh_request_put(struct ssh_request *rqst)
{
	ssh_packet_put(&rqst->packet);
}


static inline struct ssh_request *ssh_rtx_tx_next(struct ssh_rtx *rtx)
{
	struct ssh_request *rqst = ERR_PTR(-ENOENT);
	struct ssh_request *p, *n;

	if (atomic_read(&rtx->pending.count) >= SSH_RTX_MAX_PENDING)
		return ERR_PTR(-EBUSY);

	spin_lock(&rtx->queue.lock);

	// find first non-locked request and remove it
	list_for_each_entry_safe(p, n, &rtx->queue.head, queue_node) {
		if (unlikely(test_bit(SSH_REQUEST_SF_LOCKED_BIT, &p->state)))
			continue;

		/*
		 * Remove from queue and mark as transmitting. Ensure that the
		 * state does not get zero via memory barrier.
		 */
		set_bit(SSH_REQUEST_SF_TRANSMITTING_BIT, &p->state);
		smp_mb__before_atomic();
		clear_bit(SSH_REQUEST_SF_QUEUED_BIT, &p->state);

		list_del(&p->queue_node);

		rqst = p;
		break;
	}

	spin_unlock(&rtx->queue.lock);
	return rqst;
}

static inline int ssh_rtx_tx_pending_push(struct ssh_request *rqst)
{
	struct ssh_rtx *rtx = rqst->rtx;

	spin_lock(&rtx->pending.lock);

	if (test_bit(SSH_REQUEST_SF_LOCKED_BIT, &rqst->state)) {
		spin_unlock(&rtx->pending.lock);
		return -EINVAL;
	}

	if (test_and_set_bit(SSH_REQUEST_SF_PENDING_BIT, &rqst->state)) {
		spin_unlock(&rtx->pending.lock);
		return -EALREADY;
	}

	atomic_inc(&rtx->pending.count);
	ssh_request_get(rqst);
	list_add_tail(&rqst->pending_node, &rtx->pending.head);

	spin_unlock(&rtx->pending.lock);
	return 0;
}

static inline int ssh_rtx_tx_try_process_one(struct ssh_rtx *rtx)
{
	struct ssh_request *rqst;
	int status;

	rqst = ssh_rtx_tx_next(rtx);
	if (IS_ERR(rqst))
		return PTR_ERR(rqst);

	status = ssh_rtx_tx_pending_push(rqst);
	if (status) {
		ssh_request_put(rqst);
		return -EAGAIN;
	}

	/* Part 3: Submit packet. */
	status = ssh_ptx_submit(&rtx->ptx, &rqst->packet);
	if (status) {
		/*
		 * If submitting the packet failed, the packet has either been
		 * submmitted/queued before (which cannot happen as we have
		 * guaranteed that requests cannot be re-submitted), or the
		 * packet was marked as locked. To mark the packet locked at
		 * this stage, the request, and thus the package itself, had to
		 * have been canceled. Simply drop the reference. Cancellation
		 * itself will remove it from the set of pending requests.
		 */
		ssh_request_put(rqst);
		return -EAGAIN;
	}

	ssh_request_put(rqst);
	return 0;
}

static inline bool ssh_rtx_tx_schedule(struct ssh_rtx *rtx)
{
	if (atomic_read(&rtx->pending.count) < SSH_RTX_MAX_PENDING)
		return schedule_work(&rtx->tx_work);
	else
		return false;
}

static void ssh_rtx_tx_work_fn(struct work_struct *work)
{
	struct ssh_rtx *rtx = container_of(work, struct ssh_rtx, tx_work);
	int i, status;

	/*
	 * Try to be nice and not block the workqueue: Run a maximum of 10
	 * tries, then re-submit if necessary. This should not be neccesary,
	 * for normal execution, but guarantee it anyway.
	 */
	for (i = 0; i < 10; i++) {
		status = ssh_rtx_tx_try_process_one(rtx);
		if (status == -ENOENT || status == -EBUSY)
			return;		// no more requests to process

		BUG_ON(status != 0 || status != -EAGAIN);
	}

	// out of tries, reschedule
	ssh_rtx_tx_schedule(rtx);
}


static int ssh_rtx_submit(struct ssh_rtx *rtx, struct ssh_request *rqst)
{
	/*
	 * Ensure that requests expecting a response are sequenced. If this
	 * invariant ever changes, see the comment in ssh_rtx_complete on what
	 * is required to be changed in the code.
	 */
	if (rqst->type & SSH_REQUEST_TY_EXPRESP)
		if (!(rqst->packet.type & SSH_PACKET_TY_SEQUENCED))
			return -EINVAL;

	// try to set rtx and check if this request has already been submitted
	if (cmpxchg(&rqst->rtx, NULL, rtx) != NULL)
		return -EALREADY;

	spin_lock(&rtx->queue.lock);

	if (test_bit(SSH_REQUEST_SF_LOCKED_BIT, &rqst->state)) {
		spin_unlock(&rtx->queue.lock);
		return -EINVAL;
	}

	if (test_and_set_bit(SSH_REQUEST_SF_QUEUED_BIT, &rqst->state)) {
		spin_unlock(&rtx->queue.lock);
		return -EALREADY;
	}

	ssh_request_get(rqst);
	list_add_tail(&rqst->queue_node, &rtx->queue.head);

	spin_unlock(&rtx->queue.lock);

	ssh_rtx_tx_schedule(rtx);
	return 0;
}


static inline void ssh_rtx_queue_remove(struct ssh_request *rqst)
{
	bool remove;

	spin_lock(&rqst->rtx->queue.lock);

	remove = test_and_clear_bit(SSH_REQUEST_SF_QUEUED_BIT, &rqst->state);
	if (remove)
		list_del(&rqst->queue_node);

	spin_unlock(&rqst->rtx->queue.lock);

	if (remove)
		ssh_request_put(rqst);
}

static inline void ssh_rtx_pending_remove(struct ssh_request *rqst)
{
	bool remove;

	spin_lock(&rqst->rtx->pending.lock);

	remove = test_and_clear_bit(SSH_REQUEST_SF_PENDING_BIT, &rqst->state);
	if (remove) {
		atomic_dec(&rqst->rtx->pending.count);
		list_del(&rqst->pending_node);
	}

	spin_unlock(&rqst->rtx->pending.lock);

	if (remove)
		ssh_request_put(rqst);
}


static inline void ssh_rtx_timeout_start(struct ssh_request *rqst)
{
	// if this fails, someone else is setting or cancelling the timeout
	if (!mutex_trylock(&rqst->timeout.lock))
		return;

	if (test_bit(SSH_REQUEST_SF_LOCKED_BIT, &rqst->state))
		return;

	ssh_request_get(rqst);
	mod_timer(&rqst->timeout.timer, jiffies + SSH_RTX_RQST_TIMEOUT);

	mutex_unlock(&rqst->timeout.lock);
}

static inline void ssh_rtx_timeout_cancel_sync(struct ssh_request *rqst)
{
	bool pending;

	mutex_lock(&rqst->timeout.lock);
	pending = del_timer_sync(&rqst->timeout.timer);
	pending |= cancel_work_sync(&rqst->timeout.work);
	mutex_unlock(&rqst->timeout.lock);

	if (pending)
		ssh_request_put(rqst);
}


static void ssh_rtx_complete_with_status(struct ssh_request *rqst, int status)
{
	struct ssh_rtx *rtx = READ_ONCE(rqst->rtx);

	// rqst->rtx may not be set if we're cancelling before submitting
	if (rtx) {
		rtx_dbg(rtx, "rtx: completing request (rqid: 0x%04x,"
			" status: %d)\n", rqst->request_id, status);
	}

	// TODO
}

static void ssh_rtx_complete_with_rsp(struct ssh_request *rqst /* TODO */)
{
	rtx_dbg(rqst->rtx, "rtx: completing request with response"
		" (rqid: 0x%04x)\n", rqst->request_id);

	// TODO
}

static void ssh_rtx_complete(struct ssh_rtx *rtx, u16 rqid /* TODO */)
{
	struct ssh_request *r = NULL;
	struct ssh_request *p, *n;

	/*
	 * Get request from pending based on request ID and mark it as response
	 * received and locked.
	 */
	spin_lock(&rtx->pending.lock);
	list_for_each_entry_safe(p, n, &rtx->pending.head, pending_node) {
		// we generally expect requests to be processed in order
		if (unlikely(p->request_id != rqid))
			continue;

		/*
		 * Mark as "responce received" and "locked" as we're going to
		 * complete it. Ensure that the state doesn't get zero by
		 * employing a memory barrier.
		 */
		set_bit(SSH_REQUEST_SF_LOCKED_BIT, &p->state);
		set_bit(SSH_REQUEST_SF_RSPRCVD_BIT, &p->state);
		smp_mb__before_atomic();
		clear_bit(SSH_REQUEST_SF_PENDING_BIT, &p->state);

		atomic_dec(&rtx->pending.count);
		list_del(&p->pending_node);

		r = p;
		break;
	}
	spin_unlock(&rtx->pending.lock);

	if (!r) {
		rtx_err(rtx, "rtx: received unexpected command message"
			" (rqid = 0x%04x)\n", rqid);
		return;
	}

	// if the request hasn't been completed yet, we will do this now
	if (test_and_set_bit(SSH_REQUEST_SF_COMPLETED_BIT, &r->state)) {
		ssh_request_put(r);
		ssh_rtx_tx_schedule(rtx);
		return;
	}

	// disable timeout first
	ssh_rtx_timeout_cancel_sync(r);

	/*
	 * Make sure the request has been transmitted. In case of a sequenced
	 * request, we are guaranteed that the completion callback will run on
	 * the receiver thread directly when the ACK for the packet has been
	 * received. Similarly, this function is guaranteed to run on the
	 * receiver thread. Thus we are guaranteed that if the packet has been
	 * successfully transmitted and received an ACK, the transmitted flag
	 * has been set and is visible here.
	 *
	 * We are currently not handling unsequenced packets here, as those
	 * should never expect a response as ensured in ssh_rtx_submit. If this
	 * ever changes, one would have to test for
	 *
	 * 	(r->state & (transmitting | transmitted))
	 *
	 * on unsequenced packets to determine if they could have been
	 * transmitted. There are no synchronization guarantees as in the
	 * sequenced case, since, in this case, the callback function will not
	 * run on the same thread. Thus an exact determination is impossible.
	 */
	if (!test_bit(SSH_REQUEST_SF_TRANSMITTED_BIT, &r->state)) {
		rtx_err(rtx, "rtx: received response before ACK for request"
			" (rqid = 0x%04x)\n", rqid);

		/*
		 * NB: Timeout has already been canceled, request already been
		 * removed from pending and marked as locked and completed. As
		 * we receive a "false" response, the packet might still be
		 * queued though.
		 */
		ssh_rtx_queue_remove(r);

		ssh_rtx_complete_with_status(r, -EREMOTEIO);
		ssh_request_put(r);

		ssh_rtx_tx_schedule(rtx);
		return;
	}

	/*
	 * NB: Timeout has already been canceled, request already been
	 * removed from pending and marked as locked and completed. The request
	 * can also not be queued any more, as it has been marked as
	 * transmitting and later transmitted. Thus no need to remove it from
	 * anywhere.
	 */

	ssh_rtx_complete_with_rsp(r);
	ssh_request_put(r);

	ssh_rtx_tx_schedule(rtx);
}


static inline bool ssh_rtx_cancel_nonpending(struct ssh_request *r)
{
	unsigned long state;
	bool remove;

	/*
	 * Handle unsubmitted request: Try to mark the packet as locked,
	 * expecting the state to be zero (i.e. unsubmitted). Note that, if
	 * setting the state worked, we might still be adding the packet to the
	 * queue in a currently executing submit call. In that case, however,
	 * rqst->rtx must have been set previously, as locked is checked after
	 * setting rqst->rtx. Thus only if we successfully lock this request and
	 * rqst->rtx is NULL, we have successfully removed the request.
	 * Otherwise we need to try and grab it from the queue.
	 *
	 * Note that if the CMPXCHG fails, we are guaranteed that rqst->rtx has
	 * been set and is non-NULL, as states can only be nonzero after this
	 * has been set.
	 */
	state = cmpxchg(&r->state, 0, SSH_REQUEST_SF_LOCKED_BIT);
	if (!state && !READ_ONCE(r->rtx)) {
		if (test_and_set_bit(SSH_REQUEST_SF_COMPLETED_BIT, &r->state))
			return true;

		ssh_rtx_complete_with_status(r, -EINTR);
		return true;
	}

	spin_lock(&r->rtx->queue.lock);

	/*
	 * Note: 1) Requests cannot be re-submitted. 2) If a request is queued,
	 * it cannot be "transmitting"/"pending" yet. Thus, if we successfully
	 * remove the the request here, we have removed all its occurences in
	 * the system.
	 */

	remove = test_and_clear_bit(SSH_REQUEST_SF_QUEUED_BIT, &r->state);
	if (!remove) {
		spin_unlock(&r->rtx->queue.lock);
		return false;
	}

	set_bit(SSH_REQUEST_SF_LOCKED_BIT, &r->state);
	list_del(&r->queue_node);

	spin_unlock(&r->rtx->queue.lock);

	ssh_request_put(r);	// drop reference obtained from queue

	if (test_and_set_bit(SSH_REQUEST_SF_COMPLETED_BIT, &r->state))
		return true;

	ssh_rtx_complete_with_status(r, -EINTR);
	return true;
}

static inline bool ssh_rtx_cancel_pending(struct ssh_request *r)
{
	// if the packet is already locked, it's going to be removed shortly
	if (test_and_set_bit(SSH_REQUEST_SF_LOCKED_BIT, &r->state))
		return true;

	/*
	 * Now that we have locked the packet, we have guaranteed that it can't
	 * be added to the system any more. If rqst->rtx is zero, the locked
	 * check in ssh_rtx_submit has not been run and any submission,
	 * currently in progress or called later, won't add the packet. Thus we
	 * can directly complete it.
	 */
	if (!READ_ONCE(r->rtx)) {
		if (test_and_set_bit(SSH_REQUEST_SF_COMPLETED_BIT, &r->state))
			return true;

		ssh_rtx_complete_with_status(r, -EINTR);
		return true;
	}

	/*
	 * Try to cancel the packet. If the packet has not been completed yet,
	 * this will subsequently (and synchronously) call the completion
	 * callback of the packet, which will complete the request.
	 */
	ssh_ptx_cancel(&r->packet);

	/*
	 * If the packet has been completed with success, i.e. has not been
	 * canceled by the above call, the request may not have been completed
	 * yet (may be waiting for a response). Check if we need to do this
	 * here.
	 */
	if (test_and_set_bit(SSH_REQUEST_SF_COMPLETED_BIT, &r->state))
		return true;

	ssh_rtx_queue_remove(r);
	ssh_rtx_timeout_cancel_sync(r);
	ssh_rtx_pending_remove(r);
	ssh_rtx_complete_with_status(r, -EINTR);

	return true;
}

static bool ssh_rtx_cancel(struct ssh_request *rqst, bool pending)
{
	struct ssh_rtx *rtx;
	bool canceled;

	if (pending)
		canceled = ssh_rtx_cancel_pending(rqst);
	else
		canceled = ssh_rtx_cancel_nonpending(rqst);

	// note: rqst->rtx may be NULL if request has not been submitted yet
	rtx = READ_ONCE(rqst->rtx);
	if (canceled && rtx)
		ssh_rtx_tx_schedule(rtx);

	return canceled;
}


static void ssh_rtx_packet_callback(struct ssh_packet *p, int status)
{
	struct ssh_request *r = container_of(p, struct ssh_request, packet);

	if (unlikely(status)) {
		set_bit(SSH_REQUEST_SF_LOCKED_BIT, &r->state);

		if (test_and_set_bit(SSH_REQUEST_SF_COMPLETED_BIT, &r->state))
			return;

		/*
		 * The packet may get cancelled even though it has not been
		 * submitted yet. The request may still be queued. Check the
		 * queue and remove it if necessary. As the timeout would have
		 * been started in this function on success, there's no need to
		 * cancel it here.
		 */
		ssh_rtx_queue_remove(r);
		ssh_rtx_pending_remove(r);
		ssh_rtx_complete_with_status(r, -EINTR);

		ssh_rtx_tx_schedule(r->rtx);
		return;
	}

	/*
	 * Mark as transmitted, ensure that state doesn't get zero by inserting
	 * a memory barrier.
	 */
	set_bit(SSH_REQUEST_SF_TRANSMITTED_BIT, &r->state);
	smp_mb__before_atomic();
	clear_bit(SSH_REQUEST_SF_TRANSMITTING_BIT, &r->state);

	// if we expect a response, we just need to start the timeout
	if (r->type & SSH_REQUEST_TY_EXPRESP) {
		ssh_rtx_timeout_start(r);
		return;
	}

	/*
	 * If we don't expect a response, lock, remove, and complete the
	 * request. Note that, at this point, the request is guaranteed to have
	 * left the queue and no timeout has been started. Thus we only need to
	 * remove it from pending. If the request has already been completed (it
	 * may have been canceled) return.
	 */

	set_bit(SSH_REQUEST_SF_LOCKED_BIT, &r->state);
	if (test_and_set_bit(SSH_REQUEST_SF_COMPLETED_BIT, &r->state))
		return;

	ssh_rtx_pending_remove(r);
	ssh_rtx_complete_with_status(r, 0);

	ssh_rtx_tx_schedule(r->rtx);
}


static void ssh_rtx_timeout_wfn(struct work_struct *work)
{
	struct ssh_request *rqst;
	struct ssh_rtx *rtx;

	rqst = container_of(work, struct ssh_request, timeout.work);
	rtx = rqst->rtx;

	set_bit(SSH_REQUEST_SF_LOCKED_BIT, &rqst->state);
	if (test_and_set_bit(SSH_REQUEST_SF_COMPLETED_BIT, &rqst->state))
		return;

	/*
	 * The timeout is activated only after the request has been removed from
	 * the queue. Thus no need to check it. The timeout is guaranteed to run
	 * only once, so we also don't need to handle that.
	 */
	ssh_rtx_pending_remove(rqst);
	ssh_rtx_complete_with_status(rqst, -ETIMEDOUT);
	ssh_request_put(rqst);

	ssh_rtx_tx_schedule(rtx);
}

static void ssh_rtx_timeout_tfn(struct timer_list *tl)
{
	struct ssh_packet *packet;

	packet = container_of(tl, struct ssh_packet, timeout.timer);
	schedule_work(&packet->timeout.work);
}


/* -- TODO ------------------------------------------------------------------ */

static inline struct sam_ssh_ec *surface_sam_ssh_acquire(void)
{
	struct sam_ssh_ec *ec = &ssh_ec;

	mutex_lock(&ec->lock);
	return ec;
}

static inline void surface_sam_ssh_release(struct sam_ssh_ec *ec)
{
	mutex_unlock(&ec->lock);
}

static inline struct sam_ssh_ec *surface_sam_ssh_acquire_init(void)
{
	struct sam_ssh_ec *ec = surface_sam_ssh_acquire();

	if (smp_load_acquire(&ec->state) == SSH_EC_UNINITIALIZED) {
		surface_sam_ssh_release(ec);
		return NULL;
	}

	return ec;
}

int surface_sam_ssh_consumer_register(struct device *consumer)
{
	u32 flags = DL_FLAG_PM_RUNTIME | DL_FLAG_AUTOREMOVE_CONSUMER;
	struct sam_ssh_ec *ec;
	struct device_link *link;

	ec = surface_sam_ssh_acquire_init();
	if (!ec)
		return -ENXIO;

	link = device_link_add(consumer, &ec->serdev->dev, flags);
	if (!link)
		return -EFAULT;

	surface_sam_ssh_release(ec);
	return 0;
}
EXPORT_SYMBOL_GPL(surface_sam_ssh_consumer_register);


static int surface_sam_ssh_rqst_unlocked(struct sam_ssh_ec *ec,
					 const struct surface_sam_ssh_rqst *rqst,
					 struct surface_sam_ssh_buf *result);

static int surface_sam_ssh_event_enable(struct sam_ssh_ec *ec, u8 tc,
					u8 unknown, u16 rqid)
{
	u8 pld[4] = { tc, unknown, rqid & 0xff, rqid >> 8 };
	u8 buf[1] = { 0x00 };
	int status;

	struct surface_sam_ssh_rqst rqst = {
		.tc  = 0x01,
		.cid = 0x0b,
		.iid = 0x00,
		.pri = SURFACE_SAM_PRIORITY_NORMAL,
		.snc = 0x01,
		.cdl = 0x04,
		.pld = pld,
	};

	struct surface_sam_ssh_buf result = {
		result.cap = ARRAY_SIZE(buf),
		result.len = 0,
		result.data = buf,
	};

	// only allow RQIDs that lie within event spectrum
	if (!ssh_rqid_is_event(rqid))
		return -EINVAL;

	status = surface_sam_ssh_rqst_unlocked(ec, &rqst, &result);

	if (status) {
		dev_err(&ec->serdev->dev, "failed to enable event source"
			" (tc: 0x%02x, rqid: 0x%04x)\n", tc, rqid);
	}

	if (buf[0] != 0x00) {
		pr_warn(SSH_RQST_TAG_FULL
			"unexpected result while enabling event source: "
			"0x%02x\n", buf[0]);
	}

	return status;

}

static int surface_sam_ssh_event_disable(struct sam_ssh_ec *ec, u8 tc,
					 u8 unknown, u16 rqid)
{
	u8 pld[4] = { tc, unknown, rqid & 0xff, rqid >> 8 };
	u8 buf[1] = { 0x00 };
	int status;

	struct surface_sam_ssh_rqst rqst = {
		.tc  = 0x01,
		.cid = 0x0c,
		.iid = 0x00,
		.pri = SURFACE_SAM_PRIORITY_NORMAL,
		.snc = 0x01,
		.cdl = 0x04,
		.pld = pld,
	};

	struct surface_sam_ssh_buf result = {
		result.cap = ARRAY_SIZE(buf),
		result.len = 0,
		result.data = buf,
	};

	// only allow RQIDs that lie within event spectrum
	if (!ssh_rqid_is_event(rqid))
		return -EINVAL;

	status = surface_sam_ssh_rqst_unlocked(ec, &rqst, &result);

	if (status) {
		dev_err(&ec->serdev->dev, "failed to disable event source"
			" (tc: 0x%02x, rqid: 0x%04x)\n", tc, rqid);
	}

	if (buf[0] != 0x00) {
		dev_warn(&ec->serdev->dev,
			"unexpected result while disabling event source: "
			"0x%02x\n", buf[0]);
	}

	return status;
}


int surface_sam_ssh_notifier_register(u8 tc, struct notifier_block *nb)
{
	struct sam_ssh_ec *ec;
	struct srcu_notifier_head *nh;
	u16 event = ssh_tc_to_event(tc);
	u16 rqid = ssh_event_to_rqid(event);
	int status;

	if (!ssh_rqid_is_event(rqid))
		return -EINVAL;

	ec = surface_sam_ssh_acquire_init();
	if (!ec)
		return -ENXIO;

	nh = &ec->events.notifier[event];
	status = srcu_notifier_chain_register(nh, nb);
	if (status) {
		surface_sam_ssh_release(ec);
		return status;
	}

	if (ec->events.notifier_count[event] == 0) {
		status = surface_sam_ssh_event_enable(ec, tc, 0x01, rqid);
		if (status) {
			srcu_notifier_chain_unregister(nh, nb);
			surface_sam_ssh_release(ec);
			return status;
		}
	}
	ec->events.notifier_count[event] += 1;

	surface_sam_ssh_release(ec);
	return 0;
}
EXPORT_SYMBOL_GPL(surface_sam_ssh_notifier_register);

int surface_sam_ssh_notifier_unregister(u8 tc, struct notifier_block *nb)
{
	struct sam_ssh_ec *ec;
	struct srcu_notifier_head *nh;
	u16 event = ssh_tc_to_event(tc);
	u16 rqid = ssh_event_to_rqid(event);
	int status;

	if (!ssh_rqid_is_event(rqid))
		return -EINVAL;

	ec = surface_sam_ssh_acquire_init();
	if (!ec)
		return -ENXIO;

	nh = &ec->events.notifier[event];
	status = srcu_notifier_chain_unregister(nh, nb);
	if (status) {
		surface_sam_ssh_release(ec);
		return status;
	}

	ec->events.notifier_count[event] -= 1;
	if (ec->events.notifier_count == 0)
		status = surface_sam_ssh_event_disable(ec, tc, 0x01, rqid);

	surface_sam_ssh_release(ec);
	return status;
}
EXPORT_SYMBOL_GPL(surface_sam_ssh_notifier_unregister);


static int ssh_send_msgbuf(struct sam_ssh_ec *ec, const struct msgbuf *msgb,
			   long timeout)
{
	struct serdev_device *serdev = ec->serdev;
	size_t len = msgb_bytes_used(msgb);
	int status;

	ssh_dbg(ec, "tx: sending data (length: %zu)\n", len);
	print_hex_dump_debug("tx: ", DUMP_PREFIX_OFFSET, 16, 1, msgb->buffer,
			     len, false);

	status = serdev_device_write(serdev, msgb->buffer, len, timeout);
	if (status < 0)
		return status;
	if ((size_t)status < len)
		return -EINTR;

	serdev_device_wait_until_sent(serdev, 0);
	return 0;
}

static inline void ssh_receiver_restart(struct sam_ssh_ec *ec,
					const struct surface_sam_ssh_rqst *rqst)
{
	unsigned long flags;

	spin_lock_irqsave(&ec->receiver.lock, flags);
	reinit_completion(&ec->receiver.signal);
	ec->receiver.state = SSH_RCV_CONTROL;
	ec->receiver.expect.pld = rqst->snc;
	ec->receiver.expect.seq = ec->counter.seq;
	ec->receiver.expect.rqid = ec->counter.rqid;
	ec->receiver.eval_buf.len = 0;
	spin_unlock_irqrestore(&ec->receiver.lock, flags);
}

static inline void ssh_receiver_discard(struct sam_ssh_ec *ec)
{
	unsigned long flags;

	spin_lock_irqsave(&ec->receiver.lock, flags);
	ec->receiver.state = SSH_RCV_DISCARD;
	ec->receiver.eval_buf.len = 0;
	kfifo_reset(&ec->receiver.fifo);
	spin_unlock_irqrestore(&ec->receiver.lock, flags);
}

static int surface_sam_ssh_rqst_unlocked(struct sam_ssh_ec *ec,
					 const struct surface_sam_ssh_rqst *rqst,
					 struct surface_sam_ssh_buf *result)
{
	struct ssh_fifo_packet packet = {};
	struct msgbuf msgb;
	u16 rqid = ec->counter.rqid;
	int status, try;
	unsigned int rem;

	// TODO: assumption doesn't hold any more, allow larger payloads
	if (rqst->cdl > SURFACE_SAM_SSH_MAX_RQST_PAYLOAD) {
		ssh_err(ec, SSH_RQST_TAG "request payload too large\n");
		return -EINVAL;
	}

	// TODO: calculate actual message length
	status = msgb_alloc(&msgb, SSH_MAX_WRITE, GFP_KERNEL);
	if (status)
		return status;

	// write command in buffer, we may need it multiple times
	msgb_push_cmd(&msgb, ec->counter.seq, rqst, rqid);

	ssh_receiver_restart(ec, rqst);

	// send command, try to get an ack response
	for (try = 0; try < SSH_NUM_RETRY; try++) {
		status = ssh_send_msgbuf(ec, &msgb, SSH_TX_TIMEOUT);
		if (status)
			goto out;

		rem = wait_for_completion_timeout(&ec->receiver.signal, SSH_RX_TIMEOUT);
		if (rem) {
			// completion assures valid packet, thus ignore returned length
			(void) !kfifo_out(&ec->receiver.fifo, &packet, sizeof(packet));

			if (packet.type == SSH_FRAME_TYPE_ACK)
				break;
		}
	}

	// check if we ran out of tries?
	if (try >= SSH_NUM_RETRY) {
		ssh_err(ec, SSH_RQST_TAG "communication failed %d times, giving up\n", try);
		status = -EIO;
		goto out;
	}

	ec->counter.seq += 1;
	ec->counter.rqid = ssh_rqid_next(ec->counter.rqid);

	// get command response/payload
	if (rqst->snc && result) {
		rem = wait_for_completion_timeout(&ec->receiver.signal, SSH_RX_TIMEOUT);
		if (rem) {
			// completion assures valid packet, thus ignore returned length
			(void) !kfifo_out(&ec->receiver.fifo, &packet, sizeof(packet));

			if (result->cap < packet.len) {
				status = -EINVAL;
				goto out;
			}

			// completion assures valid packet, thus ignore returned length
			(void) !kfifo_out(&ec->receiver.fifo, result->data, packet.len);
			result->len = packet.len;
		} else {
			ssh_err(ec, SSH_RQST_TAG "communication timed out\n");
			status = -EIO;
			goto out;
		}

		// send ACK
		if (packet.type == SSH_FRAME_TYPE_DATA_SEQ) {
			// TODO: add send_ack function?
			msgb_reset(&msgb);
			msgb_push_ack(&msgb, packet.seq);

			status = ssh_send_msgbuf(ec, &msgb, SSH_TX_TIMEOUT);
			if (status)
				goto out;
		}
	}

out:
	ssh_receiver_discard(ec);
	msgb_free(&msgb);
	return status;
}

int surface_sam_ssh_rqst(const struct surface_sam_ssh_rqst *rqst, struct surface_sam_ssh_buf *result)
{
	struct sam_ssh_ec *ec;
	int status;

	ec = surface_sam_ssh_acquire_init();
	if (!ec) {
		pr_warn(SSH_RQST_TAG_FULL "embedded controller is uninitialized\n");
		return -ENXIO;
	}

	if (smp_load_acquire(&ec->state) == SSH_EC_SUSPENDED) {
		ssh_warn(ec, SSH_RQST_TAG "embedded controller is suspended\n");

		surface_sam_ssh_release(ec);
		return -EPERM;
	}

	status = surface_sam_ssh_rqst_unlocked(ec, rqst, result);

	surface_sam_ssh_release(ec);
	return status;
}
EXPORT_SYMBOL_GPL(surface_sam_ssh_rqst);


/**
 * surface_sam_ssh_ec_resume - Resume the EC if it is in a suspended mode.
 * @ec: the EC to resume
 *
 * Moves the EC from a suspended state to a normal state. See the
 * `surface_sam_ssh_ec_suspend` function what the specific differences of
 * these states are. Multiple repeated calls to this function seem to be
 * handled fine by the EC, after the first call, the state will remain
 * "normal".
 *
 * Must be called with the EC initialized and its lock held.
 */
static int surface_sam_ssh_ec_resume(struct sam_ssh_ec *ec)
{
	u8 buf[1] = { 0x00 };
	int status;

	struct surface_sam_ssh_rqst rqst = {
		.tc  = 0x01,
		.cid = 0x16,
		.iid = 0x00,
		.pri = SURFACE_SAM_PRIORITY_NORMAL,
		.snc = 0x01,
		.cdl = 0x00,
		.pld = NULL,
	};

	struct surface_sam_ssh_buf result = {
		result.cap = ARRAY_SIZE(buf),
		result.len = 0,
		result.data = buf,
	};

	ssh_dbg(ec, "pm: resuming system aggregator module\n");
	status = surface_sam_ssh_rqst_unlocked(ec, &rqst, &result);
	if (status)
		return status;

	/*
	 * The purpose of the return value of this request is unknown. Based on
	 * logging and experience, we expect it to be zero. No other value has
	 * been observed so far.
	 */
	if (buf[0] != 0x00) {
		ssh_warn(ec, "unexpected result while trying to resume EC: "
			 "0x%02x\n", buf[0]);
	}

	return 0;
}

/**
 * surface_sam_ssh_ec_suspend - Put the EC in a suspended mode:
 * @ec: the EC to suspend
 *
 * Tells the EC to enter a suspended mode. In this mode, events are quiesced
 * and the wake IRQ is armed (note that the wake IRQ does not fire if the EC
 * has not been suspended via this request). On some devices, the keyboard
 * backlight is turned off. Apart from this, the EC seems to continue to work
 * as normal, meaning requests sent to it are acknowledged and seem to be
 * correctly handled, including potential responses. Multiple repeated calls
 * to this function seem to be handled fine by the EC, after the first call,
 * the state will remain "suspended".
 *
 * Must be called with the EC initialized and its lock held.
 */
static int surface_sam_ssh_ec_suspend(struct sam_ssh_ec *ec)
{
	u8 buf[1] = { 0x00 };
	int status;

	struct surface_sam_ssh_rqst rqst = {
		.tc  = 0x01,
		.cid = 0x15,
		.iid = 0x00,
		.pri = SURFACE_SAM_PRIORITY_NORMAL,
		.snc = 0x01,
		.cdl = 0x00,
		.pld = NULL,
	};

	struct surface_sam_ssh_buf result = {
		result.cap = ARRAY_SIZE(buf),
		result.len = 0,
		result.data = buf,
	};

	ssh_dbg(ec, "pm: suspending system aggregator module\n");
	status = surface_sam_ssh_rqst_unlocked(ec, &rqst, &result);
	if (status)
		return status;

	/*
	 * The purpose of the return value of this request is unknown. Based on
	 * logging and experience, we expect it to be zero. No other value has
	 * been observed so far.
	 */
	if (buf[0] != 0x00) {
		ssh_warn(ec, "unexpected result while trying to suspend EC: "
			 "0x%02x\n", buf[0]);
	}

	return 0;
}


static int surface_sam_ssh_get_controller_version(struct sam_ssh_ec *ec, u32 *version)
{
	struct surface_sam_ssh_rqst rqst = {
		.tc  = 0x01,
		.cid = 0x13,
		.iid = 0x00,
		.pri = SURFACE_SAM_PRIORITY_NORMAL,
		.snc = 0x01,
		.cdl = 0x00,
		.pld = NULL,
	};

	struct surface_sam_ssh_buf result = {
		result.cap = sizeof(*version),
		result.len = 0,
		result.data = (u8 *)version,
	};

	*version = 0;
	return surface_sam_ssh_rqst_unlocked(ec, &rqst, &result);
}

static int surface_sam_ssh_log_controller_version(struct sam_ssh_ec *ec)
{
	u32 version, a, b, c;
	int status;

	status = surface_sam_ssh_get_controller_version(ec, &version);
	if (status)
		return status;

	a = (version >> 24) & 0xff;
	b = le16_to_cpu((version >> 8) & 0xffff);
	c = version & 0xff;

	dev_info(&ec->serdev->dev, "SAM controller version: %u.%u.%u\n",
		 a, b, c);
	return 0;
}


static inline bool ssh_is_valid_syn(const u8 *ptr)
{
	return ptr[0] == 0xaa && ptr[1] == 0x55;
}

static inline bool ssh_is_valid_ter(const u8 *ptr)
{
	return ptr[0] == 0xff && ptr[1] == 0xff;
}

static inline bool ssh_is_valid_crc(const u8 *begin, const u8 *end)
{
	u16 crc;

	crc = ssh_crc(begin, end - begin);
	return (end[0] == (crc & 0xff)) && (end[1] == (crc >> 8));
}


static void surface_sam_ssh_event_work_ack_handler(struct work_struct *_work)
{
	u8 buf[SSH_MSG_LEN_CTRL];
	struct msgbuf msgb;
	struct surface_sam_ssh_event *event;
	struct ssh_event_work *work;
	struct sam_ssh_ec *ec;
	struct device *dev;
	int status;

	work = container_of(_work, struct ssh_event_work, work_ack);
	event = &work->event;
	ec = work->ec;
	dev = &ec->serdev->dev;

	if (smp_load_acquire(&ec->state) == SSH_EC_INITIALIZED) {
		msgb_init(&msgb, buf, ARRAY_SIZE(buf));
		msgb_push_ack(&msgb, work->seq);

		status = ssh_send_msgbuf(ec, &msgb, SSH_TX_TIMEOUT);
		if (status)
			ssh_err(ec, SSH_EVENT_TAG "failed to send ACK: %d\n", status);
	}

	if (refcount_dec_and_test(&work->refcount))
		kfree(work);
}

static void surface_sam_ssh_event_work_evt_handler(struct work_struct *_work)
{
	struct ssh_event_work *work;
	struct srcu_notifier_head *nh;
	struct surface_sam_ssh_event *event;
	struct sam_ssh_ec *ec;
	struct device *dev;
	int status = 0, ncalls = 0;

	work = container_of(_work, struct ssh_event_work, work_evt);
	event = &work->event;
	ec = work->ec;
	dev = &ec->serdev->dev;

	nh = &ec->events.notifier[ssh_rqid_to_event(event->rqid)];
	status = __srcu_notifier_call_chain(nh, event->tc, event, -1, &ncalls);
	status = notifier_to_errno(status);

	if (status < 0)
		ssh_err(ec, "event: error handling event: %d\n", status);

	if (ncalls == 0) {
		ssh_warn(ec, "event: unhandled event"
			 " (rqid: 0x%04x, tc: 0x%02x, cid: 0x%02x)\n",
			 event->rqid, event->tc, event->cid);
	}

	if (refcount_dec_and_test(&work->refcount))
		kfree(work);
}

static void ssh_rx_handle_event(struct sam_ssh_ec *ec,
				const struct ssh_frame *frame,
				const struct ssh_command *command,
				const struct bufspan *command_data)
{
	struct ssh_event_work *work;

	work = kzalloc(sizeof(struct ssh_event_work) + command_data->len, GFP_ATOMIC);
	if (!work)
		return;

	refcount_set(&work->refcount, 1);
	work->ec         = ec;
	work->seq        = frame->seq;
	work->event.rqid = get_unaligned_le16(&command->rqid),
	work->event.tc   = command->tc;
	work->event.cid  = command->cid;
	work->event.iid  = command->iid;
	work->event.pri  = command->pri_in;
	work->event.len  = command_data->len;
	work->event.pld  = ((u8 *)work) + sizeof(struct ssh_event_work);
	memcpy(work->event.pld, command_data->ptr, command_data->len);

	// queue ACK for if required
	if (frame->type == SSH_FRAME_TYPE_DATA_SEQ) {
		refcount_set(&work->refcount, 2);
		INIT_WORK(&work->work_ack, surface_sam_ssh_event_work_ack_handler);
		queue_work(ec->events.queue_ack, &work->work_ack);
	}

	INIT_WORK(&work->work_evt, surface_sam_ssh_event_work_evt_handler);
	queue_work(ec->events.queue_evt, &work->work_evt);
}

static void ssh_rx_complete_command(struct sam_ssh_ec *ec,
				    const struct ssh_frame *frame,
				    const struct ssh_command *command,
				    const struct bufspan *command_data)
{
	struct ssh_receiver *rcv = &ec->receiver;
	struct ssh_fifo_packet packet;

	// check if we expect the message
	if (unlikely(rcv->state != SSH_RCV_COMMAND)) {
		ssh_dbg(ec, "rx: discarding message: command not expected\n");
		return;
	}

	// check if response is for our request
	if (unlikely(rcv->expect.rqid != get_unaligned_le16(&command->rqid))) {
		ssh_dbg(ec, "rx: discarding message: command not a match\n");
		return;
	}

	// we now have a valid & expected command message
	ssh_dbg(ec, "rx: valid command message received\n");

	packet.type = frame->type;
	packet.seq = frame->seq;
	packet.len = command_data->len;

	if (unlikely(kfifo_avail(&rcv->fifo) < sizeof(packet) + packet.len)) {
		ssh_warn(ec, "rx: dropping frame: not enough space in fifo (type = %d)\n",
			 frame->type);
		return;
	}

	kfifo_in(&rcv->fifo, &packet, sizeof(packet));
	kfifo_in(&rcv->fifo, command_data->ptr, command_data->len);

	rcv->state = SSH_RCV_DISCARD;
	complete(&rcv->signal);
}

static void ssh_receive_control_frame(struct sam_ssh_ec *ec,
				      const struct ssh_frame *frame)
{
	struct ssh_receiver *rcv = &ec->receiver;
	struct ssh_fifo_packet packet;

	// check if we expect the message
	if (unlikely(rcv->state != SSH_RCV_CONTROL)) {
		ssh_err(ec, "rx: discarding message: control not expected\n");
		return;
	}

	// check if it is for our request
	if (unlikely(frame->type == SSH_FRAME_TYPE_ACK && frame->seq != rcv->expect.seq)) {
		ssh_err(ec, "rx: discarding message: ACK does not match\n");
		return;
	}

	// we now have a valid & expected ACK/RETRY message
	ssh_dbg(ec, "rx: valid control message received (type: 0x%02x)\n", frame->type);

	packet.type = frame->type;
	packet.seq  = frame->seq;
	packet.len  = 0;

	if (unlikely(kfifo_avail(&rcv->fifo) < sizeof(packet))) {
		ssh_warn(ec, "rx: dropping frame: not enough space in fifo (type = %d)\n",
			 frame->type);
		return;
	}

	kfifo_in(&rcv->fifo, (u8 *) &packet, sizeof(packet));

	// update decoder state
	if (frame->type == SSH_FRAME_TYPE_ACK)
		rcv->state = rcv->expect.pld ? SSH_RCV_COMMAND : SSH_RCV_DISCARD;

	complete(&rcv->signal);
}

static void ssh_receive_command_frame(struct sam_ssh_ec *ec,
				      const struct ssh_frame *frame,
				      const struct ssh_command *command,
				      const struct bufspan *command_data)
{
	// check if we received an event notification
	if (ssh_rqid_is_event(get_unaligned_le16(&command->rqid))) {
		ssh_rx_handle_event(ec, frame, command, command_data);
	} else {
		ssh_rx_complete_command(ec, frame, command, command_data);
	}
}

static void ssh_receive_data_frame(struct sam_ssh_ec *ec,
				   const struct ssh_frame *frame,
				   const struct bufspan *payload)
{
	struct ssh_command *command;
	struct bufspan command_data;

	if (likely(payload->ptr[0] == SSH_PLD_TYPE_CMD)) {
		sshp_parse_command(ec, payload, &command, &command_data);
		if (unlikely(!command))
			return;

		ssh_receive_command_frame(ec, frame, command, &command_data);
	} else {
		ssh_err(ec, "rx: unknown frame payload type (type: 0x%02x)\n",
			payload->ptr[0]);
	}
}

static size_t ssh_eval_buf(struct sam_ssh_ec *ec, u8 *buf, size_t size)
{
	struct ssh_frame *frame;
	struct bufspan source = { .ptr = buf, .len = size };
	struct bufspan payload;
	size_t n;

	// parse and validate frame
	n = sshp_parse_frame(ec, &source, &frame, &payload);
	if (!frame)
		return n;

	switch (frame->type) {
	case SSH_FRAME_TYPE_ACK:
	case SSH_FRAME_TYPE_NAK:
		ssh_receive_control_frame(ec, frame);
		break;

	case SSH_FRAME_TYPE_DATA_SEQ:
	case SSH_FRAME_TYPE_DATA_NSQ:
		ssh_receive_data_frame(ec, frame, &payload);
		break;

	default:
		ssh_warn(ec, "rx: unknown frame type 0x%02x\n", frame->type);
	}

	return n + ssh_message_length(frame->len);
}

static inline int ssh_rx_thread_wait(struct sam_ssh_ec *ec)
{
	return wait_event_interruptible(ec->receiver.rcvb_wq,
					!kfifo_is_empty(&ec->receiver.rcvb)
					|| kthread_should_stop());
}

static int ssh_rx_threadfn(void *data)
{
	struct sam_ssh_ec *ec = data;
	struct ssh_receiver *rcv = &ec->receiver;

	while (!kthread_should_stop()) {
		size_t offs = 0;
		size_t n;

		ssh_rx_thread_wait(ec);
		if (kthread_should_stop())
			break;

		n = rcv->eval_buf.cap - rcv->eval_buf.len;
		n = kfifo_out(&rcv->rcvb, rcv->eval_buf.ptr + rcv->eval_buf.len, n);

		ssh_dbg(ec, "rx: received data (size: %zu)\n", n);
		print_hex_dump_debug("rx: ", DUMP_PREFIX_OFFSET, 16, 1,
				     rcv->eval_buf.ptr + rcv->eval_buf.len, n,
				     false);

		rcv->eval_buf.len += n;

		// evaluate buffer until we need more bytes or eval-buf is empty
		while (offs < rcv->eval_buf.len) {
			n = rcv->eval_buf.len - offs;
			n = ssh_eval_buf(ec, rcv->eval_buf.ptr + offs, n);
			if (n == 0)
				break;	// need more bytes

			offs += n;
		}

		// throw away the evaluated parts
		rcv->eval_buf.len -= offs;
		memmove(rcv->eval_buf.ptr, rcv->eval_buf.ptr + offs, rcv->eval_buf.len);
	}

	return 0;
}

static void ssh_rx_wakeup(struct sam_ssh_ec *ec)
{
	wake_up(&ec->receiver.rcvb_wq);
}

static inline int ssh_rx_start(struct sam_ssh_ec *ec)
{
	ec->receiver.thread = kthread_run(ssh_rx_threadfn, ec, "surface-sh-rx");
	if (IS_ERR(ec->receiver.thread))
		return PTR_ERR(ec->receiver.thread);

	return 0;
}

static inline int ssh_rx_stop(struct sam_ssh_ec *ec)
{
	return kthread_stop(ec->receiver.thread);
}

static int ssh_receive_buf(struct serdev_device *serdev,
			   const unsigned char *buf, size_t size)
{
	struct sam_ssh_ec *ec = serdev_device_get_drvdata(serdev);
	struct ssh_receiver *rcv = &ec->receiver;
	size_t n;

	n = kfifo_in(&rcv->rcvb, buf, size);
	if (n)
		ssh_rx_wakeup(ec);

	return n;
}


static const struct acpi_gpio_params gpio_ssh_wakeup_int = { 0, 0, false };
static const struct acpi_gpio_params gpio_ssh_wakeup     = { 1, 0, false };

static const struct acpi_gpio_mapping ssh_acpi_gpios[] = {
	{ "ssh_wakeup-int-gpio", &gpio_ssh_wakeup_int, 1 },
	{ "ssh_wakeup-gpio",     &gpio_ssh_wakeup,     1 },
	{ },
};

static irqreturn_t ssh_wake_irq_handler(int irq, void *dev_id)
{
	struct serdev_device *serdev = dev_id;

	dev_dbg(&serdev->dev, "pm: wake irq triggered\n");

	// TODO: Send GPIO callback command repeatedly to EC until callback
	//       returns 0x00. Return flag of callback is "has more events".
	//       Each time the command is sent, one event is "released". Once
	//       all events have been released (return = 0x00), the GPIO is
	//       re-armed.

	return IRQ_HANDLED;
}

static int ssh_setup_irq(struct serdev_device *serdev)
{
	const int irqf = IRQF_SHARED | IRQF_ONESHOT | IRQF_TRIGGER_RISING;
	struct gpio_desc *gpiod;
	int irq;
	int status;

	gpiod = gpiod_get(&serdev->dev, "ssh_wakeup-int", GPIOD_ASIS);
	if (IS_ERR(gpiod))
		return PTR_ERR(gpiod);

	irq = gpiod_to_irq(gpiod);
	gpiod_put(gpiod);

	if (irq < 0)
		return irq;

	status = request_threaded_irq(irq, NULL, ssh_wake_irq_handler,
				      irqf, "surface_sam_sh_wakeup", serdev);
	if (status)
		return status;

	return irq;
}


static acpi_status ssh_setup_from_resource(struct acpi_resource *rsc, void *ctx)
{
	struct serdev_device *serdev = ctx;
	struct acpi_resource_common_serialbus *serial;
	struct acpi_resource_uart_serialbus *uart;
	bool flow_control;
	int status = 0;

	if (rsc->type != ACPI_RESOURCE_TYPE_SERIAL_BUS)
		return AE_OK;

	serial = &rsc->data.common_serial_bus;
	if (serial->type != ACPI_RESOURCE_SERIAL_TYPE_UART)
		return AE_OK;

	uart = &rsc->data.uart_serial_bus;

	// set up serdev device
	serdev_device_set_baudrate(serdev, uart->default_baud_rate);

	// serdev currently only supports RTSCTS flow control
	if (uart->flow_control & SSH_SUPPORTED_FLOW_CONTROL_MASK) {
		dev_warn(&serdev->dev, "setup: unsupported flow control"
			 " (value: 0x%02x)\n", uart->flow_control);
	}

	// set RTSCTS flow control
	flow_control = uart->flow_control & ACPI_UART_FLOW_CONTROL_HW;
	serdev_device_set_flow_control(serdev, flow_control);

	// serdev currently only supports EVEN/ODD parity
	switch (uart->parity) {
	case ACPI_UART_PARITY_NONE:
		status = serdev_device_set_parity(serdev, SERDEV_PARITY_NONE);
		break;
	case ACPI_UART_PARITY_EVEN:
		status = serdev_device_set_parity(serdev, SERDEV_PARITY_EVEN);
		break;
	case ACPI_UART_PARITY_ODD:
		status = serdev_device_set_parity(serdev, SERDEV_PARITY_ODD);
		break;
	default:
		dev_warn(&serdev->dev, "setup: unsupported parity"
			 " (value: 0x%02x)\n", uart->parity);
		break;
	}

	if (status) {
		dev_err(&serdev->dev, "setup: failed to set parity"
			" (value: 0x%02x)\n", uart->parity);
		return status;
	}

	return AE_CTRL_TERMINATE;       // we've found the resource and are done
}


static int surface_sam_ssh_suspend(struct device *dev)
{
	struct sam_ssh_ec *ec;
	int status;

	dev_dbg(dev, "pm: suspending\n");

	ec = surface_sam_ssh_acquire_init();
	if (ec) {
		status = surface_sam_ssh_ec_suspend(ec);
		if (status) {
			surface_sam_ssh_release(ec);
			return status;
		}

		if (device_may_wakeup(dev)) {
			status = enable_irq_wake(ec->irq);
			if (status) {
				surface_sam_ssh_release(ec);
				return status;
			}

			ec->irq_wakeup_enabled = true;
		} else {
			ec->irq_wakeup_enabled = false;
		}

		smp_store_release(&ec->state, SSH_EC_SUSPENDED);
		surface_sam_ssh_release(ec);
	}

	return 0;
}

static int surface_sam_ssh_resume(struct device *dev)
{
	struct sam_ssh_ec *ec;
	int status;

	dev_dbg(dev, "pm: resuming\n");

	ec = surface_sam_ssh_acquire_init();
	if (ec) {
		smp_store_release(&ec->state, SSH_EC_INITIALIZED);

		if (ec->irq_wakeup_enabled) {
			status = disable_irq_wake(ec->irq);
			if (status) {
				surface_sam_ssh_release(ec);
				return status;
			}

			ec->irq_wakeup_enabled = false;
		}

		status = surface_sam_ssh_ec_resume(ec);
		if (status) {
			surface_sam_ssh_release(ec);
			return status;
		}

		surface_sam_ssh_release(ec);
	}

	return 0;
}

static SIMPLE_DEV_PM_OPS(surface_sam_ssh_pm_ops, surface_sam_ssh_suspend,
			 surface_sam_ssh_resume);


static const struct serdev_device_ops ssh_device_ops = {
	.receive_buf  = ssh_receive_buf,
	.write_wakeup = serdev_device_write_wakeup,
};

static int surface_sam_ssh_probe(struct serdev_device *serdev)
{
	struct sam_ssh_ec *ec;
	struct workqueue_struct *event_queue_ack;
	struct workqueue_struct *event_queue_evt;
	u8 *read_buf;
	u8 *recv_buf;
	u8 *eval_buf;
	acpi_handle *ssh = ACPI_HANDLE(&serdev->dev);
	acpi_status status;
	int irq, i;

	if (gpiod_count(&serdev->dev, NULL) < 0)
		return -ENODEV;

	status = devm_acpi_dev_add_driver_gpios(&serdev->dev, ssh_acpi_gpios);
	if (status)
		return status;

	// allocate buffers
	read_buf = kzalloc(SSH_READ_BUF_LEN, GFP_KERNEL);
	if (!read_buf) {
		status = -ENOMEM;
		goto err_read_buf;
	}

	recv_buf = kzalloc(SSH_RECV_BUF_LEN, GFP_KERNEL);
	if (!read_buf) {
		status = -ENOMEM;
		goto err_recv_buf;
	}

	eval_buf = kzalloc(SSH_EVAL_BUF_LEN, GFP_KERNEL);
	if (!eval_buf) {
		status = -ENOMEM;
		goto err_eval_buf;
	}

	event_queue_ack = create_singlethread_workqueue("surface_sh_ackq");
	if (!event_queue_ack) {
		status = -ENOMEM;
		goto err_ackq;
	}

	event_queue_evt = create_workqueue("surface_sh_evtq");
	if (!event_queue_evt) {
		status = -ENOMEM;
		goto err_evtq;
	}

	irq = ssh_setup_irq(serdev);
	if (irq < 0) {
		status = irq;
		goto err_irq;
	}

	// set up EC
	ec = surface_sam_ssh_acquire();
	if (smp_load_acquire(&ec->state) != SSH_EC_UNINITIALIZED) {
		dev_err(&serdev->dev, "embedded controller already initialized\n");
		surface_sam_ssh_release(ec);

		status = -EBUSY;
		goto err_busy;
	}

	ec->serdev = serdev;
	ec->irq    = irq;

	// initialize receiver
	init_completion(&ec->receiver.signal);
	init_waitqueue_head(&ec->receiver.rcvb_wq);
	kfifo_init(&ec->receiver.fifo, read_buf, SSH_READ_BUF_LEN);
	kfifo_init(&ec->receiver.rcvb, recv_buf, SSH_RECV_BUF_LEN);
	ec->receiver.eval_buf.ptr = eval_buf;
	ec->receiver.eval_buf.cap = SSH_EVAL_BUF_LEN;
	ec->receiver.eval_buf.len = 0;

	// initialize event handling
	ec->events.queue_ack = event_queue_ack;
	ec->events.queue_evt = event_queue_evt;

	for (i = 0; i < SURFACE_SAM_SSH_MAX_EVENT_ID; i++) {
		srcu_init_notifier_head(&ec->events.notifier[i]);
		ec->events.notifier_count[i] = 0;
	}

	serdev_device_set_drvdata(serdev, ec);

	smp_store_release(&ec->state, SSH_EC_INITIALIZED);

	status = ssh_rx_start(ec);
	if (status)
		goto err_rxstart;

	serdev_device_set_client_ops(serdev, &ssh_device_ops);
	status = serdev_device_open(serdev);
	if (status)
		goto err_open;

	status = acpi_walk_resources(ssh, METHOD_NAME__CRS,
				     ssh_setup_from_resource, serdev);
	if (ACPI_FAILURE(status))
		goto err_devinit;

	status = surface_sam_ssh_log_controller_version(ec);
	if (status)
		goto err_devinit;

	status = surface_sam_ssh_ec_resume(ec);
	if (status)
		goto err_devinit;

	surface_sam_ssh_release(ec);

	// TODO: The EC can wake up the system via the associated GPIO interrupt in
	// multiple situations. One of which is the remaining battery capacity
	// falling below a certain threshold. Normally, we should use the
	// device_init_wakeup function, however, the EC also seems to have other
	// reasons for waking up the system and it seems that Windows has
	// additional checks whether the system should be resumed. In short, this
	// causes some spourious unwanted wake-ups. For now let's thus default
	// power/wakeup to false.
	device_set_wakeup_capable(&serdev->dev, true);
	acpi_walk_dep_device_list(ssh);

	return 0;

err_devinit:
	serdev_device_close(serdev);
err_open:
	ssh_rx_stop(ec);
err_rxstart:
	smp_store_release(&ec->state, SSH_EC_UNINITIALIZED);
	serdev_device_set_drvdata(serdev, NULL);
	surface_sam_ssh_release(ec);
err_busy:
	free_irq(irq, serdev);
err_irq:
	destroy_workqueue(event_queue_evt);
err_evtq:
	destroy_workqueue(event_queue_ack);
err_ackq:
	kfree(eval_buf);
err_eval_buf:
	kfree(recv_buf);
err_recv_buf:
	kfree(read_buf);
err_read_buf:
	return status;
}

static void surface_sam_ssh_remove(struct serdev_device *serdev)
{
	struct sam_ssh_ec *ec;
	unsigned long flags;
	int status, i;

	ec = surface_sam_ssh_acquire_init();
	if (!ec)
		return;

	free_irq(ec->irq, serdev);

	// suspend EC and disable events
	status = surface_sam_ssh_ec_suspend(ec);
	if (status)
		dev_err(&serdev->dev, "failed to suspend EC: %d\n", status);

	// make sure all events (received up to now) have been properly handled
	flush_workqueue(ec->events.queue_ack);
	flush_workqueue(ec->events.queue_evt);

	status = ssh_rx_stop(ec);
	if (status)
		dev_err(&serdev->dev, "error stopping receiver thread: %d\n", status);

	// remove event handlers
	for (i = 0; i < SURFACE_SAM_SSH_MAX_EVENT_ID; i++) {
		srcu_cleanup_notifier_head(&ec->events.notifier[i]);
		ec->events.notifier_count[i] = 0;
	}

	// set device to deinitialized state
	smp_store_release(&ec->state, SSH_EC_UNINITIALIZED);
	ec->serdev = NULL;

	/*
	 * Flush any event that has not been processed yet to ensure we're not going to
	 * use the serial device any more (e.g. for ACKing).
	 */
	flush_workqueue(ec->events.queue_ack);
	flush_workqueue(ec->events.queue_evt);

	serdev_device_close(serdev);

	/*
	 * Only at this point, no new events can be received. Destroying the
	 * workqueue here flushes all remaining events. Those events will be
	 * silently ignored and neither ACKed nor any handler gets called.
	 */
	destroy_workqueue(ec->events.queue_ack);
	destroy_workqueue(ec->events.queue_evt);

	// free receiver
	spin_lock_irqsave(&ec->receiver.lock, flags);
	ec->receiver.state = SSH_RCV_DISCARD;
	kfifo_free(&ec->receiver.fifo);
	kfifo_free(&ec->receiver.rcvb);

	kfree(ec->receiver.eval_buf.ptr);
	ec->receiver.eval_buf.ptr = NULL;
	ec->receiver.eval_buf.cap = 0;
	ec->receiver.eval_buf.len = 0;
	spin_unlock_irqrestore(&ec->receiver.lock, flags);

	device_set_wakeup_capable(&serdev->dev, false);
	serdev_device_set_drvdata(serdev, NULL);
	surface_sam_ssh_release(ec);
}


static const struct acpi_device_id surface_sam_ssh_match[] = {
	{ "MSHW0084", 0 },
	{ },
};
MODULE_DEVICE_TABLE(acpi, surface_sam_ssh_match);

static struct serdev_device_driver surface_sam_ssh = {
	.probe = surface_sam_ssh_probe,
	.remove = surface_sam_ssh_remove,
	.driver = {
		.name = "surface_sam_ssh",
		.acpi_match_table = surface_sam_ssh_match,
		.pm = &surface_sam_ssh_pm_ops,
		.probe_type = PROBE_PREFER_ASYNCHRONOUS,
	},
};


static int __init surface_sam_ssh_init(void)
{
	return serdev_device_driver_register(&surface_sam_ssh);
}

static void __exit surface_sam_ssh_exit(void)
{
	serdev_device_driver_unregister(&surface_sam_ssh);
}

/*
 * Ensure that the driver is loaded late due to some issues with the UART
 * communication. Specifically, we want to ensure that DMA is ready and being
 * used. Not using DMA can result in spurious communication failures,
 * especially during boot, which among other things will result in wrong
 * battery information (via ACPI _BIX) being displayed. Using a late init_call
 * instead of the normal module_init gives the DMA subsystem time to
 * initialize and via that results in a more stable communication, avoiding
 * such failures.
 */
late_initcall(surface_sam_ssh_init);
module_exit(surface_sam_ssh_exit);

MODULE_AUTHOR("Maximilian Luz <luzmaximilian@gmail.com>");
MODULE_DESCRIPTION("Surface Serial Hub Driver for 5th Generation Surface Devices");
MODULE_LICENSE("GPL");

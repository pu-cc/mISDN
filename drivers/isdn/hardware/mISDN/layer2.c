/* $Id: layer2.c,v 0.3 2001/02/13 10:42:55 kkeil Exp $
 *
 * Author       Karsten Keil (keil@isdn4linux.de)
 *
 *		This file is (c) under GNU PUBLIC LICENSE
 *		For changes and modifications please read
 *		../../../Documentation/isdn/HiSax.cert
 *
 */
#include <linux/module.h>
#include "hisaxl2.h"
#include "helper.h"
#include "debug.h"

const char *l2_revision = "$Revision: 0.3 $";

static void l2m_debug(struct FsmInst *fi, char *fmt, ...);

static layer2_t *l2list = NULL;
static int debug = 0;
static hisaxobject_t isdnl2;

static
struct Fsm l2fsm = {NULL, 0, 0, NULL, NULL};

enum {
	ST_L2_1,
	ST_L2_2,
	ST_L2_3,
	ST_L2_4,
	ST_L2_5,
	ST_L2_6,
	ST_L2_7,
	ST_L2_8,
};

#define L2_STATE_COUNT (ST_L2_8+1)

static char *strL2State[] =
{
	"ST_L2_1",
	"ST_L2_2",
	"ST_L2_3",
	"ST_L2_4",
	"ST_L2_5",
	"ST_L2_6",
	"ST_L2_7",
	"ST_L2_8",
};

enum {
	EV_L2_UI,
	EV_L2_SABME,
	EV_L2_DISC,
	EV_L2_DM,
	EV_L2_UA,
	EV_L2_FRMR,
	EV_L2_SUPER,
	EV_L2_I,
	EV_L2_DL_DATA,
	EV_L2_ACK_PULL,
	EV_L2_DL_UNITDATA,
	EV_L2_DL_ESTABLISH_REQ,
	EV_L2_DL_RELEASE_REQ,
	EV_L2_MDL_ASSIGN,
	EV_L2_MDL_REMOVE,
	EV_L2_MDL_ERROR,
	EV_L1_DEACTIVATE,
	EV_L2_T200,
	EV_L2_T203,
	EV_L2_SET_OWN_BUSY,
	EV_L2_CLEAR_OWN_BUSY,
	EV_L2_FRAME_ERROR,
};

#define L2_EVENT_COUNT (EV_L2_FRAME_ERROR+1)

static char *strL2Event[] =
{
	"EV_L2_UI",
	"EV_L2_SABME",
	"EV_L2_DISC",
	"EV_L2_DM",
	"EV_L2_UA",
	"EV_L2_FRMR",
	"EV_L2_SUPER",
	"EV_L2_I",
	"EV_L2_DL_DATA",
	"EV_L2_ACK_PULL",
	"EV_L2_DL_UNITDATA",
	"EV_L2_DL_ESTABLISH_REQ",
	"EV_L2_DL_RELEASE_REQ",
	"EV_L2_MDL_ASSIGN",
	"EV_L2_MDL_REMOVE",
	"EV_L2_MDL_ERROR",
	"EV_L1_DEACTIVATE",
	"EV_L2_T200",
	"EV_L2_T203",
	"EV_L2_SET_OWN_BUSY",
	"EV_L2_CLEAR_OWN_BUSY",
	"EV_L2_FRAME_ERROR",
};

static int l2addrsize(layer2_t *l2);

static int
l2up(layer2_t *l2, u_int prim, u_int nr, int dtyp, void *arg) {
	hisaxif_t *up = &l2->inst.up;
	int err = -EINVAL;

	if(up)
		err = up->func(up, prim, nr, dtyp, arg);
	return(err);
}

static int
l2down(layer2_t *l2, u_int prim, u_int nr, int dtyp, void *arg) {
	hisaxif_t *down = &l2->inst.down;
	int err = -EINVAL;

	if (down) {
		if (prim == PH_DATA_REQ) {
			if (test_and_set_bit(FLG_L1_BUSY, &l2->flag)) {
				skb_queue_tail(&l2->ph_queue, arg);
				return(0);
			}
			l2->ph_skb = arg;
			l2->ph_nr = nr;
		}
		err = down->func(down, prim, nr, dtyp, arg);
	} else
		printk(KERN_WARNING "l2down: no down func prim %x\n", prim);
	return(err);
}

static int
ph_data_confirm(hisaxif_t *up, u_int nr, int dtyp, void *arg) {
	layer2_t *l2 = up->fdata;
	struct sk_buff *skb = arg; 
	hisaxif_t *next = up->next;
	int ret = -EAGAIN;

	if (test_bit(FLG_L1_BUSY, &l2->flag)) {
		if (skb == l2->ph_skb) {
			if ((skb = skb_dequeue(&l2->ph_queue))) {
				l2->ph_skb = skb;
				l2->ph_nr = l2->msgnr++;
				l2down(l2, PH_DATA_REQ, l2->ph_nr, dtyp, skb);
			} else
				l2->ph_skb = NULL;
			if (next)
				ret = next->func(next, PH_DATA_CNF, nr, dtyp, arg);
			if (ret) {
				dev_kfree_skb(arg);
				ret = 0;
			}
			if (!l2->ph_skb) {
				test_and_clear_bit(FLG_L1_BUSY, &l2->flag);
				FsmEvent(&l2->l2m, EV_L2_ACK_PULL, NULL);
			}
		}
	}
	if (ret && next)
		ret = next->func(next, PH_DATA_CNF, nr, dtyp, arg);
	if (!test_and_set_bit(FLG_L1_BUSY, &l2->flag)) {
		if ((skb = skb_dequeue(&l2->ph_queue))) {
			l2->ph_skb = skb;
			l2->ph_nr = l2->msgnr++;
			l2down(l2, PH_DATA_REQ, l2->ph_nr, dtyp, skb);
		} else
			test_and_clear_bit(FLG_L1_BUSY, &l2->flag);
	}
	return(ret);
}

static int
l2mgr(layer2_t *l2, u_int prim, void *arg) {
	printk(KERN_DEBUG "l2mgr: prim %x %p\n", prim, arg);
	return(0);
}

static void
set_peer_busy(layer2_t *l2) {
	test_and_set_bit(FLG_PEER_BUSY, &l2->flag);
	if (skb_queue_len(&l2->i_queue) || skb_queue_len(&l2->ui_queue))
		test_and_set_bit(FLG_L2BLOCK, &l2->flag);
}

static void
clear_peer_busy(layer2_t *l2) {
	if (test_and_clear_bit(FLG_PEER_BUSY, &l2->flag))
		test_and_clear_bit(FLG_L2BLOCK, &l2->flag);
}

static void
InitWin(layer2_t *l2)
{
	int i;

	for (i = 0; i < MAX_WINDOW; i++)
		l2->windowar[i] = NULL;
}

static int
freewin(layer2_t *l2)
{
	int i, cnt = 0;

	for (i = 0; i < MAX_WINDOW; i++) {
		if (l2->windowar[i]) {
			cnt++;
			dev_kfree_skb(l2->windowar[i]);
			l2->windowar[i] = NULL;
		}
	}
	return cnt;
}

static void
ReleaseWin(layer2_t *l2)
{
	int cnt;

	if((cnt = freewin(l2)))
		printk(KERN_WARNING "isdnl2 freed %d skbuffs in release\n", cnt);
}

inline unsigned int
cansend(layer2_t *l2)
{
	unsigned int p1;

	if(test_bit(FLG_MOD128, &l2->flag))
		p1 = (l2->vs - l2->va) % 128;
	else
		p1 = (l2->vs - l2->va) % 8;
	return ((p1 < l2->window) && !test_bit(FLG_PEER_BUSY, &l2->flag));
}

inline void
clear_exception(layer2_t *l2)
{
	test_and_clear_bit(FLG_ACK_PEND, &l2->flag);
	test_and_clear_bit(FLG_REJEXC, &l2->flag);
	test_and_clear_bit(FLG_OWN_BUSY, &l2->flag);
	clear_peer_busy(l2);
}

inline int
l2headersize(layer2_t *l2, int ui)
{
	return (((test_bit(FLG_MOD128, &l2->flag) && (!ui)) ? 2 : 1) +
		(test_bit(FLG_LAPD, &l2->flag) ? 2 : 1));
}

inline int
l2addrsize(layer2_t *l2)
{
	return (test_bit(FLG_LAPD, &l2->flag) ? 2 : 1);
}

static int
sethdraddr(layer2_t *l2, u_char * header, int rsp)
{
	u_char *ptr = header;
	int crbit = rsp;

	if (test_bit(FLG_LAPD, &l2->flag)) {
		*ptr++ = (l2->sapi << 2) | (rsp ? 2 : 0);
		*ptr++ = (l2->tei << 1) | 1;
		return (2);
	} else {
		if (test_bit(FLG_ORIG, &l2->flag))
			crbit = !crbit;
		if (crbit)
			*ptr++ = 1;
		else
			*ptr++ = 3;
		return (1);
	}
}

inline static void
enqueue_super(layer2_t *l2, struct sk_buff *skb)
{
	l2down(l2, PH_DATA | REQUEST, l2->msgnr++, DTYPE_SKB, skb);
}

#define enqueue_ui(a, b) enqueue_super(a, b)

inline int
IsUI(u_char * data)
{
	return ((data[0] & 0xef) == UI);
}

inline int
IsUA(u_char * data)
{
	return ((data[0] & 0xef) == UA);
}

inline int
IsDM(u_char * data)
{
	return ((data[0] & 0xef) == DM);
}

inline int
IsDISC(u_char * data)
{
	return ((data[0] & 0xef) == DISC);
}

inline int
IsRR(u_char * data, layer2_t *l2)
{
	if (test_bit(FLG_MOD128, &l2->flag))
		return (data[0] == RR);
	else
		return ((data[0] & 0xf) == 1);
}

inline int
IsSFrame(u_char * data, layer2_t *l2)
{
	register u_char d = *data;
	
	if (!test_bit(FLG_MOD128, &l2->flag))
		d &= 0xf;
	return(((d & 0xf3) == 1) && ((d & 0x0c) != 0x0c));
}

inline int
IsSABME(u_char * data, layer2_t *l2)
{
	u_char d = data[0] & ~0x10;

	return (test_bit(FLG_MOD128, &l2->flag) ? d == SABME : d == SABM);
}

inline int
IsREJ(u_char * data, layer2_t *l2)
{
	return (test_bit(FLG_MOD128, &l2->flag) ? data[0] == REJ : (data[0] & 0xf) == REJ);
}

inline int
IsFRMR(u_char * data)
{
	return ((data[0] & 0xef) == FRMR);
}

inline int
IsRNR(u_char * data, layer2_t *l2)
{
	return (test_bit(FLG_MOD128, &l2->flag) ? data[0] == RNR : (data[0] & 0xf) == RNR);
}

int
iframe_error(layer2_t *l2, struct sk_buff *skb)
{
	int i = l2addrsize(l2) + (test_bit(FLG_MOD128, &l2->flag) ? 2 : 1);
	int rsp = *skb->data & 0x2;

	if (test_bit(FLG_ORIG, &l2->flag))
		rsp = !rsp;
	if (rsp)
		return 'L';
	if (skb->len < i)
		return 'N';
	if ((skb->len - i) > l2->maxlen)
		return 'O';
	return 0;
}

int
super_error(layer2_t *l2, struct sk_buff *skb)
{
	if (skb->len != l2addrsize(l2) +
	    (test_bit(FLG_MOD128, &l2->flag) ? 2 : 1))
		return 'N';
	return 0;
}

int
unnum_error(layer2_t *l2, struct sk_buff *skb, int wantrsp)
{
	int rsp = (*skb->data & 0x2) >> 1;
	if (test_bit(FLG_ORIG, &l2->flag))
		rsp = !rsp;
	if (rsp != wantrsp)
		return 'L';
	if (skb->len != l2addrsize(l2) + 1)
		return 'N';
	return 0;
}

int
UI_error(layer2_t *l2, struct sk_buff *skb)
{
	int rsp = *skb->data & 0x2;
	if (test_bit(FLG_ORIG, &l2->flag))
		rsp = !rsp;
	if (rsp)
		return 'L';
	if (skb->len > l2->maxlen + l2addrsize(l2) + 1)
		return 'O';
	return 0;
}

int
FRMR_error(layer2_t *l2, struct sk_buff *skb)
{
	int headers = l2addrsize(l2) + 1;
	u_char *datap = skb->data + headers;
	int rsp = *skb->data & 0x2;

	if (test_bit(FLG_ORIG, &l2->flag))
		rsp = !rsp;
	if (!rsp)
		return 'L';
	if (test_bit(FLG_MOD128, &l2->flag)) {
		if (skb->len < headers + 5)
			return 'N';
		else
			l2m_debug(&l2->l2m, "FRMR information %2x %2x %2x %2x %2x",
				datap[0], datap[1], datap[2],
				datap[3], datap[4]);
	} else {
		if (skb->len < headers + 3)
			return 'N';
		else
			l2m_debug(&l2->l2m, "FRMR information %2x %2x %2x",
				datap[0], datap[1], datap[2]);
	}
	return 0;
}

static unsigned int
legalnr(layer2_t *l2, unsigned int nr)
{
	if(test_bit(FLG_MOD128, &l2->flag))
		return ((nr - l2->va) % 128) <= ((l2->vs - l2->va) % 128);
	else
		return ((nr - l2->va) % 8) <= ((l2->vs - l2->va) % 8);
}

static void
setva(layer2_t *l2, unsigned int nr)
{
	int len;

	while (l2->va != nr) {
		(l2->va)++;
		if(test_bit(FLG_MOD128, &l2->flag))
			l2->va %= 128;
		else
			l2->va %= 8;
		len = l2->windowar[l2->sow]->len;
		if (PACKET_NOACK == l2->windowar[l2->sow]->pkt_type)
			len = -1;
		dev_kfree_skb(l2->windowar[l2->sow]);
		l2->windowar[l2->sow] = NULL;
		l2->sow = (l2->sow + 1) % l2->window;
//		if (st->lli.l2writewakeup && (len >=0))
//			st->lli.l2writewakeup(st, len);
	}
}

static void
send_uframe(layer2_t *l2, u_char cmd, u_char cr)
{
	struct sk_buff *skb;
	u_char tmp[MAX_HEADER_LEN];
	int i;

	i = sethdraddr(l2, tmp, cr);
	tmp[i++] = cmd;
	if (!(skb = alloc_skb(i, GFP_ATOMIC))) {
		printk(KERN_WARNING "isdnl2 can't alloc sbbuff for send_uframe\n");
		return;
	}
	memcpy(skb_put(skb, i), tmp, i);
	enqueue_super(l2, skb);
}

inline u_char
get_PollFlag(layer2_t *l2, struct sk_buff * skb)
{
	return (skb->data[l2addrsize(l2)] & 0x10);
}

inline u_char
get_PollFlagFree(layer2_t *l2, struct sk_buff *skb)
{
	u_char PF;

	PF = get_PollFlag(l2, skb);
	dev_kfree_skb(skb);
	return (PF);
}

inline void
start_t200(layer2_t *l2, int i)
{
	FsmAddTimer(&l2->t200, l2->T200, EV_L2_T200, NULL, i);
	test_and_set_bit(FLG_T200_RUN, &l2->flag);
}

inline void
restart_t200(layer2_t *l2, int i)
{
	FsmRestartTimer(&l2->t200, l2->T200, EV_L2_T200, NULL, i);
	test_and_set_bit(FLG_T200_RUN, &l2->flag);
}

inline void
stop_t200(layer2_t *l2, int i)
{
	if(test_and_clear_bit(FLG_T200_RUN, &l2->flag))
		FsmDelTimer(&l2->t200, i);
}

inline void
st5_dl_release_l2l3(layer2_t *l2)
{
	int pr;
	int nr;

	if (test_and_clear_bit(FLG_PEND_REL, &l2->flag)) {
		pr = DL_RELEASE | CONFIRM;
		nr = l2->last_nr;
	} else {
		pr = DL_RELEASE | INDICATION;
		nr = l2->msgnr++;
	}
	l2up(l2, pr, nr, 0, NULL);
}

inline void
lapb_dl_release_l2l3(layer2_t *l2, int f, u_int nr)
{
	if (test_bit(FLG_LAPB, &l2->flag))
		l2down(l2, PH_DEACTIVATE | REQUEST, l2->msgnr++, 0, NULL);
	l2up(l2, DL_RELEASE | f, nr, 0, NULL);
}

static void
establishlink(struct FsmInst *fi)
{
	layer2_t *l2 = fi->userdata;
	u_char cmd;

	clear_exception(l2);
	l2->rc = 0;
	cmd = (test_bit(FLG_MOD128, &l2->flag) ? SABME : SABM) | 0x10;
	send_uframe(l2, cmd, CMD);
	FsmDelTimer(&l2->t203, 1);
	restart_t200(l2, 1);
	test_and_clear_bit(FLG_PEND_REL, &l2->flag);
	freewin(l2);
	FsmChangeState(fi, ST_L2_5);
}

static void
l2_mdl_error_ua(struct FsmInst *fi, int event, void *arg)
{
	struct sk_buff *skb = arg;
	layer2_t *l2 = fi->userdata;

	if (get_PollFlagFree(l2, skb))
		l2mgr(l2, MDL_ERROR | INDICATION, (void *) 'C');
	else
		l2mgr(l2, MDL_ERROR | INDICATION, (void *) 'D');
}

static void
l2_mdl_error_dm(struct FsmInst *fi, int event, void *arg)
{
	struct sk_buff *skb = arg;
	layer2_t *l2 = fi->userdata;

	if (get_PollFlagFree(l2, skb))
		l2mgr(l2, MDL_ERROR | INDICATION, (void *) 'B');
	else {
		l2mgr(l2, MDL_ERROR | INDICATION, (void *) 'E');
		establishlink(fi);
		test_and_clear_bit(FLG_L3_INIT, &l2->flag);
	}
}

static void
l2_st8_mdl_error_dm(struct FsmInst *fi, int event, void *arg)
{
	struct sk_buff *skb = arg;
	layer2_t *l2 = fi->userdata;

	if (get_PollFlagFree(l2, skb))
		l2mgr(l2, MDL_ERROR | INDICATION, (void *) 'B');
	else {
		l2mgr(l2, MDL_ERROR | INDICATION, (void *) 'E');
	}
	establishlink(fi);
	test_and_clear_bit(FLG_L3_INIT, &l2->flag);
}

static void
l2_go_st3(struct FsmInst *fi, int event, void *arg)
{
	FsmChangeState(fi, ST_L2_3); 
}

static void
l2_mdl_assign(struct FsmInst *fi, int event, void *arg)
{
	layer2_t *l2 = fi->userdata;

	FsmChangeState(fi, ST_L2_3); 
	l2_tei(l2->tm, MDL_ASSIGN | INDICATION, l2->msgnr++, 0, NULL);
}

static void
l2_queue_ui_assign(struct FsmInst *fi, int event, void *arg)
{
	layer2_t *l2 = fi->userdata;
	struct sk_buff *skb = arg;

	skb_queue_tail(&l2->ui_queue, skb);
	FsmChangeState(fi, ST_L2_2);
	l2_tei(l2->tm, MDL_ASSIGN | INDICATION, l2->msgnr++, 0, NULL);
}

static void
l2_queue_ui(struct FsmInst *fi, int event, void *arg)
{
	layer2_t *l2 = fi->userdata;
	struct sk_buff *skb = arg;

	skb_queue_tail(&l2->ui_queue, skb);
}

static void
tx_ui(layer2_t *l2)
{
	struct sk_buff *skb;
	u_char header[MAX_HEADER_LEN];
	int i;

	i = sethdraddr(l2, header, CMD);
	header[i++] = UI;
	while ((skb = skb_dequeue(&l2->ui_queue))) {
		memcpy(skb_push(skb, i), header, i);
		enqueue_ui(l2, skb);
	}
}

static void
l2_send_ui(struct FsmInst *fi, int event, void *arg)
{
	layer2_t *l2 = fi->userdata;
	struct sk_buff *skb = arg;

	skb_queue_tail(&l2->ui_queue, skb);
	tx_ui(l2);
}

static void
l2_got_ui(struct FsmInst *fi, int event, void *arg)
{
	layer2_t *l2 = fi->userdata;
	struct sk_buff *skb = arg;

	skb_pull(skb, l2headersize(l2, 1));
	l2up(l2, DL_UNITDATA | INDICATION, l2->msgnr++, DTYPE_SKB, skb);
/*	^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^
 *		in states 1-3 for broadcast
 */
}

static void
l2_establish(struct FsmInst *fi, int event, void *arg)
{
	layer2_t *l2 = fi->userdata;

	establishlink(fi);
	test_and_set_bit(FLG_L3_INIT, &l2->flag);
}

static void
l2_discard_i_setl3(struct FsmInst *fi, int event, void *arg)
{
	layer2_t *l2 = fi->userdata;

	discard_queue(&l2->i_queue);
	test_and_set_bit(FLG_L3_INIT, &l2->flag);
	test_and_clear_bit(FLG_PEND_REL, &l2->flag);
}

static void
l2_l3_reestablish(struct FsmInst *fi, int event, void *arg)
{
	layer2_t *l2 = fi->userdata;

	discard_queue(&l2->i_queue);
	establishlink(fi);
	test_and_set_bit(FLG_L3_INIT, &l2->flag);
}

static void
l2_release(struct FsmInst *fi, int event, void *arg)
{
	layer2_t *l2 = fi->userdata;

	l2up(l2, DL_RELEASE | CONFIRM, l2->last_nr, 0, NULL);
}

static void
l2_pend_rel(struct FsmInst *fi, int event, void *arg)
{
	layer2_t *l2 = fi->userdata;

	test_and_set_bit(FLG_PEND_REL, &l2->flag);
}

static void
l2_disconnect(struct FsmInst *fi, int event, void *arg)
{
	layer2_t *l2 = fi->userdata;

	discard_queue(&l2->i_queue);
	freewin(l2);
	FsmChangeState(fi, ST_L2_6);
	l2->rc = 0;
	send_uframe(l2, DISC | 0x10, CMD);
	FsmDelTimer(&l2->t203, 1);
	restart_t200(l2, 2);
}

static void
l2_start_multi(struct FsmInst *fi, int event, void *arg)
{
	layer2_t *l2 = fi->userdata;
	struct sk_buff *skb = arg;

	send_uframe(l2, UA | get_PollFlagFree(l2, skb), RSP);

	clear_exception(l2);
	l2->vs = 0;
	l2->va = 0;
	l2->vr = 0;
	l2->sow = 0;
	FsmChangeState(fi, ST_L2_7);
	FsmAddTimer(&l2->t203, l2->T203, EV_L2_T203, NULL, 3);

	l2up(l2, DL_ESTABLISH | INDICATION, l2->msgnr++, 0, NULL);
}

static void
l2_send_UA(struct FsmInst *fi, int event, void *arg)
{
	layer2_t *l2 = fi->userdata;
	struct sk_buff *skb = arg;

	send_uframe(l2, UA | get_PollFlagFree(l2, skb), RSP);
}

static void
l2_send_DM(struct FsmInst *fi, int event, void *arg)
{
	layer2_t *l2 = fi->userdata;
	struct sk_buff *skb = arg;

	send_uframe(l2, DM | get_PollFlagFree(l2, skb), RSP);
}

static void
l2_restart_multi(struct FsmInst *fi, int event, void *arg)
{
	layer2_t *l2 = fi->userdata;
	struct sk_buff *skb = arg;
	int est = 0;

	send_uframe(l2, UA | get_PollFlagFree(l2, skb), RSP);

	l2mgr(l2, MDL_ERROR | INDICATION, (void *) 'F');

	if (l2->vs != l2->va) {
		discard_queue(&l2->i_queue);
		est = 1;
	}

	clear_exception(l2);
	l2->vs = 0;
	l2->va = 0;
	l2->vr = 0;
	l2->sow = 0;
	FsmChangeState(fi, ST_L2_7);
	stop_t200(l2, 3);
	FsmRestartTimer(&l2->t203, l2->T203, EV_L2_T203, NULL, 3);

	if (est)
		l2up(l2, DL_ESTABLISH | INDICATION, l2->msgnr++, 0, NULL);

	if (skb_queue_len(&l2->i_queue) && cansend(l2))
		FsmEvent(fi, EV_L2_ACK_PULL, NULL);
}

static void
l2_stop_multi(struct FsmInst *fi, int event, void *arg)
{
	layer2_t *l2 = fi->userdata;
	struct sk_buff *skb = arg;

	FsmChangeState(fi, ST_L2_4);
	FsmDelTimer(&l2->t203, 3);
	stop_t200(l2, 4);

	send_uframe(l2, UA | get_PollFlagFree(l2, skb), RSP);
	discard_queue(&l2->i_queue);
	freewin(l2);
	lapb_dl_release_l2l3(l2, INDICATION, l2->msgnr++);
}

static void
l2_connected(struct FsmInst *fi, int event, void *arg)
{
	layer2_t *l2 = fi->userdata;
	struct sk_buff *skb = arg;
	int pr=-1;
	u_int nr = l2->last_nr;

	if (!get_PollFlag(l2, skb)) {
		l2_mdl_error_ua(fi, event, arg);
		return;
	}
	dev_kfree_skb(skb);
	if (test_and_clear_bit(FLG_PEND_REL, &l2->flag))
		l2_disconnect(fi, event, arg);
	if (test_and_clear_bit(FLG_L3_INIT, &l2->flag)) {
		pr = DL_ESTABLISH | CONFIRM;
	} else if (l2->vs != l2->va) {
		discard_queue(&l2->i_queue);
		pr = DL_ESTABLISH | INDICATION;
		nr = l2->msgnr++;
	}
	stop_t200(l2, 5);

	l2->vr = 0;
	l2->vs = 0;
	l2->va = 0;
	l2->sow = 0;
	FsmChangeState(fi, ST_L2_7);
	FsmAddTimer(&l2->t203, l2->T203, EV_L2_T203, NULL, 4);
	if (pr != -1)
		l2up(l2, pr, nr, 0, NULL);

	if (skb_queue_len(&l2->i_queue) && cansend(l2))
		FsmEvent(fi, EV_L2_ACK_PULL, NULL);
}

static void
l2_released(struct FsmInst *fi, int event, void *arg)
{
	layer2_t *l2 = fi->userdata;
	struct sk_buff *skb = arg;

	if (!get_PollFlag(l2, skb)) {
		l2_mdl_error_ua(fi, event, arg);
		return;
	}
	dev_kfree_skb(skb);
	stop_t200(l2, 6);
	lapb_dl_release_l2l3(l2, CONFIRM, l2->last_nr);
	FsmChangeState(fi, ST_L2_4);
}

static void
l2_reestablish(struct FsmInst *fi, int event, void *arg)
{
	layer2_t *l2 = fi->userdata;
	struct sk_buff *skb = arg;

	if (!get_PollFlagFree(l2, skb)) {
		establishlink(fi);
		test_and_set_bit(FLG_L3_INIT, &l2->flag);
	}
}

static void
l2_st5_dm_release(struct FsmInst *fi, int event, void *arg)
{
	layer2_t *l2 = fi->userdata;
	struct sk_buff *skb = arg;

	if (get_PollFlagFree(l2, skb)) {
		stop_t200(l2, 7);
	 	if (!test_bit(FLG_L3_INIT, &l2->flag))
			discard_queue(&l2->i_queue);
		if (test_bit(FLG_LAPB, &l2->flag))
			l2down(l2, PH_DEACTIVATE | REQUEST, l2->msgnr++, 0, NULL);
		st5_dl_release_l2l3(l2);
		FsmChangeState(fi, ST_L2_4);
	}
}

static void
l2_st6_dm_release(struct FsmInst *fi, int event, void *arg)
{
	layer2_t *l2 = fi->userdata;
	struct sk_buff *skb = arg;

	if (get_PollFlagFree(l2, skb)) {
		stop_t200(l2, 8);
		lapb_dl_release_l2l3(l2, CONFIRM, l2->last_nr);
		FsmChangeState(fi, ST_L2_4);
	}
}

inline void
enquiry_cr(layer2_t *l2, u_char typ, u_char cr, u_char pf)
{
	struct sk_buff *skb;
	u_char tmp[MAX_HEADER_LEN];
	int i;

	i = sethdraddr(l2, tmp, cr);
	if (test_bit(FLG_MOD128, &l2->flag)) {
		tmp[i++] = typ;
		tmp[i++] = (l2->vr << 1) | (pf ? 1 : 0);
	} else
		tmp[i++] = (l2->vr << 5) | typ | (pf ? 0x10 : 0);
	if (!(skb = alloc_skb(i, GFP_ATOMIC))) {
		printk(KERN_WARNING "isdnl2 can't alloc sbbuff for enquiry_cr\n");
		return;
	}
	memcpy(skb_put(skb, i), tmp, i);
	enqueue_super(l2, skb);
}

inline void
enquiry_response(layer2_t *l2)
{
	if (test_bit(FLG_OWN_BUSY, &l2->flag))
		enquiry_cr(l2, RNR, RSP, 1);
	else
		enquiry_cr(l2, RR, RSP, 1);
	test_and_clear_bit(FLG_ACK_PEND, &l2->flag);
}

inline void
transmit_enquiry(layer2_t *l2)
{
	if (test_bit(FLG_OWN_BUSY, &l2->flag))
		enquiry_cr(l2, RNR, CMD, 1);
	else
		enquiry_cr(l2, RR, CMD, 1);
	test_and_clear_bit(FLG_ACK_PEND, &l2->flag);
	start_t200(l2, 9);
}


static void
nrerrorrecovery(struct FsmInst *fi)
{
	layer2_t *l2 = fi->userdata;

	l2mgr(l2, MDL_ERROR | INDICATION, (void *) 'J');
	establishlink(fi);
	test_and_clear_bit(FLG_L3_INIT, &l2->flag);
}

static void
invoke_retransmission(layer2_t *l2, unsigned int nr)
{
	unsigned int p1;

	if (l2->vs != nr) {
		while (l2->vs != nr) {
			(l2->vs)--;
			if(test_bit(FLG_MOD128, &l2->flag)) {
				l2->vs %= 128;
				p1 = (l2->vs - l2->va) % 128;
			} else {
				l2->vs %= 8;
				p1 = (l2->vs - l2->va) % 8;
			}
			p1 = (p1 + l2->sow) % l2->window;
//			if (test_bit(FLG_LAPB, &l2->flag))
//				st->l1.bcs->tx_cnt += l2->windowar[p1]->len + l2headersize(l2, 0);
			skb_queue_head(&l2->i_queue, l2->windowar[p1]);
			l2->windowar[p1] = NULL;
		}
		FsmEvent(&l2->l2m, EV_L2_ACK_PULL, NULL);
	}
}

static void
l2_st7_got_super(struct FsmInst *fi, int event, void *arg)
{
	layer2_t *l2 = fi->userdata;
	struct sk_buff *skb = arg;
	int PollFlag, rsp, typ = RR;
	unsigned int nr;

	rsp = *skb->data & 0x2;
	if (test_bit(FLG_ORIG, &l2->flag))
		rsp = !rsp;

	skb_pull(skb, l2addrsize(l2));
	if (IsRNR(skb->data, l2)) {
		set_peer_busy(l2);
		typ = RNR;
	} else
		clear_peer_busy(l2);
	if (IsREJ(skb->data, l2))
		typ = REJ;

	if (test_bit(FLG_MOD128, &l2->flag)) {
		PollFlag = (skb->data[1] & 0x1) == 0x1;
		nr = skb->data[1] >> 1;
	} else {
		PollFlag = (skb->data[0] & 0x10);
		nr = (skb->data[0] >> 5) & 0x7;
	}
	dev_kfree_skb(skb);

	if (PollFlag) {
		if (rsp)
			l2mgr(l2, MDL_ERROR | INDICATION, (void *) 'A');
		else
			enquiry_response(l2);
	}
	if (legalnr(l2, nr)) {
		if (typ == REJ) {
			setva(l2, nr);
			invoke_retransmission(l2, nr);
			stop_t200(l2, 10);
			if (FsmAddTimer(&l2->t203, l2->T203,
					EV_L2_T203, NULL, 6))
				l2m_debug(&l2->l2m, "Restart T203 ST7 REJ");
		} else if ((nr == l2->vs) && (typ == RR)) {
			setva(l2, nr);
			stop_t200(l2, 11);
			FsmRestartTimer(&l2->t203, l2->T203,
					EV_L2_T203, NULL, 7);
		} else if ((l2->va != nr) || (typ == RNR)) {
			setva(l2, nr);
			if (typ != RR)
				FsmDelTimer(&l2->t203, 9);
			restart_t200(l2, 12);
		}
		if (skb_queue_len(&l2->i_queue) && (typ == RR))
			FsmEvent(fi, EV_L2_ACK_PULL, NULL);
	} else
		nrerrorrecovery(fi);
}

static void
l2_feed_i_if_reest(struct FsmInst *fi, int event, void *arg)
{
	layer2_t *l2 = fi->userdata;
	struct sk_buff *skb = arg;

//	if (test_bit(FLG_LAPB, &l2->flag))
//		st->l1.bcs->tx_cnt += skb->len + l2headersize(l2, 0);
	if (!test_bit(FLG_L3_INIT, &l2->flag))
		skb_queue_tail(&l2->i_queue, skb);
	else
		dev_kfree_skb(skb);
}

static void
l2_feed_i_pull(struct FsmInst *fi, int event, void *arg)
{
	layer2_t *l2 = fi->userdata;
	struct sk_buff *skb = arg;

//	if (test_bit(FLG_LAPB, &l2->flag))
//		st->l1.bcs->tx_cnt += skb->len + l2headersize(l2, 0);
	skb_queue_tail(&l2->i_queue, skb);
	FsmEvent(fi, EV_L2_ACK_PULL, NULL);
}

static void
l2_feed_iqueue(struct FsmInst *fi, int event, void *arg)
{
	layer2_t *l2 = fi->userdata;
	struct sk_buff *skb = arg;

//	if (test_bit(FLG_LAPB, &l2->flag))
//		st->l1.bcs->tx_cnt += skb->len + l2headersize(l2, 0);
	skb_queue_tail(&l2->i_queue, skb);
}

static void
l2_got_iframe(struct FsmInst *fi, int event, void *arg)
{
	layer2_t *l2 = fi->userdata;
	struct sk_buff *skb = arg;
	int PollFlag, ns, i;
	unsigned int nr;

	i = l2addrsize(l2);
	if (test_bit(FLG_MOD128, &l2->flag)) {
		PollFlag = ((skb->data[i + 1] & 0x1) == 0x1);
		ns = skb->data[i] >> 1;
		nr = (skb->data[i + 1] >> 1) & 0x7f;
	} else {
		PollFlag = (skb->data[i] & 0x10);
		ns = (skb->data[i] >> 1) & 0x7;
		nr = (skb->data[i] >> 5) & 0x7;
	}
	if (test_bit(FLG_OWN_BUSY, &l2->flag)) {
		dev_kfree_skb(skb);
		if (PollFlag)
			enquiry_response(l2);
	} else if (l2->vr == ns) {
		(l2->vr)++;
		if(test_bit(FLG_MOD128, &l2->flag))
			l2->vr %= 128;
		else
			l2->vr %= 8;
		test_and_clear_bit(FLG_REJEXC, &l2->flag);
		if (PollFlag)
			enquiry_response(l2);
		else
			test_and_set_bit(FLG_ACK_PEND, &l2->flag);
		skb_pull(skb, l2headersize(l2, 0));
		l2up(l2, DL_DATA | INDICATION, l2->msgnr++, DTYPE_SKB, skb);
	} else {
		/* n(s)!=v(r) */
		dev_kfree_skb(skb);
		if (test_and_set_bit(FLG_REJEXC, &l2->flag)) {
			if (PollFlag)
				enquiry_response(l2);
		} else {
			enquiry_cr(l2, REJ, RSP, PollFlag);
			test_and_clear_bit(FLG_ACK_PEND, &l2->flag);
		}
	}
	if (legalnr(l2, nr)) {
		if (!test_bit(FLG_PEER_BUSY, &l2->flag) && (fi->state == ST_L2_7)) {
			if (nr == l2->vs) {
				stop_t200(l2, 13);
				FsmRestartTimer(&l2->t203, l2->T203,
						EV_L2_T203, NULL, 7);
			} else if (nr != l2->va)
				restart_t200(l2, 14);
		}
		setva(l2, nr);
	} else {
		nrerrorrecovery(fi);
		return;
	}
	if (skb_queue_len(&l2->i_queue) && (fi->state == ST_L2_7))
		FsmEvent(fi, EV_L2_ACK_PULL, NULL);
	if (test_and_clear_bit(FLG_ACK_PEND, &l2->flag))
		enquiry_cr(l2, RR, RSP, 0);
}

static void
l2_got_tei(struct FsmInst *fi, int event, void *arg)
{
	layer2_t *l2 = fi->userdata;

	l2->tei = *((int *)arg);

	if (fi->state == ST_L2_3) {
		establishlink(fi);
		test_and_set_bit(FLG_L3_INIT, &l2->flag);
	} else
		FsmChangeState(fi, ST_L2_4);
	if (skb_queue_len(&l2->ui_queue))
		tx_ui(l2);
}

static void
l2_st5_tout_200(struct FsmInst *fi, int event, void *arg)
{
	layer2_t *l2 = fi->userdata;

	if (test_bit(FLG_LAPD, &l2->flag) &&
		test_bit(FLG_DCHAN_BUSY, &l2->flag)) {
		FsmAddTimer(&l2->t200, l2->T200, EV_L2_T200, NULL, 9);
	} else if (l2->rc == l2->N200) {
		FsmChangeState(fi, ST_L2_4);
		test_and_clear_bit(FLG_T200_RUN, &l2->flag);
		discard_queue(&l2->i_queue);
		l2mgr(l2, MDL_ERROR | INDICATION, (void *) 'G');
		if (test_bit(FLG_LAPB, &l2->flag))
			l2down(l2, PH_DEACTIVATE | REQUEST, l2->msgnr++, 0, NULL);
		st5_dl_release_l2l3(l2);
	} else {
		l2->rc++;
		FsmAddTimer(&l2->t200, l2->T200, EV_L2_T200, NULL, 9);
		send_uframe(l2, (test_bit(FLG_MOD128, &l2->flag) ? SABME : SABM)
			    | 0x10, CMD);
	}
}

static void
l2_st6_tout_200(struct FsmInst *fi, int event, void *arg)
{
	layer2_t *l2 = fi->userdata;

	if (test_bit(FLG_LAPD, &l2->flag) &&
		test_bit(FLG_DCHAN_BUSY, &l2->flag)) {
		FsmAddTimer(&l2->t200, l2->T200, EV_L2_T200, NULL, 9);
	} else if (l2->rc == l2->N200) {
		FsmChangeState(fi, ST_L2_4);
		test_and_clear_bit(FLG_T200_RUN, &l2->flag);
		l2mgr(l2, MDL_ERROR | INDICATION, (void *) 'H');
		lapb_dl_release_l2l3(l2, CONFIRM, l2->last_nr);
	} else {
		l2->rc++;
		FsmAddTimer(&l2->t200, l2->T200, EV_L2_T200,
			    NULL, 9);
		send_uframe(l2, DISC | 0x10, CMD);
	}
}

static void
l2_st7_tout_200(struct FsmInst *fi, int event, void *arg)
{
	layer2_t *l2 = fi->userdata;

	if (test_bit(FLG_LAPD, &l2->flag) &&
		test_bit(FLG_DCHAN_BUSY, &l2->flag)) {
		FsmAddTimer(&l2->t200, l2->T200, EV_L2_T200, NULL, 9);
		return;
	}
	test_and_clear_bit(FLG_T200_RUN, &l2->flag);
	l2->rc = 0;
	FsmChangeState(fi, ST_L2_8);
	transmit_enquiry(l2);
	l2->rc++;
}

static void
l2_st8_tout_200(struct FsmInst *fi, int event, void *arg)
{
	layer2_t *l2 = fi->userdata;

	if (test_bit(FLG_LAPD, &l2->flag) &&
		test_bit(FLG_DCHAN_BUSY, &l2->flag)) {
		FsmAddTimer(&l2->t200, l2->T200, EV_L2_T200, NULL, 9);
		return;
	}
	test_and_clear_bit(FLG_T200_RUN, &l2->flag);
	if (l2->rc == l2->N200) {
		l2mgr(l2, MDL_ERROR | INDICATION, (void *) 'I');
		establishlink(fi);
		test_and_clear_bit(FLG_L3_INIT, &l2->flag);
	} else {
		transmit_enquiry(l2);
		l2->rc++;
	}
}

static void
l2_st7_tout_203(struct FsmInst *fi, int event, void *arg)
{
	layer2_t *l2 = fi->userdata;

	if (test_bit(FLG_LAPD, &l2->flag) &&
		test_bit(FLG_DCHAN_BUSY, &l2->flag)) {
		FsmAddTimer(&l2->t203, l2->T203, EV_L2_T203, NULL, 9);
		return;
	}
	FsmChangeState(fi, ST_L2_8);
	transmit_enquiry(l2);
	l2->rc = 0;
}

static void
l2_pull_iqueue(struct FsmInst *fi, int event, void *arg)
{
	layer2_t *l2 = fi->userdata;
	struct sk_buff *skb, *oskb;
	u_char header[MAX_HEADER_LEN];
	int i;
	int unsigned p1;
	long flags;

	if (!cansend(l2))
		return;

	skb = skb_dequeue(&l2->i_queue);
	if (!skb)
		return;

	save_flags(flags);
	cli();
	if(test_bit(FLG_MOD128, &l2->flag))
		p1 = (l2->vs - l2->va) % 128;
	else
		p1 = (l2->vs - l2->va) % 8;
	p1 = (p1 + l2->sow) % l2->window;
	if (l2->windowar[p1]) {
		printk(KERN_WARNING "isdnl2 try overwrite ack queue entry %d\n",
		       p1);
		dev_kfree_skb(l2->windowar[p1]);
	}
	l2->windowar[p1] = skb_clone(skb, GFP_ATOMIC);

	i = sethdraddr(l2, header, CMD);

	if (test_bit(FLG_MOD128, &l2->flag)) {
		header[i++] = l2->vs << 1;
		header[i++] = l2->vr << 1;
		l2->vs = (l2->vs + 1) % 128;
	} else {
		header[i++] = (l2->vr << 5) | (l2->vs << 1);
		l2->vs = (l2->vs + 1) % 8;
	}
	restore_flags(flags);

	p1 = skb->data - skb->head;
	if (p1 >= i)
		memcpy(skb_push(skb, i), header, i);
	else {
		printk(KERN_WARNING
		"isdnl2 pull_iqueue skb header(%d/%d) too short\n", i, p1);
		oskb = skb;
		skb = alloc_skb(oskb->len + i, GFP_ATOMIC);
		memcpy(skb_put(skb, i), header, i);
		memcpy(skb_put(skb, oskb->len), oskb->data, oskb->len);
		dev_kfree_skb(oskb);
	}
	l2down(l2, PH_DATA_REQ, l2->msgnr++, DTYPE_SKB, skb);
	test_and_clear_bit(FLG_ACK_PEND, &l2->flag);
	if (!test_and_set_bit(FLG_T200_RUN, &l2->flag)) {
		FsmDelTimer(&l2->t203, 13);
		FsmAddTimer(&l2->t200, l2->T200, EV_L2_T200, NULL, 11);
	}
}

static void
l2_st8_got_super(struct FsmInst *fi, int event, void *arg)
{
	layer2_t *l2 = fi->userdata;
	struct sk_buff *skb = arg;
	int PollFlag, rsp, rnr = 0;
	unsigned int nr;

	rsp = *skb->data & 0x2;
	if (test_bit(FLG_ORIG, &l2->flag))
		rsp = !rsp;

	skb_pull(skb, l2addrsize(l2));

	if (IsRNR(skb->data, l2)) {
		set_peer_busy(l2);
		rnr = 1;
	} else
		clear_peer_busy(l2);

	if (test_bit(FLG_MOD128, &l2->flag)) {
		PollFlag = (skb->data[1] & 0x1) == 0x1;
		nr = skb->data[1] >> 1;
	} else {
		PollFlag = (skb->data[0] & 0x10);
		nr = (skb->data[0] >> 5) & 0x7;
	}
	dev_kfree_skb(skb);
	if (rsp && PollFlag) {
		if (legalnr(l2, nr)) {
			if (rnr) {
				restart_t200(l2, 15);
			} else {
				stop_t200(l2, 16);
				FsmAddTimer(&l2->t203, l2->T203,
					    EV_L2_T203, NULL, 5);
				setva(l2, nr);
			}
			invoke_retransmission(l2, nr);
			FsmChangeState(fi, ST_L2_7);
			if (skb_queue_len(&l2->i_queue) && cansend(l2))
				FsmEvent(fi, EV_L2_ACK_PULL, NULL);
		} else
			nrerrorrecovery(fi);
	} else {
		if (!rsp && PollFlag)
			enquiry_response(l2);
		if (legalnr(l2, nr)) {
			setva(l2, nr);
		} else
			nrerrorrecovery(fi);
	}
}

static void
l2_got_FRMR(struct FsmInst *fi, int event, void *arg)
{
	layer2_t *l2 = fi->userdata;
	struct sk_buff *skb = arg;

	skb_pull(skb, l2addrsize(l2) + 1);

	if (!(skb->data[0] & 1) || ((skb->data[0] & 3) == 1) ||		/* I or S */
	    (IsUA(skb->data) && (fi->state == ST_L2_7))) {
		l2mgr(l2, MDL_ERROR | INDICATION, (void *) 'K');
		establishlink(fi);
		test_and_clear_bit(FLG_L3_INIT, &l2->flag);
	}
	dev_kfree_skb(skb);
}

static void
l2_st24_tei_remove(struct FsmInst *fi, int event, void *arg)
{
	layer2_t *l2 = fi->userdata;

	discard_queue(&l2->ui_queue);
	l2->tei = -1;
	FsmChangeState(fi, ST_L2_1);
}

static void
l2_st3_tei_remove(struct FsmInst *fi, int event, void *arg)
{
	layer2_t *l2 = fi->userdata;

	discard_queue(&l2->ui_queue);
	l2->tei = -1;
	l2up(l2, DL_RELEASE | INDICATION, l2->msgnr++, 0, NULL);
	FsmChangeState(fi, ST_L2_1);
}

static void
l2_st5_tei_remove(struct FsmInst *fi, int event, void *arg)
{
	layer2_t *l2 = fi->userdata;

	discard_queue(&l2->i_queue);
	discard_queue(&l2->ui_queue);
	freewin(l2);
	l2->tei = -1;
	stop_t200(l2, 17);
	st5_dl_release_l2l3(l2);
	FsmChangeState(fi, ST_L2_1);
}

static void
l2_st6_tei_remove(struct FsmInst *fi, int event, void *arg)
{
	layer2_t *l2 = fi->userdata;

	discard_queue(&l2->ui_queue);
	l2->tei = -1;
	stop_t200(l2, 18);
	l2up(l2, DL_RELEASE | CONFIRM, l2->last_nr, 0, NULL);
	FsmChangeState(fi, ST_L2_1);
}

static void
l2_tei_remove(struct FsmInst *fi, int event, void *arg)
{
	layer2_t *l2 = fi->userdata;

	discard_queue(&l2->i_queue);
	discard_queue(&l2->ui_queue);
	freewin(l2);
	l2->tei = -1;
	stop_t200(l2, 17);
	FsmDelTimer(&l2->t203, 19);
	l2up(l2, DL_RELEASE | INDICATION, l2->msgnr++, 0, NULL);
	FsmChangeState(fi, ST_L2_1);
}

static void
l2_st14_persistant_da(struct FsmInst *fi, int event, void *arg)
{
	layer2_t *l2 = fi->userdata;
	
	discard_queue(&l2->i_queue);
	discard_queue(&l2->ui_queue);
	if (test_and_clear_bit(FLG_ESTAB_PEND, &l2->flag))
		l2up(l2, DL_RELEASE | INDICATION, l2->msgnr++, 0, NULL);
}

static void
l2_st5_persistant_da(struct FsmInst *fi, int event, void *arg)
{
	layer2_t *l2 = fi->userdata;

	discard_queue(&l2->i_queue);
	discard_queue(&l2->ui_queue);
	freewin(l2);
	stop_t200(l2, 19);
	st5_dl_release_l2l3(l2);
	FsmChangeState(fi, ST_L2_4);
}

static void
l2_st6_persistant_da(struct FsmInst *fi, int event, void *arg)
{
	layer2_t *l2 = fi->userdata;

	discard_queue(&l2->ui_queue);
	stop_t200(l2, 20);
	l2up(l2, DL_RELEASE | CONFIRM, l2->last_nr, 0, NULL);
	FsmChangeState(fi, ST_L2_4);
}

static void
l2_persistant_da(struct FsmInst *fi, int event, void *arg)
{
	layer2_t *l2 = fi->userdata;

	discard_queue(&l2->i_queue);
	discard_queue(&l2->ui_queue);
	freewin(l2);
	stop_t200(l2, 19);
	FsmDelTimer(&l2->t203, 19);
	l2up(l2, DL_RELEASE | INDICATION, l2->msgnr++, 0, NULL);
	FsmChangeState(fi, ST_L2_4);
}

static void
l2_set_own_busy(struct FsmInst *fi, int event, void *arg)
{
	layer2_t *l2 = fi->userdata;

	if(!test_and_set_bit(FLG_OWN_BUSY, &l2->flag)) {
		enquiry_cr(l2, RNR, RSP, 0);
		test_and_clear_bit(FLG_ACK_PEND, &l2->flag);
	}
}

static void
l2_clear_own_busy(struct FsmInst *fi, int event, void *arg)
{
	layer2_t *l2 = fi->userdata;

	if(!test_and_clear_bit(FLG_OWN_BUSY, &l2->flag)) {
		enquiry_cr(l2, RR, RSP, 0);
		test_and_clear_bit(FLG_ACK_PEND, &l2->flag);
	}
}

static void
l2_frame_error(struct FsmInst *fi, int event, void *arg)
{
	layer2_t *l2 = fi->userdata;

	l2mgr(l2, MDL_ERROR | INDICATION, arg);
}

static void
l2_frame_error_reest(struct FsmInst *fi, int event, void *arg)
{
	layer2_t *l2 = fi->userdata;

	l2mgr(l2, MDL_ERROR | INDICATION, arg);
	establishlink(fi);
	test_and_clear_bit(FLG_L3_INIT, &l2->flag);
}

static struct FsmNode L2FnList[] =
{
	{ST_L2_1, EV_L2_DL_ESTABLISH_REQ, l2_mdl_assign},
	{ST_L2_2, EV_L2_DL_ESTABLISH_REQ, l2_go_st3},
	{ST_L2_4, EV_L2_DL_ESTABLISH_REQ, l2_establish},
	{ST_L2_5, EV_L2_DL_ESTABLISH_REQ, l2_discard_i_setl3},
	{ST_L2_7, EV_L2_DL_ESTABLISH_REQ, l2_l3_reestablish},
	{ST_L2_8, EV_L2_DL_ESTABLISH_REQ, l2_l3_reestablish},
	{ST_L2_4, EV_L2_DL_RELEASE_REQ, l2_release},
	{ST_L2_5, EV_L2_DL_RELEASE_REQ, l2_pend_rel},
	{ST_L2_7, EV_L2_DL_RELEASE_REQ, l2_disconnect},
	{ST_L2_8, EV_L2_DL_RELEASE_REQ, l2_disconnect},
	{ST_L2_5, EV_L2_DL_DATA, l2_feed_i_if_reest},
	{ST_L2_7, EV_L2_DL_DATA, l2_feed_i_pull},
	{ST_L2_8, EV_L2_DL_DATA, l2_feed_iqueue},
	{ST_L2_1, EV_L2_DL_UNITDATA, l2_queue_ui_assign},
	{ST_L2_2, EV_L2_DL_UNITDATA, l2_queue_ui},
	{ST_L2_3, EV_L2_DL_UNITDATA, l2_queue_ui},
	{ST_L2_4, EV_L2_DL_UNITDATA, l2_send_ui},
	{ST_L2_5, EV_L2_DL_UNITDATA, l2_send_ui},
	{ST_L2_6, EV_L2_DL_UNITDATA, l2_send_ui},
	{ST_L2_7, EV_L2_DL_UNITDATA, l2_send_ui},
	{ST_L2_8, EV_L2_DL_UNITDATA, l2_send_ui},
	{ST_L2_1, EV_L2_MDL_ASSIGN, l2_got_tei},
	{ST_L2_2, EV_L2_MDL_ASSIGN, l2_got_tei},
	{ST_L2_3, EV_L2_MDL_ASSIGN, l2_got_tei},
	{ST_L2_2, EV_L2_MDL_ERROR, l2_st24_tei_remove},
	{ST_L2_3, EV_L2_MDL_ERROR, l2_st3_tei_remove},
	{ST_L2_4, EV_L2_MDL_REMOVE, l2_st24_tei_remove},
	{ST_L2_5, EV_L2_MDL_REMOVE, l2_st5_tei_remove},
	{ST_L2_6, EV_L2_MDL_REMOVE, l2_st6_tei_remove},
	{ST_L2_7, EV_L2_MDL_REMOVE, l2_tei_remove},
	{ST_L2_8, EV_L2_MDL_REMOVE, l2_tei_remove},
	{ST_L2_4, EV_L2_SABME, l2_start_multi},
	{ST_L2_5, EV_L2_SABME, l2_send_UA},
	{ST_L2_6, EV_L2_SABME, l2_send_DM},
	{ST_L2_7, EV_L2_SABME, l2_restart_multi},
	{ST_L2_8, EV_L2_SABME, l2_restart_multi},
	{ST_L2_4, EV_L2_DISC, l2_send_DM},
	{ST_L2_5, EV_L2_DISC, l2_send_DM},
	{ST_L2_6, EV_L2_DISC, l2_send_UA},
	{ST_L2_7, EV_L2_DISC, l2_stop_multi},
	{ST_L2_8, EV_L2_DISC, l2_stop_multi},
	{ST_L2_4, EV_L2_UA, l2_mdl_error_ua},
	{ST_L2_5, EV_L2_UA, l2_connected},
	{ST_L2_6, EV_L2_UA, l2_released},
	{ST_L2_7, EV_L2_UA, l2_mdl_error_ua},
	{ST_L2_8, EV_L2_UA, l2_mdl_error_ua},
	{ST_L2_4, EV_L2_DM, l2_reestablish},
	{ST_L2_5, EV_L2_DM, l2_st5_dm_release},
	{ST_L2_6, EV_L2_DM, l2_st6_dm_release},
	{ST_L2_7, EV_L2_DM, l2_mdl_error_dm},
	{ST_L2_8, EV_L2_DM, l2_st8_mdl_error_dm},
	{ST_L2_1, EV_L2_UI, l2_got_ui},
	{ST_L2_2, EV_L2_UI, l2_got_ui},
	{ST_L2_3, EV_L2_UI, l2_got_ui},
	{ST_L2_4, EV_L2_UI, l2_got_ui},
	{ST_L2_5, EV_L2_UI, l2_got_ui},
	{ST_L2_6, EV_L2_UI, l2_got_ui},
	{ST_L2_7, EV_L2_UI, l2_got_ui},
	{ST_L2_8, EV_L2_UI, l2_got_ui},
	{ST_L2_7, EV_L2_FRMR, l2_got_FRMR},
	{ST_L2_8, EV_L2_FRMR, l2_got_FRMR},
	{ST_L2_7, EV_L2_SUPER, l2_st7_got_super},
	{ST_L2_8, EV_L2_SUPER, l2_st8_got_super},
	{ST_L2_7, EV_L2_I, l2_got_iframe},
	{ST_L2_8, EV_L2_I, l2_got_iframe},
	{ST_L2_5, EV_L2_T200, l2_st5_tout_200},
	{ST_L2_6, EV_L2_T200, l2_st6_tout_200},
	{ST_L2_7, EV_L2_T200, l2_st7_tout_200},
	{ST_L2_8, EV_L2_T200, l2_st8_tout_200},
	{ST_L2_7, EV_L2_T203, l2_st7_tout_203},
	{ST_L2_7, EV_L2_ACK_PULL, l2_pull_iqueue},
	{ST_L2_7, EV_L2_SET_OWN_BUSY, l2_set_own_busy},
	{ST_L2_8, EV_L2_SET_OWN_BUSY, l2_set_own_busy},
	{ST_L2_7, EV_L2_CLEAR_OWN_BUSY, l2_clear_own_busy},
	{ST_L2_8, EV_L2_CLEAR_OWN_BUSY, l2_clear_own_busy},
	{ST_L2_4, EV_L2_FRAME_ERROR, l2_frame_error},
	{ST_L2_5, EV_L2_FRAME_ERROR, l2_frame_error},
	{ST_L2_6, EV_L2_FRAME_ERROR, l2_frame_error},
	{ST_L2_7, EV_L2_FRAME_ERROR, l2_frame_error_reest},
	{ST_L2_8, EV_L2_FRAME_ERROR, l2_frame_error_reest},
	{ST_L2_1, EV_L1_DEACTIVATE, l2_st14_persistant_da},
	{ST_L2_2, EV_L1_DEACTIVATE, l2_st24_tei_remove},
	{ST_L2_3, EV_L1_DEACTIVATE, l2_st3_tei_remove},
	{ST_L2_4, EV_L1_DEACTIVATE, l2_st14_persistant_da},
	{ST_L2_5, EV_L1_DEACTIVATE, l2_st5_persistant_da},
	{ST_L2_6, EV_L1_DEACTIVATE, l2_st6_persistant_da},
	{ST_L2_7, EV_L1_DEACTIVATE, l2_persistant_da},
	{ST_L2_8, EV_L1_DEACTIVATE, l2_persistant_da},
};

#define L2_FN_COUNT (sizeof(L2FnList)/sizeof(struct FsmNode))

static int
ph_data_indication(hisaxif_t *hif, u_int nr, int dtyp, void *arg) {
	layer2_t *l2 = hif->fdata;
	struct sk_buff *skb = arg;
	u_char *datap;
	int ret = -EINVAL;
	int err = -EINVAL;
	int psapi, ptei;
	int len;
	int c = 0;

	if (!skb) {
		printk(KERN_WARNING "l2 ph_data_indication no data frame\n");
		if (hif->next)
			ret = hif->next->func(hif->next, PH_DATA_IND, nr, dtyp,
				arg);
		return(ret);
	}
	datap = skb->data;
	len = l2addrsize(l2);
	if (skb->len <= len) {
		FsmEvent(&l2->l2m, EV_L2_FRAME_ERROR, (void *) 'N');
		if (hif->next)
			ret = hif->next->func(hif->next, PH_DATA_IND, nr, dtyp,
				arg);
		return(ret);
	}
	if (test_bit(FLG_LAPD, &l2->flag)) {
		psapi = *datap++;
		ptei = *datap++;
		if ((psapi & 1) || !(ptei & 1)) {
			printk(KERN_WARNING "l2 D-channel frame wrong EA0/EA1\n");
			return(ret);
		}
		psapi >>= 2;
		ptei >>= 1;
		if ((psapi != l2->sapi) && (psapi != TEI_SAPI)) {
			if (hif->next)
				ret = hif->next->func(hif->next, PH_DATA_IND,
					nr, dtyp, arg);
			return(ret);
		}
		if (ptei == GROUP_TEI) {
			if (hif->next) {
				if ((skb = skb_clone(arg, GFP_ATOMIC))) {
					err = hif->next->func(hif->next,
						PH_DATA_IND, nr, dtyp, skb);
					if (err)
						dev_kfree_skb(skb);
				}
				skb = arg;
			}
			if (psapi == TEI_SAPI)
				return(l2_tei(l2->tm,
					MDL_UNITDATA | INDICATION, nr, dtyp, skb));
		} else if ((ptei != l2->tei) || (psapi == TEI_SAPI)) {
			if (hif->next)
				ret = hif->next->func(hif->next,  PH_DATA_IND,
					nr, dtyp, arg);
			return(ret);
		}
	} else
		datap += len;
	if (!(*datap & 1)) {	/* I-Frame */
		if(!(c = iframe_error(l2, skb)))
			ret = FsmEvent(&l2->l2m, EV_L2_I, skb);
	} else if (IsSFrame(datap, l2)) {	/* S-Frame */
		if(!(c = super_error(l2, skb)))
			ret = FsmEvent(&l2->l2m, EV_L2_SUPER, skb);
	} else if (IsUI(datap)) {
		if(!(c = UI_error(l2, skb)))
			ret = FsmEvent(&l2->l2m, EV_L2_UI, skb);
	} else if (IsSABME(datap, l2)) {
		if(!(c = unnum_error(l2, skb, CMD)))
			ret = FsmEvent(&l2->l2m, EV_L2_SABME, skb);
	} else if (IsUA(datap)) {
		if(!(c = unnum_error(l2, skb, RSP)))
			ret = FsmEvent(&l2->l2m, EV_L2_UA, skb);
	} else if (IsDISC(datap)) {
		if(!(c = unnum_error(l2, skb, CMD)))
			ret = FsmEvent(&l2->l2m, EV_L2_DISC, skb);
	} else if (IsDM(datap)) {
		if(!(c = unnum_error(l2, skb, RSP)))
			ret = FsmEvent(&l2->l2m, EV_L2_DM, skb);
	} else if (IsFRMR(datap)) {
		if(!(c = FRMR_error(l2, skb)))
			ret = FsmEvent(&l2->l2m, EV_L2_FRMR, skb);
	} else {
		c = 'L';
	}
	if (c) {
		printk(KERN_WARNING "l2 D-channel frame error %c\n",c);
		FsmEvent(&l2->l2m, EV_L2_FRAME_ERROR, (void *)(long)c);
		if (err)
			return(err);
	}
	if (ret) {
		dev_kfree_skb(skb);
		ret = 0;
	}
	return(ret);
}

static int
l2from_down(hisaxif_t *hif, u_int prim, u_int nr, int dtyp, void *arg) {
	layer2_t *l2;
	int ret = -EINVAL;
	int *iarg;

	if (!hif)
		return(-EINVAL);
	l2 = hif->fdata;
	if (!l2) {
		if (hif->next)
			ret = hif->next->func(hif->next, prim, nr, dtyp, arg);
		return(ret);
	}
	switch (prim) {
		case (PH_DATA_IND):
			return(ph_data_indication(hif, nr, dtyp, arg));
		case (PH_DATA | CONFIRM):
			return(ph_data_confirm(hif, nr, dtyp, arg));
		case (PH_CONTROL | INDICATION):
			iarg = arg;
			if (dtyp == 4) {
				if (*iarg == HW_D_BLOCKED)
					test_and_set_bit(FLG_DCHAN_BUSY,
						&l2->flag);
				else if (*iarg == HW_D_NOBLOCKED)
					test_and_clear_bit(FLG_DCHAN_BUSY,
						&l2->flag);
				else
					break;
				ret = 0;
			}
			break;
		case (PH_ACTIVATE | CONFIRM):
		case (PH_ACTIVATE | INDICATION):
			test_and_set_bit(FLG_L1_ACTIV, &l2->flag);
			if (test_and_clear_bit(FLG_ESTAB_PEND, &l2->flag))
				FsmEvent(&l2->l2m, EV_L2_DL_ESTABLISH_REQ, arg);
			ret = 0; 
			break;
		case (PH_DEACTIVATE | INDICATION):
		case (PH_DEACTIVATE | CONFIRM):
			test_and_clear_bit(FLG_L1_ACTIV, &l2->flag);
			FsmEvent(&l2->l2m, EV_L1_DEACTIVATE, arg);
			ret = 0;
			break;
		case (MDL_STATUS | REQUEST):
			if (test_bit(FLG_LAPD, &l2->flag)) {
				if (l2->tei == dtyp) {
					teimgr_t **p = arg;
					if (p) {
						*p = l2->tm;
						return(0);
					}
				}
			}
			break;
		default:
			l2m_debug(&l2->l2m, "l2 unknown pr %04x", prim);
			ret = -EINVAL;
			break;
	}
	if (hif->next)
		ret = hif->next->func(hif->next, prim, nr, dtyp, arg);
	return(ret);
}

static int
l2from_up(hisaxif_t *hif, u_int prim, u_int nr, int dtyp, void *arg) {
	layer2_t *l2;

	if (!hif || !hif->fdata)
		return(-EINVAL);
	l2 = hif->fdata;
	switch (prim) {
		case (DL_DATA | REQUEST):
			if (FsmEvent(&l2->l2m, EV_L2_DL_DATA, arg)) {
				dev_kfree_skb((struct sk_buff *) arg);
			}
			break;
		case (DL_UNITDATA | REQUEST):
			if (FsmEvent(&l2->l2m, EV_L2_DL_UNITDATA, arg)) {
				dev_kfree_skb((struct sk_buff *) arg);
			}
			break;
		case (DL_ESTABLISH | REQUEST):
			l2->last_nr = nr;
			if (test_bit(FLG_L1_ACTIV, &l2->flag)) {
				if (test_bit(FLG_LAPD, &l2->flag) ||
					test_bit(FLG_ORIG, &l2->flag)) {
					FsmEvent(&l2->l2m, EV_L2_DL_ESTABLISH_REQ, arg);
				}
			} else {
				if (test_bit(FLG_LAPD, &l2->flag) ||
					test_bit(FLG_ORIG, &l2->flag)) {
					test_and_set_bit(FLG_ESTAB_PEND, &l2->flag);
				}
				l2down(l2, PH_ACTIVATE | REQUEST, l2->msgnr++, 0, NULL);
			}
			break;
		case (DL_RELEASE | REQUEST):
			l2->last_nr = nr;
			if (test_bit(FLG_LAPB, &l2->flag)) {
				l2down(l2, PH_DEACTIVATE | REQUEST, l2->msgnr++, 0, NULL);
			}
			FsmEvent(&l2->l2m, EV_L2_DL_RELEASE_REQ, arg);
			break;
		case (MDL_ASSIGN | REQUEST):
			FsmEvent(&l2->l2m, EV_L2_MDL_ASSIGN, arg);
			break;
		case (MDL_REMOVE | REQUEST):
			FsmEvent(&l2->l2m, EV_L2_MDL_REMOVE, arg);
			break;
		case (MDL_ERROR | RESPONSE):
			FsmEvent(&l2->l2m, EV_L2_MDL_ERROR, arg);
		case (MDL_STATUS | REQUEST):
			l2up(l2, MDL_STATUS | CONFIRM, nr, 0, (void *)l2->tei);
			break;
	}
	return(0);
}

int
tei_l2(layer2_t *l2, u_int prim, u_int nr, int dtyp, void *arg)
{
	if (!l2)
		return(-EINVAL);
	switch(prim) {
	    case (MDL_UNITDATA | REQUEST):
		return(l2down(l2, PH_DATA_REQ, nr, dtyp, arg));
	    case (MDL_ASSIGN | REQUEST):
		FsmEvent(&l2->l2m, EV_L2_MDL_ASSIGN, arg);
		break;
	    case (MDL_REMOVE | REQUEST):
		FsmEvent(&l2->l2m, EV_L2_MDL_REMOVE, arg);
		break;
	    case (MDL_ERROR | RESPONSE):
		FsmEvent(&l2->l2m, EV_L2_MDL_ERROR, arg);
		break;
	    case (MDL_STATUS | REQUEST):
		if (l2->inst.st && l2->inst.st->inst[1]) {
			hisaxif_t *up = &l2->inst.st->inst[1]->up;
			if (up)
				return(up->func(up, MDL_STATUS | REQUEST,
					nr, dtyp, arg));
		}
		break;
	}
	return(0);
}

static void
l2m_debug(struct FsmInst *fi, char *fmt, ...)
{
	layer2_t *l2 = fi->userdata;
	logdata_t log;

	va_start(log.args, fmt);
	log.fmt = fmt;
	log.head = l2->inst.id;
	l2->inst.obj->ctrl(&l2->inst, MGR_DEBUGDATA | REQUEST, &log);
	va_end(log.args);
}

static void
release_l2(layer2_t *l2)
{
	hisaxinstance_t  *inst = &l2->inst;
	hisaxif_t	hif;

	FsmDelTimer(&l2->t200, 21);
	FsmDelTimer(&l2->t203, 16);
	discard_queue(&l2->i_queue);
	discard_queue(&l2->ui_queue);
	discard_queue(&l2->ph_queue);
	if (l2->ph_skb)
		dev_kfree_skb(l2->ph_skb);
	ReleaseWin(l2);
	release_tei(l2->tm);
	memset(&hif, 0, sizeof(hisaxif_t));
	hif.fdata = l2;
	hif.func = l2from_up;
	hif.protocol = inst->up.protocol;
	hif.layer = inst->up.layer;
	isdnl2.ctrl(inst->st, MGR_DELIF | REQUEST, &hif);
	hif.fdata = l2;
	hif.func = l2from_down;
	hif.protocol = inst->down.protocol;
	hif.layer = inst->down.layer;
	isdnl2.ctrl(inst->st, MGR_DELIF | REQUEST, &hif);
	REMOVE_FROM_LISTBASE(l2, l2list);
	REMOVE_FROM_LIST(inst);
	if (inst->st)
		if (inst->st->inst[inst->layer] == inst)
			inst->st->inst[inst->layer] = inst->next;
	kfree(l2);
	isdnl2.refcnt--;
}

static layer2_t *
create_l2(hisaxstack_t *st, hisaxif_t *hif) {
	layer2_t *nl2;
	int lay, err;

	if (!hif)
		return(NULL);
	printk(KERN_DEBUG "create_l2 prot %x\n", hif->protocol);
	if (!st) {
		printk(KERN_ERR "create_l2 no stack\n");
		return(NULL);
	}
	if (!(nl2 = kmalloc(sizeof(layer2_t), GFP_ATOMIC))) {
		printk(KERN_ERR "kmalloc layer2 failed\n");
		return(NULL);
	}
	memset(nl2, 0, sizeof(layer2_t));
	nl2->debug = debug;
	nl2->inst.protocol = hif->protocol;
	switch(nl2->inst.protocol) {
	    case ISDN_PID_L2_LAPD:
	    	sprintf(nl2->inst.id, "lapd %d", st->id);
		test_and_set_bit(FLG_LAPD, &nl2->flag);
		test_and_set_bit(FLG_MOD128, &nl2->flag);
		test_and_set_bit(FLG_LAPD, &nl2->flag);
		test_and_set_bit(FLG_ORIG, &nl2->flag);
		nl2->sapi = 0;
		nl2->tei = -1;
		nl2->maxlen = MAX_DFRAME_LEN;
		nl2->window = 1;
		nl2->T200 = 1000;
		nl2->N200 = 3;
		nl2->T203 = 10000;
		if (create_teimgr(nl2)) {
			kfree(nl2);
			return(NULL);
		}
		break;
	    case ISDN_PID_L2_LAPB:
		test_and_set_bit(FLG_LAPB, &nl2->flag);
		sprintf(nl2->inst.id, "lapb %d", st->id);
		nl2->window = 7;
		nl2->maxlen = MAX_DATA_SIZE;
		nl2->T200 = 1000;
		nl2->N200 = 4;
		nl2->T203 = 5000;
		break;
	    default:
		printk(KERN_ERR "layer1 create failed prt %x\n",nl2->inst.protocol);
		kfree(nl2);
		return(NULL);
	}
	nl2->inst.obj = &isdnl2;
	nl2->msgnr = 1;
	skb_queue_head_init(&nl2->i_queue);
	skb_queue_head_init(&nl2->ui_queue);
	skb_queue_head_init(&nl2->ph_queue);
	InitWin(nl2);
	nl2->l2m.fsm = &l2fsm;
	if (test_bit(FLG_LAPB, &nl2->flag))
		nl2->l2m.state = ST_L2_4;
	else
		nl2->l2m.state = ST_L2_1;
	nl2->l2m.debug = debug;
	nl2->l2m.userdata = nl2;
	nl2->l2m.userint = 0;
	nl2->l2m.printdebug = l2m_debug;

	FsmInitTimer(&nl2->l2m, &nl2->t200);
	FsmInitTimer(&nl2->l2m, &nl2->t203);
	nl2->inst.layer = hif->layer;
	nl2->inst.data = nl2;
	APPEND_TO_LIST(nl2, l2list);
	isdnl2.ctrl(st, MGR_ADDLAYER | INDICATION, &nl2->inst);
	lay = nl2->inst.layer + 1;
	if ((lay<0) || (lay>MAX_LAYER)) {
		lay = 0;
		nl2->inst.up.protocol = ISDN_PID_NONE;
	} else
		nl2->inst.up.protocol = st->protocols[lay];
	nl2->inst.up.layer = lay;
	nl2->inst.up.stat = IF_DOWN;
	lay = nl2->inst.layer - 1;
	if ((lay<0) || (lay>MAX_LAYER)) {
		lay = 0;
		nl2->inst.down.protocol = ISDN_PID_NONE;
	} else
		nl2->inst.down.protocol = st->protocols[lay];
	nl2->inst.down.layer = lay;
	nl2->inst.down.stat = IF_UP;
	err = isdnl2.ctrl(st, MGR_ADDIF | REQUEST, &nl2->inst.down);
	if (err) {
		release_l2(nl2);
		printk(KERN_ERR "layer2 down interface request failed %d\n", err);
		return(NULL);
	}
	err = isdnl2.ctrl(st, MGR_ADDIF | REQUEST, &nl2->inst.up);
	if (err) {
		release_l2(nl2);
		printk(KERN_ERR "layer2 up interface request failed %d\n", err);
		return(NULL);
	}
	return(nl2);
}

static int
add_if(layer2_t *l2, hisaxif_t *hif) {
	int err;
	hisaxinstance_t *inst = &l2->inst;

	printk(KERN_DEBUG "layer2 add_if lay %d/%d prot %x\n", hif->layer,
		hif->stat, hif->protocol);
	hif->fdata = l2;
	if (IF_TYPE(hif) == IF_UP) {
		hif->func = l2from_up;
		if (inst->up.stat == IF_NOACTIV) {
			inst->up.stat = IF_DOWN;
			inst->up.protocol =
				inst->st->protocols[inst->up.layer];
			err = isdnl2.ctrl(inst->st, MGR_ADDIF | REQUEST, &inst->up);
			if (err)
				inst->up.stat = IF_NOACTIV;
		}
	} else if (IF_TYPE(hif) == IF_DOWN) {
		hif->func = l2from_down;
		if (inst->down.stat == IF_NOACTIV) {
			inst->down.stat = IF_UP;
			inst->down.protocol =
				inst->st->protocols[inst->down.layer];
			err = isdnl2.ctrl(inst->st, MGR_ADDIF | REQUEST, &inst->down);
			if (err)
				inst->down.stat = IF_NOACTIV;
		}
	} else
		return(-EINVAL);
	return(0);
}

static int
del_if(layer2_t *l2, hisaxif_t *hif) {
	int err;
	hisaxinstance_t *inst = &l2->inst;

	printk(KERN_DEBUG "layer2 del_if lay %d/%d %p/%p\n", hif->layer,
		hif->stat, hif->func, hif->fdata);
	if ((hif->func == inst->up.func) && (hif->fdata == inst->up.fdata)) {
		inst->up.stat = IF_NOACTIV;
		inst->up.protocol = ISDN_PID_NONE;
		err = isdnl2.ctrl(inst->st, MGR_ADDIF | REQUEST, &inst->up);
	} else if ((hif->func == inst->down.func) && (hif->fdata == inst->down.fdata)) {
		inst->down.stat = IF_NOACTIV;
		inst->down.protocol = ISDN_PID_NONE;
		err = isdnl2.ctrl(inst->st, MGR_ADDIF | REQUEST, &inst->down);
	} else {
		printk(KERN_DEBUG "layer2 del_if no if found\n");
		return(-EINVAL);
	}
	return(0);
}

static char MName[] = "ISDNL2";

static int L2Protocols[] = {	ISDN_PID_L2_LAPD,
				ISDN_PID_L2_LAPB
			};
#define PROTOCOLCNT	(sizeof(L2Protocols)/sizeof(int))
 
#ifdef MODULE
MODULE_AUTHOR("Karsten Keil");
MODULE_PARM(debug, "1i");
#define Isdnl2Init init_module
#endif

static int
l2_manager(void *data, u_int prim, void *arg) {
	hisaxstack_t *st = data;
	layer2_t *l2l = l2list;

//	printk(KERN_DEBUG "l2_manager data:%p prim:%x arg:%p\n", data, prim, arg);
	if (!data)
		return(-EINVAL);
	while(l2l) {
		if (l2l->inst.st == st)
			break;
		l2l = l2l->next;
	}
	switch(prim) {
	    case MGR_ADDIF | REQUEST:
		if (!l2l)
			l2l = create_l2(st, arg);
		if (!l2l) {
			printk(KERN_WARNING "l2_manager create_l2 failed\n");
			return(-EINVAL);
		}
		return(add_if(l2l, arg));
		break;
	    case MGR_DELIF | REQUEST:
		if (!l2l) {
			printk(KERN_WARNING "l2_manager delif no instance\n");
			return(-EINVAL);
		}
		return(del_if(l2l, arg));
		break;
	    case MGR_RELEASE | INDICATION:
	    	if (l2l) {
			printk(KERN_DEBUG "release_l2 id %x\n", l2l->inst.st->id);
	    		release_l2(l2l);
	    	} else 
	    		printk(KERN_WARNING "l2_manager release no instance\n");
	    	break;
	    		
	    default:
		printk(KERN_WARNING "l2_manager prim %x not handled\n", prim);
		return(-EINVAL);
	}
	return(0);
}

int Isdnl2Init(void)
{
	int err;

	isdnl2.name = MName;
	isdnl2.protocols = L2Protocols;
	isdnl2.protcnt = PROTOCOLCNT;
	isdnl2.own_ctrl = l2_manager;
	isdnl2.prev = NULL;
	isdnl2.next = NULL;
	isdnl2.layer = 2;
	l2fsm.state_count = L2_STATE_COUNT;
	l2fsm.event_count = L2_EVENT_COUNT;
	l2fsm.strEvent = strL2Event;
	l2fsm.strState = strL2State;
	FsmNew(&l2fsm, L2FnList, L2_FN_COUNT);
	TEIInit();
	if ((err = HiSax_register(&isdnl2))) {
		printk(KERN_ERR "Can't register %s error(%d)\n", MName, err);
		FsmFree(&l2fsm);
	}
	return(err);
}

#ifdef MODULE
void cleanup_module(void)
{
	int err;

	if ((err = HiSax_unregister(&isdnl2))) {
		printk(KERN_ERR "Can't unregister ISDN layer 2 error(%d)\n", err);
	}
	if(l2list) {
		printk(KERN_WARNING "hisaxl2 l2list not empty\n");
		while(l2list)
			release_l2(l2list);
	}
	TEIFree();
	FsmFree(&l2fsm);
}
#endif
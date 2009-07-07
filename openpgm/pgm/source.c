/* vim:ts=8:sts=8:sw=4:noai:noexpandtab
 *
 * PGM source transport.
 *
 * Copyright (c) 2006-2009 Miru Limited.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <limits.h>
#include <netdb.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <net/if.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <netinet/udp.h>
#ifdef CONFIG_HAVE_POLL
#	include <poll.h>
#endif
#ifdef CONFIG_HAVE_EPOLL
#	include <sys/epoll.h>
#endif
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <arpa/inet.h>

#include <glib.h>

#include "pgm/transport.h"
#include "pgm/source.h"
#include "pgm/if.h"
#include "pgm/ip.h"
#include "pgm/packet.h"
#include "pgm/net.h"
#include "pgm/txwi.h"
#include "pgm/rxwi.h"
#include "pgm/rate_control.h"
#include "pgm/sn.h"
#include "pgm/time.h"
#include "pgm/timer.h"
#include "pgm/checksum.h"
#include "pgm/reed_solomon.h"
#include "pgm/err.h"

#define SOURCE_DEBUG
//#define SPM_DEBUG

#ifndef SOURCE_DEBUG
#	define g_trace(m,...)		while (0)
#else
#	include <ctype.h>
#	ifdef SPM_DEBUG
#		define g_trace(m,...)		g_debug(__VA_ARGS__)
#	else
#		define g_trace(m,...)		do { if (strcmp((m),"SPM")) { g_debug(__VA_ARGS__); } } while (0)
#	endif
#endif


/* locals */
static int send_spm (pgm_transport_t*);
static int pgm_reset_heartbeat_spm (pgm_transport_t*);
static gssize pgm_transport_send_onev (pgm_transport_t*, const struct pgm_iovec*, guint, int);
static int send_ncf (pgm_transport_t*, struct sockaddr*, struct sockaddr*, guint32, gboolean);
static int send_ncf_list (pgm_transport_t*, struct sockaddr*, struct sockaddr*, pgm_sqn_list_t*, gboolean);
static gssize pgm_transport_send_one (pgm_transport_t*, struct pgm_sk_buff_t*, int);
static gssize pgm_transport_send_one_copy (pgm_transport_t*, gconstpointer, gsize, int);
static int send_rdata (pgm_transport_t*, guint32, gpointer, gsize, gboolean, guint32);


/* Linux 2.6 limited to millisecond resolution with conventional timers, however RDTSC
 * and future high-resolution timers allow nanosecond resolution.  Current ethernet technology
 * is limited to microseconds at best so we'll sit there for a bit.
 *
 * on success, returns 0.  on invalid setting, returns -EINVAL.
 */

int
pgm_transport_set_ambient_spm (
	pgm_transport_t*	transport,
	guint			spm_ambient_interval	/* in microseconds */
	)
{
	g_return_val_if_fail (transport != NULL, -EINVAL);
	g_return_val_if_fail (!transport->is_bound, -EINVAL);
	g_return_val_if_fail (spm_ambient_interval > 0, -EINVAL);

	g_static_mutex_lock (&transport->mutex);
	transport->spm_ambient_interval = spm_ambient_interval;
	g_static_mutex_unlock (&transport->mutex);

	return 0;
}

/* an array of intervals appropriately tuned till ambient period is reached.
 *
 * array is zero leaded for ambient state, and zero terminated for easy detection.
 *
 * on success, returns 0.  on invalid setting, returns -EINVAL.
 */

int
pgm_transport_set_heartbeat_spm (
	pgm_transport_t*	transport,
	const guint*		spm_heartbeat_interval,
	int			len
	)
{
	g_return_val_if_fail (transport != NULL, -EINVAL);
	g_return_val_if_fail (!transport->is_bound, -EINVAL);
	g_return_val_if_fail (len > 0, -EINVAL);
	for (int i = 0; i < len; i++) {
		g_return_val_if_fail (spm_heartbeat_interval[i] > 0, -EINVAL);
	}

	g_static_mutex_lock (&transport->mutex);
	if (transport->spm_heartbeat_interval)
		g_free (transport->spm_heartbeat_interval);
	transport->spm_heartbeat_interval = g_malloc (sizeof(guint) * (len+2));
	memcpy (&transport->spm_heartbeat_interval[1], spm_heartbeat_interval, sizeof(guint) * len);
	transport->spm_heartbeat_interval[0] = transport->spm_heartbeat_interval[len] = 0;
	g_static_mutex_unlock (&transport->mutex);

	return 0;
}

/* 0 < txw_preallocate <= txw_sqns 
 *
 * can only be enforced at bind.
 *
 * on success, returns 0.  on invalid setting, returns -EINVAL.
 */

int
pgm_transport_set_txw_preallocate (
	pgm_transport_t*	transport,
	guint			sqns
	)
{
	g_return_val_if_fail (transport != NULL, -EINVAL);
	g_return_val_if_fail (!transport->is_bound, -EINVAL);
	g_return_val_if_fail (sqns > 0, -EINVAL);

	g_static_mutex_lock (&transport->mutex);
	transport->txw_preallocate = sqns;
	g_static_mutex_unlock (&transport->mutex);

	return 0;
}

/* 0 < txw_sqns < one less than half sequence space
 *
 * on success, returns 0.  on invalid setting, returns -EINVAL.
 */

int
pgm_transport_set_txw_sqns (
	pgm_transport_t*	transport,
	guint			sqns
	)
{
	g_return_val_if_fail (transport != NULL, -EINVAL);
	g_return_val_if_fail (!transport->is_bound, -EINVAL);
	g_return_val_if_fail (sqns < ((UINT32_MAX/2)-1), -EINVAL);
	g_return_val_if_fail (sqns > 0, -EINVAL);

	g_static_mutex_lock (&transport->mutex);
	transport->txw_sqns = sqns;
	g_static_mutex_unlock (&transport->mutex);

	return 0;
}

/* 0 < secs < ( txw_sqns / txw_max_rte )
 *
 * can only be enforced upon bind.
 *
 * on success, returns 0.  on invalid setting, returns -EINVAL.
 */

int
pgm_transport_set_txw_secs (
	pgm_transport_t*	transport,
	guint			secs
	)
{
	g_return_val_if_fail (transport != NULL, -EINVAL);
	g_return_val_if_fail (!transport->is_bound, -EINVAL);
	g_return_val_if_fail (secs > 0, -EINVAL);

	g_static_mutex_lock (&transport->mutex);
	transport->txw_secs = secs;
	g_static_mutex_unlock (&transport->mutex);

	return 0;
}

/* 0 < txw_max_rte < interface capacity
 *
 *  10mb :   1250000
 * 100mb :  12500000
 *   1gb : 125000000
 *
 * no practical way to determine upper limit and enforce.
 *
 * on success, returns 0.  on invalid setting, returns -EINVAL.
 */

int
pgm_transport_set_txw_max_rte (
	pgm_transport_t*	transport,
	guint			max_rte
	)
{
	g_return_val_if_fail (transport != NULL, -EINVAL);
	g_return_val_if_fail (!transport->is_bound, -EINVAL);
	g_return_val_if_fail (max_rte > 0, -EINVAL);

	g_static_mutex_lock (&transport->mutex);
	transport->txw_max_rte = max_rte;
	g_static_mutex_unlock (&transport->mutex);

	return 0;
}

/* prototype of function to send pro-active parity NAKs.
 */
static int
pgm_schedule_proactive_nak (
	pgm_transport_t*	transport,
	guint32			nak_tg_sqn	/* transmission group (shifted) */
	)
{
	int retval = 0;

	pgm_txw_retransmit_push (transport->txw,
				 nak_tg_sqn | transport->rs_proactive_h,
				 TRUE /* is_parity */,
				 transport->tg_sqn_shift);
	if (!pgm_notify_send (&transport->rdata_notify)) {
		g_critical ("send to rdata notify channel failed :(");
		retval = -EINVAL;
	}
	return retval;
}

/* a deferred request for RDATA, now processing in the timer thread, we check the transmit
 * window to see if the packet exists and forward on, maintaining a lock until the queue is
 * empty.
 *
 * returns TRUE to keep monitoring the event source.
 */

gboolean
on_nak_notify (
	G_GNUC_UNUSED GIOChannel*	source,
	G_GNUC_UNUSED GIOCondition	condition,
	gpointer			data
	)
{
	pgm_transport_t* transport = data;

/* remove one event from notify channel */
	pgm_notify_read (&transport->rdata_notify);

/* We can flush queue and block all odata, or process one set, or process each
 * sequence number individually.
 */
	struct pgm_sk_buff_t* r_skb;
	guint32		unfolded_checksum;
	gboolean	is_parity = FALSE;
	guint		rs_h = 0;

/* parity packets are re-numbered across the transmission group with index h, sharing the space
 * with the original packets.  beyond the transmission group size (k), the PGM option OPT_PARITY_GRP
 * provides the extra offset value.
 */

/* peek from the retransmit queue so we can eliminate duplicate NAKs up until the repair packet
 * has been retransmitted.
 */
	g_static_rw_lock_reader_lock (&transport->txw_lock);
	if (!pgm_txw_retransmit_try_peek (transport->txw, &r_skb, &unfolded_checksum, &is_parity, &rs_h))
	{
		gboolean is_var_pktlen = FALSE;
		gboolean has_saved_partial_csum = TRUE;

		rs_h %= transport->rs_n - transport->rs_k;	/* wrap around 2t parity packets */

/* calculate parity packet */
		if (is_parity)
		{
			const guint32 tg_sqn_mask = 0xffffffff << transport->tg_sqn_shift;
			const guint32 tg_sqn = r_skb->sequence & tg_sqn_mask;

			gboolean is_op_encoded = FALSE;

			guint16 parity_length = 0;
			const guint8* src[ transport->rs_k ];
			for (unsigned i = 0; i < transport->rs_k; i++)
			{
				const struct pgm_sk_buff_t* odata_skb = pgm_txw_peek (transport->txw, tg_sqn + i);
				const guint16 odata_tsdu_length = g_ntohs (odata_skb->pgm_header->pgm_tsdu_length);
				if (!parity_length)
				{
					parity_length = odata_tsdu_length;
				}
				else if (odata_tsdu_length != parity_length)
				{
					is_var_pktlen = TRUE;

					if (odata_tsdu_length > parity_length)
						parity_length = odata_tsdu_length;
				}

				src[i] = odata_skb->data;
				if (odata_skb->pgm_header->pgm_options & PGM_OPT_PRESENT) {
					is_op_encoded = TRUE;
				}
			}

/* construct basic PGM header to be completed by send_rdata() */
			r_skb = transport->parity_buffer;
			r_skb->data = r_skb->tail = r_skb->head;

/* space for PGM header */
			pgm_skb_put (r_skb, sizeof(struct pgm_header));

			r_skb->pgm_header	= r_skb->data;
			r_skb->pgm_data		= (gpointer)( r_skb->pgm_header + 1 );
			memcpy (r_skb->pgm_header->pgm_gsi, &transport->tsi.gsi, sizeof(pgm_gsi_t));
			r_skb->pgm_header->pgm_options = PGM_OPT_PARITY;

/* append actual TSDU length if variable length packets, zero pad as necessary.
 */
			if (is_var_pktlen)
			{
				r_skb->pgm_header->pgm_options |= PGM_OPT_VAR_PKTLEN;

				for (unsigned i = 0; i < transport->rs_k; i++)
				{
					struct pgm_sk_buff_t* odata_skb = pgm_txw_peek (transport->txw, tg_sqn + i);
					const guint16 odata_tsdu_length = g_ntohs (odata_skb->pgm_header->pgm_tsdu_length);

					g_assert (odata_tsdu_length == odata_skb->len);
					g_assert (parity_length >= odata_tsdu_length);

					if (!odata_skb->zero_padded) {
						memset (odata_skb->tail, 0, parity_length - odata_tsdu_length);
						*(guint16*)((guint8*)odata_skb->data + parity_length) = odata_tsdu_length;
						odata_skb->zero_padded = 1;
					}
				}
				parity_length += 2;
			}

			r_skb->pgm_header->pgm_tsdu_length = g_htons (parity_length);

/* space for DATA */
			pgm_skb_put (r_skb, sizeof(struct pgm_data) + parity_length);

			r_skb->pgm_data->data_sqn	= g_htonl ( tg_sqn | rs_h );

			gpointer data_bytes		= r_skb->pgm_data + 1;

/* encode every option separately, currently only one applies: opt_fragment
 */
			if (is_op_encoded)
			{
				r_skb->pgm_header->pgm_options |= PGM_OPT_PRESENT;

				struct pgm_opt_fragment null_opt_fragment;
				guint8* opt_src[ transport->rs_k ];
				memset (&null_opt_fragment, 0, sizeof(null_opt_fragment));
				*(guint8*)&null_opt_fragment |= PGM_OP_ENCODED_NULL;
				for (unsigned i = 0; i < transport->rs_k; i++)
				{
					const struct pgm_sk_buff_t* odata_skb = pgm_txw_peek (transport->txw, tg_sqn + i);

					if (odata_skb->pgm_opt_fragment)
					{
						g_assert (odata_skb->pgm_header->pgm_options & PGM_OPT_PRESENT);
/* skip three bytes of header */
						opt_src[i] = (guint8*)odata_skb->pgm_opt_fragment + sizeof (struct pgm_opt_header);
					}
					else
					{
						opt_src[i] = (guint8*)&null_opt_fragment;
					}
				}

/* add options to this rdata packet */
				const guint16 opt_total_length = sizeof(struct pgm_opt_length) +
								 sizeof(struct pgm_opt_header) +
								 sizeof(struct pgm_opt_fragment);

/* add space for PGM options */
				pgm_skb_put (r_skb, opt_total_length);

				struct pgm_opt_length* opt_len		= data_bytes;
				opt_len->opt_type			= PGM_OPT_LENGTH;
				opt_len->opt_length			= sizeof(struct pgm_opt_length);
				opt_len->opt_total_length		= g_htons ( opt_total_length );
				struct pgm_opt_header* opt_header 	= (struct pgm_opt_header*)(opt_len + 1);
				opt_header->opt_type			= PGM_OPT_FRAGMENT | PGM_OPT_END;
				opt_header->opt_length			= sizeof(struct pgm_opt_header) + sizeof(struct pgm_opt_fragment);
				opt_header->opt_reserved 		= PGM_OP_ENCODED;
				struct pgm_opt_fragment* opt_fragment	= (struct pgm_opt_fragment*)(opt_header + 1);

/* The cast below is the correct way to handle the problem. 
 * The (void *) cast is to avoid a GCC warning like: 
 *
 *   "warning: dereferencing type-punned pointer will break strict-aliasing rules"
 */
				pgm_rs_encode (transport->rs, (const void**)(void*)opt_src, transport->rs_k + rs_h, opt_fragment + sizeof(struct pgm_opt_header), sizeof(struct pgm_opt_fragment) - sizeof(struct pgm_opt_header));

				data_bytes = opt_fragment + 1;
			}

/* encode payload */
			pgm_rs_encode (transport->rs, (const void**)(void*)src, transport->rs_k + rs_h, data_bytes, parity_length);
			has_saved_partial_csum = FALSE;
		}

		send_rdata (transport, r_skb->sequence, r_skb->data, r_skb->len, has_saved_partial_csum, unfolded_checksum);

/* now remove sequence number from retransmit queue, re-enabling NAK processing for this sequence number */
		pgm_txw_retransmit_remove_head (transport->txw);
	}
	g_static_rw_lock_reader_unlock (&transport->txw_lock);

	return TRUE;
}

/* SPMR indicates if multicast to cancel own SPMR, or unicast to send SPM.
 *
 * rate limited to 1/IHB_MIN per TSI (13.4).
 *
 * if SPMR was valid, returns 0.
 */

int
on_spmr (
	pgm_transport_t*	transport,
	pgm_peer_t*		peer,
	struct pgm_header*	header,
	gpointer		data,
	gsize			len
	)
{
	g_trace ("INFO","on_spmr()");

	int retval;

	if ((retval = pgm_verify_spmr (header, data, len)) == 0)
	{

/* we are the source */
		if (peer == NULL)
		{
			send_spm (transport);
		}
		else
		{
/* we are a peer */
			g_trace ("INFO", "suppressing SPMR due to peer multicast SPMR.");
			g_static_mutex_lock (&peer->mutex);
			peer->spmr_expiry = 0;
			g_static_mutex_unlock (&peer->mutex);
		}
	}
	else
	{
		transport->cumulative_stats[PGM_PC_SOURCE_PACKETS_DISCARDED]++;
	}

	return retval;
}

/* NAK requesting RDATA transmission for a sending transport, only valid if
 * sequence number(s) still in transmission window.
 *
 * we can potentially have different IP versions for the NAK packet to the send group.
 *
 * TODO: fix IPv6 AFIs
 *
 * take in a NAK and pass off to an asynchronous queue for another thread to process
 *
 * if NAK is valid, returns 0.  on error, -EINVAL is returned.
 */

int
on_nak (
	pgm_transport_t*	transport,
	struct pgm_header*	header,
	gpointer		data,
	gsize			len
	)
{
	g_trace ("INFO","on_nak()");

	const gboolean is_parity = header->pgm_options & PGM_OPT_PARITY;

	if (is_parity) {
		transport->cumulative_stats[PGM_PC_SOURCE_PARITY_NAKS_RECEIVED]++;

		if (!transport->use_ondemand_parity) {
			transport->cumulative_stats[PGM_PC_SOURCE_MALFORMED_NAKS]++;
			transport->cumulative_stats[PGM_PC_SOURCE_PACKETS_DISCARDED]++;
			goto out;
		}
	} else {
		transport->cumulative_stats[PGM_PC_SOURCE_SELECTIVE_NAKS_RECEIVED]++;
	}

	int retval;
	if ((retval = pgm_verify_nak (header, data, len)) != 0)
	{
		transport->cumulative_stats[PGM_PC_SOURCE_MALFORMED_NAKS]++;
		transport->cumulative_stats[PGM_PC_SOURCE_PACKETS_DISCARDED]++;
		goto out;
	}

	const struct pgm_nak* nak = (struct pgm_nak*)data;
	const struct pgm_nak6* nak6 = (struct pgm_nak6*)data;
		
/* NAK_SRC_NLA contains our transport unicast NLA */
	struct sockaddr_storage nak_src_nla;
	pgm_nla_to_sockaddr (&nak->nak_src_nla_afi, (struct sockaddr*)&nak_src_nla);

	if (pgm_sockaddr_cmp ((struct sockaddr*)&nak_src_nla, (struct sockaddr*)&transport->send_addr) != 0) {
		retval = -EINVAL;
		transport->cumulative_stats[PGM_PC_SOURCE_MALFORMED_NAKS]++;
		transport->cumulative_stats[PGM_PC_SOURCE_PACKETS_DISCARDED]++;
		goto out;
	}

/* NAK_GRP_NLA containers our transport multicast group */ 
	struct sockaddr_storage nak_grp_nla;
	pgm_nla_to_sockaddr ((nak->nak_src_nla_afi == AFI_IP6) ? &nak6->nak6_grp_nla_afi : &nak->nak_grp_nla_afi,
				(struct sockaddr*)&nak_grp_nla);

	if (pgm_sockaddr_cmp ((struct sockaddr*)&nak_grp_nla, (struct sockaddr*)&transport->send_gsr.gsr_group) != 0) {
		retval = -EINVAL;
		transport->cumulative_stats[PGM_PC_SOURCE_MALFORMED_NAKS]++;
		transport->cumulative_stats[PGM_PC_SOURCE_PACKETS_DISCARDED]++;
		goto out;
	}

/* create queue object */
	pgm_sqn_list_t sqn_list;
	sqn_list.sqn[0] = g_ntohl (nak->nak_sqn);
	sqn_list.len = 1;

	g_trace ("INFO", "nak_sqn %" G_GUINT32_FORMAT, sqn_list.sqn[0]);

/* check NAK list */
	const guint32* nak_list = NULL;
	guint nak_list_len = 0;
	if (header->pgm_options & PGM_OPT_PRESENT)
	{
		const struct pgm_opt_length* opt_len = (nak->nak_src_nla_afi == AFI_IP6) ?
							(const struct pgm_opt_length*)(nak6 + 1) :
							(const struct pgm_opt_length*)(nak + 1);
		if (opt_len->opt_type != PGM_OPT_LENGTH)
		{
			retval = -EINVAL;
			transport->cumulative_stats[PGM_PC_SOURCE_MALFORMED_NAKS]++;
			transport->cumulative_stats[PGM_PC_SOURCE_PACKETS_DISCARDED]++;
			goto out;
		}
		if (opt_len->opt_length != sizeof(struct pgm_opt_length))
		{
			retval = -EINVAL;
			transport->cumulative_stats[PGM_PC_SOURCE_MALFORMED_NAKS]++;
			transport->cumulative_stats[PGM_PC_SOURCE_PACKETS_DISCARDED]++;
			goto out;
		}
/* TODO: check for > 16 options & past packet end */
		const struct pgm_opt_header* opt_header = (const struct pgm_opt_header*)opt_len;
		do {
			opt_header = (const struct pgm_opt_header*)((const char*)opt_header + opt_header->opt_length);

			if ((opt_header->opt_type & PGM_OPT_MASK) == PGM_OPT_NAK_LIST)
			{
				nak_list = ((const struct pgm_opt_nak_list*)(opt_header + 1))->opt_sqn;
				nak_list_len = ( opt_header->opt_length - sizeof(struct pgm_opt_header) - sizeof(guint8) ) / sizeof(guint32);
				break;
			}
		} while (!(opt_header->opt_type & PGM_OPT_END));
	}

/* nak list numbers */
#ifdef TRANSPORT_DEBUG
	if (nak_list)
	{
		char nak_sz[1024] = "";
		const guint32 *nakp = nak_list, *nake = nak_list + nak_list_len;
		while (nakp < nake) {
			char tmp[1024];
			sprintf (tmp, "%" G_GUINT32_FORMAT " ", g_ntohl(*nakp));
			strcat (nak_sz, tmp);
			nakp++;
		}
	g_trace ("INFO", "nak list %s", nak_sz);
	}
#endif
	for (unsigned i = 0; i < nak_list_len; i++)
	{
		sqn_list.sqn[sqn_list.len++] = g_ntohl (*nak_list);
		nak_list++;
	}

/* send NAK confirm packet immediately, then defer to timer thread for a.s.a.p
 * delivery of the actual RDATA packets.
 */
	if (nak_list_len) {
		send_ncf_list (transport, (struct sockaddr*)&nak_src_nla, (struct sockaddr*)&nak_grp_nla, &sqn_list, is_parity);
	} else {
		send_ncf (transport, (struct sockaddr*)&nak_src_nla, (struct sockaddr*)&nak_grp_nla, sqn_list.sqn[0], is_parity);
	}

/* queue retransmit requests */
	for (unsigned i = 0; i < sqn_list.len; i++)
	{
		int cnt = pgm_txw_retransmit_push (transport->txw, sqn_list.sqn[i], is_parity, transport->tg_sqn_shift);
		if (cnt > 0)
		{
			if (!pgm_notify_send (&transport->rdata_notify)) {
				g_critical ("send to rdata notify channel failed :(");
				retval = -EINVAL;
			}
		}
	}

out:
	return retval;
}

/* Null-NAK, or N-NAK propogated by a DLR for hand waving excitement
 *
 * if NNAK is valid, returns 0.  on error, -EINVAL is returned.
 */

int
on_nnak (
	pgm_transport_t*	transport,
	struct pgm_header*	header,
	gpointer		data,
	gsize			len
	)
{
	g_trace ("INFO","on_nnak()");
	transport->cumulative_stats[PGM_PC_SOURCE_SELECTIVE_NNAK_PACKETS_RECEIVED]++;

	int retval;
	if ((retval = pgm_verify_nnak (header, data, len)) != 0)
	{
		transport->cumulative_stats[PGM_PC_SOURCE_NNAK_ERRORS]++;
		transport->cumulative_stats[PGM_PC_SOURCE_PACKETS_DISCARDED]++;
		goto out;
	}

	const struct pgm_nak* nnak = (struct pgm_nak*)data;
	const struct pgm_nak6* nnak6 = (struct pgm_nak6*)data;
		
/* NAK_SRC_NLA contains our transport unicast NLA */
	struct sockaddr_storage nnak_src_nla;
	pgm_nla_to_sockaddr (&nnak->nak_src_nla_afi, (struct sockaddr*)&nnak_src_nla);

	if (pgm_sockaddr_cmp ((struct sockaddr*)&nnak_src_nla, (struct sockaddr*)&transport->send_addr) != 0) {
		retval = -EINVAL;
		transport->cumulative_stats[PGM_PC_SOURCE_NNAK_ERRORS]++;
		transport->cumulative_stats[PGM_PC_SOURCE_PACKETS_DISCARDED]++;
		goto out;
	}

/* NAK_GRP_NLA containers our transport multicast group */ 
	struct sockaddr_storage nnak_grp_nla;
	pgm_nla_to_sockaddr ((nnak->nak_src_nla_afi == AFI_IP6) ? &nnak6->nak6_grp_nla_afi : &nnak->nak_grp_nla_afi,
				(struct sockaddr*)&nnak_grp_nla);

	if (pgm_sockaddr_cmp ((struct sockaddr*)&nnak_grp_nla, (struct sockaddr*)&transport->send_gsr.gsr_group) != 0) {
		retval = -EINVAL;
		transport->cumulative_stats[PGM_PC_SOURCE_NNAK_ERRORS]++;
		transport->cumulative_stats[PGM_PC_SOURCE_PACKETS_DISCARDED]++;
		goto out;
	}

/* check NNAK list */
	guint nnak_list_len = 0;
	if (header->pgm_options & PGM_OPT_PRESENT)
	{
		const struct pgm_opt_length* opt_len = (nnak->nak_src_nla_afi == AFI_IP6) ?
							(const struct pgm_opt_length*)(nnak6 + 1) :
							(const struct pgm_opt_length*)(nnak + 1);
		if (opt_len->opt_type != PGM_OPT_LENGTH)
		{
			retval = -EINVAL;
			transport->cumulative_stats[PGM_PC_SOURCE_NNAK_ERRORS]++;
			transport->cumulative_stats[PGM_PC_SOURCE_PACKETS_DISCARDED]++;
			goto out;
		}
		if (opt_len->opt_length != sizeof(struct pgm_opt_length))
		{
			retval = -EINVAL;
			transport->cumulative_stats[PGM_PC_SOURCE_NNAK_ERRORS]++;
			transport->cumulative_stats[PGM_PC_SOURCE_PACKETS_DISCARDED]++;
			goto out;
		}
/* TODO: check for > 16 options & past packet end */
		const struct pgm_opt_header* opt_header = (const struct pgm_opt_header*)opt_len;
		do {
			opt_header = (const struct pgm_opt_header*)((const char*)opt_header + opt_header->opt_length);

			if ((opt_header->opt_type & PGM_OPT_MASK) == PGM_OPT_NAK_LIST)
			{
				nnak_list_len = ( opt_header->opt_length - sizeof(struct pgm_opt_header) - sizeof(guint8) ) / sizeof(guint32);
				break;
			}
		} while (!(opt_header->opt_type & PGM_OPT_END));
	}

	transport->cumulative_stats[PGM_PC_SOURCE_SELECTIVE_NNAKS_RECEIVED] += 1 + nnak_list_len;

out:
	return retval;
}

/* ambient/heartbeat SPM's
 *
 * heartbeat: ihb_tmr decaying between ihb_min and ihb_max 2x after last packet
 *
 * on success, 0 is returned.  on error, -1 is returned, and errno set
 * appropriately.
 */

static
int
send_spm (
	pgm_transport_t*	transport
	)
{
	g_static_mutex_lock (&transport->mutex);
	int result = send_spm_unlocked (transport);
	g_static_mutex_unlock (&transport->mutex);
	return result;
}

int
send_spm_unlocked (
	pgm_transport_t*	transport
	)
{
	g_trace ("SPM","send_spm");

/* recycles a transport global packet */
	struct pgm_header *header = (struct pgm_header*)transport->spm_packet;
	struct pgm_spm *spm = (struct pgm_spm*)(header + 1);

	spm->spm_sqn		= g_htonl (transport->spm_sqn++);
	g_static_rw_lock_reader_lock (&transport->txw_lock);
	spm->spm_trail		= g_htonl (pgm_txw_trail(transport->txw));
	spm->spm_lead		= g_htonl (pgm_txw_lead(transport->txw));
	g_static_rw_lock_reader_unlock (&transport->txw_lock);

/* checksum optional for SPMs */
	header->pgm_checksum	= 0;
	header->pgm_checksum	= pgm_csum_fold (pgm_csum_partial ((char*)header, transport->spm_len, 0));

	gssize sent = pgm_sendto (transport,
				TRUE,				/* rate limited */
				TRUE,				/* with router alert */
				header,
				transport->spm_len,
				MSG_CONFIRM,			/* not expecting a reply */
				(struct sockaddr*)&transport->send_gsr.gsr_group,
				pgm_sockaddr_len(&transport->send_gsr.gsr_group));

	if ( sent != (gssize)transport->spm_len )
	{
		return -1;
	}

	transport->cumulative_stats[PGM_PC_SOURCE_BYTES_SENT] += transport->spm_len;
	return 0;
}

/* send a NAK confirm (NCF) message with provided sequence number list.
 *
 * on success, 0 is returned.  on error, -1 is returned, and errno set
 * appropriately.
 */

static
int
send_ncf (
	pgm_transport_t*	transport,
	struct sockaddr*	nak_src_nla,
	struct sockaddr*	nak_grp_nla,
	guint32			sequence_number,
	gboolean		is_parity		/* send parity NCF */
	)
{
	g_trace ("INFO", "send_ncf()");

	gsize tpdu_length = sizeof(struct pgm_header) + sizeof(struct pgm_nak);
	if (pgm_sockaddr_family(nak_src_nla) == AF_INET6) {
		tpdu_length += sizeof(struct pgm_nak6) - sizeof(struct pgm_nak);
	}
	guint8 buf[ tpdu_length ];

	struct pgm_header *header = (struct pgm_header*)buf;
	struct pgm_nak *ncf = (struct pgm_nak*)(header + 1);
	struct pgm_nak6 *ncf6 = (struct pgm_nak6*)(header + 1);
	memcpy (header->pgm_gsi, &transport->tsi.gsi, sizeof(pgm_gsi_t));
	
	header->pgm_sport	= transport->tsi.sport;
	header->pgm_dport	= transport->dport;
	header->pgm_type        = PGM_NCF;
        header->pgm_options     = is_parity ? PGM_OPT_PARITY : 0;
        header->pgm_tsdu_length = 0;

/* NCF */
	ncf->nak_sqn		= g_htonl (sequence_number);

/* source nla */
	pgm_sockaddr_to_nla (nak_src_nla, (char*)&ncf->nak_src_nla_afi);

/* group nla */
	pgm_sockaddr_to_nla (nak_grp_nla, (ncf->nak_src_nla_afi == AFI_IP6) ?
						(char*)&ncf6->nak6_grp_nla_afi :
						(char*)&ncf->nak_grp_nla_afi );

        header->pgm_checksum    = 0;
        header->pgm_checksum	= pgm_csum_fold (pgm_csum_partial ((char*)header, tpdu_length, 0));

	gssize sent = pgm_sendto (transport,
				FALSE,			/* not rate limited */
				TRUE,			/* with router alert */
				header,
				tpdu_length,
				MSG_CONFIRM,		/* not expecting a reply */
				(struct sockaddr*)&transport->send_gsr.gsr_group,
				pgm_sockaddr_len(&transport->send_gsr.gsr_group));

	if ( sent != (gssize)tpdu_length )
	{
		return -1;
	}

	transport->cumulative_stats[PGM_PC_SOURCE_BYTES_SENT] += tpdu_length;

	return 0;
}

/* A NCF packet with a OPT_NAK_LIST option extension
 *
 * on success, 0 is returned.  on error, -1 is returned, and errno set
 * appropriately.
 */

static
int
send_ncf_list (
	pgm_transport_t*	transport,
	struct sockaddr*	nak_src_nla,
	struct sockaddr*	nak_grp_nla,
	pgm_sqn_list_t*		sqn_list,
	gboolean		is_parity		/* send parity NCF */
	)
{
	g_assert (sqn_list->len > 1);
	g_assert (sqn_list->len <= 63);
	g_assert (pgm_sockaddr_family(nak_src_nla) == pgm_sockaddr_family(nak_grp_nla));

	gsize tpdu_length = sizeof(struct pgm_header) + sizeof(struct pgm_nak)
			+ sizeof(struct pgm_opt_length)		/* includes header */
			+ sizeof(struct pgm_opt_header) + sizeof(struct pgm_opt_nak_list)
			+ ( (sqn_list->len-1) * sizeof(guint32) );
	if (pgm_sockaddr_family(nak_src_nla) == AFI_IP6) {
		tpdu_length += sizeof(struct pgm_nak6) - sizeof(struct pgm_nak);
	}
	guint8 buf[ tpdu_length ];

	struct pgm_header *header = (struct pgm_header*)buf;
	struct pgm_nak *ncf = (struct pgm_nak*)(header + 1);
	struct pgm_nak6 *ncf6 = (struct pgm_nak6*)(header + 1);
	memcpy (header->pgm_gsi, &transport->tsi.gsi, sizeof(pgm_gsi_t));

	header->pgm_sport	= transport->tsi.sport;
	header->pgm_dport	= transport->dport;
	header->pgm_type        = PGM_NCF;
        header->pgm_options     = is_parity ? (PGM_OPT_PRESENT | PGM_OPT_NETWORK | PGM_OPT_PARITY) : (PGM_OPT_PRESENT | PGM_OPT_NETWORK);
        header->pgm_tsdu_length = 0;

/* NCF */
	ncf->nak_sqn		= g_htonl (sqn_list->sqn[0]);

/* source nla */
	pgm_sockaddr_to_nla (nak_src_nla, (char*)&ncf->nak_src_nla_afi);

/* group nla */
	pgm_sockaddr_to_nla (nak_grp_nla, (ncf->nak_src_nla_afi == AFI_IP6) ? 
						(char*)&ncf6->nak6_grp_nla_afi :
						(char*)&ncf->nak_grp_nla_afi );

/* OPT_NAK_LIST */
	struct pgm_opt_length* opt_len = (ncf->nak_src_nla_afi == AFI_IP6) ?
						(struct pgm_opt_length*)(ncf6 + 1) :
						(struct pgm_opt_length*)(ncf + 1);
	opt_len->opt_type	= PGM_OPT_LENGTH;
	opt_len->opt_length	= sizeof(struct pgm_opt_length);
	opt_len->opt_total_length = g_htons (	sizeof(struct pgm_opt_length) +
						sizeof(struct pgm_opt_header) +
						sizeof(struct pgm_opt_nak_list) +
						( (sqn_list->len-1) * sizeof(guint32) ) );
	struct pgm_opt_header* opt_header = (struct pgm_opt_header*)(opt_len + 1);
	opt_header->opt_type	= PGM_OPT_NAK_LIST | PGM_OPT_END;
	opt_header->opt_length	= sizeof(struct pgm_opt_header) + sizeof(struct pgm_opt_nak_list)
				+ ( (sqn_list->len-1) * sizeof(guint32) );
	struct pgm_opt_nak_list* opt_nak_list = (struct pgm_opt_nak_list*)(opt_header + 1);
	opt_nak_list->opt_reserved = 0;

#ifdef TRANSPORT_DEBUG
	char nak1[1024];
	sprintf (nak1, "send_ncf_list( %" G_GUINT32_FORMAT " + [", sqn_list->sqn[0]);
#endif
	for (unsigned i = 1; i < sqn_list->len; i++) {
		opt_nak_list->opt_sqn[i-1] = g_htonl (sqn_list->sqn[i]);

#ifdef TRANSPORT_DEBUG
		char nak2[1024];
		sprintf (nak2, "%" G_GUINT32_FORMAT " ", sqn_list->sqn[i]);
		strcat (nak1, nak2);
#endif
	}

#ifdef TRANSPORT_DEBUG
	g_trace ("INFO", "%s]%i )", nak1, sqn_list->len);
#endif

        header->pgm_checksum    = 0;
        header->pgm_checksum	= pgm_csum_fold (pgm_csum_partial ((char*)header, tpdu_length, 0));

	gssize sent = pgm_sendto (transport,
				FALSE,			/* not rate limited */
				TRUE,			/* with router alert */
				header,
				tpdu_length,
				MSG_CONFIRM,		/* not expecting a reply */
				(struct sockaddr*)&transport->send_gsr.gsr_group,
				pgm_sockaddr_len(&transport->send_gsr.gsr_group));

	if ( sent != (gssize)tpdu_length )
	{
		return -1;
	}

	transport->cumulative_stats[PGM_PC_SOURCE_BYTES_SENT] += tpdu_length;

	return 0;
}

/* cancel any pending heartbeat SPM and schedule a new one
 *
 * on success, 0 is returned.  on error, -1 is returned, and errno set
 * appropriately.
 */

static
int
pgm_reset_heartbeat_spm (pgm_transport_t* transport)
{
	int retval = 0;

	g_static_mutex_lock (&transport->mutex);

/* re-set spm timer */
	transport->spm_heartbeat_state = 1;
	transport->next_heartbeat_spm = pgm_time_update_now() + transport->spm_heartbeat_interval[transport->spm_heartbeat_state++];

/* prod timer thread if sleeping */
	if (pgm_time_after( transport->next_poll, transport->next_heartbeat_spm ))
	{
		transport->next_poll = transport->next_heartbeat_spm;
		g_trace ("INFO","pgm_reset_heartbeat_spm: prod timer thread");
		if (!pgm_notify_send (&transport->timer_notify)) {
			g_critical ("send to timer notify channel failed :(");
			retval = -EINVAL;
		}
	}

	g_static_mutex_unlock (&transport->mutex);

	return retval;
}

/* state helper for resuming sends
 */
#define STATE(x)	(transport->pkt_dontwait_state.x)

/* send one PGM data packet, transmit window owned memory.
 *
 * on success, returns number of data bytes pushed into the transmit window and
 * attempted to send to the socket layer.  on non-blocking sockets, -1 is returned
 * if the packet sizes would exceed the current rate limit. on invalid arguments,
 * -EINVAL is returned.
 *
 * ! always returns successful if data is pushed into the transmit window, even if
 * sendto() double fails ¡  we don't want the application to try again as that is the
 * reliable transports role.
 */

static
gssize
pgm_transport_send_one (
	pgm_transport_t*	transport,
	struct pgm_sk_buff_t*	skb,
	int			flags
	)
{
	g_return_val_if_fail (transport != NULL, -EINVAL);
	g_return_val_if_fail (skb != NULL, -EINVAL);
	g_return_val_if_fail (skb->len <= transport->max_tsdu, -EMSGSIZE);

	g_assert( !(flags & MSG_WAITALL && !(flags & MSG_DONTWAIT)) );

	const guint16 tsdu_length = skb->len;

/* continue if send would block */
	if (transport->is_apdu_eagain) {
		goto retry_send;
	}

/* add PGM header to skbuff */
	STATE(skb) = pgm_skb_get(skb);
	STATE(skb)->transport = transport;
	STATE(skb)->tstamp = pgm_time_update_now();
	STATE(skb)->data  = (guint8*)STATE(skb)->data - pgm_transport_pkt_offset(FALSE);
	STATE(skb)->len  += pgm_transport_pkt_offset(FALSE);

	STATE(skb)->pgm_header = (struct pgm_header*)STATE(skb)->data;
	STATE(skb)->pgm_data   = (struct pgm_data*)(STATE(skb)->pgm_header + 1);
	memcpy (STATE(skb)->pgm_header->pgm_gsi, &transport->tsi.gsi, sizeof(pgm_gsi_t));
	STATE(skb)->pgm_header->pgm_sport	= transport->tsi.sport;
	STATE(skb)->pgm_header->pgm_dport	= transport->dport;
	STATE(skb)->pgm_header->pgm_type        = PGM_ODATA;
        STATE(skb)->pgm_header->pgm_options     = 0;
        STATE(skb)->pgm_header->pgm_tsdu_length = g_htons (tsdu_length);

	g_static_rw_lock_writer_lock (&transport->txw_lock);

/* ODATA */
        STATE(skb)->pgm_data->data_sqn		= g_htonl (pgm_txw_next_lead(transport->txw));
        STATE(skb)->pgm_data->data_trail	= g_htonl (pgm_txw_trail(transport->txw));

        STATE(skb)->pgm_header->pgm_checksum    = 0;
	const gsize pgm_header_len		= (guint8*)(STATE(skb)->pgm_data + 1) - (guint8*)STATE(skb)->pgm_header;
	const guint32 unfolded_header		= pgm_csum_partial (STATE(skb)->pgm_header, pgm_header_len, 0);
	STATE(unfolded_odata)			= pgm_csum_partial ((guint8*)(STATE(skb)->pgm_data + 1), tsdu_length, 0);
        STATE(skb)->pgm_header->pgm_checksum	= pgm_csum_fold (pgm_csum_block_add (unfolded_header, STATE(unfolded_odata), pgm_header_len));

/* add to transmit window */
	pgm_txw_add (transport->txw, STATE(skb));

	gssize sent;
retry_send:
	sent = pgm_sendto (transport,
				TRUE,			/* rate limited */
				FALSE,			/* regular socket */
				STATE(skb)->data,
				STATE(skb)->len,
				(flags & MSG_DONTWAIT && flags & MSG_WAITALL) ?
					flags & ~(MSG_DONTWAIT | MSG_WAITALL) :
					flags,
				(struct sockaddr*)&transport->send_gsr.gsr_group,
				pgm_sockaddr_len(&transport->send_gsr.gsr_group));
	if (sent < 0 && errno == EAGAIN)
	{
		transport->is_apdu_eagain = TRUE;
		return -1;
	}

/* save unfolded odata for retransmissions */
	*(guint32*)&STATE(skb)->cb = STATE(unfolded_odata);

/* release txw lock here in order to allow spms to lock mutex */
	g_static_rw_lock_writer_unlock (&transport->txw_lock);

	transport->is_apdu_eagain = FALSE;
	pgm_reset_heartbeat_spm (transport);

	if ( sent == (gssize)STATE(skb)->len )
	{
		transport->cumulative_stats[PGM_PC_SOURCE_DATA_BYTES_SENT] += tsdu_length;
		transport->cumulative_stats[PGM_PC_SOURCE_DATA_MSGS_SENT]  ++;
		transport->cumulative_stats[PGM_PC_SOURCE_BYTES_SENT]	   += STATE(skb)->len + transport->iphdr_len;
	}

/* check for end of transmission group */
	if (transport->use_proactive_parity)
	{
		const guint32 odata_sqn = g_ntohl (STATE(skb)->pgm_data->data_sqn);
		const guint32 tg_sqn_mask = 0xffffffff << transport->tg_sqn_shift;
		if (!((odata_sqn + 1) & ~tg_sqn_mask))
		{
			pgm_schedule_proactive_nak (transport, odata_sqn & tg_sqn_mask);
		}
	}

/* remove applications reference to skbuff */
	pgm_free_skb (STATE(skb));
	return (gssize)tsdu_length;
}

/* send one PGM original data packet, callee owned memory.
 *
 * on success, returns number of data bytes pushed into the transmit window and
 * attempted to send to the socket layer.  on non-blocking sockets, -1 is returned
 * if the packet sizes would exceed the current rate limit.
 */

static
gssize
pgm_transport_send_one_copy (
	pgm_transport_t*	transport,
	gconstpointer		tsdu,
	gsize			tsdu_length,
	int			flags
	)
{
	g_return_val_if_fail (transport != NULL, -EINVAL);
	if (tsdu_length) {
		g_return_val_if_fail (tsdu != NULL, -EINVAL);
		g_return_val_if_fail (tsdu_length <= transport->max_tsdu, -EMSGSIZE);
	}

	g_assert( !(flags & MSG_WAITALL && !(flags & MSG_DONTWAIT)) );

/* continue if blocked mid-apdu */
	if (transport->is_apdu_eagain) {
		goto retry_send;
	}

	g_static_rw_lock_writer_lock (&transport->txw_lock);
	STATE(skb) = pgm_alloc_skb (transport->max_tpdu);
	STATE(skb)->transport = transport;
	STATE(skb)->tstamp = pgm_time_update_now();
	pgm_skb_put (STATE(skb), pgm_transport_pkt_offset (FALSE) + tsdu_length);

	STATE(skb)->pgm_header	= (struct pgm_header*)STATE(skb)->data;
	STATE(skb)->pgm_data	= (struct pgm_data*)(STATE(skb)->pgm_header + 1);
	memcpy (STATE(skb)->pgm_header->pgm_gsi, &transport->tsi.gsi, sizeof(pgm_gsi_t));
	STATE(skb)->pgm_header->pgm_sport	= transport->tsi.sport;
	STATE(skb)->pgm_header->pgm_dport	= transport->dport;
	STATE(skb)->pgm_header->pgm_type	= PGM_ODATA;
	STATE(skb)->pgm_header->pgm_options	= 0;
	STATE(skb)->pgm_header->pgm_tsdu_length = g_htons (tsdu_length);

/* ODATA */
	STATE(skb)->pgm_data->data_sqn		= g_htonl (pgm_txw_next_lead(transport->txw));
	STATE(skb)->pgm_data->data_trail	= g_htonl (pgm_txw_trail(transport->txw));

	STATE(skb)->pgm_header->pgm_checksum	= 0;
	const gsize pgm_header_len		= (guint8*)(STATE(skb)->pgm_data + 1) - (guint8*)STATE(skb)->pgm_header;
	const guint32 unfolded_header		= pgm_csum_partial (STATE(skb)->pgm_header, pgm_header_len, 0);
	STATE(unfolded_odata)			= pgm_csum_partial_copy (tsdu, (guint8*)(STATE(skb)->pgm_data + 1), tsdu_length, 0);
	STATE(skb)->pgm_header->pgm_checksum	= pgm_csum_fold (pgm_csum_block_add (unfolded_header, STATE(unfolded_odata), pgm_header_len));

/* add to transmit window */
	pgm_txw_add (transport->txw, STATE(skb));

	gssize sent;
retry_send:
	sent = pgm_sendto (transport,
				TRUE,			/* rate limited */
				FALSE,			/* regular socket */
				STATE(skb)->data,
				STATE(skb)->len,
				(flags & MSG_DONTWAIT && flags & MSG_WAITALL) ?
					flags & ~(MSG_DONTWAIT | MSG_WAITALL) :
					flags,
				(struct sockaddr*)&transport->send_gsr.gsr_group,
				pgm_sockaddr_len(&transport->send_gsr.gsr_group));
	if (sent < 0 && errno == EAGAIN)
	{
		transport->is_apdu_eagain = TRUE;
		return -1;
	}

/* save unfolded odata for retransmissions */
	*(guint32*)&STATE(skb)->cb = STATE(unfolded_odata);

/* release txw lock here in order to allow spms to lock mutex */
	g_static_rw_lock_writer_unlock (&transport->txw_lock);

	transport->is_apdu_eagain = FALSE;
	pgm_reset_heartbeat_spm (transport);

	if ( sent == (gssize)STATE(skb)->len )
	{
		transport->cumulative_stats[PGM_PC_SOURCE_DATA_BYTES_SENT] += tsdu_length;
		transport->cumulative_stats[PGM_PC_SOURCE_DATA_MSGS_SENT]  ++;
		transport->cumulative_stats[PGM_PC_SOURCE_BYTES_SENT]	   += STATE(skb)->len + transport->iphdr_len;
	}

/* check for end of transmission group */
	if (transport->use_proactive_parity)
	{
		const guint32 odata_sqn = g_ntohl (STATE(skb)->pgm_data->data_sqn);
		const guint32 tg_sqn_mask = 0xffffffff << transport->tg_sqn_shift;
		if (!((odata_sqn + 1) & ~tg_sqn_mask))
		{
			pgm_schedule_proactive_nak (transport, odata_sqn & tg_sqn_mask);
		}
	}

/* return data payload length sent */
	return (gssize)tsdu_length;
}

/* send one PGM original data packet, callee owned scatter/gather io vector
 *
 *    ⎢ DATA₀ ⎢
 *    ⎢ DATA₁ ⎢ → pgm_transport_send_onev() →  ⎢ TSDU₀ ⎢ → libc
 *    ⎢   ⋮   ⎢
 *
 * on success, returns number of data bytes pushed into the transmit window and
 * attempted to send to the socket layer.  on non-blocking sockets, -1 is returned
 * if the packet sizes would exceed the current rate limit.
 */

static
gssize
pgm_transport_send_onev (
	pgm_transport_t*	transport,
	const struct pgm_iovec*	vector,
	guint			count,		/* number of items in vector */
	int			flags
	)
{
	g_return_val_if_fail (transport != NULL, -EINVAL);
	if (count) {
		g_return_val_if_fail (vector != NULL, -EINVAL);
	} else {
/* pass on zero length call so we don't have to check count on first iteration. */
		return pgm_transport_send_one_copy (transport, NULL, 0, flags);
	}
	g_return_val_if_fail (transport != NULL, -EINVAL);

	g_assert( !(flags & MSG_WAITALL && !(flags & MSG_DONTWAIT)) );

/* continue if blocked on send */
	if (transport->is_apdu_eagain) {
		goto retry_send;
	}

	STATE(tsdu_length) = 0;
	for (unsigned i = 0; i < count; i++)
	{
#ifdef TRANSPORT_DEBUG
		if (vector[i].iov_len)
		{
			g_assert( vector[i].iov_base );
		}
#endif
		STATE(tsdu_length) += vector[i].iov_len;
	}
	g_return_val_if_fail (STATE(tsdu_length) <= transport->max_tsdu, -EMSGSIZE);

	g_static_rw_lock_writer_lock (&transport->txw_lock);
	STATE(skb) = pgm_alloc_skb (transport->max_tpdu);
	STATE(skb)->transport = transport;
	STATE(skb)->tstamp = pgm_time_update_now();
	pgm_skb_put (STATE(skb), pgm_transport_pkt_offset (FALSE) + STATE(tsdu_length));

	STATE(skb)->pgm_header  = (struct pgm_header*)STATE(skb)->data;
	STATE(skb)->pgm_data    = (struct pgm_data*)(STATE(skb)->pgm_header + 1);
	memcpy (STATE(skb)->pgm_header->pgm_gsi, &transport->tsi.gsi, sizeof(pgm_gsi_t));
	STATE(skb)->pgm_header->pgm_sport	= transport->tsi.sport;
	STATE(skb)->pgm_header->pgm_dport	= transport->dport;
	STATE(skb)->pgm_header->pgm_type	= PGM_ODATA;
	STATE(skb)->pgm_header->pgm_options	= 0;
	STATE(skb)->pgm_header->pgm_tsdu_length = g_htons (STATE(tsdu_length));

/* ODATA */
	STATE(skb)->pgm_data->data_sqn		= g_htonl (pgm_txw_next_lead(transport->txw));
	STATE(skb)->pgm_data->data_trail	= g_htonl (pgm_txw_trail(transport->txw));

	STATE(skb)->pgm_header->pgm_checksum	= 0;
	const gsize pgm_header_len		= (guint8*)(STATE(skb)->pgm_data + 1) - (guint8*)STATE(skb)->pgm_header;
	const guint32 unfolded_header		= pgm_csum_partial (STATE(skb)->pgm_header, pgm_header_len, 0);

/* unroll first iteration to make friendly branch prediction */
	guint8*	dst		= (guint8*)(STATE(skb)->pgm_data + 1);
	STATE(unfolded_odata)	= pgm_csum_partial_copy ((const guint8*)vector[0].iov_base, dst, vector[0].iov_len, 0);

/* iterate over one or more vector elements to perform scatter/gather checksum & copy */
	for (unsigned i = 1; i < count; i++)
	{
		dst += vector[i-1].iov_len;
		const guint32 unfolded_element = pgm_csum_partial_copy ((const guint8*)vector[i].iov_base, dst, vector[i].iov_len, 0);
		STATE(unfolded_odata) = pgm_csum_block_add (STATE(unfolded_odata), unfolded_element, vector[i-1].iov_len);
	}

	STATE(skb)->pgm_header->pgm_checksum	= pgm_csum_fold (pgm_csum_block_add (unfolded_header, STATE(unfolded_odata), pgm_header_len));

/* add to transmit window */
	pgm_txw_add (transport->txw, STATE(skb));

	gssize sent;
retry_send:
	sent = pgm_sendto (transport,
				TRUE,			/* rate limited */
				FALSE,			/* regular socket */
				STATE(skb)->data,
				STATE(skb)->len,
				(flags & MSG_DONTWAIT && flags & MSG_WAITALL) ?
					flags & ~(MSG_DONTWAIT | MSG_WAITALL) :
					flags,
				(struct sockaddr*)&transport->send_gsr.gsr_group,
				pgm_sockaddr_len(&transport->send_gsr.gsr_group));
	if (sent < 0 && errno == EAGAIN)
	{
		transport->is_apdu_eagain = TRUE;
		return -1;
	}

/* save unfolded odata for retransmissions */
	*(guint32*)&STATE(skb)->cb = STATE(unfolded_odata);

/* release txw lock here in order to allow spms to lock mutex */
	g_static_rw_lock_writer_unlock (&transport->txw_lock);

	transport->is_apdu_eagain = FALSE;
	pgm_reset_heartbeat_spm (transport);

	if ( sent == (gssize)STATE(skb)->len )
	{
		transport->cumulative_stats[PGM_PC_SOURCE_DATA_BYTES_SENT] += STATE(tsdu_length);
		transport->cumulative_stats[PGM_PC_SOURCE_DATA_MSGS_SENT]  ++;
		transport->cumulative_stats[PGM_PC_SOURCE_BYTES_SENT]	   += STATE(skb)->len + transport->iphdr_len;
	}

/* check for end of transmission group */
	if (transport->use_proactive_parity)
	{
		const guint32 odata_sqn = g_ntohl (STATE(skb)->pgm_data->data_sqn);
		guint32 tg_sqn_mask = 0xffffffff << transport->tg_sqn_shift;
		if (!((odata_sqn + 1) & ~tg_sqn_mask))
		{
			pgm_schedule_proactive_nak (transport, odata_sqn & tg_sqn_mask);
		}
	}

/* return data payload length sent */
	return (gssize)STATE(tsdu_length);
}

/* send PGM original data, callee owned memory.  if larger than maximum TPDU
 * size will be fragmented.
 *
 * on success, returns number of data bytes pushed into the transmit window and
 * attempted to send to the socket layer.  on non-blocking sockets, -1 is returned
 * if the packet sizes would exceed the current rate limit.
 */
gssize
pgm_transport_send (
	pgm_transport_t*	transport,
	gconstpointer		apdu,
	gsize			apdu_length,
	int			flags		/* MSG_DONTWAIT = rate non-blocking,
						   MSG_WAITALL  = packet blocking   */
	)
{
	g_return_val_if_fail (transport != NULL, -EINVAL);

/* reject on closed transport */
	if (!transport->is_open) {
		errno = ECONNRESET;
		return -1;
	}

/* pass on non-fragment calls */
	if (apdu_length < transport->max_tsdu) {
		return pgm_transport_send_one_copy (transport, apdu, apdu_length, flags);
	}
	g_return_val_if_fail (apdu != NULL, -EINVAL);
	g_return_val_if_fail (apdu_length <= (transport->txw_sqns * pgm_transport_max_tsdu (transport, TRUE)), -EMSGSIZE);

	g_assert( !(flags & MSG_WAITALL && !(flags & MSG_DONTWAIT)) );

	gsize bytes_sent	= 0;		/* counted at IP layer */
	guint packets_sent	= 0;		/* IP packets */
	gsize data_bytes_sent	= 0;

/* continue if blocked mid-apdu */
	if (transport->is_apdu_eagain) {
		goto retry_send;
	}

/* if non-blocking calculate total wire size and check rate limit */
	STATE(is_rate_limited) = FALSE;
	if (flags & MSG_DONTWAIT && flags & MSG_WAITALL)
	{
		const gsize header_length = pgm_transport_pkt_offset (TRUE);
		gsize tpdu_length = 0;
		gsize offset_	  = 0;
		do {
			gsize tsdu_length = MIN( pgm_transport_max_tsdu (transport, TRUE), apdu_length - offset_ );
			tpdu_length += transport->iphdr_len + header_length + tsdu_length;
			offset_ += tsdu_length;
		} while (offset_ < apdu_length);

/* calculation includes one iphdr length already */
		int result = pgm_rate_check (transport->rate_control, tpdu_length - transport->iphdr_len, flags);
		if (result == -1) {
			return (gssize)result;
		}

		STATE(is_rate_limited) = TRUE;
	}

	STATE(data_bytes_offset) = 0;

	g_static_rw_lock_writer_lock (&transport->txw_lock);
	STATE(first_sqn)	= pgm_txw_next_lead(transport->txw);

	do {
/* retrieve packet storage from transmit window */
		gsize header_length = pgm_transport_pkt_offset (TRUE);
		STATE(tsdu_length) = MIN( pgm_transport_max_tsdu (transport, TRUE), apdu_length - STATE(data_bytes_offset) );

		STATE(skb) = pgm_alloc_skb (transport->max_tpdu);
		STATE(skb)->transport = transport;
		STATE(skb)->tstamp = pgm_time_update_now();
		pgm_skb_put (STATE(skb), header_length +  STATE(tsdu_length));

		STATE(skb)->pgm_header  = (struct pgm_header*)STATE(skb)->data;
		STATE(skb)->pgm_data    = (struct pgm_data*)(STATE(skb)->pgm_header + 1);
		memcpy (STATE(skb)->pgm_header->pgm_gsi, &transport->tsi.gsi, sizeof(pgm_gsi_t));
		STATE(skb)->pgm_header->pgm_sport	= transport->tsi.sport;
		STATE(skb)->pgm_header->pgm_dport	= transport->dport;
		STATE(skb)->pgm_header->pgm_type	= PGM_ODATA;
		STATE(skb)->pgm_header->pgm_options	= PGM_OPT_PRESENT;
		STATE(skb)->pgm_header->pgm_tsdu_length = g_htons (STATE(tsdu_length));

/* ODATA */
		STATE(skb)->pgm_data->data_sqn		= g_htonl (pgm_txw_next_lead(transport->txw));
		STATE(skb)->pgm_data->data_trail	= g_htonl (pgm_txw_trail(transport->txw));

/* OPT_LENGTH */
		struct pgm_opt_length* opt_len		= (struct pgm_opt_length*)(STATE(skb)->pgm_data + 1);
		opt_len->opt_type			= PGM_OPT_LENGTH;
		opt_len->opt_length			= sizeof(struct pgm_opt_length);
		opt_len->opt_total_length		= g_htons (	sizeof(struct pgm_opt_length) +
									sizeof(struct pgm_opt_header) +
									sizeof(struct pgm_opt_fragment) );
/* OPT_FRAGMENT */
		struct pgm_opt_header* opt_header	= (struct pgm_opt_header*)(opt_len + 1);
		opt_header->opt_type			= PGM_OPT_FRAGMENT | PGM_OPT_END;
		opt_header->opt_length			= sizeof(struct pgm_opt_header) +
						  	  sizeof(struct pgm_opt_fragment);
		STATE(skb)->pgm_opt_fragment			= (struct pgm_opt_fragment*)(opt_header + 1);
		STATE(skb)->pgm_opt_fragment->opt_reserved	= 0;
		STATE(skb)->pgm_opt_fragment->opt_sqn		= g_htonl (STATE(first_sqn));
		STATE(skb)->pgm_opt_fragment->opt_frag_off	= g_htonl (STATE(data_bytes_offset));
		STATE(skb)->pgm_opt_fragment->opt_frag_len	= g_htonl (apdu_length);

/* TODO: the assembly checksum & copy routine is faster than memcpy & pgm_cksum on >= opteron hardware */
		STATE(skb)->pgm_header->pgm_checksum	= 0;
		const gsize pgm_header_len		= (guint8*)(STATE(skb)->pgm_opt_fragment + 1) - (guint8*)STATE(skb)->pgm_header;
		const guint32 unfolded_header		= pgm_csum_partial (STATE(skb)->pgm_header, pgm_header_len, 0);
		STATE(unfolded_odata)			= pgm_csum_partial_copy ((const guint8*)apdu + STATE(data_bytes_offset), STATE(skb)->pgm_opt_fragment + 1, STATE(tsdu_length), 0);
		STATE(skb)->pgm_header->pgm_checksum	= pgm_csum_fold (pgm_csum_block_add (unfolded_header, STATE(unfolded_odata), pgm_header_len));

/* add to transmit window */
		pgm_txw_add (transport->txw, STATE(skb));

		gssize sent;
retry_send:
		sent = pgm_sendto (transport,
					!STATE(is_rate_limited),	/* rate limit on blocking */
					FALSE,				/* regular socket */
					STATE(skb)->data,
					STATE(skb)->len,
					(flags & MSG_DONTWAIT && flags & MSG_WAITALL) ?
						flags & ~(MSG_DONTWAIT | MSG_WAITALL) :
						flags,
					(struct sockaddr*)&transport->send_gsr.gsr_group,
					pgm_sockaddr_len(&transport->send_gsr.gsr_group));
		if (sent < 0 && errno == EAGAIN)
		{
			transport->is_apdu_eagain = TRUE;
			goto blocked;
		}

/* save unfolded odata for retransmissions */
		*(guint32*)&STATE(skb)->cb = STATE(unfolded_odata);

		if ( sent == (gssize)STATE(skb)->len )
		{
			bytes_sent += STATE(skb)->len + transport->iphdr_len;	/* as counted at IP layer */
			packets_sent++;							/* IP packets */
			data_bytes_sent += STATE(tsdu_length);
		}

		STATE(data_bytes_offset) += STATE(tsdu_length);

/* check for end of transmission group */
		if (transport->use_proactive_parity)
		{
			const guint32 odata_sqn = g_ntohl (STATE(skb)->pgm_data->data_sqn);
			guint32 tg_sqn_mask = 0xffffffff << transport->tg_sqn_shift;
			if (!((odata_sqn + 1) & ~tg_sqn_mask))
			{
				pgm_schedule_proactive_nak (transport, odata_sqn & tg_sqn_mask);
			}
		}

	} while ( STATE(data_bytes_offset)  < apdu_length);
	g_assert( STATE(data_bytes_offset) == apdu_length );

/* release txw lock here in order to allow spms to lock mutex */
	g_static_rw_lock_writer_unlock (&transport->txw_lock);

	transport->is_apdu_eagain = FALSE;
	pgm_reset_heartbeat_spm (transport);

	transport->cumulative_stats[PGM_PC_SOURCE_BYTES_SENT]      += bytes_sent;
	transport->cumulative_stats[PGM_PC_SOURCE_DATA_MSGS_SENT]  += packets_sent;
	transport->cumulative_stats[PGM_PC_SOURCE_DATA_BYTES_SENT] += data_bytes_sent;

	return (gssize)apdu_length;

blocked:
	if (bytes_sent)
	{
		pgm_reset_heartbeat_spm (transport);

		transport->cumulative_stats[PGM_PC_SOURCE_BYTES_SENT]      += bytes_sent;
		transport->cumulative_stats[PGM_PC_SOURCE_DATA_MSGS_SENT]  += packets_sent;
		transport->cumulative_stats[PGM_PC_SOURCE_DATA_BYTES_SENT] += data_bytes_sent;
	}

	errno = EAGAIN;
	return -1;
}

/* send PGM original data, callee owned scatter/gather IO vector.  if larger than maximum TPDU
 * size will be fragmented.
 *
 * is_one_apdu = true:
 *
 *    ⎢ DATA₀ ⎢
 *    ⎢ DATA₁ ⎢ → pgm_transport_sendv() →  ⎢ ⋯ TSDU₁ TSDU₀ ⎢ → libc
 *    ⎢   ⋮   ⎢
 *
 * is_one_apdu = false:
 *
 *    ⎢ APDU₀ ⎢                            ⎢ ⋯ TSDU₁,₀ TSDU₀,₀ ⎢
 *    ⎢ APDU₁ ⎢ → pgm_transport_sendv() →  ⎢ ⋯ TSDU₁,₁ TSDU₀,₁ ⎢ → libc
 *    ⎢   ⋮   ⎢                            ⎢     ⋮       ⋮     ⎢
 *
 * on success, returns number of data bytes pushed into the transmit window and
 * attempted to send to the socket layer.  on non-blocking sockets, -1 is returned
 * if the packet sizes would exceed the current rate limit.
 */
gssize
pgm_transport_sendv (
	pgm_transport_t*	transport,
	const struct pgm_iovec*	vector,
	guint			count,		/* number of items in vector */
	int			flags,		/* MSG_DONTWAIT = rate non-blocking,
						   MSG_WAITALL  = packet blocking   */
	gboolean		is_one_apdu	/* true  = vector = apdu,
                                                   false = vector::iov_base = apdu */
	)
{
	g_return_val_if_fail (transport != NULL, -EINVAL);

/* reject on closed transport */
	if (!transport->is_open) {
		errno = ECONNRESET;
		return -1;
	}

/* pass on zero length as cannot count vector lengths */
	if (count == 0) {
		return pgm_transport_send_one_copy (transport, NULL, count, flags);
	}
	g_return_val_if_fail (vector != NULL, -EINVAL);

	g_assert( !(flags & MSG_WAITALL && !(flags & MSG_DONTWAIT)) );

	gsize bytes_sent	= 0;
	guint packets_sent	= 0;
	gsize data_bytes_sent	= 0;

/* continue if blocked mid-apdu */
	if (transport->is_apdu_eagain) {
		if (is_one_apdu) {
			if (STATE(apdu_length) < transport->max_tsdu) {
				return pgm_transport_send_onev (transport, vector, count, flags);
			} else {
				goto retry_one_apdu_send;
			}
		} else {
			goto retry_send;
		}
	}

/* calculate (total) APDU length */
	STATE(apdu_length)	= 0;
	for (unsigned i = 0; i < count; i++)
	{
#ifdef TRANSPORT_DEBUG
		if (vector[i].iov_len)
		{
			g_assert( vector[i].iov_base );
		}
#endif
		STATE(apdu_length) += vector[i].iov_len;
	}

/* pass on non-fragment calls */
	if (is_one_apdu && STATE(apdu_length) < transport->max_tsdu) {
		return pgm_transport_send_onev (transport, vector, count, flags);
	}
	g_return_val_if_fail (STATE(apdu_length) <= (transport->txw_sqns * pgm_transport_max_tsdu (transport, TRUE)), -EMSGSIZE);

/* if non-blocking calculate total wire size and check rate limit */
	STATE(is_rate_limited) = FALSE;
	if (flags & MSG_DONTWAIT && flags & MSG_WAITALL)
        {
		const gsize header_length = pgm_transport_pkt_offset (TRUE);
                gsize tpdu_length = 0;
		guint offset_	  = 0;
		do {
			gsize tsdu_length = MIN( pgm_transport_max_tsdu (transport, TRUE), STATE(apdu_length) - offset_ );
			tpdu_length += transport->iphdr_len + header_length + tsdu_length;
			offset_     += tsdu_length;
		} while (offset_ < STATE(apdu_length));

/* calculation includes one iphdr length already */
                int result = pgm_rate_check (transport->rate_control, tpdu_length - transport->iphdr_len, flags);
                if (result == -1) {
			return (gssize)result;
                }
		STATE(is_rate_limited) = TRUE;
        }

/* non-fragmented packets can be forwarded onto basic send() */
	if (!is_one_apdu)
	{
		for (STATE(data_pkt_offset) = 0; STATE(data_pkt_offset) < count; STATE(data_pkt_offset)++)
		{
			gssize sent;
retry_send:
			sent = pgm_transport_send (transport, vector[STATE(data_pkt_offset)].iov_base, vector[STATE(data_pkt_offset)].iov_len, flags);
			if (sent < 0 && errno == EAGAIN)
			{
				transport->is_apdu_eagain = TRUE;
				return -1;
			}

			if (sent == (gssize)vector[STATE(data_pkt_offset)].iov_len)
			{
				data_bytes_sent += vector[STATE(data_pkt_offset)].iov_len;
			}
		}

		transport->is_apdu_eagain = FALSE;
		return (gssize)data_bytes_sent;
	}

	STATE(data_bytes_offset)	= 0;
	STATE(vector_index)		= 0;
	STATE(vector_offset)		= 0;

	g_static_rw_lock_writer_lock (&transport->txw_lock);
	STATE(first_sqn)		= pgm_txw_next_lead(transport->txw);

	do {
/* retrieve packet storage from transmit window */
		gsize header_length = pgm_transport_pkt_offset (TRUE);
		STATE(tsdu_length) = MIN( pgm_transport_max_tsdu (transport, TRUE), STATE(apdu_length) - STATE(data_bytes_offset) );
		STATE(skb) = pgm_alloc_skb (transport->max_tpdu);
		STATE(skb)->transport = transport;
		STATE(skb)->tstamp = pgm_time_update_now();
		pgm_skb_put (STATE(skb), header_length + STATE(tsdu_length));

		STATE(skb)->pgm_header  = (struct pgm_header*)STATE(skb)->data;
		STATE(skb)->pgm_data    = (struct pgm_data*)(STATE(skb)->pgm_header + 1);
		memcpy (STATE(skb)->pgm_header->pgm_gsi, &transport->tsi.gsi, sizeof(pgm_gsi_t));
		STATE(skb)->pgm_header->pgm_sport	= transport->tsi.sport;
		STATE(skb)->pgm_header->pgm_dport	= transport->dport;
		STATE(skb)->pgm_header->pgm_type	= PGM_ODATA;
		STATE(skb)->pgm_header->pgm_options	= PGM_OPT_PRESENT;
		STATE(skb)->pgm_header->pgm_tsdu_length = g_htons (STATE(tsdu_length));

/* ODATA */
		STATE(skb)->pgm_data->data_sqn		= g_htonl (pgm_txw_next_lead(transport->txw));
		STATE(skb)->pgm_data->data_trail	= g_htonl (pgm_txw_trail(transport->txw));

/* OPT_LENGTH */
		struct pgm_opt_length* opt_len		= (struct pgm_opt_length*)(STATE(skb)->pgm_data + 1);
		opt_len->opt_type			= PGM_OPT_LENGTH;
		opt_len->opt_length			= sizeof(struct pgm_opt_length);
		opt_len->opt_total_length		= g_htons (	sizeof(struct pgm_opt_length) +
									sizeof(struct pgm_opt_header) +
									sizeof(struct pgm_opt_fragment) );
/* OPT_FRAGMENT */
		struct pgm_opt_header* opt_header	= (struct pgm_opt_header*)(opt_len + 1);
		opt_header->opt_type			= PGM_OPT_FRAGMENT | PGM_OPT_END;
		opt_header->opt_length			= sizeof(struct pgm_opt_header) +
							  sizeof(struct pgm_opt_fragment);
		STATE(skb)->pgm_opt_fragment			= (struct pgm_opt_fragment*)(opt_header + 1);
		STATE(skb)->pgm_opt_fragment->opt_reserved	= 0;
		STATE(skb)->pgm_opt_fragment->opt_sqn		= g_htonl (STATE(first_sqn));
		STATE(skb)->pgm_opt_fragment->opt_frag_off	= g_htonl (STATE(data_bytes_offset));
		STATE(skb)->pgm_opt_fragment->opt_frag_len	= g_htonl (STATE(apdu_length));

/* checksum & copy */
		STATE(skb)->pgm_header->pgm_checksum	= 0;
		const gsize pgm_header_len		= (guint8*)(STATE(skb)->pgm_opt_fragment + 1) - (guint8*)STATE(skb)->pgm_header;
		const guint32 unfolded_header		= pgm_csum_partial (STATE(skb)->pgm_header, pgm_header_len, 0);

/* iterate over one or more vector elements to perform scatter/gather checksum & copy
 *
 * STATE(vector_index)	- index into application scatter/gather vector
 * STATE(vector_offset) - current offset into current vector element
 * STATE(unfolded_odata)- checksum accumulator
 */
		const guint8* src	= (const guint8*)vector[STATE(vector_index)].iov_base + STATE(vector_offset);
		guint8* dst		= (guint8*)(STATE(skb)->pgm_opt_fragment + 1);
		gsize src_length	= vector[STATE(vector_index)].iov_len - STATE(vector_offset);
		gsize dst_length	= 0;
		gsize copy_length	= MIN( STATE(tsdu_length), src_length );
		STATE(unfolded_odata)	= pgm_csum_partial_copy (src, dst, copy_length, 0);

		for(;;)
		{
			if (copy_length == src_length)
			{
/* application packet complete */
				STATE(vector_index)++;
				STATE(vector_offset) = 0;
			}
			else
			{
/* data still remaining */
				STATE(vector_offset) += copy_length;
			}

			dst_length += copy_length;

			if (dst_length == STATE(tsdu_length))
			{
/* transport packet complete */
				break;
			}

			src		= (const guint8*)vector[STATE(vector_index)].iov_base + STATE(vector_offset);
			dst	       += copy_length;
			src_length	= vector[STATE(vector_index)].iov_len - STATE(vector_offset);
			copy_length	= MIN( STATE(tsdu_length) - dst_length, src_length );
			const guint32 unfolded_element = pgm_csum_partial_copy (src, dst, copy_length, 0);
			STATE(unfolded_odata) = pgm_csum_block_add (STATE(unfolded_odata), unfolded_element, dst_length);
		}

		STATE(skb)->pgm_header->pgm_checksum = pgm_csum_fold (pgm_csum_block_add (unfolded_header, STATE(unfolded_odata), pgm_header_len));

/* add to transmit window */
		pgm_txw_add (transport->txw, STATE(skb));

		gssize sent;
retry_one_apdu_send:
		sent = pgm_sendto (transport,
					!STATE(is_rate_limited),	/* rate limited on blocking */
					FALSE,				/* regular socket */
					STATE(skb)->data,
					STATE(skb)->len,
					(flags & MSG_DONTWAIT && flags & MSG_WAITALL) ?
						flags & ~(MSG_DONTWAIT | MSG_WAITALL) :
						flags,
					(struct sockaddr*)&transport->send_gsr.gsr_group,
					pgm_sockaddr_len(&transport->send_gsr.gsr_group));
		if (sent < 0 && errno == EAGAIN)
		{
			transport->is_apdu_eagain = TRUE;
			goto blocked;
		}

/* save unfolded odata for retransmissions */
		*(guint32*)&STATE(skb)->cb = STATE(unfolded_odata);

		if ( sent == (gssize)STATE(skb)->len )
		{
			bytes_sent += STATE(skb)->len + transport->iphdr_len;	/* as counted at IP layer */
			packets_sent++;							/* IP packets */
			data_bytes_sent += STATE(tsdu_length);
		}

		STATE(data_bytes_offset) += STATE(tsdu_length);

/* check for end of transmission group */
		if (transport->use_proactive_parity)
		{
			const guint32 odata_sqn = g_ntohl (STATE(skb)->pgm_data->data_sqn);
			guint32 tg_sqn_mask = 0xffffffff << transport->tg_sqn_shift;
			if (!((odata_sqn + 1) & ~tg_sqn_mask))
			{
				pgm_schedule_proactive_nak (transport, odata_sqn & tg_sqn_mask);
			}
		}

	} while ( STATE(data_bytes_offset)  < STATE(apdu_length) );
	g_assert( STATE(data_bytes_offset) == STATE(apdu_length) );

/* release txw lock here in order to allow spms to lock mutex */
	g_static_rw_lock_writer_unlock (&transport->txw_lock);

	transport->is_apdu_eagain = FALSE;
	pgm_reset_heartbeat_spm (transport);

	transport->cumulative_stats[PGM_PC_SOURCE_BYTES_SENT]      += bytes_sent;
	transport->cumulative_stats[PGM_PC_SOURCE_DATA_MSGS_SENT]  += packets_sent;
	transport->cumulative_stats[PGM_PC_SOURCE_DATA_BYTES_SENT] += data_bytes_sent;

	return (gssize)STATE(apdu_length);

blocked:
	if (bytes_sent)
	{
		pgm_reset_heartbeat_spm (transport);

		transport->cumulative_stats[PGM_PC_SOURCE_BYTES_SENT]      += bytes_sent;
		transport->cumulative_stats[PGM_PC_SOURCE_DATA_MSGS_SENT]  += packets_sent;
		transport->cumulative_stats[PGM_PC_SOURCE_DATA_BYTES_SENT] += data_bytes_sent;
	}

	errno = EAGAIN;
	return -1;
}

/* send PGM original data, transmit window owned scatter/gather IO vector.
 *
 *    ⎢ TSDU₀ ⎢
 *    ⎢ TSDU₁ ⎢ → pgm_transport_send_packetv() →  ⎢ ⋯ TSDU₁ TSDU₀ ⎢ → libc
 *    ⎢   ⋮   ⎢
 *
 * on success, returns number of data bytes pushed into the transmit window and
 * attempted to send to the socket layer.  on non-blocking sockets, -1 is returned
 * if the packet sizes would exceed the current rate limit.
 */
gssize
pgm_transport_send_skbv (
	pgm_transport_t*	transport,
	struct pgm_sk_buff_t*	vector,		/* packet */
	guint			count,
	int			flags,		/* MSG_DONTWAIT = rate non-blocking,
						   MSG_WAITALL  = packet blocking   */
	gboolean		is_one_apdu	/* true: vector = apdu,
                                                  false: vector::iov_base = apdu */
	)
{
	g_return_val_if_fail (transport != NULL, -EINVAL);

/* reject on closed transport */
	if (!transport->is_open) {
		errno = ECONNRESET;
		return -1;
	}

/* pass on zero length as cannot count vector lengths */
	if (count == 0) {
		return pgm_transport_send_one_copy (transport, NULL, count, flags);
	}
	g_return_val_if_fail (vector != NULL, -EINVAL);
	if (count == 1) {
		return pgm_transport_send_one (transport, vector, flags);
	}

	g_assert( !(flags & MSG_WAITALL && !(flags & MSG_DONTWAIT)) );

	gsize bytes_sent	= 0;
	guint packets_sent	= 0;
	gsize data_bytes_sent	= 0;

/* continue if blocked mid-apdu */
	if (transport->is_apdu_eagain) {
		goto retry_send;
	}

	STATE(is_rate_limited) = FALSE;
	if (flags & MSG_DONTWAIT && flags & MSG_WAITALL)
	{
		gsize total_tpdu_length = 0;
		for (guint i = 0; i < count; i++)
		{
			total_tpdu_length += transport->iphdr_len + pgm_transport_pkt_offset (is_one_apdu) + vector[i].len;
		}

/* calculation includes one iphdr length already */
		int result = pgm_rate_check (transport->rate_control, total_tpdu_length - transport->iphdr_len, flags);
		if (result == -1) {
			return (gssize)result;
		}

		STATE(is_rate_limited) = TRUE;
	}

	g_static_rw_lock_writer_lock (&transport->txw_lock);

	if (is_one_apdu)
	{
		STATE(apdu_length)	= 0;
		STATE(first_sqn)	= pgm_txw_next_lead(transport->txw);
		for (guint i = 0; i < count; i++)
		{
			g_return_val_if_fail (vector[i].len <= transport->max_tsdu_fragment, -EMSGSIZE);
			STATE(apdu_length) += vector[i].len;
		}
	}

	for (STATE(vector_index) = 0; STATE(vector_index) < count; STATE(vector_index)++)
	{
		STATE(tsdu_length) = vector[STATE(vector_index)].len;
		
		STATE(skb) = pgm_skb_get(&vector[STATE(vector_index)]);
		STATE(skb)->transport = transport;
		STATE(skb)->tstamp = pgm_time_update_now();
		STATE(skb)->data  = (guint8*)STATE(skb)->data - pgm_transport_pkt_offset(is_one_apdu);
		STATE(skb)->len  += pgm_transport_pkt_offset(is_one_apdu);

		STATE(skb)->pgm_header = (struct pgm_header*)STATE(skb)->data;
		STATE(skb)->pgm_data   = (struct pgm_data*)(STATE(skb)->pgm_header + 1);
		memcpy (STATE(skb)->pgm_header->pgm_gsi, &transport->tsi.gsi, sizeof(pgm_gsi_t));
		STATE(skb)->pgm_header->pgm_sport	= transport->tsi.sport;
		STATE(skb)->pgm_header->pgm_dport	= transport->dport;
		STATE(skb)->pgm_header->pgm_type	= PGM_ODATA;
		STATE(skb)->pgm_header->pgm_options	= is_one_apdu ? PGM_OPT_PRESENT : 0;
		STATE(skb)->pgm_header->pgm_tsdu_length = g_htons (STATE(tsdu_length));

/* ODATA */
		STATE(skb)->pgm_data->data_sqn		= g_htonl (pgm_txw_next_lead(transport->txw));
		STATE(skb)->pgm_data->data_trail	= g_htonl (pgm_txw_trail(transport->txw));

		gpointer dst = NULL;

		if (is_one_apdu)
		{
/* OPT_LENGTH */
			struct pgm_opt_length* opt_len		= (struct pgm_opt_length*)(STATE(skb)->pgm_data + 1);
			opt_len->opt_type			= PGM_OPT_LENGTH;
			opt_len->opt_length			= sizeof(struct pgm_opt_length);
			opt_len->opt_total_length		= g_htons (	sizeof(struct pgm_opt_length) +
										sizeof(struct pgm_opt_header) +
										sizeof(struct pgm_opt_fragment) );
/* OPT_FRAGMENT */
			struct pgm_opt_header* opt_header	= (struct pgm_opt_header*)(opt_len + 1);
			opt_header->opt_type			= PGM_OPT_FRAGMENT | PGM_OPT_END;
			opt_header->opt_length			= sizeof(struct pgm_opt_header) +
								  sizeof(struct pgm_opt_fragment);
			STATE(skb)->pgm_opt_fragment			= (struct pgm_opt_fragment*)(opt_header + 1);
			STATE(skb)->pgm_opt_fragment->opt_reserved	= 0;
			STATE(skb)->pgm_opt_fragment->opt_sqn		= g_htonl (STATE(first_sqn));
			STATE(skb)->pgm_opt_fragment->opt_frag_off	= g_htonl (STATE(data_bytes_offset));
			STATE(skb)->pgm_opt_fragment->opt_frag_len	= g_htonl (STATE(apdu_length));

			dst = STATE(skb)->pgm_opt_fragment + 1;
		}
		else
		{
			dst = STATE(skb)->pgm_data + 1;
		}

/* TODO: the assembly checksum & copy routine is faster than memcpy & pgm_cksum on >= opteron hardware */
		STATE(skb)->pgm_header->pgm_checksum	= 0;
		const gsize pgm_header_len		= (guint8*)dst - (guint8*)STATE(skb)->pgm_header;
		const guint32 unfolded_header		= pgm_csum_partial (STATE(skb)->pgm_header, pgm_header_len, 0);
		STATE(unfolded_odata)			= pgm_csum_partial ((guint8*)(STATE(skb)->pgm_opt_fragment + 1), STATE(tsdu_length), 0);
		STATE(skb)->pgm_header->pgm_checksum	= pgm_csum_fold (pgm_csum_block_add (unfolded_header, STATE(unfolded_odata), pgm_header_len));

/* add to transmit window */
		pgm_txw_add (transport->txw, STATE(skb));
		gssize sent;
retry_send:
		sent = pgm_sendto (transport,
					!STATE(is_rate_limited),	/* rate limited on blocking */
					FALSE,				/* regular socket */
					STATE(skb)->data,
					STATE(skb)->len,
					(flags & MSG_DONTWAIT && flags & MSG_WAITALL) ?
						flags & ~(MSG_DONTWAIT | MSG_WAITALL) :
						flags,
					(struct sockaddr*)&transport->send_gsr.gsr_group,
					pgm_sockaddr_len(&transport->send_gsr.gsr_group));
		if (sent < 0 && errno == EAGAIN)
		{
			transport->is_apdu_eagain = TRUE;
			goto blocked;
		}

/* save unfolded odata for retransmissions */
		*(guint32*)&STATE(skb)->cb = STATE(unfolded_odata);

		if (sent == (gssize)STATE(skb)->len)
		{
			bytes_sent += STATE(skb)->len + transport->iphdr_len;	/* as counted at IP layer */
			packets_sent++;							/* IP packets */
			data_bytes_sent += STATE(tsdu_length);
		}

		pgm_free_skb (STATE(skb));
		STATE(data_bytes_offset) += STATE(tsdu_length);

/* check for end of transmission group */
		if (transport->use_proactive_parity)
		{
			const guint32 odata_sqn = g_ntohl (STATE(skb)->pgm_data->data_sqn);
			guint32 tg_sqn_mask = 0xffffffff << transport->tg_sqn_shift;
			if (!((odata_sqn + 1) & ~tg_sqn_mask))
			{
				pgm_schedule_proactive_nak (transport, odata_sqn & tg_sqn_mask);
			}
		}

	}
#ifdef TRANSPORT_DEBUG
	if (is_one_apdu)
	{
		g_assert( STATE(data_bytes_offset) == STATE(apdu_length) );
	}
#endif

/* release txw lock here in order to allow spms to lock mutex */
	g_static_rw_lock_writer_unlock (&transport->txw_lock);

	transport->is_apdu_eagain = FALSE;
	pgm_reset_heartbeat_spm (transport);

	transport->cumulative_stats[PGM_PC_SOURCE_BYTES_SENT]      += bytes_sent;
	transport->cumulative_stats[PGM_PC_SOURCE_DATA_MSGS_SENT]  += packets_sent;
	transport->cumulative_stats[PGM_PC_SOURCE_DATA_BYTES_SENT] += data_bytes_sent;

	return (gssize)data_bytes_sent;

blocked:
	if (bytes_sent)
	{
		pgm_reset_heartbeat_spm (transport);

		transport->cumulative_stats[PGM_PC_SOURCE_BYTES_SENT]      += bytes_sent;
		transport->cumulative_stats[PGM_PC_SOURCE_DATA_MSGS_SENT]  += packets_sent;
		transport->cumulative_stats[PGM_PC_SOURCE_DATA_BYTES_SENT] += data_bytes_sent;
	}

	errno = EAGAIN;
	return -1;
}

/* cleanup resuming send state helper 
 */
#undef STATE

/* send repair packet.
 *
 * on success, 0 is returned.  on error, -1 is returned, and errno set
 * appropriately.
 */

static
int
send_rdata (
	pgm_transport_t*	transport,
	G_GNUC_UNUSED guint32	sequence_number,
	gpointer		data,
	gsize			len,
	gboolean		has_saved_partial_csum,
	guint32			partial_csum
	)
{
/* update previous odata/rdata contents */
	struct pgm_header* header = (struct pgm_header*)data;
	struct pgm_data* rdata    = (struct pgm_data*)(header + 1);
	header->pgm_type          = PGM_RDATA;

/* RDATA */
        rdata->data_trail       = g_htonl (pgm_txw_trail(transport->txw));

	guint32 unfolded_odata	= 0;
	if (has_saved_partial_csum)
	{
		unfolded_odata  = partial_csum;
	}
	header->pgm_sport	= transport->tsi.sport;
	header->pgm_dport	= transport->dport;

        header->pgm_checksum    = 0;

	gsize pgm_header_len	= len - g_ntohs(header->pgm_tsdu_length);
	guint32 unfolded_header = pgm_csum_partial (header, pgm_header_len, 0);
	if (!has_saved_partial_csum)
	{
		unfolded_odata	= pgm_csum_partial ((guint8*)data + pgm_header_len, g_ntohs(header->pgm_tsdu_length), 0);
	}
	header->pgm_checksum	= pgm_csum_fold (pgm_csum_block_add (unfolded_header, unfolded_odata, pgm_header_len));

	gssize sent = pgm_sendto (transport,
				TRUE,			/* rate limited */
				TRUE,			/* with router alert */
				header,
				len,
				MSG_CONFIRM,		/* not expecting a reply */
				(struct sockaddr*)&transport->send_gsr.gsr_group,
				pgm_sockaddr_len(&transport->send_gsr.gsr_group));

/* re-save unfolded payload for further retransmissions */
	if (has_saved_partial_csum)
	{
		*(guint32*)(void*)&header->pgm_sport = unfolded_odata;
	}

/* re-set spm timer: we are already in the timer thread, no need to prod timers
 */
	g_static_mutex_lock (&transport->mutex);
	transport->spm_heartbeat_state = 1;
	transport->next_heartbeat_spm = pgm_time_update_now() + transport->spm_heartbeat_interval[transport->spm_heartbeat_state++];
	g_static_mutex_unlock (&transport->mutex);

	if ( sent != (gssize)len )
	{
		return -1;
	}

	transport->cumulative_stats[PGM_PC_SOURCE_SELECTIVE_BYTES_RETRANSMITTED] += g_ntohs(header->pgm_tsdu_length);
	transport->cumulative_stats[PGM_PC_SOURCE_SELECTIVE_MSGS_RETRANSMITTED]++;	/* impossible to determine APDU count */
	transport->cumulative_stats[PGM_PC_SOURCE_BYTES_SENT] += len + transport->iphdr_len;

	return 0;
}

/* eof */

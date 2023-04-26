/*
 * Copyright (c) 2019-2022 Amazon.com, Inc. or its affiliates.
 * All rights reserved.
 *
 * This software is available to you under a choice of one of two
 * licenses.  You may choose to be licensed under the terms of the GNU
 * General Public License (GPL) Version 2, available from the file
 * COPYING in the main directory of this source tree, or the
 * BSD license below:
 *
 *     Redistribution and use in source and binary forms, with or
 *     without modification, are permitted provided that the following
 *     conditions are met:
 *
 *      - Redistributions of source code must retain the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer.
 *
 *      - Redistributions in binary form must reproduce the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer in the documentation and/or other materials
 *        provided with the distribution.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include <inttypes.h>
#include <stdlib.h>
#include <string.h>
#include "ofi.h"
#include <ofi_util.h>
#include <ofi_iov.h>
#include "efa.h"
#include "efa_av.h"
#include "efa_cq.h"
#include "rxr_msg.h"
#include "rxr_rma.h"
#include "rxr_pkt_cmd.h"
#include "rxr_pkt_type_base.h"
#include "rxr_atomic.h"
#include <infiniband/verbs.h>
#include "rxr_pkt_pool.h"
#include "rxr_tp.h"
#include "rxr_cntr.h"
#include "efa_rdm_srx.h"
#include "efa_rdm_cq.h"

void recv_rdma_with_imm_completion(struct efa_rdm_ep *ep, int32_t imm_data, uint64_t flags, struct rxr_pkt_entry *pkt_entry);

struct efa_ep_addr *efa_rdm_ep_raw_addr(struct efa_rdm_ep *ep)
{
	return &ep->base_ep.src_addr;
}

const char *efa_rdm_ep_raw_addr_str(struct efa_rdm_ep *ep, char *buf, size_t *buflen)
{
	return ofi_straddr(buf, buflen, FI_ADDR_EFA, efa_rdm_ep_raw_addr(ep));
}

/**
 * @brief return peer's raw address in #efa_ep_addr
 * 
 * @param[in] ep		end point 
 * @param[in] addr 		libfabric address
 * @returns
 * If peer exists, return peer's raw addrress as pointer to #efa_ep_addr;
 * Otherwise, return NULL
 * @relates efa_rdm_peer
 */
struct efa_ep_addr *efa_rdm_ep_get_peer_raw_addr(struct efa_rdm_ep *ep, fi_addr_t addr)
{
	struct efa_av *efa_av;
	struct efa_conn *efa_conn;

	efa_av = ep->base_ep.av;
	efa_conn = efa_av_addr_to_conn(efa_av, addr);
	return efa_conn ? efa_conn->ep_addr : NULL;
}

/**
 * @brief return peer's ahn
 *
 * @param[in] ep		end point
 * @param[in] addr 		libfabric address
 * @returns
 * If peer exists, return peer's ahn
 * Otherwise, return -1
 */
int32_t efa_rdm_ep_get_peer_ahn(struct efa_rdm_ep *ep, fi_addr_t addr)
{
	struct efa_av *efa_av;
	struct efa_conn *efa_conn;

	efa_av = ep->base_ep.av;
	efa_conn = efa_av_addr_to_conn(efa_av, addr);
	return efa_conn ? efa_conn->ah->ahn : -1;
}

/**
 * @brief return peer's raw address in a reable string
 * 
 * @param[in] ep		end point 
 * @param[in] addr 		libfabric address
 * @param[out] buf		a buffer tat to be used to store string
 * @param[in,out] buflen	length of `buf` as input. length of the string as output.
 * @relates efa_rdm_peer
 * @return a string with peer's raw address
 */
const char *efa_rdm_ep_get_peer_raw_addr_str(struct efa_rdm_ep *ep, fi_addr_t addr, char *buf, size_t *buflen)
{
	return ofi_straddr(buf, buflen, FI_ADDR_EFA, efa_rdm_ep_get_peer_raw_addr(ep, addr));
}

/**
 * @brief get pointer to efa_rdm_peer structure for a given libfabric address
 * 
 * @param[in]		ep		endpoint 
 * @param[in]		addr 		libfabric address
 * @returns if peer exists, return pointer to #efa_rdm_peer;
 *          otherwise, return NULL.
 */
struct efa_rdm_peer *efa_rdm_ep_get_peer(struct efa_rdm_ep *ep, fi_addr_t addr)
{
	struct util_av_entry *util_av_entry;
	struct efa_av_entry *av_entry;

	if (OFI_UNLIKELY(addr == FI_ADDR_NOTAVAIL))
		return NULL;

	util_av_entry = ofi_bufpool_get_ibuf(ep->base_ep.util_ep.av->av_entry_pool,
	                                     addr);
	av_entry = (struct efa_av_entry *)util_av_entry->data;
	return av_entry->conn.ep_addr ? &av_entry->conn.rdm_peer : NULL;
}

/**
 * @brief allocate an rxe for an operation
 *
 * @param ep[in]	end point
 * @param addr[in]	fi address of the sender/requester.
 * @param op[in]	operation type (ofi_op_msg/ofi_op_tagged/ofi_op_read/ofi_op_write/ofi_op_atomic_xxx)
 * @return		if allocation succeeded, return pointer to rxe
 * 			if allocation failed, return NULL
 */
struct efa_rdm_ope *efa_rdm_ep_alloc_rxe(struct efa_rdm_ep *ep, fi_addr_t addr, uint32_t op)
{
	struct efa_rdm_ope *rxe;

	rxe = ofi_buf_alloc(ep->ope_pool);
	if (OFI_UNLIKELY(!rxe)) {
		EFA_WARN(FI_LOG_EP_CTRL, "RX entries exhausted\n");
		return NULL;
	}
	memset(rxe, 0, sizeof(struct efa_rdm_ope));

	rxe->ep = ep;
	dlist_insert_tail(&rxe->ep_entry, &ep->rxe_list);
	rxe->type = EFA_RDM_RXE;
	rxe->rx_id = ofi_buf_index(rxe);
	dlist_init(&rxe->queued_pkts);

	rxe->state = EFA_RDM_RXE_INIT;
	rxe->addr = addr;
	if (addr != FI_ADDR_UNSPEC) {
		rxe->peer = efa_rdm_ep_get_peer(ep, addr);
		assert(rxe->peer);
		dlist_insert_tail(&rxe->peer_entry, &rxe->peer->rxe_list);
	} else {
		/*
		 * If msg->addr is not provided, rxe->peer will be set
		 * after it is matched with a message.
		 */
		assert(op == ofi_op_msg || op == ofi_op_tagged);
		rxe->peer = NULL;
	}

	rxe->bytes_runt = 0;
	rxe->bytes_received_via_mulreq = 0;
	rxe->cuda_copy_method = EFA_RDM_CUDA_COPY_UNSPEC;
	rxe->efa_outstanding_tx_ops = 0;
	rxe->op = op;

	rxe->peer_rxe.addr = addr;
	/* This field points to the fid_peer_srx struct that's part of the peer API
	*  We always set it to the EFA provider's SRX here. For SHM messages, we will set
	*  this to SHM provider's SRX in the get_msg/get_tag function call
	*/
	rxe->peer_rxe.srx = &ep->peer_srx;

	dlist_init(&rxe->entry);
	switch (op) {
	case ofi_op_tagged:
		rxe->cq_entry.flags = (FI_RECV | FI_MSG | FI_TAGGED);
		break;
	case ofi_op_msg:
		rxe->cq_entry.flags = (FI_RECV | FI_MSG);
		break;
	case ofi_op_read_rsp:
		rxe->cq_entry.flags = (FI_REMOTE_READ | FI_RMA);
		break;
	case ofi_op_write:
		rxe->cq_entry.flags = (FI_REMOTE_WRITE | FI_RMA);
		break;
	case ofi_op_atomic:
		rxe->cq_entry.flags = (FI_REMOTE_WRITE | FI_ATOMIC);
		break;
	case ofi_op_atomic_fetch:
	case ofi_op_atomic_compare:
		rxe->cq_entry.flags = (FI_REMOTE_READ | FI_ATOMIC);
		break;
	default:
		EFA_WARN(FI_LOG_EP_CTRL,
			"Unknown operation while %s\n", __func__);
		assert(0 && "Unknown operation");
	}

	return rxe;
}

/**
 * @brief post user provided receiving buffer to the device.
 *
 * The user receive buffer was converted to an RX packet, then posted to the device.
 *
 * @param[in]	ep		endpint
 * @param[in]	rxe	rxe that contain user buffer information
 * @param[in]	flags		user supplied flags passed to fi_recv
 */
int efa_rdm_ep_post_user_recv_buf(struct efa_rdm_ep *ep, struct efa_rdm_ope *rxe, uint64_t flags)
{
	struct rxr_pkt_entry *pkt_entry;
	struct efa_mr *mr;
	int err;

	assert(rxe->iov_count == 1);
	assert(rxe->iov[0].iov_len >= ep->msg_prefix_size);
	pkt_entry = (struct rxr_pkt_entry *)rxe->iov[0].iov_base;
	assert(pkt_entry);

	/*
	 * The ownership of the prefix buffer lies with the application, do not
	 * put it on the dbg list for cleanup during shutdown or poison it. The
	 * provider loses jurisdiction over it soon after writing the rx
	 * completion.
	 */
	dlist_init(&pkt_entry->entry);
	mr = (struct efa_mr *)rxe->desc[0];
	pkt_entry->mr = &mr->mr_fid;
	pkt_entry->alloc_type = RXR_PKT_FROM_USER_BUFFER;
	pkt_entry->flags = RXR_PKT_ENTRY_IN_USE;
	pkt_entry->next = NULL;
	/*
	 * The actual receiving buffer size (pkt_size) is
	 *    rxe->total_len - sizeof(struct rxr_pkt_entry)
	 * because the first part of user buffer was used to
	 * construct pkt_entry. The actual receiving buffer
	 * posted to device starts from pkt_entry->wiredata.
	 */
	pkt_entry->pkt_size = rxe->iov[0].iov_len - sizeof(struct rxr_pkt_entry);

	pkt_entry->ope = rxe;
	rxe->state = EFA_RDM_RXE_MATCHED;

	err = rxr_pkt_entry_recv(ep, pkt_entry, rxe->desc, flags);
	if (OFI_UNLIKELY(err)) {
		rxr_pkt_entry_release_rx(ep, pkt_entry);
		EFA_WARN(FI_LOG_EP_CTRL,
			"failed to post user supplied buffer %d (%s)\n", -err,
			fi_strerror(-err));
		return err;
	}

	ep->efa_rx_pkts_posted++;
	return 0;
}

/**
 * @brief post an internal receive buffer to lower endpoint
 *
 * The buffer was posted as undirected recv, (address was set to FI_ADDR_UNSPEC)
 *
 * @param[in]	ep		endpoint
 * @param[in]	flags		flags passed to lower provider, can have FI_MORE
 * @return	On success, return 0
 * 		On failure, return a negative error code.
 */
int efa_rdm_ep_post_internal_rx_pkt(struct efa_rdm_ep *ep, uint64_t flags)
{
	void *desc;
	struct rxr_pkt_entry *rx_pkt_entry = NULL;
	int ret = 0;

	rx_pkt_entry = rxr_pkt_entry_alloc(ep, ep->efa_rx_pkt_pool, RXR_PKT_FROM_EFA_RX_POOL);

	if (OFI_UNLIKELY(!rx_pkt_entry)) {
		EFA_WARN(FI_LOG_EP_CTRL,
			"Unable to allocate rx_pkt_entry\n");
		return -FI_ENOMEM;
	}

	rx_pkt_entry->ope = NULL;

#if ENABLE_DEBUG
	dlist_insert_tail(&rx_pkt_entry->dbg_entry,
				  &ep->rx_posted_buf_list);
#endif
	desc = fi_mr_desc(rx_pkt_entry->mr);
	ret = rxr_pkt_entry_recv(ep, rx_pkt_entry, &desc, flags);
	if (OFI_UNLIKELY(ret)) {
		rxr_pkt_entry_release_rx(ep, rx_pkt_entry);
		EFA_WARN(FI_LOG_EP_CTRL,
			"failed to post buf %d (%s)\n", -ret,
			fi_strerror(-ret));
		return ret;
	}
	ep->efa_rx_pkts_posted++;

	return 0;
}

/**
 * @brief bulk post internal receive buffer(s) to device
 *
 * When posting multiple buffers, this function will use
 * FI_MORE flag to achieve better performance.
 *
 * @param[in]	ep		endpint
 * @param[in]	nrecv		number of receive buffers to post
 * @return	On success, return 0
 * 		On failure, return negative libfabric error code
 */
static inline
ssize_t efa_rdm_ep_bulk_post_internal_rx_pkts(struct efa_rdm_ep *ep, int nrecv)
{
	int i;
	ssize_t err;
	uint64_t flags;

	flags = FI_MORE;
	for (i = 0; i < nrecv; ++i) {
		if (i == nrecv - 1)
			flags = 0;

		err = efa_rdm_ep_post_internal_rx_pkt(ep, flags);
		if (OFI_UNLIKELY(err))
			return err;
	}

	return 0;
}

/* create a new txe */
struct efa_rdm_ope *efa_rdm_ep_alloc_txe(struct efa_rdm_ep *efa_rdm_ep,
					   const struct fi_msg *msg,
					   uint32_t op,
					   uint64_t tag,
					   uint64_t flags)
{
	struct efa_rdm_ope *txe;

	txe = ofi_buf_alloc(efa_rdm_ep->ope_pool);
	if (OFI_UNLIKELY(!txe)) {
		EFA_DBG(FI_LOG_EP_CTRL, "TX entries exhausted.\n");
		return NULL;
	}

	efa_rdm_txe_construct(txe, efa_rdm_ep, msg, op, flags);
	if (op == ofi_op_tagged) {
		txe->cq_entry.tag = tag;
		txe->tag = tag;
	}

	dlist_insert_tail(&txe->ep_entry, &efa_rdm_ep->txe_list);
	return txe;
}


/* Generic send */
static void efa_rdm_ep_free_res(struct efa_rdm_ep *efa_rdm_ep)
{
	struct dlist_entry *entry, *tmp;
	struct efa_rdm_ope *rxe;
	struct efa_rdm_ope *txe;
	struct efa_rdm_ope *ope;
#if ENABLE_DEBUG
	struct rxr_pkt_entry *pkt_entry;
#endif

	dlist_foreach_safe(&efa_rdm_ep->rx_unexp_list, entry, tmp) {
		rxe = container_of(entry, struct efa_rdm_ope, entry);
		EFA_WARN(FI_LOG_EP_CTRL,
			"Closing ep with unmatched unexpected rxe: %p pkt_entry %p\n",
			rxe, rxe->unexp_pkt);
		/* rxe for peer srx does not allocate unexp_pkt */
		if (!(rxe->rxr_flags & EFA_RDM_RXE_FOR_PEER_SRX))
			rxr_pkt_entry_release_rx(efa_rdm_ep, rxe->unexp_pkt);
		efa_rdm_rxe_release(rxe);
	}

	dlist_foreach_safe(&efa_rdm_ep->rx_unexp_tagged_list, entry, tmp) {
		rxe = container_of(entry, struct efa_rdm_ope, entry);
		EFA_WARN(FI_LOG_EP_CTRL,
			"Closing ep with unmatched unexpected tagged rxe: %p pkt_entry %p\n",
			rxe, rxe->unexp_pkt);
		/* rxe for peer srx does not allocate unexp_pkt */
		if (!(rxe->rxr_flags & EFA_RDM_RXE_FOR_PEER_SRX))
			rxr_pkt_entry_release_rx(efa_rdm_ep, rxe->unexp_pkt);
		efa_rdm_rxe_release(rxe);
	}

	dlist_foreach_safe(&efa_rdm_ep->ope_queued_rnr_list, entry, tmp) {
		txe = container_of(entry, struct efa_rdm_ope,
					queued_rnr_entry);
		EFA_WARN(FI_LOG_EP_CTRL,
			"Closing ep with queued rnr txe: %p\n",
			txe);
		efa_rdm_txe_release(txe);
	}

	dlist_foreach_safe(&efa_rdm_ep->ope_queued_ctrl_list, entry, tmp) {
		ope = container_of(entry, struct efa_rdm_ope,
					queued_ctrl_entry);
		EFA_WARN(FI_LOG_EP_CTRL,
			"Closing ep with queued ctrl ope: %p\n",
			ope);
		if (ope->type == EFA_RDM_TXE) {
			efa_rdm_txe_release(ope);
		} else {
			assert(ope->type == EFA_RDM_RXE);
			efa_rdm_rxe_release(ope);
		}
	}

#if ENABLE_DEBUG
	dlist_foreach_safe(&efa_rdm_ep->rx_posted_buf_list, entry, tmp) {
		pkt_entry = container_of(entry, struct rxr_pkt_entry, dbg_entry);
		rxr_pkt_entry_release_rx(efa_rdm_ep, pkt_entry);
	}

	dlist_foreach_safe(&efa_rdm_ep->rx_pkt_list, entry, tmp) {
		pkt_entry = container_of(entry, struct rxr_pkt_entry, dbg_entry);
		EFA_WARN(FI_LOG_EP_CTRL,
			"Closing ep with unreleased RX pkt_entry: %p\n",
			pkt_entry);
		rxr_pkt_entry_release_rx(efa_rdm_ep, pkt_entry);
	}

	dlist_foreach_safe(&efa_rdm_ep->tx_pkt_list, entry, tmp) {
		pkt_entry = container_of(entry, struct rxr_pkt_entry, dbg_entry);
		EFA_WARN(FI_LOG_EP_CTRL,
			"Closing ep with unreleased TX pkt_entry: %p\n",
			pkt_entry);
		rxr_pkt_entry_release_tx(efa_rdm_ep, pkt_entry);
	}
#endif

	dlist_foreach_safe(&efa_rdm_ep->rxe_list, entry, tmp) {
		rxe = container_of(entry, struct efa_rdm_ope,
					ep_entry);
		if (!(rxe->rxr_flags & EFA_RDM_RXE_MULTI_RECV_POSTED))
			EFA_WARN(FI_LOG_EP_CTRL,
				"Closing ep with unreleased rxe\n");
		efa_rdm_rxe_release(rxe);
	}

	dlist_foreach_safe(&efa_rdm_ep->txe_list, entry, tmp) {
		txe = container_of(entry, struct efa_rdm_ope,
					ep_entry);
		EFA_WARN(FI_LOG_EP_CTRL,
			"Closing ep with unreleased txe: %p\n",
			txe);
		efa_rdm_txe_release(txe);
	}

	if (efa_rdm_ep->ope_pool)
		ofi_bufpool_destroy(efa_rdm_ep->ope_pool);

	if (efa_rdm_ep->map_entry_pool)
		ofi_bufpool_destroy(efa_rdm_ep->map_entry_pool);

	if (efa_rdm_ep->rx_readcopy_pkt_pool) {
		EFA_INFO(FI_LOG_EP_CTRL, "current usage of read copy packet pool is %d\n",
			efa_rdm_ep->rx_readcopy_pkt_pool_used);
		EFA_INFO(FI_LOG_EP_CTRL, "maximum usage of read copy packet pool is %d\n",
			efa_rdm_ep->rx_readcopy_pkt_pool_max_used);
		assert(!efa_rdm_ep->rx_readcopy_pkt_pool_used);
		rxr_pkt_pool_destroy(efa_rdm_ep->rx_readcopy_pkt_pool);
	}

	if (efa_rdm_ep->rx_ooo_pkt_pool)
		rxr_pkt_pool_destroy(efa_rdm_ep->rx_ooo_pkt_pool);

	if (efa_rdm_ep->rx_unexp_pkt_pool)
		rxr_pkt_pool_destroy(efa_rdm_ep->rx_unexp_pkt_pool);

	if (efa_rdm_ep->efa_rx_pkt_pool)
		rxr_pkt_pool_destroy(efa_rdm_ep->efa_rx_pkt_pool);

	if (efa_rdm_ep->efa_tx_pkt_pool)
		rxr_pkt_pool_destroy(efa_rdm_ep->efa_tx_pkt_pool);
}

/*
 * @brief determine whether an endpoint has unfinished send
 *
 * Unfinished send includes queued ctrl packets, queued
 * RNR packets and inflight TX packets.
 *
 * @param[in]	efa_rdm_ep	endpoint
 * @return	a boolean
 */
static
bool efa_rdm_ep_has_unfinished_send(struct efa_rdm_ep *efa_rdm_ep)
{
	return !dlist_empty(&efa_rdm_ep->ope_queued_rnr_list) ||
	       !dlist_empty(&efa_rdm_ep->ope_queued_ctrl_list) ||
	       (efa_rdm_ep->efa_outstanding_tx_ops > 0);
}

/*
 * @brief wait for send to finish
 *
 * Wait for queued packet to be sent, and inflight send to
 * complete.
 *
 * @param[in]	efa_rdm_ep		endpoint
 * @return 	no return
 */
static inline
void efa_rdm_ep_wait_send(struct efa_rdm_ep *efa_rdm_ep)
{
	ofi_mutex_lock(&efa_rdm_ep->base_ep.util_ep.lock);

	while (efa_rdm_ep_has_unfinished_send(efa_rdm_ep)) {
		efa_rdm_ep_progress_internal(efa_rdm_ep);
	}

	ofi_mutex_unlock(&efa_rdm_ep->base_ep.util_ep.lock);
}

static int efa_rdm_ep_close(struct fid *fid)
{
	int ret, retv = 0;
	struct efa_rdm_ep *efa_rdm_ep;

	efa_rdm_ep = container_of(fid, struct efa_rdm_ep, base_ep.util_ep.ep_fid.fid);

	efa_rdm_ep_wait_send(efa_rdm_ep);

	ret = efa_base_ep_destruct(&efa_rdm_ep->base_ep);
	if (ret) {
		EFA_WARN(FI_LOG_EP_CTRL, "Unable to close base endpoint\n");
		retv = ret;
	}

	ret = -ibv_destroy_cq(ibv_cq_ex_to_cq(efa_rdm_ep->ibv_cq_ex));
	if (ret) {
		EFA_WARN(FI_LOG_EP_CTRL, "Unable to close ibv_cq_ex\n");
		retv = ret;
	}

	if (efa_rdm_ep->shm_ep) {
		ret = fi_close(&efa_rdm_ep->shm_ep->fid);
		if (ret) {
			EFA_WARN(FI_LOG_EP_CTRL, "Unable to close shm EP\n");
			retv = ret;
		}
	}

	efa_rdm_ep_free_res(efa_rdm_ep);
	free(efa_rdm_ep);
	return retv;
}

static int efa_rdm_ep_bind(struct fid *ep_fid, struct fid *bfid, uint64_t flags)
{
	struct efa_rdm_ep *efa_rdm_ep =
		container_of(ep_fid, struct efa_rdm_ep, base_ep.util_ep.ep_fid.fid);
	struct efa_rdm_cq *cq;
	struct efa_av *av;
	struct util_cntr *cntr;
	struct util_eq *eq;
	int ret = 0;

	switch (bfid->fclass) {
	case FI_CLASS_AV:
		av = container_of(bfid, struct efa_av, util_av.av_fid.fid);
		/* Bind util provider endpoint and av */
		ret = ofi_ep_bind_av(&efa_rdm_ep->base_ep.util_ep, &av->util_av);
		if (ret)
			return ret;

		ret = efa_base_ep_bind_av(&efa_rdm_ep->base_ep, av);
		if (ret)
			return ret;

		/* Bind shm provider endpoint & shm av */
		if (efa_rdm_ep->shm_ep) {
			assert(av->shm_rdm_av);
			ret = fi_ep_bind(efa_rdm_ep->shm_ep, &av->shm_rdm_av->fid, flags);
			if (ret)
				return ret;
		}
		break;
	case FI_CLASS_CQ:
		cq = container_of(bfid, struct efa_rdm_cq, util_cq.cq_fid.fid);

		ret = ofi_ep_bind_cq(&efa_rdm_ep->base_ep.util_ep, &cq->util_cq, flags);
		if (ret)
			return ret;

		if (cq->shm_cq) {
			/* Bind ep with shm provider's cq */
			ret = fi_ep_bind(efa_rdm_ep->shm_ep, &cq->shm_cq->fid, flags);
			if (ret)
				return ret;
		}
		break;
	case FI_CLASS_CNTR:
		cntr = container_of(bfid, struct util_cntr, cntr_fid.fid);

		ret = ofi_ep_bind_cntr(&efa_rdm_ep->base_ep.util_ep, cntr, flags);
		if (ret)
			return ret;
		break;
	case FI_CLASS_EQ:
		eq = container_of(bfid, struct util_eq, eq_fid.fid);

		ret = ofi_ep_bind_eq(&efa_rdm_ep->base_ep.util_ep, eq);
		if (ret)
			return ret;
		break;
	default:
		EFA_WARN(FI_LOG_EP_CTRL, "invalid fid class\n");
		ret = -FI_EINVAL;
		break;
	}
	return ret;
}

/*
 * For a given peer, trigger a handshake packet and determine if both peers
 * support rdma read.
 *
 * @param[in,out]	ep	efa_rdm_ep
 * @param[in]		addr	remote address
 * @param[in,out]	peer	remote peer
 * @return 		1 if supported, 0 if not, negative errno on error
 */
int efa_rdm_ep_determine_rdma_read_support(struct efa_rdm_ep *ep, fi_addr_t addr,
				       struct efa_rdm_peer *peer)
{
	int ret;

	if (!peer->is_local) {
		ret = rxr_pkt_trigger_handshake(ep, addr, peer);
		if (OFI_UNLIKELY(ret))
			return ret;

		if (!(peer->flags & EFA_RDM_PEER_HANDSHAKE_RECEIVED))
			return -FI_EAGAIN;
	}

	if (!efa_both_support_rdma_read(ep, peer))
		return 0;

	return 1;
}

/**
 * @brief determine if both peers support rdma write.
 *
 * If no prior communication with the given peer, and we support RDMA WRITE
 * locally, then trigger a handshake packet and determine if both peers
 * support rdma write.
 *
 * @param[in,out]	ep	efa_rdm_ep
 * @param[in]		addr	remote address
 * @param[in,out]	peer	remote peer
 * @return 		1 if supported, 0 if not, negative errno on error
 */
int efa_rdm_ep_determine_rdma_write_support(struct efa_rdm_ep *ep, fi_addr_t addr,
					struct efa_rdm_peer *peer)
{
	int ret;

	/* no need to trigger handshake if we don't support RDMA WRITE locally */
	if (!efa_rdm_ep_support_rdma_write(ep))
		return false;

	if (!peer->is_local) {
		ret = rxr_pkt_trigger_handshake(ep, addr, peer);
		if (OFI_UNLIKELY(ret))
			return ret;

		if (!(peer->flags & EFA_RDM_PEER_HANDSHAKE_RECEIVED))
			return -FI_EAGAIN;
	}

	if (!efa_both_support_rdma_write(ep, peer))
		return 0;

	return 1;
}

static
void efa_rdm_ep_set_extra_info(struct efa_rdm_ep *ep)
{
	memset(ep->extra_info, 0, sizeof(ep->extra_info));

	/* RDMA read is an extra feature defined in protocol version 4 (the base version) */
	if (efa_rdm_ep_support_rdma_read(ep))
		ep->extra_info[0] |= RXR_EXTRA_FEATURE_RDMA_READ;

	/* RDMA write is defined in protocol v4, and introduced in libfabric 1.18.0 */
	if (efa_rdm_ep_support_rdma_write(ep))
		ep->extra_info[0] |= RXR_EXTRA_FEATURE_RDMA_WRITE;

	ep->extra_info[0] |= RXR_EXTRA_FEATURE_DELIVERY_COMPLETE;

	if (ep->use_zcpy_rx) {
		/*
		 * zero copy receive requires the packet header length remains
		 * constant, so the application receive buffer is match with
		 * incoming application data.
		 */
		ep->extra_info[0] |= RXR_EXTRA_REQUEST_CONSTANT_HEADER_LENGTH;
	}

	ep->extra_info[0] |= RXR_EXTRA_REQUEST_CONNID_HEADER;

	ep->extra_info[0] |= RXR_EXTRA_FEATURE_RUNT;
}

/**
 * @brief set the "use_shm_for_tx" field of efa_rdm_ep
 * The field is set based on various factors, including
 * environment variables, user hints, user's fi_setopt()
 * calls.
 * This function should be called during call to fi_enable(),
 * after user called fi_setopt().
 *
 * @param[in,out]	ep	endpoint to set the field
 */
static
void efa_rdm_ep_set_use_shm_for_tx(struct efa_rdm_ep *ep)
{
	if (!efa_rdm_ep_domain(ep)->shm_domain) {
		ep->use_shm_for_tx = false;
		return;
	}

	assert(ep->user_info);

	/* App provided hints supercede environmental variables.
	 *
	 * Using the shm provider comes with some overheads, so avoid
	 * initializing the provider if the app provides a hint that it does not
	 * require node-local communication. We can still loopback over the EFA
	 * device in cases where the app violates the hint and continues
	 * communicating with node-local peers.
	 *
	 * aws-ofi-nccl relies on this feature.
	 */
	if ((ep->user_info->caps & FI_REMOTE_COMM)
	    /* but not local communication */
	    && !(ep->user_info->caps & FI_LOCAL_COMM)) {
		ep->use_shm_for_tx = false;
		return;
	}

	/* TODO Update shm provider to support HMEM atomic */
	if ((ep->user_info->caps) & FI_ATOMIC && (ep->user_info->caps & FI_HMEM)) {
		ep->use_shm_for_tx = false;
		return;
	}

	/*
	 * shm provider must make cuda calls to transfer cuda memory.
	 * if cuda call is not allowed, we cannot use shm for transfer.
	 *
	 * Note that the other two hmem interfaces supported by EFA,
	 * AWS Neuron and Habana Synapse, have no SHM provider
	 * support anyways, so disabling SHM will not impact them.
	 */
	if ((ep->user_info->caps & FI_HMEM)
	    && hmem_ops[FI_HMEM_CUDA].initialized
	    && !ep->cuda_api_permitted) {
		ep->use_shm_for_tx = false;
		return;
	}

	ep->use_shm_for_tx = rxr_env.enable_shm_transfer;
	return;
}

static
int efa_rdm_ep_create_base_ep_ibv_qp(struct efa_rdm_ep *ep)
{
	struct ibv_qp_init_attr_ex attr_ex = { 0 };

	attr_ex.cap.max_send_wr = ep->base_ep.domain->device->rdm_info->tx_attr->size;
	attr_ex.cap.max_send_sge = ep->base_ep.domain->device->rdm_info->tx_attr->iov_limit;
	attr_ex.send_cq = ibv_cq_ex_to_cq(ep->ibv_cq_ex);

	attr_ex.cap.max_recv_wr = ep->base_ep.domain->device->rdm_info->rx_attr->size;
	attr_ex.cap.max_recv_sge = ep->base_ep.domain->device->rdm_info->rx_attr->iov_limit;
	attr_ex.recv_cq = ibv_cq_ex_to_cq(ep->ibv_cq_ex);

	attr_ex.cap.max_inline_data = ep->base_ep.domain->device->efa_attr.inline_buf_size;

	attr_ex.qp_type = IBV_QPT_DRIVER;
	attr_ex.comp_mask = IBV_QP_INIT_ATTR_PD | IBV_QP_INIT_ATTR_SEND_OPS_FLAGS;
	attr_ex.send_ops_flags = IBV_QP_EX_WITH_SEND;
	if (efa_device_support_rdma_read())
		attr_ex.send_ops_flags |= IBV_QP_EX_WITH_RDMA_READ;
	if (efa_device_support_rdma_write()) {
		attr_ex.send_ops_flags |= IBV_QP_EX_WITH_RDMA_WRITE;
		attr_ex.send_ops_flags |= IBV_QP_EX_WITH_RDMA_WRITE_WITH_IMM;
	}
	attr_ex.pd = efa_rdm_ep_domain(ep)->ibv_pd;

	attr_ex.qp_context = ep;
	attr_ex.sq_sig_all = 1;

	return efa_base_ep_create_qp(&ep->base_ep, &attr_ex);
}

static int efa_rdm_ep_ctrl(struct fid *fid, int command, void *arg)
{
	struct efa_rdm_ep *ep;
	char shm_ep_name[EFA_SHM_NAME_MAX], ep_addr_str[OFI_ADDRSTRLEN];
	size_t shm_ep_name_len, ep_addr_strlen;
	int ret = 0;

	switch (command) {
	case FI_ENABLE:
		ep = container_of(fid, struct efa_rdm_ep, base_ep.util_ep.ep_fid.fid);
		ret = efa_base_ep_enable(&ep->base_ep);
		if (ret)
			return ret;

		ofi_mutex_lock(&ep->base_ep.util_ep.lock);

		efa_rdm_ep_set_extra_info(ep);

		ep_addr_strlen = sizeof(ep_addr_str);
		efa_rdm_ep_raw_addr_str(ep, ep_addr_str, &ep_addr_strlen);
		EFA_WARN(FI_LOG_EP_CTRL, "libfabric %s efa endpoint created! address: %s\n",
			fi_tostr("1", FI_TYPE_VERSION), ep_addr_str);

		efa_rdm_ep_set_use_shm_for_tx(ep);

		/* Enable shm provider endpoint & post recv buff.
		 * Once core ep enabled, 18 bytes efa_addr (16 bytes raw + 2 bytes qpn) is set.
		 * We convert the address to 'gid_qpn' format, and set it as shm ep name, so
		 * that shm ep can create shared memory region with it when enabling.
		 * In this way, each peer is able to open and map to other local peers'
		 * shared memory region.
		 */
		if (ep->shm_ep) {
			shm_ep_name_len = EFA_SHM_NAME_MAX;
			ret = efa_shm_ep_name_construct(shm_ep_name, &shm_ep_name_len, &ep->base_ep.src_addr);
			if (ret < 0)
				goto out;
			fi_setname(&ep->shm_ep->fid, shm_ep_name, shm_ep_name_len);

			/* Bind srx to shm ep */
			ret = fi_ep_bind(ep->shm_ep, &ep->peer_srx.ep_fid.fid, 0);
			if (ret)
				goto out;

			ret = fi_enable(ep->shm_ep);
			if (ret)
				goto out;
		}
out:
		ofi_mutex_unlock(&ep->base_ep.util_ep.lock);
		break;
	default:
		ret = -FI_ENOSYS;
		break;
	}

	return ret;
}

static struct fi_ops efa_rdm_ep_fi_ops = {
	.size = sizeof(struct fi_ops),
	.close = efa_rdm_ep_close,
	.bind = efa_rdm_ep_bind,
	.control = efa_rdm_ep_ctrl,
	.ops_open = fi_no_ops_open,
};

static int efa_rdm_ep_cancel_match_recv(struct dlist_entry *item,
				    const void *context)
{
	struct efa_rdm_ope *rxe = container_of(item,
						     struct efa_rdm_ope,
						     entry);
	return rxe->cq_entry.op_context == context;
}

static ssize_t efa_rdm_ep_cancel_recv(struct efa_rdm_ep *ep,
				  struct dlist_entry *recv_list,
				  void *context)
{
	struct dlist_entry *entry;
	struct efa_rdm_ope *rxe;
	struct fi_cq_err_entry err_entry;
	uint32_t api_version;

	ofi_mutex_lock(&ep->base_ep.util_ep.lock);
	entry = dlist_remove_first_match(recv_list,
					 &efa_rdm_ep_cancel_match_recv,
					 context);
	if (!entry) {
		ofi_mutex_unlock(&ep->base_ep.util_ep.lock);
		return 0;
	}

	rxe = container_of(entry, struct efa_rdm_ope, entry);
	rxe->rxr_flags |= EFA_RDM_RXE_RECV_CANCEL;
	if (rxe->fi_flags & FI_MULTI_RECV &&
	    rxe->rxr_flags & EFA_RDM_RXE_MULTI_RECV_POSTED) {
		if (dlist_empty(&rxe->multi_recv_consumers)) {
			/*
			 * No pending messages for the buffer,
			 * release it back to the app.
			 */
			rxe->cq_entry.flags |= FI_MULTI_RECV;
		} else {
			rxe = container_of(rxe->multi_recv_consumers.next,
						struct efa_rdm_ope,
						multi_recv_entry);
			rxr_msg_multi_recv_handle_completion(ep, rxe);
		}
	} else if (rxe->fi_flags & FI_MULTI_RECV &&
		   rxe->rxr_flags & EFA_RDM_RXE_MULTI_RECV_CONSUMER) {
		rxr_msg_multi_recv_handle_completion(ep, rxe);
	}
	ofi_mutex_unlock(&ep->base_ep.util_ep.lock);
	memset(&err_entry, 0, sizeof(err_entry));
	err_entry.op_context = rxe->cq_entry.op_context;
	err_entry.flags |= rxe->cq_entry.flags;
	err_entry.tag = rxe->tag;
	err_entry.err = FI_ECANCELED;
	err_entry.prov_errno = -FI_ECANCELED;

	api_version =
		 efa_rdm_ep_domain(ep)->util_domain.fabric->fabric_fid.api_version;
	if (FI_VERSION_GE(api_version, FI_VERSION(1, 5)))
		err_entry.err_data_size = 0;
	/*
	 * Other states are currently receiving data. Subsequent messages will
	 * be sunk (via EFA_RDM_RXE_RECV_CANCEL flag) and the completion suppressed.
	 */
	if (rxe->state & (EFA_RDM_RXE_INIT | EFA_RDM_RXE_UNEXP | EFA_RDM_RXE_MATCHED))
		efa_rdm_rxe_release(rxe);
	return ofi_cq_write_error(ep->base_ep.util_ep.rx_cq, &err_entry);
}

static ssize_t efa_rdm_ep_cancel(fid_t fid_ep, void *context)
{
	struct efa_rdm_ep *ep;
	int ret;

	ep = container_of(fid_ep, struct efa_rdm_ep, base_ep.util_ep.ep_fid.fid);

	ret = efa_rdm_ep_cancel_recv(ep, &ep->rx_list, context);
	if (ret)
		return ret;

	ret = efa_rdm_ep_cancel_recv(ep, &ep->rx_tagged_list, context);
	return ret;
}

/*
 * Validate p2p opt passed by the user and set the endpoint option if it is
 * valid. If the option chosen is invalid or not supported, return an error.
 *
 * @param[in]	efa_ep	efa endpoint
 * @return 	0 on success, negative errno on error
 */
static int efa_set_fi_hmem_p2p_opt(struct efa_rdm_ep *efa_rdm_ep, int opt)
{
	int i, err;

	/*
	 * Check the opt's validity against the first initialized non-system FI_HMEM
	 * interface
	 */
	/*
	 * TODO this assumes only one non-stantard interface is initialized at a
	 * time. Refactor to handle multiple initialized interfaces to impose
	 * tighter restrictions on valid p2p options.
	 */
	EFA_HMEM_IFACE_FOREACH_NON_SYSTEM(i) {
		err = efa_domain_hmem_validate_p2p_opt(efa_rdm_ep_domain(efa_rdm_ep), efa_hmem_ifaces[i], opt);
		if (err == -FI_ENODATA)
			continue;

		if (!err)
			efa_rdm_ep->hmem_p2p_opt = opt;
		return err;
	}
	return -FI_EINVAL;
}

/**
 * @brief set cuda_api_permitted flag in efa_rdm_ep
 * @param[in,out]	ep			endpoint
 * @param[in]		cuda_api_permitted	whether cuda api is permitted
 * @return		0 on success,
 *			-FI_EOPNOTSUPP if endpoint relies on CUDA API call to support CUDA memory
 * @related efa_rdm_ep
 */
static int efa_rdm_ep_set_cuda_api_permitted(struct efa_rdm_ep *ep, bool cuda_api_permitted)
{
	if (!hmem_ops[FI_HMEM_CUDA].initialized) {
		EFA_WARN(FI_LOG_EP_CTRL, "FI_OPT_CUDA_API_PERMITTED cannot be set when "
			 "CUDA library or CUDA device is not available\n");
		return -FI_EINVAL;
	}

	if (cuda_api_permitted) {
		ep->cuda_api_permitted = true;
		return FI_SUCCESS;
	}

	/* CUDA memory can be supported by using either peer to peer or CUDA API. If neither is
	 * available, we cannot support CUDA memory
	 */
	if (!efa_rdm_ep_domain(ep)->hmem_info[FI_HMEM_CUDA].p2p_supported_by_device)
		return -FI_EOPNOTSUPP;

	ep->cuda_api_permitted = false;
	return 0;
}

/**
 * @brief set use_device_rdma flag in efa_rdm_ep.
 *
 * If the environment variable FI_EFA_USE_DEVICE_RDMA is set, this function will
 * return an error if the value of use_device_rdma is in conflict with the
 * environment setting.
 *
 * @param[in,out]	ep			endpoint
 * @param[in]		use_device_rdma		when true, use device RDMA capabilities.
 * @return		0 on success
 *
 * @related efa_rdm_ep
 */
static int efa_rdm_ep_set_use_device_rdma(struct efa_rdm_ep *ep, bool use_device_rdma)
{
	bool env_value, env_set;

	uint32_t api_version =
		 efa_rdm_ep_domain(ep)->util_domain.fabric->fabric_fid.api_version;

	env_set = rxr_env_has_use_device_rdma();
	if (env_set) {
		env_value = efa_rdm_get_use_device_rdma(api_version);
	}

	if FI_VERSION_LT(api_version, FI_VERSION(1, 18)) {
		/* let the application developer know something is wrong */
		EFA_WARN( FI_LOG_EP_CTRL,
			"Applications using libfabric API version <1.18 are not "
			"allowed to call fi_setopt with FI_OPT_EFA_USE_DEVICE_RDMA.  "
			"Please select a newer libfabric API version in "
			"fi_getinfo during startup to use this option.\n");
		return -FI_ENOPROTOOPT;
	}

	if (env_set && use_device_rdma && !env_value) {
		/* conflict: environment off, but application on */
		/* environment wins: turn it off */
		ep->use_device_rdma = env_value;
		EFA_WARN(FI_LOG_EP_CTRL,
		"Application used fi_setopt to request use_device_rdma, "
		"but user has disabled this by setting the environment "
		"variable FI_EFA_USE_DEVICE_RDMA to 1.\n");
		return -FI_EINVAL;
	}
	if (env_set && !use_device_rdma && env_value) {
		/* conflict: environment on, but application off */
		/* environment wins: turn it on */
		ep->use_device_rdma = env_value;
		EFA_WARN(FI_LOG_EP_CTRL,
		"Application used fi_setopt to disable use_device_rdma, "
		"but this conflicts with user's environment "
		"which has FI_EFA_USE_DEVICE_RDMA=1.  Proceeding with "
		"use_device_rdma=true\n");
		return -FI_EINVAL;
	}
	if (use_device_rdma && !efa_device_support_rdma_read()) {
		/* conflict: application on, hardware off. */
		/* hardware always wins ;-) */
		ep->use_device_rdma = false;
		EFA_WARN(FI_LOG_EP_CTRL,
		"Application used setopt to request use_device_rdma, "
		"but EFA device does not support it\n");
		return -FI_EOPNOTSUPP;
	}
	ep->use_device_rdma = use_device_rdma;
	return 0;
}

/**
 * @brief set sendrecv_in_order_aligned_128_bytes flag in efa_rdm_ep
 *
 * @param[in,out]	ep					endpoint
 * @param[in]		sendrecv_in_order_aligned_128_bytes	whether to enable in_order send/recv
 *								for each 128 bytes aligned buffer
 * @return		0 on success, -FI_EOPNOTSUPP if the option cannot be supported
 */
static
int efa_rdm_ep_set_sendrecv_in_order_aligned_128_bytes(struct efa_rdm_ep *ep,
						   bool sendrecv_in_order_aligned_128_bytes)
{
	/*
	 * RDMA read is used to copy data from host bounce buffer to the
	 * application buffer on device
	 */
	if (sendrecv_in_order_aligned_128_bytes &&
	    !efa_base_ep_support_op_in_order_aligned_128_bytes(&ep->base_ep, IBV_WR_RDMA_READ))
		return -FI_EOPNOTSUPP;

	ep->sendrecv_in_order_aligned_128_bytes = sendrecv_in_order_aligned_128_bytes;
	return 0;
}

/**
 * @brief set write_in_order_aligned_128_bytes flag in efa_rdm_ep
 * @param[in,out]	ep					endpoint
 * @param[in]		write_in_order_aligned_128_bytes	whether to enable RDMA in order write
 *								for each 128 bytes aligned buffer.
 * @return		0 on success, -FI_EOPNOTSUPP if the option cannot be supported.
 */
static
int efa_rdm_ep_set_write_in_order_aligned_128_bytes(struct efa_rdm_ep *ep,
						bool write_in_order_aligned_128_bytes)
{
	if (write_in_order_aligned_128_bytes &&
	    !efa_base_ep_support_op_in_order_aligned_128_bytes(&ep->base_ep, IBV_WR_RDMA_WRITE))
		return -FI_EOPNOTSUPP;

	ep->write_in_order_aligned_128_bytes = write_in_order_aligned_128_bytes;
	return 0;
}

static int efa_rdm_ep_getopt(fid_t fid, int level, int optname, void *optval,
			 size_t *optlen)
{
	struct efa_rdm_ep *efa_rdm_ep;

	efa_rdm_ep = container_of(fid, struct efa_rdm_ep, base_ep.util_ep.ep_fid.fid);

	if (level != FI_OPT_ENDPOINT)
		return -FI_ENOPROTOOPT;

	switch (optname) {
	case FI_OPT_MIN_MULTI_RECV:
		*(size_t *)optval = efa_rdm_ep->min_multi_recv_size;
		*optlen = sizeof(size_t);
		break;
	case FI_OPT_EFA_RNR_RETRY:
		*(size_t *)optval = efa_rdm_ep->base_ep.rnr_retry;
		*optlen = sizeof(size_t);
		break;
	case FI_OPT_FI_HMEM_P2P:
		*(int *)optval = efa_rdm_ep->hmem_p2p_opt;
		*optlen = sizeof(int);
		break;
	case FI_OPT_EFA_EMULATED_READ:
		*(bool *)optval = !efa_rdm_ep_support_rdma_read(efa_rdm_ep);
		*optlen = sizeof(bool);
		break;
	case FI_OPT_EFA_EMULATED_WRITE:
		*(bool *)optval = !efa_rdm_ep_support_rdma_write(efa_rdm_ep);
		*optlen = sizeof(bool);
		break;
	case FI_OPT_EFA_EMULATED_ATOMICS:
		*(bool *)optval = true;
		*optlen = sizeof(bool);
		break;
	case FI_OPT_EFA_USE_DEVICE_RDMA:
		*(bool *)optval = efa_rdm_ep->use_device_rdma;
		*optlen = sizeof(bool);
		break;
	case FI_OPT_EFA_SENDRECV_IN_ORDER_ALIGNED_128_BYTES:
		*(bool *)optval = efa_rdm_ep->sendrecv_in_order_aligned_128_bytes;
		*optlen = sizeof(bool);
		break;
	case FI_OPT_EFA_WRITE_IN_ORDER_ALIGNED_128_BYTES:
		*(bool *)optval = efa_rdm_ep->write_in_order_aligned_128_bytes;
		*optlen = sizeof(bool);
		break;
	default:
		EFA_WARN(FI_LOG_EP_CTRL,
			"Unknown endpoint option %s\n", __func__);
		return -FI_ENOPROTOOPT;
	}

	return FI_SUCCESS;
}

static int efa_rdm_ep_setopt(fid_t fid, int level, int optname,
			 const void *optval, size_t optlen)
{
	struct efa_rdm_ep *efa_rdm_ep;
	int intval, ret;

	efa_rdm_ep = container_of(fid, struct efa_rdm_ep, base_ep.util_ep.ep_fid.fid);

	if (level != FI_OPT_ENDPOINT)
		return -FI_ENOPROTOOPT;

	switch (optname) {
	case FI_OPT_MIN_MULTI_RECV:
		if (optlen != sizeof(size_t))
			return -FI_EINVAL;

		efa_rdm_ep->min_multi_recv_size = *(size_t *)optval;
		break;
	case FI_OPT_EFA_RNR_RETRY:
		if (optlen != sizeof(size_t))
			return -FI_EINVAL;

		/*
		 * Application is required to call to fi_setopt before EP
		 * enabled. If it's calling to fi_setopt after EP enabled,
		 * fail the call.
		 *
		 * efa_ep->qp will be NULL before EP enabled, use it to check
		 * if the call to fi_setopt is before or after EP enabled for
		 * convience, instead of calling to ibv_query_qp
		 */
		if (efa_rdm_ep->base_ep.efa_qp_enabled) {
			EFA_WARN(FI_LOG_EP_CTRL,
				"The option FI_OPT_EFA_RNR_RETRY is required \
				to be set before EP enabled %s\n", __func__);
			return -FI_EINVAL;
		}

		if (!efa_domain_support_rnr_retry_modify(efa_rdm_ep_domain(efa_rdm_ep))) {
			EFA_WARN(FI_LOG_EP_CTRL,
				"RNR capability is not supported %s\n", __func__);
			return -FI_ENOSYS;
		}
		efa_rdm_ep->base_ep.rnr_retry = *(size_t *)optval;
		break;
	case FI_OPT_FI_HMEM_P2P:
		if (optlen != sizeof(int))
			return -FI_EINVAL;

		intval = *(int *)optval;

		ret = efa_set_fi_hmem_p2p_opt(efa_rdm_ep, intval);
		if (ret)
			return ret;
		break;
	case FI_OPT_CUDA_API_PERMITTED:
		if (optlen != sizeof(bool))
			return -FI_EINVAL;
		ret = efa_rdm_ep_set_cuda_api_permitted(efa_rdm_ep, *(bool *)optval);
		if (ret)
			return ret;
		break;
	case FI_OPT_EFA_USE_DEVICE_RDMA:
		if (optlen != sizeof(bool))
			return -FI_EINVAL;
		ret = efa_rdm_ep_set_use_device_rdma(efa_rdm_ep, *(bool *)optval);
		if (ret)
			return ret;
		break;
	case FI_OPT_EFA_SENDRECV_IN_ORDER_ALIGNED_128_BYTES:
		if (optlen != sizeof(bool))
			return -FI_EINVAL;
		ret = efa_rdm_ep_set_sendrecv_in_order_aligned_128_bytes(efa_rdm_ep, *(bool *)optval);
		if (ret)
			return ret;
		break;
	case FI_OPT_EFA_WRITE_IN_ORDER_ALIGNED_128_BYTES:
		if (optlen != sizeof(bool))
			return -FI_EINVAL;
		ret = efa_rdm_ep_set_write_in_order_aligned_128_bytes(efa_rdm_ep, *(bool *)optval);
		if (ret)
			return ret;
		break;
	default:
		EFA_WARN(FI_LOG_EP_CTRL,
			"Unknown endpoint option %s\n", __func__);
		return -FI_ENOPROTOOPT;
	}

	return FI_SUCCESS;
}

static struct fi_ops_ep rxr_ops_ep = {
	.size = sizeof(struct fi_ops_ep),
	.cancel = efa_rdm_ep_cancel,
	.getopt = efa_rdm_ep_getopt,
	.setopt = efa_rdm_ep_setopt,
	.tx_ctx = fi_no_tx_ctx,
	.rx_ctx = fi_no_rx_ctx,
	.rx_size_left = fi_no_rx_size_left,
	.tx_size_left = fi_no_tx_size_left,
};



/** @brief Initializes the endpoint.
 *
 * This function allocates the various buffer pools for the EFA and SHM
 * provider and does other endpoint initialization.
 *
 * @param ep efa_rdm_ep struct to initialize.
 * @return 0 on success, fi_errno on error.
 */
int efa_rdm_ep_init(struct efa_rdm_ep *ep)
{
	int ret;

	ret = rxr_pkt_pool_create(
		ep,
		RXR_PKT_FROM_EFA_TX_POOL,
		rxr_get_tx_pool_chunk_cnt(ep),
		rxr_get_tx_pool_chunk_cnt(ep), /* max count */
		RXR_BUF_POOL_ALIGNMENT,
		&ep->efa_tx_pkt_pool);
	if (ret)
		goto err_free;

	ret = rxr_pkt_pool_create(
		ep,
		RXR_PKT_FROM_EFA_RX_POOL,
		rxr_get_rx_pool_chunk_cnt(ep),
		rxr_get_rx_pool_chunk_cnt(ep), /* max count */
		RXR_BUF_POOL_ALIGNMENT,
		&ep->efa_rx_pkt_pool);
	if (ret)
		goto err_free;

	if (rxr_env.rx_copy_unexp) {
		ret = rxr_pkt_pool_create(
			ep,
			RXR_PKT_FROM_UNEXP_POOL,
			rxr_env.unexp_pool_chunk_size,
			0, /* max count = 0, so pool is allowed to grow */
			RXR_BUF_POOL_ALIGNMENT,
			&ep->rx_unexp_pkt_pool);
		if (ret)
			goto err_free;
	}

	if (rxr_env.rx_copy_ooo) {
		ret = rxr_pkt_pool_create(
			ep,
			RXR_PKT_FROM_OOO_POOL,
			rxr_env.ooo_pool_chunk_size,
			0, /* max count = 0, so pool is allowed to grow */
			RXR_BUF_POOL_ALIGNMENT,
			&ep->rx_ooo_pkt_pool);
		if (ret)
			goto err_free;
	}

	if ((rxr_env.rx_copy_unexp || rxr_env.rx_copy_ooo) &&
	    (efa_rdm_ep_domain(ep)->util_domain.mr_mode & FI_MR_HMEM)) {
		/* this pool is only needed when application requested FI_HMEM capability */
		ret = rxr_pkt_pool_create(
			ep,
			RXR_PKT_FROM_READ_COPY_POOL,
			rxr_env.readcopy_pool_size,
			rxr_env.readcopy_pool_size, /* max count */
			EFA_RDM_IN_ORDER_ALIGNMENT, /* support in-order aligned send/recv */
			&ep->rx_readcopy_pkt_pool);
		if (ret)
			goto err_free;

		ep->rx_readcopy_pkt_pool_used = 0;
		ep->rx_readcopy_pkt_pool_max_used = 0;
	}

	ret = ofi_bufpool_create(&ep->map_entry_pool,
				 sizeof(struct rxr_pkt_rx_map),
				 RXR_BUF_POOL_ALIGNMENT,
				 0, /* no limit for max_cnt */
				 ep->rx_size, 0);

	if (ret)
		goto err_free;

	ret = ofi_bufpool_create(&ep->rx_atomrsp_pool, ep->mtu_size,
				 RXR_BUF_POOL_ALIGNMENT,
				 0, /* no limit for max_cnt */
				 rxr_env.atomrsp_pool_size, 0);
	if (ret)
		goto err_free;

	ret = ofi_bufpool_create(&ep->ope_pool,
				 sizeof(struct efa_rdm_ope),
				 RXR_BUF_POOL_ALIGNMENT,
				 0, /* no limit for max_cnt */
				 ep->tx_size + ep->rx_size, 0);
	if (ret)
		goto err_free;

	/* Initialize entry list */
	dlist_init(&ep->rx_list);
	dlist_init(&ep->rx_unexp_list);
	dlist_init(&ep->rx_tagged_list);
	dlist_init(&ep->rx_unexp_tagged_list);
	dlist_init(&ep->rx_posted_buf_list);
	dlist_init(&ep->ope_queued_rnr_list);
	dlist_init(&ep->ope_queued_ctrl_list);
	dlist_init(&ep->ope_queued_read_list);
	dlist_init(&ep->ope_longcts_send_list);
	dlist_init(&ep->read_pending_list);
	dlist_init(&ep->peer_backoff_list);
	dlist_init(&ep->handshake_queued_peer_list);
#if ENABLE_DEBUG
	dlist_init(&ep->ope_recv_list);
	dlist_init(&ep->rx_pkt_list);
	dlist_init(&ep->tx_pkt_list);
#endif
	dlist_init(&ep->rxe_list);
	dlist_init(&ep->txe_list);

	/* Initialize pkt to rx map */
	ep->pkt_rx_map = NULL;
	return 0;

err_free:
	if (ep->rx_atomrsp_pool)
		ofi_bufpool_destroy(ep->rx_atomrsp_pool);

	if (ep->map_entry_pool)
		ofi_bufpool_destroy(ep->map_entry_pool);

	if (ep->ope_pool)
		ofi_bufpool_destroy(ep->ope_pool);

	if (ep->rx_readcopy_pkt_pool)
		rxr_pkt_pool_destroy(ep->rx_readcopy_pkt_pool);

	if (rxr_env.rx_copy_ooo && ep->rx_ooo_pkt_pool)
		rxr_pkt_pool_destroy(ep->rx_ooo_pkt_pool);

	if (rxr_env.rx_copy_unexp && ep->rx_unexp_pkt_pool)
		rxr_pkt_pool_destroy(ep->rx_unexp_pkt_pool);

	if (ep->efa_rx_pkt_pool)
		rxr_pkt_pool_destroy(ep->efa_rx_pkt_pool);

	if (ep->efa_tx_pkt_pool)
		rxr_pkt_pool_destroy(ep->efa_tx_pkt_pool);

	return ret;
}

struct fi_ops_cm efa_rdm_ep_cm = {
	.size = sizeof(struct fi_ops_cm),
	.setname = fi_no_setname,
	.getname = efa_base_ep_getname,
	.getpeer = fi_no_getpeer,
	.connect = fi_no_connect,
	.listen = fi_no_listen,
	.accept = fi_no_accept,
	.reject = fi_no_reject,
	.shutdown = fi_no_shutdown,
	.join = fi_no_join,
};

/*
 * @brief explicitly allocate a chunk of memory for 6 pools on RX side:
 *     efa's receive packet pool (efa_rx_pkt_pool)
 *     unexpected packet pool (rx_unexp_pkt_pool),
 *     out-of-order packet pool (rx_ooo_pkt_pool), and
 *     local read-copy packet pool (rx_readcopy_pkt_pool).
 *
 * This function is called when the progress engine is called for
 * the 1st time on this endpoint.
 *
 * @param ep[in,out]	endpoint
 * @return		On success, return 0
 * 			On failure, return a negative error code.
 */
int efa_rdm_ep_grow_rx_pools(struct efa_rdm_ep *ep)
{
	int err;

	assert(ep->efa_rx_pkt_pool);
	err = rxr_pkt_pool_grow(ep->efa_rx_pkt_pool);
	if (err) {
		EFA_WARN(FI_LOG_CQ,
			"cannot allocate memory for EFA's RX packet pool. error: %s\n",
			strerror(-err));
		return err;
	}

	if (ep->rx_unexp_pkt_pool) {
		assert(ep->rx_unexp_pkt_pool);
		err = rxr_pkt_pool_grow(ep->rx_unexp_pkt_pool);
		if (err) {
			EFA_WARN(FI_LOG_CQ,
				"cannot allocate memory for unexpected packet pool. error: %s\n",
				strerror(-err));
			return err;
		}
	}

	if (ep->rx_ooo_pkt_pool) {
		assert(ep->rx_ooo_pkt_pool);
		err = rxr_pkt_pool_grow(ep->rx_ooo_pkt_pool);
		if (err) {
			EFA_WARN(FI_LOG_CQ,
				"cannot allocate memory for out-of-order packet pool. error: %s\n",
				strerror(-err));
			return err;
		}
	}

	if (ep->rx_readcopy_pkt_pool) {
		err = rxr_pkt_pool_grow(ep->rx_readcopy_pkt_pool);
		if (err) {
			EFA_WARN(FI_LOG_CQ,
				"cannot allocate and register memory for readcopy packet pool. error: %s\n",
				strerror(-err));
			return err;
		}
	}

	return 0;
}

/**
 * @brief post internal receive buffers for progress engine.
 *
 * It is more efficient to post multiple receive buffers
 * to the device at once than to post each receive buffer
 * individually.
 *
 * Therefore, after an internal receive buffer (a packet
 * entry) was processed, it is not posted to the device
 * right away.
 *
 * Instead, we increase counter
 *      ep->efa_rx_pkts_to_post
 * by one.
 *
 * Later, progress engine calls this function to
 * bulk post internal receive buffers (according to
 * the counter).
 *
 * This function also control number of internal
 * buffers posted to the device in zero copy receive
 * mode.
 *
 * param[in]	ep	endpoint
 */
static inline
void efa_rdm_ep_progress_post_internal_rx_pkts(struct efa_rdm_ep *ep)
{
	int err;

	if (ep->use_zcpy_rx) {
		/*
		 * In zero copy receive mode,
		 *
		 * If application did not post any receive buffer,
		 * we post one internal buffer so endpoint can
		 * receive RxR control packets such as handshake.
		 *
		 * If buffers have posted to the device, we do NOT
		 * repost internal buffers to maximize the chance
		 * user buffer is used to receive data.
		 */
		if (ep->efa_rx_pkts_posted == 0 && ep->efa_rx_pkts_to_post == 0) {
			ep->efa_rx_pkts_to_post = 1;
		} else if (ep->efa_rx_pkts_posted > 0 && ep->efa_rx_pkts_to_post > 0){
			ep->efa_rx_pkts_to_post = 0;
		}
	} else {
		if (ep->efa_rx_pkts_posted == 0 && ep->efa_rx_pkts_to_post == 0) {
			/* Both efa_rx_pkts_posted and efa_rx_pkts_to_post equal to 0 means
			 * this is the first call of the progress engine on this endpoint.
			 *
			 * In this case, we explictly allocate the 1st chunk of memory
			 * for unexp/ooo/readcopy RX packet pool.
			 *
			 * The reason to explicitly allocate the memory for RX packet
			 * pool is to improve efficiency.
			 *
			 * Without explicit memory allocation, a pkt pools's memory
			 * is allocated when 1st packet is allocated from it.
			 * During the computation, different processes got their 1st
			 * unexp/ooo/read-copy packet at different time. Therefore,
			 * if we do not explicitly allocate memory at the beginning,
			 * memory will be allocated at different time.
			 *
			 * When one process is allocating memory, other processes
			 * have to wait. When each process allocate memory at different
			 * time, the accumulated waiting time became significant.
			 *
			 * By explicitly allocating memory at 1st call to progress
			 * engine, the memory allocation is parallelized.
			 * (This assumes the 1st call to the progress engine on
			 * all processes happen at roughly the same time, which
			 * is a valid assumption according to our knowledge of
			 * the workflow of most application)
			 *
			 * The memory was not allocated during endpoint initialization
			 * because some applications will initialize some endpoints
			 * but never uses it, thus allocating memory initialization
			 * causes waste.
			 */
			err = efa_rdm_ep_grow_rx_pools(ep);
			if (err)
				goto err_exit;

			ep->efa_rx_pkts_to_post = rxr_get_rx_pool_chunk_cnt(ep);
		}
	}

	err = efa_rdm_ep_bulk_post_internal_rx_pkts(ep, ep->efa_rx_pkts_to_post);
	if (err)
		goto err_exit;

	ep->efa_rx_pkts_to_post = 0;

	return;

err_exit:

	efa_eq_write_error(&ep->base_ep.util_ep, err, FI_EFA_ERR_INTERNAL_RX_BUF_POST);
}

static inline ssize_t efa_rdm_ep_send_queued_pkts(struct efa_rdm_ep *ep,
					      struct dlist_entry *pkts)
{
	struct dlist_entry *tmp;
	struct rxr_pkt_entry *pkt_entry;
	ssize_t ret;

	dlist_foreach_container_safe(pkts, struct rxr_pkt_entry,
				     pkt_entry, entry, tmp) {

		/* If send succeeded, pkt_entry->entry will be added
		 * to peer->outstanding_tx_pkts. Therefore, it must
		 * be removed from the list before send.
		 */
		dlist_remove(&pkt_entry->entry);

		ret = rxr_pkt_entry_send(ep, pkt_entry, 0);
		if (ret) {
			if (ret == -FI_EAGAIN) {
				/* add the pkt back to pkts, so it can be resent again */
				dlist_insert_tail(&pkt_entry->entry, pkts);
			}

			return ret;
		}
	}
	return 0;
}

static inline void efa_rdm_ep_check_peer_backoff_timer(struct efa_rdm_ep *ep)
{
	struct efa_rdm_peer *peer;
	struct dlist_entry *tmp;

	if (OFI_LIKELY(dlist_empty(&ep->peer_backoff_list)))
		return;

	dlist_foreach_container_safe(&ep->peer_backoff_list, struct efa_rdm_peer,
				     peer, rnr_backoff_entry, tmp) {
		if (ofi_gettime_us() >= peer->rnr_backoff_begin_ts +
					peer->rnr_backoff_wait_time) {
			peer->flags &= ~EFA_RDM_PEER_IN_BACKOFF;
			dlist_remove(&peer->rnr_backoff_entry);
		}
	}
}

#if HAVE_EFADV_CQ_EX
/**
 * @brief Read peer raw address from EFA device and look up the peer address in AV.
 * This function should only be called if the peer AH is unknown.
 * @return Peer address, or FI_ADDR_NOTAVAIL if unavailable.
 */
static inline fi_addr_t rdm_ep_determine_peer_address_from_efadv(struct efa_rdm_ep *ep,
																 struct ibv_cq_ex *ibv_cqx)
{
	struct rxr_pkt_entry *pkt_entry;
	struct efa_ep_addr efa_ep_addr = {0};
	fi_addr_t addr;
	union ibv_gid gid = {0};
	uint32_t *connid = NULL;

	if (ep->ibv_cq_ex_type != EFADV_CQ) {
		/* EFA DV CQ is not supported. This could be due to old EFA kernel module versions. */
		return FI_ADDR_NOTAVAIL;
	}

	/* Attempt to read sgid from EFA firmware */
	if (efadv_wc_read_sgid(efadv_cq_from_ibv_cq_ex(ibv_cqx), &gid) < 0) {
		/* Return code is negative if the peer AH is known */
		return FI_ADDR_NOTAVAIL;
	}

	pkt_entry = (void *)(uintptr_t)ibv_cqx->wr_id;

	connid = rxr_pkt_connid_ptr(pkt_entry);
	if (!connid) {
		return FI_ADDR_NOTAVAIL;
	}

	/*
	 * Use raw:qpn:connid as the key to lookup AV for peer's fi_addr
	 */
	memcpy(efa_ep_addr.raw, gid.raw, sizeof(efa_ep_addr.raw));
	efa_ep_addr.qpn = ibv_wc_read_src_qp(ibv_cqx);
	efa_ep_addr.qkey = *connid;
	addr = ofi_av_lookup_fi_addr(&ep->base_ep.av->util_av, &efa_ep_addr);
	if (addr != FI_ADDR_NOTAVAIL) {
		char gid_str_cdesc[INET6_ADDRSTRLEN];
		inet_ntop(AF_INET6, gid.raw, gid_str_cdesc, INET6_ADDRSTRLEN);
		EFA_WARN(FI_LOG_AV,
				"Recovered peer fi_addr. [Raw]:[QPN]:[QKey] = [%s]:[%" PRIu16 "]:[%" PRIu32 "]\n",
				gid_str_cdesc, efa_ep_addr.qpn, efa_ep_addr.qkey);
	}

	return addr;
}

/**
 * @brief Determine peer address from ibv_cq_ex
 * Attempt to inject or determine peer address if not available. This usually
 * happens when the endpoint receives the first packet from a new peer.
 * There is an edge case for EFA endpoint - the device might lose the address
 * handle of a known peer due to a firmware bug and return FI_ADDR_NOTAVAIL.
 * The provider needs to look up the address using Raw address:QPN:QKey.
 * Note: This function introduces addtional overhead. It should only be called if
 * efa_av_lookup_address_rdm fails to find the peer address.
 * @param ep Pointer to RDM endpoint
 * @param ibv_cqx Pointer to CQ
 * @returns Peer address, or FI_ADDR_NOTAVAIL if unsuccessful.
 */
static inline fi_addr_t rdm_ep_determine_addr_from_ibv_cq_ex(struct efa_rdm_ep *ep, struct ibv_cq_ex *ibv_cqx)
{
	struct rxr_pkt_entry *pkt_entry;
	fi_addr_t addr = FI_ADDR_NOTAVAIL;

	pkt_entry = (void *)(uintptr_t)ibv_cqx->wr_id;

	addr = rxr_pkt_determine_addr(ep, pkt_entry);

	if (addr == FI_ADDR_NOTAVAIL) {
		addr = rdm_ep_determine_peer_address_from_efadv(ep, ibv_cqx);
	}

	return addr;
}
#else
/**
 * @brief Determine peer address from ibv_cq_ex
 * Attempt to inject peer address if not available. This usually
 * happens when the endpoint receives the first packet from a new peer.
 * Note: This function introduces addtional overhead. It should only be called if
 * efa_av_lookup_address_rdm fails to find the peer address.
 * @param ep Pointer to RDM endpoint
 * @param ibv_cqx Pointer to CQ
 * @returns Peer address, or FI_ADDR_NOTAVAIL if unsuccessful.
 */
static inline fi_addr_t rdm_ep_determine_addr_from_ibv_cq_ex(struct efa_rdm_ep *ep, struct ibv_cq_ex *ibv_cqx)
{
	struct rxr_pkt_entry *pkt_entry;

	pkt_entry = (void *)(uintptr_t)ibv_cqx->wr_id;

	return rxr_pkt_determine_addr(ep, pkt_entry);
}
#endif

/**
 * @brief poll rdma-core cq and process the cq entry
 *
 * @param[in]	ep	RDM endpoint
 * @param[in]	cqe_to_process	Max number of cq entry to poll and process. Must be positive.
 */
static inline void rdm_ep_poll_ibv_cq_ex(struct efa_rdm_ep *ep, size_t cqe_to_process)
{
	bool should_end_poll = false;
	/* Initialize an empty ibv_poll_cq_attr struct for ibv_start_poll.
	 * EFA expects .comp_mask = 0, or otherwise returns EINVAL.
	 */
	struct ibv_poll_cq_attr poll_cq_attr = {.comp_mask = 0};
	struct efa_av *efa_av;
	struct rxr_pkt_entry *pkt_entry;
	ssize_t err;
	size_t i = 0;
	int prov_errno;

	assert(cqe_to_process > 0);

	efa_av = ep->base_ep.av;

	/* Call ibv_start_poll only once */
	err = ibv_start_poll(ep->ibv_cq_ex, &poll_cq_attr);
	should_end_poll = !err;

	while (!err) {
		pkt_entry = (void *)(uintptr_t)ep->ibv_cq_ex->wr_id;
		rxr_tracepoint(poll_cq, (size_t) ep->ibv_cq_ex->wr_id);
		if (ep->ibv_cq_ex->status) {
			prov_errno = ibv_wc_read_vendor_err(ep->ibv_cq_ex);
			if (ibv_wc_read_opcode(ep->ibv_cq_ex) == IBV_WC_SEND) {
#if ENABLE_DEBUG
				ep->failed_send_comps++;
#endif
				rxr_pkt_handle_send_error(ep, pkt_entry, FI_EIO, prov_errno);
			} else {
				assert(ibv_wc_read_opcode(ep->ibv_cq_ex) == IBV_WC_RECV);
				rxr_pkt_handle_recv_error(ep, pkt_entry, FI_EIO, prov_errno);
			}
			break;
		}

		switch (ibv_wc_read_opcode(ep->ibv_cq_ex)) {
		case IBV_WC_SEND:
#if ENABLE_DEBUG
			ep->send_comps++;
#endif
			rxr_pkt_handle_send_completion(ep, pkt_entry);
			break;
		case IBV_WC_RECV:
			pkt_entry->addr = efa_av_reverse_lookup_rdm(efa_av, ibv_wc_read_slid(ep->ibv_cq_ex),
								ibv_wc_read_src_qp(ep->ibv_cq_ex), pkt_entry);

			if (pkt_entry->addr == FI_ADDR_NOTAVAIL) {
				pkt_entry->addr = rdm_ep_determine_addr_from_ibv_cq_ex(ep, ep->ibv_cq_ex);
			}

			pkt_entry->pkt_size = ibv_wc_read_byte_len(ep->ibv_cq_ex);
			assert(pkt_entry->pkt_size > 0);
			rxr_pkt_handle_recv_completion(ep, pkt_entry);
#if ENABLE_DEBUG
			ep->recv_comps++;
#endif
			break;
		case IBV_WC_RDMA_READ:
		case IBV_WC_RDMA_WRITE:
			rxr_pkt_handle_rma_completion(ep, pkt_entry);
			break;
		case IBV_WC_RECV_RDMA_WITH_IMM:
			recv_rdma_with_imm_completion(ep,
				ibv_wc_read_imm_data(ep->ibv_cq_ex),
				FI_REMOTE_CQ_DATA | FI_RMA | FI_REMOTE_WRITE,
				pkt_entry );
			break;
		default:
			EFA_WARN(FI_LOG_EP_CTRL,
				"Unhandled cq type\n");
			assert(0 && "Unhandled cq type");
		}

		i++;
		if (i == cqe_to_process) {
			break;
		}

		/*
		 * ibv_next_poll MUST be call after the current WC is fully processed,
		 * which prevents later calls on ibv_cq_ex from reading the wrong WC.
		 */
		err = ibv_next_poll(ep->ibv_cq_ex);
	}

	if (err && err != ENOENT) {
		err = err > 0 ? err : -err;
		prov_errno = ibv_wc_read_vendor_err(ep->ibv_cq_ex);
		efa_eq_write_error(&ep->base_ep.util_ep, err, prov_errno);
	}

	if (should_end_poll)
		ibv_end_poll(ep->ibv_cq_ex);
}

void efa_rdm_ep_progress_internal(struct efa_rdm_ep *ep)
{
	struct ibv_send_wr *bad_wr;
	struct efa_rdm_ope *ope;
	struct efa_rdm_peer *peer;
	struct dlist_entry *tmp;
	ssize_t ret;
	uint64_t flags;

	/* Poll the EFA completion queue. Restrict poll size
	 * to avoid CQE flooding and thereby blocking user thread. */
	rdm_ep_poll_ibv_cq_ex(ep, rxr_env.efa_cq_read_size);

	efa_rdm_ep_progress_post_internal_rx_pkts(ep);

	efa_rdm_ep_check_peer_backoff_timer(ep);
	/*
	 * Resend handshake packet for any peers where the first
	 * handshake send failed.
	 */
	dlist_foreach_container_safe(&ep->handshake_queued_peer_list,
				     struct efa_rdm_peer, peer,
				     handshake_queued_entry, tmp) {
		if (peer->flags & EFA_RDM_PEER_IN_BACKOFF)
			continue;

		ret = rxr_pkt_post_handshake(ep, peer);
		if (ret == -FI_EAGAIN)
			break;

		if (OFI_UNLIKELY(ret)) {
			EFA_WARN(FI_LOG_EP_CTRL,
				"Failed to post HANDSHAKE to peer %ld: %s\n",
				peer->efa_fiaddr, fi_strerror(-ret));
			efa_eq_write_error(&ep->base_ep.util_ep, FI_EIO, FI_EFA_ERR_PEER_HANDSHAKE);
			return;
		}

		dlist_remove(&peer->handshake_queued_entry);
		peer->flags &= ~EFA_RDM_PEER_HANDSHAKE_QUEUED;
		peer->flags |= EFA_RDM_PEER_HANDSHAKE_SENT;
	}

	/*
	 * Send any queued ctrl packets.
	 */
	dlist_foreach_container_safe(&ep->ope_queued_rnr_list,
				     struct efa_rdm_ope,
				     ope, queued_rnr_entry, tmp) {
		peer = efa_rdm_ep_get_peer(ep, ope->addr);
		assert(peer);

		if (peer->flags & EFA_RDM_PEER_IN_BACKOFF)
			continue;

		assert(ope->rxr_flags & EFA_RDM_OPE_QUEUED_RNR);
		assert(!dlist_empty(&ope->queued_pkts));
		ret = efa_rdm_ep_send_queued_pkts(ep, &ope->queued_pkts);

		if (ret == -FI_EAGAIN)
			break;

		if (OFI_UNLIKELY(ret)) {
			assert(ope->type == EFA_RDM_RXE || ope->type == EFA_RDM_TXE);
			if (ope->type == EFA_RDM_RXE)
				efa_rdm_rxe_handle_error(ope, -ret, FI_EFA_ERR_PKT_SEND);
			else
				efa_rdm_txe_handle_error(ope, -ret, FI_EFA_ERR_PKT_SEND);
			return;
		}

		dlist_remove(&ope->queued_rnr_entry);
		ope->rxr_flags &= ~EFA_RDM_OPE_QUEUED_RNR;
	}

	dlist_foreach_container_safe(&ep->ope_queued_ctrl_list,
				     struct efa_rdm_ope,
				     ope, queued_ctrl_entry, tmp) {
		peer = efa_rdm_ep_get_peer(ep, ope->addr);
		assert(peer);

		if (peer->flags & EFA_RDM_PEER_IN_BACKOFF)
			continue;

		assert(ope->rxr_flags & EFA_RDM_OPE_QUEUED_CTRL);
		ret = rxr_pkt_post(ep, ope, ope->queued_ctrl_type, 0);
		if (ret == -FI_EAGAIN)
			break;

		if (OFI_UNLIKELY(ret)) {
			efa_rdm_rxe_handle_error(ope, -ret, FI_EFA_ERR_PKT_POST);
			return;
		}

		/* it can happen that rxr_pkt_post() released ope
		 * (if the ope is rxe and packet type is EOR and inject is used). In
		 * that case rxe's state has been set to EFA_RDM_OPE_FREE and
		 * it has been removed from ep->op_queued_entry_list, so nothing
		 * is left to do.
		 */
		if (ope->state == EFA_RDM_OPE_FREE)
			continue;

		ope->rxr_flags &= ~EFA_RDM_OPE_QUEUED_CTRL;
		dlist_remove(&ope->queued_ctrl_entry);
	}

	/*
	 * Send data packets until window or data queue is exhausted.
	 */
	dlist_foreach_container(&ep->ope_longcts_send_list, struct efa_rdm_ope,
				ope, entry) {
		peer = efa_rdm_ep_get_peer(ep, ope->addr);
		assert(peer);
		if (peer->flags & EFA_RDM_PEER_IN_BACKOFF)
			continue;

		/*
		 * Do not send DATA packet until we received HANDSHAKE packet from the peer,
		 * this is because endpoint does not know whether peer need connid in header
		 * until it get the HANDSHAKE packet.
		 *
		 * We only do this for DATA packet because other types of packets always
		 * has connid in there packet header. If peer does not make use of the connid,
		 * the connid can be safely ignored.
		 *
		 * DATA packet is different because for DATA packet connid is an optional
		 * header inserted between the mandatory header and the application data.
		 * Therefore if peer does not use/understand connid, it will take connid
		 * as application data thus cause data corruption.
		 *
		 * This will not cause deadlock because peer will send a HANDSHAKE packet
		 * back upon receiving 1st packet from the endpoint, and in all 3 sub0protocols
		 * (long-CTS message, emulated long-CTS write and emulated long-CTS read)
		 * where DATA packet is used, endpoint will send other types of packet to
		 * peer before sending DATA packets. The workflow of the 3 sub-protocol
		 * can be found in protocol v4 document chapter 3.
		 */
		if (!(peer->flags & EFA_RDM_PEER_HANDSHAKE_RECEIVED))
			continue;
		while (ope->window > 0) {
			flags = FI_MORE;
			if (ep->efa_max_outstanding_tx_ops - ep->efa_outstanding_tx_ops <= 1 ||
			    ope->window <= ep->max_data_payload_size)
				flags = 0;
			/*
			 * The core's TX queue is full so we can't do any
			 * additional work.
			 */
			if (ep->efa_outstanding_tx_ops == ep->efa_max_outstanding_tx_ops)
				goto out;

			if (peer->flags & EFA_RDM_PEER_IN_BACKOFF)
				break;
			ret = rxr_pkt_post(ep, ope, RXR_DATA_PKT, flags);
			if (OFI_UNLIKELY(ret)) {
				if (ret == -FI_EAGAIN)
					goto out;

				efa_rdm_txe_handle_error(ope, -ret, FI_EFA_ERR_PKT_POST);
				return;
			}
		}
	}

	/*
	 * Send remote read requests until finish or error encoutered
	 */
	dlist_foreach_container_safe(&ep->ope_queued_read_list, struct efa_rdm_ope,
				     ope, queued_read_entry, tmp) {
		peer = efa_rdm_ep_get_peer(ep, ope->addr);
		/*
		 * Here peer can be NULL, when the read request is a
		 * local read request. Local read request is used to copy
		 * data from host memory to device memory on same process.
		 */
		if (peer && (peer->flags & EFA_RDM_PEER_IN_BACKOFF))
			continue;

		/*
		 * The core's TX queue is full so we can't do any
		 * additional work.
		 */
		if (ep->efa_outstanding_tx_ops == ep->efa_max_outstanding_tx_ops)
			goto out;

		ret = efa_rdm_ope_post_read(ope);
		if (ret == -FI_EAGAIN)
			break;

		if (OFI_UNLIKELY(ret)) {
			assert(ope->type == EFA_RDM_TXE || ope->type == EFA_RDM_RXE);
			if (ope->type == EFA_RDM_TXE)
				efa_rdm_txe_handle_error(ope, -ret, FI_EFA_ERR_READ_POST);
			else
				efa_rdm_rxe_handle_error(ope, -ret, FI_EFA_ERR_READ_POST);

			return;
		}

		ope->rxr_flags &= ~EFA_RDM_OPE_QUEUED_READ;
		dlist_remove(&ope->queued_read_entry);
	}

out:
	if (ep->base_ep.xmit_more_wr_tail != &ep->base_ep.xmit_more_wr_head) {
		ret = efa_rdm_ep_post_flush(ep, &bad_wr);
		if (OFI_UNLIKELY(ret))
			efa_eq_write_error(&ep->base_ep.util_ep, -ret, FI_EFA_ERR_WR_POST_SEND);
	}

	return;
}

void efa_rdm_ep_progress(struct util_ep *util_ep)
{
	struct efa_rdm_ep *ep;

	ep = container_of(util_ep, struct efa_rdm_ep, base_ep.util_ep);

	ofi_mutex_lock(&ep->base_ep.util_ep.lock);
	efa_rdm_ep_progress_internal(ep);
	ofi_mutex_unlock(&ep->base_ep.util_ep.lock);
}

int rxr_endpoint(struct fid_domain *domain, struct fi_info *info,
		 struct fid_ep **ep, void *context)
{
	struct efa_domain *efa_domain = NULL;
	struct efa_rdm_ep *efa_rdm_ep = NULL;
	struct fi_cq_attr cq_attr;
	int ret, retv, i;
	struct fi_peer_srx_context peer_srx_context = {0};
	struct fi_rx_attr peer_srx_attr = {0};
	struct fid_ep *peer_srx_ep = NULL;

	efa_rdm_ep = calloc(1, sizeof(*efa_rdm_ep));
	if (!efa_rdm_ep)
		return -FI_ENOMEM;

	efa_domain = container_of(domain, struct efa_domain,
				  util_domain.domain_fid);
	memset(&cq_attr, 0, sizeof(cq_attr));
	cq_attr.format = FI_CQ_FORMAT_DATA;
	cq_attr.wait_obj = FI_WAIT_NONE;

	ret = efa_base_ep_construct(&efa_rdm_ep->base_ep, domain, info,
				    efa_rdm_ep_progress, context);
	if (ret)
		goto err_free_ep;

	efa_rdm_peer_srx_construct(efa_rdm_ep, &efa_rdm_ep->peer_srx);

	if (efa_domain->shm_domain) {
		peer_srx_context.srx = &efa_rdm_ep->peer_srx;
		peer_srx_attr.op_flags |= FI_PEER;
		ret = fi_srx_context(efa_domain->shm_domain, &peer_srx_attr, &peer_srx_ep, &peer_srx_context);
		if (ret)
			goto err_destroy_base_ep;

		assert(!strcmp(efa_domain->shm_info->fabric_attr->name, "shm"));
		ret = fi_endpoint(efa_domain->shm_domain, efa_domain->shm_info,
				  &efa_rdm_ep->shm_ep, efa_rdm_ep);
		if (ret)
			goto err_destroy_base_ep;
	} else {
		efa_rdm_ep->shm_ep = NULL;
	}

	efa_rdm_ep->user_info = fi_dupinfo(info);
	if (!efa_rdm_ep->user_info) {
		ret = -FI_ENOMEM;
		goto err_free_ep;
	}

	efa_rdm_ep->host_id = efa_get_host_id(rxr_env.host_id_file);
	if (efa_rdm_ep->host_id) {
		EFA_INFO(FI_LOG_EP_CTRL, "efa_rdm_ep->host_id: i-%017lx\n", efa_rdm_ep->host_id);
	}

	efa_rdm_ep->rx_size = info->rx_attr->size;
	efa_rdm_ep->tx_size = info->tx_attr->size;
	efa_rdm_ep->rx_iov_limit = info->rx_attr->iov_limit;
	efa_rdm_ep->tx_iov_limit = info->tx_attr->iov_limit;
	efa_rdm_ep->inject_size = info->tx_attr->inject_size;
	efa_rdm_ep->efa_max_outstanding_tx_ops = efa_domain->device->rdm_info->tx_attr->size;
	efa_rdm_ep->efa_max_outstanding_rx_ops = efa_domain->device->rdm_info->rx_attr->size;
	efa_rdm_ep->efa_device_iov_limit = efa_domain->device->rdm_info->tx_attr->iov_limit;
	efa_rdm_ep->use_device_rdma = efa_rdm_get_use_device_rdma(info->fabric_attr->api_version);

	cq_attr.size = MAX(efa_rdm_ep->rx_size + efa_rdm_ep->tx_size,
			   rxr_env.cq_size);

	if (info->tx_attr->op_flags & FI_DELIVERY_COMPLETE)
		EFA_INFO(FI_LOG_CQ, "FI_DELIVERY_COMPLETE unsupported\n");

	assert(info->tx_attr->msg_order == info->rx_attr->msg_order);
	efa_rdm_ep->msg_order = info->rx_attr->msg_order;
	efa_rdm_ep->max_msg_size = info->ep_attr->max_msg_size;
	efa_rdm_ep->msg_prefix_size = info->ep_attr->msg_prefix_size;
	efa_rdm_ep->max_proto_hdr_size = rxr_pkt_max_hdr_size();
	efa_rdm_ep->mtu_size = efa_domain->device->rdm_info->ep_attr->max_msg_size;

	efa_rdm_ep->max_data_payload_size = efa_rdm_ep->mtu_size - sizeof(struct rxr_data_hdr) - sizeof(struct rxr_data_opt_connid_hdr);
	efa_rdm_ep->min_multi_recv_size = efa_rdm_ep->mtu_size - efa_rdm_ep->max_proto_hdr_size;

	if (rxr_env.tx_queue_size > 0 &&
	    rxr_env.tx_queue_size < efa_rdm_ep->efa_max_outstanding_tx_ops)
		efa_rdm_ep->efa_max_outstanding_tx_ops = rxr_env.tx_queue_size;


	efa_rdm_ep->use_zcpy_rx = efa_rdm_ep_use_zcpy_rx(efa_rdm_ep, info);
	EFA_INFO(FI_LOG_EP_CTRL, "efa_rdm_ep->use_zcpy_rx = %d\n", efa_rdm_ep->use_zcpy_rx);

	efa_rdm_ep->handle_resource_management = info->domain_attr->resource_mgmt;
	EFA_INFO(FI_LOG_EP_CTRL,
		"efa_rdm_ep->handle_resource_management = %d\n",
		efa_rdm_ep->handle_resource_management);

#if ENABLE_DEBUG
	efa_rdm_ep->efa_total_posted_tx_ops = 0;
	efa_rdm_ep->send_comps = 0;
	efa_rdm_ep->failed_send_comps = 0;
	efa_rdm_ep->recv_comps = 0;
#endif

	efa_rdm_ep->efa_rx_pkts_posted = 0;
	efa_rdm_ep->efa_rx_pkts_to_post = 0;
	efa_rdm_ep->efa_outstanding_tx_ops = 0;

	assert(!efa_rdm_ep->ibv_cq_ex);

	ret = efa_cq_ibv_cq_ex_open(&cq_attr, efa_domain->device->ibv_ctx,
				    &efa_rdm_ep->ibv_cq_ex, &efa_rdm_ep->ibv_cq_ex_type);

	if (ret) {
		EFA_WARN(FI_LOG_CQ, "Unable to create extended CQ: %s\n", strerror(errno));
		goto err_close_shm_ep;
	}

	ret = efa_rdm_ep_init(efa_rdm_ep);
	if (ret)
		goto err_close_core_cq;

	/* Set hmem_p2p_opt */
	efa_rdm_ep->hmem_p2p_opt = FI_HMEM_P2P_DISABLED;

	/*
	 * TODO this assumes only one non-stantard interface is initialized at a
	 * time. Refactor to handle multiple initialized interfaces to impose
	 * tighter requirements for the default p2p opt
	 */
	EFA_HMEM_IFACE_FOREACH_NON_SYSTEM(i) {
		if (efa_rdm_ep->base_ep.domain->hmem_info[efa_hmem_ifaces[i]].initialized &&
			efa_rdm_ep->base_ep.domain->hmem_info[efa_hmem_ifaces[i]].p2p_supported_by_device) {
			efa_rdm_ep->hmem_p2p_opt = efa_rdm_ep->base_ep.domain->hmem_info[efa_hmem_ifaces[i]].p2p_required_by_impl
				? FI_HMEM_P2P_REQUIRED
				: FI_HMEM_P2P_PREFERRED;
			break;
		}
	}

	efa_rdm_ep->cuda_api_permitted = (FI_VERSION_GE(info->fabric_attr->api_version, FI_VERSION(1, 18)));
	efa_rdm_ep->sendrecv_in_order_aligned_128_bytes = false;
	efa_rdm_ep->write_in_order_aligned_128_bytes = false;

	ret = efa_rdm_ep_create_base_ep_ibv_qp(efa_rdm_ep);
	if (ret)
		goto err_close_core_cq;

	*ep = &efa_rdm_ep->base_ep.util_ep.ep_fid;
	(*ep)->msg = &rxr_ops_msg;
	(*ep)->rma = &rxr_ops_rma;
	(*ep)->atomic = &rxr_ops_atomic;
	(*ep)->tagged = &rxr_ops_tagged;
	(*ep)->fid.ops = &efa_rdm_ep_fi_ops;
	(*ep)->ops = &rxr_ops_ep;
	(*ep)->cm = &efa_rdm_ep_cm;
	return 0;

err_close_core_cq:
	retv = -ibv_destroy_cq(ibv_cq_ex_to_cq(efa_rdm_ep->ibv_cq_ex));
	if (retv)
		EFA_WARN(FI_LOG_CQ, "Unable to close cq: %s\n",
			fi_strerror(-retv));
err_close_shm_ep:
	if (efa_rdm_ep->shm_ep) {
		retv = fi_close(&efa_rdm_ep->shm_ep->fid);
		if (retv)
			EFA_WARN(FI_LOG_EP_CTRL, "Unable to close shm EP: %s\n",
				fi_strerror(-retv));
	}
err_destroy_base_ep:
	efa_base_ep_destruct(&efa_rdm_ep->base_ep);
err_free_ep:
	if (efa_rdm_ep)
		free(efa_rdm_ep);
	return ret;
}

/**
 * @brief record the event that a TX op has been submitted
 *
 * This function is called after a TX operation has been posted
 * successfully. It will:
 *
 *  1. increase the outstanding tx_op counter in endpoint and
 *     in the peer structure.
 *
 *  2. add the TX packet to peer's outstanding TX packet list.
 *
 * Both send and read are considered TX operation.
 *
 * The tx_op counters used to prevent over posting the device
 * and used in flow control. They are also usefull for debugging.
 *
 * Peer's outstanding TX packet list is used when removing a peer
 * to invalidate address of these packets, so that the completion
 * of these packet is ignored.
 *
 * @param[in,out]	ep		endpoint
 * @param[in]		pkt_entry	TX pkt_entry, which contains
 * 					the info of the TX op.
 */
void efa_rdm_ep_record_tx_op_submitted(struct efa_rdm_ep *ep, struct rxr_pkt_entry *pkt_entry)
{
	struct efa_rdm_peer *peer;
	struct efa_rdm_ope *ope;

	ope = pkt_entry->ope;
	/*
	 * peer can be NULL when the pkt_entry is a RMA_CONTEXT_PKT,
	 * and the RMA is a local read toward the endpoint itself
	 */
	peer = efa_rdm_ep_get_peer(ep, pkt_entry->addr);
	if (peer)
		dlist_insert_tail(&pkt_entry->entry,
				  &peer->outstanding_tx_pkts);

	assert(pkt_entry->alloc_type == RXR_PKT_FROM_EFA_TX_POOL);
	ep->efa_outstanding_tx_ops++;
	if (peer)
		peer->efa_outstanding_tx_ops++;

	if (ope)
		ope->efa_outstanding_tx_ops++;
#if ENABLE_DEBUG
	ep->efa_total_posted_tx_ops++;
#endif
}

/**
 * @brief record the event that an TX op is completed
 *
 * This function is called when the completion of
 * a TX operation is received. It will
 *
 * 1. decrease the outstanding tx_op counter in the endpoint
 *    and in the peer.
 *
 * 2. remove the TX packet from peer's outstanding
 *    TX packet list.
 *
 * Both send and read are considered TX operation.
 *
 * One may ask why this function is not integrated
 * into rxr_pkt_entry_relase_tx()?
 *
 * The reason is the action of decrease tx_op counter
 * is not tied to releasing a TX pkt_entry.
 *
 * Sometimes we need to decreate the tx_op counter
 * without releasing a TX pkt_entry. For example,
 * we handle a TX pkt_entry encountered RNR. We need
 * to decrease the tx_op counter and queue the packet.
 *
 * Sometimes we need release TX pkt_entry without
 * decreasing the tx_op counter. For example, when
 * rxr_pkt_post() failed to post a pkt entry.
 *
 * @param[in,out]	ep		endpoint
 * @param[in]		pkt_entry	TX pkt_entry, which contains
 * 					the info of the TX op
 */
void efa_rdm_ep_record_tx_op_completed(struct efa_rdm_ep *ep, struct rxr_pkt_entry *pkt_entry)
{
	struct efa_rdm_ope *ope = NULL;
	struct efa_rdm_peer *peer;

	ope = pkt_entry->ope;
	/*
	 * peer can be NULL when:
	 *
	 * 1. the pkt_entry is a RMA_CONTEXT_PKT, and the RMA op is a local read
	 *    toward the endpoint itself.
	 * 2. peer's address has been removed from address vector. Either because
	 *    a new peer has the same GID+QPN was inserted to address, or because
	 *    application removed the peer from address vector.
	 */
	peer = efa_rdm_ep_get_peer(ep, pkt_entry->addr);
	if (peer)
		dlist_remove(&pkt_entry->entry);

	assert(pkt_entry->alloc_type == RXR_PKT_FROM_EFA_TX_POOL);
	ep->efa_outstanding_tx_ops--;
	if (peer)
		peer->efa_outstanding_tx_ops--;

	if (ope)
		ope->efa_outstanding_tx_ops--;
}

/**
 * @brief handle RX completion due to FI_REMOTE_CQ_DATA via RECV_RDMA_WITH_IMM
 *
 * This function handles hardware-assisted RDMA writes with immediate data at
 * remote endpoint.  These do not have a packet context, nor do they have a
 * connid available.
 *
 * @param[in,out]	ep		endpoint
 * @param[in]		int32_t		Data provided in the IMMEDIATE value.
 * @param[in]		flags		flags (such as FI_REMOTE_CQ_DATA)
 */
void recv_rdma_with_imm_completion(struct efa_rdm_ep *ep, int32_t imm_data,
				   uint64_t flags, struct rxr_pkt_entry *pkt_entry)
{
	struct util_cq *target_cq;
	int ret;
	fi_addr_t src_addr;
	struct efa_av *efa_av;

	target_cq = ep->base_ep.util_ep.rx_cq;
	efa_av = ep->base_ep.av;

	if (ep->base_ep.util_ep.caps & FI_SOURCE) {
		src_addr = efa_av_reverse_lookup_rdm(efa_av,
						ibv_wc_read_slid(ep->ibv_cq_ex),
						ibv_wc_read_src_qp(ep->ibv_cq_ex),
						NULL);
		ret = ofi_cq_write_src(target_cq, NULL, flags, 0, NULL, imm_data, 0, src_addr);
	} else {
		ret = ofi_cq_write(target_cq, NULL, flags, 0, NULL, imm_data, 0);
	}

	if (OFI_UNLIKELY(ret)) {
		EFA_WARN(FI_LOG_CQ,
			"Unable to write a cq entry for remote for RECV_RDMA operation: %s\n",
			fi_strerror(-ret));
		efa_eq_write_error(&ep->base_ep.util_ep, FI_EIO, FI_EFA_ERR_WRITE_SHM_CQ_ENTRY);
	}

	efa_cntr_report_rx_completion(&ep->base_ep.util_ep, flags);

	/* Recv with immediate will consume a pkt_entry, but the pkt is not
	   filled, so free the pkt_entry and record we have one less posted
	   packet now. */
	ep->efa_rx_pkts_posted--;
	rxr_pkt_entry_release_rx(ep, pkt_entry);
}

/* @brief Queue a packet that encountered RNR error and setup RNR backoff
 *
 * We uses an exponential backoff strategy to handle RNR errors.
 *
 * `Backoff` means if a peer encountered RNR, an endpoint will
 * wait a period of time before sending packets to the peer again
 *
 * `Exponential` means the more RNR encountered, the longer the
 * backoff wait time will be.
 *
 * To quantify how long a peer stay in backoff mode, two parameters
 * are defined:
 *
 *    rnr_backoff_begin_ts (ts is timestamp) and rnr_backoff_wait_time.
 *
 * A peer stays in backoff mode until:
 *
 * current_timestamp >= (rnr_backoff_begin_ts + rnr_backoff_wait_time),
 *
 * with one exception: a peer can got out of backoff mode early if a
 * packet's send completion to this peer was reported by the device.
 *
 * Specifically, the implementation of RNR backoff is:
 *
 * For a peer, the first time RNR is encountered, the packet will
 * be resent immediately.
 *
 * The second time RNR is encountered, the endpoint will put the
 * peer in backoff mode, and initialize rnr_backoff_begin_timestamp
 * and rnr_backoff_wait_time.
 *
 * The 3rd and following time RNR is encounter, the RNR will be handled
 * like this:
 *
 *     If peer is already in backoff mode, rnr_backoff_begin_ts
 *     will be updated
 *
 *     Otherwise, peer will be put in backoff mode again,
 *     rnr_backoff_begin_ts will be updated and rnr_backoff_wait_time
 *     will be doubled until it reached maximum wait time.
 *
 * @param[in]	ep		endpoint
 * @param[in]	list		queued RNR packet list
 * @param[in]	pkt_entry	packet entry that encounter RNR
 */
void efa_rdm_ep_queue_rnr_pkt(struct efa_rdm_ep *ep,
			  struct dlist_entry *list,
			  struct rxr_pkt_entry *pkt_entry)
{
	struct efa_rdm_peer *peer;

#if ENABLE_DEBUG
	dlist_remove(&pkt_entry->dbg_entry);
#endif
	dlist_insert_tail(&pkt_entry->entry, list);

	peer = efa_rdm_ep_get_peer(ep, pkt_entry->addr);
	assert(peer);
	if (!(pkt_entry->flags & RXR_PKT_ENTRY_RNR_RETRANSMIT)) {
		/* This is the first time this packet encountered RNR,
		 * we are NOT going to put the peer in backoff mode just yet.
		 */
		pkt_entry->flags |= RXR_PKT_ENTRY_RNR_RETRANSMIT;
		peer->rnr_queued_pkt_cnt++;
		return;
	}

	/* This packet has encountered RNR multiple times, therefore the peer
	 * need to be in backoff mode.
	 *
	 * If the peer is already in backoff mode, we just need to update the
	 * RNR backoff begin time.
	 *
	 * Otherwise, we need to put the peer in backoff mode and set up backoff
	 * begin time and wait time.
	 */
	if (peer->flags & EFA_RDM_PEER_IN_BACKOFF) {
		peer->rnr_backoff_begin_ts = ofi_gettime_us();
		return;
	}

	peer->flags |= EFA_RDM_PEER_IN_BACKOFF;
	dlist_insert_tail(&peer->rnr_backoff_entry,
			  &ep->peer_backoff_list);

	peer->rnr_backoff_begin_ts = ofi_gettime_us();
	if (peer->rnr_backoff_wait_time == 0) {
		if (rxr_env.rnr_backoff_initial_wait_time > 0)
			peer->rnr_backoff_wait_time = rxr_env.rnr_backoff_initial_wait_time;
		else
			peer->rnr_backoff_wait_time = MAX(RXR_RAND_MIN_TIMEOUT,
							  rand() %
							  RXR_RAND_MAX_TIMEOUT);

		EFA_DBG(FI_LOG_EP_DATA,
		       "initializing backoff timeout for peer: %" PRIu64
		       " timeout: %ld rnr_queued_pkts: %d\n",
		       pkt_entry->addr, peer->rnr_backoff_wait_time,
		       peer->rnr_queued_pkt_cnt);
	} else {
		peer->rnr_backoff_wait_time = MIN(peer->rnr_backoff_wait_time * 2,
						  rxr_env.rnr_backoff_wait_time_cap);
		EFA_DBG(FI_LOG_EP_DATA,
		       "increasing backoff timeout for peer: %" PRIu64
		       " to %ld rnr_queued_pkts: %d\n",
		       pkt_entry->addr, peer->rnr_backoff_wait_time,
		       peer->rnr_queued_pkt_cnt);
	}
}

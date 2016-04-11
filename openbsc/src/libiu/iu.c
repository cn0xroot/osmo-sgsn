#include <stdint.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdbool.h>

#include <osmocom/core/select.h>
#include <osmocom/core/prim.h>
#include <osmocom/core/talloc.h>
#include <osmocom/core/logging.h>
#include <osmocom/core/application.h>
#include <osmocom/vty/logging.h>

#include <osmocom/gsm/gsm48.h>
#include <osmocom/gprs/gprs_msgb.h>

#include <osmocom/sigtran/sua.h>
#include <osmocom/sigtran/sccp_sap.h>
#include <osmocom/sigtran/sccp_helpers.h>

#include <openbsc/gprs_sgsn.h>
#include <openbsc/iu.h>
#include <openbsc/debug.h>

#include <pdp.h>

#include <osmocom/ranap/ranap_ies_defs.h>
#include <osmocom/ranap/ranap_common.h>
#include <osmocom/ranap/ranap_common_cn.h>
#include <osmocom/ranap/ranap_msg_factory.h>

#include <asn1c/asn1helpers.h>

/* Parsed global RNC id. See also struct RANAP_GlobalRNC_ID, and note that the
 * PLMN identity is a BCD representation of the MCC and MNC.
 * See iu_grnc_id_parse(). */
struct iu_grnc_id {
	uint16_t mcc;
	uint16_t mnc;
	uint16_t rnc_id;
};

/* A remote RNC (Radio Network Controller, like BSC but for UMTS) that has
 * called us and is currently reachable at the given osmo_sua_link. So, when we
 * know a LAC for a subscriber, we can page it at the RNC matching that LAC or
 * RAC. An HNB-GW typically presents itself as if it were a single RNC, even
 * though it may have several RNCs in hNodeBs connected to it. Those will then
 * share the same RNC id, which they actually receive and adopt from the HNB-GW
 * in the HNBAP HNB REGISTER ACCEPT message. */
struct iu_rnc {
	struct llist_head entry;

	uint16_t rnc_id;
	uint16_t lac; /* Location Area Code (used for CS and PS) */
	uint8_t rac; /* Routing Area Code (used for PS only) */
	struct osmo_sua_link *link;
};

void *talloc_iu_ctx;

int asn1_xer_print = 1;
void *talloc_asn1_ctx;

iu_recv_cb_t global_iu_recv_cb = NULL;
iu_event_cb_t global_iu_event_cb = NULL;

static LLIST_HEAD(ue_conn_ctx_list);
static LLIST_HEAD(rnc_list);

struct ue_conn_ctx *ue_conn_ctx_alloc(struct osmo_sua_link *link, uint32_t conn_id)
{
	struct ue_conn_ctx *ctx = talloc_zero(talloc_iu_ctx, struct ue_conn_ctx);

	ctx->link = link;
	ctx->conn_id = conn_id;
	llist_add(&ctx->list, &ue_conn_ctx_list);

	return ctx;
}

struct ue_conn_ctx *ue_conn_ctx_find(struct osmo_sua_link *link,
				     uint32_t conn_id)
{
	struct ue_conn_ctx *ctx;

	llist_for_each_entry(ctx, &ue_conn_ctx_list, list) {
		if (ctx->link == link && ctx->conn_id == conn_id)
			return ctx;
	}
	return NULL;
}

static struct iu_rnc *iu_rnc_alloc(uint16_t rnc_id, uint16_t lac, uint8_t rac,
				   struct osmo_sua_link *link)
{
	struct iu_rnc *rnc = talloc_zero(talloc_iu_ctx, struct iu_rnc);

	rnc->rnc_id = rnc_id;
	rnc->lac = lac;
	rnc->rac = rac;
	rnc->link = link;
	llist_add(&rnc->entry, &rnc_list);

	LOGP(DRANAP, LOGL_NOTICE, "New RNC %d (LAC=%d RAC=%d)\n",
	     rnc->rnc_id, rnc->lac, rnc->rac);

	return rnc;
}

static struct iu_rnc *iu_rnc_register(uint16_t rnc_id, uint16_t lac,
				      uint8_t rac, struct osmo_sua_link *link)
{
	struct iu_rnc *rnc;
	llist_for_each_entry(rnc, &rnc_list, entry) {
		if (rnc->rnc_id != rnc_id)
			continue;

		/* We have this RNC Id registered already. Make sure that the
		 * details match. */

		/* TODO should a mismatch be an error? */
		if (rnc->lac != lac || rnc->rac != rac)
			LOGP(DRANAP, LOGL_NOTICE, "RNC %d changes its details:"
			     " LAC=%d RAC=%d --> LAC=%d RAC=%d\n",
			     rnc->rnc_id, rnc->lac, rnc->rac,
			     lac, rac);
		rnc->lac = lac;
		rnc->rac = rac;

		if (link && rnc->link != link)
			LOGP(DRANAP, LOGL_NOTICE, "RNC %d on new link"
			     " (LAC=%d RAC=%d)\n",
			     rnc->rnc_id, rnc->lac, rnc->rac);
		rnc->link = link;
		return rnc;
	}

	/* Not found, make a new one. */
	return iu_rnc_alloc(rnc_id, lac, rac, link);
}

/* Discard/invalidate all ue_conn_ctx and iu_rnc entries that reference the
 * given link, since this link is invalid and about to be deallocated. For
 * each ue_conn_ctx, invoke the iu_event_cb_t with IU_EVENT_LINK_INVALIDATED.
 */
void iu_link_del(struct osmo_sua_link *link)
{
	struct iu_rnc *rnc, *rnc_next;
	llist_for_each_entry_safe(rnc, rnc_next, &rnc_list, entry) {
		if (!rnc->link)
			continue;
		if (rnc->link != link)
			continue;
		rnc->link = NULL;
		llist_del(&rnc->entry);
		talloc_free(rnc);
	}

	struct ue_conn_ctx *uec, *uec_next;
	llist_for_each_entry_safe(uec, uec_next, &ue_conn_ctx_list, list) {
		if (uec->link != link)
			continue;
		uec->link = NULL;
		global_iu_event_cb(uec, IU_EVENT_LINK_INVALIDATED, NULL);
	}
}

/***********************************************************************
 * RANAP handling
 ***********************************************************************/

static int iu_rab_act(struct ue_conn_ctx *ue_ctx, struct msgb *msg)
{
	struct osmo_scu_prim *prim;

	/* wrap RANAP message in SCCP N-DATA.req */
	prim = (struct osmo_scu_prim *) msgb_push(msg, sizeof(*prim));
	prim->u.data.conn_id = ue_ctx->conn_id;
	osmo_prim_init(&prim->oph,
		       SCCP_SAP_USER,
		       OSMO_SCU_PRIM_N_DATA,
		       PRIM_OP_REQUEST,
		       msg);
	return osmo_sua_user_link_down(ue_ctx->link, &prim->oph);
}

int iu_rab_act_cs(struct ue_conn_ctx *ue_ctx, uint32_t rtp_ip, uint16_t rtp_port)
{
	struct msgb *msg;

	/* FIXME: Generate unique RAB ID per UE */
	msg = ranap_new_msg_rab_assign_voice(1, rtp_ip, rtp_port);
	msg->l2h = msg->data;
	return iu_rab_act(ue_ctx, msg);
}

int iu_rab_act_ps(uint8_t rab_id, struct sgsn_pdp_ctx *pdp)
{
	struct msgb *msg;
	struct sgsn_mm_ctx *mm = pdp->mm;
	struct ue_conn_ctx *uectx;
	uint32_t ggsn_ip;

	uectx = mm->iu.ue_ctx;

	/* Get the IP address for ggsn user plane */
	memcpy(&ggsn_ip, pdp->lib->gsnru.v, pdp->lib->gsnru.l);
	ggsn_ip = htonl(ggsn_ip);

	msg = ranap_new_msg_rab_assign_data(rab_id, ggsn_ip, pdp->lib->teid_own);
	msg->l2h = msg->data;
	return iu_rab_act(uectx, msg);
}

int iu_rab_deact(struct ue_conn_ctx *ue_ctx, uint8_t rab_id)
{
	/* FIXME */
	return -1;
}

int iu_tx_sec_mode_cmd(struct ue_conn_ctx *uectx, struct gsm_auth_tuple *tp,
		       int send_ck, int new_key)
{
	struct osmo_scu_prim *prim;
	struct msgb *msg;
	uint8_t ik[16];
	uint8_t ck[16];
	unsigned int i;

	/* C5 function to derive IK from Kc */
	for (i = 0; i < 4; i++)
		ik[i] = tp->kc[i] ^ tp->kc[i+4];
	memcpy(ik+4, tp->kc, 8);
	for (i = 12; i < 16; i++)
		ik[i] = ik[i-12];

	if (send_ck) {
		/* C4 function to derive CK from Kc */
		memcpy(ck, tp->kc, 8);
		memcpy(ck+8, tp->kc, 8);
	}

	/* create RANAP message */
	msg = ranap_new_msg_sec_mod_cmd(ik, send_ck? ck : NULL, new_key ? RANAP_KeyStatus_new : RANAP_KeyStatus_old);
	msg->l2h = msg->data;
	/* wrap RANAP message in SCCP N-DATA.req */
	prim = (struct osmo_scu_prim *) msgb_push(msg, sizeof(*prim));
	prim->u.data.conn_id = uectx->conn_id;
	osmo_prim_init(&prim->oph, SCCP_SAP_USER,
			OSMO_SCU_PRIM_N_DATA,
			PRIM_OP_REQUEST, msg);
	osmo_sua_user_link_down(uectx->link, &prim->oph);

	return 0;
}

static int iu_grnc_id_parse(struct iu_grnc_id *dst,
			    struct RANAP_GlobalRNC_ID *src)
{
	/* The size is coming from arbitrary sender, check it gracefully */
	if (src->pLMNidentity.size != 3) {
		LOGP(DRANAP, LOGL_ERROR, "Invalid PLMN Identity size:"
		     " should be 3, is %d\n", src->pLMNidentity.size);
		return -1;
	}
	gsm48_mcc_mnc_from_bcd(&src->pLMNidentity.buf[0],
			       &dst->mcc, &dst->mnc);
	dst->rnc_id = (uint16_t)src->rNC_ID;
	return 0;
}

#if 0
 -- not used at present --
static int iu_grnc_id_compose(struct iu_grnc_id *src,
			      struct RANAP_GlobalRNC_ID *dst)
{
	/* The caller must ensure proper size */
	OSMO_ASSERT(dst->pLMNidentity.size == 3);
	gsm48_mcc_mnc_to_bcd(&dst->pLMNidentity.buf[0],
			     src->mcc, src->mnc);
	dst->rNC_ID = src->rnc_id;
	return 0;
}
#endif

static int ranap_handle_co_initial_ue(void *ctx, RANAP_InitialUE_MessageIEs_t *ies)
{
	struct ue_conn_ctx *ue_conn = ctx;
	struct gprs_ra_id ra_id;
	struct iu_grnc_id grnc_id;
	uint16_t sai;
	struct msgb *msg = msgb_alloc(256, "RANAP->NAS");

	if (ranap_parse_lai(&ra_id, &ies->lai) != 0) {
		LOGP(DRANAP, LOGL_ERROR, "Failed to parse RANAP LAI IE\n");
		return -1;
	}

	if (iu_grnc_id_parse(&grnc_id, &ies->globalRNC_ID) != 0) {
		LOGP(DRANAP, LOGL_ERROR,
		     "Failed to parse RANAP Global-RNC-ID IE\n");
		return -1;
	}

	sai = asn1str_to_u16(&ies->sai.sAC);
	msgb_gmmh(msg) = msgb_put(msg, ies->nas_pdu.size);
	memcpy(msgb_gmmh(msg), ies->nas_pdu.buf, ies->nas_pdu.size);

	/* Make sure we know the RNC Id and LAC+RAC coming in on this connection. */
	iu_rnc_register(grnc_id.rnc_id, ra_id.lac, ra_id.rac, ue_conn->link);

	/* Feed into the MM layer */
	msg->dst = ctx;
	global_iu_recv_cb(msg, &ra_id, &sai);

	return 0;
}

static int ranap_handle_co_dt(void *ctx, RANAP_DirectTransferIEs_t *ies)
{
	struct gprs_ra_id _ra_id, *ra_id = NULL;
	uint16_t _sai, *sai = NULL;
	struct msgb *msg = msgb_alloc(256, "RANAP->NAS");

	if (ies->presenceMask & DIRECTTRANSFERIES_RANAP_LAI_PRESENT) {
		if (ranap_parse_lai(&_ra_id, &ies->lai) != 0) {
			LOGP(DRANAP, LOGL_ERROR, "Failed to parse RANAP LAI IE\n");
			return -1;
		}
		ra_id = &_ra_id;
		if (ies->presenceMask & DIRECTTRANSFERIES_RANAP_RAC_PRESENT) {
			_ra_id.rac = asn1str_to_u8(&ies->rac);
		}
		if (ies->presenceMask & DIRECTTRANSFERIES_RANAP_SAI_PRESENT) {
			_sai = asn1str_to_u16(&ies->sai.sAC);
			sai = &_sai;
		}
	}

	msgb_gmmh(msg) = msgb_put(msg, ies->nas_pdu.size);
	memcpy(msgb_gmmh(msg), ies->nas_pdu.buf, ies->nas_pdu.size);

	/* Feed into the MM/CC/SMS-CP layer */
	msg->dst = ctx;
	global_iu_recv_cb(msg, ra_id, sai);

	return 0;
}

static int ranap_handle_co_err_ind(void *ctx, RANAP_ErrorIndicationIEs_t *ies)
{
	if (ies->presenceMask & ERRORINDICATIONIES_RANAP_CAUSE_PRESENT)
		LOGP(DRANAP, LOGL_ERROR, "Rx Error Indication (%s)\n",
			ranap_cause_str(&ies->cause));
	else
		LOGP(DRANAP, LOGL_ERROR, "Rx Error Indication\n");

	return 0;
}

int iu_tx(struct msgb *msg, uint8_t sapi)
{
	struct ue_conn_ctx *uectx = msg->dst;
	struct osmo_scu_prim *prim;

	LOGP(DRANAP, LOGL_INFO, "Transmitting L3 Message as RANAP DT (SUA link %p conn_id %u)\n",
	     uectx->link, uectx->conn_id);

	msg = ranap_new_msg_dt(sapi, msg->data, msgb_length(msg));
	msg->l2h = msg->data;
	prim = (struct osmo_scu_prim *) msgb_push(msg, sizeof(*prim));
	prim->u.data.conn_id = uectx->conn_id;
	osmo_prim_init(&prim->oph, SCCP_SAP_USER,
			OSMO_SCU_PRIM_N_DATA,
			PRIM_OP_REQUEST, msg);
	osmo_sua_user_link_down(uectx->link, &prim->oph);
	return 0;
}

static int ranap_handle_co_iu_rel_req(struct ue_conn_ctx *ctx, RANAP_Iu_ReleaseRequestIEs_t *ies)
{
	struct msgb *msg;
	struct osmo_scu_prim *prim;

	LOGP(DRANAP, LOGL_INFO, "Received Iu Release Request, Sending Release Command\n");
	msg = ranap_new_msg_iu_rel_cmd(&ies->cause);
	msg->l2h = msg->data;
	prim = (struct osmo_scu_prim *) msgb_push(msg, sizeof(*prim));
	prim->u.data.conn_id = ctx->conn_id;
	osmo_prim_init(&prim->oph, SCCP_SAP_USER,
			OSMO_SCU_PRIM_N_DATA,
			PRIM_OP_REQUEST, msg);
	osmo_sua_user_link_down(ctx->link, &prim->oph);
	return 0;
}

static int ranap_handle_co_rab_ass_resp(struct ue_conn_ctx *ctx, RANAP_RAB_AssignmentResponseIEs_t *ies)
{
	int rc = -1;

	LOGP(DRANAP, LOGL_INFO, "RAB Asignment Response:");
	if (ies->presenceMask & RAB_ASSIGNMENTRESPONSEIES_RANAP_RAB_SETUPORMODIFIEDLIST_PRESENT) {
		/* TODO: Iterate over list of SetupOrModifiedList IEs and handle each one */
		RANAP_IE_t *ranap_ie = ies->raB_SetupOrModifiedList.raB_SetupOrModifiedList_ies.list.array[0];
		RANAP_RAB_SetupOrModifiedItemIEs_t setup_ies;

		rc = ranap_decode_rab_setupormodifieditemies_fromlist(&setup_ies, &ranap_ie->value);
		if (rc) {
			LOGP(DRANAP, LOGL_ERROR, "Error in ranap_decode_rab_setupormodifieditemies()\n");
			return rc;
		}

		rc = global_iu_event_cb(ctx, IU_EVENT_RAB_ASSIGN, &setup_ies);

		ranap_free_rab_setupormodifieditemies(&setup_ies);
	}

	LOGPC(DRANAP, LOGL_INFO, "\n");

	return rc;
}

/* Entry point for connection-oriented RANAP message */
static void cn_ranap_handle_co(void *ctx, ranap_message *message)
{
	int rc;

	LOGP(DRANAP, LOGL_NOTICE, "handle_co(dir=%u, proc=%u)\n", message->direction, message->procedureCode);

	switch (message->direction) {
	case RANAP_RANAP_PDU_PR_initiatingMessage:
		switch (message->procedureCode) {
		case RANAP_ProcedureCode_id_InitialUE_Message:
			rc = ranap_handle_co_initial_ue(ctx, &message->msg.initialUE_MessageIEs);
			break;
		case RANAP_ProcedureCode_id_DirectTransfer:
			rc = ranap_handle_co_dt(ctx, &message->msg.directTransferIEs);
			break;
		case RANAP_ProcedureCode_id_ErrorIndication:
			rc = ranap_handle_co_err_ind(ctx, &message->msg.errorIndicationIEs);
			break;
		case RANAP_ProcedureCode_id_Iu_ReleaseRequest:
			/* Iu Release Request */
			rc = ranap_handle_co_iu_rel_req(ctx, &message->msg.iu_ReleaseRequestIEs);
			break;
		default:
			LOGP(DRANAP, LOGL_ERROR, "Received Initiating Message: unknown Procedure Code %d\n",
			     message->procedureCode);
			rc = -1;
			break;
		}
		break;
	case RANAP_RANAP_PDU_PR_successfulOutcome:
		switch (message->procedureCode) {
		case RANAP_ProcedureCode_id_SecurityModeControl:
			/* Security Mode Complete */
			LOGP(DRANAP, LOGL_NOTICE, "FIXME: Handle security mode complete\n");
			rc = global_iu_event_cb(ctx, IU_EVENT_SECURITY_MODE_COMPLETE, NULL);
			break;
		case RANAP_ProcedureCode_id_Iu_Release:
			/* Iu Release Complete */
			LOGP(DRANAP, LOGL_NOTICE, "FIXME: Handle Iu release complete\n");
			rc = global_iu_event_cb(ctx, IU_EVENT_IU_RELEASE, NULL);
			if (rc) {
				LOGP(DRANAP, LOGL_ERROR, "Iu Release event: Iu Event callback returned %d\n",
				     rc);
			}
			break;
		default:
			LOGP(DRANAP, LOGL_ERROR, "Received Successful Outcome: unknown Procedure Code %d\n",
			     message->procedureCode);
			rc = -1;
			break;
		}
	case RANAP_RANAP_PDU_PR_outcome:
		switch (message->procedureCode) {
		case RANAP_ProcedureCode_id_RAB_Assignment:
			/* RAB Assignment Response */
			rc = ranap_handle_co_rab_ass_resp(ctx, &message->msg.raB_AssignmentResponseIEs);
			break;
		default:
			LOGP(DRANAP, LOGL_ERROR, "Received Outcome: unknown Procedure Code %d\n",
			     message->procedureCode);
			rc = -1;
			break;
		}
	case RANAP_RANAP_PDU_PR_unsuccessfulOutcome:
	default:
		LOGP(DRANAP, LOGL_ERROR, "Received Unsuccessful Outcome: Procedure Code %d\n",
		     message->procedureCode);
		rc = -1;
		break;
	}

	if (rc) {
		LOGP(DRANAP, LOGL_ERROR, "Error in cn_ranap_handle_co (%d)\n",
		     rc);
		/* TODO handling of the error? */
	}
}

static int ranap_handle_cl_reset_req(void *ctx, RANAP_ResetIEs_t *ies)
{
	/* FIXME: send reset response */
	return -1;
}

static int ranap_handle_cl_err_ind(void *ctx, RANAP_ErrorIndicationIEs_t *ies)
{
	if (ies->presenceMask & ERRORINDICATIONIES_RANAP_CAUSE_PRESENT)
		LOGP(DRANAP, LOGL_ERROR, "Rx Error Indication (%s)\n",
			ranap_cause_str(&ies->cause));
	else
		LOGP(DRANAP, LOGL_ERROR, "Rx Error Indication\n");

	return 0;
}

/* Entry point for connection-less RANAP message */
static void cn_ranap_handle_cl(void *ctx, ranap_message *message)
{
	int rc;

	switch (message->direction) {
	case RANAP_RANAP_PDU_PR_initiatingMessage:
		switch (message->procedureCode) {
		case RANAP_ProcedureCode_id_Reset:
			/* received reset.req, send reset.resp */
			rc = ranap_handle_cl_reset_req(ctx, &message->msg.resetIEs);
			break;
		case RANAP_ProcedureCode_id_ErrorIndication:
			rc = ranap_handle_cl_err_ind(ctx, &message->msg.errorIndicationIEs);
			break;
		default:
			rc = -1;
			break;
		}
		break;
	case RANAP_RANAP_PDU_PR_successfulOutcome:
	case RANAP_RANAP_PDU_PR_unsuccessfulOutcome:
	case RANAP_RANAP_PDU_PR_outcome:
	default:
		rc = -1;
		break;
	}

	if (rc) {
		LOGP(DRANAP, LOGL_ERROR, "Error in cn_ranap_handle_cl (%d)\n",
		     rc);
		/* TODO handling of the error? */
	}
}

/***********************************************************************
 * Paging
 ***********************************************************************/

/* Send a paging command down a given SUA link. tmsi and paging_cause are
 * optional and may be passed NULL and 0, respectively, to disable their use.
 * See enum RANAP_PagingCause.
 *
 * If TMSI is given, the IMSI is not sent over the air interface. Nevertheless,
 * the IMSI is still required for resolution in the HNB-GW and/or(?) RNC. */
static int iu_tx_paging_cmd(struct osmo_sua_link *link,
			    const char *imsi, const uint32_t *tmsi,
			    bool is_ps, uint32_t paging_cause)
{
	struct msgb *msg;
	msg = ranap_new_msg_paging_cmd(imsi, tmsi, is_ps? 1 : 0, paging_cause);
	msg->l2h = msg->data;
	return osmo_sccp_tx_unitdata_ranap(link, 1, 2, msg->data,
					   msgb_length(msg));
}

static int iu_page(const char *imsi, const uint32_t *tmsi_or_ptimsi,
		   uint16_t lac, uint8_t rac, bool is_ps)
{
	struct iu_rnc *rnc;
	int pagings_sent = 0;

	LOGP(DRANAP, LOGL_DEBUG, "%s: Looking for RNCs to page for IMSI %s"
	     " (paging will use %s)\n",
	     is_ps? "IuPS" : "IuCS",
	     imsi,
	     tmsi_or_ptimsi ? (is_ps? "PTMSI" : "TMSI")
		              : "IMSI"
	    );

	llist_for_each_entry(rnc, &rnc_list, entry) {
		if (!rnc->link) {
			/* Not actually connected, don't count it. */
			continue;
		}
		if (rnc->lac != lac)
			continue;
		if (is_ps && rnc->rac != rac)
			continue;

		/* Found a match! */
		if (iu_tx_paging_cmd(rnc->link, imsi, tmsi_or_ptimsi, is_ps, 0)
		    == 0) {
			LOGP(DRANAP, LOGL_DEBUG,
			     "%s: Paged for IMSI %s on RNC %d, on SUA link %p\n",
			     is_ps? "IuPS" : "IuCS",
			     imsi, rnc->rnc_id, rnc->link);
			pagings_sent ++;
		}
	}

	/* Some logging... */
	if (pagings_sent > 0) {
		LOGP(DRANAP, LOGL_DEBUG,
		     "%s: %d RNCs were paged for IMSI %s.\n",
		     is_ps? "IuPS" : "IuCS",
		     pagings_sent, imsi);
	}
	else {
		if (is_ps) {
			LOGP(DRANAP, LOGL_ERROR, "IuPS: Found no RNC to page for"
			     " LAC %d RAC %d (would have paged IMSI %s)\n",
			     lac, rac, imsi);
		}
		else {
			LOGP(DRANAP, LOGL_ERROR, "IuCS: Found no RNC to page for"
			     " LAC %d (would have paged IMSI %s)\n",
			     lac, imsi);
		}
	}

	return pagings_sent;
}

int iu_page_cs(const char *imsi, const uint32_t *tmsi, uint16_t lac)
{
	return iu_page(imsi, tmsi, lac, 0, false);
}

int iu_page_ps(const char *imsi, const uint32_t *ptmsi, uint16_t lac, uint8_t rac)
{
	return iu_page(imsi, ptmsi, lac, rac, true);
}


/***********************************************************************
 *
 ***********************************************************************/

int tx_unitdata(struct osmo_sua_link *link);
int tx_conn_req(struct osmo_sua_link *link, uint32_t conn_id);

struct osmo_prim_hdr *make_conn_req(uint32_t conn_id);
struct osmo_prim_hdr *make_dt1_req(uint32_t conn_id, const uint8_t *data, unsigned int len);

struct osmo_prim_hdr *make_conn_resp(struct osmo_scu_connect_param *param)
{
	struct msgb *msg = msgb_alloc(1024, "conn_resp");
	struct osmo_scu_prim *prim;

	prim = (struct osmo_scu_prim *) msgb_put(msg, sizeof(*prim));
	osmo_prim_init(&prim->oph, SCCP_SAP_USER,
			OSMO_SCU_PRIM_N_CONNECT,
			PRIM_OP_RESPONSE, msg);
	memcpy(&prim->u.connect, param, sizeof(prim->u.connect));
	return &prim->oph;
}

static int sccp_sap_up(struct osmo_prim_hdr *oph, void *link)
{
	struct osmo_scu_prim *prim = (struct osmo_scu_prim *) oph;
	struct osmo_prim_hdr *resp = NULL;
	int rc;
	struct ue_conn_ctx *ue;

	printf("sccp_sap_up(%s)\n", osmo_scu_prim_name(oph));

	switch (OSMO_PRIM_HDR(oph)) {
	case OSMO_PRIM(OSMO_SCU_PRIM_N_CONNECT, PRIM_OP_CONFIRM):
		/* confirmation of outbound connection */
		rc = -1;
		break;
	case OSMO_PRIM(OSMO_SCU_PRIM_N_CONNECT, PRIM_OP_INDICATION):
		/* indication of new inbound connection request*/
		printf("N-CONNECT.ind(X->%u)\n", prim->u.connect.conn_id);
		if (/*  prim->u.connect.called_addr.ssn != OSMO_SCCP_SSN_RANAP || */
		    !msgb_l2(oph->msg) || msgb_l2len(oph->msg) == 0) {
			LOGP(DGPRS, LOGL_NOTICE, "Received invalid N-CONNECT.ind\n");
			return 0;
		}
		ue = ue_conn_ctx_alloc(link, prim->u.connect.conn_id);
		/* first ensure the local SUA/SCCP socket is ACTIVE */
		resp = make_conn_resp(&prim->u.connect);
		osmo_sua_user_link_down(link, resp);
		/* then handle the RANAP payload */
		rc = ranap_cn_rx_co(cn_ranap_handle_co, ue, msgb_l2(oph->msg), msgb_l2len(oph->msg));
		break;
	case OSMO_PRIM(OSMO_SCU_PRIM_N_DISCONNECT, PRIM_OP_INDICATION):
		/* indication of disconnect */
		printf("N-DISCONNECT.ind(%u)\n", prim->u.disconnect.conn_id);
		ue = ue_conn_ctx_find(link, prim->u.disconnect.conn_id);
		rc = ranap_cn_rx_co(cn_ranap_handle_co, ue, msgb_l2(oph->msg), msgb_l2len(oph->msg));
		break;
	case OSMO_PRIM(OSMO_SCU_PRIM_N_DATA, PRIM_OP_INDICATION):
		/* connection-oriented data received */
		printf("N-DATA.ind(%u, %s)\n", prim->u.data.conn_id,
			osmo_hexdump(msgb_l2(oph->msg), msgb_l2len(oph->msg)));
		/* resolve UE context */
		ue = ue_conn_ctx_find(link, prim->u.data.conn_id);
		rc = ranap_cn_rx_co(cn_ranap_handle_co, ue, msgb_l2(oph->msg), msgb_l2len(oph->msg));
		break;
	case OSMO_PRIM(OSMO_SCU_PRIM_N_UNITDATA, PRIM_OP_INDICATION):
		/* connection-less data received */
		printf("N-UNITDATA.ind(%s)\n", 
			osmo_hexdump(msgb_l2(oph->msg), msgb_l2len(oph->msg)));
		rc = ranap_cn_rx_cl(cn_ranap_handle_cl, link, msgb_l2(oph->msg), msgb_l2len(oph->msg));
		break;
	default:
		rc = -1;
		break;
	}

	msgb_free(oph->msg);
	return rc;
}

int iu_init(void *ctx, const char *listen_addr, uint16_t listen_port,
	    iu_recv_cb_t iu_recv_cb, iu_event_cb_t iu_event_cb)
{
	struct osmo_sua_user *user;
	talloc_iu_ctx = talloc_named_const(ctx, 1, "iu");
	talloc_asn1_ctx = talloc_named_const(talloc_iu_ctx, 1, "asn1");

	global_iu_recv_cb = iu_recv_cb;
	global_iu_event_cb = iu_event_cb;
	osmo_sua_set_log_area(DSUA);
	user = osmo_sua_user_create(talloc_iu_ctx, sccp_sap_up, talloc_iu_ctx);
	return osmo_sua_server_listen(user, listen_addr, listen_port);
}


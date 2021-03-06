/*	$KAME: ipsec_doi.c,v 1.168 2004/03/03 02:28:46 sakane Exp $	*/

/*
 * Copyright (C) 1995, 1996, 1997, and 1998 WIDE Project.
 * All rights reserved.
 * 
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the project nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 * 
 * THIS SOFTWARE IS PROVIDED BY THE PROJECT AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE PROJECT OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include <config.h>

#include <sys/types.h>
#include <sys/param.h>
#include <sys/socket.h>

#include <netinet/in.h>

#ifdef HAVE_NETINET6_IPSEC_H
# include <netinet6/ipsec.h>
#else
# ifdef HAVE_NETIPSEC_IPSEC_H
#  include <netipsec/ipsec.h>
# else
#  include <linux/ipsec.h>
# endif
#endif

#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <netdb.h>
#if TIME_WITH_SYS_TIME
# include <sys/time.h>
# include <time.h>
#else
# if HAVE_SYS_TIME_H
#  include <sys/time.h>
# else
#  include <time.h>
# endif
#endif

#include "racoon.h"

#include "var.h"
/* #include "vmbuf.h" */
/* #include "misc.h" */
#include "plog.h"
#include "debug.h"

/* #include "cfparse_proto.h" */
#include "isakmp.h"
#include "isakmp_impl.h"
#include "isakmp_var.h"
#include "ikev1_impl.h"
#include "ipsec_doi.h"
#include "dhgroup.h"
#include "oakley.h"
#include "remoteconf.h"
/* #include "localconf.h" */
#include "sockmisc.h"
#include "handler.h"
/* #include "policy.h" */
#include "algorithm.h"
/* #include "sainfo.h" */
#include "proposal.h"
#include "crypto_impl.h"
#include "strnames.h"
#include "gcmalloc.h"

#ifdef ENABLE_NATT
#include "ikev1_natt.h"
#endif
#ifdef HAVE_GSSAPI
#include "gssapi.h"
#endif

#include "ike_conf.h"

int verbose_proposal_check = 1;

static rc_vchar_t *get_ph1approval (struct ph1handle *,
					struct prop_pair **);
static struct isakmpsa *get_ph1approvalx (struct prop_pair *,
	struct isakmpsa *, struct isakmpsa *, rc_type);
static void print_ph1mismatched (struct prop_pair *, struct isakmpsa *);
static int t2isakmpsa (struct isakmp_pl_t *, struct isakmpsa *);
static int cmp_aproppair_i (struct prop_pair *, struct prop_pair *);
static struct prop_pair *get_ph2approval (struct ph2handle *,
	struct prop_pair **);
static struct prop_pair *get_ph2approvalx (struct ph2handle *,
	struct prop_pair *);
static void free_proppair0 (struct prop_pair *);

static int get_transform
	(struct isakmp_pl_p *, struct prop_pair **, int *);
static uint32_t ipsecdoi_set_ld (rc_vchar_t *);

static int check_doi (uint32_t);
static int check_situation (uint32_t);

static int check_prot_main (int);
static int check_prot_quick (int);
static int (*check_protocol[]) (int) = {
	check_prot_main,	/* IPSECDOI_TYPE_PH1 */
	check_prot_quick,	/* IPSECDOI_TYPE_PH2 */
};

static int check_spi_size (int, int);

static int check_trns_isakmp (int);
static int check_trns_ah (int);
static int check_trns_esp (int);
static int check_trns_ipcomp (int);
static int (*check_transform[]) (int) = {
	0,
	check_trns_isakmp,	/* IPSECDOI_PROTO_ISAKMP */
	check_trns_ah,		/* IPSECDOI_PROTO_IPSEC_AH */
	check_trns_esp,		/* IPSECDOI_PROTO_IPSEC_ESP */
	check_trns_ipcomp,	/* IPSECDOI_PROTO_IPCOMP */
};

static int check_attr_isakmp (struct isakmp_pl_t *);
static int check_attr_ah (struct isakmp_pl_t *);
static int check_attr_esp (struct isakmp_pl_t *);
static int check_attr_ipsec (int, struct isakmp_pl_t *);
static int check_attr_ipcomp (struct isakmp_pl_t *);
static int (*check_attributes[]) (struct isakmp_pl_t *) = {
	0,
	check_attr_isakmp,	/* IPSECDOI_PROTO_ISAKMP */
	check_attr_ah,		/* IPSECDOI_PROTO_IPSEC_AH */
	check_attr_esp,		/* IPSECDOI_PROTO_IPSEC_ESP */
	check_attr_ipcomp,	/* IPSECDOI_PROTO_IPCOMP */
};

static int setph1prop (struct isakmpsa *, caddr_t);
static int setph1trns (struct isakmpsa *, caddr_t);
static int setph1attr (struct isakmpsa *, caddr_t);
static rc_vchar_t *setph2proposal0 (const struct ph2handle *,
	const struct saprop *, const struct saproto *);

#if 0
static rc_vchar_t *getidval (int, rc_vchar_t *);
#endif

#ifdef HAVE_GSSAPI
static struct isakmpsa *fixup_initiator_sa (struct isakmpsa *,
	struct isakmpsa *);
#endif

/*%%%*/
/*
 * check phase 1 SA payload.
 * make new SA payload to be replyed not including general header.
 * the pointer to one of isakmpsa in proposal is set into iph1->approval.
 * OUT:
 *	positive: the pointer to new buffer of SA payload.
 *		  network byte order.
 *	NULL	: error occurd.
 */
int
ipsecdoi_checkph1proposal(rc_vchar_t *sa, struct ph1handle *iph1)
{
	rc_vchar_t *newsa;		/* new SA payload approved. */
	struct prop_pair **pair;

	/* get proposal pair */
	pair = get_proppair(sa, IPSECDOI_TYPE_PH1);
	if (pair == NULL)
		return -1;

	/* check and get one SA for use */
	newsa = get_ph1approval(iph1, pair);
	
	free_proppair(pair);

	if (newsa == NULL)
		return -1;

	iph1->sa_ret = newsa;

	return 0;
}

/*
 * acceptable check for remote configuration.
 * return a new SA payload to be reply to peer.
 */
static rc_vchar_t *
get_ph1approval(struct ph1handle *iph1, struct prop_pair **pair)
{
	rc_vchar_t *newsa;
	struct isakmpsa *sa, tsa;
	struct prop_pair *s, *p;
#if 0
	int prophlen;
#endif
	int i;

	if (iph1->approval) {
		delisakmpsa(iph1->approval);
		iph1->approval = NULL;
	}

	for (i = 0; i < MAXPROPPAIRLEN; i++) {
		if (pair[i] == NULL)
			continue;
		for (s = pair[i]; s; s = s->next) {
#if 0
			prophlen = sizeof(struct isakmp_pl_p)
					+ s->prop->spi_size;
#endif
			/* compare proposal and select one */
			for (p = s; p; p = p->tnext) {
				sa = get_ph1approvalx(p, iph1->proposal,
						      &tsa,
						      ikev1_proposal_check(iph1->rmconf));
				if (sa != NULL)
					goto found;
			}
		}
	}

	/*
	 * if there is no suitable proposal, racoon complains about all of
	 * mismatched items in those proposal.
	 */
	if (verbose_proposal_check) {
		for (i = 0; i < MAXPROPPAIRLEN; i++) {
			if (pair[i] == NULL)
				continue;
			for (s = pair[i]; s; s = s->next) {
#if 0
				prophlen = sizeof(struct isakmp_pl_p)
						+ s->prop->spi_size;
#endif
				for (p = s; p; p = p->tnext) {
					print_ph1mismatched(p,
							    iph1->proposal);
				}
			}
		}
	}
	plog(PLOG_PROTOERR, PLOGLOC, NULL, "no suitable proposal found.\n");

	return NULL;

found:
	plog(PLOG_DEBUG, PLOGLOC, NULL, "an acceptable proposal found.\n");

	/* check DH group settings */
	if (sa->dhgrp) {
		if (sa->dhgrp->prime && sa->dhgrp->gen1) {
			/* it's ok */
			goto saok;
		}
		plog(PLOG_PROTOWARN, PLOGLOC, 0,
			"invalid DH parameter found, use default.\n");
		oakley_dhgrp_free(sa->dhgrp);
		sa->dhgrp = NULL;
	}

	if (oakley_setdhgroup(sa->dh_group, &sa->dhgrp) == -1) {
		racoon_free(sa);
		return NULL;
	}

saok:
#ifdef HAVE_GSSAPI
	if (sa->gssid != NULL)
		plog(PLOG_DEBUG, PLOGLOC, NULL, "gss id in new sa '%s'\n",
		    sa->gssid->v);
	if (iph1-> side == INITIATOR) {
		if (iph1->proposal->gssid != NULL)
			iph1->gi_i = rc_vdup(iph1->proposal->gssid);
		if (tsa.gssid != NULL)
			iph1->gi_r = rc_vdup(tsa.gssid);
		iph1->approval = fixup_initiator_sa(sa, &tsa);
	} else {
		if (tsa.gssid != NULL) {
			iph1->gi_r = rc_vdup(tsa.gssid);
			if (iph1->proposal->gssid != NULL)
				iph1->gi_i =
				    rc_vdup(iph1->proposal->gssid);
			else
				iph1->gi_i = gssapi_get_default_id(iph1);
			if (sa->gssid == NULL && iph1->gi_i != NULL)
				sa->gssid = rc_vdup(iph1->gi_i);
		}
		iph1->approval = sa;
	}
	if (iph1->gi_i != NULL)
		plog(PLOG_DEBUG, PLOGLOC, NULL, "GIi is %*s\n",
		    iph1->gi_i->l, iph1->gi_i->v);
	if (iph1->gi_r != NULL)
		plog(PLOG_DEBUG, PLOGLOC, NULL, "GIr is %*s\n",
		    iph1->gi_r->l, iph1->gi_r->v);
#else
	iph1->approval = sa;
#endif

	newsa = get_sabyproppair(p, iph1);
	if (newsa == NULL && iph1->approval != NULL) {
		delisakmpsa(iph1->approval);
		iph1->approval = NULL;
	}

	return newsa;
}

/*
 * compare peer's single proposal and all of my proposal.
 * and select one if suiatable.
 * p       : one of peer's proposal.
 * proposal: my proposals.
 */
static struct isakmpsa *
get_ph1approvalx(struct prop_pair *p, struct isakmpsa *proposal,
    struct isakmpsa *sap, rc_type check_level)
{
	struct isakmp_pl_p *prop = p->prop;
	struct isakmp_pl_t *trns = p->trns;
	struct isakmpsa sa, *s, *tsap;

	plog(PLOG_DEBUG, PLOGLOC, NULL,
       		"prop#=%d, prot-id=%s, spi-size=%d, #trns=%d\n",
		prop->p_no, s_ipsecdoi_proto(prop->proto_id),
		prop->spi_size, prop->num_t);

	plog(PLOG_DEBUG, PLOGLOC, NULL,
		"trns#=%d, trns-id=%s\n",
		trns->t_no,
		s_ipsecdoi_trns(prop->proto_id, trns->t_id));

	tsap = sap != NULL ? sap : &sa;

	memset(tsap, 0, sizeof(*tsap));
	if (t2isakmpsa(trns, tsap) < 0)
		return NULL;
	for (s = proposal; s != NULL; s = s->next) {
		int authmethod;

#ifdef ENABLE_HYBRID
		authmethod = switch_authmethod(s->authmethod);
#else
		authmethod = s->authmethod;
#endif
		plog(PLOG_DEBUG, PLOGLOC, NULL, "Compared: DB:Peer\n");
		plog(PLOG_DEBUG, PLOGLOC, NULL, "(lifetime = %ld:%ld)\n",
			(long)s->lifetime, (long)tsap->lifetime);
		plog(PLOG_DEBUG, PLOGLOC, NULL, "(lifebyte = %zu:%zu)\n",
			s->lifebyte, tsap->lifebyte);
		plog(PLOG_DEBUG, PLOGLOC, NULL, "enctype = %s:%s\n",
			s_oakley_attr_v(OAKLEY_ATTR_ENC_ALG,
					s->enctype),
			s_oakley_attr_v(OAKLEY_ATTR_ENC_ALG,
					tsap->enctype));
		plog(PLOG_DEBUG, PLOGLOC, NULL, "(encklen = %d:%d)\n",
			s->encklen, tsap->encklen);
		plog(PLOG_DEBUG, PLOGLOC, NULL, "hashtype = %s:%s\n",
			s_oakley_attr_v(OAKLEY_ATTR_HASH_ALG,
					s->hashtype),
			s_oakley_attr_v(OAKLEY_ATTR_HASH_ALG,
					tsap->hashtype));
		plog(PLOG_DEBUG, PLOGLOC, NULL, "authmethod = %s:%s\n",
			s_oakley_attr_v(OAKLEY_ATTR_AUTH_METHOD,
					authmethod),
			s_oakley_attr_v(OAKLEY_ATTR_AUTH_METHOD,
					tsap->authmethod));
		plog(PLOG_DEBUG, PLOGLOC, NULL, "dh_group = %s:%s\n",
			s_oakley_attr_v(OAKLEY_ATTR_GRP_DESC,
					s->dh_group),
			s_oakley_attr_v(OAKLEY_ATTR_GRP_DESC,
					tsap->dh_group));
#if 0
		/* XXX to be considered ? */
		if (tsap->lifebyte > s->lifebyte) ;
#endif
		/*
		 * if responder side and peer's key length in proposal
		 * is bigger than mine, it might be accepted.
		 */
		if(tsap->enctype == s->enctype &&
		    tsap->authmethod == authmethod &&
		    tsap->hashtype == s->hashtype &&
		    tsap->dh_group == s->dh_group &&
		    tsap->encklen == s->encklen) {
			switch(check_level) {
			case RCT_PCT_OBEY:
				goto found;
				break;

			case RCT_PCT_STRICT:
				if ((s->lifetime != 0 && 
				     tsap->lifetime > s->lifetime) ||
				    (s->lifebyte != 0 &&
				     tsap->lifebyte > s->lifebyte))
					continue;
				goto found;
				break;

			case RCT_PCT_CLAIM:
				if (s->lifetime == 0 ||
				    tsap->lifetime < s->lifetime)
					s->lifetime = tsap->lifetime;
				if (s->lifebyte == 0 ||
				    tsap->lifebyte < s->lifebyte)
					s->lifebyte = tsap->lifebyte;
				goto found;
				break;

			case RCT_PCT_EXACT:
				if ((tsap->lifetime != s->lifetime) ||
				    (tsap->lifebyte != s->lifebyte))
					continue;
				goto found;
				break;

			default:
				plog(PLOG_PROTOERR, PLOGLOC, NULL, 
				    "Unexpected proposal_check value\n");
				continue;
				break;
			}
		}
	}

found:
	if (tsap->dhgrp != NULL) {
		oakley_dhgrp_free(tsap->dhgrp);
		tsap->dhgrp = NULL;
	}

	if ((s = dupisakmpsa(s)) != NULL) {
		switch(check_level) {
		case RCT_PCT_OBEY:
			s->lifetime = tsap->lifetime;
			s->lifebyte = tsap->lifebyte;
			break;

		case RCT_PCT_STRICT:
			s->lifetime = tsap->lifetime;
			s->lifebyte = tsap->lifebyte;
			break;

		case RCT_PCT_CLAIM:
			if (tsap->lifetime < s->lifetime)
				s->lifetime = tsap->lifetime;
			if (tsap->lifebyte < s->lifebyte)
				s->lifebyte = tsap->lifebyte;
			break;

		default:
			break;
		}
	}

	return s;
}

/*
 * print all of items in peer's proposal which are mismatched to my proposal.
 * p       : one of peer's proposal.
 * proposal: my proposals.
 */
static void
print_ph1mismatched(struct prop_pair *p, struct isakmpsa *proposal)
{
	struct isakmpsa sa, *s;

	memset(&sa, 0, sizeof(sa));
	if (t2isakmpsa(p->trns, &sa) < 0)
		return;
	plog(PLOG_PROTOERR, PLOGLOC, NULL,
	    "ours: enctype=%s authmethod=%s hashtype %s dh_group %s\n",
	    s_oakley_attr_v(OAKLEY_ATTR_ENC_ALG, sa.enctype),
	    s_oakley_attr_v(OAKLEY_ATTR_AUTH_METHOD, sa.authmethod),
	    s_oakley_attr_v(OAKLEY_ATTR_HASH_ALG, sa.hashtype),
	    s_oakley_attr_v(OAKLEY_ATTR_GRP_DESC, sa.dh_group));
	for (s = proposal; s ; s = s->next) {
		plog(PLOG_PROTOERR, PLOGLOC, NULL,
		    "DB(prop#%d:trns#%d):Peer(prop#%d:trns#%d) "
		    "theirs: enctype=%s authmethod=%s hashtype %s dh_group %s\n",
		    s->prop_no, s->trns_no,
		    p->prop->p_no, p->trns->t_no,
		    s_oakley_attr_v(OAKLEY_ATTR_ENC_ALG, s->enctype),
		    s_oakley_attr_v(OAKLEY_ATTR_AUTH_METHOD, s->authmethod),
		    s_oakley_attr_v(OAKLEY_ATTR_HASH_ALG, s->hashtype),
		    s_oakley_attr_v(OAKLEY_ATTR_GRP_DESC, s->dh_group));
		if (sa.enctype != s->enctype) {
			plog(PLOG_PROTOERR, PLOGLOC, NULL,
			    "rejected enctype\n");
			continue;
		}
		if (sa.authmethod != s->authmethod) {
			plog(PLOG_PROTOERR, PLOGLOC, NULL,
			    "rejected authmethod\n");
			continue;
		}
		if (sa.hashtype != s->hashtype) {
			plog(PLOG_PROTOERR, PLOGLOC, NULL,
			    "rejected hashtype\n");
			continue;
		}
		if (sa.dh_group != s->dh_group) {
			plog(PLOG_PROTOERR, PLOGLOC, NULL,
			    "rejected dh_group\n");
			continue;
		}
		plog(PLOG_PROTOERR, PLOGLOC, NULL, "ACCEPTED\n");
	}

	if (sa.dhgrp != NULL)
		oakley_dhgrp_free(sa.dhgrp);
}

/*
 * get ISAKMP data attributes
 */
static int
t2isakmpsa(struct isakmp_pl_t *trns, struct isakmpsa *sa)
{
	struct isakmp_data *d, *prev;
	int flag, type;
	int error = -1;
	int life_t;
	int keylen = 0;
	rc_vchar_t *val = NULL;
	int len, tlen;
	unsigned char *p;

	tlen = get_uint16(&trns->h.len) - sizeof(*trns);
	prev = (struct isakmp_data *)NULL;
	d = (struct isakmp_data *)(trns + 1);

	/* default */
	life_t = OAKLEY_ATTR_SA_LD_TYPE_DEFAULT;
	sa->lifetime = OAKLEY_ATTR_SA_LD_SEC_DEFAULT;
	sa->lifebyte = 0;
	sa->dhgrp = racoon_calloc(1, sizeof(struct dhgroup));
	if (!sa->dhgrp)
		goto err;

	while (tlen > 0) {

		type = get_uint16(&d->type) & ~ISAKMP_GEN_MASK;
		flag = get_uint16(&d->type) & ISAKMP_GEN_MASK;

		plog(PLOG_DEBUG, PLOGLOC, NULL,
			"type=%s, flag=0x%04x, lorv=%s\n",
			s_oakley_attr(type), flag,
			s_oakley_attr_v(type, get_uint16(&d->lorv)));

		/* get variable-sized item */
		switch (type) {
		case OAKLEY_ATTR_GRP_PI:
		case OAKLEY_ATTR_GRP_GEN_ONE:
		case OAKLEY_ATTR_GRP_GEN_TWO:
		case OAKLEY_ATTR_GRP_CURVE_A:
		case OAKLEY_ATTR_GRP_CURVE_B:
		case OAKLEY_ATTR_SA_LD:
		case OAKLEY_ATTR_GRP_ORDER:
			if (flag) {	/*TV*/
				len = 2;
				p = (unsigned char *)&d->lorv;
			} else {	/*TLV*/
				len = get_uint16(&d->lorv);
				p = (unsigned char *)(d + 1);
			}
			val = rc_vmalloc(len);
			if (!val)
				return -1;
			memcpy(val->v, p, len);
			break;

		default:
			break;
		}

		switch (type) {
		case OAKLEY_ATTR_ENC_ALG:
			sa->enctype = get_uint16(&d->lorv);
			break;

		case OAKLEY_ATTR_HASH_ALG:
			sa->hashtype = get_uint16(&d->lorv);
			break;

		case OAKLEY_ATTR_AUTH_METHOD:
			sa->authmethod = get_uint16(&d->lorv);
			break;

		case OAKLEY_ATTR_GRP_DESC:
			sa->dh_group = get_uint16(&d->lorv);
			break;

		case OAKLEY_ATTR_GRP_TYPE:
		{
			uint16_t xtype = get_uint16(&d->lorv);
			if (xtype == OAKLEY_ATTR_GRP_TYPE_MODP)
				sa->dhgrp->type = xtype;
			else
				return -1;
			break;
		}
		case OAKLEY_ATTR_GRP_PI:
			sa->dhgrp->prime = val;
			break;

		case OAKLEY_ATTR_GRP_GEN_ONE:
			rc_vfree(val);
			if (!flag)
				sa->dhgrp->gen1 = get_uint16(&d->lorv);
			else {
				uint16_t xlen = get_uint16(&d->lorv);
				sa->dhgrp->gen1 = 0;
				if (xlen > 4)
					return -1;
				memcpy(&sa->dhgrp->gen1, d + 1, xlen);
				sa->dhgrp->gen1 = ntohl(sa->dhgrp->gen1);
			}
			break;

		case OAKLEY_ATTR_GRP_GEN_TWO:
			rc_vfree(val);
			if (!flag)
				sa->dhgrp->gen2 = get_uint16(&d->lorv);
			else {
				uint16_t xlen = get_uint16(&d->lorv);
				sa->dhgrp->gen2 = 0;
				if (xlen > 4)
					return -1;
				memcpy(&sa->dhgrp->gen2, d + 1, xlen);
				sa->dhgrp->gen2 = ntohl(sa->dhgrp->gen2);
			}
			break;

		case OAKLEY_ATTR_GRP_CURVE_A:
			sa->dhgrp->curve_a = val;
			break;

		case OAKLEY_ATTR_GRP_CURVE_B:
			sa->dhgrp->curve_b = val;
			break;

		case OAKLEY_ATTR_SA_LD_TYPE:
		{
			uint16_t xtype = get_uint16(&d->lorv);
			switch (xtype) {
			case OAKLEY_ATTR_SA_LD_TYPE_SEC:
			case OAKLEY_ATTR_SA_LD_TYPE_KB:
				life_t = xtype;
				break;
			default:
				life_t = OAKLEY_ATTR_SA_LD_TYPE_DEFAULT;
				break;
			}
			break;
		}
		case OAKLEY_ATTR_SA_LD:
			if (!prev
			 || (get_uint16(&prev->type) & ~ISAKMP_GEN_MASK) !=
					OAKLEY_ATTR_SA_LD_TYPE) {
				plog(PLOG_PROTOERR, PLOGLOC, NULL,
				    "life duration must follow ltype\n");
				break;
			}

			switch (life_t) {
			case IPSECDOI_ATTR_SA_LD_TYPE_SEC:
				sa->lifetime = ipsecdoi_set_ld(val);
				rc_vfree(val);
				if (sa->lifetime == 0) {
					plog(PLOG_PROTOERR, PLOGLOC, NULL,
						"invalid life duration.\n");
					goto err;
				}
				break;
			case IPSECDOI_ATTR_SA_LD_TYPE_KB:
				sa->lifebyte = ipsecdoi_set_ld(val);
				rc_vfree(val);
				if (sa->lifetime == 0) {
					plog(PLOG_PROTOERR, PLOGLOC, NULL,
						"invalid life duration.\n");
					goto err;
				}
				break;
			default:
				rc_vfree(val);
				plog(PLOG_PROTOERR, PLOGLOC, NULL,
					"invalid life type: %d\n", life_t);
				goto err;
			}
			break;

		case OAKLEY_ATTR_KEY_LEN:
		{
			uint16_t xlen = get_uint16(&d->lorv);
			if (xlen % 8 != 0) {
				plog(PLOG_PROTOERR, PLOGLOC, NULL,
					"keylen %d: not multiple of 8\n",
					xlen);
				goto err;
			}
			sa->encklen = xlen;
			keylen++;
			break;
		}
		case OAKLEY_ATTR_PRF:
		case OAKLEY_ATTR_FIELD_SIZE:
			/* unsupported */
			break;

		case OAKLEY_ATTR_GRP_ORDER:
			sa->dhgrp->order = val;
			break;
#ifdef HAVE_GSSAPI
		case OAKLEY_ATTR_GSS_ID:
		{
			int len = get_uint16(&d->lorv);

			sa->gssid = rc_vmalloc(len);
			memcpy(sa->gssid->v, d + 1, len);
			plog(PLOG_DEBUG, PLOGLOC, NULL,
			    "received gss id '%s' (len %d)\n", sa->gssid->v,
			    sa->gssid->l);
			break;
		}
#endif

		default:
			break;
		}

		prev = d;
		if (flag) {
			tlen -= sizeof(*d);
			d = (struct isakmp_data *)((char *)d + sizeof(*d));
		} else {
			tlen -= (sizeof(*d) + get_uint16(&d->lorv));
			d = (struct isakmp_data *)((char *)d + sizeof(*d) + get_uint16(&d->lorv));
		}
	}

	/* key length must not be specified on some algorithms */
	if (keylen) {
		if (sa->enctype == OAKLEY_ATTR_ENC_ALG_DES
#ifdef HAVE_OPENSSL_IDEA_H
		 || sa->enctype == OAKLEY_ATTR_ENC_ALG_IDEA
#endif
		 || sa->enctype == OAKLEY_ATTR_ENC_ALG_3DES) {
			plog(PLOG_PROTOERR, PLOGLOC, NULL,
				"keylen must not be specified "
				"for encryption algorithm %d\n",
				sa->enctype);
			return -1;
		}
	}

	return 0;
err:
	return error;
}

/*%%%*/
/*
 * check phase 2 SA payload and select single proposal.
 * make new SA payload to be replyed not including general header.
 * This function is called by responder only.
 * OUT:
 *	0: succeed.
 *	-1: error occured.
 */
int
ipsecdoi_selectph2proposal(struct ph2handle *iph2)
{
	struct prop_pair **pair;
	struct prop_pair *ret;

	/* get proposal pair */
	pair = get_proppair(iph2->sa, IPSECDOI_TYPE_PH2);
	if (pair == NULL)
		return -1;

	/* check and select a proposal. */
	ret = get_ph2approval(iph2, pair);
	free_proppair(pair);
	if (ret == NULL)
		return -1;

	/* make a SA to be replayed. */
	/* SPI must be updated later. */
	iph2->sa_ret = get_sabyproppair(ret, iph2->ph1);
	free_proppair0(ret);
	if (iph2->sa_ret == NULL)
		return -1;

	return 0;
}

/*
 * check phase 2 SA payload returned from responder.
 * This function is called by initiator only.
 * OUT:
 *	0: valid.
 *	-1: invalid.
 */
int
ipsecdoi_checkph2proposal(struct ph2handle *iph2)
{
	struct prop_pair **rpair = NULL, **spair = NULL;
	struct prop_pair *p;
	rc_vchar_t	*old_sa;
	int i, n, num;
	int error = -1;

	/* get proposal pair of SA sent. */
	spair = get_proppair(iph2->sa, IPSECDOI_TYPE_PH2);
	if (spair == NULL) {
		plog(PLOG_PROTOERR, PLOGLOC, NULL,
			"failed to get prop pair.\n");
		goto end;
	}

	/* XXX should check the number of transform */

	/* get proposal pair of SA replyed */
	rpair = get_proppair(iph2->sa_ret, IPSECDOI_TYPE_PH2);
	if (rpair == NULL) {
		plog(PLOG_PROTOERR, PLOGLOC, NULL,
			"failed to get prop pair.\n");
		goto end;
	}

	/* check proposal is only one ? */
	n = 0;
	num = 0;
	for (i = 0; i < MAXPROPPAIRLEN; i++) {
		if (rpair[i]) {
			n = i;
			num++;
		}
	}
	if (num == 0) {
		plog(PLOG_PROTOERR, PLOGLOC, NULL,
			"no proposal received.\n");
		goto end;
	}
	if (num != 1) {
		plog(PLOG_PROTOERR, PLOGLOC, NULL,
			"some proposals received.\n");
		goto end;
	}

	if (spair[n] == NULL) {
		plog(PLOG_PROTOWARN, PLOGLOC, 0,
			"invalid proposal number:%d received.\n", i);
	}
	

	if (rpair[n]->tnext != NULL) {
		plog(PLOG_PROTOERR, PLOGLOC, NULL,
			"multi transforms replyed.\n");
		goto end;
	}

	if (cmp_aproppair_i(rpair[n], spair[n])) {
		plog(PLOG_PROTOERR, PLOGLOC, NULL,
			"proposal mismathed.\n");
		goto end;
	}

	/*
	 * check and select a proposal.
	 * ensure that there is no modification of the proposal by
	 * cmp_aproppair_i()
	 */
	p = get_ph2approval(iph2, rpair);
	if (p == NULL)
		goto end;

	/* make a SA to be replayed. */
	old_sa = iph2->sa_ret;	/* since p->prop points inside iph2->sa_ret */
	iph2->sa_ret = get_sabyproppair(p, iph2->ph1);
	rc_vfree(old_sa);
	free_proppair0(p);
	if (iph2->sa_ret == NULL)
		goto end;

	error = 0;

end:
	if (rpair)
		free_proppair(rpair);
	if (spair)
		free_proppair(spair);

	return error;
}

/*
 * compare two prop_pair which is assumed to have same proposal number.
 * the case of bundle or single SA, NOT multi transforms.
 * a: a proposal that is multi protocols and single transform, usually replyed.
 * b: a proposal that is multi protocols and multi transform, usually sent.
 * NOTE: this function is for initiator.
 * OUT
 *	0: equal
 *	1: not equal
 * XXX cannot understand the comment!
 */
static int
cmp_aproppair_i(struct prop_pair *a, struct prop_pair *b)
{
	struct prop_pair *p, *q, *r;
	int len;

	for (p = a, q = b; p && q; p = p->next, q = q->next) {
		for (r = q; r; r = r->tnext) {
			/* compare trns */
			if (p->trns->t_no == r->trns->t_no)
				break;
		}
		if (!r) {
			/* no suitable transform found */
			plog(PLOG_PROTOERR, PLOGLOC, NULL,
				"no suitable transform found.\n");
			return -1;
		}

		/* compare prop */
		if (p->prop->p_no != r->prop->p_no) {
			plog(PLOG_PROTOWARN, PLOGLOC, 0,
				"proposal #%d mismatched, "
				"expected #%d.\n",
				r->prop->p_no, p->prop->p_no);
			/*FALLTHROUGH*/
		}

		if (p->prop->proto_id != r->prop->proto_id) {
			plog(PLOG_PROTOERR, PLOGLOC, NULL,
				"proto_id mismathed: my:%d peer:%d\n",
				r->prop->proto_id, p->prop->proto_id);
			return -1;
		}

		if (p->prop->spi_size != r->prop->spi_size) {
			plog(PLOG_PROTOERR, PLOGLOC, NULL,
				"invalid spi size: %d.\n",
				p->prop->spi_size);
			return -1;
		}

		/* check #of transforms */
		if (p->prop->num_t != 1) {
			plog(PLOG_PROTOWARN, PLOGLOC, 0,
				"#of transform is %d, "
				"but expected 1.\n", p->prop->num_t);
			/*FALLTHROUGH*/
		}

		if (p->trns->t_id != r->trns->t_id) {
			plog(PLOG_PROTOWARN, PLOGLOC, 0,
				"transform number has been modified.\n");
			/*FALLTHROUGH*/
		}
		if (p->trns->reserved != r->trns->reserved) {
			plog(PLOG_PROTOWARN, PLOGLOC, 0,
				"reserved field should be zero.\n");
			/*FALLTHROUGH*/
		}

		/* compare attribute */
		len = get_uint16(&r->trns->h.len) - sizeof(*p->trns);
		if (memcmp(p->trns + 1, r->trns + 1, len) != 0) {
			plog(PLOG_PROTOWARN, PLOGLOC, 0,
				"attribute has been modified.\n");
			/*FALLTHROUGH*/
		}
	}
	if ((p && !q) || (!p && q)) {
		/* # of protocols mismatched */
		plog(PLOG_PROTOERR, PLOGLOC, NULL,
			"#of protocols mismatched.\n");
		return -1;
	}

	return 0;
}

/*
 * acceptable check for policy configuration.
 * return a new SA payload to be reply to peer.
 */
static struct prop_pair *
get_ph2approval(struct ph2handle *iph2, struct prop_pair **pair)
{
	struct prop_pair *ret;
	int i;

	iph2->approval = NULL;

	plog(PLOG_DEBUG, PLOGLOC, NULL,
		"begin compare proposals.\n");

	for (i = 0; i < MAXPROPPAIRLEN; i++) {
		if (pair[i] == NULL)
			continue;
		plog(PLOG_DEBUG, PLOGLOC, NULL,
			"pair[%d]: %p\n", i, pair[i]);
		print_proppair(PLOG_DEBUG, pair[i]);;

		/* compare proposal and select one */
		ret = get_ph2approvalx(iph2, pair[i]);
		if (ret != NULL) {
			/* found */
			return ret;
		}
	}

	plog(PLOG_PROTOERR, PLOGLOC, NULL, "no suitable policy found.\n");

	return NULL;
}

/*
 * compare my proposal and peers just one proposal.
 * set a approval.
 */
static struct prop_pair *
get_ph2approvalx(struct ph2handle *iph2, struct prop_pair *pp)
{
	struct prop_pair *ret = NULL;
	struct saprop *pr0, *pr = NULL;
	struct saprop *q1, *q2;

	pr0 = aproppair2saprop(pp);
	if (pr0 == NULL)
		return NULL;

	for (q1 = pr0; q1; q1 = q1->next) {
		for (q2 = iph2->proposal; q2; q2 = q2->next) {
			plog(PLOG_DEBUG, PLOGLOC, NULL,
				"peer's single bundle:\n");
			printsaprop0(PLOG_DEBUG, q1);
			plog(PLOG_DEBUG, PLOGLOC, NULL,
				"my single bundle:\n");
			printsaprop0(PLOG_DEBUG, q2);

			pr = cmpsaprop_alloc(iph2->ph1, q1, q2, iph2->side);
			if (pr != NULL)
				goto found;

			plog(PLOG_PROTOERR, PLOGLOC, NULL,
				"not matched\n");
		}
	}
	/* no proposal matching */
err:
	flushsaprop(pr0);
	return NULL;

found:
	flushsaprop(pr0);
	plog(PLOG_DEBUG, PLOGLOC, NULL, "matched\n");
	iph2->approval = pr;

    {
	struct saproto *sp;
	struct prop_pair *p, *n, *x;

	ret = NULL;

	for (p = pp; p; p = p->next) {
		/*
		 * find a proposal with matching proto_id.
		 * we have analyzed validity already, in cmpsaprop_alloc().
		 */
		for (sp = pr->head; sp; sp = sp->next) {
			if (sp->proto_id == p->prop->proto_id)
				break;
		}
		if (!sp)
			goto err;
		if (sp->head->next)
			goto err;	/* XXX */

		for (x = p; x; x = x->tnext)
			if (sp->head->trns_no == x->trns->t_no)
				break;
		if (!x)
			goto err;	/* XXX */

		n = racoon_calloc(1, sizeof(struct prop_pair));
		if (!n) {
			plog(PLOG_PROTOERR, PLOGLOC, NULL,
				"failed to get buffer.\n");
			goto err;
		}

		n->prop = x->prop;
		n->trns = x->trns;

		/* need to preserve the order */
		for (x = ret; x && x->next; x = x->next)
			;
		if (x && x->prop == n->prop) {
			for (/*nothing*/; x && x->tnext; x = x->tnext)
				;
			x->tnext = n;
		} else {
			if (x)
				x->next = n;
			else {
				ret = n;
			}
		}

		/* #of transforms should be updated ? */
	}
    }

	return ret;
}

void
free_proppair(struct prop_pair **pair)
{
	int i;

	for (i = 0; i < MAXPROPPAIRLEN; i++) {
		free_proppair0(pair[i]);
		pair[i] = NULL;
	}
	racoon_free(pair);
}

static void
free_proppair0(struct prop_pair *pair)
{
	struct prop_pair *p, *q, *r, *s;

	for (p = pair; p; p = q) {
		q = p->next;
		for (r = p; r; r = s) {
			s = r->tnext;
			racoon_free(r);
		}
	}
}

/*
 * get proposal pairs from SA payload.
 * tiny check for proposal payload.
 */
struct prop_pair **
get_proppair(rc_vchar_t *sa, int mode)
{
	struct prop_pair **pair = NULL;
	int num_p = 0;			/* number of proposal for use */
	int tlen;
	caddr_t bp;
	int i;
	struct ipsecdoi_sa_b *sab = (struct ipsecdoi_sa_b *)sa->v;

	plog(PLOG_DEBUG, PLOGLOC, NULL, "total SA len=%zu\n", sa->l);
	plogdump(PLOG_DEBUG, PLOGLOC, 0, sa->v, sa->l);

	/* check SA payload size */
	if (sa->l < sizeof(*sab)) {
		plog(PLOG_PROTOERR, PLOGLOC, NULL,
			"Invalid SA length = %zu.\n", sa->l);
		return NULL;
	}

	/* check DOI */
	if (check_doi(get_uint32(&sab->doi)) < 0)
		return NULL;

	/* check SITUATION */
	if (check_situation(get_uint32(&sab->sit)) < 0)
		return NULL;

	pair = racoon_calloc(1, MAXPROPPAIRLEN * sizeof(*pair));
	if (pair == NULL) {
		plog(PLOG_PROTOERR, PLOGLOC, NULL,
			"failed to get buffer.\n");
		return NULL;
	}
	memset(pair, 0, sizeof(*pair));

	bp = (caddr_t)(sab + 1);
	tlen = sa->l - sizeof(*sab);

    {
	struct isakmp_pl_p *prop;
	int proplen;
	rc_vchar_t *pbuf = NULL;
	struct isakmp_parse_t *pa;

	pbuf = isakmp_parsewoh(ISAKMP_NPTYPE_P, (struct isakmp_gen *)bp, tlen);
	if (pbuf == NULL)
		goto bad;

	for (pa = (struct isakmp_parse_t *)pbuf->v;
	     pa->type != ISAKMP_NPTYPE_NONE;
	     pa++) {
		/* check the value of next payload */
		if (pa->type != ISAKMP_NPTYPE_P) {
			plog(PLOG_PROTOERR, PLOGLOC, NULL,
				"Invalid payload type=%u\n", pa->type);
			rc_vfree(pbuf);
			goto bad;
		}

		prop = (struct isakmp_pl_p *)pa->ptr;
		proplen = pa->len;

		plog(PLOG_DEBUG, PLOGLOC, NULL,
			"proposal #%u len=%d\n", prop->p_no, proplen);

		if (proplen == 0) {
			plog(PLOG_PROTOERR, PLOGLOC, NULL,
				"invalid proposal with length %d\n", proplen);
			rc_vfree(pbuf);
			goto bad;
		}

		/* check Protocol ID */
		if (!check_protocol[mode]) {
			plog(PLOG_PROTOERR, PLOGLOC, NULL,
				"unsupported mode %d\n", mode);
			continue;
		}

		if (check_protocol[mode](prop->proto_id) < 0)
			continue;

		/* check SPI length when IKE. */
		if (check_spi_size(prop->proto_id, prop->spi_size) < 0)
			continue;

		/* get transform */
		if (get_transform(prop, pair, &num_p) < 0) {
			rc_vfree(pbuf);
			goto bad;
		}
	}
	rc_vfree(pbuf);
	pbuf = NULL;
    }

    {
	int notrans, nprop;
	struct prop_pair *p, *q;

	/* check for proposals with no transforms */
	for (i = 0; i < MAXPROPPAIRLEN; i++) {
		if (!pair[i])
			continue;

		plog(PLOG_DEBUG, PLOGLOC, NULL, "pair %d:\n", i);
		print_proppair(PLOG_DEBUG, pair[i]);

		notrans = nprop = 0;
		for (p = pair[i]; p; p = p->next) {
			if (p->trns == NULL) {
				notrans++;
				break;
			}
			for (q = p; q; q = q->tnext)
				nprop++;
		}

#if 0
		/*
		 * XXX at this moment, we cannot accept proposal group
		 * with multiple proposals.  this should be fixed.
		 */
		if (pair[i]->next) {
			plog(LLV_WARNING, PLOGLOC, NULL,
				"proposal #%u ignored "
				"(multiple proposal not supported)\n",
				pair[i]->prop->p_no);
			notrans++;
		}
#endif

		if (notrans) {
			for (p = pair[i]; p; p = q) {
				q = p->next;
				racoon_free(p);
			}
			pair[i] = NULL;
			num_p--;
		} else {
			plog(PLOG_DEBUG, PLOGLOC, NULL,
				"proposal #%u: %d transform\n",
				pair[i]->prop->p_no, nprop);
		}
	}
    }

	/* bark if no proposal is found. */
	if (num_p <= 0) {
		plog(PLOG_PROTOERR, PLOGLOC, NULL,
			"no Proposal found.\n");
		goto bad;
	}

	return pair;

  bad:
	if (pair)
		racoon_free(pair);
	return NULL;
}

/*
 * check transform payload.
 * OUT:
 *	positive: return the pointer to the payload of valid transform.
 *	0	: No valid transform found.
 */
static int
get_transform(struct isakmp_pl_p *prop, struct prop_pair **pair, int *num_p)
{
	int tlen; /* total length of all transform in a proposal */
	caddr_t bp;
	struct isakmp_pl_t *trns;
	int trnslen;
	rc_vchar_t *pbuf = NULL;
	struct isakmp_parse_t *pa;
	struct prop_pair *p = NULL, *q;
	int num_t;

	bp = (caddr_t)prop + sizeof(struct isakmp_pl_p) + prop->spi_size;
	tlen = get_uint16(&prop->h.len)
		- (sizeof(struct isakmp_pl_p) + prop->spi_size);
	pbuf = isakmp_parsewoh(ISAKMP_NPTYPE_T, (struct isakmp_gen *)bp, tlen);
	if (pbuf == NULL)
		return -1;

	/* check and get transform for use */
	num_t = 0;
	for (pa = (struct isakmp_parse_t *)pbuf->v;
	     pa->type != ISAKMP_NPTYPE_NONE;
	     pa++) {

		num_t++;

		/* check the value of next payload */
		if (pa->type != ISAKMP_NPTYPE_T) {
			plog(PLOG_PROTOERR, PLOGLOC, NULL,
				"Invalid payload type=%u\n", pa->type);
			break;
		}

		trns = (struct isakmp_pl_t *)pa->ptr;
		trnslen = pa->len;

		plog(PLOG_DEBUG, PLOGLOC, NULL,
			"transform #%u len=%u\n", trns->t_no, trnslen);

		/* check transform ID */
		if (prop->proto_id >= ARRAYLEN(check_transform)) {
			plog(PLOG_PROTOWARN, PLOGLOC, 0,
				"unsupported proto_id %u\n",
				prop->proto_id);
			continue;
		}
		if (prop->proto_id >= ARRAYLEN(check_attributes)) {
			plog(PLOG_PROTOWARN, PLOGLOC, 0,
				"unsupported proto_id %u\n",
				prop->proto_id);
			continue;
		}

		if (!check_transform[prop->proto_id]
		 || !check_attributes[prop->proto_id]) {
			plog(PLOG_PROTOWARN, PLOGLOC, 0,
				"unsupported proto_id %u\n",
				prop->proto_id);
			continue;
		}
		if (check_transform[prop->proto_id](trns->t_id) < 0)
			continue;

		/* check data attributes */
		if (check_attributes[prop->proto_id](trns) != 0)
			continue;

		p = racoon_calloc(1, sizeof(*p));
		if (p == NULL) {
			plog(PLOG_PROTOERR, PLOGLOC, NULL,
				"failed to get buffer.\n");
			rc_vfree(pbuf);
			return -1;
		}
		p->prop = prop;
		p->trns = trns;

		/* need to preserve the order */
		for (q = pair[prop->p_no]; q && q->next; q = q->next)
			;
		if (q && q->prop == p->prop) {
			for (/*nothing*/; q && q->tnext; q = q->tnext)
				;
			q->tnext = p;
		} else {
			if (q)
				q->next = p;
			else {
				pair[prop->p_no] = p;
				(*num_p)++;
			}
		}
	}

	rc_vfree(pbuf);

	return 0;
}

/*
 * make a new SA payload from prop_pair.
 * NOTE: this function make spi value clear.
 */
rc_vchar_t *
get_sabyproppair(struct prop_pair *pair, struct ph1handle *iph1)
{
	rc_vchar_t *newsa;
	int newtlen;
	uint8_t *np_p = NULL;
	struct prop_pair *p;
	int prophlen, trnslen;
	caddr_t bp;

	newtlen = sizeof(struct ipsecdoi_sa_b);
	for (p = pair; p; p = p->next) {
		newtlen += (sizeof(struct isakmp_pl_p)
				+ p->prop->spi_size
				+ get_uint16(&p->trns->h.len));
	}

	newsa = rc_vmalloc(newtlen);
	if (newsa == NULL) {
		plog(PLOG_PROTOERR, PLOGLOC, NULL, "failed to get newsa.\n");
		return NULL;
	}
	bp = newsa->v;

	put_uint16(&((struct isakmp_gen *)bp)->len, newtlen);

	/* update some of values in SA header */
	put_uint32(&((struct ipsecdoi_sa_b *)bp)->doi, ikev1_doitype(iph1->rmconf));
	put_uint32(&((struct ipsecdoi_sa_b *)bp)->sit, ikev1_sittype(iph1->rmconf));
	bp += sizeof(struct ipsecdoi_sa_b);

	/* create proposal payloads */
	for (p = pair; p; p = p->next) {
		prophlen = sizeof(struct isakmp_pl_p)
				+ p->prop->spi_size;
		trnslen = get_uint16(&p->trns->h.len);

		if (np_p)
			*np_p = ISAKMP_NPTYPE_P;

		/* create proposal */

		memcpy(bp, p->prop, prophlen);
		((struct isakmp_pl_p *)bp)->h.np = ISAKMP_NPTYPE_NONE;
		put_uint16(&((struct isakmp_pl_p *)bp)->h.len, prophlen + trnslen);
		((struct isakmp_pl_p *)bp)->num_t = 1;
		np_p = &((struct isakmp_pl_p *)bp)->h.np;
		memset(bp + sizeof(struct isakmp_pl_p), 0, p->prop->spi_size);
		bp += prophlen;

		/* create transform */
		memcpy(bp, p->trns, trnslen);
		((struct isakmp_pl_t *)bp)->h.np = ISAKMP_NPTYPE_NONE;
		put_uint16(&((struct isakmp_pl_t *)bp)->h.len, trnslen);
		bp += trnslen;
	}

	return newsa;
}

/*
 * update responder's spi
 */
int
ipsecdoi_updatespi(struct ph2handle *iph2)
{
	struct prop_pair **pair, *p;
	struct saprop *pp;
	struct saproto *pr;
	int i;
	int error = -1;
	uint8_t *spi;

	pair = get_proppair(iph2->sa_ret, IPSECDOI_TYPE_PH2);
	if (pair == NULL)
		return -1;
	for (i = 0; i < MAXPROPPAIRLEN; i++) {
		if (pair[i])
			break;
	}
	if (i == MAXPROPPAIRLEN || pair[i]->tnext) {
		/* multiple transform must be filtered by selectph2proposal.*/
		goto end;
	}

	pp = iph2->approval;

	/* create proposal payloads */
	for (p = pair[i]; p; p = p->next) {
		/*
		 * find a proposal/transform with matching proto_id/t_id.
		 * we have analyzed validity already, in cmpsaprop_alloc().
		 */
		for (pr = pp->head; pr; pr = pr->next) {
			if (p->prop->proto_id == pr->proto_id &&
			    p->trns->t_id == pr->head->trns_id) {
				break;
			}
		}
		if (!pr)
			goto end;

		/*
		 * XXX SPI bits are left-filled, for use with IPComp.
		 * we should be switching to variable-length spi field...
		 */
		spi = (uint8_t *)&pr->spi;
		spi += sizeof(pr->spi);
		spi -= pr->spisize;
		memcpy((caddr_t)p->prop + sizeof(*p->prop), spi, pr->spisize);
	}

	error = 0;
end:
	free_proppair(pair);
	return error;
}

/*
 * make a new SA payload from prop_pair.
 */
rc_vchar_t *
get_sabysaprop(struct saprop *pp0, rc_vchar_t *sa0)
{
	struct prop_pair **pair;
	rc_vchar_t *newsa = NULL;
	int newtlen;
	uint8_t *np_p = NULL;
	struct prop_pair *p = NULL;
	struct saprop *pp;
	struct saproto *pr;
	struct satrns *tr;
	int prophlen, trnslen;
	caddr_t bp;
	int error = -1;

	/* get proposal pair */
	pair = get_proppair(sa0, IPSECDOI_TYPE_PH2);
	if (pair == NULL)
		return NULL;

	newtlen = sizeof(struct ipsecdoi_sa_b);
	for (pp = pp0; pp; pp = pp->next) {

		if (pair[pp->prop_no] == NULL)
			goto out;

		for (pr = pp->head; pr; pr = pr->next) {
			newtlen += (sizeof(struct isakmp_pl_p)
				+ pr->spisize);

			for (tr = pr->head; tr; tr = tr->next) {
				for (p = pair[pp->prop_no]; p; p = p->tnext) {
					if (tr->trns_no == p->trns->t_no)
						break;
				}
				if (p == NULL)
					goto out;

				newtlen += get_uint16(&p->trns->h.len);
			}
		}
	}

	newsa = rc_vmalloc(newtlen);
	if (newsa == NULL) {
		plog(PLOG_PROTOERR, PLOGLOC, NULL, "failed to get newsa.\n");
		goto out;
	}
	bp = newsa->v;

	/* some of values of SA must be updated in the out of this function */
	put_uint16(&((struct isakmp_gen *)bp)->len, newtlen);
	bp += sizeof(struct ipsecdoi_sa_b);

	/* create proposal payloads */
	for (pp = pp0; pp; pp = pp->next) {

		for (pr = pp->head; pr; pr = pr->next) {
			prophlen = sizeof(struct isakmp_pl_p)
					+ p->prop->spi_size;

			for (tr = pr->head; tr; tr = tr->next) {
				for (p = pair[pp->prop_no]; p; p = p->tnext) {
					if (tr->trns_no == p->trns->t_no)
						break;
				}
				if (p == NULL)
					goto out;

				trnslen = get_uint16(&p->trns->h.len);

				if (np_p)
					*np_p = ISAKMP_NPTYPE_P;

				/* create proposal */

				memcpy(bp, p->prop, prophlen);
				((struct isakmp_pl_p *)bp)->h.np = ISAKMP_NPTYPE_NONE;
				put_uint16(&((struct isakmp_pl_p *)bp)->h.len, prophlen + trnslen);
				((struct isakmp_pl_p *)bp)->num_t = 1;
				np_p = &((struct isakmp_pl_p *)bp)->h.np;
				bp += prophlen;

				/* create transform */
				memcpy(bp, p->trns, trnslen);
				((struct isakmp_pl_t *)bp)->h.np = ISAKMP_NPTYPE_NONE;
				put_uint16(&((struct isakmp_pl_t *)bp)->h.len, trnslen);
				bp += trnslen;
			}
		}
	}

	error = 0;
  out:
	if (pair != NULL)
		racoon_free(pair);

	if (error != 0) {
		if (newsa)
			rc_vfree(newsa);
		newsa = NULL;
	}

	return newsa;
}

/*
 * If some error happens then return 0.  Although 0 means that lifetime is zero,
 * such a value should not be accepted.
 * Also 0 of lifebyte should not be included in a packet although 0 means not
 * to care of it.
 */
static uint32_t
ipsecdoi_set_ld(rc_vchar_t *buf)
{
	uint32_t ld;

	if (buf == 0)
		return 0;

	switch (buf->l) {
	case 2:
		ld = get_uint16((uint16_t *)buf->v);
		break;
	case 4:
		ld = get_uint32((uint32_t *)buf->v);
		break;
	default:
		plog(PLOG_PROTOERR, PLOGLOC, NULL,
			"length %zu of life duration "
			"isn't supported.\n", buf->l);
		return 0;
	}

	return ld;
}

/*%%%*/
/*
 * check DOI
 */
static int
check_doi(uint32_t doi)
{
	switch (doi) {
	case IPSEC_DOI:
		return 0;
	default:
		plog(PLOG_PROTOERR, PLOGLOC, NULL,
			"invalid value of DOI 0x%08x.\n", doi);
		return -1;
	}
	/* NOT REACHED */
}

/*
 * check situation
 */
static int
check_situation(uint32_t sit)
{
	switch (sit) {
	case IPSECDOI_SIT_IDENTITY_ONLY:
		return 0;

	case IPSECDOI_SIT_SECRECY:
	case IPSECDOI_SIT_INTEGRITY:
		plog(PLOG_PROTOERR, PLOGLOC, NULL,
			"situation 0x%08x unsupported yet.\n", sit);
		return -1;

	default:
		plog(PLOG_PROTOERR, PLOGLOC, NULL,
			"invalid situation 0x%08x.\n", sit);
		return -1;
	}
	/* NOT REACHED */
}

/*
 * check protocol id in main mode
 */
static int
check_prot_main(int proto_id)
{
	switch (proto_id) {
	case IPSECDOI_PROTO_ISAKMP:
		return 0;

	default:
		plog(PLOG_PROTOERR, PLOGLOC, NULL,
			"Illegal protocol id=%u.\n", proto_id);
		return -1;
	}
	/* NOT REACHED */
}

/*
 * check protocol id in quick mode
 */
static int
check_prot_quick(int proto_id)
{
	switch (proto_id) {
	case IPSECDOI_PROTO_IPSEC_AH:
	case IPSECDOI_PROTO_IPSEC_ESP:
		return 0;

	case IPSECDOI_PROTO_IPCOMP:
		return 0;

	default:
		plog(PLOG_PROTOERR, PLOGLOC, NULL,
			"invalid protocol id %d.\n", proto_id);
		return -1;
	}
	/* NOT REACHED */
}

static int
check_spi_size(int proto_id, int size)
{
	switch (proto_id) {
	case IPSECDOI_PROTO_ISAKMP:
		if (size != 0) {
			/* WARNING */
			plog(PLOG_DEBUG, PLOGLOC, NULL,
				"SPI size isn't zero, but IKE proposal.\n");
		}
		return 0;

	case IPSECDOI_PROTO_IPSEC_AH:
	case IPSECDOI_PROTO_IPSEC_ESP:
		if (size != 4) {
			plog(PLOG_PROTOERR, PLOGLOC, NULL,
				"invalid SPI size=%d for IPSEC proposal.\n",
				size);
			return -1;
		}
		return 0;

	case IPSECDOI_PROTO_IPCOMP:
		if (size != 2 && size != 4) {
			plog(PLOG_PROTOERR, PLOGLOC, NULL,
				"invalid SPI size=%d for IPCOMP proposal.\n",
				size);
			return -1;
		}
		return 0;

	default:
		/* ??? */
		return -1;
	}
	/* NOT REACHED */
}

/*
 * check transform ID in ISAKMP.
 */
static int
check_trns_isakmp(int t_id)
{
	switch (t_id) {
	case IPSECDOI_KEY_IKE:
		return 0;
	default:
		plog(PLOG_PROTOERR, PLOGLOC, NULL,
			"invalid transform-id=%u in proto_id=%u.\n",
			t_id, IPSECDOI_KEY_IKE);
		return -1;
	}
	/* NOT REACHED */
}

/*
 * check transform ID in AH.
 */
static int
check_trns_ah(int t_id)
{
	switch (t_id) {
	case IPSECDOI_AH_MD5:
	case IPSECDOI_AH_SHA:
	case IPSECDOI_AH_SHA256:
	case IPSECDOI_AH_SHA384:
	case IPSECDOI_AH_SHA512:
		return 0;
	case IPSECDOI_AH_DES:
		plog(PLOG_PROTOERR, PLOGLOC, NULL,
			"not support transform-id=%u in AH.\n", t_id);
		return -1;
	default:
		plog(PLOG_PROTOERR, PLOGLOC, NULL,
			"invalid transform-id=%u in AH.\n", t_id);
		return -1;
	}
	/* NOT REACHED */
}

/*
 * check transform ID in ESP.
 */
static int
check_trns_esp(int t_id)
{
	switch (t_id) {
	case IPSECDOI_ESP_DES:
	case IPSECDOI_ESP_3DES:
	case IPSECDOI_ESP_NULL:
	case IPSECDOI_ESP_RC5:
	case IPSECDOI_ESP_CAST:
	case IPSECDOI_ESP_BLOWFISH:
	case IPSECDOI_ESP_AES:
	case IPSECDOI_ESP_TWOFISH:
		return 0;
	case IPSECDOI_ESP_DES_IV32:
	case IPSECDOI_ESP_DES_IV64:
	case IPSECDOI_ESP_IDEA:
	case IPSECDOI_ESP_3IDEA:
	case IPSECDOI_ESP_RC4:
		plog(PLOG_PROTOERR, PLOGLOC, NULL,
			"not support transform-id=%u in ESP.\n", t_id);
		return -1;
	default:
		plog(PLOG_PROTOERR, PLOGLOC, NULL,
			"invalid transform-id=%u in ESP.\n", t_id);
		return -1;
	}
	/* NOT REACHED */
}

/*
 * check transform ID in IPCOMP.
 */
static int
check_trns_ipcomp(int t_id)
{
	switch (t_id) {
	case IPSECDOI_IPCOMP_OUI:
	case IPSECDOI_IPCOMP_DEFLATE:
	case IPSECDOI_IPCOMP_LZS:
		return 0;
	default:
		plog(PLOG_PROTOERR, PLOGLOC, NULL,
			"invalid transform-id=%u in IPCOMP.\n", t_id);
		return -1;
	}
	/* NOT REACHED */
}

/*
 * check data attributes in IKE.
 */
static int
check_attr_isakmp(struct isakmp_pl_t *trns)
{
	struct isakmp_data *d;
	int tlen;
	int flag, type;
	uint16_t lorv;

	tlen = get_uint16(&trns->h.len) - sizeof(struct isakmp_pl_t);
	d = (struct isakmp_data *)((caddr_t)trns + sizeof(struct isakmp_pl_t));

	while (tlen > 0) {
		type = get_uint16(&d->type) & ~ISAKMP_GEN_MASK;
		flag = get_uint16(&d->type) & ISAKMP_GEN_MASK;
		lorv = get_uint16(&d->lorv);

		plog(PLOG_DEBUG, PLOGLOC, NULL,
			"type=%s, flag=0x%04x, lorv=%s\n",
			s_oakley_attr(type), flag,
			s_oakley_attr_v(type, lorv));

		/*
		 * some of the attributes must be encoded in TV.
		 * see RFC2409 Appendix A "Attribute Classes".
		 */
		switch (type) {
		case OAKLEY_ATTR_ENC_ALG:
		case OAKLEY_ATTR_HASH_ALG:
		case OAKLEY_ATTR_AUTH_METHOD:
		case OAKLEY_ATTR_GRP_DESC:
		case OAKLEY_ATTR_GRP_TYPE:
		case OAKLEY_ATTR_SA_LD_TYPE:
		case OAKLEY_ATTR_PRF:
		case OAKLEY_ATTR_KEY_LEN:
		case OAKLEY_ATTR_FIELD_SIZE:
			if (!flag) {	/* TLV*/
				plog(PLOG_PROTOERR, PLOGLOC, NULL,
					"oakley attribute %d must be TV.\n",
					type);
				return -1;
			}
			break;
		}

		/* sanity check for TLV.  length must be specified. */
		if (!flag && lorv == 0) {	/*TLV*/
			plog(PLOG_PROTOERR, PLOGLOC, NULL,
				"invalid length %d for TLV attribute %d.\n",
				lorv, type);
			return -1;
		}

		switch (type) {
		case OAKLEY_ATTR_ENC_ALG:
			if (!alg_oakley_encdef_ok(lorv)) {
				plog(PLOG_PROTOERR, PLOGLOC, NULL,
					"invalied encryption algorithm=%d.\n",
					lorv);
				return -1;
			}
			break;

		case OAKLEY_ATTR_HASH_ALG:
			if (!alg_oakley_hashdef_ok(lorv)) {
				plog(PLOG_PROTOERR, PLOGLOC, NULL,
					"invalied hash algorithm=%d.\n",
					lorv);
				return -1;
			}
			break;

		case OAKLEY_ATTR_AUTH_METHOD:
			switch (lorv) {
			case OAKLEY_ATTR_AUTH_METHOD_PSKEY:
			case OAKLEY_ATTR_AUTH_METHOD_RSASIG:
			case OAKLEY_ATTR_AUTH_METHOD_GSSAPI_KRB:
				break;
			case OAKLEY_ATTR_AUTH_METHOD_DSSSIG:
			case OAKLEY_ATTR_AUTH_METHOD_RSAENC:
			case OAKLEY_ATTR_AUTH_METHOD_RSAREV:
				plog(PLOG_PROTOERR, PLOGLOC, NULL,
					"auth method %d isn't supported.\n",
					lorv);
				return -1;
			default:
				plog(PLOG_PROTOERR, PLOGLOC, NULL,
					"invalid auth method %d.\n",
					lorv);
				return -1;
			}
			break;

		case OAKLEY_ATTR_GRP_DESC:
			if (!alg_oakley_dhdef_ok(lorv)) {
				plog(PLOG_PROTOERR, PLOGLOC, NULL,
					"invalid DH group %d.\n",
					lorv);
				return -1;
			}
			break;

		case OAKLEY_ATTR_GRP_TYPE:
			switch (lorv) {
			case OAKLEY_ATTR_GRP_TYPE_MODP:
				break;
			default:
				plog(PLOG_PROTOERR, PLOGLOC, NULL,
					"unsupported DH group type %d.\n",
					lorv);
				return -1;
			}
			break;

		case OAKLEY_ATTR_GRP_PI:
		case OAKLEY_ATTR_GRP_GEN_ONE:
			/* sanity checks? */
			break;

		case OAKLEY_ATTR_GRP_GEN_TWO:
		case OAKLEY_ATTR_GRP_CURVE_A:
		case OAKLEY_ATTR_GRP_CURVE_B:
			plog(PLOG_PROTOERR, PLOGLOC, NULL,
				"attr type=%u isn't supported.\n", type);
			return -1;

		case OAKLEY_ATTR_SA_LD_TYPE:
			switch (lorv) {
			case OAKLEY_ATTR_SA_LD_TYPE_SEC:
			case OAKLEY_ATTR_SA_LD_TYPE_KB:
				break;
			default:
				plog(PLOG_PROTOERR, PLOGLOC, NULL,
					"invalid life type %d.\n", lorv);
				return -1;
			}
			break;

		case OAKLEY_ATTR_SA_LD:
			/* should check the value */
			break;

		case OAKLEY_ATTR_PRF:
		case OAKLEY_ATTR_KEY_LEN:
			break;

		case OAKLEY_ATTR_FIELD_SIZE:
			plog(PLOG_PROTOERR, PLOGLOC, NULL,
				"attr type=%u isn't supported.\n", type);
			return -1;

		case OAKLEY_ATTR_GRP_ORDER:
			break;

		case OAKLEY_ATTR_GSS_ID:
			break;

		default:
			plog(PLOG_PROTOERR, PLOGLOC, NULL,
				"invalid attribute type %d.\n", type);
			return -1;
		}

		if (flag) {
			tlen -= sizeof(*d);
			d = (struct isakmp_data *)((char *)d
				+ sizeof(*d));
		} else {
			tlen -= (sizeof(*d) + lorv);
			d = (struct isakmp_data *)((char *)d
				+ sizeof(*d) + lorv);
		}
	}

	return 0;
}

/*
 * check data attributes in IPSEC AH/ESP.
 */
static int
check_attr_ah(struct isakmp_pl_t *trns)
{
	return check_attr_ipsec(IPSECDOI_PROTO_IPSEC_AH, trns);
}

static int
check_attr_esp(struct isakmp_pl_t *trns)
{
	return check_attr_ipsec(IPSECDOI_PROTO_IPSEC_ESP, trns);
}

static int
check_attr_ipsec(int proto_id, struct isakmp_pl_t *trns)
{
	struct isakmp_data *d;
	int tlen;
	int flag;
	uint16_t type = 0;
	uint16_t lorv;
	int attrseen[16];	/* XXX magic number */

	tlen = get_uint16(&trns->h.len) - sizeof(struct isakmp_pl_t);
	d = (struct isakmp_data *)((caddr_t)trns + sizeof(struct isakmp_pl_t));
	memset(attrseen, 0, sizeof(attrseen));

	while (tlen > 0) {
		type = get_uint16(&d->type) & ~ISAKMP_GEN_MASK;
		flag = get_uint16(&d->type) & ISAKMP_GEN_MASK;
		lorv = get_uint16(&d->lorv);

		plog(PLOG_DEBUG, PLOGLOC, NULL,
			"type=%s, flag=0x%04x, lorv=%s\n",
			s_ipsecdoi_attr(type), flag,
			s_ipsecdoi_attr_v(type, lorv));

		if (type < ARRAYLEN(attrseen))
			attrseen[type]++;

		switch (type) {
		case IPSECDOI_ATTR_ENC_MODE:
			if (! flag) {
				plog(PLOG_PROTOERR, PLOGLOC, NULL,
					"must be TV when ENC_MODE.\n");
				return -1;
			}

			switch (lorv) {
			case IPSECDOI_ATTR_ENC_MODE_TUNNEL:
			case IPSECDOI_ATTR_ENC_MODE_TRNS:
				break;
#ifdef ENABLE_NATT
			case IPSECDOI_ATTR_ENC_MODE_UDPTUNNEL_RFC:
			case IPSECDOI_ATTR_ENC_MODE_UDPTRNS_RFC:
			case IPSECDOI_ATTR_ENC_MODE_UDPTUNNEL_DRAFT:
			case IPSECDOI_ATTR_ENC_MODE_UDPTRNS_DRAFT:
				plog(PLOG_DEBUG, PLOGLOC, NULL,
				     "UDP encapsulation requested\n");
				break;
#endif
			default:
				plog(PLOG_PROTOERR, PLOGLOC, NULL,
					"invalid encryption mode=%u.\n",
					lorv);
				return -1;
			}
			break;

		case IPSECDOI_ATTR_AUTH:
			if (! flag) {
				plog(PLOG_PROTOERR, PLOGLOC, NULL,
					"must be TV when AUTH.\n");
				return -1;
			}

			switch (lorv) {
			case IPSECDOI_ATTR_AUTH_HMAC_MD5:
				if (proto_id == IPSECDOI_PROTO_IPSEC_AH &&
				    trns->t_id != IPSECDOI_AH_MD5) {
ahmismatch:
					plog(PLOG_PROTOERR, PLOGLOC, NULL,
						"auth algorithm %u conflicts "
						"with transform %u.\n",
						lorv, trns->t_id);
					return -1;
				}
				break;
			case IPSECDOI_ATTR_AUTH_HMAC_SHA1:
				if (proto_id == IPSECDOI_PROTO_IPSEC_AH) {
					if (trns->t_id != IPSECDOI_AH_SHA)
						goto ahmismatch;
				}
				break;
 			case IPSECDOI_ATTR_AUTH_HMAC_SHA2_256:
 				if (proto_id == IPSECDOI_PROTO_IPSEC_AH) {
 					if (trns->t_id != IPSECDOI_AH_SHA256)
 						goto ahmismatch;
 				}	
 				break;
 			case IPSECDOI_ATTR_AUTH_HMAC_SHA2_384:
 				if (proto_id == IPSECDOI_PROTO_IPSEC_AH) {
 					if (trns->t_id != IPSECDOI_AH_SHA384)
 						goto ahmismatch;
 				}
 				break;
 			case IPSECDOI_ATTR_AUTH_HMAC_SHA2_512:
 				if (proto_id == IPSECDOI_PROTO_IPSEC_AH) {
 					if (trns->t_id != IPSECDOI_AH_SHA512)
 					goto ahmismatch;
 				}
 				break;
			case IPSECDOI_ATTR_AUTH_DES_MAC:
			case IPSECDOI_ATTR_AUTH_KPDK:
				plog(PLOG_PROTOERR, PLOGLOC, NULL,
					"auth algorithm %u isn't supported.\n",
					lorv);
				return -1;
			default:
				plog(PLOG_PROTOERR, PLOGLOC, NULL,
					"invalid auth algorithm=%u.\n",
					lorv);
				return -1;
			}
			break;

		case IPSECDOI_ATTR_SA_LD_TYPE:
			if (! flag) {
				plog(PLOG_PROTOERR, PLOGLOC, NULL,
					"must be TV when LD_TYPE.\n");
				return -1;
			}

			switch (lorv) {
			case IPSECDOI_ATTR_SA_LD_TYPE_SEC:
			case IPSECDOI_ATTR_SA_LD_TYPE_KB:
				break;
			default:
				plog(PLOG_PROTOERR, PLOGLOC, NULL,
					"invalid life type %d.\n", lorv);
				return -1;
			}
			break;

		case IPSECDOI_ATTR_SA_LD:
			if (flag) {
				/* i.e. ISAKMP_GEN_TV */
				plog(PLOG_DEBUG, PLOGLOC, NULL,
					"life duration was in TLV.\n");
			} else {
				/* i.e. ISAKMP_GEN_TLV */
				if (lorv == 0) {
					plog(PLOG_PROTOERR, PLOGLOC, NULL,
						"invalid length of LD\n");
					return -1;
				}
			}
			break;

		case IPSECDOI_ATTR_GRP_DESC:
			if (! flag) {
				plog(PLOG_PROTOERR, PLOGLOC, NULL,
					"must be TV when GRP_DESC.\n");
				return -1;
			}

			if (!alg_oakley_dhdef_ok(lorv)) {
				plog(PLOG_PROTOERR, PLOGLOC, NULL,
					"invalid group description=%u.\n",
					lorv);
				return -1;
			}
			break;

		case IPSECDOI_ATTR_KEY_LENGTH:
			if (! flag) {
				plog(PLOG_PROTOERR, PLOGLOC, NULL,
					"must be TV when KEY_LENGTH.\n");
				return -1;
			}
			break;

		case IPSECDOI_ATTR_KEY_ROUNDS:
		case IPSECDOI_ATTR_COMP_DICT_SIZE:
		case IPSECDOI_ATTR_COMP_PRIVALG:
			plog(PLOG_PROTOERR, PLOGLOC, NULL,
				"attr type=%u isn't supported.\n", type);
			return -1;

		default:
			plog(PLOG_PROTOERR, PLOGLOC, NULL,
				"invalid attribute type %d.\n", type);
			return -1;
		}

		if (flag) {
			tlen -= sizeof(*d);
			d = (struct isakmp_data *)((char *)d
				+ sizeof(*d));
		} else {
			tlen -= (sizeof(*d) + lorv);
			d = (struct isakmp_data *)((caddr_t)d
				+ sizeof(*d) + lorv);
		}
	}

	if (proto_id == IPSECDOI_PROTO_IPSEC_AH &&
	    !attrseen[IPSECDOI_ATTR_AUTH]) {
		plog(PLOG_PROTOERR, PLOGLOC, NULL,
			"attr AUTH must be present for AH.\n");
		return -1;
	}

	if (proto_id == IPSECDOI_PROTO_IPSEC_ESP &&
	    trns->t_id == IPSECDOI_ESP_NULL &&
	    !attrseen[IPSECDOI_ATTR_AUTH]) {
		plog(PLOG_PROTOERR, PLOGLOC, NULL,
		    "attr AUTH must be present for ESP NULL encryption.\n");
		return -1;
	}

	return 0;
}

static int
check_attr_ipcomp(struct isakmp_pl_t *trns)
{
	struct isakmp_data *d;
	int tlen;
	int flag;
	uint16_t type = 0;
	uint16_t lorv;
	int attrseen[16];	/* XXX magic number */

	tlen = get_uint16(&trns->h.len) - sizeof(struct isakmp_pl_t);
	d = (struct isakmp_data *)((caddr_t)trns + sizeof(struct isakmp_pl_t));
	memset(attrseen, 0, sizeof(attrseen));

	while (tlen > 0) {
		type = get_uint16(&d->type) & ~ISAKMP_GEN_MASK;
		flag = get_uint16(&d->type) & ISAKMP_GEN_MASK;
		lorv = get_uint16(&d->lorv);

		plog(PLOG_DEBUG, PLOGLOC, NULL,
			"type=%d, flag=0x%04x, lorv=0x%04x\n",
			type, flag, lorv);

		if (type < ARRAYLEN(attrseen))
			attrseen[type]++;

		switch (type) {
		case IPSECDOI_ATTR_ENC_MODE:
			if (! flag) {
				plog(PLOG_PROTOERR, PLOGLOC, NULL,
					"must be TV when ENC_MODE.\n");
				return -1;
			}

			switch (lorv) {
			case IPSECDOI_ATTR_ENC_MODE_TUNNEL:
			case IPSECDOI_ATTR_ENC_MODE_TRNS:
				break;
#ifdef ENABLE_NATT
			case IPSECDOI_ATTR_ENC_MODE_UDPTUNNEL_RFC:
			case IPSECDOI_ATTR_ENC_MODE_UDPTRNS_RFC:
			case IPSECDOI_ATTR_ENC_MODE_UDPTUNNEL_DRAFT:
			case IPSECDOI_ATTR_ENC_MODE_UDPTRNS_DRAFT:
				plog(PLOG_DEBUG, PLOGLOC, NULL,
				     "UDP encapsulation requested\n");
				break;
#endif
			default:
				plog(PLOG_PROTOERR, PLOGLOC, NULL,
					"invalid encryption mode=%u.\n",
					lorv);
				return -1;
			}
			break;

		case IPSECDOI_ATTR_SA_LD_TYPE:
			if (! flag) {
				plog(PLOG_PROTOERR, PLOGLOC, NULL,
					"must be TV when LD_TYPE.\n");
				return -1;
			}

			switch (lorv) {
			case IPSECDOI_ATTR_SA_LD_TYPE_SEC:
			case IPSECDOI_ATTR_SA_LD_TYPE_KB:
				break;
			default:
				plog(PLOG_PROTOERR, PLOGLOC, NULL,
					"invalid life type %d.\n", lorv);
				return -1;
			}
			break;

		case IPSECDOI_ATTR_SA_LD:
			if (flag) {
				/* i.e. ISAKMP_GEN_TV */
				plog(PLOG_DEBUG, PLOGLOC, NULL,
					"life duration was in TLV.\n");
			} else {
				/* i.e. ISAKMP_GEN_TLV */
				if (lorv == 0) {
					plog(PLOG_PROTOERR, PLOGLOC, NULL,
						"invalid length of LD\n");
					return -1;
				}
			}
			break;

		case IPSECDOI_ATTR_GRP_DESC:
			if (! flag) {
				plog(PLOG_PROTOERR, PLOGLOC, NULL,
					"must be TV when GRP_DESC.\n");
				return -1;
			}

			if (!alg_oakley_dhdef_ok(lorv)) {
				plog(PLOG_PROTOERR, PLOGLOC, NULL,
					"invalid group description=%u.\n",
					lorv);
				return -1;
			}
			break;

		case IPSECDOI_ATTR_AUTH:
			plog(PLOG_PROTOERR, PLOGLOC, NULL,
				"invalid attr type=%u.\n", type);
			return -1;

		case IPSECDOI_ATTR_KEY_LENGTH:
		case IPSECDOI_ATTR_KEY_ROUNDS:
		case IPSECDOI_ATTR_COMP_DICT_SIZE:
		case IPSECDOI_ATTR_COMP_PRIVALG:
			plog(PLOG_PROTOERR, PLOGLOC, NULL,
				"attr type=%u isn't supported.\n", type);
			return -1;

		default:
			plog(PLOG_PROTOERR, PLOGLOC, NULL,
				"invalid attribute type %d.\n", type);
			return -1;
		}

		if (flag) {
			tlen -= sizeof(*d);
			d = (struct isakmp_data *)((char *)d
				+ sizeof(*d));
		} else {
			tlen -= (sizeof(*d) + lorv);
			d = (struct isakmp_data *)((caddr_t)d
				+ sizeof(*d) + lorv);
		}
	}

#if 0
	if (proto_id == IPSECDOI_PROTO_IPCOMP &&
	    !attrseen[IPSECDOI_ATTR_AUTH]) {
		plog(PLOG_PROTOERR, PLOGLOC, NULL,
			"attr AUTH must be present for AH.\n", type);
		return -1;
	}
#endif

	return 0;
}

/* %%% */
/*
 * create phase1 proposal from remote configuration.
 * NOT INCLUDING isakmp general header of SA payload
 */
rc_vchar_t *
ipsecdoi_setph1proposal(struct isakmpsa *props)
{
	rc_vchar_t *mysa;
	int sablen;

	/* count total size of SA minus isakmp general header */
	/* not including isakmp general header of SA payload */
	sablen = sizeof(struct ipsecdoi_sa_b);
	sablen += setph1prop(props, NULL);

	mysa = rc_vmalloc(sablen);
	if (mysa == NULL) {
		plog(PLOG_PROTOERR, PLOGLOC, NULL,
			"failed to allocate my sa buffer\n");
		return NULL;
	}

	/* create SA payload */
	/* not including isakmp general header */
	put_uint32(&((struct ipsecdoi_sa_b *)mysa->v)->doi, ikev1_doitype(props->rmconf));
	put_uint32(&((struct ipsecdoi_sa_b *)mysa->v)->sit, ikev1_sittype(props->rmconf));

	(void)setph1prop(props, mysa->s + sizeof(struct ipsecdoi_sa_b));

	return mysa;
}

static int
setph1prop(struct isakmpsa *props, caddr_t buf)
{
	struct isakmp_pl_p *prop = NULL;
	struct isakmpsa *s = NULL;
	int proplen, trnslen;
	uint8_t *np_t; /* pointer next trns type in previous header */
	int trns_num;
	caddr_t p = buf;

	proplen = sizeof(*prop);
	if (buf) {
		/* create proposal */
		prop = (struct isakmp_pl_p *)p;
		prop->h.np = ISAKMP_NPTYPE_NONE;
		prop->p_no = props->prop_no;
		prop->proto_id = IPSECDOI_PROTO_ISAKMP;
		prop->spi_size = 0;
		p += sizeof(*prop);
	}

	np_t = NULL;
	trns_num = 0;

	for (s = props; s != NULL; s = s->next) {
		if (np_t)
			*np_t = ISAKMP_NPTYPE_T;

		trnslen = setph1trns(s, p);
		proplen += trnslen;
		if (buf) {
			/* save buffer to pre-next payload */
			np_t = &((struct isakmp_pl_t *)p)->h.np;
			p += trnslen;

			/* count up transform length */
			trns_num++;
		}
	}

	/* update proposal length */
	if (buf) {
		put_uint16(&prop->h.len, proplen);
		prop->num_t = trns_num;
	}

	return proplen;
}

static int
setph1trns(struct isakmpsa *sa, caddr_t buf)
{
	struct isakmp_pl_t *trns = NULL;
	int trnslen, attrlen;
	caddr_t p = buf;

	trnslen = sizeof(*trns);
	if (buf) {
		/* create transform */
		trns = (struct isakmp_pl_t *)p;
		trns->h.np  = ISAKMP_NPTYPE_NONE;
		trns->t_no  = sa->trns_no;
		trns->t_id  = IPSECDOI_KEY_IKE;
		p += sizeof(*trns);
	}

	attrlen = setph1attr(sa, p);
	trnslen += attrlen;
	if (buf)
		p += attrlen;

	if (buf)
		put_uint16(&trns->h.len, trnslen);

	return trnslen;
}

static int
setph1attr(struct isakmpsa *sa, caddr_t buf)
{
	caddr_t p = buf;
	int attrlen = 0;

	if (sa->lifetime) {
		uint32_t lifetime = htonl((uint32_t)sa->lifetime);

		attrlen += sizeof(struct isakmp_data)
			+ sizeof(struct isakmp_data);
		if (sa->lifetime > 0xffff)
			attrlen += sizeof(lifetime);
		if (buf) {
			p = isakmp_set_attr_l(p, OAKLEY_ATTR_SA_LD_TYPE,
						OAKLEY_ATTR_SA_LD_TYPE_SEC);
			if (sa->lifetime > 0xffff) {
				p = isakmp_set_attr_v(p, OAKLEY_ATTR_SA_LD,
						(caddr_t)&lifetime,
					       	sizeof(lifetime));
			} else {
				p = isakmp_set_attr_l(p, OAKLEY_ATTR_SA_LD,
							sa->lifetime);
			}
		}
	}

	if (sa->lifebyte) {
		uint32_t lifebyte = htonl((uint32_t)sa->lifebyte);
		
		attrlen += sizeof(struct isakmp_data)
			+ sizeof(struct isakmp_data);
		if (sa->lifebyte > 0xffff)
			attrlen += sizeof(lifebyte);
		if (buf) {
			p = isakmp_set_attr_l(p, OAKLEY_ATTR_SA_LD_TYPE,
						OAKLEY_ATTR_SA_LD_TYPE_KB);
			if (sa->lifebyte > 0xffff) {
				p = isakmp_set_attr_v(p, OAKLEY_ATTR_SA_LD,
							(caddr_t)&lifebyte,
						       	sizeof(lifebyte));
			} else {
				p = isakmp_set_attr_l(p, OAKLEY_ATTR_SA_LD,
							sa->lifebyte);
			}
		}
	}

	if (sa->enctype) {
		attrlen += sizeof(struct isakmp_data);
		if (buf)
			p = isakmp_set_attr_l(p, OAKLEY_ATTR_ENC_ALG, sa->enctype);
	}
	if (sa->encklen) {
		attrlen += sizeof(struct isakmp_data);
		if (buf)
			p = isakmp_set_attr_l(p, OAKLEY_ATTR_KEY_LEN, sa->encklen);
	}
	if (sa->authmethod) {
		attrlen += sizeof(struct isakmp_data);
		if (buf)
			p = isakmp_set_attr_l(p, OAKLEY_ATTR_AUTH_METHOD, sa->authmethod);
	}
	if (sa->hashtype) {
		attrlen += sizeof(struct isakmp_data);
		if (buf)
			p = isakmp_set_attr_l(p, OAKLEY_ATTR_HASH_ALG, sa->hashtype);
	}
	switch (sa->dh_group) {
	case OAKLEY_ATTR_GRP_DESC_MODP768:
	case OAKLEY_ATTR_GRP_DESC_MODP1024:
	case OAKLEY_ATTR_GRP_DESC_MODP1536:
	case OAKLEY_ATTR_GRP_DESC_MODP2048:
	case OAKLEY_ATTR_GRP_DESC_MODP3072:
	case OAKLEY_ATTR_GRP_DESC_MODP4096:
	case OAKLEY_ATTR_GRP_DESC_MODP6144:
	case OAKLEY_ATTR_GRP_DESC_MODP8192:
		/* don't attach group type for known groups */
		attrlen += sizeof(struct isakmp_data);
		if (buf) {
			p = isakmp_set_attr_l(p, OAKLEY_ATTR_GRP_DESC,
				sa->dh_group);
		}
		break;
	case OAKLEY_ATTR_GRP_DESC_EC2N155:
	case OAKLEY_ATTR_GRP_DESC_EC2N185:
		/* don't attach group type for known groups */
		attrlen += sizeof(struct isakmp_data);
		if (buf) {
			p = isakmp_set_attr_l(p, OAKLEY_ATTR_GRP_TYPE,
				OAKLEY_ATTR_GRP_TYPE_EC2N);
		}
		break;
	case 0:
	default:
		break;
	}

#ifdef HAVE_GSSAPI
	if (sa->authmethod == OAKLEY_ATTR_AUTH_METHOD_GSSAPI_KRB &&
	    sa->gssid != NULL) {
		attrlen += sizeof(struct isakmp_data);
		attrlen += sa->gssid->l;
		if (buf) {
			plog(PLOG_DEBUG, PLOGLOC, NULL, "gss id attr: len %d, "
			    "val '%s'\n", sa->gssid->l, sa->gssid->v);
			p = isakmp_set_attr_v(p, OAKLEY_ATTR_GSS_ID,
				(caddr_t)sa->gssid->v, 
				sa->gssid->l);
		}
	}
#endif

	return attrlen;
}

static rc_vchar_t *
setph2proposal0(const struct ph2handle *iph2, const struct saprop *pp,
    const struct saproto *pr)
{
	rc_vchar_t *p;
	struct isakmp_pl_p *prop;
	struct isakmp_pl_t *trns;
	struct satrns *tr;
	int attrlen;
	size_t trnsoff;
	caddr_t x0, x;
	uint8_t *np_t; /* pointer next trns type in previous header */
	const uint8_t *spi;

	p = rc_vmalloc(sizeof(*prop) + sizeof(pr->spi));
	if (p == NULL)
		return NULL;

	/* create proposal */
	prop = (struct isakmp_pl_p *)p->v;
	prop->h.np = ISAKMP_NPTYPE_NONE;
	prop->p_no = pp->prop_no;
	prop->proto_id = pr->proto_id;
	prop->num_t = 1;

	spi = (const uint8_t *)&pr->spi;
	switch (pr->proto_id) {
	case IPSECDOI_PROTO_IPCOMP:
		/*
		 * draft-shacham-ippcp-rfc2393bis-05.txt:
		 * construct 16bit SPI (CPI).
		 * XXX we may need to provide a configuration option to
		 * generate 32bit SPI.  otherwise we cannot interoeprate
		 * with nodes that uses 32bit SPI, in case we are initiator.
		 */
		prop->spi_size = sizeof(uint16_t);
		spi += sizeof(pr->spi) - sizeof(uint16_t);
		p->l -= sizeof(pr->spi);
		p->l += sizeof(uint16_t);
		break;
	default:
		prop->spi_size = sizeof(pr->spi);
		break;
	}
	memcpy(prop + 1, spi, prop->spi_size);

	/* create transform */
	trnsoff = sizeof(*prop) + prop->spi_size;
	np_t = NULL;

	for (tr = pr->head; tr; tr = tr->next) {
	
		switch (pr->proto_id) {
		case IPSECDOI_PROTO_IPSEC_ESP:
			/*
			 * don't build a null encryption
			 * with no authentication transform.
			 */
			if (tr->trns_id == IPSECDOI_ESP_NULL &&
			    tr->authtype == IPSECDOI_ATTR_AUTH_NONE)
				continue;
			break;
		}

		if (np_t) {
			*np_t = ISAKMP_NPTYPE_T;
			prop->num_t++;
		}

		/* get attribute length */
		attrlen = 0;
		if (pp->lifetime) {
			attrlen += sizeof(struct isakmp_data)
				+ sizeof(struct isakmp_data);
			if (pp->lifetime > 0xffff)
				attrlen += sizeof(uint32_t);
		}
		if (pp->lifebyte && pp->lifebyte != IPSECDOI_ATTR_SA_LD_KB_MAX) {
			attrlen += sizeof(struct isakmp_data)
				+ sizeof(struct isakmp_data);
			if (pp->lifebyte > 0xffff)
				attrlen += sizeof(uint32_t);
		}
		attrlen += sizeof(struct isakmp_data);	/* enc mode */
		if (tr->encklen)
			attrlen += sizeof(struct isakmp_data);

		switch (pr->proto_id) {
		case IPSECDOI_PROTO_IPSEC_ESP:
			/* non authentication mode ? */
			if (tr->authtype != IPSECDOI_ATTR_AUTH_NONE)
				attrlen += sizeof(struct isakmp_data);
			break;
		case IPSECDOI_PROTO_IPSEC_AH:
			if (tr->authtype == IPSECDOI_ATTR_AUTH_NONE) {
				plog(PLOG_PROTOERR, PLOGLOC, NULL,
					"no authentication algorithm found "
					"but protocol is AH.\n");
				rc_vfree(p);
				return NULL;
			}
			attrlen += sizeof(struct isakmp_data);
			break;
		case IPSECDOI_PROTO_IPCOMP:
			break;
		default:
			plog(PLOG_PROTOERR, PLOGLOC, NULL,
				"invalid protocol: %d\n", pr->proto_id);
			rc_vfree(p);
			return NULL;
		}

		if (alg_oakley_dhdef_ok(pp->pfs_group))
			attrlen += sizeof(struct isakmp_data);

		p = rc_vrealloc(p, p->l + sizeof(*trns) + attrlen);
		if (p == NULL)
			return NULL;
		prop = (struct isakmp_pl_p *)p->v;

		/* set transform's values */
		trns = (void *)(p->u + trnsoff);
		trns->h.np  = ISAKMP_NPTYPE_NONE;
		trns->t_no  = tr->trns_no;
		trns->t_id  = tr->trns_id;

		/* set attributes */
		x = x0 = p->s + trnsoff + sizeof(*trns);

		if (pp->lifetime) {
			x = isakmp_set_attr_l(x, IPSECDOI_ATTR_SA_LD_TYPE,
						IPSECDOI_ATTR_SA_LD_TYPE_SEC);
			if (pp->lifetime > 0xffff) {
				uint32_t v = htonl((uint32_t)pp->lifetime);
				x = isakmp_set_attr_v(x, IPSECDOI_ATTR_SA_LD,
							(caddr_t)&v, sizeof(v));
			} else {
				x = isakmp_set_attr_l(x, IPSECDOI_ATTR_SA_LD,
							pp->lifetime);
			}
		}

		if (pp->lifebyte && pp->lifebyte != IPSECDOI_ATTR_SA_LD_KB_MAX) {
			x = isakmp_set_attr_l(x, IPSECDOI_ATTR_SA_LD_TYPE,
						IPSECDOI_ATTR_SA_LD_TYPE_KB);
			if (pp->lifebyte > 0xffff) {
				uint32_t v = htonl((uint32_t)pp->lifebyte);
				x = isakmp_set_attr_v(x, IPSECDOI_ATTR_SA_LD,
							(caddr_t)&v, sizeof(v));
			} else {
				x = isakmp_set_attr_l(x, IPSECDOI_ATTR_SA_LD,
							pp->lifebyte);
			}
		}

		x = isakmp_set_attr_l(x, IPSECDOI_ATTR_ENC_MODE, pr->encmode);

		if (tr->encklen)
			x = isakmp_set_attr_l(x, IPSECDOI_ATTR_KEY_LENGTH, tr->encklen);

		/* mandatory check has done above. */
		if ((pr->proto_id == IPSECDOI_PROTO_IPSEC_ESP && tr->authtype != IPSECDOI_ATTR_AUTH_NONE)
		 || pr->proto_id == IPSECDOI_PROTO_IPSEC_AH)
			x = isakmp_set_attr_l(x, IPSECDOI_ATTR_AUTH, tr->authtype);

		if (alg_oakley_dhdef_ok(pp->pfs_group))
			x = isakmp_set_attr_l(x, IPSECDOI_ATTR_GRP_DESC,
				pp->pfs_group);

		/* update length of this transform. */
		trns = (void *)(p->u + trnsoff);
		put_uint16(&trns->h.len, sizeof(*trns) + attrlen);

		/* save buffer to pre-next payload */
		np_t = &trns->h.np;

		trnsoff += (sizeof(*trns) + attrlen);
	}

	if (np_t == NULL) {
		plog(PLOG_PROTOERR, PLOGLOC, NULL,
			"no suitable proposal was created.\n");
		return NULL;
	}

	/* update length of this protocol. */
	put_uint16(&prop->h.len, p->l);

	return p;
}

/*
 * create phase2 proposal from policy configuration.
 * NOT INCLUDING isakmp general header of SA payload.
 * This function is called by initiator only.
 */
int
ipsecdoi_setph2proposal(struct ph2handle *iph2)
{
	struct saprop *proposal, *a;
	struct saproto *b = NULL;
	rc_vchar_t *q;
	struct ipsecdoi_sa_b *sab;
	struct isakmp_pl_p *prop;
	size_t propoff;	/* for previous field of type of next payload. */

	proposal = iph2->proposal;

	iph2->sa = rc_vmalloc(sizeof(*sab));
	if (iph2->sa == NULL) {
		plog(PLOG_PROTOERR, PLOGLOC, NULL,
			"failed to allocate my sa buffer\n");
		return -1;
	}

	/* create SA payload */
	sab = (struct ipsecdoi_sa_b *)iph2->sa->v;
	put_uint32(&sab->doi, IPSEC_DOI);
	put_uint32(&sab->sit, IPSECDOI_SIT_IDENTITY_ONLY); /* XXX configurable ? */

	prop = NULL;
	propoff = 0;
	for (a = proposal; a; a = a->next) {
		for (b = a->head; b; b = b->next) {
#ifdef ENABLE_NATT
			if (iph2->ph1->natt_flags & NAT_DETECTED) {
			  int udp_diff = iph2->ph1->natt_options->mode_udp_diff;
			  plog (PLOG_INFO, PLOGLOC, NULL,
				"NAT detected -> UDP encapsulation "
				"(ENC_MODE %d->%d).\n",
				b->encmode,
				b->encmode+udp_diff);
			  /* Tunnel -> UDP-Tunnel, Transport -> UDP_Transport */
			  b->encmode += udp_diff;
			  b->udp_encap = 1;
			}
#endif

			q = setph2proposal0(iph2, a, b);
			if (q == NULL) {
				VPTRINIT(iph2->sa);
				return -1;
			}

			iph2->sa = rc_vrealloc(iph2->sa, iph2->sa->l + q->l);
			if (iph2->sa == NULL) {
				plog(PLOG_PROTOERR, PLOGLOC, NULL,
					"failed to allocate my sa buffer\n");
				if (q)
					rc_vfree(q);
				return -1;
			}
			memcpy(iph2->sa->u + iph2->sa->l - q->l, q->v, q->l);
			if (propoff != 0) {
				prop = (void *)(iph2->sa->u + propoff);
				prop->h.np = ISAKMP_NPTYPE_P;
			}
			propoff = iph2->sa->l - q->l;

			rc_vfree(q);
		}
	}

	return 0;
}

/*
 * return 1 if all of the given protocols are transport mode.
 */
int
ipsecdoi_transportmode(struct saprop *pp)
{
	struct saproto *pr = NULL;

	for (; pp; pp = pp->next) {
		for (pr = pp->head; pr; pr = pr->next) {
			if (pr->encmode != IPSECDOI_ATTR_ENC_MODE_TRNS)
				return 0;
		}
	}

	return 1;
}

#if 0
int
ipsecdoi_get_defaultlifetime(void)
{
	return IPSECDOI_ATTR_SA_LD_SEC_DEFAULT;
}
#endif

int
ipsecdoi_checkalgtypes(int proto_id, int enc, int auth, int comp)
{
#define TMPALGTYPE2STR(n) s_algtype(algclass_ipsec_##n, n)
	switch (proto_id) {
	case IPSECDOI_PROTO_IPSEC_ESP:
		if (enc == 0 || comp != 0) {
			plog(PLOG_PROTOERR, PLOGLOC, NULL,
				"illegal algorithm defined "
				"ESP enc=%s auth=%s comp=%s.\n",
				TMPALGTYPE2STR(enc),
				TMPALGTYPE2STR(auth),
				TMPALGTYPE2STR(comp));
			return -1;
		}
		break;
	case IPSECDOI_PROTO_IPSEC_AH:
		if (enc != 0 || auth == 0 || comp != 0) {
			plog(PLOG_PROTOERR, PLOGLOC, NULL,
				"illegal algorithm defined "
				"AH enc=%s auth=%s comp=%s.\n",
				TMPALGTYPE2STR(enc),
				TMPALGTYPE2STR(auth),
				TMPALGTYPE2STR(comp));
			return -1;
		}
		break;
	case IPSECDOI_PROTO_IPCOMP:
		if (enc != 0 || auth != 0 || comp == 0) {
			plog(PLOG_PROTOERR, PLOGLOC, NULL,
				"illegal algorithm defined "
				"IPcomp enc=%s auth=%s comp=%s.\n",
				TMPALGTYPE2STR(enc),
				TMPALGTYPE2STR(auth),
				TMPALGTYPE2STR(comp));
			return -1;
		}
		break;
	default:
		plog(PLOG_PROTOERR, PLOGLOC, NULL,
			"invalid ipsec protocol %d\n", proto_id);
		return -1;
	}
#undef TMPALGTYPE2STR
	return 0;
}

int
ipproto2doi(int proto)
{
	switch (proto) {
	case IPPROTO_AH:
		return IPSECDOI_PROTO_IPSEC_AH;
	case IPPROTO_ESP:
		return IPSECDOI_PROTO_IPSEC_ESP;
	case IPPROTO_IPCOMP:
		return IPSECDOI_PROTO_IPCOMP;
	}
	return -1;	/* XXX */
}

int
doi2ipproto(int proto)
{
	switch (proto) {
	case IPSECDOI_PROTO_IPSEC_AH:
		return IPPROTO_AH;
	case IPSECDOI_PROTO_IPSEC_ESP:
		return IPPROTO_ESP;
	case IPSECDOI_PROTO_IPCOMP:
		return IPPROTO_IPCOMP;
	}
	return -1;	/* XXX */
}

/*
 * check the following:
 * - In main mode with pre-shared key, only address type can be used.
 * - if proper type for phase 1 ?
 * - if phase 1 ID payload conformed RFC2407 4.6.2.
 *   (proto, port) must be (0, 0), (udp, 500) or (udp, [specified]).
 * - if ID payload sent from peer is equal to the ID expected by me.
 *
 * both of "id" and "id_p" should be ID payload without general header,
 */
int
ipsecdoi_checkid1(struct ph1handle *iph1)
{
	struct ipsecdoi_id_b *id_b;
#if 0
	struct sockaddr *sa;
	caddr_t sa1, sa2;
#endif

	if (iph1->id_p == NULL) {
		plog(PLOG_PROTOERR, PLOGLOC, NULL,
			"invalid iph1 passed id_p == NULL\n");
		return ISAKMP_INTERNAL_ERROR;
	}
	if (iph1->id_p->l < sizeof(*id_b)) {
		plog(PLOG_PROTOERR, PLOGLOC, NULL,
			"invalid value passed as \"ident\" (len=%lu)\n",
			(u_long)iph1->id_p->l);
		return ISAKMP_NTYPE_INVALID_ID_INFORMATION;
	}

	id_b = (struct ipsecdoi_id_b *)iph1->id_p->v;

	/* In main mode with pre-shared key, only address type can be used. */
	if (iph1->etype == ISAKMP_ETYPE_IDENT &&
	    iph1->approval->authmethod == OAKLEY_ATTR_AUTH_METHOD_PSKEY) {
		 if (id_b->type != IPSECDOI_ID_IPV4_ADDR
		  && id_b->type != IPSECDOI_ID_IPV6_ADDR) {
			plog(PLOG_PROTOERR, PLOGLOC, NULL,
				"Expecting IP address type in main mode, "
				"but %s.\n", s_ipsecdoi_ident(id_b->type));
			return ISAKMP_NTYPE_INVALID_ID_INFORMATION;
		}
	}

	/* if proper type for phase 1 ? */
	switch (id_b->type) {
	case IPSECDOI_ID_IPV4_ADDR_SUBNET:
	case IPSECDOI_ID_IPV6_ADDR_SUBNET:
	case IPSECDOI_ID_IPV4_ADDR_RANGE:
	case IPSECDOI_ID_IPV6_ADDR_RANGE:
		plog(PLOG_PROTOWARN, PLOGLOC, 0,
		     "peer ID type %s is not acceptable\n",
		     s_ipsecdoi_ident(id_b->type));
		/*FALLTHROUGH*/
	}

	/* if phase 1 ID payload conformed RFC2407 4.6.2. */
	if (id_b->type == IPSECDOI_ID_IPV4_ADDR ||
	    id_b->type == IPSECDOI_ID_IPV6_ADDR) {

		if (id_b->proto_id == 0 && get_uint16(&id_b->port) != 0) {
			plog(PLOG_PROTOWARN, PLOGLOC, 0,
				"protocol ID and Port mismatched. "
				"proto_id:%d port:%d\n",
				id_b->proto_id, get_uint16(&id_b->port));
			/*FALLTHROUGH*/

		} else if (id_b->proto_id == IPPROTO_UDP) {
			/*
			 * copmaring with expecting port.
			 * always permit if port is equal to PORT_ISAKMP
			 */
			if (get_uint16(&id_b->port) != PORT_ISAKMP) {

				uint16_t port;

				switch (iph1->remote->sa_family) {
				case AF_INET:
					port = ((struct sockaddr_in *)iph1->remote)->sin_port;
					break;
#ifdef INET6
				case AF_INET6:
					port = ((struct sockaddr_in6 *)iph1->remote)->sin6_port;
					break;
#endif
				default:
					plog(PLOG_PROTOERR, PLOGLOC, NULL,
						"invalid family: %d\n",
						iph1->remote->sa_family);
					return ISAKMP_NTYPE_INVALID_ID_INFORMATION;
				}
				if (get_uint16(&id_b->port) != port) {
					plog(PLOG_PROTOWARN, PLOGLOC, 0,
						"port %d expected, but %d\n",
						port, get_uint16(&id_b->port));
					/*FALLTHROUGH*/
				}
			}
		}
	}

#if 0
	/* compare with the ID if specified. */
	if (ikev1_peers_id(iph1->rmconf)) {
		rc_type rc_id_type;
		rc_vchar_t *id_data;

		/* check the type of both IDs */
		if (ikev1_compare_id(iph1->id_p, ikev1_peers_id(iph1->rmconf)) != 0) {
			if (ikev1_verify_id(iph1->rmconf) == RCT_BOOL_ON) {
				plog(PLOG_PROTOERR, PLOGLOC, 0,
				     "peer identifier does not match\n");
				return ISAKMP_NTYPE_INVALID_ID_INFORMATION;
			}
			plog(PLOG_PROTOWARN, PLOGLOC, 0,
			     "peer identifier does not match\n");
		}
	}
#endif

	return 0;
}

/*
 * create ID payload for phase 1 and set into iph1->id.
 * NOT INCLUDING isakmp general header.
 * see, RFC2407 4.6.2.1
 */
int
ipsecdoi_setid1(struct ph1handle *iph1)
{
	struct rc_idlist *id;
	rc_vchar_t *id_data = 0;
	int id_type;
	struct ipsecdoi_id_b id_b;
	rc_vchar_t *ret;

	id = ikev1_my_id(iph1->rmconf);
	id_data = ike_identifier_data(id, &id_type);
	if (!id_data)
		goto err;

	/* init */
	id_b.type = id_type;
	id_b.proto_id = 0;
	id_b.port = 0;

	ret = rc_vmalloc(sizeof(id_b) + id_data->l);
	if (ret == NULL)
		goto err;

	memcpy(ret->u, &id_b, sizeof(id_b));
	memcpy(ret->u + sizeof(id_b), id_data->v, id_data->l);

	iph1->id = ret;

	plog(PLOG_DEBUG, PLOGLOC, NULL,
	     "using ID type %s\n", s_ipsecdoi_ident(id_b.type));
	if (id_data)
		rc_vfree(id_data);
	return 0;

err:
	if (id_data)
		rc_vfree(id_data);
	plog(PLOG_INTERR, PLOGLOC, NULL, "failed constructing my ID\n");
	return -1;
}

#if 0
static rc_vchar_t *
getidval(type, val)
	int type;
	rc_vchar_t *val;
{
	rc_vchar_t *new = NULL;

	if (val)
		new = rc_vdup(val);
	else if (lcconf->ident[type])
		new = rc_vdup(lcconf->ident[type]);

	return new;
}
#endif

#if 0
/* it's only called by cfparse.y. */
int
set_identifier(vpp, type, value)
	rc_vchar_t **vpp, *value;
	int type;
{
	rc_vchar_t *new = NULL;

	/* simply return if value is null. */
	if (!value)
		return 0;

	switch (type) {
	case IDTYPE_FQDN:
	case IDTYPE_USERFQDN:
		/* length is adjusted since QUOTEDSTRING teminates NULL. */
		new = rc_vmalloc(value->l - 1);
		if (new == NULL)
			return -1;
		memcpy(new->v, value->v, new->l);
		break;
	case IDTYPE_KEYID:
	{
		FILE *fp;
		char b[512];
		int tlen, len;

		fp = fopen(value->v, "r");
		if (fp == NULL) {
			plog(PLOG_PROTOERR, PLOGLOC, NULL,
				"can not open %s\n", value->v);
			return -1;
		}
		tlen = 0;
		while ((len = fread(b, 1, sizeof(b), fp)) != 0) {
			new = rc_vrealloc(new, tlen + len);
			if (!new) {
				fclose(fp);
				return -1;
			}
			memcpy(new->v + tlen, b, len);
			tlen += len;
		}
		break;
	}
	case IDTYPE_ADDRESS:
	{
		struct sockaddr *sa;

		/* length is adjusted since QUOTEDSTRING teminates NULL. */
		if (value->l == 0)
			break;

		sa = str2saddr(value->v, NULL);
		if (sa == NULL) {
			plog(PLOG_PROTOERR, PLOGLOC, NULL,
				"invalid ip address %s\n", value->v);
			return -1;
		}

		new = rc_vmalloc(sa->sa_len);
		if (new == NULL)
			return -1;
		memcpy(new->v, sa, new->l);
		break;
	}
	case IDTYPE_ASN1DN:
		new = eay_str2asn1dn(value->v, value->l - 1);
		if (new == NULL)
			return -1;
		break;
	}

	*vpp = new;

	return 0;
}
#endif

/*
 * create ID payload for phase 2, and set into iph2->id and id_p.  There are
 * NOT INCLUDING isakmp general header.
 * this function is for initiator.  responder will get to copy from payload.
 * responder ID type is always address type.
 * see, RFC2407 4.6.2.1
 */
int
ipsecdoi_setid2(struct ph2handle *iph2)
{
	struct rcf_selector *sel;
	int proto;

	/* check there is phase 2 handler ? */
	sel = iph2->selector;
	assert(sel->src->type == RCT_ADDR_INET);
	assert(sel->dst->type == RCT_ADDR_INET);
	proto = sel->upper_layer_protocol;
	if (proto == RC_PROTO_ANY)
		proto = 0;

	iph2->id = ipsecdoi_sockaddr2id(sel->src->a.ipaddr, sel->src->prefixlen, proto);
	if (iph2->id == NULL) {
		plog(PLOG_PROTOERR, PLOGLOC, NULL,
		     "failed to create ID payload for %s\n",
		     rc_vmem2str(sel->sl_index));
		return -1;
	}
	plog(PLOG_DEBUG, PLOGLOC, NULL, "use local ID type %s\n",
		s_ipsecdoi_ident(((struct ipsecdoi_id_b *)iph2->id->v)->type));

	/* remote side */
	iph2->id_p = ipsecdoi_sockaddr2id(sel->dst->a.ipaddr, sel->dst->prefixlen, proto);
	if (iph2->id_p == NULL) {
		plog(PLOG_PROTOERR, PLOGLOC, NULL,
		     "failed to create ID payload for %s\n",
		     rc_vmem2str(sel->sl_index));
		VPTRINIT(iph2->id);
		return -1;
	}
	plog(PLOG_DEBUG, PLOGLOC, NULL,
		"use remote ID type %s\n",
		s_ipsecdoi_ident(((struct ipsecdoi_id_b *)iph2->id_p->v)->type));

	return 0;
}

/*
 * set address type of ID.
 * NOT INCLUDING general header.
 */
rc_vchar_t *
ipsecdoi_sockaddr2id(struct sockaddr *saddr, unsigned int prefixlen,
    unsigned int ul_proto)
{
	rc_vchar_t *new;
	int type, len1, len2;
	caddr_t sa;
	uint16_t port;

	/*
	 * Q. When type is SUBNET, is it allowed to be ::1/128.
	 * A. Yes. (consensus at bake-off)
	 */
	switch (saddr->sa_family) {
	case AF_INET:
		len1 = sizeof(struct in_addr);
		if (prefixlen == (sizeof(struct in_addr) << 3)) {
			type = IPSECDOI_ID_IPV4_ADDR;
			len2 = 0;
		} else {
			type = IPSECDOI_ID_IPV4_ADDR_SUBNET;
			len2 = sizeof(struct in_addr);
		}
		sa = (caddr_t)&((struct sockaddr_in *)(saddr))->sin_addr;
		port = ((struct sockaddr_in *)(saddr))->sin_port;
		break;
#ifdef INET6
	case AF_INET6:
		len1 = sizeof(struct in6_addr);
		if (prefixlen == (sizeof(struct in6_addr) << 3)) {
			type = IPSECDOI_ID_IPV6_ADDR;
			len2 = 0;
		} else {
			type = IPSECDOI_ID_IPV6_ADDR_SUBNET;
			len2 = sizeof(struct in6_addr);
		}
		sa = (caddr_t)&((struct sockaddr_in6 *)(saddr))->sin6_addr;
		port = ((struct sockaddr_in6 *)(saddr))->sin6_port;
		break;
#endif
	default:
		plog(PLOG_PROTOERR, PLOGLOC, NULL,
			"invalid family: %d.\n", saddr->sa_family);
		return NULL;
	}

	/* get ID buffer */
	new = rc_vmalloc(sizeof(struct ipsecdoi_id_b) + len1 + len2);
	if (new == NULL) {
		plog(PLOG_PROTOERR, PLOGLOC, NULL,
			"failed to get ID buffer.\n");
		return NULL;
	}

	memset(new->v, 0, new->l);

	/* set the part of header. */
	((struct ipsecdoi_id_b *)new->v)->type = type;

	/* set ul_proto and port */
	/*
	 * NOTE: we use both IPSEC_ULPROTO_ANY and IPSEC_PORT_ANY as wild card
	 * because 0 means port number of 0.  Instead of 0, we use IPSEC_*_ANY.
	 */
	((struct ipsecdoi_id_b *)new->v)->proto_id =
		ul_proto == IPSEC_ULPROTO_ANY ? 0 : ul_proto;
	((struct ipsecdoi_id_b *)new->v)->port =
		port == IPSEC_PORT_ANY ? 0 : port;
	memcpy(new->u + sizeof(struct ipsecdoi_id_b), sa, len1);

	/* set address */

	/* set prefix */
	if (len2) {
		unsigned char *p = (unsigned char *)new->v + sizeof(struct ipsecdoi_id_b) + len1;
		unsigned int bits = prefixlen;

		while (bits >= 8) {
			*p++ = 0xff;
			bits -= 8;
		}

		if (bits > 0)
			*p = ~((1 << (8 - bits)) - 1);
	}

	return new;
}

/*
 * create sockaddr structure from ID payload (buf).
 * buffers (saddr, prefixlen, ul_proto) must be allocated.
 * see, RFC2407 4.6.2.1
 */
int
ipsecdoi_id2sockaddr(rc_vchar_t *buf, struct sockaddr *saddr,
    uint8_t *prefixlen, uint16_t *ul_proto)
{
	struct ipsecdoi_id_b *id_b = (struct ipsecdoi_id_b *)buf->v;
	unsigned int plen = 0;
	in_port_t *port;

	/*
	 * When a ID payload of subnet type with a IP address of full bit
	 * masked, it has to be processed as host address.
	 * e.g. below 2 type are same.
	 *      type = ipv6 subnet, data = 2001::1/128
	 *      type = ipv6 address, data = 2001::1
	 */
	switch (id_b->type) {
	case IPSECDOI_ID_IPV4_ADDR:
	case IPSECDOI_ID_IPV4_ADDR_SUBNET:
		SET_SOCKADDR_LEN(saddr, sizeof(struct sockaddr_in));
		saddr->sa_family = AF_INET;
		break;
#ifdef INET6
	case IPSECDOI_ID_IPV6_ADDR:
	case IPSECDOI_ID_IPV6_ADDR_SUBNET:
		SET_SOCKADDR_LEN(saddr, sizeof(struct sockaddr_in6));
		saddr->sa_family = AF_INET6;
		*rcs_getsascopeid(saddr) = 0;
		break;
#endif
	default:
		plog(PLOG_PROTOERR, PLOGLOC, NULL,
			"unsupported ID type %d\n", id_b->type);
		return ISAKMP_NTYPE_INVALID_ID_INFORMATION;
	}
	port = rcs_getsaport(saddr);
	/* see sockaddr2id() */
	*port = id_b->port == 0 ? IPSEC_PORT_ANY : id_b->port;
	memcpy(rcs_getsaaddr(saddr), buf->u + sizeof(*id_b),
	    rcs_getsaaddrlen(saddr));

	/* get prefix length */
	switch (id_b->type) {
	case IPSECDOI_ID_IPV4_ADDR:
		plen = sizeof(struct in_addr) << 3;
		break;
#ifdef INET6
	case IPSECDOI_ID_IPV6_ADDR:
		plen = sizeof(struct in6_addr) << 3;
		break;
#endif
	case IPSECDOI_ID_IPV4_ADDR_SUBNET:
#ifdef INET6
	case IPSECDOI_ID_IPV6_ADDR_SUBNET:
#endif
	    {
		unsigned char *p;
		unsigned int max;
		size_t alen = sizeof(struct in_addr);

		switch (id_b->type) {
		case IPSECDOI_ID_IPV4_ADDR_SUBNET:
			alen = sizeof(struct in_addr);
			break;
#ifdef INET6
		case IPSECDOI_ID_IPV6_ADDR_SUBNET:
			alen = sizeof(struct in6_addr);
			break;
#endif
		}

		/* sanity check */
		if (buf->l < alen)
			return ISAKMP_INTERNAL_ERROR;

		/* get subnet mask length */
		plen = 0;
		max = alen <<3;

		p = (unsigned char *) buf->v
			+ sizeof(struct ipsecdoi_id_b)
			+ alen;

		for (; *p == 0xff; p++) {
			plen += 8;
			if (plen >= max)
				break;
		}

		if (plen < max) {
			unsigned int l = 0;
			unsigned char b = ~(*p);

			while (b) {
				b >>= 1;
				l++;
			}

			l = 8 - l;
			plen += l;
		}
	    }
		break;
	}

	*prefixlen = plen;
	*ul_proto = id_b->proto_id == 0
				? IPSEC_ULPROTO_ANY
				: id_b->proto_id;	/* see sockaddr2id() */

	return 0;
}

/*
 * make printable string from ID payload except of general header.
 */
const char *
ipsecdoi_id2str(const rc_vchar_t *id)
{
	static char buf[256];

	/* XXX */
	buf[0] = '\0';

	return buf;
}

/*
 * set IPsec data attributes into a proposal.
 * NOTE: MUST called per a transform.
 */
int
ipsecdoi_t2satrns(struct isakmp_pl_t *t, struct saprop *pp,
    struct saproto *pr, struct satrns *tr)
{
	struct isakmp_data *d, *prev;
	int flag, type;
	int error = -1;
	int life_t;
	int tlen;

	tr->trns_no = t->t_no;
	tr->trns_id = t->t_id;

	tlen = get_uint16(&t->h.len) - sizeof(*t);
	prev = (struct isakmp_data *)NULL;
	d = (struct isakmp_data *)(t + 1);

	/* default */
	life_t = IPSECDOI_ATTR_SA_LD_TYPE_DEFAULT;
	pp->lifetime = IPSECDOI_ATTR_SA_LD_SEC_DEFAULT;
	pp->lifebyte = 0;
	tr->authtype = IPSECDOI_ATTR_AUTH_NONE;

	while (tlen > 0) {

		type = get_uint16(&d->type) & ~ISAKMP_GEN_MASK;
		flag = get_uint16(&d->type) & ISAKMP_GEN_MASK;

		plog(PLOG_DEBUG, PLOGLOC, NULL,
			"type=%s, flag=0x%04x, lorv=%s\n",
			s_ipsecdoi_attr(type), flag,
			s_ipsecdoi_attr_v(type, get_uint16(&d->lorv)));

		switch (type) {
		case IPSECDOI_ATTR_SA_LD_TYPE:
		{
			uint16_t xtype = get_uint16(&d->lorv);
			switch (xtype) {
			case IPSECDOI_ATTR_SA_LD_TYPE_SEC:
			case IPSECDOI_ATTR_SA_LD_TYPE_KB:
				life_t = xtype;
				break;
			default:
				plog(PLOG_PROTOWARN, PLOGLOC, 0,
					"invalid life duration type. "
					"using default value.\n");
				life_t = IPSECDOI_ATTR_SA_LD_TYPE_DEFAULT;
				break;
			}
			break;
		}
		case IPSECDOI_ATTR_SA_LD:
			if (prev == NULL
			 || (get_uint16(&prev->type) & ~ISAKMP_GEN_MASK) !=
					IPSECDOI_ATTR_SA_LD_TYPE) {
				plog(PLOG_PROTOERR, PLOGLOC, NULL,
				    "life duration must follow ltype\n");
				break;
			}

		    {
			uint32_t xt;
			rc_vchar_t *ld_buf = NULL;

			if (flag) {
				/* i.e. ISAKMP_GEN_TV */
				ld_buf = rc_vmalloc(sizeof(d->lorv));
				if (ld_buf == NULL) {
					plog(PLOG_PROTOERR, PLOGLOC, NULL,
					    "failed to get LD buffer.\n");
					goto end;
				}
				memcpy(ld_buf->v, &d->lorv, sizeof(d->lorv));
			} else {
				int len = get_uint16(&d->lorv);
				/* i.e. ISAKMP_GEN_TLV */
				ld_buf = rc_vmalloc(len);
				if (ld_buf == NULL) {
					plog(PLOG_PROTOERR, PLOGLOC, NULL,
					    "failed to get LD buffer.\n");
					goto end;
				}
				memcpy(ld_buf->v, d + 1, len);
			}
			switch (life_t) {
			case IPSECDOI_ATTR_SA_LD_TYPE_SEC:
				xt = ipsecdoi_set_ld(ld_buf);
				rc_vfree(ld_buf);
				if (xt == 0) {
					plog(PLOG_PROTOERR, PLOGLOC, NULL,
						"invalid life duration.\n");
					goto end;
				}
				/* lifetime must be equal in a proposal. */
				if (pp->lifetime == IPSECDOI_ATTR_SA_LD_SEC_DEFAULT)
					pp->lifetime = xt;
				else if (pp->lifetime != xt) {
					plog(PLOG_PROTOERR, PLOGLOC, NULL,
						"lifetime mismatched "
						"in a proposal, "
						"prev:%ld curr:%lu.\n",
						(long)pp->lifetime, (unsigned long)xt);
					goto end;
				}
				break;
			case IPSECDOI_ATTR_SA_LD_TYPE_KB:
				xt = ipsecdoi_set_ld(ld_buf);
				rc_vfree(ld_buf);
				if (xt == 0) {
					plog(PLOG_PROTOERR, PLOGLOC, NULL,
						"invalid life duration.\n");
					goto end;
				}
				/* lifebyte must be equal in a proposal. */
				if (pp->lifebyte == 0)
					pp->lifebyte = xt;
				else if ((uint32_t)pp->lifebyte != xt) {
					plog(PLOG_PROTOERR, PLOGLOC, NULL,
						"lifebyte mismatched "
						"in a proposal, "
						"prev:%ld curr:%lu.\n",
						(long)pp->lifebyte, (unsigned long)xt);
					goto end;
				}
				break;
			default:
				rc_vfree(ld_buf);
				plog(PLOG_PROTOERR, PLOGLOC, NULL,
					"invalid life type: %d\n", life_t);
				goto end;
			}
		    }
			break;

		case IPSECDOI_ATTR_GRP_DESC:
			/*
			 * RFC2407: 4.5 IPSEC Security Association Attributes
			 *   Specifies the Oakley Group to be used in a PFS QM
			 *   negotiation.  For a list of supported values, see
			 *   Appendix A of [IKE].
			 */
			if (pp->pfs_group == 0)
				pp->pfs_group = get_uint16(&d->lorv);
			else if ((uint16_t)pp->pfs_group != get_uint16(&d->lorv)) {
				plog(PLOG_PROTOERR, PLOGLOC, NULL,
					"pfs_group mismatched "
					"in a proposal.\n");
				goto end;
			}
			break;

		case IPSECDOI_ATTR_ENC_MODE:
			if (pr->encmode &&
			    (uint16_t)pr->encmode != get_uint16(&d->lorv)) {
				plog(PLOG_PROTOERR, PLOGLOC, NULL,
					"multiple encmode exist "
					"in a transform.\n");
				goto end;
			}
			pr->encmode = get_uint16(&d->lorv);
			break;

		case IPSECDOI_ATTR_AUTH:
			if (tr->authtype != IPSECDOI_ATTR_AUTH_NONE) {
				plog(PLOG_PROTOERR, PLOGLOC, NULL,
					"multiple authtype exist "
					"in a transform.\n");
				goto end;
			}
			tr->authtype = get_uint16(&d->lorv);
			break;

		case IPSECDOI_ATTR_KEY_LENGTH:
			if (pr->proto_id != IPSECDOI_PROTO_IPSEC_ESP) {
				plog(PLOG_PROTOERR, PLOGLOC, NULL,
					"key length defined but not ESP");
				goto end;
			}
			tr->encklen = get_uint16(&d->lorv);
			break;

		case IPSECDOI_ATTR_KEY_ROUNDS:
		case IPSECDOI_ATTR_COMP_DICT_SIZE:
		case IPSECDOI_ATTR_COMP_PRIVALG:
		default:
			break;
		}

		prev = d;
		if (flag) {
			tlen -= sizeof(*d);
			d = (struct isakmp_data *)((char *)d + sizeof(*d));
		} else {
			tlen -= (sizeof(*d) + get_uint16(&d->lorv));
			d = (struct isakmp_data *)((caddr_t)d + sizeof(*d) + get_uint16(&d->lorv));
		}
	}

	error = 0;
end:
	return error;
}

int
ipsecdoi_authalg_rct2trnsid(rc_type alg)
{
	switch (alg) {
	case RCT_ALG_HMAC_MD5:
		return IPSECDOI_AH_MD5;
	case RCT_ALG_HMAC_SHA1:
		return IPSECDOI_AH_SHA;
#if 0
	case RCT_ALG_DES_MAC:
		return IPSECDOI_AH_DES;
#endif
	case RCT_ALG_KPDK_MD5:
		return IPSECDOI_AH_MD5;
	case RCT_ALG_KPDK_SHA1:
		return IPSECDOI_AH_SHA;
	default:
		plog(PLOG_INTERR, PLOGLOC, 0,
		     "unsupported authentication algorithm for AH: %s\n",
		     rct2str(alg));
		return -1;
	}
}

#if 0
int
ipsecdoi_authalg2trnsid(alg)
	int alg;
{
	switch (alg) {
        case IPSECDOI_ATTR_AUTH_HMAC_MD5:
		return IPSECDOI_AH_MD5;
        case IPSECDOI_ATTR_AUTH_HMAC_SHA1:
		return IPSECDOI_AH_SHA;
        case IPSECDOI_ATTR_AUTH_DES_MAC:
		return IPSECDOI_AH_DES;
	case IPSECDOI_ATTR_AUTH_KPDK:
		return IPSECDOI_AH_MD5;	/* XXX */
	default:
		plog(PLOG_PROTOERR, PLOGLOC, NULL,
			"invalid authentication algorithm:%d\n", alg);
	}
	return -1;
}
#endif

#ifdef HAVE_GSSAPI
struct isakmpsa *
fixup_initiator_sa(match, received)
	struct isakmpsa *match, *received;
{
	struct isakmpsa *newsa;

	if (received->gssid == NULL)
		return match;

	newsa = newisakmpsa();
	memcpy(newsa, match, sizeof *newsa);

	if (match->dhgrp != NULL) {
		newsa->dhgrp = racoon_calloc(1, sizeof(struct dhgroup));
		memcpy(newsa->dhgrp, match->dhgrp, sizeof (struct dhgroup));
	}
	newsa->next = NULL;
	newsa->rmconf = NULL;

	newsa->gssid = rc_vdup(received->gssid);

	return newsa;
}
#endif

#if 0
static int rm_idtype2doi[] = {
	IPSECDOI_ID_FQDN,
	IPSECDOI_ID_USER_FQDN,
	IPSECDOI_ID_KEY_ID,
	255,	/* it's type of "address"
		 * it expands into 4 types by another function. */
	IPSECDOI_ID_DER_ASN1_DN,
};

/*
 * convert idtype to DOI value.
 * OUT	255  : NG
 *	other: converted.
 */
int
idtype2doi(idtype)
	int idtype;
{
	if (ARRAYLEN(rm_idtype2doi) > idtype)
		return rm_idtype2doi[idtype];
	return 255;
}

int
doi2idtype(doi)
	int doi;
{
	switch(doi) {
	case IPSECDOI_ID_FQDN:
		return(IDTYPE_FQDN);
	case IPSECDOI_ID_USER_FQDN:
		return(IDTYPE_USERFQDN);
	case IPSECDOI_ID_KEY_ID:
		return(IDTYPE_KEYID);
	case IPSECDOI_ID_DER_ASN1_DN:
		return(IDTYPE_ASN1DN);
	case IPSECDOI_ID_IPV4_ADDR:
	case IPSECDOI_ID_IPV4_ADDR_SUBNET:
	case IPSECDOI_ID_IPV6_ADDR:
	case IPSECDOI_ID_IPV6_ADDR_SUBNET:
		return(IDTYPE_ADDRESS);
	default:
		plog(PLOG_PROTOWARN, PLOGLOC, 0,
		     "Improper idtype:%s\n",
		     s_ipsecdoi_ident(doi));
		return(IDTYPE_ADDRESS);	/* XXX */
	}
	/*NOTREACHED*/
}
#endif


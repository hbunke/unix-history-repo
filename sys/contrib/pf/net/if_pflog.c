/*	$FreeBSD$	*/
/*	$OpenBSD: if_pflog.c,v 1.9 2003/05/14 08:42:00 canacar Exp $	*/
/*
 * The authors of this code are John Ioannidis (ji@tla.org),
 * Angelos D. Keromytis (kermit@csd.uch.gr) and 
 * Niels Provos (provos@physnet.uni-hamburg.de).
 *
 * This code was written by John Ioannidis for BSD/OS in Athens, Greece, 
 * in November 1995.
 *
 * Ported to OpenBSD and NetBSD, with additional transforms, in December 1996,
 * by Angelos D. Keromytis.
 *
 * Additional transforms and features in 1997 and 1998 by Angelos D. Keromytis
 * and Niels Provos.
 *
 * Copyright (C) 1995, 1996, 1997, 1998 by John Ioannidis, Angelos D. Keromytis
 * and Niels Provos.
 * Copyright (c) 2001, Angelos D. Keromytis, Niels Provos.
 *
 * Permission to use, copy, and modify this software with or without fee
 * is hereby granted, provided that this entire notice is included in
 * all copies of any software which is or includes a copy or
 * modification of this software. 
 * You may use this code under the GNU public license if you so wish. Please
 * contribute changes back to the authors under this freer than GPL license
 * so that we may further the use of strong encryption without limitations to
 * all.
 *
 * THIS SOFTWARE IS BEING PROVIDED "AS IS", WITHOUT ANY EXPRESS OR
 * IMPLIED WARRANTY. IN PARTICULAR, NONE OF THE AUTHORS MAKES ANY
 * REPRESENTATION OR WARRANTY OF ANY KIND CONCERNING THE
 * MERCHANTABILITY OF THIS SOFTWARE OR ITS FITNESS FOR ANY PARTICULAR
 * PURPOSE.
 */

#if defined(__FreeBSD__)
#include "opt_inet.h"
#include "opt_inet6.h"
#endif

#if !defined(__FreeBSD__)
#include "bpfilter.h"
#include "pflog.h"
#elif __FreeBSD__ >= 5
#include "opt_bpf.h"
#define NBPFILTER DEV_BPF
#include "opt_pf.h"
#define NPFLOG DEV_PFLOG
#endif

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/mbuf.h>
#include <sys/socket.h>
#if defined(__FreeBSD__)
#include <sys/kernel.h>
#include <sys/malloc.h>
#include <sys/sockio.h>
#else
#include <sys/ioctl.h>
#endif

#include <net/if.h>
#include <net/if_types.h>
#include <net/route.h>
#include <net/bpf.h>

#ifdef	INET
#include <netinet/in.h>
#include <netinet/in_var.h>
#include <netinet/in_systm.h>
#include <netinet/ip.h>
#endif

#if defined(__FreeBSD__)
#include <machine/in_cksum.h>
#endif

#ifdef INET6
#ifndef INET
#include <netinet/in.h>
#endif
#include <netinet6/nd6.h>
#endif /* INET6 */

#include <net/pfvar.h>
#include <net/if_pflog.h>

#if defined(__FreeBSD__)
#define PFLOGNAME	"pflog"
#endif

#define PFLOGMTU	(32768 + MHLEN + MLEN)

#ifdef PFLOGDEBUG
#define DPRINTF(x)    do { if (pflogdebug) printf x ; } while (0)
#else
#define DPRINTF(x)
#endif

#if !defined(__FreeBSD__)
struct pflog_softc pflogif[NPFLOG];
#endif

#if defined(__FreeBSD__)
void	pflog_clone_destroy(struct ifnet *);
int	pflog_clone_create(struct if_clone *, int);
#else
void	pflogattach(int);
#endif
int	pflogoutput(struct ifnet *, struct mbuf *, struct sockaddr *,
	    	       struct rtentry *);
int	pflogioctl(struct ifnet *, u_long, caddr_t);
void	pflogrtrequest(int, struct rtentry *, struct sockaddr *);
void	pflogstart(struct ifnet *);

#if !defined(__FreeBSD__)
extern int ifqmaxlen;
#endif

#if defined(__FreeBSD__)
static MALLOC_DEFINE(M_PFLOG, PFLOGNAME, "Packet Filter Logging Interface");
static LIST_HEAD(pflog_list, pflog_softc) pflog_list;
struct if_clone pflog_cloner = IF_CLONE_INITIALIZER(PFLOGNAME,
	pflog_clone_create, pflog_clone_destroy, 1, IF_MAXUNIT);

void
pflog_clone_destroy(struct ifnet *ifp)
{
	struct pflog_softc *sc;

	sc = ifp->if_softc;

	/*
	 * Does we really need this?
	 */
	IF_DRAIN(&ifp->if_snd);

	bpfdetach(ifp);
	if_detach(ifp);
	LIST_REMOVE(sc, sc_next);
	free(sc, M_PFLOG);
}
#endif /* __FreeBSD__ */

#if defined(__FreeBSD__)
int
pflog_clone_create(struct if_clone *ifc, int unit)
{
	struct pflog_softc *sc;

	MALLOC(sc, struct pflog_softc *, sizeof(*sc), M_PFLOG, M_WAITOK|M_ZERO);

#if (__FreeBSD_version < 501113)
        sc->sc_if.if_name = PFLOGNAME;
        sc->sc_if.if_unit = unit;
#else
	if_initname(&sc->sc_if, ifc->ifc_name, unit);
#endif
        sc->sc_if.if_mtu = PFLOGMTU;
        sc->sc_if.if_ioctl = pflogioctl;
        sc->sc_if.if_output = pflogoutput;
        sc->sc_if.if_start = pflogstart;
        sc->sc_if.if_type = IFT_PFLOG;
        sc->sc_if.if_snd.ifq_maxlen = ifqmaxlen;
        sc->sc_if.if_hdrlen = PFLOG_HDRLEN;
        sc->sc_if.if_softc = sc;
	/*
	 * We would get a message like
	 * "in6_ifattach: pflog0 is not multicast capable, IPv6 not enabled".
	 * We need a patch to in6_ifattach() to exclude interface type
	 * IFT_PFLOG.
	 */
        if_attach(&sc->sc_if);

        LIST_INSERT_HEAD(&pflog_list, sc, sc_next);
#if NBPFILTER > 0
	bpfattach(&sc->sc_if, DLT_PFLOG, PFLOG_HDRLEN);
#endif

        return (0);
}
#else /* !__FreeBSD__ */
void
pflogattach(int npflog)
{
	struct ifnet *ifp;
	int i;

	bzero(pflogif, sizeof(pflogif));

	for (i = 0; i < NPFLOG; i++) {
		ifp = &pflogif[i].sc_if;
		snprintf(ifp->if_xname, sizeof ifp->if_xname, "pflog%d", i);
		ifp->if_softc = &pflogif[i];
		ifp->if_mtu = PFLOGMTU;
		ifp->if_ioctl = pflogioctl;
		ifp->if_output = pflogoutput;
		ifp->if_start = pflogstart;
		ifp->if_type = IFT_PFLOG;
		ifp->if_snd.ifq_maxlen = ifqmaxlen;
		ifp->if_hdrlen = PFLOG_HDRLEN;
		if_attach(ifp);
		if_alloc_sadl(ifp);

#if NBPFILTER > 0
		bpfattach(&pflogif[i].sc_if.if_bpf, ifp, DLT_PFLOG,
			  PFLOG_HDRLEN);
#endif
	}
}
#endif /* __FreeBSD__ */

/*
 * Start output on the pflog interface.
 */
void
pflogstart(struct ifnet *ifp)
{
	struct mbuf *m;
#if defined(__FreeBSD__) && defined(ALTQ)
	struct ifaltq *ifq;
#else
	struct ifqueue *ifq;
#endif
	int s;

#if defined(__FreeBSD__)
	ifq = &ifp->if_snd;
#endif
	for (;;) {
		s = splimp();
#if defined(__FreeBSD__)
		IF_LOCK(ifq);
		_IF_DROP(ifq);
		_IF_DEQUEUE(ifq, m);
		IF_UNLOCK(ifq);			
#else
		IF_DROP(&ifp->if_snd);
		IF_DEQUEUE(&ifp->if_snd, m);
#endif
		splx(s);
		if (m == NULL)
			return;
		else
			m_freem(m);
	}
}

int
pflogoutput(struct ifnet *ifp, struct mbuf *m, struct sockaddr *dst,
	struct rtentry *rt)
{
	m_freem(m);
	return (0);
}

/* ARGSUSED */
void
pflogrtrequest(int cmd, struct rtentry *rt, struct sockaddr *sa)
{
	if (rt)
		rt->rt_rmx.rmx_mtu = PFLOGMTU;
}

/* ARGSUSED */
int
pflogioctl(struct ifnet *ifp, u_long cmd, caddr_t data)
{
	switch (cmd) {
	case SIOCSIFADDR:
	case SIOCAIFADDR:
	case SIOCSIFDSTADDR:
	case SIOCSIFFLAGS:
		if (ifp->if_flags & IFF_UP)
			ifp->if_flags |= IFF_RUNNING;
		else
			ifp->if_flags &= ~IFF_RUNNING;
		break;
	default:
		return (EINVAL);
	}

	return (0);
}

int
pflog_packet(struct ifnet *ifp, struct mbuf *m, sa_family_t af, u_int8_t dir,
    u_int8_t reason, struct pf_rule *rm, struct pf_rule *am,
    struct pf_ruleset *ruleset)
{
#if NBPFILTER > 0
	struct ifnet *ifn;
	struct pfloghdr hdr;
	struct mbuf m1;

	if (ifp == NULL || m == NULL || rm == NULL)
		return (-1);

	hdr.length = PFLOG_REAL_HDRLEN;
	hdr.af = af;
	hdr.action = rm->action;
	hdr.reason = reason;
#if defined(__FreeBSD__) && (__FreeBSD_version < 501113)
	snprintf(hdr.ifname, IFNAMSIZ, "%s%d", ifp->if_name, ifp->if_unit);
#else
	memcpy(hdr.ifname, ifp->if_xname, sizeof(hdr.ifname));
#endif

	if (am == NULL) {
		hdr.rulenr = htonl(rm->nr);
		hdr.subrulenr = -1;
		bzero(hdr.ruleset, sizeof(hdr.ruleset));
	} else {
		hdr.rulenr = htonl(am->nr);
		hdr.subrulenr = htonl(rm->nr);
		if (ruleset == NULL)
			bzero(hdr.ruleset, sizeof(hdr.ruleset));
		else
			memcpy(hdr.ruleset, ruleset->name,
			    sizeof(hdr.ruleset));

			
	}
	hdr.dir = dir;

#ifdef INET
	if (af == AF_INET && dir == PF_OUT) {
		struct ip *ip;

		ip = mtod(m, struct ip *);
		ip->ip_sum = 0;
		ip->ip_sum = in_cksum(m, ip->ip_hl << 2);
	}
#endif /* INET */

	m1.m_next = m;
	m1.m_len = PFLOG_HDRLEN;
	m1.m_data = (char *) &hdr;

#if defined(__FreeBSD__)
	KASSERT((!LIST_EMPTY(&pflog_list)), ("pflog: no interface"));
	ifn = &LIST_FIRST(&pflog_list)->sc_if;
#else
	ifn = &(pflogif[0].sc_if);
#endif

	if (ifn->if_bpf)
		bpf_mtap(ifn->if_bpf, &m1);
#endif

	return (0);
}

#if defined(__FreeBSD__)
static int
pflog_modevent(module_t mod, int type, void *data)
{
	int error = 0;

	switch (type) {
	case MOD_LOAD:
		LIST_INIT(&pflog_list);
		if_clone_attach(&pflog_cloner);
		printf("pflog: $Name:  $\n");
		break;

	case MOD_UNLOAD:
		if_clone_detach(&pflog_cloner);
		while (!LIST_EMPTY(&pflog_list))
			pflog_clone_destroy(
				&LIST_FIRST(&pflog_list)->sc_if);
		break;

	default:
		error = EINVAL;
		break;
	}

	return error;
}

static moduledata_t pflog_mod = {
	"pflog",
	pflog_modevent,
	0
};

#define PFLOG_MODVER 1

DECLARE_MODULE(pflog, pflog_mod, SI_SUB_PSEUDO, SI_ORDER_ANY);
MODULE_VERSION(pflog, PFLOG_MODVER);
#endif /* __FreeBSD__ */

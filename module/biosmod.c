/* $dellflash $ */
/*
 * Copyright (c) 2009 Marco Peereboom <marco@peereboom.us>
 * Copyright (c) 2009 Jordan Hargrave <jordan@peereboom.us>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <sys/param.h>
#include <sys/ioctl.h>
#include <sys/systm.h>
#include <sys/cdefs.h>
#include <sys/conf.h>
#include <sys/malloc.h>
#include <sys/exec.h>
#include <sys/lkm.h>
#include <sys/mount.h>
#include <sys/file.h>
#include <sys/errno.h>
#include <sys/syscall.h>
#include <sys/proc.h>
#include <sys/types.h>
#include <sys/syscallargs.h>

#include <uvm/uvm.h>
#include <uvm/uvm_extern.h>

#include <machine/bus.h>
#include <machine/smbiosvar.h>

#include "biosmod.h"

#define DEVNAME(_s)		((_s)->sc_devname)
#define SMBIOS_TYPE_DELLTOKEN	0xd4
#define DELLTOKEN_RBU_SET	0x5c
#define DELLTOKEN_RBU_CLR	0x5d
#define DELLSIG			"dell "

/* #define BIOS_DEBUG */
#ifdef BIOS_DEBUG
#define DPRINTF(x...)		do { if (biosmod_debug) printf(x); } while(0)
#define DNPRINTF(n,x...)	do { if (biosmod_debug & n) printf(x); } while(0)
#define	BIOS_D_MEM		0x0001

uint32_t			biosmod_debug = BIOS_D_MEM;
#else
#define DPRINTF(x...)
#define DNPRINTF(n,x...)
#endif

struct dellindex {
	uint16_t	index_port;
	uint16_t	data_port;
	uint8_t		cksum_type;
	uint8_t		cksum_start;
	uint8_t		cksum_end;
	uint8_t		cksum_index;
} __packed;

struct delltoken {
	uint16_t	token;
	uint8_t		token_index;
	uint8_t		andbits;
	uint8_t		orbits;
} __packed;

struct biosmod_softc {
	char			*sc_devname;

	struct pglist		sc_pl;
	u_int8_t		*sc_va;
	vsize_t			sc_sz;
	int			sc_triggered;
} biosmod_sc = { BIOSMOD_NAME };

struct flashbios_args {
	syscallarg(void *)	data;
	syscallarg(size_t)	datalen;
};

int	flashbios(struct proc *, void *, register_t *);

struct sysent flashbios_ent = {
	2, sizeof(struct flashbios_args), 0, flashbios
};

MOD_SYSCALL(BIOSMOD_NAME, -1, &flashbios_ent);

int
biosmod_allocmem(struct biosmod_softc *sc, size_t size)
{
	struct vm_page		*m;
	struct vm_page		*pg;
	vaddr_t			va = 0, vasave;

	DNPRINTF(BIOS_D_MEM, "%s: biosmod_allocmem: %d\n",
	    DEVNAME(sc), round_page(size));

	TAILQ_INIT(&sc->sc_pl);
	sc->sc_sz = round_page(size);
	if (uvm_pglistalloc(sc->sc_sz, 0x100000, 0xffffffff, PAGE_SIZE, 0,
	    &sc->sc_pl, 1, UVM_PLA_NOWAIT | UVM_PLA_ZERO)) {
		printf("%s: uvm_pglistalloc failed\n", DEVNAME(sc));
		return (1);
	}

	if (uvm_map(kernel_map, &va, sc->sc_sz, NULL, UVM_UNKNOWN_OFFSET,
	    0, UVM_MAPFLAG(UVM_PROT_ALL, UVM_PROT_ALL,
	    UVM_INH_NONE, UVM_ADV_RANDOM, 0))) {
		printf("%s: uvm_map failed\n", DEVNAME(sc));
		goto pgfree;
	}

	m = TAILQ_FIRST(&sc->sc_pl);
	if (m == NULL) {
		printf("%s: no map available\n", DEVNAME(sc));
		goto pgfree;
	}

	sc->sc_va = (u_int8_t *)va;
	vasave = va;
	for (pg = TAILQ_FIRST(&sc->sc_pl); pg != NULL;
	    pg = TAILQ_NEXT(pg, pageq)) {
		pmap_kenter_pa(va, VM_PAGE_TO_PHYS(pg),
			VM_PROT_READ | VM_PROT_WRITE);
		va += PAGE_SIZE;
	}
	pmap_update(pmap_kernel());

	DNPRINTF(BIOS_D_MEM, "    pa %p va %p size %u\n",
	    m->phys_addr, sc->sc_va, sc->sc_sz);

	return (0);

pgfree:
	uvm_pglistfree(&sc->sc_pl);
	return (1);
}

void
biosmod_freemem(struct biosmod_softc *sc)
{
	DNPRINTF(BIOS_D_MEM, "%s: biosmod_freemem\n", DEVNAME(sc));

	pmap_kremove((vaddr_t)sc->sc_va, sc->sc_sz);
	pmap_update(pmap_kernel());
}

int
update_token(struct dellindex *pindex, struct delltoken *ptoken)
{
	int			s, v, oi;

	if (ptoken->token_index >= pindex->cksum_start &&
	    ptoken->token_index <= pindex->cksum_end) {
		printf("set token checksum not implemented\n");
		return (1);
	}

	s = splhigh();

	/* save old index */
	oi = inb(pindex->index_port);

	/* RMW value */
	outb(pindex->index_port, ptoken->token_index);
	v = inb(pindex->data_port);
	v &= ptoken->andbits;
	v |= ptoken->orbits;
	outb(pindex->index_port, ptoken->token_index);
	outb(pindex->data_port, v);

	/* restore index */
	outb(pindex->index_port, oi);
	splx(s);

	return (0);
}

int
set_token(struct biosmod_softc *sc, int token)
{
	struct smbtable		tbl;
	struct dellindex	*pindex;
	struct delltoken	*ptoken;

	bzero(&tbl, sizeof tbl);

	while (smbios_find_table(SMBIOS_TYPE_DELLTOKEN, &tbl)) {
		pindex = tbl.tblhdr;
		ptoken = (void *)&pindex[1];
		while (ptoken->token != 0xffff) {	
			if (ptoken->token == token) {
				update_token(pindex, ptoken);
				return (1);
			}
			ptoken++;
		}
	}

	return (0);
}

int
biosmod_handle(struct lkm_table *lkmtp, int cmd)
{
	int			err = EINVAL;
	struct biosmod_softc	*sc = &biosmod_sc;
	extern char		*hw_vendor;

	if (strncasecmp(hw_vendor, DELLSIG, strlen(DELLSIG))) {
		printf("%s: can not flash BIOS for vendor %s\n",
		    DEVNAME(sc), hw_vendor);
		return (err);
	}

	switch( cmd) {
	case LKM_E_LOAD:
		DNPRINTF(BIOS_D_MEM, "%s: loading\n", DEVNAME(sc));
		if (lkmexists(lkmtp))
			return (EEXIST);
		err = 0;
		break;

	case LKM_E_UNLOAD:
		DNPRINTF(BIOS_D_MEM, "%s: unloading\n", DEVNAME(sc));
		if (sc->sc_triggered) {
			bzero(sc->sc_va, sc->sc_sz);
			biosmod_freemem(sc);
			set_token(sc, DELLTOKEN_RBU_CLR);
			sc->sc_triggered = 0;
			DNPRINTF(BIOS_D_MEM, "%s: flash bios disabled\n",
			    DEVNAME(sc));
		}

		err = 0;
		break;

	default:
		break;
	}

	return (err);
}

biosmod(struct lkm_table *lkmtp, int cmd, int ver)
{
	DISPATCH(lkmtp, cmd, ver, biosmod_handle, biosmod_handle, lkm_nofunc);
}

int
flashbios(struct proc *p, void *args, register_t *ret)
{
	int			rv = EINVAL;
	struct flashbios_args	*a = args;
	struct biosmod_softc	*sc = &biosmod_sc;

	DNPRINTF(BIOS_D_MEM, "%s: flashbios %p\n", DEVNAME(sc), a);

	if (args == NULL || a == NULL)
		goto done;

	if (biosmod_allocmem(sc, SCARG(a, datalen))) {
		rv = ENOMEM;
		goto done;
	}
	sc->sc_triggered = 1;

	if (copyin(SCARG(a, data), sc->sc_va, SCARG(a, datalen))) {
		rv = EFAULT;
		goto done;
	}

	/* flip the bits to enable flash */
	if (!set_token(sc, DELLTOKEN_RBU_SET)) {
		rv = ENODEV;
		goto done;
	}

	rv = 0;
done:
	if (ret)
		*ret = rv;

	return (rv);
}

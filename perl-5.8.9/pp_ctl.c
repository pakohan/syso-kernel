/*    pp_ctl.c
 *
 *    Copyright (C) 1991, 1992, 1993, 1994, 1995, 1996, 1997, 1998, 1999, 2000,
 *    2001, 2002, 2003, 2004, 2005, 2006, 2007, 2008 by Larry Wall and others
 *
 *    You may distribute under the terms of either the GNU General Public
 *    License or the Artistic License, as specified in the README file.
 *
 */

/*
 * Now far ahead the Road has gone,
 * And I must follow, if I can,
 * Pursuing it with eager feet,
 * Until it joins some larger way
 * Where many paths and errands meet.
 * And whither then?  I cannot say.
 */

/* This file contains control-oriented pp ("push/pop") functions that
 * execute the opcodes that make up a perl program. A typical pp function
 * expects to find its arguments on the stack, and usually pushes its
 * results onto the stack, hence the 'pp' terminology. Each OP structure
 * contains a pointer to the relevant pp_foo() function.
 *
 * Control-oriented means things like pp_enteriter() and pp_next(), which
 * alter the flow of control of the program.
 */


#include "EXTERN.h"
#define PERL_IN_PP_CTL_C
#include "perl.h"

#ifndef WORD_ALIGN
#define WORD_ALIGN sizeof(U32)
#endif

#define DOCATCH(o) ((CATCH_GET == TRUE) ? docatch(o) : (o))

#define dopoptosub(plop)	dopoptosub_at(cxstack, (plop))

PP(pp_wantarray)
{
    dSP;
    I32 cxix;
    EXTEND(SP, 1);

    cxix = dopoptosub(cxstack_ix);
    if (cxix < 0)
	RETPUSHUNDEF;

    switch (cxstack[cxix].blk_gimme) {
    case G_ARRAY:
	RETPUSHYES;
    case G_SCALAR:
	RETPUSHNO;
    default:
	RETPUSHUNDEF;
    }
}

PP(pp_regcreset)
{
    /* XXXX Should store the old value to allow for tie/overload - and
       restore in regcomp, where marked with XXXX. */
    PL_reginterp_cnt = 0;
    TAINT_NOT;
    return NORMAL;
}

PP(pp_regcomp)
{
    dSP;
    register PMOP *pm = (PMOP*)cLOGOP->op_other;
    SV *tmpstr;
    MAGIC *mg = NULL;

    tmpstr = POPs;

    /* prevent recompiling under /o and ithreads. */
#if defined(USE_ITHREADS) || defined(USE_5005THREADS)
    if (pm->op_pmflags & PMf_KEEP && PM_GETRE(pm))
	 RETURN;
#endif

    if (SvROK(tmpstr)) {
	SV * const sv = SvRV(tmpstr);
	if(SvMAGICAL(sv))
	    mg = mg_find(sv, PERL_MAGIC_qr);
    }
    if (mg) {
	regexp * const re = (regexp *)mg->mg_obj;
	ReREFCNT_dec(PM_GETRE(pm));
	PM_SETRE(pm, ReREFCNT_inc(re));
    }
    else {
	STRLEN len;
	const char *t = SvPV_const(tmpstr, len);
	regexp * const re = PM_GETRE(pm);

	/* Check against the last compiled regexp. */
	if (!re || !re->precomp || re->prelen != (I32)len ||
	    memNE(re->precomp, t, len))
	{
	    if (re) {
	        ReREFCNT_dec(re);
		PM_SETRE(pm, NULL);	/* crucial if regcomp aborts */
	    }
	    if (PL_op->op_flags & OPf_SPECIAL)
		PL_reginterp_cnt = I32_MAX; /* Mark as safe.  */

	    pm->op_pmflags = pm->op_pmpermflags;	/* reset case sensitivity */
	    if (DO_UTF8(tmpstr))
		pm->op_pmdynflags |= PMdf_DYN_UTF8;
	    else {
		pm->op_pmdynflags &= ~PMdf_DYN_UTF8;
		if (pm->op_pmdynflags & PMdf_UTF8)
		    t = (char*)bytes_to_utf8((U8*)t, &len);
	    }
	    PM_SETRE(pm, CALLREGCOMP(aTHX_ (char *)t, (char *)t + len, pm));
	    if (!DO_UTF8(tmpstr) && (pm->op_pmdynflags & PMdf_UTF8))
		Safefree(t);
	    PL_reginterp_cnt = 0;	/* XXXX Be extra paranoid - needed
					   inside tie/overload accessors.  */
	}
    }

#ifndef INCOMPLETE_TAINTS
    if (PL_tainting) {
	if (PL_tainted)
	    pm->op_pmdynflags |= PMdf_TAINTED;
	else
	    pm->op_pmdynflags &= ~PMdf_TAINTED;
    }
#endif

    if (!PM_GETRE(pm)->prelen && PL_curpm)
	pm = PL_curpm;
    else if (strEQ("\\s+", PM_GETRE(pm)->precomp))
	pm->op_pmflags |= PMf_WHITE;
    else
	pm->op_pmflags &= ~PMf_WHITE;

    /* XXX runtime compiled output needs to move to the pad */
    if (pm->op_pmflags & PMf_KEEP) {
	pm->op_private &= ~OPpRUNTIME;	/* no point compiling again */
#if !defined(USE_ITHREADS) && !defined(USE_5005THREADS)
	/* XXX can't change the optree at runtime either */
	cLOGOP->op_first->op_next = PL_op->op_next;
#endif
    }
    RETURN;
}

PP(pp_substcont)
{
    dSP;
    register PERL_CONTEXT *cx = &cxstack[cxstack_ix];
    register PMOP * const pm = (PMOP*) cLOGOP->op_other;
    register SV * const dstr = cx->sb_dstr;
    register char *s = cx->sb_s;
    register char *m = cx->sb_m;
    char *orig = cx->sb_orig;
    register REGEXP * const rx = cx->sb_rx;
    SV *nsv = NULL;
    REGEXP *old = PM_GETRE(pm);
    if(old != rx) {
	if(old)
	    ReREFCNT_dec(old);
	PM_SETRE(pm,ReREFCNT_inc(rx));
    }

    rxres_restore(&cx->sb_rxres, rx);
    RX_MATCH_UTF8_set(rx, DO_UTF8(cx->sb_targ));

    if (cx->sb_iters++) {
	const I32 saviters = cx->sb_iters;
	if (cx->sb_iters > cx->sb_maxiters)
	    DIE(aTHX_ "Substitution loop");

	if (!(cx->sb_rxtainted & 2) && SvTAINTED(TOPs))
	    cx->sb_rxtainted |= 2;
	sv_catsv(dstr, POPs);

	/* Are we done */
	if (CxONCE(cx) || !CALLREGEXEC(aTHX_ rx, s, cx->sb_strend, orig,
				     s == m, cx->sb_targ, NULL,
				     ((cx->sb_rflags & REXEC_COPY_STR)
				      ? (REXEC_IGNOREPOS|REXEC_NOT_FIRST)
				      : (REXEC_COPY_STR|REXEC_IGNOREPOS|REXEC_NOT_FIRST))))
	{
	    SV * const targ = cx->sb_targ;

	    assert(cx->sb_strend >= s);
	    if(cx->sb_strend > s) {
		 if (DO_UTF8(dstr) && !SvUTF8(targ))
		      sv_catpvn_utf8_upgrade(dstr, s, cx->sb_strend - s, nsv);
		 else
		      sv_catpvn(dstr, s, cx->sb_strend - s);
	    }
	    cx->sb_rxtainted |= RX_MATCH_TAINTED(rx);

	    SvPV_free(targ);
	    SvPV_set(targ, SvPVX(dstr));
	    SvCUR_set(targ, SvCUR(dstr));
	    SvLEN_set(targ, SvLEN(dstr));
	    if (DO_UTF8(dstr))
		SvUTF8_on(targ);
	    SvPV_set(dstr, NULL);

	    TAINT_IF(cx->sb_rxtainted & 1);
	    mPUSHi(saviters - 1);

	    (void)SvPOK_only_UTF8(targ);
	    TAINT_IF(cx->sb_rxtainted);
	    SvSETMAGIC(targ);
	    SvTAINT(targ);

	    LEAVE_SCOPE(cx->sb_oldsave);
	    POPSUBST(cx);
	    RETURNOP(pm->op_next);
	}
	cx->sb_iters = saviters;
    }
    if (RX_MATCH_COPIED(rx) && rx->subbeg != orig) {
	m = s;
	s = orig;
	cx->sb_orig = orig = rx->subbeg;
	s = orig + (m - s);
	cx->sb_strend = s + (cx->sb_strend - m);
    }
    cx->sb_m = m = rx->startp[0] + orig;
    if (m > s) {
	if (DO_UTF8(dstr) && !SvUTF8(cx->sb_targ))
	    sv_catpvn_utf8_upgrade(dstr, s, m - s, nsv);
	else
	    sv_catpvn(dstr, s, m-s);
    }
    cx->sb_s = rx->endp[0] + orig;
    { /* Update the pos() information. */
	SV * const sv = cx->sb_targ;
	MAGIC *mg;
	(void)SvUPGRADE(sv, SVt_PVMG);
	if (!(mg = mg_find(sv, PERL_MAGIC_regex_global))) {
#ifdef PERL_OLD_COPY_ON_WRITE
	    if (SvIsCOW(lsv))
		sv_force_normal_flags(sv, 0);
#endif
	    mg = sv_magicext(sv, NULL, PERL_MAGIC_regex_global, &PL_vtbl_mglob,
			     NULL, 0);
	}
	mg->mg_len = m - orig;
    }
    if (old != rx)
	(void)ReREFCNT_inc(rx);
    cx->sb_rxtainted |= RX_MATCH_TAINTED(rx);
    rxres_save(&cx->sb_rxres, rx);
    RETURNOP(pm->op_pmreplstart);
}

void
Perl_rxres_save(pTHX_ void **rsp, REGEXP *rx)
{
    UV *p = (UV*)*rsp;
    U32 i;
    PERL_UNUSED_CONTEXT;

    if (!p || p[1] < rx->nparens) {
	i = 6 + rx->nparens * 2;
	if (!p)
	    Newx(p, i, UV);
	else
	    Renew(p, i, UV);
	*rsp = (void*)p;
    }

    *p++ = PTR2UV(RX_MATCH_COPIED(rx) ? rx->subbeg : NULL);
    RX_MATCH_COPIED_off(rx);

    *p++ = rx->nparens;

    *p++ = PTR2UV(rx->subbeg);
    *p++ = (UV)rx->sublen;
    for (i = 0; i <= rx->nparens; ++i) {
	*p++ = (UV)rx->startp[i];
	*p++ = (UV)rx->endp[i];
    }
}

void
Perl_rxres_restore(pTHX_ void **rsp, REGEXP *rx)
{
    UV *p = (UV*)*rsp;
    U32 i;
    PERL_UNUSED_CONTEXT;

    if (RX_MATCH_COPIED(rx))
	Safefree(rx->subbeg);
    RX_MATCH_COPIED_set(rx, *p);
    *p++ = 0;

    rx->nparens = *p++;

    rx->subbeg = INT2PTR(char*,*p++);
    rx->sublen = (I32)(*p++);
    for (i = 0; i <= rx->nparens; ++i) {
	rx->startp[i] = (I32)(*p++);
	rx->endp[i] = (I32)(*p++);
    }
}

void
Perl_rxres_free(pTHX_ void **rsp)
{
    UV * const p = (UV*)*rsp;
    PERL_UNUSED_CONTEXT;

    if (p) {
#ifdef PERL_POISON
	void *tmp = INT2PTR(char*,*p);
	Safefree(tmp);
	if (*p)
	    PoisonFree(*p, 1, sizeof(*p));
#else
	Safefree(INT2PTR(char*,*p));
#endif
	Safefree(p);
	*rsp = NULL;
    }
}

PP(pp_formline)
{
    dSP; dMARK; dORIGMARK;
    register SV * const tmpForm = *++MARK;
    register U32 *fpc;
    register char *t;
    const char *f;
    register I32 arg;
    register SV *sv = NULL;
    const char *item = NULL;
    I32 itemsize  = 0;
    I32 fieldsize = 0;
    I32 lines = 0;
    bool chopspace = (strchr(PL_chopset, ' ') != NULL);
    const char *chophere = NULL;
    char *linemark = NULL;
    NV value;
    bool gotsome = FALSE;
    STRLEN len;
    const STRLEN fudge = SvPOK(tmpForm)
			? (SvCUR(tmpForm) * (IN_BYTES ? 1 : 3) + 1) : 0;
    bool item_is_utf8 = FALSE;
    bool targ_is_utf8 = FALSE;
    SV * nsv = NULL;
    OP * parseres = NULL;
    const char *fmt;

    if (!SvMAGICAL(tmpForm) || !SvCOMPILED(tmpForm)) {
	if (SvREADONLY(tmpForm)) {
	    SvREADONLY_off(tmpForm);
	    parseres = doparseform(tmpForm);
	    SvREADONLY_on(tmpForm);
	}
	else
	    parseres = doparseform(tmpForm);
	if (parseres)
	    return parseres;
    }
    SvPV_force(PL_formtarget, len);
    if (DO_UTF8(PL_formtarget))
	targ_is_utf8 = TRUE;
    t = SvGROW(PL_formtarget, len + fudge + 1);  /* XXX SvCUR bad */
    t += len;
    f = SvPV_const(tmpForm, len);
    /* need to jump to the next word */
    fpc = (U32*)(f + len + WORD_ALIGN - SvCUR(tmpForm) % WORD_ALIGN);

    for (;;) {
	DEBUG_f( {
	    const char *name = "???";
	    arg = -1;
	    switch (*fpc) {
	    case FF_LITERAL:	arg = fpc[1]; name = "LITERAL";	break;
	    case FF_BLANK:	arg = fpc[1]; name = "BLANK";	break;
	    case FF_SKIP:	arg = fpc[1]; name = "SKIP";	break;
	    case FF_FETCH:	arg = fpc[1]; name = "FETCH";	break;
	    case FF_DECIMAL:	arg = fpc[1]; name = "DECIMAL";	break;

	    case FF_CHECKNL:	name = "CHECKNL";	break;
	    case FF_CHECKCHOP:	name = "CHECKCHOP";	break;
	    case FF_SPACE:	name = "SPACE";		break;
	    case FF_HALFSPACE:	name = "HALFSPACE";	break;
	    case FF_ITEM:	name = "ITEM";		break;
	    case FF_CHOP:	name = "CHOP";		break;
	    case FF_LINEGLOB:	name = "LINEGLOB";	break;
	    case FF_NEWLINE:	name = "NEWLINE";	break;
	    case FF_MORE:	name = "MORE";		break;
	    case FF_LINEMARK:	name = "LINEMARK";	break;
	    case FF_END:	name = "END";		break;
	    case FF_0DECIMAL:	name = "0DECIMAL";	break;
	    case FF_LINESNGL:	name = "LINESNGL";	break;
	    }
	    if (arg >= 0)
		PerlIO_printf(Perl_debug_log, "%-16s%ld\n", name, (long) arg);
	    else
		PerlIO_printf(Perl_debug_log, "%-16s\n", name);
	} );
	switch (*fpc++) {
	case FF_LINEMARK:
	    linemark = t;
	    lines++;
	    gotsome = FALSE;
	    break;

	case FF_LITERAL:
	    arg = *fpc++;
	    if (targ_is_utf8 && !SvUTF8(tmpForm)) {
		SvCUR_set(PL_formtarget, t - SvPVX_const(PL_formtarget));
		*t = '\0';
		sv_catpvn_utf8_upgrade(PL_formtarget, f, arg, nsv);
		t = SvEND(PL_formtarget);
		f += arg;
		break;
	    }
	    if (!targ_is_utf8 && DO_UTF8(tmpForm)) {
		SvCUR_set(PL_formtarget, t - SvPVX_const(PL_formtarget));
		*t = '\0';
		sv_utf8_upgrade(PL_formtarget);
		SvGROW(PL_formtarget, SvCUR(PL_formtarget) + fudge + 1);
		t = SvEND(PL_formtarget);
		targ_is_utf8 = TRUE;
	    }
	    while (arg--)
		*t++ = *f++;
	    break;

	case FF_SKIP:
	    f += *fpc++;
	    break;

	case FF_FETCH:
	    arg = *fpc++;
	    f += arg;
	    fieldsize = arg;

	    if (MARK < SP)
		sv = *++MARK;
	    else {
		sv = &PL_sv_no;
		if (ckWARN(WARN_SYNTAX))
		    Perl_warner(aTHX_ packWARN(WARN_SYNTAX), "Not enough format arguments");
	    }
	    break;

	case FF_CHECKNL:
	    {
		const char *send;
		const char *s = item = SvPV_const(sv, len);
		itemsize = len;
		if (DO_UTF8(sv)) {
		    itemsize = sv_len_utf8(sv);
		    if (itemsize != (I32)len) {
			I32 itembytes;
			if (itemsize > fieldsize) {
			    itemsize = fieldsize;
			    itembytes = itemsize;
			    sv_pos_u2b(sv, &itembytes, 0);
			}
			else
			    itembytes = len;
			send = chophere = s + itembytes;
			while (s < send) {
			    if (*s & ~31)
				gotsome = TRUE;
			    else if (*s == '\n')
				break;
			    s++;
			}
			item_is_utf8 = TRUE;
			itemsize = s - item;
			sv_pos_b2u(sv, &itemsize);
			break;
		    }
		}
		item_is_utf8 = FALSE;
		if (itemsize > fieldsize)
		    itemsize = fieldsize;
		send = chophere = s + itemsize;
		while (s < send) {
		    if (*s & ~31)
			gotsome = TRUE;
		    else if (*s == '\n')
			break;
		    s++;
		}
		itemsize = s - item;
		break;
	    }

	case FF_CHECKCHOP:
	    {
		const char *s = item = SvPV_const(sv, len);
		itemsize = len;
		if (DO_UTF8(sv)) {
		    itemsize = sv_len_utf8(sv);
		    if (itemsize != (I32)len) {
			I32 itembytes;
			if (itemsize <= fieldsize) {
			    const char *send = chophere = s + itemsize;
			    while (s < send) {
				if (*s == '\r') {
				    itemsize = s - item;
				    chophere = s;
				    break;
				}
				if (*s++ & ~31)
				    gotsome = TRUE;
			    }
			}
			else {
			    const char *send;
			    itemsize = fieldsize;
			    itembytes = itemsize;
			    sv_pos_u2b(sv, &itembytes, 0);
			    send = chophere = s + itembytes;
			    while (s < send || (s == send && isSPACE(*s))) {
				if (isSPACE(*s)) {
				    if (chopspace)
					chophere = s;
				    if (*s == '\r')
					break;
				}
				else {
				    if (*s & ~31)
					gotsome = TRUE;
				    if (strchr(PL_chopset, *s))
					chophere = s + 1;
				}
				s++;
			    }
			    itemsize = chophere - item;
			    sv_pos_b2u(sv, &itemsize);
			}
			item_is_utf8 = TRUE;
			break;
		    }
		}
		item_is_utf8 = FALSE;
		if (itemsize <= fieldsize) {
		    const char *const send = chophere = s + itemsize;
		    while (s < send) {
			if (*s == '\r') {
			    itemsize = s - item;
			    chophere = s;
			    break;
			}
			if (*s++ & ~31)
			    gotsome = TRUE;
		    }
		}
		else {
		    const char *send;
		    itemsize = fieldsize;
		    send = chophere = s + itemsize;
		    while (s < send || (s == send && isSPACE(*s))) {
			if (isSPACE(*s)) {
			    if (chopspace)
				chophere = s;
			    if (*s == '\r')
				break;
			}
			else {
			    if (*s & ~31)
				gotsome = TRUE;
			    if (strchr(PL_chopset, *s))
				chophere = s + 1;
			}
			s++;
		    }
		    itemsize = chophere - item;
		}
		break;
	    }

	case FF_SPACE:
	    arg = fieldsize - itemsize;
	    if (arg) {
		fieldsize -= arg;
		while (arg-- > 0)
		    *t++ = ' ';
	    }
	    break;

	case FF_HALFSPACE:
	    arg = fieldsize - itemsize;
	    if (arg) {
		arg /= 2;
		fieldsize -= arg;
		while (arg-- > 0)
		    *t++ = ' ';
	    }
	    break;

	case FF_ITEM:
	    {
		const char *s = item;
		arg = itemsize;
		if (item_is_utf8) {
		    if (!targ_is_utf8) {
			SvCUR_set(PL_formtarget, t - SvPVX_const(PL_formtarget));
			*t = '\0';
			sv_utf8_upgrade(PL_formtarget);
			SvGROW(PL_formtarget, SvCUR(PL_formtarget) + fudge + 1);
			t = SvEND(PL_formtarget);
			targ_is_utf8 = TRUE;
		    }
		    while (arg--) {
			if (UTF8_IS_CONTINUED(*s)) {
			    STRLEN skip = UTF8SKIP(s);
			    switch (skip) {
			    default:
				Move(s,t,skip,char);
				s += skip;
				t += skip;
				break;
			    case 7: *t++ = *s++;
			    case 6: *t++ = *s++;
			    case 5: *t++ = *s++;
			    case 4: *t++ = *s++;
			    case 3: *t++ = *s++;
			    case 2: *t++ = *s++;
			    case 1: *t++ = *s++;
			    }
			}
			else {
			    if ( !((*t++ = *s++) & ~31) )
				t[-1] = ' ';
			}
		    }
		    break;
		}
		if (targ_is_utf8 && !item_is_utf8) {
		    SvCUR_set(PL_formtarget, t - SvPVX_const(PL_formtarget));
		    *t = '\0';
		    sv_catpvn_utf8_upgrade(PL_formtarget, s, arg, nsv);
		    for (; t < SvEND(PL_formtarget); t++) {
#ifdef EBCDIC
			const int ch = *t;
			if (iscntrl(ch))
#else
			    if (!(*t & ~31))
#endif
				*t = ' ';
		    }
		    break;
		}
		while (arg--) {
#ifdef EBCDIC
		    const int ch = *t++ = *s++;
		    if (iscntrl(ch))
#else
			if ( !((*t++ = *s++) & ~31) )
#endif
			    t[-1] = ' ';
		}
		break;
	    }

	case FF_CHOP:
	    {
		const char *s = chophere;
		if (chopspace) {
		    while (isSPACE(*s))
			s++;
		}
		sv_chop(sv,(char *)s);
		SvSETMAGIC(sv);
		break;
	    }

	case FF_LINESNGL:
	    chopspace = 0;
	case FF_LINEGLOB:
	    {
		const bool oneline = fpc[-1] == FF_LINESNGL;
		const char *s = item = SvPV_const(sv, len);
		item_is_utf8 = DO_UTF8(sv);
		itemsize = len;
		if (itemsize) {
		    STRLEN to_copy = itemsize;
		    const char *const send = s + len;
		    const U8 *source = (const U8 *) s;
		    U8 *tmp = NULL;

		    gotsome = TRUE;
		    chophere = s + itemsize;
		    while (s < send) {
			if (*s++ == '\n') {
			    if (oneline) {
				to_copy = s - SvPVX_const(sv) - 1;
				chophere = s;
				break;
			    } else {
				if (s == send) {
				    itemsize--;
				    to_copy--;
				} else
				    lines++;
			    }
			}
		    }
		    if (targ_is_utf8 && !item_is_utf8) {
			source = tmp = bytes_to_utf8((U8 *)source, &to_copy);
			SvCUR_set(PL_formtarget,
				  t - SvPVX_const(PL_formtarget));
		    } else {
			if (item_is_utf8 && !targ_is_utf8) {
			    /* Upgrade targ to UTF8, and then we reduce it to
			       a problem we have a simple solution for.  */
			    SvCUR_set(PL_formtarget,
				      t - SvPVX_const(PL_formtarget));
			    targ_is_utf8 = TRUE;
			    /* Don't need get magic.  */
			    sv_utf8_upgrade_flags(PL_formtarget, 0);
			} else {
			    SvCUR_set(PL_formtarget,
				      t - SvPVX_const(PL_formtarget));
			}

			/* Easy. They agree.  */
			assert (item_is_utf8 == targ_is_utf8);
		    }
		    SvGROW(PL_formtarget,
			   SvCUR(PL_formtarget) + to_copy + fudge + 1);
		    t = SvPVX(PL_formtarget) + SvCUR(PL_formtarget);

		    Copy(source, t, to_copy, char);
		    t += to_copy;
		    SvCUR_set(PL_formtarget, SvCUR(PL_formtarget) + to_copy);
		    if (item_is_utf8) {
			if (SvGMAGICAL(sv)) {
			    /* Mustn't call sv_pos_b2u() as it does a second
			       mg_get(). Is this a bug? Do we need a _flags()
			       variant? */
			    itemsize = utf8_length((U8 *)source,
						   (U8 *)source + itemsize);
			} else {
			    sv_pos_b2u(sv, &itemsize);
			}
			assert(!tmp);
		    } else if (tmp) {
			Safefree(tmp);
		    }
		}
		break;
	    }

	case FF_0DECIMAL:
	    arg = *fpc++;
#if defined(USE_LONG_DOUBLE)
	    fmt = (const char *)
		((arg & 256) ?
		 "%#0*.*" PERL_PRIfldbl : "%0*.*" PERL_PRIfldbl);
#else
	    fmt = (const char *)
		((arg & 256) ?
		 "%#0*.*f"              : "%0*.*f");
#endif
	    goto ff_dec;
	case FF_DECIMAL:
	    arg = *fpc++;
#if defined(USE_LONG_DOUBLE)
 	    fmt = (const char *)
		((arg & 256) ? "%#*.*" PERL_PRIfldbl : "%*.*" PERL_PRIfldbl);
#else
            fmt = (const char *)
		((arg & 256) ? "%#*.*f"              : "%*.*f");
#endif
	ff_dec:
	    /* If the field is marked with ^ and the value is undefined,
	       blank it out. */
	    if ((arg & 512) && !SvOK(sv)) {
		arg = fieldsize;
		while (arg--)
		    *t++ = ' ';
		break;
	    }
	    gotsome = TRUE;
	    value = SvNV(sv);
	    /* overflow evidence */
	    if (num_overflow(value, fieldsize, arg)) {
	        arg = fieldsize;
		while (arg--)
		    *t++ = '#';
		break;
	    }
	    /* Formats aren't yet marked for locales, so assume "yes". */
	    {
		STORE_NUMERIC_STANDARD_SET_LOCAL();
		my_snprintf(t, SvLEN(PL_formtarget) - (t - SvPVX(PL_formtarget)), fmt, (int) fieldsize, (int) arg & 255, value);
		RESTORE_NUMERIC_STANDARD();
	    }
	    t += fieldsize;
	    break;

	case FF_NEWLINE:
	    f++;
	    while (t-- > linemark && *t == ' ') ;
	    t++;
	    *t++ = '\n';
	    break;

	case FF_BLANK:
	    arg = *fpc++;
	    if (gotsome) {
		if (arg) {		/* repeat until fields exhausted? */
		    *t = '\0';
		    SvCUR_set(PL_formtarget, t - SvPVX_const(PL_formtarget));
		    lines += FmLINES(PL_formtarget);
		    if (lines == 200) {
			arg = t - linemark;
			if (strnEQ(linemark, linemark - arg, arg))
			    DIE(aTHX_ "Runaway format");
		    }
		    if (targ_is_utf8)
			SvUTF8_on(PL_formtarget);
		    FmLINES(PL_formtarget) = lines;
		    SP = ORIGMARK;
		    RETURNOP(cLISTOP->op_first);
		}
	    }
	    else {
		t = linemark;
		lines--;
	    }
	    break;

	case FF_MORE:
	    {
		const char *s = chophere;
		const char *send = item + len;
		if (chopspace) {
		    while (isSPACE(*s) && (s < send))
			s++;
		}
		if (s < send) {
		    char *s1;
		    arg = fieldsize - itemsize;
		    if (arg) {
			fieldsize -= arg;
			while (arg-- > 0)
			    *t++ = ' ';
		    }
		    s1 = t - 3;
		    if (strnEQ(s1,"   ",3)) {
			while (s1 > SvPVX_const(PL_formtarget) && isSPACE(s1[-1]))
			    s1--;
		    }
		    *s1++ = '.';
		    *s1++ = '.';
		    *s1++ = '.';
		}
		break;
	    }
	case FF_END:
	    *t = '\0';
	    SvCUR_set(PL_formtarget, t - SvPVX_const(PL_formtarget));
	    if (targ_is_utf8)
		SvUTF8_on(PL_formtarget);
	    FmLINES(PL_formtarget) += lines;
	    SP = ORIGMARK;
	    RETPUSHYES;
	}
    }
}

PP(pp_grepstart)
{
    dSP;
    SV *src;

    if (PL_stack_base + *PL_markstack_ptr == SP) {
	(void)POPMARK;
	if (GIMME_V == G_SCALAR)
	    mXPUSHi(0);
	RETURNOP(PL_op->op_next->op_next);
    }
    PL_stack_sp = PL_stack_base + *PL_markstack_ptr + 1;
    pp_pushmark();				/* push dst */
    pp_pushmark();				/* push src */
    ENTER;					/* enter outer scope */

    SAVETMPS;
    /* SAVE_DEFSV does *not* suffice here for USE_5005THREADS */
    SAVESPTR(DEFSV);
    ENTER;					/* enter inner scope */
    SAVEVPTR(PL_curpm);

    src = PL_stack_base[*PL_markstack_ptr];
    SvTEMP_off(src);
    DEFSV = src;

    PUTBACK;
    if (PL_op->op_type == OP_MAPSTART)
	pp_pushmark();			/* push top */
    return ((LOGOP*)PL_op->op_next)->op_other;
}

PP(pp_mapwhile)
{
    dSP;
    const I32 gimme = GIMME_V;
    I32 items = (SP - PL_stack_base) - *PL_markstack_ptr; /* how many new items */
    I32 count;
    I32 shift;
    SV** src;
    SV** dst;

    /* first, move source pointer to the next item in the source list */
    ++PL_markstack_ptr[-1];

    /* if there are new items, push them into the destination list */
    if (items && gimme != G_VOID) {
	/* might need to make room back there first */
	if (items > PL_markstack_ptr[-1] - PL_markstack_ptr[-2]) {
	    /* XXX this implementation is very pessimal because the stack
	     * is repeatedly extended for every set of items.  Is possible
	     * to do this without any stack extension or copying at all
	     * by maintaining a separate list over which the map iterates
	     * (like foreach does). --gsar */

	    /* everything in the stack after the destination list moves
	     * towards the end the stack by the amount of room needed */
	    shift = items - (PL_markstack_ptr[-1] - PL_markstack_ptr[-2]);

	    /* items to shift up (accounting for the moved source pointer) */
	    count = (SP - PL_stack_base) - (PL_markstack_ptr[-1] - 1);

	    /* This optimization is by Ben Tilly and it does
	     * things differently from what Sarathy (gsar)
	     * is describing.  The downside of this optimization is
	     * that leaves "holes" (uninitialized and hopefully unused areas)
	     * to the Perl stack, but on the other hand this
	     * shouldn't be a problem.  If Sarathy's idea gets
	     * implemented, this optimization should become
	     * irrelevant.  --jhi */
            if (shift < count)
                shift = count; /* Avoid shifting too often --Ben Tilly */

	    EXTEND(SP,shift);
	    src = SP;
	    dst = (SP += shift);
	    PL_markstack_ptr[-1] += shift;
	    *PL_markstack_ptr += shift;
	    while (count--)
		*dst-- = *src--;
	}
	/* copy the new items down to the destination list */
	dst = PL_stack_base + (PL_markstack_ptr[-2] += items) - 1;
	if (gimme == G_ARRAY) {
	    while (items-- > 0)
		*dst-- = SvTEMP(TOPs) ? POPs : sv_mortalcopy(POPs);
	}
	else {
	    /* scalar context: we don't care about which values map returns
	     * (we use undef here). And so we certainly don't want to do mortal
	     * copies of meaningless values. */
	    while (items-- > 0) {
		(void)POPs;
		*dst-- = &PL_sv_undef;
	    }
	}
    }
    LEAVE;					/* exit inner scope */

    /* All done yet? */
    if (PL_markstack_ptr[-1] > *PL_markstack_ptr) {

	(void)POPMARK;				/* pop top */
	LEAVE;					/* exit outer scope */
	(void)POPMARK;				/* pop src */
	items = --*PL_markstack_ptr - PL_markstack_ptr[-1];
	(void)POPMARK;				/* pop dst */
	SP = PL_stack_base + POPMARK;		/* pop original mark */
	if (gimme == G_SCALAR) {
	    dTARGET;
	    XPUSHi(items);
	}
	else if (gimme == G_ARRAY)
	    SP += items;
	RETURN;
    }
    else {
	SV *src;

	ENTER;					/* enter inner scope */
	SAVEVPTR(PL_curpm);

	/* set $_ to the new source item */
	src = PL_stack_base[PL_markstack_ptr[-1]];
	SvTEMP_off(src);
	DEFSV = src;

	RETURNOP(cLOGOP->op_other);
    }
}

/* Range stuff. */

PP(pp_range)
{
    if (GIMME == G_ARRAY)
	return NORMAL;
    if (SvTRUEx(PAD_SV(PL_op->op_targ)))
	return cLOGOP->op_other;
    else
	return NORMAL;
}

PP(pp_flip)
{
    dSP;

    if (GIMME == G_ARRAY) {
	RETURNOP(((LOGOP*)cUNOP->op_first)->op_other);
    }
    else {
	dTOPss;
	SV * const targ = PAD_SV(PL_op->op_targ);
	int flip = 0;

	if (PL_op->op_private & OPpFLIP_LINENUM) {
	    if (GvIO(PL_last_in_gv)) {
		flip = SvIV(sv) == (IV)IoLINES(GvIOp(PL_last_in_gv));
	    }
	    else {
		GV * const gv = gv_fetchpvs(".", GV_ADD|GV_NOTQUAL, SVt_PV);
		if (gv && GvSV(gv))
		    flip = SvIV(sv) == SvIV(GvSV(gv));
	    }
	} else {
	    flip = SvTRUE(sv);
	}
	if (flip) {
	    sv_setiv(PAD_SV(cUNOP->op_first->op_targ), 1);
	    if (PL_op->op_flags & OPf_SPECIAL) {
		sv_setiv(targ, 1);
		SETs(targ);
		RETURN;
	    }
	    else {
		sv_setiv(targ, 0);
		SP--;
		RETURNOP(((LOGOP*)cUNOP->op_first)->op_other);
	    }
	}
	sv_setpvn(TARG, "", 0);
	SETs(targ);
	RETURN;
    }
}

/* This code tries to decide if "$left .. $right" should use the
   magical string increment, or if the range is numeric (we make
   an exception for .."0" [#18165]). AMS 20021031. */

#define RANGE_IS_NUMERIC(left,right) ( \
	SvNIOKp(left)  || (SvOK(left)  && !SvPOKp(left))  || \
	SvNIOKp(right) || (SvOK(right) && !SvPOKp(right)) || \
	(((!SvOK(left) && SvOK(right)) || ((!SvOK(left) || \
          looks_like_number(left)) && SvPOKp(left) && *SvPVX_const(left) != '0')) \
         && (!SvOK(right) || looks_like_number(right))))

PP(pp_flop)
{
    dSP;

    if (GIMME == G_ARRAY) {
	dPOPPOPssrl;

	SvGETMAGIC(left);
	SvGETMAGIC(right);

	if (RANGE_IS_NUMERIC(left,right)) {
	    register IV i, j;
	    IV max;
	    if ((SvOK(left) && SvNV(left) < IV_MIN) ||
		(SvOK(right) && SvNV(right) > IV_MAX))
		DIE(aTHX_ "Range iterator outside integer range");
	    i = SvIV(left);
	    max = SvIV(right);
	    if (max >= i) {
		j = max - i + 1;
		EXTEND_MORTAL(j);
		EXTEND(SP, j);
	    }
	    else
		j = 0;
	    while (j--) {
		SV * const sv = sv_2mortal(newSViv(i++));
		PUSHs(sv);
	    }
	}
	else {
	    SV * const final = sv_mortalcopy(right);
	    STRLEN len;
	    const char * const tmps = SvPV_const(final, len);

	    SV *sv = sv_mortalcopy(left);
	    SvPV_force_nolen(sv);
	    while (!SvNIOKp(sv) && SvCUR(sv) <= len) {
		XPUSHs(sv);
	        if (strEQ(SvPVX_const(sv),tmps))
	            break;
		sv = sv_2mortal(newSVsv(sv));
		sv_inc(sv);
	    }
	}
    }
    else {
	dTOPss;
	SV * const targ = PAD_SV(cUNOP->op_first->op_targ);
	int flop = 0;
	sv_inc(targ);

	if (PL_op->op_private & OPpFLIP_LINENUM) {
	    if (GvIO(PL_last_in_gv)) {
		flop = SvIV(sv) == (IV)IoLINES(GvIOp(PL_last_in_gv));
	    }
	    else {
		GV * const gv = gv_fetchpvs(".", GV_ADD|GV_NOTQUAL, SVt_PV);
		if (gv && GvSV(gv)) flop = SvIV(sv) == SvIV(GvSV(gv));
	    }
	}
	else {
	    flop = SvTRUE(sv);
	}

	if (flop) {
	    sv_setiv(PAD_SV(((UNOP*)cUNOP->op_first)->op_first->op_targ), 0);
	    sv_catpvs(targ, "E0");
	}
	SETs(targ);
    }

    RETURN;
}

/* Control. */

static const char * const context_name[] = {
    "pseudo-block",
    "subroutine",
    "eval",
    "loop",
    "substitution",
    "block",
    "format"
};

STATIC I32
S_dopoptolabel(pTHX_ const char *label)
{
    register I32 i;

    for (i = cxstack_ix; i >= 0; i--) {
	register const PERL_CONTEXT * const cx = &cxstack[i];
	switch (CxTYPE(cx)) {
	case CXt_SUBST:
	case CXt_SUB:
	case CXt_FORMAT:
	case CXt_EVAL:
	case CXt_NULL:
	    if (ckWARN(WARN_EXITING))
		Perl_warner(aTHX_ packWARN(WARN_EXITING), "Exiting %s via %s",
			context_name[CxTYPE(cx)], OP_NAME(PL_op));
	    if (CxTYPE(cx) == CXt_NULL)
		return -1;
	    break;
	case CXt_LOOP:
	    if ( !cx->blk_loop.label || strNE(label, cx->blk_loop.label) ) {
		DEBUG_l(Perl_deb(aTHX_ "(Skipping label #%ld %s)\n",
			(long)i, cx->blk_loop.label));
		continue;
	    }
	    DEBUG_l( Perl_deb(aTHX_ "(Found label #%ld %s)\n", (long)i, label));
	    return i;
	}
    }
    return i;
}



I32
Perl_dowantarray(pTHX)
{
    const I32 gimme = block_gimme();
    return (gimme == G_VOID) ? G_SCALAR : gimme;
}

I32
Perl_block_gimme(pTHX)
{
    const I32 cxix = dopoptosub(cxstack_ix);
    if (cxix < 0)
	return G_VOID;

    switch (cxstack[cxix].blk_gimme) {
    case G_VOID:
	return G_VOID;
    case G_SCALAR:
	return G_SCALAR;
    case G_ARRAY:
	return G_ARRAY;
    default:
	Perl_croak(aTHX_ "panic: bad gimme: %d\n", cxstack[cxix].blk_gimme);
	/* NOTREACHED */
	return 0;
    }
}

I32
Perl_is_lvalue_sub(pTHX)
{
    const I32 cxix = dopoptosub(cxstack_ix);
    assert(cxix >= 0);  /* We should only be called from inside subs */

    if (CxLVAL(cxstack + cxix) && CvLVALUE(cxstack[cxix].blk_sub.cv))
	return CxLVAL(cxstack + cxix);
    else
	return 0;
}

STATIC I32
S_dopoptosub_at(pTHX_ const PERL_CONTEXT *cxstk, I32 startingblock)
{
    I32 i;
    for (i = startingblock; i >= 0; i--) {
	register const PERL_CONTEXT * const cx = &cxstk[i];
	switch (CxTYPE(cx)) {
	default:
	    continue;
	case CXt_EVAL:
	case CXt_SUB:
	case CXt_FORMAT:
	    DEBUG_l( Perl_deb(aTHX_ "(Found sub #%ld)\n", (long)i));
	    return i;
	}
    }
    return i;
}

STATIC I32
S_dopoptoeval(pTHX_ I32 startingblock)
{
    I32 i;
    for (i = startingblock; i >= 0; i--) {
	register const PERL_CONTEXT *cx = &cxstack[i];
	switch (CxTYPE(cx)) {
	default:
	    continue;
	case CXt_EVAL:
	    DEBUG_l( Perl_deb(aTHX_ "(Found eval #%ld)\n", (long)i));
	    return i;
	}
    }
    return i;
}

STATIC I32
S_dopoptoloop(pTHX_ I32 startingblock)
{
    I32 i;
    for (i = startingblock; i >= 0; i--) {
	register const PERL_CONTEXT * const cx = &cxstack[i];
	switch (CxTYPE(cx)) {
	case CXt_SUBST:
	case CXt_SUB:
	case CXt_FORMAT:
	case CXt_EVAL:
	case CXt_NULL:
	    if (ckWARN(WARN_EXITING))
		Perl_warner(aTHX_ packWARN(WARN_EXITING), "Exiting %s via %s",
			context_name[CxTYPE(cx)], OP_NAME(PL_op));
	    if ((CxTYPE(cx)) == CXt_NULL)
		return -1;
	    break;
	case CXt_LOOP:
	    DEBUG_l( Perl_deb(aTHX_ "(Found loop #%ld)\n", (long)i));
	    return i;
	}
    }
    return i;
}

void
Perl_dounwind(pTHX_ I32 cxix)
{
    I32 optype;

    while (cxstack_ix > cxix) {
	SV *sv;
        register PERL_CONTEXT *cx = &cxstack[cxstack_ix];
	DEBUG_l(PerlIO_printf(Perl_debug_log, "Unwinding block %ld, type %s\n",
			      (long) cxstack_ix, PL_block_type[CxTYPE(cx)]));
	/* Note: we don't need to restore the base context info till the end. */
	switch (CxTYPE(cx)) {
	case CXt_SUBST:
	    POPSUBST(cx);
	    continue;  /* not break */
	case CXt_SUB:
	    POPSUB(cx,sv);
	    LEAVESUB(sv);
	    break;
	case CXt_EVAL:
	    POPEVAL(cx);
	    break;
	case CXt_LOOP:
	    POPLOOP(cx);
	    break;
	case CXt_NULL:
	    break;
	case CXt_FORMAT:
	    POPFORMAT(cx);
	    break;
	}
	cxstack_ix--;
    }
    PERL_UNUSED_VAR(optype);
}

void
Perl_qerror(pTHX_ SV *err)
{
    if (PL_in_eval)
	sv_catsv(ERRSV, err);
    else if (PL_errors)
	sv_catsv(PL_errors, err);
    else
	Perl_warn(aTHX_ "%"SVf, (void*)err);
    ++PL_error_count;
}

OP *
Perl_die_where(pTHX_ char *message, STRLEN msglen)
{
    if (PL_in_eval) {
	I32 cxix;
	I32 gimme;

	if (message) {
	    if (PL_in_eval & EVAL_KEEPERR) {
                static const char prefix[] = "\t(in cleanup) ";
		SV * const err = ERRSV;
		const char *e = NULL;
		if (!SvPOK(err))
		    sv_setpvn(err,"",0);
		else if (SvCUR(err) >= sizeof(prefix)+msglen-1) {
		    STRLEN len;
		    e = SvPV_const(err, len);
		    e += len - msglen;
		    if (*e != *message || strNE(e,message))
			e = NULL;
		}
		if (!e) {
		    SvGROW(err, SvCUR(err)+sizeof(prefix)+msglen);
		    sv_catpvn(err, prefix, sizeof(prefix)-1);
		    sv_catpvn(err, message, msglen);
		    if (ckWARN(WARN_MISC)) {
			const STRLEN start = SvCUR(err)-msglen-sizeof(prefix)+1;
			Perl_warner(aTHX_ packWARN(WARN_MISC), "%s",
				SvPVX_const(err)+start);
		    }
		}
	    }
	    else {
		sv_setpvn(ERRSV, message, msglen);
	    }
	}

	while ((cxix = dopoptoeval(cxstack_ix)) < 0
	       && PL_curstackinfo->si_prev)
	{
	    dounwind(-1);
	    POPSTACK;
	}

	if (cxix >= 0) {
	    I32 optype;
	    register PERL_CONTEXT *cx;
	    SV **newsp;

	    if (cxix < cxstack_ix)
		dounwind(cxix);

	    POPBLOCK(cx,PL_curpm);
	    if (CxTYPE(cx) != CXt_EVAL) {
		if (!message)
		    message = (char *)SvPVx_const(ERRSV, msglen);
		PerlIO_write(Perl_error_log, (const char *)"panic: die ", 11);
		PerlIO_write(Perl_error_log, message, msglen);
		my_exit(1);
	    }
	    POPEVAL(cx);

	    if (gimme == G_SCALAR)
		*++newsp = &PL_sv_undef;
	    PL_stack_sp = newsp;

	    LEAVE;

	    /* LEAVE could clobber PL_curcop (see save_re_context())
	     * XXX it might be better to find a way to avoid messing with
	     * PL_curcop in save_re_context() instead, but this is a more
	     * minimal fix --GSAR */
	    PL_curcop = cx->blk_oldcop;

	    if (optype == OP_REQUIRE) {
		const char* msg = SvPVx_nolen_const(ERRSV);
		DIE(aTHX_ "%sCompilation failed in require",
		    *msg ? msg : "Unknown error\n");
	    }
	    return pop_return();
	}
    }
    if (!message)
	message = (char *)SvPVx_const(ERRSV, msglen);

    write_to_stderr(message, msglen);
    my_failure_exit();
    /* NOTREACHED */
    return 0;
}

PP(pp_xor)
{
    dSP; dPOPTOPssrl;
    if (SvTRUE(left) != SvTRUE(right))
	RETSETYES;
    else
	RETSETNO;
}

PP(pp_caller)
{
    dSP;
    register I32 cxix = dopoptosub(cxstack_ix);
    register const PERL_CONTEXT *cx;
    register const PERL_CONTEXT *ccstack = cxstack;
    const PERL_SI *top_si = PL_curstackinfo;
    I32 gimme;
    const char *stashname;
    I32 count = 0;

    if (MAXARG)
	count = POPi;

    for (;;) {
	/* we may be in a higher stacklevel, so dig down deeper */
	while (cxix < 0 && top_si->si_type != PERLSI_MAIN) {
	    top_si = top_si->si_prev;
	    ccstack = top_si->si_cxstack;
	    cxix = dopoptosub_at(ccstack, top_si->si_cxix);
	}
	if (cxix < 0) {
	    if (GIMME != G_ARRAY) {
		EXTEND(SP, 1);
		RETPUSHUNDEF;
            }
	    RETURN;
	}
	/* caller() should not report the automatic calls to &DB::sub */
	if (PL_DBsub && GvCV(PL_DBsub) && cxix >= 0 &&
		ccstack[cxix].blk_sub.cv == GvCV(PL_DBsub))
	    count++;
	if (!count--)
	    break;
	cxix = dopoptosub_at(ccstack, cxix - 1);
    }

    cx = &ccstack[cxix];
    if (CxTYPE(cx) == CXt_SUB || CxTYPE(cx) == CXt_FORMAT) {
        const I32 dbcxix = dopoptosub_at(ccstack, cxix - 1);
	/* We expect that ccstack[dbcxix] is CXt_SUB, anyway, the
	   field below is defined for any cx. */
	/* caller() should not report the automatic calls to &DB::sub */
	if (PL_DBsub && GvCV(PL_DBsub) && dbcxix >= 0 && ccstack[dbcxix].blk_sub.cv == GvCV(PL_DBsub))
	    cx = &ccstack[dbcxix];
    }

    stashname = CopSTASHPV(cx->blk_oldcop);
    if (GIMME != G_ARRAY) {
        EXTEND(SP, 1);
	if (!stashname)
	    PUSHs(&PL_sv_undef);
	else {
	    dTARGET;
	    sv_setpv(TARG, stashname);
	    PUSHs(TARG);
	}
	RETURN;
    }

    EXTEND(SP, 10);

    if (!stashname)
	PUSHs(&PL_sv_undef);
    else
	mPUSHs(newSVpv(stashname, 0));
    mPUSHs(newSVpv(OutCopFILE(cx->blk_oldcop), 0));
    mPUSHi((I32)CopLINE(cx->blk_oldcop));
    if (!MAXARG)
	RETURN;
    if (CxTYPE(cx) == CXt_SUB || CxTYPE(cx) == CXt_FORMAT) {
	GV * const cvgv = CvGV(ccstack[cxix].blk_sub.cv);
	/* So is ccstack[dbcxix]. */
	if (isGV(cvgv)) {
	    SV * const sv = newSV(0);
	    gv_efullname3(sv, cvgv, NULL);
	    mPUSHs(sv);
	    mPUSHi((I32)(I32)CxHASARGS(cx));
	}
	else {
	    PUSHs(newSVpvs_flags("(unknown)", SVs_TEMP));
	    mPUSHi((I32)CxHASARGS(cx));
	}
    }
    else {
	PUSHs(newSVpvs_flags("(eval)", SVs_TEMP));
	mPUSHi(0);
    }
    gimme = (I32)cx->blk_gimme;
    if (gimme == G_VOID)
	PUSHs(&PL_sv_undef);
    else
	mPUSHi(gimme & G_ARRAY);
    if (CxTYPE(cx) == CXt_EVAL) {
	/* eval STRING */
	if (CxOLD_OP_TYPE(cx) == OP_ENTEREVAL) {
	    PUSHs(cx->blk_eval.cur_text);
	    PUSHs(&PL_sv_no);
	}
	/* require */
	else if (cx->blk_eval.old_namesv) {
	    mPUSHs(newSVsv(cx->blk_eval.old_namesv));
	    PUSHs(&PL_sv_yes);
	}
	/* eval BLOCK (try blocks have old_namesv == 0) */
	else {
	    PUSHs(&PL_sv_undef);
	    PUSHs(&PL_sv_undef);
	}
    }
    else {
	PUSHs(&PL_sv_undef);
	PUSHs(&PL_sv_undef);
    }
    if (CxTYPE(cx) == CXt_SUB && CxHASARGS(cx)
	&& CopSTASH_eq(PL_curcop, PL_debstash))
    {
	AV * const ary = cx->blk_sub.argarray;
	const int off = AvARRAY(ary) - AvALLOC(ary);

	if (!PL_dbargs) {
	    GV* const tmpgv = gv_fetchpvs("DB::args", GV_ADD, SVt_PVAV);
	    PL_dbargs = GvAV(gv_AVadd(tmpgv));
	    GvMULTI_on(tmpgv);
	    AvREAL_off(PL_dbargs);	/* XXX should be REIFY (see av.h) */
	}

	if (AvMAX(PL_dbargs) < AvFILLp(ary) + off)
	    av_extend(PL_dbargs, AvFILLp(ary) + off);
	Copy(AvALLOC(ary), AvARRAY(PL_dbargs), AvFILLp(ary) + 1 + off, SV*);
	AvFILLp(PL_dbargs) = AvFILLp(ary) + off;
    }
    /* XXX only hints propagated via op_private are currently
     * visible (others are not easily accessible, since they
     * use the global PL_hints) */
    mPUSHi(CopHINTS_get(cx->blk_oldcop));
    {
	SV * mask ;
	SV * const old_warnings = cx->blk_oldcop->cop_warnings ;

	if  (old_warnings == pWARN_NONE ||
		(old_warnings == pWARN_STD && (PL_dowarn & G_WARN_ON) == 0))
            mask = newSVpvn(WARN_NONEstring, WARNsize) ;
        else if (old_warnings == pWARN_ALL ||
		  (old_warnings == pWARN_STD && PL_dowarn & G_WARN_ON)) {
	    /* Get the bit mask for $warnings::Bits{all}, because
	     * it could have been extended by warnings::register */
	    SV **bits_all;
	    HV * const bits = get_hv("warnings::Bits", FALSE);
	    if (bits && (bits_all=hv_fetchs(bits, "all", FALSE))) {
		mask = newSVsv(*bits_all);
	    }
	    else {
		mask = newSVpvn(WARN_ALLstring, WARNsize) ;
	    }
	}
        else
            mask = newSVsv(old_warnings);
        mPUSHs(mask);
    }
    RETURN;
}

PP(pp_reset)
{
    dSP;
    const char * const tmps = (MAXARG < 1) ? (const char *)"" : POPpconstx;
    sv_reset((char *)tmps, CopSTASH(PL_curcop));
    PUSHs(&PL_sv_yes);
    RETURN;
}

/* like pp_nextstate, but used instead when the debugger is active */

PP(pp_dbstate)
{
    PL_curcop = (COP*)PL_op;
    TAINT_NOT;		/* Each statement is presumed innocent */
    PL_stack_sp = PL_stack_base + cxstack[cxstack_ix].blk_oldsp;
    FREETMPS;

    if (PL_op->op_flags & OPf_SPECIAL /* breakpoint */
	    || SvIV(PL_DBsingle) || SvIV(PL_DBsignal) || SvIV(PL_DBtrace))
    {
	dSP;
	register PERL_CONTEXT *cx;
	const I32 gimme = G_ARRAY;
	U8 hasargs;
	GV * const gv = PL_DBgv;
	register CV * const cv = GvCV(gv);

	if (!cv)
	    DIE(aTHX_ "No DB::DB routine defined");

	if (CvDEPTH(cv) >= 1 && !(PL_debug & DEBUG_DB_RECURSE_FLAG))
	    /* don't do recursive DB::DB call */
	    return NORMAL;

	ENTER;
	SAVETMPS;

	SAVEI32(PL_debug);
	SAVESTACK_POS();
	PL_debug = 0;
	hasargs = 0;
	SPAGAIN;

	if (CvISXSUB(cv)) {
	    CvDEPTH(cv)++;
	    PUSHMARK(SP);
	    (void)(*CvXSUB(cv))(aTHX_ cv);

	    CvDEPTH(cv)--;
	    FREETMPS;
	    LEAVE;
	    return NORMAL;
	} else {
	    push_return(PL_op->op_next);
	    PUSHBLOCK(cx, CXt_SUB, SP);
	    PUSHSUB_DB(cx);
	    CvDEPTH(cv)++;
	    SAVECOMPPAD();
	    PAD_SET_CUR_NOSAVE(CvPADLIST(cv), 1);
	    RETURNOP(CvSTART(cv));
	}
    }
    else
	return NORMAL;
}

PP(pp_enteriter)
{
    dSP; dMARK;
    register PERL_CONTEXT *cx;
    const I32 gimme = GIMME_V;
    SV **svp;
    U32 cxtype = CXt_LOOP | CXp_FOREACH;
#ifdef USE_ITHREADS
    void *iterdata;
#endif

    ENTER;
    SAVETMPS;

#ifdef USE_5005THREADS
    if (PL_op->op_flags & OPf_SPECIAL) {
	svp = &THREADSV(PL_op->op_targ);	/* per-thread variable */
	SAVEGENERICSV(*svp);
	*svp = newSV(0);
    }
    else
#endif /* USE_5005THREADS */
    if (PL_op->op_targ) {
	SAVEPADSVANDMORTALIZE(PL_op->op_targ);
#ifndef USE_ITHREADS
	svp = &PAD_SVl(PL_op->op_targ);		/* "my" variable */
#else
	iterdata = INT2PTR(void*, PL_op->op_targ);
	cxtype |= CXp_PADVAR;
#endif
    }
    else {
	GV * const gv = (GV*)POPs;
	svp = &GvSV(gv);			/* symbol table variable */
	SAVEGENERICSV(*svp);
	*svp = newSV(0);
#ifdef USE_ITHREADS
	iterdata = (void*)gv;
#endif
    }

    if (PL_op->op_private & OPpITER_DEF)
	cxtype |= CXp_FOR_DEF;

    ENTER;

    PUSHBLOCK(cx, cxtype, SP);
#ifdef USE_ITHREADS
    PUSHLOOP(cx, iterdata, MARK);
#else
    PUSHLOOP(cx, svp, MARK);
#endif
    if (PL_op->op_flags & OPf_STACKED) {
	cx->blk_loop.iterary = (AV*)SvREFCNT_inc(POPs);
	if (SvTYPE(cx->blk_loop.iterary) != SVt_PVAV) {
	    dPOPss;
	    SV * const right = (SV*)cx->blk_loop.iterary;
	    SvGETMAGIC(sv);
	    SvGETMAGIC(right);
	    if (RANGE_IS_NUMERIC(sv,right)) {
		if ((SvOK(sv) && SvNV(sv) < IV_MIN) ||
		    (SvOK(right) && SvNV(right) >= IV_MAX))
		    DIE(aTHX_ "Range iterator outside integer range");
		cx->blk_loop.iterix = SvIV(sv);
		cx->blk_loop.itermax = SvIV(right);
#ifdef DEBUGGING
		/* for correct -Dstv display */
		cx->blk_oldsp = sp - PL_stack_base;
#endif
	    }
	    else {
		cx->blk_loop.iterlval = newSVsv(sv);
		(void) SvPV_force_nolen(cx->blk_loop.iterlval);
		/* This will do the upgrade to SVt_PV, and warn if the value
		   is uninitialised.  */
		(void) SvPV_nolen_const(right);
		/* Doing this avoids a check every time in pp_iter in pp_hot.c
		   to replace !SvOK() with a pointer to "".  */
		if (!SvOK(right)) {
		    SvREFCNT_dec(right);
		    cx->blk_loop.iterary = (AV*) &PL_sv_no;
		}
	    }
	}
	else if (PL_op->op_private & OPpITER_REVERSED) {
	    cx->blk_loop.itermax = 0;
	    cx->blk_loop.iterix = AvFILL(cx->blk_loop.iterary) + 1;

	}
    }
    else {
	cx->blk_loop.iterary = PL_curstack;
	if (PL_op->op_private & OPpITER_REVERSED) {
	    cx->blk_loop.itermax = MARK - PL_stack_base + 1;
	    cx->blk_loop.iterix = cx->blk_oldsp + 1;
	}
	else {
	    cx->blk_loop.iterix = MARK - PL_stack_base;
	}
    }

    RETURN;
}

PP(pp_enterloop)
{
    dSP;
    register PERL_CONTEXT *cx;
    const I32 gimme = GIMME_V;

    ENTER;
    SAVETMPS;
    ENTER;

    PUSHBLOCK(cx, CXt_LOOP, SP);
    PUSHLOOP(cx, 0, SP);

    RETURN;
}

PP(pp_leaveloop)
{
    dSP;
    register PERL_CONTEXT *cx;
    I32 gimme;
    SV **newsp;
    PMOP *newpm;
    SV **mark;

    POPBLOCK(cx,newpm);
    assert(CxTYPE(cx) == CXt_LOOP);
    mark = newsp;
    newsp = PL_stack_base + cx->blk_loop.resetsp;

    TAINT_NOT;
    if (gimme == G_VOID)
	NOOP;
    else if (gimme == G_SCALAR) {
	if (mark < SP)
	    *++newsp = sv_mortalcopy(*SP);
	else
	    *++newsp = &PL_sv_undef;
    }
    else {
	while (mark < SP) {
	    *++newsp = sv_mortalcopy(*++mark);
	    TAINT_NOT;		/* Each item is independent */
	}
    }
    SP = newsp;
    PUTBACK;

    POPLOOP(cx);	/* Stack values are safe: release loop vars ... */
    PL_curpm = newpm;	/* ... and pop $1 et al */

    LEAVE;
    LEAVE;

    return NORMAL;
}

PP(pp_return)
{
    dSP; dMARK;
    register PERL_CONTEXT *cx;
    bool popsub2 = FALSE;
    bool clear_errsv = FALSE;
    I32 gimme;
    SV **newsp;
    PMOP *newpm;
    I32 optype = 0;
    SV *sv;

    const I32 cxix = dopoptosub(cxstack_ix);

    if (cxix < 0) {
	if (CxMULTICALL(cxstack)) { /* In this case we must be in a
				     * sort block, which is a CXt_NULL
				     * not a CXt_SUB */
	    dounwind(0);
	    PL_stack_base[1] = *PL_stack_sp;
	    PL_stack_sp = PL_stack_base + 1;
	    return 0;
	}
	else
	    DIE(aTHX_ "Can't return outside a subroutine");
    }
    if (cxix < cxstack_ix)
	dounwind(cxix);

    if (CxMULTICALL(&cxstack[cxix])) {
	gimme = cxstack[cxix].blk_gimme;
	if (gimme == G_VOID)
	    PL_stack_sp = PL_stack_base;
	else if (gimme == G_SCALAR) {
	    PL_stack_base[1] = *PL_stack_sp;
	    PL_stack_sp = PL_stack_base + 1;
	}
	return 0;
    }

    POPBLOCK(cx,newpm);
    switch (CxTYPE(cx)) {
    case CXt_SUB:
	popsub2 = TRUE;
	cxstack_ix++; /* preserve cx entry on stack for use by POPSUB */
	break;
    case CXt_EVAL:
	if (!(PL_in_eval & EVAL_KEEPERR))
	    clear_errsv = TRUE;
	POPEVAL(cx);
	if (CxTRYBLOCK(cx))
	    break;
	lex_end();
	if (optype == OP_REQUIRE &&
	    (MARK == SP || (gimme == G_SCALAR && !SvTRUE(*SP))) )
	{
	    /* Unassume the success we assumed earlier. */
	    SV * const nsv = cx->blk_eval.old_namesv;
	    (void)hv_delete(GvHVn(PL_incgv), SvPVX_const(nsv), SvCUR(nsv), G_DISCARD);
	    DIE(aTHX_ "%"SVf" did not return a true value", (void*)nsv);
	}
	break;
    case CXt_FORMAT:
	POPFORMAT(cx);
	break;
    default:
	DIE(aTHX_ "panic: return");
    }

    TAINT_NOT;
    if (gimme == G_SCALAR) {
	if (MARK < SP) {
	    if (popsub2) {
		if (cx->blk_sub.cv && CvDEPTH(cx->blk_sub.cv) > 1) {
		    if (SvTEMP(TOPs)) {
			*++newsp = SvREFCNT_inc(*SP);
			FREETMPS;
			sv_2mortal(*newsp);
		    }
		    else {
			sv = SvREFCNT_inc(*SP);	/* FREETMPS could clobber it */
			FREETMPS;
			*++newsp = sv_mortalcopy(sv);
			SvREFCNT_dec(sv);
		    }
		}
		else
		    *++newsp = (SvTEMP(*SP)) ? *SP : sv_mortalcopy(*SP);
	    }
	    else
		*++newsp = sv_mortalcopy(*SP);
	}
	else
	    *++newsp = &PL_sv_undef;
    }
    else if (gimme == G_ARRAY) {
	while (++MARK <= SP) {
	    *++newsp = (popsub2 && SvTEMP(*MARK))
			? *MARK : sv_mortalcopy(*MARK);
	    TAINT_NOT;		/* Each item is independent */
	}
    }
    PL_stack_sp = newsp;

    LEAVE;
    /* Stack values are safe: */
    if (popsub2) {
	cxstack_ix--;
	POPSUB(cx,sv);	/* release CV and @_ ... */
    }
    else
	sv = NULL;
    PL_curpm = newpm;	/* ... and pop $1 et al */

    LEAVESUB(sv);
    if (clear_errsv) {
	CLEAR_ERRSV();
    }
    return pop_return();
}

PP(pp_last)
{
    dSP;
    I32 cxix;
    register PERL_CONTEXT *cx;
    I32 pop2 = 0;
    I32 gimme;
    I32 optype;
    OP *nextop;
    SV **newsp;
    PMOP *newpm;
    SV **mark;
    SV *sv = NULL;


    if (PL_op->op_flags & OPf_SPECIAL) {
	cxix = dopoptoloop(cxstack_ix);
	if (cxix < 0)
	    DIE(aTHX_ "Can't \"last\" outside a loop block");
    }
    else {
	cxix = dopoptolabel(cPVOP->op_pv);
	if (cxix < 0)
	    DIE(aTHX_ "Label not found for \"last %s\"", cPVOP->op_pv);
    }
    if (cxix < cxstack_ix)
	dounwind(cxix);

    POPBLOCK(cx,newpm);
    cxstack_ix++; /* temporarily protect top context */
    mark = newsp;
    switch (CxTYPE(cx)) {
    case CXt_LOOP:
	pop2 = CXt_LOOP;
	newsp = PL_stack_base + cx->blk_loop.resetsp;
	nextop = cx->blk_loop.last_op->op_next;
	break;
    case CXt_SUB:
	pop2 = CXt_SUB;
	nextop = pop_return();
	break;
    case CXt_EVAL:
	POPEVAL(cx);
	nextop = pop_return();
	break;
    case CXt_FORMAT:
	POPFORMAT(cx);
	nextop = pop_return();
	break;
    default:
	DIE(aTHX_ "panic: last");
    }

    TAINT_NOT;
    if (gimme == G_SCALAR) {
	if (MARK < SP)
	    *++newsp = ((pop2 == CXt_SUB) && SvTEMP(*SP))
			? *SP : sv_mortalcopy(*SP);
	else
	    *++newsp = &PL_sv_undef;
    }
    else if (gimme == G_ARRAY) {
	while (++MARK <= SP) {
	    *++newsp = ((pop2 == CXt_SUB) && SvTEMP(*MARK))
			? *MARK : sv_mortalcopy(*MARK);
	    TAINT_NOT;		/* Each item is independent */
	}
    }
    SP = newsp;
    PUTBACK;

    LEAVE;
    cxstack_ix--;
    /* Stack values are safe: */
    switch (pop2) {
    case CXt_LOOP:
	POPLOOP(cx);	/* release loop vars ... */
	LEAVE;
	break;
    case CXt_SUB:
	POPSUB(cx,sv);	/* release CV and @_ ... */
	break;
    }
    PL_curpm = newpm;	/* ... and pop $1 et al */

    LEAVESUB(sv);
    PERL_UNUSED_VAR(optype);
    PERL_UNUSED_VAR(gimme);
    return nextop;
}

PP(pp_next)
{
    I32 cxix;
    register PERL_CONTEXT *cx;
    I32 inner;

    if (PL_op->op_flags & OPf_SPECIAL) {
	cxix = dopoptoloop(cxstack_ix);
	if (cxix < 0)
	    DIE(aTHX_ "Can't \"next\" outside a loop block");
    }
    else {
	cxix = dopoptolabel(cPVOP->op_pv);
	if (cxix < 0)
	    DIE(aTHX_ "Label not found for \"next %s\"", cPVOP->op_pv);
    }
    if (cxix < cxstack_ix)
	dounwind(cxix);

    /* clear off anything above the scope we're re-entering, but
     * save the rest until after a possible continue block */
    inner = PL_scopestack_ix;
    TOPBLOCK(cx);
    if (PL_scopestack_ix < inner)
	leave_scope(PL_scopestack[PL_scopestack_ix]);
    PL_curcop = cx->blk_oldcop;
    return cx->blk_loop.next_op;
}

PP(pp_redo)
{
    I32 cxix;
    register PERL_CONTEXT *cx;
    I32 oldsave;
    OP* redo_op;

    if (PL_op->op_flags & OPf_SPECIAL) {
	cxix = dopoptoloop(cxstack_ix);
	if (cxix < 0)
	    DIE(aTHX_ "Can't \"redo\" outside a loop block");
    }
    else {
	cxix = dopoptolabel(cPVOP->op_pv);
	if (cxix < 0)
	    DIE(aTHX_ "Label not found for \"redo %s\"", cPVOP->op_pv);
    }
    if (cxix < cxstack_ix)
	dounwind(cxix);

    redo_op = cxstack[cxix].blk_loop.redo_op;
    if (redo_op->op_type == OP_ENTER) {
	/* pop one less context to avoid $x being freed in while (my $x..) */
	cxstack_ix++;
	assert(CxTYPE(&cxstack[cxstack_ix]) == CXt_BLOCK);
	redo_op = redo_op->op_next;
    }

    TOPBLOCK(cx);
    oldsave = PL_scopestack[PL_scopestack_ix - 1];
    LEAVE_SCOPE(oldsave);
    FREETMPS;
    PL_curcop = cx->blk_oldcop;
    return redo_op;
}

STATIC OP *
S_dofindlabel(pTHX_ OP *o, const char *label, OP **opstack, OP **oplimit)
{
    OP **ops = opstack;
    static const char too_deep[] = "Target of goto is too deeply nested";

    if (ops >= oplimit)
	Perl_croak(aTHX_ too_deep);
    if (o->op_type == OP_LEAVE ||
	o->op_type == OP_SCOPE ||
	o->op_type == OP_LEAVELOOP ||
	o->op_type == OP_LEAVESUB ||
	o->op_type == OP_LEAVETRY)
    {
	*ops++ = cUNOPo->op_first;
	if (ops >= oplimit)
	    Perl_croak(aTHX_ too_deep);
    }
    *ops = 0;
    if (o->op_flags & OPf_KIDS) {
	OP *kid;
	/* First try all the kids at this level, since that's likeliest. */
	for (kid = cUNOPo->op_first; kid; kid = kid->op_sibling) {
	    if ((kid->op_type == OP_NEXTSTATE || kid->op_type == OP_DBSTATE) &&
		    kCOP->cop_label && strEQ(kCOP->cop_label, label))
		return kid;
	}
	for (kid = cUNOPo->op_first; kid; kid = kid->op_sibling) {
	    if (kid == PL_lastgotoprobe)
		continue;
	    if (kid->op_type == OP_NEXTSTATE || kid->op_type == OP_DBSTATE) {
	        if (ops == opstack)
		    *ops++ = kid;
		else if (ops[-1]->op_type == OP_NEXTSTATE ||
		         ops[-1]->op_type == OP_DBSTATE)
		    ops[-1] = kid;
		else
		    *ops++ = kid;
	    }
	    if ((o = dofindlabel(kid, label, ops, oplimit)))
		return o;
	}
    }
    *ops = 0;
    return 0;
}

PP(pp_goto)
{
    dSP;
    OP *retop = NULL;
    I32 ix;
    register PERL_CONTEXT *cx;
#define GOTO_DEPTH 64
    OP *enterops[GOTO_DEPTH];
    const char *label = NULL;
    const bool do_dump = (PL_op->op_type == OP_DUMP);
    static const char must_have_label[] = "goto must have label";

    if (PL_op->op_flags & OPf_STACKED) {
	SV * const sv = POPs;

	/* This egregious kludge implements goto &subroutine */
	if (SvROK(sv) && SvTYPE(SvRV(sv)) == SVt_PVCV) {
	    I32 cxix;
	    register PERL_CONTEXT *cx;
	    CV* cv = (CV*)SvRV(sv);
	    SV** mark;
	    I32 items = 0;
	    I32 oldsave;
	    bool reified = 0;

	retry:
	    if (!CvROOT(cv) && !CvXSUB(cv)) {
		const GV * const gv = CvGV(cv);
		if (gv) {
		    GV *autogv;
		    SV *tmpstr;
		    /* autoloaded stub? */
		    if (cv != GvCV(gv) && (cv = GvCV(gv)))
			goto retry;
		    autogv = gv_autoload4(GvSTASH(gv), GvNAME(gv),
					  GvNAMELEN(gv), FALSE);
		    if (autogv && (cv = GvCV(autogv)))
			goto retry;
		    tmpstr = sv_newmortal();
		    gv_efullname3(tmpstr, (GV *) gv, NULL);
		    DIE(aTHX_ "Goto undefined subroutine &%"SVf"",(void*)tmpstr);
		}
		DIE(aTHX_ "Goto undefined subroutine");
	    }

	    /* First do some returnish stuff. */
	    SvREFCNT_inc_simple_void(cv); /* avoid premature free during unwind */
	    FREETMPS;
	    cxix = dopoptosub(cxstack_ix);
	    if (cxix < 0)
		DIE(aTHX_ "Can't goto subroutine outside a subroutine");
	    if (cxix < cxstack_ix)
		dounwind(cxix);
	    TOPBLOCK(cx);
	    SPAGAIN;
	    /* ban goto in eval: see <20050521150056.GC20213@iabyn.com> */
	    if (CxTYPE(cx) == CXt_EVAL) {
		if (CxREALEVAL(cx))
		    DIE(aTHX_ "Can't goto subroutine from an eval-string");
		else
		    DIE(aTHX_ "Can't goto subroutine from an eval-block");
	    }
	    else if (CxMULTICALL(cx))
		DIE(aTHX_ "Can't goto subroutine from a sort sub (or similar callback)");
	    if (CxTYPE(cx) == CXt_SUB && CxHASARGS(cx)) {
		/* put @_ back onto stack */
		AV* av = cx->blk_sub.argarray;

		items = AvFILLp(av) + 1;
		EXTEND(SP, items+1); /* @_ could have been extended. */
		Copy(AvARRAY(av), SP + 1, items, SV*);
#ifndef USE_5005THREADS
		SvREFCNT_dec(GvAV(PL_defgv));
		GvAV(PL_defgv) = cx->blk_sub.savearray;
#endif /* USE_5005THREADS */
		CLEAR_ARGARRAY(av);
		/* abandon @_ if it got reified */
		if (AvREAL(av)) {
		    reified = 1;
		    SvREFCNT_dec(av);
		    av = newAV();
		    av_extend(av, items-1);
		    AvFLAGS(av) = AVf_REIFY;
		    PAD_SVl(0) = (SV*)(cx->blk_sub.argarray = av);
		}
	    }
	    else if (CvISXSUB(cv)) {	/* put GvAV(defgv) back onto stack */
#ifdef USE_5005THREADS
		AV* const av = (AV*)PAD_SVl(0);
#else
		AV* const av = GvAV(PL_defgv);
#endif
		items = AvFILLp(av) + 1;
		EXTEND(SP, items+1); /* @_ could have been extended. */
		Copy(AvARRAY(av), SP + 1, items, SV*);
	    }
	    mark = SP;
	    SP += items;
	    if (CxTYPE(cx) == CXt_SUB &&
		!(CvDEPTH(cx->blk_sub.cv) = cx->blk_sub.olddepth))
		SvREFCNT_dec(cx->blk_sub.cv);
	    oldsave = PL_scopestack[PL_scopestack_ix - 1];
	    LEAVE_SCOPE(oldsave);

	    /* Now do some callish stuff. */
	    SAVETMPS;
	    SAVEFREESV(cv); /* later, undo the 'avoid premature free' hack */
	    if (CvISXSUB(cv)) {
		OP* retop = pop_return();
		if (reified) {
		    I32 index;
		    for (index=0; index<items; index++)
			sv_2mortal(SP[-index]);
		}
#ifdef PERL_XSUB_OLDSTYLE
		if (CvOLDSTYLE(cv)) {
		    I32 (*fp3)(int,int,int);
		    while (SP > mark) {
			SP[1] = SP[0];
			SP--;
		    }
		    fp3 = (I32(*)(int,int,int))CvXSUB(cv);
		    items = (*fp3)(CvXSUBANY(cv).any_i32,
		                   mark - PL_stack_base + 1,
				   items);
		    SP = PL_stack_base + items;
		}
		else
#endif /* PERL_XSUB_OLDSTYLE */
		{
		    SV **newsp;
		    I32 gimme;

		    /* XS subs don't have a CxSUB, so pop it */
		    POPBLOCK(cx, PL_curpm);
		    /* Push a mark for the start of arglist */
		    PUSHMARK(mark);
		    PUTBACK;
		    (void)(*CvXSUB(cv))(aTHX_ cv);


		    /* Put these at the bottom since the vars are set but not used */
		    PERL_UNUSED_VAR(newsp);
		    PERL_UNUSED_VAR(gimme);
		}
		LEAVE;
		return retop;
	    }
	    else {
		AV* const padlist = CvPADLIST(cv);
		if (CxTYPE(cx) == CXt_EVAL) {
		    PL_in_eval = CxOLD_IN_EVAL(cx);
		    PL_eval_root = cx->blk_eval.old_eval_root;
		    cx->cx_type = CXt_SUB;
		    cx->blk_sub.hasargs = 0;
		}
		cx->blk_sub.cv = cv;
		cx->blk_sub.olddepth = CvDEPTH(cv);

		CvDEPTH(cv)++;
		if (CvDEPTH(cv) < 2)
		    SvREFCNT_inc_simple_void_NN(cv);
		else {
		    if (CvDEPTH(cv) == PERL_SUB_DEPTH_WARN && ckWARN(WARN_RECURSION))
			sub_crush_depth(cv);
		    pad_push(padlist, CvDEPTH(cv), 1);
		}
#ifdef USE_5005THREADS
		if (!cx->blk_sub.hasargs) {
		    AV* av = (AV*)PAD_SVl(0);
		
		    items = AvFILLp(av) + 1;
		    if (items) {
			/* Mark is at the end of the stack. */
			EXTEND(SP, items);
			Copy(AvARRAY(av), SP + 1, items, SV*);
			SP += items;
			PUTBACK ;		
		    }
		}
#endif /* USE_5005THREADS */
		SAVECOMPPAD();
		PAD_SET_CUR_NOSAVE(padlist, CvDEPTH(cv));
#ifndef USE_5005THREADS
		if (CxHASARGS(cx))
#endif /* USE_5005THREADS */
		{
		    AV* const av = (AV*)PAD_SVl(0);

#ifndef USE_5005THREADS
		    cx->blk_sub.savearray = GvAV(PL_defgv);
		    GvAV(PL_defgv) = (AV*)SvREFCNT_inc_simple(av);
#endif /* USE_5005THREADS */
		    CX_CURPAD_SAVE(cx->blk_sub);
		    cx->blk_sub.argarray = av;

		    if (items >= AvMAX(av) + 1) {
			SV **ary = AvALLOC(av);
			if (AvARRAY(av) != ary) {
			    AvMAX(av) += AvARRAY(av) - AvALLOC(av);
			    SvPV_set(av, (char*)ary);
			}
			if (items >= AvMAX(av) + 1) {
			    AvMAX(av) = items - 1;
			    Renew(ary,items+1,SV*);
			    AvALLOC(av) = ary;
			    SvPV_set(av, (char*)ary);
			}
		    }
		    ++mark;
		    Copy(mark,AvARRAY(av),items,SV*);
		    AvFILLp(av) = items - 1;
		    assert(!AvREAL(av));
		    if (reified) {
			/* transfer 'ownership' of refcnts to new @_ */
			AvREAL_on(av);
			AvREIFY_off(av);
		    }
		    while (items--) {
			if (*mark)
			    SvTEMP_off(*mark);
			mark++;
		    }
		}
		if (PERLDB_SUB) {	/* Checking curstash breaks DProf. */
		    Perl_get_db_sub(aTHX_ NULL, cv);
		    if (PERLDB_GOTO) {
			CV * const gotocv = get_cv("DB::goto", FALSE);
			if (gotocv) {
			    PUSHMARK( PL_stack_sp );
			    call_sv((SV*)gotocv, G_SCALAR | G_NODEBUG);
			    PL_stack_sp--;
			}
		    }
		}
		RETURNOP(CvSTART(cv));
	    }
	}
	else {
	    label = SvPV_nolen_const(sv);
	    if (!(do_dump || *label))
		DIE(aTHX_ must_have_label);
	}
    }
    else if (PL_op->op_flags & OPf_SPECIAL) {
	if (! do_dump)
	    DIE(aTHX_ must_have_label);
    }
    else
	label = cPVOP->op_pv;

    if (label && *label) {
	OP *gotoprobe = NULL;
	bool leaving_eval = FALSE;
	bool in_block = FALSE;
	PERL_CONTEXT *last_eval_cx = NULL;

	/* find label */

	PL_lastgotoprobe = NULL;
	*enterops = 0;
	for (ix = cxstack_ix; ix >= 0; ix--) {
	    cx = &cxstack[ix];
	    switch (CxTYPE(cx)) {
	    case CXt_EVAL:
		leaving_eval = TRUE;
                if (!CxTRYBLOCK(cx)) {
		    gotoprobe = (last_eval_cx ?
				last_eval_cx->blk_eval.old_eval_root :
				PL_eval_root);
		    last_eval_cx = cx;
		    break;
                }
                /* else fall through */
	    case CXt_LOOP:
		gotoprobe = cx->blk_oldcop->op_sibling;
		break;
	    case CXt_SUBST:
		continue;
	    case CXt_BLOCK:
		if (ix) {
		    gotoprobe = cx->blk_oldcop->op_sibling;
		    in_block = TRUE;
		} else
		    gotoprobe = PL_main_root;
		break;
	    case CXt_SUB:
		if (CvDEPTH(cx->blk_sub.cv) && !CxMULTICALL(cx)) {
		    gotoprobe = CvROOT(cx->blk_sub.cv);
		    break;
		}
		/* FALL THROUGH */
	    case CXt_FORMAT:
	    case CXt_NULL:
		DIE(aTHX_ "Can't \"goto\" out of a pseudo block");
	    default:
		if (ix)
		    DIE(aTHX_ "panic: goto");
		gotoprobe = PL_main_root;
		break;
	    }
	    if (gotoprobe) {
		retop = dofindlabel(gotoprobe, label,
				    enterops, enterops + GOTO_DEPTH);
		if (retop)
		    break;
	    }
	    PL_lastgotoprobe = gotoprobe;
	}
	if (!retop)
	    DIE(aTHX_ "Can't find label %s", label);

	/* if we're leaving an eval, check before we pop any frames
           that we're not going to punt, otherwise the error
	   won't be caught */

	if (leaving_eval && *enterops && enterops[1]) {
	    I32 i;
            for (i = 1; enterops[i]; i++)
                if (enterops[i]->op_type == OP_ENTERITER)
                    DIE(aTHX_ "Can't \"goto\" into the middle of a foreach loop");
	}

	/* pop unwanted frames */

	if (ix < cxstack_ix) {
	    I32 oldsave;

	    if (ix < 0)
		ix = 0;
	    dounwind(ix);
	    TOPBLOCK(cx);
	    oldsave = PL_scopestack[PL_scopestack_ix];
	    LEAVE_SCOPE(oldsave);
	}

	/* push wanted frames */

	if (*enterops && enterops[1]) {
	    OP * const oldop = PL_op;
	    ix = enterops[1]->op_type == OP_ENTER && in_block ? 2 : 1;
	    for (; enterops[ix]; ix++) {
		PL_op = enterops[ix];
		/* Eventually we may want to stack the needed arguments
		 * for each op.  For now, we punt on the hard ones. */
		if (PL_op->op_type == OP_ENTERITER)
		    DIE(aTHX_ "Can't \"goto\" into the middle of a foreach loop");
		CALL_FPTR(PL_op->op_ppaddr)(aTHX);
	    }
	    PL_op = oldop;
	}
    }

    if (do_dump) {
#ifdef VMS
	if (!retop) retop = PL_main_start;
#endif
	PL_restartop = retop;
	PL_do_undump = TRUE;

	my_unexec();

	PL_restartop = 0;		/* hmm, must be GNU unexec().. */
	PL_do_undump = FALSE;
    }

    RETURNOP(retop);
}

PP(pp_exit)
{
    dSP;
    I32 anum;

    if (MAXARG < 1)
	anum = 0;
    else {
	anum = SvIVx(POPs);
#ifdef VMS
        if (anum == 1 && (PL_op->op_private & OPpEXIT_VMSISH))
	    anum = 0;
        VMSISH_HUSHED  = VMSISH_HUSHED || (PL_op->op_private & OPpHUSH_VMSISH);
#endif
    }
    PL_exit_flags |= PERL_EXIT_EXPECTED;
    my_exit(anum);
    PUSHs(&PL_sv_undef);
    RETURN;
}

/* Eval. */

STATIC void
S_save_lines(pTHX_ AV *array, SV *sv)
{
    const char *s = SvPVX_const(sv);
    const char * const send = SvPVX_const(sv) + SvCUR(sv);
    I32 line = 1;

    while (s && s < send) {
	const char *t;
	SV * const tmpstr = newSV_type(SVt_PVMG);

	t = strchr(s, '\n');
	if (t)
	    t++;
	else
	    t = send;

	sv_setpvn(tmpstr, s, t - s);
	av_store(array, line++, tmpstr);
	s = t;
    }
}

#ifdef PERL_FLEXIBLE_EXCEPTIONS
STATIC void *
S_vdocatch_body(pTHX_ va_list args)
{
    docatch_body();
    return NULL;
}
#endif

STATIC void
S_docatch_body(pTHX)
{
    CALLRUNOPS(aTHX);
    return;
}

STATIC OP *
S_docatch(pTHX_ OP *o)
{
    int ret;
    OP * const oldop = PL_op;
    dJMPENV;

#ifdef DEBUGGING
    assert(CATCH_GET == TRUE);
#endif
    PL_op = o;

#ifdef PERL_FLEXIBLE_EXCEPTIONS
 redo_body:
    CALLPROTECT(aTHX_ pcur_env, &ret, MEMBER_TO_FPTR(S_vdocatch_body));
#else
    JMPENV_PUSH(ret);
#endif
    switch (ret) {
    case 0:
	cxstack[cxstack_ix].blk_eval.cur_top_env = PL_top_env;
#ifndef PERL_FLEXIBLE_EXCEPTIONS
 redo_body:
	docatch_body();
#endif
	break;
    case 3:
	/* die caught by an inner eval - continue inner loop */

	/* NB XXX we rely on the old popped CxEVAL still being at the top
	 * of the stack; the way die_where() currently works, this
	 * assumption is valid. In theory The cur_top_env value should be
	 * returned in another global, the way retop (aka PL_restartop)
	 * is. */

	if (PL_restartop
	    && cxstack[cxstack_ix+1].blk_eval.cur_top_env == PL_top_env)
	{
	    PL_op = PL_restartop;
	    PL_restartop = 0;
	    goto redo_body;
	}
	/* FALL THROUGH */
    default:
	JMPENV_POP;
	PL_op = oldop;
	JMPENV_JUMP(ret);
	/* NOTREACHED */
    }
    JMPENV_POP;
    PL_op = oldop;
    return NULL;
}

OP *
Perl_sv_compile_2op(pTHX_ SV *sv, OP** startop, char *code, PAD** padp)
/* sv Text to convert to OP tree. */
/* startop op_free() this to undo. */
/* code Short string id of the caller. */
{
    /* FIXME - how much of this code is common with pp_entereval?  */
    dSP;				/* Make POPBLOCK work. */
    PERL_CONTEXT *cx;
    SV **newsp;
    I32 gimme = G_VOID;
    I32 optype;
    OP dummy;
    char tbuf[TYPE_DIGITS(long) + 12 + 10];
    char *tmpbuf = tbuf;
    char *safestr;
    int runtime;
    CV* runcv = Nullcv;	/* initialise to avoid compiler warnings */
    STRLEN len;

    ENTER;
    lex_start(sv);
    SAVETMPS;
    /* switch to eval mode */

    if (IN_PERL_COMPILETIME) {
	SAVECOPSTASH_FREE(&PL_compiling);
	CopSTASH_set(&PL_compiling, PL_curstash);
    }
    if (PERLDB_NAMEEVAL && CopLINE(PL_curcop)) {
	SV * const sv = sv_newmortal();
	Perl_sv_setpvf(aTHX_ sv, "_<(%.10seval %lu)[%s:%"IVdf"]",
		       code, (unsigned long)++PL_evalseq,
		       CopFILE(PL_curcop), (IV)CopLINE(PL_curcop));
	tmpbuf = SvPVX(sv);
	len = SvCUR(sv);
    }
    else
	len = my_snprintf(tmpbuf, sizeof(tbuf), "_<(%.10s_eval %lu)", code,
			  (unsigned long)++PL_evalseq);
    SAVECOPFILE_FREE(&PL_compiling);
    CopFILE_set(&PL_compiling, tmpbuf+2);
    SAVECOPLINE(&PL_compiling);
    CopLINE_set(&PL_compiling, 1);
    /* XXX For C<eval "...">s within BEGIN {} blocks, this ends up
       deleting the eval's FILEGV from the stash before gv_check() runs
       (i.e. before run-time proper). To work around the coredump that
       ensues, we always turn GvMULTI_on for any globals that were
       introduced within evals. See force_ident(). GSAR 96-10-12 */
    safestr = savepvn(tmpbuf, len);
    SAVEDELETE(PL_defstash, safestr, len);
    SAVEHINTS();
#ifdef OP_IN_REGISTER
    PL_opsave = op;
#else
    SAVEVPTR(PL_op);
#endif

    /* we get here either during compilation, or via pp_regcomp at runtime */
    runtime = IN_PERL_RUNTIME;
    if (runtime)
	runcv = find_runcv(NULL);

    PL_op = &dummy;
    PL_op->op_type = OP_ENTEREVAL;
    PL_op->op_flags = 0;			/* Avoid uninit warning. */
    PUSHBLOCK(cx, CXt_EVAL|(IN_PERL_COMPILETIME ? 0 : CXp_REAL), SP);
    PUSHEVAL(cx, 0, NULL);

    if (runtime)
	(void) doeval(G_SCALAR, startop, runcv, PL_curcop->cop_seq);
    else
	(void) doeval(G_SCALAR, startop, PL_compcv, PL_cop_seqmax);
    POPBLOCK(cx,PL_curpm);
    POPEVAL(cx);

    (*startop)->op_type = OP_NULL;
    (*startop)->op_ppaddr = PL_ppaddr[OP_NULL];
    lex_end();
    /* XXX DAPM do this properly one year */
    *padp = (AV*)SvREFCNT_inc_simple(PL_comppad);
    LEAVE;
    if (IN_PERL_COMPILETIME)
	CopHINTS_set(&PL_compiling, PL_hints);
#ifdef OP_IN_REGISTER
    op = PL_opsave;
#endif
    PERL_UNUSED_VAR(newsp);
    PERL_UNUSED_VAR(optype);

    return PL_eval_start;
}


/*
=for apidoc find_runcv

Locate the CV corresponding to the currently executing sub or eval.
If db_seqp is non_null, skip CVs that are in the DB package and populate
*db_seqp with the cop sequence number at the point that the DB:: code was
entered. (allows debuggers to eval in the scope of the breakpoint rather
than in the scope of the debugger itself).

=cut
*/

CV*
Perl_find_runcv(pTHX_ U32 *db_seqp)
{
    PERL_SI	 *si;

    if (db_seqp)
	*db_seqp = PL_curcop->cop_seq;
    for (si = PL_curstackinfo; si; si = si->si_prev) {
        I32 ix;
	for (ix = si->si_cxix; ix >= 0; ix--) {
	    const PERL_CONTEXT *cx = &(si->si_cxstack[ix]);
	    if (CxTYPE(cx) == CXt_SUB || CxTYPE(cx) == CXt_FORMAT) {
		CV * const cv = cx->blk_sub.cv;
		/* skip DB:: code */
		if (db_seqp && PL_debstash && CvSTASH(cv) == PL_debstash) {
		    *db_seqp = cx->blk_oldcop->cop_seq;
		    continue;
		}
		return cv;
	    }
	    else if (CxTYPE(cx) == CXt_EVAL && !CxTRYBLOCK(cx))
		return PL_compcv;
	}
    }
    return PL_main_cv;
}


/* Compile a require/do, an eval '', or a /(?{...})/.
 * In the last case, startop is non-null, and contains the address of
 * a pointer that should be set to the just-compiled code.
 * outside is the lexically enclosing CV (if any) that invoked us.
 * Returns a bool indicating whether the compile was successful; if so,
 * PL_eval_start contains the first op of the compiled ocde; otherwise,
 * pushes undef (also croaks if startop != NULL).
 */

/* With USE_5005THREADS, eval_owner must be held on entry to doeval */
STATIC bool
S_doeval(pTHX_ int gimme, OP** startop, CV* outside, U32 seq)
{
    dSP;
    OP * const saveop = PL_op;

    PL_in_eval = ((saveop && saveop->op_type == OP_REQUIRE)
		  ? (EVAL_INREQUIRE | (PL_in_eval & EVAL_INEVAL))
		  : EVAL_INEVAL);

    PUSHMARK(SP);

    SAVESPTR(PL_compcv);
    PL_compcv = (CV*)newSV_type(SVt_PVCV);
    CvEVAL_on(PL_compcv);
    assert(CxTYPE(&cxstack[cxstack_ix]) == CXt_EVAL);
    cxstack[cxstack_ix].blk_eval.cv = PL_compcv;

#ifdef USE_5005THREADS
    CvOWNER(PL_compcv) = 0;
    New(666, CvMUTEXP(PL_compcv), 1, perl_mutex);
    MUTEX_INIT(CvMUTEXP(PL_compcv));
#endif /* USE_5005THREADS */

    CvOUTSIDE_SEQ(PL_compcv) = seq;
    CvOUTSIDE(PL_compcv) = (CV*)SvREFCNT_inc_simple(outside);

    /* set up a scratch pad */

    CvPADLIST(PL_compcv) = pad_new(padnew_SAVE);
    PL_op = NULL; /* avoid PL_op and PL_curpad referring to different CVs */


    SAVEMORTALIZESV(PL_compcv);	/* must remain until end of current statement */

    /* make sure we compile in the right package */

    if (CopSTASH_ne(PL_curcop, PL_curstash)) {
	SAVESPTR(PL_curstash);
	PL_curstash = CopSTASH(PL_curcop);
    }
    SAVESPTR(PL_beginav);
    PL_beginav = newAV();
    SAVEFREESV(PL_beginav);
    SAVEI32(PL_error_count);

    /* try to compile it */

    PL_eval_root = NULL;
    PL_error_count = 0;
    PL_curcop = &PL_compiling;
    CopARYBASE_set(PL_curcop, 0);
    if (saveop && (saveop->op_type != OP_REQUIRE) && (saveop->op_flags & OPf_SPECIAL))
	PL_in_eval |= EVAL_KEEPERR;
    else
	CLEAR_ERRSV();
    if (yyparse() || PL_error_count || !PL_eval_root) {
	SV **newsp;			/* Used by POPBLOCK. */
	PERL_CONTEXT *cx;
	I32 optype = 0;			/* Might be reset by POPEVAL. */
	const char *msg;

	PL_op = saveop;
	if (PL_eval_root) {
	    op_free(PL_eval_root);
	    PL_eval_root = NULL;
	}
	SP = PL_stack_base + POPMARK;		/* pop original mark */
	if (!startop) {
	    POPBLOCK(cx,PL_curpm);
	    POPEVAL(cx);
	    pop_return();
	}
	lex_end();
	LEAVE;

	msg = SvPVx_nolen_const(ERRSV);
	if (optype == OP_REQUIRE) {
            const char* const msg = SvPVx_nolen_const(ERRSV);
	    Perl_croak(aTHX_ "%sCompilation failed in require",
		       *msg ? msg : "Unknown error\n");
	}
	else if (startop) {
	    POPBLOCK(cx,PL_curpm);
	    POPEVAL(cx);
	    Perl_croak(aTHX_ "%sCompilation failed in regexp",
		       (*msg ? msg : "Unknown error\n"));
	}
	else {
	    if (!*msg) {
	        sv_setpvs(ERRSV, "Compilation error");
	    }
	}
#ifdef USE_5005THREADS
	MUTEX_LOCK(&PL_eval_mutex);
	PL_eval_owner = 0;
	COND_SIGNAL(&PL_eval_cond);
	MUTEX_UNLOCK(&PL_eval_mutex);
#endif /* USE_5005THREADS */
	PERL_UNUSED_VAR(newsp);
	PUSHs(&PL_sv_undef);
	PUTBACK;
	return FALSE;
    }
    CopLINE_set(&PL_compiling, 0);
    if (startop) {
	*startop = PL_eval_root;
    } else
	SAVEFREEOP(PL_eval_root);

    /* Set the context for this new optree.
     * If the last op is an OP_REQUIRE, force scalar context.
     * Otherwise, propagate the context from the eval(). */
    if (PL_eval_root->op_type == OP_LEAVEEVAL
	    && cUNOPx(PL_eval_root)->op_first->op_type == OP_LINESEQ
	    && cLISTOPx(cUNOPx(PL_eval_root)->op_first)->op_last->op_type
	    == OP_REQUIRE)
	scalar(PL_eval_root);
    else if (gimme & G_VOID)
	scalarvoid(PL_eval_root);
    else if (gimme & G_ARRAY)
	list(PL_eval_root);
    else
	scalar(PL_eval_root);

    DEBUG_x(dump_eval());

    /* Register with debugger: */
    if (PERLDB_INTER && saveop && saveop->op_type == OP_REQUIRE) {
	CV * const cv = get_cv("DB::postponed", FALSE);
	if (cv) {
	    dSP;
	    PUSHMARK(SP);
	    XPUSHs((SV*)CopFILEGV(&PL_compiling));
	    PUTBACK;
	    call_sv((SV*)cv, G_DISCARD);
	}
    }

    /* compiled okay, so do it */

    CvDEPTH(PL_compcv) = 1;
    SP = PL_stack_base + POPMARK;		/* pop original mark */
    PL_op = saveop;			/* The caller may need it. */
    PL_lex_state = LEX_NOTPARSING;	/* $^S needs this. */
#ifdef USE_5005THREADS
    MUTEX_LOCK(&PL_eval_mutex);
    PL_eval_owner = 0;
    COND_SIGNAL(&PL_eval_cond);
    MUTEX_UNLOCK(&PL_eval_mutex);
#endif /* USE_5005THREADS */

    PUTBACK;
    return TRUE;
}

STATIC PerlIO *
S_check_type_and_open(pTHX_ const char *name)
{
    Stat_t st;
    const int st_rc = PerlLIO_stat(name, &st);

    if (st_rc < 0 || S_ISDIR(st.st_mode) || S_ISBLK(st.st_mode)) {
	return NULL;
    }

    return PerlIO_open(name, PERL_SCRIPT_MODE);
}

#ifndef PERL_DISABLE_PMC
STATIC PerlIO *
S_doopen_pm(pTHX_ const char *name, const STRLEN namelen)
{
    PerlIO *fp;

    if (namelen > 3 && memEQs(name + namelen - 3, 3, ".pm")) {
	SV *const pmcsv = newSV(namelen + 2);
	char *const pmc = SvPVX(pmcsv);
	Stat_t pmcstat;

	memcpy(pmc, name, namelen);
	pmc[namelen] = 'c';
	pmc[namelen + 1] = '\0';

	if (PerlLIO_stat(pmc, &pmcstat) < 0) {
	    fp = check_type_and_open(name);
	}
	else {
	    Stat_t pmstat;
	    if (PerlLIO_stat(name, &pmstat) < 0 ||
	        pmstat.st_mtime < pmcstat.st_mtime)
	    {
		fp = check_type_and_open(pmc);
	    }
	    else {
		fp = check_type_and_open(name);
	    }
	}
	SvREFCNT_dec(pmcsv);
    }
    else {
	fp = check_type_and_open(name);
    }
    return fp;
}
#else
#  define doopen_pm(name, namelen) check_type_and_open(name)
#endif /* !PERL_DISABLE_PMC */

PP(pp_require)
{
    dSP;
    register PERL_CONTEXT *cx;
    SV *sv;
    const char *name;
    STRLEN len;
    const char *tryname = NULL;
    SV *namesv = NULL;
    const I32 gimme = GIMME_V;
    int filter_has_file = 0;
    PerlIO *tryrsfp = NULL;
    SV *filter_cache = NULL;
    SV *filter_state = NULL;
    SV *filter_sub = NULL;
    SV *hook_sv = NULL;
    SV *encoding;
    OP *op;

    sv = POPs;
    if (SvNIOKp(sv) && PL_op->op_type != OP_DOFILE) {
	if (SvPOK(sv) && SvNOK(sv) && SvNV(sv)) {		/* require v5.6.1 */
	    UV rev = 0, ver = 0, sver = 0;
	    STRLEN len;
	    U8 *s = (U8*)SvPVX(sv);
	    U8 *end = (U8*)SvPVX(sv) + SvCUR(sv);
	    if (s < end) {
		rev = utf8n_to_uvchr(s, end - s, &len, 0);
		s += len;
		if (s < end) {
		    ver = utf8n_to_uvchr(s, end - s, &len, 0);
		    s += len;
		    if (s < end)
			sver = utf8n_to_uvchr(s, end - s, &len, 0);
		}
	    }
	    if (cUNOP->op_first->op_type == OP_CONST
		&& cUNOP->op_first->op_private & OPpCONST_NOVER) {
		if (PERL_REVISION > rev
		    || (PERL_REVISION == rev
			&& (PERL_VERSION > ver
			    || (PERL_VERSION == ver
				&& PERL_SUBVERSION >= sver))))
		    {
			DIE(aTHX_ "Perls since v%"UVuf".%"UVuf".%"UVuf
			    " too modern--this is v%d.%d.%d, stopped",
			    rev, ver, sver,
			    PERL_REVISION, PERL_VERSION, PERL_SUBVERSION);
		    }

	    } else {
		if (PERL_REVISION < rev
		    || (PERL_REVISION == rev
			&& (PERL_VERSION < ver
			    || (PERL_VERSION == ver
				&& PERL_SUBVERSION < sver))))
		    {
			DIE(aTHX_ "Perl v%"UVuf".%"UVuf".%"UVuf
			    " required--this is only v%d.%d.%d, stopped",
			    rev, ver, sver,
			    PERL_REVISION, PERL_VERSION, PERL_SUBVERSION);
		    }
	    }
	    RETPUSHYES;
	}
	else if (!SvPOKp(sv)) {			/* require 5.005_03 */
	    const NV nrev = SvNV(sv);
	    if (cUNOP->op_first->op_type == OP_CONST
		&& cUNOP->op_first->op_private & OPpCONST_NOVER) {
		if ((NV)PERL_REVISION + ((NV)PERL_VERSION/(NV)1000)
		    + ((NV)PERL_SUBVERSION/(NV)1000000)
		    + 0.00000099 > nrev) {
		    UV rev = (UV)nrev;
		    NV nver = (nrev - rev) * 1000;
		    UV ver = (UV)(nver + 0.0009);
		    NV nsver = (nver - ver) * 1000;
		    UV sver = (UV)(nsver + 0.0009);

		    DIE(aTHX_ "Perls since v%"UVuf".%"UVuf".%"UVuf
			" too modern--this is v%d.%d.%d, stopped",
			rev, ver, sver, PERL_REVISION, PERL_VERSION,
			PERL_SUBVERSION);
		}
	    } else if ((NV)PERL_REVISION + ((NV)PERL_VERSION/(NV)1000)
		       + ((NV)PERL_SUBVERSION/(NV)1000000)
		       + 0.00000099 < nrev) {
		UV rev = (UV)nrev;
		NV nver = (nrev - rev) * 1000;
		UV ver = (UV)(nver + 0.0009);
		NV nsver = (nver - ver) * 1000;
		UV sver = (UV)(nsver + 0.0009);

		/* help out with the "use 5.6" confusion */
		if (sver == 0 && (rev > 5 || (rev == 5 && ver >= 100))) {
		    DIE(aTHX_ "Perl v%"UVuf".%"UVuf".%"UVuf" required"
			" (did you mean v%"UVuf".%03"UVuf"?)--"
			"this is only v%d.%d.%d, stopped",
			rev, ver, sver, rev, ver/100,
			PERL_REVISION, PERL_VERSION, PERL_SUBVERSION);
		}
		else {
		    DIE(aTHX_ "Perl v%"UVuf".%"UVuf".%"UVuf" required--"
			"this is only v%d.%d.%d, stopped",
			rev, ver, sver, PERL_REVISION, PERL_VERSION,
			PERL_SUBVERSION);
		}
	    }
	    RETPUSHYES;
	}
    }
    name = SvPV_const(sv, len);
    if (!(name && len > 0 && *name))
	DIE(aTHX_ "Null filename used");
    TAINT_PROPER("require");
    if (PL_op->op_type == OP_REQUIRE) {
	SV * const * const svp = hv_fetch(GvHVn(PL_incgv), name, len, 0);
	if (svp && *svp != &PL_sv_undef)
	    RETPUSHYES;
    }

    /* prepare to compile file */

    if (path_is_absolute(name)) {
	tryname = name;
	tryrsfp = doopen_pm(name, len);
    }
#ifdef MACOS_TRADITIONAL
    if (!tryrsfp) {
	char newname[256];

	MacPerl_CanonDir(name, newname, 1);
	if (path_is_absolute(newname)) {
	    tryname = newname;
	    tryrsfp = doopen_pm(newname, strlen(newname));
	}
    }
#endif
    if (!tryrsfp) {
	AV * const ar = GvAVn(PL_incgv);
	I32 i;
#ifdef VMS
	char *unixname;
	if ((unixname = tounixspec(name, NULL)) != NULL)
#endif
	{
	    namesv = newSV_type(SVt_PV);
	    for (i = 0; i <= AvFILL(ar); i++) {
		SV * const dirsv = *av_fetch(ar, i, TRUE);

		if (SvTIED_mg((SV*)ar, PERL_MAGIC_tied))
		    mg_get(dirsv);
		if (SvROK(dirsv)) {
		    int count;
		    SV **svp;
		    SV *loader = dirsv;

		    if (SvTYPE(SvRV(loader)) == SVt_PVAV
			&& !sv_isobject(loader))
		    {
			loader = *av_fetch((AV *)SvRV(loader), 0, TRUE);
		    }

		    Perl_sv_setpvf(aTHX_ namesv, "/loader/0x%"UVxf"/%s",
				   PTR2UV(SvRV(dirsv)), name);
		    tryname = SvPVX_const(namesv);
		    tryrsfp = NULL;

		    ENTER;
		    SAVETMPS;
		    EXTEND(SP, 2);

		    PUSHMARK(SP);
		    PUSHs(dirsv);
		    PUSHs(sv);
		    PUTBACK;
		    if (sv_isobject(loader))
			count = call_method("INC", G_ARRAY);
		    else
			count = call_sv(loader, G_ARRAY);
		    SPAGAIN;

		    /* Adjust file name if the hook has set an %INC entry */
		    svp = hv_fetch(GvHVn(PL_incgv), name, len, 0);
		    if (svp)
			tryname = SvPVX_const(*svp);

		    if (count > 0) {
			int i = 0;
			SV *arg;

			SP -= count - 1;
			arg = SP[i++];

			if (SvROK(arg) && (SvTYPE(SvRV(arg)) <= SVt_PVLV)
			    && !isGV_with_GP(SvRV(arg))) {
			    filter_cache = SvRV(arg);
			    SvREFCNT_inc_simple_void_NN(filter_cache);

			    if (i < count) {
				arg = SP[i++];
			    }
			}

			if (SvROK(arg) && isGV_with_GP(SvRV(arg))) {
			    arg = SvRV(arg);
			}

			if (isGV_with_GP(arg)) {
			    IO * const io = GvIO((GV *)arg);

			    ++filter_has_file;

			    if (io) {
				tryrsfp = IoIFP(io);
				if (IoOFP(io) && IoOFP(io) != IoIFP(io)) {
				    PerlIO_close(IoOFP(io));
				}
				IoIFP(io) = NULL;
				IoOFP(io) = NULL;
			    }

			    if (i < count) {
				arg = SP[i++];
			    }
			}

			if (SvROK(arg) && SvTYPE(SvRV(arg)) == SVt_PVCV) {
			    filter_sub = arg;
			    SvREFCNT_inc_simple_void_NN(filter_sub);

			    if (i < count) {
				filter_state = SP[i];
				SvREFCNT_inc_simple_void(filter_state);
			    }
			}

			if (!tryrsfp && (filter_cache || filter_sub)) {
			    tryrsfp = PerlIO_open(BIT_BUCKET,
						  PERL_SCRIPT_MODE);
			}
			SP--;
		    }

		    PUTBACK;
		    FREETMPS;
		    LEAVE;

		    if (tryrsfp) {
			hook_sv = dirsv;
			break;
		    }

		    filter_has_file = 0;
		    if (filter_cache) {
			SvREFCNT_dec(filter_cache);
			filter_cache = NULL;
		    }
		    if (filter_state) {
			SvREFCNT_dec(filter_state);
			filter_state = NULL;
		    }
		    if (filter_sub) {
			SvREFCNT_dec(filter_sub);
			filter_sub = NULL;
		    }
		}
		else {
		  if (!path_is_absolute(name)
#ifdef MACOS_TRADITIONAL
			/* We consider paths of the form :a:b ambiguous and interpret them first
			   as global then as local
			*/
			|| (*name == ':' && name[1] != ':' && strchr(name+2, ':'))
#endif
		  ) {
		    const char *dir;
		    STRLEN dirlen;

		    if (SvOK(dirsv)) {
			dir = SvPV_const(dirsv, dirlen);
		    } else {
			dir = "";
			dirlen = 0;
		    }

#ifdef MACOS_TRADITIONAL
		    char buf1[256];
		    char buf2[256];

		    MacPerl_CanonDir(name, buf2, 1);
		    Perl_sv_setpvf(aTHX_ namesv, "%s%s", MacPerl_CanonDir(dir, buf1, 0), buf2+(buf2[0] == ':'));
#else
#  ifdef VMS
		    char *unixdir;
		    if ((unixdir = tounixpath(dir, NULL)) == NULL)
			continue;
		    sv_setpv(namesv, unixdir);
		    sv_catpv(namesv, unixname);
#  else
#    ifdef SYMBIAN
		    if (PL_origfilename[0] &&
			PL_origfilename[1] == ':' &&
			!(dir[0] && dir[1] == ':'))
		        Perl_sv_setpvf(aTHX_ namesv,
				       "%c:%s\\%s",
				       PL_origfilename[0],
				       dir, name);
		    else
		        Perl_sv_setpvf(aTHX_ namesv,
				       "%s\\%s",
				       dir, name);
#    else
		    /* The equivalent of		    
		       Perl_sv_setpvf(aTHX_ namesv, "%s/%s", dir, name);
		       but without the need to parse the format string, or
		       call strlen on either pointer, and with the correct
		       allocation up front.  */
		    {
			char *tmp = SvGROW(namesv, dirlen + len + 2);

			memcpy(tmp, dir, dirlen);
			tmp +=dirlen;
			*tmp++ = '/';
			/* name came from an SV, so it will have a '\0' at the
			   end that we can copy as part of this memcpy().  */
			memcpy(tmp, name, len + 1);

			SvCUR_set(namesv, dirlen + len + 1);

			/* Don't even actually have to turn SvPOK_on() as we
			   access it directly with SvPVX() below.  */
		    }
#    endif
#  endif
#endif
		    TAINT_PROPER("require");
		    tryname = SvPVX_const(namesv);
		    tryrsfp = doopen_pm(tryname, SvCUR(namesv));
		    if (tryrsfp) {
			if (tryname[0] == '.' && tryname[1] == '/')
			    tryname += 2;
			break;
		    }
		    else if (errno == EMFILE)
			/* no point in trying other paths if out of handles */
			break;
		  }
		}
	    }
	}
    }
    SAVECOPFILE_FREE(&PL_compiling);
    CopFILE_set(&PL_compiling, tryrsfp ? tryname : name);
    SvREFCNT_dec(namesv);
    if (!tryrsfp) {
	if (PL_op->op_type == OP_REQUIRE) {
	    const char *msgstr = name;
	    if(errno == EMFILE) {
		SV * const msg
		    = sv_2mortal(Perl_newSVpvf(aTHX_ "%s:   %s", msgstr,
					       Strerror(errno)));
		msgstr = SvPV_nolen_const(msg);
	    } else {
	        if (namesv) {			/* did we lookup @INC? */
		    AV * const ar = GvAVn(PL_incgv);
		    I32 i;
		    SV * const msg = sv_2mortal(Perl_newSVpvf(aTHX_ 
			"%s in @INC%s%s (@INC contains:",
			msgstr,
			(instr(msgstr, ".h ")
			 ? " (change .h to .ph maybe?)" : ""),
			(instr(msgstr, ".ph ")
			 ? " (did you run h2ph?)" : "")
							      ));
		    
		    for (i = 0; i <= AvFILL(ar); i++) {
			sv_catpvs(msg, " ");
			sv_catsv(msg, *av_fetch(ar, i, TRUE));
		    }
		    sv_catpvs(msg, ")");
		    msgstr = SvPV_nolen_const(msg);
		}    
	    }
	    DIE(aTHX_ "Can't locate %s", msgstr);
	}

	RETPUSHUNDEF;
    }
    else
	SETERRNO(0, SS_NORMAL);

    /* Assume success here to prevent recursive requirement. */
    /* name is never assigned to again, so len is still strlen(name)  */
    /* Check whether a hook in @INC has already filled %INC */
    if (!hook_sv) {
	(void)hv_store(GvHVn(PL_incgv), name, len, newSVpv(CopFILE(&PL_compiling),0),0);
    } else {
	SV** const svp = hv_fetch(GvHVn(PL_incgv), name, len, 0);
	if (!svp)
	    (void)hv_store(GvHVn(PL_incgv), name, len, SvREFCNT_inc_simple(hook_sv), 0 );
    }

    ENTER;
    SAVETMPS;
    lex_start(NULL);
    SAVEGENERICSV(PL_rsfp_filters);
    PL_rsfp_filters = NULL;

    PL_rsfp = tryrsfp;
    SAVEHINTS();
    PL_hints = 0;
    SAVESPTR(PL_compiling.cop_warnings);
    if (PL_dowarn & G_WARN_ALL_ON)
        PL_compiling.cop_warnings = pWARN_ALL ;
    else if (PL_dowarn & G_WARN_ALL_OFF)
        PL_compiling.cop_warnings = pWARN_NONE ;
    else if (PL_taint_warn)
        PL_compiling.cop_warnings = newSVpvn(WARN_TAINTstring, WARNsize);
    else
        PL_compiling.cop_warnings = pWARN_STD ;
    SAVESPTR(PL_compiling.cop_io);
    PL_compiling.cop_io = NULL;

    if (filter_sub || filter_cache) {
	SV * const datasv = filter_add(S_run_user_filter, NULL);
	IoLINES(datasv) = filter_has_file;
	IoTOP_GV(datasv) = (GV *)filter_state;
	IoBOTTOM_GV(datasv) = (GV *)filter_sub;
	IoFMT_GV(datasv) = (GV *)filter_cache;
    }

    /* switch to eval mode */
    push_return(PL_op->op_next);
    PUSHBLOCK(cx, CXt_EVAL, SP);
    PUSHEVAL(cx, name, NULL);

    SAVECOPLINE(&PL_compiling);
    CopLINE_set(&PL_compiling, 0);

    PUTBACK;
#ifdef USE_5005THREADS
    MUTEX_LOCK(&PL_eval_mutex);
    if (PL_eval_owner && PL_eval_owner != thr)
	while (PL_eval_owner)
	    COND_WAIT(&PL_eval_cond, &PL_eval_mutex);
    PL_eval_owner = thr;
    MUTEX_UNLOCK(&PL_eval_mutex);
#endif /* USE_5005THREADS */

    /* Store and reset encoding. */
    encoding = PL_encoding;
    PL_encoding = NULL;

    if (doeval(gimme, NULL, NULL, PL_curcop->cop_seq))
	op = DOCATCH(PL_eval_start);
    else
	op = PL_op->op_next;

    /* Restore encoding. */
    PL_encoding = encoding;

    return op;
}

PP(pp_entereval)
{
    dSP;
    register PERL_CONTEXT *cx;
    dPOPss;
    const I32 gimme = GIMME_V;
    const I32 was = PL_sub_generation;
    char tbuf[TYPE_DIGITS(long) + 12];
    char *tmpbuf = tbuf;
    char *safestr;
    STRLEN len;
    bool ok;
    CV* runcv;
    U32 seq;
    const char * const fakestr = "_<(eval )";
    const int fakelen = 9 + 1;

    TAINT_IF(SvTAINTED(sv));
    TAINT_PROPER("eval");

    ENTER;
    lex_start(sv);
    SAVETMPS;

    /* switch to eval mode */

    if (PERLDB_NAMEEVAL && CopLINE(PL_curcop)) {
	SV * const temp_sv = sv_newmortal();
	Perl_sv_setpvf(aTHX_ temp_sv, "_<(eval %lu)[%s:%"IVdf"]",
		       (unsigned long)++PL_evalseq,
		       CopFILE(PL_curcop), (IV)CopLINE(PL_curcop));
	tmpbuf = SvPVX(temp_sv);
	len = SvCUR(temp_sv);
    }
    else
	len = my_snprintf(tmpbuf, sizeof(tbuf), "_<(eval %lu)", (unsigned long)++PL_evalseq);
    SAVECOPFILE_FREE(&PL_compiling);
    CopFILE_set(&PL_compiling, tmpbuf+2);
    SAVECOPLINE(&PL_compiling);
    CopLINE_set(&PL_compiling, 1);
    /* XXX For C<eval "...">s within BEGIN {} blocks, this ends up
       deleting the eval's FILEGV from the stash before gv_check() runs
       (i.e. before run-time proper). To work around the coredump that
       ensues, we always turn GvMULTI_on for any globals that were
       introduced within evals. See force_ident(). GSAR 96-10-12 */
    safestr = savepvn(tmpbuf, len);
    SAVEDELETE(PL_defstash, safestr, len);
    SAVEHINTS();
    PL_hints = PL_op->op_targ;
    SAVESPTR(PL_compiling.cop_warnings);
    if (specialWARN(PL_curcop->cop_warnings))
        PL_compiling.cop_warnings = PL_curcop->cop_warnings;
    else {
        PL_compiling.cop_warnings = newSVsv(PL_curcop->cop_warnings);
        SAVEFREESV(PL_compiling.cop_warnings);
    }
    SAVESPTR(PL_compiling.cop_io);
    if (specialCopIO(PL_curcop->cop_io))
        PL_compiling.cop_io = PL_curcop->cop_io;
    else {
        PL_compiling.cop_io = newSVsv(PL_curcop->cop_io);
        SAVEFREESV(PL_compiling.cop_io);
    }
    /* special case: an eval '' executed within the DB package gets lexically
     * placed in the first non-DB CV rather than the current CV - this
     * allows the debugger to execute code, find lexicals etc, in the
     * scope of the code being debugged. Passing &seq gets find_runcv
     * to do the dirty work for us */
    runcv = find_runcv(&seq);

    push_return(PL_op->op_next);
    PUSHBLOCK(cx, (CXt_EVAL|CXp_REAL), SP);
    PUSHEVAL(cx, 0, NULL);

    /* prepare to compile string */

    if ((PERLDB_LINE || PERLDB_SAVESRC) && PL_curstash != PL_debstash)
	save_lines(CopFILEAV(&PL_compiling), PL_linestr);
    PUTBACK;
#ifdef USE_5005THREADS
    MUTEX_LOCK(&PL_eval_mutex);
    if (PL_eval_owner && PL_eval_owner != thr)
	while (PL_eval_owner)
	    COND_WAIT(&PL_eval_cond, &PL_eval_mutex);
    PL_eval_owner = thr;
    MUTEX_UNLOCK(&PL_eval_mutex);
#endif /* USE_5005THREADS */
    ok = doeval(gimme, NULL, runcv, seq);
    if (ok ? (was != (I32)PL_sub_generation /* Some subs defined here. */
	      ? (PERLDB_LINE || PERLDB_SAVESRC)
	      :  PERLDB_SAVESRC_NOSUBS)
	: 0 /* PERLDB_SAVESRC_INVALID */
	/* Much that I'd like to think that it was this trivial to add this
	   feature, it's not, due to
	       lex_end();
	       LEAVE;
	   in S_doeval() for the failure case. So really we want a more
	   sophisticated way of (optionally) clearing the source code.
	   Particularly as the current way is buggy, as a syntactically
	   invalid eval string can still define a subroutine that is retained,
	   and the user may wish to breakpoint. */) {
	/* Copy in anything fake and short. */
	my_strlcpy(safestr, fakestr, fakelen);
    }
    return ok ? DOCATCH(PL_eval_start) : PL_op->op_next;
}

PP(pp_leaveeval)
{
    dSP;
    register SV **mark;
    SV **newsp;
    PMOP *newpm;
    I32 gimme;
    register PERL_CONTEXT *cx;
    OP *retop;
    const U8 save_flags = PL_op -> op_flags;
    I32 optype;

    POPBLOCK(cx,newpm);
    POPEVAL(cx);
    retop = pop_return();

    TAINT_NOT;
    if (gimme == G_VOID)
	MARK = newsp;
    else if (gimme == G_SCALAR) {
	MARK = newsp + 1;
	if (MARK <= SP) {
	    if (SvFLAGS(TOPs) & SVs_TEMP)
		*MARK = TOPs;
	    else
		*MARK = sv_mortalcopy(TOPs);
	}
	else {
	    MEXTEND(mark,0);
	    *MARK = &PL_sv_undef;
	}
	SP = MARK;
    }
    else {
	/* in case LEAVE wipes old return values */
	for (mark = newsp + 1; mark <= SP; mark++) {
	    if (!(SvFLAGS(*mark) & SVs_TEMP)) {
		*mark = sv_mortalcopy(*mark);
		TAINT_NOT;	/* Each item is independent */
	    }
	}
    }
    PL_curpm = newpm;	/* Don't pop $1 et al till now */

#ifdef DEBUGGING
    assert(CvDEPTH(PL_compcv) == 1);
#endif
    CvDEPTH(PL_compcv) = 0;
    lex_end();

    if (optype == OP_REQUIRE &&
	!(gimme == G_SCALAR ? SvTRUE(*SP) : SP > newsp))
    {
	/* Unassume the success we assumed earlier. */
	SV * const nsv = cx->blk_eval.old_namesv;
	(void)hv_delete(GvHVn(PL_incgv), SvPVX_const(nsv), SvCUR(nsv), G_DISCARD);
	retop = Perl_die(aTHX_ "%"SVf" did not return a true value", (void*)nsv);
	/* die_where() did LEAVE, or we won't be here */
    }
    else {
	LEAVE;
	if (!(save_flags & OPf_SPECIAL)) {
	    CLEAR_ERRSV();
	}
    }

    RETURNOP(retop);
}

/* Common code for Perl_call_sv and Perl_fold_constants, put here to keep it
   close to the related Perl_create_eval_scope.  */
void
Perl_delete_eval_scope(pTHX)
{
    SV **newsp;
    PMOP *newpm;
    I32 gimme;
    register PERL_CONTEXT *cx;
    I32 optype;
	
    POPBLOCK(cx,newpm);
    POPEVAL(cx);
    PL_curpm = newpm;
    LEAVE;
    pop_return();
    PERL_UNUSED_VAR(newsp);
    PERL_UNUSED_VAR(gimme);
    PERL_UNUSED_VAR(optype);
}

/* Common-ish code salvaged from Perl_call_sv and pp_entertry, because it was
   also needed by Perl_fold_constants.  */
PERL_CONTEXT *
Perl_create_eval_scope(pTHX_ U32 flags)
{
    PERL_CONTEXT *cx;
    const I32 gimme = GIMME_V;
	
    ENTER;
    SAVETMPS;

    PUSHBLOCK(cx, (CXt_EVAL|CXp_TRYBLOCK), PL_stack_sp);
    PUSHEVAL(cx, 0, 0);

    PL_in_eval = EVAL_INEVAL;
    if (flags & G_KEEPERR)
	PL_in_eval |= EVAL_KEEPERR;
    else
	CLEAR_ERRSV();
    if (flags & G_FAKINGEVAL) {
	PL_eval_root = PL_op; /* Only needed so that goto works right. */
    }
    return cx;
}
    
PP(pp_entertry)
{
    push_return(cLOGOP->op_other->op_next);
    create_eval_scope(0);
    return DOCATCH(PL_op->op_next);
}

PP(pp_leavetry)
{
    dSP;
    SV **newsp;
    PMOP *newpm;
    I32 gimme;
    register PERL_CONTEXT *cx;
    I32 optype;

    POPBLOCK(cx,newpm);
    POPEVAL(cx);

    TAINT_NOT;
    if (gimme == G_VOID)
	SP = newsp;
    else if (gimme == G_SCALAR) {
	register SV **mark;
	MARK = newsp + 1;
	if (MARK <= SP) {
	    if (SvFLAGS(TOPs) & (SVs_PADTMP|SVs_TEMP))
		*MARK = TOPs;
	    else
		*MARK = sv_mortalcopy(TOPs);
	}
	else {
	    MEXTEND(mark,0);
	    *MARK = &PL_sv_undef;
	}
	SP = MARK;
    }
    else {
	/* in case LEAVE wipes old return values */
	register SV **mark;
	for (mark = newsp + 1; mark <= SP; mark++) {
	    if (!(SvFLAGS(*mark) & (SVs_PADTMP|SVs_TEMP))) {
		*mark = sv_mortalcopy(*mark);
		TAINT_NOT;	/* Each item is independent */
	    }
	}
    }
    PL_curpm = newpm;	/* Don't pop $1 et al till now */

    LEAVE;
    CLEAR_ERRSV();
    RETURN;
}

STATIC OP *
S_doparseform(pTHX_ SV *sv)
{
    STRLEN len;
    register char *s = SvPV_force(sv, len);
    register char * const send = s + len;
    register char *base = NULL;
    register I32 skipspaces = 0;
    bool noblank   = FALSE;
    bool repeat    = FALSE;
    bool postspace = FALSE;
    U32 *fops;
    register U32 *fpc;
    U32 *linepc = NULL;
    register I32 arg;
    bool ischop;
    bool unchopnum = FALSE;
    int maxops = 12; /* FF_LINEMARK + FF_END + 10 (\0 without preceding \n) */

    if (len == 0)
	Perl_croak(aTHX_ "Null picture in formline");

    /* estimate the buffer size needed */
    for (base = s; s <= send; s++) {
	if (*s == '\n' || *s == '@' || *s == '^')
	    maxops += 10;
    }
    s = base;
    base = NULL;

    Newx(fops, maxops, U32);
    fpc = fops;

    if (s < send) {
	linepc = fpc;
	*fpc++ = FF_LINEMARK;
	noblank = repeat = FALSE;
	base = s;
    }

    while (s <= send) {
	switch (*s++) {
	default:
	    skipspaces = 0;
	    continue;

	case '~':
	    if (*s == '~') {
		repeat = TRUE;
		*s = ' ';
	    }
	    noblank = TRUE;
	    s[-1] = ' ';
	    /* FALL THROUGH */
	case ' ': case '\t':
	    skipspaces++;
	    continue;
        case 0:
	    if (s < send) {
	        skipspaces = 0;
                continue;
            } /* else FALL THROUGH */
	case '\n':
	    arg = s - base;
	    skipspaces++;
	    arg -= skipspaces;
	    if (arg) {
		if (postspace)
		    *fpc++ = FF_SPACE;
		*fpc++ = FF_LITERAL;
		*fpc++ = (U16)arg;
	    }
	    postspace = FALSE;
	    if (s <= send)
		skipspaces--;
	    if (skipspaces) {
		*fpc++ = FF_SKIP;
		*fpc++ = (U16)skipspaces;
	    }
	    skipspaces = 0;
	    if (s <= send)
		*fpc++ = FF_NEWLINE;
	    if (noblank) {
		*fpc++ = FF_BLANK;
		if (repeat)
		    arg = fpc - linepc + 1;
		else
		    arg = 0;
		*fpc++ = (U16)arg;
	    }
	    if (s < send) {
		linepc = fpc;
		*fpc++ = FF_LINEMARK;
		noblank = repeat = FALSE;
		base = s;
	    }
	    else
		s++;
	    continue;

	case '@':
	case '^':
	    ischop = s[-1] == '^';

	    if (postspace) {
		*fpc++ = FF_SPACE;
		postspace = FALSE;
	    }
	    arg = (s - base) - 1;
	    if (arg) {
		*fpc++ = FF_LITERAL;
		*fpc++ = (U16)arg;
	    }

	    base = s - 1;
	    *fpc++ = FF_FETCH;
	    if (*s == '*') {
		s++;
		*fpc++ = 2;  /* skip the @* or ^* */
		if (ischop) {
		    *fpc++ = FF_LINESNGL;
		    *fpc++ = FF_CHOP;
		} else
		    *fpc++ = FF_LINEGLOB;
	    }
	    else if (*s == '#' || (*s == '.' && s[1] == '#')) {
		arg = ischop ? 512 : 0;
		base = s - 1;
		while (*s == '#')
		    s++;
		if (*s == '.') {
                    const char * const f = ++s;
		    while (*s == '#')
			s++;
		    arg |= 256 + (s - f);
		}
		*fpc++ = s - base;		/* fieldsize for FETCH */
		*fpc++ = FF_DECIMAL;
                *fpc++ = (U16)arg;
                unchopnum |= ! ischop;
            }
            else if (*s == '0' && s[1] == '#') {  /* Zero padded decimals */
                arg = ischop ? 512 : 0;
		base = s - 1;
                s++;                                /* skip the '0' first */
                while (*s == '#')
                    s++;
                if (*s == '.') {
                    const char * const f = ++s;
                    while (*s == '#')
                        s++;
                    arg |= 256 + (s - f);
                }
                *fpc++ = s - base;                /* fieldsize for FETCH */
                *fpc++ = FF_0DECIMAL;
		*fpc++ = (U16)arg;
                unchopnum |= ! ischop;
	    }
	    else {
		I32 prespace = 0;
		bool ismore = FALSE;

		if (*s == '>') {
		    while (*++s == '>') ;
		    prespace = FF_SPACE;
		}
		else if (*s == '|') {
		    while (*++s == '|') ;
		    prespace = FF_HALFSPACE;
		    postspace = TRUE;
		}
		else {
		    if (*s == '<')
			while (*++s == '<') ;
		    postspace = TRUE;
		}
		if (*s == '.' && s[1] == '.' && s[2] == '.') {
		    s += 3;
		    ismore = TRUE;
		}
		*fpc++ = s - base;		/* fieldsize for FETCH */

		*fpc++ = ischop ? FF_CHECKCHOP : FF_CHECKNL;

		if (prespace)
		    *fpc++ = (U16)prespace;
		*fpc++ = FF_ITEM;
		if (ismore)
		    *fpc++ = FF_MORE;
		if (ischop)
		    *fpc++ = FF_CHOP;
	    }
	    base = s;
	    skipspaces = 0;
	    continue;
	}
    }
    *fpc++ = FF_END;

    assert (fpc <= fops + maxops); /* ensure our buffer estimate was valid */
    arg = fpc - fops;
    { /* need to jump to the next word */
        int z;
	z = WORD_ALIGN - SvCUR(sv) % WORD_ALIGN;
	SvGROW(sv, SvCUR(sv) + z + arg * sizeof(U32) + 4);
	s = SvPVX(sv) + SvCUR(sv) + z;
    }
    Copy(fops, s, arg, U32);
    Safefree(fops);
    sv_magic(sv, NULL, PERL_MAGIC_fm, NULL, 0);
    SvCOMPILED_on(sv);

    if (unchopnum && repeat)
        DIE(aTHX_ "Repeated format line will never terminate (~~ and @#)");
    return 0;
}


STATIC bool
S_num_overflow(NV value, I32 fldsize, I32 frcsize)
{
    /* Can value be printed in fldsize chars, using %*.*f ? */
    NV pwr = 1;
    NV eps = 0.5;
    bool res = FALSE;
    int intsize = fldsize - (value < 0 ? 1 : 0);

    if (frcsize & 256)
        intsize--;
    frcsize &= 255;
    intsize -= frcsize;

    while (intsize--) pwr *= 10.0;
    while (frcsize--) eps /= 10.0;

    if( value >= 0 ){
        if (value + eps >= pwr)
	    res = TRUE;
    } else {
        if (value - eps <= -pwr)
	    res = TRUE;
    }
    return res;
}

static I32
S_run_user_filter(pTHX_ int idx, SV *buf_sv, int maxlen)
{
    SV * const datasv = FILTER_DATA(idx);
    const int filter_has_file = IoLINES(datasv);
    SV * const filter_state = (SV *)IoTOP_GV(datasv);
    SV * const filter_sub = (SV *)IoBOTTOM_GV(datasv);
    int status = 0;
    SV *upstream;
    STRLEN got_len;
    const char *got_p = NULL;
    const char *prune_from = NULL;
    bool read_from_cache = FALSE;
    STRLEN umaxlen;

    assert(maxlen >= 0);
    umaxlen = maxlen;

    /* I was having segfault trouble under Linux 2.2.5 after a
       parse error occured.  (Had to hack around it with a test
       for PL_error_count == 0.)  Solaris doesn't segfault --
       not sure where the trouble is yet.  XXX */

    if (IoFMT_GV(datasv)) {
	SV *const cache = (SV *)IoFMT_GV(datasv);
	if (SvOK(cache)) {
	    STRLEN cache_len;
	    const char *cache_p = SvPV(cache, cache_len);
	    STRLEN take = 0;

	    if (umaxlen) {
		/* Running in block mode and we have some cached data already.
		 */
		if (cache_len >= umaxlen) {
		    /* In fact, so much data we don't even need to call
		       filter_read.  */
		    take = umaxlen;
		}
	    } else {
		const char *const first_nl =
		    (const char *)memchr(cache_p, '\n', cache_len);
		if (first_nl) {
		    take = first_nl + 1 - cache_p;
		}
	    }
	    if (take) {
		sv_catpvn(buf_sv, cache_p, take);
		sv_chop(cache, (char *)cache_p + take);
		/* Definately not EOF  */
		return 1;
	    }

	    sv_catsv(buf_sv, cache);
	    if (umaxlen) {
		umaxlen -= cache_len;
	    }
	    SvOK_off(cache);
	    read_from_cache = TRUE;
	}
    }

    /* Filter API says that the filter appends to the contents of the buffer.
       Usually the buffer is "", so the details don't matter. But if it's not,
       then clearly what it contains is already filtered by this filter, so we
       don't want to pass it in a second time.
       I'm going to use a mortal in case the upstream filter croaks.  */
    upstream = ((SvOK(buf_sv) && sv_len(buf_sv)) || SvGMAGICAL(buf_sv))
	? sv_newmortal() : buf_sv;
    SvUPGRADE(upstream, SVt_PV);
	
    if (filter_has_file) {
	status = FILTER_READ(idx+1, upstream, 0);
    }

    if (filter_sub && status >= 0) {
	dSP;
	int count;

	ENTER;
	SAVE_DEFSV;
	SAVETMPS;
	EXTEND(SP, 2);

	DEFSV = upstream;
	PUSHMARK(SP);
	mPUSHi(0);
	if (filter_state) {
	    PUSHs(filter_state);
	}
	PUTBACK;
	count = call_sv(filter_sub, G_SCALAR);
	SPAGAIN;

	if (count > 0) {
	    SV *out = POPs;
	    if (SvOK(out)) {
		status = SvIV(out);
	    }
	}

	PUTBACK;
	FREETMPS;
	LEAVE;
    }

    if(SvOK(upstream)) {
	got_p = SvPV(upstream, got_len);
	if (umaxlen) {
	    if (got_len > umaxlen) {
		prune_from = got_p + umaxlen;
	    }
	} else {
	    const char *const first_nl =
		(const char *)memchr(got_p, '\n', got_len);
	    if (first_nl && first_nl + 1 < got_p + got_len) {
		/* There's a second line here... */
		prune_from = first_nl + 1;
	    }
	}
    }
    if (prune_from) {
	/* Oh. Too long. Stuff some in our cache.  */
	STRLEN cached_len = got_p + got_len - prune_from;
	SV *cache = (SV *)IoFMT_GV(datasv);

	if (!cache) {
	    IoFMT_GV(datasv) = (GV*) (cache = newSV(got_len - umaxlen));
	} else if (SvOK(cache)) {
	    /* Cache should be empty.  */
	    assert(!SvCUR(cache));
	}

	sv_setpvn(cache, prune_from, cached_len);
	/* If you ask for block mode, you may well split UTF-8 characters.
	   "If it breaks, you get to keep both parts"
	   (Your code is broken if you  don't put them back together again
	   before something notices.) */
	if (SvUTF8(upstream)) {
	    SvUTF8_on(cache);
	}
	SvCUR_set(upstream, got_len - cached_len);
	/* Can't yet be EOF  */
	if (status == 0)
	    status = 1;
    }

    /* If they are at EOF but buf_sv has something in it, then they may never
       have touched the SV upstream, so it may be undefined.  If we naively
       concatenate it then we get a warning about use of uninitialised value.
    */
    if (upstream != buf_sv && (SvOK(upstream) || SvGMAGICAL(upstream))) {
	sv_catsv(buf_sv, upstream);
    }

    if (status <= 0) {
	IoLINES(datasv) = 0;
	SvREFCNT_dec(IoFMT_GV(datasv));
	if (filter_state) {
	    SvREFCNT_dec(filter_state);
	    IoTOP_GV(datasv) = NULL;
	}
	if (filter_sub) {
	    SvREFCNT_dec(filter_sub);
	    IoBOTTOM_GV(datasv) = NULL;
	}
	filter_del(S_run_user_filter);
    }
    if (status == 0 && read_from_cache) {
	/* If we read some data from the cache (and by getting here it implies
	   that we emptied the cache) then we aren't yet at EOF, and mustn't
	   report that to our caller.  */
	return 1;
    }
    return status;
}

/* perhaps someone can come up with a better name for
   this?  it is not really "absolute", per se ... */
static bool
S_path_is_absolute(const char *name)
{
    if (PERL_FILE_IS_ABSOLUTE(name)
#ifdef MACOS_TRADITIONAL
	|| (*name == ':')
#else
	|| (*name == '.' && (name[1] == '/' ||
			     (name[1] == '.' && name[2] == '/')))
#endif
	 )
    {
	return TRUE;
    }
    else
    	return FALSE;
}

/*
 * Local variables:
 * c-indentation-style: bsd
 * c-basic-offset: 4
 * indent-tabs-mode: t
 * End:
 *
 * ex: set ts=8 sts=4 sw=4 noet:
 */

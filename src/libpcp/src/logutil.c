/*
 * Copyright (c) 2012-2017,2020-2021 Red Hat.
 * Copyright (c) 1995-2002,2004 Silicon Graphics, Inc.  All Rights Reserved.
 *
 * This library is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published
 * by the Free Software Foundation; either version 2.1 of the License, or
 * (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU Lesser General Public
 * License for more details.
 *
 * Thread-safe notes:
 *
 * __pmLogReads is a diagnostic counter that is maintained with
 * non-atomic updates ... we've decided that it is acceptable for the
 * value to be subject to possible (but unlikely) missed updates
 */

#include <inttypes.h>
#include <assert.h>
#include <sys/stat.h>
#include "pmapi.h"
#include "libpcp.h"
#include "internal.h"

PCP_DATA int	__pmLogReads;

/*
 * first two fields are made to look like a pmValueSet when no values are
 * present ... used to populate the pmValueSet in a pmResult when values
 * for particular metrics are not available from this log record.
 */
typedef struct {
    pmID	pc_pmid;
    int		pc_numval;	/* MUST be 0 */
    				/* value control for interpolation */
} pmid_ctl;

/*
 * Hash control for requested metrics, used to construct 'No values'
 * result when the corresponding metric is requested but there is
 * no values available in the pmResult
 *
 * Note, this hash table is global across all contexts.
 */
static __pmHashCtl	pc_hc;

static int LogCheckForNextArchive(__pmContext *, int, pmResult **);
static int LogChangeToNextArchive(__pmContext *);
static int LogChangeToPreviousArchive(__pmContext *);

#ifdef PM_MULTI_THREAD
static pthread_mutex_t	logutil_lock = PTHREAD_MUTEX_INITIALIZER;
#else
void			*logutil_lock;
#endif

/*
 * Control structure for the current context ...
 */
typedef struct {
    __pmContext	*ctxp;			/* NULL or a locked context */
    int		need_ctx_unlock;	/* 1 if the context lock was acquired */
    					/* in a call to lock_ctx() */
} ctx_ctl_t;

#if defined(PM_MULTI_THREAD) && defined(PM_MULTI_THREAD_DEBUG)
/*
 * return true if lock == logutil_lock
 */
int
__pmIsLogutilLock(void *lock)
{
    return lock == (void *)&logutil_lock;
}
#endif

static void
dumpbuf(int nch, __pmPDU *pb)
{
    int		i, j;

    nch /= sizeof(__pmPDU);
    fprintf(stderr, "%03d: ", 0);
    for (j = 0, i = 0; j < nch; j++) {
	if (i == 8) {
	    fprintf(stderr, "\n%03d: ", j);
	    i = 0;
	}
	fprintf(stderr, "%8x ", pb[j]);
	i++;
    }
    fputc('\n', stderr);
}

/*
 * ensure the current context is locked
 */
static int
lock_ctx(__pmContext *ctxp, ctx_ctl_t *ccp)
{
    if (ctxp == NULL)
	return PM_ERR_NOCONTEXT;

    ccp->ctxp = ctxp;
    if (PM_IS_LOCKED(ctxp->c_lock))
	ccp->need_ctx_unlock = 0;
    else {
	PM_LOCK(ctxp->c_lock);
	ccp->need_ctx_unlock = 1;
    }

    return 0;
}

static int
__pmLogLabelVersion(__pmLogLabel *llp)
{
//#ifdef __PCP_EXPERIMENTAL_ARCHIVE_VERSION3
//    return (llp->all.ill_magic & 0xff);
//#else
    return (llp->ill_magic & 0xff);
//#endif
}

int
__pmLogVersion(__pmLogCtl *lcp)
{
    return __pmLogLabelVersion(&lcp->l_label);
}

static size_t
__pmLogLabelSize(__pmLogCtl *lcp)
{
//#ifdef __PCP_EXPERIMENTAL_ARCHIVE_VERSION3
//    if (__pmLogLabelVersion(&lcp->l_label) >= PM_LOG_VERS03)
//	return lcp->l_label.v3.ill_align_len;
//    return sizeof(__pmLogLabel2);
//#else
    (void)lcp;
    return sizeof(__pmLogLabel);
//#endif
}

static size_t
__pmLogLabelTotalSize(__pmLogCtl *lcp)
{
    /* include the preamble and trailing lengths */
    return __pmLogLabelSize(lcp) + 2 * sizeof(int);
}

static int
checkLabelConsistency(__pmContext *ctxp, const __pmLogLabel *lp)
{
    __pmArchCtl	*acp;

    /* No checking to do if there are no other archives */
    acp = ctxp->c_archctl;
    if (acp->ac_num_logs < 1)
	return 0; /* ok */

    /*
     * When checking for consistency, it is sufficient to check vs the 
     * first archive in the context.
     * The version number is checked by __pmLogChkLabel.
     * Check the hostname.
     */
    if (strcmp(lp->ill_hostname, ctxp->c_archctl->ac_log_list[0]->ml_hostname) != 0)
	return PM_ERR_LOGHOST;

    /* All is ok */
    return 0;
}

int
__pmLogChkLabel(__pmArchCtl *acp, __pmFILE *f, __pmLogLabel *lp, int vol)
{
    __pmLogCtl	*lcp = acp->ac_log;
    int		len = 0;
    int		version = UNKNOWN_VERSION;
    int		xpectlen = sizeof(__pmLogLabel) + 2 * sizeof(len);
    int		n;
    int		sts;
    int		diag_output = 0;
    struct stat	sbuf;

    if (vol >= 0 && vol < lcp->l_numseen && lcp->l_seen[vol]) {
	/* FastPath, cached result of previous check for this volume */
	__pmFseek(f, (long)(sizeof(__pmLogLabel) + 2*sizeof(int)), SEEK_SET);
	version = 0;
	goto func_return;
    }

    if (vol >= 0 && vol >= lcp->l_numseen) {
	lcp->l_seen = (int *)realloc(lcp->l_seen, (vol+1)*(int)sizeof(lcp->l_seen[0]));
	if (lcp->l_seen == NULL)
	    lcp->l_numseen = 0;
	else {
	    int 	i;
	    for (i = lcp->l_numseen; i < vol; i++)
		lcp->l_seen[i] = 0;
	    lcp->l_numseen = vol+1;
	}
    }

    if (pmDebugOptions.log) {
	fprintf(stderr, "__pmLogChkLabel: fd=%d vol=%d", __pmFileno(f), vol);
	diag_output = 1;
    }

    __pmFseek(f, (long)0, SEEK_SET);
    n = (int)__pmFread(&len, 1, sizeof(len), f);
    len = ntohl(len);
    if (n != sizeof(len) || len != xpectlen) {
	if (__pmFeof(f)) {
	    __pmClearerr(f);
	    if (pmDebugOptions.log)
		fprintf(stderr, " file is empty");
	    version = PM_ERR_NODATA;
	    goto func_return;
	}
	else {
	    if (pmDebugOptions.log)
		fprintf(stderr, " header read -> %d (expect %d) or bad header len=%d (expected %d)",
		    n, (int)sizeof(len), len, xpectlen);
	    if (__pmFerror(f)) {
		__pmClearerr(f);
		version = -oserror();
		goto func_return;
	    }
	    else {
		version = PM_ERR_LABEL;
		goto func_return;
	    }
	}
    }

    if ((n = (int)__pmFread(lp, 1, sizeof(__pmLogLabel), f)) != sizeof(__pmLogLabel)) {
	if (pmDebugOptions.log)
	    fprintf(stderr, " bad label len=%d: expected %d",
		n, (int)sizeof(__pmLogLabel));
	if (__pmFerror(f)) {
	    __pmClearerr(f);
	    version = -oserror();
	    goto func_return;
	}
	else {
	    version = PM_ERR_LABEL;
	    goto func_return;
	}
    }
    else {
	/* swab internal log label */
	lp->ill_magic = ntohl(lp->ill_magic);
	lp->ill_pid = ntohl(lp->ill_pid);
	lp->ill_start.tv_sec = ntohl(lp->ill_start.tv_sec);
	lp->ill_start.tv_usec = ntohl(lp->ill_start.tv_usec);
	lp->ill_vol = ntohl(lp->ill_vol);
    }

    n = (int)__pmFread(&len, 1, sizeof(len), f);
    len = ntohl(len);
    if (n != sizeof(len) || len != xpectlen) {
	if (pmDebugOptions.log)
	    fprintf(stderr, " trailer read -> %d (expect %d) or bad trailer len=%d (expected %d)",
		n, (int)sizeof(len), len, xpectlen);
	if (__pmFerror(f)) {
	    __pmClearerr(f);
	    version = -oserror();
	    goto func_return;
	}
	else {
	    version = PM_ERR_LABEL;
	    goto func_return;
	}
    }

    version = lp->ill_magic & 0xff;
    if ((lp->ill_magic & 0xffffff00) != PM_LOG_MAGIC ||
	(version != PM_LOG_VERS02) || lp->ill_vol != vol) {
	if (pmDebugOptions.log) {
	    if ((lp->ill_magic & 0xffffff00) != PM_LOG_MAGIC)
		fprintf(stderr, " label magic 0x%x not 0x%x as expected", (lp->ill_magic & 0xffffff00), PM_LOG_MAGIC);
	    if (version != PM_LOG_VERS02)
		fprintf(stderr, " label version %d not supported", version);
	    if (lp->ill_vol != vol)
		fprintf(stderr, " label volume %d not %d as expected", lp->ill_vol, vol);
	}
	version = PM_ERR_LABEL;
	goto func_return;
    }

    if (__pmSetVersionIPC(__pmFileno(f), version) < 0)
	return -oserror();
    if (pmDebugOptions.log)
	fprintf(stderr, " [magic=%8x version=%d vol=%d pid=%d host=%s]",
		lp->ill_magic, version, lp->ill_vol, lp->ill_pid, lp->ill_hostname);

    /*
     * If we have the label record, and nothing else this is really
     * an empty archive (probably pmlogger was killed off before any
     * data records were written) ... better to return PM_ERR_NODATA
     * here, rather than to stumble into PM_ERR_LOGREC at the first
     * call to __pmLogRead*()
     */
    if ((sts = __pmFstat(f, &sbuf)) >= 0) {
	if (sbuf.st_size == xpectlen) {
	    if (pmDebugOptions.log)
		fprintf(stderr, " file is empty");
	    version = PM_ERR_NODATA;
	}
    }

    if (vol >= 0 && vol < lcp->l_numseen)
	lcp->l_seen[vol] = 1;

func_return:
    if (pmDebugOptions.log && diag_output)
	fputc('\n', stderr);

    return version;
}

static __pmFILE *
_logpeek(__pmArchCtl *acp, int vol)
{
    __pmLogCtl		*lcp = acp->ac_log;
    int			sts;
    __pmFILE		*f;
    __pmLogLabel	label;
    char		fname[MAXPATHLEN];

    pmsprintf(fname, sizeof(fname), "%s.%d", lcp->l_name, vol);
    /* need mutual exclusion here to avoid race with a concurrent uncompress */
    PM_LOCK(logutil_lock);
    if ((f = __pmFopen(fname, "r")) == NULL) {
	PM_UNLOCK(logutil_lock);
	return f;
    }
    PM_UNLOCK(logutil_lock);

    if ((sts = __pmLogChkLabel(acp, f, &label, vol)) < 0) {
	__pmFclose(f);
	setoserror(sts);
	return NULL;
    }
    
    return f;
}

int
__pmLogChangeVol(__pmArchCtl *acp, int vol)
{
    __pmLogCtl	*lcp = acp->ac_log;
    char	fname[MAXPATHLEN];
    int		sts;

    if (acp->ac_curvol == vol)
	return 0;

    if (acp->ac_mfp != NULL) {
	__pmResetIPC(__pmFileno(acp->ac_mfp));
	__pmFclose(acp->ac_mfp);
    }
    pmsprintf(fname, sizeof(fname), "%s.%d", lcp->l_name, vol);
    /* need mutual exclusion here to avoid race with a concurrent uncompress */
    PM_LOCK(logutil_lock);
    if ((acp->ac_mfp = __pmFopen(fname, "r")) == NULL) {
	PM_UNLOCK(logutil_lock);
	return -oserror();
    }
    PM_UNLOCK(logutil_lock);

    if ((sts = __pmLogChkLabel(acp, acp->ac_mfp, &lcp->l_label, vol)) < 0) {
	return sts;
    }
    acp->ac_curvol = vol;

    if (pmDebugOptions.log)
	fprintf(stderr, "__pmLogChangeVol: change to volume %d\n", vol);

    return sts;
}

#ifdef __PCP_EXPERIMENTAL_ARCHIVE_VERSION3
static int
__pmLogLoadIndex3(__pmLogCtl *lcp)
{
    int		sts = 0;
    __pmFILE	*f = lcp->l_tifp;
    __pmLogTI3	*tip3;
    size_t	bytes;

    lcp->l_numti = 0;
    lcp->l_ti = NULL;

    if (lcp->l_tifp != NULL) {
	__pmFseek(f, (long)__pmLogLabelTotalSize(lcp), SEEK_SET);
	for ( ; ; ) {
	    bytes = (1 + lcp->l_numti) * sizeof(__pmLogTI);
	    lcp->l_ti = (__pmLogTI *)realloc(lcp->l_ti, bytes);
	    if (lcp->l_ti == NULL) {
		sts = -oserror();
		break;
	    }
	    tip3 = &lcp->l_ti[lcp->l_numti].v3;
	    bytes = __pmFread(tip3, 1, sizeof(__pmLogTI3), f);
	    if (bytes != sizeof(__pmLogTI3)) {
		if (__pmFeof(f)) {
		    __pmClearerr(f);
		    sts = 0; 
		    break;
		}
	  	if (pmDebugOptions.log)
	    	    fprintf(stderr, "%s: bad TI entry len=%zu: expected %zu\n",
			    "__pmLogLoadIndex3", bytes, sizeof(__pmLogTI3));
		if (__pmFerror(f)) {
		    __pmClearerr(f);
		    sts = -oserror();
		    break;
		}
		else {
		    sts = PM_ERR_LOGREC;
		    break;
		}
	    }
	    else {
		/* swab the version 3 archive temporal index record */
		__htonll((char *)&tip3->ti_sec);
		tip3->ti_nsec = ntohl(tip3->ti_nsec);
		tip3->ti_vol = ntohl(tip3->ti_vol);
		__htonll((char *)&tip3->ti_meta);
		__htonll((char *)&tip3->ti_log);
	    }

	    lcp->l_numti++;
	}
    }
    return sts;
}
#endif

static int
__pmLogLoadIndex2(__pmLogCtl *lcp)
{
    int		sts = 0;
    __pmFILE	*f = lcp->l_tifp;
    __pmLogTI2	*tip2;
    size_t	bytes;

    lcp->l_numti = 0;
    lcp->l_ti = NULL;

    if (lcp->l_tifp != NULL) {
	__pmFseek(f, (long)__pmLogLabelTotalSize(lcp), SEEK_SET);
	for ( ; ; ) {
	    bytes = (1 + lcp->l_numti) * sizeof(__pmLogTI);
	    lcp->l_ti = (__pmLogTI *)realloc(lcp->l_ti, bytes);
	    if (lcp->l_ti == NULL) {
		sts = -oserror();
		break;
	    }
	    tip2 = &lcp->l_ti[lcp->l_numti].v2;
	    bytes = __pmFread(tip2, 1, sizeof(__pmLogTI2), f);
	    if (bytes != sizeof(__pmLogTI2)) {
		if (__pmFeof(f)) {
		    __pmClearerr(f);
		    sts = 0; 
		    break;
		}
	  	if (pmDebugOptions.log)
	    	    fprintf(stderr, "%s: bad TI entry len=%zu: expected %zu\n",
			    "__pmLogLoadIndex2", bytes, sizeof(__pmLogTI2));
		if (__pmFerror(f)) {
		    __pmClearerr(f);
		    sts = -oserror();
		    break;
		}
		else {
		    sts = PM_ERR_LOGREC;
		    break;
		}
	    }
	    else {
		/* swab the version 2 temporal index record */
		tip2->ti_stamp.tv_sec = ntohl(tip2->ti_stamp.tv_sec);
		tip2->ti_stamp.tv_usec = ntohl(tip2->ti_stamp.tv_usec);
		tip2->ti_vol = ntohl(tip2->ti_vol);
		tip2->ti_meta = ntohl(tip2->ti_meta);
		tip2->ti_log = ntohl(tip2->ti_log);
	    }

	    lcp->l_numti++;
	}
    }

    return sts;
}

static int
__pmLogLoadIndex(__pmLogCtl *lcp)
{
#ifdef __PCP_EXPERIMENTAL_ARCHIVE_VERSION3
    if (__pmLogVersion(lcp) >= PM_LOG_VERS03)
	return __pmLogLoadIndex3(lcp);
#endif
    return __pmLogLoadIndex2(lcp);
}

const char *
__pmLogName_r(const char *base, int vol, char *buf, int buflen)
{
    switch (vol) {
	case PM_LOG_VOL_TI:
	    pmsprintf(buf, buflen, "%s.index", base);
	    break;

	case PM_LOG_VOL_META:
	    pmsprintf(buf, buflen, "%s.meta", base);
	    break;

	default:
	    pmsprintf(buf, buflen, "%s.%d", base, vol);
	    break;
    }

    return buf;
}

const char *
__pmLogName(const char *base, int vol)
{
    static char		tbuf[MAXPATHLEN];

    return __pmLogName_r(base, vol, tbuf, sizeof(tbuf));
}

__pmFILE *
__pmLogNewFile(const char *base, int vol)
{
    char	fname[MAXPATHLEN];
    __pmFILE	*f;
    int		save_error;

    __pmLogName_r(base, vol, fname, sizeof(fname));

    if (access(fname, R_OK) != -1) {
	/* exists and readable ... */
	pmprintf("__pmLogNewFile: \"%s\" already exists, not over-written\n", fname);
	pmflush();
	setoserror(EEXIST);
	return NULL;
    }

    if ((f = __pmFopen(fname, "w")) == NULL) {
	char	errmsg[PM_MAXERRMSGLEN];
	save_error = oserror();
	pmprintf("__pmLogNewFile: failed to create \"%s\": %s\n", fname, osstrerror_r(errmsg, sizeof(errmsg)));

	pmflush();
	setoserror(save_error);
	return NULL;
    }
    /*
     * Want unbuffered I/O for all files of the archive, so a single
     * fwrite() maps to one logical record for each of the metadata
     * records, the index records and the data (pmResult) records.
     */
    __pmSetvbuf(f, NULL, _IONBF, 0);

    if ((save_error = __pmSetVersionIPC(__pmFileno(f), PDU_VERSION)) < 0) {
	char	errmsg[PM_MAXERRMSGLEN];
	pmprintf("__pmLogNewFile: failed to setup \"%s\": %s\n", fname, osstrerror_r(errmsg, sizeof(errmsg)));
	pmflush();
	__pmFclose(f);
	setoserror(save_error);
	return NULL;
    }

    return f;
}

int
__pmLogWriteLabel(__pmFILE *f, const __pmLogLabel *lp)
{
    int		sts = 0;
    struct {				/* skeletal external record */
	int		header;
	__pmLogLabel	label;
	int		trailer;
    } out;

    out.header = out.trailer = htonl((int)sizeof(out));

    /* swab */
    out.label.ill_magic = htonl(lp->ill_magic);
    out.label.ill_pid = htonl(lp->ill_pid);
    out.label.ill_start.tv_sec = htonl(lp->ill_start.tv_sec);
    out.label.ill_start.tv_usec = htonl(lp->ill_start.tv_usec);
    out.label.ill_vol = htonl(lp->ill_vol);
    memmove((void *)out.label.ill_hostname, (void *)lp->ill_hostname, sizeof(lp->ill_hostname));
    memmove((void *)out.label.ill_tz, (void *)lp->ill_tz, sizeof(lp->ill_tz));

    if ((sts = __pmFwrite(&out, 1, sizeof(out), f)) != sizeof(out)) {
	char	errmsg[PM_MAXERRMSGLEN];
	pmprintf("__pmLogWriteLabel: write failed: returns %d expecting %d: %s\n",
	    sts, (int)sizeof(out), osstrerror_r(errmsg, sizeof(errmsg)));
	pmflush();
	sts = -oserror();
    }
    else
	sts = 0;

    return sts;
}

int
__pmLogCreate(const char *host, const char *base, int log_version,
	      __pmArchCtl *acp)
{
    __pmLogCtl	*lcp = acp->ac_log;
    int		save_error = 0;
    char	fname[MAXPATHLEN];

    lcp->l_minvol = lcp->l_maxvol = acp->ac_curvol = 0;
    lcp->l_hashpmid.nodes = lcp->l_hashpmid.hsize = 0;
    lcp->l_hashindom.nodes = lcp->l_hashindom.hsize = 0;
    lcp->l_trimindom.nodes = lcp->l_trimindom.hsize = 0;
    lcp->l_hashlabels.nodes = lcp->l_hashlabels.hsize = 0;
    lcp->l_hashtext.nodes = lcp->l_hashtext.hsize = 0;
    lcp->l_tifp = lcp->l_mdfp = acp->ac_mfp = NULL;

    if ((lcp->l_tifp = __pmLogNewFile(base, PM_LOG_VOL_TI)) != NULL) {
	if ((lcp->l_mdfp = __pmLogNewFile(base, PM_LOG_VOL_META)) != NULL) {
	    if ((acp->ac_mfp = __pmLogNewFile(base, 0)) != NULL) {
		char	tzbuf[PM_TZ_MAXLEN];
		char	*tz;
                int	sts;

		tz = __pmTimezone_r(tzbuf, sizeof(tzbuf));

		lcp->l_label.ill_magic = PM_LOG_MAGIC | log_version;
		/*
		 * Warning	ill_hostname may be truncated, but we
		 *		guarantee it will be null-byte terminated
		 */
		strncpy(lcp->l_label.ill_hostname, host, PM_LOG_MAXHOSTLEN-1);
		lcp->l_label.ill_hostname[PM_LOG_MAXHOSTLEN-1] = '\0';
		lcp->l_label.ill_pid = (int)getpid();
		/*
		 * hack - how do you get the TZ for a remote host?
		 */
		strcpy(lcp->l_label.ill_tz, tz ? tz : "");
		lcp->l_state = PM_LOG_STATE_NEW;

                /*
                 * __pmLogNewFile sets the IPC version to PDU_VERSION
                 * we want log_version instead
                 */
		sts = __pmSetVersionIPC(__pmFileno(lcp->l_tifp), log_version);
		if (sts < 0)
                    return sts;
		sts = __pmSetVersionIPC(__pmFileno(lcp->l_mdfp), log_version);
		if (sts < 0)
                    return sts;
		sts = __pmSetVersionIPC(__pmFileno(acp->ac_mfp), log_version);
		return sts;
	    }
	    else {
		save_error = oserror();
		unlink(__pmLogName_r(base, PM_LOG_VOL_TI, fname, sizeof(fname)));
		unlink(__pmLogName_r(base, PM_LOG_VOL_META, fname, sizeof(fname)));
		setoserror(save_error);
	    }
	}
	else {
	    save_error = oserror();
	    unlink(__pmLogName_r(base, PM_LOG_VOL_TI, fname, sizeof(fname)));
	    setoserror(save_error);
	}
    }

    lcp->l_tifp = lcp->l_mdfp = acp->ac_mfp = NULL;
    return oserror() ? -oserror() : -EPERM;
}

static void
logFreeHashPMID(__pmHashCtl *hcp)
{
    __pmHashNode	*hp;
    __pmHashNode	*prior_hp;
    int			i;

    for (i = 0; i < hcp->hsize; i++) {
	for (hp = hcp->hash[i], prior_hp = NULL; hp != NULL; hp = hp->next) {
	    if (hp->data != NULL)
		free(hp->data);
	    if (prior_hp != NULL)
		free(prior_hp);
	    prior_hp = hp;
	}
	if (prior_hp != NULL)
	    free(prior_hp);
    }
    free(hcp->hash);
}

static void
logFreeHashInDom(__pmHashCtl *hcp)
{
    __pmHashNode	*hp;
    __pmHashNode	*prior_hp;
    __pmLogInDom	*idp;
    __pmLogInDom	*prior_idp;
    int			i;

    for (i = 0; i < hcp->hsize; i++) {
	for (hp = hcp->hash[i], prior_hp = NULL; hp != NULL; hp = hp->next) {
	    for (idp = (__pmLogInDom *)hp->data, prior_idp = NULL;
		idp != NULL; idp = idp->next) {
		if (idp->buf != NULL)
		    free(idp->buf);
		if (idp->allinbuf == 0 && idp->namelist != NULL)
		    free(idp->namelist);
		if (prior_idp != NULL)
		    free(prior_idp);
		prior_idp = idp;
	    }
	    if (prior_idp != NULL)
		free(prior_idp);
	    if (prior_hp != NULL)
		free(prior_hp);
	    prior_hp = hp;
	}
	if (prior_hp != NULL)
	    free(prior_hp);
    }
    free(hcp->hash);
}

static void
logFreeTrimInDom(__pmHashCtl *hcp)
{
    __pmHashNode	*hp;
    __pmHashNode	*prior_hp;
    __pmHashCtl		*icp;
    __pmHashNode	*ip;
    __pmHashNode	*prior_ip;
    __pmLogTrimInDom	*indomp;
    int			h;
    int			i;

    /* loop over all indoms */
    for (h = 0; h < hcp->hsize; h++) {
	for (hp = hcp->hash[h], prior_hp = NULL; hp != NULL; hp = hp->next) {
	    indomp = (__pmLogTrimInDom *)hp->data;
	    icp = &indomp->hashinst;
	    /* loop over all instances for this indom */
	    for (i = 0; i < icp->hsize; i++) {
		for (ip = icp->hash[i], prior_ip = NULL; ip != NULL; ip = ip->next) {
		    free((__pmLogTrimInst *)ip->data);
		    if (prior_ip != NULL)
			free(prior_ip);
		    prior_ip = ip;
		}
		if (prior_ip != NULL)
		    free(prior_ip);
	    }
	    if (icp->hsize > 0)
		free(icp->hash);
	    free(indomp);
	    if (prior_hp != NULL)
		free(prior_hp);
	    prior_hp = hp;
	}
	if (prior_hp != NULL)
	    free(prior_hp);
    }
    free(hcp->hash);
}

static void
logFreeHashLabels(__pmHashCtl *type_ctl)
{
    __pmHashCtl		*ident_ctl;
    __pmHashNode 	*type_node;
    __pmHashNode 	*curr_type_node;
    __pmHashNode	*ident_node;
    __pmHashNode	*curr_ident_node;
    __pmLogLabelSet	*label;
    __pmLogLabelSet	*curr_label;
    pmLabelSet		*labelset;
    int			i;
    int			j;
    int			k;

    for (i = 0; i < type_ctl->hsize; i++) {
	for (type_node = type_ctl->hash[i]; type_node != NULL; ) {
	    ident_ctl = (__pmHashCtl *) type_node->data;

	    for (j = 0; j < ident_ctl->hsize; j++) {
		for (ident_node = ident_ctl->hash[j]; ident_node != NULL; ) {
		    for (label = (__pmLogLabelSet *)ident_node->data; label != NULL; ) {
			for (k = 0; k < label->nsets; k++) {
			    labelset = &label->labelsets[k];
			    free(labelset->json);
			    free(labelset->labels);
			}
			free(label->labelsets);
			curr_label = label;
			label = label->next;
			free(curr_label);
		    }
		    curr_ident_node = ident_node;
		    ident_node = ident_node->next;
		    free(curr_ident_node);
		}
	    }

	    curr_type_node = type_node;
	    type_node = type_node->next;
	    free(ident_ctl->hash);
	    free(ident_ctl);
	    free(curr_type_node);
	}
    }
    free(type_ctl->hash);
}

static void
logFreeHashText(__pmHashCtl *type_ctl)
{
    __pmHashCtl		*ident_ctl;
    __pmHashNode 	*type_node;
    __pmHashNode 	*curr_type_node;
    __pmHashNode	*ident_node;
    __pmHashNode	*curr_ident_node;
    char		*text;
    int			i;
    int			j;

    for (i = 0; i < type_ctl->hsize; i++) {
	for (type_node = type_ctl->hash[i]; type_node != NULL; ) {
	    ident_ctl = (__pmHashCtl *) type_node->data;

	    for (j = 0; j < ident_ctl->hsize; j++) {
		for (ident_node = ident_ctl->hash[j]; ident_node != NULL; ) {
		    text = (char *)ident_node->data;
		    curr_ident_node = ident_node;
		    ident_node = ident_node->next;
		    free(curr_ident_node);
		    free(text);
		}
	    }

	    curr_type_node = type_node;
	    type_node = type_node->next;
	    free(ident_ctl->hash);
	    free(ident_ctl);
	    free(curr_type_node);
	}
    }
    free(type_ctl->hash);
}

static void
logFreeMeta(__pmLogCtl *lcp)
{
    if (lcp->l_pmns != NULL) {
	__pmFreePMNS(lcp->l_pmns);
	lcp->l_pmns = NULL;
    }

    if (lcp->l_hashpmid.hsize != 0)
	logFreeHashPMID(&lcp->l_hashpmid);

    if (lcp->l_hashindom.hsize != 0)
	logFreeHashInDom(&lcp->l_hashindom);

    if (lcp->l_trimindom.hsize != 0)
	logFreeTrimInDom(&lcp->l_trimindom);

    if (lcp->l_hashlabels.hsize != 0)
	logFreeHashLabels(&lcp->l_hashlabels);

    if (lcp->l_hashtext.hsize != 0)
	logFreeHashText(&lcp->l_hashtext);
}

/*
 * Close the log files.
 * Free up the space used by __pmLogCtl.
 */

void
__pmLogClose(__pmArchCtl *acp)
{
    __pmLogCtl	*lcp = acp->ac_log;

    /*
     * We no longer free l_pmns here or clear l_hashpmid or l_hashindom here.
     * They may be needed by the next archive of a multi-archive context.
     * They are now now freed as needed using logFreeMeta().
     */
    if (lcp->l_tifp != NULL) {
	__pmResetIPC(__pmFileno(lcp->l_tifp));
	__pmFclose(lcp->l_tifp);
	lcp->l_tifp = NULL;
    }
    if (lcp->l_mdfp != NULL) {
	__pmResetIPC(__pmFileno(lcp->l_mdfp));
	__pmFclose(lcp->l_mdfp);
	lcp->l_mdfp = NULL;
    }
    if (acp->ac_mfp != NULL) {
	__pmResetIPC(__pmFileno(acp->ac_mfp));
	__pmFclose(acp->ac_mfp);
	acp->ac_mfp = NULL;
    }
    if (lcp->l_name != NULL) {
	free(lcp->l_name);
	lcp->l_name = NULL;
    }
    if (lcp->l_seen != NULL) {
	free(lcp->l_seen);
	lcp->l_seen = NULL;
	lcp->l_numseen = 0;
    }
    if (lcp->l_ti != NULL)
	free(lcp->l_ti);
}

int
__pmLogAddVolume(__pmArchCtl *acp, unsigned int vol)
{
    __pmLogCtl	*lcp = acp->ac_log;

    if (lcp->l_minvol == -1) {
	lcp->l_minvol = vol;
	lcp->l_maxvol = vol;
    } else if (vol < lcp->l_minvol) {
	lcp->l_minvol = vol;
    } else if (vol > lcp->l_maxvol) {
	lcp->l_maxvol = vol;
    }
    return 0;
}

int
__pmLogLoadLabel(__pmArchCtl *acp, const char *name)
{
    __pmLogCtl	*lcp = acp->ac_log;
    int		sts;
    int		blen;
    int		exists = 0;
    int		sep = pmPathSeparator();
    char	*base;
    char	*tbuf;
    char	*tp;
    char	*dir;
    DIR		*dirp = NULL;
    char	filename[MAXPATHLEN];
#if defined(HAVE_READDIR64)
    struct dirent64	*direntp;
#else
    struct dirent	*direntp;
#endif

    /*
     * find directory name component ... copy as dirname() may clobber
     * the string
     */
    if ((tbuf = strdup(name)) == NULL)
	return -oserror();
    PM_LOCK(__pmLock_extcall);
    dir = dirname(tbuf);		/* THREADSAFE */

    /*
     * Find file name component
     * basename(3) may modify the buffer passed to it. Use a copy.
     */
    strncpy(filename, name, MAXPATHLEN);
    filename[MAXPATHLEN-1] = '\0';
    if ((base = strdup(basename(filename))) == NULL) {		/* THREADSAFE */
	sts = -oserror();
	free(tbuf);
	PM_UNLOCK(__pmLock_extcall);
	return sts;
    }
    PM_UNLOCK(__pmLock_extcall);

    /*
     * See if the file exists, as named
     * __pmCompressedFileExists() may modify the buffer passed to it.
     * Use a copy.
     */
    strncpy(filename, name, MAXPATHLEN);
    filename[MAXPATHLEN-1] = '\0';
    if (access(name, R_OK) == 0 ||
	__pmCompressedFileIndex(filename, sizeof(filename)) >= 0) {
	/*
	 * The file exists as named, so it can't be the base name of the archive.
	 * Assume that it is the name of an actual file associated with the
	 * archive (i.e. .meta, .index or an actual volume) and try to
	 * strip the file name down to its base name.
	 */
	__pmLogBaseName(base);
    }

    pmsprintf(filename, sizeof(filename), "%s%c%s", dir, sep, base);
    if ((lcp->l_name = strdup(filename)) == NULL) {
	sts = -oserror();
	free(tbuf);
	free(base);
	return sts;
    }

    lcp->l_minvol = -1;
    lcp->l_tifp = lcp->l_mdfp = acp->ac_mfp = NULL;
    lcp->l_ti = NULL;
    lcp->l_numseen = 0; lcp->l_seen = NULL;

    blen = (int)strlen(base);
    /* dirp is an on-stack variable, so readdir*() is THREADSAFE */
    if ((dirp = opendir(dir)) != NULL) {
#if defined(HAVE_READDIR64)
	while ((direntp = readdir64(dirp)) != NULL)		/* THREADSAFE */
#else
	while ((direntp = readdir(dirp)) != NULL)		/* THREADSAFE */
#endif
	{
	    /*
	     * direntp->d_name is defined as an array by POSIX, so we
	     * can pass it to __pmLogBaseName, which will strip the
	     * suffix by modifying the data in place. The suffix can
	     * still be found after the base name.
	     */
	    pmsprintf(filename, sizeof(filename), "%s%c%s", dir, sep, direntp->d_name);
	    if (__pmLogBaseName(direntp->d_name) == NULL)
		continue; /* not an archive file */
	    if (strcmp(base, direntp->d_name) != 0)
		continue;
	    if (pmDebugOptions.log) {
		fprintf(stderr, "__pmLogOpen: inspect file \"%s\"\n", filename);
	    }
	    tp = &direntp->d_name[blen+1];
	    if (strcmp(tp, "index") == 0) {
		exists = 1;
		if ((lcp->l_tifp = __pmFopen(filename, "r")) == NULL) {
		    sts = -oserror();
		    goto cleanup;
		}
	    }
	    else if (strcmp(tp, "meta") == 0) {
		exists = 1;
		if ((lcp->l_mdfp = __pmFopen(filename, "r")) == NULL) {
		    sts = -oserror();
		    goto cleanup;
		}
	    }
	    else {
		char		*q;
		unsigned int	vol;

		vol = (unsigned int)strtoul(tp, &q, 10);
		if (*q == '\0') {
		    exists = 1;
		    if ((sts = __pmLogAddVolume(acp, vol)) < 0)
			goto cleanup;
		}
	    }
	}
	closedir(dirp);
	dirp = NULL;
    }
    else {
	sts = -oserror();
	if (pmDebugOptions.log) {
	    char	errmsg[PM_MAXERRMSGLEN];
	    fprintf(stderr, "__pmLogOpen: cannot scan directory \"%s\": %s\n", dir, pmErrStr_r(sts, errmsg, sizeof(errmsg)));
	}
	goto cleanup;
	
    }

    if (lcp->l_minvol == -1 || lcp->l_mdfp == NULL) {
	if (pmDebugOptions.log) {
	    if (lcp->l_minvol == -1)
		fprintf(stderr, "__pmLogOpen: Not found: data file \"%s.0\" (or similar)\n", base);
	    if (lcp->l_mdfp == NULL)
		fprintf(stderr, "__pmLogOpen: Not found: metadata file \"%s.meta\"\n", base);
	}
	if (exists)
	    sts = PM_ERR_LOGFILE;
	else
	    sts = -ENOENT;
	goto cleanup;
    }
    free(tbuf);
    free(base);
    return 0;

cleanup:
    if (dirp != NULL)
	closedir(dirp);
    __pmLogClose(acp);
    logFreeMeta(lcp);
    free(tbuf);
    free(base);
    return sts;
}

int
__pmLogOpen(const char *name, __pmContext *ctxp)
{
    __pmArchCtl	*acp = ctxp->c_archctl;
    __pmLogCtl	*lcp = ctxp->c_archctl->ac_log;
    __pmLogLabel label;
    int		version;
    int		sts;

    if ((sts = __pmLogLoadLabel(ctxp->c_archctl, name)) < 0)
	return sts;

    acp->ac_curvol = -1;
    if ((sts = __pmLogChangeVol(acp, lcp->l_minvol)) < 0)
	goto cleanup;
    else
	version = sts;

    ctxp->c_origin = lcp->l_label.ill_start;

    if (lcp->l_tifp) {
	sts = __pmLogChkLabel(acp, lcp->l_tifp, &label, PM_LOG_VOL_TI);
	if (sts < 0)
	    goto cleanup;
	else if (sts != version) {
	    /* mismatch between meta & actual data versions! */
	    sts = PM_ERR_LABEL;
	    goto cleanup;
	}

	if (lcp->l_label.ill_pid != label.ill_pid ||
		strcmp(lcp->l_label.ill_hostname, label.ill_hostname) != 0) {
	    sts = PM_ERR_LABEL;
	    goto cleanup;
	}
    }

    if ((sts = __pmLogChkLabel(acp, lcp->l_mdfp, &label, PM_LOG_VOL_META)) < 0)
	goto cleanup;
    else if (sts != version) {	/* version mismatch between meta & ti */
	sts = PM_ERR_LABEL;
	goto cleanup;
    }

    /*
     * Perform consistency checks between this label and the labels of other
     * archives possibly making up this context.
     */
    if ((sts = checkLabelConsistency(ctxp, &lcp->l_label)) < 0)
	goto cleanup;

    if ((sts = __pmLogLoadMeta(acp)) < 0)
	goto cleanup;

    if ((sts = __pmLogLoadIndex(lcp)) < 0)
	goto cleanup;

    if (lcp->l_label.ill_pid != label.ill_pid ||
	strcmp(lcp->l_label.ill_hostname, label.ill_hostname) != 0) {
	    sts = PM_ERR_LABEL;
	    goto cleanup;
    }

    PM_LOCK(lcp->l_lock);
    lcp->l_refcnt = 0;
    PM_UNLOCK(lcp->l_lock);
    lcp->l_physend = -1;

    ctxp->c_mode = (ctxp->c_mode & 0xffff0000) | PM_MODE_FORW;

    return 0;

cleanup:
    __pmLogClose(acp);
    logFreeMeta(lcp);
    return sts;
}

/*
 * This condition (ti_log == 0) been seen in QA where pmlogger churns
 * quickly ... trying to understand why using this diagnostic.
 */
static void
__pmLogIndexZeroTILogDiagnostic(const __pmArchCtl *acp)
{
    struct stat	sbuf;
    int		sts;

    fprintf(stderr, "%s: Botch: log offset == 0\n", "__pmLogPutIndex");
    fprintf(stderr, "  __pmFileno=%d __pmFtell -> %lld\n",
		    __pmFileno(acp->ac_mfp), (long long)__pmFtell(acp->ac_mfp));
    if ((sts = __pmFstat(acp->ac_mfp, &sbuf)) < 0)
	fprintf(stderr, "  __pmFstat failed -> %d\n", sts);
    else
	fprintf(stderr, "  __pmFstat st_size=%lld st_ino=%lld\n",
			(long long)sbuf.st_size, (long long)sbuf.st_ino);
}

#ifdef __PCP_EXPERIMENTAL_ARCHIVE_VERSION3
static int
__pmLogPutHighResIndex3(const __pmArchCtl *acp, const pmTimespec *ts)
{
    __pmLogCtl		*lcp = acp->ac_log;
    __pmLogTI3		ti3;
    size_t		bytes;

    ti3.ti_sec = (__int64_t)ts->tv_sec;
    ti3.ti_nsec = (__int32_t)ts->tv_nsec;
    ti3.ti_vol = acp->ac_curvol;
    ti3.ti_meta = (__pmoff64_t)__pmFtell(lcp->l_mdfp);
    ti3.ti_log = (__pmoff64_t)__pmFtell(acp->ac_mfp);

    if (ti3.ti_log == 0)
	__pmLogIndexZeroTILogDiagnostic(acp);

    if (pmDebugOptions.log)
	fprintf(stderr, "%s: "
		"timestamp=%lld.09%d vol=%d meta posn=%lld log posn=%lld\n",
	    "__pmLogPutIndex",
	    (long long)ti3.ti_sec, ti3.ti_nsec, ti3.ti_vol,
	    (long long)ti3.ti_meta, (long long)ti3.ti_log);

    __htonll((char *)&ti3.ti_sec);
    ti3.ti_nsec = htonl(ti3.ti_nsec);
    ti3.ti_vol = htonl(ti3.ti_vol);
    __htonll((char *)&ti3.ti_meta);
    __htonll((char *)&ti3.ti_log);

    bytes = __pmFwrite(&ti3, 1, sizeof(ti3), lcp->l_tifp);
    if (bytes != sizeof(ti3)) {
	char	errmsg[PM_MAXERRMSGLEN];
	pmNotifyErr(LOG_ERR, "%s: PCP archive temporal index write failed - "
			"got %zu expecting %zu: %s\n",
			"__pmLogPutIndex", bytes, sizeof(ti3),
			osstrerror_r(errmsg, sizeof(errmsg)));
	return -errno;
    }
    if (__pmFflush(lcp->l_tifp) != 0) {
	pmNotifyErr(LOG_ERR, "%s: PCP archive temporal index flush failed\n",
			"__pmLogPutIndex");
	return -errno;
    }
    return 0;
}
#endif

static int
__pmLogPutHighResIndex2(const __pmArchCtl *acp, const pmTimespec *ts)
{
    __pmLogCtl		*lcp = acp->ac_log;
    __pmLogTI2		ti2;
    size_t		bytes;

    ti2.ti_stamp.tv_sec = (__int32_t)ts->tv_sec;
    ti2.ti_stamp.tv_usec = (__int32_t)ts->tv_nsec / 1000;
    ti2.ti_vol = acp->ac_curvol;

    if (sizeof(off_t) > sizeof(__pmoff32_t)) {
	/* check for overflow of the offset ... */
	off_t	tmp;

	tmp = __pmFtell(lcp->l_mdfp);
	assert(tmp >= 0);
	ti2.ti_meta = (__pmoff32_t)tmp;
	if (tmp != ti2.ti_meta) {
	    pmNotifyErr(LOG_ERR, "%s: PCP archive file (%s) too big\n",
			"__pmLogPutIndex", "meta");
	    return -E2BIG;
	}
	tmp = __pmFtell(acp->ac_mfp);
	assert(tmp >= 0);
	ti2.ti_log = (__pmoff32_t)tmp;
	if (tmp != ti2.ti_log) {
	    pmNotifyErr(LOG_ERR, "%s: PCP archive file (%s) too big\n",
			"__pmLogPutIndex", "data");
	    return -E2BIG;
	}
    }
    else {
	ti2.ti_meta = (__pmoff32_t)__pmFtell(lcp->l_mdfp);
	ti2.ti_log = (__pmoff32_t)__pmFtell(acp->ac_mfp);
    }

    if (ti2.ti_log == 0)
	__pmLogIndexZeroTILogDiagnostic(acp);

    if (pmDebugOptions.log) {
	fprintf(stderr, "%s: timestamp=%d.06%d vol=%d meta posn=%ld log posn=%ld\n",
	    "__pmLogPutIndex",
	    (int)ti2.ti_stamp.tv_sec, (int)ti2.ti_stamp.tv_usec,
	    ti2.ti_vol, (long)ti2.ti_meta, (long)ti2.ti_log);
    }

    ti2.ti_stamp.tv_sec = htonl(ti2.ti_stamp.tv_sec);
    ti2.ti_stamp.tv_usec = htonl(ti2.ti_stamp.tv_usec);
    ti2.ti_vol = htonl(ti2.ti_vol);
    ti2.ti_meta = htonl(ti2.ti_meta);
    ti2.ti_log = htonl(ti2.ti_log);

    bytes = __pmFwrite(&ti2, 1, sizeof(ti2), lcp->l_tifp);
    if (bytes != sizeof(ti2)) {
	char	errmsg[PM_MAXERRMSGLEN];
	pmNotifyErr(LOG_ERR, "%s: PCP archive temporal index write failed - "
			"got %zu expecting %zu: %s\n",
			"__pmLogPutIndex", bytes, sizeof(ti2),
			osstrerror_r(errmsg, sizeof(errmsg)));
	return -errno;
    }
    if (__pmFflush(lcp->l_tifp) != 0) {
	pmNotifyErr(LOG_ERR, "%s: PCP archive temporal index flush failed\n",
			"__pmLogPutIndex");
	return -errno;
    }
    return 0;
}

int
__pmLogPutHighResIndex(const __pmArchCtl *acp, pmTimespec *ts)
{
    struct timespec	tmp;
    pmTimespec		stamp;
    __pmLogCtl		*lcp = acp->ac_log;

    if (lcp->l_tifp == NULL || lcp->l_mdfp == NULL || acp->ac_mfp == NULL) {
	/*
	 * archive not really created (failed in __pmLogCreate) ...
	 * nothing to be done
	 */
	return 0;
    }

    __pmFflush(lcp->l_mdfp);
    __pmFflush(acp->ac_mfp);

    if (ts == NULL) {
	pmtimespecNow(&tmp);
	stamp.tv_sec = tmp.tv_sec;
	stamp.tv_nsec = tmp.tv_nsec;
	ts = &stamp;
    }

#ifdef __PCP_EXPERIMENTAL_ARCHIVE_VERSION3
    if (__pmLogVersion(lcp) >= PM_LOG_VERS03)
	return __pmLogPutHighResIndex3(acp, ts);
#endif
    return __pmLogPutHighResIndex2(acp, ts);
}

void
__pmLogPutIndex(const __pmArchCtl *acp, const pmTimeval *tp)
{
    if (tp == NULL) {
	__pmLogPutHighResIndex(acp, NULL);
    } else {
	pmTimespec ts;
	ts.tv_sec = tp->tv_sec;
	ts.tv_nsec = tp->tv_usec * 1000;
	__pmLogPutHighResIndex(acp, &ts);
    }
}

static int
logputresult(int version,__pmArchCtl *acp, __pmPDU *pb)
{
    /*
     * This is a bit tricky ...
     *
     *  Input
     *  :---------:----------:----------:---------------- .........:---------:
     *  | int len | int type | int from | timestamp, .... pmResult | unused  |
     *  :---------:----------:----------:---------------- .........:---------:
     *  ^
     *  |
     *  pb
     *
     *  Output
     *  :---------:----------:----------:---------------- .........:---------:
     *  | unused  | unused   | int len  | timestamp, .... pmResult | int len |
     *  :---------:----------:----------:---------------- .........:---------:
     *                       ^
     *                       |
     *                       start
     *
     * If version == 1, pb[] does not have room for trailer len.
     * If version == 2, pb[] does have room for trailer len.
     */
    __pmLogCtl		*lcp = acp->ac_log;
    int			sz;
    int			sts = 0;
    int			save_from;
    __pmPDU		*start = &pb[2];

    if (lcp->l_state == PM_LOG_STATE_NEW) {
	int		i;
	pmTimeval	*tvp;
	/*
	 * first result, do the label record
	 */
	i = sizeof(__pmPDUHdr) / sizeof(__pmPDU);
	tvp = (pmTimeval *)&pb[i];
	lcp->l_label.ill_start.tv_sec = ntohl(tvp->tv_sec);
	lcp->l_label.ill_start.tv_usec = ntohl(tvp->tv_usec);
	lcp->l_label.ill_vol = PM_LOG_VOL_TI;
	__pmLogWriteLabel(lcp->l_tifp, &lcp->l_label);
	lcp->l_label.ill_vol = PM_LOG_VOL_META;
	__pmLogWriteLabel(lcp->l_mdfp, &lcp->l_label);
	lcp->l_label.ill_vol = 0;
	__pmLogWriteLabel(acp->ac_mfp, &lcp->l_label);
	lcp->l_state = PM_LOG_STATE_INIT;
    }

    sz = pb[0] - (int)sizeof(__pmPDUHdr) + 2 * (int)sizeof(int);

    if (pmDebugOptions.log) {
	fprintf(stderr, "logputresult: pdubuf=" PRINTF_P_PFX "%p input len=%d output len=%d posn=%ld\n", pb, pb[0], sz, (long)__pmFtell(acp->ac_mfp));
    }

    save_from = start[0];
    start[0] = htonl(sz);	/* swab */

    if (version == 1) {
	if ((sts = __pmFwrite(start, 1, sz-sizeof(int), acp->ac_mfp)) != sz-sizeof(int)) {
	    char	errmsg[PM_MAXERRMSGLEN];
	    pmprintf("__pmLogPutResult: write failed: returns %d expecting %d: %s\n",
		sts, (int)(sz-sizeof(int)), osstrerror_r(errmsg, sizeof(errmsg)));
	    pmflush();
	    sts = -oserror();
	}
	else {
	    if ((sts = __pmFwrite(start, 1, sizeof(int), acp->ac_mfp)) != sizeof(int)) {
		char	errmsg[PM_MAXERRMSGLEN];
		pmprintf("__pmLogPutResult: trailer write failed: returns %d expecting %d: %s\n",
		    sts, (int)sizeof(int), osstrerror_r(errmsg, sizeof(errmsg)));
		pmflush();
		sts = -oserror();
	    }
	}
    }
    else {
	/* assume version == 2 */
	start[(sz-1)/sizeof(__pmPDU)] = start[0];
	if ((sts = __pmFwrite(start, 1, sz, acp->ac_mfp)) != sz) {
	    char	errmsg[PM_MAXERRMSGLEN];
	    pmprintf("__pmLogPutResult2: write failed: returns %d expecting %d: %s\n",
	    	sts, sz, osstrerror_r(errmsg, sizeof(errmsg)));
	    pmflush();
	    sts = -oserror();
	}
    }

    /* restore and unswab */
    start[0] = save_from;

    return sts;
}

/*
 * original routine, pb[] does not have room for trailer, so 2 writes
 * needed
 */
int
__pmLogPutResult(__pmArchCtl *acp, __pmPDU *pb)
{
    return logputresult(1, acp, pb);
}

/*
 * new routine, pb[] does have room for trailer, so only 1 write
 * needed
 */
int
__pmLogPutResult2(__pmArchCtl *acp, __pmPDU *pb)
{
    return logputresult(2, acp, pb);
}

/*
 * check if PDU buffer seems even half-way reasonable ...
 * only used when trying to locate end of archive.
 * return 0 for OK, -1 for bad.
 */
static int
paranoidCheck(int len, __pmPDU *pb)
{
    int			numpmid;
    size_t		hdrsz;		/* bytes for the PDU head+tail */
    int			i;
    int			j;
    int			vsize;		/* size of vlist_t's in PDU buffer */
    int			vbsize;		/* size of pmValueBlocks */
    int			numval;		/* number of values */
    int			valfmt;

    struct result_t {			/* from p_result.c */
	__pmPDUHdr		hdr;
	pmTimeval		timestamp;	/* when returned */
	int			numpmid;	/* no. of PMIDs to follow */
	__pmPDU			data[1];	/* zero or more */
    }			*pp;
    struct vlist_t {			/* from p_result.c */
	pmID			pmid;
	int			numval;		/* no. of vlist els to follow, or error */
	int			valfmt;		/* insitu or pointer */
	__pmValue_PDU		vlist[1];	/* zero or more */
    }			*vlp;

    /*
     * to start with, need space for result_t with no data (__pmPDU)
     * ... this is the external size, which consists of
     * <header len>
     * <timestamp> (2 words)
     * <numpmid>
     * <trailer len>
     *
     * it is confusing because *pb and result_t include the fake
     * __pmPDUHdr which is not really in the external file
     */
    hdrsz = 5 * sizeof(__pmPDU);

    if (len < hdrsz) {
	if (pmDebugOptions.log) {
	    fprintf(stderr, "\nparanoidCheck: len=%d, min len=%d\n",
		len, (int)hdrsz);
	    dumpbuf(len, &pb[3]); /* skip first 3 words, start @ timestamp */
	}
	return -1;
    }

    pp = (struct result_t *)pb;
    numpmid = ntohl(pp->numpmid);

    /*
     * This is a re-implementation of much of __pmDecodeResult()
     */

    if (numpmid < 1) {
	if (len != hdrsz) {
	    if (pmDebugOptions.log) {
		fprintf(stderr, "\nparanoidCheck: numpmid=%d len=%d, expected len=%d\n",
		    numpmid, len, (int)hdrsz);
		dumpbuf(len, &pb[3]); /* skip first 3 words, start @ timestamp */
	    }
	    return -1;
	}
    }

    /*
     * Calculate vsize and vbsize from the original PDU buffer ...
     * :---------:-----------:----------------:--------------------:
     * : numpmid : timestamp : ... vlists ... : .. pmValueBocks .. :
     * :---------:-----------:----------------:--------------------:
     *                        <---  vsize ---> <---   vbsize   --->
     *                              bytes             bytes
     */

    vsize = vbsize = 0;
    for (i = 0; i < numpmid; i++) {
	vlp = (struct vlist_t *)&pp->data[vsize/sizeof(__pmPDU)];
	vsize += sizeof(vlp->pmid) + sizeof(vlp->numval);
	if (len < hdrsz + vsize + vbsize) {
	    if (pmDebugOptions.log) {
		fprintf(stderr, "\nparanoidCheck: vset[%d] len=%d, need len>=%d (%d+%d+%d)\n",
		    i, len, (int)(hdrsz + vsize + vbsize), (int)hdrsz, vsize, vbsize);
		dumpbuf(len, &pb[3]); /* skip first 3 words, start @ timestamp */
	    }
	    return -1;
	}
	numval = ntohl(vlp->numval);
	if (numval > 0) {
#ifdef DESPERATE
	    pmID		pmid;
#endif
	    valfmt = ntohl(vlp->valfmt);
	    if (valfmt != PM_VAL_INSITU &&
		valfmt != PM_VAL_DPTR &&
		valfmt != PM_VAL_SPTR) {
		if (pmDebugOptions.log) {
		    fprintf(stderr, "\nparanoidCheck: vset[%d] bad valfmt=%d\n",
			i, valfmt);
		    dumpbuf(len, &pb[3]); /* skip first 3 words, start @ timestamp */
		}
		return -1;
	    }
#ifdef DESPERATE
	    {
		char	strbuf[20];
		if (i == 0) fputc('\n', stderr);
		pmid = __ntohpmID(vlp->pmid);
		fprintf(stderr, "vlist[%d] pmid: %s numval: %d valfmt: %d\n",
		    i, pmIDStr_r(pmid, strbuf, sizeof(strbuf)), numval, valfmt);
	    }
#endif
	    vsize += sizeof(vlp->valfmt) + numval * sizeof(__pmValue_PDU);
	    if (valfmt != PM_VAL_INSITU) {
		for (j = 0; j < numval; j++) {
		    int			index = (int)ntohl((long)vlp->vlist[j].value.lval);
		    pmValueBlock	*pduvbp;
		    int			vlen;
		    
		    if (index < 0 || index * sizeof(__pmPDU) > len) {
			if (pmDebugOptions.log) {
			    fprintf(stderr, "\nparanoidCheck: vset[%d] val[%d], bad pval index=%d not in range 0..%d\n",
				i, j, index, (int)(len / sizeof(__pmPDU)));
			    dumpbuf(len, &pb[3]); /* skip first 3 words, start @ timestamp */
			}
			return -1;
		    }
		    pduvbp = (pmValueBlock *)&pb[index];
		    __ntohpmValueBlock(pduvbp);
		    vlen = pduvbp->vlen;
		    __htonpmValueBlock(pduvbp);		/* restore pdubuf! */
		    if (vlen < sizeof(__pmPDU)) {
			if (pmDebugOptions.log) {
			    fprintf(stderr, "\nparanoidCheck: vset[%d] val[%d], bad vlen=%d\n",
				i, j, vlen);
			    dumpbuf(len, &pb[3]); /* skip first 3 words, start @ timestamp */
			}
			return -1;
		    }
		    vbsize += PM_PDU_SIZE_BYTES(vlen);
		}
	    }
	}
    }

    return 0;
}

static int
paranoidLogRead(__pmContext *ctxp, int mode, __pmFILE *peekf, pmResult **result)
{
    return __pmLogRead_ctx(ctxp, mode, peekf, result, PMLOGREAD_TO_EOF);
}

/*
 * We've reached the beginning or the end of an archive. If we will
 * be switching to another archive, then generate a MARK record to represent
 * the gap in recording between the archives.
 *
 * Internal variant of __pmLogGenerateMark() ... using a
 * __pmContext * instead of a __pmLogCtl * as the first argument
 * so that the current context can be carried down the call stack.
 */

int
__pmLogGenerateMark_ctx(__pmContext *ctxp, int mode, pmResult **result)
{
    __pmLogCtl		*lcp = ctxp->c_archctl->ac_log;
    pmResult		*pr;
    int			sts;
    struct timeval	end;

    PM_ASSERT_IS_LOCKED(ctxp->c_lock);

    if ((pr = (pmResult *)malloc(sizeof(pmResult))) == NULL)
	pmNoMem("generateMark", sizeof(pmResult), PM_FATAL_ERR);

    /*
     * A mark record has numpmid == 0 and the timestamp set to one millisecond
     * after the end, or before the beginning of the archive.
     */
    pr->numpmid = 0;
    if (mode == PM_MODE_FORW) {
	if ((sts = __pmGetArchiveEnd_ctx(ctxp, &end)) < 0) {
	    free(pr);
	    return sts;
	}
	pr->timestamp.tv_sec = lcp->l_endtime.tv_sec;
	pr->timestamp.tv_usec = lcp->l_endtime.tv_usec;
	pr->timestamp.tv_usec += 1000;
	if (pr->timestamp.tv_usec > 1000000) {
	    pr->timestamp.tv_usec -= 1000000;
	    pr->timestamp.tv_sec++;
	}
    }
    else {
	pr->timestamp.tv_sec = lcp->l_label.ill_start.tv_sec;
	pr->timestamp.tv_usec = lcp->l_label.ill_start.tv_usec;
	if (pr->timestamp.tv_usec >= 1000)
	    pr->timestamp.tv_usec -= 1000;
	else {
	    pr->timestamp.tv_usec = 1000000 - 1000 + pr->timestamp.tv_usec;;
	    pr->timestamp.tv_sec--;
	}
    }
    *result = pr;
    return 0;
}

int
__pmLogGenerateMark(__pmLogCtl *lcp, int mode, pmResult **result)
{
    int		sts;
    __pmContext	*ctxp;

    if ((sts = pmWhichContext()) < 0)
	return sts;
    ctxp = __pmHandleToPtr(sts);
    if (ctxp == NULL)
	return PM_ERR_NOCONTEXT;
    if (ctxp->c_type != PM_CONTEXT_ARCHIVE) {
	PM_UNLOCK(ctxp->c_lock);
	return PM_ERR_NOTARCHIVE;
    }
    sts = __pmLogGenerateMark_ctx(ctxp, mode, result);
    PM_UNLOCK(ctxp->c_lock);
    return sts;
}

static void
clearMarkDone(__pmContext *ctxp)
{
    /* The current context should be an archive context. */
    if (ctxp != NULL) {
	if (ctxp->c_type == PM_CONTEXT_ARCHIVE)
	    ctxp->c_archctl->ac_mark_done = 0;
    }
}

/*
 * read next forward or backward from the log
 *
 * by default (peekf == NULL) use acp->ac_mfp and roll volume or archive
 * at end of file if another volume or archive is available
 *
 * if peekf != NULL, use this stream, and do not roll volume or archive
 *
 * Internal variant of __pmLogRead() ... using a __pmContext * instead
 * of a __pmLogCtl * as the first argument so that the current context
 * can be carried down the call stack.
 */
int
__pmLogRead_ctx(__pmContext *ctxp, int mode, __pmFILE *peekf, pmResult **result, int option)
{
    __pmLogCtl	*lcp;
    __pmArchCtl	*acp;
    int		head;
    int		rlen;
    int		trail;
    int		sts;
    long	offset;
    __pmPDU	*pb;
    __pmFILE	*f;
    int		n;
    ctx_ctl_t	ctx_ctl = { NULL, 0 };

    sts = lock_ctx(ctxp, &ctx_ctl);
    if (sts < 0)
	goto func_return;

    lcp = ctxp->c_archctl->ac_log;
    acp = ctxp->c_archctl;

    /*
     * Strip any XTB data from mode, its not used here
     */
    mode &= __PM_MODE_MASK;

    if (peekf != NULL)
	f = peekf;
    else
	f = acp->ac_mfp;

    offset = __pmFtell(f);
    assert(offset >= 0);
    if (pmDebugOptions.log) {
	fprintf(stderr, "__pmLogRead: fd=%d%s mode=%s vol=%d posn=%ld ",
	    __pmFileno(f), peekf == NULL ? "" : " (peek)",
	    mode == PM_MODE_FORW ? "forw" : "back",
	    acp->ac_curvol, (long)offset);
    }

    if (mode == PM_MODE_BACK) {
       for ( ; ; ) {
	   if (offset <= sizeof(__pmLogLabel) + 2 * sizeof(int)) {
		if (pmDebugOptions.log)
		    fprintf(stderr, "BEFORE start\n");
		sts = PM_ERR_EOL;
		if (peekf == NULL) {
		    int		vol = acp->ac_curvol-1;
		    while (vol >= lcp->l_minvol) {
			if (__pmLogChangeVol(acp, vol) >= 0) {
			    f = acp->ac_mfp;
			    __pmFseek(f, 0L, SEEK_END);
			    offset = __pmFtell(f);
			    assert(offset >= 0);
			    if (pmDebugOptions.log) {
				fprintf(stderr, "vol=%d posn=%ld ",
				    acp->ac_curvol, (long)offset);
			    }
			    break;
			}
			vol--;
		    }
		    if (vol >= lcp->l_minvol)
			continue; /* Try this volume */

		    /*
		     * No more volumes. See if there is a previous archive to
		     * switch to.
		     */
		    sts = LogCheckForNextArchive(ctxp, PM_MODE_BACK, result);
		    if (sts == 0) {
			/* There is a next archive to change to. */
			if (*result != NULL) {
			    /* A mark record was generated */
			    sts = 0;
			    goto func_return;
			}

			/*
			 * Mark was previously generated. Try the previous
			 * archive, if any.
			 */
			if ((sts = LogChangeToPreviousArchive(ctxp)) == 0) {
			    lcp = ctxp->c_archctl->ac_log;
			    f = acp->ac_mfp;
			    offset = __pmFtell(f);
			    assert(offset >= 0);
			    if (pmDebugOptions.log) {
				fprintf(stderr, "arch=%s vol=%d posn=%ld ",
					lcp->l_name, acp->ac_curvol, (long)offset);
			    }
			    continue; /* Try this archive */
			}
			/* No more archives */
		    }
		}
		goto func_return;
	   }
	   else {
	       __pmFseek(f, -(long)sizeof(head), SEEK_CUR);
	       break;
	   }
	}
    }

again:
    n = (int)__pmFread(&head, 1, sizeof(head), f);
    head = ntohl(head); /* swab head */
    if (n != sizeof(head)) {
	if (__pmFeof(f)) {
	    /* no more data ... looks like End of Archive volume */
	    __pmClearerr(f);
	    if (pmDebugOptions.log)
		fprintf(stderr, "AFTER end\n");
	    __pmFseek(f, offset, SEEK_SET);
	    sts = PM_ERR_EOL;
	    if (peekf == NULL) {
		/* Try the next volume. */
		int	vol = acp->ac_curvol+1;
		while (vol <= lcp->l_maxvol) {
		    if (__pmLogChangeVol(acp, vol) >= 0) {
			f = acp->ac_mfp;
			goto again;
		    }
		    vol++;
		}
		/*
		 * No more volumes. See if there is another archive to switch
		 * to.
		 */
		sts = LogCheckForNextArchive(ctxp, PM_MODE_FORW, result);
		if (sts == 0) {
		    /* There is a next archive to change to. */
		    if (*result != NULL) {
			/* A mark record was generated */
			sts = 0;
			goto func_return;
		    }

		    /* Mark was previously generated. Try the next archive. */
		    if ((sts = LogChangeToNextArchive(ctxp)) == 0) {
			lcp = ctxp->c_archctl->ac_log;
			f = acp->ac_mfp;
			offset = __pmFtell(f);
			assert(offset >= 0);
			if (pmDebugOptions.log) {
			    fprintf(stderr, "arch=%s vol=%d posn=%ld ",
				    lcp->l_name, acp->ac_curvol, (long)offset);
			}
			goto again;
		    }
		}
	    }
	    goto func_return;
	}

	if (pmDebugOptions.log)
	    fprintf(stderr, "\nError: header fread got %d expected %d\n", n, (int)sizeof(head));
	if (__pmFerror(f)) {
	    /* I/O error */
	    __pmClearerr(f);
	    sts = -oserror();
	    goto func_return;
	}
	else {
	    /* corrupted archive */
	    sts = PM_ERR_LOGREC;
	    goto func_return;
	}
    }

    /*
     * If we're here, then we're not at a multi-archive boundary. Clearing the
     * ac_mark_done flag here automatically handles changes in direction which
     * happen right at the boundary.
     */
    clearMarkDone(ctxp);

    /*
     * This is pretty ugly (forward case shown backwards is similar) ...
     *
     *  Input
     *                         head    <--- rlen bytes --------->   tail
     *  :---------:---------:---------:--------------------------:---------:
     *  |   ???   |   ???   | int len | timestamp, .... pmResult | int len |
     *  :---------:---------:---------:--------------------------:---------:
     *  ^                             ^
     *  |                             |
     *  pb                            read into here
     *
     *  Decode
     *  <----  __pmPDUHdr  ----------->
     *  :---------:---------:---------:--------------------------:---------:
     *  | length  | pdutype |  anon   | timestamp, .... pmResult | int len |
     *  :---------:---------:---------:--------------------------:---------:
     *  ^
     *  |
     *  pb
     *
     * Note: cannot volume switch in the middle of a log record
     */

    rlen = head - 2 * (int)sizeof(head);
    if (rlen < 0 || (mode == PM_MODE_BACK && rlen > offset)) {
	/*
	 * corrupted! usually means a truncated log ...
	 */
	if (pmDebugOptions.log)
	    fprintf(stderr, "\nError: truncated log? rlen=%d (offset %d)\n",
		rlen, (int)offset);
	sts = PM_ERR_LOGREC;
	goto func_return;
    }
    /*
     * need to add int at end for trailer in case buffer is used
     * subsequently by __pmLogPutResult2()
     */
    if ((pb = __pmFindPDUBuf(rlen + (int)sizeof(__pmPDUHdr) + (int)sizeof(int))) == NULL) {
	if (pmDebugOptions.log) {
	    char	errmsg[PM_MAXERRMSGLEN];
	    fprintf(stderr, "\nError: __pmFindPDUBuf(%d) %s\n",
		(int)(rlen + sizeof(__pmPDUHdr)),
		osstrerror_r(errmsg, sizeof(errmsg)));
	}
	__pmFseek(f, offset, SEEK_SET);
	sts = -oserror();
	goto func_return;
    }

    if (mode == PM_MODE_BACK)
	__pmFseek(f, -(long)(sizeof(head) + rlen), SEEK_CUR);

    if ((n = (int)__pmFread(&pb[3], 1, rlen, f)) != rlen) {
	/* data read failed */
	__pmUnpinPDUBuf(pb);
	if (pmDebugOptions.log)
	    fprintf(stderr, "\nError: data fread got %d expected %d\n", n, rlen);
	__pmFseek(f, offset, SEEK_SET);
	if (__pmFerror(f)) {
	    /* I/O error */
	    __pmClearerr(f);
	    sts = -oserror();
	    goto func_return;
	}
	__pmClearerr(f);

	/* corrupted archive */
	sts = PM_ERR_LOGREC;
	goto func_return;
    }
    else {
	__pmPDUHdr *header = (__pmPDUHdr *)pb;
	header->len = sizeof(*header) + rlen;
	header->type = PDU_RESULT;
	header->from = FROM_ANON;
	/* swab PDU buffer - done later in __pmDecodeResult */

	if (pmDebugOptions.pdu && pmDebugOptions.desperate) {
	    int	j;
	    char	*p;
	    int	jend = PM_PDU_SIZE(header->len);

	    /* clear the padding bytes, lest they contain garbage */
	    p = (char *)pb + header->len;
	    while (p < (char *)pb + jend*sizeof(__pmPDU))
		*p++ = '~';	/* buffer end */

	    fprintf(stderr, "__pmLogRead: PDU buffer\n");
	    for (j = 0; j < jend; j++) {
		if ((j % 8) == 0 && j > 0)
		    fprintf(stderr, "\n%03d: ", j);
		fprintf(stderr, "%8x ", pb[j]);
	    }
	    putc('\n', stderr);
	}
    }

    if (mode == PM_MODE_BACK)
	__pmFseek(f, -(long)(rlen + sizeof(head)), SEEK_CUR);

    if ((n = (int)__pmFread(&trail, 1, sizeof(trail), f)) != sizeof(trail)) {
	__pmUnpinPDUBuf(pb);
	if (pmDebugOptions.log)
	    fprintf(stderr, "\nError: trailer fread got %d expected %d\n", n, (int)sizeof(trail));
	__pmFseek(f, offset, SEEK_SET);
	if (__pmFerror(f)) {
	    /* I/O error */
	    __pmClearerr(f);
	    sts = -oserror();
	    goto func_return;
	}
	__pmClearerr(f);

	/* corrupted archive */
	sts = PM_ERR_LOGREC;
	goto func_return;
    }
    else {
	/* swab trail */
	trail = ntohl(trail);
    }

    if (trail != head) {
	if (pmDebugOptions.log)
	    fprintf(stderr, "\nError: record length mismatch: header (%d) != trailer (%d)\n", head, trail);
	__pmUnpinPDUBuf(pb);
	sts = PM_ERR_LOGREC;
	goto func_return;
    }

    if (option == PMLOGREAD_TO_EOF && paranoidCheck(head, pb) == -1) {
	__pmUnpinPDUBuf(pb);
	sts = PM_ERR_LOGREC;
	goto func_return;
    }

    if (mode == PM_MODE_BACK)
	__pmFseek(f, -(long)sizeof(trail), SEEK_CUR);

    __pmOverrideLastFd(__pmFileno(f));
    sts = __pmDecodeResult_ctx(ctxp, pb, result); /* also swabs the result */

    if (pmDebugOptions.log) {
	head -= sizeof(head) + sizeof(trail);
	if (sts >= 0) {
	    pmTimeval	tmp;
	    fprintf(stderr, "@");
	    pmPrintStamp(stderr, &(*result)->timestamp);
	    tmp.tv_sec = (__int32_t)(*result)->timestamp.tv_sec;
	    tmp.tv_usec = (__int32_t)(*result)->timestamp.tv_usec;
	    fprintf(stderr, " (t=%.6f)", __pmTimevalSub(&tmp, &lcp->l_label.ill_start));
	}
	else {
	    char	errmsg[PM_MAXERRMSGLEN];
	    fprintf(stderr, "__pmLogRead: __pmDecodeResult failed: %s\n", pmErrStr_r(sts, errmsg, sizeof(errmsg)));
	    fprintf(stderr, "@unknown time");
	}
	fprintf(stderr, " len=header+%d+trailer\n", head);
    }

    /* exported to indicate how efficient we are ... */
    __pmLogReads++;

    if (sts < 0) {
	__pmUnpinPDUBuf(pb);
	sts = PM_ERR_LOGREC;
	goto func_return;
    }

    if (pmDebugOptions.pdu) {
	fprintf(stderr, "__pmLogRead timestamp=");
	pmPrintStamp(stderr, &(*result)->timestamp);
	fprintf(stderr, " " PRINTF_P_PFX "%p ... " PRINTF_P_PFX "%p", &pb[3], &pb[head/sizeof(__pmPDU)+3]);
	fputc('\n', stderr);
	dumpbuf(rlen, &pb[3]);		/* see above to explain "3" */
    }

    __pmUnpinPDUBuf(pb);
    sts = 0;

func_return:

    if (ctx_ctl.need_ctx_unlock)
	PM_UNLOCK(ctx_ctl.ctxp->c_lock);

    return sts;
}

int
__pmLogRead(__pmArchCtl *acp, int mode, __pmFILE *peekf, pmResult **result, int option)
{
    int		sts;
    __pmContext	*ctxp;

    if ((sts = pmWhichContext()) < 0)
	return sts;
    ctxp = __pmHandleToPtr(sts);
    if (ctxp == NULL)
	return PM_ERR_NOCONTEXT;
    if (ctxp->c_type != PM_CONTEXT_ARCHIVE) {
	PM_UNLOCK(ctxp->c_lock);
	return PM_ERR_NOTARCHIVE;
    }
    sts = __pmLogRead_ctx(ctxp, mode, peekf, result, option);
    PM_UNLOCK(ctxp->c_lock);
    return sts;
}

static int
check_all_derived(int numpmid, pmID pmidlist[])
{
    int	i;

    /*
     * Special case ... if we ONLY have derived metrics in the input
     * pmidlist then all the derived metrics must be constant
     * expressions, so skip all the processing.
     * This rare, but avoids reading to the end of an archive
     * for no good reason.
     */

    for (i = 0; i < numpmid; i++) {
	if (!IS_DERIVED(pmidlist[i]))
	    return 0;
    }
    return 1;
}

int
__pmLogFetch(__pmContext *ctxp, int numpmid, pmID pmidlist[], pmResult **result)
{
    int		i;
    int		j;
    int		u;
    int		all_derived;
    int		sts = 0;
    int		found;
    double	tdiff;
    pmResult	*newres;
    pmDesc	desc;
    int		kval;
    __pmHashNode	*hp;
    pmid_ctl	*pcp;
    int		nskip;
    pmTimeval	tmp;
    int		ctxp_mode;
    ctx_ctl_t	ctx_ctl = { NULL, 0 };

    sts = lock_ctx(ctxp, &ctx_ctl);
    if (sts < 0)
	goto func_return;

    ctxp_mode = ctxp->c_mode & __PM_MODE_MASK;

    if (ctxp_mode == PM_MODE_INTERP) {
	sts = __pmLogFetchInterp(ctxp, numpmid, pmidlist, result);
	goto func_return;
    }

    all_derived = check_all_derived(numpmid, pmidlist);

    /* re-establish position */
    sts = __pmLogChangeVol(ctxp->c_archctl, ctxp->c_archctl->ac_vol);
    if (sts < 0)
	goto func_return;
    __pmFseek(ctxp->c_archctl->ac_mfp, 
	    (long)ctxp->c_archctl->ac_offset, SEEK_SET);

more:

    found = 0;
    nskip = 0;
    *result = NULL;
    while (!found) {
	if (ctxp->c_archctl->ac_serial == 0) {
	    /*
	     * no serial access, so need to make sure we are
	     * starting in the correct place
	     */
	    sts = __pmLogSetTime(ctxp);
	    if (sts < 0)
		goto func_return;
	    ctxp->c_archctl->ac_offset = __pmFtell(ctxp->c_archctl->ac_mfp);
	    ctxp->c_archctl->ac_vol = ctxp->c_archctl->ac_curvol;
	    /*
	     * we're in the approximate place (thanks to the temporal
	     * index, now fine-tuning by backing up (opposite direction
	     * to desired direction) until we're in the right place
	     */
	    nskip = 0;
	    while (__pmLogRead_ctx(ctxp, ctxp_mode == PM_MODE_FORW ? PM_MODE_BACK : PM_MODE_FORW, NULL, result, PMLOGREAD_NEXT) >= 0) {
		nskip++;
		tmp.tv_sec = (__int32_t)(*result)->timestamp.tv_sec;
		tmp.tv_usec = (__int32_t)(*result)->timestamp.tv_usec;
		tdiff = __pmTimevalSub(&tmp, &ctxp->c_origin);
		if (ctxp_mode == PM_MODE_FORW && tdiff < 0) {
		    /* too far ... next one forward is the one we need */
		    pmFreeResult(*result);
		    *result = NULL;
		    break;
		}
		else if (ctxp_mode == PM_MODE_BACK && tdiff > 0) {
		    /* too far ... next one back is the one we need */
		    pmFreeResult(*result);
		    *result = NULL;
		    break;
		}
		else if (tdiff == 0) {
		    /*
		     * exactly the one we wanted, but we're going in the
		     * wrong direction, so we need to read this one again
		     * in the right direction to avoid reading it twice
		     * (once above and once the next time through here)
		     */
		    pmFreeResult(*result);
		    *result = NULL;
		    break;
		}
		ctxp->c_archctl->ac_offset = __pmFtell(ctxp->c_archctl->ac_mfp);
		ctxp->c_archctl->ac_vol = ctxp->c_archctl->ac_curvol;
		pmFreeResult(*result);
		tmp.tv_sec = -1;
	    }
	    ctxp->c_archctl->ac_serial = 1;
	    if (pmDebugOptions.log) {
		if (nskip) {
		    fprintf(stderr, "__pmLogFetch: ctx=%d skip reverse %d to ",
			pmWhichContext(), nskip);
		    if (tmp.tv_sec != -1)
			__pmPrintTimeval(stderr, &tmp);
		    else
			fprintf(stderr, "unknown time");
		    fprintf(stderr, ", found=%d\n", found);
		}
#ifdef DESPERATE
		else
		    fprintf(stderr, "__pmLogFetch: ctx=%d no skip reverse\n",
			pmWhichContext());
#endif
	    }
	    nskip = 0;
	}
	if ((sts = __pmLogRead_ctx(ctxp, ctxp->c_mode, NULL, result, PMLOGREAD_NEXT)) < 0)
	    break;
	tmp.tv_sec = (__int32_t)(*result)->timestamp.tv_sec;
	tmp.tv_usec = (__int32_t)(*result)->timestamp.tv_usec;
	tdiff = __pmTimevalSub(&tmp, &ctxp->c_origin);
	if ((tdiff < 0 && ctxp_mode == PM_MODE_FORW) ||
	    (tdiff > 0 && ctxp_mode == PM_MODE_BACK)) {
		nskip++;
		pmFreeResult(*result);
		*result = NULL;
		continue;
	}
	found = 1;
	if (pmDebugOptions.log) {
	    if (nskip) {
		fprintf(stderr, "__pmLogFetch: ctx=%d skip %d to ",
		    pmWhichContext(), nskip);
		    pmPrintStamp(stderr, &(*result)->timestamp);
		    fputc('\n', stderr);
		}
#ifdef DESPERATE
	    else
		fprintf(stderr, "__pmLogFetch: ctx=%d no skip\n",
		    pmWhichContext());
#endif
	}
    }
    if (found) {
	ctxp->c_origin.tv_sec = (__int32_t)(*result)->timestamp.tv_sec;
	ctxp->c_origin.tv_usec = (__int32_t)(*result)->timestamp.tv_usec;
    }

    if (*result != NULL && (*result)->numpmid == 0) {
	/*
	 * mark record, and not interpolating ...
	 * if pmFetchArchive(), return it
	 * otherwise keep searching
	 */
	if (numpmid != 0) {
	    pmFreeResult(*result);
	    goto more;
	}
    }
    else if (found) {
	if (numpmid > 0) {
	    /*
	     * not necesssarily after them all, so cherry-pick the metrics
	     * we wanted ..
	     * there are two tricks here ...
	     * (1) pmValueSets for metrics requested, but not in the pmResult
	     *     from the log are assigned using the first two fields in the
	     *     pmid_ctl struct -- since these are allocated once as
	     *	   needed, and never free'd, we have to make sure pmFreeResult
	     *     finds a pmValueSet in a pinned PDU buffer ... this means
	     *     we must find at least one real value from the log to go
	     *     with any "unavailable" results
	     * (2) real pmValueSets can be ignored, they are in a pdubuf
	     *     and will be reclaimed when the buffer is unpinned in
	     *     pmFreeResult
	     */

	    i = (int)sizeof(pmResult) + numpmid * (int)sizeof(pmValueSet *);
	    if ((newres = (pmResult *)malloc(i)) == NULL) {
		pmNoMem("__pmLogFetch.newres", i, PM_FATAL_ERR);
	    }
	    newres->numpmid = numpmid;
	    newres->timestamp = (*result)->timestamp;
	    u = 0;
	    PM_LOCK(logutil_lock);
	    for (j = 0; j < numpmid; j++) {
		hp = __pmHashSearch((int)pmidlist[j], &pc_hc);
		if (hp == NULL) {
		    /* first time we've been asked for this one */
		    if ((pcp = (pmid_ctl *)malloc(sizeof(pmid_ctl))) == NULL) {
			PM_UNLOCK(logutil_lock);
			pmNoMem("__pmLogFetch.pmid_ctl", sizeof(pmid_ctl), PM_FATAL_ERR);
			/* NOTREACHED */
		    }
		    pcp->pc_pmid = pmidlist[j];
		    pcp->pc_numval = 0;
		    sts = __pmHashAdd((int)pmidlist[j], (void *)pcp, &pc_hc);
		    if (sts < 0) {
			PM_UNLOCK(logutil_lock);
			goto func_return;
		    }
		}
		else
		    pcp = (pmid_ctl *)hp->data;
		for (i = 0; i < (*result)->numpmid; i++) {
		    if (pmidlist[j] == (*result)->vset[i]->pmid) {
			/* match */
			newres->vset[j] = (*result)->vset[i];
			u++;
			break;
		    }
		}
		if (i == (*result)->numpmid) {
		    /*
		     * requested metric not returned from the log, construct
		     * a "no values available" pmValueSet from the pmid_ctl
		     */
		    newres->vset[j] = (pmValueSet *)pcp;
		}
	    }
	    PM_UNLOCK(logutil_lock);
	    if (u == 0 && !all_derived) {
		/*
		 * not one of our pmids was in the log record, try
		 * another log record ...
		 */
		pmFreeResult(*result);
		free(newres);
		goto more;
	    }
	    /*
	     * *result malloc'd in __pmLogRead, but vset[]'s are either in
	     * pdubuf or the pmid_ctl struct
	     */
	    free(*result);
	    *result = newres;
	}
	else
	    /* numpmid == 0, pmFetchArchive() call */
	    newres = *result;
	/*
	 * Apply instance profile filtering ...
	 * Note. This is a little strange, as in the numpmid == 0,
	 *       pmFetchArchive() case, this for-loop is not executed ...
	 *       this is correct, the instance profile is ignored for
	 *       pmFetchArchive()
	 */
	for (i = 0; i < numpmid; i++) {
	    if (newres->vset[i]->numval <= 0) {
		/*
		 * no need to xlate numval for an error ... already done
		 * below __pmLogRead() in __pmDecodeResult() ... also xlate
		 * here would have been skipped in the pmFetchArchive() case
		 */
		continue;
	    }
	    sts = __pmLogLookupDesc(ctxp->c_archctl, newres->vset[i]->pmid, &desc);
	    if (sts < 0) {
		char	strbuf[20];
		char	errmsg[PM_MAXERRMSGLEN];
		pmNotifyErr(LOG_WARNING, "__pmLogFetch: missing pmDesc for pmID %s: %s",
			    pmIDStr_r(desc.pmid, strbuf, sizeof(strbuf)), pmErrStr_r(sts, errmsg, sizeof(errmsg)));
		pmFreeResult(newres);
		break;
	    }
	    if (desc.indom == PM_INDOM_NULL)
		/* no instance filtering to be done for these ones */
		continue;

	    /*
	     * scan instances, keeping those "in" the instance profile
	     *
	     * WARNING
	     *		This compresses the pmValueSet INSITU, and since
	     *		these are in a PDU buffer it trashes the PDU
	     *		buffer - which means there is no clever way of
	     *		re-using the PDU buffer to satisfy multiple
	     *		pmFetch requests.
	     *		Fortunately, stdio buffering means copying to
	     *		make additional PDU buffers is not too expensive.
	     */
	    kval = 0;
	    for (j = 0; j < newres->vset[i]->numval; j++) {
		if (__pmInProfile(desc.indom, ctxp->c_instprof, newres->vset[i]->vlist[j].inst)) {
		    if (kval != j)
			 /* struct assignment */
			 newres->vset[i]->vlist[kval] = newres->vset[i]->vlist[j];
		    kval++;
		}
	    }
	    newres->vset[i]->numval = kval;
	}
    }

    /* remember your position in this context */
    ctxp->c_archctl->ac_offset = __pmFtell(ctxp->c_archctl->ac_mfp);
    assert(ctxp->c_archctl->ac_offset >= 0);
    ctxp->c_archctl->ac_vol = ctxp->c_archctl->ac_curvol;

func_return:

    if (ctx_ctl.need_ctx_unlock)
	PM_UNLOCK(ctx_ctl.ctxp->c_lock);

    return sts;
}

/*
 * error handling wrappers around __pmLogChangeVol() to deal with
 * missing volumes ... return lcp->l_ti[] index for entry matching
 * success
 */

#ifdef __PCP_EXPERIMENTAL_ARCHIVE_VERSION3
static int
VolSkip3(__pmArchCtl *acp, int mode, int j)
{
    __pmLogCtl	*lcp = acp->ac_log;
    int		vol = lcp->l_ti[j].v3.ti_vol;

    while (lcp->l_minvol <= vol && vol <= lcp->l_maxvol) {
	if (__pmLogChangeVol(acp, vol) >= 0)
	    return j;
	if (pmDebugOptions.log)
	    fprintf(stderr, "VolSkip: Skip missing vol %d\n", vol);
	if (mode == PM_MODE_FORW) {
	    for (j++; j < lcp->l_numti; j++)
		if (lcp->l_ti[j].v3.ti_vol != vol)
		    break;
	    if (j == lcp->l_numti)
		return PM_ERR_EOL;
	    vol = lcp->l_ti[j].v3.ti_vol;
	}
	else {
	    for (j--; j >= 0; j--)
		if (lcp->l_ti[j].v3.ti_vol != vol)
		    break;
	    if (j < 0)
		return PM_ERR_EOL;
	    vol = lcp->l_ti[j].v3.ti_vol;
	}
    }
    return PM_ERR_EOL;
}
#endif

static int
VolSkip2(__pmArchCtl *acp, int mode, int j)
{
    __pmLogCtl	*lcp = acp->ac_log;
    int		vol = lcp->l_ti[j].v2.ti_vol;

    while (lcp->l_minvol <= vol && vol <= lcp->l_maxvol) {
	if (__pmLogChangeVol(acp, vol) >= 0)
	    return j;
	if (pmDebugOptions.log)
	    fprintf(stderr, "VolSkip: Skip missing vol %d\n", vol);
	if (mode == PM_MODE_FORW) {
	    for (j++; j < lcp->l_numti; j++)
		if (lcp->l_ti[j].v2.ti_vol != vol)
		    break;
	    if (j == lcp->l_numti)
		return PM_ERR_EOL;
	    vol = lcp->l_ti[j].v2.ti_vol;
	}
	else {
	    for (j--; j >= 0; j--)
		if (lcp->l_ti[j].v2.ti_vol != vol)
		    break;
	    if (j < 0)
		return PM_ERR_EOL;
	    vol = lcp->l_ti[j].v2.ti_vol;
	}
    }
    return PM_ERR_EOL;
}


static int
VolSkip(__pmArchCtl *acp, int mode, int j)
{
#ifdef __PCP_EXPERIMENTAL_ARCHIVE_VERSION3
    if (__pmLogVersion(acp->ac_log) >= PM_LOG_VERS03)
	return VolSkip3(acp, mode, j);
#endif
    return VolSkip2(acp, mode, j);
}

static void
__pmLogPrintOffsetTI(__pmLogCtl *lcp, int version, int j)
{
    if (version >= PM_LOG_VERS03) {
#ifdef __PCP_EXPERIMENTAL_ARCHIVE_VERSION3
	pmTimespec      timestamp;

	timestamp.tv_sec = lcp->l_ti[j].v3.ti_sec;
	timestamp.tv_nsec = lcp->l_ti[j].v3.ti_nsec;
	__pmPrintTimespec(stderr, &timestamp);
#endif
    }
    else {
	__pmPrintTimeval(stderr, &lcp->l_ti[j].v2.ti_stamp);
    }
}

int
__pmLogSetTime(__pmContext *ctxp)
{
    __pmArchCtl	*acp = ctxp->c_archctl;
    __pmLogCtl	*lcp = acp->ac_log;
    pmTimeval	save_origin;
    int		save_mode;
    int		version = __pmLogVersion(lcp);
    double	t_hi;
    int		mode;
    int		i;

    mode = ctxp->c_mode & __PM_MODE_MASK; /* strip XTB data */

    if (mode == PM_MODE_INTERP)
	mode = ctxp->c_delta > 0 ? PM_MODE_FORW : PM_MODE_BACK;

    if (pmDebugOptions.log) {
	fprintf(stderr, "%s(%d) ", "__pmLogSetTime", pmWhichContext());
	__pmPrintTimeval(stderr, &ctxp->c_origin);
	fprintf(stderr, " delta=%d", ctxp->c_delta);
    }

    /*
     * Ultra coarse positioning. Start within the correct archive.
     * We're looking for the first archive which starts after the origin.
     */
    for (i = 0; i < acp->ac_num_logs; ++i) {
	t_hi = __pmTimevalSub(&acp->ac_log_list[i]->ml_starttime, &ctxp->c_origin);
	if (t_hi >= 0)
	    break; /* found it! */
    }
    if (mode == PM_MODE_FORW) {
	/* back up one archive, if possible. */
	if (i > 0)
	    --i;
    }
    else {
	/* Use the final archive, if none start after the origin. */
	if (i >= acp->ac_num_logs)
	    --i;
    }

    /*
     * __pmLogChangeArchive() will update the c_origin and c_mode fields of
     * the current context via __pmLogOpen(). However, we don't want that
     * here, so save this information and restore it after switching to the
     * new archive.
     */
    save_origin = ctxp->c_origin;
    save_mode = ctxp->c_mode;
    __pmLogChangeArchive(ctxp, i);
    ctxp->c_origin = save_origin;
    ctxp->c_mode = save_mode;

    if (lcp->l_numti) {
	/* we have a temporal index, use it! */
	int		j = -1;
	int		try;
	int		toobig = 0;
	int		match = 0;
	int		tivol;
	int		vol;
	int		numti = lcp->l_numti;
	off_t		tilog;
	__pmFILE	*f;
	__pmLogTI	*tip = lcp->l_ti;
	double		t_lo;
	struct stat	sbuf;

	sbuf.st_size = -1;

	for (i = 0; i < numti; i++, tip++) {
#ifdef __PCP_EXPERIMENTAL_ARCHIVE_VERSION3
	    if (version >= PM_LOG_VERS03) {
		tivol = tip->v3.ti_vol;
		tilog = tip->v3.ti_log;
	    } else {
#endif
		tivol = tip->v2.ti_vol;
		tilog = tip->v2.ti_log;
#ifdef __PCP_EXPERIMENTAL_ARCHIVE_VERSION3
	    }
#endif
	    if (tivol < lcp->l_minvol)
		/* skip missing preliminary volumes */
		continue;
	    if (tivol == lcp->l_maxvol) {
		/* truncated check for last volume */
		if (sbuf.st_size < 0) {
		    sbuf.st_size = 0;
		    vol = lcp->l_maxvol;
		    if (vol >= 0 && vol < lcp->l_numseen && lcp->l_seen[vol])
			__pmFstat(acp->ac_mfp, &sbuf);
		    else if ((f = _logpeek(acp, lcp->l_maxvol)) != NULL) {
			__pmFstat(f, &sbuf);
			__pmFclose(f);
		    }
		}
		if (tilog > sbuf.st_size) {
		    j = i;
		    toobig++;
		    break;
		}
	    }
#ifdef __PCP_EXPERIMENTAL_ARCHIVE_VERSION3
	    if (version >= PM_LOG_VERS03) {
		pmTimespec origin, timestamp;
		origin.tv_sec = ctxp->c_origin.tv_sec;
		origin.tv_nsec = ctxp->c_origin.tv_usec * 1000;
		timestamp.tv_sec = tip->v3.ti_sec;
		timestamp.tv_nsec = tip->v3.ti_nsec;
		t_hi = __pmTimespecSub(&timestamp, &origin);
	    } else {
#endif
		t_hi = __pmTimevalSub(&tip->v2.ti_stamp, &ctxp->c_origin);
#ifdef __PCP_EXPERIMENTAL_ARCHIVE_VERSION3
	    }
#endif
	    if (t_hi > 0) {
		j = i;
		break;
	    }
	    else if (t_hi == 0) {
		j = i;
		match = 1;
		break;
	    }
	}
	if (i == numti)
	    j = numti;

	acp->ac_serial = 1;

	if (match) {
	    try = j;
	    j = VolSkip(acp, mode, j);
	    if (j < 0) {
		if (pmDebugOptions.log)
		    fprintf(stderr, "%s: VolSkip mode=%d vol=%d failed #1\n",
				    "__pmLogSetTime", mode, try);
		return PM_ERR_LOGFILE;
	    }
#ifdef __PCP_EXPERIMENTAL_ARCHIVE_VERSION3
	    if (version >= PM_LOG_VERS03)
		tilog = lcp->l_ti[j].v3.ti_log;
	    else
#endif
		tilog = lcp->l_ti[j].v2.ti_log;
	    __pmFseek(acp->ac_mfp, (long)tilog, SEEK_SET);
	    if (mode == PM_MODE_BACK)
		acp->ac_serial = 0;
	    if (pmDebugOptions.log) {
		fprintf(stderr, " at ti[%d]@", j);
		__pmLogPrintOffsetTI(lcp, version, j);
	    }
	}
	else if (j < 1) {
	    try = 0;
	    j = VolSkip(acp, PM_MODE_FORW, 0);
	    if (j < 0) {
		if (pmDebugOptions.log)
		    fprintf(stderr, "%s: VolSkip mode=%d vol=%d failed #2\n",
				    "__pmLogSetTime", PM_MODE_FORW, try);
		return PM_ERR_LOGFILE;
	    }
#ifdef __PCP_EXPERIMENTAL_ARCHIVE_VERSION3
	    if (version >= PM_LOG_VERS03)
		tilog = lcp->l_ti[j].v3.ti_log;
	    else
#endif
		tilog = lcp->l_ti[j].v2.ti_log;
	    __pmFseek(acp->ac_mfp, (long)tilog, SEEK_SET);
	    if (pmDebugOptions.log) {
		fprintf(stderr, " before start ti@");
		__pmLogPrintOffsetTI(lcp, version, j);
	    }
	}
	else if (j == numti) {
	    try = numti-1;
	    j = VolSkip(acp, PM_MODE_BACK, numti-1);
	    if (j < 0) {
		if (pmDebugOptions.log)
		    fprintf(stderr, "%s: VolSkip mode=%d vol=%d failed #3\n",
				    "__pmLogSetTime", PM_MODE_BACK, try);
		return PM_ERR_LOGFILE;
	    }
#ifdef __PCP_EXPERIMENTAL_ARCHIVE_VERSION3
	    if (version >= PM_LOG_VERS03)
		tilog = lcp->l_ti[j].v3.ti_log;
	    else
#endif
		tilog = lcp->l_ti[j].v2.ti_log;
	    __pmFseek(acp->ac_mfp, (long)tilog, SEEK_SET);
	    if (mode == PM_MODE_BACK)
		acp->ac_serial = 0;
	    if (pmDebugOptions.log) {
		fprintf(stderr, " after end ti@");
		__pmLogPrintOffsetTI(lcp, version, j);
	    }
	}
	else {
	    /*
	     *    [j-1]             [origin]           [j]
	     *      <----- t_lo -------><----- t_hi ---->
	     *
	     * choose closest index point.  if toobig, [j] is not
	     * really valid (log truncated or incomplete)
	     */
#ifdef __PCP_EXPERIMENTAL_ARCHIVE_VERSION3
	    if (version >= PM_LOG_VERS03) {
		pmTimespec origin, timestamp;
		origin.tv_sec = ctxp->c_origin.tv_sec;
		origin.tv_nsec = ctxp->c_origin.tv_usec * 1000;

		timestamp.tv_sec = lcp->l_ti[j].v3.ti_sec;
		timestamp.tv_nsec = lcp->l_ti[j].v3.ti_nsec;
		t_hi = __pmTimespecSub(&timestamp, &origin);
		timestamp.tv_sec = lcp->l_ti[j-1].v3.ti_sec;
		timestamp.tv_nsec = lcp->l_ti[j-1].v3.ti_nsec;
		t_lo = __pmTimespecSub(&origin, &timestamp);
	    } else {
		pmTimeval timestamp;
		timestamp = lcp->l_ti[j].v2.ti_stamp;
		t_hi = __pmTimevalSub(&timestamp, &ctxp->c_origin);
		timestamp = lcp->l_ti[j-1].v2.ti_stamp;
		t_lo = __pmTimevalSub(&ctxp->c_origin, &timestamp);
	    }
#else
	    t_hi = __pmTimevalSub(&lcp->l_ti[j].v2.ti_stamp, &ctxp->c_origin);
	    t_lo = __pmTimevalSub(&ctxp->c_origin, &lcp->l_ti[j-1].v2.ti_stamp);
#endif
	    if (t_hi <= t_lo && !toobig) {
		try = j;
		j = VolSkip(acp, mode, j);
		if (j < 0) {
		    if (pmDebugOptions.log)
			fprintf(stderr, "%s: VolSkip mode=%d vol=%d failed #4\n",
					"__pmLogSetTime", mode, try);
		    return PM_ERR_LOGFILE;
		}
#ifdef __PCP_EXPERIMENTAL_ARCHIVE_VERSION3
		if (version >= PM_LOG_VERS03)
		    tilog = lcp->l_ti[j].v3.ti_log;
		else
#endif
		    tilog = lcp->l_ti[j].v2.ti_log;
		__pmFseek(acp->ac_mfp, (long)tilog, SEEK_SET);
		if (mode == PM_MODE_FORW)
		    acp->ac_serial = 0;
		if (pmDebugOptions.log) {
		    fprintf(stderr, " before ti[%d]@", j);
		    __pmLogPrintOffsetTI(lcp, version, j);
		}
	    }
	    else {
		try = j-1;
		j = VolSkip(acp, mode, j-1);
		if (j < 0) {
		    if (pmDebugOptions.log)
			fprintf(stderr, "%s: VolSkip mode=%d vol=%d failed #5\n",
					"__pmLogSetTime", mode, try);
		    return PM_ERR_LOGFILE;
		}
#ifdef __PCP_EXPERIMENTAL_ARCHIVE_VERSION3
		if (version >= PM_LOG_VERS03)
		    tilog = lcp->l_ti[j].v3.ti_log;
		else
#endif
		    tilog = lcp->l_ti[j].v2.ti_log;
		__pmFseek(acp->ac_mfp, (long)tilog, SEEK_SET);
		if (mode == PM_MODE_BACK)
		    acp->ac_serial = 0;
		if (pmDebugOptions.log) {
		    fprintf(stderr, " after ti[%d]@", j);
		    __pmLogPrintOffsetTI(lcp, version, j);
		}
	    }
	    if (acp->ac_serial && mode == PM_MODE_FORW) {
		/*
		 * back up one record ...
		 * index points to the END of the record!
		 */
		pmResult	*result;
		if (pmDebugOptions.log)
		    fprintf(stderr, " back up ...\n");
		if (__pmLogRead_ctx(ctxp, PM_MODE_BACK, NULL, &result, PMLOGREAD_NEXT) >= 0)
		    pmFreeResult(result);
		if (pmDebugOptions.log)
		    fprintf(stderr, "...");
	    }
	}
    }
    else {
	/* index either not available, or not useful */
	int	j;
	if (mode == PM_MODE_FORW) {
	    for (j = lcp->l_minvol; j <= lcp->l_maxvol; j++) {
		if (__pmLogChangeVol(acp, j) >= 0)
		    break;
	    }
	    if (j > lcp->l_maxvol) {
		/* no volume found */
		if (pmDebugOptions.log)
		    fprintf(stderr, " index not useful, no volume between %d...%d\n",
			    lcp->l_minvol, lcp->l_maxvol);
		acp->ac_curvol = -1;
		acp->ac_mfp = NULL;
		return PM_ERR_LOGFILE;
	    }
	    __pmFseek(acp->ac_mfp, (long)__pmLogLabelTotalSize(lcp), SEEK_SET);
	}
	else if (mode == PM_MODE_BACK) {
	    for (j = lcp->l_maxvol; j >= lcp->l_minvol; j--) {
		if (__pmLogChangeVol(acp, j) >= 0)
		    break;
	    }
	    if (j < lcp->l_minvol) {
		/* no volume found */
		if (pmDebugOptions.log)
		    fprintf(stderr, " index not useful, no volume between %d...%d\n",
			    lcp->l_maxvol, lcp->l_minvol);
		acp->ac_curvol = -1;
		acp->ac_mfp = NULL;
		return PM_ERR_LOGFILE;
	    }
	    __pmFseek(acp->ac_mfp, (long)0, SEEK_END);
	}

	if (pmDebugOptions.log)
	    fprintf(stderr, " index not useful\n");
    }

    if (pmDebugOptions.log)
	fprintf(stderr, " vol=%d posn=%ld serial=%d\n",
	    acp->ac_curvol, (long)__pmFtell(acp->ac_mfp), acp->ac_serial);

    /* remember your position in this context */
    acp->ac_offset = __pmFtell(acp->ac_mfp);
    assert(acp->ac_offset >= 0);
    acp->ac_vol = acp->ac_curvol;

    return 0;
}

/* Read the label of the current archive. */
int
__pmGetArchiveLabel(__pmLogCtl *lcp, pmLogLabel *lp)
{
    __pmLogLabel	*rlp;

    /*
     * we have to copy the structure to hide the differences
     * between the internal pmTimeval and the external struct timeval
     */
    rlp = &lcp->l_label;
    lp->ll_magic = rlp->ill_magic;
    lp->ll_pid = (pid_t)rlp->ill_pid;
    lp->ll_start.tv_sec = rlp->ill_start.tv_sec;
    lp->ll_start.tv_usec = rlp->ill_start.tv_usec;
    memcpy(lp->ll_hostname, rlp->ill_hostname, PM_LOG_MAXHOSTLEN);
    memcpy(lp->ll_tz, rlp->ill_tz, sizeof(lp->ll_tz));
    return 0;
}

/* Read the label of the first archive in the context. */
int
pmGetArchiveLabel(pmLogLabel *lp)
{
    int		save_arch = 0;		/* pander to gcc */
    int		save_vol = 0;		/* pander to gcc */
    long	save_offset = 0;	/* pander to gcc */
    int		sts;
    int		restore = 0;
    __pmContext	*ctxp;
    __pmArchCtl	*acp;
    __pmLogCtl	*lcp;

    ctxp = __pmHandleToPtr(pmWhichContext());
    if (ctxp == NULL) 
	return PM_ERR_NOCONTEXT;
    if (ctxp->c_type != PM_CONTEXT_ARCHIVE) {
	PM_UNLOCK(ctxp->c_lock);
	return PM_ERR_NOTARCHIVE;
    }
    acp = ctxp->c_archctl;
    lcp = acp->ac_log;

    /* If necessary, switch to the first archive in the context. */
    if (acp->ac_cur_log != 0) {
	/* Save the initial state. */
	save_arch = ctxp->c_archctl->ac_cur_log;
	save_vol = ctxp->c_archctl->ac_vol;
	save_offset = ctxp->c_archctl->ac_offset;

	if ((sts = __pmLogChangeArchive(ctxp, 0)) < 0) {
	    PM_UNLOCK(ctxp->c_lock);
	    return sts;
	}
	lcp = acp->ac_log;
	restore = 1;
    }

    /* Get the label. */
    if ((sts = __pmGetArchiveLabel(lcp, lp)) < 0) {
	PM_UNLOCK(ctxp->c_lock);
	return sts;
    }

    if (restore) {
	/* Restore to the initial state. */
	if ((sts = __pmLogChangeArchive(ctxp, save_arch)) < 0) {
	    PM_UNLOCK(ctxp->c_lock);
	    return sts;
	}
	if ((sts = __pmLogChangeVol(acp, save_vol)) < 0) {
	    PM_UNLOCK(ctxp->c_lock);
	    return sts;
	}
	__pmFseek(acp->ac_mfp, save_offset, SEEK_SET);
    }

    PM_UNLOCK(ctxp->c_lock);
    return 0;
}

/*
 * Get the end time of the current archive.
 *
 * Internal variant of __pmGetArchiveEnd() ... using a
 * __pmContext * instead of a __pmLogCtl * as the first argument
 * so that the current context can be carried down the call stack.
 */
int
__pmGetArchiveEnd_ctx(__pmContext *ctxp, struct timeval *tp)
{
    __pmArchCtl	*acp = ctxp->c_archctl;
    __pmLogCtl	*lcp = ctxp->c_archctl->ac_log;
    struct stat	sbuf;
    __pmFILE	*f;
    long	save = 0;
    pmResult	*rp = NULL;
    pmResult	*nrp;
    int		i;
    int		sts;
    int		found;
    int		head;
    long	offset;
    int		vol;
    __pmoff32_t	logend;
    __pmoff32_t	physend = 0;

    PM_ASSERT_IS_LOCKED(ctxp->c_lock);

    /*
     * default, when all else fails ...
     */
    tp->tv_sec = INT_MAX;
    tp->tv_usec = 0;

    /*
     * expect things to be stable, so l_maxvol is not empty, and
     * l_physend does not change for l_maxvol ... the ugliness is
     * to handle situations where these expectations are not met
     */
    found = 0;
    sts = PM_ERR_LOGREC;	/* default error condition */
    f = NULL;

    /*
     * start at last volume and work backwards until success or
     * failure
     */
    for (vol = lcp->l_maxvol; vol >= lcp->l_minvol; vol--) {
	if (acp->ac_curvol == vol) {
	    f = acp->ac_mfp;
	    save = __pmFtell(f);
	    assert(save >= 0);
	}
	else if ((f = _logpeek(acp, vol)) == NULL) {
	    /* failed to open this one, try previous volume(s) */
	    continue;
	}

	if (__pmFstat(f, &sbuf) < 0) {
	    /* if we can't stat() this one, then try previous volume(s) */
	    goto prior_vol;
	}

	if (vol == lcp->l_maxvol && sbuf.st_size == lcp->l_physend) {
	    /* nothing changed, return cached stuff */
	    tp->tv_sec = lcp->l_endtime.tv_sec;
	    tp->tv_usec = lcp->l_endtime.tv_usec;
	    sts = 0;
	    break;
	}

	/* if this volume is empty, try previous volume */
	if (sbuf.st_size <= (int)sizeof(__pmLogLabel) + 2*(int)sizeof(int)) {
	    goto prior_vol;
	}

	physend = (__pmoff32_t)sbuf.st_size;
	if (sizeof(off_t) > sizeof(__pmoff32_t)) {
	    /* 64-bit off_t */
	    if (physend != sbuf.st_size) {
		/* oops, 32-bit offset not the same */
		pmNotifyErr(LOG_ERR, "pmGetArchiveEnd: PCP archive file"
			" (meta) too big (%"PRIi64" bytes)\n",
			(uint64_t)sbuf.st_size);
		sts = PM_ERR_TOOBIG;
		break;
	    }
	}

	/* try to read backwards for the last physical record ... */
	__pmFseek(f, (long)physend, SEEK_SET);
	if (paranoidLogRead(ctxp, PM_MODE_BACK, f, &rp) >= 0) {
	    /* success, we are done! */
	    found = 1;
	    break;
	}

	/*
	 * failure at the physical end of file may be related to a truncted
	 * block flush for a growing archive.  Scan temporal index, and use
	 * last entry at or before end of physical file for this volume
	 */
	logend = (int)sizeof(__pmLogLabel) + 2*(int)sizeof(int);
	for (i = lcp->l_numti - 1; i >= 0; i--) {
	    if (lcp->l_ti[i].v2.ti_vol != vol)
		continue;
	    if (lcp->l_ti[i].v2.ti_log <= physend) {
		logend = lcp->l_ti[i].v2.ti_log;
		break;
	    }
	}
	if (i < 0) {
	    /* no dice in the temporal index, try previous volume */
	    goto prior_vol;
	}

	/*
	 * Now chase it forwards from the last index entry ...
	 *
	 * BUG 357003 - pmchart can't read archive file
	 *	turns out the index may point to the _end_ of the last
	 *	valid record, so if not at start of volume, back up one
	 *	record, then scan forwards.
	 */
	assert(f != NULL);
	__pmFseek(f, (long)logend, SEEK_SET);
	if (logend > (int)sizeof(__pmLogLabel) + 2*(int)sizeof(int)) {
	    if (paranoidLogRead(ctxp, PM_MODE_BACK, f, &rp) < 0) {
		/* this is badly damaged! */
		if (pmDebugOptions.log) {
		    fprintf(stderr, "pmGetArchiveEnd: "
                            "Error reading record ending at posn=%d ti[%d]@",
			    logend, i);
		    __pmPrintTimeval(stderr, &lcp->l_ti[i].v2.ti_stamp);
		    fputc('\n', stderr);
		}
		break;
	    }
	}

        /* Keep reading records from "logend" until can do so no more... */
	for ( ; ; ) {
	    offset = __pmFtell(f);
	    assert(offset >= 0);
	    if ((int)__pmFread(&head, 1, sizeof(head), f) != sizeof(head))
		/* cannot read header for log record !!?? */
		break;
	    head = ntohl(head);
	    if (offset + head > physend)
		/* last record is incomplete */
		break;
	    __pmFseek(f, offset, SEEK_SET);
	    if (paranoidLogRead(ctxp, PM_MODE_FORW, f, &nrp) < 0)
		/* this record is truncated, or bad, we lose! */
		break;
	    /* this one is ok, remember it as it may be the last one */
	    found = 1;
	    if (rp != NULL)
		pmFreeResult(rp);
	    rp = nrp;
	}
	if (found)
	    break;

prior_vol:
	/*
	 * this probably means this volume contains no useful records,
	 * try the previous volume
	 */
	if (f != acp->ac_mfp) {
	    /* f comes from _logpeek(), close it */
	    __pmFclose(f);
	    f = NULL;
	}

    }/*for*/

    if (f == acp->ac_mfp)
	__pmFseek(f, save, SEEK_SET); /* restore file pointer in current vol */ 
    else if (f != NULL)
	/* temporary __pmFILE * from _logpeek() */
	__pmFclose(f);

    if (found) {
	tp->tv_sec = (time_t)rp->timestamp.tv_sec;
	tp->tv_usec = (int)rp->timestamp.tv_usec;
	if (vol == lcp->l_maxvol) {
	    lcp->l_endtime.tv_sec = (__int32_t)rp->timestamp.tv_sec;
	    lcp->l_endtime.tv_usec = (__int32_t)rp->timestamp.tv_usec;
	    lcp->l_physend = physend;
	}
	sts = 0;
    }
    if (rp != NULL) {
	/*
	 * rp is not NULL from found==1 path _or_ from error break
	 * after an initial paranoidLogRead() success
	 */
	pmFreeResult(rp);
    }

    return sts;
}

int
__pmGetArchiveEnd(__pmArchCtl *acp, struct timeval *tp)
{
    int		sts;
    __pmContext	*ctxp;

    if ((sts = pmWhichContext()) < 0)
	return sts;
    ctxp = __pmHandleToPtr(sts);
    if (ctxp == NULL)
	return PM_ERR_NOCONTEXT;
    if (ctxp->c_type != PM_CONTEXT_ARCHIVE) {
	PM_UNLOCK(ctxp->c_lock);
	return PM_ERR_NOTARCHIVE;
    }
    sts = __pmGetArchiveEnd_ctx(ctxp, tp);
    PM_UNLOCK(ctxp->c_lock);
    return sts;
}

/* Get the end time of the final archive in the context. */
int
pmGetArchiveEnd(struct timeval *tp)
{
    int		sts;
    sts = pmGetArchiveEnd_ctx(NULL, tp);
    return sts;

}

/*
 * ctxp->c_lock is held throughout this routine
 */
int
pmGetArchiveEnd_ctx(__pmContext *ctxp, struct timeval *tp)
{
    int		save_arch = 0;		/* pander to gcc */
    int		save_vol = 0;		/* pander to gcc */
    long	save_offset = 0;	/* pander to gcc */
    int		sts;
    int		restore = 0;
    int		need_unlock = 0;
    __pmArchCtl	*acp;

    if (ctxp == NULL) {
	if ((sts = pmWhichContext()) < 0) {
	    return sts;
	}
	ctxp = __pmHandleToPtr(sts);
	if (ctxp == NULL)
	    return PM_ERR_NOCONTEXT;
	need_unlock = 1;
    }
    else
	PM_ASSERT_IS_LOCKED(ctxp->c_lock);

    /*
     * set l_physend and l_endtime
     * at the end of ... ctxp->c_archctl->ac_log
     */
    if (ctxp->c_type != PM_CONTEXT_ARCHIVE) {
	return PM_ERR_NOTARCHIVE;
    }
    acp = ctxp->c_archctl;

    /* If necessary, switch to the last archive in the context. */
    if (acp->ac_cur_log != acp->ac_num_logs - 1) {
	/* Save the initial state. */
	save_arch = ctxp->c_archctl->ac_cur_log;
	save_vol = ctxp->c_archctl->ac_vol;
	save_offset = ctxp->c_archctl->ac_offset;

	if ((sts = __pmLogChangeArchive(ctxp, acp->ac_num_logs - 1)) < 0) {
	    if (need_unlock)
		PM_UNLOCK(ctxp->c_lock);
	    return sts;
	}
	restore = 1;
    }

    if ((sts = __pmGetArchiveEnd_ctx(ctxp, tp)) < 0) {
	if (need_unlock)
	    PM_UNLOCK(ctxp->c_lock);
	return sts;
    }

    if (restore) {
	/* Restore to the initial state. */
	if ((sts = __pmLogChangeArchive(ctxp, save_arch)) < 0) {
	    if (need_unlock)
		PM_UNLOCK(ctxp->c_lock);
	    return sts;
	}
	if ((sts = __pmLogChangeVol(acp, save_vol)) < 0) {
	    if (need_unlock)
		PM_UNLOCK(ctxp->c_lock);
	    return sts;
	}
	__pmFseek(acp->ac_mfp, save_offset, SEEK_SET);
    }

    if (need_unlock)
	PM_UNLOCK(ctxp->c_lock);
    return sts;
}

int
__pmLogChangeArchive(__pmContext *ctxp, int arch)
{
    __pmArchCtl		*acp = ctxp->c_archctl;
    __pmMultiLogCtl	*mlcp = acp->ac_log_list[arch];
    int			sts;

    /*
     * If we're already using the requested archive, then we don't need to
     * switch.
     */
    if (arch == acp->ac_cur_log)
	return 0;

    /*
     * Obtain a handle for the named archive.
     * __pmFindOrOpenArchive() will take care of closing the active archive,
     * if necessary.
     */
    sts = __pmFindOrOpenArchive(ctxp, mlcp->ml_name, 1/*multi_arch*/);
    if (sts < 0)
	return sts;

    acp->ac_cur_log = arch;
    acp->ac_mark_done = 0;

    return sts;
}

/*
 * Check whether there is a next archive to switch to. Generate a MARK
 * record if one has not already been generated.
 *
 * Internal variant of __pmLogCheckForNextArchive() ... using a
 * __pmContext * instead of a __pmLogCtl * as the first argument
 * so that the current context can be carried down the call stack.
 */
static int
LogCheckForNextArchive(__pmContext *ctxp, int mode, pmResult **result)
{
    __pmArchCtl	*acp;
    int		sts = 0;

    /*
     * Check whether there is a subsequent archive to switch to.
     */
    acp = ctxp->c_archctl;
    if ((mode == PM_MODE_FORW && acp->ac_cur_log >= acp->ac_num_logs - 1) ||
	(mode == PM_MODE_BACK && acp->ac_cur_log == 0))
	sts = PM_ERR_EOL; /* no more archives */
    else {
	/*
	 * Check whether we need to generate a mark record.
	 */
	if (! acp->ac_mark_done) {
	    sts = __pmLogGenerateMark_ctx(ctxp, mode, result);
	    acp->ac_mark_done = mode;
	}
	else {
	    *result = NULL;
	}
    }

    return sts;
}

int
__pmLogCheckForNextArchive(__pmLogCtl *lcp, int mode, pmResult **result)
{
    int		sts;
    __pmContext	*ctxp;

    if ((sts = pmWhichContext()) < 0)
	return sts;
    ctxp = __pmHandleToPtr(sts);
    if (ctxp == NULL)
	return PM_ERR_NOCONTEXT;
    if (ctxp->c_type != PM_CONTEXT_ARCHIVE) {
	PM_UNLOCK(ctxp->c_lock);
	return PM_ERR_NOTARCHIVE;
    }
    sts = LogCheckForNextArchive(ctxp, mode, result);
    PM_UNLOCK(ctxp->c_lock);
    return sts;
}

/*
 * Advance forward to the next archive in the context, if any.
 *
 * Internal variant of __pmLogChangeToNextArchive() ... using a
 * __pmContext * instead of a __pmLogCtl * as the first argument
 * so that the current context can be carried down the call stack.
 */
static int
LogChangeToNextArchive(__pmContext *ctxp)
{
    __pmLogCtl	*lcp = ctxp->c_archctl->ac_log;
    __pmArchCtl	*acp;
    pmTimeval	prev_endtime;
    pmTimeval	save_origin;
    int		save_mode;

    /*
     * Check whether there is a subsequent archive to switch to.
     */
    acp = ctxp->c_archctl;
    if (acp->ac_cur_log >= acp->ac_num_logs - 1)
	return PM_ERR_EOL; /* no more archives */

    /*
     * We're changing to the next archive because we have reached the end of
     * the current one while reading forward.
     * We will need to check for temporal overlap between the current
     * archive and the next archive. We do this at the time of the attempted
     * transtition because archives can be 'live' and their ranges
     * in time can change dynamically.
     *
     * l_endtime for the current archive was updated when the <mark>
     * record was generated. Save it.
     */
    prev_endtime = lcp->l_endtime;
	       
    /*
     * __pmLogChangeArchive() will update the c_origin and c_mode fields of
     * the current context via __pmLogOpen(). However, we don't want that
     * here, so save this information and restore it after switching to the
     * new archive.
     */
    save_origin = ctxp->c_origin;
    save_mode = ctxp->c_mode;
    /* Switch to the next archive. */
    __pmLogChangeArchive(ctxp, acp->ac_cur_log + 1);
    lcp = acp->ac_log;
    ctxp->c_origin = save_origin;
    ctxp->c_mode = save_mode;

    /*
     * We want to reposition to the start of the archive.
     * Start after the header + label record + trailer
     */
    acp->ac_offset = __pmLogLabelTotalSize(lcp);
    acp->ac_vol = acp->ac_curvol;

    /*
     * Check for temporal overlap here. Do this last in case the API client
     * chooses to keep reading anyway.
     */
    if (__pmTimevalSub(&prev_endtime, &lcp->l_label.ill_start) > 0)
	return PM_ERR_LOGOVERLAP;

    return 0;
}

int
__pmLogChangeToNextArchive(__pmLogCtl **lcp)
{
    int		sts;
    __pmContext	*ctxp;

    if ((sts = pmWhichContext()) < 0)
	return sts;
    ctxp = __pmHandleToPtr(sts);
    if (ctxp == NULL)
	return PM_ERR_NOCONTEXT;
    if (ctxp->c_type != PM_CONTEXT_ARCHIVE) {
	PM_UNLOCK(ctxp->c_lock);
	return PM_ERR_NOTARCHIVE;
    }
    if ((sts = LogChangeToNextArchive(ctxp)) == 0) {
	*lcp = ctxp->c_archctl->ac_log;
    }
    PM_UNLOCK(ctxp->c_lock);
    return sts;
}

/*
 * Advance backward to the previous archive in the context, if any.
 *
 * Internal variant of __pmLogChangeToPreviousArchive() ... using a
 * __pmContext * instead of a __pmLogCtl * as the first argument
 * so that the current context can be carried down the call stack.
 */
static int
LogChangeToPreviousArchive(__pmContext *ctxp)
{
    __pmLogCtl		*lcp = ctxp->c_archctl->ac_log;
    __pmArchCtl		*acp;
    struct timeval	current_endtime;
    pmTimeval		prev_starttime;
    pmTimeval		save_origin;
    int			save_mode;
    int			sts;
    int			j;

    /*
     * Check whether there is a previous archive to switch to.
     */
    acp = ctxp->c_archctl;
    if (acp->ac_cur_log == 0) {
	return PM_ERR_EOL; /* no more archives */
    }

    /*
     * We're changing to the previous archive because we have reached the
     * beginning of the current one while reading backward.
     * We will need to check for temporal overlap between the current
     * archive and the next archive. We do this at the time of the attempted
     * transtition because archives can be 'live' and their ranges
     * in time can change dynamically.
     *
     * Save the start time of the current archive.
     */
    prev_starttime = lcp->l_label.ill_start;

    /*
     * __pmLogChangeArchive() will update the c_origin and c_mode fields of
     * the current context, either via __pmLogOpen() or directly, if the
     * new archive is already open. However, we don't want that here, so
     * save this information and restore it after switching to the new
     * archive.
     */
    save_origin = ctxp->c_origin;
    save_mode = ctxp->c_mode;
    /* Switch to the next archive. */
    __pmLogChangeArchive(ctxp, acp->ac_cur_log - 1);
    lcp = acp->ac_log;
    ctxp->c_origin = save_origin;
    ctxp->c_mode = save_mode;

    /*
     * We need the current end time of the new archive in order to compare
     * with the start time of the previous one.
     */
    if ((sts = __pmGetArchiveEnd_ctx(ctxp, &current_endtime)) < 0) {
	return sts;
    }

    /* Set up to scan backwards from the end of the archive. */
    for (j = lcp->l_maxvol; j >= lcp->l_minvol; j--) {
	if (__pmLogChangeVol(acp, j) >= 0)
	    break;
    }
    if (j < lcp->l_minvol) {
	/* no volume found */
	if (pmDebugOptions.log)
	    fprintf(stderr, "LogChangeToPreviousArchive: no volume between %d...%d\n",
		    lcp->l_maxvol, lcp->l_minvol);
	acp->ac_curvol = -1;
	acp->ac_mfp = NULL;
	return PM_ERR_LOGFILE;
    }
    __pmFseek(acp->ac_mfp, (long)0, SEEK_END);
    acp->ac_offset = __pmFtell(acp->ac_mfp);
    assert(acp->ac_offset >= 0);
    acp->ac_vol = acp->ac_curvol;

    /*
     * Check for temporal overlap here. Do this last in case the API client
     * chooses to keep reading anyway.
     */
    if (__pmTimevalSub(&lcp->l_endtime, &prev_starttime) > 0) {
	return PM_ERR_LOGOVERLAP;  /* temporal overlap */
    }

    return 0;
}

int
__pmLogChangeToPreviousArchive(__pmLogCtl **lcp)
{
    int		sts;
    __pmContext	*ctxp;

    if ((sts = pmWhichContext()) < 0)
	return sts;
    ctxp = __pmHandleToPtr(sts);
    if (ctxp == NULL)
	return PM_ERR_NOCONTEXT;
    if (ctxp->c_type != PM_CONTEXT_ARCHIVE) {
	PM_UNLOCK(ctxp->c_lock);
	return PM_ERR_NOTARCHIVE;
    }
    if ((sts = LogChangeToPreviousArchive(ctxp)) == 0) {
	*lcp = ctxp->c_archctl->ac_log;
    }
    PM_UNLOCK(ctxp->c_lock);
    return sts;
}

pmTimeval *
__pmLogStartTime(__pmArchCtl *acp)
{
    return &acp->ac_log_list[0]->ml_starttime;
}

void
__pmArchCtlFree(__pmArchCtl *acp)
{
    /*
     * If this is the last ref, then close the archive.
     * refcnt == 0 means the log is not open.
     */
    __pmLogCtl *lcp = acp->ac_log;

    if (lcp != NULL) {
	PM_LOCK(lcp->l_lock);
	if (--lcp->l_refcnt == 0) {
	    PM_UNLOCK(lcp->l_lock);
	    __pmLogClose(acp);
	    logFreeMeta(lcp);
#ifdef PM_MULTI_THREAD
	    __pmDestroyMutex(&lcp->l_lock);
#endif
	    free(lcp);
	}
	else
	    PM_UNLOCK(lcp->l_lock);
    }

    /* We need to clean up the archive list. */
    if (acp->ac_log_list != NULL) {
	while (--acp->ac_num_logs >= 0) {
	    assert (acp->ac_log_list[acp->ac_num_logs] != NULL);
	    free(acp->ac_log_list[acp->ac_num_logs]->ml_name);
	    free(acp->ac_log_list[acp->ac_num_logs]->ml_hostname);
	    free(acp->ac_log_list[acp->ac_num_logs]->ml_tz);
	    free(acp->ac_log_list[acp->ac_num_logs]);
	}
	free(acp->ac_log_list);
    }

    /* And the cache. */
    if (acp->ac_cache != NULL)
	free(acp->ac_cache);

    if (acp->ac_mfp != NULL) {
	__pmResetIPC(__pmFileno(acp->ac_mfp));
	__pmFclose(acp->ac_mfp);
	acp->ac_mfp = NULL;
    }

    /* Now we can free it. */
    free(acp);
}

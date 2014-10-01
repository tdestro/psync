/* $Id$ */
/* %PSC_COPYRIGHT% */

#include <sys/param.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/un.h>

#include <fcntl.h>
#include <gcrypt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "pfl/alloc.h"
#include "pfl/net.h"
#include "pfl/pool.h"
#include "pfl/stat.h"
#include "pfl/str.h"
#include "pfl/thread.h"
#include "pfl/walk.h"

#include "psync.h"
#include "rpc.h"

#define MAX_BUFSZ	(1024 * 1024)

char			 objns_path[PATH_MAX];
int			 objns_depth = 2;

volatile sig_atomic_t	 exit_from_signal;

void *
buf_get(size_t len)
{
	struct buf *b;

	b = psc_pool_get(buf_pool);
	if (len > b->len) {
		b->buf = psc_realloc(b->buf, len, 0);
		// memset(b->buf + oldlen, 0, len - oldlen);
		b->len = len;
	}
	return (b);

}

#define buf_release(b)	psc_pool_return(buf_pool, (b))


void
rpc_send_getfile(uint64_t xid, const char *fn)
{
	struct rpc_getfile_req gfq;
	struct iovec iov[2];
	struct stream *st;

warnx("getfile");
	memset(&gfq, 0, sizeof(gfq));

	if (opt_recursive)
		gfq.flags = RPC_GETFILE_F_RECURSE;

	iov[0].iov_base = &gfq;
	iov[0].iov_len = sizeof(gfq);

	iov[1].iov_base = (void *)fn;
	iov[1].iov_len = strlen(fn) + 1;

	st = stream_get();
	stream_sendxv(st, xid, OPC_GETFILE_REQ, iov, nitems(iov));
	stream_release(st);
}

void
rpc_send_putdata(uint64_t fid, off_t off, const void *buf, size_t len)
{
	struct rpc_putdata pd;
	struct iovec iov[2];
	struct stream *st;

	memset(&pd, 0, sizeof(pd));
	pd.fid = fid;
	pd.off = off;

	iov[0].iov_base = &pd;
	iov[0].iov_len = sizeof(pd);

	iov[1].iov_base = (void *)buf;
	iov[1].iov_len = len;

	st = stream_get();
	stream_sendv(st, OPC_PUTDATA, iov, nitems(iov));
	stream_release(st);
}

void
rpc_send_putname(const char *fn, const struct stat *stb)
{
	struct rpc_putname pn;
	struct iovec iov[2];
	struct stream *st;
warnx("putname");

	memset(&pn, 0, sizeof(pn));
	pn.pstb.dev = stb->st_dev;
	pn.pstb.rdev = stb->st_rdev;
	pn.pstb.mode = stb->st_mode;
	pn.pstb.uid = stb->st_uid;
	pn.pstb.gid = stb->st_gid;
	pn.pstb.size = stb->st_size;
	PFL_STB_ATIME_GET(stb, &pn.pstb.atim.tv_sec,
	    &pn.pstb.atim.tv_nsec);
	PFL_STB_MTIME_GET(stb, &pn.pstb.mtim.tv_sec,
	    &pn.pstb.mtim.tv_nsec);

	iov[0].iov_base = &pn;
	iov[0].iov_len = sizeof(pn);

	iov[1].iov_base = (void *)fn;
	iov[1].iov_len = strlen(fn) + 1;

	st = stream_get();
	stream_sendv(st, OPC_PUTNAME, iov, nitems(iov));
	stream_release(st);
}

#define LASTFIELDLEN(h, type) ((h)->msglen - sizeof(type))

void
rpc_handle_getfile_req(struct hdr *h, void *buf)
{
	struct rpc_getfile_req *gfq = buf;
	struct rpc_getfile_rep gfp;
	struct stream *st;
	size_t end;

	end = LASTFIELDLEN(h, struct rpc_getfile_req);
	// assert(end > 0)
	gfq->fn[end - 1] = '\0';

	gfp.rc = pfl_filewalk(gfq->fn, gfq->flags &
	    RPC_GETFILE_F_RECURSE ? PFL_FILEWALKF_RECURSIVE : 0,
	    NULL, walk_cb, NULL);

	st = stream_get();
	stream_send(st, OPC_GETFILE_REP, &gfp, sizeof(gfp));
	stream_release(st);
}

void
rpc_handle_getfile_rep(struct hdr *h, void *buf)
{
	struct rpc_getfile_rep *gfp = buf;
	size_t end;

	(void)h;
	(void)gfp;
	(void)end;
}

void
rpc_handle_putdata(struct hdr *h, void *buf)
{
	struct rpc_putdata *pd = buf;
	ssize_t rc;
	int fd;

#if 0
	if (h->msglen == 0 ||
	    h->msglen > MAX_BUFSZ) {
		psclog_warn("invalid msglen");
		return;
	}
#endif

	fd = fcache_search(pd->fid);
	rc = pwrite(fd, pd->data, h->msglen, pd->off);
	if (rc != h->msglen)
		err(1, "write");
}

void
rpc_handle_checkzero(struct hdr *h, void *buf)
{
	struct rpc_checkzero_req *czq = buf;
	struct rpc_checkzero_rep czp;
	struct stream *st;
	struct buf *bp;
	ssize_t rc;
	int fd;

#if 0
	if (czq->len == 0 ||
	    czq->len > MAX_BUFSZ)
		PFL_GOTOERR(out, czp.rc = EINVAL);
#endif

	bp = buf_get(czq->len);

	fd = fcache_search(czq->fid);
	rc = pread(fd, bp->buf, czq->len, czq->off);
	if (rc == -1)
		err(1, "read");
	if ((uint64_t)rc != czq->len)
		warnx("read: short I/O");
	czp.rc = pfl_memchk(bp->buf, 0, rc);

	buf_release(bp);

	st = stream_get();
	stream_sendx(st, h->xid, OPC_CHECKZERO_REP, &czp, sizeof(czp));
	stream_release(st);
}

void
rpc_handle_getcksum(struct hdr *h, void *buf)
{
	struct buf *bp;
	struct rpc_getcksum_req *gcq = buf;
	struct rpc_getcksum_rep gcp;
	struct stream *st;
	gcry_error_t gerr;
	gcry_md_hd_t hd;
	ssize_t rc;
	int fd;

#if 0
	if (czq->len == 0 ||
	    czq->len > MAX_BUFSZ)
		PFL_GOTOERR(out, czp.rc = EINVAL);
#endif

	bp = buf_get(gcq->len);

	fd = fcache_search(gcq->fid);
	rc = pread(fd, bp->buf, gcq->len, gcq->off);
	if (rc == -1)
		err(1, "read");
	if ((uint64_t)rc != gcq->len)
		warnx("read: short I/O");

	gerr = gcry_md_open(&hd, GCRY_MD_SHA256, 0);
	if (gerr)
		errx(1, "gcry_md_open: error=%d", gerr);
	gcry_md_write(hd, bp->buf, rc);
	memcpy(gcry_md_read(hd, 0), gcp.digest, ALGLEN);
	gcry_md_close(hd);

	buf_release(bp);

	st = stream_get();
	stream_sendx(st, h->xid, OPC_GETCKSUM_REP, &gcp, sizeof(gcp));
	stream_release(st);
}

/*
 * Apply substitution on filename received.
 */
const char *
userfn_subst(uint64_t xid, const char *fn)
{
	struct xid_mapping *xm;

	/* if there is a direct substitution for this xid, use it */
	xm = psc_hashtbl_search(&xmcache, NULL, NULL, &xid);
	if (xm)
		return (xm->fn);

	return (fn);
}

void
rpc_handle_putname(struct hdr *h, void *buf)
{
	struct rpc_putname *pn = buf;
	struct timespec ts[2];
	struct timeval tv[2];
	char objfn[PATH_MAX];
	const char *ufn;
	int fd = -1;

	/* apply incoming name substitutions */
	ufn = userfn_subst(h->xid, pn->fn);

	if (S_ISCHR(pn->pstb.mode) ||
	    S_ISBLK(pn->pstb.mode)) {
		if (mknod(ufn, pn->pstb.mode, pn->pstb.rdev) == -1) {
			psclog_warn("mknod %s", ufn);
			return;
		}
	} else if (S_ISDIR(pn->pstb.mode)) {
		if (mkdir(ufn, pn->pstb.mode) == -1) {
			psclog_warn("mkdir %s", ufn);
			return;
		}
	} else if (S_ISFIFO(pn->pstb.mode)) {
		if (mkfifo(ufn, pn->pstb.mode) == -1) {
			psclog_warn("mkfifo %s", ufn);
			return;
		}
	} else if (S_ISLNK(pn->pstb.mode)) {
		if (symlink(objfn, ufn) == -1) {
			psclog_warn("symlink %s", ufn);
			return;
		}
	} else if (S_ISSOCK(pn->pstb.mode)) {
		struct sockaddr_un sun;

		fd = socket(AF_LOCAL, SOCK_STREAM, PF_UNSPEC);
		if (fd == -1) {
			psclog_warn("socket %s", ufn);
			return;
		}
		memset(&sun, 0, sizeof(sun));
		sun.sun_family = AF_LOCAL;
		strlcpy(sun.sun_path, ufn, sizeof(sun.sun_path));
		SOCKADDR_SETLEN(&sun);
		if (bind(fd, (struct sockaddr *)&sun,
		    sizeof(sun)) == -1) {
			close(fd);
			psclog_warn("bind %s", ufn);
			return;
		}
		close(fd);
		fd = -1;
	} else if (S_ISREG(pn->pstb.mode)) {
		objns_makepath(objfn, pn->fid);
		fd = open(objfn, O_CREAT | O_RDWR, 0600);
		if (fd == -1) {
			psclog_warn("open %s", ufn);
			return;
		}
		if (link(objfn, ufn) == -1) {
			psclog_warn("chown %s", ufn);
			return;
		}
		if (ftruncate(fd, pn->pstb.size) == -1)
			psclog_warn("chown %s", ufn);

		fcache_insert(pn->fid, fd);
	}

#ifdef HAVE_FUTIMENS
	(void)tv;
	ts[0].tv_sec = pn->pstb.atim.tv_sec;
	ts[0].tv_nsec = pn->pstb.atim.tv_nsec;

	ts[1].tv_sec = pn->pstb.mtim.tv_nsec;
	ts[1].tv_nsec = pn->pstb.mtim.tv_nsec;
#else
	(void)ts;
	tv[0].tv_sec = pn->pstb.atim.tv_sec;
	tv[0].tv_usec = pn->pstb.atim.tv_nsec / 1000;

	tv[1].tv_sec = pn->pstb.atim.tv_sec;
	tv[1].tv_usec = pn->pstb.atim.tv_nsec / 1000;
#endif

	/* BSD file flags */
	/* MacOS setattrlist */
	/* linux file attributes: FS_IOC_GETFLAGS */
	/* extattr */

	if (fd == -1) {
		if (lchown(ufn, pn->pstb.uid, pn->pstb.gid) == -1)
			psclog_warn("chown %s", ufn);
		if (lchmod(ufn, pn->pstb.mode) == -1)
			psclog_warn("chmod %s", ufn);

#ifdef HAVE_FUTIMENS
		if (utimensat(AT_FDCWD, ufn, ts,
		    AT_SYMLINK_NOFOLLOW) == -1)
			psclog_warn("utimens %s", ufn);
#else
		if (lutimes(ufn, tv) == -1)
			psclog_warn("utimes %s", ufn);
#endif

	} else {
		if (fchown(fd, pn->pstb.uid, pn->pstb.gid) == -1)
			psclog_warn("chown %s", ufn);
		if (fchmod(fd, pn->pstb.mode) == -1)
			psclog_warn("chmod %s", ufn);

#ifdef HAVE_FUTIMENS
		struct timespec ts[2];

		if (futimens(fd, ts) == -1)
			psclog_warn("utimens %s", ufn);
#else
		struct timeval tv[2];

		if (futimes(fd, tv) == -1)
			psclog_warn("utimes %s", ufn);
#endif
	}
}

void
rpc_handle_ctl(struct hdr *h, void *buf)
{
	struct rpc_ctl_req *cq = buf;
	struct rpc_ctl_rep *cp;

	(void)cq;
	(void)cp;
	(void)h;
	(void)buf;
}

typedef void (*op_handler_t)(struct hdr *, void *);

op_handler_t ops[] = {
	rpc_handle_getfile_req,
	rpc_handle_getfile_rep,
	rpc_handle_putdata,
	rpc_handle_checkzero,
	rpc_handle_getcksum,
	rpc_handle_putname,
	rpc_handle_ctl
};

void
handle_signal(__unusedx int sig)
{
	exit_from_signal = 1;
}

void
recvthr_main(struct psc_thread *thr)
{
	struct recvthr *rt;
	uint32_t bufsz = 0;
	struct hdr hdr;
	ssize_t rc;
	void *buf;

warnx("RECV");
	rt = thr->pscthr_private;
	while (pscthr_run(thr)) {
warnx("WAIT RECV %d", rt->st->rfd);
		rc = atomicio_read(rt->st->rfd, &hdr, sizeof(hdr));
warnx("recv %zd", rc);
		if (rc == 0)
			break;

		if (hdr.msglen > bufsz) {
			if (hdr.msglen > MAX_BUFSZ)
				errx(1, "invalid bufsz received from "
				    "peer: %u", hdr.msglen);
			bufsz = hdr.msglen;
			buf = realloc(buf, bufsz);
			if (buf == NULL)
				err(1, NULL);
		}
		if (hdr.opc >= nitems(ops))
			errx(1, "invalid opcode received from "
			    "peer: %u", hdr.opc);
		atomicio_read(rt->st->rfd, buf, hdr.msglen);

		if (exit_from_signal)
			break;

		ops[hdr.opc](&hdr, buf);

		if (exit_from_signal)
			break;
	}
}
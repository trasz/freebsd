/*-
 * Copyright (c) 2014 The FreeBSD Foundation
 * All rights reserved.
 *
 * This software was developed by Edward Tomasz Napierala under sponsorship
 * from the FreeBSD Foundation.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 * $FreeBSD$
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/buf.h>
#include <sys/conf.h>
#include <sys/dirent.h>
#include <sys/ioccom.h>
#include <sys/kernel.h>
#include <sys/module.h>
#include <sys/mount.h>
#include <sys/sx.h>
#include <sys/sysctl.h>
#include <sys/vnode.h>
#include <vm/uma.h>

#include "autofs.h"
#include "autofs_ioctl.h"

MALLOC_DEFINE(M_AUTOFS, "autofs", "Automounter filesystem");

uma_zone_t autofs_request_zone;
uma_zone_t autofs_node_zone;

static int	autofs_open(struct cdev *dev, int flags, int fmt,
		    struct thread *td);
static int	autofs_close(struct cdev *dev, int flag, int fmt,
		    struct thread *td);
static int	autofs_ioctl(struct cdev *dev, u_long cmd, caddr_t arg,
		    int mode, struct thread *td);

static struct cdevsw autofs_cdevsw = {
     .d_version = D_VERSION,
     .d_open   = autofs_open,
     .d_close   = autofs_close,
     .d_ioctl   = autofs_ioctl,
     .d_name    = "autofs",
};

struct autofs_softc	*sc;

SYSCTL_NODE(_vfs, OID_AUTO, autofs, CTLFLAG_RD, 0, "Automounter filesystem");
int autofs_debug = 2;
TUNABLE_INT("vfs.autofs.debug", &autofs_debug);
SYSCTL_INT(_vfs_autofs, OID_AUTO, autofs_debug, CTLFLAG_RW,
    &autofs_debug, 2, "Enable debug messages");

int
autofs_init(struct vfsconf *vfsp)
{
	int error;

	sc = malloc(sizeof(*sc), M_AUTOFS, M_WAITOK | M_ZERO);

	error = make_dev_p(MAKEDEV_CHECKNAME, &sc->sc_cdev, &autofs_cdevsw,
	    NULL, UID_ROOT, GID_WHEEL, 0600, "autofs");
	if (error != 0) {
		AUTOFS_WARN("failed to create device node, error %d", error);
		free(sc, M_AUTOFS);
		return (error);
	}
	sc->sc_cdev->si_drv1 = sc;

	autofs_request_zone = uma_zcreate("autofs_request",
	    sizeof(struct autofs_request), NULL, NULL, NULL, NULL,
	    UMA_ALIGN_PTR, 0);
	autofs_node_zone = uma_zcreate("autofs_node",
	    sizeof(struct autofs_node), NULL, NULL, NULL, NULL,
	    UMA_ALIGN_PTR, 0);

	TAILQ_INIT(&sc->sc_mounts);
	TAILQ_INIT(&sc->sc_requests);
	cv_init(&sc->sc_cv, "autofs_cv");
	sx_init(&sc->sc_lock, "autofs_lock");

	return (0);
}

int
autofs_uninit(struct vfsconf *vfsp)
{

	if (sc->sc_cdev != NULL) {
		//AUTOFS_DEBUG("removing device node");
		destroy_dev(sc->sc_cdev);
		//AUTOFS_DEBUG("device node removed");
	}

	uma_zdestroy(autofs_request_zone);
	uma_zdestroy(autofs_node_zone);

	free(sc, M_AUTOFS);
	return (0);
}

bool
autofs_ignore_thread(const struct thread *td)
{
	const struct proc *p;

	if (sc->sc_dev_opened == false)
		return (false);

	p = td->td_proc;

	sx_slock(&proctree_lock);
	/*
	 * XXX: There was something wrong about iterating this way; ask kib@.
	 */
	for (; p != NULL; p = p->p_pptr) {
		if (p->p_pid == sc->sc_dev_pid) {
			sx_sunlock(&proctree_lock);

			return (true);
		}
	}
	sx_sunlock(&proctree_lock);

	return (false);
}

static int
autofs_ioctl_request(struct autofs_softc *sc, struct autofs_daemon_request *adr)
{
	struct autofs_request *ar;
	int error;

	sx_xlock(&sc->sc_lock);
	for (;;) {
		TAILQ_FOREACH(ar, &sc->sc_requests, ar_next) {
			if (ar->ar_done)
				continue;
			if (ar->ar_in_progress)
				continue;

			break;
		}

		if (ar != NULL)
			break;

		error = cv_wait_sig(&sc->sc_cv, &sc->sc_lock);
		if (error != 0) {
			sx_xunlock(&sc->sc_lock);
			return (error);
		}
	}

	ar->ar_in_progress = true;
	sx_xunlock(&sc->sc_lock);

	adr->adr_id = ar->ar_id;
	strlcpy(adr->adr_from, ar->ar_from, sizeof(adr->adr_from));
	strlcpy(adr->adr_mountpoint, ar->ar_mountpoint, sizeof(adr->adr_mountpoint));
	strlcpy(adr->adr_path, ar->ar_path, sizeof(adr->adr_path));
	strlcpy(adr->adr_options, ar->ar_options, sizeof(adr->adr_options));

	return (0);
}

static int
autofs_ioctl_done(struct autofs_softc *sc, struct autofs_daemon_done *add)
{
	struct autofs_request *ar;

	sx_xlock(&sc->sc_lock);
	TAILQ_FOREACH(ar, &sc->sc_requests, ar_next) {
		if (ar->ar_id == add->add_id)
			break;
	}

	if (ar == NULL) {
		sx_xunlock(&sc->sc_lock);
		return (ESRCH);
	}

	ar->ar_done = true;
	ar->ar_in_progress = false;
	cv_signal(&sc->sc_cv);

	sx_xunlock(&sc->sc_lock);

	return (0);
}

static int
autofs_open(struct cdev *dev, int flags, int fmt, struct thread *td)
{

	if (sc->sc_dev_opened)
		return (EBUSY);

	sc->sc_dev_pid = td->td_proc->p_pid;
	sc->sc_dev_opened = true;

	//AUTOFS_DEBUG("open, pid %d", sc->sc_dev_pid);

	return (0);
}


static int
autofs_close(struct cdev *dev, int flag, int fmt, struct thread *td)
{

	KASSERT(sc->sc_dev_opened, ("not opened?"));

	sc->sc_dev_opened = false;
	//AUTOFS_DEBUG("last close");

	return (0);
}

static int
autofs_ioctl(struct cdev *dev, u_long cmd, caddr_t arg, int mode,
    struct thread *td)
{
	//struct autofs_softc *sc = dev->si_drv1;

	KASSERT(sc->sc_dev_opened, ("not opened?"));

	switch (cmd) {
	case AUTOFSREQUEST:
		return (autofs_ioctl_request(sc,
		    (struct autofs_daemon_request *)arg));
	case AUTOFSDONE:
		return (autofs_ioctl_done(sc,
		    (struct autofs_daemon_done *)arg));
	default:
		AUTOFS_DEBUG("invalid cmd %lx", cmd);
		return (EINVAL);
	}
}

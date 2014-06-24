/*-
 * Copyright (c) 2013 The FreeBSD Foundation
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

#ifndef AUTOFS_IOCTL_H
#define	AUTOFS_IOCTL_H

#define	AUTOFS_PATH		"/dev/autofs"

struct autofs_daemon_request {
	/*
	 * Request identifier.
	 */
	int		adr_id;

	/*
	 * The "from" field, containing map name.  For example,
	 * when accessing '/net/192.168.1.3/tank/vm/', that would
	 * be '/net'.
	 */
	char		adr_from[MAXPATHLEN];

	/*
	 * Prefix, which is basically the mountpoint from auto_master(5).
	 * In example above that would be "/net"; for direct maps it's "/".
	 */
	char		adr_prefix[MAXPATHLEN];

	/*
	 * Path the autofs is mounted on.
	 */
	char		adr_mountpoint[MAXPATHLEN];

	/*
	 * Key used as argument for dynamic maps; in example above that would
	 * be '192.168.1.3'.
	 */
	char		adr_key[MAXPATHLEN];

	/*
	 * Path "under" the mountpoint; in example above that would
	 * be '/tank/vm'.
	 */
	char		adr_path[MAXPATHLEN];

	/*
	 * Mount options from auto_master(5).
	 */
	char		adr_options[MAXPATHLEN];
};

struct autofs_daemon_done {
	/*
	 * Identifier, copied from adr_id.
	 */
	int		add_id;
};

#define	AUTOFSREQUEST	_IOR('I', 0x01, struct autofs_daemon_request)
#define	AUTOFSDONE	_IOW('I', 0x02, struct autofs_daemon_done)

#endif /* !AUTOFS_IOCTL_H */

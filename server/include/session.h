#ifndef _SESSION_H
#define _SESSION_H
/*
 * This file is distributed as part of the SkySQL Gateway.  It is free
 * software: you can redistribute it and/or modify it under the terms of the
 * GNU General Public License as published by the Free Software Foundation,
 * version 2.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more
 * details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program; if not, write to the Free Software Foundation, Inc., 51
 * Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * Copyright SkySQL Ab 2013
 */

/**
 * @file session.h
 *
 * @verbatim
 * Revision History
 *
 * Date		Who			Description
 * 01-06-2013	Mark Riddoch		Initial implementation
 * 14-06-2013	Massimiliano Pinto	Added void *data to session
 *					for session specific data
 * 01-07-2013	Massimiliano Pinto	Removed backends pointer
 *					from struct session
 * 02-09-2013	Massimiliano Pinto	Added session ref counter
 *
 * @endverbatim
 */
#define SES_CMD

#include <time.h>
#include <atomic.h>
#include <spinlock.h>
#if defined(SES_CMD)
#include <buffer.h>
#endif /* SES_CMD */
#include <skygw_utils.h>

struct dcb;
struct service;

/**
 * The session statistics structure
 */
typedef struct {
	time_t		connect;	/**< Time when the session was started */
} SESSION_STATS;


typedef enum {
        SESSION_STATE_ALLOC,
        SESSION_STATE_READY,
        SESSION_STATE_LISTENER,
        SESSION_STATE_LISTENER_STOPPED,
        SESSION_STATE_FREE
} session_state_t;

typedef struct session SESSION;

#if defined(SES_CMD)
typedef enum ses_cmd_state_t {
        SES_CMD_INIT = 0,
        SES_CMD_SAVED,
        SES_CMD_SENDING,
        SES_CMD_REPLIED_WHILE_SENDING,
        SES_CMD_SENT,
        SES_CMD_SENT_AND_REPLIED
} ses_cmd_state_t;

typedef enum ses_cmd_event_t {
        SES_CMD_EV_SENT = 0,
        SES_CMD_EV_REPLIED
} ses_cmd_event_t;

typedef struct ses_command_st ses_command_t;

struct ses_command_st {
#if defined(SS_DEBUG)
        skygw_chk_t     ses_cmd_chk_top;
#endif
        SPINLOCK        ses_cmd_lock;
        GWBUF*          ses_cmd_buf;
        SESSION*        ses_cmd_session;
        ses_command_t*  ses_cmd_next;
        ses_cmd_state_t ses_cmd_state;
#if defined(SS_DEBUG)
        skygw_chk_t     ses_cmd_chk_tail;
#endif
};
#endif /* SES_CMD */

/**
 * The session status block
 *
 * A session status block is created for each user (client) connection
 * to the database, it links the descriptors, routing implementation
 * and originating service together for the client session.
 */
struct session {
#if defined(SS_DEBUG)
        skygw_chk_t     ses_chk_top;
#endif
        SPINLOCK        ses_lock;
	session_state_t state;		  /**< Current descriptor state */
	struct dcb	*client;	  /**< The client connection */
	void 		*data;		  /**< The session data */
	void		*router_session;  /**< The router instance data */
	SESSION_STATS	stats;		  /**< Session statistics */
	struct service	*service;	  /**< The service this session is using */
	struct session	*next;		  /**< Linked list of all sessions */
	int		refcount;	  /**< Reference count on the session */
#if defined(SES_CMD)
        ses_command_t   *ses_sesvar_cmds; /**< list of executed session variable cmds */
#endif
#if defined(SS_DEBUG)
        skygw_chk_t     ses_chk_tail;
#endif
};

#define SESSION_PROTOCOL(x, type)	DCB_PROTOCOL((x)->client, type)

SESSION	*session_alloc(struct service *, struct dcb *);
bool    session_free(SESSION *);
void	printAllSessions();
void	printSession(SESSION *);
void	dprintAllSessions(struct dcb *);
void	dprintSession(struct dcb *, SESSION *);
char	*session_state(int);
bool	session_link_dcb(SESSION *, struct dcb *);

#if defined(SES_CMD)
/** Allocate memory for session command obj */
ses_command_t*  ses_command_init(SESSION* ses, GWBUF* cmdstr);
ses_cmd_state_t ses_sescmd_get_next_state(ses_cmd_state_t curr_state);
bool            ses_sescmd_proceed(ses_command_t* sescmd); /*< Move sescmd to next state */
bool            ses_sescmd_proceed_nolock(ses_command_t* sescmd); /*< Same w/o lock */
bool            ses_sescmd_proceed_to_nolock(ses_command_t* sescmd, ses_cmd_state_t next);
void            ses_add_sescmd(ses_command_t* sescmd); /*< Add cmd to session's sescmd list */
void            ses_sescmd_lock(ses_command_t* sescmd);
void            ses_sescmd_unlock(ses_command_t* sescmd);
bool            ses_sescmd_proceed_due(ses_command_t* sc, ses_cmd_event_t ev);
bool            ses_sescmd_proceed_due_nolock(ses_command_t* sc, ses_cmd_event_t ev);

#endif /* SES_CMD */

#endif

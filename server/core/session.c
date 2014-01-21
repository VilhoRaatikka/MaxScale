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
 * @file session.c  - A representation of the session within the gateway.
 *
 * @verbatim
 * Revision History
 *
 * Date		Who			Description
 * 17/06/13	Mark Riddoch		Initial implementation
 * 02/09/13	Massimiliano Pinto	Added session refcounter
 *
 * @endverbatim
 */
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <session.h>
#include <service.h>
#include <router.h>
#include <dcb.h>
#include <spinlock.h>
#include <atomic.h>
#include <skygw_utils.h>
#include <skygw_debug.h>
#include <log_manager.h>

extern int lm_enabled_logfiles_bitmask;

static SPINLOCK	session_spin = SPINLOCK_INIT;
static SESSION	*allSessions = NULL;

#if defined(SES_CMD)
static void ses_sescmds_free(SESSION* session);
#endif

/**
 * Allocate a new session for a new client of the specified service.
 *
 * Create the link to the router session by calling the newSession
 * entry point of the router using the router instance of the
 * service this session is part of.
 *
 * @param service	The service this connection was established by
 * @param client_dcb	The client side DCB
 * @return		The newly created session or NULL if an error occured
 */
SESSION *
session_alloc(SERVICE *service, DCB *client_dcb)
{
        SESSION 	*session;

        session = (SESSION *)calloc(1, sizeof(SESSION));
        ss_info_dassert(session != NULL,
                        "Allocating memory for session failed.");
        
        if (session == NULL) {
                int eno = errno;
                errno = 0;
                LOGIF(LE, (skygw_log_write_flush(
                        LOGFILE_ERROR,
                        "Error : Failed to allocate memory for "
                        "session object due error %d, %s.",
                        eno,
                        strerror(eno))));
		goto return_session;
        }
#if defined(SS_DEBUG)
        session->ses_chk_top = CHK_NUM_SESSION;
        session->ses_chk_tail = CHK_NUM_SESSION;
#endif
        spinlock_init(&session->ses_lock);
        /*<
         * Prevent backend threads from accessing before session is completely
         * initialized.
         */
        spinlock_acquire(&session->ses_lock);
        session->service = service;
	session->client = client_dcb;
	memset(&session->stats, 0, sizeof(SESSION_STATS));
	session->stats.connect = time(0);
	session->state = SESSION_STATE_ALLOC;
        /*<
	 * Associate the session to the client DCB and set the reference count on
	 * the session to indicate that there is a single reference to the
         * session. There is no need to protect this or use atomic add as the
         * session has not been made available to the other threads at this
         * point.
         */
        session->data = client_dcb->data;
	client_dcb->session = session;
	session->refcount = 1;
        /*<
         * This indicates that session is ready to be shared with backend
         * DCBs.
         */
        session->state = SESSION_STATE_READY;
        
        /*< Release session lock */
        spinlock_release(&session->ses_lock);

	/*
	 * Only create a router session if we are not the listening 
	 * DCB. Creating a router session may create a connection to a
	 * backend server, depending upon the router module implementation
	 * and should be avoided for the listener session
	 *
	 * Router session creation may create other DCBs that link to the
	 * session, therefore it is important that the session lock is
         * relinquished beforethe router call.
	 */
	if (client_dcb->state != DCB_STATE_LISTENING)
	{
		session->router_session =
                    service->router->newSession(service->router_instance,
                                                session);
	
                if (session->router_session == NULL) {
                        /*<
                         * Decrease refcount, set dcb's session pointer NULL
                         * and set session pointer to NULL.
                         */
                        session_free(session);
                        client_dcb->session = NULL;
                        session = NULL;
                        LOGIF(LE, (skygw_log_write_flush(
                                LOGFILE_ERROR,
                                "Error : Failed to create router "
                                "client session. Freeing allocated resources.")));
                        
                        goto return_session;
                }
        }
	spinlock_acquire(&session_spin);
	session->next = allSessions;
	allSessions = session;
	spinlock_release(&session_spin);
	atomic_add(&service->stats.n_sessions, 1);
	atomic_add(&service->stats.n_current, 1);
        CHK_SESSION(session);
        
return_session:
	return session;
}

/**
 * Link a session to a DCB.
 *
 * @param session	The session to link with the dcb
 * @param dcb		The DCB to be linked
 * @return		True if the session was sucessfully linked to the DCB
 */
bool
session_link_dcb(SESSION *session, DCB *dcb)
{
	spinlock_acquire(&session->ses_lock);
        ss_info_dassert(session->state != SESSION_STATE_FREE,
            "If session->state is SESSION_STATE_FREE then this attempt to "
            "access freed memory block.");
	if (session->state == SESSION_STATE_FREE)
	{
		spinlock_release(&session->ses_lock);
		return false;
	}
	atomic_add(&session->refcount, 1);
	dcb->session = session;
	spinlock_release(&session->ses_lock);
	return true;
}

int session_unlink_dcb(
        SESSION* session,
        DCB*     dcb)
{
        int nlink;
        
        CHK_SESSION(session);
        
	spinlock_acquire(&session->ses_lock);
        ss_dassert(session->refcount > 0);
        /*<
         * Remove dcb from session's router_client_session.
         */
        nlink = atomic_add(&session->refcount, -1);
        nlink -= 1;

        if (nlink == 0)
	{
                session->state = SESSION_STATE_FREE;
        }

        if (dcb != NULL)
        {
                 dcb->session = NULL;
        }
        spinlock_release(&session->ses_lock);
        
        return nlink;
}

/**
 * Deallocate the specified session
 *
 * @param session	The session to deallocate
 */
bool session_free(
        SESSION *session)
{
        bool    succp = false;
        SESSION *ptr;
        int     nlink;

        CHK_SESSION(session);

        /*<
         * Remove one reference. If there are no references left,
         * free session.
         */
        nlink = session_unlink_dcb(session, NULL);

        if (nlink != 0) {
                ss_dassert(nlink > 0);
                goto return_succp;
        }
        
	/* First of all remove from the linked list */
	spinlock_acquire(&session_spin);
	if (allSessions == session)
	{
		allSessions = session->next;
	}
	else
	{
		ptr = allSessions;
		while (ptr && ptr->next != session)
		{
			ptr = ptr->next;
		}
		if (ptr)
			ptr->next = session->next;
	}
	spinlock_release(&session_spin);
	atomic_add(&session->service->stats.n_current, -1);

	/* Free router_session and session */
        if (session->router_session) {
                session->service->router->freeSession(
                        session->service->router_instance,
                        session->router_session);
        }
#if defined(SES_CMD)
        ses_sescmds_free(session);
#endif
	free(session);
        succp = true;
        
return_succp :
        return succp;
}

/**
 * Print details of an individual session
 *
 * @param session	Session to print
 */
void
printSession(SESSION *session)
{
	printf("Session %p\n", session);
	printf("\tState:    	%s\n", session_state(session->state));
	printf("\tService:	%s (%p)\n", session->service->name, session->service);
	printf("\tClient DCB:	%p\n", session->client);
	printf("\tConnected:	%s", asctime(localtime(&session->stats.connect)));
}

/**
 * Print all sessions
 *
 * Designed to be called within a debugger session in order
 * to display all active sessions within the gateway
 */
void
printAllSessions()
{
SESSION	*ptr;

	spinlock_acquire(&session_spin);
	ptr = allSessions;
	while (ptr)
	{
		printSession(ptr);
		ptr = ptr->next;
	}
	spinlock_release(&session_spin);
}


/**
 * Check sessions
 *
 * Designed to be called within a debugger session in order
 * to display information regarding "interesting" sessions
 */
void
CheckSessions()
{
SESSION	*ptr;
int	noclients = 0;
int	norouter = 0;

	spinlock_acquire(&session_spin);
	ptr = allSessions;
	while (ptr)
	{
		if (ptr->state != SESSION_STATE_LISTENER ||
				ptr->state != SESSION_STATE_LISTENER_STOPPED)
		{
			if (ptr->client == NULL && ptr->refcount)
			{
				if (noclients == 0)
				{
					printf("Sessions without a client DCB.\n");
					printf("==============================\n");
				}
				printSession(ptr);
				noclients++;
			}
		}
		ptr = ptr->next;
	}
	spinlock_release(&session_spin);
	if (noclients)
		printf("%d Sessions have no clients\n", noclients);
	spinlock_acquire(&session_spin);
	ptr = allSessions;
	while (ptr)
	{
		if (ptr->state != SESSION_STATE_LISTENER ||
				ptr->state != SESSION_STATE_LISTENER_STOPPED)
		{
			if (ptr->router_session == NULL && ptr->refcount)
			{
				if (norouter == 0)
				{
					printf("Sessions without a router session.\n");
					printf("==================================\n");
				}
				printSession(ptr);
				norouter++;
			}
		}
		ptr = ptr->next;
	}
	spinlock_release(&session_spin);
	if (norouter)
		printf("%d Sessions have no router session\n", norouter);
}
/**
 * Print all sessions to a DCB
 *
 * Designed to be called within a debugger session in order
 * to display all active sessions within the gateway
 *
 * @param dcb	The DCB to print to
 */
void
dprintAllSessions(DCB *dcb)
{
SESSION	*ptr;

	spinlock_acquire(&session_spin);
	ptr = allSessions;
	while (ptr)
	{
		dcb_printf(dcb, "Session %p\n", ptr);
		dcb_printf(dcb, "\tState:    		%s\n", session_state(ptr->state));
		dcb_printf(dcb, "\tService:		%s (%p)\n", ptr->service->name, ptr->service);
		dcb_printf(dcb, "\tClient DCB:		%p\n", ptr->client);
		if (ptr->client && ptr->client->remote)
			dcb_printf(dcb, "\tClient Address:		%s\n", ptr->client->remote);
		dcb_printf(dcb, "\tConnected:		%s", asctime(localtime(&ptr->stats.connect)));
		ptr = ptr->next;
	}
	spinlock_release(&session_spin);
}

/**
 * Print a particular session to a DCB
 *
 * Designed to be called within a debugger session in order
 * to display all active sessions within the gateway
 *
 * @param dcb	The DCB to print to
 * @param ptr	The session to print
 */
void
dprintSession(DCB *dcb, SESSION *ptr)
{
	dcb_printf(dcb, "Session %p\n", ptr);
	dcb_printf(dcb, "\tState:    		%s\n", session_state(ptr->state));
	dcb_printf(dcb, "\tService:		%s (%p)\n", ptr->service->name, ptr->service);
	dcb_printf(dcb, "\tClient DCB:		%p\n", ptr->client);
	if (ptr->client && ptr->client->remote)
		dcb_printf(dcb, "\tClient Address:		%s\n", ptr->client->remote);
	dcb_printf(dcb, "\tConnected:		%s", asctime(localtime(&ptr->stats.connect)));
}

/**
 * Convert a session state to a string representation
 *
 * @param state		The session state
 * @return A string representation of the session state
 */
char *
session_state(int state)
{
	switch (state)
	{
	case SESSION_STATE_ALLOC:
		return "Session Allocated";
	case SESSION_STATE_READY:
		return "Session Ready";
	case SESSION_STATE_LISTENER:
		return "Listener Session";
	case SESSION_STATE_LISTENER_STOPPED:
		return "Stopped Listener Session";
	default:
		return "Invalid State";
	}
}

/** 
 * @node Initialize session variable command structure 
 *
 * Parameters:
 * @param cmdstr - pointer to command string of type GWBUF
 *         
 *
 * @return Initialized session variable command structure or NULL in
 * case of error.
 *
 * 
 * @details Memory allocation is done with calloc but could be optimized
 * by using small preallocated set of memory chunks and recycling them.
 * It is not very likely that different set commands are executed
 * in large scale. Redundant autocommit=xxx calls can be reduced to one, which
 * makes it possible to recycle buffers.
 *
 */
#if defined(SES_CMD)
ses_command_t* ses_command_init(
        SESSION* ses,
        GWBUF*   cmdstr)
{
        ses_command_t* sc = NULL;
        CHK_SESSION(ses);
        
        if (cmdstr == NULL || ses == NULL)
        {
                LOGIF(LE, (skygw_log_write_flush(
                                   LOGFILE_ERROR,
                                   (ses == NULL ?
                                    "Error : Session pointer is NULL." :
                                    "Error : Session command pointer is NULL."))));
                goto return_sc;
        }
        
        CHK_GWBUF(cmdstr);
        sc = (ses_command_t*)calloc(1, sizeof(ses_command_t));
        
        if (sc == NULL)
        {
                LOGIF(LE, (skygw_log_write_flush(
                                   LOGFILE_ERROR,
                                   "Error : Failed to allocate memory for "
                                   "session command object.")));
                goto return_sc;
        }
#if defined(SS_DEBUG)
        sc->ses_cmd_chk_top  = CHK_NUM_SES_CMD;
        sc->ses_cmd_chk_tail = CHK_NUM_SES_CMD;
#endif
        sc->ses_cmd_state = SES_CMD_INIT;
        sc->ses_cmd_session = ses;
        spinlock_init(&sc->ses_cmd_lock);
        sc->ses_cmd_buf = cmdstr;
        CHK_SES_CMD(sc);
return_sc:
        return sc;
}

/** 
 * @node Link session variable command to sessions list of them 
 *
 * Parameters:
 * @param sc command struct
 *
 * @return void
 *
 */
void ses_add_sescmd(
        ses_command_t* sc)
{
        ses_command_t** p_sc = &sc->ses_cmd_session->ses_sesvar_cmds;
        
        CHK_SES_CMD(sc);
        CHK_SESSION(sc->ses_cmd_session);
        
        spinlock_acquire(&sc->ses_cmd_session->ses_lock);
        if (*p_sc == NULL)
        {
                *p_sc = sc;
        }
        else
        {
                while ((*p_sc)->ses_cmd_next != NULL)
                {
                        *p_sc = (*p_sc)->ses_cmd_next;
                }
                (*p_sc)->ses_cmd_next = sc;
        }
        sc->ses_cmd_state = SES_CMD_SAVED;
        spinlock_release(&sc->ses_cmd_session->ses_lock);
}


/** 
 * @node Frees session variable command structures which
 * are stored to session struct.
 *
 * @return void
 *
 * 
 * @details Go through the list of sessoin varibale commands and free
 * each of them.
 *
 */
static void ses_sescmds_free(
        SESSION* session)
{
        ses_command_t* sc; 
        ses_command_t* next;

        CHK_SESSION(session);
        sc = session->ses_sesvar_cmds;
        session->ses_sesvar_cmds = NULL;

        while (sc != NULL)
        {
                CHK_SES_CMD(sc);
                ss_dassert(sc->ses_cmd_state == SES_CMD_SENT_AND_REPLIED);
                next = sc->ses_cmd_next;
                free(sc);
                sc = next;
        }        
}

ses_cmd_state_t ses_sescmd_get_next_state(
        ses_cmd_state_t curr_state)
{
        ses_cmd_state_t new_state;
        ss_dassert(curr_state >= SES_CMD_INIT &&
                   curr_state <= SES_CMD_SENT_AND_REPLIED);

        switch (curr_state) {

        case SES_CMD_INIT:
                new_state = SES_CMD_SAVED;
                break;
                
        case SES_CMD_SAVED:
                new_state = SES_CMD_SENDING;
                break;
                
        case SES_CMD_SENDING:
                /**< Error - undeterministic transition */
                goto return_curr;
                break;
                
        case SES_CMD_SENT:
                new_state = SES_CMD_SENT_AND_REPLIED;
                break;

        case SES_CMD_REPLIED_WHILE_SENDING:
                new_state = SES_CMD_SENT_AND_REPLIED;
                break;

        default:
                /* Error, illegal state */
                goto return_curr;
                break;
        } /* switch */

        return new_state;
return_curr:
        return curr_state;
}


bool ses_sescmd_proceed_to_nolock(
        ses_command_t*  sc,
        ses_cmd_state_t next)
{
        bool             succp = false;
        ses_cmd_state_t* curr;

        curr = &sc->ses_cmd_state;

        /**
         * if next state is INIT it indicates that deterministic step can be
         * taken and the next state is determined by state machine.
         */
        if (next == SES_CMD_INIT)
        {
                next = ses_sescmd_get_next_state(*curr);
        }
        
        if (*curr == next)
        {
                goto return_succp;
        }
        *curr = next;
        succp = true;

return_succp:
        return succp;
}



bool ses_sescmd_proceed_nolock(
        ses_command_t* sc)
{
        bool succp;
        succp = ses_sescmd_proceed_to_nolock(sc, SES_CMD_INIT);
        return succp;
}

bool ses_sescmd_proceed(
        ses_command_t* sc)
{
        bool succp;

        spinlock_acquire(&sc->ses_cmd_lock);
        succp = ses_sescmd_proceed_nolock(sc);
        spinlock_release(&sc->ses_cmd_lock);
        return succp;
}

void ses_sescmd_lock(
        ses_command_t* sc)
{
        CHK_SES_CMD(sc);
        spinlock_acquire(&sc->ses_cmd_lock);
}

void ses_sescmd_unlock(
        ses_command_t* sc)
{
        CHK_SES_CMD(sc);
        spinlock_release(&sc->ses_cmd_lock);
}

bool ses_sescmd_proceed_due(
        ses_command_t*  sc,
        ses_cmd_event_t ev)
{
        bool succp;
        
        CHK_SES_CMD(sc);
        spinlock_acquire(&sc->ses_cmd_lock);

        succp = ses_sescmd_proceed_due_nolock(sc, ev);
        
        spinlock_release(&sc->ses_cmd_lock);
        return succp;
}

bool ses_sescmd_proceed_due_nolock(
        ses_command_t*  sc,
        ses_cmd_event_t ev)
{
        bool            succp = true;
        ses_cmd_state_t curr = sc->ses_cmd_state;
        
        CHK_SES_CMD(sc);

        switch (curr) {
        case SES_CMD_SENDING:
                if (ev == SES_CMD_EV_SENT)
                {
                        sc->ses_cmd_state = SES_CMD_SENT;
                }
                else if (ev == SES_CMD_EV_REPLIED)
                {
                        sc->ses_cmd_state = SES_CMD_REPLIED_WHILE_SENDING;
                }
                else
                {
                        succp = false;
                }
                break;
        default:
                sc->ses_cmd_state = ses_sescmd_get_next_state(curr);
                succp = true;
                break;
        } /* switch */
        
        return succp;
}


#endif /* SES_CMD */

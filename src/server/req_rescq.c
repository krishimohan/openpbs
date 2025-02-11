/*
 * Copyright (C) 1994-2021 Altair Engineering, Inc.
 * For more information, contact Altair at www.altair.com.
 *
 * This file is part of both the OpenPBS software ("OpenPBS")
 * and the PBS Professional ("PBS Pro") software.
 *
 * Open Source License Information:
 *
 * OpenPBS is free software. You can redistribute it and/or modify it under
 * the terms of the GNU Affero General Public License as published by the
 * Free Software Foundation, either version 3 of the License, or (at your
 * option) any later version.
 *
 * OpenPBS is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU Affero General Public
 * License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * Commercial License Information:
 *
 * PBS Pro is commercially licensed software that shares a common core with
 * the OpenPBS software.  For a copy of the commercial license terms and
 * conditions, go to: (http://www.pbspro.com/agreement.html) or contact the
 * Altair Legal Department.
 *
 * Altair's dual-license business model allows companies, individuals, and
 * organizations to create proprietary derivative works of OpenPBS and
 * distribute them - whether embedded or bundled with other software -
 * under a commercial license agreement.
 *
 * Use of Altair's trademarks, including but not limited to "PBS™",
 * "OpenPBS®", "PBS Professional®", and "PBS Pro™" and Altair's logos is
 * subject to Altair's trademark licensing policies.
 */

/*
 * @file	req_rescq.c
 *
 * @brief
 * 	req_rescq.c	-	Functions relating to the Resource Query Batch Request.
 *
 * Included functions are:
 *	resv_idle_delete()
 *	cnvrt_qmove()
 *	resv_timer_init()
 *	assign_resv_resc()
 *	req_confirmresv()
 *
 */
#include <pbs_config.h>   /* the master config generated by configure */

#include <sys/types.h>
#include "libpbs.h"
#include <ctype.h>
#include <errno.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "server_limits.h"
#include "list_link.h"
#include "attribute.h"
#include "resource.h"
#include "server.h"
#include "batch_request.h"
#include "resv_node.h"
#include "pbs_nodes.h"
#include "queue.h"
#include "job.h"
#include "reservation.h"
#include "sched_cmds.h"
#include "work_task.h"
#include "credential.h"
#include "pbs_error.h"
#include "svrfunc.h"
#include "log.h"
#include "acct.h"
#include "pbs_license.h"
#include "libutil.h"


/* forward definitions to keep the compiler happy */
struct name_and_val {
	char  *pn;
	char  *pv;
};

int  gen_task_Time4resv(resc_resv*);
void revert_alter_reservation(resc_resv *presv);

extern int     svr_totnodes;
extern time_t  time_now;

extern int cnvrt_local_move(job *, struct batch_request *);

/**
 * @brief work task to delete reservation if there are no jobs in the reservation queue
 *
 * @param[in] ptask - work task
 *
 */
void
resv_idle_delete(struct work_task *ptask)
{
	resc_resv *presv;
	int num_jobs;

	presv = ptask->wt_parm1;

	if (presv == NULL)
		return;

	num_jobs = presv->ri_qp->qu_numjobs;
	if (svr_chk_history_conf()) {
		num_jobs -= (presv->ri_qp->qu_njstate[JOB_STATE_MOVED] + presv->ri_qp->qu_njstate[JOB_STATE_FINISHED] +
			presv->ri_qp->qu_njstate[JOB_STATE_EXPIRED]);
	}

	if (num_jobs == 0) {
		log_eventf(PBSEVENT_RESV, PBS_EVENTCLASS_RESV, LOG_DEBUG, presv->ri_qs.ri_resvID,
			"Deleting reservation after being idle for %d seconds",
			get_rattr_long(presv, RESV_ATR_del_idle_time));
		gen_future_deleteResv(presv, 1);
	}
}

/**
 * @brief if there are no jobs in the reservation queue, set a timer to delete the reservation
 *
 * @param[in]	presv - pointer to reservation
 */
void
set_idle_delete_task(resc_resv *presv)
{
	struct work_task *wt;
	long retry_time;
	int num_jobs;

	if (presv == NULL)
		return;

	if (!is_rattr_set(presv, RESV_ATR_del_idle_time))
		return;

	num_jobs = presv->ri_qp->qu_numjobs;
	if (svr_chk_history_conf()) {
		num_jobs -= (presv->ri_qp->qu_njstate[JOB_STATE_MOVED] + presv->ri_qp->qu_njstate[JOB_STATE_FINISHED] +
			presv->ri_qp->qu_njstate[JOB_STATE_EXPIRED]);
	}

	if (num_jobs == 0 && presv->ri_qs.ri_state == RESV_RUNNING) {
		delete_task_by_parm1_func(presv, resv_idle_delete, DELETE_ONE); /* Delete the previous task if it exists */
		retry_time = time_now + get_rattr_long(presv, RESV_ATR_del_idle_time);
		if (retry_time < presv->ri_qs.ri_etime) {
			wt = set_task(WORK_Timed, retry_time, resv_idle_delete, presv);
			append_link(&presv->ri_svrtask, &wt->wt_linkobj, wt);
		}
	}
}

/**
 * @brief
 *		qmove a job into a reservation
 *
 * @parm[in]	presv	-	reservation structure
 *
 * @return	int
 *
 * @retval	0	: Success
 * @retval	-1	: Failure
 *
 */
int
cnvrt_qmove(resc_resv *presv)
{
	int rc;
	struct job *pjob;
	char *q_job_id, *at;
	struct batch_request *reqcnvrt;

	if (gen_task_EndResvWindow(presv)) {
		(void)resv_purge(presv);
		return (-1);
	}

	pjob = find_job(get_rattr_str(presv, RESV_ATR_convert));
	if (pjob != NULL)
		q_job_id = pjob->ji_qs.ji_jobid;
	else {
		(void)resv_purge(presv);
		return (-1);
	}
	if ((reqcnvrt = alloc_br(PBS_BATCH_MoveJob)) == NULL) {
		(void)resv_purge(presv);
		return (-1);
	}
	reqcnvrt->rq_perm = (presv->ri_brp)->rq_perm;
	strcpy(reqcnvrt->rq_user, (presv->ri_brp)->rq_user);
	strcpy(reqcnvrt->rq_host, (presv->ri_brp)->rq_host);

	snprintf(reqcnvrt->rq_ind.rq_move.rq_jid, sizeof(reqcnvrt->rq_ind.rq_move.rq_jid), "%s", q_job_id);
	at = strchr(presv->ri_qs.ri_resvID, (int)'.');
	if (at)
		*at = '\0';

	snprintf(reqcnvrt->rq_ind.rq_move.rq_destin, sizeof(reqcnvrt->rq_ind.rq_move.rq_destin), "%s", presv->ri_qs.ri_resvID);
	if (at)
		*at = '.';

	snprintf(pjob->ji_qs.ji_destin, PBS_MAXROUTEDEST, "%s", reqcnvrt->rq_ind.rq_move.rq_destin);
	rc = cnvrt_local_move(pjob, reqcnvrt);

	if (rc != 0) return (-1);
	return (0);
}


/**
 * @brief
 * 		resv_timer_init - initialize timed task for removing empty reservation
 */
void
resv_timer_init(void)
{
	resc_resv *presv;
	presv = (resc_resv *)GET_NEXT(svr_allresvs);
	while (presv) {
		if (is_rattr_set(presv, RESV_ATR_del_idle_time))
			set_idle_delete_task(presv);
		presv = (resc_resv *)GET_NEXT(presv->ri_allresvs);
	}
}


/*-----------------------------------------------------------------------
 Functions that deals with a resc_resv* rather than a job*
 These may end up being deleted if the two can be easily merged
 -----------------------------------------------------------------------
 */


/**
 * @brief
 * 		remove_node_from_resv - procedure removes node from reservation
 *		the node is removed from RESV_ATR_resv_nodes and assigned
 *		resources are accounted back to loaner's pool and finally the
 *		reservation is removed from the node
 *
 * @parm[in,out]	presv	-	reservation structure
 * @parm[in,out]	pnode	-	pointer to node
 *
 */
void
remove_node_from_resv(resc_resv *presv, struct pbsnode *pnode)
{
	char *begin = NULL;
	char *end = NULL;
	char *tmp_buf;
	struct resvinfo *rinfp, *prev;
	attribute tmpatr;

	/* +2 for colon and termination */
	tmp_buf = malloc(strlen(pnode->nd_name) + 2);
	if (tmp_buf == NULL) {
		snprintf(log_buffer, LOG_BUF_SIZE, "malloc failure (errno %d)", errno);
			log_err(PBSE_SYSTEM, __func__, log_buffer);
		return;
	}

	snprintf(tmp_buf, strlen(pnode->nd_name) + 1, "%s:", pnode->nd_name);

	/* remove the node '(vn[n]:foo)' from RESV_ATR_resv_nodes attribute */
	if (is_rattr_set(presv, RESV_ATR_resv_nodes)) {
		if ((begin = strstr(get_rattr_str(presv, RESV_ATR_resv_nodes), tmp_buf)) != NULL) {

			while (*begin != '(')
				begin--;

			end = strchr(begin, ')');
			end++;

			if (presv->ri_giveback) {
				/* resources were actually assigned to this reservation
				 * we must return the resources back into the loaner's pool.
				 *
				 * We use temp attribute for this and this attribute will
				 * contain only the removed part of resv_nodes and this part will be returned
				 */
				// FIXME: Can we create some utility func for below?
				clear_attr(&tmpatr, &resv_attr_def[(int)RESV_ATR_resv_nodes]);
				resv_attr_def[(int)RESV_ATR_resv_nodes].at_set(&tmpatr, get_rattr(presv, RESV_ATR_resv_nodes), SET);
				tmpatr.at_flags = (get_rattr(presv, RESV_ATR_resv_nodes))->at_flags;

				strncpy(tmpatr.at_val.at_str, begin, (end - begin));
				tmpatr.at_val.at_str[end - begin] = '\0';

				update_node_rassn(&tmpatr, DECR);

				resv_attr_def[(int)RESV_ATR_resv_nodes].at_free(&tmpatr);

				/* Note: We do not want to set presv->ri_giveback to 0 here.
				 * The resv_nodes may not be empty yet and there could
				 * be server resources assigned - it will be handled later.
				 */
			}

			/* remove "(vn[n]:foo)" from the resv_nodes, no '+' is removed yet */
			memmove(begin, end, strlen(end) + 1); /* +1 for '\0' */

			if (strlen(get_rattr_str(presv, RESV_ATR_resv_nodes)) == 0) {
				free_rattr(presv, RESV_ATR_resv_nodes);
				/* full remove of RESV_ATR_resv_nodes is dangerous;
				 * the associated job can run anywhere without RESV_ATR_resv_nodes
				 * so stop the associated queue */
				change_enableORstart(presv, Q_CHNG_START, ATR_FALSE);
			} else {
				/* resv_nodes looks like "+(vn2:foo)" or "(vn1:foo)+" or "(vn1:foo)++(vn3:bar)"
				 * the extra '+' is removed here */
				int tmp_len;
				char *nodes = get_rattr_str(presv, RESV_ATR_resv_nodes);

				/* remove possible leading '+' */
				if (nodes[0] == '+')
					memmove(nodes, nodes + 1, strlen(nodes));

				/* remove possible trailing '+' */
				tmp_len = strlen(nodes);
				if (nodes[tmp_len - 1] == '+')
					nodes[tmp_len - 1] = '\0';

				/* change possible '++' into single '+' */
				if ((begin = strstr(nodes, "++")) != NULL)
					memmove(begin, begin + 1, strlen(begin + 1) + 1);
				set_rattr_str_slim(presv, RESV_ATR_resv_nodes, nodes, NULL);
			}
		}
	}

	/* traverse the reservations of the node and remove the reservation if found */
	for (prev = NULL, rinfp = pnode->nd_resvp; rinfp; prev = rinfp, rinfp = rinfp->next) {
		if (strcmp(presv->ri_qs.ri_resvID, rinfp->resvp->ri_qs.ri_resvID) == 0) {
			if (prev == NULL)
				pnode->nd_resvp = rinfp->next;
			else
				prev->next = rinfp->next;
			free(rinfp);
			break;
		}
	}

	free(tmp_buf);
}

/**
 * @brief
 * 		remove_host_from_resv - it calls remove_node_from_resv() for all
 *		vnodes on the host
 *
 * @parm[in,out]	presv	-	reservation structure
 * @parm[in]		hostname -	string with hostname
 *
 */
void
remove_host_from_resv(resc_resv *presv, char *hostname) {
	pbsnode_list_t *pl = NULL;
	pbsnode_list_t *prev = NULL;

	for (prev = NULL, pl = presv->ri_pbsnode_list; pl != NULL;) {
		if (strcmp(pl->vnode->nd_hostname, hostname) == 0) {
			remove_node_from_resv(presv, pl->vnode);
			if (prev == NULL) {
				presv->ri_pbsnode_list = pl->next;
				free(pl);
				pl = presv->ri_pbsnode_list;
			} else {
				prev->next = pl->next;
				free(pl);
				pl = prev->next;
			}
		} else {
			prev = pl;
			pl = pl->next;
		}
	}
}

/**
 * @brief
 * 		degrade_overlapping_resv - by traversing all associated nodes
 *		of the presv, search all overlapping reservations and if the
 *		reservation is not 'maintenance' and it is confirmed then
 *		degrade the reservation and wipe the overloaded node from the
 *		overlapping reservation with remove_node_from_resv()
 *
 * @parm[in,out]	presv	-	reservation structure
 *
 */
void
degrade_overlapping_resv(resc_resv *presv)
{
	pbsnode_list_t *pl = NULL;
	struct resvinfo *rip;
	resc_resv *tmp_presv;
	int modified;

	for (pl = presv->ri_pbsnode_list; pl != NULL; pl = pl->next) {
		do {
			modified = 0;

			for (rip = pl->vnode->nd_resvp; rip; rip = rip->next) {

				tmp_presv = rip->resvp;

				if (tmp_presv->ri_qs.ri_resvID[0] == PBS_MNTNC_RESV_ID_CHAR)
					continue;

				if (tmp_presv->ri_qs.ri_state == RESV_UNCONFIRMED)
					continue;

				if (strcmp(presv->ri_qs.ri_resvID, tmp_presv->ri_qs.ri_resvID) != 0 &&
					presv->ri_qs.ri_stime <= tmp_presv->ri_qs.ri_etime &&
					presv->ri_qs.ri_etime >= tmp_presv->ri_qs.ri_stime) {

					set_resv_retry(tmp_presv, time_now);

					if (tmp_presv->ri_qs.ri_state == RESV_CONFIRMED) {
						resv_setResvState(tmp_presv, RESV_DEGRADED, RESV_IN_CONFLICT);
					} else {
						resv_setResvState(tmp_presv, tmp_presv->ri_qs.ri_state, RESV_IN_CONFLICT);
					}

					remove_host_from_resv(tmp_presv, pl->vnode->nd_hostname);

					resv_save_db(tmp_presv);

					/* we need 'break' here and start over because remove_host_from_resv()
					 * modifies pl->vnode->nd_resvp */
					modified = 1;
					break;
				}
			}
		} while (modified);
	}
}

/**
 * @brief
 * 		assign_resv_resc - function examines the reservation object
 * 		and server global parameters to obtain the node specification.
 * 		If the above yields a non-NULL node spec, global function
 * 		set_nodes() is called to locate a set of nodes for the subject
 * 		reservation and allocate them to the reservation - each node
 * 		that's allocated to the reservation gets a resvinfo structure
 * 		added to its list of resvinfo structures and that structure
 * 		points to the reservation.
 *
 * @parm[in,out]	presv	-	reservation structure
 * @parm[in]	vnodes	-	original vnode list from scheduler/operator
 * @parm[in]	svr_init	- the server is recovering jobs and reservations
 *
 * @return	int
 * @return	0 : no problems detected in the process
 * @retval	non-zero	: error code if problem occurs
 */
int
assign_resv_resc(resc_resv *presv, char *vnodes, int svr_init)
{
	int		  ret;
	char     *node_str = NULL;
	char		 *host_str = NULL;	/* used only as arg to set_nodes */
	char * host_str2 = NULL;
	if ((vnodes == NULL) || (*vnodes == '\0'))
		return (PBSE_BADNODESPEC);

	ret = set_nodes((void *)presv, RESC_RESV_OBJECT, vnodes,
					&node_str, &host_str, &host_str2, 0, svr_init);

	if (ret == PBSE_NONE) {
		/* update resc_resv object's RESV_ATR_resv_nodes attribute */
		set_rattr_str_slim(presv, RESV_ATR_resv_nodes, node_str, NULL);
	}

	return (ret);
}



/**
 * @brief
 * req_confirmresv -	confirm an advance or standing reservation and
 * 			set the assigned resources and optionally the start time.
 *
 *			Handle the reconfirmation of a degraded reservation: The
 *			reconfirmation is handled by altering the reservation's execvnodes with
 *			alternate execvnodes.
 *
 *			Handle the confirmation/denial of reservation alter request.
 *
 * @param
 * preq[in, out]   -	The batch request containing the success or failure of a
 * 			reservation confirmation or re-confirmation.
 */

void
req_confirmresv(struct batch_request *preq)
{
	time_t newstart = 0;
	resc_resv *presv = NULL;
	int rc = 0;
	int state = 0;
	int sub = 0;
	int resv_count = 0;
	int is_degraded = 0;
	int is_confirmed = 0;
	char *execvnodes = NULL;
	char *next_execvnode = NULL;
	char **short_xc = NULL;
	char **tofree = NULL;
	int is_being_altered = 0;
	char *tmp_buf = NULL;
	size_t tmp_buf_size = 0;
	char buf[PBS_MAXQRESVNAME+PBS_MAXHOSTNAME + 256] = {0}; /* FQDN resvID+text */
	char *partition_name = NULL;

	if ((preq->rq_perm & (ATR_DFLAG_MGWR | ATR_DFLAG_OPWR)) == 0) {
		req_reject(PBSE_PERM, 0, preq);
		return;
	}

	presv = find_resv(preq->rq_ind.rq_run.rq_jid);
	if (presv == NULL) {
		req_reject(PBSE_UNKRESVID, 0, preq);
		return;
	}
	is_degraded = (presv->ri_qs.ri_substate == RESV_DEGRADED || presv->ri_qs.ri_substate == RESV_IN_CONFLICT) ? 1 : 0;
	is_being_altered = presv->ri_alter.ra_flags;
	is_confirmed = (presv->ri_qs.ri_substate == RESV_CONFIRMED) ? 1 : 0;

	presv->rep_sched_count++;

	/* Check if preq is coming from scheduler */
	if (preq->rq_extend == NULL) {
		req_reject(PBSE_resvFail, 0, preq);
		return;
	}

	/* If the reservation was degraded and it could not be reconfirmed by the
	 * scheduler, then the retry time for that reservation is reset to the half-
	 * time between now and the time to reservation start or, if the retry time
	 * is invalid, set it to some time after the soonest occurrence is to start
	 */
	if (strcmp(preq->rq_extend, PBS_RESV_CONFIRM_FAIL) == 0) {
		int force_requested = FALSE;
		if (is_degraded && !is_being_altered) {
			long retry_time;
			retry_time = determine_resv_retry(presv);

			set_resv_retry(presv, retry_time);

		} else {
			if (presv->rep_sched_count >= presv->req_sched_count) {
				/* Clients waiting on an interactive request must be
				* notified of the failure to confirm
				*/
				if ((presv->ri_brp != NULL) && is_rattr_set(presv, RESV_ATR_interactive)) {
					if (!(presv->ri_alter.ra_flags & RESV_ALTER_FORCED)) {
						(get_rattr(presv, RESV_ATR_interactive))->at_flags &= ~ATR_VFLAG_SET;
						snprintf(buf, sizeof(buf), "%s DENIED", presv->ri_qs.ri_resvID);
						(void)reply_text(presv->ri_brp, PBSE_NONE, buf);
						presv->ri_brp = NULL;
					}
				}
				if (!is_being_altered && !is_confirmed) {
					log_event(PBS_EVENTCLASS_RESV, PBS_EVENTCLASS_RESV, LOG_INFO, presv->ri_qs.ri_resvID, "Reservation denied");
					(void)snprintf(log_buffer, sizeof(log_buffer), "requestor=%s@%s", msg_daemonname, server_host);
					account_recordResv(PBS_ACCT_DRss, presv, log_buffer);
					log_event(PBSEVENT_DEBUG, PBS_EVENTCLASS_RESV, LOG_NOTICE, presv->ri_qs.ri_resvID, "reservation deleted");
					resv_purge(presv);
				}
			}
		}
		if (presv->ri_qs.ri_state == RESV_BEING_ALTERED) {
			if (!(presv->ri_alter.ra_flags & RESV_ALTER_FORCED)) {
				revert_alter_reservation(presv);
				log_event(PBSEVENT_RESV, PBS_EVENTCLASS_RESV, LOG_INFO,
					presv->ri_qs.ri_resvID, "Reservation alter denied");
			} else if (presv->rep_sched_count >= presv->req_sched_count)
				force_requested = TRUE;
		}
		if (is_being_altered)
			free_rattr(presv, RESV_ATR_alter_revert);

		if (force_requested == FALSE) {
			reply_ack(preq);
			return;
		} else {
			/* This can only happen when ralter was requested with -Wforce option.
			 * Even though all schedulers have rejected the change, enforce it.
			 */
			presv->ri_alter.ra_flags &= ~RESV_ALTER_FORCED;
			free(preq->rq_extend);
			if (pbs_asprintf(&preq->rq_extend, "%s:partition=%s", PBS_RESV_CONFIRM_SUCCESS,
					 get_rattr_str(presv, RESV_ATR_partition)) == -1) {
				req_reject(PBSE_SYSTEM, 0, preq);
				return;
			}
			/* set start time and destination in the preq structure */
			if (is_rattr_set(presv, RESV_ATR_start))
				preq->rq_ind.rq_run.rq_resch = get_rattr_long(presv, RESV_ATR_start);
			if (is_rattr_set(presv, RESV_ATR_resv_nodes)) {
				preq->rq_ind.rq_run.rq_destin = create_resv_destination(presv);
				if (preq->rq_ind.rq_run.rq_destin == NULL) {
					req_reject(PBSE_SYSTEM, 0, preq);
					return;
				}
			}
		}
	}

	if (is_being_altered)
		free_rattr(presv, RESV_ATR_alter_revert);

	/* if passed in the confirmation, set a new start time */
	if ((newstart = (time_t)preq->rq_ind.rq_run.rq_resch) != 0) {
		presv->ri_qs.ri_stime = newstart;
		set_rattr_l_slim(presv, RESV_ATR_start, newstart, SET);

		presv->ri_qs.ri_etime = newstart + presv->ri_qs.ri_duration;
		set_rattr_l_slim(presv, RESV_ATR_end, presv->ri_qs.ri_etime, SET);
	}

	/* The main difference between an advance reservation and a standing
	 * reservation is the format of the execvnodes returned by "rq_destin":
	 * An advance reservation has a single execvnode while a standing reservation
	 * has a sting with the  particular format:
	 *    <num_resv>#<execvnode1>[<range>]<exevnode2>[...
	 * describing the execvnodes associated to each occurrence.
	 */
	if (get_rattr_str(presv, RESV_ATR_resv_standing)) {
		/* The number of occurrences in the standing reservation and index are parsed
		 * from the execvnode string which is of the form:
		 *     <num_occurrences>#<vnode1>[range1]<vnode2>[range2]...
		 */
		resv_count = get_execvnodes_count(preq->rq_ind.rq_run.rq_destin);
		if (resv_count == 0) {
			req_reject(PBSE_INTERNAL, 0, preq);
			return;
		}

		execvnodes = strdup(preq->rq_ind.rq_run.rq_destin);
		if (execvnodes == NULL) {
			req_reject(PBSE_SYSTEM, 0, preq);
			return;
		}
		DBPRT(("stdg_resv conf: execvnodes_seq is %s\n", execvnodes));

		/* execvnodes is of the form:
		 *       <num_resv>#<(execvnode1)>[<range>]<(exevnode2)>[...
		 * this "condensed" string is unrolled into a pointer array of
		 * execvnodes per occurrence, e.g. short_xc[0] are the execvnodes
		 * for 1st occurrence, short_xc[1] for the 2nd etc...
		 * If something goes wrong during unrolling then NULL is returned.
		 * which causes the confirmation message to be rejected
		 */
		short_xc = unroll_execvnode_seq(execvnodes, &tofree);
		if (short_xc == NULL) {
			free(execvnodes);
			req_reject(PBSE_SYSTEM, 0, preq);
			return;
		}
		/* The execvnode of the soonest (i.e., next) occurrence */
		next_execvnode = strdup(short_xc[0]);
		if (next_execvnode == NULL) {
			free(short_xc);
			free_execvnode_seq(tofree);
			free(execvnodes);
			req_reject(PBSE_SYSTEM, 0, preq);
			return;
		}
		/* Release the now obsolete allocations used to manipulate the
		 * unrolled string */
		free(short_xc);
		free_execvnode_seq(tofree);
		free(execvnodes);

		/* When confirming for the first time, set the index and count */
		if (!is_degraded) {

			/* Add first occurrence's end date on timed task list */
			if (get_rattr_long(presv, RESV_ATR_start) != PBS_RESV_FUTURE_SCH) {
				if (gen_task_EndResvWindow(presv)) {
					free(next_execvnode);
					req_reject(PBSE_SYSTEM, 0, preq);
					return;
				}
			}
			/* Set first occurrence to index 1
			 * (rather than 0 because it gets displayed in pbs_rstat -f) */
			set_rattr_l_slim(presv, RESV_ATR_resv_idx, 1, SET);
		}

		/* Skip setting the execvnodes sequence when reconfirming the last
		 * occurrence or when altering a reservation.
		 */
		if (!is_being_altered) {
			char *new_execvnode = preq->rq_ind.rq_run.rq_destin;
			int remaining_occurrences = get_rattr_long(presv, RESV_ATR_resv_count) - get_rattr_long(presv, RESV_ATR_resv_idx) + 1; /* resv_idx starts at 1 */
			if (get_execvnodes_count(new_execvnode) != remaining_occurrences) {
				log_event(PBSEVENT_RESV, PBS_EVENTCLASS_RESV, LOG_WARNING, presv->ri_qs.ri_resvID, "Number of execvnodes given does not equal the number of occurrences left");
				free(next_execvnode);
				req_reject(PBSE_BADATVAL, 0, preq);
				return;
			}

			if (remaining_occurrences > 0) {
				/* now assign the execvnodes sequence attribute */
				set_rattr_str_slim(presv, RESV_ATR_resv_execvnodes, preq->rq_ind.rq_run.rq_destin, NULL);
			}
		}
	} else { /* Advance reservation */
		next_execvnode = strdup(preq->rq_ind.rq_run.rq_destin);
		if (next_execvnode == NULL) {
			req_reject(PBSE_SYSTEM, 0, preq);
			return;
		}
	}

	/* Is reservation still a viable reservation? */
	if ((rc = chk_resvReq_viable(presv)) != 0) {
		free(next_execvnode);
		req_reject(PBSE_BADTSPEC, 0, preq);
		return;
	}

	/* When reconfirming a degraded reservation, first free the nodes linked
	 * to the reservation and unset all attributes relating to retry attempts
	 */
	if (is_degraded) {
		if (presv->ri_qs.ri_state == RESV_RUNNING) {
			if (presv->ri_giveback) {
				set_resc_assigned((void *) presv, 1, DECR);
				presv->ri_giveback = 0;
			}
		}
		free_resvNodes(presv);
		/* Reset retry time */
		unset_resv_retry(presv);
		/* reset vnodes_down counter to 0 */
		presv->ri_vnodes_down = 0;
	}

	if (is_being_altered & RESV_END_TIME_MODIFIED) {
		if (gen_task_EndResvWindow(presv)) {
			free(next_execvnode);
			req_reject(PBSE_SYSTEM, 0, preq);
			return;
		}
	}

	/*
	 * Assign the allocated resources to the reservation
	 * and the reservation to the associated vnodes.
	 */
	if (is_being_altered) {
		if ((is_being_altered & RESV_SELECT_MODIFIED) && presv->ri_qs.ri_stime < time_now) {
			/* If we are both degraded and ralter -lselect, we are fine.  We will have unset ri_giveback above */
			if (presv->ri_giveback) {
				set_resc_assigned((void *) presv, 1, DECR);
				presv->ri_giveback = 0;
			}
		}

		free_resvNodes(presv);
	}
	rc = assign_resv_resc(presv, next_execvnode, FALSE);

	if (presv->ri_qs.ri_stime < time_now) {
		if (is_degraded || is_being_altered & RESV_SELECT_MODIFIED) {
			if (presv->ri_giveback == 0) {
				set_resc_assigned((void *) presv, 1, INCR);
				presv->ri_giveback = 1;
			}
		}
	}

	if (rc != PBSE_NONE) {
		free(next_execvnode);
		req_reject(rc, 0, preq);
		return;
	}

	/* place "Time4resv" task on "task_list_timed" only if this is a
	 * confirmation but not the reconfirmation of a degraded reservation as
	 * in this case, the reservation had already been confirmed and added to
	 * the task list before
	 */
	if (!is_degraded && (!is_being_altered || is_being_altered & RESV_START_TIME_MODIFIED) &&
	    (rc = gen_task_Time4resv(presv)) != 0) {
		free(next_execvnode);
		req_reject(rc, 0, preq);
		return;
	}

	/*
	 * compute new values for state and substate
	 * and update the resc_resv object with these
	 * newly computed values
	 */
	eval_resvState(presv, RESVSTATE_gen_task_Time4resv, 0, &state, &sub);
	resv_setResvState(presv, state, sub);
	if (strncmp(preq->rq_extend, PBS_RESV_CONFIRM_SUCCESS, strlen(PBS_RESV_CONFIRM_SUCCESS)) == 0) {
		char *p_tmp;
		p_tmp = strstr(preq->rq_extend, ":partition=");
		if (p_tmp) {
			p_tmp += strlen(":partition=");
			partition_name = strdup(p_tmp);
		} else
			partition_name = strdup(DEFAULT_PARTITION);

		if (partition_name == NULL) {
			req_reject(PBSE_SYSTEM, 0, preq);
			return;
		}
		/* Reservation is not degraded anymore */
		is_degraded = 0;

	}
	if (state == RESV_CONFIRMED && partition_name != NULL) {
		/* Set the name of the partition where the reservation is confirmed*/
		pbs_queue *rque = NULL;
		char *qname = NULL;
		char *p;
		set_rattr_str_slim(presv, RESV_ATR_partition, partition_name, NULL);
		qname = strdup(presv->ri_qs.ri_resvID);
		if (qname == NULL) {
			log_err(PBSE_INTERNAL, __func__, "malloc failed");
			req_reject(PBSE_SYSTEM, 0, preq);
			free(partition_name);
			return;
		}
		p = strpbrk(qname, ".");
		if (p != NULL)
			*p = '\0';
		rque = find_queuebyname(qname);
		if (rque == NULL) {
			log_err(PBSE_INTERNAL, __func__, "Reservation queue not found");
			req_reject(PBSE_INTERNAL, 0, preq);
			free(partition_name);
			free (qname);
			return;
		} else {
			set_qattr_str_slim(rque, QA_ATR_partition, partition_name, NULL);
			que_save_db(rque);
		}
		free(qname);
	}
	free(partition_name);
	resv_save_db(presv);

	log_buffer[0] = '\0';

	/*
	 * Notify all interested parties that the reservation
	 * is moving from state UNCONFIRMED to CONFIRMED
	 */
	if (presv->ri_brp) {
		presv = find_resv(presv->ri_qs.ri_resvID);
		if (get_rattr_str(presv, RESV_ATR_convert) != NULL) {
			rc = cnvrt_qmove(presv);
			if (rc != 0) {
				snprintf(buf, sizeof(buf), "%.240s FAILED", presv->ri_qs.ri_resvID);
			} else {
				snprintf(buf, sizeof(buf), "%.240s CONFIRMED", presv->ri_qs.ri_resvID);
			}
		} else {
			snprintf(buf, sizeof(buf), "%.240s CONFIRMED", presv->ri_qs.ri_resvID);
		}

		rc = reply_text(presv->ri_brp, PBSE_NONE, buf);
		presv->ri_brp = NULL;
	}

	svr_mailownerResv(presv, MAIL_CONFIRM, MAIL_NORMAL, log_buffer);
	(get_rattr(presv, RESV_ATR_interactive))->at_flags &= ~ATR_VFLAG_SET;

	if (is_being_altered) {
		/*
		 * If the reservation is currently running and its start time is being
		 * altered after the current time, It is going back to the confirmed state.
		 * We need to stop the reservation queue as it would have been started at
		 * the original start time.
		 * This will prevent any jobs - that are submitted after the
		 * reservation's start time is changed - from running.
		 * The reservation went to CO from RN while being altered, that means the reservation
		 * had resources assigned. We should decrement their usages until it starts running
		 * again, where the resources will be accounted again.
		 */
		if (presv->ri_qs.ri_state == RESV_CONFIRMED && presv->ri_alter.ra_state == RESV_RUNNING) {
			change_enableORstart(presv, Q_CHNG_START, "FALSE");
			if (presv->ri_giveback) {
				set_resc_assigned((void *)presv, 1, DECR);
				presv->ri_giveback = 0;
			}
		}
		if (presv->ri_alter.ra_flags & RESV_SELECT_MODIFIED)
			free_rattr(presv, RESV_ATR_SchedSelect_orig);

		presv->ri_alter.ra_flags = 0;


		log_event(PBSEVENT_RESV, PBS_EVENTCLASS_RESV, LOG_INFO,
			  presv->ri_qs.ri_resvID, "Reservation alter confirmed");
	} else
		log_event(PBSEVENT_RESV, PBS_EVENTCLASS_RESV, LOG_INFO,
			  presv->ri_qs.ri_resvID, "Reservation confirmed");

	if (!is_degraded) {
		/* 100 extra bytes for field names, times, and count */
		tmp_buf_size = 100 + strlen(preq->rq_user) + strlen(preq->rq_host) + strlen(next_execvnode);
		if (tmp_buf_size > sizeof(buf)) {
			tmp_buf = malloc(tmp_buf_size);
			if (tmp_buf == NULL) {
				snprintf(log_buffer, LOG_BUF_SIZE-1, "malloc failure (errno %d)", errno);
				log_err(PBSE_SYSTEM, __func__, log_buffer);
				free(next_execvnode);
				reply_ack(preq);
				return;
			}
		} else {
			tmp_buf = buf;
			tmp_buf_size = sizeof(buf);
		}

		if (get_rattr_long(presv, RESV_ATR_resv_standing)) {
			(void)snprintf(tmp_buf, tmp_buf_size, "requestor=%s@%s start=%ld end=%ld nodes=%s count=%ld",
				preq->rq_user, preq->rq_host,
				presv->ri_qs.ri_stime, presv->ri_qs.ri_etime,
				next_execvnode,
				get_rattr_long(presv, RESV_ATR_resv_count));
		} else {
			(void)snprintf(tmp_buf, tmp_buf_size, "requestor=%s@%s start=%ld end=%ld nodes=%s",
				preq->rq_user, preq->rq_host,
				presv->ri_qs.ri_stime, presv->ri_qs.ri_etime,
				next_execvnode);
		}
		char hook_msg[HOOK_MSG_SIZE] = {0};
		switch (process_hooks(preq, hook_msg, sizeof(hook_msg), pbs_python_set_interrupt)) {
			case 0: /* explicit reject */
			case 1: /* no recreate request as there are only read permissions */
			case 2: /* no hook script executed - go ahead and accept event*/
				break;
			default:
				log_event(PBSEVENT_DEBUG2, PBS_EVENTCLASS_HOOK, LOG_INFO, __func__,
					"resv_confirm event: accept req by default");
		}
		account_recordResv(PBS_ACCT_CR, presv, tmp_buf);
		if (tmp_buf != buf) {
			free(tmp_buf);
			tmp_buf_size = 0;
		}
	}

	if (presv->ri_qs.ri_resvID[0] == PBS_MNTNC_RESV_ID_CHAR)
		degrade_overlapping_resv(presv);

	free(next_execvnode);
	reply_ack(preq);

	return;
}

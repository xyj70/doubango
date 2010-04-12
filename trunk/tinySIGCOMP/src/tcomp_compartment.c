/*
* Copyright (C) 2009 Mamadou Diop.
*
* Contact: Mamadou Diop <diopmamadou(at)doubango.org>
*	
* This file is part of Open Source Doubango Framework.
*
* DOUBANGO is free software: you can redistribute it and/or modify
* it under the terms of the GNU General Public License as published by
* the Free Software Foundation, either version 3 of the License, or
* (at your option) any later version.
*	
* DOUBANGO is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
* GNU General Public License for more details.
*	
* You should have received a copy of the GNU General Public License
* along with DOUBANGO.
*
*/

/**@file tcomp_compartment.c
 * @brief  SigComp compartment.
 * An application-specific grouping of messages that relate to a peer endpoint.  Depending on the signaling protocol, this grouping may
 * relate to application concepts such as "session", "dialog", "connection", or "association".  The application allocates state
 * memory on a per-compartment basis, and determines when a compartment should be created or closed.
 *
 * @author Mamadou Diop <diopmamadou(at)yahoo.fr>
 *
 * @date Created: Sat Nov 8 16:54:58 2009 mdiop
 */
#include "tcomp_compartment.h"

#include "tsk_debug.h"

#include <assert.h>


/**Sets remote parameters
*/
void tcomp_compartment_setRemoteParams(tcomp_compartment_t *compartment, tcomp_params_t *lpParams)
{
	if(!compartment)
	{
		TSK_DEBUG_ERROR("NULL sigcomp compartment.");
		return;
	}

	/* CPB||DMS||SMS [1-BYTE] */
	if(tcomp_params_hasCpbDmsSms(lpParams))
	{
		tcomp_params_setCpbCode(compartment->remote_parameters, lpParams->cpbCode);
		tcomp_params_setDmsCode(compartment->remote_parameters, lpParams->dmsCode);
		tcomp_params_setSmsCode(compartment->remote_parameters, lpParams->smsCode);
	}

	/* SigComp version */
	if(lpParams->SigComp_version)
	{
		compartment->remote_parameters->SigComp_version = lpParams->SigComp_version;
	}

	/*
	*	Returned states
	*	FIXME: check states about quota
	*	FIXME: not tic tac
	*	FIXME: what about returned feedback?
	*/
	if(lpParams->returnedStates && tcomp_buffer_getSize(lpParams->returnedStates))
	{
		TSK_OBJECT_SAFE_FREE(compartment->remote_parameters->returnedStates);
		/* swap */
		compartment->remote_parameters->returnedStates = lpParams->returnedStates;
		lpParams->returnedStates = 0;
	}
}

/**Sets requested feedback
*/
void tcomp_compartment_setReqFeedback(tcomp_compartment_t *compartment, tcomp_buffer_handle_t *feedback)
{
	if(!compartment)
	{
		TSK_DEBUG_ERROR("NULL sigcomp compartment.");
		return;
	}

	tsk_safeobj_lock(compartment);

	/* Delete old */
	TSK_OBJECT_SAFE_FREE(compartment->lpReqFeedback);

	compartment->lpReqFeedback = _TCOMP_BUFFER_CREATE(tcomp_buffer_getBuffer(feedback), tcomp_buffer_getSize(feedback));

	tsk_safeobj_unlock(compartment);
}

/**Sets returned feedback
*/
void tcomp_compartment_setRetFeedback(tcomp_compartment_t *compartment, tcomp_buffer_handle_t *feedback)
{
	if(!compartment)
	{
		TSK_DEBUG_ERROR("NULL sigcomp compartment.");
		return;
	}
	
	tsk_safeobj_lock(compartment);
	
	// Delete old
	TSK_OBJECT_SAFE_FREE(compartment->lpRetFeedback);
	
	compartment->lpRetFeedback = _TCOMP_BUFFER_CREATE(tcomp_buffer_getBuffer(feedback), tcomp_buffer_getSize(feedback));

#if USE_ONLY_ACKED_STATES
	/*
	* ACK STATE ==> Returned feedback contains the partial state-id.
	*/
	if(compartment->compressorData && !compartment->compressorData_isStream)
	{
		tcomp_buffer_handle_t *stateid = _TCOMP_BUFFER_CREATE(tcomp_buffer_getBufferAtPos(feedback, 1), tcomp_buffer_getSize(feedback)-1);
		compartment->ackGhost(compartment->compressorData, stateid);
		TSK_OBJECT_SAFE_FREE(stateid);
	}
#endif
	
	tsk_safeobj_unlock(compartment);
}

/**Clears are compartment from the history.
*/
void tcomp_compartment_clearStates(tcomp_compartment_t *compartment)
{
	if(!compartment)
	{
		TSK_DEBUG_ERROR("NULL sigcomp compartment.");
		return;
	}

	tsk_safeobj_lock(compartment);

	tsk_list_clear_items(compartment->local_states);
	compartment->total_memory_left = compartment->total_memory_size;

	tsk_safeobj_unlock(compartment);
}

/**Removes a state from the compartment by priority.
*/
void tcomp_compartment_freeStateByPriority(tcomp_compartment_t *compartment)
{
	tcomp_state_t *lpState;
	tsk_list_item_t *item;

	if(!compartment)
	{
		TSK_DEBUG_ERROR("NULL sigcomp compartment.");
		return;
	}

	tsk_safeobj_lock(compartment);

	lpState = 0;
	item = 0;

	/*
	 * The order in which the existing state items are freed is determined by the state_retention_priority, which is set when the
	 * state items are created.  The state_retention_priority of 65535 is reserved for locally available states; these states must always be
	 * freed first.  Apart from this special case, states with the lowest state_retention_priority are always freed first.  In the event of
	 * a tie, then the state item created first in the compartment is also the first to be freed.
	*/
	tsk_list_foreach(item, compartment->local_states)
	{
		tcomp_state_t *curr = item->data;

		if(!curr) continue;

		/* First --> always ok */
		if(item == compartment->local_states->head)
		{
			lpState = curr;
			continue;
		}
		
		/* Local state ? */
		if(curr->retention_priority == 65535)
		{
			lpState = curr;
			break;
		}

		/* Lower priority? */
		if(curr->retention_priority < lpState->retention_priority)
		{
			lpState = curr;
			continue;
		}
	}

	if(lpState)
	{
		compartment->total_memory_left += TCOMP_GET_STATE_SIZE(lpState);
		tsk_list_remove_item_by_data(compartment->local_states, lpState);
	}

	tsk_safeobj_unlock(compartment);
}

/**Removes a state from the compartment.
*/
void tcomp_compartment_freeState(tcomp_compartment_t *compartment, tcomp_state_t **lpState)
{
	if(!compartment)
	{
		TSK_DEBUG_ERROR("NULL sigcomp compartment.");
		return;
	}

	tsk_safeobj_lock(compartment);

	compartment->total_memory_left += TCOMP_GET_STATE_SIZE(*lpState);
	tsk_list_remove_item_by_data(compartment->local_states, *lpState);
	*lpState = 0;
	TSK_DEBUG_INFO("SigComp - Free state.");

	tsk_safeobj_unlock(compartment);
}

/**Remove states
*/
void tcomp_compartment_freeStates(tcomp_compartment_t *compartment, tcomp_tempstate_to_free_t **tempStates, uint8_t size)
{
	uint8_t i;
	tcomp_state_t *lpState;
	tsk_list_item_t *item;

	if(!compartment)
	{
		TSK_DEBUG_ERROR("NULL sigcomp compartment.");
		return;
	}

	if(!tempStates || !size)
	{
		return;
	}
	
	lpState = 0;
	item = 0;

	for (i = 0; i < size; i++)
	{
		/* lock */
		tsk_safeobj_lock(compartment);

		tsk_list_foreach(item, compartment->local_states)
		{
			tcomp_state_t *curr = item->data;
			
			/* Compare current state with provided partial state */
			if(tcomp_buffer_startsWith(curr->identifier, tempStates[i]->identifier))
			{
				/*
				* If more than one state item in the compartment matches the partial state identifier, 
				* then the state free request is ignored.
				*/
				if(lpState)
				{
					lpState = 0;
					break;
				}
				else
				{
					lpState = curr;
				}
			}
		}
		
		/* unlock */
		tsk_safeobj_unlock(compartment);

		if(lpState)
		{
			tcomp_compartment_freeState(compartment, &lpState);
		}
	}	
}

/**Adds a state to the compartment.
*/
void tcomp_compartment_addState(tcomp_compartment_t *compartment, tcomp_state_t **lpState)
{
	if(!compartment)
	{
		TSK_DEBUG_ERROR("NULL sigcomp compartment.");
		return;
	}

	tsk_safeobj_lock(compartment);

	tcomp_state_makeValid(*lpState);
	compartment->total_memory_left -= TCOMP_GET_STATE_SIZE(*lpState);
	tsk_list_push_back_data(compartment->local_states, ((void**) lpState));
	
	TSK_DEBUG_INFO("SigComp - Add new state.");
	*lpState = 0;

	tsk_safeobj_unlock(compartment);
}

/**Finds a state.
*/
uint16_t tcomp_compartment_findState(tcomp_compartment_t *compartment, const tcomp_buffer_handle_t *partial_identifier, tcomp_state_t **lpState)
{
	uint16_t count = 0;
	tsk_list_item_t *item;

	if(!compartment)
	{
		TSK_DEBUG_ERROR("NULL sigcomp compartment.");
		return 0;
	}
	
	tsk_safeobj_lock(compartment);

	item = 0;
	
	tsk_list_foreach(item, compartment->local_states)
	{
		tcomp_state_t *curr = item->data;
		
		if(tcomp_buffer_startsWith(curr->identifier, partial_identifier))
		{
			*lpState = curr; // override
			count++;
		}
	}

	tsk_safeobj_unlock(compartment);

	return count;
}

/**Removes a Ghost state.
*/
void tcomp_compartment_freeGhostState(tcomp_compartment_t *compartment)
{
	if(!compartment)
	{
		TSK_DEBUG_ERROR("NULL sigcomp compartment.");
		return;
	}

	tsk_safeobj_lock(compartment);

	if(compartment->compressorData)
	{
		compartment->freeGhostState(compartment->compressorData);
	}
	else
	{
		TSK_DEBUG_WARN("No compression data to free.");
	}

	tsk_safeobj_unlock(compartment);
}

/**Adds a NACK to the compartment.
*/
void tcomp_compartment_addNack(tcomp_compartment_t *compartment, const uint8_t nackId[TSK_SHA1_DIGEST_SIZE])
{
	tcomp_buffer_handle_t *id;

	if(!compartment)
	{
		TSK_DEBUG_ERROR("NULL sigcomp compartment.");
		return;
	}

	tsk_safeobj_lock(compartment);

	// FIXME: very bad
	if(compartment->nacks_history_count >= NACK_MAX_HISTORY_SIZE)
	{
		//tsk_list_item_t *item;
		tsk_list_item_t *item2delete = 0;

		/*tsk_list_foreach(item, compartment->nacks)
		{
			item2delete = item;
		}*/
		
		item2delete = compartment->nacks->tail;

		tsk_list_remove_item(compartment->nacks, item2delete);
		compartment->nacks_history_count--;
	}


	id = _TCOMP_BUFFER_CREATE(nackId, TSK_SHA1_DIGEST_SIZE);
	tsk_list_push_back_data(compartment->nacks, ((void**) &id));
	compartment->nacks_history_count++;

	tsk_safeobj_unlock(compartment);
}

/**Checks is the NACK exist.
*/
int tcomp_compartment_hasNack(tcomp_compartment_t *compartment, const tcomp_buffer_handle_t *nackId)
{
	int ret = 0;
	tsk_list_item_t *item;

	if(!compartment)
	{
		TSK_DEBUG_ERROR("NULL sigcomp compartment.");
		return 0;
	}

	tsk_safeobj_lock(compartment);

	item = 0;
	
	tsk_list_foreach(item, compartment->nacks)
	{
		tcomp_buffer_handle_t *curr = item->data;
	
		if(tcomp_buffer_equals(curr, nackId))
		{
			TSK_DEBUG_INFO("SigComp - Nack found.");
			ret = 1;
			break;
		}
	}

	tsk_safeobj_unlock(compartment);

	return ret;
}










//========================================================
//	State object definition
//

static tsk_object_t* tcomp_compartment_create(tsk_object_t* self, va_list * app)
{
	tcomp_compartment_t *compartment = self;
	if(compartment)
	{
		uint64_t id = va_arg(*app, uint64_t);
#if defined (__GNUC__)
		uint16_t sigCompParameters = (uint16_t)va_arg(*app, unsigned);
#else
		uint16_t sigCompParameters = va_arg(*app, uint16_t);
#endif

		/* Initialize safeobject */
		tsk_safeobj_init(compartment);

		/*
		  +---+---+---+---+---+---+---+---+
		  |  cpb  |    dms    |    sms    |
		  +---+---+---+---+---+---+---+---+
		  |        SigComp_version        |
		  +---+---+---+---+---+---+---+---+
		*/

		// I always assume that remote params are equal to local params

		/* Identifier */
		compartment->identifier = id;

		/* Remote parameters */
		compartment->remote_parameters = TCOMP_PARAMS_CREATE();
		tcomp_params_setParameters(compartment->remote_parameters, sigCompParameters);

		/* Local parameters */
		compartment->local_parameters = TCOMP_PARAMS_CREATE();
		tcomp_params_setParameters(compartment->local_parameters, sigCompParameters);

		/* Total size */
		compartment->total_memory_size = compartment->total_memory_left = compartment->local_parameters->smsValue;

		/* Empty list. */
		compartment->nacks = TSK_LIST_CREATE();
		
		/* Empty list. */
		compartment->local_states = TSK_LIST_CREATE();
	}

	return self;
}

static tsk_object_t* tcomp_compartment_destroy(tsk_object_t* self)
{
	tcomp_compartment_t *compartment = self;
	if(compartment)
	{
		/* Deinitialize safeobject */
		tsk_safeobj_deinit(compartment);

		/* Delete all states */
		TSK_OBJECT_SAFE_FREE(compartment->local_states);
		
		/* Delete feedbacks */
		TSK_OBJECT_SAFE_FREE(compartment->lpReqFeedback);
		TSK_OBJECT_SAFE_FREE(compartment->lpRetFeedback);
		
		/* Delete Nacks */
		TSK_OBJECT_SAFE_FREE(compartment->nacks);
		
		/* Delete Compressor data */
		TSK_OBJECT_SAFE_FREE(compartment->compressorData);
		compartment->ackGhost = 0;
		compartment->freeGhostState = 0;
		
		/* Delete params */
		TSK_OBJECT_SAFE_FREE(compartment->local_parameters);
		TSK_OBJECT_SAFE_FREE(compartment->remote_parameters);

		/* Delete NACKS */
		TSK_OBJECT_SAFE_FREE(compartment->nacks);

		/* Delete local states */
		TSK_OBJECT_SAFE_FREE(compartment->local_states);
	}
	
	return self;
}

static int tcomp_compartment_cmp(const tsk_object_t *obj1, const tsk_object_t *obj2)
{
	const tcomp_compartment_t *compartment1 = obj1;
	const tcomp_compartment_t *compartment2 = obj2;
	uint64_t res = (compartment1->identifier - compartment2->identifier);
	return res > 0 ? (int)1 : (res < 0 ? (int)-1 : (int)0);
}

static const tsk_object_def_t tsk_compartment_def_s = 
{
	sizeof(tcomp_compartment_t),
	tcomp_compartment_create,
	tcomp_compartment_destroy,
	tcomp_compartment_cmp
};
const tsk_object_def_t *tcomp_compartment_def_t = &tsk_compartment_def_s;

/*
 * Copyright (C) 1994-2020 Altair Engineering, Inc.
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

/**
 * @file	disrcs.c
 *
 * @par Synopsis:
 *	char *disrcs(int stream, size_t *nchars, int *retval)
 *
 *	Gets a Data-is-Strings character string from <stream> and converts it
 *	into a counted string, returns a pointer to it, and puts the character
 *	count into *<nchars>.  The character string in <stream> consists of an
 *	unsigned integer, followed by a number of characters determined by the
 *	unsigned integer.
 *
 *	The data returned has an NULL byte appended to the end in case the
 *	calling program wishes to treat it as a NULL terminated string.
 *	This means the space allocated for the data is one byte larger than
 *	indicated by the count.
 *
 *	*<retval> gets DIS_SUCCESS if everything works well.  It gets an error
 *	code otherwise.  In case of an error, the <stream> character pointer is
 *	reset, making it possible to retry with some other conversion strategy.
 *	In case of an error, disrcs returns NULL and <nchars> is set to 0.
 */
#include <pbs_config.h>   /* the master config generated by configure */

#include <assert.h>
#include <stddef.h>
#include <stdlib.h>

#include "dis.h"
#include "dis_.h"

/**
 * @brief
 *	-Gets a Data-is-Strings character string from <stream> and converts it
 *      into a counted string, returns a pointer to it, and puts the character
 *      count into *<nchars>.  The character string in <stream> consists of an
 *      unsigned integer, followed by a number of characters determined by the
 *      unsigned integer.
 *
 * @param[in] stream - socket descriptor
 * @param[out] nchars - character count
 * @param[out] retval - success/error code
 *
 * @return	char*
 * @retval	pointer to converted string	success
 * @retval	NULL				error
 *
 */
char *
disrcs(int stream, size_t *nchars, int *retval)
{
	int		locret;
	int		negate;
	unsigned	count = 0;
	char		*value = NULL;

	assert(nchars != NULL);
	assert(retval != NULL);

	locret = disrsi_(stream, &negate, &count, 1, 0);
	locret = negate ? DIS_BADSIGN : locret;
	if (locret == DIS_SUCCESS) {
		if (negate)
			locret = DIS_BADSIGN;
		else {
			value = (char *)malloc((size_t)count+1);
			if (value == NULL)
				locret = DIS_NOMALLOC;
			else {
				if (dis_gets(stream, value,
					(size_t)count) != (size_t)count)
					locret = DIS_PROTO;
				else
					value[count] = '\0';
			}
		}
	}
	if ((*retval = locret) != DIS_SUCCESS && value != NULL) {
		count = 0;
		free(value);
		value = NULL;
	}
	*nchars = count;
	return (value);
}

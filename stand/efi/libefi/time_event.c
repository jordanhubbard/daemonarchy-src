/*-
 * Copyright (c) 2016 Andrew Turner
 * All rights reserved.
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
 */

#include <efi.h>
#include <efilib.h>

#include <time.h>
#include <sys/time.h>

static EFI_EVENT time_event;
static uint64_t curtime;

static void EFIAPI
time_update(EFI_EVENT event, void *context)
{

	(void)event;
	(void)context;
	curtime++;
}

void
efi_time_init(void)
{
	EFI_STATUS status;

	if (time_event != NULL)
		return;
	/* Create a timer event. Never arm or close an unavailable handle. */
	status = BS->CreateEvent(EVT_TIMER | EVT_NOTIFY_SIGNAL, TPL_CALLBACK,
	    time_update, NULL, &time_event);
	if (!EFI_ERROR(status)) {
		/* Use a 1s timer. */
		status = BS->SetTimer(time_event, TimerPeriodic, 10000000);
		if (EFI_ERROR(status))
			BS->CloseEvent(time_event);
	}
	if (EFI_ERROR(status)) {
		time_event = NULL;
		printf("EFI timer unavailable\n");
	}
}

void
efi_time_fini(void)
{

	if (time_event == NULL)
		return;
	/* Cancel the timer */
	BS->SetTimer(time_event, TimerCancel, 0);
	BS->CloseEvent(time_event);
	time_event = NULL;
}

time_t
time(time_t *tloc)
{
	time_t t;

	t = curtime;
	if (tloc != NULL)
		*tloc = t;

	return (t);
}

time_t
getsecs(void)
{
    return time(0);
}

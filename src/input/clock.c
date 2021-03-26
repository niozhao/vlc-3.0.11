/*****************************************************************************
 * clock.c: Clock/System date convertions, stream management
 *****************************************************************************
 * Copyright (C) 1999-2008 VLC authors and VideoLAN
 * Copyright (C) 2008 Laurent Aimar
 * $Id$
 *
 * Authors: Christophe Massiot <massiot@via.ecp.fr>
 *          Laurent Aimar < fenrir _AT_ videolan _DOT_ org >
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2.1 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston MA 02110-1301, USA.
 *****************************************************************************/

/*****************************************************************************
 * Preamble
 *****************************************************************************/
#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include <vlc_common.h>
#include <vlc_input.h>
#include "clock.h"
#include <assert.h>


#if defined (__ANDROID__) || defined (ANDROID)

#include <android/log.h>
#ifndef  LOG_TAG
#define  LOG_TAG    "vlc_clock"
#define  LOGI(...)  __android_log_print(ANDROID_LOG_INFO,LOG_TAG,__VA_ARGS__)
#define  LOGE(...)  __android_log_print(ANDROID_LOG_ERROR,LOG_TAG,__VA_ARGS__)
#endif

#define GetCurrentThreadId()  pthread_self()
#define OutputDebugStringA(...)  LOGI(__VA_ARGS__)
#define _abs64(...) llabs(__VA_ARGS__)
#endif

/* TODO:
 * - clean up locking once clock code is stable
 *
 */

/*
 * DISCUSSION : SYNCHRONIZATION METHOD
 *
 * In some cases we can impose the pace of reading (when reading from a
 * file or a pipe), and for the synchronization we simply sleep() until
 * it is time to deliver the packet to the decoders. When reading from
 * the network, we must be read at the same pace as the server writes,
 * otherwise the kernel's buffer will trash packets. The risk is now to
 * overflow the input buffers in case the server goes too fast, that is
 * why we do these calculations :
 *
 * We compute a mean for the pcr because we want to eliminate the
 * network jitter and keep the low frequency variations. The mean is
 * in fact a low pass filter and the jitter is a high frequency signal
 * that is why it is eliminated by the filter/average.
 *
 * The low frequency variations enable us to synchronize the client clock
 * with the server clock because they represent the time variation between
 * the 2 clocks. Those variations (ie the filtered pcr) are used to compute
 * the presentation dates for the audio and video frames. With those dates
 * we can decode (or trash) the MPEG2 stream at "exactly" the same rate
 * as it is sent by the server and so we keep the synchronization between
 * the server and the client.
 *
 * It is a very important matter if you want to avoid underflow or overflow
 * in all the FIFOs, but it may be not enough.
 */

/* i_cr_average : Maximum number of samples used to compute the
 * dynamic average value.
 * We use the following formula :
 * new_average = (old_average * c_average + new_sample_value) / (c_average +1)
 */


/*****************************************************************************
 * Constants
 *****************************************************************************/

/* Maximum gap allowed between two CRs. */
#define CR_MAX_GAP (60 * CLOCK_FREQ)

/* Latency introduced on DVDs with CR == 0 on chapter change - this is from
 * my dice --Meuuh */
#define CR_MEAN_PTS_GAP (300000)

/* Rate (in 1/256) at which we will read faster to try to increase our
 * internal buffer (if we control the pace of the source).
 */
#define CR_BUFFERING_RATE (48)

/* Extra internal buffer value (in CLOCK_FREQ)
 * It is 60s max, remember as it is limited by the size it takes by es_out.c
 * it can be really large.
 */
//#define CR_BUFFERING_TARGET (60000000)
/* Due to some problems in es_out, we cannot use a large value yet */
#define CR_BUFFERING_TARGET (100000)

/*
*  the init value for decoder latency, 1 second is enough.
*  will auto adjust soon.
*/
#define INIT_DECODER_LATENCY (1000 * 1000)
/*****************************************************************************
 * Structures
 *****************************************************************************/

/**
 * This structure holds long term average
 */
typedef struct
{
    mtime_t i_value;    //the original vlc algorithm 
    int     i_residue;
    int     i_divider;

    mtime_t i_variance;    //drift's variance
    mtime_t i_means;       //drift's means.
    mtime_t i_residue_means;
    mtime_t i_residue_variance;
    mtime_t i_maxOffset;   //max network jetter
    mtime_t i_startCount;  //the time point to get i_maxOffset. if i_maxOffset not increase for a while, try decrease it!
    mtime_t i_count;
} average_t;

typedef struct
{
    mtime_t i_means;
    mtime_t i_residue;
    mtime_t i_count;
    mtime_t i_max;
    mtime_t i_maxCount;
}decoder_latency_statistics;

static void    AvgInit( average_t *, int i_divider );
static void    AvgClean( average_t * );

static void    AvgReset( average_t * );
static void    AvgUpdate( average_t *, mtime_t i_value );
static mtime_t AvgGet( average_t * );
static void    AvgRescale( average_t *, int i_divider );

static void latencyStatisUpdate(decoder_latency_statistics*, mtime_t i_latency);
static void latencyStatisInit(decoder_latency_statistics*);
/* */
typedef struct
{
    mtime_t i_stream;
    mtime_t i_system;
} clock_point_t;

static inline clock_point_t clock_point_Create( mtime_t i_stream, mtime_t i_system )
{
    clock_point_t p = { .i_stream = i_stream, .i_system = i_system };
    return p;
}

/* */
#define INPUT_CLOCK_LATE_COUNT (3)
#define BUFFERED_PTS_COUNT (100)

/* */
struct input_clock_t
{
    /* */
    vlc_mutex_t lock;

    /* Last point
     * It is used to detect unexpected stream discontinuities */
    clock_point_t last;

    /* Maximal timestamp returned by input_clock_ConvertTS (in system unit) */
    mtime_t i_ts_max;

    /* Amount of extra buffering expressed in stream clock */
    mtime_t i_buffering_duration;

    /* Clock drift */
    mtime_t i_next_drift_update;
    average_t drift;

    /* Late statistics */
    struct
    {
        mtime_t  pi_value[INPUT_CLOCK_LATE_COUNT];
        unsigned i_index;
    } late;

    int continuous_late_count;    //if this value large than 66 * 2, reset clock.

    /* Reference point */
    clock_point_t ref;
    bool          b_has_reference;

    /* External clock drift */
    mtime_t       i_external_clock;
    bool          b_has_external_clock;

    /* Current modifiers */
    bool    b_paused;
    int     i_rate;
    mtime_t i_pts_delay;
    mtime_t i_pause_date;

    /*decoder latency estimate*/
    struct
    {
        clock_point_t values[BUFFERED_PTS_COUNT];
        unsigned i_index;   //next insert position
    }clock_points;
    decoder_latency_statistics stat;
};



static mtime_t ClockStreamToSystem( input_clock_t *, mtime_t i_stream );
static mtime_t ClockSystemToStream( input_clock_t *, mtime_t i_system );

static mtime_t ClockGetTsOffset( input_clock_t * );
static mtime_t getDecoderLatency(input_clock_t* cl);
static void updateDecoderLatency(input_clock_t* cl, mtime_t i_stream);
static mtime_t getNetworkJitter(input_clock_t* cl);

char* toString(input_clock_t* cl)
{
    const int bufS = 4096;
    char* buf = (char*)malloc(bufS);
    if(buf != NULL)
    {
		memset(buf, 0, bufS);
		snprintf(buf, bufS, "%ld,i_ts_max(%lld),drift(%lld,%lld,%lld,%lld,%lld,%lld),decode late[%lld,%lld,%lld,%lld],ref(%lld,%lld),mdate(%lld)", GetCurrentThreadId(),cl->i_ts_max,
			cl->drift.i_value, cl->drift.i_means, cl->drift.i_variance, cl->drift.i_count, cl->drift.i_maxOffset, cl->drift.i_startCount,
			cl->stat.i_means, cl->stat.i_max, cl->stat.i_count, cl->stat.i_maxCount,
			cl->ref.i_stream, cl->ref.i_system, mdate());
    }
    return buf;
}

/*****************************************************************************
 * input_clock_New: create a new clock
 *****************************************************************************/
input_clock_t *input_clock_New( int i_rate )
{
    input_clock_t *cl = malloc( sizeof(*cl) );
    if( !cl )
        return NULL;

    vlc_mutex_init( &cl->lock );
    cl->b_has_reference = false;
    cl->ref = clock_point_Create( VLC_TS_INVALID, VLC_TS_INVALID );
    cl->b_has_external_clock = false;

    cl->last = clock_point_Create( VLC_TS_INVALID, VLC_TS_INVALID );

    cl->i_ts_max = VLC_TS_INVALID;

    cl->i_buffering_duration = 0;

    cl->i_next_drift_update = VLC_TS_INVALID;
    AvgInit( &cl->drift, 10 );

    cl->continuous_late_count = 0;
    cl->late.i_index = 0;
    for( int i = 0; i < INPUT_CLOCK_LATE_COUNT; i++ )
        cl->late.pi_value[i] = 0;

    cl->i_rate = i_rate;
    cl->i_pts_delay = 0;
    cl->b_paused = false;
    cl->i_pause_date = VLC_TS_INVALID;

    cl->clock_points.i_index = 0;
    memset(cl->clock_points.values, 0, sizeof(cl->clock_points.values));
    
    latencyStatisInit(&cl->stat);

    return cl;
}

/*****************************************************************************
 * input_clock_Delete: destroy a new clock
 *****************************************************************************/
void input_clock_Delete( input_clock_t *cl )
{
    AvgClean( &cl->drift );
    vlc_mutex_destroy( &cl->lock );
    free( cl );
}

void input_clock_Update_new(input_clock_t* cl, vlc_object_t* p_log,
    bool* pb_late,
    bool b_can_pace_control, bool b_buffering_allowed,
    mtime_t i_ck_stream, mtime_t i_ck_system)
{
    bool b_reset_reference = false;

    assert(i_ck_stream > VLC_TS_INVALID && i_ck_system > VLC_TS_INVALID);

    vlc_mutex_lock(&cl->lock);

    if (!cl->b_has_reference)
    {
        /* */
        b_reset_reference = true;
    }
    else if (cl->last.i_stream > VLC_TS_INVALID &&
        ((cl->last.i_stream - i_ck_stream) > CR_MAX_GAP ||
            (cl->last.i_stream - i_ck_stream) < -CR_MAX_GAP))
    {
        /* Stream discontinuity, for which we haven't received a
         * warning from the stream control facilities (dd-edited
         * stream ?). */
        msg_Warn(p_log, "clock gap, unexpected stream discontinuity");
        cl->i_ts_max = VLC_TS_INVALID;

        /* */
        msg_Warn(p_log, "feeding synchro with a new reference point trying to recover from clock gap");
        b_reset_reference = true;
    }

    /* */
    if (b_reset_reference)
    {
        cl->i_next_drift_update = VLC_TS_INVALID;
        AvgReset(&cl->drift);
        latencyStatisInit(&cl->stat);

        /* Feed synchro with a new reference point. */
        cl->b_has_reference = true;
        cl->ref = clock_point_Create(i_ck_stream,
            __MAX(cl->i_ts_max + CR_MEAN_PTS_GAP, i_ck_system));
        cl->b_has_external_clock = false;
    }

    /* Compute the drift between the stream clock and the system clock
     * when we don't control the source pace, if in buffering status, b_can_pace_control is true, so will not update drift*/
    if (!b_can_pace_control && cl->i_next_drift_update < i_ck_system)
    {
        const mtime_t i_converted = ClockSystemToStream(cl, i_ck_system);

        AvgUpdate(&cl->drift, i_converted - i_ck_stream);

        cl->i_next_drift_update = i_ck_system;    // changed, update every time!
    }

    /* Update the extra buffering value */
    if (!b_can_pace_control || b_reset_reference)
    {
        cl->i_buffering_duration = 0;
    }
    else if (b_buffering_allowed)
    {
        /* Try to bufferize more than necessary by reading
         * CR_BUFFERING_RATE/256 faster until we have CR_BUFFERING_TARGET.
         */
        const mtime_t i_duration = __MAX(i_ck_stream - cl->last.i_stream, 0);

        cl->i_buffering_duration += (i_duration * CR_BUFFERING_RATE + 255) / 256;
        if (cl->i_buffering_duration > CR_BUFFERING_TARGET)
            cl->i_buffering_duration = CR_BUFFERING_TARGET;
    }
    //fprintf( stderr, "input_clock_Update: %d :: %lld\n", b_buffering_allowed, cl->i_buffering_duration/1000 );

    /* */
    //clock_point_t lastPoint = cl->last;
    cl->last = clock_point_Create(i_ck_stream, i_ck_system);

    cl->clock_points.values[cl->clock_points.i_index] = cl->last;
    cl->clock_points.i_index = (cl->clock_points.i_index + 1) % BUFFERED_PTS_COUNT;

    *pb_late = false;

    //mtime_t refSystemOffset = i_ck_system - ClockStreamToSystem(cl, i_ck_stream);
    //char* buf2 = toString(cl);
    //msg_Warn(p_log, "frameTrace clock Update:\nbuf2:%s,\nnow(%lld,%lld),lastPointOffset(%lld,%lld),refSystemOffset:%lld,resetRef:%d, b_can_pace_control:%d,b_buffering_allowed:%d", buf2, i_ck_stream, i_ck_system, i_ck_stream - lastPoint.i_stream, i_ck_system - lastPoint.i_system, refSystemOffset, b_reset_reference, b_can_pace_control, b_buffering_allowed);
    //if (buf2 != NULL) free(buf2);
    vlc_mutex_unlock(&cl->lock);
}

/*****************************************************************************
 * input_clock_Update: manages a clock reference
 *
 *  i_ck_stream: date in stream clock
 *  i_ck_system: date in system clock
 *****************************************************************************/
void input_clock_Update( input_clock_t *cl, vlc_object_t *p_log,
                         bool *pb_late,
                         bool b_can_pace_control, bool b_buffering_allowed,
                         mtime_t i_ck_stream, mtime_t i_ck_system )
{

    return input_clock_Update_new(cl, p_log, pb_late, b_can_pace_control, b_buffering_allowed, i_ck_stream, i_ck_system);

    bool b_reset_reference = false;

    assert( i_ck_stream > VLC_TS_INVALID && i_ck_system > VLC_TS_INVALID );

    vlc_mutex_lock( &cl->lock );

    if( !cl->b_has_reference )
    {
        /* */
        b_reset_reference= true;
    }
    else if( cl->last.i_stream > VLC_TS_INVALID &&
             ( (cl->last.i_stream - i_ck_stream) > CR_MAX_GAP ||
               (cl->last.i_stream - i_ck_stream) < -CR_MAX_GAP ) )
    {
        /* Stream discontinuity, for which we haven't received a
         * warning from the stream control facilities (dd-edited
         * stream ?). */
        msg_Warn( p_log, "clock gap, unexpected stream discontinuity" );
        cl->i_ts_max = VLC_TS_INVALID;

        /* */
        msg_Warn( p_log, "feeding synchro with a new reference point trying to recover from clock gap" );
        b_reset_reference= true;
    }

    /* */
    if( b_reset_reference )
    {
        cl->i_next_drift_update = VLC_TS_INVALID;
        AvgReset( &cl->drift );
        latencyStatisInit(&cl->stat);

        /* Feed synchro with a new reference point. */
        cl->b_has_reference = true;
        cl->ref = clock_point_Create( i_ck_stream,
                                      __MAX( cl->i_ts_max + CR_MEAN_PTS_GAP, i_ck_system ) );
        cl->b_has_external_clock = false;
    }

    /* Compute the drift between the stream clock and the system clock
     * when we don't control the source pace */
    if( !b_can_pace_control && cl->i_next_drift_update < i_ck_system )
    {
        const mtime_t i_converted = ClockSystemToStream( cl, i_ck_system );

        AvgUpdate( &cl->drift, i_converted - i_ck_stream );

        cl->i_next_drift_update = i_ck_system + CLOCK_FREQ/5; /* FIXME why that */
    }

    /* Update the extra buffering value */
    if( !b_can_pace_control || b_reset_reference )
    {
        cl->i_buffering_duration = 0;
    }
    else if( b_buffering_allowed )
    {
        /* Try to bufferize more than necessary by reading
         * CR_BUFFERING_RATE/256 faster until we have CR_BUFFERING_TARGET.
         */
        const mtime_t i_duration = __MAX( i_ck_stream - cl->last.i_stream, 0 );

        cl->i_buffering_duration += ( i_duration * CR_BUFFERING_RATE + 255 ) / 256;
        if( cl->i_buffering_duration > CR_BUFFERING_TARGET )
            cl->i_buffering_duration = CR_BUFFERING_TARGET;
    }
    //fprintf( stderr, "input_clock_Update: %d :: %lld\n", b_buffering_allowed, cl->i_buffering_duration/1000 );

    /* */
    cl->last = clock_point_Create( i_ck_stream, i_ck_system );

    /* It does not take the decoder latency into account but it is not really
     * the goal of the clock here */
    const mtime_t i_system_expected = ClockStreamToSystem( cl, i_ck_stream + AvgGet( &cl->drift ) );
    const mtime_t i_late = ( i_ck_system - cl->i_pts_delay ) - i_system_expected;
    *pb_late = i_late > 0;
    if( i_late > 0 )
    {
        cl->late.pi_value[cl->late.i_index] = i_late;
        cl->late.i_index = ( cl->late.i_index + 1 ) % INPUT_CLOCK_LATE_COUNT;
    }

    vlc_mutex_unlock( &cl->lock );
}

/*****************************************************************************
 * input_clock_Reset:
 *****************************************************************************/
void input_clock_Reset( input_clock_t *cl )
{
    vlc_mutex_lock( &cl->lock );

    cl->b_has_reference = false;
    cl->ref = clock_point_Create( VLC_TS_INVALID, VLC_TS_INVALID );
    cl->b_has_external_clock = false;
    cl->i_ts_max = VLC_TS_INVALID;

    vlc_mutex_unlock( &cl->lock );
}

/*****************************************************************************
 * input_clock_ChangeRate:
 *****************************************************************************/
void input_clock_ChangeRate( input_clock_t *cl, int i_rate )
{
    vlc_mutex_lock( &cl->lock );

    if( cl->b_has_reference )
    {
        /* Move the reference point (as if we were playing at the new rate
         * from the start */
        cl->ref.i_system = cl->last.i_system - (cl->last.i_system - cl->ref.i_system) * i_rate / cl->i_rate;
    }
    cl->i_rate = i_rate;

    vlc_mutex_unlock( &cl->lock );
}

/*****************************************************************************
 * input_clock_ChangePause:
 *****************************************************************************/
void input_clock_ChangePause( input_clock_t *cl, bool b_paused, mtime_t i_date )
{
    vlc_mutex_lock( &cl->lock );
    assert( (!cl->b_paused) != (!b_paused) );

    if( cl->b_paused )
    {
        const mtime_t i_duration = i_date - cl->i_pause_date;

        if( cl->b_has_reference && i_duration > 0 )
        {
            cl->ref.i_system += i_duration;
            cl->last.i_system += i_duration;
        }
    }
    cl->i_pause_date = i_date;
    cl->b_paused = b_paused;

    vlc_mutex_unlock( &cl->lock );
}

/*****************************************************************************
 * input_clock_GetWakeup
 *****************************************************************************/
mtime_t input_clock_GetWakeup( input_clock_t *cl )
{
    mtime_t i_wakeup = 0;

    vlc_mutex_lock( &cl->lock );

    /* Synchronized, we can wait */
    if( cl->b_has_reference )
        i_wakeup = ClockStreamToSystem( cl, cl->last.i_stream + AvgGet( &cl->drift ) - cl->i_buffering_duration );

    vlc_mutex_unlock( &cl->lock );

    return i_wakeup;
}

int input_clock_ConvertTS_new(vlc_object_t* p_object, input_clock_t* cl,
	int* pi_rate, mtime_t* pi_ts0, mtime_t* pi_ts1,
	mtime_t i_ts_bound, bool bVideoES)
{
	assert(pi_ts0);
	vlc_mutex_lock(&cl->lock);

	if (pi_rate)
		*pi_rate = cl->i_rate;

	if (!cl->b_has_reference)
	{
		vlc_mutex_unlock(&cl->lock);
		msg_Err(p_object, "Timestamp conversion failed for %"PRId64": "
			"no reference clock", *pi_ts0);
		*pi_ts0 = VLC_TS_INVALID;
		if (pi_ts1)
			*pi_ts1 = VLC_TS_INVALID;
		return VLC_EGENERIC;
	}

	/* calculate decoder latency*/
    if(bVideoES)
	    updateDecoderLatency(cl, *pi_ts0);  //use current stream time to update decoder latency

	/* */
	const mtime_t i_ts_buffering = cl->i_buffering_duration * cl->i_rate / INPUT_RATE_DEFAULT;
	const mtime_t i_ts_delay = ClockGetTsOffset(cl) + getNetworkJitter(cl) + getDecoderLatency(cl);  //changed!

	/* */
	if (*pi_ts0 > VLC_TS_INVALID)
	{
		*pi_ts0 = ClockStreamToSystem(cl, *pi_ts0 + AvgGet(&cl->drift));
		if (*pi_ts0 > cl->i_ts_max)
			cl->i_ts_max = *pi_ts0;
		*pi_ts0 += i_ts_delay;
	}

	/* XXX we do not update i_ts_max on purpose */
	if (pi_ts1 && *pi_ts1 > VLC_TS_INVALID)
	{
		*pi_ts1 = ClockStreamToSystem(cl, *pi_ts1 + AvgGet(&cl->drift)) +
			i_ts_delay;
	}
    //check auto-adjust algorithm validity!
    if (mdate() - *pi_ts0 >= 16 * 1000)  //this picture is too late to be display
    {
        if (cl->continuous_late_count++ > 66 * 2)
        {
            char* thisStatus = toString(cl);
			msg_Err(p_object, "convert stream to system time continuous late(2 second)!algorithm error, reset clock,current clock:%s", thisStatus);
            if (thisStatus) free(thisStatus);
            input_clock_Reset(cl);
            cl->continuous_late_count = 0;
        }
    }
    else
        cl->continuous_late_count = 0;

	vlc_mutex_unlock(&cl->lock);

	/* Check ts validity */
	if (i_ts_bound != INT64_MAX && *pi_ts0 > VLC_TS_INVALID) {
		if (*pi_ts0 >= mdate() + i_ts_delay + i_ts_buffering + i_ts_bound) {
			msg_Err(p_object,
				"Timestamp conversion failed (delay %"PRId64", buffering "
				"%"PRId64", bound %"PRId64")",
				i_ts_delay, i_ts_buffering, i_ts_bound);
			return VLC_EGENERIC;
		}
	}

	return VLC_SUCCESS;
}

/*****************************************************************************
 * input_clock_ConvertTS
 *****************************************************************************/
int input_clock_ConvertTS( vlc_object_t *p_object, input_clock_t *cl,
                           int *pi_rate, mtime_t *pi_ts0, mtime_t *pi_ts1,
                           mtime_t i_ts_bound, bool bVideoES)
{
    return input_clock_ConvertTS_new(p_object,cl,pi_rate,pi_ts0,pi_ts1,i_ts_bound, bVideoES);

    assert( pi_ts0 );
    vlc_mutex_lock( &cl->lock );

    if( pi_rate )
        *pi_rate = cl->i_rate;

    if( !cl->b_has_reference )
    {
        vlc_mutex_unlock( &cl->lock );
        msg_Err(p_object, "Timestamp conversion failed for %"PRId64": "
                "no reference clock", *pi_ts0);
        *pi_ts0 = VLC_TS_INVALID;
        if( pi_ts1 )
            *pi_ts1 = VLC_TS_INVALID;
        return VLC_EGENERIC;
    }

    /* */
    const mtime_t i_ts_buffering = cl->i_buffering_duration * cl->i_rate / INPUT_RATE_DEFAULT;
    const mtime_t i_ts_delay = cl->i_pts_delay + ClockGetTsOffset( cl );

    /* */
    if( *pi_ts0 > VLC_TS_INVALID )
    {
        *pi_ts0 = ClockStreamToSystem( cl, *pi_ts0 + AvgGet( &cl->drift ) );
        if( *pi_ts0 > cl->i_ts_max )
            cl->i_ts_max = *pi_ts0;
        *pi_ts0 += i_ts_delay;
    }

    /* XXX we do not update i_ts_max on purpose */
    if( pi_ts1 && *pi_ts1 > VLC_TS_INVALID )
    {
        *pi_ts1 = ClockStreamToSystem( cl, *pi_ts1 + AvgGet( &cl->drift ) ) +
                  i_ts_delay;
    }

    vlc_mutex_unlock( &cl->lock );

    /* Check ts validity */
    if (i_ts_bound != INT64_MAX && *pi_ts0 > VLC_TS_INVALID) {
        if (*pi_ts0 >= mdate() + i_ts_delay + i_ts_buffering + i_ts_bound) {
            msg_Err(p_object,
                "Timestamp conversion failed (delay %"PRId64", buffering "
                "%"PRId64", bound %"PRId64")",
                i_ts_delay, i_ts_buffering, i_ts_bound);
            return VLC_EGENERIC;
        }
    }

    return VLC_SUCCESS;
}
/*****************************************************************************
 * input_clock_GetRate: Return current rate
 *****************************************************************************/
int input_clock_GetRate( input_clock_t *cl )
{
    int i_rate;

    vlc_mutex_lock( &cl->lock );
    i_rate = cl->i_rate;
    vlc_mutex_unlock( &cl->lock );

    return i_rate;
}

int input_clock_GetState( input_clock_t *cl,
                          mtime_t *pi_stream_start, mtime_t *pi_system_start,
                          mtime_t *pi_stream_duration, mtime_t *pi_system_duration )
{
    vlc_mutex_lock( &cl->lock );

    if( !cl->b_has_reference )
    {
        vlc_mutex_unlock( &cl->lock );
        return VLC_EGENERIC;
    }

    *pi_stream_start = cl->ref.i_stream;
    *pi_system_start = cl->ref.i_system;

    *pi_stream_duration = cl->last.i_stream - cl->ref.i_stream;
    *pi_system_duration = cl->last.i_system - cl->ref.i_system;

    vlc_mutex_unlock( &cl->lock );

    return VLC_SUCCESS;
}

void input_clock_ChangeDriftStartPoint(input_clock_t* cl,mtime_t i_system)
{
	vlc_mutex_lock(&cl->lock);

	assert(cl->b_has_reference);
	
    cl->i_next_drift_update = i_system + 33*1000;   //start update drift 33ms later!

	//char* buf[4096] = { 0 };
	//snprintf(buf, 4096, "clock delay drift start point to :i_system:%lld\n", i_system);
	//OutputDebugStringA(buf);

	vlc_mutex_unlock(&cl->lock); 
}


void input_clock_ChangeSystemOrigin( input_clock_t *cl, bool b_absolute, mtime_t i_system )
{
    vlc_mutex_lock( &cl->lock );

    assert( cl->b_has_reference );
    mtime_t i_offset;
    if( b_absolute )
    {
        i_offset = i_system - cl->ref.i_system - ClockGetTsOffset( cl );
    }
    else
    {
        if( !cl->b_has_external_clock )
        {
            cl->b_has_external_clock = true;
            cl->i_external_clock     = i_system;
        }
        i_offset = i_system - cl->i_external_clock;
    }

    cl->ref.i_system += i_offset;
    cl->last.i_system += i_offset;

    vlc_mutex_unlock( &cl->lock );
}

void input_clock_GetSystemOrigin( input_clock_t *cl, mtime_t *pi_system, mtime_t *pi_delay )
{
    vlc_mutex_lock( &cl->lock );

    assert( cl->b_has_reference );

    *pi_system = cl->ref.i_system;
    if( pi_delay )
        *pi_delay  = cl->i_pts_delay;

    vlc_mutex_unlock( &cl->lock );
}

//#warning "input_clock_SetJitter needs more work"
void input_clock_SetJitter( input_clock_t *cl,
                            mtime_t i_pts_delay, int i_cr_average )
{
    vlc_mutex_lock( &cl->lock );

    /* Update late observations */
    const mtime_t i_delay_delta = i_pts_delay - cl->i_pts_delay;
    mtime_t pi_late[INPUT_CLOCK_LATE_COUNT];
    for( int i = 0; i < INPUT_CLOCK_LATE_COUNT; i++ )
        pi_late[i] = __MAX( cl->late.pi_value[(cl->late.i_index + 1 + i)%INPUT_CLOCK_LATE_COUNT] - i_delay_delta, 0 );

    for( int i = 0; i < INPUT_CLOCK_LATE_COUNT; i++ )
        cl->late.pi_value[i] = 0;
    cl->late.i_index = 0;

    for( int i = 0; i < INPUT_CLOCK_LATE_COUNT; i++ )
    {
        if( pi_late[i] <= 0 )
            continue;
        cl->late.pi_value[cl->late.i_index] = pi_late[i];
        cl->late.i_index = ( cl->late.i_index + 1 ) % INPUT_CLOCK_LATE_COUNT;
    }

    /* TODO always save the value, and when rebuffering use the new one if smaller
     * TODO when increasing -> force rebuffering
     */
    if( cl->i_pts_delay < i_pts_delay )
        cl->i_pts_delay = i_pts_delay;

    /* */
    if( i_cr_average < 10 )
        i_cr_average = 10;

    if( cl->drift.i_divider != i_cr_average )
        AvgRescale( &cl->drift, i_cr_average );

    vlc_mutex_unlock( &cl->lock );
}

mtime_t input_clock_GetJitter( input_clock_t *cl )
{
    vlc_mutex_lock( &cl->lock );

#if INPUT_CLOCK_LATE_COUNT != 3
#   error "unsupported INPUT_CLOCK_LATE_COUNT"
#endif
    /* Find the median of the last late values
     * It works pretty well at rejecting bad values
     *
     * XXX we only increase pts_delay over time, decreasing it is
     * not that easy if we want to be robust.
     */
    const mtime_t *p = cl->late.pi_value;
    mtime_t i_late_median = p[0] + p[1] + p[2] - __MIN(__MIN(p[0],p[1]),p[2]) - __MAX(__MAX(p[0],p[1]),p[2]);
    mtime_t i_pts_delay = cl->i_pts_delay ;

    vlc_mutex_unlock( &cl->lock );

    return i_pts_delay + i_late_median;
}

/*****************************************************************************
 * ClockStreamToSystem: converts a movie clock to system date
 *****************************************************************************/
static mtime_t ClockStreamToSystem( input_clock_t *cl, mtime_t i_stream )
{
    if( !cl->b_has_reference )
        return VLC_TS_INVALID;

    return ( i_stream - cl->ref.i_stream ) * cl->i_rate / INPUT_RATE_DEFAULT +
           cl->ref.i_system;
}

/*****************************************************************************
 * ClockSystemToStream: converts a system date to movie clock
 *****************************************************************************
 * Caution : a valid reference point is needed for this to operate.
 *****************************************************************************/
static mtime_t ClockSystemToStream( input_clock_t *cl, mtime_t i_system )
{
    assert( cl->b_has_reference );
    return ( i_system - cl->ref.i_system ) * INPUT_RATE_DEFAULT / cl->i_rate +
            cl->ref.i_stream;
}

/**
 * It returns timestamp display offset due to ref/last modfied on rate changes
 * It ensures that currently converted dates are not changed.
 */
static mtime_t ClockGetTsOffset( input_clock_t *cl )
{
    return cl->i_pts_delay * ( cl->i_rate - INPUT_RATE_DEFAULT ) / INPUT_RATE_DEFAULT;
}

/*****************************************************************************
 * Long term average helpers
 *****************************************************************************/
static void AvgInit( average_t *p_avg, int i_divider )
{
    p_avg->i_divider = i_divider;
    AvgReset( p_avg );
}
static void AvgClean( average_t *p_avg )
{
    VLC_UNUSED(p_avg);
}
static void AvgReset( average_t *p_avg )
{
    p_avg->i_value = 0;
    p_avg->i_residue = 0;
    p_avg->i_count = 0;
    p_avg->i_means = 0;
    p_avg->i_variance = 0;
    p_avg->i_residue_means = 0;
    p_avg->i_residue_variance = 0;
    p_avg->i_maxOffset = 0;
    p_avg->i_startCount = 0;
}
static void AvgUpdate( average_t *p_avg, mtime_t i_value )
{
    //old
	const int i_f0 = __MIN(p_avg->i_divider - 1, p_avg->i_count);
	const int i_f1 = p_avg->i_divider - i_f0;

	const mtime_t i_tmp = i_f0 * p_avg->i_value + i_f1 * i_value + p_avg->i_residue;

	p_avg->i_value = i_tmp / p_avg->i_divider;
	p_avg->i_residue = i_tmp % p_avg->i_divider;

    //new 
    const int STATICS_CIRCLE = 10 * 30;   //10 second
    int index = p_avg->i_count % STATICS_CIRCLE;
    mtime_t last_variance = 0;
    if (0 == index)
    {
        //new circle
        last_variance = p_avg->i_variance / 2;  //use the last one as the new circle init variance.
        p_avg->i_residue_means = 0;
        p_avg->i_means = 0;
        p_avg->i_variance = 0;
        p_avg->i_residue_variance = 0;
    }
	mtime_t i_tmp_new = p_avg->i_means * index + i_value + p_avg->i_residue_means;
	p_avg->i_means = i_tmp_new / (index + 1);
	p_avg->i_residue_means = i_tmp_new % (index + 1);
    mtime_t offset = _abs64(i_value - p_avg->i_means);
    i_tmp_new = p_avg->i_variance * index + offset * offset + p_avg->i_residue_variance + last_variance;
    p_avg->i_variance = i_tmp_new / (index + 1);
    p_avg->i_residue_variance = i_tmp_new % (index + 1);
    
    if (offset > p_avg->i_maxOffset)
    {
        p_avg->i_maxOffset = (offset * 3 + p_avg->i_maxOffset) / 4;  //weighting calculate
        p_avg->i_startCount = p_avg->i_count; 
    }

    if (p_avg->i_count - p_avg->i_startCount >= 2) 
    {
        //try decrease i_maxOffset.
        p_avg->i_maxOffset = sqrt(p_avg->i_variance);   //70%'s data is ok!
        p_avg->i_startCount = p_avg->i_count;
    }
    
    p_avg->i_count++;
}
static mtime_t AvgGet( average_t *p_avg )
{
    //return p_avg->i_value;
    return p_avg->i_means;
}

static void AvgRescale( average_t *p_avg, int i_divider )
{
    const mtime_t i_tmp = p_avg->i_value * p_avg->i_divider + p_avg->i_residue;

    p_avg->i_divider = i_divider;
    p_avg->i_value   = i_tmp / p_avg->i_divider;
    p_avg->i_residue = i_tmp % p_avg->i_divider;
}

static void updateDecoderLatency(input_clock_t* cl, mtime_t i_stream)
{
    bool bFind = false;
    mtime_t the_i_system = 0;
    unsigned maxPtsIndex = (cl->clock_points.i_index - 1 + BUFFERED_PTS_COUNT) % BUFFERED_PTS_COUNT;
    unsigned minPtsIndex = cl->clock_points.i_index;

    //search direction is from big stream value to small
    unsigned i = maxPtsIndex;
	do
	{
		mtime_t cur = cl->clock_points.values[i].i_stream;
        if (cur == i_stream)
        {
            the_i_system = cl->clock_points.values[i].i_system;
            break;
        }
        else if (cur < i_stream)
        {
            //not find the stream exactly, use the nearest one to estimate it 
			the_i_system = i_stream - cl->clock_points.values[i].i_stream + cl->clock_points.values[i].i_system; 
			//the_i_system = ClockStreamToSystem(cl, i_stream + AvgGet(&cl->drift));
            break;
        }
        else
            i = (i - 1 + BUFFERED_PTS_COUNT) % BUFFERED_PTS_COUNT;
    } while (i != maxPtsIndex);  ////already search a loop, break
    
    mtime_t current = mdate();
    mtime_t thisLatency = current + 500 - the_i_system;  //mdate() precision is ms,+ 500 to make thisLatency not zero
    //char buf[1024] = {0};
    //snprintf(buf, 1024, "updateDecoderLatency, input istream:%lld, get system:%lld, current:%lld, thisLatency:%lld, jitter:%lld, latency:%lld\n", i_stream, the_i_system, current, thisLatency, getNetworkJitter(cl),getDecoderLatency(cl));
    //OutputDebugStringA(buf);
    latencyStatisUpdate(&cl->stat,thisLatency);
}

static void latencyStatisUpdate(decoder_latency_statistics* statics, mtime_t latency)
{
    //drop the first 120 update, those value are not correctly, will increase error! about 2 second in 30fps.
    const int drop_count = 0;  

    mtime_t count = statics->i_count - drop_count;
    if (count >= 0)
    {
        const int DECODER_LATENCY_STATISTICS_CIRCLE = 30 * 3 * 2;  // 2 : a frame will call this function twice
        int index = count % DECODER_LATENCY_STATISTICS_CIRCLE;
        if (0 == index)
        {
            //latency = (latency + statics->i_means) / 2;  //use this value as the new circle init value
            statics->i_means = 0;
            statics->i_residue = 0;
        }
        const mtime_t tmp = statics->i_means * index + latency + statics->i_residue;
        statics->i_means = tmp / (index + 1);
        statics->i_residue = tmp % (index + 1);
        if (latency > statics->i_max || INIT_DECODER_LATENCY == statics->i_max)
        {
            if(INIT_DECODER_LATENCY == statics->i_max)  //first call
                statics->i_max = statics->i_means; 
            else
                statics->i_max = (latency * 3 + statics->i_max) / 4;
			statics->i_maxCount = statics->i_count;
        }

        if (statics->i_count - statics->i_maxCount >= 1 * 2) 
        {
            //try decrease
            statics->i_max = (statics->i_means + latency) / 2;  // the newest latency is more suitable
			statics->i_maxCount = statics->i_count;
        }
    }
    else
    {
        
    }
    statics->i_count++;
}

static void latencyStatisInit(decoder_latency_statistics* st)
{
    st->i_count = 0;
    st->i_means = INIT_DECODER_LATENCY;
    st->i_residue = 0;
    st->i_max = INIT_DECODER_LATENCY;
    st->i_maxCount = 205 * 1000;    //set this value, the purpose is: update it after i_max is valid
}

static mtime_t getDecoderLatency(input_clock_t* cl)
{
    return cl->stat.i_max;
}

static mtime_t getNetworkJitter(input_clock_t* cl)
{
    return cl->drift.i_maxOffset;
}

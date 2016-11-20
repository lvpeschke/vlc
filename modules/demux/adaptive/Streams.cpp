/*
 * Streams.cpp
 *****************************************************************************
 * Copyright (C) 2014 - VideoLAN and VLC authors
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published
 * by the Free Software Foundation; either version 2.1 of the License, or
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

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include "Streams.hpp"
#include "logic/AbstractAdaptationLogic.h"
#include "http/HTTPConnection.hpp"
#include "http/HTTPConnectionManager.h"
#include "playlist/BaseRepresentation.h"
#include "playlist/SegmentChunk.hpp"
#include "plumbing/SourceStream.hpp"
#include "plumbing/CommandsQueue.hpp"
#include "tools/Debug.hpp"
#include <vlc_demux.h>
/* LVP added */
#include <iostream>
#include <ctime>
#include <cinttypes>

using namespace adaptive;
using namespace adaptive::http;

AbstractStream::AbstractStream(demux_t * demux_)
{
    p_realdemux = demux_;
    format = StreamFormat::UNSUPPORTED;
    currentChunk = NULL;
    eof = false;
    dead = false;
    disabled = false;
    discontinuity = false;
    needrestart = false;
    inrestart = false;
    segmentTracker = NULL;
    demuxersource = NULL;
    commandsqueue = NULL;
    demuxer = NULL;
    fakeesout = NULL;
    last_buffer_status = buffering_lessthanmin;
    vlc_mutex_init(&lock);
}

bool AbstractStream::init(const StreamFormat &format_, SegmentTracker *tracker, AbstractConnectionManager *conn)
{
    /* Don't even try if not supported or already init */
    if((unsigned)format_ == StreamFormat::UNSUPPORTED || demuxersource)
        return false;

    demuxersource = new (std::nothrow) ChunksSourceStream( VLC_OBJECT(p_realdemux), this );
    if(demuxersource)
    {
        CommandsFactory *factory = new (std::nothrow) CommandsFactory();
        if(factory)
        {
            commandsqueue = new (std::nothrow) CommandsQueue(factory);
            if(commandsqueue)
            {
                fakeesout = new (std::nothrow) FakeESOut(p_realdemux->out, commandsqueue);
                if(fakeesout)
                {
                    /* All successfull */
                    fakeesout->setExtraInfoProvider( this );
                    format = format_;
                    segmentTracker = tracker;
                    segmentTracker->registerListener(this);
                    segmentTracker->notifyBufferingState(true);
                    connManager = conn;
                    return true;
                }
                delete commandsqueue;
                commandsqueue = NULL;
            }
            else
            {
                delete factory;
            }
        }
        delete demuxersource;
    }

    return false;
}

AbstractStream::~AbstractStream()
{
    delete currentChunk;
    if(segmentTracker)
        segmentTracker->notifyBufferingState(false);
    delete segmentTracker;

    delete demuxer;
    delete demuxersource;
    delete fakeesout;
    delete commandsqueue;

    vlc_mutex_destroy(&lock);
}

void AbstractStream::prepareRestart(bool b_discontinuity)
{
    if(demuxer)
    {
        /* Enqueue Del Commands for all current ES */
        demuxer->drain();
        setTimeOffset(true);
        /* Enqueue Del Commands for all current ES */
        fakeesout->scheduleAllForDeletion();
        if(b_discontinuity)
            fakeesout->schedulePCRReset();
        commandsqueue->Commit();
        /* ignoring demuxer's own Del commands */
        commandsqueue->setDrop(true);
        delete demuxer;
        commandsqueue->setDrop(false);
        demuxer = NULL;
    }
}

void AbstractStream::setLanguage(const std::string &lang)
{
    language = lang;
}

void AbstractStream::setDescription(const std::string &desc)
{
    description = desc;
}

mtime_t AbstractStream::getPCR() const
{
    vlc_mutex_lock(const_cast<vlc_mutex_t *>(&lock));
    mtime_t pcr = isDisabled() ? VLC_TS_INVALID : commandsqueue->getPCR();
    vlc_mutex_unlock(const_cast<vlc_mutex_t *>(&lock));
    return pcr;
}

mtime_t AbstractStream::getMinAheadTime() const
{
    if(!segmentTracker)
        return 0;
    return segmentTracker->getMinAheadTime();
}

mtime_t AbstractStream::getFirstDTS() const
{
    mtime_t dts;
    vlc_mutex_lock(const_cast<vlc_mutex_t *>(&lock));
    if(isDisabled())
    {
        dts = VLC_TS_INVALID;
    }
    else
    {
        dts = commandsqueue->getFirstDTS();
        if(dts == VLC_TS_INVALID)
            dts = commandsqueue->getPCR();
    }
    vlc_mutex_unlock(const_cast<vlc_mutex_t *>(&lock));
    return dts;
}

int AbstractStream::esCount() const
{
    return fakeesout->esCount();
}

bool AbstractStream::seekAble() const
{
    return (demuxer &&
            !fakeesout->restarting() &&
            !discontinuity &&
            !commandsqueue->isFlushing() );
}

bool AbstractStream::isSelected() const
{
    return fakeesout->hasSelectedEs();
}

bool AbstractStream::reactivate(mtime_t basetime)
{
    if(setPosition(basetime, false))
    {
        setDisabled(false);
        return true;
    }
    else
    {
        eof = true; /* can't reactivate */
        return false;
    }
}

bool AbstractStream::startDemux()
{
	/* LVP added */
	msg_Dbg(p_realdemux, "LVP entered AbstractStream::startDemux()");
	
    if(demuxer)
        return false;

    demuxersource->Reset();
    demuxer = createDemux(format);
    if(!demuxer && format != StreamFormat())
        msg_Err(p_realdemux, "Failed to create demuxer %p %s", (void *)demuxer,
                format.str().c_str());

    return !!demuxer;
}

bool AbstractStream::restartDemux()
{
    bool b_ret = true;
    if(!demuxer)
    {
        b_ret = startDemux();
    }
    else if(demuxer->needsRestartOnSeek())
    {
        inrestart = true;
        /* Push all ES as recycling candidates */
        fakeesout->recycleAll();
        /* Restart with ignoring es_Del pushes to queue when terminating demux */
        commandsqueue->setDrop(true);
        demuxer->destroy();
        commandsqueue->setDrop(false);
        b_ret = demuxer->create();
        inrestart = false;
    }
    else
    {
        commandsqueue->Commit();
    }
    return b_ret;
}

void AbstractStream::setDisabled(bool b)
{
    if(disabled != b)
        segmentTracker->notifyBufferingState(!b);
    disabled = b;
}

bool AbstractStream::isDisabled() const
{
    return dead || disabled;
}

bool AbstractStream::canActivate() const
{
    return !dead;
}

bool AbstractStream::drain()
{
    return fakeesout->drain();
}

AbstractStream::buffering_status AbstractStream::getLastBufferStatus() const
{
    return last_buffer_status;
}

mtime_t AbstractStream::getDemuxedAmount() const
{
    return commandsqueue->getDemuxedAmount();
}

AbstractStream::buffering_status AbstractStream::bufferize(mtime_t nz_deadline,
                                                           unsigned i_min_buffering, unsigned i_extra_buffering)
{
    last_buffer_status = doBufferize(nz_deadline, i_min_buffering, i_extra_buffering);
    return last_buffer_status;
}

AbstractStream::buffering_status AbstractStream::doBufferize(mtime_t nz_deadline,
                                                             unsigned i_min_buffering, unsigned i_extra_buffering)
{
    vlc_mutex_lock(&lock);

    /* Ensure it is configured */
    if(!segmentTracker || !connManager || dead)
    {
        vlc_mutex_unlock(&lock);
        return AbstractStream::buffering_end;
    }

    /* Disable streams that are not selected (alternate streams) */
    if(esCount() && !isSelected() && !fakeesout->restarting())
    {
        setDisabled(true);
        segmentTracker->reset();
        commandsqueue->Abort(false);
        msg_Dbg(p_realdemux, "deactivating stream %s", format.str().c_str());
        vlc_mutex_unlock(&lock);
        return AbstractStream::buffering_end;
    }

    if(commandsqueue->isFlushing())
    {
        vlc_mutex_unlock(&lock);
        return AbstractStream::buffering_suspended;
    }

    if(!demuxer)
    {
        format = segmentTracker->getCurrentFormat();
        if(!startDemux())
        {
            /* If demux fails because of probing failure / wrong format*/
            if(discontinuity)
            {
                msg_Dbg( p_realdemux, "Flushing on format change" );
                prepareRestart();
                discontinuity = false;
                commandsqueue->setFlush();
                vlc_mutex_unlock(&lock);
                return AbstractStream::buffering_ongoing;
            }
            dead = true; /* Prevent further retries */
            commandsqueue->setEOF();
            vlc_mutex_unlock(&lock);
            return AbstractStream::buffering_end;
        }
    }

    const int64_t i_total_buffering = i_min_buffering + i_extra_buffering;

    mtime_t i_demuxed = commandsqueue->getDemuxedAmount();
    segmentTracker->notifyBufferingLevel(i_demuxed, i_total_buffering);
    if(i_demuxed < i_total_buffering) /* not already demuxed */
    {
        if(!segmentTracker->segmentsListReady()) /* Live Streams */
        {
            vlc_mutex_unlock(&lock);
            return AbstractStream::buffering_suspended;
        }

        nz_deadline = commandsqueue->getBufferingLevel() +
                     (i_total_buffering - commandsqueue->getDemuxedAmount()) / (CLOCK_FREQ/4);

        /* need to read, demuxer still buffering, ... */
        vlc_mutex_unlock(&lock);
        int i_ret = demuxer->demux(nz_deadline);
        vlc_mutex_lock(&lock);
        if(i_ret != VLC_DEMUXER_SUCCESS)
        {
            if(discontinuity || needrestart)
            {
                msg_Dbg(p_realdemux, "Restarting demuxer");
                prepareRestart(discontinuity);
                if(discontinuity)
                {
                    msg_Dbg(p_realdemux, "Flushing on discontinuity");
                    commandsqueue->setFlush();
                    discontinuity = false;
                }
                needrestart = false;
                vlc_mutex_unlock(&lock);
                return AbstractStream::buffering_ongoing;
            }
            commandsqueue->setEOF();
            vlc_mutex_unlock(&lock);
            return AbstractStream::buffering_end;
        }
        i_demuxed = commandsqueue->getDemuxedAmount();
        segmentTracker->notifyBufferingLevel(i_demuxed, i_total_buffering);
    }
    vlc_mutex_unlock(&lock);

    if(i_demuxed < i_total_buffering) /* need to read more */
    {
        if(i_demuxed < i_min_buffering)
            return AbstractStream::buffering_lessthanmin; /* high prio */
        return AbstractStream::buffering_ongoing;
    }
    return AbstractStream::buffering_full;
}

AbstractStream::status AbstractStream::dequeue(mtime_t nz_deadline, mtime_t *pi_pcr)
{
    vlc_mutex_locker locker(&lock);

    *pi_pcr = nz_deadline;

    if(commandsqueue->isFlushing())
    {
        *pi_pcr = commandsqueue->Process(p_realdemux->out, VLC_TS_0 + nz_deadline);
        if(!commandsqueue->isEmpty())
            return AbstractStream::status_demuxed;

        if(!commandsqueue->isEOF())
        {
            commandsqueue->Abort(true); /* reset buffering level and flags */
            return AbstractStream::status_discontinuity;
        }
    }

    if(isDisabled() || commandsqueue->isEOF())
    {
        *pi_pcr = nz_deadline;
        return AbstractStream::status_eof;
    }

    AdvDebug(msg_Dbg(p_realdemux, "Stream %s pcr %ld dts %ld deadline %ld buflevel %ld",
                     description.c_str(), commandsqueue->getPCR(), commandsqueue->getFirstDTS(),
                     nz_deadline, commandsqueue->getBufferingLevel()));
					 
    /* LVP added, TFE & TFE DEBUG */
    // mdate, desc (text), pcr, dts, buflevel, nz deadline, demuxed amount
    // demuxed amount = bufferinglevel - getFirstDTS
    msg_Info(p_realdemux, "TFE stream, %" PRId64 ", %s, %" PRId64 ", %" PRId64 ", %" PRId64 ", %" PRId64 ", %" PRId64,
            mdate(),
            (!description.empty() ? description.c_str() : "?"),
            commandsqueue->getPCR(), commandsqueue->getFirstDTS(),
            commandsqueue->getBufferingLevel(), nz_deadline, commandsqueue->getDemuxedAmount());
    //msg_Info(p_realdemux, "TFE DEBUG stream is %s, %" PRId64, description.c_str(), mdate());
    //std::cerr << "TFE stream, " << mdate() << ", " << description.c_str() << std::endl;
    //std::cerr << "TFE pcr, " << mdate() << ", " << commandsqueue->getPCR() << std::endl;
    //std::cerr << "TFE dts, " << mdate() << ", " << commandsqueue->getFirstDTS() << std::endl;
    //std::cerr << "TFE buffering level, " << mdate() << ", " << commandsqueue->getBufferingLevel() << std::endl;
    //std::cerr << "TFE nz deadline, " << mdate() << ", " << nz_deadline << std::endl; // ??
    //std::cerr << "TFE demuxed amount, " << mdate() << ", " << commandsqueue->getDemuxedAmount() << std::endl;

    if(nz_deadline + VLC_TS_0 <= commandsqueue->getBufferingLevel()) /* demuxed */
    {
        *pi_pcr = commandsqueue->Process( p_realdemux->out, VLC_TS_0 + nz_deadline );
        return AbstractStream::status_demuxed;
    }

    return AbstractStream::status_buffering;
}

block_t * AbstractStream::readNextBlock()
{
    if (currentChunk == NULL && !eof)
        currentChunk = segmentTracker->getNextChunk(!fakeesout->restarting(), connManager);

    if(discontinuity || needrestart)
    {
        msg_Info(p_realdemux, "Encountered discontinuity");
        /* Force stream/demuxer to end for this call */
        return NULL;
    }

    if(currentChunk == NULL)
    {
        eof = true;
        return NULL;
    }

    const bool b_segment_head_chunk = (currentChunk->getBytesRead() == 0);

    block_t *block = currentChunk->readBlock();
    if(block == NULL)
    {
        delete currentChunk;
        currentChunk = NULL;
        return NULL;
    }

    if (currentChunk->isEmpty())
    {
        delete currentChunk;
        currentChunk = NULL;
    }

    block = checkBlock(block, b_segment_head_chunk);

    return block;
}

bool AbstractStream::setPosition(mtime_t time, bool tryonly)
{
    if(!seekAble())
        return false;

    bool ret = segmentTracker->setPositionByTime(time, demuxer->needsRestartOnSeek(), tryonly);
    if(!tryonly && ret)
    {
        if(demuxer->needsRestartOnSeek())
        {
            if(currentChunk)
                delete currentChunk;
            currentChunk = NULL;
            needrestart = false;

            setTimeOffset(-1);
            setTimeOffset(segmentTracker->getPlaybackTime());

            if( !restartDemux() )
                dead = true;
        }
        else commandsqueue->Abort( true );

        es_out_Control(p_realdemux->out, ES_OUT_SET_NEXT_DISPLAY_TIME,
                       VLC_TS_0 + time);
    }
    return ret;
}

mtime_t AbstractStream::getPlaybackTime() const
{
    return segmentTracker->getPlaybackTime();
}

void AbstractStream::runUpdates()
{
    if(!isDisabled())
        segmentTracker->updateSelected();
}

void AbstractStream::fillExtraFMTInfo( es_format_t *p_fmt ) const
{
    if(!p_fmt->psz_language && !language.empty())
        p_fmt->psz_language = strdup(language.c_str());
    if(!p_fmt->psz_description && !description.empty())
        p_fmt->psz_description = strdup(description.c_str());
}

void AbstractStream::setTimeOffset(mtime_t i_offset)
{
    /* Check if we need to set an offset as the demuxer
     * will start from zero from seek point */
    if(i_offset < 0) /* reset */
    {
        fakeesout->setTimestampOffset(0);
    }
    else if(demuxer)
    {
        if(demuxer->alwaysStartsFromZero())
            fakeesout->setTimestampOffset(i_offset);
    }
}

void AbstractStream::trackerEvent(const SegmentTrackerEvent &event)
{
    switch(event.type)
    {
        case SegmentTrackerEvent::DISCONTINUITY:
            discontinuity = true;
            break;

        case SegmentTrackerEvent::FORMATCHANGE:
            /* Check if our current demux is still valid */
            if(*event.u.format.f != format)
            {
                /* Format has changed between segments, we need to drain and change demux */
                msg_Info(p_realdemux, "Changing stream format %s -> %s",
                         format.str().c_str(), event.u.format.f->str().c_str());
                format = *event.u.format.f;

                /* This is an implict discontinuity */
                discontinuity = true;
            }
            break;

        case SegmentTrackerEvent::SWITCHING:
            if(demuxer && demuxer->needsRestartOnSwitch() && !inrestart)
            {
                needrestart = true;
            }
        default:
            break;
    }
}

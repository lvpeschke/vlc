/*
 * RateBasedAdaptationLogic.cpp
 *****************************************************************************
 * Copyright (C) 2010 - 2011 Klagenfurt University
 *
 * Created on: Aug 10, 2010
 * Authors: Christopher Mueller <christopher.mueller@itec.uni-klu.ac.at>
 *          Christian Timmerer  <christian.timmerer@itec.uni-klu.ac.at>
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

#include "RateBasedAdaptationLogic.h"
#include "Representationselectors.hpp"

#include "../playlist/BaseRepresentation.h"
#include "../playlist/BasePeriod.h"
#include "../http/Chunk.h"
#include "../tools/Debug.hpp"
/* LVP added */
#include <iostream>
#include <ctime>
#include <cinttypes>

using namespace adaptive::logic;
using namespace adaptive;

RateBasedAdaptationLogic::RateBasedAdaptationLogic  (vlc_object_t *p_obj_, int w, int h) :
                          AbstractAdaptationLogic   (),
                          bpsAvg(0),
                          currentBps(0)
{
    width  = w;
    height = h;
    usedBps = 0;
    dllength = 0;
    p_obj = p_obj_;
    dlsize = 0;
    vlc_mutex_init(&lock);
}

RateBasedAdaptationLogic::~RateBasedAdaptationLogic()
{
    vlc_mutex_destroy(&lock);
}

BaseRepresentation *RateBasedAdaptationLogic::getNextRepresentation(BaseAdaptationSet *adaptSet, BaseRepresentation *currep)
{
    if(adaptSet == NULL)
        return NULL;

    vlc_mutex_lock(const_cast<vlc_mutex_t *>(&lock));
    size_t availBps = currentBps + ((currep) ? currep->getBandwidth() : 0);
    vlc_mutex_unlock(const_cast<vlc_mutex_t *>(&lock));
    if(availBps > usedBps) {
        /* LVP added, TFE DEBUG */
        msg_Info(p_obj, "TFE DEBUG availBps > usedBps, %" PRId64 ", %zu, %zu", mdate(), availBps, usedBps);
        availBps -= usedBps;
    }
    else {
        /* LVP added, TFE DEBUG */
        msg_Info(p_obj, "TFE DEBUG availBps will be 0, %" PRId64 ", %zu, %zu", mdate(), availBps, usedBps);
        availBps = 0;
    }


    RepresentationSelector selector;
    BaseRepresentation *rep = selector.select(adaptSet, availBps, width, height);
    if ( rep == NULL )
    {
        /* LVP added, TFE DEBUG */
        msg_Info(p_obj, "TFE DEBUG rep is null, %" PRId64, mdate());

        rep = selector.select(adaptSet);
        if ( rep == NULL ) {

            /* LVP added, TFE DEBUG */
            msg_Info(p_obj, "TFE DEBUG rep is still null, %" PRId64, mdate());

            return NULL;
        }

    }

    /* LVP added, TFE */
    msg_Info(p_obj, "TFE rblogic base representation, %" PRId64 ", %s, %" PRIu64,
            mdate(),
            adaptSet->getID().str().c_str(),
            rep->getBandwidth());

    return rep;
}

void RateBasedAdaptationLogic::updateDownloadRate(const ID &, size_t size, mtime_t time)
{
    if(unlikely(time == 0)){
        /* LVP added, TFE DEBUG */
        msg_Info(p_obj, "TFE DEBUG unlikely(time==0) happened in ... Logic update download rate, %" PRId64,
                mdate());
        return;
    }

    /* Accumulate up to observation window */
    dllength += time;
    dlsize += size;

    if(dllength < CLOCK_FREQ / 4){
        /* LVP added, TFE DEBUG */
        msg_Info(p_obj, "TFE DEBUG dllength < CLOCK_FREQ / 4 happened in ... Logic update download rate, %" PRId64,
                mdate());
        return;
    }

    const size_t bps = CLOCK_FREQ * dlsize * 8 / dllength;

    vlc_mutex_lock(&lock);
    bpsAvg = average.push(bps);

    BwDebug(msg_Dbg(p_obj, "bw estimation bps %zu -> avg %zu",
                            bps / 8000, bpsAvg / 8000));

    currentBps = bpsAvg * 3/4;
    dlsize = dllength = 0;

    BwDebug(msg_Info(p_obj, "Current bandwidth %zu KiB/s using %u%%",
                    (bpsAvg / 8000), (bpsAvg) ? (unsigned)(usedBps * 100.0 / bpsAvg) : 0));

    /* LVP added, TFE */
    // always called by HTTPConnectionManager updateDownloadRate, chunkBuffered updateDownloadRate
    // mdate, observed, avg, current, used
    msg_Info(p_obj, "TFE rblogic update download rate, %" PRId64 ", %zu, %zu, %zu, %zu",
            mdate(), bps, bpsAvg, currentBps, usedBps);

    vlc_mutex_unlock(&lock);
}

void RateBasedAdaptationLogic::trackerEvent(const SegmentTrackerEvent &event)
{
    if(event.type == SegmentTrackerEvent::SWITCHING)
    {
        vlc_mutex_lock(&lock);
        if(event.u.switching.prev)
            usedBps -= event.u.switching.prev->getBandwidth();
        if(event.u.switching.next)
            usedBps += event.u.switching.next->getBandwidth();

        BwDebug(msg_Info(p_obj, "New bandwidth usage %zu KiB/s %u%%",
                        (usedBps / 8000), (bpsAvg) ? (unsigned)(usedBps * 100.0 / bpsAvg) : 0 ));
        vlc_mutex_unlock(&lock);

        /* LVP added, TFE */
        // TODO here
        const std::string id;
        if(event.u.switching.next) {
            id = event.u.switching.next->getAdaptationSet()->getID().str();
        } else if(event.u.switching.prev) {
            id = event.u.switching.prev->getAdaptationSet()->getID().str();
        msg_Info(p_obj, "TFE OLD rblogic new bps, %" PRId64 ", %zu",
                mdate(), usedBps);
        msg_Info(p_obj, "TFE rblogic new bps, %" PRId64 ", %s, %zu",
                mdate(), (id.empty()) ? "\0" : id.c_str(), usedBps);
    }
	/* LVP added, TFE */
	else if(event.type == SegmentTrackerEvent::BUFFERING_STATE)
	{
        const ID &id = *event.u.buffering.id;
    	msg_Info(p_obj, "TFE rblogic BUFFERING_STATE bool, %" PRId64 ", %s, %d",
                mdate(), id.str().c_str(), event.u.buffering.enabled);
	}
	else if(event.type == SegmentTrackerEvent::BUFFERING_LEVEL_CHANGE)
	{
        const ID &id = *event.u.buffering.id;
        msg_Info(p_obj, "TFE rblogic BUFFERING_LEVEL_CHANGE, %" PRId64 ", %s, %" PRId64 ", %" PRId64,
                mdate(),
                id.str().c_str(),
                event.u.buffering_level.current, event.u.buffering_level.target);
	}
	
}

FixedRateAdaptationLogic::FixedRateAdaptationLogic(size_t bps) :
    AbstractAdaptationLogic()
{
    currentBps = bps;
}

BaseRepresentation *FixedRateAdaptationLogic::getNextRepresentation(BaseAdaptationSet *adaptSet, BaseRepresentation *)
{
    if(adaptSet == NULL)
        return NULL;

    RepresentationSelector selector;
    BaseRepresentation *rep = selector.select(adaptSet, currentBps);
    if ( rep == NULL )
    {
        rep = selector.select(adaptSet);
        if ( rep == NULL )
            return NULL;
    }
    return rep;
}

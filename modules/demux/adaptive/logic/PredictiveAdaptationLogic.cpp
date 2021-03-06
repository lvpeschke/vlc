/*
 * PredictiveAdaptationLogic.cpp
 *****************************************************************************
 * Copyright (C) 2016 - VideoLAN Authors
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

#include "PredictiveAdaptationLogic.hpp"

#include "Representationselectors.hpp"

#include "../playlist/BaseAdaptationSet.h"
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

PredictiveStats::PredictiveStats()
{
    segments_count = 0;
    buffering_level = 0;
    buffering_target = 1;
    last_download_rate = 0;
    last_duration = 1;
}

bool PredictiveStats::starting() const
{
    return (segments_count < 3) || !last_download_rate;
}

PredictiveAdaptationLogic::PredictiveAdaptationLogic(vlc_object_t *p_obj_)
    : AbstractAdaptationLogic()
{
    p_obj = p_obj_;
    usedBps = 0;
    vlc_mutex_init(&lock);
}

PredictiveAdaptationLogic::~PredictiveAdaptationLogic()
{
    vlc_mutex_destroy(&lock);
}

BaseRepresentation *PredictiveAdaptationLogic::getNextRepresentation(BaseAdaptationSet *adaptSet, BaseRepresentation *prevRep)
{
    RepresentationSelector selector;
    BaseRepresentation *rep;

    vlc_mutex_lock(&lock);

    std::map<ID, PredictiveStats>::iterator it = streams.find(adaptSet->getID());
    /* LVP added, TFE */
    msg_Info(p_obj, "TFE predictive number of streams, %" PRId64 ", %zu", mdate(), streams.size());
    if(it == streams.end())
    {
        rep = selector.highest(adaptSet);

        /* LVP added, TFE */
        msg_Info(p_obj, "TFE predictive streams end rep highest, %" PRId64 ", %s, %s",
                mdate(),
                adaptSet->getID().str().c_str(),
                (rep) ? rep->getMimeType().c_str() : ((prevRep) ? prevRep->getMimeType().c_str() : adaptSet->getMimeType().c_str()));
    }
    else
    {
        PredictiveStats &stats = (*it).second;

        double f_buffering_level = (double)stats.buffering_level / stats.buffering_target;
        double f_min_buffering_level = f_buffering_level;
        unsigned i_max_bitrate = 0;
        if(streams.size() > 1)
        {
            std::map<ID, PredictiveStats>::const_iterator it2 = streams.begin();
            for(; it2 != streams.end(); ++it2)
            {
                if(it2 == it)
                    continue;

                const PredictiveStats &other = (*it2).second;
                f_min_buffering_level = std::min((double)other.buffering_level / other.buffering_target,
                                                 f_min_buffering_level);
                i_max_bitrate = std::max(i_max_bitrate, other.last_download_rate);
            }
        }
        /* LVP added, TFE */
	    msg_Info(p_obj, "TFE predictive stats, %" PRId64 ", %s, %s, %" PRId64 ", %" PRId64 ", %f, %f, %u",
                mdate(),
                adaptSet->getID().str().c_str(),
                (prevRep) ? prevRep->getMimeType().c_str() : adaptSet->getMimeType().c_str(),
                stats.buffering_level, stats.buffering_target, f_buffering_level,
                f_min_buffering_level, i_max_bitrate);

        if(stats.starting())
        {
            rep = selector.highest(adaptSet);

            /* LVP added, TFE */
            msg_Info(p_obj, "TFE predictive stats starting rep highest, %" PRId64 ", %s, %s",
                    mdate(),
                    adaptSet->getID().str().c_str(),
                    (rep) ? rep->getMimeType().c_str() : ((prevRep) ? prevRep->getMimeType().c_str() : adaptSet->getMimeType().c_str()));
        }
        else
        {
            const unsigned i_available_bw = getAvailableBw(i_max_bitrate, prevRep);
            /* LVP added, TFE */
            msg_Info(p_obj, "TFE predictive availableBw, %" PRId64 ", %u", mdate(), i_available_bw);
            
            if(!prevRep)
            {
                rep = selector.select(adaptSet, i_available_bw);
            }
            else if(f_buffering_level > 0.8)
            {
                rep = selector.select(adaptSet, std::max((uint64_t) i_available_bw,
                                                         (uint64_t) prevRep->getBandwidth()));
            }
            else if(f_buffering_level > 0.5)
            {
                rep = prevRep;
            }
            else
            {
                if(f_buffering_level > 2 * stats.last_duration)
                {
                    rep = selector.lower(adaptSet, prevRep);
                }
                else
                {
                    rep = selector.select(adaptSet, i_available_bw * f_buffering_level);
                }
            }
        }

        BwDebug( for(it=streams.begin(); it != streams.end(); ++it)
        {
            const PredictiveStats &s = (*it).second;
            msg_Info(p_obj, "Stream %s buffering level %.2f%",
                 (*it).first.str().c_str(), (double) s.buffering_level / s.buffering_target);
        } );

        BwDebug( if( rep != prevRep )
                    msg_Info(p_obj, "Stream %s new bandwidth usage %zu KiB/s",
                         adaptSet->getID().str().c_str(), rep->getBandwidth() / 8000); );

        /* LVP added, TFE */
        msg_Info(p_obj, "TFE predictive bandwidth usage bps, %" PRId64 ", %s, %s, %" PRIu64,
                mdate(),
                adaptSet->getID().str().c_str(),
                (rep) ? rep->getMimeType().c_str() : ((prevRep) ? prevRep->getMimeType().c_str() : adaptSet->getMimeType().c_str()),
                rep->getBandwidth());

        stats.segments_count++;
    }

    vlc_mutex_unlock(&lock);

    return rep;
}

void PredictiveAdaptationLogic::updateDownloadRate(const ID &id, size_t dlsize, mtime_t time)
{
    vlc_mutex_lock(&lock);
    std::map<ID, PredictiveStats>::iterator it = streams.find(id);
    if(it != streams.end())
    {
        /* LVP added, TFE */
        msg_Info(p_obj, "TFE predictive update last download rate input, %" PRId64 ", %s, %" PRId64 ", %zu",
                mdate(), id.str().c_str(), time, dlsize);

        PredictiveStats &stats = (*it).second;
        stats.last_download_rate = stats.average.push(CLOCK_FREQ * dlsize * 8 / time);

        /* LVP added, TFE & TFE DEBUG */
        msg_Info(p_obj, "TFE predictive update last download rate, %" PRId64 ", %s, %lu",
                mdate(), id.str().c_str(), (CLOCK_FREQ * dlsize * 8 / time));
    }

    vlc_mutex_unlock(&lock);
}

unsigned PredictiveAdaptationLogic::getAvailableBw(unsigned i_bw, const BaseRepresentation *curRep) const
{
    unsigned i_remain = i_bw;
    i_remain -= usedBps;
    if(curRep)
        i_remain += curRep->getBandwidth();
    return i_remain;
}

void PredictiveAdaptationLogic::trackerEvent(const SegmentTrackerEvent &event)
{
    switch(event.type)
    {
    case SegmentTrackerEvent::SWITCHING:
        {
            vlc_mutex_lock(&lock);
            if(event.u.switching.prev)
                usedBps -= event.u.switching.prev->getBandwidth();
            if(event.u.switching.next)
                usedBps += event.u.switching.next->getBandwidth();
				
            BwDebug(msg_Info(p_obj, "New total bandwidth usage %zu KiB/s", (usedBps / 8000)));
            vlc_mutex_unlock(&lock);

            /* LVP added, TFE */
            if(event.u.switching.next) {
                msg_Info(p_obj, "TFE predictive new bps, %" PRId64 ", %s, %s, %u",
                        mdate(),
                        event.u.switching.next->getAdaptationSet()->getID().str().c_str(),
                        event.u.switching.next->getMimeType().c_str(),
                        usedBps);
            } else if(event.u.switching.prev) {
                msg_Info(p_obj, "TFE predictive new bps, %" PRId64 ", %s, %s, %u",
                        mdate(),
                        event.u.switching.prev->getAdaptationSet()->getID().str().c_str(),
                        event.u.switching.prev->getMimeType().c_str(),
                        usedBps);
            } else {
                msg_Info(p_obj, "TFE predictive new bps, %" PRId64 ", , , %u",
                        mdate(), usedBps);
            }
        }
        break;

    case SegmentTrackerEvent::BUFFERING_STATE:
        {
            const ID &id = *event.u.buffering.id;
            vlc_mutex_lock(&lock);
            if(event.u.buffering.enabled)
            {
                if(streams.find(id) == streams.end())
                {
                    PredictiveStats stats;
                    streams.insert(std::pair<ID, PredictiveStats>(id, stats));
                }
            }
            else
            {
                std::map<ID, PredictiveStats>::iterator it = streams.find(id);
                if(it != streams.end())
                    streams.erase(it);
            }
            vlc_mutex_unlock(&lock);
            //BwDebug(msg_Info(p_obj, "Stream %s is now known %sactive",
            //                 (event.u.buffering.enabled) ? "" : "in"));
            /* LVP added, TFE */
            msg_Info(p_obj, "TFE predictive SegmentTrackerEvent BUFFERING_STATE bool, %" PRId64 ", %s, %d",
                    mdate(), id.str().c_str(), event.u.buffering.enabled);
        }
        break;

    case SegmentTrackerEvent::BUFFERING_LEVEL_CHANGE:
        {
            const ID &id = *event.u.buffering.id;
            vlc_mutex_lock(&lock);
            PredictiveStats &stats = streams[id];
            stats.buffering_level = event.u.buffering_level.current;
            stats.buffering_target = event.u.buffering_level.target;
            vlc_mutex_unlock(&lock);
            /* LVP added, TFE */
            msg_Info(p_obj, "TFE predictive SegmentTrackerEvent BUFFERING_LEVEL_CHANGE, %" PRId64 ", %s, %" PRId64 ", %" PRId64,
                    mdate(), id.str().c_str(), event.u.buffering_level.current, event.u.buffering_level.target);
        }
        break;

    case SegmentTrackerEvent::SEGMENT_CHANGE:
        {
            const ID &id = *event.u.segment.id;
            vlc_mutex_lock(&lock);
            PredictiveStats &stats = streams[id];
            stats.last_duration = event.u.segment.duration;
            vlc_mutex_unlock(&lock);
        }
        break;

    default:
            break;
    }
}

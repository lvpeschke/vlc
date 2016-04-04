/*
 * Representationselectors.cpp
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

#include "Representationselectors.hpp"
#include "../playlist/BaseRepresentation.h"
#include "../playlist/BaseAdaptationSet.h"
#include "../playlist/BasePeriod.h"
#include <limits>
/* LVP added */
#include <iostream>
#include <ctime>

using namespace adaptive::logic;

RepresentationSelector::RepresentationSelector()
{
}

/* LVP case 1 */
BaseRepresentation * RepresentationSelector::select(BaseAdaptationSet *adaptSet) const
{
    return select(adaptSet, std::numeric_limits<uint64_t>::max()); /* LVP, case 2 */
}

/* LVP case 2 */
BaseRepresentation * RepresentationSelector::select(BaseAdaptationSet *adaptSet, uint64_t bitrate) const
{
    if (adaptSet == NULL)
        return NULL;

    BaseRepresentation *best = NULL;
    std::vector<BaseRepresentation *> reps = adaptSet->getRepresentations();
    BaseRepresentation *candidate = select(reps, (best)?best->getBandwidth():0, bitrate); /* LVP, case 3 */
    if (candidate)
    {
        if (candidate->getBandwidth() > bitrate) /* none matched, returned lowest */ {
            /* LVP added, TFE */
            std::cerr << "TFE base representation lowest possible, " << std::time(nullptr) << std::endl;
            return candidate;
        }

        best = candidate;
    }

    return best;
}

/* LVP case 4 */
BaseRepresentation * RepresentationSelector::select(BaseAdaptationSet *adaptSet, uint64_t bitrate,
                                                int width, int height) const
{
    if(adaptSet == NULL)
        return NULL;

    std::vector<BaseRepresentation *> resMatchReps;

    /* subset matching WxH */
    std::vector<BaseRepresentation *> reps = adaptSet->getRepresentations();
    std::vector<BaseRepresentation *>::const_iterator repIt;
    for(repIt=reps.begin(); repIt!=reps.end(); ++repIt)
    {
        if((*repIt)->getWidth() == width && (*repIt)->getHeight() == height)
            resMatchReps.push_back(*repIt);
    }

    if(resMatchReps.empty())
        return select(adaptSet, bitrate); /* LVP, case 2 */
    else
        return select(resMatchReps, 0, bitrate); /* LVP, case 3 */
}

/* LVP case 3 */
BaseRepresentation * RepresentationSelector::select(std::vector<BaseRepresentation *>& reps,
                                                uint64_t minbitrate, uint64_t maxbitrate) const
{
    BaseRepresentation  *candidate = NULL, *lowest = NULL;
    std::vector<BaseRepresentation *>::const_iterator repIt;
    for(repIt=reps.begin(); repIt!=reps.end(); ++repIt)
    {
        if ( !lowest || (*repIt)->getBandwidth() < lowest->getBandwidth())
            lowest = *repIt;

        if ( (*repIt)->getBandwidth() < maxbitrate &&
             (*repIt)->getBandwidth() > minbitrate )
        {
            candidate = (*repIt);
            minbitrate = (*repIt)->getBandwidth();
        }
    }

    /* LVP added, TFE */
    if (candidate) std::cerr << "TFE base representation bw " << std::time(nullptr) << ", " << candidate->getBandwidth() << std::endl;
    else std::cerr << "TFE base representation lowest bw " << std::time(nullptr) << ", " << lowest->getBandwidth() << std::endl;

    if (!candidate)
        return candidate = lowest;

    return candidate;
}

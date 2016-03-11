/*
 * HLSStreams.hpp
 *****************************************************************************
 * Copyright (C) 2015 - VideoLAN and VLC authors
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
#ifndef HLSSTREAM_HPP
#define HLSSTREAM_HPP

#include "../adaptive/Streams.hpp"

namespace hls
{
    using namespace adaptive;

    class HLSStream : public AbstractStream
    {
        public:
            HLSStream(demux_t *, const StreamFormat &);
            virtual bool setPosition(mtime_t, bool); /* reimpl */

        protected:
            virtual AbstractDemuxer * createDemux(const StreamFormat &); /* reimpl */
            virtual bool restartDemux(); /* reimpl */
            virtual void prepareFormatChange(); /* reimpl */

            virtual block_t *checkBlock(block_t *, bool); /* reimpl */

        private:
            bool b_timestamps_offset_set;
            mtime_t i_aac_offset;
    };

    class HLSStreamFactory : public AbstractStreamFactory
    {
        public:
            virtual AbstractStream *create(demux_t*, const StreamFormat &,
                                   SegmentTracker *, HTTPConnectionManager *) const;
    };

}
#endif // HLSSTREAMS_HPP

/*
 * HTTPConnection.cpp
 *****************************************************************************
 * Copyright (C) 2014-2015 - VideoLAN and VLC Authors
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

#include "HTTPConnection.hpp"
#include "ConnectionParams.hpp"
#include "Sockets.hpp"
#include "../adaptive/tools/Helper.h"

#include <sstream>
#include <vlc_stream.h>
/* LVP added */
#include <iostream>
#include <ctime>
#include <cstdint>
#include <cinttypes>

using namespace adaptive::http;

AbstractConnection::AbstractConnection(vlc_object_t *p_object_)
{
    p_object = p_object_;
    available = true;
    bytesRead = 0;
    contentLength = 0;
}

AbstractConnection::~AbstractConnection()
{

}

bool AbstractConnection::prepare(const ConnectionParams &params_)
{
    if (!available)
        return false;
    params = params_;
    available = false;
    return true;
}

size_t AbstractConnection::getContentLength() const
{
    return contentLength;
}

HTTPConnection::HTTPConnection(vlc_object_t *p_object_, Socket *socket_, bool persistent)
    : AbstractConnection( p_object_ )
{
    socket = socket_;
    psz_useragent = var_InheritString(p_object_, "http-user-agent");
    queryOk = false;
    retries = 0;
    connectionClose = !persistent;
}

HTTPConnection::~HTTPConnection()
{
    free(psz_useragent);
    delete socket;
}

bool HTTPConnection::canReuse(const ConnectionParams &params_) const
{
    /* LVP added */
    msg_Dbg(p_object, "LVP entered HTTPConnection::canReuse, params next");
    //std::cerr << "LVP" <<
    //        " available " << available <<
    //        ", hostname " << params.getHostname() << ", _hostname " << params_.getHostname() <<
    //        ", scheme " << params.getScheme() << ", _scheme " << params_.getScheme() <<
    //        ", port " << params.getPort() << ", _port " << params_.getPort() << std::endl;

    return ( available &&
             params.getHostname() == params_.getHostname() &&
             params.getScheme() == params_.getScheme() &&
             params.getPort() == params_.getPort() );
}

bool HTTPConnection::connect()
{
    /* LVP added */
    msg_Dbg(p_object, "LVP HTTPConnection::connect!!");

    return socket->connect(p_object, params.getHostname().c_str(),
                                     params.getPort());
}

bool HTTPConnection::connected() const
{
    /* LVP added */
    msg_Dbg(p_object, "LVP HTTPConnection::connected (bool) is %d", socket->connected());

    return socket->connected();
}

void HTTPConnection::disconnect()
{
    queryOk = false;
    bytesRead = 0;
    contentLength = 0;
    bytesRange = BytesRange();
    socket->disconnect();

    /* LVP added */
    msg_Dbg(p_object, "LVP HTTPConnection::disconnected!!");
}

int HTTPConnection::request(const std::string &path, const BytesRange &range)
{
    /* LVP added */
    msg_Dbg(p_object, "LVP entered HTTPConnection::request");

    queryOk = false;

    /* Set new path for this query */
    params.setPath(path);

    msg_Dbg(p_object, "Retrieving %s @%zu", params.getUrl().c_str(),
                       range.isValid() ? range.getStartByte() : 0);

    if(!connected() && ( params.getHostname().empty() || !connect() ))
        return VLC_EGENERIC;

    bytesRange = range;
    if(range.isValid() && range.getEndByte() > 0)
        contentLength = range.getEndByte() - range.getStartByte() + 1;

    std::string header = buildRequestHeader(path);
    if(connectionClose) {
        /* LVP added, TFE DEBUG */
        msg_Info(p_object, "TFE DEBUG HTTPConnection::request connectionClose true, %" PRId64, mdate());
	//std::cerr << "TFE DEBUG HTTPConnection::request connectionClose true, " << mdate() << std::endl;

        header.append("Connection: close\r\n");
    }
    header.append("\r\n");

    if(!send( header ))
    {
        /* LVP added */
        msg_Dbg(p_object, "LVP HTTPConnection::request disconnect, header not sent ?");

        socket->disconnect();
        if(!connectionClose)
        {
            /* server closed connection pipeline after last req. need new */
            connectionClose = true;
            return request(path, range);
        }
        return VLC_EGENERIC;
    }

    /* LVP added */
    msg_Dbg(p_object, "LVP HTTPConnection::request send returned true");

    int i_ret = parseReply();
    if(i_ret == VLC_SUCCESS)
    {
        queryOk = true;
    }
    else if(i_ret == VLC_EGENERIC)
    {

        /* LVP added */
        msg_Dbg(p_object, "LVP HTTPConnection::request disconnect, query not okay ?");

        socket->disconnect();
        if(!connectionClose)
        {
            connectionClose = true;
            return request(path, range);
        }
    }

    return i_ret;
}

ssize_t HTTPConnection::read(void *p_buffer, size_t len)
{
    /* LVP added */
    msg_Dbg(p_object, "LVP enter HTTPConnection::read");

    if( !connected() ||
       (!queryOk && bytesRead == 0) )
        return VLC_EGENERIC;

    if(len == 0)
        return VLC_SUCCESS;

    queryOk = false;

    const size_t toRead = (contentLength) ? contentLength - bytesRead : len;
    if (toRead == 0)
        return VLC_SUCCESS;

    if(len > toRead)
        len = toRead;

    ssize_t ret = socket->read(p_object, p_buffer, len);
    if(ret >= 0)
        bytesRead += ret;


    /* LVP reverse commit 874a409499639af8068458e4d8f22ff3202ff074 */
    if(ret < 0 || (size_t)ret < len) /* set EOF */
    //if(ret < 0 || (size_t)ret < len || /* set EOF */
    //   contentLength == bytesRead )
    {
        /* LVP added */
        msg_Dbg(p_object, "LVP HTTPConnection::read disconnect, EOF ?");
        if (ret < 0)
            msg_Dbg(p_object, "LVP HTTPConnection::read disconnect, ret < 0 ");
        if ((size_t)ret < len)
            msg_Dbg(p_object, "LVP HTTPConnection::read disconnect, ret < len ");
        if (contentLength == bytesRead)
            msg_Dbg(p_object, "LVP HTTPConnection::read disconnect, contentLength == bytesRead ");

        socket->disconnect();
        return ret;
    }

    /* LVP added, TFE */
    if (contentLength == bytesRead)
        msg_Info(p_object, "TFE read HTTP response done, %" PRId64 ", %zu", mdate(), contentLength);
        //std::cerr << "TFE read HTTP response done, " << mdate() << ", " << contentLength << std::endl;
    /*
    else
        std::cerr << "TFE read HTTP response in progress, " << mdate() << std::endl;
	std::cerr << "TFE read, " << mdate() << ", " << ret << std::endl;
	std::cerr << "TFE bytesRead, " << mdate() << ", " << bytesRead << std::endl;
    */

    return ret;
}

bool HTTPConnection::send(const std::string &data)
{
    /* LVP added */
	// useless: redirects to socket
	
    return send(data.c_str(), data.length());
}

bool HTTPConnection::send(const void *buf, size_t size)
{
    /* LVP added, TFE */
    msg_Info(p_object, "TFE send HTTP on socket, %" PRId64, mdate());
    //std::cerr << "TFE send HTTP on socket, " << mdate() << std::endl;

    return socket->send(p_object, buf, size);
}

int HTTPConnection::parseReply()
{
    /* LVP added */
    msg_Dbg(p_object, "LVP entered HTTPConnection::parseReply");

    std::string line = readLine();

    if(line.empty())
        return VLC_EGENERIC;

    if (line.compare(0, 9, "HTTP/1.1 ")!=0)
    {
        if(line.compare(0, 9, "HTTP/1.0 ")!=0)
            return VLC_ENOOBJ;
        else
            connectionClose = true;
    }

    std::istringstream ss(line.substr(9));
    ss.imbue(std::locale("C"));
    int replycode;
    ss >> replycode;

    /* LVP added, TFE */
    msg_Info(p_object, "TFE HTTP replycode, %" PRId64 ", %d", mdate(), replycode);
    //std::cerr << "TFE HTTP replycode, " << mdate() << ", " << replycode << std::endl;

    if (replycode != 200 && replycode != 206)
        return VLC_ENOOBJ;

    line = readLine();

    while(!line.empty() && line.compare("\r\n"))
    {
        size_t split = line.find_first_of(':');
        size_t value = split + 1;

        while(line.at(value) == ' ')
            value++;

        onHeader(line.substr(0, split), line.substr(value));
        line = readLine();
    }

    return VLC_SUCCESS;
}

std::string HTTPConnection::readLine()
{
    return socket->readline(p_object);
}

void HTTPConnection::setUsed( bool b )
{
    /* LVP added */
    msg_Dbg(p_object, "LVP HTTPConnection::setUsed (available set to %d)", !b);

    available = !b;
    if(available)
    {

        /* LVP added */
        msg_Dbg(p_object, "LVP HTTPConnection::setUsed (is now available)");

        if(!connectionClose && contentLength == bytesRead )
        {
            /* LVP added */
            msg_Dbg(p_object, "LVP HTTPConnection::setUsed (entered this part)");

            queryOk = false;
            bytesRead = 0;
            contentLength = 0;
            bytesRange = BytesRange();
        }
        else  /* We can't resend request if we haven't finished reading */
        {
            /* LVP added */
            msg_Dbg(p_object, "LVP HTTPConnection::setUsed seems to have failed, disconnect");

            disconnect();
        }

    }
}

void HTTPConnection::onHeader(const std::string &key,
                              const std::string &value)
{
    if(key == "Content-Length")
    {
        std::istringstream ss(value);
        ss.imbue(std::locale("C"));
        size_t length;
        ss >> length;
        contentLength = length;
    }
    else if (key == "Connection" && value =="close")
    {
        connectionClose = true;
    }
}

std::string HTTPConnection::buildRequestHeader(const std::string &path) const
{
    std::stringstream req;
    req << "GET " << path << " HTTP/1.1\r\n" <<
           "Host: " << params.getHostname() << "\r\n" <<
           "Cache-Control: no-cache" << "\r\n" <<
           "Accept-Encoding: identity" << "\r\n" <<
           "User-Agent: " << std::string(psz_useragent) << "\r\n";
    req << extraRequestHeaders();
    return req.str();
}

std::string HTTPConnection::extraRequestHeaders() const
{
    std::stringstream ss;
    ss.imbue(std::locale("C"));
    if(bytesRange.isValid())
    {
        ss << "Range: bytes=" << bytesRange.getStartByte() << "-";
        if(bytesRange.getEndByte())
            ss << bytesRange.getEndByte();
        ss << "\r\n";
    }
    return ss.str();
}

StreamUrlConnection::StreamUrlConnection(vlc_object_t *p_object)
    : AbstractConnection(p_object)
{
    p_streamurl = NULL;
    bytesRead = 0;
    contentLength = 0;
}

StreamUrlConnection::~StreamUrlConnection()
{
    reset();
}

void StreamUrlConnection::reset()
{
    if(p_streamurl)
        vlc_stream_Delete(p_streamurl);
    p_streamurl = NULL;
    bytesRead = 0;
    contentLength = 0;
    bytesRange = BytesRange();
}

bool StreamUrlConnection::canReuse(const ConnectionParams &) const
{
    return available;
}

int StreamUrlConnection::request(const std::string &path, const BytesRange &range)
{
    reset();

    /* Set new path for this query */
    params.setPath(path);

    msg_Dbg(p_object, "Retrieving %s @%zu", params.getUrl().c_str(),
                      range.isValid() ? range.getStartByte() : 0);

    p_streamurl = vlc_stream_NewMRL(p_object, params.getUrl().c_str());
    if(!p_streamurl)
        return VLC_EGENERIC;

    if(range.isValid() && range.getEndByte() > 0)
    {
        if(vlc_stream_Seek(p_streamurl, range.getStartByte()) != VLC_SUCCESS)
        {
            vlc_stream_Delete(p_streamurl);
            return VLC_EGENERIC;
        }
        bytesRange = range;
        contentLength = range.getEndByte() - range.getStartByte() + 1;
    }

    int64_t i_size = stream_Size(p_streamurl);
    if(i_size > -1)
    {
        if(!range.isValid() || contentLength > (size_t) i_size)
            contentLength = (size_t) i_size;
    }
    return VLC_SUCCESS;
}

ssize_t StreamUrlConnection::read(void *p_buffer, size_t len)
{
    /* LVP added */
    msg_Dbg(p_object, "LVP enter StreamUrlConnection::read");

    if( !p_streamurl )
        return VLC_EGENERIC;

    if(len == 0)
        return VLC_SUCCESS;

    const size_t toRead = (contentLength) ? contentLength - bytesRead : len;
    if (toRead == 0)
        return VLC_SUCCESS;

    if(len > toRead)
        len = toRead;

    ssize_t ret = vlc_stream_Read(p_streamurl, p_buffer, len);
    if(ret >= 0)
        bytesRead += ret;

    if(ret < 0 || (size_t)ret < len || /* set EOF */
       contentLength == bytesRead )
    {
        reset();
        return ret;
    }

    return ret;
}

void StreamUrlConnection::setUsed( bool b )
{
    available = !b;
    if(available && contentLength == bytesRead)
       reset();
}

ConnectionFactory::ConnectionFactory()
{
}

ConnectionFactory::~ConnectionFactory()
{
}

AbstractConnection * ConnectionFactory::createConnection(vlc_object_t *p_object,
                                                         const ConnectionParams &params)
{
    /* LVP added, TFE DEBUG */
    msg_Info(p_object, "TFE DEBUG ConnectionFactory::createConnection entered, %" PRId64, mdate());
    //std::cerr << "TFE DEBUG ConnectionFactory::createConnection entered" << std::endl;

    if((params.getScheme() != "http" && params.getScheme() != "https") || params.getHostname().empty())
        return NULL;

    const int sockettype = (params.getScheme() == "https") ? TLSSocket::TLS : Socket::REGULAR;
    Socket *socket = (sockettype == TLSSocket::TLS) ? new (std::nothrow) TLSSocket()
                                                    : new (std::nothrow) Socket();
    if(!socket)
        return NULL;

    /* disable pipelined tls until we have ticket/resume session support */
    HTTPConnection *conn = new (std::nothrow)
            HTTPConnection(p_object, socket, sockettype != TLSSocket::TLS);

    if(!conn)
    {
	    /* LVP added */
	    // dead code
        delete socket;
        return NULL;
    }

    /* LVP added, TFE DEBUG */
    msg_Info(p_object, "TFE DEBUG ConnectionFactory::createConnection conn persistent set to %s, %" PRId64,
            (sockettype != TLSSocket::TLS) ? "true" : "false", mdate());
    //std::cerr << "TFE DEBUG ConnectionFactory::createConnection conn is " << conn
    //	      << " and persistent is set to " << (sockettype != TLSSocket::TLS) << std::endl;

    return conn;
}

AbstractConnection * StreamUrlConnectionFactory::createConnection(vlc_object_t *p_object,
                                                                  const ConnectionParams &)
{
    return new (std::nothrow) StreamUrlConnection(p_object);
}

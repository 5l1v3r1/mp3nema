/******************************************************************************
 * stream.c 
 *
 * mp3nema - MP3 analysis and data hiding utility
 *
 * Copyright (C) 2008 Matt Davis (enferex) of 757Labs (www.757labs.com)
 *
 * stream.c is part of mp3nema.
 * mp3nema is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * mp3nema is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with mp3nema.  If not, see <http://www.gnu.org/licenses/>.
 *****************************************************************************/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <netdb.h>
#include <unistd.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include "main.h"
#include "utils.h"


static int connect_host(const char *host, int port)
{
    int                 sd;
    struct hostent     *hostname;
    struct sockaddr_in  addr;

    if ((sd = socket(PF_INET, SOCK_STREAM, 0)) == -1)
      return 0;

    if (!(hostname = gethostbyname(host)))
      return 0;

    memset(&addr, 0, sizeof(struct sockaddr_in));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    memcpy(&addr.sin_addr.s_addr, hostname->h_addr, hostname->h_length);

    if (connect(sd, (const struct sockaddr *)&addr,
                sizeof(struct sockaddr_in)) == -1)
      return 0;

    return sd;
}


static void suck_data_from_stream(
    int         sockfd,
    FILE       *savefp,
    flags_t     flags,
    const char *host)
{
    static const int  brain_sz = DEFAULT_BLK_SZ * 4;

    int            ignore_oob, index;
    int            recv_sz, curr_brain_sz, frame_length;
    char           data[DEFAULT_BLK_SZ], brain[brain_sz];
    FILE          *oob_file;
    STREAM_OBJECT  type;
    mp3_frame_t    frame;
    id3_tag_t      id3_tag;
    
    /* If we want to store oob data */ 
    oob_file = NULL;
    if (flags & FLAG_EXTRACT_MODE)
      oob_file = util_create_file(host, "extracted-oob", "dat");

    /* Grab data from stream (don't analyize first chunk) */
    ignore_oob = 1;
    index = 0;
    curr_brain_sz = 0;
    while ((recv_sz = read(sockfd, data, sizeof(data))) > 0)
    {
        /* Once buffer is full analyize all frames */
        if (curr_brain_sz + recv_sz >= brain_sz)
        {
            while ( 1 )
            {
                type = next_mp3_frame_or_id3v2(NULL, brain, curr_brain_sz,
                                               ignore_oob, &index, oob_file);

                /* At the end if UNKNOWN or our length does not match 
                 * amount in buffer * continue gathering 
                 */
                if (type == STREAM_OBJECT_UNKNOWN)
                {
                    curr_brain_sz = 0;
                    ignore_oob = 1;
                    break;
                }
                else if (type == STREAM_OBJECT_MP3_FRAME)
                {
                     mp3_set_header(&frame, brain + index);
                     frame_length = frame.audio_size + frame.header_size;
#ifdef DEBUG
                     printf("frame: %d\n", frame_length);
#endif

                   /* Gather more data */
                   if (frame_length >= curr_brain_sz)
                       break;

                   /* Remove the frame and continue analyizing */
                   else if (frame_length != 0)
                   {
                       frame_length += index;
                       memmove(brain, brain + frame_length,
                               brain_sz - frame_length);
                       curr_brain_sz -= frame_length;
                    }
                    else
                    {
                        curr_brain_sz = 0;
                        ignore_oob = 1;
                        break;
                    }
                }
                else if (type == STREAM_OBJECT_ID3V2_TAG)
                {
                    id3_set_header(&id3_tag, brain + index);
                    id3_tag.size += 10 + ((id3_tag.footer) ? 10 : 0);
                  
                    /* Gather more data */
                    if (id3_tag.size >= curr_brain_sz)
                      break;

                    /* Remove the tag and continue analyizing */
                    else if (id3_tag.size != 0)
                    {
#ifdef DEBUG
                     printf("tag: %d\n", id3_tag.size);
#endif
                       id3_tag.size += index;
                       memmove(brain, brain + id3_tag.size,
                               brain_sz - id3_tag.size);
                       curr_brain_sz -= id3_tag.size;
                    }
                    else
                    {
                        curr_brain_sz = 0;
                        ignore_oob = 1;
                        break;
                    }
                }
                ignore_oob = 0;
            }
        }

        /* Add data to buffer */
        if (curr_brain_sz + recv_sz > brain_sz)
        {
            curr_brain_sz = 0;
            continue;
        }

        memcpy(brain + curr_brain_sz, data, recv_sz);
        curr_brain_sz += recv_sz;

        if (flags & FLAG_CAPTURE_MODE)
          fwrite(data, recv_sz, 1, savefp);
    }
}


static int get_stream_info(
    flags_t     flags,
    int         sd, 
    const char *host,
    const char *port,
    const char *file)
{
    static const int  buf_blk_sz = DEFAULT_BLK_SZ;

    int         total_sz, recv_sz, n_buf_blks;
    char       *buf, *c, query[DEFAULT_BLK_SZ];
    char        data[DEFAULT_BLK_SZ];
    FILE       *fp;
    hostdata_t  newhost;

    snprintf(query, sizeof(query), 
             "GET %s HTTP/1.0\r\nHost: %s:%s\r\n\r\n",
             file, host, port);

#ifdef DEBUG
        printf("query: %s", query);
#endif

    if (write(sd, query, strlen(query)) < 1)
      return 0;

    /* Query for stream info */
    fp = NULL;
    total_sz = 0;
    n_buf_blks = 0;
    buf = NULL;

    while ((recv_sz = read(sd, data, sizeof(data))) > 0 )
    {
        if ((recv_sz + total_sz) >= (n_buf_blks * buf_blk_sz))
          buf = realloc(buf, (++n_buf_blks) * buf_blk_sz);

        memcpy(buf + total_sz, data, recv_sz);
        total_sz += recv_sz;
    }

    /* Get the potential new host from the just read in data */
    if (buf && !(c = strstr(buf, "http://")))
      return 0;
    else if (buf)
    {
        strtok(c, "\n");
        util_url_to_host_port_file(c, &newhost);
    }

    /* Disconnect here and contact server in m3u/pls */
    close(sd);
    if (!(sd = connect_host(newhost.host, newhost.portnum)))
      return 0;

    /* Get data from new host */
    snprintf(query, sizeof(query),
             "GET %s HTTP/1.0\r\nHost: %s:%s\r\n\r\n",
             newhost.file, newhost.host, newhost.port);

#ifdef DEBUG
        printf("query: %s", query);
#endif
        
    if (write(sd, query, strlen(query)) < 1)
      return 0;

    /* Create file to capture stream to */
    if ((flags & FLAG_CAPTURE_MODE) &&
        (!(fp = util_create_file(host, "captured-stream", "mp3"))))
      return 0;

    /* Pull data from stream and analyize */
    suck_data_from_stream(sd, fp, flags, host);

    /* Clean */
    free(buf);
    if (fp)
      fclose(fp);

    return 1;
}


void handle_as_stream(
    const char *url,
    flags_t     flags)
{
    int        sd;
    hostdata_t host;

    util_url_to_host_port_file(url, &host);
    
    if (!(sd = connect_host(host.host, host.portnum)))
      return;

    if (!(get_stream_info(flags, sd, host.host, host.port, host.file)))
      return;

    /* Disconnect */
    close(sd);
}

/*
    Copyright 2016-2021 Arisotura

    This file is part of melonDS.

    melonDS is free software: you can redistribute it and/or modify it under
    the terms of the GNU General Public License as published by the Free
    Software Foundation, either version 3 of the License, or (at your option)
    any later version.

    melonDS is distributed in the hope that it will be useful, but WITHOUT ANY
    WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
    FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.

    You should have received a copy of the GNU General Public License along
    with melonDS. If not, see http://www.gnu.org/licenses/.
*/

// indirect LAN interface, powered by BSD sockets.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <cctype>
#include <stdarg.h>
#include "Wifi.h"
#include "LAN_Socket.h"
#include "Config.h"
#include "FIFO.h"

#include <slirp/libslirp.h>

#ifdef __WIN32__
	#include <ws2tcpip.h>
#else
	#include <sys/socket.h>
	#include <netdb.h>
	#include <poll.h>
	#include <time.h>
#endif


namespace LAN_Socket
{

const u32 kSubnet   = 0x0A400000;
const u32 kServerIP = kSubnet | 0x01;
const u32 kDNSIP    = kSubnet | 0x02;
const u32 kClientIP = kSubnet | 0x10;

const u8 kServerMAC[6] = {0x00, 0xAB, 0x33, 0x28, 0x99, 0x44};

FIFO<u32, (0x8000 >> 2)> RXBuffer;

u32 IPv4ID;

Slirp* Ctx = nullptr;

namespace
{

inline u16 ReadBE16(const u8* p)
{
    return (u16)((p[0] << 8) | p[1]);
}

inline u32 ReadBE32(const u8* p)
{
    return (u32)((p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3]);
}

void Append(char* out, size_t outlen, size_t& used, const char* fmt, ...)
{
    if (used >= outlen) return;
    va_list args;
    va_start(args, fmt);
    int wrote = vsnprintf(out + used, outlen - used, fmt, args);
    va_end(args);
    if (wrote > 0) used += (size_t)wrote;
}

void MacToStr(const u8* mac, char* out, size_t outlen)
{
    if (outlen < 18) { if (outlen) out[0] = '\0'; return; }
    snprintf(out, outlen, "%02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

void IPv4ToStr(const u8* ip, char* out, size_t outlen)
{
    snprintf(out, outlen, "%u.%u.%u.%u", ip[0], ip[1], ip[2], ip[3]);
}

void TcpFlagsToStr(u8 flags, char* out, size_t outlen)
{
    char tmp[8];
    int o = 0;
    if (flags & 0x01) tmp[o++] = 'F';
    if (flags & 0x02) tmp[o++] = 'S';
    if (flags & 0x04) tmp[o++] = 'R';
    if (flags & 0x08) tmp[o++] = 'P';
    if (flags & 0x10) tmp[o++] = 'A';
    if (flags & 0x20) tmp[o++] = 'U';
    if (flags & 0x40) tmp[o++] = 'E';
    if (flags & 0x80) tmp[o++] = 'C';
    tmp[o] = '\0';
    snprintf(out, outlen, "%s", o ? tmp : "-");
}

void DescribeDns(const u8* payload, int payloadLen, char* out, size_t outlen, size_t& used)
{
    if (payloadLen < 12) return;
    u16 id = ReadBE16(payload);
    u16 flags = ReadBE16(payload + 2);
    u16 qd = ReadBE16(payload + 4);
    u16 an = ReadBE16(payload + 6);
    Append(out, outlen, used, " dns id=%04X flags=%04X q=%u a=%u", id, flags, qd, an);

    if (qd == 0) return;
    int off = 12;
    char name[128];
    int nameLen = 0;
    while (off < payloadLen && payload[off] != 0)
    {
        u8 len = payload[off];
        if (len & 0xC0) { off += 2; break; } // pointer
        off++;
        for (int i = 0; i < len && off < payloadLen && nameLen < (int)sizeof(name)-1; i++, off++)
        {
            name[nameLen++] = (char)payload[off];
        }
        if (nameLen < (int)sizeof(name)-1) name[nameLen++] = '.';
    }
    if (nameLen > 0 && name[nameLen-1] == '.') nameLen--;
    name[nameLen] = '\0';
    if (off < payloadLen) off++;
    if (off + 4 <= payloadLen)
    {
        u16 qtype = ReadBE16(payload + off);
        u16 qclass = ReadBE16(payload + off + 2);
        Append(out, outlen, used, " qname=%s qtype=%u qclass=%u", name, qtype, qclass);
    }
}

void DescribeFrame(const u8* data, int len, char* out, size_t outlen)
{
    size_t used = 0;
    if (len < 14)
    {
        Append(out, outlen, used, "len=%d (short)", len);
        return;
    }

    char dstmac[32], srcmac[32];
    MacToStr(data, dstmac, sizeof(dstmac));
    MacToStr(data + 6, srcmac, sizeof(srcmac));
    u16 ethertype = ReadBE16(data + 12);
    Append(out, outlen, used, "eth dst=%s src=%s type=0x%04X len=%d", dstmac, srcmac, ethertype, len);

    if (ethertype == 0x0800 && len >= 34)
    {
        u8 ihl = (data[14] & 0x0F) * 4;
        if (ihl < 20 || len < 14 + ihl) return;
        u8 proto = data[23];
        char srcip[32], dstip[32];
        IPv4ToStr(data + 26, srcip, sizeof(srcip));
        IPv4ToStr(data + 30, dstip, sizeof(dstip));
        Append(out, outlen, used, " ip %s -> %s proto=%u", srcip, dstip, proto);

        int l4 = 14 + ihl;
        int payloadLen = len - l4;
        if (proto == 6 && payloadLen >= 20)
        {
            u16 sport = ReadBE16(data + l4);
            u16 dport = ReadBE16(data + l4 + 2);
            u32 seq = ReadBE32(data + l4 + 4);
            u32 ack = ReadBE32(data + l4 + 8);
            u8 dataoff = (data[l4 + 12] >> 4) * 4;
            u8 flags = data[l4 + 13];
            u16 win = ReadBE16(data + l4 + 14);
            char fstr[16];
            TcpFlagsToStr(flags, fstr, sizeof(fstr));
            Append(out, outlen, used, " tcp %u -> %u flags=%s seq=%u ack=%u win=%u", sport, dport, fstr, seq, ack, win);

            int poff = l4 + dataoff;
            int plen = len - poff;
            if (plen > 0)
            {
                Append(out, outlen, used, " payload=%d", plen);
                if (plen >= 2 && data[poff] == 0x16 && data[poff+1] == 0x03)
                    Append(out, outlen, used, " tls=handshake");
                else if (plen >= 2 && data[poff] == 0x15 && data[poff+1] == 0x03)
                    Append(out, outlen, used, " tls=alert");
                else if (plen >= 4 && memcmp(&data[poff], "GET ", 4) == 0)
                    Append(out, outlen, used, " http=GET");
                else if (plen >= 5 && memcmp(&data[poff], "POST ", 5) == 0)
                    Append(out, outlen, used, " http=POST");
            }
        }
        else if (proto == 17 && payloadLen >= 8)
        {
            u16 sport = ReadBE16(data + l4);
            u16 dport = ReadBE16(data + l4 + 2);
            Append(out, outlen, used, " udp %u -> %u", sport, dport);
            int uoff = l4 + 8;
            int ulen = len - uoff;
            if (sport == 53 || dport == 53)
                DescribeDns(data + uoff, ulen, out, outlen, used);
        }
    }
    else if (ethertype == 0x0806 && len >= 42)
    {
        u16 oper = ReadBE16(data + 20);
        char spa[32], tpa[32];
        IPv4ToStr(data + 28, spa, sizeof(spa));
        IPv4ToStr(data + 38, tpa, sizeof(tpa));
        Append(out, outlen, used, " arp op=%u %s -> %s", oper, spa, tpa);
    }
}

}

/*const int FDListMax = 64;
struct pollfd FDList[FDListMax];
int FDListSize;*/


#ifdef __WIN32__

#define poll WSAPoll

// https://stackoverflow.com/questions/5404277/porting-clock-gettime-to-windows

struct timespec { long tv_sec; long tv_nsec; };
#define CLOCK_MONOTONIC 1312

int clock_gettime(int, struct timespec *spec)
{
    __int64 wintime;
    GetSystemTimeAsFileTime((FILETIME*)&wintime);
    wintime -=116444736000000000LL;                 //1jan1601 to 1jan1970
    spec->tv_sec  = wintime / 10000000LL;           //seconds
    spec->tv_nsec = wintime % 10000000LL * 100;     //nano-seconds
    return 0;
}

#endif // __WIN32__


void RXEnqueue(const void* buf, int len)
{
    int alignedlen = (len + 3) & ~3;
    int totallen = alignedlen + 4;

    if (!RXBuffer.CanFit(totallen >> 2))
    {
        printf("slirp: !! NOT ENOUGH SPACE IN RX BUFFER\n");
        return;
    }

    u32 header = (alignedlen & 0xFFFF) | (len << 16);
    RXBuffer.Write(header);
    for (int i = 0; i < alignedlen; i += 4)
        RXBuffer.Write(((u32*)buf)[i>>2]);
}

ssize_t SlirpCbSendPacket(const void* buf, size_t len, void* opaque)
{
    if (len > 2048)
    {
        printf("slirp: packet too big (%zu)\n", len);
        return 0;
    }

    printf("slirp: response packet of %zu bytes, type %04X\n", len, ntohs(((u16*)buf)[6]));
    {
        char summary[512];
        DescribeFrame((const u8*)buf, (int)len, summary, sizeof(summary));
        printf("LAN_RX: %s\n", summary);
    }

    RXEnqueue(buf, len);

    return len;
}

void SlirpCbGuestError(const char* msg, void* opaque)
{
    printf("SLIRP: error: %s\n", msg);
}

int64_t SlirpCbClockGetNS(void* opaque)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

void* SlirpCbTimerNew(SlirpTimerCb cb, void* cb_opaque, void* opaque)
{
    return nullptr;
}

void SlirpCbTimerFree(void* timer, void* opaque)
{
}

void SlirpCbTimerMod(void* timer, int64_t expire_time, void* opaque)
{
}

void SlirpCbRegisterPollFD(int fd, void* opaque)
{
    printf("Slirp: register poll FD %d\n", fd);

    /*if (FDListSize >= FDListMax)
    {
        printf("!! SLIRP FD LIST FULL\n");
        return;
    }

    for (int i = 0; i < FDListSize; i++)
    {
        if (FDList[i].fd == fd) return;
    }

    FDList[FDListSize].fd = fd;
    FDListSize++;*/
}

void SlirpCbUnregisterPollFD(int fd, void* opaque)
{
    printf("Slirp: unregister poll FD %d\n", fd);

    /*if (FDListSize < 1)
    {
        printf("!! SLIRP FD LIST EMPTY\n");
        return;
    }

    for (int i = 0; i < FDListSize; i++)
    {
        if (FDList[i].fd == fd)
        {
            FDListSize--;
            FDList[i] = FDList[FDListSize];
        }
    }*/
}

void SlirpCbNotify(void* opaque)
{
    printf("Slirp: notify???\n");
}

SlirpCb cb =
{
    .send_packet = SlirpCbSendPacket,
    .guest_error = SlirpCbGuestError,
    .clock_get_ns = SlirpCbClockGetNS,
    .timer_new = SlirpCbTimerNew,
    .timer_free = SlirpCbTimerFree,
    .timer_mod = SlirpCbTimerMod,
    .register_poll_fd = SlirpCbRegisterPollFD,
    .unregister_poll_fd = SlirpCbUnregisterPollFD,
    .notify = SlirpCbNotify
};

bool Init()
{
    IPv4ID = 0;

    //FDListSize = 0;
    //memset(FDList, 0, sizeof(FDList));

    SlirpConfig cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.version = 1;

    cfg.in_enabled = true;
    *(u32*)&cfg.vnetwork = htonl(kSubnet);
    *(u32*)&cfg.vnetmask = htonl(0xFFFFFF00);
    *(u32*)&cfg.vhost = htonl(kServerIP);
    cfg.vhostname = "melonServer";
    *(u32*)&cfg.vdhcp_start = htonl(kClientIP);
    *(u32*)&cfg.vnameserver = htonl(kDNSIP);

    Ctx = slirp_new(&cfg, &cb, nullptr);

    return true;
}

void DeInit()
{
    if (Ctx)
    {
        slirp_cleanup(Ctx);
        Ctx = nullptr;
    }
}


void FinishUDPFrame(u8* data, int len)
{
    u8* ipheader = &data[0xE];
    u8* udpheader = &data[0x22];

    // lengths
    *(u16*)&ipheader[2] = htons(len - 0xE);
    *(u16*)&udpheader[4] = htons(len - (0xE + 0x14));

    // IP checksum
    u32 tmp = 0;

    for (int i = 0; i < 20; i += 2)
        tmp += ntohs(*(u16*)&ipheader[i]);
    while (tmp >> 16)
        tmp = (tmp & 0xFFFF) + (tmp >> 16);
    tmp ^= 0xFFFF;
    *(u16*)&ipheader[10] = htons(tmp);

    // UDP checksum
    // (note: normally not mandatory, but some older sgIP versions require it)
    tmp = 0;
    tmp += ntohs(*(u16*)&ipheader[12]);
    tmp += ntohs(*(u16*)&ipheader[14]);
    tmp += ntohs(*(u16*)&ipheader[16]);
    tmp += ntohs(*(u16*)&ipheader[18]);
    tmp += ntohs(0x1100);
    tmp += (len-0x22);
    for (u8* i = udpheader; i < &udpheader[len-0x23]; i += 2)
        tmp += ntohs(*(u16*)i);
    if (len & 1)
        tmp += ntohs((u_short)udpheader[len-0x23]);
    while (tmp >> 16)
        tmp = (tmp & 0xFFFF) + (tmp >> 16);
    tmp ^= 0xFFFF;
    if (tmp == 0) tmp = 0xFFFF;
    *(u16*)&udpheader[6] = htons(tmp);
}

void HandleDNSFrame(u8* data, int len)
{
    u8* ipheader = &data[0xE];
    u8* udpheader = &data[0x22];
    u8* dnsbody = &data[0x2A];

    u32 srcip = ntohl(*(u32*)&ipheader[12]);
    u16 srcport = ntohs(*(u16*)&udpheader[0]);

    u16 id = ntohs(*(u16*)&dnsbody[0]);
    u16 flags = ntohs(*(u16*)&dnsbody[2]);
    u16 numquestions = ntohs(*(u16*)&dnsbody[4]);
    u16 numanswers = ntohs(*(u16*)&dnsbody[6]);
    u16 numauth = ntohs(*(u16*)&dnsbody[8]);
    u16 numadd = ntohs(*(u16*)&dnsbody[10]);

    printf("DNS: ID=%04X, flags=%04X, Q=%d, A=%d, auth=%d, add=%d\n",
           id, flags, numquestions, numanswers, numauth, numadd);

    // for now we only take 'simple' DNS requests
    if (flags & 0x8000) return;
    if (numquestions != 1 || numanswers != 0) return;

    u8 resp[1024];
    u8* out = &resp[0];

    // ethernet
    memcpy(out, &data[6], 6); out += 6;
    memcpy(out, kServerMAC, 6); out += 6;
    *(u16*)out = htons(0x0800); out += 2;

    // IP
    u8* resp_ipheader = out;
    *out++ = 0x45;
    *out++ = 0x00;
    *(u16*)out = 0; out += 2; // total length
    *(u16*)out = htons(IPv4ID); out += 2; IPv4ID++;
    *out++ = 0x00;
    *out++ = 0x00;
    *out++ = 0x80; // TTL
    *out++ = 0x11; // protocol (UDP)
    *(u16*)out = 0; out += 2; // checksum
    *(u32*)out = htonl(kDNSIP); out += 4; // source IP
    *(u32*)out = htonl(srcip); out += 4; // destination IP

    // UDP
    u8* resp_udpheader = out;
    *(u16*)out = htons(53); out += 2; // source port
    *(u16*)out = htons(srcport); out += 2; // destination port
    *(u16*)out = 0; out += 2; // length
    *(u16*)out = 0; out += 2; // checksum

    // DNS
    u8* resp_body = out;
    *(u16*)out = htons(id); out += 2; // ID
    *(u16*)out = htons(0x8000); out += 2; // flags
    *(u16*)out = htons(numquestions); out += 2; // num questions
    *(u16*)out = htons(numquestions); out += 2; // num answers
    *(u16*)out = 0; out += 2; // num authority
    *(u16*)out = 0; out += 2; // num additional

    u32 curoffset = 12;
    for (u16 i = 0; i < numquestions; i++)
    {
        if (curoffset >= (len-0x2A)) return;

        u8 bitlength = 0;
        while ((bitlength = dnsbody[curoffset++]) != 0)
            curoffset += bitlength;

        curoffset += 4;
    }

    u32 qlen = curoffset-12;
    if (qlen > 512) return;
    memcpy(out, &dnsbody[12], qlen); out += qlen;

    curoffset = 12;
	for (u16 i = 0; i < numquestions; i++)
	{
		// assemble the requested domain name
		u8 bitlength = 0;
		char domainname[256] = ""; int o = 0;
		while ((bitlength = dnsbody[curoffset++]) != 0)
		{
		    if ((o+bitlength) >= 255)
            {
                // welp. atleast try not to explode.
                domainname[o++] = '\0';
                break;
            }

			strncpy(&domainname[o], (const char *)&dnsbody[curoffset], bitlength);
			o += bitlength;

			curoffset += bitlength;
			if (dnsbody[curoffset] != 0)
				domainname[o++] = '.';
            else
                domainname[o++] = '\0';
		}

		u16 type = ntohs(*(u16*)&dnsbody[curoffset]);
		u16 cls = ntohs(*(u16*)&dnsbody[curoffset+2]);

		printf("- q%d: %04X %04X %s", i, type, cls, domainname);

		// get answer
		struct addrinfo dns_hint;
		struct addrinfo* dns_res;
		u32 addr_res;

		memset(&dns_hint, 0, sizeof(dns_hint));
		dns_hint.ai_family = AF_INET; // TODO: other address types (INET6, etc)
		if (getaddrinfo(domainname, "0", &dns_hint, &dns_res) == 0)
        {
            struct addrinfo* p = dns_res;
            while (p)
            {
                struct sockaddr_in* addr = (struct sockaddr_in*)p->ai_addr;
                addr_res = *(u32*)&addr->sin_addr;

                printf(" -> %d.%d.%d.%d",
                       addr_res & 0xFF, (addr_res >> 8) & 0xFF,
                       (addr_res >> 16) & 0xFF, addr_res >> 24);

                break;
                p = p->ai_next;
            }
        }
        else
        {
            printf(" shat itself :(");
            addr_res = 0;
        }

		printf("\n");
		curoffset += 4;

		// TODO: betterer support
		// (under which conditions does the C00C marker work?)
		*(u16*)out = htons(0xC00C); out += 2;
		*(u16*)out = htons(type); out += 2;
		*(u16*)out = htons(cls); out += 2;
		*(u32*)out = htonl(3600); out += 4; // TTL (hardcoded for now)
		*(u16*)out = htons(4); out += 2; // address length
		*(u32*)out = addr_res; out += 4; // address
    }

    u32 framelen = (u32)(out - &resp[0]);
    if (framelen & 1) { *out++ = 0; framelen++; }
    FinishUDPFrame(resp, framelen);

    {
        char summary[512];
        DescribeFrame(resp, (int)framelen, summary, sizeof(summary));
        printf("LAN_RX (dns): %s\n", summary);
    }

    RXEnqueue(resp, framelen);
}

int SendPacket(u8* data, int len)
{
    if (!Ctx) return 0;

    if (len > 2048)
    {
        printf("LAN_SendPacket: error: packet too long (%d)\n", len);
        return 0;
    }

    {
        char summary[512];
        DescribeFrame(data, len, summary, sizeof(summary));
        printf("LAN_TX: %s\n", summary);
    }

    u16 ethertype = ntohs(*(u16*)&data[0xC]);

    if (ethertype == 0x800)
    {
        u8 protocol = data[0x17];
        if (protocol == 0x11) // UDP
        {
            u16 dstport = ntohs(*(u16*)&data[0x24]);
            if (dstport == 53 && htonl(*(u32*)&data[0x1E]) == kDNSIP) // DNS
            {
                HandleDNSFrame(data, len);
                return len;
            }
        }
    }

    slirp_input(Ctx, data, len);
    return len;
}

const int PollListMax = 64;
struct pollfd PollList[PollListMax];
int PollListSize;

int SlirpCbAddPoll(int fd, int events, void* opaque)
{
    if (PollListSize >= PollListMax)
    {
        printf("slirp: POLL LIST FULL\n");
        return -1;
    }

    int idx = PollListSize++;

    //printf("Slirp: add poll: fd=%d, idx=%d, events=%08X\n", fd, idx, events);

    u16 evt = 0;

    if (events & SLIRP_POLL_IN) evt |= POLLIN;
    if (events & SLIRP_POLL_OUT) evt |= POLLWRNORM;

#ifndef __WIN32__
    // CHECKME
    if (events & SLIRP_POLL_PRI) evt |= POLLPRI;
    if (events & SLIRP_POLL_ERR) evt |= POLLERR;
    if (events & SLIRP_POLL_HUP) evt |= POLLHUP;
#endif // !__WIN32__

    PollList[idx].fd = fd;
    PollList[idx].events = evt;

    return idx;
}

int SlirpCbGetREvents(int idx, void* opaque)
{
    if (idx < 0 || idx >= PollListSize)
        return 0;

    //printf("Slirp: get revents, idx=%d, res=%04X\n", idx, FDList[idx].revents);

    u16 evt = PollList[idx].revents;
    int ret = 0;

    if (evt & POLLIN) ret |= SLIRP_POLL_IN;
    if (evt & POLLWRNORM) ret |= SLIRP_POLL_OUT;
    if (evt & POLLPRI) ret |= SLIRP_POLL_PRI;
    if (evt & POLLERR) ret |= SLIRP_POLL_ERR;
    if (evt & POLLHUP) ret |= SLIRP_POLL_HUP;

    return ret;
}

int RecvPacket(u8* data)
{
    if (!Ctx) return 0;

    int ret = 0;

    //if (PollListSize > 0)
    {
        u32 timeout = 0;
        PollListSize = 0;
        slirp_pollfds_fill(Ctx, &timeout, SlirpCbAddPoll, nullptr);
        int res = poll(PollList, PollListSize, timeout);
        slirp_pollfds_poll(Ctx, res<0, SlirpCbGetREvents, nullptr);
    }

    if (!RXBuffer.IsEmpty())
    {
        u32 header = RXBuffer.Read();
        u32 len = header & 0xFFFF;

        for (int i = 0; i < len; i += 4)
            ((u32*)data)[i>>2] = RXBuffer.Read();

        ret = header >> 16;
    }

    return ret;
}

}

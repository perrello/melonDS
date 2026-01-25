#include <cstdio>
#include <cstring>
#include <string>

#ifdef __EMSCRIPTEN__
#include <deque>
#include <utility>
#include <vector>
#include <cstdarg>
#include <emscripten/emscripten.h>
#endif

#if defined(_WIN32) && !defined(_XBOX)
#include <winsock2.h>
#include <windows.h>
#include <ws2tcpip.h>
#define socket_t    SOCKET
#define sockaddr_t  SOCKADDR
#define pcap_dev_name description
#else
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/in.h>
#define socket_t    int
#define sockaddr_t  struct sockaddr
#define closesocket close
#define pcap_dev_name name
#endif

#if defined(__HAIKU__)
#include <sys/select.h>
#endif

#ifdef HAVE_PCAP
#include "libui_sdl/LAN_PCap.h"
#include "libui_sdl/LAN_Socket.h"
#endif

#ifdef HAVE_LIBNX
#include <switch/services/bsd.h>
#endif

#ifndef HAVE_WIFI
#define SO_REUSEADDR 0
#define SO_BROADCAST 0
#define socket(domain, type, protocol) NULL
#define bind(sockfd, addr, addrlen) -1
#define setsockopt(sockfd, level, optname, optval, optlen) -1
#define sendto(sockfd, buf, len, flags, dest_addr, addrlen) 0
#define recvfrom(sockfd, buf, len, flags, src_addr, addrlen) 0
#endif

#ifdef HAVE_THREADS
#include <stdlib.h>

#include <rthreads/rthreads.h>
#include <rthreads/rsemaphore.h>
#endif

#include <streams/file_stream.h>
#include <streams/file_stream_transforms.h>
#include <retro_timers.h>

#include "types.h"
#include "utils.h"
#include "Platform.h"

extern char retro_base_directory[4096];

#ifndef INVALID_SOCKET
#define INVALID_SOCKET  (socket_t)-1
#endif

#define NIFI_VER 1

socket_t MPSocket;
sockaddr_t MPSendAddr;
u8 PacketBuffer[2048];

#ifdef __EMSCRIPTEN__
namespace
{
   std::deque<std::vector<u8>> mp_rx_queue;
   std::deque<std::vector<u8>> lan_rx_queue;
   int mp_client_id = 0;
   int mp_client_count = 0;

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
      if (used >= outlen)
         return;
      va_list args;
      va_start(args, fmt);
      int wrote = vsnprintf(out + used, outlen - used, fmt, args);
      va_end(args);
      if (wrote > 0)
         used += (size_t)wrote;
   }

   void MacToStr(const u8* mac, char* out, size_t outlen)
   {
      if (outlen < 18)
      {
         if (outlen) out[0] = '\0';
         return;
      }
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
      if (payloadLen < 12)
         return;
      u16 id = ReadBE16(payload);
      u16 flags = ReadBE16(payload + 2);
      u16 qd = ReadBE16(payload + 4);
      u16 an = ReadBE16(payload + 6);
      Append(out, outlen, used, " dns id=%04X flags=%04X q=%u a=%u", id, flags, qd, an);

      if (qd == 0)
         return;
      int off = 12;
      char name[128];
      int nameLen = 0;
      while (off < payloadLen && payload[off] != 0)
      {
         u8 len = payload[off];
         if (len & 0xC0)
         {
            off += 2;
            break;
         }
         off++;
         for (int i = 0; i < len && off < payloadLen && nameLen < (int)sizeof(name) - 1; i++, off++)
            name[nameLen++] = (char)payload[off];
         if (nameLen < (int)sizeof(name) - 1)
            name[nameLen++] = '.';
      }
      if (nameLen > 0 && name[nameLen - 1] == '.')
         nameLen--;
      name[nameLen] = '\0';
      if (off < payloadLen) off++;
      if (off + 4 <= payloadLen)
      {
         u16 qtype = ReadBE16(payload + off);
         u16 qclass = ReadBE16(payload + off + 2);
         Append(out, outlen, used, " qname=%s qtype=%u qclass=%u", name, qtype, qclass);
      }
   }

   const char* TlsRecordTypeName(u8 t)
   {
      switch (t)
      {
         case 0x14: return "change_cipher_spec";
         case 0x15: return "alert";
         case 0x16: return "handshake";
         case 0x17: return "application";
         default: return "unknown";
      }
   }

   const char* TlsHandshakeTypeName(u8 t)
   {
      switch (t)
      {
         case 0x01: return "client_hello";
         case 0x02: return "server_hello";
         case 0x0B: return "certificate";
         case 0x0E: return "server_hello_done";
         case 0x10: return "client_key_exchange";
         case 0x14: return "finished";
         default: return "other";
      }
   }

   const char* TlsAlertLevelName(u8 t)
   {
      switch (t)
      {
         case 0x01: return "warning";
         case 0x02: return "fatal";
         default: return "unknown";
      }
   }

   const char* TlsAlertDescName(u8 t)
   {
      switch (t)
      {
         case 0x0A: return "unexpected_message";
         case 0x14: return "bad_record_mac";
         case 0x15: return "decryption_failed";
         case 0x28: return "handshake_failure";
         case 0x2A: return "bad_certificate";
         case 0x2E: return "certificate_unknown";
         case 0x30: return "illegal_parameter";
         case 0x46: return "protocol_version";
         case 0x47: return "insufficient_security";
         default: return "other";
      }
   }

   void DescribeTls(const u8* payload, int payloadLen, char* out, size_t outlen, size_t& used)
   {
      if (payloadLen < 5)
         return;
      u8 rectype = payload[0];
      u16 version = ReadBE16(payload + 1);
      u16 rlen = ReadBE16(payload + 3);
      if (rectype < 0x14 || rectype > 0x17)
         return;
      Append(out, outlen, used, " tls=%s v=%04X rlen=%u", TlsRecordTypeName(rectype), version, rlen);
      if (rectype == 0x16 && payloadLen >= 9)
      {
         u8 hstype = payload[5];
         Append(out, outlen, used, " hs=%s", TlsHandshakeTypeName(hstype));
      }
      else if (rectype == 0x15 && payloadLen >= 7)
      {
         u8 level = payload[5];
         u8 desc = payload[6];
         Append(out, outlen, used, " alert=%s/%s(%u)", TlsAlertLevelName(level), TlsAlertDescName(desc), desc);
      }
   }

   void DescribeHttp(const u8* payload, int payloadLen, char* out, size_t outlen, size_t& used)
   {
      if (payloadLen < 5)
         return;
      if (payloadLen >= 5 && memcmp(payload, "HTTP/", 5) == 0)
      {
         if (payloadLen >= 12)
         {
            char code[4] = {0};
            memcpy(code, payload + 9, 3);
            Append(out, outlen, used, " http=RESP %s", code);
         }
         else
         {
            Append(out, outlen, used, " http=RESP");
         }
      }
      else if (payloadLen >= 4 && memcmp(payload, "GET ", 4) == 0)
         Append(out, outlen, used, " http=GET");
      else if (payloadLen >= 5 && memcmp(payload, "POST ", 5) == 0)
         Append(out, outlen, used, " http=POST");
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
         if (ihl < 20 || len < 14 + ihl)
            return;
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
               DescribeTls(data + poff, plen, out, outlen, used);
               DescribeHttp(data + poff, plen, out, outlen, used);
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

   void LogLanFrame(const char* tag, const u8* data, int len)
   {
      char summary[512];
      DescribeFrame(data, len, summary, sizeof(summary));
      printf("%s %s\n", tag, summary);
   }
}

EM_JS(void, netpacket_bridge_send, (int flags, const u8* data, int len), {
   if (typeof Module === 'undefined') return;
   var bridge = Module.NetpacketBridge;
   if (bridge && typeof bridge.onSend === 'function') {
      var payload = HEAPU8.slice(data, data + len);
      bridge.onSend(flags, payload);
   }
});

EM_JS(int, netpacket_bridge_connected, (int clientId, int maxClients), {
   if (typeof Module === 'undefined') return 1;
   var bridge = Module.NetpacketBridge;
   if (bridge && typeof bridge.onConnected === 'function') {
      return bridge.onConnected(clientId, maxClients) ? 1 : 0;
   }
   return 1;
});

EM_JS(void, netpacket_bridge_disconnected, (int clientId), {
   if (typeof Module === 'undefined') return;
   var bridge = Module.NetpacketBridge;
   if (bridge && typeof bridge.onDisconnected === 'function') {
      bridge.onDisconnected(clientId);
   }
});

EM_JS(void, lan_packet_bridge_send, (const u8* data, int len), {
   if (typeof Module === 'undefined') return;
   var bridge = Module.LANPacketBridge;
   if (bridge && typeof bridge.onSend === 'function') {
      var payload = HEAPU8.slice(data, data + len);
      bridge.onSend(payload);
   }
});

extern "C" {
EMSCRIPTEN_KEEPALIVE
void netpacket_receive(const u8* data, int len, int client_id)
{
   (void)client_id;
   if (!data || len <= 0)
      return;

   std::vector<u8> frame(len);
   memcpy(frame.data(), data, len);
   mp_rx_queue.push_back(std::move(frame));
}

EMSCRIPTEN_KEEPALIVE
void netpacket_set_client_state(int client_id, int max_clients)
{
   mp_client_id = client_id;
   mp_client_count = max_clients;
   netpacket_bridge_connected(client_id, max_clients);
}

EMSCRIPTEN_KEEPALIVE
void lan_packet_receive(const u8* data, int len)
{
   if (!data || len <= 0)
      return;

   std::vector<u8> frame(len);
   memcpy(frame.data(), data, len);
   lan_rx_queue.push_back(std::move(frame));
}
} // extern "C"
#endif

namespace Platform
{
    FILE* OpenFile(const char* path, const char* mode, bool mustexist)
    {
    FILE* ret;

    if (mustexist)
    {
        ret = fopen(path, "rb");
        if (ret) fclose(ret);
        ret = fopen(path, mode);
    }
    else
        ret = fopen(path, mode);

    return ret;
}

   FILE* OpenLocalFile(const char* path, const char* mode)
   {
      std::string fullpath = std::string(retro_base_directory) + std::string(1, PLATFORM_DIR_SEPERATOR) + std::string(path);
      FILE* f = OpenFile(fullpath.c_str(), mode, true);
      return f;
   }

   FILE* OpenDataFile(const char* path)
   {
      return OpenLocalFile(path, "rb");
   }

   void StopEmu()
   {
       return;
   }

   void Semaphore_Reset(Semaphore *sema)
   {
   #ifdef HAVE_THREADS
      while (ssem_get((ssem_t*)sema) > 0) {
        ssem_trywait((ssem_t*)sema);
      }
   #endif
   }

   void Semaphore_Post(Semaphore *sema, int count)
   {
   #ifdef HAVE_THREADS
       for (int i = 0; i < count; i++)
      {
         ssem_signal((ssem_t*)sema);
      }
   #endif
   }

   void Semaphore_Wait(Semaphore *sema)
   {
   #ifdef HAVE_THREADS
      ssem_wait((ssem_t*)sema);
   #endif
   }

   void Semaphore_Free(Semaphore *sema)
   {
   #ifdef HAVE_THREADS
      ssem_t *sem = (ssem_t*)sema;
      if (sem)
         ssem_free(sem);
   #endif
   }

   Semaphore *Semaphore_Create()
   {
   #ifdef HAVE_THREADS
      ssem_t *sem = ssem_new(0);
      if (sem)
         return (Semaphore*)sem;
   #endif
      return NULL;
   }

   Mutex* Mutex_Create()
   {
   #ifdef HAVE_THREADS
      return (Mutex*)slock_new();
   #endif
      return NULL;
   }

   void Mutex_Free(Mutex* mutex)
   {
   #ifdef HAVE_THREADS
      slock_free((slock_t*)mutex);
   #endif
   }

   void Mutex_Lock(Mutex* mutex)
   {
   #ifdef HAVE_THREADS
      slock_lock((slock_t*)mutex);
   #endif
   }

   void Mutex_Unlock(Mutex* mutex)
   {
   #ifdef HAVE_THREADS
      slock_unlock((slock_t*)mutex);
   #endif
   }

   bool Mutex_TryLock(Mutex* mutex)
   {
   #ifdef HAVE_THREADS
      slock_try_lock((slock_t*)mutex);
   #endif
      return true;
   }

   void Thread_Free(Thread *thread)
   {
   #if HAVE_THREADS
      sthread_detach((sthread_t*)thread);
   #endif
   }

   struct ThreadData
   {
      std::function<void()> fn;
   };

   void function_trampoline(void* param) {
      ThreadData* data = (ThreadData*)param;
      data->fn();
      delete data;
   }

   Thread *Thread_Create(std::function<void()> func)
   {
   #if HAVE_THREADS
      return (Thread*) sthread_create(function_trampoline, new ThreadData{func});
   #endif
   }

   void Thread_Wait(Thread *thread)
   {
   #if HAVE_THREADS
      sthread_join((sthread_t*)thread);
   #endif
   }


   bool MP_Init()
   {
#ifdef __EMSCRIPTEN__
      mp_rx_queue.clear();
      netpacket_bridge_connected(mp_client_id, mp_client_count);
      return true;
#else
      int opt_true = 1;
      int res;

#ifdef _WIN32
      WSADATA wsadata;
      if (WSAStartup(MAKEWORD(2, 2), &wsadata) != 0)
      {
         return false;
      }
#endif // __WXMSW__

      MPSocket = socket(AF_INET, SOCK_DGRAM, 0);
      if (MPSocket < 0)
      {
         return false;
      }

      res = setsockopt(MPSocket, SOL_SOCKET, SO_REUSEADDR, (const char*)&opt_true, sizeof(int));
      if (res < 0)
      {
         closesocket(MPSocket);
         MPSocket = INVALID_SOCKET;
         return false;
      }

      sockaddr_t saddr;
      saddr.sa_family = AF_INET;
      *(u32*)&saddr.sa_data[2] = htonl(INADDR_ANY);
      *(u16*)&saddr.sa_data[0] = htons(7064);
      res = bind(MPSocket, &saddr, sizeof(sockaddr_t));
      if (res < 0)
      {
         closesocket(MPSocket);
         MPSocket = INVALID_SOCKET;
         return false;
      }

      res = setsockopt(MPSocket, SOL_SOCKET, SO_BROADCAST, (const char*)&opt_true, sizeof(int));
      if (res < 0)
      {
         closesocket(MPSocket);
         MPSocket = INVALID_SOCKET;
         return false;
      }

      MPSendAddr.sa_family = AF_INET;
      *(u32*)&MPSendAddr.sa_data[2] = htonl(INADDR_BROADCAST);
      *(u16*)&MPSendAddr.sa_data[0] = htons(7064);

      return true;
#endif
   }

   void MP_DeInit()
   {
#ifdef __EMSCRIPTEN__
      mp_rx_queue.clear();
      netpacket_bridge_disconnected(mp_client_id);
      return;
#else
      if (MPSocket >= 0)
         closesocket(MPSocket);

#ifdef _WIN32
      WSACleanup();
#endif // __WXMSW__
#endif
   }

   int MP_SendPacket(u8* data, int len)
   {
#ifdef __EMSCRIPTEN__
      if (len <= 0)
         return 0;
      if (len > 2048-8)
      {
         printf("MP_SendPacket: error: packet too long (%d)\n", len);
         return 0;
      }

      netpacket_bridge_send(0, data, len);
      return len;
#else
      if (MPSocket < 0)
      {
         printf("MP_SendPacket: early return (%d)\n", len);
         return 0;
      }

      if (len > 2048-8)
      {
         printf("MP_SendPacket: error: packet too long (%d)\n", len);
         return 0;
      }

      *(u32*)&PacketBuffer[0] = htonl(0x4946494E); // NIFI
      PacketBuffer[4] = NIFI_VER;
      PacketBuffer[5] = 0;
      *(u16*)&PacketBuffer[6] = htons(len);
      memcpy(&PacketBuffer[8], data, len);

      int slen = sendto(MPSocket, (const char*)PacketBuffer, len+8, 0, &MPSendAddr, sizeof(sockaddr_t));
      if (slen < 8) return 0;
      return slen - 8;

#endif
   }

   int MP_RecvPacket(u8* data, bool block)
   {
#ifdef __EMSCRIPTEN__
      (void)block;
      if (mp_rx_queue.empty())
         return 0;

      std::vector<u8> frame = std::move(mp_rx_queue.front());
      mp_rx_queue.pop_front();

      int len = (int)frame.size();
      if (len > 2048)
         len = 2048;

      memcpy(data, frame.data(), len);
      return len;
#else
      if (MPSocket < 0)
      {
         printf("MP_RecvPacket: early return\n");
         return 0;
      }

      fd_set fd;
      struct timeval tv;

      FD_ZERO(&fd);
      FD_SET(MPSocket, &fd);
      tv.tv_sec = 0;
      tv.tv_usec = block ? 5000 : 0;

      if (!select(MPSocket+1, &fd, 0, 0, &tv))
      {
         return 0;
      }

      sockaddr_t fromAddr;
      socklen_t fromLen = sizeof(sockaddr_t);
      int rlen = recvfrom(MPSocket, (char*)PacketBuffer, 2048, 0, &fromAddr, &fromLen);
      if (rlen < 8+24)
      {
         return 0;
      }
      rlen -= 8;

      if (ntohl(*(u32*)&PacketBuffer[0]) != 0x4946494E)
      {
         return 0;
      }

      if (PacketBuffer[4] != NIFI_VER)
      {
         return 0;
      }

      if (ntohs(*(u16*)&PacketBuffer[6]) != rlen)
      {
        return 0;
      }

      memcpy(data, &PacketBuffer[8], rlen);
      return rlen;
#endif
   }

   bool LAN_Init()
   {
#ifdef __EMSCRIPTEN__
      lan_rx_queue.clear();
      return true;
#elif defined(HAVE_PCAP)
    if (Config::DirectLAN)
    {
        if (!LAN_PCap::Init(true))
            return false;
    }
    else
    {
        if (!LAN_Socket::Init())
            return false;
    }

    return true;
#else
   return false;
#endif
   }

   void LAN_DeInit()
   {
#ifdef __EMSCRIPTEN__
      lan_rx_queue.clear();
      return;
#elif defined(HAVE_PCAP)
      // checkme. blarg
      //if (Config::DirectLAN)
      //    LAN_PCap::DeInit();
      //else
      //    LAN_Socket::DeInit();
      LAN_PCap::DeInit();
      LAN_Socket::DeInit();
#endif
   }

   int LAN_SendPacket(u8* data, int len)
   {
#ifdef __EMSCRIPTEN__
      if (len <= 0)
         return 0;
      if (len > 2048)
      {
         printf("LAN_SendPacket: error: packet too long (%d)\n", len);
         return 0;
      }

      LogLanFrame("LAN_TX:", data, len);
      lan_packet_bridge_send(data, len);
      return len;
#elif defined(HAVE_PCAP)
      if (Config::DirectLAN)
         return LAN_PCap::SendPacket(data, len);
      else
         return LAN_Socket::SendPacket(data, len);
#else
      return 0;
#endif
   }

   int LAN_RecvPacket(u8* data)
   {
#ifdef __EMSCRIPTEN__
      if (lan_rx_queue.empty())
         return 0;

      std::vector<u8> frame = std::move(lan_rx_queue.front());
      lan_rx_queue.pop_front();

      int len = (int)frame.size();
      if (len > 2048)
         len = 2048;

      LogLanFrame("LAN_RX:", frame.data(), len);
      memcpy(data, frame.data(), len);
      return len;
#elif defined(HAVE_PCAP)
      if (Config::DirectLAN)
         return LAN_PCap::RecvPacket(data);
      else
         return LAN_Socket::RecvPacket(data);
#else
      return 0;
#endif
   }

#ifdef HAVE_OPENGL
   void* GL_GetProcAddress(const char* proc)
   {
      return (void*)nullptr;
   }
#endif

   void Sleep(u64 usecs)
   {
      retro_sleep(usecs / 1000);
   }
};

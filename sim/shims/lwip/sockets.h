/* sim/shims/lwip/sockets.h — host 桩：lwip socket API 与 BSD socket 同构。
 * UDP 发现、配对探测用的 socket/sendto/recvfrom 全部走系统实现。 */
#pragma once

#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#ifndef IPPROTO_IP
#define IPPROTO_IP 0
#endif

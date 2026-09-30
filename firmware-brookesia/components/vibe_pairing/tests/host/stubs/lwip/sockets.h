#pragma once
#include "mock.hpp"
#define socket mock_socket
#define setsockopt mock_setsockopt
#define sendto mock_sendto
#define recvfrom mock_recvfrom
#define close mock_close

/**
 * File: traceroute.cpp
 * Compile:
 *   Linux:   g++ -std=c++11 -o traceroute traceroute.cpp
 *   Windows: cl traceroute.cpp /EHsc iphlpapi.lib ws2_32.lib
 * Run (Administrator / root):
 *   traceroute.exe 8.8.8.8
 *
 * Description:
 *   Traceroute to target IP, print each hop with public/non-public classification.
 *   Windows: ICMP Echo + TTL (using IcmpSendEcho2)
 *   Linux:   UDP probes + raw socket
 */

#include <iostream>
#include <string>
#include <cstring>
#include <chrono>
#include <thread>
#include <vector>
#include <algorithm>
#include <cmath>

#ifdef _WIN32
    #define _WINSOCK_DEPRECATED_NO_WARNINGS
    #include <winsock2.h>
    #include <ws2tcpip.h>
    #include <iphlpapi.h>
    #include <icmpapi.h>      // for IcmpCreateFile, IcmpSendEcho2, etc.
    #pragma comment(lib, "ws2_32.lib")
    #pragma comment(lib, "iphlpapi.lib")
    typedef SOCKET socket_t;
    #define CLOSE_SOCKET closesocket
    #define INVALID_SOCKET_VAL INVALID_SOCKET
    #define SOCKET_ERROR_VAL SOCKET_ERROR
#else
    #include <unistd.h>
    #include <sys/socket.h>
    #include <netinet/ip.h>
    #include <netinet/ip_icmp.h>
    #include <netinet/udp.h>
    #include <arpa/inet.h>
    #include <fcntl.h>
    typedef int socket_t;
    #define CLOSE_SOCKET close
    #define INVALID_SOCKET_VAL (-1)
    #define SOCKET_ERROR_VAL (-1)
#endif

#define DEFAULT_MAX_HOPS 30
#define TIMEOUT_MS 1000

// ----------------------------------------------------------------------
//  IPv4 public address check (RFC 6890 based)
bool is_public_ipv4(const char *ip_str) {
    struct in_addr addr;
    if (inet_pton(AF_INET, ip_str, &addr) != 1) return false;
    uint32_t ip = ntohl(addr.s_addr);

    if ((ip >> 24) == 0) return false;                    // 0.0.0.0/8
    if ((ip & 0xFF000000) == 0x0A000000) return false;    // 10.0.0.0/8
    if ((ip & 0xFFC00000) == 0x64400000) return false;    // 100.64.0.0/10
    if ((ip & 0xFF000000) == 0x7F000000) return false;    // 127.0.0.0/8
    if ((ip & 0xFFFF0000) == 0xA9FE0000) return false;    // 169.254.0.0/16
    if ((ip & 0xFFF00000) == 0xAC100000) return false;    // 172.16.0.0/12
    if (ip == 0xC0000000) return false;                   // 192.0.0.0/24
    if ((ip & 0xFFFFFF00) == 0xC0000200) return false;    // 192.0.2.0/24
    if ((ip & 0xFFFFFF00) == 0xC0586300) return false;    // 192.88.99.0/24
    if ((ip & 0xFFFF0000) == 0xC0A80000) return false;    // 192.168.0.0/16
    if ((ip & 0xFFFE0000) == 0xC6120000) return false;    // 198.18.0.0/15
    if ((ip & 0xFFFFFF00) == 0xC6336400) return false;    // 198.51.100.0/24
    if ((ip & 0xFFFFFF00) == 0xCB007100) return false;    // 203.0.113.0/24
    if ((ip & 0xF0000000) >= 0xE0000000) return false;    // 224.0.0.0/4
    if ((ip & 0xF0000000) == 0xF0000000) return false;    // 240.0.0.0/4
    if (ip == 0xFFFFFFFF) return false;                   // 255.255.255.255

    return true;
}

// ----------------------------------------------------------------------
//  Windows implementation (ICMP traceroute using IcmpSendEcho2)
#ifdef _WIN32

bool traceroute(const char* dest_ip, int max_hops) {
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        std::cerr << "WSAStartup failed" << std::endl;
        return false;
    }

    // 1. Create ICMP handle (fixed: IcmpCreateFile, not lcmpCreateFile)
    HANDLE hIcmp = IcmpCreateFile();
    if (hIcmp == INVALID_HANDLE_VALUE) {
        std::cerr << "IcmpCreateFile failed" << std::endl;
        WSACleanup();
        return false;
    }

    // Convert target IP
    unsigned long dest_addr = inet_addr(dest_ip);
    if (dest_addr == INADDR_NONE) {
        std::cerr << "Invalid IP address" << std::endl;
        IcmpCloseHandle(hIcmp);
        WSACleanup();
        return false;
    }

    char send_data[32] = "Traceroute probe";
    DWORD reply_size   = sizeof(ICMP_ECHO_REPLY) + sizeof(send_data) + 8;
    char* reply_buffer = new char[reply_size];

    bool reached = false;

    for (int ttl = 1; ttl <= max_hops && !reached; ++ttl) {
        // Setup IP options with TTL
        IP_OPTION_INFORMATION option;
        option.Ttl         = (UCHAR)ttl;
        option.Tos         = 0;
        option.Flags       = 0;
        option.OptionsSize = 0;
        option.OptionsData = NULL;

        // 2. Send echo request (IcmpSendEcho2, not lcmpSendEcho2)
        DWORD reply_count = IcmpSendEcho2(hIcmp, NULL, NULL, NULL, dest_addr, send_data, sizeof(send_data), &option,
                                          reply_buffer, reply_size, TIMEOUT_MS);

        if (reply_count > 0) {
            PICMP_ECHO_REPLY pReply = (PICMP_ECHO_REPLY)reply_buffer;
            if (pReply->Status == IP_SUCCESS) {
                // Destination reached
                struct in_addr addr;
                addr.S_un.S_addr = pReply->Address;
                char hop_ip[INET_ADDRSTRLEN];
                inet_ntop(AF_INET, &addr, hop_ip, sizeof(hop_ip));
                const char* type = is_public_ipv4(hop_ip) ? "[PUBLIC]" : "[NON-PUBLIC]";
                std::cout << ttl << "  " << type << " " << hop_ip
                          << "  Destination reached  RTT=" << pReply->RoundTripTime << "ms" << std::endl;
                reached = true;
            } else if (pReply->Status == IP_TTL_EXPIRED_TRANSIT || pReply->Status == IP_TTL_EXPIRED_REASSEM) {
                // Intermediate hop
                struct in_addr addr;
                addr.S_un.S_addr = pReply->Address;
                char hop_ip[INET_ADDRSTRLEN];
                inet_ntop(AF_INET, &addr, hop_ip, sizeof(hop_ip));
                const char* type = is_public_ipv4(hop_ip) ? "[PUBLIC]" : "[NON-PUBLIC]";
                std::cout << ttl << "  " << type << " " << hop_ip
                          << "  ICMP Time Exceeded  RTT=" << pReply->RoundTripTime << "ms" << std::endl;
            } else {
                std::cout << ttl << "  -> No response (status=" << pReply->Status << ")" << std::endl;
            }
        } else {
            std::cout << ttl << "  -> No response (timeout)" << std::endl;
        }

        Sleep(500);
    }

    delete[] reply_buffer;
    IcmpCloseHandle(hIcmp); // fixed: IcmpCloseHandle
    WSACleanup();
    return reached;
}

// ----------------------------------------------------------------------
//  Linux implementation (UDP traceroute)
#else

bool traceroute(const char *dest_ip, int max_hops) {
    struct sockaddr_in dest_addr;
    dest_addr.sin_family = AF_INET;
    if (inet_pton(AF_INET, dest_ip, &dest_addr.sin_addr) != 1) {
        std::cerr << "Invalid target IP" << std::endl;
        return false;
    }

    socket_t send_sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (send_sock == INVALID_SOCKET_VAL) {
        perror("socket send");
        return false;
    }

    socket_t recv_sock = socket(AF_INET, SOCK_RAW, IPPROTO_ICMP);
    if (recv_sock == INVALID_SOCKET_VAL) {
        perror("socket recv (need root)");
        CLOSE_SOCKET(send_sock);
        return false;
    }

    struct timeval tv;
    tv.tv_sec = TIMEOUT_MS / 1000;
    tv.tv_usec = (TIMEOUT_MS % 1000) * 1000;
    setsockopt(recv_sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    bool reached = false;
    int base_port = 33434;

    for (int ttl = 1; ttl <= max_hops && !reached; ++ttl) {
        if (setsockopt(send_sock, IPPROTO_IP, IP_TTL, &ttl, sizeof(ttl)) < 0) {
            perror("setsockopt TTL");
            break;
        }

        int dest_port = base_port + ttl;
        dest_addr.sin_port = htons(dest_port);

        auto start = std::chrono::steady_clock::now();
        ssize_t sent = sendto(send_sock, NULL, 0, 0,
                              (struct sockaddr*)&dest_addr, sizeof(dest_addr));
        if (sent < 0) {
            perror("sendto");
            continue;
        }

        char buf[512];
        struct sockaddr_in from_addr;
        socklen_t from_len = sizeof(from_addr);
        ssize_t recv_len = recvfrom(recv_sock, buf, sizeof(buf), 0,
                                    (struct sockaddr*)&from_addr, &from_len);
        auto end = std::chrono::steady_clock::now();
        double rtt_ms = std::chrono::duration<double, std::milli>(end - start).count();

        if (recv_len < 0) {
            std::cout << ttl << "  -> No response (timeout)" << std::endl;
            continue;
        }

        struct iphdr *ip_h = (struct iphdr*)buf;
        int ip_hlen = ip_h->ihl * 4;
        struct icmphdr *icmp = (struct icmphdr*)(buf + ip_hlen);

        char hop_ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &from_addr.sin_addr, hop_ip, sizeof(hop_ip));
        const char *type = is_public_ipv4(hop_ip) ? "[PUBLIC]" : "[NON-PUBLIC]";

        if (icmp->type == 11) { // Time Exceeded
            std::cout << ttl << "  " << type << " " << hop_ip
                      << "  ICMP Time Exceeded  RTT=" << rtt_ms << "ms" << std::endl;
        } else if (icmp->type == 3 && icmp->code == 3) { // Port Unreachable
            std::cout << ttl << "  " << type << " " << hop_ip
                      << "  Destination reached  RTT=" << rtt_ms << "ms" << std::endl;
            reached = true;
        } else {
            std::cout << ttl << "  " << type << " " << hop_ip
                      << "  Unexpected ICMP type/code" << std::endl;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }

    CLOSE_SOCKET(send_sock);
    CLOSE_SOCKET(recv_sock);
    return reached;
}
#endif

// ----------------------------------------------------------------------
int main(int argc, char *argv[]) {
    if (argc < 2) {
        std::cerr << "Usage: " << argv[0] << " <target_ip> [max_hops]" << std::endl;
        return 1;
    }

    const char *target = argv[1];
    int max_hops = (argc >= 3) ? std::stoi(argv[2]) : DEFAULT_MAX_HOPS;

    std::cout << "Traceroute to " << target << ", max hops = " << max_hops << std::endl;
    std::cout << "Classification: [PUBLIC] vs [NON-PUBLIC]" << std::endl;
    std::cout << "--------------------------------------------------" << std::endl;

    bool success = traceroute(target, max_hops);

    if (!success) {
        std::cout << "\nFailed to reach destination." << std::endl;
    } else {
        std::cout << "\nTrace completed." << std::endl;
    }

    return 0;
}
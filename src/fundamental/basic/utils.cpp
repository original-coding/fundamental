#include "utils.hpp"

#if TARGET_PLATFORM_LINUX
    #include <arpa/inet.h>
    #include <net/ethernet.h>
    #include <net/if.h>
    #include <net/route.h>
    #include <netdb.h>
    #include <netinet/ether.h>
    #include <netinet/in.h>
    #include <netinet/ip.h>
    #include <netpacket/packet.h>
    #include <sys/ioctl.h>
    #include <sys/socket.h>
    #include <unistd.h>
    #define INVALID_SOCKET (-1)
    #define SOCKET         int
#elif TARGET_PLATFORM_WINDOWS
    #include <windows.h>
#endif

#include <cstdlib>
#include <cstring>

#include <iomanip>
#include <sstream>

namespace Fundamental
{
namespace Utils
{
fpid_t GetProcessId() {
    fpid_t ret = 0;
#if TARGET_PLATFORM_LINUX
    pid_t processID = getpid();
    ret             = static_cast<fpid_t>(processID);
#elif TARGET_PLATFORM_WINDOWS
    DWORD processID = GetCurrentProcessId();
    ret             = static_cast<fpid_t>(processID);
#endif
    return ret;
}

void SetThreadName(const std::string& name) {
#if TARGET_PLATFORM_LINUX
    ::pthread_setname_np(pthread_self(), name.substr(0, 15).c_str());
#endif
}

std::string BufferToHex(const void* buffer, std::size_t size, std::size_t group_size, std::int8_t spilt_char) {
    std::ostringstream oss;
    oss << std::hex << std::setfill('0');
    const std::uint8_t* ptr = static_cast<const std::uint8_t*>(buffer);
    for (size_t i = 0; i < size; ++i) {
        oss << std::setw(2) << static_cast<int>(ptr[i]);
        if (group_size > 0 && i > 0 && (i % group_size == 0))
            oss << '\n';
        else if (spilt_char > 0) {
            oss << static_cast<char>(spilt_char);
        }
    }
    return oss.str();
}
std::string BufferDumpAscii(const void* buffer, std::size_t size) {
    std::stringstream ss;
    const std::uint8_t* ptr = static_cast<const std::uint8_t*>(buffer);
    for (std::size_t i = 0; i < size; ++i) {
        char c = ptr[i];
        if (c >= 32 && c <= 126) ss << c;
    }
    return ss.str();
}
std::unordered_map<std::string, NetworkInfo> GetLocalNetInformation() {
    std::unordered_map<std::string, NetworkInfo> ret;
    do {
#if TARGET_PLATFORM_LINUX
        SOCKET sockfd = -1;
        char buf[512] = { 0 };
        struct ifconf ifconf;
        struct ifreq* ifreq;
        sockfd = socket(AF_INET, SOCK_DGRAM, 0);
        if (sockfd == INVALID_SOCKET) {
            close(sockfd);
            break;
        }
        ifconf.ifc_len = 512;
        ifconf.ifc_buf = buf;
        if (ioctl(sockfd, SIOCGIFCONF, &ifconf) < 0) {
            close(sockfd);
            break;
        }
        ifreq = (struct ifreq*)ifconf.ifc_buf;
        for (int i = (ifconf.ifc_len / sizeof(struct ifreq)); i > 0; i--) {
            if (ifreq->ifr_flags == AF_INET) {
                NetworkInfo newItem;
                newItem.ifName     = ifreq->ifr_name;
                newItem.isLoopback = 0 == strcmp(ifreq->ifr_name, "lo");
                newItem.ipv4       = inet_ntoa(((struct sockaddr_in*)&(ifreq->ifr_addr))->sin_addr);
                if (!newItem.isLoopback) {
                    struct ifreq ifr2;
                    strcpy(ifr2.ifr_name, ifreq->ifr_name);
                    uint8_t mac[6] = { 0 };
                    char mac1[128] = { 0 };
                    if ((ioctl(sockfd, SIOCGIFHWADDR, &ifr2)) == 0) {
                        memcpy(mac, ifr2.ifr_hwaddr.sa_data, 6);
                        sprintf(mac1, "%02x:%02x:%02x:%02x:%02x:%02x", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
                        newItem.mac = mac1;
                    }
                }
                ret.emplace(newItem.ifName, std::move(newItem));
                ifreq++;
            }
        }
        close(sockfd);
#endif
    } while (0);
    return ret;
}
std::string RemoveComments(std::string_view input) {
    std::string result;
    bool inSingleLineComment = false;
    bool inMultiLineComment  = false;
    for (size_t i = 0; i < input.size(); ++i) {
        if (inSingleLineComment) {
            if (input[i] == '\n') {
                inSingleLineComment = false;
                result += '\n';
            }
        } else if (inMultiLineComment) {
            if (input[i] == '*' && i + 1 < input.size() && input[i + 1] == '/') {
                inMultiLineComment = false;
                ++i;
            }
        } else {
            if (input[i] == '/' && i + 1 < input.size()) {
                if (input[i + 1] == '/') {
                    inSingleLineComment = true;
                    i++;                    
                } else if (input[i + 1] == '*') {
                    inMultiLineComment = true; 
                    i++;                     
                } else {
                    result += input[i];
                }
            } else {
                result += input[i];
            }
        }
    }

    return result;
}
} // namespace Utils
} // namespace Fundamental
#include "udp_socket.hpp"
#include "misc.hpp"
#include "socketoptions.hpp"

// submodules
#include "spdlog/spdlog.h"

// System includes for multicast
#include <arpa/inet.h>
#include <netinet/in.h>

using namespace std;
using namespace xtransmit;
using shared_udp = shared_ptr<socket::udp>;

#define LOG_SOCK_UDP "SOCKET::UDP "

socket::udp::udp(const UriParser &src_uri, Direction dir)
	: m_host(src_uri.host())
	, m_port(src_uri.portno())
	, m_options(src_uri.parameters())
	, m_is_multicast(false)
{
	// Safely set multicast flag after validating host
	if (!m_host.empty() && m_host.length() > 0) {
		m_is_multicast = is_multicast_address(m_host);
	}
	sockaddr_in sa     = sockaddr_in();
	sa.sin_family      = AF_INET;
	sa.sin_addr.s_addr = INADDR_ANY;
	m_bind_socket      = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);

	if (m_bind_socket == INVALID_SOCKET)
		throw socket::exception("Failed to create a UDP socket");

	if (m_options.count("blocking"))
	{
		m_blocking_mode = !false_names.count(m_options.at("blocking"));
		m_options.erase("blocking");
	}

	int yes = 1;
	::setsockopt(m_bind_socket, SOL_SOCKET, SO_REUSEADDR, (const char *)&yes, sizeof yes);

	if (!m_blocking_mode)
	{ // set non-blocking mode
		unsigned long nonblocking = 1;
#if defined(_WIN32)
		if (ioctlsocket(m_bind_socket, FIONBIO, &nonblocking) == SOCKET_ERROR)
#else
		if (ioctl(m_bind_socket, FIONBIO, (const char *)&nonblocking) < 0)
#endif
		{
			throw socket::exception("Failed to set blocking mode for UDP");
		}
	}

	if (m_host.empty()) {
		throw socket::exception("UDP socket host cannot be empty");
	}

	netaddr_any sa_requested;
	try
	{
		sa_requested = create_addr(m_host, m_port);
	}
	catch (const std::invalid_argument &)
	{
		throw socket::exception("create_addr_inet failed for host: '" + m_host + "'");
	}

	const auto bind_me = [&](const sockaddr* sa) {
		const int       bind_res = ::bind(m_bind_socket, sa, sizeof *sa);
		if (bind_res < 0)
		{
			throw socket::exception("UDP binding has failed");
		}
	};

	bool ip_bonded = false;
	netaddr_any sa_bind;
	memset(&sa_bind, 0, sizeof(sa_bind));
	
	if (m_options.count("bind"))
	{
		string bindipport = m_options.at("bind");
		transform(bindipport.begin(), bindipport.end(), bindipport.begin(), [](char c) { return tolower(c); });
		const size_t idx    = bindipport.find(":");
		const string bindip = bindipport.substr(0, idx);
		const int bindport = idx != string::npos
			? stoi(bindipport.substr(idx + 1, bindipport.size() - (idx + 1)))
			: m_port;
		m_options.erase("bind");

		try
		{
			sa_bind = create_addr(bindip, bindport);
		}
		catch (const std::invalid_argument&)
		{
			throw socket::exception("create_addr_inet failed");
		}

		// For non-multicast, bind to the specified interface
		// For multicast, we'll bind to the multicast address later
		if (!m_is_multicast) {
			bind_me(reinterpret_cast<const sockaddr*>(&sa_bind));
		}
		ip_bonded = true;
		spdlog::info(LOG_SOCK_UDP "udp://{}:{:d}: bound to '{}:{}'.",
			m_host, m_port, bindip, bindport);
	}

	// Use direction hint for multicast, fall back to heuristics
	bool is_multicast_receiver = false;
	if (m_is_multicast) {
		if (dir == Direction::RECEIVER) {
			is_multicast_receiver = true;
		} else if (dir == Direction::SENDER) {
			is_multicast_receiver = false;
		} else {
			// Fall back to current logic: multicast + bind = receiver
			is_multicast_receiver = m_is_multicast && ip_bonded;
		}
	}
	
	if (m_is_multicast)
	{
		// For multicast, always bind to the multicast address first (like swxtch)
		bind_me(reinterpret_cast<const sockaddr*>(&sa_requested));
		
		if (is_multicast_receiver)
		{
			// Multicast receiver: join the group
			setup_multicast_receiver(&sa_bind.sin);
			spdlog::info(LOG_SOCK_UDP "Configured for multicast reception from {}", m_host);
		}
		else
		{
			// Multicast sender: set outgoing interface
			setup_multicast_sender(ip_bonded ? &sa_bind.sin : nullptr);
			spdlog::info(LOG_SOCK_UDP "Configured for multicast transmission to {}", m_host);
		}
		m_dst_addr = sa_requested.sin;
	}
	else if (m_host != "" || ip_bonded)
	{
		m_dst_addr = sa_requested.sin;
	}
	else
	{
		bind_me(reinterpret_cast<const sockaddr*>(&sa_requested));
	}
}

socket::udp::~udp() { closesocket(m_bind_socket); }

size_t socket::udp::read(const mutable_buffer &buffer, int timeout_ms)
{
	while (!m_blocking_mode)
	{
		fd_set set;
		timeval tv;
		FD_ZERO(&set);
		FD_SET(m_bind_socket, &set);
		tv.tv_sec = 0;
		tv.tv_usec = 10000;
		const int select_ret = ::select((int)m_bind_socket + 1, &set, NULL, &set, &tv);

		if (select_ret != 0)    // ready
			break;

		if (timeout_ms >= 0)   // timeout
			return 0;
	}

	const int res =
		::recv(m_bind_socket, static_cast<char *>(buffer.data()), (int)buffer.size(), 0);
	if (res == -1)
	{
#ifndef _WIN32
#define NET_ERROR errno
#else
#define NET_ERROR WSAGetLastError()
#endif
		const int err = NET_ERROR;
		if (err != EAGAIN && err != EINTR && err != ECONNREFUSED)
			throw socket::exception("udp::read::recv");

		spdlog::info("UDP reading failed: error {0}. Again.", err);
		return 0;
	}

	return static_cast<size_t>(res);
}

int socket::udp::write(const const_buffer &buffer, int timeout_ms)
{
	while (!m_blocking_mode)
	{
		fd_set set;
		timeval tv;
		FD_ZERO(&set);
		FD_SET(m_bind_socket, &set);
		tv.tv_sec = 0;
		tv.tv_usec = 10000;
		const int select_ret = ::select((int)m_bind_socket + 1, nullptr, &set, &set, &tv);

		if (select_ret != 0)    // ready
			break;

		if (timeout_ms >= 0)   // timeout
			return 0;
	}

	const int res = ::sendto(m_bind_socket,
							 static_cast<const char *>(buffer.data()),
							 (int)buffer.size(),
							 0,
							 (sockaddr *)&m_dst_addr,
							 sizeof m_dst_addr);
	if (res == -1)
	{
#ifndef _WIN32
#define NET_ERROR errno
#else
#define NET_ERROR WSAGetLastError()
#endif
		const int err = NET_ERROR;
		if (err != EAGAIN && err != EINTR && err != ECONNREFUSED)
		{
			spdlog::info("udp::write::sendto: error {0}.", err);
			throw socket::exception("udp::write::sendto error");
		}

		spdlog::info("udp::sendto failed: error {0}. Again.", err);
		return 0;
	}

	return static_cast<size_t>(res);
}

bool socket::udp::is_multicast_address(const string& host) const
{
	if (host.empty() || host.length() == 0)
		return false;
		
	struct in_addr addr;
	memset(&addr, 0, sizeof(addr));
	
	if (inet_pton(AF_INET, host.c_str(), &addr) != 1)
		return false; // Invalid IP address
	
	// Multicast range: 224.0.0.0 to 239.255.255.255 (224.0.0.0/4)
	uint32_t ip = ntohl(addr.s_addr);
	return (ip >= 0xE0000000 && ip <= 0xEFFFFFFF);
}

void socket::udp::setup_multicast_sender(const sockaddr_in* bind_addr)
{
	// Set TTL for multicast packets
	int ttl = 1; // Default: local network only
	if (m_options.count("ttl"))
	{
		ttl = stoi(m_options.at("ttl"));
		m_options.erase("ttl");
	}
	
	if (setsockopt(m_bind_socket, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl)) < 0)
	{
		spdlog::warn(LOG_SOCK_UDP "Failed to set multicast TTL to {}", ttl);
	}
	else
	{
		spdlog::info(LOG_SOCK_UDP "Multicast TTL set to {}", ttl);
	}
	
	// Control multicast loopback
	int loopback = 1; // Default: enable loopback
	if (m_options.count("loopback"))
	{
		loopback = !false_names.count(m_options.at("loopback"));
		m_options.erase("loopback");
	}
	
	if (setsockopt(m_bind_socket, IPPROTO_IP, IP_MULTICAST_LOOP, &loopback, sizeof(loopback)) < 0)
	{
		spdlog::warn(LOG_SOCK_UDP "Failed to set multicast loopback");
	}
	else
	{
		spdlog::info(LOG_SOCK_UDP "Multicast loopback {}", loopback ? "enabled" : "disabled");
	}
	
	// Set outgoing interface for multicast packets
	if (bind_addr)
	{
		if (setsockopt(m_bind_socket, IPPROTO_IP, IP_MULTICAST_IF, &bind_addr->sin_addr, sizeof(bind_addr->sin_addr)) < 0)
		{
			spdlog::warn(LOG_SOCK_UDP "Failed to set multicast outgoing interface");
		}
		else
		{
			char addr_str[INET_ADDRSTRLEN];
			inet_ntop(AF_INET, &bind_addr->sin_addr, addr_str, INET_ADDRSTRLEN);
			spdlog::info(LOG_SOCK_UDP "Multicast outgoing interface set to {}", addr_str);
		}
	}
	
	// Allow explicit interface override
	if (m_options.count("interface"))
	{
		const string& interface = m_options.at("interface");
		if (interface.empty()) {
			spdlog::warn(LOG_SOCK_UDP "Empty interface parameter specified");
			m_options.erase("interface");
			return;
		}
		
		struct in_addr if_addr;
		if (inet_pton(AF_INET, interface.c_str(), &if_addr) == 1)
		{
			if (setsockopt(m_bind_socket, IPPROTO_IP, IP_MULTICAST_IF, &if_addr, sizeof(if_addr)) < 0)
			{
				spdlog::warn(LOG_SOCK_UDP "Failed to set multicast interface to {}", interface);
			}
			else
			{
				spdlog::info(LOG_SOCK_UDP "Multicast outgoing interface set to {}", interface);
			}
		}
		else
		{
			spdlog::warn(LOG_SOCK_UDP "Invalid interface address: {}", interface);
		}
		m_options.erase("interface");
	}
}

void socket::udp::setup_multicast_receiver(const sockaddr_in* bind_addr)
{
	if (m_host.empty()) {
		spdlog::error(LOG_SOCK_UDP "Cannot setup multicast receiver: empty host");
		return;
	}

	struct ip_mreq mreq;
	memset(&mreq, 0, sizeof(mreq));
	
	if (inet_pton(AF_INET, m_host.c_str(), &mreq.imr_multiaddr) != 1) {
		spdlog::error(LOG_SOCK_UDP "Invalid multicast address: '{}'", m_host);
		return;
	}
	
	// Use bound interface if available, otherwise INADDR_ANY
	mreq.imr_interface.s_addr = bind_addr ? bind_addr->sin_addr.s_addr : INADDR_ANY;
	
	// Allow overriding interface with explicit parameter
	if (m_options.count("interface"))
	{
		const string& interface = m_options.at("interface");
		if (inet_pton(AF_INET, interface.c_str(), &mreq.imr_interface) != 1)
		{
			spdlog::warn(LOG_SOCK_UDP "Invalid interface address: {}", interface);
			// Keep the bind_addr interface if it was set, otherwise use INADDR_ANY
			mreq.imr_interface.s_addr = bind_addr ? bind_addr->sin_addr.s_addr : INADDR_ANY;
		}
		m_options.erase("interface");
	}
	
	if (setsockopt(m_bind_socket, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq)) < 0)
	{
		throw socket::exception("Failed to join multicast group " + m_host);
	}
	
	spdlog::info(LOG_SOCK_UDP "Joined multicast group {}", m_host);
}

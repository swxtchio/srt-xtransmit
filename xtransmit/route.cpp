#include <atomic>
#include <chrono>
#include <ctime>
#include <future>
#include <iomanip>
#include <memory>
#include <thread>
#include <vector>

// submodules
#include "spdlog/spdlog.h"

// xtransmit
#include "srt_socket.hpp"
#include "udp_socket.hpp"
#include "misc.hpp"
#include "route.hpp"
#include "socket_stats.hpp"

// OpenSRT
#include "apputil.hpp"
#include "uriparser.hpp"

using namespace std;
using namespace xtransmit;
using namespace xtransmit::route;
using namespace std::chrono;

using shared_srt = std::shared_ptr<socket::srt>;
using shared_sock = std::shared_ptr<socket::isocket>;

#define LOG_SC_ROUTE "ROUTE "

namespace xtransmit
{
namespace route
{

	void route(shared_sock src, shared_sock dst,
		const config& cfg, const string&& desc, const atomic_bool& force_break)
	{
		vector<char> buffer(cfg.message_size);

		socket::isocket& sock_src = *src.get();
		socket::isocket& sock_dst = *dst.get();

		spdlog::info(LOG_SC_ROUTE "{0} Started", desc);

		while (!force_break)
		{
			const size_t bytes_read = sock_src.read(mutable_buffer(buffer.data(), buffer.size()), -1);
			// spdlog::info("{} recv {} bytes", desc, bytes_read);

			if (bytes_read == 0)
			{
				spdlog::info(LOG_SC_ROUTE "{} read 0 bytes on a socket (spurious read-ready?). Retrying.", desc);
				continue;
			}

			// SRT can return 0 on SRT_EASYNCSND. Rare for sending. However might be worth to retry.
			const int bytes_sent = sock_dst.write(const_buffer(buffer.data(), bytes_read));
			// spdlog::info("{} sent {} bytes", desc, bytes_sent);

			if (bytes_sent != bytes_read)
			{
				spdlog::info("{} write returned {} bytes, expected {}", desc, bytes_sent, bytes_read);
				continue;
			}
		}
	}
}
}


void xtransmit::route::run(const vector<string>& src_urls, const vector<string>& dst_urls,
	const config& cfg, const atomic_bool& force_break)
{
	vector<UriParser> parsed_src_urls;
	for (const string& url : src_urls)
	{
		parsed_src_urls.emplace_back(url);
	}

	vector<UriParser> parsed_dst_urls;
	for (const string& url : dst_urls)
	{
		parsed_dst_urls.emplace_back(url);
	}

	shared_sock_t src_sock; // A shared pointer to store a listening socket for multiple connections.
	shared_sock_t dst_sock; // A shared pointer to store a listening socket for multiple connections.
	steady_clock::time_point next_reconnect = steady_clock::now();

	do {
		try {
			const auto tnow = steady_clock::now();
			if (tnow < next_reconnect)
				this_thread::sleep_until(next_reconnect);

			next_reconnect = tnow + seconds(1);

			const bool write_stats = cfg.stats_file != "" && cfg.stats_freq_ms > 0;
			// make_unique is not supported by GCC 4.8, only starting from GCC 4.9 :(
			unique_ptr<socket::stats_writer> stats = write_stats
				? unique_ptr<socket::stats_writer>(new socket::stats_writer(cfg.stats_file, milliseconds(cfg.stats_freq_ms)))
				: nullptr;

			shared_sock dst = create_connection(parsed_dst_urls, dst_sock);
			shared_sock src = create_connection(parsed_src_urls, src_sock);

			// Closing a listener socket (if any) will not allow further connections.
			if (!cfg.reconnect)
				src_sock.reset();
				dst_sock.reset();

			if (stats)
			{
				stats->add_socket(src);
				stats->add_socket(dst);
			}

			future<void> route_bkwd = cfg.bidir
				? ::async(::launch::async, route, dst, src, cfg, "[DST->SRC]", ref(force_break))
				: future<void>();

			route(src, dst, cfg, "[SRC->DST]", force_break);
			route_bkwd.wait();

			if (stats)
				stats->remove_socket(src->id());
				stats->remove_socket(dst->id());
		}
		catch (const socket::exception & e)
		{
			spdlog::error(LOG_SC_ROUTE "{}", e.what());
		}
	} while (cfg.reconnect && !force_break);
}

CLI::App* xtransmit::route::add_subcommand(CLI::App& app, config& cfg, vector<string>& src_urls, vector<string>& dst_urls)
{
	const map<string, int> to_ms{ {"s", 1000}, {"ms", 1} };

	CLI::App* sc_route = app.add_subcommand("route", "Route data (SRT, UDP)")->fallthrough();
	sc_route->add_option("-i,--input",  src_urls, "Source URIs");
	sc_route->add_option("-o,--output", dst_urls, "Destination URIs");
	sc_route->add_option("--msgsize", cfg.message_size, "Size of a buffer to receive message payload");
	sc_route->add_flag("--bidir", cfg.bidir, "Enable bidirectional transmission");
	sc_route->add_flag("--reconnect", cfg.reconnect, "Reconnect automatically");
	sc_route->add_option("--statsfile", cfg.stats_file, "output stats report filename");
	sc_route->add_option("--statsfreq", cfg.stats_freq_ms, "output stats report frequency (ms)")
		->transform(CLI::AsNumberWithUnit(to_ms, CLI::AsNumberWithUnit::CASE_SENSITIVE));

	return sc_route;
}




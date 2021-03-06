/*

Copyright (c) 2008, Arvid Norberg
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions
are met:

    * Redistributions of source code must retain the above copyright
      notice, this list of conditions and the following disclaimer.
    * Redistributions in binary form must reproduce the above copyright
      notice, this list of conditions and the following disclaimer in
      the documentation and/or other materials provided with the distribution.
    * Neither the name of the author nor the names of its
      contributors may be used to endorse or promote products derived
      from this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
POSSIBILITY OF SUCH DAMAGE.

*/

#include <fstream>
#include <map>
#include <tuple>
#include <functional>
#include <random>

#include "libtorrent/session.hpp"
#include "libtorrent/hasher.hpp"
#include "libtorrent/http_parser.hpp"
#include "libtorrent/assert.hpp"
#include "libtorrent/alert_types.hpp"
#include "libtorrent/create_torrent.hpp"
#include "libtorrent/socket_io.hpp" // print_endpoint
#include "libtorrent/socket_type.hpp"
#include "libtorrent/ip_filter.hpp"
#include "libtorrent/session_stats.hpp"
#include "libtorrent/random.hpp"
#include "libtorrent/torrent_info.hpp"
#include "libtorrent/broadcast_socket.hpp" // for supports_ipv6()
#include "libtorrent/hex.hpp" // to_hex
#include "libtorrent/aux_/vector.hpp"
#include "libtorrent/aux_/path.hpp"

#include "test.hpp"
#include "test_utils.hpp"
#include "setup_transfer.hpp"

#ifdef TORRENT_USE_OPENSSL
#include <boost/asio/ssl/stream.hpp>
#include <boost/asio/ssl/context.hpp>
#endif

#ifndef _WIN32
#include <spawn.h>
#include <csignal>
#endif

using namespace lt;

#if defined TORRENT_WINDOWS
#include <conio.h>
#endif

namespace {
	std::uint32_t g_addr = 0x92343023;
}

void init_rand_address()
{
	g_addr = 0x92343023;
}

address rand_v4()
{
	address_v4 ret;
	do
	{
		g_addr += 0x3080ca;
		ret = address_v4(g_addr);
	} while (is_any(ret) || is_local(ret) || is_loopback(ret));
	return ret;
}

sha1_hash rand_hash()
{
	sha1_hash ret;
	for (int i = 0; i < 20; ++i)
		ret[static_cast<std::size_t>(i)] = std::uint8_t(lt::random(0xff));
	return ret;
}

sha1_hash to_hash(char const* s)
{
	sha1_hash ret;
	aux::from_hex({s, 40}, ret.data());
	return ret;
}

#if TORRENT_USE_IPV6
address rand_v6()
{
	address_v6::bytes_type bytes;
	for (int i = 0; i < int(bytes.size()); ++i)
		bytes[static_cast<std::size_t>(i)] = std::uint8_t(lt::random(0xff));
	return address_v6(bytes);
}
#endif

static std::uint16_t g_port = 0;

tcp::endpoint rand_tcp_ep(lt::address(&rand_addr)())
{
	// make sure we don't produce the same "random" port twice
	g_port = (g_port + 1) % 14038;
	return tcp::endpoint(rand_addr(), g_port + 1024);
}

udp::endpoint rand_udp_ep(lt::address(&rand_addr)())
{
	g_port = (g_port + 1) % 14037;
	return udp::endpoint(rand_addr(), g_port + 1024);
}

std::map<std::string, std::int64_t> get_counters(lt::session& s)
{
	using namespace lt;
	s.post_session_stats();

	std::map<std::string, std::int64_t> ret;
	alert const* a = wait_for_alert(s, session_stats_alert::alert_type
		, "get_counters()");

	TEST_CHECK(a);
	if (!a) return ret;

	session_stats_alert const* sa = alert_cast<session_stats_alert>(a);
	if (!sa) return ret;

	static std::vector<stats_metric> metrics = session_stats_metrics();
	for (auto const& m : metrics)
		ret[m.name] = sa->counters()[static_cast<std::size_t>(m.value_index)];
	return ret;
}
namespace {
bool should_print(lt::alert* a)
{
#ifndef TORRENT_DISABLE_LOGGING
	if (auto pla = alert_cast<peer_log_alert>(a))
	{
		if (pla->direction != peer_log_alert::incoming_message
			&& pla->direction != peer_log_alert::outgoing_message)
			return false;
	}
#endif
	if (alert_cast<session_stats_alert>(a)
		|| alert_cast<piece_finished_alert>(a)
		|| alert_cast<block_finished_alert>(a)
		|| alert_cast<block_downloading_alert>(a))
	{
		return false;
	}
	return true;
}
}
alert const* wait_for_alert(lt::session& ses, int type, char const* name
	, pop_alerts const p)
{
	// we pop alerts in batches, but we wait for individual messages. This is a
	// cache to keep around alerts that came *after* the one we're waiting for.
	// To let subsequent calls to this function be able to pick those up, despite
	// already being popped off the sessions alert queue.
	static std::map<lt::session*, std::vector<alert*>> cache;
	auto& alerts = cache[&ses];

	time_point end_time = lt::clock_type::now() + seconds(10);
	while (true)
	{
		time_point now = clock_type::now();
		if (now > end_time) return nullptr;

		alert const* ret = nullptr;

		if (alerts.empty())
		{
			ses.wait_for_alert(end_time - now);
			ses.pop_alerts(&alerts);
		}
		for (auto i = alerts.begin(); i != alerts.end(); ++i)
		{
			auto a = *i;
			if (should_print(a))
			{
				std::printf("%s: %s: [%s] %s\n", time_now_string(), name
					, a->what(), a->message().c_str());
			}
			if (a->type() == type)
			{
				ret = a;
				if (p == pop_alerts::pop_all) alerts.clear();
				else alerts.erase(alerts.begin(), std::next(i));
				return ret;
			}
		}
		alerts.clear();
	}
}

int load_file(std::string const& filename, std::vector<char>& v
	, lt::error_code& ec, int limit)
{
	ec.clear();
	FILE* f = fopen(filename.c_str(), "rb");
	if (f == nullptr)
	{
		ec.assign(errno, boost::system::system_category());
		return -1;
	}

	int r = fseek(f, 0, SEEK_END);
	if (r != 0)
	{
		ec.assign(errno, boost::system::system_category());
		fclose(f);
		return -1;
	}
	long s = ftell(f);
	if (s < 0)
	{
		ec.assign(errno, boost::system::system_category());
		fclose(f);
		return -1;
	}

	if (s > limit)
	{
		fclose(f);
		return -2;
	}

	r = fseek(f, 0, SEEK_SET);
	if (r != 0)
	{
		ec.assign(errno, boost::system::system_category());
		fclose(f);
		return -1;
	}

	v.resize(static_cast<std::size_t>(s));
	if (s == 0)
	{
		fclose(f);
		return 0;
	}

	r = int(fread(&v[0], 1, v.size(), f));
	if (r < 0)
	{
		ec.assign(errno, boost::system::system_category());
		fclose(f);
		return -1;
	}

	fclose(f);

	if (r != s) return -3;

	return 0;
}

bool print_alerts(lt::session& ses, char const* name
	, bool allow_no_torrents, bool allow_failed_fastresume
	, std::function<bool(lt::alert const*)> predicate, bool no_output)
{
	bool ret = false;
	std::vector<torrent_handle> handles = ses.get_torrents();
	TEST_CHECK(!handles.empty() || allow_no_torrents);
	torrent_handle h;
	if (!handles.empty()) h = handles[0];
	std::vector<alert*> alerts;
	ses.pop_alerts(&alerts);
	for (auto a : alerts)
	{
		if (predicate && predicate(a)) ret = true;
		if (peer_disconnected_alert const* p = alert_cast<peer_disconnected_alert>(a))
		{
			std::printf("%s: %s: [%s] (%s): %s\n", time_now_string(), name, a->what()
				, print_endpoint(p->endpoint).c_str(), p->message().c_str());
		}
		else if (should_print(a) && !no_output)
		{
			std::printf("%s: %s: [%s] %s\n", time_now_string(), name, a->what(), a->message().c_str());
		}

		TEST_CHECK(alert_cast<fastresume_rejected_alert>(a) == nullptr || allow_failed_fastresume);

		invalid_request_alert const* ira = alert_cast<invalid_request_alert>(a);
		if (ira)
		{
			std::printf("peer error: %s\n", ira->message().c_str());
			TEST_CHECK(false);
		}
	}
	return ret;
}

void wait_for_listen(lt::session& ses, char const* name)
{
	bool listen_done = false;
	alert const* a = nullptr;
	do
	{
		print_alerts(ses, name, true, true, [&listen_done](lt::alert const* al)
			{
				if (alert_cast<listen_failed_alert>(al)
					|| alert_cast<listen_succeeded_alert>(al))
				{
					listen_done = true;
				}
				return true;
			}, false);
		if (listen_done) break;
		a = ses.wait_for_alert(milliseconds(500));
	} while (a);
	// we din't receive a listen alert!
	TEST_CHECK(listen_done);
}

void wait_for_downloading(lt::session& ses, char const* name)
{
	time_point start = clock_type::now();
	bool downloading_done = false;
	alert const* a = nullptr;
	do
	{
		print_alerts(ses, name, true, true, [&downloading_done](lt::alert const* al)
			{
				state_changed_alert const* sc = alert_cast<state_changed_alert>(al);
				if (sc && sc->state == torrent_status::downloading)
					downloading_done = true;
				return true;
			}, false);
		if (downloading_done) break;
		if (total_seconds(clock_type::now() - start) > 10) break;
		a = ses.wait_for_alert(seconds(2));
	} while (a);
	if (!downloading_done)
	{
		std::printf("%s: did not receive a state_changed_alert indicating "
			"the torrent is downloading. waited: %d ms\n"
			, name, int(total_milliseconds(clock_type::now() - start)));
	}
}

void print_ses_rate(float const time
	, lt::torrent_status const* st1
	, lt::torrent_status const* st2
	, lt::torrent_status const* st3)
{
	if (st1)
	{
		std::printf("%3.1fs | %dkB/s %dkB/s %d%% %d cc:%d%s", static_cast<double>(time)
			, int(st1->download_payload_rate / 1000)
			, int(st1->upload_payload_rate / 1000)
			, int(st1->progress * 100)
			, st1->num_peers
			, st1->connect_candidates
			, st1->errc ? (" [" + st1->errc.message() + "]").c_str() : "");
	}
	if (st2)
		std::printf(" : %3.1fs | %dkB/s %dkB/s %d%% %d cc:%d%s", static_cast<double>(time)
			, int(st2->download_payload_rate / 1000)
			, int(st2->upload_payload_rate / 1000)
			, int(st2->progress * 100)
			, st2->num_peers
			, st2->connect_candidates
			, st2->errc ? (" [" + st1->errc.message() + "]").c_str() : "");
	if (st3)
		std::printf(" : %3.1fs | %dkB/s %dkB/s %d%% %d cc:%d%s", static_cast<double>(time)
			, int(st3->download_payload_rate / 1000)
			, int(st3->upload_payload_rate / 1000)
			, int(st3->progress * 100)
			, st3->num_peers
			, st3->connect_candidates
			, st3->errc ? (" [" + st1->errc.message() + "]").c_str() : "");

	std::printf("\n");
}

#ifdef _WIN32
typedef DWORD pid_type;
#else
typedef pid_t pid_type;
#endif

struct proxy_t
{
	pid_type pid;
	int type;
};

// maps port to proxy type
static std::map<int, proxy_t> running_proxies;

void stop_proxy(int port)
{
	std::printf("stopping proxy on port %d\n", port);
	// don't shut down proxies until the test is
	// completely done. This saves a lot of time.
	// they're closed at the end of main() by
	// calling stop_all_proxies().
}

namespace {

// returns 0 on failure, otherwise pid
pid_type async_run(char const* cmdline)
{
#ifdef _WIN32
	char buf[2048];
	std::snprintf(buf, sizeof(buf), "%s", cmdline);

	PROCESS_INFORMATION pi;
	STARTUPINFOA startup;
	memset(&startup, 0, sizeof(startup));
	startup.cb = sizeof(startup);
	startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
	startup.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
	startup.hStdError = GetStdHandle(STD_INPUT_HANDLE);
	int ret = CreateProcessA(NULL, buf, NULL, NULL, TRUE
		, CREATE_NEW_PROCESS_GROUP, NULL, NULL, &startup, &pi);

	if (ret == 0)
	{
		int error = GetLastError();
		std::printf("failed (%d) %s\n", error, error_code(error, system_category()).message().c_str());
		return 0;
	}
	return pi.dwProcessId;
#else
	pid_type p;
	char arg_storage[4096];
	char* argp = arg_storage;
	std::vector<char*> argv;
	argv.push_back(argp);
	for (char const* in = cmdline; *in != '\0'; ++in)
	{
		if (*in != ' ')
		{
			*argp++ = *in;
			continue;
		}
		*argp++ = '\0';
		argv.push_back(argp);
	}
	*argp = '\0';
	argv.push_back(nullptr);

	int ret = posix_spawnp(&p, argv[0], nullptr, nullptr, &argv[0], nullptr);
	if (ret != 0)
	{
		std::printf("failed (%d) %s\n", errno, strerror(errno));
		return 0;
	}
	return p;
#endif
}

void stop_process(pid_type p)
{
#ifdef _WIN32
	HANDLE proc = OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE, p);
	TerminateProcess(proc, 138);
	CloseHandle(proc);
#else
	std::printf("killing pid: %d\n", p);
	kill(p, SIGKILL);
#endif
}

} // anonymous namespace

void stop_all_proxies()
{
	std::map<int, proxy_t> proxies = running_proxies;
	for (std::map<int, proxy_t>::iterator i = proxies.begin()
		, end(proxies.end()); i != end; ++i)
	{
		stop_process(i->second.pid);
		running_proxies.erase(i->second.pid);
	}
}

// returns a port on success and -1 on failure
int start_proxy(int proxy_type)
{
	using namespace lt;

	std::map<int, proxy_t> :: iterator i = running_proxies.begin();
	for (; i != running_proxies.end(); ++i)
	{
		if (i->second.type == proxy_type) { return i->first; }
	}

	int port = 2000 + static_cast<int>(lt::random(6000));
	error_code ec;
	io_service ios;

	// make sure the port we pick is free
	do {
		++port;
		tcp::socket s(ios);
		s.open(tcp::v4(), ec);
		if (ec) break;
		s.bind(tcp::endpoint(address::from_string("127.0.0.1")
			, std::uint16_t(port)), ec);
	} while (ec);


	char const* type = "";
	char const* auth = "";
	char const* cmd = "";

	switch (proxy_type)
	{
		case settings_pack::socks4:
			type = "socks4";
			auth = " --allow-v4";
			cmd = "python ../socks.py";
			break;
		case settings_pack::socks5:
			type = "socks5";
			cmd = "python ../socks.py";
			break;
		case settings_pack::socks5_pw:
			type = "socks5";
			auth = " --username testuser --password testpass";
			cmd = "python ../socks.py";
			break;
		case settings_pack::http:
			type = "http";
			cmd = "python ../http.py";
			break;
		case settings_pack::http_pw:
			type = "http";
			auth = " --username testuser --password testpass";
			cmd = "python ../http.py";
			break;
	}
	char buf[512];
	std::snprintf(buf, sizeof(buf), "%s --port %d%s", cmd, port, auth);

	std::printf("%s starting proxy on port %d (%s %s)...\n", time_now_string(), port, type, auth);
	std::printf("%s\n", buf);
	pid_type r = async_run(buf);
	if (r == 0) abort();
	proxy_t t = { r, proxy_type };
	running_proxies.insert(std::make_pair(port, t));
	std::printf("%s launched\n", time_now_string());
	std::this_thread::sleep_for(lt::milliseconds(500));
	return port;
}

using namespace lt;

template <class T>
std::shared_ptr<T> clone_ptr(std::shared_ptr<T> const& ptr)
{
	return std::make_shared<T>(*ptr);
}

std::uint8_t random_byte()
{ return static_cast<std::uint8_t>(lt::random(0xff)); }

std::vector<char> generate_piece(piece_index_t const idx, int const piece_size)
{
	using namespace lt;
	std::vector<char> ret(static_cast<std::size_t>(piece_size));

	std::mt19937 rng(static_cast<std::uint32_t>(static_cast<int>(idx)));
	std::uniform_int_distribution<int> rand(-128, 127);
	for (char& c : ret)
	{
		c = static_cast<char>(rand(rng));
	}
	return ret;
}

lt::file_storage make_file_storage(const int file_sizes[], int num_files
	, int const piece_size, std::string base_name)
{
	using namespace lt;
	file_storage fs;
	for (int i = 0; i != num_files; ++i)
	{
		char filename[200];
		std::snprintf(filename, sizeof(filename), "test%d", i);
		char dirname[200];
		std::snprintf(dirname, sizeof(dirname), "%s%d", base_name.c_str()
			, i / 5);
		std::string full_path = combine_path(dirname, filename);

		fs.add_file(full_path, file_sizes[i]);
	}

	fs.set_piece_length(piece_size);
	fs.set_num_pieces(int((fs.total_size() + piece_size - 1) / piece_size));

	return fs;
}

std::shared_ptr<lt::torrent_info> make_torrent(const int file_sizes[]
	, int const num_files, int const piece_size)
{
	using namespace lt;
	file_storage fs = make_file_storage(file_sizes, num_files, piece_size);

	lt::create_torrent ct(fs, piece_size, 0x4000
		, lt::create_torrent::optimize_alignment);

	for (piece_index_t i(0); i < fs.end_piece(); ++i)
	{
		std::vector<char> piece = generate_piece(i, fs.piece_size(i));
		ct.set_hash(i, hasher(piece).final());
	}

	std::vector<char> buf;
	bencode(std::back_inserter(buf), ct.generate());
	return std::make_shared<torrent_info>(buf, from_span);
}

void create_random_files(std::string const& path, const int file_sizes[], int num_files)
{
	error_code ec;
	aux::vector<char> random_data(300000);
	for (int i = 0; i != num_files; ++i)
	{
		std::generate(random_data.begin(), random_data.end(), random_byte);
		char filename[200];
		std::snprintf(filename, sizeof(filename), "test%d", i);
		char dirname[200];
		std::snprintf(dirname, sizeof(dirname), "test_dir%d", i / 5);

		std::string full_path = combine_path(path, dirname);
		create_directory(full_path, ec);
		full_path = combine_path(full_path, filename);

		int to_write = file_sizes[i];
		file f(full_path, open_mode::write_only, ec);
		if (ec) std::printf("failed to create file \"%s\": (%d) %s\n"
			, full_path.c_str(), ec.value(), ec.message().c_str());
		std::int64_t offset = 0;
		while (to_write > 0)
		{
			int const s = std::min(to_write, static_cast<int>(random_data.size()));
			iovec_t b = { random_data.data(), size_t(s)};
			f.writev(offset, b, ec);
			if (ec) std::printf("failed to write file \"%s\": (%d) %s\n"
				, full_path.c_str(), ec.value(), ec.message().c_str());
			offset += s;
			to_write -= s;
		}
	}
}

std::shared_ptr<torrent_info> create_torrent(std::ostream* file
	, char const* name, int piece_size
	, int num_pieces, bool add_tracker, std::string ssl_certificate)
{
	// exercise the path when encountering invalid urls
	char const* invalid_tracker_url = "http:";
	char const* invalid_tracker_protocol = "foo://non/existent-name.com/announce";

	file_storage fs;
	int total_size = piece_size * num_pieces;
	fs.add_file(name, total_size);
	lt::create_torrent t(fs, piece_size);
	if (add_tracker)
	{
		t.add_tracker(invalid_tracker_url);
		t.add_tracker(invalid_tracker_protocol);
	}

	if (!ssl_certificate.empty())
	{
		std::vector<char> file_buf;
		error_code ec;
		int res = load_file(ssl_certificate, file_buf, ec);
		if (ec || res < 0)
		{
			std::printf("failed to load SSL certificate: %s\n", ec.message().c_str());
		}
		else
		{
			std::string pem;
			std::copy(file_buf.begin(), file_buf.end(), std::back_inserter(pem));
			t.set_root_cert(pem);
		}
	}

	aux::vector<char> piece(static_cast<std::size_t>(piece_size));
	for (int i = 0; i < int(piece.size()); ++i)
		piece[i] = (i % 26) + 'A';

	// calculate the hash for all pieces
	sha1_hash ph = hasher(piece).final();
	for (piece_index_t i(0); i < t.files().end_piece(); ++i)
		t.set_hash(i, ph);

	if (file)
	{
		while (total_size > 0)
		{
			file->write(&piece[0], std::min(piece.end_index(), total_size));
			total_size -= piece.end_index();
		}
	}

	std::vector<char> tmp;
	std::back_insert_iterator<std::vector<char>> out(tmp);

	entry tor = t.generate();

	bencode(out, tor);
	error_code ec;
	return std::make_shared<torrent_info>(tmp, ec, from_span);
}

std::tuple<torrent_handle, torrent_handle, torrent_handle>
setup_transfer(lt::session* ses1, lt::session* ses2, lt::session* ses3
	, bool clear_files, bool use_metadata_transfer, bool connect_peers
	, std::string suffix, int piece_size
	, std::shared_ptr<torrent_info>* torrent, bool super_seeding
	, add_torrent_params const* p, bool stop_lsd, bool use_ssl_ports
	, std::shared_ptr<torrent_info>* torrent2)
{
	TORRENT_ASSERT(ses1);
	TORRENT_ASSERT(ses2);

	if (stop_lsd)
	{
		settings_pack pack;
		pack.set_bool(settings_pack::enable_lsd, false);
		ses1->apply_settings(pack);
		ses2->apply_settings(pack);
		if (ses3) ses3->apply_settings(pack);
	}

	// This has the effect of applying the global
	// rule to all peers, regardless of if they're local or not
	ip_filter f;
	f.add_rule(address_v4::from_string("0.0.0.0")
		, address_v4::from_string("255.255.255.255")
		, 1 << static_cast<std::uint32_t>(lt::session::global_peer_class_id));
	ses1->set_peer_class_filter(f);
	ses2->set_peer_class_filter(f);
	if (ses3) ses3->set_peer_class_filter(f);

	settings_pack pack;
	pack.set_int(settings_pack::alert_mask
		, ~(alert::progress_notification | alert::stats_notification));
	if (ses3) pack.set_bool(settings_pack::allow_multiple_connections_per_ip, true);
	pack.set_int(settings_pack::mixed_mode_algorithm, settings_pack::prefer_tcp);
	pack.set_int(settings_pack::max_failcount, 1);
	peer_id pid;
	std::generate(&pid[0], &pid[0] + 20, random_byte);
	pack.set_str(settings_pack::peer_fingerprint, pid.to_string());
	ses1->apply_settings(pack);
	TORRENT_ASSERT(ses1->id() == pid);

	std::generate(&pid[0], &pid[0] + 20, random_byte);
	TORRENT_ASSERT(ses1->id() != pid);
	pack.set_str(settings_pack::peer_fingerprint, pid.to_string());
	ses2->apply_settings(pack);
	TORRENT_ASSERT(ses2->id() == pid);
	if (ses3)
	{
		std::generate(&pid[0], &pid[0] + 20, random_byte);
		TORRENT_ASSERT(ses1->id() != pid);
		TORRENT_ASSERT(ses2->id() != pid);
		pack.set_str(settings_pack::peer_fingerprint, pid.to_string());
		ses3->apply_settings(pack);
		TORRENT_ASSERT(ses3->id() == pid);
	}

	TORRENT_ASSERT(ses1->id() != ses2->id());
	if (ses3) TORRENT_ASSERT(ses3->id() != ses2->id());

	std::shared_ptr<torrent_info> t;
	if (torrent == nullptr)
	{
		error_code ec;
		create_directory("tmp1" + suffix, ec);
		std::ofstream file(combine_path("tmp1" + suffix, "temporary").c_str());
		t = ::create_torrent(&file, "temporary", piece_size, 9, false);
		file.close();
		if (clear_files)
		{
			remove_all(combine_path("tmp2" + suffix, "temporary"), ec);
			remove_all(combine_path("tmp3" + suffix, "temporary"), ec);
		}
		std::printf("generated torrent: %s tmp1%s/temporary\n", aux::to_hex(t->info_hash()).c_str(), suffix.c_str());
	}
	else
	{
		t = *torrent;
	}

	// they should not use the same save dir, because the
	// file pool will complain if two torrents are trying to
	// use the same files
	add_torrent_params param;
	param.flags &= ~torrent_flags::paused;
	param.flags &= ~torrent_flags::auto_managed;
	if (p) param = *p;
	param.ti = clone_ptr(t);
	param.save_path = "tmp1" + suffix;
	param.flags |= torrent_flags::seed_mode;
	error_code ec;
	torrent_handle tor1 = ses1->add_torrent(param, ec);
	if (ec)
	{
		std::printf("ses1.add_torrent: %s\n", ec.message().c_str());
		return std::make_tuple(torrent_handle(), torrent_handle(), torrent_handle());
	}
	if (super_seeding)
	{
		tor1.set_flags(torrent_flags::super_seeding);
	}

	// the downloader cannot use seed_mode
	param.flags &= ~torrent_flags::seed_mode;

	TEST_CHECK(!ses1->get_torrents().empty());

	torrent_handle tor2;
	torrent_handle tor3;

	if (ses3)
	{
		param.ti = clone_ptr(t);
		param.save_path = "tmp3" + suffix;
		tor3 = ses3->add_torrent(param, ec);
		TEST_CHECK(!ses3->get_torrents().empty());
	}

	if (use_metadata_transfer)
	{
		param.ti.reset();
		param.info_hash = t->info_hash();
	}
	else if (torrent2)
	{
		param.ti = clone_ptr(*torrent2);
	}
	else
	{
		param.ti = clone_ptr(t);
	}
	param.save_path = "tmp2" + suffix;

	tor2 = ses2->add_torrent(param, ec);
	TEST_CHECK(!ses2->get_torrents().empty());

	TORRENT_ASSERT(ses1->get_torrents().size() == 1);
	TORRENT_ASSERT(ses2->get_torrents().size() == 1);

//	std::this_thread::sleep_for(lt::milliseconds(100));

	if (connect_peers)
	{
		wait_for_downloading(*ses2, "ses2");

		int port = 0;
		if (use_ssl_ports)
		{
			port = ses2->ssl_listen_port();
			std::printf("%s: ses2->ssl_listen_port(): %d\n", time_now_string(), port);
		}

		if (port == 0)
		{
			port = ses2->listen_port();
			std::printf("%s: ses2->listen_port(): %d\n", time_now_string(), port);
		}

		std::printf("%s: ses1: connecting peer port: %d\n"
			, time_now_string(), port);
		tor1.connect_peer(tcp::endpoint(address::from_string("127.0.0.1", ec)
			, std::uint16_t(port)));

		if (ses3)
		{
			// give the other peers some time to get an initial
			// set of pieces before they start sharing with each-other

			wait_for_downloading(*ses3, "ses3");

			port = 0;
			int port2 = 0;
			if (use_ssl_ports)
			{
				port = ses2->ssl_listen_port();
				port2 = ses1->ssl_listen_port();
			}

			if (port == 0) port = ses2->listen_port();
			if (port2 == 0) port2 = ses1->listen_port();

			std::printf("ses3: connecting peer port: %d\n", port);
			tor3.connect_peer(tcp::endpoint(
					address::from_string("127.0.0.1", ec), std::uint16_t(port)));
			std::printf("ses3: connecting peer port: %d\n", port2);
				tor3.connect_peer(tcp::endpoint(
					address::from_string("127.0.0.1", ec)
					, std::uint16_t(port2)));
		}
	}

	return std::make_tuple(tor1, tor2, tor3);
}

namespace {
pid_type web_server_pid = 0;
}

int start_web_server(bool ssl, bool chunked_encoding, bool keepalive, int min_interval)
{
	int port = 2000 + static_cast<int>(lt::random(6000));
	error_code ec;
	io_service ios;

	// make sure the port we pick is free
	do {
		++port;
		tcp::socket s(ios);
		s.open(tcp::v4(), ec);
		if (ec) break;
		s.bind(tcp::endpoint(address::from_string("127.0.0.1")
			, std::uint16_t(port)), ec);
	} while (ec);

	char buf[200];
	std::snprintf(buf, sizeof(buf), "python ../web_server.py %d %d %d %d %d"
		, port, chunked_encoding, ssl, keepalive, min_interval);

	std::printf("%s starting web_server on port %d...\n", time_now_string(), port);

	std::printf("%s\n", buf);
	pid_type r = async_run(buf);
	if (r == 0) abort();
	web_server_pid = r;
	std::printf("%s launched\n", time_now_string());
	std::this_thread::sleep_for(lt::milliseconds(500));
	return port;
}

void stop_web_server()
{
	if (web_server_pid == 0) return;
	std::printf("stopping web server\n");
	stop_process(web_server_pid);
	web_server_pid = 0;
}

tcp::endpoint ep(char const* ip, int port)
{
	error_code ec;
	tcp::endpoint ret(address::from_string(ip, ec), std::uint16_t(port));
	TEST_CHECK(!ec);
	return ret;
}

udp::endpoint uep(char const* ip, int port)
{
	error_code ec;
	udp::endpoint ret(address::from_string(ip, ec), std::uint16_t(port));
	TEST_CHECK(!ec);
	return ret;
}

lt::address addr(char const* ip)
{
	lt::error_code ec;
	auto ret = lt::address::from_string(ip, ec);
	TEST_CHECK(!ec);
	return ret;
}

lt::address_v4 addr4(char const* ip)
{
	lt::error_code ec;
	auto ret = lt::address_v4::from_string(ip, ec);
	TEST_CHECK(!ec);
	return ret;
}

#if TORRENT_USE_IPV6
lt::address_v6 addr6(char const* ip)
{
	lt::error_code ec;
	auto ret = lt::address_v6::from_string(ip, ec);
	TEST_CHECK(!ec);
	return ret;
}
#endif

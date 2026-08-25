#undef NDEBUG
#include"Boss/Mod/Initiator.hpp"
#include"Boss/Msg/CommandFail.hpp"
#include"Boss/Msg/CommandRequest.hpp"
#include"Boss/Msg/CommandResponse.hpp"
#include"Boss/Msg/JsonCout.hpp"
#include"Boss/Msg/Manifestation.hpp"
#include"Boss/Shutdown.hpp"
#include"Ev/Io.hpp"
#include"Ev/ThreadPool.hpp"
#include"Ev/concurrent.hpp"
#include"Ev/start.hpp"
#include"Ev/yield.hpp"
#include"Jsmn/Object.hpp"
#include"Jsmn/Parser.hpp"
#include"Json/Out.hpp"
#include"Net/Fd.hpp"
#include"S/Bus.hpp"
#include<assert.h>
#include<deque>
#include<dirent.h>
#include<errno.h>
#include<fcntl.h>
#include<limits.h>
#include<signal.h>
#include<stdint.h>
#include<stdio.h>
#include<string>
#include<sys/resource.h>
#include<sys/socket.h>
#include<sys/stat.h>
#include<sys/types.h>
#include<sys/wait.h>
#include<unistd.h>
#include<vector>

/* The CLN version gate in Initiator, exercised through `init`
 * against a mock lightningd RPC: v26.04 and newer pass silently,
 * v25.09 up to v26.04 warn and proceed, an unparseable version
 * warns and proceeds, clboss-skip-cln-version-check turns a refusal
 * into an Info line, and anything older than v25.09 refuses to
 * start -- an Error line, then abort(), with no data.clboss or
 * keys.clboss left behind.  The refusal is observed from a forked
 * child.  */

namespace {

std::string const self_id
= "020000000000000000000000000000000000000000000000000000000000000000";

Jsmn::Object parse_json(std::string const& text) {
	auto parser = Jsmn::Parser();
	auto objs = parser.feed(text);
	assert(objs.size() == 1);
	return std::move(objs[0]);
}

bool contains(std::string const& s, char const* needle) {
	return s.find(needle) != std::string::npos;
}

bool file_exists(std::string const& path) {
	struct stat st;
	return stat(path.c_str(), &st) == 0;
}

std::string make_temp_dir() {
	auto tmpl = std::string("/tmp/clboss-clnversion-test-XXXXXX");
	auto modifiable = std::vector<char>(tmpl.begin(), tmpl.end());
	modifiable.push_back('\0');
	auto t = mkdtemp(modifiable.data());
	assert(t != nullptr);
	return std::string(t);
}

/* Remove whatever a case left in its directory, then the
 * directory.  */
void remove_temp_dir(std::string const& dir) {
	auto d = opendir(dir.c_str());
	assert(d != nullptr);
	auto names = std::vector<std::string>();
	while (auto e = readdir(d)) {
		auto n = std::string(e->d_name);
		if (n == "." || n == "..")
			continue;
		names.push_back(n);
	}
	closedir(d);
	for (auto const& n : names) {
		auto res = unlink((dir + "/" + n).c_str());
		assert(res == 0);
	}
	auto res = rmdir(dir.c_str());
	assert(res == 0);
}

class CwdGuard {
private:
	std::string old_cwd;
public:
	explicit
	CwdGuard(std::string const& dir) {
		auto oldbuf = std::vector<char>(PATH_MAX, '\0');
		auto p = getcwd(oldbuf.data(), oldbuf.size());
		assert(p != nullptr);
		old_cwd = std::string(p);
		auto res = chdir(dir.c_str());
		assert(res == 0);
	}
	~CwdGuard() {
		auto res = chdir(old_cwd.c_str());
		assert(res == 0);
	}
	CwdGuard(CwdGuard&&) =delete;
	CwdGuard(CwdGuard const&) =delete;
};

/* Answers the two RPC calls Initiator makes before it responds to
 * `init`: getinfo (the version under test) and listconfigs.  */
class RpcServerMock {
private:
	Net::Fd fd;
	Jsmn::Parser parser;
	std::deque<Jsmn::Object> requests;

	Ev::Io<Jsmn::Object> read_request(std::size_t retries = 0) {
		return Ev::yield().then([this]() {
			if (!requests.empty()) {
				auto req = std::move(requests.front());
				requests.pop_front();
				return Ev::lift(std::move(req));
			}
			return Ev::lift(Jsmn::Object());
		}).then([this, retries](Jsmn::Object req) {
			if (!req.is_null())
				return Ev::lift(std::move(req));
			assert(retries < 100000);

			char buf[512];
			auto rd = ssize_t();
			do {
				rd = read(fd.get(), buf, sizeof(buf));
			} while (rd < 0 && errno == EINTR);

			if (rd < 0 && (errno == EWOULDBLOCK || errno == EAGAIN))
				return read_request(retries + 1);
			assert(rd > 0);

			auto parsed = parser.feed(std::string(buf, std::size_t(rd)));
			for (auto& p : parsed)
				requests.push_back(std::move(p));

			return read_request(retries + 1);
		});
	}

	Ev::Io<void> write_all(std::string data) {
		return Ev::yield().then([this, data]() {
			auto wr = ssize_t();
			do {
				wr = write(fd.get(), data.c_str(), data.size());
			} while (wr < 0 && errno == EINTR);

			if (wr < 0 && (errno == EWOULDBLOCK || errno == EAGAIN))
				return write_all(data);
			assert(wr >= 0);
			if (std::size_t(wr) < data.size())
				return write_all(data.substr(std::size_t(wr)));

			return Ev::lift();
		});
	}

	static std::string extract_id_and_check_method( Jsmn::Object const& req
						      , std::string const& method
						      ) {
		assert(req.is_object());
		assert(req.has("id"));
		assert(req["id"].is_number());
		assert(req.has("method"));
		assert(req["method"].is_string());
		assert(std::string(req["method"]) == method);
		return req["id"].direct_text();
	}

	Ev::Io<void> reply_result(std::string const& id, std::string const& result) {
		auto response = std::string();
		response += R"({"jsonrpc":"2.0","id":)";
		response += id;
		response += R"(,"result":)";
		response += result;
		response += "}\n\n";
		return write_all(std::move(response));
	}

public:
	explicit
	RpcServerMock(Net::Fd fd_) : fd(std::move(fd_)), parser(), requests() {
		auto flags = fcntl(fd.get(), F_GETFL);
		assert(flags >= 0);
		flags |= O_NONBLOCK;
		auto res = fcntl(fd.get(), F_SETFL, flags);
		assert(res == 0);
	}
	RpcServerMock(RpcServerMock&&) =delete;

	Ev::Io<void> run(std::string getinfo_result) {
		return read_request().then([this, getinfo_result](Jsmn::Object req) {
			auto id = extract_id_and_check_method(req, "getinfo");
			return reply_result(id, getinfo_result);
		}).then([this]() {
			return read_request();
		}).then([this](Jsmn::Object req) {
			auto id = extract_id_and_check_method(req, "listconfigs");
			return reply_result(id, R"({"configs": {}})");
		});
	}
};

struct LogLine {
	std::string level;
	std::string message;
};

struct Outcome {
	bool init_ok;
	std::vector<LogLine> logs;
};

/* getinfo result for a version string; no "version" key when
 * version is null.  */
std::string getinfo_result(char const* version) {
	auto rv = std::string(R"({"id":")") + self_id + "\"";
	if (version)
		rv += std::string(R"(,"version":")") + version + "\"";
	rv += "}";
	return rv;
}

/* Run `init` against the mock in `dir`.  Every log line is
 * collected; with `error_sink` set, Error lines are also written
 * there at once, for the refusal case whose process never returns
 * from here.  */
Outcome run_case( std::string const& dir
		, char const* version
		, bool skip_flag
		, FILE* error_sink = nullptr
		) {
	auto cwd = CwdGuard(dir);

	int socks[2];
	auto res = socketpair(AF_UNIX, SOCK_STREAM, 0, socks);
	assert(res == 0);
	auto server_socket = Net::Fd(socks[0]);
	auto client_socket = Net::Fd(socks[1]);

	auto bus = S::Bus();
	auto threadpool = Ev::ThreadPool();
	auto server = RpcServerMock(std::move(server_socket));

	auto client_socket_holder = std::make_shared<Net::Fd>(std::move(client_socket));
	auto initiator = Boss::Mod::Initiator(
		bus, threadpool,
		[client_socket_holder]( std::string const& lightning_dir
				      , std::string const& rpc_file
				      ) {
			assert(lightning_dir == ".");
			assert(rpc_file == "lightning-rpc");
			auto fd = Net::Fd();
			std::swap(fd, *client_socket_holder);
			assert(fd);
			return fd;
		}
	);

	auto got = Outcome{false, {}};
	auto received_fail = false;

	bus.subscribe<Boss::Msg::JsonCout>([&](Boss::Msg::JsonCout const& m) {
		auto js = parse_json(m.obj.output());
		if ( !js.is_object() || !js.has("method")
		  || std::string(js["method"]) != "log"
		   )
			return Ev::lift();
		auto params = js["params"];
		auto line = LogLine{
			std::string(params["level"]),
			std::string(params["message"])
		};
		if (error_sink && line.level == "error") {
			fprintf(error_sink, "%s\n", line.message.c_str());
			fflush(error_sink);
		}
		got.logs.push_back(std::move(line));
		return Ev::lift();
	});
	bus.subscribe<Boss::Msg::CommandResponse>([&](Boss::Msg::CommandResponse const& m) {
		m.id.cmatch([&](std::uint64_t id) {
			assert(id == 42);
		}, [&](std::string const&) {
			assert(false);
		});
		got.init_ok = true;
		return Ev::lift();
	});
	bus.subscribe<Boss::Msg::CommandFail>([&](Boss::Msg::CommandFail const&) {
		received_fail = true;
		return Ev::lift();
	});

	auto params_text = std::string(R"JSON(
	{
	  "configuration": {
	    "network": "regtest",
	    "lightning-dir": ".",
	    "rpc-file": "lightning-rpc"
	  })JSON");
	if (skip_flag)
		params_text += R"JSON(,
	  "options": { "clboss-skip-cln-version-check": true })JSON";
	params_text += "\n\t}\n";

	auto req = Boss::Msg::CommandRequest{
		"init",
		parse_json(params_text),
		Ln::CommandId::left(std::uint64_t(42))
	};

	auto server_code = server.run(getinfo_result(version));
	/* Manifestation first: Initiator learns its own option names
	 * there, and init ignores options it has not manifested.  */
	auto client_code = Ev::lift().then([&]() {
		return bus.raise(Boss::Msg::Manifestation{});
	}).then([&]() {
		return bus.raise(req);
	}).then([&]() {
		assert(!received_fail);
		return bus.raise(Boss::Shutdown{});
	}).then([&]() {
		return Ev::lift(0);
	});

	auto code = Ev::lift().then([&]() {
		return Ev::concurrent(std::move(server_code));
	}).then([&]() {
		return client_code;
	});

	auto ec = Ev::start(code);
	assert(ec == 0);
	return got;
}

Outcome run_case_in_temp_dir(char const* version, bool skip_flag) {
	auto dir = make_temp_dir();
	auto got = run_case(dir, version, skip_flag);
	remove_temp_dir(dir);
	return got;
}

std::size_t count_level(Outcome const& o, char const* level) {
	auto n = std::size_t(0);
	for (auto const& l : o.logs)
		if (l.level == level)
			++n;
	return n;
}
/* The one line at `level` whose message contains `needle`; asserts
 * exactly one such line.  */
std::string the_line(Outcome const& o, char const* level, char const* needle) {
	auto found = std::vector<std::string>();
	for (auto const& l : o.logs)
		if (l.level == level && contains(l.message, needle))
			found.push_back(l.message);
	assert(found.size() == 1);
	return found[0];
}

/* A refused version: the child process must die by abort() after
 * logging the refusal, having created no on-disk state.  */
void check_refused(char const* version) {
	auto dir = make_temp_dir();
	auto sink_path = dir + "/refusal.log";

	auto pid = fork();
	assert(pid >= 0);
	if (pid == 0) {
		/* abort() must not leave a core file in the
		 * directory the parent is about to inspect.  */
		struct rlimit no_core = {0, 0};
		setrlimit(RLIMIT_CORE, &no_core);
		auto sink = fopen(sink_path.c_str(), "w");
		assert(sink != nullptr);
		run_case(dir, version, false, sink);
		/* Not reached when the gate refuses.  */
		_exit(0);
	}

	int status = 0;
	auto res = waitpid(pid, &status, 0);
	assert(res == pid);
	assert(WIFSIGNALED(status));
	assert(WTERMSIG(status) == SIGABRT);

	auto sink = fopen(sink_path.c_str(), "r");
	assert(sink != nullptr);
	auto text = std::string();
	char buf[4096];
	while (auto rd = fread(buf, 1, sizeof(buf), sink))
		text += std::string(buf, rd);
	fclose(sink);
	assert(contains(text, "Initiator: CLN "));
	assert(contains(text, version));
	assert(contains(text, "older than v25.09"));
	assert(contains(text, "Refusing to start"));
	assert(contains(text, "clboss-skip-cln-version-check"));

	assert(!file_exists(dir + "/data.clboss"));
	assert(!file_exists(dir + "/keys.clboss"));
	remove_temp_dir(dir);
}

}

int main() {
	/* Refusals first: fork() wants no threads alive, and the
	 * cases below each start and join a thread pool.  */
	check_refused("v24.11");
	check_refused("v25.08");
	check_refused("v0.12.1");

	/* v26.04 and newer: silent.  Patch levels, git-describe
	 * suffixes, and rc tags are ignored by the parse.  */
	for (auto v : { "v26.04", "v26.04.1-16-g01bb5aa", "v26.06rc1"
		      , "v27.01" }) {
		auto o = run_case_in_temp_dir(v, false);
		assert(o.init_ok);
		assert(count_level(o, "warn") == 0);
		assert(count_level(o, "error") == 0);
		for (auto const& l : o.logs)
			assert(!contains(l.message, "skip-cln-version-check set"));
	}

	/* v25.09 up to v26.04: one Warn, startup continues.  */
	for (auto v : { "v25.09", "v25.12", "v26.03" }) {
		auto o = run_case_in_temp_dir(v, false);
		assert(o.init_ok);
		assert(count_level(o, "error") == 0);
		assert(count_level(o, "warn") == 1);
		auto line = the_line(o, "warn", "older than v26.04");
		assert(contains(line, v));
		assert(contains(line, "proceeding"));
	}

	/* Unparseable: one Warn naming the string, startup continues.
	 * The parse wants "v<major>.<minor>".  */
	for (auto v : { "26.04", "v26", "dev-build", "" }) {
		auto o = run_case_in_temp_dir(v, false);
		assert(o.init_ok);
		assert(count_level(o, "error") == 0);
		assert(count_level(o, "warn") == 1);
		auto line = the_line(o, "warn", "unrecognized CLN version");
		assert(contains(line, (std::string("\"") + v + "\"").c_str()));
		assert(contains(line, "proceeding"));
	}
	/* No version field at all reads as the empty string.  */
	{
		auto o = run_case_in_temp_dir(nullptr, false);
		assert(o.init_ok);
		assert(count_level(o, "warn") == 1);
		auto line = the_line(o, "warn", "unrecognized CLN version");
		assert(contains(line, "\"\""));
	}

	/* The skip flag: a version the gate would refuse starts, with
	 * an Info line saying the check is off; nothing at Warn.  */
	{
		auto o = run_case_in_temp_dir("v24.11", true);
		assert(o.init_ok);
		assert(count_level(o, "warn") == 0);
		assert(count_level(o, "error") == 0);
		auto line = the_line(o, "info", "clboss-skip-cln-version-check set");
		assert(contains(line, "not enforcing"));
		assert(contains(line, "v24.11"));
	}
	/* The flag does not add noise when the version passes.  */
	{
		auto o = run_case_in_temp_dir("v26.04", true);
		assert(o.init_ok);
		assert(count_level(o, "warn") == 0);
		the_line(o, "info", "clboss-skip-cln-version-check set");
	}

	return 0;
}

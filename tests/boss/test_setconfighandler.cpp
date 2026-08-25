#undef NDEBUG

#include"Boss/Mod/SetConfigHandler.hpp"
#include"Boss/Msg/CommandFail.hpp"
#include"Boss/Msg/CommandRequest.hpp"
#include"Boss/Msg/CommandResponse.hpp"
#include"Boss/Msg/ManifestOption.hpp"
#include"Boss/Msg/Option.hpp"
#include"Boss/Msg/OptionType.hpp"
#include"Ev/Io.hpp"
#include"Ev/start.hpp"
#include"Jsmn/Object.hpp"
#include"Json/Out.hpp"
#include"Ln/CommandId.hpp"
#include"S/Bus.hpp"

#include<assert.h>
#include<cstdint>
#include<memory>
#include<string>
#include<vector>

/* SetConfigHandler turns lightningd's `setconfig` method call for a
 * dynamic option into a Msg::Option on the bus and answers the call
 * from the owner's verdict: an untouched reject_reason acknowledges
 * (lightningd persists the value only then), a filled one fails the
 * call with the invalid-params code so nothing is persisted.  */

namespace {

/* What the handler put on the bus for one setconfig call.  */
struct Seen {
	/* Msg::Option deliveries, in order.  */
	std::vector<Boss::Msg::Option> options;
	std::vector<Boss::Msg::CommandResponse> responses;
	std::vector<Boss::Msg::CommandFail> fails;
	void clear() {
		options.clear();
		responses.clear();
		fails.clear();
	}
};

Boss::Msg::CommandRequest setconfig( char const* params_json
				   , Ln::CommandId id
				   ) {
	return Boss::Msg::CommandRequest{
		"setconfig",
		Jsmn::Object::parse_json(params_json),
		std::move(id)
	};
}

Ln::CommandId num_id(std::uint64_t n) {
	return Ln::CommandId::left(n);
}

bool id_is(Ln::CommandId const& id, std::uint64_t n) {
	auto rv = false;
	id.cmatch([&](std::uint64_t const& l) {
		rv = (l == n);
	}, [&](std::string const&) {
		rv = false;
	});
	return rv;
}
bool id_is(Ln::CommandId const& id, std::string const& s) {
	auto rv = false;
	id.cmatch([&](std::uint64_t const&) {
		rv = false;
	}, [&](std::string const& r) {
		rv = (r == s);
	});
	return rv;
}

}

int main() {
	auto bus = S::Bus();
	auto handler = Boss::Mod::SetConfigHandler(bus);
	auto seen = std::make_shared<Seen>();

	/* Stand in for the option owners.  clboss-veto rejects the
	 * value "bogus" the way RebalanceModeManager rejects an
	 * unrecognized mode; every other value is accepted.  */
	bus.subscribe<Boss::Msg::Option
		     >([seen](Boss::Msg::Option const& o) {
		seen->options.push_back(o);
		if ( o.name == "clboss-veto"
		  && o.value.is_string()
		  && std::string(o.value) == "bogus"
		   )
			o.reject("clboss-veto: unrecognized value 'bogus'");
		return Ev::lift();
	});
	bus.subscribe<Boss::Msg::CommandResponse
		     >([seen](Boss::Msg::CommandResponse const& m) {
		seen->responses.push_back(m);
		return Ev::lift();
	});
	bus.subscribe<Boss::Msg::CommandFail
		     >([seen](Boss::Msg::CommandFail const& m) {
		seen->fails.push_back(m);
		return Ev::lift();
	});

	auto manifest = [&bus]( char const* name
			      , Boss::Msg::OptionType type
			      , bool dynamic
			      ) {
		return bus.raise(Boss::Msg::ManifestOption{
			name, type, Json::Out::direct(std::string("")),
			"test option", dynamic
		});
	};

	auto code = Ev::lift().then([&]() {
		return manifest("clboss-dyn", Boss::Msg::OptionType_String, true)
		     + manifest("clboss-veto", Boss::Msg::OptionType_String, true)
		     + manifest("clboss-num", Boss::Msg::OptionType_Int, true)
		     + manifest("clboss-static", Boss::Msg::OptionType_Int, false)
		     ;

	/* Accepted value: the owner sees it, the call is acknowledged
	 * with an empty object and the request id.  */
	}).then([&]() {
		seen->clear();
		return bus.raise(setconfig(
			R"({"config": "clboss-dyn", "val": "abc"})",
			num_id(1)
		));
	}).then([&]() {
		assert(seen->options.size() == 1);
		assert(seen->options[0].name == "clboss-dyn");
		assert(seen->options[0].value.is_string());
		assert(std::string(seen->options[0].value) == "abc");
		/* The setconfig path allocates the back-channel; the
		 * init-time path leaves it null.  */
		assert(seen->options[0].reject_reason);
		assert(seen->options[0].reject_reason->empty());
		assert(seen->fails.empty());
		assert(seen->responses.size() == 1);
		assert(id_is(seen->responses[0].id, 1));
		assert(seen->responses[0].response.output() == "{}");

	/* Rejected value: the owner's reason becomes the error
	 * message, code -32602, and nothing acknowledges the call.  */
		seen->clear();
		return bus.raise(setconfig(
			R"({"config": "clboss-veto", "val": "bogus"})",
			num_id(2)
		));
	}).then([&]() {
		assert(seen->options.size() == 1);
		assert(seen->options[0].name == "clboss-veto");
		assert(seen->responses.empty());
		assert(seen->fails.size() == 1);
		assert(id_is(seen->fails[0].id, 2));
		assert(seen->fails[0].code == -32602);
		assert( seen->fails[0].message
		     == "setconfig: clboss-veto: unrecognized value 'bogus'"
		      );

	/* The same option accepts another value.  */
		seen->clear();
		return bus.raise(setconfig(
			R"({"config": "clboss-veto", "val": "fine"})",
			num_id(3)
		));
	}).then([&]() {
		assert(seen->options.size() == 1);
		assert(seen->fails.empty());
		assert(seen->responses.size() == 1);
		assert(id_is(seen->responses[0].id, 3));

	/* Lightningd encodes every setconfig value as a JSON string,
	 * numeric options included; the handler forwards it as-is
	 * and the owner parses.  A string request id round-trips.  */
		seen->clear();
		return bus.raise(setconfig(
			R"({"config": "clboss-num", "val": "42"})",
			Ln::CommandId::right(std::string("req-42"))
		));
	}).then([&]() {
		assert(seen->options.size() == 1);
		assert(seen->options[0].name == "clboss-num");
		assert(seen->options[0].value.is_string());
		assert(std::string(seen->options[0].value) == "42");
		assert(seen->fails.empty());
		assert(seen->responses.size() == 1);
		assert(id_is(seen->responses[0].id, std::string("req-42")));

	/* An option registered without dynamic = true is refused
	 * before any owner hears about it.  */
		seen->clear();
		return bus.raise(setconfig(
			R"({"config": "clboss-static", "val": "7"})",
			num_id(4)
		));
	}).then([&]() {
		assert(seen->options.empty());
		assert(seen->responses.empty());
		assert(seen->fails.size() == 1);
		assert(id_is(seen->fails[0].id, 4));
		assert(seen->fails[0].code == -32602);
		assert( seen->fails[0].message
		     == "setconfig: option 'clboss-static' is not dynamic"
		      );

	/* An option we never registered.  */
		seen->clear();
		return bus.raise(setconfig(
			R"({"config": "clboss-nope", "val": "x"})",
			num_id(5)
		));
	}).then([&]() {
		assert(seen->options.empty());
		assert(seen->responses.empty());
		assert(seen->fails.size() == 1);
		assert(seen->fails[0].code == -32602);
		assert( seen->fails[0].message
		     == "setconfig: unknown option 'clboss-nope'"
		      );

	/* Malformed: no config name.  */
		seen->clear();
		return bus.raise(setconfig(
			R"({"val": "x"})",
			num_id(6)
		));
	}).then([&]() {
		assert(seen->options.empty());
		assert(seen->responses.empty());
		assert(seen->fails.size() == 1);
		assert(seen->fails[0].code == -32602);
		assert( seen->fails[0].message
		     == "setconfig: missing or non-string 'config' parameter"
		      );

	/* No val at all: forwarded as a null value, acknowledged.  */
		seen->clear();
		return bus.raise(setconfig(
			R"({"config": "clboss-dyn"})",
			num_id(7)
		));
	}).then([&]() {
		assert(seen->options.size() == 1);
		assert(seen->options[0].value.is_null());
		assert(seen->fails.empty());
		assert(seen->responses.size() == 1);

	/* Other methods are not ours.  */
		seen->clear();
		return bus.raise(Boss::Msg::CommandRequest{
			"getinfo",
			Jsmn::Object::parse_json(R"({"config": "clboss-dyn"})"),
			num_id(8)
		});
	}).then([&]() {
		assert(seen->options.empty());
		assert(seen->responses.empty());
		assert(seen->fails.empty());
		return Ev::lift(0);
	});

	return Ev::start(std::move(code));
}

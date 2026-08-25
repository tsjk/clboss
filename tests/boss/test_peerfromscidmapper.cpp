#undef NDEBUG
#include"Boss/Mod/ConstructedListpeers.hpp"
#include"Boss/Mod/PeerFromScidMapper.hpp"
#include"Boss/Msg/ListpeersResult.hpp"
#include"Boss/Msg/ManifestNotification.hpp"
#include"Boss/Msg/Manifestation.hpp"
#include"Boss/Msg/Notification.hpp"
#include"Boss/Msg/RequestPeerFromScid.hpp"
#include"Boss/Msg/ResponsePeerFromScid.hpp"
#include"Ev/Io.hpp"
#include"Ev/start.hpp"
#include"Ev/yield.hpp"
#include"Jsmn/Object.hpp"
#include"Ln/NodeId.hpp"
#include"Ln/Scid.hpp"
#include"S/Bus.hpp"
#include"Util/make_unique.hpp"
#include<assert.h>
#include<memory>

namespace {

auto const peer_a = Ln::NodeId("020000000000000000000000000000000000000000000000000000000000000000");
auto const peer_b = Ln::NodeId("020000000000000000000000000000000000000000000000000000000000000001");
auto const peer_c = Ln::NodeId("020000000000000000000000000000000000000000000000000000000000000002");

auto const listpeers_result = R"JSON(
{ "peers": [ { "id": "020000000000000000000000000000000000000000000000000000000000000000"
             , "channels": [ { "state": "CHANNELD_NORMAL"
                             , "short_channel_id": "1000x1x0"
                             }
                           ]
             }
           , { "id": "020000000000000000000000000000000000000000000000000000000000000001"
             , "channels": [ { "state": "CHANNELD_NORMAL"
                             , "short_channel_id": "1000x1x1"
                             }
                           ]
             }
           ]
}
)JSON";

/* The snapshot after peer_c's channel locked in.  */
auto const listpeers_result_later = R"JSON(
{ "peers": [ { "id": "020000000000000000000000000000000000000000000000000000000000000000"
             , "channels": [ { "state": "CHANNELD_NORMAL"
                             , "short_channel_id": "1000x1x0"
                             }
                           ]
             }
           , { "id": "020000000000000000000000000000000000000000000000000000000000000002"
             , "channels": [ { "state": "CHANNELD_NORMAL"
                             , "short_channel_id": "3000x1x0"
                             }
                           ]
             }
           ]
}
)JSON";

/* Lock-in of a channel with peer_c, the shape lightningd sends.  */
auto const locked_in = R"JSON(
{ "channel_state_changed":
  { "peer_id": "020000000000000000000000000000000000000000000000000000000000000002"
  , "channel_id": "0000000000000000000000000000000000000000000000000000000000000000"
  , "short_channel_id": "3000x1x0"
  , "timestamp": "2026-08-25T00:00:00.000Z"
  , "old_state": "CHANNELD_AWAITING_LOCKIN"
  , "new_state": "CHANNELD_NORMAL"
  , "cause": "user"
  , "message": "Lockin complete"
  }
}
)JSON";

/* Before the funding confirms the field is null.  */
auto const not_confirmed = R"JSON(
{ "channel_state_changed":
  { "peer_id": "020000000000000000000000000000000000000000000000000000000000000002"
  , "channel_id": "0000000000000000000000000000000000000000000000000000000000000000"
  , "short_channel_id": null
  , "timestamp": "2026-08-25T00:00:00.000Z"
  , "new_state": "CHANNELD_AWAITING_LOCKIN"
  , "cause": "user"
  }
}
)JSON";

Boss::Msg::ListpeersResult snapshot(char const* json) {
	return Boss::Msg::ListpeersResult{
		Boss::Mod::convert_legacy_listpeers(
			Jsmn::Object::parse_json(json)["peers"]
		),
		false
	};
}

}

int main() {
	auto bus = S::Bus();

	/* Module under test.  */
	Boss::Mod::PeerFromScidMapper mut(bus);

	auto got_manifest = false;
	bus.subscribe<Boss::Msg::ManifestNotification
		     >([&](Boss::Msg::ManifestNotification const& m) {
		assert(m.name == "channel_state_changed");
		got_manifest = true;
		return Ev::lift();
	});

	auto response = std::unique_ptr<Boss::Msg::ResponsePeerFromScid>();
	bus.subscribe<Boss::Msg::ResponsePeerFromScid
		     >([&](Boss::Msg::ResponsePeerFromScid const& m) {
		response = Util::make_unique<
			Boss::Msg::ResponsePeerFromScid>(m);
		return Ev::lift();
	});

	auto request = [&](char const* scid) {
		response = nullptr;
		return bus.raise(Boss::Msg::RequestPeerFromScid{
			nullptr, Ln::Scid(scid)
		}).then([]() {
			return Ev::yield(42);
		});
	};
	auto notify = [&](char const* json) {
		return bus.raise(Boss::Msg::Notification{
			"channel_state_changed",
			Jsmn::Object::parse_json(json)
		}).then([]() {
			return Ev::yield(42);
		});
	};

	auto code = Ev::lift().then([&]() {
		return bus.raise(Boss::Msg::Manifestation{});
	}).then([&]() {
		return Ev::yield(42);
	}).then([&]() {
		assert(got_manifest);

		/* A notification before the first snapshot is ignored:
		 * the snapshot will list the channel.  */
		return notify(locked_in);
	}).then([&]() {
		/* Requests before the first snapshot wait for it.  */
		return request("1000x1x0");
	}).then([&]() {
		assert(!response);
		return bus.raise(snapshot(listpeers_result));
	}).then([&]() {
		return Ev::yield(42);
	}).then([&]() {
		assert(response);
		assert(response->scid == Ln::Scid("1000x1x0"));
		assert(response->peer == peer_a);

		/* After the snapshot, a known scid resolves.  */
		return request("1000x1x1");
	}).then([&]() {
		assert(response);
		assert(response->peer == peer_b);

		/* An unknown scid answers unknown at once.  */
		return request("3000x1x0");
	}).then([&]() {
		assert(response);
		assert(response->scid == Ln::Scid("3000x1x0"));
		assert(!response->peer);

		/* A state change without a short channel id teaches
		 * nothing and does no harm.  */
		return notify(not_confirmed);
	}).then([&]() {
		return request("3000x1x0");
	}).then([&]() {
		assert(response);
		assert(!response->peer);

		/* Lock-in teaches the channel without a snapshot.  */
		return notify(locked_in);
	}).then([&]() {
		return request("3000x1x0");
	}).then([&]() {
		assert(response);
		assert(response->peer == peer_c);

		/* The earlier entries are untouched.  */
		return request("1000x1x1");
	}).then([&]() {
		assert(response);
		assert(response->peer == peer_b);

		/* A repeated notification is idempotent.  */
		return notify(locked_in);
	}).then([&]() {
		return request("3000x1x0");
	}).then([&]() {
		assert(response);
		assert(response->peer == peer_c);

		/* The next snapshot replaces the table and still
		 * answers correctly.  */
		return bus.raise(snapshot(listpeers_result_later));
	}).then([&]() {
		return Ev::yield(42);
	}).then([&]() {
		return request("3000x1x0");
	}).then([&]() {
		assert(response);
		assert(response->peer == peer_c);
		return request("1000x1x0");
	}).then([&]() {
		assert(response);
		assert(response->peer == peer_a);
		/* Gone from the snapshot: unknown again.  */
		return request("1000x1x1");
	}).then([&]() {
		assert(response);
		assert(!response->peer);
		return Ev::lift(0);
	});

	return Ev::start(code);
}

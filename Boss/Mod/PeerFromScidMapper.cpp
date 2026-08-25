#include"Boss/Mod/PeerFromScidMapper.hpp"
#include"Boss/Msg/ListpeersResult.hpp"
#include"Boss/Msg/ManifestNotification.hpp"
#include"Boss/Msg/Manifestation.hpp"
#include"Boss/Msg/Notification.hpp"
#include"Boss/Msg/RequestPeerFromScid.hpp"
#include"Boss/Msg/ResponsePeerFromScid.hpp"
#include"Boss/log.hpp"
#include"Ev/Io.hpp"
#include"Jsmn/Object.hpp"
#include"Ln/NodeId.hpp"
#include"Ln/Scid.hpp"
#include"S/Bus.hpp"
#include"Util/make_unique.hpp"
#include<exception>
#include<map>
#include<queue>

namespace Boss { namespace Mod {

class PeerFromScidMapper::Impl {
private:
	S::Bus& bus;

	std::unique_ptr<std::map<Ln::Scid, Ln::NodeId>> map;

	typedef
	std::queue<Msg::RequestPeerFromScid> PendingQ;
	PendingQ pendings;

	void start() {
		bus.subscribe<Msg::ListpeersResult
			     >([this](Msg::ListpeersResult const& m) {
			auto tmp = std::map<Ln::Scid, Ln::NodeId>();
			for (auto p : m.cpeers) {
				auto node = p.first;
				auto cs = p.second.channels;
				for (auto c : cs) {
					if (!c.has("short_channel_id"))
						continue;
					auto scid_j = c["short_channel_id"];
					if (!scid_j.is_string())
						continue;
					auto scid_s = std::string(scid_j);
					if (!Ln::Scid::valid_string(scid_s))
						continue;
					auto scid = Ln::Scid(scid_s);
					tmp[scid] = node;
				}
			}
			map = Util::make_unique<std::map< Ln::Scid
							, Ln::NodeId
							>>(std::move(tmp));
			auto ppendings = std::make_shared<PendingQ>(std::move(pendings));
			return resume_pendings(std::move(ppendings));
		});

		/* A channel that locked in since the last snapshot is
		 * usable at once, and the parts and forwards through
		 * it would miss the table until the next snapshot.
		 * Learn it from the state-change notification, which
		 * carries the short channel id once the funding has
		 * confirmed.  */
		bus.subscribe<Msg::Manifestation
			     >([this](Msg::Manifestation const& _) {
			/* ChannelCreateDestroyMonitor manifests the same
			 * name; the manifester keeps one entry per name.  */
			return bus.raise(Msg::ManifestNotification{
				"channel_state_changed"
			});
		});
		bus.subscribe<Msg::Notification
			     >([this](Msg::Notification const& n) {
			if (n.notification != "channel_state_changed")
				return Ev::lift();
			/* No table yet: the first snapshot lists the
			 * channel, and the requests parked until then
			 * replay against it.  */
			if (!map)
				return Ev::lift();

			auto node = Ln::NodeId();
			auto scid = Ln::Scid();
			try {
				auto payload = n.params["channel_state_changed"];
				/* Null until the funding confirms; nothing
				 * to learn then.  */
				if ( !payload.has("short_channel_id")
				  || !payload["short_channel_id"].is_string()
				   )
					return Ev::lift();
				auto scid_s = std::string(
					payload["short_channel_id"]
				);
				if (!Ln::Scid::valid_string(scid_s))
					return Ev::lift();
				scid = Ln::Scid(scid_s);
				node = Ln::NodeId(std::string(
					payload["peer_id"]
				));
			} catch (std::exception const&) {
				/* ChannelCreateDestroyMonitor reads the same
				 * notification and reports a malformed one.  */
				return Ev::lift();
			}

			auto it = map->find(scid);
			if (it != map->end() && it->second == node)
				return Ev::lift();
			(*map)[scid] = node;
			return Boss::log( bus, Debug
					, "PeerFromScidMapper: learned %s -> %s "
					  "from channel_state_changed."
					, std::string(scid).c_str()
					, std::string(node).c_str()
					);
		});

		bus.subscribe<Msg::RequestPeerFromScid
			     >([this](Msg::RequestPeerFromScid const& m) {
			if (!map) {
				pendings.push(m);
				return Ev::lift();
			}

			auto it = map->find(m.scid);
			if (it == map->end()) {
				return bus.raise(Msg::ResponsePeerFromScid{
					m.requester, m.scid, Ln::NodeId()
				});
			}
			return bus.raise(Msg::ResponsePeerFromScid{
				m.requester, m.scid, it->second
			});
		});
	}

	Ev::Io<void>
	resume_pendings(std::shared_ptr<PendingQ> const& ppendings) {
		return Ev::lift().then([this, ppendings]() {
			if (ppendings->empty())
				return Ev::lift();
			auto pending = std::move(ppendings->front());
			ppendings->pop();
			return bus.raise(std::move(pending))
			     + resume_pendings(ppendings)
			     ;
		});
	}

public:
	Impl( S::Bus& bus_
	    ) : bus(bus_)
	      { start(); }
};

PeerFromScidMapper::PeerFromScidMapper(PeerFromScidMapper&&) =default;
PeerFromScidMapper::~PeerFromScidMapper() =default;

PeerFromScidMapper::PeerFromScidMapper(S::Bus& bus)
	: pimpl(Util::make_unique<Impl>(bus)) { }

}}

#ifndef BOSS_MOD_PEERFROMSCIDMAPPER_HPP
#define BOSS_MOD_PEERFROMSCIDMAPPER_HPP

#include<memory>

namespace S { class Bus; }

namespace Boss { namespace Mod {

/** class Boss::Mod::PeerFromScidMapper
 *
 * @brief Handles `Boss::Msg::RequestPeerFromScid` messages,
 * figuring out the peer node ID from a given SCID, and
 * broadcasts `Boss::Msg::ResponsePeerFromScid` in response.
 *
 * The table is rebuilt from each `Boss::Msg::ListpeersResult`
 * snapshot and extended between snapshots from CLN's
 * `channel_state_changed` notification, so a channel that just
 * locked in resolves before its first part or forward.
 */
class PeerFromScidMapper {
private:
	class Impl;
	std::unique_ptr<Impl> pimpl;

public:
	PeerFromScidMapper() =delete;
	PeerFromScidMapper(PeerFromScidMapper const&) =delete;

	PeerFromScidMapper(PeerFromScidMapper&&);
	~PeerFromScidMapper();

	explicit
	PeerFromScidMapper(S::Bus& bus);
};

}}

#endif /* !defined(BOSS_MOD_PEERFROMSCIDMAPPER_HPP) */
